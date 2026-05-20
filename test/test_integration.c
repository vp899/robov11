/**
 * @file test_integration.c
 * @brief Integration tests for RoboControl protocol stack
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rc_proto.h"
#include "rc_conn.h"
#include "rc_crypto.h"
#include "rc_congestion.h"

static void test_full_handshake_flow(void)
{
    printf("  Testing full handshake flow...\n");

    /* Simulate X25519 key exchange */
    uint8_t cpriv[32], cpub[32], spriv[32], spub[32];
    assert(rc_x25519_keygen(cpriv, cpub) == 0);
    assert(rc_x25519_keygen(spriv, spub) == 0);

    uint8_t cshared[32], sshared[32];
    assert(rc_x25519_shared(cpriv, spub, cshared) == 0);
    assert(rc_x25519_shared(spriv, cpub, sshared) == 0);
    assert(memcmp(cshared, sshared, 32) == 0);

    /* Derive session keys */
    rc_session_t csess, ssess;
    rc_session_init(&csess, RC_AEAD_AES_256_GCM);
    rc_session_init(&ssess, RC_AEAD_AES_256_GCM);

    uint8_t cr[32], sr[32];
    memset(cr, 0xAA, 32); memset(sr, 0xBB, 32);
    assert(rc_derive_session_keys(cshared, 32, cr, sr, &csess) == 0);
    assert(rc_derive_session_keys(sshared, 32, cr, sr, &ssess) == 0);
    assert(memcmp(csess.encrypt_ctx.key, ssess.encrypt_ctx.key, 32) == 0);

    rc_session_destroy(&csess);
    rc_session_destroy(&ssess);
    printf("    PASS: full handshake flow\n");
}

static void test_encrypted_data_transfer(void)
{
    printf("  Testing encrypted data transfer...\n");

    uint8_t ss[32], cr[32], sr[32];
    memset(ss, 0xAA, 32); memset(cr, 0x11, 32); memset(sr, 0x22, 32);

    rc_session_t session;
    rc_session_init(&session, RC_AEAD_AES_256_GCM);
    rc_derive_session_keys(ss, 32, cr, sr, &session);

    uint8_t frame[1024];
    for (int i = 0; i < 1024; i++) frame[i] = (uint8_t)(i & 0xFF);

    uint8_t encrypted[1024 + RC_MAX_TAG_LEN];
    size_t ct_len = 0;
    uint8_t nonce[RC_AES256_NONCE_LEN];
    memset(nonce, 0x01, sizeof(nonce));

    assert(rc_aead_encrypt(&session.encrypt_ctx, nonce,
                            frame, sizeof(frame), NULL, 0,
                            encrypted, &ct_len) == 0);

    rc_aead_ctx_t dctx;
    rc_aead_init(&dctx, RC_AEAD_AES_256_GCM, session.encrypt_ctx.key);
    uint8_t decrypted[1024];
    size_t dec_len = 0;
    assert(rc_aead_decrypt(&dctx, nonce, encrypted, ct_len,
                            NULL, 0, decrypted, &dec_len) == 0);
    assert(dec_len == sizeof(frame));
    assert(memcmp(decrypted, frame, sizeof(frame)) == 0);

    rc_session_destroy(&session);
    printf("    PASS: encrypted data transfer\n");
}

static void test_conn_with_congestion(void)
{
    printf("  Testing connection with congestion control...\n");

    rc_conn_pool_t pool;
    rc_conn_pool_init(&pool, 16);
    rc_conn_t *conn = rc_conn_create(&pool, -1, 1);
    assert(conn != NULL);
    rc_conn_set_state(conn, RC_STATE_ESTABLISHED);

    rc_cubic_t cubic;
    rc_cubic_init(&cubic, 1460);

    for (int i = 0; i < 1000; i++)
        rc_cubic_on_ack(&cubic, 1000, 10);
    assert(cubic.cwnd > RC_CUBIC_INIT_CWND);

    rc_cubic_on_loss(&cubic);
    uint32_t post_loss = cubic.cwnd;
    (void)post_loss;

    for (int i = 0; i < 100; i++)
        rc_cubic_on_ack(&cubic, 1000, 10);
    assert(cubic.cwnd >= post_loss);

    rc_conn_release(conn);
    rc_conn_pool_destroy(&pool);
    printf("    PASS: connection with congestion control\n");
}

static void test_data_packet_roundtrip(void)
{
    printf("  Testing data packet round-trip...\n");

    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_DATA, 42, 7);
    hdr.payload_len = 100;

    uint8_t packet[RC_HEADER_SIZE + 100];
    rc_hdr_pack(packet, &hdr);
    memset(packet + RC_HEADER_SIZE, 0xAB, 100);

    rc_pkt_hdr_t parsed;
    assert(rc_hdr_unpack(&parsed, packet) == 0);
    assert(parsed.seq == 42);
    assert(parsed.conn_id == 7);
    assert(parsed.payload_len == 100);

    for (int i = 0; i < 100; i++)
        assert(packet[RC_HEADER_SIZE + i] == 0xAB);

    printf("    PASS: data packet round-trip\n");
}

int main(void)
{
    printf("Running test_integration...\n");
    test_full_handshake_flow();
    test_encrypted_data_transfer();
    test_conn_with_congestion();
    test_data_packet_roundtrip();
    printf("test_integration: ALL PASSED\n");
    return 0;
}
