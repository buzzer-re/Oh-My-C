/* context.c - the /context viewer (ports of omp overlays/context-viewer.ts, context-viewer-format.ts,
 * context-viewer-overview.ts, chrome/context-view-model.ts, chrome/context-presentation.ts, chrome/context-map.ts,
 * status-line/context-usage.ts computeContextBreakdown and session/context-view.ts buildSessionContextView).
 *
 * Three states: Overview (usage summary, occupancy map, category legend) -> a category's entry previews -> one
 * entry's full content. Token figures are omc's estimate (~4.1 characters per token, the status-line gauge's rule;
 * omp counts with its native tokenizer); "Used" is the gauge's figure, anchored on provider-reported usage. */
#include "ui/ui.h"
#include "ui/modelbrowser.h"
#include "ui/tools/tc.h"

#include "config/settings.h"
#include "tools/outsink.h"

#include <math.h>
#include <ctype.h>
#include <time.h>

/* ---------- token estimate ---------- */

static int64_t tok(const char *s) { return s ? (int64_t)((double)strlen(s) / 4.1) : 0; }

/* ---------- model ---------- */

enum { C_SYSTEM, C_SKILLS, C_BUILTIN, C_CUSTOM, C_USER, C_ASSIST, C_THINK, C_CALLS, C_OUTPUT, C_N, P_BUFFER = C_N, P_FREE };

static const struct {
    const char *id, *label, *overview;
    bool openable;
    tui_tc_t color;
    const char *symbol;
} PRES[] = {
    {"system-prompt", "System Prompt", "System Prompt (excluding Skills)", true, TUI_TC_ACCENT, "context.content"},
    {"skills", "Skills", NULL, false, TUI_TC_SUCCESS, "context.content"},
    {"built-in-tools", "Built-in Tools", NULL, true, TUI_TC_WARNING, "context.content"},
    {"custom-tools", "Custom Tools", NULL, true, TUI_TC_CUSTOM_MESSAGE_LABEL, "context.content"},
    {"user-messages", "User / Developer Messages", NULL, true, TUI_TC_ACCENT, "context.messages"},
    {"assistant-messages", "Assistant Messages", NULL, true, TUI_TC_USER_MESSAGE_TEXT, "context.messages"},
    {"assistant-thinking", "Assistant Thinking", NULL, true, TUI_TC_CUSTOM_MESSAGE_LABEL, "context.content"},
    {"tool-calls", "Tool Calls", NULL, true, TUI_TC_WARNING, "context.content"},
    {"tool-output", "Tool Output", NULL, true, TUI_TC_MUTED, "context.messages"},
    {"buffer", "Auto-Compact Buffer", NULL, false, TUI_TC_WARNING, "context.buffer"},
    {"free", "Free Space", NULL, false, TUI_TC_DIM, "context.free"},
};

typedef struct {
    char *id, *label;
    int64_t tokens;
    bool interrupted;
} cgroup_t;

typedef struct {
    char *id, *label, *text, *info, *path, *call_id;
    int64_t tokens, ts; /* ts 0: none */
    bool interrupted;
    cgroup_t *group; /* borrowed */
} centry_t;

typedef struct ccat {
    int id;
    char *key, *label;
    int64_t tokens;
    VEC(centry_t *) entries; /* borrowed */
    VEC(struct ccat *) children;
} ccat_t;

typedef struct {
    int64_t window, used, buffer, free_, threshold; /* threshold < 0: none */
    ccat_t cats[C_N];
    VEC(centry_t *) all;
    VEC(cgroup_t *) groups;
    char *model_label;
} cdata_t;

static centry_t *entry_new(cdata_t *d, int cat, const char *id, const char *label, const char *text, int64_t tokens) {
    centry_t *e = xcalloc(1, sizeof *e);
    e->id = xstrdup(id);
    e->label = xstrdup(label);
    e->text = xstrdup(text ? text : "");
    e->tokens = tokens;
    vec_push(&d->all, e);
    if (cat >= 0) {
        vec_push(&d->cats[cat].entries, e);
        d->cats[cat].tokens += tokens;
    }
    return e;
}

static void tool_output(cdata_t *d, const char *tool, centry_t *e) {
    ccat_t *cat = &d->cats[C_OUTPUT], *g = NULL;
    for (size_t i = 0; i < cat->children.n; i++)
        if (str_eq(cat->children.v[i]->label, tool)) g = cat->children.v[i];
    if (!g) {
        g = xcalloc(1, sizeof *g);
        g->id = C_OUTPUT;
        g->key = xasprintf("tool-output/tool-%zu", cat->children.n);
        g->label = xstrdup(tool);
        vec_push(&cat->children, g);
    }
    vec_push(&g->entries, e);
    g->tokens += e->tokens;
    vec_push(&cat->entries, e);
    cat->tokens += e->tokens;
}

static char *summarize_content(const json_t *content) {
    if (json_is_str(content)) return xstrdup(json_sv(content));
    sb_t b = SB_INIT;
    json_t *x;
    json_arr_foreach(content, i, x) {
        if (b.len) sb_putc(&b, '\n');
        if (str_eq(json_get_str(x, "type"), "image")) {
            const char *data = json_get_str(x, "data");
            sb_appendf(&b, "[image \xc2\xb7 %s \xc2\xb7 ~%zu bytes]", json_get_str(x, "mimeType") ? json_get_str(x, "mimeType") : "",
                       data ? strlen(data) * 3 / 4 : 0);
        } else {
            sb_append(&b, json_get_str(x, "text") ? json_get_str(x, "text") : "");
        }
    }
    return sb_detach(&b);
}

static char *opaque_size(const char *name, const char *v) {
    char *n = NULL;
    size_t len = utf8_strlen(v);
    /* toLocaleString("en-US") */
    char buf[32];
    snprintf(buf, sizeof buf, "%zu", len);
    sb_t b = SB_INIT;
    size_t l = strlen(buf);
    for (size_t i = 0; i < l; i++) {
        if (i && (l - i) % 3 == 0) sb_putc(&b, ',');
        sb_putc(&b, buf[i]);
    }
    n = xasprintf("%s \xc2\xb7 %s chars", name, sb_cstr(&b));
    sb_free(&b);
    return n;
}

