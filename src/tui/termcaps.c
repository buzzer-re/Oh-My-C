/* termcaps.c - hyperlink policy, image protocols and inline image rendering (see termcaps.h). */
#include "tui/termcaps.h"

#include "core/fs.h"
#include "core/proc.h"
#include "img/img.h"
#include "tui/notify.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <unistd.h>

static bool env_on(const char *name) {
    const char *v = env_get(name);
    if (!v) return false;
    char *t = str_lower(v), *s = str_trim(t);
    bool on = !strcmp(s, "1") || !strcmp(s, "true") || !strcmp(s, "on") || !strcmp(s, "yes") || !strcmp(s, "y");
    free(s);
    free(t);
    return on;
}

static bool env_off(const char *name) {
    const char *v = env_get(name);
    if (!v) return false;
    char *t = str_lower(v), *s = str_trim(t);
    bool off = !strcmp(s, "0") || !strcmp(s, "false") || !strcmp(s, "off") || !strcmp(s, "no") || !strcmp(s, "n");
    free(s);
    free(t);
    return off;
}

static bool herdr_inside(void) {
    return str_eq(env_get("HERDR_ENV"), "1") || env_get("HERDR_PANE_ID") || env_get("HERDR_TAB_ID") ||
           env_get("HERDR_WORKSPACE_ID");
}

static bool parse_major_minor(const char *v, int *maj, int *min) {
    if (!v) return false;
    while (isspace((unsigned char)*v)) v++;
    if (!isdigit((unsigned char)*v)) return false;
    char *e;
    long a = strtol(v, &e, 10);
    if (*e != '.' || !isdigit((unsigned char)e[1])) return false;
    long b = strtol(e + 1, NULL, 10);
    *maj = (int)a;
    *min = (int)b;
    return true;
}

/* KNOWN_TERMINALS[id].hyperlinks */
static bool terminal_hyperlinks(const char *id) {
    static const char *const ON[] = {"kitty", "ghostty", "wezterm", "iterm2", "vscode",
                                     "alacritty", "otty", "rio", "tern", "monstar"};
    for (size_t i = 0; id && i < ARRAY_LEN(ON); i++)
        if (!strcmp(id, ON[i])) return true;
    return false;
}

/* ---------- hyperlinks ---------- */

bool tui_hyperlinks_detect(const char *terminal_id) {
    if (str_eq(env_get("PI_NO_HYPERLINKS"), "1")) return false;
    if (str_eq(env_get("PI_FORCE_HYPERLINKS"), "1")) return true;
    /* multiplexer policies: Herdr renders OSC 8 itself, screen drops it, tmux >= 3.4 forwards it */
    bool tmux = tui_inside_tmux(), screen = env_get("STY") != NULL, herdr = herdr_inside();
    if (herdr && !tmux && !screen) return true;
    if (!terminal_hyperlinks(terminal_id)) return false;
    if (screen) return false;
    if (tmux) {
        int maj, min;
        return str_ieq(env_get("TERM_PROGRAM"), "tmux") && parse_major_minor(env_get("TERM_PROGRAM_VERSION"), &maj, &min) &&
               (maj > 3 || (maj == 3 && min >= 4));
    }
    char *term = str_lower(env_get("TERM") ? env_get("TERM") : "");
    bool mux_term = str_starts(term, "tmux") || str_starts(term, "screen");
    free(term);
    return !mux_term;
}

bool tui_hyperlinks_detected(void) {
    static int cached = -1;
    if (cached < 0) cached = tui_hyperlinks_detect(tui_terminal_id());
    return cached;
}

static int g_link_mode = -1; /* -1 no policy pushed yet, 0 off, 1 auto, 2 always */

void tui_hyperlinks_apply_setting(const char *mode) {
    if (str_eq(mode, "off")) g_link_mode = 0;
    else if (str_eq(mode, "auto")) g_link_mode = 1;
    else if (str_eq(mode, "always")) g_link_mode = 2;
    else if (g_link_mode < 0) g_link_mode = 1;
}

bool tui_hyperlinks_enabled(void) {
    switch (g_link_mode) {
    case 0: return false;
    case 2: return true;
    case 1:
        if (env_get("NO_COLOR") && *env_get("NO_COLOR")) return false;
        if (!isatty(STDOUT_FILENO)) return false;
        return tui_hyperlinks_detected();
    default: return tui_hyperlinks_detected();
    }
}

/* ---------- image protocol ---------- */

static int g_forced = -2; /* -2 unresolved, -1 not forced, else a tui_img_proto_t */

static int forced_protocol(void) {
    if (g_forced != -2) return g_forced;
    const char *raw = env_get("PI_FORCE_IMAGE_PROTOCOL");
    char *t = raw ? str_lower(raw) : NULL, *s = t ? str_trim(t) : NULL;
    if (!s || !*s) g_forced = -1;
    else if (!strcmp(s, "kitty")) g_forced = TUI_IMG_KITTY;
    else if (!strcmp(s, "iterm2") || !strcmp(s, "iterm")) g_forced = TUI_IMG_ITERM2;
    else if (!strcmp(s, "sixel")) g_forced = TUI_IMG_SIXEL;
    else g_forced = TUI_IMG_NONE;
    free(s);
    free(t);
    return g_forced;
}

bool tui_image_protocol_forced(void) { return forced_protocol() != -1; }

