/* widgets.c - small presentation helpers shared by the setup wizard and overlays (ports of omp
 * key-hint-format.ts, chrome/overlay-box.ts, prompt/welcome.ts gradient, components/wizard-step.ts). */
#include "ui/overlays.h"
#include "ui/ui.h"

#include <math.h>
#include <ctype.h>

/* ---------- key hints (ui_key_hint lives in statusline.c) ---------- */

char *ui_editor_key(const char *action) {
    const strvec_t *keys = tui_keybindings_get(tui_keybindings_global(), action);
    if (!keys || !keys->n) return xstrdup("");
    return ui_key_hint(keys->v[0]);
}

/* ---------- lines ---------- */

char *ui_center_line(const char *line, int width) {
    int w = tui_visible_width(line);
    if (w >= width) return tui_truncate_to_width(line, width, NULL);
    int left = (width - w) / 2;
    return xasprintf("%*s%s%*s", left, "", line, width - left - w, "");
}

char *ui_fit_line(const char *line, int width) {
    if (width <= 0) return xstrdup("");
    if (tui_visible_width(line) > width) return tui_truncate_to_width(line, width, NULL);
    return tui_pad_to_width(line, width);
}

void ui_push_text(strvec_t *out, const char *text, int width) {
    if (!text) return;
    bool blank = true;
    for (const char *p = text; *p; p++)
        if (!isspace((unsigned char)*p)) blank = false;
    if (blank) return;
    char *t = tui_replace_tabs(text);
    tui_wrap_text_with_ansi(t, MAX(1, width), out);
    free(t);
}

/* ---------- box chrome ---------- */

static int g_box_tone = -1; /* overlay-box `color` (border + title), -1 = border / accent */

int ui_box_tone(int tc) {
    int prev = g_box_tone;
    g_box_tone = tc;
    return prev;
}

static void paint(sb_t *b, const char *s) { tui_fg(b, NULL, g_box_tone >= 0 ? (tui_tc_t)g_box_tone : TUI_TC_BORDER, s); }

char *ui_box_top(int width, const char *title) {
    const char *h = tui_sym("boxRound.horizontal");
    int inner = MAX(0, width - 2);
    sb_t b = SB_INIT, r = SB_INIT;
    if (!title || !*title) {
        sb_append(&r, tui_sym("boxRound.topLeft"));
        sb_repeat(&r, h, inner);
        sb_append(&r, tui_sym("boxRound.topRight"));
        paint(&b, sb_cstr(&r));
        sb_free(&r);
        return sb_detach(&b);
    }
    char *t = xasprintf(" %s ", title);
    char *shown = tui_truncate_to_width(t, MAX(0, inner - 2), NULL);
    free(t);
    int fill = MAX(0, inner - 1 - tui_visible_width(shown));
    sb_append(&r, tui_sym("boxRound.topLeft"));
    sb_append(&r, h);
    paint(&b, sb_cstr(&r));
    sb_clear(&r);
    char *acc = tui_fgs(NULL, g_box_tone >= 0 ? (tui_tc_t)g_box_tone : TUI_TC_ACCENT, shown);
    char *bold = tui_bold(acc);
    sb_append(&b, bold);
    free(bold);
    free(acc);
    free(shown);
    sb_repeat(&r, h, fill);
    sb_append(&r, tui_sym("boxRound.topRight"));
    paint(&b, sb_cstr(&r));
    sb_free(&r);
    return sb_detach(&b);
}

char *ui_box_row(const char *content, int width) {
    sb_t b = SB_INIT;
    paint(&b, tui_sym("boxRound.vertical"));
    sb_putc(&b, ' ');
    if (width > 4) {
        char *c = ui_fit_line(content, width - 4);
        sb_append(&b, c);
        free(c);
    }
    sb_putc(&b, ' ');
    paint(&b, tui_sym("boxRound.vertical"));
    return sb_detach(&b);
}

