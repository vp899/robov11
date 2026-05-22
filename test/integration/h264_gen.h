/**
 * @file h264_gen.h
 * @brief Fake H.264 NALU generator for testing.
 *
 * Generates I-frame and P-frame NALUs with proper start codes and
 * type bytes, filling body with random data.  Simulates a robot
 * camera stream at configurable bitrate.
 */

#ifndef H264_GEN_H
#define H264_GEN_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* H.264 NALU types */
#define H264_NALU_SLICE     1   /**< P-frame slice */
#define H264_NALU_IDR       5   /**< I-frame (IDR) */
#define H264_NALU_SPS       7   /**< Sequence Parameter Set */
#define H264_NALU_PPS       8   /**< Picture Parameter Set */

/* H.264 start code */
#define H264_START_CODE     {0x00, 0x00, 0x00, 0x01}

/**
 * @brief Generate an I-frame NALU (IDR + SPS + PPS).
 *
 * Layout:
 *   [start_code(4)] [NALU_type=5(1)] [random_body]
 *   [start_code(4)] [NALU_type=7(1)] [sps_data(8)]
 *   [start_code(4)] [NALU_type=8(1)] [pps_data(4)]
 *
 * Total size ≈ target_size bytes.
 *
 * @param buf         Output buffer.
 * @param target_size Target total size in bytes (~30KB for I-frame).
 * @param stream_id   Stream identifier (for seeding RNG).
 * @param frame_num   Frame number (for seeding RNG).
 * @return Number of bytes written.
 */
static inline size_t h264_gen_iframe(uint8_t *buf, size_t target_size,
                                      uint16_t stream_id, uint32_t frame_num)
{
    uint8_t sc[] = H264_START_CODE;
    size_t pos = 0;

    /* SPS NALU (small, fixed) */
    memcpy(buf + pos, sc, 4); pos += 4;
    buf[pos++] = H264_NALU_SPS | 0x60;  /* forbidden=0, nal_ref_idc=3, type=7 */
    /* Fake SPS: profile_idc, constraint_set flags, level_idc */
    buf[pos++] = 0x64;  /* High profile */
    buf[pos++] = 0x00;
    buf[pos++] = 0x1f;  /* Level 3.1 */
    /* Fill rest of SPS with deterministic data */
    for (int i = 0; i < 4; i++) {
        buf[pos++] = (uint8_t)((stream_id * 7 + frame_num * 3 + i) & 0xFF);
    }

    /* PPS NALU (small, fixed) */
    memcpy(buf + pos, sc, 4); pos += 4;
    buf[pos++] = H264_NALU_PPS | 0x60;
    for (int i = 0; i < 4; i++) {
        buf[pos++] = (uint8_t)((stream_id * 11 + frame_num * 5 + i) & 0xFF);
    }

    /* IDR NALU (bulk of the I-frame) */
    memcpy(buf + pos, sc, 4); pos += 4;
    buf[pos++] = H264_NALU_IDR | 0x60;  /* forbidden=0, nal_ref_idc=3, type=5 */

    /* Fill remaining space with deterministic pseudo-random data */
    size_t remaining = target_size - pos;
    uint32_t seed = (uint32_t)(stream_id * 1000000 + frame_num);
    for (size_t i = 0; i < remaining; i++) {
        seed = seed * 1103515245 + 12345;  /* LCG */
        buf[pos++] = (uint8_t)((seed >> 16) & 0xFF);
    }

    return pos;
}

/**
 * @brief Generate a P-frame NALU (single slice).
 *
 * Layout:
 *   [start_code(4)] [NALU_type=1(1)] [random_body]
 *
 * @param buf         Output buffer.
 * @param target_size Target total size in bytes (~3KB for P-frame).
 * @param stream_id   Stream identifier.
 * @param frame_num   Frame number.
 * @return Number of bytes written.
 */
static inline size_t h264_gen_pframe(uint8_t *buf, size_t target_size,
                                      uint16_t stream_id, uint32_t frame_num)
{
    uint8_t sc[] = H264_START_CODE;
    size_t pos = 0;

    /* Slice header */
    memcpy(buf + pos, sc, 4); pos += 4;
    buf[pos++] = H264_NALU_SLICE | 0x40;  /* forbidden=0, nal_ref_idc=2, type=1 */

    /* Fill with deterministic pseudo-random data */
    size_t remaining = target_size - pos;
    uint32_t seed = (uint32_t)(stream_id * 2000000 + frame_num * 7 + 0xABCD);
    for (size_t i = 0; i < remaining; i++) {
        seed = seed * 1103515245 + 12345;
        buf[pos++] = (uint8_t)((seed >> 16) & 0xFF);
    }

    return pos;
}

/**
 * @brief Verify a received H.264 NALU has correct structure.
 *
 * Checks start codes and NALU type bytes.  Does NOT verify body content
 * (since it's random), but checks structural integrity.
 *
 * @param data  Received data.
 * @param len   Data length.
 * @return 0 if valid, -1 if no start code, -2 if invalid NALU type.
 */
static inline int h264_verify(const uint8_t *data, size_t len)
{
    if (len < 5) return -1;  /* minimum: 4-byte start code + 1-byte NALU type */

    /* Check for start code */
    if (data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x00 || data[3] != 0x01)
        return -1;

    /* Check NALU type */
    uint8_t nal_type = data[4] & 0x1F;
    if (nal_type != H264_NALU_SLICE && nal_type != H264_NALU_IDR &&
        nal_type != H264_NALU_SPS && nal_type != H264_NALU_PPS)
        return -2;

    return 0;
}

/**
 * @brief Verify that received body data matches expected pseudo-random sequence.
 *
 * @param data       Received data (starting from body, after NALU header).
 * @param len        Body length.
 * @param stream_id  Stream ID (for seed).
 * @param frame_num  Frame number (for seed).
 * @param is_iframe  True if I-frame (different seed offset).
 * @return Number of mismatched bytes (0 = perfect match).
 */
static inline uint32_t h264_verify_body(const uint8_t *data, size_t len,
                                          uint16_t stream_id, uint32_t frame_num,
                                          int is_iframe)
{
    uint32_t seed;
    if (is_iframe) {
        seed = (uint32_t)(stream_id * 1000000 + frame_num);
    } else {
        seed = (uint32_t)(stream_id * 2000000 + frame_num * 7 + 0xABCD);
    }

    uint32_t mismatches = 0;
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245 + 12345;
        uint8_t expected = (uint8_t)((seed >> 16) & 0xFF);
        if (data[i] != expected) mismatches++;
    }
    return mismatches;
}

/**
 * @brief Extract frame info from a received packet payload.
 *
 * Payload format: [stream_id(2)] [frame_num(4)] [is_iframe(1)] [h264_data...]
 */
typedef struct {
    uint16_t stream_id;
    uint32_t frame_num;
    int      is_iframe;
    const uint8_t *h264_data;
    size_t   h264_len;
} h264_pkt_info_t;

static inline int h264_parse_pkt(const uint8_t *payload, uint16_t payload_len,
                                  h264_pkt_info_t *info)
{
    if (payload_len < 7) return -1;

    info->stream_id = ((uint16_t)payload[0] << 8) | payload[1];
    info->frame_num = ((uint32_t)payload[2] << 24) | ((uint32_t)payload[3] << 16) |
                      ((uint32_t)payload[4] << 8) | payload[5];
    info->is_iframe = payload[6];
    info->h264_data = payload + 7;
    info->h264_len  = payload_len - 7;

    return 0;
}

#endif /* H264_GEN_H */
