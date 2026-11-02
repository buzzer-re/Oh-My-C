/* dump.c - `/dump` family: session text dump, LLM-request sidecar, `/dump all|anon` zip archives and the
 * advisor history text. See dump.h. */
#include "session/dump.h"

#include "coding/advisor.h"
#include "coding/judgment.h"
#include "core/fs.h"
#include "session/anon.h"
#include "session/session.h"
#include "tools/outsink.h"
#include "tools/xdev.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <zlib.h>

#define DUMP_APP_NAME "omp" /* omp's file-name prefix (APP_NAME) */

static double json_num_val(const json_t *j) { return j->is_int ? (double)j->u.num.i : j->u.num.d; }

/* ---------- small helpers ---------- */

static void push(strvec_t *l, const char *s) { strvec_push(l, s ? s : ""); }

static void push_own(strvec_t *l, char *s) { strvec_push_own(l, s ? s : xstrdup("")); }

static char *trim_end(const char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

static char *trim_all(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return trim_end(s);
}

/* lines.join("\n").trim() */
static char *join_trim(strvec_t *l) {
    char *j = strvec_join(l, "\n");
    char *t = trim_all(j);
    free(j);
    return t;
}

/* JS Date.parse of an ISO-8601 UTC timestamp; NAN when unparseable */
static double iso_ms(const char *s) {
    if (!s) return NAN;
    int Y, M, D, h, m;
    double sec;
    char z = 0;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%lf%c", &Y, &M, &D, &h, &m, &sec, &z) < 6) return NAN;
    struct tm tm = {0};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = 0;
    time_t t = timegm(&tm);
    return (double)t * 1000.0 + floor(sec * 1000.0 + 0.5);
}

char *dump_format_duration(double ms) {
    const double SEC = 1000, MIN = 60 * SEC, HOUR = 60 * MIN, DAY = 24 * HOUR;
    if (!isfinite(ms) || ms <= 0) return xstrdup("0ms");
    if (ms < SEC) return xasprintf("%dms", (int)floor(ms));
    if (ms < MIN) {
        double tenths = floor(ms / 100 + 0.5);
        return tenths < 600 ? xasprintf("%.1fs", tenths / 10) : xstrdup("1m");
    }
    if (ms < HOUR) {
        long mins = (long)floor(ms / MIN), secs = (long)floor(fmod(ms, MIN) / SEC);
        return secs > 0 ? xasprintf("%ldm%lds", mins, secs) : xasprintf("%ldm", mins);
    }
    if (ms < DAY) {
        long hours = (long)floor(ms / HOUR), mins = (long)floor(fmod(ms, HOUR) / MIN);
        return mins > 0 ? xasprintf("%ldh%ldm", hours, mins) : xasprintf("%ldh", hours);
    }
    long days = (long)floor(ms / DAY), hours = (long)floor(fmod(ms, DAY) / HOUR);
    return hours > 0 ? xasprintf("%ldd%ldh", days, hours) : xasprintf("%ldd", days);
}

/* formatDumpTime */
static char *dump_time(double ms) {
    if (!isfinite(ms) || fabs(ms) > 8.64e15) return xasprintf("invalid time %.0f", ms);
    return iso8601_from_ms((int64_t)ms);
}

/* headingStamp */
static char *heading_stamp(const json_t *msg) {
    const json_t *ts = json_get(msg, "timestamp");
    if (!json_is_num(ts)) return xstrdup("");
    double t = json_num_val(ts);
    if (!isfinite(t) || t <= 0) return xstrdup("");
    char *iso = dump_time(t);
    sb_t b = SB_INIT;
    sb_appendf(&b, " \xc2\xb7 %s", iso);
    free(iso);
    if (str_eq(json_get_str(msg, "role"), "assistant")) {
        strvec_t timing = {0};
        if (json_is_num(json_get(msg, "duration"))) {
            char *d = dump_format_duration(json_num_val(json_get(msg, "duration")));
            push_own(&timing, xasprintf("took %s", d));
            free(d);
        }
        if (json_is_num(json_get(msg, "ttft"))) {
            char *d = dump_format_duration(json_num_val(json_get(msg, "ttft")));
            push_own(&timing, xasprintf("ttft %s", d));
            free(d);
        }
        if (timing.n) {
            char *j = strvec_join(&timing, ", ");
            sb_appendf(&b, " (%s)", j);
            free(j);
        }
        strvec_free(&timing);
    }
    return sb_detach(&b);
}

/* ---------- thinking ---------- */

/* canonicalizeMessage: trimmed text, "" when only dots/ellipses/whitespace */
static char *canonicalize(const char *text) {
    if (!text) return xstrdup("");
    char *t = trim_all(text);
    for (const unsigned char *p = (const unsigned char *)t; *p;) {
        if (*p == '.' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
            continue;
        }
        if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0xA6) {
            p += 3;
            continue;
        }
        return t;
    }
    free(t);
    return xstrdup("");
}

static bool ascii_ws(char c) { return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32; }

static ssize_t find_close(const char *open, const char *close, const char *text, size_t start, size_t end) {
    int depth = 1;
    size_t cur = start, ol = strlen(open), cl = strlen(close);
    while (cur < end) {
        const char *nc = strstr(text + cur, close);
        if (!nc || (size_t)(nc - text) >= end) return -1;
        const char *no = strstr(text + cur, open);
        if (no && no < nc) {
            depth++;
            cur = (size_t)(no - text) + ol;
            continue;
        }
        if (--depth == 0) return nc - text;
        cur = (size_t)(nc - text) + cl;
    }
    return -1;
}

/* unwrapDelimitedThinking */
static char *unwrap_thinking(const char *open, const char *close, const char *text) {
    size_t len = strlen(text), end = len, cur = 0, ol = strlen(open), cl = strlen(close);
    while (end > 0 && ascii_ws(text[end - 1])) end--;
    while (cur < end && ascii_ws(text[cur])) cur++;
    if (cur >= end || strncmp(text + cur, open, ol)) return xstrdup(text);
    strvec_t segs = {0};
    while (cur < end) {
        if (strncmp(text + cur, open, ol)) {
            strvec_free(&segs);
            return xstrdup(text);
        }
        size_t is = cur + ol;
        ssize_t ie = find_close(open, close, text, is, end);
        if (ie < 0) {
            strvec_free(&segs);
            return xstrdup(text);
        }
        size_t te = (size_t)ie, ts = is;
        while (te > is && ascii_ws(text[te - 1])) te--;
        while (ts < te && ascii_ws(text[ts])) ts++;
        char *inner = xstrndup(text + ts, te - ts);
        push_own(&segs, unwrap_thinking(open, close, inner));
        free(inner);
        cur = (size_t)ie + cl;
        while (cur < end && ascii_ws(text[cur])) cur++;
    }
    char *r = strvec_join(&segs, "\n");
    strvec_free(&segs);
    return r;
}

/* renderDelimitedThinking */
static char *render_thinking(const char *text) {
    if (!*text) return xstrdup("");
    char *u = unwrap_thinking("<thinking>", "</thinking>", text);
    char *r = xasprintf("<thinking>\n%s\n</thinking>", u);
    free(u);
    return r;
}

