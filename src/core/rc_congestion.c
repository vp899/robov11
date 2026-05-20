#include <stdbool.h>
/**
 * @file rc_congestion.c
 * @brief CUBIC congestion control (RFC 8312) implementation.
 *
 * Full CUBIC algorithm including:
 *   - Slow start (exponential growth)
 *   - Congestion avoidance (cubic growth)
 *   - Fast recovery
 *   - Fast convergence
 *   - RTT measurement and smoothing
 */

#include "rc_congestion.h"

#include <string.h>
#include <math.h>
#include <syslog.h>

/* ================================================================== */
/*  Fixed-point cube root helper                                       */
/* ================================================================== */

/**
 * @brief Integer cube root using Newton's method.
 *
 * Used by CUBIC to compute K = cbrt(W_max * (1 - beta) / C).
 * We work in fixed-point with 10 bits of fractional precision.
 *
 * @param x  Input value (fixed-point, scale = RC_CUBIC_BETA_SCALE).
 * @return Cube root (same scale).
 */
static uint32_t icbrt(uint64_t x)
{
    if (x == 0) return 0;

    /* Newton's method: guess, refine */
    uint32_t guess = 1;
    /* Find initial guess: largest power of 2 whose cube <= x */
    while ((uint64_t)guess * guess * guess <= x) {
        guess <<= 1;
    }
    guess >>= 1;

    /* Refine with a few Newton iterations */
    for (int i = 0; i < 20; i++) {
        uint64_t cube = (uint64_t)guess * guess * guess;
        if (cube == x) break;
        /* Newton step: guess = (2*guess + x/guess^2) / 3 */
        uint64_t div = x / ((uint64_t)guess * guess);
        guess = (uint32_t)(((uint64_t)2 * guess + div) / 3);
    }

    return guess;
}

/* ================================================================== */
/*  CUBIC function: W(t) = C * (t - K)^3 + W_max                      */
/* ================================================================== */

/**
 * @brief Evaluate the CUBIC function.
 *
 * Computes: W(t) = C * (t - K)^3 + W_max
 *
 * All values are in fixed-point (scale = RC_CUBIC_BETA_SCALE = 1024).
 * The result is converted back to bytes.
 *
 * @param cubic  CUBIC state.
 * @return Window size in bytes.
 */
static uint32_t cubic_eval(const rc_cubic_t *cubic)
{
    /*
     * t and K are in "ticks" (each tick = one ACK period roughly).
     * We compute (t - K)^3, multiply by C, add W_max.
     */
    int64_t t = (int64_t)cubic->t_sec;
    int64_t k = (int64_t)cubic->k;

    int64_t diff = t - k;
    int64_t diff3 = diff * diff * diff;

    /* C * diff3, with C in fixed-point */
    int64_t w = (int64_t)cubic->w_max +
                (diff3 * RC_CUBIC_C) / (RC_CUBIC_C_SCALE * RC_CUBIC_BETA_SCALE);

    if (w < (int64_t)RC_CUBIC_MIN_CWND) {
        w = RC_CUBIC_MIN_CWND;
    }

    return (uint32_t)w;
}

/* ================================================================== */
/*  API implementation                                                 */
/* ================================================================== */

void rc_cubic_init(rc_cubic_t *cubic, uint32_t mss)
{
    memset(cubic, 0, sizeof(*cubic));

    cubic->mss = (mss > 0) ? mss : RC_CUBIC_MSS;
    cubic->cwnd = RC_CUBIC_INIT_CWND;
    cubic->ssthresh = 0xFFFFFFFF;  /* effectively infinite */
    cubic->cwnd_prior = 0;

    cubic->w_max = 0;
    cubic->k = 0;
    cubic->t_sec = 0;

    cubic->srtt = 0;
    cubic->rtt_var = 0;
    cubic->min_rtt = 0xFFFFFFFF;
    cubic->rto = 1000;  /* default 1 second */

    cubic->pacing_rate = 0;
    cubic->total_acked = 0;
    cubic->recovery_episodes = 0;
    cubic->ack_count = 0;

    cubic->state = RC_CC_SLOW_START;
    cubic->in_slow_start = true;
}

