/**
 * @file rc_internal.h
 * @brief Internal function prototypes — shared across core translation units.
 */

#ifndef RC_INTERNAL_H
#define RC_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "rc_conn.h"
#include "rc_proto.h"

/* Forward declarations for opaque types used in prototypes */
typedef struct rc_retx_state rc_retx_state_t;
typedef struct rc_p2p_state  rc_p2p_state_t;

/* Provided by rc_epoll.c or common.h */
static inline int64_t rc_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- rc_retransmit.c ------------------------------------------------ */
uint32_t rc_calc_rto(uint32_t srtt_ms, uint32_t rttvar_ms);
rc_retx_state_t *rc_retx_create(void);
void     rc_retx_destroy(rc_retx_state_t *state);
int      rc_retx_enqueue(rc_retx_state_t *state, uint32_t seq,
                         const uint8_t *pkt_data, uint16_t pkt_len,
                         uint32_t rto_ms);
int      rc_retx_process_ack(rc_retx_state_t *state, uint32_t ack_seq);
int      rc_retx_process_sack(rc_retx_state_t *state,
                              const rc_sack_block_t *blocks, uint32_t count);
int      rc_retx_detect_loss(rc_retx_state_t *state, uint64_t now_ms,
                             uint32_t *lost_seq, int max_lost);
int      rc_retransmit_record_sent(rc_conn_t *conn, rc_retx_state_t *retx,
                                   uint32_t seq, const uint8_t *data,
                                   uint16_t len);

/* ---- rc_p2p.c ------------------------------------------------------- */
typedef struct rc_candidate  rc_candidate_t;
typedef struct rc_addr       rc_addr_t;

rc_p2p_state_t *rc_p2p_create(void);
void     rc_p2p_destroy(rc_p2p_state_t *p2p);
int      rc_p2p_set_stun(rc_p2p_state_t *p2p, const char *host, uint16_t port);
uint32_t rc_p2p_probe(rc_p2p_state_t *p2p, const rc_candidate_t *candidate,
                      uint32_t timeout_ms);
int      rc_p2p_fallback_relay(rc_conn_t *conn, rc_p2p_state_t *p2p);
bool     rc_p2p_tick(rc_p2p_state_t *p2p, const rc_addr_t *peer_addr);

#endif /* RC_INTERNAL_H */
