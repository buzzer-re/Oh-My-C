/* appearance.c - theme / glyph preset / color-blind state (port of omp theme.ts selection logic:
 * titanium is the default dark theme, light the default light one; colorBlindMode shifts
 * toolDiffAdded toward blue) and the composer-shape preview (overlays/composer-shape-preview.ts). */
#include "ui/overlays.h"
#include "ui/ui.h"

#include "config/settings.h"
#include "core/embed.h"
#include "core/fs.h"
#include "tui/motion.h"

#include <math.h>

static char *g_theme_name;
static bool g_color_blind;

const char *ui_theme_current_name(void) { return g_theme_name; }

/* cfgMotionResolved + its effect: tui.motion's preset under the user's own spinner interval and shimmer; a setting set anywhere
 * (file, flag, env) wins over the preset. Also tui.maxFps. */
void ui_apply_motion(void) {
    tui_motion_mode_t mode = TUI_MOTION_FULL;
    settings_parse_motion_mode(settings_str("tui.motion", "full"), &mode, NULL);
    int interval = tui_motion_preset_interval(mode);
    const json_t *si = settings_get("tui.spinnerInterval");
    if (si && !json_is_null(si)) {
        char *t = json_is_str(si) ? xstrdup(json_sv(si)) : json_dump(si, 0);
        int v;
        if (settings_parse_spinner_interval(t, &v, NULL)) interval = v;
        free(t);
    }
    tui_set_spinner_interval(interval);
    int fps = TUI_DEFAULT_MAX_FPS;
    const json_t *mf = settings_get("tui.maxFps");
    if (mf && !json_is_null(mf)) {
        char *t = json_is_str(mf) ? xstrdup(json_sv(mf)) : json_dump(mf, 0);
        int v;
        if (settings_parse_max_fps(t, &v, NULL)) fps = v;
        free(t);
    }
    tui_set_max_fps(fps);
    bool effects = tui_motion_preset_effects(mode);
    tui_set_motion_effects(effects);
    const char *sh = settings_str("display.shimmer", "classic");
    tui_set_shimmer_mode(!effects || str_eq(sh, "disabled") ? TUI_SHIMMER_DISABLED : str_eq(sh, "kitt") ? TUI_SHIMMER_KITT : TUI_SHIMMER_CLASSIC);
}

/* HSV shift used by colorBlindMode (loader.ts COLORBLIND_ADJUSTMENT = {h: 60, s: 0.71}) */
static int64_t adjust_hsv(int64_t rgb) {
    double r = ((rgb >> 16) & 255) / 255.0, g = ((rgb >> 8) & 255) / 255.0, b = (rgb & 255) / 255.0;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn;
    double h = 0, s = mx == 0 ? 0 : d / mx, v = mx;
    if (d > 0) {
        if (mx == r) h = 60 * fmod((g - b) / d, 6);
        else if (mx == g) h = 60 * ((b - r) / d + 2);
        else h = 60 * ((r - g) / d + 4);
    }
    if (h < 0) h += 360;
    h = fmod(h + 60, 360);
    s = fmin(1, s * 0.71);
    double c = v * s, x = c * (1 - fabs(fmod(h / 60, 2) - 1)), m = v - c;
    double rr = 0, gg = 0, bb = 0;
    if (h < 60) rr = c, gg = x;
    else if (h < 120) rr = x, gg = c;
    else if (h < 180) gg = c, bb = x;
    else if (h < 240) gg = x, bb = c;
    else if (h < 300) rr = x, bb = c;
    else rr = c, bb = x;
    int R = (int)lround((rr + m) * 255), G = (int)lround((gg + m) * 255), B = (int)lround((bb + m) * 255);
    return ((int64_t)R << 16) | (G << 8) | B;
}

static void apply_color_blind(tui_theme_t *t) {
    if (!g_color_blind) return;
    int64_t rgb = t->rgb[TUI_TC_TOOL_DIFF_ADDED];
    if (rgb < 0) return;
    int64_t n = adjust_hsv(rgb);
    t->rgb[TUI_TC_TOOL_DIFF_ADDED] = n;
    int r = (int)((n >> 16) & 255), g = (int)((n >> 8) & 255), b = (int)(n & 255);
    if (t->mode == TUI_COLOR_TRUECOLOR) snprintf(t->fg[TUI_TC_TOOL_DIFF_ADDED], sizeof t->fg[0], "\x1b[38;2;%d;%d;%dm", r, g, b);
    else snprintf(t->fg[TUI_TC_TOOL_DIFF_ADDED], sizeof t->fg[0], "\x1b[38;5;%dm", tui_rgb_to_256(r, g, b));
}

