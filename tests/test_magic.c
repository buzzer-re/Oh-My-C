/* test_magic.c - `edit.mode: magic` (tools/magic.c): file views, line-operation compilation, the described-change
 * loop against the real replace engine, and payload sections. Cases from omp's edit-magic.test.ts (v19.1.0); the one
 * that relies on omp's native parse check is adapted (omc questions the range through its block hint instead). */
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

static const char *SOURCE = "function total(items: number[]) {\n"
                            "\tlet sum = 0;\n"
                            "\tfor (const item of items) {\n"
                            "\t\tsum += item;\n"
                            "\t}\n"
                            "\tconsole.log(sum);\n"
                            "\treturn sum;\n"
                            "}\n";

static const char *THREE = "function a() {\n  return 1;\n}\n\nfunction b() {\n  return 2;\n}\n\nfunction c() {\n  return 3;\n}\n";

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

static char *fn(const char *name, int value) { return xasprintf("function %s() {\n  return %d;\n}\n", name, value); }

/* markers ("⋮ …" lines) of a view, joined by "|" */
static char *markers(const char *text) {
    strvec_t lines = str_split(text, "\n"), out = {0};
    for (size_t i = 0; i < lines.n; i++)
        if (str_starts(lines.v[i], "\xe2\x8b\xae")) strvec_push(&out, lines.v[i]);
    char *r = strvec_join(&out, "|");
    strvec_free(&lines);
    strvec_free(&out);
    return r;
}

/* content after the operations in reply, or "problem: …" */
static char *after_ex(const char *content, const char *reply, const char *change, bool cut) {
    magic_views_t views;
    magic_file_views(content, change ? change : "described change", -1, &views);
    magic_compiled_t c;
    magic_compile(reply, content, &views.v[0], cut, &c);
    char *r;
    if (c.problem) {
        r = xasprintf("problem: %s", c.problem);
    } else {
        const char *m = strstr(content, c.old_s);
        CHECK(m && !strstr(m + 1, c.old_s));
        r = replace_first(content, c.old_s, c.new_s);
    }
    magic_compiled_free(&c);
    magic_views_free(&views);
    return r;
}
static char *after(const char *content, const char *reply) { return after_ex(content, reply, NULL, false); }

static void test_file_views(void) {
    char *lng = long_source(50);
    magic_views_t v;
    magic_file_views(lng, "In `step30()`, replace `return doubled - 30;` with `return doubled + 30;`.", -1, &v);
    CHECK(v.n == 2 && v.v[0].partial && !v.v[1].partial);
    char *m = markers(v.v[0].text);
    CHECK_STR(m, "\xe2\x8b\xae lines 13-228 not shown|\xe2\x8b\xae lines 260-388 not shown");
    free(m);
    CHECK_HAS(v.v[0].text, "241-247\texport function step30(input: number): number {\n242\t\tconst doubled = input * 2;");
    CHECK(str_starts(v.v[0].text, "1-7\texport function step0("));
    CHECK(str_ends(v.v[0].text, "400\t"));
    CHECK_HAS(v.v[1].text, "\n100\t\t\treturn doubled - 12;\n");
    magic_views_free(&v);

    /* a repeating line placed by the other lines the change names */
    magic_file_views(lng, "In `step30()`, change `const doubled = input * 2;` to `const doubled = input * 3;`.", -1, &v);
    CHECK_HAS(v.v[0].text, "\n242\t\tconst doubled = input * 2;\n");
    CHECK(!strstr(v.v[0].text, "\n202\t") && !strstr(v.v[0].text, "\n322\t"));
    m = markers(v.v[0].text);
    CHECK(str_count(m, "|") == 1);
    free(m);
    magic_views_free(&v);
    magic_file_views(lng, "Delete lines 120-121 and line 300.", -1, &v);
    m = markers(v.v[0].text);
    CHECK_STR(m, "\xe2\x8b\xae lines 13-107 not shown|\xe2\x8b\xae lines 134-287 not shown|\xe2\x8b\xae lines 313-388 not shown");
    free(m);
    magic_views_free(&v);

    /* whole when short, when nothing is named, or when most of it is named */
    magic_file_views(SOURCE, "In `total`, delete `console.log(sum);`.", -1, &v);
    CHECK(v.n == 1 && !v.v[0].partial);
    magic_views_free(&v);
    magic_file_views(lng, "Tidy up the error handling.", -1, &v);
    CHECK(v.n == 1 && !v.v[0].partial);
    magic_views_free(&v);
    sb_t many = SB_INIT;
    sb_append(&many, "Add a comment above each of ");
    for (int n = 0; n < 25; n++) sb_appendf(&many, "%s`step%d(`", n ? ", " : "", 2 * n);
    sb_append(&many, ".");
    magic_file_views(lng, many.s, -1, &v);
    CHECK(v.n == 1 && !v.v[0].partial);
    magic_views_free(&v);
    sb_free(&many);

    /* views over the limit are dropped */
    magic_file_views(lng, "In `step30()`, return `doubled + 30`.", 4000, &v);
    CHECK(v.n == 1 && v.v[0].partial);
    magic_views_free(&v);
    magic_file_views(lng, "Tidy up the error handling.", 4000, &v);
    CHECK(v.n == 0);
    magic_views_free(&v);
    free(lng);
}

