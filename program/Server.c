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

typedef struct {
    char name[MAX_NAME];
    int wins;
} ScoreEntry;

typedef struct {
    int position[MAX_PLAYERS];
    int connected[MAX_PLAYERS];
    int current_turn;
    int game_started;
    int game_over;
    int winner_id;
    int round_no;
    int target_players;
    int active_players;
    char player_name[MAX_PLAYERS][MAX_NAME];

    ScoreEntry scores[SCORE_MAX];
    int score_count;

    char log_queue[LOG_QUEUE_SIZE][LOG_MSG_LEN];
    int log_head;
    int log_tail;

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

static void reset_game_locked(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game->position[i] = 0;
    }
    game->current_turn = 0;
    game->game_over = 0;
    game->winner_id = -1;
    game->game_started = 1;
    game->round_no++;
}

static int find_next_active_locked(int after) {
    for (int i = 1; i <= MAX_PLAYERS; i++) {
        int idx = (after + i) % MAX_PLAYERS;
        if (game->connected[idx]) {
            return idx;
        }
    }
    return -1;
}

static void *scheduler_thread(void *arg) {
    (void)arg;
    int last_turn = -1;
    int last_round = 0;

    while (server_running) {
        pthread_mutex_lock(&game->state_mutex);

        if (!game->game_started || game->game_over || game->active_players < MIN_PLAYERS) {
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

        game->current_turn = next;
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

static void send_line(int sock, const char *msg) {
    send(sock, msg, strlen(msg), 0);
}

static void handle_client(int sock, int id) {
    char buffer[256];
    srand((unsigned int)(time(NULL) ^ (getpid() << 16)));

    send_line(sock, "Enter your name:\n");
    int n = recv_line(sock, buffer, sizeof(buffer));
    if (n <= 0) {
        close(sock);
        return;
    }
    if (buffer[0] == '\0') {
        snprintf(buffer, sizeof(buffer), "Player%d", id + 1);
    }

    pthread_mutex_lock(&game->state_mutex);
    strncpy(game->player_name[id], buffer, MAX_NAME - 1);
    game->player_name[id][MAX_NAME - 1] = '\0';
    pthread_mutex_unlock(&game->state_mutex);

    snprintf(buffer, sizeof(buffer), "Welcome %s! Waiting for the game to start...\n", game->player_name[id]);
    send_line(sock, buffer);
    enqueue_log("Player %d (%s) connected", id + 1, game->player_name[id]);

    while (server_running) {
        if (sem_wait(&game->turn_sem[id]) != 0) {
            continue;
        }

        pthread_mutex_lock(&game->state_mutex);
        if (!game->connected[id]) {
            pthread_mutex_unlock(&game->state_mutex);
            break;
        }
        if (!game->game_started || game->game_over) {
            pthread_mutex_unlock(&game->state_mutex);
            sem_post(&game->turn_done);
            continue;
        }
        pthread_mutex_unlock(&game->state_mutex);

        send_line(sock, "YOUR_TURN: press ENTER to roll the dice.\n");
        n = recv_line(sock, buffer, sizeof(buffer));
        if (n <= 0) {
            pthread_mutex_lock(&game->state_mutex);
            game->connected[id] = 0;
            game->active_players--;
            pthread_mutex_unlock(&game->state_mutex);
            enqueue_log("Player %d (%s) disconnected", id + 1, game->player_name[id]);
            sem_post(&game->turn_done);
            break;
        }

        pthread_mutex_lock(&game->state_mutex);
        int dice = (rand() % 6) + 1;
        int new_pos = game->position[id] + dice;
        if (new_pos <= BOARD_SIZE) {
            game->position[id] = new_pos;
        }
        game->position[id] = apply_snakes_ladders(game->position[id]);

        snprintf(buffer, sizeof(buffer), "Player %s rolled %d -> position %d\n",
                 game->player_name[id], dice, game->position[id]);
        pthread_mutex_unlock(&game->state_mutex);

        send_line(sock, buffer);
        enqueue_log("%s", buffer);

        pthread_mutex_lock(&game->state_mutex);
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

        sem_post(&game->turn_done);
    }

    close(sock);
}

static void reap(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static void handle_sigint(int sig) {
    (void)sig;
    server_running = 0;
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

int main(void) {
    signal(SIGCHLD, reap);
    signal(SIGINT, handle_sigint);

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

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&game->state_mutex, &attr);
    pthread_mutex_init(&game->log_mutex, &attr);

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
        pthread_mutex_unlock(&game->state_mutex);

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

    pthread_mutex_lock(&game->state_mutex);
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
