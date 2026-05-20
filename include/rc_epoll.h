/**
 * @file rc_epoll.h
 * @brief High-performance event loop with hierarchical timer wheel.
 *
 * Uses Linux epoll for I/O multiplexing (EPOLLEXCLUSIVE for
 * thundering-herd avoidance) and a hierarchical timer wheel for
 * efficient timeout management at millisecond resolution.
 *
 * Supports:
 *   - 10M+ file descriptors
 *   - Batch event processing (up to 1024 events per poll)
 *   - SO_REUSEPORT for multi-threaded accept
 *   - Hierarchical timer wheel (ms precision, years of range)
 */

#ifndef RC_EPOLL_H
#define RC_EPOLL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/epoll.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

/** Maximum events per epoll_wait call. */
#define RC_EPOLL_MAX_EVENTS     1024

/** Timer wheel slots per level. */
#define RC_TIMER_SLOTS          256

/** Number of timer wheel levels. */
#define RC_TIMER_LEVELS         4

/**
 * Timer resolution per level (ms):
 *   Level 0: 1 ms   × 256 = 256 ms
 *   Level 1: 256 ms × 256 = 65.536 s
 *   Level 2: 65.536 s × 256 = 16,777 s (~4.66 h)
 *   Level 3: 16,777 s × 256 = 4,294,967 s (~49.7 days)
 */
#define RC_TIMER_LEVEL_MS(lvl)  (1U << (8 * (lvl)))   /* approximate */

/* ------------------------------------------------------------------ */
/*  Event callback types                                               */
/* ------------------------------------------------------------------ */

/** Forward declarations. */
typedef struct rc_event_loop rc_event_loop_t;
typedef struct rc_timer rc_timer_t;

/**
 * @brief I/O event callback.
 * @param loop   Event loop.
 * @param fd     File descriptor that triggered the event.
 * @param events epoll event mask (EPOLLIN, EPOLLOUT, etc.).
 * @param arg    User-provided opaque pointer.
 */
typedef void (*rc_io_cb)(rc_event_loop_t *loop, int fd,
                         uint32_t events, void *arg);

/**
 * @brief Timer callback.
 * @param loop  Event loop.
 * @param timer The timer that fired.
 * @param arg   User-provided opaque pointer.
 */
typedef void (*rc_timer_cb)(rc_event_loop_t *loop, rc_timer_t *timer,
                            void *arg);

/* ------------------------------------------------------------------ */
/*  I/O watcher                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief I/O watcher — wraps an fd registered with epoll.
 */
typedef struct rc_io_watcher {
    int               fd;         /**< File descriptor               */
    uint32_t          events;     /**< Monitored events mask         */
    rc_io_cb          callback;   /**< Event callback                */
    void             *arg;        /**< Opaque user data              */
    bool              active;     /**< True if registered with epoll */
    struct rc_io_watcher *next;   /**< Free-list linkage             */
} rc_io_watcher_t;

/* ------------------------------------------------------------------ */
/*  Timer (hierarchical wheel)                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Timer entry in the wheel.
 */
struct rc_timer {
    uint64_t          expiry_ms;    /**< Absolute expiry time (ms)    */
    uint32_t          interval_ms;  /**< Repeat interval (0 = once)   */
    rc_timer_cb       callback;     /**< Timer callback               */
    void             *arg;          /**< Opaque user data             */
    bool              active;       /**< True if armed                */
    rc_timer_t       *next;         /**< Wheel slot linkage           */
    rc_timer_t       *prev;         /**< Wheel slot linkage           */
};

/* ------------------------------------------------------------------ */
/*  Event loop                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Main event loop.
 */
struct rc_event_loop {
    int               epfd;                              /**< epoll fd           */
    struct epoll_event events[RC_EPOLL_MAX_EVENTS];      /**< Batch buffer       */

    /* --- I/O watchers --- */
    rc_io_watcher_t  *io_watchers;                       /**< Free list head     */
    rc_io_watcher_t  *io_pool;                           /**< Pre-allocated pool */
    uint32_t          io_pool_size;                      /**< Pool size          */

    /* --- Timer wheel --- */
    rc_timer_t       *wheel[RC_TIMER_LEVELS][RC_TIMER_SLOTS]; /**< Timer slots  */
    uint64_t          current_ms;                        /**< Current time (ms)  */
    uint32_t          wheel_ticks[RC_TIMER_LEVELS];      /**< Per-level cursor   */

