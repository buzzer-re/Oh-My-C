/* dialect.c - model transcript dialects for summary input (see dialect.h). */
#include "ai/dialect.h"

#include <ctype.h>
#include <math.h>

#define TOOL_RESULT_MAX_UNITS 2000

/* ---------- model class / dialect choice ---------- */

const char *dialect_for_class(const char *c) {
    if (!c) return "xml";
    if (!strcmp(c, "anthropic")) return "anthropic";
    if (!strcmp(c, "glm")) return "glm";
    if (!strcmp(c, "gemini")) return "gemini";
    if (!strcmp(c, "gemma")) return "gemma";
    if (!strcmp(c, "kimi")) return "kimi";
    if (!strcmp(c, "qwen")) return "qwen3";
    if (!strcmp(c, "deepseek")) return "deepseek";
    if (!strcmp(c, "minimax")) return "minimax";
    if (!strcmp(c, "openai") || !strcmp(c, "gpt-oss")) return "harmony";
    return "xml"; /* FALLBACK_DIALECT */
}

/* classifyModel("", id, {lenient}) reduced to the class tokens of the taxonomy rules */
static const char *classify_id(const char *id) {
    if (!id) return "unknown";
    const char *slash = strrchr(id, '/');
    char *low = str_lower(slash ? slash + 1 : id);
    const char *c = "unknown";
    if (strstr(low, "gpt-oss")) c = "gpt-oss";
    else if (strstr(low, "claude")) c = "anthropic";
    else if (strstr(low, "deepseek")) c = "deepseek";
    else if (strstr(low, "gemini")) c = "gemini";
    else if (strstr(low, "gemma")) c = "gemma";
    else if (strstr(low, "glm")) c = "glm";
    else if (strstr(low, "kimi")) c = "kimi";
    else if (strstr(low, "minimax")) c = "minimax";
    else if (strstr(low, "qwen") || strstr(low, "qwq")) c = "qwen";
    else if (str_starts(low, "gpt-") || str_starts(low, "chatgpt-") || str_starts(low, "codex-") || str_starts(low, "o1-") ||
             str_starts(low, "o1.") || str_starts(low, "o3-") || str_starts(low, "o3.") || str_starts(low, "o4-") ||
             !strcmp(low, "o1") || !strcmp(low, "o3") || !strcmp(low, "o4"))
        c = "openai";
    free(low);
    return c;
}

const char *dialect_model_class(const ai_model_t *m, const char *model_id) {
    const json_t *identity = m && m->raw ? json_get_obj(m->raw, "identity") : NULL;
    const char *c = identity ? json_get_str(identity, "class") : NULL;
    if (c && *c && strcmp(c, "unknown")) return c;
    return classify_id(model_id ? model_id : m ? m->id : NULL);
}

const char *dialect_for_model(const ai_model_t *m) { return dialect_for_class(dialect_model_class(m, m ? m->id : NULL)); }

bool dialect_drops_thinking(const ai_model_t *m) { return str_eq(dialect_model_class(m, m ? m->id : NULL), "anthropic"); }

/* ---------- shared rendering helpers (dialect/rendering.ts) ---------- */

static void js_json(sb_t *b, const json_t *v) {
    if (!v) sb_append(b, "null");
    else json_dump_js_sb(b, v);
}

/* messageContentText */
static void content_text(const json_t *content, sb_t *b) {
    if (json_is_str(content)) {
        sb_append(b, json_sv(content));
        return;
    }
    const json_t *blk;
    json_arr_foreach(content, i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "text") && json_get_str(blk, "text")) sb_append(b, json_get_str(blk, "text"));
        else if (str_eq(t, "image")) {
            const char *mime = json_get_str(blk, "mimeType");
            if (mime) sb_appendf(b, "[Image: %s]", mime);
            else sb_append(b, "[Image]");
        }
    }
}

static char *content_text_dup(const json_t *content) {
    sb_t b = SB_INIT;
    content_text(content, &b);
    return sb_detach(&b);
}

typedef struct {
    char *text, *thinking;
    VEC(const json_t *) calls;
} parts_t;

/* assistantTranscriptParts */
static void assistant_parts(const json_t *msg, parts_t *p) {
    memset(p, 0, sizeof *p);
    sb_t text = SB_INIT, think = SB_INIT;
    bool any_think = false;
    const json_t *blk;
    json_arr_foreach(json_get_arr(msg, "content"), i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "text")) sb_append(&text, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
        else if (str_eq(t, "thinking")) {
            if (any_think) sb_putc(&think, '\n');
            sb_append(&think, json_get_str(blk, "thinking") ? json_get_str(blk, "thinking") : "");
            any_think = true;
        } else if (str_eq(t, "toolCall")) vec_push(&p->calls, blk);
    }
    p->text = sb_detach(&text);
    p->thinking = sb_detach(&think);
}

static void parts_free(parts_t *p) {
    free(p->text);
    free(p->thinking);
    vec_free(&p->calls);
}

