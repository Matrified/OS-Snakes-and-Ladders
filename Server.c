/*
– INDIVIDUAL CONTRIBUTIONS

Hadi:
- Overall system architecture and design
- TCP server setup and client connection handling
- POSIX shared memory design and layout
- Process-shared mutex initialization and synchronization
- Hybrid concurrency model using fork() and pthreads
- Round Robin turn scheduling logic
- Multi-game reset and coordination logic
- Final integration and debugging

Aman:
- Snakes and ladders rule implementation
- Dice rolling and movement constraints
- Win condition logic
- Client handler flow testing

Amr:
- Logger design and implementation (game.log)
- Persistent score storage (scores.txt)
- File I/O synchronization
- System testing and validation
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <semaphore.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>

#define PORT 5555
#define MAX_PLAYERS 5
#define MIN_PLAYERS 3
#define BOARD_SIZE 100
#define SHM_NAME "/snl_shm"
#define SCORE_FILE "scores.txt"
#define MAX_NAME 32
#define LOG_QUEUE_SIZE 64
#define LOG_MSG_LEN 128
#define SCORE_MAX 50

/* Persistent score entry (saved to scores.txt). */
typedef struct {
    char name[MAX_NAME];
    int wins;
} ScoreEntry;

/* Shared state between parent threads and forked children. */
typedef struct {
    int position[MAX_PLAYERS];
    int connected[MAX_PLAYERS];
    int current_turn;
    int game_started;
    int game_over;
    int winner_id;
    int round_no;
    int game_over_notice;
    int turn_count;
    int target_players;
    int active_players;
    char player_name[MAX_PLAYERS][MAX_NAME];
    int ready[MAX_PLAYERS];

    ScoreEntry scores[SCORE_MAX];
    int score_count;

    /* Log queue (non-blocking for game flow). */
    char log_queue[LOG_QUEUE_SIZE][LOG_MSG_LEN];
    int log_head;
    int log_tail;

    /* Sync primitives (process-shared). */
    pthread_mutex_t state_mutex;
    pthread_mutex_t log_mutex;
    sem_t log_items;
    sem_t log_spaces;
    sem_t turn_sem[MAX_PLAYERS];
    sem_t turn_done;
} SharedGame;

static SharedGame *game = NULL;
static volatile sig_atomic_t server_running = 1;
static int server_fd = -1;

static int snakes[4][2] = {
    {99, 54}, {70, 55}, {52, 42}, {25, 2}
};

static int ladders[4][2] = {
    {6, 25}, {11, 40}, {46, 90}, {60, 85}
};

