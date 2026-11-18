/*
 * provider_bedrock.c - Amazon Bedrock ConverseStream transport (port of omp amazon-bedrock.ts).
 *
 * Calls bedrock-runtime.{region}.amazonaws.com/model/{id}/converse-stream directly over HTTPS with SigV4
 * (or an Amazon Bedrock API key as a bearer token) and decodes application/vnd.amazon.eventstream.
 * A models.yml baseUrl is used verbatim (VPC endpoint, gateway, ...); only AWS's own regional host is
 * re-pointed at the resolved region.
 */
#define _GNU_SOURCE
#include "provider_bedrock.h"

#include <math.h>
#include <strings.h>

#define NO_TOOLS_SENTINEL_NAME "__no_tools__"
#define THINKING_BINDING_CONTROLS_BETA "thinking-binding-controls-2026-08-01"
#define MAX_TOOL_CALL_ID_LEN 64
#define MIN_OUTPUT_TOKENS 1024
#define OUTPUT_CAP_WHEN_UNKNOWN 64000
#define REQUEST_METADATA_MAX_ENTRIES 16
#define MAX_STREAM_BLOCKS 4096

/* stream.ts BEDROCK_CLAUDE_THINKING (option shaping) */
static const int64_t BEDROCK_CLAUDE_THINKING[] = {0, 1024, 2048, 8192, 16384, 16384, 32768};
/* amazon-bedrock.ts DEFAULT_BEDROCK_THINKING_BUDGETS (request body) */
static const int64_t DEFAULT_BEDROCK_THINKING_BUDGETS[] = {0, 1024, 2048, 8192, 16384, 32768, 32768};

/* ======================================================================
 * small helpers
 * ====================================================================== */

static bool blank(const char *s) {
    if (!s) return true;
    for (; *s; s++)
        if (!strchr(" \t\n\r\f\v", *s)) return false;
    return true;
}

static const char *thinking_mode(const ai_model_t *m) { return m->thinking ? json_get_str(m->thinking, "mode") : NULL; }
static bool thinking_flag(const ai_model_t *m, const char *k) { return m->thinking && json_get_bool(m->thinking, k, false); }

static bool compat_flag(const ai_model_t *m, const char *k, bool def) {
    json_t *v = model_compat_get(m, k);
    return json_is_bool(v) ? v->u.b : def;
}

static char *normalize_tool_id(const char *id) {
    if (!id) id = "";
    size_t n = strlen(id);
    if (n > MAX_TOOL_CALL_ID_LEN) n = MAX_TOOL_CALL_ID_LEN;
    char *out = xmalloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        out[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
                     ? c
                     : '_';
    }
    out[n] = '\0';
    return out;
}

/* renderDemotedThinking: thinking rendered as visible text in the target model's dialect */
static char *render_demoted_thinking(const char *model_id, const char *text) {
    if (!text || !*text) return xstrdup("");
    if (str_istr(model_id, "claude")) return xstrdup(text);
    if (str_istr(model_id, "gemini")) return xasprintf("```thinking\n%s\n```", text);
    if (str_istr(model_id, "minimax")) return xasprintf("<thinking>\n%s\n</thinking>", text);
    return xasprintf("<think>\n%s\n</think>", text);
}

static bool efforts_contain(const json_t *arr, ai_effort_t e) {
    json_t *it;
    json_arr_foreach(arr, i, it) {
        if (str_eq(json_sv(it), effort_name(e))) return true;
    }
    return false;
}

/* requireSupportedEffort (clamped like the rest of omc instead of throwing) */
static ai_effort_t clamp_effort(const ai_model_t *m, ai_effort_t e) {
    json_t *arr = m->thinking ? json_get_arr(m->thinking, "efforts") : NULL;
    if (!arr || json_len(arr) == 0 || efforts_contain(arr, e)) return e;
    for (int x = (int)e; x >= EFFORT_MINIMAL; x--)
        if (efforts_contain(arr, (ai_effort_t)x)) return (ai_effort_t)x;
    for (int x = (int)e; x <= EFFORT_MAX; x++)
        if (efforts_contain(arr, (ai_effort_t)x)) return (ai_effort_t)x;
    return e;
}

static ai_effort_t minimum_effort(const ai_model_t *m) {
    json_t *arr = m->thinking ? json_get_arr(m->thinking, "efforts") : NULL;
    for (int x = EFFORT_MINIMAL; x <= EFFORT_MAX; x++)
        if (arr && efforts_contain(arr, (ai_effort_t)x)) return (ai_effort_t)x;
    return EFFORT_OFF;
}

/* effortMap lookup; "minimal" -> "low" (mapEffortToAnthropicAdaptiveEffort) */
static const char *mapped_effort(const ai_model_t *m, ai_effort_t e, bool minimal_to_low) {
    json_t *map = m->thinking ? json_get_obj(m->thinking, "effortMap") : NULL;
    const char *s = map ? json_get_str(map, effort_name(e)) : NULL;
    if (!s) s = effort_name(e);
    if (minimal_to_low && !strcmp(s, "minimal")) return "low";
    return s;
}

static int64_t custom_budget(const ai_stream_opts_t *o, ai_effort_t e) {
    json_t *v = o->thinking_budgets ? json_get(o->thinking_budgets, effort_name(e)) : NULL;
    return json_is_num(v) ? json_d2i(v->u.num.d) : -1;
}

/* encodeURIComponent */
static void uri_component(sb_t *b, const char *s) {
    static const char HEXU[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()", c))
            sb_putc(b, (char)c);
        else {
            sb_putc(b, '%');
            sb_putc(b, HEXU[c >> 4]);
            sb_putc(b, HEXU[c & 15]);
        }
    }
}

/* ======================================================================
 * region / URL
 * ====================================================================== */

static char *region_from_arn(const char *id) {
    if (!id || !str_starts(id, "arn:")) return NULL;
    strvec_t parts = str_split(id, ":");
    char *r = NULL;
    if (parts.n >= 4 && !strcmp(parts.v[0], "arn") && !strcmp(parts.v[2], "bedrock") && *parts.v[3])
        r = xstrdup(parts.v[3]);
    strvec_free(&parts);
    return r;
}

static const struct {
    const char *geo, *region;
} GEO_DEFAULT[] = {{"us", "us-east-1"},        {"us-gov", "us-gov-west-1"}, {"eu", "eu-west-1"},
                   {"apac", "ap-southeast-1"}, {"au", "ap-southeast-2"},    {"jp", "ap-northeast-1"}};

static const char *profile_geo(const char *id) {
    const char *dot = id ? strchr(id, '.') : NULL;
    if (!dot || dot == id) return NULL;
    size_t n = (size_t)(dot - id);
    for (size_t i = 0; i < ARRAY_LEN(GEO_DEFAULT); i++)
        if (strlen(GEO_DEFAULT[i].geo) == n && !strncmp(id, GEO_DEFAULT[i].geo, n)) return GEO_DEFAULT[i].geo;
    return NULL;
}

static const char *geo_default(const char *geo) {
    for (size_t i = 0; i < ARRAY_LEN(GEO_DEFAULT); i++)
        if (!strcmp(GEO_DEFAULT[i].geo, geo)) return GEO_DEFAULT[i].region;
    return "us-east-1";
}