/* tmuxPaneAllowsPassthrough: tmux 3.3+ drops DCS passthrough unless allow-passthrough is on */
static bool tmux_allows_passthrough(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 0;
    char *tmux = which("tmux");
    if (!tmux) return false;
    const char *pane = env_get("TMUX_PANE");
    const char *argv[] = {tmux, "show-options", "-Apqv", pane ? "-t" : "allow-passthrough", pane ? pane : NULL,
                          pane ? "allow-passthrough" : NULL, NULL};
    proc_opts_t o = {0};
    o.timeout_ms = 2000;
    o.max_output = 4096;
    proc_result_t r = {0};
    if (proc_run(argv, &o, &r) == 0 && r.exit_code == 0) {
        char *v = str_trim(r.out ? r.out : "");
        cached = !*v || !strcmp(v, "on") || !strcmp(v, "all");
        free(v);
    }
    proc_result_free(&r);
    free(tmux);
    return cached;
}

tui_img_proto_t tui_resolve_image_protocol(const char *id, bool is_tty) {
    tui_img_proto_t p = TUI_IMG_NONE;
    if (str_eq(id, "kitty") || str_eq(id, "ghostty") || str_eq(id, "wezterm") || str_eq(id, "warp") ||
        str_eq(id, "otty") || str_eq(id, "rio") || str_eq(id, "rex") || str_eq(id, "tern") || str_eq(id, "monstar"))
        p = TUI_IMG_KITTY;
    else if (str_eq(id, "iterm2")) p = TUI_IMG_ITERM2;
    if (p == TUI_IMG_NONE && is_tty && !str_eq(id, "vscode") && !str_eq(id, "alacritty")) {
        /* getFallbackImageProtocol: tmux TERM names the pane, not the client; screen/ghostty TERMs speak Kitty */
        char *term = str_lower(env_get("TERM") ? env_get("TERM") : "");
        if (!env_get("TMUX") && !strstr(term, "tmux") && (strstr(term, "screen") || strstr(term, "ghostty")))
            p = TUI_IMG_KITTY;
        free(term);
    }
    if (p != TUI_IMG_NONE && env_get("PASEO_TERMINAL_ID")) return TUI_IMG_NONE; /* isPaseoEmbedder */
    if (p != TUI_IMG_NONE && herdr_inside()) return TUI_IMG_NONE;             /* imagesRequireOverride */
    if (p == TUI_IMG_KITTY && env_get("TMUX") && !tmux_allows_passthrough()) return TUI_IMG_NONE;
    return p;
}

static int g_proto = -1;

tui_img_proto_t tui_image_protocol(void) {
    if (g_proto < 0) {
        int f = forced_protocol();
        g_proto = f >= 0 ? f : (int)tui_resolve_image_protocol(tui_terminal_id(), isatty(STDOUT_FILENO));
    }
    return (tui_img_proto_t)g_proto;
}

void tui_set_image_protocol(tui_img_proto_t p) { g_proto = (int)p; }

bool tui_kitty_placeholders_detect(const char *id) {
    if (env_on("PI_NO_KITTY_PLACEHOLDERS")) return false;
    if (env_on("PI_KITTY_PLACEHOLDERS")) return true;
    if (env_off("PI_KITTY_PLACEHOLDERS")) return false;
    bool mux = tui_inside_multiplexer();
    if (mux) {
        const char *f = env_get("PI_FORCE_IMAGE_PROTOCOL");
        char *t = f ? str_lower(f) : NULL, *s = t ? str_trim(t) : NULL;
        bool kitty = s && !strcmp(s, "kitty");
        free(s);
        free(t);
        if (kitty) return true;
    }
    if (herdr_inside()) return false;
    return str_eq(id, "kitty") || str_eq(id, "ghostty") || str_eq(id, "monstar") || str_eq(id, "otty") || str_eq(id, "rio");
}

bool tui_kitty_placeholders(void) {
    static int cached = -1;
    if (cached < 0) cached = tui_kitty_placeholders_detect(tui_terminal_id());
    return cached;
}

static int g_cell_w = 9, g_cell_h = 18;
void tui_cell_dims(int *w, int *h) {
    *w = g_cell_w;
    *h = g_cell_h;
}
void tui_set_cell_dims(int w, int h) {
    if (w > 0 && h > 0) g_cell_w = w, g_cell_h = h;
}

static bool g_bg_light;
bool g_inband;      /* DEC 2048 in-band resize reports enabled (they carry the cell pixel size) */
static sb_t g_pending; /* bytes written before the next frame: one-time Kitty transmits, mode switches */
uint32_t tui_terminal_background(void) { return g_bg_light ? 0xffffff : 0x000000; }
void tui_set_terminal_background_light(bool light) { g_bg_light = light; }

char *tui_termcaps_probes(void) {
    if (!isatty(STDOUT_FILENO)) return xstrdup("");
    tui_img_proto_t p = tui_image_protocol();
    /* DEC 2048 (DECRQM first): in-band resize reports carry the cell pixel size where CSI 16 t gets no answer */
    if (p == TUI_IMG_NONE) return tui_image_protocol_forced() ? xstrdup("") : xstrdup("\x1b[?2048$p\x1b[?2;1;0S");
    if (p == TUI_IMG_ITERM2) return xstrdup("\x1b[?2048$p\x1b[16t\x1b]1337;ReportCellSize\x1b\\");
    return xstrdup("\x1b[?2048$p\x1b[16t");
}

static void enable_sixel(bool *changed) {
    if (tui_image_protocol() != TUI_IMG_NONE || tui_image_protocol_forced()) return;
    /* tmux answers DA1 for its own emulator: trust only its record of the client (tmuxClientRendersSixel) */
    if (env_get("TMUX")) return;
    tui_set_image_protocol(TUI_IMG_SIXEL);
    *changed = true;
}

