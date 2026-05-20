#include <stdbool.h>
/**
 * @file rc_handshake.c
 * @brief 3-way handshake with ECDHE (X25519) key exchange.
 *
 * Handshake flow:
 *   1. Client → Server:  ClientHello  (pubkey, random, cipher_suites)
 *   2. Server → Client:  ServerHello  (pubkey, random, selected_cipher)
 *   3. Client → Server:  Finish       (verify_data = HMAC(transcript))
 *
 * After step 2 both sides derive session keys via HKDF.
 * After step 3 the server verifies the client knows the shared secret.
 *
 * Session resumption: if the client presents a valid session_id, the
 * server skips the full key exchange and derives keys from the stored
 * session ticket.
 */

#include "rc_conn.h"
#include "rc_proto.h"
#include "rc_crypto.h"

#include <string.h>
#include <stdlib.h>
#include <syslog.h>

/* --- Function prototypes (satisfy -Wmissing-prototypes) ------------- */
int rc_handshake_send_client_hello(rc_conn_t *conn);
int rc_handshake_process_server_hello(rc_conn_t *conn,
                                      const rc_server_hello_t *server_hello);
int rc_handshake_process_client_hello(rc_conn_t *conn,
                                      const rc_client_hello_t *client_hello);
int rc_handshake_process_finish(rc_conn_t *conn, const rc_finish_t *finish);
int rc_handshake_resume(rc_conn_t *conn, uint32_t session_id);

/* ================================================================== */
/*  Transcript hash                                                    */
/* ================================================================== */

/**
 * @brief Build the handshake transcript for verification.
 *
 * Concatenates all handshake messages (serialised) and hashes them
 * with HMAC-SHA256 using the derived master secret as key.
 *
 * @param client_hello   Serialised ClientHello.
 * @param ch_len         ClientHello length.
 * @param server_hello   Serialised ServerHello.
 * @param sh_len         ServerHello length.
 * @param master_secret  Master key material (32 bytes).
 * @param[out] verify    Output verify_data (32 bytes).
 * @return 0 on success, -1 on failure.
 */
static int build_transcript(const uint8_t *client_hello, size_t ch_len,
                            const uint8_t *server_hello, size_t sh_len,
                            const uint8_t *master_secret,
                            uint8_t *verify)
{
    /*
     * transcript = client_hello || server_hello
     * verify_data = HMAC-SHA256(master_secret, transcript)
     */
    size_t total = ch_len + sh_len;
    uint8_t *transcript = (uint8_t *)malloc(total);
    if (!transcript) {
        syslog(LOG_ERR, "handshake: transcript alloc failed");
        return -1;
    }

    memcpy(transcript, client_hello, ch_len);
    memcpy(transcript + ch_len, server_hello, sh_len);

    int ret = rc_hmac_sha256(master_secret, 32,
                             transcript, total, verify);
    free(transcript);
    return ret;
}

/* ================================================================== */
/*  Client side                                                        */
/* ================================================================== */

/**
 * @brief State carried across the client handshake steps.
 */
typedef struct hs_client_ctx {
    uint8_t  client_privkey[32];     /**< X25519 private key           */
    uint8_t  client_pubkey[32];      /**< X25519 public key            */
    uint8_t  client_random[32];      /**< Client random nonce          */
    rc_pkt_hdr_t hello_hdr;          /**< Sent HELLO header            */
} hs_client_ctx_t;

/**
 * @brief Build and send a ClientHello packet.
 *
 * @param conn  Connection (must be in CONNECTING state).
 * @return 0 on success, -1 on failure.
 */
