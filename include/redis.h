/*
 * robocontrol/redis.h - Lightweight Redis client (RESP protocol over TCP)
 * Supports: GET, SET, DEL, PUBLISH, SUBSCRIBE, LPUSH, RPUSH, LRANGE, EXPIRE, PING
 * Connection pooling with configurable pool size.
 */
#ifndef ROBOCONTROL_REDIS_H
#define ROBOCONTROL_REDIS_H

#include "common.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RC_REDIS_MAX_CMD_LEN   4096
#define RC_REDIS_MAX_RESP_LEN  (1 << 20)  /* 1 MB max response */
#define RC_REDIS_MAX_ARGS      32

typedef enum {
    RC_REDIS_REPLY_STRING = 0,
    RC_REDIS_REPLY_INTEGER,
    RC_REDIS_REPLY_ARRAY,
    RC_REDIS_REPLY_NIL,
    RC_REDIS_REPLY_ERROR,
    RC_REDIS_REPLY_STATUS,
} rc_redis_reply_type_t;

typedef struct {
    rc_redis_reply_type_t type;
    int64_t integer;
    char   *str;
    size_t  str_len;
    /* For arrays */
    void  **elements;
    int     element_count;
} rc_redis_reply_t;

typedef struct rc_redis_conn {
    int    fd;
    int    in_use;
    int64_t last_used;
    struct rc_redis_conn *next;
} rc_redis_conn_t;

typedef struct {
    char    host[256];
    int     port;
    int     pool_size;
    int     timeout_ms;
    rc_redis_conn_t *free_list;
    rc_spinlock_t    lock;
    rc_atomic64_t    total_commands;
    rc_atomic64_t    total_errors;
} rc_redis_pool_t;

/* ── Connection helpers ─────────────────────────────────────── */
static inline int rc_redis_connect_host(const char *host, int port, int timeout_ms) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    rc_set_nonblock(fd);
    rc_set_tcp_nodelay(fd);

    int ret = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (ret < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        struct timeval tv = { .tv_sec = timeout_ms / 1000,
                              .tv_usec = (timeout_ms % 1000) * 1000 };
        FD_ZERO(&wfds); FD_SET(fd, &wfds);
        ret = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (ret <= 0) { close(fd); return -1; }

        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err) { close(fd); return -1; }
    } else if (ret < 0) {
        close(fd);
        return -1;
    }

    /* Set blocking for command/response */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct timeval tv = { .tv_sec = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    return fd;
}

static inline void rc_redis_reply_free(rc_redis_reply_t *reply) {
    if (!reply) return;
    free(reply->str);
    if (reply->elements) {
        for (int i = 0; i < reply->element_count; i++) {
            rc_redis_reply_free((rc_redis_reply_t *)reply->elements[i]);
        }
        free(reply->elements);
    }
}

/* ── RESP reader helpers ────────────────────────────────────── */
static inline int rc_redis_read_line(int fd, char *buf, size_t max) {
    size_t off = 0;
    while (off < max - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') {
            if (off > 0 && buf[off - 1] == '\r') off--;
            buf[off] = '\0';
            return (int)off;
        }
        buf[off++] = c;
    }
    return -1;
}