/* ---------- tool inventory ---------- */

static void comment_lines(strvec_t *l, const char *text) {
    strvec_t parts = str_split(text, "\n");
    for (size_t i = 0; i < parts.n; i++) {
        char *line = xasprintf("// %s", parts.v[i]);
        push_own(l, trim_end(line));
        free(line);
    }
    strvec_free(&parts);
}

char *dump_tool_inventory(agent_tool_t *const *tools, size_t ntools) {
    if (!ntools) return xstrdup("");
    strvec_t decls = {0};
    for (size_t i = 0; i < ntools; i++) {
        const agent_tool_t *t = tools[i];
        char *params = json_schema_to_ts_style(t->parameters, true);
        strvec_t lines = {0};
        const char *desc = t->description ? t->description : "";
        if (*desc) comment_lines(&lines, desc);
        char *ex = agent_tool_examples_jsdoc(t);
        if (ex && *ex) {
            if (*desc) push(&lines, "//");
            comment_lines(&lines, ex);
        }
        free(ex);
        push_own(&lines, !strcmp(params, "{}") ? xasprintf("type %s = ();", t->name) : xasprintf("type %s = (_: %s);", t->name, params));
        push_own(&decls, strvec_join(&lines, "\n"));
        strvec_free(&lines);
        free(params);
    }
    char *body = strvec_join(&decls, "\n\n");
    strvec_free(&decls);
    char *r = xasprintf("## functions\n\nnamespace functions {\n\n%s\n\n} // namespace functions", body);
    free(body);
    return r;
}

/* ---------- transcript ---------- */

static char *content_text(const json_t *content, const char *image) {
    if (json_is_str(content)) return xstrdup(json_sv(content));
    strvec_t parts = {0};
    json_t *b;
    json_arr_foreach(content, i, b) push(&parts, str_eq(json_get_str(b, "type"), "text") ? json_get_str(b, "text") : image);
    char *r = strvec_join(&parts, "\n");
    strvec_free(&parts);
    return r;
}

static void append_assistant_content(strvec_t *l, const json_t *content) {
    json_t *c;
    json_arr_foreach(content, i, c) {
        const char *t = json_get_str(c, "type");
        if (str_eq(t, "text")) push(l, json_get_str(c, "text"));
        else if (str_eq(t, "thinking")) {
            char *th = canonicalize(json_get_str(c, "thinking"));
            if (*th) {
                char *r = render_thinking(th);
                push_own(l, xasprintf("%s\n", r));
                free(r);
            }
            free(th);
        } else if (str_eq(t, "toolCall")) {
            push_own(l, xasprintf("### Tool Call: %s", json_get_str(c, "name") ? json_get_str(c, "name") : ""));
            const json_t *raw = json_get(c, "arguments");
            if (!json_is_obj(raw)) continue;
            const char *intent = json_get_str(raw, INTENT_FIELD);
            if (intent) {
                char *it = trim_all(intent);
                if (*it) {
                    strvec_t parts = str_split(intent, "\n");
                    for (size_t k = 0; k < parts.n; k++) push_own(l, xasprintf("// %s", parts.v[k]));
                    strvec_free(&parts);
                }
                free(it);
            }
            json_t *args = json_obj();
            const char *k;
            json_t *v;
            json_obj_foreach(raw, j, k, v) if (strcmp(k, INTENT_FIELD)) json_set(args, k, json_clone(v));
            if (json_len(args)) {
                push(l, "```yaml");
                char *y = bun_yaml_stringify(args);
                push_own(l, trim_end(y));
                free(y);
                push(l, "```\n");
            }
            json_free(args);
        }
    }
}

static const char *const CUSTOM_ACRONYMS[][2] = {{"acp", "ACP"}, {"irc", "IRC"}, {"lsp", "LSP"}, {"mcp", "MCP"},
                                                  {"rpc", "RPC"}, {"ttsr", "TTSR"}, {"tui", "TUI"}, {"xdev", "XDev"}};

/* systemNoticeTitle */
static char *system_notice_title(const char *custom_type) {
    strvec_t words = {0};
    for (const char *p = custom_type ? custom_type : ""; *p;) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        size_t n = 0;
        while (isalnum((unsigned char)p[n])) n++;
        if (n) strvec_push_own(&words, xstrndup(p, n));
        p += n;
    }
    if (words.n) {
        char *low = str_lower(words.v[words.n - 1]);
        if (!strcmp(low, "notice")) free(words.v[--words.n]);
        free(low);
    }
    sb_t b = SB_INIT;
    for (size_t i = 0; i < words.n; i++) {
        char *low = str_lower(words.v[i]);
        const char *ac = NULL;
        for (size_t k = 0; k < ARRAY_LEN(CUSTOM_ACRONYMS); k++)
            if (!strcmp(CUSTOM_ACRONYMS[k][0], low)) ac = CUSTOM_ACRONYMS[k][1];
        if (i) sb_putc(&b, ' ');
        if (ac) sb_append(&b, ac);
        else {
            sb_putc(&b, (char)toupper((unsigned char)words.v[i][0]));
            sb_append(&b, words.v[i] + 1);
        }
        free(low);
    }
    strvec_free(&words);
    char *label = sb_detach(&b);
    char *r = *label ? xasprintf("System Notice: %s", label) : xstrdup("System Notice");
    free(label);
    return r;
}

static void append_custom(strvec_t *l, const json_t *msg) {
    char *content = content_text(json_get(msg, "content"), "[Image]");
    char *stamp = heading_stamp(msg);
    const char *s = content;
    while (*s && isspace((unsigned char)*s)) s++;
    bool notice = !strncmp(s, "<system-notice", 14) && (s[14] == '>' || isspace((unsigned char)s[14]));
    if (!notice) {
        push_own(l, xasprintf("## %s%s\n", json_get_str(msg, "customType") ? json_get_str(msg, "customType") : "", stamp));
        push(l, content);
        push(l, "\n");
    } else {
        size_t longest = 0, run = 0;
        for (const char *p = content; *p; p++) {
            run = *p == '`' ? run + 1 : 0;
            if (run > longest) longest = run;
        }
        size_t fl = longest + 1 > 3 ? longest + 1 : 3;
        char *fence = xcalloc(fl + 1, 1);
        memset(fence, '`', fl);
        char *title = system_notice_title(json_get_str(msg, "customType"));
        push_own(l, xasprintf("## %s%s\n", title, stamp));
        push_own(l, xasprintf("%sxml", fence));
        push(l, content);
        push(l, fence);
        push(l, "\n");
        free(title);
        free(fence);
    }
    free(stamp);
    free(content);
}

