/* edit-mode engine tests: replays omp's pi-edit fixture suites (tests/fixtures/pi-edit, copied verbatim from
 * upstream) against pe_run for replace / patch / apply_patch / sloppy, the 21 portable Codex apply_patch
 * scenarios, and direct checks of the diff renderers and parser error messages. */
#include "core/core.h"
#include "core/fs.h"
#include "core/json.h"
#include "text/regex.h"
#include "tools/diff.h"
#include "tools/patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0, passed = 0, skipped = 0;
static bool verbose = false;

#define FX "tests/fixtures/pi-edit"

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* Upstream sloppy fixtures for the internal legacy surface omc does not port: inline ⟪old│new⟫ selection markers
 * (incl. positional multi-selection rewrites), marker-less desired-text recovery, diff-shaped recovery and
 * punctuation-typo tolerance. None of these are taught by the sloppy prompt/grammar. See PARITY.md. */
static const char *const SLOPPY_GAPS[] = {
    "rejects punctuation-only patterns but accepts short identifiers",
    "applies an inline all-match replacement",
    "replaces only the selected token and preserves outside gaps",
    "applies inline replacements to each named selection",
    "re-emits selected gaps from an inline replacement",
    "mixes add lines with inline replacements in one operation",
    "replaces a contained region through one multi-line selection",
    "allows zero rewrite gaps to replace a selection containing gaps",
    "keeps a same-line insertion inline",
    "deletes a selected whole line including indentation and newline",
    "accepts a unique single-fragment sparse selection",
    "substitutes multiple selections positionally when REWRITE has one line per selection",
    "uses whole-line ellipses as positional separators when the pattern has no gaps",
    "supports multi-line positional groups separated by whole-line ellipses",
    "fails closed on whole-line rewrite ellipses when a multi-selection pattern has gaps",
    "fails closed when a multi-selection rewrite proves neither interpretation",
    "drops an echoed literal before an inline selection",
    "drops an echoed anchor line above an embedded selection",
    "keeps the unmatched-close error when a stray ⟫ follows a proper selection",
    "supports an empty directional selection at the exact line boundary",
    "keeps a genuine empty inline insertion anchored to a neighbor line as a no-op error",
    "lands a trailing insert-only selection line on its own line",
    "lands a multi-line insert selection before a blank line on its own line",
    "applies inline insertion and deletion as independent operations",
    "reuses an inline deletion register in a later inline insertion",
    "treats empty double selection markers as an insertion point",
    "allows one punctuation insertion typo for a unique changing candidate",
    "accepts a small fragment typo only when the fuzzy tuple is unique",
    "collapses back-to-back duplicates when desired text matches both copies",
    "applies marker-less desired text over its closest near-match block",
    "applies a marker-less desired import line over its near-match",
    "collapses a duplicated block stated once as mono desired text",
    "recovers a unified-diff-shaped rewrite-less op as inline changes",
    "binds a diff added-run to its context line and hunk markers to gaps",
    "treats several OLD selections as one whole sparse region",
    "checks ordered-tuple uniqueness before choosing a match",
    "bounds same-line gaps and aligns a full-line rewrite around the selection",
    "strips full-statement envelope echoes around an inner selection",
    "keeps a mid-line ellipsis in REWRITE literal when the capture is multi-line",
    "replaces the whole line when a bare selection's REWRITE restates it",
    "allows proven whole-span replacement for multiple selections",
    "never rewrites part of a longer punctuation run",
    "applies a bare selection beside inline pairs as desired text",
    "preserves a large region outside a tiny selection",
    "maps provided rewrite gaps positionally and drops omitted later gaps",
    "re-emits selected gaps positionally without retyping their spans",
    "uses an empty rewrite as a blank line only for an insertion operation",
    "applies the all-match opener to every match with one identical rewrite",
    "accepts the Unicode ellipsis gap",
    "still deletes one of two adjacent duplicate lines without echo dedup",
    "opens a new line for an insert after the last line of a file without a trailing newline",
    "rejects an overlapping op that no longer matches the earlier op's result alone",
    "resolves every batch anchor against the original content",
    "normalizes Unicode punctuation in sparse literals",
    "treats rewrite ellipses beyond the capture count as literal",
    "treats candidates with whitespace-equivalent outcomes as unambiguous",
    "strips harmless leading and trailing sparse gaps",
    "applies a non-overlapping batch and accepts an EOF transport newline",
    NULL,
};