static inline int rc_redis_read_reply(int fd, rc_redis_reply_t *reply) {
    char line[RC_REDIS_MAX_RESP_LEN];
    if (rc_redis_read_line(fd, line, sizeof(line)) < 0) return RC_ERR_IO;

    switch (line[0]) {
    case '+': /* Status */
        reply->type = RC_REDIS_REPLY_STATUS;
        reply->str = rc_strdup(line + 1);
        reply->str_len = strlen(reply->str ? reply->str : "");
        return RC_OK;

    case '-': /* Error */
        reply->type = RC_REDIS_REPLY_ERROR;
        reply->str = rc_strdup(line + 1);
        reply->str_len = strlen(reply->str ? reply->str : "");
        return RC_ERR;

    case ':': /* Integer */
        reply->type = RC_REDIS_REPLY_INTEGER;
        reply->integer = (int64_t)strtoll(line + 1, NULL, 10);
        return RC_OK;

    case '$': { /* Bulk string */
        int blen = atoi(line + 1);
        if (blen < 0) { reply->type = RC_REDIS_REPLY_NIL; return RC_OK; }
        reply->str = malloc((size_t)blen + 1);
        if (!reply->str) return RC_ERR_NOMEM;
        size_t got = 0;
        while (got < (size_t)blen) {
            ssize_t n = recv(fd, reply->str + got, (size_t)blen - got, 0);
            if (n <= 0) { free(reply->str); reply->str = NULL; return RC_ERR_IO; }
            got += (size_t)n;
        }
        reply->str[blen] = '\0';
        reply->str_len = (size_t)blen;
        reply->type = RC_REDIS_REPLY_STRING;
        /* Read trailing \r\n */
        char dummy[4];
        recv(fd, dummy, 2, 0);
        return RC_OK;
    }

    case '*': { /* Array */
        int count = atoi(line + 1);
        if (count < 0) { reply->type = RC_REDIS_REPLY_NIL; return RC_OK; }
        reply->type = RC_REDIS_REPLY_ARRAY;
        reply->element_count = count;
        reply->elements = calloc((size_t)count, sizeof(void *));
        if (!reply->elements) return RC_ERR_NOMEM;
        for (int i = 0; i < count; i++) {
            rc_redis_reply_t *elem = calloc(1, sizeof(rc_redis_reply_t));
            if (!elem) return RC_ERR_NOMEM;
            reply->elements[i] = elem;
            int r = rc_redis_read_reply(fd, elem);
            if (r != RC_OK) return r;
        }
        return RC_OK;
    }

    default:
        return RC_ERR_PROTO;
    }
}

/* ── Command sender ─────────────────────────────────────────── */
static inline int rc_redis_sendv(int fd, const char **args, int argc) {
    char cmd[RC_REDIS_MAX_CMD_LEN];
    int n = snprintf(cmd, sizeof(cmd), "*%d\r\n", argc);
    for (int i = 0; i < argc && (size_t)n < sizeof(cmd); i++) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n,
                      "$%zu\r\n%s\r\n", strlen(args[i]), args[i]);
    }
    size_t sent = 0;
    while (sent < (size_t)n) {
        ssize_t w = send(fd, cmd + sent, (size_t)n - sent, 0);
        if (w <= 0) return RC_ERR_IO;
        sent += (size_t)w;
    }
    return RC_OK;
}

static inline int rc_redis_command(int fd, rc_redis_reply_t *reply,
                                    const char **args, int argc) {
    if (rc_redis_sendv(fd, args, argc) != RC_OK) return RC_ERR_IO;
    return rc_redis_read_reply(fd, reply);
}

/* ── Connection pool ────────────────────────────────────────── */
static inline int rc_redis_pool_init(rc_redis_pool_t *pool, const char *url,
                                      int pool_size, int timeout_ms) {
    memset(pool, 0, sizeof(*pool));
    pool->pool_size = pool_size;
    pool->timeout_ms = timeout_ms;
    rc_spin_init(&pool->lock);
    rc_atomic_init(&pool->total_commands, 0);
    rc_atomic_init(&pool->total_errors, 0);

    /* Parse redis://host:port */
    const char *p = url;
    if (strncmp(p, "redis://", 8) == 0) p += 8;
    const char *colon = strchr(p, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= sizeof(pool->host)) hlen = sizeof(pool->host) - 1;
        memcpy(pool->host, p, hlen);
        pool->host[hlen] = '\0';
        pool->port = atoi(colon + 1);
    } else {
        strncpy(pool->host, p, sizeof(pool->host) - 1);
        pool->port = 6379;
    }
    if (pool->port <= 0) pool->port = 6379;

    /* Pre-create connections */
    for (int i = 0; i < pool_size; i++) {
        int fd = rc_redis_connect_host(pool->host, pool->port, timeout_ms);
        if (fd < 0) continue;
        rc_redis_conn_t *conn = calloc(1, sizeof(rc_redis_conn_t));
        if (!conn) { close(fd); continue; }
        conn->fd = fd;
        conn->in_use = 0;
        conn->last_used = rc_time_ms();
        rc_spin_lock(&pool->lock);
        conn->next = pool->free_list;
        pool->free_list = conn;
        rc_spin_unlock(&pool->lock);
    }
    return RC_OK;
}