typedef struct {
    const char *id, *name;
    size_t index;
    char *text;
    bool is_error;
} result_t;

typedef VEC(result_t) results_t;

/* collectToolResultRun: consecutive toolResult messages from i; returns the next index */
static size_t result_run(const json_t *msgs, size_t i, results_t *out) {
    size_t n = json_len(msgs);
    while (i < n && str_eq(json_get_str(json_at(msgs, i), "role"), "toolResult")) {
        const json_t *m = json_at(msgs, i);
        result_t r = {.id = json_get_str(m, "toolCallId") ? json_get_str(m, "toolCallId") : "",
                      .name = json_get_str(m, "toolName") ? json_get_str(m, "toolName") : "",
                      .index = out->n,
                      .text = content_text_dup(json_get(m, "content")),
                      .is_error = json_get_bool(m, "isError", false)};
        vec_push(out, r);
        i++;
    }
    return i;
}

static void results_free(results_t *r) {
    for (size_t i = 0; i < r->n; i++) free(r->v[i].text);
    vec_free(r);
}

static void xml_attr(sb_t *b, const char *s) {
    for (; *s; s++) {
        if (*s == '&') sb_append(b, "&amp;");
        else if (*s == '"') sb_append(b, "&quot;");
        else if (*s == '<') sb_append(b, "&lt;");
        else if (*s == '>') sb_append(b, "&gt;");
        else sb_putc(b, *s);
    }
}

static void xml_text(sb_t *b, const char *s) {
    for (; *s; s++) {
        if (*s == '&') sb_append(b, "&amp;");
        else if (*s == '<') sb_append(b, "&lt;");
        else if (*s == '>') sb_append(b, "&gt;");
        else sb_putc(b, *s);
    }
}

static const char *call_name(const json_t *call) { return json_get_str(call, "name") ? json_get_str(call, "name") : ""; }
static const char *call_id(const json_t *call) { return json_get_str(call, "id") ? json_get_str(call, "id") : ""; }

/* renderInvoke without declared argument shapes: every value as JSON */
static void render_invoke(sb_t *b, const json_t *call) {
    sb_append(b, "<invoke name=\"");
    xml_attr(b, call_name(call));
    sb_append(b, "\">");
    const char *key;
    const json_t *v;
    json_obj_foreach(json_get_obj(call, "arguments"), i, key, v) {
        sb_append(b, "<parameter name=\"");
        xml_attr(b, key);
        sb_append(b, "\">");
        js_json(b, v);
        sb_append(b, "</parameter>");
    }
    sb_append(b, "</invoke>");
}

static void render_invokes(sb_t *b, const parts_t *p) {
    for (size_t i = 0; i < p->calls.n; i++) {
        if (i) sb_putc(b, '\n');
        render_invoke(b, p->calls.v[i]);
    }
}

static bool ascii_ws(char c) { return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32; }

/* findDelimitedThinkingClose: index of the matching close in [start, end), or -1 */
static ssize_t find_close(const char *open, const char *close, const char *t, size_t start, size_t end) {
    size_t lo = strlen(open), lc = strlen(close);
    int depth = 1;
    size_t cur = start;
    while (cur < end) {
        const char *nc = strstr(t + cur, close);
        if (!nc || (size_t)(nc - t) >= end) return -1;
        const char *no = strstr(t + cur, open);
        if (no && no < nc) {
            depth++;
            cur = (size_t)(no - t) + lo;
            continue;
        }
        depth--;
        if (depth == 0) return nc - t;
        cur = (size_t)(nc - t) + lc;
    }
    return -1;
}

/* unwrapDelimitedThinking (owned) */
static char *unwrap_thinking(const char *open, const char *close, const char *text, int depth) {
    size_t n = strlen(text), lo = strlen(open), lc = strlen(close);
    size_t end = n;
    while (end > 0 && ascii_ws(text[end - 1])) end--;
    size_t cur = 0;
    while (cur < end && ascii_ws(text[cur])) cur++;
    if (depth > 32 || cur >= end || strncmp(text + cur, open, lo)) return xstrdup(text);
    strvec_t segs = {0};
    while (cur < end) {
        if (strncmp(text + cur, open, lo)) {
            strvec_free(&segs);
            return xstrdup(text);
        }
        size_t is = cur + lo;
        ssize_t ie = find_close(open, close, text, is, end);
        if (ie < 0) {
            strvec_free(&segs);
            return xstrdup(text);
        }
        size_t te = (size_t)ie;
        while (te > is && ascii_ws(text[te - 1])) te--;
        size_t ts = is;
        while (ts < te && ascii_ws(text[ts])) ts++;
        char *inner = xstrndup(text + ts, te - ts);
        strvec_push_own(&segs, unwrap_thinking(open, close, inner, depth + 1));
        free(inner);
        cur = (size_t)ie + lc;
        while (cur < end && ascii_ws(text[cur])) cur++;
    }
    char *out = strvec_join(&segs, "\n");
    strvec_free(&segs);
    return out;
}

