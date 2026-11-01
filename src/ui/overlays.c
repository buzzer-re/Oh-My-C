/* overlays.c - selectors and panels opened from the main screen (ports of omp overlays):
 * the editor-slot selector host (SelectorController.showSelector), the thinking-level selector,
 * the fullscreen command-report panel used by /hotkeys (overlays/report-panel.ts + hotkeys-markdown.ts)
 * and the model picker. */
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
    {"minimal", "min", "Very brief reasoning (~1k tokens)"},
    {"low", "low", "Light reasoning (~2k tokens)"},
    {"medium", "medium", "Moderate reasoning (~8k tokens)"},
    {"high", "high", "Deep reasoning (~16k tokens)"},
    {"xhigh", "xhigh", "Extended reasoning (~32k tokens)"},
    {"max", "max", "Maximum reasoning the model supports"},
};

static void thinking_pick(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    ui_t *u = ud;
    ai_effort_t e;
    bool ok = effort_parse(it->value, &e) == 0;
    ui_close_selector(u); /* frees `it` */
    if (ok) app_set_thinking(u->app, e);
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
    size_t n = 0;
    for (int i = EFFORT_OFF; i <= EFFORT_MAX; i++)
        if (i == EFFORT_OFF || model_supports_effort(a->model, (ai_effort_t)i)) n++;
    tui_select_list_t *l = tui_select_list_new((int)n);
    for (int i = EFFORT_OFF; i <= EFFORT_MAX; i++) {
        if (i != EFFORT_OFF && !model_supports_effort(a->model, (ai_effort_t)i)) continue;
        const char *name = effort_name((ai_effort_t)i);
        for (size_t k = 0; k < ARRAY_LEN(THINKING_META); k++)
            if (!strcmp(THINKING_META[k].value, name)) tui_select_list_add(l, name, THINKING_META[k].label, THINKING_META[k].desc);
    }
    tui_select_list_select_value(l, effort_name(a->thinking));
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
    if (r->fullscreen) tui_term_puts(tui_term(u->tui), "\x1b[?1049l");
    tui_set_focus(u->tui, &u->editor->base);
    tui_request_full_render(u->tui);
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
    tui_term_puts(tui_term(u->tui), "\x1b[?1049h");
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &r->base, &o);
    tui_request_full_render(u->tui);
}

/* ---------- /hotkeys (hotkeys-markdown.ts) ---------- */

static void hk_row(sb_t *b, const char *keys, const char *action) { sb_appendf(b, "| %s | %s |\n", keys, action); }

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

#define HK(...)                                                                                                        \
    do {                                                                                                               \
        static const char *const ks_[] = {__VA_ARGS__};                                                                \
        char *k_ = hk_keys(ks_, ARRAY_LEN(ks_));                                                                       \
        hk_row(&b, k_, act_);                                                                                          \
        free(k_);                                                                                                      \
    } while (0)

