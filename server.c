#include <sys/socket.h>   
#include <netinet/in.h>   
#include <unistd.h>       
#include <pthread.h>      
#include <string.h>       
#include <stdio.h>        
#include <stdlib.h>       
#define PORTA            8080
#define BACKLOG          20
#define BUFFER_RICHIESTA 2048
#define BUFFER_LETTURA   1024
#define MAX_PERCORSO     256

static int invia_completo(int client_fd, const char *buffer, size_t len)
{
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t n = send(client_fd, buffer + total_sent, len - total_sent, 0);
        if (n <= 0) {
            if (n < 0) {
                perror("send");
            }
            return -1;
        }
        total_sent += (size_t)n;
    }
    return 0;
}

void *gestisci_client(void *arg)
{
    int *p_fd = (int *)arg;
    int client_fd = *p_fd;
    free(p_fd);
    p_fd = NULL;
    char buffer[BUFFER_RICHIESTA];
    memset(buffer, 0, sizeof(buffer));
    //traccia i file aperti per la chiusura unificata in cleanup 
    FILE *fp = NULL;
    ssize_t bytes_ricevuti = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_ricevuti <= 0) {
        if (bytes_ricevuti < 0) {
            perror("recv");
        }
        goto cleanup;
    }
    buffer[bytes_ricevuti] = '\0';
    char *fine_riga = strchr(buffer, '\r');
    if (fine_riga == NULL) {
        fine_riga = strchr(buffer, '\n');
    }
    if (fine_riga == NULL) {
        const char *risposta_400 = "HTTP/1.0 400 Bad Request\r\n\r\n";
        invia_completo(client_fd, risposta_400, strlen(risposta_400));
        goto cleanup;
    }
    *fine_riga = '\0';
    char *saveptr = NULL;
    //strtok r salva stato in saveptr così ogni thread ha segnalibro (no conflitti memoria strtok)
    char *metodo = strtok_r(buffer, " ", &saveptr);
    char *url = strtok_r(NULL, " ", &saveptr);
    if (metodo == NULL || url == NULL) {
        const char *risposta_400 = "HTTP/1.0 400 Bad Request\r\n\r\n";
        invia_completo(client_fd, risposta_400, strlen(risposta_400));
        goto cleanup;
    }
    if (strcmp(metodo, "GET") != 0) {
        const char *risposta_501 = "HTTP/1.0 501 Not Implemented\r\n\r\n";
        invia_completo(client_fd, risposta_501, strlen(risposta_501));
        goto cleanup;
    }
    //evita directory traversal per file fuori dalla cartella del progetto
    if (strstr(url, "..") != NULL) {
    const char *risposta_403 = "HTTP/1.0 403 Forbidden\r\n\r\n";
    invia_completo(client_fd, risposta_403, strlen(risposta_403));
    goto cleanup;
    }
    // / diventa index, url normali perdono /, path assoluto rifiutato
    const char *nome_file = url;
if (nome_file[0] == '\0' || strcmp(nome_file, "/") == 0) {
    nome_file = "index.html";
} else if (nome_file[0] == '/') {
    nome_file++;   
    if (nome_file[0] == '/') {
        const char *risposta_403 = "HTTP/1.0 403 Forbidden\r\n\r\n";
        invia_completo(client_fd, risposta_403, strlen(risposta_403));
        goto cleanup;
    }
}
    char percorso[MAX_PERCORSO];
    if (snprintf(percorso, sizeof(percorso), "%s", nome_file) >= (int)sizeof(percorso)) {
        const char *risposta_400 = "HTTP/1.0 400 Bad Request\r\n\r\n";
        invia_completo(client_fd, risposta_400, strlen(risposta_400));
        goto cleanup;
    }
    fp = fopen(percorso, "r");
    if (fp != NULL) {
        const char *header_200 =
            "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n";
        if (invia_completo(client_fd, header_200, strlen(header_200)) < 0) {
            goto cleanup;
        }
        char buffer_lettura[BUFFER_LETTURA];
        size_t bytes_read;
        while ((bytes_read = fread(buffer_lettura, 1, sizeof(buffer_lettura), fp)) > 0) {
            size_t total_sent = 0;
            while (total_sent < bytes_read) {
                ssize_t n = send(client_fd, buffer_lettura + total_sent, bytes_read - total_sent, 0);
                if (n <= 0) {
                    if (n < 0) {
                        perror("send");
                    }
                    goto cleanup;
                }
                total_sent += (size_t)n;
            }
        }
        if (ferror(fp)) {
            perror("fread");
            goto cleanup;
        }
        fclose(fp);
        fp = NULL;
        goto cleanup;
    }
    //file richiesto da fopen assente
    fp = fopen("404.html", "r");
    if (fp != NULL) {
        const char *header_404 =
            "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n";
        if (invia_completo(client_fd, header_404, strlen(header_404)) < 0) {
            goto cleanup;
        }
        char buffer_lettura[BUFFER_LETTURA];
        size_t bytes_read;
        while ((bytes_read = fread(buffer_lettura, 1, sizeof(buffer_lettura), fp)) > 0) {
            size_t total_sent = 0;
            while (total_sent < bytes_read) {
                ssize_t n = send(client_fd, buffer_lettura + total_sent, bytes_read - total_sent, 0);
                if (n <= 0) {
                    if (n < 0) {
                        perror("send");
                    }
                    goto cleanup;
                }
                total_sent += (size_t)n;
            }
        }
        if (ferror(fp)) {
            perror("fread");
            goto cleanup;
        }
        fclose(fp);
        fp = NULL;
        goto cleanup;
    }
    //fallback di emergenza se anche 404.html non esiste 
    const char *fallback =
        "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
        "<html><body><h1>404 Not Found (Fallback)</h1></body></html>";
    invia_completo(client_fd, fallback, strlen(fallback));
//uscita unica per chiudere risorse e terminare thread
cleanup:
    if (fp != NULL) {
        fclose(fp);
    }
    close(client_fd);
    pthread_exit(NULL);
    return NULL;
}

int main(void)
{
    int server_fd;
    struct sockaddr_in indirizzo_server;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    //attiva reuseaddr per riusare 8080 dopo riavvio
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    memset(&indirizzo_server, 0, sizeof(indirizzo_server));
    indirizzo_server.sin_family = AF_INET;
    indirizzo_server.sin_addr.s_addr = htonl(INADDR_ANY);
    indirizzo_server.sin_port = htons(PORTA);
    if (bind(server_fd, (struct sockaddr *)&indirizzo_server, sizeof(indirizzo_server)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("Server HTTP in ascolto su http://localhost:%d\n", PORTA);

    for (;;) {
        //indirizzo client usata solo per i parametri di accept
        struct sockaddr_in indirizzo_client;
        socklen_t lunghezza_client = sizeof(indirizzo_client);
        int client_fd = accept(server_fd, (struct sockaddr *)&indirizzo_client, &lunghezza_client);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        //perché pthreadcreate vuole void e client viene sovrascritto dal prossimo accept, evita race condition
        int *p_client_fd = malloc(sizeof(int));
        if (p_client_fd == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }
        *p_client_fd = client_fd;
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, gestisci_client, p_client_fd) != 0) {
            perror("pthread_create");
            free(p_client_fd);
            close(client_fd);
            continue;
        }
        //evito join per non avere zombie e tornare subito ad accept dopo aver creato thread (crea e distrugge)
        if (pthread_detach(thread_id) != 0) {
            perror("pthread_detach");
        }
    }
    close(server_fd);
    return 0;
}