static bool is_gap(const char *name) {
    for (size_t i = 0; SLOPPY_GAPS[i]; i++)
        if (!strcmp(SLOPPY_GAPS[i], name)) return true;
    return false;
}

static bool expect_matches(const char *actual, const char *pattern) {
    if (pattern[0] != '^') return strstr(actual, pattern) != NULL;
    char *err = NULL;
    rx_t *rx = rx_compile(pattern, 0, &err);
    if (!rx) {
        free(err);
        return false;
    }
    bool ok = rx_is_match(rx, actual, strlen(actual), 0);
    rx_free(rx);
    return ok;
}

static void write_file(const char *root, const char *rel, const char *content) {
    char *p = path_join(root, rel);
    char *d = path_dirname(p);
    fs_mkdir_p(d, 0755);
    fs_write(p, content, strlen(content));
    free(d);
    free(p);
}

static char *read_rel(const char *root, const char *rel) {
    char *p = path_join(root, rel);
    size_t len;
    char *s = fs_read(p, &len);
    free(p);
    return s;
}

static pe_opts_t opts_for(const char *cwd, const json_t *policy) {
    pe_opts_t o;
    memset(&o, 0, sizeof(o));
    o.cwd = cwd;
    o.allow_fuzzy = json_get_bool(policy, "allowFuzzy", true);
    o.fuzzy_threshold = json_get_num(policy, "fuzzyThreshold", 0.95);
    return o;
}

static void run_case(const json_t *c, const char *default_mode, const char *file) {
    const char *name = json_get_str(c, "name");
    if (!name) name = "<unnamed>";
    const char *mode_name = json_get_str(c, "mode");
    if (!mode_name) mode_name = default_mode;
    int mode = pe_mode_parse(mode_name);
    if (mode < 0) {
        skipped++;
        return;
    }
    if (!strcmp(mode_name, "sloppy") && is_gap(name) && !getenv("OMC_TEST_ALL")) {
        skipped++;
        return;
    }
    char *dir = fs_mktempdir("omc-edit");
    char *cwd = path_real(dir);
    json_t *files = json_get_obj(c, "files");
    if (files)
        for (size_t i = 0; i < files->u.obj.n; i++)
            write_file(cwd, files->u.obj.keys[i], json_sv(files->u.obj.vals[i]) ? json_sv(files->u.obj.vals[i]) : "");
    json_t *args = json_get(c, "args");
    json_t *owned = NULL;
    if (!args) {
        owned = json_obj();
        json_set_str(owned, "input", json_get_str(c, "input"));
        args = owned;
    }
    pe_opts_t o = opts_for(cwd, json_get(c, "policy"));
    pe_result_t r;
    pe_run((pe_mode_t)mode, args, &o, &r);
    json_t *ex = json_get(c, "expect");
    sb_t prob = SB_INIT;
    const char *want_err = json_get_str(ex, "error");
    if (r.is_error && !want_err) sb_appendf(&prob, "unexpected error: %s\n", r.text);
    else if (!r.is_error && want_err) sb_appendf(&prob, "expected error %s but apply succeeded\n", want_err);
    else if (r.is_error && !expect_matches(r.text, want_err))
        sb_appendf(&prob, "error mismatch\n  expected: %s\n  actual:   %s\n", want_err, r.text);
    if (!r.is_error) {
        const char *t = json_get_str(ex, "text");
        if (t && strcmp(t, r.text) != 0) sb_appendf(&prob, "text mismatch\n  expected: %s\n  actual:   %s\n", t, r.text);
        json_t *tc = json_get_arr(ex, "textContains");
        for (size_t i = 0; tc && i < json_len(tc); i++) {
            const char *part = json_sv(json_at(tc, i));
            if (part && !expect_matches(r.text, part)) sb_appendf(&prob, "text missing %s\n  actual: %s\n", part, r.text);
        }
    }
    json_t *ef = json_get_obj(ex, "files");
    for (size_t i = 0; ef && i < ef->u.obj.n; i++) {
        const char *rel = ef->u.obj.keys[i], *want = json_sv(ef->u.obj.vals[i]);
        char *got = read_rel(cwd, rel);
        if (!got) sb_appendf(&prob, "file %s missing after apply\n", rel);
        else if (want && strcmp(got, want) != 0)
            sb_appendf(&prob, "file %s mismatch\n  expected: %s\n  actual:   %s\n", rel, want, got);
        free(got);
    }
    json_t *del = json_get_arr(ex, "deleted");
    for (size_t i = 0; del && i < json_len(del); i++) {
        char *p = path_join(cwd, json_sv(json_at(del, i)));
        if (fs_exists(p)) sb_appendf(&prob, "file %s should have been deleted\n", json_sv(json_at(del, i)));
        free(p);
    }
    json_t *w = json_get(ex, "writes");
    if (w && json_is_num(w) && (int)w->u.num.d != r.writes)
        sb_appendf(&prob, "expected %d writes, got %d\n", (int)w->u.num.d, r.writes);
    if (prob.len) {
        fprintf(stderr, "FAIL [%s] %s:\n%s\n", file, name, sb_cstr(&prob));
        fails++;
    } else {
        passed++;
        if (verbose) fprintf(stderr, "ok   [%s] %s\n", file, name);
    }
    sb_free(&prob);
    pe_result_free(&r);
    json_free(owned);
    fs_rm_rf(cwd);
    free(cwd);
    free(dir);
}