bool tui_termcaps_response(const char *seq, size_t n, bool *changed) {
    *changed = false;
    if (n < 4 || seq[0] != '\x1b') return false;
    char *s = xstrndup(seq, n);
    bool used = false;
    int a, b, c;
    int r4, c4;
    if (sscanf(s, "\x1b[48;%d;%d;%d;%dt", &r4, &c4, &a, &b) == 4) { /* in-band resize report: pixels / cells */
        if (r4 > 0 && c4 > 0 && a > 0 && b > 0) {
            int cw = MAX(1, (int)lround((double)b / c4)), ch = MAX(1, (int)lround((double)a / r4));
            if (cw != g_cell_w || ch != g_cell_h) {
                tui_set_cell_dims(cw, ch);
                *changed = tui_image_protocol() != TUI_IMG_NONE;
            }
        }
        used = true;
    } else if (sscanf(s, "\x1b[?2048;%d$y", &a) == 1) { /* DECRQM: 1 set / 2 reset = supported */
        if ((a == 1 || a == 2) && !g_inband) {
            g_inband = true;
            sb_append(&g_pending, "\x1b[?2048h");
            *changed = true;
        }
        used = true;
    } else if (sscanf(s, "\x1b[6;%d;%dt", &a, &b) == 2) { /* CSI 6 ; height ; width t */
        if (a > 0 && b > 0 && (a != g_cell_h || b != g_cell_w)) {
            tui_set_cell_dims(b, a);
            *changed = true;
        }
        used = true;
    } else if (str_starts(s, "\x1b]1337;ReportCellSize=")) { /* iTerm2: height;width[;scale] in points */
        double h = 0, w = 0, scale = 1;
        int k = sscanf(s + 23, "%lf;%lf;%lf", &h, &w, &scale);
        if (k >= 2 && h > 0 && w > 0) {
            tui_set_cell_dims((int)lround(w * (k >= 3 ? scale : 1)), (int)lround(h * (k >= 3 ? scale : 1)));
            *changed = true;
        }
        used = true;
    } else if (sscanf(s, "\x1b[?2;%d;%d;%dS", &a, &b, &c) == 3 || sscanf(s, "\x1b[?2;%d;%dS", &a, &b) == 2) {
        if (a == 0 && b > 0) enable_sixel(changed); /* XTSMGRAPHICS: success with a geometry */
        used = true;
    } else if (n >= 4 && s[1] == '[' && s[2] == '?' && s[n - 1] == 'c') { /* DA1: attribute 4 is SIXEL */
        const char *p = s + 3;
        while (*p) {
            if (*p == '4' && (p == s + 3 || p[-1] == ';') && (p[1] == ';' || p[1] == 'c')) {
                enable_sixel(changed);
                break;
            }
            p++;
        }
        used = false; /* the keyboard probe still consumes DA1 */
    }
    free(s);
    return used;
}

/* ---------- encoders ---------- */

static char *wrap_passthrough(char *payload) {
    if (!tui_inside_tmux()) return payload;
    char *w = tui_wrap_tmux_passthrough(payload);
    free(payload);
    return w;
}

static char *chunk_kitty_apc(const char *lead, const char *b64) {
    const size_t CHUNK = 4096;
    size_t len = strlen(b64);
    if (len <= CHUNK) return wrap_passthrough(xasprintf("\x1b_G%s;%s\x1b\\", lead, b64));
    sb_t b = SB_INIT;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = MIN(CHUNK, len - off);
        bool last = off + CHUNK >= len;
        char *one = off == 0 ? xasprintf("\x1b_G%s,m=1;%.*s\x1b\\", lead, (int)n, b64 + off)
                             : xasprintf("\x1b_Gq=2,m=%d;%.*s\x1b\\", last ? 0 : 1, (int)n, b64 + off);
        one = wrap_passthrough(one);
        sb_append(&b, one);
        free(one);
    }
    return sb_detach(&b);
}

char *tui_kitty_encode(const char *b64, int cols, int rows) {
    sb_t p = SB_INIT;
    sb_append(&p, "a=T,f=100,q=2,C=1");
    if (cols) sb_appendf(&p, ",c=%d", cols);
    if (rows) sb_appendf(&p, ",r=%d", rows);
    char *r = chunk_kitty_apc(sb_cstr(&p), b64);
    sb_free(&p);
    return r;
}

char *tui_kitty_transmit(const char *b64, uint32_t id) {
    char lead[64];
    snprintf(lead, sizeof lead, "a=t,f=100,q=2,i=%u", id);
    return chunk_kitty_apc(lead, b64);
}

char *tui_kitty_placement(uint32_t id, uint32_t pid, int cols, int rows) {
    sb_t p = SB_INIT;
    sb_appendf(&p, "\x1b_Ga=p,q=2,C=1,i=%u", id);
    if (pid) sb_appendf(&p, ",p=%u", pid);
    if (cols) sb_appendf(&p, ",c=%d", cols);
    if (rows) sb_appendf(&p, ",r=%d", rows);
    sb_append(&p, "\x1b\\");
    return wrap_passthrough(sb_detach(&p));
}

char *tui_kitty_delete_all(void) { return wrap_passthrough(xstrdup("\x1b_Ga=d,d=A,q=2\x1b\\")); }

