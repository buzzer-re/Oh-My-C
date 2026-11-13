/*
 * provider_ollama.c - Ollama native chat (api "ollama-chat"): POST {base}/api/chat, NDJSON stream.
 * Port of packages/ai/src/providers/ollama.ts. Leaked-markup healing runs only its ThinkingInbandScanner (the kimi/dsml
 * tool-call grammars of StreamMarkupHealing are not ported).
 */
#include "ai/provider_openai.h"
#include "ai/think_heal.h"

#include <math.h>

#define OLLAMA_MAX_LINE (32u << 20)

static char *ollama_base(const ai_model_t *m) {
    char *b = oai_base_url(m, "https://ollama.com");
    if (str_ends(b, "/api")) b[strlen(b) - 4] = 0;
    return b;
}

static const char *map_think(const ai_model_t *m, const ai_stream_opts_t *o, bool *as_bool, bool *bval) {
    *as_bool = false;
    if (o->disable_reasoning && m->reasoning) {
        *as_bool = true;
        *bval = false;
        return NULL;
    }
    if (o->reasoning == EFFORT_OFF) return NULL;
    const char *e = effort_name(o->reasoning);
    if (str_eq(m->provider, "ollama-cloud")) {
        const char *mapped = json_get_str(json_get_obj(m->thinking, "effortMap"), e);
        if (mapped) e = mapped;
    }
    if (str_eq(e, "minimal") || str_eq(e, "low")) return "low";
    if (str_eq(e, "medium")) return "medium";
    if (str_eq(e, "high") || str_eq(e, "xhigh")) return "high";
    if (str_eq(e, "max")) return "max";
    return NULL;
}

/* {content, images?} from string or [Text|Image] */
static void plain_content(json_t *dst, const json_t *content, bool images_ok) {
    if (json_is_str(content)) {
        json_set_str(dst, "content", json_sv(content));
        return;
    }
    sb_t t = SB_INIT;
    json_t *imgs = json_arr();
    bool omitted = false, first = true;
    json_t *b;
    json_arr_foreach(content, i, b) {
        const char *ty = json_get_str(b, "type");
        if (str_eq(ty, "text")) {
            if (!first) sb_putc(&t, '\n');
            sb_append(&t, json_get_str(b, "text") ? json_get_str(b, "text") : "");
            first = false;
        } else if (str_eq(ty, "image")) {
            if (images_ok && json_get_str(b, "data")) json_push_str(imgs, json_get_str(b, "data"));
            else omitted = true;
        }
    }
    if (omitted) {
        if (t.len) sb_putc(&t, '\n');
        sb_append(&t, "[image omitted: model does not support vision]");
    }
    json_set(dst, "content", json_str_own(sb_detach(&t)));
    if (json_len(imgs)) json_set(dst, "images", imgs);
    else json_free(imgs);
}