static void add_assistant(cdata_t *d, const json_t *m, size_t idx) {
    const json_t *content = json_get_arr(m, "content"), *b;
    size_t nthink = 0, nblocks = json_len(content);
    int64_t replay = 0;
    bool all_empty_thinking = nblocks > 0;
    json_arr_foreach(content, i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "thinking") || str_eq(t, "redactedThinking")) {
            nthink++;
            replay += tok(json_get_str(b, "thinking"));
            char *tr = str_trim(json_get_str(b, "thinking") ? json_get_str(b, "thinking") : "");
            if (*tr || !str_eq(t, "thinking")) all_empty_thinking = false;
            free(tr);
        } else {
            all_empty_thinking = false;
        }
    }
    bool interrupted = str_eq(json_get_str(m, "stopReason"), "aborted") && replay == 0 &&
                       json_get_int(json_get(m, "usage"), "output", 0) == 0 && all_empty_thinking;
    char prefix[64];
    snprintf(prefix, sizeof prefix, "%zu/%zu", idx, idx);
    cgroup_t *group = NULL;
    if (nthink > 1) {
        group = xcalloc(1, sizeof *group);
        group->id = xasprintf("assistant-thinking/%s", prefix);
        group->label = xasprintf("Message %zu", idx + 1);
        group->tokens = replay;
        group->interrupted = interrupted;
        vec_push(&d->groups, group);
        d->cats[C_THINK].tokens += replay;
    }
    int64_t ts = json_get_int(m, "timestamp", 0);
    size_t ti = 0;
    json_arr_foreach(content, bi, b) {
        const char *t = json_get_str(b, "type");
        bool think = str_eq(t, "thinking") || str_eq(t, "redactedThinking");
        int cat = str_eq(t, "toolCall") ? C_CALLS : think ? C_THINK : C_ASSIST;
        char *text = NULL, *opaque = NULL, *label;
        int64_t tokens;
        if (str_eq(t, "toolCall")) {
            text = json_dump(json_get(b, "arguments"), 2);
            char *compact = json_dump(json_get(b, "arguments"), 0);
            tokens = tok(json_get_str(b, "name")) + tok(compact);
            free(compact);
            label = xstrdup(json_get_str(b, "name") ? json_get_str(b, "name") : "");
        } else if (think) {
            const char *th = json_get_str(b, "thinking");
            text = xstrdup(str_eq(t, "thinking") && th && *th ? th : "[Encrypted thinking]");
            if (str_eq(t, "thinking") && json_get_str(b, "thinkingSignature"))
                opaque = opaque_size("signature", json_get_str(b, "thinkingSignature"));
            else if (str_eq(t, "redactedThinking")) opaque = opaque_size("data", json_get_str(b, "data") ? json_get_str(b, "data") : "");
            tokens = group ? 0 : replay;
            label = xasprintf("Thinking %zu/%zu \xc2\xb7 message %zu", ++ti, nthink, idx + 1);
        } else {
            if (!str_eq(t, "image")) {
                text = xstrdup(json_get_str(b, "text") ? json_get_str(b, "text") : "");
            } else {
                json_t *one = json_arr();
                json_push(one, json_clone(b));
                text = summarize_content(one);
                json_free(one);
            }
            tokens = tok(text);
            label = xasprintf("%s \xc2\xb7 message %zu", t ? t : "text", idx + 1);
        }
        char *id = xasprintf("%s/%s/%zu", PRES[cat].id, prefix, bi);
        centry_t *e = entry_new(d, -1, id, label, text, tokens);
        vec_push(&d->cats[cat].entries, e);
        if (!(think && group)) d->cats[cat].tokens += tokens;
        e->ts = ts;
        if (opaque) e->info = opaque;
        if (str_eq(t, "toolCall") && json_get_str(b, "id")) e->call_id = xstrdup(json_get_str(b, "id"));
        if (think) {
            e->group = group;
            e->interrupted = interrupted;
        }
        free(id);
        free(label);
        free(text);
    }
}

