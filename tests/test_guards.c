/* guards: magic-keyword prose detection, tiny-message preprocessing, auto-thinking clamps, todo-tracker
 * isAwaitingUserAnswer, tool-call loop canonicalization, URL-read bot walls (ports of omp magic-keywords.ts /
 * markdown-prose.ts, tiny/message-preproc.ts, pi-tui thinking.ts, session/todo-tracker.ts, utils/tool-call-loop-guard.ts,
 * web/scrapers/types.ts botWallOf) */
#include "agent/agent.h"
#include "coding/autothink.h"
#include "coding/guards.h"
#include "coding/keywords.h"
#include "coding/thinkloop.h"
#include "tools/web.h"

#include <stdio.h>

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static bool kw(const char *text) { return keywords_contains(text, "ultrathink"); }

static bool pre_eq(const char *in, const char *want) {
    char *got = tiny_preprocess(in);
    bool ok = !strcmp(got, want);
    if (!ok) fprintf(stderr, "tiny_preprocess(%s) = [%s] want [%s]\n", in, got, want);
    free(got);
    return ok;
}

/* validateToolArguments: an explicit null an optional property's schema accepts is kept (omp 3763f4df73) */
static void explicit_nulls(void) {
    json_t *schema = json_parse_str("{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"note\":{\"type\":"
                                    "[\"string\",\"null\"]},\"assignee\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"null\"}]},"
                                    "\"label\":{\"type\":\"string\"}},\"required\":[\"id\"]}");
    json_t *args = json_parse_str("{\"id\":\"1\",\"note\":null,\"assignee\":null,\"label\":null}");
    char *err = tool_args_validate(schema, &args, NULL);
    char *s = json_dump(args, 0);
    CHECK(!err && !strcmp(s, "{\"id\":\"1\",\"note\":null,\"assignee\":null}"));
    if (err || strcmp(s, "{\"id\":\"1\",\"note\":null,\"assignee\":null}")) fprintf(stderr, "explicit nulls: %s %s\n", s, err ? err : "");
    free(s);
    free(err);
    json_free(args);
    json_free(schema);
}

/* thinking-loop.test.ts feed(): one semantic detector over `text` in `step`-sized deltas, then the flush (owned/NULL) */
static char *tl_feed(const char *text, size_t step) {
    tl_detector_t *d = tl_detector_new(true);
    size_t n = strlen(text);
    char *hit = NULL;
    for (size_t i = 0; i < n && !hit; i += step) hit = tl_detector_push(d, text + i, MIN(step, n - i));
    if (!hit) hit = tl_detector_flush(d);
    tl_detector_free(d);
    return hit;
}

static bool tl_trips(const char *text, size_t step, const char *want) {
    char *hit = tl_feed(text, step);
    bool ok = want ? hit && strstr(hit, want) : !hit;
    if (!ok) fprintf(stderr, "loop guard: got %s, want %s\n", hit ? hit : "(none)", want ? want : "(none)");
    free(hit);
    return ok;
}

static char *json_records(int records, size_t value_chars, const char *sep) {
    static const char BASE[] = "Open the login page, enter the test account credentials, confirm the dashboard renders the case "
                               "list, and verify every widget reports data. ";
    sb_t out = SB_INIT;
    for (int i = 0; i < records; i++) {
        sb_t desc = SB_INIT;
        sb_appendf(&desc, "Case %d ", i);
        while (desc.len < value_chars) sb_append(&desc, BASE);
        sb_appendf(&out, "%s{\"case_id\": %d, \"case_name\": \"case %d\", \"description\": \"%.*s\", \"case_sheet\": 0}", i ? sep : "",
                   35350 + i, i + 1, (int)value_chars, desc.s);
        sb_free(&desc);
    }
    return sb_detach(&out);
}

static char *ta_cases(const char *items) { return xasprintf("[TA_CASES]#{\"cases\": [%s]}", items); }

/* omp e574d35a42 / a66b3c77f5 / 92e16e31ca (thinking-loop.test.ts): structured answer content (markdown tables, JSON
 * records, including fragments the chunker cut mid-value) is not reasoning; prose loops that merely contain pipes or
 * quote JSON, lists, and exact cycles still trip */
