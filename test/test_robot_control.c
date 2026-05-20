/**
 * @file test_robot_control.c
 * @brief Robot remote control simulation.
 *
 * Operator sends control commands (joystick / button / emergency stop),
 * robot responds with telemetry (position, battery, sensor).
 * Verifies command-response pairing, data integrity, rapid-fire handling.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rc_proto.h"
#include "rc_epoll.h"

/* ── Robot control protocol (layered on RoboControl DATA) ──────────── */
#define CMD_JOYSTICK    0x01
#define CMD_BUTTON      0x02
#define CMD_MODE        0x03
#define CMD_EMERGENCY   0xFF

#define TEL_ACK         0x13

typedef struct __attribute__((packed)) {
    uint8_t  cmd_type;
    uint8_t  seq;
    int16_t  axis_x;      /* -1000..1000 */
    int16_t  axis_y;
    int16_t  axis_z;
    int16_t  rotation;
    uint32_t ts_ms;
} control_cmd_t;

typedef struct __attribute__((packed)) {
    uint8_t  tel_type;
    uint8_t  cmd_seq_acked;
    float    pos_x;
    float    pos_y;
    float    pos_z;
    float    yaw;
    uint16_t battery_mv;
    uint8_t  battery_pct;
    uint16_t distance_mm;
    uint32_t ts_ms;
} telemetry_t;

static int      g_robot_fd = -1;
static uint16_t g_port     = 0;
static volatile int g_running = 0;
static int g_cmds_sent = 0, g_cmds_acked = 0;

/* ── robot thread: recv commands → send telemetry ──────────────────── */
static void *robot_thread(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    while (g_running) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(g_robot_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &flen);
        if (n < (ssize_t)(RC_HEADER_SIZE + sizeof(control_cmd_t))) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { usleep(100); continue; }
            break;
        }
        rc_pkt_hdr_t hdr;
        if (rc_hdr_unpack(&hdr, buf) != 0) continue;
        if (hdr.type != RC_PKT_DATA) continue;

        control_cmd_t cmd;
        memcpy(&cmd, buf + RC_HEADER_SIZE, sizeof(cmd));

        telemetry_t tel;
        memset(&tel, 0, sizeof(tel));
        tel.tel_type       = TEL_ACK;
        tel.cmd_seq_acked  = cmd.seq;
        tel.pos_x          = (float)cmd.axis_x * 0.001f;
        tel.pos_y          = (float)cmd.axis_y * 0.001f;
        tel.pos_z          = (float)cmd.axis_z * 0.001f;
        tel.yaw            = (float)cmd.rotation * 0.001f;
        tel.battery_mv     = 12600;
        tel.battery_pct    = 85;
        tel.distance_mm    = 1500;
        tel.ts_ms          = (uint32_t)(rc_time_ms() & 0xFFFFFFFF);
        if (cmd.cmd_type == CMD_EMERGENCY) tel.battery_pct = 0;

        rc_pkt_hdr_t resp;
        rc_hdr_init(&resp, RC_PKT_DATA, hdr.seq, hdr.conn_id);
        resp.payload_len = sizeof(telemetry_t);
        resp.flags       = RC_FLAG_RELIABLE;
        uint8_t out[RC_HEADER_SIZE + sizeof(telemetry_t)];
        rc_hdr_pack(out, &resp);
        memcpy(out + RC_HEADER_SIZE, &tel, sizeof(tel));
        sendto(g_robot_fd, out, sizeof(out), 0,
               (struct sockaddr *)&from, flen);
    }
    return NULL;
}