/* ------------------------------------------------------------------ */

void rc_cubic_on_ack(rc_cubic_t *cubic, uint32_t bytes_acked, uint32_t rtt_ms)
{
    if (!cubic || bytes_acked == 0) return;

    cubic->total_acked += bytes_acked;
    cubic->ack_count++;

    /* Update RTT if provided */
    if (rtt_ms > 0) {
        rc_cubic_update_rtt(cubic, rtt_ms);
    }

    switch (cubic->state) {
    case RC_CC_SLOW_START:
        /*
         * Slow start: cwnd += bytes_acked (exponential growth).
         * Exit when cwnd >= ssthresh.
         */
        cubic->cwnd += bytes_acked;
        if (cubic->cwnd >= cubic->ssthresh) {
            cubic->state = RC_CC_CONGESTION_AVOIDANCE;
            cubic->in_slow_start = false;

            /* Record W_max and compute K for CUBIC function */
            cubic->w_max = cubic->cwnd;
            cubic->cwnd_prior = cubic->cwnd;
            cubic->t_sec = 0;

            /* K = cbrt(W_max * (1 - beta) / C) */
            uint64_t w_scaled = (uint64_t)cubic->w_max * RC_CUBIC_BETA_SCALE;
            uint64_t numerator = w_scaled * (RC_CUBIC_BETA_SCALE - RC_CUBIC_BETA);
            uint64_t denom = RC_CUBIC_C;
            if (denom > 0) {
                cubic->k = icbrt(numerator / denom);
            }

            syslog(LOG_DEBUG, "cubic: exiting slow start, cwnd=%u ssthresh=%u",
                   cubic->cwnd, cubic->ssthresh);
        }
        break;

    case RC_CC_CONGESTION_AVOIDANCE: {
        /*
         * CUBIC congestion avoidance.
         *
         * 1. Compute target: W_cubic = C*(t-K)^3 + W_max
         * 2. Compute TCP-friendly target: W_tcp = W_max*beta + 3*(1-beta)/(1+beta) * t
         * 3. cwnd = max(W_cubic, W_tcp) if W_cubic < W_max
         *         = W_cubic              otherwise
         */
        cubic->t_sec++;

        uint32_t w_cubic = cubic_eval(cubic);

        /* TCP-friendly Reno target */
        uint32_t w_tcp = (cubic->w_max * RC_CUBIC_BETA / RC_CUBIC_BETA_SCALE) +
                         (3 * (RC_CUBIC_BETA_SCALE - RC_CUBIC_BETA) * cubic->t_sec) /
                         ((RC_CUBIC_BETA_SCALE + RC_CUBIC_BETA) * RC_CUBIC_BETA_SCALE);
        w_tcp *= cubic->mss;

        uint32_t target = (w_cubic > w_tcp) ? w_cubic : w_tcp;

        /* Grow cwnd toward target by at most 1 MSS per ACK */
        if (cubic->cwnd < target) {
            uint32_t increment = cubic->mss * bytes_acked / cubic->cwnd;
            if (increment == 0) increment = 1;
            cubic->cwnd += increment;
        } else {
            /* Above target: grow slowly (1 MSS per RTT) */
            uint32_t increment = cubic->mss * bytes_acked / cubic->cwnd / 2;
            if (increment == 0) increment = 1;
            cubic->cwnd += increment;
        }

        break;
    }

    case RC_CC_FAST_RECOVERY:
        /*
         * Fast recovery: inflate cwnd by bytes_acked (like TCP).
         * Exit when all lost data is retransmitted.
         */
        cubic->cwnd += bytes_acked;
        break;
    }

    /* Update pacing rate: cwnd / srtt */
    if (cubic->srtt > 0) {
        cubic->pacing_rate = (uint32_t)(
            ((uint64_t)cubic->cwnd * 1000) / cubic->srtt);
    }
}

