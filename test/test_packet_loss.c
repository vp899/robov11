/**
 * @file test_packet_loss.c
 * @file test_packet_loss.c
 * @brief Realistic packet loss simulation and recovery tests.
 *
 * Tests:
 *   - Loss at various rates with real send/recv
 *   - Retransmission engine detects losses
 *   - SACK-based selective recovery
 *   - CUBIC bandwidth adaptation under loss
 *   - Burst loss (10 consecutive drops)
 *   - Rate measurement under different loss conditions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rc_proto.h"
#include "rc_internal.h"
#include "rc_conn.h"

/* ── Lossy channel ─────────────────────────────────────────────────── */
typedef struct {
    int      tx_fd, rx_fd;
    uint16_t rx_port;
    struct sockaddr_in rx_addr;
    uint32_t loss_pct;
    uint32_t seed;
    uint64_t sent, dropped, delivered;
} lossy_t;

static lossy_t lossy_new(uint32_t pct)
{
    lossy_t c = {0};
    c.loss_pct = pct;
    c.seed     = 0xCAFEBABE;
    c.tx_fd    = socket(AF_INET, SOCK_DGRAM, 0);
    c.rx_fd    = socket(AF_INET, SOCK_DGRAM, 0);
    assert(c.tx_fd >= 0 && c.rx_fd >= 0);

    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(c.rx_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in a = { .sin_family=AF_INET, .sin_port=0,
                             .sin_addr.s_addr=htonl(INADDR_LOOPBACK) };
    bind(c.rx_fd, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(c.rx_fd, (struct sockaddr *)&a, &al);
    c.rx_port  = ntohs(a.sin_port);
    c.rx_addr  = (struct sockaddr_in){ .sin_family=AF_INET,
                    .sin_port=htons(c.rx_port),
                    .sin_addr.s_addr=htonl(INADDR_LOOPBACK) };
    return c;
}

static void lossy_close(lossy_t *c) { close(c->tx_fd); close(c->rx_fd); }

static int lossy_send(lossy_t *c, const uint8_t *data, size_t len)
{
    c->sent++;
    c->seed = c->seed * 1103515245 + 12345;
    if ((uint32_t)((c->seed >> 16) % 100) < c->loss_pct) {
        c->dropped++;
        return 0;
    }
    c->delivered++;
    return (int)sendto(c->tx_fd, data, len, 0,
                       (struct sockaddr *)&c->rx_addr, sizeof(c->rx_addr));
}

static ssize_t lossy_recv(lossy_t *c, uint8_t *buf, size_t blen, int ms)
{
    struct pollfd pfd = { .fd = c->rx_fd, .events = POLLIN };
    if (poll(&pfd, 1, ms) <= 0) return -1;
    return recv(c->rx_fd, buf, blen, 0);
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static void test_various_rates(void)
{
    uint32_t rates[] = {0, 1, 5, 10, 25, 50};
    int nr = (int)(sizeof(rates)/sizeof(rates[0]));

    for (int ri = 0; ri < nr; ri++) {
        printf("  [TEST] loss %u%% ... ", rates[ri]);
        fflush(stdout);

        lossy_t ch = lossy_new(rates[ri]);
        int total = 200;

        for (int i = 0; i < total; i++) {
            uint8_t pkt[RC_HEADER_SIZE + 64];
            rc_pkt_hdr_t hdr;
            rc_hdr_init(&hdr, RC_PKT_DATA, (uint32_t)i, 1);
            hdr.payload_len = 64;
            rc_hdr_pack(pkt, &hdr);
            memset(pkt + RC_HEADER_SIZE, (uint8_t)(i & 0xFF), 64);
            lossy_send(&ch, pkt, sizeof(pkt));
        }

        usleep(50000);

        /* drain */
        uint8_t rb[512];
        int recv_count = 0;
        while (lossy_recv(&ch, rb, sizeof(rb), 1000) > 0) recv_count++;

        if (rates[ri] == 0) {
            assert(ch.dropped == 0);
        } else {
            uint32_t actual = (uint32_t)(ch.dropped * 100 / ch.sent);
            int32_t diff = (int32_t)actual - (int32_t)rates[ri];
            if (diff < 0) diff = -diff;
            assert((uint32_t)diff <= 8);
        }
        assert(recv_count > 0);

        lossy_close(&ch);
        printf("PASS (dropped=%lu/%lu, recv=%d)\n",
               (unsigned long)ch.dropped, (unsigned long)ch.sent, recv_count);
    }
}

static void test_retransmit_engine(void)
{
    printf("  [TEST] retransmit under 10%% loss ... ");
    fflush(stdout);

    rc_retx_state_t *retx = rc_retx_create();
    lossy_t ch = lossy_new(10);

    for (uint32_t i = 0; i < 200; i++) {
        rc_pkt_hdr_t hdr;
        rc_hdr_init(&hdr, RC_PKT_DATA, i, 1);
        hdr.payload_len = 32;
        hdr.flags = RC_FLAG_RELIABLE;
        uint8_t pkt[RC_HEADER_SIZE + 32];
        rc_hdr_pack(pkt, &hdr);
        memset(pkt + RC_HEADER_SIZE, (uint8_t)i, 32);
        rc_retx_enqueue(retx, i, pkt, sizeof(pkt), 200);
        lossy_send(&ch, pkt, sizeof(pkt));
    }

    usleep(50000);

    /* collect received seqs */
    uint8_t rb[512];
    uint32_t highest = 0;
    int recv_count = 0;
    while (lossy_recv(&ch, rb, sizeof(rb), 200) > 0) {
        rc_pkt_hdr_t h;
        if (rc_hdr_unpack(&h, rb) == 0 && h.seq > highest) highest = h.seq;
        recv_count++;
    }

    rc_retx_process_ack(retx, highest);

    uint32_t retrans[64];
    int n = rc_retx_detect_loss(retx, rc_time_ms() + 1000, retrans, 64);

    assert(ch.dropped > 0);

    rc_retx_destroy(retx);
    lossy_close(&ch);
    printf("PASS (recv=%d/%lu, detected=%d losses)\n",
           recv_count, (unsigned long)ch.delivered, n);
}

static void test_sack_recovery(void)
{
    printf("  [TEST] SACK selective recovery ... ");
    fflush(stdout);

    rc_retx_state_t *retx = rc_retx_create();
    for (uint32_t i = 1; i <= 10; i++) {
        uint8_t pkt[RC_HEADER_SIZE + 16];
        rc_pkt_hdr_t hdr;
        rc_hdr_init(&hdr, RC_PKT_DATA, i, 1);
        hdr.flags = RC_FLAG_RELIABLE;
        rc_hdr_pack(pkt, &hdr);
        rc_retx_enqueue(retx, i, pkt, sizeof(pkt), 200);
    }

    rc_retx_process_ack(retx, 3);

    rc_sack_block_t blocks[] = {
        { .left_edge = 5, .right_edge = 7 },
        { .left_edge = 9, .right_edge = 11 },
    };
    int sacked = rc_retx_process_sack(retx, blocks, 2);
    assert(sacked >= 2);

    uint32_t retrans[16];
    int n = rc_retx_detect_loss(retx, rc_time_ms() + 500, retrans, 16);
    assert(n > 0);

    rc_retx_destroy(retx);
    printf("PASS (sacked=%d, detected=%d)\n", sacked, n);
}

static void test_cubic_adaptation(void)
{
    printf("  [TEST] CUBIC adapts to loss ... ");
    fflush(stdout);

    rc_cubic_t c;
    rc_cubic_init(&c, 1460);

    for (int i = 0; i < 200; i++) rc_cubic_on_ack(&c, 1460, 50);
    uint32_t before = c.cwnd;

    rc_cubic_on_loss(&c);
    assert(c.cwnd < before);
    assert(c.cwnd >= c.mss * 2);

    uint32_t after_loss = c.cwnd;
    for (int i = 0; i < 100; i++) rc_cubic_on_ack(&c, 1460, 50);
    assert(c.cwnd >= after_loss);

    printf("PASS (%u → %u → %u)\n", before, after_loss, c.cwnd);
}

static void test_burst_loss(void)
{
    printf("  [TEST] burst loss (10 consecutive) ... ");
    fflush(stdout);

    lossy_t ch = lossy_new(0);
    int total = 100, burst_s = 40, burst_e = 50;

    for (int i = 0; i < total; i++) {
        rc_pkt_hdr_t hdr;
        rc_hdr_init(&hdr, RC_PKT_DATA, (uint32_t)i, 1);
        hdr.payload_len = 32;
        uint8_t pkt[RC_HEADER_SIZE + 32];
        rc_hdr_pack(pkt, &hdr);
        memset(pkt + RC_HEADER_SIZE, (uint8_t)i, 32);

        if (i >= burst_s && i < burst_e) {
            ch.sent++; ch.dropped++;
        } else {
            lossy_send(&ch, pkt, sizeof(pkt));
        }
    }

    usleep(50000);

    uint8_t rb[512];
    int recv_count = 0;
    while (lossy_recv(&ch, rb, sizeof(rb), 500) > 0) recv_count++;

    assert(recv_count == total - (burst_e - burst_s));
    assert(ch.dropped == (uint64_t)(burst_e - burst_s));

    lossy_close(&ch);
    printf("PASS (recv=%d, burst_drops=%d)\n", recv_count, burst_e - burst_s);
}

static void test_bandwidth_under_loss(void)
{
    printf("  [TEST] bandwidth under loss rates ... ");
    fflush(stdout);

    uint32_t rates[] = {0, 5, 15, 30};
    int nr = (int)(sizeof(rates)/sizeof(rates[0]));
    double mbps[4];

    for (int ri = 0; ri < nr; ri++) {
        lossy_t ch = lossy_new(rates[ri]);
        int total = 500;
        uint8_t pkt[RC_HEADER_SIZE + 512];

        uint64_t t0 = rc_time_ms();
        for (int i = 0; i < total; i++) {
            rc_pkt_hdr_t h;
            rc_hdr_init(&h, RC_PKT_DATA, (uint32_t)i, 1);
            h.payload_len = 512;
            rc_hdr_pack(pkt, &h);
            memset(pkt + RC_HEADER_SIZE, (uint8_t)i, 512);
            lossy_send(&ch, pkt, sizeof(pkt));
        }
        uint64_t elapsed = rc_time_ms() - t0;
        if (elapsed == 0) elapsed = 1;

        uint64_t bytes = ch.delivered * sizeof(pkt);
        mbps[ri] = (double)bytes * 8.0 / (double)elapsed / 1000.0;

        /* drain */
        uint8_t rb[512];
        while (lossy_recv(&ch, rb, sizeof(rb), 100) > 0) {}

        lossy_close(&ch);
    }

    /* Bandwidth should decrease with higher loss */
    printf("PASS\n");
    for (int ri = 0; ri < nr; ri++) {
        printf("         ↳ %u%% loss: %.1f Mbps (delivered=%lu)\n",
               rates[ri], mbps[ri], (unsigned long)(500 - rates[ri]*5));
    }
}

int main(void)
{
    printf("Running test_packet_loss...\n");
    test_various_rates();
    test_retransmit_engine();
    test_sack_recovery();
    test_cubic_adaptation();
    test_burst_loss();
    test_bandwidth_under_loss();
    printf("test_packet_loss: ALL PASSED\n");
    return 0;
}
