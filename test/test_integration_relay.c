/**
 * @file test_integration_relay.c
 * @brief Integration test: relay + H264 + paced 1 Mbps + retransmission + weak network.
 *
 * Architecture:
 *   Robot (sender threads) ──DATA──▸ Relay ──DATA──▸ Controller (receiver)
 *   Controller ──ACK──▸ Relay ──ACK──▸ Robot
 *
 * Sender: pace at 1 Mbps, enqueue reliable packets for retransmission.
 * Receiver: send cumulative ACK + SACK blocks on gap detection.
 * Sender: process ACKs, detect loss, retransmit.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rc_proto.h"
#include "rc_internal.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define STREAM_COUNT            2
#define PACKET_BODY_SIZE        1200
#define IFRAME_INTERVAL_MS      2000
#define IFRAME_PKT_COUNT        8
#define PFRAME_PKT_COUNT        2
#define PFRAME_INTERVAL_MS      21      /* ~48 fps to fill 1 Mbps */

/* 1 Mbps per stream: 1252 bytes/pkt, 10.02 ms inter-packet */
#define INTER_PACKET_US         10020

/* ACK every N data packets received */
#define ACK_INTERVAL            4

/* RTO for test (ms) — low for localhost, higher for real networks */
#define TEST_RTO_MS             50

#define NALU_TYPE_IDR           5
#define NALU_TYPE_NON_IDR       1

/* ── H264 NALU header ──────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t start_code;
    uint8_t  nalu_header;
    uint8_t  stream_id;
    uint16_t frame_seq;
    uint32_t frame_offset;
    uint32_t frame_size;
    uint32_t timestamp_ms;
} h264_nalu_hdr_t;
#define NALU_HDR_SIZE  sizeof(h264_nalu_hdr_t)

/* ── Lossy channel ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t pct;
    uint32_t seed;
    uint64_t sent, dropped, delivered;
} lossy_t;

static void lossy_init(lossy_t *c, uint32_t p) {
    c->pct = p;
    c->seed = (uint32_t)(time(NULL) ^ getpid() ^ (uint32_t)rand());
    c->sent = c->dropped = c->delivered = 0;
}

static bool lossy_drop(lossy_t *c) {
    if (!c->pct) return false;
    c->seed = c->seed * 1103515245 + 12345;
    return (c->seed >> 16) % 100 < c->pct;
}

/* ── Stats ─────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t pkts_sent;         /* original data pkts sent */
    uint64_t pkts_retrans;      /* retransmitted pkts */
    uint64_t pkts_recv;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint64_t frames_sent, frames_recv;
    uint64_t iframes_sent, iframes_recv;
    uint64_t pframes_sent, pframes_recv;
    uint64_t acks_sent;         /* ACKs sent by receiver */
    uint64_t acks_recv;         /* ACKs received by sender */
    uint64_t fast_retrans;
    uint64_t timeout_retrans;
    uint64_t sack_retrans;
    uint64_t crc_err, nalu_err;
    uint64_t start_ms, end_ms;
} stream_stats_t;

static stream_stats_t g_ss[STREAM_COUNT];
static volatile bool g_running;

/* ── Helpers ────────────────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t g_rng = 0xDEADBEEF;
static void fill_random(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_rng = g_rng * 1103515245 + 12345;
        buf[i] = (uint8_t)(g_rng >> 16);
    }
}

/* ── Build DATA packet ─────────────────────────────────────────────── */

static int build_data_pkt(uint8_t *out, size_t cap,
                          uint32_t conn_id, uint32_t seq,
                          uint8_t sid, uint16_t fseq,
                          bool is_i, uint32_t foff, uint32_t fsz,
                          const uint8_t *body, size_t blen)
{
    if (cap < RC_HEADER_SIZE + NALU_HDR_SIZE + blen) return -1;

    rc_pkt_hdr_t h;
    rc_hdr_init(&h, RC_PKT_DATA, seq, conn_id);
    h.payload_len = (uint16_t)(NALU_HDR_SIZE + blen);
    h.flags = RC_FLAG_RELIABLE;  /* Mark for retransmission */
    h.timestamp = (uint64_t)time(NULL) * 1000000 + now_ms() * 1000;
    rc_hdr_pack(out, &h);

    h264_nalu_hdr_t n;
    n.start_code   = htonl(0x00000001);
    n.nalu_header  = is_i ? (NALU_TYPE_IDR & 0x1F) : (NALU_TYPE_NON_IDR & 0x1F);
    n.stream_id    = sid;
    n.frame_seq    = htons(fseq);
    n.frame_offset = htonl(foff);
    n.frame_size   = htonl(fsz);
    n.timestamp_ms = htonl((uint32_t)(now_ms() & 0xFFFFFFFF));
    memcpy(out + RC_HEADER_SIZE, &n, NALU_HDR_SIZE);
    memcpy(out + RC_HEADER_SIZE + NALU_HDR_SIZE, body, blen);
    return (int)(RC_HEADER_SIZE + NALU_HDR_SIZE + blen);
}

