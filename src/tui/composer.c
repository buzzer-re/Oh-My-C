/* composer.c - built-in composer chrome styles (port of omp components/composer/). */
#include "tui/composer.h"

#include "tui/ansi.h"
#include "tui/symbols.h"

const char *const TUI_COMPOSER_SHAPES[8] = {"box", "band", "claude", "pi", "borderless", "rule", "field", "rail"};

static char *col(tui_composer_color_fn fn, const tui_composer_ctx_t *c, const char *s) {
    return fn ? fn(c->ud, s) : xstrdup(s);
}

static char *rep(const char *g, int n) {
    sb_t b = SB_INIT;
    for (int i = 0; i < n; i++) sb_append(&b, g);
    return sb_detach(&b);
}

static char *spaces(int n) {
    sb_t b = SB_INIT;
    tui_sb_spaces(&b, n);
    return sb_detach(&b);
}

/* push a + b + c (owned parts freed) */
static void push3(strvec_t *out, char *a, const char *b, char *c) {
    strvec_push_own(out, xasprintf("%s%s%s", a ? a : "", b ? b : "", c ? c : ""));
    free(a);
    free(c);
}

#define H tui_sym("boxRound.horizontal")
#define V tui_sym("boxRound.vertical")

/* ---------- box ---------- */

static int box_pad(int theme_px) { return MAX(0, theme_px < 0 ? 2 : theme_px); }
static int side_px1(int px) { return px + 1; }
static int side_px(int px) { return px; }
static int side_0(int px) { return 0; }
static int pad_0(int t) { return 0; }
static int pad_1(int t) { return 1; }

static char *box_top(const tui_composer_ctx_t *c) {
    char *h = rep(H, c->padding_x);
    char *tl_raw = xasprintf("%s%s", tui_sym("boxRound.topLeft"), h);
    char *tr_raw = xasprintf("%s%s", h, tui_sym("boxRound.topRight"));
    char *tl = col(c->border, c, tl_raw), *tr = col(c->border, c, tr_raw);
    free(h), free(tl_raw), free(tr_raw);
    int fillw = MAX(0, c->width - side_px1(c->padding_x) * 2);
    char *mid;
    if (!c->top_content) {
        mid = rep(H, fillw);
        char *m = col(c->border, c, mid);
        free(mid);
        mid = m;
    } else {
        char *content;
        int cw;
        if (c->top_width <= fillw) {
            content = xstrdup(c->top_content);
            cw = c->top_width;
        } else {
            content = tui_truncate_to_width(c->top_content, MAX(0, fillw - 1), NULL);
            cw = tui_visible_width(content);
        }
        char *f = rep(H, MAX(0, fillw - cw));
        char *fc = col(c->border, c, f);
        mid = xasprintf("%s%s", content, fc);
        free(f), free(fc), free(content);
    }
    char *r = xasprintf("%s%s%s", tl, mid, tr);
    free(tl), free(tr), free(mid);
    return r;
}

static void box_row(const tui_composer_row_t *r, strvec_t *out) {
    const tui_composer_ctx_t *c = r->ctx;
    int right_cells = MAX(1, c->padding_x + 1 - r->cursor_overflow);
    if (r->is_last_row && r->ime_safe_tail) {
        char *sp = spaces(c->padding_x), *raw = xasprintf("%s%s", V, sp);
        char *lb = col(c->border, c, raw);
        free(sp), free(raw);
        push3(out, lb, r->text, NULL);
        char *h = rep(H, MAX(0, c->width - 2));
        raw = xasprintf("%s%s%s", tui_sym("boxRound.bottomLeft"), h, tui_sym("boxRound.bottomRight"));
        strvec_push_own(out, col(c->border, c, raw));
        free(h), free(raw);
        return;
    }
    char *pad = spaces(r->pad);
    if (r->is_last_row) {
        char *sp = spaces(MAX(0, c->padding_x - 1));
        char *raw = xasprintf("%s%s%s", tui_sym("boxRound.bottomLeft"), H, sp);
        char *bl = col(c->border, c, raw);
        free(sp), free(raw);
        int rp = MAX(0, right_cells - 2);
        sp = spaces(rp);
        raw = xasprintf("%s%s%s", sp, right_cells >= 2 ? H : "", tui_sym("boxRound.bottomRight"));
        char *br = col(c->border, c, raw);
        free(sp), free(raw);
        strvec_push_own(out, xasprintf("%s%s%s%s", bl, r->text, pad, br));
        free(bl), free(br), free(pad);
        return;
    }
    char *sp = spaces(c->padding_x), *raw = xasprintf("%s%s", V, sp);
    char *lb = col(c->border, c, raw);
    free(sp), free(raw);
    sp = spaces(MAX(0, right_cells - 1));
    raw = xasprintf("%s%s", sp, r->scrollbar_thumb ? "\xe2\x96\x88" : V);
    char *rb = col(c->border, c, raw);
    free(sp), free(raw);
    strvec_push_own(out, xasprintf("%s%s%s%s", lb, r->text, pad, rb));
    free(lb), free(rb), free(pad);
}

