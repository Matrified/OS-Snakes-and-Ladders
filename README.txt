Snakes & Ladders (OS Assignment)

How to Compile (Linux)
1) make

How to Run
1) ./server
2) Enter number of players (3 to 5).
3) In separate terminals run: ./client (one per player)

Game Rules (text-based)
- 3 to 5 players.
- Server rolls the dice (1-6).
- If a player lands on a ladder, they climb up.
- If a player lands on a snake, they slide down.
- First player to reach square 100 wins.

Deployment Mode
- Multi-machine capable using TCP sockets (IPv4). (Works locally on 127.0.0.1)

Concurrency Model (Hybrid)
- Server forks one child process per client.
- Parent runs two threads: Round Robin scheduler and Logger.
- Shared game state is in POSIX shared memory and protected by process-shared mutexes and semaphores.

Files
- Server.c / Client.c
- Makefile
- scores.txt (persistent win counts)
- game.log (event log)
