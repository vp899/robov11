/**
 * @file transport_mode.h
 * @brief Transport mode configuration for RoboControl integration tests.
 *
 * Supports two modes:
 *   - RELIABLE:    100% delivery guarantee via ACK/SACK retransmission.
 *                  Higher latency, no data loss. Best for control commands.
 *   - REALTIME:    Best-effort delivery, no retransmission.
 *                  Lowest latency, tolerates packet loss. Best for video streams.
 */

#ifndef TRANSPORT_MODE_H
#define TRANSPORT_MODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ── Transport modes ──────────────────────────────────────────────── */

typedef enum {
    TRANSPORT_RELIABLE  = 0,   /**< ACK-based retransmission, 100% delivery */
    TRANSPORT_REALTIME  = 1,   /**< Fire-and-forget, lowest latency        */
} transport_mode_t;

static inline const char *transport_mode_name(transport_mode_t m)
{
    switch (m) {
    case TRANSPORT_RELIABLE: return "RELIABLE";
    case TRANSPORT_REALTIME: return "REALTIME";
    default:                 return "UNKNOWN";
    }
}

/* ── Per-mode tuning parameters ───────────────────────────────────── */

typedef struct {
    transport_mode_t mode;

    /* Retransmission */
    bool     enable_retransmit;     /**< Retransmit lost packets?          */
    uint32_t retx_timeout_ms;       /**< Base retransmit timeout (ms)      */
    uint32_t max_retx;              /**< Max retransmit attempts per pkt   */
    uint32_t dup_ack_thresh;        /**< Duplicate ACKs for fast retx      */

    /* SACK */
    bool     enable_sack;           /**< Selective ACK support?            */

    /* Congestion control */
    bool     enable_congestion;     /**< CUBIC congestion control?         */

    /* Pacing */
    bool     enable_pacing;         /**< Rate-limit sends?                 */

    /* ACK generation (receiver side) */
    bool     send_acks;             /**< Receiver sends ACK packets?       */
    uint32_t ack_interval_ms;       /**< ACK generation interval (ms)      */

    /* Timeout */
    uint32_t recv_timeout_ms;       /**< Receive timeout (ms)              */

} transport_config_t;

/**
 * @brief Get reliable transport configuration.
 *
 * Full retransmission, SACK, congestion control — 100% delivery guarantee.
 */
static inline transport_config_t transport_config_reliable(void)
{
    transport_config_t cfg = {
        .mode              = TRANSPORT_RELIABLE,
        .enable_retransmit = true,
        .retx_timeout_ms   = 200,
        .max_retx          = 20,
        .dup_ack_thresh    = 3,
        .enable_sack       = true,
        .enable_congestion = true,
        .enable_pacing     = true,
        .send_acks         = true,
        .ack_interval_ms   = 50,
        .recv_timeout_ms   = 5000,
    };
    return cfg;
}

/**
 * @brief Get real-time transport configuration.
 *
 * No retransmission, no ACKs — minimum latency for live video.
 */
static inline transport_config_t transport_config_realtime(void)
{
    transport_config_t cfg = {
        .mode              = TRANSPORT_REALTIME,
        .enable_retransmit = false,
        .retx_timeout_ms   = 0,
        .max_retx          = 0,
        .dup_ack_thresh    = 0,
        .enable_sack       = false,
        .enable_congestion = false,
        .enable_pacing     = true,   /* still pace to avoid flooding */
        .send_acks         = false,
        .ack_interval_ms   = 0,
        .recv_timeout_ms   = 3000,
    };
    return cfg;
}

static inline transport_config_t transport_config_get(transport_mode_t mode)
{
    switch (mode) {
    case TRANSPORT_RELIABLE: return transport_config_reliable();
    case TRANSPORT_REALTIME: return transport_config_realtime();
    default:                 return transport_config_realtime();
    }
}

static inline void transport_config_print(const transport_config_t *cfg, FILE *out)
{
    fprintf(out, "  Transport mode:      %s\n", transport_mode_name(cfg->mode));
    fprintf(out, "  Retransmit:          %s\n", cfg->enable_retransmit ? "ON" : "OFF");
    if (cfg->enable_retransmit) {
        fprintf(out, "    Timeout:           %u ms\n", cfg->retx_timeout_ms);
        fprintf(out, "    Max retries:       %u\n", cfg->max_retx);
        fprintf(out, "    Dup ACK thresh:    %u\n", cfg->dup_ack_thresh);
    }
    fprintf(out, "  SACK:                %s\n", cfg->enable_sack ? "ON" : "OFF");
    fprintf(out, "  Congestion control:  %s\n", cfg->enable_congestion ? "ON" : "OFF");
    fprintf(out, "  Pacing:              %s\n", cfg->enable_pacing ? "ON" : "OFF");
    fprintf(out, "  ACK generation:      %s\n", cfg->send_acks ? "ON" : "OFF");
    if (cfg->send_acks)
        fprintf(out, "    ACK interval:      %u ms\n", cfg->ack_interval_ms);
}

#endif /* TRANSPORT_MODE_H */