static bool region_serves_geo(const char *r, const char *geo) {
    if (!strcmp(geo, "us-gov")) return str_starts(r, "us-gov-");
    if (!strcmp(geo, "us")) return str_starts(r, "us-") && !str_starts(r, "us-gov-");
    if (!strcmp(geo, "eu")) return str_starts(r, "eu-");
    if (!strcmp(geo, "apac")) return str_starts(r, "ap-");
    if (!strcmp(geo, "au")) return !strcmp(r, "ap-southeast-2") || !strcmp(r, "ap-southeast-4");
    if (!strcmp(geo, "jp")) return !strcmp(r, "ap-northeast-1") || !strcmp(r, "ap-northeast-3");
    return false;
}

char *bedrock_resolve_region(const ai_model_t *m, const char *guardrail_identifier) {
    char *explicit_region = region_from_arn(m->id);
    if (explicit_region) return explicit_region;
    char *ambient = aws_ambient_region(NULL);
    char *guard = region_from_arn(guardrail_identifier);
    const char *geo = profile_geo(m->id);
    char *out;
    if (geo) {
        if (ambient && region_serves_geo(ambient, geo)) out = xstrdup(ambient);
        else if (guard && region_serves_geo(guard, geo)) out = xstrdup(guard);
        else out = xstrdup(geo_default(geo));
    } else {
        out = xstrdup(ambient ? ambient : guard ? guard : "us-east-1");
    }
    free(ambient);
    free(guard);
    return out;
}

