/* welcome.c - welcome banner (port of omp packages/tui/src/prompt/welcome.ts): the gradient π
 * logo beside the half-block `omc` wordmark with the version under it, and the tip of the
 * session centered below. A 3 s intro sweeps the gradient (with a fading shine) before the
 * banner settles on its resting frame. */
#include "ui/ui.h"

#include "core/embed.h"
#include "tui/color.h"

#include <ctype.h>

#include <math.h>

static const char *const PI_LOGO[5] = {
    "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88",
    "   \xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88   ",
    "   \xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88   ",
    "   \xe2\x96\x92\xe2\x96\x92  \xe2\x96\x88\xe2\x96\x88   ",
    "       \xe2\x96\x88\xe2\x96\x88   ",
};
#define LOGO_WIDTH 12

/* `omc` in the same half-block style as omp's `omp` wordmark (▄▀▀▄ █▀▄▀▄ █▀▀▄); without the
 * `p` descender the third row is empty but keeps its slot so the version stays put. */
static const char *const WORDMARK[3] = {
    "\xe2\x96\x84\xe2\x96\x80\xe2\x96\x80\xe2\x96\x84 \xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x80\xe2\x96\x84 \xe2\x96\x84\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80",
    "\xe2\x96\x80\xe2\x96\x84\xe2\x96\x84\xe2\x96\x80 \xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x80\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84",
    "",
};
#define WORDMARK_WIDTH 15
#define LOCKUP_GAP 4
#define TIP_MEASURE 72
#define TIP_MIN_COLUMNS 50
#define INTRO_MS 3000
#define INTRO_TICK_MS 33

typedef struct {
    tui_component_t base;
    int64_t anim_start; /* 0 = settled */
    int timer;
    tui_t *tui;
    double nag_roll, tip_roll;
    bool rolled;
    char *tip_override; /* tests */
    int upd_phase;      /* VersionUpdate after the version label: UI_UPD_* (0 = none) */
    char *upd_version;
    double upd_fraction; /* < 0: bytes not counted yet */
    int cache_w;
    strvec_t cache;
} welcome_t;

/* ---------- tips ---------- */

static strvec_t g_tips;
static bool g_tips_loaded;

static void load_tips(void) {
    if (g_tips_loaded) return;
    g_tips_loaded = true;
    size_t n = 0;
    const char *d = embed_get("assets/tui/tips.txt", &n);
    if (!d) return;
    char *all = xstrndup(d, n);
    for (char *line = strtok(all, "\n"); line; line = strtok(NULL, "\n")) {
        char *t = str_trim(line);
        if (*t) {
            const char *body = t;
            if (str_starts(body, "[NO-TERN]")) { /* classic renderer: marker stripped, tip kept */
                body += 9;
                while (*body == ' ' || *body == '\t') body++;
            }
            strvec_push(&g_tips, body);
        }
        free(t);
    }
    free(all);
}

static bool is_new_tip(const char *t) {
    size_t n = strlen(t);
    while (n && isspace((unsigned char)t[n - 1])) n--;
    return n >= 5 && !strncmp(t + n - 5, "[NEW]", 5);
}

/* pickWeightedTip: "[NEW]" tips weigh 4, others 1 */
static const char *pick_tip(double r) {
    load_tips();
    if (!g_tips.n) return NULL;
    double total = 0;
    for (size_t i = 0; i < g_tips.n; i++) total += is_new_tip(g_tips.v[i]) ? 4 : 1;
    double acc = r * total;
    for (size_t i = 0; i < g_tips.n; i++) {
        acc -= is_new_tip(g_tips.v[i]) ? 4 : 1;
        if (acc < 0) return g_tips.v[i];
    }
    return g_tips.v[g_tips.n - 1];
}

static const char *welcome_tip(welcome_t *w) {
    if (w->tip_override) return w->tip_override;
    if (!w->rolled) {
        w->rolled = true;
        uint32_t r[2];
        rand_bytes(r, sizeof r);
        w->nag_roll = (double)r[0] / 4294967296.0;
        w->tip_roll = (double)r[1] / 4294967296.0;
    }
    if (str_eq(tui_sym_preset(), "unicode") && w->nag_roll < 0.1) return "Please use nerdfont \xf0\x9f\x98\xad.";
    return pick_tip(w->tip_roll);
}

