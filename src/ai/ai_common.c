#define _GNU_SOURCE
#include "ai.h"

#include <math.h>
#include <time.h>

/* ---------- effort ---------- */

static const char *effort_names[] = {"off", "minimal", "low", "medium", "high", "xhigh", "max"};

const char *effort_name(ai_effort_t e) {
    if ((int)e < 0 || (size_t)e >= ARRAY_LEN(effort_names)) return "off";
    return effort_names[e];
}

int effort_parse(const char *s, ai_effort_t *out) {
    if (!s) return -1;
    for (size_t i = 0; i < ARRAY_LEN(effort_names); i++) {
        if (strcasecmp(s, effort_names[i]) == 0) {
            *out = (ai_effort_t)i;
            return 0;
        }
    }
    if (strcasecmp(s, "none") == 0 || strcasecmp(s, "false") == 0) {
        *out = EFFORT_OFF;
        return 0;
    }
    return -1;
}

static const char *ev_names[] = {"start",        "text_start",     "text_delta",     "text_end",
                                 "thinking_start", "thinking_delta", "thinking_end",   "toolcall_start",
                                 "toolcall_delta", "toolcall_end",   "done",           "error"};

const char *ai_event_name(ai_event_type_t t) { return ev_names[t]; }

void ai_stream_opts_init(ai_stream_opts_t *o) {
    memset(o, 0, sizeof(*o));
    o->temperature = NAN;
    o->top_p = NAN;
    o->max_retries = 3;
}

/* ---------- registry ---------- */

typedef struct {
    char *api;
    ai_stream_fn fn;
} api_entry_t;
static VEC(api_entry_t) g_apis;

void ai_register_api(const char *api, ai_stream_fn fn) {
    for (size_t i = 0; i < g_apis.n; i++) {
        if (strcmp(g_apis.v[i].api, api) == 0) {
            g_apis.v[i].fn = fn;
            return;
        }
    }
    api_entry_t e = {xstrdup(api), fn};
    vec_push(&g_apis, e);
}

ai_stream_fn ai_lookup_api(const char *api) {
    for (size_t i = 0; i < g_apis.n; i++)
        if (strcmp(g_apis.v[i].api, api) == 0) return g_apis.v[i].fn;
    return NULL;
}

json_t *ai_usage_new(void) {
    json_t *u = json_obj();
    json_set_int(u, "input", 0);
    json_set_int(u, "output", 0);
    json_set_int(u, "cacheRead", 0);
    json_set_int(u, "cacheWrite", 0);
    json_set_int(u, "totalTokens", 0);
    json_t *c = json_obj();
    json_set_num(c, "input", 0);
    json_set_num(c, "output", 0);
    json_set_num(c, "cacheRead", 0);
    json_set_num(c, "cacheWrite", 0);
    json_set_num(c, "total", 0);
    json_set(u, "cost", c);
    return u;
}

void ai_usage_compute_cost(const ai_model_t *m, json_t *u) {
    double in = (double)json_get_int(u, "input", 0), out = (double)json_get_int(u, "output", 0);
    double cr = (double)json_get_int(u, "cacheRead", 0), cw = (double)json_get_int(u, "cacheWrite", 0);
    json_t *c = json_obj();
    double ci = m ? in * m->cost_input / 1e6 : 0, co = m ? out * m->cost_output / 1e6 : 0;
    double ccr = m ? cr * m->cost_cache_read / 1e6 : 0, ccw = m ? cw * m->cost_cache_write / 1e6 : 0;
    json_set_num(c, "input", ci);
    json_set_num(c, "output", co);
    json_set_num(c, "cacheRead", ccr);
    json_set_num(c, "cacheWrite", ccw);
    json_set_num(c, "total", ci + co + ccr + ccw);
    json_set(u, "cost", c);
    json_set_int(u, "totalTokens", (int64_t)(in + out + cr + cw));
}