/* ^bedrock-runtime\.[a-z0-9-]+\.amazonaws\.com$ */
static bool is_aws_regional_host(const char *h) {
    const char *pre = "bedrock-runtime.", *suf = ".amazonaws.com";
    size_t n = strlen(h), pl = strlen(pre), sl = strlen(suf);
    if (n <= pl + sl || strncmp(h, pre, pl) || strcmp(h + n - sl, suf)) return false;
    for (size_t i = pl; i < n - sl; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

typedef struct {
    char *url;   /* full request URL */
    char *host;  /* Host header value (host[:port]) */
    char *path;  /* URL path (no query) */
    char *query; /* without '?', or NULL */
} bedrock_target_t;

static void target_free(bedrock_target_t *t) {
    free(t->url);
    free(t->host);
    free(t->path);
    free(t->query);
}

static int resolve_target(const ai_model_t *m, const char *region, bedrock_target_t *t) {
    memset(t, 0, sizeof(*t));
    char *base = m->base_url && *m->base_url ? xstrdup(m->base_url)
                                             : xasprintf("https://bedrock-runtime.%s.amazonaws.com", region);
    url_t u;
    if (url_parse(base, &u) != 0) {
        free(base);
        return -1;
    }
    free(base);
    char *hostname = xstrdup(u.host);
    if (is_aws_regional_host(hostname)) {
        free(hostname);
        hostname = xasprintf("bedrock-runtime.%s.amazonaws.com", region);
    }
    bool tls = !strcmp(u.scheme, "https") || !strcmp(u.scheme, "wss");
    bool def_port = (tls && u.port == 443) || (!tls && u.port == 80);
    bool v6 = strchr(hostname, ':') != NULL;
    t->host = def_port ? xasprintf("%s%s%s", v6 ? "[" : "", hostname, v6 ? "]" : "")
                       : xasprintf("%s%s%s:%d", v6 ? "[" : "", hostname, v6 ? "]" : "", u.port);
    const char *q = strchr(u.path, '?');
    char *pathname = q ? xstrndup(u.path, (size_t)(q - u.path)) : xstrdup(u.path);
    if (q && q[1]) t->query = xstrdup(q + 1);
    size_t pl = strlen(pathname);
    while (pl > 0 && pathname[pl - 1] == '/') pathname[--pl] = '\0';
    sb_t p = SB_INIT;
    sb_appendf(&p, "%s/model/", pathname);
    uri_component(&p, m->id);
    sb_append(&p, "/converse-stream");
    t->path = sb_detach(&p);
    free(pathname);
    t->url = xasprintf("%s://%s%s%s%s", u.scheme, t->host, t->path, t->query ? "?" : "", t->query ? t->query : "");
    free(hostname);
    url_free(&u);
    return 0;
}

char *bedrock_request_url(const ai_model_t *m, const char *region) {
    bedrock_target_t t;
    if (resolve_target(m, region, &t) != 0) return NULL;
    char *u = t.url;
    t.url = NULL;
    target_free(&t);
    return u;
}

/* ======================================================================
 * request body
 * ====================================================================== */

typedef struct {
    int remaining;
    bool ttl_1h;
} cache_policy_t;

static json_t *take_cache_point(cache_policy_t *p) {
    if (p->remaining <= 0) return NULL;
    p->remaining--;
    json_t *cp = json_obj(), *in = json_obj();
    json_set_str(in, "type", "default");
    if (p->ttl_1h) json_set_str(in, "ttl", "1h");
    json_set(cp, "cachePoint", in);
    return cp;
}

static const char *cache_retention(const ai_stream_opts_t *o) {
    if (o->cache_retention && *o->cache_retention) return o->cache_retention;
    const char *e = env_get("PI_CACHE_RETENTION");
    if (str_eq(e, "long") || str_eq(e, "short") || str_eq(e, "none")) return e;
    return "short";
}

static cache_policy_t prompt_cache_policy(const ai_model_t *m, const char *retention) {
    cache_policy_t p = {0, false};
    const char *mode = model_compat_str(m, "promptCacheMode");
    if (!strcmp(retention, "none") || str_eq(mode, "automatic")) return p;
    bool forced = env_truthy("AWS_BEDROCK_FORCE_CACHE");
    bool explicit_mode = str_eq(mode, "explicit");
    if (!explicit_mode && !forced) return p;
    int64_t max = 2;
    if (explicit_mode) {
        json_t *v = model_compat_get(m, "promptCacheMaximumCheckpoints");
        max = json_is_num(v) ? json_d2i(v->u.num.d) : 0;
    }
    if (max <= 0) return p;
    p.remaining = (int)MIN(max, 2);
    p.ttl_1h = !strcmp(retention, "long") && compat_flag(m, "supportsLongPromptCacheRetention", false);
    return p;
}

static json_t *image_block(const char *mime, const char *data, char **err) {
    const char *fmt = NULL;
    if (str_eq(mime, "image/jpeg") || str_eq(mime, "image/jpg")) fmt = "jpeg";
    else if (str_eq(mime, "image/png")) fmt = "png";
    else if (str_eq(mime, "image/gif")) fmt = "gif";
    else if (str_eq(mime, "image/webp")) fmt = "webp";
    if (!fmt) {
        *err = xasprintf("Unknown image type: %s", mime ? mime : "undefined");
        return NULL;
    }
    json_t *img = json_obj(), *src = json_obj();
    json_set_str(src, "bytes", data ? data : "");
    json_set(img, "source", src);
    json_set_str(img, "format", fmt);
    json_t *w = json_obj();
    json_set(w, "image", img);
    return w;
}

static json_t *text_wire(const char *t) {
    json_t *b = json_obj();
    json_set_str(b, "text", t ? t : "");
    return b;
}

static json_t *tool_result_block(const ai_model_t *m, const json_t *msg, json_t *hoisted, char **err) {
    json_t *content = json_arr();
    bool is_error = json_get_bool(msg, "isError", false);
    /* Claude on Bedrock rejects non-text blocks in an error result: always hoist those */
    bool hoist = is_error || json_get_bool(m->raw, "requiresToolResultImageHoisting", false);
    json_t *blk;
    json_arr_foreach(json_get(msg, "content"), i, blk) {
        if (str_eq(json_get_str(blk, "type"), "image")) {
            json_t *img = image_block(json_get_str(blk, "mimeType"), json_get_str(blk, "data"), err);
            if (!img) {
                json_free(content);
                return NULL;
            }
            if (hoist) {
                json_push(content, text_wire("(see attached image)"));
                json_push(hoisted, img);
            } else {
                json_push(content, img);
            }
        } else {
            json_push(content, text_wire(json_get_str(blk, "text")));
        }
    }
    json_t *tr = json_obj();
    json_set(tr, "toolUseId", json_str_own(normalize_tool_id(json_get_str(msg, "toolCallId"))));
    json_set(tr, "content", content);
    json_set_str(tr, "status", is_error ? "error" : "success");
    json_t *w = json_obj();
    json_set(w, "toolResult", tr);
    return w;
}

static json_t *wire_msg(const char *role, json_t *content) {
    json_t *w = json_obj();
    json_set_str(w, "role", role);
    json_set(w, "content", content);
    return w;
}

static json_t *reasoning_wire(const char *text, const char *sig) {
    json_t *rt = json_obj();
    json_set_str(rt, "text", text ? text : "");
    json_set_str(rt, "signature", sig);
    json_t *rc = json_obj();
    json_set(rc, "reasoningText", rt);
    json_t *w = json_obj();
    json_set(w, "reasoningContent", rc);
    return w;
}

static json_t *convert_user(const json_t *msg, char **err) {
    json_t *c = json_get(msg, "content");
    if (json_is_str(c)) {
        if (blank(json_sv(c))) return NULL;
        json_t *arr = json_arr();
        json_push(arr, text_wire(json_sv(c)));
        return wire_msg("user", arr);
    }
    json_t *blocks = json_arr(), *blk;
    json_arr_foreach(c, j, blk) {
        const char *ty = json_get_str(blk, "type");
        if (str_eq(ty, "text")) {
            const char *t = json_get_str(blk, "text");
            if (blank(t)) continue;
            json_push(blocks, text_wire(t));
        } else if (str_eq(ty, "image")) {
            json_t *img = image_block(json_get_str(blk, "mimeType"), json_get_str(blk, "data"), err);
            if (!img) break;
            json_push(blocks, img);
        } else {
            *err = xstrdup("Unknown user content type");
            break;
        }
    }
    if (*err || json_len(blocks) == 0) {
        json_free(blocks);
        return NULL;
    }
    return wire_msg("user", blocks);
}

static json_t *convert_assistant(const ai_model_t *m, const json_t *msg, char **err) {
    json_t *c = json_get(msg, "content");
    if (json_len(c) == 0) return NULL;
    json_t *blocks = json_arr(), *blk;
    json_arr_foreach(c, j, blk) {
        const char *ty = json_get_str(blk, "type");
        if (str_eq(ty, "text")) {
            const char *t = json_get_str(blk, "text");
            if (blank(t)) continue;
            json_push(blocks, text_wire(t));
        } else if (str_eq(ty, "toolCall")) {
            json_t *tu = json_obj();
            json_set(tu, "toolUseId", json_str_own(normalize_tool_id(json_get_str(blk, "id"))));
            json_set_str(tu, "name", json_get_str(blk, "name") ? json_get_str(blk, "name") : "");
            json_t *args = json_get(blk, "arguments");
            json_set(tu, "input", args ? json_clone(args) : json_obj());
            json_t *w = json_obj();
            json_set(w, "toolUse", tu);
            json_push(blocks, w);
        } else if (str_eq(ty, "thinking")) {
            const char *th = json_get_str(blk, "thinking");
            const char *sig = json_get_str(blk, "thinkingSignature");
            if (blank(th) && (!sig || !*sig)) continue;
            if (sig && *sig) {
                json_push(blocks, reasoning_wire(th, sig));
            } else {
                /* unsigned reasoning is not replayable (Nova rejects it): demote to text */
                char *d = render_demoted_thinking(m->id, th);
                json_push(blocks, text_wire(d));
                free(d);
            }
        } else {
            *err = xstrdup("Unknown assistant content type");
            break;
        }
    }
    if (*err || json_len(blocks) == 0) {
        json_free(blocks);
        return NULL;
    }
    return wire_msg("assistant", blocks);
}

static json_t *convert_messages(const ai_model_t *m, const json_t *messages, cache_policy_t *cp, char **err) {
    json_t *result = json_arr();
    json_t *empty = messages ? NULL : json_arr();
    json_t *tm = ai_transform_messages(messages ? messages : empty, m);
    json_free(empty);
    size_t n = json_len(tm);
    for (size_t i = 0; i < n && !*err; i++) {
        const json_t *msg = json_at(tm, i);
        const char *role = json_get_str(msg, "role");
        json_t *w = NULL;
        if (str_eq(role, "user") || str_eq(role, "developer")) {
            w = convert_user(msg, err);
        } else if (str_eq(role, "assistant")) {
            w = convert_assistant(m, msg, err);
        } else if (str_eq(role, "toolResult")) {
            /* all consecutive tool results go into one user message (Bedrock requirement) */
            json_t *blocks = json_arr(), *hoisted = json_arr();
            size_t j = i;
            for (; j < n && str_eq(json_get_str(json_at(tm, j), "role"), "toolResult"); j++) {
                json_t *b = tool_result_block(m, json_at(tm, j), hoisted, err);
                if (!b) break;
                json_push(blocks, b);
            }
            i = j - 1;
            while (json_len(hoisted)) json_push(blocks, json_arr_remove(hoisted, 0));
            json_free(hoisted);
            if (*err) json_free(blocks);
            else w = wire_msg("user", blocks);
        } else {
            *err = xstrdup("Unknown message role");
        }
        if (w) json_push(result, w);
    }
    json_free(tm);
    /* the final user boundary gets the first cache checkpoint */
    size_t rn = json_len(result);
    if (!*err && rn > 0) {
        json_t *last = json_at(result, rn - 1);
        if (str_eq(json_get_str(last, "role"), "user")) {
            json_t *pt = take_cache_point(cp);
            if (pt) json_push(json_get(last, "content"), pt);
        }
    }
    return result;
}

static json_t *build_system(const ai_context_t *ctx, cache_policy_t *cp) {
    json_t *blocks = json_arr(), *it;
    if (ctx && json_is_arr(ctx->system_prompt)) {
        json_arr_foreach(ctx->system_prompt, i, it) {
            if (json_is_str(it) && !blank(json_sv(it))) json_push(blocks, text_wire(json_sv(it)));
        }
    } else if (ctx && json_is_str(ctx->system_prompt) && !blank(json_sv(ctx->system_prompt))) {
        json_push(blocks, text_wire(json_sv(ctx->system_prompt)));
    }
    if (json_len(blocks) == 0) {
        json_free(blocks);
        return NULL;
    }
    json_t *pt = take_cache_point(cp);
    if (pt) json_push(blocks, pt);
    return blocks;
}

static bool messages_have_tool_blocks(const json_t *msgs) {
    json_t *msg, *blk;
    json_arr_foreach(msgs, i, msg) {
        json_arr_foreach(json_get(msg, "content"), j, blk) {
            if (json_get(blk, "toolUse") || json_get(blk, "toolResult")) return true;
        }
    }
    return false;
}

static json_t *tool_spec(const json_t *tool) {
    json_t *spec = json_obj();
    json_set_str(spec, "name", json_get_str(tool, "name") ? json_get_str(tool, "name") : "");
    /* Bedrock permits omission but rejects an explicitly empty description */
    const char *desc = json_get_str(tool, "description");
    if (desc && *desc) json_set_str(spec, "description", desc);
    json_t *params = json_get(tool, "parameters");
    json_t *schema;
    if (json_is_obj(params)) schema = json_clone(params);
    else {
        schema = json_obj();
        json_set_str(schema, "type", "object");
        json_set(schema, "properties", json_obj());
    }
    json_t *is = json_obj();
    json_set(is, "json", schema);
    json_set(spec, "inputSchema", is);
    json_t *w = json_obj();
    json_set(w, "toolSpec", spec);
    return w;
}

static json_t *sentinel_tool(void) {
    json_t *t = json_obj();
    json_set_str(t, "name", NO_TOOLS_SENTINEL_NAME);
    json_set_str(t, "description", "Placeholder required by Bedrock validation. Do not call; answer with text.");
    json_t *params = json_obj();
    json_set_str(params, "type", "object");
    json_set(params, "properties", json_obj());
    json_set(t, "parameters", params);
    json_t *spec = tool_spec(t);
    json_free(t);
    return spec;
}

static json_t *choice_obj(const char *kind, const char *name) {
    json_t *c = json_obj();
    if (name) {
        json_t *t = json_obj();
        json_set_str(t, "name", name);
        json_set(c, "tool", t);
    } else {
        json_set(c, kind, json_obj());
    }
    return c;
}

/* mapAnthropicToolChoice: "auto" | "any" | "none" | "tool" (+name); NULL when unset/unknown */
static const char *map_tool_choice(const json_t *tc, const char **name) {
    *name = NULL;
    if (!tc) return NULL;
    if (json_is_str(tc)) {
        const char *s = json_sv(tc);
        if (!strcmp(s, "required")) return "any";
        if (!strcmp(s, "auto") || !strcmp(s, "none") || !strcmp(s, "any")) return s;
        return NULL;
    }
    if (json_is_obj(tc) && str_eq(json_get_str(tc, "type"), "tool")) {
        const char *n = json_get_str(tc, "name");
        if (n && *n) {
            *name = n;
            return "tool";
        }
    }
    return NULL;
}

static json_t *plan_tool_config(const json_t *tools, const json_t *tool_choice, const json_t *msgs, bool *sentinel) {
    *sentinel = false;
    bool has_tools = json_len(tools) > 0;
    const char *tname;
    const char *choice = map_tool_choice(tool_choice, &tname);
    json_t *t;
    if (str_eq(choice, "none")) {
        /* Bedrock requires a toolConfig whenever history carries toolUse/toolResult blocks */
        if (!messages_have_tool_blocks(msgs)) return NULL;
        json_t *cfg = json_obj(), *arr = json_arr();
        if (!has_tools) {
            json_push(arr, sentinel_tool());
            json_set(cfg, "tools", arr);
            json_set(cfg, "toolChoice", choice_obj("auto", NULL));
            *sentinel = true;
            return cfg;
        }
        json_arr_foreach(tools, i, t) json_push(arr, tool_spec(t));
        json_set(cfg, "tools", arr);
        return cfg;
    }
    if (!has_tools) return NULL;
    json_t *cfg = json_obj(), *arr = json_arr();
    json_arr_foreach(tools, i, t) json_push(arr, tool_spec(t));
    json_set(cfg, "tools", arr);
    if (str_eq(choice, "auto")) json_set(cfg, "toolChoice", choice_obj("auto", NULL));
    else if (str_eq(choice, "any")) json_set(cfg, "toolChoice", choice_obj("any", NULL));
    else if (str_eq(choice, "tool")) json_set(cfg, "toolChoice", choice_obj(NULL, tname));
    return cfg;
}

typedef struct {
    ai_effort_t reasoning; /* EFFORT_OFF => no reasoning */
    int64_t max_tokens;    /* <0 => unset */
    bool budget_override;  /* stream.ts shrank the level's budget */
    int64_t budget_value;
} bedrock_opts_t;

/* stream.ts mapOptionsForApi("bedrock-converse-stream") */
static void resolve_options(const ai_model_t *m, const ai_stream_opts_t *o, bedrock_opts_t *r) {
    memset(r, 0, sizeof(*r));
    ai_effort_t e = o->disable_reasoning ? EFFORT_OFF : o->reasoning;
    /* normalizeMandatoryReasoningOptions */
    if (m->reasoning && e == EFFORT_OFF && thinking_flag(m, "requiresEffort") && !thinking_flag(m, "suppressWhenOff"))
        e = minimum_effort(m);
    if (e != EFFORT_OFF && m->reasoning) e = clamp_effort(m, e);
    if ((int)e < EFFORT_OFF || (int)e > EFFORT_MAX) e = EFFORT_OFF;
    r->reasoning = e;
    int64_t base = o->max_tokens > 0 ? o->max_tokens : (m->max_tokens > 0 ? m->max_tokens : -1);
    r->max_tokens = base;
    const char *mode = thinking_mode(m);
    if (str_eq(mode, "anthropic-adaptive")) {
        /* adaptive shares max_tokens between thinking and the answer: add the effort budget on top */
        int64_t budget = 0;
        if (e != EFFORT_OFF) {
            budget = custom_budget(o, e);
            if (budget < 0) budget = BEDROCK_CLAUDE_THINKING[e];
        }
        if (!m->reasoning || base < 0 || budget <= 0) return;
        int64_t un = base + budget;
        r->max_tokens = m->max_tokens > 0 ? MIN(un, m->max_tokens) : un;
        return;
    }
    if (str_eq(mode, "effort")) return;
    if (e == EFFORT_OFF || !m->reasoning) return;
    int64_t budget = custom_budget(o, e);
    if (budget < 0) budget = BEDROCK_CLAUDE_THINKING[e];
    int64_t maxt = base >= 0 ? base : (m->max_tokens > 0 ? m->max_tokens : OUTPUT_CAP_WHEN_UNKNOWN);
    if (maxt <= budget) {
        int64_t desired = budget + MIN_OUTPUT_TOKENS;
        if (m->max_tokens > 0) desired = MIN(desired, m->max_tokens);
        if (desired > maxt) maxt = desired;
        if (maxt <= budget) {
            r->budget_override = true;
            r->budget_value = MAX(0, maxt - MIN_OUTPUT_TOKENS);
        }
    }
    r->max_tokens = maxt;
}

/* buildAdditionalModelRequestFields */
static json_t *additional_fields(const ai_model_t *m, const ai_stream_opts_t *o, const bedrock_opts_t *r) {
    if (!m->reasoning) return NULL;
    const char *display = o->hide_thinking_summary ? "omitted" : NULL;
    if (r->reasoning == EFFORT_OFF) {
        if (str_eq(model_compat_str(m, "disabledThinking"), "disabled")) {
            json_t *f = json_obj(), *t = json_obj(), *oc = json_obj();
            json_set_str(t, "type", "disabled");
            json_set(f, "thinking", t);
            json_set_str(oc, "effort", "low");
            json_set(f, "output_config", oc);
            return f;
        }
        return NULL;
    }
    const char *mode = thinking_mode(m);
    if (str_eq(mode, "anthropic-adaptive")) {
        json_t *f = json_obj(), *t = json_obj(), *oc = json_obj();
        json_set_str(t, "type", "adaptive");
        if (thinking_flag(m, "supportsDisplay")) json_set_str(t, "display", display ? display : "summarized");
        json_set(f, "thinking", t);
        json_set_str(oc, "effort", mapped_effort(m, r->reasoning, true));
        json_set(f, "output_config", oc);
        return f;
    }
    if (str_eq(mode, "effort")) {
        /* OpenAI-schema models on Bedrock take reasoning.effort instead of the Anthropic budget block */
        json_t *f = json_obj(), *re = json_obj();
        json_set_str(re, "effort", mapped_effort(m, r->reasoning, false));
        json_set(f, "reasoning", re);
        return f;
    }
    int64_t budget = r->budget_override ? r->budget_value : custom_budget(o, r->reasoning);
    if (budget < 0) budget = DEFAULT_BEDROCK_THINKING_BUDGETS[r->reasoning];
    json_t *f = json_obj(), *t = json_obj();
    json_set_str(t, "type", "enabled");
    json_set_int(t, "budget_tokens", budget);
    json_set_str(t, "display", display ? display : "summarized");
    json_set(f, "thinking", t);
    return f;
}

static json_t *apply_thinking_binding(json_t *fields, const char *behavior) {
    json_t *f = fields ? fields : json_obj();
    json_t *t = json_get_obj(f, "thinking");
    if (!t) {
        t = json_obj();
        json_set_str(t, "type", "adaptive");
        json_set(f, "thinking", t);
    }
    json_t *bb = json_obj();
    json_set_str(bb, "prefix_mismatch_behavior", behavior);
    json_set(t, "block_binding", bb);
    json_t *betas = json_arr(), *it;
    bool have = false;
    json_arr_foreach(json_get_arr(f, "anthropic_beta"), i, it) {
        if (!json_is_str(it)) continue;
        if (!strcmp(json_sv(it), THINKING_BINDING_CONTROLS_BETA)) have = true;
        json_push_str(betas, json_sv(it));
    }
    if (!have) json_push_str(betas, THINKING_BINDING_CONTROLS_BETA);
    json_set(f, "anthropic_beta", betas);
    return f;
}

/* /^[a-zA-Z0-9\s:_@$#=/+,\-.]*$/ and <= 256 chars */
static bool metadata_value_ok(const char *s) {
    if (strlen(s) > 256) return false;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (strchr(" \t\n\r\f\v:_@$#=/+,-.", c)) continue;
        return false;
    }
    return true;
}

/* invalid / excess entries are dropped (never fail the turn); NULL when nothing remains */
static json_t *sanitize_request_metadata(const json_t *raw) {
    if (!json_is_obj(raw)) return NULL;
    json_t *out = json_obj();
    const char *k;
    json_t *v;
    int kept = 0;
    sb_t dropped = SB_INIT;
    json_obj_foreach(raw, i, k, v) {
        if (!json_is_str(v) || !*k || !metadata_value_ok(k) || !metadata_value_ok(json_sv(v)) ||
            kept >= REQUEST_METADATA_MAX_ENTRIES) {
            sb_appendf(&dropped, "%s%s", dropped.len ? ", " : "", k);
            continue;
        }
        json_set_str(out, k, json_sv(v));
        kept++;
    }
    if (dropped.len) LOGW("Bedrock requestMetadata entries dropped: %s", dropped.s);
    sb_free(&dropped);
    if (!kept) {
        json_free(out);
        return NULL;
    }
    return out;
}

void bedrock_request_free(bedrock_request_t *r) {
    json_free(r->body);
    free(r->error);
    memset(r, 0, sizeof(*r));
}

void bedrock_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                           bedrock_request_t *out) {
    memset(out, 0, sizeof(*out));
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    bedrock_opts_t r;
    resolve_options(m, o, &r);
    cache_policy_t cp = prompt_cache_policy(m, cache_retention(o));
    char *err = NULL;
    json_t *msgs = convert_messages(m, ctx ? ctx->messages : NULL, &cp, &err);
    if (err) {
        json_free(msgs);
        out->error = err;
        return;
    }
    json_t *tool_config = plan_tool_config(ctx ? ctx->tools : NULL, o->tool_choice, msgs, &out->sentinel_injected);
    json_t *fields = additional_fields(m, o, &r);
    json_t *ft = json_get_obj(fields, "thinking");
    bool thinking_disabled = ft && str_eq(json_get_str(ft, "type"), "disabled");
    const char *prefix_behavior = !thinking_disabled && thinking_flag(m, "prefixBinding") &&
                                          compat_flag(m, "supportsThinkingBindingControls", false)
                                      ? "drop_block"
                                      : NULL;
    json_t *tc = json_get_obj(tool_config, "toolChoice");
    bool forced = tc && (json_get(tc, "any") || json_get(tc, "tool"));
    if (forced && !compat_flag(m, "supportsForcedToolChoice", true)) {
        /* some models (Opus/Sonnet 5.5) reject forced tool use: keep tools offered under auto */
        json_set(tool_config, "toolChoice", choice_obj("auto", NULL));
    } else if (forced && !thinking_disabled) {
        /* Bedrock rejects thinking + forced tool_choice; prefix-bound lines think regardless */
        if (thinking_flag(m, "prefixBinding")) json_set(tool_config, "toolChoice", choice_obj("auto", NULL));
        else {
            json_free(fields);
            fields = NULL;
        }
    }
    if (prefix_behavior) fields = apply_thinking_binding(fields, prefix_behavior);

    json_t *body = json_obj();
    json_set(body, "messages", msgs);
    json_t *sys = build_system(ctx, &cp);
    if (sys) json_set(body, "system", sys);
    json_t *ic = json_obj();
    if (r.max_tokens > 0) json_set_int(ic, "maxTokens", r.max_tokens);
    json_t *sp = model_compat_get(m, "supportsSamplingParams");
    bool sampling = !(json_is_bool(sp) && !sp->u.b);
    if (sampling && !isnan(o->temperature)) json_set_num(ic, "temperature", o->temperature);
    if (sampling && !isnan(o->top_p)) json_set_num(ic, "topP", o->top_p);
    json_set(body, "inferenceConfig", ic);
    if (tool_config) json_set(body, "toolConfig", tool_config);
    const char *gid = json_get_str(m->raw, "guardrailIdentifier");
    if (gid && *gid) {
        json_t *g = json_obj();
        json_set_str(g, "guardrailIdentifier", gid);
        const char *gv = json_get_str(m->raw, "guardrailVersion");
        json_set_str(g, "guardrailVersion", gv && *gv ? gv : "DRAFT");
        const char *gt = json_get_str(m->raw, "guardrailTrace");
        if (gt) json_set_str(g, "trace", gt);
        json_set(body, "guardrailConfig", g);
    }
    if (fields) json_set(body, "additionalModelRequestFields", fields);
    json_t *md = sanitize_request_metadata(json_get(m->raw, "requestMetadata"));
    if (md) json_set(body, "requestMetadata", md);
    if (prefix_behavior) {
        json_t *paths = json_arr();
        json_push_str(paths, "/input_transformations");
        json_set(body, "additionalModelResponseFieldPaths", paths);
    }
    out->body = body;
}

