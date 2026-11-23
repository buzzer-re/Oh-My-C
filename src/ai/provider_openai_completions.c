/*
 * provider_openai_completions.c - OpenAI Chat Completions streaming (api "openai-completions" and
 * "openrouter"), plus helpers shared with the Responses provider.
 * Port of packages/ai/src/providers/openai-completions.ts + openai-shared.ts (policy subset).
 */
#include "ai/compat_detect.h"
#include "ai/provider_openai.h"
#include "ai/think_heal.h"

#include <math.h>

#define OPENAI_MAX_OUTPUT_TOKENS 64000
#define OPENAI_TOOL_CALL_ID_MAX 40

/* ====================================================================== */
/* shared helpers                                                          */
/* ====================================================================== */

bool oai_cbool(const ai_model_t *m, const json_t *over, const char *key, bool def) {
    json_t *v = over ? json_get(over, key) : NULL;
    if (json_is_bool(v)) return v->u.b;
    return model_compat_bool(m, key, def);
}

const char *oai_cstr(const ai_model_t *m, const json_t *over, const char *key, const char *def) {
    const char *s = over ? json_get_str(over, key) : NULL;
    if (!s) s = model_compat_str(m, key);
    return s ? s : def;
}

json_t *oai_cget(const ai_model_t *m, const json_t *over, const char *key) {
    json_t *v = over ? json_get(over, key) : NULL;
    if (!v && m) v = model_compat_get(m, key);
    return v;
}

void oai_emit(ai_builder_t *b, ai_event_type_t t, int idx, const char *delta, size_t n, const json_t *blk) {
    if (!b->cb) return;
    ai_event_t ev = {.type = t, .content_index = idx, .delta = delta, .delta_len = n, .block = blk, .partial = b->msg};
    b->cb(b->ud, &ev);
}

bool oai_is_reasoning_field(const char *s) {
    return str_eq(s, "reasoning_content") || str_eq(s, "reasoning") || str_eq(s, "reasoning_text");
}

json_t *oai_parse_args(const char *s, size_t n) {
    json_t *j = NULL;
    if (s && n) {
        j = json_parse(s, n, NULL);
        if (!j) j = json_parse_partial(s, n);
    }
    if (!json_is_obj(j)) {
        json_free(j);
        j = json_obj();
    }
    return j;
}

char *oai_hash36(const char *s) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    uint64_t h = hash_xx64(s ? s : "", s ? strlen(s) : 0, 0);
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    buf[i] = 0;
    do {
        buf[--i] = digits[h % 36];
        h /= 36;
    } while (h && i > 0);
    return xstrdup(buf + i);
}

static const char *derive_disable_mode(const char *fmt) {
    if (str_eq(fmt, "openrouter")) return "openrouter-enabled-false";
    if (str_eq(fmt, "zai") || str_eq(fmt, "kimi")) return "zai-thinking-disabled";
    if (str_eq(fmt, "qwen")) return "qwen-enable-thinking-false";
    if (str_eq(fmt, "qwen-chat-template")) return "qwen-template-false";
    if (str_eq(fmt, "chat-template")) return "chat-template-thinking-false";
    return "lowest-effort";
}

const char *oai_effort_wire(const ai_model_t *m, const json_t *over, const char *effort) {
    if (!effort) return NULL;
    json_t *map = oai_cget(m, over, "reasoningEffortMap");
    const char *s = json_is_obj(map) ? json_get_str(map, effort) : NULL;
    if (!s && m && json_is_obj(m->thinking)) s = json_get_str(json_get_obj(m->thinking, "effortMap"), effort);
    return s ? s : effort;
}

const char *oai_tool_choice_mode(const json_t *tc) {
    const char *s = json_sv(tc);
    if (!s) return NULL;
    if (str_eq(s, "any")) return "required";
    if (str_eq(s, "auto") || str_eq(s, "none") || str_eq(s, "required")) return s;
    return NULL;
}

const char *oai_tool_choice_name(const json_t *tc) {
    if (!json_is_obj(tc)) return NULL;
    const char *t = json_get_str(tc, "type");
    if (!str_eq(t, "tool") && !str_eq(t, "function")) return NULL;
    const char *n = json_get_str(tc, "name");
    if (!n) n = json_get_str(json_get_obj(tc, "function"), "name");
    return n && *n ? n : NULL;
}

bool oai_tool_choice_forced(const json_t *tc) {
    if (!tc || json_is_null(tc)) return false;
    const char *s = json_sv(tc);
    if (s) return !str_eq(s, "auto") && !str_eq(s, "none");
    return true;
}

static bool implicit_disable(const char *mode) {
    return str_eq(mode, "zai-thinking-disabled") || str_eq(mode, "qwen-enable-thinking-false") ||
           str_eq(mode, "qwen-template-false") || str_eq(mode, "chat-template-thinking-false");
}

void oai_policy_resolve(oai_policy_t *p, const ai_model_t *m, const ai_stream_opts_t *o, const json_t *wire_tc,
                        bool has_tools) {
    memset(p, 0, sizeof(*p));
    p->model_supported = m && m->reasoning;
    if (o->reasoning != EFFORT_OFF && p->model_supported) {
        ai_effort_t e = model_clamp_effort(m, o->reasoning);
        if (e != EFFORT_OFF) p->requested = effort_name(e);
    }
    bool forced_sup = oai_cbool(m, NULL, "disableReasoningOnForcedToolChoice", false) &&
                      oai_cbool(m, NULL, "supportsForcedToolChoice", true) && oai_tool_choice_forced(wire_tc);
    bool any_sup = !forced_sup && oai_cbool(m, NULL, "disableReasoningOnToolChoice", false) && wire_tc != NULL;
    bool tools_sup = !forced_sup && !any_sup && oai_cbool(m, NULL, "disableReasoningWithTools", false) && has_tools;
    const char *conflict = forced_sup ? "forced-tool-choice" : any_sup ? "tool-choice" : tools_sup ? "tools" : NULL;
    const char *reason = o->disable_reasoning ? "caller" : conflict;
    bool allowed = p->requested && !o->disable_reasoning && p->model_supported;
    bool enabled_before = allowed && !reason;
    const char *base_mode = oai_cstr(m, NULL, "reasoningDisableMode",
                                     derive_disable_mode(oai_cstr(m, NULL, "thinkingFormat", "openai")));
    const char *base_wire = enabled_before ? oai_effort_wire(m, NULL, p->requested) : NULL;
    bool disabled_by_none = enabled_before && str_eq(base_mode, "zai-thinking-disabled") && str_eq(base_wire, "none");
    p->enabled = enabled_before && !disabled_by_none;
    json_t *wt = p->enabled ? oai_cget(m, NULL, "whenThinking") : NULL;
    p->over = json_is_obj(wt) ? wt : NULL;
    p->omit_effort = oai_cbool(m, p->over, "omitReasoningEffort", false) ||
                     !oai_cbool(m, p->over, "supportsReasoningEffort", true);
    p->dialect = oai_cstr(m, p->over, "thinkingFormat", "openai");
    p->disable_mode = oai_cstr(m, p->over, "reasoningDisableMode", derive_disable_mode(p->dialect));
    p->supports_params = oai_cbool(m, p->over, "supportsReasoningParams", true);
    const char *wire = p->enabled ? oai_effort_wire(m, p->over, p->requested) : NULL;
    bool without_req = p->model_supported && !p->requested && !o->disable_reasoning && implicit_disable(p->disable_mode);
    p->disabled = (p->model_supported && str_eq(reason, "caller")) || conflict != NULL || without_req || disabled_by_none;
    if (p->disabled && oai_cbool(m, p->over, "supportsReasoningEffort", true) && !p->omit_effort) {
        if (str_eq(p->disable_mode, "none-effort")) {
            wire = "none";
        } else if (str_eq(reason, "caller") && !p->requested && str_eq(p->disable_mode, "lowest-effort")) {
            json_t *efforts = m ? json_get_arr(m->thinking, "efforts") : NULL;
            const char *lowest = json_sv(json_at(efforts, 0));
            if (lowest) wire = oai_effort_wire(m, p->over, lowest);
        }
    }
    if (wire) snprintf(p->wire, sizeof(p->wire), "%s", wire);
    p->disable_reason = reason ? reason : (without_req || disabled_by_none) ? "not-requested" : NULL;
}

/* ---------- strict JSON schema adaptation ---------- */

static bool str_in(const char *s, const char *const *list) {
    for (; *list; list++)
        if (strcmp(s, *list) == 0) return true;
    return false;
}

static const char *const k_strict_keep[] = {"type",     "properties", "required",         "additionalProperties",
                                            "items",    "enum",       "const",            "anyOf",
                                            "description", "title",   "$ref",             "$defs",
                                            "definitions", "pattern", "format",           "minimum",
                                            "maximum",  "exclusiveMinimum", "exclusiveMaximum", "multipleOf",
                                            "minItems", "maxItems",   NULL};
static const char *const k_strict_drop[] = {"default", "examples", "$schema", "$id", "$comment",
                                            "deprecated", "readOnly", "writeOnly", NULL};

static json_t *strict_node(const json_t *s, int depth, bool *ok);

static json_t *strict_map(const json_t *obj, int depth, bool *ok) {
    json_t *r = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(obj, mi, k, v) {
        json_t *c = strict_node(v, depth + 1, ok);
        if (!*ok) {
            json_free(c);
            break;
        }
        json_set(r, k, c);
    }
    return r;
}

static bool is_nullable_anyof(const json_t *n) {
    json_t *any = json_get_arr(n, "anyOf");
    json_t *e;
    json_arr_foreach(any, ai, e) {
        if (str_eq(json_get_str(e, "type"), "null")) return true;
    }
    return false;
}

static json_t *null_schema(void) {
    json_t *n = json_obj();
    json_set_str(n, "type", "null");
    return n;
}

