#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#define MAX_ROOMS      64
#define MAX_CLIENTS    128
#define MAX_PER_ROOM   64
#define ROOM_NAME_LEN  64
#define NICK_LEN       32
#define LINE_LEN       1024
#define HISTORY_TAIL   50

#define DATA_DIR       "data"
#define ROOMS_FILE     "data/rooms.txt"
#define MSG_DIR        "data/messages"

typedef struct {
    char name[ROOM_NAME_LEN];
    int  members[MAX_PER_ROOM]; // array of client fds
    int  nmembers;
} Room;

typedef struct {
    int  fd; // client socket fd
    int  active;
    int  has_nick;
    char nick[NICK_LEN];
    int  room_idx;
} Client;

static Room    rooms[MAX_ROOMS];
static int     nrooms = 0;
static Client  clients[MAX_CLIENTS];

static pthread_mutex_t state_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t io_mu    = PTHREAD_MUTEX_INITIALIZER;

// utility functions

static void send_line(int fd, const char *s) {
    size_t n = strlen(s);
    send(fd, s, n, MSG_NOSIGNAL);
    send(fd, "\n", 1, MSG_NOSIGNAL);
}

static void send_fmt(int fd, const char *fmt, ...) {
    char buf[LINE_LEN + 128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send_line(fd, buf);
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) {
        s[--n] = 0;
    }
}

static int valid_name(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') return 0;
    }
    return 1;
}

static int find_client_slot(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].fd == fd) return i;
    }
    return -1;
}

static int find_room(const char *name) {
    for (int i = 0; i < nrooms; i++) {
        if (strcmp(rooms[i].name, name) == 0) return i;
    }
    return -1;
}

static int nick_taken_locked(const char *nick) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].has_nick &&
            strcmp(clients[i].nick, nick) == 0) return 1;
    }
    return 0;
}

// state manipulation functions (caller must hold state_mu)
static void room_remove_member_locked(int room_idx, int fd) {
    if (room_idx < 0) return;
    Room *r = &rooms[room_idx];
    for (int i = 0; i < r->nmembers; i++) {
        if (r->members[i] == fd) {
            r->members[i] = r->members[--r->nmembers];
            return;
        }
    }
}

// caller must hold state_mu
static void broadcast_room_locked(int room_idx, int except_fd, const char *line) {
    if (room_idx < 0) return;
    Room *r = &rooms[room_idx];
    for (int i = 0; i < r->nmembers; i++) {
        if (r->members[i] == except_fd) continue;
        send_line(r->members[i], line);
    }
}

// initialization and persistence
static void ensure_dirs(void) {
    mkdir(DATA_DIR, 0755);
    mkdir(MSG_DIR, 0755);
}

static void load_rooms(void) {
    FILE *f = fopen(ROOMS_FILE, "r");
    if (!f) return;
    char line[ROOM_NAME_LEN + 4];
    while (fgets(line, sizeof(line), f) && nrooms < MAX_ROOMS) {
        rstrip(line);
        if (!*line) continue;
        if (!valid_name(line)) continue;
        if (find_room(line) >= 0) continue;
        strncpy(rooms[nrooms].name, line, ROOM_NAME_LEN - 1);
        rooms[nrooms].nmembers = 0;
        nrooms++;
    }
    fclose(f);
}

static void persist_room(const char *name) {
    pthread_mutex_lock(&io_mu);
    FILE *f = fopen(ROOMS_FILE, "a");
    if (f) {
        fprintf(f, "%s\n", name);
        fclose(f);
    }
    pthread_mutex_unlock(&io_mu);
}

static void persist_message(const char *room, const char *nick, const char *text) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.log", MSG_DIR, room);
    pthread_mutex_lock(&io_mu);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%ld\t%s\t%s\n", (long)time(NULL), nick, text);
        fclose(f);
    }
    pthread_mutex_unlock(&io_mu);
}