/* ======================================================================
 * stream state machine
 * ====================================================================== */

static const struct {
    const char *code;
    int status;
} EXC_STATUS[] = {
    {"accessdeniedexception", 403},       {"conflictexception", 400},         {"internalserverexception", 500},
    {"modelerrorexception", 424},         {"modelnotreadyexception", 429},    {"modelstreamerrorexception", 424},
    {"modeltimeoutexception", 408},       {"resourcenotfoundexception", 404}, {"servicequotaexceededexception", 400},
    {"serviceunavailableexception", 503}, {"throttlingexception", 429},       {"validationexception", 400},
};

int bedrock_exception_status(const char *code) {
    if (!code) return 400;
    char *t = str_trim(code);
    int st = 400;
    for (size_t i = 0; i < ARRAY_LEN(EXC_STATUS); i++)
        if (!strcasecmp(t, EXC_STATUS[i].code)) st = EXC_STATUS[i].status;
    free(t);
    return st;
}

const char *bedrock_map_stop_reason(const char *r) {
    if (!r) return "error";
    if (!strcmp(r, "end_turn") || !strcmp(r, "stop_sequence")) return "stop";
    if (!strcmp(r, "max_tokens") || !strcmp(r, "model_context_window_exceeded")) return "length";
    if (!strcmp(r, "tool_use")) return "toolUse";
    return "error";
}