static bool key_name_ok(const char *k) {
    char *dup = xstrdup(k);
    bool ok = true;
    strvec_t parts = {0};
    for (char *p = dup, *q; p; p = q) {
        q = strchr(p, '+');
        if (q) *q++ = 0;
        strvec_push(&parts, p);
    }
    for (size_t i = 0; i < parts.n; i++) {
        if (!*parts.v[i]) ok = false;
        if (i + 1 < parts.n && !str_eq(parts.v[i], "ctrl") && !str_eq(parts.v[i], "shift") && !str_eq(parts.v[i], "alt") &&
            !str_eq(parts.v[i], "super"))
            ok = false;
    }
    strvec_free(&parts);
    free(dup);
    return ok;
}

/* {key:x} / {keys:a,b} / {tap:x}; malformed or {action:...} placeholders stay verbatim */
static char *expand_tip_keys(const char *tip) {
    sb_t b = SB_INIT;
    const char *p = tip;
    while (*p) {
        const char *open = strchr(p, '{');
        if (!open) break;
        const char *close = strchr(open, '}');
        const char *colon = open ? strchr(open, ':') : NULL;
        if (!close || !colon || colon > close) {
            sb_appendn(&b, p, (size_t)(open - p + 1));
            p = open + 1;
            continue;
        }
        char *kind = xstrndup(open + 1, (size_t)(colon - open - 1));
        char *val = xstrndup(colon + 1, (size_t)(close - colon - 1));
        char *rep = NULL;
        if (str_eq(kind, "key") || str_eq(kind, "keys") || str_eq(kind, "tap")) {
            strvec_t keys = {0};
            char *d = xstrdup(val);
            for (char *k = strtok(d, ","); k; k = strtok(NULL, ",")) strvec_push(&keys, k);
            free(d);
            bool ok = keys.n > 0;
            for (size_t i = 0; i < keys.n; i++)
                if (!key_name_ok(keys.v[i])) ok = false;
            if (ok) {
                if (str_eq(kind, "keys")) {
                    sb_t kb = SB_INIT;
                    for (size_t i = 0; i < keys.n; i++) {
                        char *h = ui_key_hint(keys.v[i]);
                        if (i) sb_putc(&kb, '/');
                        sb_append(&kb, h);
                        free(h);
                    }
                    rep = sb_detach(&kb);
                } else if (str_eq(kind, "tap")) {
                    rep = ui_double_tap_hint(keys.v[0]);
                } else {
                    rep = ui_key_hint(keys.v[0]);
                }
            }
            strvec_free(&keys);
        }
        sb_appendn(&b, p, (size_t)(open - p));
        if (rep) sb_append(&b, rep);
        else sb_appendn(&b, open, (size_t)(close - open + 1));
        free(rep), free(kind), free(val);
        p = close + 1;
    }
    sb_append(&b, p);
    return sb_detach(&b);
}

/* markdown links `[label](url)` become `label (url)` (no OSC 8 hyperlinks) */
static char *link_tip_text(const char *tip) {
    sb_t b = SB_INIT;
    const char *p = tip;
    while (*p) {
        const char *lb = strchr(p, '[');
        if (!lb) break;
        const char *rb = strchr(lb, ']');
        if (!rb || rb[1] != '(') {
            sb_appendn(&b, p, (size_t)(lb - p + 1));
            p = lb + 1;
            continue;
        }
        const char *rp = strchr(rb + 2, ')');
        bool ok = rp && rp > rb + 2 && rb > lb + 1;
        for (const char *q = rb + 2; ok && q < rp; q++)
            if (isspace((unsigned char)*q)) ok = false;
        if (!ok) {
            sb_appendn(&b, p, (size_t)(lb - p + 1));
            p = lb + 1;
            continue;
        }
        sb_appendn(&b, p, (size_t)(lb - p));
        sb_appendn(&b, lb + 1, (size_t)(rb - lb - 1));
        sb_append(&b, " (");
        sb_appendn(&b, rb + 2, (size_t)(rp - rb - 2));
        sb_putc(&b, ')');
        p = rp + 1;
    }
    sb_append(&b, p);
    return sb_detach(&b);
}

