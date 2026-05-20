#include <stdbool.h>
/**
 * @file rc_epoll.c
 * @brief High-performance event loop with hierarchical timer wheel.
 *
 * Uses Linux epoll with EPOLLEXCLUSIVE for thundering-herd avoidance
 * and a 4-level hierarchical timer wheel for efficient timeout
 * management at millisecond resolution over long durations (up to ~50 days).
 *
 * Design:
 *   - I/O watchers and timers are pre-allocated in pools to avoid malloc
 *     in the hot path.
 *   - Batch event processing: up to 1024 events per epoll_wait.
 *   - SO_REUSEPORT for multi-threaded accept.
 *   - eventfd for cross-thread wakeup.
 */

#include "rc_epoll.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

/* ================================================================== */
/*  Time helpers                                                       */
/* ================================================================== */

uint64_t rc_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ================================================================== */
/*  Socket helpers                                                     */
/* ================================================================== */

int rc_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int rc_set_reuseport(int fd)
{
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
}

/* ================================================================== */
/*  Event loop creation / destruction                                  */
/* ================================================================== */

rc_event_loop_t *rc_loop_create(uint32_t max_fds)
{
    if (max_fds == 0) max_fds = 1024;

    rc_event_loop_t *loop = (rc_event_loop_t *)calloc(1, sizeof(*loop));
    if (!loop) {
        syslog(LOG_ERR, "epoll: loop alloc failed");
        return NULL;
    }

    /* Create epoll instance with EPOLLEXCLUSIVE support */
    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epfd < 0) {
        syslog(LOG_ERR, "epoll: epoll_create1 failed: %s", strerror(errno));
        free(loop);
        return NULL;
    }

    /* Create eventfd for cross-thread wakeup */
    loop->wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wakeup_fd < 0) {
        syslog(LOG_ERR, "epoll: eventfd failed: %s", strerror(errno));
        close(loop->epfd);
        free(loop);
        return NULL;
    }

    /* Register wakeup fd with epoll */
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.ptr = NULL,  /* NULL ptr signals wakeup */
    };
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->wakeup_fd, &ev) < 0) {
        syslog(LOG_ERR, "epoll: failed to add wakeup fd: %s", strerror(errno));
        close(loop->wakeup_fd);
        close(loop->epfd);
        free(loop);
        return NULL;
    }

    /* Pre-allocate I/O watcher pool */
    uint32_t io_pool_size = max_fds;
    loop->io_pool = (rc_io_watcher_t *)calloc(io_pool_size,
                                               sizeof(rc_io_watcher_t));
    if (!loop->io_pool) {
        syslog(LOG_ERR, "epoll: io pool alloc failed (%u entries)", io_pool_size);
        close(loop->wakeup_fd);
        close(loop->epfd);
        free(loop);
        return NULL;
    }
    loop->io_pool_size = io_pool_size;

    /* Build free list */
    for (uint32_t i = 0; i < io_pool_size - 1; i++) {
        loop->io_pool[i].next = &loop->io_pool[i + 1];
    }
    loop->io_pool[io_pool_size - 1].next = NULL;
    loop->io_watchers = &loop->io_pool[0];

    /* Pre-allocate timer pool */
    uint32_t timer_pool_size = max_fds * 2;  /* timers can outnumber fds */
    loop->timer_pool = (rc_timer_t *)calloc(timer_pool_size, sizeof(rc_timer_t));
    if (!loop->timer_pool) {
        syslog(LOG_ERR, "epoll: timer pool alloc failed");
        free(loop->io_pool);
        close(loop->wakeup_fd);
        close(loop->epfd);
        free(loop);
        return NULL;
    }
    loop->timer_pool_size = timer_pool_size;

    /* Build timer free list */
    for (uint32_t i = 0; i < timer_pool_size - 1; i++) {
        loop->timer_pool[i].next = &loop->timer_pool[i + 1];
    }
    loop->timer_pool[timer_pool_size - 1].next = NULL;
    loop->timer_free = &loop->timer_pool[0];

    /* Initialise timer wheel */
    memset(loop->wheel, 0, sizeof(loop->wheel));
    memset(loop->wheel_ticks, 0, sizeof(loop->wheel_ticks));
    loop->current_ms = rc_time_ms();

    loop->running = false;
    loop->thread_id = 0;

    syslog(LOG_INFO, "epoll: loop created (max_fds=%u, timers=%u)",
           max_fds, timer_pool_size);
    return loop;
}

