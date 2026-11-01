/* test_toolcards.c - tool card regression checks against captured omp output.
 *
 * Fixtures under tests/fixtures/toolcards/ are `tmux capture-pane -e` captures of omp v18.14.2 driven by
 * tests/tui/compare.sh (120 columns, titanium theme, truecolor, unicode glyphs, mock model). Each case
 * renders the same tool call through ui/tools (tc_render / tc_render_read_group) at width 120 and compares
 * the card rows cell by cell (glyph, fg, bg, attributes) with the fixture rows from an anchor line on.
 * tmux only emits SGR changes, so a fixture is parsed as one stream (state carries across rows). */
#include "core/core.h"
#include "core/fs.h"
#include "core/json.h"
#include "tui/ansi.h"
#include "tui/symbols.h"
#include "tui/theme.h"
#include "ui/tools/tc.h"

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

typedef struct {
    char ch[16];
    char fg[24], bg[24];
    int attr; /* 1 bold 2 dim 4 italic 8 underline 16 inverse 32 strike */
} cell_t;
typedef VEC(cell_t) cells_t;

static void apply_sgr(const char *params, cell_t *st) {
    int v[32], n = 0;
    const char *p = params;
    while (*p && n < 32) {
        v[n++] = atoi(p);
        while (*p && *p != ';' && *p != ':') p++;
        if (*p) p++;
    }
    if (!n) v[n++] = 0;
    for (int i = 0; i < n; i++) {
        int c = v[i];
        if (c == 0) st->fg[0] = st->bg[0] = 0, st->attr = 0;
        else if (c == 1) st->attr |= 1;
        else if (c == 2) st->attr |= 2;
        else if (c == 3) st->attr |= 4;
        else if (c == 4) st->attr |= 8;
        else if (c == 7) st->attr |= 16;
        else if (c == 9) st->attr |= 32;
        else if (c == 22) st->attr &= ~3;
        else if (c == 23) st->attr &= ~4;
        else if (c == 24) st->attr &= ~8;
        else if (c == 27) st->attr &= ~16;
        else if (c == 29) st->attr &= ~32;
        else if (c == 39) st->fg[0] = 0;
        else if (c == 49) st->bg[0] = 0;
        else if ((c == 38 || c == 48) && i + 1 < n) {
            char *dst = c == 38 ? st->fg : st->bg;
            if (v[i + 1] == 2 && i + 4 < n) snprintf(dst, 24, "%d,%d,%d", v[i + 2], v[i + 3], v[i + 4]), i += 4;
            else if (v[i + 1] == 5 && i + 2 < n) snprintf(dst, 24, "p%d", v[i + 2]), i += 2;
        } else if ((c >= 30 && c <= 37) || (c >= 90 && c <= 97)) snprintf(st->fg, 24, "b%d", c);
        else if ((c >= 40 && c <= 47) || (c >= 100 && c <= 107)) snprintf(st->bg, 24, "b%d", c);
    }
}

/* one row to cells, continuing SGR state `st` */
static void to_cells(const char *s, cell_t *st, cells_t *out) {
    size_t len = strlen(s), i = 0;
    while (i < len) {
        if (s[i] == 0x1b) {
            if (s[i + 1] == '[') {
                size_t j = i + 2;
                while (j < len && !(s[j] >= 0x40 && s[j] <= 0x7e)) j++;
                if (j < len && s[j] == 'm') {
                    char *params = xstrndup(s + i + 2, j - i - 2);
                    apply_sgr(params, st);
                    free(params);
                }
                i = j + 1;
            } else {
                size_t j = i + 2;
                while (j < len && s[j] != 0x07 && !(s[j] == 0x1b && s[j + 1] == '\\')) j++;
                i = j < len && s[j] == 0x07 ? j + 1 : j + 2;
            }
            continue;
        }
        uint32_t cp;
        int k = utf8_decode(s + i, len - i, &cp);
        int w = tui_cp_width(cp);
        if (w == 0 && out->n) {
            cell_t *prev = &out->v[out->n - 1];
            size_t pl = strlen(prev->ch);
            if (pl + (size_t)k < sizeof prev->ch) memcpy(prev->ch + pl, s + i, (size_t)k), prev->ch[pl + k] = 0;
        } else if (w > 0) {
            cell_t c = *st;
            memcpy(c.ch, s + i, (size_t)k);
            c.ch[k] = 0;
            vec_push(out, c);
            for (int x = 1; x < w; x++) {
                cell_t pad = *st;
                pad.ch[0] = 0;
                vec_push(out, pad);
            }
        }
        i += (size_t)k;
    }
    while (out->n && !strcmp(out->v[out->n - 1].ch, " ") && !out->v[out->n - 1].bg[0] && !(out->v[out->n - 1].attr & 24)) out->n--;
}

