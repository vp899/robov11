#include <stdbool.h>
/**
 * @file rc_p2p.c
 * @brief P2P NAT traversal with STUN-like hole punching and ICE-lite.
 *
 * Implements:
 *   - UDP hole punching (STUN-like binding requests)
 *   - ICE-lite candidate gathering
 *   - Connection probing
 *   - Fallback to relay when direct connection fails
 */

#include "rc_conn.h"
#include "rc_proto.h"
#include "rc_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ================================================================== */
/*  Constants                                                          */
/* ================================================================== */

/** STUN magic cookie (RFC 5389). */
#define STUN_MAGIC              0x2112A442U

/** STUN binding request type. */
#define STUN_BINDING_REQ        0x0001

/** STUN binding response type. */
#define STUN_BINDING_RESP       0x0101

/** STUN attribute: MAPPED-ADDRESS. */
#define STUN_ATTR_MAPPED_ADDR   0x0001

/** STUN attribute: XOR-MAPPED-ADDRESS. */
#define STUN_ATTR_XOR_MAPPED    0x0020

/** Maximum number of ICE candidates. */
#define RC_MAX_CANDIDATES       8

/** Hole punch probe interval in milliseconds. */
#define RC_PUNCH_INTERVAL_MS    500

/** Maximum hole punch attempts before falling back to relay. */
#define RC_PUNCH_MAX_ATTEMPTS   10

/* ================================================================== */
/*  Structures                                                         */
/* ================================================================== */

/**
 * @brief Network address (IPv4 or IPv6).
 */
typedef struct rc_addr {
    struct sockaddr_storage ss;       /**< Socket address storage      */
    socklen_t               ss_len;   /**< Address length              */
    char                    str[64];  /**< Printable representation    */
} rc_addr_t;

/**
 * @brief ICE candidate.
 */
typedef struct rc_candidate {
    rc_addr_t    addr;           /**< Candidate address               */
    uint32_t     priority;       /**< ICE priority                    */
    bool         is_local;       /**< True if host candidate          */
    bool         is_reflexive;   /**< True if server-reflexive (STUN) */
    bool         is_relay;       /**< True if relay candidate         */
} rc_candidate_t;

/**
 * @brief P2P connection state.
 */
typedef struct rc_p2p_state {
    /* --- Candidates --- */
    rc_candidate_t candidates[RC_MAX_CANDIDATES];
    uint32_t       candidate_count;

    /* --- STUN server --- */
    rc_addr_t      stun_server;          /**< STUN server address     */
    int            stun_fd;              /**< UDP socket for STUN      */

    /* --- Hole punching --- */
    int            punch_fd;             /**< UDP socket for punching  */
    uint32_t       punch_attempts;       /**< Number of probes sent    */
    uint64_t       last_punch_ms;        /**< Last probe timestamp     */
    bool           punch_success;        /**< Hole punch succeeded     */

    /* --- Result --- */
    rc_addr_t      peer_addr;            /**< Negotiated peer address  */
    bool           use_relay;            /**< True if relay required   */
    uint32_t       relay_session_id;     /**< Relay session if needed  */
} rc_p2p_state_t;

/* ================================================================== */
/*  STUN protocol                                                      */
/* ================================================================== */

/**
 * @brief Build a STUN binding request.
 *
 * @param[out] buf      Output buffer (at least 28 bytes).
 * @param      buf_len  Buffer size.
 * @param      txn_id   12-byte transaction ID (random).
 * @return Packet length, or -1 on error.
 */
static int stun_build_request(uint8_t *buf, size_t buf_len,
                              const uint8_t *txn_id)
{
    if (buf_len < 28) return -1;

    /* STUN header: type (2) + length (2) + magic (4) + txn_id (12) */
    buf[0] = (STUN_BINDING_REQ >> 8) & 0xFF;
    buf[1] = STUN_BINDING_REQ & 0xFF;
    buf[2] = 0;  /* message length = 0 (no attributes) */
    buf[3] = 0;

    /* Magic cookie */
    buf[4] = (STUN_MAGIC >> 24) & 0xFF;
    buf[5] = (STUN_MAGIC >> 16) & 0xFF;
    buf[6] = (STUN_MAGIC >> 8) & 0xFF;
    buf[7] = STUN_MAGIC & 0xFF;

    /* Transaction ID */
    memcpy(buf + 8, txn_id, 12);

    return 20;  /* STUN header is 20 bytes, but we allocated 28 for safety */
}

/**
 * @brief Parse a STUN binding response.
 *
 * @param buf       Response bytes.
 * @param buf_len   Response length.
 * @param[out] addr  Extracted mapped address.
 * @return 0 on success, -1 on parse error.
 */
