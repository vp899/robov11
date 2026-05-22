#include <stdbool.h>
/**
 * @file rc_relay.c
 * @brief Relay server — session management, bandwidth accounting,
 *        multi-stream forwarding, and routing table.
 *
 * When a direct P2P connection cannot be established (NAT type,
 * firewall, etc.), the relay server acts as a transparent forwarder
 * between two endpoints.  It maintains per-session bandwidth counters
 * and supports multiplexing multiple logical streams (e.g., camera
 * feeds, telemetry, control commands) over a single relay session.
 *
 * Architecture:
 *   Client A ──UDP──▸ Relay ──UDP──▸ Client B
 *
 * The relay does NOT decrypt or inspect payload; it merely forwards
 * packets based on the conn_id routing table.
 */

#include "rc_conn.h"
#include "rc_relay.h"
#include "rc_internal.h"
#include "rc_proto.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/* ================================================================== */
/*  Constants                                                          */
/* ================================================================== */

/** Maximum concurrent relay sessions. */
#define RC_RELAY_MAX_SESSIONS       100000

/** Maximum streams per relay session. */
#define RC_RELAY_MAX_STREAMS        16

/** Bandwidth accounting interval in seconds. */
#define RC_RELAY_BW_INTERVAL_SEC    1

/** Per-session bandwidth limit in bytes/sec (default 50 Mbps). */
#define RC_RELAY_BW_LIMIT_BPS       (50 * 1024 * 1024 / 8)

/** Relay routing table buckets. */
#define RC_RELAY_ROUTE_BUCKETS      65537

/* ================================================================== */
/*  Structures                                                         */
/* ================================================================== */

/**
 * @brief Per-stream bandwidth and statistics.
 */
typedef struct rc_relay_stream {
    uint32_t    stream_id;          /**< Logical stream identifier      */
    uint64_t    bytes_forwarded;    /**< Total bytes forwarded          */
    uint64_t    pkts_forwarded;     /**< Total packets forwarded        */
    uint64_t    bytes_dropped;      /**< Bytes dropped (over limit)     */
    uint32_t    current_bps;        /**< Current throughput (bytes/sec) */
    bool        active;             /**< Stream is active               */
} rc_relay_stream_t;

/**
 * @brief Relay session — connects two endpoints.
 *
 * Each session has an ingress side (source) and an egress side
 * (destination).  The relay forwards packets from ingress to egress.
 */
typedef struct rc_relay_session {
    uint32_t    session_id;                         /**< Unique session ID     */
    uint32_t    ingress_conn_id;                    /**< Source connection ID   */
    uint32_t    egress_conn_id;                     /**< Destination conn ID    */

    struct sockaddr_storage ingress_addr;            /**< Source address         */
    socklen_t               ingress_addr_len;
    struct sockaddr_storage egress_addr;             /**< Destination address    */
    socklen_t               egress_addr_len;

    int         relay_fd;                           /**< Relay UDP socket       */

    /* --- Streams --- */
    rc_relay_stream_t streams[RC_RELAY_MAX_STREAMS]; /**< Per-stream stats     */
    uint32_t    stream_count;                        /**< Active stream count   */

    /* --- Bandwidth --- */
    uint64_t    bw_bytes_this_sec;                   /**< Bytes in current sec  */
    uint64_t    bw_limit_bps;                        /**< Bandwidth limit       */
    uint64_t    bw_window_start_ms;                  /**< BW window start       */

    /* --- State --- */
    bool        active;                              /**< Session is active     */
    uint64_t    created_ms;                          /**< Creation timestamp    */
    uint64_t    last_activity_ms;                    /**< Last forward time     */

    /* --- Aggregate stats --- */
    uint64_t    bytes_forwarded;                     /**< Total bytes forwarded */
    uint64_t    pkts_forwarded;                      /**< Total pkts forwarded  */
    uint64_t    bytes_dropped;                       /**< Bytes dropped (limit) */

    /* --- Linkage --- */
    struct rc_relay_session *next;                   /**< Hash chain            */
} rc_relay_session_t;

/**
 * @brief Relay routing table entry.
 *
 * Maps a conn_id to a relay session + direction.
 */
typedef struct rc_relay_route {
    uint32_t              conn_id;     /**< Connection identifier         */
    rc_relay_session_t   *session;     /**< Owning session                */
    bool                  is_ingress;  /**< True if this is the source    */
    struct rc_relay_route *next;       /**< Hash chain                    */
} rc_relay_route_t;