static void hsl_rgb(double h, double s, double l, int *r, int *g, int *b) {
    double a = s * MIN(l, 1 - l);
    double ch[3];
    const int ns[3] = {0, 8, 4};
    for (int i = 0; i < 3; i++) {
        double k = fmod(ns[i] + h / 30, 12);
        ch[i] = l - a * MAX(-1.0, MIN(MIN(k - 3, 9 - k), 1.0));
    }
    *r = (int)floor(ch[0] * 255 + 0.5), *g = (int)floor(ch[1] * 255 + 0.5), *b = (int)floor(ch[2] * 255 + 0.5);
}

static char *new_tag(double phase) {
    const char *text = "NEW!";
    double wrapped = fmod(fmod(phase, 1) + 1, 1);
    sb_t b = SB_INIT;
    sb_append(&b, "\x1b[1m");
    for (int i = 0; i < 4; i++) {
        int hue = (int)floor(fmod((double)i / 4 + wrapped, 1) * 360 + 0.5);
        int r, g, bl;
        hsl_rgb(hue, 0.95, 0.6, &r, &g, &bl);
        char esc[32];
        sb_append(&b, tui_rgb_fg(TH(), (uint32_t)(r << 16 | g << 8 | bl), esc, sizeof esc));
        sb_putc(&b, text[i]);
    }
    sb_append(&b, "\x1b[0m");
    return sb_detach(&b);
}

/* renderWelcomeTip: `Tip:` and the body wrapped together to `width`, unindented */
void ui_welcome_tip_lines(const char *tip, int width, double phase, strvec_t *out) {
    const char *label = "Tip: ";
    if (width - 5 < 8) return;
    bool isnew = is_new_tip(tip);
    char *t = xstrdup(tip);
    if (isnew) {
        size_t n = strlen(t);
        while (n && isspace((unsigned char)t[n - 1])) n--;
        n -= 5;
        while (n && isspace((unsigned char)t[n - 1])) n--;
        t[n] = 0;
    }
    char *ex = expand_tip_keys(t);
    char *body = link_tip_text(ex);
    char *full = xasprintf("%s%s", label, body);
    char *tabs = tui_replace_tabs(full);
    strvec_t wrapped = {0};
    tui_wrap_text_with_ansi(tabs, width, &wrapped);
    size_t start = out->n;
    for (size_t i = 0; i < wrapped.n; i++) {
        char *line = wrapped.v[i];
        size_t n = strlen(line);
        while (n && line[n - 1] == ' ') line[--n] = 0;
        char *styled;
        if (i == 0 && str_starts(line, label)) {
            char *lab = fg(TUI_TC_CUSTOM_MESSAGE_LABEL, label);
            char *rest = fg(TUI_TC_MUTED, line + 5);
            styled = xasprintf("\x1b[3m%s%s\x1b[23m", lab, rest);
            free(lab), free(rest);
        } else {
            char *rest = fg(TUI_TC_MUTED, line);
            styled = xasprintf("\x1b[3m%s\x1b[23m", rest);
            free(rest);
        }
        strvec_push_own(out, styled);
    }
    if (isnew && out->n > start) {
        char *tag = new_tag(phase);
        char *last = out->v[out->n - 1];
        if (tui_visible_width(last) + 5 <= width) {
            out->v[out->n - 1] = xasprintf("%s %s", last, tag);
            free(last);
            free(tag);
        } else {
            strvec_push_own(out, tag);
        }
    }
    strvec_free(&wrapped);
    free(t), free(ex), free(body), free(full), free(tabs);
}

/* ---------- gradient logo ---------- */

static const int STOPS[3][3] = {{248, 79, 204}, {147, 98, 244}, {0, 219, 228}};
static const int RAMP256[7] = {206, 170, 134, 99, 69, 74, 44};

