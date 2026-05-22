#include <stdbool.h>
/**
 * @file billing_service.c
 * @brief Billing service for RoboControl
 *
 * HTTP API (port 9800) for subscription management, usage tracking,
 * and invoice generation. Redis-backed with subscription tiers
 * and overage alerts.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

/* ── Constants ──────────────────────────────────────────────────────── */
#define BILLING_PORT        9800
#define HEALTH_PORT         9801
#define MAX_CONNECTIONS     5000
#define EPOLL_BATCH         128
#define MAX_REQ_SIZE        8192

/* ── Subscription tiers ─────────────────────────────────────────────── */
typedef enum {
    TIER_FREE       = 0,
    TIER_PRO        = 1,
    TIER_ENTERPRISE = 2
} tier_t;

typedef struct {
    const char *name;
    uint32_t    max_robots;
    uint64_t    daily_bytes;      /* daily bandwidth limit */
    uint64_t    monthly_price;    /* cents */
} tier_info_t;

static const tier_info_t TIERS[] = {
    [TIER_FREE]       = {"free",       1,    100ULL * 1024 * 1024,          0},
    [TIER_PRO]        = {"pro",        10,   10ULL * 1024 * 1024 * 1024,   2900},
    [TIER_ENTERPRISE] = {"enterprise", 0,    0,                            9900}, /* 0 = unlimited
*/
};

/* ── Usage record ───────────────────────────────────────────────────── */
typedef struct {
    uint64_t bytes_today;
    uint64_t bytes_this_month;
    uint64_t connection_seconds_today;
    uint64_t connection_seconds_month;
    uint64_t last_reset_day;
    uint64_t last_reset_month;
} usage_t;

/* ── Subscription ───────────────────────────────────────────────────── */
typedef struct {
    char        user_id[64];
    tier_t      tier;
    uint64_t    subscribed_at;
    uint64_t    expires_at;
    bool        active;
} subscription_t;

/* ── Server ─────────────────────────────────────────────────────────── */
typedef struct {
    int             epoll_fd;
    int             listen_fd;
    int             health_fd;
    volatile bool   running;
    redisContext    *redis;
} billing_server_t;

static billing_server_t g_server;

/* ── Helpers ────────────────────────────────────────────────────────── */
static uint64_t now_sec(void) { return (uint64_t)time(NULL); }
static uint64_t today_start(void) { return now_sec() - (now_sec() % 86400); }

static void http_reply(int fd, int code, const char *status, const char *body)
{
    char resp[4096];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        code, status, strlen(body), body);
    send(fd, resp, (size_t)len, 0);
}

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
    const char *end = p;
    while (*end && *end != ',' && *end != '}' && *end != '\n') end++;
    size_t len = (size_t)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/* ── Redis operations ───────────────────────────────────────────────── */

static tier_t redis_get_tier(billing_server_t *srv, const char *user_id)
{
    redisReply *r = redisCommand(srv->redis, "HGET billing:sub:%s tier", user_id);
    if (!r || r->type != REDIS_REPLY_STRING) { if (r) freeReplyObject(r); return TIER_FREE; }
    int t = atoi(r->str);
    freeReplyObject(r);
    return (t >= 0 && t <= TIER_ENTERPRISE) ? (tier_t)t : TIER_FREE;
}

static void redis_set_subscription(billing_server_t *srv, const char *user_id,
                                     tier_t tier, uint64_t expires)
{
    redisCommand(srv->redis, "HSET billing:sub:%s tier %d subscribed_at %lu expires_at %lu active 1",
                 user_id, tier, now_sec(), expires);
    redisCommand(srv->redis, "SADD billing:users %s", user_id);
}

static usage_t redis_get_usage(billing_server_t *srv, const char *user_id)
{
    usage_t u = {0};
    redisReply *r = redisCommand(srv->redis, "HGETALL billing:usage:%s", user_id);
    if (!r || r->type != REDIS_REPLY_ARRAY) { if (r) freeReplyObject(r); return u; }
    for (size_t i = 0; i + 1 < r->elements; i += 2) {
        if (strcmp(r->element[i]->str, "bytes_today") == 0)
            u.bytes_today = strtoull(r->element[i + 1]->str, NULL, 10);
        else if (strcmp(r->element[i]->str, "bytes_month") == 0)
            u.bytes_this_month = strtoull(r->element[i + 1]->str, NULL, 10);
        else if (strcmp(r->element[i]->str, "conn_sec_today") == 0)
            u.connection_seconds_today = strtoull(r->element[i + 1]->str, NULL, 10);
    }
    freeReplyObject(r);
    return u;
}