static json_t *new_assistant(const ai_model_t *m) {
    json_t *msg = json_obj();
    json_set_str(msg, "role", "assistant");
    json_set(msg, "content", json_arr());
    json_set_str(msg, "api", m ? m->api : "unknown");
    json_set_str(msg, "provider", m ? m->provider : "unknown");
    json_set_str(msg, "model", m ? m->id : "unknown");
    json_set(msg, "usage", ai_usage_new());
    json_set_str(msg, "stopReason", "stop");
    json_set_int(msg, "timestamp", now_ms());
    return msg;
}

json_t *ai_error_message(const ai_model_t *m, const char *stop_reason, const char *err) {
    json_t *msg = new_assistant(m);
    json_set_str(msg, "stopReason", stop_reason ? stop_reason : "error");
    if (err) json_set_str(msg, "errorMessage", err);
    return msg;
}

/* ---------- builder ---------- */

void aib_init(ai_builder_t *b, const ai_model_t *m, ai_event_cb cb, void *ud) {
    memset(b, 0, sizeof(*b));
    b->model = m;
    b->cb = cb;
    b->ud = ud;
    b->msg = new_assistant(m);
    b->cur_index = -1;
    b->t_start = mono_ms();
}

static void emit(ai_builder_t *b, ai_event_type_t t, int idx, const char *delta, size_t dlen, const json_t *block,
                 const char *reason) {
    if (!b->cb) return;
    ai_event_t ev = {.type = t, .content_index = idx, .delta = delta, .delta_len = dlen, .block = block,
                     .partial = b->msg, .reason = reason};
    b->cb(b->ud, &ev);
}

void aib_start(ai_builder_t *b) {
    if (b->started) return;
    b->started = true;
    emit(b, AI_EV_START, -1, NULL, 0, NULL, NULL);
}

static json_t *content(ai_builder_t *b) { return json_get(b->msg, "content"); }

int aib_block_count(const ai_builder_t *b) { return (int)json_len(json_get(b->msg, "content")); }

static void first_token(ai_builder_t *b) {
    if (!b->t_first_token) b->t_first_token = mono_ms();
}

void aib_end_block(ai_builder_t *b) {
    if (b->cur_index < 0) return;
    json_t *blk = json_at(content(b), (size_t)b->cur_index);
    int idx = b->cur_index;
    int kind = b->cur_kind;
    b->cur_index = -1;
    b->cur_kind = 0;
    if (kind == 1) {
        json_set(blk, "text", json_strn(sb_cstr(&b->buf), b->buf.len));
        sb_clear(&b->buf);
        emit(b, AI_EV_TEXT_END, idx, NULL, 0, blk, NULL);
    } else if (kind == 2) {
        json_set(blk, "thinking", json_strn(sb_cstr(&b->buf), b->buf.len));
        sb_clear(&b->buf);
        emit(b, AI_EV_THINKING_END, idx, NULL, 0, blk, NULL);
    } else if (kind == 3) {
        json_t *args = json_get(blk, "arguments");
        bool have_args = json_is_obj(args) && json_len(args) > 0;
        json_t *full = b->buf.len ? json_parse(b->buf.s, b->buf.len, NULL) : NULL;
        if (json_is_obj(full)) {
            json_set(blk, "arguments", full);
        } else if (b->buf.len && !have_args) {
            json_free(full);
            json_t *parsed = json_parse(b->buf.s, b->buf.len, NULL);
            if (!parsed) parsed = json_parse_partial(b->buf.s, b->buf.len);
            if (!json_is_obj(parsed)) {
                json_free(parsed);
                parsed = json_obj();
            }
            json_set(blk, "arguments", parsed);
        } else if (!json_is_obj(args)) {
            json_free(full);
            json_set(blk, "arguments", json_obj());
        } else {
            json_free(full);
        }
        sb_clear(&b->buf);
        emit(b, AI_EV_TOOLCALL_END, idx, NULL, 0, blk, NULL);
    }
}