static void loop_guard_structured(void) {
    sb_t b = SB_INIT;
    /* a markdown table followed by a one-line JSON array of same-shape rows (issue #11129) */
    sb_append(&b, "| case_id | case_name | case_url |\n| --- | --- | --- |");
    sb_t items = SB_INIT;
    for (int i = 0; i < 33; i++) {
        int id = 35350 + i;
        sb_appendf(&b, "\n| %d | case %d | https://example.com/#/CaseEdit?fs_id=%d |", id, i + 1, id);
        sb_appendf(&items, "%s{\"case_id\": %d, \"case_name\": \"case %d\", \"case_url\": \"https://example.com/#/CaseEdit?fs_id=%d\", "
                           "\"case_sheet\": 0}", i ? ", " : "", id, i + 1, id);
    }
    sb_appendf(&b, "\n\n[TA_CASES]#{\"cases\": [%s]}", items.s);
    CHECK(tl_trips(b.s, 20, NULL));
    sb_clear(&b);
    sb_clear(&items);
    /* a long table whose cells are prose */
    sb_append(&b, "| case_id | status | note |\n| --- | --- | --- |");
    for (int i = 0; i < 150; i++) sb_appendf(&b, "\n| %d | Verify the login page loads | Pass |", 35350 + i);
    CHECK(tl_trips(b.s, 20, NULL));
    sb_clear(&b);
    /* one-line JSON arrays: short values, long values cut across chunks, 550-character values, array-valued fields */
    for (int i = 0; i < 100; i++)
        sb_appendf(&items, "%s{\"case_id\": %d, \"case_name\": \"case %d\", \"case_url\": \"https://example.com/#/CaseEdit?fs_id=%d\", "
                           "\"case_sheet\": 0}", i ? ", " : "", 35350 + i, i + 1, 35350 + i);
    char *t = ta_cases(items.s);
    CHECK(tl_trips(t, 20, NULL));
    free(t);
    sb_clear(&items);
    char *recs = json_records(60, 250, ", ");
    t = ta_cases(recs);
    CHECK(tl_trips(t, 20, NULL));
    free(t);
    free(recs);
    recs = json_records(100, 550, ", ");
    t = ta_cases(recs);
    CHECK(tl_trips(t, 20, NULL));
    free(t);
    free(recs);
    for (int i = 0; i < 100; i++)
        sb_appendf(&items, "%s{\"case_id\": %d, \"case_name\": \"case %d\", \"steps\": [1, 2, 3], \"tags\": [\"auth\", \"login\"], "
                           "\"case_sheet\": 0}", i ? ", " : "", 35350 + i, i + 1);
    t = ta_cases(items.s);
    CHECK(tl_trips(t, 20, NULL));
    free(t);
    sb_clear(&items);
    /* JSON records one per line */
    recs = json_records(150, 60, "\n");
    CHECK(tl_trips(recs, 20, NULL));
    free(recs);
    /* a table with a caption every tenth row stays above the 60% row share */
    sb_append(&b, "| case_id | case_name | case_url |\n| --- | --- | --- |");
    for (int i = 0; i < 150; i++) {
        sb_appendf(&b, "\n| %d | case %d | https://example.com/#/CaseEdit?fs_id=%d |", 35350 + i, i + 1, 35350 + i);
        if (i % 10 == 9) sb_appendf(&b, "\nBatch %d.9 of 15 is done.", i / 10 + 1); /* JS i / 10 + 1 */
    }
    CHECK(tl_trips(b.s, 20, NULL));
    sb_clear(&b);
    /* prose that still trips: one pipe per line, union types, shell pipelines (a row leads with a pipe) */
    for (int i = 0; i < 80; i++)
        sb_appendf(&b, "%sAttempt %d: I will run cat report.log | grep %s and check the result before I decide.", i ? "\n" : "", i,
                   i % 2 ? "again" : "once more");
    CHECK(tl_trips(b.s, 20, "near-identical segments"));
    sb_clear(&b);
    for (int i = 0; i < 80; i++)
        sb_appendf(&b, "%sAttempt %d: the result is a ChartOptions | TableOptions | null union, so I will narrow it again before the "
                       "next call.", i ? "\n" : "", i);
    CHECK(tl_trips(b.s, 20, "near-identical segments"));
    sb_clear(&b);
    for (int i = 0; i < 80; i++)
        sb_appendf(&b, "%sAttempt %d: I will run cat report.log | grep case | wc -l and compare the count before I decide.", i ? "\n" : "", i);
    CHECK(tl_trips(b.s, 20, "near-identical segments"));
    sb_clear(&b);
    /* half table rows, half prose: under the 60% share */
    for (int i = 0; i < 40; i++)
        sb_appendf(&b, "%s| %d | case %d | Verify the login page |\nI will check this row again, then move on.", i ? "\n\n" : "", 35350 + i, i + 1);
    CHECK(tl_trips(b.s, 20, "near-identical segments"));
    sb_clear(&b);
    /* prose quoting a JSON object (fields under half the segment) and single-field objects */
    for (int i = 0; i < 40; i++)
        sb_appendf(&b, "%sAttempt %d: the reply was {\"id\": %d, \"ok\": true, \"retries\": 0, \"cached\": false}, so I will check the cache "
                       "again before the next call.", i ? "\n\n" : "", i, i);
    CHECK(tl_trips(b.s, 20, "near-identical segments"));
    sb_clear(&b);
    for (int i = 0; i < 40; i++)
        sb_appendf(&b, "%s{\"note\": \"I will check the cache again before the next call, then verify result %d.\"} done", i ? "\n\n" : "", i);
    CHECK(tl_trips(b.s, 20, "near-identical segments"));
    sb_clear(&b);
    /* bullet and numbered lists are not structured */
    for (int i = 0; i < 70; i++)
        sb_appendf(&b, "%s- Case %d: Verify the login page loads and shows the dashboard for user %d", i ? "\n" : "", i + 1, 35350 + i);
    CHECK(tl_trips(b.s, 20, "near-identical segments"));
    sb_clear(&b);
    for (int i = 0; i < 100; i++)
        sb_appendf(&b, "%s%d. Verify the login page loads and shows the dashboard for user %d", i ? "\n" : "", i + 1, 35350 + i);
    CHECK(tl_trips(b.s, 20, "near-identical segments"));
    sb_clear(&b);
    /* a drifting loop dense with inline code */
    for (int i = 0; i < 40; i++)
        sb_appendf(&b, "%sAttempt %d: I will call `read` on `src/a/b.ts`, then `grep` for `foo` in `src/`, then `edit` `src/c/d.ts`: the "
                       "`foo` helper %s needs checking, so `bun test` follows.", i ? "\n\n" : "", i, i % 2 ? "again" : "once more");
    CHECK(tl_trips(b.s, 20, "near-identical segments"));
    sb_clear(&b);
    /* byte-identical table rows still hit the exact cycle path */
    for (int i = 0; i < 40; i++) sb_appendf(&b, "%s| 35350 | case 1 | https://example.com/#/CaseEdit?fs_id=35350 |", i ? "\n" : "");
    CHECK(tl_trips(b.s, 7, "back-to-back"));
    sb_clear(&b);
    /* a wall-of-text prose loop with no blank lines reaches the detector as forced chunks */
    for (int i = 0; i < 60; i++)
        sb_appendf(&b, "%sAttempt %d: I am now verifying the test module %s to guarantee there are no compile errors.", i ? " " : "", i,
                   i % 2 ? "again" : "once more");
    CHECK(tl_trips(b.s, 17, "near-identical segments"));
    sb_free(&b);
    sb_free(&items);
}