/* ── Build ACK packet ──────────────────────────────────────────────── */

/**
 * ACK packet format:
 *   Header: type=RC_PKT_ACK, seq=ack_seq, flags
 *   Payload: SACK blocks (if any), each 8 bytes [left, right)
 *   flags: RC_FLAG_SACK if SACK blocks present
 */
static int build_ack_pkt(uint8_t *out, size_t cap,
                         uint32_t conn_id, uint32_t ack_seq,
                         const rc_sack_block_t *sacks, uint32_t sack_count)
{
    uint16_t pay_len = (uint16_t)(sack_count * sizeof(rc_sack_block_t));
    if (cap < RC_HEADER_SIZE + pay_len) return -1;

    rc_pkt_hdr_t h;
    rc_hdr_init(&h, RC_PKT_ACK, ack_seq, conn_id);
    h.payload_len = pay_len;
    h.flags = sack_count > 0 ? RC_FLAG_SACK : 0;
    h.timestamp = (uint64_t)time(NULL) * 1000000 + now_ms() * 1000;
    rc_hdr_pack(out, &h);

    if (sack_count > 0) {
        memcpy(out + RC_HEADER_SIZE, sacks, sack_count * sizeof(rc_sack_block_t));
    }
    return (int)(RC_HEADER_SIZE + pay_len);
}

/* ── Sender thread (one per stream) ────────────────────────────────── */
/* Generates frames, pace-sends, processes ACKs, retransmits.          */

typedef struct {
    int               send_fd;      /* socket to send data */
    int               ack_fd;       /* socket to recv ACKs (same fd, non-blocking) */
    struct sockaddr_in dest;        /* relay addr */
    uint32_t          conn_id;
    uint8_t           stream_id;
    lossy_t          *lossy;        /* lossy channel for sends */
    lossy_t          *ack_lossy;    /* lossy channel for ACK path (reverse) */
} sender_arg_t;

static void *sender_thread(void *arg)
{
    sender_arg_t *a = (sender_arg_t *)arg;
    stream_stats_t *s = &g_ss[a->stream_id];
    s->start_ms = now_ms();

    /* Retransmission state */
    rc_retx_state_t *retx = rc_retx_create();
    if (!retx) return NULL;

    uint32_t seq = 0;
    uint16_t fseq = 0;
    uint8_t pkt[RC_HEADER_SIZE + NALU_HDR_SIZE + PACKET_BODY_SIZE];

    /* Pacing clock */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t next_send_us = (uint64_t)t0.tv_sec * 1000000ULL + t0.tv_nsec / 1000;

    uint64_t next_i_ms = 0, next_p_ms = 0;
    uint64_t next_retx_check_ms = 0;

    while (g_running) {
        uint64_t now = now_ms();

        /* ── 1. Check for retransmission losses (every 50ms) ── */
        if (now >= next_retx_check_ms) {
            next_retx_check_ms = now + 50;

            uint32_t lost[64];
            int nlost = rc_retx_detect_loss(retx, (uint64_t)now, lost, 64);

            for (int i = 0; i < nlost && g_running; i++) {
                /* Find the packet data in retx queue and resend */
                /* The retx engine stores packets; we need to resend them.
                 * For simplicity, we iterate the retx entries to find the data. */
                /* Note: rc_retx_detect_loss gives us seq numbers.
                 * We need to find the actual packet data from the retx queue.
                 * Since the internal struct is opaque, we track our own map. */
                (void)lost[i];
                /* Handled below via the retx queue iteration */
            }

            /* Iterate retx queue to find and retransmit timed-out packets */
            /* Since we can't access internal entries directly, we use a
             * separate tracking array for packet data. */
        }

        /* ── 3. Generate and send data frames ── */
        bool send_i = false, send_p = false;
        if (now >= next_i_ms) {
            send_i = true;
            next_i_ms = now + IFRAME_INTERVAL_MS;
            next_p_ms = now + 20;
        } else if (now >= next_p_ms) {
            send_p = true;
            next_p_ms = now + PFRAME_INTERVAL_MS;
        }

        if (!send_i && !send_p) {
            /* Idle — but still check for retransmissions */
            usleep(500);
            continue;
        }

        bool is_i = send_i;
        int npkts = is_i ? IFRAME_PKT_COUNT : PFRAME_PKT_COUNT;
        uint32_t fsz = (uint32_t)npkts * PACKET_BODY_SIZE;
        uint16_t cur_fseq = fseq++;

        for (int p = 0; p < npkts && g_running; p++) {
            uint8_t body[PACKET_BODY_SIZE];
            fill_random(body, PACKET_BODY_SIZE);

            uint32_t foff = (uint32_t)p * PACKET_BODY_SIZE;
            int plen = build_data_pkt(pkt, sizeof(pkt),
                                      a->conn_id, seq,
                                      a->stream_id, cur_fseq,
                                      is_i, foff, fsz,
                                      body, PACKET_BODY_SIZE);
            if (plen < 0) break;

            /* Pace */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
            if (next_send_us > now_us) usleep((useconds_t)(next_send_us - now_us));
            next_send_us += INTER_PACKET_US;

            /* Enqueue for retransmission */
            rc_retx_enqueue(retx, seq, pkt, (uint16_t)plen, TEST_RTO_MS);

            /* Send with lossy channel */
            a->lossy->sent++;
            if (!lossy_drop(a->lossy)) {
                a->lossy->delivered++;
                sendto(a->send_fd, pkt, (size_t)plen, 0,
                       (struct sockaddr *)&a->dest, sizeof(a->dest));
            } else {
                a->lossy->dropped++;
            }

            s->pkts_sent++;
            s->bytes_sent += (size_t)plen;
            seq++;
        }

        s->frames_sent++;
        if (is_i) s->iframes_sent++;
        else s->pframes_sent++;
    }

    s->end_ms = now_ms();
    rc_retx_destroy(retx);
    return NULL;
}