bool ui_theme_preview(const char *name, char **err) {
    if (!name) return false;
    char *e = NULL;
    tui_theme_t *t = tui_theme_load(name, tui_detect_color_mode(), &e);
    if (!t) {
        if (err) *err = e;
        else free(e);
        return false;
    }
    free(e);
    apply_color_blind(t);
    tui_theme_set(t);
    if (g_theme_name != name) {
        free(g_theme_name);
        g_theme_name = xstrdup(name);
    }
    return true;
}

void ui_set_color_blind(bool on) { g_color_blind = on; }

static bool terminal_is_light(void) { return str_eq(tui_detect_theme_name(), "light"); }

void ui_theme_auto(void) {
    const char *name = terminal_is_light() ? settings_str("theme.light", "light") : settings_str("theme.dark", "titanium");
    if (!ui_theme_preview(name, NULL)) ui_theme_preview(terminal_is_light() ? "light" : "titanium", NULL);
}

void ui_apply_appearance(void) {
    const char *preset = settings_str("symbolPreset", "unicode");
    if (!tui_sym_set_preset(preset)) tui_sym_set_preset("unicode");
    g_color_blind = settings_bool("colorBlindMode", false);
    ui_theme_auto();
}

bool ui_theme_is_light(const char *name) {
    tui_theme_t *t = tui_theme_load(name, TUI_COLOR_TRUECOLOR, NULL);
    if (!t) return false;
    int64_t rgb = t->rgb[TUI_TC_STATUS_LINE_BG];
    tui_theme_free(t);
    if (rgb < 0) return false;
    double r = ((rgb >> 16) & 255) / 255.0, g = ((rgb >> 8) & 255) / 255.0, b = (rgb & 255) / 255.0;
    return 0.2126 * r + 0.7152 * g + 0.0722 * b > 0.5;
}

void ui_theme_list(strvec_t *out) {
    strvec_t names = {0};
    for (size_t i = 0; i < omc_embedded_count; i++) {
        const char *n = omc_embedded[i].name;
        if (!str_starts(n, "assets/tui/themes/") || !str_ends(n, ".json")) continue;
        const char *b = n + strlen("assets/tui/themes/");
        strvec_push_own(&names, xstrndup(b, strlen(b) - 5));
    }
    char *dir = omc_agent_path("themes");
    dirlist_t dl = {0};
    if (fs_list_dir(dir, &dl) == 0)
        for (size_t i = 0; i < dl.n; i++) {
            const char *n = dl.v[i].name;
            if (!str_ends(n, ".json")) continue;
            char *base = xstrndup(n, strlen(n) - 5);
            if (!strvec_contains(&names, base)) strvec_push_own(&names, base);
            else free(base);
        }
    dirlist_free(&dl);
    free(dir);
    strvec_sort(&names);
    for (size_t i = 0; i < names.n; i++) strvec_push(out, names.v[i]);
    strvec_free(&names);
}

/* ---------- composer shapes ---------- */

const ui_composer_shape_t UI_COMPOSER_SHAPES[8] = {
    {"band", "Status Band (Default)", "Flush soft-capped status band above a curved prompt, no frame"},
    {"box", "Rounded Box", "Status line embedded in top border, compact 2-line prompt"},
    {"claude", "Claude Code", "Full-width horizontal rules above and below, status line at bottom"},
    {"pi", "Pi", "Framed horizontal rules with status line at bottom"},
    {"borderless", "Borderless", "Clean prompt glyph with status line at bottom, no box borders"},
    {"rule", "Top Rule Dock", "Single top rule with status docked onto it and below"},
    {"field", "Compact Field", "Filled one-row field with accent end caps"},
    {"rail", "Accent Rail", "Filled one-row field anchored by a single accent rail"},
};

/* ---------- status rows for the preview: the live status line (statusline.c) in each layout ---------- */

/* render with upstream's previewTitle: an unnamed session shows the stand-in title */
static char *render_titled(ui_t *u, int width, const char *layout, const char *title) {
    session_t *ss = u->app ? u->app->session : NULL;
    bool stand_in = ss && title && (!ss->title || !*ss->title);
    char *saved = stand_in ? ss->title : NULL;
    if (stand_in) ss->title = (char *)title;
    char *s = ui_status_render(u, width, layout);
    if (stand_in) ss->title = saved;
    return s;
}

static char *status_layout(ui_t *u, int width, const char *layout, const char *title, int *out_width) {
    char *s = render_titled(u, width, layout, title);
    if (out_width) *out_width = tui_visible_width(s);
    return s;
}

char *ui_status_band_top(ui_t *u, int width, const char *title, int *out_width) {
    return status_layout(u, width, "band", title, out_width);
}

