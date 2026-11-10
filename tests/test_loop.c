/* test_loop.c - /loop mode model (coding/loop.c: argument parsing, budget, condition verdicts, texts) and the
 * /copy targets (ui/copysel.c: markdown blocks, rendered links, last code / command / link). */
#include "coding/loop.h"
#include "coding/slashcmd.h"
#include "ui/overlays.h"

#include <stdio.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

#define USAGE                                                                                                          \
    "Usage: /loop [count|duration] [--while|--until '<command>'] [prompt]. Examples: /loop 10, /loop 10m, /loop 20 "   \
    "--until 'bun test' fix the failing tests."

static char *parse_err(const char *args) {
    loop_args_t a;
    char *e = loop_parse_args(args, &a);
    if (!e) loop_args_free(&a);
    return e;
}

static bool err_is(const char *args, const char *want) {
    char *e = parse_err(args);
    bool ok = e && str_eq(e, want);
    if (!ok) fprintf(stderr, "  /loop %s -> %s\n", args, e ? e : "(ok)");
    free(e);
    return ok;
}

static void test_parse(void) {
    loop_args_t a;
    CHECK(!loop_parse_args("", &a) && a.limit_kind == LOOP_LIMIT_NONE && !a.prompt && !a.has_cond);
    loop_args_free(&a);
    CHECK(!loop_parse_args("10", &a) && a.limit_kind == LOOP_LIMIT_ITERATIONS && a.amount == 10 && !a.prompt);
    loop_args_free(&a);
    CHECK(!loop_parse_args("10m", &a) && a.limit_kind == LOOP_LIMIT_DURATION && a.amount == 600000);
    loop_args_free(&a);
    CHECK(!loop_parse_args("1h30m fix it", &a) && a.amount == 5400000 && str_eq(a.prompt, "fix it"));
    loop_args_free(&a);
    CHECK(!loop_parse_args("10 minutes go", &a) && a.limit_kind == LOOP_LIMIT_DURATION && a.amount == 600000 &&
          str_eq(a.prompt, "go"));
    loop_args_free(&a);
    CHECK(!loop_parse_args("3 fix the bug", &a) && a.amount == 3 && str_eq(a.prompt, "fix the bug"));
    loop_args_free(&a);
    CHECK(!loop_parse_args("keep going", &a) && a.limit_kind == LOOP_LIMIT_NONE && str_eq(a.prompt, "keep going"));
    loop_args_free(&a);
    CHECK(!loop_parse_args("20 --until 'bun test' fix the failing tests", &a) && a.amount == 20 && a.has_cond &&
          a.cond.until && str_eq(a.cond.command, "bun test") && str_eq(a.prompt, "fix the failing tests"));
    loop_args_free(&a);
    CHECK(!loop_parse_args("--while=\"git diff --quiet\"", &a) && a.has_cond && !a.cond.until &&
          str_eq(a.cond.command, "git diff --quiet") && !a.prompt);
    loop_args_free(&a);
    CHECK(!loop_parse_args("--until make\\ test go", &a) && str_eq(a.cond.command, "make test") && str_eq(a.prompt, "go"));
    loop_args_free(&a);

    CHECK(err_is("0", "Loop count must be a positive integer."));
    CHECK(err_is("0m", "Loop duration must be positive."));
    CHECK(err_is("0 minutes", "Loop duration must be positive."));
    CHECK(err_is("10x", "Loop duration unit must be seconds, minutes, or hours."));
    CHECK(err_is("-1", USAGE));
    CHECK(err_is("1.5h", USAGE));
    CHECK(err_is("--forever", "Unknown /loop flag --forever. " USAGE));
    CHECK(err_is("--While x", "Unknown /loop flag --While. " USAGE));
    CHECK(err_is("--while a --until b", "Use only one of --while or --until."));
    CHECK(err_is("--until 'oops", "--until has an unterminated quote."));
    CHECK(err_is("--until", "--until needs a shell command. Quote it when it contains spaces: /loop --until 'bun test'."));
    CHECK(err_is("--while -x", "--while needs a shell command. Quote it when it contains spaces: /loop --while 'bun test'."));
}

