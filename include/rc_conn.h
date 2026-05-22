/**
 * @file rc_conn.h
 * @brief Connection manager — state machine, pool, and ring buffers.
 *
 * Manages the lifecycle of every connection from the moment a SYN-like
 * HELLO arrives until the FIN handshake completes.  Internally stores
 * connections in a hash table keyed by `conn_id` for O(1) lookup, and
 * each connection carries its own send/receive ring buffers for zero-
 * contention producer/consumer data flow.
 *
 * Thread safety:
 *   - The hash table itself is protected by a striped lock array
 *     (one lock per 1024 buckets).
 *   - Each connection carries its own mutex for state transitions.
 *   - Ring buffers are lock-free SPSC (single-producer single-consumer).
 */

#ifndef RC_CONN_H
#define RC_CONN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/time.h>

#include "rc_proto.h"
#include "rc_crypto.h"
#include "rc_congestion.h"
#include "rc_pacing.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Connection states                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Connection state machine.
 *
 * Transitions:
 * @code
 *   CLOSED → CONNECTING → HANDSHAKING → ESTABLISHED → CLOSING → CLOSED
 * @endcode
 */
typedef enum rc_conn_state {
    RC_STATE_CLOSED       = 0,  /**< No connection                   */
    RC_STATE_CONNECTING   = 1,  /**< HELLO sent, awaiting HELLO_ACK  */
    RC_STATE_HANDSHAKING  = 2,  /**< Key exchange in progress        */
    RC_STATE_ESTABLISHED  = 3,  /**< Fully operational               */
    RC_STATE_CLOSING      = 4,  /**< FIN sent, awaiting FIN_ACK      */
} rc_conn_state_t;

/* ------------------------------------------------------------------ */
/*  Ring buffer (lock-free SPSC)                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Lock-free single-producer single-consumer ring buffer.
 *
 * Size is always a power of two so the index mask is cheap.
 */
typedef struct rc_ringbuf {
    uint8_t  *buf;          /**< Backing storage                   */
    uint32_t  size;         /**< Capacity (power of 2)             */
    uint32_t  mask;         /**< size - 1, for fast modulo          */
    volatile uint32_t head; /**< Write position (producer)          */
    volatile uint32_t tail; /**< Read position  (consumer)          */
} rc_ringbuf_t;

/* ------------------------------------------------------------------ */
/*  Callbacks                                                          */
/* ------------------------------------------------------------------ */

/** Forward declaration. */
typedef struct rc_conn rc_conn_t;

/**
 * @brief Callback invoked when data is available on a connection.
 * @param conn  The connection.
 * @param data  Pointer to received data.
 * @param len   Length of data in bytes.
 */
typedef void (*rc_on_data_fn)(rc_conn_t *conn, const uint8_t *data, size_t len);

/**
 * @brief Callback invoked when the connection state changes.
 * @param conn   The connection.
 * @param old_state  Previous state.
 * @param new_state  New state.
 */
typedef void (*rc_on_state_fn)(rc_conn_t *conn,
                               rc_conn_state_t old_state,
                               rc_conn_state_t new_state);

/**
 * @brief Callback invoked on connection errors.
 * @param conn     The connection (may be NULL for listener errors).
 * @param errcode  Negative error code.
 * @param msg      Human-readable error description.
 */
typedef void (*rc_on_error_fn)(rc_conn_t *conn, int errcode, const char *msg);

/* ------------------------------------------------------------------ */
/*  Connection object                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Per-connection state.
 *
 * Every active connection is represented by one of these.  They live
 * in the connection pool's hash table and are reference-counted so
 * that concurrent readers never access freed memory.
 */
struct rc_conn {
    /* --- identity --- */
    uint32_t          conn_id;        /**< Unique connection identifier */
    int               fd;             /**< Socket file descriptor       */

    /* --- state machine --- */
    volatile rc_conn_state_t state;   /**< Current state                */
    pthread_mutex_t   state_mtx;      /**< Protects state transitions   */
    uint32_t          refcnt;         /**< Reference count              */