static json_t *strict_node(const json_t *s, int depth, bool *ok) {
    if (!*ok) return NULL;
    if (depth > 32 || !json_is_obj(s)) {
        *ok = false;
        return NULL;
    }
    if (json_get(s, "$ref")) {
        const char *k;
        json_t *v;
        json_obj_foreach(s, ri, k, v) {
            (void)v;
            if (strcmp(k, "$ref") != 0 && !str_in(k, k_strict_drop)) {
                *ok = false;
                return NULL;
            }
        }
    }
    json_t *r = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(s, si, k, v) {
        if (str_in(k, k_strict_drop)) continue;
        if (!str_in(k, k_strict_keep)) {
            *ok = false;
            break;
        }
        if (strcmp(k, "properties") == 0 || strcmp(k, "$defs") == 0 || strcmp(k, "definitions") == 0) {
            if (!json_is_obj(v)) {
                *ok = false;
                break;
            }
            json_set(r, k, strict_map(v, depth, ok));
        } else if (strcmp(k, "items") == 0) {
            json_set(r, k, strict_node(v, depth + 1, ok));
        } else if (strcmp(k, "anyOf") == 0) {
            if (!json_is_arr(v)) {
                *ok = false;
                break;
            }
            json_t *arr = json_arr();
            json_t *e;
            json_arr_foreach(v, ei, e) {
                json_t *c = strict_node(e, depth + 1, ok);
                if (!*ok) {
                    json_free(c);
                    break;
                }
                json_push(arr, c);
            }
            json_set(r, k, arr);
        } else if (strcmp(k, "additionalProperties") == 0) {
            if (!json_is_bool(v) || v->u.b) *ok = false;
        } else if (strcmp(k, "type") == 0 && json_is_arr(v)) {
            json_t *e;
            json_arr_foreach(v, ti, e) {
                if (str_eq(json_sv(e), "object")) *ok = false;
            }
            json_set(r, k, json_clone(v));
        } else {
            json_set(r, k, json_clone(v));
        }
        if (!*ok) break;
    }
    if (*ok && str_eq(json_get_str(s, "type"), "object")) {
        json_t *props = json_get(r, "properties");
        if (!json_is_obj(props)) {
            *ok = false; /* open map: not representable */
        } else {
            json_t *orig_req = json_get_arr(s, "required");
            json_t *req = json_arr();
            for (size_t i = 0; i < props->u.obj.n; i++) {
                const char *pk = props->u.obj.keys[i];
                bool required = false;
                json_t *e;
                json_arr_foreach(orig_req, qi, e) {
                    if (str_eq(json_sv(e), pk)) required = true;
                }
                json_push_str(req, pk);
                if (required) continue;
                json_t *p = props->u.obj.vals[i];
                json_t *wrapped;
                if (is_nullable_anyof(p)) continue;
                if (json_get_arr(p, "anyOf") && json_len(p) == 1) {
                    json_push(json_get(p, "anyOf"), null_schema());
                    continue;
                }
                wrapped = json_obj();
                json_t *any = json_arr();
                json_t *desc = json_take(p, "description");
                json_push(any, json_clone(p));
                json_push(any, null_schema());
                json_set(wrapped, "anyOf", any);
                if (desc) json_set(wrapped, "description", desc);
                props->u.obj.vals[i] = wrapped;
                json_free(p);
            }
            json_set(r, "required", req);
            json_set_bool(r, "additionalProperties", false);
        }
    }
    if (!*ok) {
        json_free(r);
        return NULL;
    }
    return r;
}

json_t *oai_strict_schema(const json_t *schema) {
    bool ok = true;
    json_t *r = strict_node(schema, 0, &ok);
    if (!ok) {
        json_free(r);
        return NULL;
    }
    return r;
}

char *oai_prompt_cache_key(const ai_stream_opts_t *o, size_t max_len, const char *prefix) {
    const char *ret = o->cache_retention ? o->cache_retention : env_get("PI_CACHE_RETENTION");
    if (str_eq(ret, "none")) return NULL;
    /* the OpenRouter session id follows the request lineage; the cache key may be pinned to the main session */
    const char *id = o->prompt_cache_key && strcmp(prefix, "session_") ? o->prompt_cache_key : o->session_id;
    if (str_empty(id)) return NULL;
    if (strlen(id) <= max_len) return xstrdup(id);
    char *h = oai_hash36(id);
    char *r = xasprintf("%s%s", prefix, h);
    free(h);
    return r;
}

/* omp openai-shared: the host's worker-affinity header (`x-grok-conv-id`, Morph's `x-session-id`) mirrors the normalized
 * prompt-cache key (`promptCacheKey`, else the session id; none when cache retention is off) */
void oai_apply_prompt_cache_session_header(http_headers_t *h, const ai_model_t *m, const ai_stream_opts_t *o) {
    const char *hdr = oai_cstr(m, NULL, "promptCacheSessionHeader", NULL);
    if (!hdr || !*hdr) return;
    char *key = oai_prompt_cache_key(o, 64, "pc_");
    if (key && !http_headers_get(h, hdr)) http_headers_set(h, hdr, key);
    free(key);
}

bool oai_send_service_tier(const ai_model_t *m, const char *tier) { return ai_should_send_service_tier(tier, m); }

char *oai_image_url(const json_t *img) {
    const char *url = json_get_str(img, "url");
    if (url) return xstrdup(url);
    const char *mt = json_get_str(img, "mimeType");
    const char *data = json_get_str(img, "data");
    return xasprintf("data:%s;base64,%s", mt ? mt : "image/png", data ? data : "");
}

static bool blank(const char *s) {
    if (!s) return true;
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') return false;
    return true;
}

json_t *oai_system_prompts(const ai_context_t *ctx) {
    json_t *out = json_arr();
    json_t *e;
    if (json_is_str(ctx->system_prompt)) {
        if (!blank(json_sv(ctx->system_prompt))) json_push_str(out, json_sv(ctx->system_prompt));
        return out;
    }
    json_arr_foreach(ctx->system_prompt, i, e) {
        if (json_is_str(e) && !blank(json_sv(e))) json_push_str(out, json_sv(e));
    }
    return out;
}

int64_t oai_output_tokens(const ai_model_t *m, const ai_stream_opts_t *o, bool clamp_to_model) {
    if (json_get_bool(m->raw, "omitMaxOutputTokens", false)) return 0;
    bool always = oai_cbool(m, NULL, "alwaysSendMaxTokens", false);
    bool explicit_cap = o->max_tokens > 0;
    int64_t req = explicit_cap ? o->max_tokens : m->max_tokens;
    if (req <= 0) req = always ? (m->max_tokens > 0 ? m->max_tokens : OPENAI_MAX_OUTPUT_TOKENS) : 0;
    if (req <= 0) return 0;
    if (oai_cbool(m, NULL, "isOpenRouterHost", false) && !always && !explicit_cap) return 0;
    int64_t clamp = clamp_to_model && m->max_tokens > 0 ? m->max_tokens : OPENAI_MAX_OUTPUT_TOKENS;
    if (m->max_tokens > 0 && req > m->max_tokens) req = m->max_tokens;
    if (req > clamp) req = clamp;
    return req > 0 ? req : 0;
}

char *oai_bearer_key(const ai_model_t *m, const ai_stream_opts_t *o) {
    if (!str_empty(o->api_key)) return xstrdup(o->api_key);
    char *k = ai_env_api_key(m->provider);
    if (k && *k) return k;
    free(k);
    if (str_eq(m->provider, "openai") && env_get("OPENAI_API_KEY")) return xstrdup(env_get("OPENAI_API_KEY"));
    return NULL;
}

char *oai_base_url(const ai_model_t *m, const char *def) {
    const char *b = !str_empty(m->base_url) ? m->base_url : def;
    char *r = xstrdup(b ? b : "");
    size_t n = strlen(r);
    while (n > 0 && r[n - 1] == '/') r[--n] = 0;
    return r;
}

void oai_apply_inference_headers(http_headers_t *h, const ai_model_t *m, const ai_stream_opts_t *o) {
    ai_apply_inference_headers(h, m, !str_empty(o->session_id) ? o->session_id : o->prompt_cache_key, false);
    /* applySessionHeader: the model's compat.sessionHeader, a header already set wins */
    const char *name = model_compat_str(m, "sessionHeader");
    if (!str_empty(name) && !str_empty(o->session_id) && !http_headers_get(h, name)) http_headers_set(h, name, o->session_id);
}

void oai_add_openrouter_headers(http_headers_t *h) {
    http_headers_set(h, "User-Agent", OMC_NAME "/" OMC_VERSION);
    http_headers_set(h, "HTTP-Referer", "https://github.com/can1357/oh-my-pi");
    http_headers_set(h, "X-OpenRouter-Title", OMC_NAME);
    http_headers_set(h, "X-OpenRouter-Categories", "cli-agent");
    http_headers_set(h, "X-OpenRouter-Cache", "true");
    http_headers_set(h, "X-OpenRouter-Cache-TTL", "3600");
}

void oai_usage_accounting(ai_builder_t *b, int64_t prompt, int64_t output, int64_t cached, int64_t reasoning,
                          int64_t cw_or, int64_t cw_ds, bool has_ds) {
    int64_t cw = cw_or >= 0 ? cw_or : cw_ds >= 0 ? cw_ds : 0;
    bool ds = has_ds && cw_or < 0 && cw_ds > 0;
    int64_t input = ds ? prompt - cached : prompt - cached - cw;
    if (input < 0) input = 0;
    aib_usage(b, input, output < 0 ? 0 : output, cached < 0 ? 0 : cached, ds ? 0 : cw);
    if (reasoning > 0) aib_usage_add_reasoning(b, reasoning);
}

/* ====================================================================== */
/* request building                                                        */
/* ====================================================================== */

static int64_t num_or(const json_t *o, const char *k, int64_t def) {
    json_t *v = json_get(o, k);
    return json_is_num(v) ? (v->is_int ? v->u.num.i : json_d2i(v->u.num.d)) : def;
}

static char *mistral_id(const char *id) {
    char out[10];
    size_t n = 0;
    for (const char *p = id ? id : ""; *p && n < 9; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[n++] = c;
    }
    static const char pad[] = "ABCDEFGHI";
    for (size_t i = n; i < 9; i++) out[i] = pad[i - n];
    out[9] = 0;
    return xstrdup(out);
}