static bool cell_eq(const cell_t *a, const cell_t *b) {
    if (strcmp(a->ch, b->ch)) return false;
    if (strcmp(a->bg, b->bg) || (a->attr & 24) != (b->attr & 24)) return false;
    if (!strcmp(a->ch, " ") || !a->ch[0]) return true;
    return !strcmp(a->fg, b->fg) && a->attr == b->attr;
}

typedef struct {
    strvec_t raw;
    VEC(cells_t) rows;
} fixture_t;

static void load_fixture(const char *name, fixture_t *f) {
    char *path = xasprintf("tests/fixtures/toolcards/%s.omp.ansi", name);
    char *d = fs_read(path, NULL);
    if (!d) {
        fprintf(stderr, "missing fixture %s\n", path);
        exit(1);
    }
    memset(f, 0, sizeof *f);
    f->raw = str_split(d, "\n");
    cell_t st = {0};
    for (size_t i = 0; i < f->raw.n; i++) {
        cells_t c = {0};
        to_cells(f->raw.v[i], &st, &c);
        vec_push(&f->rows, c);
    }
    free(d);
    free(path);
}

static void free_fixture(fixture_t *f) {
    for (size_t i = 0; i < f->rows.n; i++) vec_free(&f->rows.v[i]);
    vec_free(&f->rows);
    strvec_free(&f->raw);
}

static void compare_rows(const char *name, fixture_t *fx, size_t start, const strvec_t *got);

/* compare `got` rows with the fixture rows starting at the first row containing `anchor` */
static void compare(const char *name, const char *anchor, const strvec_t *got) {
    fixture_t f;
    load_fixture(name, &f);
    size_t start = SIZE_MAX;
    for (size_t i = 0; i < f.raw.n && start == SIZE_MAX; i++) {
        char *p = tui_strip_ansi(f.raw.v[i]);
        if (strstr(p, anchor)) start = i;
        free(p);
    }
    CHECK(start != SIZE_MAX, "%s: anchor '%s' not in fixture", name, anchor);
    if (start != SIZE_MAX) compare_rows(name, &f, start, got);
    free_fixture(&f);
}

static char *stripped_trim(const char *s) {
    char *p = tui_strip_ansi(s);
    size_t n = strlen(p);
    while (n && p[n - 1] == ' ') p[--n] = 0;
    return p;
}

/* Replay specs (tests/tui/specs/<name>.json, the transcripts `compare.sh replay` resumed in omp): every tool
 * entry is rendered at width 120 and compared with the omp capture spec_<name>, each card located by its
 * first row after the previous card. */
static void run_spec(const char *name) {
    char *path = xasprintf("tests/tui/specs/%s.json", name);
    char *d = fs_read(path, NULL);
    json_t *spec = d ? json_parse_str(d) : NULL;
    CHECK(spec != NULL, "spec %s unreadable", path);
    char *fname = xasprintf("spec_%s", name);
    fixture_t f;
    load_fixture(fname, &f);
    size_t pos = 0;
    json_t *e;
    json_arr_foreach(spec, i, e) {
        const char *tool = json_get_str(e, "tool");
        if (!tool) continue;
        json_t *r = json_obj(), *content = json_arr(), *blk = json_obj();
        json_set_str(blk, "type", "text");
        json_set_str(blk, "text", json_get_str(e, "text") ? json_get_str(e, "text") : "");
        json_push(content, blk);
        json_set(r, "content", content);
        if (json_get(e, "details")) json_set(r, "details", json_clone(json_get(e, "details")));
        json_t *args = json_get(e, "args");
        /* github renders its own card only when the tool is registered (gh on PATH + github.enabled) */
        tc_call_t c = {.name = tool, .label = !strcmp(tool, "github") ? "GitHub" : NULL, .args = args,
                       .content = json_get_arr(r, "content"), .details = json_get_obj(r, "details"),
                       .has_result = true, .is_error = json_get_bool(e, "isError", false), .spinner = -1, .elapsed_ms = -1,
                       .cwd = "/work", .term_rows = 80};
        strvec_t out = {0};
        tc_render(&c, 120, &out);
        char *anchor = out.n ? stripped_trim(out.v[0]) : xstrdup("");
        size_t start = SIZE_MAX;
        for (size_t k = pos; k < f.raw.n && start == SIZE_MAX; k++) {
            char *p = stripped_trim(f.raw.v[k]);
            if (*anchor && !strcmp(p, anchor)) start = k;
            free(p);
        }
        CHECK(start != SIZE_MAX, "%s: card %zu '%s' not found in fixture", name, i, anchor);
        if (start != SIZE_MAX) {
            compare_rows(fname, &f, start, &out);
            pos = start + out.n;
        }
        free(anchor);
        strvec_free(&out);
        json_free(r);
    }
    free_fixture(&f);
    json_free(spec);
    free(fname);
    free(d);
    free(path);
}

