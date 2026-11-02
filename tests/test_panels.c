/* test_panels.c - WP-7 report panels: report-panel docking vs full-screen page (CommandController.showCommandReport,
 * text captured from omp v18.14.2 with tests/tui/compare.sh), the /todo Markdown round-trip and ops
 * (tools/todo.ts phasesToMarkdown / markdownToPhases / applyOpsToPhases) and the native wrapper's long-word rule. */
#include "core/fs.h"
#include "tools/tools.h"
#include "tui/ansi.h"
#include "ui/ui.h"

#include <fcntl.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_todo_markdown(void) {
    strvec_t errs = {0};
    json_t *p = todo_markdown_to_phases("# Setup\n- [x] init repo\n- [ ] write tests\n\n## Ship\n* \\[!\\] deploy <!-- blocker: needs keys -->\n+ [-] drop it\n", &errs);
    CHECK(errs.n == 0, "unexpected parse errors: %zu", errs.n);
    CHECK(json_len(p) == 2, "phases: %zu", json_len(p));
    const json_t *t0 = json_at(json_get(json_at(p, 0), "tasks"), 1);
    CHECK(str_eq(json_get_str(t0, "status"), "in_progress"), "first pending normalized to in_progress: %s", json_get_str(t0, "status"));
    const json_t *b = json_at(json_get(json_at(p, 1), "tasks"), 0);
    CHECK(str_eq(json_get_str(b, "content"), "deploy") && str_eq(json_get_str(b, "blocker"), "needs keys"), "blocker: %s / %s",
          json_get_str(b, "content"), json_get_str(b, "blocker"));
    char *md = todo_phases_to_markdown(p);
    const char *want = "# Setup\n- [x] init repo\n- [/] write tests\n\n# Ship\n- [!] deploy <!-- blocker: needs keys -->\n- [-] drop it\n";
    CHECK(!strcmp(md, want), "markdown:\n%s", md);
    free(md);
    /* tasks before any heading land in "Todos"; bad markers and lines are reported with their line number */
    json_free(p);
    p = todo_markdown_to_phases("- [ ] a\n- [?] b\nnot a task\n", &errs);
    CHECK(json_len(p) == 1 && str_eq(json_get_str(json_at(p, 0), "name"), "Todos"), "implicit phase");
    CHECK(errs.n == 2 && strstr(errs.v[0], "Line 2: unknown status marker \"[?]\"") && strstr(errs.v[1], "Line 3: unrecognized syntax \"not a task\""),
          "errors: %s | %s", errs.n ? errs.v[0] : "", errs.n > 1 ? errs.v[1] : "");
    json_free(p);
    strvec_free(&errs);
    char *empty = todo_phases_to_markdown(NULL);
    CHECK(!strcmp(empty, "# Todos\n"), "empty markdown");
    free(empty);
}

static void test_todo_ops(void) {
    strvec_t errs = {0};
    json_t *p = todo_markdown_to_phases("# A\n- [ ] one\n- [ ] two\n# B\n- [ ] three\n", &errs);
    json_t *args = json_obj();
    json_set_str(args, "task", "two");
    json_t *n1 = todo_apply_op(p, "start", args, &errs);
    char *md = todo_phases_to_markdown(n1);
    CHECK(!strcmp(md, "# A\n- [ ] one\n- [/] two\n\n# B\n- [ ] three\n"), "start:\n%s", md);
    free(md);
    json_t *phase = json_obj();
    json_set_str(phase, "phase", "A");
    json_t *n2 = todo_apply_op(n1, "done", phase, &errs);
    md = todo_phases_to_markdown(n2);
    CHECK(!strcmp(md, "# A\n- [x] one\n- [x] two\n\n# B\n- [/] three\n"), "done phase:\n%s", md);
    free(md);
    json_t *bad = json_obj();
    json_set_str(bad, "task", "missing");
    json_t *n3 = todo_apply_op(n2, "rm", bad, &errs);
    CHECK(errs.n == 1 && !strcmp(errs.v[0], "Task \"missing\" not found"), "rm error: %s", errs.n ? errs.v[0] : "");
    json_free(n3), json_free(n2), json_free(n1), json_free(p), json_free(args), json_free(phase), json_free(bad);
    strvec_free(&errs);
}

