/* WP-29 rich rendering and display options: terminal image protocols (Kitty transmit / placement / Unicode
 * placeholders, iTerm2, SIXEL encoder round trip), image fit and fallback text, the tui.hyperlinks policy,
 * the image-protocol resolution and terminal replies, the cache-miss marker and the generation-rate meter.
 * Expected escape shapes are omp v18.14.2's (captured under `script` with PI_FORCE_IMAGE_PROTOCOL). */
#include "coding/tokrate.h"
#include "img/img.h"
#include "tui/termcaps.h"
#include "ui/ui.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static bool eq_free(char *got, const char *want) {
    bool ok = got && want && !strcmp(got, want);
    if (!ok) fprintf(stderr, "  got  [%s]\n  want [%s]\n", got ? got : "(null)", want);
    free(got);
    return ok;
}

static const char *ENV_KEYS[] = {"KITTY_WINDOW_ID", "GHOSTTY_RESOURCES_DIR", "WEZTERM_PANE", "ITERM_SESSION_ID", "VSCODE_PID",
                                 "ALACRITTY_WINDOW_ID", "TERM_PROGRAM", "TERM_PROGRAM_VERSION", "LC_TERMINAL", "TERM",
                                 "TMUX", "TMUX_PANE", "STY", "HERDR_ENV", "PI_NO_HYPERLINKS", "PI_FORCE_HYPERLINKS",
                                 "PASEO_TERMINAL_ID", "PI_FORCE_IMAGE_PROTOCOL", "PI_KITTY_PLACEHOLDERS",
                                 "PI_NO_KITTY_PLACEHOLDERS", "ZELLIJ"};
static void clear_env(void) {
    for (size_t i = 0; i < sizeof ENV_KEYS / sizeof *ENV_KEYS; i++) unsetenv(ENV_KEYS[i]);
}

/* ---- a minimal SIXEL decoder (raster attributes, RGB palette, runs, $ and -) for the round trip ---- */
typedef struct {
    int w, h;
    int *px; /* palette index, -1 transparent */
    int pal[256][3];
} sixel_img_t;

static bool sixel_decode(const char *s, sixel_img_t *o) {
    memset(o, 0, sizeof *o);
    const char *p = strchr(s, 'q');
    if (!p || p[1] != '"') return false;
    p += 2;
    int pan, pad;
    if (sscanf(p, "%d;%d;%d;%d", &pan, &pad, &o->w, &o->h) != 4) return false;
    o->px = malloc(sizeof(int) * (size_t)o->w * (size_t)o->h);
    for (int i = 0; i < o->w * o->h; i++) o->px[i] = -1;
    while (*p && *p != '#' && *p != '-' && *p != '!' && !(*p >= '?' && *p <= '~')) p++;
    int x = 0, y = 0, color = 0;
    while (*p && *p != '\x1b') {
        if (*p == '#') {
            int c = (int)strtol(p + 1, (char **)&p, 10);
            if (*p == ';') {
                int kind, r, g, b;
                sscanf(p, ";%d;%d;%d;%d", &kind, &r, &g, &b);
                o->pal[c][0] = r, o->pal[c][1] = g, o->pal[c][2] = b;
                while (*p == ';' || isdigit((unsigned char)*p)) p++;
            }
            color = c;
        } else if (*p == '$') {
            x = 0, p++;
        } else if (*p == '-') {
            x = 0, y += 6, p++;
        } else {
            int count = 1;
            if (*p == '!') count = (int)strtol(p + 1, (char **)&p, 10);
            int bits = *p++ - 63;
            for (int k = 0; k < count; k++, x++)
                for (int b = 0; b < 6; b++)
                    if ((bits >> b) & 1 && x < o->w && y + b < o->h) o->px[(y + b) * o->w + x] = color;
        }
    }
    return true;
}