static void gradient_escape(sb_t *b, double t, double shine_strength, double shine_pos) {
    if (TH()->mode == TUI_COLOR_TRUECOLOR) {
        double seg = t * 2;
        int i = MIN(1, (int)floor(seg));
        double f = seg - i;
        double r = STOPS[i][0] + (STOPS[i + 1][0] - STOPS[i][0]) * f;
        double g = STOPS[i][1] + (STOPS[i + 1][1] - STOPS[i][1]) * f;
        double bl = STOPS[i][2] + (STOPS[i + 1][2] - STOPS[i][2]) * f;
        if (shine_strength > 0) {
            double in = MAX(0.0, 1 - fabs(t - shine_pos) / 0.18) * shine_strength;
            if (in > 0) {
                r += (255 - r) * in;
                g += (255 - g) * in;
                bl += (255 - bl) * in;
            }
        }
        sb_appendf(b, "\x1b[38;2;%d;%d;%dm", (int)floor(r + 0.5), (int)floor(g + 0.5), (int)floor(bl + 0.5));
        return;
    }
    int idx = MIN(6, MAX(0, (int)floor(t * 6 + 0.5)));
    if (shine_strength > 0 && MAX(0.0, 1 - fabs(t - shine_pos) / 0.18) * shine_strength > 0.5) idx = 6;
    sb_appendf(b, "\x1b[38;5;%dm", RAMP256[idx]);
}

static void gradient_logo(double phase, double shine_strength, double shine_pos, strvec_t *out) {
    double np = fmod(fmod(phase, 1) + 1, 1);
    for (int y = 0; y < 5; y++) {
        sb_t b = SB_INIT;
        const char *p = PI_LOGO[y];
        int x = 0;
        while (*p) {
            uint32_t cp;
            int k = utf8_decode(p, strlen(p), &cp);
            if (cp == ' ') {
                sb_putc(&b, ' ');
            } else {
                double base = ((double)x / 11 + (double)y / 4) / 2;
                double t = np == 0 ? base : fmod(base + np, 1);
                gradient_escape(&b, t, shine_strength, shine_pos);
                sb_appendn(&b, p, (size_t)k);
                sb_append(&b, "\x1b[0m");
            }
            p += k;
            x++;
        }
        strvec_push_own(out, sb_detach(&b));
    }
}

static void logo_frame(welcome_t *w, strvec_t *out) {
    if (!w->anim_start) {
        gradient_logo(0, 0, 0, out);
        return;
    }
    double el = (double)(mono_ms() - w->anim_start);
    if (el >= INTRO_MS) {
        gradient_logo(0, 0, 0, out);
        return;
    }
    double p = el / INTRO_MS;
    double eased = 1 - pow(1 - p, 3);
    double phase = fmod(fmod((1 - eased) * 2.5, 1) + 1, 1);
    double pos = fmod(fmod(p * 3, 1) + 1, 1);
    gradient_logo(phase, pow(1 - eased, 1.5), pos, out);
}

/* ---------- background auto-update indicator (welcome.ts versionUpdateLabels / appendVersionUpdate) ---------- */

#define UPDATE_BAR_WIDTH 10

/* terminal forms, widest first: `→ v<new>` with a progress bar and percent while downloading (`updating…` before
 * bytes are counted), `✔ next launch` once ready, `⚠ update failed` when stopped; then the arrow and version with
 * the bare percent */
