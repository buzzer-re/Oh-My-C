/* ask.c - the ask tool's dialog (port of omp overlays/ask-dialog.ts AskDialogComponent, hosted in the
 * composer's place like extension-ui-controller #showLocalAskDialog) and its custom-answer / note prompt
 * (overlays/hook-editor.ts, prompt style).
 *
 * The dialog: an "Ask" panel (title "Ask (Ns)" while an ask.timeout countdown runs) with a header
 * (question tabs + Submit when there are several questions or a multi-select one, then the question as
 * markdown, capped at 4 rows), a fixed-height body (options as radio/check rows with descriptions,
 * previews and an "Other (type your own)" row, or the "Review answers" list) and a one-line key-hint
 * footer. The height is measured once from the tallest tab (≤ 70% of the terminal) so it never jumps.
 * Alt+H collapses it to one line; Ctrl+O expands truncated questions/descriptions; a composer draft
 * present when the dialog opened keeps the keyboard until ↑/↓ hands it over. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include "config/settings.h"
#include "tui/markdown.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>

#define OTHER_OPTION "Other (type your own)"
#define SUBMIT_OPTION "Submit"
#define RECOMMENDED_SUFFIX " (Recommended)"
#define DIALOG_HEIGHT_RATIO 0.7
#define MIN_DIALOG_ROWS 12
#define MIN_BODY_ROWS 5
#define MAX_HEADER_CHIP_WIDTH 16
#define MAX_HEADER_ROWS 4
#define MAX_DESC_ROWS 2
#define MAX_PROMPT_TITLE_ROWS 3

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

typedef struct {
    char *label, *description, *preview; /* description/preview NULL when absent/blank */
} aopt_t;

typedef struct {
    char *id, *question, *header;
    aopt_t *opts;
    size_t nopt;
    bool multi;
    int recommended; /* -1 = none */
    char **display;  /* display labels (badged, disambiguated) */
} aq_t;

typedef struct {
    bool *sel; /* per option */
    char *custom;
    char *note;
    int note_row; /* -1 none; option index; nopt = the Other row */
    int cursor, scroll;
    bool manual_scroll, timed_out;
} ast_t;

typedef struct ask_view {
    tui_component_t base;
    ui_t *u;
    aq_t *q;
    ast_t *st;
    size_t nq;
    size_t active_tab;
    int submit_scroll, body_rows, content_width;
    bool can_page, closed, expanded, hidden, header_exp, desc_exp;
    int stable_key_w, stable_key_rows, stable_key_exp, stable_total;
    int just_q, just_row; /* just_q < 0: none */
    /* countdown (ask.timeout) */
    int64_t timeout_ms, deadline;
    int remaining; /* -1: no countdown */
    int timer_id;
    /* draft guard */
    bool guard, guard_released;
    /* prompt (custom answer / note) */
    tui_editor_t *ped;
    char *ptitle;
    strvec_t pdetail;
    int pkind; /* 0 none, 1 custom answer, 2 note */
    size_t pq;
    int prow;
    ui_ask_done_fn done;
    void *ud;
} ask_view_t;

/* ---------- text helpers ---------- */

static char *sanitize_cr(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\r') {
            if (p[1] == '\n') continue;
            while (p[1] == '\r') p++;
            sb_putc(&b, ' ');
        } else sb_putc(&b, *p);
    }
    return sb_detach(&b);
}

/* replaceTabs + collapse whitespace + trim */
static char *inline_input(const char *s) {
    char *t = tui_replace_tabs(s ? s : "");
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = t; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') sp = true;
        else {
            if (sp && b.len) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *p);
        }
    }
    free(t);
    return sb_detach(&b);
}

