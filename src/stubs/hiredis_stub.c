/**
 * Minimal hiredis stub — returns NULL/0 for everything.
 * Allows linking without a real Redis library.
 */
#include "hiredis/hiredis.h"
#include <stdlib.h>
#include <stdarg.h>

redisContext *redisConnect(const char *ip, int port) {
    (void)ip; (void)port;
    return NULL;
}

void redisFree(redisContext *c) {
    (void)c;
}

void *redisCommand(redisContext *c, const char *format, ...) {
    (void)c; (void)format;
    return NULL;
}

void freeReplyObject(void *reply) {
    (void)reply;
}
