#include <stdbool.h>
/**
 * @file rc_crypto.c
 * @brief Cryptographic implementation using OpenSSL 3.x.
 *
 * Provides AES-256-GCM, ChaCha20-Poly1305, HKDF-SHA256, X25519,
 * HMAC-SHA256, and record-layer encrypt/decrypt.
 */

#include "rc_crypto.h"

#include <string.h>
#include <syslog.h>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/core_names.h>
#include <openssl/params.h>

/* ================================================================== */
/*  Random bytes                                                       */
/* ================================================================== */

int rc_random_bytes(uint8_t *buf, size_t len)
{
    if (RAND_bytes(buf, (int)len) != 1) {
        syslog(LOG_ERR, "rc_crypto: RAND_bytes failed");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/*  Nonce derivation                                                   */
/* ================================================================== */

void rc_derive_nonce(uint8_t *nonce, const uint8_t *salt, uint64_t counter)
{
    /* nonce = salt[0..3] || counter_be64 */
    memcpy(nonce, salt, 4);
    for (int i = 11; i >= 4; i--) {
        nonce[i] = (uint8_t)(counter & 0xFF);
        counter >>= 8;
    }
}

/* ================================================================== */
/*  AEAD context                                                       */
/* ================================================================== */

int rc_aead_init(rc_aead_ctx_t *ctx, rc_aead_algo_t algo, const uint8_t *key)
{
    if (!ctx || !key) return -1;

    ctx->algo = algo;
    ctx->nonce_counter = 0;

    switch (algo) {
    case RC_AEAD_AES_256_GCM:
        ctx->key_len = RC_AES256_KEY_LEN;
        break;
    case RC_AEAD_CHACHA20_POLY1305:
        ctx->key_len = RC_CHACHA_KEY_LEN;
        break;
    default:
        syslog(LOG_ERR, "rc_crypto: unknown AEAD algorithm %d", algo);
        return -1;
    }

    memcpy(ctx->key, key, ctx->key_len);

    /* Generate a random nonce salt */
    if (rc_random_bytes(ctx->nonce_salt, sizeof(ctx->nonce_salt)) != 0) {
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */

int rc_aead_encrypt(rc_aead_ctx_t *ctx, const uint8_t *nonce,
                    const uint8_t *plaintext, size_t pt_len,
                    const uint8_t *aad, size_t aad_len,
                    uint8_t *ciphertext, size_t *ct_len)
{
    if (!ctx || !nonce || !plaintext || !ciphertext || !ct_len) return -1;

    const EVP_CIPHER *cipher = NULL;
    switch (ctx->algo) {
    case RC_AEAD_AES_256_GCM:
        cipher = EVP_aes_256_gcm();
        break;
    case RC_AEAD_CHACHA20_POLY1305:
        cipher = EVP_chacha20_poly1305();
        break;
    default:
        return -1;
    }

    EVP_CIPHER_CTX *evp_ctx = EVP_CIPHER_CTX_new();
    if (!evp_ctx) {
        syslog(LOG_ERR, "rc_crypto: EVP_CIPHER_CTX_new failed");
        return -1;
    }

    int ret = -1;
    int out_len = 0;
    int total_len = 0;

    /* Initialise encryption */
    if (EVP_EncryptInit_ex(evp_ctx, cipher, NULL, NULL, NULL) != 1)
        goto cleanup;

    /* Set IV length (12 bytes) */
    if (EVP_CIPHER_CTX_ctrl(evp_ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1)
        goto cleanup;

    /* Set key and IV */
    if (EVP_EncryptInit_ex(evp_ctx, NULL, NULL, ctx->key, nonce) != 1)
        goto cleanup;

    /* Process AAD if provided */
    if (aad && aad_len > 0) {
        if (EVP_EncryptUpdate(evp_ctx, NULL, &out_len, aad, (int)aad_len) != 1)
            goto cleanup;
    }

    /* Encrypt plaintext */
    if (EVP_EncryptUpdate(evp_ctx, ciphertext, &out_len,
                          plaintext, (int)pt_len) != 1)
        goto cleanup;
    total_len = out_len;

    /* Finalise */
    if (EVP_EncryptFinal_ex(evp_ctx, ciphertext + total_len, &out_len) != 1)
        goto cleanup;
    total_len += out_len;

    /* Append authentication tag */
    if (EVP_CIPHER_CTX_ctrl(evp_ctx, EVP_CTRL_GCM_GET_TAG,
                            RC_MAX_TAG_LEN, ciphertext + total_len) != 1)
        goto cleanup;
    total_len += RC_MAX_TAG_LEN;

    *ct_len = (size_t)total_len;
    ret = 0;

cleanup:
    EVP_CIPHER_CTX_free(evp_ctx);
    if (ret != 0) {
        syslog(LOG_ERR, "rc_crypto: AEAD encrypt failed");
    }
    return ret;
}

/* ------------------------------------------------------------------ */

int rc_aead_decrypt(rc_aead_ctx_t *ctx, const uint8_t *nonce,
                    const uint8_t *ciphertext, size_t ct_len,
                    const uint8_t *aad, size_t aad_len,
                    uint8_t *plaintext, size_t *pt_len)
{
    if (!ctx || !nonce || !ciphertext || !plaintext || !pt_len) return -1;
    if (ct_len < RC_MAX_TAG_LEN) return -1;

    const EVP_CIPHER *cipher = NULL;
    switch (ctx->algo) {
    case RC_AEAD_AES_256_GCM:
        cipher = EVP_aes_256_gcm();
        break;
    case RC_AEAD_CHACHA20_POLY1305:
        cipher = EVP_chacha20_poly1305();
        break;
    default:
        return -1;
    }

    size_t enc_len = ct_len - RC_MAX_TAG_LEN;

    EVP_CIPHER_CTX *evp_ctx = EVP_CIPHER_CTX_new();
    if (!evp_ctx) return -1;

    int ret = -1;
    int out_len = 0;
    int total_len = 0;

    if (EVP_DecryptInit_ex(evp_ctx, cipher, NULL, NULL, NULL) != 1)
        goto cleanup;

    if (EVP_CIPHER_CTX_ctrl(evp_ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1)
        goto cleanup;

    if (EVP_DecryptInit_ex(evp_ctx, NULL, NULL, ctx->key, nonce) != 1)
        goto cleanup;

    /* Process AAD */
    if (aad && aad_len > 0) {
        if (EVP_DecryptUpdate(evp_ctx, NULL, &out_len, aad, (int)aad_len) != 1)
            goto cleanup;
    }

    /* Decrypt ciphertext */
    if (EVP_DecryptUpdate(evp_ctx, plaintext, &out_len,
                          ciphertext, (int)enc_len) != 1)
        goto cleanup;
    total_len = out_len;

    /* Set expected tag */
    if (EVP_CIPHER_CTX_ctrl(evp_ctx, EVP_CTRL_GCM_SET_TAG,
                            RC_MAX_TAG_LEN,
                            (void *)(ciphertext + enc_len)) != 1)
        goto cleanup;

    /* Finalise and verify tag */
    if (EVP_DecryptFinal_ex(evp_ctx, plaintext + total_len, &out_len) != 1) {
        syslog(LOG_WARNING, "rc_crypto: AEAD authentication failed");
        goto cleanup;
    }
    total_len += out_len;

    *pt_len = (size_t)total_len;
    ret = 0;

cleanup:
    EVP_CIPHER_CTX_free(evp_ctx);
    return ret;
}

/* ================================================================== */
/*  HKDF-SHA256                                                        */
/* ================================================================== */

int rc_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                   const uint8_t *salt, size_t salt_len,
                   const uint8_t *info, size_t info_len,
                   uint8_t *okm, size_t okm_len)
{
    if (!ikm || !okm) return -1;
    if (okm_len > 255 * 32) return -1;  /* HKDF limit */

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    if (!ctx) return -1;

    int ret = -1;

    if (EVP_PKEY_derive_init(ctx) != 1) goto cleanup;
    if (EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) != 1) goto cleanup;

    if (salt && salt_len > 0) {
        if (EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, (int)salt_len) != 1)
            goto cleanup;
    }

    if (EVP_PKEY_CTX_set1_hkdf_key(ctx, ikm, (int)ikm_len) != 1)
        goto cleanup;

    if (info && info_len > 0) {
        if (EVP_PKEY_CTX_add1_hkdf_info(ctx, info, (int)info_len) != 1)
            goto cleanup;
    }

    if (EVP_PKEY_derive(ctx, okm, &okm_len) != 1) goto cleanup;
    ret = 0;

cleanup:
    EVP_PKEY_CTX_free(ctx);
    if (ret != 0) {
        syslog(LOG_ERR, "rc_crypto: HKDF-SHA256 failed");
    }
    return ret;
}

/* ================================================================== */
/*  Session key derivation                                             */
/* ================================================================== */

int rc_derive_session_keys(const uint8_t *shared_secret, size_t ss_len,
                           const uint8_t *client_random,
                           const uint8_t *server_random,
                           rc_session_t *session)
{
    if (!shared_secret || !client_random || !server_random || !session)
        return -1;

    /*
     * Derive 64 bytes of key material:
     *   OKM = HKDF-SHA256(
     *       IKM  = shared_secret,
     *       salt = client_random || server_random,
     *       info = "RoboControl v1 key expansion"
     *   )
     *
     * Split into:
     *   [0..31]  client_write_key  (32 bytes)
     *   [32..63] server_write_key  (32 bytes)
     *
     * IVs are derived separately from the master secret.
     */
    uint8_t salt[64];
    memcpy(salt, client_random, 32);
    memcpy(salt + 32, server_random, 32);

    const char *info = "RoboControl v1 key expansion";
    uint8_t okm[RC_HKDF_OKM_LEN];

    if (rc_hkdf_sha256(shared_secret, ss_len,
                       salt, sizeof(salt),
                       (const uint8_t *)info, strlen(info),
                       okm, sizeof(okm)) != 0) {
        return -1;
    }

    /* Store master secret for key rotation */
    memcpy(session->master_secret, okm, sizeof(session->master_secret));

    /*
     * Derive IVs: HKDF with different info string.
     */
    const char *iv_info = "RoboControl v1 iv expansion";
    uint8_t iv_material[24]; /* 12 bytes client IV + 12 bytes server IV */

    if (rc_hkdf_sha256(okm, sizeof(okm),
                       salt, sizeof(salt),
                       (const uint8_t *)iv_info, strlen(iv_info),
                       iv_material, sizeof(iv_material)) != 0) {
        return -1;
    }

    /* Initialise AEAD contexts:
     * client_write_key → encrypt_ctx (client sends)
     * server_write_key → decrypt_ctx (client receives)
     *
     * For server-side the roles are swapped at connection setup. */
    rc_aead_init(&session->encrypt_ctx, session->algo, okm);
    rc_aead_init(&session->decrypt_ctx, session->algo, okm + 32);

    memcpy(session->encrypt_ctx.nonce_salt, iv_material, 12);
    memcpy(session->decrypt_ctx.nonce_salt, iv_material + 12, 12);

    session->encrypt_seq = 0;
    session->decrypt_seq = 0;
    session->handshake_done = true;

    /* Zero sensitive intermediate material */
    memset(okm, 0, sizeof(okm));
    memset(iv_material, 0, sizeof(iv_material));

    return 0;
}

/* ================================================================== */
/*  Session management                                                 */
/* ================================================================== */

int rc_session_init(rc_session_t *session, rc_aead_algo_t algo)
{
    if (!session) return -1;
    memset(session, 0, sizeof(*session));
    session->algo = algo;
    return 0;
}

/* ------------------------------------------------------------------ */

void rc_session_destroy(rc_session_t *session)
{
    if (!session) return;

    /* Explicitly zero all sensitive material */
    memset(session->master_secret, 0, sizeof(session->master_secret));
    memset(session->encrypt_ctx.key, 0, sizeof(session->encrypt_ctx.key));
    memset(session->decrypt_ctx.key, 0, sizeof(session->decrypt_ctx.key));
    memset(session, 0, sizeof(*session));
}

/* ------------------------------------------------------------------ */

int rc_session_rotate_keys(rc_session_t *session)
{
    if (!session || !session->handshake_done) return -1;

    /*
     * Derive fresh keys from master_secret using a new salt
     * that includes the current encryption sequence number.
     */
    uint8_t new_salt[40];
    memcpy(new_salt, session->master_secret, 32);
    /* Append current sequence as big-endian 8 bytes */
    uint64_t seq = session->encrypt_seq;
    for (int i = 39; i >= 32; i--) {
        new_salt[i] = (uint8_t)(seq & 0xFF);
        seq >>= 8;
    }

    const char *info = "RoboControl v1 key rotation";
    uint8_t okm[64];

    if (rc_hkdf_sha256(session->master_secret, 32,
                       new_salt, sizeof(new_salt),
                       (const uint8_t *)info, strlen(info),
                       okm, sizeof(okm)) != 0) {
        return -1;
    }

    /* Update AEAD contexts with new keys */
    memcpy(session->encrypt_ctx.key, okm, 32);
    memcpy(session->decrypt_ctx.key, okm + 32, 32);

    /* Generate new nonce salts */
    rc_random_bytes(session->encrypt_ctx.nonce_salt, 12);
    rc_random_bytes(session->decrypt_ctx.nonce_salt, 12);

    /* Reset sequence numbers */
    session->encrypt_seq = 0;
    session->decrypt_seq = 0;

    memset(okm, 0, sizeof(okm));
    syslog(LOG_INFO, "rc_crypto: session keys rotated");
    return 0;
}

/* ================================================================== */
/*  HMAC-SHA256                                                        */
/* ================================================================== */

int rc_hmac_sha256(const uint8_t *key, size_t key_len,
                   const uint8_t *data, size_t data_len,
                   uint8_t *mac)
{
    unsigned int mac_len = RC_HMAC_SHA256_LEN;
    if (HMAC(EVP_sha256(), key, (int)key_len,
             data, data_len, mac, &mac_len) == NULL) {
        syslog(LOG_ERR, "rc_crypto: HMAC-SHA256 failed");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/*  X25519 key exchange                                                */
/* ================================================================== */

int rc_x25519_keygen(uint8_t *privkey, uint8_t *pubkey)
{
    if (!privkey || !pubkey) return -1;

    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (!pctx) return -1;

    int ret = -1;
    size_t priv_len = RC_X25519_PRIVKEY_LEN;
    size_t pub_len = RC_X25519_PUBKEY_LEN;

    if (EVP_PKEY_keygen_init(pctx) != 1) goto cleanup;
    if (EVP_PKEY_keygen(pctx, &pkey) != 1) goto cleanup;

    /* Extract raw private key */
    if (EVP_PKEY_get_raw_private_key(pkey, privkey, &priv_len) != 1)
        goto cleanup;

    /* Extract raw public key */
    if (EVP_PKEY_get_raw_public_key(pkey, pubkey, &pub_len) != 1)
        goto cleanup;

    ret = 0;

cleanup:
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(pctx);
    return ret;
}

/* ------------------------------------------------------------------ */

int rc_x25519_shared(const uint8_t *privkey, const uint8_t *peer_pubkey,
                     uint8_t *secret)
{
    if (!privkey || !peer_pubkey || !secret) return -1;

    int ret = -1;
    size_t secret_len = RC_X25519_SHARED_LEN;
    EVP_PKEY *priv_pkey = NULL;
    EVP_PKEY *peer_pkey = NULL;
    EVP_PKEY_CTX *dctx = NULL;

    /* Build our private key object */
    priv_pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_X25519, NULL, privkey, RC_X25519_PRIVKEY_LEN);
    if (!priv_pkey) return -1;

    /* Build peer's public key object */
    peer_pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, NULL, peer_pubkey, RC_X25519_PUBKEY_LEN);
    if (!peer_pkey) goto cleanup;

    /* Derive shared secret */
    dctx = EVP_PKEY_CTX_new(priv_pkey, NULL);
    if (!dctx) goto cleanup2;

    if (EVP_PKEY_derive_init(dctx) != 1) goto cleanup3;
    if (EVP_PKEY_derive_set_peer(dctx, peer_pkey) != 1) goto cleanup3;

    if (EVP_PKEY_derive(dctx, secret, &secret_len) != 1) goto cleanup3;

    ret = 0;

cleanup3:
    EVP_PKEY_CTX_free(dctx);
cleanup2:
    EVP_PKEY_free(peer_pkey);
cleanup:
    EVP_PKEY_free(priv_pkey);
    return ret;
}

/* ================================================================== */
/*  Record-layer encrypt / decrypt                                     */
/* ================================================================== */

int rc_record_encrypt(rc_session_t *session, const rc_pkt_hdr_t *hdr,
                      uint8_t *payload, size_t *len)
{
    if (!session || !hdr || !payload || !len) return -1;
    if (!session->handshake_done) return -1;

    /* Derive nonce from session salt + monotonic counter */
    uint8_t nonce[12];
    rc_derive_nonce(nonce, session->encrypt_ctx.nonce_salt,
                    session->encrypt_seq);
    session->encrypt_seq++;

    /* Use serialised header as AAD (binds payload to header fields) */
    uint8_t hdr_buf[RC_HEADER_SIZE];
    rc_hdr_pack(hdr_buf, hdr);

    /* Encrypt in-place: caller must have room for tag appended */
    size_t ct_len = 0;
    if (rc_aead_encrypt(&session->encrypt_ctx, nonce,
                        payload, *len,
                        hdr_buf, RC_HEADER_SIZE,
                        payload, &ct_len) != 0) {
        return -1;
    }

    *len = ct_len;
    return 0;
}

/* ------------------------------------------------------------------ */

int rc_record_decrypt(rc_session_t *session, const rc_pkt_hdr_t *hdr,
                      uint8_t *payload, size_t *len)
{
    if (!session || !hdr || !payload || !len) return -1;
    if (!session->handshake_done) return -1;
    if (*len < RC_MAX_TAG_LEN) return -1;

    uint8_t nonce[12];
    rc_derive_nonce(nonce, session->decrypt_ctx.nonce_salt,
                    session->decrypt_seq);
    session->decrypt_seq++;

    uint8_t hdr_buf[RC_HEADER_SIZE];
    rc_hdr_pack(hdr_buf, hdr);

    size_t pt_len = 0;
    if (rc_aead_decrypt(&session->decrypt_ctx, nonce,
                        payload, *len,
                        hdr_buf, RC_HEADER_SIZE,
                        payload, &pt_len) != 0) {
        return -1;
    }

    *len = pt_len;
    return 0;
}