static char *sanitize_call_id(const char *id, size_t max) {
    sb_t b = SB_INIT;
    for (const char *p = id; *p && b.len < max; p++) {
        char c = *p;
        bool okc = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        sb_putc(&b, okc ? c : '_');
    }
    return sb_detach(&b);
}

typedef struct {
    const ai_model_t *m;
    bool mistral, limit40;
    hmap_t remap; /* original id -> normalized (owned char*) */
    int gen;
} idnorm_t;

static char *norm_tool_id(idnorm_t *n, const char *id, bool same_model) {
    if (!id) id = "";
    if (n->mistral) return mistral_id(id);
    const char *bar = strchr(id, '|');
    if (!same_model && bar) {
        char *call = xstrndup(id, (size_t)(bar - id));
        char *r = sanitize_call_id(call, OPENAI_TOOL_CALL_ID_MAX);
        free(call);
        return r;
    }
    if (n->limit40 && strlen(id) > OPENAI_TOOL_CALL_ID_MAX) return xstrndup(id, OPENAI_TOOL_CALL_ID_MAX);
    return xstrdup(id);
}

static char *ensure_tool_id(idnorm_t *n, const char *raw, bool same_model, const char *seed) {
    char *r = norm_tool_id(n, raw, same_model);
    if (!blank(r)) return r;
    free(r);
    char *s = xasprintf("%s:%s:%s:%d", n->m->provider, n->m->id, seed, ++n->gen);
    char *h = oai_hash36(s);
    free(s);
    r = xasprintf("call_%s", h);
    free(h);
    if (n->mistral) {
        char *mm = mistral_id(r);
        free(r);
        r = mm;
    }
    return r;
}

static json_t *text_part(const char *t) {
    json_t *p = json_obj();
    json_set_str(p, "type", "text");
    json_set_str(p, "text", t);
    return p;
}

static json_t *image_url_part(const json_t *img) {
    json_t *p = json_obj();
    json_set_str(p, "type", "image_url");
    json_t *iu = json_obj();
    json_set(iu, "url", json_str_own(oai_image_url(img)));
    const char *detail = json_get_str(img, "detail");
    if (detail && !str_eq(detail, "original")) json_set_str(iu, "detail", detail);
    json_set(p, "image_url", iu);
    return p;
}

static json_t *msg_obj(const char *role, json_t *content) {
    json_t *o = json_obj();
    json_set_str(o, "role", role);
    json_set(o, "content", content ? content : json_null());
    return o;
}

static bool has_key(const json_t *o, const char *k) { return json_get(o, k) != NULL; }

/* signatureReasoningField: the reasoning field a thinking block's text arrived in, from its bare marker or its
 * reasoning_details envelope ({"reasoningDetails": [...], "field"?}); other signatures pass through. buf holds a parsed
 * field. */
static const char *sig_field(const char *sig, char buf[32]) {
    if (!sig || sig[0] != '{') return sig;
    json_t *env = json_parse_str(sig);
    const char *f = json_is_arr(json_get(env, "reasoningDetails")) ? json_get_str(env, "field") : NULL;
    const char *r = sig;
    if (json_is_arr(json_get(env, "reasoningDetails"))) {
        r = NULL;
        if (oai_is_reasoning_field(f)) {
            snprintf(buf, 32, "%s", f);
            r = buf;
        }
    }
    json_free(env);
    return r;
}

static bool readable_detail(const json_t *d) {
    const char *t = json_get_str(d, "type");
    return str_eq(t, "reasoning.text") || str_eq(t, "reasoning.summary");
}

static bool signed_detail(const json_t *d) { return !json_is_null(json_get(d, "signature")) || !json_is_null(json_get(d, "data")); }

static void convert_assistant(json_t *out, const json_t *msg, const ai_model_t *m, const json_t *over, idnorm_t *idn,
                              size_t msg_index) {
    bool think_as_text = oai_cbool(m, over, "requiresThinkingAsText", false);
    bool rc_tools = oai_cbool(m, over, "requiresReasoningContentForToolCalls", false);
    bool rc_all = oai_cbool(m, over, "requiresReasoningContentForAllAssistantTurns", false);
    bool rc_synth = oai_cbool(m, over, "allowsSyntheticReasoningContentForToolCalls", true);
    const char *rc_field = oai_cstr(m, over, "reasoningContentField", "reasoning_content");
    bool replay_rc = oai_cbool(m, over, "replayReasoningContent", false);
    const char *fmt = oai_cstr(m, over, "thinkingFormat", "openai");
    bool content_for_tools = oai_cbool(m, over, "requiresAssistantContentForToolCalls", false);
    const char *synth_fallback = oai_cstr(m, over, "syntheticReasoningContentFallback", "");
    bool same_model = str_eq(json_get_str(msg, "provider"), m->provider) && str_eq(json_get_str(msg, "api"), m->api) &&
                      str_eq(json_get_str(msg, "model"), m->id);

    json_t *am = json_obj();
    json_set_str(am, "role", "assistant");
    json_set(am, "content", json_null());

    json_t *content = json_get(msg, "content");
    sb_t text = SB_INIT, thinking = SB_INIT, all_thinking = SB_INIT;
    bool have_text = false;
    int n_think = 0, n_think_all = 0, n_tools = 0;
    const char *first_sig = NULL, *first_sig_all = NULL;
    json_t *blk;
    json_arr_foreach(content, i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "text")) {
            const char *s = json_get_str(blk, "text");
            if (!blank(s)) {
                sb_append(&text, s);
                have_text = true;
            }
        } else if (str_eq(t, "thinking")) {
            const char *s = json_get_str(blk, "thinking");
            if (n_think_all++) sb_putc(&all_thinking, '\n');
            sb_append(&all_thinking, s ? s : "");
            if (!first_sig_all) first_sig_all = json_get_str(blk, "thinkingSignature");
            if (!blank(s)) {
                if (n_think) sb_putc(&thinking, think_as_text ? ' ' : '\n');
                if (think_as_text) sb_appendf(&thinking, "<think>\n%s\n</think>", s);
                else sb_append(&thinking, s);
                if (!n_think) first_sig = json_get_str(blk, "thinkingSignature");
                n_think++;
            }
        } else if (str_eq(t, "toolCall")) {
            n_tools++;
        }
    }
    if (have_text) json_set(am, "content", json_strn(text.s, text.len));
    char fbuf[32], fbuf_all[32];
    first_sig = sig_field(first_sig, fbuf);
    first_sig_all = sig_field(first_sig_all, fbuf_all);

    if (n_think > 0) {
        if (think_as_text) {
            if (have_text) {
                char *c = xasprintf("%s %s", sb_cstr(&thinking), sb_cstr(&text));
                json_set(am, "content", json_str_own(c));
            } else {
                json_set(am, "content", json_strn(thinking.s, thinking.len));
            }
        } else if (rc_tools) {
            const char *field = oai_is_reasoning_field(first_sig) ? (rc_synth ? first_sig : rc_field) : NULL;
            if (field) json_set(am, field, json_strn(thinking.s, thinking.len));
        } else if (str_eq(fmt, "zai") && m->reasoning) {
            json_set(am, rc_field, json_strn(thinking.s, thinking.len));
        } else if (replay_rc) {
            json_set(am, oai_is_reasoning_field(first_sig) ? first_sig : rc_field, json_strn(thinking.s, thinking.len));
        }
    }
    if (rc_tools) {
        const char *field = rc_synth && oai_is_reasoning_field(first_sig) ? first_sig : rc_field;
        if (!json_truthy(json_get(am, field))) {
            json_t *r = json_get(am, "reasoning"), *rt = json_get(am, "reasoning_text");
            if (json_truthy(r) && !str_eq(field, "reasoning")) json_set(am, field, json_clone(r));
            else if (json_truthy(rt) && !str_eq(field, "reasoning_text")) json_set(am, field, json_clone(rt));
            else if (n_think > 0 && !think_as_text) json_set(am, field, json_strn(thinking.s, thinking.len));
        }
    }
    bool can_synth = rc_tools && rc_synth && (str_eq(fmt, "openai") || str_eq(fmt, "openrouter") || str_eq(fmt, "zai"));
    bool needs = rc_all || n_tools > 0;
    bool has_rf = has_key(am, "reasoning_content") || has_key(am, "reasoning") || has_key(am, "reasoning_text") ||
                  has_key(am, rc_field);
    if (needs && !has_rf && rc_tools && !rc_synth && n_think_all > 0 && oai_is_reasoning_field(first_sig_all)) {
        json_set(am, rc_field, json_strn(all_thinking.s, all_thinking.len));
        has_rf = true;
    }
    if (needs && !has_rf && rc_tools && !rc_synth) {
        json_set_str(am, rc_field, synth_fallback);
        has_rf = true;
    }
    if (n_tools > 0 && can_synth && !has_rf) {
        json_set_str(am, rc_field, ".");
        has_rf = true;
    }
    if (n_tools > 0) {
        json_t *tcs = json_arr();
        json_t *details = json_arr();
        int k = 0;
        json_arr_foreach(content, j, blk) {
            if (!str_eq(json_get_str(blk, "type"), "toolCall")) continue;
            const char *raw = json_get_str(blk, "id");
            const char *name = json_get_str(blk, "name");
            char seed[96];
            snprintf(seed, sizeof(seed), "%zu:%d:%.60s", msg_index, k++, name ? name : "");
            char *id = ensure_tool_id(idn, raw, same_model, seed);
            if (raw && !hmap_has(&idn->remap, raw)) hmap_put(&idn->remap, raw, xstrdup(id));
            json_t *tc = json_obj();
            json_set(tc, "id", json_str_own(id));
            json_set_str(tc, "type", "function");
            json_t *fn = json_obj();
            json_set_str(fn, "name", name ? name : "");
            json_t *args = json_get(blk, "arguments");
            json_set(fn, "arguments", args ? json_str_own(json_dump(args, 0)) : json_str("{}"));
            json_set(tc, "function", fn);
            /* stored per-call provider signature (Gemini extra_content / OpenRouter reasoning_details) */
            json_t *sig = json_parse_str(json_get_str(blk, "thoughtSignature"));
            json_t *per = json_get_obj(sig, "perCall");
            if (per) {
                if (json_get(per, "google") || json_get(per, "vertex")) json_set(tc, "extra_content", json_clone(per));
                else json_push(details, json_clone(per));
            }
            json_free(sig);
            json_push(tcs, tc);
        }
        json_set(am, "tool_calls", tcs);
        json_set(am, "__per_call_details", details);
    }
    /* replay reasoning_details exactly as received; once the envelope is gone (a pass rewrote the turn, or it predates
     * it), the encrypted entries kept on unchanged tool calls still replay. A request that drops the model's own thinking
     * also drops its unsigned text. */
    {
        json_t *received = json_arr();
        json_arr_foreach(content, j, blk) {
            if (!str_eq(json_get_str(blk, "type"), "thinking")) continue;
            const char *sig = json_get_str(blk, "thinkingSignature");
            json_t *env = sig && sig[0] == '{' ? json_parse_str(sig) : NULL;
            json_t *d;
            json_arr_foreach(json_get_arr(env, "reasoningDetails"), k, d) json_push(received, json_clone(d));
            json_free(env);
        }
        json_t *per = json_take(am, "__per_call_details");
        json_t *details = json_len(received) ? received : per;
        if (details == received) json_free(per);
        else json_free(received);
        bool sends_own = oai_cbool(m, over, "mistralReasoningContentParts", false) || think_as_text || rc_tools ||
                         (str_eq(fmt, "zai") && m->reasoning) || replay_rc;
        json_t *replayed = json_arr(), *d;
        json_arr_foreach(details, k, d) {
            if (sends_own || !readable_detail(d) || signed_detail(d)) json_push(replayed, json_clone(d));
        }
        json_free(details);
        if (json_len(replayed)) json_set(am, "reasoning_details", replayed);
        else json_free(replayed);
    }
    json_t *c = json_get(am, "content");
    if (json_is_null(c) && (has_rf || n_tools > 0)) json_set_str(am, "content", "");
    c = json_get(am, "content");
    bool has_content = json_is_str(c) && c->u.str.len > 0;
    if (!has_content && n_tools > 0 && content_for_tools) json_set_str(am, "content", ".");
    if (!has_content && n_tools == 0 && !has_rf) json_free(am);
    else json_push(out, am);
    sb_free(&text);
    sb_free(&thinking);
    sb_free(&all_thinking);
}