/* ------------------------------------------------------------------ */

void rc_loop_destroy(rc_event_loop_t *loop)
{
    if (!loop) return;

    /* Close all registered fds */
    if (loop->io_pool) {
        for (uint32_t i = 0; i < loop->io_pool_size; i++) {
            if (loop->io_pool[i].active && loop->io_pool[i].fd >= 0) {
                epoll_ctl(loop->epfd, EPOLL_CTL_DEL,
                          loop->io_pool[i].fd, NULL);
            }
        }
        free(loop->io_pool);
    }

    if (loop->timer_pool) free(loop->timer_pool);
    if (loop->wakeup_fd >= 0) close(loop->wakeup_fd);
    if (loop->epfd >= 0) close(loop->epfd);

    free(loop);
    syslog(LOG_INFO, "epoll: loop destroyed");
}

/* ================================================================== */
/*  I/O watcher management                                             */
/* ================================================================== */

rc_io_watcher_t *rc_io_add(rc_event_loop_t *loop, int fd, uint32_t events,
                           rc_io_cb callback, void *arg)
{
    if (!loop || fd < 0 || !callback) return NULL;

    /* Pop from free list */
    if (!loop->io_watchers) {
        syslog(LOG_WARNING, "epoll: io watcher pool exhausted");
        return NULL;
    }

    rc_io_watcher_t *w = loop->io_watchers;
    loop->io_watchers = w->next;

    w->fd       = fd;
    w->events   = events;
    w->callback = callback;
    w->arg      = arg;
    w->active   = true;
    w->next     = NULL;

    /* Register with epoll.  Use EPOLLEXCLUSIVE on listen sockets
     * to distribute events across threads. */
    struct epoll_event ev = {
        .events = events | EPOLLET | EPOLLEXCLUSIVE,
        .data.ptr = w,
    };

    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        syslog(LOG_ERR, "epoll: EPOLL_CTL_ADD failed (fd=%d): %s",
               fd, strerror(errno));
        /* Return to free list */
        w->next = loop->io_watchers;
        loop->io_watchers = w;
        return NULL;
    }

    return w;
}

/* ------------------------------------------------------------------ */

int rc_io_mod(rc_event_loop_t *loop, rc_io_watcher_t *watcher,
              uint32_t events)
{
    if (!loop || !watcher || !watcher->active) return -1;

    struct epoll_event ev = {
        .events = events | EPOLLET | EPOLLEXCLUSIVE,
        .data.ptr = watcher,
    };

    if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, watcher->fd, &ev) < 0) {
        syslog(LOG_ERR, "epoll: EPOLL_CTL_MOD failed (fd=%d): %s",
               watcher->fd, strerror(errno));
        return -1;
    }

    watcher->events = events;
    return 0;
}

/* ------------------------------------------------------------------ */

void rc_io_del(rc_event_loop_t *loop, rc_io_watcher_t *watcher)
{
    if (!loop || !watcher || !watcher->active) return;

    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, watcher->fd, NULL);

    watcher->active = false;
    watcher->fd = -1;

    /* Return to free list */
    watcher->next = loop->io_watchers;
    loop->io_watchers = watcher;
}

/* ================================================================== */
/*  Timer wheel                                                        */
/* ================================================================== */

/**
 * @brief Determine which wheel level and slot a timer belongs to.
 *
 * Level 0 covers the first 256 ms, level 1 covers up to ~65 s, etc.
 *
 * @param loop      Event loop.
 * @param expiry_ms Absolute expiry time in ms.
 * @param[out] level  Wheel level.
 * @param[out] slot   Slot within that level.
 */
