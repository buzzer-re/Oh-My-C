/* hooksel.c - HookSelectorComponent (port of omp packages/tui/src/overlays/hook-selector.ts) for short option
 * menus with descriptions, shown in the composer's place (showHookSelector): an OverlayPanel titled with the
 * title's first line, the remaining title lines in the accent color, the options (cursor + label, a muted
 * description indented under it, the highlighted block on selectedBg) and the dim controls hint. The search /
 * compact mode for lists taller than 12 rows, sliders, markers and timeouts are not ported (no caller needs them).
 */
#include "ui/ui.h"

typedef struct {
    tui_component_t base;
    ui_t *u;
    char *title;
    strvec_t detail, labels, descs;
    char *helptext;
    int sel;
    bool done;
    void (*cb)(void *ud, int choice);
    void *ud;
} hook_sel_t;

/* Text(row, 1, 0, bgFn): one cell of padding each side, wrapped, the selected block painted on selectedBg */
static void row_text(strvec_t *out, const char *text, int width, bool highlight) {
    strvec_t w = {0};
    tui_wrap_text_with_ansi(text, MAX(1, width - 2), &w);
    for (size_t i = 0; i < w.n; i++) {
        char *line = xasprintf(" %s", w.v[i]);
        char *pad = tui_pad_to_width(line, width);
        strvec_push_own(out, highlight ? tui_bgs(NULL, TUI_TC_SELECTED_BG, pad) : xstrdup(pad));
        free(pad);
        free(line);
    }
    strvec_free(&w);
}

static void hs_body(hook_sel_t *h, int width, strvec_t *out) {
    strvec_push(out, "");
    for (size_t i = 0; i < h->detail.n; i++) {
        char *a = tui_fgs(NULL, TUI_TC_ACCENT, h->detail.v[i]);
        ui_push_text(out, a, width);
        free(a);
    }
    strvec_push(out, "");
    for (size_t i = 0; i < h->labels.n; i++) {
        bool selected = (int)i == h->sel;
        char *prefix = selected ? xasprintf("%s ", tui_sym("nav.cursor")) : xstrdup("  ");
        char *pre = selected ? tui_fgs(NULL, TUI_TC_ACCENT, prefix) : xstrdup(prefix);
        char *label = tui_fgs(NULL, selected ? TUI_TC_ACCENT : TUI_TC_TEXT, h->labels.v[i]);
        char *row = xasprintf("%s%s", pre, label);
        row_text(out, row, width, selected);
        free(prefix), free(pre), free(label), free(row);
        if (*h->descs.v[i]) {
            char *d = tui_fgs(NULL, TUI_TC_MUTED, h->descs.v[i]);
            char *drow = xasprintf("    %s", d);
            row_text(out, drow, width, selected);
            free(d), free(drow);
        }
    }
    strvec_push(out, "");
    char *hint;
    if (h->helptext) hint = xstrdup(h->helptext);
    else {
        char *up = ui_editor_key("tui.select.up"), *down = ui_editor_key("tui.select.down");
        char *en = ui_key_hint("enter"), *esc = ui_editor_key("tui.select.cancel");
        hint = xasprintf("%s/%s navigate  %s select  %s cancel", up, down, en, esc);
        free(up), free(down), free(en), free(esc);
    }
    char *dh = tui_fgs(NULL, TUI_TC_DIM, hint);
    ui_push_text(out, dh, width);
    strvec_push(out, "");
    free(dh), free(hint);
}

static void hook_sel_render(tui_component_t *c, int width, strvec_t *out) {
    hook_sel_t *h = (hook_sel_t *)c;
    strvec_push_own(out, ui_box_top(width, h->title));
    strvec_t body = {0};
    hs_body(h, MAX(1, width - 4), &body);
    for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
    strvec_free(&body);
    strvec_push_own(out, ui_box_bottom(width));
}

typedef struct {
    hook_sel_t *h;
    int choice;
} hook_done_t;

static void hook_finish_posted(void *p) {
    hook_done_t *d = p;
    hook_sel_t *h = d->h;
    void (*cb)(void *, int) = h->cb;
    void *ud = h->ud;
    int choice = d->choice;
    ui_close_selector(h->u); /* destroys h */
    free(d);
    if (cb) cb(ud, choice);
}

static void hook_finish(hook_sel_t *h, int choice) {
    if (h->done) return;
    h->done = true;
    hook_done_t *d = xcalloc(1, sizeof *d);
    d->h = h;
    d->choice = choice;
    tui_post(h->u->tui, hook_finish_posted, d);
}

static bool hook_sel_key(tui_component_t *c, const tui_key_t *k) {
    hook_sel_t *h = (hook_sel_t *)c;
    if (h->done) return true;
    int n = (int)h->labels.n;
    if (tui_kb(k, "tui.select.cancel")) hook_finish(h, -1);
    else if (tui_kb(k, "tui.select.up")) h->sel = MAX(0, h->sel - 1); /* MenuSelection.move(-1, false) */
    else if (tui_kb(k, "tui.select.down")) h->sel = MIN(n - 1, h->sel + 1);
    else if (tui_key_matches(k, "enter") || tui_key_matches(k, "return") ||
             (k->kind == TUI_KEY_CHAR && k->codepoint == '\n' && !k->mods))
        hook_finish(h, h->sel);
    tui_component_request_render(c);
    return true;
}

static void hook_sel_destroy(tui_component_t *c) {
    hook_sel_t *h = (hook_sel_t *)c;
    free(h->title);
    free(h->helptext);
    strvec_free(&h->detail);
    strvec_free(&h->labels);
    strvec_free(&h->descs);
    free(h);
}

static const tui_component_vt HOOK_SEL_VT = {"hook-selector", hook_sel_render, hook_sel_key, NULL, hook_sel_destroy};

void ui_hook_select_desc(ui_t *u, const char *title, const char *const *labels, const char *const *descs, size_t n,
                         const char *help, void (*cb)(void *ud, int choice), void *ud) {
    hook_sel_t *h = xcalloc(1, sizeof *h);
    tui_component_init(&h->base, &HOOK_SEL_VT);
    h->u = u;
    strvec_t lines = str_split_lines(title, strlen(title));
    h->title = xstrdup(lines.n ? lines.v[0] : "");
    for (size_t i = 1; i < lines.n; i++) strvec_push(&h->detail, lines.v[i]);
    strvec_free(&lines);
    for (size_t i = 0; i < n; i++) {
        strvec_push(&h->labels, labels[i]);
        strvec_push(&h->descs, descs && descs[i] ? descs[i] : "");
    }
    h->helptext = help ? xstrdup(help) : NULL;
    h->cb = cb;
    h->ud = ud;
    ui_show_selector(u, &h->base, &h->base);
}