static void redis_add_usage(billing_server_t *srv, const char *user_id,
                              uint64_t bytes, uint64_t conn_sec)
{
    uint64_t today = today_start();
    redisCommand(srv->redis, "HINCRBY billing:usage:%s bytes_today %lu", user_id, bytes);
    redisCommand(srv->redis, "HINCRBY billing:usage:%s bytes_month %lu", user_id, bytes);
    redisCommand(srv->redis, "HINCRBY billing:usage:%s conn_sec_today %lu", user_id, conn_sec);
    redisCommand(srv->redis, "HSET billing:usage:%s last_day %lu", user_id, today);

    /* Check for overage */
    tier_t tier = redis_get_tier(srv, user_id);
    const tier_info_t *info = &TIERS[tier];

    usage_t u = redis_get_usage(srv, user_id);
    if (info->daily_bytes > 0 && u.bytes_today > info->daily_bytes) {
        syslog(LOG_WARNING, "billing: user %s exceeded daily limit (%lu > %lu)",
               user_id, (unsigned long)u.bytes_today, (unsigned long)info->daily_bytes);
        redisCommand(srv->redis, "PUBLISH billing:alerts "
                     "{\"user\":\"%s\",\"type\":\"overage\",\"bytes\":%lu}",
                     user_id, (unsigned long)u.bytes_today);
    }
}

static void redis_reset_daily(billing_server_t *srv, const char *user_id)
{
    redisCommand(srv->redis, "HSET billing:usage:%s bytes_today 0 conn_sec_today 0", user_id);
}

/* ── Request handler ────────────────────────────────────────────────── */