static void test_budget_and_texts(void) {
    loop_limit_t l;
    loop_limit_init(&l, LOOP_LIMIT_ITERATIONS, 2, 0);
    CHECK(!loop_limit_exhausted(&l, 0) && loop_limit_consume(&l, 0) && loop_limit_consume(&l, 0));
    CHECK(loop_limit_exhausted(&l, 0) && !loop_limit_consume(&l, 0));
    char *s = loop_format_limit(&l, 0);
    CHECK(str_eq(s, "0/2"));
    free(s);
    s = loop_describe_limit_runtime(&l);
    CHECK(str_eq(s, "0 of 2 iterations remaining"));
    free(s);
    loop_limit_init(&l, LOOP_LIMIT_DURATION, 5400000, 1000);
    CHECK(!loop_limit_duration_expired(&l, 1000) && loop_limit_consume(&l, 5000) && loop_limit_duration_expired(&l, 5401000));
    s = loop_format_limit(&l, 1000);
    CHECK(str_eq(s, "1h30m left"));
    free(s);
    s = loop_format_limit(&l, 1000 + 5400000 - 61000);
    CHECK(str_eq(s, "1m1s left"));
    free(s);
    s = loop_describe_limit(LOOP_LIMIT_DURATION, 5400000);
    CHECK(str_eq(s, "90 minutes"));
    free(s);
    s = loop_describe_limit(LOOP_LIMIT_ITERATIONS, 1);
    CHECK(str_eq(s, "1 iteration"));
    free(s);

    loop_args_t a;
    CHECK(!loop_parse_args("2 --until 'test -f x' hi", &a));
    loop_limit_t rt;
    loop_limit_init(&rt, a.limit_kind, a.amount, 0);
    s = loop_enabled_message(&a, &rt, "Esc");
    CHECK(str_eq(s, "Loop mode enabled. Limited to 2 iterations. 2 of 2 iterations remaining. Continuing until `test -f x` "
                    "succeeds. Repeating it after each turn. Esc suspends the ongoing loop; /loop again to disable."));
    free(s);

    loop_state_t st = {0};
    s = loop_autocomplete_description(&st);
    CHECK(str_eq(s, "Loop: off"));
    free(s);
    st.enabled = true;
    st.limit = rt;
    st.has_cond = true;
    st.cond = a.cond;
    s = loop_autocomplete_description(&st);
    CHECK(str_eq(s, "Loop: on (2 of 2 iterations remaining, until `test -f x` succeeds)"));
    free(s);
    s = loop_status_label(&st);
    CHECK(str_eq(s, "Loop: next prompt repeats"));
    free(s);
    st.prompt = "hi";
    s = loop_status_label(&st);
    CHECK(str_eq(s, "Loop running"));
    free(s);
    s = loop_summarize_condition(&st.cond, 40);
    CHECK(str_eq(s, "until: test -f x"));
    free(s);
    loop_args_free(&a);

    /* condition verdicts */
    loop_cond_t c = {"bun test", true};
    char *m;
    CHECK(loop_condition_verdict(&c, 1, false, false, NULL, "", 30000, &m) == LOOP_VERDICT_CONTINUE && !m);
    CHECK(loop_condition_verdict(&c, 0, false, false, NULL, "", 30000, &m) == LOOP_VERDICT_HALT &&
          str_eq(m, "Loop condition `bun test` is now satisfied. Loop mode disabled."));
    free(m);
    c.until = false;
    CHECK(loop_condition_verdict(&c, 1, false, false, NULL, "", 30000, &m) == LOOP_VERDICT_HALT &&
          str_eq(m, "Loop condition `bun test` no longer holds. Loop mode disabled."));
    free(m);
    CHECK(loop_condition_verdict(&c, 127, false, false, NULL, "\n  sh: bun: not found\nmore", 30000, &m) == LOOP_VERDICT_ERROR &&
          str_eq(m, "Loop condition `bun test` failed (exit 127): sh: bun: not found. Loop mode disabled."));
    free(m);
    CHECK(loop_condition_verdict(&c, -1, true, true, NULL, "", 1500, &m) == LOOP_VERDICT_ERROR &&
          str_eq(m, "Loop condition `bun test` timed out after 1500ms. Loop mode disabled."));
    free(m);
    CHECK(loop_condition_verdict(&c, -1, false, true, NULL, "", 1500, &m) == LOOP_VERDICT_ABORTED && !m);
}

