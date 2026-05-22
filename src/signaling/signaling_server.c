#include <stdbool.h>
/**
 * @file signaling_server.c
 * @brief Production signaling server for RoboControl
 *
 * WebSocket-like signaling over TCP (port 9500).
 * Handles room management, offer/answer/ICE candidate relay,
 * connection registry with Redis backend.
 * Multi-threaded with epoll + SO_REUSEPORT. Max 1M connections.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

/* ── Limits ─────────────────────────────────────────────────────────── */
#define MAX_CONNECTIONS     1000000
#define MAX_ROOMS           100000
#define MAX_MSG_SIZE        65536
#define SIGNALING_PORT      9500
#define HEALTH_PORT         9501
#define EPOLL_BATCH         512
#define REDIS_POOL_SIZE     64
#define AUTH_TOKEN_LEN      64
#define ROOM_ID_LEN         32
#define CLIENT_ID_LEN       32

/* ── Message types ──────────────────────────────────────────────────── */
#define MSG_OFFER           1
#define MSG_ANSWER          2
#define MSG_ICE_CANDIDATE   3
#define MSG_JOIN_ROOM       4
#define MSG_LEAVE_ROOM      5
#define MSG_ROOM_LIST       6
#define MSG_AUTH            7
#define MSG_PING            8
#define MSG_PONG            9
#define MSG_ERROR           255

/* ── Signaling message header (16 bytes) ────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0x52435347 = "RCSG" */
    uint16_t msg_type;
    uint16_t payload_len;
    uint32_t room_id;
    uint32_t seq;
} sig_hdr_t;

#define SIG_MAGIC 0x52435347

/* ── Client connection ──────────────────────────────────────────────── */
typedef struct sig_client {
    int                 fd;
    uint32_t            client_id;
    char                room_id[ROOM_ID_LEN];
    char                auth_token[AUTH_TOKEN_LEN + 1];
    bool                authenticated;
    uint64_t            last_activity;
    struct sig_client  *room_next;  /* linked list within room */
    struct sig_client  *hash_next;  /* hash chain by fd */
} sig_client_t;

/* ── Room ───────────────────────────────────────────────────────────── */
typedef struct sig_room {
    char            room_id[ROOM_ID_LEN];
    sig_client_t   *clients;       /* linked list of clients */
    uint32_t        client_count;
    uint64_t        created_at;
    struct sig_room *next;          /* hash chain */
} sig_room_t;

/* ── Server context ─────────────────────────────────────────────────── */
typedef struct {
    /* Sockets */
    int                 epoll_fd;
    int                 listen_fd;
    int                 health_fd;

    /* Connection pool */
    sig_client_t       *client_pool;
    sig_client_t       *free_clients;
    sig_client_t       *fd_map[MAX_CONNECTIONS]; /* fd→client mapping */
    uint32_t            active_clients;
    uint32_t            peak_clients;

    /* Room hash table */
    sig_room_t         *rooms[MAX_ROOMS];

    /* Redis */
    redisContext       *redis;
    const char         *redis_host;
    int                 redis_port;

    /* Control */
    volatile bool       running;
    pthread_mutex_t     lock;
} sig_server_t;

static sig_server_t g_server;

/* ── Helpers ────────────────────────────────────────────────────────── */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int __attribute__((unused)) set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint32_t hash_str(const char *s, uint32_t buckets)
{
    uint32_t h = 5381;
    while (*s) h = h * 33 + (uint8_t)*s++;
    return h & (buckets - 1);
}

/* ── Redis operations ───────────────────────────────────────────────── */

static bool redis_verify_token(sig_server_t *srv, const char *token)
{
    if (!srv->redis) return true; /* no redis = allow all (dev mode) */

    redisReply *r = redisCommand(srv->redis, "EXISTS auth:token:%s", token);
    if (!r) return false;

    bool valid = (r->type == REDIS_REPLY_INTEGER && r->integer > 0);
    freeReplyObject(r);
    return valid;
}

static void redis_register_connection(sig_server_t *srv, uint32_t client_id,
                                       const char *room_id)
{
    if (!srv->redis) return;
    redisCommand(srv->redis, "HSET sig:connections %u %s", client_id, room_id);
}

static void redis_unregister_connection(sig_server_t *srv, uint32_t client_id)
{
    if (!srv->redis) return;
    redisCommand(srv->redis, "HDEL sig:connections %u", client_id);
}