static bool blank(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

/* renderPreviewContent: fenced code segments highlighted, the rest as markdown in `slot` */
static void preview_content(const char *text, int width, int slot, strvec_t *out) {
    strvec_t lines = str_split_lines(text, strlen(text));
    sb_t md = SB_INIT, code = SB_INIT;
    char fch = 0;
    size_t flen = 0;
    char *lang = NULL;
    tui_md_opts_t o;
    tui_md_opts_default(&o);
    o.text_slot = slot;
    width = MAX(1, width);
    for (size_t i = 0; i <= lines.n; i++) {
        const char *l = i < lines.n ? lines.v[i] : NULL;
        const char *t = l;
        while (t && *t == ' ') t++;
        if (l && !fch && t - l < 4 && (*t == '`' || *t == '~')) {
            size_t n = 0;
            while (t[n] == *t) n++;
            if (n >= 3) {
                if (md.len) {
                    tui_md_render(sb_cstr(&md), width, &o, out);
                    sb_clear(&md);
                }
                fch = *t;
                flen = n;
                char *lg = str_trim(t + n);
                char *sp = strpbrk(lg, " \t");
                if (sp) *sp = 0;
                free(lang);
                lang = *lg ? lg : (free(lg), NULL);
                sb_clear(&code);
                continue;
            }
        }
        if (fch) {
            size_t n = 0;
            if (l)
                while (t[n] == fch) n++;
            if (!l || (n >= flen && blank(t + n))) {
                strvec_t hl = {0};
                tui_highlight_code(sb_cstr(&code), lang ? lang : "", NULL, &hl);
                for (size_t k = 0; k < hl.n; k++) ui_push_text(out, hl.v[k], width), (void)0;
                if (!hl.n) strvec_push(out, "");
                strvec_free(&hl);
                fch = 0;
                continue;
            }
            if (code.len) sb_putc(&code, '\n');
            sb_append(&code, l);
            continue;
        }
        if (!l) break;
        if (md.len) sb_putc(&md, '\n');
        sb_append(&md, l);
    }
    if (md.len) tui_md_render(sb_cstr(&md), width, &o, out);
    sb_free(&md);
    sb_free(&code);
    free(lang);
    strvec_free(&lines);
}

static char *inline_md(const char *text, int slot) {
    tui_md_opts_t o;
    tui_md_opts_default(&o);
    o.text_slot = slot;
    return tui_md_inline(text, &o);
}

/* ---------- questions ---------- */

static void build_display(aq_t *q) {
    strvec_t taken = {0};
    strvec_push(&taken, OTHER_OPTION);
    strvec_push(&taken, "Chat about this");
    strvec_push(&taken, "Next \xe2\x86\x92");
    q->display = xcalloc(q->nopt + 1, sizeof *q->display);
    for (size_t i = 0; i < q->nopt; i++) {
        char *base = sanitize_cr(q->opts[i].label);
        if (q->recommended == (int)i && !str_ends(base, RECOMMENDED_SUFFIX)) {
            char *b2 = xasprintf("%s" RECOMMENDED_SUFFIX, base);
            free(base);
            base = b2;
        }
        char *bs = sanitize_cr(base);
        free(base);
        char *cand = xstrdup(bs);
        for (int suffix = 2; strvec_contains(&taken, cand); suffix++) {
            free(cand);
            cand = xasprintf("%s (%d)", bs, suffix);
        }
        free(bs);
        strvec_push(&taken, cand);
        q->display[i] = cand;
    }
    strvec_free(&taken);
}

static const char *row_label(const aq_t *q, int row) { return row < (int)q->nopt ? q->display[row] : OTHER_OPTION; }
static int row_count(const aq_t *q) { return (int)q->nopt + 1; }
static bool is_answered(const aq_t *q, const ast_t *s) {
    for (size_t i = 0; i < q->nopt; i++)
        if (s->sel[i]) return true;
    return s->custom != NULL;
}
static bool label_selected(const aq_t *q, const ast_t *s, size_t i) {
    for (size_t k = 0; k < q->nopt; k++)
        if (s->sel[k] && !strcmp(q->opts[k].label, q->opts[i].label)) return true;
    return false;
}

static void clear_note(ast_t *s) {
    free(s->note);
    s->note = NULL;
    s->note_row = -1;
}

/* the note that goes out with the answer (its row must be part of the answer) */
static const char *submitted_note(const aq_t *q, const ast_t *s, int *opt) {
    if (opt) *opt = -1;
    if (!s->note || s->note_row < 0) return NULL;
    if (s->note_row == (int)q->nopt) return s->custom ? s->note : NULL;
    if (s->note_row < (int)q->nopt && s->sel[s->note_row]) {
        if (opt) *opt = s->note_row;
        return s->note;
    }
    return NULL;
}

static char *tab_name(const aq_t *q, size_t index) {
    char *h = q->header ? str_trim(q->header) : xstrdup("");
    char *src = *h ? h : NULL;
    char *id = sanitize_cr(q->id);
    char *name = src ? tui_replace_tabs(src) : *id ? tui_replace_tabs(id) : xasprintf("Q%zu", index + 1);
    free(h);
    free(id);
    return name;
}

static char *tab_label(const aq_t *q, size_t index) {
    char *n = tab_name(q, index);
    char *t = tui_truncate_to_width(n, MAX_HEADER_CHIP_WIDTH, NULL);
    free(n);
    return t;
}

static void wrap_title(const aq_t *q, int width, strvec_t *out) {
    char *t = sanitize_cr(q->question);
    preview_content(t, MAX(1, width), TUI_TC_TEXT, out);
    free(t);
}

static void render_title(const aq_t *q, int width, int max_rows, strvec_t *out) {
    strvec_t w = {0};
    wrap_title(q, width, &w);
    if ((int)w.n <= max_rows) {
        for (size_t i = 0; i < w.n; i++) strvec_push(out, w.v[i]);
    } else {
        for (int i = 0; i < max_rows - 1; i++) strvec_push(out, w.v[i]);
        sb_t rest = SB_INIT;
        for (size_t i = (size_t)MAX(0, max_rows - 1); i < w.n; i++) {
            if (rest.len) sb_putc(&rest, ' ');
            sb_append(&rest, w.v[i]);
        }
        strvec_push_own(out, tui_truncate_to_width(sb_cstr(&rest), MAX(1, width), NULL));
        sb_free(&rest);
    }
    strvec_free(&w);
}

static void wrap_desc(const char *desc, int content_width, strvec_t *out) {
    char *t = str_trim(desc);
    char *r = inline_md(t, TUI_TC_MUTED);
    tui_wrap_text_with_ansi(r, MAX(1, content_width - 6), out);
    free(r);
    free(t);
}

static bool descs_overflow(const aq_t *q, int content_width) {
    for (size_t i = 0; i < q->nopt; i++) {
        if (!q->opts[i].description) continue;
        strvec_t w = {0};
        wrap_desc(q->opts[i].description, content_width, &w);
        bool over = w.n > MAX_DESC_ROWS;
        strvec_free(&w);
        if (over) return true;
    }
    return false;
}

static const char *marker(bool multi, bool checked) {
    return tui_sym(multi ? (checked ? "checkbox.checked" : "checkbox.unchecked")
                         : (checked ? "radio.selected" : "radio.unselected"));
}

/* renderRowLabel */
static void render_row(const aq_t *q, const ast_t *s, int row, bool selected, int width, bool expanded, strvec_t *out) {
    bool is_other = row >= (int)q->nopt;
    bool checked = is_other ? s->custom != NULL : label_selected(q, s, (size_t)row);
    tui_tc_t color = selected ? TUI_TC_ACCENT : checked ? TUI_TC_TOOL_OUTPUT : TUI_TC_TEXT;
    char *mk = S(checked ? TUI_TC_SUCCESS : TUI_TC_DIM, marker(q->multi, checked));
    char *mark = xasprintf("%s ", mk);
    free(mk);
    char *cur = NULL;
    if (selected) {
        char *c = xasprintf("%s ", tui_sym("nav.cursor"));
        cur = S(TUI_TC_ACCENT, c);
        free(c);
    } else cur = xstrdup("  ");
    char *label = inline_md(row_label(q, row), color);
    char *note = s->note && s->note_row == row ? S(TUI_TC_SUCCESS, "  \xe2\x9c\x8e note") : xstrdup("");
    int cw = tui_visible_width(cur), mw = tui_visible_width(mark), nw = tui_visible_width(note);
    strvec_t wl = {0};
    tui_wrap_text_with_ansi(label, MAX(1, width - cw - mw - nw), &wl);
    strvec_push_own(out, xasprintf("%s%s%s%s", cur, mark, wl.n ? wl.v[0] : "", note));
    for (size_t i = 1; i < wl.n; i++) strvec_push_own(out, xasprintf("%*s%s", cw + mw, "", wl.v[i]));
    strvec_free(&wl);
    if (!is_other) {
        const aopt_t *o = &q->opts[row];
        if (o->description) {
            strvec_t w = {0};
            wrap_desc(o->description, width, &w);
            size_t shown = expanded ? w.n : MIN(w.n, (size_t)MAX_DESC_ROWS);
            for (size_t i = 0; i < shown; i++) {
                char *t = tui_truncate_to_width(w.v[i], MAX(1, width - 6), NULL);
                strvec_push_own(out, xasprintf("      %s", t));
                free(t);
            }
            strvec_free(&w);
        }
        if (o->preview) {
            strvec_t p = {0};
            preview_content(o->preview, MAX(1, width - 8), TUI_TC_MUTED, &p);
            char *bar = S(TUI_TC_BORDER, "\xe2\x94\x82");
            for (size_t i = 0; i < p.n; i++) strvec_push_own(out, xasprintf("      %s %s", bar, p.v[i]));
            free(bar);
            strvec_free(&p);
        }
    } else if (s->custom) {
        char *pv = inline_input(s->custom);
        char *t = tui_truncate_to_width(pv, MAX(1, width - 6), NULL);
        char *l = xasprintf("      %s", t);
        strvec_push_own(out, S(TUI_TC_MUTED, l));
        free(l);
        free(t);
        free(pv);
    }
    free(cur);
    free(mark);
    free(label);
    free(note);
}

/* ---------- dialog state ---------- */

static bool has_submit_tab(const ask_view_t *v) {
    if (v->nq > 1) return true;
    for (size_t i = 0; i < v->nq; i++)
        if (v->q[i].multi) return true;
    return false;
}
static size_t submit_tab(const ask_view_t *v) { return v->nq; }
static bool on_submit_tab(const ask_view_t *v) { return has_submit_tab(v) && v->active_tab == submit_tab(v); }
static size_t cur_q(const ask_view_t *v) { return v->nq ? MIN(v->active_tab, v->nq - 1) : 0; }

static void request(ask_view_t *v) {
    if (v->u && v->u->tui) tui_request_render(v->u->tui);
}

static int term_rows(const ask_view_t *v) {
    int r = v->u && v->u->tui ? tui_term(v->u->tui)->rows : 0;
    return r > 0 ? r : 40;
}

static int measure_height(ask_view_t *v, int width, int rows) {
    int maxh = MAX(MIN_DIALOG_ROWS, (int)floor(rows * DIALOG_HEIGHT_RATIO));
    int chrome = 5, tabrows = has_submit_tab(v) ? 1 : 0, needed = MIN_DIALOG_ROWS;
    for (size_t i = 0; i < v->nq; i++) {
        strvec_t t = {0};
        render_title(&v->q[i], width, v->expanded ? INT_MAX : MAX_HEADER_ROWS, &t);
        int header = tabrows + (int)t.n;
        strvec_free(&t);
        strvec_t body = {0};
        for (int r = 0; r < row_count(&v->q[i]); r++) render_row(&v->q[i], &v->st[i], r, false, width, v->expanded, &body);
        needed = MAX(needed, chrome + header + MAX(MIN_BODY_ROWS, (int)body.n));
        strvec_free(&body);
    }
    if (has_submit_tab(v)) needed = MAX(needed, chrome + tabrows + 1 + MAX(MIN_BODY_ROWS, 2 + (int)v->nq + 2));
    return MIN(needed, maxh);
}

static int dialog_height(ask_view_t *v, int width, int rows) {
    if (v->stable_total > 0 && v->stable_key_w == width && v->stable_key_rows == rows && v->stable_key_exp == v->expanded)
        return v->stable_total;
    v->stable_total = measure_height(v, width, rows);
    v->stable_key_w = width;
    v->stable_key_rows = rows;
    v->stable_key_exp = v->expanded;
    return v->stable_total;
}

static char *title_text(const ask_view_t *v) {
    return v->remaining < 0 ? xstrdup("Ask") : xasprintf("Ask (%ds)", v->remaining);
}

static bool guard_blocks(const ask_view_t *v) { return v->guard && !v->guard_released && !tui_editor_is_empty(v->u->editor); }

static char *key2(const char *a, const char *b) {
    char *x = ui_key_hint(a), *y = ui_key_hint(b), *r = xasprintf("%s/%s", x, y);
    free(x);
    free(y);
    return r;
}

static char *guard_hint(void) {
    char *ud = key2("up", "down"), *r = xasprintf("%s answer \xc2\xb7 typing edits your prompt", ud);
    free(ud);
    return r;
}

static void render_header(ask_view_t *v, int width, int max_title_rows, strvec_t *out) {
    if (has_submit_tab(v)) {
        ui_tab_t *tabs = xcalloc(v->nq + 1, sizeof *tabs);
        char **labels = xcalloc(v->nq + 1, sizeof *labels);
        for (size_t i = 0; i < v->nq; i++) tabs[i].label = labels[i] = tab_label(&v->q[i], i);
        tabs[v->nq].label = "Submit";
        ui_tab_bar_render("", tabs, v->nq + 1, v->active_tab, false, width, out);
        for (size_t i = 0; i < v->nq; i++) free(labels[i]);
        free(labels);
        free(tabs);
    }
    if (on_submit_tab(v)) {
        v->header_exp = v->desc_exp = false;
        char *a = S(TUI_TC_ACCENT, "Review answers");
        strvec_push_own(out, tui_bold(a));
        free(a);
        return;
    }
    if (!v->nq) {
        v->header_exp = v->desc_exp = false;
        return;
    }
    const aq_t *q = &v->q[cur_q(v)];
    strvec_t w = {0};
    wrap_title(q, width, &w);
    v->header_exp = w.n > MAX_HEADER_ROWS;
    strvec_free(&w);
    v->desc_exp = descs_overflow(q, width);
    render_title(q, width, v->expanded ? max_title_rows : MAX_HEADER_ROWS, out);
}

static char *expand_hint(const ask_view_t *v) {
    if (!v->header_exp && !v->desc_exp) return xstrdup("");
    char *k = ui_editor_key("app.tools.expand"), *r = xasprintf(" \xc2\xb7 %s %s", k, v->expanded ? "collapse" : "expand");
    free(k);
    return r;
}

static char *footer_text(ask_view_t *v, const char *indicator) {
    char *hide = ui_editor_key("app.ask.hide"), *esc = ui_editor_key("tui.select.cancel");
    char *cancel = xasprintf("%s cancel \xc2\xb7 %s hide", esc, hide);
    char *enter = ui_key_hint("enter"), *updown = key2("up", "down"), *exp = expand_hint(v), *r;
    if (guard_blocks(v)) {
        char *g = guard_hint();
        r = xasprintf("%s%s \xc2\xb7 %s", g, exp, cancel);
        free(g);
    } else if (on_submit_tab(v)) {
        char *scroll = *indicator ? xasprintf(" %s scroll \xc2\xb7", indicator) : xstrdup("");
        r = xasprintf("%s submit \xc2\xb7 %s scroll \xc2\xb7%s %s", enter, updown, scroll, cancel);
        free(scroll);
    } else {
        const aq_t *q = v->nq ? &v->q[cur_q(v)] : NULL;
        const char *enter_action = v->nq > 1 ? "next" : "submit";
        char *action;
        if (q && q->multi) {
            char *sp = ui_key_hint("space");
            action = xasprintf("%s toggle \xc2\xb7 %s %s", sp, enter, enter_action);
            free(sp);
        } else {
            char *n = ui_key_hint("n");
            action = xasprintf("%s select \xc2\xb7 %s note", enter, n);
            free(n);
        }
        char *tabs;
        if (has_submit_tab(v)) {
            char *t = ui_key_hint("tab"), *l = ui_key_hint("left"), *rt = ui_key_hint("right");
            tabs = xasprintf(" \xc2\xb7 %s/%s/%s question", t, l, rt);
            free(t);
            free(l);
            free(rt);
        } else tabs = xstrdup("");
        if (v->can_page && *indicator) {
            char *pg = key2("pageup", "pagedown");
            char *pu = ui_editor_key("tui.select.pageUp"), *pd = ui_editor_key("tui.select.pageDown");
            free(pg);
            pg = xasprintf("%s/%s", pu, pd);
            free(pu);
            free(pd);
            r = xasprintf("%s \xc2\xb7 %s%s \xc2\xb7 %s%s \xc2\xb7 %s %s", action, updown, tabs, cancel, exp, pg, indicator);
            free(pg);
        } else {
            char *scroll = *indicator ? xasprintf(" %s scroll \xc2\xb7", indicator) : xstrdup("");
            r = xasprintf("%s \xc2\xb7 %s move%s \xc2\xb7%s %s%s", action, updown, tabs, scroll, cancel, exp);
            free(scroll);
        }
        free(action);
        free(tabs);
    }
    free(hide);
    free(esc);
    free(cancel);
    free(enter);
    free(updown);
    free(exp);
    return r;
}

static const char *clip_indicator(int offset, int rows, int total) {
    bool above = offset > 0, below = offset + rows < total;
    if (above && below) return "\xe2\x86\x95";
    if (above) return "\xe2\x86\x91";
    if (below) return "\xe2\x86\x93";
    return "";
}

static int scroll_for_cursor(int cur, int cs, int ce, int rows, int total, bool manual) {
    int maxoff = MAX(0, total - rows);
    if (!maxoff) return 0;
    int next = MAX(0, MIN(cur, maxoff));
    int crow = ce - cs;
    if (manual && crow > rows) next = MAX(cs, MIN(next, ce - rows));
    else if (cs < next || ce > next + rows) next = crow <= rows ? ce - rows : cs;
    return MAX(0, MIN(next, maxoff));
}

static const char *question_body(ask_view_t *v, int width, int rows, strvec_t *out) {
    if (!v->nq) {
        for (int i = 0; i < rows; i++) strvec_push(out, "");
        return "";
    }
    aq_t *q = &v->q[cur_q(v)];
    ast_t *s = &v->st[cur_q(v)];
    int nrows = row_count(q);
    s->cursor = MAX(0, MIN(s->cursor, nrows - 1));
    int cw = width;
    strvec_t all = {0};
    int *start = xcalloc((size_t)nrows + 1, sizeof *start);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < all.n; i++) free(all.v[i]);
        all.n = 0;
        for (int r = 0; r < nrows; r++) {
            start[r] = (int)all.n;
            render_row(q, s, r, r == s->cursor, cw, v->expanded, &all);
        }
        start[nrows] = (int)all.n;
        if (pass == 0 && width > 1 && (int)all.n > rows) cw = width - 1;
        else break;
    }
    int cs = start[s->cursor], ce = start[s->cursor + 1];
    v->can_page = ce - cs > rows;
    s->scroll = scroll_for_cursor(s->scroll, cs, ce, rows, (int)all.n, s->manual_scroll);
    ui_scroll_view(&all, rows, &s->scroll, width, out);
    const char *ind = clip_indicator(s->scroll, rows, (int)all.n);
    strvec_free(&all);
    free(start);
    return ind;
}