static bool word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static const char *skip_ws1(const char *q) {
    const char *s = q;
    while (*q && strchr(" \t\n\r\f\v", *q)) q++;
    return q == s ? NULL : q;
}

static const char *expect_word(const char *q, const char *w) {
    size_t n = strlen(w);
    return q && !strncmp(q, w, n) ? q + n : NULL;
}

/* /invalid\s+`?signature`?\s+in\s+`?thinking`?/ on a lowercased message */
static bool match_invalid_signature(const char *l) {
    for (const char *p = strstr(l, "invalid"); p; p = strstr(p + 1, "invalid")) {
        const char *q = skip_ws1(p + 7);
        if (q && *q == '`') q++;
        q = expect_word(q, "signature");
        if (q && *q == '`') q++;
        q = q ? skip_ws1(q) : NULL;
        q = expect_word(q, "in");
        q = q ? skip_ws1(q) : NULL;
        if (q && *q == '`') q++;
        if (expect_word(q, "thinking")) return true;
    }
    return false;
}

bool bedrock_is_prefix_binding_error(const char *msg) {
    if (!msg) return false;
    char *l = str_lower(msg);
    bool compaction = false;
    for (const char *p = strstr(l, "compaction_"); p; p = strstr(p + 1, "compaction_")) {
        char nx = p[11];
        if ((p == l || !word_char(p[-1])) && ((nx >= 'a' && nx <= 'z') || nx == '_')) compaction = true;
    }
    bool ok = !compaction && match_invalid_signature(l) &&
              (strstr(l, "bound to a different conversation") || strstr(l, "prefix_mismatch_behavior"));
    free(l);
    return ok;
}