/* kitty gen/rowcolumn-diacritics.txt */
static const uint32_t DIACRITICS[] = {
    0x305,   0x30d,   0x30e,   0x310,   0x312,   0x33d,   0x33e,   0x33f,   0x346,   0x34a,   0x34b,   0x34c,   0x350,
    0x351,   0x352,   0x357,   0x35b,   0x363,   0x364,   0x365,   0x366,   0x367,   0x368,   0x369,   0x36a,   0x36b,
    0x36c,   0x36d,   0x36e,   0x36f,   0x483,   0x484,   0x485,   0x486,   0x487,   0x592,   0x593,   0x594,   0x595,
    0x597,   0x598,   0x599,   0x59c,   0x59d,   0x59e,   0x59f,   0x5a0,   0x5a1,   0x5a8,   0x5a9,   0x5ab,   0x5ac,
    0x5af,   0x5c4,   0x610,   0x611,   0x612,   0x613,   0x614,   0x615,   0x616,   0x617,   0x657,   0x658,   0x659,
    0x65a,   0x65b,   0x65d,   0x65e,   0x6d6,   0x6d7,   0x6d8,   0x6d9,   0x6da,   0x6db,   0x6dc,   0x6df,   0x6e0,
    0x6e1,   0x6e2,   0x6e4,   0x6e7,   0x6e8,   0x6eb,   0x6ec,   0x730,   0x732,   0x733,   0x735,   0x736,   0x73a,
    0x73d,   0x73f,   0x740,   0x741,   0x743,   0x745,   0x747,   0x749,   0x74a,   0x7eb,   0x7ec,   0x7ed,   0x7ee,
    0x7ef,   0x7f0,   0x7f1,   0x7f3,   0x816,   0x817,   0x818,   0x819,   0x81b,   0x81c,   0x81d,   0x81e,   0x81f,
    0x820,   0x821,   0x822,   0x823,   0x825,   0x826,   0x827,   0x829,   0x82a,   0x82b,   0x82c,   0x82d,   0x951,
    0x953,   0x954,   0xf82,   0xf83,   0xf86,   0xf87,   0x135d,  0x135e,  0x135f,  0x17dd,  0x193a,  0x1a17,  0x1a75,
    0x1a76,  0x1a77,  0x1a78,  0x1a79,  0x1a7a,  0x1a7b,  0x1a7c,  0x1b6b,  0x1b6d,  0x1b6e,  0x1b6f,  0x1b70,  0x1b71,
    0x1b72,  0x1b73,  0x1cd0,  0x1cd1,  0x1cd2,  0x1cda,  0x1cdb,  0x1ce0,  0x1dc0,  0x1dc1,  0x1dc3,  0x1dc4,  0x1dc5,
    0x1dc6,  0x1dc7,  0x1dc8,  0x1dc9,  0x1dcb,  0x1dcc,  0x1dd1,  0x1dd2,  0x1dd3,  0x1dd4,  0x1dd5,  0x1dd6,  0x1dd7,
    0x1dd8,  0x1dd9,  0x1dda,  0x1ddb,  0x1ddc,  0x1ddd,  0x1dde,  0x1ddf,  0x1de0,  0x1de1,  0x1de2,  0x1de3,  0x1de4,
    0x1de5,  0x1de6,  0x1dfe,  0x20d0,  0x20d1,  0x20d4,  0x20d5,  0x20d6,  0x20d7,  0x20db,  0x20dc,  0x20e1,  0x20e7,
    0x20e9,  0x20f0,  0x2cef,  0x2cf0,  0x2cf1,  0x2de0,  0x2de1,  0x2de2,  0x2de3,  0x2de4,  0x2de5,  0x2de6,  0x2de7,
    0x2de8,  0x2de9,  0x2dea,  0x2deb,  0x2dec,  0x2ded,  0x2dee,  0x2def,  0x2df0,  0x2df1,  0x2df2,  0x2df3,  0x2df4,
    0x2df5,  0x2df6,  0x2df7,  0x2df8,  0x2df9,  0x2dfa,  0x2dfb,  0x2dfc,  0x2dfd,  0x2dfe,  0x2dff,  0xa66f,  0xa67c,
    0xa67d,  0xa6f0,  0xa6f1,  0xa8e0,  0xa8e1,  0xa8e2,  0xa8e3,  0xa8e4,  0xa8e5,  0xa8e6,  0xa8e7,  0xa8e8,  0xa8e9,
    0xa8ea,  0xa8eb,  0xa8ec,  0xa8ed,  0xa8ee,  0xa8ef,  0xa8f0,  0xa8f1,  0xaab0,  0xaab2,  0xaab3,  0xaab7,  0xaab8,
    0xaabe,  0xaabf,  0xaac1,  0xfe20,  0xfe21,  0xfe22,  0xfe23,  0xfe24,  0xfe25,  0xfe26,  0x10a0f, 0x10a38, 0x1d185,
    0x1d186, 0x1d187, 0x1d188, 0x1d189, 0x1d1aa, 0x1d1ab, 0x1d1ac, 0x1d1ad, 0x1d242, 0x1d243, 0x1d244,
};
#define PLACEHOLDER_MAX ((int)ARRAY_LEN(DIACRITICS))

static void put_cp(sb_t *b, uint32_t cp) {
    char buf[4];
    if (cp < 0x80) buf[0] = (char)cp, sb_appendn(b, buf, 1);
    else if (cp < 0x800) {
        buf[0] = (char)(0xc0 | (cp >> 6)), buf[1] = (char)(0x80 | (cp & 0x3f));
        sb_appendn(b, buf, 2);
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xe0 | (cp >> 12)), buf[1] = (char)(0x80 | ((cp >> 6) & 0x3f)), buf[2] = (char)(0x80 | (cp & 0x3f));
        sb_appendn(b, buf, 3);
    } else {
        buf[0] = (char)(0xf0 | (cp >> 18)), buf[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3f)), buf[3] = (char)(0x80 | (cp & 0x3f));
        sb_appendn(b, buf, 4);
    }
}

