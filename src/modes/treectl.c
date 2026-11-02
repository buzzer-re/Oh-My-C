/* treectl.c - /tree and rewind controllers (port of omp SelectorController.showTreeSelector,
 * showUserMessageSelector and #rewindFromTranscript, as shipped in omp 18.14.2).
 *
 * /tree opens the session tree selector in the composer's place. Picking an entry navigates the tree in
 * the same session file (app_navigate_tree); with branchSummary.enabled a "Summarize branch?" prompt
 * offers No summary / Summarize / Summarize with custom prompt first (Shift+Enter skips it and
 * summarizes). Summaries run off the UI thread behind a "Summarizing branch... (Esc to cancel)" row;
 * typed input queues meanwhile, Esc aborts and reopens the tree. Shift+L labels are appended as label
 * entries. The rewind selector (/branch, /rewind, doubleEscapeAction) is a fullscreen page on the
 * alternate screen; picking an item rewinds there (a user prompt returns to the editor). */
#include "modes/interactive.h"

#include "config/settings.h"

#include <pthread.h>

static struct {
    ui_t *u;
    char *pending_id;   /* tree pick waiting for the posted close */
    bool pending_sum;
    char *target;       /* navigation target while the summary prompt / editor is open */
    bool running;       /* summary thread */
    pthread_t th;
    bool summarize;
    char *custom;
    int rc;
    app_nav_result_t res;
    char *err;
    tui_component_t *rewind;
} g;

static void navigate(ui_t *u, bool summarize, const char *custom);
static void summary_prompt(ui_t *u);

/* ---------- navigation ---------- */

static void finish_nav(ui_t *u, int rc, app_nav_result_t *res, char *err) {
    if (rc == 1) {
        add_notice(u, "info", "Branch summarization cancelled");
        tc_tree_open(u);
    } else if (rc < 0) {
        add_notice(u, "error", err ? err : "Navigation failed");
    } else {
        ui_chat_rebuild(u);
        char *cur = tui_editor_get_text(u->editor);
        char *trim = str_trim(cur ? cur : "");
        if (res->editor_text && !*trim) tui_editor_set_text(u->editor, res->editor_text);
        free(cur), free(trim);
        add_notice(u, "info", "Navigated to selected point");
    }
    free(res->editor_text);
    res->editor_text = NULL;
    free(err);
    tui_request_render(u->tui);
}

static void nav_done(void *p) {
    ui_t *u = p;
    pthread_join(g.th, NULL);
    g.running = false;
    ui_working_set_message(u, NULL);
    ui_working_stop(u);
    iq_hold(u, false);
    char *err = g.err;
    g.err = NULL;
    free(g.target);
    g.target = NULL;
    free(g.custom);
    g.custom = NULL;
    finish_nav(u, g.rc, &g.res, err);
}

static void *nav_main(void *p) {
    ui_t *u = p;
    g.rc = app_navigate_tree(u->app, g.target, g.summarize, g.custom, &g.res, &g.err);
    tui_post(u->tui, nav_done, u);
    return NULL;
}

static void navigate(ui_t *u, bool summarize, const char *custom) {
    if (!summarize) {
        app_nav_result_t res;
        char *err = NULL;
        int rc = app_navigate_tree(u->app, g.target, false, NULL, &res, &err);
        free(g.target);
        g.target = NULL;
        finish_nav(u, rc, &res, err);
        return;
    }
    g.summarize = true;
    free(g.custom);
    g.custom = custom ? xstrdup(custom) : NULL;
    g.running = true;
    iq_hold(u, true);
    char *esc = ui_editor_key("app.interrupt");
    if (!*esc) free(esc), esc = ui_key_hint("escape");
    char *msg = xasprintf("Summarizing branch... (%s to cancel)", esc);
    ui_working_set_message(u, msg);
    ui_working_start(u);
    free(msg), free(esc);
    if (pthread_create(&g.th, NULL, nav_main, u) != 0) {
        g.running = false;
        iq_hold(u, false);
        ui_working_stop(u);
        add_notice(u, "error", "failed to start the branch summary");
    }
}

static void custom_done(void *ud, const char *text) {
    ui_t *u = ud;
    if (!text) { /* cancelled: back to the summary choice */
        summary_prompt(u);
        return;
    }
    navigate(u, true, text);
}

static void summary_choice(void *ud, int index) {
    ui_t *u = ud;
    if (index < 0) { /* Esc: back to the tree */
        free(g.target);
        g.target = NULL;
        tc_tree_open(u);
    } else if (index == 2) ui_hook_editor(u, "Custom summarization instructions", custom_done, u);
    else navigate(u, index == 1, NULL);
}

static void summary_prompt(ui_t *u) {
    static const char *const OPTS[] = {"No summary", "Summarize", "Summarize with custom prompt"};
    ui_hook_select(u, "Summarize branch?", OPTS, 3, summary_choice, u);
}

/* ---------- /tree ---------- */

static void tree_picked(void *p) {
    ui_t *u = p;
    char *id = g.pending_id;
    g.pending_id = NULL;
    if (!id) return;
    session_t *s = u->app->session;
    ui_close_selector(u);
    if (str_eq(id, s->leaf_id)) {
        const json_t *e = session_get(s, id);
        const json_t *m = json_get(e, "message");
        bool ask = str_eq(json_get_str(e, "type"), "message") && str_eq(json_get_str(m, "role"), "toolResult") &&
                   str_eq(json_get_str(m, "toolName"), "ask");
        if (!ask) {
            free(id);
            add_notice(u, "info", "Already at this point");
            return;
        }
    }
    free(g.target);
    g.target = id;
    if (!g.pending_sum && settings_bool("branchSummary.enabled", false)) summary_prompt(u);
    else navigate(u, g.pending_sum, NULL);
}