static void test_images(void) {
    clear_env();
    /* calculateImageFit: 48x24 px into 98 columns x 20 rows */
    int c, r;
    tui_image_fit(48, 24, 98, 20, 9, 18, &c, &r);
    CHECK(c == 80 && r == 20);
    tui_image_fit(48, 24, 98, 20, 16, 32, &c, &r);
    CHECK(c == 80 && r == 20);
    tui_image_fit(48, 24, 0, 0, 9, 18, &c, &r);
    CHECK(c == 6 && r == 2);
    CHECK(eq_free(tui_image_fallback("image/png", 48, 24, NULL), "[Image: [image/png] 48x24]"));
    CHECK(eq_free(tui_image_fallback("image/jpeg", 0, 0, "a.jpg"), "[Image: a.jpg [image/jpeg]]"));
    tui_image_opts_t o = tui_image_default_opts(40, 100, 20);
    CHECK(o.max_cols == 100 && o.max_rows == 20);
    o = tui_image_default_opts(20, 100, 20);
    CHECK(o.max_rows == 12);
    o = tui_image_default_opts(40, 100, 0);
    CHECK(o.max_rows == 24);

    /* Kitty: transmit (chunked past 4096), direct placement, delete-all, placeholder grid */
    CHECK(eq_free(tui_kitty_transmit("QUJD", 7), "\x1b_Ga=t,f=100,q=2,i=7;QUJD\x1b\\"));
    char big[5001];
    memset(big, 'A', 5000);
    big[5000] = 0;
    char *tx = tui_kitty_transmit(big, 9);
    CHECK(str_starts(tx, "\x1b_Ga=t,f=100,q=2,i=9,m=1;AAAA"));
    CHECK(strstr(tx, "\x1b\\\x1b_Gq=2,m=0;AAAA") != NULL);
    free(tx);
    CHECK(eq_free(tui_kitty_placement(7, 1, 80, 20), "\x1b_Ga=p,q=2,C=1,i=7,p=1,c=80,r=20\x1b\\"));
    CHECK(eq_free(tui_kitty_encode("QUJD", 4, 2), "\x1b_Ga=T,f=100,q=2,C=1,c=4,r=2;QUJD\x1b\\"));
    CHECK(eq_free(tui_kitty_delete_all(), "\x1b_Ga=d,d=A,q=2\x1b\\"));
    strvec_t g = {0};
    tui_kitty_placeholder_lines(0x010203, 0x010203, 2, 2, &g);
    CHECK(g.n == 2);
    CHECK(g.n == 2 && !strcmp(g.v[0], "\x1b_Ga=p,U=1,q=2,i=66051,p=66051,c=2,r=2\x1b\\\x1b[38;2;1;2;3m\x1b[58:2::1:2:3m"
                                      "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\xf4\x8e\xbb\xae\xcc\x85\xcc\x8d\x1b[39;59m"));
    CHECK(g.n == 2 && !strcmp(g.v[1], "\x1b[38;2;1;2;3m\x1b[58:2::1:2:3m\xf4\x8e\xbb\xae\xcc\x8d\xcc\x85\xf4\x8e\xbb\xae\xcc\x8d"
                                      "\xcc\x8d\x1b[39;59m"));
    CHECK(g.n == 2 && tui_visible_width(g.v[1]) == 2);
    strvec_free(&g);
    /* inside tmux the APC rides the DCS passthrough envelope */
    setenv("TMUX", "/tmp/x,1,0", 1);
    CHECK(eq_free(tui_kitty_placement(7, 1, 2, 2), "\x1bPtmux;\x1b\x1b_Ga=p,q=2,C=1,i=7,p=1,c=2,r=2\x1b\x1b\\\x1b\\"));
    unsetenv("TMUX");

    CHECK(eq_free(tui_iterm2_encode("QUJD", 80, 20, true), "\x1b]1337;File=inline=1;width=80;height=20:QUJD\a"));

    /* SIXEL: a 12x12 image, left half red, right half blue; header, bands and pixels survive a decode */
    img_t im;
    CHECK(img_alloc(&im, 12, 12) == 0);
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 12; x++) {
            uint8_t *p = im.px + (y * 12 + x) * 4;
            p[0] = x < 6 ? 255 : 0, p[1] = 0, p[2] = x < 6 ? 0 : 255, p[3] = 255;
        }
    size_t pl = 0;
    uint8_t *png = img_png_encode(&im, -1, &pl);
    char *six = tui_sixel_encode(png, pl, 12, 12, 0);
    CHECK(six && str_starts(six, "\x1bP9;1;0q\"1;1;12;12#0;2;"));
    CHECK(six && str_ends(six, "$-\x1b\\"));
    sixel_img_t d;
    if (six && sixel_decode(six, &d)) {
        CHECK(d.w == 12 && d.h == 12);
        int l = d.px[0], rr = d.px[11];
        CHECK(l >= 0 && rr >= 0 && d.pal[l][0] == 100 && d.pal[l][2] == 0 && d.pal[rr][2] == 100 && d.pal[rr][0] == 0);
        bool full = true;
        for (int i = 0; i < 144; i++) full &= d.px[i] >= 0;
        CHECK(full);
        free(d.px);
    } else CHECK(false);
    free(six);
    /* a 7 px tall image keeps whole 6 px bands only (band_rows) */
    six = tui_sixel_encode(png, pl, 12, 7, 0);
    CHECK(six && str_starts(six, "\x1bP9;1;0q\"1;1;12;6#"));
    free(six);
    free(png);
    img_free(&im);
    CHECK(eq_free(tui_sixel_ensure_raster_width(xstrdup("\x1bP9;1;0q\"1;1;5;12#0;2;0;0;0-#0~\x1b\\")),
                  "\x1bP9;1;0q\"1;1;5;12#0;2;0;0;0!5?$-#0~\x1b\\"));
    CHECK(eq_free(tui_sixel_ensure_raster_width(xstrdup("\x1bP9;1;0q\"1;1;5;6#0;2;0;0;0#0~$-\x1b\\")),
                  "\x1bP9;1;0q\"1;1;5;6#0;2;0;0;0#0~$-\x1b\\"));

    /* image lines and the placement lift */
    tui_set_image_protocol(TUI_IMG_KITTY);
    CHECK(tui_is_image_line("\x1b" "7\x1b[19A\x1b_Ga=p,q=2,C=1,i=1\x1b\\\x1b" "8"));
    CHECK(tui_is_image_line("\x1b[38;2;1;2;3m\xf4\x8e\xbb\xae\xcc\x85"));
    CHECK(!tui_is_image_line("plain \x1b[1mtext"));
    CHECK(tui_image_lift("\x1b" "7\x1b[19A\x1b_G") == 19 && tui_image_lift("abc") == 0);
    tui_set_image_protocol(TUI_IMG_SIXEL);
    CHECK(tui_is_image_line("\x1bP9;1;0q\"1;1;2;6#0~\x1b\\"));
    tui_set_image_protocol(TUI_IMG_ITERM2);
    CHECK(tui_is_image_line("\x1b]1337;File=inline=1:QQ\a"));
    tui_set_image_protocol(TUI_IMG_NONE);
    CHECK(!tui_is_image_line("\x1b]1337;File=inline=1:QQ\a"));
    /* without a protocol an image renders its fallback text */
    strvec_t rows = {0};
    tui_image_render("iVBORw0KGgo=", "image/png", "k", NULL, 80, NULL, "", &rows);
    CHECK(rows.n == 1 && !strcmp(rows.v[0], "[Image: [image/png]]"));
    strvec_free(&rows);
}