static void build(cdata_t *d, app_t *a) {
    agent_t *ag = a->agent;
    for (int i = 0; i < C_N; i++) {
        d->cats[i].id = i;
        d->cats[i].label = xstrdup(PRES[i].label);
    }
    pthread_mutex_lock(&ag->mu);
    json_t *sys = json_clone(ag->system_prompt), *msgs = json_clone(ag->messages);
    bool has_read = false;
    json_t *tools = json_arr();
    for (size_t i = 0; i < ag->tools.n; i++) {
        agent_tool_t *t = ag->tools.v[i];
        if (str_eq(t->name, "read")) has_read = true;
        /* toolWireSchema without the harness intent field (the agent loop injects it per request) */
        json_t *w = agent_tool_wire(t);
        json_set(w, "parameters", json_clone(t->parameters));
        json_push(tools, w);
    }
    pthread_mutex_unlock(&ag->mu);

    /* skill listing (renderedSkills: the read tool is present, hidden skills excluded) */
    int64_t skills = 0;
    if (settings_bool("skillful", true) && has_read) {
        size_t chars = 0;
        for (size_t i = 0; i < a->disc.skills.n; i++) {
            if (a->disc.skills.v[i].hide) continue;
            chars += strlen(a->disc.skills.v[i].name) + (a->disc.skills.v[i].description ? strlen(a->disc.skills.v[i].description) : 0);
        }
        skills = (int64_t)((double)chars / 4.1);
    }
    /* system prompt blocks */
    json_t *x;
    int64_t sys_ctx = 0, sys0 = 0, nonmsg = 0;
    json_arr_foreach(sys, i, x) {
        const char *s = json_sv(x) ? json_sv(x) : "";
        char *id = xasprintf("system-prompt/%zu", i), *label = xasprintf("System Block %zu", i + 1);
        entry_new(d, C_SYSTEM, id, label, s, tok(s));
        free(id);
        free(label);
        if (i == 0) sys0 = tok(s);
        else sys_ctx += tok(s);
    }
    d->cats[C_SKILLS].tokens = skills;
    d->cats[C_SYSTEM].tokens = MAX(0, d->cats[C_SYSTEM].tokens - skills);
    /* tool definitions: name, description (with rendered examples), wire schema */
    int64_t tools_tok = 0;
    json_arr_foreach(tools, i, x) {
        const char *name = json_get_str(x, "name") ? json_get_str(x, "name") : "";
        const char *desc = json_get_str(x, "description") ? json_get_str(x, "description") : "";
        bool mcp = str_starts(name, "mcp__");
        int cat = mcp ? C_CUSTOM : C_BUILTIN;
        char *compact = json_dump(json_get(x, "parameters"), 0), *pretty = json_dump(json_get(x, "parameters"), 2);
        int64_t t = tok(name) + tok(desc) + tok(compact);
        tools_tok += t;
        char *text = *desc ? xasprintf("%s\n\n%s\n\n%s", name, desc, pretty) : xasprintf("%s\n\n%s", name, pretty);
        char *id = xasprintf("%s/%zu", PRES[cat].id, i);
        centry_t *e = entry_new(d, cat, id, name, text, t);
        e->info = xstrdup(mcp ? "mcp" : "builtin");
        e->path = xasprintf("<%s:%s>", mcp ? "mcp" : "builtin", name);
        free(id);
        free(text);
        free(compact);
        free(pretty);
    }
    /* messages */
    json_arr_foreach(msgs, i, x) {
        const char *role = json_get_str(x, "role");
        if (json_get_bool(x, "excludeFromContext", false)) continue;
        if (str_eq(role, "assistant")) {
            add_assistant(d, x, i);
            continue;
        }
        char prefix[64];
        snprintf(prefix, sizeof prefix, "%zu/%zu", i, i);
        int64_t ts = json_get_int(x, "timestamp", 0);
        bool bash = str_eq(role, "bashExecution"), py = str_eq(role, "pythonExecution");
        if (str_eq(role, "toolResult") || bash || py) {
            const char *tool = bash ? "bash" : py ? "python" : json_get_str(x, "toolName") ? json_get_str(x, "toolName") : "";
            char *text = bash ? out_bash_execution_text(x) : py ? out_python_execution_text(x) : summarize_content(json_get(x, "content"));
            char *id = xasprintf("tool-output/%s", prefix);
            centry_t *e = entry_new(d, -1, id, tool, text, tok(text));
            e->ts = ts;
            if (bash || py) e->info = xasprintf("%s \xe2\x86\x92 user", role);
            else if (json_get_str(x, "toolCallId")) e->call_id = xstrdup(json_get_str(x, "toolCallId"));
            tool_output(d, tool, e);
            free(id);
            free(text);
            continue;
        }
        const char *conv = str_eq(role, "developer") ? "developer" : "user";
        char *text = summarize_content(json_get(x, "content"));
        char *id = xasprintf("user-messages/%s", prefix), *label = xasprintf("%s \xc2\xb7 message %zu", conv, i + 1);
        centry_t *e = entry_new(d, C_USER, id, label, text, tok(text));
        e->ts = ts;
        const char *ct = json_get_str(x, "customType");
        if (!str_eq(role, conv) || ct)
            e->info = xasprintf("%s%s%s%s%s", role ? role : "?", ct ? " \xc2\xb7 " : "", ct ? ct : "",
                                str_eq(role, conv) ? "" : " \xe2\x86\x92 ", str_eq(role, conv) ? "" : conv);
        free(id);
        free(label);
        free(text);
    }
    /* tool output: chronological entries, groups heaviest first */
    ccat_t *out = &d->cats[C_OUTPUT];
    for (size_t i = 1; i < out->children.n; i++) /* stable insertion sort by tokens desc */
        for (size_t k = i; k > 0 && out->children.v[k]->tokens > out->children.v[k - 1]->tokens; k--) {
            ccat_t *t = out->children.v[k];
            out->children.v[k] = out->children.v[k - 1];
            out->children.v[k - 1] = t;
        }
    for (size_t i = 1; i < out->entries.n; i++)
        for (size_t k = i; k > 0 && out->entries.v[k]->ts < out->entries.v[k - 1]->ts; k--) {
            centry_t *t = out->entries.v[k];
            out->entries.v[k] = out->entries.v[k - 1];
            out->entries.v[k - 1] = t;
        }

    /* breakdown (computeContextBreakdown over SessionStats.getContextBreakdown) */
    nonmsg = MAX(0, sys0 - skills) + sys_ctx + skills + tools_tok;
    (void)nonmsg;
    d->window = a->model ? a->model->context_window : 0;
    d->used = ui_context_used(a);
    d->threshold = -1;
    int64_t buf = 0;
    if (d->window > 0) {
        bool enabled = settings_bool("compaction.enabled", true);
        if (enabled && !str_eq(settings_str("compaction.strategy", ""), "off")) {
            int64_t th = app_compaction_threshold(a->model, d->window);
            if (th > 0 && th <= d->window) d->threshold = th;
            buf = MAX(0, d->window - th);
        }
        if (buf == 0 && enabled)
            buf = MAX((int64_t)floor(d->window * 0.15), (int64_t)settings_int("compaction.reserveTokens", 16384));
    }
    d->buffer = MIN(buf, MAX(0, d->window - d->used));
    d->free_ = MAX(0, d->window - d->used - d->buffer);
    d->model_label = xstrdup(a->model ? (a->model->name && *a->model->name ? a->model->name : a->model->id) : "no model");
    json_free(sys);
    json_free(msgs);
    json_free(tools);
}

static void cdata_free(cdata_t *d) {
    for (int i = 0; i < C_N; i++) {
        ccat_t *c = &d->cats[i];
        for (size_t k = 0; k < c->children.n; k++) {
            vec_free(&c->children.v[k]->entries);
            free(c->children.v[k]->key);
            free(c->children.v[k]->label);
            free(c->children.v[k]);
        }
        vec_free(&c->children);
        vec_free(&c->entries);
        free(c->label);
    }
    for (size_t i = 0; i < d->all.n; i++) {
        centry_t *e = d->all.v[i];
        free(e->id), free(e->label), free(e->text), free(e->info), free(e->path), free(e->call_id);
        free(e);
    }
    vec_free(&d->all);
    for (size_t i = 0; i < d->groups.n; i++) {
        free(d->groups.v[i]->id), free(d->groups.v[i]->label);
        free(d->groups.v[i]);
    }
    vec_free(&d->groups);
    free(d->model_label);
}

/* ---------- formatting (context-viewer-format.ts) ---------- */

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static char *commas(int64_t v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)(v < 0 ? -v : v));
    sb_t b = SB_INIT;
    if (v < 0) sb_putc(&b, '-');
    size_t l = strlen(buf);
    for (size_t i = 0; i < l; i++) {
        if (i && (l - i) % 3 == 0) sb_putc(&b, ',');
        sb_putc(&b, buf[i]);
    }
    return sb_detach(&b);
}

/* normalized newlines, control characters removed, tabs expanded */
static char *safe_content(const char *v) {
    size_t n = 0;
    char *s = out_sanitize_lines(v ? v : "", strlen(v ? v : ""), &n);
    char *r = tc_replace_tabs(s);
    free(s);
    return r;
}

static char *shorten_home(const char *s) {
    const char *home = home_dir();
    size_t hl = home ? strlen(home) : 0;
    if (!hl || (hl == 1 && home[0] == '/')) return xstrdup(s);
    sb_t b = SB_INIT;
    for (const char *p = s; *p;) {
        if (!strncmp(p, home, hl) && (p[hl] == '/' || p[hl] == 0 || isspace((unsigned char)p[hl]))) {
            sb_putc(&b, '~');
            p += hl;
        } else {
            sb_putc(&b, *p++);
        }
    }
    return sb_detach(&b);
}

static char *safe_text(const char *v) {
    char *c = safe_content(v);
    char *r = shorten_home(c);
    free(c);
    return r;
}

static char *safe_line(const char *v) {
    char *s = safe_text(v);
    for (char *p = s; *p; p++)
        if (*p == '\n') *p = ' ';
    return s;
}