/* renderDelimitedThinking */
static void delimited_thinking(sb_t *b, const char *open, const char *close, const char *text) {
    if (!*text) return;
    char *u = unwrap_thinking(open, close, text, 0);
    sb_appendf(b, "%s\n%s\n%s", open, u, close);
    free(u);
}

static void simple_thinking(sb_t *b, const char *open, const char *close, const char *text) {
    if (*text) sb_appendf(b, "%s\n%s\n%s", open, text, close);
}

/* renderToolResponseResults */
static void tool_response_results(sb_t *b, const results_t *r) {
    for (size_t i = 0; i < r->n; i++) sb_appendf(b, "%s<tool_response>\n%s\n</tool_response>", i ? "\n" : "", r->v[i].text);
}

/* `<function_results>` (anthropic, minimax) */
static void function_results(sb_t *b, const results_t *r) {
    sb_append(b, "<function_results>\n");
    for (size_t i = 0; i < r->n; i++) {
        const char *tag = r->v[i].is_error ? "error" : "result", *st = r->v[i].is_error ? "stderr" : "stdout";
        if (i) sb_putc(b, '\n');
        sb_appendf(b, "<%s>\n<tool_name>", tag);
        xml_text(b, r->v[i].name);
        sb_appendf(b, "</tool_name>\n<%s>%s</%s>\n</%s>", st, r->v[i].text, st, tag);
    }
    sb_append(b, "\n</function_results>");
}

static void json_obj2(sb_t *b, const json_t *call) {
    /* stringifyJson({ name, arguments }) */
    sb_append(b, "{\"name\":");
    json_js_quote_sb(b, call_name(call), strlen(call_name(call)));
    sb_append(b, ",\"arguments\":");
    js_json(b, json_get(call, "arguments"));
    sb_putc(b, '}');
}

/* ---------- legacy text transcripts: xml, anthropic, minimax ---------- */

typedef enum { D_XML, D_ANTHROPIC, D_MINIMAX } legacy_t;

static void legacy_calls(sb_t *b, legacy_t d, const parts_t *p) {
    if (d == D_XML) {
        render_invokes(b, p);
        return;
    }
    if (!p->calls.n) return;
    sb_append(b, d == D_ANTHROPIC ? "<function_calls>\n" : "<minimax:tool_call>\n");
    render_invokes(b, p);
    sb_append(b, d == D_ANTHROPIC ? "\n</function_calls>" : "\n</minimax:tool_call>");
}

static char *render_legacy(const json_t *msgs, legacy_t d) {
    sb_t out = SB_INIT;
    size_t n = json_len(msgs);
    for (size_t i = 0; i < n;) {
        const json_t *m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            parts_t p;
            assistant_parts(m, &p);
            sb_append(&out, "\n\nAssistant: ");
            delimited_thinking(&out, "<thinking>", "</thinking>", p.thinking);
            sb_append(&out, p.text);
            legacy_calls(&out, d, &p);
            parts_free(&p);
            i++;
            continue;
        }
        if (str_eq(role, "toolResult")) {
            results_t r = {0};
            i = result_run(msgs, i, &r);
            sb_append(&out, "\n\nHuman: ");
            if (d == D_XML) tool_response_results(&out, &r);
            else function_results(&out, &r);
            results_free(&r);
            continue;
        }
        char *text = content_text_dup(json_get(m, "content"));
        if (str_eq(role, "developer")) {
            if (*text) sb_appendf(&out, "%s%s", out.len ? "\n\n" : "", text);
        } else sb_appendf(&out, "\n\nHuman: %s", text);
        free(text);
        i++;
    }
    return sb_detach(&out);
}

/* ---------- ChatML transcripts: qwen3, hermes ---------- */

static void chatml_turn(sb_t *b, const char *role, const char *body) { sb_appendf(b, "<|im_start|>%s\n%s<|im_end|>\n", role, body); }

static char *render_chatml(const json_t *msgs, bool hermes) {
    size_t n = json_len(msgs);
    if (!n) return xstrdup("");
    sb_t out = SB_INIT;
    for (size_t i = 0; i < n;) {
        const json_t *m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        sb_t body = SB_INIT;
        if (str_eq(role, "assistant")) {
            parts_t p;
            assistant_parts(m, &p);
            if (hermes) delimited_thinking(&body, "<think>", "</think>", p.thinking);
            else simple_thinking(&body, "<think>", "</think>", p.thinking);
            sb_append(&body, p.text);
            for (size_t k = 0; k < p.calls.n; k++) {
                if (k) sb_putc(&body, '\n');
                sb_append(&body, "<tool_call>\n");
                json_obj2(&body, p.calls.v[k]);
                sb_append(&body, "\n</tool_call>");
            }
            chatml_turn(&out, "assistant", sb_cstr(&body));
            parts_free(&p);
            i++;
        } else if (str_eq(role, "toolResult")) {
            results_t r = {0};
            i = result_run(msgs, i, &r);
            tool_response_results(&body, &r);
            chatml_turn(&out, hermes ? "tool" : "user", sb_cstr(&body));
            results_free(&r);
        } else {
            content_text(json_get(m, "content"), &body);
            chatml_turn(&out, str_eq(role, "developer") ? "system" : role ? role : "user", sb_cstr(&body));
            i++;
        }
        sb_free(&body);
    }
    return sb_detach(&out);
}

