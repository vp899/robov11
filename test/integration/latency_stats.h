/**
 * @file latency_stats.h
 * @brief Per-packet latency recording and statistical analysis.
 *
 * Records every packet's send/receive timestamps, computes per-packet
 * delay, and reports mean, stddev, min, max, percentiles.
 */

#ifndef LATENCY_STATS_H
#define LATENCY_STATS_H

#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LAT_MAX_SAMPLES  200000   /**< Max packets to record (3 min @ ~1100 pps) */

/**
 * @brief Single packet latency record.
 */
typedef struct {
    uint32_t seq;            /**< Packet sequence number */
    uint16_t stream_id;      /**< Stream identifier */
    uint64_t send_ts_us;     /**< Sender timestamp (µs) */
    uint64_t recv_ts_us;     /**< Receiver timestamp (µs) */
    uint32_t payload_len;    /**< Payload size */
} latency_record_t;

/**
 * @brief Latency statistics collector.
 */
typedef struct {
    latency_record_t records[LAT_MAX_SAMPLES];
    uint32_t         count;
    uint64_t         first_send_us;
    uint64_t         last_recv_us;
    uint32_t         lost_packets;   /**< seq gaps detected */
} latency_stats_t;

/**
 * @brief Initialize latency stats.
 */
static inline void lat_init(latency_stats_t *s)
{
    memset(s, 0, sizeof(*s));
}

/**
 * @brief Record a packet's latency.
 *
 * @param s           Stats collector.
 * @param seq         Packet sequence number.
 * @param stream_id   Stream ID.
 * @param send_ts_us  Sender timestamp in µs (from packet header).
 * @param recv_ts_us  Receiver timestamp in µs (local clock).
 * @param payload_len Payload length.
 */
static inline void lat_record(latency_stats_t *s, uint32_t seq, uint16_t stream_id,
                               uint64_t send_ts_us, uint64_t recv_ts_us,
                               uint32_t payload_len)
{
    if (s->count >= LAT_MAX_SAMPLES) return;

    latency_record_t *r = &s->records[s->count++];
    r->seq         = seq;
    r->stream_id   = stream_id;
    r->send_ts_us  = send_ts_us;
    r->recv_ts_us  = recv_ts_us;
    r->payload_len = payload_len;

    if (s->first_send_us == 0 || send_ts_us < s->first_send_us)
        s->first_send_us = send_ts_us;
    if (recv_ts_us > s->last_recv_us)
        s->last_recv_us = recv_ts_us;
}

/**
 * @brief Get delay in µs for a record.
 */
static inline double lat_delay_us(const latency_record_t *r)
{
    if (r->recv_ts_us <= r->send_ts_us) return 0.0;
    return (double)(r->recv_ts_us - r->send_ts_us);
}

/**
 * @brief Compute mean latency in µs.
 */
static inline double lat_mean_us(const latency_stats_t *s)
{
    if (s->count == 0) return 0.0;
    double sum = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        sum += lat_delay_us(&s->records[i]);
    }
    return sum / s->count;
}

/**
 * @brief Compute standard deviation of latency in µs.
 */
static inline double lat_stddev_us(const latency_stats_t *s)
{
    if (s->count < 2) return 0.0;
    double mean = lat_mean_us(s);
    double sum_sq = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        double d = lat_delay_us(&s->records[i]) - mean;
        sum_sq += d * d;
    }
    return sqrt(sum_sq / (s->count - 1));
}

/**
 * @brief Compute min latency in µs.
 */
static inline double lat_min_us(const latency_stats_t *s)
{
    if (s->count == 0) return 0.0;
    double min_val = lat_delay_us(&s->records[0]);
    for (uint32_t i = 1; i < s->count; i++) {
        double d = lat_delay_us(&s->records[i]);
        if (d < min_val) min_val = d;
    }
    return min_val;
}

/**
 * @brief Compute max latency in µs.
 */
