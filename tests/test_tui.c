/* test_tui.c - unit tests for src/tui */
#include "core/core.h"
#include "core/fs.h"
#include "core/json.h"
#include "tui/ansi.h"
#include "tui/components.h"
#include "tui/editor.h"
#include "tui/fuzzy.h"
#include "tui/keys.h"
#include "tui/markdown.h"
#include "tui/render.h"
#include "tui/term.h"
#include "tui/theme.h"
#include "tui/symbols.h"

#include <fcntl.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_STR(a, b)                                                                     \
    do {                                                                                    \
        checks++;                                                                           \
        const char *a_ = (a), *b_ = (b);                                                    \
        if (!str_eq(a_, b_)) {                                                              \
            failures++;                                                                     \
            fprintf(stderr, "%s:%d: expected \"%s\" got \"%s\"\n", __FILE__, __LINE__, b_, a_ ? a_ : "(null)"); \
        }                                                                                   \
    } while (0)

/* ---------- keys ---------- */

static void key_id_of(const char *bytes, char *buf, size_t cap) {
    tui_key_t k[4];
    size_t n = tui_keys_decode_all(bytes, strlen(bytes), k, 4);
    buf[0] = 0;
    if (n >= 1) tui_key_id(&k[0], buf, cap);
    for (size_t i = 0; i < n; i++) tui_key_free(&k[i]);
}

static void test_keys(void) {
    char id[64];
    struct {
        const char *in, *id;
    } cases[] = {
        {"a", "a"},
        {"A", "shift+a"},
        {"\x03", "ctrl+c"},
        {"\x01", "ctrl+a"},
        {"\r", "enter"},
        {"\n", "ctrl+j"},
        {"\t", "tab"},
        {"\x7f", "backspace"},
        {"\x1b[A", "up"},
        {"\x1bOB", "down"},
        {"\x1b[1;5C", "ctrl+right"},
        {"\x1b[1;3D", "alt+left"},
        {"\x1b[3~", "delete"},
        {"\x1b[3;5~", "ctrl+delete"},
        {"\x1b[5~", "pageup"},
        {"\x1b[6~", "pagedown"},
        {"\x1b[H", "home"},
        {"\x1b[F", "end"},
        {"\x1b[1~", "home"},
        {"\x1bOP", "f1"},
        /* omp 79272682 / 42af8370: kitty sends an unmodified F1/F2/F4 as CSI P/Q/S; CSI 1 Q is equivalent */
        {"\x1b[P", "f1"},
        {"\x1b[Q", "f2"},
        {"\x1b[S", "f4"},
        {"\x1b[1Q", "f2"},
        {"\x1b[1;1Q", "f2"},
        {"\x1b[1;2Q", "shift+f2"},
        {"\x1b[20;5~", "ctrl+f9"},
        {"\x1b[6;5~", "ctrl+pagedown"},
        {"\x1b[15~", "f5"},
        {"\x1b[24~", "f12"},
        {"\x1b[Z", "shift+tab"},
        {"\x1b" "b", "alt+b"},
        {"\x1b\r", "alt+enter"},
        {"\x1b\x7f", "alt+backspace"},
        {"\x1b[13;2u", "shift+enter"},
        {"\x1b[99;5u", "ctrl+c"},
        {"\x1b[97;7u", "ctrl+alt+a"},
        {"\x1b[27;5;13~", "ctrl+enter"},
        {"\x1b[27u", "escape"},
        {"\x1b[127;5u", "ctrl+backspace"},
        {"\x1b[1;2A", "shift+up"},
        {"\x1f", "ctrl+_"},
        {"\x1b", "escape"},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        key_id_of(cases[i].in, id, sizeof id);
        CHECK_STR(id, cases[i].id);
    }

    /* utf-8 text */
    tui_key_t k[8];
    size_t n = tui_keys_decode_all("\xc3\xa9x", 3, k, 8);
    CHECK(n == 2);
    CHECK(k[0].kind == TUI_KEY_CHAR && k[0].codepoint == 0xE9);
    CHECK_STR(tui_key_text(&k[0]), "\xc3\xa9");
    for (size_t i = 0; i < n; i++) tui_key_free(&k[i]);

    /* kitty shifted letter carries uppercase text */
    n = tui_keys_decode_all("\x1b[97;2u", 7, k, 8);
    CHECK(n == 1 && tui_key_text(&k[0]) && !strcmp(tui_key_text(&k[0]), "A"));
    tui_key_id(&k[0], id, sizeof id);
    CHECK_STR(id, "shift+a");
    tui_key_free(&k[0]);

    /* release events are dropped */
    n = tui_keys_decode_all("\x1b[97;1:3u" "b", 10, k, 8);
    CHECK(n == 1 && k[0].codepoint == 'b');
    for (size_t i = 0; i < n; i++) tui_key_free(&k[i]);

    /* bracketed paste */
    const char *p = "x\x1b[200~hello\r\nworld\x1b[201~y";
    n = tui_keys_decode_all(p, strlen(p), k, 8);
    CHECK(n == 3);
    CHECK(k[1].kind == TUI_KEY_PASTE);
    CHECK_STR(k[1].text, "hello\r\nworld");
    CHECK(k[2].codepoint == 'y');
    for (size_t i = 0; i < n; i++) tui_key_free(&k[i]);

    /* focus + responses */
    n = tui_keys_decode_all("\x1b[I\x1b[?1u\x1b[?62;22c\x1b]11;rgb:0000/0000/0000\x07", 40, k, 8);
    CHECK(n == 4);
    CHECK(k[0].kind == TUI_KEY_FOCUS_IN);
    CHECK(k[1].kind == TUI_KEY_RESPONSE && k[2].kind == TUI_KEY_RESPONSE && k[3].kind == TUI_KEY_RESPONSE);
    for (size_t i = 0; i < n; i++) tui_key_free(&k[i]);

    /* split sequence across feeds + escape timeout */
    tui_keydec_t d;
    tui_keydec_init(&d);
    tui_key_t key;
    tui_keydec_feed(&d, "\x1b[", 2);
    CHECK(!tui_keydec_next(&d, &key, 1000));
    CHECK(tui_keydec_timeout_ms(&d, 1000) == TUI_ESC_TIMEOUT_MS);
    tui_keydec_feed(&d, "B", 1);
    CHECK(tui_keydec_next(&d, &key, 1001) && key.kind == TUI_KEY_DOWN);
    tui_key_free(&key);
    tui_keydec_feed(&d, "\x1b", 1);
    CHECK(!tui_keydec_next(&d, &key, 2000));
    CHECK(!tui_keydec_next(&d, &key, 2000 + TUI_ESC_TIMEOUT_MS - 1));
    CHECK(tui_keydec_next(&d, &key, 2000 + TUI_ESC_TIMEOUT_MS) && key.kind == TUI_KEY_ESCAPE);
    tui_key_free(&key);
    /* split paste */
    tui_keydec_feed(&d, "\x1b[200~abc", 9);
    CHECK(!tui_keydec_next(&d, &key, 3000));
    tui_keydec_feed(&d, "def\x1b[20", 7);
    CHECK(!tui_keydec_next(&d, &key, 3000));
    tui_keydec_feed(&d, "1~", 2);
    CHECK(tui_keydec_next(&d, &key, 3000) && key.kind == TUI_KEY_PASTE);
    CHECK_STR(key.text, "abcdef");
    tui_key_free(&key);
    tui_keydec_free(&d);

    /* matcher */
    n = tui_keys_decode_all("\x03", 1, k, 8);
    CHECK(tui_key_matches(&k[0], "ctrl+c"));
    CHECK(tui_key_matches(&k[0], "Ctrl+C") == false); /* uppercase letter implies shift */
    CHECK(tui_key_matches(&k[0], "control+c"));
    CHECK(!tui_key_matches(&k[0], "ctrl+d"));
    tui_key_free(&k[0]);
    n = tui_keys_decode_all("\x1b[1;6A", 6, k, 8);
    CHECK(tui_key_matches(&k[0], "shift+ctrl+up"));
    CHECK(tui_key_matches(&k[0], "ctrl+shift+up"));
    tui_key_free(&k[0]);
    n = tui_keys_decode_all("?", 1, k, 8);
    CHECK(tui_key_matches(&k[0], "?"));
    tui_key_free(&k[0]);

    char canon[64];
    CHECK(tui_key_canonical("Alt+Shift+P", canon, sizeof canon));
    CHECK_STR(canon, "shift+alt+p");
    CHECK(tui_key_canonical("esc", canon, sizeof canon));
    CHECK_STR(canon, "escape");
    CHECK(!tui_key_canonical("hyper+x", canon, sizeof canon));

    /* keybindings: defaults + JSON override */
    tui_keybindings_t *kb = tui_keybindings_new();
    n = tui_keys_decode_all("\n", 1, k, 8); /* ctrl+j; alt+enter is the editor's hard-coded fallback */
    CHECK(tui_keybindings_matches(kb, &k[0], "tui.input.newLine"));
    CHECK(!tui_keybindings_matches(kb, &k[0], "tui.input.submit"));
    tui_key_free(&k[0]);
    json_t *j = json_parse_str("{\"tui.input.submit\": [\"ctrl+s\", \"Enter\"], \"tui.editor.yank\": []}");
    CHECK(tui_keybindings_load_json(kb, j) == 2);
    json_free(j);
    n = tui_keys_decode_all("\x13", 1, k, 8);
    CHECK(tui_keybindings_matches(kb, &k[0], "tui.input.submit"));
    tui_key_free(&k[0]);
    n = tui_keys_decode_all("\x19", 1, k, 8);
    CHECK(!tui_keybindings_matches(kb, &k[0], "tui.editor.yank"));
    tui_key_free(&k[0]);
    tui_keybindings_free(kb);
}

