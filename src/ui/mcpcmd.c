/* mcpcmd.c - /mcp in the TUI (port of omp modes/controllers/mcp-command-controller.ts, v18.14.2): help, list,
 * add (quick add from arguments or the wizard, mcpwizard.c), remove, test, reauth, unauth, enable, disable,
 * reconnect, reload, resources, prompts, notifications; the MCP OAuth presentation blocks; `/login <value>`
 * as the pasted code of a waiting MCP OAuth flow; the live tool / prompt refresh and the mcp.notifications
 * resource-update injection (sdk.ts setOnToolsChanged / setOnResourcesChanged).
 *
 * Read-only listings are command reports (ui_show_report); results are bordered command messages in the
 * transcript (showCommandMessage). Everything that connects to a server runs on a worker thread and talks
 * to the UI through tui_post. omc keeps MCP OAuth credentials url-keyed in auth.json ("mcp:<url>"), so a
 * signed-in server needs no `auth` block in mcp.json. The Smithery registry subcommands are not ported. */
#include "ui/mcpint.h"

#include "coding/commands.h"
#include "config/settings.h"
#include "modes/interactive.h"
#include "tools/tools.h"
#include "ui/tools/tc.h"

#include <ctype.h>
#include <pthread.h>
#include <strings.h>

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static char *interrupt_key(void) {
    const strvec_t *keys = tui_keybindings_get(tui_keybindings_global(), "app.interrupt");
    return ui_key_hint(keys && keys->n ? keys->v[0] : "escape");
}

void mcpui_spawn(void *(*fn)(void *), void *arg) {
    pthread_t th;
    if (pthread_create(&th, NULL, fn, arg) == 0) pthread_detach(th);
    else fn(arg);
}

/* ---------- transcript blocks ---------- */

/* showCommandMessage (DynamicBorder + Text(text, 1, 1) + DynamicBorder) or the animated "Connecting to …"
 * ChatBlock (Spacer + Text(…, 1, 0)); refcounted so a worker can update it after the transcript owns it */
typedef struct {
    tui_component_t base;
    int refs;
    pthread_mutex_t mu;
    bool bordered;
    char *text;  /* styled text; NULL while the spinner runs */
    char *name;  /* spinner: server name */
    int frame, timer;
    tui_t *tui;
} blk_t;

static void blk_unref(blk_t *b) {
    if (__atomic_sub_fetch(&b->refs, 1, __ATOMIC_SEQ_CST)) return;
    free(b->text);
    free(b->name);
    pthread_mutex_destroy(&b->mu);
    free(b);
}

static void border_row(strvec_t *out, int width) {
    sb_t r = SB_INIT;
    sb_repeat(&r, tui_sym("boxRound.horizontal"), MAX(1, width));
    strvec_push_own(out, S(TUI_TC_BORDER, sb_cstr(&r)));
    sb_free(&r);
}

static void text_rows(strvec_t *out, const char *text, int width) {
    strvec_t w = {0};
    char *clean = tui_replace_tabs(text);
    tui_wrap_text_with_ansi(clean, MAX(1, width - 2), &w);
    free(clean);
    for (size_t i = 0; i < w.n; i++) strvec_push_own(out, xasprintf(" %s", w.v[i]));
    strvec_free(&w);
}

static void blk_render(tui_component_t *c, int width, strvec_t *out) {
    blk_t *b = (blk_t *)c;
    pthread_mutex_lock(&b->mu);
    char *t;
    if (b->text) t = xstrdup(b->text);
    else {
        const tui_theme_t *th = TH();
        const char *f = th->nspinner ? th->spinner[b->frame % th->nspinner] : "|";
        char *raw = xasprintf("%s Connecting to \"%s\"...", f, b->name ? b->name : "");
        t = S(TUI_TC_MUTED, raw);
        free(raw);
    }
    pthread_mutex_unlock(&b->mu);
    if (b->bordered) {
        border_row(out, width);
        strvec_push(out, "");
        text_rows(out, t, width);
        strvec_push(out, "");
        border_row(out, width);
    } else {
        strvec_push(out, "");
        text_rows(out, t, width);
    }
    free(t);
}

static void blk_destroy(tui_component_t *c) {
    blk_t *b = (blk_t *)c;
    if (b->timer > 0 && b->tui) tui_remove_timer(b->tui, b->timer);
    b->timer = 0;
    blk_unref(b);
}

static const tui_component_vt BLK_VT = {"mcp-block", blk_render, NULL, NULL, blk_destroy};

static blk_t *blk_new(bool bordered, const char *text) {
    blk_t *b = xcalloc(1, sizeof *b);
    tui_component_init(&b->base, &BLK_VT);
    pthread_mutex_init(&b->mu, NULL);
    b->refs = 1;
    b->bordered = bordered;
    b->text = text ? xstrdup(text) : NULL;
    return b;
}

static void blk_set_text(blk_t *b, const char *text) {
    pthread_mutex_lock(&b->mu);
    free(b->text);
    b->text = xstrdup(text);
    pthread_mutex_unlock(&b->mu);
}

static void present(ui_t *u, tui_component_t *c) {
    tui_container_add(u->chat, c);
    tui_request_render(u->tui);
}

void mcpui_show_message(ui_t *u, const char *text) { present(u, &blk_new(true, text)->base); }
static void show_message(ui_t *u, const char *text) { mcpui_show_message(u, text); }

/* #showReport: a read-only listing outside the transcript (body is text.trim()) */
void mcpui_show_report(ui_t *u, const char *title, const char *text) {
    char *t = str_trim(text);
    ui_show_report(u, title, &tui_text_new(t, 0, 0)->base);
    free(t);
}

static void show_report(ui_t *u, const char *title, const char *text) { mcpui_show_report(u, title, text); }

/* ---------- posting from workers ---------- */

enum { P_MSG, P_ERR, P_STATUS, P_WARN, P_REPORT, P_COMP, P_SYNC, P_BLK_TEXT, P_BLK_SPIN, P_COPY };

typedef struct {
    ui_t *u;
    int kind;
    char *a, *b;
    tui_component_t *c;
    blk_t *blk;
} post_t;

static void sync_tools(ui_t *u);

static void spin_tick(void *ud) {
    blk_t *b = ud;
    pthread_mutex_lock(&b->mu);
    b->frame++;
    pthread_mutex_unlock(&b->mu);
    if (b->tui) tui_request_render(b->tui);
}

static void post_cb(void *p) {
    post_t *x = p;
    ui_t *u = x->u;
    switch (x->kind) {
    case P_MSG: show_message(u, x->a); break;
    case P_ERR: add_notice(u, "error", x->a); break;
    case P_STATUS: add_notice(u, "info", x->a); break;
    case P_WARN: add_notice(u, "warning", x->a); break;
    case P_REPORT: show_report(u, x->a, x->b); break;
    case P_COMP: present(u, x->c); break;
    case P_SYNC: sync_tools(u); break;
    case P_BLK_SPIN: /* present the spinner block and start its animation (80 ms like McpConnectingBlock) */
        x->blk->tui = u->tui;
        __atomic_add_fetch(&x->blk->refs, 1, __ATOMIC_SEQ_CST); /* the transcript's reference */
        present(u, &x->blk->base);
        x->blk->timer = tui_add_timer(u->tui, 80, true, spin_tick, x->blk);
        blk_unref(x->blk); /* the post's reference */
        break;
    case P_BLK_TEXT:
        if (x->blk->timer > 0) tui_remove_timer(u->tui, x->blk->timer);
        x->blk->timer = 0;
        blk_set_text(x->blk, x->a);
        tui_request_render(u->tui);
        blk_unref(x->blk);
        break;
    case P_COPY: tui_term_copy_osc52(tui_term(u->tui), x->a, strlen(x->a)); break;
    }
    free(x->a);
    free(x->b);
    free(x);
}

static void post(ui_t *u, int kind, const char *a, const char *b) {
    post_t *x = xcalloc(1, sizeof *x);
    x->u = u;
    x->kind = kind;
    x->a = a ? xstrdup(a) : NULL;
    x->b = b ? xstrdup(b) : NULL;
    tui_post(u->tui, post_cb, x);
}

static void post_comp(ui_t *u, tui_component_t *c) {
    post_t *x = xcalloc(1, sizeof *x);
    x->u = u;
    x->kind = P_COMP;
    x->c = c;
    tui_post(u->tui, post_cb, x);
}

/* a bordered message block the worker updates later (blk_post_text drops the worker's reference) */
static blk_t *post_block(ui_t *u, bool bordered, const char *text) {
    blk_t *b = blk_new(bordered, text);
    b->refs = 2; /* transcript + worker */
    post_comp(u, &b->base);
    return b;
}

static void blk_post_text(ui_t *u, blk_t *b, const char *text) {
    post_t *x = xcalloc(1, sizeof *x);
    x->u = u;
    x->kind = P_BLK_TEXT;
    x->a = xstrdup(text);
    x->blk = b;
    tui_post(u->tui, post_cb, x);
}

#define MSG(u_, t_) post((u_), P_MSG, (t_), NULL)
#define ERR(u_, t_) post((u_), P_ERR, (t_), NULL)
#define STATUS(u_, t_) post((u_), P_STATUS, (t_), NULL)

/* ---------- tool refresh (session.refreshMCPTools) ---------- */

static int g_sync_timer;

static void sync_retry(void *ud) {
    g_sync_timer = 0;
    sync_tools(ud);
}

/* replace the agent's MCP tools with the manager's; deferred while a turn runs (tools in flight stay valid) */
static void sync_tools(ui_t *u) {
    app_t *a = u->app;
    if (!a->mcp || a->opts.no_tools) return;
    if (u->worker_running || agent_is_running(a->agent)) {
        if (!g_sync_timer) g_sync_timer = tui_add_timer(u->tui, 300, false, sync_retry, u);
        return;
    }
    mcp_sync_tools(a->mcp, a->agent);
    tools_finalize(a->agent, a->opts.tools);
    app_rebuild_system_prompt(a);
    tui_request_render(u->tui);
}

/* ---------- live notifications ---------- */

typedef struct {
    ui_t *u;
    char *server, *uri;
} res_ev_t;

static json_t *g_pending_res; /* [{server, uri}] waiting for the debounce window (UI thread) */
static int g_res_timer;

static void res_flush(void *ud) {
    ui_t *u = ud;
    g_res_timer = 0;
    json_t *items = g_pending_res;
    g_pending_res = NULL;
    if (!json_len(items) || !settings_bool("mcp.notifications", false)) {
        json_free(items);
        return;
    }
    /* buildMcpNotificationBatchMessage */
    sb_t b = SB_INIT;
    sb_appendf(&b, "[MCP notification] %zu resource(s) updated:", json_len(items));
    json_t *e;
    json_arr_foreach(items, i, e) sb_appendf(&b, "\n- server=\"%s\" uri=%s", json_get_str(e, "server"), json_get_str(e, "uri"));
    sb_append(&b, "\nUse read(path=\"mcp://<uri>\") to inspect if relevant.");
    json_free(items);
    if (u->worker_running) {
        agent_follow_up(u->app->agent, msg_user_text(sb_cstr(&b))); /* drawn when the run delivers it */
    } else if (u->app->model) {
        ix_start_prompt(u, sb_cstr(&b)); /* the yield queue's idle flush: a visible user turn */
    }
    sb_free(&b);
}

