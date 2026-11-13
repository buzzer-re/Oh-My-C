#define _GNU_SOURCE
#include "ai.h"
#include "provider_transport.h"
#include "leaked_thinking.h"
#include "coding/turn_recovery.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/fs.h"

static _Thread_local const ai_model_t *g_req_model; /* the model of this thread's ai_stream, for the dump */
static _Thread_local int g_req_status;              /* the status of the attempt that failed last, 0 after a success */


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
    o->max_retries = 4; /* fetchWithRetry: 5 attempts */
    o->first_event_timeout_ms = -1;
    o->idle_timeout_ms = -1;
}

/* ---------- model compat accessors ---------- */

json_t *model_compat_get(const ai_model_t *m, const char *key) {
    if (!m) return NULL;
    json_t *v = m->compat ? json_get(m->compat, key) : NULL;
    if (!v && m->compat_defaults) v = json_get(m->compat_defaults, key);
    return v;
}

bool model_compat_bool(const ai_model_t *m, const char *key, bool def) {
    json_t *v = model_compat_get(m, key);
    return json_is_bool(v) ? v->u.b : def;
}

const char *model_compat_str(const ai_model_t *m, const char *key) { return json_sv(model_compat_get(m, key)); }

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
        if (b->defer_toolcall_end) vec_push(&b->deferred_tc, idx);
        else emit(b, AI_EV_TOOLCALL_END, idx, NULL, 0, blk, NULL);
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
    for (size_t i = 0; i < b->deferred_tc.n; i++)
        emit(b, AI_EV_TOOLCALL_END, b->deferred_tc.v[i], NULL, 0, json_at(content(b), (size_t)b->deferred_tc.v[i]), NULL);
    vec_free(&b->deferred_tc);
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
    vec_free(&b->deferred_tc);
    json_free(b->msg);
    b->msg = NULL;
    sb_free(&b->buf);
}

/* ---------- stream entry ---------- */

/* catalog.c; weak so provider unit tests can link without the catalog */
extern char *catalog_provider_disabled_error(const char *provider) __attribute__((weak));