static void send_history(int fd, const char *room) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.log", MSG_DIR, room);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char *lines[HISTORY_TAIL];
    int count = 0, head = 0;
    for (int i = 0; i < HISTORY_TAIL; i++) lines[i] = NULL;

    char buf[LINE_LEN + 128];
    while (fgets(buf, sizeof(buf), f)) {
        rstrip(buf);
        if (lines[head]) free(lines[head]);
        lines[head] = strdup(buf);
        head = (head + 1) % HISTORY_TAIL;
        if (count < HISTORY_TAIL) count++;
    }
    fclose(f);

    int start = (count < HISTORY_TAIL) ? 0 : head;
    for (int i = 0; i < count; i++) {
        char *ln = lines[(start + i) % HISTORY_TAIL];
        if (!ln) continue;
        char *t1 = strchr(ln, '\t');
        if (!t1) continue;
        char *nick = t1 + 1;
        char *t2 = strchr(nick, '\t');
        if (!t2) continue;
        *t2 = 0;
        char *text = t2 + 1;
        send_fmt(fd, "MSG %s %s %s", room, nick, text);
    }
    for (int i = 0; i < HISTORY_TAIL; i++) free(lines[i]);
}

// command handlers
static void cmd_nick(int ci, char *arg) {
    int fd = clients[ci].fd;
    if (!arg || !*arg) { send_line(fd, "ERR usage: /nick <name>"); return; }
    if (strlen(arg) >= NICK_LEN) { send_line(fd, "ERR nick too long"); return; }
    if (!valid_name(arg)) { send_line(fd, "ERR invalid nick"); return; }

    pthread_mutex_lock(&state_mu);
    if (clients[ci].has_nick) {
        pthread_mutex_unlock(&state_mu);
        send_line(fd, "ERR nick already set");
        return;
    }
    if (nick_taken_locked(arg)) {
        pthread_mutex_unlock(&state_mu);
        send_line(fd, "ERR nick taken");
        return;
    }
    strncpy(clients[ci].nick, arg, NICK_LEN - 1);
    clients[ci].has_nick = 1;
    pthread_mutex_unlock(&state_mu);
    send_fmt(fd, "OK nick %s", arg);
}

static void cmd_rooms(int ci) {
    int fd = clients[ci].fd;
    char buf[LINE_LEN];
    size_t off = 0;
    off += snprintf(buf + off, sizeof(buf) - off, "ROOMS");
    pthread_mutex_lock(&state_mu);
    for (int i = 0; i < nrooms && off < sizeof(buf) - 2; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, " %s", rooms[i].name);
    }
    pthread_mutex_unlock(&state_mu);
    send_line(fd, buf);
}

static void cmd_create(int ci, char *arg) {
    int fd = clients[ci].fd;
    if (!arg || !*arg) { send_line(fd, "ERR usage: /create <room>"); return; }
    if (strlen(arg) >= ROOM_NAME_LEN) { send_line(fd, "ERR room name too long"); return; }
    if (!valid_name(arg)) { send_line(fd, "ERR invalid room name"); return; }

    pthread_mutex_lock(&state_mu);
    if (find_room(arg) >= 0) {
        pthread_mutex_unlock(&state_mu);
        send_line(fd, "ERR room exists");
        return;
    }
    if (nrooms >= MAX_ROOMS) {
        pthread_mutex_unlock(&state_mu);
        send_line(fd, "ERR too many rooms");
        return;
    }
    strncpy(rooms[nrooms].name, arg, ROOM_NAME_LEN - 1);
    rooms[nrooms].nmembers = 0;
    nrooms++;
    pthread_mutex_unlock(&state_mu);

    persist_room(arg);
    send_fmt(fd, "OK created %s", arg);
}

