/* server.c
   Quiz réseau multi-clients (fork) avec Menu et Base de données
   Compile: gcc server.c -o server -lsqlite3
*/

// Bibliothèques
#include <stdio.h>      // Entrées/Sorties
#include <stdlib.h>     // Fonctions utilitaires
#include <string.h>     // Manipulation des chaînes de caractères
#include <ctype.h>      // Fonctions de classification de caractères
#include <unistd.h>     // Fonctions POSIX (fork, close)
#include <time.h>       // Gestion du temps
#include <errno.h>      // Gestion des erreurs
#include <sys/types.h>  // Types de données
#include <sys/socket.h> // Fonctions de socket
#include <netinet/in.h> // Structures d'adresse Internet
#include <arpa/inet.h>  // Fonctions de conversion d'adresse
#include <sys/wait.h>   // Gestion des processus enfants
#include <stdarg.h>     // Gestion des arguments variables
#include <sqlite3.h>    // Librairie SQLite pour la base de données

// Constantes
#define PORT 5000       // Port d'écoute du serveur
#define MAXQ 2000       // Nombre maximum de questions
#define BUFSIZE 1024    // Taille des buffers de communication
#define DB_FILE "quiz.db" // Nom du fichier de la base de données SQLite

typedef struct {
    char question[512]; // Texte des questions
    char answer[256];   // Réponses correctes
} QA;

QA questions[MAXQ]; // Tableau stockant les questions/réponses
int qcount = 0;     // Compte le nombre de questions chargées

// Normalise une chaîne de caractère pour la comparaison
void normalize(char *s) {
    char *start = s;
    // Suppression des espaces de début
    while (*start && isspace((unsigned char)*start)) start++;
    // Suppression des espaces de fin
    char *end = s + strlen(start);
    while (end > start && isspace((unsigned char)*(end-1))) end--;
    *end = '\0';
    // Conversion en minuscule
    for (char *p = start; *p; ++p) {
        if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
        *p = tolower((unsigned char)*p);
    }
    if (start != s) memmove(s, start, strlen(start)+1);
}

// Envoi d'un message sur la socket (permet de simplifier l'envoi de texte en incluant des variables)
ssize_t send_all(int sock, const char *fmt, ...) {
    char buf[BUFSIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args); 
    va_end(args);
    return send(sock, buf, strlen(buf), 0);
}

// Reçoit une ligne depuis la socket
ssize_t recv_line(int sock, char *buf, size_t bufsz) {
    size_t idx = 0;
    while (idx + 1 < bufsz) { // tant qu'il reste de la place dans le buffer
        char c;
        ssize_t r = recv(sock, &c, 1, 0);  // lit un seul caractère
        if (r <= 0) return (r == 0 && idx>0) ? (ssize_t)idx : -1; 
        buf[idx++] = c;
        if (c == '\n') break; // sortir de la boucle si la touche entrée est pressée
    }
    buf[idx] = '\0'; // caractère spécial indiquant que la ligne est terminée
    
    // Suppression du caractère '\n' (touche entrée)
    if (idx > 0 && buf[idx-1] == '\n') buf[idx-1] = '\0';
    return idx;
}

// Initialise la base de données
void init_db() {
    sqlite3 *db;
    char *err_msg = 0;
    int rc = sqlite3_open(DB_FILE, &db); 
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Erreur ouverture DB: %s\n", sqlite3_errmsg(db));
        return;
    }
    const char *sql_scores = 
        "CREATE TABLE IF NOT EXISTS scores ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "username TEXT, "
        "score INTEGER, "
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";
    rc = sqlite3_exec(db, sql_scores, 0, 0, &err_msg); 
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Erreur SQL: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
    sqlite3_close(db); 
}

// Sauvegarde le score d'un utilisateur dans la base
void save_score_db(const char *user, int score) {
    sqlite3 *db;
    sqlite3_open(DB_FILE, &db);
    const char *sql = "INSERT INTO scores (username, score) VALUES (?, ?);";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, 0); 
    sqlite3_bind_text(stmt, 1, user, -1, SQLITE_STATIC); 
    sqlite3_bind_int(stmt, 2, score); 
    sqlite3_step(stmt); 
    sqlite3_finalize(stmt); 
    sqlite3_close(db);
}