    /* --- Timer pool --- */
    rc_timer_t       *timer_pool;                        /**< Pre-allocated pool */
    rc_timer_t       *timer_free;                        /**< Free list head     */
    uint32_t          timer_pool_size;                   /**< Pool size          */

    /* --- State --- */
    volatile bool     running;                           /**< Loop is running    */
    int               wakeup_fd;                         /**< eventfd for wakeup */
    pthread_t         thread_id;                         /**< Owning thread      */
};

/* ------------------------------------------------------------------ */
/*  Event loop API                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Create and initialise an event loop.
 * @param max_fds  Hint for max simultaneous fds (0 = 1024).
 * @return Pointer to loop, or NULL on failure.
 */
rc_event_loop_t *rc_loop_create(uint32_t max_fds);

/**
 * @brief Destroy an event loop and release all resources.
 * @param loop  Event loop.
 */
void rc_loop_destroy(rc_event_loop_t *loop);

/**
 * @brief Run the event loop until rc_loop_stop is called.
 * @param loop  Event loop.
 */
void rc_loop_run(rc_event_loop_t *loop);

/**
 * @brief Signal the event loop to stop after the current iteration.
 * @param loop  Event loop.
 */
void rc_loop_stop(rc_event_loop_t *loop);

/**
 * @brief Wake up the event loop from another thread.
 * @param loop  Event loop.
 */
void rc_loop_wakeup(rc_event_loop_t *loop);

/* ------------------------------------------------------------------ */
/*  I/O watcher API                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Create an I/O watcher for a file descriptor.
 * @param loop     Event loop.
 * @param fd       File descriptor.
 * @param events   epoll events (EPOLLIN | EPOLLOUT | etc.).
 * @param callback Event callback.
 * @param arg      Opaque user data.
 * @return Watcher handle, or NULL on failure.
 */
rc_io_watcher_t *rc_io_add(rc_event_loop_t *loop, int fd, uint32_t events,
                           rc_io_cb callback, void *arg);

/**
 * @brief Modify the events of an existing I/O watcher.
 * @param loop     Event loop.
 * @param watcher  Watcher to modify.
 * @param events   New event mask.
 * @return 0 on success, -1 on failure.
 */
int rc_io_mod(rc_event_loop_t *loop, rc_io_watcher_t *watcher,
              uint32_t events);

/**
 * @brief Remove an I/O watcher.
 * @param loop     Event loop.
 * @param watcher  Watcher to remove.
 */
void rc_io_del(rc_event_loop_t *loop, rc_io_watcher_t *watcher);

/* ------------------------------------------------------------------ */
/*  Timer API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Create a new timer.
 * @param loop        Event loop.
 * @param delay_ms    Initial delay in milliseconds.
 * @param interval_ms Repeat interval (0 = one-shot).
 * @param callback    Timer callback.
 * @param arg         Opaque user data.
 * @return Timer handle, or NULL on failure.
 */
rc_timer_t *rc_timer_add(rc_event_loop_t *loop, uint32_t delay_ms,
                         uint32_t interval_ms, rc_timer_cb callback,
                         void *arg);

/**
 * @brief Cancel a running timer.
 * @param loop   Event loop.
 * @param timer  Timer to cancel.
 */
void rc_timer_cancel(rc_event_loop_t *loop, rc_timer_t *timer);

/**
 * @brief Rearm a timer with a new delay.
 * @param loop      Event loop.
 * @param timer     Timer to rearm.
 * @param delay_ms  New delay in milliseconds.
 */
void rc_timer_rearm(rc_event_loop_t *loop, rc_timer_t *timer,
                    uint32_t delay_ms);

/**
 * @brief Get the current monotonic time in milliseconds.
 * @return Milliseconds since an arbitrary epoch.
 */
uint64_t rc_time_ms(void);

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Set a socket to SO_REUSEPORT for multi-threaded accept.
 * @param fd  Socket file descriptor.
 * @return 0 on success, -1 on failure.
 */
int rc_set_reuseport(int fd);

/**
 * @brief Set a socket to non-blocking mode.
 * @param fd  Socket file descriptor.
 * @return 0 on success, -1 on failure.
 */
int rc_set_nonblocking(int fd);

#ifdef __cplusplus
}
#endif

#endif /* RC_EPOLL_H */
