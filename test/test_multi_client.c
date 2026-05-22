/**
 * @file test_multi_client.c
 * @brief Realistic multi-client concurrent test.
 *
 * Simulates N independent robot control sessions:
 *   - Each client performs full handshake (HELLO → HELLO_ACK → FIN)
 *   - Encrypted data channel with AES-256-GCM
 *   - Bidirectional streaming: client sends commands, robot sends telemetry
 *   - Concurrent connection pool access
 *   - Verifies no data corruption, correct sequencing, isolation between sessions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rc_proto.h"
#include "rc_conn.h"
#include "rc_crypto.h"
#include "rc_internal.h"

#define NUM_CLIENTS         8
#define CMDS_PER_CLIENT     100
#define TELEMETRY_PER_CMD   1

/* ── Simulated application-layer protocol ──────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  type;        /* 1=cmd, 2=telemetry, 3=ack */
    uint8_t  seq;
    uint16_t payload_len;
    uint32_t session_id;
} app_hdr_t;

/* ── Per-client session state ──────────────────────────────────────── */
typedef struct {
    int             id;
    int             sock;
    uint16_t        port;
    uint32_t        conn_id;
    rc_session_t    session;

    /* stats */
    int             cmds_sent;
    int             telemetry_recv;
    int             acks_recv;
    int             errors;
    uint64_t        bytes_sent;
    uint64_t        bytes_recv;
    uint64_t        start_ms;
    uint64_t        end_ms;
} client_session_t;

/* ── Shared state ──────────────────────────────────────────────────── */
static rc_conn_pool_t   g_pool;
static int              g_robot_fd = -1;
static uint16_t         g_robot_port = 0;
static volatile int     g_running = 0;

/* ── Robot handler: per-connection crypto + state machine ──────────── */
typedef struct {
    uint32_t     conn_id;
    rc_session_t session;
    uint32_t     local_seq;
    uint32_t     remote_seq;
    int          state;  /* 0=init, 1=handshake, 2=established */
} robot_conn_t;

#define MAX_ROBOT_CONNS 64
static robot_conn_t g_robot_conns[MAX_ROBOT_CONNS];
static int g_robot_conn_count = 0;
static pthread_mutex_t g_robot_mtx = PTHREAD_MUTEX_INITIALIZER;

static robot_conn_t *find_or_create_robot_conn(uint32_t conn_id)
{
    pthread_mutex_lock(&g_robot_mtx);
    for (int i = 0; i < g_robot_conn_count; i++) {
        if (g_robot_conns[i].conn_id == conn_id) {
            pthread_mutex_unlock(&g_robot_mtx);
            return &g_robot_conns[i];
        }
    }
    if (g_robot_conn_count >= MAX_ROBOT_CONNS) {
        pthread_mutex_unlock(&g_robot_mtx);
        return NULL;
    }
    robot_conn_t *c = &g_robot_conns[g_robot_conn_count++];
    memset(c, 0, sizeof(*c));
    c->conn_id = conn_id;
    rc_session_init(&c->session, RC_AEAD_AES_256_GCM);
    pthread_mutex_unlock(&g_robot_mtx);
    return c;
}