static char *token_estimate(const centry_t *e, const cgroup_t *g) {
    if (e && e->group) return xstrdup("\xe2\x80\x94");
    bool interrupted = e ? e->interrupted : g->interrupted;
    if (interrupted) return xstrdup("N/A \xc2\xb7 Interrupted");
    int64_t t = e ? e->tokens : g->tokens;
    char *c = commas(t), *r = xasprintf("%s %s", c, t == 1 ? "token" : "tokens");
    free(c);
    return r;
}

static char *entry_header(const centry_t *e, int cat) {
    char *lab = safe_line(e->label), *b = tui_bold(lab), *name = S(PRES[cat].color, b), *est = token_estimate(e, NULL);
    char *tail = xasprintf("] %s", est);
    char *l = S(TUI_TC_DIM, "["), *r = S(TUI_TC_DIM, tail);
    char *out = xasprintf("%s%s%s", l, name, r);
    free(lab), free(b), free(name), free(est), free(tail), free(l), free(r);
    return out;
}

static char *group_header(const cgroup_t *g, int cat) {
    char *lab = safe_line(g->label), *b = tui_bold(lab), *name = S(PRES[cat].color, b), *est = token_estimate(NULL, g);
    char *tail = xasprintf(" \xc2\xb7 %s", est), *r = S(TUI_TC_DIM, tail);
    char *out = xasprintf("%s%s", name, r);
    free(lab), free(b), free(name), free(est), free(tail), free(r);
    return out;
}

static char *preview_hint(bool selected) {
    char *more = S(TUI_TC_DIM, "  \xe2\x80\xa6 more");
    if (!selected) return more;
    char *k = ui_editor_key("tui.select.confirm");
    if (!*k) {
        free(k);
        return more;
    }
    char *dot = S(TUI_TC_DIM, " \xc2\xb7 "), *v = xasprintf("%s View Content", k), *va = S(TUI_TC_ACCENT, v);
    char *r = xasprintf("%s%s%s", more, dot, va);
    free(more), free(dot), free(v), free(va), free(k);
    return r;
}

/* toLocaleString([], {year numeric, month/day/hour/minute 2-digit}) in en-US */
static char *fmt_ts(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    int h = tm.tm_hour % 12;
    return xasprintf("%02d/%02d/%04d, %02d:%02d %s", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900, h ? h : 12, tm.tm_min,
                     tm.tm_hour < 12 ? "AM" : "PM");
}

static void entry_metadata(const centry_t *e, bool with_group, strvec_t *out) {
    if (e->group && with_group) {
        char *l = safe_line(e->group->label), *est = token_estimate(NULL, e->group);
        strvec_push_own(out, xasprintf("%s \xc2\xb7 %s", l, est));
        free(l), free(est);
    }
    sb_t b = SB_INIT;
    if (e->info && *e->info) sb_append(&b, e->info);
    if (e->path && *e->path) {
        char *p = tc_shorten_path(e->path);
        sb_appendf(&b, "%s%s", b.len ? " \xc2\xb7 " : "", p);
        free(p);
    }
    char *info = safe_text(sb_cstr(&b));
    sb_free(&b);
    if (*info) strvec_push(out, info);
    free(info);
    if (e->call_id) {
        char *c = safe_text(e->call_id);
        if (*c) strvec_push(out, c);
        free(c);
    }
    if (e->ts) strvec_push_own(out, fmt_ts(e->ts));
}

static int preview_rows(const centry_t *e, int height) { return MAX(1, MIN(10, height - (e->group ? 3 : 2))); }