json_t *ai_stream(const ai_model_t *model, const ai_context_t *ctx, const ai_stream_opts_t *opts, ai_event_cb cb,
                  void *ud) {
    ai_stream_fn fn;
    ai_stream_opts_t def;
    if (!opts) {
        ai_stream_opts_init(&def);
        opts = &def;
    }
    ai_stream_opts_t tiered;
    if (opts->has_service_tiers) { /* effectiveServiceTier(model): the entry for this model's family */
        tiered = *opts;
        tiered.service_tier = ai_resolve_model_service_tier(&opts->service_tiers, model);
        opts = &tiered;
    }
    /* assertProviderEnabled: a provider the auth rules turn off is never called, even with a key passed in */
    char *paused = catalog_provider_disabled_error ? catalog_provider_disabled_error(model->provider) : NULL;
    if (paused) {
        json_t *m = ai_error_message(model, "error", paused);
        json_set_int(m, "errorId", AIE_CLASS | AIE_AUTH_FAILED); /* ProviderDisabledError is a MissingApiKeyError */
        free(paused);
        if (cb) {
            ai_event_t ev = {.type = AI_EV_ERROR, .content_index = -1, .partial = m, .reason = "error"};
            cb(ud, &ev);
        }
        return m;
    }
    ai_transport_t t;
    char *terr = NULL;
    if (ai_transport_prepare(model, ctx, opts, &t, &terr) != 0) {
        json_t *m = ai_error_message(model, "error", terr ? terr : "provider configuration error");
        free(terr);
        ai_transport_free(&t);
        if (cb) {
            ai_event_t ev = {.type = AI_EV_ERROR, .content_index = -1, .partial = m, .reason = "error"};
            cb(ud, &ev);
        }
        return m;
    }
    fn = ai_lookup_api(t.model.api);
    if (!fn) {
        char *e = xasprintf("Unsupported API \"%s\" for model %s/%s", t.model.api, model->provider, model->id);
        json_t *m = ai_error_message(model, "error", e);
        free(e);
        ai_transport_free(&t);
        if (cb) {
            ai_event_t ev = {.type = AI_EV_ERROR, .content_index = -1, .partial = m, .reason = "error"};
            cb(ud, &ev);
        }
        return m;
    }
    const ai_model_t *outer = g_req_model;
    g_req_model = &t.model;
    g_req_status = 0;
    /* healLeakedThinking: leaked reasoning markup becomes thinking blocks live, except on official endpoints */
    leaked_thinking_t *heal = ai_reaches_official_endpoint(&t.model) ? NULL : leaked_thinking_new(cb, ud);
    json_t *r = heal ? leaked_thinking_finish(heal, ai_stream_guarded(fn, &t.model, ctx, &t.opts, leaked_thinking_cb, heal))
                     : ai_stream_guarded(fn, &t.model, ctx, &t.opts, cb, ud);
    leaked_thinking_free(heal);
    g_req_model = outer;
    if (g_req_status && str_eq(json_get_str(r, "stopReason"), "error") && !json_get(r, "errorStatus"))
        json_set_int(r, "errorStatus", g_req_status);
    g_req_status = 0;
    ai_transport_finish(&t, r);
    ai_transport_free(&t);
    return r;
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

char *ai_rewrite_url(const char *url) {
    const char *rw = env_get("OMC_PROVIDER_URL_REWRITE");
    char *out = NULL;
    if (rw && *rw && url) {
        strvec_t parts = str_split(rw, ",");
        for (size_t i = 0; i < parts.n && !out; i++) {
            char *eq = strchr(parts.v[i], '=');
            if (!eq) continue;
            *eq = 0;
            char *from = str_trim(parts.v[i]), *to = str_trim(eq + 1);
            if (*from && str_starts(url, from)) out = xasprintf("%s%s", to, url + strlen(from));
            free(from);
            free(to);
        }
        strvec_free(&parts);
    }
    return out ? out : xstrdup(url ? url : "");
}

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

void ai_apply_inference_headers(http_headers_t *h, const ai_model_t *m, const char *session_id, bool anthropic) {
    const char *backend = json_get_str(m->raw, "providerType") ? json_get_str(m->raw, "providerType") : m->provider;
    if (str_eq(backend, "vercel-ai-gateway")) {
        if (!http_headers_get(h, "http-referer")) http_headers_set(h, "http-referer", "https://github.com/can1357/oh-my-pi");
        if (!http_headers_get(h, "x-title")) http_headers_set(h, "x-title", OMC_NAME);
    }
    bool opencode = str_eq(backend, "opencode-go") || str_eq(backend, "opencode-zen");
    if (!str_empty(session_id)) {
        if (anthropic) http_headers_set(h, "X-Claude-Code-Session-Id", session_id);
        else if (str_eq(backend, "openai")) {
            http_headers_set(h, "session_id", session_id);
            http_headers_set(h, "x-client-request-id", session_id);
        }
    }
    if (opencode) {
        if (!http_headers_get(h, "User-Agent")) http_headers_set(h, "User-Agent", OMC_NAME "/" OMC_VERSION);
        if (!str_empty(session_id)) http_headers_set(h, "x-opencode-session", session_id);
    }
}

/* ---------- provider error text and the rejected-request dump (http-inspector.ts, openai-http.ts, classes.ts) ---------- */

static char *trimmed_copy(const char *s) {
    if (!s) return xstrdup("");
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

/* getStringProperty: a string that is not blank */
static const char *nonblank_prop(const json_t *o, const char *k) {
    const char *v = json_get_str(o, k);
    if (!v) return NULL;
    for (const char *p = v; *p; p++)
        if (!isspace((unsigned char)*p)) return v;
    return NULL;
}

/* createOpenAIHttpError's message merged with the captured body: `<status> <detail>` carrying the core message once,
 * plus the ` (type=… param=… code=…)` suffix of the payload (numbered by what is present, as formatCapturedHttpErrorParts) */
static char *openai_error_text(int status, const char *raw) {
    char *body = trimmed_copy(raw);
    if (!*body) {
        free(body);
        return xasprintf("%d status code (no body)", status);
    }
    json_t *j = json_parse_str(body);
    const json_t *obj = json_is_obj(j) ? j : NULL;
    const json_t *eo = json_get_obj(obj, "error");
    const char *es = json_get_str(obj, "error");
    const char *emsg = eo ? json_get_str(eo, "message") : NULL;
    /* parseEnvelope */
    const char *detail = body;
    if (eo) detail = emsg && *emsg ? emsg : body;
    else if (es && *es) detail = es;
    else if (json_get_str(obj, "message") && *json_get_str(obj, "message")) detail = json_get_str(obj, "message");
    /* formatCapturedHttpErrorParts */
    const char *core = body;
    sb_t suffix = SB_INIT;
    if (obj) {
        const json_t *ep = eo ? eo : obj;
        core = nonblank_prop(ep, "message");
        if (!core) core = nonblank_prop(obj, "message");
        if (!core && !eo) core = nonblank_prop(obj, "error");
        if (!core) core = body;
        static const char *const keys[] = {"type", "param", "code"};
        int n = 0;
        for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
            const char *v = nonblank_prop(ep, keys[i]);
            if (!v) v = nonblank_prop(obj, keys[i]);
            if (v) {
                sb_appendf(&suffix, "%s%s=%s", n ? " " : " (", keys[n], v);
                n++;
            }
        }
        if (n) sb_putc(&suffix, ')');
    }
    /* mergeCapturedBody */
    char *msg = xasprintf("%d %s", status, detail);
    char *r;
    if (strstr(msg, core)) r = suffix.len && !strstr(msg, suffix.s) ? xasprintf("%s%s", msg, suffix.s) : xstrdup(msg);
    else r = xasprintf("%s\n%s%s", msg, core, suffix.len ? suffix.s : "");
    free(msg);
    sb_free(&suffix);
    /* readOpenRouterRawMetadata: some providers behind OpenRouter explain themselves here */
    const char *rawm = nonblank_prop(json_get_obj(eo, "metadata"), "raw");
    if (rawm) {
        char *t = trimmed_copy(rawm);
        char *r2 = xasprintf("%s\n%s", r, t);
        free(t);
        free(r);
        r = r2;
    }
    json_free(j);
    free(body);
    return r;
}

/* formatCapturedHttpErrorParts' text: core message and type/param/code suffix, "" for a blank body. Owned. */
static char *captured_error_text(const char *raw) {
    char *body = trimmed_copy(raw);
    if (!*body) return body;
    json_t *j = json_parse_str(body);
    const json_t *obj = json_is_obj(j) ? j : NULL;
    sb_t b = SB_INIT;
    if (!obj) {
        sb_append(&b, body);
    } else {
        const json_t *eo = json_get_obj(obj, "error");
        const json_t *ep = eo ? eo : obj;
        const char *core = nonblank_prop(ep, "message");
        if (!core) core = nonblank_prop(obj, "message");
        if (!core && !eo) core = nonblank_prop(obj, "error");
        sb_append(&b, core ? core : body);
        static const char *const keys[] = {"type", "param", "code"};
        int n = 0;
        for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
            const char *v = nonblank_prop(ep, keys[i]);
            if (!v) v = nonblank_prop(obj, keys[i]);
            if (v) {
                sb_appendf(&b, "%s%s=%s", n ? " " : " (", keys[n], v);
                n++;
            }
        }
        if (n) sb_putc(&b, ')');
    }
    json_free(j);
    free(body);
    return sb_detach(&b);
}