static void free_str(void *p) { free(p); }

json_t *oaic_convert_messages(const ai_model_t *m, const ai_context_t *ctx, const json_t *over) {
    json_t *out = json_arr();
    idnorm_t idn = {.m = m,
                    .mistral = oai_cbool(m, over, "requiresMistralToolIds", false),
                    .limit40 = oai_cbool(m, over, "usesOpenAIToolCallIdLimit", false)};
    hmap_init(&idn.remap);
    bool dev_supported = oai_cbool(m, over, "supportsDeveloperRole", false);
    bool asst_after_tool = oai_cbool(m, over, "requiresAssistantAfterToolResult", false);
    bool tool_name = oai_cbool(m, over, "requiresToolResultName", false);
    bool images_ok = m->input_image && !oai_cbool(m, over, "stripImageInput", false);

    json_t *sys = oai_system_prompts(ctx);
    if (json_len(sys) > 0) {
        const char *role = m->reasoning && dev_supported ? "developer" : "system";
        if (oai_cbool(m, over, "supportsMultipleSystemMessages", true)) {
            json_t *s;
            json_arr_foreach(sys, i, s) json_push(out, msg_obj(role, json_str(json_sv(s))));
        } else {
            sb_t b = SB_INIT;
            json_t *s;
            json_arr_foreach(sys, i, s) {
                if (i) sb_append(&b, "\n\n");
                sb_append(&b, json_sv(s));
            }
            json_push(out, msg_obj(role, json_str_own(sb_detach(&b))));
        }
    }
    json_free(sys);

    json_t *msgs = ai_transform_messages(ctx->messages, m);
    const char *last_role = NULL;
    size_t n = json_len(msgs);
    for (size_t i = 0; i < n; i++) {
        json_t *msg = json_at(msgs, i);
        const char *role = json_get_str(msg, "role");
        if (!role) continue;
        bool is_user = str_eq(role, "user") || str_eq(role, "developer");
        if (asst_after_tool && str_eq(last_role, "toolResult") && is_user)
            json_push(out, msg_obj("assistant", json_str("I have processed the tool results.")));
        if (is_user) {
            json_t *c = json_get(msg, "content");
            const char *r = dev_supported && str_eq(role, "developer") ? "developer" : "user";
            if (json_is_str(c)) {
                if (blank(json_sv(c))) continue;
                json_push(out, msg_obj(r, json_clone(c)));
            } else {
                json_t *parts = json_arr();
                bool omitted = false;
                json_t *blk;
                json_arr_foreach(c, j, blk) {
                    const char *t = json_get_str(blk, "type");
                    if (str_eq(t, "text")) {
                        if (!blank(json_get_str(blk, "text"))) json_push(parts, text_part(json_get_str(blk, "text")));
                    } else if (str_eq(t, "image")) {
                        if (images_ok) json_push(parts, image_url_part(blk));
                        else omitted = true;
                    }
                }
                if (omitted) json_push(parts, text_part("[image omitted: model does not support vision]"));
                if (json_len(parts) == 0) {
                    json_free(parts);
                    continue;
                }
                json_push(out, msg_obj("user", parts));
            }
            last_role = str_eq(role, "developer") ? (m->reasoning && dev_supported ? "developer" : "system") : role;
        } else if (str_eq(role, "assistant")) {
            convert_assistant(out, msg, m, over, &idn, i);
            last_role = "assistant";
        } else if (str_eq(role, "toolResult")) {
            json_t *imgs = json_arr();
            size_t j = i;
            for (; j < n && str_eq(json_get_str(json_at(msgs, j), "role"), "toolResult"); j++) {
                json_t *tm = json_at(msgs, j);
                sb_t txt = SB_INIT;
                bool has_img = false, first = true;
                json_t *blk;
                json_arr_foreach(json_get(tm, "content"), k, blk) {
                    const char *t = json_get_str(blk, "type");
                    if (str_eq(t, "text")) {
                        if (!first) sb_putc(&txt, '\n');
                        sb_append(&txt, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
                        first = false;
                    } else if (str_eq(t, "image")) {
                        has_img = true;
                        if (images_ok) {
                            json_t *p = json_obj();
                            json_set_str(p, "type", "image_url");
                            json_t *iu = json_obj();
                            json_set(iu, "url", json_str_own(oai_image_url(blk)));
                            json_set(p, "image_url", iu);
                            json_push(imgs, p);
                        }
                    }
                }
                if (json_is_str(json_get(tm, "content"))) sb_append(&txt, json_get_str(tm, "content"));
                const char *body;
                char *tmp = NULL;
                if (has_img && !images_ok) {
                    tmp = txt.len ? xasprintf("%s\n[image omitted: model does not support vision]", txt.s)
                                  : xstrdup("[image omitted: model does not support vision]");
                    body = tmp;
                } else {
                    body = txt.len ? txt.s : has_img ? "(see attached image)" : "";
                }
                const char *orig = json_get_str(tm, "toolCallId");
                const char *mapped = orig ? hmap_get(&idn.remap, orig) : NULL;
                char seed[96];
                snprintf(seed, sizeof(seed), "%zu:%.60s", j, json_get_str(tm, "toolName") ? json_get_str(tm, "toolName") : "tool");
                char *id = mapped ? xstrdup(mapped) : ensure_tool_id(&idn, orig, false, seed);
                if (idn.mistral) {
                    char *mm = mistral_id(id);
                    free(id);
                    id = mm;
                }
                json_t *o = json_obj();
                json_set_str(o, "role", "tool");
                json_set_str(o, "content", body);
                json_set(o, "tool_call_id", json_str_own(id));
                if (tool_name && json_get_str(tm, "toolName")) json_set_str(o, "name", json_get_str(tm, "toolName"));
                json_push(out, o);
                free(tmp);
                sb_free(&txt);
            }
            i = j - 1;
            if (json_len(imgs) > 0) {
                if (asst_after_tool)
                    json_push(out, msg_obj("assistant", json_str("I have processed the tool results.")));
                json_t *parts = json_arr();
                json_push(parts, text_part("Attached image(s) from tool result:"));
                json_t *p;
                json_arr_foreach(imgs, k, p) json_push(parts, json_clone(p));
                json_push(out, msg_obj("user", parts));
                last_role = "user";
            } else {
                last_role = "toolResult";
            }
            json_free(imgs);
        }
    }
    json_free(msgs);
    hmap_free(&idn.remap, free_str);
    return out;
}

static void add_anthropic_cache_control(json_t *messages) {
    for (size_t i = json_len(messages); i > 0; i--) {
        json_t *msg = json_at(messages, i - 1);
        const char *role = json_get_str(msg, "role");
        if (!str_eq(role, "user") && !str_eq(role, "assistant") && !str_eq(role, "developer") && !str_eq(role, "tool"))
            continue;
        json_t *c = json_get(msg, "content");
        json_t *cc = json_obj();
        json_set_str(cc, "type", "ephemeral");
        if (json_is_str(c)) {
            if (blank(json_sv(c))) {
                json_free(cc);
                continue;
            }
            json_t *part = text_part(json_sv(c));
            json_set(part, "cache_control", cc);
            json_t *arr = json_arr();
            json_push(arr, part);
            json_set(msg, "content", arr);
            return;
        }
        for (size_t j = json_len(c); j > 0; j--) {
            json_t *part = json_at(c, j - 1);
            if (str_eq(json_get_str(part, "type"), "text") && !blank(json_get_str(part, "text"))) {
                json_set(part, "cache_control", cc);
                return;
            }
        }
        json_free(cc);
    }
}

static json_t *tool_params(const json_t *tool) {
    json_t *p = json_get(tool, "parameters");
    if (json_is_obj(p)) return json_clone(p);
    json_t *o = json_obj();
    json_set_str(o, "type", "object");
    json_set(o, "properties", json_obj());
    return o;
}

static json_t *convert_tools_cc(const ai_model_t *m, const json_t *over, const json_t *tools) {
    bool no_strict = env_truthy("PI_NO_STRICT");
    bool supports = oai_cbool(m, over, "supportsStrictMode", false);
    const char *mode = oai_cstr(m, over, "toolStrictMode", "mixed");
    bool grammar = str_eq(oai_cstr(m, over, "toolSchemaFlavor", NULL), "grammar");
    size_t n = json_len(tools);
    json_t **adapted = xcalloc(n ? n : 1, sizeof(json_t *));
    bool all = n > 0;
    for (size_t i = 0; i < n; i++) {
        json_t *t = json_at(tools, i);
        json_t *st = json_get(t, "strict");
        bool strict = !no_strict && supports && !(json_is_bool(st) && !st->u.b);
        adapted[i] = strict ? oai_strict_schema(json_get(t, "parameters")) : NULL;
        if (!adapted[i]) all = false;
    }
    const char *eff = str_eq(mode, "none") ? "none" : str_eq(mode, "all_strict") ? (all ? "all_strict" : "none") : "mixed";
    json_t *out = json_arr();
    for (size_t i = 0; i < n; i++) {
        json_t *t = json_at(tools, i);
        json_t *st = json_get(t, "strict");
        bool inc = str_eq(eff, "all_strict") || (str_eq(eff, "mixed") && adapted[i]);
        bool expl_false = !inc && json_is_bool(st) && !st->u.b && str_eq(eff, "mixed") && supports;
        json_t *fn = json_obj();
        json_set_str(fn, "name", json_get_str(t, "name") ? json_get_str(t, "name") : "");
        json_set_str(fn, "description", json_get_str(t, "description") ? json_get_str(t, "description") : "");
        if (inc) {
            json_set(fn, "parameters", adapted[i]);
            adapted[i] = NULL;
            json_set_bool(fn, "strict", true);
        } else {
            json_set(fn, "parameters", tool_params(t));
            if (expl_false) json_set_bool(fn, "strict", false);
        }
        /* grammar-constrained local backends (llama.cpp, LM Studio, vLLM) build a GBNF grammar from the schema and
         * 400 on a bare boolean subschema (omp sanitizeSchemaForGrammar) */
        if (grammar) json_set(fn, "parameters", compat_schema_for_grammar(json_get(fn, "parameters")));
        json_t *w = json_obj();
        json_set_str(w, "type", "function");
        json_set(w, "function", fn);
        json_push(out, w);
        json_free(adapted[i]);
    }
    free(adapted);
    return out;
}

static bool history_has_tools(const json_t *messages) {
    json_t *msg;
    json_arr_foreach(messages, i, msg) {
        const char *r = json_get_str(msg, "role");
        if (str_eq(r, "toolResult")) return true;
        if (str_eq(r, "assistant")) {
            json_t *b;
            json_arr_foreach(json_get(msg, "content"), j, b) {
                if (str_eq(json_get_str(b, "type"), "toolCall")) return true;
            }
        }
    }
    return false;
}

static bool tools_have(const json_t *tools, const char *name) {
    json_t *t;
    json_arr_foreach(tools, i, t) {
        if (str_eq(json_get_str(json_get_obj(t, "function"), "name"), name)) return true;
    }
    return false;
}

static void encode_disabled(json_t *p, const char *mode) {
    json_del(p, "reasoning_effort");
    if (str_eq(mode, "none-effort")) {
        json_set_str(p, "reasoning_effort", "none");
    } else if (str_eq(mode, "zai-thinking-disabled")) {
        json_t *t = json_obj();
        json_set_str(t, "type", "disabled");
        json_set(p, "thinking", t);
    } else if (str_eq(mode, "qwen-enable-thinking-false")) {
        json_set_bool(p, "enable_thinking", false);
    } else if (str_eq(mode, "qwen-template-false") || str_eq(mode, "chat-template-thinking-false")) {
        json_t *k = json_get_obj(p, "chat_template_kwargs");
        if (!k) {
            k = json_obj();
            json_set(p, "chat_template_kwargs", k);
        }
        json_set_bool(k, str_eq(mode, "qwen-template-false") ? "enable_thinking" : "thinking", false);
    } else if (str_eq(mode, "openrouter-enabled-false") || str_eq(mode, "cline-enabled-false")) {
        json_t *r = json_obj();
        json_set_bool(r, "enabled", false);
        json_set(p, "reasoning", r);
    } else if (str_eq(mode, "venice-disable-thinking")) {
        json_t *v = json_get_obj(p, "venice_parameters");
        if (!v) {
            v = json_obj();
            json_set(p, "venice_parameters", v);
        }
        json_set_bool(v, "disable_thinking", true);
    } else {
        json_del(p, "reasoning");
    }
}

static json_t *kwargs(json_t *p) {
    json_t *k = json_get_obj(p, "chat_template_kwargs");
    if (!k) {
        k = json_obj();
        json_set(p, "chat_template_kwargs", k);
    }
    return k;
}

/* applyChatCompletionsCompatPolicy */
static void apply_reasoning_cc(json_t *p, const ai_model_t *m, const oai_policy_t *pol) {
    if (oai_cbool(m, pol->over, "qwenPreserveThinking", false)) {
        if (str_eq(pol->dialect, "qwen")) json_set_bool(p, "preserve_thinking", true);
        json_set_bool(kwargs(p), "preserve_thinking", true);
    }
    if ((!pol->model_supported && !pol->disabled) || !pol->supports_params) return;
    const char *wire = pol->wire[0] ? pol->wire : NULL;
    const char *keep = oai_cstr(m, pol->over, "thinkingKeep", NULL);
    if (pol->enabled) {
        const char *mode = pol->disable_mode;
        if (str_eq(mode, "zai-thinking-disabled")) {
            if (str_eq(wire, "none")) {
                encode_disabled(p, mode);
                return;
            }
            json_t *t = json_obj();
            json_set_str(t, "type", "enabled");
            if (str_eq(pol->dialect, "kimi") && wire) {
                json_set_str(t, "effort", wire);
                if (keep) json_set_str(t, "keep", keep);
                json_set(p, "thinking", t);
                return;
            }
            if (keep) json_set_str(t, "keep", keep);
            json_set(p, "thinking", t);
            if (oai_cbool(m, pol->over, "supportsReasoningEffort", true) && wire) json_set_str(p, "reasoning_effort", wire);
        } else if (str_eq(mode, "qwen-enable-thinking-false")) {
            json_set_bool(p, "enable_thinking", true);
            if (oai_cbool(m, pol->over, "qwenTemplateReasoningEffort", false) && wire) {
                json_set_str(p, "reasoning_effort", wire);
                json_set_str(kwargs(p), "reasoning_effort", wire);
            }
        } else if (str_eq(mode, "qwen-template-false")) {
            json_t *k = kwargs(p);
            json_set_bool(k, "enable_thinking", true);
            if (oai_cbool(m, pol->over, "qwenTemplateReasoningEffort", false) && wire) json_set_str(k, "reasoning_effort", wire);
        } else if (str_eq(mode, "chat-template-thinking-false")) {
            json_t *k = kwargs(p);
            json_set_bool(k, "thinking", true);
            if (wire) json_set_str(k, "reasoning_effort", wire);
        } else if (str_eq(mode, "openrouter-enabled-false")) {
            if (wire) {
                json_t *r = json_obj();
                json_set_str(r, "effort", wire);
                json_set(p, "reasoning", r);
            }
        } else if (!pol->omit_effort && wire) {
            json_set_str(p, "reasoning_effort", wire);
        }
        return;
    }
    if (!pol->disabled) return;
    if (str_eq(pol->disable_reason, "caller") && !pol->requested &&
        (str_eq(pol->disable_mode, "lowest-effort") || str_eq(pol->disable_mode, "none-effort")) && wire) {
        json_set_str(p, "reasoning_effort", wire);
        return;
    }
    encode_disabled(p, pol->disable_mode);
}

json_t *oaic_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o) {
    json_t *p = json_obj();
    const char *wire_id = json_get_str(m->raw, "requestModelId");
    json_set_str(p, "model", wire_id ? wire_id : m->id);
    json_set(p, "messages", json_arr());
    json_set_bool(p, "stream", true);
    bool has_ctx_tools = json_len(ctx->tools) > 0;
    oai_policy_t ipol;
    oai_policy_resolve(&ipol, m, o, o->tool_choice, has_ctx_tools);
    const json_t *iov = ipol.over;

    if (oai_cbool(m, iov, "supportsUsageInStreaming", true)) {
        json_t *so = json_obj();
        json_set_bool(so, "include_usage", true);
        json_set(p, "stream_options", so);
    }
    if (oai_cbool(m, iov, "supportsStore", false)) json_set_bool(p, "store", false);
    if (oai_cbool(m, iov, "supportsSamplingParams", true)) {
        if (!isnan(o->temperature)) json_set_num(p, "temperature", o->temperature);
        if (!isnan(o->top_p)) json_set_num(p, "top_p", o->top_p);
    }
    if (json_len(o->stop_sequences) > 0 && oai_cbool(m, iov, "supportsPenaltyAndStopParams", true)) {
        if (json_len(o->stop_sequences) == 1) {
            json_set(p, "stop", json_clone(json_at(o->stop_sequences, 0)));
        } else {
            json_t *a = json_arr();
            for (size_t i = 0; i < json_len(o->stop_sequences) && i < 4; i++) json_push(a, json_clone(json_at(o->stop_sequences, i)));
            json_set(p, "stop", a);
        }
    }
    if (oai_send_service_tier(m, o->service_tier)) json_set_str(p, "service_tier", o->service_tier);

    if (has_ctx_tools) json_set(p, "tools", convert_tools_cc(m, iov, ctx->tools));
    else if (!ctx->tools && history_has_tools(ctx->messages)) json_set(p, "tools", json_arr());

    /* tool_choice */
    if (o->tool_choice && oai_cbool(m, iov, "supportsToolChoice", true)) {
        const char *mode = oai_tool_choice_mode(o->tool_choice);
        const char *name = oai_tool_choice_name(o->tool_choice);
        if (mode) {
            json_set_str(p, "tool_choice", mode);
        } else if (name) {
            json_t *tc = json_obj();
            json_set_str(tc, "type", "function");
            json_t *fn = json_obj();
            json_set_str(fn, "name", name);
            json_set(tc, "function", fn);
            json_set(p, "tool_choice", tc);
        }
    }
    json_t *tools = json_get(p, "tools");
    json_t *tc = json_get(p, "tool_choice");
    const char *forced_name = json_is_obj(tc) ? json_get_str(json_get_obj(tc, "function"), "name") : NULL;
    char *forced_copy = forced_name ? xstrdup(forced_name) : NULL;
    if (json_is_obj(tc) && !oai_cbool(m, iov, "supportsNamedToolChoice", true)) {
        if (forced_copy && tools_have(tools, forced_copy)) {
            json_t *f = json_arr();
            json_t *t;
            json_arr_foreach(tools, i, t) {
                if (str_eq(json_get_str(json_get_obj(t, "function"), "name"), forced_copy)) json_push(f, json_clone(t));
            }
            json_set(p, "tools", f);
            tools = f;
        }
        json_set_str(p, "tool_choice", "required");
    }
    tc = json_get(p, "tool_choice");
    if (oai_tool_choice_forced(tc) && tc && !oai_cbool(m, iov, "supportsForcedToolChoice", true))
        json_set_str(p, "tool_choice", "auto");
    tc = json_get(p, "tool_choice");
    if (tc && json_len(tools) == 0 && (str_eq(json_sv(tc), "none") || oai_tool_choice_forced(tc))) json_del(p, "tool_choice");
    if (forced_copy && !tools_have(tools, forced_copy) && json_is_obj(json_get(p, "tool_choice"))) json_del(p, "tool_choice");
    free(forced_copy);
    tc = json_get(p, "tool_choice");
    if (str_eq(json_sv(tc), "auto") && oai_cbool(m, NULL, "disableReasoningOnToolChoice", false) && m->reasoning &&
        ipol.requested && !o->disable_reasoning)
        json_del(p, "tool_choice");

    oai_policy_t pol;
    oai_policy_resolve(&pol, m, o, json_get(p, "tool_choice"), json_len(json_get(p, "tools")) > 0);
    json_t *msgs = oaic_convert_messages(m, ctx, pol.over);
    if (str_eq(oai_cstr(m, pol.over, "cacheControlFormat", NULL), "anthropic")) add_anthropic_cache_control(msgs);
    json_set(p, "messages", msgs);

    bool clamp = str_eq(m->provider, "cline-pass") || oai_cbool(m, pol.over, "clampOutputToModelMax", false) ||
                 (str_eq(pol.dialect, "zai") && oai_cbool(m, pol.over, "zaiReasoningEffortDialect", false));
    int64_t cap = oai_output_tokens(m, o, clamp);
    if (cap > 0) {
        const char *field = oai_cstr(m, pol.over, "maxTokensField", "max_completion_tokens");
        json_set_int(p, str_eq(field, "max_tokens") ? "max_tokens" : "max_completion_tokens", cap);
    }
    if (str_eq(pol.dialect, "zai") && oai_cbool(m, pol.over, "zaiReasoningEffortDialect", false) &&
        oai_cbool(m, pol.over, "supportsReasoningEffort", true) && json_len(json_get(p, "tools")) > 0)
        json_set_bool(p, "tool_stream", true);

    apply_reasoning_cc(p, m, &pol);

    json_t *routing = oai_cget(m, pol.over, "openRouterRouting");
    if (oai_cbool(m, pol.over, "isOpenRouterHost", false) && json_is_obj(routing)) json_set(p, "provider", json_clone(routing));
    json_t *extra = oai_cget(m, pol.over, "extraBody");
    if (json_is_obj(extra)) {
        json_t *venice = json_clone(json_get_obj(p, "venice_parameters"));
        const char *k;
        json_t *v;
        json_obj_foreach(extra, ei, k, v) json_set(p, k, json_clone(v));
        if (venice && json_get_bool(venice, "disable_thinking", false)) {
            json_t *merged = json_is_obj(json_get(extra, "venice_parameters")) ? json_clone(json_get(extra, "venice_parameters")) : json_obj();
            json_merge(merged, venice);
            json_set(p, "venice_parameters", merged);
        }
        json_free(venice);
        if (oai_cbool(m, pol.over, "dropThinkingWhenReasoningEffort", false) && json_get(p, "reasoning_effort"))
            json_del(p, "thinking");
    }
    if (oai_cbool(m, NULL, "supportsPromptCacheKey", false)) {
        char *key = oai_prompt_cache_key(o, 64, "pc_");
        if (key) json_set(p, "prompt_cache_key", json_str_own(key));
    }
    return p;
}