void tui_kitty_placeholder_lines(uint32_t id, uint32_t pid, int cols, int rows, strvec_t *out) {
    char lead[96];
    int l = snprintf(lead, sizeof lead, "\x1b[38;2;%u;%u;%um", (id >> 16) & 0xff, (id >> 8) & 0xff, id & 0xff);
    if (pid) snprintf(lead + l, sizeof lead - (size_t)l, "\x1b[58:2::%u:%u:%um", (pid >> 16) & 0xff, (pid >> 8) & 0xff, pid & 0xff);
    for (int r = 0; r < rows; r++) {
        sb_t b = SB_INIT;
        if (r == 0) {
            sb_t p = SB_INIT;
            sb_appendf(&p, "\x1b_Ga=p,U=1,q=2,i=%u", id);
            if (pid) sb_appendf(&p, ",p=%u", pid);
            sb_appendf(&p, ",c=%d,r=%d\x1b\\", cols, rows);
            char *apc = wrap_passthrough(sb_detach(&p));
            sb_append(&b, apc);
            free(apc);
        }
        sb_append(&b, lead);
        for (int c = 0; c < cols; c++) {
            put_cp(&b, 0x10eeee);
            if (r < PLACEHOLDER_MAX) put_cp(&b, DIACRITICS[r]);
            if (c < PLACEHOLDER_MAX) put_cp(&b, DIACRITICS[c]);
        }
        sb_append(&b, "\x1b[39;59m");
        strvec_push_own(out, sb_detach(&b));
    }
}

char *tui_iterm2_encode(const char *b64, int w, int h, bool preserve) {
    sb_t b = SB_INIT;
    sb_append(&b, "\x1b]1337;File=inline=1");
    if (w > 0) sb_appendf(&b, ";width=%d", w);
    if (h > 0) sb_appendf(&b, ";height=%d", h);
    if (!preserve) sb_append(&b, ";preserveAspectRatio=0");
    sb_putc(&b, ':');
    sb_append(&b, b64);
    sb_putc(&b, '\a');
    return sb_detach(&b);
}

/* ---------- SIXEL encoder ---------- */

typedef struct {
    uint32_t key; /* 15-bit RGB555 histogram bin */
    uint32_t count;
    uint64_t r, g, b;
} bin_t;

typedef struct {
    int lo, hi; /* bins [lo, hi) */
} box_t;

static int bin_ch(const bin_t *b, int ch) {
    return ch == 0 ? (int)(b->key >> 10) & 31 : ch == 1 ? (int)(b->key >> 5) & 31 : (int)b->key & 31;
}

static int g_sort_ch;
static int bin_cmp(const void *a, const void *b) {
    return bin_ch(a, g_sort_ch) - bin_ch(b, g_sort_ch);
}

/* median cut over a 5-bit/channel histogram: palette of at most 256 colors */
static int median_cut(const img_t *im, uint8_t pal[256][3]) {
    uint32_t *hist = xcalloc(32768, sizeof *hist);
    uint64_t *sum = xcalloc(32768 * 3, sizeof *sum);
    size_t n = (size_t)im->w * (size_t)im->h;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = im->px + i * 4;
        if (!p[3]) continue;
        uint32_t k = ((uint32_t)(p[0] >> 3) << 10) | ((uint32_t)(p[1] >> 3) << 5) | (p[2] >> 3);
        hist[k]++;
        sum[k * 3] += p[0], sum[k * 3 + 1] += p[1], sum[k * 3 + 2] += p[2];
    }
    int nb = 0;
    for (uint32_t k = 0; k < 32768; k++) nb += hist[k] > 0;
    bin_t *bins = xcalloc((size_t)MAX(nb, 1), sizeof *bins);
    nb = 0;
    for (uint32_t k = 0; k < 32768; k++)
        if (hist[k]) bins[nb++] = (bin_t){k, hist[k], sum[k * 3], sum[k * 3 + 1], sum[k * 3 + 2]};
    free(hist);
    free(sum);
    if (!nb) {
        free(bins);
        return 0;
    }
    box_t boxes[256];
    int nbox = 1;
    boxes[0] = (box_t){0, nb};
    while (nbox < 256) {
        /* split the box with the widest channel range weighted by its pixel count */
        int best = -1, best_ch = 0;
        double best_score = 0;
        for (int i = 0; i < nbox; i++) {
            if (boxes[i].hi - boxes[i].lo < 2) continue;
            int mn[3] = {31, 31, 31}, mx[3] = {0, 0, 0};
            uint64_t cnt = 0;
            for (int j = boxes[i].lo; j < boxes[i].hi; j++) {
                for (int c = 0; c < 3; c++) {
                    int v = bin_ch(&bins[j], c);
                    mn[c] = MIN(mn[c], v);
                    mx[c] = MAX(mx[c], v);
                }
                cnt += bins[j].count;
            }
            int ch = 0;
            for (int c = 1; c < 3; c++)
                if (mx[c] - mn[c] > mx[ch] - mn[ch]) ch = c;
            double score = (double)(mx[ch] - mn[ch]) * sqrt((double)cnt);
            if (mx[ch] > mn[ch] && score > best_score) best_score = score, best = i, best_ch = ch;
        }
        if (best < 0) break;
        box_t *bx = &boxes[best];
        g_sort_ch = best_ch;
        qsort(bins + bx->lo, (size_t)(bx->hi - bx->lo), sizeof *bins, bin_cmp);
        uint64_t total = 0, acc = 0;
        for (int j = bx->lo; j < bx->hi; j++) total += bins[j].count;
        int cut = bx->lo + 1;
        for (int j = bx->lo; j < bx->hi - 1; j++) {
            acc += bins[j].count;
            if (acc * 2 >= total) {
                cut = j + 1;
                break;
            }
            cut = j + 2;
        }
        if (cut >= bx->hi) cut = bx->hi - 1;
        boxes[nbox++] = (box_t){cut, bx->hi};
        bx->hi = cut;
    }
    for (int i = 0; i < nbox; i++) {
        uint64_t c = 0, r = 0, g = 0, b = 0;
        for (int j = boxes[i].lo; j < boxes[i].hi; j++) c += bins[j].count, r += bins[j].r, g += bins[j].g, b += bins[j].b;
        pal[i][0] = (uint8_t)(r / MAX(c, 1)), pal[i][1] = (uint8_t)(g / MAX(c, 1)), pal[i][2] = (uint8_t)(b / MAX(c, 1));
    }
    free(bins);
    return nbox;
}