/* cut at a UTF-8 boundary at most n bytes in */
static size_t utf8_cut(const char *s, size_t n) {
    size_t l = strlen(s);
    if (n >= l) return l;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

static bool entry_preview(const centry_t *e, int width, int rows, int source_width, strvec_t *out) {
    size_t max_chars = (size_t)MAX(1, source_width) * (size_t)rows * 2;
    strvec_t meta = {0};
    entry_metadata(e, false, &meta);
    char *metadata = strvec_join(&meta, "\n");
    strvec_free(&meta);
    size_t ml = utf8_cut(metadata, MIN(strlen(metadata), max_chars));
    char *body = tc_trim_end(e->text);
    size_t room = max_chars > ml + (ml ? 1 : 0) ? max_chars - ml - (ml ? 1 : 0) : 0;
    size_t bl = utf8_cut(body, MIN(strlen(body), room));
    char *mpart = xstrndup(metadata, ml), *bpart = xstrndup(body, bl);
    char *values[2] = {safe_content(mpart), safe_content(bpart)};
    const char *prefixes[2] = {"", "  "};
    bool complete = ml == strlen(metadata) && bl == strlen(body), cut = false;
    size_t start = out->n;
    for (int p = 0; p < 2 && !cut; p++) {
        if (!*values[p]) continue;
        strvec_t paras = str_split_lines(values[p], strlen(values[p]));
        if (!paras.n) strvec_push(&paras, "");
        for (size_t k = 0; k < paras.n && !cut; k++) {
            int remaining = rows - (int)(out->n - start);
            if (remaining == 0) {
                cut = true;
                break;
            }
            strvec_t w = {0};
            if (*paras.v[k]) {
                char *st = p == 0 ? S(TUI_TC_DIM, paras.v[k]) : xstrdup(paras.v[k]);
                tui_wrap_text_with_ansi(st, MAX(1, width - (int)strlen(prefixes[p])), &w);
                free(st);
            } else {
                strvec_push(&w, "");
            }
            for (size_t j = 0; j < w.n && (int)j < remaining; j++) strvec_push_own(out, xasprintf("%s%s", prefixes[p], w.v[j]));
            if ((int)w.n > remaining) cut = true;
            strvec_free(&w);
        }
        strvec_free(&paras);
    }
    free(values[0]), free(values[1]), free(mpart), free(bpart), free(metadata), free(body);
    return complete && !cut;
}

/* ---------- ScrollView over lines (components/scroll-view.ts, scrollbar auto, unstyled) ---------- */

typedef struct {
    int offset, height;
    char *anchor; /* last revealed range signature */
} sv_t;

static void sv_clamp(sv_t *v, int total) { v->offset = MAX(0, MIN(v->offset, MAX(0, total - v->height))); }

/* revealRange: move only when the anchor changed (manual scrolling survives unchanged renders) */
static void sv_reveal(sv_t *v, const char *id, int start, int end, int total, int width, bool oversized_start) {
    char *sig = xasprintf("%s|%d|%d|%d|%d|%d", id, start, end, total, width, v->height);
    if (v->anchor && !strcmp(v->anchor, sig)) {
        free(sig);
        return;
    }
    free(v->anchor);
    v->anchor = sig;
    int top = MAX(0, start), bottom = MIN(total, end);
    if (top < v->offset || (oversized_start && bottom - top >= v->height)) v->offset = top;
    else if (bottom > v->offset + v->height) v->offset = MAX(top, bottom - 1) - v->height + 1;
    sv_clamp(v, total);
}

static void sv_render(sv_t *v, const strvec_t *lines, int width, strvec_t *out) {
    int total = (int)lines->n, h = v->height;
    sv_clamp(v, total);
    bool bar = total > h && h > 0 && width > 0;
    int cw = MAX(0, width - (bar ? 1 : 0));
    int ts = 0, te = h;
    if (bar) {
        int size = MAX(1, MIN((h * h) / total, h)), maxoff = total - h;
        ts = maxoff ? (int)lround((double)v->offset / maxoff * (h - size)) : 0;
        te = ts + size;
    }
    for (int r = 0; r < h; r++) {
        int i = v->offset + r;
        char *rt = tui_replace_tabs(i < total ? lines->v[i] : "");
        char *tr = tui_truncate_to_width(rt, cw, NULL);
        free(rt);
        if (!bar) {
            strvec_push_own(out, tr);
            continue;
        }
        char *pad = tui_pad_to_width(tr, cw);
        strvec_push_own(out, xasprintf("%s%s", pad, r >= ts && r < te ? "\xe2\x96\x88" : "\xe2\x94\x82"));
        free(pad);
        free(tr);
    }
}

/* ---------- overview model (context-viewer-overview.ts) ---------- */

typedef struct {
    char *key, *label, *tokcol, *sharecol;
    int pid;
    int64_t tokens; /* < 0: unattributed */
    ccat_t *cat;    /* opened on confirm; NULL: listed only */
    bool child;
} orow_t;

typedef struct {
    tui_component_t base;
    ui_t *u;
    cdata_t d;
    int64_t alloc[C_N]; /* attributed usage per category */
    VEC(int64_t) child_alloc; /* tool-output children, in order */
    bool has_alloc, drawable;
    strvec_t notes;
    VEC(orow_t) rows;
    int osel;
    int state; /* 0 overview, 1 category, 2 entry */
    ccat_t *cat;
    int esel;
    centry_t *entry;
    int entry_parent;
    int body_width, body_rows;
    sv_t ov, pv, ev;
    /* category preview cache */
    strvec_t pv_lines;
    VEC(int) pv_start;
    VEC(int) pv_end;
    VEC(bool) pv_complete;
    ccat_t *pv_cat;
    int pv_w, pv_h;
    strvec_t ev_lines;
    int ev_w;
} cview_t;

/* largest-remainder split of total by weights; false when the weights cannot scale */
static bool allocate(const int64_t *w, size_t n, int64_t total, int64_t *out) {
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)w[i];
    if (!(sum > 0)) return false;
    double *rem = xcalloc(n ? n : 1, sizeof *rem);
    int64_t given = 0;
    for (size_t i = 0; i < n; i++) {
        double exact = (double)total * (double)w[i] / sum;
        out[i] = (int64_t)floor(exact);
        rem[i] = exact - floor(exact);
        given += out[i];
    }
    for (int64_t k = 0; k < total - given; k++) {
        size_t best = 0;
        double br = -1;
        for (size_t i = 0; i < n; i++)
            if (rem[i] > br) best = i, br = rem[i];
        out[best]++;
        rem[best] = -2;
    }
    free(rem);
    return true;
}

static char *fmt_share(int64_t tokens, int64_t window) {
    if (window <= 0) return xstrdup("\xe2\x80\x94");
    double p = (double)tokens / (double)window * 100;
    return p > 0 && p < 0.05 ? xstrdup("<0.1%") : xasprintf("%.1f%%", p);
}

static char *pct_string(int64_t part, int64_t whole, int digits) {
    if (whole <= 0) return xstrdup("0%");
    double p = (double)part / (double)whole * 100;
    if (p > 0 && p < 0.05) return xstrdup("<0.1%");
    return xasprintf("%.*f%%", digits, p);
}

static void add_row(cview_t *v, const char *key, int pid, const char *label, int64_t tokens, ccat_t *cat, bool child) {
    orow_t r = {xstrdup(key), xstrdup(label), NULL, NULL, pid, tokens, cat, child};
    vec_push(&v->rows, r);
}

static void build_rows(cview_t *v) {
    cdata_t *d = &v->d;
    for (int i = 0; i < C_N; i++) {
        ccat_t *c = &d->cats[i];
        bool openable = PRES[i].openable;
        add_row(v, PRES[i].id, i, PRES[i].overview ? PRES[i].overview : c->label, v->has_alloc ? v->alloc[i] : -1,
                openable && c->entries.n ? c : NULL, false);
        for (size_t k = 0; k < c->children.n; k++)
            add_row(v, c->children.v[k]->key, i, c->children.v[k]->label,
                    v->has_alloc && k < v->child_alloc.n ? v->child_alloc.v[k] : -1, openable ? c->children.v[k] : NULL, true);
    }
    add_row(v, "buffer", P_BUFFER, PRES[P_BUFFER].label, d->buffer, NULL, false);
    add_row(v, "free", P_FREE, PRES[P_FREE].label, d->free_, NULL, false);
    int tw = 1, sw = 1;
    for (size_t i = 0; i < v->rows.n; i++) {
        orow_t *r = &v->rows.v[i];
        if (r->tokens < 0) continue;
        char *n = ui_format_number((double)r->tokens), *s = fmt_share(r->tokens, d->window);
        tw = MAX(tw, (int)strlen(n));
        sw = MAX(sw, tui_visible_width(s));
        free(n), free(s);
    }
    for (size_t i = 0; i < v->rows.n; i++) {
        orow_t *r = &v->rows.v[i];
        if (r->tokens < 0) {
            r->tokcol = xstrdup("\xe2\x80\x94");
            r->sharecol = xstrdup("");
            continue;
        }
        char *n = ui_format_number((double)r->tokens), *s = fmt_share(r->tokens, d->window);
        r->tokcol = xasprintf("%*s", tw, n);
        r->sharecol = xasprintf("  %*s", sw + (int)strlen(s) - tui_visible_width(s), s);
        free(n), free(s);
    }
}