static void test_protocols(void) {
    clear_env();
    CHECK(tui_resolve_image_protocol("kitty", true) == TUI_IMG_KITTY);
    CHECK(tui_resolve_image_protocol("rex", false) == TUI_IMG_KITTY); /* omp c76e737a: Kitty transport, other caps stay unassumed */
    CHECK(tui_hyperlinks_detect("rex")); /* omp 5d7135364c: Rex renders OSC 8 links */
    CHECK(tui_resolve_image_protocol("ghostty", false) == TUI_IMG_KITTY);
    CHECK(tui_resolve_image_protocol("iterm2", true) == TUI_IMG_ITERM2);
    CHECK(tui_resolve_image_protocol("vscode", true) == TUI_IMG_NONE);
    CHECK(tui_resolve_image_protocol("base", true) == TUI_IMG_NONE);
    setenv("TERM", "xterm-ghostty", 1);
    CHECK(tui_resolve_image_protocol("base", true) == TUI_IMG_KITTY);
    CHECK(tui_resolve_image_protocol("base", false) == TUI_IMG_NONE);
    unsetenv("TERM");
    setenv("PASEO_TERMINAL_ID", "1", 1);
    CHECK(tui_resolve_image_protocol("kitty", true) == TUI_IMG_NONE);
    unsetenv("PASEO_TERMINAL_ID");
    CHECK(tui_kitty_placeholders_detect("kitty") && tui_kitty_placeholders_detect("ghostty"));
    CHECK(!tui_kitty_placeholders_detect("wezterm"));
    setenv("PI_NO_KITTY_PLACEHOLDERS", "1", 1);
    CHECK(!tui_kitty_placeholders_detect("kitty"));
    unsetenv("PI_NO_KITTY_PLACEHOLDERS");
    setenv("PI_KITTY_PLACEHOLDERS", "1", 1);
    CHECK(tui_kitty_placeholders_detect("wezterm"));
    unsetenv("PI_KITTY_PLACEHOLDERS");

    /* terminal replies: cell size, SIXEL attribute in DA1 */
    bool changed = false;
    tui_set_image_protocol(TUI_IMG_KITTY);
    CHECK(tui_termcaps_response("\x1b[6;32;16t", 11, &changed) && changed);
    int w, h;
    tui_cell_dims(&w, &h);
    CHECK(w == 16 && h == 32);
    CHECK(tui_termcaps_response("\x1b[48;40;100;800;900t", 20, &changed));
    tui_cell_dims(&w, &h);
    CHECK(w == 9 && h == 20);
    tui_set_image_protocol(TUI_IMG_NONE);
    CHECK(!tui_termcaps_response("\x1b[?1;2;4c", 9, &changed) && changed && tui_image_protocol() == TUI_IMG_SIXEL);
    tui_set_image_protocol(TUI_IMG_NONE);
    CHECK(!tui_termcaps_response("\x1b[?1;2c", 7, &changed) && !changed && tui_image_protocol() == TUI_IMG_NONE);
    CHECK(tui_termcaps_response("\x1b[?2;0;1000;1000S", 17, &changed) && changed && tui_image_protocol() == TUI_IMG_SIXEL);
    tui_set_image_protocol(TUI_IMG_NONE);
}

