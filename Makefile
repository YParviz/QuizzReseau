CC = gcc
CFLAGS = -Wall -Wextra -O2

all: server client

serveur: serveur.c
	$(CC) $(CFLAGS) server.c -o server

client: client.c
	$(CC) $(CFLAGS) client.c -o client

clean:
	rm -f server client