/* ── send one command, wait for telemetry ──────────────────────────── */
static int send_cmd(uint8_t type, int16_t x, int16_t y, int16_t z,
                    int16_t rot, uint8_t seq, telemetry_t *out_tel)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    assert(fd >= 0);

    control_cmd_t cmd = {
        .cmd_type = type, .seq = seq,
        .axis_x = x, .axis_y = y, .axis_z = z, .rotation = rot,
        .ts_ms  = (uint32_t)(rc_time_ms() & 0xFFFFFFFF)
    };
    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_DATA, (uint32_t)seq, 1);
    hdr.payload_len = sizeof(control_cmd_t);
    hdr.flags       = RC_FLAG_RELIABLE;
    uint8_t pkt[RC_HEADER_SIZE + sizeof(control_cmd_t)];
    rc_hdr_pack(pkt, &hdr);
    memcpy(pkt + RC_HEADER_SIZE, &cmd, sizeof(cmd));

    struct sockaddr_in dst = {
        .sin_family = AF_INET, .sin_port = htons(g_port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
    g_cmds_sent++;

    uint8_t rbuf[512];
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ret = -1;
    if (poll(&pfd, 1, 2000) > 0) {
        ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
        if (n >= (ssize_t)(RC_HEADER_SIZE + sizeof(telemetry_t))) {
            rc_pkt_hdr_t rhdr;
            if (rc_hdr_unpack(&rhdr, rbuf) == 0 && rhdr.type == RC_PKT_DATA) {
                memcpy(out_tel, rbuf + RC_HEADER_SIZE, sizeof(telemetry_t));
                g_cmds_acked++;
                ret = 0;
            }
        }
    }
    close(fd);
    return ret;
}

/* ── tests ─────────────────────────────────────────────────────────── */
static void setup(void)
{
    g_robot_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    assert(g_robot_fd >= 0);
    struct sockaddr_in a = {
        .sin_family = AF_INET, .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    assert(bind(g_robot_fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    socklen_t al = sizeof(a);
    getsockname(g_robot_fd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    g_running = 1;
}

static void teardown(void)
{
    g_running = 0;
    close(g_robot_fd);
}

static void test_joystick(void)
{
    printf("  [TEST] joystick commands ... ");
    fflush(stdout);
    setup();
    pthread_t tid;
    pthread_create(&tid, NULL, robot_thread, NULL);
    usleep(10000);

    int16_t moves[][4] = {
        { 500,  300,    0,    0},
        { 500,  300,    0,   50},
        {-100,  800,    0,    0},
        {   0,    0,  200,    0},
        {-1000,   0,    0, -500},
    };
    for (int i = 0; i < 5; i++) {
        telemetry_t tel;
        int r = send_cmd(CMD_JOYSTICK, moves[i][0], moves[i][1],
                         moves[i][2], moves[i][3], (uint8_t)i, &tel);
        assert(r == 0);
        assert(tel.tel_type == TEL_ACK);
        assert(tel.cmd_seq_acked == (uint8_t)i);
        assert(tel.battery_mv >= 10000);
    }
    teardown();
    pthread_join(tid, NULL);
    printf("PASS\n");
}

static void test_emergency_stop(void)
{
    printf("  [TEST] emergency stop ... ");
    fflush(stdout);
    setup();
    pthread_t tid;
    pthread_create(&tid, NULL, robot_thread, NULL);
    usleep(10000);

    telemetry_t tel;
    send_cmd(CMD_JOYSTICK, 100, 100, 0, 0, 1, &tel);
    int r = send_cmd(CMD_EMERGENCY, 0, 0, 0, 0, 2, &tel);
    assert(r == 0);
    assert(tel.cmd_seq_acked == 2);
    assert(tel.battery_pct == 0);   /* safe state */

    teardown();
    pthread_join(tid, NULL);
    printf("PASS\n");
}

static void test_rapid_fire(void)
{
    printf("  [TEST] rapid-fire 100 commands ... ");
    fflush(stdout);
    setup();
    pthread_t tid;
    pthread_create(&tid, NULL, robot_thread, NULL);
    usleep(10000);

    g_cmds_sent = g_cmds_acked = 0;
    for (int i = 0; i < 100; i++) {
        telemetry_t tel;
        send_cmd(CMD_JOYSTICK, (int16_t)(i*10-500), (int16_t)(500-i*5),
                 0, (int16_t)(i*3), (uint8_t)(i & 0xFF), &tel);
    }
    assert(g_cmds_sent == 100);
    assert(g_cmds_acked >= 80);

    teardown();
    pthread_join(tid, NULL);
    printf("PASS (%d/100 acked)\n", g_cmds_acked);
}

static void test_mode_switch(void)
{
    printf("  [TEST] mode switch ... ");
    fflush(stdout);
    setup();
    pthread_t tid;
    pthread_create(&tid, NULL, robot_thread, NULL);
    usleep(10000);

    telemetry_t tel;
    assert(send_cmd(CMD_MODE, 1, 0, 0, 0, 10, &tel) == 0);
    assert(send_cmd(CMD_MODE, 2, 0, 0, 0, 11, &tel) == 0);
    assert(send_cmd(CMD_MODE, 3, 0, 0, 0, 12, &tel) == 0);

    teardown();
    pthread_join(tid, NULL);
    printf("PASS\n");
}

int main(void)
{
    printf("Running test_robot_control...\n");
    test_joystick();
    test_emergency_stop();
    test_rapid_fire();
    test_mode_switch();
    printf("test_robot_control: ALL PASSED\n");
    return 0;
}