static char *none_chrome(const tui_composer_ctx_t *c) { return NULL; }

/* ---------- band ---------- */

static char *band_top(const tui_composer_ctx_t *c) {
    /* the band always owns its chrome row; empty until the status line has content */
    if (!c->top_content || !*c->top_content) return xstrdup("");
    return c->top_width > c->width ? tui_truncate_to_width(c->top_content, c->width, NULL) : xstrdup(c->top_content);
}

static void band_row(const tui_composer_row_t *r, strvec_t *out) {
    char *g = r->gutter && *r->gutter ? col(r->ctx->border, r->ctx, r->gutter) : NULL;
    char *pad = spaces(r->pad);
    push3(out, g, r->text, pad);
}

/* ---------- pi / borderless / claude / rule ---------- */

static char *full_rule(const tui_composer_ctx_t *c) {
    char *h = rep(H, c->width);
    char *r = col(c->border, c, h);
    free(h);
    return r;
}

static void pi_row(const tui_composer_row_t *r, strvec_t *out) {
    char *sp = spaces(r->ctx->padding_x), *pad = spaces(r->pad);
    strvec_push_own(out, xasprintf("%s%s%s%s", sp, r->gutter ? r->gutter : "", r->text, pad));
    free(sp), free(pad);
}

static void plain_row(const tui_composer_row_t *r, strvec_t *out) {
    char *pad = spaces(r->pad);
    strvec_push_own(out, xasprintf("%s%s%s", r->gutter ? r->gutter : "", r->text, pad));
    free(pad);
}

static char *top_rule(const tui_composer_ctx_t *c) {
    if (c->top_content && c->top_width > 0 && c->width > 2) {
        char *content = xstrdup(c->top_content);
        int cw = c->top_width;
        if (cw > c->width - 2) {
            free(content);
            content = tui_truncate_to_width(c->top_content, c->width - 2, NULL);
            cw = tui_visible_width(content);
        }
        char *lf = rep(H, MAX(0, c->width - cw - 1));
        char *l = col(c->border, c, lf), *rr = col(c->border, c, H);
        char *res = xasprintf("%s%s%s", l, content, rr);
        free(lf), free(l), free(rr), free(content);
        return res;
    }
    return full_rule(c);
}

/* ---------- field / rail ---------- */

static void field_row(const tui_composer_row_t *r, strvec_t *out) {
    const tui_composer_ctx_t *c = r->ctx;
    char *left = col(c->accent, c, "\xe2\x96\x90"); /* ▐ */
    char *sp = spaces(c->padding_x);
    char *lf = xasprintf("%s%s%s", sp, r->gutter ? r->gutter : "", r->text);
    free(sp);
    if (r->ime_safe_tail) {
        char *s = col(c->surface, c, lf);
        push3(out, left, s, NULL);
        free(s), free(lf);
        return;
    }
    int rc = MAX(1, c->padding_x + 1 - r->cursor_overflow);
    char *pad = spaces(r->pad), *rp = spaces(rc - 1);
    char *interior = xasprintf("%s%s%s", lf, pad, rp);
    char *s = col(c->surface, c, interior);
    char *rg = col(c->accent, c, r->scrollbar_thumb ? "\xe2\x96\x88" : "\xe2\x96\x8c"); /* ▌ */
    strvec_push_own(out, xasprintf("%s%s%s", left, s, rg));
    free(left), free(lf), free(pad), free(rp), free(interior), free(s), free(rg);
}