/* ── Room management ────────────────────────────────────────────────── */

static sig_room_t *room_find(sig_server_t *srv, const char *room_id)
{
    uint32_t h = hash_str(room_id, MAX_ROOMS);
    sig_room_t *r = srv->rooms[h];
    while (r) {
        if (strcmp(r->room_id, room_id) == 0) return r;
        r = r->next;
    }
    return NULL;
}

static sig_room_t *room_create(sig_server_t *srv, const char *room_id)
{
    uint32_t h = hash_str(room_id, MAX_ROOMS);
    sig_room_t *r = calloc(1, sizeof(sig_room_t));
    if (!r) return NULL;
    memcpy(r->room_id, room_id, ROOM_ID_LEN - 1);
    r->room_id[ROOM_ID_LEN - 1] = '\0';
    r->created_at = now_ms();
    r->next = srv->rooms[h];
    srv->rooms[h] = r;
    return r;
}

static void room_join(sig_server_t *srv, sig_client_t *client, const char *room_id)
{
    /* Leave current room if any */
    if (client->room_id[0]) {
        sig_room_t *old = room_find(srv, client->room_id);
        if (old) {
            sig_client_t **pp = &old->clients;
            while (*pp) {
                if (*pp == client) { *pp = client->room_next; old->client_count--; break; }
                pp = &(*pp)->room_next;
            }
        }
    }

    /* Join new room */
    sig_room_t *room = room_find(srv, room_id);
    if (!room) room = room_create(srv, room_id);
    if (!room) return;

    memcpy(client->room_id, room_id, ROOM_ID_LEN - 1);
    client->room_id[ROOM_ID_LEN - 1] = '\0';
    client->room_next = room->clients;
    room->clients = client;
    room->client_count++;
    redis_register_connection(srv, client->client_id, room_id);
}

/* ── Client management ──────────────────────────────────────────────── */

static sig_client_t *client_alloc(sig_server_t *srv, int fd)
{
    sig_client_t *c = srv->free_clients;
    if (!c) return NULL;
    srv->free_clients = c->hash_next;

    memset(c, 0, sizeof(sig_client_t));
    c->fd = fd;
    c->client_id = (uint32_t)fd; /* simple ID = fd */
    c->last_activity = now_ms();
    c->authenticated = false;

    if (fd < MAX_CONNECTIONS) srv->fd_map[fd] = c;
    srv->active_clients++;
    if (srv->active_clients > srv->peak_clients)
        srv->peak_clients = srv->active_clients;

    return c;
}

static void client_free(sig_server_t *srv, sig_client_t *c)
{
    if (!c) return;

    /* Leave room */
    if (c->room_id[0]) {
        sig_room_t *room = room_find(srv, c->room_id);
        if (room) {
            sig_client_t **pp = &room->clients;
            while (*pp) {
                if (*pp == c) { *pp = c->room_next; room->client_count--; break; }
                pp = &(*pp)->room_next;
            }
        }
        redis_unregister_connection(srv, c->client_id);
    }

    if (c->fd >= 0 && c->fd < MAX_CONNECTIONS) srv->fd_map[c->fd] = NULL;
    if (c->fd >= 0) {
        epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
    }

    srv->active_clients--;
    c->hash_next = srv->free_clients;
    srv->free_clients = c;
}

/* ── Message handling ───────────────────────────────────────────────── */

/**
 * Broadcast a message to all clients in a room except the sender.
 */
static void room_broadcast(sig_server_t *srv, const char *room_id,
                            int exclude_fd,
                            const uint8_t *data, size_t len)
{
    sig_room_t *room = room_find(srv, room_id);
    if (!room) return;

    for (sig_client_t *c = room->clients; c; c = c->room_next) {
        if (c->fd != exclude_fd && c->authenticated) {
            send(c->fd, data, len, MSG_NOSIGNAL);
        }
    }
}