static int nearest(uint8_t pal[256][3], int n, int r, int g, int b) {
    int best = 0;
    long bd = LONG_MAX;
    for (int i = 0; i < n; i++) {
        long dr = r - pal[i][0], dg = g - pal[i][1], db = b - pal[i][2];
        long d = dr * dr * 30 + dg * dg * 59 + db * db * 11;
        if (d < bd) bd = d, best = i;
    }
    return best;
}

static void put_run(sb_t *b, int bits, int count) {
    char ch = (char)(63 + bits);
    if (count > 3) sb_appendf(b, "!%d%c", count, ch);
    else
        for (int i = 0; i < count; i++) sb_putc(b, ch);
}

char *tui_sixel_ensure_raster_width(char *seq) {
    /* ^ESC P [0-9;]* q " d;d;W;d (#i[;1|2;d;d;d] | $)* followed by '-' -> insert !W?$ */
    const char *p = seq;
    if (p[0] != '\x1b' || p[1] != 'P') return seq;
    p += 2;
    while (isdigit((unsigned char)*p) || *p == ';') p++;
    if (*p++ != 'q' || *p++ != '"') return seq;
    long vals[4];
    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)*p)) return seq;
        vals[i] = strtol(p, (char **)&p, 10);
        if (i < 3 && *p++ != ';') return seq;
    }
    for (;;) {
        if (*p == '$') {
            p++;
            continue;
        }
        if (*p != '#') break;
        const char *q = p + 1;
        if (!isdigit((unsigned char)*q)) break;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == ';') {
            const char *r = q + 1;
            if (*r != '1' && *r != '2') break;
            r++;
            for (int i = 0; i < 3; i++) {
                if (*r++ != ';' || !isdigit((unsigned char)*r)) goto done;
                while (isdigit((unsigned char)*r)) r++;
            }
            q = r;
        }
        p = q;
    }
done:
    if (*p != '-') return seq;
    size_t at = (size_t)(p - seq);
    char *out = xasprintf("%.*s!%ld?$%s", (int)at, seq, vals[2], seq + at);
    free(seq);
    return out;
}

char *tui_sixel_encode(const uint8_t *data, size_t n, int w, int h, uint32_t bg) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return NULL;
    img_t src = {0};
    if (img_decode(data, n, &src, NULL, NULL) != 0) return NULL;
    img_t im = {0};
    bool translucent = img_translucent(&src);
    if (src.w != w || src.h != h) {
        if (translucent) img_premultiply(&src);
        if (img_resize(&src, w, h, &im) != 0) {
            img_free(&src);
            return NULL;
        }
        if (translucent) img_unpremultiply(&im);
        img_free(&src);
    } else {
        im = src;
    }
    /* partial coverage composited over the terminal background; holes stay transparent */
    int bgc[3] = {(int)(bg >> 16) & 0xff, (int)(bg >> 8) & 0xff, (int)bg & 0xff};
    size_t npx = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npx; i++) {
        uint8_t *p = im.px + i * 4;
        if (p[3] == 0 || p[3] == 255) continue;
        for (int c = 0; c < 3; c++) p[c] = (uint8_t)((p[c] * p[3] + bgc[c] * (255 - p[3]) + 127) / 255);
        p[3] = 255;
    }
    int rows = h < 6 ? h : h - h % 6; /* band_rows: a partial last band is dropped */
    uint8_t pal[256][3];
    int np = median_cut(&im, pal);
    /* Floyd-Steinberg error diffusion into palette indices (-1 = transparent) */
    int16_t *idx = xmalloc(npx * sizeof *idx);
    float *err = xcalloc((size_t)(w + 2) * 2 * 3, sizeof *err);
    for (int y = 0; y < h; y++) {
        float *cur = err + (size_t)(y & 1) * (size_t)(w + 2) * 3, *nxt = err + (size_t)((y + 1) & 1) * (size_t)(w + 2) * 3;
        memset(nxt, 0, (size_t)(w + 2) * 3 * sizeof *nxt);
        for (int x = 0; x < w; x++) {
            const uint8_t *p = im.px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (!p[3] || !np) {
                idx[(size_t)y * (size_t)w + (size_t)x] = -1;
                continue;
            }
            float v[3];
            for (int c = 0; c < 3; c++) {
                v[c] = p[c] + cur[(x + 1) * 3 + c];
                v[c] = v[c] < 0 ? 0 : v[c] > 255 ? 255 : v[c];
            }
            int k = nearest(pal, np, (int)v[0], (int)v[1], (int)v[2]);
            idx[(size_t)y * (size_t)w + (size_t)x] = (int16_t)k;
            for (int c = 0; c < 3; c++) {
                float e = v[c] - pal[k][c];
                cur[(x + 2) * 3 + c] += e * 7 / 16;
                nxt[x * 3 + c] += e * 3 / 16;
                nxt[(x + 1) * 3 + c] += e * 5 / 16;
                nxt[(x + 2) * 3 + c] += e * 1 / 16;
            }
        }
    }
    free(err);
    img_free(&im);
    sb_t b = SB_INIT;
    sb_appendf(&b, "\x1bP9;1;0q\"1;1;%d;%d", w, rows);
    for (int i = 0; i < np; i++)
        sb_appendf(&b, "#%d;2;%d;%d;%d", i, (pal[i][0] * 100 + 127) / 255, (pal[i][1] * 100 + 127) / 255,
                   (pal[i][2] * 100 + 127) / 255);
    uint8_t *bits = xcalloc((size_t)MAX(np, 1) * (size_t)w, 1);
    bool *used = xcalloc((size_t)MAX(np, 1), sizeof *used);
    /* icy_sixel's layout: every color run of a band spans the full width and ends with `$`, every band
     * with `-` */
    for (int y0 = 0; y0 < rows; y0 += 6) {
        memset(bits, 0, (size_t)MAX(np, 1) * (size_t)w);
        memset(used, 0, (size_t)MAX(np, 1) * sizeof *used);
        for (int dy = 0; dy < 6 && y0 + dy < rows; dy++)
            for (int x = 0; x < w; x++) {
                int k = idx[(size_t)(y0 + dy) * (size_t)w + (size_t)x];
                if (k < 0) continue;
                bits[(size_t)k * (size_t)w + (size_t)x] |= (uint8_t)(1 << dy);
                used[k] = true;
            }
        for (int k = 0; k < np; k++) {
            if (!used[k]) continue;
            const uint8_t *row = bits + (size_t)k * (size_t)w;
            sb_appendf(&b, "#%d", k);
            for (int x = 0; x < w;) {
                int run = 1;
                while (x + run < w && row[x + run] == row[x]) run++;
                put_run(&b, row[x], run);
                x += run;
            }
            sb_putc(&b, '$');
        }
        sb_putc(&b, '-');
    }
    sb_append(&b, "\x1b\\");
    free(bits);
    free(used);
    free(idx);
    return tui_sixel_ensure_raster_width(sb_detach(&b));
}

