/*
 * robocontrol/crypto.h - Lightweight crypto primitives
 * HMAC-SHA256, AES-256-CTR, key derivation, key rotation
 * Self-contained implementation (no OpenSSL dependency)
 */
#ifndef ROBOCONTROL_CRYPTO_H
#define ROBOCONTROL_CRYPTO_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RC_CRYPTO_KEY_SIZE   32  /* AES-256 */
#define RC_CRYPTO_IV_SIZE    16
#define RC_CRYPTO_HMAC_SIZE  32
#define RC_CRYPTO_SALT_SIZE  16
#define RC_CRYPTO_NONCE_SIZE 12

/* ── SHA-256 ────────────────────────────────────────────────── */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
} rc_sha256_ctx_t;

static inline uint32_t rc_rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static inline void rc_sha256_transform(rc_sha256_ctx_t *ctx, const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rc_rotr32(w[i-15], 7) ^ rc_rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rc_rotr32(w[i-2], 17) ^ rc_rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = rc_rotr32(e, 6) ^ rc_rotr32(e, 11) ^ rc_rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = rc_rotr32(a, 2) ^ rc_rotr32(a, 13) ^ rc_rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static inline void rc_sha256_init(rc_sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
    memset(ctx->buffer, 0, 64);
}

static inline void rc_sha256_update(rc_sha256_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;

    if (idx > 0) {
        size_t fill = 64 - idx;
        if (len >= fill) {
            memcpy(ctx->buffer + idx, p, fill);
            rc_sha256_transform(ctx, ctx->buffer);
            p += fill; len -= fill; idx = 0;
        } else {
            memcpy(ctx->buffer + idx, p, len);
            return;
        }
    }
    while (len >= 64) {
        rc_sha256_transform(ctx, p);
        p += 64; len -= 64;
    }
    if (len > 0) memcpy(ctx->buffer, p, len);
}

static inline void rc_sha256_final(rc_sha256_ctx_t *ctx, uint8_t hash[32]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = (size_t)(ctx->count & 63);

    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        memset(ctx->buffer + idx, 0, 64 - idx);
        rc_sha256_transform(ctx, ctx->buffer);
        idx = 0;
    }
    memset(ctx->buffer + idx, 0, 56 - idx);

    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    rc_sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        hash[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

static inline void rc_sha256(const void *data, size_t len, uint8_t hash[32]) {
    rc_sha256_ctx_t ctx;
    rc_sha256_init(&ctx);
    rc_sha256_update(&ctx, data, len);
    rc_sha256_final(&ctx, hash);
}

/* ── HMAC-SHA256 ────────────────────────────────────────────── */
static inline void rc_hmac_sha256(const uint8_t *key, size_t key_len,
                                   const uint8_t *data, size_t data_len,
                                   uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64];
    rc_sha256_ctx_t ctx;
    uint8_t kh[32];

    /* If key > 64 bytes, hash it first */
    if (key_len > 64) {
        rc_sha256(key, key_len, kh);
        key = kh; key_len = 32;
    }

    memset(k, 0, 64);
    memcpy(k, key, key_len);
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }

    /* Inner hash: H(ipad || data) */
    rc_sha256_init(&ctx);
    rc_sha256_update(&ctx, ipad, 64);
    rc_sha256_update(&ctx, data, data_len);
    rc_sha256_final(&ctx, out);

    /* Outer hash: H(opad || inner_hash) */
    rc_sha256_init(&ctx);
    rc_sha256_update(&ctx, opad, 64);
    rc_sha256_update(&ctx, out, 32);
    rc_sha256_final(&ctx, out);
}

