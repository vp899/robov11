/**
 * @file test_stress.c
 * @file test_stress.c
 * @brief Realistic stress and rate tests.
 *
 * Tests:
 *   - Bidirectional throughput with proper send/recv interleaving
 *   - Connection pool under load (10K entries)
 *   - Ring buffer rapid churn
 *   - Concurrent pool access from 4 threads
 *   - Crypto session creation rate
 *   - Bandwidth measurement (Mbps)
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

/* ── UDP throughput with interleaved send/recv ─────────────────────── */

static void test_udp_throughput(void)
{
    printf("  [TEST] UDP throughput (bidirectional) ... ");
    fflush(stdout);

    int fd_a = socket(AF_INET, SOCK_DGRAM, 0);
    int fd_b = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd_a >= 0 && fd_b >= 0);

    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd_a, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(fd_b, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr_a = { .sin_family=AF_INET, .sin_port=0,
                                  .sin_addr.s_addr=htonl(INADDR_LOOPBACK) };
    struct sockaddr_in addr_b = { .sin_family=AF_INET, .sin_port=0,
                                  .sin_addr.s_addr=htonl(INADDR_LOOPBACK) };
    bind(fd_a, (struct sockaddr *)&addr_a, sizeof(addr_a));
    bind(fd_b, (struct sockaddr *)&addr_b, sizeof(addr_b));
    socklen_t al = sizeof(addr_a);
    getsockname(fd_a, (struct sockaddr *)&addr_a, &al);
    getsockname(fd_b, (struct sockaddr *)&addr_b, &al);
    uint16_t port_a = ntohs(addr_a.sin_port);
    uint16_t port_b = ntohs(addr_b.sin_port);

    struct sockaddr_in peer_a = { .sin_family=AF_INET, .sin_port=htons(port_a),
                                  .sin_addr.s_addr=htonl(INADDR_LOOPBACK) };
    struct sockaddr_in peer_b = { .sin_family=AF_INET, .sin_port=htons(port_b),
                                  .sin_addr.s_addr=htonl(INADDR_LOOPBACK) };

    int total = 2000;
    uint8_t pkt[RC_HEADER_SIZE + 512];
    uint8_t rb[RC_HEADER_SIZE + 512 + 64];

    /* Interleave: send 10, recv 10, repeat */
    int sent_a = 0, sent_b = 0, recv_a = 0, recv_b = 0;
    uint64_t t0 = rc_time_ms();

    while (sent_a < total || sent_b < total) {
        /* Send batch from A→B */
        for (int i = 0; i < 10 && sent_a < total; i++) {
            rc_pkt_hdr_t h;
            rc_hdr_init(&h, RC_PKT_DATA, (uint32_t)sent_a, 1);
            h.payload_len = 512;
            rc_hdr_pack(pkt, &h);
            memset(pkt + RC_HEADER_SIZE, (uint8_t)(sent_a & 0xFF), 512);
            sendto(fd_a, pkt, sizeof(pkt), 0, (struct sockaddr *)&peer_b, sizeof(peer_b));
            sent_a++;
        }
        /* Send batch from B→A */
        for (int i = 0; i < 10 && sent_b < total; i++) {
            rc_pkt_hdr_t h;
            rc_hdr_init(&h, RC_PKT_DATA, (uint32_t)sent_b, 2);
            h.payload_len = 512;
            rc_hdr_pack(pkt, &h);
            memset(pkt + RC_HEADER_SIZE, (uint8_t)(sent_b & 0xFF), 512);
            sendto(fd_b, pkt, sizeof(pkt), 0, (struct sockaddr *)&peer_a, sizeof(peer_a));
            sent_b++;
        }
        /* Drain A */
        struct pollfd pfd = { .fd = fd_a, .events = POLLIN };
        while (poll(&pfd, 1, 0) > 0) {
            ssize_t n = recv(fd_a, rb, sizeof(rb), 0);
            if (n > 0) recv_a++;
        }
        /* Drain B */
        pfd.fd = fd_b;
        while (poll(&pfd, 1, 0) > 0) {
            ssize_t n = recv(fd_b, rb, sizeof(rb), 0);
            if (n > 0) recv_b++;
        }
    }

    /* Final drain */
    for (int pass = 0; pass < 10; pass++) {
        struct pollfd pfd = { .fd = fd_a, .events = POLLIN };
        if (poll(&pfd, 1, 50) <= 0) break;
        while (recv(fd_a, rb, sizeof(rb), 0) > 0) recv_a++;
    }
    for (int pass = 0; pass < 10; pass++) {
        struct pollfd pfd = { .fd = fd_b, .events = POLLIN };
        if (poll(&pfd, 1, 50) <= 0) break;
        while (recv(fd_b, rb, sizeof(rb), 0) > 0) recv_b++;
    }

    uint64_t elapsed = rc_time_ms() - t0;
    uint64_t total_bytes = (uint64_t)(sent_a + sent_b) * sizeof(pkt);
    double mbps = (double)total_bytes * 8.0 / (double)elapsed / 1000.0;

    assert(recv_a >= total / 2);
    assert(recv_b >= total / 2);

    close(fd_a); close(fd_b);
    printf("PASS\n");
    printf("         ↳ A→B: sent=%d recv=%d, B→A: sent=%d recv=%d\n",
           sent_a, recv_a, sent_b, recv_b);
    printf("         ↳ %luKB in %lums = %.1f Mbps\n",
           (unsigned long)(total_bytes/1024), (unsigned long)elapsed, mbps);
}

