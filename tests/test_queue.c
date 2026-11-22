/* test_queue.c - message queue pieces: queue-input.ts list splitting, the agent queue editing API
 * (one-at-a-time delivery, dequeue order, restore), the queued-messages band rows (captured from omp
 * v18.14.2 with tests/tui/compare.sh) and the pythonExecution context text. */
#include "agent/agent.h"
#include "config/settings.h"
#include "core/fs.h"
#include "tools/outsink.h"
#include "tui/ansi.h"
#include "ui/ui.h"
#include "modes/interactive.h"

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
    /* formatQueueShorthand reads back as the same messages */
    {
        strvec_t m = {0};
        strvec_push(&m, "one");
        char *f = ui_format_queue_shorthand(&m);
        CHECK(!strcmp(f, "=> one"), "shorthand single: %s", f);
        free(f);
        strvec_push(&m, "two\nlines");
        f = ui_format_queue_shorthand(&m);
        CHECK(!strcmp(f, "=>\n1. one\n2. two\n   lines"), "shorthand list: %s", f);
        strvec_t back = {0};
        ui_split_queued_messages(ui_queue_shorthand(f), &back);
        CHECK(back.n == 2 && !strcmp(back.v[0], "one") && !strcmp(back.v[1], "two\n   lines"), "shorthand round trip");
        strvec_free(&back);
        free(f);
        strvec_free(&m);
    }
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
    bool sq = true;
    json_t *m = agent_pop_last_queued_ex(a, &sq);
    char *t = agent_queued_text(m);
    CHECK(!strcmp(t, "s2") && !sq, "pop last steering: %s", t);
    free(t);
    json_free(m);
    m = agent_pop_last_queued(a);
    t = agent_queued_text(m);
    CHECK(!strcmp(t, "s1"), "pop steering again: %s", t);
    free(t);
    json_free(m);
    bool fq = false;
    m = agent_pop_last_queued_ex(a, &fq);
    t = agent_queued_text(m);
    CHECK(!strcmp(t, "f1") && fq, "then follow-ups, tagged as such: %s", t);
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

static void test_forced_delivery(void) {
    agent_t *a = agent_new();
    CHECK(!agent_take_forced_delivery(a), "no forced delivery by default");
    CHECK(!agent_has_interruptible_input(a), "an empty queue is not interruptible");
    agent_steer(a, synthetic("hidden steer"));
    CHECK(!agent_has_interruptible_input(a), "agent-authored input does not count");
    agent_follow_up(a, msg_user_text("f1"));
    agent_follow_up(a, synthetic("hidden follow-up"));
    agent_steer(a, msg_user_text("s1"));
    agent_follow_up(a, msg_user_text("f2"));
    CHECK(agent_has_interruptible_input(a), "user input counts");
    agent_abort_deliver_queued(a);
    CHECK(a->cancel, "the forced delivery aborts the run");
    CHECK(agent_take_forced_delivery(a), "forced");
    CHECK(!agent_take_forced_delivery(a), "taken once");
    CHECK(json_len(a->steering) == 4 && json_len(a->follow_up) == 1, "user follow-ups moved behind the steers");
    char *t = agent_queued_text(json_at(a->steering, 2));
    CHECK(!strcmp(t, "f1"), "first promoted: %s", t);
    free(t);
    t = agent_queued_text(json_at(a->steering, 3));
    CHECK(!strcmp(t, "f2"), "second promoted: %s", t);
    free(t);
    CHECK(!json_get_bool(json_at(a->steering, 2), "steering", false), "a promoted follow-up stays unmarked");
    agent_free(a);
}

static json_t *typed(const char *t) {
    json_t *c = json_arr();
    json_push(c, text_block(t));
    return msg_user_typed(c);
}