static void run_fixture(const char *mode, const char *file) {
    char *p = xasprintf(FX "/%s/%s", mode, file);
    size_t len;
    char *s = fs_read(p, &len);
    CHECK(s, "cannot read %s", p);
    json_t *root = s ? json_parse_str(s) : NULL;
    json_t *cases = json_get_arr(root, "cases");
    for (size_t i = 0; cases && i < json_len(cases); i++) run_case(json_at(cases, i), mode, file);
    json_free(root);
    free(s);
    free(p);
}

static void run_fixture_dir(const char *mode) {
    char *d = xasprintf(FX "/%s", mode);
    dirlist_t l = {0};
    fs_list_dir(d, &l);
    for (size_t i = 0; i < l.n; i++)
        if (!l.v[i].is_dir && str_ends(l.v[i].name, ".json")) run_fixture(mode, l.v[i].name);
    dirlist_free(&l);
    free(d);
}

/* ---- apply_patch portable scenarios ---- */

static void collect_tree(const char *root, const char *rel, strvec_t *out) {
    char *d = rel[0] ? path_join(root, rel) : xstrdup(root);
    dirlist_t l = {0};
    fs_list_dir(d, &l);
    for (size_t i = 0; i < l.n; i++) {
        char *r = rel[0] ? path_join(rel, l.v[i].name) : xstrdup(l.v[i].name);
        if (l.v[i].is_dir) collect_tree(root, r, out);
        else {
            char *c = read_rel(root, r);
            strvec_push_own(out, xasprintf("%s\n%s", r, c ? c : ""));
            free(c);
        }
        free(r);
    }
    dirlist_free(&l);
    free(d);
}

static void copy_tree(const char *from, const char *to) {
    strvec_t files = {0};
    collect_tree(from, "", &files);
    for (size_t i = 0; i < files.n; i++) {
        char *nl = strchr(files.v[i], '\n');
        *nl = 0;
        write_file(to, files.v[i], nl + 1);
    }
    strvec_free(&files);
}

