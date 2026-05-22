/**
 * @file remote_receiver.c
 * @brief Remote control — receives H.264 streams via relay, validates,
 *        records per-packet latency, generates report.
 *
 * Usage: ./remote_receiver <relay_ip> <relay_port> <duration_sec> <output_prefix>
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

#include "latency_stats.h"
#include "h264_gen.h"

/* ── Protocol ─────────────────────────────────────────────────────── */
#define RC_MAGIC            0x524F424FU
#define RC_PROTO_VERSION    1U
#define RC_HEADER_SIZE      32
#define RC_MAX_PAYLOAD      1400
#define RC_PKT_DATA         0x0003
#define RC_PKT_RELAY_REG    0x0010
#define RC_PKT_RELAY_ACK    0x0011
#define NUM_STREAMS         2

typedef struct __attribute__((packed)) {
    uint32_t magic; uint16_t version; uint16_t type;
    uint32_t seq; uint32_t ack; uint64_t timestamp;
    uint16_t payload_len; uint16_t flags; uint32_t conn_id;
} pkt_hdr_t;

/* ── Per-stream state ─────────────────────────────────────────────── */
typedef struct {
    uint16_t    stream_id;
    uint32_t    received_packets;
    uint32_t    received_frames;
    uint64_t    received_bytes;
    uint32_t    corrupted_packets;
    uint32_t    invalid_structure;
    uint32_t    missing_frames;
    uint32_t    last_frame_num;
    bool        have_last_frame;
    latency_stats_t lat;
} stream_rx_t;

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