static json_t *open_block(ai_builder_t *b, int kind) {
    aib_start(b);
    if (b->cur_kind == kind && b->cur_index >= 0) return json_at(content(b), (size_t)b->cur_index);
    aib_end_block(b);
    json_t *blk = json_obj();
    if (kind == 1) {
        json_set_str(blk, "type", "text");
        json_set_str(blk, "text", "");
    } else if (kind == 2) {
        json_set_str(blk, "type", "thinking");
        json_set_str(blk, "thinking", "");
    }
    json_push(content(b), blk);
    b->cur_index = (int)json_len(content(b)) - 1;
    b->cur_kind = kind;
    sb_clear(&b->buf);
    if (kind == 1) emit(b, AI_EV_TEXT_START, b->cur_index, NULL, 0, NULL, NULL);
    else if (kind == 2) emit(b, AI_EV_THINKING_START, b->cur_index, NULL, 0, NULL, NULL);
    return blk;
}

void aib_text_delta(ai_builder_t *b, const char *s, size_t n) {
    if (!n) return;
    first_token(b);
    json_t *blk = open_block(b, 1);
    sb_appendn(&b->buf, s, n);
    /* keep partial in sync for renderers */
    json_set(blk, "text", json_strn(b->buf.s, b->buf.len));
    emit(b, AI_EV_TEXT_DELTA, b->cur_index, s, n, NULL, NULL);
}

void aib_thinking_delta(ai_builder_t *b, const char *s, size_t n) {
    if (!n) return;
    first_token(b);
    json_t *blk = open_block(b, 2);
    sb_appendn(&b->buf, s, n);
    json_set(blk, "thinking", json_strn(b->buf.s, b->buf.len));
    emit(b, AI_EV_THINKING_DELTA, b->cur_index, s, n, NULL, NULL);
}

static json_t *last_block_of(ai_builder_t *b, const char *type) {
    json_t *c = content(b);
    for (size_t i = json_len(c); i > 0; i--) {
        json_t *blk = json_at(c, i - 1);
        if (str_eq(json_get_str(blk, "type"), type)) return blk;
    }
    return NULL;
}

void aib_set_thinking_signature(ai_builder_t *b, const char *sig) {
    json_t *blk = last_block_of(b, "thinking");
    if (!blk) {
        open_block(b, 2);
        blk = last_block_of(b, "thinking");
    }
    if (blk && sig) json_set_str(blk, "thinkingSignature", sig);
}

void aib_set_text_signature(ai_builder_t *b, const char *sig) {
    json_t *blk = last_block_of(b, "text");
    if (blk && sig) json_set_str(blk, "textSignature", sig);
}

void aib_redacted_thinking(ai_builder_t *b, const char *data) {
    aib_start(b);
    aib_end_block(b);
    json_t *blk = json_obj();
    json_set_str(blk, "type", "redactedThinking");
    json_set_str(blk, "data", data ? data : "");
    json_push(content(b), blk);
}

void aib_toolcall_start(ai_builder_t *b, const char *id, const char *name) {
    first_token(b);
    aib_start(b);
    aib_end_block(b);
    json_t *blk = json_obj();
    json_set_str(blk, "type", "toolCall");
    char *gen = NULL;
    if (!id || !*id) {
        char *r = rand_hex(12);
        gen = xasprintf("call_%s", r);
        free(r);
    }
    json_set_str(blk, "id", gen ? gen : id);
    free(gen);
    json_set_str(blk, "name", name ? name : "");
    json_set(blk, "arguments", json_obj());
    json_push(content(b), blk);
    b->cur_index = (int)json_len(content(b)) - 1;
    b->cur_kind = 3;
    sb_clear(&b->buf);
    emit(b, AI_EV_TOOLCALL_START, b->cur_index, NULL, 0, NULL, NULL);
}

void aib_toolcall_delta(ai_builder_t *b, const char *s, size_t n) {
    if (b->cur_kind != 3 || !n) return;
    sb_appendn(&b->buf, s, n);
    json_t *blk = json_at(content(b), (size_t)b->cur_index);
    json_t *partial = json_parse_partial(b->buf.s, b->buf.len);
    if (json_is_obj(partial)) json_set(blk, "arguments", partial);
    else json_free(partial);
    emit(b, AI_EV_TOOLCALL_DELTA, b->cur_index, s, n, NULL, NULL);
}

