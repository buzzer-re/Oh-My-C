/* setup.c - first-run setup wizard (port of omp packages/tui/src/setup: wizard-overlay.ts, scenes).
 *
 * A fullscreen overlay: animated splash (water + starfield under the 2x brand mark, 2.6s) →
 * dissolve (420ms) → five scenes (sign in, default model, glyph mode, composer shape, theme) →
 * "Setup saved" outro (1.2s). Choices persist to config.yml exactly like omp (modelRoles.default,
 * symbolPreset, composer.shape, theme.dark/theme.light, colorBlindMode) and `setupVersion: 2` is
 * written once the wizard completes (even when every scene was skipped). */
#include "ui/overlays.h"
#include "ui/modelbrowser.h"
#include "ui/ui.h"

#include "config/auth.h"
#include "config/settings.h"
#include "core/fs.h"
#include "tui/fuzzy.h"

#include <math.h>
#include <ctype.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>


#define SPLASH_MS 2600
#define TICK_MS 33
#define TRANSITION_MS 420
#define OUTRO_MS 1200
#define SCENE_MARGIN_X 4
#define MIN_CONTENT_WIDTH 20

typedef enum { PH_SPLASH, PH_TRANSITION, PH_SCENE, PH_OUTRO, PH_DONE } phase_t;

typedef struct scene scene_t;
typedef struct wizard wizard_t;

struct scene {
    wizard_t *w;
    const char *title;
    char *(*subtitle)(scene_t *s); /* owned, NULL = none */
    void (*render)(scene_t *s, int width, int max_lines, strvec_t *out);
    void (*key)(scene_t *s, const tui_key_t *k);
    void (*destroy)(scene_t *s);
};

typedef struct {
    const char *id, *title;
    int min_version;
    scene_t *(*mount)(wizard_t *w);
} scene_def_t;

struct wizard {
    tui_component_t base;
    ui_t *u;
    phase_t phase;
    int64_t phase_start;
    const scene_def_t *scenes[8];
    int nscenes, index;
    scene_t *active;
    int timer;
    void (*done)(void *ud);
    void *done_ud;
    bool no_mark; /* runProviderSetupWizard: markComplete false (setupVersion untouched) */
};

static wizard_t *g_wizard;

static int64_t elapsed(const wizard_t *w) { return mono_ms() - w->phase_start; }

static int term_rows(const wizard_t *w) {
    tui_t *t = w->u->tui;
    return t ? MAX(1, tui_term(t)->rows) : 24;
}

static void request_render(wizard_t *w) {
    if (w->u->tui) tui_request_render(w->u->tui);
}

static void scene_finish(wizard_t *w);

/* ---------- styling helpers ---------- */

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static char *bold_fg(tui_tc_t slot, const char *s) {
    char *a = S(slot, s);
    char *b = tui_bold(a);
    free(a);
    return b;
}

static char *indent_line(const char *line, int width, int indent) {
    int pre = MIN(indent, MAX(0, width - 1));
    char *l = xasprintf("%*s%s", pre, "", line);
    char *r = width > 0 ? ui_fit_line(l, width) : xstrdup("");
    free(l);
    return r;
}

static char *footer_hint(void) {
    char *up = ui_editor_key("tui.select.up"), *down = ui_editor_key("tui.select.down");
    char *conf = ui_editor_key("tui.select.confirm"), *canc = ui_editor_key("tui.select.cancel");
    char *cc = ui_key_hint("ctrl+c");
    char *r = xasprintf("%s/%s select \xc2\xb7 %s confirm \xc2\xb7 %s skip \xc2\xb7 %s exit setup", up, down, conf, canc, cc);
    free(up);
    free(down);
    free(conf);
    free(canc);
    free(cc);
    return r;
}

/* ---------- splash (scenes/splash.ts) ---------- */

#define LARGE_LOGO_W 24
#define LARGE_LOGO_H 10

/* 2x brand mark: every glyph doubled horizontally, every row doubled vertically (codepoints) */
static void large_logo(strvec_t *rows) {
    for (int y = 0; y < 5; y++) {
        sb_t b = SB_INIT;
        for (const char *p = UI_PI_LOGO[y]; *p;) {
            if (*p == ' ') {
                sb_append(&b, "  ");
                p++;
                continue;
            }
            size_t l = ((unsigned char)*p >= 0xE0) ? 3 : ((unsigned char)*p >= 0xC0) ? 2 : 1;
            sb_appendn(&b, p, l);
            sb_appendn(&b, p, l);
            p += l;
        }
        strvec_push(rows, sb_cstr(&b));
        strvec_push(rows, sb_cstr(&b));
        sb_free(&b);
    }
}

static char *skip_hint(void) {
    char *k = ui_key_hint("enter");
    char *r = xasprintf("press %s to skip", k);
    free(k);
    return r;
}

/* JS `(a*x) ^ (b*y) ^ ...` on doubles: each product ToInt32-wrapped */
static int32_t to_i32(int64_t v) { return (int32_t)(uint32_t)(uint64_t)v; }

static const char *sky_glyph(int x, int y, int frame, int *slot) {
    int32_t h = to_i32((int64_t)x * 73856093) ^ to_i32((int64_t)y * 19349663) ^ to_i32((int64_t)frame * 83492791);
    int64_t bucket = llabs((int64_t)h) % 150;
    if (bucket == 0) return *slot = TUI_TC_ACCENT, "\xe2\x9c\xa6";
    if (bucket == 1) return *slot = TUI_TC_BORDER, "\xe2\x9c\xa7";
    if (bucket == 2) return *slot = TUI_TC_BORDER, "\xc2\xb7";
    return NULL;
}

static const char *star_at(int x, int y, int frame, int *slot) {
    int32_t h = to_i32((int64_t)x * 73856093) ^ to_i32((int64_t)y * 19349663) ^ to_i32((int64_t)frame * 83492791);
    int64_t bucket = llabs((int64_t)h) % 97;
    if (bucket == 0) return *slot = TUI_TC_ACCENT, "\xe2\x9c\xa6";
    if (bucket == 1) return *slot = TUI_TC_MUTED, "\xc2\xb7";
    return NULL;
}

static double water_jitter(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (double)h / 4294967296.0;
}

static const char *water_char(double a) {
    if (a > 0.62) return "\xe2\x96\x88";
    if (a > 0.5) return "\xe2\x96\x93";
    if (a > 0.36) return "\xe2\x96\x92";
    if (a > 0.24) return "\xe2\x96\x91";
    return NULL;
}

static double screen_t(int x, int y, int w, int h, double phase) {
    double span = MAX(1, w + h - 1);
    double base = (x + (h - 1 - y)) / span;
    return fmod(fmod(base + phase, 1) + 1, 1);
}