static char *hotkeys_markdown(void) {
    sb_t b = SB_INIT;
    const char *act_;
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
    act_ = "Copy current line";
    HK("alt+shift+l");
    act_ = "Copy whole prompt";
    HK("alt+shift+c");
    sb_append(&b, "\n**Other**\n| Key | Action |\n|-----|--------|\n");
    act_ = "Path completion / accept autocomplete";
    HK("tab");
    act_ = "Cancel autocomplete / interrupt active work";
    HK("escape");
    act_ = "Clear editor (first) / exit (second)";
    HK("ctrl+c");
    act_ = "Delete char forward (with draft) / exit (empty prompt)";
    HK("ctrl+d");
    act_ = "Suspend to background";
    HK("ctrl+z");
    act_ = "Reset terminal display";
    HK("alt+l");
    act_ = "Cycle thinking level";
    HK("shift+tab");
    act_ = "Cycle role models (slow/default/smol)";
    HK("ctrl+p");
    act_ = "Cycle role models (backward)";
    HK("shift+ctrl+p");
    act_ = "Select model (temporary)";
    HK("alt+p");
    act_ = "Select model (set roles)";
    HK("alt+m");
    act_ = "Toggle plan mode";
    HK("alt+shift+p");
    act_ = "Search prompt history";
    HK("ctrl+r");
    act_ = "Toggle tool output expansion";
    HK("ctrl+o");
    act_ = "Toggle tool activity visibility";
    HK("ctrl+shift+o");
    act_ = "Hide/show a pending ask question";
    HK("alt+h");
    act_ = "Approve a pending tool call";
    HK("ctrl+enter", "ctrl+q");
    act_ = "Toggle thinking block visibility";
    HK("ctrl+t");
    act_ = "Edit message in external editor";
    HK("ctrl+g");
    act_ = "Open a local:// file in external editor";
    HK("alt+g");
    act_ = "Retry last failed assistant turn";
    HK("f5", "alt+r");
    act_ = "Paste image or text from clipboard";
    HK("ctrl+v");
    {
        char *k = hk_keys((const char *const[]){"space"}, 1);
        char *row = xasprintf("Hold %s", k);
        hk_row(&b, row, "Speech-to-text (push-to-talk): hold to record, release to transcribe");
        free(row);
        free(k);
    }
    act_ = "Start/stop live voice mode (/live)";
    HK("ctrl+l");
    {
        char *a = hk_keys((const char *const[]){"alt+a"}, 1), *s = hk_keys((const char *const[]){"ctrl+s"}, 1),
             *l = hk_keys((const char *const[]){"left"}, 1);
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
    char *md = hotkeys_markdown();
    tui_markdown_t *m = tui_markdown_new(md, 0, 0);
    free(md);
    ui_show_report(u, "Keyboard Shortcuts", &m->base);
}

/* ---------- model picker (/model, alt+m): the model browser in a "Models" panel ---------- */

typedef struct {
    tui_component_t base;
    ui_t *u;
    ui_model_browser_t *b;
} model_view_t;

static void model_view_render(tui_component_t *c, int width, strvec_t *out) {
    model_view_t *v = (model_view_t *)c;
    strvec_t body = {0};
    int inner = MAX(1, width - 4);
    char *head = S(TUI_TC_MUTED, "All available models");
    strvec_push_own(&body, head);
    ui_model_browser_render(v->b, inner, &body);
    strvec_push_own(out, ui_box_top(width, "Models"));
    for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
    strvec_push_own(out, ui_box_divider(width));
    char *e = ui_key_hint("enter"), *esc = ui_key_hint("escape");
    char *up = ui_key_hint("up"), *dn = ui_key_hint("down");
    char *f = xasprintf("%s select \xc2\xb7 %s/%s navigate \xc2\xb7 type to search \xc2\xb7 %s close", e, up, dn, esc);
    strvec_push_own(out, ui_box_row(f, width));
    strvec_push_own(out, ui_box_bottom(width));
    free(f);
    free(e);
    free(esc);
    free(up);
    free(dn);
    strvec_free(&body);
}

static bool model_view_key(tui_component_t *c, const tui_key_t *k) {
    model_view_t *v = (model_view_t *)c;
    int r = ui_model_browser_key(v->b, k);
    if (r == UI_MB_CANCEL) {
        ui_close_selector(v->u);
        return true;
    }
    if (r == UI_MB_ACTIVATE) {
        const ui_model_item_t *it = ui_model_browser_selected(v->b);
        char *sel = it ? xstrdup(it->selector) : NULL;
        ui_t *u = v->u;
        ui_close_selector(u);
        if (sel) {
            char *err = NULL;
            if (app_set_model(u->app, sel, &err) != 0) add_notice(u, "error", err);
            else settings_set_global("modelRoles.default", json_str(sel)); /* set roles: persists default */
            free(err);
            free(sel);
        }
        return true;
    }
    tui_component_request_render(c);
    return true;
}

static void model_view_destroy(tui_component_t *c) {
    model_view_t *v = (model_view_t *)c;
    ui_model_browser_free(v->b);
    free(v);
}

static const tui_component_vt MODEL_VIEW_VT = {"model-picker", model_view_render, model_view_key, NULL, model_view_destroy};

void ui_overlay_model(ui_t *u) {
    model_view_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &MODEL_VIEW_VT);
    v->u = u;
    v->b = ui_model_browser_new();
    ui_model_browser_load_available(v->b);
    v->b->single_pool = v->b->items.n == 1;
    char *cur = app_model_label(u->app);
    ui_model_browser_set_current(v->b, cur);
    free(cur);
    ui_show_selector(u, &v->base, &v->base);
}