static void append_transcript(strvec_t *l, const json_t *messages) {
    json_t *msg;
    json_arr_foreach(messages, i, msg) {
        const char *role = json_get_str(msg, "role");
        char *stamp = heading_stamp(msg);
        if (str_eq(role, "user") || str_eq(role, "developer")) {
            push_own(l, xasprintf("%s%s\n", str_eq(role, "developer") ? "## Developer" : "## User", stamp));
            const json_t *c = json_get(msg, "content");
            if (json_is_str(c)) push(l, json_sv(c));
            else {
                json_t *b;
                json_arr_foreach(c, k, b) {
                    const char *t = json_get_str(b, "type");
                    if (str_eq(t, "text")) push(l, json_get_str(b, "text"));
                    else if (str_eq(t, "image")) push(l, "[Image]");
                }
            }
            push(l, "\n");
        } else if (str_eq(role, "assistant")) {
            push_own(l, xasprintf("## Assistant%s\n", stamp));
            append_assistant_content(l, json_get(msg, "content"));
            push(l, "");
        } else if (str_eq(role, "toolResult")) {
            push_own(l, xasprintf("### Tool Result: %s%s", json_get_str(msg, "toolName") ? json_get_str(msg, "toolName") : "", stamp));
            if (json_get_bool(msg, "isError", false)) push(l, "(error)");
            json_t *b;
            json_arr_foreach(json_get(msg, "content"), k, b) {
                const char *t = json_get_str(b, "type");
                if (str_eq(t, "text")) {
                    push(l, "```");
                    push(l, json_get_str(b, "text"));
                    push(l, "```");
                } else if (str_eq(t, "image")) push(l, "[Image output]");
            }
            push(l, "");
        } else if (str_eq(role, "bashExecution") || str_eq(role, "pythonExecution")) {
            if (!json_get_bool(msg, "excludeFromContext", false)) {
                bool bash = str_eq(role, "bashExecution");
                push_own(l, xasprintf("## %s Execution%s\n", bash ? "Bash" : "Python", stamp));
                push_own(l, bash ? out_bash_execution_text(msg) : out_python_execution_text(msg));
                push(l, "\n");
            }
        } else if (str_eq(role, "custom") || str_eq(role, "hookMessage")) {
            append_custom(l, msg);
        } else if (str_eq(role, "branchSummary")) {
            push_own(l, xasprintf("## Branch Summary%s\n", stamp));
            push_own(l, xasprintf("(from branch: %s)\n", json_get_str(msg, "fromId") ? json_get_str(msg, "fromId") : ""));
            push(l, json_get_str(msg, "summary"));
            push(l, "\n");
        } else if (str_eq(role, "compactionSummary")) {
            push_own(l, xasprintf("## Compaction Summary%s\n", stamp));
            push_own(l, xasprintf("(%lld tokens before compaction)\n", (long long)json_get_int(msg, "tokensBefore", 0)));
            push(l, json_get_str(msg, "summary"));
            push(l, "\n");
        } else if (str_eq(role, "fileMention")) {
            push_own(l, xasprintf("## File Mention%s\n", stamp));
            json_t *f;
            json_arr_foreach(json_get(msg, "files"), k, f) {
                push_own(l, xasprintf("<file path=\"%s\">", json_get_str(f, "path") ? json_get_str(f, "path") : ""));
                if (json_get_str(f, "content") && *json_get_str(f, "content")) push(l, json_get_str(f, "content"));
                if (json_get(f, "image")) push(l, "[Image attached]");
                push(l, "</file>\n");
            }
            push(l, "\n");
        }
        free(stamp);
    }
}

char *dump_format_text(const json_t *messages, const json_t *system_prompt, const ai_model_t *model, const char *thinking_level,
                       agent_tool_t *const *tools, size_t ntools) {
    strvec_t l = {0};
    strvec_t sp = {0};
    json_t *x;
    json_arr_foreach(system_prompt, i, x) if (json_sv(x) && *json_sv(x)) push(&sp, json_sv(x));
    if (sp.n) {
        push(&l, "## System Prompt\n");
        for (size_t i = 0; i < sp.n; i++) {
            if (sp.n > 1) push_own(&l, xasprintf("### System Prompt %zu\n", i + 1));
            push(&l, sp.v[i]);
            push(&l, "\n");
        }
    }
    strvec_free(&sp);
    push(&l, "## Configuration\n");
    push_own(&l, model ? xasprintf("Model: %s/%s", model->provider, model->id) : xstrdup("Model: (not selected)"));
    push_own(&l, xasprintf("Thinking Level: %s", thinking_level ? thinking_level : ""));
    push(&l, "\n");
    if (ntools) {
        push(&l, "## Available Tools\n");
        push_own(&l, dump_tool_inventory(tools, ntools));
        push(&l, "\n");
    }
    append_transcript(&l, messages);
    char *r = join_trim(&l);
    strvec_free(&l);
    return r;
}

char *dump_format_subagent_text(const char *key, const json_t *messages, const char *model, const char *thinking_level, bool aborted) {
    strvec_t l = {0};
    push_own(&l, xasprintf("# Subagent: %s\n", key));
    push_own(&l, xasprintf("Model: %s", model ? model : "(unknown)"));
    if (thinking_level && *thinking_level) push_own(&l, xasprintf("Thinking Level: %s", thinking_level));
    if (aborted) push(&l, "Status: aborted");
    push(&l, "\n");
    append_transcript(&l, messages);
    char *r = join_trim(&l);
    strvec_free(&l);
    return r;
}

/* ---------- concise history (session-history-format.ts, default options) ---------- */

/* oneLine: whitespace runs collapsed, cut to 120 UTF-16 units with an ellipsis */
static char *one_line(const char *text, size_t max) {
    sb_t b = SB_INIT;
    bool ws = false;
    for (const char *p = text ? text : ""; *p; p++) {
        if (isspace((unsigned char)*p)) ws = true;
        else {
            if (ws && b.len) sb_putc(&b, ' ');
            ws = false;
            sb_putc(&b, *p);
        }
    }
    char *flat = sb_detach(&b);
    size_t units = 0, cut = 0;
    for (const unsigned char *p = (const unsigned char *)flat; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;
        size_t w = *p >= 0xF0 ? 2 : 1;
        if (units + w <= max - 1) cut = (size_t)((const char *)p - flat) + 1;
        units += w;
    }
    if (units <= max) return flat;
    /* extend `cut` over the continuation bytes of the last kept code point */
    while (flat[cut] && ((unsigned char)flat[cut] & 0xC0) == 0x80) cut++;
    char *r = xasprintf("%.*s\xe2\x80\xa6", (int)cut, flat);
    free(flat);
    return r;
}

static const char *primary_arg_value(const json_t *v, char **owned) {
    *owned = NULL;
    if (json_is_str(v) && *json_sv(v)) return json_sv(v);
    if (json_is_arr(v) && json_len(v)) {
        strvec_t parts = {0};
        json_t *x;
        json_arr_foreach(v, i, x) {
            if (!json_is_str(x)) {
                strvec_free(&parts);
                return "";
            }
            push(&parts, json_sv(x));
        }
        *owned = strvec_join(&parts, ", ");
        strvec_free(&parts);
        return *owned;
    }
    return "";
}

