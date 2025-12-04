/* serveur.c
   Quiz réseau multi-clients (fork)
   Compile: gcc serveur.c -o serveur
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h> 

#define PORT 5000
#define MAXQ 2000
#define QMAXLEN 512
#define AMAXLEN 256
#define BACKLOG 10
#define BUFSIZE 512

typedef struct {
    char question[QMAXLEN];
    char answer[AMAXLEN];
} QA;

QA questions[MAXQ];
int qcount = 0;

/* trim leading/trailing spaces and convert to lowercase for comparison, and strip accents */
void normalize(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    char *end = s + strlen(start);
    while (end > start && isspace((unsigned char)*(end-1))) end--;
    *end = '\0';

    for (char *p = start; *p; ++p) {
        if (*p == '\r' || *p == '\n') { *p = '\0'; break; }

        unsigned char c = (unsigned char)*p;

        // Permet de ne pas prendre en compte les accents dans la réponse de l'utilisateur
        // Utilisation des codes hexadécimaux Latin-1 pour éviter les warnings multi-char
        switch (c) {
            case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: *p = 'a'; break; 
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: *p = 'a'; break;
            
            case 0xC8: case 0xC9: case 0xCA: case 0xCB: *p = 'e'; break; 
            case 0xE8: case 0xE9: case 0xEA: case 0xEB: *p = 'e'; break;
            
            case 0xCC: case 0xCD: case 0xCE: case 0xCF: *p = 'i'; break;
            case 0xEC: case 0xED: case 0xEE: case 0xEF: *p = 'i'; break;
            
            case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: *p = 'o'; break;
            case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: *p = 'o'; break;
            
            case 0xD9: case 0xDA: case 0xDB: case 0xDC: *p = 'u'; break; 
            case 0xF9: case 0xFA: case 0xFB: case 0xFC: *p = 'u'; break; 
            
            case 0xC7: case 0xE7: *p = 'c'; break; 

            case 0xDD: case 0xFF: *p = 'y'; break; 

            default: *p = tolower(c); break; 
        }
    }
    
    // move to beginning
    if (start != s) memmove(s, start, strlen(start)+1);
}

/* safe send - sends all bytes */
ssize_t send_all(int sock, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sock, buf + total, len - total, 0);
        if (n <= 0) return n;
        total += n;
    }
    return total;
}

/* read a line terminated by '\n' or until BUFSIZE-1; returns length or -1 */
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

/* load questions file into questions[]; format: question|answer per line */
int load_questions(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;
    char line[QMAXLEN + AMAXLEN + 16];
    qcount = 0;
    while (fgets(line, sizeof(line), f) && qcount < MAXQ) {
        char *sep = strchr(line, '|');
        if (!sep) continue; // skip malformed
        *sep = '\0';
        char *q = line;
        char *a = sep + 1;
        // strip newline from answer
        char *nl = strchr(a, '\n');
        if (nl) *nl = '\0';
        strncpy(questions[qcount].question, q, QMAXLEN-1);
        questions[qcount].question[QMAXLEN-1] = '\0';
        strncpy(questions[qcount].answer, a, AMAXLEN-1);
        questions[qcount].answer[AMAXLEN-1] = '\0';
        qcount++;
    }
    fclose(f);
    return qcount;
}

/* pick a random unused question index from avail[] of size navail */
int pick_random(int avail[], int navail) {
    if (navail <= 0) return -1;
    int r = rand() % navail;
    return avail[r];
}