/* ---------- glm ---------- */

static void glm_calls(sb_t *b, const parts_t *p) {
    for (size_t k = 0; k < p->calls.n; k++) {
        const json_t *call = p->calls.v[k];
        if (k) sb_putc(b, '\n');
        sb_appendf(b, "<tool_call>%s", call_name(call));
        const char *key;
        const json_t *v;
        json_obj_foreach(json_get_obj(call, "arguments"), i, key, v) {
            sb_appendf(b, "\n<arg_key>%s</arg_key>\n<arg_value>", key);
            js_json(b, v);
            sb_append(b, "</arg_value>");
        }
        sb_append(b, "\n</tool_call>");
    }
}

static char *render_glm(const json_t *msgs) {
    size_t n = json_len(msgs);
    if (!n) return xstrdup("");
    sb_t out = SB_INIT;
    sb_append(&out, "[gMASK]<sop>");
    for (size_t i = 0; i < n;) {
        const json_t *m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            parts_t p;
            assistant_parts(m, &p);
            sb_append(&out, "<|assistant|>\n");
            if (*p.thinking) sb_appendf(&out, "\n<think>\n%s\n</think>", p.thinking);
            sb_append(&out, p.text);
            glm_calls(&out, &p);
            parts_free(&p);
            i++;
            continue;
        }
        if (str_eq(role, "toolResult")) {
            results_t r = {0};
            i = result_run(msgs, i, &r);
            sb_append(&out, "<|observation|>\n");
            tool_response_results(&out, &r);
            results_free(&r);
            continue;
        }
        sb_appendf(&out, "<|%s|>\n", str_eq(role, "developer") ? "system" : role ? role : "user");
        content_text(json_get(m, "content"), &out);
        i++;
    }
    return sb_detach(&out);
}

/* ---------- gemini ---------- */

static void py_string(sb_t *b, const char *s) {
    sb_putc(b, '"');
    for (; *s; s++) {
        if (*s == '\\') sb_append(b, "\\\\");
        else if (*s == '"') sb_append(b, "\\\"");
        else if (*s == '\n') sb_append(b, "\\n");
        else if (*s == '\r') sb_append(b, "\\r");
        else if (*s == '\t') sb_append(b, "\\t");
        else sb_putc(b, *s);
    }
    sb_putc(b, '"');
}

/* pyValue */
static void py_value(sb_t *b, const json_t *v, int depth) {
    if (json_is_null(v)) sb_append(b, "None");
    else if (json_is_bool(v)) sb_append(b, v->u.b ? "True" : "False");
    else if (json_is_num(v)) {
        if (isfinite(v->u.num.d)) json_js_num_sb(b, v->u.num.d);
        else py_string(b, isnan(v->u.num.d) ? "NaN" : v->u.num.d > 0 ? "Infinity" : "-Infinity");
    } else if (json_is_str(v)) py_string(b, json_sv(v));
    else if (json_is_arr(v)) {
        sb_putc(b, '[');
        const json_t *x;
        json_arr_foreach(v, i, x) {
            if (i) sb_append(b, ", ");
            if (depth < 64) py_value(b, x, depth + 1);
        }
        sb_putc(b, ']');
    } else {
        sb_putc(b, '{');
        const char *k;
        const json_t *x;
        json_obj_foreach(v, i, k, x) {
            if (i) sb_append(b, ", ");
            py_string(b, k);
            sb_append(b, ": ");
            if (depth < 64) py_value(b, x, depth + 1);
        }
        sb_putc(b, '}');
    }
}

static void gemini_call(sb_t *b, const json_t *call) {
    sb_appendf(b, "default_api.%s(", call_name(call));
    const char *key;
    const json_t *v;
    json_obj_foreach(json_get_obj(call, "arguments"), i, key, v) {
        sb_appendf(b, "%s%s=", i ? ", " : "", key);
        py_value(b, v, 0);
    }
    sb_putc(b, ')');
}

static void gemini_turn(sb_t *b, const char *role, const char *body) {
    sb_appendf(b, "<start_of_turn>%s\n%s<end_of_turn>\n", role, body);
}

static char *join_user(const char *left, const char *right) {
    if (!*left) return xstrdup(right);
    if (!*right) return xstrdup(left);
    return xasprintf("%s\n%s", left, right);
}