static inline rc_redis_conn_t *rc_redis_pool_acquire(rc_redis_pool_t *pool) {
    rc_spin_lock(&pool->lock);
    rc_redis_conn_t *conn = pool->free_list;
    if (conn) {
        pool->free_list = conn->next;
        conn->next = NULL;
        conn->in_use = 1;
        conn->last_used = rc_time_ms();
        rc_spin_unlock(&pool->lock);
        return conn;
    }
    rc_spin_unlock(&pool->lock);

    /* Pool empty - create new connection */
    int fd = rc_redis_connect_host(pool->host, pool->port, pool->timeout_ms);
    if (fd < 0) return NULL;
    conn = calloc(1, sizeof(rc_redis_conn_t));
    if (!conn) { close(fd); return NULL; }
    conn->fd = fd;
    conn->in_use = 1;
    conn->last_used = rc_time_ms();
    return conn;
}

static inline void rc_redis_pool_release(rc_redis_pool_t *pool, rc_redis_conn_t *conn) {
    if (!conn) return;
    conn->in_use = 0;
    conn->last_used = rc_time_ms();
    rc_spin_lock(&pool->lock);
    conn->next = pool->free_list;
    pool->free_list = conn;
    rc_spin_unlock(&pool->lock);
}

static inline void rc_redis_pool_destroy(rc_redis_pool_t *pool) {
    rc_spin_lock(&pool->lock);
    rc_redis_conn_t *conn = pool->free_list;
    while (conn) {
        rc_redis_conn_t *next = conn->next;
        close(conn->fd);
        free(conn);
        conn = next;
    }
    pool->free_list = NULL;
    rc_spin_unlock(&pool->lock);
}

/* ── High-level commands ────────────────────────────────────── */
static inline int rc_redis_ping(rc_redis_pool_t *pool) {
    rc_redis_conn_t *conn = rc_redis_pool_acquire(pool);
    if (!conn) return RC_ERR_IO;
    const char *args[] = { "PING" };
    rc_redis_reply_t reply;
    int r = rc_redis_command(conn->fd, &reply, args, 1);
    rc_redis_pool_release(pool, conn);
    rc_redis_reply_free(&reply);
    return r;
}

static inline int rc_redis_set(rc_redis_pool_t *pool, const char *key,
                                const char *value, int ttl_sec) {
    rc_redis_conn_t *conn = rc_redis_pool_acquire(pool);
    if (!conn) return RC_ERR_IO;

    char ttl_str[32];
    const char *args[6];
    int argc;
    if (ttl_sec > 0) {
        snprintf(ttl_str, sizeof(ttl_str), "%d", ttl_sec);
        args[0] = "SET"; args[1] = key; args[2] = value;
        args[3] = "EX"; args[4] = ttl_str;
        argc = 5;
    } else {
        args[0] = "SET"; args[1] = key; args[2] = value;
        argc = 3;
    }

    rc_redis_reply_t reply;
    int r = rc_redis_command(conn->fd, &reply, args, argc);
    rc_atomic_inc(&pool->total_commands);
    if (r != RC_OK) rc_atomic_inc(&pool->total_errors);
    rc_redis_pool_release(pool, conn);
    rc_redis_reply_free(&reply);
    return r;
}

static inline char *rc_redis_get(rc_redis_pool_t *pool, const char *key) {
    rc_redis_conn_t *conn = rc_redis_pool_acquire(pool);
    if (!conn) return NULL;

    const char *args[] = { "GET", key };
    rc_redis_reply_t reply;
    int r = rc_redis_command(conn->fd, &reply, args, 2);
    rc_atomic_inc(&pool->total_commands);
    rc_redis_pool_release(pool, conn);

    if (r != RC_OK || reply.type == RC_REDIS_REPLY_NIL) {
        rc_redis_reply_free(&reply);
        return NULL;
    }
    char *result = reply.str;
    reply.str = NULL; /* Transfer ownership */
    rc_redis_reply_free(&reply);
    return result;
}