static char *legend_line(const orow_t *r, bool selected, int width) {
    tui_tc_t color = PRES[r->pid].color;
    char *glyph = S(color, tui_sym(PRES[r->pid].symbol));
    const char *cur = tui_sym("nav.cursor");
    char *cursor = selected ? S(TUI_TC_ACCENT, cur) : xasprintf("%*s", tui_visible_width(cur), "");
    char *prefix = xasprintf("%s %s%s ", cursor, r->child ? "  " : "", glyph);
    char *tc = S(selected ? color : TUI_TC_TEXT, r->tokcol), *sc = S(selected ? color : TUI_TC_DIM, r->sharecol);
    char *suffix = xasprintf(" %s%s", tc, sc);
    int budget = MAX(0, width - tui_visible_width(prefix) - tui_visible_width(suffix));
    char *sl = safe_line(r->label), *label = tui_truncate_to_width(sl, budget, NULL);
    char *name;
    if (selected) {
        char *b = tui_bold(label);
        name = S(color, b);
        free(b);
    } else {
        name = xstrdup(label);
    }
    char *line = xasprintf("%s%s%*s%s", prefix, name, MAX(0, budget - tui_visible_width(label)), "", suffix);
    char *out = tui_truncate_to_width(line, width, NULL);
    free(glyph), free(cursor), free(prefix), free(tc), free(sc), free(suffix), free(sl), free(label), free(name), free(line);
    return out;
}

/* ---------- viewer ---------- */

static bool row_enabled(const cview_t *v, int i) { return i >= 0 && i < (int)v->rows.n && v->rows.v[i].cat; }

static bool osel_move(cview_t *v, int delta) {
    for (int i = v->osel + delta; i >= 0 && i < (int)v->rows.n; i += delta)
        if (row_enabled(v, i)) {
            v->osel = i;
            return true;
        }
    return false;
}

static void summary_lines(cview_t *v, strvec_t *out) {
    if (v->state != 0) return;
    cdata_t *d = &v->d;
    char *used = ui_format_number((double)d->used), *win = ui_format_number((double)d->window), *pct = pct_string(d->used, d->window, 0);
    strvec_push_own(out, xasprintf("Used %s of %s tokens \xc2\xb7 %s", used, win, pct));
    if (d->threshold < 0) {
        char *f = ui_format_number((double)d->free_);
        strvec_push_own(out, xasprintf("%s free", f));
        free(f);
    } else {
        char *at = pct_string(d->threshold, d->window, 0);
        if (d->used >= d->threshold) strvec_push_own(out, xasprintf("Past auto-compaction at %s", at));
        else {
            char *left = ui_format_number((double)(d->threshold - d->used));
            strvec_push_own(out, xasprintf("%s left before auto-compaction at %s", left, at));
            free(left);
        }
        free(at);
    }
    free(used), free(win), free(pct);
}

static void render_overview(cview_t *v, strvec_t *out) {
    int width = v->body_width, height = v->body_rows;
    cdata_t *d = &v->d;
    /* mapGeometry */
    bool map = false;
    int columns = 0, mrows = 0, cell = 1;
    const char *gap = "";
    if (v->drawable && width >= 52 && height >= 2) {
        gap = width >= 72 ? " " : "";
        for (int p = 0; p <= P_FREE; p++) cell = MAX(cell, tui_visible_width(tui_sym(PRES[p].symbol)));
        int g = (int)strlen(gap);
        columns = MIN(16, (width - 32 - 3 + g) / (cell + g));
        mrows = MIN(16, height);
        map = columns >= 1;
    }
    int mapw = map ? columns * (cell + (int)strlen(gap)) - (int)strlen(gap) : 0;
    int list_w = map ? width - mapw - 3 : width;
    strvec_t legend = {0};
    for (size_t i = 0; i < v->rows.n; i++) strvec_push_own(&legend, legend_line(&v->rows.v[i], (int)i == v->osel, MAX(0, list_w - 1)));
    for (size_t i = 0; i < v->notes.n; i++) {
        char *dn = S(TUI_TC_DIM, v->notes.v[i]);
        tui_wrap_text_with_ansi(dn, MAX(1, list_w - 1), &legend);
        free(dn);
    }
    v->ov.height = height;
    sv_reveal(&v->ov, v->osel >= 0 ? v->rows.v[v->osel].key : "", v->osel, v->osel + 1, (int)legend.n, list_w, false);
    strvec_t shown = {0};
    sv_render(&v->ov, &legend, list_w, &shown);
    strvec_free(&legend);
    if (!map) {
        for (size_t i = 0; i < shown.n; i++) strvec_push(out, shown.v[i]);
        strvec_free(&shown);
        return;
    }
    /* planContextMap: categories, free space, then buffer laid over the grid in window order */
    int ncell = columns * mrows;
    double per = (double)d->window / ncell;
    struct {
        int id;
        double start, end;
    } ranges[C_N + 2];
    int nr = 0;
    double cursor = 0;
    for (int i = 0; i < C_N + 2; i++) {
        int id = i < C_N ? i : i == C_N ? P_FREE : P_BUFFER;
        double t = i < C_N ? (double)v->alloc[i] : id == P_FREE ? (double)d->free_ : (double)d->buffer;
        if (!(t > 0) || cursor >= d->window) continue;
        double end = MIN((double)d->window, cursor + t);
        if (end > cursor) ranges[nr].id = id, ranges[nr].start = cursor, ranges[nr].end = end, nr++;
        cursor = end;
    }
    for (int r = 0; r < height; r++) {
        sb_t g = SB_INIT;
        if (r < mrows)
            for (int c = 0; c < columns; c++) {
                int idx = r * columns + c;
                double s = idx * per, e = (idx + 1) * per, best = 0;
                int win = -1;
                for (int k = 0; k < nr; k++) {
                    double cov = MIN(e, ranges[k].end) - MAX(s, ranges[k].start);
                    if (cov > best) best = cov, win = ranges[k].id;
                }
                if (win < 0) continue;
                if (c) sb_append(&g, gap);
                const char *glyph = tui_sym(PRES[win].symbol);
                char *painted = S(PRES[win].color, glyph);
                if (best / per >= 0.7) sb_append(&g, painted);
                else sb_appendf(&g, "\x1b[2m%s\x1b[22m", painted);
                free(painted);
                for (int k = tui_visible_width(glyph); k < cell; k++) sb_putc(&g, ' ');
            }
        int pad = MAX(0, mapw - tui_visible_width(sb_cstr(&g))) + 3;
        strvec_push_own(out, xasprintf("%s%*s%s", sb_cstr(&g), pad, "", r < (int)shown.n ? shown.v[r] : ""));
        sb_free(&g);
    }
    strvec_free(&shown);
}

