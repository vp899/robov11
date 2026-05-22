/*
 * robocontrol/http.h - Simple HTTP/1.1 parser and response builder
 */
#ifndef ROBOCONTROL_HTTP_H
#define ROBOCONTROL_HTTP_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RC_HTTP_MAX_HEADERS   32
#define RC_HTTP_MAX_URL_LEN   512

typedef enum {
    RC_HTTP_GET = 0,
    RC_HTTP_POST,
    RC_HTTP_PUT,
    RC_HTTP_DELETE,
    RC_HTTP_HEAD,
    RC_HTTP_OPTIONS,
} rc_http_method_t;

typedef struct {
    char *name;
    char *value;
} rc_http_header_t;

typedef struct {
    rc_http_method_t method;
    char    url[RC_HTTP_MAX_URL_LEN];
    char   *body;
    size_t  body_len;
    rc_http_header_t headers[RC_HTTP_MAX_HEADERS];
    int     header_count;
    int     keep_alive;
    int     content_length;
    /* Parsed URL path (without query) */
    char    path[RC_HTTP_MAX_URL_LEN];
    char   *query_string;
} rc_http_request_t;

typedef struct {
    int     status_code;
    const char *status_text;
    char   *body;
    size_t  body_len;
    rc_http_header_t headers[RC_HTTP_MAX_HEADERS];
    int     header_count;
} rc_http_response_t;

/* ── HTTP method strings ────────────────────────────────────── */
static const char *rc_http_method_str(rc_http_method_t m) {
    switch (m) {
    case RC_HTTP_GET:     return "GET";
    case RC_HTTP_POST:    return "POST";
    case RC_HTTP_PUT:     return "PUT";
    case RC_HTTP_DELETE:  return "DELETE";
    case RC_HTTP_HEAD:    return "HEAD";
    case RC_HTTP_OPTIONS: return "OPTIONS";
    default:              return "UNKNOWN";
    }
}

static rc_http_method_t rc_parse_method(const char *m) {
    if (strcmp(m, "GET") == 0) return RC_HTTP_GET;
    if (strcmp(m, "POST") == 0) return RC_HTTP_POST;
    if (strcmp(m, "PUT") == 0) return RC_HTTP_PUT;
    if (strcmp(m, "DELETE") == 0) return RC_HTTP_DELETE;
    if (strcmp(m, "HEAD") == 0) return RC_HTTP_HEAD;
    if (strcmp(m, "OPTIONS") == 0) return RC_HTTP_OPTIONS;
    return RC_HTTP_GET;
}