static int on_frame(void *ud, const aws_es_msg_t *m);

void bedrock_stream_init(bedrock_stream_t *s, const ai_model_t *m, bool sentinel_injected, ai_event_cb cb, void *ud) {
    memset(s, 0, sizeof(*s));
    aib_init(&s->b, m, cb, ud);
    json_set_str(s->b.msg, "api", "bedrock-converse-stream");
    s->model = m;
    s->sentinel_injected = sentinel_injected;
    s->stop_reason = "stop";
    aws_es_init(&s->dec, on_frame, s);
}

void bedrock_stream_free(bedrock_stream_t *s) {
    for (size_t i = 0; i < s->blocks.n; i++) sb_free(&s->blocks.v[i].sig);
    vec_free(&s->blocks);
    aws_es_free(&s->dec);
    free(s->error);
    s->error = NULL;
    json_free(s->input_transformations);
    s->input_transformations = NULL;
    aib_free(&s->b);
}

static void set_error(bedrock_stream_t *s, char *msg) {
    if (s->error) free(msg);
    else s->error = msg;
}

static bedrock_block_t *find_block(bedrock_stream_t *s, int64_t wire) {
    for (size_t i = 0; i < s->blocks.n; i++)
        if (s->blocks.v[i].wire == wire) return &s->blocks.v[i]; /* first registered for an index wins */
    return NULL;
}

static bedrock_block_t *add_block(bedrock_stream_t *s, int64_t wire, int kind) {
    if (s->blocks.n >= MAX_STREAM_BLOCKS) return NULL;
    bedrock_block_t b = {.wire = wire, .ci = s->b.cur_index, .kind = kind, .closed = false, .sig = SB_INIT};
    vec_push(&s->blocks, b);
    return &vec_last(&s->blocks);
}

static json_t *content_arr(bedrock_stream_t *s) { return json_get(s->b.msg, "content"); }

static bool is_current(bedrock_stream_t *s, const bedrock_block_t *b) {
    return !b->closed && b->ci >= 0 && b->ci == s->b.cur_index;
}

static void on_block_start(bedrock_stream_t *s, const json_t *ev) {
    int64_t idx = json_get_int(ev, "contentBlockIndex", -1);
    json_t *tu = json_get_obj(json_get_obj(ev, "start"), "toolUse");
    if (!tu) return;
    const char *name = json_get_str(tu, "name");
    /* drop the sentinel call only when we injected it ourselves */
    if (s->sentinel_injected && str_eq(name, NO_TOOLS_SENTINEL_NAME)) return;
    if (find_block(s, idx) || s->blocks.n >= MAX_STREAM_BLOCKS) return;
    char *id = normalize_tool_id(json_get_str(tu, "toolUseId"));
    aib_toolcall_start(&s->b, id, name ? name : "");
    /* normalizeToolCallId("") stays empty, unlike the builder's generated fallback */
    if (!id[0]) json_set_str(json_at(content_arr(s), (size_t)s->b.cur_index), "id", "");
    free(id);
    add_block(s, idx, 3);
}