static char *render_gemini(const json_t *msgs) {
    size_t n = json_len(msgs);
    if (!n) return xstrdup("");
    sb_t out = SB_INIT;
    sb_append(&out, "<bos>");
    char *pre = xstrdup("");
    for (size_t i = 0; i < n;) {
        const json_t *m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "developer")) {
            char *t = content_text_dup(json_get(m, "content"));
            char *j = join_user(pre, t);
            free(pre), free(t);
            pre = j;
            i++;
            continue;
        }
        if (!str_eq(role, "assistant") && !str_eq(role, "toolResult")) {
            char *t = content_text_dup(json_get(m, "content"));
            char *j = join_user(pre, t);
            gemini_turn(&out, "user", j);
            free(j), free(t);
            free(pre);
            pre = xstrdup("");
            i++;
            continue;
        }
        if (*pre) {
            gemini_turn(&out, "user", pre);
            free(pre);
            pre = xstrdup("");
        }
        sb_t body = SB_INIT;
        if (str_eq(role, "assistant")) {
            parts_t p;
            assistant_parts(m, &p);
            if (*p.thinking) sb_appendf(&body, "```thinking\n%s\n```\n", p.thinking);
            sb_append(&body, p.text);
            sb_append(&body, "```tool_code\n");
            if (p.calls.n == 1) gemini_call(&body, p.calls.v[0]);
            else {
                sb_putc(&body, '[');
                for (size_t k = 0; k < p.calls.n; k++) {
                    if (k) sb_append(&body, ", ");
                    gemini_call(&body, p.calls.v[k]);
                }
                sb_putc(&body, ']');
            }
            sb_append(&body, "\n```");
            gemini_turn(&out, "model", sb_cstr(&body));
            parts_free(&p);
            i++;
        } else {
            results_t r = {0};
            i = result_run(msgs, i, &r);
            for (size_t k = 0; k < r.n; k++) sb_appendf(&body, "%s```tool_outputs\n%s\n```", k ? "\n" : "", r.v[k].text);
            gemini_turn(&out, "user", sb_cstr(&body));
            results_free(&r);
        }
        sb_free(&body);
    }
    if (*pre) gemini_turn(&out, "user", pre);
    free(pre);
    return sb_detach(&out);
}

/* ---------- gemma ---------- */

#define GEMMA_STR "<|\"|>"

static void gemma_value(sb_t *b, const json_t *v, int depth) {
    if (json_is_null(v)) sb_append(b, "null");
    else if (json_is_bool(v)) sb_append(b, v->u.b ? "true" : "false");
    else if (json_is_num(v)) {
        if (isfinite(v->u.num.d)) json_js_num_sb(b, v->u.num.d);
        else sb_append(b, isnan(v->u.num.d) ? "NaN" : v->u.num.d > 0 ? "Infinity" : "-Infinity");
    } else if (json_is_str(v)) sb_appendf(b, GEMMA_STR "%s" GEMMA_STR, json_sv(v));
    else if (json_is_arr(v)) {
        sb_putc(b, '[');
        const json_t *x;
        json_arr_foreach(v, i, x) {
            if (i) sb_putc(b, ',');
            if (depth < 64) gemma_value(b, x, depth + 1);
        }
        sb_putc(b, ']');
    } else {
        sb_putc(b, '{');
        const char *k;
        const json_t *x;
        json_obj_foreach(v, i, k, x) {
            sb_appendf(b, "%s%s:", i ? "," : "", k);
            if (depth < 64) gemma_value(b, x, depth + 1);
        }
        sb_putc(b, '}');
    }
}

static void gemma_results(sb_t *b, const results_t *r) {
    for (size_t k = 0; k < r->n; k++) {
        sb_appendf(b, "<|tool_response>response:%s{output:", r->v[k].name);
        json_t *parsed = json_parse_str(r->v[k].text); /* parseMaybeJson */
        if (parsed) gemma_value(b, parsed, 0);
        else sb_appendf(b, GEMMA_STR "%s" GEMMA_STR, r->v[k].text);
        json_free(parsed);
        sb_append(b, "}<tool_response|>");
    }
}

static void gemma_turn(sb_t *b, const char *role, const char *body) { sb_appendf(b, "<|turn>%s\n%s<turn|>", role, body); }

