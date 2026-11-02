/* overlays.c - selectors and panels opened from the main screen (ports of omp overlays):
 * the editor-slot selector host (SelectorController.showSelector), the thinking-level selector,
 * the fullscreen command-report panel used by /hotkeys (overlays/report-panel.ts + hotkeys-markdown.ts).
 * The Models hub (/model, alt+m) lives in modelhub.c. */
#include "ui/overlays.h"
#include "ui/modelbrowser.h"
#include "ui/ui.h"

#include "config/settings.h"
#include "tui/markdown.h"

#include <math.h>

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

/* ---------- selector host: the selector replaces the composer (editor + its status rows) ---------- */

static tui_component_t *g_sel;
static bool g_editor_hidden, g_status_hidden;

bool ui_selector_open(const ui_t *u) { return g_sel != NULL; }

static size_t root_index(ui_t *u, tui_component_t *c) {
    tui_container_t *root = tui_root(u->tui);
    for (size_t i = 0; i < root->children.n; i++)
        if (root->children.v[i] == c) return i;
    return root->children.n;
}

void ui_close_selector(ui_t *u) {
    if (!g_sel) return;
    tui_component_t *c = g_sel;
    g_sel = NULL;
    tui_container_remove(tui_root(u->tui), c);
    tui_component_destroy(c);
    u->editor->base.hidden = g_editor_hidden;
    if (u->status) u->status->hidden = g_status_hidden;
    tui_set_focus(u->tui, &u->editor->base);
    tui_request_render(u->tui);
}

void ui_show_selector(ui_t *u, tui_component_t *comp, tui_component_t *focus) {
    ui_close_selector(u);
    g_sel = comp;
    g_editor_hidden = u->editor->base.hidden;
    g_status_hidden = u->status ? u->status->hidden : false;
    tui_container_insert(tui_root(u->tui), root_index(u, &u->editor->base) + 1, comp);
    u->editor->base.hidden = true;
    if (u->status) u->status->hidden = true;
    tui_attach(u->tui, comp);
    tui_set_focus(u->tui, focus ? focus : comp);
    tui_request_render(u->tui);
}

/* ---------- thinking selector (overlays/thinking-selector.ts) ---------- */

static const struct {
    const char *value, *label, *desc;
} THINKING_META[] = {
    {"off", "off", "No reasoning"},
    {"auto", "auto", "Auto-detect per prompt"},
    {"minimal", "min", "Very brief reasoning (~1k tokens)"},
    {"low", "low", "Light reasoning (~2k tokens)"},
    {"medium", "medium", "Moderate reasoning (~8k tokens)"},
    {"high", "high", "Deep reasoning (~16k tokens)"},
    {"xhigh", "xhigh", "Extended reasoning (~32k tokens)"},
    {"max", "max", "Maximum reasoning the model supports"},
};

static void thinking_pick(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    ui_t *u = ud;
    char *level = xstrdup(it->value);
    ui_close_selector(u); /* frees `it` */
    app_set_thinking_level(u->app, level);
    free(level);
    tui_request_render(u->tui);
}

static void selector_cancel(tui_select_list_t *l, void *ud) { ui_close_selector((ui_t *)ud); }

void ui_overlay_thinking(ui_t *u) {
    app_t *a = u->app;
    if (!a->model || !a->model->reasoning) {
        char *m = a->model ? xasprintf("%s/%s has no adjustable thinking level.", a->model->provider, a->model->id)
                           : xstrdup("The current model has no adjustable thinking level.");
        add_notice(u, "info", m);
        free(m);
        return;
    }
    ui_panel_t *p = ui_panel_new("Thinking Level");
    /* getAvailableEffortSelectors: off, auto (never for an unverified ladder), then the supported efforts */
    bool auto_ok = !json_get_bool(a->model->thinking, "unverified", false);
    strvec_t levels = {0};
    strvec_push(&levels, "off");
    if (auto_ok) strvec_push(&levels, "auto");
    for (int i = EFFORT_MINIMAL; i <= EFFORT_MAX; i++)
        if (model_supports_effort(a->model, (ai_effort_t)i)) strvec_push(&levels, effort_name((ai_effort_t)i));
    tui_select_list_t *l = tui_select_list_new((int)levels.n);
    for (size_t i = 0; i < levels.n; i++)
        for (size_t k = 0; k < ARRAY_LEN(THINKING_META); k++)
            if (!strcmp(THINKING_META[k].value, levels.v[i]))
                tui_select_list_add(l, levels.v[i], THINKING_META[k].label, THINKING_META[k].desc);
    strvec_free(&levels);
    tui_select_list_select_value(l, a->auto_thinking ? "auto" : effort_name(a->thinking));
    l->on_select = thinking_pick;
    l->on_cancel = selector_cancel;
    l->ud = u;
    tui_container_add(&p->c, &l->base);
    ui_show_selector(u, &p->c.base, &l->base);
}