static char *primary_arg_text(const char *name, const json_t *args) {
    if (!json_is_obj(args)) return xstrdup("");
    char *o1, *o2;
    if (str_eq(name, "advise")) {
        const char *note = json_get_str(args, "note") ? json_get_str(args, "note") : "";
        const char *sev = json_get_str(args, "severity") ? json_get_str(args, "severity") : "";
        if (*note && *sev) return xasprintf("%s: %s", sev, note);
        if (*note) return xstrdup(note);
        if (*sev) return xstrdup(sev);
    }
    if (str_eq(name, "grep")) {
        const char *pat = primary_arg_value(json_get(args, "pattern"), &o1);
        const char *paths = primary_arg_value(json_get(args, "path"), &o2);
        char *o3 = NULL;
        if (!*paths) paths = primary_arg_value(json_get(args, "paths"), &o3);
        char *r = *pat && *paths ? xasprintf("%s @ %s", pat, paths) : *pat ? xstrdup(pat) : *paths ? xstrdup(paths) : NULL;
        free(o1);
        free(o2);
        free(o3);
        if (r) return r;
    }
    if (str_eq(name, "glob")) {
        const char *paths = primary_arg_value(json_get(args, "path"), &o1);
        char *o3 = NULL;
        if (!*paths) paths = primary_arg_value(json_get(args, "paths"), &o3);
        char *r = *paths ? xstrdup(paths) : NULL;
        free(o1);
        free(o3);
        if (r) return r;
    }
    if (str_eq(name, "ast_grep")) {
        const char *pat = primary_arg_value(json_get(args, "pat"), &o1);
        char *r = *pat ? xstrdup(pat) : NULL;
        free(o1);
        if (r) return r;
    }
    static const char *const KEYS[] = {"path", "file_path", "filePath", "command", "cmd", "pattern", "url", "query",
                                       "prompt", "assignment", "note", "message", "op", "name", "id"};
    for (size_t i = 0; i < ARRAY_LEN(KEYS); i++) {
        const char *v = primary_arg_value(json_get(args, KEYS[i]), &o1);
        char *r = *v ? xstrdup(v) : NULL;
        free(o1);
        if (r) return r;
    }
    json_t *rest = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(args, i, k, v) {
        if (!strcmp(k, INTENT_FIELD)) continue;
        if (json_is_str(v) && *json_sv(v)) {
            json_free(rest);
            return xstrdup(json_sv(v));
        }
        json_set(rest, k, json_clone(v));
    }
    char *r = json_len(rest) ? json_dump_js(rest) : xstrdup("{}");
    json_free(rest);
    return r;
}

static size_t line_count(const char *t) {
    if (!t || !*t) return 0;
    size_t n = 1;
    for (; *t; t++)
        if (*t == '\n') n++;
    return n;
}

static char *tool_call_line(const char *name, const json_t *args, const json_t *result) {
    char *pa = primary_arg_text(name, args);
    char *pl = one_line(pa, 120);
    char *head = xasprintf("\xe2\x86\x92 %s(%s)", name ? name : "", pl);
    free(pa);
    free(pl);
    char *r;
    if (!result) r = xasprintf("%s \xe2\x87\x92 pending", head);
    else {
        char *text = content_text(json_get(result, "content"), "[image]");
        size_t lines = line_count(text);
        char *count = xasprintf("%zu %s", lines, lines == 1 ? "line" : "lines");
        if (json_get_bool(result, "isError", false)) {
            char *first = xstrndup(text, strcspn(text, "\n"));
            char *fl = one_line(first, 120);
            r = *fl ? xasprintf("%s \xe2\x87\x92 error \xc2\xb7 %s \xe2\x80\x94 %s", head, count, fl)
                    : xasprintf("%s \xe2\x87\x92 error \xc2\xb7 %s", head, count);
            free(fl);
            free(first);
        } else r = xasprintf("%s \xe2\x87\x92 ok \xc2\xb7 %s", head, count);
        free(count);
        free(text);
    }
    free(head);
    return r;
}

static bool hidden_custom_shown(const char *type) {
    static const char *const T[] = {"plan-mode-context", "plan-mode-reference", "image-attachment-description", "image-attachment"};
    for (size_t i = 0; i < ARRAY_LEN(T); i++)
        if (str_eq(type, T[i])) return true;
    return false;
}

static char *custom_one_liner(const json_t *msg) {
    const char *type = json_get_str(msg, "customType") ? json_get_str(msg, "customType") : "";
    const json_t *d = json_get(msg, "details");
#define DSTR(K_) (json_get_str(d, K_) ? json_get_str(d, K_) : "")
    if (!strcmp(type, "irc:incoming")) {
        char *p = one_line(DSTR("message"), 120);
        char *r = xasprintf("[irc] %s \xe2\x86\x92 me: %s", *DSTR("from") ? DSTR("from") : "?", p);
        free(p);
        return r;
    }
    if (!strcmp(type, "irc:relay") || !strcmp(type, "irc:workpool")) {
        char *p = one_line(DSTR("body"), 120);
        char *r = !strcmp(type, "irc:relay") ? xasprintf("[irc] %s \xe2\x86\x92 %s: %s", *DSTR("from") ? DSTR("from") : "?", *DSTR("to") ? DSTR("to") : "?", p)
                                              : xasprintf("[pool] %s \xe2\x86\x92 %s: %s", DSTR("pool"), *DSTR("to") ? DSTR("to") : "?", p);
        free(p);
        return r;
    }
    if (!strcmp(type, "async-result")) {
        const json_t *jobs = json_get(d, "jobs");
        strvec_t labels = {0};
        if (json_is_arr(jobs) && json_len(jobs)) {
            json_t *j;
            json_arr_foreach(jobs, i, j) push(&labels, json_get_str(j, "label") && *json_get_str(j, "label") ? json_get_str(j, "label")
                                                       : json_get_str(j, "jobId")                                ? json_get_str(j, "jobId")
                                                                                                                 : "job");
        } else {
            push(&labels, json_get_str(d, "label") && *json_get_str(d, "label") ? json_get_str(d, "label")
                          : json_get_str(d, "jobId")                            ? json_get_str(d, "jobId")
                                                                                : "job");
        }
        char *j = strvec_join(&labels, ", ");
        char *p = one_line(j, 120);
        char *r = xasprintf("[async-result] %s", p);
        free(p);
        free(j);
        strvec_free(&labels);
        return r;
    }
    char *text = content_text(json_get(msg, "content"), "[image]");
    char *r;
    if (!strcmp(type, "image-attachment")) {
        const char *path = DSTR("path");
        char *found = NULL;
        if (!*path) {
            const char *sp = strstr(text, "Source path: `");
            if (sp && (sp == text || sp[-1] == '\n')) {
                const char *s = sp + 14, *e = strchr(s, '\n');
                size_t n = e ? (size_t)(e - s) : strlen(s);
                if (n > 1 && s[n - 1] == '`') found = xstrndup(s, n - 1);
            }
            path = found ? found : "";
        }
        if (!*path) {
            char *p = one_line(text, 120);
            r = xasprintf("[%s] %s", type, p);
            free(p);
        } else {
            const json_t *idx = json_get(d, "index");
            char *is = NULL;
            if (json_is_num(idx)) is = xasprintf("%lld", (long long)json_num_val(idx));
            else {
                const char *m = strstr(text, "`[Image #");
                if (m && isdigit((unsigned char)m[9])) is = xstrndup(m + 9, strspn(m + 9, "0123456789"));
                if (is && strncmp(m + 9 + strlen(is), "]`", 2)) {
                    free(is);
                    is = NULL;
                }
            }
            r = xasprintf("[image-attachment] Image #%s: %s", is ? is : "?", path);
            free(is);
        }
        free(found);
    } else {
        char *p = one_line(text, 120);
        r = xasprintf("[%s] %s", type, p);
        free(p);
    }
    free(text);
#undef DSTR
    return r;
}