json_t *ollama_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o) {
    json_t *p = json_obj();
    json_set_str(p, "model", m->id);
    json_t *msgs = json_arr();
    bool images_ok = m->input_image;
    json_t *sys = oai_system_prompts(ctx), *s;
    size_t nsys = json_len(sys);
    json_arr_foreach(sys, i, s) {
        json_t *o2 = json_obj();
        json_set_str(o2, "role", "system");
        json_set_str(o2, "content", json_sv(s));
        json_push(msgs, o2);
    }
    json_free(sys);
    /* thinking history is stripped unless compat.stripThinkingHistory === false */
    bool strip_thinking = model_compat_bool(m, "stripThinkingHistory", true);
    json_t *tm = ai_transform_messages(ctx->messages, m), *msg;
    json_arr_foreach(tm, i, msg) {
        const char *role = json_get_str(msg, "role");
        json_t *om = json_obj();
        if (str_eq(role, "user") || str_eq(role, "developer")) {
            json_set_str(om, "role", str_eq(role, "user") ? "user" : "system");
            plain_content(om, json_get(msg, "content"), images_ok);
        } else if (str_eq(role, "toolResult")) {
            json_set_str(om, "role", "tool");
            if (json_get_str(msg, "toolName")) json_set_str(om, "tool_name", json_get_str(msg, "toolName"));
            plain_content(om, json_get(msg, "content"), images_ok);
        } else if (str_eq(role, "assistant")) {
            sb_t text = SB_INIT, think = SB_INIT;
            int nt = 0, nk = 0;
            json_t *calls = json_arr(), *b;
            json_arr_foreach(json_get(msg, "content"), j, b) {
                const char *ty = json_get_str(b, "type");
                if (str_eq(ty, "text")) {
                    if (nt++) sb_putc(&text, '\n');
                    sb_append(&text, json_get_str(b, "text") ? json_get_str(b, "text") : "");
                } else if (str_eq(ty, "thinking")) {
                    if (nk++) sb_putc(&think, '\n');
                    sb_append(&think, json_get_str(b, "thinking") ? json_get_str(b, "thinking") : "");
                } else if (str_eq(ty, "toolCall")) {
                    json_t *c = json_obj(), *fn = json_obj();
                    json_set_str(c, "type", "function");
                    json_set_str(fn, "name", json_get_str(b, "name") ? json_get_str(b, "name") : "");
                    json_t *args = json_get(b, "arguments");
                    json_set(fn, "arguments", json_is_obj(args) ? json_clone(args) : json_obj());
                    json_set(c, "function", fn);
                    json_push(calls, c);
                }
            }
            json_set_str(om, "role", "assistant");
            json_set(om, "content", json_str_own(sb_detach(&text)));
            if (nk && !strip_thinking) json_set(om, "thinking", json_str_own(sb_detach(&think)));
            sb_free(&think);
            if (json_len(calls)) json_set(om, "tool_calls", calls);
            else json_free(calls);
        } else {
            json_free(om);
            continue;
        }
        json_push(msgs, om);
    }
    json_free(tm);
    /* Ollama generates nothing without a user turn: demote the last non-prefix system turn */
    bool has_user = false;
    json_arr_foreach(msgs, i, msg) {
        if (str_eq(json_get_str(msg, "role"), "user")) has_user = true;
    }
    if (!has_user) {
        for (size_t i = json_len(msgs); i > nsys; i--) {
            json_t *mm = json_at(msgs, i - 1);
            if (str_eq(json_get_str(mm, "role"), "system")) {
                json_set_str(mm, "role", "user");
                break;
            }
        }
    }
    json_set(p, "messages", msgs);
    /* tools (named tool_choice narrows the list) */
    const char *named = oai_tool_choice_name(o->tool_choice);
    if (json_len(ctx->tools)) {
        json_t *tools = json_arr(), *t;
        json_arr_foreach(ctx->tools, i, t) {
            if (named && !str_eq(json_get_str(t, "name"), named)) continue;
            json_t *w = json_obj(), *fn = json_obj();
            json_set_str(w, "type", "function");
            json_set_str(fn, "name", json_get_str(t, "name") ? json_get_str(t, "name") : "");
            if (json_get_str(t, "description")) json_set_str(fn, "description", json_get_str(t, "description"));
            json_t *params = json_get(t, "parameters");
            if (json_is_obj(params)) {
                json_set(fn, "parameters", json_clone(params));
            } else {
                json_t *po = json_obj();
                json_set_str(po, "type", "object");
                json_set(po, "properties", json_obj());
                json_set(fn, "parameters", po);
            }
            json_set(w, "function", fn);
            json_push(tools, w);
        }
        if (json_len(tools) || named) json_set(p, "tools", tools);
        else json_free(tools);
    }
    bool as_bool, bval;
    const char *think = map_think(m, o, &as_bool, &bval);
    if (as_bool) json_set_bool(p, "think", bval);
    else if (think) json_set_str(p, "think", think);
    const char *mode = oai_tool_choice_mode(o->tool_choice);
    if (named) json_set_str(p, "tool_choice", "required");
    else if (mode && !str_eq(mode, "auto")) json_set_str(p, "tool_choice", mode);
    json_t *opts = json_obj();
    int64_t cap = o->max_tokens > 0 ? o->max_tokens : m->max_tokens;
    if (cap > 0 && !json_get_bool(m->raw, "omitMaxOutputTokens", false)) {
        if (str_eq(m->provider, "ollama-cloud") && cap > 65536) cap = 65536;
        json_set_int(opts, "num_predict", cap);
    }
    if (!isnan(o->temperature)) json_set_num(opts, "temperature", o->temperature);
    if (!isnan(o->top_p)) json_set_num(opts, "top_p", o->top_p);
    if (json_len(opts)) json_set(p, "options", opts);
    else json_free(opts);
    json_set_bool(p, "stream", true);
    return p;
}