/* ---------- ansi utils ---------- */

static void test_ansi(void) {
    CHECK(tui_visible_width("hello") == 5);
    CHECK(tui_visible_width("\x1b[31mhello\x1b[0m") == 5);
    CHECK(tui_visible_width("\xe6\x97\xa5\xe6\x9c\xac") == 4);
    CHECK(tui_visible_width("a" TUI_CURSOR_MARKER "b") == 2);
    CHECK(tui_visible_width("\x1b]8;;http://x\x07link\x1b]8;;\x07") == 4);
    CHECK(tui_visible_width("a\tb") == 2 + TUI_TAB_WIDTH);

    char *t = tui_truncate_to_width("hello world", 8, NULL);
    CHECK(tui_visible_width(t) == 8);
    char *plain = tui_strip_ansi(t);
    CHECK_STR(plain, "hello w\xe2\x80\xa6");
    free(plain);
    free(t);
    t = tui_truncate_to_width("short", 10, NULL);
    CHECK_STR(t, "short");
    free(t);
    t = tui_truncate_to_width("\x1b[1mbold text here\x1b[0m", 6, "");
    plain = tui_strip_ansi(t);
    CHECK_STR(plain, "bold t");
    CHECK(str_starts(t, "\x1b[1m"));
    free(plain);
    free(t);
    /* wide chars never split */
    t = tui_truncate_to_width("\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5", 5, "");
    CHECK(tui_visible_width(t) == 4);
    free(t);

    strvec_t w = {0};
    tui_wrap_text_with_ansi("the quick brown fox jumps", 10, &w);
    CHECK(w.n == 3);
    if (w.n == 3) {
        CHECK_STR(w.v[0], "the quick");
        CHECK_STR(w.v[1], "brown fox");
        CHECK_STR(w.v[2], "jumps");
    }
    strvec_free(&w);
    /* SGR state carried to the next line */
    tui_wrap_text_with_ansi("\x1b[31mred words wrap here\x1b[0m", 9, &w);
    CHECK(w.n >= 2);
    if (w.n >= 2) {
        CHECK(str_starts(w.v[1], "\x1b[31m"));
        CHECK(str_ends(w.v[0], "\x1b[0m"));
    }
    for (size_t i = 0; i < w.n; i++) CHECK(tui_visible_width(w.v[i]) <= 9);
    strvec_free(&w);
    /* long word broken */
    tui_wrap_text_with_ansi("abcdefghijklmnop", 5, &w);
    CHECK(w.n == 4);
    if (w.n == 4) CHECK_STR(w.v[3], "p");
    strvec_free(&w);
    /* explicit newlines */
    tui_wrap_text_with_ansi("a\n\nb", 10, &w);
    CHECK(w.n == 3);
    strvec_free(&w);

    char *pd = tui_pad_to_width("\x1b[1mab\x1b[0m", 5);
    CHECK(tui_visible_width(pd) == 5 && str_ends(pd, "   "));
    free(pd);

    char *sl = tui_slice_cols("\x1b[32mhello world\x1b[0m", 6, 5);
    char *sp = tui_strip_ansi(sl);
    CHECK_STR(sp, "world");
    CHECK(str_starts(sl, "\x1b[32m"));
    free(sp);
    free(sl);
    char *rt = tui_replace_tabs("a\tb");
    CHECK_STR(rt, "a   b");
    free(rt);
}

/* ---------- theme ---------- */

static void test_theme(void) {
    tui_theme_t *th = tui_theme_builtin("dark", TUI_COLOR_TRUECOLOR);
    CHECK(th != NULL);
    CHECK_STR(th->fg[TUI_TC_ACCENT], "\x1b[38;2;254;188;56m");
    CHECK_STR(th->fg[TUI_TC_TEXT], "\x1b[39m");
    CHECK_STR(th->fg[TUI_TC_STATUS_LINE_SEP], "\x1b[38;5;244m");
    CHECK(th->nspinner == 8);
    tui_theme_free(th);
    th = tui_theme_builtin("light", TUI_COLOR_256);
    CHECK(th && strstr(th->fg[TUI_TC_ACCENT], "38;5;"));
    tui_theme_free(th);
    CHECK(tui_rgb_to_256(255, 0, 0) == 196);
    CHECK(tui_rgb_to_256(128, 128, 128) == 244);
    CHECK(tui_theme_slot_from_name("mdHeading") == TUI_TC_MD_HEADING);
    char *err = NULL;
    json_t *j = json_parse_str("{\"name\":\"x\",\"colors\":{\"accent\":\"#ff0000\"}}");
    th = tui_theme_from_json(j, TUI_COLOR_TRUECOLOR, &err);
    CHECK(th == NULL && err && strstr(err, "missing required colors"));
    free(err);
    json_free(j);
}

/* ---------- markdown ---------- */

static strvec_t md_plain(const char *src, int width) {
    tui_md_opts_t o;
    tui_md_opts_default(&o);
    strvec_t lines = {0}, plain = {0};
    tui_md_render(src, width, &o, &lines);
    for (size_t i = 0; i < lines.n; i++) {
        CHECK(tui_visible_width(lines.v[i]) <= width);
        strvec_push_own(&plain, tui_strip_ansi(lines.v[i]));
    }
    strvec_free(&lines);
    return plain;
}

static bool has_line(const strvec_t *v, const char *s) {
    for (size_t i = 0; i < v->n; i++)
        if (!strcmp(v->v[i], s)) return true;
    return false;
}

static void test_markdown(void) {
    tui_theme_set(tui_theme_builtin("dark", TUI_COLOR_TRUECOLOR));
    const char *doc = "# Title\n"
                      "\n"
                      "Some **bold** and *italic* text with `code` and a [link](https://example.com).\n"
                      "\n"
                      "- item one\n"
                      "- item two\n"
                      "  - nested\n"
                      "1. first\n"
                      "2. second\n"
                      "\n"
                      "> quoted text\n"
                      "\n"
                      "```c\n"
                      "int main(void) { return 0; }\n"
                      "```\n"
                      "\n"
                      "| Name | Value |\n"
                      "|------|------:|\n"
                      "| a    | 1     |\n"
                      "\n"
                      "---\n"
                      "### Sub\n";
    strvec_t p = md_plain(doc, 60);
    CHECK(p.n > 10);
    CHECK(has_line(&p, "Title"));
    CHECK(has_line(&p, "Some bold and italic text with code and a link"));
    CHECK(has_line(&p, "(https://example.com)."));
    CHECK(has_line(&p, "- item one"));
    CHECK(has_line(&p, "  - nested"));
    CHECK(has_line(&p, "1. first"));
    CHECK(has_line(&p, "2. second"));
    CHECK(has_line(&p, "\xe2\x96\x8f quoted text"));
    CHECK(has_line(&p, "```c"));
    CHECK(has_line(&p, "  int main(void) { return 0; }"));
    CHECK(has_line(&p, "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac"
                       "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90"));
    CHECK(has_line(&p, "\xe2\x94\x82 Name \xe2\x94\x82 Value \xe2\x94\x82"));
    CHECK(has_line(&p, "\xe2\x94\x82 a    \xe2\x94\x82     1 \xe2\x94\x82"));
    CHECK(has_line(&p, "### Sub"));
    /* title followed by blank line */
    CHECK(p.n >= 2 && !strcmp(p.v[0], "Title") && !strcmp(p.v[1], ""));
    strvec_free(&p);

    /* raw output checks: bold SGR, OSC 8 link, keyword coloring */
    tui_md_opts_t o;
    tui_md_opts_default(&o);
    o.hyperlinks = true;
    strvec_t raw = {0};
    tui_md_render(doc, 60, &o, &raw);
    char *all = strvec_join(&raw, "\n");
    CHECK(strstr(all, "\x1b[1mbold") != NULL);
    CHECK(strstr(all, "\x1b]8;;https://example.com\a") != NULL);
    CHECK(strstr(all, "\x1b[38;2;86;156;214mreturn") != NULL); /* syntaxKeyword #569CD6 */
    free(all);
    strvec_free(&raw);

    /* streaming: unclosed fence renders without crashing */
    p = md_plain("text\n```python\ndef f():\n    pass", 40);
    CHECK(has_line(&p, "  def f():"));
    strvec_free(&p);

    /* wrapping keeps widths */
    p = md_plain("word word word word word word word word word word word word", 20);
    CHECK(p.n == 3);
    strvec_free(&p);

    /* narrow table falls back */
    p = md_plain("| a | b | c | d |\n|---|---|---|---|\n| 1 | 2 | 3 | 4 |", 8);
    CHECK(p.n >= 2);
    strvec_free(&p);

    /* deep nesting is bounded */
    sb_t deep = SB_INIT;
    for (int i = 0; i < 500; i++) sb_append(&deep, "> ");
    sb_append(&deep, "deep");
    p = md_plain(sb_cstr(&deep), 40);
    CHECK(p.n >= 1);
    strvec_free(&p);
    sb_clear(&deep);
    for (int i = 0; i < 300; i++) {
        sb_repeat(&deep, "  ", i);
        sb_append(&deep, "- x\n");
    }
    p = md_plain(sb_cstr(&deep), 2000);
    CHECK(p.n >= 1);
    strvec_free(&p);
    sb_free(&deep);

    /* emphasis edge cases */
    char *s = tui_md_inline("snake_case_name and a * b * c", NULL);
    char *sp = tui_strip_ansi(s);
    CHECK_STR(sp, "snake_case_name and a * b * c");
    free(sp);
    free(s);
}