static const char *get_body(const char *req)
{
    const char *p = strstr(req, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

static void handle_request(billing_server_t *srv, int fd, const char *req)
{
    char method[8] = {0}, path[256] = {0};
    sscanf(req, "%7s %255s", method, path);

    if (strcmp(method, "POST") == 0) {
        const char *body = get_body(req);
        if (!body) { http_reply(fd, 400, "Bad Request", "{\"error\":\"empty body\"}\n"); return; }

        if (strcmp(path, "/billing/subscribe") == 0) {
            char user_id[64] = {0}, tier_str[16] = {0};
            json_field(body, "user_id", user_id, sizeof(user_id));
            json_field(body, "tier", tier_str, sizeof(tier_str));
            if (!user_id[0]) { http_reply(fd, 400, "Bad Request", "{\"error\":\"user_id required\"}\n"); return; }

            tier_t tier = TIER_FREE;
            if (strcmp(tier_str, "pro") == 0) tier = TIER_PRO;
            else if (strcmp(tier_str, "enterprise") == 0) tier = TIER_ENTERPRISE;

            uint64_t expires = now_sec() + 30 * 86400; /* 30 days */
            redis_set_subscription(srv, user_id, tier, expires);

            char resp[256];
            snprintf(resp, sizeof(resp),
                "{\"user_id\":\"%s\",\"tier\":\"%s\",\"expires\":%lu}\n",
                user_id, TIERS[tier].name, (unsigned long)expires);
            http_reply(fd, 200, "OK", resp);
            syslog(LOG_INFO, "billing: %s subscribed to %s", user_id, TIERS[tier].name);

        } else if (strcmp(path, "/billing/usage") == 0) {
            char user_id[64] = {0}, bytes_str[32] = {0}, conn_str[32] = {0};
            json_field(body, "user_id", user_id, sizeof(user_id));
            json_field(body, "bytes", bytes_str, sizeof(bytes_str));
            json_field(body, "connection_seconds", conn_str, sizeof(conn_str));
            if (!user_id[0]) { http_reply(fd, 400, "Bad Request", "{\"error\":\"user_id required\"}\n"); return; }

            uint64_t bytes = strtoull(bytes_str, NULL, 10);
            uint64_t conn_sec = strtoull(conn_str, NULL, 10);
            redis_add_usage(srv, user_id, bytes, conn_sec);

            usage_t u = redis_get_usage(srv, user_id);
            tier_t tier = redis_get_tier(srv, user_id);
            char resp[512];
            snprintf(resp, sizeof(resp),
                "{\"user_id\":\"%s\",\"tier\":\"%s\","
                "\"bytes_today\":%lu,\"bytes_month\":%lu,"
                "\"daily_limit\":%lu}\n",
                user_id, TIERS[tier].name,
                (unsigned long)u.bytes_today, (unsigned long)u.bytes_this_month,
                (unsigned long)TIERS[tier].daily_bytes);
            http_reply(fd, 200, "OK", resp);

        } else if (strcmp(path, "/billing/usage/reset") == 0) {
            char user_id[64] = {0};
            json_field(body, "user_id", user_id, sizeof(user_id));
            if (!user_id[0]) { http_reply(fd, 400, "Bad Request", "{\"error\":\"user_id required\"}\n"); return; }
            redis_reset_daily(srv, user_id);
            http_reply(fd, 200, "OK", "{\"ok\":true}\n");

        } else {
            http_reply(fd, 404, "Not Found", "{\"error\":\"not found\"}\n");
        }

    } else if (strcmp(method, "GET") == 0) {
        if (strncmp(path, "/billing/invoice/", 17) == 0) {
            const char *invoice_id = path + 17;
            char resp[512];
            /* Simplified invoice: in production, store invoices in Redis/DB */
            snprintf(resp, sizeof(resp),
                "{\"invoice_id\":\"%s\",\"status\":\"generated\","
                "\"amount_cents\":0,\"currency\":\"USD\"}\n", invoice_id);
            http_reply(fd, 200, "OK", resp);

        } else if (strcmp(path, "/billing/tiers") == 0) {
            char resp[1024];
            snprintf(resp, sizeof(resp),
                "{\"tiers\":["
                "{\"name\":\"free\",\"max_robots\":1,\"daily_bytes\":%lu,\"price_cents\":0},"
                "{\"name\":\"pro\",\"max_robots\":10,\"daily_bytes\":%lu,\"price_cents\":2900},"
                "{\"name\":\"enterprise\",\"max_robots\":0,\"daily_bytes\":0,\"price_cents\":9900}"
                "]}\n",
                (unsigned long)TIERS[0].daily_bytes,
                (unsigned long)TIERS[1].daily_bytes);
            http_reply(fd, 200, "OK", resp);

        } else if (strcmp(path, "/health") == 0) {
            http_reply(fd, 200, "OK", "{\"status\":\"ok\"}\n");
        } else {
            http_reply(fd, 404, "Not Found", "{\"error\":\"not found\"}\n");
        }
    } else {
        http_reply(fd, 405, "Method Not Allowed", "{\"error\":\"method not allowed\"}\n");
    }
}

/* ── Signal handler ─────────────────────────────────────────────────── */
static void signal_handler(int sig) { (void)sig; g_server.running = false; }

/* ── Main ───────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    openlog("robocontrol-billing", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    billing_server_t *srv = &g_server;
    memset(srv, 0, sizeof(*srv));
    srv->running = true;

    struct sigaction sa = { .sa_handler = signal_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    srv->redis = redisConnect("127.0.0.1", 6379);
    if (!srv->redis || srv->redis->err) {
        syslog(LOG_WARNING, "billing: Redis unavailable");
        if (srv->redis) { redisFree(srv->redis); srv->redis = NULL; }
    }

    srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    int opt = 1;
    srv->listen_fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv->listen_fd < 0) srv->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in6 addr = { .sin6_family = AF_INET6, .sin6_port = htons(BILLING_PORT), .sin6_addr = in6addr_any };
    bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv->listen_fd, 512);

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
    struct sockaddr_storage peer;
    socklen_t plen;
    char buf[MAX_REQ_SIZE];

    syslog(LOG_INFO, "billing: listening on port %d", BILLING_PORT);

    while (srv->running) {
        int nfds = epoll_wait(srv->epoll_fd, events, EPOLL_BATCH, 2000);
        if (nfds < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == srv->listen_fd) {
                for (;;) {
                    plen = sizeof(peer);
                    int cfd = accept4(srv->listen_fd, (struct sockaddr *)&peer, &plen, SOCK_CLOEXEC);
                    if (cfd < 0) break;

                    ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
                    if (n > 0) {
                        buf[n] = '\0';
                        handle_request(srv, cfd, buf);
                    }
                    close(cfd);
                }
            } else if (fd == srv->health_fd) {
                plen = sizeof(peer);
                int cfd = accept4(srv->health_fd, (struct sockaddr *)&peer, &plen, SOCK_CLOEXEC);
                if (cfd >= 0) {
                    http_reply(cfd, 200, "OK", "{\"status\":\"ok\"}\n");
                    close(cfd);
                }
            }
        }
    }

    syslog(LOG_INFO, "billing: shutting down");
    if (srv->redis) redisFree(srv->redis);
    closelog();
    return 0;
}