/* ---------- layout helpers ---------- */

void tui_image_fit(int iw, int ih, int max_cols, int max_rows, int cw, int ch, int *cols, int *rows) {
    iw = MAX(iw, 1), ih = MAX(ih, 1), cw = MAX(cw, 1), ch = MAX(ch, 1);
    if (max_cols <= 0 && max_rows <= 0) {
        *cols = MAX(1, (iw + cw - 1) / cw);
        *rows = MAX(1, (ih + ch - 1) / ch);
        return;
    }
    double mw = max_cols > 0 ? (double)max_cols * cw : INFINITY, mh = max_rows > 0 ? (double)max_rows * ch : INFINITY;
    double scale = fmin(mw / iw, mh / ih);
    int c = MAX(1, (int)floor(iw * scale / cw)), r = MAX(1, (int)ceil(ih * scale / ch - 1e-9));
    *cols = max_cols > 0 ? MIN(c, max_cols) : c;
    *rows = max_rows > 0 ? MIN(r, max_rows) : r;
}

char *tui_image_fallback(const char *mime, int w, int h, const char *filename) {
    sb_t b = SB_INIT;
    sb_append(&b, "[Image: ");
    if (filename && *filename) sb_appendf(&b, "%s ", filename);
    sb_appendf(&b, "[%s]", mime ? mime : "");
    if (w > 0 && h > 0) sb_appendf(&b, " %dx%d", w, h);
    sb_putc(&b, ']');
    return sb_detach(&b);
}

bool tui_is_image_line(const char *line) {
    tui_img_proto_t p = tui_image_protocol();
    if (!p || !line) return false;
    size_t n = strnlen(line, p == TUI_IMG_SIXEL ? 128 : 512);
    for (size_t i = 0; i < n; i++) {
        const char *s = line + i;
        if (p == TUI_IMG_KITTY && (unsigned char)s[0] == 0xf4 && (unsigned char)s[1] == 0x8e && (unsigned char)s[2] == 0xbb &&
            (unsigned char)s[3] == 0xae)
            return true; /* U+10EEEE */
        if (s[0] != '\x1b') continue;
        if (p == TUI_IMG_KITTY && s[1] == '_' && s[2] == 'G') return true;
        if (p == TUI_IMG_KITTY && !strncmp(s, "\x1bPtmux;\x1b\x1b_G", 10)) return true;
        if (p == TUI_IMG_ITERM2 && !strncmp(s, "\x1b]1337;File=", 12)) return true;
        if (p == TUI_IMG_SIXEL && s[1] == 'P') {
            const char *q = s + 2;
            while (isdigit((unsigned char)*q) || *q == ';') q++;
            if (*q == 'q') return true;
        }
    }
    return false;
}

int tui_image_lift(const char *line) {
    int n = 0;
    if (line && sscanf(line, "\x1b" "7\x1b[%dA", &n) == 1) return n;
    return 0;
}

/* ---------- Image component ---------- */

tui_image_opts_t tui_image_default_opts(int viewport_rows, int max_cols_setting, int max_rows_setting) {
    tui_image_opts_t o = {0};
    o.max_cols = max_cols_setting;
    int setting = MAX(0, max_rows_setting);
    int frac = viewport_rows > 0 ? viewport_rows * 6 / 10 : 0;
    if (setting == 0) o.max_rows = frac;
    else if (frac > 0) o.max_rows = MIN(setting, frac);
    else o.max_rows = setting;
    return o;
}

static hmap_t g_ids;          /* image key -> id (uintptr) */
static hmap_t g_transmitted;  /* "<id>" -> transmitted PNG base64 */
static hmap_t g_placement;    /* "<id>" -> placement id (uintptr): the renderer numbers placements from 1 */
static uint32_t g_next_placement = 1;
static hmap_t g_png;          /* key -> converted PNG base64 (Kitty speaks PNG only) */
static hmap_t g_sixel;        /* "key|w|h" -> sequence */
static bool g_maps;
static uint32_t g_next_id;

static void maps_init(void) {
    if (g_maps) return;
    hmap_init(&g_ids);
    hmap_init(&g_transmitted);
    hmap_init(&g_placement);
    hmap_init(&g_png);
    hmap_init(&g_sixel);
    g_maps = true;
    /* nextImageIdSeed: a random 24-bit base so ids from earlier runs in the same terminal never collide */
    uint32_t seed = 0;
    rand_bytes(&seed, sizeof seed);
    seed &= 0xffffff;
    g_next_id = ((seed + 0x10000) & 0xffffff) ? (seed + 0x10000) & 0xffffff : 1;
}

