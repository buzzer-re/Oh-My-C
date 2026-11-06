/* responses_ws.c - standard Responses over WebSocket; see responses_ws.h. There is no reader thread: the socket is only
 * read during an exchange, so idle expiry and a peer close are noticed when the session next asks for it. */
#define _GNU_SOURCE
#include "ai/responses_ws.h"

#include "net/ws.h"

#include <ctype.h>
#include <pthread.h>
#include <strings.h>

#define IDLE_REUSE_MS 30000
#define STALE_ANCHOR_LIMIT 3   /* consecutive definite anchor rejections before a session stops sending anchors */
#define CONNECT_FAILURE_LIMIT 2 /* consecutive pre-send network failures before a session sticks to HTTP/SSE */

typedef struct {
    char *url;
    http_headers_t headers;
    int64_t connect_timeout_ms;
    ws_t *ws;
    bool completed;
    int64_t last_inbound;
    char *last_response_id;
    json_t *last_request, *last_items;
} sock_t;

typedef struct {
    char *key, *session_id;
    bool sse_only, chain_disabled;
    int connect_failures, stale_anchors, active;
    sock_t *cached;
    bool cached_busy;
} tsession_t;

struct rws_lease {
    tsession_t *entry; /* NULL: no logical session, or released by rws_release_session */
    sock_t *sock;
    bool retained;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static VEC(tsession_t *) g_sessions;

static void clear_baseline(sock_t *s) {
    free(s->last_response_id);
    json_free(s->last_request);
    json_free(s->last_items);
    s->last_response_id = NULL;
    s->last_request = s->last_items = NULL;
}

static void sock_close(sock_t *s) {
    s->completed = false;
    clear_baseline(s);
    if (s->ws) ws_close(s->ws, 1000, "released");
    s->ws = NULL;
}

static void sock_free(sock_t *s) {
    if (!s) return;
    sock_close(s);
    free(s->url);
    http_headers_free(&s->headers);
    free(s);
}

static void headers_copy(http_headers_t *to, const http_headers_t *from) {
    for (size_t i = 0; from && i < from->n; i++) http_headers_set(to, from->v[i].name, from->v[i].value);
}

static bool headers_same(const http_headers_t *a, const http_headers_t *b) {
    if (a->n != b->n) return false;
    for (size_t i = 0; i < a->n; i++)
        if (!str_eq(http_headers_get(b, a->v[i].name), a->v[i].value)) return false;
    return true;
}

/* isHealthyForReuse: open, inside the idle limit and not closed by the peer meanwhile */
static bool sock_reusable(sock_t *s) {
    if (!s->completed || !s->ws || !ws_is_open(s->ws)) return false;
    if (mono_ms() - s->last_inbound > IDLE_REUSE_MS) return false;
    for (;;) {
        ws_msg_t m;
        int r = ws_recv(s->ws, 1, NULL, &m, NULL);
        if (r == -2) return true;
        if (r != 1) return false;
        bool closed = m.opcode == WS_OP_CLOSE;
        ws_msg_free(&m);
        if (closed) return false; /* a data frame between exchanges is stale: dropped */
    }
}

static sock_t *sock_new(const char *url, const http_headers_t *headers, int64_t connect_timeout_ms) {
    sock_t *s = xcalloc(1, sizeof *s);
    s->url = xstrdup(url);
    headers_copy(&s->headers, headers);
    s->connect_timeout_ms = connect_timeout_ms;
    return s;
}

static char *scope_key(const char *url, const char *model_id, const char *session_id) {
    return xasprintf("%s\n%s\n%s", url, model_id ? model_id : "", session_id);
}

static tsession_t *session_find(const char *key) {
    for (size_t i = 0; i < g_sessions.n; i++)
        if (!strcmp(g_sessions.v[i]->key, key)) return g_sessions.v[i];
    return NULL;
}

bool rws_is_sse_only(const char *url, const char *model_id, const char *session_id) {
    if (str_empty(session_id)) return false;
    char *key = scope_key(url, model_id, session_id);
    pthread_mutex_lock(&g_mu);
    tsession_t *e = session_find(key);
    bool r = e && e->sse_only;
    pthread_mutex_unlock(&g_mu);
    free(key);
    return r;
}

rws_lease_t *rws_acquire(const char *url, const char *model_id, const char *session_id, const http_headers_t *headers,
                         int64_t connect_timeout_ms, bool retain) {
    rws_lease_t *l = xcalloc(1, sizeof *l);
    if (str_empty(session_id)) {
        l->sock = sock_new(url, headers, connect_timeout_ms);
        return l;
    }
    char *key = scope_key(url, model_id, session_id);
    pthread_mutex_lock(&g_mu);
    tsession_t *e = session_find(key);
    if (e) {
        free(key);
    } else {
        e = xcalloc(1, sizeof *e);
        e->key = key;
        e->session_id = xstrdup(session_id);
        vec_push(&g_sessions, e);
    }
    if (e->cached && !e->cached_busy) {
        /* changed credentials or options, a dead or idle socket: full input over a new one */
        if (!sock_reusable(e->cached) || !headers_same(&e->cached->headers, headers) || !retain || e->sse_only) {
            sock_free(e->cached);
            e->cached = NULL;
        }
    }
    bool retained = retain && !e->sse_only && !(e->cached && e->cached_busy);
    if (retained) {
        if (!e->cached) e->cached = sock_new(url, headers, connect_timeout_ms);
        e->cached_busy = true;
        l->sock = e->cached;
    } else {
        l->sock = sock_new(url, headers, connect_timeout_ms); /* a concurrent request gets a temporary socket */
    }
    l->retained = retained;
    e->active++;
    l->entry = e;
    pthread_mutex_unlock(&g_mu);
    return l;
}

int rws_connect(rws_lease_t *l, int *status, char **err) {
    *status = 0;
    *err = NULL;
    if (l->sock->ws && ws_is_open(l->sock->ws)) return 0;
    char *url;
    if (str_starts(l->sock->url, "https://")) url = xasprintf("wss://%s", l->sock->url + 8);
    else if (str_starts(l->sock->url, "http://")) url = xasprintf("ws://%s", l->sock->url + 7);
    else url = xstrdup(l->sock->url);
    int st = 0;
    char *e = NULL;
    l->sock->ws = ws_connect(url, &l->sock->headers, l->sock->connect_timeout_ms, &st, &e);
    free(url);
    if (!l->sock->ws) {
        *status = st;
        if (st) {
            *err = xasprintf("websocket upgrade rejected (HTTP %d)", st);
            free(e);
        } else {
            *err = e ? e : xstrdup("connection timeout");
        }
        return -1;
    }
    l->sock->last_inbound = mono_ms();
    return 0;
}

void rws_mark_sse_only(rws_lease_t *l) {
    pthread_mutex_lock(&g_mu);
    tsession_t *e = l->entry;
    if (e) {
        e->sse_only = true;
        if (e->cached && !e->cached_busy) {
            sock_free(e->cached);
            e->cached = NULL;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void rws_record_connect_failure(rws_lease_t *l) {
    bool sticky = false;
    pthread_mutex_lock(&g_mu);
    if (l->entry) sticky = ++l->entry->connect_failures >= CONNECT_FAILURE_LIMIT;
    pthread_mutex_unlock(&g_mu);
    if (sticky) rws_mark_sse_only(l);
}

bool rws_chain_disabled(const rws_lease_t *l) {
    pthread_mutex_lock(&g_mu);
    bool r = l->entry && l->entry->chain_disabled;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static void record_chain_result(rws_lease_t *l, bool accepted) {
    pthread_mutex_lock(&g_mu);
    tsession_t *e = l->entry;
    if (e) {
        if (accepted) {
            e->stale_anchors = 0;
        } else if (++e->stale_anchors >= STALE_ANCHOR_LIMIT) {
            e->chain_disabled = true;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void rws_close_socket(rws_lease_t *l) { sock_close(l->sock); }

void rws_release(rws_lease_t *l) {
    if (!l) return;
    pthread_mutex_lock(&g_mu);
    tsession_t *e = l->entry;
    bool reusable = l->sock->completed && l->sock->ws && ws_is_open(l->sock->ws);
    if (e) {
        e->active--;
        if (reusable) e->connect_failures = 0; /* a completed exchange proves the transport */
    }
    if (l->retained && e && e->cached == l->sock) {
        e->cached_busy = false;
        if (!reusable || e->sse_only) {
            sock_free(l->sock);
            e->cached = NULL;
        }
    } else {
        sock_free(l->sock);
    }
    pthread_mutex_unlock(&g_mu);
    free(l);
}

void rws_release_session(const char *session_id) {
    if (!session_id) return;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_sessions.n; i++) {
        tsession_t *e = g_sessions.v[i];
        if (strcmp(e->session_id, session_id) || e->active) continue;
        sock_free(e->cached);
        free(e->key);
        free(e->session_id);
        free(e);
        memmove(&g_sessions.v[i], &g_sessions.v[i + 1], (g_sessions.n - i - 1) * sizeof g_sessions.v[0]);
        g_sessions.n--;
        i--;
    }
    pthread_mutex_unlock(&g_mu);
}

/* ---------- continuation ---------- */

/* deepEqualsWithout, keys sharing a position in `omit` being ignored */
static bool key_in(const char *k, const char *const *omit, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(k, omit[i])) return true;
    return false;
}

static bool equal_without(const json_t *a, const json_t *b, const char *const *omit, size_t n) {
    if (!json_is_obj(a) || !json_is_obj(b)) return json_equal(a, b);
    const char *k;
    json_t *v;
    json_obj_foreach(a, i, k, v) {
        if (key_in(k, omit, n)) continue;
        if (!json_get(b, k) || !json_equal(v, json_get(b, k))) return false;
    }
    json_obj_foreach(b, j, k, v) {
        if (!key_in(k, omit, n) && !json_get(a, k)) return false;
    }
    return true;
}

/* one sanitized history item against the same item in the next full input (the omit keys of buildResponsesDeltaInput) */
static bool same_input_item(const json_t *held, const json_t *cur) {
    static const char *const lifecycle[] = {"status"}, *const replayed[] = {"status", "id"};
    const char *t = json_get_str(held, "type");
    if (str_eq(t, "message") || str_eq(t, "function_call") || str_eq(t, "custom_tool_call"))
        return equal_without(held, cur, replayed, ARRAY_LEN(replayed));
    return equal_without(held, cur, lifecycle, ARRAY_LEN(lifecycle));
}

static void message_text(const json_t *item, sb_t *out) {
    const json_t *c = json_get(item, "content");
    if (json_is_str(c)) {
        sb_append(out, json_sv(c));
        return;
    }
    json_t *p;
    json_arr_foreach(c, i, p) {
        const char *t = json_get_str(p, "text");
        if (!t) t = json_get_str(p, "refusal");
        if (t) sb_append(out, t);
    }
}

/* A server output item against the item omc rebuilds from the assistant message. omp replays the native items verbatim;
 * omc rebuilds them, so the content decides and the server's extra fields (logprobs, annotations, argument spacing) do
 * not. A mismatch only costs a full-history resend. */
static bool same_output_item(const json_t *server, const json_t *cur) {
    const char *t = json_get_str(server, "type");
    if (!str_eq(t, json_get_str(cur, "type"))) return false;
    if (str_eq(t, "function_call")) {
        if (!str_eq(json_get_str(server, "call_id"), json_get_str(cur, "call_id"))) return false;
        if (!str_eq(json_get_str(server, "name"), json_get_str(cur, "name"))) return false;
        const char *as = json_get_str(server, "arguments"), *ac = json_get_str(cur, "arguments");
        json_t *ja = as ? json_parse_str(as) : NULL, *jc = ac ? json_parse_str(ac) : NULL;
        bool same = ja && jc ? json_equal(ja, jc) : str_eq(as, ac);
        json_free(ja);
        json_free(jc);
        return same;
    }
    if (str_eq(t, "message")) {
        sb_t a = SB_INIT, b = SB_INIT;
        message_text(server, &a);
        message_text(cur, &b);
        bool same = !strcmp(sb_cstr(&a), sb_cstr(&b));
        sb_free(&a);
        sb_free(&b);
        return same;
    }
    if (str_eq(t, "reasoning")) {
        const char *es = json_get_str(server, "encrypted_content");
        if (es) return str_eq(es, json_get_str(cur, "encrypted_content"));
    }
    return same_input_item(server, cur);
}

/* buildResponsesDeltaInput: the items past the baseline when the request extends it, NULL otherwise. Owned. */
static json_t *delta_input(const json_t *prev, const json_t *prev_items, const json_t *cur) {
    static const char *const top[] = {"input", "client_metadata"};
    const json_t *pi = json_get(prev, "input"), *ci = json_get(cur, "input");
    if (!json_is_arr(pi) || !json_is_arr(ci)) return NULL;
    if (!equal_without(prev, cur, top, ARRAY_LEN(top))) return NULL;
    size_t base = json_len(pi) + json_len(prev_items);
    if (json_len(ci) <= base) return NULL;
    size_t idx = 0;
    for (size_t i = 0; i < json_len(pi); i++, idx++)
        if (!same_input_item(json_at(pi, i), json_at(ci, idx))) return NULL;
    for (size_t i = 0; i < json_len(prev_items); i++, idx++)
        if (!same_output_item(json_at(prev_items, i), json_at(ci, idx))) return NULL;
    json_t *d = json_arr();
    for (; idx < json_len(ci); idx++) json_push(d, json_clone(json_at(ci, idx)));
    return d;
}

static bool blank_text(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

/* sanitizeOpenAIResponsesAssistantHistoryItemsForReplay kept every item: some output, no whitespace-only message */
static bool items_replayable(const json_t *items) {
    bool output = false;
    json_t *it;
    json_arr_foreach(items, i, it) {
        if (!json_is_obj(it)) return false; /* a hole: an output item never reported */
        const char *t = json_get_str(it, "type");
        if (str_eq(t, "reasoning")) continue;
        if (str_eq(t, "message") && str_eq(json_get_str(it, "role"), "assistant")) {
            sb_t b = SB_INIT;
            message_text(it, &b);
            bool empty = blank_text(sb_cstr(&b));
            sb_free(&b);
            if (empty) return false;
        }
        output = true;
    }
    return output;
}

/* ---------- exchange ---------- */

/* extractHttpStatusFromError over a frame or its detail: status / statusCode / response.status, 0 when absent */
static int frame_status(const json_t *o) {
    static const char *const keys[] = {"status", "statusCode", "status_code"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        const json_t *v = json_get(o, keys[i]);
        int64_t n = json_is_num(v) ? json_get_int(o, keys[i], 0) : json_is_str(v) ? strtoll(json_sv(v), NULL, 10) : 0;
        if (n >= 100 && n <= 599) return (int)n;
    }
    const json_t *r = json_get_obj(o, "response");
    return r ? frame_status(r) : 0;
}

/* hasOpenAIResponsesGenerationEvidence */
static bool generation_evidence(const json_t *ev) {
    const json_t *r = json_get_obj(ev, "response");
    if (!r) return false;
    if (json_len(json_get_arr(r, "output")) > 0) return true;
    const json_t *u = json_get_obj(r, "usage");
    return json_get_int(u, "input_tokens", 0) > 0 || json_get_int(u, "output_tokens", 0) > 0 || json_get_int(u, "total_tokens", 0) > 0;
}

/* isOpenAIResponsesStalePreviousResponseError */
static bool stale_anchor_error(const json_t *e) {
    if (!json_is_obj(e)) return false;
    if (str_eq(json_get_str(e, "code"), "previous_response_not_found")) return true;
    const char *m = json_get_str(e, "message");
    if (!m) return false;
    bool named = strcasestr(m, "previous response") || strcasestr(m, "previous_response") || strcasestr(m, "previousresponse");
    return named && (strcasestr(m, "not found") || strcasestr(m, "not_found") || strcasestr(m, "notfound") ||
                     strcasestr(m, "invalid") || strcasestr(m, "expired") || strcasestr(m, "stale") || strcasestr(m, "unsupported"));
}

/* readOpenAIResponsesRequestRejection: never overload, auth or unknown envelopes, or a generation that made progress */
static bool is_request_rejection(const json_t *ev, int *status) {
    const char *t = json_get_str(ev, "type");
    if (!str_eq(t, "error") && !str_eq(t, "response.failed")) return false;
    if (generation_evidence(ev)) return false;
    const json_t *resp = json_get_obj(ev, "response");
    const json_t *detail = json_get_obj(resp, "error");
    if (!detail) detail = json_get_obj(ev, "error");
    if (!detail) detail = ev;
    int st = frame_status(ev);
    if (!st) st = frame_status(detail);
    if (st != 400 && st != 422 && !(st == 0 && str_eq(json_get_str(detail, "type"), "invalid_request_error"))) return false;
    *status = st;
    return true;
}

rws_result_t rws_exchange(rws_lease_t *l, const json_t *body, bool chain, int64_t idle_ms, volatile sig_atomic_t *cancel,
                          ai_sse_cb cb, void *ud, int *status, char **err) {
    sock_t *s = l->sock;
    *status = 0;
    *err = NULL;
    s->completed = false;
    json_t *request = json_clone(body);
    json_set_str(request, "type", "response.create");
    json_del(request, "stream");
    json_del(request, "stream_options");
    json_del(request, "background");
    json_t *delta = chain && s->last_response_id ? delta_input(s->last_request, s->last_items, request) : NULL;
    json_t *wire = json_clone(request);
    bool chained = delta != NULL;
    if (delta) {
        json_set_str(wire, "previous_response_id", s->last_response_id);
        json_set(wire, "input", delta);
    }
    clear_baseline(s);
    rws_result_t result = RWS_CLOSED;
    json_t *output_items = NULL;
    for (;;) {
        char *text = json_dump(wire, 0);
        int sent = s->ws ? ws_send_text(s->ws, text, strlen(text)) : -1;
        free(text);
        if (sent != 0) {
            result = RWS_CLOSED;
            break;
        }
        bool first = true, stale_retry = false, ended = false;
        json_free(output_items);
        output_items = json_arr();
        while (!ended) {
            ws_msg_t m;
            char *rerr = NULL;
            int r = ws_recv(s->ws, idle_ms, cancel, &m, &rerr);
            free(rerr);
            if (r == -4) {
                result = RWS_ABORTED;
                ended = true;
                sock_close(s);
                break;
            }
            if (r != 1 || m.opcode == WS_OP_CLOSE) {
                if (r == 1) ws_msg_free(&m);
                result = RWS_CLOSED;
                ended = true;
                sock_close(s);
                break;
            }
            s->last_inbound = mono_ms();
            if (!m.len) {
                ws_msg_free(&m);
                continue;
            }
            json_t *ev = json_parse(m.data, m.len, NULL);
            if (!json_is_obj(ev)) {
                json_free(ev);
                ws_msg_free(&m);
                result = RWS_PROTOCOL;
                *err = xstrdup("websocket frame is not a JSON object");
                ended = true;
                sock_close(s);
                break;
            }
            const char *type = json_get_str(ev, "type");
            /* the connection flattens error.{code,message} onto an error frame */
            json_t *flat = str_eq(type, "error") ? json_get_obj(ev, "error") : NULL;
            const json_t *anchor_err = str_eq(type, "response.failed") ? json_get(json_get_obj(ev, "response"), "error") : flat ? flat : ev;
            if (first && chained && stale_anchor_error(anchor_err) && !generation_evidence(ev)) {
                int st = frame_status(ev);
                if (!st) st = frame_status(anchor_err);
                if (st == 0 || st == 400 || st == 422) {
                    stale_retry = true;
                    record_chain_result(l, false);
                    json_free(ev);
                    ws_msg_free(&m);
                    break;
                }
            }
            if (first) {
                int st = 0;
                if (is_request_rejection(ev, &st)) {
                    /* the decoder reports it as any other error frame; the socket stays healthy */
                    *status = st;
                    cb(ud, NULL, m.data, m.len);
                    s->completed = true;
                    result = RWS_REJECTED;
                    json_free(ev);
                    ws_msg_free(&m);
                    ended = true;
                    break;
                }
            }
            first = false;
            if (str_eq(type, "response.output_item.done")) {
                size_t idx = (size_t)json_get_int(ev, "output_index", 0);
                while (json_len(output_items) <= idx && idx < 65536) json_push(output_items, json_null());
                if (idx < 65536) {
                    json_t *item = json_clone(json_get(ev, "item"));
                    json_free(output_items->u.arr.items[idx]);
                    output_items->u.arr.items[idx] = item;
                }
            }
            bool terminal = str_eq(type, "response.completed") || str_eq(type, "response.incomplete") || str_eq(type, "response.done");
            if (terminal) {
                s->completed = true;
                if (chained) record_chain_result(l, true);
                const json_t *resp = json_get_obj(ev, "response");
                const char *rs = json_get_str(resp, "status");
                const char *rid = json_get_str(resp, "id");
                if (resp && rid && !str_eq(type, "response.incomplete") && !str_eq(rs, "incomplete") && !str_eq(rs, "failed")) {
                    const json_t *items = json_is_arr(json_get(resp, "output")) ? json_get(resp, "output") : output_items;
                    if (items_replayable(items)) {
                        s->last_response_id = xstrdup(rid);
                        s->last_request = json_clone(request);
                        s->last_items = json_clone(items);
                    }
                }
            }
            int cr = cb(ud, NULL, m.data, m.len);
            json_free(ev);
            ws_msg_free(&m);
            if (terminal) {
                result = RWS_DONE;
                ended = true;
            } else if (cr) {
                /* the exchange ended without a terminal frame (response.failed, an error event): not reusable */
                result = RWS_STOPPED;
                ended = true;
                sock_close(s);
            }
        }
        if (!stale_retry) break;
        /* a definite first-frame rejection permits one full exchange, not a reconnect or uncertain replay */
        if (!s->ws || !ws_is_open(s->ws)) {
            result = RWS_CLOSED;
            break;
        }
        chained = false;
        json_free(wire);
        wire = json_clone(request);
    }
    json_free(output_items);
    json_free(wire);
    json_free(request);
    return result;
}