static void prepare_preview(cview_t *v, int width, int height) {
    v->pv.height = height;
    if (v->pv_cat == v->cat && v->pv_w == width && v->pv_h == height) return;
    strvec_free(&v->pv_lines);
    v->pv_start.n = v->pv_end.n = v->pv_complete.n = 0;
    strvec_t seen = {0};
    int cat = v->cat->id;
    for (size_t i = 0; i < v->cat->entries.n; i++) {
        centry_t *e = v->cat->entries.v[i];
        int start = (int)v->pv_lines.n;
        const char *indent = e->group ? "  " : "";
        if (e->group && !strvec_contains(&seen, e->group->id)) {
            strvec_push(&seen, e->group->id);
            char *gh = group_header(e->group, cat);
            strvec_push_own(&v->pv_lines, tui_truncate_to_width(gh, MAX(1, width - 1), NULL));
            free(gh);
        }
        int ew = MAX(1, width - 1 - (int)strlen(indent));
        char *hd = entry_header(e, cat), *tr = tui_truncate_to_width(hd, ew, NULL);
        strvec_push_own(&v->pv_lines, xasprintf("%s%s", indent, tr));
        free(hd), free(tr);
        strvec_t pl = {0};
        bool complete = entry_preview(e, ew, preview_rows(e, v->body_rows), v->body_width, &pl);
        for (size_t k = 0; k < pl.n; k++) strvec_push_own(&v->pv_lines, xasprintf("%s%s", indent, pl.v[k]));
        strvec_free(&pl);
        if (!complete) strvec_push_own(&v->pv_lines, preview_hint(false));
        vec_push(&v->pv_start, start);
        vec_push(&v->pv_end, (int)v->pv_lines.n);
        vec_push(&v->pv_complete, complete);
        strvec_push(&v->pv_lines, "");
    }
    strvec_free(&seen);
    v->pv_cat = v->cat, v->pv_w = width, v->pv_h = height;
}

static void render_category(cview_t *v, strvec_t *out) {
    int cw = MAX(1, v->body_width - 2);
    prepare_preview(v, cw, v->body_rows);
    int sel = v->esel;
    bool has = sel >= 0 && sel < (int)v->pv_start.n;
    int rs = has ? v->pv_start.v[sel] : 0, re = has ? v->pv_end.v[sel] : 0;
    if (has) sv_reveal(&v->pv, v->cat->entries.v[sel]->id, rs, re, (int)v->pv_lines.n, cw, true);
    strvec_t shown = {0};
    sv_render(&v->pv, &v->pv_lines, cw, &shown);
    for (size_t i = 0; i < shown.n; i++) {
        int at = v->pv.offset + (int)i;
        bool selected = has && at >= rs && at < re;
        char *line = xstrdup(shown.v[i]);
        if (selected && !v->pv_complete.v[sel] && at == re - 1) {
            free(line);
            char *h = preview_hint(true);
            line = tui_truncate_to_width(h, cw - 1, NULL);
            free(h);
        }
        char *g = selected ? S(TUI_TC_ACCENT, "\xe2\x94\x83 ") : xstrdup("  ");
        strvec_push_own(out, xasprintf("%s%s", g, line));
        free(g);
        free(line);
    }
    strvec_free(&shown);
}

static void ensure_entry_layout(cview_t *v) {
    int width = v->body_width;
    v->ev.height = v->body_rows;
    if (v->ev_w == width) return;
    strvec_free(&v->ev_lines);
    int cw = MAX(1, width - 1);
    centry_t *e = v->entry;
    char *hd = entry_header(e, v->cat->id);
    strvec_push_own(&v->ev_lines, tui_truncate_to_width(hd, cw, NULL));
    free(hd);
    strvec_t meta = {0};
    entry_metadata(e, true, &meta);
    for (size_t i = 0; i < meta.n; i++) {
        char *dm = S(TUI_TC_DIM, meta.v[i]);
        tui_wrap_text_with_ansi(dm, cw, &v->ev_lines);
        free(dm);
    }
    strvec_free(&meta);
    char *body = tc_trim_end(e->text), *safe = safe_content(body);
    strvec_t ls = str_split_lines(safe, strlen(safe));
    for (size_t i = 0; i < ls.n; i++) {
        if (*ls.v[i]) tui_wrap_text_with_ansi(ls.v[i], cw, &v->ev_lines);
        else strvec_push(&v->ev_lines, "");
    }
    strvec_free(&ls);
    free(body), free(safe);
    v->ev_w = width;
}

static char *footer_hints(cview_t *v) {
    char *up = ui_editor_key("tui.select.up"), *down = ui_editor_key("tui.select.down");
    char *nav = *up && *down ? xasprintf("%s/%s", up, down) : xstrdup(*up ? up : down);
    bool can_open = v->state == 0 ? row_enabled(v, v->osel) : v->state == 1 && v->esel >= 0 && v->esel < (int)v->cat->entries.n;
    char *confirm = can_open ? ui_editor_key("tui.select.confirm") : xstrdup("");
    char *cancel = ui_editor_key("tui.select.cancel");
    const char *movement = v->state == 0 ? "select/scroll" : v->state == 1 ? "select" : "scroll";
    strvec_t parts = {0};
    if (*nav) strvec_push_own(&parts, xasprintf("%s %s", nav, movement));
    if (v->state == 2) {
        char *pu = ui_editor_key("tui.select.pageUp"), *pd = ui_editor_key("tui.select.pageDown");
        if (*pu || *pd) strvec_push_own(&parts, *pu && *pd ? xasprintf("%s/%s page", pu, pd) : xasprintf("%s page", *pu ? pu : pd));
        free(pu), free(pd);
    }
    if (*confirm) strvec_push_own(&parts, xasprintf("%s open", confirm));
    if (*cancel) strvec_push_own(&parts, xasprintf("%s %s", cancel, v->state == 0 ? "close" : "back"));
    char *r = strvec_join(&parts, " \xc2\xb7 ");
    strvec_free(&parts);
    free(up), free(down), free(nav), free(confirm), free(cancel);
    return r;
}

