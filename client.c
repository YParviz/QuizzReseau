/* client.c - Client compatible avec le mode Arcade (INPUT:) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

#define DEFAULT_PORT 5000
#define BUFSIZE 1024

/* Lecture ligne par ligne depuis le socket */
ssize_t recv_line(int sock, char *buf, size_t bufsz) {
    size_t idx = 0;
    while (idx + 1 < bufsz) {
        char c;
        ssize_t r = recv(sock, &c, 1, 0);
        if (r <= 0) return (r == 0 && idx > 0) ? (ssize_t)idx : -1;
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
    
    // Résolution d'adresse (IP ou DNS)
    if (inet_pton(AF_INET, server, &servaddr.sin_addr) <= 0) {
        struct addrinfo hints, *res;
        memset(&hints,0,sizeof(hints));
        hints.ai_family = AF_INET;
        if (getaddrinfo(server, NULL, &hints, &res) != 0) {
            perror("getaddrinfo"); close(sd); return 1;
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
    
    // --- BOUCLE PRINCIPALE ---
    while (1) {
        // 1. On attend un message du serveur
        ssize_t r = recv_line(sd, buf, sizeof(buf));
        if (r <= 0) break; // Serveur déconnecté

        // 2. Si le message commence par "INPUT:", c'est à nous de jouer
        if (strncmp(buf, "INPUT:", 6) == 0) {
            // On affiche le message (en sautant le préfixe "INPUT: ")
            printf("%s", buf + 7); 
            
            // Saisie utilisateur
            char answer[BUFSIZE];
            if (!fgets(answer, sizeof(answer), stdin)) break;
            
            // Envoi de la réponse au serveur
            if (send(sd, answer, strlen(answer), 0) <= 0) break;
        } 
        // 3. Si c'est un message de fin
        else if (strncmp(buf, "BYE:", 4) == 0) {
            printf("%s", buf);
            break;
        }
        // 4. Sinon, c'est juste un message d'info (Score, etc.)
        else {
            printf("%s", buf);
        }
    }

    close(sd);
    printf("Session terminée.\n");
    return 0;
}