static void compare_rows(const char *name, fixture_t *fx, size_t start, const strvec_t *got) {
    fixture_t f = *fx;
    for (size_t r = 0; r < got->n; r++) {
        CHECK(start + r < f.rows.n, "%s: fixture too short", name);
        if (start + r >= f.rows.n) break;
        cells_t *want = &f.rows.v[start + r];
        cells_t have = {0};
        cell_t st = {0};
        to_cells(got->v[r], &st, &have);
        char *pg = tui_strip_ansi(got->v[r]);
        if (want->n != have.n) {
            char *pw = tui_strip_ansi(f.raw.v[start + r]);
            CHECK(false, "%s row %zu: %zu cells, want %zu\n  want |%s|\n  got  |%s|", name, r, have.n, want->n, pw, pg);
            free(pw);
        } else {
            for (size_t i = 0; i < want->n; i++)
                if (!cell_eq(&want->v[i], &have.v[i])) {
                    CHECK(false, "%s row %zu col %zu: '%s' fg=%s bg=%s attr=%d, want '%s' fg=%s bg=%s attr=%d\n  got |%s|", name, r,
                          i, have.v[i].ch, have.v[i].fg, have.v[i].bg, have.v[i].attr, want->v[i].ch, want->v[i].fg,
                          want->v[i].bg, want->v[i].attr, pg);
                    break;
                }
        }
        free(pg);
        vec_free(&have);
    }
}

static json_t *J(const char *s) {
    json_t *j = json_parse_str(s);
    if (!j) {
        fprintf(stderr, "bad json in test: %s\n", s);
        exit(1);
    }
    return j;
}

static tc_call_t call(const char *name, const json_t *args, const json_t *result, bool is_error) {
    tc_call_t c = {.name = name, .args = args, .spinner = -1, .elapsed_ms = -1, .cwd = "/work", .term_rows = 50};
    if (result) {
        c.content = json_get_arr(result, "content");
        c.details = json_get_obj(result, "details");
        c.has_result = true;
    }
    c.is_error = is_error;
    return c;
}

static void run(const char *fixture, const char *anchor, const char *name, const char *args, const char *result, bool err) {
    json_t *a = J(args), *r = result ? J(result) : NULL;
    tc_call_t c = call(name, a, r, err);
    strvec_t out = {0};
    tc_render(&c, 120, &out);
    compare(fixture, anchor, &out);
    strvec_free(&out);
    json_free(a);
    json_free(r);
}