static void timer_slot(rc_event_loop_t *loop, uint64_t expiry_ms,
                       uint32_t *level, uint32_t *slot)
{
    uint64_t delta = expiry_ms - loop->current_ms;

    if (delta < RC_TIMER_SLOTS) {
        *level = 0;
        *slot = (uint32_t)(expiry_ms % RC_TIMER_SLOTS);
    } else if (delta < (uint64_t)RC_TIMER_SLOTS * RC_TIMER_SLOTS) {
        *level = 1;
        *slot = (uint32_t)((expiry_ms / RC_TIMER_SLOTS) % RC_TIMER_SLOTS);
    } else if (delta < (uint64_t)RC_TIMER_SLOTS * RC_TIMER_SLOTS * RC_TIMER_SLOTS) {
        *level = 2;
        *slot = (uint32_t)((expiry_ms / ((uint64_t)RC_TIMER_SLOTS * RC_TIMER_SLOTS))
                           % RC_TIMER_SLOTS);
    } else {
        *level = 3;
        *slot = (uint32_t)((expiry_ms / ((uint64_t)RC_TIMER_SLOTS * RC_TIMER_SLOTS *
                                          RC_TIMER_SLOTS)) % RC_TIMER_SLOTS);
    }
}

/**
 * @brief Insert a timer into the wheel at the appropriate slot.
 */
static void timer_insert(rc_event_loop_t *loop, rc_timer_t *t)
{
    uint32_t level, slot;
    timer_slot(loop, t->expiry_ms, &level, &slot);

    /* Insert at head of the doubly-linked list for this slot */
    t->prev = NULL;
    t->next = loop->wheel[level][slot];
    if (loop->wheel[level][slot]) {
        loop->wheel[level][slot]->prev = t;
    }
    loop->wheel[level][slot] = t;
    t->active = true;
}

/**
 * @brief Remove a timer from its current wheel slot.
 */
static void timer_remove(rc_event_loop_t *loop, rc_timer_t *t)
{
    if (!t->active) return;

    /* Unlink from doubly-linked list */
    if (t->prev) {
        t->prev->next = t->next;
    } else {
        /* t is the head — find the slot it's in */
        uint32_t level, slot;
        timer_slot(loop, t->expiry_ms, &level, &slot);
        loop->wheel[level][slot] = t->next;
    }
    if (t->next) {
        t->next->prev = t->prev;
    }

    t->next = NULL;
    t->prev = NULL;
    t->active = false;
}

/* ------------------------------------------------------------------ */

rc_timer_t *rc_timer_add(rc_event_loop_t *loop, uint32_t delay_ms,
                         uint32_t interval_ms, rc_timer_cb callback,
                         void *arg)
{
    if (!loop || !callback) return NULL;

    /* Pop from free list */
    if (!loop->timer_free) {
        syslog(LOG_WARNING, "epoll: timer pool exhausted");
        return NULL;
    }

    rc_timer_t *t = loop->timer_free;
    loop->timer_free = t->next;

    t->expiry_ms   = loop->current_ms + delay_ms;
    t->interval_ms = interval_ms;
    t->callback    = callback;
    t->arg         = arg;
    t->active      = false;
    t->next        = NULL;
    t->prev        = NULL;

    timer_insert(loop, t);
    return t;
}

/* ------------------------------------------------------------------ */

void rc_timer_cancel(rc_event_loop_t *loop, rc_timer_t *timer)
{
    if (!loop || !timer) return;

    timer_remove(loop, timer);

    /* Return to free list */
    timer->next = loop->timer_free;
    loop->timer_free = timer;
}

/* ------------------------------------------------------------------ */

void rc_timer_rearm(rc_event_loop_t *loop, rc_timer_t *timer,
                    uint32_t delay_ms)
{
    if (!loop || !timer) return;

    timer_remove(loop, timer);
    timer->expiry_ms = loop->current_ms + delay_ms;
    timer_insert(loop, timer);
}

/* ================================================================== */
/*  Timer processing                                                   */
/* ================================================================== */

/**
 * @brief Process all expired timers up to the current time.
 *
 * Walks level-0 slot for the current tick.  When level-0 wraps,
 * cascades timers from level-1 down to level-0 (and so on).
 *
 * @param loop  Event loop.
 */