char *ui_box_divider(int width) {
    sb_t r = SB_INIT, b = SB_INIT;
    sb_append(&r, tui_sym("boxSharp.teeRight"));
    sb_repeat(&r, tui_sym("boxRound.horizontal"), MAX(0, width - 2));
    sb_append(&r, tui_sym("boxSharp.teeLeft"));
    paint(&b, sb_cstr(&r));
    sb_free(&r);
    return sb_detach(&b);
}

char *ui_box_bottom(int width) {
    sb_t r = SB_INIT, b = SB_INIT;
    sb_append(&r, tui_sym("boxRound.bottomLeft"));
    sb_repeat(&r, tui_sym("boxRound.horizontal"), MAX(0, width - 2));
    sb_append(&r, tui_sym("boxRound.bottomRight"));
    paint(&b, sb_cstr(&r));
    sb_free(&r);
    return sb_detach(&b);
}

/* ---------- gradient brand mark ---------- */

const char *const UI_PI_LOGO[5] = {
    "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88",
    "   \xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88   ",
    "   \xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88   ",
    "   \xe2\x96\x92\xe2\x96\x92  \xe2\x96\x88\xe2\x96\x88   ",
    "       \xe2\x96\x88\xe2\x96\x88   ",
};

char *ui_gradient_escape(double t, double shine_pos, double shine_strength) {
    static const double STOPS[3][3] = {{248, 79, 204}, {147, 98, 244}, {0, 219, 228}};
    static const int RAMP[7] = {206, 170, 134, 99, 69, 74, 44};
    double ss = shine_strength > 0 ? shine_strength : 0;
    if (tui_theme()->mode == TUI_COLOR_TRUECOLOR) {
        double seg = t * 2;
        int i = MIN(1, (int)floor(seg));
        if (i < 0) i = 0;
        double f = seg - i;
        double r = STOPS[i][0] + (STOPS[i + 1][0] - STOPS[i][0]) * f;
        double g = STOPS[i][1] + (STOPS[i + 1][1] - STOPS[i][1]) * f;
        double bl = STOPS[i][2] + (STOPS[i + 1][2] - STOPS[i][2]) * f;
        if (ss > 0) {
            double in = MAX(0, 1 - fabs(t - shine_pos) / 0.18) * ss;
            if (in > 0) {
                r += (255 - r) * in;
                g += (255 - g) * in;
                bl += (255 - bl) * in;
            }
        }
        return xasprintf("\x1b[38;2;%d;%d;%dm", (int)lround(r), (int)lround(g), (int)lround(bl));
    }
    int idx = MIN(6, MAX(0, (int)floor(t * 6 + 0.5)));
    if (ss > 0 && MAX(0, 1 - fabs(t - shine_pos) / 0.18) * ss > 0.5) idx = 6;
    return xasprintf("\x1b[38;5;%dm", RAMP[idx]);
}

/* codepoints of a UTF-8 string (art is cell-per-codepoint) */
static size_t cps(const char *s, strvec_t *out) {
    for (const char *p = s; *p;) {
        size_t l = 1;
        unsigned char c = (unsigned char)*p;
        if (c >= 0xF0) l = 4;
        else if (c >= 0xE0) l = 3;
        else if (c >= 0xC0) l = 2;
        strvec_push_own(out, xstrndup(p, l));
        p += strnlen(p, l);
    }
    return out->n;
}

void ui_gradient_logo(const char *const *lines, size_t n, double phase, double shine_pos, double shine_strength,
                      strvec_t *out) {
    size_t cols = 0;
    for (size_t y = 0; y < n; y++) {
        strvec_t c = {0};
        size_t nc = cps(lines[y], &c);
        cols = MAX(cols, nc);
        strvec_free(&c);
    }
    double xs = MAX(1, (double)cols - 1), ys = MAX(1, (double)n - 1);
    double np = fmod(fmod(phase, 1) + 1, 1);
    for (size_t y = 0; y < n; y++) {
        strvec_t c = {0};
        cps(lines[y], &c);
        sb_t b = SB_INIT;
        for (size_t x = 0; x < c.n; x++) {
            if (!strcmp(c.v[x], " ")) {
                sb_putc(&b, ' ');
                continue;
            }
            double base = ((double)x / xs + (double)y / ys) / 2;
            double t = np == 0 ? base : fmod(base + np, 1);
            char *e = ui_gradient_escape(t, shine_pos, shine_strength);
            sb_append(&b, e);
            sb_append(&b, c.v[x]);
            sb_append(&b, TUI_RESET);
            free(e);
        }
        strvec_free(&c);
        strvec_push_own(out, sb_detach(&b));
    }
}