char *dump_history_markdown(const json_t *messages) {
    strvec_t l = {0};
    hmap_t results, consumed;
    hmap_init(&results);
    hmap_init(&consumed);
    json_t *m;
    json_arr_foreach(messages, i, m) if (str_eq(json_get_str(m, "role"), "toolResult") && json_get_str(m, "toolCallId"))
        hmap_put(&results, json_get_str(m, "toolCallId"), m);
    json_arr_foreach(messages, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "user") || str_eq(role, "developer")) {
            char *text = content_text(json_get(m, "content"), "[image]");
            char *t = trim_all(text);
            if (*t) {
                push_own(&l, xasprintf("## %s", role));
                push(&l, "");
                push(&l, text);
                push(&l, "");
            }
            free(t);
            free(text);
        } else if (str_eq(role, "assistant")) {
            strvec_t body = {0};
            json_t *b;
            json_arr_foreach(json_get(m, "content"), k, b) {
                const char *t = json_get_str(b, "type");
                if (str_eq(t, "text")) {
                    char *tt = trim_all(json_get_str(b, "text") ? json_get_str(b, "text") : "");
                    if (*tt) push(&body, json_get_str(b, "text"));
                    free(tt);
                } else if (str_eq(t, "toolCall")) {
                    const char *id = json_get_str(b, "id");
                    const json_t *res = id ? hmap_get(&results, id) : NULL;
                    if (res) hmap_put(&consumed, id, (void *)1);
                    push_own(&body, tool_call_line(json_get_str(b, "name"), json_get(b, "arguments"), res));
                }
            }
            if (body.n) {
                push(&l, "## assistant");
                push(&l, "");
                for (size_t k = 0; k < body.n; k++) push(&l, body.v[k]);
                push(&l, "");
            }
            strvec_free(&body);
        } else if (str_eq(role, "toolResult")) {
            if (json_get_str(m, "toolCallId") && hmap_has(&consumed, json_get_str(m, "toolCallId"))) continue;
            push_own(&l, tool_call_line(json_get_str(m, "toolName"), NULL, m));
            push(&l, "");
        } else if (str_eq(role, "bashExecution") || str_eq(role, "pythonExecution")) {
            if (json_get_bool(m, "excludeFromContext", false)) continue;
            bool bash = str_eq(role, "bashExecution");
            const json_t *ec = json_get(m, "exitCode");
            char *status = json_get_bool(m, "cancelled", false) ? xstrdup("cancelled")
                           : json_is_num(ec) && json_num_val(ec) != 0 ? xasprintf("error \xc2\xb7 exit %lld", (long long)json_num_val(ec))
                                                                      : xstrdup("ok");
            size_t lines = line_count(json_get_str(m, "output"));
            char *src = one_line(json_get_str(m, bash ? "command" : "code"), 120);
            push_own(&l, xasprintf("\xe2\x86\x92 user-%s! %s \xe2\x87\x92 %s \xc2\xb7 %zu %s", bash ? "bash" : "python", src, status, lines,
                                   lines == 1 ? "line" : "lines"));
            push(&l, "");
            free(src);
            free(status);
        } else if (str_eq(role, "custom") || str_eq(role, "hookMessage")) {
            const json_t *disp = json_get(m, "display");
            if (json_is_bool(disp) && !json_get_bool(m, "display", true) && !hidden_custom_shown(json_get_str(m, "customType"))) continue;
            push_own(&l, custom_one_liner(m));
            push(&l, "");
        } else if (str_eq(role, "branchSummary")) {
            char *p = one_line(json_get_str(m, "summary"), 120);
            push_own(&l, xasprintf("[branch] from %s: %s", json_get_str(m, "fromId") ? json_get_str(m, "fromId") : "", p));
            push(&l, "");
            free(p);
        } else if (str_eq(role, "compactionSummary")) {
            char *p = one_line(json_get_str(m, "summary"), 120);
            push_own(&l, xasprintf("[compaction] %s", p));
            push(&l, "");
            free(p);
        } else if (str_eq(role, "fileMention")) {
            strvec_t paths = {0};
            json_t *f;
            json_arr_foreach(json_get(m, "files"), k, f) push(&paths, json_get_str(f, "path"));
            char *j = strvec_join(&paths, ", ");
            char *p = one_line(j, 120);
            push_own(&l, xasprintf("[file-mention] %s", p));
            push(&l, "");
            free(p);
            free(j);
            strvec_free(&paths);
        }
    }
    hmap_free(&results, NULL);
    hmap_free(&consumed, NULL);
    char *t = join_trim(&l);
    strvec_free(&l);
    char *r = xasprintf("%s\n", t);
    free(t);
    return r;
}

/* ---------- session context as AgentMessages ---------- */

static void emit_context_entry(json_t *out, const json_t *e) {
    const char *t = json_get_str(e, "type");
    double ts = iso_ms(json_get_str(e, "timestamp"));
    if (str_eq(t, "message")) {
        if (json_get(e, "message")) json_push(out, json_clone(json_get(e, "message")));
    } else if (str_eq(t, "custom_message")) {
        json_t *m = json_obj();
        json_set_str(m, "role", "custom");
        json_set_str(m, "customType", json_get_str(e, "customType") ? json_get_str(e, "customType") : "");
        json_set(m, "content", json_get(e, "content") ? json_clone(json_get(e, "content")) : json_str(""));
        json_set_bool(m, "display", json_get_bool(e, "display", true));
        if (json_get(e, "details")) json_set(m, "details", json_clone(json_get(e, "details")));
        if (json_get_str(e, "attribution")) json_set_str(m, "attribution", json_get_str(e, "attribution"));
        json_set(m, "timestamp", isfinite(ts) ? json_int((int64_t)ts) : json_null());
        json_push(out, m);
    } else if (str_eq(t, "branch_summary")) {
        json_t *m = json_obj();
        json_set_str(m, "role", "branchSummary");
        json_set_str(m, "summary", json_get_str(e, "summary") ? json_get_str(e, "summary") : "");
        json_set_str(m, "fromId", json_get_str(e, "fromId") ? json_get_str(e, "fromId") : "");
        json_set(m, "timestamp", isfinite(ts) ? json_int((int64_t)ts) : json_null());
        json_push(out, m);
    }
}