/* ====================================================================== */
/* stream decoding                                                         */
/* ====================================================================== */

typedef struct {
    int index;    /* stream index, -1 if none */
    int block;    /* content index */
    char *id;     /* provider id (NULL until seen) */
    sb_t args;
    json_t *obj_args; /* object-shaped arguments (MiniMax) */
} oaic_tc_t;

struct oaic_stream {
    ai_builder_t b;
    const ai_model_t *m;
    VEC(oaic_tc_t) tcs;
    int batch[16]; /* unkeyed batch offset -> tcs index (+1) */
    char *stop;
    char *finish_err;
    char *error;
    bool finished, done, saw_usage, has_response_id;
    /* StreamMarkupHealing over the visible content when compat.streamMarkupHealingPattern is set (only its
     * ThinkingInbandScanner: the kimi/dsml/qwen tool-call grammars are not ported) */
    think_heal_t *heal;
    bool suppress_healed_thinking; /* explicit reasoning seen: drop thinking the healer recovers */
    /* reasoning_details (omp 5759bdacc9 / 4a064201a9 / 67ccb2ca07 / 25c2fb8211): one thinking source per turn (0 none,
     * 1 a reasoning field, 2 the reasoning.text entries), the field mirrored beside winning entries, and every entry as
     * received (streamed fragments of one entry concatenated) */
    int reasoning_source;
    const char *mirrored_field;
    json_t *received_details;
};