static int __attribute__((unused)) stun_parse_response(const uint8_t *buf, size_t buf_len,
                               rc_addr_t *addr)
{
    if (buf_len < 20) return -1;

    uint16_t msg_type = (buf[0] << 8) | buf[1];
    uint16_t msg_len  = (buf[2] << 8) | buf[3];
    uint32_t magic    = ((uint32_t)buf[4] << 24) |
                        ((uint32_t)buf[5] << 16) |
                        ((uint32_t)buf[6] << 8)  |
                        buf[7];

    if (msg_type != STUN_BINDING_RESP) return -1;
    if (magic != STUN_MAGIC) return -1;
    if (buf_len < (size_t)(20 + msg_len)) return -1;

    /* Scan attributes for XOR-MAPPED-ADDRESS */
    const uint8_t *attr = buf + 20;
    const uint8_t *end = buf + 20 + msg_len;

    while (attr + 4 <= end) {
        uint16_t attr_type = (attr[0] << 8) | attr[1];
        uint16_t attr_len  = (attr[2] << 8) | attr[3];

        if (attr + 4 + attr_len > end) break;

        if ((attr_type == STUN_ATTR_XOR_MAPPED ||
             attr_type == STUN_ATTR_MAPPED_ADDR) && attr_len >= 8) {
            uint8_t family = attr[5];
            uint16_t port;
            uint32_t ip;

            if (family == 0x01 && attr_len >= 8) {
                /* IPv4 */
                port = (uint16_t)((attr[6] << 8) | attr[7]);
                ip   = ((uint32_t)attr[8] << 24) |
                       ((uint32_t)attr[9] << 16) |
                       ((uint32_t)attr[10] << 8) |
                       attr[11];

                if (attr_type == STUN_ATTR_XOR_MAPPED) {
                    port ^= (STUN_MAGIC >> 16);
                    ip   ^= STUN_MAGIC;
                }

                struct sockaddr_in *sin = (struct sockaddr_in *)&addr->ss;
                sin->sin_family = AF_INET;
                sin->sin_port = htons(port);
                sin->sin_addr.s_addr = htonl(ip);
                addr->ss_len = sizeof(*sin);
                inet_ntop(AF_INET, &sin->sin_addr, addr->str, sizeof(addr->str));
                return 0;
            }
        }

        /* Advance to next attribute (padded to 4 bytes) */
        attr += 4 + ((attr_len + 3) & ~3);
    }

    return -1;  /* no mapped address found */
}

/* ================================================================== */
/*  Candidate gathering                                                */
/* ================================================================== */

/**
 * @brief Gather local host candidates.
 *
 * Binds a UDP socket to discover local addresses.
 *
 * @param p2p  P2P state.
 * @return Number of candidates gathered.
 */
static int gather_host_candidates(rc_p2p_state_t *p2p)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        syslog(LOG_ERR, "p2p: socket() failed: %s", strerror(errno));
        return 0;
    }

    /* Bind to any address, ephemeral port */
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = 0;

    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        syslog(LOG_ERR, "p2p: bind() failed: %s", strerror(errno));
        close(fd);
        return 0;
    }

    /* Get the bound address */
    socklen_t slen = sizeof(sin);
    getsockname(fd, (struct sockaddr *)&sin, &slen);

    if (p2p->candidate_count < RC_MAX_CANDIDATES) {
        rc_candidate_t *c = &p2p->candidates[p2p->candidate_count++];
        memset(c, 0, sizeof(*c));
        memcpy(&c->addr.ss, &sin, sizeof(sin));
        c->addr.ss_len = sizeof(sin);
        inet_ntop(AF_INET, &sin.sin_addr, c->addr.str, sizeof(c->addr.str));
        c->priority = 100;
        c->is_local = true;
        syslog(LOG_INFO, "p2p: host candidate %s:%u",
               c->addr.str, ntohs(sin.sin_port));
    }

    p2p->punch_fd = fd;
    return 1;
}

/**
 * @brief Gather server-reflexive candidates via STUN.
 *
 * @param p2p  P2P state.
 * @return Number of reflexive candidates gathered.
 */