static void test_compile(void) {
    char *r, *want, *a, *b, *c;
    /* deletes, inserts, replaces and substitutes by original line number in one reply */
    r = after(SOURCE, "[\"delete\",6,6]\n[\"insert\",\"after\",2,\"\\tlet count = 0;\"]\n[\"replace\",7,7,\"\\treturn sum / "
                      "count;\"]\n[\"sub\",4,\"+=\",\"-=\"]\n[\"sub\",\"all\",\"items\",\"values\"]");
    CHECK_STR(r, "function total(values: number[]) {\n\tlet sum = 0;\n\tlet count = 0;\n\tfor (const item of values) {\n"
                 "\t\tsum -= item;\n\t}\n\treturn sum / count;\n}\n");
    free(r);

    /* a sub's line number is a hint when one line holds the text */
    r = after(SOURCE, "[\"sub\",3,\"sum = 0\",\"sum = 1\"]");
    want = replace_first(SOURCE, "sum = 0", "sum = 1");
    CHECK_STR(r, want);
    free(r);
    free(want);
    r = after(SOURCE, "[\"sub\",3,\"sum\",\"total\"]");
    CHECK_HAS(r, "the nearest line that does is 2 or 4");
    free(r);
    r = after(SOURCE, "[\"sub\",\"all\",\"nothing\",\"x\"]");
    CHECK_HAS(r, "does not contain");
    free(r);

    /* near-miss forms */
    r = after(SOURCE, "[\"remove\",6]");
    want = replace_first(SOURCE, "\tconsole.log(sum);\n", "");
    CHECK_STR(r, want);
    free(r);
    free(want);
    a = replace_first(THREE, "return 1;", "return 0;");
    b = replace_first(a, "return 2;", "return 1;");
    char *swapped = replace_first(b, "return 0;", "return 2;");
    free(a);
    free(b);
    r = after(THREE, "[\"swap\",2,6]");
    CHECK_STR(r, swapped);
    free(r);
    r = after(THREE, "[\"swap\",2,6,6,2]");
    CHECK_STR(r, swapped);
    free(r);
    free(swapped);
    want = replace_first(THREE, "  return 1;", "return 1;");
    r = after(THREE, "[\"dedent\",2,2]");
    CHECK_STR(r, want);
    free(r);
    r = after(THREE, "[\"indent\",2,2,-2]");
    CHECK_STR(r, want);
    free(r);
    free(want);
    want = replace_first(THREE, "  return 1;", "    return 1;");
    r = after(THREE, "[\"indent\",2,2]");
    CHECK_STR(r, want);
    free(r);
    free(want);

    /* inserts at one place merge in order, never into the place a move lands */
    want = replace_first(THREE, "  return 1;\n", "  return 1;\n  // one\n  // two\n");
    r = after(THREE, "[\"insert\",\"after\",2,\"  // one\"]\n[\"insert\",\"before\",3,\"  // two\"]");
    CHECK_STR(r, want);
    free(r);
    free(want);
    r = after(THREE, "[\"move\",9,11,\"before\",1]\n[\"insert\",\"before\",1,\"// top\"]");
    CHECK_HAS(r, "insert at the same place");
    free(r);

    /* blank lines between blocks are separators */
    a = fn("a", 1), b = fn("b", 2), c = fn("c", 3);
    want = xasprintf("%s\n%s\n%s", b, a, c);
    r = after(THREE, "[\"swap\",1,3,5,7]");
    CHECK_STR(r, want);
    free(r);
    r = after(THREE, "[\"swap\",1,4,5,8]");
    CHECK_STR(r, want);
    free(r);
    free(want);
    want = xasprintf("%s\n%s\n%s", c, a, b);
    r = after(THREE, "[\"move\",9,11,\"before\",1]");
    CHECK_STR(r, want);
    free(r);
    r = after(THREE, "[\"move\",8,11,\"before\",1]");
    CHECK_STR(r, want);
    free(r);
    free(want);
    want = xasprintf("%s\n%s", a, c);
    r = after(THREE, "[\"delete\",5,7]");
    CHECK_STR(r, want);
    free(r);
    free(want);
    r = after(THREE, "[\"move\",5,7,\"after\",4]");
    CHECK_HAS(r, "nothing moves");
    free(r);
    free(a);
    free(b);
    free(c);

    /* unwrap: delete the wrapper lines, dedent what stays */
    const char *wrapped = "start();\nif (ready) {\n  first();\n\n  second();\n}\nend();\n";
    r = after(wrapped, "[\"delete\",2,2]\n[\"dedent\",3,5,2]\n[\"delete\",6,6]");
    CHECK_STR(r, "start();\nfirst();\n\nsecond();\nend();\n");
    free(r);
    r = after(wrapped, "[\"dedent\",1,3,2]");
    CHECK_HAS(r, "problem: Line 1 does not have the specified indentation");
    free(r);

    /* the ways small models wrap operations */
    char *deleted = replace_first(SOURCE, "\tconsole.log(sum);\n", "");
    r = after(SOURCE, "```json\n[\"delete\",\"6\",\"6\"]\n```\nExplanation: line 6 is the log call.");
    CHECK_STR(r, deleted);
    free(r);
    r = after(SOURCE, "[[\"delete\",6,6]]");
    CHECK_STR(r, deleted);
    free(r);
    r = after(SOURCE, "{\"delete\", 6, 6}");
    CHECK_STR(r, deleted);
    free(r);
    r = after(SOURCE, "[{\"delete\", 6, 6}]");
    CHECK_STR(r, deleted);
    free(r);
    r = after(SOURCE, "[\"delete\",6,6]\nWait, that is the wrong line.\n[\"delete\",7,7]");
    CHECK_HAS(r, "problem:");
    free(r);
    r = after(SOURCE, "I deleted line 6 as requested.");
    CHECK_HAS(r, "problem:");
    free(r);

    /* a reply cut at the output limit counts only when its operations were finished */
    r = after_ex(SOURCE, "[\"delete\",6,6]\nExplanation: the log call is on", NULL, true);
    CHECK_STR(r, deleted);
    free(r);
    r = after_ex(SOURCE, "[\"delete\",6,6]\n[\"sub\",2,\"0\",", NULL, true);
    CHECK_HAS(r, "problem:");
    free(r);
    free(deleted);

    /* operations it cannot place are rejected */
    r = after(SOURCE, "[\"delete\",3,5]\n[\"replace\",4,4,\"x\"]");
    CHECK_HAS(r, "same lines");
    free(r);
    r = after(SOURCE, "[\"delete\",8,12]");
    CHECK_HAS(r, "expected [\"delete\", first, last]");
    free(r);
    r = after(SOURCE, "[\"replace\",2,2,\"\\tlet sum = 0;\"]");
    CHECK_HAS(r, "no changes");
    free(r);
    r = after(SOURCE, "[\"erase\",6,6]");
    CHECK_STR(r, "problem: Operation 1 ([\"erase\",6,6]): unknown operation; use one of delete, replace, sub, insert, move, "
                 "swap, indent, dedent.");
    free(r);
    char *lng = long_source(50);
    const char *change = "In `step30()`, replace `return doubled - 30;` with `return doubled + 30;`.";
    r = after_ex(lng, "[\"delete\",100,100]", change, false);
    CHECK_HAS(r, "not shown");
    free(r);
    r = after_ex(lng, "[\"sub\",244,\"- 30\",\"+ 30\"]", change, false);
    want = replace_first(lng, "doubled - 30;", "doubled + 30;");
    CHECK_STR(r, want);
    free(r);
    free(want);
    free(lng);

    /* JS replaceAll substitution patterns in a sub's replacement */
    r = after("echo pid\n", "[\"sub\",1,\"pid\",\"$$ $&\"]");
    CHECK_STR(r, "echo $ pid\n");
    free(r);
}