static inline double lat_max_us(const latency_stats_t *s)
{
    if (s->count == 0) return 0.0;
    double max_val = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        double d = lat_delay_us(&s->records[i]);
        if (d > max_val) max_val = d;
    }
    return max_val;
}

/**
 * @brief Compute percentile latency in µs.
 *
 * @param pctl  Percentile (0-100).
 */
static inline double lat_percentile_us(const latency_stats_t *s, int pctl)
{
    if (s->count == 0) return 0.0;

    /* Copy delays to temp array for sorting */
    double *delays = (double *)malloc(s->count * sizeof(double));
    if (!delays) return 0.0;

    for (uint32_t i = 0; i < s->count; i++) {
        delays[i] = lat_delay_us(&s->records[i]);
    }

    /* Simple insertion sort (good enough for stats) */
    for (uint32_t i = 1; i < s->count; i++) {
        double key = delays[i];
        uint32_t j = i;
        while (j > 0 && delays[j - 1] > key) {
            delays[j] = delays[j - 1];
            j--;
        }
        delays[j] = key;
    }

    uint32_t idx = (uint32_t)((double)pctl / 100.0 * (s->count - 1));
    double result = delays[idx];
    free(delays);
    return result;
}

/**
 * @brief Print full latency report to a FILE*.
 */
static inline void lat_report(const latency_stats_t *s, const char *label,
                               uint32_t loss_rate_pct, FILE *out)
{
    double mean  = lat_mean_us(s);
    double sd    = lat_stddev_us(s);
    double mn    = lat_min_us(s);
    double mx    = lat_max_us(s);
    double p50   = lat_percentile_us(s, 50);
    double p95   = lat_percentile_us(s, 95);
    double p99   = lat_percentile_us(s, 99);
    double dur_s = (s->last_recv_us - s->first_send_us) / 1000000.0;

    fprintf(out, "\n");
    fprintf(out, "╔══════════════════════════════════════════════════════════════╗\n");
    fprintf(out, "║  %-58s  ║\n", label);
    fprintf(out, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(out, "║  Packet loss rate:     %3u%%                                 ║\n", loss_rate_pct);
    fprintf(out, "║  Packets received:     %-10u                          ║\n", s->count);
    fprintf(out, "║  Packets lost (gaps):  %-10u                          ║\n", s->lost_packets);
    fprintf(out, "║  Duration:             %-10.2f s                       ║\n", dur_s);
    fprintf(out, "║  Throughput:           %-10.2f pkt/s                   ║\n",
            s->count > 0 && dur_s > 0 ? s->count / dur_s : 0.0);
    fprintf(out, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(out, "║  Latency (µs):                                              ║\n");
    fprintf(out, "║    Mean:     %12.1f                                  ║\n", mean);
    fprintf(out, "║    Stddev:   %12.1f                                  ║\n", sd);
    fprintf(out, "║    Min:      %12.1f                                  ║\n", mn);
    fprintf(out, "║    Max:      %12.1f                                  ║\n", mx);
    fprintf(out, "║    P50:      %12.1f                                  ║\n", p50);
    fprintf(out, "║    P95:      %12.1f                                  ║\n", p95);
    fprintf(out, "║    P99:      %12.1f                                  ║\n", p99);
    fprintf(out, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(out, "║  Latency (ms):                                              ║\n");
    fprintf(out, "║    Mean:     %12.3f                                  ║\n", mean / 1000.0);
    fprintf(out, "║    Stddev:   %12.3f                                  ║\n", sd / 1000.0);
    fprintf(out, "║    Min:      %12.3f                                  ║\n", mn / 1000.0);
    fprintf(out, "║    Max:      %12.3f                                  ║\n", mx / 1000.0);
    fprintf(out, "║    P50:      %12.3f                                  ║\n", p50 / 1000.0);
    fprintf(out, "║    P95:      %12.3f                                  ║\n", p95 / 1000.0);
    fprintf(out, "║    P99:      %12.3f                                  ║\n", p99 / 1000.0);
    fprintf(out, "╚══════════════════════════════════════════════════════════════╝\n");
}

#endif /* LATENCY_STATS_H */
