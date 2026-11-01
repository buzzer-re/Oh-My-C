/* ws.h - RFC 6455 WebSocket client over net_conn_t (TCP or TLS, CONNECT proxy for wss).
 *
 * Client frames are masked; server frames must not be. Fragmented messages are reassembled; pings are answered
 * with pongs while receiving; a close frame is echoed once. Frame and message sizes are bounded (ws_t.max_message,
 * default 16 MiB) so a hostile peer cannot make the client buffer unbounded data. */
#ifndef OMC_WS_H
#define OMC_WS_H

#include "net/http.h"

typedef struct ws ws_t;

enum { WS_OP_CONT = 0, WS_OP_TEXT = 1, WS_OP_BINARY = 2, WS_OP_CLOSE = 8, WS_OP_PING = 9, WS_OP_PONG = 10 };

typedef struct {
    int opcode;       /* WS_OP_TEXT / WS_OP_BINARY for data, WS_OP_CLOSE when the peer closed */
    char *data;       /* owned, NUL-terminated (len excludes the NUL) */
    size_t len;
    int close_code;   /* WS_OP_CLOSE: peer status (1005 when absent) */
    char *close_reason; /* WS_OP_CLOSE: owned UTF-8 reason or NULL */
} ws_msg_t;

/* Opening handshake. `headers` (may be NULL) are sent verbatim after the protocol headers. On failure returns
 * NULL with *err (owned) and *status (HTTP status of a non-101 answer, 0 transport). */
ws_t *ws_connect(const char *url, const http_headers_t *headers, int64_t timeout_ms, int *status, char **err);
void ws_set_max_message(ws_t *w, size_t max);
/* 0 ok, -1 failure (connection unusable) */
int ws_send_text(ws_t *w, const char *data, size_t len);
int ws_send_binary(ws_t *w, const void *data, size_t len);
int ws_send_ping(ws_t *w, const void *data, size_t len);
/* Next data/close message. Returns 1 with *out filled, -2 idle timeout (no byte for timeout_ms; <0 = none),
 * -4 cancelled (*cancel set), -1 protocol/transport error (*err owned when err != NULL) or peer gone without close. */
int ws_recv(ws_t *w, int64_t timeout_ms, volatile sig_atomic_t *cancel, ws_msg_t *out, char **err);
void ws_msg_free(ws_msg_t *m);
/* Send a close frame (code 0 => none) unless one was sent, then drop the connection; frees w. */
void ws_close(ws_t *w, int code, const char *reason);
bool ws_is_open(const ws_t *w);

/* exposed for tests: encode one client frame (masked with mask_key) into b */
void ws_encode_frame(sb_t *b, int opcode, bool fin, const unsigned char mask_key[4], const void *data, size_t len);
/* Sec-WebSocket-Accept for a key (owned) */
char *ws_accept_key(const char *key);

#endif
