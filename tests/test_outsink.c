/* test_outsink.c - OutputSink / output-meta port (expected strings captured from omp 18.14.2) */
#include "tools/outsink.h"
#include "tools/jobtext.h"
#include "core/fs.h"
#include "core/fs.h"

#include <stdio.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)
#define CHECK_STR(a, b)                                                                                                \
    do {                                                                                                               \
        const char *a_ = (a), *b_ = (b);                                                                               \
        if (strcmp(a_, b_)) {                                                                                          \
            fprintf(stderr, "FAIL %s:%d:\n  got: [%s]\n want: [%s]\n", __FILE__, __LINE__, a_, b_);                     \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static char *san(const char *s) { return out_sanitize(s, strlen(s), NULL); }

static void test_sanitize(void) {
    /* the ANSI matrix omp's Bun.stripANSI produced (see tests/parity request_diff runs) */
    const char *in = "A\033[31mred\033[0m|B\033[2K\033[1A|C\033]0;my title\007|D\033]8;;http://x.y/z?a=1\033\\link\033]8;;\033\\|"
                     "E\033(B|F\0337\0338|G\033[?25l|H\033=|I\033>|J\033M|K\033[38;5;196mx\033[48;2;1;2;3my|L\033P1$r0m\033\\|"
                     "M\302\233\"31mN\302\205O\177P\001Q\tR\033]1337;File=a b c\007|S\033_APC\033\\|T\033[1;2;3;4;5;6;7m|U\033[12345m|"
                     "V\033%G|W\033#8|X\033c|Y\033\033[1m|Z\033";
    char *o = san(in);
    CHECK_STR(o, "Ared|B|C|Dlink|E|F|G|H|I|J|Kxy|L|MNOPQ\tR|S|T|U|V|W|X|Y|Z");
    free(o);
    o = san("a\033]0;title");
    CHECK_STR(o, "a");
    free(o);
    o = san("b\033[31");
    CHECK_STR(o, "b");
    free(o);
    /* no ESC: only controls go (a C1 CSI stays a dropped control, its parameters stay) */
    o = san("x\302\233\"31my\r\n");
    CHECK_STR(o, "x\"31my\n");
    free(o);
    /* invalid UTF-8 -> U+FFFD per maximal subpart */
    o = san("A\377\376\200abc\303(\342\202\nB\355\240\200C");
    CHECK_STR(o, "A\357\277\275\357\277\275\357\277\275abc\357\277\275(\357\277\275\nB\357\277\275\357\277\275\357\277\275C");
    free(o);
    o = out_sanitize_lines("a\rb\r\nc\r", 8, NULL);
    CHECK_STR(o, "a\nb\nc\n");
    free(o);
}

static void test_format(void) {
    char *s = out_format_bytes(512);
    CHECK_STR(s, "512B");
    free(s);
    s = out_format_bytes(117658);
    CHECK_STR(s, "114.9KB");
    free(s);
    s = out_format_bytes(22859264);
    CHECK_STR(s, "21.8MB");
    free(s);
    s = out_format_grouped(2991842);
    CHECK_STR(s, "2,991,842");
    free(s);
    s = out_format_grouped(562);
    CHECK_STR(s, "562");
    free(s);
    s = out_middle_marker(1, 300);
    CHECK_STR(s, "[\xE2\x80\xA6" "300B elided\xE2\x80\xA6]");
    free(s);
}

static out_sink_t *bash_sink(const char *path, const char *id) {
    out_sink_opts_t o = {.artifact_path = path, .artifact_id = id, .spill_threshold = 50 * 1024, .head_bytes = 20 * 1024,
                         .max_columns = 768, .artifact_max_bytes = -1, .artifact_head_bytes = -1};
    return out_sink_new(&o);
}

/* omp: `seq 1 30000` through bash */
static void test_seq(const char *dir) {
    char *path = xasprintf("%s/0.bash.log", dir);
    out_sink_t *s = bash_sink(path, "0");
    sb_t all = SB_INIT;
    for (int i = 1; i <= 30000; i++) sb_appendf(&all, "%d\n", i);
    /* odd chunking must not matter */
    for (size_t off = 0; off < all.len;) {
        size_t n = MIN((size_t)977, all.len - off);
        out_sink_push(s, all.s + off, n);
        off += n;
    }
    out_summary_t sum;
    out_sink_dump(s, NULL, &sum);
    CHECK(sum.truncated);
    CHECK(sum.total_lines == 30001);
    CHECK(sum.elided_lines == 20564);
    /* omp 18.17: both windows cut back to whole lines */
    CHECK(strstr(sum.output, "\n4316\n4317\n[\xE2\x80\xA6" "20564ln elided\xE2\x80\xA6]\n24882\n") != NULL);
    CHECK(str_ends(sum.output, "29999\n30000\n"));
    CHECK_STR(sum.artifact_id, "0");
    size_t n;
    char *art = fs_read(path, &n);
    CHECK(art && n == all.len && !memcmp(art, all.s, n));
    free(art);
    json_t *meta = NULL;
    out_meta_from_summary(&meta, &sum, "tail", 1, 0);
    char *notice = out_format_notice(meta);
    CHECK_STR(notice, "\n\n[Showing lines 1-4317 and 24882-30000 of 30000; 20,564 middle lines (114.9KB) elided. Read "
                      "artifact://0 for full output]");
    char *warn = out_format_warning(meta);
    CHECK(warn && !strncmp(warn, "Showing lines 1-4317", 20));
    char *text = xasprintf("%s%s", sum.output, notice);
    char *stripped = out_strip_notice(text, meta);
    CHECK(str_ends(stripped, "30000\n"));
    free(stripped);
    free(text);
    free(warn);
    free(notice);
    json_free(meta);
    out_summary_free(&sum);
    out_sink_free(s);
    sb_free(&all);
    free(path);
}

static void test_columns(const char *dir) {
    char *path = xasprintf("%s/1.bash.log", dir);
    out_sink_t *s = bash_sink(path, "1");
    sb_t in = SB_INIT;
    for (int i = 1; i <= 39; i++) {
        for (int k = 0; k < (i % 3 == 0 ? 400 : 1); k++) sb_appendf(&in, "w%d", i);
        sb_putc(&in, '\n');
    }
    out_sink_push(s, in.s, 100);
    out_sink_push(s, in.s + 100, in.len - 100);
    out_summary_t sum;
    out_sink_dump(s, NULL, &sum);
    CHECK(!sum.truncated);
    CHECK(sum.column_truncated_lines == 13);
    CHECK(sum.column_first == 3 && sum.column_last == 39);
    const char *l3 = strstr(sum.output, "\nw3w3");
    CHECK(l3 && strstr(l3 + 1, "\xE2\x80\xA6\n") - (l3 + 1) == 765);
    json_t *meta = NULL;
    out_meta_from_summary(&meta, &sum, "tail", 1, 0);
    char *notice = out_format_notice(meta);
    CHECK_STR(notice, "\n\n[Some lines truncated to 768 bytes. Use artifact://1:raw:3-3 to read line 3 whole; cut lines run to "
                      "line 39, each read the same way]");
    free(notice);
    out_meta_column_raw_cap(meta, 50 * 1024);
    notice = out_format_notice(meta);
    CHECK_STR(notice, "\n\n[Some lines truncated to 768 bytes. Use artifact://1:raw:3-3 to read line 3 uncut, byte-capped at 50.0KB; "
                      "cut lines run to line 39, each read the same way]");
    free(notice);
    json_free(meta);
    out_summary_free(&sum);
    out_sink_free(s);
    sb_free(&in);
    /* needsJobCaption / normalizeJobCaption / resolveToolCallIntent's cleaning */
    CHECK(job_caption_needed("while ssh worker 'systemctl is-active indexer'; do sleep 60; done"));
    CHECK(!job_caption_needed("bun test 2>&1"));
    CHECK(!job_caption_needed("curl -sO 'https://x.test/a?one=1&two=2|3'"));
    CHECK(!job_caption_needed("echo \"a;b\" &> out"));
    CHECK(job_caption_needed("echo \"$(date)\""));
    CHECK(job_caption_needed("sleep 3; echo hi"));
    CHECK(job_caption_needed("cat a | wc -l"));
    CHECK(job_caption_needed("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
    {
        char *c = job_caption_normalize("- \"Checks worker every 60s until the indexer stops.\"\nExtra");
        CHECK_STR(c, "Checks worker every 60s until the indexer stops");
        free(c);
        c = job_caption_normalize("\n  Caption:   Tails \x1b[1mlogs/app.log\x1b[0m   until cancelled.\nExtra");
        CHECK_STR(c, "Tails logs/app.log until cancelled");
        free(c);
        CHECK(job_caption_normalize("  \n ") == NULL);
        char *home = xasprintf("Tails %s/logs/x until done", home_dir()), *want = xstrdup("Tails ~/logs/x until done");
        c = job_caption_normalize(home);
        CHECK_STR(c, want);
        free(c), free(home), free(want);
        c = job_clean_intent("  Watch\tthe\n build ");
        CHECK_STR(c, "Watch the build");
        free(c);
        CHECK(job_clean_intent("  \n") == NULL);
        /* toolIntentLabel (omp 4d08e232b3): the label before the first colon-space; URLs, file:line and bare labels stay */
        static const char *const IL[][2] = {{"[Checking retry]: Find where", "[Checking retry]"},
                                           {"Reading https://x.example/a", "Reading https://x.example/a"},
                                           {"Fixing src/a.c:12 bug", "Fixing src/a.c:12 bug"},
                                           {"Label:", "Label"},
                                           {": detail", ": detail"},
                                           {"  : detail", "  : detail"},
                                           {"Plain", "Plain"}};
        for (size_t i = 0; i < sizeof IL / sizeof IL[0]; i++) {
            c = tool_intent_label(IL[i][0]);
            CHECK_STR(c, IL[i][1]);
            free(c);
        }
        c = job_display_label(NULL, "lbl");
        CHECK_STR(c, "lbl");
        free(c);
        char *lng = xmalloc(301);
        memset(lng, 'x', 300);
        lng[300] = 0;
        c = job_truncate(lng, 200);
        CHECK(strlen(c) == 199 + 3 && str_ends(c, "\xE2\x80\xA6"));
        free(c), free(lng);
    }
    /* isColumnTruncationNotice / hasColumnTruncationClause: bracketed, a stated cap and unit, fractional caps too */
    CHECK(out_is_column_truncation_notice("[Some lines truncated to 768 chars]"));
    CHECK(out_is_column_truncation_notice("  [Some lines truncated to 768 bytes. Use :raw:3-3 to read line 3 whole]\r"));
    CHECK(out_is_column_truncation_notice("[Some lines truncated to 768.5 chars. Use :raw:1-1 to read line 1 whole]"));
    CHECK(out_is_column_truncation_notice("[Some lines truncated to 1e+21 chars]"));
    CHECK(!out_is_column_truncation_notice("[Some lines truncated to explain the format]"));
    CHECK(!out_is_column_truncation_notice("[Some lines truncated to explain the format"));
    CHECK(!out_is_column_truncation_notice("[Some lines truncated to 768 cats]"));
    CHECK(!out_is_column_truncation_notice("[Showing lines 1-4 of 5. Some lines truncated to 768 chars]"));
    CHECK(out_has_column_truncation_clause("[Showing lines 1-4 of 5. Use :5 to continue. Some lines truncated to 768 chars. Use :raw:1-1 to read line 1 whole]"));
    CHECK(out_has_column_truncation_clause("[Some lines truncated to 768 chars]"));
    CHECK(!out_has_column_truncation_clause("[Showing lines 1-4 of 5. Use :5 to continue]"));
    CHECK(!out_has_column_truncation_clause("Note. Some lines truncated to 768 chars]"));
    free(path);
}

static void test_small_and_notice(void) {
    out_sink_t *s = out_sink_new(NULL);
    out_sink_push(s, "hi\n", 3);
    out_summary_t sum;
    out_sink_dump(s, "Command timed out after 2 seconds", &sum);
    CHECK_STR(sum.output, "[Command timed out after 2 seconds]\nhi\n");
    CHECK(!sum.truncated && sum.total_lines == 2 && !sum.artifact_id);
    out_summary_free(&sum);
    out_sink_free(s);
    /* escape and CR split across chunks */
    s = out_sink_new(NULL);
    out_sink_push(s, "a\033[3", 4);
    out_sink_push(s, "1mb\r", 4);
    out_sink_push(s, "\nc\r", 3);
    out_sink_dump(s, NULL, &sum);
    CHECK_STR(sum.output, "ab\nc\n");
    out_summary_free(&sum);
    out_sink_free(s);
    /* empty output */
    s = out_sink_new(NULL);
    out_sink_dump(s, NULL, &sum);
    CHECK_STR(sum.output, "");
    CHECK(sum.total_lines == 0);
    out_summary_free(&sum);
    out_sink_free(s);
}

static void test_artifact_cap(const char *dir) {
    char *path = xasprintf("%s/2.bash.log", dir);
    out_sink_opts_t o = {.artifact_path = path, .artifact_id = "2", .spill_threshold = 64, .head_bytes = 0,
                         .artifact_max_bytes = 100, .artifact_head_bytes = -1};
    out_sink_t *s = out_sink_new(&o);
    for (int i = 0; i < 100; i++) {
        char b[16];
        int n = snprintf(b, sizeof b, "%04d\n", i);
        out_sink_push(s, b, (size_t)n);
    }
    out_summary_t sum;
    out_sink_dump(s, NULL, &sum);
    CHECK(sum.truncated && sum.artifact_elided_bytes == 400);
    char *art = fs_read(path, NULL);
    CHECK(art && strstr(art, "[ARTIFACT TRUNCATED: kept first 50B + last 50B of 500B; 400B elided from the middle]"));
    free(art);
    json_t *meta = NULL;
    out_meta_from_summary(&meta, &sum, "tail", 1, 0);
    char *notice = out_format_notice(meta);
    CHECK(strstr(notice, "Read artifact://2 for a head/tail sample of the output; 400B from its middle was not saved"));
    free(notice);
    json_free(meta);
    out_summary_free(&sum);
    out_sink_free(s);
    free(path);
}

static void test_truncate(void) {
    sb_t b = SB_INIT;
    for (int i = 0; i < 2000; i++) sb_appendf(&b, "line %d\n", i);
    out_trunc_t r;
    out_truncate_tail(b.s, b.len, 1000, 50, &r);
    CHECK(r.truncated && r.output_lines <= 50 && str_ends(r.content, "line 1999\n"));
    out_trunc_free(&r);
    out_truncate_head(b.s, b.len, 100000, 10, &r);
    CHECK(r.truncated && r.by == OUT_BY_LINES && r.output_lines == 10 && str_ends(r.content, "line 9"));
    out_trunc_free(&r);
    out_truncate_middle(b.s, b.len, 2000, 1000, 1000, 500, &r);
    CHECK(r.truncated && r.by == OUT_BY_MIDDLE && strstr(r.content, "ln elided") && r.head_lines > 0 && r.tail_lines > 0);
    json_t *meta = NULL;
    out_meta_from_trunc(&meta, &r, "middle", 1, 0, "3", 0, 0);
    char *n = out_format_notice(meta);
    CHECK(strstr(n, "middle lines") && str_ends(n, "Read artifact://3 for full output]"));
    free(n);
    json_free(meta);
    out_trunc_free(&r);
    out_truncate_middle("short", 5, 2000, 1000, 1000, 500, &r);
    CHECK(!r.truncated && !strcmp(r.content, "short"));
    out_trunc_free(&r);
    bool capped;
    char *c = out_enforce_inline_cap(b.s, b.len, 1000, "7", &capped);
    CHECK(capped && strstr(c, "B elided") && str_ends(c, "[raw output: artifact://7]"));
    free(c);
    sb_free(&b);
}

static void test_bash_execution_text(void) {
    json_t *m = json_parse_str("{\"role\":\"bashExecution\",\"command\":\"ls\",\"output\":\"a\",\"exitCode\":2,\"cancelled\":false}");
    char *t = out_bash_execution_text(m);
    CHECK_STR(t, "Ran `ls`\n```\na\n```\n\nCommand exited with code 2");
    free(t);
    json_free(m);
}

int main(void) {
    char *dir = fs_mktempdir("omc-outsink-test");
    test_sanitize();
    test_format();
    test_seq(dir);
    test_columns(dir);
    test_small_and_notice();
    test_artifact_cap(dir);
    test_truncate();
    test_bash_execution_text();
    fs_rm_rf(dir);
    free(dir);
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    else printf("outsink: ok\n");
    return fails != 0;
}
