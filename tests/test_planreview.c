/* test_planreview.c - plan review overlay (ui/planreview.c) against an omp v18.17.0 capture.
 *
 * tests/fixtures/planreview/overlay.ansi is `tmux capture-pane -e` of omp's plan review overlay (120x40,
 * titanium, truecolor, unicode glyphs) for tests/fixtures/planreview/plan.md, proposed in plan mode
 * through tests/tui/compare.sh with the mock model. The keep-context option label carries omp's own
 * context estimate, so that row is compared without its numbers. Also checks the section parser,
 * section delete/undo feedback and annotations. */
#include "config/settings.h"
#include "core/fs.h"
#include "tui/color.h"
#include "ui/ui.h"

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

static const char *const OPTS[] = {"Approve and execute", "Approve and compact context",
                                   "Approve and keep context (~105 / 100k)", "Refine plan", "Save and quit"};

static char *last_feedback, *last_edit, *picked;
static void on_feedback(void *ud, const char *f) {
    (void)ud;
    free(last_feedback);
    last_feedback = xstrdup(f);
}
static void on_edit(void *ud, const char *c) {
    (void)ud;
    free(last_edit);
    last_edit = xstrdup(c);
}
static void on_pick(void *ud, const char *l) {
    (void)ud;
    free(picked);
    picked = xstrdup(l);
}
static void on_copy(void *ud, const char *c) { (void)ud, (void)c; }
static void on_ext(void *ud) { (void)ud; }

static ui_plan_review_t *make(const char *plan) {
    ui_plan_review_opts_t o = {.prompt_title = "Plan mode - next step",
                               .options = OPTS,
                               .noptions = 5,
                               .help_text = "\xe2\x8e\x8b cancel",
                               .external_editor_label = "Ctrl+G"};
    ui_plan_review_cb_t cb = {.on_pick = on_pick, .on_copy = on_copy, .on_external_editor = on_ext,
                              .on_feedback = on_feedback, .on_plan_edited = on_edit};
    return ui_plan_review_new(NULL, plan, &o, &cb);
}

static void press(ui_plan_review_t *p, const char *bytes) {
    tui_key_t keys[64];
    size_t n = tui_keys_decode_all(bytes, strlen(bytes), keys, 64);
    for (size_t i = 0; i < n; i++) {
        tui_component_handle_key(ui_plan_review_component(p), &keys[i]);
        tui_key_free(&keys[i]);
    }
}

static void render(ui_plan_review_t *p, strvec_t *out) { tui_component_render(ui_plan_review_component(p), 120, out); }

static void test_capture(const char *plan) {
    size_t n;
    char *ref = fs_read("tests/fixtures/planreview/overlay.ansi", &n);
    CHECK(ref, "fixture missing");
    if (!ref) return;
    strvec_t want = str_split_lines(ref, n);
    ui_plan_review_t *p = make(plan);
    strvec_t got = {0};
    render(p, &got);
    CHECK(got.n == 40, "rows: %zu", got.n);
    for (size_t i = 0; i < got.n && i < want.n; i++) {
        char *g = tui_strip_ansi(got.v[i]), *w = tui_strip_ansi(want.v[i]);
        char *gt = str_trim(g), *wt = str_trim(w);
        CHECK(!strcmp(gt, wt), "row %zu\n  omp: %s\n  omc: %s", i, wt, gt);
        free(gt), free(wt), free(g), free(w);
    }
    /* colors: the title, the glowing ToC entry, the selected option, the help line */
    CHECK(strstr(got.v[0], "\x1b[38;2;0;180;255m Plan Review "), "title color: %s", got.v[0]);
    CHECK(strstr(got.v[1], "\x1b[38;2;0;180;255m\xe2\x96\x8e" "Context"), "toc glow: %s", got.v[1]);
    CHECK(strstr(got.v[32], "\x1b[38;2;0;180;255m"), "cursor color");
    CHECK(strstr(got.v[38], "\x1b[38;2;107;114;128m"), "help dim");
    strvec_free(&got);
    strvec_free(&want);
    free(ref);
    tui_component_destroy(ui_plan_review_component(p));
}

static void test_edit_flow(const char *plan) {
    ui_plan_review_t *p = make(plan);
    strvec_t out = {0};
    render(p, &out);
    strvec_free(&out);
    /* Tab -> toc (actions -> toc wraps), cursor on Context; d deletes it */
    press(p, "\t");
    render(p, &out);
    CHECK(strstr(out.v[38], "section") && strstr(out.v[38], "delete"), "toc help: %s", out.v[38]);
    strvec_free(&out);
    press(p, "d");
    CHECK(last_edit && !strstr(last_edit, "## Context") && strstr(last_edit, "## Steps"), "delete: %s", last_edit ? last_edit : "");
    CHECK(last_feedback && strstr(last_feedback, "Remove these sections:\n- Context\n"), "feedback: %s", last_feedback ? last_feedback : "");
    press(p, "u");
    CHECK(last_edit && strstr(last_edit, "## Context"), "undo");
    CHECK(last_feedback && !*last_feedback, "undo feedback");
    /* annotate the section: a, type, enter */
    press(p, "a");
    press(p, "needs a timeout\r");
    CHECK(last_feedback && strstr(last_feedback, "## Context\n- needs a timeout\n"), "annotation: %s", last_feedback ? last_feedback : "");
    render(p, &out);
    bool marker = false;
    for (size_t i = 0; i < out.n; i++)
        if (strstr(out.v[i], "Context \xe2\x9c\x8e")) marker = true;
    CHECK(marker, "toc annotation marker");
    strvec_free(&out);
    /* back to actions, pick the second option */
    press(p, "\t\t\x1b[B\r");
    CHECK(picked && !strcmp(picked, "Approve and compact context"), "picked: %s", picked ? picked : "");
    render(p, &out);
    bool sub = false;
    for (size_t i = 0; i < out.n; i++)
        if (strstr(out.v[i], "Approve and compact context \xe2\x80\x94 submitting\xe2\x80\xa6")) sub = true;
    CHECK(sub, "submitting row");
    strvec_free(&out);
    tui_component_destroy(ui_plan_review_component(p));
}

int main(void) {
    char *dir = fs_mktempdir("omc-planreview");
    setenv("OMC_CODING_AGENT_DIR", dir, 1);
    setenv("COLORTERM", "truecolor", 1);
    settings_init(dir, NULL);
    tui_theme_set(tui_theme_builtin("titanium", TUI_COLOR_TRUECOLOR));
    tui_sym_set_preset("unicode");
    size_t n;
    char *plan = fs_read("tests/fixtures/planreview/plan.md", &n);
    CHECK(plan, "plan fixture");
    if (plan) {
        test_capture(plan);
        test_edit_flow(plan);
    }
    free(plan);
    free(last_feedback);
    free(last_edit);
    free(picked);
    fs_rm_rf(dir);
    free(dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