json_t *dump_context_messages(const json_t *path) {
    json_t *out = json_arr();
    size_t n = json_len(path);
    ssize_t boundary = -1, compaction = -1;
    for (size_t i = 0; i < n; i++) {
        const char *t = json_get_str(json_at(path, i), "type");
        if (str_eq(t, "reset_boundary")) {
            boundary = (ssize_t)i;
            compaction = -1;
        } else if (str_eq(t, "compaction")) compaction = (ssize_t)i;
    }
    size_t start = (size_t)(boundary + 1);
    if (compaction >= 0) {
        json_t *c = json_at(path, (size_t)compaction);
        json_t *m = json_obj();
        json_set_str(m, "role", "compactionSummary");
        json_set_str(m, "summary", json_get_str(c, "summary") ? json_get_str(c, "summary") : "");
        json_set_int(m, "tokensBefore", json_get_int(c, "tokensBefore", 0));
        double ts = iso_ms(json_get_str(c, "timestamp"));
        json_set(m, "timestamp", isfinite(ts) ? json_int((int64_t)ts) : json_null());
        json_push(out, m);
        const char *first = json_get_str(c, "firstKeptEntryId");
        ssize_t kept = -1;
        for (size_t i = start; first && i < (size_t)compaction; i++)
            if (str_eq(json_get_str(json_at(path, i), "id"), first)) kept = (ssize_t)i;
        if (kept >= 0)
            for (size_t i = (size_t)kept; i < (size_t)compaction; i++) emit_context_entry(out, json_at(path, i));
        start = (size_t)compaction + 1;
    }
    for (size_t i = start; i < n; i++) emit_context_entry(out, json_at(path, i));
    return out;
}

static json_t *session_messages(app_t *a) {
    if (!a->session || !a->session->leaf_id) return json_arr();
    json_t *path = session_path_to(a->session, a->session->leaf_id);
    json_t *m = dump_context_messages(path);
    json_free(path);
    return m;
}

/* this.thinkingLevel: unset (empty) for a model without a thinking dial */
static const char *thinking_level(const ai_model_t *m, ai_effort_t e) { return m && m->reasoning ? effort_name(e) : NULL; }

/* ---------- JSON.stringify(value, null, 2) ---------- */

static void js_pretty(sb_t *b, const json_t *j, int level) {
    if (json_is_arr(j) && json_len(j)) {
        sb_putc(b, '[');
        json_t *x;
        json_arr_foreach(j, i, x) {
            sb_appendf(b, "%s\n%*s", i ? "," : "", (level + 1) * 2, "");
            js_pretty(b, x, level + 1);
        }
        sb_appendf(b, "\n%*s]", level * 2, "");
    } else if (json_is_obj(j) && json_len(j)) {
        sb_putc(b, '{');
        const char *k;
        json_t *v;
        json_obj_foreach(j, i, k, v) {
            sb_appendf(b, "%s\n%*s", i ? "," : "", (level + 1) * 2, "");
            json_js_quote_sb(b, k, strlen(k));
            sb_append(b, ": ");
            js_pretty(b, v, level + 1);
        }
        sb_appendf(b, "\n%*s}", level * 2, "");
    } else json_dump_js_sb(b, j);
}

/* ---------- tmp names ---------- */

/* Snowflake.next(): 42-bit ms since 2015-01-01 << 22 | a random-seeded 22-bit sequence, 16 hex chars */
static char *snowflake(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint32_t seq;
    static bool seeded;
    pthread_mutex_lock(&mu);
    if (!seeded) {
        unsigned char r[4] = {0};
        rand_bytes(r, sizeof r);
        seq = ((uint32_t)r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16 | (uint32_t)r[3] << 24) & 0x3fffff;
        seeded = true;
    }
    seq = (seq + 1) & 0x3fffff;
    uint64_t dt = (uint64_t)(now_ms() - 1420070400000LL);
    uint64_t v = (dt << 22) | seq;
    pthread_mutex_unlock(&mu);
    return xasprintf("%016llx", (unsigned long long)v);
}

static char *tmp_path(const char *stem, const char *ext) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    char *sf = snowflake();
    size_t dl = strlen(dir);
    char *p = xasprintf("%.*s/%s-%s.%s", (int)(dl > 1 && dir[dl - 1] == '/' ? dl - 1 : dl), dir, stem, sf, ext);
    free(sf);
    return p;
}

/* ---------- llm request ---------- */

/* #formatLlmRequestJson: the request context at the convertToLlm boundary */
static char *llm_request_json(app_t *a) {
    agent_t *ag = a->agent;
    json_t *p = json_obj();
    pthread_mutex_lock(&ag->mu);
    const ai_model_t *m = ag->model ? ag->model : a->model;
    json_set(p, "model", m && m->raw ? json_clone(m->raw) : json_null());
    const char *tl = thinking_level(m, a->thinking);
    json_set(p, "thinkingLevel", tl ? json_str(tl) : json_null());
    json_set(p, "serviceTier", json_null());
    json_set(p, "systemPrompt", ag->system_prompt ? json_clone(ag->system_prompt) : json_arr());
    json_t *tools = json_arr();
    for (size_t i = 0; i < ag->tools.n; i++) {
        const agent_tool_t *t = ag->tools.v[i];
        json_t *o = json_obj();
        json_set_str(o, "name", t->name);
        json_set_str(o, "description", t->description ? t->description : "");
        json_set(o, "parameters", t->parameters ? json_clone(t->parameters) : json_obj());
        json_push(tools, o);
    }
    json_set(p, "tools", tools);
    json_set(p, "messages", json_clone(ag->messages));
    pthread_mutex_unlock(&ag->mu);
    sb_t b = SB_INIT;
    js_pretty(&b, p, 0);
    sb_putc(&b, '\n');
    json_free(p);
    return sb_detach(&b);
}

char *app_dump_llm_request(app_t *a, char **err) {
    *err = NULL;
    json_t *msgs = session_messages(a);
    size_t n = json_len(msgs);
    json_free(msgs);
    if (!n) return NULL;
    char *body = llm_request_json(a);
    char *path = tmp_path(DUMP_APP_NAME "-llm-request", "json");
    int rc = fs_write(path, body, strlen(body));
    free(body);
    if (rc) {
        *err = xasprintf("%s: %s", path, strerror(-rc));
        free(path);
        return NULL;
    }
    return path;
}

char *app_format_session_text(app_t *a) {
    json_t *msgs = session_messages(a);
    if (!json_len(msgs)) {
        json_free(msgs);
        return NULL;
    }
    agent_t *ag = a->agent;
    pthread_mutex_lock(&ag->mu);
    const ai_model_t *m = ag->model ? ag->model : a->model;
    char *t = dump_format_text(msgs, ag->system_prompt, m, thinking_level(m, a->thinking), ag->tools.v, ag->tools.n);
    pthread_mutex_unlock(&ag->mu);
    json_free(msgs);
    return t;
}

/* ---------- zip ---------- */