static void tree_select(void *ud, const char *entry_id, bool summarize) {
    ui_t *u = ud;
    if (g.pending_id) return;
    g.pending_id = xstrdup(entry_id);
    g.pending_sum = summarize;
    tui_post(u->tui, tree_picked, u); /* the selector closes outside its own key handler */
}

static void tree_cancel_posted(void *p) { ui_close_selector((ui_t *)p); }

static void tree_cancel(void *ud) {
    ui_t *u = ud;
    tui_post(u->tui, tree_cancel_posted, u);
}

static void tree_label(void *ud, const char *entry_id, const char *label) {
    ui_t *u = ud;
    if (u->app->session) session_append_label(u->app->session, entry_id, label);
    tui_request_render(u->tui);
}

void tc_tree_open(ui_t *u) {
    session_t *s = u->app->session;
    if (!s || !json_len(s->entries)) {
        add_notice(u, "info", "No entries in session");
        return;
    }
    if (u->worker_running || g.running) {
        add_notice(u, "warning", "Cannot switch branches while the agent is running.");
        return;
    }
    ui_tree_cb_t cb = {tree_select, tree_cancel, tree_label, u};
    int rows = tui_term(u->tui)->rows;
    tui_component_t *c = ui_tree_selector_new(s->entries, s->leaf_id, rows, settings_str("treeFilterMode", "default"), &cb);
    ui_show_selector(u, c, c);
}

/* ---------- rewind ---------- */

static void rewind_close(ui_t *u) {
    if (!g.rewind) return;
    tui_component_t *c = g.rewind;
    g.rewind = NULL;
    tui_overlay_hide(u->tui, c);
    tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    tui_component_destroy(c);
    tui_request_render(u->tui);
}

static void rewind_picked(void *p) {
    ui_t *u = p;
    char *id = g.pending_id;
    g.pending_id = NULL;
    if (!id) return;
    session_t *s = u->app->session;
    const json_t *e = session_get(s, id);
    const json_t *m = json_get(e, "message");
    const char *t = json_get_str(e, "type");
    bool user = (str_eq(t, "message") && (str_eq(json_get_str(m, "role"), "user") ||
                                          (str_eq(json_get_str(m, "role"), "custom") && str_eq(json_get_str(m, "attribution"), "user")))) ||
                (str_eq(t, "custom_message") && str_eq(json_get_str(e, "attribution"), "user") &&
                 (str_eq(json_get_str(e, "customType"), "skill-prompt") || str_eq(json_get_str(e, "customType"), "collab-prompt")));
    if (!e) {
        rewind_close(u);
    } else if (str_eq(id, s->leaf_id) && !user) {
        rewind_close(u);
        add_notice(u, "info", "Already at this point");
    } else {
        app_nav_result_t res;
        char *err = NULL;
        int rc = app_navigate_tree(u->app, id, false, NULL, &res, &err);
        if (rc != 0) {
            rewind_close(u);
            add_notice(u, "error", err ? err : "Navigation failed");
        } else {
            ui_chat_rebuild(u);
            char *cur = tui_editor_get_text(u->editor);
            char *trim = str_trim(cur ? cur : "");
            if (res.editor_text && (user || !*trim)) tui_editor_set_text(u->editor, res.editor_text);
            free(cur), free(trim);
            rewind_close(u);
            add_notice(u, "info", "Rewound to selected point");
        }
        free(res.editor_text);
        free(err);
    }
    free(id);
}

static void rewind_select(void *ud, const char *entry_id) {
    ui_t *u = ud;
    if (g.pending_id) return;
    g.pending_id = xstrdup(entry_id);
    tui_post(u->tui, rewind_picked, u);
}

static void rewind_cancel_posted(void *p) { rewind_close((ui_t *)p); }

static void rewind_cancel(void *ud) {
    ui_t *u = ud;
    tui_post(u->tui, rewind_cancel_posted, u);
}

void tc_rewind_open(ui_t *u) {
    if (g.rewind) return;
    if (u->worker_running || g.running) {
        add_notice(u, "warning", "Cannot switch branches while the agent is running.");
        return;
    }
    ui_rewind_cb_t cb = {rewind_select, rewind_cancel, u};
    tui_component_t *c = ui_rewind_selector_new(u, &cb);
    if (!c) {
        add_notice(u, "info", "No messages to branch from");
        return;
    }
    g.rewind = c;
    /* fullscreen page on the alternate screen: the parked transcript stays untouched until the rewind */
    tui_alt_screen(u->tui, true);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, c, &o);
    tui_request_render(u->tui);
}

/* ---------- lifecycle ---------- */

bool tc_escape(ui_t *u) {
    if (!g.running) return false;
    app_abort_branch_summary(u->app);
    return true;
}

void tc_shutdown(ui_t *u) {
    if (g.rewind) rewind_close(u);
    if (!g.running) return;
    app_abort_branch_summary(u->app);
    pthread_join(g.th, NULL);
    g.running = false;
    u->bg_kind = 0;
    free(g.res.editor_text);
    free(g.err);
    free(g.target);
    free(g.custom);
    memset(&g, 0, sizeof g);
}
