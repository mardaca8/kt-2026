/*
 * Chat klientas
 *
 * Dvi gijos:
 *   - skaitymo gija: server -> stdout
 *   - pagrindine gija: stdin -> server
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#define BUFFLEN 1024

static int g_socket = -1;
static volatile int g_done = 0;

static void *reader_thread(void *arg) {
    (void)arg;
    char buf[BUFFLEN + 1];
    for (;;) {
        ssize_t n = recv(g_socket, buf, BUFFLEN, 0);
        if (n <= 0) {
            g_done = 1;
            printf("\n[disconnected]\n");
            fflush(stdout);
            break;
        }
        buf[n] = 0;
        fputs(buf, stdout);
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "USAGE: %s <ip> <port>\n", argv[0]);
        exit(1);
    }
    unsigned int port = atoi(argv[2]);
    if (port < 1 || port > 65535) {
        printf("ERROR #1: invalid port specified.\n");
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);

    g_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_socket < 0) {
        fprintf(stderr, "ERROR #2: cannot create socket.\n");
        exit(1);
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port   = htons(port);
    if (inet_aton(argv[1], &servaddr.sin_addr) <= 0) {
        fprintf(stderr, "ERROR #3: Invalid remote IP address.\n");
        exit(1);
    }

    if (connect(g_socket, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        fprintf(stderr, "ERROR #4: error in connect().\n");
        exit(1);
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, reader_thread, NULL) != 0) {
        fprintf(stderr, "ERROR #5: pthread_create failed.\n");
        exit(1);
    }
    pthread_detach(tid);

    char line[BUFFLEN];
    while (!g_done && fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        if (n == 0) continue;
        if (line[n-1] != '\n') {
            if (n < sizeof(line) - 1) { line[n++] = '\n'; line[n] = 0; }
        }
        if (send(g_socket, line, strlen(line), 0) < 0) break;
        if (strncmp(line, "/quit", 5) == 0) break;
    }

    shutdown(g_socket, SHUT_RDWR);
    close(g_socket);
    return 0;
}