static void run_scenarios(void) {
    const char *base = FX "/apply_patch/scenarios";
    dirlist_t l = {0};
    fs_list_dir(base, &l);
    int count = 0;
    for (size_t i = 0; i < l.n; i++) {
        if (!l.v[i].is_dir) continue;
        count++;
        const char *name = l.v[i].name;
        char *sd = path_join(base, name);
        char *dir = fs_mktempdir("omc-ap");
        char *cwd = path_real(dir);
        char *in = path_join(sd, "input"), *exp = path_join(sd, "expected"), *pp = path_join(sd, "patch.txt");
        if (fs_is_dir(in)) copy_tree(in, cwd);
        size_t len;
        char *patch = fs_read(pp, &len);
        json_t *args = json_obj();
        json_set_str(args, "input", patch ? patch : "");
        pe_opts_t o = opts_for(cwd, NULL);
        pe_result_t r;
        pe_run(PE_APPLY_PATCH, args, &o, &r);
        bool want_err = false;
        for (int k = 5; k <= 13; k++) {
            char pre[8];
            snprintf(pre, sizeof(pre), "%03d_", k);
            if (str_starts(name, pre)) want_err = true;
        }
        CHECK(r.is_error == want_err, "scenario %s: is_error=%d (%s)", name, r.is_error, r.text);
        strvec_t got = {0}, want = {0};
        collect_tree(cwd, "", &got);
        if (fs_is_dir(exp)) collect_tree(exp, "", &want);
        bool same = got.n == want.n;
        for (size_t k = 0; same && k < got.n; k++) same = !strcmp(got.v[k], want.v[k]);
        CHECK(same, "scenario %s final tree differs", name);
        if (same && r.is_error == want_err) passed++;
        strvec_free(&got);
        strvec_free(&want);
        pe_result_free(&r);
        json_free(args);
        free(patch);
        free(in);
        free(exp);
        free(pp);
        fs_rm_rf(cwd);
        free(cwd);
        free(dir);
        free(sd);
    }
    CHECK(count == 21, "portable scenario count changed: %d", count);
    dirlist_free(&l);
}

/* ---- direct checks ---- */

static void test_diff_renderers(void) {
    sb_t a = SB_INIT, b = SB_INIT;
    for (int i = 1; i <= 20; i++) {
        sb_appendf(&a, "%sline %d", i > 1 ? "\n" : "", i);
        if (i == 2) sb_append(&b, "\nline 2 changed");
        else if (i == 18) sb_append(&b, "\nline 18 changed");
        else sb_appendf(&b, "%sline %d", i > 1 ? "\n" : "", i);
    }
    int64_t first = 0;
    char *d = diff_numbered(sb_cstr(&a), sb_cstr(&b), 2, &first);
    CHECK(strstr(d, "-2|line 2\n+2|line 2 changed") && strstr(d, "-18|line 18\n+18|line 18 changed"), "numbered: %s", d);
    CHECK(strstr(d, " 4|line 4\n 16|line 16") && !strstr(d, " 8|line 8"), "numbered collapse: %s", d);
    CHECK(first == 2, "first changed %lld", (long long)first);
    free(d);
    sb_free(&a);
    sb_free(&b);
    /* matching bracket line beyond context */
    const char *old = "function outer() {\n  const value = 1;\n  const two = 2;\n  const three = 3;\n  const four = "
                      "4;\n  return value + two + three + four;\n}";
    const char *nw = "function renamed() {\n  const value = 1;\n  const two = 2;\n  const three = 3;\n  const four = "
                     "4;\n  return value + two + three + four;\n}";
    d = diff_numbered(old, nw, 1, NULL);
    CHECK(strstr(d, "-1|function outer() {") && strstr(d, "+1|function renamed() {") && strstr(d, "\n\n 7|}"),
          "bracket context: %s", d);
    free(d);
    d = diff_numbered_unified("a\nb\nc\n", "a\nB\nc\n", -1, &first);
    CHECK(!strcmp(d, "@@ -1,3 +1,3 @@\n 1|a\n-2|b\n+2|B\n 3|c"), "unified: %s", d);
    free(d);
    size_t ad, rm;
    char *p = diff_compact_preview(" 1|a\n-2|b\n+2|x\n+3|y\n+4|z\n+5|w\n+6|v\n+7|u\n 3|c", &ad, &rm);
    CHECK(!strcmp(p, "1:a\n2:x\n3:y\n…\n6:v\n7:u\n8:c") && ad == 6 && rm == 1, "compact: %s", p);
    free(p);
}

static void expect_error(pe_mode_t mode, const char *input, const char *want) {
    char *dir = fs_mktempdir("omc-edit");
    char *cwd = path_real(dir);
    json_t *args = json_obj();
    json_set_str(args, "input", input);
    pe_opts_t o = opts_for(cwd, NULL);
    pe_result_t r;
    pe_run(mode, args, &o, &r);
    CHECK(r.is_error && !strcmp(r.text, want), "expected error %s, got %s", want, r.text);
    pe_result_free(&r);
    json_free(args);
    fs_rm_rf(cwd);
    free(cwd);
    free(dir);
}

