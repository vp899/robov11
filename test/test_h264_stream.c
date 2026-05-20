/**
 * @file test_h264_stream.c
 * @brief H264 video stream simulation — I-frame / P-frame delivery.
 *
 * Simulates a robot camera streaming H264-encoded frames over UDP.
 * Validates:
 *   - I-frame (keyframe) delivery and reassembly from fragments
 *   - P-frame (delta) delivery
 *   - Full GOP sequence (1 I + 29 P)
 *   - Gap detection and I-frame recovery after packet loss
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
#include "rc_epoll.h"

#define FRAME_TYPE_I     0
#define FRAME_TYPE_P     1
#define MAX_PACKET_SZ    1400
#define IFRAME_FRAGS     20      /* ~28 KB */
#define PFRAME_FRAGS     4       /* ~5.6 KB */
#define GOP_SIZE         30

/* ── H264 fragment header (layered on DATA) ────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  frame_type;
    uint8_t  stream_id;
    uint16_t frame_seq;
    uint16_t frag_idx;
    uint16_t frag_total;
    uint32_t frame_size;
    uint32_t ts_ms;
} h264_frag_t;

/* ── frame reassembly ──────────────────────────────────────────────── */
typedef struct {
    uint16_t frame_seq;
    uint8_t  frame_type;
    uint32_t frame_size;
    uint8_t  data[65000];
    uint32_t bytes_recv;
    uint16_t frags_recv;
    uint16_t frag_total;
    int      complete;
} reasm_t;

static void reasm_reset(reasm_t *r) { memset(r, 0, sizeof(*r)); }

static int reasm_add(reasm_t *r, const h264_frag_t *f, const uint8_t *pay, size_t plen)
{
    if (f->frag_idx == 0 || f->frame_seq != r->frame_seq) {
        reasm_reset(r);
        r->frame_seq  = f->frame_seq;
        r->frame_type = f->frame_type;
        r->frame_size = f->frame_size;
        r->frag_total = f->frag_total;
    }
    uint32_t off = (uint32_t)f->frag_idx * MAX_PACKET_SZ;
    if (off + plen > sizeof(r->data)) return -1;
    memcpy(r->data + off, pay, plen);
    r->bytes_recv += (uint32_t)plen;
    r->frags_recv++;
    if (r->frags_recv >= r->frag_total) r->complete = 1;
    return 0;
}

/* ── UDP helpers ───────────────────────────────────────────────────── */
typedef struct { int fd; uint16_t port; struct sockaddr_in peer; int peer_set; } chan_t;