/* ---------- OverlayPanel ---------- */

static void panel_render(tui_component_t *c, int width, strvec_t *out) {
    ui_panel_t *p = (ui_panel_t *)c;
    int inner = MAX(1, width - 4);
    strvec_push_own(out, ui_box_top(width, p->title));
    for (size_t i = 0; i < p->c.children.n; i++) {
        strvec_t lines = {0};
        tui_component_render(p->c.children.v[i], inner, &lines);
        for (size_t k = 0; k < lines.n; k++) strvec_push_own(out, ui_box_row(lines.v[k], width));
        strvec_free(&lines);
    }
    strvec_push_own(out, ui_box_bottom(width));
}

static bool panel_key(tui_component_t *c, const tui_key_t *k) {
    ui_panel_t *p = (ui_panel_t *)c;
    for (size_t i = 0; i < p->c.children.n; i++)
        if (p->c.children.v[i]->vt->handle_key && tui_component_handle_key(p->c.children.v[i], k)) return true;
    return false;
}

static void panel_invalidate(tui_component_t *c) {
    ui_panel_t *p = (ui_panel_t *)c;
    for (size_t i = 0; i < p->c.children.n; i++) tui_component_invalidate(p->c.children.v[i]);
}

static void panel_destroy(tui_component_t *c) {
    ui_panel_t *p = (ui_panel_t *)c;
    tui_container_destroy_children(&p->c);
    vec_free(&p->c.children);
    free(p->title);
    free(p);
}

static const tui_component_vt PANEL_VT = {"overlay-panel", panel_render, panel_key, panel_invalidate, panel_destroy};

ui_panel_t *ui_panel_new(const char *title) {
    ui_panel_t *p = xcalloc(1, sizeof *p);
    tui_container_init(&p->c);
    p->c.base.vt = &PANEL_VT;
    ui_panel_set_title(p, title);
    return p;
}

void ui_panel_set_title(ui_panel_t *p, const char *title) {
    /* titles inset into a border row never carry line breaks */
    char *t = xstrdup(title ? title : "");
    for (char *q = t; *q; q++)
        if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
    char *tr = str_trim(t);
    free(t);
    free(p->title);
    p->title = tr;
}

/* ---------- WizardStep ---------- */

void ui_wizard_step_render(const ui_wizard_step_t *s, int width, strvec_t *out) {
    int gap = s->gap < 0 ? 1 : s->gap;
    int minc = s->min_content > 0 ? s->min_content : 1;
    const strvec_t *slots[5] = {s->heading, s->intro, s->preview, s->status, s->footer};
    bool has[5];
    for (int i = 0; i < 5; i++) has[i] = slots[i] && slots[i]->n > 0;
    if (has[2] && s->preview_optional && s->max_height >= 0) {
        int fixed = 0, nfixed = 0;
        for (int i = 0; i < 5; i++)
            if (i != 2 && has[i]) {
                fixed += (int)slots[i]->n;
                nfixed++;
            }
        int required = fixed + MAX(0, nfixed) * gap + minc; /* (nfixed + 1 - 1) gaps */
        if (required + (int)slots[2]->n + gap > s->max_height) has[2] = false;
    }
    int surround = 0, nslots = 1;
    for (int i = 0; i < 5; i++)
        if (has[i]) {
            surround += (int)slots[i]->n;
            nslots++;
        }
    int budget = s->max_height < 0 ? -1 : MAX(0, s->max_height - surround - (nslots - 1) * gap);
    if (s->fit) s->fit(s->ud, budget);
    strvec_t content = {0};
    if (s->content) s->content(s->ud, MAX(1, width), &content);
    if (budget >= 0)
        while ((int)content.n > budget) free(content.v[--content.n]);
    const strvec_t *ordered[6] = {has[0] ? slots[0] : NULL, has[1] ? slots[1] : NULL, has[2] ? slots[2] : NULL,
                                  content.n ? &content : NULL, has[3] ? slots[3] : NULL, has[4] ? slots[4] : NULL};
    strvec_t lines = {0};
    for (int i = 0; i < 6; i++) {
        if (!ordered[i]) continue;
        if (lines.n && gap > 0)
            for (int g = 0; g < gap; g++) strvec_push(&lines, "");
        for (size_t k = 0; k < ordered[i]->n; k++) strvec_push(&lines, ordered[i]->v[k]);
    }
    for (size_t i = 0; i < lines.n; i++) {
        if (s->max_height >= 0 && (int)i >= s->max_height) break;
        strvec_push(out, lines.v[i]);
    }
    strvec_free(&lines);
    strvec_free(&content);
}

