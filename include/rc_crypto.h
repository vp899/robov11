/**
 * @file rc_crypto.h
 * @brief Cryptographic primitives for RoboControl.
 *
 * Provides AEAD encryption (AES-256-GCM and ChaCha20-Poly1305),
 * HKDF-SHA256 key derivation, session key management, and DTLS-like
 * record-layer encryption/decryption.
 *
 * All crypto operations go through OpenSSL 3.x.
 */

#ifndef RC_CRYPTO_H
#define RC_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "rc_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

/** AES-256-GCM key size in bytes. */
#define RC_AES256_KEY_LEN      32

/** AES-256-GCM nonce (IV) size in bytes. */
#define RC_AES256_NONCE_LEN    12

/** AES-256-GCM authentication tag size in bytes. */
#define RC_AES256_TAG_LEN      16

/** ChaCha20-Poly1305 key size in bytes. */
#define RC_CHACHA_KEY_LEN      32

/** ChaCha20-Poly1305 nonce size in bytes. */
#define RC_CHACHA_NONCE_LEN    12

/** ChaCha20-Poly1305 tag size in bytes. */
#define RC_CHACHA_TAG_LEN      16

/** HKDF-SHA256 output key material length. */
#define RC_HKDF_OKM_LEN        64

/** Maximum AEAD tag size (both algorithms use 16). */
#define RC_MAX_TAG_LEN         16

/** X25519 public key size. */
#define RC_X25519_PUBKEY_LEN   32

/** X25519 private key size. */
#define RC_X25519_PRIVKEY_LEN  32

/** Shared secret size from X25519. */
#define RC_X25519_SHARED_LEN   32

/** HMAC-SHA256 output size. */
#define RC_HMAC_SHA256_LEN     32

/* ------------------------------------------------------------------ */
/*  Cipher algorithm identifiers                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Supported AEAD algorithms.
 */
typedef enum rc_aead_algo {
    RC_AEAD_AES_256_GCM       = 1,  /**< AES-256-GCM            */
    RC_AEAD_CHACHA20_POLY1305 = 2,  /**< ChaCha20-Poly1305      */
} rc_aead_algo_t;

/* ------------------------------------------------------------------ */
/*  AEAD context                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Opaque AEAD encryption/decryption context.
 *
 * Wraps the OpenSSL EVP_CIPHER_CTX so callers do not need to
 * include OpenSSL headers.
 */
typedef struct rc_aead_ctx {
    rc_aead_algo_t algo;          /**< Algorithm identifier            */
    uint8_t        key[32];       /**< Symmetric key (max 32 bytes)    */
    uint32_t       key_len;       /**< Actual key length               */
    uint64_t       nonce_counter; /**< Monotonic nonce counter         */
    uint8_t        nonce_salt[12]; /**< Salt mixed into nonce derivation*/
} rc_aead_ctx_t;

/* ------------------------------------------------------------------ */
/*  Session (per-connection keys)                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Per-connection cryptographic session.
 *
 * Holds the derived keys, AEAD contexts for send/receive, and
 * sequence numbers for nonce derivation.
 */
typedef struct rc_session {
    rc_aead_algo_t  algo;                     /**< Negotiated algorithm */
    rc_aead_ctx_t   encrypt_ctx;              /**< Outbound encryption  */
    rc_aead_ctx_t   decrypt_ctx;              /**< Inbound decryption   */
    uint8_t         master_secret[64];        /**< HKDF-derived master  */
    uint32_t        session_id;               /**< Resumption ticket    */
    bool            is_resumed;               /**< True if resumed      */
    uint64_t        encrypt_seq;              /**< Outbound nonce seq   */
    uint64_t        decrypt_seq;              /**< Inbound nonce seq    */
    bool            handshake_done;           /**< Keys are active      */
} rc_session_t;

/* ------------------------------------------------------------------ */
/*  AEAD API                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise an AEAD context.
 * @param[out] ctx   Context to initialise.
 * @param      algo  Algorithm to use.
 * @param      key   Key material (32 bytes).
 * @return 0 on success, -1 on failure.
 */
int rc_aead_init(rc_aead_ctx_t *ctx, rc_aead_algo_t algo, const uint8_t *key);

/**
 * @brief Encrypt plaintext with AEAD.
 *
 * @param ctx         AEAD context.
 * @param nonce       Nonce (12 bytes).  Must be unique per key.
 * @param plaintext   Input plaintext.
 * @param pt_len      Plaintext length.
 * @param aad         Additional authenticated data (may be NULL).
 * @param aad_len     AAD length.
 * @param ciphertext  Output buffer (must hold pt_len + tag_len bytes).
 * @param ct_len      [out] Written ciphertext length (includes tag).
 * @return 0 on success, -1 on failure.
 */
int rc_aead_encrypt(rc_aead_ctx_t *ctx, const uint8_t *nonce,
                    const uint8_t *plaintext, size_t pt_len,
                    const uint8_t *aad, size_t aad_len,
                    uint8_t *ciphertext, size_t *ct_len);

/**
 * @brief Decrypt ciphertext with AEAD.
 *
 * @param ctx         AEAD context.
 * @param nonce       Nonce used during encryption.
 * @param ciphertext  Input ciphertext (includes appended tag).
 * @param ct_len      Ciphertext length (including tag).
 * @param aad         Additional authenticated data (may be NULL).
 * @param aad_len     AAD length.
 * @param plaintext   Output buffer (must hold ct_len - tag_len bytes).
 * @param pt_len      [out] Written plaintext length.
 * @return 0 on success, -1 on authentication failure.
 */