static void cmd_join(int ci, char *arg) {
    int fd = clients[ci].fd;
    if (!clients[ci].has_nick) { send_line(fd, "ERR set /nick first"); return; }
    if (!arg || !*arg) { send_line(fd, "ERR usage: /join <room>"); return; }

    char join_msg[NICK_LEN + 16];
    char who[LINE_LEN];
    char target[ROOM_NAME_LEN];
    int new_idx;

    pthread_mutex_lock(&state_mu);
    new_idx = find_room(arg);
    if (new_idx < 0) {
        pthread_mutex_unlock(&state_mu);
        send_line(fd, "ERR no such room");
        return;
    }
    Room *r = &rooms[new_idx];
    if (r->nmembers >= MAX_PER_ROOM) {
        pthread_mutex_unlock(&state_mu);
        send_line(fd, "ERR room full");
        return;
    }

    int old_idx = clients[ci].room_idx;
    if (old_idx == new_idx) {
        pthread_mutex_unlock(&state_mu);
        send_line(fd, "ERR already in room");
        return;
    }
    if (old_idx >= 0) {
        char leave_msg[NICK_LEN + 16];
        snprintf(leave_msg, sizeof(leave_msg), "LEAVE %s", clients[ci].nick);
        room_remove_member_locked(old_idx, fd);
        broadcast_room_locked(old_idx, fd, leave_msg);
    }

    r->members[r->nmembers++] = fd;
    clients[ci].room_idx = new_idx;
    snprintf(join_msg, sizeof(join_msg), "JOIN %s", clients[ci].nick);
    broadcast_room_locked(new_idx, fd, join_msg);

    size_t off = 0;
    off += snprintf(who + off, sizeof(who) - off, "WHO %s", r->name);
    for (int i = 0; i < r->nmembers && off < sizeof(who) - 2; i++) {
        int mfd = r->members[i];
        int mci = find_client_slot(mfd);
        if (mci >= 0 && clients[mci].has_nick) {
            off += snprintf(who + off, sizeof(who) - off, " %s", clients[mci].nick);
        }
    }
    strncpy(target, r->name, ROOM_NAME_LEN);
    pthread_mutex_unlock(&state_mu);

    send_fmt(fd, "OK joined %s", target);
    send_history(fd, target);
    send_line(fd, who);
}

static void cmd_who(int ci) {
    int fd = clients[ci].fd;
    char who[LINE_LEN];
    pthread_mutex_lock(&state_mu);
    int ri = clients[ci].room_idx;
    if (ri < 0) {
        pthread_mutex_unlock(&state_mu);
        send_line(fd, "ERR not in a room");
        return;
    }
    Room *r = &rooms[ri];
    size_t off = 0;
    off += snprintf(who + off, sizeof(who) - off, "WHO %s", r->name);
    for (int i = 0; i < r->nmembers && off < sizeof(who) - 2; i++) {
        int mci = find_client_slot(r->members[i]);
        if (mci >= 0 && clients[mci].has_nick) {
            off += snprintf(who + off, sizeof(who) - off, " %s", clients[mci].nick);
        }
    }
    pthread_mutex_unlock(&state_mu);
    send_line(fd, who);
}

static void cmd_leave(int ci) {
    int fd = clients[ci].fd;
    char leave_msg[NICK_LEN + 16];
    pthread_mutex_lock(&state_mu);
    int ri = clients[ci].room_idx;
    if (ri < 0) {
        pthread_mutex_unlock(&state_mu);
        send_line(fd, "ERR not in a room");
        return;
    }
    snprintf(leave_msg, sizeof(leave_msg), "LEAVE %s", clients[ci].nick);
    room_remove_member_locked(ri, fd);
    broadcast_room_locked(ri, fd, leave_msg);
    clients[ci].room_idx = -1;
    pthread_mutex_unlock(&state_mu);
    send_line(fd, "OK left");
}

static void handle_message(int ci, const char *text) {
    int fd = clients[ci].fd;
    if (!clients[ci].has_nick) { send_line(fd, "ERR set /nick first"); return; }

    char msg[LINE_LEN + NICK_LEN + ROOM_NAME_LEN + 32];
    char room_name[ROOM_NAME_LEN];
    char nick_copy[NICK_LEN];

    pthread_mutex_lock(&state_mu);
    int ri = clients[ci].room_idx;
    if (ri < 0) {
        pthread_mutex_unlock(&state_mu);
        send_line(fd, "ERR /join a room first");
        return;
    }
    strncpy(room_name, rooms[ri].name, ROOM_NAME_LEN);
    strncpy(nick_copy, clients[ci].nick, NICK_LEN);
    snprintf(msg, sizeof(msg), "MSG %s %s %s", room_name, nick_copy, text);
    broadcast_room_locked(ri, -1, msg);
    pthread_mutex_unlock(&state_mu);

    persist_message(room_name, nick_copy, text);
}

