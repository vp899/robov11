/**
 * @file robot_sender.c
 * @brief Robot process — sends 2 H.264 video streams via relay.
 *
 * Supports two transport modes:
 *   - RELIABLE:  ACK-based retransmission for 100% delivery.
 *   - REALTIME:  Fire-and-forget for minimum latency.
 *
 * Each stream has its own independent timer and pacer at 1 Mbps.
 *
 * Usage: ./robot_sender <relay_ip> <relay_port> <duration_sec> [reliable|realtime]
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <poll.h>

#include "pacing.h"
#include "h264_gen.h"
#include "transport_mode.h"

/* ── Protocol ─────────────────────────────────────────────────────── */
#define RC_MAGIC            0x524F424FU
#define RC_PROTO_VERSION    1U
#define RC_HEADER_SIZE      32
#define RC_MAX_PAYLOAD      1400
#define RC_PKT_DATA         0x0003
#define RC_PKT_ACK          0x0004
#define RC_PKT_RELAY_REG    0x0010
#define RC_PKT_RELAY_ACK    0x0011
#define RC_PKT_FIN          0x00FF
#define RC_FLAG_RELIABLE    (1 << 4)
#define RC_FLAG_SACK        (1 << 2)

/* ── Stream params ────────────────────────────────────────────────── */
#define NUM_STREAMS         2
#define STREAM_BITRATE_BPS  1000000    /* 1 Mbps per stream */
#define IFRAME_SIZE         30000
#define PFRAME_SIZE         3000
#define IFRAME_INTERVAL     30
#define FPS                 2
#define MAX_PACKET_PAYLOAD  (RC_MAX_PAYLOAD - 7)

/* ── Retransmission queue ─────────────────────────────────────────── */
#define RETX_QUEUE_SIZE     8192

typedef struct {
    uint32_t    seq;
    uint16_t    stream_id;
    uint64_t    sent_ts_us;
    uint32_t    retx_timeout_ms;
    uint32_t    retx_count;
    bool        acked;
    uint8_t    *pkt_data;
    uint16_t    pkt_len;
} retx_entry_t;

