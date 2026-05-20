#include <stdbool.h>
/**
 * @file auth_service.c
 * @brief Authentication service for RoboControl
 *
 * HTTP API (port 9700) for token management, device registration,
 * and service-to-service authentication. Redis-backed with
 * rate limiting and token blacklist support.
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
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <hiredis/hiredis.h>

/* ── Constants ──────────────────────────────────────────────────────── */
#define AUTH_PORT           9700
#define HEALTH_PORT         9701
#define MAX_CONNECTIONS     10000
#define EPOLL_BATCH         256
#define MAX_REQ_SIZE        8192
#define TOKEN_LEN           64
#define SECRET_KEY          "robocontrol-secret-key-change-in-production"
#define TOKEN_TTL           86400      /* 24 hours */
#define REFRESH_TTL         604800     /* 7 days */
#define RATE_LIMIT_WINDOW   60         /* seconds */
#define RATE_LIMIT_MAX      100        /* requests per window */

/* ── Rate limiter (token bucket) ────────────────────────────────────── */
typedef struct {
    uint32_t tokens;
    uint32_t max_tokens;
    uint32_t refill_rate;   /* tokens per second */
    uint64_t last_refill;
} rate_limit_t;

/* ── Connection ─────────────────────────────────────────────────────── */
typedef struct {
    int         fd;
    char        req_buf[MAX_REQ_SIZE];
    size_t      req_len;
    uint64_t    last_activity;
    rate_limit_t rate;
} auth_conn_t;

/* ── Server ─────────────────────────────────────────────────────────── */
typedef struct {
    int             epoll_fd;
    int             listen_fd;
    int             health_fd;
    auth_conn_t    *conns[MAX_CONNECTIONS];
    uint32_t        active_conns;
    volatile bool   running;
    redisContext    *redis;
    pthread_mutex_t lock;
} auth_server_t;

static auth_server_t g_server;

/* ── Helpers ────────────────────────────────────────────────────────── */