char *ui_status_top_border(ui_t *u, int width, const char *title, int *out_width) {
    return status_layout(u, width, "box", title, out_width);
}

char *ui_status_rule_chip(ui_t *u, int width, const char *title, int *out_width) {
    return status_layout(u, width, "plain-right", title, out_width);
}

char *ui_status_bottom_bar(ui_t *u, int width, bool full, const char *title) {
    return render_titled(u, width, full ? "plain-full" : "plain-left", title);
}

/* ---------- composer styles (components/composer) ---------- */

typedef struct {
    const char *id;
    bool filled;
    int pad_default;   /* defaultPaddingX(undefined) */
    int chrome_extra;  /* sideChromeWidth = paddingX + chrome_extra (band/borderless: always 0) */
    bool zero_chrome;
    int attach;        /* 0 none, 1 top-border, 2 top-band, 3 top-rule-chip */
    int bottom_bar;    /* 0 none, 1 left, 2 full */
    bool bar_gap;
    const char *gutter;
} cstyle_t;

static const cstyle_t STYLES[8] = {
    {"band", false, 0, 0, true, 2, 0, false, "\xe2\x95\xb0\xe2\x94\x80 "},
    {"box", false, 2, 1, false, 1, 0, false, NULL},
    {"claude", false, 0, 0, false, 3, 1, false, "\xe2\x9d\xaf "},
    {"pi", false, 1, 0, false, 0, 2, false, NULL},
    {"borderless", false, 0, 0, true, 0, 2, false, "\xe2\x9d\xaf "},
    {"rule", false, 0, 0, false, 3, 1, true, "\xe2\x9d\xaf "},
    {"field", true, 1, 1, false, 0, 2, true, NULL},
    {"rail", true, 1, 1, false, 0, 2, true, NULL},
};

static char *rep(const char *s, int n) {
    sb_t b = SB_INIT;
    sb_repeat(&b, s, MAX(0, n));
    return sb_detach(&b);
}

static char *bc(const char *s) { return tui_fgs(NULL, TUI_TC_BORDER_ACCENT, s); }