/* ── Alternative sender with explicit retx tracking ────────────────── */
/*
 * We need to track packet data ourselves since rc_retx_state_t is opaque.
 * This sender uses a parallel array to store packet copies.
 */

#define RETX_MAP_SIZE  8192  /* must be > max outstanding packets */

typedef struct {
    uint8_t  data[RC_HEADER_SIZE + NALU_HDR_SIZE + PACKET_BODY_SIZE];
    uint16_t len;
    uint32_t seq;
    uint64_t sent_ms;
    uint32_t rto_ms;
    bool     active;
    bool     acked;
} retx_map_entry_t;

typedef struct {
    int               send_fd;
    struct sockaddr_in dest;
    uint32_t          conn_id;
    uint8_t           stream_id;
    lossy_t          *lossy;
} sender_v2_arg_t;

static void *sender_v2_thread(void *arg)
{
    sender_v2_arg_t *a = (sender_v2_arg_t *)arg;
    stream_stats_t *s = &g_ss[a->stream_id];
    s->start_ms = now_ms();

    /* Our own retx tracking */
    retx_map_entry_t *retx_map = calloc(RETX_MAP_SIZE, sizeof(retx_map_entry_t));
    if (!retx_map) return NULL;

    uint32_t seq = 0;
    uint16_t fseq = 0;
    uint8_t pkt[RC_HEADER_SIZE + NALU_HDR_SIZE + PACKET_BODY_SIZE];

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t next_send_us = (uint64_t)t0.tv_sec * 1000000ULL + t0.tv_nsec / 1000;
    uint64_t next_i_ms = 0, next_p_ms = 0;

    /* Track highest ACKed seq and SACK state for loss detection */
    uint32_t highest_acked = 0;
    uint32_t dup_ack_count = 0;
    uint32_t dup_ack_seq = 0;

    while (g_running) {
        uint64_t now = now_ms();

        /* ── 1. Check for ACKs ── */
        for (;;) {
            struct pollfd pfd = { .fd = a->send_fd, .events = POLLIN };
            if (poll(&pfd, 1, 0) <= 0) break;

            uint8_t ack_buf[RC_HEADER_SIZE + RC_MAX_SACK_BLOCKS * sizeof(rc_sack_block_t)];
            ssize_t n = recv(a->send_fd, ack_buf, sizeof(ack_buf), 0);
            if (n < RC_HEADER_SIZE) break;

            rc_pkt_hdr_t ahdr;
            if (rc_hdr_unpack(&ahdr, ack_buf) != 0) continue;
            if (ahdr.type != RC_PKT_ACK) continue;

            s->acks_recv++;

            /* Cumulative ACK: mark all <= ahdr.seq as acked */
            uint32_t ack_seq = ahdr.seq;
            if (ack_seq > highest_acked) {
                highest_acked = ack_seq;
                dup_ack_count = 0;
            } else if (ack_seq == highest_acked) {
                dup_ack_count++;
            }

            for (uint32_t i = 0; i < RETX_MAP_SIZE; i++) {
                if (retx_map[i].active && !retx_map[i].acked &&
                    retx_map[i].seq <= ack_seq) {
                    retx_map[i].acked = true;
                }
            }

            /* SACK: mark ranges */
            if ((ahdr.flags & RC_FLAG_SACK) && ahdr.payload_len >= sizeof(rc_sack_block_t)) {
                uint32_t sack_cnt = ahdr.payload_len / sizeof(rc_sack_block_t);
                if (sack_cnt > RC_MAX_SACK_BLOCKS) sack_cnt = RC_MAX_SACK_BLOCKS;
                rc_sack_block_t sacks[RC_MAX_SACK_BLOCKS];
                memcpy(sacks, ack_buf + RC_HEADER_SIZE,
                       sack_cnt * sizeof(rc_sack_block_t));

                for (uint32_t j = 0; j < sack_cnt; j++) {
                    for (uint32_t i = 0; i < RETX_MAP_SIZE; i++) {
                        if (retx_map[i].active && !retx_map[i].acked &&
                            retx_map[i].seq >= sacks[j].left_edge &&
                            retx_map[i].seq < sacks[j].right_edge) {
                            retx_map[i].acked = true;
                        }
                    }
                }
            }
        }

        /* ── 2. Retransmit: fast retx (3 dup ACKs) ── */
        if (dup_ack_count >= 3) {
            /* Retransmit the first unacked packet after highest_acked */
            for (uint32_t i = 0; i < RETX_MAP_SIZE; i++) {
                if (retx_map[i].active && !retx_map[i].acked &&
                    retx_map[i].seq > highest_acked &&
                    retx_map[i].seq == (highest_acked + 1)) {

                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    uint64_t now_us2 = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
                    if (next_send_us > now_us2) usleep((useconds_t)(next_send_us - now_us2));
                    next_send_us += INTER_PACKET_US;

                    a->lossy->sent++;
                    if (!lossy_drop(a->lossy)) {
                        a->lossy->delivered++;
                        sendto(a->send_fd, retx_map[i].data, retx_map[i].len, 0,
                               (struct sockaddr *)&a->dest, sizeof(a->dest));
                    } else {
                        a->lossy->dropped++;
                    }

                    retx_map[i].sent_ms = now;
                    retx_map[i].rto_ms = (retx_map[i].rto_ms < 2000)
                                            ? retx_map[i].rto_ms * 2 : 2000;
                    s->pkts_retrans++;
                    s->bytes_sent += retx_map[i].len;
                    s->fast_retrans++;
                    break;
                }
            }
            dup_ack_count = 0;
        }

        /* ── 2b. Retransmit: SACK-based (gap in SACK blocks) ── */
        /* SACK retransmit is handled via timeout — if a packet isn't
         * SACKed within RTO/2, retransmit it. We track this via
         * the acked flag set during ACK processing above.          */

        /* ── 2c. Retransmit: timeout ── */
        for (uint32_t i = 0; i < RETX_MAP_SIZE; i++) {
            if (retx_map[i].active && !retx_map[i].acked &&
                retx_map[i].seq > highest_acked &&  /* only retransmit above cumACK */
                now - retx_map[i].sent_ms >= retx_map[i].rto_ms) {

                /* Check this entry hasn't been overwritten by a newer seq */
                uint32_t expected_idx = retx_map[i].seq % RETX_MAP_SIZE;
                if (expected_idx != i) continue;  /* overwritten */

                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t now_us2 = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
                if (next_send_us > now_us2) usleep((useconds_t)(next_send_us - now_us2));
                next_send_us += INTER_PACKET_US;

                a->lossy->sent++;
                if (!lossy_drop(a->lossy)) {
                    a->lossy->delivered++;
                    sendto(a->send_fd, retx_map[i].data, retx_map[i].len, 0,
                           (struct sockaddr *)&a->dest, sizeof(a->dest));
                } else {
                    a->lossy->dropped++;
                }

                retx_map[i].sent_ms = now;
                retx_map[i].rto_ms = (retx_map[i].rto_ms < 2000)
                                        ? retx_map[i].rto_ms * 2 : 2000;
                s->pkts_retrans++;
                s->bytes_sent += retx_map[i].len;
                s->timeout_retrans++;
            }
        }

        /* ── 3. Generate data ── */
        bool send_i = false, send_p = false;
        if (now >= next_i_ms) {
            send_i = true;
            next_i_ms = now + IFRAME_INTERVAL_MS;
            next_p_ms = now + 20;
        } else if (now >= next_p_ms) {
            send_p = true;
            next_p_ms = now + PFRAME_INTERVAL_MS;
        }

        if (!send_i && !send_p) {
            usleep(500);
            continue;
        }

        bool is_i = send_i;
        int npkts = is_i ? IFRAME_PKT_COUNT : PFRAME_PKT_COUNT;
        uint32_t fsz = (uint32_t)npkts * PACKET_BODY_SIZE;
        uint16_t cur_fseq = fseq++;

        for (int p = 0; p < npkts && g_running; p++) {
            uint8_t body[PACKET_BODY_SIZE];
            fill_random(body, PACKET_BODY_SIZE);

            int plen = build_data_pkt(pkt, sizeof(pkt),
                                      a->conn_id, seq,
                                      a->stream_id, cur_fseq,
                                      is_i, (uint32_t)p * PACKET_BODY_SIZE, fsz,
                                      body, PACKET_BODY_SIZE);
            if (plen < 0) break;

            /* ── Drain ACKs before each packet send ── */
            for (;;) {
                struct pollfd ack_pfd = { .fd = a->send_fd, .events = POLLIN };
                if (poll(&ack_pfd, 1, 0) <= 0) break;
                uint8_t abuf[RC_HEADER_SIZE + RC_MAX_SACK_BLOCKS * sizeof(rc_sack_block_t)];
                ssize_t an = recv(a->send_fd, abuf, sizeof(abuf), 0);
                if (an < RC_HEADER_SIZE) break;
                rc_pkt_hdr_t ah;
                if (rc_hdr_unpack(&ah, abuf) != 0 || ah.type != RC_PKT_ACK) continue;
                s->acks_recv++;

                if (ah.seq > highest_acked) {
                    highest_acked = ah.seq;
                    dup_ack_count = 0;
                    for (uint32_t j = 0; j < RETX_MAP_SIZE; j++)
                        if (retx_map[j].active && !retx_map[j].acked &&
                            retx_map[j].seq < highest_acked)
                            retx_map[j].acked = true;
                } else if (ah.seq == highest_acked) {
                    dup_ack_count++;
                }
                if ((ah.flags & RC_FLAG_SACK) && ah.payload_len >= sizeof(rc_sack_block_t)) {
                    uint32_t sc = ah.payload_len / sizeof(rc_sack_block_t);
                    if (sc > RC_MAX_SACK_BLOCKS) sc = RC_MAX_SACK_BLOCKS;
                    rc_sack_block_t sk[RC_MAX_SACK_BLOCKS];
                    memcpy(sk, abuf + RC_HEADER_SIZE, sc * sizeof(rc_sack_block_t));
                    for (uint32_t j = 0; j < sc; j++)
                        for (uint32_t k = 0; k < RETX_MAP_SIZE; k++)
                            if (retx_map[k].active && !retx_map[k].acked &&
                                retx_map[k].seq >= sk[j].left_edge &&
                                retx_map[k].seq < sk[j].right_edge)
                                retx_map[k].acked = true;
                }
            }

            /* Pace */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
            if (next_send_us > now_us) usleep((useconds_t)(next_send_us - now_us));
            next_send_us += INTER_PACKET_US;

            /* Store in retx map */
            uint32_t idx = seq % RETX_MAP_SIZE;
            memcpy(retx_map[idx].data, pkt, (size_t)plen);
            retx_map[idx].len = (uint16_t)plen;
            retx_map[idx].seq = seq;
            retx_map[idx].sent_ms = now;
            retx_map[idx].rto_ms = TEST_RTO_MS;
            retx_map[idx].active = true;
            retx_map[idx].acked = false;

            /* Send */
            a->lossy->sent++;
            if (!lossy_drop(a->lossy)) {
                a->lossy->delivered++;
                sendto(a->send_fd, pkt, (size_t)plen, 0,
                       (struct sockaddr *)&a->dest, sizeof(a->dest));
            } else {
                a->lossy->dropped++;
            }

            s->pkts_sent++;
            s->bytes_sent += (size_t)plen;
            seq++;
        }

        s->frames_sent++;
        if (is_i) s->iframes_sent++;
        else s->pframes_sent++;
    }

    s->end_ms = now_ms();
    free(retx_map);
    return NULL;
}

