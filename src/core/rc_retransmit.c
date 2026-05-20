#include <stdbool.h>
/**
 * @file rc_retransmit.c
 * @brief Retransmission engine with SACK-based loss detection.
 *
 * Implements:
 *   - RTO calculation: RTO = SRTT + max(G, 4 * RTTVAR)
 *   - Fast retransmit: 3 duplicate ACKs trigger immediate retransmit
 *   - SACK-based selective retransmission
 *   - Retransmission queue management
 */

#include "rc_conn.h"
#include "rc_proto.h"
#include "rc_internal.h"

#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/time.h>

/* ================================================================== */
/*  Retransmission queue                                               */
/* ================================================================== */

/** Maximum retransmission queue depth per connection. */
#define RC_RETX_QUEUE_MAX   4096

/**
 * @brief Entry in the retransmission queue.
 *
 * Stores a copy of the sent packet so it can be retransmitted.
 */
typedef struct rc_retx_entry {
    uint32_t    seq;            /**< Sequence number of this packet   */
    uint32_t    sent_ts_ms;     /**< Timestamp when sent (ms)         */
    uint32_t    rto_ms;         /**< RTO at time of sending           */
    uint16_t    payload_len;    /**< Payload length                   */
    bool        acked;          /**< True if acknowledged             */
    bool        sacked;         /**< True if SACKed                   */
    uint8_t    *pkt_data;       /**< Copy of the full packet          */
    uint16_t    pkt_len;        /**< Total packet length              */
} rc_retx_entry_t;

/**
 * @brief Retransmission state per connection.
 */
typedef struct rc_retx_state {
    rc_retx_entry_t entries[RC_RETX_QUEUE_MAX];  /**< Circular buffer */
    uint32_t head;          /**< Oldest unacked entry                 */
    uint32_t tail;          /**< Next free slot                       */
    uint32_t count;         /**< Number of entries in queue           */

    /* --- Duplicate ACK tracking --- */
    uint32_t dup_ack_count; /**< Consecutive duplicate ACKs           */
    uint32_t dup_ack_seq;   /**< Sequence being dup-acked             */

    /* --- SACK state --- */
    rc_sack_block_t sack_blocks[RC_MAX_SACK_BLOCKS]; /**< Received SACKs */
    uint32_t        sack_count;                        /**< SACK count     */

    /* --- Statistics --- */
    uint32_t fast_retransmits;   /**< Fast retransmit count          */
    uint32_t timeout_retransmits; /**< Timeout retransmit count       */
} rc_retx_state_t;

/* ================================================================== */
/*  RTO calculation                                                    */
/* ================================================================== */

/**
 * @brief Calculate RTO from smoothed RTT and variance.
 *
 * Standard TCP formula:
 *   RTO = SRTT + max(G, 4 * RTTVAR)
 *
 * Clamped to [RC_MIN_RTO_MS, RC_MAX_RTO_MS].
 *
 * @param srtt_ms    Smoothed RTT in milliseconds.
 * @param rttvar_ms  RTT variance in milliseconds.
 * @return RTO in milliseconds.
 */
uint32_t rc_calc_rto(uint32_t srtt_ms, uint32_t rttvar_ms)
{
    uint32_t rto = srtt_ms + (rttvar_ms > 50 ? 4 * rttvar_ms : 200);
    if (rto < RC_MIN_RTO_MS) rto = RC_MIN_RTO_MS;
    if (rto > RC_MAX_RTO_MS) rto = RC_MAX_RTO_MS;
    return rto;
}

/* ================================================================== */
/*  Retransmission queue operations                                    */
/* ================================================================== */

/**
 * @brief Allocate and initialise retransmission state for a connection.
 * @return Pointer to retransmission state, or NULL on failure.
 */
rc_retx_state_t *rc_retx_create(void)
{
    rc_retx_state_t *state = (rc_retx_state_t *)calloc(1, sizeof(*state));
    if (!state) {
        syslog(LOG_ERR, "rc_retransmit: alloc failed");
        return NULL;
    }
    return state;
}

/**
 * @brief Free retransmission state and all queued packet copies.
 * @param state  Retransmission state.
 */
void rc_retx_destroy(rc_retx_state_t *state)
{
    if (!state) return;
    for (uint32_t i = 0; i < RC_RETX_QUEUE_MAX; i++) {
        if (state->entries[i].pkt_data) {
            free(state->entries[i].pkt_data);
        }
    }
    free(state);
}