static void res_event_ui(void *p) {
    res_ev_t *ev = p;
    if (!g_pending_res) g_pending_res = json_arr();
    bool seen = false;
    json_t *e;
    json_arr_foreach(g_pending_res, i, e) seen = seen || (str_eq(json_get_str(e, "server"), ev->server) && str_eq(json_get_str(e, "uri"), ev->uri));
    if (!seen) {
        json_t *o = json_obj();
        json_set_str(o, "server", ev->server);
        json_set_str(o, "uri", ev->uri);
        json_push(g_pending_res, o);
    }
    if (g_res_timer) tui_remove_timer(ev->u->tui, g_res_timer);
    int64_t ms = settings_int("mcp.notificationDebounceMs", 500);
    g_res_timer = tui_add_timer(ev->u->tui, (int)MAX(1, MIN(ms, 60000)), false, res_flush, ev->u);
    free(ev->server);
    free(ev->uri);
    free(ev);
}

static void late_connect_ui(void *ud);

static void cb_tools(void *ud, const char *server) {
    UNUSED(server);
    ui_t *u = ud;
    post(u, P_SYNC, NULL, NULL);
}

static void cb_connected(void *ud, const char *server) {
    UNUSED(server);
    ui_t *u = ud;
    tui_post(u->tui, late_connect_ui, u);
}

static void prompts_ui(void *ud) {
    ui_t *u = ud;
    tui_request_render(u->tui); /* the slash menu reads MCP prompt commands live */
}

static void cb_prompts(void *ud, const char *server) {
    UNUSED(server);
    ui_t *u = ud;
    tui_post(u->tui, prompts_ui, u);
}

static void cb_resource(void *ud, const char *server, const char *uri) {
    ui_t *u = ud;
    res_ev_t *ev = xcalloc(1, sizeof *ev);
    ev->u = u;
    ev->server = xstrdup(server);
    ev->uri = xstrdup(uri);
    tui_post(u->tui, res_event_ui, ev);
}

static ui_t *g_install_ui;

static void notif_setting_changed(void *ud) {
    UNUSED(ud);
    ui_t *u = g_install_ui;
    if (u && u->app->mcp) mcp_set_notifications(u->app->mcp, settings_bool("mcp.notifications", false));
}

static void install_callbacks(ui_t *u) {
    if (!u->app->mcp) return;
    mcp_callbacks_t cb = {cb_tools, cb_prompts, cb_resource, cb_connected, u};
    mcp_set_callbacks(u->app->mcp, &cb);
    mcp_set_notifications(u->app->mcp, settings_bool("mcp.notifications", false));
}

/* formatMCPConnectionStatusMessage over the manager's servers (#handleMcpConnectionStatusEvent's status) */
static void connection_status(ui_t *u) {
    mcp_manager_t *m = u->app->mcp;
    if (!m || settings_bool("startup.quiet", false)) return;
    json_t *names = mcp_server_names(m), *n;
    sb_t pend = SB_INIT, conn = SB_INIT, fail = SB_INIT;
    int np = 0, nc = 0, nf = 0;
    json_t *failed = mcp_failed(m);
    json_arr_foreach(names, i, n) {
        const char *nm = json_sv(n);
        mcp_state_t st = mcp_server_state(m, nm);
        if (st == MCP_CONNECTED) sb_appendf(&conn, "%s%s", nc++ ? ", " : "", nm);
        else if (st == MCP_CONNECTING) sb_appendf(&pend, "%s%s", np++ ? ", " : "", nm);
        else {
            json_t *e;
            const char *err = "failed";
            json_arr_foreach(failed, k, e) if (str_eq(json_get_str(e, "name"), nm)) err = json_get_str(e, "error");
            json_t *c = mcp_server_config(m, nm);
            char *src = c && json_get_str(c, "_source") ? tc_shorten_path(json_get_str(c, "_source")) : NULL;
            char *flat = xstrdup(err ? err : "failed");
            for (char *q = flat; *q; q++)
                if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
            sb_appendf(&fail, "%s%s%s%s%s: %s", nf++ ? "; " : "", nm, src ? " [config: " : "", src ? src : "", src ? "]" : "", flat);
            free(flat);
            free(src);
            json_free(c);
        }
    }
    char *msg = NULL;
    if (np) {
        if (!nc && !nf) msg = xasprintf("Connecting to MCP servers: %s…", sb_cstr(&pend));
        else {
            sb_t b = SB_INIT;
            if (nc) sb_appendf(&b, "Connected: %s.", sb_cstr(&conn));
            if (nf) sb_appendf(&b, "%sFailed: %s.", b.len ? " " : "", sb_cstr(&fail));
            sb_appendf(&b, " Still connecting: %s…", sb_cstr(&pend));
            msg = sb_detach(&b);
        }
    } else if (nf) {
        msg = nc ? xasprintf("MCP finished with failures. Connected: %s. Failed: %s", sb_cstr(&conn), sb_cstr(&fail))
                 : xasprintf("MCP %s failed to connect: %s", nf == 1 ? "server" : "servers", sb_cstr(&fail));
    } else if (nc) {
        msg = xasprintf("Connected to MCP %s: %s.", nc == 1 ? "server" : "servers", sb_cstr(&conn));
    }
    if (msg) add_notice(u, "info", msg);
    free(msg);
    sb_free(&pend), sb_free(&conn), sb_free(&fail);
    json_free(names);
    json_free(failed);
}

static bool g_startup_phase = true; /* the connection status line follows startup connections only */

static void late_connect_ui(void *ud) {
    ui_t *u = ud;
    sync_tools(u);
    if (g_startup_phase) connection_status(u);
}

void ui_mcp_install(ui_t *u) {
    g_install_ui = u;
    install_callbacks(u);
    static bool listening;
    if (!listening) {
        listening = true;
        settings_on_change(notif_setting_changed, NULL);
    }
    /* servers that finished connecting after the startup window: bind their tools now */
    if (u->app->mcp) tui_post(u->tui, late_connect_ui, u); /* after the startup transcript is drawn */
}

/* the session's manager, created on first use like omp's always-present MCPManager (NULL: MCP off) */
static mcp_manager_t *manager(ui_t *u) {
    app_t *a = u->app;
    if (a->mcp) return a->mcp;
    if (a->opts.no_mcp || a->opts.no_tools || !settings_bool("mcp.enabled", true)) return NULL;
    a->mcp = mcp_manager_new();
    a->owns_mcp = true;
    install_callbacks(u);
    return a->mcp;
}

/* ---------- OAuth flow presentation + manual input / Esc ---------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int *g_esc_abort; /* editor-focused OAuth flow: Esc sets it */
static int g_flows;               /* MCP OAuth flows waiting for a code */
static char *g_manual;            /* `/login <value>` for the waiting flow */

/* wrapUrlRows */
static void url_rows(strvec_t *out, const char *label, const char *url, int width) {
    char *clean = tui_replace_tabs(url);
    int eff = MAX(16, width);
    int inline_w = 1 + (int)strlen(label) + 1 + tui_visible_width(clean);
    if (inline_w <= eff) {
        char *t = xasprintf("%s %s", label, clean), *m = S(TUI_TC_MUTED, t);
        strvec_push_own(out, xasprintf(" %s", m));
        free(t);
        free(m);
    } else {
        char *m = S(TUI_TC_MUTED, label);
        strvec_push_own(out, xasprintf(" %s", m));
        free(m);
        size_t n = strlen(clean);
        for (size_t i = 0; i < n; i += (size_t)eff) {
            char *chunk = xstrndup(clean + i, MIN((size_t)eff, n - i));
            strvec_push_own(out, S(TUI_TC_MUTED, chunk));
            free(chunk);
        }
    }
    free(clean);
}

/* MCPAuthorizationLinkPrompt */
typedef struct {
    tui_component_t base;
    char *url, *launch;
} link_t;

static void link_render(tui_component_t *c, int width, strvec_t *out) {
    link_t *l = (link_t *)c;
    char *ok = S(TUI_TC_SUCCESS, "Open authorization URL:");
    strvec_push_own(out, xasprintf(" %s", ok));
    free(ok);
    char *hl = xasprintf("\x1b]8;;%s\x1b\\Click here to authorize\x1b]8;;\x1b\\", l->url), *a = S(TUI_TC_ACCENT, hl);
    strvec_push_own(out, xasprintf(" %s", a));
    free(hl);
    free(a);
    url_rows(out, "Copy URL:", l->url, width);
    if (l->launch) url_rows(out, "Local shortcut (this machine only):", l->launch, width);
}

static void link_destroy(tui_component_t *c) {
    link_t *l = (link_t *)c;
    free(l->url);
    free(l->launch);
    free(l);
}

static const tui_component_vt LINK_VT = {"mcp-auth-link", link_render, NULL, NULL, link_destroy};

static void add_text(tui_container_t *c, tui_tc_t slot, const char *t) {
    char *s = S(slot, t);
    tui_container_add(c, &tui_text_new(s, 1, 0)->base);
    free(s);
}

typedef struct {
    ui_t *u;
} oflow_t;