/* ---------- applyDescribedChange against a file through the real replace engine ---------- */

typedef struct {
    const char *const *replies;
    size_t next;
    strvec_t requests;
    const char *file;
    const char *dir;
    char *raw;
} script_t;

static char *s_complete(magic_io_t *io, const char *request, int64_t max_tokens, bool *truncated, char **err) {
    script_t *s = io->ud;
    UNUSED(max_tokens);
    strvec_push(&s->requests, request);
    *truncated = false;
    if (!s->replies[s->next]) {
        *err = xstrdup("the model was asked for more replies than scripted");
        return NULL;
    }
    return xstrdup(s->replies[s->next++]);
}

static void s_apply(magic_io_t *io, const char *old_s, const char *new_s, bool check, pe_result_t *r) {
    script_t *s = io->ud;
    json_t *args = json_obj(), *edits = json_arr(), *e = json_obj();
    json_set_str(args, "path", "sample.ts");
    json_set_str(e, "old_string", old_s);
    json_set_str(e, "new_string", new_s);
    json_push(edits, e);
    json_set(args, "edits", edits);
    pe_opts_t o = {.cwd = s->dir, .allow_fuzzy = true, .fuzzy_threshold = 0.95};
    if (check) {
        o.raw_snapshot = s->raw;
        o.raw_snapshot_len = strlen(s->raw);
    }
    pe_run(PE_REPLACE, args, &o, r);
    json_free(args);
}