/* ---------- golden transcript renders (captured from omp v18.13.3 under tmux, titanium theme) ----------
 * tests/tui/golden/<name>.ansi are `tmux capture-pane -e` cuts (tests/tui/mkgolden.py) of the reference app
 * rendering tests/tui/fixtures/<name>.md through the mock model. tmux re-encodes SGR, so both sides are
 * reduced to cells: every visible character tagged with its effective attributes and colors. */

/* one line of cells; `st` carries over between lines (tmux capture) or starts fresh (TUI lines) */
static char *cells(const char *line, tui_sgr_t *st) {
    sb_t b = SB_INIT;
    char key[160] = "", cur[160] = "";
    size_t n = strlen(line), last_visible = 0;
    sb_t out = SB_INIT;
    for (size_t i = 0; i < n;) {
        if (line[i] == 0x1b) {
            size_t l = tui_esc_len(line + i, n - i);
            if (!l) l = 1;
            tui_sgr_feed(st, line + i, l);
            i += l;
            continue;
        }
        uint32_t cp;
        int k = utf8_decode(line + i, n - i, &cp);
        snprintf(key, sizeof key, "%s%s%s%s%s%s|%s|%s", st->bold ? "b" : "", st->dim ? "d" : "", st->italic ? "i" : "",
                 st->underline ? "u" : "", st->strike ? "s" : "", st->inverse ? "r" : "", st->fg, st->bg);
        if (strcmp(key, cur)) {
            sb_appendf(&b, "{%s}", key);
            snprintf(cur, sizeof cur, "%s", key);
        }
        sb_appendn(&b, line + i, (size_t)k);
        if (cp != ' ') last_visible = b.len;
        i += (size_t)k;
    }
    sb_appendn(&out, b.s ? b.s : "", last_visible); /* tmux drops trailing blank cells */
    sb_free(&b);
    return sb_detach(&out);
}

static void check_golden(const char *name, const strvec_t *got) {
    char *path = xasprintf("tests/tui/golden/%s.ansi", name);
    char *want = fs_read(path, NULL);
    CHECK(want != NULL);
    if (!want) {
        free(path);
        return;
    }
    strvec_t wl = str_split_lines(want, strlen(want));
    while (wl.n && !wl.v[wl.n - 1][0]) free(vec_pop(&wl));
    tui_sgr_t ws;
    tui_sgr_reset(&ws);
    size_t n = MAX(wl.n, got->n);
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        char *a = i < wl.n ? cells(wl.v[i], &ws) : xstrdup("<missing>");
        tui_sgr_t gs;
        tui_sgr_reset(&gs);
        char *b = i < got->n ? cells(got->v[i], &gs) : xstrdup("<missing>");
        if (strcmp(a, b) && bad++ < 3) fprintf(stderr, "golden %s line %zu:\n  want %s\n  got  %s\n", name, i + 1, a, b);
        CHECK(!strcmp(a, b));
        free(a);
        free(b);
    }
    strvec_free(&wl);
    free(want);
    free(path);
}

static strvec_t render_fixture(const char *name, tui_md_opts_t *o, int width) {
    char *path = xasprintf("tests/tui/fixtures/%s.md", name);
    char *src = fs_read(path, NULL);
    CHECK(src != NULL);
    strvec_t out = {0};
    if (src) {
        char *t = str_trim(src);
        tui_md_render(t, width, o, &out);
        free(t);
    }
    free(src);
    free(path);
    return out;
}

/* tui.colorfulMarkdown (colorful-markdown.test.ts): off keeps mdHeading/mdCode and uncolored emphasis */
static void test_colorful_markdown(void) {
    char *err = NULL;
    tui_theme_t *th = tui_theme_load("titanium", TUI_COLOR_TRUECOLOR, &err);
    CHECK(th != NULL);
    free(err);
    if (!th) return;
    tui_theme_set(th);
    tui_md_opts_t o;
    tui_md_opts_default(&o);
    o.hyperlinks = false;
    const char *doc = "## Two\n\n**loud `code` tail** *soft*";
    const char *magenta = tui_ansi_fg_seq(NULL, TUI_ANSI_MAGENTA);
    strvec_t on = {0}, off = {0};
    tui_md_render(doc, 60, &o, &on);
    char *j = strvec_join(&on, "\n");
    CHECK(strstr(j, tui_ansi_fg_seq(NULL, TUI_ANSI_YELLOW)) != NULL);
    CHECK(strstr(j, magenta) != NULL);
    CHECK(strstr(j, tui_ansi_fg_seq(NULL, TUI_ANSI_GREEN)) != NULL);
    free(j);
    tui_md_set_colorful(false);
    tui_md_render(doc, 60, &o, &off);
    tui_md_set_colorful(true);
    j = strvec_join(&off, "\n");
    CHECK(strstr(j, tui_fg_seq(NULL, TUI_TC_MD_HEADING)) != NULL);
    CHECK(strstr(j, tui_fg_seq(NULL, TUI_TC_MD_CODE)) != NULL);
    CHECK(strstr(j, magenta) == NULL);
    free(j);
    strvec_free(&on);
    strvec_free(&off);
    /* a theme without terminal.ansi falls back to the terminal's own palette */
    char *src = fs_read("assets/tui/themes/dark.json", NULL);
    CHECK(src != NULL);
    json_t *bare = src ? json_parse(src, strlen(src), NULL) : NULL;
    CHECK(bare != NULL);
    if (bare) {
        tui_theme_t *one = tui_theme_from_json(bare, TUI_COLOR_TRUECOLOR, NULL);
        CHECK(one && !strcmp(one->ansi_fg[5], "\x1b[38;2;188;63;188m"));
        tui_theme_free(one);
        json_del(bare, "terminal");
        one = tui_theme_from_json(bare, TUI_COLOR_TRUECOLOR, NULL);
        CHECK(one && !strcmp(one->ansi_fg[5], "\x1b[38;5;5m"));
        tui_theme_free(one);
        json_free(bare);
    }
    free(src);
    /* a thinking block's forced run color stays over the strong role */
    tui_md_opts_t t = o;
    t.text_slot = TUI_TC_THINKING_TEXT;
    strvec_t th_out = {0};
    tui_md_render("**x**", 60, &t, &th_out);
    j = strvec_join(&th_out, "\n");
    CHECK(strstr(j, magenta) == NULL);
    free(j);
    strvec_free(&th_out);
}