/* AnthropicApiError.fromResponse: the body as it came, cut at 64 KiB */
static char *anthropic_error_text(int status, const char *raw) {
    char *body = trimmed_copy(raw);
    char *r;
    if (!*body) r = xasprintf("%d status code (no body)", status);
    else if (strlen(body) > 64 * 1024) r = xasprintf("%d %.*s\n[Response body truncated after 64 KiB]", status, 64 * 1024, body);
    else r = xasprintf("%d %s", status, body);
    free(body);
    return r;
}

/* OpenAIHttpError's message alone: azure-openai-responses hands finalize no captured response */
static char *azure_error_text(int status, const char *raw) {
    char *body = trimmed_copy(raw);
    char *r;
    if (!*body) {
        r = xasprintf("%d status code (no body)", status);
    } else {
        json_t *j = json_parse_str(body);
        const json_t *eo = json_is_obj(j) ? json_get_obj(j, "error") : NULL;
        const char *es = json_is_obj(j) ? json_get_str(j, "error") : NULL;
        const char *top = json_is_obj(j) ? json_get_str(j, "message") : NULL;
        const char *detail = body;
        if (eo) detail = json_get_str(eo, "message") && *json_get_str(eo, "message") ? json_get_str(eo, "message") : body;
        else if (es && *es) detail = es;
        else if (top && *top) detail = top;
        r = xasprintf("%d %.4096s", status, detail); /* MAX_DETAIL_CHARS */
        json_free(j);
    }
    free(body);
    return r;
}

/* OllamaApiError over the captured body (mergeCapturedBody: the message never holds it) */
static char *ollama_error_text(int status, const char *url, const char *raw) {
    char *captured = captured_error_text(raw);
    char *r = *captured ? xasprintf("HTTP %d from %s\n%s", status, url, captured) : xasprintf("HTTP %d from %s", status, url);
    free(captured);
    return r;
}

const char *ai_http_status_text(int status) {
    switch (status) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
    }
}

/* parseCodexError: friendlyMessage || error.message || the raw body || the status text */
static char *codex_error_text(int status, const char *raw, const http_headers_t *h) {
    const char *body = raw ? raw : "";
    const char *status_text = ai_http_status_text(status);
    json_t *j = json_parse_str(body);
    const json_t *err = json_is_obj(j) ? json_get_obj(j, "error") : NULL;
    const char *code = json_get_str(err, "code");
    if (!code) code = json_get_str(err, "type");
    bool have_resets = json_is_num(json_get(err, "resets_at"));
    double err_resets = json_get_num(err, "resets_at", 0);
    double primary = 0, secondary = 0, pused = -1, sused = -1;
    const char *v;
    if ((v = http_headers_get(h, "x-codex-primary-reset-at"))) primary = atof(v);
    if ((v = http_headers_get(h, "x-codex-secondary-reset-at"))) secondary = atof(v);
    if ((v = http_headers_get(h, "x-codex-primary-used-percent"))) pused = atof(v);
    if ((v = http_headers_get(h, "x-codex-secondary-used-percent"))) sused = atof(v);
    char *friendly = NULL;
    double resets = 0;
    if (code && (str_istr(code, "usage_limit_reached") || str_istr(code, "usage_not_included"))) {
        const char *plan = json_get_str(err, "plan_type");
        resets = have_resets ? err_resets : primary ? primary : secondary;
        sb_t b = SB_INIT;
        sb_append(&b, "You have hit your ChatGPT usage limit");
        if (plan && *plan) {
            char *low = xstrdup(plan);
            for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
            sb_appendf(&b, " (%s plan)", low);
            free(low);
        }
        sb_putc(&b, '.');
        if (resets > 0) sb_appendf(&b, " Try again in ~%lld min.", (long long)MAX(0.0, llround((resets * 1000 - (double)now_ms()) / 60000.0)));
        friendly = sb_detach(&b);
    } else if (j && ((code && str_istr(code, "rate_limit_exceeded")) || status == 429)) {
        /* only the error's own resets_at or an exhausted window's reset; with both spent the later one */
        double exhausted = 0;
        if (pused >= 100 && primary > 0) exhausted = MAX(exhausted, primary);
        if (sused >= 100 && secondary > 0) exhausted = MAX(exhausted, secondary);
        resets = have_resets ? err_resets : exhausted;
        sb_t b = SB_INIT;
        sb_append(&b, "ChatGPT rate limit exceeded.");
        if (resets > 0) sb_appendf(&b, " Try again in ~%lld min.", (long long)MAX(0.0, llround((resets * 1000 - (double)now_ms()) / 60000.0)));
        friendly = sb_detach(&b);
    }
    char *r;
    const char *emsg = json_get_str(err, "message");
    if (friendly) r = friendly;
    else if (emsg && *emsg) r = xstrdup(emsg);
    else if (*body) r = xstrdup(body);
    else r = xstrdup(*status_text ? status_text : "Request failed");
    json_free(j);
    return r;
}