static inline int rc_redis_del(rc_redis_pool_t *pool, const char *key) {
    rc_redis_conn_t *conn = rc_redis_pool_acquire(pool);
    if (!conn) return RC_ERR_IO;

    const char *args[] = { "DEL", key };
    rc_redis_reply_t reply;
    int r = rc_redis_command(conn->fd, &reply, args, 2);
    rc_atomic_inc(&pool->total_commands);
    rc_redis_pool_release(pool, conn);
    rc_redis_reply_free(&reply);
    return r;
}

static inline int rc_redis_exists(rc_redis_pool_t *pool, const char *key) {
    rc_redis_conn_t *conn = rc_redis_pool_acquire(pool);
    if (!conn) return 0;

    const char *args[] = { "EXISTS", key };
    rc_redis_reply_t reply;
    int r = rc_redis_command(conn->fd, &reply, args, 2);
    rc_atomic_inc(&pool->total_commands);
    rc_redis_pool_release(pool, conn);

    int exists = (r == RC_OK && reply.type == RC_REDIS_REPLY_INTEGER && reply.integer > 0);
    rc_redis_reply_free(&reply);
    return exists;
}

static inline int rc_redis_expire(rc_redis_pool_t *pool, const char *key, int ttl_sec) {
    rc_redis_conn_t *conn = rc_redis_pool_acquire(pool);
    if (!conn) return RC_ERR_IO;

    char ttl_str[32];
    snprintf(ttl_str, sizeof(ttl_str), "%d", ttl_sec);
    const char *args[] = { "EXPIRE", key, ttl_str };
    rc_redis_reply_t reply;
    int r = rc_redis_command(conn->fd, &reply, args, 3);
    rc_atomic_inc(&pool->total_commands);
    rc_redis_pool_release(pool, conn);
    rc_redis_reply_free(&reply);
    return r;
}

static inline int64_t rc_redis_incr(rc_redis_pool_t *pool, const char *key) {
    rc_redis_conn_t *conn = rc_redis_pool_acquire(pool);
    if (!conn) return -1;

    const char *args[] = { "INCR", key };
    rc_redis_reply_t reply;
    int r = rc_redis_command(conn->fd, &reply, args, 2);
    rc_atomic_inc(&pool->total_commands);
    rc_redis_pool_release(pool, conn);

    int64_t val = -1;
    if (r == RC_OK && reply.type == RC_REDIS_REPLY_INTEGER) val = reply.integer;
    rc_redis_reply_free(&reply);
    return val;
}

static inline int64_t rc_redis_incrby(rc_redis_pool_t *pool, const char *key, int64_t amount) {
    rc_redis_conn_t *conn = rc_redis_pool_acquire(pool);
    if (!conn) return -1;

    char amt_str[32];
    snprintf(amt_str, sizeof(amt_str), "%ld", (long)amount);
    const char *args[] = { "INCRBY", key, amt_str };
    rc_redis_reply_t reply;
    int r = rc_redis_command(conn->fd, &reply, args, 3);
    rc_atomic_inc(&pool->total_commands);
    rc_redis_pool_release(pool, conn);

    int64_t val = -1;
    if (r == RC_OK && reply.type == RC_REDIS_REPLY_INTEGER) val = reply.integer;
    rc_redis_reply_free(&reply);
    return val;
}

/* ── Cleanup stale connections ──────────────────────────────── */
static inline void rc_redis_pool_cleanup(rc_redis_pool_t *pool, int64_t max_age_ms) {
    int64_t now = rc_time_ms();
    rc_spin_lock(&pool->lock);
    rc_redis_conn_t **pp = &pool->free_list;
    while (*pp) {
        if ((now - (*pp)->last_used) > max_age_ms) {
            rc_redis_conn_t *dead = *pp;
            *pp = dead->next;
            close(dead->fd);
            free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
    rc_spin_unlock(&pool->lock);
}

#ifdef __cplusplus
}
#endif

#endif /* ROBOCONTROL_REDIS_H */
