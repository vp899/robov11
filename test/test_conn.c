/**
 * @file test_conn.c
 * @brief Unit tests for connection manager (rc_conn)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rc_conn.h"

static void test_conn_create_destroy(void)
{
    rc_conn_pool_t pool;
    rc_conn_pool_init(&pool, 1024);

    rc_conn_t *conn = rc_conn_create(&pool, -1, 1);
    assert(conn != NULL);
    assert(conn->conn_id == 1);
    assert(conn->state == RC_STATE_CLOSED);

    rc_conn_release(conn);
    rc_conn_pool_destroy(&pool);
    printf("  PASS: test_conn_create_destroy\n");
}

static void test_conn_state_machine(void)
{
    rc_conn_pool_t pool;
    rc_conn_pool_init(&pool, 1024);

    rc_conn_t *conn = rc_conn_create(&pool, -1, 1);
    assert(conn != NULL);

    rc_conn_set_state(conn, RC_STATE_CONNECTING);
    assert(conn->state == RC_STATE_CONNECTING);

    rc_conn_set_state(conn, RC_STATE_HANDSHAKING);
    assert(conn->state == RC_STATE_HANDSHAKING);

    rc_conn_set_state(conn, RC_STATE_ESTABLISHED);
    assert(conn->state == RC_STATE_ESTABLISHED);

    rc_conn_set_state(conn, RC_STATE_CLOSING);
    assert(conn->state == RC_STATE_CLOSING);

    rc_conn_release(conn);
    rc_conn_pool_destroy(&pool);
    printf("  PASS: test_conn_state_machine\n");
}

static void test_conn_lookup(void)
{
    rc_conn_pool_t pool;
    rc_conn_pool_init(&pool, 1024);

    rc_conn_t *c1 = rc_conn_create(&pool, -1, 100);
    rc_conn_t *c2 = rc_conn_create(&pool, -1, 200);
    rc_conn_t *c3 = rc_conn_create(&pool, -1, 300);

    assert(rc_conn_lookup(&pool, 100) == c1);
    assert(rc_conn_lookup(&pool, 200) == c2);
    assert(rc_conn_lookup(&pool, 300) == c3);
    assert(rc_conn_lookup(&pool, 999) == NULL);

    rc_conn_release(c1);
    /* After release, lookup may still find it in CLOSED state — that's OK */

    rc_conn_release(c2);
    rc_conn_release(c3);
    rc_conn_pool_destroy(&pool);
    printf("  PASS: test_conn_lookup\n");
}

static void test_conn_max_capacity(void)
{
    rc_conn_pool_t pool;
    rc_conn_pool_init(&pool, 4);

    rc_conn_t *conns[4];
    for (int i = 0; i < 4; i++) {
        conns[i] = rc_conn_create(&pool, -1, (uint32_t)(i + 1));
        assert(conns[i] != NULL);
    }

    /* Pool full — next should fail */
    assert(rc_conn_create(&pool, -1, 5) == NULL);

    for (int i = 0; i < 4; i++) rc_conn_release(conns[i]);
    rc_conn_pool_destroy(&pool);
    printf("  PASS: test_conn_max_capacity\n");
}

static void test_ring_buffer(void)
{
    rc_ringbuf_t rb;
    rc_ringbuf_init(&rb, 1024);

    /* Write and read */
    const char *msg = "Hello, robot!";
    rc_ringbuf_write(&rb, (const uint8_t *)msg, strlen(msg));

    uint8_t buf[64];
    size_t rlen = rc_ringbuf_read(&rb, buf, sizeof(buf));
    (void)rlen;
    assert(rlen == strlen(msg));
    assert(memcmp(buf, msg, strlen(msg)) == 0);

    /* Empty read should return 0 */
    rlen = rc_ringbuf_read(&rb, buf, sizeof(buf));
    assert(rlen == 0);

    rc_ringbuf_destroy(&rb);
    printf("  PASS: test_ring_buffer\n");
}

static void test_ring_buffer_wrap(void)
{
    rc_ringbuf_t rb;
    rc_ringbuf_init(&rb, 16); /* small buffer */

    uint8_t data[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    /* Fill and drain multiple times to test wrap-around */
    for (int i = 0; i < 100; i++) {
        rc_ringbuf_write(&rb, data, sizeof(data));
        uint8_t out[8];
        rc_ringbuf_read(&rb, out, sizeof(out));
        assert(memcmp(out, data, sizeof(data)) == 0);
    }

    rc_ringbuf_destroy(&rb);
    printf("  PASS: test_ring_buffer_wrap\n");
}

int main(void)
{
    printf("Running test_conn...\n");
    test_conn_create_destroy();
    test_conn_state_machine();
    test_conn_lookup();
    test_conn_max_capacity();
    test_ring_buffer();
    test_ring_buffer_wrap();
    printf("test_conn: ALL PASSED\n");
    return 0;
}
