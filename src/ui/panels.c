/* panels.c - /session, /tools and /jobs (ports of omp CommandController.handleSessionCommand / handleToolsCommand /
 * handleJobsCommand, overlays/session-info-overlay.ts, overlays/job-tree.ts, prompt/tools-markdown.ts and the
 * builtin-session.ts `/session` and `/jobs` subcommands). */
#include "ui/ui.h"
#include "ui/tools/tc.h"

#include "config/auth.h"
#include "config/settings.h"
#include "mcp/mcp.h"
#include "tools/jobs.h"
#include "tools/outsink.h"
#include "tools/xdev.h"

#include <ctype.h>
#include <math.h>
#include <unistd.h>

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static void kv(sb_t *b, const char *key, const char *value) {
    char *k = S(TUI_TC_DIM, key);
    sb_appendf(b, "%s %s\n", k, value);
    free(k);
}

static void head(sb_t *b, const char *title) {
    char *t = tui_bold(title);
    sb_appendf(b, "%s\n", t);
    free(t);
}

/* toLocaleString() of an integer (en-US grouping) */
static char *grouped(int64_t v) {
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

static void kv_int(sb_t *b, const char *key, int64_t v) {
    char *g = grouped(v);
    kv(b, key, g);
    free(g);
}

/* ---------- provider auth (resolveProviderAuthMode, KeyCascade.describe) ---------- */

static const json_t *stored_credential(const char *provider, json_t *list) {
    json_t *e;
    json_arr_foreach(list, i, e) if (str_eq(json_get_str(e, "provider"), provider)) return e;
    return NULL;
}

static bool env_key(const char *provider) {
    const char *name = ai_env_api_key_name(provider);
    return name && env_get(name) && *env_get(name);
}

static const char *config_key(const char *provider) { return json_get_str(catalog_provider_info(provider), "configApiKey"); }

static const char *auth_mode(const char *provider, const json_t *stored) {
    if (stored && str_eq(json_get_str(stored, "type"), "oauth")) return "oauth";
    if (stored) return "api key";
    if (env_key(provider)) return "env api key";
    if (auth_has_runtime_key(provider) || config_key(provider)) return "runtime/fallback";
    return "unknown";
}

/* NULL when no credential is configured */
static char *auth_source(const char *provider, const json_t *stored) {
    if (auth_has_runtime_key(provider)) return xstrdup("runtime override (--api-key)");
    if (config_key(provider)) return xstrdup("config override (models.yml)");
    if (stored && str_eq(json_get_str(stored, "type"), "oauth")) {
        const char *email = json_get_str(stored, "email");
        return xasprintf("local store \xc2\xb7 oauth #1 (%s)", email && *email ? email : "cred 1");
    }
    if (stored) return xstrdup("local store \xc2\xb7 api_key #1 (cred 1)");
    if (env_key(provider)) return xstrdup("env (over local store)");
    return NULL;
}

/* formatEndpoint: origin + path without its trailing slash ("/" when empty) */
static char *format_endpoint(const char *url) {
    if (!url) return xstrdup("");
    const char *scheme = strstr(url, "://");
    if (!scheme) return xstrdup(url);
    const char *path = strchr(scheme + 3, '/');
    char *q = xstrdup(url);
    char *cut = strpbrk(q + (scheme - url) + 3, "?#");
    if (cut) *cut = 0;
    if (!path || (cut && path - url >= cut - q)) {
        char *r = xasprintf("%s/", q);
        free(q);
        return r;
    }
    size_t n = strlen(q);
    if (n && q[n - 1] == '/' && (size_t)(path - url) < n - 1) q[n - 1] = 0;
    return q;
}

/* shouldEnableAppendOnlyContext (config/append-only-context-mode.ts) */
static bool append_only(const char *setting, const ai_model_t *m) {
    if (str_eq(setting, "on")) return true;
    if (str_eq(setting, "off")) return false;
    if (!m) return false;
    static const char *const PREFER[] = {"deepseek", "ollama", "ollama-cloud", "lm-studio", "llama.cpp",
                                         "google-antigravity", "google", "google-vertex"};
    for (size_t i = 0; i < ARRAY_LEN(PREFER); i++)
        if (str_eq(m->provider, PREFER[i])) return true;
    if (str_starts(m->provider, "xiaomi") || (m->base_url && strstr(m->base_url, "xiaomimimo.com"))) return true;
    const char *u = m->base_url ? strstr(m->base_url, "://") : NULL;
    if (u) {
        u += 3;
        size_t hl = strcspn(u, ":/?#");
        char *host = xstrndup(u, hl);
        for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
        bool local = !strcmp(host, "localhost") || !strcmp(host, "127.0.0.1") || !strcmp(host, "0.0.0.0") ||
                     !strcmp(host, "[::1]") || str_starts(host, "10.") || str_starts(host, "192.168.") || str_ends(host, ".local");
        if (!local && str_starts(host, "172.")) {
            int oct = atoi(host + 4);
            local = oct >= 16 && oct <= 31;
        }
        free(host);
        if (local) return true;
    }
    return m->compat && json_get(m->compat, "supportsStore") && json_get_bool(m->compat, "supportsStore", false);
}

/* ---------- /session info overlay (overlays/session-info-overlay.ts) ---------- */

typedef struct {
    tui_component_t base;
    ui_t *u;
    tui_text_t *info;
    int scroll, body_rows, total;
} sinfo_t;

static sinfo_t *g_sinfo;

#define PANEL_CHROME_ROWS 4

static void sinfo_render(tui_component_t *c, int width, strvec_t *out) {
    sinfo_t *s = (sinfo_t *)c;
    int inner = MAX(1, width - 4);
    int max_body = MAX(1, tui_term(s->u->tui)->rows - PANEL_CHROME_ROWS);
    strvec_t lines = {0};
    tui_component_render(&s->info->base, inner, &lines);
    bool overflow = (int)lines.n > max_body;
    if (overflow) {
        strvec_free(&lines);
        tui_component_render(&s->info->base, MAX(1, inner - 1), &lines);
    }
    int total = (int)lines.n, h = MAX(1, MIN(total, max_body));
    s->body_rows = h;
    s->total = total;
    int maxoff = MAX(0, total - h);
    s->scroll = MAX(0, MIN(s->scroll, maxoff));
    bool bar = total > h;
    int cw = bar ? inner - 1 : inner, ts = 0, te = h;
    if (bar) {
        int size = MAX(1, MIN((h * h) / total, h));
        ts = maxoff ? (int)lround((double)s->scroll / maxoff * (h - size)) : 0;
        te = ts + size;
    }
    strvec_push_own(out, ui_box_top(width, "Session Info"));
    for (int r = 0; r < h; r++) {
        int i = s->scroll + r;
        char *tr = tui_truncate_to_width(i < total ? lines.v[i] : "", cw, "");
        char *row;
        if (bar) {
            char *pad = tui_pad_to_width(tr, cw);
            char *g = S(r >= ts && r < te ? TUI_TC_ACCENT : TUI_TC_DIM, r >= ts && r < te ? "\xe2\x96\x88" : "\xe2\x94\x82");
            row = xasprintf("%s%s", pad, g);
            free(pad), free(g);
        } else {
            row = xstrdup(tr);
        }
        strvec_push_own(out, ui_box_row(row, width));
        free(row), free(tr);
    }
    strvec_free(&lines);
    strvec_push_own(out, ui_box_divider(width));
    char *up = ui_key_hint("up"), *down = ui_key_hint("down"), *cancel = ui_editor_key("tui.select.cancel");
    char *hint = xasprintf("%s/%s scroll \xc2\xb7 %s close", up, down, cancel);
    char *th = tui_truncate_to_width(hint, inner, NULL), *dh = S(TUI_TC_DIM, th);
    strvec_push_own(out, ui_box_row(dh, width));
    free(up), free(down), free(cancel), free(hint), free(th), free(dh);
    strvec_push_own(out, ui_box_bottom(width));
}

static void sinfo_close(ui_t *u) {
    sinfo_t *s = g_sinfo;
    if (!s) return;
    g_sinfo = NULL;
    tui_overlay_hide(u->tui, &s->base);
    tui_set_focus(u->tui, &u->editor->base);
    tui_request_render(u->tui);
    tui_component_destroy(&s->base);
}

static bool sinfo_key(tui_component_t *c, const tui_key_t *k) {
    sinfo_t *s = (sinfo_t *)c;
    if (tui_kb(k, "tui.select.cancel") || tui_key_matches(k, "escape")) {
        sinfo_close(s->u);
        return true;
    }
    int page = MAX(1, s->body_rows - 1);
    if (tui_key_matches(k, "up")) s->scroll--;
    else if (tui_key_matches(k, "down")) s->scroll++;
    else if (tui_key_matches(k, "pageUp")) s->scroll -= page;
    else if (tui_key_matches(k, "pageDown")) s->scroll += page;
    else if (tui_key_matches(k, "home")) s->scroll = 0;
    else if (tui_key_matches(k, "end")) s->scroll = s->total;
    else return true;
    tui_component_request_render(c);
    return true;
}

static void sinfo_destroy(tui_component_t *c) {
    sinfo_t *s = (sinfo_t *)c;
    tui_component_destroy(&s->info->base);
    free(s);
}

static const tui_component_vt SINFO_VT = {"session-info", sinfo_render, sinfo_key, NULL, sinfo_destroy};

static char *session_info_text(ui_t *u) {
    app_t *a = u->app;
    json_t *st = app_stats(a);
    int64_t tool_calls = 0, total_msgs = 0, in = 0, out = 0, cr = 0, cw = 0, total_tok = 0;
    pthread_mutex_lock(&a->agent->mu);
    json_t *m, *b;
    total_msgs = (int64_t)json_len(a->agent->messages);
    json_arr_foreach(a->agent->messages, i, m) {
        if (!str_eq(json_get_str(m, "role"), "assistant")) continue;
        json_arr_foreach(json_get_arr(m, "content"), k, b) if (str_eq(json_get_str(b, "type"), "toolCall")) tool_calls++;
        const json_t *us = json_get(m, "usage");
        in += json_get_int(us, "input", 0);
        out += json_get_int(us, "output", 0);
        cr += json_get_int(us, "cacheRead", 0);
        cw += json_get_int(us, "cacheWrite", 0);
        total_tok += json_get_int(us, "totalTokens", json_get_int(us, "input", 0) + json_get_int(us, "output", 0) +
                                                         json_get_int(us, "cacheRead", 0) + json_get_int(us, "cacheWrite", 0));
    }
    pthread_mutex_unlock(&a->agent->mu);
    sb_t b2 = SB_INIT;
    sb_t *s = &b2;
    kv(s, "File:", json_get_str(st, "sessionFile") ? json_get_str(st, "sessionFile") : "In-memory");
    kv(s, "ID:", json_get_str(st, "sessionId") ? json_get_str(st, "sessionId") : "");
    kv(s, "Build:", OMC_VERSION);
    sb_append(s, "\n");
    head(s, "Provider");
    const ai_model_t *mdl = a->model;
    if (!mdl) {
        char *d = S(TUI_TC_DIM, "No model selected");
        sb_appendf(s, "%s\n", d);
        free(d);
    } else {
        json_t *list = auth_list();
        const json_t *stored = stored_credential(mdl->provider, list);
        kv(s, "Name:", mdl->provider);
        kv(s, "Model:", mdl->id);
        kv(s, "API:", mdl->api ? mdl->api : "");
        kv(s, "Auth:", auth_mode(mdl->provider, stored));
        char *ep = format_endpoint(mdl->base_url);
        kv(s, "Endpoint:", ep);
        free(ep);
        char *src = auth_source(mdl->provider, stored);
        if (src) kv(s, "Source:", src);
        free(src);
        json_free(list);
    }
    sb_append(s, "\n");
    head(s, "Messages");
    kv_int(s, "User:", json_get_int(st, "userMessages", 0));
    kv_int(s, "Assistant:", json_get_int(st, "assistantMessages", 0));
    kv_int(s, "Tool Calls:", tool_calls);
    kv_int(s, "Tool Results:", json_get_int(st, "toolResults", 0));
    kv_int(s, "Total:", total_msgs);
    sb_append(s, "\n");
    {
        const char *setting = settings_str("provider.appendOnlyContext", "auto");
        bool on = append_only(setting, mdl);
        char *label = on ? S(TUI_TC_SUCCESS, "active") : S(TUI_TC_DIM, "inactive");
        char *sl = str_eq(setting, "auto") ? xasprintf("%s (%s)", setting, mdl ? mdl->provider : "?") : xstrdup(setting);
        char *v = xasprintf("%s (setting: %s)", label, sl);
        kv(s, "Append-Only:", v);
        free(label), free(sl), free(v);
    }
    head(s, "Tokens");
    kv_int(s, "Input:", in);
    kv_int(s, "Output:", out);
    if (cr > 0) kv_int(s, "Cache Read:", cr);
    if (cw > 0) kv_int(s, "Cache Write:", cw);
    kv_int(s, "Total:", total_tok);
    double cost = json_get_num(st, "cost", 0);
    if (cost > 0) {
        sb_append(s, "\n");
        head(s, "Cost");
        char *c = xasprintf("%.4f", cost);
        kv(s, "Total:", c);
        free(c);
    }
    {
        sb_append(s, "\n");
        head(s, "MCP Servers");
        json_t *conn = mcp_connected(a->mcp), *x;
        if (!json_len(conn)) {
            char *d = S(TUI_TC_DIM, "None connected");
            sb_appendf(s, "%s\n", d);
            free(d);
        }
        json_arr_foreach(conn, i, x) {
            char *k = xasprintf("%s:", json_get_str(x, "name")), *ok = S(TUI_TC_SUCCESS, "connected");
            char *n = xasprintf("(%lld tools)", (long long)json_get_int(x, "tools", 0)), *dn = S(TUI_TC_DIM, n);
            char *v = xasprintf("%s %s", ok, dn);
            kv(s, k, v);
            free(k), free(ok), free(n), free(dn), free(v);
        }
        json_free(conn);
    }
    json_free(st);
    return sb_detach(&b2);
}

static void session_info(ui_t *u) {
    sinfo_close(u);
    char *info = session_info_text(u);
    sinfo_t *s = xcalloc(1, sizeof *s);
    tui_component_init(&s->base, &SINFO_VT);
    s->u = u;
    s->info = tui_text_new(info, 0, 0);
    free(info);
    g_sinfo = s;
    tui_overlay_opts_t o = {TUI_ANCHOR_BOTTOM, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &s->base, &o);
    tui_overlay_fill_screen(u->tui, &s->base);
    tui_request_render(u->tui);
}

/* ---------- /session delete (SelectorController.handleSessionDeleteCommand) ---------- */

static void session_delete_confirmed(void *ud, bool yes) {
    ui_t *u = ud;
    app_t *a = u->app;
    if (!yes || !app_session_file(a)) {
        add_notice(u, "info", "Delete cancelled");
        return;
    }
    char *path = xstrdup(app_session_file(a));
    ui_btw_dispose(u);
    app_new_session(a); /* #detachActiveSessionBeforeDeletion */
    tui_container_clear(u->chat);
    ui_report_clear(u);
    unlink(path);
    free(path);
    add_notice(u, "info", "Session deleted");
    ui_overlay_session(u);
}

static void session_delete(ui_t *u) {
    const char *file = app_session_file(u->app);
    if (!file) {
        add_notice(u, "error", "No session file to delete (in-memory session)");
        return;
    }
    if (!fs_exists(file)) {
        add_notice(u, "error", "Session has not been saved yet");
        return;
    }
    ui_hook_confirm(u, "Delete Session", "This will permanently delete the current session.\nYou will be returned to the session selector.",
                    session_delete_confirmed, u);
}

/* ---------- /session pin (handleSessionPinCommand; omc stores one credential per provider) ---------- */

static void session_pin(ui_t *u, const char *arg) {
    app_t *a = u->app;
    if (u->worker_running) {
        add_notice(u, "info", "Cannot pin an account while the session is streaming.");
        return;
    }
    if (!a->model) {
        add_notice(u, "info", "Select a model before pinning a provider account.");
        return;
    }
    const char *provider = a->model->provider, *pname = provider;
    json_t *d;
    json_arr_foreach(auth_oauth_providers(), i, d) if (str_eq(json_get_str(d, "id"), provider) && json_get_str(d, "name")) pname =
        json_get_str(d, "name");
    json_t *list = auth_list();
    const json_t *stored = stored_credential(provider, list);
    if (!stored || !str_eq(json_get_str(stored, "type"), "oauth")) {
        char *src = auth_source(provider, stored);
        char *m = src ? xasprintf("No stored OAuth accounts for %s. Current auth comes from %s.", pname, src)
                      : xasprintf("No stored OAuth accounts for %s. Use /login to add one.", pname);
        add_notice(u, "info", m);
        free(m), free(src);
        json_free(list);
        return;
    }
    const char *email = json_get_str(stored, "email");
    char *label = email && *email ? xstrdup(email) : xstrdup("OAuth credential #1");
    char *sel = str_trim(arg);
    for (char *p = sel; *p; p++) *p = (char)tolower((unsigned char)*p);
    char *m;
    if (!*sel) {
        m = xasprintf("OAuth accounts for %s:\n1. %s (active)\n\nPin one with `/session pin <number|email|account id>`.", pname, label);
    } else {
        char *ll = xstrdup(label);
        for (char *p = ll; *p; p++) *p = (char)tolower((unsigned char)*p);
        bool hit = !strcmp(sel, "1") || !strcmp(sel, "active") || !strcmp(sel, ll) || !strcmp(sel, "oauth credential #1");
        free(ll);
        char *raw = str_trim(arg);
        m = hit ? xasprintf("Pinned %s to this session for %s.", label, pname) : xasprintf("No %s account matches \"%s\".", pname, raw);
        free(raw);
    }
    add_notice(u, "info", m);
    free(m), free(sel), free(label);
    json_free(list);
}

static void split_verb(const char *arg, char **verb, char **rest) {
    while (*arg == ' ' || *arg == '\t') arg++;
    size_t n = strcspn(arg, " \t");
    *verb = xstrndup(arg, n);
    for (char *p = *verb; *p; p++) *p = (char)tolower((unsigned char)*p);
    *rest = str_trim(arg + n);
}

void ui_session_command(ui_t *u, const char *arg) {
    char *verb, *rest;
    split_verb(arg, &verb, &rest);
    if (!strcmp(verb, "delete") && !*rest) session_delete(u);
    else if (!strcmp(verb, "pin")) session_pin(u, rest);
    else if (!*verb || (!strcmp(verb, "info") && !*rest)) session_info(u);
    else add_notice(u, "info", "Usage: /session [info|delete|pin [account]]");
    free(verb), free(rest);
}

/* ---------- /tools (prompt/tools-markdown.ts buildToolsMarkdown) ---------- */

static char *table_cell(const char *v) {
    sb_t b = SB_INIT;
    bool nl = false;
    for (const char *p = v ? v : ""; *p; p++) {
        if (*p == '\r' || *p == '\n') {
            nl = true;
            continue;
        }
        if (nl) sb_putc(&b, ' ');
        nl = false;
        if (*p == '|') sb_putc(&b, '\\');
        sb_putc(&b, *p);
    }
    char *t = str_trim(sb_cstr(&b));
    sb_free(&b);
    return t;
}

void ui_tools_command(ui_t *u) {
    agent_t *ag = u->app->agent;
    sb_t rows = SB_INIT;
    size_t n = 0;
    pthread_mutex_lock(&ag->mu);
    for (size_t i = 0; i < ag->tools.n; i++) {
        agent_tool_t *t = ag->tools.v[i];
        if (t->hidden || t->mounted) continue;
        char *d = table_cell(t->description);
        sb_appendf(&rows, "\n| `%s` | %s |", t->name, *d ? d : "No description provided.");
        free(d);
        n++;
    }
    pthread_mutex_unlock(&ag->mu);
    json_t *xd = xdev_entries(ag), *e;
    json_arr_foreach(xd, i, e) {
        char *d = table_cell(json_get_str(e, "summary"));
        sb_appendf(&rows, "\n| `xd://%s` | %s |", json_get_str(e, "name"), *d ? d : "No description provided.");
        free(d);
        n++;
    }
    json_free(xd);
    char *md = n ? xasprintf("| Tool | Description |\n|------|-------------|%s", sb_cstr(&rows)) : xstrdup("No tools are currently visible to the agent.");
    sb_free(&rows);
    ui_show_markdown_report(u, "Available Tools", md);
    free(md);
}

/* ---------- /jobs (handleJobsCommand + job-tree.ts; omc jobs have no subagent owner, so the tree is flat) ---------- */

static char *job_status(const char *st) {
    if (str_eq(st, "running")) return S(TUI_TC_WARNING, "running");
    if (str_eq(st, "completed")) return S(TUI_TC_SUCCESS, "completed");
    if (str_eq(st, "cancelled")) return S(TUI_TC_DIM, "cancelled");
    return S(TUI_TC_ERROR, "failed");
}

static char *truncate_label(const char *label, int max) {
    if (tui_visible_width(label) <= max) return xstrdup(label);
    if (max <= 1) return xstrdup("\xe2\x80\xa6");
    return tui_truncate_to_width(label, max, NULL);
}

static int job_cmp(const void *pa, const void *pb) {
    const json_t *a = *(const json_t *const *)pa, *b = *(const json_t *const *)pb;
    bool ra = str_eq(json_get_str(a, "status"), "running"), rb = str_eq(json_get_str(b, "status"), "running");
    if (ra != rb) return ra ? -1 : 1;
    int64_t sa = json_get_int(a, "startedAt", 0), sb2 = json_get_int(b, "startedAt", 0);
    return sa < sb2 ? -1 : sa > sb2;
}

static void jobs_report(ui_t *u, bool full) {
    json_t *snap = jobs_snapshot(u->app->env->jobs), *x;
    VEC(json_t *) running = {0};
    VEC(json_t *) recent = {0};
    json_arr_foreach(snap, i, x) {
        if (str_eq(json_get_str(x, "status"), "running")) vec_push(&running, x);
        else vec_push(&recent, x);
    }
    /* the 10 newest settled jobs */
    qsort(recent.v, recent.n, sizeof *recent.v, job_cmp);
    size_t skip = recent.n > 10 ? recent.n - 10 : 0;
    sb_t b = SB_INIT;
    char *k = S(TUI_TC_DIM, "Running:");
    sb_appendf(&b, "%s %zu\n", k, running.n);
    free(k);
    if (!running.n && recent.n == skip) {
        char *d = S(TUI_TC_DIM, "No async jobs yet.");
        sb_appendf(&b, "\n%s", d);
        free(d);
    } else {
        sb_putc(&b, '\n');
        VEC(json_t *) rows = {0};
        for (size_t i = 0; i < running.n; i++) vec_push(&rows, running.v[i]);
        for (size_t i = skip; i < recent.n; i++) vec_push(&rows, recent.v[i]);
        qsort(rows.v, rows.n, sizeof *rows.v, job_cmp);
        int columns = tui_term(u->tui)->cols > 0 ? tui_term(u->tui)->cols : 100;
        int64_t now = now_ms();
        for (size_t i = 0; i < rows.n; i++) {
            json_t *j = rows.v[i];
            const char *id = json_get_str(j, "id") ? json_get_str(j, "id") : "";
            int64_t ended = json_get_int(j, "endedAt", 0), started = json_get_int(j, "startedAt", now);
            char *dur = tc_format_duration(MAX(0, (ended ? ended : now) - started));
            char *type = xasprintf("[%s]", json_get_str(j, "type") ? json_get_str(j, "type") : "");
            char *did = S(TUI_TC_DIM, id), *dt = S(TUI_TC_DIM, type), *stt = job_status(json_get_str(j, "status"));
            char *pd = xasprintf("(%s)", dur), *dd = S(TUI_TC_DIM, pd);
            sb_appendf(&b, "%s %s %s %s\n", did, dt, stt, dd);
            free(dur), free(type), free(did), free(dt), free(stt), free(pd), free(dd);
            const char *label = json_get_str(j, "label") ? json_get_str(j, "label") : "";
            const char *text = full && json_get_str(j, "command") ? json_get_str(j, "command") : label;
            if (!strcmp(text, id)) continue;
            const char *indent = "  ";
            char *prefix = S(TUI_TC_DIM, indent);
            if (!full) {
                char *l = truncate_label(text, MAX(24, columns - 22 - 2)), *dl = S(TUI_TC_DIM, l);
                sb_appendf(&b, "%s%s\n", prefix, dl);
                free(l), free(dl);
            } else {
                size_t n = 0;
                char *san = out_sanitize(text, strlen(text), &n), *rt = tc_replace_tabs(san);
                strvec_t wl = {0};
                tui_wrap_text_with_ansi(rt, MAX(1, columns - 4 - 2), &wl);
                for (size_t w = 0; w < wl.n; w++) {
                    char *dl = S(TUI_TC_DIM, wl.v[w]);
                    sb_appendf(&b, "%s%s\n", prefix, dl);
                    free(dl);
                }
                strvec_free(&wl);
                free(san), free(rt);
            }
            free(prefix);
        }
        vec_free(&rows);
        while (b.len && (b.s[b.len - 1] == '\n' || b.s[b.len - 1] == ' ')) b.s[--b.len] = 0;
    }
    ui_show_report(u, "Background Jobs", &tui_text_new(sb_cstr(&b), 0, 0)->base);
    sb_free(&b);
    vec_free(&running);
    vec_free(&recent);
    json_free(snap);
}

/* handleJobsKillCommand */
static void jobs_kill(ui_t *u, const char *arg) {
    jobs_t *jm = u->app->env->jobs;
    char *target = str_trim(arg);
    if (!*target) {
        add_notice(u, "info", "Usage: /jobs kill <id>|all");
        free(target);
        return;
    }
    json_t *snap = jobs_snapshot(jm), *x;
    if (!strcmp(target, "all")) {
        int cancelled = 0;
        json_arr_foreach(snap, i, x) if (str_eq(json_get_str(x, "status"), "running") && jobs_cancel(jm, json_get_str(x, "id"))) cancelled++;
        char *m = cancelled ? xasprintf("Cancelled %d background job%s.", cancelled, cancelled == 1 ? "" : "s")
                            : xstrdup("No running background jobs to cancel.");
        add_notice(u, "info", m);
        free(m);
    } else {
        size_t n = 0;
        char *san = out_sanitize(target, strlen(target), &n);
        sb_t c = SB_INIT;
        bool sp = false;
        for (char *p = san; *p; p++) {
            if (isspace((unsigned char)*p)) {
                sp = true;
                continue;
            }
            if (sp && c.len) sb_putc(&c, ' ');
            sp = false;
            sb_putc(&c, *p);
        }
        char *safe = tui_truncate_to_width(sb_cstr(&c), 60, NULL);
        sb_free(&c);
        free(san);
        bool running = false;
        json_arr_foreach(snap, i, x) if (str_eq(json_get_str(x, "id"), target) && str_eq(json_get_str(x, "status"), "running")) running = true;
        char *m = !running ? xasprintf("No running background job with id \"%s\".", safe)
                  : jobs_cancel(jm, target) ? xasprintf("Cancelled background job %s.", safe)
                                            : xasprintf("Could not cancel background job %s.", safe);
        add_notice(u, "info", m);
        free(m), free(safe);
    }
    json_free(snap);
    free(target);
}

void ui_jobs_command(ui_t *u, const char *arg) {
    char *verb, *rest;
    split_verb(arg, &verb, &rest);
    if (!u->app->env || !u->app->env->jobs) add_notice(u, "warning", "Async background jobs are unavailable in this session.");
    else if (!strcmp(verb, "kill")) jobs_kill(u, rest);
    else if (*rest || (*verb && strcmp(verb, "full"))) add_notice(u, "info", "Usage: /jobs [full|kill <id>|kill all]");
    else jobs_report(u, !strcmp(verb, "full"));
    free(verb), free(rest);
}
