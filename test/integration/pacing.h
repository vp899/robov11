/**
 * @file pacing.h
 * @brief Pacing send utility — token-bucket rate limiter for UDP.
 *
 * Controls send rate to simulate real-world bandwidth constraints.
 * Supports sub-millisecond granularity using busy-wait for accuracy.
 */

#ifndef PACING_H
#define PACING_H

#include <stdint.h>
#include <time.h>
#include <stdbool.h>

/**
 * @brief Pacer state — token bucket with microsecond precision.
 */
typedef struct {
    uint64_t rate_bps;        /**< Target rate in bits per second */
    uint64_t bucket_bytes;    /**< Current tokens in bytes */
    uint64_t max_bucket;      /**< Max burst in bytes (= rate/8 for 1s burst) */
    uint64_t last_refill_us;  /**< Last refill timestamp (µs, monotonic) */
    uint64_t total_sent;      /**< Total bytes sent through this pacer */
    uint64_t total_sleep_us;  /**< Total time spent pacing (µs) */
} pacer_t;

/**
 * @brief Get current monotonic time in microseconds.
 */
static inline uint64_t pacer_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @brief Initialize a pacer with a target bitrate.
 *
 * @param p         Pacer to initialize.
 * @param rate_bps  Target rate in bits per second (e.g., 1000000 for 1 Mbps).
 */
static inline void pacer_init(pacer_t *p, uint64_t rate_bps)
{
    p->rate_bps       = rate_bps;
    p->bucket_bytes   = rate_bps / 8;  /* Start with full bucket (1s worth) */
    p->max_bucket     = rate_bps / 8;
    p->last_refill_us = pacer_now_us();
    p->total_sent     = 0;
    p->total_sleep_us = 0;
}

/**
 * @brief Refill the token bucket based on elapsed time.
 */
static inline void pacer_refill(pacer_t *p)
{
    uint64_t now = pacer_now_us();
    uint64_t elapsed = now - p->last_refill_us;
    if (elapsed == 0) return;

    /* Add tokens proportional to elapsed time */
    uint64_t new_tokens = (p->rate_bps * elapsed) / 8000000ULL;
    p->bucket_bytes += new_tokens;
    if (p->bucket_bytes > p->max_bucket)
        p->bucket_bytes = p->max_bucket;

    p->last_refill_us = now;
}

/**
 * @brief Wait until enough tokens are available to send `bytes` bytes.
 *
 * This is the core pacing function. It sleeps (busy-waits for sub-ms accuracy)
 * until the token bucket has enough capacity, then deducts the tokens.
 *
 * @param p      Pacer state.
 * @param bytes  Number of bytes to send.
 */
static inline void pacer_wait(pacer_t *p, uint64_t bytes)
{
    pacer_refill(p);

    while (p->bucket_bytes < bytes) {
        /* Calculate how long to wait for enough tokens */
        uint64_t deficit = bytes - p->bucket_bytes;
        uint64_t wait_us = (deficit * 8000000ULL) / p->rate_bps;
        if (wait_us < 1) wait_us = 1;

        uint64_t start = pacer_now_us();

        if (wait_us > 2000) {
            /* Long wait: sleep most of it, then busy-wait the rest */
            struct timespec ts = {
                .tv_sec  = (wait_us - 1000) / 1000000,
                .tv_nsec = ((wait_us - 1000) % 1000000) * 1000,
            };
            nanosleep(&ts, NULL);
        }
        /* Busy-wait for remaining time (sub-ms accuracy) */
        while (pacer_now_us() - start < wait_us) {
            /* spin */
        }

        pacer_refill(p);
    }

    p->bucket_bytes -= bytes;
    p->total_sent += bytes;
}

/**
 * @brief Check if we can send `bytes` without waiting (non-blocking).
 *
 * @return true if tokens available (and deducted), false if would need to wait.
 */
static inline bool pacer_try_send(pacer_t *p, uint64_t bytes)
{
    pacer_refill(p);
    if (p->bucket_bytes >= bytes) {
        p->bucket_bytes -= bytes;
        p->total_sent += bytes;
        return true;
    }
    return false;
}

/**
 * @brief Get effective send rate so far (for reporting).
 *
 * @return Effective rate in bits per second.
 */
static inline uint64_t pacer_effective_rate(const pacer_t *p)
{
    uint64_t elapsed = pacer_now_us() - p->last_refill_us + p->total_sleep_us;
    if (elapsed == 0) return 0;
    return (p->total_sent * 8 * 1000000ULL) / elapsed;
}

#endif /* PACING_H */