static chan_t chan_new(void)
{
    chan_t c = { .fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0), .peer_set = 0 };
    assert(c.fd >= 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(c.fd, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(c.fd, (struct sockaddr *)&a, &al);
    c.port = ntohs(a.sin_port);
    return c;
}

static void chan_link(chan_t *a, chan_t *b)
{
    a->peer.sin_family = AF_INET;
    a->peer.sin_port   = htons(b->port);
    a->peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a->peer_set = 1;
    b->peer.sin_family = AF_INET;
    b->peer.sin_port   = htons(a->port);
    b->peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    b->peer_set = 1;
}

static int chan_send(chan_t *c, const uint8_t *data, size_t len)
{
    return (int)sendto(c->fd, data, len, 0,
                       (struct sockaddr *)&c->peer, sizeof(c->peer));
}

static ssize_t chan_recv(chan_t *c, uint8_t *buf, size_t blen, int ms)
{
    struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
    if (poll(&pfd, 1, ms) <= 0) return -1;
    return recv(c->fd, buf, blen, 0);
}

/* ── frame generators ──────────────────────────────────────────────── */
static void gen_iframe(uint8_t *buf, size_t sz, uint16_t seq)
{
    buf[0]=0x00; buf[1]=0x00; buf[2]=0x00; buf[3]=0x01;
    buf[4]=0x65; /* IDR */
    buf[5]=(uint8_t)(seq>>8); buf[6]=(uint8_t)(seq&0xFF);
    for (size_t i=7; i<sz; i++) buf[i]=(uint8_t)((i*7+seq*31)&0xFF);
}

static void gen_pframe(uint8_t *buf, size_t sz, uint16_t seq)
{
    buf[0]=0x00; buf[1]=0x00; buf[2]=0x00; buf[3]=0x01;
    buf[4]=0x41; /* non-IDR */
    buf[5]=(uint8_t)(seq>>8); buf[6]=(uint8_t)(seq&0xFF);
    for (size_t i=7; i<sz; i++) buf[i]=(uint8_t)((i*3+seq*13)&0xFF);
}

/* ── send one frame as N fragments ─────────────────────────────────── */
static void send_frame(chan_t *c, uint8_t ftype, uint16_t fseq,
                       const uint8_t *data, uint32_t size)
{
    uint16_t nfrags = (uint16_t)((size + MAX_PACKET_SZ - 1) / MAX_PACKET_SZ);
    for (uint16_t f = 0; f < nfrags; f++) {
        uint32_t off = (uint32_t)f * MAX_PACKET_SZ;
        uint32_t flen = size - off;
        if (flen > MAX_PACKET_SZ) flen = MAX_PACKET_SZ;

        rc_pkt_hdr_t hdr;
        rc_hdr_init(&hdr, RC_PKT_DATA, (uint32_t)fseq*1000+f, 1);
        hdr.payload_len = (uint16_t)(sizeof(h264_frag_t) + flen);
        hdr.flags = RC_FLAG_RELIABLE;

        uint8_t pkt[RC_HEADER_SIZE + sizeof(h264_frag_t) + MAX_PACKET_SZ + 16];
        rc_hdr_pack(pkt, &hdr);

        h264_frag_t fh = {
            .frame_type=ftype, .stream_id=1, .frame_seq=fseq,
            .frag_idx=f, .frag_total=nfrags,
            .frame_size=size, .ts_ms=(uint32_t)(rc_time_ms()&0xFFFFFFFF)
        };
        memcpy(pkt+RC_HEADER_SIZE, &fh, sizeof(fh));
        memcpy(pkt+RC_HEADER_SIZE+sizeof(fh), data+off, flen);
        chan_send(c, pkt, RC_HEADER_SIZE+sizeof(fh)+flen);
    }
}

/* ── drain one frame from channel ──────────────────────────────────── */
static int recv_frame(chan_t *c, reasm_t *ra, int timeout_ms)
{
    reasm_reset(ra);
    uint8_t buf[RC_HEADER_SIZE + sizeof(h264_frag_t) + MAX_PACKET_SZ + 64];
    uint64_t deadline = rc_time_ms() + timeout_ms;
    while (rc_time_ms() < deadline) {
        ssize_t n = chan_recv(c, buf, sizeof(buf), 50);
        if (n < (ssize_t)(RC_HEADER_SIZE + sizeof(h264_frag_t))) continue;
        h264_frag_t fh;
        memcpy(&fh, buf+RC_HEADER_SIZE, sizeof(fh));
        reasm_add(ra, &fh,
                  buf+RC_HEADER_SIZE+sizeof(fh),
                  (size_t)n-RC_HEADER_SIZE-sizeof(fh));
        if (ra->complete) return 0;
    }
    return -1;
}

/* ── tests ─────────────────────────────────────────────────────────── */

static void test_iframe(void)
{
    printf("  [TEST] I-frame delivery ... ");
    fflush(stdout);
    chan_t tx = chan_new(), rx = chan_new();
    chan_link(&tx, &rx);

    uint32_t sz = IFRAME_FRAGS * MAX_PACKET_SZ;
    uint8_t *frame = malloc(sz);
    gen_iframe(frame, sz, 0);
    send_frame(&tx, FRAME_TYPE_I, 0, frame, sz);

    reasm_t ra;
    int r = recv_frame(&rx, &ra, 3000);
    assert(r == 0);
    assert(ra.complete);
    assert(ra.frame_type == FRAME_TYPE_I);
    assert(ra.data[4] == 0x65);  /* IDR NAL */
    assert(ra.bytes_recv == sz);

    free(frame);
    close(tx.fd); close(rx.fd);
    printf("PASS\n");
}

static void test_pframe(void)
{
    printf("  [TEST] P-frame delivery ... ");
    fflush(stdout);
    chan_t tx = chan_new(), rx = chan_new();
    chan_link(&tx, &rx);

    uint32_t sz = PFRAME_FRAGS * MAX_PACKET_SZ;
    uint8_t *frame = malloc(sz);
    gen_pframe(frame, sz, 5);
    send_frame(&tx, FRAME_TYPE_P, 5, frame, sz);

    reasm_t ra;
    int r = recv_frame(&rx, &ra, 2000);
    assert(r == 0);
    assert(ra.complete);
    assert(ra.frame_type == FRAME_TYPE_P);
    assert(ra.data[4] == 0x41);  /* non-IDR NAL */

    free(frame);
    close(tx.fd); close(rx.fd);
    printf("PASS\n");
}

static void test_gop(void)
{
    printf("  [TEST] GOP sequence (1I + 29P) ... ");
    fflush(stdout);
    chan_t tx = chan_new(), rx = chan_new();
    chan_link(&tx, &rx);

    int complete = 0;
    for (int f = 0; f < GOP_SIZE; f++) {
        int is_i = (f == 0);
        uint32_t sz = (is_i ? IFRAME_FRAGS : PFRAME_FRAGS) * MAX_PACKET_SZ;
        uint8_t *frame = malloc(sz);
        if (is_i) gen_iframe(frame, sz, (uint16_t)f);
        else      gen_pframe(frame, sz, (uint16_t)f);

        send_frame(&tx, is_i ? FRAME_TYPE_I : FRAME_TYPE_P,
                   (uint16_t)f, frame, sz);

        reasm_t ra;
        if (recv_frame(&rx, &ra, 2000) == 0 && ra.complete) complete++;
        free(frame);
    }

    assert(complete >= GOP_SIZE * 90 / 100);
    close(tx.fd); close(rx.fd);
    printf("PASS (%d/%d complete)\n", complete, GOP_SIZE);
}

static void test_recovery(void)
{
    printf("  [TEST] I-frame recovery after loss ... ");
    fflush(stdout);
    chan_t tx = chan_new(), rx = chan_new();
    chan_link(&tx, &rx);

    /* send I(0) */
    uint32_t isz = IFRAME_FRAGS * MAX_PACKET_SZ;
    uint8_t *iframe = malloc(isz);
    gen_iframe(iframe, isz, 0);
    send_frame(&tx, FRAME_TYPE_I, 0, iframe, isz);
    reasm_t ra;
    recv_frame(&rx, &ra, 3000);  /* drain */

    /* send P(1) */
    uint32_t psz = PFRAME_FRAGS * MAX_PACKET_SZ;
    uint8_t *pframe = malloc(psz);
    gen_pframe(pframe, psz, 1);
    send_frame(&tx, FRAME_TYPE_P, 1, pframe, psz);
    recv_frame(&rx, &ra, 2000);

    /* P(2) dropped — skip */

    /* send P(3) — receiver detects gap */
    gen_pframe(pframe, psz, 3);
    send_frame(&tx, FRAME_TYPE_P, 3, pframe, psz);

    /* receive P(3) and check frame_seq jump */
    reasm_t p3;
    recv_frame(&rx, &p3, 2000);
    /* p3.frame_seq == 3, but last seen was 1 → gap detected */
    int gap_detected = (p3.frame_seq > 1 + 1);

    /* recovery: send I(4) */
    gen_iframe(iframe, isz, 4);
    send_frame(&tx, FRAME_TYPE_I, 4, iframe, isz);
    reasm_t rec;
    int r = recv_frame(&rx, &rec, 3000);

    assert(gap_detected);
    assert(r == 0 && rec.complete);
    assert(rec.frame_type == FRAME_TYPE_I);
    assert(rec.frame_seq == 4);

    free(iframe); free(pframe);
    close(tx.fd); close(rx.fd);
    printf("PASS\n");
}

int main(void)
{
    printf("Running test_h264_stream...\n");
    test_iframe();
    test_pframe();
    test_gop();
    test_recovery();
    printf("test_h264_stream: ALL PASSED\n");
    return 0;
}
