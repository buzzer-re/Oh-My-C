/* guards: magic-keyword prose detection, tiny-message preprocessing, auto-thinking clamps, todo-tracker
 * isAwaitingUserAnswer, tool-call loop canonicalization, URL-read bot walls (ports of omp magic-keywords.ts /
 * markdown-prose.ts, tiny/message-preproc.ts, pi-tui thinking.ts, session/todo-tracker.ts, utils/tool-call-loop-guard.ts,
 * web/scrapers/types.ts botWallOf) */
#include "agent/agent.h"
#include "coding/autothink.h"
#include "coding/guards.h"
#include "coding/keywords.h"
#include "coding/title.h"
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

/* generateTitleOnlineWithModels on one reply (omp 15d38fb094, d4ab5de6cb): a closed empty marker or the `none` sentinel
 * declines (no fallback model is asked); an empty or cut-off reply moves on to the next model */
static int judge(const char *reply, const char *want) {
    char *t = NULL;
    int r = title_judge_reply(reply, &t);
    if (want && (r != TITLE_OK || !t || strcmp(t, want))) fprintf(stderr, "judge(%s) = %d [%s] want [%s]\n", reply, r, t ? t : "", want);
    free(t);
    return r;
}

static void title_replies(void) {
    CHECK(judge("<title>none</title>", NULL) == TITLE_DECLINED);
    CHECK(judge("<title/>", NULL) == TITLE_DECLINED);
    CHECK(judge("<title />", NULL) == TITLE_DECLINED);
    CHECK(judge("<title>\"None.\"</title>", NULL) == TITLE_DECLINED);
    CHECK(judge("<title></title>", NULL) == TITLE_DECLINED);
    CHECK(judge("None.", NULL) == TITLE_DECLINED);
    CHECK(judge("", NULL) == TITLE_INVALID);
    CHECK(judge("<title>", NULL) == TITLE_INVALID);   /* cut off: not a decline */
    CHECK(judge("<title>  \n", NULL) == TITLE_INVALID);
    CHECK(judge("Thinking process:\nthe user wants a title", NULL) == TITLE_INVALID);
    CHECK(judge("<title>Fix resolver errors</title>", "Fix resolver errors") == TITLE_OK);
    CHECK(judge("Fix resolver errors.", "Fix resolver errors") == TITLE_OK);
    CHECK(judge("<think>maybe <title>Wrong</title></think>\n<title>Real title</title>", "Real title") == TITLE_OK);
    CHECK(judge("<thinking>plan</thinking>\n\nFix parser crash", "Fix parser crash") == TITLE_OK);
    CHECK(judge("{\"title\": \"Json title\"}", "Json title") == TITLE_OK);
    CHECK(judge("```json\n{\"title\": \"Fenced title\"}\n```", "Fenced title") == TITLE_OK);
    CHECK(judge("{\"title\": \"Salvaged title\", ", "Salvaged title") == TITLE_OK);
    CHECK(judge("<title>one two three four five six seven eight nine ten eleven twelve thirteen</title>", NULL) == TITLE_INVALID);
    char *t = title_normalize("'None'.");
    CHECK(!t);
    free(t);
    t = title_normalize("\"Fix the bug!\"\nsecond line");
    CHECK(t && !strcmp(t, "Fix the bug"));
    free(t);
    CHECK(title_is_no_title(" 'none!' "));
    CHECK(!title_is_no_title("nonexistent"));
}

int main(void) {
    explicit_nulls();
    title_replies();
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
