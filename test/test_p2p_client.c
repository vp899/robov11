/**
 * @file test_p2p_client.c
 * @brief Self-contained P2P client test — no external library deps.
 *
 * Re-implements the minimal protocol helpers (hdr pack/unpack, CRC32)
 * so the test compiles standalone without OpenSSL/librobocontrol.
 *
 * Tests: packet build/parse, PING↔PONG exchange, DATA send/recv,
 *        multi-packet burst — all over loopback UDP.
 *
 * Build:
 *   gcc -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2 \
 *       -o test_p2p_client test_p2p_client.c
 * Run:
 *   ./test_p2p_client
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

/* ================================================================== */
/*  Constants (mirrored from rc_proto.h)                               */
/* ================================================================== */

#define RC_MAGIC            0x524F424FU   /* "ROBO" */
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
    RC_PKT_RELAY_REQ  = 0x0008,
    RC_PKT_RELAY_ACK  = 0x0009,
    RC_PKT_AUTH_REQ   = 0x000A,
    RC_PKT_AUTH_ACK   = 0x000B,
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

_Static_assert(sizeof(rc_pkt_hdr_t) == RC_HEADER_SIZE,
               "header must be 32 bytes");

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static uint64_t time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *pkt_type_name(uint16_t type)
{
    switch (type) {
    case RC_PKT_HELLO:     return "HELLO";
    case RC_PKT_HELLO_ACK: return "HELLO_ACK";
    case RC_PKT_DATA:      return "DATA";
    case RC_PKT_ACK:       return "ACK";
    case RC_PKT_NACK:      return "NACK";
    case RC_PKT_PING:      return "PING";
    case RC_PKT_PONG:      return "PONG";
    case RC_PKT_RELAY_REQ: return "RELAY_REQ";
    case RC_PKT_RELAY_ACK: return "RELAY_ACK";
    case RC_PKT_AUTH_REQ:  return "AUTH_REQ";
    case RC_PKT_AUTH_ACK:  return "AUTH_ACK";
    case RC_PKT_FIN:       return "FIN";
    default:               return "UNKNOWN";
    }
}

/* ---- Header init / pack / unpack ---- */

static void rc_hdr_init(rc_pkt_hdr_t *hdr, rc_pkt_type_t type,
                        uint32_t seq, uint32_t conn)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic   = RC_MAGIC;
    hdr->version = RC_PROTO_VERSION;
    hdr->type    = (uint16_t)type;
    hdr->seq     = seq;
    hdr->conn_id = conn;
    hdr->timestamp = (uint64_t)time_ms() * 1000; /* µs */
}

static void rc_hdr_pack(uint8_t *dst, const rc_pkt_hdr_t *hdr)
{
    /* Network byte order */
    dst[0]  = (hdr->magic >> 24) & 0xFF;
    dst[1]  = (hdr->magic >> 16) & 0xFF;
    dst[2]  = (hdr->magic >> 8)  & 0xFF;
    dst[3]  =  hdr->magic & 0xFF;

    dst[4]  = (hdr->version >> 8) & 0xFF;
    dst[5]  =  hdr->version & 0xFF;

    dst[6]  = (hdr->type >> 8) & 0xFF;
    dst[7]  =  hdr->type & 0xFF;

    dst[8]  = (hdr->seq >> 24) & 0xFF;
    dst[9]  = (hdr->seq >> 16) & 0xFF;
    dst[10] = (hdr->seq >> 8)  & 0xFF;
    dst[11] =  hdr->seq & 0xFF;

    dst[12] = (hdr->ack >> 24) & 0xFF;
    dst[13] = (hdr->ack >> 16) & 0xFF;
    dst[14] = (hdr->ack >> 8)  & 0xFF;
    dst[15] =  hdr->ack & 0xFF;

    for (int i = 0; i < 8; i++)
        dst[16 + i] = (hdr->timestamp >> (56 - 8 * i)) & 0xFF;

    dst[24] = (hdr->payload_len >> 8) & 0xFF;
    dst[25] =  hdr->payload_len & 0xFF;

    dst[26] = (hdr->flags >> 8) & 0xFF;
    dst[27] =  hdr->flags & 0xFF;

    dst[28] = (hdr->conn_id >> 24) & 0xFF;
    dst[29] = (hdr->conn_id >> 16) & 0xFF;
    dst[30] = (hdr->conn_id >> 8)  & 0xFF;
    dst[31] =  hdr->conn_id & 0xFF;
}