/* ── PBKDF2-HMAC-SHA256 (for key derivation) ────────────────── */
static inline int rc_pbkdf2_hmac_sha256(const uint8_t *password, size_t pass_len,
                                          const uint8_t *salt, size_t salt_len,
                                          uint32_t iterations, uint8_t *out, size_t out_len) {
    if (iterations == 0 || out_len == 0) return RC_ERR_INVAL;

    uint32_t blocks = (uint32_t)((out_len + 31) / 32);
    uint8_t u[32], t[32];
    uint8_t salt_block[256];

    for (uint32_t block = 1; block <= blocks; block++) {
        /* U1 = HMAC(password, salt || INT(block)) */
        size_t sl = salt_len + 4;
        if (sl > sizeof(salt_block)) return RC_ERR_INVAL;
        memcpy(salt_block, salt, salt_len);
        salt_block[salt_len]     = (uint8_t)(block >> 24);
        salt_block[salt_len + 1] = (uint8_t)(block >> 16);
        salt_block[salt_len + 2] = (uint8_t)(block >> 8);
        salt_block[salt_len + 3] = (uint8_t)(block);

        rc_hmac_sha256(password, pass_len, salt_block, sl, u);
        memcpy(t, u, 32);

        for (uint32_t iter = 1; iter < iterations; iter++) {
            rc_hmac_sha256(password, pass_len, u, 32, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }

        size_t offset = (size_t)(block - 1) * 32;
        size_t copy = out_len - offset;
        if (copy > 32) copy = 32;
        memcpy(out + offset, t, copy);
    }
    return RC_OK;
}

/* ── AES-256-CTR (self-contained implementation) ────────────── */
/* AES S-Box */
static const uint8_t rc_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t rc_aes_rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

typedef struct {
    uint32_t round_key[60]; /* 15 rounds * 4 words */
    int nr_rounds;
} rc_aes256_ctx_t;

static inline uint32_t rc_aes_sub_word(uint32_t w) {
    return ((uint32_t)rc_aes_sbox[(w >> 24) & 0xFF] << 24) |
           ((uint32_t)rc_aes_sbox[(w >> 16) & 0xFF] << 16) |
           ((uint32_t)rc_aes_sbox[(w >> 8)  & 0xFF] << 8)  |
           ((uint32_t)rc_aes_sbox[w & 0xFF]);
}

static inline uint32_t rc_aes_rot_word(uint32_t w) {
    return (w << 8) | (w >> 24);
}

static inline void rc_aes256_key_expand(rc_aes256_ctx_t *ctx, const uint8_t key[32]) {
    ctx->nr_rounds = 14;
    uint32_t *rk = ctx->round_key;

    for (int i = 0; i < 8; i++) {
        rk[i] = ((uint32_t)key[4*i] << 24) | ((uint32_t)key[4*i+1] << 16) |
                ((uint32_t)key[4*i+2] << 8) | (uint32_t)key[4*i+3];
    }
    for (int i = 8; i < 60; i++) {
        uint32_t temp = rk[i - 1];
        if (i % 8 == 0) {
            temp = rc_aes_sub_word(rc_aes_rot_word(temp)) ^ ((uint32_t)rc_aes_rcon[i/8] << 24);
        } else if (i % 8 == 4) {
            temp = rc_aes_sub_word(temp);
        }
        rk[i] = rk[i - 8] ^ temp;
    }
}

static inline void rc_aes_add_round_key(uint8_t state[16], const uint32_t *rk, int round) {
    for (int c = 0; c < 4; c++) {
        uint32_t w = rk[round * 4 + c];
        state[c]      ^= (uint8_t)(w >> 24);
        state[4 + c]  ^= (uint8_t)(w >> 16);
        state[8 + c]  ^= (uint8_t)(w >> 8);
        state[12 + c] ^= (uint8_t)(w);
    }
}

static inline void rc_aes_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) state[i] = rc_aes_sbox[state[i]];
}

static inline void rc_aes_shift_rows(uint8_t state[16]) {
    uint8_t t;
    /* Row 1: shift left by 1 */
    t = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = t;
    /* Row 2: shift left by 2 */
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    /* Row 3: shift left by 3 */
    t = state[15]; state[15] = state[11]; state[11] = state[7]; state[7] = state[3]; state[3] = t;
}

static inline uint8_t rc_aes_xtime(uint8_t x) {
    return (x << 1) ^ ((x & 0x80) ? 0x1b : 0x00);
}

static inline void rc_aes_mix_columns(uint8_t state[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t *col = state + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        uint8_t e = a0 ^ a1 ^ a2 ^ a3;
        col[0] ^= e ^ rc_aes_xtime(a0 ^ a1);
        col[1] ^= e ^ rc_aes_xtime(a1 ^ a2);
        col[2] ^= e ^ rc_aes_xtime(a2 ^ a3);
        col[3] ^= e ^ rc_aes_xtime(a3 ^ a0);
    }
}