void ui_composer_preview(ui_t *u, const char *shape, int width, strvec_t *out) {
    const cstyle_t *st = &STYLES[0];
    for (int i = 0; i < 8; i++)
        if (str_eq(STYLES[i].id, shape)) st = &STYLES[i];
    int pw = MAX(24, width);
    int padx = st->pad_default;
    int chrome = st->zero_chrome ? 0 : padx + st->chrome_extra;
    const char *H = tui_sym("boxRound.horizontal");
    char *top = NULL;
    int topw = 0;
    const char *title = "omp"; /* upstream PREVIEW_TITLE: stand-in session title */
    if (st->attach == 1) top = ui_status_top_border(u, MAX(1, pw - chrome * 2), title, &topw);
    else if (st->attach == 2) top = ui_status_band_top(u, pw, title, &topw);
    else if (st->attach == 3) top = ui_status_rule_chip(u, pw, title, &topw);
    const char *gutter = st->gutter ? st->gutter : "";
    int content_w = MAX(1, pw - chrome * 2 - tui_visible_width(gutter));
    char *prompt = tui_truncate_to_width("Ask anything, edit files, run tools", MAX(1, content_w - 1), NULL);
    char *row = xasprintf("%s\x1b[7m \x1b[27m", prompt);
    char *text = st->filled ? xstrdup(row) : tui_fgs(NULL, TUI_TC_TEXT, row);
    char *pad = xasprintf("%*s", MAX(0, content_w - tui_visible_width(prompt) - 1), "");
    char *sg = *gutter ? tui_fgs(NULL, TUI_TC_ACCENT, gutter) : xstrdup("");
    /* top */
    if (!strcmp(st->id, "band")) {
        strvec_push_own(out, top && *top ? (topw > pw ? tui_truncate_to_width(top, pw, NULL) : xstrdup(top)) : xstrdup(""));
    } else if (!strcmp(st->id, "box")) {
        char *tl = xasprintf("%s%s", tui_sym("boxRound.topLeft"), H), *tr = xasprintf("%s%s", H, tui_sym("boxRound.topRight"));
        /* paddingX=2: topLeft + horizontal*padx */
        free(tl);
        free(tr);
        char *h2 = rep(H, padx);
        char *l = xasprintf("%s%s", tui_sym("boxRound.topLeft"), h2), *r = xasprintf("%s%s", h2, tui_sym("boxRound.topRight"));
        char *lc = bc(l), *rc = bc(r);
        int fillw = MAX(0, pw - chrome * 2);
        char *mid;
        if (!top) {
            char *f = rep(H, fillw);
            mid = bc(f);
            free(f);
        } else if (topw <= fillw) {
            char *f = rep(H, fillw - topw), *fc = bc(f);
            mid = xasprintf("%s%s", top, fc);
            free(f);
            free(fc);
        } else {
            char *t2 = tui_truncate_to_width(top, MAX(0, fillw - 1), NULL);
            char *f = rep(H, MAX(0, fillw - tui_visible_width(t2))), *fc = bc(f);
            mid = xasprintf("%s%s", t2, fc);
            free(t2);
            free(f);
            free(fc);
        }
        strvec_push_own(out, xasprintf("%s%s%s", lc, mid, rc));
        free(mid);
        free(lc);
        free(rc);
        free(l);
        free(r);
        free(h2);
    } else if (!strcmp(st->id, "claude") || !strcmp(st->id, "rule")) {
        if (top && topw > 0 && pw > 2) {
            char *content = top;
            int cw = topw;
            char *tr = NULL;
            if (cw > pw - 2) {
                tr = tui_truncate_to_width(top, pw - 2, NULL);
                content = tr;
                cw = tui_visible_width(tr);
            }
            char *f = rep(H, MAX(0, pw - cw - 1)), *fc = bc(f), *hc = bc(H);
            strvec_push_own(out, xasprintf("%s%s%s", fc, content, hc));
            free(f);
            free(fc);
            free(hc);
            free(tr);
        } else {
            char *f = rep(H, pw);
            strvec_push_own(out, bc(f));
            free(f);
        }
    } else if (!strcmp(st->id, "pi")) {
        char *f = rep(H, pw);
        strvec_push_own(out, bc(f));
        free(f);
    }
    /* row */
    if (!strcmp(st->id, "box")) {
        char *p1 = xasprintf("%*s", MAX(0, padx - 1), "");
        char *l = xasprintf("%s%s%s", tui_sym("boxRound.bottomLeft"), H, p1);
        int right = MAX(1, padx + 1);
        char *p2 = xasprintf("%*s", MAX(0, right - 2), "");
        char *r = xasprintf("%s%s%s", p2, right >= 2 ? H : "", tui_sym("boxRound.bottomRight"));
        char *lc = bc(l), *rc = bc(r);
        strvec_push_own(out, xasprintf("%s%s%s%s", lc, text, pad, rc));
        free(lc);
        free(rc);
        free(l);
        free(r);
        free(p1);
        free(p2);
    } else if (!strcmp(st->id, "band")) {
        char *g = *sg ? bc(sg) : xstrdup("");
        strvec_push_own(out, xasprintf("%s%s%s", g, text, pad));
        free(g);
    } else if (!strcmp(st->id, "pi")) {
        strvec_push_own(out, xasprintf("%*s%s%s%s", chrome, "", sg, text, pad));
    } else if (!strcmp(st->id, "field") || !strcmp(st->id, "rail")) {
        bool field = !strcmp(st->id, "field");
        char *cap = tui_fgs(NULL, TUI_TC_ACCENT, field ? "\xe2\x96\x90" : "\xe2\x96\x8e");
        char *interior = field ? xasprintf("%*s%s%s%s%*s", padx, "", sg, text, pad, MAX(0, padx), "")
                               : xasprintf("%*s%s%s%s%*s", padx, "", sg, text, pad, MAX(0, padx + 1), "");
        char *fgd = tui_fgs(NULL, TUI_TC_USER_MESSAGE_TEXT, interior);
        char *surf = tui_bgs(NULL, TUI_TC_USER_MESSAGE_BG, fgd);
        char *rc = field ? tui_fgs(NULL, TUI_TC_ACCENT, "\xe2\x96\x8c") : xstrdup("");
        strvec_push_own(out, xasprintf("%s%s%s", cap, surf, rc));
        free(cap);
        free(interior);
        free(fgd);
        free(surf);
        free(rc);
    } else {
        strvec_push_own(out, xasprintf("%s%s%s", sg, text, pad));
    }
    /* bottom */
    if (!strcmp(st->id, "claude") || !strcmp(st->id, "pi")) {
        char *f = rep(H, pw);
        strvec_push_own(out, bc(f));
        free(f);
    }
    if (st->bottom_bar) {
        char *bar = ui_status_bottom_bar(u, pw, st->bottom_bar == 2, title);
        if (bar && *bar) {
            if (st->bar_gap) strvec_push(out, "");
            strvec_push_own(out, bar);
        } else {
            free(bar);
        }
    }
    free(top);
    free(prompt);
    free(row);
    free(text);
    free(pad);
    free(sg);
}