static int rc_hdr_unpack(rc_pkt_hdr_t *hdr, const uint8_t *src)
{
    hdr->magic = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
                 ((uint32_t)src[2] << 8)  |  (uint32_t)src[3];

    if (hdr->magic != RC_MAGIC) return -1;

    hdr->version = ((uint16_t)src[4] << 8) | src[5];
    if (hdr->version != RC_PROTO_VERSION) return -1;

    hdr->type = ((uint16_t)src[6] << 8) | src[7];
    hdr->seq  = ((uint32_t)src[8] << 24) | ((uint32_t)src[9] << 16) |
                ((uint32_t)src[10] << 8) | (uint32_t)src[11];
    hdr->ack  = ((uint32_t)src[12] << 24) | ((uint32_t)src[13] << 16) |
                ((uint32_t)src[14] << 8) | (uint32_t)src[15];

    hdr->timestamp = 0;
    for (int i = 0; i < 8; i++)
        hdr->timestamp = (hdr->timestamp << 8) | src[16 + i];

    hdr->payload_len = ((uint16_t)src[24] << 8) | src[25];
    hdr->flags       = ((uint16_t)src[26] << 8) | src[27];
    hdr->conn_id     = ((uint32_t)src[28] << 24) | ((uint32_t)src[29] << 16) |
                       ((uint32_t)src[30] << 8) | (uint32_t)src[31];

    return 0;
}

/* ---- UDP helpers ---- */

static int udp_bind_loopback(uint16_t *out_port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);

    struct sockaddr_in sin = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port        = 0,
    };
    assert(bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0);

    socklen_t slen = sizeof(sin);
    getsockname(fd, (struct sockaddr *)&sin, &slen);
    *out_port = ntohs(sin.sin_port);
    return fd;
}

static ssize_t udp_send_to(int fd, uint16_t port,
                            const uint8_t *data, size_t len)
{
    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port        = htons(port),
    };
    return sendto(fd, data, len, 0,
                  (struct sockaddr *)&dst, sizeof(dst));
}

static ssize_t udp_recv_timeout(int fd, uint8_t *buf, size_t len,
                                 uint32_t timeout_ms)
{
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return recv(fd, buf, len, 0);
}

/* ================================================================== */
/*  Test 1: P2P create/destroy lifecycle                               */
/* ================================================================== */
static void test_p2p_lifecycle(void)
{
    printf("[TEST] P2P lifecycle ... ");

    /* Simulate rc_p2p_create: allocate + bind a UDP socket */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);

    struct sockaddr_in sin = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = 0,
    };
    assert(bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0);

    /* Get bound port */
    socklen_t slen = sizeof(sin);
    getsockname(fd, (struct sockaddr *)&sin, &slen);
    assert(sin.sin_port != 0);

    close(fd);
    printf("PASS (port %u)\n", ntohs(sin.sin_port));
}

/* ================================================================== */
/*  Test 2: PING/PONG packet build & parse                             */
/* ================================================================== */
static void test_ping_pong_packet(void)
{
    printf("[TEST] PING/PONG packet build & parse ... ");

    /* Build PING */
    rc_pkt_hdr_t ping;
    rc_hdr_init(&ping, RC_PKT_PING, 1, 0);
    ping.payload_len = 0;

    uint8_t wire[RC_HEADER_SIZE];
    rc_hdr_pack(wire, &ping);

    /* Parse */
    rc_pkt_hdr_t parsed;
    assert(rc_hdr_unpack(&parsed, wire) == 0);
    assert(parsed.type == RC_PKT_PING);
    assert(parsed.seq == 1);
    assert(parsed.payload_len == 0);
    assert(parsed.magic == RC_MAGIC);

    /* Build PONG */
    rc_pkt_hdr_t pong;
    rc_hdr_init(&pong, RC_PKT_PONG, 0, 0);
    pong.ack = parsed.seq;

    uint8_t wire2[RC_HEADER_SIZE];
    rc_hdr_pack(wire2, &pong);

    rc_pkt_hdr_t parsed2;
    assert(rc_hdr_unpack(&parsed2, wire2) == 0);
    assert(parsed2.type == RC_PKT_PONG);
    assert(parsed2.ack == 1);

    printf("PASS\n");
}