/* Push a message into the shared log queue (drop if full). */
static void enqueue_log(const char *fmt, ...) {
    char msg[LOG_MSG_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    size_t len = strlen(msg);
    if (len > 0 && msg[len - 1] == '\n') {
        msg[len - 1] = '\0';
    }

    /* Don't block gameplay on logging. */
    if (sem_trywait(&game->log_spaces) != 0) {
        return;
    }

    pthread_mutex_lock(&game->log_mutex);
    strncpy(game->log_queue[game->log_tail], msg, LOG_MSG_LEN - 1);
    game->log_queue[game->log_tail][LOG_MSG_LEN - 1] = '\0';
    game->log_tail = (game->log_tail + 1) % LOG_QUEUE_SIZE;
    pthread_mutex_unlock(&game->log_mutex);
    sem_post(&game->log_items);
}

/* Dedicated logger thread (parent process). */
static void *logger_thread(void *arg) {
    (void)arg;
    while (server_running) {
        if (sem_wait(&game->log_items) != 0) {
            continue;
        }

        pthread_mutex_lock(&game->log_mutex);
        char msg[LOG_MSG_LEN];
        strncpy(msg, game->log_queue[game->log_head], LOG_MSG_LEN);
        game->log_head = (game->log_head + 1) % LOG_QUEUE_SIZE;
        pthread_mutex_unlock(&game->log_mutex);
        sem_post(&game->log_spaces);

        FILE *fp = fopen("game.log", "a");
        if (fp) {
            fprintf(fp, "%s\n", msg);
            fclose(fp);
        }
    }
    return NULL;
}

/* Load scores.txt into shared memory at startup. */
static void load_scores_file(void) {
    FILE *fp = fopen(SCORE_FILE, "r");
    if (!fp) {
        return;
    }

    char name[MAX_NAME];
    int wins = 0;
    while (fscanf(fp, "%31s %d", name, &wins) == 2) {
        if (game->score_count >= SCORE_MAX) {
            break;
        }
        strncpy(game->scores[game->score_count].name, name, MAX_NAME - 1);
        game->scores[game->score_count].name[MAX_NAME - 1] = '\0';
        game->scores[game->score_count].wins = wins;
        game->score_count++;
    }
    fclose(fp);
}

/* Persist scores from memory to disk. */
static void save_scores_file(void) {
    FILE *fp = fopen(SCORE_FILE, "w");
    if (!fp) {
        return;
    }
    for (int i = 0; i < game->score_count; i++) {
        fprintf(fp, "%s %d\n", game->scores[i].name, game->scores[i].wins);
    }
    fclose(fp);
}

/* Update winner score (caller must hold state_mutex). */
static void update_score_locked(const char *name) {
    for (int i = 0; i < game->score_count; i++) {
        if (strncmp(game->scores[i].name, name, MAX_NAME) == 0) {
            game->scores[i].wins++;
            return;
        }
    }
    if (game->score_count < SCORE_MAX) {
        strncpy(game->scores[game->score_count].name, name, MAX_NAME - 1);
        game->scores[game->score_count].name[MAX_NAME - 1] = '\0';
        game->scores[game->score_count].wins = 1;
        game->score_count++;
    }
}

/* Apply snake or ladder rule if the player lands exactly on a start cell. */
static int apply_snakes_ladders(int pos) {
    for (int i = 0; i < 4; i++) {
        if (snakes[i][0] == pos) {
            return snakes[i][1];
        }
    }
    for (int i = 0; i < 4; i++) {
        if (ladders[i][0] == pos) {
            return ladders[i][1];
        }
    }
    return pos;
}

/* Best-effort send (small messages). */
static void send_line(int sock, const char *msg);

/* Reset game state for a new round (caller must hold state_mutex). */
static void reset_game_locked(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game->position[i] = 0;
        game->ready[i] = 0;
    }
    game->current_turn = 0;
    game->game_over = 0;
    game->winner_id = -1;
    game->game_over_notice = 0;
    game->turn_count = 0;
    game->game_started = 1;
    game->round_no++;
}

/* Build a compact "Positions" line (caller must hold state_mutex). */
static void build_positions_locked(char *out, size_t len) {
    size_t used = 0;
    out[0] = '\0';

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!game->connected[i]) {
            continue;
        }
        int written = 0;
        if (game->player_name[i][0]) {
            written = snprintf(out + used, len - used, "%s:%d ",
                               game->player_name[i], game->position[i]);
        } else {
            written = snprintf(out + used, len - used, "Player%d:%d ",
                               i + 1, game->position[i]);
        }
        if (written < 0 || (size_t)written >= len - used) {
            break;
        }
        used += (size_t)written;
    }
}

/* Simple serpentine board (numbers only). */
static void build_board_numbers(char *out, size_t len) {
    size_t used = 0;
    out[0] = '\0';

    for (int row = 9; row >= 0; row--) {
        char line[256];
        size_t off = 0;
        int start = row * 10 + 1;

        for (int col = 0; col < 10; col++) {
            int num = (row % 2 == 0) ? (start + col) : (start + (9 - col));
            off += snprintf(line + off, sizeof(line) - off, "[%3d]", num);
        }

        off += snprintf(line + off, sizeof(line) - off, "\n");
        if (used + off >= len) {
            break;
        }
        memcpy(out + used, line, off);
        used += off;
        out[used] = '\0';
    }
}