static void render_compact_splash(int w, int h, double phase, double spos, double sstr, strvec_t *out) {
    strvec_t art = {0};
    if (h >= 14) large_logo(&art);
    else
        for (int i = 0; i < 5; i++) strvec_push(&art, UI_PI_LOGO[i]);
    strvec_t content = {0};
    ui_gradient_logo((const char *const *)art.v, art.n, phase, spos, sstr, &content);
    strvec_push(&content, "");
    strvec_push_own(&content, tui_bold("O h   M y   P i"));
    int start = MAX(0, (h - (int)content.n) / 2);
    for (int y = 0; y < h; y++) {
        int i = y - start;
        const char *item = i >= 0 && i < (int)content.n ? content.v[i] : NULL;
        char *c = item ? ui_center_line(item, w) : xstrdup("");
        strvec_push_own(out, w > 0 ? ui_fit_line(c, w) : xstrdup(""));
        free(c);
    }
    if (h > 2) {
        char *hint = skip_hint(), *d = S(TUI_TC_DIM, hint), *c = ui_center_line(d, w);
        free(out->v[out->n - 2]);
        out->v[out->n - 2] = w > 0 ? ui_fit_line(c, w) : xstrdup("");
        free(c);
        free(d);
        free(hint);
    }
    strvec_free(&content);
    strvec_free(&art);
}

static void render_splash(int width, int height, int64_t ms, strvec_t *out) {
    int w = MAX(1, width), h = MAX(1, height);
    double progress = MAX(0.0, MIN(1.0, (double)ms / SPLASH_MS));
    double phase = progress * 1.8;
    double spos = fmod(progress * 2.5, 1), sstr = MAX(0.0, 1 - progress * 0.35);
    if (w < 56 || h < 22) {
        render_compact_splash(w, h, phase, spos, sstr, out);
        return;
    }
    int frame = (int)(ms / TICK_MS);
    int cx = w / 2;
    double st = frame * 0.13;
    char **cells = xcalloc((size_t)w * h, sizeof *cells); /* NULL = space */
    char **diag = xcalloc((size_t)(w + h), sizeof *diag);
#define GRAD(x, y) (diag[(x) + (h - 1 - (y))] ? diag[(x) + (h - 1 - (y))] : (diag[(x) + (h - 1 - (y))] = ui_gradient_escape(screen_t(x, y, w, h, phase), spos, sstr)))
    int hx = (w - LARGE_LOGO_W) / 2;
    int hy = MAX(2, (int)floor(h * 0.16));
    int water_top = hy + LARGE_LOGO_H;
    int water_h = MAX(1, h - water_top);
    for (int y = water_top; y < h; y++) {
        int dy = y - water_top;
        double fade = MAX(0.0, 1 - ((double)dy / water_h) * 0.55);
        for (int x = 0; x < w; x++) {
            double edge = pow(MAX(0.0, 1 - fabs((double)(x - cx)) / (w * 0.5)), 0.7);
            double dx = (x - cx) / 2.0;
            double radial = sqrt(dx * dx + (double)dy * dy) * 0.55;
            double diagonal = x * 0.22 + y * 0.45;
            double cross = fabs(dx) * 0.8 + dy * 0.5;
            double jitter = (water_jitter(x, y) - 0.5) * 0.06;
            double wave = 0.5 * sin(radial - st) + 0.3 * sin(diagonal - st * 0.7) + 0.2 * sin(cross - st * 1.4);
            double level = 0.5 + 0.5 * wave;
            const char *ch = water_char(level * edge * fade + jitter);
            if (ch) cells[y * w + x] = xasprintf("%s%s%s", GRAD(x, y), ch, TUI_RESET);
        }
    }
    for (int y = 0; y < water_top - 1 && y < h; y++)
        for (int x = 0; x < w; x++) {
            int slot;
            const char *g = sky_glyph(x, y, frame >> 3, &slot);
            if (g) cells[y * w + x] = S((tui_tc_t)slot, g);
        }
    strvec_t logo = {0};
    large_logo(&logo);
    for (int row = 0; row < (int)logo.n; row++) {
        int col = 0;
        for (const char *p = logo.v[row]; *p; col++) {
            size_t l = ((unsigned char)*p >= 0xE0) ? 3 : ((unsigned char)*p >= 0xC0) ? 2 : 1;
            int x = hx + col, y = hy + row;
            if (*p != ' ' && y >= 0 && y < h && x >= 0 && x < w) {
                free(cells[y * w + x]);
                char *g = xstrndup(p, l);
                cells[y * w + x] = xasprintf("%s%s%s", GRAD(x, y), g, TUI_RESET);
                free(g);
            }
            p += l;
        }
    }
    strvec_free(&logo);
    char *hint = skip_hint();
    int hw = tui_visible_width(hint);
    int hs = (w - hw) / 2, hr = h - 1;
    for (int x = hs - 1; x <= hs + hw; x++)
        if (x >= 0 && x < w) {
            free(cells[hr * w + x]);
            cells[hr * w + x] = NULL;
        }
    int col = hs;
    for (const char *p = hint; *p; col++) {
        size_t l = ((unsigned char)*p >= 0xE0) ? 3 : ((unsigned char)*p >= 0xC0) ? 2 : 1;
        if (col >= 0 && col < w && *p != ' ') {
            char *g = xstrndup(p, l);
            free(cells[hr * w + col]);
            cells[hr * w + col] = S(TUI_TC_DIM, g);
            free(g);
        }
        p += l;
    }
    free(hint);
    for (int y = 0; y < h; y++) {
        sb_t b = SB_INIT;
        for (int x = 0; x < w; x++) {
            char *c = cells[y * w + x];
            sb_append(&b, c ? c : " ");
            free(c);
        }
        strvec_push_own(out, sb_detach(&b));
    }
    for (int i = 0; i < w + h; i++) free(diag[i]);
    free(diag);
    free(cells);
#undef GRAD
}

/* ---------- outro (scenes/outro.ts) ---------- */

static void render_outro(int width, int height, int64_t ms, strvec_t *out) {
    int frame = (int)(ms / TICK_MS) + 1000;
    strvec_t lines = {0};
    for (int y = 0; y < height; y++) {
        sb_t b = SB_INIT;
        for (int x = 0; x < width; x++) {
            int slot;
            const char *g = star_at(x, y, frame >> 3, &slot);
            if (g) tui_fg(&b, NULL, (tui_tc_t)slot, g);
            else sb_putc(&b, ' ');
        }
        strvec_push_own(&lines, sb_detach(&b));
    }
    double progress = MAX(0.0, MIN(1.0, (double)ms / OUTRO_MS));
    strvec_t content = {0};
    ui_gradient_logo(UI_PI_LOGO, 5, progress * 1.2, fmod(progress * 2, 1), 1 - progress, &content);
    strvec_push(&content, "");
    char *saved = xasprintf("%s Setup saved", tui_sym("status.success"));
    strvec_push_own(&content, bold_fg(TUI_TC_SUCCESS, saved));
    free(saved);
    strvec_push_own(&content, S(TUI_TC_MUTED, "Handing off to the normal CLI\xe2\x80\xa6"));
    strvec_push(&content, "");
    int sw = MAX(1, MIN(width - 8, (int)floor((width - 8) * progress)));
    sb_t sweep = SB_INIT, a = SB_INIT, d = SB_INIT;
    sb_repeat(&a, "\xe2\x94\x81", sw);
    sb_repeat(&d, "\xe2\x94\x80", MAX(0, width - 8 - sw));
    tui_fg(&sweep, NULL, TUI_TC_ACCENT, sb_cstr(&a));
    tui_fg(&sweep, NULL, TUI_TC_DIM, sb_cstr(&d));
    sb_free(&a);
    sb_free(&d);
    strvec_push_own(&content, sb_detach(&sweep));
    int start = MAX(0, (height - (int)content.n) / 2);
    for (int i = 0; i < (int)content.n && start + i < (int)lines.n; i++) {
        free(lines.v[start + i]);
        lines.v[start + i] = ui_center_line(content.v[i], width);
    }
    for (size_t i = 0; i < lines.n; i++) strvec_push_own(out, width > 0 ? ui_fit_line(lines.v[i], width) : xstrdup(""));
    strvec_free(&lines);
    strvec_free(&content);
}

