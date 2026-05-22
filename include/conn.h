/*
 * robocontrol/conn.h - Connection manager with hash table
 */
#ifndef ROBOCONTROL_CONN_H
#define ROBOCONTROL_CONN_H

#include "common.h"
#include "ringbuf.h"
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Connection states ──────────────────────────────────────── */
typedef enum {
    RC_CONN_IDLE = 0,
    RC_CONN_CONNECTING,
    RC_CONN_HANDSHAKE,
    RC_CONN_AUTHENTICATED,
    RC_CONN_ACTIVE,
    RC_CONN_CLOSING,
    RC_CONN_CLOSED,
} rc_conn_state_t;

/* ── Connection entry ───────────────────────────────────────── */
typedef struct rc_conn {
    int              fd;
    uint32_t         session_id;
    uint32_t         client_id;
    rc_conn_state_t  state;
    struct sockaddr_in addr;
    int64_t          created_at;
    int64_t          last_active;
    int64_t          last_ping;
    uint32_t         seq_num;
    uint32_t         remote_seq;
    rc_ringbuf_t     recv_buf;
    rc_ringbuf_t     send_buf;
    uint8_t          has_crypto;
    /* Forward declaration - actual crypto ctx in include/crypto.h */
    void            *crypto_ctx;
    /* User data pointer (for application layer) */
    void            *user_data;
    /* Hash chain */
    struct rc_conn  *next;
    struct rc_conn  *prev;
} rc_conn_t;

/* ── Connection hash table ──────────────────────────────────── */
#define RC_CONN_HASH_SIZE   (1 << 16)  /* 65536 buckets */
#define RC_CONN_HASH_MASK   (RC_CONN_HASH_SIZE - 1)

typedef struct {
    rc_conn_t      *buckets[RC_CONN_HASH_SIZE];
    rc_spinlock_t   locks[RC_CONN_HASH_SIZE];
    rc_atomic64_t   count;
    rc_atomic64_t   total_created;
    rc_atomic64_t   total_destroyed;
} rc_conn_table_t;

static inline uint32_t rc_conn_hash(uint32_t session_id) {
    uint32_t h = session_id;
    h ^= h >> 16;
    h *= 0x45d9f3b;
    h ^= h >> 16;
    return h & RC_CONN_HASH_MASK;
}

static inline int rc_conn_table_init(rc_conn_table_t *table) {
    memset(table->buckets, 0, sizeof(table->buckets));
    for (int i = 0; i < RC_CONN_HASH_SIZE; i++) {
        rc_spin_init(&table->locks[i]);
    }
    rc_atomic_init(&table->count, 0);
    rc_atomic_init(&table->total_created, 0);
    rc_atomic_init(&table->total_destroyed, 0);
    return RC_OK;
}

static inline rc_conn_t *rc_conn_create(rc_conn_table_t *table, int fd,
                                         uint32_t session_id,
                                         const struct sockaddr_in *addr) {
    rc_conn_t *conn = calloc(1, sizeof(rc_conn_t));
    if (!conn) return NULL;

    conn->fd = fd;
    conn->session_id = session_id;
    conn->state = RC_CONN_CONNECTING;
    if (addr) conn->addr = *addr;
    conn->created_at = rc_time_ms();
    conn->last_active = conn->created_at;
    conn->last_ping = conn->created_at;
    conn->seq_num = 0;
    conn->remote_seq = 0;

    if (rc_ringbuf_init(&conn->recv_buf, RINGBUF_SIZE) != RC_OK) {
        free(conn);
        return NULL;
    }
    if (rc_ringbuf_init(&conn->send_buf, RINGBUF_SIZE) != RC_OK) {
        rc_ringbuf_destroy(&conn->recv_buf);
        free(conn);
        return NULL;
    }

    /* Insert into hash table */
    uint32_t idx = rc_conn_hash(session_id);
    rc_spin_lock(&table->locks[idx]);
    conn->next = table->buckets[idx];
    conn->prev = NULL;
    if (table->buckets[idx]) {
        table->buckets[idx]->prev = conn;
    }
    table->buckets[idx] = conn;
    rc_spin_unlock(&table->locks[idx]);

    rc_atomic_inc(&table->count);
    rc_atomic_inc(&table->total_created);

    return conn;
}

static inline rc_conn_t *rc_conn_find(rc_conn_table_t *table, uint32_t session_id) {
    uint32_t idx = rc_conn_hash(session_id);
    rc_spin_lock(&table->locks[idx]);
    rc_conn_t *conn = table->buckets[idx];
    while (conn) {
        if (conn->session_id == session_id) {
            rc_spin_unlock(&table->locks[idx]);
            return conn;
        }
        conn = conn->next;
    }
    rc_spin_unlock(&table->locks[idx]);
    return NULL;
}

static inline void rc_conn_destroy(rc_conn_table_t *table, rc_conn_t *conn) {
    if (!conn) return;

    /* Remove from hash table */
    uint32_t idx = rc_conn_hash(conn->session_id);
    rc_spin_lock(&table->locks[idx]);
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else {
        table->buckets[idx] = conn->next;
    }
    if (conn->next) {
        conn->next->prev = conn->prev;
    }
    rc_spin_unlock(&table->locks[idx]);

    /* Cleanup */
    rc_ringbuf_destroy(&conn->recv_buf);
    rc_ringbuf_destroy(&conn->send_buf);
    if (conn->fd >= 0) close(conn->fd);
    free(conn->crypto_ctx);
    conn->crypto_ctx = NULL;

    rc_atomic_dec(&table->count);
    rc_atomic_inc(&table->total_destroyed);
}

static inline void rc_conn_touch(rc_conn_t *conn) {
    conn->last_active = rc_time_ms();
}

static inline int rc_conn_is_stale(rc_conn_t *conn, int64_t timeout_ms) {
    return (rc_time_ms() - conn->last_active) > timeout_ms;
}

static inline void rc_conn_table_walk(rc_conn_table_t *table,
                                       void (*callback)(rc_conn_t *conn, void *ctx),
                                       void *ctx) {
    for (int i = 0; i < RC_CONN_HASH_SIZE; i++) {
        rc_spin_lock(&table->locks[i]);
        rc_conn_t *conn = table->buckets[i];
        while (conn) {
            rc_conn_t *next = conn->next;
            callback(conn, ctx);
            conn = next;
        }
        rc_spin_unlock(&table->locks[i]);
    }
}

static inline int64_t rc_conn_table_count(rc_conn_table_t *table) {
    return rc_atomic_get(&table->count);
}

#ifdef __cplusplus
}
#endif

#endif /* ROBOCONTROL_CONN_H */