// client thread
static void process_line(int ci, char *line) {
    rstrip(line);
    if (!*line) return;

    if (line[0] == '/') {
        char *cmd = line + 1;
        char *arg = strchr(cmd, ' ');
        if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }

        if      (strcmp(cmd, "nick")   == 0) cmd_nick(ci, arg);
        else if (strcmp(cmd, "rooms")  == 0) cmd_rooms(ci);
        else if (strcmp(cmd, "create") == 0) cmd_create(ci, arg);
        else if (strcmp(cmd, "join")   == 0) cmd_join(ci, arg);
        else if (strcmp(cmd, "who")    == 0) cmd_who(ci);
        else if (strcmp(cmd, "leave")  == 0) cmd_leave(ci);
        else if (strcmp(cmd, "quit")   == 0) {
            send_line(clients[ci].fd, "OK bye");
            shutdown(clients[ci].fd, SHUT_RDWR);
        }
        else send_line(clients[ci].fd, "ERR unknown command");
    } else {
        handle_message(ci, line);
    }
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;

    pthread_mutex_lock(&state_mu);
    int ci = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].active   = 1;
            clients[i].fd       = fd;
            clients[i].has_nick = 0;
            clients[i].nick[0]  = 0;
            clients[i].room_idx = -1;
            ci = i;
            break;
        }
    }
    pthread_mutex_unlock(&state_mu);

    if (ci < 0) {
        send_line(fd, "ERR server full");
        close(fd);
        return NULL;
    }

    send_line(fd, "INFO welcome. commands: /nick /rooms /create /join /who /leave /quit");

    char buf[LINE_LEN * 2];
    size_t have = 0;

    for (;;) {
        ssize_t n = recv(fd, buf + have, sizeof(buf) - have - 1, 0);
        if (n <= 0) break;
        have += (size_t)n;
        buf[have] = 0;

        for (;;) {
            char *nl = memchr(buf, '\n', have);
            if (!nl) break;
            size_t linelen = (size_t)(nl - buf);
            char line[LINE_LEN + 1];
            size_t copy = linelen < LINE_LEN ? linelen : LINE_LEN;
            memcpy(line, buf, copy);
            line[copy] = 0;
            process_line(ci, line);
            size_t consumed = linelen + 1;
            memmove(buf, buf + consumed, have - consumed);
            have -= consumed;
        }
        if (have >= sizeof(buf) - 1) have = 0; /* drop overlong */
    }

    pthread_mutex_lock(&state_mu);
    int ri = clients[ci].room_idx;
    if (ri >= 0 && clients[ci].has_nick) {
        char leave_msg[NICK_LEN + 16];
        snprintf(leave_msg, sizeof(leave_msg), "LEAVE %s", clients[ci].nick);
        room_remove_member_locked(ri, fd);
        broadcast_room_locked(ri, fd, leave_msg);
    } else if (ri >= 0) {
        room_remove_member_locked(ri, fd);
    }
    clients[ci].active = 0;
    pthread_mutex_unlock(&state_mu);

    close(fd);
    return NULL;
}

// main server loop
int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("USAGE: %s <port>\n", argv[0]);
        exit(1);
    }
    unsigned int port = atoi(argv[1]);
    if (port < 1 || port > 65535) {
        printf("ERROR #1: invalid port specified.\n");
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);

    ensure_dirs();
    load_rooms();

    int l_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (l_socket < 0) {
        fprintf(stderr, "ERROR #2: cannot create listening socket.\n");
        exit(1);
    }
    int yes = 1;
    setsockopt(l_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons(port);

    if (bind(l_socket, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        fprintf(stderr, "ERROR #3: bind listening socket.\n");
        exit(1);
    }
    if (listen(l_socket, 16) < 0) {
        fprintf(stderr, "ERROR #4: error in listen().\n");
        exit(1);
    }

    printf("Chat server listening on port %u (rooms loaded: %d)\n", port, nrooms);

    for (;;) {
        struct sockaddr_in clientaddr;
        socklen_t clen = sizeof(clientaddr);
        int c_socket = accept(l_socket, (struct sockaddr *)&clientaddr, &clen);
        if (c_socket < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "ERROR #5: accept failed.\n");
            continue;
        }
        printf("Connection from %s\n", inet_ntoa(clientaddr.sin_addr));

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, (void *)(intptr_t)c_socket) != 0) {
            fprintf(stderr, "ERROR: pthread_create failed.\n");
            close(c_socket);
            continue;
        }
        pthread_detach(tid);
    }

    return 0;
}