typedef struct {
    bool ok;
    char *err;
    char *after;
    strvec_t requests;
} outcome_t;

static outcome_t run_ex(const char *dir, const char *const *replies, const char *content, const char *change) {
    char *file = xasprintf("%s/sample.ts", dir);
    fs_write(file, content, strlen(content));
    script_t s = {replies, 0, {0}, file, dir, xstrdup(content)};
    magic_views_t views;
    magic_file_views(content, change, -1, &views);
    magic_io_t io = {s_complete, s_apply, &s};
    pe_result_t res;
    outcome_t o = {0};
    o.ok = magic_apply_described("sample.ts", change, content, &views, &io, &res, &o.err);
    if (o.ok) pe_result_free(&res);
    size_t len;
    o.after = fs_read(file, &len);
    o.requests = s.requests;
    magic_views_free(&views);
    free(s.raw);
    free(file);
    return o;
}
static outcome_t run(const char *dir, const char *const *replies, const char *content) {
    return run_ex(dir, replies, content, "described change");
}
static void outcome_free(outcome_t *o) {
    free(o->err);
    free(o->after);
    strvec_free(&o->requests);
}

static void test_apply(const char *dir) {
    outcome_t o;
    {
        const char *rep[] = {"[\"delete\",2,2]", NULL};
        o = run(dir, rep, "first\nlast");
        CHECK(o.ok);
        CHECK_STR(o.after, "first");
        outcome_free(&o);
    }
    {
        const char *rep[] = {"[\"sub\",3,\"sum\",\"total\"]", "[\"sub\",2,\"sum = 0\",\"sum = 1\"]", NULL};
        o = run(dir, rep, SOURCE);
        CHECK(o.ok && o.requests.n == 2);
        CHECK(!strstr(o.requests.v[0], "<rejection>"));
        CHECK_HAS(o.requests.v[1], "[\"sub\",3,\"sum\",\"total\"]");
        CHECK_HAS(o.requests.v[1], "<rejection>\nOperation 1 ([\"sub\",3,\"sum\",\"total\"]): line 3 does not contain");
        char *want = replace_first(SOURCE, "let sum = 0;", "let sum = 1;");
        CHECK_STR(o.after, want);
        free(want);
        outcome_free(&o);
    }
    {
        const char *f = "[\"sub\",2,\"0\",\"1\"]\n[\"sub\",7,\"no such text\",\"x\"]";
        const char *rep[] = {f, f, f, NULL};
        o = run(dir, rep, SOURCE);
        CHECK(!o.ok && o.requests.n == 3);
        CHECK_HAS(o.err, "the file is unchanged");
        CHECK_STR(o.after, SOURCE);
        outcome_free(&o);
    }
    {
        /* omp: the native parse check rejects 3-4 (it leaves the loop's brace behind); omc's block hint does */
        const char *rep[] = {"[\"delete\",3,4]", "[\"delete\",3,5]", NULL};
        o = run(dir, rep, SOURCE);
        CHECK(o.ok && o.requests.n == 2);
        CHECK_HAS(o.requests.v[1], "<rejection>\nNothing was applied: line 3 opens a block that ends on line 5");
        char *want = replace_first(SOURCE, "\tfor (const item of items) {\n\t\tsum += item;\n\t}\n", "");
        CHECK_STR(o.after, want);
        free(want);
        outcome_free(&o);
    }
    {
        const char *rep[] = {"[\"delete\",3,6]", "[\"delete\",3,6]", NULL};
        o = run(dir, rep, SOURCE);
        CHECK(o.ok && o.requests.n == 2);
        CHECK_HAS(o.requests.v[1], "<rejection>\nNothing was applied: line 3 opens a block that ends on line 5, but the range "
                                   "ends on line 6 (`console.log(sum);`). Correct the range, or send the same operations "
                                   "again if that is intended.");
        CHECK_STR(o.after, "function total(items: number[]) {\n\tlet sum = 0;\n\treturn sum;\n}\n");
        outcome_free(&o);
    }
    {
        const char *wrapped = "start();\nif (ready) {\n  first();\n\n  second();\n}\nend();\n";
        const char *rep[] = {"[\"delete\",2,2]\n[\"dedent\",3,5,2]\n[\"delete\",6,6]", NULL};
        o = run(dir, rep, wrapped);
        CHECK(o.ok && o.requests.n == 1);
        CHECK_STR(o.after, "start();\nfirst();\n\nsecond();\nend();\n");
        outcome_free(&o);
        const char *spaced = "function f() {\n  a();\n\n  if (x) {\n    b();\n  }\n}\n";
        const char *rep2[] = {"[\"delete\",3,3]", NULL};
        o = run(dir, rep2, spaced);
        CHECK(o.ok && o.requests.n == 1);
        CHECK_STR(o.after, "function f() {\n  a();\n  if (x) {\n    b();\n  }\n}\n");
        outcome_free(&o);
    }
    {
        const char *rep[] = {"CANNOT: `drain` is not in this file", NULL};
        o = run(dir, rep, SOURCE);
        CHECK(!o.ok && o.requests.n == 1);
        CHECK_STR(o.err, "No change was made to sample.ts: `drain` is not in this file");
        CHECK_STR(o.after, SOURCE);
        outcome_free(&o);
    }
    char *lng = long_source(50);
    {
        const char *rep[] = {"[\"sub\",244,\"tripled\",\"doubled\"]", "[\"sub\",244,\"- 30\",\"+ 30\"]", NULL};
        o = run_ex(dir, rep, lng, "In `step30()`, replace `return doubled - 30;` with `return doubled + 30;`.");
        CHECK(o.ok && o.requests.n == 2);
        CHECK_HAS(o.requests.v[0], "\xe2\x8b\xae lines 13-228 not shown");
        CHECK(!strstr(o.requests.v[0], "\n100\t"));
        CHECK(!strstr(o.requests.v[1], "\xe2\x8b\xae"));
        CHECK_HAS(o.requests.v[1], "\n100\t");
        CHECK_HAS(o.requests.v[1], "<rejection>");
        CHECK_HAS(o.after, "\t\treturn doubled + 30;\n");
        outcome_free(&o);
    }
    {
        const char *rep[] = {"CANNOT: the target is not shown", "CANNOT: `step30` returns nothing like that", NULL};
        o = run_ex(dir, rep, lng, "In `step30()`, replace `return tripled;` with `return doubled;`.");
        CHECK(!o.ok && o.requests.n == 2);
        CHECK_HAS(o.requests.v[1], "\n100\t");
        CHECK(!strstr(o.requests.v[1], "<reply>"));
        CHECK_HAS(o.err, "`step30` returns nothing like that");
        CHECK(!strstr(o.err, "line numbers"));
        outcome_free(&o);
    }
    free(lng);
    {
        const char *rep[] = {"[\"sub\",7,\"sum;\",\"sum + 0;\"]", NULL};
        o = run(dir, rep, SOURCE);
        CHECK(o.requests.n == 1);
        CHECK_HAS(o.requests.v[0], "2\t\tlet sum = 0;\n3-5\t\tfor (const item of items) {\n4\t\t\tsum += item;");
        CHECK_HAS(o.requests.v[0], "File `sample.ts`. Each line is shown as its number, a tab, then its text;");
        CHECK(!strstr(o.requests.v[0], "marks omitted lines"));
        outcome_free(&o);
    }
}

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
    test_compile();
    test_apply(dir);
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