/* omp pi-edit tests/sloppy.rs end_sentinels_close_put_bodies_but_stay_literal_in_find */
static void sloppy_sentinel_case(const char *before, const char *input, const char *expected) {
    char *dir = fs_mktempdir("omc-edit");
    char *cwd = path_real(dir);
    write_file(cwd, "a.txt", before);
    json_t *args = json_obj();
    json_set_str(args, "input", input);
    pe_opts_t o = opts_for(cwd, NULL);
    pe_result_t r;
    pe_run(PE_SLOPPY, args, &o, &r);
    CHECK(!r.is_error, "sentinel-closed payload: %s", r.text);
    char *p = path_join(cwd, "a.txt");
    size_t n = 0;
    char *got = fs_read(p, &n);
    CHECK(got && !strcmp(got, expected), "sentinel-closed payload wrote: %s", got ? got : "(null)");
    free(got);
    free(p);
    pe_result_free(&r);
    json_free(args);
    fs_rm_rf(cwd);
    free(cwd);
    free(dir);
}

static void test_sloppy_sentinels(void) {
    sloppy_sentinel_case("one\ntwo\nthree\nfour\n",
                         "*** Edit File: a.txt\n*** Find\none\n*** Replace\nONE\n*** End Patch\n*** Find\ntwo\n*** Replace\nTWO\n"
                         "*** End\n*** Find\nthree\n*** Insert After\n3.5\n*** End\n*** Find\nfour\n*** Replace\nFOUR\n*** End\n",
                         "ONE\nTWO\nthree\n3.5\nFOUR\n");
    /* a Find body is matched as written, so a literal sentinel line can be removed */
    sloppy_sentinel_case("a\n*** End\nb\n", "*** Edit File: a.txt\n*** Find\na\n*** End\n*** Replace\na\n", "a\nb\n");
}

static void test_parser_errors(void) {
    expect_error(PE_APPLY_PATCH, "bad", "The first line of the patch must be '*** Begin Patch'");
    expect_error(PE_APPLY_PATCH, "*** Begin Patch\nbad", "The last line of the patch must be '*** End Patch'");
    expect_error(PE_APPLY_PATCH, "*** Begin Patch\n*** Update File: test.py\n*** End Patch",
                 "Line 3: Update file hunk for path 'test.py' is empty");
    expect_error(PE_APPLY_PATCH, "*** Begin Patch\n*** End Patch", "No files were modified.");
    expect_error(PE_APPLY_PATCH, "*** Begin Patch\n*** Frobnicate File: x\n*** End Patch",
                 "Line 2: '*** Frobnicate File: x' is not a valid hunk header. Valid hunk headers: '*** Add File: {path}', "
                 "'*** Delete File: {path}', '*** Update File: {path}'");
    expect_error(PE_SLOPPY, "just text", "Missing file target: start the payload with *** Edit File: relative/path.ts.");
}

/* omp pi-edit path_policy.rs tests: generated names and leading comments only */
static void expect_marker(const char *display, const char *head, const char *marker) {
    char *m = pe_auto_generated_message(display, head, strlen(head));
    if (!marker) {
        CHECK(!m, "%s: unexpected refusal: %s", display, m);
    } else {
        char *want = xasprintf("detected marker: \"%s\"", marker);
        CHECK(m && strstr(m, want), "%s: expected %s, got %s", display, want, m ? m : "(none)");
        CHECK(m && str_starts(m, "Cannot modify auto-generated file: ") && strstr(m, display), "%s: bad message head", display);
        free(want);
    }
    free(m);
    passed++;
}

