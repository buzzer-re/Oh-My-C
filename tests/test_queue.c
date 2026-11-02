/* test_queue.c - message queue pieces: queue-input.ts list splitting, the agent queue editing API
 * (one-at-a-time delivery, dequeue order, restore), the queued-messages band rows (captured from omp
 * v18.14.2 with tests/tui/compare.sh) and the pythonExecution context text. */
#include "agent/agent.h"
#include "config/settings.h"
#include "core/fs.h"
#include "tools/outsink.h"
#include "tui/ansi.h"
#include "ui/ui.h"

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

static void split_case(const char *in, const char *const *want, size_t n) {
    strvec_t out = {0};
    ui_split_queued_messages(in, &out);
    CHECK(out.n == n, "split %s: %zu parts, want %zu", in, out.n, n);
    for (size_t i = 0; i < n && i < out.n; i++) CHECK(!strcmp(out.v[i], want[i]), "split %s [%zu]: '%s' want '%s'", in, i, out.v[i], want[i]);
    strvec_free(&out);
}

static void test_split(void) {
    split_case("  hello  ", (const char *[]){"hello"}, 1);
    split_case("", NULL, 0);
    split_case("1. alpha\n2. beta", (const char *[]){"alpha", "beta"}, 2);
    split_case("1) a\n   more\n2) b", (const char *[]){"a\n   more", "b"}, 2);
    split_case("a. x\nb. y\nc. z", (const char *[]){"x", "y", "z"}, 3);
    split_case("i. x\nii. y\niii. z", (const char *[]){"x", "y", "z"}, 3);
    /* not sequential, mixed punctuation, single item: the whole text is one message */
    split_case("1. a\n3. b", (const char *[]){"1. a\n3. b"}, 1);
    split_case("1. a\n2) b", (const char *[]){"1. a\n2) b"}, 1);
    split_case("1. only", (const char *[]){"1. only"}, 1);
    split_case("1. a\n2.", (const char *[]){"a"}, 1); /* an unfinished trailing item is dropped */
    CHECK(ui_queue_shorthand("-> x") && !strcmp(ui_queue_shorthand("-> x"), " x"), "-> shorthand");
    CHECK(ui_queue_shorthand("=>x") != NULL, "=> shorthand");
    CHECK(ui_queue_shorthand("- > x") == NULL, "no shorthand");
}

static json_t *synthetic(const char *t) {
    json_t *m = msg_user_text(t);
    json_set_bool(m, "synthetic", true);
    return m;
}

static void test_agent_queue(void) {
    agent_t *a = agent_new();
    agent_steer(a, msg_user_text("s1"));
    agent_steer(a, synthetic("advisor note"));
    agent_steer(a, msg_user_text("s2"));
    agent_follow_up(a, msg_user_text("f1"));
    strvec_t st = {0}, fu = {0};
    agent_queue_texts(a, &st, &fu);
    CHECK(st.n == 2 && !strcmp(st.v[0], "s1") && !strcmp(st.v[1], "s2"), "steering texts skip synthetic messages");
    CHECK(fu.n == 1 && !strcmp(fu.v[0], "f1"), "follow-up texts");
    strvec_free(&st);
    strvec_free(&fu);
    CHECK(json_get_bool(json_at(a->steering, 0), "steering", false), "steered user input is marked for the interjection envelope");
    CHECK(!json_get_bool(json_at(a->steering, 1), "steering", false), "synthetic steering is not marked");
    /* dequeue: the last user-authored steering message first, then follow-ups */
    json_t *m = agent_pop_last_queued(a);
    char *t = agent_queued_text(m);
    CHECK(!strcmp(t, "s2"), "pop last steering: %s", t);
    free(t);
    json_free(m);
    m = agent_pop_last_queued(a);
    t = agent_queued_text(m);
    CHECK(!strcmp(t, "s1"), "pop steering again: %s", t);
    free(t);
    json_free(m);
    m = agent_pop_last_queued(a);
    t = agent_queued_text(m);
    CHECK(!strcmp(t, "f1"), "then follow-ups: %s", t);
    free(t);
    json_free(m);
    CHECK(agent_pop_last_queued(a) == NULL, "agent-authored messages are never dequeued");
    CHECK(agent_queued_count(a) == 1, "the synthetic message stays queued");
    /* Esc restore takes every user message and leaves the rest */
    agent_steer(a, msg_user_text("x"));
    agent_follow_up(a, msg_user_text("y"));
    json_t *taken = agent_take_user_queued(a);
    CHECK(json_len(json_get_arr(taken, "steering")) == 1 && json_len(json_get_arr(taken, "followUp")) == 1, "take user queued");
    CHECK(agent_queued_count(a) == 1, "take leaves agent-authored messages");
    json_free(taken);
    /* image-only chip text */
    json_t *c = json_arr();
    json_push(c, image_block("AAAA", "image/png"));
    m = msg_user_content(c);
    t = agent_queued_text(m);
    CHECK(!strcmp(t, "[Image]"), "image chip: %s", t);
    free(t);
    json_free(m);
    agent_free(a);
}

