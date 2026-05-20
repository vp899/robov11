#include <stdbool.h>
/**
 * @file rc_conn.c
 * @brief Connection manager — pool, lifecycle, ring buffers.
 */

#include "rc_conn.h"
#include "rc_proto.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <sys/time.h>

/* ================================================================== */
/*  Ring buffer (lock-free SPSC)                                       */
/* ================================================================== */

/**
 * @brief Round up to the next power of two.
 * @param v  Input value.
 * @return Smallest power of two >= v.
 */
static uint32_t next_pow2(uint32_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

/* ------------------------------------------------------------------ */

int rc_ringbuf_init(rc_ringbuf_t *rb, uint32_t size)
{
    if (size == 0) size = 4096;
    size = next_pow2(size);
    rb->buf = (uint8_t *)calloc(1, size);
    if (!rb->buf) {
        syslog(LOG_ERR, "rc_conn: ringbuf alloc failed (%u bytes)", size);
        return -1;
    }
    rb->size = size;
    rb->mask = size - 1;
    rb->head = 0;
    rb->tail = 0;
    return 0;
}

/* ------------------------------------------------------------------ */

void rc_ringbuf_destroy(rc_ringbuf_t *rb)
{
    if (rb->buf) {
        free(rb->buf);
        rb->buf = NULL;
    }
    rb->size = 0;
    rb->mask = 0;
    rb->head = 0;
    rb->tail = 0;
}

/* ------------------------------------------------------------------ */

size_t rc_ringbuf_write(rc_ringbuf_t *rb, const uint8_t *data, size_t len)
{
    uint32_t free_space = rb->size - (rb->head - rb->tail);
    if (len > free_space) {
        len = free_space;  /* partial write — buffer is full */
    }

    uint32_t pos = rb->head & rb->mask;
    uint32_t first_chunk = rb->size - pos;

    if (len <= first_chunk) {
        /* Single contiguous write */
        memcpy(rb->buf + pos, data, len);
    } else {
        /* Wrapping write */
        memcpy(rb->buf + pos, data, first_chunk);
        memcpy(rb->buf, data + first_chunk, len - first_chunk);
    }

    /* Memory barrier: make data visible before updating head.
     * On x86, stores are not reordered, but we use __atomic for
     * portability and to prevent compiler reordering. */
    __atomic_store_n(&rb->head, rb->head + (uint32_t)len, __ATOMIC_RELEASE);
    return len;
}

/* ------------------------------------------------------------------ */

size_t rc_ringbuf_read(rc_ringbuf_t *rb, uint8_t *dst, size_t len)
{
    uint32_t avail = rb->head - rb->tail;
    if (len > avail) {
        len = avail;  /* partial read — not enough data */
    }

    uint32_t pos = rb->tail & rb->mask;
    uint32_t first_chunk = rb->size - pos;

    if (len <= first_chunk) {
        memcpy(dst, rb->buf + pos, len);
    } else {
        memcpy(dst, rb->buf + pos, first_chunk);
        memcpy(dst + first_chunk, rb->buf, len - first_chunk);
    }

    __atomic_store_n(&rb->tail, rb->tail + (uint32_t)len, __ATOMIC_RELEASE);
    return len;
}

/* ================================================================== */
/*  Hash function for conn_id                                          */
/* ================================================================== */

/**
 * @brief Fast hash for 32-bit connection IDs.
 *
 * Uses the finalisation step from MurmurHash3 for good distribution.
 */
static inline uint32_t conn_hash(uint32_t key)
{
    key ^= key >> 16;
    key *= 0x85EBCA6BU;
    key ^= key >> 13;
    key *= 0xC2B2AE35U;
    key ^= key >> 16;
    return key;
}

/* ================================================================== */
/*  Connection pool                                                    */
/* ================================================================== */

int rc_conn_pool_init(rc_conn_pool_t *pool, uint64_t max_conn)
{
    if (max_conn == 0) max_conn = RC_MAX_CONNECTIONS;
    pool->max_conn = max_conn;
    pool->count    = 0;

    pool->buckets = (rc_conn_t **)calloc(RC_CONN_POOL_BUCKETS,
                                          sizeof(rc_conn_t *));
    if (!pool->buckets) {
        syslog(LOG_ERR, "rc_conn: pool alloc failed (%lu buckets)",
               RC_CONN_POOL_BUCKETS);
        return -1;
    }

    for (uint64_t i = 0; i < RC_CONN_POOL_LOCKS; i++) {
        if (pthread_mutex_init(&pool->locks[i], NULL) != 0) {
            /* Clean up already-initialised locks */
            for (uint64_t j = 0; j < i; j++) {
                pthread_mutex_destroy(&pool->locks[j]);
            }
            free(pool->buckets);
            pool->buckets = NULL;
            return -1;
        }
    }

    syslog(LOG_INFO, "rc_conn: pool initialised (max=%lu, buckets=%lu)",
           max_conn, RC_CONN_POOL_BUCKETS);
    return 0;
}

/* ------------------------------------------------------------------ */

void rc_conn_pool_destroy(rc_conn_pool_t *pool)
{
    if (!pool->buckets) return;

    /* Walk all buckets and free every connection in the chain */
    for (uint64_t i = 0; i < RC_CONN_POOL_BUCKETS; i++) {
        rc_conn_t *conn = pool->buckets[i];
        while (conn) {
            rc_conn_t *next = (rc_conn_t *)conn->user_data;
            if (conn->fd >= 0) {
                close(conn->fd);
                conn->fd = -1;
            }
            rc_ringbuf_destroy(&conn->send_buf);
            rc_ringbuf_destroy(&conn->recv_buf);
            pthread_mutex_destroy(&conn->state_mtx);
            rc_session_destroy(&conn->session);
            free(conn);
            conn = next;
        }
    }

    for (uint64_t i = 0; i < RC_CONN_POOL_LOCKS; i++) {
        pthread_mutex_destroy(&pool->locks[i]);
    }

    free(pool->buckets);
    pool->buckets = NULL;
    pool->count = 0;
}

/* ------------------------------------------------------------------ */

/**
 * @brief Get the lock index for a given bucket.
 * @param bucket_idx  Bucket index.
 * @return Lock index (0..RC_CONN_POOL_LOCKS-1).
 */
static inline uint64_t bucket_lock_idx(uint64_t bucket_idx)
{
    return bucket_idx & (RC_CONN_POOL_LOCKS - 1);
}

/* ------------------------------------------------------------------ */

rc_conn_t *rc_conn_create(rc_conn_pool_t *pool, int fd, uint32_t conn_id)
{
    if (pool->count >= pool->max_conn) {
        syslog(LOG_WARNING, "rc_conn: pool full (%lu/%lu)",
               pool->count, pool->max_conn);
        return NULL;
    }

    /* Allocate connection */
    rc_conn_t *conn = (rc_conn_t *)calloc(1, sizeof(rc_conn_t));
    if (!conn) {
        syslog(LOG_ERR, "rc_conn: alloc failed (%zu bytes)", sizeof(rc_conn_t));
        return NULL;
    }

    /* Assign connection ID */
    if (conn_id == 0) {
        /* Auto-generate a unique conn_id */
        static volatile uint32_t next_id = 1;
        conn_id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
    }
    conn->conn_id = conn_id;
    conn->fd      = fd;

    /* Initialise state machine */
    conn->state = RC_STATE_CLOSED;
    if (pthread_mutex_init(&conn->state_mtx, NULL) != 0) {
        free(conn);
        return NULL;
    }
    conn->refcnt = 1;

    /* Initialise ring buffers */
    if (rc_ringbuf_init(&conn->send_buf, 65536) != 0 ||
        rc_ringbuf_init(&conn->recv_buf, 65536) != 0) {
        rc_ringbuf_destroy(&conn->send_buf);
        pthread_mutex_destroy(&conn->state_mtx);
        free(conn);
        return NULL;
    }

    /* Initialise crypto session */
    rc_session_init(&conn->session, RC_AEAD_AES_256_GCM);

    /* Initialise congestion control */
    rc_cubic_init(&conn->cubic, RC_CUBIC_MSS);

    /* Timing */
    gettimeofday(&conn->created_at, NULL);
    conn->last_recv = conn->created_at;
    conn->last_send = conn->created_at;
    conn->rto_ms    = RC_MIN_RTO_MS;

    /* Insert into hash table */
    uint32_t hash  = conn_hash(conn_id);
    uint64_t bucket = hash % RC_CONN_POOL_BUCKETS;
    uint64_t lock_i = bucket_lock_idx(bucket);

    pthread_mutex_lock(&pool->locks[lock_i]);
    /* Chain at head (simple insertion) */
    conn->user_data = pool->buckets[bucket];
    pool->buckets[bucket] = conn;
    pool->count++;
    pthread_mutex_unlock(&pool->locks[lock_i]);

    syslog(LOG_DEBUG, "rc_conn: created conn_id=%u fd=%d", conn_id, fd);
    return conn;
}

/* ------------------------------------------------------------------ */

rc_conn_t *rc_conn_lookup(rc_conn_pool_t *pool, uint32_t conn_id)
{
    uint32_t hash   = conn_hash(conn_id);
    uint64_t bucket = hash % RC_CONN_POOL_BUCKETS;
    uint64_t lock_i = bucket_lock_idx(bucket);

    pthread_mutex_lock(&pool->locks[lock_i]);
    rc_conn_t *conn = pool->buckets[bucket];
    while (conn) {
        if (conn->conn_id == conn_id) {
            __atomic_fetch_add(&conn->refcnt, 1, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&pool->locks[lock_i]);
            return conn;
        }
        /* Follow the chain stored in user_data during pool lifetime */
        conn = (rc_conn_t *)conn->user_data;
    }
    pthread_mutex_unlock(&pool->locks[lock_i]);
    return NULL;
}

/* ------------------------------------------------------------------ */

void rc_conn_release(rc_conn_t *conn)
{
    if (!conn) return;

    uint32_t old = __atomic_fetch_sub(&conn->refcnt, 1, __ATOMIC_ACQ_REL);
    if (old == 1) {
        /* Last reference — close fd and mark as dead.
         * Actual freeing is deferred to rc_conn_pool_destroy()
         * which owns the memory.  This avoids double-free when
         * the caller releases before destroying the pool. */
        if (conn->fd >= 0) {
            close(conn->fd);
            conn->fd = -1;
        }
        rc_ringbuf_destroy(&conn->send_buf);
        rc_ringbuf_destroy(&conn->recv_buf);
        conn->state = RC_STATE_CLOSED;
    }
}

/* ------------------------------------------------------------------ */

int rc_conn_set_state(rc_conn_t *conn, rc_conn_state_t new_state)
{
    pthread_mutex_lock(&conn->state_mtx);

    rc_conn_state_t old_state = conn->state;

    /* Validate transition */
    bool valid = false;
    switch (old_state) {
    case RC_STATE_CLOSED:
        valid = (new_state == RC_STATE_CONNECTING);
        break;
    case RC_STATE_CONNECTING:
        valid = (new_state == RC_STATE_HANDSHAKING ||
                 new_state == RC_STATE_CLOSED);
        break;
    case RC_STATE_HANDSHAKING:
        valid = (new_state == RC_STATE_ESTABLISHED ||
                 new_state == RC_STATE_CLOSED);
        break;
    case RC_STATE_ESTABLISHED:
        valid = (new_state == RC_STATE_CLOSING ||
                 new_state == RC_STATE_CLOSED);
        break;
    case RC_STATE_CLOSING:
        valid = (new_state == RC_STATE_CLOSED);
        break;
    }

    if (!valid) {
        pthread_mutex_unlock(&conn->state_mtx);
        syslog(LOG_WARNING, "rc_conn: invalid transition %s → %s for conn_id=%u",
               rc_conn_state_name(old_state),
               rc_conn_state_name(new_state), conn->conn_id);
        return -1;
    }

    conn->state = new_state;
    pthread_mutex_unlock(&conn->state_mtx);

    syslog(LOG_DEBUG, "rc_conn: conn_id=%u %s → %s",
           conn->conn_id,
           rc_conn_state_name(old_state),
           rc_conn_state_name(new_state));

    /* Fire callback outside the lock */
    if (conn->on_state) {
        conn->on_state(conn, old_state, new_state);
    }

    return 0;
}

/* ------------------------------------------------------------------ */

void rc_conn_close(rc_conn_t *conn)
{
    if (!conn) return;

    rc_conn_set_state(conn, RC_STATE_CLOSING);
    rc_conn_set_state(conn, RC_STATE_CLOSED);

    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
}

/* ------------------------------------------------------------------ */

const char *rc_conn_state_name(rc_conn_state_t state)
{
    switch (state) {
    case RC_STATE_CLOSED:      return "CLOSED";
    case RC_STATE_CONNECTING:  return "CONNECTING";
    case RC_STATE_HANDSHAKING: return "HANDSHAKING";
    case RC_STATE_ESTABLISHED: return "ESTABLISHED";
    case RC_STATE_CLOSING:     return "CLOSING";
    default:                   return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */

int rc_conn_purge_timeouts(rc_conn_pool_t *pool, uint32_t timeout_ms)
{
    int purged = 0;
    struct timeval now;
    gettimeofday(&now, NULL);

    for (uint64_t i = 0; i < RC_CONN_POOL_BUCKETS; i++) {
        uint64_t lock_i = bucket_lock_idx(i);
        pthread_mutex_lock(&pool->locks[i]);

        rc_conn_t **pp = &pool->buckets[i];
        while (*pp) {
            rc_conn_t *conn = *pp;
            uint64_t idle_ms = (uint64_t)(now.tv_sec - conn->last_recv.tv_sec) * 1000 +
                               (uint64_t)(now.tv_usec - conn->last_recv.tv_usec) / 1000;

            if (idle_ms > timeout_ms &&
                conn->state != RC_STATE_CLOSED) {
                /* Remove from chain */
                *pp = (rc_conn_t *)conn->user_data;
                pool->count--;

                syslog(LOG_INFO, "rc_conn: purging idle conn_id=%u (%lu ms)",
                       conn->conn_id, idle_ms);

                /* Close outside the pool lock */
                pthread_mutex_unlock(&pool->locks[lock_i]);
                rc_conn_close(conn);
                rc_conn_release(conn);
                purged++;
                pthread_mutex_lock(&pool->locks[lock_i]);
                continue;
            }
            pp = (rc_conn_t **)&conn->user_data;
        }

        pthread_mutex_unlock(&pool->locks[lock_i]);
    }

    return purged;
}