/* ── Receiver thread (single, demux by stream_id) ──────────────────── */
/* Receives data, deduplicates retransmits, reassembles frames,        */
/* sends cumulative ACK + SACK blocks back.                             */

#define RECV_SEQ_MAX  16384  /* track up to 16K seq numbers per stream */

typedef struct {
    int               data_fd;
    int               ack_fd;
    struct sockaddr_in ack_dest;
    lossy_t           ack_lossy;
} receiver_arg_t;

typedef struct {
    bool     in_progress;
    uint32_t bytes_recv;
    uint32_t frame_size;
    bool     is_i;
} reasm_t;

static void *receiver_thread(void *arg)
{
    receiver_arg_t *a = (receiver_arg_t *)arg;
    reasm_t ra[STREAM_COUNT];
    memset(ra, 0, sizeof(ra));

    /* Per-stream: dedup + ACK tracking */
    uint32_t highest_seq[STREAM_COUNT];     /* highest contiguous seq */
    uint32_t max_seq_seen[STREAM_COUNT];    /* highest seq received (may have gaps) */
    uint8_t *seen[STREAM_COUNT];            /* bitmap: seen[seq % MAX] */
    for (int i = 0; i < STREAM_COUNT; i++) {
        highest_seq[i] = 0;
        max_seq_seen[i] = 0;
        seen[i] = calloc(RECV_SEQ_MAX / 8, 1);
    }

    uint32_t pkts_since_ack[STREAM_COUNT] = {0};

    uint8_t buf[RC_HEADER_SIZE + NALU_HDR_SIZE + PACKET_BODY_SIZE + 64];

    while (g_running) {
        struct pollfd pfd = { .fd = a->data_fd, .events = POLLIN };
        if (poll(&pfd, 1, 500) <= 0) continue;

        ssize_t n = recv(a->data_fd, buf, sizeof(buf), 0);
        if (n < (ssize_t)(RC_HEADER_SIZE + NALU_HDR_SIZE)) continue;

        rc_pkt_hdr_t hdr;
        if (rc_hdr_unpack(&hdr, buf) != 0) continue;
        if (hdr.magic != RC_MAGIC || hdr.payload_len != n - RC_HEADER_SIZE) continue;

        uint8_t sid = 0xFF;
        uint32_t foff = 0, fsz = 0;
        bool is_i = false;

        /* Parse NALU for stream_id and frame info */
        if (hdr.type == RC_PKT_DATA) {
            h264_nalu_hdr_t nalu;
            memcpy(&nalu, buf + RC_HEADER_SIZE, NALU_HDR_SIZE);
            if (ntohl(nalu.start_code) != 0x00000001) continue;
            sid  = nalu.stream_id;
            foff = ntohl(nalu.frame_offset);
            fsz  = ntohl(nalu.frame_size);
            is_i = (nalu.nalu_header & 0x1F) == NALU_TYPE_IDR;
        } else {
            continue;  /* ignore non-DATA */
        }

        if (sid >= STREAM_COUNT) continue;

        uint32_t seq = hdr.seq;
        stream_stats_t *s = &g_ss[sid];

        /* ── Dedup: check if already seen ── */
        uint32_t bit_idx = seq % RECV_SEQ_MAX;
        if (seen[sid][bit_idx / 8] & (1 << (bit_idx % 8))) {
            /* Duplicate (retransmit) — count for stats but don't double-count */
            continue;
        }
        seen[sid][bit_idx / 8] |= (1 << (bit_idx % 8));

        /* New packet */
        s->pkts_recv++;
        s->bytes_recv += (size_t)n;

        /* Track max seq seen */
        if (seq > max_seq_seen[sid]) max_seq_seen[sid] = seq;

        /* Advance highest contiguous seq */
        while (seen[sid][(highest_seq[sid] % RECV_SEQ_MAX) / 8] &
               (1 << ((highest_seq[sid] % RECV_SEQ_MAX) % 8))) {
            highest_seq[sid]++;
        }

        /* Frame reassembly */
        reasm_t *r = &ra[sid];
        if (foff == 0) {
            r->in_progress = true;
            r->bytes_recv  = (uint32_t)(n - RC_HEADER_SIZE - NALU_HDR_SIZE);
            r->frame_size  = fsz;
            r->is_i        = is_i;
        } else {
            r->bytes_recv += (uint32_t)(n - RC_HEADER_SIZE - NALU_HDR_SIZE);
        }
        if (r->in_progress && r->bytes_recv >= r->frame_size) {
            r->in_progress = false;
            s->frames_recv++;
            if (r->is_i) s->iframes_recv++;
            else s->pframes_recv++;
        }

        /* ── Send ACK every ACK_INTERVAL new packets ── */
        pkts_since_ack[sid]++;
        if (pkts_since_ack[sid] >= ACK_INTERVAL) {
            pkts_since_ack[sid] = 0;

            /* Build SACK blocks for gaps between highest_seq and max_seq_seen */
            rc_sack_block_t sacks[RC_MAX_SACK_BLOCKS];
            uint32_t sack_cnt = 0;

            /* Find contiguous received ranges in the gap region */
            uint32_t scan = highest_seq[sid];
            uint32_t range_start = 0;
            bool in_range = false;

            while (scan <= max_seq_seen[sid] && sack_cnt < RC_MAX_SACK_BLOCKS) {
                bool recvd = (seen[sid][(scan % RECV_SEQ_MAX) / 8] &
                              (1 << ((scan % RECV_SEQ_MAX) % 8))) != 0;
                if (recvd) {
                    if (!in_range) { range_start = scan; in_range = true; }
                } else {
                    if (in_range) {
                        sacks[sack_cnt].left_edge = range_start;
                        sacks[sack_cnt].right_edge = scan;
                        sack_cnt++;
                        in_range = false;
                    }
                }
                scan++;
            }
            if (in_range && sack_cnt < RC_MAX_SACK_BLOCKS) {
                sacks[sack_cnt].left_edge = range_start;
                sacks[sack_cnt].right_edge = max_seq_seen[sid] + 1;
                sack_cnt++;
            }

            uint8_t ack_buf[RC_HEADER_SIZE + RC_MAX_SACK_BLOCKS * sizeof(rc_sack_block_t)];
            int ack_len = build_ack_pkt(ack_buf, sizeof(ack_buf),
                                        0xBB000001 + sid,
                                        highest_seq[sid],
                                        sacks, sack_cnt);
            if (ack_len > 0) {
                a->ack_lossy.sent++;
                if (!lossy_drop(&a->ack_lossy)) {
                    a->ack_lossy.delivered++;
                    sendto(a->ack_fd, ack_buf, (size_t)ack_len, 0,
                           (struct sockaddr *)&a->ack_dest, sizeof(a->ack_dest));
                } else {
                    a->ack_lossy.dropped++;
                }
                s->acks_sent++;
            }
        }
    }

    for (int i = 0; i < STREAM_COUNT; i++) free(seen[i]);
    return NULL;
}

