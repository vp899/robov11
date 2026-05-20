/*
 * robocontrol/proto.h - Wire protocol definitions
 * Binary framing: [magic:2][version:1][type:1][flags:1][reserved:1][session:4][seq:4][length:2][payload:var][crc32:4]
 * Total header: 16 bytes, max payload: 64KB
 */
#ifndef ROBOCONTROL_PROTO_H
#define ROBOCONTROL_PROTO_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Protocol constants ─────────────────────────────────────── */
#define RC_PROTO_MAGIC          0x5243  /* "RC" */
#define RC_PROTO_VERSION        1
#define RC_PROTO_HEADER_SIZE    16
#define RC_PROTO_MAX_PAYLOAD    65536
#define RC_PROTO_MAX_PACKET     (RC_PROTO_HEADER_SIZE + RC_PROTO_MAX_PAYLOAD + 4)

/* ── Message types ──────────────────────────────────────────── */
typedef enum {
    RC_MSG_HANDSHAKE_REQ    = 0x01,
    RC_MSG_HANDSHAKE_RES    = 0x02,
    RC_MSG_HANDSHAKE_ACK    = 0x03,
    RC_MSG_DATA             = 0x10,
    RC_MSG_DATA_ACK         = 0x11,
    RC_MSG_PING             = 0x20,
    RC_MSG_PONG             = 0x21,
    RC_MSG_DISCONNECT       = 0x30,
    RC_MSG_ERROR            = 0x31,
    /* Signaling messages */
    RC_MSG_JOIN_ROOM        = 0x40,
    RC_MSG_LEAVE_ROOM       = 0x41,
    RC_MSG_OFFER            = 0x42,
    RC_MSG_ANSWER           = 0x43,
    RC_MSG_ICE_CANDIDATE    = 0x44,
    RC_MSG_ROOM_LIST        = 0x45,
    RC_MSG_PEER_JOIN        = 0x46,
    RC_MSG_PEER_LEAVE       = 0x47,
    /* Relay control */
    RC_MSG_RELAY_CREATE     = 0x50,
    RC_MSG_RELAY_DESTROY    = 0x51,
    RC_MSG_RELAY_DATA       = 0x52,
    RC_MSG_RELAY_STATS      = 0x53,
    RC_MSG_RELAY_MIGRATE    = 0x54,
} rc_msg_type_t;

/* ── Flags ──────────────────────────────────────────────────── */
#define RC_FLAG_ENCRYPTED   0x01
#define RC_FLAG_COMPRESSED  0x02
#define RC_FLAG_RELIABLE    0x04
#define RC_FLAG_FRAGMENT    0x08
#define RC_FLAG_LAST_FRAG   0x10
#define RC_FLAG_ACK_REQ     0x20

/* ── Packet structure ───────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  magic[2];      /* RC_PROTO_MAGIC */
    uint8_t  version;
    uint8_t  type;           /* rc_msg_type_t */
    uint8_t  flags;
    uint8_t  reserved;
    uint32_t session_id;
    uint32_t seq_num;
    uint16_t payload_len;
    uint16_t reserved2;
    /* followed by payload[payload_len] then 4-byte CRC32 */
} rc_packet_t;

/* ── Handshake request payload ──────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t proto_version;
    uint32_t client_id;
    uint32_t capabilities;   /* bitmask */
    uint16_t mtu;
    uint16_t token_len;
    /* followed by: token[token_len] */
} rc_handshake_req_t;

/* ── Handshake response payload ─────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t session_id;
    uint32_t server_id;
    uint32_t capabilities;
    uint16_t mtu;
    uint16_t assigned_port;
    uint8_t  status;         /* 0=ok, 1=auth_fail, 2=full, 3=version_mismatch */
    uint8_t  reserved[3];
} rc_handshake_res_t;

