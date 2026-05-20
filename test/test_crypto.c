/**
 * @file test_crypto.c
 * @brief Unit tests for crypto (rc_crypto)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rc_crypto.h"

static void test_encrypt_decrypt_aes_gcm(void)
{
    uint8_t key[32];
    memset(key, 0x42, 32);

    rc_aead_ctx_t ctx;
    rc_aead_init(&ctx, RC_AEAD_AES_256_GCM, key);

    const uint8_t plaintext[] = "Hello from robot camera 1! Frame data here.";
    size_t pt_len = sizeof(plaintext) - 1;
    uint8_t ciphertext[256 + RC_MAX_TAG_LEN];
    size_t ct_len = 0;
    uint8_t nonce[RC_AES256_NONCE_LEN];
    memset(nonce, 0x11, sizeof(nonce));

    assert(rc_aead_encrypt(&ctx, nonce, plaintext, pt_len, NULL, 0, ciphertext, &ct_len) == 0);
    assert(ct_len == pt_len + RC_AES256_TAG_LEN);

    rc_aead_ctx_t dctx;
    rc_aead_init(&dctx, RC_AEAD_AES_256_GCM, key);
    uint8_t decrypted[256];
    size_t dec_len = 0;
    assert(rc_aead_decrypt(&dctx, nonce, ciphertext, ct_len, NULL, 0, decrypted, &dec_len) == 0);
    assert(dec_len == pt_len);
    assert(memcmp(decrypted, plaintext, pt_len) == 0);

    printf("  PASS: test_encrypt_decrypt_aes_gcm\n");
}

static void test_encrypt_decrypt_chacha20(void)
{
    uint8_t key[32];
    memset(key, 0x53, 32);

    rc_aead_ctx_t ctx;
    rc_aead_init(&ctx, RC_AEAD_CHACHA20_POLY1305, key);

    const uint8_t plaintext[] = "ChaCha20-Poly1305 test with robot control data.";
    size_t pt_len = sizeof(plaintext) - 1;
    uint8_t ciphertext[256 + RC_MAX_TAG_LEN];
    size_t ct_len = 0;
    uint8_t nonce[RC_CHACHA_NONCE_LEN];
    memset(nonce, 0x22, sizeof(nonce));

    assert(rc_aead_encrypt(&ctx, nonce, plaintext, pt_len, NULL, 0, ciphertext, &ct_len) == 0);

    rc_aead_ctx_t dctx;
    rc_aead_init(&dctx, RC_AEAD_CHACHA20_POLY1305, key);
    uint8_t decrypted[256];
    size_t dec_len = 0;
    assert(rc_aead_decrypt(&dctx, nonce, ciphertext, ct_len, NULL, 0, decrypted, &dec_len) == 0);
    assert(dec_len == pt_len);
    assert(memcmp(decrypted, plaintext, pt_len) == 0);

    printf("  PASS: test_encrypt_decrypt_chacha20\n");
}

static void test_tampered_ciphertext(void)
{
    uint8_t key[32];
    memset(key, 0x42, 32);

    rc_aead_ctx_t ctx;
    rc_aead_init(&ctx, RC_AEAD_AES_256_GCM, key);

    const uint8_t plaintext[] = "Tamper test";
    uint8_t ciphertext[64 + RC_MAX_TAG_LEN];
    size_t ct_len = 0;
    uint8_t nonce[RC_AES256_NONCE_LEN];
    memset(nonce, 0x33, sizeof(nonce));

    assert(rc_aead_encrypt(&ctx, nonce, plaintext, 11, NULL, 0, ciphertext, &ct_len) == 0);

    ciphertext[0] ^= 0xFF; /* tamper */

    rc_aead_ctx_t dctx;
    rc_aead_init(&dctx, RC_AEAD_AES_256_GCM, key);
    uint8_t decrypted[64];
    size_t dec_len = 0;
    assert(rc_aead_decrypt(&dctx, nonce, ciphertext, ct_len, NULL, 0, decrypted, &dec_len) != 0);

    printf("  PASS: test_tampered_ciphertext\n");
}

static void test_key_derivation(void)
{
    uint8_t shared_secret[32], cr[32], sr[32];
    memset(shared_secret, 0xAA, 32);
    memset(cr, 0x11, 32);
    memset(sr, 0x22, 32);

    rc_session_t session;
    rc_session_init(&session, RC_AEAD_AES_256_GCM);

    assert(rc_derive_session_keys(shared_secret, 32, cr, sr, &session) == 0);

    /* Keys should be non-zero */
    int all_zero = 1;
    for (int i = 0; i < 32; i++) {
        if (session.encrypt_ctx.key[i] != 0) { all_zero = 0; break; }
    }
    assert(!all_zero);

    rc_session_destroy(&session);
    printf("  PASS: test_key_derivation\n");
}

static void test_key_rotation(void)
{
    rc_session_t session;
    rc_session_init(&session, RC_AEAD_AES_256_GCM);

    uint8_t ss[32], cr[32], sr[32];
    memset(ss, 0x11, 32); memset(cr, 0x22, 32); memset(sr, 0x33, 32);
    rc_derive_session_keys(ss, 32, cr, sr, &session);

    uint8_t old_key[32];
    memcpy(old_key, session.encrypt_ctx.key, 32);

    assert(rc_session_rotate_keys(&session) == 0);
    assert(memcmp(session.encrypt_ctx.key, old_key, 32) != 0);

    rc_session_destroy(&session);
    printf("  PASS: test_key_rotation\n");
}

int main(void)
{
    printf("Running test_crypto...\n");
    test_encrypt_decrypt_aes_gcm();
    test_encrypt_decrypt_chacha20();
    test_tampered_ciphertext();
    test_key_derivation();
    test_key_rotation();
    printf("test_crypto: ALL PASSED\n");
    return 0;
}