/* ---------- wizard frame ---------- */

static void render_scene_frame(wizard_t *w, int width, int height, strvec_t *out) {
    const scene_def_t *def = w->index < w->nscenes ? w->scenes[w->index] : NULL;
    const char *title = w->active ? w->active->title : def ? def->title : "Setup";
    char *subtitle = w->active && w->active->subtitle ? w->active->subtitle(w->active) : NULL;
    int content_w = MAX(MIN_CONTENT_WIDTH, width - SCENE_MARGIN_X * 2);
    strvec_t lines = {0};
    strvec_push(&lines, "");
    strvec_t logo = {0};
    ui_gradient_logo(UI_PI_LOGO, 5, 0, 0, 0, &logo);
    for (size_t i = 0; i < logo.n; i++) strvec_push_own(&lines, ui_center_line(logo.v[i], width));
    strvec_free(&logo);
    char *app = bold_fg(TUI_TC_ACCENT, UI_APP_NAME);
    strvec_push_own(&lines, ui_center_line(app, width));
    free(app);
    char *step = xasprintf("Setup step %d of %d", w->index + 1, w->nscenes);
    char *steps = S(TUI_TC_MUTED, step);
    strvec_push_own(&lines, ui_center_line(steps, width));
    free(steps);
    free(step);
    strvec_push(&lines, "");
    char *bt = tui_bold(title);
    strvec_push_own(&lines, indent_line(bt, width, SCENE_MARGIN_X));
    free(bt);
    if (subtitle) {
        char *ms = S(TUI_TC_MUTED, subtitle);
        strvec_push_own(&lines, indent_line(ms, width, SCENE_MARGIN_X));
        free(ms);
        free(subtitle);
    }
    strvec_push(&lines, "");
    char *fh = footer_hint(), *fd = S(TUI_TC_DIM, fh);
    int max_body = MAX(0, height - (int)lines.n - 2);
    if (w->active) {
        strvec_t body = {0};
        w->active->render(w->active, content_w, max_body, &body);
        for (size_t i = 0; i < body.n && (int)i < max_body; i++) strvec_push_own(&lines, indent_line(body.v[i], width, SCENE_MARGIN_X));
        strvec_free(&body);
    }
    while ((int)lines.n + 2 < height) strvec_push(&lines, "");
    strvec_push(&lines, "");
    strvec_push_own(&lines, ui_center_line(fd, width));
    free(fd);
    free(fh);
    for (size_t i = 0; i < lines.n; i++) strvec_push(out, lines.v[i]);
    strvec_free(&lines);
}

static double row_noise(int y) {
    uint32_t h = (uint32_t)((int32_t)y ^ (int32_t)0x9e3779b9) * 2654435761u;
    return (double)((h ^ (h >> 15))) / 4294967296.0;
}

static void wizard_render(tui_component_t *c, int width, strvec_t *out) {
    wizard_t *w = (wizard_t *)c;
    int sw = MAX(1, width), h = term_rows(w);
    strvec_t lines = {0};
    switch (w->phase) {
    case PH_SPLASH: render_splash(sw, h, elapsed(w), &lines); break;
    case PH_TRANSITION: {
        double progress = MIN(1.0, (double)elapsed(w) / TRANSITION_MS);
        strvec_t a = {0}, b = {0};
        render_splash(sw, h, SPLASH_MS + elapsed(w), &a);
        render_scene_frame(w, sw, h, &b);
        double eased = progress * progress * (3 - 2 * progress);
        int denom = MAX(1, h - 1);
        for (int y = 0; y < h; y++) {
            double th = 0.78 * ((double)y / denom) + 0.22 * row_noise(y);
            const strvec_t *src = eased >= th ? &b : &a;
            strvec_push(&lines, y < (int)src->n ? src->v[y] : "");
        }
        strvec_free(&a);
        strvec_free(&b);
        break;
    }
    case PH_OUTRO: render_outro(sw, h, elapsed(w), &lines); break;
    case PH_SCENE: render_scene_frame(w, sw, h, &lines); break;
    case PH_DONE: break;
    }
    for (int y = 0; y < h; y++) strvec_push_own(out, ui_fit_line(y < (int)lines.n ? lines.v[y] : "", sw));
    strvec_free(&lines);
}

/* ---------- phases ---------- */

static void unmount_scene(wizard_t *w) {
    if (!w->active) return;
    scene_t *s = w->active;
    w->active = NULL;
    if (s->destroy) s->destroy(s);
}

static void complete(wizard_t *w);
static void tick(void *ud);

/* #startTimer: repaint during timed phases only (splash, transition, outro); a scene waiting for input needs none */
static void sync_timer(wizard_t *w) {
    bool timed = w->phase == PH_SPLASH || w->phase == PH_TRANSITION || w->phase == PH_OUTRO;
    if (!timed) {
        if (w->timer) tui_remove_timer(w->u->tui, w->timer);
        w->timer = 0;
    } else if (!w->timer) w->timer = tui_add_timer(w->u->tui, TICK_MS, true, tick, w);
}

static void begin_outro(wizard_t *w) {
    if (w->phase == PH_DONE) return;
    unmount_scene(w);
    w->phase = PH_OUTRO;
    w->phase_start = mono_ms();
    sync_timer(w);
    request_render(w);
}

static void mount_scene(wizard_t *w, phase_t target) {
    unmount_scene(w);
    if (w->index >= w->nscenes) {
        begin_outro(w);
        return;
    }
    w->phase = target;
    w->phase_start = mono_ms();
    w->active = w->scenes[w->index]->mount(w);
    sync_timer(w);
    request_render(w);
}

static void scene_finish(wizard_t *w) {
    if (w->phase != PH_SCENE && w->phase != PH_TRANSITION) return;
    unmount_scene(w);
    w->index++;
    mount_scene(w, PH_SCENE);
}

static void tick(void *ud) {
    wizard_t *w = ud;
    int64_t e = elapsed(w);
    if (w->phase == PH_SPLASH && e >= SPLASH_MS) mount_scene(w, PH_TRANSITION);
    else if (w->phase == PH_TRANSITION && e >= TRANSITION_MS) {
        w->phase = PH_SCENE;
        w->phase_start = mono_ms();
        sync_timer(w);
        request_render(w);
    } else if (w->phase == PH_OUTRO && e >= OUTRO_MS) complete(w);
    else if (w->phase != PH_SCENE) request_render(w);
}

static bool wizard_key(tui_component_t *c, const tui_key_t *k) {
    wizard_t *w = (wizard_t *)c;
    if (w->phase == PH_DONE) return true;
    if (k->kind == TUI_KEY_MOUSE || k->kind == TUI_KEY_RESPONSE) return true;
    if (tui_key_matches(k, "ctrl+c")) {
        begin_outro(w);
        return true;
    }
    bool go = tui_key_matches(k, "enter") || tui_key_matches(k, "space") || tui_key_matches(k, "escape");
    if (w->phase == PH_SPLASH) {
        if (go) mount_scene(w, PH_TRANSITION);
        return true;
    }
    if (w->phase == PH_OUTRO) {
        if (go) complete(w);
        return true;
    }
    if (w->active && w->active->key) w->active->key(w->active, k);
    request_render(w);
    return true;
}