int rc_handshake_send_client_hello(rc_conn_t *conn)
{
    if (!conn) return -1;

    /* Generate ephemeral X25519 key pair */
    hs_client_ctx_t *ctx = (hs_client_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;

    if (rc_x25519_keygen(ctx->client_privkey, ctx->client_pubkey) != 0) {
        syslog(LOG_ERR, "handshake: X25519 keygen failed");
        free(ctx);
        return -1;
    }

    if (rc_random_bytes(ctx->client_random, 32) != 0) {
        free(ctx);
        return -1;
    }

    /* Build ClientHello payload */
    rc_client_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.conn_id = conn->conn_id;
    memcpy(hello.client_random, ctx->client_random, 32);
    memcpy(hello.client_pubkey, ctx->client_pubkey, 32);
    hello.cipher_suites[0] = RC_CS_AES_256_GCM;
    hello.cipher_suites[1] = RC_CS_CHACHA20_POLY1305;
    hello.extensions_len = 0;

    /* Build header */
    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_HELLO, conn->local_seq++, conn->conn_id);
    hdr.payload_len = sizeof(rc_client_hello_t);
    hdr.flags = RC_FLAG_RELIABLE;
    ctx->hello_hdr = hdr;

    /* Serialise into send buffer */
    uint8_t buf[RC_HEADER_SIZE + sizeof(rc_client_hello_t)];
    rc_hdr_pack(buf, &hdr);
    memcpy(buf + RC_HEADER_SIZE, &hello, sizeof(hello));

    /* Queue into send ring buffer */
    size_t written = rc_ringbuf_write(&conn->send_buf, buf, sizeof(buf));
    if (written != sizeof(buf)) {
        syslog(LOG_WARNING, "handshake: send buffer full for conn_id=%u",
               conn->conn_id);
        free(ctx);
        return -1;
    }

    /* Store context for later steps */
    conn->user_data = ctx;

    /* Transition to HANDSHAKING */
    rc_conn_set_state(conn, RC_STATE_HANDSHAKING);

    syslog(LOG_DEBUG, "handshake: ClientHello sent for conn_id=%u",
           conn->conn_id);
    return 0;
}

/* ------------------------------------------------------------------ */

/**
 * @brief Process a ServerHello and send the Finish message.
 *
 * @param conn          Connection.
 * @param server_hello  Deserialised ServerHello.
 * @return 0 on success, -1 on failure.
 */
int rc_handshake_process_server_hello(rc_conn_t *conn,
                                      const rc_server_hello_t *server_hello)
{
    if (!conn || !server_hello) return -1;

    hs_client_ctx_t *ctx = (hs_client_ctx_t *)conn->user_data;
    if (!ctx) {
        syslog(LOG_ERR, "handshake: no client context for conn_id=%u",
               conn->conn_id);
        return -1;
    }

    /* Compute shared secret */
    uint8_t shared_secret[32];
    if (rc_x25519_shared(ctx->client_privkey,
                         server_hello->server_pubkey,
                         shared_secret) != 0) {
        syslog(LOG_ERR, "handshake: X25519 shared secret failed");
        return -1;
    }

    /* Derive session keys */
    if (rc_derive_session_keys(shared_secret, 32,
                               ctx->client_random,
                               server_hello->server_random,
                               &conn->session) != 0) {
        syslog(LOG_ERR, "handshake: key derivation failed");
        return -1;
    }

    /* Build Finish message */
    rc_finish_t finish;
    memset(&finish, 0, sizeof(finish));
    finish.conn_id = conn->conn_id;

    /* Compute verify_data over the transcript */
    rc_pkt_hdr_t sh_hdr;
    rc_hdr_init(&sh_hdr, RC_PKT_HELLO_ACK, 0, conn->conn_id);
    sh_hdr.payload_len = sizeof(rc_server_hello_t);

    uint8_t sh_buf[RC_HEADER_SIZE + sizeof(rc_server_hello_t)];
    rc_hdr_pack(sh_buf, &sh_hdr);
    memcpy(sh_buf + RC_HEADER_SIZE, server_hello, sizeof(rc_server_hello_t));

    uint8_t ch_buf[RC_HEADER_SIZE + sizeof(rc_client_hello_t)];
    rc_hdr_pack(ch_buf, &ctx->hello_hdr);
    /* Reconstruct ClientHello from context */
    rc_client_hello_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.conn_id = conn->conn_id;
    memcpy(ch.client_random, ctx->client_random, 32);
    memcpy(ch.client_pubkey, ctx->client_pubkey, 32);
    ch.cipher_suites[0] = RC_CS_AES_256_GCM;
    ch.cipher_suites[1] = RC_CS_CHACHA20_POLY1305;
    memcpy(ch_buf + RC_HEADER_SIZE, &ch, sizeof(ch));

    if (build_transcript(ch_buf, sizeof(ch_buf),
                         sh_buf, sizeof(sh_buf),
                         conn->session.master_secret,
                         finish.verify_data) != 0) {
        return -1;
    }

    /* Build FIN packet */
    rc_pkt_hdr_t fin_hdr;
    rc_hdr_init(&fin_hdr, RC_PKT_FIN, conn->local_seq++, conn->conn_id);
    fin_hdr.payload_len = sizeof(rc_finish_t);
    fin_hdr.flags = RC_FLAG_RELIABLE | RC_FLAG_FIN_ACK;

    uint8_t fin_buf[RC_HEADER_SIZE + sizeof(rc_finish_t)];
    rc_hdr_pack(fin_buf, &fin_hdr);
    memcpy(fin_buf + RC_HEADER_SIZE, &finish, sizeof(finish));

    size_t written = rc_ringbuf_write(&conn->send_buf, fin_buf, sizeof(fin_buf));
    if (written != sizeof(fin_buf)) {
        syslog(LOG_WARNING, "handshake: send buffer full for FIN");
        return -1;
    }

    /* Transition to ESTABLISHED */
    rc_conn_set_state(conn, RC_STATE_ESTABLISHED);

    /* Clean up handshake context */
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
    conn->user_data = NULL;

    /* Zero the shared secret */
    memset(shared_secret, 0, sizeof(shared_secret));

    syslog(LOG_INFO, "handshake: client handshake complete for conn_id=%u",
           conn->conn_id);
    return 0;
}