int rc_aead_decrypt(rc_aead_ctx_t *ctx, const uint8_t *nonce,
                    const uint8_t *ciphertext, size_t ct_len,
                    const uint8_t *aad, size_t aad_len,
                    uint8_t *plaintext, size_t *pt_len);

/* ------------------------------------------------------------------ */
/*  Key derivation (HKDF-SHA256)                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Derive key material using HKDF-SHA256 (RFC 5869).
 *
 * @param ikm        Input key material.
 * @param ikm_len    IKM length.
 * @param salt       Salt (may be NULL for default).
 * @param salt_len   Salt length.
 * @param info       Context/application info.
 * @param info_len   Info length.
 * @param okm        Output key material.
 * @param okm_len    Desired OKM length (max 255 * 32).
 * @return 0 on success, -1 on failure.
 */
int rc_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                   const uint8_t *salt, size_t salt_len,
                   const uint8_t *info, size_t info_len,
                   uint8_t *okm, size_t okm_len);

/**
 * @brief Derive session keys from a shared secret.
 *
 * Uses HKDF to produce:
 *   - client_write_key (32 bytes)
 *   - server_write_key (32 bytes)
 *   - client_write_iv  (12 bytes)
 *   - server_write_iv  (12 bytes)
 *
 * @param shared_secret  ECDH shared secret.
 * @param ss_len         Shared secret length.
 * @param client_random  Client nonce (32 bytes).
 * @param server_random  Server nonce (32 bytes).
 * @param[out] session   Session to populate with derived keys.
 * @return 0 on success, -1 on failure.
 */
int rc_derive_session_keys(const uint8_t *shared_secret, size_t ss_len,
                           const uint8_t *client_random,
                           const uint8_t *server_random,
                           rc_session_t *session);

/* ------------------------------------------------------------------ */
/*  Session management                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise a crypto session.
 * @param[out] session  Session to initialise.
 * @param      algo     Preferred AEAD algorithm.
 * @return 0 on success.
 */
int rc_session_init(rc_session_t *session, rc_aead_algo_t algo);

/**
 * @brief Destroy a crypto session (zero sensitive material).
 * @param session  Session to destroy.
 */
void rc_session_destroy(rc_session_t *session);

/**
 * @brief Rotate session keys.
 *
 * Derives fresh keys from the current master secret using a new
 * salt derived from the current encryption sequence number.
 *
 * @param session  Session to rotate.
 * @return 0 on success, -1 on failure.
 */
int rc_session_rotate_keys(rc_session_t *session);

/* ------------------------------------------------------------------ */
/*  Nonce generation                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Generate cryptographically secure random bytes.
 * @param[out] buf  Output buffer.
 * @param      len  Number of bytes.
 * @return 0 on success, -1 on failure.
 */
int rc_random_bytes(uint8_t *buf, size_t len);

/**
 * @brief Derive a nonce from a base salt and a monotonic counter.
 *
 * Nonce = salt[0..3] || counter (big-endian 8 bytes).
 * Total: 12 bytes (suitable for both AES-GCM and ChaCha20).
 *
 * @param[out] nonce    Output nonce (12 bytes).
 * @param      salt     Base salt (at least 4 bytes).
 * @param      counter  Monotonic counter.
 */
void rc_derive_nonce(uint8_t *nonce, const uint8_t *salt, uint64_t counter);

/* ------------------------------------------------------------------ */
/*  HMAC-SHA256                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Compute HMAC-SHA256.
 * @param key      HMAC key.
 * @param key_len  Key length.
 * @param data     Input data.
 * @param data_len Data length.
 * @param[out] mac Output MAC (32 bytes).
 * @return 0 on success, -1 on failure.
 */
int rc_hmac_sha256(const uint8_t *key, size_t key_len,
                   const uint8_t *data, size_t data_len,
                   uint8_t *mac);

/* ------------------------------------------------------------------ */
/*  X25519 key exchange                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Generate an X25519 key pair.
 * @param[out] privkey  Private key (32 bytes).
 * @param[out] pubkey   Public key (32 bytes).
 * @return 0 on success, -1 on failure.
 */
int rc_x25519_keygen(uint8_t *privkey, uint8_t *pubkey);

/**
 * @brief Compute X25519 shared secret.
 * @param privkey       Our private key (32 bytes).
 * @param peer_pubkey   Peer's public key (32 bytes).
 * @param[out] secret   Shared secret (32 bytes).
 * @return 0 on success, -1 on failure.
 */
int rc_x25519_shared(const uint8_t *privkey, const uint8_t *peer_pubkey,
                     uint8_t *secret);

/**
 * @brief Record-layer encrypt a packet payload.
 *
 * Encrypts the payload in-place, appending the AEAD tag.  The caller
 * must ensure the buffer has at least pt_len + RC_MAX_TAG_LEN bytes.
 *
 * @param session    Crypto session.
 * @param hdr        Packet header (used as AAD).
 * @param payload    Payload to encrypt (in/out).
 * @param[in,out] len  In: plaintext length. Out: ciphertext + tag length.
 * @return 0 on success, -1 on failure.
 */
int rc_record_encrypt(rc_session_t *session, const rc_pkt_hdr_t *hdr,
                      uint8_t *payload, size_t *len);

/**
 * @brief Record-layer decrypt a packet payload.
 *
 * @param session    Crypto session.
 * @param hdr        Packet header (used as AAD).
 * @param payload    Ciphertext with appended tag.
 * @param[in,out] len  In: ciphertext + tag length. Out: plaintext length.
 * @return 0 on success, -1 on auth failure.
 */
int rc_record_decrypt(rc_session_t *session, const rc_pkt_hdr_t *hdr,
                      uint8_t *payload, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* RC_CRYPTO_H */