static void wizard_destroy(tui_component_t *c) { (void)c; }

static const tui_component_vt WIZARD_VT = {"setup-wizard", wizard_render, wizard_key, NULL, wizard_destroy};

/* ---------- persistence ---------- */

static void save_str(const char *path, const char *v) { settings_set_global(path, json_str(v)); }

static void invalidate_ui(wizard_t *w) {
    tui_t *t = w->u->tui;
    if (!t) return;
    tui_component_invalidate(&tui_root(t)->base);
    tui_request_render(t);
}

/* ======================================================================================= */
/* scene: sign in (scenes/sign-in.ts, v18.14.2: provider panel, then a login dialog)        */
/* ======================================================================================= */

#define DIALOG_HEAD_ROWS 3

typedef struct {
    scene_t s;
    ui_oauth_sel_t *sel;
    ui_login_t *login;
    int outcome; /* -1 none, UI_LOGIN_SUCCESS / UI_LOGIN_ERROR */
    char *outcome_name, *outcome_msg;
    bool signed_in;
} signin_t;

static void signin_fit(void *ud, int budget) {
    signin_t *si = ud;
    if (budget >= 0) ui_oauth_sel_set_max_height(si->sel, budget);
}

static void signin_content(void *ud, int width, strvec_t *out) {
    signin_t *si = ud;
    ui_oauth_sel_render(si->sel, width, out);
}

static void signin_render(scene_t *s, int width, int max_lines, strvec_t *out) {
    signin_t *si = (signin_t *)s;
    if (si->login && !ui_login_active(si->login)) {
        ui_login_free(si->login);
        si->login = NULL;
    }
    if (si->login) {
        strvec_t lines = {0};
        ui_login_render(si->login, width, &lines);
        if ((int)lines.n <= max_lines) {
            for (size_t i = 0; i < lines.n; i++) strvec_push(out, lines.v[i]);
        } else {
            int head = max_lines >= DIALOG_HEAD_ROWS * 2 ? DIALOG_HEAD_ROWS : 0;
            for (int i = 0; i < head; i++) strvec_push(out, lines.v[i]);
            for (size_t i = lines.n - (size_t)(max_lines - head); i < lines.n; i++) strvec_push(out, lines.v[i]);
        }
        strvec_free(&lines);
        return;
    }
    strvec_t intro = {0}, status = {0};
    if (max_lines >= 17 + 2) {
        strvec_push_own(&intro, S(TUI_TC_MUTED, "Pick a provider to sign in \xe2\x80\x94 you can connect more than one."));
        strvec_push(&intro, "");
    }
    if (si->outcome == UI_LOGIN_SUCCESS) {
        char *a = xasprintf("%s Signed in to %s", tui_sym("status.success"), si->outcome_name);
        char *as = S(TUI_TC_SUCCESS, a);
        ui_push_text(&status, as, width);
        char *path = omc_agent_path("auth.json");
        char *b = xasprintf("Credentials saved to %s", path);
        char *bs = S(TUI_TC_DIM, b);
        ui_push_text(&status, bs, width);
        free(a);
        free(as);
        free(b);
        free(bs);
        free(path);
    } else if (si->outcome == UI_LOGIN_ERROR) {
        char *a = xasprintf("Login failed: %s", si->outcome_msg ? si->outcome_msg : "unknown error");
        char *as = S(TUI_TC_ERROR, a);
        ui_push_text(&status, as, width);
        char *k = ui_editor_key("tui.select.cancel");
        char *b = xasprintf("Choose another provider or press %s to continue.", k);
        char *bs = S(TUI_TC_DIM, b);
        ui_push_text(&status, bs, width);
        free(a);
        free(as);
        free(b);
        free(bs);
        free(k);
    }
    ui_wizard_step_t step = {0};
    step.gap = 0;
    step.max_height = max_lines;
    step.intro = &intro;
    step.status = &status;
    step.fit = signin_fit;
    step.content = signin_content;
    step.ud = si;
    ui_wizard_step_render(&step, width, out);
    strvec_free(&intro);
    strvec_free(&status);
}

static char *signin_subtitle(scene_t *s) {
    char *k = ui_editor_key("tui.select.cancel");
    char *r = xasprintf("Sign in to one or more providers. Press %s when you're done.", k);
    free(k);
    return r;
}

static void signin_done(void *ud, int outcome, const char *name, const char *err) {
    signin_t *si = ud;
    if (outcome != UI_LOGIN_CANCELLED) {
        si->outcome = outcome;
        free(si->outcome_name);
        free(si->outcome_msg);
        si->outcome_name = xstrdup(name ? name : "");
        si->outcome_msg = err ? xstrdup(err) : NULL;
        if (outcome == UI_LOGIN_SUCCESS) {
            si->signed_in = true;
            ui_oauth_sel_refresh(si->sel);
        }
    }
    /* the dialog closes; freed on the next key or render pass */
    request_render(si->s.w);
}

static void signin_key(scene_t *s, const tui_key_t *k) {
    signin_t *si = (signin_t *)s;
    if (si->login) {
        ui_login_key(si->login, k);
        if (!ui_login_active(si->login)) {
            ui_login_free(si->login);
            si->login = NULL;
        }
        return;
    }
    const char *picked = NULL;
    int r = ui_oauth_sel_key(si->sel, k, &picked);
    if (r == UI_SEL_CANCEL) {
        scene_finish(s->w);
    } else if (r == UI_SEL_PICK && picked) {
        si->outcome = -1;
        si->login = ui_login_start(s->w->u, picked, signin_done, si);
    }
}

static void signin_destroy(scene_t *s) {
    signin_t *si = (signin_t *)s;
    if (si->login) {
        ui_login_t *l = si->login;
        si->login = NULL;
        ui_login_free(l);
    }
    ui_oauth_sel_free(si->sel);
    free(si->outcome_name);
    free(si->outcome_msg);
    free(si);
}

static scene_t *signin_mount(wizard_t *w) {
    signin_t *si = xcalloc(1, sizeof *si);
    si->s.w = w;
    si->s.title = "Sign in to your providers";
    si->s.subtitle = signin_subtitle;
    si->s.render = signin_render;
    si->s.key = signin_key;
    si->s.destroy = signin_destroy;
    si->outcome = -1;
    si->sel = ui_oauth_sel_new(false);
    return &si->s;
}

/* ======================================================================================= */
/* scene: default model (scenes/model.ts)                                                   */
/* ======================================================================================= */

typedef struct {
    scene_t s;
    ui_model_browser_t *b;
    char *status;
    tui_tc_t status_color;
} model_scene_t;

static void model_fit(void *ud, int budget) {
    model_scene_t *m = ud;
    int visible = budget < 0 ? 10 : budget - 5;
    ui_model_browser_set_max_visible(m->b, MAX(1, MIN(10, visible)));
}

static void model_content(void *ud, int width, strvec_t *out) {
    model_scene_t *m = ud;
    ui_model_browser_render(m->b, width, out);
}