static void of_on_auth(void *ud, const char *url, const char *launch, const char *instructions) {
    UNUSED(instructions);
    oflow_t *f = ud;
    tui_container_t *b = tui_container_new();
    add_text(b, TUI_TC_ACCENT, "━━━ OAuth Authorization Required ━━━");
    tui_container_add(b, &tui_spacer_new(1)->base);
    add_text(b, TUI_TC_MUTED, "Preparing browser authorization...");
    tui_container_add(b, &tui_spacer_new(1)->base);
    char *k = interrupt_key();
    char *w = xasprintf("Waiting for authorization... (Press %s to cancel, 5 minute timeout)", k);
    add_text(b, TUI_TC_MUTED, w);
    free(w);
    free(k);
    add_text(b, TUI_TC_MUTED, "Headless? Paste the redirect URL or code with /login <value>.");
    tui_container_add(b, &tui_spacer_new(1)->base);
    add_text(b, TUI_TC_ACCENT, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    ui_open_in_browser(url);
    post(f->u, P_COPY, url, NULL);
    tui_container_add(b, &tui_spacer_new(1)->base);
    add_text(b, TUI_TC_SUCCESS, "→ Attempting to open browser...");
    tui_container_add(b, &tui_spacer_new(1)->base);
    add_text(b, TUI_TC_MUTED, "Alternative if browser did not open:");
    link_t *l = xcalloc(1, sizeof *l);
    tui_component_init(&l->base, &LINK_VT);
    l->url = xstrdup(url);
    l->launch = launch && strcmp(launch, url) ? xstrdup(launch) : NULL;
    tui_container_add(b, &l->base);
    post_comp(f->u, &b->base);
}

/* present([Spacer(1), Text(muted message, 1, 0)]) */
static void present_row(ui_t *u, tui_tc_t slot, const char *msg) {
    tui_container_t *b = tui_container_new();
    tui_container_add(b, &tui_spacer_new(1)->base);
    add_text(b, slot, msg);
    post_comp(u, &b->base);
}

static void of_on_progress(void *ud, const char *msg) { present_row(((oflow_t *)ud)->u, TUI_TC_MUTED, msg); }

static char *of_poll(void *ud) {
    UNUSED(ud);
    pthread_mutex_lock(&g_mu);
    char *v = g_manual;
    g_manual = NULL;
    pthread_mutex_unlock(&g_mu);
    return v;
}

int mcpui_oauth_flow(ui_t *u, const char *server_url, const json_t *cfg, const char *www_auth, volatile int *abort,
                     bool editor_esc, char **err, bool *cancelled) {
    *err = NULL;
    *cancelled = false;
    pthread_mutex_lock(&g_mu);
    g_flows++;
    if (editor_esc) g_esc_abort = abort;
    pthread_mutex_unlock(&g_mu);
    oflow_t f = {u};
    auth_login_hooks_t h = {0};
    h.on_auth = of_on_auth;
    h.on_progress = of_on_progress;
    h.poll_code = of_poll;
    h.abort = abort;
    h.ud = &f;
    char *e = NULL;
    int rc = mcp_oauth_login_hooked(server_url, cfg, www_auth, &h, &e);
    pthread_mutex_lock(&g_mu);
    if (--g_flows == 0) {
        free(g_manual);
        g_manual = NULL;
    }
    if (g_esc_abort == abort) g_esc_abort = NULL;
    pthread_mutex_unlock(&g_mu);
    if (rc == 0) {
        present_row(u, TUI_TC_SUCCESS, "✓ Authorization completed in browser.");
        free(e);
        return 0;
    }
    /* runMcpOAuthFlow's error mapping */
    const char *m = e ? e : "";
    if (abort && *abort) {
        *cancelled = true;
        *err = xstrdup("OAuth flow cancelled");
    } else if (strstr(m, "timeout") || strstr(m, "timed out")) *err = xstrdup("OAuth flow timed out. Please try again.");
    else if (strstr(m, "403") || strstr(m, "unauthorized")) *err = xstrdup("OAuth authorization failed. Please check your client credentials.");
    else if (strstr(m, "invalid_grant")) *err = xstrdup("OAuth authorization code is invalid or expired. Please try again.");
    else if (strstr(m, "ECONNREFUSED") || strstr(m, "Connection refused") || strstr(m, "fetch failed"))
        *err = xstrdup("Could not connect to OAuth server. Please check the URLs and your network connection.");
    else *err = xasprintf("OAuth authentication failed: %s", m);
    free(e);
    return -1;
}

bool ui_mcp_manual_input(ui_t *u, const char *value) {
    UNUSED(u);
    pthread_mutex_lock(&g_mu);
    bool take = g_flows > 0 && value && *value;
    if (take) {
        free(g_manual);
        g_manual = xstrdup(value);
    }
    pthread_mutex_unlock(&g_mu);
    return take;
}

/* ---------- /mcp test Esc (mcpTestEscapeHandlers) ---------- */

typedef struct {
    int refs;
    volatile int cancel;
    bool hint_shown, settled;
    int64_t settled_at;
    char *name;
    blk_t *hint;
} mtest_t;

static mtest_t *g_test; /* UI thread */

static void test_unref(mtest_t *t) {
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_SEQ_CST)) return;
    free(t->name);
    free(t);
}

static void test_drop_ui(void) {
    if (!g_test) return;
    mtest_t *t = g_test;
    g_test = NULL;
    test_unref(t);
}

bool ui_mcp_escape(ui_t *u) {
    pthread_mutex_lock(&g_mu);
    volatile int *ab = g_esc_abort;
    if (ab) *ab = 1;
    pthread_mutex_unlock(&g_mu);
    if (ab) return true;
    if (!g_test) return false;
    mtest_t *t = g_test;
    if (t->settled) {
        if (mono_ms() - t->settled_at < 5000) {
            char *m = xasprintf("MCP test for \"%s\" already finished", t->name);
            add_notice(u, "info", m);
            free(m);
            return true;
        }
        test_drop_ui();
        return false;
    }
    t->cancel = 1;
    if (t->hint_shown && t->hint) {
        char *raw = xasprintf("Testing connection to \"%s\"...", t->name), *m = S(TUI_TC_MUTED, raw);
        blk_set_text(t->hint, m);
        free(raw);
        free(m);
        tui_request_render(u->tui);
    }
    test_drop_ui(); /* the dispatcher consumed the ownership */
    return true;
}

/* ---------- shared pieces ---------- */

typedef struct {
    ui_t *u;
    char *text;  /* full command text */
    char *verb;
    char *name;  /* parts.slice(2).join(" ") or NULL */
    char *cwd;
    mcp_manager_t *m;
    mtest_t *test;
    /* add */
    json_t *cfg;
    char *scope;
    bool has_token, command_add;
    bool enabled;
} mjob_t;

static void job_free(mjob_t *j) {
    free(j->text);
    free(j->verb);
    free(j->name);
    free(j->cwd);
    json_free(j->cfg);
    free(j->scope);
    free(j);
}

static bool project_enabled(void) { return settings_bool("mcp.enableProjectConfig", true); }

static int64_t startup_window(void) {
    int64_t w = settings_int("mcp.startupTimeoutMs", 250);
    return w <= 0 ? 30000 : w;
}

/* #showMCPConnectionErrors */
static void show_conn_errors(ui_t *u, const json_t *errors) {
    if (!json_len(errors)) return;
    sb_t b = SB_INIT;
    char *w = S(TUI_TC_WARNING, "Some servers failed to connect:");
    sb_appendf(&b, "\n%s\n\n", w);
    free(w);
    json_t *e;
    json_arr_foreach(errors, i, e) sb_appendf(&b, "  %s: %s\n", json_get_str(e, "name"), json_get_str(e, "error"));
    MSG(u, sb_cstr(&b));
    sb_free(&b);
}

/* servers that failed outright (still-connecting ones are not errors yet) */
static json_t *hard_failures(mcp_manager_t *m) {
    json_t *f = mcp_failed(m), *out = json_arr(), *e;
    json_arr_foreach(f, i, e) {
        if (mcp_server_state(m, json_get_str(e, "name")) == MCP_CONNECTING) continue;
        json_push(out, json_clone(e));
    }
    json_free(f);
    return out;
}

/* reloadServers: reconnect every configured server and rebind the session's MCP tools */
static void reload_servers(ui_t *u, mcp_manager_t *m, const char *cwd) {
    if (!m) return;
    mcp_disconnect_all(m);
    json_t *cfg = mcp_discover_config(cwd);
    mcp_connect_all(m, cfg, startup_window());
    json_free(cfg);
    post(u, P_SYNC, NULL, NULL);
    json_t *errs = hard_failures(m);
    show_conn_errors(u, errs);
    json_free(errs);
}

/* #waitForServerConnectionWithAnimation */
static mcp_state_t wait_animated(ui_t *u, mcp_manager_t *m, const char *name, bool suppress) {
    if (!m) return MCP_DISCONNECTED;
    blk_t *b = blk_new(false, NULL);
    b->name = xstrdup(name);
    b->refs = 2; /* post + worker */
    post_t *x = xcalloc(1, sizeof *x);
    x->u = u;
    x->kind = P_BLK_SPIN;
    x->blk = b;
    tui_post(u->tui, post_cb, x);
    int64_t deadline = mono_ms() + 10000;
    while (mcp_server_state(m, name) == MCP_CONNECTING && mono_ms() < deadline) sleep_ms(50);
    mcp_state_t st = mcp_server_state(m, name);
    if (st == MCP_CONNECTED) post(u, P_SYNC, NULL, NULL);
    char *raw, *t;
    if (st == MCP_CONNECTED) {
        raw = xasprintf("%s Connected to \"%s\"", tui_sym("status.enabled"), name);
        t = S(TUI_TC_SUCCESS, raw);
    } else if (st == MCP_CONNECTING) {
        raw = xasprintf("◌ \"%s\" is still connecting...", name);
        t = S(TUI_TC_MUTED, raw);
    } else if (suppress) {
        raw = xasprintf("◌ Connection check complete for \"%s\"", name);
        t = S(TUI_TC_MUTED, raw);
    } else {
        raw = xasprintf("⚠ Could not connect to \"%s\" yet", name);
        t = S(TUI_TC_WARNING, raw);
    }
    blk_post_text(u, b, t);
    free(raw);
    free(t);
    return st;
}

/* #handleTestConnection: one-off connection, throwing on failure */
static int test_connection(const char *name, const json_t *cfg, char **err, bool *auth, char **www) {
    int64_t to = json_get_int(cfg, "timeout", 30000);
    return mcp_probe(name, cfg, to, NULL, NULL, NULL, err, auth, www);
}

static char *accent(const char *s) { return S(TUI_TC_ACCENT, s); }

/* ---------- add ---------- */

/* #handleWizardComplete (worker) */
static void *add_worker(void *p) {
    mjob_t *j = p;
    ui_t *u = j->u;
    char *path = mcp_config_path(j->scope, j->cwd), *err = NULL;
    if (mcp_config_add(path, j->name, j->cfg, &err) != 0) {
        const char *tip = "";
        char *lt = NULL;
        if (strstr(err, "EACCES") || strstr(err, "ermission denied")) tip = "\n\nTip: Check file permissions for the config directory.";
        else if (strstr(err, "ENOSPC") || strstr(err, "No space")) tip = "\n\nTip: Insufficient disk space.";
        else if (strstr(err, "already exists")) {
            char *a = accent("/mcp list");
            lt = xasprintf("\n\nTip: Use %s to see existing servers.", a);
            free(a);
            tip = lt;
        }
        char *m = xasprintf("Failed to add server: %s%s", err, tip);
        ERR(u, m);
        free(m);
        free(lt);
        free(err);
        free(path);
        job_free(j);
        return NULL;
    }
    free(path);
    reload_servers(u, j->m, j->cwd);
    bool disabled = json_is_bool(json_get(j->cfg, "enabled")) && !json_get_bool(j->cfg, "enabled", true);
    mcp_state_t st = disabled ? MCP_DISCONNECTED : wait_animated(u, j->m, j->name, true);
    bool connected = st == MCP_CONNECTED, connecting = st == MCP_CONNECTING;
    if (!connected && !connecting && !disabled) {
        char *e = NULL;
        if (test_connection(j->name, j->cfg, &e, NULL, NULL) == 0) {
            connected = true;
            /* #syncManagerConnection */
            if (j->m && mcp_server_state(j->m, j->name) == MCP_DISCONNECTED) {
                mcp_connect_server(j->m, j->name, j->cfg, 30000);
                if (mcp_server_state(j->m, j->name) == MCP_CONNECTED) post(u, P_SYNC, NULL, NULL);
            }
        }
        free(e);
    }
    sb_t b = SB_INIT;
    char *raw = xasprintf("+ Added server \"%s\" to %s config", j->name, str_eq(j->scope, "user") ? "user" : "project");
    char *s = S(TUI_TC_SUCCESS, raw);
    sb_appendf(&b, "\n%s\n\n", s);
    free(raw);
    free(s);
    char *cmd = xasprintf("/mcp test %s", j->name), *ac = accent(cmd);
    if (connected) {
        raw = xasprintf("%s Successfully connected to server", tui_sym("status.enabled"));
        s = S(TUI_TC_SUCCESS, raw);
        sb_appendf(&b, "%s\n\n", s);
    } else if (connecting) {
        raw = xstrdup("◌ Server is connecting in background...");
        s = S(TUI_TC_MUTED, raw);
        char *t2 = xasprintf("  Run %s in a few seconds.", ac), *m2 = S(TUI_TC_MUTED, t2);
        sb_appendf(&b, "%s\n%s\n\n", s, m2);
        free(t2);
        free(m2);
    } else {
        raw = xstrdup("⚠ Server added but not yet connected");
        s = S(TUI_TC_WARNING, raw);
        char *t2 = xasprintf("  Run %s to test the connection.", ac), *m2 = S(TUI_TC_MUTED, t2);
        sb_appendf(&b, "%s\n%s\n\n", s, m2);
        free(t2);
        free(m2);
    }
    free(raw);
    free(s);
    free(cmd);
    free(ac);
    char *al = accent("/mcp list"), *t3 = xasprintf("Run %s to see all configured servers.", al), *m3 = S(TUI_TC_MUTED, t3);
    sb_appendf(&b, "%s\n", m3);
    free(al);
    free(t3);
    free(m3);
    MSG(u, sb_cstr(&b));
    sb_free(&b);
    job_free(j);
    return NULL;
}

