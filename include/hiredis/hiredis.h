/**
 * Minimal hiredis stub for RoboControl compilation.
 */
#ifndef HIREDIS_H
#define HIREDIS_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REDIS_REPLY_STRING  1
#define REDIS_REPLY_ARRAY   2
#define REDIS_REPLY_INTEGER 3
#define REDIS_REPLY_NIL     4
#define REDIS_REPLY_STATUS  5
#define REDIS_REPLY_ERROR   6

typedef struct redisReply {
    int type;
    long long integer;
    size_t len;
    char *str;
    size_t elements;
    struct redisReply **element;
} redisReply;

typedef struct redisContext {
    int err;
    char errstr[128];
    int fd;
    int flags;
} redisContext;

redisContext *redisConnect(const char *ip, int port);
void redisFree(redisContext *c);
void *redisCommand(redisContext *c, const char *format, ...);
void freeReplyObject(void *reply);

#ifdef __cplusplus
}
#endif

#endif /* HIREDIS_H */