static void model_render(scene_t *s, int width, int max_lines, strvec_t *out) {
    model_scene_t *m = (model_scene_t *)s;
    strvec_t intro = {0};
    if (m->status) {
        char *t = S(m->status_color, m->status);
        ui_push_text(&intro, t, width);
        free(t);
    } else {
        char *k = ui_key_hint("enter");
        char *t = xasprintf("Type to search. %s saves the highlighted model as your default.", k);
        char *ts = S(TUI_TC_MUTED, t);
        ui_push_text(&intro, ts, width);
        free(ts);
        free(t);
        free(k);
    }
    ui_wizard_step_t step = {0};
    step.gap = -1;
    step.max_height = max_lines;
    step.intro = &intro;
    step.min_content = 1;
    step.fit = model_fit;
    step.content = model_content;
    step.ud = m;
    ui_wizard_step_render(&step, width, out);
    strvec_free(&intro);
}

static void model_key(scene_t *s, const tui_key_t *k) {
    model_scene_t *m = (model_scene_t *)s;
    int r = ui_model_browser_key(m->b, k);
    if (r == UI_MB_CANCEL) {
        scene_finish(s->w);
    } else if (r == UI_MB_ACTIVATE) {
        const ui_model_item_t *it = ui_model_browser_selected(m->b);
        if (!it) return;
        char *sel = xstrdup(it->selector);
        char *err = NULL;
        ui_t *u = s->w->u;
        if (app_set_model(u->app, sel, &err) != 0) {
            free(m->status);
            m->status = xstrdup(err ? err : "cannot select model");
            m->status_color = TUI_TC_ERROR;
            free(err);
            free(sel);
            return;
        }
        save_str("modelRoles.default", sel);
        free(sel);
        scene_finish(s->w);
    }
}

static void model_destroy(scene_t *s) {
    model_scene_t *m = (model_scene_t *)s;
    ui_model_browser_free(m->b);
    free(m->status);
    free(m);
}

static char *model_subtitle(scene_t *s) { return xstrdup("Search configured models and save the model used for new sessions."); }

static scene_t *model_mount(wizard_t *w) {
    model_scene_t *m = xcalloc(1, sizeof *m);
    m->s.w = w;
    m->s.title = "Choose your default model";
    m->s.subtitle = model_subtitle;
    m->s.render = model_render;
    m->s.key = model_key;
    m->s.destroy = model_destroy;
    m->b = ui_model_browser_new();
    ui_model_browser_load_available(m->b);
    m->b->single_pool = m->b->items.n == 1;
    char *cur = app_model_label(w->u->app);
    ui_model_browser_set_current(m->b, cur);
    free(cur);
    return &m->s;
}

/* ======================================================================================= */
/* scene: glyph mode (scenes/glyph.ts)                                                      */
/* ======================================================================================= */

static const char *const GLYPH_PRESETS[3] = {"nerd", "unicode", "ascii"};
static const char *const GLYPH_LABELS[3] = {"Nerd Font", "Unicode", "ASCII"};
static const char *const GLYPH_SAMPLES[3] = {
    "\xef\x80\x8c  \xee\x82\xb0  \xee\xb0\x99  \xf3\xb0\x89\x8b  \xee\x9c\xa5  \xe2\x95\xad\xe2\x94\x80\xe2\x95\xae  \xe2\x94\x9c\xe2\x94\x80  \xe2\x97\x86  \xe2\x9c\x94  \xe2\x9c\x96",
    "\xe2\x9c\x94  \xe2\x9c\x96  \xf0\x9f\x93\x81  \xe2\xac\xa2  \xe2\x95\xad\xe2\x94\x80\xe2\x95\xae  \xe2\x94\x9c\xe2\x94\x80  \xe2\x80\xa2  \xe2\xa0\x8b  \xe2\x86\x92",
    "[+]  [-]  >  +  [D]  +-+  |-  *  ->",
};

typedef struct {
    scene_t s;
    tui_select_list_t *list;
    int rows;
} list_scene_t;

static void list_fit(void *ud, int budget) {
    list_scene_t *l = ud;
    tui_select_list_set_max_visible(l->list, l->rows);
}

static void list_content(void *ud, int width, strvec_t *out) {
    list_scene_t *l = ud;
    tui_component_render(&l->list->base, width, out);
}

static void glyph_preview(wizard_t *w, const char *preset) {
    tui_sym_set_preset(preset);
    invalidate_ui(w);
}

static void glyph_change(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    list_scene_t *g = ud;
    glyph_preview(g->s.w, it->value);
}

static void glyph_select(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    list_scene_t *g = ud;
    char *v = xstrdup(it->value);
    save_str("symbolPreset", v);
    glyph_preview(g->s.w, v);
    free(v);
    scene_finish(g->s.w);
}

static void list_cancel(tui_select_list_t *l, void *ud) {
    list_scene_t *g = ud;
    scene_finish(g->s.w);
}

static void glyph_render(scene_t *s, int width, int max_lines, strvec_t *out) {
    list_scene_t *g = (list_scene_t *)s;
    strvec_t intro = {0};
    char *t = S(TUI_TC_MUTED, "If a row shows boxes, tofu, or misaligned icons, pick another.");
    ui_push_text(&intro, t, width);
    free(t);
    ui_wizard_step_t step = {0};
    step.gap = -1;
    step.max_height = max_lines;
    step.intro = &intro;
    step.min_content = 3;
    step.fit = list_fit;
    step.content = list_content;
    step.ud = g;
    ui_wizard_step_render(&step, width, out);
    strvec_free(&intro);
}

static void list_scene_key(scene_t *s, const tui_key_t *k, int quick_max, void (*preview_idx)(list_scene_t *, int)) {
    list_scene_t *g = (list_scene_t *)s;
    const char *txt = tui_key_text(k);
    if (k->kind == TUI_KEY_CHAR && txt && strlen(txt) == 1 && txt[0] >= '1' && txt[0] < '1' + quick_max) {
        int q = txt[0] - '1';
        tui_select_list_set_selected(g->list, (size_t)q);
        if (preview_idx) preview_idx(g, q);
        return;
    }
    tui_component_handle_key(&g->list->base, k);
}

static void glyph_quick(list_scene_t *g, int q) { glyph_preview(g->s.w, GLYPH_PRESETS[q]); }

static void glyph_key(scene_t *s, const tui_key_t *k) { list_scene_key(s, k, 3, glyph_quick); }

static void list_destroy(scene_t *s) {
    list_scene_t *g = (list_scene_t *)s;
    tui_component_destroy(&g->list->base);
    free(g);
}

static char *glyph_subtitle(scene_t *s) { return xstrdup("Pick the row that renders cleanly in your terminal."); }