void mcpui_add_server(ui_t *u, const char *name, const json_t *cfg, const char *scope) {
    mjob_t *j = xcalloc(1, sizeof *j);
    j->u = u;
    j->name = xstrdup(name);
    j->cfg = json_clone(cfg);
    j->scope = xstrdup(scope);
    j->cwd = xstrdup(u->app->cwd);
    j->m = manager(u);
    mcpui_spawn(add_worker, j);
}

void mcpui_wizard_cancelled(ui_t *u) {
    char *muted = S(TUI_TC_MUTED, "Server creation cancelled.");
    char *cc = ui_key_hint("ctrl+c"), *ik = interrupt_key();
    char *tip = xasprintf("Tip: Press %s or %s anytime to cancel", cc, ik), *d = S(TUI_TC_DIM, tip);
    char *t = xasprintf("\n%s\n\n%s\n", muted, d);
    show_message(u, t);
    free(muted), free(cc), free(ik), free(tip), free(d), free(t);
}

/* quick add with a URL: auth detection + OAuth like the wizard, then #handleWizardComplete */
static void *quick_add_worker(void *p) {
    mjob_t *j = p;
    ui_t *u = j->u;
    char *err = NULL, *www = NULL;
    bool auth = false;
    if (test_connection(j->name, j->cfg, &err, &auth, &www) != 0) {
        if (j->has_token) {
            char *m = xasprintf("Authentication failed for \"%s\": %s", j->name, err);
            ERR(u, m);
            free(m);
            goto done;
        }
        if (auth) {
            const char *url = json_get_str(j->cfg, "url");
            json_t *ep = mcp_oauth_discover(url, www);
            if (!ep) {
                char *m = xasprintf("Authentication required for \"%s\", but OAuth endpoints could not be discovered. "
                                    "Use /mcp add %s (wizard) or configure auth manually.",
                                    j->name, j->name);
                ERR(u, m);
                free(m);
                goto done;
            }
            json_free(ep);
            volatile int abort_flag = 0;
            char *oerr = NULL;
            bool cancelled = false;
            if (mcpui_oauth_flow(u, url, j->cfg, www, &abort_flag, true, &oerr, &cancelled) != 0) {
                char *m = cancelled ? xasprintf("Add cancelled for \"%s\"", j->name)
                                    : xasprintf("OAuth flow failed for \"%s\": %s", j->name, oerr);
                post(u, cancelled ? P_STATUS : P_ERR, m, NULL);
                free(m);
                free(oerr);
                goto done;
            }
        }
    }
    free(err);
    free(www);
    add_worker(j);
    return NULL;
done:
    free(err);
    free(www);
    job_free(j);
    return NULL;
}

/* #parseAddCommand */
static void handle_add(ui_t *u, const char *text) {
    const char *rest = text;
    while (*rest && !isspace((unsigned char)*rest)) rest++; /* /mcp */
    while (isspace((unsigned char)*rest)) rest++;
    while (*rest && !isspace((unsigned char)*rest)) rest++; /* add */
    strvec_t tok = commands_parse_args(rest);
    char *name = NULL, *url = NULL, *token = NULL;
    const char *scope = "project", *transport = "http";
    strvec_t cmd = {0};
    bool has_cmd = false;
    const char *error = NULL;
    char *error_own = NULL;
    size_t i = 0;
    if (tok.n && tok.v[0][0] != '-') {
        name = xstrdup(tok.v[0]);
        i = 1;
    }
    while (i < tok.n && !error && !error_own) {
        const char *a = tok.v[i], *v = i + 1 < tok.n ? tok.v[i + 1] : NULL;
        if (!strcmp(a, "--")) {
            for (size_t k = i + 1; k < tok.n; k++) strvec_push(&cmd, tok.v[k]);
            has_cmd = cmd.n > 0;
            break;
        } else if (!strcmp(a, "--scope")) {
            if (!v || (strcmp(v, "project") && strcmp(v, "user"))) error = "Invalid --scope value. Use project or user.";
            else scope = !strcmp(v, "user") ? "user" : "project";
        } else if (!strcmp(a, "--url")) {
            if (!v) error = "Missing value for --url.";
            else url = xstrdup(v);
        } else if (!strcmp(a, "--transport")) {
            if (!v || (strcmp(v, "http") && strcmp(v, "sse"))) error = "Invalid --transport value. Use http or sse.";
            else transport = !strcmp(v, "sse") ? "sse" : "http";
        } else if (!strcmp(a, "--token")) {
            if (!v) error = "Missing value for --token.";
            else token = xstrdup(v);
        } else {
            error_own = xasprintf("Unknown option: %s", a);
        }
        i += 2;
    }
    bool quick = url || has_cmd;
    if (!error && !error_own && quick) {
        if (!name) error = "Server name required for quick add. Usage: /mcp add <name> ...";
        else if (url && has_cmd) error = "Use either --url or -- <command...>, not both.";
        else if (token && !url) error = "--token requires --url (HTTP/SSE transport).";
    }
    if (error || error_own) add_notice(u, "error", error ? error : error_own);
    else if (!quick) ui_mcp_wizard_open(u, name);
    else {
        json_t *cfg = json_obj();
        bool cmd_add = has_cmd;
        if (has_cmd) {
            json_set_str(cfg, "type", "stdio");
            json_set_str(cfg, "command", cmd.v[0]);
            if (cmd.n > 1) {
                json_t *args = json_arr();
                for (size_t k = 1; k < cmd.n; k++) json_push_str(args, cmd.v[k]);
                json_set(cfg, "args", args);
            }
        } else {
            json_set_str(cfg, "type", transport);
            char *nu = strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8) ? xasprintf("https://%s", url) : xstrdup(url);
            json_set_str(cfg, "url", nu);
            free(nu);
            if (token) {
                json_t *h = json_obj();
                char *bt = xasprintf("Bearer %s", token);
                json_set_str(h, "Authorization", bt);
                free(bt);
                json_set(cfg, "headers", h);
            }
        }
        mjob_t *j = xcalloc(1, sizeof *j);
        j->u = u;
        j->name = xstrdup(name);
        j->cfg = cfg;
        j->scope = xstrdup(scope);
        j->cwd = xstrdup(u->app->cwd);
        j->m = manager(u);
        j->has_token = token != NULL;
        mcpui_spawn(cmd_add ? add_worker : quick_add_worker, j);
    }
    free(error_own);
    free(name);
    free(url);
    free(token);
    strvec_free(&cmd);
    strvec_free(&tok);
}

/* ---------- list ---------- */

static const char *status_suffix(mcp_manager_t *m, const char *name, const json_t *cfg, char **own) {
    bool inactive = cfg && json_is_bool(json_get(cfg, "enabled")) && !json_get_bool(cfg, "enabled", true);
    mcp_state_t st = mcp_server_state(m, name);
    if (inactive) *own = S(TUI_TC_WARNING, " ◌ inactive");
    else if (st == MCP_CONNECTED) *own = S(TUI_TC_SUCCESS, " ● connected");
    else if (st == MCP_CONNECTING) *own = S(TUI_TC_MUTED, " ◌ connecting");
    else *own = S(TUI_TC_MUTED, " ○ not connected");
    return *own;
}

static void list_level(sb_t *b, mcp_manager_t *m, const char *label, const char *path, const json_t *servers) {
    if (!json_len(servers)) return;
    char *sp = tc_shorten_path(path), *pl = xasprintf(" (%s):", sp), *a = accent(label), *mu = S(TUI_TC_MUTED, pl);
    sb_appendf(b, "%s%s\n", a, mu);
    free(sp), free(pl), free(a), free(mu);
    const char *k;
    json_t *v;
    json_obj_foreach(servers, i, k, v) {
        char *st = NULL, *an = accent(k);
        const char *type = json_get_str(v, "type") ? json_get_str(v, "type") : "stdio";
        char *tr = xasprintf("[%s]", type), *d = S(TUI_TC_DIM, tr);
        sb_appendf(b, "  %s%s %s\n", an, status_suffix(m, k, v, &st), d);
        free(st), free(an), free(tr), free(d);
    }
    sb_putc(b, '\n');
}