/* ---------- ScrollView over pre-rendered lines (components/scroll-view.ts, scrollbar "auto") ---------- */

void ui_scroll_view(const strvec_t *lines, int height, int *offset, int width, strvec_t *out) {
    int total = (int)lines->n;
    bool bar = total > height && width > 1;
    int maxoff = MAX(0, total - height);
    *offset = MAX(0, MIN(*offset, maxoff));
    int cw = MAX(0, width - (bar ? 1 : 0));
    int ts = 0, te = height;
    if (bar) {
        int size = MAX(1, MIN((height * height) / total, height));
        ts = maxoff ? (int)lround((double)*offset / maxoff * (height - size)) : 0;
        te = ts + size;
    }
    for (int row = 0; row < height; row++) {
        int i = *offset + row;
        const char *src = i < total ? lines->v[i] : "";
        char *rt = tui_replace_tabs(src);
        char *tr = tui_truncate_to_width(rt, cw, NULL);
        free(rt);
        if (!bar) {
            strvec_push_own(out, tr);
            continue;
        }
        char *pad = tui_pad_to_width(tr, cw);
        bool thumb = row >= ts && row < te;
        char *g = tui_fgs(NULL, thumb ? TUI_TC_ACCENT : TUI_TC_MUTED, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
        strvec_push_own(out, xasprintf("%s%s", pad, g));
        free(g);
        free(pad);
        free(tr);
    }
}

/* ---------- TabBar (components/tab-bar.ts with chrome/shared getTabBarTheme) ---------- */

static char *tab_style(int kind, const char *text) {
    if (kind == 1) { /* active */
        char *f = tui_fgs(NULL, TUI_TC_TEXT, text), *b = tui_bgs(NULL, TUI_TC_SELECTED_BG, f), *r = tui_bold(b);
        free(f);
        free(b);
        return r;
    }
    return tui_fgs(NULL, kind == 2 ? TUI_TC_DIM : TUI_TC_MUTED, text);
}

static void chunks_clear(strvec_t *v) {
    for (size_t i = 0; i < v->n; i++) free(v->v[i]);
    v->n = 0;
}

void ui_tab_bar_render(const char *label, const ui_tab_t *tabs, size_t n, size_t active, bool show_hint, int width,
                       strvec_t *out) {
    int maxw = MAX(1, width);
    const char **labels = xcalloc(n + 1, sizeof *labels);
    for (size_t i = 0; i < n; i++) labels[i] = tabs[i].label;
    strvec_t chunks = {0};
    for (;;) {
        chunks_clear(&chunks);
        if (label && *label) {
            char *l = xasprintf("%s:", label), *a = tui_fgs(NULL, TUI_TC_ACCENT, l);
            strvec_push_own(&chunks, tui_bold(a));
            free(a);
            free(l);
            strvec_push(&chunks, "  ");
        }
        for (size_t i = 0; i < n; i++) {
            char *t = xasprintf(" %s ", labels[i]);
            strvec_push_own(&chunks, tab_style(tabs[i].muted ? 2 : i == active ? 1 : 0, t));
            free(t);
            if (i + 1 < n) strvec_push(&chunks, "  ");
        }
        if (show_hint) {
            strvec_push(&chunks, "  ");
            char *k = ui_key_hint("tab"), *h = xasprintf("(%s to cycle)", k);
            strvec_push_own(&chunks, tui_fgs(NULL, TUI_TC_DIM, h));
            free(h);
            free(k);
        }
        int total = 0;
        for (size_t i = 0; i < chunks.n; i++) total += tui_visible_width(chunks.v[i]);
        if (total <= maxw) break;
        /* collapse, farthest from the active tab first, the tabs that have a short label */
        size_t best = n;
        for (size_t i = 0; i < n; i++) {
            if (i == active || !tabs[i].short_label || labels[i] == tabs[i].short_label) continue;
            size_t d = i > active ? i - active : active - i, bd = best > active ? best - active : active - best;
            if (best == n || d > bd) best = i;
        }
        if (best == n) break;
        labels[best] = tabs[best].short_label;
    }
    sb_t line = SB_INIT;
    int cur = 0;
    size_t before = out->n;
    for (size_t i = 0; i < chunks.n; i++) {
        int w = tui_visible_width(chunks.v[i]);
        if (w <= 0) continue;
        if (w > maxw) {
            if (cur) {
                strvec_push(out, sb_cstr(&line));
                sb_clear(&line);
                cur = 0;
            }
            strvec_push_own(out, tui_truncate_to_width(chunks.v[i], maxw, ""));
            continue;
        }
        if (cur > 0 && cur + w > maxw) {
            strvec_push(out, sb_cstr(&line));
            sb_clear(&line);
            cur = 0;
        }
        sb_append(&line, chunks.v[i]);
        cur += w;
    }
    if (cur) strvec_push(out, sb_cstr(&line));
    if (out->n == before) strvec_push(out, "");
    sb_free(&line);
    strvec_free(&chunks);
    free(labels);
}

/* ---------- single-line Input (components/input.ts) ---------- */

static size_t in_prev_cp(const char *s, size_t i) {
    if (!i) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
    return i;
}

static size_t in_next_cp(const char *s, size_t n, size_t i) {
    if (i >= n) return n;
    i++;
    while (i < n && ((unsigned char)s[i] & 0xc0) == 0x80) i++;
    return i;
}

static void in_delete(ui_input_t *in, size_t a, size_t b) {
    if (b <= a) return;
    memmove(in->v.s + a, in->v.s + b, in->v.len - b + 1);
    in->v.len -= b - a;
    in->cur = a;
}

static void in_insert(ui_input_t *in, const char *t) {
    sb_t n = SB_INIT;
    sb_appendn(&n, in->v.s ? in->v.s : "", in->cur);
    for (const char *p = t; *p; p++) sb_putc(&n, *p == '\n' || *p == '\r' || *p == '\t' ? ' ' : *p);
    size_t cur = n.len;
    if (in->v.s) sb_append(&n, in->v.s + in->cur);
    sb_free(&in->v);
    in->v = n;
    in->cur = cur;
}

static bool in_is_word(char ch) { return isalnum((unsigned char)ch) || ((unsigned char)ch & 0x80) || ch == '_'; }

static size_t in_word_left(const char *s, size_t i) {
    while (i > 0 && !in_is_word(s[i - 1])) i--;
    while (i > 0 && in_is_word(s[i - 1])) i--;
    return i;
}

static size_t in_word_right(const char *s, size_t n, size_t i) {
    while (i < n && !in_is_word(s[i])) i++;
    while (i < n && in_is_word(s[i])) i++;
    return i;
}

const char *ui_input_value(const ui_input_t *in) { return in->v.s ? in->v.s : ""; }

void ui_input_set(ui_input_t *in, const char *s) {
    sb_free(&in->v);
    sb_append(&in->v, s ? s : "");
    in->cur = in->v.len;
}

void ui_input_free(ui_input_t *in) {
    sb_free(&in->v);
    in->cur = 0;
}

bool ui_input_key(ui_input_t *in, const tui_key_t *k) {
    const char *s = in->v.s ? in->v.s : "";
    size_t n = in->v.len;
    if (k->kind == TUI_KEY_PASTE && k->text) {
        in_insert(in, k->text);
        return true;
    }
    const char *t = tui_key_text(k);
    if (t && *t && (unsigned char)*t >= 0x20) {
        in_insert(in, t);
        return true;
    }
    if (tui_kb(k, "tui.editor.deleteCharBackward")) in_delete(in, in_prev_cp(s, in->cur), in->cur);
    else if (tui_kb(k, "tui.editor.deleteCharForward")) in_delete(in, in->cur, in_next_cp(s, n, in->cur));
    else if (tui_kb(k, "tui.editor.deleteWordBackward")) in_delete(in, in_word_left(s, in->cur), in->cur);
    else if (tui_kb(k, "tui.editor.deleteWordForward")) in_delete(in, in->cur, in_word_right(s, n, in->cur));
    else if (tui_kb(k, "tui.editor.deleteToLineStart")) in_delete(in, 0, in->cur);
    else if (tui_kb(k, "tui.editor.deleteToLineEnd")) in_delete(in, in->cur, n);
    else if (tui_kb(k, "tui.editor.cursorWordLeft")) in->cur = in_word_left(s, in->cur);
    else if (tui_kb(k, "tui.editor.cursorWordRight")) in->cur = in_word_right(s, n, in->cur);
    else if (tui_kb(k, "tui.editor.cursorLeft")) in->cur = in_prev_cp(s, in->cur);
    else if (tui_kb(k, "tui.editor.cursorRight")) in->cur = in_next_cp(s, n, in->cur);
    else if (tui_kb(k, "tui.editor.cursorLineStart")) in->cur = 0;
    else if (tui_kb(k, "tui.editor.cursorLineEnd")) in->cur = n;
    else return false;
    return true;
}

char *ui_input_render(const ui_input_t *in, const char *prompt, int width, bool focused) {
    return ui_input_render_hw(in, prompt, width, focused, false);
}

char *ui_input_render_hw(const ui_input_t *in, const char *prompt, int width, bool focused, bool hw_cursor) {
    int avail = width - tui_visible_width(prompt);
    if (avail <= 0) return tui_truncate_to_width(prompt, MAX(0, width), "");
    const char *s = in->v.s ? in->v.s : "";
    /* window the value around the cursor: the cursor cell (a trailing space at the end) stays visible */
    char *before_all = xstrndup(s, in->cur);
    int bw = tui_visible_width(before_all);
    size_t after_cur = in_next_cp(s, in->v.len, in->cur);
    char *at = in->cur < in->v.len ? xstrndup(s + in->cur, after_cur - in->cur) : xstrdup(" ");
    int aw = MAX(1, tui_visible_width(at));
    int skip = MAX(0, bw + aw - avail);
    char *before = tui_slice_cols(before_all, skip, bw - skip);
    int rem = MAX(0, avail - tui_visible_width(before) - aw);
    char *after = tui_slice_cols(in->cur < in->v.len ? s + after_cur : "", 0, rem);
    sb_t b = SB_INIT;
    sb_append(&b, prompt);
    sb_append(&b, before);
    if (focused) sb_append(&b, TUI_CURSOR_MARKER);
    if (hw_cursor) sb_append(&b, at); /* the terminal draws its own cursor (Input #cursorCell) */
    else sb_appendf(&b, "\x1b[7m%s\x1b[27m", at);
    sb_append(&b, after);
    int used = tui_visible_width(before) + aw + tui_visible_width(after);
    tui_sb_spaces(&b, MAX(0, avail - used));
    free(before_all), free(at), free(before), free(after);
    return sb_detach(&b);
}