static void update_labels(const welcome_t *w, strvec_t *out) {
    tui_tc_t tone = w->upd_phase == UI_UPD_READY ? TUI_TC_SUCCESS : w->upd_phase == UI_UPD_FAILED ? TUI_TC_WARNING : TUI_TC_ACCENT;
    char *arrow = xasprintf("\xe2\x86\x92 v%s", w->upd_version), *head = fg(tone, arrow);
    free(arrow);
    char *detail, *compact = xstrdup(head);
    if (w->upd_phase == UI_UPD_UPDATING) {
        if (w->upd_fraction < 0) {
            detail = fg(TUI_TC_MUTED, "updating\xe2\x80\xa6");
        } else {
            double f = MIN(1.0, MAX(0.0, w->upd_fraction));
            char *pct_raw = xasprintf("%d%%", (int)floor(f * 100)), *pct = fg(TUI_TC_ACCENT, pct_raw);
            int filled = (int)floor(f * UPDATE_BAR_WIDTH + 0.5);
            sb_t a = SB_INIT, e = SB_INIT;
            for (int i = 0; i < filled; i++) sb_append(&a, tui_sym("progress.filled"));
            for (int i = filled; i < UPDATE_BAR_WIDTH; i++) sb_append(&e, tui_sym("progress.empty"));
            char *fa = fg(TUI_TC_ACCENT, sb_cstr(&a)), *ea = fg(TUI_TC_DIM, sb_cstr(&e));
            detail = xasprintf("%s%s %s", fa, ea, pct);
            free(compact);
            compact = xasprintf("%s %s", head, pct);
            free(fa);
            free(ea);
            sb_free(&a);
            sb_free(&e);
            free(pct_raw);
            free(pct);
        }
    } else {
        bool ready = w->upd_phase == UI_UPD_READY;
        char *t = xasprintf("%s %s", tui_sym(ready ? "status.success" : "status.warning"), ready ? "next launch" : "update failed");
        detail = fg(tone, t);
        free(t);
    }
    strvec_push_own(out, xasprintf("%s %s", head, detail));
    strvec_push_own(out, compact);
    free(head);
    free(detail);
}

/* the widest form that fits after the version label, keeping the right margin column clear; nothing when none fits */
static void append_update(const welcome_t *w, strvec_t *lines, size_t row, int width) {
    if (!w->upd_phase || !w->upd_version || row >= lines->n) return;
    int room = width - 2 - tui_visible_width(lines->v[row]);
    strvec_t forms = {0};
    update_labels(w, &forms);
    for (size_t i = 0; i < forms.n; i++) {
        if (tui_visible_width(forms.v[i]) > room) continue;
        char *r = xasprintf("%s %s", lines->v[row], forms.v[i]);
        free(lines->v[row]);
        lines->v[row] = r;
        break;
    }
    strvec_free(&forms);
}

void ui_welcome_set_update(tui_component_t *c, int phase, const char *version, double fraction) {
    welcome_t *w = (welcome_t *)c;
    free(w->upd_version);
    w->upd_phase = version ? phase : UI_UPD_NONE;
    w->upd_version = version ? xstrdup(version) : NULL;
    w->upd_fraction = fraction;
    w->cache_w = -1;
}

/* ---------- component ---------- */

/* `block_w` > 0 centers as if the block were that wide (what overhangs it runs on to the right) */
static void center_block(strvec_t *lines, size_t from, int width, int block_w) {
    int widest = block_w;
    for (size_t i = from; i < lines->n && block_w <= 0; i++) widest = MAX(widest, tui_visible_width(lines->v[i]));
    int indent = MAX(0, (width - widest) / 2);
    for (size_t i = from; i < lines->n; i++) {
        sb_t b = SB_INIT;
        tui_sb_spaces(&b, indent);
        sb_append(&b, lines->v[i]);
        free(lines->v[i]);
        lines->v[i] = sb_detach(&b);
    }
}

