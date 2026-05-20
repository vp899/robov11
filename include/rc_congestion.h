/**
 * @file rc_congestion.h
 * @brief CUBIC congestion control (RFC 8312).
 *
 * Implements the CUBIC algorithm for congestion window management.
 * CUBIC is the default TCP congestion control in Linux and provides
 * good throughput over both high-speed and long-delay paths while
 * maintaining fairness.
 *
 * Key state variables:
 *   - cwnd:     Current congestion window (bytes).
 *   - ssthresh: Slow-start threshold.
 *   - W_max:    Window size before the last reduction.
 *   - K:        Time period for the cubic function to grow to W_max.
 *   - t_sec:    Elapsed time since last congestion event.
 */

#ifndef RC_CONGESTION_H
#define RC_CONGESTION_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

/** Initial congestion window in bytes (10 × MSS). */
#define RC_CUBIC_INIT_CWND     (10 * 1460)

/** Minimum congestion window in bytes. */
#define RC_CUBIC_MIN_CWND      (1 * 1460)

/** Default MSS in bytes. */
#define RC_CUBIC_MSS           1460

/** CUBIC beta (multiplicative decrease factor) × 1024 for fixed-point. */
#define RC_CUBIC_BETA_SCALE    1024
#define RC_CUBIC_BETA          717     /* 0.7 * 1024 ≈ 717 */

/** CUBIC scaling constant C × 1024 for fixed-point. */
#define RC_CUBIC_C_SCALE       1024
#define RC_CUBIC_C             4       /* 0.4 × 1024 ≈ 410, simplified */

/** Fast convergence factor × 1024. */
#define RC_CUBIC_FC_SCALE      1024
#define RC_CUBIC_FC_BETA       819     /* 0.8 × 1024 ≈ 819 */

/* ------------------------------------------------------------------ */
/*  Congestion states                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Congestion control state machine.
 */
typedef enum rc_cc_state {
    RC_CC_SLOW_START    = 0,  /**< Exponential growth              */
    RC_CC_CONGESTION_AVOIDANCE = 1,  /**< CUBIC growth            */
    RC_CC_FAST_RECOVERY = 2,  /**< Recovering from loss            */
} rc_cc_state_t;

/* ------------------------------------------------------------------ */
/*  CUBIC state                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief CUBIC congestion control state (per-connection).
 */
typedef struct rc_cubic {
    rc_cc_state_t state;          /**< Current CC state                  */

    uint32_t cwnd;                /**< Congestion window (bytes)         */
    uint32_t ssthresh;            /**< Slow-start threshold              */
    uint32_t cwnd_prior;          /**< cwnd before last reduction        */

    /* --- CUBIC parameters --- */
    uint32_t w_max;               /**< W_max: window at last reduction   */
    uint32_t k;                   /**< K: cubic root time (fixed-point)  */
    uint32_t t_sec;               /**< Time since last event (ticks)     */

    /* --- RTT tracking --- */
    uint32_t srtt;                /**< Smoothed RTT (ms)                 */
    uint32_t rtt_var;             /**< RTT variance (ms)                 */
    uint32_t min_rtt;             /**< Minimum observed RTT (ms)         */
    uint32_t rto;                 /**< Retransmission timeout (ms)       */

    /* --- pacing --- */
    uint32_t pacing_rate;         /**< Pacing rate (bytes/sec)           */

    /* --- statistics --- */
    uint64_t total_acked;         /**< Total bytes acknowledged          */
    uint32_t recovery_episodes;   /**< Number of loss recovery episodes  */
    uint32_t ack_count;           /**< ACKs received in current epoch    */

    /* --- configuration --- */
    uint32_t mss;                 /**< Maximum segment size              */
    bool     in_slow_start;       /**< True if in slow start             */
} rc_cubic_t;

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise CUBIC state with default parameters.
 * @param[out] cubic  CUBIC state to initialise.
 * @param      mss    Maximum segment size (0 = RC_CUBIC_MSS).
 */
void rc_cubic_init(rc_cubic_t *cubic, uint32_t mss);

/**
 * @brief Process an ACK acknowledgement.
 *
 * Updates cwnd according to the CUBIC algorithm.  Called once per
 * received ACK (or SACK block that acknowledges new data).
 *
 * @param cubic         CUBIC state.
 * @param bytes_acked   Number of bytes newly acknowledged.
 * @param rtt_ms        Measured RTT for this ACK (0 if unavailable).
 */
void rc_cubic_on_ack(rc_cubic_t *cubic, uint32_t bytes_acked, uint32_t rtt_ms);

/**
 * @brief Process a loss event.
 *
 * Reduces cwnd using CUBIC's multiplicative decrease and enters
 * fast recovery if appropriate.
 *
 * @param cubic  CUBIC state.
 */
void rc_cubic_on_loss(rc_cubic_t *cubic);

/**
 * @brief Get the current congestion window.
 * @param cubic  CUBIC state.
 * @return cwnd in bytes.
 */
static inline uint32_t rc_cubic_get_cwnd(const rc_cubic_t *cubic)
{
    return cubic->cwnd;
}

/**
 * @brief Get the current pacing rate.
 * @param cubic  CUBIC state.
 * @return Pacing rate in bytes/sec.
 */
static inline uint32_t rc_cubic_get_pacing_rate(const rc_cubic_t *cubic)
{
    return cubic->pacing_rate;
}

/**
 * @brief Update RTT measurements.
 *
 * Applies the TCP-like EWMA smoothing:
 *   SRTT = (7/8) * SRTT + (1/8) * RTT
 *   RTTVAR = (3/4) * RTTVAR + (1/4) * |SRTT - RTT|
 *   RTO = SRTT + max(G, 4 * RTTVAR)
 *
 * @param cubic  CUBIC state.
 * @param rtt_ms  Measured RTT in milliseconds.
 */
void rc_cubic_update_rtt(rc_cubic_t *cubic, uint32_t rtt_ms);

/**
 * @brief Check if the connection is in slow start.
 * @param cubic  CUBIC state.
 * @return True if in slow start.
 */
static inline bool rc_cubic_in_slow_start(const rc_cubic_t *cubic)
{
    return cubic->state == RC_CC_SLOW_START;
}

/**
 * @brief Get a human-readable CC state name.
 * @param state  CC state.
 * @return Static string.
 */
const char *rc_cc_state_name(rc_cc_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* RC_CONGESTION_H */
