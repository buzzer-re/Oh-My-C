/* http.h - HTTP/1.1 client over plain TCP or TLS (OpenSSL), with SSE helpers. */
#ifndef OMC_HTTP_H
#define OMC_HTTP_H

#include "core/core.h"

#include <signal.h>

typedef struct {
    char *scheme; /* "http" / "https" / "ws" / "wss" */
    char *host;   /* without brackets for IPv6 */
    int port;
    char *path;   /* path + query, always starts with '/' */
    char *userinfo;
    char *fragment;
} url_t;

int url_parse(const char *s, url_t *u); /* 0 ok */
void url_free(url_t *u);
char *url_resolve(const char *base, const char *ref); /* owned absolute URL */
char *url_encode(const char *s);                      /* percent-encode (RFC3986 unreserved kept) */
char *url_decode(const char *s);

typedef struct {
    char *name;
    char *value;
} http_header_t;
typedef VEC(http_header_t) http_headers_t;

void http_headers_add(http_headers_t *h, const char *name, const char *value);
void http_headers_set(http_headers_t *h, const char *name, const char *value);
void http_headers_del(http_headers_t *h, const char *name); /* case-insensitive, every occurrence */
const char *http_headers_get(const http_headers_t *h, const char *name); /* case-insensitive */
void http_headers_free(http_headers_t *h);

/* return non-zero from a callback to abort the transfer */
typedef int (*http_headers_cb)(void *ud, int status, const http_headers_t *headers);
typedef int (*http_data_cb)(void *ud, const char *data, size_t len);

typedef struct {
    const char *method; /* default GET, or POST if body */
    const char *url;
    const http_headers_t *headers;
    const char *body;
    size_t body_len;
    int64_t timeout_ms;         /* whole request (0 => none) */
    int64_t connect_timeout_ms; /* default 30s */
    int64_t idle_timeout_ms;    /* max gap between received bytes (0 => 300s) */
    int max_redirects;          /* 0 => none followed */
    bool no_decompress;         /* don't send Accept-Encoding/decode gzip */
    bool no_proxy;
    size_t max_body;            /* when buffering (0 => 64MB) */
    http_headers_cb on_headers;
    http_data_cb on_data;       /* if set, body is streamed (decoded) and not buffered */
    void *ud;
    volatile sig_atomic_t *cancel;
} http_req_t;

typedef struct {
    int status;
    http_headers_t headers;
    char *body; /* buffered body (NUL-terminated) unless streaming */
    size_t body_len;
    char *error; /* transport error message, NULL on success */
    char *final_url;
    bool aborted;
    bool truncated;
    /* the connection was up (TLS done, or a pooled socket) and failed before any response headers: how far the
     * request got (cowork-fetch.ts RequestUploadProgress). `error` keeps the transport's own wording. */
    bool lost_before_response;
    bool body_sent;        /* the socket took the whole body (the OS may still hold some of it unsent) */
    size_t body_bytes;     /* request body size on the wire */
    int64_t elapsed_ms;    /* since the request started */
    const char *lost_code; /* the socket error's code (static: "EPIPE", "ECONNRESET", ...), NULL when unknown */
    bool via_proxy;        /* the request went through a configured proxy */
} http_resp_t;

int http_request(const http_req_t *req, http_resp_t *resp); /* 0 if a response was received */
void http_resp_free(http_resp_t *r);

/* convenience */
int http_get(const char *url, const http_headers_t *headers, int64_t timeout_ms, http_resp_t *resp);
int http_post_json(const char *url, const http_headers_t *headers, const char *json, int64_t timeout_ms,
                   http_resp_t *resp);

void http_global_init(void);
void http_pool_clear(void);

/* ---------- Server-Sent Events ---------- */
typedef int (*sse_event_cb)(void *ud, const char *event, const char *data, size_t data_len);
typedef struct {
    sb_t line;
    sb_t data;
    char *event;
    bool has_data;
    sse_event_cb cb;
    void *ud;
} sse_parser_t;

void sse_init(sse_parser_t *p, sse_event_cb cb, void *ud);
int sse_feed(sse_parser_t *p, const char *data, size_t len); /* returns cb abort code */
int sse_finish(sse_parser_t *p);
void sse_free(sse_parser_t *p);

/* ---------- raw connection (used by websockets / http) ---------- */
typedef struct net_conn net_conn_t;
net_conn_t *net_connect(const char *host, int port, bool tls, int64_t timeout_ms, char **err);
ssize_t net_read(net_conn_t *c, void *buf, size_t n, int64_t timeout_ms); /* -2 timeout */
int net_write_all(net_conn_t *c, const void *buf, size_t n);
void net_close(net_conn_t *c);
int net_fd(net_conn_t *c);
/* connect for url's scheme (https/wss => TLS), honoring HTTPS_PROXY / NO_PROXY for TLS targets */
net_conn_t *net_open(const url_t *u, int64_t timeout_ms, char **err);

#endif