oaic_stream_t *oaic_stream_new(const ai_model_t *m, ai_event_cb cb, void *ud) {
    oaic_stream_t *s = xcalloc(1, sizeof(*s));
    s->m = m;
    aib_init(&s->b, m, cb, ud);
    s->b.defer_toolcall_end = true;
    if (json_is_str(model_compat_get(m, "streamMarkupHealingPattern"))) {
        s->heal = xmalloc(sizeof(*s->heal));
        think_heal_init(s->heal);
    }
    return s;
}

static json_t *s_content(oaic_stream_t *s) { return json_get(s->b.msg, "content"); }

static void apply_usage_cc(oaic_stream_t *s, const json_t *u) {
    if (!json_is_obj(u)) return;
    s->saw_usage = true;
    json_t *ptd = json_get_obj(u, "prompt_tokens_details");
    json_t *ctd = json_get_obj(u, "completion_tokens_details");
    int64_t prompt = num_or(u, "prompt_tokens", 0), out = num_or(u, "completion_tokens", 0);
    int64_t cands[4] = {num_or(u, "cached_tokens", -1), num_or(u, "prompt_cache_hit_tokens", -1),
                        num_or(ptd, "cached_tokens", -1), num_or(u, "cachedContentTokenCount", -1)};
    int64_t cached = 0;
    for (int i = 0; i < 4; i++)
        if (cands[i] > 0) {
            cached = cands[i];
            break;
        }
    int64_t hit = num_or(u, "prompt_cache_hit_tokens", -1), miss = num_or(u, "prompt_cache_miss_tokens", -1);
    oai_usage_accounting(&s->b, prompt, out, cached, num_or(ctd, "reasoning_tokens", 0),
                         num_or(ptd, "cache_write_tokens", -1), miss, hit >= 0 && miss >= 0);
}