static void cview_render(tui_component_t *c, int width, strvec_t *out) {
    cview_t *v = (cview_t *)c;
    v->body_width = MAX(1, width - 4);
    int rows = MAX(1, tui_term(v->u->tui)->rows);
    strvec_t summary = {0};
    summary_lines(v, &summary);
    while ((int)summary.n > MAX(0, rows - 5)) free(summary.v[--summary.n]);
    v->body_rows = MAX(0, rows - (int)summary.n - 4);
    char *title;
    if (v->state == 0) {
        char *ml = safe_text(v->d.model_label);
        title = xasprintf("Context \xc2\xb7 %s", ml);
        free(ml);
    } else {
        title = safe_text(v->cat->label);
    }
    for (char *q = title; *q; q++)
        if (*q == '\n') *q = ' ';
    strvec_t lines = {0};
    strvec_push_own(&lines, ui_box_top(width, title));
    free(title);
    for (size_t i = 0; i < summary.n; i++) {
        char *dl = S(TUI_TC_DIM, summary.v[i]), *t = tui_truncate_to_width(dl, v->body_width, NULL);
        strvec_push_own(&lines, ui_box_row(t, width));
        free(dl), free(t);
    }
    strvec_free(&summary);
    strvec_t body = {0};
    if (v->state == 0) render_overview(v, &body);
    else if (v->state == 1) render_category(v, &body);
    else {
        ensure_entry_layout(v);
        sv_render(&v->ev, &v->ev_lines, v->body_width, &body);
    }
    for (int i = 0; i < v->body_rows; i++) {
        const char *l = i < (int)body.n ? body.v[i] : "";
        char *t = tui_visible_width(l) > v->body_width ? tui_truncate_to_width(l, v->body_width, NULL) : xstrdup(l);
        strvec_push_own(&lines, ui_box_row(t, width));
        free(t);
    }
    strvec_free(&body);
    strvec_push_own(&lines, ui_box_divider(width));
    char *hints = footer_hints(v), *dh = S(TUI_TC_DIM, hints), *th = tui_truncate_to_width(dh, v->body_width, NULL);
    strvec_push_own(&lines, ui_box_row(th, width));
    free(hints), free(dh), free(th);
    strvec_push_own(&lines, ui_box_bottom(width));
    /* short terminals keep the top border, footer and bottom border, in that priority */
    size_t n = lines.n;
    if (rows >= 4) {
        for (size_t i = 0; i < n; i++) strvec_push(out, lines.v[i]);
    } else if (rows == 3) {
        strvec_push(out, lines.v[0]), strvec_push(out, lines.v[n - 2]), strvec_push(out, lines.v[n - 1]);
    } else if (rows == 2) {
        strvec_push(out, lines.v[0]), strvec_push(out, lines.v[n - 1]);
    } else {
        strvec_push(out, lines.v[n - 1]);
    }
    strvec_free(&lines);
}

static void open_entry(cview_t *v, centry_t *e, int parent) {
    v->entry = e;
    v->entry_parent = parent;
    v->ev_w = -1;
    v->ev.offset = 0;
    v->state = 2;
}

static void activate(cview_t *v) {
    if (v->state == 0) {
        if (!row_enabled(v, v->osel)) return;
        v->cat = v->rows.v[v->osel].cat;
        v->esel = v->cat->entries.n ? 0 : -1;
        v->pv_cat = NULL;
        v->pv.offset = 0;
        free(v->pv.anchor);
        v->pv.anchor = NULL;
        if (v->cat->entries.n == 1) open_entry(v, v->cat->entries.v[0], 0);
        else v->state = 1;
    } else if (v->state == 1) {
        if (v->esel >= 0 && v->esel < (int)v->cat->entries.n) open_entry(v, v->cat->entries.v[v->esel], 1);
    }
}

static void back(cview_t *v) {
    if (v->state == 2) {
        v->state = v->entry_parent;
        v->entry = NULL;
        strvec_free(&v->ev_lines);
        v->ev_w = -1;
    } else if (v->state == 1) {
        v->state = 0;
    } else {
        ui_close_sheet(v->u);
    }
}

static bool cview_key(tui_component_t *c, const tui_key_t *k) {
    cview_t *v = (cview_t *)c;
    ui_t *u = v->u;
    if (tui_kb(k, "tui.select.cancel")) {
        if (v->state == 0) {
            ui_close_sheet(u); /* destroys v */
            return true;
        }
        back(v);
    } else if (tui_kb(k, "tui.select.confirm")) {
        activate(v);
    } else if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) {
        int delta = tui_kb(k, "tui.select.up") ? -1 : 1;
        if (v->state == 2) {
            ensure_entry_layout(v);
            v->ev.offset += delta;
            sv_clamp(&v->ev, (int)v->ev_lines.n);
        } else if (v->state == 0) {
            if (!osel_move(v, delta)) v->ov.offset += delta; /* clamped on render */
        } else {
            int n = (int)v->cat->entries.n;
            if (v->esel + delta >= 0 && v->esel + delta < n) v->esel += delta;
        }
    } else if (tui_kb(k, "tui.select.pageUp") || tui_kb(k, "tui.select.pageDown")) {
        if (v->state == 2) {
            ensure_entry_layout(v);
            v->ev.offset += (tui_kb(k, "tui.select.pageUp") ? -1 : 1) * MAX(1, v->body_rows - 1);
            sv_clamp(&v->ev, (int)v->ev_lines.n);
        }
    }
    tui_request_render(u->tui);
    return true;
}

static void cview_destroy(tui_component_t *c) {
    cview_t *v = (cview_t *)c;
    cdata_free(&v->d);
    for (size_t i = 0; i < v->rows.n; i++) free(v->rows.v[i].key), free(v->rows.v[i].label), free(v->rows.v[i].tokcol), free(v->rows.v[i].sharecol);
    vec_free(&v->rows);
    vec_free(&v->child_alloc);
    strvec_free(&v->notes);
    strvec_free(&v->pv_lines);
    vec_free(&v->pv_start);
    vec_free(&v->pv_end);
    vec_free(&v->pv_complete);
    strvec_free(&v->ev_lines);
    free(v->ov.anchor), free(v->pv.anchor), free(v->ev.anchor);
    free(v);
}

static const tui_component_vt CVIEW_VT = {"context-viewer", cview_render, cview_key, NULL, cview_destroy};

void ui_context_command(ui_t *u) {
    app_t *a = u->app;
    if (!a->model) {
        add_notice(u, "warning", "Context usage is unavailable: no model is selected for this session.");
        return;
    }
    if (!(a->model->context_window > 0)) {
        add_notice(u, "warning", "Context usage is unavailable: the model's context window is unknown or invalid.");
        return;
    }
    cview_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &CVIEW_VT);
    v->u = u;
    v->ev_w = -1;
    build(&v->d, a);
    /* allocateUsage: Used split by category weight, each category's share by its children */
    int64_t w[C_N];
    for (int i = 0; i < C_N; i++) w[i] = v->d.cats[i].tokens;
    v->has_alloc = allocate(w, C_N, v->d.used, v->alloc);
    ccat_t *out = &v->d.cats[C_OUTPUT];
    if (v->has_alloc && out->children.n) {
        int64_t *cw = xcalloc(out->children.n, sizeof *cw), *ca = xcalloc(out->children.n, sizeof *ca);
        for (size_t i = 0; i < out->children.n; i++) cw[i] = out->children.v[i]->tokens;
        if (allocate(cw, out->children.n, v->alloc[C_OUTPUT], ca))
            for (size_t i = 0; i < out->children.n; i++) vec_push(&v->child_alloc, ca[i]);
        free(cw), free(ca);
    }
    v->drawable = v->has_alloc && v->d.window > 0;
    if (v->has_alloc) strvec_push(&v->notes, "Category totals split Used by estimated share; the entries inside may not add up to them.");
    build_rows(v);
    v->osel = -1;
    for (size_t i = 0; i < v->rows.n; i++)
        if (row_enabled(v, (int)i)) {
            v->osel = (int)i;
            break;
        }
    if (v->osel < 0) v->osel = 0;
    ui_show_sheet(u, &v->base);
}