static scene_t *glyph_mount(wizard_t *w) {
    list_scene_t *g = xcalloc(1, sizeof *g);
    g->s.w = w;
    g->s.title = "Choose glyph mode";
    g->s.subtitle = glyph_subtitle;
    g->s.render = glyph_render;
    g->s.key = glyph_key;
    g->s.destroy = list_destroy;
    g->rows = 3;
    g->list = tui_select_list_new(3);
    for (int i = 0; i < 3; i++) {
        char *label = xasprintf("%d  %s", i + 1, GLYPH_LABELS[i]);
        tui_select_list_add(g->list, GLYPH_PRESETS[i], label, GLYPH_SAMPLES[i]);
        free(label);
    }
    int cur = 1;
    for (int i = 0; i < 3; i++)
        if (str_eq(tui_sym_preset(), GLYPH_PRESETS[i])) cur = i;
    tui_select_list_set_selected(g->list, (size_t)cur);
    g->list->on_change = glyph_change;
    g->list->on_select = glyph_select;
    g->list->on_cancel = list_cancel;
    g->list->ud = g;
    return &g->s;
}

/* ======================================================================================= */
/* scene: composer shape (scenes/composer.ts)                                               */
/* ======================================================================================= */

typedef struct {
    list_scene_t l;
    char *shape;
} composer_scene_t;

static void composer_change(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    composer_scene_t *c = ud;
    free(c->shape);
    c->shape = xstrdup(it->value);
    request_render(c->l.s.w);
}

static void composer_select(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    composer_scene_t *c = ud;
    save_str("composer.shape", it->value);
    scene_finish(c->l.s.w);
}

static void composer_render(scene_t *s, int width, int max_lines, strvec_t *out) {
    composer_scene_t *c = (composer_scene_t *)s;
    strvec_t intro = {0}, preview = {0}, lines = {0};
    char *k = ui_editor_key("tui.select.confirm");
    char *t = xasprintf("Select a layout; live preview updates below. Press %s to confirm.", k);
    char *ts = S(TUI_TC_MUTED, t);
    ui_push_text(&intro, ts, width);
    free(ts);
    free(t);
    free(k);
    char *pv = S(TUI_TC_MUTED, "Preview:");
    ui_push_text(&preview, pv, width);
    free(pv);
    ui_composer_preview(s->w->u, c->shape, width, &lines);
    for (size_t i = 0; i < lines.n; i++) ui_push_text(&preview, lines.v[i], width);
    ui_wizard_step_t step = {0};
    step.gap = -1;
    step.max_height = max_lines;
    step.intro = &intro;
    step.preview = &preview;
    step.preview_optional = true;
    step.min_content = 8;
    step.fit = list_fit;
    step.content = list_content;
    step.ud = &c->l;
    ui_wizard_step_render(&step, width, out);
    strvec_free(&intro);
    strvec_free(&preview);
    strvec_free(&lines);
}

static void composer_quick(list_scene_t *l, int q) {
    composer_scene_t *c = (composer_scene_t *)l;
    free(c->shape);
    c->shape = xstrdup(UI_COMPOSER_SHAPES[q].value);
}

static void composer_key(scene_t *s, const tui_key_t *k) { list_scene_key(s, k, 8, composer_quick); }

static void composer_destroy(scene_t *s) {
    composer_scene_t *c = (composer_scene_t *)s;
    free(c->shape);
    tui_component_destroy(&c->l.list->base);
    free(c);
}

static char *composer_subtitle(scene_t *s) { return xstrdup("Pick the prompt and status line layout for your workflow."); }

static scene_t *composer_mount(wizard_t *w) {
    composer_scene_t *c = xcalloc(1, sizeof *c);
    c->l.s.w = w;
    c->l.s.title = "Choose composer shape";
    c->l.s.subtitle = composer_subtitle;
    c->l.s.render = composer_render;
    c->l.s.key = composer_key;
    c->l.s.destroy = composer_destroy;
    c->l.rows = 8;
    c->l.list = tui_select_list_new(8);
    const char *cfg = settings_str("composer.shape", "band");
    int cur = 0;
    for (int i = 0; i < 8; i++) {
        char *label = xasprintf("%d  %s", i + 1, UI_COMPOSER_SHAPES[i].label);
        tui_select_list_add(c->l.list, UI_COMPOSER_SHAPES[i].value, label, UI_COMPOSER_SHAPES[i].description);
        free(label);
        if (str_eq(cfg, UI_COMPOSER_SHAPES[i].value)) cur = i;
    }
    c->shape = xstrdup(UI_COMPOSER_SHAPES[cur].value);
    tui_select_list_set_selected(c->l.list, (size_t)cur);
    c->l.list->on_change = composer_change;
    c->l.list->on_select = composer_select;
    c->l.list->on_cancel = list_cancel;
    c->l.list->ud = c;
    return &c->l.s;
}

/* ======================================================================================= */
/* scene: theme (scenes/theme.ts)                                                           */
/* ======================================================================================= */

typedef struct {
    list_scene_t l;
    bool all;
    char *message;
    char *orig_theme;
    char *orig_preset;
    bool orig_cb;
} theme_scene_t;

static const struct {
    const char *value, *label, *desc;
} CURATED[6] = {
    {"auto", "Match terminal", "Titanium in dark terminals, Light in light terminals"},
    {"theme:titanium", "Titanium", "Default dark theme"},
    {"theme:light", "Light", "Default light theme"},
    {"colorblind", "Colorblind colors", "Adjust red/green contrast"},
    {"ansi", "ANSI-safe", "ASCII glyphs with the dark terminal theme"},
    {"browse", "Browse all\xe2\x80\xa6", "Show every built-in and custom theme"},
};

static void theme_change(tui_select_list_t *l, const tui_select_item_t *it, void *ud);
static void theme_select(tui_select_list_t *l, const tui_select_item_t *it, void *ud);
static void theme_cancel(tui_select_list_t *l, void *ud);

static int curated_index(void) {
    const char *cur = ui_theme_current_name();
    if (str_eq(cur, "titanium")) return 1;
    if (str_eq(cur, "light")) return 2;
    return 0;
}

static void theme_make_list(theme_scene_t *t, bool all) {
    if (t->l.list) tui_component_destroy(&t->l.list->base);
    t->all = all;
    if (!all) {
        t->l.list = tui_select_list_new(6);
        for (int i = 0; i < 6; i++) tui_select_list_add(t->l.list, CURATED[i].value, CURATED[i].label, CURATED[i].desc);
        tui_select_list_set_selected(t->l.list, (size_t)curated_index());
    } else {
        strvec_t names = {0};
        ui_theme_list(&names);
        t->l.list = tui_select_list_new(MIN(10, MAX(1, (int)names.n)));
        size_t sel = 0;
        for (size_t i = 0; i < names.n; i++) {
            char *v = xasprintf("theme:%s", names.v[i]);
            bool cur = str_eq(names.v[i], t->orig_theme);
            tui_select_list_add(t->l.list, v, names.v[i], cur ? "current" : NULL);
            if (cur) sel = i;
            free(v);
        }
        tui_select_list_set_selected(t->l.list, sel);
        strvec_free(&names);
    }
    t->l.list->on_change = theme_change;
    t->l.list->on_select = theme_select;
    t->l.list->on_cancel = theme_cancel;
    t->l.list->ud = t;
}

static void apply_presentation(const char *preset, bool cb) {
    tui_sym_set_preset(preset);
    ui_set_color_blind(cb);
}