void aib_toolcall_set_args(ai_builder_t *b, json_t *args) {
    if (b->cur_kind != 3) {
        json_free(args);
        return;
    }
    json_t *blk = json_at(content(b), (size_t)b->cur_index);
    if (!json_is_obj(args)) {
        json_free(args);
        args = json_obj();
    }
    json_set(blk, "arguments", args);
    sb_clear(&b->buf);
}

void aib_toolcall_set_field(ai_builder_t *b, const char *key, const char *val) {
    json_t *blk = b->cur_kind == 3 ? json_at(content(b), (size_t)b->cur_index) : last_block_of(b, "toolCall");
    if (blk && val) json_set_str(blk, key, val);
}

void aib_usage(ai_builder_t *b, int64_t input, int64_t output, int64_t cache_read, int64_t cache_write) {
    json_t *u = json_get(b->msg, "usage");
    if (input >= 0) json_set_int(u, "input", input);
    if (output >= 0) json_set_int(u, "output", output);
    if (cache_read >= 0) json_set_int(u, "cacheRead", cache_read);
    if (cache_write >= 0) json_set_int(u, "cacheWrite", cache_write);
    ai_usage_compute_cost(b->model, u);
}

void aib_usage_add_reasoning(ai_builder_t *b, int64_t r) {
    json_t *u = json_get(b->msg, "usage");
    json_set_int(u, "reasoningTokens", r);
}

void aib_set(ai_builder_t *b, const char *key, json_t *val) { json_set(b->msg, key, val); }

json_t *aib_finish(ai_builder_t *b, const char *stop_reason, const char *error_message) {
    aib_start(b);
    aib_end_block(b);
    if (!stop_reason) stop_reason = "stop";
    /* if the model emitted tool calls, the stop reason is toolUse regardless of provider wording */
    if (strcmp(stop_reason, "stop") == 0 && last_block_of(b, "toolCall")) stop_reason = "toolUse";
    json_set_str(b->msg, "stopReason", stop_reason);
    if (error_message) json_set_str(b->msg, "errorMessage", error_message);
    ai_usage_compute_cost(b->model, json_get(b->msg, "usage"));
    int64_t end = mono_ms();
    json_set_int(b->msg, "duration", end - b->t_start);
    if (b->t_first_token) json_set_int(b->msg, "ttft", b->t_first_token - b->t_start);
    bool err = strcmp(stop_reason, "error") == 0 || strcmp(stop_reason, "aborted") == 0;
    emit(b, err ? AI_EV_ERROR : AI_EV_DONE, -1, NULL, 0, NULL, stop_reason);
    json_t *m = b->msg;
    b->msg = NULL;
    return m;
}

void aib_free(ai_builder_t *b) {
    json_free(b->msg);
    b->msg = NULL;
    sb_free(&b->buf);
}

/* ---------- stream entry ---------- */

json_t *ai_stream(const ai_model_t *model, const ai_context_t *ctx, const ai_stream_opts_t *opts, ai_event_cb cb,
                  void *ud) {
    ai_stream_fn fn = ai_lookup_api(model->api);
    ai_stream_opts_t def;
    if (!opts) {
        ai_stream_opts_init(&def);
        opts = &def;
    }
    if (!fn) {
        char *e = xasprintf("Unsupported API \"%s\" for model %s/%s", model->api, model->provider, model->id);
        json_t *m = ai_error_message(model, "error", e);
        free(e);
        if (cb) {
            ai_event_t ev = {.type = AI_EV_ERROR, .content_index = -1, .partial = m, .reason = "error"};
            cb(ud, &ev);
        }
        return m;
    }
    return fn(model, ctx, opts, cb, ud);
}

json_t *ai_complete(const ai_model_t *model, const ai_context_t *ctx, const ai_stream_opts_t *opts) {
    return ai_stream(model, ctx, opts, NULL, NULL);
}

char *ai_message_text(const json_t *msg) {
    sb_t b = SB_INIT;
    json_t *c = json_get(msg, "content");
    if (json_is_str(c)) return xstrdup(json_sv(c));
    json_t *blk;
    json_arr_foreach(c, i, blk) {
        if (str_eq(json_get_str(blk, "type"), "text")) sb_append(&b, json_get_str(blk, "text"));
    }
    return sb_detach(&b);
}