// Envoie le top 5 des meilleurs scores des utilisateurs
void send_global_leaderboard(int sock) {
    sqlite3 *db;
    sqlite3_open(DB_FILE, &db);
    sqlite3_stmt *stmt; // curseur placé au début de la liste des réponses
    // Sélectionne les 5 meilleurs scores par ordre décroissant pour les afficher
    const char *sql = "SELECT username, score FROM scores ORDER BY score DESC LIMIT 5;";
    char buffer[BUFSIZE] = "\n=== TOP 5 MEILLEURS JOUEURS ===\n";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        int rank = 1;
        // Boucle sur les résultats
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            char line[128];
            // Récupère les données
            const unsigned char *u = sqlite3_column_text(stmt, 0); // lis la colonne 0 (pseudo)
            int s = sqlite3_column_int(stmt, 1); // lis la colonne 1 (score)
            snprintf(line, sizeof(line), "%d. %s : %d pts\n", rank++, u, s); // formate l'affichage de la ligne
            strcat(buffer, line); // ajout de la ligne au texte final
        }
    }
    strcat(buffer, "===============================\n");
    // Envoie le classement complet
    send_all(sock, "%s", buffer); 
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

// Charge les question à partir du fichier questions.txt
int load_questions(const char *filename) {
    FILE *f = fopen(filename, "r"); // le serveur cherche le fichier dans le disque dur en lecture seule ("r")
    if (!f) return 0; // Échec ouverture fichier
    char line[768];
    qcount = 0;
    while (fgets(line, sizeof(line), f) && qcount < MAXQ) {
        // Cherche le séparateur '|'
        char *sep = strchr(line, '|'); 
        if (!sep) continue;
        *sep = '\0'; // Remplace '|' par '\0' pour séparer la question et la réponse
        // Copie la question
        strncpy(questions[qcount].question, line, 511); 
        // Copie la réponse
        strncpy(questions[qcount].answer, sep + 1, 255); 
        char *nl = strchr(questions[qcount].answer, '\n');
        if (nl) *nl = '\0';
        qcount++;
    }
    fclose(f);
    return qcount;
}

// --- Fonction dédiée au jeu ---
void play_quiz(int csock, const char *username) {
    char buf[BUFSIZE];
    int score = 0;
    
    // Tableau des questions disponibles (indices)
    int *avail = malloc(qcount * sizeof(int)); 
    if (!avail) return;
    // Initialise le tableau
    for(int i=0; i<qcount; i++) avail[i] = i; 
    int n_avail = qcount; // Nombre de questions disponibles

    send_all(csock, "INFO: C'est parti %s ! Attention, une erreur et c'est fini !\n", username);

    // Boucle de jeu
    while (n_avail > 0) {
        // Choix d'une question aléatoire
        int idx_in_avail = rand() % n_avail; 
        int q_idx = avail[idx_in_avail];

        // Envoi de la question
        send_all(csock, "\nINPUT: QUESTION: %s\n", questions[q_idx].question);
        
        // Réception de la réponse
        if (recv_line(csock, buf, sizeof(buf)) <= 0) break;

        // Quitter le jeu
        if (strcasecmp(buf, "q") == 0) break; 

        char correct[256];
        strcpy(correct, questions[q_idx].answer);
        // Normalisation
        normalize(buf); // réponse de l'utilisateur
        normalize(correct); // bonne réponse

        if (strcmp(buf, correct) == 0) { // comparaison des 2
            score++; // incrémente le score car la réponse est bonne
            send_all(csock, "Bonne réponse ! Score actuel: %d\n", score);
            // Retire la question du tableau et décrémente le nb de questions disponibles
            avail[idx_in_avail] = avail[n_avail-1];
            n_avail--;
        } else {
            // Mauvaise réponse, affichage de la phrase de fin et sortie du jeu
            send_all(csock, "INCORRECT. La réponse était: %s\n", questions[q_idx].answer);
            break; 
        }
    }

    // Fin du jeu
    send_all(csock, "FIN DU QUIZ. Score final: %d\n", score);
    save_score_db(username, score);
    send_all(csock, "Score sauvegardé en base de données.\n");
    send_global_leaderboard(csock);
    
    free(avail);
}