/* ---------- report panel (overlays/report-panel.ts), fullscreen when taller than the screen ---------- */

typedef struct {
    tui_component_t base;
    ui_t *u;
    char *title;
    tui_component_t *body;
    int scroll, body_rows, total_rows;
    bool fullscreen;
} report_t;

#define TEXT_CHROME_ROWS 5

static void report_render(tui_component_t *c, int width, strvec_t *out) {
    report_t *r = (report_t *)c;
    int inner = MAX(1, width - 4);
    strvec_t body = {0};
    tui_component_render(r->body, MAX(1, inner - 1), &body);
    int rows = (int)body.n;
    int cap = r->fullscreen ? MAX(1, tui_term(r->u->tui)->rows) : -1;
    int body_max = cap < 0 ? rows : MAX(1, cap - TEXT_CHROME_ROWS);
    bool overflows = rows > body_max;
    r->body_rows = MIN(rows, body_max);
    r->total_rows = rows;
    r->scroll = MAX(0, MIN(r->scroll, rows - r->body_rows));
    strvec_t lines = {0};
    strvec_push_own(&lines, ui_box_top(width, r->title));
    strvec_push_own(&lines, ui_box_row("", width));
    /* ScrollView(scrollbar auto): rows rendered one column narrower when the content overflows */
    if (!overflows) {
        strvec_t full = {0};
        tui_component_render(r->body, inner, &full);
        for (size_t i = 0; i < full.n; i++) strvec_push_own(&lines, ui_box_row(full.v[i], width));
        strvec_free(&full);
    } else {
        int h = r->body_rows, size = MAX(1, MIN((h * h) / rows, h)), maxoff = rows - h;
        int ts = maxoff ? (int)lround((double)r->scroll / maxoff * (h - size)) : 0;
        for (int i = 0; i < h; i++) {
            const char *src = body.v[r->scroll + i];
            char *fit = ui_fit_line(src, inner - 1);
            bool thumb = i >= ts && i < ts + size;
            char *bar = tui_fgs(NULL, thumb ? TUI_TC_ACCENT : TUI_TC_DIM, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
            char *row = xasprintf("%s%s", fit, bar);
            strvec_push_own(&lines, ui_box_row(row, width));
            free(row);
            free(bar);
            free(fit);
        }
    }
    strvec_push_own(&lines, ui_box_row("", width));
    char *close = ui_editor_key(r->fullscreen ? "tui.select.cancel" : "app.interrupt");
    if (!*close) {
        free(close);
        close = ui_key_hint("escape");
    }
    char *hint;
    if (overflows) {
        char *pu = ui_editor_key("tui.select.pageUp"), *pd = ui_editor_key("tui.select.pageDown");
        hint = xasprintf("%s/%s scroll \xc2\xb7 %s to close", pu, pd, close);
        free(pu);
        free(pd);
    } else {
        hint = xasprintf("%s to close", close);
    }
    char *hs = S(TUI_TC_MUTED, hint);
    strvec_push_own(&lines, ui_box_row(hs, width));
    strvec_push_own(&lines, ui_box_bottom(width));
    free(hs);
    free(hint);
    free(close);
    for (size_t i = 0; i < lines.n; i++) {
        if (cap >= 0 && (int)i >= cap) break;
        strvec_push(out, lines.v[i]);
    }
    if (r->fullscreen)
        for (int i = (int)lines.n; i < cap; i++) strvec_push(out, "");
    strvec_free(&lines);
    strvec_free(&body);
}

static void report_close(report_t *r) {
    ui_t *u = r->u;
    tui_overlay_hide(u->tui, &r->base);
    if (r->fullscreen) tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    tui_request_render(u->tui);
    tui_component_destroy(&r->base);
}

static bool report_key(tui_component_t *c, const tui_key_t *k) {
    report_t *r = (report_t *)c;
    if (tui_kb(k, "tui.select.cancel")) {
        report_close(r);
        return true;
    }
    int page = MAX(1, r->body_rows - 1);
    if (tui_kb(k, "tui.select.up")) r->scroll--;
    else if (tui_kb(k, "tui.select.down")) r->scroll++;
    else if (tui_kb(k, "tui.select.pageUp")) r->scroll -= page;
    else if (tui_kb(k, "tui.select.pageDown")) r->scroll += page;
    else if (tui_key_matches(k, "home")) r->scroll = 0;
    else if (tui_key_matches(k, "end")) r->scroll = r->total_rows;
    else return true;
    tui_component_request_render(c);
    return true;
}

static void report_destroy(tui_component_t *c) {
    report_t *r = (report_t *)c;
    tui_component_destroy(r->body);
    free(r->title);
    free(r);
}

static const tui_component_vt REPORT_VT = {"report-panel", report_render, report_key, NULL, report_destroy};

void ui_show_report(ui_t *u, const char *title, tui_component_t *body) {
    report_t *r = xcalloc(1, sizeof *r);
    tui_component_init(&r->base, &REPORT_VT);
    r->u = u;
    r->title = xstrdup(title);
    r->body = body;
    /* fullscreen page on the alternate screen (the text-mode report that does not fit inline) */
    r->fullscreen = true;
    tui_alt_screen(u->tui, true);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &r->base, &o);
}