/* ── Handshake ACK payload ──────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t session_id;
    uint32_t client_nonce;
    uint32_t server_nonce;
} rc_handshake_ack_t;

/* ── ICE candidate payload ──────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  candidate_type;  /* 0=host, 1=srflx, 2=relay */
    uint8_t  transport;       /* 0=udp, 1=tcp */
    uint16_t port;
    uint32_t address;
    uint16_t foundation_len;
    /* followed by: foundation[foundation_len] */
} rc_ice_candidate_t;

/* ── Disconnect payload ─────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t reason;
    uint16_t message_len;
    /* followed by: message[message_len] */
} rc_disconnect_t;

/* Disconnect reasons */
#define RC_DISCONNECT_NORMAL     0
#define RC_DISCONNECT_TIMEOUT    1
#define RC_DISCONNECT_ERROR      2
#define RC_DISCONNECT_KICKED     3
#define RC_DISCONNECT_RATE_LIMIT 4

/* ── Error payload ──────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t error_code;
    uint16_t message_len;
    /* followed by: message[message_len] */
} rc_error_t;

/* ── CRC32 (Castagnoli / iSCSI polynomial is also fine, but we use standard) ── */
static inline uint32_t rc_crc32(const void *data, size_t len) {
    static uint32_t table[256];
    static int initialized = 0;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/* ── Packet encode/decode ───────────────────────────────────── */
static inline int rc_packet_encode(rc_msg_type_t type, uint8_t flags,
                                    uint32_t session_id, uint32_t seq_num,
                                    const void *payload, uint16_t payload_len,
                                    uint8_t *out, size_t out_size) {
    size_t total = RC_PROTO_HEADER_SIZE + (size_t)payload_len + 4; /* +4 for CRC */
    if (out_size < total || payload_len > RC_PROTO_MAX_PAYLOAD) return RC_ERR_INVAL;

    rc_packet_t *pkt = (rc_packet_t *)out;
    pkt->magic[0] = (RC_PROTO_MAGIC >> 8) & 0xFF;
    pkt->magic[1] = RC_PROTO_MAGIC & 0xFF;
    pkt->version = RC_PROTO_VERSION;
    pkt->type = (uint8_t)type;
    pkt->flags = flags;
    pkt->reserved = 0;
    pkt->session_id = session_id;
    pkt->seq_num = seq_num;
    pkt->payload_len = payload_len;
    pkt->reserved2 = 0;

    if (payload_len > 0 && payload) {
        memcpy(out + RC_PROTO_HEADER_SIZE, payload, payload_len);
    }

    /* CRC over header + payload */
    uint32_t crc = rc_crc32(out, RC_PROTO_HEADER_SIZE + payload_len);
    memcpy(out + RC_PROTO_HEADER_SIZE + payload_len, &crc, 4);

    return (int)total;
}

static inline int rc_packet_decode(const uint8_t *data, size_t data_len,
                                    rc_msg_type_t *out_type, uint8_t *out_flags,
                                    uint32_t *out_session, uint32_t *out_seq,
                                    const uint8_t **out_payload, uint16_t *out_payload_len) {
    if (data_len < RC_PROTO_HEADER_SIZE) return RC_ERR_PROTO;

    const rc_packet_t *pkt = (const rc_packet_t *)data;

    /* Verify magic */
    uint16_t magic = ((uint16_t)pkt->magic[0] << 8) | pkt->magic[1];
    if (magic != RC_PROTO_MAGIC) return RC_ERR_PROTO;
    if (pkt->version != RC_PROTO_VERSION) return RC_ERR_PROTO;

    uint16_t plen = pkt->payload_len;
    size_t total = RC_PROTO_HEADER_SIZE + (size_t)plen + 4;
    if (data_len < total) return RC_ERR_PROTO;
    if (plen > RC_PROTO_MAX_PAYLOAD) return RC_ERR_PROTO;

    /* Verify CRC */
    uint32_t expected_crc, actual_crc;
    memcpy(&expected_crc, data + RC_PROTO_HEADER_SIZE + plen, 4);
    actual_crc = rc_crc32(data, RC_PROTO_HEADER_SIZE + plen);
    if (expected_crc != actual_crc) return RC_ERR_PROTO;

    *out_type = (rc_msg_type_t)pkt->type;
    *out_flags = pkt->flags;
    *out_session = pkt->session_id;
    *out_seq = pkt->seq_num;
    *out_payload = (plen > 0) ? data + RC_PROTO_HEADER_SIZE : NULL;
    *out_payload_len = plen;

    return (int)total;
}

/* ── Handshake helpers ──────────────────────────────────────── */
static inline int rc_encode_handshake_req(uint32_t client_id, uint32_t caps,
                                           uint16_t mtu, const char *token,
                                           uint8_t *out, size_t out_size) {
    uint16_t tlen = token ? (uint16_t)strlen(token) : 0;
    uint8_t buf[sizeof(rc_handshake_req_t) + MAX_TOKEN_LEN];
    rc_handshake_req_t *req = (rc_handshake_req_t *)buf;
    req->proto_version = RC_PROTO_VERSION;
    req->client_id = client_id;
    req->capabilities = caps;
    req->mtu = mtu;
    req->token_len = tlen;
    if (tlen > 0) memcpy(buf + sizeof(*req), token, tlen);

    return rc_packet_encode(RC_MSG_HANDSHAKE_REQ, RC_FLAG_RELIABLE,
                            0, 0, buf, (uint16_t)(sizeof(*req) + tlen), out, out_size);
}

static inline int rc_encode_handshake_res(uint32_t session_id, uint32_t server_id,
                                           uint32_t caps, uint16_t mtu,
                                           uint16_t port, uint8_t status,
                                           uint8_t *out, size_t out_size) {
    rc_handshake_res_t res = {
        .session_id = session_id,
        .server_id = server_id,
        .capabilities = caps,
        .mtu = mtu,
        .assigned_port = port,
        .status = status,
    };
    return rc_packet_encode(RC_MSG_HANDSHAKE_RES, RC_FLAG_RELIABLE,
                            0, 0, &res, sizeof(res), out, out_size);
}

static inline int rc_encode_handshake_ack(uint32_t session_id, uint32_t client_nonce,
                                           uint32_t server_nonce,
                                           uint8_t *out, size_t out_size) {
    rc_handshake_ack_t ack = {
        .session_id = session_id,
        .client_nonce = client_nonce,
        .server_nonce = server_nonce,
    };
    return rc_packet_encode(RC_MSG_HANDSHAKE_ACK, RC_FLAG_RELIABLE,
                            session_id, 0, &ack, sizeof(ack), out, out_size);
}

static inline int rc_encode_ping(uint32_t session_id, uint32_t seq,
                                  uint8_t *out, size_t out_size) {
    return rc_packet_encode(RC_MSG_PING, 0, session_id, seq, NULL, 0, out, out_size);
}

static inline int rc_encode_pong(uint32_t session_id, uint32_t seq,
                                  uint8_t *out, size_t out_size) {
    return rc_packet_encode(RC_MSG_PONG, 0, session_id, seq, NULL, 0, out, out_size);
}

static inline int rc_encode_disconnect(uint32_t session_id, uint16_t reason,
                                        const char *msg,
                                        uint8_t *out, size_t out_size) {
    uint16_t mlen = msg ? (uint16_t)strlen(msg) : 0;
    if (mlen > 256) mlen = 256;
    uint8_t buf[sizeof(rc_disconnect_t) + 256];
    rc_disconnect_t *disc = (rc_disconnect_t *)buf;
    disc->reason = reason;
    disc->message_len = mlen;
    if (mlen > 0) memcpy(buf + sizeof(*disc), msg, mlen);

    return rc_packet_encode(RC_MSG_DISCONNECT, RC_FLAG_RELIABLE,
                            session_id, 0, buf, (uint16_t)(sizeof(*disc) + mlen), out, out_size);
}

#ifdef __cplusplus
}
#endif

#endif /* ROBOCONTROL_PROTO_H */