static void test_golden(void) {
    char *err = NULL;
    tui_theme_t *th = tui_theme_load("titanium", TUI_COLOR_TRUECOLOR, &err);
    CHECK(th != NULL);
    free(err);
    if (!th) return;
    tui_theme_set(th);
    /* assistant prose: Markdown(text, 1, 0, theme, undefined, codeBlockIndent 0) */
    tui_md_opts_t o;
    tui_md_opts_default(&o);
    o.hyperlinks = false;
    o.padding_x = 1;
    o.code_indent = 0;
    const char *const names[] = {"basic", "code", "lists"};
    for (size_t i = 0; i < ARRAY_LEN(names); i++) {
        strvec_t got = render_fixture(names[i], &o, 100);
        check_golden(names[i], &got);
        strvec_free(&got);
    }
    /* thinking block (thinkingText, italic, default code indent), a spacer row, then the answer text */
    strvec_t got = {0};
    tui_md_opts_t t;
    tui_md_opts_default(&t);
    t.hyperlinks = false;
    t.padding_x = 1;
    t.text_slot = TUI_TC_THINKING_TEXT;
    t.italic = true;
    tui_md_render("Let me consider **this** carefully.\n\nSecond paragraph of thought.", 100, &t, &got);
    strvec_push(&got, "");
    tui_md_render("The answer is *42*.", 100, &o, &got);
    check_golden("think", &got);
    strvec_free(&got);
    /* user bubble: Markdown(text, 1, 1) on userMessageBg, prose in the contrast color of the bubble */
    tui_md_opts_t u;
    tui_md_opts_default(&u);
    u.hyperlinks = false;
    u.padding_x = 1;
    u.bg_slot = TUI_TC_USER_MESSAGE_BG;
    u.text_sgr = "\x1b[38;2;229;229;231m";
    got = (strvec_t){0};
    strvec_push_own(&got, tui_bgs(tui_theme(), TUI_TC_USER_MESSAGE_BG, "                                        "));
    tui_md_render("think:Let me consider **this** carefully.\\n\\nSecond paragraph of thought.|The answer is *42*.", 100,
                  &u, &got);
    check_golden("user", &got);
    strvec_free(&got);
    /* code highlighting by itself: unsupported languages come back verbatim */
    strvec_t hl = {0};
    tui_highlight_code("plain text", "no-such-lang", tui_theme(), &hl);
    CHECK(hl.n == 1 && !strcmp(hl.v[0], "plain text"));
    strvec_free(&hl);
    tui_theme_set(NULL);
}

/* ---------- editor ---------- */

static void feed(tui_editor_t *ed, const char *bytes) {
    tui_key_t k[256];
    size_t n = tui_keys_decode_all(bytes, strlen(bytes), k, 256);
    for (size_t i = 0; i < n; i++) {
        tui_editor_handle_key(ed, &k[i]);
        tui_key_free(&k[i]);
    }
}

static void check_text(tui_editor_t *ed, const char *expect) {
    char *t = tui_editor_get_text(ed);
    CHECK_STR(t, expect);
    free(t);
}

static char *g_submitted;
static void on_submit(tui_editor_t *ed, const char *text, void *ud) {
    free(g_submitted);
    g_submitted = xstrdup(text);
}

static bool fake_provider(void *ud, const char *before, bool first_line, bool forced, tui_ac_result_t *out) {
    if (first_line && before[0] == '/' && !strchr(before, ' ')) {
        tui_ac_result_add(out, "/help ", "/help", "Show help");
        tui_ac_result_add(out, "/model ", "/model", "Pick model");
        out->prefix_len = strlen(before);
        return true;
    }
    return false;
}

static void test_editor(void) {
    tui_editor_t *ed = tui_editor_new();
    tui_editor_set_on_submit(ed, on_submit, NULL);
    feed(ed, "hello world");
    check_text(ed, "hello world");
    CHECK(ed->cc == 11);
    feed(ed, "\x17"); /* ctrl+w */
    check_text(ed, "hello ");
    feed(ed, "\x1f"); /* undo */
    check_text(ed, "hello world");
    feed(ed, "\x01"); /* ctrl+a */
    CHECK(ed->cc == 0);
    feed(ed, "\x1b" "f"); /* alt+f */
    CHECK(ed->cc == 5);
    feed(ed, "\x0b"); /* ctrl+k */
    check_text(ed, "hello");
    feed(ed, "\x19"); /* ctrl+y */
    check_text(ed, "hello world");
    feed(ed, "\x05\x1b" "b"); /* ctrl+e, alt+b */
    CHECK(ed->cc == 6);
    feed(ed, "\x1b" "d"); /* alt+d */
    check_text(ed, "hello ");
    feed(ed, "\x15"); /* ctrl+u */
    check_text(ed, "");
    /* multi-line via shift+enter (kitty) and alt+enter */
    feed(ed, "ab\x1b[13;2u" "cd\x1b\r" "ef");
    check_text(ed, "ab\ncd\nef");
    CHECK(ed->cl == 2 && ed->cc == 2);
    feed(ed, "\x1b[A"); /* up */
    CHECK(ed->cl == 1 && ed->cc == 2);
    feed(ed, "\x1b[D\x1b[D\x7f"); /* left left backspace joins lines */
    check_text(ed, "abcd\nef");
    CHECK(ed->cl == 0 && ed->cc == 2);
    feed(ed, "\x1b[3~"); /* delete */
    check_text(ed, "abd\nef");
    /* utf-8 backspace */
    tui_editor_clear(ed);
    feed(ed, "h\xc3\xa9\xe6\x97\xa5");
    feed(ed, "\x7f");
    check_text(ed, "h\xc3\xa9");
    /* submit */
    feed(ed, "\r");
    CHECK_STR(g_submitted, "h\xc3\xa9");
    CHECK(tui_editor_is_empty(ed));
    /* history */
    feed(ed, "second\r");
    feed(ed, "\x1b[A");
    check_text(ed, "second");
    feed(ed, "\x1b[A");
    check_text(ed, "h\xc3\xa9");
    feed(ed, "\x1b[B\x1b[B");
    check_text(ed, "");
    /* large paste collapses into a marker */
    sb_t big = SB_INIT;
    sb_append(&big, "\x1b[200~");
    for (int i = 0; i < 15; i++) sb_appendf(&big, "line %d\n", i);
    sb_append(&big, "\x1b[201~");
    feed(ed, "x ");
    feed(ed, sb_cstr(&big));
    sb_free(&big);
    check_text(ed, "x [Paste #1, +16 lines]");
    char *exp = tui_editor_get_expanded_text(ed);
    CHECK(str_starts(exp, "x line 0\nline 1\n") && strstr(exp, "line 14\n"));
    free(exp);
    feed(ed, "\x7f"); /* atomic delete */
    check_text(ed, "x ");
    /* small paste inserted verbatim, CRLF normalized */
    feed(ed, "\x1b[200~a\r\nb\x1b[201~");
    check_text(ed, "x a\nb");
    /* word wrap layout and cursor marker */
    tui_editor_clear(ed);
    feed(ed, "aaaa bbbb cccc dddd");
    ed->base.focused = true;
    strvec_t out = {0};
    tui_component_render(&ed->base, 12, &out);
    CHECK(out.n == 4); /* border, 2 content rows, border */
    int marker_row = -1;
    for (size_t i = 0; i < out.n; i++) {
        CHECK(tui_visible_width(out.v[i]) <= 12);
        if (strstr(out.v[i], TUI_CURSOR_MARKER)) marker_row = (int)i;
    }
    CHECK(marker_row == 2);
    if (out.n == 4) {
        char *pl = tui_strip_ansi(out.v[1]);
        CHECK(str_starts(pl, "aaaa bbbb "));
        free(pl);
    }
    strvec_free(&out);
    /* up moves by visual line */
    feed(ed, "\x1b[A");
    CHECK(ed->cl == 0 && ed->cc == 9);
    /* autocomplete */
    tui_editor_clear(ed);
    tui_editor_set_autocomplete(ed, fake_provider, NULL);
    feed(ed, "/m");
    CHECK(tui_editor_autocomplete_open(ed));
    feed(ed, "\x1b[B\t"); /* down, tab accept */
    check_text(ed, "/model ");
    CHECK(!tui_editor_autocomplete_open(ed));
    tui_component_destroy(&ed->base);
    free(g_submitted);
    g_submitted = NULL;
}

/* ---------- select list ---------- */

static void test_select(void) {
    tui_select_list_t *l = tui_select_list_new(3);
    tui_select_list_add(l, "apple", NULL, "fruit");
    tui_select_list_add(l, "banana", NULL, NULL);
    tui_select_list_add(l, "cherry", NULL, NULL);
    tui_select_list_add(l, "date", NULL, NULL);
    strvec_t out = {0};
    tui_component_render(&l->base, 50, &out);
    CHECK(out.n == 4); /* 3 items + scroll status */
    strvec_free(&out);
    tui_key_t k[4];
    size_t n = tui_keys_decode_all("an", 2, k, 4);
    for (size_t i = 0; i < n; i++) {
        tui_component_handle_key(&l->base, &k[i]);
        tui_key_free(&k[i]);
    }
    CHECK(tui_select_list_visible_count(l) == 1);
    CHECK_STR(tui_select_list_selected(l)->value, "banana");
    tui_select_list_set_filter(l, NULL); /* upstream setQuery retains the selected item */
    CHECK_STR(tui_select_list_selected(l)->value, "banana");
    tui_select_list_move(l, -1, true);
    CHECK_STR(tui_select_list_selected(l)->value, "apple");
    tui_select_list_move(l, -1, true); /* wraps */
    CHECK_STR(tui_select_list_selected(l)->value, "date");
    tui_component_destroy(&l->base);
    CHECK(tui_fuzzy_score("mdl", "model") > 0);
    CHECK(tui_fuzzy_score("xz", "model") < 0);
}