/* ---------- /hotkeys (hotkeys-markdown.ts) ---------- */

static void hk_row(sb_t *b, const char *keys, const char *action) { sb_appendf(b, "| %s | %s |\n", keys, action); }

/* formatKeyHints of a key list, in backticks (markdownPart) */
static char *hk_keys(const char *const *keys, size_t n) {
    sb_t b = SB_INIT;
    sb_putc(&b, '`');
    for (size_t i = 0; i < n; i++) {
        char *h = ui_key_hint(keys[i]);
        sb_appendf(&b, "%s%s", i ? "/" : "", h);
        free(h);
    }
    sb_putc(&b, '`');
    return sb_detach(&b);
}

/* an action's effective keys; `Disabled` when unbound */
static char *hk_act(const char *action) {
    const strvec_t *v = tui_keybindings_get(tui_keybindings_global(), action);
    if (!v || !v->n) return xstrdup("`Disabled`");
    return hk_keys((const char *const *)v->v, v->n);
}

#define HK(...)                                                                                                        \
    do {                                                                                                               \
        static const char *const ks_[] = {__VA_ARGS__};                                                                \
        char *k_ = hk_keys(ks_, ARRAY_LEN(ks_));                                                                       \
        hk_row(&b, k_, act_);                                                                                          \
        free(k_);                                                                                                      \
    } while (0)

#define HA(ACTION, TEXT)                                                                                               \
    do {                                                                                                               \
        char *k_ = hk_act(ACTION);                                                                                     \
        hk_row(&b, k_, TEXT);                                                                                          \
        free(k_);                                                                                                      \
    } while (0)