/* Show snakes and ladders list below the board. */
static void send_snakes_ladders(int sock) {
    char line[128];
    send_line(sock, "Snakes: ");
    for (int i = 0; i < 4; i++) {
        snprintf(line, sizeof(line), "%d->%d ", snakes[i][0], snakes[i][1]);
        send_line(sock, line);
    }
    send_line(sock, "\nLadders: ");
    for (int i = 0; i < 4; i++) {
        snprintf(line, sizeof(line), "%d->%d ", ladders[i][0], ladders[i][1]);
        send_line(sock, line);
    }
    send_line(sock, "\n");
}

/* Find next connected player in round-robin order. */
static int find_next_active_locked(int after) {
    for (int i = 1; i <= MAX_PLAYERS; i++) {
        int idx = (after + i) % MAX_PLAYERS;
        if (game->connected[idx]) {
            return idx;
        }
    }
    return -1;
}

/* Round-robin scheduler thread (parent process). */
static void *scheduler_thread(void *arg) {
    (void)arg;
    int last_turn = -1;
    int last_round = 0;

    while (server_running) {
        pthread_mutex_lock(&game->state_mutex);

        /* If game ended, wake clients so they can print winner/scoreboard. */
        if (game->game_over) {
            if (game->game_over_notice != game->round_no) {
                game->game_over_notice = game->round_no;
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (game->connected[i]) {
                        sem_post(&game->turn_sem[i]);
                    }
                }
            }
            pthread_mutex_unlock(&game->state_mutex);
            sleep(1);
            continue;
        }

        /* Wait until enough players are connected. */
        if (!game->game_started || game->active_players < MIN_PLAYERS) {
            pthread_mutex_unlock(&game->state_mutex);
            sleep(1);
            continue;
        }
        int all_ready = 1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (game->connected[i] && !game->ready[i]) {
                all_ready = 0;
                break;
            }
        }
        if (!all_ready) {
            pthread_mutex_unlock(&game->state_mutex);
            sleep(1);
            continue;
        }

        if (game->round_no != last_round) {
            last_round = game->round_no;
            last_turn = -1;
        }

        int next = find_next_active_locked(last_turn);
        if (next < 0) {
            pthread_mutex_unlock(&game->state_mutex);
            sleep(1);
            continue;
        }

        /* Announce next turn in shared state. */
        game->current_turn = next;
        enqueue_log("Turn -> Player %d (%s)", next + 1,
                    game->player_name[next][0] ? game->player_name[next] : "Player");
        pthread_mutex_unlock(&game->state_mutex);

        sem_post(&game->turn_sem[next]);

        sem_wait(&game->turn_done);
        last_turn = next;

        pthread_mutex_lock(&game->state_mutex);
        if (game->game_over && game->active_players >= MIN_PLAYERS) {
            pthread_mutex_unlock(&game->state_mutex);
            sleep(2);
            pthread_mutex_lock(&game->state_mutex);
            if (game->game_over && game->active_players >= MIN_PLAYERS) {
                reset_game_locked();
                enqueue_log("New game started (round %d)", game->round_no);
            }
        }
        pthread_mutex_unlock(&game->state_mutex);
    }
    return NULL;
}

/* Read a line from socket (newline-terminated). */
static int recv_line(int sock, char *buf, size_t max_len) {
    size_t idx = 0;
    while (idx < max_len - 1) {
        char c;
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) {
            return n;
        }
        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            buf[idx++] = c;
        }
    }
    buf[idx] = '\0';
    return (int)idx;
}

/* Best-effort send (small messages). */
static void send_line(int sock, const char *msg) {
    ssize_t n = send(sock, msg, strlen(msg), 0);
    (void)n; /* ignore send errors; disconnect is handled on recv */
}

/* Print scoreboard to a client. */
static void send_scoreboard_lines(int sock, ScoreEntry *scores, int count) {
    char line[128];
    send_line(sock, "Scoreboard:\n");
    if (count <= 0) {
        send_line(sock, "  (no scores yet)\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        snprintf(line, sizeof(line), "  %d) %s - %d wins\n",
                 i + 1, scores[i].name, scores[i].wins);
        send_line(sock, line);
    }
}