/* wrap_single_line: a token wider than the line starts its own row, then breaks */
static void test_wrap_long_word(void) {
    strvec_t out = {0};
    tui_wrap_text_with_ansi("File: /aaaaaaaaaabbbbbbbbbbcccc", 12, &out);
    CHECK(out.n == 4, "rows: %zu", out.n);
    if (out.n == 4) {
        CHECK(!strcmp(out.v[0], "File:"), "row0 '%s'", out.v[0]);
        CHECK(!strcmp(out.v[1], "/aaaaaaaaaab"), "row1 '%s'", out.v[1]);
        CHECK(!strcmp(out.v[3], "c"), "row3 '%s'", out.v[3]);
    }
    strvec_free(&out);
}

static char *plain_row(const char *s) {
    char *p = tui_strip_ansi(s);
    size_t n = strlen(p);
    while (n && p[n - 1] == ' ') p[--n] = 0;
    return p;
}

/* a short report docks between the slot and the chrome below it; a tall one becomes a focused full-screen page */
static void test_report_docking(void) {
    int pfd[2];
    CHECK(pipe(pfd) == 0, "pipe");
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    tui_term_t *tm = tui_term_new(-1, pfd[1]);
    tui_term_set_size(tm, 60, 20);
    tui_t *t = tui_new(tm);
    ui_t u = {0};
    u.tui = t;
    u.editor = tui_editor_new();
    tui_container_add(tui_root(t), &tui_text_new("transcript", 0, 0)->base);
    tui_container_add(tui_root(t), ui_report_slot_new(&u));
    tui_container_add(tui_root(t), &tui_text_new("status row\n\neditor", 0, 0)->base);
    tui_render_now(t);
    CHECK(ui_report_rows(&u) == 17, "rows above the chrome: %d", ui_report_rows(&u));
    ui_show_report(&u, "Background Jobs", &tui_text_new("Running: 0\n\nNo async jobs yet.", 0, 0)->base);
    tui_render_now(t);
    const strvec_t *f = tui_prev_lines(t);
    static const char *const want[] = {
        "transcript",
        "╭─ Background Jobs ────────────────────────────────────────╮",
        "│                                                          │",
        "│ Running: 0                                               │",
        "│                                                          │",
        "│ No async jobs yet.                                       │",
        "│                                                          │",
        "│ ⎋ to close                                               │",
        "╰──────────────────────────────────────────────────────────╯",
        "status row",
    };
    CHECK(f->n == 12, "docked frame rows: %zu", f->n);
    for (size_t i = 0; i < ARRAY_LEN(want) && i < f->n; i++) {
        char *p = plain_row(f->v[i]);
        CHECK(!strcmp(p, want[i]), "row %zu: '%s' want '%s'", i, p, want[i]);
        free(p);
    }
    CHECK(!tui_alt_screen_active(t), "docked report stays on the main screen");
    CHECK(ui_report_dismiss(&u), "Esc dismisses the docked report");
    CHECK(!ui_report_dismiss(&u), "nothing left to dismiss");
    tui_render_now(t);
    CHECK(tui_prev_lines(t)->n == 4, "dismissed: %zu rows", tui_prev_lines(t)->n);
    /* taller than the rows above the chrome: a full-screen page with the scroll hint */
    sb_t big = SB_INIT;
    for (int i = 0; i < 40; i++) sb_appendf(&big, "line %d\n", i);
    ui_show_report(&u, "Tall", &tui_text_new(sb_cstr(&big), 0, 0)->base);
    sb_free(&big);
    CHECK(tui_alt_screen_active(t), "tall report opens on the alternate screen");
    tui_render_now(t);
    f = tui_prev_lines(t);
    CHECK(f->n == 20, "page rows: %zu", f->n);
    if (f->n == 20) {
        char *p = plain_row(f->v[18]);
        CHECK(strstr(p, "PgUp/PgDn scroll · ⎋ to close") != NULL, "page hint '%s'", p);
        free(p);
    }
    ui_report_clear(&u);
    CHECK(!tui_alt_screen_active(t), "page closed");
    tui_component_destroy(&u.editor->base);
    tui_free(t);
    close(pfd[0]);
}

int main(void) {
    test_todo_markdown();
    test_todo_ops();
    test_wrap_long_word();
    test_report_docking();
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
