/**
 * @file rc_proto.h
 * @brief RoboControl wire protocol definitions.
 *
 * Defines the binary packet format used for all communication between
 * controllers and robots.  Every packet on the wire begins with a fixed
 * 32-byte header followed by an optional payload (≤ RC_MAX_PAYLOAD).
 *
 * Design goals:
 *   - Fixed header size for zero-copy parsing in the fast path.
 *   - CRC-32 integrity check on every packet.
 *   - Sequence numbers for ACK/loss detection.
 *   - Connection ID for demuxing without deep inspection.
 *
 * @note All multi-byte fields are in **network byte order** (big-endian).
 */

#ifndef RC_PROTO_H
#define RC_PROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

/** Magic bytes identifying a RoboControl packet: "ROBO" */
#define RC_MAGIC            0x524F424FU

/** Current protocol version */
#define RC_PROTO_VERSION    1U

/** Maximum payload size (bytes).  Fits comfortably in one MTU. */
#define RC_MAX_PAYLOAD      1400

/** Maximum number of concurrent connections the system supports. */
#define RC_MAX_CONNECTIONS  10000000UL   /* 10 million */

/** Handshake timeout in milliseconds. */
#define RC_HANDSHAKE_TIMEOUT_MS  10000

/** Idle connection keep-alive interval in milliseconds. */
#define RC_KEEPALIVE_INTERVAL_MS 30000

/** Maximum retransmission timeout in milliseconds. */
#define RC_MAX_RTO_MS       60000

/** Minimum retransmission timeout in milliseconds. */
#define RC_MIN_RTO_MS       200

/** Number of duplicate ACKs that trigger fast retransmit. */
#define RC_DUP_ACK_THRESH   3

/** Maximum SACK blocks per packet. */
#define RC_MAX_SACK_BLOCKS  4

/** Size of the fixed packet header in bytes. */
#define RC_HEADER_SIZE      32

/* ------------------------------------------------------------------ */
/*  Packet types                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Packet type identifiers.
 *
 * Encoded in the 16-bit `type` field of every header.
 */
typedef enum rc_pkt_type {
    RC_PKT_HELLO      = 0x0001,  /**< Client → Server: initiate handshake  */
    RC_PKT_HELLO_ACK  = 0x0002,  /**< Server → Client: handshake response  */
    RC_PKT_DATA       = 0x0003,  /**< Application data                     */
    RC_PKT_ACK        = 0x0004,  /**< Cumulative / selective acknowledgement*/
    RC_PKT_NACK       = 0x0005,  /**< Negative acknowledgement (loss)      */
    RC_PKT_PING       = 0x0006,  /**< Liveness probe                       */
    RC_PKT_PONG       = 0x0007,  /**< Liveness response                    */
    RC_PKT_RELAY_REQ  = 0x0008,  /**< Request relay service                */
    RC_PKT_RELAY_ACK  = 0x0009,  /**< Relay assignment response            */
    RC_PKT_AUTH_REQ   = 0x000A,  /**< Authentication request               */
    RC_PKT_AUTH_ACK   = 0x000B,  /**< Authentication response              */
    RC_PKT_FIN        = 0x00FF,  /**< Graceful close                       */
} rc_pkt_type_t;

/* ------------------------------------------------------------------ */
/*  Header flags                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Bit-flags for the 16-bit `flags` field.
 */
typedef enum rc_hdr_flag {
    RC_FLAG_ENCRYPTED  = (1 << 0),  /**< Payload is encrypted               */
    RC_FLAG_COMPRESSED = (1 << 1),  /**< Payload is zlib-compressed          */
    RC_FLAG_SACK       = (1 << 2),  /**< ACK contains SACK blocks           */
    RC_FLAG_FIN_ACK    = (1 << 3),  /**< FIN acknowledgement                 */
    RC_FLAG_RELIABLE   = (1 << 4),  /**< Delivery guaranteed (retransmit)    */
    RC_FLAG_PRIORITY   = (1 << 5),  /**< High-priority packet                */
} rc_hdr_flag_t;

/* ------------------------------------------------------------------ */
/*  Wire-format header (32 bytes, packed)                              */
/* ------------------------------------------------------------------ */

/**
 * @brief 32-byte packet header — layout matches the wire exactly.
 *
 * Field layout (network byte order):
 * @code
 *  Offset  Size  Field
 *  ──────  ────  ─────────────
 *   0       4    magic
 *   4       2    version
 *   6       2    type
 *   8       4    seq
 *  12       4    ack
 *  16       8    timestamp
 *  24       2    payload_len
 *  26       2    flags
 *  28       4    conn_id
 * @endcode
 *
 * Total: 32 bytes.  The struct is packed to guarantee exact layout.
 */
typedef struct __attribute__((packed)) rc_pkt_hdr {
    uint32_t magic;         /**< RC_MAGIC ("ROBO")                    */
    uint16_t version;       /**< Protocol version (RC_PROTO_VERSION)  */
    uint16_t type;          /**< rc_pkt_type_t                        */
    uint32_t seq;           /**< Sender's sequence number             */
    uint32_t ack;           /**< Cumulative ACK number                */
    uint64_t timestamp;     /**< Sender timestamp (µs since epoch)    */
    uint16_t payload_len;   /**< Length of payload following header   */
    uint16_t flags;         /**< Bitmask of rc_hdr_flag_t             */
    uint32_t conn_id;       /**< Connection identifier                */
} rc_pkt_hdr_t;