static char *render_gemma(const json_t *msgs) {
    size_t n = json_len(msgs);
    if (!n) return xstrdup("");
    sb_t out = SB_INIT;
    sb_append(&out, "<bos>");
    for (size_t i = 0; i < n;) {
        const json_t *m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        sb_t body = SB_INIT;
        if (str_eq(role, "assistant")) {
            parts_t p;
            assistant_parts(m, &p);
            if (*p.thinking) sb_appendf(&body, "<|channel>thought\n%s<channel|>", p.thinking);
            sb_append(&body, p.text);
            for (size_t k = 0; k < p.calls.n; k++) {
                sb_appendf(&body, "<|tool_call>call:%s{", call_name(p.calls.v[k]));
                const char *key;
                const json_t *v;
                json_obj_foreach(json_get_obj(p.calls.v[k], "arguments"), a, key, v) {
                    sb_appendf(&body, "%s%s:", a ? "," : "", key);
                    gemma_value(&body, v, 0);
                }
                sb_append(&body, "}<tool_call|>");
            }
            size_t next = i + 1;
            if (next < n && str_eq(json_get_str(json_at(msgs, next), "role"), "toolResult")) {
                results_t r = {0};
                next = result_run(msgs, next, &r);
                gemma_results(&body, &r);
                results_free(&r);
            }
            gemma_turn(&out, "model", sb_cstr(&body));
            parts_free(&p);
            i = next;
        } else if (str_eq(role, "toolResult")) {
            results_t r = {0};
            i = result_run(msgs, i, &r);
            gemma_results(&body, &r);
            gemma_turn(&out, "model", sb_cstr(&body));
            results_free(&r);
        } else {
            content_text(json_get(m, "content"), &body);
            gemma_turn(&out, str_eq(role, "developer") ? "system" : role ? role : "user", sb_cstr(&body));
            i++;
        }
        sb_free(&body);
    }
    return sb_detach(&out);
}

/* ---------- kimi ---------- */

static char *kimi_call_id(const char *name, const char *id, size_t index) {
    char *t = str_trim(id);
    if (str_starts(t, "functions.")) return t;
    free(t);
    return xasprintf("functions.%s:%zu", name, index);
}

static void kimi_turn(sb_t *b, const char *role, const char *name, const char *body) {
    sb_appendf(b, "<|im_%s|>%s<|im_middle|>%s<|im_end|>", role, name, body);
}

static char *render_kimi(const json_t *msgs) {
    sb_t out = SB_INIT;
    size_t n = json_len(msgs);
    for (size_t i = 0; i < n;) {
        const json_t *m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        sb_t body = SB_INIT;
        if (str_eq(role, "assistant")) {
            parts_t p;
            assistant_parts(m, &p);
            simple_thinking(&body, "<think>", "</think>", p.thinking);
            sb_append(&body, p.text);
            if (p.calls.n) {
                sb_append(&body, "<|tool_calls_section_begin|>");
                for (size_t k = 0; k < p.calls.n; k++) {
                    char *cid = kimi_call_id(call_name(p.calls.v[k]), call_id(p.calls.v[k]), k);
                    sb_appendf(&body, "<|tool_call_begin|>%s<|tool_call_argument_begin|>", cid);
                    js_json(&body, json_get(p.calls.v[k], "arguments"));
                    sb_append(&body, "<|tool_call_end|>");
                    free(cid);
                }
                sb_append(&body, "<|tool_calls_section_end|>");
            }
            kimi_turn(&out, "assistant", "assistant", sb_cstr(&body));
            parts_free(&p);
            i++;
        } else if (str_eq(role, "toolResult")) {
            results_t r = {0};
            i = result_run(msgs, i, &r);
            for (size_t k = 0; k < r.n; k++) {
                char *cid = kimi_call_id(r.v[k].name, r.v[k].id, r.v[k].index);
                char *t = xasprintf("## Return of %s\n%s", cid, r.v[k].text);
                kimi_turn(&out, "system", r.v[k].name, t);
                free(t), free(cid);
            }
            results_free(&r);
        } else {
            content_text(json_get(m, "content"), &body);
            const char *rn = str_eq(role, "developer") ? "system" : role ? role : "user";
            kimi_turn(&out, rn, rn, sb_cstr(&body));
            i++;
        }
        sb_free(&body);
    }
    return sb_detach(&out);
}

/* ---------- deepseek ---------- */

#define DS_BOS "<\xef\xbd\x9c" "begin\xe2\x96\x81of\xe2\x96\x81sentence\xef\xbd\x9c>"
#define DS_EOS "<\xef\xbd\x9c" "end\xe2\x96\x81of\xe2\x96\x81sentence\xef\xbd\x9c>"
#define DS_USER "<\xef\xbd\x9cUser\xef\xbd\x9c>"
#define DS_ASSISTANT "<\xef\xbd\x9c" "Assistant\xef\xbd\x9c>"
#define DS_CALLS_BEGIN "<\xef\xbd\x9ctool\xe2\x96\x81" "calls\xe2\x96\x81" "begin\xef\xbd\x9c>"
#define DS_CALLS_END "<\xef\xbd\x9ctool\xe2\x96\x81" "calls\xe2\x96\x81" "end\xef\xbd\x9c>"
#define DS_CALL_BEGIN "<\xef\xbd\x9ctool\xe2\x96\x81" "call\xe2\x96\x81" "begin\xef\xbd\x9c>"
#define DS_CALL_END "<\xef\xbd\x9ctool\xe2\x96\x81" "call\xe2\x96\x81" "end\xef\xbd\x9c>"
#define DS_SEP "<\xef\xbd\x9ctool\xe2\x96\x81sep\xef\xbd\x9c>"
#define DS_OUT_BEGIN "<\xef\xbd\x9ctool\xe2\x96\x81output\xe2\x96\x81" "begin\xef\xbd\x9c>"
#define DS_OUT_END "<\xef\xbd\x9ctool\xe2\x96\x81output\xe2\x96\x81" "end\xef\xbd\x9c>"