static char *answer_summary(const aq_t *q, const ast_t *s) {
    sb_t b = SB_INIT;
    int n = 0;
    for (size_t i = 0; i < q->nopt; i++) {
        if (!label_selected(q, s, i)) continue;
        if (!q->multi && n) break;
        if (n++) sb_append(&b, ", ");
        sb_append(&b, q->display[i]);
    }
    if (q->multi) {
        if (s->custom) {
            char *c = inline_input(s->custom);
            sb_appendf(&b, "%sOther: \xe2\x80\x9c%s\xe2\x80\x9d", n++ ? ", " : "", c);
            free(c);
        }
    } else if (s->custom) {
        char *c = inline_input(s->custom);
        sb_clear(&b);
        sb_appendf(&b, "\xe2\x80\x9c%s\xe2\x80\x9d", c);
        free(c);
        n = 1;
    }
    if (!n) {
        sb_free(&b);
        return S(TUI_TC_WARNING, "unanswered");
    }
    return sb_detach(&b);
}

static size_t unanswered(const ask_view_t *v) {
    size_t n = 0;
    for (size_t i = 0; i < v->nq; i++)
        if (!is_answered(&v->q[i], &v->st[i])) n++;
    return n;
}

static const char *submit_body(ask_view_t *v, int width, int rows, strvec_t *out) {
    strvec_t all = {0};
    size_t un = unanswered(v);
    if (un) {
        char *e = ui_key_hint("enter");
        char *t = xasprintf("%zu unanswered question%s; %s still submits.", un, un == 1 ? "" : "s", e);
        strvec_push_own(&all, S(TUI_TC_WARNING, t));
        free(t);
        free(e);
        strvec_push(&all, "");
    }
    for (size_t i = 0; i < v->nq; i++) {
        char *label = tab_label(&v->q[i], i);
        char *head = xasprintf("%zu. %s:", i + 1, label), *hd = S(TUI_TC_DIM, head);
        char *ans = answer_summary(&v->q[i], &v->st[i]);
        strvec_push_own(&all, xasprintf("%s %s", hd, ans));
        free(label);
        free(head);
        free(hd);
        free(ans);
        const char *note = submitted_note(&v->q[i], &v->st[i], NULL);
        if (note && !blank(note)) {
            char *n = inline_input(note), *t = tui_truncate_to_width(n, MAX(1, width - 9), NULL);
            char *l = xasprintf("   Note: %s", t);
            strvec_push_own(&all, S(TUI_TC_MUTED, l));
            free(l);
            free(t);
            free(n);
        }
    }
    strvec_push(&all, "");
    char *sub = xasprintf("%s " SUBMIT_OPTION, tui_sym("nav.cursor"));
    strvec_push_own(&all, S(TUI_TC_ACCENT, sub));
    free(sub);
    v->submit_scroll = MAX(0, MIN(v->submit_scroll, (int)all.n - rows));
    ui_scroll_view(&all, rows, &v->submit_scroll, width, out);
    const char *ind = clip_indicator(v->submit_scroll, rows, (int)all.n);
    strvec_free(&all);
    return ind;
}