static char *http_error_text(int policy, int status, const char *url, const http_headers_t *h, const char *body) {
    switch (policy) {
    case AI_RETRY_ANTHROPIC: return anthropic_error_text(status, body);
    case AI_RETRY_OPENAI:
        return g_req_model && str_eq(g_req_model->api, "azure-openai-responses") ? azure_error_text(status, body) : openai_error_text(status, body);
    case AI_RETRY_OLLAMA: return ollama_error_text(status, url, body);
    case AI_RETRY_CODEX: return codex_error_text(status, body, h);
    default: return ai_extract_error(status, body);
    }
}

char *ai_rewrap_http_error(const char *text, const char *err) {
    const char *dump = err ? strstr(err, "\nraw-http-request") : NULL;
    const char *hint = err ? strstr(err, " retry-after-ms=") : NULL;
    if (hint && dump && hint > dump) hint = NULL;
    size_t hint_len = !hint ? 0 : dump ? (size_t)(dump - hint) : strlen(hint);
    return xasprintf("%s%.*s%s", text, (int)hint_len, hint ? hint : "", dump ? dump : "");
}

/* pruneHttpRequestDumps: walking newest first, `*.json` older than 7 days or past the 64 MiB running total go */
typedef struct {
    char *name;
    int64_t size;
    time_t mtime;
} req_dump_t;

static int req_dump_newest_first(const void *pa, const void *pb) {
    const req_dump_t *a = pa, *b = pb;
    if (a->mtime != b->mtime) return a->mtime > b->mtime ? -1 : 1;
    return -strcmp(a->name, b->name);
}

static void prune_request_dumps(const char *dir, const char *keep) {
    DIR *d = opendir(dir);
    if (!d) return;
    VEC(req_dump_t) v = {0};
    time_t now = time(NULL);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!str_ends(e->d_name, ".json")) continue;
        char *p = path_join(dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            req_dump_t x = {xstrdup(e->d_name), st.st_size, st.st_mtime};
            vec_push(&v, x);
        }
        free(p);
    }
    closedir(d);
    qsort(v.v, v.n, sizeof *v.v, req_dump_newest_first);
    int64_t retained = 0;
    for (size_t i = 0; i < v.n; i++) {
        bool stay = str_eq(v.v[i].name, keep) || (now - v.v[i].mtime <= 7 * 24 * 3600 && retained + v.v[i].size <= 64LL * 1024 * 1024);
        if (stay) {
            retained += v.v[i].size;
        } else {
            char *p = path_join(dir, v.v[i].name);
            unlink(p);
            free(p);
        }
        free(v.v[i].name);
    }
    free(v.v);
}

/* appendRawHttpRequestDumpFor400: a 400 or 413 keeps its request under <root>/logs/http-400-requests, credentials
 * redacted, and the error names the file. Takes msg, returns the text to use. */