/* upstream fuzzy.ts ranking and SelectList layout (setup wizard / overlays) */
static void test_fuzzy_select(void) {
    const char *names[] = {"ChatGPT Plus/Pro (Codex Subscription) openai-codex", "Anthropic (Claude Pro/Max) anthropic",
                           "OpenRouter openrouter", "OpenCode Zen opencode-zen", "vLLM (Local OpenAI-compatible) vllm",
                           "Kimi Code kimi-code"};
    size_t idx[6];
    size_t n = tui_fuzzy_rank("open", names, 6, idx);
    CHECK(n == 4);
    CHECK(idx[0] == 2 && idx[1] == 3); /* whole-word prefix hits first, registry order kept on ties */
    CHECK(tui_fuzzy_rank("", names, 6, idx) == 6 && idx[5] == 5);
    char *norm = tui_fuzzy_normalize("parseHTTPResponse_v2");
    CHECK_STR(norm, "parse http response v2");
    free(norm);
    tui_fuzzy_query_t *q = tui_fuzzy_query_new("gpt4");
    CHECK(tui_fuzzy_query_match(q, "gpt-4o", NULL)); /* compact phrase across the dash */
    tui_fuzzy_query_free(q);

    /* glyph-scene list: cursor + label padded to the 32-col primary column, muted description */
    tui_select_list_t *l = tui_select_list_new(3);
    tui_select_list_add(l, "nerd", "1  Nerd Font", "a b");
    tui_select_list_add(l, "unicode", "2  Unicode", "c d");
    tui_select_list_set_selected(l, 1);
    strvec_t out = {0};
    tui_component_render(&l->base, 80, &out);
    CHECK(out.n == 2); /* no overflow: no search status row, no scrollbar */
    char *r0 = tui_strip_ansi(out.v[0]), *r1 = tui_strip_ansi(out.v[1]);
    CHECK(str_starts(r0, "  1 Nerd Font"));
    CHECK(strstr(r0, "a b") == r0 + 2 + 32); /* sanitized label, description at prefix + 32 */
    CHECK(str_starts(r1, "\xe2\x9d\xaf 2 Unicode"));
    free(r0);
    free(r1);
    strvec_free(&out);
    tui_component_destroy(&l->base);

    /* overflow: scrollbar column + "Type to search" */
    l = tui_select_list_new(2);
    for (int i = 0; i < 5; i++) {
        char v[8];
        snprintf(v, sizeof v, "item%d", i);
        tui_select_list_add(l, v, NULL, NULL);
    }
    tui_component_render(&l->base, 40, &out);
    CHECK(out.n == 3);
    char *bar = tui_strip_ansi(out.v[0]), *st = tui_strip_ansi(out.v[2]);
    CHECK(str_ends(bar, "\xe2\x96\x88")); /* thumb at the top */
    CHECK_STR(st, "  Type to search");
    free(bar);
    free(st);
    strvec_free(&out);
    tui_component_destroy(&l->base);
}

/* ---------- differential renderer ---------- */

static char *drain_fd(int fd) {
    sb_t b = SB_INIT;
    char buf[4096];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) sb_appendn(&b, buf, (size_t)r);
    return sb_detach(&b);
}

static void test_renderer(void) {
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    tui_term_t *tm = tui_term_new(-1, pfd[1]);
    tui_term_set_size(tm, 40, 10);
    tui_t *t = tui_new(tm);
    tui_text_t *a = tui_text_new("line one", 0, 0);
    tui_text_t *b = tui_text_new("line two", 0, 0);
    tui_text_t *c = tui_text_new("line three", 0, 0);
    tui_container_add(tui_root(t), &a->base);
    tui_container_add(tui_root(t), &b->base);
    tui_container_add(tui_root(t), &c->base);
    tui_render_now(t);
    char *o1 = drain_fd(pfd[0]);
    CHECK(strstr(o1, "line one") && strstr(o1, "line two") && strstr(o1, "line three"));
    free(o1);

    /* no change: nothing but sync/cursor bookkeeping */
    tui_render_now(t);
    char *o0 = drain_fd(pfd[0]);
    CHECK(!strstr(o0, "line"));
    free(o0);

    /* one-line change: only that line is rewritten */
    tui_text_set(b, "line 2!");
    tui_render_now(t);
    char *o2 = drain_fd(pfd[0]);
    CHECK(strstr(o2, "line 2!") != NULL);
    CHECK(!strstr(o2, "line one") && !strstr(o2, "line three"));
    CHECK(str_count(o2, "\x1b[K") == 1 && !strstr(o2, "\x1b[2K")); /* written, then erased to EOL */
    CHECK(strstr(o2, "\x1b[1A") != NULL); /* moved up one row from the last line */
    CHECK(strlen(o2) < 80);
    free(o2);

    /* append */
    tui_text_t *d = tui_text_new("line four", 0, 0);
    tui_container_add(tui_root(t), &d->base);
    tui_render_now(t);
    char *o3 = drain_fd(pfd[0]);
    CHECK(strstr(o3, "line four") && !strstr(o3, "line three"));
    free(o3);
    CHECK(tui_prev_lines(t)->n == 4);

    /* shrink: trailing lines cleared */
    tui_container_remove(tui_root(t), &d->base);
    tui_component_destroy(&d->base);
    tui_render_now(t);
    char *o4 = drain_fd(pfd[0]);
    CHECK(strstr(o4, "\x1b[2K") != NULL && !strstr(o4, "line"));
    free(o4);
    CHECK(tui_prev_lines(t)->n == 3);

    /* scroll content past the viewport, then change a line in scrollback: committed history is
     * never repainted (omp), so no clear and nothing written */
    for (int i = 0; i < 12; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "extra %d", i);
        tui_container_add(tui_root(t), &tui_text_new(buf, 0, 0)->base);
    }
    tui_render_now(t);
    free(drain_fd(pfd[0]));
    int fulls = tui_full_redraw_count(t);
    tui_text_set(a, "changed first");
    tui_render_now(t);
    char *o5 = drain_fd(pfd[0]);
    CHECK(tui_full_redraw_count(t) == fulls);
    CHECK(!strstr(o5, "\x1b[3J") && !strstr(o5, "changed first"));
    free(o5);

    /* overlay composited over the bottom of the viewport */
    tui_select_list_t *sl = tui_select_list_new(3);
    tui_select_list_add(sl, "opt-a", NULL, NULL);
    tui_select_list_add(sl, "opt-b", NULL, NULL);
    tui_overlay_opts_t oo = {TUI_ANCHOR_BOTTOM, 20, 0, 0, 0};
    tui_overlay_show(t, &sl->base, &oo);
    tui_render_now(t);
    char *o6 = drain_fd(pfd[0]);
    CHECK(strstr(o6, "opt-a") && strstr(o6, "opt-b"));
    free(o6);
    const strvec_t *pl = tui_prev_lines(t);
    char *last = tui_strip_ansi(pl->v[pl->n - 1]);
    CHECK(strstr(last, "opt-b") != NULL);
    CHECK(tui_visible_width(last) <= 40);
    free(last);
    CHECK(tui_focus(t) == &sl->base);
    tui_overlay_hide(t, &sl->base);
    CHECK(tui_focus(t) == NULL);
    tui_render_now(t);
    free(drain_fd(pfd[0]));
    tui_component_destroy(&sl->base);

    /* cursor marker positions the hardware cursor */
    tui_editor_t *ed = tui_editor_new();
    tui_container_add(tui_root(t), &ed->base);
    tui_set_focus(t, &ed->base);
    feed(ed, "abc");
    tui_render_now(t);
    char *o7 = drain_fd(pfd[0]);
    CHECK(strstr(o7, "\x1b[4G") != NULL); /* column after "abc" */
    CHECK(!strstr(o7, "\x1b_pi:c"));
    free(o7);

    /* resize -> the screen is repainted from its top row, scrollback is kept (omp `preserve`) */
    tui_term_set_size(tm, 30, 10);
    tui_render_now(t);
    CHECK(tui_full_redraw_count(t) == fulls);
    char *o8 = drain_fd(pfd[0]);
    CHECK(strstr(o8, "\x1b[H") && !strstr(o8, "\x1b[3J"));
    free(o8);

    /* an explicit full request clears screen + scrollback */
    tui_request_full_render(t);
    tui_render_now(t);
    CHECK(tui_full_redraw_count(t) == fulls + 1);
    char *o9 = drain_fd(pfd[0]);
    CHECK(strstr(o9, "\x1b[3J") != NULL);
    free(o9);

    /* posted callbacks + timers via run_once */
    tui_free(t);
    close(pfd[0]);
}

