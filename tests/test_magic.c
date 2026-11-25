/* test_magic.c - `edit.mode: magic` (tools/magic.c): initial file views, the draft tools, written tool calls, the
 * described-change loop against the real replace engine, JSON parse status and payload sections. Cases from omp's
 * edit-magic.test.ts (v19.1.1); the syntax case uses a JSON file, the language omc parses (omp checks TypeScript too). */
#include "core/core.h"
#include "core/fs.h"
#include "core/json.h"
#include "tools/magic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)
#define CHECK_STR(got, want)                                                                                           \
    do {                                                                                                               \
        const char *g_ = (got), *w_ = (want);                                                                          \
        if (!g_ || strcmp(g_, w_)) {                                                                                   \
            fprintf(stderr, "FAIL %s:%d:\n  got  %s\n  want %s\n", __FILE__, __LINE__, g_ ? g_ : "(null)", w_);        \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)
#define CHECK_HAS(got, part)                                                                                           \
    do {                                                                                                               \
        const char *g_ = (got), *p_ = (part);                                                                          \
        if (!g_ || !strstr(g_, p_)) {                                                                                  \
            fprintf(stderr, "FAIL %s:%d: missing \"%s\" in:\n%s\n", __FILE__, __LINE__, p_, g_ ? g_ : "(null)");      \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)
#define CHECK_LACKS(got, part)                                                                                         \
    do {                                                                                                               \
        const char *g_ = (got), *p_ = (part);                                                                          \
        if (!g_ || strstr(g_, p_)) {                                                                                   \
            fprintf(stderr, "FAIL %s:%d: unexpected \"%s\" in:\n%s\n", __FILE__, __LINE__, p_, g_ ? g_ : "(null)");   \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

#define G "\xe2\x94\x82" /* │ */
#define OMIT "\xe2\x8b\xae"

static const char *SOURCE = "function total(items: number[]) {\n"
                            "\tlet sum = 0;\n"
                            "\tfor (const item of items) {\n"
                            "\t\tsum += item;\n"
                            "\t}\n"
                            "\tconsole.log(sum);\n"
                            "\treturn sum;\n"
                            "}\n";

/* `count` eight-line functions step0…; stepN opens on line 8*N+1 */
static char *long_source(int count) {
    sb_t b = SB_INIT;
    for (int n = 0; n < count; n++)
        sb_appendf(&b,
                   "export function step%d(input: number): number {\n\tconst doubled = input * 2;\n\tif (doubled > %d) {\n"
                   "\t\treturn doubled - %d;\n\t}\n\treturn doubled;\n}\n\n",
                   n, n, n);
    return sb_detach(&b);
}

static char *replace_first(const char *s, const char *from, const char *to) {
    const char *m = strstr(s, from);
    if (!m) return xstrdup(s);
    return xasprintf("%.*s%s%s", (int)(m - s), s, to, m + strlen(from));
}

static char *replace_all(const char *s, const char *from, const char *to) { return str_replace(s, from, to); }

/* markers ("⋮ …" lines) of a view, joined by "|" */
static char *markers(const char *text) {
    strvec_t lines = str_split(text, "\n"), out = {0};
    for (size_t i = 0; i < lines.n; i++)
        if (str_starts(lines.v[i], OMIT)) strvec_push(&out, lines.v[i]);
    char *r = strvec_join(&out, "|");
    strvec_free(&lines);
    strvec_free(&out);
    return r;
}

static size_t count_markers(const char *text) {
    char *m = markers(text);
    size_t n = *m ? str_count(m, "|") + 1 : 0;
    free(m);
    return n;
}

/* ---------- fileViews ---------- */

static void test_file_views(void) {
    char *lng = long_source(50);
    magic_views_t v;
    magic_file_views(lng, "In `step30()`, replace `return doubled - 30;` with `return doubled + 30;`.", -1, &v);
    CHECK(v.n == 2 && v.v[0].partial && !v.v[1].partial);
    CHECK(v.v[0].total_lines == 400 && v.v[1].total_lines == 400);
    char *m = markers(v.v[0].text);
    CHECK_STR(m, OMIT " lines 13-228 not shown|" OMIT " lines 260-388 not shown");
    free(m);
    CHECK_HAS(v.v[0].text, "241" G "export function step30(input: number): number {\n242" G "\tconst doubled = input * 2;");
    CHECK(str_starts(v.v[0].text, "1" G "export function step0("));
    CHECK(str_ends(v.v[0].text, "400" G));
    CHECK_HAS(v.v[1].text, "\n100" G "\t\treturn doubled - 12;\n");
    magic_views_free(&v);

    /* qualified method names, ln references and repeating lines; tiny gaps merge */
    strvec_t lines = {0};
    for (int i = 0; i < 260; i++) strvec_push_own(&lines, xasprintf("\tlet v%d = %d;", i, i));
    free(lines.v[0]), lines.v[0] = xstrdup("impl Line {");
    free(lines.v[120]), lines.v[120] = xstrdup("\tfn blank() {");
    free(lines.v[121]), lines.v[121] = xstrdup("\t\treturn 0;");
    free(lines.v[122]), lines.v[122] = xstrdup("\t}");
    free(lines.v[259]), lines.v[259] = xstrdup("}");
    char *content = strvec_join(&lines, "\n");
    strvec_free(&lines);
    magic_file_views(content, "In `Line::blank`, ln 150, and ln 180, update body.", -1, &v);
    CHECK(v.v[0].partial);
    CHECK_HAS(v.v[0].text, "121" G "\tfn blank() {");
    CHECK_HAS(v.v[0].text, "\n150" G);
    CHECK_HAS(v.v[0].text, "\n165" G);
    CHECK_HAS(v.v[0].text, "\n180" G);
    CHECK(count_markers(v.v[0].text) == 2);
    {
        strvec_t ls = str_split(v.v[0].text, "\n");
        for (size_t i = 0; i < ls.n; i++) {
            int a, b;
            if (sscanf(ls.v[i], OMIT " lines %d-%d not shown", &a, &b) == 2) CHECK(b - a + 1 > 8);
        }
        strvec_free(&ls);
    }
    magic_views_free(&v);
    free(content);
    /* `const doubled = input * 2;` is in all fifty functions; co-location keeps only step30's copy */
    magic_file_views(lng, "In `step30()`, change `const doubled = input * 2;` to `const doubled = input * 3;`.", -1, &v);
    CHECK_HAS(v.v[0].text, "\n242" G "\tconst doubled = input * 2;\n");
    CHECK_LACKS(v.v[0].text, "\n202" G);
    CHECK(count_markers(v.v[0].text) == 2);
    magic_views_free(&v);

    /* whole files when short or mostly named; head+tail when an unnamed file exceeds the limit */
    magic_file_views(SOURCE, "In `total`, delete `console.log(sum);`.", -1, &v);
    CHECK(v.n == 1 && !v.v[0].partial);
    magic_views_free(&v);
    magic_file_views(lng, "Tidy up the error handling.", -1, &v);
    CHECK(v.n == 1 && !v.v[0].partial);
    magic_views_free(&v);
    sb_t many = SB_INIT;
    for (int n = 0; n < 25; n++) sb_appendf(&many, "%s`step%d(`", n ? ", " : "", 2 * n);
    char *change = xasprintf("Add a comment above each of %s.", sb_cstr(&many));
    sb_free(&many);
    magic_file_views(lng, change, -1, &v);
    CHECK(v.n == 1 && !v.v[0].partial);
    magic_views_free(&v);
    free(change);
    magic_file_views(lng, "In `step30()`, return `doubled + 30`.", 4000, &v);
    CHECK(v.n == 1 && v.v[0].partial);
    magic_views_free(&v);
    magic_file_views(lng, "Tidy up the error handling.", 4000, &v);
    CHECK(v.n == 1 && v.v[0].partial);
    CHECK_HAS(v.v[0].text, OMIT " Open more of the file");
    CHECK_LACKS(v.v[0].text, "step30(");
    magic_views_free(&v);
    free(lng);
}

/* ---------- draft tools ---------- */

/* applyReplace on text with args (JSON): the error, or the new draft followed by "\n----\n" and the message */
static char *apply_replace(const char *text, const char *args_json) {
    json_t *args = json_parse_str(args_json);
    char *content = NULL, *message = NULL, *error = NULL;
    magic_step_t st;
    char *r;
    if (magic_apply_replace(text, args, &content, &st, &message, &error)) {
        r = xasprintf("%s\n----\n%s", content, message);
        free(st.old_s);
        free(st.rep);
    } else {
        r = xasprintf("ERROR %s", error);
    }
    free(content);
    free(message);
    free(error);
    json_free(args);
    return r;
}

static void test_tools(void) {
    /* ambiguous anchors need replace_all; replacement text is literal */
    char *r = apply_replace(SOURCE, "{\"old_string\":\"sum\",\"new_string\":\"$&\"}");
    CHECK_HAS(r, "ERROR old_string matches more than once (first on line 2). Include more context or pass replace_all: true.");
    free(r);
    r = apply_replace(SOURCE, "{\"old_string\":\"sum\",\"new_string\":\"$&\",\"replace_all\":true}");
    char *want = replace_all(SOURCE, "sum", "$&");
    CHECK(str_starts(r, want));
    free(want);
    free(r);

    /* the copied gutter is pointed out with the exact draft lines, nothing staged */
    const char *src = "header\n\tstart();\n    first();\n\tsecond();\nend();\ncleanup();\ntrailer\n";
    const char *gargs = "{\"old_string\":\"2" G "\\tstart();\\n\\t    first();\\n\\tsecond();\\n5\\tend();\\n" G "cleanup();\",\"new_string\":\"logged();\"}";
    r = apply_replace(src, gargs);
    CHECK_HAS(r, "ERROR old_string not found. The line-number gutter was copied.");
    CHECK_HAS(r, "2" G "\tstart();\n3" G "    first();\n4" G "\tsecond();\n5" G "end();\n6" G "cleanup();");
    free(r);
    char *twice = xasprintf("%s%s", src, src);
    r = apply_replace(twice, gargs);
    CHECK_HAS(r, "Search for a short fragment");
    free(r);
    free(twice);

    /* whitespace drift: exact numbered lines only for a unique match */
    src = "start();\n\tconst count = 1;\n\treturn count + 2;\nend();\n";
    const char *wargs = "{\"old_string\":\"    const count=1;\\n    return count+2;\",\"new_string\":\"return 0;\"}";
    r = apply_replace(src, wargs);
    CHECK_STR(r, "ERROR old_string not found. Whitespace differs. Copy the exact text after " G ":\n2" G "\tconst count = 1;\n3" G
                 "\treturn count + 2;");
    free(r);
    twice = xasprintf("%s%s", src, src);
    r = apply_replace(twice, wargs);
    CHECK_HAS(r, "Search for a short fragment");
    free(r);
    free(twice);

    /* an already-present replacement */
    r = apply_replace("header\nreturn ready;\n", "{\"old_string\":\"return pending;\",\"new_string\":\"return ready;\"}");
    CHECK_STR(r, "ERROR old_string not found. new_string is already present on line 2. Check if this change was already applied.");
    free(r);

    /* post-edit line numbers, the replacement and two lines around it */
    src = "far before\nbefore one\nbefore two\nold\nafter one\nafter two\nfar after";
    r = apply_replace(src, "{\"old_string\":\"old\",\"new_string\":\"new one\\nnew two\"}");
    CHECK(str_ends(r, "Replaced text on line 4.\nDraft near the edit:\n2" G "before one\n3" G "before two\n4" G "new one\n5" G
                      "new two\n6" G "after one\n7" G "after two"));
    free(r);

    /* a wide line keeps the edit visible without echoing the line */
    sb_t wide = SB_INIT;
    for (int i = 0; i < 10000; i++) sb_putc(&wide, 'x');
    sb_append(&wide, "old");
    for (int i = 0; i < 10000; i++) sb_putc(&wide, 'y');
    r = apply_replace(sb_cstr(&wide), "{\"old_string\":\"old\",\"new_string\":\"updated\"}");
    const char *msg = strstr(r, "\n----\n");
    CHECK(msg && strlen(msg) < 2000);
    CHECK_HAS(msg, "\n1" G "\xe2\x80\xa6xxxxxxxxxx");
    CHECK_HAS(msg, "xupdatedy");
    CHECK(msg && str_ends(msg, "y\xe2\x80\xa6"));
    free(r);
    sb_free(&wide);

    /* every match counted, the original lines capped */
    sb_t olds = SB_INIT;
    for (int i = 0; i < 60; i++) sb_append(&olds, "old old\n");
    r = apply_replace(sb_cstr(&olds), "{\"old_string\":\"old\",\"new_string\":\"new\\ntext\",\"replace_all\":true}");
    CHECK_HAS(r, "\n----\nReplaced 120 matches (original lines 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, \xe2\x80\xa6).\nDraft near the first edit:");
    free(r);
    sb_free(&olds);

    r = apply_replace("start\n\t \nend", "{\"old_string\":\"\\t \\n\",\"new_string\":\"\",\"replace_all\":true}");
    CHECK_STR(r, "ERROR replace_all cannot be used on whitespace-only old_string.");
    free(r);
    r = apply_replace("delete this", "{\"old_string\":\"delete this\"}");
    CHECK_STR(r, "ERROR new_string is required; pass \"\" to delete.");
    free(r);
    /* omptype failures: descriptions as the expectation, all fields reported */
    r = apply_replace("x", "{\"old_string\":\"\",\"new_string\":5,\"replace_all\":\"yes\"}");
    CHECK_STR(r, "ERROR old_string must be Exact text, including any whitespace you want to match. (was 0)\nnew_string must be "
                 "Replacement text; empty to delete. (was a number)\nreplace_all must be Default false. Set true only to "
                 "replace every occurrence in the draft. (was \"yes\")");
    free(r);
    r = apply_replace("a\nb", "{\"old_string\":\"a\\u0000\",\"new_string\":\"b\"}");
    CHECK_STR(r, "ERROR NUL bytes are not allowed in text edits.");
    free(r);

    /* read: bounded output, resumable after an oversized line */
    sb_t big = SB_INIT;
    for (int i = 0; i < 20000; i++) sb_putc(&big, 'x');
    sb_append(&big, "\nlet next = 1;\n");
    json_t *a = json_parse_str("{\"start_line\":1,\"end_line\":2}");
    bool err;
    r = magic_read_lines(sb_cstr(&big), a, &err);
    CHECK(!err && strlen(r) < 8100 * 3);
    CHECK_HAS(r, "starting at line 2");
    free(r);
    json_free(a);
    a = json_parse_str("{\"start_line\":2,\"end_line\":2}");
    r = magic_read_lines(sb_cstr(&big), a, &err);
    CHECK_HAS(r, "2" G "let next = 1;");
    free(r);
    json_free(a);
    sb_free(&big);
    a = json_parse_str("{\"start_line\":0.5,\"end_line\":3}");
    r = magic_read_lines(SOURCE, a, &err);
    CHECK(err);
    CHECK_STR(r, "start_line (0.5) must be...\n  \xe2\x97\xa6 First line, 1-based.\n  \xe2\x97\xa6 First line, 1-based.");
    free(r);
    json_free(a);
    a = json_parse_str("{\"start_line\":5,\"end_line\":2}");
    r = magic_read_lines(SOURCE, a, &err);
    CHECK(err);
    CHECK_STR(r, "Invalid range for 8 lines.");
    free(r);
    json_free(a);
    a = json_parse_str("{\"start_line\":1,\"end_line\":1}");
    r = magic_read_lines("", a, &err);
    CHECK(!err);
    CHECK_STR(r, "The draft is empty.");
    free(r);
    json_free(a);
    /* search: literal, numbered, counted */
    a = json_parse_str("{\"query\":\"sum\"}");
    r = magic_search(SOURCE, a, &err);
    CHECK_STR(r, "4 matches:\n2" G "\tlet sum = 0;\n4" G "\t\tsum += item;\n6" G "\tconsole.log(sum);\n7" G "\treturn sum;");
    free(r);
    json_free(a);
    a = json_parse_str("{\"query\":\"\"}");
    r = magic_search(SOURCE, a, &err);
    CHECK(err);
    CHECK_STR(r, "query must be Literal substring to find. (was 0)");
    free(r);
    json_free(a);
}

/* ---------- written calls ---------- */

static char *written(const char *text) {
    bool attempted;
    json_t *calls = magic_parse_written_calls(text, &attempted);
    char *d = json_dump(calls, 0);
    char *r = xasprintf("%s%s", attempted ? "A " : "- ", d);
    free(d);
    json_free(calls);
    return r;
}

static void test_written_calls(void) {
    char *r = written("I'll update the return value.\nreplace(old_string=\"return sum;\", new_string='return sum + \\x31;', replace_all=True)");
    CHECK_STR(r, "A [{\"name\":\"replace\",\"arguments\":{\"old_string\":\"return sum;\",\"new_string\":\"return sum + 1;\",\"replace_all\":true}}]");
    free(r);
    r = written("```tool_code\nreplace(old_string=\"\"\"\tconsole.log(sum);\n\treturn sum;\"\"\",\nnew_string=\"\"\"x\"\"\", replace_all=False)\n```");
    CHECK_STR(r, "A [{\"name\":\"replace\",\"arguments\":{\"old_string\":\"\\tconsole.log(sum);\\n\\treturn sum;\",\"new_string\":\"x\",\"replace_all\":false}}]");
    free(r);
    r = written("```python\n[read(start_line=7, end_line=0x7),\nsearch(query='a\\'b'),]\n```");
    CHECK_STR(r, "A [{\"name\":\"read\",\"arguments\":{\"start_line\":7,\"end_line\":7}},{\"name\":\"search\",\"arguments\":{\"query\":\"a'b\"}}]");
    free(r);
    /* Gemma's spellings: arguments beside `function`, a nested `function` object, the tool as the only key */
    r = written("```tool_code\n[{\"function\": \"replace\", \"old_string\": \"a\", \"new_string\": \"\", \"replace_all\": null},"
                " {\"function\": {\"name\": \"search\", \"parameters\": {\"query\": \"x\"}}}]\n```\n```json\n{\"replace\": "
                "{\"old_string\": \"b\", \"new_string\": \"c\"}}\n```\n```json\n{\"name\": \"write\", \"arguments\": {}}\n```");
    CHECK_STR(r, "A [{\"name\":\"replace\",\"arguments\":{\"old_string\":\"a\",\"new_string\":\"\",\"replace_all\":null}},{\"name\":"
                 "\"search\",\"arguments\":{\"query\":\"x\"}},{\"name\":\"replace\",\"arguments\":{\"old_string\":\"b\","
                 "\"new_string\":\"c\"}}]");
    free(r);
    /* Python literals outside strings, untouched inside them */
    r = written("```json\n[{\"replace\":{\"old_string\":\"a\",\"new_string\":\"b // False None True\",\"replace_all\":True}}]\n```");
    CHECK_STR(r, "A [{\"name\":\"replace\",\"arguments\":{\"old_string\":\"a\",\"new_string\":\"b // False None True\",\"replace_all\":true}}]");
    free(r);
    /* arguments as a JSON string */
    r = written("```json\n{\"name\":\"read\",\"arguments\":\"{\\\"start_line\\\":1,\\\"end_line\\\":2}\"}\n```");
    CHECK_STR(r, "A [{\"name\":\"read\",\"arguments\":{\"start_line\":1,\"end_line\":2}}]");
    free(r);
    /* prose that mentions a call is no call and no attempt */
    r = written("I can use replace(old_string=\"return sum;\", new_string=\"return sum + 999;\") for this.");
    CHECK_STR(r, "- []");
    free(r);
    /* attempts that never run: positional arguments, expressions, an unknown tool in a list, a missing comma */
    r = written("replace(\"return sum;\", \"return sum + 999;\")");
    CHECK_STR(r, "A []");
    free(r);
    r = written("replace(old_string=\"return sum;\", new_string=\"return \" + \"sum;\")");
    CHECK_STR(r, "A []");
    free(r);
    r = written("[replace(old_string=\"a\", new_string=\"b\"), write()]");
    CHECK_STR(r, "A []");
    free(r);
    r = written("```tool_code\n[replace(old_string=\"a\" new_string=\"b\")]\n```");
    CHECK_STR(r, "A []");
    free(r);
    r = written("```json\n{\"name\": \"replace\", \"arguments\": \n```");
    CHECK_STR(r, "A []");
    free(r);
    r = written("```\nplain code\n```\nDONE");
    CHECK_STR(r, "- []");
    free(r);
    /* an open fence that is a tool fence counts as an attempt */
    r = written("```tool_code\nreplace(old_string=\"a\"");
    CHECK_STR(r, "A []");
    free(r);

    /* normalizeApplyArguments */
    json_t *call = json_parse_str("{\"name\":\"replace\",\"arguments\":{\"old_string\":\"a\",\"new_string\":\"b\",\"replace_all\":null}}");
    bool repaired;
    char *e = NULL;
    json_t *args = magic_normalize_args(call, &repaired, &e);
    char *d = json_dump(args, 0);
    CHECK_STR(d, "{\"old_string\":\"a\",\"new_string\":\"b\"}");
    free(d);
    json_free(args);
    json_free(call);
    call = json_parse_str("{\"name\":\"read\",\"arguments\":{\"start_line\":\"7\",\"end_line\":\"+8\"}}");
    args = magic_normalize_args(call, &repaired, &e);
    d = json_dump(args, 0);
    CHECK_STR(d, "{\"start_line\":7,\"end_line\":8}");
    free(d);
    json_free(args);
    json_free(call);
    call = json_parse_str("{\"name\":\"replace\",\"arguments\":{\"__parseError\":\"Unexpected non-whitespace after JSON object\","
                          "\"__rawJson\":\"{\\\"old_string\\\":\\\"a\\\",\\\"new_string\\\":\\\"b\\\"}{\\\"old_string\\\":\\\"a\\\"}\"}}");
    args = magic_normalize_args(call, &repaired, &e);
    CHECK(!args);
    CHECK_STR(e, "Malformed replace arguments: Unexpected non-whitespace after JSON object\nSend one JSON object per call.");
    free(e);
    e = NULL;
    json_free(call);
    call = json_parse_str("{\"name\":\"replace\",\"arguments\":{\"__parseError\":\"x\",\"__rawJson\":\"{\\\"old_string\\\":\\\"a\\\","
                          "\\\"new_string\\\":\\\"} {\\\"} \\n {\\\"new_string\\\":\\\"} {\\\",\\\"old_string\\\":\\\"a\\\"}\"}}");
    args = magic_normalize_args(call, &repaired, &e);
    CHECK(args && repaired);
    d = json_dump(args, 0);
    CHECK_STR(d, "{\"old_string\":\"a\",\"new_string\":\"} {\"}");
    free(d);
    json_free(args);
    json_free(call);
}

/* ---------- applyDescribedChange against a file through the real replace engine ---------- */

typedef struct {
    json_t *turns; /* scripted assistant messages */
    size_t next;
    json_t *transcripts;
    const char *dir, *name;
    char *raw;
} script_t;

static json_t *s_complete(magic_io_t *io, const json_t *messages, const json_t *tools, char **err) {
    script_t *s = io->ud;
    UNUSED(tools);
    json_push(s->transcripts, json_clone(messages));
    if (s->next >= json_len(s->turns)) {
        *err = xstrdup("the model was asked for more turns than scripted");
        return NULL;
    }
    return json_clone(json_at(s->turns, s->next++));
}

static void s_apply(magic_io_t *io, const char *old_s, const char *new_s, pe_result_t *r) {
    script_t *s = io->ud;
    json_t *args = json_obj(), *edits = json_arr(), *e = json_obj();
    json_set_str(args, "path", s->name);
    json_set_str(e, "old_string", old_s);
    json_set_str(e, "new_string", new_s);
    json_set_bool(e, "replace_all", true);
    json_push(edits, e);
    json_set(args, "edits", edits);
    pe_opts_t o = {.cwd = s->dir, .allow_fuzzy = true, .fuzzy_threshold = 0.95, .raw_snapshot = s->raw, .raw_snapshot_len = strlen(s->raw)};
    pe_run(PE_REPLACE, args, &o, r);
    json_free(args);
}

/* an assistant turn: native calls ([["name", {args}], ...]) */
static json_t *tool_msg(const char *calls_json) {
    static int seq;
    json_t *calls = json_parse_str(calls_json), *m = json_obj(), *content = json_arr(), *c;
    json_set_str(m, "role", "assistant");
    json_arr_foreach(calls, i, c) {
        json_t *b = json_obj();
        json_set_str(b, "type", "toolCall");
        char *id = xasprintf("tc_%d", ++seq);
        json_set_str(b, "id", id);
        free(id);
        json_set_str(b, "name", json_sv(json_at(c, 0)));
        json_set(b, "arguments", json_clone(json_at(c, 1)));
        json_push(content, b);
    }
    json_set(m, "content", content);
    json_set_str(m, "stopReason", "toolUse");
    json_free(calls);
    return m;
}

static json_t *text_msg_stop(const char *text, const char *stop) {
    json_t *m = json_obj(), *content = json_arr(), *b = json_obj();
    json_set_str(m, "role", "assistant");
    json_set_str(b, "type", "text");
    json_set_str(b, "text", text);
    json_push(content, b);
    json_set(m, "content", content);
    json_set_str(m, "stopReason", stop);
    return m;
}
static json_t *text_msg(const char *text) { return text_msg_stop(text, "stop"); }

typedef struct {
    bool ok;
    char *err;
    char *after;
    json_t *transcripts;
} outcome_t;

/* the turns (a NULL-terminated list, consumed) against `name` holding content */
static outcome_t run_file(const char *dir, const char *name, json_t **turns, const char *content, const char *change) {
    char *file = xasprintf("%s/%s", dir, name);
    fs_write(file, content, strlen(content));
    json_t *list = json_arr();
    for (size_t i = 0; turns[i]; i++) json_push(list, turns[i]);
    script_t s = {list, 0, json_arr(), dir, name, xstrdup(content)};
    magic_views_t views;
    magic_file_views(content, change, -1, &views);
    magic_io_t io = {s_complete, s_apply, &s};
    pe_result_t res;
    outcome_t o = {0};
    o.ok = magic_apply_described(name, change, content, &views, &io, &res, &o.err);
    if (o.ok) pe_result_free(&res);
    size_t len;
    o.after = fs_read(file, &len);
    o.transcripts = s.transcripts;
    magic_views_free(&views);
    json_free(list);
    free(s.raw);
    free(file);
    return o;
}
static outcome_t run(const char *dir, json_t **turns) { return run_file(dir, "sample.ts", turns, SOURCE, "described change"); }

static void outcome_free(outcome_t *o) {
    free(o->err);
    free(o->after);
    json_free(o->transcripts);
}

static const json_t *transcript(const outcome_t *o, size_t n) { return json_at(o->transcripts, n); }

/* the text of a message (string content, or its text blocks) */
static char *msg_text(const json_t *m) {
    const json_t *c = json_get(m, "content");
    if (json_is_str(c)) return xstrdup(c->u.str.s);
    sb_t b = SB_INIT;
    json_t *blk;
    json_arr_foreach(c, i, blk) if (json_get_str(blk, "text")) sb_append(&b, json_get_str(blk, "text"));
    return sb_detach(&b);
}

/* the last message of transcript n, as text */
static char *last_text(const outcome_t *o, size_t n) {
    const json_t *t = transcript(o, n);
    return t && json_len(t) ? msg_text(json_at(t, json_len(t) - 1)) : xstrdup("");
}

/* tool results of transcript n: "ok"/"error" joined by "," */
static char *results(const outcome_t *o, size_t n) {
    strvec_t v = {0};
    json_t *m;
    json_arr_foreach(transcript(o, n), i, m) if (str_eq(json_get_str(m, "role"), "toolResult"))
        strvec_push(&v, json_get_bool(m, "isError", false) ? "error" : "ok");
    char *r = strvec_join(&v, ",");
    strvec_free(&v);
    return r;
}

/* the text of the k-th tool result of transcript n */
static char *result_text(const outcome_t *o, size_t n, size_t k) {
    json_t *m;
    size_t seen = 0;
    json_arr_foreach(transcript(o, n), i, m) if (str_eq(json_get_str(m, "role"), "toolResult")) {
        if (seen++ == k) return msg_text(m);
    }
    return xstrdup("");
}

static char *fenced(const char *lang, const char *body) { return xasprintf("```%s\n%s\n```", lang, body); }

static void test_apply(const char *dir) {
    outcome_t o;
    {
        /* calls written as fenced JSON instead of native calls */
        char *f1 = fenced("tool_code", "[\n  {\"function\": \"replace\", \"old_string\": \"\\tconsole.log(sum);\\n\", \"new_string\": \"\", "
                                       "\"replace_all\": null},\n  {\"function\": {\"name\": \"search\", \"parameters\": {\"query\": \"return sum\"}}}\n]");
        char *f2 = fenced("json", "{\"replace\": {\"old_string\": \"items: number[]\", \"new_string\": \"items: readonly number[]\"}}");
        char *f3 = fenced("json", "{\"name\": \"write\", \"arguments\": {}}");
        char *t2 = xasprintf("%s\n%s", f2, f3);
        json_t *turns[] = {text_msg(f1), text_msg(t2), text_msg("DONE"), NULL};
        o = run(dir, turns);
        CHECK(o.ok);
        char *r = results(&o, 1);
        CHECK_STR(r, "ok,ok");
        free(r);
        r = results(&o, 2);
        CHECK_STR(r, "ok,ok,ok");
        free(r);
        char *w1 = replace_first(SOURCE, "\tconsole.log(sum);\n", ""), *w2 = replace_first(w1, "items: number[]", "items: readonly number[]");
        CHECK_STR(o.after, w2);
        free(w1), free(w2), free(f1), free(f2), free(f3), free(t2);
        outcome_free(&o);
    }
    {
        /* several replaces across turns; a miss keeps the successful ones */
        json_t *turns[] = {tool_msg("[[\"replace\",{\"old_string\":\"\\tconsole.log(sum);\\n\",\"new_string\":\"\"}],[\"replace\",{\"old_"
                                    "string\":\"\\treturn sum_wrong;\",\"new_string\":\"\\treturn sum + 1;\"}]]"),
                           text_msg("DONE"), tool_msg("[[\"read\",{\"start_line\":5,\"end_line\":7}]]"),
                           tool_msg("[[\"replace\",{\"old_string\":\"\\treturn sum;\",\"new_string\":\"\\treturn sum + 1;\"}]]"),
                           text_msg("DONE"), NULL};
        o = run(dir, turns);
        CHECK(o.ok);
        char *r = results(&o, 1);
        CHECK_STR(r, "ok,error");
        free(r);
        r = result_text(&o, 1, 1);
        CHECK_HAS(r, "old_string not found");
        free(r);
        r = last_text(&o, 2);
        CHECK_HAS(r, "replacement failed");
        free(r);
        r = last_text(&o, 3);
        CHECK_HAS(r, "6" G "\treturn sum;");
        free(r);
        char *w1 = replace_first(SOURCE, "\tconsole.log(sum);\n", ""), *w2 = replace_first(w1, "return sum;", "return sum + 1;");
        CHECK_STR(o.after, w2);
        free(w1), free(w2);
        outcome_free(&o);
    }
    char *lng = long_source(50);
    {
        /* omitted regions of a long file through search and read */
        json_t *turns[] = {tool_msg("[[\"search\",{\"query\":\"step10(\"}]]"), tool_msg("[[\"read\",{\"start_line\":81,\"end_line\":88}]]"),
                           tool_msg("[[\"replace\",{\"old_string\":\"return doubled - 10;\",\"new_string\":\"return doubled + 10;\"}]]"),
                           text_msg("DONE"), NULL};
        o = run_file(dir, "sample.ts", turns, lng,
                     "In `step30()`, check how the earlier helpers work, and make the tenth helper add its threshold.");
        CHECK(o.ok && json_len(o.transcripts) == 4);
        char *first = msg_text(json_at(transcript(&o, 0), 0));
        CHECK_LACKS(first, "export function step10(");
        free(first);
        char *r = last_text(&o, 1);
        CHECK_HAS(r, "81" G "export function step10(");
        free(r);
        r = last_text(&o, 2);
        CHECK_HAS(r, "84" G "\t\treturn doubled - 10;");
        free(r);
        char *w = replace_first(lng, "return doubled - 10;", "return doubled + 10;");
        CHECK_STR(o.after, w);
        free(w);
        outcome_free(&o);
    }
    {
        /* a draft that stops parsing is caught before writing, the breaking replace named with live lines, the repair
         * committed (omp checks TypeScript too; omc parses JSON) */
        const char *pkg = "{\n  \"items\": [1, 2],\n  \"total\": 3\n}\n";
        json_t *turns[] = {tool_msg("[[\"replace\",{\"old_string\":\"\\\"total\\\": 3\",\"new_string\":\"\\\"total\\\": 4\"}],[\"replace\","
                                    "{\"old_string\":\"[1, 2],\",\"new_string\":\"[1, 2]\"}]]"),
                           text_msg("DONE"),
                           tool_msg("[[\"replace\",{\"old_string\":\"[1, 2]\\n\",\"new_string\":\"[1, 2],\\n\"}]]"), text_msg("DONE"), NULL};
        o = run_file(dir, "sample.json", turns, pkg, "described change");
        CHECK(o.ok && json_len(o.transcripts) == 4);
        char *fb = last_text(&o, 2);
        CHECK_STR(fb, "The draft no longer parses. The draft first stopped parsing after replace #2 (then at line 2). Current lines "
                      "near that location:\n1" G "{\n2" G "  \"items\": [1, 2]\n3" G "  \"total\": 4\n4" G "}\nUse `read` and "
                      "`replace` to repair the draft; successful edits are still in it.");
        free(fb);
        CHECK_STR(o.after, "{\n  \"items\": [1, 2],\n  \"total\": 4\n}\n");
        outcome_free(&o);
    }
    {
        /* a whole-file refusal ends the call; one on an uninspected excerpt starts over with the whole file */
        json_t *turns[] = {text_msg("CANNOT: `drain` is not in this file"), NULL};
        o = run(dir, turns);
        CHECK(!o.ok && json_len(o.transcripts) == 1);
        CHECK_STR(o.err, "No change was made to sample.ts: `drain` is not in this file");
        CHECK_STR(o.after, SOURCE);
        outcome_free(&o);
        json_t *turns2[] = {text_msg("CANNOT: target is omitted"),
                            tool_msg("[[\"replace\",{\"old_string\":\"return doubled - 30;\",\"new_string\":\"return doubled + 30;\"}]]"),
                            text_msg("DONE"), NULL};
        o = run_file(dir, "sample.ts", turns2, lng, "In `step30()`, replace `return doubled - 30;` with `return doubled + 30;`.");
        CHECK(o.ok && json_len(o.transcripts) == 3);
        char *a = msg_text(json_at(transcript(&o, 0), 0)), *b = msg_text(json_at(transcript(&o, 1), 0));
        CHECK_HAS(a, OMIT);
        CHECK_LACKS(b, OMIT);
        free(a), free(b);
        CHECK_HAS(o.after, "return doubled + 30;");
        outcome_free(&o);
    }
    free(lng);
    {
        /* prose instead of a call is nudged once */
        json_t *turns[] = {text_msg("I will update line 7 to return sum + 1."),
                           tool_msg("[[\"replace\",{\"old_string\":\"return sum;\",\"new_string\":\"return sum + 1;\"}]]"), text_msg("DONE"), NULL};
        o = run(dir, turns);
        CHECK(o.ok && json_len(o.transcripts) == 3);
        char *r = last_text(&o, 1);
        CHECK_STR(r, "Apply the requested changes with `replace` before replying `DONE`. Use `read` or `search` if needed.");
        free(r);
        char *w = replace_first(SOURCE, "return sum;", "return sum + 1;");
        CHECK_STR(o.after, w);
        free(w);
        outcome_free(&o);
    }
    {
        /* a turn cut at the output limit is neither staged nor replayed */
        json_t *cut = tool_msg("[[\"replace\",{\"old_string\":\"let sum = 0;\",\"new_string\":\"let sum = 99;\"}]]");
        json_set_str(cut, "stopReason", "length");
        json_t *turns[] = {cut, tool_msg("[[\"replace\",{\"old_string\":\"return sum;\",\"new_string\":\"return sum + 1;\"}]]"),
                           text_msg("DONE"), NULL};
        o = run(dir, turns);
        CHECK(o.ok && json_len(o.transcripts) == 3);
        char *r = last_text(&o, 1);
        CHECK_STR(r, "The last response hit the output limit. Use smaller `replace` calls, one section at a time.");
        free(r);
        CHECK(json_len(transcript(&o, 1)) == 2); /* the cut reply is not in the conversation */
        char *w = replace_first(SOURCE, "return sum;", "return sum + 1;");
        CHECK_STR(o.after, w);
        free(w);
        outcome_free(&o);
    }
    {
        /* DONE without a change, twice: the repeated feedback ends the section */
        json_t *turns[] = {text_msg("DONE"), text_msg("Done."), NULL};
        o = run(dir, turns);
        CHECK(!o.ok);
        CHECK_STR(o.err, "The change to sample.ts did not finish; the file is unchanged. Apply the requested changes with `replace` "
                         "before replying `DONE`. Use `read` or `search` if needed.");
        outcome_free(&o);
    }
    {
        /* an empty file is refused before any turn */
        json_t *turns[] = {NULL};
        o = run_file(dir, "empty.ts", turns, "", "x");
        CHECK(!o.ok && json_len(o.transcripts) == 0);
        CHECK_STR(o.err, "empty.ts is empty; use write instead.");
        outcome_free(&o);
    }
    {
        /* a file changed while the model answered is refused at write time */
        json_t *turns[] = {tool_msg("[[\"replace\",{\"old_string\":\"\\tconsole.log(sum);\\n\",\"new_string\":\"\"}]]"), text_msg("DONE"), NULL};
        char *file = xasprintf("%s/stale.ts", dir);
        fs_write(file, SOURCE, strlen(SOURCE));
        json_t *list = json_arr();
        for (size_t i = 0; turns[i]; i++) json_push(list, turns[i]);
        script_t s = {list, 0, json_arr(), dir, "stale.ts", xstrdup(SOURCE)};
        char *changed = replace_first(SOURCE, "let sum = 0;", "let sum = 10;");
        fs_write(file, changed, strlen(changed));
        magic_views_t views;
        magic_file_views(SOURCE, "x", -1, &views);
        magic_io_t io = {s_complete, s_apply, &s};
        pe_result_t res;
        char *err = NULL;
        CHECK(!magic_apply_described("stale.ts", "x", SOURCE, &views, &io, &res, &err));
        CHECK_STR(err, "The file changed while the edit was being prepared; re-read and retry.");
        size_t len;
        char *now = fs_read(file, &len);
        CHECK_STR(now, changed);
        free(now), free(err), free(changed), free(file), free(s.raw);
        json_free(s.transcripts);
        json_free(list);
        magic_views_free(&views);
    }
}

/* ---------- call intake ---------- */

static void test_intake(const char *dir) {
    char *revised = replace_first(SOURCE, "return sum;", "return sum + 1;");
    const char *upd = "[[\"replace\",{\"old_string\":\"return sum;\",\"new_string\":\"return sum + 1;\"}]]";
    struct {
        const char *t1, *t2, *want;
    } shapes[] = {
        {"I'll update the return value.\nreplace(old_string=\"return sum;\", new_string='return sum + \\x31;', replace_all=True)", NULL, "ok"},
        {"```tool_code\nreplace(old_string=\"\"\"\tconsole.log(sum);\n\treturn sum;\"\"\",\nnew_string=\"\"\"\tconsole.log(sum);\n\treturn sum + 1;\"\"\", "
         "replace_all=False)\n```",
         NULL, "ok"},
        {"```tool_response\n[read(start_line=7, end_line=7)]\n```", "replace(old_string='''return sum;''', new_string=\"return sum + 1;\")", "ok,ok"},
        {"```python\n[read(start_line=7, end_line=7),\nreplace(old_string='return sum;', new_string='return sum + 1;', replace_all=None),\n"
         "search(query='return sum + 1;')]\n```",
         NULL, "ok,ok,ok"},
    };
    for (size_t k = 0; k < sizeof shapes / sizeof *shapes; k++) {
        json_t *turns[] = {text_msg(shapes[k].t1), shapes[k].t2 ? text_msg(shapes[k].t2) : text_msg("DONE"), shapes[k].t2 ? text_msg("DONE") : NULL,
                           NULL};
        outcome_t o = run(dir, turns);
        CHECK(o.ok);
        char *r = results(&o, json_len(o.transcripts) - 1);
        CHECK_STR(r, shapes[k].want);
        free(r);
        CHECK_STR(o.after, revised);
        outcome_free(&o);
    }
    {
        /* Python literals in fenced JSON, words inside replacement text untouched */
        char *f = fenced("json", "[\n{\"replace\":{\"old_string\":\"let sum = 0;\",\"new_string\":\"let sum = 1;\",\"replace_all\":True}},\n"
                                 "{\"replace\":{\"old_string\":\"return sum;\",\"new_string\":\"return sum + 1; // False None True\",\"replace_all\":None}},\n"
                                 "{\"replace\":{\"old_string\":\"console.log(sum);\",\"new_string\":\"console.info(sum);\",\"replace_all\":False}}\n]");
        json_t *turns[] = {text_msg(f), text_msg("DONE"), NULL};
        outcome_t o = run(dir, turns);
        char *w1 = replace_first(revised, "let sum = 0;", "let sum = 1;"), *w2 = replace_first(w1, "console.log", "console.info"),
             *w3 = replace_first(w2, "return sum + 1;", "return sum + 1; // False None True");
        CHECK_STR(o.after, w3);
        free(w1), free(w2), free(w3), free(f);
        outcome_free(&o);
    }
    {
        /* a call mentioned in prose does not run */
        json_t *turns[] = {text_msg("I can use replace(old_string=\"return sum;\", new_string=\"return sum + 999;\") for this."), tool_msg(upd),
                           text_msg("DONE"), NULL};
        outcome_t o = run(dir, turns);
        char *r = results(&o, 1);
        CHECK_STR(r, "");
        free(r);
        r = last_text(&o, 1);
        CHECK_HAS(r, "Apply the requested changes");
        free(r);
        CHECK_STR(o.after, revised);
        outcome_free(&o);
    }
    {
        /* native null options, boolean strings and integer strings follow the schemas */
        char *content = xasprintf("%s// return sum;\n", SOURCE);
        json_t *turns[] = {tool_msg("[[\"read\",{\"start_line\":\"7\",\"end_line\":\"7\"}],[\"replace\",{\"old_string\":\"console.log\",\"new_string\":"
                                    "\"console.info\",\"replace_all\":null}],[\"replace\",{\"old_string\":\"let sum = 0;\",\"new_string\":\"let sum = 1;\","
                                    "\"replace_all\":\"false\"}],[\"replace\",{\"old_string\":\"return sum;\",\"new_string\":\"return sum + 1;\","
                                    "\"replace_all\":\"true\"}]]"),
                           text_msg("DONE"), NULL};
        outcome_t o = run_file(dir, "sample.ts", turns, content, "described change");
        char *r = result_text(&o, 1, 0);
        CHECK_HAS(r, "Lines 7-7");
        free(r);
        char *w1 = replace_first(content, "console.log", "console.info"), *w2 = replace_first(w1, "let sum = 0;", "let sum = 1;"),
             *w3 = replace_all(w2, "return sum;", "return sum + 1;");
        CHECK_STR(o.after, w3);
        free(w1), free(w2), free(w3), free(content);
        outcome_free(&o);
    }
    {
        /* an identical failed replace reports its first error; an applied one is not repeated and DONE still works */
        json_t *turns[] = {tool_msg("[[\"replace\",{\"old_string\":\"return missing;\",\"new_string\":\"return sum + 1;\"}]]"),
                           tool_msg("[[\"replace\",{\"new_string\":\"return sum + 1;\",\"old_string\":\"return missing;\"}]]"), tool_msg(upd),
                           text_msg("DONE"), NULL};
        outcome_t o = run(dir, turns);
        char *first = result_text(&o, 2, 0), *retry = result_text(&o, 2, 1);
        CHECK_HAS(retry, "already failed");
        CHECK_HAS(retry, first);
        free(first), free(retry);
        CHECK_STR(o.after, revised);
        outcome_free(&o);
        const char *once = "[[\"replace\",{\"old_string\":\"return sum;\",\"new_string\":\"// once\\n\\treturn sum;\"}]]";
        json_t *turns2[] = {tool_msg(once), tool_msg(once), text_msg("DONE"), NULL};
        o = run(dir, turns2);
        CHECK(o.ok);
        retry = result_text(&o, 2, 1);
        CHECK_STR(retry, "This exact replace was already applied. Do not repeat it.");
        free(retry);
        char *w = replace_first(SOURCE, "return sum;", "// once\n\treturn sum;");
        CHECK_STR(o.after, w);
        free(w);
        outcome_free(&o);
    }
    {
        /* malformed written calls get the call-format feedback and never run */
        const char *bad[] = {"replace(\"return sum;\", \"return sum + 999;\")",
                             "replace(old_string=\"return sum;\", new_string=\"return \" + \"sum + 999;\")",
                             "[replace(old_string=\"return sum;\", new_string=\"return sum + 999;\"), write()]",
                             "```tool_code\n[replace(old_string=\"return sum;\" new_string=\"return sum + 999;\")]\n```"};
        for (size_t k = 0; k < 4; k++) {
            json_t *turns[] = {text_msg(bad[k]), tool_msg(upd), text_msg("DONE"), NULL};
            outcome_t o = run(dir, turns);
            char *r = results(&o, 1);
            CHECK_STR(r, "");
            free(r);
            r = last_text(&o, 1);
            CHECK_STR(r, "Use native tool calls, or write one JSON object per call in its own fenced block:\n\n```json\n"
                         "{\"name\":\"read\",\"arguments\":{\"start_line\":1,\"end_line\":5}}\n```");
            free(r);
            CHECK_STR(o.after, revised);
            outcome_free(&o);
        }
    }
    {
        /* an unknown tool fails the turn's replacements */
        json_t *turns[] = {tool_msg("[[\"write\",{\"path\":\"x\"}]]"), text_msg("DONE"), tool_msg(upd), text_msg("DONE"), NULL};
        outcome_t o = run(dir, turns);
        char *r = result_text(&o, 1, 0);
        CHECK_STR(r, "Available tools: replace, read, search.");
        free(r);
        r = last_text(&o, 2);
        CHECK_HAS(r, "At least one replacement failed.");
        free(r);
        CHECK_STR(o.after, revised);
        outcome_free(&o);
    }
    free(revised);
}

/* ---------- parse status (tree-sitter-json, as omp parses `.json`) ---------- */

static void test_parse_status(void) {
    CHECK(magic_parse_status("a.ts", "{") == -1);
    CHECK(magic_parse_status("dir.json/a.txt", "{") == -1);
    CHECK(magic_parse_status(".json", "{") == -1);
    CHECK(magic_parse_status("A.JSON", "{\"a\": 1}") == 1);
    const char *ok[] = {"{\"a\": 1} // c\n", "{\"a\": /* c */ 1}", "{\"a\": 1} {\"b\": 2}", "{\"a\": 1.}", "{\"a\": \"x\ty\"}",
                        "{\"a\": \"\\u\"}", "\n", "1", "{\"a\": 1E5}", "{\"a\": -0.5}", "{\"a\": \"\\/\"}", "{\"a\": \"x\ry\"}",
                        "{\"a\": 1.e5}", "truefalse", "1-2", "01", "{\"a\":\v1}", "{\"a\":\f1}", "{\"a\": 1e-5}", "{\"a\" /*x*/ : 1}",
                        "{\"\": 1}", "{\"a\": 1} //c", "{\"a\": 1} /** x **/", "{\"a\": [{\"b\": [[], {}]}, null, true, false]}",
                        "{\"a\": \"x\\\"y\"}", "{\"a\": 1e999}"};
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
        if (magic_parse_status("p.json", ok[i]) != 1) {
            fprintf(stderr, "FAIL %s:%d: should parse: %s\n", __FILE__, __LINE__, ok[i]);
            fails++;
        }
    const char *bad[] = {"{\"a\": 1,}", "{\"a\": [1,]}", "{\"a\": 1e+5}", "{\"a\": +1}", "{\"a\": 0x1F}", "{\"a\": .5}", "{\"a\": 01}",
                         "{\"a\": \"x\ny\"}", "{\"a\": \"\\x41\"}", "{'a': 1}", "{\"a\": NaN}", "{\"a\": 1} /* x", "{a: 1}", "{\"a\": -}",
                         "{\"a\":\xc2\xa0" "1}", "{\"a\": 1.5e}", "{\"a\": 1} # c", "{\"a\":\xc2\x85" "1}", "{\"a\":\xef\xbb\xbf" "1}",
                         "{\"a\": Infinity}", "{\"a\": 0b11}", "{\"a\":\xe2\x80\xa8" "1}", "{\"a\": -01}", "[1,,2]", "[,1]", "{\"a\": 1} /",
                         "{\"a\": 1}x", "{\"a\" 1}", "{\"a\": -.5}", "{\"a\": True}", "{", "[", "}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        if (magic_parse_status("p.json", bad[i]) != 0) {
            fprintf(stderr, "FAIL %s:%d: should not parse: %s\n", __FILE__, __LINE__, bad[i]);
            fails++;
        }
}

/* ---------- sections ---------- */

static void test_sections(void) {
    magic_sections_t s;
    char *err = NULL;
    CHECK(magic_parse_input("# a.ts\n- Rename x to y.\n\n# b.sh\n- Add after line 2:\n```sh\n# starts the job\nstart\n```\n# "
                            "a.ts\n- Then drop the import.\n",
                            &s, &err));
    CHECK(s.n == 2);
    if (s.n == 2) {
        CHECK_STR(s.v[0].path, "a.ts");
        CHECK_STR(s.v[0].change, "- Rename x to y.\n\n- Then drop the import.");
        CHECK_STR(s.v[1].path, "b.sh");
        CHECK_STR(s.v[1].change, "- Add after line 2:\n```sh\n# starts the job\nstart\n```");
    }
    magic_sections_free(&s);
    CHECK(!magic_parse_input("- Rename x to y.\n# a.ts\n- ok", &s, &err));
    CHECK_HAS(err, "# path");
    free(err);
    err = NULL;
    CHECK(!magic_parse_input("# a.ts\n# b.ts\n- Delete line 3.", &s, &err));
    CHECK_STR(err, "No change is described for a.ts.");
    free(err);
    err = NULL;
    CHECK(!magic_parse_input("", &s, &err));
    CHECK_STR(err, "Give one `# path` line followed by its changes per file.");
    free(err);
    strvec_t paths = {0};
    magic_edit_paths("# a.ts\n- x\n#  b.ts  \n- y\n# a.ts\n- z", &paths);
    CHECK(paths.n == 2 && !strcmp(paths.v[0], "a.ts") && !strcmp(paths.v[1], "b.ts"));
    strvec_free(&paths);
    /* the apply tools as omp sends them */
    json_t *tools = magic_apply_tools();
    char *d = json_dump(tools, 0);
    CHECK_HAS(d, "{\"name\":\"read\",\"description\":\"Read numbered lines from the current draft.\",\"parameters\":{\"type\":\"object\","
                 "\"properties\":{\"start_line\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"First line, 1-based.\"}");
    CHECK(json_len(tools) == 3);
    free(d);
    json_free(tools);
}

int main(void) {
    /* throwaway HOME / agent dir first: prompts may be overridden from the agent dir */
    char tmpl[] = "/tmp/omc-test-magic.XXXXXX";
    const char *base = getenv("TMPDIR");
    char *dir_t = xasprintf("%s/omc-test-magic.XXXXXX", base && *base ? base : "/tmp");
    char *dir = mkdtemp(dir_t);
    if (!dir) dir = mkdtemp(tmpl);
    if (!dir) return 1;
    char *agent = xasprintf("%s/agent", dir);
    setenv("HOME", dir, 1);
    setenv("OMC_CODING_AGENT_DIR", agent, 1);
    test_file_views();
    test_tools();
    test_written_calls();
    test_apply(dir);
    test_intake(dir);
    test_parse_status();
    test_sections();
    char *rm = xasprintf("rm -rf '%s'", dir);
    if (system(rm) != 0) fprintf(stderr, "cleanup failed\n");
    free(rm);
    free(agent);
    free(dir_t);
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    else printf("test_magic: ok\n");
    return fails ? 1 : 0;
}