/**
 * @brief Relay server state.
 */
typedef struct rc_relay_server {
    /* --- Routing table --- */
    rc_relay_route_t  *routes[RC_RELAY_ROUTE_BUCKETS];  /**< Route hash table */
    pthread_mutex_t    route_lock;                       /**< Route table lock  */

    /* --- Session pool --- */
    rc_relay_session_t *sessions;          /**< Session array             */
    uint32_t            session_count;     /**< Active sessions           */
    uint32_t            max_sessions;      /**< Maximum sessions          */
    pthread_mutex_t     session_lock;      /**< Session pool lock         */

    /* --- Socket --- */
    int                 listen_fd;         /**< Relay listen socket       */

    /* --- Statistics --- */
    uint64_t            total_forwarded;   /**< Total bytes forwarded     */
    uint64_t            total_dropped;     /**< Total bytes dropped       */
    uint64_t            total_sessions;    /**< Sessions created (total)  */
} rc_relay_server_t;

/* ================================================================== */
/*  Hash helpers                                                       */
/* ================================================================== */

/**
 * @brief Fast hash for 32-bit keys (FNV-1a variant).
 */
static inline uint32_t relay_hash(uint32_t key)
{
    uint32_t h = 2166136261u;
    h ^= (key >> 16); h *= 16777619u;
    h ^= (key & 0xFFFF); h *= 16777619u;
    return h;
}

/* ================================================================== */
/*  Session management                                                 */
/* ================================================================== */

/**
 * @brief Create a relay server.
 * @param max_sessions  Maximum concurrent sessions (0 = default).
 * @return Pointer to server, or NULL on failure.
 */
rc_relay_server_t *rc_relay_create(uint32_t max_sessions)
{
    if (max_sessions == 0) max_sessions = RC_RELAY_MAX_SESSIONS;

    rc_relay_server_t *srv = (rc_relay_server_t *)calloc(1, sizeof(*srv));
    if (!srv) {
        syslog(LOG_ERR, "relay: alloc failed");
        return NULL;
    }

    srv->max_sessions = max_sessions;
    srv->sessions = (rc_relay_session_t *)calloc(max_sessions,
                                                  sizeof(rc_relay_session_t));
    if (!srv->sessions) {
        syslog(LOG_ERR, "relay: session array alloc failed (%u entries)",
               max_sessions);
        free(srv);
        return NULL;
    }

    if (pthread_mutex_init(&srv->route_lock, NULL) != 0 ||
        pthread_mutex_init(&srv->session_lock, NULL) != 0) {
        free(srv->sessions);
        free(srv);
        return NULL;
    }

    srv->listen_fd = -1;

    syslog(LOG_INFO, "relay: server created (max_sessions=%u)", max_sessions);
    return srv;
}

/**
 * @brief Destroy a relay server and all sessions.
 * @param srv  Relay server.
 */
void rc_relay_destroy(rc_relay_server_t *srv)
{
    if (!srv) return;

    /* Free route table entries */
    pthread_mutex_lock(&srv->route_lock);
    for (uint32_t i = 0; i < RC_RELAY_ROUTE_BUCKETS; i++) {
        rc_relay_route_t *r = srv->routes[i];
        while (r) {
            rc_relay_route_t *next = r->next;
            free(r);
            r = next;
        }
    }
    pthread_mutex_unlock(&srv->route_lock);

    /* Close sockets */
    if (srv->listen_fd >= 0) close(srv->listen_fd);

    pthread_mutex_destroy(&srv->route_lock);
    pthread_mutex_destroy(&srv->session_lock);
    free(srv->sessions);
    free(srv);

    syslog(LOG_INFO, "relay: server destroyed");
}

/* ================================================================== */
/*  Route table                                                        */
/* ================================================================== */

/**
 * @brief Add a route mapping a conn_id to a relay session.
 *
 * @param srv       Relay server.
 * @param conn_id   Connection ID to map.
 * @param session   Relay session.
 * @param ingress   True if this conn_id is the ingress (source).
 * @return 0 on success, -1 on failure.
 */
static int route_add(rc_relay_server_t *srv, uint32_t conn_id,
                     rc_relay_session_t *session, bool ingress)
{
    rc_relay_route_t *route = (rc_relay_route_t *)calloc(1, sizeof(*route));
    if (!route) return -1;

    route->conn_id    = conn_id;
    route->session    = session;
    route->is_ingress = ingress;

    uint32_t bucket = relay_hash(conn_id) % RC_RELAY_ROUTE_BUCKETS;

    pthread_mutex_lock(&srv->route_lock);
    route->next = srv->routes[bucket];
    srv->routes[bucket] = route;
    pthread_mutex_unlock(&srv->route_lock);

    return 0;
}

