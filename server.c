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
    char answer[256]; // Réponses correctes
    int difficulty; // 1, 2 ou 3
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
        "difficulty TEXT, " // Colonne pour le niveau
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";
    rc = sqlite3_exec(db, sql_scores, 0, 0, &err_msg); 
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Erreur SQL: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
    sqlite3_close(db); 
}

// Sauvegarde le score d'un utilisateur dans la base avec la difficulté
void save_score_db_with_diff(const char *user, int score, const char *diff_name) {
    sqlite3 *db;
    sqlite3_open(DB_FILE, &db);
    // On inclut la difficulté dans l'insertion
    const char *sql = "INSERT INTO scores (username, score, difficulty) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, 0); 
    sqlite3_bind_text(stmt, 1, user, -1, SQLITE_STATIC); 
    sqlite3_bind_int(stmt, 2, score); 
    sqlite3_bind_text(stmt, 3, diff_name, -1, SQLITE_STATIC);
    sqlite3_step(stmt); 
    sqlite3_finalize(stmt); 
    sqlite3_close(db);
}

// Envoie les meilleurs scores des utilisateurs
void send_leaderboard(int sock, int diff_filter) {
    sqlite3 *db;
    sqlite3_open(DB_FILE, &db);
    sqlite3_stmt *stmt; // curseur placé au début de la liste des réponses
    char buffer[BUFSIZE * 2] = ""; // Buffer plus large pour contenir tout le classement
    
    // Si diff_filter > 0, on affiche le top 5 du niveau de l'utilisateur
    // Si diff_filter == 0, on affiche le top 3 de tous les niveaux (l'utilisateur est dans le menu)
    int start = (diff_filter == 0) ? 1 : diff_filter;
    int end = (diff_filter == 0) ? 3 : diff_filter;
    int limit = (diff_filter == 0) ? 3 : 5;

    for (int d = start; d <= end; d++) {
        char title[100];
        char *diff_name = (d==1)?"DEBUTANT":(d==2)?"INTERMEDIAIRE":"EXPERT";
        snprintf(title, sizeof(title), "\n--- TOP %d %s ---\n", limit, diff_name);
        strcat(buffer, title);

        // Sélectionne les meilleurs scores filtrés par difficulté
        const char *sql = "SELECT username, score FROM scores WHERE difficulty = ? ORDER BY score DESC LIMIT ?;";
        sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
        sqlite3_bind_text(stmt, 1, diff_name, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, limit);

        int rank = 1;
        // Boucle sur les résultats
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            char line[128];
            const unsigned char *u = sqlite3_column_text(stmt, 0); // pseudo
            int s = sqlite3_column_int(stmt, 1); // score
            snprintf(line, sizeof(line), "%d. %s : %d pts\n", rank++, u, s);
            strcat(buffer, line);
        }
        sqlite3_finalize(stmt);
    }
    // Envoie le classement complet
    send_all(sock, "%s", buffer);
    sqlite3_close(db);
}

// Charge les questions à partir du fichier questions.txt
int load_questions(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return 0;
    char line[1024];
    qcount = 0;
    // Découpe le texte en 3 (question, réponse, difficulté)
    while (fgets(line, sizeof(line), f) && qcount < MAXQ) {
        char *q = strtok(line, "|");
        char *a = strtok(NULL, "|");
        char *d = strtok(NULL, "|");

        if (q && a && d) {
            strncpy(questions[qcount].question, q, 511);
            strncpy(questions[qcount].answer, a, 255);
            questions[qcount].difficulty = atoi(d);
            qcount++;
        }
    }
    fclose(f);
    return qcount;
}