static void test_hyperlinks(void) {
    clear_env();
    CHECK(tui_hyperlinks_detect("kitty") && tui_hyperlinks_detect("vscode"));
    CHECK(!tui_hyperlinks_detect("warp") && !tui_hyperlinks_detect("base"));
    setenv("STY", "1.pts", 1);
    CHECK(!tui_hyperlinks_detect("kitty"));
    unsetenv("STY");
    setenv("TMUX", "/tmp/x", 1);
    setenv("TERM_PROGRAM", "tmux", 1);
    setenv("TERM_PROGRAM_VERSION", "3.4", 1);
    CHECK(tui_hyperlinks_detect("kitty"));
    setenv("TERM_PROGRAM_VERSION", "3.3a", 1);
    CHECK(!tui_hyperlinks_detect("kitty"));
    clear_env();
    setenv("TERM", "screen-256color", 1);
    CHECK(!tui_hyperlinks_detect("kitty"));
    setenv("PI_FORCE_HYPERLINKS", "1", 1);
    CHECK(tui_hyperlinks_detect("base"));
    setenv("PI_NO_HYPERLINKS", "1", 1);
    CHECK(!tui_hyperlinks_detect("kitty"));
    clear_env();
    tui_hyperlinks_apply_setting("off");
    CHECK(!tui_hyperlinks_enabled());
    tui_hyperlinks_apply_setting("always");
    CHECK(tui_hyperlinks_enabled());
    tui_hyperlinks_apply_setting("bogus"); /* unknown keeps the last policy */
    CHECK(tui_hyperlinks_enabled());
}