static int gather_srflx_candidates(rc_p2p_state_t *p2p)
{
    if (p2p->stun_server.ss_len == 0) {
        syslog(LOG_DEBUG, "p2p: no STUN server configured");
        return 0;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    /* Set non-blocking for timeout handling */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* Send STUN binding request */
    uint8_t txn_id[12];
    rc_random_bytes(txn_id, sizeof(txn_id));

    uint8_t req[28];
    int req_len = stun_build_request(req, sizeof(req), txn_id);
    if (req_len < 0) {
        close(fd);
        return 0;
    }

    sendto(fd, req, (size_t)req_len, 0,
           (struct sockaddr *)&p2p->stun_server.ss,
           p2p->stun_server.ss_len);

    p2p->stun_fd = fd;
    syslog(LOG_DEBUG, "p2p: STUN request sent to %s", p2p->stun_server.str);
    return 1;  /* response processed asynchronously */
}

/* ================================================================== */
/*  Hole punching                                                      */
/* ================================================================== */

/**
 * @brief Send a hole-punch probe to the peer.
 *
 * Sends a small UDP packet to the peer's reflexive address to
 * create a NAT mapping.  The packet is a RoboControl PING.
 *
 * @param p2p       P2P state.
 * @param peer_addr Peer's server-reflexive address.
 * @return 0 on success, -1 on error.
 */
static int send_punch_probe(rc_p2p_state_t *p2p, const rc_addr_t *peer_addr)
{
    if (p2p->punch_fd < 0) return -1;

    /* Build a PING packet */
    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_PING, 0, 0);
    hdr.payload_len = 0;

    uint8_t buf[RC_HEADER_SIZE];
    rc_hdr_pack(buf, &hdr);

    ssize_t sent = sendto(p2p->punch_fd, buf, sizeof(buf), 0,
                          (struct sockaddr *)&peer_addr->ss,
                          peer_addr->ss_len);
    if (sent < 0) {
        syslog(LOG_WARNING, "p2p: punch probe send failed: %s",
               strerror(errno));
        return -1;
    }

    p2p->punch_attempts++;
    p2p->last_punch_ms = rc_time_ms();

    syslog(LOG_DEBUG, "p2p: punch probe #%u sent to %s",
           p2p->punch_attempts, peer_addr->str);
    return 0;
}

/* ================================================================== */
/*  Connection probing                                                 */
/* ================================================================== */

/**
 * @brief Probe a candidate to check connectivity.
 *
 * Sends a PING and waits for a PONG.  If a response arrives within
 * the timeout, the candidate is viable.
 *
 * @param p2p       P2P state.
 * @param candidate Candidate to probe.
 * @param timeout_ms  Probe timeout in milliseconds.
 * @return RTT in milliseconds on success, 0 on timeout.
 */
uint32_t rc_p2p_probe(rc_p2p_state_t *p2p, const rc_candidate_t *candidate,
                      uint32_t timeout_ms)
{
    if (!p2p || !candidate) return 0;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    /* Set receive timeout */
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Send PING */
    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_PING, 0, 0);
    hdr.payload_len = 0;

    uint8_t buf[RC_HEADER_SIZE];
    rc_hdr_pack(buf, &hdr);

    uint64_t send_ts = rc_time_ms();
    sendto(fd, buf, sizeof(buf), 0,
           (struct sockaddr *)&candidate->addr.ss,
           candidate->addr.ss_len);

    /* Wait for PONG */
    uint8_t recv_buf[RC_HEADER_SIZE + RC_MAX_PAYLOAD];
    ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);
    close(fd);

    if (n >= (ssize_t)RC_HEADER_SIZE) {
        rc_pkt_hdr_t resp;
        if (rc_hdr_unpack(&resp, recv_buf) == 0 &&
            resp.type == RC_PKT_PONG) {
            uint64_t rtt = rc_time_ms() - send_ts;
            syslog(LOG_DEBUG, "p2p: probe to %s succeeded (rtt=%lu ms)",
                   candidate->addr.str, rtt);
            return (uint32_t)rtt;
        }
    }

    return 0;  /* timeout */
}

/* ================================================================== */
/*  Fallback to relay                                                  */
/* ================================================================== */

/**
 * @brief Fall back to relay when direct connection fails.
 *
 * Sends a RELAY_REQ packet through the signaling server.
 *
 * @param conn      Connection to relay.
 * @param p2p       P2P state.
 * @return 0 on success, -1 on failure.
 */
int rc_p2p_fallback_relay(rc_conn_t *conn, rc_p2p_state_t *p2p)
{
    if (!conn || !p2p) return -1;

    p2p->use_relay = true;

    /* Build RELAY_REQ */
    rc_pkt_hdr_t hdr;
    rc_hdr_init(&hdr, RC_PKT_RELAY_REQ, conn->local_seq++, conn->conn_id);
    hdr.payload_len = 0;
    hdr.flags = RC_FLAG_RELIABLE;

    uint8_t buf[RC_HEADER_SIZE];
    rc_hdr_pack(buf, &hdr);

    size_t written = rc_ringbuf_write(&conn->send_buf, buf, sizeof(buf));
    if (written != sizeof(buf)) {
        syslog(LOG_ERR, "p2p: failed to queue RELAY_REQ");
        return -1;
    }

    syslog(LOG_INFO, "p2p: falling back to relay for conn_id=%u",
           conn->conn_id);
    return 0;
}