/* buildHotkeysMarkdown over the live bindings (hotkeys-markdown.ts) */
char *ui_hotkeys_markdown(void) {
    sb_t b = SB_INIT;
    const char *act_;
    tui_keybindings_t *kb = tui_keybindings_global();
    char *up = ui_key_hint("up");
    sb_append(&b, "**Navigation**\n| Key | Action |\n|-----|--------|\n");
    char *nav = xasprintf("Move cursor / browse history (%s when empty)", up);
    act_ = nav;
    HK("up", "down", "left", "right");
    free(nav);
    free(up);
    act_ = "Move by word";
    HK("alt+left", "alt+right");
#ifdef __APPLE__
    act_ = "Start of line";
    HK("ctrl+a", "home", "super+left");
    act_ = "End of line";
    HK("ctrl+e", "end", "super+right");
#else
    act_ = "Start of line";
    HK("ctrl+a", "home");
    act_ = "End of line";
    HK("ctrl+e", "end");
#endif
    sb_append(&b, "\n**Editing**\n| Key | Action |\n|-----|--------|\n");
    act_ = "Send message";
    HK("enter");
    act_ = "New line";
    HK("shift+enter", "alt+enter");
    act_ = "Delete word backwards";
    HK("ctrl+w", "alt+backspace");
    act_ = "Delete to start of line";
    HK("ctrl+u");
    act_ = "Delete to end of line";
    HK("ctrl+k");
    HA("app.clipboard.copyLine", "Copy current line");
    HA("app.clipboard.copyPrompt", "Copy whole prompt");
    sb_append(&b, "\n**Other**\n| Key | Action |\n|-----|--------|\n");
    act_ = "Path completion / accept autocomplete";
    HK("tab");
    HA("app.interrupt", "Cancel autocomplete / interrupt active work");
    HA("app.clear", "Clear editor (first) / exit (second)");
    {
        /* exit keys split by role: one that also forward-deletes (readline ^D) does so with a draft */
        const strvec_t *ex = tui_keybindings_get(kb, "app.exit");
        strvec_t del = {0}, quit = {0};
        for (size_t i = 0; ex && i < ex->n; i++)
            strvec_push(tui_keybindings_key_bound(kb, ex->v[i], "tui.editor.deleteCharForward") ? &del : &quit, ex->v[i]);
        if (del.n) {
            char *k = hk_keys((const char *const *)del.v, del.n);
            hk_row(&b, k, "Delete char forward (with draft) / exit (empty prompt)");
            free(k);
        }
        if (quit.n || !del.n) {
            char *k = quit.n ? hk_keys((const char *const *)quit.v, quit.n) : xstrdup("`Disabled`");
            hk_row(&b, k, "Exit");
            free(k);
        }
        strvec_free(&del);
        strvec_free(&quit);
    }
    HA("app.suspend", "Suspend to background");
    HA("app.display.reset", "Reset terminal display");
    HA("app.thinking.cycle", "Cycle thinking level");
    HA("app.model.cycleForward", "Cycle role models (slow/default/smol)");
    HA("app.model.cycleBackward", "Cycle role models (backward)");
    HA("app.model.selectTemporary", "Select model (temporary)");
    HA("app.model.select", "Select model (set roles)");
    HA("app.plan.toggle", "Toggle plan mode");
    HA("app.history.search", "Search prompt history");
    HA("app.tools.expand", "Toggle tool output expansion");
    HA("app.tools.toggleVisibility", "Toggle tool activity visibility");
    HA("app.links.hint", "Pick an extension link on screen by its tag");
    HA("app.ask.hide", "Hide/show a pending ask question");
    HA("app.approval.approve", "Approve a pending tool call");
    HA("app.thinking.toggle", "Toggle thinking block visibility");
    HA("app.editor.external", "Edit message in external editor");
    HA("app.local.open", "Open a local:// file in external editor");
    HA("app.retry", "Retry last failed assistant turn");
    HA("app.clipboard.pasteImage", "Paste image or text from clipboard");
    {
        const strvec_t *ptt = tui_keybindings_get(kb, "app.stt.pushToTalk");
        char *k = hk_act("app.stt.pushToTalk");
        if (ptt && ptt->n) {
            char *row = xasprintf("Hold %s", k);
            hk_row(&b, row, "Speech-to-text (push-to-talk): hold to record, release to transcribe");
            free(row);
        } else {
            hk_row(&b, k, "Speech-to-text (push-to-talk) (disabled)");
        }
        free(k);
    }
    HA("app.live.toggle", "Start/stop live voice mode (/live)");
    {
        char *a = hk_act("app.agents.hub"), *s = hk_act("app.session.observe"), *l = hk_keys((const char *const[]){"left"}, 1);
        char *row = xasprintf("%s / %s / double-tap %s (empty editor)", a, s, l);
        hk_row(&b, row, "Open the agent hub");
        free(row);
        free(a);
        free(s);
        free(l);
    }
    hk_row(&b, "`#<number>`", "GitHub issue/PR reference (e.g. `#3164` \xe2\x86\x92 `pr://`/`issue://`)");
    hk_row(&b, "`#` / `#<text>`", "Prompt actions (copy / undo / move cursor)");
    hk_row(&b, "`/`", "Slash commands");
    hk_row(&b, "`!`", "Run bash command");
    hk_row(&b, "`!!`", "Run bash command (excluded from context)");
    hk_row(&b, "`$`", "Run Python in shared kernel");
    hk_row(&b, "`$$`", "Run Python (excluded from context)");
    char *md = sb_detach(&b);
    char *t = str_trim(md);
    free(md);
    return t;
}

void ui_overlay_hotkeys(ui_t *u) {
    char *md = ui_hotkeys_markdown();
    tui_markdown_t *m = tui_markdown_new(md, 0, 0);
    free(md);
    ui_show_report(u, "Keyboard Shortcuts", &m->base);
}