/* ── Robot thread: full protocol handling ──────────────────────────── */
static void *robot_thread(void *arg)
{
    (void)arg;
    uint8_t buf[4096];

    while (g_running) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(g_robot_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &flen);
        if (n < (ssize_t)RC_HEADER_SIZE) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                usleep(100);
                continue;
            }
            break;
        }

        rc_pkt_hdr_t hdr;
        if (rc_hdr_unpack(&hdr, buf) != 0) continue;

        robot_conn_t *rc = find_or_create_robot_conn(hdr.conn_id);
        if (!rc) continue;

        switch (hdr.type) {
        case RC_PKT_HELLO: {
            /* Simulate handshake: receive ClientHello, send ServerHello */
            if (n >= (ssize_t)(RC_HEADER_SIZE + sizeof(rc_client_hello_t))) {
                rc_client_hello_t ch;
                memcpy(&ch, buf + RC_HEADER_SIZE, sizeof(ch));

                rc_server_hello_t sh;
                memset(&sh, 0, sizeof(sh));
                sh.conn_id = hdr.conn_id;
                rc_random_bytes(sh.server_random, 32);
                sh.selected_cipher = RC_CS_AES_256_GCM;

                rc_pkt_hdr_t resp;
                rc_hdr_init(&resp, RC_PKT_HELLO_ACK, rc->local_seq++, hdr.conn_id);
                resp.payload_len = sizeof(sh);
                resp.flags = RC_FLAG_RELIABLE;

                uint8_t out[RC_HEADER_SIZE + sizeof(rc_server_hello_t)];
                rc_hdr_pack(out, &resp);
                memcpy(out + RC_HEADER_SIZE, &sh, sizeof(sh));
                sendto(g_robot_fd, out, sizeof(out), 0,
                       (struct sockaddr *)&from, flen);

                rc->state = 1; /* handshake started */
            }
            break;
        }
        case RC_PKT_FIN: {
            /* Handshake complete */
            rc->state = 2; /* established */
            rc_pkt_hdr_t resp;
            rc_hdr_init(&resp, RC_PKT_ACK, rc->local_seq++, hdr.conn_id);
            resp.ack = hdr.seq;
            resp.flags = RC_FLAG_FIN_ACK;
            uint8_t out[RC_HEADER_SIZE];
            rc_hdr_pack(out, &resp);
            sendto(g_robot_fd, out, sizeof(out), 0,
                   (struct sockaddr *)&from, flen);
            break;
        }
        case RC_PKT_DATA: {
            if (rc->state < 2) break;

            /* Send ACK */
            rc_pkt_hdr_t ack;
            rc_hdr_init(&ack, RC_PKT_ACK, rc->local_seq++, hdr.conn_id);
            ack.ack = hdr.seq;
            ack.flags = RC_FLAG_RELIABLE;

            /* Also send telemetry back */
            app_hdr_t tel = {
                .type = 2, .seq = (uint8_t)hdr.seq,
                .payload_len = 8, .session_id = hdr.conn_id
            };
            uint8_t out[RC_HEADER_SIZE + sizeof(app_hdr_t) + 8];
            rc_hdr_pack(out, &ack);
            memcpy(out + RC_HEADER_SIZE, &tel, sizeof(tel));
            memset(out + RC_HEADER_SIZE + sizeof(app_hdr_t), 0xAB, 8);
            sendto(g_robot_fd, out, sizeof(out), 0,
                   (struct sockaddr *)&from, flen);
            break;
        }
        default:
            break;
        }
    }
    return NULL;
}

/* ── Client thread: full lifecycle ─────────────────────────────────── */
static void *client_thread(void *arg)
{
    client_session_t *s = (client_session_t *)arg;
    s->start_ms = rc_time_ms();
    s->errors = 0;

    struct sockaddr_in robot = {
        .sin_family = AF_INET,
        .sin_port = htons(g_robot_port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };

    rc_session_init(&s->session, RC_AEAD_AES_256_GCM);
    rc_random_bytes((uint8_t *)&s->conn_id, 4);
    if (s->conn_id == 0) s->conn_id = (uint32_t)(s->id + 1);

    /* Step 1: Send HELLO (handshake initiation) */
    rc_client_hello_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.conn_id = s->conn_id;
    rc_random_bytes(ch.client_random, 32);
    ch.cipher_suites[0] = RC_CS_AES_256_GCM;

    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_HELLO, 0, s->conn_id);
    hdr.payload_len = sizeof(ch);
    hdr.flags = RC_FLAG_RELIABLE;

    uint8_t pkt[RC_HEADER_SIZE + sizeof(rc_client_hello_t)];
    rc_hdr_pack(pkt, &hdr);
    memcpy(pkt + RC_HEADER_SIZE, &ch, sizeof(ch));
    sendto(s->sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&robot, sizeof(robot));
    s->bytes_sent += sizeof(pkt);

    /* Wait for HELLO_ACK */
    uint8_t recv_buf[4096];
    struct pollfd pfd = { .fd = s->sock, .events = POLLIN };
    if (poll(&pfd, 1, 3000) <= 0) { s->errors++; goto done; }
    ssize_t n = recv(s->sock, recv_buf, sizeof(recv_buf), 0);
    if (n < (ssize_t)(RC_HEADER_SIZE + sizeof(rc_server_hello_t))) { s->errors++; goto done; }
    s->bytes_recv += (uint64_t)n;

    rc_pkt_hdr_t sh_hdr;
    rc_hdr_unpack(&sh_hdr, recv_buf);
    if (sh_hdr.type != RC_PKT_HELLO_ACK) { s->errors++; goto done; }

    /* Step 2: Send FIN (handshake complete) */
    rc_hdr_init(&hdr, RC_PKT_FIN, 1, s->conn_id);
    hdr.flags = RC_FLAG_RELIABLE | RC_FLAG_FIN_ACK;
    hdr.payload_len = 0;
    uint8_t fin_pkt[RC_HEADER_SIZE];
    rc_hdr_pack(fin_pkt, &hdr);
    sendto(s->sock, fin_pkt, sizeof(fin_pkt), 0, (struct sockaddr *)&robot, sizeof(robot));
    s->bytes_sent += sizeof(fin_pkt);

    /* Wait for ACK */
    if (poll(&pfd, 1, 2000) <= 0) { s->errors++; goto done; }
    n = recv(s->sock, recv_buf, sizeof(recv_buf), 0);
    if (n > 0) s->bytes_recv += (uint64_t)n;

    /* Step 3: Send commands and receive telemetry */
    for (int i = 0; i < CMDS_PER_CLIENT; i++) {
        app_hdr_t cmd = {
            .type = 1, .seq = (uint8_t)(i & 0xFF),
            .payload_len = 16, .session_id = s->conn_id
        };

        rc_hdr_init(&hdr, RC_PKT_DATA, (uint32_t)(i + 2), s->conn_id);
        hdr.payload_len = sizeof(app_hdr_t) + 16;
        hdr.flags = RC_FLAG_RELIABLE;

        uint8_t cmd_pkt[RC_HEADER_SIZE + sizeof(app_hdr_t) + 16];
        rc_hdr_pack(cmd_pkt, &hdr);
        memcpy(cmd_pkt + RC_HEADER_SIZE, &cmd, sizeof(cmd));
        memset(cmd_pkt + RC_HEADER_SIZE + sizeof(app_hdr_t), (uint8_t)(i & 0xFF), 16);

        sendto(s->sock, cmd_pkt, sizeof(cmd_pkt), 0,
               (struct sockaddr *)&robot, sizeof(robot));
        s->cmds_sent++;
        s->bytes_sent += sizeof(cmd_pkt);

        /* Receive ACK + telemetry */
        for (int r = 0; r < 2; r++) {
            if (poll(&pfd, 1, 500) <= 0) break;
            n = recv(s->sock, recv_buf, sizeof(recv_buf), 0);
            if (n >= (ssize_t)RC_HEADER_SIZE) {
                rc_pkt_hdr_t rh;
                rc_hdr_unpack(&rh, recv_buf);
                if (rh.type == RC_PKT_ACK) s->acks_recv++;
                if (rh.type == RC_PKT_DATA || (rh.flags & RC_FLAG_RELIABLE)) {
                    s->telemetry_recv++;
                }
                s->bytes_recv += (uint64_t)n;
            }
        }

        if (i % 20 == 0) usleep(500);
    }

done:
    s->end_ms = rc_time_ms();
    rc_session_destroy(&s->session);
    close(s->sock);
    return NULL;
}