static uint32_t acquire_id(const char *key) {
    maps_init();
    if (key && *key) {
        void *v = hmap_get(&g_ids, key);
        if (v) return (uint32_t)(uintptr_t)v;
    }
    uint32_t id = g_next_id;
    g_next_id = (g_next_id + 1) & 0xffffff;
    if (!g_next_id) g_next_id = 1;
    if (key && *key) hmap_put(&g_ids, key, (void *)(uintptr_t)id);
    return id;
}

char *tui_image_reset_display(void) {
    if (!g_maps || tui_image_protocol() != TUI_IMG_KITTY || !g_transmitted.n) return NULL;
    hmap_foreach(&g_transmitted, it) {
        char *tx = tui_kitty_transmit(it->val, (uint32_t)strtoul(it->key, NULL, 10));
        sb_append(&g_pending, tx);
        free(tx);
    }
    return tui_kitty_delete_all();
}

char *tui_image_take_pending(void) {
    if (!g_pending.len) return NULL;
    return sb_detach(&g_pending);
}

static void fallback_lines(const char *mime, int w, int h, const char *filename, const char *sgr, strvec_t *out) {
    char *t = tui_image_fallback(mime, w, h, filename);
    strvec_push_own(out, sgr && *sgr ? xasprintf("%s%s\x1b[39m", sgr, t) : xstrdup(t));
    free(t);
}

void tui_image_render(const char *b64, const char *mime, const char *key, const char *filename, int width,
                      const tui_image_opts_t *o, const char *sgr, strvec_t *out) {
    size_t n = 0;
    uint8_t *raw = b64 ? base64_decode(b64, strlen(b64), &n) : NULL;
    img_meta_t m = {0};
    bool sniffed = raw && img_sniff(raw, n, &m) && m.w > 0 && m.h > 0;
    int iw = sniffed ? m.w : 800, ih = sniffed ? m.h : 600;
    tui_img_proto_t p = tui_image_protocol();
    if (!p || !raw) {
        fallback_lines(mime, sniffed ? m.w : 0, sniffed ? m.h : 0, filename, sgr, out);
        free(raw);
        return;
    }
    int cap = o ? o->max_cols : 0;
    int max_w = MAX(1, width - 2);
    if (cap > 0) max_w = MIN(max_w, cap);
    int cw, ch, cols, rows;
    tui_cell_dims(&cw, &ch);
    tui_image_fit(iw, ih, max_w, o ? o->max_rows : 0, cw, ch, &cols, &rows);
    char *seq = NULL;
    if (p == TUI_IMG_KITTY) {
        maps_init();
        const char *png = b64;
        char *own = NULL;
        if (!str_eq(mime, "image/png")) {
            const char *conv = key ? hmap_get(&g_png, key) : NULL;
            if (!conv) {
                img_t im = {0};
                if (img_decode(raw, n, &im, NULL, NULL) == 0) {
                    size_t pl = 0;
                    uint8_t *pb = img_png_encode(&im, -1, &pl);
                    char *enc = base64_encode(pb, pl);
                    free(pb);
                    img_free(&im);
                    if (key) hmap_put(&g_png, key, enc);
                    else own = enc;
                    conv = enc;
                }
            }
            if (!conv) {
                fallback_lines(mime, iw, ih, filename, sgr, out);
                free(raw);
                return;
            }
            png = conv;
        }
        uint32_t id = acquire_id(key);
        char ids[16];
        snprintf(ids, sizeof ids, "%u", id);
        if (!hmap_get(&g_transmitted, ids)) {
            char *tx = tui_kitty_transmit(png, id);
            sb_append(&g_pending, tx);
            free(tx);
            hmap_put(&g_transmitted, ids, xstrdup(png)); /* kept to transmit again after a destructive reset */
        }
        free(own);
        if (tui_kitty_placeholders() && cols <= PLACEHOLDER_MAX && rows <= PLACEHOLDER_MAX) {
            tui_kitty_placeholder_lines(id, id, cols, rows, out);
            free(raw);
            return;
        }
        void *pv = hmap_get(&g_placement, ids);
        uint32_t pid = pv ? (uint32_t)(uintptr_t)pv : g_next_placement++;
        if (!pv) hmap_put(&g_placement, ids, (void *)(uintptr_t)pid);
        seq = tui_kitty_placement(id, pid, cols, rows);
    } else if (p == TUI_IMG_ITERM2) {
        seq = tui_iterm2_encode(b64, cols, rows, true);
    } else {
        int hpx = rows * ch, wpx = cols * cw;
        if (hpx < 6) rows = (6 + ch - 1) / ch;
        char *ck = xasprintf("%s|%d|%d|%u", key ? key : "", wpx, hpx, tui_terminal_background());
        maps_init();
        const char *cached = key ? hmap_get(&g_sixel, ck) : NULL;
        if (cached) seq = xstrdup(cached);
        else {
            seq = tui_sixel_encode(raw, n, wpx, hpx, tui_terminal_background());
            if (seq && key) hmap_put(&g_sixel, ck, xstrdup(seq));
        }
        free(ck);
        if (!seq) {
            fallback_lines(mime, iw, ih, filename, sgr, out);
            free(raw);
            return;
        }
    }
    free(raw);
    /* direct placement: rows-1 reserved rows, then the last row lifts the cursor to the block's origin */
    for (int i = 0; i < rows - 1; i++) strvec_push(out, "\x1b[0m");
    int up = rows - 1;
    strvec_push_own(out, up > 0 ? xasprintf("\x1b" "7\x1b[%dA%s\x1b" "8", up, seq) : xstrdup(seq));
    free(seq);
}