/* Child process: handle one player session. */
static void handle_client(int sock, int id) {
    char buffer[256];
    srand((unsigned int)(time(NULL) ^ (getpid() << 16)));

    send_line(sock, "Enter your name (no spaces):\n");
    int n = recv_line(sock, buffer, sizeof(buffer));
    if (n <= 0) {
        close(sock);
        return;
    }
    /* Replace spaces to keep name tokens simple in scores.txt. */
    for (int i = 0; buffer[i] != '\0'; i++) {
        if (isspace((unsigned char)buffer[i])) {
            buffer[i] = '_';
        }
    }
    if (buffer[0] == '\0') {
        snprintf(buffer, sizeof(buffer), "Player%d", id + 1);
    }

    pthread_mutex_lock(&game->state_mutex);
    strncpy(game->player_name[id], buffer, MAX_NAME - 1);
    game->player_name[id][MAX_NAME - 1] = '\0';
    game->ready[id] = 1;
    pthread_mutex_unlock(&game->state_mutex);

    snprintf(buffer, sizeof(buffer), "Welcome %s! Waiting for the game to start...\n", game->player_name[id]);
    send_line(sock, buffer);
    send_line(sock, "Rules: first to reach 100 wins (exact roll needed). Snakes down, ladders up.\n");
    pthread_mutex_lock(&game->state_mutex);
    int connected_now = game->active_players;
    int target_total = game->target_players;
    pthread_mutex_unlock(&game->state_mutex);
    snprintf(buffer, sizeof(buffer), "Players connected: %d/%d\n", connected_now, target_total);
    send_line(sock, buffer);
    send_line(sock, "Waiting for other players to join...\n");
    enqueue_log("Player %d (%s) connected", id + 1, game->player_name[id]);

    int game_started_notice = 0;
    int game_over_notice = 0;
    int my_turns = 0; /* used for periodic board display */
    while (server_running) {
        send_line(sock, "Waiting for your turn...\n");
        if (sem_wait(&game->turn_sem[id]) != 0) {
            continue;
        }

        pthread_mutex_lock(&game->state_mutex);
        if (!game->connected[id]) {
            pthread_mutex_unlock(&game->state_mutex);
            break;
        }
        if (!game->game_over) {
            game_over_notice = 0;
        }
        if (!game->ready[id]) {
            pthread_mutex_unlock(&game->state_mutex);
            continue;
        }

        if (game->game_over) {
            int winner = game->winner_id;
            char winner_name[MAX_NAME];
            winner_name[0] = '\0';
            if (winner >= 0 && winner < MAX_PLAYERS) {
                strncpy(winner_name, game->player_name[winner], MAX_NAME - 1);
                winner_name[MAX_NAME - 1] = '\0';
            }

            /* Copy scoreboard locally so we can unlock before sending. */
            ScoreEntry scores_local[SCORE_MAX];
            int score_count_local = game->score_count;
            if (score_count_local > SCORE_MAX) {
                score_count_local = SCORE_MAX;
            }
            for (int i = 0; i < score_count_local; i++) {
                scores_local[i] = game->scores[i];
            }
            pthread_mutex_unlock(&game->state_mutex);

            if (!game_over_notice) {
                send_line(sock, "\n==============================\n");
                if (winner_name[0]) {
                    snprintf(buffer, sizeof(buffer), "WINNER: %s\n", winner_name);
                    send_line(sock, buffer);
                } else {
                    send_line(sock, "GAME OVER\n");
                }
                send_line(sock, "==============================\n");
                send_scoreboard_lines(sock, scores_local, score_count_local);
                game_over_notice = 1;
                game_started_notice = 0;
            }
            continue;
        }

        if (!game->game_started) {
            pthread_mutex_unlock(&game->state_mutex);
            continue;
        }

        /* If you were woken by the scheduler, it's your turn. */
        pthread_mutex_unlock(&game->state_mutex);

        if (!game_started_notice) {
            send_line(sock, "Game started! Your turn will be announced.\n");
            game_started_notice = 1;
        }

        /* Show board on first turn and every 3rd turn for this player. */
        if (my_turns == 0 || ((my_turns + 1) % 3 == 0)) {
            char board[2048];
            build_board_numbers(board, sizeof(board));
            send_line(sock, "\n----- Board (numbers) -----\n");
            send_line(sock, board);
            send_snakes_ladders(sock);
            send_line(sock, "---------------------------\n");
        }

        send_line(sock, "YOUR_TURN: press ENTER to roll the dice.\n");
        n = recv_line(sock, buffer, sizeof(buffer));
        if (n <= 0) {
            pthread_mutex_lock(&game->state_mutex);
            game->connected[id] = 0;
            game->active_players--;
            pthread_mutex_unlock(&game->state_mutex);
            enqueue_log("Player %d (%s) disconnected (recv=%d errno=%d)",
                        id + 1, game->player_name[id], n, errno);
            sem_post(&game->turn_done);
            break;
        }

        pthread_mutex_lock(&game->state_mutex);
        /* Server-side dice roll (clients never roll). */
        int dice = (rand() % 6) + 1;
        int before = game->position[id];
        int moved = 0;
        int after = before;
        int hit_snake = 0;
        int hit_ladder = 0;
        int jump_from = 0;
        int jump_to = 0;
        /* track turns for optional future features */

        if (before + dice <= BOARD_SIZE) {
            moved = 1;
            after = before + dice;
            int adjusted = apply_snakes_ladders(after);
            if (adjusted != after) {
                if (adjusted < after) {
                    hit_snake = 1;
                } else {
                    hit_ladder = 1;
                }
                jump_from = after;
                jump_to = adjusted;
                after = adjusted;
            }
            game->position[id] = after;
        }

        game->turn_count++;

        snprintf(buffer, sizeof(buffer), "Player %s rolled %d -> position %d\n",
                 game->player_name[id], dice, game->position[id]);
        pthread_mutex_unlock(&game->state_mutex);

        send_line(sock, buffer);
        enqueue_log("%s", buffer);

        if (!moved) {
            send_line(sock, "Exact roll needed to reach 100. You stay in place.\n");
            enqueue_log("Player %s needed exact roll (stayed at %d)", game->player_name[id], before);
        }
        if (hit_snake) {
            snprintf(buffer, sizeof(buffer), "Snake! %d -> %d\n", jump_from, jump_to);
            send_line(sock, buffer);
            enqueue_log("Player %s hit a snake (%d -> %d)", game->player_name[id], jump_from, jump_to);
        } else if (hit_ladder) {
            snprintf(buffer, sizeof(buffer), "Ladder! %d -> %d\n", jump_from, jump_to);
            send_line(sock, buffer);
            enqueue_log("Player %s climbed a ladder (%d -> %d)", game->player_name[id], jump_from, jump_to);
        }

        /* Build positions line safely. */
        pthread_mutex_lock(&game->state_mutex);
        char pos_line[256];
        build_positions_locked(pos_line, sizeof(pos_line));
        pthread_mutex_unlock(&game->state_mutex);
        if (pos_line[0]) {
            send_line(sock, "Positions: ");
            send_line(sock, pos_line);
            send_line(sock, "\n");
        }

        pthread_mutex_lock(&game->state_mutex);
        /* Win check (first to reach 100). */
        if (game->position[id] == BOARD_SIZE && !game->game_over) {
            game->game_over = 1;
            game->winner_id = id;
            update_score_locked(game->player_name[id]);
            save_scores_file();
            snprintf(buffer, sizeof(buffer), "Player %s WON the game\n", game->player_name[id]);
            enqueue_log("Player %s WON the game", game->player_name[id]);
        }
        pthread_mutex_unlock(&game->state_mutex);

        if (buffer[0] != '\0' && strstr(buffer, "WON") != NULL) {
            send_line(sock, buffer);
        }

        my_turns++;
        sem_post(&game->turn_done);
    }

    close(sock);
}