static void *list_worker(void *p) {
    mjob_t *j = p;
    ui_t *u = j->u;
    char *up = mcp_config_path("user", j->cwd), *pp = mcp_config_path("project", j->cwd), *err = NULL;
    json_t *uc = mcp_config_read(up, &err), *pc = err ? NULL : mcp_config_read(pp, &err);
    if (err) {
        char *m = xasprintf("Failed to list servers: %s", err);
        ERR(u, m);
        free(m);
        goto out;
    }
    json_t *us = json_get_obj(uc, "mcpServers"), *ps = json_get_obj(pc, "mcpServers");
    json_t *disabled = json_arr(), *x;
    json_arr_foreach(json_get_arr(uc, "disabledServers"), i, x) if (json_is_str(x)) json_push_str(disabled, json_sv(x));
    /* runtime-discovered servers not in the two config files */
    json_t *discovered = json_arr(); /* [{name, provider, source}] */
    if (j->m) {
        json_t *names = mcp_server_names(j->m), *n;
        json_arr_foreach(names, i, n) {
            const char *nm = json_sv(n);
            if (json_get(us, nm) || json_get(ps, nm)) continue;
            bool dis = false;
            json_arr_foreach(disabled, k, x) dis = dis || str_eq(json_sv(x), nm);
            if (dis) continue;
            json_t *c = mcp_server_config(j->m, nm);
            if (c && json_get_str(c, "_source")) {
                json_t *o = json_obj();
                json_set_str(o, "name", nm);
                json_set_str(o, "provider", json_get_str(c, "_provider") ? json_get_str(c, "_provider") : "OMP");
                json_set_str(o, "source", json_get_str(c, "_source"));
                json_push(discovered, o);
            }
            json_free(c);
        }
        json_free(names);
    }
    if (!json_len(us) && !json_len(ps) && !json_len(discovered) && !json_len(disabled)) {
        char *mu = S(TUI_TC_MUTED, "No MCP servers configured."), *a = accent("/mcp add");
        char *t = xasprintf("%s\n\nUse %s to add a server.", mu, a);
        post(u, P_REPORT, "MCP Servers", t);
        free(mu), free(a), free(t);
    } else {
        sb_t b = SB_INIT;
        list_level(&b, j->m, "User level", up, us);
        list_level(&b, j->m, "Project level", pp, ps);
        /* groupBySource: one group per provider + path, first-seen order */
        bool *done = xcalloc(json_len(discovered) + 1, sizeof(bool));
        for (size_t i = 0; i < json_len(discovered); i++) {
            if (done[i]) continue;
            json_t *g = json_at(discovered, i);
            char *sp = tc_shorten_path(json_get_str(g, "source")), *pl = xasprintf(" (%s):", sp);
            char *a = accent(json_get_str(g, "provider")), *mu = S(TUI_TC_MUTED, pl);
            sb_appendf(&b, "%s%s\n", a, mu);
            free(sp), free(pl), free(a), free(mu);
            for (size_t k = i; k < json_len(discovered); k++) {
                json_t *e = json_at(discovered, k);
                if (done[k] || !str_eq(json_get_str(e, "provider"), json_get_str(g, "provider")) ||
                    !str_eq(json_get_str(e, "source"), json_get_str(g, "source")))
                    continue;
                done[k] = true;
                char *st = NULL, *an = accent(json_get_str(e, "name"));
                sb_appendf(&b, "  %s%s\n", an, status_suffix(j->m, json_get_str(e, "name"), NULL, &st));
                free(st), free(an);
            }
            sb_putc(&b, '\n');
        }
        free(done);
        /* disabled via /mcp disable (third-party configs) */
        bool any = false;
        json_arr_foreach(disabled, i, x) {
            if (json_get(us, json_sv(x)) || json_get(ps, json_sv(x))) continue;
            if (!any) {
                char *a = accent("Disabled"), *mu = S(TUI_TC_MUTED, " (discovered servers):");
                sb_appendf(&b, "%s%s\n", a, mu);
                free(a), free(mu);
                any = true;
            }
            char *an = accent(json_sv(x)), *w = S(TUI_TC_WARNING, " ◌ disabled");
            sb_appendf(&b, "  %s%s\n", an, w);
            free(an), free(w);
        }
        if (any) sb_putc(&b, '\n');
        post(u, P_REPORT, "Configured MCP Servers", sb_cstr(&b));
        sb_free(&b);
    }
    json_free(disabled);
    json_free(discovered);
out:
    json_free(uc);
    json_free(pc);
    free(err);
    free(up);
    free(pp);
    job_free(j);
    return NULL;
}

/* ---------- remove ---------- */

/* parseRemoveArgs */
static bool parse_name_scope(const char *rest, char **name, const char **scope, char **err) {
    strvec_t tok = commands_parse_args(rest);
    *name = NULL;
    *scope = "project";
    *err = NULL;
    size_t i = 0;
    if (tok.n && tok.v[0][0] != '-') {
        *name = xstrdup(tok.v[0]);
        i = 1;
    }
    while (i < tok.n) {
        if (!strcmp(tok.v[i], "--scope")) {
            const char *v = i + 1 < tok.n ? tok.v[i + 1] : NULL;
            if (!v || (strcmp(v, "project") && strcmp(v, "user"))) {
                *err = xstrdup("Invalid --scope value. Use project or user.");
                break;
            }
            *scope = !strcmp(v, "user") ? "user" : "project";
            i += 2;
            continue;
        }
        *err = xasprintf("Unknown option: %s", tok.v[i]);
        break;
    }
    strvec_free(&tok);
    if (*err) {
        free(*name);
        *name = NULL;
        return false;
    }
    return true;
}

static void *remove_worker(void *p) {
    mjob_t *j = p;
    ui_t *u = j->u;
    char *path = mcp_config_path(j->scope, j->cwd), *err = NULL;
    json_t *c = mcp_config_read(path, &err);
    if (!c) {
        char *m = xasprintf("Failed to remove server: %s", err);
        ERR(u, m);
        free(m);
    } else if (!json_get(json_get_obj(c, "mcpServers"), j->name)) {
        char *m = xasprintf("Server \"%s\" not found in %s config.", j->name, j->scope);
        ERR(u, m);
        free(m);
    } else {
        if (j->m) mcp_disconnect_server(j->m, j->name);
        if (mcp_config_remove(path, j->name, &err) != 0) {
            char *m = xasprintf("Failed to remove server: %s", err);
            ERR(u, m);
            free(m);
        } else {
            reload_servers(u, j->m, j->cwd);
            char *raw = xasprintf("- Removed server \"%s\" from %s config", j->name, j->scope), *s = S(TUI_TC_SUCCESS, raw);
            char *t = xasprintf("\n%s\n", s);
            MSG(u, t);
            free(raw), free(s), free(t);
        }
    }
    json_free(c);
    free(err);
    free(path);
    job_free(j);
    return NULL;
}

/* ---------- test ---------- */

static void test_settle_ui(void *p) {
    mtest_t *t = p;
    t->settled = true;
    t->settled_at = mono_ms();
    if (g_test == t && !t->hint_shown) test_drop_ui(); /* no "(esc to cancel)" was shown: release Esc now */
    test_unref(t);
}

static void test_hint_ui(void *p) {
    mtest_t *t = p;
    t->hint_shown = true;
    test_unref(t);
}

static void *test_worker(void *p) {
    mjob_t *j = p;
    ui_t *u = j->u;
    mtest_t *t = j->test;
    char *path = NULL, *err = NULL;
    const char *scope;
    json_t *cfg = NULL;
    bool disc;
    char *settle = xasprintf("Tested connection to \"%s\".", j->name);
    if (mcp_find_server(j->name, j->cwd, j->m, project_enabled(), &path, &scope, &cfg, &disc) != 0) {
        char *a = accent("/mcp list");
        char *m = xasprintf("Server \"%s\" not found.\n\nTip: Run %s to see available servers.", j->name, a);
        ERR(u, m);
        free(a), free(m);
        goto out;
    }
    if (json_is_bool(json_get(cfg, "enabled")) && !json_get_bool(cfg, "enabled", true)) {
        char *m = xasprintf("Server \"%s\" is disabled. Run /mcp enable %s first.", j->name, j->name);
        ERR(u, m);
        free(m);
        goto out;
    }
    if (t->cancel) {
        char *m = xasprintf("Cancelled MCP test for \"%s\"", j->name);
        STATUS(u, m);
        free(m);
        goto out;
    }
    char *ik = interrupt_key(), *raw = xasprintf("Testing connection to \"%s\"... (%s to cancel)", j->name, ik), *hs = S(TUI_TC_MUTED, raw);
    t->hint = post_block(u, true, hs);
    __atomic_add_fetch(&t->refs, 1, __ATOMIC_SEQ_CST);
    tui_post(u->tui, test_hint_ui, t);
    free(ik), free(raw), free(hs);
    json_t *info = NULL, *tools = NULL;
    int64_t to = json_get_int(cfg, "timeout", 30000);
    if (mcp_probe(j->name, cfg, to, &t->cancel, &info, &tools, &err, NULL, NULL) == 0 && !t->cancel) {
        sb_t b = SB_INIT;
        char *r2 = xasprintf("%s Successfully connected to \"%s\"", tui_sym("status.enabled"), j->name), *s = S(TUI_TC_SUCCESS, r2);
        sb_appendf(&b, "\n%s\n\n  Server: %s v%s\n  Tools: %zu\n", s, json_get_str(info, "name") ? json_get_str(info, "name") : "undefined",
                   json_get_str(info, "version") ? json_get_str(info, "version") : "undefined", json_len(tools));
        free(r2), free(s);
        if (json_len(tools) && json_len(tools) <= 10) {
            sb_append(&b, "\n  Available tools:\n");
            json_t *x;
            json_arr_foreach(tools, i, x) sb_appendf(&b, "    • %s\n", json_get_str(x, "name"));
        }
        /* #syncManagerConnection */
        if (j->m && mcp_server_state(j->m, j->name) == MCP_DISCONNECTED) {
            mcp_connect_server(j->m, j->name, cfg, 30000);
            if (mcp_server_state(j->m, j->name) == MCP_CONNECTED) post(u, P_SYNC, NULL, NULL);
        }
        MSG(u, sb_cstr(&b));
        sb_free(&b);
    } else if (t->cancel) {
        free(settle);
        settle = xasprintf("Cancelled connection test for \"%s\".", j->name);
        char *m = xasprintf("Cancelled MCP test for \"%s\"", j->name);
        STATUS(u, m);
        free(m);
    } else {
        const char *e = err ? err : "";
        const char *tip = "";
        if (strstr(e, "ENOENT") || strstr(e, "not found") || strstr(e, "No such file")) tip = "\n\nTip: Check that the command or URL is correct.";
        else if (strstr(e, "EACCES") || strstr(e, "ermission denied")) tip = "\n\nTip: Check file/command permissions.";
        else if (strstr(e, "ECONNREFUSED") || strstr(e, "Connection refused")) tip = "\n\nTip: Check that the server is running and the URL/port is correct.";
        else if (strstr(e, "timeout") || strstr(e, "timed out")) tip = "\n\nTip: The server may be slow or unresponsive. Try increasing the timeout.";
        else if (strstr(e, "401") || strstr(e, "403")) tip = "\n\nTip: Check your authentication credentials.";
        free(settle);
        settle = xasprintf("Connection test for \"%s\" failed.", j->name);
        char *m = xasprintf("Failed to connect to \"%s\": %s%s", j->name, e, tip);
        ERR(u, m);
        free(m);
    }
    {
        char *ms = S(TUI_TC_MUTED, settle);
        blk_post_text(u, t->hint, ms);
        free(ms);
    }
    json_free(info);
    json_free(tools);
out:
    tui_post(u->tui, test_settle_ui, t); /* takes the worker's reference */
    free(settle);
    free(err);
    free(path);
    json_free(cfg);
    job_free(j);
    return NULL;
}

/* ---------- enable / disable ---------- */