static uint32_t crc32_of(const char *d, size_t n) { return (uint32_t)crc32(0L, (const Bytef *)d, (uInt)n); }

static void le16(sb_t *b, unsigned v) {
    sb_putc(b, (char)(v & 0xff));
    sb_putc(b, (char)((v >> 8) & 0xff));
}

static void le32(sb_t *b, uint32_t v) {
    le16(b, v & 0xffff);
    le16(b, v >> 16);
}

/* zlib.deflateRawSync (default level) */
static char *deflate_raw(const char *d, size_t n, size_t *out_len) {
    z_stream z = {0};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    uLong cap = deflateBound(&z, (uLong)n);
    char *buf = xmalloc(cap ? cap : 1);
    z.next_in = (Bytef *)d;
    z.avail_in = (uInt)n;
    z.next_out = (Bytef *)buf;
    z.avail_out = (uInt)cap;
    int rc = deflate(&z, Z_FINISH);
    *out_len = z.total_out;
    deflateEnd(&z);
    if (rc != Z_STREAM_END) {
        free(buf);
        return NULL;
    }
    return buf;
}

int dump_zip_write(const char *path, const char *const *names, const char *const *data, const size_t *lens, size_t n, char **err) {
    sb_t local = SB_INIT, central = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        const char *name = names[i];
        if (!*name || name[0] == '/' || strstr(name, "..") || strchr(name, '\\')) {
            *err = xasprintf("Cannot write unsafe ZIP member path '%s'", name);
            sb_free(&local);
            sb_free(&central);
            return -1;
        }
        size_t nl = strlen(name), dl = 0;
        char *defl = lens[i] ? deflate_raw(data[i], lens[i], &dl) : NULL;
        bool stored = !defl || dl >= lens[i];
        const char *payload = stored ? data[i] : defl;
        size_t pl = stored ? lens[i] : dl;
        uint32_t crc = crc32_of(data[i], lens[i]);
        size_t offset = local.len;
        le32(&local, 0x04034b50);
        le16(&local, 20);
        le16(&local, 0x800);
        le16(&local, stored ? 0 : 8);
        le16(&local, 0);
        le16(&local, 0x21);
        le32(&local, crc);
        le32(&local, (uint32_t)pl);
        le32(&local, (uint32_t)lens[i]);
        le16(&local, (unsigned)nl);
        le16(&local, 0);
        sb_appendn(&local, name, nl);
        sb_appendn(&local, payload, pl);
        le32(&central, 0x02014b50);
        le16(&central, (3u << 8) | 20);
        le16(&central, 20);
        le16(&central, 0x800);
        le16(&central, stored ? 0 : 8);
        le16(&central, 0);
        le16(&central, 0x21);
        le32(&central, crc);
        le32(&central, (uint32_t)pl);
        le32(&central, (uint32_t)lens[i]);
        le16(&central, (unsigned)nl);
        le16(&central, 0);
        le16(&central, 0);
        le16(&central, 0);
        le16(&central, 0);
        le32(&central, (uint32_t)0100644 << 16);
        le32(&central, (uint32_t)offset);
        sb_appendn(&central, name, nl);
        free(defl);
    }
    size_t cd_off = local.len, cd_len = central.len;
    sb_appendn(&local, central.s ? central.s : "", central.len);
    le32(&local, 0x06054b50);
    le16(&local, 0);
    le16(&local, 0);
    le16(&local, (unsigned)n);
    le16(&local, (unsigned)n);
    le32(&local, (uint32_t)cd_len);
    le32(&local, (uint32_t)cd_off);
    le16(&local, 0);
    int rc = fs_write(path, local.s, local.len);
    sb_free(&local);
    sb_free(&central);
    if (rc) {
        *err = xasprintf("%s: %s", path, strerror(-rc));
        return -1;
    }
    return 0;
}

/* ---------- archives ---------- */

void dump_archive_free(dump_archive_t *ar) {
    free(ar->path);
    strvec_free(&ar->files);
    free(ar->subagent_error);
    strvec_free(&ar->malformed);
    strvec_free(&ar->unreadable);
    memset(ar, 0, sizeof *ar);
}

char *dump_archive_report(const dump_archive_t *ar) {
    strvec_t l = {0};
    push_own(&l, xasprintf("Session dump archive: %s", ar->path));
    size_t fc = ar->files.n;
    push_own(&l, xasprintf("Contains %zu file%s (%d subagent transcript%s):", fc, fc == 1 ? "" : "s", ar->subagent_count,
                           ar->subagent_count == 1 ? "" : "s"));
    for (size_t i = 0; i < fc; i++) push_own(&l, xasprintf("  %s", ar->files.v[i]));
    if (ar->subagent_error) push_own(&l, xasprintf("Subagent transcripts unavailable: %s", ar->subagent_error));
    for (size_t i = 0; i < ar->malformed.n; i++) {
        const char *tab = strchr(ar->malformed.v[i], '\t');
        long cnt = tab ? atol(tab + 1) : 0;
        push_own(&l, xasprintf("Skipped %ld malformed record%s in %.*s", cnt, cnt == 1 ? "" : "s",
                               (int)(tab ? (size_t)(tab - ar->malformed.v[i]) : strlen(ar->malformed.v[i])), ar->malformed.v[i]));
    }
    for (size_t i = 0; i < ar->unreadable.n; i++) push_own(&l, xasprintf("Not exported (no readable session header): %s", ar->unreadable.v[i]));
    push(&l, ar->anonymized ? ANON_REVIEW_NOTE : "This archive persists on disk and may contain raw context/secrets \xe2\x80\x94 treat accordingly.");
    char *r = strvec_join(&l, "\n");
    strvec_free(&l);
    return r;
}

/* collectSubSessions: `<dir>/<stem>/<AgentId>.jsonl` (nested the same way), sorted, advisors and
 * backups skipped */
typedef struct {
    char *key;
    session_t *s;
    bool aborted;
} subsess_t;
typedef VEC(subsess_t) subsess_list_t;

static int cmp_str(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

static void collect_dir(const char *dir, const char *parent, subsess_list_t *out, strvec_t *unreadable) {
    DIR *d = opendir(dir);
    if (!d) return;
    strvec_t files = {0}, dirs = {0};
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2]))) continue;
        char *p = xasprintf("%s/%s", dir, de->d_name);
        struct stat st;
        if (lstat(p, &st) == 0) {
            if (S_ISREG(st.st_mode)) push(&files, de->d_name);
            else if (S_ISDIR(st.st_mode)) push(&dirs, de->d_name);
        }
        free(p);
    }
    closedir(d);
    qsort(files.v, files.n, sizeof *files.v, cmp_str);
    for (size_t i = 0; i < files.n; i++) {
        const char *name = files.v[i];
        if (!str_ends(name, ".jsonl") || strstr(name, ".bak")) continue;
        if (!strcmp(name, "__advisor.jsonl") || !strncmp(name, "__advisor.", 10)) continue;
        char *agent = xstrndup(name, strlen(name) - 6);
        char *key = parent ? xasprintf("%s/%s", parent, agent) : xstrdup(agent);
        char *path = xasprintf("%s/%s", dir, name);
        char *err = NULL;
        session_t *s = session_open(path, &err);
        if (s && json_len(s->entries)) {
            char *tomb = xasprintf("%s.tombstone", name);
            subsess_t ss = {xstrdup(key), s, strvec_contains(&files, tomb)};
            free(tomb);
            vec_push(out, ss);
        } else {
            if (!s && err && !strstr(err, "empty")) push(unreadable, key);
            if (s) session_free(s);
        }
        free(err);
        if (strvec_contains(&dirs, agent)) {
            char *sub = xasprintf("%s/%s", dir, agent);
            collect_dir(sub, key, out, unreadable);
            free(sub);
        }
        free(path);
        free(key);
        free(agent);
    }
    strvec_free(&files);
    strvec_free(&dirs);
}

