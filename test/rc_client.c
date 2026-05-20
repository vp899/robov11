/**
 * @file rc_client.c
 * @brief RoboControl test client — connects to server, sends commands.
 *
 * Performs HELLO handshake, then sends a burst of DATA packets
 * and prints ACK responses.  Fully self-contained.
 *
 * Build:
 *   gcc -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -O2 \
 *       -o rc_client rc_client.c
 * Run:
 *   ./rc_client <server_ip> [port]   # default port 9500
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

/* ---- Protocol ---- */
#define RC_MAGIC            0x524F424FU
#define RC_PROTO_VERSION    1U
#define RC_HEADER_SIZE      32
#define RC_MAX_PAYLOAD      1400

typedef enum rc_pkt_type {
    RC_PKT_HELLO      = 0x0001,
    RC_PKT_HELLO_ACK  = 0x0002,
    RC_PKT_DATA       = 0x0003,
    RC_PKT_ACK        = 0x0004,
    RC_PKT_PING       = 0x0006,
    RC_PKT_PONG       = 0x0007,
    RC_PKT_FIN        = 0x00FF,
} rc_pkt_type_t;

typedef struct __attribute__((packed)) rc_pkt_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t seq;
    uint32_t ack;
    uint64_t timestamp;
    uint16_t payload_len;
    uint16_t flags;
    uint32_t conn_id;
} rc_pkt_hdr_t;

_Static_assert(sizeof(rc_pkt_hdr_t) == RC_HEADER_SIZE, "hdr size");

static uint64_t time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *pkt_name(uint16_t t)
{
    switch (t) {
    case RC_PKT_HELLO:     return "HELLO";
    case RC_PKT_HELLO_ACK: return "HELLO_ACK";
    case RC_PKT_DATA:      return "DATA";
    case RC_PKT_ACK:       return "ACK";
    case RC_PKT_PING:      return "PING";
    case RC_PKT_PONG:      return "PONG";
    case RC_PKT_FIN:       return "FIN";
    default:               return "???";
    }
}

static void hdr_pack(uint8_t *d, const rc_pkt_hdr_t *h)
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

static int hdr_unpack(rc_pkt_hdr_t *h, const uint8_t *s)
{
    h->magic=(s[0]<<24)|(s[1]<<16)|(s[2]<<8)|s[3];
    if (h->magic!=RC_MAGIC) return -1;
    h->version=(s[4]<<8)|s[5];
    if (h->version!=RC_PROTO_VERSION) return -1;
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

static ssize_t send_pkt(int fd, const struct sockaddr *dst, socklen_t dst_len,
                         rc_pkt_hdr_t *hdr, const uint8_t *payload,
                         uint16_t payload_len)
{
    uint8_t buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
    hdr->payload_len = payload_len;
    hdr->timestamp   = (uint64_t)time_ms() * 1000;
    hdr_pack(buf, hdr);
    if (payload && payload_len > 0)
        memcpy(buf + RC_HEADER_SIZE, payload, payload_len);
    return sendto(fd, buf, RC_HEADER_SIZE + payload_len, 0, dst, dst_len);
}

static ssize_t recv_pkt(int fd, rc_pkt_hdr_t *hdr, uint8_t *payload,
                         uint32_t timeout_ms)
{
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < (ssize_t)RC_HEADER_SIZE) return n;

    if (hdr_unpack(hdr, buf) != 0) return -1;
    if (payload && hdr->payload_len > 0)
        memcpy(payload, buf + RC_HEADER_SIZE, hdr->payload_len);
    return n;
}