/**
 * @brief Enqueue a packet for potential retransmission.
 *
 * @param state       Retransmission state.
 * @param seq         Packet sequence number.
 * @param pkt_data    Full packet bytes (header + payload).
 * @param pkt_len     Total packet length.
 * @param rto_ms      Current RTO value.
 * @return 0 on success, -1 if queue is full.
 */
int rc_retx_enqueue(rc_retx_state_t *state, uint32_t seq,
                    const uint8_t *pkt_data, uint16_t pkt_len,
                    uint32_t rto_ms)
{
    if (state->count >= RC_RETX_QUEUE_MAX) {
        syslog(LOG_WARNING, "rc_retransmit: queue full (seq=%u)", seq);
        return -1;
    }

    rc_retx_entry_t *e = &state->entries[state->tail % RC_RETX_QUEUE_MAX];
    e->seq = seq;
    e->sent_ts_ms = (uint32_t)(rc_time_ms() & 0xFFFFFFFF);
    e->rto_ms = rto_ms;
    e->acked = false;
    e->sacked = false;
    e->pkt_len = pkt_len;

    /* Copy packet data */
    if (e->pkt_data) free(e->pkt_data);
    e->pkt_data = (uint8_t *)malloc(pkt_len);
    if (!e->pkt_data) return -1;
    memcpy(e->pkt_data, pkt_data, pkt_len);

    state->tail++;
    state->count++;
    return 0;
}

/* ================================================================== */
/*  ACK processing (cumulative + SACK)                                 */
/* ================================================================== */

/**
 * @brief Process a cumulative ACK.
 *
 * Marks all entries with seq <= ack_seq as acknowledged.
 *
 * @param state    Retransmission state.
 * @param ack_seq  Cumulative ACK sequence number.
 * @return Number of entries newly acknowledged.
 */
int rc_retx_process_ack(rc_retx_state_t *state, uint32_t ack_seq)
{
    int acked = 0;
    uint32_t idx = state->head;

    for (uint32_t i = 0; i < state->count; i++) {
        rc_retx_entry_t *e = &state->entries[idx % RC_RETX_QUEUE_MAX];
        if (!e->acked && e->seq <= ack_seq) {
            e->acked = true;
            acked++;
        }
        idx++;
    }

    /* Drain acknowledged entries from the head */
    while (state->count > 0) {
        rc_retx_entry_t *e = &state->entries[state->head % RC_RETX_QUEUE_MAX];
        if (e->acked) {
            free(e->pkt_data);
            e->pkt_data = NULL;
            state->head++;
            state->count--;
        } else {
            break;
        }
    }

    /* Track duplicate ACKs for fast retransmit */
    if (acked == 0 && state->count > 0) {
        if (ack_seq == state->dup_ack_seq) {
            state->dup_ack_count++;
        } else {
            state->dup_ack_seq = ack_seq;
            state->dup_ack_count = 1;
        }
    } else {
        state->dup_ack_count = 0;
    }

    return acked;
}

/**
 * @brief Process SACK blocks from an ACK packet.
 *
 * Marks entries within SACK ranges as selectively acknowledged.
 *
 * @param state   Retransmission state.
 * @param blocks  Array of SACK blocks.
 * @param count   Number of SACK blocks.
 * @return Number of entries newly SACKed.
 */
int rc_retx_process_sack(rc_retx_state_t *state,
                         const rc_sack_block_t *blocks, uint32_t count)
{
    if (count > RC_MAX_SACK_BLOCKS) count = RC_MAX_SACK_BLOCKS;

    int sacked = 0;

    /* Store SACK blocks for loss detection */
    memcpy(state->sack_blocks, blocks, count * sizeof(rc_sack_block_t));
    state->sack_count = count;

    uint32_t idx = state->head;
    for (uint32_t i = 0; i < state->count; i++) {
        rc_retx_entry_t *e = &state->entries[idx % RC_RETX_QUEUE_MAX];
        if (e->acked || e->sacked) {
            idx++;
            continue;
        }

        /* Check if this seq falls within any SACK block */
        for (uint32_t j = 0; j < count; j++) {
            if (e->seq >= blocks[j].left_edge &&
                e->seq < blocks[j].right_edge) {
                e->sacked = true;
                sacked++;
                break;
            }
        }
        idx++;
    }

    return sacked;
}

/* ================================================================== */
/*  Loss detection                                                     */
/* ================================================================== */

/**
 * @brief Check for packets that need retransmission.
 *
 * Two triggers:
 *   1. Fast retransmit: ≥3 duplicate ACKs (or equivalent SACK holes).
 *   2. Timeout: oldest unacked packet exceeds its RTO.
 *
 * @param state        Retransmission state.
 * @param now_ms       Current time in milliseconds.
 * @param[out] retrans Array to fill with sequence numbers to retransmit.
 * @param max_retrans  Maximum entries in retrans array.
 * @return Number of packets to retransmit.
 */
