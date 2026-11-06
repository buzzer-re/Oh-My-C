/* keywordglow.c - the magic-keyword glow (port of omp prompt/gradient-highlight.ts, prompt/magic-keywords.ts highlight side and
 * CustomEditor's shimmer frame chain): standalone prose keywords paint with a per-word HSL gradient in the composer and in sent
 * bubbles; the composer's rotates while the prompt is focused. */
#include "ui/ui.h"

#include "coding/keywords.h"
#include "config/settings.h"
#include "tui/ansi.h"
#include "tui/motion.h"

#include <math.h>

#define GRADIENT_STOPS 14
#define GRADIENT_SATURATION 90.0
#define SHIMMER_PERIOD_MS 1800
#define SHIMMER_FRAME_MS 70

/* hsl -> rgb as CSS color functions compute it */
static void hsl_rgb(double h, double s, double l, int *r, int *g, int *b) {
    s /= 100;
    l /= 100;
    double a = s * MIN(l, 1 - l);
    double ch[3];
    static const double N[3] = {0, 8, 4};
    for (int i = 0; i < 3; i++) {
        double k = fmod(N[i] + h / 30, 12);
        double m = MIN(k - 3, MIN(9 - k, 1.0));
        ch[i] = l - a * MAX(-1.0, m);
    }
    *r = (int)lround(ch[0] * 255);
    *g = (int)lround(ch[1] * 255);
    *b = (int)lround(ch[2] * 255);
}

static void stop_seq(int keyword, int stop, char *out, size_t cap) {
    double hf, ht, lf, lt;
    keywords_glow_spec(keyword, &hf, &ht, &lf, &lt);
    double t = (double)stop / GRADIENT_STOPS;
    double hue = fmod(hf + t * (ht - hf), 360);
    int r, g, b;
    hsl_rgb(round(hue), GRADIENT_SATURATION, round(lf + t * (lt - lf)), &r, &g, &b);
    if (tui_theme()->mode == TUI_COLOR_TRUECOLOR) snprintf(out, cap, "\x1b[38;2;%d;%d;%dm", r, g, b);
    else snprintf(out, cap, "\x1b[38;5;%dm", tui_rgb_to_256(r, g, b));
}

/* the escape starting at s[0] (CSI, OSC, APC): its length */
static size_t escape_len(const char *s) {
    if (s[0] != 0x1b) return 0;
    if (s[1] == '[') {
        size_t i = 2;
        while (s[i] && !(s[i] >= 0x40 && s[i] <= 0x7e)) i++;
        return s[i] ? i + 1 : i;
    }
    if (s[1] == ']' || s[1] == '_' || s[1] == 'P') {
        size_t i = 2;
        while (s[i] && s[i] != '\a' && !(s[i] == 0x1b && s[i + 1] == '\\')) i++;
        if (!s[i]) return i;
        return s[i] == '\a' ? i + 1 : i + 2;
    }
    return s[1] ? 2 : 1;
}

char *ui_keywords_paint(const char *row, const char *reset_to, double phase, const char *only_fg) {
    /* the row's text, and where each byte came from */
    size_t n = strlen(row);
    sb_t plain = SB_INIT;
    size_t *origin = xmalloc((n + 1) * sizeof *origin);
    for (size_t i = 0; i < n;) {
        size_t e = escape_len(row + i);
        if (e) {
            i += e;
            continue;
        }
        origin[plain.len] = i;
        sb_putc(&plain, row[i]);
        i++;
    }
    keyword_range_t *rs = NULL;
    size_t nr = keywords_ranges(sb_cstr(&plain), &rs);
    if (!nr) {
        free(rs);
        free(origin);
        sb_free(&plain);
        return NULL;
    }
    /* per plain byte: the keyword painted over it and its index in the word */
    int *owner = xmalloc((plain.len + 1) * sizeof *owner);
    size_t *range_of = xmalloc((plain.len + 1) * sizeof *range_of);
    for (size_t i = 0; i < plain.len; i++) owner[i] = -1;
    for (size_t r = 0; r < nr; r++)
        for (size_t i = rs[r].start; i < rs[r].end; i++) {
            owner[i] = rs[r].keyword;
            range_of[i] = r;
        }
    double wrapped = fmod(fmod(phase, 1) + 1, 1);
    const char *reset = reset_to ? reset_to : "\x1b[39m";
    sb_t b = SB_INIT;
    char active_fg[48] = "", prev[32] = "";
    bool skip_range = false, painted = false;
    size_t pi = 0, cur_range = (size_t)-1;
    for (size_t i = 0; i < n;) {
        size_t e = escape_len(row + i);
        if (e) {
            sb_appendn(&b, row + i, e);
            if (row[i + 1] == '[' && row[i + e - 1] == 'm') {
                /* the row's own style took over: the next character paints its stop again */
                prev[0] = 0;
                if (e < sizeof active_fg && (!strncmp(row + i, "\x1b[38;", 5))) {
                    memcpy(active_fg, row + i, e);
                    active_fg[e] = 0;
                } else if (!strncmp(row + i, "\x1b[39m", 5) || !strncmp(row + i, "\x1b[0m", 4)) active_fg[0] = 0;
            }
            i += e;
            continue;
        }
        int kw = pi < plain.len ? owner[pi] : -1;
        if (kw >= 0) {
            size_t r = range_of[pi];
            if (r != cur_range) {
                cur_range = r;
                skip_range = only_fg && strcmp(active_fg, only_fg) != 0;
                prev[0] = 0;
            }
            if (!skip_range) {
                size_t len = rs[r].end - rs[r].start, at = pi - rs[r].start;
                double t = fmod((double)at / (double)len + wrapped, 1);
                int stop = (int)floor(t * GRADIENT_STOPS) % GRADIENT_STOPS;
                char seq[32];
                stop_seq(kw, stop, seq, sizeof seq);
                if (strcmp(seq, prev)) {
                    sb_append(&b, seq);
                    snprintf(prev, sizeof prev, "%s", seq);
                }
                painted = true;
            }
        }
        sb_putc(&b, row[i]);
        i++;
        pi++;
        if (kw >= 0 && !skip_range && pi == rs[cur_range].end) {
            sb_append(&b, reset);
            prev[0] = 0;
        }
    }
    free(owner);
    free(range_of);
    free(rs);
    free(origin);
    sb_free(&plain);
    if (!painted) {
        sb_free(&b);
        return NULL;
    }
    return sb_detach(&b);
}

/* ---------- the composer ---------- */

static int g_frame_timer;

static void frame_cb(void *ud) {
    ui_t *u = ud;
    g_frame_timer = 0;
    tui_request_render(u->tui);
}

static char *row_hook(void *ud, tui_editor_t *ed, const char *row) {
    ui_t *u = ud;
    /* with decorative motion off the keyword keeps its static highlight; the pending frame chain ends here */
    bool animated = ed->base.focused && settings_bool("magicKeywords.enabled", true) && tui_motion_effects_enabled();
    double phase = animated ? (double)(now_ms() % SHIMMER_PERIOD_MS) / SHIMMER_PERIOD_MS : 0;
    char *r = ui_keywords_paint(row, NULL, phase, NULL);
    /* the next render decides whether to schedule another, so the chain stops by itself */
    if (r && animated && !g_frame_timer) g_frame_timer = tui_add_timer(u->tui, SHIMMER_FRAME_MS, false, frame_cb, u);
    return r;
}

void ui_keywords_attach(ui_t *u) { tui_editor_set_row_hook(u->editor, row_hook, u); }