static void map_finish(oaic_stream_t *s, const char *reason) {
    char low[64];
    snprintf(low, sizeof(low), "%s", reason);
    for (char *c = low; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    const char *stop = "error";
    char *err = NULL;
    if (!strcmp(low, "stop") || !strcmp(low, "end")) stop = "stop";
    else if (!strcmp(low, "length") || !strcmp(low, "max_tokens")) stop = "length";
    else if (!strcmp(low, "tool_calls") || !strcmp(low, "function_call")) stop = "toolUse";
    else if (!strcmp(low, "content_filter")) err = xstrdup("Provider finish_reason: content_filter");
    else if (!strcmp(low, "network_error")) err = xstrdup("Provider finish_reason: network_error");
    else if (!strcmp(low, "error")) err = xstrdup("Provider returned error finish_reason");
    else if (!strcmp(low, "insufficient_system_resource") || !strcmp(low, "unknown") || !strcmp(low, "other") ||
             !strcmp(low, "unspecified") || !strcmp(low, "finish_reason_unspecified"))
        err = xasprintf("Provider returned error finish_reason: %s", low);
    else err = xasprintf("Provider finish_reason: %s", reason);
    free(s->stop);
    s->stop = xstrdup(stop);
    if (err) {
        free(s->finish_err);
        s->finish_err = err;
    }
}

/* accumulateReasoningDetail: a reasoning.text / reasoning.summary fragment of the same entry (type, index, id) extends it */
static void accumulate_detail(json_t *details, const json_t *d) {
    const char *type = json_get_str(d, "type");
    const char *frag = str_eq(type, "reasoning.text") ? "text" : str_eq(type, "reasoning.summary") ? "summary" : NULL;
    json_t *last = json_len(details) ? json_at(details, json_len(details) - 1) : NULL;
    json_t *did = json_get(d, "id"), *lid = last ? json_get(last, "id") : NULL;
    if (!frag || !last || !str_eq(json_get_str(last, "type"), type) || !json_equal(json_get(last, "index"), json_get(d, "index")) ||
        (!json_is_null(did) && !json_is_null(lid) && !json_equal(did, lid))) {
        json_push(details, json_clone(d));
        return;
    }
    const char *k;
    json_t *v;
    json_obj_foreach(d, i, k, v) {
        if (!strcmp(k, frag) && json_is_str(v)) {
            const char *prev = json_get_str(last, k);
            char *cat = xasprintf("%s%s", prev ? prev : "", json_sv(v));
            json_set(last, k, json_str_own(cat));
        } else if (!json_is_null(v) || !json_get(last, k)) {
            json_set(last, k, json_clone(v));
        }
    }
}

static void thinking_delta(oaic_stream_t *s, const char *t, size_t n, const char *sig) {
    if (!n) return;
    bool fresh = s->b.cur_kind != 2;
    aib_thinking_delta(&s->b, t, n);
    if (fresh && sig) aib_set_thinking_signature(&s->b, sig);
    /* openThinkingBlock: chat-completions reasoning is the model's own trace */
    if (fresh) json_set_bool(json_at(s_content(s), (size_t)s->b.cur_index), "summary", false);
}

static void heal_emit(void *ud, int kind, const char *t, size_t n) {
    oaic_stream_t *s = ud;
    if (kind == THINK_HEAL_TEXT) aib_text_delta(&s->b, t, n);
    else if (kind == THINK_HEAL_THINKING && !s->suppress_healed_thinking) thinking_delta(s, t, n, NULL);
}

/* visible content: through the healer when one is configured */
static void visible_text(oaic_stream_t *s, const char *t, size_t n) {
    if (!n) return;
    if (s->heal) think_heal_feed(s->heal, t, n, heal_emit, s);
    else aib_text_delta(&s->b, t, n);
}

static oaic_tc_t *tc_find_id(oaic_stream_t *s, const char *id) {
    for (size_t i = 0; i < s->tcs.n; i++)
        if (s->tcs.v[i].id && str_eq(s->tcs.v[i].id, id)) return &s->tcs.v[i];
    return NULL;
}

static oaic_tc_t *tc_find_index(oaic_stream_t *s, int idx) {
    for (size_t i = s->tcs.n; i > 0; i--)
        if (s->tcs.v[i - 1].index == idx) return &s->tcs.v[i - 1];
    return NULL;
}

static void handle_tool_calls(oaic_stream_t *s, json_t *calls) {
    size_t n = json_len(calls);
    for (size_t k = 0; k < n; k++) {
        json_t *c = json_at(calls, k);
        json_t *fn = json_get_obj(c, "function");
        json_t *jidx = json_get(c, "index");
        int idx = json_is_num(jidx) ? (int)num_or(c, "index", -1) : -1;
        const char *id = json_get_str(c, "id");
        if (id && !*id) id = NULL;
        const char *name = json_get_str(fn, "name");
        if (name && !*name) name = NULL;
        bool unkeyed = n > 1 && idx < 0 && !id;
        oaic_tc_t *t = id ? tc_find_id(s, id) : NULL;
        oaic_tc_t *by_idx = idx >= 0 ? tc_find_index(s, idx) : NULL;
        if (!t && by_idx && (!id || !by_idx->id || !name)) t = by_idx;
        if (!t && unkeyed && k < 16 && s->batch[k]) t = &s->tcs.v[s->batch[k] - 1];
        if (!t && !unkeyed && s->b.cur_kind == 3 && s->tcs.n) {
            oaic_tc_t *last = &s->tcs.v[s->tcs.n - 1];
            if (last->block == s->b.cur_index && (!id || str_eq(last->id, id))) t = last;
        }
        if (!t) {
            aib_toolcall_start(&s->b, id ? id : "", name ? name : "");
            oaic_tc_t nt = {.index = idx, .block = s->b.cur_index, .id = id ? xstrdup(id) : NULL};
            vec_push(&s->tcs, nt);
            t = &s->tcs.v[s->tcs.n - 1];
            if (unkeyed && k < 16) s->batch[k] = (int)s->tcs.n;
        } else if (idx >= 0) {
            t->index = idx;
        }
        json_t *blk = json_at(s_content(s), (size_t)t->block);
        if (id && !str_eq(t->id, id)) {
            free(t->id);
            t->id = xstrdup(id);
            json_set_str(blk, "id", id);
        }
        if (name) json_set_str(blk, "name", name);
        json_t *extra = json_get_obj(c, "extra_content");
        if (extra && (json_get(extra, "google") || json_get(extra, "vertex"))) {
            json_t *sig = json_obj();
            json_set(sig, "perCall", json_clone(extra));
            json_set(blk, "thoughtSignature", json_str_own(json_dump(sig, 0)));
            json_free(sig);
        }
        json_t *args = json_get(fn, "arguments");
        bool current = s->b.cur_kind == 3 && s->b.cur_index == t->block;
        if (json_is_str(args) && args->u.str.len) {
            sb_appendn(&t->args, args->u.str.s, args->u.str.len);
            if (current) {
                aib_toolcall_delta(&s->b, args->u.str.s, args->u.str.len);
            } else {
                json_t *pa = json_parse_partial(t->args.s, t->args.len);
                if (json_is_obj(pa)) json_set(blk, "arguments", pa);
                else json_free(pa);
                oai_emit(&s->b, AI_EV_TOOLCALL_DELTA, t->block, args->u.str.s, args->u.str.len, NULL);
            }
        } else if (json_is_obj(args)) {
            if (!t->obj_args) t->obj_args = json_obj();
            json_merge(t->obj_args, args);
            json_set(blk, "arguments", json_clone(t->obj_args));
            oai_emit(&s->b, AI_EV_TOOLCALL_DELTA, t->block, "", 0, NULL);
        }
    }
}

static void handle_content_value(oaic_stream_t *s, json_t *content) {
    if (json_is_str(content)) {
        visible_text(s, content->u.str.s, content->u.str.len);
        return;
    }
    json_t *part;
    json_arr_foreach(content, i, part) {
        const char *t = json_get_str(part, "type");
        if (str_eq(t, "text")) {
            json_t *tx = json_get(part, "text");
            if (json_is_str(tx)) visible_text(s, tx->u.str.s, tx->u.str.len);
        } else if (str_eq(t, "thinking")) {
            json_t *inner;
            json_arr_foreach(json_get(part, "thinking"), j, inner) {
                json_t *tx = json_get(inner, "text");
                if (str_eq(json_get_str(inner, "type"), "text") && json_is_str(tx) && tx->u.str.len) {
                    thinking_delta(s, tx->u.str.s, tx->u.str.len, "mistral-content-parts");
                    s->suppress_healed_thinking = true;
                }
            }
        }
    }
}

int oaic_stream_on_sse(void *ud, const char *event, const char *data, size_t len) {
    oaic_stream_t *s = ud;
    while (len && (*data == ' ' || *data == '\n' || *data == '\r')) data++, len--;
    while (len && (data[len - 1] == ' ' || data[len - 1] == '\n' || data[len - 1] == '\r')) len--;
    if (!len) return 0;
    if (len == 6 && memcmp(data, "[DONE]", 6) == 0) {
        s->done = true;
        return OAI_STREAM_STOP;
    }
    json_t *j = json_parse(data, len, NULL);
    if (!json_is_obj(j)) {
        json_free(j);
        return 0;
    }
    json_t *err = json_get(j, "error");
    const char *flat = json_get_str(j, "message");
    if (json_is_obj(err) || json_is_str(err) || (flat && !json_get(j, "choices"))) {
        const char *msg = json_is_obj(err) ? json_get_str(err, "message") : json_is_str(err) ? json_sv(err) : flat;
        json_t *code = json_is_obj(err) ? json_get(err, "code") : NULL;
        free(s->error);
        if (json_is_num(code) || json_is_str(code)) {
            char *cs = json_is_str(code) ? xstrdup(json_sv(code)) : json_dump(code, 0);
            s->error = xasprintf("%s %s", cs, msg ? msg : "Provider returned an in-band stream error");
            free(cs);
        } else {
            s->error = xstrdup(msg ? msg : "Provider returned an in-band OpenAI completions stream error");
        }
        json_free(j);
        return OAI_STREAM_STOP;
    }
    aib_start(&s->b);
    const char *rid = json_get_str(j, "id");
    if (rid && !s->has_response_id) {
        aib_set(&s->b, "responseId", json_str(rid));
        s->has_response_id = true;
    }
    if (json_is_obj(json_get(j, "usage"))) apply_usage_cc(s, json_get(j, "usage"));
    json_t *choice = json_at(json_get_arr(j, "choices"), 0);
    if (!json_is_obj(choice)) {
        int rc = s->finished && s->saw_usage ? OAI_STREAM_STOP : 0;
        json_free(j);
        return rc;
    }
    if (!json_is_obj(json_get(j, "usage")) && json_is_obj(json_get(choice, "usage")))
        apply_usage_cc(s, json_get(choice, "usage"));
    const char *fr = json_get_str(choice, "finish_reason");
    const char *sr = json_get_str(choice, "stop_reason");
    if (str_ieq(sr, "repetition") || str_ieq(sr, "repetition_detected")) {
        free(s->stop);
        s->stop = xstrdup("error");
        free(s->finish_err);
        s->finish_err = xasprintf("Provider stopped generation: %s%s%s", fr ? "finish_reason=" : "", fr ? fr : "",
                                  sr ? " stop_reason=repetition" : "");
        s->finished = true;
    } else if (fr && *fr) {
        map_finish(s, fr);
        s->finished = true;
    }
    json_t *delta = json_get_obj(choice, "delta");
    if (delta) {
        static const char *const fields[] = {"reasoning_content", "reasoning", "reasoning_text"};
        const char *found = NULL;
        json_t *fv = NULL;
        for (int i = 0; i < 3 && !found; i++) {
            json_t *v = json_get(delta, fields[i]);
            if (json_is_str(v) && v->u.str.len) found = fields[i], fv = v;
        }
        sb_t details_text = SB_INIT;
        json_t *det0;
        json_arr_foreach(json_get_arr(delta, "reasoning_details"), di0, det0) {
            if (str_eq(json_get_str(det0, "type"), "reasoning.text") && json_get_str(det0, "text"))
                sb_append(&details_text, json_get_str(det0, "text"));
        }
        if (found && s->reasoning_source != 2) {
            s->reasoning_source = 1;
            thinking_delta(s, fv->u.str.s, fv->u.str.len, found);
            s->suppress_healed_thinking = true;
        } else if (s->reasoning_source != 1) {
            /* Bifrost streams Gemini thinking only as reasoning.text entries; once they won the turn, a plain field beside
             * them is OpenRouter's mirror: kept as the replay marker, never rendered */
            if (found && !s->mirrored_field) s->mirrored_field = found;
            if (details_text.len) {
                s->reasoning_source = 2;
                thinking_delta(s, details_text.s, details_text.len, NULL);
                s->suppress_healed_thinking = true;
            }
        }
        sb_free(&details_text);
        handle_content_value(s, json_get(delta, "content"));
        json_t *calls = json_get_arr(delta, "tool_calls");
        if (json_len(calls)) handle_tool_calls(s, calls);
        json_t *det;
        json_arr_foreach(json_get_arr(delta, "reasoning_details"), di, det) {
            if (!json_is_obj(det)) continue;
            if (!s->received_details) s->received_details = json_arr();
            accumulate_detail(s->received_details, det);
            /* an encrypted entry also stays on the tool call it names (blob in OpenRouter's `data` or Bifrost's
             * `signature`): a pass that rewrites the turn drops the envelope, and an unchanged call keeps its signature */
            if (!str_eq(json_get_str(det, "type"), "reasoning.encrypted")) continue;
            const char *did = json_get_str(det, "id");
            if (!did || (json_is_null(json_get(det, "data")) && json_is_null(json_get(det, "signature")))) continue;
            json_t *blk;
            json_arr_foreach(s_content(s), bi, blk) {
                if (str_eq(json_get_str(blk, "type"), "toolCall") && str_eq(json_get_str(blk, "id"), did)) {
                    json_t *sig = json_obj();
                    json_set(sig, "perCall", json_clone(det));
                    json_set(blk, "thoughtSignature", json_str_own(json_dump(sig, 0)));
                    json_free(sig);
                }
            }
        }
    }
    json_free(j);
    return 0;
}

static void free_stream(oaic_stream_t *s) {
    for (size_t i = 0; i < s->tcs.n; i++) {
        free(s->tcs.v[i].id);
        sb_free(&s->tcs.v[i].args);
        json_free(s->tcs.v[i].obj_args);
    }
    vec_free(&s->tcs);
    free(s->stop);
    free(s->finish_err);
    free(s->error);
    if (s->heal) {
        think_heal_free(s->heal);
        free(s->heal);
    }
    json_free(s->received_details);
    aib_free(&s->b);
    free(s);
}

static bool has_visible(const json_t *content) {
    json_t *b;
    json_arr_foreach(content, i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "toolCall")) return true;
        if (str_eq(t, "text") && !blank(json_get_str(b, "text"))) return true;
        if (str_eq(t, "thinking") && !blank(json_get_str(b, "thinking"))) return true;
    }
    return false;
}