/* ---- Commands to send ---- */
static const char *commands[] = {
    "move_forward 1.5m/s",
    "rotate_left 90deg",
    "camera_snapshot front",
    "arm_extend 0.3m",
    "gripper_close",
    "move_backward 0.5m/s",
    "camera_stream start",
    "battery_status",
    "stop_all",
    "move_forward 2.0m/s",
};
#define CMD_COUNT (sizeof(commands) / sizeof(commands[0]))

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server_ip> [port]\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    uint16_t port = 9500;
    if (argc > 2) port = (uint16_t)atoi(argv[2]);

    uint32_t conn_id = (uint32_t)(time_ms() & 0xFFFF);

    /* Setup socket */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
    };
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP: %s\n", server_ip);
        return 1;
    }

    printf("═══════════════════════════════════════════════════════\n");
    printf(" RoboControl Client -> %s:%u (conn_id=%u)\n",
           server_ip, port, conn_id);
    printf("═══════════════════════════════════════════════════════\n\n");

    uint32_t seq = 0;
    rc_pkt_hdr_t hdr, resp;
    uint8_t payload_buf[RC_MAX_PAYLOAD];

    /* ---- Phase 1: PING test ---- */
    printf("── Phase 1: PING ──────────────────────────────────────\n");
    {
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic   = RC_MAGIC;
        hdr.version = RC_PROTO_VERSION;
        hdr.type    = RC_PKT_PING;
        hdr.seq     = seq++;
        hdr.conn_id = conn_id;

        uint64_t t0 = time_ms();
        send_pkt(fd, (struct sockaddr *)&dst, sizeof(dst),
                 &hdr, NULL, 0);
        printf("[TX] PING  seq=%u\n", hdr.seq);

        ssize_t n = recv_pkt(fd, &resp, payload_buf, 3000);
        uint64_t rtt = time_ms() - t0;

        if (n >= (ssize_t)RC_HEADER_SIZE && resp.type == RC_PKT_PONG) {
            printf("[RX] PONG  ack=%u  RTT=%lu ms  ✓\n\n", resp.ack, rtt);
        } else {
            printf("[RX] TIMEOUT or bad response  ✗\n\n");
        }
    }

    /* ---- Phase 2: Handshake ---- */
    printf("── Phase 2: Handshake (HELLO → HELLO_ACK) ────────────\n");
    {
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic   = RC_MAGIC;
        hdr.version = RC_PROTO_VERSION;
        hdr.type    = RC_PKT_HELLO;
        hdr.seq     = seq++;
        hdr.conn_id = conn_id;

        send_pkt(fd, (struct sockaddr *)&dst, sizeof(dst),
                 &hdr, NULL, 0);
        printf("[TX] HELLO  seq=%u conn=%u\n", hdr.seq, hdr.conn_id);

        ssize_t n = recv_pkt(fd, &resp, payload_buf, 3000);
        if (n >= (ssize_t)RC_HEADER_SIZE && resp.type == RC_PKT_HELLO_ACK) {
            printf("[RX] HELLO_ACK  ack=%u conn=%u  ✓\n",
                   resp.ack, resp.conn_id);
            printf("  ✓ Connection ESTABLISHED\n\n");
        } else {
            printf("[RX] TIMEOUT or bad response  ✗\n\n");
            close(fd);
            return 1;
        }
    }

    /* ---- Phase 3: Data exchange ---- */
    printf("── Phase 3: DATA exchange (%lu commands) ─────────────\n",
           (unsigned long)CMD_COUNT);

    uint32_t acked = 0;
    uint64_t total_rtt = 0;

    for (size_t i = 0; i < CMD_COUNT; i++) {
        size_t len = strlen(commands[i]);

        memset(&hdr, 0, sizeof(hdr));
        hdr.magic   = RC_MAGIC;
        hdr.version = RC_PROTO_VERSION;
        hdr.type    = RC_PKT_DATA;
        hdr.seq     = seq++;
        hdr.conn_id = conn_id;

        uint64_t t0 = time_ms();
        send_pkt(fd, (struct sockaddr *)&dst, sizeof(dst),
                 &hdr, (const uint8_t *)commands[i], (uint16_t)len);
        printf("[TX] DATA  seq=%-3u  \"%s\"\n", hdr.seq, commands[i]);

        ssize_t n = recv_pkt(fd, &resp, payload_buf, 3000);
        uint64_t rtt = time_ms() - t0;

        if (n >= (ssize_t)RC_HEADER_SIZE && resp.type == RC_PKT_ACK &&
            resp.ack == hdr.seq) {
            printf("      └─ ACK  ack=%u  RTT=%lu ms  ✓\n", resp.ack, rtt);
            acked++;
            total_rtt += rtt;
        } else {
            printf("      └─ TIMEOUT  ✗\n");
        }

        usleep(50000);  /* 50ms between commands */
    }

    /* ---- Phase 4: FIN ---- */
    printf("\n── Phase 4: Graceful close (FIN) ─────────────────────\n");
    {
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic   = RC_MAGIC;
        hdr.version = RC_PROTO_VERSION;
        hdr.type    = RC_PKT_FIN;
        hdr.seq     = seq++;
        hdr.conn_id = conn_id;

        send_pkt(fd, (struct sockaddr *)&dst, sizeof(dst),
                 &hdr, NULL, 0);
        printf("[TX] FIN  seq=%u conn=%u\n", hdr.seq, hdr.conn_id);

        ssize_t n = recv_pkt(fd, &resp, payload_buf, 3000);
        if (n >= (ssize_t)RC_HEADER_SIZE) {
            printf("[RX] ACK  ack=%u  ✓\n", resp.ack);
        }
    }

    /* ---- Summary ---- */
    printf("\n═══════════════════════════════════════════════════════\n");
    printf(" Summary\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Commands sent:  %lu\n", (unsigned long)CMD_COUNT);
    printf("  ACKs received:  %u / %lu\n", acked, (unsigned long)CMD_COUNT);
    if (acked > 0)
        printf("  Avg RTT:        %lu ms\n", (unsigned long)(total_rtt / acked));
    printf("  Status:         %s\n",
           acked == CMD_COUNT ? "ALL PASSED ✓" : "SOME FAILED ✗");
    printf("═══════════════════════════════════════════════════════\n");

    close(fd);
    return (acked == CMD_COUNT) ? 0 : 1;
}
