/*
 * robocontrol/congestion.h - CUBIC congestion control (RFC 8312)
 */
#ifndef ROBOCONTROL_CONGESTION_H
#define ROBOCONTROL_CONGESTION_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── CUBIC constants ────────────────────────────────────────── */
#define RC_CUBIC_BETA         0.7     /* multiplicative decrease factor */
#define RC_CUBIC_C            0.4     /* scaling constant */
#define RC_CUBIC_MIN_CWND     4       /* minimum cwnd in MSS */
#define RC_CUBIC_MAX_CWND     65535   /* maximum cwnd in MSS */
#define RC_CUBIC_MSS          1460    /* default MSS */
#define RC_CUBIC_SSTHRESH_MAX 0x7FFFFFFF

/* ── Congestion state ───────────────────────────────────────── */
typedef enum {
    RC_CUBIC_SLOW_START = 0,
    RC_CUBIC_CONGESTION_AVOIDANCE,
    RC_CUBIC_RECOVERY,
} rc_cubic_state_t;

/* ── CUBIC context ──────────────────────────────────────────── */
typedef struct {
    rc_cubic_state_t state;
    double   cwnd;           /* congestion window (in MSS units) */
    double   ssthresh;       /* slow start threshold */
    double   w_max;          /* cwnd before last reduction */
    double   k;              /* time period for cwnd to grow to w_max */
    double   w_rtcp;         /* cwnd at start of congestion avoidance */
    int64_t  t_ca_start;     /* time entering congestion avoidance (ms) */
    int64_t  recovery_start; /* time entering recovery (ms) */
    uint32_t ack_count;
    uint32_t loss_count;
    uint32_t rtt_ms;         /* smoothed RTT estimate */
    uint32_t rtt_var;        /* RTT variance */
    uint32_t rtt_min;        /* minimum RTT observed */
    uint32_t mss;
    /* Stats */
    rc_atomic64_t total_acked;
    rc_atomic64_t total_lost;
    rc_atomic64_t total_retrans;
} rc_cubic_t;

static inline void rc_cubic_init(rc_cubic_t *cubic, uint32_t mss) {
    cubic->state = RC_CUBIC_SLOW_START;
    cubic->mss = mss ? mss : RC_CUBIC_MSS;
    cubic->cwnd = (double)RC_CUBIC_MIN_CWND;
    cubic->ssthresh = (double)RC_CUBIC_SSTHRESH_MAX;
    cubic->w_max = (double)RC_CUBIC_MAX_CWND;
    cubic->k = 0.0;
    cubic->w_rtcp = 0.0;
    cubic->t_ca_start = 0;
    cubic->recovery_start = 0;
    cubic->ack_count = 0;
    cubic->loss_count = 0;
    cubic->rtt_ms = 100;
    cubic->rtt_var = 50;
    cubic->rtt_min = UINT32_MAX;
    rc_atomic_init(&cubic->total_acked, 0);
    rc_atomic_init(&cubic->total_lost, 0);
    rc_atomic_init(&cubic->total_retrans, 0);
}

static inline void rc_cubic_update_rtt(rc_cubic_t *cubic, uint32_t measured_rtt) {
    if (measured_rtt == 0) measured_rtt = 1;
    if (measured_rtt < cubic->rtt_min) cubic->rtt_min = measured_rtt;

    /* Jacobson/Karels algorithm */
    int32_t delta = (int32_t)cubic->rtt_ms - (int32_t)measured_rtt;
    if (delta < 0) delta = -delta;
    cubic->rtt_var = (3 * cubic->rtt_var + (uint32_t)delta) / 4;
    cubic->rtt_ms = (7 * cubic->rtt_ms + measured_rtt) / 8;
    if (cubic->rtt_ms == 0) cubic->rtt_ms = 1;
}

/* CUBIC window calculation: W(t) = C * (t - K)^3 + W_max */
static inline double rc_cubic_cwnd(rc_cubic_t *cubic) {
    int64_t now = rc_time_ms();
    double t = (double)(now - cubic->t_ca_start) / 1000.0; /* seconds */
    double elapsed = t - cubic->k;
    double w_cubic = RC_CUBIC_C * elapsed * elapsed * elapsed + cubic->w_max;
    return MAX(w_cubic, (double)RC_CUBIC_MIN_CWND);
}