static void render_lines(welcome_t *w, int width, strvec_t *out) {
    int room = width - 2;
    if (room < 4) return;
    /* the logo sits where omp centers it (its lockup holds a bare "v<version>"); omc's label then runs on to the
     * right, or stays bare when the row has no room for it */
    char *bare = xasprintf("v%s", OMC_VERSION);
    int lockup_w = LOGO_WIDTH + LOCKUP_GAP + MAX(WORDMARK_WIDTH, tui_visible_width(bare));
    bool labeled = MAX(0, (width - lockup_w) / 2) + LOGO_WIDTH + LOCKUP_GAP + tui_visible_width(OMC_VERSION_LABEL) <= room;
    char *version = fg(TUI_TC_DIM, labeled ? OMC_VERSION_LABEL : bare);
    free(bare);
    strvec_t logo = {0};
    logo_frame(w, &logo);
    size_t start = out->n;
    if (room >= lockup_w) {
        for (int i = 0; i < 5; i++) {
            const char *beside = "";
            char *styled = NULL;
            if (i >= 1 && i <= 3) {
                if (*WORDMARK[i - 1]) styled = xasprintf("\x1b[1m%s%s\x1b[39m\x1b[22m", tui_fg_seq(TH(), TUI_TC_TEXT), WORDMARK[i - 1]);
                beside = styled ? styled : "";
            } else if (i == 4) {
                beside = version;
            }
            strvec_push_own(out, xasprintf("%s    %s", logo.v[i], beside));
            free(styled);
        }
    } else if (room >= LOGO_WIDTH) {
        for (int i = 0; i < 5; i++) strvec_push(out, logo.v[i]);
    }
    center_block(out, start, width, room >= lockup_w ? lockup_w : 0);
    /* centered without the update indicator, which then trails the version: the lockup stays put as it appears */
    if (room >= lockup_w) append_update(w, out, start + 4, width);
    strvec_free(&logo);
    free(version);
    const char *tip = width >= TIP_MIN_COLUMNS ? welcome_tip(w) : NULL;
    if (tip) {
        strvec_t tl = {0};
        double phase = is_new_tip(tip) ? (double)mono_ms() / 1500.0 : 0;
        ui_welcome_tip_lines(tip, MIN(room, TIP_MEASURE), phase, &tl);
        if (tl.n) {
            strvec_push(out, "");
            for (size_t i = 0; i < tl.n; i++) {
                size_t at = out->n;
                strvec_push(out, tl.v[i]);
                center_block(out, at, width, 0);
            }
        }
        strvec_free(&tl);
    }
}

static void welcome_render(tui_component_t *c, int width, strvec_t *out) {
    welcome_t *w = (welcome_t *)c;
    bool animating = w->anim_start != 0;
    /* omp: the header retires to native history only after the intro settles */
    if (animating) tui_hold_history(c->tui ? c->tui : w->tui, out->n);
    if (!animating && w->cache_w == width) {
        for (size_t i = 0; i < w->cache.n; i++) strvec_push(out, w->cache.v[i]);
        return;
    }
    strvec_t lines = {0};
    render_lines(w, width, &lines);
    for (size_t i = 0; i < lines.n; i++) strvec_push(out, lines.v[i]);
    strvec_free(&w->cache);
    if (animating) {
        strvec_free(&lines);
        w->cache_w = -1;
    } else {
        w->cache = lines;
        w->cache_w = width;
    }
}

static void welcome_invalidate(tui_component_t *c) {
    welcome_t *w = (welcome_t *)c;
    w->cache_w = -1;
}

static void stop_intro(welcome_t *w) {
    if (w->timer && w->tui) tui_remove_timer(w->tui, w->timer);
    w->timer = 0;
    w->anim_start = 0;
    w->cache_w = -1;
}

static void welcome_destroy(tui_component_t *c) {
    welcome_t *w = (welcome_t *)c;
    stop_intro(w);
    strvec_free(&w->cache);
    free(w->tip_override);
    free(w->upd_version);
    free(w);
}

static const tui_component_vt WELCOME_VT = {"welcome", welcome_render, NULL, welcome_invalidate, welcome_destroy};

tui_component_t *ui_welcome_new(void) {
    welcome_t *w = xcalloc(1, sizeof *w);
    tui_component_init(&w->base, &WELCOME_VT);
    w->cache_w = -1;
    return &w->base;
}

void ui_welcome_set_tip(tui_component_t *c, const char *tip) {
    welcome_t *w = (welcome_t *)c;
    free(w->tip_override);
    w->tip_override = tip ? xstrdup(tip) : NULL;
    w->cache_w = -1;
}

static void intro_tick(void *ud) {
    welcome_t *w = ud;
    tui_t *t = w->tui;
    if (mono_ms() - w->anim_start >= INTRO_MS) stop_intro(w);
    tui_request_render(t);
}

void ui_welcome_play_intro(tui_component_t *c, tui_t *tui) {
    welcome_t *w = (welcome_t *)c;
    stop_intro(w);
    w->tui = tui;
    w->anim_start = mono_ms();
    if (!w->anim_start) w->anim_start = 1;
    w->timer = tui_add_timer(tui, INTRO_TICK_MS, true, intro_tick, w);
    tui_request_render(tui);
}
