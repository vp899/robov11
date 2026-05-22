/**
 * @file rc_server.c
 * @brief RoboControl test server — handles handshake + data echo.
 *
 * Listens on UDP, processes HELLO → sends HELLO_ACK, then echoes
 * DATA packets back with ACK.  Fully self-contained, no external deps.
 *
 * Build:
 *   gcc -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -O2 \
 *       -o rc_server rc_server.c
 * Run:
 *   ./rc_server [port]        # default 9500
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

/* ---- Protocol constants ---- */
#define RC_MAGIC            0x524F424FU
#define RC_PROTO_VERSION    1U
#define RC_HEADER_SIZE      32
#define RC_MAX_PAYLOAD      1400

typedef enum rc_pkt_type {
    RC_PKT_HELLO      = 0x0001,
    RC_PKT_HELLO_ACK  = 0x0002,
    RC_PKT_DATA       = 0x0003,
    RC_PKT_ACK        = 0x0004,
    RC_PKT_NACK       = 0x0005,
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

static volatile bool g_running = true;

static void sig_handler(int sig) { (void)sig; g_running = false; }

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

/* ---- Session tracking ---- */
typedef struct {
    struct sockaddr_storage addr;
    socklen_t               addr_len;
    uint32_t                conn_id;
    uint32_t                client_seq;   /* last seq from client */
    uint32_t                server_seq;   /* our outgoing seq */
    bool                    established;
    uint64_t                last_seen_ms;
    uint64_t                pkts_recv;
    uint64_t                pkts_sent;
    uint64_t                bytes_recv;
} session_t;

#define MAX_SESSIONS 64
static session_t g_sessions[MAX_SESSIONS];
static int       g_session_count = 0;

static session_t *find_or_create(const struct sockaddr_storage *addr,
                                  socklen_t addr_len, uint32_t conn_id)
{
    for (int i = 0; i < g_session_count; i++) {
        if (g_sessions[i].conn_id == conn_id &&
            g_sessions[i].addr_len == addr_len &&
            memcmp(&g_sessions[i].addr, addr, addr_len) == 0)
            return &g_sessions[i];
    }
    if (g_session_count >= MAX_SESSIONS) return NULL;
    session_t *s = &g_sessions[g_session_count++];
    memset(s, 0, sizeof(*s));
    memcpy(&s->addr, addr, addr_len);
    s->addr_len = addr_len;
    s->conn_id  = conn_id;
    return s;
}

int main(int argc, char *argv[])
{
    uint16_t port = 9500;
    if (argc > 1) port = (uint16_t)atoi(argv[1]);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sin = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(port),
    };
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind"); close(fd); return 1;
    }

    printf("═══════════════════════════════════════════════════════\n");
    printf(" RoboControl Server listening on UDP :%u\n", port);
    printf("═══════════════════════════════════════════════════════\n\n");

    uint8_t buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];

    while (g_running) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);

        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &from_len);
        if (n < (ssize_t)RC_HEADER_SIZE) continue;

        rc_pkt_hdr_t hdr;
        if (hdr_unpack(&hdr, buf) != 0) {
            fprintf(stderr, "[WARN] bad packet (magic mismatch)\n");
            continue;
        }

        char addr_str[64] = "?";
        if (from.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&from;
            inet_ntop(AF_INET, &s->sin_addr, addr_str, sizeof(addr_str));
            snprintf(addr_str + strlen(addr_str),
                     sizeof(addr_str) - strlen(addr_str),
                     ":%u", ntohs(s->sin_port));
        }

        printf("[RX] %-10s seq=%-6u ack=%-6u conn=%-6u plen=%-4u from %s\n",
               pkt_name(hdr.type), hdr.seq, hdr.ack, hdr.conn_id,
               hdr.payload_len, addr_str);

        session_t *sess = find_or_create(&from, from_len, hdr.conn_id);
        if (!sess) continue;
        sess->last_seen_ms = time_ms();
        sess->pkts_recv++;
        sess->bytes_recv += (uint64_t)n;

        rc_pkt_hdr_t resp;
        uint8_t resp_buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];

        switch (hdr.type) {
        case RC_PKT_PING:
            /* Reply with PONG */
            memset(&resp, 0, sizeof(resp));
            resp.magic     = RC_MAGIC;
            resp.version   = RC_PROTO_VERSION;
            resp.type      = RC_PKT_PONG;
            resp.ack       = hdr.seq;
            resp.timestamp = (uint64_t)time_ms() * 1000;
            hdr_pack(resp_buf, &resp);
            sendto(fd, resp_buf, RC_HEADER_SIZE, 0,
                   (struct sockaddr *)&from, from_len);
            sess->pkts_sent++;
            printf("[TX] PONG     ack=%u -> %s\n", hdr.seq, addr_str);
            break;

        case RC_PKT_HELLO:
            /* Handshake: reply HELLO_ACK */
            memset(&resp, 0, sizeof(resp));
            resp.magic     = RC_MAGIC;
            resp.version   = RC_PROTO_VERSION;
            resp.type      = RC_PKT_HELLO_ACK;
            resp.seq       = sess->server_seq++;
            resp.ack       = hdr.seq;
            resp.conn_id   = hdr.conn_id;
            resp.timestamp = (uint64_t)time_ms() * 1000;
            resp.payload_len = 0;
            hdr_pack(resp_buf, &resp);
            sendto(fd, resp_buf, RC_HEADER_SIZE, 0,
                   (struct sockaddr *)&from, from_len);
            sess->established = true;
            sess->client_seq  = hdr.seq;
            sess->pkts_sent++;
            printf("[TX] HELLO_ACK ack=%u conn=%u -> %s\n",
                   hdr.seq, hdr.conn_id, addr_str);
            printf("  ✓ Session ESTABLISHED (conn_id=%u)\n\n", hdr.conn_id);
            break;

        case RC_PKT_DATA:
            if (!sess->established) {
                fprintf(stderr, "[WARN] DATA before HELLO, ignoring\n");
                break;
            }
            /* Print payload */
            if (hdr.payload_len > 0 && hdr.payload_len <= n - RC_HEADER_SIZE) {
                char msg[RC_MAX_PAYLOAD + 1];
                memcpy(msg, buf + RC_HEADER_SIZE, hdr.payload_len);
                msg[hdr.payload_len] = '\0';
                printf("  └─ payload: \"%s\"\n", msg);
            }
            sess->client_seq = hdr.seq;

            /* Send ACK */
            memset(&resp, 0, sizeof(resp));
            resp.magic     = RC_MAGIC;
            resp.version   = RC_PROTO_VERSION;
            resp.type      = RC_PKT_ACK;
            resp.seq       = sess->server_seq++;
            resp.ack       = hdr.seq;
            resp.conn_id   = hdr.conn_id;
            resp.timestamp = (uint64_t)time_ms() * 1000;
            hdr_pack(resp_buf, &resp);
            sendto(fd, resp_buf, RC_HEADER_SIZE, 0,
                   (struct sockaddr *)&from, from_len);
            sess->pkts_sent++;
            printf("[TX] ACK      ack=%u conn=%u\n", hdr.seq, hdr.conn_id);
            break;

        case RC_PKT_FIN:
            printf("  ✗ Session CLOSED (conn_id=%u)\n\n", hdr.conn_id);
            /* Send FIN_ACK */
            memset(&resp, 0, sizeof(resp));
            resp.magic   = RC_MAGIC;
            resp.version = RC_PROTO_VERSION;
            resp.type    = RC_PKT_ACK;
            resp.ack     = hdr.seq;
            resp.conn_id = hdr.conn_id;
            hdr_pack(resp_buf, &resp);
            sendto(fd, resp_buf, RC_HEADER_SIZE, 0,
                   (struct sockaddr *)&from, from_len);
            sess->established = false;
            break;

        default:
            fprintf(stderr, "[WARN] unhandled type 0x%04X\n", hdr.type);
            break;
        }
    }

    printf("\n[STAT] Server shutting down. Sessions served: %d\n",
           g_session_count);
    for (int i = 0; i < g_session_count; i++) {
        printf("  conn_id=%u  pkts_rx=%lu  pkts_tx=%lu  bytes_rx=%lu\n",
               g_sessions[i].conn_id,
               (unsigned long)g_sessions[i].pkts_recv,
               (unsigned long)g_sessions[i].pkts_sent,
               (unsigned long)g_sessions[i].bytes_recv);
    }

    close(fd);
    return 0;
}