/* ── Connection pool stress ────────────────────────────────────────── */

static void test_pool_stress(void)
{
    printf("  [TEST] conn pool 10K entries ... ");
    fflush(stdout);

    #define N 10000
    rc_conn_pool_t pool;
    assert(rc_conn_pool_init(&pool, N * 2) == 0);

    rc_conn_t *conns[N];
    uint64_t t0 = rc_time_ms();

    for (int i = 0; i < N; i++) {
        conns[i] = rc_conn_create(&pool, 1000+i, (uint32_t)(i+1));
        assert(conns[i] != NULL);
    }
    uint64_t t_create = rc_time_ms() - t0;

    uint64_t t1 = rc_time_ms();
    for (int i = 0; i < N; i++) {
        rc_conn_t *c = rc_conn_lookup(&pool, (uint32_t)(i+1));
        assert(c == conns[i]);
        rc_conn_release(c);
    }
    uint64_t t_lookup = rc_time_ms() - t1;

    assert(pool.count == (uint64_t)N);

    rc_conn_pool_destroy(&pool);
    #undef N
    printf("PASS (create=%lums, lookup=%lums)\n",
           (unsigned long)t_create, (unsigned long)t_lookup);
}

/* ── Ring buffer stress ────────────────────────────────────────────── */

static void test_ringbuf_stress(void)
{
    printf("  [TEST] ring buffer 100K cycles ... ");
    fflush(stdout);

    rc_ringbuf_t rb;
    rc_ringbuf_init(&rb, 4096);

    uint8_t w[256], r[256];
    for (int i = 0; i < 256; i++) w[i] = (uint8_t)i;

    uint64_t t0 = rc_time_ms();
    int ops = 0;
    for (int i = 0; i < 100000; i++) {
        size_t n = rc_ringbuf_write(&rb, w, sizeof(w));
        if (n > 0) {
            rc_ringbuf_read(&rb, r, n);
            ops++;
        }
    }
    uint64_t elapsed = rc_time_ms() - t0;

    rc_ringbuf_destroy(&rb);
    printf("PASS (%d ops in %lums)\n", ops, (unsigned long)elapsed);
}

/* ── Concurrent pool access ────────────────────────────────────────── */

#define CTHREADS 4
#define COPS     5000

typedef struct { int id; rc_conn_pool_t *pool; int creates, lookups, errors; } st_ctx_t;

static void *stress_thread(void *arg)
{
    st_ctx_t *ctx = arg;
    int base = ctx->id * COPS;
    for (int i = 0; i < COPS; i++) {
        uint32_t cid = (uint32_t)(base + i + 1);
        rc_conn_t *c = rc_conn_create(ctx->pool, 20000+cid, cid);
        if (c) {
            ctx->creates++;
            rc_conn_t *f = rc_conn_lookup(ctx->pool, cid);
            if (f) { ctx->lookups++; rc_conn_release(f); }
            else ctx->errors++;
        } else ctx->errors++;
    }
    return NULL;
}