static void render_hidden(ask_view_t *v, int width, strvec_t *out) {
    char *hide = ui_editor_key("app.ask.hide"), *enter = ui_key_hint("enter"), *esc = ui_editor_key("tui.select.cancel");
    bool blocked = guard_blocks(v);
    char *show = blocked ? xstrdup(hide) : xasprintf("%s/%s", hide, enter);
    sb_t h = SB_INIT;
    if (blocked) {
        char *g = guard_hint();
        sb_appendf(&h, "%s \xc2\xb7 ", g);
        free(g);
    }
    sb_appendf(&h, "%s show \xc2\xb7 %s cancel", show, esc);
    char *ht = xasprintf(" \xc2\xb7 %s", sb_cstr(&h));
    char *hint = S(TUI_TC_DIM, ht);
    char *tt = title_text(v), *lt = xasprintf("%s hidden:", tt), *label = S(TUI_TC_ACCENT, lt);
    char *more = NULL;
    if (v->nq > 1) {
        char *m = xasprintf(" (+%zu more)", v->nq - 1);
        more = S(TUI_TC_DIM, m);
        free(m);
    } else more = xstrdup("");
    char *qs = v->nq ? sanitize_cr(v->q[cur_q(v)].question) : xstrdup("");
    char *summary = inline_input(qs);
    int sw = MAX(1, width - tui_visible_width(label) - tui_visible_width(more) - tui_visible_width(hint) - 1);
    char *clipped = tui_truncate_to_width(summary, sw, NULL), *cm = S(TUI_TC_MUTED, clipped);
    char *line = xasprintf("%s %s%s%s", label, cm, more, hint);
    strvec_push_own(out, tui_truncate_to_width(line, width, NULL));
    free(line);
    free(cm);
    free(clipped);
    free(summary);
    free(qs);
    free(more);
    free(label);
    free(lt);
    free(tt);
    free(hint);
    free(ht);
    sb_free(&h);
    free(show);
    free(hide);
    free(enter);
    free(esc);
}

