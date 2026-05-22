/**
 * @file include/rc_relay.h
 * @brief Relay server — session management, bandwidth accounting,
 *        multi-stream forwarding, and routing table.
 */

#ifndef RC_RELAY_H
#define RC_RELAY_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque relay server handle. */
typedef struct rc_relay_server rc_relay_server_t;

/**
 * @brief Create a relay server.
 * @param max_sessions  Maximum concurrent sessions (0 = 100000).
 * @return Pointer to server, or NULL on failure.
 */
rc_relay_server_t *rc_relay_create(uint32_t max_sessions);

/**
 * @brief Destroy a relay server and all sessions.
 * @param srv  Relay server.
 */
void rc_relay_destroy(rc_relay_server_t *srv);

/**
 * @brief Create a relay session between two endpoints.
 * @param srv           Relay server.
 * @param ingress_conn  Source connection ID.
 * @param egress_conn   Destination connection ID.
 * @param ingress_addr  Source address.
 * @param egress_addr   Destination address.
 * @param relay_fd      UDP socket for forwarding.
 * @return Session ID (>0) on success, 0 on failure.
 */
uint32_t rc_relay_session_create(rc_relay_server_t *srv,
                                 uint32_t ingress_conn,
                                 uint32_t egress_conn,
                                 const struct sockaddr *ingress_addr,
                                 const struct sockaddr *egress_addr,
                                 int relay_fd);

/**
 * @brief Destroy a relay session.
 * @param srv        Relay server.
 * @param session_id Session ID to destroy.
 */
void rc_relay_session_destroy(rc_relay_server_t *srv, uint32_t session_id);

/**
 * @brief Forward a packet through the relay.
 * @param srv      Relay server.
 * @param pkt      Raw packet bytes.
 * @param pkt_len  Packet length.
 * @param from     Source address.
 * @param from_len Source address length.
 * @return 0 on success, -1 on drop, -2 on routing error.
 */
int rc_relay_forward(rc_relay_server_t *srv,
                     const uint8_t *pkt, size_t pkt_len,
                     const struct sockaddr *from, socklen_t from_len);

/**
 * @brief Get per-stream statistics.
 * @param srv        Relay server.
 * @param session_id Session ID.
 * @param stream_id  Stream ID (0..15).
 * @param[out] bytes  Bytes forwarded.
 * @param[out] pkts   Packets forwarded.
 * @return 0 on success, -1 if not found.
 */
int rc_relay_stream_stats(rc_relay_server_t *srv, uint32_t session_id,
                          uint32_t stream_id, uint64_t *bytes, uint64_t *pkts);

/**
 * @brief Purge idle relay sessions.
 * @param srv         Relay server.
 * @param timeout_ms  Idle timeout in milliseconds.
 * @return Number of sessions purged.
 */
int rc_relay_purge_idle(rc_relay_server_t *srv, uint64_t timeout_ms);

/**
 * @brief Get aggregate relay statistics.
 * @param srv              Relay server.
 * @param[out] sessions    Active session count.
 * @param[out] forwarded   Total bytes forwarded.
 * @param[out] dropped     Total bytes dropped.
 */
void rc_relay_stats(rc_relay_server_t *srv,
                    uint32_t *sessions, uint64_t *forwarded,
                    uint64_t *dropped);

#ifdef __cplusplus
}
#endif

#endif /* RC_RELAY_H */