/* ---------- NDJSON stream ---------- */

struct ollama_stream {
    ai_builder_t b;
    const ai_model_t *m;
    sb_t line;
    char *stop;
    char *error;
    bool done;
    think_heal_t heal;             /* StreamMarkupHealing over message.content */
    bool suppress_healed_thinking; /* native message.thinking seen: drop thinking the healer recovers */
};

ollama_stream_t *ollama_stream_new(const ai_model_t *m, ai_event_cb cb, void *ud) {
    ollama_stream_t *s = xcalloc(1, sizeof(*s));
    s->m = m;
    aib_init(&s->b, m, cb, ud);
    think_heal_init(&s->heal);
    return s;
}

static void heal_emit(void *ud, int kind, const char *t, size_t n) {
    ollama_stream_t *s = ud;
    if (kind == THINK_HEAL_TEXT) aib_text_delta(&s->b, t, n);
    else if (kind == THINK_HEAL_THINKING && !s->suppress_healed_thinking) aib_thinking_delta(&s->b, t, n);
}

static int handle_chunk(ollama_stream_t *s, const char *line, size_t len) {
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) len--;
    if (!len) return 0;
    json_t *j = json_parse(line, len, NULL);
    if (!json_is_obj(j)) {
        json_free(j);
        return 0;
    }
    aib_start(&s->b);
    const char *err = json_get_str(j, "error");
    if (err) {
        free(s->error);
        s->error = xasprintf("Ollama stream error: %s", err);
        json_free(j);
        return OAI_STREAM_STOP;
    }
    json_t *msg = json_get_obj(j, "message");
    json_t *th = json_get(msg, "thinking");
    if (json_is_str(th) && th->u.str.len) {
        s->suppress_healed_thinking = true;
        aib_thinking_delta(&s->b, th->u.str.s, th->u.str.len);
    }
    json_t *c = json_get(msg, "content");
    if (json_is_str(c) && c->u.str.len) think_heal_feed(&s->heal, c->u.str.s, c->u.str.len, heal_emit, s);
    json_t *call;
    json_arr_foreach(json_get_arr(msg, "tool_calls"), i, call) {
        json_t *fn = json_get_obj(call, "function");
        const char *name = json_get_str(fn, "name");
        if (!name) name = "unknown_tool";
        char *id = xasprintf("ollama:%d:%s", aib_block_count(&s->b), name);
        aib_toolcall_start(&s->b, id, name);
        free(id);
        json_t *args = json_get(fn, "arguments");
        if (json_is_str(args)) {
            aib_toolcall_delta(&s->b, args->u.str.s, args->u.str.len);
            aib_toolcall_set_args(&s->b, oai_parse_args(args->u.str.s, args->u.str.len));
        } else {
            if (json_is_obj(args)) {
                char *d = json_dump(args, 0);
                aib_toolcall_delta(&s->b, d, strlen(d));
                free(d);
            }
            aib_toolcall_set_args(&s->b, json_is_obj(args) ? json_clone(args) : json_obj());
        }
    }
    if (json_get_bool(j, "done", false)) {
        think_heal_flush(&s->heal, heal_emit, s);
        aib_end_block(&s->b);
        s->done = true;
        const char *dr = json_get_str(j, "done_reason");
        const char *stop = "stop";
        if (str_eq(dr, "length")) stop = "length";
        else if (str_eq(dr, "tool_calls")) stop = "toolUse";
        else if (str_eq(dr, "load")) stop = "error";
        free(s->stop);
        s->stop = xstrdup(stop);
        int64_t cached = json_get_int(j, "prompt_eval_cached_count", 0);
        int64_t in = json_get_int(j, "prompt_eval_count", 0) - cached;
        aib_usage(&s->b, in < 0 ? 0 : in, json_get_int(j, "eval_count", 0), cached, 0);
    }
    json_free(j);
    return 0;
}