static void on_block_delta(bedrock_stream_t *s, const json_t *ev) {
    int64_t idx = json_get_int(ev, "contentBlockIndex", -1);
    json_t *d = json_get_obj(ev, "delta");
    if (!d) return;
    bedrock_block_t *blk = find_block(s, idx);
    json_t *text = json_get(d, "text");
    if (text) {
        /* text blocks get no contentBlockStart: the first delta opens them */
        if (!json_is_str(text)) return;
        if (!blk) {
            if (!text->u.str.len || s->blocks.n >= MAX_STREAM_BLOCKS) return;
            aib_text_delta(&s->b, text->u.str.s, text->u.str.len);
            add_block(s, idx, 1);
        } else if (blk->kind == 1 && is_current(s, blk)) {
            aib_text_delta(&s->b, text->u.str.s, text->u.str.len);
        }
        return;
    }
    json_t *tu = json_get_obj(d, "toolUse");
    if (tu) {
        if (blk && blk->kind == 3 && is_current(s, blk)) {
            json_t *in = json_get(tu, "input");
            if (json_is_str(in) && in->u.str.len) aib_toolcall_delta(&s->b, in->u.str.s, in->u.str.len);
        }
        return;
    }
    json_t *rc = json_get_obj(d, "reasoningContent");
    if (!rc) return;
    json_t *rt = json_get(rc, "text");
    const char *sig = json_get_str(rc, "signature");
    if (!blk) {
        if (s->blocks.n >= MAX_STREAM_BLOCKS) return;
        if (json_is_str(rt) && rt->u.str.len) {
            aib_thinking_delta(&s->b, rt->u.str.s, rt->u.str.len);
        } else {
            /* open an empty thinking block (emits thinking_start, no delta) */
            aib_start(&s->b);
            aib_end_block(&s->b);
            json_t *tb = json_obj();
            json_set_str(tb, "type", "thinking");
            json_set_str(tb, "thinking", "");
            json_push(content_arr(s), tb);
            s->b.cur_index = (int)json_len(content_arr(s)) - 1;
            s->b.cur_kind = 2;
            sb_clear(&s->b.buf);
            if (s->b.cb) {
                ai_event_t e = {.type = AI_EV_THINKING_START, .content_index = s->b.cur_index, .partial = s->b.msg};
                s->b.cb(s->b.ud, &e);
            }
        }
        json_set_str(json_at(content_arr(s), (size_t)s->b.cur_index), "thinkingSignature", "");
        blk = add_block(s, idx, 2);
        if (blk && sig && *sig) sb_append(&blk->sig, sig);
        return;
    }
    if (blk->kind != 2) return;
    if (json_is_str(rt) && rt->u.str.len && is_current(s, blk)) aib_thinking_delta(&s->b, rt->u.str.s, rt->u.str.len);
    if (sig && *sig) sb_append(&blk->sig, sig);
}

static void flush_signature(bedrock_stream_t *s, bedrock_block_t *blk) {
    if (blk->kind == 2 && blk->ci >= 0 && blk->sig.len)
        json_set(json_at(content_arr(s), (size_t)blk->ci), "thinkingSignature", json_strn(blk->sig.s, blk->sig.len));
}

static void on_block_stop(bedrock_stream_t *s, const json_t *ev) {
    bedrock_block_t *blk = find_block(s, json_get_int(ev, "contentBlockIndex", -1));
    if (!blk || blk->closed) return;
    flush_signature(s, blk);
    if (is_current(s, blk)) aib_end_block(&s->b);
    blk->closed = true;
}

static void on_message_stop(bedrock_stream_t *s, const json_t *ev) {
    const char *raw = json_get_str(ev, "stopReason");
    json_t *fields = json_get_obj(ev, "additionalModelResponseFields");
    json_t *tr = fields ? json_get_arr(fields, "input_transformations") : NULL;
    if (tr) {
        json_t *keep = json_arr(), *it;
        json_arr_foreach(tr, i, it) {
            if (json_is_obj(it) && json_get_str(it, "type")) json_push(keep, json_clone(it));
        }
        if (json_len(keep)) {
            json_free(s->input_transformations);
            s->input_transformations = keep;
            json_arr_foreach(keep, i, it) {
                if (str_eq(json_get_str(it, "reason"), "prefix_binding_mismatch"))
                    LOGW("bedrock: %s dropped thinking at %s (prefix binding mismatch)", s->model->id,
                         json_get_str(it, "path") ? json_get_str(it, "path") : "?");
            }
        } else {
            json_free(keep);
        }
    }
    /* a sentinel-only request must never surface a tool-use stop */
    s->stop_reason = s->sentinel_injected && str_eq(raw, "tool_use") ? "stop" : bedrock_map_stop_reason(raw);
    if (!strcmp(s->stop_reason, "error")) {
        char *msg;
        if (str_eq(raw, "guardrail_intervened"))
            msg = xasprintf("Response blocked by Amazon Bedrock guardrail (stop reason: %s).", raw);
        else if (str_eq(raw, "content_filtered"))
            msg = xasprintf("Response filtered by Amazon Bedrock content filters (stop reason: %s).", raw);
        else msg = xasprintf("Generation failed with stop reason: %s", raw ? raw : "unknown");
        set_error(s, msg);
    }
}

static void on_metadata(bedrock_stream_t *s, const json_t *ev) {
    json_t *u = json_get_obj(ev, "usage");
    if (!u) return;
    aib_usage(&s->b, json_get_int(u, "inputTokens", 0), json_get_int(u, "outputTokens", 0),
              json_get_int(u, "cacheReadInputTokens", 0), json_get_int(u, "cacheWriteInputTokens", 0));
}

static char *payload_text(const aws_es_msg_t *m) { return xstrndup((const char *)m->payload, m->payload_len); }

static int on_frame(void *ud, const aws_es_msg_t *m) {
    bedrock_stream_t *s = ud;
    const char *mt = http_headers_get(&m->headers, ":message-type");
    if (str_eq(mt, "exception")) {
        const char *code = http_headers_get(&m->headers, ":exception-type");
        if (!code || !*code) code = "Exception";
        json_t *j = m->payload_len ? json_parse((const char *)m->payload, m->payload_len, NULL) : NULL;
        const char *jm = json_get_str(j, "message");
        char *msg = jm && *jm ? xstrdup(jm) : payload_text(m);
        set_error(s, xasprintf("%s: %s", code, msg));
        LOGD("bedrock: in-stream exception %s (status %d)", code, bedrock_exception_status(code));
        free(msg);
        json_free(j);
        return 2;
    }
    if (str_eq(mt, "error")) {
        const char *code = http_headers_get(&m->headers, ":error-code");
        if (!code || !*code) code = "UnknownError";
        const char *em = http_headers_get(&m->headers, ":error-message");
        char *msg = em && *em ? xstrdup(em) : payload_text(m);
        set_error(s, xasprintf("%s: %s", code, msg));
        free(msg);
        return 2;
    }
    if (!str_eq(mt, "event")) return 0;
    json_t *ev = m->payload_len ? json_parse((const char *)m->payload, m->payload_len, NULL) : json_obj();
    if (!ev) return 0;
    const char *et = http_headers_get(&m->headers, ":event-type");
    int rc = 0;
    if (str_eq(et, "messageStart")) {
        if (!str_eq(json_get_str(ev, "role"), "assistant")) {
            set_error(s, xstrdup("Unexpected assistant message start but got user message start instead"));
            rc = 2;
        } else {
            aib_start(&s->b);
        }
    } else if (str_eq(et, "contentBlockStart")) {
        on_block_start(s, ev);
    } else if (str_eq(et, "contentBlockDelta")) {
        on_block_delta(s, ev);
    } else if (str_eq(et, "contentBlockStop")) {
        on_block_stop(s, ev);
    } else if (str_eq(et, "messageStop")) {
        on_message_stop(s, ev);
    } else if (str_eq(et, "metadata")) {
        on_metadata(s, ev);
    }
    json_free(ev);
    return rc;
}

int bedrock_stream_feed(void *ud, const char *data, size_t len) {
    bedrock_stream_t *s = ud;
    int rc = aws_es_feed(&s->dec, data, len);
    if (rc == -1) {
        set_error(s, xstrdup(s->dec.error ? s->dec.error : "event stream framing error"));
        return 2;
    }
    return rc;
}