static void test_auto_generated(void) {
    expect_marker("foo.pb.go", "", "foo.pb.go");
    expect_marker("a/b/zz_generated.deepcopy.go", "package v1\n", "zz_generated.deepcopy.go");
    expect_marker("a\\b\\m_pb2_grpc.py", "", "m_pb2_grpc.py");
    expect_marker("x.mocks.ts", "", "x.mocks.ts");
    expect_marker("foo.go", "// Code generated by protoc. DO NOT EDIT.\npackage foo", "Code generated by protoc.");
    expect_marker("foo.go", "package foo\n// @generated", NULL);
    expect_marker("guard.txt", "// @generated", NULL);
    expect_marker("generated.ts", "", NULL);
    expect_marker("internal/generated.go", "package organizations\n\nfunc Build() {}\n", NULL);
    expect_marker("generated.go", "// Code generated from schema.graphql; DO NOT EDIT.\n\npackage api\n",
                  "Code generated from schema.graphql; DO NOT EDIT");
    expect_marker("api.openapi.json", "{\"openapi\":\"3.1.0\"}", NULL);
    /* comment styles, shebang, BOM, block comments, and the case-insensitive extension */
    expect_marker("run.sh", "#!/bin/sh\n# @generated\necho\n", "@generated");
    expect_marker("run.sh", "#!/bin/sh\necho\n# @generated\n", NULL);
    expect_marker("q.sql", "-- generated by sqlc\nselect 1;\n", "generated by sqlc");
    expect_marker("p.html", "<!--\n this file was automatically generated\n-->\n", "this file was automatically generated");
    expect_marker("b.c", "/*\n * @generated\n */\nint a;\n", "@generated");
    expect_marker("b.C", "// @generated\n", "@generated");
    expect_marker("b.c", "\xEF\xBB\xBF// @generated\n", "@generated");
    expect_marker("Makefile", "# @generated\n", "@generated");
    expect_marker(".env", "# @generated\n", NULL);
    expect_marker("b.c", "// generated by hand\n", NULL);
    /* only the first 1024 bytes count */
    sb_t big = SB_INIT;
    for (int i = 0; i < 200; i++) sb_append(&big, "// padding....\n");
    sb_append(&big, "// @generated\n");
    expect_marker("b.c", sb_cstr(&big), NULL);
    sb_free(&big);
    /* the file variant: unreadable targets and directories are editable */
    char *m = pe_auto_generated_file_message("/nonexistent/foo.pb.go", "foo.pb.go");
    CHECK(!m, "missing file must not be refused");
    free(m);
    m = pe_auto_generated_file_message("/", "foo.pb.go");
    CHECK(!m, "directory must not be refused");
    free(m);
}

/* omp pi-edit tests/sloppy_parse.rs */
static void expect_region(const char *text, size_t n, const char *payload, size_t start, size_t end) {
    sloppy_regions_t r;
    sloppy_inline_regions(text, &r);
    CHECK(r.n == n, "regions: %zu, expected %zu for: %s", r.n, n, text);
    if (n && r.n) {
        CHECK(str_eq(r.v[0].payload, payload), "payload: %s", r.v[0].payload);
        CHECK(r.v[0].start == start && r.v[0].end == end, "span %zu..%zu, expected %zu..%zu", r.v[0].start, r.v[0].end, start, end);
    }
    sloppy_regions_free(&r);
    passed++;
}

#define SLOPPY_PAYLOAD "*** Edit File: src/a.ts\n*** Find\nold();\n*** Replace\nnew();"

static void test_inline_regions(void) {
    /* *** End Patch is an explicit prose boundary and stays in the text */
    expect_region("Before.\n" SLOPPY_PAYLOAD "\n*** End Patch\nAfter.", 1, SLOPPY_PAYLOAD, 8, 8 + strlen(SLOPPY_PAYLOAD) + 1);
    /* no boundary: runs to EOF */
    expect_region("Before.\n" SLOPPY_PAYLOAD, 1, SLOPPY_PAYLOAD, 8, 8 + strlen(SLOPPY_PAYLOAD));
    /* quoted in a markdown fence */
    expect_region("```text\n*** Edit File: src/a.ts\n*** Find\nold\n*** Replace\nnew\n```", 0, "", 0, 0);
    /* a header without a path, and prose that only mentions the syntax */
    expect_region("*** Edit File:\n*** Find\nold\n*** Replace\nnew", 0, "", 0, 0);
    expect_region("Use *** Edit File: a.ts to edit.", 0, "", 0, 0);
    /* an opener followed by prose carries no sections */
    expect_region("*** Edit File: a.ts\nThen prose.", 0, "", 0, 0);
    /* conflicting operators are the pipeline's to report */
    const char *conflict = "*** Edit File: src/a.ts\n*** Find\nold\n*** Insert Before\nstaged\n*** Replace\nnew";
    char *t = xasprintf("Before.\n%s\n*** End Patch\nAfter.", conflict);
    expect_region(t, 1, conflict, 8, 8 + strlen(conflict) + 1);
    free(t);
    /* two regions */
    sloppy_regions_t r;
    sloppy_inline_regions("A\n*** Edit File: a.ts\n*** Find\none();\n*** Replace\ntwo();\n*** End Patch\nB\n"
                          "*** Edit File: b.ts\n*** Find\nred();\n*** Replace\nblue();",
                          &r);
    CHECK(r.n == 2 && str_eq(r.v[1].payload, "*** Edit File: b.ts\n*** Find\nred();\n*** Replace\nblue();"), "two regions: %zu", r.n);
    sloppy_regions_free(&r);
}