int ollama_stream_on_data(void *ud, const char *data, size_t len) {
    ollama_stream_t *s = ud;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            int rc = handle_chunk(s, sb_cstr(&s->line), s->line.len);
            sb_clear(&s->line);
            if (rc) return rc;
        } else {
            if (s->line.len >= OLLAMA_MAX_LINE) {
                free(s->error);
                s->error = xstrdup("Ollama stream line exceeds size limit");
                return OAI_STREAM_STOP;
            }
            sb_putc(&s->line, data[i]);
        }
    }
    return 0;
}

static bool visible(const json_t *content) {
    json_t *b;
    json_arr_foreach(content, i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "toolCall")) return true;
        const char *v = json_get_str(b, str_eq(t, "text") ? "text" : "thinking");
        for (; v && *v; v++)
            if (*v != ' ' && *v != '\n' && *v != '\t' && *v != '\r') return true;
    }
    return false;
}

json_t *ollama_stream_finish(ollama_stream_t *s, int rc, const char *terr) {
    if (rc == 0 && s->line.len) handle_chunk(s, sb_cstr(&s->line), s->line.len);
    if ((rc == 0 || rc == OAI_STREAM_STOP) && !s->error) think_heal_flush(&s->heal, heal_emit, s);
    const char *stop = s->stop ? s->stop : "stop";
    const char *err = NULL;
    if (rc == -2) {
        stop = "aborted";
        err = terr ? terr : "Request was aborted";
    } else if (rc != 0 && rc != OAI_STREAM_STOP) {
        stop = "error";
        err = terr ? terr : "request failed";
    } else if (s->error) {
        stop = "error";
        err = s->error;
    } else if (str_eq(stop, "length") && !visible(json_get(s->b.msg, "content"))) {
        stop = "error";
        err = "Model returned no content: prompt filled the context window; raise Ollama num_ctx or shorten the prompt.";
    } else if (str_eq(stop, "error")) {
        err = "Ollama loaded the model but generated nothing (done_reason: load): the request contained no user-role message.";
    }
    json_t *msg = aib_finish(&s->b, stop, err);
    sb_free(&s->line);
    think_heal_free(&s->heal);
    free(s->stop);
    free(s->error);
    aib_free(&s->b);
    free(s);
    return msg;
}

typedef struct {
    const ai_model_t *m;
    const ai_context_t *ctx;
    const ai_stream_opts_t *o;
} ollama_attempt_t;

static json_t *ollama_stream_once(void *self, ai_event_cb cb, void *ud) {
    ollama_attempt_t *at = self;
    const ai_model_t *m = at->m;
    const ai_context_t *ctx = at->ctx;
    const ai_stream_opts_t *o = at->o;
    json_t *body = ollama_build_request(m, ctx, o);
    if (o->on_payload) o->on_payload(o->payload_ud, body);
    char *base = ollama_base(m);
    char *url = xasprintf("%s/api/chat", base);
    http_headers_t h = {0};
    ai_apply_headers(&h, m, o);
    char *key = oai_bearer_key(m, o);
    if (key) {
        char *a = xasprintf("Bearer %s", key);
        http_headers_set(&h, "Authorization", a);
        free(a);
    }
    char *payload = json_dump(body, 0);
    ollama_stream_t *st = ollama_stream_new(m, cb, ud);
    int status = 0;
    char *err = NULL;
    int rc = ai_stream_post(url, &h, payload, o, ollama_stream_on_data, st, &status, &err);
    json_t *msg = ollama_stream_finish(st, rc, err);
    free(err);
    free(payload);
    free(key);
    http_headers_free(&h);
    free(url);
    free(base);
    json_free(body);
    return msg;
}

/* streamOllama: fetchWithRetry at 2 s / 5 s / 10 s (llama.cpp tool-call parse 500s are not retried), and up to two
 * retries of an EOS-only completion */
json_t *provider_ollama_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                               ai_event_cb cb, void *ud) {
    ai_stream_opts_t lo;
    if (o) lo = *o;
    else ai_stream_opts_init(&lo);
    lo.retry_policy = AI_RETRY_OLLAMA;
    lo.max_retries = 4;
    ollama_attempt_t at = {m, ctx, &lo};
    return ai_replay_safe_stream(ollama_stream_once, &at, &lo, cb, ud, true, 0);
}