static uint64_t now_sec(void) { return (uint64_t)time(NULL); }
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void random_hex(char *out, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t buf[64];
    if (len > 64) len = 64;
    RAND_bytes(buf, (int)len);
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[buf[i] >> 4];
        out[i * 2 + 1] = hex[buf[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static void hmac_sha256(const char *key, const char *data, char *out, size_t out_len)
{
    unsigned int md_len = 0;
    unsigned char md[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(), key, (int)strlen(key),
         (const unsigned char *)data, strlen(data), md, &md_len);
    /* Hex encode */
    size_t i;
    for (i = 0; i < md_len && i * 2 < out_len - 1; i++) {
        snprintf(out + i * 2, 3, "%02x", md[i]);
    }
    out[i * 2] = '\0';
}

/* ── Rate limiting ──────────────────────────────────────────────────── */

static bool rate_check(rate_limit_t *rl)
{
    uint64_t now = now_sec();
    uint64_t elapsed = now - rl->last_refill;
    if (elapsed > 0) {
        rl->tokens += (uint32_t)(elapsed * rl->refill_rate);
        if (rl->tokens > rl->max_tokens) rl->tokens = rl->max_tokens;
        rl->last_refill = now;
    }
    if (rl->tokens > 0) { rl->tokens--; return true; }
    return false;
}

/* ── HTTP response helpers ──────────────────────────────────────────── */

static void http_reply(int fd, int code, const char *status, const char *body)
{
    char resp[4096];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n%s",
        code, status, strlen(body), body);
    send(fd, resp, (size_t)len, 0);
}

static void http_ok(int fd, const char *json) { http_reply(fd, 200, "OK", json); }
static void http_err(int fd, int code, const char *msg) {
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}\n", msg);
    http_reply(fd, code, "Error", body);
}

/* ── Redis operations ───────────────────────────────────────────────── */

static char *redis_issue_token(auth_server_t *srv, const char *device_id,
                                const char *role, char *token_out)
{
    char nonce[33];
    random_hex(nonce, 16);
    char payload[512];
    snprintf(payload, sizeof(payload), "%s:%s:%lu:%s", device_id, role, now_sec(), nonce);
    hmac_sha256(SECRET_KEY, payload, token_out, TOKEN_LEN + 1);

    /* Store in Redis */
    redisCommand(srv->redis, "SETEX auth:token:%s %d %s",
                 token_out, TOKEN_TTL, device_id);
    redisCommand(srv->redis, "HSET auth:device:%s token %s role %s",
                 device_id, token_out, role);

    return token_out;
}

static bool redis_verify_token(auth_server_t *srv, const char *token)
{
    redisReply *r = redisCommand(srv->redis, "EXISTS auth:token:%s", token);
    if (!r) return false;
    bool ok = (r->type == REDIS_REPLY_INTEGER && r->integer > 0);
    freeReplyObject(r);
    return ok;
}

static void redis_blacklist_token(auth_server_t *srv, const char *token)
{
    redisCommand(srv->redis, "DEL auth:token:%s", token);
    redisCommand(srv->redis, "SADD auth:blacklist %s", token);
}

static bool redis_register_device(auth_server_t *srv, const char *device_id,
                                    const char *name, const char *type)
{
    redisReply *r = redisCommand(srv->redis,
        "HSET auth:devices %s:name %s %s:type %s %s:registered %lu",
        device_id, name, device_id, type, device_id, now_sec());
    return (r != NULL);
}

/* ── Request routing ────────────────────────────────────────────────── */

static const char *get_header(const char *req, const char *name, char *out, size_t out_len)
{
    char search[128];
    snprintf(search, sizeof(search), "\r\n%s: ", name);
    const char *p = strstr(req, search);
    if (!p) { out[0] = '\0'; return NULL; }
    p += strlen(search);
    const char *end = strstr(p, "\r\n");
    if (!end) end = p + strlen(p);
    size_t len = (size_t)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

static const char *get_body(const char *req)
{
    const char *p = strstr(req, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/**
 * Extract JSON field value from a simple JSON body.
 * Only handles flat JSON: {"key":"value",...}
 */
static bool json_field(const char *json, const char *key, char *out, size_t out_len)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p = strchr(p + strlen(search), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        const char *end = strchr(p, '"');
        if (!end) return false;
        size_t len = (size_t)(end - p);
        if (len >= out_len) len = out_len - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        return true;
    }
    /* Number or bare value */
    const char *end = p;
    while (*end && *end != ',' && *end != '}' && *end != '\n') end++;
    size_t len = (size_t)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static void handle_request(auth_server_t *srv, int fd, const char *req, size_t len)
{
    (void)len;

    /* Parse method + path */
    char method[8] = {0}, path[256] = {0};
    sscanf(req, "%7s %255s", method, path);

    /* Rate limiting */
    auth_conn_t *c = (fd < MAX_CONNECTIONS) ? srv->conns[fd] : NULL;
    if (c && !rate_check(&c->rate)) {
        http_err(fd, 429, "rate limit exceeded");
        return;
    }

    /* API key for service-to-service */
    char api_key[256] = {0};
    get_header(req, "X-API-Key", api_key, sizeof(api_key));

    /* Route */
    if (strcmp(method, "POST") == 0) {
        const char *body = get_body(req);
        if (!body) { http_err(fd, 400, "empty body"); return; }

        if (strcmp(path, "/auth/token") == 0) {
            char device_id[128] = {0}, role[32] = {0};
            json_field(body, "device_id", device_id, sizeof(device_id));
            json_field(body, "role", role, sizeof(role));
            if (!device_id[0]) { http_err(fd, 400, "device_id required"); return; }
            if (!role[0]) strcpy(role, "robot");

            char token[TOKEN_LEN + 1];
            redis_issue_token(srv, device_id, role, token);
            char resp[256];
            snprintf(resp, sizeof(resp),
                "{\"token\":\"%s\",\"expires_in\":%d}\n", token, TOKEN_TTL);
            http_ok(fd, resp);
            syslog(LOG_INFO, "auth: token issued for device=%s role=%s", device_id, role);

        } else if (strcmp(path, "/auth/verify") == 0) {
            char token[256] = {0};
            json_field(body, "token", token, sizeof(token));
            if (!token[0]) { http_err(fd, 400, "token required"); return; }
            bool valid = redis_verify_token(srv, token);
            char resp[64];
            snprintf(resp, sizeof(resp), "{\"valid\":%s}\n", valid ? "true" : "false");
            http_ok(fd, resp);

        } else if (strcmp(path, "/auth/refresh") == 0) {
            char token[256] = {0}, device_id[128] = {0}, role[32] = {0};
            json_field(body, "token", token, sizeof(token));
            json_field(body, "device_id", device_id, sizeof(device_id));
            json_field(body, "role", role, sizeof(role));
            if (!token[0] || !device_id[0]) { http_err(fd, 400, "token+device_id required"); return; }

            /* Blacklist old, issue new */
            redis_blacklist_token(srv, token);
            if (!role[0]) strcpy(role, "robot");
            char new_token[TOKEN_LEN + 1];
            redis_issue_token(srv, device_id, role, new_token);
            char resp[256];
            snprintf(resp, sizeof(resp),
                "{\"token\":\"%s\",\"expires_in\":%d}\n", new_token, TOKEN_TTL);
            http_ok(fd, resp);

        } else if (strcmp(path, "/auth/register") == 0) {
            char device_id[128] = {0}, name[128] = {0}, type[64] = {0};
            json_field(body, "device_id", device_id, sizeof(device_id));
            json_field(body, "name", name, sizeof(name));
            json_field(body, "type", type, sizeof(type));
            if (!device_id[0]) { http_err(fd, 400, "device_id required"); return; }

            redis_register_device(srv, device_id, name[0] ? name : device_id,
                                   type[0] ? type : "robot");
            /* Issue initial token */
            char token[TOKEN_LEN + 1];
            redis_issue_token(srv, device_id, "robot", token);
            char resp[256];
            snprintf(resp, sizeof(resp),
                "{\"device_id\":\"%s\",\"token\":\"%s\"}\n", device_id, token);
            http_ok(fd, resp);
            syslog(LOG_INFO, "auth: device registered: %s", device_id);

        } else {
            http_err(fd, 404, "not found");
        }

    } else if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/health") == 0) {
            char resp[128];
            snprintf(resp, sizeof(resp),
                "{\"status\":\"ok\",\"connections\":%u}\n", srv->active_conns);
            http_ok(fd, resp);
        } else {
            http_err(fd, 404, "not found");
        }
    } else {
        http_err(fd, 405, "method not allowed");
    }
}

/* ── Signal handler ─────────────────────────────────────────────────── */
static void signal_handler(int sig) { (void)sig; g_server.running = false; }

/* ── Main ───────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    openlog("robocontrol-auth", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    auth_server_t *srv = &g_server;
    memset(srv, 0, sizeof(*srv));
    srv->running = true;
    pthread_mutex_init(&srv->lock, NULL);

    struct sigaction sa = { .sa_handler = signal_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    srv->redis = redisConnect("127.0.0.1", 6379);
    if (!srv->redis || srv->redis->err) {
        syslog(LOG_WARNING, "auth: Redis unavailable");
        if (srv->redis) { redisFree(srv->redis); srv->redis = NULL; }
    }

    srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    int opt = 1;
    srv->listen_fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv->listen_fd < 0) srv->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    struct sockaddr_in6 addr = { .sin6_family = AF_INET6, .sin6_port = htons(AUTH_PORT), .sin6_addr = in6addr_any };
    bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv->listen_fd, 1024);

    struct epoll_event ev = { .events = EPOLLIN | EPOLLEXCLUSIVE, .data.fd = srv->listen_fd };
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

    srv->health_fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv->health_fd < 0) srv->health_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in6 haddr = { .sin6_family = AF_INET6, .sin6_port = htons(HEALTH_PORT), .sin6_addr = in6addr_any };
    if (bind(srv->health_fd, (struct sockaddr *)&haddr, sizeof(haddr)) == 0) {
        listen(srv->health_fd, 16);
        ev.events = EPOLLIN; ev.data.fd = srv->health_fd;
        epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->health_fd, &ev);
    }

    struct epoll_event events[EPOLL_BATCH];
    syslog(LOG_INFO, "auth: listening on port %d", AUTH_PORT);

    while (srv->running) {
        int nfds = epoll_wait(srv->epoll_fd, events, EPOLL_BATCH, 2000);
        if (nfds < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == srv->listen_fd) {
                for (;;) {
                    struct sockaddr_storage peer;
                    socklen_t plen = sizeof(peer);
                    int cfd = accept4(srv->listen_fd, (struct sockaddr *)&peer, &plen,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;
                    if (cfd >= MAX_CONNECTIONS) { close(cfd); continue; }

                    auth_conn_t *c = calloc(1, sizeof(auth_conn_t));
                    if (!c) { close(cfd); continue; }
                    c->fd = cfd;
                    c->last_activity = now_ms();
                    c->rate.max_tokens = RATE_LIMIT_MAX;
                    c->rate.refill_rate = RATE_LIMIT_MAX / RATE_LIMIT_WINDOW;
                    c->rate.tokens = RATE_LIMIT_MAX;
                    c->rate.last_refill = now_sec();
                    srv->conns[cfd] = c;
                    srv->active_conns++;

                    ev.events = EPOLLIN | EPOLLET; ev.data.fd = cfd;
                    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, cfd, &ev);
                }
            } else if (fd == srv->health_fd) {
                struct sockaddr_storage peer;
                socklen_t plen = sizeof(peer);
                int cfd = accept4(srv->health_fd, (struct sockaddr *)&peer, &plen, SOCK_CLOEXEC);
                if (cfd >= 0) {
                    char body[128];
                    int bl = snprintf(body, sizeof(body), "{\"status\":\"ok\"}\n");
                    char resp[256];
                    int rl = snprintf(resp, sizeof(resp),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: %d\r\nConnection: close\r\n\r\n%s", bl, body);
                    send(cfd, resp, (size_t)rl, 0);
                    close(cfd);
                }
            } else {
                auth_conn_t *c = (fd < MAX_CONNECTIONS) ? srv->conns[fd] : NULL;
                if (!c) continue;

                ssize_t n = recv(fd, c->req_buf + c->req_len,
                                  MAX_REQ_SIZE - c->req_len - 1, 0);
                if (n <= 0) {
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                        srv->conns[fd] = NULL;
                        srv->active_conns--;
                        epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        free(c);
                    }
                    continue;
                }
                c->req_len += (size_t)n;
                c->req_buf[c->req_len] = '\0';

                /* Check for complete HTTP request */
                if (strstr(c->req_buf, "\r\n\r\n")) {
                    handle_request(srv, fd, c->req_buf, c->req_len);
                    srv->conns[fd] = NULL;
                    srv->active_conns--;
                    epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    free(c);
                }
            }
        }
    }

    syslog(LOG_INFO, "auth: shutting down");
    if (srv->redis) redisFree(srv->redis);
    closelog();
    return 0;
}