/* ------------------------------------------------------------------ */

void rc_cubic_on_loss(rc_cubic_t *cubic)
{
    if (!cubic) return;

    cubic->recovery_episodes++;

    /* Fast convergence: if W_max < previous W_max, reduce W_max further */
    if (cubic->w_max > 0 && cubic->cwnd < cubic->w_max) {
        /* W_max = cwnd * (1 + beta) / 2  (fast convergence) */
        cubic->w_max = (uint32_t)(
            (uint64_t)cubic->cwnd * (RC_CUBIC_BETA_SCALE + RC_CUBIC_FC_BETA) /
            (2 * RC_CUBIC_FC_SCALE));
    } else {
        cubic->w_max = cubic->cwnd;
    }

    /* Multiplicative decrease: cwnd *= beta */
    cubic->cwnd_prior = cubic->cwnd;
    cubic->cwnd = (uint32_t)(
        (uint64_t)cubic->cwnd * RC_CUBIC_BETA / RC_CUBIC_BETA_SCALE);

    if (cubic->cwnd < RC_CUBIC_MIN_CWND) {
        cubic->cwnd = RC_CUBIC_MIN_CWND;
    }

    cubic->ssthresh = cubic->cwnd;

    /* Compute new K */
    cubic->t_sec = 0;
    uint64_t w_scaled = (uint64_t)cubic->w_max * RC_CUBIC_BETA_SCALE;
    uint64_t numerator = w_scaled * (RC_CUBIC_BETA_SCALE - RC_CUBIC_BETA);
    uint64_t denom = RC_CUBIC_C;
    if (denom > 0) {
        cubic->k = icbrt(numerator / denom);
    }

    /* Enter fast recovery */
    cubic->state = RC_CC_FAST_RECOVERY;
    cubic->in_slow_start = false;

    syslog(LOG_DEBUG, "cubic: loss event, cwnd=%u ssthresh=%u W_max=%u K=%u",
           cubic->cwnd, cubic->ssthresh, cubic->w_max, cubic->k);
}

/* ------------------------------------------------------------------ */

void rc_cubic_update_rtt(rc_cubic_t *cubic, uint32_t rtt_ms)
{
    if (!cubic || rtt_ms == 0) return;

    /* Track minimum RTT */
    if (rtt_ms < cubic->min_rtt) {
        cubic->min_rtt = rtt_ms;
    }

    if (cubic->srtt == 0) {
        /* First measurement */
        cubic->srtt = rtt_ms;
        cubic->rtt_var = rtt_ms / 2;
    } else {
        /* TCP-like EWMA:
         * RTTVAR = (1 - 1/4) * RTTVAR + 1/4 * |SRTT - R|
         * SRTT   = (1 - 1/8) * SRTT   + 1/8 * R
         */
        int32_t delta = (int32_t)cubic->srtt - (int32_t)rtt_ms;
        if (delta < 0) delta = -delta;

        cubic->rtt_var = (3 * cubic->rtt_var + (uint32_t)delta) / 4;
        cubic->srtt = (7 * cubic->srtt + rtt_ms) / 8;
    }

    /* RTO = SRTT + max(G, 4 * RTTVAR), clamped to [200, 60000] */
    uint32_t rto = cubic->srtt + 4 * cubic->rtt_var;
    if (rto < RC_CUBIC_MIN_CWND) rto = 200;   /* min RTO */
    if (rto > 60000) rto = 60000;              /* max RTO */
    cubic->rto = rto;
}

/* ------------------------------------------------------------------ */

const char *rc_cc_state_name(rc_cc_state_t state)
{
    switch (state) {
    case RC_CC_SLOW_START:              return "SLOW_START";
    case RC_CC_CONGESTION_AVOIDANCE:    return "CONGESTION_AVOIDANCE";
    case RC_CC_FAST_RECOVERY:           return "FAST_RECOVERY";
    default:                            return "UNKNOWN";
    }
}