/* ================================================================== */
/*  Test 3: DATA packet with payload                                   */
/* ================================================================== */
static void test_data_packet(void)
{
    printf("[TEST] DATA packet with payload ... ");

    const char *msg = "hello from controller";
    size_t msg_len = strlen(msg);

    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_DATA, 42, 1001);
    hdr.payload_len = (uint16_t)msg_len;

    uint8_t buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
    rc_hdr_pack(buf, &hdr);
    memcpy(buf + RC_HEADER_SIZE, msg, msg_len);

    /* Parse back */
    rc_pkt_hdr_t parsed;
    assert(rc_hdr_unpack(&parsed, buf) == 0);
    assert(parsed.type == RC_PKT_DATA);
    assert(parsed.seq == 42);
    assert(parsed.conn_id == 1001);
    assert(parsed.payload_len == msg_len);
    assert(memcmp(buf + RC_HEADER_SIZE, msg, msg_len) == 0);

    printf("PASS\n");
}

/* ================================================================== */
/*  Test 4: PING↔PONG exchange over loopback UDP                       */
/* ================================================================== */
static void test_ping_pong_loopback(void)
{
    printf("[TEST] PING↔PONG exchange (loopback) ... ");

    uint16_t robot_port = 0;
    int robot_fd = udp_bind_loopback(&robot_port);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* ---- Child: robot responder (reuse inherited fd) ---- */
        int resp_fd = robot_fd;  /* parent's fd was inherited across fork */

        uint8_t recv_buf[RC_HEADER_SIZE + 64];
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);

        ssize_t n = recvfrom(resp_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&peer, &peer_len);
        if (n < (ssize_t)RC_HEADER_SIZE) _exit(1);

        rc_pkt_hdr_t ping_hdr;
        if (rc_hdr_unpack(&ping_hdr, recv_buf) != 0 ||
            ping_hdr.type != RC_PKT_PING) {
            _exit(2);
        }

        /* Reply with PONG */
        rc_pkt_hdr_t pong;
        rc_hdr_init(&pong, RC_PKT_PONG, 0, 0);
        pong.ack = ping_hdr.seq;
        pong.payload_len = 0;

        uint8_t pong_buf[RC_HEADER_SIZE];
        rc_hdr_pack(pong_buf, &pong);
        sendto(resp_fd, pong_buf, sizeof(pong_buf), 0,
               (struct sockaddr *)&peer, peer_len);

        fprintf(stderr, "  [robot] replied PONG (ack=%u)\n", ping_hdr.seq);
        close(resp_fd);
        _exit(0);
    }

    /* ---- Parent: client ---- */
    usleep(50000);

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    /* bind client to ephemeral port */
    struct sockaddr_in caddr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port        = 0,
    };
    assert(bind(client_fd, (struct sockaddr *)&caddr, sizeof(caddr)) == 0);

    /* Send PING */
    rc_pkt_hdr_t ping;
    rc_hdr_init(&ping, RC_PKT_PING, 1, 0);
    ping.payload_len = 0;

    uint8_t ping_buf[RC_HEADER_SIZE];
    rc_hdr_pack(ping_buf, &ping);

    ssize_t sent = udp_send_to(client_fd, robot_port, ping_buf, sizeof(ping_buf));
    assert(sent == (ssize_t)sizeof(ping_buf));
    printf("\n  [client] sent PING (seq=1, port=%u) -> robot port %u\n",
           0, robot_port);

    /* Recv PONG */
    uint8_t recv_buf[RC_HEADER_SIZE + 64];
    ssize_t n = udp_recv_timeout(client_fd, recv_buf, sizeof(recv_buf), 3000);
    assert(n >= (ssize_t)RC_HEADER_SIZE);

    rc_pkt_hdr_t pong_hdr;
    assert(rc_hdr_unpack(&pong_hdr, recv_buf) == 0);
    assert(pong_hdr.type == RC_PKT_PONG);
    assert(pong_hdr.ack == 1);
    printf("  [client] got PONG (ack=%u)\n", pong_hdr.ack);

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(client_fd);
    close(robot_fd);
    printf("  PASS\n");
}