/* ---------- renderer vs a tiny VT emulator ---------- */

/* Enough of a terminal to replay the renderer's output: printable ASCII (other characters become
 * '?'), CR/LF with scrolling into a scrollback, CUU/CUD/CUF/CUB/CHA/CUP, ED, EL; SGR, private
 * modes, OSC and APC strings are ignored. DECAWM is treated as off (the renderer disables it). */
#define VT_COLS 40
#define VT_ROWS 10
typedef struct {
    char screen[VT_ROWS][VT_COLS + 1];
    int cx, cy;
    strvec_t scrollback;
} vt_t;

static void vt_init(vt_t *v) {
    memset(v, 0, sizeof *v);
    for (int r = 0; r < VT_ROWS; r++) memset(v->screen[r], ' ', VT_COLS);
}

static char *vt_row(const char *row) {
    char *s = xstrdup(row);
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    return s;
}

static void vt_lf(vt_t *v) {
    if (v->cy < VT_ROWS - 1) {
        v->cy++;
        return;
    }
    char *top = vt_row(v->screen[0]);
    strvec_push(&v->scrollback, top);
    free(top);
    memmove(v->screen[0], v->screen[1], sizeof v->screen[0] * (VT_ROWS - 1));
    memset(v->screen[VT_ROWS - 1], ' ', VT_COLS);
}

static void vt_feed(vt_t *v, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p == '\r') v->cx = 0, p++;
        else if (*p == '\n') vt_lf(v), p++;
        else if (*p == 0x1b && (p[1] == ']' || p[1] == '_')) { /* OSC / APC: to BEL or ST */
            p += 2;
            while (*p && *p != 0x07 && !(*p == 0x1b && p[1] == '\\')) p++;
            p += *p == 0x07 ? 1 : *p ? 2 : 0;
        } else if (*p == 0x1b && p[1] == '[') {
            p += 2;
            bool priv = *p == '?';
            if (priv) p++;
            int a[4] = {0, 0, 0, 0}, na = 0;
            while ((*p >= '0' && *p <= '9') || *p == ';') {
                if (*p == ';') na = MIN(na + 1, 3);
                else a[na] = a[na] * 10 + (*p - '0');
                p++;
            }
            char f = (char)*p;
            if (*p) p++;
            if (priv) continue;
            int n1 = a[0] ? a[0] : 1;
            switch (f) {
            case 'A': v->cy = MAX(0, v->cy - n1); break;
            case 'B': v->cy = MIN(VT_ROWS - 1, v->cy + n1); break;
            case 'C': v->cx = MIN(VT_COLS - 1, v->cx + n1); break;
            case 'D': v->cx = MAX(0, v->cx - n1); break;
            case 'G': v->cx = MIN(VT_COLS - 1, n1 - 1); break;
            case 'H': v->cy = MIN(VT_ROWS - 1, n1 - 1), v->cx = MIN(VT_COLS - 1, (a[1] ? a[1] : 1) - 1); break;
            case 'J':
                if (a[0] == 3) strvec_free(&v->scrollback);
                else if (a[0] == 2) for (int r = 0; r < VT_ROWS; r++) memset(v->screen[r], ' ', VT_COLS);
                else {
                    memset(v->screen[v->cy] + v->cx, ' ', VT_COLS - v->cx);
                    for (int r = v->cy + 1; r < VT_ROWS; r++) memset(v->screen[r], ' ', VT_COLS);
                }
                break;
            case 'K':
                if (a[0] == 2) memset(v->screen[v->cy], ' ', VT_COLS);
                else memset(v->screen[v->cy] + v->cx, ' ', VT_COLS - v->cx);
                break;
            default: break;
            }
        } else if (*p == 0x1b) p += p[1] ? 2 : 1;
        else {
            char c = (char)*p;
            if (*p >= 0x80) { /* one UTF-8 sequence -> '?' */
                c = '?';
                p++;
                while ((*p & 0xc0) == 0x80) p++;
            } else p++;
            if (c < 0x20) continue;
            v->screen[v->cy][v->cx] = c;
            if (v->cx < VT_COLS - 1) v->cx++;
        }
    }
}

/* the emulated screen shows the frame's last VT_ROWS rows (or the whole frame from row 0) */
static bool vt_shows_tail(vt_t *v, tui_t *t) {
    const strvec_t *f = tui_prev_lines(t);
    int top = MAX(0, (int)f->n - VT_ROWS);
    bool ok = true;
    for (int r = 0; r < VT_ROWS; r++) {
        char *want = (size_t)(top + r) < f->n ? tui_strip_ansi(f->v[top + r]) : xstrdup("");
        char *got = vt_row(v->screen[r]);
        if (strcmp(want, got)) {
            fprintf(stderr, "screen row %d: want \"%s\" got \"%s\"\n", r, want, got);
            ok = false;
        }
        free(want);
        free(got);
    }
    return ok;
}

static void vt_render(vt_t *v, tui_t *t, int fd) {
    tui_render_now(t);
    char *o = drain_fd(fd);
    vt_feed(v, o);
    free(o);
}

/* a block that may still change while `active`: its rows stay out of scrollback */
typedef struct {
    tui_component_t base;
    int n;
    bool active;
} held_t;

static void held_render(tui_component_t *c, int width, strvec_t *out) {
    held_t *h = (held_t *)c;
    if (h->active) tui_hold_history(c->tui, out->n);
    for (int i = 0; i < h->n; i++) strvec_push_own(out, xasprintf("held %d", i));
}

static const tui_component_vt HELD_VT = {"held", held_render, NULL, NULL, NULL};

static void test_renderer_viewport(void) {
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    tui_term_t *tm = tui_term_new(-1, pfd[1]);
    tui_term_set_size(tm, VT_COLS, VT_ROWS);
    tui_t *t = tui_new(tm);
    vt_t v;
    vt_init(&v);
    tui_text_t *rows[25];
    for (int i = 0; i < 25; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "row %d", i);
        rows[i] = tui_text_new(buf, 0, 0);
        tui_container_add(tui_root(t), &rows[i]->base);
    }
    vt_render(&v, t, pfd[0]);
    CHECK(vt_shows_tail(&v, t));
    CHECK(v.scrollback.n == 15 && str_eq(v.scrollback.v[14], "row 14"));

    /* an animation above the viewport (same height): nothing is written, no clear */
    tui_text_set(rows[3], "ROW 3!");
    tui_render_now(t);
    char *o = drain_fd(pfd[0]);
    CHECK(!strstr(o, "ROW") && !strstr(o, "row") && !strstr(o, "\x1b[3J"));
    vt_feed(&v, o);
    free(o);
    CHECK(vt_shows_tail(&v, t));

    /* a block above the viewport grows by one row: the rows on screen do not move */
    tui_text_set(rows[5], "row 5\nrow 5b");
    tui_render_now(t);
    o = drain_fd(pfd[0]);
    CHECK(!strstr(o, "row"));
    vt_feed(&v, o);
    free(o);
    CHECK(vt_shows_tail(&v, t));

    /* ...shrinks again while three rows stream in at the bottom: those scroll the screen and the
     * rows they push into scrollback continue it without a gap or duplicate */
    tui_text_set(rows[5], "row 5");
    for (int i = 0; i < 3; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "tail %d", i);
        tui_container_add(tui_root(t), &tui_text_new(buf, 0, 0)->base);
    }
    vt_render(&v, t, pfd[0]);
    CHECK(vt_shows_tail(&v, t));
    CHECK(v.scrollback.n == 18 && str_eq(v.scrollback.v[17], "row 17"));

    /* a change above plus one on screen: the visible row is rewritten in place */
    tui_text_set(rows[2], "two");
    tui_text_set(rows[22], "row 22 edited");
    vt_render(&v, t, pfd[0]);
    CHECK(vt_shows_tail(&v, t));

    /* a tall block on screen collapses below the screen top: content is pulled back onto it */
    for (int i = 12; i < 25; i++) tui_text_set(rows[i], "");
    vt_render(&v, t, pfd[0]);
    CHECK(vt_shows_tail(&v, t));
    CHECK(tui_full_redraw_count(t) == 0);

    /* a few rows at the bottom of a scrolled frame go away (an autocomplete list closes): the frame's
     * tail moves back down to the last screen row instead of floating above blank rows */
    tui_text_t *popup = tui_text_new("p1\np2\np3\np4", 0, 0);
    for (int i = 12; i < 25; i++) tui_text_set(rows[i], "x");
    tui_container_add(tui_root(t), &popup->base);
    vt_render(&v, t, pfd[0]);
    CHECK(vt_shows_tail(&v, t));
    tui_container_remove(tui_root(t), &popup->base);
    tui_component_destroy(&popup->base);
    vt_render(&v, t, pfd[0]);
    CHECK(vt_shows_tail(&v, t));
    CHECK(tui_full_redraw_count(t) == 0);
    tui_free(t);
    strvec_free(&v.scrollback);

    /* held rows (an unsettled block) never reach scrollback; the screen shows the frame's tail and
     * the block enters scrollback whole once it settles */
    tm = tui_term_new(-1, pfd[1]);
    tui_term_set_size(tm, VT_COLS, VT_ROWS);
    t = tui_new(tm);
    vt_init(&v);
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "row %d", i);
        tui_container_add(tui_root(t), &tui_text_new(buf, 0, 0)->base);
    }
    held_t *hb = xcalloc(1, sizeof *hb);
    tui_component_init(&hb->base, &HELD_VT);
    hb->n = 20;
    hb->active = true;
    tui_container_add(tui_root(t), &hb->base);
    vt_render(&v, t, pfd[0]);
    CHECK(vt_shows_tail(&v, t));
    CHECK(v.scrollback.n == 5 && str_eq(v.scrollback.v[4], "row 4"));
    hb->n = 23;
    vt_render(&v, t, pfd[0]);
    CHECK(vt_shows_tail(&v, t));
    CHECK(v.scrollback.n == 5);
    hb->active = false;
    vt_render(&v, t, pfd[0]);
    CHECK(vt_shows_tail(&v, t));
    CHECK(v.scrollback.n == 18 && str_eq(v.scrollback.v[5], "held 0") && str_eq(v.scrollback.v[17], "held 12"));
    hb->n = 25; /* settled rows scroll out as usual */
    vt_render(&v, t, pfd[0]);
    CHECK(vt_shows_tail(&v, t));
    CHECK(v.scrollback.n == 20 && str_eq(v.scrollback.v[19], "held 14"));
    CHECK(tui_full_redraw_count(t) == 0);
    tui_free(t);
    strvec_free(&v.scrollback);
    close(pfd[0]);
}