/* child process handles a single client quiz session */
void handle_client(int csock) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(tv.tv_sec * 1000000 + tv.tv_usec + getpid());

    char buf[BUFSIZE];
    int score = 0;

    // prepare list of available indices
    int *avail = malloc(qcount * sizeof(int));
    if (!avail) { close(csock); return; }
    int n_avail = qcount;
    for (int i = 0; i < qcount; ++i) avail[i] = i;

    // greet
    snprintf(buf, sizeof(buf), "WELCOME: Bienvenue au Quiz ! Tapez 'q' pour quitter.\n");
    send_all(csock, buf, strlen(buf));

    while (n_avail > 0) {
        int idx = pick_random(avail, n_avail);
        if (idx < 0) break;

        // send question
        size_t max_q_len = sizeof(buf) - (strlen("QUESTION: \n") + 1);
        snprintf(buf, sizeof(buf), "QUESTION: %.*s\n", (int)max_q_len, questions[idx].question);
        if (send_all(csock, buf, strlen(buf)) <= 0) break;

        // receive answer (line)
        ssize_t r = recv_line(csock, buf, sizeof(buf));
        if (r <= 0) break;
        // remove trailing newline
        if (buf[r-1] == '\n') buf[r-1] = '\0';

        // check for quit
        if (strlen(buf) == 1 && (buf[0] == 'q' || buf[0] == 'Q')) {
            snprintf(buf, sizeof(buf), "BYE: Vous avez quitté. SCORE: %d\n", score);
            send_all(csock, buf, strlen(buf));
            break;
        }

        // normalize both strings
        char client_ans[AMAXLEN];
        char correct_ans[AMAXLEN];
        strncpy(client_ans, buf, AMAXLEN-1); client_ans[AMAXLEN-1]=0;
        strncpy(correct_ans, questions[idx].answer, AMAXLEN-1); correct_ans[AMAXLEN-1]=0;
        normalize(client_ans);
        normalize(correct_ans);

        if (strlen(client_ans) > 0 && strcmp(client_ans, correct_ans) == 0) {
            score++;
            snprintf(buf, sizeof(buf), "CORRECT: +1 point. SCORE: %d\n", score);
            send_all(csock, buf, strlen(buf));
            // remove idx from avail
            int pos = -1;
            for (int i = 0; i < n_avail; ++i) if (avail[i] == idx) { pos = i; break; }
            if (pos != -1) {
                avail[pos] = avail[n_avail-1];
                n_avail--;
            }
            // continue asking next
        } else {
            // incorrect -> final score displayed, session ends
            snprintf(buf, sizeof(buf), "INCORRECT: Bonne réponse: %s. SCORE FINAL: %d\n",
                     questions[idx].answer, score);
            send_all(csock, buf, strlen(buf));
            break;
        }
    }

    if (n_avail == 0) {
        snprintf(buf, sizeof(buf), "DONE: Toutes les questions ont été posées. SCORE FINAL: %d\n", score);
        send_all(csock, buf, strlen(buf));
    }

    free(avail);
    close(csock);
    exit(0); // terminate child
}

int main(int argc, char **argv) {
    const char *qfile = "questions.txt";
    if (argc >= 2) qfile = argv[1];

    if (load_questions(qfile) <= 0) {
        fprintf(stderr, "Erreur: impossible de charger les questions depuis %s\n", qfile);
        return 1;
    }
    if (qcount < 1) {
        fprintf(stderr, "Aucune question chargee.\n");
        return 1;
    }

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    if (bind(sd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind"); close(sd); return 1;
    }

    if (listen(sd, BACKLOG) < 0) { perror("listen"); close(sd); return 1; }

    printf("Serveur Quiz démarré sur le port %d, %d questions chargées.\n", PORT, qcount);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int csock = accept(sd, (struct sockaddr*)&cliaddr, &clilen);
        if (csock < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork"); close(csock);
            continue;
        } else if (pid == 0) {
            // child
            close(sd); // child doesn't need listening socket
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cliaddr.sin_addr, ip, sizeof(ip));
            printf("Client connecté depuis %s:%d [pid=%d]\n", ip, ntohs(cliaddr.sin_port), getpid());
            handle_client(csock);
            // never returns
        } else {
            // parent
            close(csock);
            // optional: reap zombies via waitpid in signal handler (omitted here for brevity)
        }
    }

    close(sd);
    return 0;
}