static void test_copy_targets(void) {
    ui_md_blocks_t b = {0};
    /* omp 9b0a5564d9: the renderer's tokens: a quote takes its lazy line, an unclosed fence runs to the end */
    ui_md_extract_blocks("intro\n```ts\nconst a = 1;\n```\n> one\n>two\ntext\n\n```\nunclosed\n> q", &b);
    CHECK(b.n == 3);
    if (b.n == 3) {
        CHECK(str_eq(b.v[0].kind, "code") && str_eq(b.v[0].lang, "ts") && str_eq(b.v[0].text, "const a = 1;"));
        CHECK(str_eq(b.v[1].kind, "quote") && str_eq(b.v[1].text, "one\ntwo\ntext"));
        CHECK(str_eq(b.v[2].kind, "code") && str_eq(b.v[2].lang, "") && str_eq(b.v[2].text, "unclosed\n> q"));
    }
    ui_md_blocks_free(&b);
    /* fences indented under list items are offered; indented code is not; code in a quote stays the quote's */
    ui_md_extract_blocks("1. step\n\n   ```sh\n   make\n     -j3\n   ```\n2. next\n   - inner\n\n     ~~~py\n     x = 1\n     ~~~\n\n"
                         "    indented\n\n> ```\n> in quote\n> ```", &b);
    CHECK(b.n == 3);
    if (b.n == 3) {
        CHECK(str_eq(b.v[0].kind, "code") && str_eq(b.v[0].lang, "sh") && str_eq(b.v[0].text, "make\n  -j3"));
        CHECK(str_eq(b.v[1].kind, "code") && str_eq(b.v[1].lang, "py") && str_eq(b.v[1].text, "x = 1"));
        CHECK(str_eq(b.v[2].kind, "quote") && str_eq(b.v[2].text, "```\nin quote\n```"));
    }
    ui_md_blocks_free(&b);

    strvec_t hrefs = {0}, labels = {0};
    ui_md_extract_links("See [the docs](https://a.example/d), <https://b.example/x>, mail [me](mailto:x@y), "
                        "`https://code.example` and https://c.example/p. Again [docs](https://a.example/d).",
                        &hrefs, &labels);
    CHECK(hrefs.n == 3);
    if (hrefs.n == 3) {
        CHECK(str_eq(hrefs.v[0], "https://a.example/d") && str_eq(labels.v[0], "the docs"));
        CHECK(str_eq(hrefs.v[1], "https://b.example/x") && str_eq(labels.v[1], "https://b.example/x"));
        CHECK(str_eq(hrefs.v[2], "https://c.example/p"));
    }
    strvec_free(&hrefs), strvec_free(&labels);

    json_t *msgs = json_parse_str("[{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"```sh\\nls\\n```\\nsee https://x.example/a\"},"
                                   "{\"type\":\"toolCall\",\"name\":\"bash\",\"arguments\":{\"command\":\"echo hi\"}}]},"
                                   "{\"role\":\"toolResult\",\"content\":[]},"
                                   "{\"role\":\"assistant\",\"content\":[{\"type\":\"toolCall\",\"name\":\"eval\",\"arguments\":"
                                   "{\"cells\":[{\"code\":\"1+1\",\"language\":\"js\"},{\"code\":\"2\"}]}}]}]");
    CHECK(msgs != NULL);
    char *code = ui_copy_last_code(msgs), *link = ui_copy_last_link(msgs);
    const char *kind = NULL;
    char *cmd = ui_copy_last_command(msgs, &kind);
    CHECK(str_eq(code, "ls") && str_eq(link, "https://x.example/a"));
    CHECK(str_eq(cmd, "1+1\n\n2") && str_eq(kind, "eval"));
    free(code), free(link), free(cmd);
    json_free(msgs);

    CHECK(slash_lookup("loop") && slash_lookup("copy") && slash_lookup("open"));
    CHECK(slash_lookup("loop")->allow_args && str_eq(slash_lookup("loop")->inline_hint, "[count|duration] [--while|--until '<cmd>'] [prompt]"));
}

int main(void) {
    test_parse();
    test_budget_and_texts();
    test_copy_targets();
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails != 0;
}