static inline void rc_cubic_on_ack(rc_cubic_t *cubic, uint32_t bytes_acked) {
    rc_atomic_add(&cubic->total_acked, (int64_t)bytes_acked);
    cubic->ack_count++;

    switch (cubic->state) {
    case RC_CUBIC_SLOW_START:
        /* Increase cwnd by 1 MSS per ACK (exponential growth) */
        cubic->cwnd += (double)cubic->mss;
        if (cubic->cwnd >= cubic->ssthresh) {
            cubic->state = RC_CUBIC_CONGESTION_AVOIDANCE;
            cubic->w_max = cubic->cwnd;
            cubic->w_rtcp = cubic->cwnd;
            /* k = cbrt(w_max * (1 - beta) / C) */
            double target = cubic->w_max * (1.0 - RC_CUBIC_BETA) / RC_CUBIC_C;
            cubic->k = (target > 0) ? pow(target, 1.0 / 3.0) : 0.0;
            cubic->t_ca_start = rc_time_ms();
        }
        break;

    case RC_CUBIC_CONGESTION_AVOIDANCE: {
        /* AIMD: TCP-friendly increase */
        double w_est = cubic->w_rtcp +
            (3.0 * (1.0 - RC_CUBIC_BETA) / (1.0 + RC_CUBIC_BETA))
            * ((double)(rc_time_ms() - cubic->t_ca_start) / 1000.0)
            / cubic->rtt_ms * 1000.0 * cubic->mss;

        double w_cubic = rc_cubic_cwnd(cubic);

        if (w_cubic < w_est) {
            cubic->cwnd = w_est;
        } else {
            cubic->cwnd = w_cubic;
        }
        if (cubic->cwnd > (double)RC_CUBIC_MAX_CWND)
            cubic->cwnd = (double)RC_CUBIC_MAX_CWND;
        break;
    }

    case RC_CUBIC_RECOVERY:
        /* Stay in recovery until cwnd reaches pre-loss level */
        if (cubic->cwnd >= cubic->w_max * RC_CUBIC_BETA) {
            cubic->state = RC_CUBIC_CONGESTION_AVOIDANCE;
            cubic->t_ca_start = rc_time_ms();
            cubic->w_rtcp = cubic->cwnd;
        }
        break;
    }
}

static inline void rc_cubic_on_loss(rc_cubic_t *cubic) {
    rc_atomic_inc(&cubic->total_lost);
    cubic->loss_count++;

    switch (cubic->state) {
    case RC_CUBIC_SLOW_START:
    case RC_CUBIC_CONGESTION_AVOIDANCE:
        cubic->w_max = cubic->cwnd;
        cubic->ssthresh = cubic->cwnd * RC_CUBIC_BETA;
        if (cubic->ssthresh < (double)RC_CUBIC_MIN_CWND)
            cubic->ssthresh = (double)RC_CUBIC_MIN_CWND;
        cubic->cwnd = cubic->ssthresh;
        cubic->w_rtcp = cubic->cwnd;
        /* k = cbrt(w_max * (1 - beta) / C) */
        {
            double target = cubic->w_max * (1.0 - RC_CUBIC_BETA) / RC_CUBIC_C;
            cubic->k = (target > 0) ? pow(target, 1.0 / 3.0) : 0.0;
        }
        cubic->state = RC_CUBIC_RECOVERY;
        cubic->recovery_start = rc_time_ms();
        break;

    case RC_CUBIC_RECOVERY:
        /* Already recovering, further reduce */
        cubic->cwnd *= RC_CUBIC_BETA;
        if (cubic->cwnd < (double)RC_CUBIC_MIN_CWND)
            cubic->cwnd = (double)RC_CUBIC_MIN_CWND;
        break;
    }
}

static inline void rc_cubic_on_retrans(rc_cubic_t *cubic) {
    rc_atomic_inc(&cubic->total_retrans);
}

static inline uint32_t rc_cubic_get_cwnd(rc_cubic_t *cubic) {
    return (uint32_t)MAX(cubic->cwnd, (double)RC_CUBIC_MIN_CWND);
}

static inline rc_cubic_state_t rc_cubic_get_state(rc_cubic_t *cubic) {
    return cubic->state;
}

#ifdef __cplusplus
}
#endif

#endif /* ROBOCONTROL_CONGESTION_H */
