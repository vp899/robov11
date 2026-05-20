/*
 * robocontrol/rate_limit.h - Token bucket rate limiter
 */
#ifndef ROBOCONTROL_RATE_LIMIT_H
#define ROBOCONTROL_RATE_LIMIT_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double   tokens;
    double   max_tokens;
    double   refill_rate;    /* tokens per second */
    int64_t  last_refill_ms;
    rc_spinlock_t lock;
    rc_atomic64_t total_allowed;
    rc_atomic64_t total_denied;
} rc_rate_limit_t;

static inline void rc_rate_limit_init(rc_rate_limit_t *rl,
                                       double max_tokens, double refill_rate) {
    rl->tokens = max_tokens;
    rl->max_tokens = max_tokens;
    rl->refill_rate = refill_rate;
    rl->last_refill_ms = rc_time_ms();
    rc_spin_init(&rl->lock);
    rc_atomic_init(&rl->total_allowed, 0);
    rc_atomic_init(&rl->total_denied, 0);
}

static inline int rc_rate_limit_check(rc_rate_limit_t *rl, double cost) {
    rc_spin_lock(&rl->lock);

    int64_t now = rc_time_ms();
    double elapsed = (double)(now - rl->last_refill_ms) / 1000.0;
    rl->tokens += elapsed * rl->refill_rate;
    if (rl->tokens > rl->max_tokens) rl->tokens = rl->max_tokens;
    rl->last_refill_ms = now;

    int allowed;
    if (rl->tokens >= cost) {
        rl->tokens -= cost;
        rc_atomic_inc(&rl->total_allowed);
        allowed = 1;
    } else {
        rc_atomic_inc(&rl->total_denied);
        allowed = 0;
    }

    rc_spin_unlock(&rl->lock);
    return allowed;
}

static inline void rc_rate_limit_reset(rc_rate_limit_t *rl) {
    rc_spin_lock(&rl->lock);
    rl->tokens = rl->max_tokens;
    rl->last_refill_ms = rc_time_ms();
    rc_spin_unlock(&rl->lock);
}

/* ── Per-key rate limiter (hash table of buckets) ───────────── */
#define RC_RATE_HASH_SIZE 4096

typedef struct rc_rate_entry {
    char    key[128];
    rc_rate_limit_t limit;
    struct rc_rate_entry *next;
    int64_t last_used;
} rc_rate_entry_t;

typedef struct {
    rc_rate_entry_t *buckets[RC_RATE_HASH_SIZE];
    rc_spinlock_t    locks[RC_RATE_HASH_SIZE];
    double           max_tokens;
    double           refill_rate;
    int64_t          cleanup_interval_ms;
    int64_t          last_cleanup;
} rc_rate_table_t;

static inline void rc_rate_table_init(rc_rate_table_t *rt,
                                       double max_tokens, double refill_rate,
                                       int64_t cleanup_interval_ms) {
    memset(rt->buckets, 0, sizeof(rt->buckets));
    for (int i = 0; i < RC_RATE_HASH_SIZE; i++)
        rc_spin_init(&rt->locks[i]);
    rt->max_tokens = max_tokens;
    rt->refill_rate = refill_rate;
    rt->cleanup_interval_ms = cleanup_interval_ms;
    rt->last_cleanup = rc_time_ms();
}

static inline uint32_t rc_rate_key_hash(const char *key) {
    uint32_t h = 0x811c9dc5;
    while (*key) {
        h ^= (uint8_t)*key++;
        h *= 0x01000193;
    }
    return h % RC_RATE_HASH_SIZE;
}

static inline rc_rate_entry_t *rc_rate_table_get_or_create(rc_rate_table_t *rt,
                                                             const char *key) {
    uint32_t idx = rc_rate_key_hash(key);

    rc_spin_lock(&rt->locks[idx]);

    rc_rate_entry_t *e = rt->buckets[idx];
    while (e) {
        if (strcmp(e->key, key) == 0) {
            e->last_used = rc_time_ms();
            rc_spin_unlock(&rt->locks[idx]);
            return e;
        }
        e = e->next;
    }

    /* Create new entry */
    e = calloc(1, sizeof(rc_rate_entry_t));
    if (!e) {
        rc_spin_unlock(&rt->locks[idx]);
        return NULL;
    }
    strncpy(e->key, key, sizeof(e->key) - 1);
    rc_rate_limit_init(&e->limit, rt->max_tokens, rt->refill_rate);
    e->last_used = rc_time_ms();
    e->next = rt->buckets[idx];
    rt->buckets[idx] = e;

    rc_spin_unlock(&rt->locks[idx]);
    return e;
}

static inline int rc_rate_table_check(rc_rate_table_t *rt, const char *key, double cost) {
    rc_rate_entry_t *e = rc_rate_table_get_or_create(rt, key);
    if (!e) return 0;
    return rc_rate_limit_check(&e->limit, cost);
}

static inline void rc_rate_table_cleanup(rc_rate_table_t *rt, int64_t max_age_ms) {
    int64_t now = rc_time_ms();
    if ((now - rt->last_cleanup) < rt->cleanup_interval_ms) return;
    rt->last_cleanup = now;

    for (int i = 0; i < RC_RATE_HASH_SIZE; i++) {
        rc_spin_lock(&rt->locks[i]);
        rc_rate_entry_t **pp = &rt->buckets[i];
        while (*pp) {
            if ((now - (*pp)->last_used) > max_age_ms) {
                rc_rate_entry_t *dead = *pp;
                *pp = dead->next;
                free(dead);
            } else {
                pp = &(*pp)->next;
            }
        }
        rc_spin_unlock(&rt->locks[i]);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* ROBOCONTROL_RATE_LIMIT_H */