/* omp QueuedMessagesBand at 100 columns (tests/tui/compare.sh: steer, follow-up, steer while streaming) */
static void test_band(void) {
    ui_t u = {0};
    strvec_push(&u.q_steer, "steer one");
    strvec_push(&u.q_steer, "steer\tthree\nline two");
    strvec_push(&u.q_follow, "follow two");
    tui_component_t *band = ui_queue_band_new(&u);
    strvec_t out = {0};
    tui_component_render(band, 100, &out);
    const char *want[] = {"",
                          " Steering \xc2\xb7 2",
                          "   1. steer one",
                          "   2. steer   three \xe2\x86\xb5 line two",
                          " After yield \xc2\xb7 1",
                          "   1. follow two",
                          "   \xe2\x94\x94 Alt+\xe2\x86\x91 to edit"};
    CHECK(out.n == ARRAY_LEN(want), "band rows: %zu", out.n);
    for (size_t i = 0; i < out.n && i < ARRAY_LEN(want); i++) {
        char *plain = tui_strip_ansi(out.v[i]);
        char *trim = xstrdup(plain);
        size_t n = strlen(trim);
        while (n && trim[n - 1] == ' ') trim[--n] = 0;
        CHECK(!strcmp(trim, want[i]), "band row %zu: '%s' want '%s'", i, trim, want[i]);
        free(trim);
        free(plain);
    }
    /* colors: heading muted, rows dim (titanium) */
    CHECK(out.n > 2 && strstr(out.v[1], "\x1b[38;2;156;163;176mSteering"), "heading is muted: %s", out.n > 1 ? out.v[1] : "");
    CHECK(out.n > 2 && strstr(out.v[2], "\x1b[38;2;107;114;128m  1. steer one"), "row is dim");
    strvec_free(&out);
    strvec_free(&u.q_steer);
    strvec_t none = {0};
    u.q_steer = none;
    strvec_free(&u.q_follow);
    tui_component_render(band, 100, &out);
    CHECK(out.n == 0, "empty band renders nothing");
    strvec_free(&out);
    tui_component_destroy(band);
}

static void test_python_text(void) {
    json_t *m = json_obj();
    json_set_str(m, "role", "pythonExecution");
    json_set_str(m, "code", "print(1)");
    json_set_str(m, "output", "1");
    json_set_int(m, "exitCode", 0);
    char *t = out_python_execution_text(m);
    CHECK(!strcmp(t, "Ran Python:\n```python\nprint(1)\n```\nOutput:\n```\n1\n```"), "python text: %s", t);
    free(t);
    json_set_int(m, "exitCode", 1);
    json_set_str(m, "output", "");
    t = out_python_execution_text(m);
    CHECK(!strcmp(t, "Ran Python:\n```python\nprint(1)\n```\n(no output)\n\nExecution failed with code 1"), "python failure: %s", t);
    free(t);
    json_free(m);
}

int main(void) {
    char *dir = fs_mktempdir("omc-queue");
    setenv("OMC_CODING_AGENT_DIR", dir, 1);
    setenv("COLORTERM", "truecolor", 1);
    settings_init(dir, NULL);
    tui_theme_set(tui_theme_builtin("titanium", TUI_COLOR_TRUECOLOR));
    tui_sym_set_preset("unicode");
    test_split();
    test_agent_queue();
    test_band();
    test_python_text();
    fs_rm_rf(dir);
    free(dir);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