static inline void rc_aes256_encrypt_block(rc_aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    memcpy(state, in, 16);

    rc_aes_add_round_key(state, ctx->round_key, 0);

    for (int round = 1; round < ctx->nr_rounds; round++) {
        rc_aes_sub_bytes(state);
        rc_aes_shift_rows(state);
        rc_aes_mix_columns(state);
        rc_aes_add_round_key(state, ctx->round_key, round);
    }

    rc_aes_sub_bytes(state);
    rc_aes_shift_rows(state);
    rc_aes_add_round_key(state, ctx->round_key, ctx->nr_rounds);

    memcpy(out, state, 16);
}

/* Increment a 128-bit counter (big-endian) */
static inline void rc_aes_ctr_inc(uint8_t ctr[16]) {
    for (int i = 15; i >= 0; i--) {
        if (++ctr[i] != 0) break;
    }
}

/* AES-256-CTR encrypt/decrypt (same operation) */
static inline int rc_aes256_ctr(rc_aes256_ctx_t *ctx,
                                 const uint8_t *input, uint8_t *output,
                                 size_t len, const uint8_t nonce[12]) {
    uint8_t ctr[16], keystream[16];
    /* Counter = nonce (12 bytes) + block_counter (4 bytes, big-endian) */
    memcpy(ctr, nonce, 12);
    ctr[12] = ctr[13] = ctr[14] = ctr[15] = 0;

    size_t off = 0;
    while (off < len) {
        rc_aes256_encrypt_block(ctx, ctr, keystream);
        size_t block = MIN(len - off, 16);
        for (size_t i = 0; i < block; i++) {
            output[off + i] = input[off + i] ^ keystream[i];
        }
        off += block;
        rc_aes_ctr_inc(ctr);
    }
    return RC_OK;
}

/* ── High-level crypto context ──────────────────────────────── */
typedef struct {
    rc_aes256_ctx_t aes;
    uint8_t key[RC_CRYPTO_KEY_SIZE];
    uint8_t nonce[RC_CRYPTO_NONCE_SIZE];
    uint64_t counter;
    int64_t  created_at;     /* for key rotation */
    int64_t  rotate_after_ms;
} rc_crypto_ctx_t;

static inline int rc_crypto_init(rc_crypto_ctx_t *ctx, const uint8_t key[32],
                                  const uint8_t nonce[12], int64_t rotate_after_ms) {
    memcpy(ctx->key, key, 32);
    memcpy(ctx->nonce, nonce, 12);
    rc_aes256_key_expand(&ctx->aes, key);
    ctx->counter = 0;
    ctx->created_at = rc_time_ms();
    ctx->rotate_after_ms = rotate_after_ms;
    return RC_OK;
}

static inline int rc_crypto_encrypt(rc_crypto_ctx_t *ctx,
                                     const uint8_t *plain, uint8_t *cipher,
                                     size_t len) {
    /* Build per-packet nonce: 8 bytes from ctx nonce + 8 bytes counter */
    uint8_t pkt_nonce[12];
    memcpy(pkt_nonce, ctx->nonce, 8);
    uint64_t ctr = __atomic_fetch_add(&ctx->counter, 1, __ATOMIC_RELAXED);
    pkt_nonce[8]  = (uint8_t)(ctr >> 32);
    pkt_nonce[9]  = (uint8_t)(ctr >> 24);
    pkt_nonce[10] = (uint8_t)(ctr >> 16);
    pkt_nonce[11] = (uint8_t)(ctr >> 8);

    /* Prepend 4-byte nonce suffix to ciphertext so receiver can reconstruct */
    uint8_t nonce_tail[4] = { pkt_nonce[8], pkt_nonce[9], pkt_nonce[10], pkt_nonce[11] };
    /* We'll just use CTR directly - caller handles framing */
    return rc_aes256_ctr(&ctx->aes, plain, cipher, len, pkt_nonce);
}

static inline int rc_crypto_decrypt(rc_crypto_ctx_t *ctx,
                                     const uint8_t *cipher, uint8_t *plain,
                                     size_t len, const uint8_t nonce_tail[4]) {
    uint8_t pkt_nonce[12];
    memcpy(pkt_nonce, ctx->nonce, 8);
    pkt_nonce[8]  = nonce_tail[0];
    pkt_nonce[9]  = nonce_tail[1];
    pkt_nonce[10] = nonce_tail[2];
    pkt_nonce[11] = nonce_tail[3];

    return rc_aes256_ctr(&ctx->aes, cipher, plain, len, pkt_nonce);
}

