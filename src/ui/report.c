/* report.c - read-only command reports (port of omp overlays/report-panel.ts and CommandController's
 * showCommandReport / #showFullscreenSheet / dismissCommandReport / clearCommandReport).
 *
 * A report that fits the rows above the editor docks there (the reportContainer slot between the /btw panel
 * and the error banner): the editor keeps focus and Esc (app.interrupt) takes it away. A taller one opens as a
 * focused full-screen page on the alternate screen whose body scrolls on the arrow/page/Home/End keys. The
 * same full-screen host serves other focused pages (the /context viewer). */
#include "ui/ui.h"

#include <math.h>

/* ---------- report panel ---------- */

typedef struct {
    tui_component_t base;
    ui_t *u;
    char *title;
    tui_component_t *body;
    int scroll, body_rows, total_rows;
    bool fullscreen; /* holdFocus: the hint names tui.select.cancel; the cap is the terminal height */
} report_t;

/* text-mode rows around the scrolled body: the borders, two spacers and the footer */
#define TEXT_CHROME_ROWS 5

static struct {
    ui_t *u;
    report_t *docked;       /* the report shown above the editor */
    tui_component_t *sheet; /* the focused full-screen page */
    int start, height;      /* the slot's rows in the last frame */
    bool seen;
} R;

static char *close_key(bool fullscreen) {
    char *k = ui_editor_key(fullscreen ? "tui.select.cancel" : "app.interrupt");
    if (*k) return k;
    free(k);
    return ui_key_hint("escape");
}

