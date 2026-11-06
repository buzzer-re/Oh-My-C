/* test_toolcards.c - tool card regression checks against captured omp output.
 *
 * Fixtures under tests/fixtures/toolcards/ are `tmux capture-pane -e` captures of omp driven by
 * tests/tui/compare.sh (omp v18.17.0, 120 columns, titanium theme, truecolor, unicode glyphs, mock model; the bash
 * cards with `display.shellVisualization: false`, since omc does not port the Rust shell engine's view; the github
 * transcripts as calls through `write xd://github`, how omp registers that tool now). Each case
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

/* whether any rendered row, stripped of styling, holds `needle` */
static bool has_row(const strvec_t *out, const char *needle) {
    bool found = false;
    for (size_t i = 0; i < out->n && !found; i++) {
        char *p = tui_strip_ansi(out->v[i]);
        found = strstr(p, needle) != NULL;
        free(p);
    }
    return found;
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
        "{\"content\":[{\"type\":\"text\",\"text\":\"bash: -c: line 2: syntax error: unexpected end of file from `if' command on line 1\\n\\nCommand exited with code 2\"}],"
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
    /* user `!cmd` execution frame: header, output pane (trailing blank row of "oops\n"), `exit 3` in the bottom border */
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
    /* omp a1e7a928: `!` runs keep the loader, then `exit N` / `cancelled`, in the frame's bottom border */
    {
        tc_bang_t b = {.command = "sleep 9", .running = true, .spinner = 0, .term_rows = 50};
        strvec_t out = {0};
        tc_render_bang(&b, 40, &out);
        char *last = stripped_trim(out.v[out.n - 1]);
        CHECK(str_starts(last, "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 \xe2\xa3\xbe Running"), "running border: %s", last);
        CHECK(tui_visible_width(last) == 40, "running border width: %s", last);
        free(last);
        strvec_free(&out);
        tc_bang_t cb = {.command = "sleep 9", .cancelled = true, .exit_code = -1, .spinner = -1, .term_rows = 50};
        tc_render_bang(&cb, 40, &out);
        last = stripped_trim(out.v[out.n - 1]);
        CHECK(str_starts(last, "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 cancelled \xe2\x94\x80"), "cancelled border: %s", last);
        CHECK(!has_row(&out, "(cancelled)"), "no cancelled row");
        free(last);
        strvec_free(&out);
        tc_bang_t ok = {.command = "true", .spinner = -1, .term_rows = 50};
        tc_render_bang(&ok, 40, &out);
        char *top = stripped_trim(out.v[0]);
        last = stripped_trim(out.v[out.n - 1]);
        CHECK(str_eq(top, last), "a clean run keeps a plain rule: %s", last);
        free(top);
        free(last);
        strvec_free(&out);
        /* no room for a label: a plain rule */
        tc_bang_t nar = {.command = "x", .exit_code = 2, .spinner = -1, .term_rows = 50};
        tc_render_bang(&nar, 6, &out);
        last = stripped_trim(out.v[out.n - 1]);
        CHECK(!strstr(last, "exit"), "narrow border: %s", last);
        free(last);
        strvec_free(&out);
    }
    /* a1e7a928: Write's spinner in the header, `streaming` in the bottom border until the args complete */
    {
        json_t *a = J("{\"path\":\"new.txt\",\"content\":\"alpha\"}");
        tc_call_t c = call("write", a, NULL, false);
        c.spinner = 0;
        strvec_t out = {0};
        tc_render(&c, 60, &out);
        CHECK(has_row(&out, "\xe2\xa3\xbe Write"), "write header spinner");
        CHECK(has_row(&out, " streaming "), "write footer label");
        CHECK(!has_row(&out, "(streaming)"), "no trailing streaming row");
        strvec_free(&out);
        c.spinner = -1;
        c.args_complete = true;
        tc_render(&c, 60, &out);
        CHECK(!has_row(&out, "streaming"), "the label ends with the args");
        CHECK(!has_row(&out, "\xe2\xa3\xbe"), "no frozen spinner");
        strvec_free(&out);
        json_free(a);
        json_t *e = J("{\"path\":\"a.txt\",\"input\":\"x\"}");
        tc_call_t ec = call("edit", e, NULL, false);
        ec.spinner = 0;
        tc_render(&ec, 60, &out);
        CHECK(has_row(&out, "\xe2\xa3\xbe Edit"), "edit header spinner");
        strvec_free(&out);
        ec.spinner = -1;
        tc_render(&ec, 60, &out);
        CHECK(!has_row(&out, "\xe2\x8f\xb3"), "no pending hourglass on edit's header");
        strvec_free(&out);
        json_free(e);
    }
    /* 59deb3aa: only a diagnostics result's rows count as status rows; a raw `request` response is the server's text */
    {
        json_t *a = J("{\"action\":\"request\"}");
        json_t *r = J("{\"content\":[{\"type\":\"text\",\"text\":\"\xe2\x9c\x98 starts with the glyph\\nsecond line\"}]}");
        tc_call_t c = call("lsp", a, r, false);
        strvec_t out = {0};
        tc_render(&c, 80, &out);
        CHECK(!has_row(&out, "No issues"), "request response isn't diagnostics");
        CHECK(has_row(&out, "starts with the glyph"), "request response keeps its text");
        strvec_free(&out);
        json_free(a);
        /* no action known: inference stays, anchored at a line start */
        a = J("{}");
        c = call("lsp", a, r, false);
        tc_render(&c, 80, &out);
        CHECK(has_row(&out, "No issues") || has_row(&out, "starts with"), "unknown action infers");
        CHECK(has_row(&out, "Diagnostics") || has_row(&out, "diagnostics"), "unknown action infers diagnostics");
        strvec_free(&out);
        json_free(a);
        json_free(r);
        a = J("{}");
        r = J("{\"content\":[{\"type\":\"text\",\"text\":\"prose holding \xe2\x9c\x98 mid-line\"}]}");
        c = call("lsp", a, r, false);
        tc_render(&c, 80, &out);
        CHECK(!has_row(&out, "No issues"), "a glyph mid-line isn't a status row");
        strvec_free(&out);
        json_free(a);
        json_free(r);
    }
    /* a1e7a928 / e2fbe87d: Task rows; a frozen live row dims its glyph; a merge failure is a warning */
    {
        json_t *a = J("{}");
        json_t *r = J("{\"content\":[],\"details\":{\"progress\":[{\"id\":\"A\",\"status\":\"running\",\"index\":0},{\"id\":\"B\",\"status\":\"pending\",\"index\":1}]}}");
        tc_call_t c = call("task", a, r, false);
        c.partial = true;
        c.now_ms = 1000;
        strvec_t out = {0};
        tc_render(&c, 80, &out);
        CHECK(has_row(&out, "\xe2\x9f\xb3 A"), "running row glyph");
        CHECK(has_row(&out, "\xe2\x8f\xb3 B"), "pending row glyph");
        strvec_free(&out);
        c.frozen = true;
        tc_render(&c, 80, &out);
        char *want = tc_fg(TUI_TC_DIM, "\xe2\x9f\xb3");
        bool found = false;
        for (size_t i = 0; i < out.n; i++) found |= strstr(out.v[i], want) != NULL;
        CHECK(found, "frozen running row dims its glyph");
        free(want);
        strvec_free(&out);
        json_free(r);
        r = J("{\"content\":[],\"details\":{\"results\":[{\"id\":\"M\",\"exitCode\":0,\"error\":\"merge conflict\",\"durationMs\":5}]}}");
        c = call("task", a, r, false);
        tc_render(&c, 80, &out);
        CHECK(has_row(&out, "\xe2\x9a\xa0 M"), "merge failure draws the warning glyph");
        strvec_free(&out);
        json_free(r);
        json_free(a);
    }
    /* omp 1442d02a / d043cc13: one path style in tool bodies */
    {
        static const struct { const char *in, *dir, *name; } P[] = {
            {"src/tools/grep.ts", "src/tools/", "grep.ts"}, {"C:\\repo\\grep.ts", "C:\\repo\\", "grep.ts"},
            {"src/components/", "src/", "components/"},     {"README.md", NULL, "README.md"},
        };
        for (size_t i = 0; i < ARRAY_LEN(P); i++) {
            char *got = tc_styled_path(P[i].in);
            char *nm = tc_fg(TUI_TC_CUSTOM_MESSAGE_LABEL, P[i].name);
            char *dr = P[i].dir ? tc_fg(TUI_TC_DIM, P[i].dir) : xstrdup("");
            char *want = xasprintf("%s%s", dr, nm);
            CHECK(str_eq(got, want), "styled path %s", P[i].in);
            free(want), free(dr), free(nm), free(got);
        }
    }
    /* classifyGroupedLines: full display paths across a nested stack, `# /` keeps its separator, a directory's `(…)`
     * stays part of its name, only an uppercase hashline tag is dropped, url-like headers stay whole */
    {
        tc_grouped_t g;
        tc_grouped_parse(&g, "# /\n## abs/\n### f.ts#AB12 (2 replacements)\n*1\xe2\x94\x82x\n\n# dir (x)/\n## a.c\n\n# n#dead\n\n# n#v2\n\n# local://a/b.ts#AB12\n 1\xe2\x94\x82y", NULL, NULL);
        CHECK(g.rows[0].kind == TC_GROW_DIR && g.rows[1].kind == TC_GROW_DIR && g.rows[2].kind == TC_GROW_FILE, "kinds");
        CHECK(str_eq(g.rows[2].display_path, "/abs/f.ts"), "abs path: %s", g.rows[2].display_path);
        CHECK(str_eq(g.rows[2].suffix, "(2 replacements)"), "suffix");
        CHECK(g.rows[3].kind == TC_GROW_CONTENT, "body");
        CHECK(str_eq(g.rows[6].display_path, "dir (x)/a.c"), "dir parens: %s", g.rows[6].display_path);
        CHECK(!g.rows[6].suffix, "no suffix");
        CHECK(str_eq(g.rows[8].display_path, "n#dead"), "lowercase kept: %s", g.rows[8].display_path);
        CHECK(str_eq(g.rows[10].display_path, "n#v2"), "other ending kept: %s", g.rows[10].display_path);
        CHECK(g.rows[12].url && str_eq(g.rows[12].display_path, "local://a/b.ts"), "url: %s", g.rows[12].display_path);
        CHECK(g.ngroups == 5, "one group per file: %zu", g.ngroups);
        tc_grouped_free(&g);
        /* a headerless URL scope's lines have no filesystem language; a file scope's do */
        tc_grouped_parse(&g, "*7\xe2\x94\x82x", "local://notes/a.ts", NULL);
        CHECK(!g.rows[0].lang, "url scope language");
        tc_grouped_free(&g);
        tc_grouped_parse(&g, "*7\xe2\x94\x82x", "/w/a.ts", NULL);
        CHECK(g.rows[0].lang && str_eq(g.rows[0].lang, "typescript"), "file scope language");
        tc_grouped_free(&g);
    }
    /* fitGroupsToBudget through the cards */
    {
        /* a single file taller than the preview shows its head and counts the matches it hides */
        json_t *a = J("{\"pat\":\"p\"}");
        json_t *r = J("{\"content\":[],\"details\":{\"matchCount\":8,\"fileCount\":1,\"filesSearched\":1,\"displayContent\":"
                      "\"# a.ts\\n*1\xe2\x94\x82" "a\\n*2\xe2\x94\x82" "a\\n*3\xe2\x94\x82" "a\\n*4\xe2\x94\x82" "a\\n*5\xe2\x94\x82" "a\\n*6\xe2\x94\x82" "a\\n*7\xe2\x94\x82" "a\\n*8\xe2\x94\x82" "a\"}}");
        tc_call_t c = call("ast_grep", a, r, false);
        strvec_t out = {0};
        tc_render(&c, 80, &out);
        CHECK(out.n == 7, "ast_grep head + 6 rows: %zu", out.n);
        CHECK(has_row(&out, "a.ts") && has_row(&out, "*4") && !has_row(&out, "*5"), "ast_grep shows the first file's head");
        CHECK(has_row(&out, "\xe2\x80\xa6 4 more matches"), "ast_grep counts hidden matches");
        strvec_free(&out);
        json_free(r);
        json_free(a);
        /* the collapsed grep list is cut between files, never right after a file row without its match */
        a = J("{\"pattern\":\"p\"}");
        r = J("{\"content\":[],\"details\":{\"matchCount\":6,\"fileCount\":3,\"displayContent\":"
              "\"# a.ts\\n*1\xe2\x94\x82" "a\\n*2\xe2\x94\x82" "a\\n*3\xe2\x94\x82" "a\\n\\n# b.ts\\n*1\xe2\x94\x82" "b\\n\\n# c.ts\\n*1\xe2\x94\x82" "c\\n*2\xe2\x94\x82" "c\"}}");
        c = call("grep", a, r, false);
        tc_render(&c, 80, &out);
        CHECK(has_row(&out, "a.ts") && !has_row(&out, "b.ts"), "a file row needs a match below it");
        CHECK(has_row(&out, "\xe2\x80\xa6 3 more matches"), "grep counts hidden matches");
        strvec_free(&out);
        json_free(r);
        json_free(a);
        /* a long first diff is cut to the preview; the summary counts the files left out */
        a = J("{\"ops\":[{\"pat\":\"p\",\"out\":\"q\"}]}");
        r = J("{\"content\":[],\"details\":{\"totalReplacements\":9,\"filesTouched\":2,\"filesSearched\":2,\"displayContent\":"
              "\"# a.ts (8 replacements)\\n-1\xe2\x94\x82" "a\\n+1\xe2\x94\x82" "b\\n-2\xe2\x94\x82" "a\\n+2\xe2\x94\x82" "b\\n-3\xe2\x94\x82" "a\\n+3\xe2\x94\x82" "b\\n-4\xe2\x94\x82" "a\\n\\n# b.ts (1 replacement)\\n-1\xe2\x94\x82" "a\"}}");
        c = call("ast_edit", a, r, false);
        tc_render(&c, 80, &out);
        CHECK(out.n == 8, "ast_edit frame + 6 rows: %zu", out.n);
        CHECK(has_row(&out, "a.ts") && has_row(&out, "(8 replacements)"), "ast_edit first file");
        CHECK(has_row(&out, "\xe2\x80\xa6 1 more file"), "ast_edit counts hidden files");
        strvec_free(&out);
        json_free(r);
        json_free(a);
    }
    /* grouped Read rows: path style, muted selectors and `?q=` queries; a URL whole in the name color */
    {
        json_t *a1 = J("{\"path\":\"src/a.ts:1-5\"}"), *a2 = J("{\"path\":\"data.json?q=.items\"}"), *a3 = J("{\"path\":\"https://x.dev/p?q=1\"}");
        json_t *r = J("{\"content\":[{\"type\":\"text\",\"text\":\"x\"}]}");
        tc_call_t cs[3] = {call("read", a1, r, false), call("read", a2, r, false), call("read", a3, r, false)};
        strvec_t out = {0};
        tc_render_read_group(cs, 3, 80, &out);
        char *dir = tc_fg(TUI_TC_DIM, "src/"), *nm = tc_fg(TUI_TC_CUSTOM_MESSAGE_LABEL, "a.ts"), *sel = tc_fg(TUI_TC_MUTED, ":1-5");
        char *w1 = xasprintf("%s%s%s", dir, nm, sel);
        CHECK(strstr(out.v[1], w1), "row path: %s", out.v[1]);
        free(w1), free(dir), free(nm), free(sel);
        char *nm2 = tc_fg(TUI_TC_CUSTOM_MESSAGE_LABEL, "data.json"), *q = tc_fg(TUI_TC_MUTED, "?q=.items");
        char *w2 = xasprintf("%s%s", nm2, q);
        CHECK(strstr(out.v[2], w2), "row query: %s", out.v[2]);
        free(w2), free(nm2), free(q);
        char *w3 = tc_fg(TUI_TC_CUSTOM_MESSAGE_LABEL, "https://x.dev/p?q=1");
        CHECK(strstr(out.v[3], w3), "row url: %s", out.v[3]);
        free(w3);
        strvec_free(&out);
        /* a lone read is a header: accent subject, success glyph */
        tc_render_read_group(cs, 1, 80, &out);
        char *acc = tc_fg(TUI_TC_ACCENT, "src/a.ts");
        CHECK(strstr(out.v[0], acc) && has_row(&out, "\xe2\x9c\x94 "), "header row: %s", out.v[0]);
        free(acc);
        strvec_free(&out);
        json_free(r), json_free(a1), json_free(a2), json_free(a3);
    }
    /* LSP diagnostics: the severity color moved from the location to the tag */
    {
        json_t *a = J("{\"action\":\"diagnostics\"}");
        json_t *r = J("{\"content\":[{\"type\":\"text\",\"text\":\"1 error(s):\\nsrc/a.ts:3:5 [error] boom\"}]}");
        tc_call_t c = call("lsp", a, r, false);
        strvec_t out = {0};
        tc_render(&c, 80, &out);
        char *tag = tc_fg(TUI_TC_ERROR, "[error]"), *pos = tc_fg(TUI_TC_MUTED, ":3:5");
        bool has_tag = false, has_pos = false;
        for (size_t i = 0; i < out.n; i++) has_tag |= strstr(out.v[i], tag) != NULL, has_pos |= strstr(out.v[i], pos) != NULL;
        CHECK(has_tag, "severity tag color");
        CHECK(has_pos, "muted location");
        free(tag), free(pos);
        strvec_free(&out);
        json_free(a), json_free(r);
    }
    /* ASCII preset: three-column status glyphs, bracketed spinner */
    {
        tui_sym_set_preset("ascii");
        tui_theme_set(tui_theme_builtin("titanium", TUI_COLOR_TRUECOLOR));
        char *ok = tc_status_icon(TC_ST_SUCCESS, -1), *er = tc_status_icon(TC_ST_ERROR, -1), *ab = tc_status_icon(TC_ST_ABORTED, -1);
        char *pe = tc_status_icon(TC_ST_PENDING, -1), *ru = tc_status_icon(TC_ST_RUNNING, 1);
        char *pok = tui_strip_ansi(ok), *per = tui_strip_ansi(er), *pab = tui_strip_ansi(ab), *ppe = tui_strip_ansi(pe), *pru = tui_strip_ansi(ru);
        CHECK(str_eq(pok, "[+]") && str_eq(per, "[-]") && str_eq(pab, "[#]") && str_eq(ppe, "[ ]"), "%s %s %s %s", pok, per, pab, ppe);
        CHECK(str_eq(pru, "[/]"), "ascii spinner frame: %s", pru);
        free(pok), free(per), free(pab), free(ppe), free(pru), free(ok), free(er), free(ab), free(pe), free(ru);
        tui_sym_set_preset("unicode");
        tui_theme_set(tui_theme_builtin("titanium", TUI_COLOR_TRUECOLOR));
    }
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