/** Compile-time sanity: header must be exactly 32 bytes. */
_Static_assert(sizeof(rc_pkt_hdr_t) == RC_HEADER_SIZE,
               "rc_pkt_hdr_t must be 32 bytes");

/* ------------------------------------------------------------------ */
/*  SACK block                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Selective acknowledgement block.
 *
 * Each block describes a contiguous range of received octets:
 *   [left_edge, right_edge)  (half-open interval).
 */
typedef struct rc_sack_block {
    uint32_t left_edge;     /**< First byte of the acknowledged range */
    uint32_t right_edge;    /**< Byte after the last acknowledged     */
} rc_sack_block_t;

/* ------------------------------------------------------------------ */
/*  Handshake message structures                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief ClientHello — first message of the 3-way handshake.
 *
 * Sent by the client after it has generated its ephemeral X25519 key
 * pair.  Contains the public key, a random nonce, and an identity
 * hint so the server can look up credentials.
 */
typedef struct __attribute__((packed)) rc_client_hello {
    uint32_t conn_id;                   /**< Proposed connection ID        */
    uint8_t  client_random[32];         /**< 256-bit random nonce          */
    uint8_t  client_pubkey[32];         /**< X25519 public key             */
    uint16_t cipher_suites[4];          /**< Preferred cipher suite IDs    */
    uint16_t extensions_len;            /**< Length of extensions blob      */
    uint8_t  extensions[128];           /**< Extensions (opaque to proto)  */
} rc_client_hello_t;

/**
 * @brief ServerHello — second message of the 3-way handshake.
 *
 * Sent by the server in response to ClientHello.  Contains the
 * server's X25519 public key, a server random nonce, the selected
 * cipher suite, and a session ticket for resumption.
 */
typedef struct __attribute__((packed)) rc_server_hello {
    uint32_t conn_id;                   /**< Confirmed connection ID       */
    uint8_t  server_random[32];         /**< 256-bit random nonce          */
    uint8_t  server_pubkey[32];         /**< X25519 public key             */
    uint16_t selected_cipher;           /**< Chosen cipher suite           */
    uint32_t session_id;                /**< Session ticket for resumption */
    uint16_t extensions_len;            /**< Length of extensions blob      */
    uint8_t  extensions[128];           /**< Extensions                    */
} rc_server_hello_t;

/**
 * @brief Finish — third message of the 3-way handshake.
 *
 * Sent by the client after deriving the session keys.  Carries a
 * HMAC-SHA256 transcript hash that proves the client knows the shared
 * secret, binding the handshake to prevent tampering.
 */
typedef struct __attribute__((packed)) rc_finish {
    uint32_t conn_id;                   /**< Connection ID                 */
    uint8_t  verify_data[32];           /**< HMAC-SHA256(transcript, key)  */
} rc_finish_t;

/* ------------------------------------------------------------------ */
/*  Cipher suites                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Supported cipher suites.
 *
 * Each suite specifies the AEAD algorithm for record encryption.
 */
typedef enum rc_cipher_suite {
    RC_CS_AES_256_GCM          = 0x0001,
    RC_CS_CHACHA20_POLY1305    = 0x0002,
} rc_cipher_suite_t;

/* ------------------------------------------------------------------ */
/*  Utility macros                                                     */
/* ------------------------------------------------------------------ */

/** Round up @p x to the next multiple of @p a (power of 2). */
#define RC_ALIGN(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))

/** Return the minimum of two values. */
#define RC_MIN(a, b)    ((a) < (b) ? (a) : (b))

/** Return the maximum of two values. */
#define RC_MAX(a, b)    ((a) > (b) ? (a) : (b))

/* ------------------------------------------------------------------ */
/*  Function prototypes                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise a packet header with sensible defaults.
 * @param[out] hdr   Pointer to header to fill.
 * @param      type  Packet type.
 * @param      seq   Sequence number.
 * @param      conn  Connection ID.
 */
void rc_hdr_init(rc_pkt_hdr_t *hdr, rc_pkt_type_t type,
                 uint32_t seq, uint32_t conn);

/**
 * @brief Serialise a header into network byte order.
 * @param[out] dst  Destination buffer (≥ RC_HEADER_SIZE bytes).
 * @param[in]  hdr  Header to serialise.
 */
void rc_hdr_pack(uint8_t *dst, const rc_pkt_hdr_t *hdr);

/**
 * @brief Deserialise a header from network byte order.
 * @param[out] hdr  Header to fill.
 * @param[in]  src  Source buffer (≥ RC_HEADER_SIZE bytes).
 * @return 0 on success, -1 on invalid magic/version.
 */
int rc_hdr_unpack(rc_pkt_hdr_t *hdr, const uint8_t *src);

/**
 * @brief Compute CRC-32 over a buffer.
 * @param data  Input data.
 * @param len   Length in bytes.
 * @return CRC-32 value.
 */
uint32_t rc_crc32(const void *data, size_t len);

/**
 * @brief Validate an incoming packet.
 *
 * Checks magic, version, type, payload length, and CRC.
 *
 * @param[in]  buf  Raw packet bytes (header + payload).
 * @param      len  Total buffer length.
 * @return 0 if valid, negative error code otherwise.
 */
int rc_pkt_validate(const uint8_t *buf, size_t len);

/**
 * @brief Get a human-readable name for a packet type.
 * @param type  Packet type.
 * @return Static string (never NULL).
 */
const char *rc_pkt_type_name(rc_pkt_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* RC_PROTO_H */