/* ---------- HTTP helpers ---------- */

void ai_apply_headers(http_headers_t *h, const ai_model_t *m, const ai_stream_opts_t *o) {
    const char *k;
    json_t *v;
    if (m && json_is_obj(m->headers)) {
        json_obj_foreach(m->headers, i, k, v) {
            if (json_is_str(v)) http_headers_set(h, k, json_sv(v));
        }
    }
    if (o && json_is_obj(o->headers)) {
        json_obj_foreach(o->headers, i, k, v) {
            if (json_is_str(v)) http_headers_set(h, k, json_sv(v));
        }
    }
}

char *ai_extract_error(int status, const char *body) {
    json_t *j = body ? json_parse_str(body) : NULL;
    char *msg = NULL;
    if (j) {
        json_t *e = json_get(j, "error");
        const char *s = NULL;
        if (json_is_obj(e)) s = json_get_str(e, "message");
        else if (json_is_str(e)) s = json_sv(e);
        if (!s) s = json_get_str(j, "message");
        if (!s) s = json_get_str(j, "detail");
        if (!s && json_is_arr(j)) {
            json_t *first = json_at(j, 0);
            json_t *fe = json_get(first, "error");
            if (fe) s = json_get_str(fe, "message");
        }
        if (s) msg = xstrdup(s);
        json_free(j);
    }
    if (!msg) {
        if (body && *body) {
            size_t n = strlen(body);
            msg = n > 600 ? xasprintf("%.600s…", body) : xstrdup(body);
        } else {
            msg = xstrdup("(empty response)");
        }
    }
    char *r = status ? xasprintf("%d %s", status, msg) : xstrdup(msg);
    free(msg);
    return r;
}

typedef struct {
    int status;
    bool ok;
    bool got_data;
    sb_t errbody;
    sse_parser_t sse;
    http_data_cb raw_cb;
    ai_sse_cb sse_cb;
    void *ud;
    int abort_rc;
    const char *retry_after;
    int64_t retry_after_ms;
} sse_ctx_t;

static int on_hdr(void *ud, int status, const http_headers_t *h) {
    sse_ctx_t *c = ud;
    c->status = status;
    c->ok = status >= 200 && status < 300;
    c->retry_after_ms = -1;
    const char *ra = http_headers_get(h, "retry-after-ms");
    if (ra) c->retry_after_ms = strtoll(ra, NULL, 10);
    else if ((ra = http_headers_get(h, "retry-after"))) {
        char *end;
        double secs = strtod(ra, &end);
        if (end != ra) {
            c->retry_after_ms = (int64_t)(secs * 1000);
        } else {
            /* HTTP-date form */
            struct tm tm;
            memset(&tm, 0, sizeof tm);
            if (strptime(ra, "%a, %d %b %Y %H:%M:%S", &tm)) {
                int64_t when = (int64_t)timegm(&tm) * 1000;
                int64_t d = when - now_ms();
                c->retry_after_ms = d > 0 ? d : 0;
            }
        }
    }
    return 0;
}

static int on_body(void *ud, const char *data, size_t len) {
    sse_ctx_t *c = ud;
    if (!c->ok) {
        if (c->errbody.len < 65536) sb_appendn(&c->errbody, data, len);
        return 0;
    }
    c->got_data = true;
    if (c->raw_cb) {
        int rc = c->raw_cb(c->ud, data, len);
        if (rc) c->abort_rc = rc;
        return rc;
    }
    int rc = sse_feed(&c->sse, data, len);
    if (rc) c->abort_rc = rc;
    return rc;
}

static bool retryable_status(int s) { return s == 408 || s == 409 || s == 425 || s == 429 || s == 500 || s == 502 || s == 503 || s == 504 || s == 520 || s == 522 || s == 524 || s == 529; }