static inline int rc_crypto_needs_rotation(rc_crypto_ctx_t *ctx) {
    if (ctx->rotate_after_ms <= 0) return 0;
    return (rc_time_ms() - ctx->created_at) >= ctx->rotate_after_ms;
}

static inline int rc_crypto_rotate(rc_crypto_ctx_t *ctx, const uint8_t new_key[32]) {
    memcpy(ctx->key, new_key, 32);
    rc_aes256_key_expand(&ctx->aes, new_key);
    ctx->counter = 0;
    ctx->created_at = rc_time_ms();
    /* Generate new nonce from SHA256 of old nonce + new key */
    uint8_t material[12 + 32];
    memcpy(material, ctx->nonce, 12);
    memcpy(material + 12, new_key, 32);
    uint8_t hash[32];
    rc_sha256(material, 44, hash);
    memcpy(ctx->nonce, hash, 12);
    return RC_OK;
}

/* ── Secure comparison (constant-time) ──────────────────────── */
static inline int rc_secure_compare(const void *a, const void *b, size_t len) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= pa[i] ^ pb[i];
    }
    return diff == 0 ? 0 : 1;
}

/* ── Token generation (HMAC-SHA256 based, for auth) ─────────── */
static inline int rc_generate_token(const uint8_t *secret, size_t secret_len,
                                     const char *user_id, int64_t expiry,
                                     char *out_token, size_t out_size) {
    /* token = base64url(user_id || expiry || hmac) */
    char payload[512];
    int plen = snprintf(payload, sizeof(payload), "%s:%ld", user_id, (long)expiry);
    if (plen < 0 || (size_t)plen >= sizeof(payload)) return RC_ERR_INVAL;

    uint8_t mac[32];
    rc_hmac_sha256(secret, secret_len, (uint8_t *)payload, (size_t)plen, mac);

    /* Simple hex encoding */
    size_t needed = (size_t)plen + 1 + 64; /* payload + '.' + hex(mac) */
    if (out_size < needed + 1) return RC_ERR_FULL;

    memcpy(out_token, payload, (size_t)plen);
    out_token[plen] = '.';
    for (int i = 0; i < 32; i++) {
        snprintf(out_token + plen + 1 + i * 2, 3, "%02x", mac[i]);
    }
    out_token[needed] = '\0';
    return RC_OK;
}

static inline int rc_verify_token(const uint8_t *secret, size_t secret_len,
                                   const char *token, char *out_user_id,
                                   size_t uid_size, int64_t *out_expiry) {
    const char *dot = strrchr(token, '.');
    if (!dot || (dot - token) < 1) return RC_ERR_AUTH;

    size_t payload_len = (size_t)(dot - token);
    const char *mac_hex = dot + 1;
    if (strlen(mac_hex) != 64) return RC_ERR_AUTH;

    uint8_t expected_mac[32];
    rc_hmac_sha256(secret, secret_len, (const uint8_t *)token, payload_len, expected_mac);

    /* Decode hex mac */
    uint8_t actual_mac[32];
    for (int i = 0; i < 32; i++) {
        unsigned int byte;
        if (sscanf(mac_hex + i * 2, "%02x", &byte) != 1) return RC_ERR_AUTH;
        actual_mac[i] = (uint8_t)byte;
    }

    if (rc_secure_compare(expected_mac, actual_mac, 32) != 0) return RC_ERR_AUTH;

    /* Parse user_id:expiry */
    char payload[512];
    if (payload_len >= sizeof(payload)) return RC_ERR_INVAL;
    memcpy(payload, token, payload_len);
    payload[payload_len] = '\0';

    char *colon = strrchr(payload, ':');
    if (!colon) return RC_ERR_AUTH;
    *colon = '\0';

    /* Check expiry */
    int64_t exp = (int64_t)atol(colon + 1);
    if (out_expiry) *out_expiry = exp;

    int64_t now = (int64_t)time(NULL);
    if (exp > 0 && now > exp) return RC_ERR_AUTH;

    if (out_user_id) {
        size_t ulen = strlen(payload);
        if (ulen >= uid_size) return RC_ERR_FULL;
        memcpy(out_user_id, payload, ulen + 1);
    }

    return RC_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* ROBOCONTROL_CRYPTO_H */
