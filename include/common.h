/*
 * robocontrol/common.h - Common utilities, logging, error handling
 * Production-grade remote robot control system
 */
#ifndef ROBOCONTROL_COMMON_H
#define ROBOCONTROL_COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <syslog.h>
#include <getopt.h>
#include <limits.h>

/* ── Version ────────────────────────────────────────────────── */
#define ROBOCONTROL_VERSION_MAJOR 1
#define ROBOCONTROL_VERSION_MINOR 0
#define ROBOCONTROL_VERSION_PATCH 0
#define ROBOCONTROL_VERSION "1.0.0"

/* ── Limits ─────────────────────────────────────────────────── */
#define MAX_EPOLL_EVENTS        4096
#define MAX_CONNECTIONS         1048576   /* 1M */
#define MAX_RELAY_SESSIONS      500000
#define MAX_ROOM_CLIENTS        64
#define MAX_TOKEN_LEN           512
#define MAX_ID_LEN              128
#define MAX_SDP_LEN             8192
#define MAX_ICE_LEN             1024
#define MAX_HTTP_HEADER         8192
#define MAX_HTTP_BODY           65536
#define MAX_PATH_LEN            256
#define MAX_KEY_LEN             256
#define MAX_URL_LEN             512
#define RINGBUF_SIZE            (1 << 20) /* 1 MB default ring buffer */
#define WORKER_THREAD_STACK     (8 * 1024 * 1024)

/* ── Error codes ────────────────────────────────────────────── */
typedef enum {
    RC_OK           =  0,
    RC_ERR          = -1,
    RC_ERR_NOMEM    = -2,
    RC_ERR_INVAL    = -3,
    RC_ERR_IO       = -4,
    RC_ERR_TIMEOUT  = -5,
    RC_ERR_AUTH     = -6,
    RC_ERR_EXISTS   = -7,
    RC_ERR_NOTFOUND = -8,
    RC_ERR_FULL     = -9,
    RC_ERR_PROTO    = -10,
    RC_ERR_CRYPTO   = -11,
    RC_ERR_RATE     = -12,
} rc_err_t;

/* ── Logging ────────────────────────────────────────────────── */
typedef enum {
    RC_LOG_DEBUG = 0,
    RC_LOG_INFO,
    RC_LOG_WARN,
    RC_LOG_ERROR,
    RC_LOG_FATAL,
} rc_log_level_t;

static rc_log_level_t g_log_level = RC_LOG_INFO;
static int g_use_syslog = 0;

static inline void rc_log_set_level(rc_log_level_t level) { g_log_level = level; }
static inline void rc_log_set_syslog(int enable) { g_use_syslog = enable; }

static inline void rc_log(rc_log_level_t level, const char *file, int line,
                          const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static inline void rc_log(rc_log_level_t level, const char *file, int line,
                          const char *fmt, ...) {
    if (level < g_log_level) return;

    static const char *level_names[] = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };
    static const int syslog_prio[] = { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR, LOG_CRIT };

    va_list ap;
    va_start(ap, fmt);

    if (g_use_syslog) {
        char buf[2048];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        syslog(syslog_prio[level], "[%s] %s:%d: %s", level_names[level], file, line, buf);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);

        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

        fprintf(stderr, "%s.%03ld [%s] %s:%d: ", timebuf, ts.tv_nsec / 1000000,
                level_names[level], file, line);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
    va_end(ap);
}

#define RC_LOG_DBG(fmt, ...) rc_log(RC_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define RC_LOG_INF(fmt, ...) rc_log(RC_LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define RC_LOG_WRN(fmt, ...) rc_log(RC_LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define RC_LOG_ERR(fmt, ...) rc_log(RC_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define RC_LOG_FTL(fmt, ...) rc_log(RC_LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* ── Atomic counters (metrics) ──────────────────────────────── */
typedef struct {
    volatile int64_t value;
} rc_atomic64_t;

static inline void rc_atomic_init(rc_atomic64_t *a, int64_t v) { a->value = v; }
static inline int64_t rc_atomic_get(rc_atomic64_t *a) { return __atomic_load_n(&a->value, __ATOMIC_RELAXED); }
static inline void rc_atomic_set(rc_atomic64_t *a, int64_t v) { __atomic_store_n(&a->value, v, __ATOMIC_RELAXED); }
static inline int64_t rc_atomic_add(rc_atomic64_t *a, int64_t v) { return __atomic_fetch_add(&a->value, v, __ATOMIC_RELAXED); }
static inline int64_t rc_atomic_inc(rc_atomic64_t *a) { return rc_atomic_add(a, 1); }
static inline int64_t rc_atomic_dec(rc_atomic64_t *a) { return rc_atomic_add(a, -1); }

/* ── Spinlock (lightweight for hot paths) ───────────────────── */
typedef struct {
    volatile int lock;
} rc_spinlock_t;

static inline void rc_spin_init(rc_spinlock_t *s) { s->lock = 0; }
static inline void rc_spin_lock(rc_spinlock_t *s) {
    while (__sync_lock_test_and_set(&s->lock, 1)) {
        __asm__ volatile("pause");
    }
}
static inline void rc_spin_unlock(rc_spinlock_t *s) { __sync_lock_release(&s->lock); }

/* ── Time helpers ───────────────────────────────────────────── */
static inline int64_t rc_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline int64_t rc_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec;
}

/* ── Socket helpers ─────────────────────────────────────────── */
static inline int rc_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return RC_ERR;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? RC_ERR : RC_OK;
}

static inline int rc_set_reuseaddr(int fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

static inline int rc_set_reuseport(int fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
}

static inline int rc_set_tcp_nodelay(int fd) {
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

static inline int rc_set_sendbuf(int fd, int size) {
    return setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

static inline int rc_set_recvbuf(int fd, int size) {
    return setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

static inline int rc_tcp_listen(const char *bind_addr, int port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    rc_set_reuseaddr(fd);
    rc_set_reuseport(fd);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (bind_addr && *bind_addr) {
        inet_pton(AF_INET, bind_addr, &addr.sin_addr);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static inline int rc_udp_bind(const char *bind_addr, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    rc_set_reuseaddr(fd);
    rc_set_reuseport(fd);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (bind_addr && *bind_addr) {
        inet_pton(AF_INET, bind_addr, &addr.sin_addr);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── Graceful shutdown ──────────────────────────────────────── */
static volatile sig_atomic_t g_shutdown = 0;
static inline void rc_request_shutdown(void) { g_shutdown = 1; }
static inline int rc_should_shutdown(void) { return g_shutdown; }

static void rc_signal_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

static inline void rc_install_signal_handlers(void) {
    struct sigaction sa = { .sa_handler = rc_signal_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* ── Thread helpers ─────────────────────────────────────────── */
static inline int rc_spawn_thread(pthread_t *t, void *(*fn)(void *), void *arg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WORKER_THREAD_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int ret = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return ret;
}

/* ── Random helpers ─────────────────────────────────────────── */
static inline void rc_random_bytes(uint8_t *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t n = read(fd, buf + off, len - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        close(fd);
    }
}

static inline uint64_t rc_random_u64(void) {
    uint64_t v;
    rc_random_bytes((uint8_t *)&v, sizeof(v));
    return v;
}

/* ── String helpers ─────────────────────────────────────────── */
static inline char *rc_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dup = malloc(len + 1);
    if (dup) memcpy(dup, s, len + 1);
    return dup;
}

static inline char *rc_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len);
        dup[len] = '\0';
    }
    return dup;
}

/* ── Min/Max ────────────────────────────────────────────────── */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#endif /* ROBOCONTROL_COMMON_H */
