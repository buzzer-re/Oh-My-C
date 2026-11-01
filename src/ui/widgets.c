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

static void paint(sb_t *b, const char *s) { tui_fg(b, NULL, TUI_TC_BORDER, s); }

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
    char *acc = tui_fgs(NULL, TUI_TC_ACCENT, shown);
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
    sb_append(&r, tui_sym("boxRound.teeRight"));
    sb_repeat(&r, tui_sym("boxRound.horizontal"), MAX(0, width - 2));
    sb_append(&r, tui_sym("boxRound.teeLeft"));
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