json_t *oaic_stream_finish(oaic_stream_t *s, int rc, const char *terr) {
    /* held-back healer fragments flush once the stream completed (a thrown stream error skips this upstream) */
    if (s->heal && (rc == 0 || rc == OAI_STREAM_STOP) && !s->error) think_heal_flush(s->heal, heal_emit, s);
    aib_end_block(&s->b);
    /* keep the received reasoning_details on the first thinking block whose signature only names its text's source
     * field, so replay sends them back verbatim; with no such block an empty thinking block holds them only when the turn
     * has other content (an encrypted-only turn stays empty so the empty-stop retry still runs) */
    if (json_len(s->received_details) > 0) {
        json_t *holder = NULL, *blk;
        json_arr_foreach(s_content(s), hi, blk) {
            if (holder || !str_eq(json_get_str(blk, "type"), "thinking")) continue;
            const char *sig = json_get_str(blk, "thinkingSignature");
            if (!sig || oai_is_reasoning_field(sig)) holder = blk;
        }
        json_t *env = json_obj();
        json_set(env, "reasoningDetails", s->received_details);
        s->received_details = NULL;
        const char *hs = holder ? json_get_str(holder, "thinkingSignature") : NULL;
        const char *field = oai_is_reasoning_field(hs) ? hs : s->mirrored_field;
        if (field) json_set_str(env, "field", field);
        char *sig = json_dump_js(env);
        json_free(env);
        if (holder) json_set_str(holder, "thinkingSignature", sig);
        else if (json_len(s_content(s)) > 0) {
            json_t *tb = json_obj();
            json_set_str(tb, "type", "thinking");
            json_set_str(tb, "thinking", "");
            json_set_str(tb, "thinkingSignature", sig);
            json_set_bool(tb, "summary", false);
            json_push(s_content(s), tb);
        }
        free(sig);
    }
    /* authoritative tool arguments from the per-call accumulators */
    for (size_t i = 0; i < s->tcs.n; i++) {
        oaic_tc_t *t = &s->tcs.v[i];
        json_t *blk = json_at(s_content(s), (size_t)t->block);
        if (t->obj_args) json_set(blk, "arguments", json_clone(t->obj_args));
        else if (t->args.len) json_set(blk, "arguments", oai_parse_args(t->args.s, t->args.len));
    }
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
    } else if (!s->finished && !s->done && aib_block_count(&s->b) > 0) {
        stop = "error";
        err = "OpenAI completions stream closed before a finish_reason was received";
    } else if (str_eq(stop, "error")) {
        err = s->finish_err ? s->finish_err : "Provider returned an error stop reason";
    } else if (str_eq(stop, "length") && oai_cbool(s->m, NULL, "emptyLengthFinishIsContextError", false) &&
               !has_visible(s_content(s))) {
        stop = "error";
        err = "Model returned no content: prompt filled the context window; raise Ollama num_ctx or shorten the prompt.";
    }
    json_t *msg = aib_finish(&s->b, stop, err);
    free_stream(s);
    return msg;
}

/* ====================================================================== */
/* entry point                                                             */
/* ====================================================================== */

typedef struct {
    const ai_model_t *m;
    const ai_context_t *ctx;
    const ai_stream_opts_t *o;
} oaic_attempt_t;

static json_t *oaic_stream_once(void *self, ai_event_cb cb, void *ud) {
    oaic_attempt_t *at = self;
    const ai_model_t *m = at->m;
    const ai_context_t *ctx = at->ctx;
    const ai_stream_opts_t *o = at->o;
    char *key = oai_bearer_key(m, o);
    json_t *body = oaic_build_request(m, ctx, o);
    if (o->on_payload) o->on_payload(o->payload_ud, body);
    char *base = oai_base_url(m, env_get("OPENAI_BASE_URL") ? env_get("OPENAI_BASE_URL") : "https://api.openai.com/v1");
    char *url;
    if (strstr(base, ".openai.azure.com")) {
        const char *ver = env_get("AZURE_OPENAI_API_VERSION") ? env_get("AZURE_OPENAI_API_VERSION") : "2024-10-21";
        char *ever = url_encode(ver);
        if (strstr(base, "/deployments/")) url = xasprintf("%s/chat/completions?api-version=%s", base, ever);
        else url = xasprintf("%s/deployments/%s/chat/completions?api-version=%s", base, m->id, ever);
        free(ever);
    } else {
        url = xasprintf("%s/chat/completions", base);
    }
    http_headers_t h = {0};
    if (json_is_obj(m->headers)) {
        const char *k;
        json_t *v;
        json_obj_foreach(m->headers, i, k, v) if (json_is_str(v)) http_headers_set(&h, k, json_sv(v));
    }
    if (str_eq(m->provider, "openrouter")) oai_add_openrouter_headers(&h);
    ai_apply_headers(&h, NULL, o);
    oai_apply_inference_headers(&h, m, o);
    oai_apply_prompt_cache_session_header(&h, m, o);
    if (key && strcmp(key, AI_NO_AUTH_SENTINEL) != 0 && !http_headers_get(&h, "Authorization")) {
        char *a = xasprintf("Bearer %s", key);
        http_headers_set(&h, "Authorization", a);
        free(a);
    }
    char *payload = json_dump(body, 0);
    oaic_stream_t *st = oaic_stream_new(m, cb, ud);
    int status = 0;
    char *err = NULL;
    int rc = ai_sse_post(url, &h, payload, o, oaic_stream_on_sse, st, &status, &err);
    json_t *msg = oaic_stream_finish(st, rc, err);
    free(err);
    free(payload);
    http_headers_free(&h);
    free(url);
    free(base);
    json_free(body);
    free(key);
    return msg;
}

/* streamOpenAICompletions: postOpenAIStream's 6 transport attempts (or maxHttpAttempts), retried once more after a
 * replay-safe provider error unless maxHttpAttempts is set, and up to twice after a benign empty completion */
json_t *provider_openai_completions_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                                           ai_event_cb cb, void *ud) {
    ai_stream_opts_t lo;
    if (o) lo = *o;
    else ai_stream_opts_init(&lo);
    lo.retry_policy = AI_RETRY_OPENAI;
    lo.max_retries = (lo.max_http_attempts > 0 ? lo.max_http_attempts : 6) - 1;
    oaic_attempt_t at = {m, ctx, &lo};
    return ai_replay_safe_stream(oaic_stream_once, &at, &lo, cb, ud, true, lo.max_http_attempts > 0 ? 0 : 1);
}