static char *render_deepseek(const json_t *msgs) {
    size_t n = json_len(msgs);
    if (!n) return xstrdup("");
    sb_t out = SB_INIT;
    sb_append(&out, DS_BOS);
    for (size_t i = 0; i < n;) {
        const json_t *m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            parts_t p;
            assistant_parts(m, &p);
            sb_append(&out, DS_ASSISTANT);
            simple_thinking(&out, "<think>", "</think>", p.thinking);
            sb_append(&out, p.text);
            if (p.calls.n) {
                sb_append(&out, DS_CALLS_BEGIN);
                for (size_t k = 0; k < p.calls.n; k++) {
                    sb_appendf(&out, DS_CALL_BEGIN "%s" DS_SEP, call_name(p.calls.v[k]));
                    js_json(&out, json_get(p.calls.v[k], "arguments"));
                    sb_append(&out, DS_CALL_END);
                }
                sb_append(&out, DS_CALLS_END);
            }
            sb_append(&out, DS_EOS);
            parts_free(&p);
            i++;
            continue;
        }
        if (str_eq(role, "toolResult")) {
            results_t r = {0};
            i = result_run(msgs, i, &r);
            for (size_t k = 0; k < r.n; k++) sb_appendf(&out, "%s" DS_OUT_BEGIN "%s" DS_OUT_END, k ? "\n" : "", r.v[k].text);
            results_free(&r);
            continue;
        }
        if (!str_eq(role, "developer")) sb_append(&out, DS_USER);
        content_text(json_get(m, "content"), &out);
        i++;
    }
    return sb_detach(&out);
}

/* ---------- harmony ---------- */

static char *render_harmony(const json_t *msgs) {
    sb_t out = SB_INIT;
    size_t n = json_len(msgs);
    for (size_t i = 0; i < n;) {
        const json_t *m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            parts_t p;
            assistant_parts(m, &p);
            bool emitted = false;
            if (*p.thinking) {
                sb_appendf(&out, "<|start|>assistant<|channel|>analysis<|message|>%s<|end|>", p.thinking);
                emitted = true;
            }
            if (*p.text) {
                sb_appendf(&out, "<|start|>assistant<|channel|>final<|message|>%s<|end|>", p.text);
                emitted = true;
            }
            for (size_t k = 0; k < p.calls.n; k++) {
                const char *nm = call_name(p.calls.v[k]);
                sb_appendf(&out, "<|start|>assistant<|channel|>commentary to=%s%s<|message|>", str_starts(nm, "functions.") ? "" : "functions.",
                           nm);
                js_json(&out, json_get(p.calls.v[k], "arguments"));
                sb_append(&out, "<|call|>");
                emitted = true;
            }
            if (!emitted) sb_append(&out, "<|start|>assistant<|channel|>final<|message|><|end|>");
            parts_free(&p);
            i++;
            continue;
        }
        if (str_eq(role, "toolResult")) {
            results_t r = {0};
            i = result_run(msgs, i, &r);
            for (size_t k = 0; k < r.n; k++)
                sb_appendf(&out, "<|start|>%s%s to=assistant<|channel|>commentary<|message|>%s<|end|>",
                           str_starts(r.v[k].name, "functions.") ? "" : "functions.", r.v[k].name, r.v[k].text);
            results_free(&r);
            continue;
        }
        sb_appendf(&out, "<|start|>%s<|message|>", role ? role : "user");
        content_text(json_get(m, "content"), &out);
        sb_append(&out, "<|end|>");
        i++;
    }
    return sb_detach(&out);
}

char *dialect_render_transcript(const char *d, const json_t *messages) {
    if (str_eq(d, "anthropic")) return render_legacy(messages, D_ANTHROPIC);
    if (str_eq(d, "minimax")) return render_legacy(messages, D_MINIMAX);
    if (str_eq(d, "qwen3")) return render_chatml(messages, false);
    if (str_eq(d, "hermes")) return render_chatml(messages, true);
    if (str_eq(d, "glm")) return render_glm(messages);
    if (str_eq(d, "gemini")) return render_gemini(messages);
    if (str_eq(d, "gemma")) return render_gemma(messages);
    if (str_eq(d, "kimi")) return render_kimi(messages);
    if (str_eq(d, "deepseek")) return render_deepseek(messages);
    if (str_eq(d, "harmony")) return render_harmony(messages);
    return render_legacy(messages, D_XML);
}