/* ================================================================== */
/*  Test 5: DATA send/recv + ACK over loopback                         */
/* ================================================================== */
static void test_data_send_recv(void)
{
    printf("[TEST] DATA send/recv + ACK (loopback) ... ");

    uint16_t robot_port = 0;
    int robot_fd = udp_bind_loopback(&robot_port);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* ---- Child: robot (reuse inherited fd) ---- */
        int resp_fd = robot_fd;

        uint8_t recv_buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);

        ssize_t n = recvfrom(resp_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&peer, &peer_len);
        if (n < (ssize_t)RC_HEADER_SIZE) _exit(1);

        rc_pkt_hdr_t hdr;
        if (rc_hdr_unpack(&hdr, recv_buf) != 0 || hdr.type != RC_PKT_DATA)
            _exit(2);

        /* Print received payload */
        if (hdr.payload_len > 0) {
            char msg[RC_MAX_PAYLOAD + 1];
            memcpy(msg, recv_buf + RC_HEADER_SIZE, hdr.payload_len);
            msg[hdr.payload_len] = '\0';
            fprintf(stderr, "  [robot] got DATA: \"%s\" (seq=%u, conn=%u)\n",
                    msg, hdr.seq, hdr.conn_id);
        }

        /* Send ACK */
        rc_pkt_hdr_t ack;
        rc_hdr_init(&ack, RC_PKT_ACK, 0, hdr.conn_id);
        ack.ack = hdr.seq;
        ack.payload_len = 0;

        uint8_t ack_buf[RC_HEADER_SIZE];
        rc_hdr_pack(ack_buf, &ack);
        sendto(resp_fd, ack_buf, sizeof(ack_buf), 0,
               (struct sockaddr *)&peer, peer_len);

        fprintf(stderr, "  [robot] sent ACK (ack=%u)\n", hdr.seq);
        close(resp_fd);
        _exit(0);
    }

    /* ---- Parent: client ---- */
    usleep(50000);

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in caddr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port        = 0,
    };
    assert(bind(client_fd, (struct sockaddr *)&caddr, sizeof(caddr)) == 0);

    const char *msg = "RoboControl: move_forward 1.5m/s";
    size_t msg_len = strlen(msg);

    rc_pkt_hdr_t data_hdr;
    rc_hdr_init(&data_hdr, RC_PKT_DATA, 100, 2001);
    data_hdr.payload_len = (uint16_t)msg_len;

    uint8_t pkt[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
    rc_hdr_pack(pkt, &data_hdr);
    memcpy(pkt + RC_HEADER_SIZE, msg, msg_len);

    ssize_t sent = udp_send_to(client_fd, robot_port,
                                pkt, RC_HEADER_SIZE + msg_len);
    assert(sent == (ssize_t)(RC_HEADER_SIZE + msg_len));
    printf("\n  [client] sent DATA: \"%s\" (seq=%u, conn=%u)\n",
           msg, data_hdr.seq, data_hdr.conn_id);

    /* Wait for ACK */
    uint8_t recv_buf[RC_HEADER_SIZE + 64];
    ssize_t n = udp_recv_timeout(client_fd, recv_buf, sizeof(recv_buf), 3000);
    assert(n >= (ssize_t)RC_HEADER_SIZE);

    rc_pkt_hdr_t ack_hdr;
    assert(rc_hdr_unpack(&ack_hdr, recv_buf) == 0);
    assert(ack_hdr.type == RC_PKT_ACK);
    assert(ack_hdr.ack == 100);
    assert(ack_hdr.conn_id == 2001);
    printf("  [client] got ACK (ack=%u, conn=%u)\n", ack_hdr.ack, ack_hdr.conn_id);

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(client_fd);
    close(robot_fd);
    printf("  PASS\n");
}

