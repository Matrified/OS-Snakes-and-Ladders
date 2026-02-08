CC=gcc
CFLAGS=-Wall -Wextra -std=c11 -pthread
LDFLAGS=-lrt

all: server client

server: Server.c
	$(CC) $(CFLAGS) -o server Server.c $(LDFLAGS)

client: Client.c
	$(CC) $(CFLAGS) -o client Client.c $(LDFLAGS)

clean:
	rm -f server client game.log scores.txt