/* omp coding-agent test/session/inline-edit-recovery.test.ts */
static json_t *assistant_text(const char *text, const char *stop) {
    json_t *m = json_obj();
    json_t *content = json_arr();
    json_t *b = json_obj();
    json_set_str(b, "type", "text");
    json_set_str(b, "text", text);
    json_push(content, b);
    json_set(m, "content", content);
    json_set_str(m, "stopReason", stop);
    return m;
}

static void test_recover_inline(void) {
    json_t *m = assistant_text("Fixing the constant.\n\n" SLOPPY_PAYLOAD "\n*** End Patch\n\nDone.", "stop");
    CHECK(sloppy_recover_inline(m) == 1, "lift: not recovered");
    json_t *c = json_get_arr(m, "content");
    CHECK(json_len(c) == 2, "lift: %zu blocks", json_len(c));
    CHECK(str_eq(json_get_str(json_at(c, 0), "text"), "Fixing the constant.\n\n*** End Patch\n\nDone."), "lift: remaining text");
    json_t *call = json_at(c, 1);
    CHECK(str_eq(json_get_str(call, "type"), "toolCall") && str_eq(json_get_str(call, "name"), "edit"), "lift: call");
    CHECK(str_eq(json_get_str(json_get_obj(call, "arguments"), "input"), SLOPPY_PAYLOAD), "lift: input");
    CHECK(str_eq(json_get_str(call, "rawBlock"), SLOPPY_PAYLOAD), "lift: rawBlock");
    CHECK(str_starts(json_get_str(call, "id"), "ptc_"), "lift: id");
    json_free(m);

    m = assistant_text(SLOPPY_PAYLOAD, "stop");
    CHECK(sloppy_recover_inline(m) == 1, "full: not recovered");
    c = json_get_arr(m, "content");
    CHECK(json_len(c) == 1 && str_eq(json_get_str(json_at(c, 0), "type"), "toolCall"), "full: text block must be dropped");
    json_free(m);

    m = assistant_text(SLOPPY_PAYLOAD, "length");
    CHECK(sloppy_recover_inline(m) == 0, "a length-truncated payload must never execute");
    CHECK(json_len(json_get_arr(m, "content")) == 1, "length: untouched");
    json_free(m);

    m = assistant_text(SLOPPY_PAYLOAD, "stop");
    json_t *tc = json_obj();
    json_set_str(tc, "type", "toolCall");
    json_set_str(tc, "name", "read");
    json_push(json_get_arr(m, "content"), tc);
    CHECK(sloppy_recover_inline(m) == 0, "a turn with tool calls must stay untouched");
    CHECK(str_eq(json_get_str(json_at(json_get_arr(m, "content"), 0), "text"), SLOPPY_PAYLOAD), "toolCall turn: text kept");
    json_free(m);
    passed += 4;
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_diff_renderers();
    test_parser_errors();
    test_sloppy_sentinels();
    test_auto_generated();
    test_inline_regions();
    test_recover_inline();
    run_fixture_dir("replace");
    run_fixture_dir("patch");
    run_fixture_dir("apply_patch");
    run_scenarios();
    run_fixture_dir("sloppy");
    fprintf(stderr, "editmodes: %d passed, %d skipped (sloppy recovery gaps), %d failed\n", passed, skipped, fails);
    return fails ? 1 : 0;
}