/* #connectEnabledMCPServer */
static void connect_enabled(ui_t *u, mcp_manager_t *m, const char *name, const char *cwd) {
    if (!m) return;
    json_t *all = mcp_discover_config(cwd);
    json_t *c = json_get_obj(all, name);
    if (c) {
        mcp_state_t st = mcp_connect_server(m, name, c, 30000); /* connectServers settles */
        json_t *errs = json_arr();
        if (st == MCP_DISCONNECTED) {
            json_t *f = mcp_failed(m), *e;
            json_arr_foreach(f, i, e) if (str_eq(json_get_str(e, "name"), name)) json_push(errs, json_clone(e));
            json_free(f);
        }
        post(u, P_SYNC, NULL, NULL);
        show_conn_errors(u, errs);
        json_free(errs);
    } else {
        post(u, P_SYNC, NULL, NULL);
    }
    json_free(all);
}

static char *state_word(mcp_state_t st, bool capital) {
    if (st == MCP_CONNECTED) return S(TUI_TC_SUCCESS, capital ? "Connected" : "connected");
    if (st == MCP_CONNECTING) return S(TUI_TC_MUTED, capital ? "Connecting" : "connecting");
    return S(TUI_TC_WARNING, capital ? "Not connected yet" : "not connected");
}

static void *enable_worker(void *p) {
    mjob_t *j = p;
    ui_t *u = j->u;
    bool on = j->enabled;
    char *path = NULL, *err = NULL;
    const char *scope;
    json_t *cfg = NULL;
    bool disc;
    if (mcp_find_server(j->name, j->cwd, j->m, project_enabled(), &path, &scope, &cfg, &disc) != 0) {
        /* a discovered server from a third-party config: the user denylist */
        char *up = mcp_config_path("user", j->cwd);
        json_t *dl = mcp_config_disabled(up), *x;
        bool cur = false;
        json_arr_foreach(dl, i, x) cur = cur || str_eq(json_sv(x), j->name);
        json_free(dl);
        json_t *mc = j->m ? mcp_server_config(j->m, j->name) : NULL;
        bool is_disc = mc && json_get_str(mc, "_source");
        json_free(mc);
        if (!is_disc && !cur) {
            char *m = xasprintf("Server \"%s\" not found.", j->name);
            ERR(u, m);
            free(m);
        } else if (cur == !on) {
            char *raw = xasprintf("Server \"%s\" is already %s.", j->name, on ? "enabled" : "disabled"), *s = S(TUI_TC_MUTED, raw);
            char *t = xasprintf("\n%s\n", s);
            MSG(u, t);
            free(raw), free(s), free(t);
        } else if (mcp_config_set_disabled(up, j->name, !on, &err) != 0) {
            char *m = xasprintf("Failed to %s server: %s", on ? "enable" : "disable", err);
            ERR(u, m);
            free(m);
        } else if (on) {
            connect_enabled(u, j->m, j->name, j->cwd);
            mcp_state_t st = wait_animated(u, j->m, j->name, false);
            char *w = state_word(st, true), *raw = xasprintf("%s Enabled \"%s\"", tui_sym("status.enabled"), j->name);
            char *s = S(TUI_TC_SUCCESS, raw), *t = xasprintf("\n%s\n\n  Status: %s\n", s, w);
            MSG(u, t);
            free(w), free(raw), free(s), free(t);
        } else {
            if (j->m) mcp_disconnect_server(j->m, j->name);
            post(u, P_SYNC, NULL, NULL);
            char *raw = xasprintf("%s Disabled \"%s\"", tui_sym("status.disabled"), j->name), *s = S(TUI_TC_MUTED, raw);
            char *t = xasprintf("\n%s\n", s);
            MSG(u, t);
            free(raw), free(s), free(t);
        }
        free(up);
        goto out;
    }
    bool now = !(json_is_bool(json_get(cfg, "enabled")) && !json_get_bool(cfg, "enabled", true));
    if (now == on) {
        char *raw = xasprintf("Server \"%s\" is already %s.", j->name, on ? "enabled" : "disabled"), *s = S(TUI_TC_MUTED, raw);
        char *t = xasprintf("\n%s\n", s);
        MSG(u, t);
        free(raw), free(s), free(t);
        goto out;
    }
    json_set_bool(cfg, "enabled", on);
    if (mcp_config_update(path, j->name, cfg, &err) != 0) {
        char *m = xasprintf("Failed to %s server: %s", on ? "enable" : "disable", err);
        ERR(u, m);
        free(m);
        goto out;
    }
    if (on) connect_enabled(u, j->m, j->name, j->cwd);
    else {
        if (j->m) mcp_disconnect_server(j->m, j->name);
        post(u, P_SYNC, NULL, NULL);
    }
    sb_t b = SB_INIT;
    char *raw = on ? xasprintf("%s Enabled \"%s\" (%s config)", tui_sym("status.enabled"), j->name, scope)
                   : xasprintf("%s Disabled \"%s\" (%s config)", tui_sym("status.disabled"), j->name, scope);
    char *s = S(on ? TUI_TC_SUCCESS : TUI_TC_MUTED, raw);
    sb_appendf(&b, "\n%s\n", s);
    free(raw), free(s);
    if (on) {
        mcp_state_t st = wait_animated(u, j->m, j->name, false);
        char *w = state_word(st, true);
        sb_appendf(&b, "\n  Status: %s\n", w);
        free(w);
    }
    MSG(u, sb_cstr(&b));
    sb_free(&b);
out:
    free(err);
    free(path);
    json_free(cfg);
    job_free(j);
    return NULL;
}

/* ---------- unauth / reauth ---------- */

static bool has_auth_header(const json_t *cfg) {
    const char *k;
    json_t *v;
    json_obj_foreach(json_get_obj(cfg, "headers"), i, k, v) if (!strcasecmp(k, "authorization")) return true;
    return false;
}

/* clearMcpClientAuthSurface */
static void clear_auth_surface(json_t *cfg) {
    json_del(cfg, "auth");
    json_t *h = json_get_obj(cfg, "headers");
    for (size_t i = json_len(h); h && i > 0; i--)
        if (!strcasecmp(h->u.obj.keys[i - 1], "authorization")) json_del(h, h->u.obj.keys[i - 1]);
}

static void *unauth_worker(void *p) {
    mjob_t *j = p;
    ui_t *u = j->u;
    char *path = NULL, *err = NULL;
    const char *scope;
    json_t *cfg = NULL;
    bool disc;
    if (mcp_find_server(j->name, j->cwd, j->m, project_enabled(), &path, &scope, &cfg, &disc) != 0) {
        char *m = xasprintf("Server \"%s\" not found.", j->name);
        ERR(u, m);
        free(m);
        goto out;
    }
    bool removed = false;
    const char *url = mcp_config_url(cfg);
    if (url && mcp_oauth_has_credential(url)) removed = mcp_oauth_logout(url) == 0;
    bool oauth_block = str_eq(json_get_str(json_get_obj(cfg, "auth"), "type"), "oauth");
    char *raw = xasprintf("- Cleared auth for \"%s\" (%s config)", j->name, scope), *s = S(TUI_TC_SUCCESS, raw);
    char *done_msg = xasprintf("\n%s\n", s);
    free(raw), free(s);
    if (disc && !oauth_block) {
        if (!removed) {
            char *r2 = xasprintf("No stored OAuth auth to remove for \"%s\".", j->name), *m2 = S(TUI_TC_MUTED, r2);
            char *t = xasprintf("\n%s\n", m2);
            MSG(u, t);
            free(r2), free(m2), free(t);
        } else {
            reload_servers(u, j->m, j->cwd);
            MSG(u, done_msg);
        }
    } else {
        json_del(cfg, "auth");
        if (mcp_config_update(path, j->name, cfg, &err) != 0) {
            char *m = xasprintf("Failed to clear auth: %s", err);
            ERR(u, m);
            free(m);
        } else {
            reload_servers(u, j->m, j->cwd);
            MSG(u, done_msg);
        }
    }
    free(done_msg);
out:
    free(err);
    free(path);
    json_free(cfg);
    job_free(j);
    return NULL;
}

/* resolveMcpOAuthEndpointsFromServer + reauthorizeMcpServer on omc's url-keyed credentials. Returns 0, or -1
 * with *err (owned) and *cancelled. *www gets the challenge for the flow. */
static int reauthorize(ui_t *u, const char *name, json_t *cfg, const char *path, bool acp_owned, volatile int *abort,
                       bool editor_esc, bool tui_messages, char **err, bool *cancelled) {
    *err = NULL;
    *cancelled = false;
    if (json_is_bool(json_get(cfg, "enabled")) && !json_get_bool(cfg, "enabled", true)) {
        *err = xasprintf("Server \"%s\" is disabled. Run /mcp enable %s first.", name, name);
        return -1;
    }
    const char *url = mcp_config_url(cfg);
    if (!url) {
        const char *remote = NULL;
        bool via_remote = str_istr(json_get_str(cfg, "command") ? json_get_str(cfg, "command") : "", "mcp-remote") != NULL;
        json_t *a;
        json_arr_foreach(json_get_arr(cfg, "args"), i, a) {
            if (!remote && json_is_str(a) && (str_starts(json_sv(a), "http://") || str_starts(json_sv(a), "https://"))) remote = json_sv(a);
            if (json_is_str(a) && strstr(json_sv(a), "mcp-remote")) via_remote = true;
        }
        char *hint = xasprintf("{ \"type\": \"http\", \"url\": \"%s\" }", remote ? remote : "<remote url>");
        *err = via_remote ? xasprintf("this server proxies OAuth through mcp-remote, which caches tokens machine-wide in ~/.mcp-auth "
                                      "(shared across every OMP profile). Clear ~/.mcp-auth to force a fresh login, or replace "
                                      "the proxy with %s so OMP manages OAuth per profile.",
                                      hint)
                          : xasprintf("stdio servers manage their own credentials, so OMP has no OAuth to reauthorize. If the "
                                      "service supports OAuth over HTTP, configure it as %s instead.",
                                      hint);
        free(hint);
        return -1;
    }
    json_t *probe = json_clone(cfg);
    json_del(probe, "auth");
    json_set_bool(probe, "_noOAuth", true);
    char *perr = NULL, *www = NULL;
    bool auth = false;
    int64_t to = json_get_int(cfg, "timeout", 30000);
    int prc = mcp_probe(name, probe, to, abort, NULL, NULL, &perr, &auth, &www);
    json_free(probe);
    free(perr);
    if (abort && *abort) {
        free(www);
        *cancelled = true;
        *err = xstrdup("OAuth flow cancelled");
        return -1;
    }
    json_t *ep = mcp_oauth_discover(url, prc == 0 ? NULL : www);
    if (!ep) {
        *err = xstrdup(prc == 0 ? "Server connection succeeded without OAuth; reauthorization is not required."
                                : "Could not discover OAuth endpoints from server response.");
        free(www);
        return -1;
    }
    json_free(ep);
    if (tui_messages) {
        char *raw = xasprintf("Reauthorizing \"%s\"...", name), *s = S(TUI_TC_MUTED, raw), *t = xasprintf("\n%s\n", s);
        MSG(u, t);
        free(raw), free(s), free(t);
    }
    json_t *flow_cfg = json_clone(cfg);
    clear_auth_surface(flow_cfg);
    int rc = mcpui_oauth_flow(u, url, flow_cfg, www, abort, editor_esc, err, cancelled);
    free(www);
    if (rc == 0 && !acp_owned && path && (json_get(cfg, "auth") || has_auth_header(cfg))) {
        char *werr = NULL;
        if (mcp_config_update(path, name, flow_cfg, &werr) != 0) {
            *err = werr;
            rc = -1;
        }
    }
    json_free(flow_cfg);
    return rc;
}