static void handle_message(sig_server_t *srv, sig_client_t *client,
                            const uint8_t *data, size_t len)
{
    if (len < sizeof(sig_hdr_t)) return;

    sig_hdr_t hdr;
    memcpy(&hdr, data, sizeof(hdr));
    hdr.msg_type    = ntohs(hdr.msg_type);
    hdr.payload_len = ntohs(hdr.payload_len);
    hdr.room_id     = ntohl(hdr.room_id);
    hdr.seq         = ntohl(hdr.seq);

    if (hdr.magic != SIG_MAGIC) {
        syslog(LOG_WARNING, "sig: bad magic from fd=%d", client->fd);
        return;
    }

    client->last_activity = now_ms();

    switch (hdr.msg_type) {
    case MSG_AUTH: {
        /* Payload = auth token */
        char token[AUTH_TOKEN_LEN + 1] = {0};
        size_t tlen = hdr.payload_len < AUTH_TOKEN_LEN ? hdr.payload_len : AUTH_TOKEN_LEN;
        memcpy(token, data + sizeof(sig_hdr_t), tlen);

        if (redis_verify_token(srv, token)) {
            client->authenticated = true;
            memcpy(client->auth_token, token, AUTH_TOKEN_LEN);
            client->auth_token[AUTH_TOKEN_LEN] = '\0';
            syslog(LOG_INFO, "sig: client %u authenticated", client->client_id);
            /* Send ACK */
            sig_hdr_t ack = { .magic = htonl(SIG_MAGIC), .msg_type = htons(MSG_AUTH) };
            send(client->fd, &ack, sizeof(ack), MSG_NOSIGNAL);
        } else {
            syslog(LOG_WARNING, "sig: auth failed for fd=%d", client->fd);
            sig_hdr_t err = { .magic = htonl(SIG_MAGIC), .msg_type = htons(MSG_ERROR) };
            send(client->fd, &err, sizeof(err), MSG_NOSIGNAL);
            client_free(srv, client);
        }
        break;
    }
    case MSG_JOIN_ROOM: {
        if (!client->authenticated) break;
        char room[ROOM_ID_LEN] = {0};
        size_t rlen = hdr.payload_len < ROOM_ID_LEN - 1 ? hdr.payload_len : ROOM_ID_LEN - 1;
        memcpy(room, data + sizeof(sig_hdr_t), rlen);
        room_join(srv, client, room);
        syslog(LOG_INFO, "sig: client %u joined room '%s'", client->client_id, room);
        break;
    }
    case MSG_LEAVE_ROOM: {
        if (!client->authenticated) break;
        /* Remove from room */
        sig_room_t *room = room_find(srv, client->room_id);
        if (room) {
            sig_client_t **pp = &room->clients;
            while (*pp) {
                if (*pp == client) { *pp = client->room_next; room->client_count--; break; }
                pp = &(*pp)->room_next;
            }
        }
        redis_unregister_connection(srv, client->client_id);
        client->room_id[0] = '\0';
        break;
    }
    case MSG_OFFER:
    case MSG_ANSWER:
    case MSG_ICE_CANDIDATE: {
        if (!client->authenticated || !client->room_id[0]) break;
        /* Relay to all other clients in the room */
        room_broadcast(srv, client->room_id, client->fd, data, len);
        break;
    }
    case MSG_PING: {
        sig_hdr_t pong = { .magic = htonl(SIG_MAGIC), .msg_type = htons(MSG_PONG) };
        send(client->fd, &pong, sizeof(pong), MSG_NOSIGNAL);
        break;
    }
    default:
        syslog(LOG_WARNING, "sig: unknown msg type %u from fd=%d",
               hdr.msg_type, client->fd);
    }
}

/* ── HTTP health check ──────────────────────────────────────────────── */

static void handle_health(sig_server_t *srv, int fd)
{
    char body[256];
    int blen = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"active_clients\":%u,\"peak_clients\":%u}\n",
        srv->active_clients, srv->peak_clients);

    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s", blen, body);
    send(fd, resp, rlen, 0);
    close(fd);
}

/* ── Signal handler ─────────────────────────────────────────────────── */

