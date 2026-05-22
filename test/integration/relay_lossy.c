/**
 * @file relay_lossy.c
 * @brief Relay server with configurable packet loss simulation.
 *
 * Usage: ./relay_lossy <port> <loss_pct>
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

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

#define RC_MAGIC            0x524F424FU
#define RC_PROTO_VERSION    1U
#define RC_HEADER_SIZE      32
#define RC_MAX_PAYLOAD      1400
#define RC_PKT_RELAY_REG    0x0010
#define RC_PKT_RELAY_ACK    0x0011
#define RECV_BUF_SIZE       65536

typedef struct __attribute__((packed)) {
    uint32_t magic; uint16_t version; uint16_t type;
    uint32_t seq; uint32_t ack; uint64_t timestamp;
    uint16_t payload_len; uint16_t flags; uint32_t conn_id;
} pkt_hdr_t;

/* ── Single session state ─────────────────────────────────────────── */
#include <sys/poll.h>
static struct sockaddr_storage g_peer_robot;
static socklen_t               g_peer_robot_len;
static struct sockaddr_storage g_peer_remote;
static socklen_t               g_peer_remote_len;
static bool g_robot_registered  = false;
static bool g_remote_registered = false;
static uint32_t g_session_id    = 1;

static volatile bool g_running = true;
static uint32_t g_loss_pct = 0;
static uint64_t g_total_pkts = 0;
static uint64_t g_total_dropped = 0;
static uint64_t g_total_bytes = 0;
static uint32_t g_rng_state = 12345;

static void sig_handler(int sig) { (void)sig; g_running = false; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t xorshift32(void)
{
    g_rng_state ^= g_rng_state << 13;
    g_rng_state ^= g_rng_state >> 17;
    g_rng_state ^= g_rng_state << 5;
    return g_rng_state;
}

static bool addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
    if (a->ss_family != b->ss_family) return false;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *a4 = (const struct sockaddr_in *)a;
        const struct sockaddr_in *b4 = (const struct sockaddr_in *)b;
        return a4->sin_addr.s_addr == b4->sin_addr.s_addr && a4->sin_port == b4->sin_port;
    }
    const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)a;
    const struct sockaddr_in6 *b6 = (const struct sockaddr_in6 *)b;
    return memcmp(&a6->sin6_addr, &b6->sin6_addr, 16) == 0 && a6->sin6_port == b6->sin6_port;
}

static const char *addr_str(const struct sockaddr_storage *ss, char *buf, size_t len)
{
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)ss;
        inet_ntop(AF_INET, &s->sin_addr, buf, len);
        size_t l = strlen(buf);
        snprintf(buf + l, len - l, ":%u", ntohs(s->sin_port));
    } else {
        buf[0] = '?'; buf[1] = 0;
    }
    return buf;
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

int main(int argc, char *argv[])
{
    uint16_t port = 19600;
    g_loss_pct = 0;
    if (argc > 1) port = (uint16_t)atoi(argv[1]);
    if (argc > 2) g_loss_pct = (uint32_t)atoi(argv[2]);
    if (g_loss_pct > 100) g_loss_pct = 100;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Larger receive buffer to avoid drops */
    int bufsize = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    struct sockaddr_in sin = {
        .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(port),
    };
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind"); close(fd); return 1;
    }

    printf("[RELAY] Listening on UDP :%u (loss=%u%%)\n", port, g_loss_pct);
    fflush(stdout);

    g_rng_state = (uint32_t)(now_ms() ^ (port << 16));

    uint8_t *buf = malloc(RECV_BUF_SIZE);
    uint64_t last_stats_ms = now_ms();
    uint64_t fwd_pkts = 0, fwd_bytes = 0;

    while (g_running) {
        /* Use poll to add a timeout so we can check g_running */
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pret = poll(&pfd, 1, 1000);  /* 1s timeout */
        if (pret <= 0) continue;

        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(fd, buf, RECV_BUF_SIZE, 0,
                             (struct sockaddr *)&from, &from_len);
        if (n < (ssize_t)RC_HEADER_SIZE) continue;

        pkt_hdr_t hdr;
        if (hdr_unpack(&hdr, buf) != 0) continue;

        /* Handle registration */
        if (hdr.type == RC_PKT_RELAY_REG) {
            char role[16] = {0};
            if (hdr.payload_len > 0 && hdr.payload_len < 16)
                memcpy(role, buf + RC_HEADER_SIZE, hdr.payload_len);

            char abuf[64];
            addr_str(&from, abuf, sizeof(abuf));

            if (strcmp(role, "robot") == 0) {
                memcpy(&g_peer_robot, &from, from_len);
                g_peer_robot_len = from_len;
                g_robot_registered = true;
                printf("[RELAY] Registered 'robot' from %s\n", abuf);
            } else if (strcmp(role, "remote") == 0) {
                memcpy(&g_peer_remote, &from, from_len);
                g_peer_remote_len = from_len;
                g_remote_registered = true;
                printf("[RELAY] Registered 'remote' from %s\n", abuf);
            }

            if (g_robot_registered && g_remote_registered)
                printf("[RELAY] Session %u READY (robot <-> remote)\n", g_session_id);

            /* Send ACK */
            pkt_hdr_t resp = {0};
            resp.magic = RC_MAGIC; resp.version = RC_PROTO_VERSION;
            resp.type = RC_PKT_RELAY_ACK; resp.conn_id = g_session_id;
            resp.ack = hdr.seq;
            uint8_t resp_buf[RC_HEADER_SIZE];
            hdr_pack(resp_buf, &resp);
            sendto(fd, resp_buf, RC_HEADER_SIZE, 0,
                   (struct sockaddr *)&from, from_len);
            fflush(stdout);
            continue;
        }

        /* Forward data: need both peers registered */
        if (!g_robot_registered || !g_remote_registered) continue;

        /* Determine source and forward direction */
        struct sockaddr_storage *dst;
        socklen_t dst_len;

        if (addr_equal(&from, &g_peer_robot)) {
            dst = &g_peer_remote;
            dst_len = g_peer_remote_len;
        } else if (addr_equal(&from, &g_peer_remote)) {
            dst = &g_peer_robot;
            dst_len = g_peer_robot_len;
        } else {
            continue;  /* Unknown sender */
        }

        /* Simulate packet loss */
        if (g_loss_pct > 0 && (xorshift32() % 100) < g_loss_pct) {
            g_total_dropped++;
            continue;
        }

        /* Forward */
        sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)dst, dst_len);
        fwd_pkts++;
        fwd_bytes += (uint64_t)n;
        g_total_pkts++;
        g_total_bytes += (uint64_t)n;

        /* Periodic stats */
        {
            uint64_t now = now_ms();
            if (now - last_stats_ms >= 5000) {
                printf("[RELAY] %lus: fwd=%lu dropped=%lu bytes=%lu\n",
                       (unsigned long)(now / 1000),
                       (unsigned long)fwd_pkts, (unsigned long)g_total_dropped,
                       (unsigned long)fwd_bytes);
                fflush(stdout);
                last_stats_ms = now;
            }
        }
    }

    printf("\n[RELAY] Shutdown: forwarded=%lu dropped=%lu bytes=%lu\n",
           (unsigned long)fwd_pkts, (unsigned long)g_total_dropped,
           (unsigned long)fwd_bytes);
    fflush(stdout);

    free(buf);
    close(fd);
    return 0;
}