    /* --- crypto session --- */
    rc_session_t      session;        /**< Crypto session (keys, etc.)  */

    /* --- congestion control --- */
    rc_cubic_t        cubic;          /**< CUBIC congestion state       */

    /* --- pacing --- */
    rc_pacer_t        pacer;          /**< Send pacer (rate limiter)    */
    bool              pacing_enabled; /**< True if pacing is active     */

    /* --- sequence tracking --- */
    uint32_t          local_seq;      /**< Next outgoing sequence #     */
    uint32_t          remote_seq;     /**< Next expected remote seq #   */
    uint32_t          ack_seq;        /**< Highest ACK received         */

    /* --- ring buffers --- */
    rc_ringbuf_t      send_buf;       /**< Outgoing data ring buffer    */
    rc_ringbuf_t      recv_buf;       /**< Incoming data ring buffer    */

    /* --- timing --- */
    struct timeval    created_at;     /**< Connection creation time     */
    struct timeval    last_recv;      /**< Last packet received time    */
    struct timeval    last_send;      /**< Last packet sent time        */
    uint32_t          rtt_ms;         /**< Smoothed RTT in milliseconds */
    uint32_t          rtt_var_ms;     /**< RTT variance                 */
    uint32_t          rto_ms;         /**< Current retransmit timeout   */

    /* --- statistics --- */
    uint64_t          bytes_sent;     /**< Total bytes sent             */
    uint64_t          bytes_recv;     /**< Total bytes received         */
    uint64_t          pkts_sent;      /**< Total packets sent           */
    uint64_t          pkts_recv;      /**< Total packets received       */
    uint32_t          retransmits;    /**< Retransmission count         */

    /* --- callbacks --- */
    rc_on_data_fn     on_data;        /**< Data-ready callback          */
    rc_on_state_fn    on_state;       /**< State-change callback        */
    rc_on_error_fn    on_error;       /**< Error callback               */
    void             *user_data;      /**< Opaque application pointer   */
};

/* ------------------------------------------------------------------ */
/*  Connection pool (hash table)                                       */
/* ------------------------------------------------------------------ */

/** Number of hash buckets (prime for good distribution). */
#define RC_CONN_POOL_BUCKETS  16777259UL  /* ~16.8M, next prime after 2^24 */

/** Number of striped locks (power of 2). */
#define RC_CONN_POOL_LOCKS    8192

/**
 * @brief Connection pool — hash table of active connections.
 */
typedef struct rc_conn_pool {
    rc_conn_t     **buckets;                        /**< Hash table buckets      */
    pthread_mutex_t locks[RC_CONN_POOL_LOCKS];      /**< Striped lock array      */
    uint64_t        count;                          /**< Current connection count */
    uint64_t        max_conn;                       /**< Maximum connections      */
} rc_conn_pool_t;

/* ------------------------------------------------------------------ */
/*  Ring buffer API                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise a ring buffer.
 * @param[out] rb    Ring buffer to initialise.
 * @param      size  Desired capacity (rounded up to next power of 2).
 * @return 0 on success, -1 on allocation failure.
 */
int rc_ringbuf_init(rc_ringbuf_t *rb, uint32_t size);

/**
 * @brief Destroy a ring buffer and free its backing memory.
 * @param rb  Ring buffer to destroy.
 */
void rc_ringbuf_destroy(rc_ringbuf_t *rb);

/**
 * @brief Write data into the ring buffer (producer side).
 * @param rb    Ring buffer.
 * @param data  Source data.
 * @param len   Bytes to write.
 * @return Number of bytes actually written (may be less if full).
 */
size_t rc_ringbuf_write(rc_ringbuf_t *rb, const uint8_t *data, size_t len);

/**
 * @brief Read data from the ring buffer (consumer side).
 * @param rb    Ring buffer.
 * @param dst   Destination buffer.
 * @param len   Maximum bytes to read.
 * @return Number of bytes actually read.
 */