/* ================================================================== */
/*  Test 6: Multi-packet burst (10 DATA → 10 ACK)                      */
/* ================================================================== */
static void test_multi_packet_burst(void)
{
    printf("[TEST] Multi-packet burst (10 pkts) ... ");

    #define BURST_COUNT 10

    uint16_t robot_port = 0;
    int robot_fd = udp_bind_loopback(&robot_port);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* ---- Child: robot (reuse inherited fd) ---- */
        int resp_fd = robot_fd;

        uint8_t recv_buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);

        for (int i = 0; i < BURST_COUNT; i++) {
            ssize_t n = recvfrom(resp_fd, recv_buf, sizeof(recv_buf), 0,
                                 (struct sockaddr *)&peer, &peer_len);
            if (n < (ssize_t)RC_HEADER_SIZE) _exit(1);

            rc_pkt_hdr_t hdr;
            if (rc_hdr_unpack(&hdr, recv_buf) != 0 ||
                hdr.type != RC_PKT_DATA) _exit(2);

            /* ACK */
            rc_pkt_hdr_t ack;
            rc_hdr_init(&ack, RC_PKT_ACK, 0, hdr.conn_id);
            ack.ack = hdr.seq;
            ack.payload_len = 0;

            uint8_t ack_buf[RC_HEADER_SIZE];
            rc_hdr_pack(ack_buf, &ack);
            sendto(resp_fd, ack_buf, sizeof(ack_buf), 0,
                   (struct sockaddr *)&peer, peer_len);
        }

        close(resp_fd);
        _exit(0);
    }

    /* ---- Parent: client ---- */
    usleep(50000);

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in caddr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port        = 0,
    };
    assert(bind(client_fd, (struct sockaddr *)&caddr, sizeof(caddr)) == 0);

    uint64_t t_start = time_ms();

    for (int i = 0; i < BURST_COUNT; i++) {
        char msg[64];
        int msg_len = snprintf(msg, sizeof(msg),
                               "cmd_%03d: throttle=0.%d", i, i);

        rc_pkt_hdr_t hdr;
        rc_hdr_init(&hdr, RC_PKT_DATA, (uint32_t)(i + 1), 3001);
        hdr.payload_len = (uint16_t)msg_len;

        uint8_t pkt[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
        rc_hdr_pack(pkt, &hdr);
        memcpy(pkt + RC_HEADER_SIZE, msg, (size_t)msg_len);

        ssize_t sent = udp_send_to(client_fd, robot_port,
                                    pkt, RC_HEADER_SIZE + (size_t)msg_len);
        assert(sent == (ssize_t)(RC_HEADER_SIZE + (size_t)msg_len));
    }

    printf("\n");
    for (int i = 0; i < BURST_COUNT; i++)
        printf("  [client] sent DATA seq=%d\n", i + 1);

    /* Collect ACKs */
    uint32_t acked = 0;
    for (int i = 0; i < BURST_COUNT; i++) {
        uint8_t recv_buf[RC_HEADER_SIZE + 64];
        ssize_t n = udp_recv_timeout(client_fd, recv_buf,
                                      sizeof(recv_buf), 2000);
        if (n >= (ssize_t)RC_HEADER_SIZE) {
            rc_pkt_hdr_t ack_hdr;
            if (rc_hdr_unpack(&ack_hdr, recv_buf) == 0 &&
                ack_hdr.type == RC_PKT_ACK) {
                acked++;
                printf("  [client] got ACK ack=%u\n", ack_hdr.ack);
            }
        }
    }

    uint64_t elapsed = time_ms() - t_start;
    printf("  %u/%d ACKs received in %lu ms\n", acked, BURST_COUNT, elapsed);

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(client_fd);
    close(robot_fd);
    assert(acked == BURST_COUNT);

    #undef BURST_COUNT
    printf("  PASS\n");
}