static void rail_row(const tui_composer_row_t *r, strvec_t *out) {
    const tui_composer_ctx_t *c = r->ctx;
    char *rail = col(c->accent, c, "\xe2\x96\x8e"); /* ▎ */
    char *sp = spaces(c->padding_x);
    char *lf = xasprintf("%s%s%s", sp, r->gutter ? r->gutter : "", r->text);
    free(sp);
    if (r->ime_safe_tail) {
        char *s = col(c->surface, c, lf);
        push3(out, rail, s, NULL);
        free(s), free(lf);
        return;
    }
    int rf = MAX(0, c->padding_x + 1 - r->cursor_overflow);
    char *pad = spaces(r->pad);
    if (r->scrollbar_thumb && rf > 0) {
        char *rp = spaces(rf - 1);
        char *interior = xasprintf("%s%s%s", lf, pad, rp);
        char *s = col(c->surface, c, interior), *th = col(c->accent, c, "\xe2\x96\x88");
        strvec_push_own(out, xasprintf("%s%s%s", rail, s, th));
        free(rp), free(interior), free(s), free(th);
    } else {
        char *rp = spaces(rf);
        char *interior = xasprintf("%s%s%s", lf, pad, rp);
        char *s = col(c->surface, c, interior);
        strvec_push_own(out, xasprintf("%s%s", rail, s));
        free(rp), free(interior), free(s);
    }
    free(rail), free(lf), free(pad);
}

static const tui_composer_style_t STYLES[8] = {
    {"box", false, true, 2, TUI_ATTACH_TOP_BORDER, TUI_BOTTOM_NONE, false, NULL, box_pad, side_px1, box_top, box_row, none_chrome},
    {"band", false, false, 1, TUI_ATTACH_TOP_BAND, TUI_BOTTOM_NONE, false, "\xe2\x95\xb0\xe2\x94\x80 ", pad_0, side_0, band_top,
     band_row, none_chrome},
    {"claude", false, false, 2, TUI_ATTACH_TOP_RULE_CHIP, TUI_BOTTOM_LEFT, false, "\xe2\x9d\xaf ", pad_0, side_px, top_rule,
     plain_row, full_rule},
    {"pi", false, false, 2, TUI_ATTACH_NONE, TUI_BOTTOM_FULL, false, NULL, pad_1, side_px, full_rule, pi_row, full_rule},
    {"borderless", false, false, 0, TUI_ATTACH_NONE, TUI_BOTTOM_FULL, false, "\xe2\x9d\xaf ", pad_0, side_0, none_chrome,
     plain_row, none_chrome},
    {"rule", false, false, 1, TUI_ATTACH_TOP_RULE_CHIP, TUI_BOTTOM_LEFT, true, "\xe2\x9d\xaf ", pad_0, side_px, top_rule,
     plain_row, none_chrome},
    {"field", true, true, 0, TUI_ATTACH_NONE, TUI_BOTTOM_FULL, true, NULL, pad_1, side_px1, none_chrome, field_row, none_chrome},
    {"rail", true, true, 0, TUI_ATTACH_NONE, TUI_BOTTOM_FULL, true, NULL, pad_1, side_px1, none_chrome, rail_row, none_chrome},
};

const tui_composer_style_t *tui_composer_style(const char *id) {
    for (size_t i = 0; i < ARRAY_LEN(STYLES); i++)
        if (str_eq(id, STYLES[i].id)) return &STYLES[i];
    return &STYLES[0];
}

bool tui_composer_is_builtin(const char *id) {
    for (size_t i = 0; i < ARRAY_LEN(STYLES); i++)
        if (str_eq(id, STYLES[i].id)) return true;
    return false;
}