typedef struct {
    retx_entry_t entries[RETX_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint64_t fast_retransmits;
    uint64_t timeout_retransmits;
    uint64_t total_acked;
} retx_queue_t;

/* ── Packet header ────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic; uint16_t version; uint16_t type;
    uint32_t seq; uint32_t ack; uint64_t timestamp;
    uint16_t payload_len; uint16_t flags; uint32_t conn_id;
} pkt_hdr_t;

/* ── SACK block ───────────────────────────────────────────────────── */
typedef struct {
    uint32_t left_edge;
    uint32_t right_edge;
} sack_block_t;

#define MAX_SACK_BLOCKS 4

/* ── Global state ─────────────────────────────────────────────────── */
static volatile bool g_running = true;
static void sig_handler(int sig) { (void)sig; g_running = false; }

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void hdr_pack(uint8_t *d, const pkt_hdr_t *h)
{
    d[0]=h->magic>>24; d[1]=(h->magic>>16)&0xFF;
    d[2]=(h->magic>>8)&0xFF; d[3]=h->magic&0xFF;
    d[4]=h->version>>8; d[5]=h->version&0xFF;
    d[6]=h->type>>8; d[7]=h->type&0xFF;
    d[8]=h->seq>>24; d[9]=(h->seq>>16)&0xFF;
    d[10]=(h->seq>>8)&0xFF; d[11]=h->seq&0xFF;
    d[12]=h->ack>>24; d[13]=(h->ack>>16)&0xFF;
    d[14]=(h->ack>>8)&0xFF; d[15]=h->ack&0xFF;
    for (int i=0;i<8;i++) d[16+i]=(h->timestamp>>(56-8*i))&0xFF;
    d[24]=h->payload_len>>8; d[25]=h->payload_len&0xFF;
    d[26]=h->flags>>8; d[27]=h->flags&0xFF;
    d[28]=h->conn_id>>24; d[29]=(h->conn_id>>16)&0xFF;
    d[30]=(h->conn_id>>8)&0xFF; d[31]=h->conn_id&0xFF;
}

static int hdr_unpack(pkt_hdr_t *h, const uint8_t *s)
{
    h->magic=(s[0]<<24)|(s[1]<<16)|(s[2]<<8)|s[3];
    if (h->magic!=RC_MAGIC) return -1;
    h->version=(s[4]<<8)|s[5];
    h->type=(s[6]<<8)|s[7];
    h->seq=(s[8]<<24)|(s[9]<<16)|(s[10]<<8)|s[11];
    h->ack=(s[12]<<24)|(s[13]<<16)|(s[14]<<8)|s[15];
    h->timestamp=0;
    for (int i=0;i<8;i++) h->timestamp=(h->timestamp<<8)|s[16+i];
    h->payload_len=(s[24]<<8)|s[25];
    h->flags=(s[26]<<8)|s[27];
    h->conn_id=(s[28]<<24)|(s[29]<<16)|(s[30]<<8)|s[31];
    return 0;
}

/* ── Retransmission queue ─────────────────────────────────────────── */

static void retx_init(retx_queue_t *q)
{
    memset(q, 0, sizeof(*q));
}

static void retx_enqueue(retx_queue_t *q, uint32_t seq, uint16_t stream_id,
                          const uint8_t *pkt, uint16_t pkt_len,
                          uint32_t rto_ms)
{
    if (q->count >= RETX_QUEUE_SIZE) return;
    retx_entry_t *e = &q->entries[q->tail % RETX_QUEUE_SIZE];
    e->seq = seq;
    e->stream_id = stream_id;
    e->sent_ts_us = now_us();
    e->retx_timeout_ms = rto_ms;
    e->retx_count = 0;
    e->acked = false;
    e->pkt_len = pkt_len;
    if (e->pkt_data) free(e->pkt_data);
    e->pkt_data = malloc(pkt_len);
    if (e->pkt_data) memcpy(e->pkt_data, pkt, pkt_len);
    q->tail++;
    q->count++;
}

static void retx_process_ack(retx_queue_t *q, uint32_t ack_seq)
{
    uint32_t idx = q->head;
    for (uint32_t i = 0; i < q->count; i++) {
        retx_entry_t *e = &q->entries[idx % RETX_QUEUE_SIZE];
        if (!e->acked && e->seq <= ack_seq) {
            e->acked = true;
            q->total_acked++;
        }
        idx++;
    }
    /* Drain acked from head */
    while (q->count > 0) {
        retx_entry_t *e = &q->entries[q->head % RETX_QUEUE_SIZE];
        if (e->acked) {
            free(e->pkt_data);
            e->pkt_data = NULL;
            q->head++;
            q->count--;
        } else {
            break;
        }
    }
}

static int retx_detect_and_retransmit(retx_queue_t *q, int fd,
                                       const struct sockaddr *dst, socklen_t dst_len)
{
    int retransmitted = 0;
    uint64_t now = now_us();
    uint32_t idx = q->head;

    for (uint32_t i = 0; i < q->count; i++) {
        retx_entry_t *e = &q->entries[idx % RETX_QUEUE_SIZE];
        if (e->acked || !e->pkt_data) { idx++; continue; }

        uint64_t age_ms = (now - e->sent_ts_us) / 1000;
        if (age_ms >= e->retx_timeout_ms) {
            /* Retransmit */
            sendto(fd, e->pkt_data, e->pkt_len, 0, dst, dst_len);
            e->sent_ts_us = now;
            e->retx_timeout_ms = (e->retx_timeout_ms < 4000)
                                  ? e->retx_timeout_ms * 2 : 4000;
            e->retx_count++;
            q->timeout_retransmits++;
            retransmitted++;
        }
        idx++;
    }
    return retransmitted;
}

/* ── Stream state ─────────────────────────────────────────────────── */
typedef struct {
    uint16_t    stream_id;
    uint32_t    frame_num;
    uint32_t    seq;            /* per-stream sequence number */
    pacer_t     pacer;
    uint64_t    total_sent;
    uint64_t    total_frames;
    uint64_t    total_pkts;
    uint64_t    next_frame_us;
    uint64_t    retx_pkts;
} stream_state_t;

static void send_frame(stream_state_t *st, int fd,
                        const struct sockaddr *dst, socklen_t dst_len,
                        uint32_t conn_id,
                        const uint8_t *h264_data, size_t h264_len,
                        int is_iframe,
                        const transport_config_t *cfg,
                        retx_queue_t *retx)
{
    size_t offset = 0;
    while (offset < h264_len && g_running) {
        size_t chunk = h264_len - offset;
        if (chunk > MAX_PACKET_PAYLOAD) chunk = MAX_PACKET_PAYLOAD;

        uint8_t payload[RC_MAX_PAYLOAD];
        payload[0] = (st->stream_id >> 8) & 0xFF;
        payload[1] = st->stream_id & 0xFF;
        payload[2] = (st->frame_num >> 24) & 0xFF;
        payload[3] = (st->frame_num >> 16) & 0xFF;
        payload[4] = (st->frame_num >> 8) & 0xFF;
        payload[5] = st->frame_num & 0xFF;
        payload[6] = (uint8_t)is_iframe;
        memcpy(payload + 7, h264_data + offset, chunk);
        uint16_t total_payload = (uint16_t)(chunk + 7);

        uint64_t pkt_size = RC_HEADER_SIZE + total_payload;

        if (cfg->enable_pacing)
            pacer_wait(&st->pacer, pkt_size);

        pkt_hdr_t hdr = {0};
        hdr.magic = RC_MAGIC;
        hdr.version = RC_PROTO_VERSION;
        hdr.type = RC_PKT_DATA;
        hdr.seq = st->seq++;
        hdr.conn_id = conn_id;
        hdr.timestamp = now_us();
        hdr.payload_len = total_payload;
        if (cfg->enable_retransmit)
            hdr.flags |= RC_FLAG_RELIABLE;

        uint8_t buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
        hdr_pack(buf, &hdr);
        memcpy(buf + RC_HEADER_SIZE, payload, total_payload);
        sendto(fd, buf, pkt_size, 0, dst, dst_len);

        /* Enqueue for retransmission if reliable mode */
        if (cfg->enable_retransmit && retx) {
            retx_enqueue(retx, hdr.seq, st->stream_id,
                         buf, (uint16_t)pkt_size, cfg->retx_timeout_ms);
        }

        st->total_sent += pkt_size;
        st->total_pkts++;
        offset += chunk;
    }
    st->total_frames++;
}

/* ── Main ─────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <relay_ip> <relay_port> <duration_sec> [reliable|realtime]\n", argv[0]);
        return 1;
    }
    const char *relay_ip = argv[1];
    uint16_t relay_port = (uint16_t)atoi(argv[2]);
    int duration_sec = atoi(argv[3]);
    if (duration_sec <= 0) duration_sec = 180;

    /* Parse transport mode */
    transport_mode_t mode = TRANSPORT_REALTIME;
    if (argc > 4) {
        if (strcmp(argv[4], "reliable") == 0)
            mode = TRANSPORT_RELIABLE;
        else if (strcmp(argv[4], "realtime") == 0)
            mode = TRANSPORT_REALTIME;
        else {
            fprintf(stderr, "Unknown mode: %s (use 'reliable' or 'realtime')\n", argv[4]);
            return 1;
        }
    }
    transport_config_t tcfg = transport_config_get(mode);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int bufsize = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in relay_addr = {
        .sin_family = AF_INET, .sin_port = htons(relay_port),
    };
    if (inet_pton(AF_INET, relay_ip, &relay_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid relay IP: %s\n", relay_ip);
        return 1;
    }

    printf("[ROBOT] Connecting to relay %s:%u for %d seconds\n",
           relay_ip, relay_port, duration_sec);
    printf("[ROBOT] Transport mode: %s\n", transport_mode_name(mode));
    transport_config_print(&tcfg, stdout);
    fflush(stdout);

    /* ── Register with relay ──────────────────────────────────────── */
    uint32_t conn_id = 0x524F4201;
    pkt_hdr_t hdr = {0};
    hdr.magic = RC_MAGIC; hdr.version = RC_PROTO_VERSION;
    hdr.type = RC_PKT_RELAY_REG; hdr.seq = 0; hdr.conn_id = conn_id;
    const char *role = "robot";
    hdr.payload_len = (uint16_t)strlen(role);
    uint8_t buf[RC_HEADER_SIZE + 64];
    hdr_pack(buf, &hdr);
    memcpy(buf + RC_HEADER_SIZE, role, strlen(role));
    sendto(fd, buf, RC_HEADER_SIZE + strlen(role), 0,
           (struct sockaddr *)&relay_addr, sizeof(relay_addr));

    uint8_t rbuf[RC_HEADER_SIZE + 64];
    ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
    if (n < (ssize_t)RC_HEADER_SIZE) {
        fprintf(stderr, "[ROBOT] Relay registration timeout\n");
        close(fd); return 1;
    }
    printf("[ROBOT] Registered with relay\n");
    fflush(stdout);

    /* ── Initialize retransmission queue (reliable mode only) ─────── */
    retx_queue_t retx;
    retx_init(&retx);

    /* ── Initialize streams ───────────────────────────────────────── */
    stream_state_t streams[NUM_STREAMS];
    uint64_t frame_interval_us = 1000000ULL / FPS;

    for (int i = 0; i < NUM_STREAMS; i++) {
        memset(&streams[i], 0, sizeof(streams[i]));
        streams[i].stream_id = (uint16_t)(i + 1);
        pacer_init(&streams[i].pacer, STREAM_BITRATE_BPS);
        streams[i].next_frame_us = now_us() + (uint64_t)i * frame_interval_us / NUM_STREAMS;
    }

    uint8_t *iframe_buf = malloc(IFRAME_SIZE);
    uint8_t *pframe_buf = malloc(PFRAME_SIZE);
    if (!iframe_buf || !pframe_buf) { fprintf(stderr, "[ROBOT] OOM\n"); return 1; }

    /* ── Main loop ────────────────────────────────────────────────── */
    uint64_t start_ms = now_ms();
    uint64_t end_ms = start_ms + (uint64_t)duration_sec * 1000;
    uint64_t last_report = start_ms;
    uint64_t last_ack_check = start_ms;

    printf("[ROBOT] Sending %d streams × %d fps × %d bps each\n",
           NUM_STREAMS, FPS, STREAM_BITRATE_BPS);
    fflush(stdout);

    while (g_running && now_ms() < end_ms) {
        uint64_t now = now_us();
        bool sent_any = false;

        for (int i = 0; i < NUM_STREAMS; i++) {
            stream_state_t *st = &streams[i];
            if (now < st->next_frame_us) continue;

            int is_iframe = (st->frame_num % IFRAME_INTERVAL == 0);
            size_t h264_len;
            if (is_iframe) {
                h264_len = h264_gen_iframe(iframe_buf, IFRAME_SIZE,
                                            st->stream_id, st->frame_num);
                send_frame(st, fd, (struct sockaddr *)&relay_addr,
                           sizeof(relay_addr), conn_id, iframe_buf, h264_len, 1,
                           &tcfg, &retx);
            } else {
                h264_len = h264_gen_pframe(pframe_buf, PFRAME_SIZE,
                                            st->stream_id, st->frame_num);
                send_frame(st, fd, (struct sockaddr *)&relay_addr,
                           sizeof(relay_addr), conn_id, pframe_buf, h264_len, 0,
                           &tcfg, &retx);
            }

            st->next_frame_us += frame_interval_us;
            if (st->next_frame_us < now) st->next_frame_us = now;
            sent_any = true;
        }

        /* Process ACKs (reliable mode) */
        if (tcfg.enable_retransmit) {
            /* Non-blocking ACK receive */
            uint8_t ack_buf[1024];
            while (1) {
                struct pollfd pfd = { .fd = fd, .events = POLLIN };
                if (poll(&pfd, 1, 0) <= 0) break;
                ssize_t an = recv(fd, ack_buf, sizeof(ack_buf), 0);
                if (an < (ssize_t)RC_HEADER_SIZE) break;

                pkt_hdr_t ack_hdr;
                if (hdr_unpack(&ack_hdr, ack_buf) != 0) continue;
                if (ack_hdr.type == RC_PKT_ACK) {
                    retx_process_ack(&retx, ack_hdr.ack);
                }
            }

            /* Retransmit detection every 50ms */
            uint64_t now_ms_val = now_ms();
            if (now_ms_val - last_ack_check >= 50) {
                int retx_count = retx_detect_and_retransmit(
                    &retx, fd, (struct sockaddr *)&relay_addr, sizeof(relay_addr));
                if (retx_count > 0) {
                    for (int i = 0; i < NUM_STREAMS; i++)
                        streams[i].retx_pkts += retx_count / NUM_STREAMS;
                }
                last_ack_check = now_ms_val;
            }
        }

        /* Sleep if nothing to send */
        if (!sent_any) {
            uint64_t earliest = UINT64_MAX;
            for (int i = 0; i < NUM_STREAMS; i++) {
                if (streams[i].next_frame_us < earliest)
                    earliest = streams[i].next_frame_us;
            }
            uint64_t now2 = now_us();
            if (earliest > now2 && earliest - now2 > 500) {
                struct timespec ts = {
                    .tv_sec  = (earliest - now2 - 200) / 1000000,
                    .tv_nsec = ((earliest - now2 - 200) % 1000000) * 1000,
                };
                nanosleep(&ts, NULL);
            }
        }

        /* Periodic report */
        uint64_t now_ms_val = now_ms();
        if (now_ms_val - last_report >= 5000) {
            last_report = now_ms_val;
            printf("[ROBOT] %lus: S1=%luF/%luP S2=%luF/%luP retx=%lu acked=%lu\n",
                   (unsigned long)((now_ms_val - start_ms) / 1000),
                   (unsigned long)streams[0].total_frames,
                   (unsigned long)streams[0].total_pkts,
                   (unsigned long)streams[1].total_frames,
                   (unsigned long)streams[1].total_pkts,
                   (unsigned long)retx.timeout_retransmits,
                   (unsigned long)retx.total_acked);
            fflush(stdout);
        }
    }

    /* ── Final report ─────────────────────────────────────────────── */
    printf("\n[ROBOT] === Final Report ===\n");
    printf("[ROBOT] Transport: %s\n", transport_mode_name(mode));
    printf("[ROBOT] Duration: %lu ms\n", (unsigned long)(now_ms() - start_ms));
    for (int i = 0; i < NUM_STREAMS; i++) {
        printf("[ROBOT] Stream %u: frames=%lu pkts=%lu bytes=%lu retx=%lu\n",
               streams[i].stream_id,
               (unsigned long)streams[i].total_frames,
               (unsigned long)streams[i].total_pkts,
               (unsigned long)streams[i].total_sent,
               (unsigned long)streams[i].retx_pkts);
    }
    if (tcfg.enable_retransmit) {
        printf("[ROBOT] Retransmission stats:\n");
        printf("[ROBOT]   Fast retx:     %lu\n", (unsigned long)retx.fast_retransmits);
        printf("[ROBOT]   Timeout retx:  %lu\n", (unsigned long)retx.timeout_retransmits);
        printf("[ROBOT]   Total acked:   %lu\n", (unsigned long)retx.total_acked);
    }
    fflush(stdout);

    /* FIN */
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = RC_MAGIC; hdr.version = RC_PROTO_VERSION;
    hdr.type = RC_PKT_FIN; hdr.seq = 0; hdr.conn_id = conn_id;
    hdr_pack(buf, &hdr);
    sendto(fd, buf, RC_HEADER_SIZE, 0,
           (struct sockaddr *)&relay_addr, sizeof(relay_addr));

    free(iframe_buf); free(pframe_buf); close(fd);
    return 0;
}