/* ================================================================== */
/*  Test 7: Large payload (near MTU)                                   */
/* ================================================================== */
static void test_large_payload(void)
{
    printf("[TEST] Large payload (1400 bytes) ... ");

    uint16_t robot_port = 0;
    int robot_fd = udp_bind_loopback(&robot_port);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* ---- Child: robot (reuse inherited fd) ---- */
        int resp_fd = robot_fd;

        uint8_t recv_buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);

        ssize_t n = recvfrom(resp_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&peer, &peer_len);
        if (n < (ssize_t)RC_HEADER_SIZE) _exit(1);

        rc_pkt_hdr_t hdr;
        if (rc_hdr_unpack(&hdr, recv_buf) != 0 || hdr.type != RC_PKT_DATA)
            _exit(2);

        /* Verify payload content */
        const uint8_t *payload = recv_buf + RC_HEADER_SIZE;
        for (uint16_t i = 0; i < hdr.payload_len; i++) {
            if (payload[i] != (uint8_t)(i & 0xFF)) _exit(3);
        }

        /* ACK */
        rc_pkt_hdr_t ack;
        rc_hdr_init(&ack, RC_PKT_ACK, 0, hdr.conn_id);
        ack.ack = hdr.seq;
        ack.payload_len = 0;

        uint8_t ack_buf[RC_HEADER_SIZE];
        rc_hdr_pack(ack_buf, &ack);
        sendto(resp_fd, ack_buf, sizeof(ack_buf), 0,
               (struct sockaddr *)&peer, peer_len);

        close(resp_fd);
        _exit(0);
    }

    usleep(50000);

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in caddr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port        = 0,
    };
    assert(bind(client_fd, (struct sockaddr *)&caddr, sizeof(caddr)) == 0);

    /* Build 1400-byte payload (pattern fill) */
    uint8_t payload[RC_MAX_PAYLOAD];
    for (int i = 0; i < RC_MAX_PAYLOAD; i++)
        payload[i] = (uint8_t)(i & 0xFF);

    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_DATA, 500, 4001);
    hdr.payload_len = RC_MAX_PAYLOAD;

    uint8_t pkt[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
    rc_hdr_pack(pkt, &hdr);
    memcpy(pkt + RC_HEADER_SIZE, payload, RC_MAX_PAYLOAD);

    ssize_t sent = udp_send_to(client_fd, robot_port,
                                pkt, RC_HEADER_SIZE + RC_MAX_PAYLOAD);
    assert(sent == (ssize_t)(RC_HEADER_SIZE + RC_MAX_PAYLOAD));
    printf("\n  [client] sent %d-byte payload (seq=%u)\n",
           RC_MAX_PAYLOAD, hdr.seq);

    uint8_t recv_buf[RC_HEADER_SIZE + 64];
    ssize_t n = udp_recv_timeout(client_fd, recv_buf, sizeof(recv_buf), 3000);
    assert(n >= (ssize_t)RC_HEADER_SIZE);

    rc_pkt_hdr_t ack_hdr;
    assert(rc_hdr_unpack(&ack_hdr, recv_buf) == 0);
    assert(ack_hdr.type == RC_PKT_ACK);
    assert(ack_hdr.ack == 500);
    printf("  [client] got ACK (ack=%u)\n", ack_hdr.ack);

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(client_fd);
    close(robot_fd);
    printf("  PASS\n");
}

/* ================================================================== */
/*  Main                                                               */
/* ================================================================== */
int main(void)
{
    printf("═══════════════════════════════════════════════════════\n");
    printf(" RoboControl P2P Client Test Suite\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    test_p2p_lifecycle();
    test_ping_pong_packet();
    test_data_packet();
    test_ping_pong_loopback();
    test_data_send_recv();
    test_multi_packet_burst();
    test_large_payload();

    printf("\n═══════════════════════════════════════════════════════\n");
    printf(" All 7 tests PASSED ✓\n");
    printf("═══════════════════════════════════════════════════════\n");

    return 0;
}