/* ── HTTP parser ────────────────────────────────────────────── */
static inline int rc_http_parse(rc_http_request_t *req, const char *raw, size_t raw_len) {
    memset(req, 0, sizeof(*req));
    req->keep_alive = 1;

    /* Find end of request line */
    const char *eol = memchr(raw, '\n', raw_len);
    if (!eol) return RC_ERR_PROTO;
    size_t line_len = (size_t)(eol - raw);
    if (line_len > 0 && *(eol - 1) == '\r') line_len--;

    /* Parse method */
    char line[2048];
    size_t ll = MIN(line_len, sizeof(line) - 1);
    memcpy(line, raw, ll);
    line[ll] = '\0';

    char *saveptr;
    char *method = strtok_r(line, " ", &saveptr);
    char *url = strtok_r(NULL, " ", &saveptr);
    char *version = strtok_r(NULL, " \r\n", &saveptr);
    if (!method || !url) return RC_ERR_PROTO;

    req->method = rc_parse_method(method);
    strncpy(req->url, url, RC_HTTP_MAX_URL_LEN - 1);

    /* Parse path and query */
    char *q = strchr(req->url, '?');
    if (q) {
        size_t plen = (size_t)(q - req->url);
        memcpy(req->path, req->url, plen);
        req->path[plen] = '\0';
        req->query_string = q + 1;
    } else {
        strncpy(req->path, req->url, RC_HTTP_MAX_URL_LEN - 1);
        req->query_string = NULL;
    }

    if (version && strstr(version, "1.0")) req->keep_alive = 0;

    /* Parse headers */
    const char *p = eol + 1;
    const char *end = raw + raw_len;

    while (p < end && *p != '\r' && *p != '\n') {
        const char *hline_end = memchr(p, '\n', (size_t)(end - p));
        if (!hline_end) break;
        size_t hline_len = (size_t)(hline_end - p);
        if (hline_len > 0 && *(hline_end - 1) == '\r') hline_len--;

        if (hline_len == 0) break; /* Empty line = end of headers */

        const char *colon = memchr(p, ':', hline_len);
        if (colon && req->header_count < RC_HTTP_MAX_HEADERS) {
            size_t name_len = (size_t)(colon - p);
            const char *val = colon + 1;
            while (val < p + hline_len && *val == ' ') val++;
            size_t val_len = (size_t)((p + hline_len) - val);

            req->headers[req->header_count].name = strndup(p, name_len);
            req->headers[req->header_count].value = strndup(val, val_len);
            req->header_count++;

            /* Special headers */
            if (strncasecmp(p, "Content-Length", 14) == 0) {
                req->content_length = atoi(val);
            }
            if (strncasecmp(p, "Connection", 10) == 0) {
                if (strncasecmp(val, "close", 5) == 0) req->keep_alive = 0;
            }
        }
        p = hline_end + 1;
    }

    /* Find body start (after \r\n\r\n) */
    const char *body_start = NULL;
    for (const char *s = raw; s < end - 3; s++) {
        if (s[0] == '\r' && s[1] == '\n' && s[2] == '\r' && s[3] == '\n') {
            body_start = s + 4;
            break;
        }
    }
    if (body_start && body_start < end) {
        size_t blen = (size_t)(end - body_start);
        if (req->content_length > 0 && (size_t)req->content_length < blen)
            blen = (size_t)req->content_length;
        req->body = strndup(body_start, blen);
        req->body_len = blen;
    }

    return RC_OK;
}

static inline void rc_http_request_free(rc_http_request_t *req) {
    for (int i = 0; i < req->header_count; i++) {
        free(req->headers[i].name);
        free(req->headers[i].value);
    }
    free(req->body);
}

static inline const char *rc_http_get_header(rc_http_request_t *req, const char *name) {
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

/* ── HTTP response builder ──────────────────────────────────── */
static inline const char *rc_http_status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

static inline int rc_http_build_response(rc_http_response_t *res,
                                          char *out, size_t out_size) {
    int n = snprintf(out, out_size,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: keep-alive\r\n"
                     "Content-Type: application/json\r\n",
                     res->status_code,
                     rc_http_status_text(res->status_code),
                     res->body_len);

    for (int i = 0; i < res->header_count && (size_t)n < out_size; i++) {
        n += snprintf(out + n, out_size - (size_t)n, "%s: %s\r\n",
                      res->headers[i].name, res->headers[i].value);
    }

    if ((size_t)n < out_size - 2) {
        out[n++] = '\r';
        out[n++] = '\n';
    }

    if (res->body && res->body_len > 0) {
        size_t copy = MIN(res->body_len, out_size - (size_t)n - 1);
        memcpy(out + n, res->body, copy);
        n += (int)copy;
    }

    return n;
}

/* ── Simple JSON builder (no parser needed - output only) ───── */
typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
    int     first;
    int     error;
} rc_json_builder_t;

static inline void rc_json_init(rc_json_builder_t *jb, char *buf, size_t cap) {
    jb->buf = buf;
    jb->len = 0;
    jb->cap = cap;
    jb->first = 1;
    jb->error = 0;
    if (cap > 0) buf[0] = '\0';
}

