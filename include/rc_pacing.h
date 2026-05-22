/**
 * @file rc_pacing.h
 * @brief Pacing send — token-bucket rate limiter for the protocol layer.
 *
 * Integrates into the core send path to smooth packet emission and
 * avoid bursty traffic.  Uses a token-bucket algorithm with
 * microsecond-precision busy-wait for sub-millisecond accuracy.
 *
 * Typical usage:
 *   rc_pacer_t pacer;
 *   rc_pacer_init(&pacer, 1000000);  // 1 Mbps
 *   ...
 *   rc_pacer_wait(&pacer, packet_size);  // blocks until tokens available
 *   sendto(fd, buf, packet_size, ...);
 */

#ifndef RC_PACING_H
#define RC_PACING_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Pacer state                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Token-bucket pacer with microsecond resolution.
 *
 * All fields are plain (no locks) — intended for single-thread use.
 * For multi-thread callers, protect externally.
 */
typedef struct rc_pacer {
    uint64_t rate_bps;          /**< Target rate (bits/sec)             */
    uint64_t bucket_bytes;      /**< Current token count (bytes)        */
    uint64_t max_bucket;        /**< Max burst (bytes) = rate_bps / 8   */
    uint64_t last_refill_us;    /**< Last refill timestamp (µs, mono)   */
    uint64_t total_sent;        /**< Cumulative bytes paced             */
    uint64_t total_sleep_us;    /**< Cumulative time spent in pacing    */
    uint32_t min_sleep_us;      /**< Minimum busy-wait granularity      */
} rc_pacer_t;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Get monotonic time in microseconds. */
static inline uint64_t rc_pacer_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise a pacer.
 *
 * @param p         Pacer to init.
 * @param rate_bps  Target bitrate in bits/sec (e.g. 1000000 for 1 Mbps).
 */
static inline void rc_pacer_init(rc_pacer_t *p, uint64_t rate_bps)
{
    memset(p, 0, sizeof(*p));
    p->rate_bps       = rate_bps;
    p->bucket_bytes   = rate_bps / 8;      /* full bucket = 1 s burst */
    p->max_bucket     = rate_bps / 8;
    p->last_refill_us = rc_pacer_now_us();
    p->min_sleep_us   = 50;                /* 50 µs busy-wait floor  */
}

/**
 * @brief Refill tokens based on elapsed wall-clock time.
 */
static inline void rc_pacer_refill(rc_pacer_t *p)
{
    uint64_t now     = rc_pacer_now_us();
    uint64_t elapsed = now - p->last_refill_us;
    if (elapsed == 0) return;

    uint64_t new_tokens = (p->rate_bps * elapsed) / 8000000ULL;
    p->bucket_bytes += new_tokens;
    if (p->bucket_bytes > p->max_bucket)
        p->bucket_bytes = p->max_bucket;

    p->last_refill_us = now;
}

/**
 * @brief Block until enough tokens are available to send @p bytes.
 *
 * Uses nanosleep for long waits (> 2 ms) and busy-wait for the
 * final stretch to achieve sub-ms accuracy.
 *
 * @param p      Pacer.
 * @param bytes  Packet size (header + payload) in bytes.
 */
static inline void rc_pacer_wait(rc_pacer_t *p, uint64_t bytes)
{
    rc_pacer_refill(p);

    while (p->bucket_bytes < bytes) {
        uint64_t deficit  = bytes - p->bucket_bytes;
        uint64_t wait_us  = (deficit * 8000000ULL) / p->rate_bps;
        if (wait_us < 1) wait_us = 1;

        uint64_t start = rc_pacer_now_us();

        if (wait_us > 2000) {
            /* Long wait: sleep most, busy-wait the tail */
            struct timespec ts = {
                .tv_sec  = (long)((wait_us - 1000) / 1000000),
                .tv_nsec = (long)(((wait_us - 1000) % 1000000) * 1000),
            };
            nanosleep(&ts, NULL);
        }
        /* Busy-wait remainder */
        while (rc_pacer_now_us() - start < wait_us) {
            /* spin */
        }

        p->total_sleep_us += rc_pacer_now_us() - start;
        rc_pacer_refill(p);
    }

    p->bucket_bytes -= bytes;
    p->total_sent   += bytes;
}

/**
 * @brief Non-blocking try-send: deduct tokens if available.
 *
 * @return true  if tokens were available (and deducted).
 * @return false if caller should wait (tokens unchanged).
 */
static inline bool rc_pacer_try_send(rc_pacer_t *p, uint64_t bytes)
{
    rc_pacer_refill(p);
    if (p->bucket_bytes >= bytes) {
        p->bucket_bytes -= bytes;
        p->total_sent   += bytes;
        return true;
    }
    return false;
}

/**
 * @brief Get effective send rate so far.
 *
 * @return Rate in bits/sec.
 */
static inline uint64_t rc_pacer_effective_rate(const rc_pacer_t *p)
{
    uint64_t elapsed = p->total_sleep_us > 0 ? p->total_sleep_us : 1;
    return (p->total_sent * 8 * 1000000ULL) / elapsed;
}

/**
 * @brief Reset the pacer (e.g. after a pause).
 */
static inline void rc_pacer_reset(rc_pacer_t *p)
{
    p->bucket_bytes   = p->max_bucket;
    p->last_refill_us = rc_pacer_now_us();
}

#ifdef __cplusplus
}
#endif

#endif /* RC_PACING_H */