static void test_cache_marker(void) {
    json_t *prev = json_parse_str("{\"input\":10,\"cacheRead\":50000,\"cacheWrite\":0}");
    json_t *cold = json_parse_str("{\"input\":900,\"cacheRead\":0,\"cacheWrite\":50000}");
    json_t *warm = json_parse_str("{\"input\":10,\"cacheRead\":50000,\"cacheWrite\":200}");
    json_t *implicit = json_parse_str("{\"input\":50000,\"cacheRead\":0,\"cacheWrite\":0}");
    json_t *small = json_parse_str("{\"input\":10,\"cacheRead\":1000,\"cacheWrite\":0}");
    CHECK(ui_detect_cache_invalidation(prev, cold) == 50900);
    CHECK(ui_detect_cache_invalidation(prev, warm) == 0);
    CHECK(ui_detect_cache_invalidation(prev, implicit) == 0);
    CHECK(ui_detect_cache_invalidation(small, cold) == 0);
    CHECK(ui_detect_cache_invalidation(NULL, cold) == 0);
    /* a cold turn can still read the shared system block back: under half of what the previous turn read is a miss */
    json_t *wide = json_parse_str("{\"input\":10,\"cacheRead\":300000,\"cacheWrite\":0}");
    json_t *shared = json_parse_str("{\"input\":100,\"cacheRead\":15000,\"cacheWrite\":285000}");
    json_t *half = json_parse_str("{\"input\":100,\"cacheRead\":150000,\"cacheWrite\":150000}");
    CHECK(ui_detect_cache_invalidation(wide, shared) == 285100);
    CHECK(ui_detect_cache_invalidation(wide, half) == 0);
    json_free(wide);
    json_free(shared);
    json_free(half);
    char *row = ui_cache_miss_divider(50900, 80);
    char *plain = tui_strip_ansi(row);
    CHECK(eq_free(plain, "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                         "\xe2\x94\x80\xe2\x94\x80 \xe2\x8a\x98 cache miss \xc2\xb7 51K tokens"));
    free(row);
    row = ui_cache_miss_divider(50900, 20); /* too narrow to frame: the bare label */
    plain = tui_strip_ansi(row);
    CHECK(eq_free(plain, "\xe2\x8a\x98 cache miss \xc2\xb7 51K tokens"));
    free(row);
    json_free(prev), json_free(cold), json_free(warm), json_free(implicit), json_free(small);
}

static void test_tokrate(void) {
    tokrate_t m;
    tokrate_init(&m);
    CHECK(tokrate_rate(&m, 0) < 0);
    /* ~100 tok/s streamed for 5 s (39-byte deltas every 100 ms, ~10 estimated tokens each), billed exactly */
    tokrate_begin(&m, 1000);
    for (int i = 0; i < 50; i++) tokrate_push(&m, "abcd abcd abcd abcd abcd abcd abcd abcd", 39, 1000 + i * 100);
    double live = tokrate_rate(&m, 6000);
    CHECK(live > 80 && live < 120);
    tokrate_end(&m, 500, 6000);
    double r = tokrate_rate(&m, 6000);
    CHECK(fabs(r - 100) < 5);
    /* between messages the reading holds (tool time is not stream time) */
    CHECK(fabs(tokrate_rate(&m, 60000) - r) < 1e-9);
    tokrate_reset(&m);
    CHECK(tokrate_rate(&m, 6000) < 0);
    /* seed: a short finished turn reads at once */
    tokrate_seed(&m, 50, 1000);
    CHECK(fabs(tokrate_rate(&m, 0) - 50) < 1e-6);
    tokrate_seed(&m, 0, 1000);
    CHECK(tokrate_rate(&m, 0) < 0);
    tokrate_destroy(&m);
}

int main(void) {
    test_images();
    test_protocols();
    test_hyperlinks();
    test_cache_marker();
    test_tokrate();
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