int rc_retx_detect_loss(rc_retx_state_t *state, uint64_t now_ms,
                        uint32_t *retrans, int max_retrans)
{
    int count = 0;

    /* --- Fast retransmit (3 dup ACKs) --- */
    if (state->dup_ack_count >= RC_DUP_ACK_THRESH) {
        /* Retransmit the packet just after the cumulative ACK */
        uint32_t idx = state->head;
        for (uint32_t i = 0; i < state->count && count < max_retrans; i++) {
            rc_retx_entry_t *e = &state->entries[idx % RC_RETX_QUEUE_MAX];
            if (!e->acked && !e->sacked && e->seq > state->dup_ack_seq) {
                retrans[count++] = e->seq;
                state->fast_retransmits++;
                break;  /* retransmit one packet per fast-retransmit event */
            }
            idx++;
        }
        state->dup_ack_count = 0;  /* reset after triggering */
    }

    /* --- SACK-based loss detection --- */
    /*
     * If there's a "hole" in the SACK blocks, the missing packets
     * are likely lost.  A hole exists when:
     *   - We have SACKed data beyond an un-SACKed packet, AND
     *   - The un-SACKed packet has been in the queue long enough (≥ RTO/2).
     */
    if (state->sack_count > 0) {
        uint32_t idx = state->head;
        for (uint32_t i = 0; i < state->count && count < max_retrans; i++) {
            rc_retx_entry_t *e = &state->entries[idx % RC_RETX_QUEUE_MAX];
            if (!e->acked && !e->sacked) {
                /* Check if a SACK block exists beyond this seq */
                bool sack_beyond = false;
                for (uint32_t j = 0; j < state->sack_count; j++) {
                    if (state->sack_blocks[j].left_edge > e->seq) {
                        sack_beyond = true;
                        break;
                    }
                }
                if (sack_beyond) {
                    uint32_t age_ms = (uint32_t)(now_ms - e->sent_ts_ms);
                    if (age_ms >= e->rto_ms / 2) {
                        retrans[count++] = e->seq;
                    }
                }
            }
            idx++;
        }
    }

    /* --- Timeout retransmit --- */
    uint32_t idx = state->head;
    for (uint32_t i = 0; i < state->count && count < max_retrans; i++) {
        rc_retx_entry_t *e = &state->entries[idx % RC_RETX_QUEUE_MAX];
        if (!e->acked && !e->sacked) {
            uint32_t age_ms = (uint32_t)(now_ms - e->sent_ts_ms);
            if (age_ms >= e->rto_ms) {
                /* Avoid duplicate with fast retransmit */
                bool already = false;
                for (int j = 0; j < count; j++) {
                    if (retrans[j] == e->seq) { already = true; break; }
                }
                if (!already) {
                    retrans[count++] = e->seq;
                    state->timeout_retransmits++;
                    /* Exponential backoff on timeout */
                    e->rto_ms = (e->rto_ms < RC_MAX_RTO_MS / 2)
                                    ? e->rto_ms * 2
                                    : RC_MAX_RTO_MS;
                    e->sent_ts_ms = (uint32_t)(now_ms & 0xFFFFFFFF);
                }
            }
        }
        idx++;
    }

    return count;
}

/* ================================================================== */
/*  Convenience: mark packet sent and enqueue                          */
/* ================================================================== */

/**
 * @brief Record a packet as sent and enqueue it for retransmission.
 *
 * @param conn      Connection (uses conn->cubic.rto).
 * @param retx      Retransmission state (stored in conn user_data or similar).
 * @param seq       Packet sequence number.
 * @param pkt_data  Full packet bytes.
 * @param pkt_len   Packet length.
 * @return 0 on success, -1 on failure.
 */
int rc_retransmit_record_sent(rc_conn_t *conn, rc_retx_state_t *retx,
                              uint32_t seq, const uint8_t *pkt_data,
                              uint16_t pkt_len)
{
    if (!conn || !retx) return -1;

    /* Only enqueue reliable packets */
    rc_pkt_hdr_t hdr;
    if (pkt_len >= RC_HEADER_SIZE) {
        rc_hdr_unpack(&hdr, pkt_data);
        if (!(hdr.flags & RC_FLAG_RELIABLE)) {
            return 0;  /* unreliable — no retransmit */
        }
    }

    return rc_retx_enqueue(retx, seq, pkt_data, pkt_len, conn->cubic.rto);
}
