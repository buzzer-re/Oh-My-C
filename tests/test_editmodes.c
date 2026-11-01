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
    "rejects an overlapping op that no longer matches the earlier op's result atomically",
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

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_diff_renderers();
    test_parser_errors();
    run_fixture_dir("replace");
    run_fixture_dir("patch");
    run_fixture_dir("apply_patch");
    run_scenarios();
    run_fixture_dir("sloppy");
    fprintf(stderr, "editmodes: %d passed, %d skipped (sloppy recovery gaps), %d failed\n", passed, skipped, fails);
    return fails ? 1 : 0;
}
