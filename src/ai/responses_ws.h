/* responses_ws.h - standard Responses over WebSocket, port of omp packages/ai/src/providers/openai-responses-session.ts
 * and responses-websocket-connection.ts: opt-in, one retained socket per logical session with incremental
 * `previous_response_id` turns, sticky HTTP/SSE fallback. */
#ifndef OMC_RESPONSES_WS_H
#define OMC_RESPONSES_WS_H

#include "ai/ai.h"

typedef struct rws_lease rws_lease_t;

typedef enum {
    RWS_DONE = 0,   /* a terminal event reached the callback */
    RWS_STOPPED,    /* the callback ended the exchange (response.failed or an error event) */
    RWS_ABORTED,    /* *cancel */
    RWS_CLOSED,     /* the socket went away before a terminal event */
    RWS_REJECTED,   /* a pre-generation request rejection reached the callback, *status */
    RWS_PROTOCOL,   /* unparsable frame */
} rws_result_t;

bool rws_is_sse_only(const char *url, const char *model_id, const char *session_id);
/* session_id NULL or empty: a temporary socket, no sticky state */
rws_lease_t *rws_acquire(const char *url, const char *model_id, const char *session_id, const http_headers_t *headers,
                         int64_t connect_timeout_ms, bool retain);
/* Handshake unless a healthy socket is retained. 0 ok, -1 failure with *err (owned) and *status (upgrade status, 0 a
 * network failure or timeout). */
int rws_connect(rws_lease_t *l, int *status, char **err);
void rws_mark_sse_only(rws_lease_t *l);
void rws_record_connect_failure(rws_lease_t *l);
bool rws_chain_disabled(const rws_lease_t *l);
/* One response.create exchange: frames go to cb (ai_sse_cb shape, event NULL). A definite first-frame stale-anchor
 * rejection resends the full request once on the same socket. idle_ms bounds silence between frames (<0 none). */
rws_result_t rws_exchange(rws_lease_t *l, const json_t *body, bool chain, int64_t idle_ms, volatile sig_atomic_t *cancel,
                          ai_sse_cb cb, void *ud, int *status, char **err);
void rws_close_socket(rws_lease_t *l); /* drop the socket and its continuation baseline */
void rws_release(rws_lease_t *l);      /* frees l; keeps a reusable socket for the session */
void rws_release_session(const char *session_id);

#endif