/* the report box at `width`, cut to `cap` rows (< 0: uncapped) */
static void report_lines(report_t *r, int width, int cap, strvec_t *out) {
    int inner = MAX(1, width - 4);
    strvec_t body = {0};
    tui_component_render(r->body, inner, &body);
    int rows = (int)body.n;
    int body_max = cap < 0 ? rows : MAX(1, cap - TEXT_CHROME_ROWS);
    bool overflows = rows > body_max;
    r->body_rows = MIN(rows, body_max);
    strvec_t lines = {0};
    strvec_push_own(&lines, ui_box_top(width, r->title));
    strvec_push_own(&lines, ui_box_row("", width));
    if (!overflows) {
        r->total_rows = rows;
        r->scroll = 0;
        for (size_t i = 0; i < body.n; i++) strvec_push_own(&lines, ui_box_row(body.v[i], width));
    } else {
        /* ScrollView (scrollbar auto, Ellipsis.Omit): the body one column narrower plus a dim │ track / accent █ thumb */
        strvec_free(&body);
        tui_component_render(r->body, MAX(1, inner - 1), &body);
        rows = (int)body.n;
        r->total_rows = rows;
        int h = r->body_rows, maxoff = MAX(0, rows - h);
        r->scroll = MAX(0, MIN(r->scroll, maxoff));
        int size = MAX(1, MIN((h * h) / MAX(1, rows), h));
        int ts = maxoff ? (int)lround((double)r->scroll / maxoff * (h - size)) : 0;
        for (int i = 0; i < h; i++) {
            int bi = r->scroll + i;
            const char *src = bi < rows ? body.v[bi] : "";
            char *tr = tui_truncate_to_width(src, inner - 1, "");
            char *fit = tui_pad_to_width(tr, inner - 1);
            bool thumb = i >= ts && i < ts + size;
            char *bar = tui_fgs(NULL, thumb ? TUI_TC_ACCENT : TUI_TC_DIM, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
            char *row = xasprintf("%s%s", fit, bar);
            strvec_push_own(&lines, ui_box_row(row, width));
            free(row);
            free(bar);
            free(fit);
            free(tr);
        }
    }
    strvec_push_own(&lines, ui_box_row("", width));
    char *close = close_key(r->fullscreen), *hint;
    if (overflows) {
        char *pu = ui_editor_key("tui.select.pageUp"), *pd = ui_editor_key("tui.select.pageDown");
        hint = xasprintf("%s/%s scroll \xc2\xb7 %s to close", pu, pd, close);
        free(pu);
        free(pd);
    } else {
        hint = xasprintf("%s to close", close);
    }
    char *hs = tui_fgs(NULL, TUI_TC_MUTED, hint);
    strvec_push_own(&lines, ui_box_row(hs, width));
    strvec_push_own(&lines, ui_box_bottom(width));
    free(hs);
    free(hint);
    free(close);
    for (size_t i = 0; i < lines.n && (cap < 0 || (int)i < cap); i++) strvec_push(out, lines.v[i]);
    strvec_free(&lines);
    strvec_free(&body);
}

static void report_render(tui_component_t *c, int width, strvec_t *out) {
    report_t *r = (report_t *)c;
    if (!r->fullscreen) { /* docked: the slot renders it */
        report_lines(r, width, ui_report_rows(r->u), out);
        return;
    }
    int cap = MAX(1, tui_term(r->u->tui)->rows);
    size_t start = out->n;
    report_lines(r, width, cap, out);
    while ((int)(out->n - start) < cap) strvec_push(out, "");
}

static bool report_key(tui_component_t *c, const tui_key_t *k) {
    report_t *r = (report_t *)c;
    if (tui_kb(k, "tui.select.cancel")) {
        ui_close_sheet(r->u);
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

/* ---------- the docking slot (omp reportContainer) ---------- */

static void slot_render(tui_component_t *c, int width, strvec_t *out) {
    size_t start = out->n;
    if (R.docked) tui_component_render(&R.docked->base, width, out);
    R.start = (int)start;
    R.height = (int)(out->n - start);
    R.seen = true;
}

static const tui_component_vt SLOT_VT = {"report-slot", slot_render, NULL, NULL, NULL};

tui_component_t *ui_report_slot_new(ui_t *u) {
    R.u = u;
    tui_component_t *c = xcalloc(1, sizeof *c);
    tui_component_init(c, &SLOT_VT);
    return c;
}

int ui_report_rows(ui_t *u) {
    if (!R.seen || tui_alt_screen_active(u->tui)) return -1;
    const strvec_t *prev = tui_prev_lines(u->tui);
    int below = (int)prev->n - (R.start + R.height);
    if (below < 0) return -1;
    return MAX(0, tui_term(u->tui)->rows - below);
}

/* ---------- full-screen sheet host ---------- */

void ui_close_sheet(ui_t *u) {
    tui_component_t *s = R.sheet;
    if (!s) return;
    R.sheet = NULL;
    tui_overlay_hide(u->tui, s);
    tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    tui_request_render(u->tui);
    tui_component_destroy(s);
}

void ui_show_sheet(ui_t *u, tui_component_t *c) {
    ui_report_clear(u);
    R.sheet = c;
    tui_alt_screen(u->tui, true);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, c, &o);
    tui_request_render(u->tui);
}

bool ui_report_dismiss(ui_t *u) {
    if (!R.docked) return false;
    report_t *r = R.docked;
    R.docked = NULL;
    tui_component_destroy(&r->base);
    tui_request_render(u->tui);
    return true;
}

void ui_report_clear(ui_t *u) {
    ui_report_dismiss(u);
    ui_close_sheet(u);
}

void ui_show_report(ui_t *u, const char *title, tui_component_t *body) {
    ui_report_clear(u);
    report_t *r = xcalloc(1, sizeof *r);
    tui_component_init(&r->base, &REPORT_VT);
    r->u = u;
    char *t = xstrdup(title ? title : "");
    for (char *q = t; *q; q++)
        if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
    r->title = str_trim(t);
    free(t);
    r->body = body;
    tui_term_t *tm = tui_term(u->tui);
    int inline_rows = ui_report_rows(u);
    if (inline_rows < 0) inline_rows = tm->rows;
    /* heightAt: the whole box when nothing caps it */
    strvec_t probe = {0};
    tui_component_render(body, MAX(1, tm->cols - 4), &probe);
    int need = (int)probe.n + TEXT_CHROME_ROWS;
    strvec_free(&probe);
    if (need > inline_rows) {
        r->fullscreen = true;
        ui_show_sheet(u, &r->base);
    } else {
        R.docked = r;
        tui_attach(u->tui, &r->base);
        tui_request_render(u->tui);
    }
}

/* a titled markdown report (#showMarkdownPanel) */
void ui_show_markdown_report(ui_t *u, const char *title, const char *markdown) {
    char *t = str_trim(markdown ? markdown : "");
    tui_markdown_t *m = tui_markdown_new(t, 0, 0);
    free(t);
    ui_show_report(u, title, &m->base);
}
