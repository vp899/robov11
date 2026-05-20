/**
 * @file test_congestion.c
 * @brief Unit tests for CUBIC congestion control (rc_congestion)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rc_congestion.h"

#define TEST_MSS  1460
#define TEST_RTT  10

static void test_init(void)
{
    rc_cubic_t cubic;
    rc_cubic_init(&cubic, TEST_MSS);
    assert(cubic.cwnd == RC_CUBIC_INIT_CWND);
    assert(cubic.ssthresh == 0xFFFFFFFF);
    assert(cubic.w_max == 0);
    assert(cubic.state == RC_CC_SLOW_START);
    printf("  PASS: test_init\n");
}

static void test_slow_start(void)
{
    rc_cubic_t cubic;
    rc_cubic_init(&cubic, TEST_MSS);

    /* In slow start, cwnd should grow on each ACK */
    uint32_t init_cwnd = cubic.cwnd;
    (void)init_cwnd;
    for (int i = 0; i < 10; i++) {
        rc_cubic_on_ack(&cubic, 1000, TEST_RTT);
    }
    assert(cubic.cwnd > init_cwnd);
    printf("  PASS: test_slow_start\n");
}

static void test_congestion_avoidance(void)
{
    rc_cubic_t cubic;
    rc_cubic_init(&cubic, TEST_MSS);

    /* Force into congestion avoidance */
    cubic.ssthresh = 10;
    cubic.cwnd = 10;
    cubic.state = RC_CC_CONGESTION_AVOIDANCE;

    uint32_t prev_cwnd = cubic.cwnd;
    (void)prev_cwnd;
    for (int i = 0; i < 100; i++) {
        rc_cubic_on_ack(&cubic, 1000, TEST_RTT);
    }
    assert(cubic.cwnd >= prev_cwnd);
    printf("  PASS: test_congestion_avoidance\n");
}

static void test_loss_recovery(void)
{
    rc_cubic_t cubic;
    rc_cubic_init(&cubic, TEST_MSS);
    cubic.cwnd = 100000;
    cubic.ssthresh = 100000;

    rc_cubic_on_loss(&cubic);
    assert(cubic.cwnd < 100000); /* Should reduce */
    assert(cubic.w_max == 100000);
    printf("  PASS: test_loss_recovery\n");
}

static void test_multiple_losses(void)
{
    rc_cubic_t cubic;
    rc_cubic_init(&cubic, TEST_MSS);
    cubic.cwnd = 200000;

    rc_cubic_on_loss(&cubic);
    uint32_t after_first = cubic.cwnd;
    (void)after_first;

    /* Recover a bit */
    for (int i = 0; i < 50; i++) rc_cubic_on_ack(&cubic, 1000, TEST_RTT);

    rc_cubic_on_loss(&cubic);
    assert(cubic.cwnd <= after_first);
    printf("  PASS: test_multiple_losses\n");
}

static void test_get_cwnd(void)
{
    rc_cubic_t cubic;
    rc_cubic_init(&cubic, TEST_MSS);
    assert(rc_cubic_get_cwnd(&cubic) == RC_CUBIC_INIT_CWND);
    printf("  PASS: test_get_cwnd\n");
}

int main(void)
{
    printf("Running test_congestion...\n");
    test_init();
    test_slow_start();
    test_congestion_avoidance();
    test_loss_recovery();
    test_multiple_losses();
    test_get_cwnd();
    printf("test_congestion: ALL PASSED\n");
    return 0;
}