static void theme_preview(theme_scene_t *t, const char *value) {
    free(t->message);
    t->message = NULL;
    if (!strcmp(value, "browse")) return;
    char *err = NULL;
    if (!strcmp(value, "auto")) {
        apply_presentation(t->orig_preset, t->orig_cb);
        ui_theme_auto();
    } else if (!strcmp(value, "colorblind")) {
        apply_presentation(t->orig_preset, true);
        ui_theme_preview(ui_theme_current_name(), NULL);
    } else if (!strcmp(value, "ansi")) {
        apply_presentation("ascii", t->orig_cb);
        if (!ui_theme_preview("dark-terminal", &err)) t->message = err ? err : xstrdup("Theme preview failed");
        else free(err);
    } else if (str_starts(value, "theme:")) {
        apply_presentation(t->orig_preset, t->orig_cb);
        if (!ui_theme_preview(value + 6, &err)) t->message = err ? err : xstrdup("Theme preview failed");
        else free(err);
    }
    invalidate_ui(t->l.s.w);
}

static void theme_change(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    theme_scene_t *t = ud;
    char *v = xstrdup(it->value);
    theme_preview(t, v);
    free(v);
}

static void theme_commit(theme_scene_t *t, const char *value) {
    if (!strcmp(value, "auto")) {
        save_str("theme.dark", "titanium");
        save_str("theme.light", "light");
        apply_presentation(t->orig_preset, t->orig_cb);
        ui_theme_auto();
    } else if (!strcmp(value, "colorblind")) {
        settings_set_global("colorBlindMode", json_bool(true));
        apply_presentation(t->orig_preset, true);
        ui_theme_preview(ui_theme_current_name(), NULL);
    } else if (!strcmp(value, "ansi")) {
        save_str("symbolPreset", "ascii");
        save_str("theme.dark", "dark-terminal");
        apply_presentation("ascii", t->orig_cb);
        ui_theme_auto();
    } else if (str_starts(value, "theme:")) {
        const char *name = value + 6;
        apply_presentation(t->orig_preset, t->orig_cb);
        save_str(ui_theme_is_light(name) ? "theme.light" : "theme.dark", name);
        ui_theme_preview(name, NULL);
    }
}

static void theme_select(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    theme_scene_t *t = ud;
    char *v = xstrdup(it->value);
    if (!strcmp(v, "browse")) {
        free(v);
        free(t->message);
        t->message = NULL;
        theme_make_list(t, true);
        request_render(t->l.s.w);
        return;
    }
    theme_commit(t, v);
    free(v);
    invalidate_ui(t->l.s.w);
    scene_finish(t->l.s.w);
}

static void theme_restore(theme_scene_t *t) {
    apply_presentation(t->orig_preset, t->orig_cb);
    if (t->orig_theme) ui_theme_preview(t->orig_theme, NULL);
    invalidate_ui(t->l.s.w);
}

static void theme_cancel(tui_select_list_t *l, void *ud) {
    theme_scene_t *t = ud;
    if (t->all) {
        theme_make_list(t, false);
        request_render(t->l.s.w);
        return;
    }
    theme_restore(t);
    scene_finish(t->l.s.w);
}

static void theme_fit(void *ud, int budget) {
    theme_scene_t *t = ud;
    int visible = budget < 0 ? 10 : budget - 1;
    tui_select_list_set_max_visible(t->l.list, MAX(1, MIN(10, visible)));
}

static char *fill_styled(const char *content, int width) {
    int w = tui_visible_width(content);
    return xasprintf("%s%*s", content, MAX(0, width - w), "");
}

static void theme_preview_lines(int width, strvec_t *out) {
    int pw = MAX(24, MIN(width, 88));
    strvec_push_own(out, tui_bold("Preview"));
    sb_t b = SB_INIT;
    char *a = xasprintf("%s success", tui_sym("status.success"));
    char *w = xasprintf("%s warning", tui_sym("status.warning"));
    char *e = xasprintf("%s error", tui_sym("status.error"));
    tui_fg(&b, NULL, TUI_TC_SUCCESS, a);
    sb_append(&b, "  ");
    tui_fg(&b, NULL, TUI_TC_WARNING, w);
    sb_append(&b, "  ");
    tui_fg(&b, NULL, TUI_TC_ERROR, e);
    sb_append(&b, "  ");
    tui_fg(&b, NULL, TUI_TC_ACCENT, "accent");
    strvec_push_own(out, sb_detach(&b));
    free(a);
    free(w);
    free(e);
    strvec_push(out, "");
    strvec_push_own(out, S(TUI_TC_MUTED, "Status line"));
    /* mock status line */
    char *sepr = xasprintf(" %s ", tui_sym("sep.pipe"));
    char *sep = S(TUI_TC_STATUS_LINE_SEP, sepr);
    char *m1 = xasprintf("%s sonnet", tui_sym("icon.model"));
    char *g1 = xasprintf("%s main +2", tui_sym("icon.git"));
    char *c1 = xasprintf("%s 42%%", tui_sym("icon.context"));
    char *k1 = xasprintf("%s 0.18", tui_sym("icon.cost"));
    char *sm = S(TUI_TC_STATUS_LINE_MODEL, m1), *sp = S(TUI_TC_STATUS_LINE_PATH, "~/project"),
         *sg = S(TUI_TC_STATUS_LINE_GIT_DIRTY, g1), *sc = S(TUI_TC_STATUS_LINE_CONTEXT, c1),
         *sk = S(TUI_TC_STATUS_LINE_COST, k1);
    char *left = xasprintf("%s%s%s%s%s", sm, sep, sp, sep, sg);
    char *right = xasprintf("%s%s%s", sc, sep, sk);
    int inner = MAX(1, pw - 2);
    int gap = MAX(1, inner - tui_visible_width(left) - tui_visible_width(right) - 2);
    char *bar = xasprintf(" %s%*s%s ", left, gap, "", right);
    char *fit = ui_fit_line(bar, pw);
    strvec_push_own(out, tui_bgs(NULL, TUI_TC_STATUS_LINE_BG, fit));
    free(fit);
    free(bar);
    free(left);
    free(right);
    free(sm);
    free(sp);
    free(sg);
    free(sc);
    free(sk);
    free(m1);
    free(g1);
    free(c1);
    free(k1);
    free(sep);
    free(sepr);
    strvec_push_own(out, S(TUI_TC_MUTED, "Editor"));
    /* mock editor */
    const char *hz = tui_sym("boxRound.horizontal");
    sb_t hb = SB_INIT;
    sb_repeat(&hb, hz, inner);
    char *top = xasprintf("%s%s%s", tui_sym("boxRound.topLeft"), sb_cstr(&hb), tui_sym("boxRound.topRight"));
    char *bot = xasprintf("%s%s%s", tui_sym("boxRound.bottomLeft"), sb_cstr(&hb), tui_sym("boxRound.bottomRight"));
    sb_free(&hb);
    strvec_push_own(out, S(TUI_TC_BORDER_ACCENT, top));
    char *gt = S(TUI_TC_ACCENT, ">"), *tx = S(TUI_TC_TEXT, "Ask anything, edit files, run tools");
    char *prompt = xasprintf("%s %s\x1b[7m \x1b[27m", gt, tx);
    char *pf = ui_fit_line(prompt, inner);
    char *vb = S(TUI_TC_BORDER_ACCENT, tui_sym("boxRound.vertical"));
    strvec_push_own(out, xasprintf("%s%s%s", vb, pf, vb));
    char *sk2 = ui_editor_key("tui.input.submit"), *nl = ui_editor_key("tui.input.newLine");
    char *hint = xasprintf("%s send \xc2\xb7 %s newline \xc2\xb7 / commands", sk2, nl);
    char *hd = S(TUI_TC_DIM, hint);
    char *hf = fill_styled(hd, inner);
    char *vm = S(TUI_TC_BORDER_MUTED, tui_sym("boxRound.vertical"));
    strvec_push_own(out, xasprintf("%s%s%s", vm, hf, vm));
    strvec_push_own(out, S(TUI_TC_BORDER_MUTED, bot));
    free(vm);
    free(hf);
    free(hd);
    free(hint);
    free(sk2);
    free(nl);
    free(vb);
    free(pf);
    free(prompt);
    free(gt);
    free(tx);
    free(top);
    free(bot);
}