/* ---------- prompt (hook-editor.ts, prompt style) ---------- */

static void render_prompt(ask_view_t *v, int width, strvec_t *out) {
    strvec_push_own(out, ui_box_top(width, v->ptitle));
    int inner = MAX(1, width - 4);
    strvec_t body = {0};
    if (v->pdetail.n) {
        strvec_push(&body, "");
        for (size_t i = 0; i < v->pdetail.n; i++) strvec_push_own(&body, S(TUI_TC_ACCENT, v->pdetail.v[i]));
    }
    strvec_push(&body, "");
    v->ped->base.focused = v->base.focused;
    tui_component_render(&v->ped->base, inner, &body);
    strvec_push(&body, "");
    char *e = ui_key_hint("enter"), *q = ui_key_hint("ctrl+q"), *esc = ui_key_hint("escape"), *g = ui_key_hint("ctrl+g");
    char *hint = xasprintf("%s or %s submit  %s cancel  %s external editor", e, q, esc, g);
    strvec_push_own(&body, S(TUI_TC_DIM, hint)); /* formTheme.hint */
    strvec_push(&body, "");
    for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
    strvec_push_own(out, ui_box_bottom(width));
    free(hint);
    free(e);
    free(q);
    free(esc);
    free(g);
    strvec_free(&body);
}

static void open_prompt(ask_view_t *v, int kind, size_t qi, int row, const char *title, const char *prefill) {
    v->pkind = kind;
    v->pq = qi;
    v->prow = row;
    /* boundPromptTitle(`${title}: `, question): flattened, wrapped to the terminal width less 4, ≤ 3 rows;
     * the first row insets into the border, the rest stay as accent body rows */
    int cols = v->u && v->u->tui ? tui_term(v->u->tui)->cols : 80;
    int w = MAX(1, (cols > 0 ? cols : 80) - 4);
    char *raw = xasprintf("%s: %s", title, v->q[qi].question), *flat = inline_input(raw);
    strvec_t wrapped = {0};
    tui_wrap_text_with_ansi(flat, w, &wrapped);
    if (wrapped.n > MAX_PROMPT_TITLE_ROWS) {
        sb_t rest = SB_INIT;
        for (size_t i = MAX_PROMPT_TITLE_ROWS - 1; i < wrapped.n; i++) {
            if (rest.len) sb_putc(&rest, ' ');
            sb_append(&rest, wrapped.v[i]);
        }
        while (wrapped.n > MAX_PROMPT_TITLE_ROWS - 1) free(wrapped.v[--wrapped.n]);
        strvec_push_own(&wrapped, tui_truncate_to_width(sb_cstr(&rest), w, NULL));
        sb_free(&rest);
    }
    free(v->ptitle);
    v->ptitle = xstrdup(wrapped.n ? wrapped.v[0] : "");
    strvec_free(&v->pdetail);
    memset(&v->pdetail, 0, sizeof v->pdetail);
    for (size_t i = 1; i < wrapped.n; i++) strvec_push(&v->pdetail, wrapped.v[i]);
    strvec_free(&wrapped);
    free(raw);
    free(flat);
    v->ped = tui_editor_new();
    tui_editor_set_composer(v->ped, "borderless");
    free(v->ped->gutter);
    v->ped->gutter = xstrdup("> ");
    v->ped->padding_x = 0;
    v->ped->disable_submit = true;
    v->ped->max_height = MAX(3, term_rows(v) - 12);
    /* getEditorTheme(): typed text in the theme's text color; the hardware cursor like the composer */
    snprintf(v->ped->text_fg, sizeof v->ped->text_fg, "%s", v->u->editor->text_fg);
    v->ped->use_terminal_cursor = v->u->editor->use_terminal_cursor;
    if (prefill) tui_editor_set_text(v->ped, prefill);
    v->ped->base.tui = v->base.tui;
    request(v);
}