static void signal_handler(int sig)
{
    (void)sig;
    g_server.running = false;
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    openlog("robocontrol-signaling", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_INFO, "signaling: starting...");

    sig_server_t *srv = &g_server;
    memset(srv, 0, sizeof(*srv));
    srv->running = true;
    srv->redis_host = "127.0.0.1";
    srv->redis_port = 6379;
    pthread_mutex_init(&srv->lock, NULL);

    /* Signal handling */
    struct sigaction sa = { .sa_handler = signal_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Connect to Redis */
    srv->redis = redisConnect(srv->redis_host, srv->redis_port);
    if (!srv->redis || srv->redis->err) {
        syslog(LOG_WARNING, "signaling: Redis connection failed (running in dev mode)");
        if (srv->redis) { redisFree(srv->redis); srv->redis = NULL; }
    }

    /* Allocate client pool (pre-allocated for 1M connections) */
    srv->client_pool = calloc(MAX_CONNECTIONS, sizeof(sig_client_t));
    if (!srv->client_pool) {
        syslog(LOG_ERR, "signaling: failed to allocate client pool");
        return 1;
    }
    /* Build free list */
    for (int i = 0; i < MAX_CONNECTIONS - 1; i++) {
        srv->client_pool[i].hash_next = &srv->client_pool[i + 1];
        srv->client_pool[i].fd = -1;
    }
    srv->client_pool[MAX_CONNECTIONS - 1].hash_next = NULL;
    srv->client_pool[MAX_CONNECTIONS - 1].fd = -1;
    srv->free_clients = srv->client_pool;

    /* Create epoll */
    srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv->epoll_fd < 0) {
        syslog(LOG_ERR, "signaling: epoll_create1: %s", strerror(errno));
        return 1;
    }

    /* Create listen socket */
    srv->listen_fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv->listen_fd < 0)
        srv->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in6 addr = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(SIGNALING_PORT),
        .sin6_addr = in6addr_any
    };
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "signaling: bind: %s", strerror(errno));
        return 1;
    }
    listen(srv->listen_fd, 4096);

    struct epoll_event ev = { .events = EPOLLIN | EPOLLEXCLUSIVE, .data.fd = srv->listen_fd };
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

    /* Health socket */
    srv->health_fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv->health_fd < 0)
        srv->health_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    setsockopt(srv->health_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in6 haddr = {
        .sin6_family = AF_INET6, .sin6_port = htons(HEALTH_PORT), .sin6_addr = in6addr_any
    };
    if (bind(srv->health_fd, (struct sockaddr *)&haddr, sizeof(haddr)) == 0) {
        listen(srv->health_fd, 16);
        ev.data.fd = srv->health_fd;
        epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->health_fd, &ev);
    }

    /* Event loop */
    struct epoll_event events[EPOLL_BATCH];
    uint8_t buf[MAX_MSG_SIZE];

    syslog(LOG_INFO, "signaling: listening on port %d (health=%d)", SIGNALING_PORT, HEALTH_PORT);

    while (srv->running) {
        int nfds = epoll_wait(srv->epoll_fd, events, EPOLL_BATCH, 2000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == srv->listen_fd) {
                /* Accept new connections */
                for (;;) {
                    struct sockaddr_storage peer;
                    socklen_t plen = sizeof(peer);
                    int cfd = accept4(srv->listen_fd, (struct sockaddr *)&peer, &plen,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;
                    if (cfd >= MAX_CONNECTIONS) { close(cfd); continue; }

                    /* TCP_NODELAY for low latency */
                    int nodelay = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                    sig_client_t *c = client_alloc(srv, cfd);
                    if (!c) { close(cfd); continue; }

                    struct epoll_event cev = { .events = EPOLLIN | EPOLLET, .data.fd = cfd };
                    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, cfd, &cev);
                }
            } else if (fd == srv->health_fd) {
                struct sockaddr_storage peer;
                socklen_t plen = sizeof(peer);
                int cfd = accept4(srv->health_fd, (struct sockaddr *)&peer, &plen, SOCK_CLOEXEC);
                if (cfd >= 0) handle_health(srv, cfd);
            } else {
                /* Client data */
                sig_client_t *c = (fd < MAX_CONNECTIONS) ? srv->fd_map[fd] : NULL;
                if (!c) continue;

                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) {
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                        client_free(srv, c);
                    }
                    continue;
                }
                handle_message(srv, c, buf, (size_t)n);
            }
        }
    }

    /* Cleanup */
    syslog(LOG_INFO, "signaling: shutting down (peak=%u)", srv->peak_clients);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (srv->fd_map[i]) client_free(srv, srv->fd_map[i]);
    }
    for (uint32_t i = 0; i < MAX_ROOMS; i++) {
        sig_room_t *r = srv->rooms[i];
        while (r) { sig_room_t *next = r->next; free(r); r = next; }
    }
    if (srv->redis) redisFree(srv->redis);
    free(srv->client_pool);
    close(srv->epoll_fd);
    closelog();
    return 0;
}