static void *reauth_worker(void *p) {
    mjob_t *j = p;
    ui_t *u = j->u;
    char *path = NULL, *err = NULL;
    const char *scope;
    json_t *cfg = NULL;
    bool disc;
    if (mcp_find_server(j->name, j->cwd, j->m, project_enabled(), &path, &scope, &cfg, &disc) != 0) {
        char *m = xasprintf("Server \"%s\" not found.", j->name);
        ERR(u, m);
        free(m);
        goto out;
    }
    if (json_is_bool(json_get(cfg, "enabled")) && !json_get_bool(cfg, "enabled", true)) {
        char *m = xasprintf("Server \"%s\" is disabled. Run /mcp enable %s first.", j->name, j->name);
        ERR(u, m);
        free(m);
        goto out;
    }
    volatile int abort_flag = 0;
    bool cancelled = false;
    if (reauthorize(u, j->name, cfg, disc ? NULL : path, false, &abort_flag, true, true, &err, &cancelled) != 0) {
        char *m = cancelled ? xasprintf("Reauthorization cancelled for \"%s\"", j->name)
                            : xasprintf("Failed to reauthorize server: %s", err ? err : "unknown error");
        post(u, cancelled ? P_STATUS : P_ERR, m, NULL);
        free(m);
        goto out;
    }
    reload_servers(u, j->m, j->cwd);
    mcp_state_t st = wait_animated(u, j->m, j->name, false);
    char *raw = xasprintf("✓ Reauthorized \"%s\" (%s config)", j->name, scope), *s = S(TUI_TC_SUCCESS, raw);
    char *w = state_word(st, false), *t = xasprintf("\n%s\n\n  Status: %s\n", s, w);
    MSG(u, t);
    free(raw), free(s), free(w), free(t);
out:
    free(err);
    free(path);
    json_free(cfg);
    job_free(j);
    return NULL;
}

/* ---------- reload / reconnect ---------- */

static void *reload_worker(void *p) {
    mjob_t *j = p;
    ui_t *u = j->u;
    char *s = S(TUI_TC_MUTED, "Reloading MCP servers and runtime tools..."), *t = xasprintf("\n%s\n", s);
    MSG(u, t);
    free(s), free(t);
    reload_servers(u, j->m, j->cwd);
    json_t *names = mcp_server_names(j->m), *n;
    int conn = 0, connecting = 0;
    json_arr_foreach(names, i, n) {
        mcp_state_t st = mcp_server_state(j->m, json_sv(n));
        conn += st == MCP_CONNECTED;
        connecting += st == MCP_CONNECTING;
    }
    json_free(names);
    char *raw = xasprintf("%s MCP reload complete", tui_sym("icon.loop")), *ok = S(TUI_TC_SUCCESS, raw);
    char *m = xasprintf("\n%s\n  Connected servers: %d\n  Connecting servers: %d\n", ok, conn, connecting);
    MSG(u, m);
    free(raw), free(ok), free(m);
    job_free(j);
    return NULL;
}

static void *reconnect_worker(void *p) {
    mjob_t *j = p;
    ui_t *u = j->u;
    char *raw = xasprintf("Reconnecting to \"%s\"...", j->name), *s = S(TUI_TC_MUTED, raw), *t = xasprintf("\n%s\n", s);
    MSG(u, t);
    free(raw), free(s), free(t);
    char *err = NULL;
    bool known = mcp_has_server(j->m, j->name);
    mcp_state_t st = known ? mcp_reconnect_server(j->m, j->name, 30000, &err) : MCP_DISCONNECTED;
    if (st == MCP_CONNECTED) {
        post(u, P_SYNC, NULL, NULL);
        json_t *tools = mcp_server_tools(j->m, j->name);
        char *r2 = xasprintf("%s Reconnected to \"%s\"", tui_sym("status.enabled"), j->name), *ok = S(TUI_TC_SUCCESS, r2);
        char *m = xasprintf("\n\n%s\n  Tools: %zu\n\n", ok, json_len(tools));
        MSG(u, m);
        free(r2), free(ok), free(m);
        json_free(tools);
    } else if (!known || !err) {
        char *m = xasprintf("Failed to reconnect to \"%s\". Check server status and logs.", j->name);
        ERR(u, m);
        free(m);
    } else {
        char *m = xasprintf("Failed to reconnect to \"%s\": %s", j->name, err);
        ERR(u, m);
        free(m);
    }
    free(err);
    job_free(j);
    return NULL;
}

/* ---------- resources / prompts / notifications ---------- */

static void resources_report(ui_t *u, mcp_manager_t *m) {
    sb_t b = SB_INIT;
    bool any = false;
    json_t *names = mcp_connected_names(m), *n;
    json_arr_foreach(names, i, n) {
        json_t *d = mcp_server_resources(m, json_sv(n));
        json_t *res = json_get_arr(d, "resources"), *tpl = json_get_arr(d, "templates"), *x;
        if (!d || (!json_len(res) && !json_len(tpl))) {
            json_free(d);
            continue;
        }
        any = true;
        char *a = accent(json_sv(n));
        sb_appendf(&b, "%s:\n", a);
        free(a);
        json_arr_foreach(res, k, x) {
            char *uri = S(TUI_TC_SUCCESS, json_get_str(x, "uri") ? json_get_str(x, "uri") : "");
            sb_appendf(&b, "  %s", uri);
            free(uri);
            if (json_get_str(x, "mimeType")) {
                char *mt = xasprintf("[%s]", json_get_str(x, "mimeType")), *dm = S(TUI_TC_DIM, mt);
                sb_appendf(&b, " %s", dm);
                free(mt), free(dm);
            }
            if (json_get_str(x, "description") && *json_get_str(x, "description")) {
                char *dd = S(TUI_TC_DIM, json_get_str(x, "description"));
                sb_appendf(&b, " %s", dd);
                free(dd);
            }
            sb_putc(&b, '\n');
        }
        if (json_len(tpl)) {
            char *mu = S(TUI_TC_MUTED, "Templates:");
            sb_appendf(&b, "  %s\n", mu);
            free(mu);
            json_arr_foreach(tpl, k, x) {
                char *ut = accent(json_get_str(x, "uriTemplate") ? json_get_str(x, "uriTemplate") : "");
                sb_appendf(&b, "    %s", ut);
                free(ut);
                if (json_get_str(x, "description") && *json_get_str(x, "description")) {
                    char *dd = S(TUI_TC_DIM, json_get_str(x, "description"));
                    sb_appendf(&b, " %s", dd);
                    free(dd);
                }
                sb_putc(&b, '\n');
            }
        }
        sb_putc(&b, '\n');
        json_free(d);
    }
    json_free(names);
    if (!any) {
        char *mu = S(TUI_TC_MUTED, "No resources available on connected servers.");
        sb_append(&b, mu);
        free(mu);
    }
    show_report(u, "MCP Resources", sb_cstr(&b));
    sb_free(&b);
}

static void prompts_report(ui_t *u, mcp_manager_t *m) {
    sb_t b = SB_INIT;
    bool any = false;
    json_t *names = mcp_connected_names(m), *n;
    json_arr_foreach(names, i, n) {
        json_t *ps = mcp_server_prompts(m, json_sv(n)), *p;
        if (!json_len(ps)) {
            json_free(ps);
            continue;
        }
        any = true;
        char *a = accent(json_sv(n));
        sb_appendf(&b, "%s:\n", a);
        free(a);
        json_arr_foreach(ps, k, p) {
            char *cn = xasprintf("/%s:%s", json_sv(n), json_get_str(p, "name") ? json_get_str(p, "name") : ""), *c = S(TUI_TC_SUCCESS, cn);
            sb_appendf(&b, "  %s", c);
            free(cn), free(c);
            if (json_get_str(p, "description") && *json_get_str(p, "description")) {
                char *dd = S(TUI_TC_DIM, json_get_str(p, "description"));
                sb_appendf(&b, " %s", dd);
                free(dd);
            }
            sb_putc(&b, '\n');
            json_t *arg;
            json_arr_foreach(json_get_arr(p, "arguments"), q, arg) {
                char *req = json_get_bool(arg, "required", false) ? S(TUI_TC_WARNING, " *") : xstrdup("");
                char *ad = json_get_str(arg, "description") && *json_get_str(arg, "description")
                               ? xasprintf(" - %s", json_get_str(arg, "description"))
                               : xstrdup("");
                char *dd = S(TUI_TC_DIM, ad);
                sb_appendf(&b, "    %s=%s%s\n", json_get_str(arg, "name") ? json_get_str(arg, "name") : "", req, dd);
                free(req), free(ad), free(dd);
            }
        }
        sb_putc(&b, '\n');
        json_free(ps);
    }
    json_free(names);
    if (!any) {
        char *mu = S(TUI_TC_MUTED, "No prompts available on connected servers.");
        sb_append(&b, mu);
        free(mu);
    }
    show_report(u, "MCP Prompts", sb_cstr(&b));
    sb_free(&b);
}

static void notifications_report(ui_t *u, mcp_manager_t *m) {
    bool on = mcp_notifications_enabled(m);
    sb_t b = SB_INIT;
    char *st = on ? S(TUI_TC_SUCCESS, "enabled") : S(TUI_TC_WARNING, "disabled"), *dd = S(TUI_TC_DIM, "(mcp.notifications setting)");
    sb_appendf(&b, "  Status: %s  %s\n\n", st, dd);
    free(st), free(dd);
    bool any = false;
    json_t *names = mcp_connected_names(m), *n;
    json_arr_foreach(names, i, n) {
        json_t *caps = mcp_server_capabilities(m, json_sv(n));
        json_t *res = json_get_obj(caps, "resources");
        bool sup_res = res != NULL, sub = json_get_bool(res, "subscribe", false);
        bool tl = json_get_bool(json_get_obj(caps, "tools"), "listChanged", false);
        bool pl = json_get_bool(json_get_obj(caps, "prompts"), "listChanged", false);
        bool rl = json_get_bool(res, "listChanged", false);
        if (!(tl || pl || rl || sub)) {
            json_free(caps);
            continue;
        }
        any = true;
        char *a = accent(json_sv(n)), *ck = S(TUI_TC_SUCCESS, "✓"), *cr = S(TUI_TC_DIM, "✗");
        sb_appendf(&b, "%s:\n", a);
        if (tl) sb_appendf(&b, "  %s tools/list_changed\n", ck);
        if (rl) sb_appendf(&b, "  %s resources/list_changed\n", ck);
        if (pl) sb_appendf(&b, "  %s prompts/list_changed\n", ck);
        if (sub) {
            json_t *subs = mcp_subscriptions(m, json_sv(n));
            size_t cnt = json_len(subs);
            char *ss;
            if (on && cnt) {
                char *t = xasprintf("subscribed (%zu URI%s)", cnt, cnt != 1 ? "s" : "");
                ss = S(TUI_TC_SUCCESS, t);
                free(t);
            } else if (on) ss = S(TUI_TC_MUTED, "no active subscriptions");
            else ss = S(TUI_TC_DIM, "inactive (notifications disabled)");
            sb_appendf(&b, "  %s resources/subscribe  %s\n", ck, ss);
            free(ss);
            json_t *x;
            if (on) json_arr_foreach(subs, k, x) {
                char *du = S(TUI_TC_DIM, json_sv(x));
                sb_appendf(&b, "    %s %s\n", ck, du);
                free(du);
            }
            json_free(subs);
        } else if (sup_res) {
            char *ns = S(TUI_TC_DIM, "not supported");
            sb_appendf(&b, "  %s resources/subscribe  %s\n", cr, ns);
            free(ns);
        }
        sb_putc(&b, '\n');
        free(a), free(ck), free(cr);
        json_free(caps);
    }
    json_free(names);
    if (!any) {
        char *mu = S(TUI_TC_MUTED, "No servers support notifications.");
        sb_append(&b, mu);
        free(mu);
    }
    show_report(u, "MCP Notifications", sb_cstr(&b));
    sb_free(&b);
}