static void advance(ask_view_t *v, int answered_row);
static void finish_submit(ask_view_t *v);

static void close_prompt(ask_view_t *v, const char *text) {
    int kind = v->pkind;
    size_t qi = v->pq;
    int row = v->prow;
    tui_component_destroy(&v->ped->base);
    v->ped = NULL;
    v->pkind = 0;
    if (!text || v->closed) {
        request(v);
        return;
    }
    aq_t *q = &v->q[qi];
    ast_t *s = &v->st[qi];
    if (kind == 1) {
        if (blank(text)) {
            free(s->custom);
            s->custom = NULL;
            if (s->note_row == row) clear_note(s);
        } else {
            free(s->custom);
            s->custom = xstrdup(text);
            if (!q->multi) {
                memset(s->sel, 0, q->nopt * sizeof *s->sel);
                if (s->note_row >= 0 && s->note_row != row) clear_note(s);
            }
            if (q->multi && v->nq == 1) {
                v->active_tab = submit_tab(v);
                v->submit_scroll = 0;
            } else advance(v, row);
        }
    } else {
        free(s->note);
        s->note = xstrdup(text);
        s->note_row = row;
        v->just_q = -1; /* the note went to it: a later question's note goes to that question's row */
    }
    request(v);
}

/* ---------- results ---------- */

/* the tool's answer object: id -> label (single), [labels…] (multi); a custom answer replaces the label
 * (single) or joins the list (multi); unanswered questions are left out */
static json_t *build_answers(const ask_view_t *v) {
    json_t *ans = json_obj();
    for (size_t i = 0; i < v->nq; i++) {
        const aq_t *q = &v->q[i];
        const ast_t *s = &v->st[i];
        if (q->multi) {
            json_t *a = json_arr();
            for (size_t k = 0; k < q->nopt; k++)
                if (label_selected(q, s, k)) {
                    bool dup = false;
                    for (size_t j = 0; j < k; j++)
                        if (!strcmp(q->opts[j].label, q->opts[k].label)) dup = true;
                    if (!dup) json_push_str(a, q->opts[k].label);
                }
            if (s->custom) json_push_str(a, s->custom);
            json_set(ans, q->id, a);
        } else if (s->custom) {
            json_set_str(ans, q->id, s->custom);
        } else {
            for (size_t k = 0; k < q->nopt; k++)
                if (s->sel[k]) {
                    json_set_str(ans, q->id, q->opts[k].label);
                    break;
                }
        }
    }
    /* the rich per-question results the ask tool formats (omp AskDialogResult): selected labels, custom
     * input, the submitted note and the option it sits on, timeout auto-selection */
    json_t *results = json_arr();
    for (size_t i = 0; i < v->nq; i++) {
        const aq_t *q = &v->q[i];
        const ast_t *s = &v->st[i];
        json_t *r = json_obj(), *sel = json_arr();
        json_set_str(r, "id", q->id);
        if (q->multi || !s->custom)
            for (size_t k = 0; k < q->nopt; k++)
                if (q->multi ? label_selected(q, s, k) : s->sel[k]) {
                    bool dup = false;
                    json_t *x;
                    json_arr_foreach(sel, j, x) dup |= str_eq(json_sv(x), q->opts[k].label);
                    if (!dup) json_push_str(sel, q->opts[k].label);
                    if (!q->multi) break;
                }
        json_set(r, "selectedOptions", sel);
        if (s->custom) json_set_str(r, "customInput", s->custom);
        int nopt = -1;
        const char *note = submitted_note(q, s, &nopt);
        if (note) {
            json_set_str(r, "note", note);
            if (nopt >= 0) json_set_str(r, "noteOption", q->opts[nopt].label);
        }
        if (s->timed_out) json_set_bool(r, "timedOut", true);
        json_push(results, r);
    }
    json_set(ans, "__results", results);
    return ans;
}

static void stop_timer(ask_view_t *v) {
    if (v->timer_id > 0 && v->u && v->u->tui) tui_remove_timer(v->u->tui, v->timer_id);
    v->timer_id = 0;
}

static void finish(ask_view_t *v, json_t *answers) {
    if (v->closed) {
        json_free(answers);
        return;
    }
    v->closed = true;
    stop_timer(v);
    ui_ask_done_fn done = v->done;
    void *ud = v->ud;
    ui_t *u = v->u;
    ui_close_nested_selector(u); /* a Ctrl+R history search open over the dialog */
    ui_close_selector(u);        /* destroys v */
    if (done) done(ud, answers);
}

static void finish_submit(ask_view_t *v) { finish(v, build_answers(v)); }

static void advance(ask_view_t *v, int answered_row) {
    size_t cur = cur_q(v);
    if (answered_row >= 0 && v->nq > 1) {
        v->just_q = (int)cur;
        v->just_row = answered_row;
    } else v->just_q = -1;
    if (v->nq == 1) {
        finish_submit(v);
        return;
    }
    v->active_tab = cur + 1 < v->nq ? cur + 1 : submit_tab(v);
    v->submit_scroll = 0;
    request(v);
}

static void handle_timeout(ask_view_t *v) {
    if (v->closed) return;
    for (size_t i = 0; i < v->nq; i++) {
        aq_t *q = &v->q[i];
        ast_t *s = &v->st[i];
        if (is_answered(q, s)) continue;
        int fb = s->note_row >= 0 && s->note_row < (int)q->nopt
                     ? s->note_row
                     : MAX(0, MIN(q->recommended < 0 ? 0 : q->recommended, (int)q->nopt - 1));
        if (fb < (int)q->nopt) s->sel[fb] = true;
        s->timed_out = true;
    }
    finish_submit(v);
}

static void countdown_tick(void *ud) {
    ask_view_t *v = ud;
    int64_t left = v->deadline - mono_ms();
    v->remaining = (int)MAX(0, (left + 999) / 1000);
    if (left <= 0) {
        if (v->pkind) return; /* deferred until the prompt closes */
        handle_timeout(v);
        return;
    }
    request(v);
}