/* ================================================================== */
/*  P2P state management                                               */
/* ================================================================== */

/**
 * @brief Create and initialise P2P state.
 * @return Pointer to P2P state, or NULL on failure.
 */
rc_p2p_state_t *rc_p2p_create(void)
{
    rc_p2p_state_t *p2p = (rc_p2p_state_t *)calloc(1, sizeof(*p2p));
    if (!p2p) return NULL;

    p2p->stun_fd = -1;
    p2p->punch_fd = -1;
    p2p->punch_success = false;
    p2p->use_relay = false;

    /* Gather host candidates */
    gather_host_candidates(p2p);

    return p2p;
}

/**
 * @brief Destroy P2P state and close sockets.
 * @param p2p  P2P state.
 */
void rc_p2p_destroy(rc_p2p_state_t *p2p)
{
    if (!p2p) return;
    if (p2p->stun_fd >= 0) close(p2p->stun_fd);
    if (p2p->punch_fd >= 0) close(p2p->punch_fd);
    free(p2p);
}

/**
 * @brief Configure the STUN server for reflexive candidate gathering.
 * @param p2p       P2P state.
 * @param host      STUN server hostname or IP.
 * @param port      STUN server port (typically 3478).
 * @return 0 on success, -1 on failure.
 */
int rc_p2p_set_stun(rc_p2p_state_t *p2p, const char *host, uint16_t port)
{
    if (!p2p || !host) return -1;

    struct sockaddr_in *sin = (struct sockaddr_in *)&p2p->stun_server.ss;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    p2p->stun_server.ss_len = sizeof(*sin);

    if (inet_pton(AF_INET, host, &sin->sin_addr) != 1) {
        syslog(LOG_ERR, "p2p: invalid STUN server address: %s", host);
        return -1;
    }

    inet_ntop(AF_INET, &sin->sin_addr, p2p->stun_server.str,
              sizeof(p2p->stun_server.str));

    /* Trigger STUN candidate gathering */
    gather_srflx_candidates(p2p);

    return 0;
}

/**
 * @brief Run the hole-punching process.
 *
 * Called periodically from the event loop.  Sends probes to the
 * peer's reflexive address and listens for responses.
 *
 * @param p2p       P2P state.
 * @param peer_addr Peer's address (from signaling).
 * @return true if hole punch succeeded or relay fallback needed.
 */
bool rc_p2p_tick(rc_p2p_state_t *p2p, const rc_addr_t *peer_addr)
{
    if (!p2p || !peer_addr) return false;

    /* Already done */
    if (p2p->punch_success || p2p->use_relay) return true;

    /* Check if we've exceeded max attempts */
    if (p2p->punch_attempts >= RC_PUNCH_MAX_ATTEMPTS) {
        syslog(LOG_WARNING, "p2p: hole punch failed after %u attempts",
               p2p->punch_attempts);
        return false;  /* caller should fallback to relay */
    }

    /* Rate-limit probes */
    uint64_t now = rc_time_ms();
    if (now - p2p->last_punch_ms < RC_PUNCH_INTERVAL_MS) {
        return false;  /* not time yet */
    }

    /* Send probe */
    send_punch_probe(p2p, peer_addr);

    /* Check for incoming data on the punch socket */
    uint8_t buf[RC_HEADER_SIZE + 64];
    struct sockaddr_storage from;
    socklen_t from_len = sizeof(from);

    ssize_t n = recvfrom(p2p->punch_fd, buf, sizeof(buf), MSG_DONTWAIT,
                         (struct sockaddr *)&from, &from_len);
    if (n >= (ssize_t)RC_HEADER_SIZE) {
        rc_pkt_hdr_t hdr;
        if (rc_hdr_unpack(&hdr, buf) == 0 &&
            (hdr.type == RC_PKT_PING || hdr.type == RC_PKT_PONG)) {
            /* Hole punch succeeded — we received from the peer */
            p2p->punch_success = true;
            memcpy(&p2p->peer_addr.ss, &from, from_len);
            p2p->peer_addr.ss_len = from_len;
            inet_ntop(from.ss_family,
                      &((struct sockaddr_in *)&from)->sin_addr,
                      p2p->peer_addr.str, sizeof(p2p->peer_addr.str));
            syslog(LOG_INFO, "p2p: hole punch succeeded! peer=%s",
                   p2p->peer_addr.str);
            return true;
        }
    }

    return false;
}