/* Reap child processes to avoid zombies. */
static void reap(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

/* Graceful shutdown on Ctrl+C. */
static void handle_sigint(int sig) {
    (void)sig;
    server_running = 0;
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
    if (game) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            sem_post(&game->turn_sem[i]);
        }
        sem_post(&game->turn_done);
        sem_post(&game->log_items);
    }
}

int main(void) {
    signal(SIGCHLD, reap);
    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    int target_players = 0;
    printf("Enter number of players (%d-%d): ", MIN_PLAYERS, MAX_PLAYERS);
    fflush(stdout);
    if (scanf("%d", &target_players) != 1) {
        printf("Invalid input.\n");
        return 1;
    }
    if (target_players < MIN_PLAYERS || target_players > MAX_PLAYERS) {
        printf("Players must be between %d and %d.\n", MIN_PLAYERS, MAX_PLAYERS);
        return 1;
    }
    printf("Waiting for %d players to connect...\n", target_players);

    /* Shared memory setup (POSIX shm). */
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        return 1;
    }
    if (ftruncate(shm_fd, sizeof(SharedGame)) != 0) {
        perror("ftruncate");
        return 1;
    }

    game = mmap(NULL, sizeof(SharedGame), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (game == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    memset(game, 0, sizeof(SharedGame));
    game->target_players = target_players;
    game->active_players = 0;
    game->winner_id = -1;
    game->round_no = 0;

    /* Process-shared mutexes for shared memory. */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&game->state_mutex, &attr);
    pthread_mutex_init(&game->log_mutex, &attr);

    /* Process-shared semaphores (pshared = 1). */
    sem_init(&game->log_items, 1, 0);
    sem_init(&game->log_spaces, 1, LOG_QUEUE_SIZE);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        sem_init(&game->turn_sem[i], 1, 0);
    }
    sem_init(&game->turn_done, 1, 0);

    FILE *score_fp = fopen(SCORE_FILE, "a");
    if (score_fp) {
        fclose(score_fp);
    }
    load_scores_file();

    /* Parent threads: scheduler + logger. */
    pthread_t sched_thread;
    pthread_t log_thread;
    pthread_create(&sched_thread, NULL, scheduler_thread, NULL);
    pthread_create(&log_thread, NULL, logger_thread, NULL);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(server_fd, MAX_PLAYERS) != 0) {
        perror("listen");
        return 1;
    }

    printf("Snakes & Ladders Server running on port %d\n", PORT);
    enqueue_log("Server started on port %d", PORT);
    enqueue_log("Target players: %d", target_players);

    /* Accept exactly N players, then start the game. */
    for (int i = 0; i < target_players; i++) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            i--;
            continue;
        }

        pthread_mutex_lock(&game->state_mutex);
        game->connected[i] = 1;
        game->active_players++;
        game->ready[i] = 0;
        int active_now = game->active_players;
        pthread_mutex_unlock(&game->state_mutex);
        printf("Player %d connected (%d/%d)\n", i + 1, active_now, target_players);
        enqueue_log("Player %d connected (%d/%d)", i + 1, active_now, target_players);

        /* One child process per client (required by assignment). */
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            pthread_mutex_lock(&game->state_mutex);
            game->connected[i] = 0;
            game->active_players--;
            pthread_mutex_unlock(&game->state_mutex);
            close(client_fd);
            i--;
            continue;
        }
        if (pid == 0) {
            close(server_fd);
            handle_client(client_fd, i);
            exit(0);
        }

        close(client_fd);
    }

    /* Wait until all players have entered their names. */
    int name_ready = 0;
    while (!name_ready) {
        pthread_mutex_lock(&game->state_mutex);
        name_ready = 1;
        for (int i = 0; i < target_players; i++) {
            if (!game->connected[i] || !game->ready[i]) {
                name_ready = 0;
                break;
            }
        }
        pthread_mutex_unlock(&game->state_mutex);
        if (!name_ready) {
            sleep(1);
        }
    }

    pthread_mutex_lock(&game->state_mutex);
    /* Start first round. */
    reset_game_locked();
    enqueue_log("New game started (round %d)", game->round_no);
    pthread_mutex_unlock(&game->state_mutex);

    while (server_running) {
        pause();
    }

    pthread_mutex_lock(&game->state_mutex);
    save_scores_file();
    pthread_mutex_unlock(&game->state_mutex);

    munmap(game, sizeof(SharedGame));
    shm_unlink(SHM_NAME);

    return 0;
}