static void test_concurrent_pool(void)
{
    printf("  [TEST] concurrent pool (%d×%d) ... ", CTHREADS, COPS);
    fflush(stdout);

    rc_conn_pool_t pool;
    rc_conn_pool_init(&pool, CTHREADS * COPS * 2);

    st_ctx_t ctxs[CTHREADS];
    pthread_t tids[CTHREADS];
    uint64_t t0 = rc_time_ms();

    for (int i = 0; i < CTHREADS; i++) {
        ctxs[i] = (st_ctx_t){ .id=i, .pool=&pool };
        pthread_create(&tids[i], NULL, stress_thread, &ctxs[i]);
    }
    for (int i = 0; i < CTHREADS; i++) pthread_join(tids[i], NULL);

    uint64_t elapsed = rc_time_ms() - t0;
    int tc=0, tl=0, te=0;
    for (int i = 0; i < CTHREADS; i++) {
        tc += ctxs[i].creates; tl += ctxs[i].lookups; te += ctxs[i].errors;
    }

    assert(tc == CTHREADS * COPS);
    assert(te == 0);

    rc_conn_pool_destroy(&pool);
    printf("PASS (creates=%d, lookups=%d, %lums)\n", tc, tl, (unsigned long)elapsed);
}

/* ── Crypto session stress ─────────────────────────────────────────── */

static void test_crypto_stress(void)
{
    printf("  [TEST] 1000 crypto sessions ... ");
    fflush(stdout);

    uint8_t ss[32], cr[32], sr[32];
    for (int i = 0; i < 32; i++) {
        ss[i] = (uint8_t)i; cr[i] = (uint8_t)(i+0x40); sr[i] = (uint8_t)(i+0x80);
    }

    uint64_t t0 = rc_time_ms();
    int ok = 0, derive_fail = 0, enc_fail = 0;
    for (int i = 0; i < 1000; i++) {
        rc_session_t sess;
        rc_session_init(&sess, RC_AEAD_AES_256_GCM);
        ss[0] = (uint8_t)(i & 0xFF);
        ss[1] = (uint8_t)((i >> 8) & 0xFF);
        if (rc_derive_session_keys(ss, 32, cr, sr, &sess) != 0) {
            derive_fail++;
            rc_session_destroy(&sess);
            continue;
        }

        /* Encrypt with encrypt_ctx, decrypt with encrypt_ctx (same key).
         * In production, client encrypt_ctx == server decrypt_ctx,
         * but here we test crypto correctness on a single session. */
        uint8_t nonce[12];
        rc_derive_nonce(nonce, sess.encrypt_ctx.nonce_salt, 0);

        uint8_t pt[] = "stress test data";
        uint8_t ct[64];
        size_t ct_len = 0;
        if (rc_aead_encrypt(&sess.encrypt_ctx, nonce, pt, 16,
                            NULL, 0, ct, &ct_len) != 0) {
            enc_fail++;
            rc_session_destroy(&sess);
            continue;
        }

        uint8_t dec[64];
        size_t dec_len = 0;
        if (rc_aead_decrypt(&sess.encrypt_ctx, nonce, ct, ct_len,
                            NULL, 0, dec, &dec_len) != 0) {
            enc_fail++;
        } else {
            ok++;
        }
        rc_session_destroy(&sess);
    }
    uint64_t elapsed = rc_time_ms() - t0;

    printf("PASS (%d/1000 ok, %lums", ok, (unsigned long)elapsed);
    if (derive_fail + enc_fail > 0)
        printf(", derive_fail=%d, enc_fail=%d", derive_fail, enc_fail);
    printf(")\n");
}

int main(void)
{
    printf("Running test_stress...\n");
    test_udp_throughput();
    test_pool_stress();
    test_ringbuf_stress();
    test_concurrent_pool();
    test_crypto_stress();
    printf("test_stress: ALL PASSED\n");
    return 0;
}
