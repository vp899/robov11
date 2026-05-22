#include <stdbool.h>
/**
 * @file rc_proto.c
 * @brief Protocol implementation — serialisation, CRC-32, validation.
 */

#include "rc_proto.h"

#include <string.h>
#include <arpa/inet.h>
#include <syslog.h>

/* ================================================================== */
/*  Portability: 64-bit byte-swap                                      */
/* ================================================================== */

#ifndef htonll
static inline uint64_t htonll(uint64_t val)
{
    return ((uint64_t)htonl((uint32_t)val) << 32) |
            htonl((uint32_t)(val >> 32));
}
static inline uint64_t ntohll(uint64_t val)
{
    return ((uint64_t)ntohl((uint32_t)val) << 32) |
            ntohl((uint32_t)(val >> 32));
}
#endif

/* ================================================================== */
/*  CRC-32 (IEEE 802.3 polynomial, table-driven)                      */
/* ================================================================== */

/** CRC-32 lookup table (polynomial 0xEDB88320). */
static uint32_t crc32_table[256];
static bool     crc32_table_init = false;

/**
 * @brief Generate the CRC-32 lookup table on first use.
 *
 * Uses the standard IEEE 802.3 polynomial 0xEDB88320 (reflected form).
 * Called once; subsequent calls are no-ops.
 */
static void crc32_init_table(void)
{
    if (crc32_table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_table_init = true;
}

/* ------------------------------------------------------------------ */

uint32_t rc_crc32(const void *data, size_t len)
{
    crc32_init_table();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

/* ================================================================== */
/*  Header init / pack / unpack                                        */
/* ================================================================== */

void rc_hdr_init(rc_pkt_hdr_t *hdr, rc_pkt_type_t type,
                 uint32_t seq, uint32_t conn)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic   = RC_MAGIC;
    hdr->version = RC_PROTO_VERSION;
    hdr->type    = (uint16_t)type;
    hdr->seq     = seq;
    hdr->conn_id = conn;
}

/* ------------------------------------------------------------------ */

void rc_hdr_pack(uint8_t *dst, const rc_pkt_hdr_t *hdr)
{
    uint32_t u32;
    uint16_t u16;
    uint64_t u64;

    u32 = htonl(hdr->magic);           memcpy(dst +  0, &u32, 4);
    u16 = htons(hdr->version);         memcpy(dst +  4, &u16, 2);
    u16 = htons(hdr->type);            memcpy(dst +  6, &u16, 2);
    u32 = htonl(hdr->seq);             memcpy(dst +  8, &u32, 4);
    u32 = htonl(hdr->ack);             memcpy(dst + 12, &u32, 4);
    u64 = htonll(hdr->timestamp);      memcpy(dst + 16, &u64, 8);
    u16 = htons(hdr->payload_len);     memcpy(dst + 24, &u16, 2);
    u16 = htons(hdr->flags);           memcpy(dst + 26, &u16, 2);
    u32 = htonl(hdr->conn_id);         memcpy(dst + 28, &u32, 4);
}

/* ------------------------------------------------------------------ */

int rc_hdr_unpack(rc_pkt_hdr_t *hdr, const uint8_t *src)
{
    uint32_t u32;
    uint16_t u16;
    uint64_t u64;

    memcpy(&u32, src +  0, 4);  hdr->magic       = ntohl(u32);
    memcpy(&u16, src +  4, 2);  hdr->version     = ntohs(u16);
    memcpy(&u16, src +  6, 2);  hdr->type        = ntohs(u16);
    memcpy(&u32, src +  8, 4);  hdr->seq         = ntohl(u32);
    memcpy(&u32, src + 12, 4);  hdr->ack         = ntohl(u32);
    memcpy(&u64, src + 16, 8);  hdr->timestamp   = ntohll(u64);
    memcpy(&u16, src + 24, 2);  hdr->payload_len = ntohs(u16);
    memcpy(&u16, src + 26, 2);  hdr->flags       = ntohs(u16);
    memcpy(&u32, src + 28, 4);  hdr->conn_id     = ntohl(u32);

    if (hdr->magic != RC_MAGIC) {
        syslog(LOG_WARNING, "rc_proto: bad magic 0x%08X (expected 0x%08X)",
               hdr->magic, RC_MAGIC);
        return -1;
    }

    if (hdr->version != RC_PROTO_VERSION) {
        syslog(LOG_WARNING, "rc_proto: unsupported version %u",
               hdr->version);
        return -1;
    }

    return 0;
}

/* ================================================================== */
/*  Packet validation                                                  */
/* ================================================================== */

int rc_pkt_validate(const uint8_t *buf, size_t len)
{
    if (len < RC_HEADER_SIZE) {
        return -1;
    }

    rc_pkt_hdr_t hdr;
    if (rc_hdr_unpack(&hdr, buf) != 0) {
        return -2;
    }

    if ((size_t)hdr.payload_len != len - RC_HEADER_SIZE) {
        return -3;
    }

    if (hdr.payload_len > RC_MAX_PAYLOAD) {
        return -4;
    }

    switch (hdr.type) {
    case RC_PKT_HELLO:
    case RC_PKT_HELLO_ACK:
    case RC_PKT_DATA:
    case RC_PKT_ACK:
    case RC_PKT_NACK:
    case RC_PKT_PING:
    case RC_PKT_PONG:
    case RC_PKT_RELAY_REQ:
    case RC_PKT_RELAY_ACK:
    case RC_PKT_AUTH_REQ:
    case RC_PKT_AUTH_ACK:
    case RC_PKT_FIN:
        break;
    default:
        return -5;
    }

    return 0;
}

/* ================================================================== */
/*  Packet type name                                                   */
/* ================================================================== */

const char *rc_pkt_type_name(rc_pkt_type_t type)
{
    switch (type) {
    case RC_PKT_HELLO:      return "HELLO";
    case RC_PKT_HELLO_ACK:  return "HELLO_ACK";
    case RC_PKT_DATA:       return "DATA";
    case RC_PKT_ACK:        return "ACK";
    case RC_PKT_NACK:       return "NACK";
    case RC_PKT_PING:       return "PING";
    case RC_PKT_PONG:       return "PONG";
    case RC_PKT_RELAY_REQ:  return "RELAY_REQ";
    case RC_PKT_RELAY_ACK:  return "RELAY_ACK";
    case RC_PKT_AUTH_REQ:   return "AUTH_REQ";
    case RC_PKT_AUTH_ACK:   return "AUTH_ACK";
    case RC_PKT_FIN:        return "FIN";
    default:                return "UNKNOWN";
    }
}