static void collect_sub_sessions(const char *session_file, subsess_list_t *out, strvec_t *unreadable) {
    if (!session_file || !str_ends(session_file, ".jsonl")) return;
    char *dir = xstrndup(session_file, strlen(session_file) - 6);
    collect_dir(dir, NULL, out, unreadable);
    free(dir);
}

static void free_sub_sessions(subsess_list_t *l) {
    for (size_t i = 0; i < l->n; i++) {
        free(l->v[i].key);
        session_free(l->v[i].s);
    }
    vec_free(l);
}

typedef struct {
    strvec_t names, data;
} members_t;

static void member(members_t *m, const char *name, char *data) {
    push(&m->names, name);
    push_own(&m->data, data);
}

static int write_members(dump_archive_t *out, members_t *m, const char *stem, char **err) {
    size_t *lens = xcalloc(m->names.n + 1, sizeof *lens);
    for (size_t i = 0; i < m->names.n; i++) lens[i] = strlen(m->data.v[i]);
    out->path = tmp_path(stem, "zip");
    int rc = dump_zip_write(out->path, (const char *const *)m->names.v, (const char *const *)m->data.v, lens, m->names.n, err);
    free(lens);
    for (size_t i = 0; i < m->names.n; i++) push(&out->files, m->names.v[i]);
    strvec_free(&m->names);
    strvec_free(&m->data);
    return rc;
}

static char *subagent_member_text(const subsess_t *ss) {
    json_t *path = session_path_to(ss->s, ss->s->leaf_id);
    json_t *msgs = dump_context_messages(path);
    json_free(path);
    char *r = NULL;
    if (json_len(msgs)) {
        session_context_t sc;
        session_build_context(ss->s, &sc);
        char *t = dump_format_subagent_text(ss->key, msgs, sc.model, sc.thinking, ss->aborted);
        r = xasprintf("%s\n", t);
        free(t);
        session_context_free(&sc);
    }
    json_free(msgs);
    return r;
}

int app_dump_archive(app_t *a, bool anon, dump_archive_t *out, char **err) {
    memset(out, 0, sizeof *out);
    *err = NULL;
    json_t *msgs = session_messages(a);
    size_t n = json_len(msgs);
    json_free(msgs);
    if (!n) return 0;
    const char *file = a->session ? a->session->file : NULL;
    subsess_list_t subs = {0};
    strvec_t unreadable = {0};
    collect_sub_sessions(file, &subs, &unreadable);
    members_t m = {0};
    int rc;
    if (!anon) {
        char *text = app_format_session_text(a);
        member(&m, "session.md", xasprintf("%s\n", text ? text : ""));
        free(text);
        member(&m, "llm-request.json", llm_request_json(a));
        for (size_t i = 0; i < subs.n; i++) {
            char *t = subagent_member_text(&subs.v[i]);
            if (!t) continue;
            char *name = xasprintf("subagents/%s.md", subs.v[i].key);
            member(&m, name, t);
            free(name);
            out->subagent_count++;
        }
        strvec_free(&unreadable); /* the text dump reports nothing for unreadable transcripts */
        rc = write_members(out, &m, DUMP_APP_NAME "-dump", err);
    } else {
        anon_t *an = anon_new();
        sb_t b = SB_INIT;
        if (a->session && a->session->header) {
            char *line = anon_entry_line(an, a->session->header);
            sb_append(&b, line);
            free(line);
        }
        json_t *e;
        json_arr_foreach(a->session ? a->session->entries : NULL, i, e) {
            char *line = anon_entry_line(an, e);
            if (b.len) sb_putc(&b, '\n');
            sb_append(&b, line);
            free(line);
        }
        sb_putc(&b, '\n');
        member(&m, "session.jsonl", sb_detach(&b));
        for (size_t i = 0; i < unreadable.n; i++) {
            strvec_t parts = str_split(unreadable.v[i], "/");
            sb_t nm = SB_INIT;
            for (size_t k = 0; k < parts.n; k++) {
                char *seg = anon_segment(an, parts.v[k]);
                sb_appendf(&nm, "%s%s", k ? "/" : "", seg);
                free(seg);
            }
            strvec_free(&parts);
            push_own(&out->unreadable, xasprintf("subagents/%s.jsonl", sb_cstr(&nm)));
            sb_free(&nm);
        }
        for (size_t i = 0; i < subs.n; i++) {
            strvec_t parts = str_split(subs.v[i].key, "/");
            sb_t nm = SB_INIT;
            for (size_t k = 0; k < parts.n; k++) {
                char *seg = anon_segment(an, parts.v[k]);
                sb_appendf(&nm, "%s%s", k ? "/" : "", seg);
                free(seg);
            }
            strvec_free(&parts);
            sb_t body = SB_INIT;
            session_t *s = subs.v[i].s;
            if (s->header) {
                char *line = anon_entry_line(an, s->header);
                sb_append(&body, line);
                free(line);
            }
            json_arr_foreach(s->entries, k, e) {
                char *line = anon_entry_line(an, e);
                if (body.len) sb_putc(&body, '\n');
                sb_append(&body, line);
                free(line);
            }
            sb_putc(&body, '\n');
            char *name = xasprintf("subagents/%s.jsonl", sb_cstr(&nm));
            member(&m, name, sb_detach(&body));
            free(name);
            sb_free(&nm);
            out->subagent_count++;
        }
        anon_free(an);
        strvec_free(&unreadable);
        out->anonymized = true;
        rc = write_members(out, &m, DUMP_APP_NAME "-dump-anon", err);
    }
    free_sub_sessions(&subs);
    if (rc) {
        dump_archive_free(out);
        return -1;
    }
    return 1;
}

/* ---------- advisor ---------- */

char *app_advisor_history_text(app_t *a, bool compact) {
    if (!a->advisor) return NULL;
    agent_t *ag = advisor_agent(a->advisor);
    if (!ag) return NULL;
    pthread_mutex_lock(&ag->mu);
    char *r = compact ? dump_history_markdown(ag->messages)
                      : dump_format_text(ag->messages, ag->system_prompt, ag->model, thinking_level(ag->model, ag->thinking), ag->tools.v,
                                         ag->tools.n);
    pthread_mutex_unlock(&ag->mu);
    return r;
}