/* ---------- dispatch ---------- */

static void show_help(ui_t *u) {
    char *c = accent("Commands:");
    char *t = xasprintf(
        "Manage Model Context Protocol (MCP) servers for external tool integrations.\n"
        "\n"
        "%s\n"
        "  /mcp add              Add a new MCP server (interactive wizard)\n"
        "  /mcp add <name> [--scope project|user] [--url <url> --transport http|sse] [--token <token>] [-- <command...>]\n"
        "  /mcp list             List all configured MCP servers\n"
        "  /mcp remove <name> [--scope project|user]    Remove an MCP server (default: project)\n"
        "  /mcp test <name>      Test connection to an MCP server\n"
        "  /mcp reauth <name>    Reauthorize OAuth for an MCP server\n"
        "  /mcp unauth <name>    Remove OAuth auth from an MCP server\n"
        "  /mcp enable <name>    Enable an MCP server\n"
        "  /mcp disable <name>   Disable an MCP server\n"
        "  /mcp reconnect <name> Reconnect to a specific MCP server\n"
        "  /mcp reload           Force reload and rediscover MCP runtime tools\n"
        "  /mcp resources        List available resources from connected servers\n"
        "  /mcp prompts          List available prompts from connected servers\n"
        "  /mcp notifications    Show notification capabilities and subscription state\n"
        "  /mcp help             Show this help message\n",
        c);
    show_report(u, "MCP Server Management", t);
    free(c);
    free(t);
}

static mjob_t *job(ui_t *u, const char *name) {
    mjob_t *j = xcalloc(1, sizeof *j);
    j->u = u;
    j->name = name ? xstrdup(name) : NULL;
    j->cwd = xstrdup(u->app->cwd);
    j->m = manager(u);
    return j;
}

/* the text after "/mcp <verb>" */
static const char *after_verb(const char *text) {
    const char *p = text;
    while (isspace((unsigned char)*p)) p++;
    while (*p && !isspace((unsigned char)*p)) p++;
    while (isspace((unsigned char)*p)) p++;
    while (*p && !isspace((unsigned char)*p)) p++;
    while (isspace((unsigned char)*p)) p++;
    return p;
}

void ui_mcp_command(ui_t *u, const char *text) {
    g_startup_phase = false;
    char *tt = str_trim(text);
    strvec_t parts = commands_parse_args(tt);
    char *sub = parts.n > 1 ? xstrdup(parts.v[1]) : NULL;
    for (char *c = sub; c && *c; c++) *c = (char)tolower((unsigned char)*c);
    sb_t nm = SB_INIT;
    for (size_t i = 2; i < parts.n; i++) sb_appendf(&nm, "%s%s", i > 2 ? " " : "", parts.v[i]);
    const char *name = nm.len ? sb_cstr(&nm) : NULL;
    if (u->app->mcp) mcp_set_notifications(u->app->mcp, settings_bool("mcp.notifications", false));
    if (!sub || !strcmp(sub, "help")) show_help(u);
    else if (!strcmp(sub, "add")) handle_add(u, tt);
    else if (!strcmp(sub, "list")) mcpui_spawn(list_worker, job(u, NULL));
    else if (!strcmp(sub, "remove") || !strcmp(sub, "rm")) {
        char *rn = NULL, *err = NULL;
        const char *scope;
        if (!parse_name_scope(after_verb(tt), &rn, &scope, &err)) add_notice(u, "error", err);
        else if (!rn) add_notice(u, "error", "Server name required. Usage: /mcp remove <name> [--scope project|user]");
        else {
            mjob_t *j = job(u, rn);
            j->scope = xstrdup(scope);
            mcpui_spawn(remove_worker, j);
        }
        free(rn);
        free(err);
    } else if (!strcmp(sub, "test")) {
        if (!name) add_notice(u, "error", "Server name required. Usage: /mcp test <name>");
        else {
            mjob_t *j = job(u, name);
            mtest_t *t = xcalloc(1, sizeof *t);
            t->refs = 2; /* Esc dispatcher + worker */
            t->name = xstrdup(name);
            test_drop_ui();
            g_test = t;
            j->test = t;
            mcpui_spawn(test_worker, j);
        }
    } else if (!strcmp(sub, "reauth")) {
        if (!name) add_notice(u, "error", "Server name required. Usage: /mcp reauth <name>");
        else mcpui_spawn(reauth_worker, job(u, name));
    } else if (!strcmp(sub, "unauth")) {
        if (!name) add_notice(u, "error", "Server name required. Usage: /mcp unauth <name>");
        else mcpui_spawn(unauth_worker, job(u, name));
    } else if (!strcmp(sub, "enable") || !strcmp(sub, "disable")) {
        bool on = !strcmp(sub, "enable");
        if (!name) {
            char *m = xasprintf("Server name required. Usage: /mcp %s <name>", sub);
            add_notice(u, "error", m);
            free(m);
        } else {
            mjob_t *j = job(u, name);
            j->enabled = on;
            mcpui_spawn(enable_worker, j);
        }
    } else if (!strcmp(sub, "resources") || !strcmp(sub, "prompts") || !strcmp(sub, "notifications")) {
        mcp_manager_t *m = manager(u);
        if (!m) add_notice(u, "error", "No MCP manager available.");
        else if (!strcmp(sub, "resources")) resources_report(u, m);
        else if (!strcmp(sub, "prompts")) prompts_report(u, m);
        else notifications_report(u, m);
    } else if (!strcmp(sub, "reconnect")) {
        if (!name) add_notice(u, "error", "Server name required. Usage: /mcp reconnect <name>");
        else if (!manager(u)) add_notice(u, "error", "MCP manager not available.");
        else mcpui_spawn(reconnect_worker, job(u, name));
    } else if (!strcmp(sub, "reload")) {
        mcpui_spawn(reload_worker, job(u, NULL));
    } else {
        char *m = xasprintf("Unknown subcommand: %s. Type /mcp help for usage.", sub);
        add_notice(u, "error", m);
        free(m);
    }
    free(sub);
    sb_free(&nm);
    strvec_free(&parts);
    free(tt);
}

/* ---------- argument completion (builtin-completions.ts buildMcpArgumentCompletions) ---------- */

static int ci_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void add_unique(strvec_t *v, const char *s) {
    if (!strvec_contains(v, s)) strvec_push(v, s);
}

int ui_mcp_complete(ui_t *u, const char *arg, tui_ac_result_t *out) {
    const char *sp = strchr(arg, ' ');
    char *raw = xstrndup(arg, (size_t)(sp - arg)), *low = xstrdup(raw);
    for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
    static const char *const SUBS[] = {"enable", "disable", "test", "remove", "reconnect", "reauth", "unauth"};
    bool ok = false;
    for (size_t i = 0; i < ARRAY_LEN(SUBS); i++) ok = ok || !strcmp(low, SUBS[i]);
    int rc = 0;
    if (!ok) goto out;
    char *prefix = xstrdup(sp + 1);
    for (char *c = prefix; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *cwd = u->app->cwd;
    char *up = mcp_config_path("user", cwd), *pp = mcp_config_path("project", cwd);
    json_t *uc = mcp_config_read(up, NULL), *pc = mcp_config_read(pp, NULL);
    strvec_t names = {0}, labels = {0}, values = {0};
    const char *k;
    json_t *v;
    if (!strcmp(low, "remove")) { /* config-file entries only; user-only names get --scope user */
        json_obj_foreach(json_get_obj(pc, "mcpServers"), i, k, v) add_unique(&names, k);
        json_obj_foreach(json_get_obj(uc, "mcpServers"), i, k, v) add_unique(&names, k);
        qsort(names.v, names.n, sizeof(char *), ci_cmp);
        for (size_t i = 0; i < names.n; i++) {
            if (strncasecmp(names.v[i], prefix, strlen(prefix))) continue;
            bool proj = json_get(json_get_obj(pc, "mcpServers"), names.v[i]) != NULL;
            strvec_push_own(&values, proj ? xasprintf("%s %s ", raw, names.v[i]) : xasprintf("%s %s --scope user ", raw, names.v[i]));
            strvec_push_own(&labels, proj ? xstrdup(names.v[i]) : xasprintf("%s (user)", names.v[i]));
        }
    } else { /* collectMcpServerNames */
        bool dis_only = !strcmp(low, "enable") || !strcmp(low, "disable");
        bool dis_cfg = dis_only || !strcmp(low, "unauth");
        json_t *x;
        if (dis_only) json_arr_foreach(json_get_arr(uc, "disabledServers"), i, x) if (json_is_str(x)) add_unique(&names, json_sv(x));
        for (int pass = 0; pass < 2; pass++)
            json_obj_foreach(json_get_obj(pass ? pc : uc, "mcpServers"), i, k, v) {
                bool off = json_is_bool(json_get(v, "enabled")) && !json_get_bool(v, "enabled", true);
                if (dis_cfg || !off) add_unique(&names, k);
            }
        json_t *rt = mcp_server_names(u->app->mcp);
        json_arr_foreach(rt, i, x) add_unique(&names, json_sv(x));
        json_free(rt);
        qsort(names.v, names.n, sizeof(char *), ci_cmp);
        for (size_t i = 0; i < names.n; i++) {
            if (strncasecmp(names.v[i], prefix, strlen(prefix))) continue;
            strvec_push_own(&values, xasprintf("%s %s ", raw, names.v[i]));
            strvec_push(&labels, names.v[i]);
        }
    }
    for (size_t i = 0; i < values.n; i++) {
        tui_ac_result_add(out, values.v[i], labels.v[i], "");
        strvec_push(&out->ghosts, "");
    }
    rc = values.n ? 1 : 0;
    strvec_free(&names);
    strvec_free(&labels);
    strvec_free(&values);
    json_free(uc);
    json_free(pc);
    free(up);
    free(pp);
    free(prefix);
out:
    free(raw);
    free(low);
    return rc;
}