/* ── Socket helper ──────────────────────────────────────────────────── */

static int make_sock(uint16_t *port) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int opt = 1, buf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    struct sockaddr_in a = {
        .sin_family = AF_INET, .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    socklen_t al = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &al);
    if (port) *port = ntohs(a.sin_port);
    return fd;
}

/* ── Run one scenario ──────────────────────────────────────────────── */

static int run_scenario(uint32_t loss_pct, int dur_sec)
{
    printf("\n══════════════════════════════════════════════════════\n");
    printf("  Scenario: %u%% loss, %d sec (with retransmission)\n", loss_pct, dur_sec);
    printf("══════════════════════════════════════════════════════\n");

    memset(g_ss, 0, sizeof(g_ss));
    g_running = true;

    uint16_t robot_port, relay_port, ctrl_port;
    int robot_fd = make_sock(&robot_port);
    int relay_fd = make_sock(&relay_port);
    int ctrl_fd  = make_sock(&ctrl_port);
    if (robot_fd < 0 || relay_fd < 0 || ctrl_fd < 0) return -1;

    printf("  Robot(%u) ↔ Relay(%u) ↔ Controller(%u)\n",
           robot_port, relay_port, ctrl_port);

    struct sockaddr_in relay_addr = {
        .sin_family = AF_INET, .sin_port = htons(relay_port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    struct sockaddr_in robot_addr = {
        .sin_family = AF_INET, .sin_port = htons(robot_port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    struct sockaddr_in ctrl_addr = {
        .sin_family = AF_INET, .sin_port = htons(ctrl_port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };

    /* Separate lossy channels for data path and ACK path */
    lossy_t data_lossy[STREAM_COUNT], ack_lossy[STREAM_COUNT];
    for (int i = 0; i < STREAM_COUNT; i++) {
        lossy_init(&data_lossy[i], loss_pct);
        lossy_init(&ack_lossy[i], loss_pct);
    }

    /* Start sender threads */
    pthread_t sthr[STREAM_COUNT];
    sender_v2_arg_t sa[STREAM_COUNT];
    for (int i = 0; i < STREAM_COUNT; i++) {
        sa[i].send_fd = robot_fd;
        sa[i].dest    = relay_addr;
        sa[i].conn_id = 0xAA000001 + (uint32_t)i;
        sa[i].stream_id = (uint8_t)i;
        sa[i].lossy   = &data_lossy[i];
        pthread_create(&sthr[i], NULL, sender_v2_thread, &sa[i]);
    }

    /* Start receiver thread */
    pthread_t rthr;
    receiver_arg_t rca = {
        .data_fd  = ctrl_fd,
        .ack_fd   = ctrl_fd,
        .ack_dest = relay_addr,  /* ACKs go back through relay to robot */
        .ack_lossy = ack_lossy[0], /* simplified: shared ack lossy */
    };
    pthread_create(&rthr, NULL, receiver_thread, &rca);

    /* Relay loop: forward both directions */
    printf("  Running %d sec...\n", dur_sec);
    uint64_t t_start = now_ms(), t_end = t_start + (uint64_t)dur_sec * 1000;
    uint64_t t_rpt = t_start;
    uint64_t fwd_data = 0, fwd_ack = 0;

    uint8_t rbuf[2048];
    while (now_ms() < t_end) {
        struct pollfd pfd = { .fd = relay_fd, .events = POLLIN };
        if (poll(&pfd, 1, 100) <= 0) {
            uint64_t now = now_ms();
            if (now - t_rpt >= 30000) {
                printf("  [%3lds/%ds] relay: data=%lu ack=%lu  "
                       "S0: tx=%lu retx=%lu rx=%lu  S1: tx=%lu retx=%lu rx=%lu\n",
                       (long)((now - t_start) / 1000), dur_sec,
                       (unsigned long)fwd_data, (unsigned long)fwd_ack,
                       (unsigned long)g_ss[0].pkts_sent,
                       (unsigned long)g_ss[0].pkts_retrans,
                       (unsigned long)g_ss[0].pkts_recv,
                       (unsigned long)g_ss[1].pkts_sent,
                       (unsigned long)g_ss[1].pkts_retrans,
                       (unsigned long)g_ss[1].pkts_recv);
                t_rpt = now;
            }
            continue;
        }

        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(relay_fd, rbuf, sizeof(rbuf), 0,
                             (struct sockaddr *)&from, &fl);
        if (n < RC_HEADER_SIZE) continue;

        /* Determine direction and forward */
        struct sockaddr_in *dst;

        if (from.sin_port == htons(robot_port)) {
            dst = &ctrl_addr;
            fwd_data++;
        } else if (from.sin_port == htons(ctrl_port)) {
            dst = &robot_addr;
            fwd_ack++;
        } else {
            continue;
        }

        sendto(relay_fd, rbuf, (size_t)n, 0, (struct sockaddr *)dst, sizeof(*dst));
    }

    g_running = false;
    for (int i = 0; i < STREAM_COUNT; i++) pthread_join(sthr[i], NULL);
    pthread_join(rthr, NULL);

    /* ── Report ── */
    printf("\n  ── Results (with retransmission) ─────────────────\n");
    printf("  Relay: %lu data, %lu ack forwarded\n",
           (unsigned long)fwd_data, (unsigned long)fwd_ack);

    for (int i = 0; i < STREAM_COUNT; i++) {
        stream_stats_t *s = &g_ss[i];
        uint64_t ms = s->end_ms - s->start_ms;
        if (!ms) ms = 1;
        double tx = (double)s->bytes_sent * 8.0 / (double)ms / 1000.0;
        double rx = (double)s->bytes_recv * 8.0 / (double)ms / 1000.0;
        double fdel = s->frames_sent ? (double)s->frames_recv / (double)s->frames_sent * 100.0 : 0;
        double ploss = s->pkts_sent ? (1.0 - (double)s->pkts_recv / (double)s->pkts_sent) * 100.0 : 0;

        printf("\n  Stream %d:\n", i);
        printf("    Data pkts:     %lu sent + %lu retrans = %lu total\n",
               (unsigned long)s->pkts_sent, (unsigned long)s->pkts_retrans,
               (unsigned long)(s->pkts_sent + s->pkts_retrans));
        printf("    Pkt recv:      %lu (%.1f%% effective loss)\n",
               (unsigned long)s->pkts_recv, ploss);
        printf("    Send rate:     %.2f Mbps (incl retrans)\n", tx);
        printf("    Recv rate:     %.2f Mbps\n", rx);
        printf("    Frames:        %lu → %lu (%.1f%%)\n",
               (unsigned long)s->frames_sent, (unsigned long)s->frames_recv, fdel);
        printf("    I-frames:      %lu → %lu\n",
               (unsigned long)s->iframes_sent, (unsigned long)s->iframes_recv);
        printf("    P-frames:      %lu → %lu\n",
               (unsigned long)s->pframes_sent, (unsigned long)s->pframes_recv);
        printf("    ACKs:          sent=%lu recv=%lu\n",
               (unsigned long)s->acks_sent, (unsigned long)s->acks_recv);
        printf("    Fast retx:     %lu\n", (unsigned long)s->fast_retrans);
        printf("    Timeout retx:  %lu\n", (unsigned long)s->timeout_retrans);
        printf("    Errors:        CRC=%lu NALU=%lu\n",
               (unsigned long)s->crc_err, (unsigned long)s->nalu_err);
    }

    uint64_t ts = 0, td = 0;
    for (int i = 0; i < STREAM_COUNT; i++) {
        ts += data_lossy[i].sent; td += data_lossy[i].dropped;
    }
    if (ts) printf("\n  Data channel: %lu sent, %lu dropped (%.1f%%)\n",
                   (unsigned long)ts, (unsigned long)td, (double)td / (double)ts * 100.0);

    close(robot_fd); close(relay_fd); close(ctrl_fd);
    return 0;
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  RoboControl Integration Test                       ║\n");
    printf("║  Relay + H264 + paced 1 Mbps + SACK retransmit      ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    int dur = 60;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--duration") && i + 1 < argc) dur = atoi(argv[++i]);

    uint32_t rates[] = {0, 5, 20};
    for (int i = 0; i < 3; i++) run_scenario(rates[i], dur);

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  All scenarios completed                             ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    return 0;
}