// --- Gère le client : Authentification et Menu Principal ---
void handle_client(int csock) {
    // Initialisation aléatoire unique
    srand(time(NULL) ^ getpid()); 

    char username[100];
    char buf[BUFSIZE];

    // Demande du pseudo
    send_all(csock, "INPUT: Bienvenue sur le Quiz Réseau ! Entrez votre pseudo : \n");

    // Réception du pseudo
    if (recv_line(csock, buf, sizeof(buf)) <= 0) {
        close(csock); 
        return;
    }
    
    // Attribue un pseudo par défaut si vide
    if (strlen(buf) == 0) strcpy(username, "anonyme");
    else strcpy(username, buf);

    // Boucle du Menu Principal
    while (1) {
        // Envoi du menu ligne par ligne pour éviter les bugs d'affichage
        send_all(csock, "\n--- MENU PRINCIPAL ---\n");
        send_all(csock, "1. Jouer au Quiz\n");
        send_all(csock, "2. Voir le classement\n");
        send_all(csock, "q. Quitter\n");
        
        // Ajout de \n à la fin de la demande de choix
        // Cela débloque le recv_line du client
        send_all(csock, "INPUT: Votre choix : \n");
        
        if (recv_line(csock, buf, sizeof(buf)) <= 0) break;

        // On nettoie la réponse au cas où il y a des espaces
        normalize(buf);

        if (strcmp(buf, "1") == 0) {
            play_quiz(csock, username);
            break; // Quitte après une partie
        } 
        else if (strcmp(buf, "2") == 0) {
            send_global_leaderboard(csock);
            // Pas de break, on revient au menu (continue)
            continue; 
        } 
        else if (strcmp(buf, "q") == 0) {
            break;
        } 
        else {
            send_all(csock, "Choix invalide (%s).\n", buf);
        }
    }

    send_all(csock, "BYE: Au revoir %s !\n", username);
    close(csock);
    exit(0);
}

// --- Fonction Principale ---
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    init_db(); // Initialise la BDD
    
    // Chargement des questions
    if (load_questions("questions.txt") == 0) {
        fprintf(stderr, "Attention: Aucune question chargée.\n");
        strcpy(questions[0].question, "Test ?");
        strcpy(questions[0].answer, "Oui");
        qcount = 1;
    } else {
        printf("%d questions chargées.\n", qcount);
    }

    // Création de la socket

    // Utilise les protocoles IPv4 (AF_INET) et TCP (SOCK_STREAM)
    int sd = socket(AF_INET, SOCK_STREAM, 0); // sd est un entier car il sert d'identifiant
    if (sd < 0) { perror("socket"); return 1; }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    // Écoute sur toutes les interfaces
    servaddr.sin_addr.s_addr = INADDR_ANY; 
    servaddr.sin_port = htons(PORT); 
    
    // Option pour réutiliser le port
    int opt=1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // attache le socket au port de la machine
    if (bind(sd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind"); return 1;
    }
    
    // écoute pour détecter si quelqu'un essaie de se connecter sur le port
    if (listen(sd, 10) < 0) { perror("listen"); return 1; }
    
    printf("Serveur Quiz démarré sur le port %d\n", PORT);

    while (1) { // ne s'arrête pas tant que le programme tourne
        struct sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);
        int csock = accept(sd, (struct sockaddr*)&cliaddr, &len); // attend qu'un client se connecte
        
        if (csock < 0) {
            if (errno == EINTR) continue; 
            perror("accept");
            continue;
        }

        // Fork
        pid_t pid = fork(); // dédoublement, 2 programmes identiques tournent en même temps
        if (pid < 0) {
            perror("fork");
            close(csock);
        } else if (pid == 0) {
            // Enfant
            close(sd); // n'a pas besoin de recevoir d'autres appels donc ferme le socket
            handle_client(csock); // fait jouer l'utilisateur
        } else {
            // Parent
            close(csock); // ferme le socket client pour lui car c'est le processus enfant qui s'en sert
            while(waitpid(-1, NULL, WNOHANG) > 0); 
        }
    }
    return 0;
}