size_t rc_ringbuf_read(rc_ringbuf_t *rb, uint8_t *dst, size_t len);

/**
 * @brief Return the number of bytes available to read.
 * @param rb  Ring buffer.
 * @return Byte count.
 */
static inline uint32_t rc_ringbuf_available(const rc_ringbuf_t *rb)
{
    return rb->head - rb->tail;
}

/**
 * @brief Return the free space in the ring buffer.
 * @param rb  Ring buffer.
 * @return Byte count.
 */
static inline uint32_t rc_ringbuf_free_space(const rc_ringbuf_t *rb)
{
    return rb->size - (rb->head - rb->tail);
}

/* ------------------------------------------------------------------ */
/*  Connection pool API                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the global connection pool.
 * @param[out] pool      Pool to initialise.
 * @param      max_conn  Maximum connections (0 = RC_MAX_CONNECTIONS).
 * @return 0 on success, -1 on failure.
 */
int rc_conn_pool_init(rc_conn_pool_t *pool, uint64_t max_conn);

/**
 * @brief Destroy the connection pool and all contained connections.
 * @param pool  Pool to destroy.
 */
void rc_conn_pool_destroy(rc_conn_pool_t *pool);

/**
 * @brief Create a new connection and insert it into the pool.
 * @param pool    Pool.
 * @param fd      Socket file descriptor.
 * @param conn_id Requested connection ID (0 = auto-assign).
 * @return Pointer to new connection, or NULL on failure.
 */
rc_conn_t *rc_conn_create(rc_conn_pool_t *pool, int fd, uint32_t conn_id);

/**
 * @brief Look up a connection by its ID.
 * @param pool     Pool.
 * @param conn_id  Connection identifier.
 * @return Pointer to connection (refcnt incremented), or NULL if not found.
 */
rc_conn_t *rc_conn_lookup(rc_conn_pool_t *pool, uint32_t conn_id);

/**
 * @brief Release a reference to a connection.
 * @param conn  Connection to release.  If refcnt reaches 0 the connection
 *              is freed and removed from the pool.
 */
void rc_conn_release(rc_conn_t *conn);

/**
 * @brief Transition a connection to a new state.
 * @param conn   Connection.
 * @param state  Target state.
 * @return 0 on success, -1 on invalid transition.
 */
int rc_conn_set_state(rc_conn_t *conn, rc_conn_state_t state);

/**
 * @brief Close and destroy a connection.
 * @param conn  Connection to close.
 */
void rc_conn_close(rc_conn_t *conn);

/**
 * @brief Return a human-readable state name.
 * @param state  Connection state.
 * @return Static string.
 */
const char *rc_conn_state_name(rc_conn_state_t state);

/**
 * @brief Purge connections that have timed out.
 * @param pool         Pool.
 * @param timeout_ms   Idle timeout in milliseconds.
 * @return Number of connections purged.
 */
int rc_conn_purge_timeouts(rc_conn_pool_t *pool, uint32_t timeout_ms);

/**
 * @brief Enable pacing on a connection.
 *
 * Sets the pacer to the given rate.  All subsequent sends through
 * rc_conn_send_paced() will be rate-limited.
 *
 * @param conn      Connection.
 * @param rate_bps  Target rate in bits/sec.
 */
void rc_conn_enable_pacing(rc_conn_t *conn, uint64_t rate_bps);

/**
 * @brief Disable pacing on a connection.
 * @param conn  Connection.
 */
void rc_conn_disable_pacing(rc_conn_t *conn);

/**
 * @brief Send a packet through the connection with optional pacing.
 *
 * If pacing is enabled, blocks until the pacer allows the send.
 * If pacing is disabled, sends immediately (no rate limit).
 *
 * @param conn   Connection (must be ESTABLISHED).
 * @param data   Packet data (header + payload).
 * @param len    Packet length in bytes.
 * @return 0 on success, -1 on error.
 */
int rc_conn_send_paced(rc_conn_t *conn, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RC_CONN_H */