static int stream_post(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                       ai_sse_cb sse_cb, http_data_cb raw_cb, void *ud, int *status, char **err) {
    if (!http_headers_get(headers, "Content-Type")) http_headers_add(headers, "Content-Type", "application/json");
    if (sse_cb && !http_headers_get(headers, "Accept")) http_headers_add(headers, "Accept", "text/event-stream");
    int max_retries = opts && opts->max_retries > 0 ? opts->max_retries : 0;
    int64_t max_delay = opts && opts->max_retry_delay_ms ? opts->max_retry_delay_ms : 60000;
    *err = NULL;
    *status = 0;
    for (int attempt = 0;; attempt++) {
        sse_ctx_t c = {0};
        c.sse_cb = sse_cb;
        c.raw_cb = raw_cb;
        c.ud = ud;
        if (sse_cb) sse_init(&c.sse, (sse_event_cb)sse_cb, ud);
        http_req_t req = {.method = "POST",
                          .url = url,
                          .headers = headers,
                          .body = body,
                          .body_len = strlen(body),
                          .idle_timeout_ms = 300000,
                          .on_headers = on_hdr,
                          .on_data = on_body,
                          .ud = &c,
                          .cancel = opts ? opts->cancel : NULL};
        http_resp_t resp;
        int rc = http_request(&req, &resp);
        if (sse_cb && rc == 0 && c.ok && !c.abort_rc && !resp.aborted) {
            int frc = sse_finish(&c.sse);
            if (frc) c.abort_rc = frc;
        }
        *status = c.status;
        bool cancelled = opts && opts->cancel && *opts->cancel;
        if (cancelled) {
            *err = xstrdup("Request was aborted");
            http_resp_free(&resp);
            sse_free(&c.sse);
            sb_free(&c.errbody);
            return -2;
        }
        if (rc == 0 && c.ok) {
            int ret = 0;
            if (c.abort_rc) ret = c.abort_rc;
            else if (resp.error) {
                *err = xstrdup(resp.error);
                ret = -1;
            }
            http_resp_free(&resp);
            sse_free(&c.sse);
            sb_free(&c.errbody);
            return ret;
        }
        /* failure: decide retry */
        bool transient = (rc != 0 && !c.got_data) || retryable_status(c.status);
        char *msg = rc != 0 ? xstrdup(resp.error ? resp.error : "network error")
                            : ai_extract_error(c.status, sb_cstr(&c.errbody));
        /* overloaded errors embedded in body */
        if (!transient && c.status == 400 && str_istr(sb_cstr(&c.errbody), "overloaded")) transient = true;
        int64_t delay = 1000LL << attempt;
        if (c.retry_after_ms >= 0) delay = c.retry_after_ms;
        http_resp_free(&resp);
        sse_free(&c.sse);
        sb_free(&c.errbody);
        if (!transient || attempt >= max_retries || c.got_data || (max_delay > 0 && delay > max_delay)) {
            if (max_delay > 0 && delay > max_delay && transient && attempt < max_retries) {
                char *m2 = xasprintf("%s (server requested retry in %llds)", msg, (long long)(delay / 1000));
                free(msg);
                msg = m2;
            }
            *err = msg;
            return -1;
        }
        LOGW("transient provider error (attempt %d): %s; retrying in %lldms", attempt + 1, msg, (long long)delay);
        free(msg);
        int64_t until = mono_ms() + delay;
        while (mono_ms() < until) {
            if (opts && opts->cancel && *opts->cancel) {
                *err = xstrdup("Request was aborted");
                return -2;
            }
            sleep_ms(50);
        }
    }
}

int ai_sse_post(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                ai_sse_cb on_event, void *ud, int *status, char **err) {
    return stream_post(url, headers, body, opts, on_event, NULL, ud, status, err);
}

int ai_stream_post(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                   http_data_cb on_data, void *ud, int *status, char **err) {
    return stream_post(url, headers, body, opts, NULL, on_data, ud, status, err);
}

/* ---------- thinking budgets ---------- */

