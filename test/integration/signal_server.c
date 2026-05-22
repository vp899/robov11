/**
 * @file signal_server.c
 * @brief Minimal signaling server for integration tests.
 *
 * Listens on TCP, handles room join and offer/answer relay.
 * Simplified version — no Redis, no auth, just room-based message routing.
 *
 * Usage: ./signal_server <port>
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <time.h>

#define MAX_CLIENTS     128
#define MAX_ROOMS       32
#define MAX_MSG_SIZE    4096
#define ROOM_ID_LEN     32
#define EPOLL_BATCH     16

/* ── Message types ────────────────────────────────────────────────── */
#define MSG_JOIN_ROOM       4
#define MSG_LEAVE_ROOM      5
#define MSG_OFFER           1
#define MSG_ANSWER          2
#define MSG_ICE_CANDIDATE   3
#define MSG_PING            8
#define MSG_PONG            9

/* ── Signaling header ─────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0x52435347 = "RCSG" */
    uint16_t msg_type;
    uint16_t payload_len;
    uint32_t room_id;
    uint32_t seq;
} sig_hdr_t;

#define SIG_MAGIC 0x52435347

/* ── Client ───────────────────────────────────────────────────────── */
typedef struct {
    int     fd;
    char    room_id[ROOM_ID_LEN];
    bool    active;
} client_t;

/* ── Room ─────────────────────────────────────────────────────────── */
typedef struct {
    char    room_id[ROOM_ID_LEN];
    int     clients[MAX_CLIENTS];
    int     count;
} room_t;

static volatile bool g_running = true;
static client_t g_clients[MAX_CLIENTS];
static room_t g_rooms[MAX_ROOMS];
static int g_room_count = 0;
static int g_epoll_fd;
static int g_active_clients = 0;

static void sig_handler(int sig) { (void)sig; g_running = false; }

static room_t *find_room(const char *room_id)
{
    for (int i = 0; i < g_room_count; i++) {
        if (strcmp(g_rooms[i].room_id, room_id) == 0)
            return &g_rooms[i];
    }
    return NULL;
}

static room_t *get_or_create_room(const char *room_id)
{
    room_t *r = find_room(room_id);
    if (r) return r;
    if (g_room_count >= MAX_ROOMS) return NULL;
    r = &g_rooms[g_room_count++];
    memset(r, 0, sizeof(*r));
    strncpy(r->room_id, room_id, ROOM_ID_LEN - 1);
    return r;
}

static void room_join(room_t *room, int fd)
{
    for (int i = 0; i < room->count; i++) {
        if (room->clients[i] == fd) return;
    }
    if (room->count < MAX_CLIENTS) {
        room->clients[room->count++] = fd;
    }
}

static void room_leave(room_t *room, int fd)
{
    for (int i = 0; i < room->count; i++) {
        if (room->clients[i] == fd) {
            room->clients[i] = room->clients[--room->count];
            return;
        }
    }
}

static void room_broadcast(room_t *room, int exclude_fd,
                            const uint8_t *data, size_t len)
{
    for (int i = 0; i < room->count; i++) {
        if (room->clients[i] != exclude_fd) {
            send(room->clients[i], data, len, MSG_NOSIGNAL);
        }
    }
}

static void handle_client(int fd)
{
    uint8_t buf[MAX_MSG_SIZE];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        /* Disconnect */
        client_t *c = (fd < MAX_CLIENTS) ? &g_clients[fd] : NULL;
        if (c && c->active) {
            room_t *r = find_room(c->room_id);
            if (r) room_leave(r, fd);
            c->active = false;
            g_active_clients--;
        }
        close(fd);
        epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        return;
    }

    if (n < (ssize_t)sizeof(sig_hdr_t)) return;

    sig_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    hdr.msg_type    = ntohs(hdr.msg_type);
    hdr.payload_len = ntohs(hdr.payload_len);
    hdr.room_id     = ntohl(hdr.room_id);

    if (hdr.magic != SIG_MAGIC) return;

    client_t *c = (fd < MAX_CLIENTS) ? &g_clients[fd] : NULL;
    if (!c) return;

    switch (hdr.msg_type) {
    case MSG_JOIN_ROOM: {
        char room_id[ROOM_ID_LEN] = {0};
        size_t rlen = hdr.payload_len < ROOM_ID_LEN - 1 ? hdr.payload_len : ROOM_ID_LEN - 1;
        memcpy(room_id, buf + sizeof(sig_hdr_t), rlen);

        room_t *room = get_or_create_room(room_id);
        if (room) {
            /* Leave old room */
            if (c->room_id[0]) {
                room_t *old = find_room(c->room_id);
                if (old) room_leave(old, fd);
            }
            room_join(room, fd);
            strncpy(c->room_id, room_id, ROOM_ID_LEN - 1);
            c->active = true;
            printf("[SIG] Client fd=%d joined room '%s' (room has %d clients)\n",
                   fd, room_id, room->count);
            fflush(stdout);
        }
        break;
    }
    case MSG_OFFER:
    case MSG_ANSWER:
    case MSG_ICE_CANDIDATE: {
        if (!c->room_id[0]) break;
        room_t *room = find_room(c->room_id);
        if (room) room_broadcast(room, fd, buf, (size_t)n);
        break;
    }
    case MSG_PING: {
        sig_hdr_t pong = { .magic = htonl(SIG_MAGIC), .msg_type = htons(MSG_PONG) };
        send(fd, &pong, sizeof(pong), MSG_NOSIGNAL);
        break;
    }
    }
}

int main(int argc, char *argv[])
{
    uint16_t port = 19500;
    if (argc > 1) port = (uint16_t)atoi(argv[1]);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int lfd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (lfd < 0) lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in6 addr = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(port),
        .sin6_addr = in6addr_any,
    };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        /* Try IPv4 */
        struct sockaddr_in addr4 = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr.s_addr = INADDR_ANY,
        };
        if (bind(lfd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0) {
            perror("bind"); close(lfd); return 1;
        }
    }
    listen(lfd, 64);

    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
    epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, lfd, &ev);

    printf("[SIG] Listening on TCP :%u\n", port);
    fflush(stdout);

    struct epoll_event events[EPOLL_BATCH];
    memset(g_clients, 0, sizeof(g_clients));

    while (g_running) {
        int nfds = epoll_wait(g_epoll_fd, events, EPOLL_BATCH, 2000);
        if (nfds < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            if (fd == lfd) {
                /* Accept */
                struct sockaddr_storage peer;
                socklen_t plen = sizeof(peer);
                int cfd = accept4(lfd, (struct sockaddr *)&peer, &plen,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (cfd < 0) continue;
                if (cfd >= MAX_CLIENTS) { close(cfd); continue; }

                int nodelay = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                struct epoll_event cev = { .events = EPOLLIN, .data.fd = cfd };
                epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, cfd, &cev);

                g_clients[cfd].fd = cfd;
                g_clients[cfd].active = false;
                g_active_clients++;

                printf("[SIG] Client connected fd=%d (active=%d)\n", cfd, g_active_clients);
                fflush(stdout);
            } else {
                handle_client(fd);
            }
        }
    }

    printf("[SIG] Shutting down (peak clients=%d)\n", g_active_clients);
    close(lfd);
    close(g_epoll_fd);
    return 0;
}
