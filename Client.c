/*
COMMENT – INDIVIDUAL CONTRIBUTIONS

Hadi:
- Client-side TCP connection logic
- Message receiving and display logic
- Client testing and validation
*/


#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#define PORT 5555

/* Read a line from the socket (newline-terminated). */
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

int main(void) {
    /* Create TCP socket. */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    /* Connect to local server. */
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect");
        return 1;
    }

    /* Receive prompt (name request). */
    char buffer[256];
    int n = recv_line(sock, buffer, sizeof(buffer));
    if (n > 0) {
        printf("%s\n", buffer);
    }

    /* Send player name. */
    char name[64];
    if (fgets(name, sizeof(name), stdin) == NULL) {
        close(sock);
        return 0;
    }
    send(sock, name, strlen(name), 0);

    /* Main receive loop. */
    while (1) {
        n = recv_line(sock, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        printf("%s\n", buffer);

        /* Only respond on your turn. */
        if (strcmp(buffer, "YOUR_TURN") == 0) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {
            }
            send(sock, "roll\n", 5, 0);
        }
    }

    /* Cleanup. */
    close(sock);
    return 0;
}
