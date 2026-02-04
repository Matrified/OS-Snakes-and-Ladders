#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#define PORT 5555

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
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect");
        return 1;
    }

    char buffer[256];
    int n = recv_line(sock, buffer, sizeof(buffer));
    if (n > 0) {
        printf("%s\n", buffer);
    }

    char name[64];
    if (fgets(name, sizeof(name), stdin) == NULL) {
        close(sock);
        return 0;
    }
    send(sock, name, strlen(name), 0);

    while (1) {
        n = recv_line(sock, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        printf("%s\n", buffer);

        if (strncmp(buffer, "YOUR_TURN", 9) == 0) {
            printf("Press ENTER to roll...\n");
            fgets(buffer, sizeof(buffer), stdin);
            send(sock, "roll\n", 5, 0);
        }
    }

    close(sock);
    return 0;
}