/* ================================================================== */
/*  Server side                                                        */
/* ================================================================== */

/**
 * @brief Server handshake context.
 */
typedef struct hs_server_ctx {
    uint8_t  server_privkey[32];
    uint8_t  server_pubkey[32];
    uint8_t  server_random[32];
    uint8_t  client_random[32];
    uint8_t  client_pubkey[32];
    uint32_t session_id;
} hs_server_ctx_t;

/**
 * @brief Process a ClientHello and send ServerHello.
 *
 * @param conn         Connection.
 * @param client_hello Deserialised ClientHello.
 * @return 0 on success, -1 on failure.
 */
int rc_handshake_process_client_hello(rc_conn_t *conn,
                                      const rc_client_hello_t *client_hello)
{
    if (!conn || !client_hello) return -1;

    hs_server_ctx_t *ctx = (hs_server_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;

    /* Generate server ephemeral keys */
    if (rc_x25519_keygen(ctx->server_privkey, ctx->server_pubkey) != 0) {
        free(ctx);
        return -1;
    }
    if (rc_random_bytes(ctx->server_random, 32) != 0) {
        free(ctx);
        return -1;
    }

    memcpy(ctx->client_random, client_hello->client_random, 32);
    memcpy(ctx->client_pubkey, client_hello->client_pubkey, 32);

    /* Select cipher suite (prefer client's first choice) */
    rc_aead_algo_t algo = RC_AEAD_AES_256_GCM;
    for (int i = 0; i < 4; i++) {
        if (client_hello->cipher_suites[i] == RC_CS_CHACHA20_POLY1305) {
            algo = RC_AEAD_CHACHA20_POLY1305;
            break;
        }
        if (client_hello->cipher_suites[i] == RC_CS_AES_256_GCM) {
            algo = RC_AEAD_AES_256_GCM;
            break;
        }
    }
    conn->session.algo = algo;

    /* Generate session ticket */
    uint32_t session_id;
    if (rc_random_bytes((uint8_t *)&session_id, 4) != 0) {
        free(ctx);
        return -1;
    }
    ctx->session_id = session_id;

    /* Build ServerHello */
    rc_server_hello_t shello;
    memset(&shello, 0, sizeof(shello));
    shello.conn_id = conn->conn_id;
    memcpy(shello.server_random, ctx->server_random, 32);
    memcpy(shello.server_pubkey, ctx->server_pubkey, 32);
    shello.selected_cipher = (uint16_t)algo;
    shello.session_id = session_id;
    shello.extensions_len = 0;

    /* Compute shared secret and derive keys now (server side) */
    uint8_t shared_secret[32];
    if (rc_x25519_shared(ctx->server_privkey,
                         ctx->client_pubkey,
                         shared_secret) != 0) {
        free(ctx);
        return -1;
    }

    if (rc_derive_session_keys(shared_secret, 32,
                               ctx->client_random,
                               ctx->server_random,
                               &conn->session) != 0) {
        free(ctx);
        return -1;
    }
    memset(shared_secret, 0, sizeof(shared_secret));

    /* Build and queue ServerHello packet */
    rc_pkt_hdr_t sh_hdr;
    rc_hdr_init(&sh_hdr, RC_PKT_HELLO_ACK, conn->local_seq++, conn->conn_id);
    sh_hdr.payload_len = sizeof(rc_server_hello_t);
    sh_hdr.flags = RC_FLAG_RELIABLE;

    uint8_t buf[RC_HEADER_SIZE + sizeof(rc_server_hello_t)];
    rc_hdr_pack(buf, &sh_hdr);
    memcpy(buf + RC_HEADER_SIZE, &shello, sizeof(shello));

    size_t written = rc_ringbuf_write(&conn->send_buf, buf, sizeof(buf));
    if (written != sizeof(buf)) {
        free(ctx);
        return -1;
    }

    conn->user_data = ctx;

    syslog(LOG_INFO, "handshake: ServerHello sent for conn_id=%u cipher=%u",
           conn->conn_id, algo);
    return 0;
}

/* ------------------------------------------------------------------ */

/**
 * @brief Process the Finish message on the server side.
 *
 * Verifies the client's HMAC transcript to confirm key possession.
 *
 * @param conn    Connection.
 * @param finish  Deserialised Finish message.
 * @return 0 on success, -1 on verification failure.
 */
int rc_handshake_process_finish(rc_conn_t *conn, const rc_finish_t *finish)
{
    if (!conn || !finish) return -1;

    hs_server_ctx_t *ctx = (hs_server_ctx_t *)conn->user_data;
    if (!ctx) return -1;

    /*
     * Reconstruct the expected verify_data from the transcript.
     * We need the original ClientHello and ServerHello bytes.
     */
    rc_client_hello_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.conn_id = conn->conn_id;
    memcpy(ch.client_random, ctx->client_random, 32);
    memcpy(ch.client_pubkey, ctx->client_pubkey, 32);

    rc_pkt_hdr_t ch_hdr;
    rc_hdr_init(&ch_hdr, RC_PKT_HELLO, 0, conn->conn_id);
    ch_hdr.payload_len = sizeof(ch);

    uint8_t ch_buf[RC_HEADER_SIZE + sizeof(ch)];
    rc_hdr_pack(ch_buf, &ch_hdr);
    memcpy(ch_buf + RC_HEADER_SIZE, &ch, sizeof(ch));

    rc_server_hello_t sh;
    memset(&sh, 0, sizeof(sh));
    sh.conn_id = conn->conn_id;
    memcpy(sh.server_random, ctx->server_random, 32);
    memcpy(sh.server_pubkey, ctx->server_pubkey, 32);
    sh.selected_cipher = (uint16_t)conn->session.algo;
    sh.session_id = ctx->session_id;

    rc_pkt_hdr_t sh_hdr;
    rc_hdr_init(&sh_hdr, RC_PKT_HELLO_ACK, 0, conn->conn_id);
    sh_hdr.payload_len = sizeof(sh);

    uint8_t sh_buf[RC_HEADER_SIZE + sizeof(sh)];
    rc_hdr_pack(sh_buf, &sh_hdr);
    memcpy(sh_buf + RC_HEADER_SIZE, &sh, sizeof(sh));

    uint8_t expected[32];
    if (build_transcript(ch_buf, sizeof(ch_buf),
                         sh_buf, sizeof(sh_buf),
                         conn->session.master_secret,
                         expected) != 0) {
        return -1;
    }

    /* Constant-time comparison to prevent timing attacks */
    volatile uint8_t diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= expected[i] ^ finish->verify_data[i];
    }

    if (diff != 0) {
        syslog(LOG_WARNING, "handshake: verify_data mismatch for conn_id=%u",
               conn->conn_id);
        return -1;
    }

    /* Transition to ESTABLISHED */
    rc_conn_set_state(conn, RC_STATE_ESTABLISHED);

    /* Clean up */
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
    conn->user_data = NULL;

    syslog(LOG_INFO, "handshake: server handshake complete for conn_id=%u",
           conn->conn_id);
    return 0;
}

/* ================================================================== */
/*  Session resumption                                                 */
/* ================================================================== */

/**
 * @brief Attempt session resumption using a cached session ticket.
 *
 * If the session_id matches a stored ticket, derive keys directly
 * from the stored master secret without a full ECDHE exchange.
 *
 * @param conn        Connection.
 * @param session_id  Session ticket from ClientHello or ServerHello.
 * @return 0 on successful resumption, -1 if ticket not found.
 */
int rc_handshake_resume(rc_conn_t *conn, uint32_t session_id)
{
    if (!conn || session_id == 0) return -1;

    /*
     * In a production implementation, this would look up the session
     * ticket in a shared-memory cache (e.g., Redis, or a lock-free
     * hash table).  For now, we log the attempt and return failure
     * to force a full handshake.
     */
    syslog(LOG_DEBUG, "handshake: session resume attempt for ticket=%u "
           "(not cached, falling back to full handshake)", session_id);

    conn->session.is_resumed = false;
    conn->session.session_id = session_id;
    return -1;
}