// --- Fonction dédiée au jeu avec gestion de la difficulté ---
void play_quiz(int csock, const char *username, int diff_choice) {
    char buf[BUFSIZE];
    int score = 0;
    char *diff_name = (diff_choice == 1) ? "DEBUTANT" : (diff_choice == 2) ? "INTERMEDIAIRE" : "EXPERT";

    // Tableau des questions disponibles (indices)
    int *avail = malloc(qcount * sizeof(int)); 
    if (!avail) return;

    // Initialise le tableau en filtrant par la difficulté choisie
    int n_avail = 0; // Nombre de questions disponibles pour ce niveau
    for(int i = 0; i < qcount; i++) {
        if (questions[i].difficulty == diff_choice) {
            avail[n_avail++] = i;
        }
    }

    // Sécurité si aucune question n'est trouvée pour ce niveau
    if (n_avail == 0) {
        send_all(csock, "Erreur: Aucune question chargée pour le niveau %s.\n", diff_name);
        free(avail);
        return;
    }

    send_all(csock, "INFO: C'est parti %s ! Mode: %s. Attention, une erreur et c'est fini !\n", username, diff_name);

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
    
    // Sauvegarde en base avec la difficulté
    save_score_db_with_diff(username, score, diff_name);
    send_all(csock, "Score sauvegardé en base de données pour le niveau %s.\n", diff_name);
    
    // Affichage du classement spécifique à la difficulté jouée
    send_leaderboard(csock, diff_choice);
    
    free(avail);
}

// --- Gère le client : Authentification et Menu Principal ---
void handle_client(int csock) {
    srand(time(NULL) ^ getpid()); 

    char username[100];
    char buf[BUFSIZE];

    // Demande du pseudo
    send_all(csock, "INPUT: Bienvenue sur le Quiz Réseau ! Entrez votre pseudo : \n");

    if (recv_line(csock, buf, sizeof(buf)) <= 0) {
        close(csock); 
        return;
    }
    
    if (strlen(buf) == 0) strcpy(username, "anonyme");
    else strcpy(username, buf);

    // Boucle du Menu Principal
    while (1) {
        send_all(csock, "\n--- MENU PRINCIPAL ---\n");
        send_all(csock, "1. Jouer au Quiz\n");
        send_all(csock, "2. Voir le classement\n");
        send_all(csock, "q. Quitter\n");
        send_all(csock, "INPUT: Votre choix : \n");
        
        if (recv_line(csock, buf, sizeof(buf)) <= 0) break;
        normalize(buf);

        if (strcmp(buf, "1") == 0) {
            // Demande de difficulté
            send_all(csock, "\n--- CHOISISSEZ LA DIFFICULTÉ ---\n");
            send_all(csock, "1. Débutant\n");
            send_all(csock, "2. Intermédiaire\n");
            send_all(csock, "3. Expert\n");
            send_all(csock, "INPUT: Votre choix (1, 2 ou 3) : \n");
            
            if (recv_line(csock, buf, sizeof(buf)) <= 0) break;
            int diff = atoi(buf);
            if (diff < 1 || diff > 3) diff = 1; // Valeur par défaut

            play_quiz(csock, username, diff);
            // On peut choisir de revenir au menu ou de quitter. Ici on continue le menu.
            continue; 
        } 
        else if (strcmp(buf, "2") == 0) {
            // Affiche le Top 3 de chaque catégorie
            send_leaderboard(csock, 0);
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
        // Question de secours
        questions[0].difficulty = 1;
        strcpy(questions[0].question, "Test ?");
        strcpy(questions[0].answer, "Oui");
        qcount = 1;
    } else {
        printf("%d questions chargées.\n", qcount);
    }

    int sd = socket(AF_INET, SOCK_STREAM, 0); 
    if (sd < 0) { perror("socket"); return 1; }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY; 
    servaddr.sin_port = htons(PORT); 
    
    int opt=1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind"); return 1;
    }
    
    if (listen(sd, 10) < 0) { perror("listen"); return 1; }
    
    printf("Serveur Quiz démarré sur le port %d\n", PORT);

    while (1) { 
        struct sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);
        int csock = accept(sd, (struct sockaddr*)&cliaddr, &len); 
        
        if (csock < 0) {
            if (errno == EINTR) continue; 
            perror("accept");
            continue;
        }

        pid_t pid = fork(); 
        if (pid < 0) {
            perror("fork");
            close(csock);
        } else if (pid == 0) {
            close(sd); 
            handle_client(csock); 
        } else {
            close(csock); 
            while(waitpid(-1, NULL, WNOHANG) > 0); 
        }
    }
    return 0;
}