static int g_posted;
static void post_cb(void *ud) { g_posted += *(int *)ud; }

static void test_loop(void) {
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    tui_term_t *tm = tui_term_new(-1, pfd[1]);
    tui_t *t = tui_new(tm);
    int v = 5;
    tui_post(t, post_cb, &v);
    tui_add_timer(t, 1, false, post_cb, &v);
    for (int i = 0; i < 20 && g_posted < 10; i++) tui_run_once(t, 5);
    CHECK(g_posted == 10);
    tui_stop(t, 3);
    CHECK(!tui_run_once(t, 0));
    tui_free(t);
    close(pfd[0]);
}

/* app.* registry, user overrides, fallback keys (app-keybindings.ts KeybindingsManager) */
static bool kb_has(tui_keybindings_t *kb, const char *action, const char *chord) {
    const strvec_t *v = tui_keybindings_get(kb, action);
    return v && strvec_contains(v, chord);
}

static void test_app_keybindings(void) {
    tui_keybindings_t *kb = tui_keybindings_new();
    CHECK(tui_keybindings_action_count() >= 77);
    CHECK_STR(tui_keybindings_action_at(0), "tui.editor.cursorUp");
    CHECK(kb_has(kb, "app.retry", "f5") && kb_has(kb, "app.retry", "alt+r"));
    CHECK(kb_has(kb, "app.model.cycleBackward", "ctrl+shift+p"));
    CHECK(kb_has(kb, "app.message.followUp", "ctrl+q") && kb_has(kb, "app.message.followUp", "ctrl+enter"));
    CHECK(kb_has(kb, "app.message.dequeue", "shift+up"));
    CHECK(tui_keybindings_get(kb, "app.session.new")->n == 0); /* unbound by default */
    CHECK(!kb_has(kb, "tui.input.newLine", "alt+enter"));     /* upstream: shift+enter, ctrl+j */
    CHECK(tui_keybindings_key_bound(kb, "ctrl+d", "tui.editor.deleteCharForward"));

    /* user config replaces an action's keys; ids are lowercased like omp's normalizeKeyId */
    json_t *cfg = json_parse_str("{\"app.retry\": [], \"app.thinking.toggle\": \"CTRL+Y\", \"app.exit\": [\"ctrl+d\", \"ctrl+x\"],"
                                 " \"app.plan.toggle\": \"ctrl+q\", \"app.clear\": 5, \"my.action\": \"alt+z\"}");
    tui_keybindings_set_user(kb, cfg);
    json_free(cfg);
    CHECK(tui_keybindings_get(kb, "app.retry")->n == 0);
    CHECK(kb_has(kb, "app.thinking.toggle", "ctrl+y") && !kb_has(kb, "app.thinking.toggle", "ctrl+t"));
    CHECK(kb_has(kb, "app.exit", "ctrl+x"));
    CHECK(kb_has(kb, "app.clear", "ctrl+c")); /* a non-string value is ignored */
    CHECK(kb_has(kb, "my.action", "alt+z"));
    /* ctrl+q claimed by another action drops out of the follow-up defaults; ctrl+enter stays */
    CHECK(!kb_has(kb, "app.message.followUp", "ctrl+q") && kb_has(kb, "app.message.followUp", "ctrl+enter"));
    CHECK(kb_has(kb, "app.message.dequeue", "shift+up"));
    tui_key_t k[2];
    size_t n = tui_keys_decode_all("\x11", 1, k, 2); /* ctrl+q */
    CHECK(n == 1 && tui_keybindings_matches(kb, &k[0], "app.plan.toggle") &&
          !tui_keybindings_matches(kb, &k[0], "app.message.followUp"));
    tui_key_free(&k[0]);
    tui_keybindings_free(kb);
}

/* emacs kill ring, yank-pop and character jump (editor.ts + kill-ring.ts) */
static void test_kill_ring_jump(void) {
    tui_editor_t *ed = tui_editor_new();
    feed(ed, "one two three");
    feed(ed, "\x17\x17"); /* ctrl+w twice: the kills accumulate into one entry */
    check_text(ed, "one ");
    CHECK(ed->kills.n == 1);
    CHECK_STR(ed->kills.v[0], "two three");
    feed(ed, "X ");
    feed(ed, "\x17"); /* a new entry after typing */
    CHECK(ed->kills.n == 2);
    feed(ed, "\x19"); /* ctrl+y yanks the newest */
    check_text(ed, "one X ");
    feed(ed, "\x1b" "y"); /* alt+y swaps it for the older entry */
    check_text(ed, "one two three");
    feed(ed, "\x1b" "y"); /* and cycles back */
    check_text(ed, "one X ");
    feed(ed, "a\x1b" "y"); /* yank-pop only right after a yank */
    check_text(ed, "one X a");
    /* ctrl+k at the end of a line kills the newline (joins), appended to a preceding forward kill */
    tui_editor_set_text(ed, "ab\ncd");
    feed(ed, "\x1b[A\x05"); /* up, end */
    feed(ed, "\x0b");
    check_text(ed, "abcd");
    CHECK_STR(ed->kills.v[ed->kills.n - 1], "\n");

    /* jump: ctrl+] then a character moves to its next occurrence; ctrl+alt+] backwards */
    tui_editor_set_text(ed, "alpha beta\ngamma");
    feed(ed, "\x1b[A\x01"); /* up, ctrl+a */
    feed(ed, "\x1d" "b");
    CHECK(ed->cl == 0 && ed->cc == 6);
    feed(ed, "\x1d" "g"); /* across lines */
    CHECK(ed->cl == 1 && ed->cc == 0);
    feed(ed, "\x1b\x1d" "a"); /* backwards from line 1 col 0 reaches line 0 */
    CHECK(ed->cl == 0 && ed->cc == 9);
    feed(ed, "\x1d" "z"); /* no match: cursor stays */
    CHECK(ed->cl == 0 && ed->cc == 9);
    feed(ed, "\x1d\x1d" "q"); /* the jump key again cancels; q is typed */
    check_text(ed, "alpha betqa\ngamma");
    /* alt+enter is upstream's hard-coded newline fallback */
    tui_editor_set_text(ed, "x");
    feed(ed, "\x1b\r");
    check_text(ed, "x\n");
    tui_editor_remember_draft(ed);
    CHECK(ed->history.n && str_eq(ed->history.v[ed->history.n - 1], "x\n"));
    tui_component_destroy(&ed->base);
}

/* ---------- WP-6: prompt actions, GitHub refs, @ mentions, text attachments ---------- */

static char *g_action, *g_action_prefix;
static void on_action(tui_editor_t *ed, const char *action, const char *prefix, void *ud) {
    free(g_action), free(g_action_prefix);
    g_action = xstrdup(action);
    g_action_prefix = xstrdup(prefix);
    if (!strcmp(action, "undo")) tui_editor_undo_past_transient(ed, prefix);
    else if (!strcmp(action, "cursor-message-start")) tui_editor_move_to_message_start(ed);
}