/**
 * @brief Look up a route by conn_id.
 *
 * @param srv      Relay server.
 * @param conn_id  Connection ID to look up.
 * @return Route entry, or NULL if not found.
 */
static rc_relay_route_t *route_lookup(rc_relay_server_t *srv, uint32_t conn_id)
{
    uint32_t bucket = relay_hash(conn_id) % RC_RELAY_ROUTE_BUCKETS;

    pthread_mutex_lock(&srv->route_lock);
    rc_relay_route_t *r = srv->routes[bucket];
    while (r) {
        if (r->conn_id == conn_id) {
            pthread_mutex_unlock(&srv->route_lock);
            return r;
        }
        r = r->next;
    }
    pthread_mutex_unlock(&srv->route_lock);
    return NULL;
}

/**
 * @brief Remove all routes for a given conn_id.
 *
 * @param srv      Relay server.
 * @param conn_id  Connection ID.
 */
static void route_remove(rc_relay_server_t *srv, uint32_t conn_id)
{
    uint32_t bucket = relay_hash(conn_id) % RC_RELAY_ROUTE_BUCKETS;

    pthread_mutex_lock(&srv->route_lock);
    rc_relay_route_t **pp = &srv->routes[bucket];
    while (*pp) {
        if ((*pp)->conn_id == conn_id) {
            rc_relay_route_t *victim = *pp;
            *pp = victim->next;
            free(victim);
        } else {
            pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&srv->route_lock);
}

/* ================================================================== */
/*  Session creation / teardown                                        */
/* ================================================================== */

/**
 * @brief Create a relay session between two endpoints.
 *
 * @param srv           Relay server.
 * @param ingress_conn  Source connection ID.
 * @param egress_conn   Destination connection ID.
 * @param ingress_addr  Source address.
 * @param egress_addr   Destination address.
 * @param relay_fd      UDP socket to use for forwarding.
 * @return Session ID (>0) on success, 0 on failure.
 */
uint32_t rc_relay_session_create(rc_relay_server_t *srv,
                                 uint32_t ingress_conn,
                                 uint32_t egress_conn,
                                 const struct sockaddr *ingress_addr,
                                 const struct sockaddr *egress_addr,
                                 int relay_fd)
{
    if (!srv) return 0;

    pthread_mutex_lock(&srv->session_lock);

    if (srv->session_count >= srv->max_sessions) {
        pthread_mutex_unlock(&srv->session_lock);
        syslog(LOG_WARNING, "relay: session pool full");
        return 0;
    }

    /* Find a free slot */
    rc_relay_session_t *sess = NULL;
    for (uint32_t i = 0; i < srv->max_sessions; i++) {
        if (!srv->sessions[i].active) {
            sess = &srv->sessions[i];
            break;
        }
    }

    if (!sess) {
        pthread_mutex_unlock(&srv->session_lock);
        return 0;
    }

    /* Initialise session */
    memset(sess, 0, sizeof(*sess));
    sess->session_id = ++srv->total_sessions;  /* monotonically increasing */
    sess->ingress_conn_id = ingress_conn;
    sess->egress_conn_id  = egress_conn;
    sess->relay_fd = relay_fd;
    sess->active = true;
    sess->created_ms = rc_time_ms();
    sess->last_activity_ms = sess->created_ms;
    sess->bw_limit_bps = RC_RELAY_BW_LIMIT_BPS;
    sess->bw_window_start_ms = sess->created_ms;

    /* Copy addresses */
    if (ingress_addr) {
        socklen_t len = (ingress_addr->sa_family == AF_INET)
                            ? sizeof(struct sockaddr_in)
                            : sizeof(struct sockaddr_in6);
        memcpy(&sess->ingress_addr, ingress_addr, len);
        sess->ingress_addr_len = len;
    }
    if (egress_addr) {
        socklen_t len = (egress_addr->sa_family == AF_INET)
                            ? sizeof(struct sockaddr_in)
                            : sizeof(struct sockaddr_in6);
        memcpy(&sess->egress_addr, egress_addr, len);
        sess->egress_addr_len = len;
    }

    srv->session_count++;
    pthread_mutex_unlock(&srv->session_lock);

    /* Add routes */
    route_add(srv, ingress_conn, sess, true);
    route_add(srv, egress_conn, sess, false);

    syslog(LOG_INFO, "relay: session %u created (%u → %u)",
           sess->session_id, ingress_conn, egress_conn);
    return sess->session_id;
}

/**
 * @brief Destroy a relay session.
 *
 * @param srv        Relay server.
 * @param session_id Session ID to destroy.
 */
void rc_relay_session_destroy(rc_relay_server_t *srv, uint32_t session_id)
{
    if (!srv) return;

    pthread_mutex_lock(&srv->session_lock);

    for (uint32_t i = 0; i < srv->max_sessions; i++) {
        rc_relay_session_t *sess = &srv->sessions[i];
        if (sess->active && sess->session_id == session_id) {
            /* Remove routes */
            route_remove(srv, sess->ingress_conn_id);
            route_remove(srv, sess->egress_conn_id);

            sess->active = false;
            srv->session_count--;

            syslog(LOG_INFO, "relay: session %u destroyed (fwd=%lu drop=%lu)",
                   session_id, sess->bytes_forwarded, sess->bytes_dropped);

            pthread_mutex_unlock(&srv->session_lock);
            return;
        }
    }

    pthread_mutex_unlock(&srv->session_lock);
}

/* ================================================================== */
/*  Bandwidth accounting                                               */
/* ================================================================== */

/**
 * @brief Check if a packet is within the bandwidth limit.
 *
 * Uses a 1-second sliding window.  If the limit is exceeded, the
 * packet is dropped and the drop counter is incremented.
 *
 * @param sess     Relay session.
 * @param pkt_len  Packet size in bytes.
 * @return true if the packet should be forwarded, false if dropped.
 */
static bool bw_check(rc_relay_session_t *sess, uint32_t pkt_len)
{
    uint64_t now = rc_time_ms();

    /* Reset window every second */
    if (now - sess->bw_window_start_ms >= RC_RELAY_BW_INTERVAL_SEC * 1000) {
        sess->bw_bytes_this_sec = 0;
        sess->bw_window_start_ms = now;
    }

    if (sess->bw_bytes_this_sec + pkt_len > sess->bw_limit_bps) {
        sess->bytes_dropped += pkt_len;
        return false;  /* over limit */
    }

    sess->bw_bytes_this_sec += pkt_len;
    return true;
}

/* ================================================================== */
/*  Packet forwarding                                                  */
/* ================================================================== */

/**
 * @brief Forward a packet through the relay.
 *
 * Looks up the routing table by conn_id and forwards the packet to
 * the opposite endpoint.  Applies bandwidth accounting.
 *
 * @param srv      Relay server.
 * @param pkt      Raw packet bytes (header + payload).
 * @param pkt_len  Packet length.
 * @param from     Source address.
 * @param from_len Source address length.
 * @return 0 on success, -1 on drop, -2 on routing error.
 */
int rc_relay_forward(rc_relay_server_t *srv,
                     const uint8_t *pkt, size_t pkt_len,
                     const struct sockaddr *from, socklen_t from_len)
{
    if (!srv || !pkt || pkt_len < RC_HEADER_SIZE) return -1;
    (void)from; (void)from_len;

    /* Parse header to get conn_id */
    rc_pkt_hdr_t hdr;
    if (rc_hdr_unpack(&hdr, pkt) != 0) return -1;

    /* Look up route */
    rc_relay_route_t *route = route_lookup(srv, hdr.conn_id);
    if (!route) {
        syslog(LOG_DEBUG, "relay: no route for conn_id=%u", hdr.conn_id);
        return -2;
    }

    rc_relay_session_t *sess = route->session;
    if (!sess || !sess->active) return -2;

    /* Bandwidth check */
    if (!bw_check(sess, (uint32_t)pkt_len)) {
        return -1;  /* dropped */
    }

    /* Determine destination */
    struct sockaddr *dst;
    socklen_t dst_len;

    if (route->is_ingress) {
        /* Forward ingress → egress */
        dst = (struct sockaddr *)&sess->egress_addr;
        dst_len = sess->egress_addr_len;
    } else {
        /* Forward egress → ingress */
        dst = (struct sockaddr *)&sess->ingress_addr;
        dst_len = sess->ingress_addr_len;
    }

    /* Update conn_id in the forwarded packet to match the destination */
    uint8_t fwd_buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
    memcpy(fwd_buf, pkt, pkt_len);

    rc_pkt_hdr_t *fwd_hdr = (rc_pkt_hdr_t *)fwd_buf;
    fwd_hdr->conn_id = route->is_ingress ? sess->egress_conn_id
                                          : sess->ingress_conn_id;

    /* Re-pack the header with the new conn_id */
    rc_hdr_pack(fwd_buf, fwd_hdr);

    /* Send */
    ssize_t sent = sendto(sess->relay_fd, fwd_buf, pkt_len, 0,
                          dst, dst_len);
    if (sent < 0) {
        syslog(LOG_WARNING, "relay: sendto failed: %s", strerror(errno));
        return -1;
    }

    /* Update statistics */
    sess->bytes_forwarded += (uint64_t)pkt_len;
    sess->pkts_forwarded++;
    sess->last_activity_ms = rc_time_ms();
    srv->total_forwarded += (uint64_t)pkt_len;

    /* Update per-stream stats */
    /* Stream ID is encoded in the lower 16 bits of flags */
    uint32_t stream_id = hdr.flags & 0x00FF;
    if (stream_id < RC_RELAY_MAX_STREAMS) {
        rc_relay_stream_t *s = &sess->streams[stream_id];
        if (!s->active) {
            s->stream_id = stream_id;
            s->active = true;
            sess->stream_count++;
        }
        s->bytes_forwarded += (uint64_t)pkt_len;
        s->pkts_forwarded++;
    }

    return 0;
}

/* ================================================================== */
/*  Multi-stream support                                               */
/* ================================================================== */

/**
 * @brief Get statistics for a specific stream within a session.
 *
 * @param srv        Relay server.
 * @param session_id Session ID.
 * @param stream_id  Stream ID (0..15).
 * @param[out] bytes  Bytes forwarded on this stream.
 * @param[out] pkts   Packets forwarded on this stream.
 * @return 0 on success, -1 if not found.
 */
int rc_relay_stream_stats(rc_relay_server_t *srv, uint32_t session_id,
                          uint32_t stream_id, uint64_t *bytes, uint64_t *pkts)
{
    if (!srv || stream_id >= RC_RELAY_MAX_STREAMS) return -1;

    pthread_mutex_lock(&srv->session_lock);
    for (uint32_t i = 0; i < srv->max_sessions; i++) {
        rc_relay_session_t *sess = &srv->sessions[i];
        if (sess->active && sess->session_id == session_id) {
            rc_relay_stream_t *s = &sess->streams[stream_id];
            if (bytes) *bytes = s->bytes_forwarded;
            if (pkts)  *pkts  = s->pkts_forwarded;
            pthread_mutex_unlock(&srv->session_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&srv->session_lock);
    return -1;
}

/* ================================================================== */
/*  Timeout / cleanup                                                  */
/* ================================================================== */

/**
 * @brief Purge relay sessions that have been idle too long.
 *
 * @param srv         Relay server.
 * @param timeout_ms  Idle timeout in milliseconds.
 * @return Number of sessions purged.
 */
int rc_relay_purge_idle(rc_relay_server_t *srv, uint64_t timeout_ms)
{
    if (!srv) return 0;

    int purged = 0;
    uint64_t now = rc_time_ms();

    pthread_mutex_lock(&srv->session_lock);
    for (uint32_t i = 0; i < srv->max_sessions; i++) {
        rc_relay_session_t *sess = &srv->sessions[i];
        if (sess->active &&
            (now - sess->last_activity_ms) > timeout_ms) {

            route_remove(srv, sess->ingress_conn_id);
            route_remove(srv, sess->egress_conn_id);

            syslog(LOG_INFO, "relay: session %u purged (idle %lu ms)",
                   sess->session_id, now - sess->last_activity_ms);

            sess->active = false;
            srv->session_count--;
            purged++;
        }
    }
    pthread_mutex_unlock(&srv->session_lock);

    return purged;
}

/**
 * @brief Get aggregate relay statistics.
 *
 * @param srv              Relay server.
 * @param[out] sessions    Current active session count.
 * @param[out] forwarded   Total bytes forwarded.
 * @param[out] dropped     Total bytes dropped.
 */
void rc_relay_stats(rc_relay_server_t *srv,
                    uint32_t *sessions, uint64_t *forwarded,
                    uint64_t *dropped)
{
    if (!srv) return;
    if (sessions) *sessions = srv->session_count;
    if (forwarded) *forwarded = srv->total_forwarded;
    if (dropped) *dropped = srv->total_dropped;
}