int main(void) {
    tui_theme_set(tui_theme_builtin("titanium", TUI_COLOR_TRUECOLOR));
    tui_sym_set_preset("unicode");

    run("bash1", "\xe2\x95\xad", "bash", "{\"command\":\"echo hi; ls\"}",
        "{\"content\":[{\"type\":\"text\",\"text\":\"hi\\na.txt\\nlong.txt\\nsrc\"}],"
        "\"details\":{\"exitCode\":0,\"wallTimeMs\":20,\"timeoutSeconds\":300}}",
        false);
    run("basherr", "\xe2\x95\xad", "bash", "{\"command\":\"echo oops; exit 3\"}",
        "{\"content\":[{\"type\":\"text\",\"text\":\"oops\\n\\nCommand exited with code 3\"}],"
        "\"details\":{\"exitCode\":3,\"wallTimeMs\":20,\"timeoutSeconds\":300}}",
        true);
    /* shell highlighting of the command row (Sublime Bash scopes: keywords, options, quotes, vars, redirects) */
    run("bashhl", "\xe2\x95\xad", "bash", "{\"command\":\"if test -n \\\"$HOME\\\"; then FOO=1 echo 'x' \\\"y $HOME\\\" 2>/dev/null\"}",
        "{\"content\":[{\"type\":\"text\",\"text\":\"error: pi-natives:command: syntax error at end of input\\n\\nCommand exited with code 2\"}],"
        "\"details\":{\"exitCode\":2,\"wallTimeMs\":10,\"timeoutSeconds\":300}}",
        true);
    run("write1", "Write:", "write", "{\"path\":\"new.txt\",\"content\":\"alpha\\\\nbeta\"}",
        "{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"details\":{}}", false);
    run("todo1", "Todo 3", "todo", "{\"op\":\"init\",\"items\":[\"first task\",\"second task\",\"third\"]}",
        "{\"content\":[{\"type\":\"text\",\"text\":\"x\"}],\"details\":{\"phases\":[{\"name\":\"Tasks\",\"tasks\":["
        "{\"content\":\"first task\",\"status\":\"in_progress\"},{\"content\":\"second task\",\"status\":\"pending\"},"
        "{\"content\":\"third\",\"status\":\"pending\"}]}]}}",
        false);
    run("grep1", "Grep:", "grep", "{\"pattern\":\"hello\"}",
        "{\"content\":[{\"type\":\"text\",\"text\":\"x\"}],\"details\":{\"matchCount\":2,\"fileCount\":2,\"scopePath\":\".\","
        "\"files\":[\"a.txt\",\"src/util.c\"],\"displayContent\":\"# a.txt#13A8\\n*1\xe2\x94\x82hello\\n 2\xe2\x94\x82world\\n"
        " 3\xe2\x94\x82line3\\n\\n# src/\\n## util.c#CE50\\n 1\xe2\x94\x82#include <stdio.h>\\n*2\xe2\x94\x82void hello(void) { puts(\\\"hello\\\"); }\"}}",
        false);
    run("glob1", "Glob:", "glob", "{\"path\":\"**/*.c\"}",
        "{\"content\":[{\"type\":\"text\",\"text\":\"x\"}],\"details\":{\"fileCount\":2,\"scopePath\":\".\","
        "\"files\":[\"src/main.c\",\"src/util.c\"]}}",
        false);
    {
        json_t *a = J("{\"path\":\"a.txt\"}"), *r = J("{\"content\":[{\"type\":\"text\",\"text\":\"x\"}],\"details\":{}}");
        tc_call_t c = call("read", a, r, false);
        strvec_t out = {0};
        tc_render_read_group(&c, 1, 120, &out);
        compare("read1", "Read a.txt", &out);
        strvec_free(&out);
        json_free(a);
        json_free(r);
    }
    /* edit: numbered diff with a single-line replacement (inverse word diff, deduped gutter) and an insertion */
    {
        char *dir = xstrdup("/tmp/claude-501/-Users-neo-Documents-research-core-projects-ohmyc/"
                            "879821bf-68dd-4be4-b028-c3f225dea888/scratchpad/tc/cmp/omp/work/a.txt");
        json_t *a = J("{\"input\":\"[a.txt#13A8]\\nPUT 2.=2:\\n+WORLD\\nPUT >3:\\n+added\"}");
        json_t *r = json_obj();
        json_t *content = J("[{\"type\":\"text\",\"text\":\"x\"}]");
        json_set(r, "content", content);
        json_t *d = json_obj();
        json_set_str(d, "path", dir);
        json_set_str(d, "diff", " 1|hello\n-2|world\n+2|WORLD\n 3|line3\n+4|added");
        json_set(r, "details", d);
        tc_call_t c = call("edit", a, r, false);
        strvec_t out = {0};
        tc_render(&c, 120, &out);
        compare("edit1", "Edit:", &out);
        strvec_free(&out);
        json_free(a);
        json_free(r);
        free(dir);
    }
    /* user `!cmd` execution frame: header, output pane (trailing blank row of "oops\n"), `(exit 3)` footer */
    {
        tc_bang_t b = {.command = "echo oops; exit 3", .output = "oops\n", .exit_code = 3, .spinner = -1, .term_rows = 50};
        strvec_t out = {0};
        tc_render_bang(&b, 120, &out);
        strvec_t rows = {0};
        for (size_t i = 1; i < out.n; i++) strvec_push(&rows, out.v[i]); /* from the header row on */
        compare("bangerr", "$ echo oops", &rows);
        strvec_free(&rows);
        strvec_free(&out);
    }
    /* replayed transcripts (tests/tui/specs) against omp captures; lsp1 hover / cells error highlighting differ in
     * token colors (omc's highlighter vs syntect), so those specs avoid such tokens or are compared via lsp2 only */
    static const char *const SPECS[] = {"ws", "ask", "goal", "find", "ast", "debug", "task", "wait", "lsp2", "github", "ghwatch"};
    for (size_t i = 0; i < ARRAY_LEN(SPECS); i++) run_spec(SPECS[i]);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