static void theme_render(scene_t *s, int width, int max_lines, strvec_t *out) {
    theme_scene_t *t = (theme_scene_t *)s;
    strvec_t intro = {0}, preview = {0}, status = {0}, raw = {0};
    char *conf = ui_editor_key("tui.select.confirm"), *canc = ui_editor_key("tui.select.cancel");
    char *i1 = xasprintf("Theme changes preview live. Nothing is saved until you press %s.", conf);
    char *i1s = S(TUI_TC_MUTED, i1);
    ui_push_text(&intro, i1s, width);
    char *i2 = t->all ? xasprintf("Browsing all themes \xc2\xb7 %s returns to curated choices", canc)
                      : xasprintf("%s skips this step", canc);
    char *i2s = S(TUI_TC_DIM, i2);
    ui_push_text(&intro, i2s, width);
    free(i1);
    free(i1s);
    free(i2);
    free(i2s);
    free(conf);
    free(canc);
    theme_preview_lines(width, &raw);
    for (size_t i = 0; i < raw.n; i++) ui_push_text(&preview, raw.v[i], width);
    if (t->message) {
        char *m = S(TUI_TC_ERROR, t->message);
        ui_push_text(&status, m, width);
        free(m);
    }
    ui_wizard_step_t step = {0};
    step.gap = -1;
    step.max_height = max_lines;
    step.intro = &intro;
    step.preview = &preview;
    step.preview_optional = true;
    step.status = &status;
    step.min_content = 7;
    step.fit = theme_fit;
    step.content = list_content;
    step.ud = t;
    ui_wizard_step_render(&step, width, out);
    strvec_free(&intro);
    strvec_free(&preview);
    strvec_free(&status);
    strvec_free(&raw);
}

static void theme_key(scene_t *s, const tui_key_t *k) {
    theme_scene_t *t = (theme_scene_t *)s;
    const char *txt = tui_key_text(k);
    if (k->kind == TUI_KEY_CHAR && txt && strlen(txt) == 1 && txt[0] >= '1' && txt[0] <= '9') {
        int q = txt[0] - '1';
        tui_select_list_set_selected(t->l.list, (size_t)q);
        if (!t->all && q < 6) theme_preview(t, CURATED[q].value);
        return;
    }
    tui_component_handle_key(&t->l.list->base, k);
}

static void theme_destroy(scene_t *s) {
    theme_scene_t *t = (theme_scene_t *)s;
    tui_component_destroy(&t->l.list->base);
    free(t->message);
    free(t->orig_theme);
    free(t->orig_preset);
    free(t);
}

static char *theme_subtitle(scene_t *s) {
    char *k = ui_editor_key("tui.select.confirm");
    char *r = xasprintf("Move through the list to preview; %s saves the highlighted choice.", k);
    free(k);
    return r;
}

static scene_t *theme_mount(wizard_t *w) {
    theme_scene_t *t = xcalloc(1, sizeof *t);
    t->l.s.w = w;
    t->l.s.title = "Pick a theme";
    t->l.s.subtitle = theme_subtitle;
    t->l.s.render = theme_render;
    t->l.s.key = theme_key;
    t->l.s.destroy = theme_destroy;
    t->orig_theme = ui_theme_current_name() ? xstrdup(ui_theme_current_name()) : NULL;
    t->orig_preset = xstrdup(settings_str("symbolPreset", tui_sym_preset()));
    t->orig_cb = settings_bool("colorBlindMode", false);
    theme_make_list(t, false);
    return &t->l.s;
}

/* ---------- scene table / entry points ---------- */

static const scene_def_t SCENES[5] = {
    {"providers", "Sign in to your providers", 1, signin_mount},
    {"model", "Choose your default model", 1, model_mount},
    {"glyph-mode", "Choose glyph mode", 1, glyph_mount},
    {"composer-shape", "Choose composer shape", 2, composer_mount},
    {"theme", "Pick a theme", 1, theme_mount},
};

static bool env_skip(const char *name) {
    const char *v = getenv(name);
    if (!v) return false;
    char *t = str_trim(v), *l = str_lower(t);
    bool on = *l && strcmp(l, "0") && strcmp(l, "false") && strcmp(l, "no");
    free(t);
    free(l);
    return on;
}

int ui_setup_pending(bool resuming) {
    if (!isatty(0) || !isatty(1)) return 0;
    if (resuming) return 0;
    if (env_skip("OMP_SKIP_SETUP") || env_skip("OMC_SKIP_SETUP")) return 0;
    if (!settings_bool("startup.setupWizard", true)) return 0;
    int stored = (int)settings_int("setupVersion", 0);
    int n = 0;
    for (size_t i = 0; i < ARRAY_LEN(SCENES); i++)
        if (SCENES[i].min_version > stored) n++;
    return n;
}

bool ui_setup_active(void) { return g_wizard != NULL; }

static void complete(wizard_t *w) {
    if (w->phase == PH_DONE) return;
    w->phase = PH_DONE;
    if (w->timer) tui_remove_timer(w->u->tui, w->timer);
    w->timer = 0;
    unmount_scene(w);
    if (!w->no_mark) settings_set_global("setupVersion", json_int(UI_SETUP_VERSION));
    tui_t *t = w->u->tui;
    tui_overlay_hide(t, &w->base);
    tui_alt_screen(t, false);
    g_wizard = NULL;
    void (*done)(void *) = w->done;
    void *ud = w->done_ud;
    free(w);
    if (done) done(ud);
}

static void setup_start(ui_t *u, bool providers_only, void (*done)(void *ud), void *ud) {
    wizard_t *w = xcalloc(1, sizeof *w);
    tui_component_init(&w->base, &WIZARD_VT);
    w->u = u;
    w->done = done;
    w->done_ud = ud;
    w->no_mark = providers_only;
    int stored = (int)settings_int("setupVersion", 0);
    for (size_t i = 0; i < ARRAY_LEN(SCENES); i++)
        if (providers_only ? str_eq(SCENES[i].id, "providers") : SCENES[i].min_version > stored)
            w->scenes[w->nscenes++] = &SCENES[i];
    w->phase = w->nscenes ? PH_SPLASH : PH_OUTRO;
    w->phase_start = mono_ms();
    g_wizard = w;
    tui_alt_screen(u->tui, true);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &w->base, &o);
    sync_timer(w);
}

void ui_setup_run(ui_t *u, void (*done)(void *ud), void *ud) { setup_start(u, false, done, ud); }

void ui_setup_run_providers(ui_t *u) {
    if (g_wizard) return;
    setup_start(u, true, NULL, NULL);
}