static void countdown_reset(ask_view_t *v) {
    if (v->timeout_ms <= 0) return;
    v->deadline = mono_ms() + v->timeout_ms;
    v->remaining = (int)((v->timeout_ms + 999) / 1000);
}

/* ---------- input ---------- */

static void commit_row(ask_view_t *v, aq_t *q, ast_t *s, int row, bool enter) {
    if (row >= (int)q->nopt) {
        char *title = xstrdup("Custom answer");
        open_prompt(v, 1, cur_q(v), row, title, s->custom);
        free(title);
        return;
    }
    if (q->multi) {
        if (enter) {
            advance(v, s->sel[row] ? row : -1);
            return;
        }
        if (label_selected(q, s, (size_t)row)) {
            for (size_t k = 0; k < q->nopt; k++)
                if (!strcmp(q->opts[k].label, q->opts[row].label)) s->sel[k] = false;
            if (s->note_row == row) clear_note(s);
        } else s->sel[row] = true;
        request(v);
        return;
    }
    memset(s->sel, 0, q->nopt * sizeof *s->sel);
    s->sel[row] = true;
    free(s->custom);
    s->custom = NULL;
    if (s->note_row >= 0 && s->note_row != row) clear_note(s);
    advance(v, row);
}

static void press_note(ask_view_t *v) {
    if (on_submit_tab(v) || !v->nq) return;
    size_t qi = cur_q(v);
    int row = v->st[qi].cursor;
    if (v->just_q >= 0 && (size_t)v->just_q != qi && !is_answered(&v->q[qi], &v->st[qi])) {
        qi = (size_t)v->just_q;
        row = v->just_row;
    }
    ast_t *s = &v->st[qi];
    char *title = xasprintf("Note for %s", row_label(&v->q[qi], row));
    open_prompt(v, 2, qi, row, title, s->note_row == row ? s->note : NULL);
    free(title);
}

static bool is_enter(const tui_key_t *k) {
    return tui_key_matches(k, "enter") || tui_key_matches(k, "return") ||
           (k->kind == TUI_KEY_CHAR && k->codepoint == '\n' && !k->mods);
}

static bool prompt_key(ask_view_t *v, const tui_key_t *k) {
    if (tui_key_matches(k, "ctrl+q") || tui_key_matches(k, "ctrl+enter") || tui_key_matches(k, "enter") ||
        tui_key_matches(k, "return")) {
        char *t = tui_editor_get_expanded_text(v->ped);
        close_prompt(v, t);
        free(t);
        return true;
    }
    if (tui_key_matches(k, "escape")) {
        close_prompt(v, NULL);
        return true;
    }
    if (tui_kb(k, "app.editor.external")) { /* HookEditorComponent #openExternalEditor */
        char *cur = tui_editor_get_expanded_text(v->ped);
        char *r = ui_dialog_external_edit(v->u, cur);
        if (r && v->pkind) tui_editor_set_text(v->ped, r);
        free(r);
        free(cur);
        request(v);
        return true;
    }
    tui_editor_handle_key(v->ped, k);
    request(v);
    return true;
}

static bool ask_key(tui_component_t *c, const tui_key_t *k) {
    ask_view_t *v = (ask_view_t *)c;
    if (v->closed) return true;
    if (v->pkind) return prompt_key(v, k);
    countdown_reset(v);
    if (!(k->kind == TUI_KEY_CHAR && (k->codepoint == 'n' || k->codepoint == 'N') && !(k->mods & ~TUI_MOD_SHIFT)))
        v->just_q = -1;
    if (tui_kb(k, "tui.select.cancel")) {
        finish(v, NULL);
        return true;
    }
    if (tui_kb(k, "app.ask.hide")) {
        v->hidden = !v->hidden;
        request(v);
        return true;
    }
    if (tui_kb(k, "app.tools.expand")) {
        if (!v->hidden && !on_submit_tab(v) && v->nq &&
            (v->expanded || v->header_exp || v->desc_exp)) {
            v->expanded = !v->expanded;
            v->stable_total = 0;
            request(v);
        }
        return true;
    }
    if (guard_blocks(v)) {
        if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) {
            v->guard_released = true;
            v->hidden = false;
        } else tui_editor_handle_key(v->u->editor, k);
        request(v);
        return true;
    }
    if (v->hidden) {
        if (is_enter(k)) {
            v->hidden = false;
            request(v);
        }
        return true;
    }
    if (has_submit_tab(v)) {
        int dir = tui_key_matches(k, "tab") || tui_key_matches(k, "right") ? 1
                  : tui_key_matches(k, "shift+tab") || tui_key_matches(k, "left") ? -1 : 0;
        if (dir) {
            size_t n = v->nq + 1;
            v->active_tab = (v->active_tab + n + (size_t)(dir < 0 ? n - 1 : 1)) % n;
            v->submit_scroll = 0;
            request(v);
            return true;
        }
    }
    if (on_submit_tab(v)) {
        if (tui_kb(k, "tui.select.up")) v->submit_scroll = MAX(0, v->submit_scroll - 1);
        else if (tui_kb(k, "tui.select.down")) v->submit_scroll++;
        else if (is_enter(k)) {
            finish_submit(v);
            return true;
        }
        request(v);
        return true;
    }
    if (!v->nq) return true;
    aq_t *q = &v->q[cur_q(v)];
    ast_t *s = &v->st[cur_q(v)];
    int nrows = row_count(q);
    if (tui_kb(k, "tui.select.pageUp")) {
        s->scroll = MAX(0, s->scroll - MAX(1, v->body_rows - 1));
        s->manual_scroll = true;
    } else if (tui_kb(k, "tui.select.pageDown")) {
        s->scroll += MAX(1, v->body_rows - 1);
        s->manual_scroll = true;
    } else if (tui_kb(k, "tui.select.up")) {
        s->cursor = MAX(0, MIN(s->cursor - 1, nrows - 1));
        s->manual_scroll = false;
    } else if (tui_kb(k, "tui.select.down")) {
        s->cursor = MAX(0, MIN(s->cursor + 1, nrows - 1));
        s->manual_scroll = false;
    } else if (k->kind == TUI_KEY_CHAR && (k->codepoint == 'n' || k->codepoint == 'N') && !(k->mods & ~TUI_MOD_SHIFT)) {
        press_note(v);
        return true;
    } else {
        bool enter = is_enter(k), space = tui_key_matches(k, "space");
        if (!enter && !(q->multi && space)) return true;
        if (enter && s->cursor >= (int)q->nopt && has_submit_tab(v)) {
            advance(v, s->custom ? s->cursor : -1);
            return true;
        }
        commit_row(v, q, s, s->cursor, enter);
        return true;
    }
    request(v);
    return true;
}

