/**
 * @file test_proto.c
 * @brief Unit tests for protocol layer (rc_proto)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rc_proto.h"

static void test_hdr_pack_unpack(void)
{
    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_DATA, 12345, 0xAABBCCDD);
    hdr.payload_len = 1400;

    uint8_t buf[RC_HEADER_SIZE];
    rc_hdr_pack(buf, &hdr);

    rc_pkt_hdr_t unpacked;
    assert(rc_hdr_unpack(&unpacked, buf) == 0);
    assert(unpacked.magic == RC_MAGIC);
    assert(unpacked.type == RC_PKT_DATA);
    assert(unpacked.seq == 12345);
    assert(unpacked.payload_len == 1400);
    assert(unpacked.conn_id == 0xAABBCCDD);

    printf("  PASS: test_hdr_pack_unpack\n");
}

static void test_hdr_invalid_magic(void)
{
    uint8_t buf[RC_HEADER_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xDE; buf[1] = 0xAD; buf[2] = 0xBE; buf[3] = 0xEF;

    rc_pkt_hdr_t hdr;
    assert(rc_hdr_unpack(&hdr, buf) != 0);

    printf("  PASS: test_hdr_invalid_magic\n");
}

static void test_all_packet_types(void)
{
    rc_pkt_type_t types[] = {
        RC_PKT_HELLO, RC_PKT_HELLO_ACK, RC_PKT_DATA, RC_PKT_ACK,
        RC_PKT_NACK, RC_PKT_PING, RC_PKT_PONG, RC_PKT_RELAY_REQ,
        RC_PKT_RELAY_ACK, RC_PKT_AUTH_REQ, RC_PKT_AUTH_ACK, RC_PKT_FIN
    };

    for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++) {
        rc_pkt_hdr_t hdr;
        rc_hdr_init(&hdr, types[i], (uint32_t)i, 1);

        uint8_t buf[RC_HEADER_SIZE];
        rc_hdr_pack(buf, &hdr);

        rc_pkt_hdr_t out;
        assert(rc_hdr_unpack(&out, buf) == 0);
        assert(out.type == types[i]);
    }

    printf("  PASS: test_all_packet_types\n");
}

static void test_crc32_basic(void)
{
    const char *data = "Hello, RoboControl!";
    uint32_t crc1 = rc_crc32((const uint8_t *)data, strlen(data));
    uint32_t crc2 = rc_crc32((const uint8_t *)data, strlen(data));
    assert(crc1 == crc2);

    const char *data2 = "Hello, RoboControl?";
    uint32_t crc3 = rc_crc32((const uint8_t *)data2, strlen(data2));
    assert(crc1 != crc3);

    printf("  PASS: test_crc32_basic\n");
}

static void test_crc32_empty(void)
{
    uint32_t crc = rc_crc32(NULL, 0);
    (void)crc;
    printf("  PASS: test_crc32_empty\n");
}

static void test_pkt_validate(void)
{
    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_DATA, 0, 1);
    hdr.payload_len = 10;

    uint8_t buf[RC_HEADER_SIZE + 10];
    rc_hdr_pack(buf, &hdr);
    memset(buf + RC_HEADER_SIZE, 0xAA, 10);

    assert(rc_pkt_validate(buf, sizeof(buf)) == 0);
    assert(rc_pkt_validate(buf, 5) != 0);

    printf("  PASS: test_pkt_validate\n");
}

int main(void)
{
    printf("Running test_proto...\n");
    test_hdr_pack_unpack();
    test_hdr_invalid_magic();
    test_all_packet_types();
    test_crc32_basic();
    test_crc32_empty();
    test_pkt_validate();
    printf("test_proto: ALL PASSED\n");
    return 0;
}