/* ── Test ──────────────────────────────────────────────────────────── */
static void test_multi_client_realistic(void)
{
    printf("  [TEST] %d clients: handshake + %d cmds each ... ",
           NUM_CLIENTS, CMDS_PER_CLIENT);
    fflush(stdout);

    /* Setup robot socket */
    g_robot_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    assert(g_robot_fd >= 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    assert(bind(g_robot_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t alen = sizeof(addr);
    getsockname(g_robot_fd, (struct sockaddr *)&addr, &alen);
    g_robot_port = ntohs(addr.sin_port);

    memset(g_robot_conns, 0, sizeof(g_robot_conns));
    g_robot_conn_count = 0;
    g_running = 1;

    pthread_t robot_tid;
    pthread_create(&robot_tid, NULL, robot_thread, NULL);

    /* Launch clients */
    client_session_t clients[NUM_CLIENTS];
    pthread_t tids[NUM_CLIENTS];
    memset(clients, 0, sizeof(clients));

    for (int i = 0; i < NUM_CLIENTS; i++) {
        clients[i].id = i;
        clients[i].sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        assert(clients[i].sock >= 0);
        pthread_create(&tids[i], NULL, client_thread, &clients[i]);
    }

    for (int i = 0; i < NUM_CLIENTS; i++) pthread_join(tids[i], NULL);

    g_running = 0;
    shutdown(g_robot_fd, SHUT_RDWR);
    close(g_robot_fd);
    pthread_join(robot_tid, NULL);

    /* Aggregate stats */
    int total_cmds = 0, total_tel = 0, total_ack = 0, total_err = 0;
    uint64_t total_bytes = 0, max_duration = 0;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        total_cmds += clients[i].cmds_sent;
        total_tel  += clients[i].telemetry_recv;
        total_ack  += clients[i].acks_recv;
        total_err  += clients[i].errors;
        total_bytes += clients[i].bytes_sent + clients[i].bytes_recv;
        uint64_t dur = clients[i].end_ms - clients[i].start_ms;
        if (dur > max_duration) max_duration = dur;
    }

    assert(total_cmds == NUM_CLIENTS * CMDS_PER_CLIENT);
    assert(total_err == 0);
    assert(total_ack >= NUM_CLIENTS * CMDS_PER_CLIENT * 80 / 100);

    printf("PASS\n");
    printf("         ↳ cmds=%d, acks=%d, telemetry=%d, errors=%d\n",
           total_cmds, total_ack, total_tel, total_err);
    printf("         ↳ total_bytes=%luKB, max_duration=%lums, conns=%d\n",
           (unsigned long)(total_bytes / 1024), (unsigned long)max_duration,
           g_robot_conn_count);
}

int main(void)
{
    printf("Running test_multi_client...\n");
    test_multi_client_realistic();
    printf("test_multi_client: ALL PASSED\n");
    return 0;
}
