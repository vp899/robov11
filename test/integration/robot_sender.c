/**
 * @file robot_sender.c
 * @brief Robot process — sends 2 H.264 video streams via relay.
 *
 * Each stream has its own independent timer and pacer at 1 Mbps.
 *
 * Usage: ./robot_sender <relay_ip> <relay_port> <duration_sec>
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

#include "pacing.h"
#include "h264_gen.h"

/* ── Protocol ─────────────────────────────────────────────────────── */
#define RC_MAGIC            0x524F424FU
#define RC_PROTO_VERSION    1U
#define RC_HEADER_SIZE      32
#define RC_MAX_PAYLOAD      1400
#define RC_PKT_DATA         0x0003
#define RC_PKT_RELAY_REG    0x0010
#define RC_PKT_RELAY_ACK    0x0011
#define RC_PKT_FIN          0x00FF

/* ── Stream params ────────────────────────────────────────────────── */
#define NUM_STREAMS         2
#define STREAM_BITRATE_BPS  1000000    /* 1 Mbps per stream */
#define IFRAME_SIZE         30000
#define PFRAME_SIZE         3000
#define IFRAME_INTERVAL     30
#define FPS                 2
#define MAX_PACKET_PAYLOAD  (RC_MAX_PAYLOAD - 7)

typedef struct __attribute__((packed)) {
    uint32_t magic; uint16_t version; uint16_t type;
    uint32_t seq; uint32_t ack; uint64_t timestamp;
    uint16_t payload_len; uint16_t flags; uint32_t conn_id;
} pkt_hdr_t;

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

/* ── Stream state ─────────────────────────────────────────────────── */
typedef struct {
    uint16_t    stream_id;
    uint32_t    frame_num;
    uint32_t    seq;            /* per-stream sequence number */
    pacer_t     pacer;
    uint64_t    total_sent;
    uint64_t    total_frames;
    uint64_t    total_pkts;
    uint64_t    next_frame_us;  /* per-stream frame timer */
} stream_state_t;

static void send_frame(stream_state_t *st, int fd,
                        const struct sockaddr *dst, socklen_t dst_len,
                        uint32_t conn_id,
                        const uint8_t *h264_data, size_t h264_len,
                        int is_iframe)
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
        pacer_wait(&st->pacer, pkt_size);

        pkt_hdr_t hdr = {0};
        hdr.magic = RC_MAGIC;
        hdr.version = RC_PROTO_VERSION;
        hdr.type = RC_PKT_DATA;
        hdr.seq = st->seq++;
        hdr.conn_id = conn_id;
        hdr.timestamp = now_us();
        hdr.payload_len = total_payload;

        uint8_t buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
        hdr_pack(buf, &hdr);
        memcpy(buf + RC_HEADER_SIZE, payload, total_payload);
        sendto(fd, buf, pkt_size, 0, dst, dst_len);

        st->total_sent += pkt_size;
        st->total_pkts++;
        offset += chunk;
    }
    st->total_frames++;
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <relay_ip> <relay_port> <duration_sec>\n", argv[0]);
        return 1;
    }
    const char *relay_ip = argv[1];
    uint16_t relay_port = (uint16_t)atoi(argv[2]);
    int duration_sec = atoi(argv[3]);
    if (duration_sec <= 0) duration_sec = 180;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
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
    printf("[ROBOT] Registered with relay, session_id=%u\n",
           ntohl(*(uint32_t *)(rbuf + 28)));
    fflush(stdout);

    /* ── Initialize streams (each with its own timer) ─────────────── */
    stream_state_t streams[NUM_STREAMS];
    uint64_t frame_interval_us = 1000000ULL / FPS;

    for (int i = 0; i < NUM_STREAMS; i++) {
        memset(&streams[i], 0, sizeof(streams[i]));
        streams[i].stream_id = (uint16_t)(i + 1);
        pacer_init(&streams[i].pacer, STREAM_BITRATE_BPS);
        /* Stagger streams: stream 0 starts at now, stream 1 at now + half interval */
        streams[i].next_frame_us = now_us() + (uint64_t)i * frame_interval_us / NUM_STREAMS;
    }

    uint8_t *iframe_buf = malloc(IFRAME_SIZE);
    uint8_t *pframe_buf = malloc(PFRAME_SIZE);
    if (!iframe_buf || !pframe_buf) { fprintf(stderr, "[ROBOT] OOM\n"); return 1; }

    /* ── Main loop ────────────────────────────────────────────────── */
    uint64_t start_ms = now_ms();
    uint64_t end_ms = start_ms + (uint64_t)duration_sec * 1000;
    uint64_t last_report = start_ms;

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
                           sizeof(relay_addr), conn_id, iframe_buf, h264_len, 1);
            } else {
                h264_len = h264_gen_pframe(pframe_buf, PFRAME_SIZE,
                                            st->stream_id, st->frame_num);
                send_frame(st, fd, (struct sockaddr *)&relay_addr,
                           sizeof(relay_addr), conn_id, pframe_buf, h264_len, 0);
            }

            /* Schedule next frame for this stream */
            st->next_frame_us += frame_interval_us;
            /* If we fell behind, catch up to now */
            if (st->next_frame_us < now) st->next_frame_us = now;
            sent_any = true;
        }

        /* Sleep a bit if nothing to send yet */
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
            printf("[ROBOT] %lus: S1=%luF/%luP S2=%luF/%luP\n",
                   (unsigned long)((now_ms_val - start_ms) / 1000),
                   (unsigned long)streams[0].total_frames,
                   (unsigned long)streams[0].total_pkts,
                   (unsigned long)streams[1].total_frames,
                   (unsigned long)streams[1].total_pkts);
            fflush(stdout);
        }
    }

    /* ── Final report ─────────────────────────────────────────────── */
    printf("\n[ROBOT] === Final Report ===\n");
    printf("[ROBOT] Duration: %lu ms\n", (unsigned long)(now_ms() - start_ms));
    for (int i = 0; i < NUM_STREAMS; i++) {
        printf("[ROBOT] Stream %u: frames=%lu pkts=%lu bytes=%lu rate=%lu bps\n",
               streams[i].stream_id,
               (unsigned long)streams[i].total_frames,
               (unsigned long)streams[i].total_pkts,
               (unsigned long)streams[i].total_sent,
               (unsigned long)pacer_effective_rate(&streams[i].pacer));
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