int main(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <relay_ip> <relay_port> <duration_sec> <output_prefix>\n", argv[0]);
        return 1;
    }
    const char *relay_ip = argv[1];
    uint16_t relay_port = (uint16_t)atoi(argv[2]);
    int duration_sec = atoi(argv[3]);
    const char *output_prefix = argv[4];
    if (duration_sec <= 0) duration_sec = 180;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in relay_addr = {
        .sin_family = AF_INET, .sin_port = htons(relay_port),
    };
    if (inet_pton(AF_INET, relay_ip, &relay_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP: %s\n", relay_ip);
        return 1;
    }

    printf("[REMOTE] Connecting to relay %s:%u for %d seconds\n",
           relay_ip, relay_port, duration_sec);
    fflush(stdout);

    /* ── Register with relay (retry up to 5 times) ────────────────── */
    uint32_t conn_id = 0x524F4201;
    bool registered = false;

    for (int attempt = 0; attempt < 5 && !registered; attempt++) {
        pkt_hdr_t hdr = {0};
        hdr.magic = RC_MAGIC; hdr.version = RC_PROTO_VERSION;
        hdr.type = RC_PKT_RELAY_REG; hdr.seq = (uint32_t)attempt;
        hdr.conn_id = conn_id;
        const char *role = "remote";
        hdr.payload_len = (uint16_t)strlen(role);

        uint8_t buf[RC_HEADER_SIZE + 64];
        hdr_pack(buf, &hdr);
        memcpy(buf + RC_HEADER_SIZE, role, strlen(role));
        sendto(fd, buf, RC_HEADER_SIZE + strlen(role), 0,
               (struct sockaddr *)&relay_addr, sizeof(relay_addr));

        uint8_t rbuf[256];
        ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
        if (n >= (ssize_t)RC_HEADER_SIZE) {
            pkt_hdr_t resp;
            if (hdr_unpack(&resp, rbuf) == 0 && resp.type == RC_PKT_RELAY_ACK) {
                printf("[REMOTE] Registered with relay, session_id=%u\n", resp.conn_id);
                fflush(stdout);
                registered = true;
            }
        }
        if (!registered) {
            fprintf(stderr, "[REMOTE] Registration attempt %d failed, retrying...\n", attempt + 1);
            usleep(500000);
        }
    }
    if (!registered) {
        fprintf(stderr, "[REMOTE] Failed to register with relay after 5 attempts\n");
        close(fd);
        return 1;
    }

    /* ── Init stream states ───────────────────────────────────────── */
    stream_rx_t *streams = calloc(NUM_STREAMS, sizeof(stream_rx_t));
    latency_stats_t overall_lat;
    lat_init(&overall_lat);
    for (int i = 0; i < NUM_STREAMS; i++) {
        streams[i].stream_id = (uint16_t)(i + 1);
        lat_init(&streams[i].lat);
    }

    uint64_t total_packets = 0, total_bytes = 0;
    uint32_t protocol_errors = 0;

    /* Receive buffer — heap allocated to avoid stack overflow */
    uint8_t *rbuf = malloc(65536);
    if (!rbuf) { fprintf(stderr, "OOM\n"); return 1; }

    /* ── Main receive loop ────────────────────────────────────────── */
    uint64_t start_ms = now_ms();
    uint64_t end_ms = start_ms + (uint64_t)duration_sec * 1000;
    uint64_t last_report = start_ms;

    printf("[REMOTE] Waiting for data...\n");
    fflush(stdout);

    while (g_running && now_ms() < end_ms) {
        ssize_t n = recv(fd, rbuf, 65536, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            break;
        }
        if (n < (ssize_t)RC_HEADER_SIZE) continue;

        uint64_t recv_ts = now_us();

        pkt_hdr_t hdr;
        if (hdr_unpack(&hdr, rbuf) != 0) { protocol_errors++; continue; }
        if (hdr.type != RC_PKT_DATA) continue;
        if (hdr.payload_len != (uint16_t)(n - RC_HEADER_SIZE)) { protocol_errors++; continue; }
        if (hdr.version != RC_PROTO_VERSION) { protocol_errors++; continue; }

        total_packets++;
        total_bytes += (uint64_t)n;

        /* Parse inner payload: [stream_id(2)] [frame_num(4)] [is_iframe(1)] [h264...] */
        uint16_t plen = hdr.payload_len;
        if (plen < 7) { protocol_errors++; continue; }

        const uint8_t *payload = rbuf + RC_HEADER_SIZE;
        uint16_t stream_id = ((uint16_t)payload[0] << 8) | payload[1];
        uint32_t frame_num = ((uint32_t)payload[2] << 24) | ((uint32_t)payload[3] << 16) |
                             ((uint32_t)payload[4] << 8) | payload[5];
        int is_iframe = payload[6];

        if (stream_id < 1 || stream_id > NUM_STREAMS) { protocol_errors++; continue; }

        stream_rx_t *st = &streams[stream_id - 1];
        st->received_packets++;
        st->received_bytes += (uint64_t)n;

        /* Record latency */
        if (hdr.timestamp > 0) {
            lat_record(&st->lat, hdr.seq, stream_id, hdr.timestamp, recv_ts, plen);
            lat_record(&overall_lat, hdr.seq, stream_id, hdr.timestamp, recv_ts, plen);
        }

        /* Frame tracking — detect new frame by frame_num change */
        if (!st->have_last_frame) {
            st->last_frame_num = frame_num;
            st->have_last_frame = true;
            st->received_frames = 1;
            /* This is the first packet of the first frame — verify structure */
            const uint8_t *h264_data = payload + 7;
            size_t h264_len = plen - 7;
            if (h264_len >= 5) {
                if (h264_verify(h264_data, h264_len) != 0) {
                    st->invalid_structure++;
                } else if (h264_len > 5) {
                    uint32_t mismatches = h264_verify_body(
                        h264_data + 5, h264_len - 5,
                        stream_id, frame_num, is_iframe);
                    if (mismatches > 0) st->corrupted_packets++;
                }
            }
        } else if (frame_num != st->last_frame_num) {
            /* New frame started — this packet is the first of the new frame */
            st->received_frames++;
            if (frame_num > st->last_frame_num + 1)
                st->missing_frames += frame_num - st->last_frame_num - 1;
            st->last_frame_num = frame_num;

            /* Verify H.264 structure of first packet of new frame */
            const uint8_t *h264_data = payload + 7;
            size_t h264_len = plen - 7;
            if (h264_len >= 5) {
                if (h264_verify(h264_data, h264_len) != 0) {
                    st->invalid_structure++;
                } else if (h264_len > 5) {
                    uint32_t mismatches = h264_verify_body(
                        h264_data + 5, h264_len - 5,
                        stream_id, frame_num, is_iframe);
                    if (mismatches > 0) st->corrupted_packets++;
                }
            }
        }
        /* Continuation packets of the same frame are not checked for
         * structure — they are mid-frame data chunks without start codes. */

        /* Periodic report */
        uint64_t now = now_ms();
        if (now - last_report >= 5000) {
            last_report = now;
            printf("[REMOTE] %lus: pkts=%lu S1=%u/%u S2=%u/%u errs=%u\n",
                   (unsigned long)((now - start_ms) / 1000),
                   (unsigned long)total_packets,
                   streams[0].received_packets, streams[0].received_frames,
                   streams[1].received_packets, streams[1].received_frames,
                   protocol_errors);
            fflush(stdout);
        }
    }

    /* ── Generate report ──────────────────────────────────────────── */
    printf("\n[REMOTE] === Final Report ===\n");
    printf("[REMOTE] Total packets: %lu  bytes: %lu  errors: %u\n",
           (unsigned long)total_packets, (unsigned long)total_bytes, protocol_errors);
    fflush(stdout);

    char report_path[256];
    snprintf(report_path, sizeof(report_path), "%s_report.txt", output_prefix);
    FILE *report = fopen(report_path, "w");
    if (!report) { report = stdout; }

    fprintf(report, "═══════════════════════════════════════════════════════════════\n");
    fprintf(report, "  RoboControl Integration Test Report\n");
    fprintf(report, "═══════════════════════════════════════════════════════════════\n\n");
    fprintf(report, "Total packets: %lu\n", (unsigned long)total_packets);
    fprintf(report, "Total bytes:   %lu\n", (unsigned long)total_bytes);
    fprintf(report, "Proto errors:  %u\n\n", protocol_errors);

    for (int i = 0; i < NUM_STREAMS; i++) {
        stream_rx_t *st = &streams[i];
        char label[64];
        snprintf(label, sizeof(label), "Stream %u", st->stream_id);

        double correct_pct = st->received_packets > 0
            ? 100.0 * (1.0 - (double)(st->corrupted_packets + st->invalid_structure) / st->received_packets)
            : 0.0;

        fprintf(report, "─── %s ───\n", label);
        fprintf(report, "  Packets:      %u\n", st->received_packets);
        fprintf(report, "  Frames:       %u\n", st->received_frames);
        fprintf(report, "  Bytes:        %lu\n", (unsigned long)st->received_bytes);
        fprintf(report, "  Corrupted:    %u\n", st->corrupted_packets);
        fprintf(report, "  Bad structure:%u\n", st->invalid_structure);
        fprintf(report, "  Missing frames: %u\n", st->missing_frames);
        fprintf(report, "  Correctness:  %.2f%%\n\n", correct_pct);

        lat_report(&st->lat, label, 0, report);
    }

    lat_report(&overall_lat, "Overall (All Streams)", 0, report);

    /* Throughput */
    double dur_s = (now_ms() - start_ms) / 1000.0;
    fprintf(report, "\n─── Throughput ───\n");
    for (int i = 0; i < NUM_STREAMS; i++) {
        double bps = dur_s > 0 ? (double)streams[i].received_bytes * 8.0 / dur_s : 0;
        fprintf(report, "  Stream %u: %.2f Mbps (%.1f pkt/s)\n",
                streams[i].stream_id, bps / 1e6,
                dur_s > 0 ? streams[i].received_packets / dur_s : 0);
    }
    fprintf(report, "  Total:    %.2f Mbps\n",
            dur_s > 0 ? (double)total_bytes * 8.0 / dur_s / 1e6 : 0);
    fprintf(report, "\n═══════════════════════════════════════════════════════════════\n");

    if (report != stdout) {
        fclose(report);
        printf("[REMOTE] Report: %s\n", report_path);
    }
    fflush(stdout);

    free(streams);
    free(rbuf);
    close(fd);
    return 0;
}