static tui_ac_default_t *wp6_provider(tui_editor_t *ed) {
    tui_ac_default_t *p = tui_ac_default_new(NULL);
    tui_ac_default_add_action(p, "copy-line", "Copy current line", "Alt+\xe2\x87\xa7L", "copy line clipboard current");
    tui_ac_default_add_action(p, "copy-prompt", "Copy whole prompt", "Alt+\xe2\x87\xa7" "C", "copy prompt clipboard message");
    tui_ac_default_add_action(p, "undo", "Undo", "Ctrl+-/Ctrl+_", "undo revert edit history");
    tui_ac_default_add_action(p, "cursor-message-start", "Move cursor to message start", "Current message",
                              "move cursor message start beginning prompt first top");
    tui_editor_set_autocomplete(ed, tui_ac_default_provider, p);
    ed->on_action = on_action;
    return p;
}

static bool g_large_taken;
static bool large_hook(void *ud, tui_editor_t *ed, const char *text, size_t lines) {
    g_large_taken = true;
    tui_editor_insert_text_attachment(ed, text, NULL, "\x1b[31m");
    return true;
}

static void test_wp6_editor(void) {
    /* undo coalescing (#insertCharacter): a whitespace chunk and the word after it are separate units */
    tui_editor_t *ed = tui_editor_new();
    feed(ed, "hello");
    feed(ed, " ");
    feed(ed, "bar");
    feed(ed, "\x1f");
    check_text(ed, "hello ");
    feed(ed, "\x1f");
    check_text(ed, "hello");
    tui_component_destroy(&ed->base);

    /* # prompt actions: ranked list, accept runs the action; undo removes the transient token first */
    ed = tui_editor_new();
    tui_ac_default_t *p = wp6_provider(ed);
    feed(ed, "hello");
    feed(ed, " world");
    feed(ed, " #");
    CHECK(tui_editor_autocomplete_open(ed));
    CHECK(ed->ac_list && tui_select_list_visible_count(ed->ac_list) == 4);
    feed(ed, "und");
    const tui_select_item_t *it = tui_select_list_selected(ed->ac_list);
    CHECK(it && !strcmp(it->label, "Undo"));
    feed(ed, "\r");
    CHECK(g_action && !strcmp(g_action, "undo") && !strcmp(g_action_prefix, "#und"));
    check_text(ed, "hello world");
    feed(ed, " #cop");
    it = tui_select_list_selected(ed->ac_list);
    CHECK(it && !strcmp(it->label, "Copy current line"));
    feed(ed, "\r");
    CHECK(!strcmp(g_action, "copy-line") && !strcmp(g_action_prefix, "#cop"));
    check_text(ed, "hello world ");
    feed(ed, "#zzzzq"); /* nothing matches: no popup */
    CHECK(!tui_editor_autocomplete_open(ed));
    tui_editor_clear(ed);

    /* #123 -> pr:// / issue:// (both kinds; a qualifier keeps one) */
    feed(ed, "see #123");
    CHECK(ed->ac_list && tui_select_list_visible_count(ed->ac_list) == 2);
    it = tui_select_list_selected(ed->ac_list);
    CHECK(it && !strcmp(it->label, "PR #123"));
    feed(ed, "\x1b[B\r"); /* down, enter */
    check_text(ed, "see issue://123 ");
    tui_editor_clear(ed);
    feed(ed, "fix pr #7");
    CHECK(ed->ac_list && tui_select_list_visible_count(ed->ac_list) == 1);
    feed(ed, "\r");
    check_text(ed, "fix pr://7 ");
    tui_editor_clear(ed);
    feed(ed, "owner/repo#12"); /* embedded hash: no GitHub ref; the action menu does not match `12` either */
    it = ed->ac_list ? tui_select_list_selected(ed->ac_list) : NULL;
    CHECK(!it || strcmp(it->label, "PR #12"));
    tui_ac_default_free(p);
    tui_component_destroy(&ed->base);

    /* @ mentions: fuzzy project-wide, scopes, listing, quoting */
    char *root = fs_mktempdir("omc-at-");
    const char *files[] = {"src/parser/lexer.c", "src/parser/parser.c", "src/main.c", "docs/my notes.md", "scripts/run.sh",
                           "packages/app/scripts/deploy.sh", "build/out.o", ".hidden/x.txt", "README.md"};
    for (size_t i = 0; i < ARRAY_LEN(files); i++) {
        char *f = path_join(root, files[i]), *d = path_dirname(f);
        fs_mkdir_p(d, 0755);
        fs_write(f, "x", 1);
        free(f), free(d);
    }
    char *gi = path_join(root, ".gitignore");
    fs_write(gi, "build/\n", 7);
    free(gi);
    tui_ac_result_t r;
    memset(&r, 0, sizeof r);
    CHECK(tui_ac_at_suggestions(root, "@par", &r));
    CHECK(r.items.n == 2 && !strcmp(r.items.v[0].label, "parser/") && !strcmp(r.items.v[0].value, "@src/parser/") &&
          !strcmp(r.items.v[0].description, "src/parser"));
    CHECK(r.items.n == 2 && !strcmp(r.items.v[1].value, "@src/parser/parser.c "));
    CHECK(r.prefix_len == 4);
    tui_ac_result_free(&r);
    memset(&r, 0, sizeof r);
    CHECK(tui_ac_at_suggestions(root, "@scri", &r)); /* equal scores: the shallower path first */
    CHECK(r.items.n == 2 && !strcmp(r.items.v[0].description, "scripts") &&
          !strcmp(r.items.v[1].description, "packages/app/scripts"));
    tui_ac_result_free(&r);
    memset(&r, 0, sizeof r);
    CHECK(tui_ac_at_suggestions(root, "@out", &r) == false); /* gitignored */
    tui_ac_result_free(&r);
    memset(&r, 0, sizeof r);
    CHECK(tui_ac_at_suggestions(root, "@my", &r));
    CHECK(r.items.n == 1 && !strcmp(r.items.v[0].value, "@\"docs/my notes.md\" "));
    tui_ac_result_free(&r);
    memset(&r, 0, sizeof r);
    CHECK(tui_ac_at_suggestions(root, "@", &r)); /* bare @: the cwd listing, directories first, dot entries kept */
    CHECK(r.items.n >= 4 && !strcmp(r.items.v[0].label, ".hidden/") && !strcmp(r.items.v[r.items.n - 1].label, "README.md"));
    tui_ac_result_free(&r);
    memset(&r, 0, sizeof r);
    CHECK(tui_ac_at_suggestions(root, "@src/", &r)); /* scoped, empty query: dirs first, shallow first */
    CHECK(r.items.n == 4 && !strcmp(r.items.v[0].value, "@src/parser/") && !strcmp(r.items.v[1].value, "@src/main.c "));
    tui_ac_result_free(&r);
    CHECK(tui_ac_at_prefix_start("look at @src/ma") == 8);
    CHECK(tui_ac_at_prefix_start("x=@a") == 2);
    CHECK(tui_ac_at_prefix_start("mail@host") == -1);
    CHECK(tui_ac_at_prefix_start("see @\"docs/my no") == 4);
    char *rm = xasprintf("rm -rf '%s'", root);
    CHECK(system(rm) == 0);
    free(rm), free(root);

    /* large paste: the host hook takes a marker-sized paste and stages a text attachment chip */
    ed = tui_editor_new();
    tui_editor_set_large_paste_hook(ed, large_hook, NULL);
    sb_t big = SB_INIT;
    for (int i = 1; i <= 12; i++) sb_appendf(&big, "line %d\n", i);
    tui_editor_paste(ed, "short");
    CHECK(!g_large_taken);
    tui_editor_paste(ed, sb_cstr(&big));
    CHECK(g_large_taken && ed->texts.n == 1 && ed->texts.v[0].line_count == 13);
    char *label = xasprintf("short%s #1 ", tui_sym("chip.paste"));
    check_text(ed, label);
    free(label);
    char *exp = tui_editor_get_expanded_text(ed);
    CHECK(str_starts(exp, "shortline 1\n") && strstr(exp, "line 12\n"));
    free(exp);
    tui_editor_clear(ed);
    CHECK(ed->texts.n == 0 && ed->text_counter == 0);
    sb_free(&big);
    tui_component_destroy(&ed->base);
    free(g_action), free(g_action_prefix);
}

int main(void) {
    test_colorful_markdown();
    test_keys();
    test_wp6_editor();
    test_app_keybindings();
    test_kill_ring_jump();
    test_ansi();
    test_theme();
    test_markdown();
    test_golden();
    test_editor();
    test_select();
    test_fuzzy_select();
    test_renderer();
    test_renderer_viewport();
    test_loop();
    tui_theme_set(NULL);
    tui_keybindings_set_global(NULL);
    printf("test_tui: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