static inline void rc_json_append(rc_json_builder_t *jb, const char *s, size_t slen) {
    if (jb->error) return;
    if (jb->len + slen >= jb->cap) { jb->error = 1; return; }
    memcpy(jb->buf + jb->len, s, slen);
    jb->len += slen;
    jb->buf[jb->len] = '\0';
}

static inline void rc_json_str(rc_json_builder_t *jb, const char *s) {
    if (!s) { rc_json_append(jb, "null", 4); return; }
    rc_json_append(jb, "\"", 1);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '"':  rc_json_append(jb, "\\\"", 2); break;
        case '\\': rc_json_append(jb, "\\\\", 2); break;
        case '\n': rc_json_append(jb, "\\n", 2); break;
        case '\r': rc_json_append(jb, "\\r", 2); break;
        case '\t': rc_json_append(jb, "\\t", 2); break;
        default:
            if ((uint8_t)*p < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
                rc_json_append(jb, esc, 6);
            } else {
                rc_json_append(jb, p, 1);
            }
        }
    }
    rc_json_append(jb, "\"", 1);
}

static inline void rc_json_key(rc_json_builder_t *jb, const char *key) {
    if (!jb->first) rc_json_append(jb, ",", 1);
    jb->first = 0;
    rc_json_str(jb, key);
    rc_json_append(jb, ":", 1);
}

static inline void rc_json_kv_str(rc_json_builder_t *jb, const char *key, const char *val) {
    rc_json_key(jb, key);
    rc_json_str(jb, val);
}

static inline void rc_json_kv_int(rc_json_builder_t *jb, const char *key, int64_t val) {
    rc_json_key(jb, key);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%ld", (long)val);
    rc_json_append(jb, tmp, strlen(tmp));
}

static inline void rc_json_kv_uint(rc_json_builder_t *jb, const char *key, uint64_t val) {
    rc_json_key(jb, key);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)val);
    rc_json_append(jb, tmp, strlen(tmp));
}

static inline void rc_json_kv_bool(rc_json_builder_t *jb, const char *key, int val) {
    rc_json_key(jb, key);
    rc_json_append(jb, val ? "true" : "false", val ? 4 : 5);
}

static inline void rc_json_object_start(rc_json_builder_t *jb, const char *key) {
    rc_json_key(jb, key);
    rc_json_append(jb, "{", 1);
    /* Save parent first state, init new */
    /* Simplified: caller manages nesting */
}

static inline void rc_json_object_end(rc_json_builder_t *jb) {
    rc_json_append(jb, "}", 1);
}

static inline void rc_json_begin_object(rc_json_builder_t *jb) {
    jb->first = 1;
    rc_json_append(jb, "{", 1);
}

static inline void rc_json_end_object(rc_json_builder_t *jb) {
    rc_json_append(jb, "}", 1);
}

/* ── Simple JSON value extractor (for parsing incoming JSON) ── */
static inline int rc_json_extract_str(const char *json, const char *key,
                                       char *out, size_t out_size) {
    /* Find "key":"value" pattern */
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *kp = strstr(json, pattern);
    if (!kp) return RC_ERR_NOTFOUND;

    const char *colon = strchr(kp + strlen(pattern), ':');
    if (!colon) return RC_ERR_PROTO;
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;

    if (*colon == '"') {
        colon++;
        const char *end = strchr(colon, '"');
        if (!end) return RC_ERR_PROTO;
        size_t len = (size_t)(end - colon);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, colon, len);
        out[len] = '\0';
        return RC_OK;
    }
    return RC_ERR_PROTO;
}

static inline int rc_json_extract_int(const char *json, const char *key, int64_t *out) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *kp = strstr(json, pattern);
    if (!kp) return RC_ERR_NOTFOUND;

    const char *colon = strchr(kp + strlen(pattern), ':');
    if (!colon) return RC_ERR_PROTO;
    colon++;
    while (*colon == ' ') colon++;

    *out = (int64_t)strtoll(colon, NULL, 10);
    return RC_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* ROBOCONTROL_HTTP_H */