static void process_timers(rc_event_loop_t *loop)
{
    uint64_t now = rc_time_ms();

    while (loop->current_ms <= now) {
        uint32_t slot = loop->current_ms % RC_TIMER_SLOTS;

        /* Process all timers in the current level-0 slot */
        rc_timer_t *t = loop->wheel[0][slot];
        loop->wheel[0][slot] = NULL;

        while (t) {
            rc_timer_t *next = t->next;
            t->next = NULL;
            t->prev = NULL;

            if (loop->current_ms >= t->expiry_ms) {
                /* Timer has expired — fire callback */
                t->active = false;
                t->callback(loop, t, t->arg);

                /* If repeating, rearm */
                if (t->interval_ms > 0) {
                    t->expiry_ms = loop->current_ms + t->interval_ms;
                    timer_insert(loop, t);
                } else {
                    /* Return to free list */
                    t->next = loop->timer_free;
                    loop->timer_free = t;
                }
            } else {
                /* Not yet expired — reinsert */
                timer_insert(loop, t);
            }

            t = next;
        }

        /* Advance current time */
        loop->current_ms++;

        /* Cascade from higher levels when lower level wraps */
        if (slot == 0) {
            for (uint32_t lvl = 1; lvl < RC_TIMER_LEVELS; lvl++) {
                uint32_t parent_slot = loop->wheel_ticks[lvl];
                loop->wheel_ticks[lvl]++;

                /* Move all timers from this parent slot down one level */
                rc_timer_t *cascade = loop->wheel[lvl][parent_slot];
                loop->wheel[lvl][parent_slot] = NULL;

                while (cascade) {
                    rc_timer_t *next_c = cascade->next;
                    cascade->next = NULL;
                    cascade->prev = NULL;
                    timer_insert(loop, cascade);
                    cascade = next_c;
                }

                /* Only cascade one level per tick unless it also wraps */
                if (loop->wheel_ticks[lvl] % RC_TIMER_SLOTS != 0) break;
            }
        }
    }
}

/* ================================================================== */
/*  Event loop run                                                     */
/* ================================================================== */

void rc_loop_run(rc_event_loop_t *loop)
{
    if (!loop) return;

    loop->running = true;
    loop->thread_id = pthread_self();

    syslog(LOG_INFO, "epoll: loop running");

    while (loop->running) {
        /* Process expired timers first */
        process_timers(loop);

        /* Calculate how long to block in epoll_wait.
         * If there are pending timers, wake up when the next one expires.
         * Otherwise, block indefinitely (or for 1 second to process timers). */
        int timeout_ms = 1000;  /* default: 1 second */
        if (loop->wheel[0][loop->current_ms % RC_TIMER_SLOTS]) {
            timeout_ms = 0;  /* immediate — timers pending */
        }

        /* Batch poll for events */
        int nfds = epoll_wait(loop->epfd, loop->events,
                              RC_EPOLL_MAX_EVENTS, timeout_ms);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "epoll: epoll_wait failed: %s", strerror(errno));
            break;
        }

        /* Process all ready events */
        for (int i = 0; i < nfds; i++) {
            rc_io_watcher_t *w = (rc_io_watcher_t *)loop->events[i].data.ptr;

            if (w == NULL) {
                /* Wakeup event — drain the eventfd */
                uint64_t val;
                ssize_t r = read(loop->wakeup_fd, &val, sizeof(val));
                (void)r;
                continue;
            }

            if (w->active && w->callback) {
                w->callback(loop, w->fd, loop->events[i].events, w->arg);
            }
        }
    }

    syslog(LOG_INFO, "epoll: loop stopped");
}

/* ------------------------------------------------------------------ */

void rc_loop_stop(rc_event_loop_t *loop)
{
    if (!loop) return;
    loop->running = false;
    rc_loop_wakeup(loop);
}

/* ------------------------------------------------------------------ */

void rc_loop_wakeup(rc_event_loop_t *loop)
{
    if (!loop || loop->wakeup_fd < 0) return;
    uint64_t val = 1;
    ssize_t w = write(loop->wakeup_fd, &val, sizeof(val));
    (void)w;
}