/* ---------- summary serialization (compaction/utils.ts) ---------- */

char *dialect_truncate_tool_result(const char *s) {
    /* JS string length counts UTF-16 units */
    size_t units = 0, cut = 0, len = strlen(s);
    bool cut_set = false;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int k = utf8_decode(s + i, len - i, &cp);
        size_t u = cp > 0xFFFF ? 2 : 1;
        if (!cut_set && units + u > TOOL_RESULT_MAX_UNITS) {
            cut = i;
            cut_set = true;
        }
        units += u;
        i += (size_t)k;
    }
    if (units <= TOOL_RESULT_MAX_UNITS) return xstrdup(s);
    return xasprintf("%.*s\n\n[... %zu more characters truncated]", (int)cut, s, units - TOOL_RESULT_MAX_UNITS);
}

char *dialect_escape_summary_tags(const char *raw) {
    sb_t esc = SB_INIT;
    for (const char *p = raw; *p;) {
        if (*p == '<') {
            const char *q = p + 1;
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q == '/') q++;
            while (*q && isspace((unsigned char)*q)) q++;
            size_t tl = !strncasecmp(q, "conversation", 12) ? 12 : !strncasecmp(q, "previous-summary", 16) ? 16 : 0;
            if (tl) {
                const char *r = q + tl;
                while (*r && isspace((unsigned char)*r)) r++;
                if (*r == '>') {
                    sb_append(&esc, "&lt;");
                    sb_appendn(&esc, p + 1, (size_t)(r + 1 - (p + 1)));
                    p = r + 1;
                    continue;
                }
            }
        }
        sb_putc(&esc, *p++);
    }
    return sb_detach(&esc);
}

/* escapeHarmonyControlTokens: <|start|> -> <\|start\|> */
static char *escape_harmony(const char *s) {
    static const char *const TOK[] = {"start", "end", "message", "channel", "constrain", "return", "call"};
    sb_t b = SB_INIT;
    for (const char *p = s; *p;) {
        if (p[0] == '<' && p[1] == '|') {
            bool hit = false;
            for (size_t t = 0; t < ARRAY_LEN(TOK) && !hit; t++) {
                size_t l = strlen(TOK[t]);
                if (!strncmp(p + 2, TOK[t], l) && p[2 + l] == '|' && p[3 + l] == '>') {
                    sb_appendf(&b, "<\\|%s\\|>", TOK[t]);
                    p += 4 + l;
                    hit = true;
                }
            }
            if (hit) continue;
        }
        sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

char *dialect_serialize_for_summary(const json_t *messages, const char *dialect, bool drop_thinking) {
    strvec_t useless = {0};
    const json_t *m;
    json_arr_foreach(messages, i, m) {
        if (str_eq(json_get_str(m, "role"), "toolResult") && json_get_bool(m, "useless", false) &&
            !json_get_bool(m, "isError", false) && json_get_str(m, "toolCallId"))
            strvec_push(&useless, json_get_str(m, "toolCallId"));
    }
    json_t *proc = json_arr();
    json_arr_foreach(messages, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            json_t *content = json_arr();
            const json_t *b;
            json_arr_foreach(json_get_arr(m, "content"), k, b) {
                const char *t = json_get_str(b, "type");
                if (str_eq(t, "toolCall") && strvec_contains(&useless, call_id(b))) continue;
                if (drop_thinking && str_eq(t, "thinking")) continue;
                json_push(content, json_clone(b));
            }
            if (json_len(content)) {
                json_t *c = json_clone(m);
                json_set(c, "content", content);
                json_push(proc, c);
            } else json_free(content);
            continue;
        }
        if (str_eq(role, "toolResult")) {
            if (json_get_str(m, "toolCallId") && strvec_contains(&useless, json_get_str(m, "toolCallId"))) continue;
            sb_t t = SB_INIT;
            const json_t *b;
            json_arr_foreach(json_get_arr(m, "content"), k, b) if (str_eq(json_get_str(b, "type"), "text"))
                sb_append(&t, json_get_str(b, "text") ? json_get_str(b, "text") : "");
            if (!t.len) {
                sb_free(&t);
                continue;
            }
            char *tr = dialect_truncate_tool_result(sb_cstr(&t));
            sb_free(&t);
            json_t *c = json_clone(m), *content = json_arr(), *tb = json_obj();
            json_set_str(tb, "type", "text");
            json_set_str(tb, "text", tr);
            free(tr);
            json_push(content, tb);
            json_set(c, "content", content);
            json_push(proc, c);
            continue;
        }
        json_push(proc, json_clone(m));
    }
    strvec_free(&useless);
    char *conv = dialect_render_transcript(dialect, proc);
    json_free(proc);
    if (str_eq(dialect, "harmony")) {
        char *e = escape_harmony(conv);
        free(conv);
        conv = e;
    }
    char *out = dialect_escape_summary_tags(conv);
    free(conv);
    return out;
}