/* ---------- render ---------- */

static void ask_render(tui_component_t *c, int width, strvec_t *out) {
    ask_view_t *v = (ask_view_t *)c;
    if (v->pkind) {
        render_prompt(v, width, out);
        return;
    }
    if (v->hidden) {
        render_hidden(v, width, out);
        return;
    }
    int inner = MAX(1, width - 4);
    v->content_width = inner;
    int total = dialog_height(v, inner, term_rows(v));
    int tabrows = has_submit_tab(v) ? 1 : 0;
    int max_title = MAX(1, total - 5 - MIN_BODY_ROWS - tabrows);
    strvec_t header = {0}, body = {0};
    render_header(v, inner, max_title, &header);
    int fixed = 1 + (int)header.n + 1 + 1 + 1 + 1;
    int rows = MAX(MIN_BODY_ROWS, total - fixed);
    v->body_rows = rows;
    const char *ind = on_submit_tab(v) ? submit_body(v, inner, rows, &body) : question_body(v, inner, rows, &body);
    char *foot = footer_text(v, ind), *fs = S(TUI_TC_DIM, foot);
    char *title = title_text(v);
    strvec_push_own(out, ui_box_top(width, title));
    for (size_t i = 0; i < header.n; i++) strvec_push_own(out, ui_box_row(header.v[i], width));
    strvec_push_own(out, ui_box_divider(width));
    for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
    strvec_push_own(out, ui_box_divider(width));
    strvec_push_own(out, ui_box_row(fs, width));
    strvec_push_own(out, ui_box_bottom(width));
    if (v->guard) { /* the parked draft stays beneath the dialog; its cursor shows while it owns typing */
        tui_component_t *ed = &v->u->editor->base;
        ed->focused = guard_blocks(v);
        ed->vt->render(ed, width, out);
    }
    free(title);
    free(foot);
    free(fs);
    strvec_free(&header);
    strvec_free(&body);
}

static void ask_destroy(tui_component_t *c) {
    ask_view_t *v = (ask_view_t *)c;
    stop_timer(v);
    if (v->ped) tui_component_destroy(&v->ped->base);
    for (size_t i = 0; i < v->nq; i++) {
        aq_t *q = &v->q[i];
        for (size_t k = 0; k < q->nopt; k++) {
            free(q->opts[k].label);
            free(q->opts[k].description);
            free(q->opts[k].preview);
            free(q->display[k]);
        }
        free(q->opts);
        free(q->display);
        free(q->id);
        free(q->question);
        free(q->header);
        free(v->st[i].sel);
        free(v->st[i].custom);
        free(v->st[i].note);
    }
    free(v->q);
    free(v->st);
    free(v->ptitle);
    strvec_free(&v->pdetail);
    if (!v->closed && v->done) v->done(v->ud, NULL); /* torn down from outside (e.g. /new): cancelled */
    free(v);
}

static const tui_component_vt ASK_VT = {"ask-dialog", ask_render, ask_key, NULL, ask_destroy};

bool ui_ask_is_dialog(const tui_component_t *c) { return c && c->vt == &ASK_VT; }

tui_editor_t *ui_ask_prompt_editor(tui_component_t *c) { return ui_ask_is_dialog(c) ? ((ask_view_t *)c)->ped : NULL; }

/* normalizeDialogQuestions */
static void load_questions(ask_view_t *v, const json_t *questions) {
    size_t n = json_is_arr(questions) ? json_len(questions) : 0;
    v->q = xcalloc(n + 1, sizeof *v->q);
    v->st = xcalloc(n + 1, sizeof *v->st);
    for (size_t i = 0; i < n; i++) {
        json_t *e = json_at(questions, i);
        if (!json_is_obj(e)) continue;
        aq_t *q = &v->q[v->nq];
        const char *id = json_get_str(e, "id"), *qs = json_get_str(e, "question"), *hd = json_get_str(e, "header");
        q->id = xstrdup(id ? id : "?");
        q->question = xstrdup(qs ? qs : "");
        q->header = hd ? sanitize_cr(hd) : NULL;
        json_t *jm = json_get(e, "multi");
        q->multi = json_is_bool(jm) && jm->u.b;
        json_t *jr = json_get(e, "recommended");
        q->recommended = -1;
        if (json_is_num(jr) && jr->u.num.d == floor(jr->u.num.d) && fabs(jr->u.num.d) < 1e9) q->recommended = (int)jr->u.num.d;
        json_t *opts = json_get_arr(e, "options");
        size_t no = opts ? json_len(opts) : 0;
        q->opts = xcalloc(no + 1, sizeof *q->opts);
        json_t *o;
        json_arr_foreach(opts, k, o) {
            if (!json_is_obj(o)) continue;
            aopt_t *a = &q->opts[q->nopt++];
            const char *l = json_get_str(o, "label"), *d = json_get_str(o, "description"), *p = json_get_str(o, "preview");
            a->label = xstrdup(l ? l : "");
            if (d && !blank(d)) {
                char *t = str_trim(d);
                a->description = sanitize_cr(t);
                free(t);
            }
            if (p && !blank(p)) a->preview = sanitize_cr(p);
        }
        build_display(q);
        ast_t *s = &v->st[v->nq];
        s->sel = xcalloc(q->nopt + 1, sizeof *s->sel);
        s->note_row = -1;
        s->cursor = MAX(0, MIN(q->recommended < 0 ? 0 : q->recommended, (int)q->nopt - 1));
        v->nq++;
    }
}

void ui_ask_dialog_show(ui_t *u, const json_t *questions, ui_ask_done_fn done, void *ud) {
    ask_view_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &ASK_VT);
    v->u = u;
    v->done = done;
    v->ud = ud;
    v->just_q = -1;
    v->remaining = -1;
    load_questions(v, questions);
    double secs = settings_num("ask.timeout", 0);
    if (secs > 0) {
        v->timeout_ms = (int64_t)(secs * 1000);
        countdown_reset(v);
        v->timer_id = tui_add_timer(u->tui, 1000, true, countdown_tick, v);
    }
    v->guard = !tui_editor_is_empty(u->editor);
    ui_show_selector(u, &v->base, &v->base);
}