/* agent-loop.ts mid-batch interrupt peeks (steeringQueueState sources, interruptMode) and createSkippedToolResult */
static void test_batch_interrupt(void) {
    agent_t *a = agent_new();
    CHECK(!agent_tool_interrupted(a) && !agent_tool_steering_signal(a), "nothing queued: no interrupt");
    agent_steer(a, typed("typed"));
    CHECK(agent_tool_steering_signal(a), "immediate mode: queued steering raises the cooperative signal");
    CHECK(str_eq(agent_tool_interrupt_source(a), "user"), "typed steering source: %s", agent_tool_interrupt_source(a));
    agent_clear_queues(a);
    CHECK(agent_tool_interrupted(a), "the interrupt stays latched once the queue empties");
    agent_free(a);

    a = agent_new();
    a->interrupt_wait = true;
    json_t *m = msg_user_text("parent says");
    json_set_str(m, "attribution", "agent");
    agent_steer(a, m);
    CHECK(agent_tool_interrupted(a), "wait mode: an interruptible wait is still cut short");
    CHECK(!agent_tool_steering_signal(a), "wait mode: no cooperative signal for other tools");
    CHECK(str_eq(agent_tool_interrupt_source(a), "agent"), "agent steering source: %s", agent_tool_interrupt_source(a));
    agent_free(a);

    a = agent_new();
    json_t *adv = json_obj();
    json_set_str(adv, "role", "custom");
    json_set_str(adv, "customType", "advisory");
    agent_steer(a, adv);
    agent_steer(a, typed("typed after"));
    CHECK(str_eq(agent_tool_interrupt_source(a), "system"), "one-at-a-time classifies the head only: %s",
          agent_tool_interrupt_source(a));
    agent_free(a);

    a = agent_new();
    json_t *irc = msg_user_text("peer");
    json_set_bool(irc, "synthetic", true);
    json_set_str(irc, "customType", "irc");
    agent_steer(a, irc);
    CHECK(str_eq(agent_tool_interrupt_source(a), "irc"), "peer interrupt source: %s", agent_tool_interrupt_source(a));
    agent_free(a);

    tool_result_t r = {0};
    agent_skipped_tool_result(&r, "user", false);
    const char *t = json_get_str(json_at(r.content, 0), "text");
    CHECK(r.is_error && str_eq(t, "Skipped due to queued user message. Do not count this skipped result as completed work or "
                                  "verification. After the queued message is handled on the next step, retry the skipped tool "
                                  "if it is still needed."),
          "skip text: %s", t);
    CHECK(json_get_bool(r.details, "__synthetic", false) && str_eq(json_get_str(r.details, "source"), "interrupt_skipped") &&
              json_get(r.details, "executed") && !json_get_bool(r.details, "executed", true),
          "skip details");
    agent_skipped_tool_result(&r, "background", true);
    t = json_get_str(json_at(r.content, 0), "text");
    CHECK(strstr(t, "Skipped due to a queued background completion (job or supervised process). ") &&
              strstr(t, "After the completion notice is handled"),
          "background skip text: %s", t);
    CHECK(json_get_bool(r.details, "__interrupted", false) && str_eq(json_get_str(r.details, "execution"), "started"),
          "interrupted details");
    agent_skipped_tool_result(&r, NULL, false);
    t = json_get_str(json_at(r.content, 0), "text");
    CHECK(strstr(t, "Skipped due to pending steering message. ") != NULL, "unknown-source skip text: %s", t);
    tool_result_free(&r);
}

/* composer-prediction-controller.ts parseComposerPrediction / parseComposerPredictions */
static void test_prediction_parse(void) {
    struct { const char *in, *want; } one[] = {
        {"  Run   the\ntests ", "Run the tests"}, {"\"Ship it\"", "Ship it"}, {"\xe2\x80\x9cShip it\xe2\x80\x9d", "Ship it"},
        {"`git push`", "git push"}, {"NO_PREDICTION", NULL}, {"Sure. NO_PREDICTION", NULL}, {"   ", NULL},
        {"\"a\" and \"b\"", "\"a\" and \"b\""}, {"\x1b[1mbold\x1b[0m\r", "bold"}};
    for (size_t i = 0; i < ARRAY_LEN(one); i++) {
        char *got = pred_parse_one(one[i].in);
        CHECK(one[i].want ? got && !strcmp(got, one[i].want) : !got, "parse_one %zu: %s", i, got ? got : "(null)");
        free(got);
    }
    sb_t big = SB_INIT;
    for (int i = 0; i < 501; i++) sb_putc(&big, 'x');
    char *got = pred_parse_one(sb_cstr(&big));
    CHECK(!got, "501 chars dropped");
    free(got);
    sb_free(&big);
    strvec_t out = {0};
    CHECK(pred_parse_many("Here are some options:\n1. Run tests\n- run tests!\n* Ship it\n2) Fix lint\nfifth\nsixth", 4, &out),
          "parse_many");
    CHECK(out.n == 4 && !strcmp(out.v[0], "Run tests") && !strcmp(out.v[1], "Ship it") && !strcmp(out.v[2], "Fix lint") &&
              !strcmp(out.v[3], "fifth"),
          "parse_many choices: %zu %s", out.n, out.n ? out.v[0] : "");
    strvec_free(&out);
    CHECK(!pred_parse_many("NO_PREDICTION\nRun tests", 4, &out) && !out.n, "leading skip declines");
    CHECK(pred_parse_many("Run tests\nNO_PREDICTION", 4, &out) && out.n == 1, "later skip line dropped");
    strvec_free(&out);
    CHECK(pred_parse_many("Only option:", 4, &out) && out.n == 1, "a lone colon line stays");
    strvec_free(&out);
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
    test_forced_delivery();
    test_batch_interrupt();
    test_band();
    test_python_text();
    test_prediction_parse();
    fs_rm_rf(dir);
    free(dir);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
