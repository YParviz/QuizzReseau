/* client.c
   Client interactif pour le Quiz réseau
   Usage: ./client <serveur-ip> [port]
   Compile: gcc client.c -o client
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#define DEFAULT_PORT 5000
#define BUFSIZE 512

/* read a line from socket (terminated by '\n') */
ssize_t recv_line(int sock, char *buf, size_t bufsz) {
    size_t idx = 0;
    while (idx + 1 < bufsz) {
        char c;
        ssize_t r = recv(sock, &c, 1, 0);
        if (r <= 0) return (r == 0 && idx>0) ? idx : -1;
        buf[idx++] = c;
        if (c == '\n') break;
    }
    buf[idx] = '\0';
    return idx;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server-ip> [port]\n", argv[0]);
        return 1;
    }
    const char *server = argv[1];
    int port = (argc >= 3) ? atoi(argv[2]) : DEFAULT_PORT;

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) { perror("socket"); return 1; }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    if (inet_pton(AF_INET, server, &servaddr.sin_addr) <= 0) {
        // try DNS
        struct addrinfo hints, *res;
        memset(&hints,0,sizeof(hints));
        hints.ai_family = AF_INET;
        if (getaddrinfo(server, NULL, &hints, &res) != 0) {
            perror("inet_pton/getaddrinfo");
            close(sd);
            return 1;
        }
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        servaddr.sin_addr = sa->sin_addr;
        freeaddrinfo(res);
    }

    if (connect(sd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect"); close(sd); return 1;
    }

    printf("Connecté au serveur %s:%d\n", server, port);

    char buf[BUFSIZE];
    // initial server greeting(s)
    while (1) {
        ssize_t r = recv_line(sd, buf, sizeof(buf));
        if (r <= 0) { break; }
        printf("%s", buf);
        // if the line is a QUESTION, break to allow user input
        if (strncmp(buf, "QUESTION:", 9) == 0) break;
        // if server sent BYE/DONE, then exit
        if (strncmp(buf, "BYE:", 4) == 0 || strncmp(buf, "DONE:", 5) == 0 || strncmp(buf, "INCORRECT:",10)==0) {
            close(sd); return 0;
        }
    }

    while (1) {
        // buf currently contains the QUESTION line (or we proceed)
        if (strncmp(buf, "QUESTION:", 9) == 0) {
            // print question already printed above
        } else {
            ssize_t r = recv_line(sd, buf, sizeof(buf));
            if (r <= 0) break;
            printf("%s", buf);
            if (strncmp(buf, "QUESTION:", 9) != 0) {
                // not a question -> maybe final message
                if (strncmp(buf,"INCORRECT:",10)==0 || strncmp(buf,"BYE:",4)==0 || strncmp(buf,"DONE:",5)==0) break;
                continue;
            }
        }

        // read user input
        char answer[BUFSIZE];
        printf("Votre réponse (q pour quitter) > ");
        if (!fgets(answer, sizeof(answer), stdin)) break;
        // ensure newline
        if (answer[strlen(answer)-1] != '\n') {
            strncat(answer, "\n", sizeof(answer)-strlen(answer)-1);
        }

        // send answer
        if (send(sd, answer, strlen(answer), 0) <= 0) { perror("send"); break; }

        // read server response (could be CORRECT or INCORRECT or SCORE)
        ssize_t r = recv_line(sd, buf, sizeof(buf));
        if (r <= 0) break;
        printf("%s", buf);
        // if INCORRECT or BYE or DONE -> session ended
        if (strncmp(buf, "INCORRECT:",10)==0 || strncmp(buf,"BYE:",4)==0 || strncmp(buf,"DONE:",5)==0) break;

        // else next iteration will get the next QUESTION which will be read at start of loop
    }

    close(sd);
    printf("Session terminée.\n");
    return 0;
}