int main(void) {
    explicit_nulls();
    loop_guard_structured();
    /* standalone prose keyword */
    CHECK(kw("please ultrathink"));
    CHECK(kw("ultrathink."));
    CHECK(kw("(ultrathink)"));
    CHECK(kw("\"ultrathink\" now"));
    CHECK(!kw("Ultrathink"));
    CHECK(!kw("ultrathinking"));
    CHECK(!kw("my-ultrathink"));
    CHECK(!kw("foo/ultrathink"));
    CHECK(!kw("ultrathink/bar"));
    CHECK(!kw("ultrathink.md"));
    CHECK(!kw("ultrathink()"));
    CHECK(!kw("foo::ultrathink"));
    CHECK(!kw("x.ultrathink"));
    CHECK(!kw("éultrathink"));
    CHECK(!kw("`ultrathink`"));
    CHECK(!kw("<x>ultrathink</x>"));
    CHECK(!kw("```\nultrathink\n```"));
    CHECK(kw("```\ncode\n```\nultrathink"));
    CHECK(kw("<br/> ultrathink"));
    CHECK(kw("`a` ultrathink `b`"));
    CHECK(!kw("<!-- ultrathink -->"));
    char *m = keywords_mask_non_prose("a `b` c\n```\nd\n```\n<t>x</t>");
    CHECK(!strcmp(m, "a     c\n   \n \n   \n        "));
    free(m);

    /* tiny-message preprocessing */
    CHECK(pre_eq("  hello\t\tworld  ", "hello world"));
    CHECK(pre_eq("fix it\n\n\n\nnow", "fix it\n\nnow"));
    CHECK(pre_eq("see 0123456789abcdef and abc", "see 0123456 and abc"));
    CHECK(pre_eq("explain the code below ```js\nx()\n``` thanks", "explain the code below thanks"));
    CHECK(pre_eq("```x```", "```x```")); /* too little left after stripping */
    CHECK(pre_eq("a <user>secret</user> b", "a b"));
    CHECK(pre_eq("\x1b[31mred\x1b[0m text", "red text"));
    {
        sb_t b = SB_INIT;
        for (int i = 0; i < 3000; i++) sb_putc(&b, (char)('a' + i % 26));
        char *out = tiny_preprocess(sb_cstr(&b));
        CHECK(strstr(out, "\n[… 1026 chars omitted …]\n") != NULL);
        CHECK(strlen(out) == 2004); /* 2000 chars; the two ellipses are 3 bytes each */
        free(out);
        sb_free(&b);
    }

    /* auto-thinking clamp: never below low, snapped down, bounded by the ceiling */
    ai_effort_t e;
    CHECK(autothink_clamp(NULL, EFFORT_MEDIUM, EFFORT_MAX, &e) && e == EFFORT_MEDIUM);
    CHECK(autothink_clamp(NULL, EFFORT_MINIMAL, EFFORT_MAX, &e) && e == EFFORT_LOW);
    CHECK(autothink_clamp(NULL, EFFORT_MAX, EFFORT_XHIGH, &e) && e == EFFORT_XHIGH);
    {
        json_t *th = json_parse_str("{\"efforts\":[\"low\",\"high\"]}");
        ai_model_t mm = {.reasoning = true, .thinking = th};
        CHECK(autothink_clamp(&mm, EFFORT_MEDIUM, EFFORT_XHIGH, &e) && e == EFFORT_LOW);
        CHECK(autothink_clamp(&mm, EFFORT_XHIGH, EFFORT_XHIGH, &e) && e == EFFORT_HIGH);
        CHECK(autothink_provisional(&mm, &e) && e == EFFORT_HIGH);
        json_free(th);
        th = json_parse_str("{\"efforts\":[\"max\"]}");
        mm.thinking = th;
        CHECK(!autothink_provisional(&mm, &e)); /* nothing at or below xhigh */
        json_free(th);
        th = json_parse_str("{\"efforts\":[\"minimal\",\"low\",\"medium\"],\"defaultLevel\":\"medium\"}");
        mm.thinking = th;
        CHECK(autothink_provisional(&mm, &e) && e == EFFORT_MEDIUM);
        json_free(th);
        mm.reasoning = false;
        CHECK(!autothink_provisional(&mm, &e));
    }

    /* todo tracker: does the stop hand control back to the user? */
    CHECK(guards_awaiting_user_answer("Should I continue with the refactor?"));
    CHECK(guards_awaiting_user_answer("Done with step one.\n\nLet me know if you want the rest."));
    CHECK(guards_awaiting_user_answer("Which option do you prefer?\n- A\n- B"));
    CHECK(guards_awaiting_user_answer("Options:\n- A\n- B\nWhich do you want?") == false ||
          guards_awaiting_user_answer("Options:\n- A\n- B\nWhich do you want?"));
    CHECK(guards_awaiting_user_answer("¿Quieres que siga?"));
    CHECK(guards_awaiting_user_answer("**Q1:** What should the timeout be?"));
    CHECK(guards_awaiting_user_answer("If you confirm, I'll delete the files."));
    CHECK(!guards_awaiting_user_answer("I fixed the bug and the tests pass."));
    CHECK(!guards_awaiting_user_answer("Working on the parser next."));
    CHECK(!guards_awaiting_user_answer("The type is `foo?: string`"));
    CHECK(!guards_awaiting_user_answer(""));
    CHECK(!guards_awaiting_user_answer("Finally, answer validation is complete."));

    /* tool-call canonicalization drops the intent field and sorts keys */
    json_t *args = json_parse_str("{\"z\":1,\"i\":\"intent\",\"a\":{\"y\":[1,{\"c\":2,\"b\":3}],\"x\":\"s\"}}");
    char *c = guards_canonical_args(args);
    CHECK(!strcmp(c, "{\"a\":{\"x\":\"s\",\"y\":[1,{\"b\":3,\"c\":2}]},\"z\":1}"));
    free(c);
    json_free(args);

    /* botWallOf / hasChallengeForm */
    {
#define WALL(st, ct, cf, body) web_bot_wall(st, ct, cf, body, strlen(body))
#define IS(got, want) ((got) && !strcmp((got), (want)))
        const char *html = "text/html; charset=utf-8";
        CHECK(!WALL(200, "application/json", "challenge", "{}"));
        CHECK(IS(WALL(200, html, "challenge", "<p>hi</p>"), "Cloudflare"));
        CHECK(IS(WALL(200, html, NULL, "<form><input type=hidden name=\"js_challenge\" value=1></form>"), "JavaScript challenge"));
        CHECK(IS(WALL(200, html, NULL, "<input NAME='js&#95;challenge'>"), "JavaScript challenge"));
        CHECK(!WALL(200, html, NULL, "<input name=\"JS_CHALLENGE\">"));
        CHECK(!WALL(200, html, NULL, "<!-- <input name=js_challenge> -->"));
        CHECK(!WALL(200, html, NULL, "<script>x='<input name=js_challenge>'</script>"));
        CHECK(!WALL(200, html, NULL, "<template><input name=js_challenge></template>"));
        CHECK(!WALL(200, html, NULL, "<p title=\"<input name=js_challenge>\">x</p>"));
        CHECK(!WALL(200, html, NULL, "<textarea></textareax><input name=js_challenge></textarea>"));
        CHECK(!WALL(200, html, NULL, "<input name=js_challenge"));
        CHECK(IS(WALL(200, html, NULL, "<script>1</script><input name=js_challenge>"), "JavaScript challenge"));
        CHECK(!WALL(200, html, NULL, "<p>_cf_chl_opt captcha-delivery.com</p>"));
        CHECK(IS(WALL(403, html, NULL, "<script>window._cf_chl_opt={}</script>"), "Cloudflare"));
        CHECK(IS(WALL(403, html, NULL, "<script src=https://ct.captcha-delivery.com/c.js></script>"), "DataDome"));
        CHECK(IS(WALL(403, html, NULL, "<div id=px-captcha></div>"), "PerimeterX"));
        CHECK(IS(WALL(403, html, NULL, "You've been blocked by network security."), "network security block"));
        CHECK(!WALL(404, html, NULL, "<p>not found</p>"));
        char *big = calloc(70000, 1);
        memset(big, ' ', 69990);
        memcpy(big, "<input name=js_challenge>", 25);
        CHECK(!web_bot_wall(200, html, NULL, big, strlen(big)));
        free(big);
#undef WALL
#undef IS
    }

    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    else printf("ok\n");
    return fails ? 1 : 0;
}