json_t *bedrock_stream_finish(bedrock_stream_t *s, int rc, const char *err, bool cancelled) {
    for (size_t i = 0; i < s->blocks.n; i++)
        if (!s->blocks.v[i].closed) flush_signature(s, &s->blocks.v[i]);
    if (!cancelled && !s->error && rc == 0 && aws_es_finish(&s->dec) != 0)
        set_error(s, xstrdup(s->dec.error ? s->dec.error : "truncated message at end of stream"));
    if (s->input_transformations) {
        aib_set(&s->b, "inputTransformations", s->input_transformations);
        s->input_transformations = NULL;
    }
    if (cancelled || rc == -2) return aib_finish(&s->b, "aborted", err ? err : "Request was aborted");
    if (s->error) return aib_finish(&s->b, "error", s->error);
    if (rc != 0) return aib_finish(&s->b, "error", err ? err : "request failed");
    return aib_finish(&s->b, s->stop_reason, NULL);
}

/* ======================================================================
 * entry point
 * ====================================================================== */

static bool signer_owned(const char *lk) {
    return !strcmp(lk, "host") || !strcmp(lk, "x-amz-date") || !strcmp(lk, "x-amz-content-sha256") ||
           !strcmp(lk, "x-amz-security-token");
}
static bool reserved(const char *lk) {
    return !strcmp(lk, "content-type") || !strcmp(lk, "accept") || !strcmp(lk, "authorization") ||
           !strcmp(lk, "content-length");
}

/* model headers then option headers (lowercased); signer-owned and reserved names dropped so the
 * signed set always matches the wire */
static void caller_headers(const ai_model_t *m, const ai_stream_opts_t *o, http_headers_t *h) {
    const json_t *srcs[2] = {m->headers, o->headers};
    for (int k = 0; k < 2; k++) {
        const char *name;
        json_t *v;
        if (!json_is_obj(srcs[k])) continue;
        json_obj_foreach(srcs[k], i, name, v) {
            if (!json_is_str(v)) continue;
            char *lk = str_lower(name);
            if (!signer_owned(lk) && !reserved(lk)) http_headers_set(h, lk, json_sv(v));
            free(lk);
        }
    }
    if (!http_headers_get(h, "user-agent")) http_headers_set(h, "user-agent", "omc/" OMC_VERSION);
}

static void drop_reasoning_content(json_t *body) {
    json_t *msgs = json_get_arr(body, "messages");
    for (size_t i = json_len(msgs); i > 0; i--) {
        json_t *msg = json_at(msgs, i - 1);
        if (!str_eq(json_get_str(msg, "role"), "assistant")) continue;
        json_t *c = json_get_arr(msg, "content");
        for (size_t j = json_len(c); j > 0; j--)
            if (json_get(json_at(c, j - 1), "reasoningContent")) json_free(json_arr_remove(c, j - 1));
        if (json_len(c) == 0) json_free(json_arr_remove(msgs, i - 1));
    }
}

static json_t *fail(const ai_model_t *m, ai_event_cb cb, void *ud, const char *msg) {
    ai_builder_t b;
    aib_init(&b, m, cb, ud);
    json_set_str(b.msg, "api", "bedrock-converse-stream");
    json_t *r = aib_finish(&b, "error", msg);
    aib_free(&b);
    return r;
}

json_t *provider_bedrock_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                                ai_event_cb cb, void *ud) {
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    char *region = bedrock_resolve_region(m, json_get_str(m->raw, "guardrailIdentifier"));
    bedrock_target_t tgt;
    if (resolve_target(m, region, &tgt) != 0) {
        char *e = xasprintf("Invalid Bedrock base URL: %s", m->base_url ? m->base_url : "");
        json_t *r = fail(m, cb, ud, e);
        free(e);
        free(region);
        return r;
    }
    bedrock_request_t req;
    bedrock_build_request(m, ctx, o, &req);
    if (req.error) {
        json_t *r = fail(m, cb, ud, req.error);
        bedrock_request_free(&req);
        target_free(&tgt);
        free(region);
        return r;
    }
    if (o->on_payload) o->on_payload(o->payload_ud, req.body);
    const char *bearer = aws_bearer_token(o->api_key);
    json_t *result = NULL;
    bool prefix_retry_done = false;
    for (;;) {
        char *body = json_dump(req.body, 0);
        http_headers_t base = {0};
        caller_headers(m, o, &base);
        http_headers_set(&base, "content-type", "application/json");
        http_headers_set(&base, "accept", "application/vnd.amazon.eventstream");
        http_headers_t h = {0};
        for (size_t i = 0; i < base.n; i++) http_headers_add(&h, base.v[i].name, base.v[i].value);
        if (bearer) {
            char *a = xasprintf("Bearer %s", bearer);
            http_headers_set(&h, "authorization", a);
            free(a);
        } else {
            aws_creds_t creds = {0};
            char *cerr = NULL;
            if (env_truthy("AWS_BEDROCK_SKIP_AUTH")) {
                creds.access_key_id = xstrdup("dummy-access-key");
                creds.secret_access_key = xstrdup("dummy-secret-key");
            } else if (aws_resolve_credentials(NULL, region, o->cancel, &creds, &cerr) != 0) {
                result = fail(m, cb, ud, cerr ? cerr : "Unable to resolve AWS credentials.");
                free(cerr);
                free(body);
                http_headers_free(&base);
                http_headers_free(&h);
                break;
            }
            aws_sign_params_t sp = {.method = "POST",
                                    .host = tgt.host,
                                    .path = tgt.path,
                                    .query = tgt.query,
                                    .body = body,
                                    .body_len = strlen(body),
                                    .region = region,
                                    .service = "bedrock",
                                    .creds = &creds,
                                    .headers = &base};
            aws_sigv4_sign(&sp, &h);
            aws_creds_free(&creds);
        }
        http_headers_free(&base);
        bedrock_stream_t s;
        bedrock_stream_init(&s, m, req.sentinel_injected, cb, ud);
        int status = 0;
        char *err = NULL;
        ai_post_ex_t ex = {0};
        int rc = ai_stream_post_ex(tgt.url, &h, body, o, bedrock_stream_feed, &s, &status, &err, &ex);
        http_headers_free(&h);
        free(body);
        bool cancelled = o->cancel && *o->cancel;
        if (rc == -1 && status > 0 && (status < 200 || status >= 300)) {
            /* stale cached credentials (rotated session keys): re-resolve next time */
            if (!bearer && (status == 401 || status == 403)) aws_invalidate_credentials(NULL, region);
            char *text = xasprintf("Bedrock HTTP %d: %.1000s", status, ex.err_body ? ex.err_body : "");
            if (!prefix_retry_done && bedrock_is_prefix_binding_error(text)) {
                prefix_retry_done = true;
                LOGW("bedrock: thinking prefix changed, stripping bound thinking and retrying (%s)", m->id);
                drop_reasoning_content(req.body);
                free(text);
                free(err);
                free(ex.err_body);
                bedrock_stream_free(&s);
                continue;
            }
            char *msg = ai_rewrap_http_error(text, err);
            free(text);
            free(err);
            err = msg;
        }
        free(ex.err_body);
        result = bedrock_stream_finish(&s, rc, err, cancelled);
        free(err);
        bedrock_stream_free(&s);
        break;
    }
    bedrock_request_free(&req);
    target_free(&tgt);
    free(region);
    return result;
}
