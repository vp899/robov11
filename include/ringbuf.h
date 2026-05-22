/*
 * robocontrol/ringbuf.h - Lock-free single-producer single-consumer ring buffer
 */
#ifndef ROBOCONTROL_RINGBUF_H
#define ROBOCONTROL_RINGBUF_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    size_t   size;      /* must be power of 2 */
    size_t   mask;
    volatile size_t head;
    volatile size_t tail;
    rc_atomic64_t bytes_written;
    rc_atomic64_t bytes_read;
    rc_atomic64_t drops;
} rc_ringbuf_t;

static inline int rc_ringbuf_init(rc_ringbuf_t *rb, size_t size) {
    /* Round up to power of 2 */
    size_t s = 1;
    while (s < size) s <<= 1;
    rb->buf = calloc(1, s);
    if (!rb->buf) return RC_ERR_NOMEM;
    rb->size = s;
    rb->mask = s - 1;
    rb->head = 0;
    rb->tail = 0;
    rc_atomic_init(&rb->bytes_written, 0);
    rc_atomic_init(&rb->bytes_read, 0);
    rc_atomic_init(&rb->drops, 0);
    return RC_OK;
}

static inline void rc_ringbuf_destroy(rc_ringbuf_t *rb) {
    free(rb->buf);
    rb->buf = NULL;
}

static inline size_t rc_ringbuf_available(const rc_ringbuf_t *rb) {
    return rb->size - (rb->head - rb->tail);
}

static inline size_t rc_ringbuf_used(const rc_ringbuf_t *rb) {
    return rb->head - rb->tail;
}

static inline size_t rc_ringbuf_write(rc_ringbuf_t *rb, const uint8_t *data, size_t len) {
    size_t avail = rc_ringbuf_available(rb);
    if (len > avail) {
        rc_atomic_add(&rb->drops, (int64_t)len);
        return 0;
    }
    size_t off = rb->head & rb->mask;
    size_t first = MIN(len, rb->size - off);
    memcpy(rb->buf + off, data, first);
    if (first < len) {
        memcpy(rb->buf, data + first, len - first);
    }
    __atomic_store_n(&rb->head, rb->head + len, __ATOMIC_RELEASE);
    rc_atomic_add(&rb->bytes_written, (int64_t)len);
    return len;
}

static inline size_t rc_ringbuf_read(rc_ringbuf_t *rb, uint8_t *out, size_t max_len) {
    size_t used = rc_ringbuf_used(rb);
    if (used == 0) return 0;
    size_t len = MIN(used, max_len);
    size_t off = rb->tail & rb->mask;
    size_t first = MIN(len, rb->size - off);
    memcpy(out, rb->buf + off, first);
    if (first < len) {
        memcpy(out + first, rb->buf, len - first);
    }
    __atomic_store_n(&rb->tail, rb->tail + len, __ATOMIC_RELEASE);
    rc_atomic_add(&rb->bytes_read, (int64_t)len);
    return len;
}

static inline void rc_ringbuf_reset(rc_ringbuf_t *rb) {
    __atomic_store_n(&rb->head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rb->tail, 0, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
}
#endif

#endif /* ROBOCONTROL_RINGBUF_H */