static char *with_request_dump(char *msg, int status, const char *url, const http_headers_t *headers, const char *body) {
    if ((status != 400 && status != 413) || !msg || !g_req_model) return msg;
    json_t *o = json_obj();
    json_set_str(o, "provider", g_req_model->provider);
    json_set_str(o, "api", g_req_model->api);
    json_set_str(o, "model", g_req_model->id);
    json_set_str(o, "method", "POST");
    const char *q = strchr(url, '?');
    if (q) json_set(o, "url", json_str_own(xasprintf("%.*s[redacted-query]", (int)(q - url), url)));
    else json_set_str(o, "url", url);
    json_t *hs = json_obj();
    static const char *const sensitive[] = {"key", "token", "secret", "auth", "credential", "cookie"};
    for (size_t i = 0; headers && i < headers->n; i++) {
        bool redact = false;
        for (size_t k = 0; k < ARRAY_LEN(sensitive) && !redact; k++) redact = str_istr(headers->v[i].name, sensitive[k]) != NULL;
        json_set_str(hs, headers->v[i].name, redact ? "[redacted]" : headers->v[i].value);
    }
    json_set(o, "headers", hs);
    json_t *b = body ? json_parse_str(body) : NULL;
    if (b) json_set(o, "body", b);
    else if (body) json_set_str(o, "body", body);
    json_t *er = json_obj();
    json_set_int(er, "status", status);
    json_set_str(er, "message", msg);
    json_set(o, "errorResponse", er);
    char *text = json_dump(o, 2);
    json_free(o);
    uint64_t h = 1469598103934665603ULL; /* FNV-1a: only names the file */
    for (const char *p = text; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    char id[16];
    size_t n = 0;
    do {
        id[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[h % 36];
        h /= 36;
    } while (h && n < sizeof id - 1);
    id[n] = 0;
    char *root = path_join(omc_config_dir(), "logs/http-400-requests");
    char *name = xasprintf("%lld-%s.json", (long long)now_ms(), id);
    char *file = path_join(root, name);
    char *content = xasprintf("%s\n", text);
    fs_mkdir_p(root, 0700);
    char *r;
    if (fs_write_atomic(file, content, strlen(content), 0600) == 0) {
        prune_request_dumps(root, name);
        r = xasprintf("%s\nraw-http-request=%s", msg, file);
    } else {
        r = xasprintf("%s\nraw-http-request-save-failed=%s", msg, strerror(errno));
    }
    free(content);
    free(file);
    free(name);
    free(root);
    free(text);
    free(msg);
    return r;
}

char *ai_strip_request_dump_lines(const char *msg) {
    if (!msg) return NULL;
    char *r = xstrdup(msg);
    for (;;) {
        char *nl = strrchr(r, '\n');
        char *last = nl ? nl + 1 : r;
        if (!str_starts(last, "raw-http-request=") && !str_starts(last, "raw-http-request-save-failed=")) break;
        *last = 0;
        if (!nl) break;
        *nl = 0;
    }
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
    http_headers_t hdrs; /* headers of a failed (non-2xx) attempt: retry hints */
    ai_post_ex_t *ex;
} sse_ctx_t;

static int on_hdr(void *ud, int status, const http_headers_t *h) {
    sse_ctx_t *c = ud;
    c->status = status;
    c->ok = status >= 200 && status < 300;
    if (c->ex && c->ex->on_headers) c->ex->on_headers(c->ex->hook_ud, status, h);
    if (!c->ok && h)
        for (size_t i = 0; i < h->n; i++) http_headers_add(&c->hdrs, h->v[i].name, h->v[i].value);
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

static bool copilot_model_not_supported(const char *body) {
    json_t *j = body ? json_parse_str(body) : NULL;
    bool r = str_eq(json_get_str(json_get_obj(j, "error"), "code"), "model_not_supported");
    json_free(j);
    return r;
}


/* byte-level read timeout: 300 s, widened so it never fires before the stream watchdog (stream_guard.c) */
static int64_t http_idle_ms(const ai_stream_opts_t *opts, const ai_post_ex_t *ex) {
    int64_t t = ex && ex->idle_timeout_ms > 0 ? ex->idle_timeout_ms : 300000;
    if (opts && (opts->first_event_timeout_ms == 0 || opts->idle_timeout_ms == 0)) return MAX(t, 24LL * 3600 * 1000);
    if (opts && opts->first_event_timeout_ms > t) t = opts->first_event_timeout_ms;
    if (opts && opts->idle_timeout_ms > t) t = opts->idle_timeout_ms;
    return t;
}

/* transport retry policies (ai.h AI_RETRY_*) */
static int64_t policy_default_delay(int policy, int attempt) {
    static const int64_t ollama[] = {2000, 5000, 10000}; /* OLLAMA_RETRY_DELAYS_MS */
    int n = MIN(attempt, 30);
    switch (policy) {
    case AI_RETRY_OLLAMA: return ollama[MIN(attempt, 2)];
    case AI_RETRY_CODEX: return 500LL * (attempt + 1);     /* CODEX_RETRY_DELAY_MS * (attempt + 1) */
    case AI_RETRY_GEMINI_CLI: return 1000LL << n;          /* BASE_DELAY_MS * 2 ** attempt */
    default: return 500LL << n;                            /* fetchWithRetry: 500 ms * 2 ** attempt */
    }
}

/* calculateAnthropicRetryDelayMs: min(0.5 s * 2^n, 8 s) with up to 25% downward jitter */
static int64_t anthropic_backoff_ms(int attempt) {
    double secs = MIN(0.5 * (double)(1LL << MIN(attempt, 30)), 8.0);
    double jitter = 1 - ((double)rand() / ((double)RAND_MAX + 1)) * 0.25;
    return (int64_t)(secs * jitter * 1000);
}

/* LiteLLM concurrency-admission 429 (`rate_limit_type: max_parallel_requests`): session recovery owns it */
static bool admission_rejection(const http_headers_t *h, const char *body) {
    const char *t = http_headers_get(h, "rate_limit_type");
    if (t) {
        while (*t == ' ') t++;
        size_t n = strlen(t);
        while (n && t[n - 1] == ' ') n--;
        if (n == strlen("max_parallel_requests") && !strncmp(t, "max_parallel_requests", n)) return true;
    }
    const char *p = body ? strstr(body, "\"rate_limit_type\"") : NULL;
    if (!p) return false;
    p += strlen("\"rate_limit_type\"");
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return !strncmp(p, "\"max_parallel_requests\"", strlen("\"max_parallel_requests\""));
}

/* formatErrorMessageWithRetryAfter: a final HTTP error carries the header timing as " retry-after-ms=N" */
static char *with_retry_after(char *msg, const http_headers_t *h) {
    if (!msg || strstr(msg, "retry-after-ms=")) return msg;
    int64_t ms = ai_retry_after_from_headers(h);
    if (ms < 0) return msg;
    char *r = xasprintf("%s retry-after-ms=%lld", msg, (long long)ms);
    free(msg);
    return r;
}

static void ctx_reset(sse_ctx_t *c, http_resp_t *resp) {
    http_resp_free(resp);
    sse_free(&c->sse);
    sb_free(&c->errbody);
    http_headers_free(&c->hdrs);
}

/* error/network-cause.ts networkFailureMessage over the transport's own wording (net/http.c). `requestUrl`'s host wins. */
char *ai_network_failure_message(const char *http_error, const char *url) {
    const char *e = http_error ? http_error : "";
    const char *kind = NULL, *detail = NULL;
    if (str_starts(e, "TLS certificate verification failed for ")) {
        kind = "tls";
        const char *sep = strstr(e, ": ");
        detail = sep ? sep + 2 : NULL;
    } else if (str_starts(e, "DNS lookup failed for ")) {
        if (strstr(e, "Temporary failure")) {
            kind = "offline";
            detail = "getaddrinfo EAI_AGAIN";
        } else {
            kind = "unreachable";
            detail = "getaddrinfo ENOTFOUND";
        }
    } else if (str_starts(e, "connect to ")) {
        if (strstr(e, "Connection refused")) kind = "refused";
        else if (strstr(e, "Network is unreachable")) kind = "offline", detail = "ENETUNREACH";
        else if (strstr(e, "No route to host")) kind = "offline", detail = "EHOSTUNREACH";
        else if (strstr(e, "timed out")) kind = "offline", detail = "ETIMEOUT";
    }
    if (!kind) return xstrdup(*e ? e : "network error");
    url_t u;
    char *host = NULL;
    if (url && url_parse(url, &u) == 0) {
        bool def_port = (str_eq(u.scheme, "https") && u.port == 443) || (str_eq(u.scheme, "http") && u.port == 80);
        host = def_port ? xstrdup(u.host) : xasprintf("%s:%d", u.host, u.port);
        url_free(&u);
    }
    char *d = detail ? xasprintf(" (%s)", detail) : xstrdup("");
    const char *h = host ? host : "the API host";
    char *msg;
    if (str_eq(kind, "offline"))
        msg = xasprintf("No internet connection: couldn't reach %s%s. Check your network or proxy and try again.", h, d);
    else if (str_eq(kind, "unreachable")) msg = xasprintf("Couldn't reach %s%s. Check the URL, your network, or proxy.", h, d);
    else if (str_eq(kind, "refused"))
        msg = xasprintf("Couldn't connect to %s (connection refused). Check that the server is running and reachable.", h);
    else
        msg = xasprintf("TLS certificate verification failed for %s%s. If you're behind a TLS-inspecting proxy, its CA may need "
                        "to be trusted.", h, d);
    free(d);
    free(host);
    return msg;
}

/* the transports omp re-describes a rejected fetch on: postOpenAIStream, Codex, the Anthropic connection error */
static char *network_failure_message(const char *http_error, const char *url, int policy) {
    if (policy == AI_RETRY_OPENAI || policy == AI_RETRY_CODEX || policy == AI_RETRY_ANTHROPIC)
        return ai_network_failure_message(http_error, url);
    return xstrdup(http_error ? http_error : "network error");
}

static int stream_post_url(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                           ai_sse_cb sse_cb, http_data_cb raw_cb, void *ud, int *status, char **err, ai_post_ex_t *ex) {
    if (!http_headers_get(headers, "Content-Type")) http_headers_add(headers, "Content-Type", "application/json");
    if (sse_cb && !http_headers_get(headers, "Accept")) http_headers_add(headers, "Accept", "text/event-stream");
    int max_retries = opts && opts->max_retries > 0 ? opts->max_retries : 0;
    int policy = opts ? opts->retry_policy : AI_RETRY_FETCH;
    /* the per-delay cap: maxRetryDelayMs for anthropic / gemini-cli, fetchWithRetry's 60 s default elsewhere */
    int64_t max_delay = 60000;
    if ((policy == AI_RETRY_ANTHROPIC || policy == AI_RETRY_GEMINI_CLI || policy == AI_RETRY_CODEX) && opts &&
        opts->max_retry_delay_ms)
        max_delay = opts->max_retry_delay_ms;
    *err = NULL;
    *status = 0;
    const char *identity_retry = NULL; /* Copilot identity the one-shot denial retry switched to */
    for (int attempt = 0;; attempt++) {
        sse_ctx_t c = {0};
        c.sse_cb = sse_cb;
        c.raw_cb = raw_cb;
        c.ud = ud;
        c.ex = ex;
        if (sse_cb) sse_init(&c.sse, (sse_event_cb)sse_cb, ud);
        http_req_t req = {.method = "POST",
                          .url = url,
                          .headers = headers,
                          .body = body,
                          .body_len = strlen(body),
                          .idle_timeout_ms = http_idle_ms(opts, ex),
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
            ctx_reset(&c, &resp);
            return -2;
        }
        if (identity_retry && opts->copilot_cache_key && rc == 0)
            ai_copilot_cache_set(opts->copilot_cache_key, c.ok ? identity_retry : NULL);
        if (rc == 0 && c.ok) {
            int ret = 0;
            if (c.abort_rc) ret = c.abort_rc;
            else if (resp.error) {
                *err = xstrdup(resp.error);
                ret = -1;
            }
            ctx_reset(&c, &resp);
            return ret;
        }
        /* GitHub Copilot client-identity denial (wrapFetchForCopilotFallback): retry once as the other surface */
        if (opts && opts->copilot_identity_retry && !identity_retry && rc == 0 &&
            (c.status == 403 || (c.status == 400 && copilot_model_not_supported(sb_cstr(&c.errbody))))) {
            const char *out = http_headers_get(headers, "Copilot-Integration-Id");
            const char *alt = NULL;
            if (str_eq(out, "copilot-chat")) alt = "copilot-developer-cli";
            else if (str_eq(out, "copilot-developer-cli") && opts->copilot_cache_key &&
                     str_eq(opts->copilot_cache_snapshot, "copilot-developer-cli"))
                alt = "copilot-chat";
            if (alt) {
                LOGW("GitHub Copilot %s identity denied (HTTP %d); retrying once as %s",
                     str_eq(alt, "copilot-chat") ? "CLI" : "chat", c.status, str_eq(alt, "copilot-chat") ? alt : "the Copilot CLI");
                http_headers_set(headers, "Copilot-Integration-Id", alt);
                identity_retry = alt;
                ctx_reset(&c, &resp);
                attempt--;
                continue;
            }
        }
        /* failure: a caller hook decides first (slow-mode lanes, rejected options) */
        if (ex && ex->on_failure && !c.got_data) {
            int64_t d = ex->on_failure(ex->hook_ud, rc == 0 ? c.status : 0, sb_cstr(&c.errbody), headers);
            if (d >= 0) {
                ctx_reset(&c, &resp);
                int64_t until = mono_ms() + d;
                while (mono_ms() < until) {
                    if (opts && opts->cancel && *opts->cancel) {
                        *err = xstrdup("Request was aborted");
                        return -2;
                    }
                    sleep_ms(MIN((int64_t)50, MAX((int64_t)1, until - mono_ms())));
                }
                attempt--;
                continue;
            }
            if (d == -2) {
                *err = rc != 0 ? network_failure_message(resp.error, url, policy) : http_error_text(policy, c.status, url, &c.hdrs, sb_cstr(&c.errbody));
                if (rc == 0) {
                    g_req_status = c.status;
                    *err = with_retry_after(*err, &c.hdrs);
                    *err = with_request_dump(*err, c.status, url, headers, body);
                }
                free(ex->err_body);
                ex->err_body = rc == 0 ? xstrdup(sb_cstr(&c.errbody)) : NULL;
                ctx_reset(&c, &resp);
                return -1;
            }
        }
        /* failure: decide retry (fetchWithRetry / the anthropic.ts provider loop) */
        const char *ebody = sb_cstr(&c.errbody);
        bool http_fail = rc == 0;
        char *msg = !http_fail ? network_failure_message(resp.error, url, policy) : http_error_text(policy, c.status, url, &c.hdrs, ebody);
        bool retry;
        int64_t delay;
        if (policy == AI_RETRY_ANTHROPIC) {
            retry = !c.got_data && (!http_fail || ai_provider_retryable_error(msg, c.status));
            /* a captured x-should-retry overrides the status and text heuristics, usage limits still stop */
            int verdict = http_fail ? ai_should_retry_verdict(&c.hdrs) : -1;
            if (verdict >= 0 && !c.got_data && !ai_usage_limit_text(msg, c.status)) retry = verdict == 1;
            int64_t backoff = anthropic_backoff_ms(attempt), hdr = http_fail ? ai_retry_after_from_headers(&c.hdrs) : -1;
            if (retry && hdr >= 0 && max_delay > 0 && hdr > max_delay) retry = false; /* over-cap hint: surface now */
            delay = hdr >= 0 ? MAX(hdr, backoff) : backoff;
        } else {
            retry = !c.got_data && (!http_fail || c.status >= 500 || c.status == 408 || c.status == 429);
            /* postOpenAIStream: a gateway's `x-should-retry: false` is terminal, as is a concurrency-admission 429 */
            if (retry && http_fail && policy == AI_RETRY_OPENAI &&
                (ai_should_retry_verdict(&c.hdrs) == 0 || admission_rejection(&c.hdrs, ebody)))
                retry = false;
            if (retry && http_fail && policy == AI_RETRY_OLLAMA && c.status >= 500 &&
                (str_istr(ebody, "failed to parse tool call arguments as json") ||
                 str_istr(ebody, "[json.exception.parse_error.101]")))
                retry = false;
            int64_t hint = http_fail ? ai_extract_retry_hint(&c.hdrs, ebody) : -1;
            if (retry && hint >= 0 && max_delay >= 0 && hint > max_delay) retry = false; /* caller deals with it */
            delay = hint >= 0 ? hint : policy_default_delay(policy, attempt);
            if (max_delay >= 0) delay = MIN(delay, max_delay);
        }
        if (http_fail) msg = with_retry_after(msg, &c.hdrs);
        if (ex) {
            free(ex->err_body);
            ex->err_body = http_fail ? xstrdup(ebody) : NULL;
        }
        ctx_reset(&c, &resp);
        if (opts && opts->copilot_cache_key && http_fail && (c.status == 401 || c.status == 403)) {
            /* http-inspector.ts rewriteCopilotError */
            free(msg);
            msg = xstrdup(c.status == 401
                              ? "GitHub Copilot authentication failed (HTTP 401). Your token may have been revoked. Please re-login "
                                "with /login github-copilot"
                              : "GitHub Copilot access denied (HTTP 403). Your token is valid but the account may not have access to "
                                "this model or feature. Check your Copilot plan or model policy settings. Business organizations can "
                                "also restrict which clients may call the API: omc sends Copilot-Integration-Id copilot-chat by "
                                "default (COPILOT_INTEGRATION_ID overrides it) and retries a denied default-identity request once as "
                                "the Copilot CLI (copilot-developer-cli). If both identities are denied, ask your org admin to allow "
                                "one of them; if you pinned an identity, try the other.");
            retry = false;
        }
        if (!retry || attempt >= max_retries) {
            if (http_fail) {
                g_req_status = c.status;
                msg = with_request_dump(msg, c.status, url, headers, body);
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
            sleep_ms(MIN((int64_t)50, MAX((int64_t)1, until - mono_ms())));
        }
    }
}

static int stream_post(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                       ai_sse_cb sse_cb, http_data_cb raw_cb, void *ud, int *status, char **err, ai_post_ex_t *ex) {
    char *u = ai_rewrite_url(url);
    int rc = stream_post_url(u, headers, body, opts, sse_cb, raw_cb, ud, status, err, ex);
    free(u);
    return rc;
}

int ai_sse_post(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                ai_sse_cb on_event, void *ud, int *status, char **err) {
    return stream_post(url, headers, body, opts, on_event, NULL, ud, status, err, NULL);
}

int ai_sse_post_ex(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                   ai_sse_cb on_event, void *ud, int *status, char **err, ai_post_ex_t *ex) {
    return stream_post(url, headers, body, opts, on_event, NULL, ud, status, err, ex);
}

int ai_stream_post(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                   http_data_cb on_data, void *ud, int *status, char **err) {
    return stream_post(url, headers, body, opts, NULL, on_data, ud, status, err, NULL);
}

int ai_stream_post_ex(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                      http_data_cb on_data, void *ud, int *status, char **err, ai_post_ex_t *ex) {
    return stream_post(url, headers, body, opts, NULL, on_data, ud, status, err, ex);
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
    bool pending_aborted = false; /* the pending calls belong to an errored/aborted turn: missing results read "aborted" */
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
                json_set_str(t, "text", pending_aborted ? "aborted" : "No result provided");
                json_push(c, t);
                json_set(tr, "content", c);
                json_set_bool(tr, "isError", true);
                json_set_int(tr, "timestamp", now_ms());
                json_push(out, tr);
            }
            strvec_free(&pending_ids);
            strvec_free(&pending_names);
            const char *stop = json_get_str(msg, "stopReason");
            bool truncated = str_eq(stop, "error") || str_eq(stop, "aborted");
            pending_aborted = truncated;
            if (truncated || str_eq(stop, "length")) {
                /* shouldDropTruncatedThinkingOnlyAssistant: a cut-off turn without text or tool calls is dropped; an
                 * errored turn keeps its calls, paired with their (synthetic) results or "aborted" */
                bool keep = false;
                json_t *blk;
                json_arr_foreach(json_get(msg, "content"), j, blk) {
                    const char *bt = json_get_str(blk, "type");
                    if (str_eq(bt, "text") || str_eq(bt, "toolCall")) keep = true;
                }
                if (!keep) continue;
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
                json_set_str(t, "text", pending_aborted ? "aborted" : "No result provided");
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
        json_set_str(t, "text", pending_aborted ? "aborted" : "No result provided");
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