int64_t ai_thinking_budget(const ai_model_t *m, ai_effort_t e, const json_t *custom) {
    if (custom) {
        int64_t v = json_get_int(custom, effort_name(e), -1);
        if (v > 0) return v;
    }
    int64_t b;
    bool google = m && m->api && str_starts(m->api, "google");
    switch (e) {
    case EFFORT_MINIMAL: b = 1024; break;
    case EFFORT_LOW: b = google ? 2048 : 4096; break;
    case EFFORT_MEDIUM: b = google ? 8192 : 8192; break;
    case EFFORT_HIGH: b = google ? 24575 : 16384; break;
    case EFFORT_XHIGH: b = 32768; break;
    case EFFORT_MAX: b = 32768; break;
    default: b = 0;
    }
    if (m && m->max_tokens > 0 && b >= m->max_tokens) b = m->max_tokens > 2048 ? m->max_tokens - 1024 : m->max_tokens / 2;
    return b;
}

/* ---------- transform messages ---------- */

json_t *ai_transform_messages(const json_t *messages, const ai_model_t *target) {
    json_t *out = json_arr();
    json_t *msg;
    /* pending tool calls awaiting results: ids */
    strvec_t pending_ids = {0}, pending_names = {0};
    json_arr_foreach(messages, i, msg) {
        const char *role = json_get_str(msg, "role");
        if (!role) continue;
        if (strcmp(role, "assistant") == 0) {
            /* synthesize missing results for previous assistant's tool calls */
            for (size_t k = 0; k < pending_ids.n; k++) {
                json_t *tr = json_obj();
                json_set_str(tr, "role", "toolResult");
                json_set_str(tr, "toolCallId", pending_ids.v[k]);
                json_set_str(tr, "toolName", pending_names.v[k]);
                json_t *c = json_arr();
                json_t *t = json_obj();
                json_set_str(t, "type", "text");
                json_set_str(t, "text", "No result provided");
                json_push(c, t);
                json_set(tr, "content", c);
                json_set_bool(tr, "isError", true);
                json_set_int(tr, "timestamp", now_ms());
                json_push(out, tr);
            }
            strvec_free(&pending_ids);
            strvec_free(&pending_names);
            const char *stop = json_get_str(msg, "stopReason");
            if (stop && (strcmp(stop, "error") == 0 || strcmp(stop, "aborted") == 0)) {
                /* keep aborted turns only if they produced content; drop tool calls (incomplete) */
                json_t *c = json_get(msg, "content");
                bool has_text = false;
                json_t *blk;
                json_arr_foreach(c, j, blk) {
                    if (str_eq(json_get_str(blk, "type"), "text") && !str_empty(json_get_str(blk, "text"))) has_text = true;
                }
                if (!has_text) continue;
            }
            bool same = target && str_eq(json_get_str(msg, "provider"), target->provider) &&
                        str_eq(json_get_str(msg, "model"), target->id) && str_eq(json_get_str(msg, "api"), target->api);
            bool same_api = target && str_eq(json_get_str(msg, "api"), target->api);
            json_t *nm = json_clone(msg);
            json_t *nc = json_arr();
            json_t *blk;
            json_t *oc = json_get(msg, "content");
            json_arr_foreach(oc, j, blk) {
                const char *t = json_get_str(blk, "type");
                if (str_eq(t, "thinking")) {
                    if (same || (same_api && json_get_str(blk, "thinkingSignature"))) {
                        json_push(nc, json_clone(blk));
                    } else {
                        const char *th = json_get_str(blk, "thinking");
                        if (!str_empty(th) && target) {
                            /* foreign thinking: demote to text in the target model's thinking dialect */
                            char *txt;
                            if (str_istr(target->id, "claude")) txt = xstrdup(th);
                            else if (str_istr(target->id, "gemini")) txt = xasprintf("```thinking\n%s\n```", th);
                            else txt = xasprintf("<think>\n%s\n</think>", th);
                            json_t *tb = json_obj();
                            json_set_str(tb, "type", "text");
                            json_set_str(tb, "text", txt);
                            json_push(nc, tb);
                            free(txt);
                        }
                    }
                } else if (str_eq(t, "redactedThinking")) {
                    if (same) json_push(nc, json_clone(blk));
                } else if (str_eq(t, "toolCall")) {
                    if (stop && (strcmp(stop, "error") == 0 || strcmp(stop, "aborted") == 0)) continue;
                    json_t *tc = json_clone(blk);
                    if (!same) json_del(tc, "thoughtSignature");
                    json_push(nc, tc);
                    strvec_push(&pending_ids, json_get_str(blk, "id") ? json_get_str(blk, "id") : "");
                    strvec_push(&pending_names, json_get_str(blk, "name") ? json_get_str(blk, "name") : "");
                } else if (str_eq(t, "text")) {
                    json_t *tb = json_clone(blk);
                    if (!same) json_del(tb, "textSignature");
                    json_push(nc, tb);
                } else if (str_eq(t, "image")) {
                    json_push(nc, json_clone(blk));
                }
            }
            json_set(nm, "content", nc);
            json_push(out, nm);
        } else if (strcmp(role, "toolResult") == 0) {
            const char *id = json_get_str(msg, "toolCallId");
            bool matched = false;
            for (size_t k = 0; k < pending_ids.n; k++) {
                if (str_eq(pending_ids.v[k], id)) {
                    free(pending_ids.v[k]);
                    free(pending_names.v[k]);
                    memmove(pending_ids.v + k, pending_ids.v + k + 1, (pending_ids.n - k - 1) * sizeof(char *));
                    memmove(pending_names.v + k, pending_names.v + k + 1, (pending_names.n - k - 1) * sizeof(char *));
                    pending_ids.n--;
                    pending_names.n--;
                    matched = true;
                    break;
                }
            }
            if (!matched) continue; /* orphaned result: drop */
            json_t *nm = json_clone(msg);
            if (target && !target->input_image) {
                json_t *c = json_get(nm, "content");
                json_t *nc = json_arr();
                json_t *blk;
                bool dropped = false;
                json_arr_foreach(c, j, blk) {
                    if (str_eq(json_get_str(blk, "type"), "image")) dropped = true;
                    else json_push(nc, json_clone(blk));
                }
                if (dropped) {
                    json_t *t = json_obj();
                    json_set_str(t, "type", "text");
                    json_set_str(t, "text", "[image omitted: model does not support vision]");
                    json_push(nc, t);
                }
                json_set(nm, "content", nc);
            }
            json_push(out, nm);
        } else {
            /* user/developer: flush pending (user interrupted before results) */
            for (size_t k = 0; k < pending_ids.n; k++) {
                json_t *tr = json_obj();
                json_set_str(tr, "role", "toolResult");
                json_set_str(tr, "toolCallId", pending_ids.v[k]);
                json_set_str(tr, "toolName", pending_names.v[k]);
                json_t *c = json_arr();
                json_t *t = json_obj();
                json_set_str(t, "type", "text");
                json_set_str(t, "text", "No result provided");
                json_push(c, t);
                json_set(tr, "content", c);
                json_set_bool(tr, "isError", true);
                json_set_int(tr, "timestamp", now_ms());
                json_push(out, tr);
            }
            strvec_free(&pending_ids);
            strvec_free(&pending_names);
            json_t *nm = json_clone(msg);
            if (target && !target->input_image && json_is_arr(json_get(nm, "content"))) {
                json_t *c = json_get(nm, "content");
                json_t *nc = json_arr();
                json_t *blk;
                json_arr_foreach(c, j, blk) {
                    if (!str_eq(json_get_str(blk, "type"), "image")) json_push(nc, json_clone(blk));
                }
                json_set(nm, "content", nc);
            }
            json_push(out, nm);
        }
    }
    for (size_t k = 0; k < pending_ids.n; k++) {
        json_t *tr = json_obj();
        json_set_str(tr, "role", "toolResult");
        json_set_str(tr, "toolCallId", pending_ids.v[k]);
        json_set_str(tr, "toolName", pending_names.v[k]);
        json_t *c = json_arr();
        json_t *t = json_obj();
        json_set_str(t, "type", "text");
        json_set_str(t, "text", "No result provided");
        json_push(c, t);
        json_set(tr, "content", c);
        json_set_bool(tr, "isError", true);
        json_set_int(tr, "timestamp", now_ms());
        json_push(out, tr);
    }
    strvec_free(&pending_ids);
    strvec_free(&pending_names);
    return out;
}
