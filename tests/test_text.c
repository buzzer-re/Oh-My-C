/* test_text.c - tests for src/text: regex, glob/gitignore, walker, search, yaml, fuzzy. */
#include "core/core.h"
#include "core/fs.h"
#include "core/json.h"
#include "text/fuzzy.h"
#include "text/glob.h"
#include "text/regex.h"
#include "text/search.h"
#include "text/walker.h"
#include "text/yaml.h"

#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (cond) g_pass++;                                                                                            \
        else {                                                                                                         \
            g_fail++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ======================================================================
 * regex
 * ====================================================================== */
typedef struct {
    const char *pat;
    int flags;
    const char *subj;
    int start, end; /* -1 = no match; -2 = compile error expected */
} rxcase_t;

static const rxcase_t RX_CASES[] = {
    {"abc", 0, "xxabcxx", 2, 5},
    {"a.c", 0, "a\nc abc", 4, 7},
    {"a.c", RX_DOTALL, "a\nc", 0, 3},
    {"(?s)a.c", 0, "a\nc", 0, 3},
    {"a+", 0, "baaab", 1, 4},
    {"a+?", 0, "baaab", 1, 2},
    {"a*", 0, "bbb", 0, 0},
    {"(a|ab)(c|bcd)", 0, "abcd", 0, 4},
    {"a|ab", 0, "ab", 0, 1},
    {"x{2,3}", 0, "xxxxx", 0, 3},
    {"x{2,3}?", 0, "xxxxx", 0, 2},
    {"x{3}", 0, "xx xxx", 3, 6},
    {"x{2,}", 0, "x xxxx", 2, 6},
    {"a{,3}", 0, "a{,3}", 0, 5}, /* invalid repetition -> literal braces */
    {"${platform}", 0, "a ${platform} b", -1, -1},
    {"\\$\\{platform\\}", 0, "a ${platform} b", 2, 13},
    {"[a-c]+", 0, "xxbcay", 2, 5},
    {"[^a-c]+", 0, "abcxyz", 3, 6},
    {"[]a]+", 0, "x]a]", 1, 4},
    {"[a-]+", 0, "x-a-", 1, 4},
    {"[[:digit:]]+", 0, "ab123", 2, 5},
    {"[[:^alpha:]]+", 0, "ab12c", 2, 4},
    {"[a-z&&[^aeiou]]+", 0, "aeibcd", 3, 6},
    {"[a-f--c]+", 0, "cabd", 1, 4},
    {"\\d+", 0, "abc 42", 4, 6},
    {"\\D+", 0, "12ab3", 2, 4},
    {"\\w+", 0, "  héllo_1 ", 2, 10},
    {"\\W+", 0, "ab, cd", 2, 4},
    {"\\s+", 0, "a \t\nb", 1, 4},
    {"\\S+", 0, "  ab ", 2, 4},
    {"\\bfoo\\b", 0, "afoo foo", 5, 8},
    {"\\Bfoo", 0, "foo afoo", 5, 8},
    {"\\<foo\\>", 0, "foobar foo", 7, 10},
    {"^foo", 0, "bar\nfoo", -1, -1},
    {"^foo", RX_MULTILINE, "bar\nfoo", 4, 7},
    {"(?m)^foo$", 0, "x\nfoo\ny", 2, 5},
    {"foo$", 0, "foo\n", -1, -1},
    {"foo\\z", 0, "foo", 0, 3},
    {"\\Afoo", 0, "xfoo", -1, -1},
    {"(?i)hello", 0, "say HeLLo", 4, 9},
    {"hello", RX_ICASE, "HELLO", 0, 5},
    {"(?i)straße", 0, "STRASSE straẞe", 8, 16},
    {"(?i)σ", 0, "ΣΑΣ", 0, 2},
    {"(?i)k", 0, "\xe2\x84\xaa", 0, 3}, /* KELVIN SIGN */
    {"(?i:a)b", 0, "AB Ab", 3, 5},
    {"(?i)a(?-i)b", 0, "AB Ab", 3, 5},
    {"é+", 0, "aéé", 1, 5},
    {".", 0, "é", 0, 2},
    {"[é-ë]", 0, "aê", 1, 3},
    {"\\x41\\x{42}\\u{43}", 0, "zABC", 1, 4},
    {"\\p{Greek}+", 0, "abc αβγ", 4, 10},
    {"\\pL+", 0, "123abc", 3, 6},
    {"\\P{L}+", 0, "ab12cd", 2, 4},
    {"\\p{Han}", 0, "a中", 1, 4},
    {"(?x) a b  # comment\n c", 0, "abc", 0, 3},
    {"a b", RX_EXTENDED, "ab", 0, 2},
    {"(?U)a+", 0, "aaa", 0, 1},
    {"", 0, "abc", 0, 0},
    {"(?:)", 0, "abc", 0, 0},
    {"a|", 0, "b", 0, 0},
    {"foo.bar", RX_LITERAL, "fooxbar foo.bar", 8, 15},
    {"FOO", RX_LITERAL | RX_ICASE, "a foo", 2, 5},
    {"a\\.b", 0, "axb a.b", 4, 7},
    {"a\\-b", 0, "a-b", 0, 3},
    {"\\Qx", 0, "", -2, -2},
    {"(?=a)a", 0, "ba", 1, 2},
    {"(a)\\1", 0, "xaa", 1, 3},
    {"(abc", 0, "", -2, -2},
    {"abc)", 0, "", -2, -2},
    {"*a", 0, "", -2, -2},
    {"[a", 0, "", -2, -2},
    {"a{3,2}", 0, "", -2, -2},
    {"[z-a]", 0, "", -2, -2},
    {"\\p{Bogus}", 0, "", -2, -2},
    {"a\nb", RX_LINE, "a\nb", -1, -1},
    {"a[^x]b", RX_LINE, "a\nb", -1, -1},
    {"a\\sb", RX_LINE, "a\nb a b", 4, 7},
    {"foo", RX_LINE, "x\nyfoo\n", 3, 6},
    {"^$", RX_LINE | RX_MULTILINE, "a\n\nb", 2, 2},
    {"(a*)*b", 0, "aaab", 0, 4},
    {"(a|b)*?c", 0, "abc", 0, 3},
};

static void test_regex_cases(void) {
    for (size_t i = 0; i < ARRAY_LEN(RX_CASES); i++) {
        const rxcase_t *c = &RX_CASES[i];
        char *err = NULL;
        rx_t *rx = rx_compile(c->pat, c->flags, &err);
        if (c->start == -2) {
            CHECK(rx == NULL && err != NULL, "regex '%s' should fail to compile", c->pat);
            free(err);
            rx_free(rx);
            continue;
        }
        CHECK(rx != NULL, "regex '%s' compile: %s", c->pat, err ? err : "");
        free(err);
        if (!rx) continue;
        rx_match_t m;
        bool ok = rx_search(rx, c->subj, strlen(c->subj), 0, &m);
        if (c->start < 0) {
            CHECK(!ok, "regex '%s' on '%s': expected no match, got %zu-%zu", c->pat, c->subj, m.start, m.end);
        } else {
            CHECK(ok && (int)m.start == c->start && (int)m.end == c->end, "regex '%s' on '%s': expected %d-%d got %s %zu-%zu",
                  c->pat, c->subj, c->start, c->end, ok ? "match" : "none", ok ? m.start : 0, ok ? m.end : 0);
        }
        /* rx_find and rx_is_match agree */
        size_t a, b;
        CHECK(rx_find(rx, c->subj, strlen(c->subj), 0, &a, &b) == ok, "rx_find agrees for '%s'", c->pat);
        CHECK(rx_is_match(rx, c->subj, strlen(c->subj), 0) == ok, "rx_is_match agrees for '%s'", c->pat);
        rx_free(rx);
    }
}

static void test_regex_groups(void) {
    char *err = NULL;
    rx_t *rx = rx_compile("(?P<key>\\w+)\\s*=\\s*(?<val>[^;]*)(;)?", 0, &err);
    CHECK(rx != NULL, "named groups compile: %s", err ? err : "");
    if (rx) {
        const char *s = "  name = omc;";
        rx_match_t m;
        CHECK(rx_search(rx, s, strlen(s), 0, &m), "named groups match");
        CHECK(rx_ngroups(rx) == 4, "ngroups=%d", rx_ngroups(rx));
        CHECK(m.groups[1].start == 2 && m.groups[1].end == 6, "group key %td-%td", m.groups[1].start, m.groups[1].end);
        CHECK(m.groups[2].start == 9 && m.groups[2].end == 12, "group val %td-%td", m.groups[2].start, m.groups[2].end);
        CHECK(m.groups[3].start == 12 && m.groups[3].end == 13, "group 3");
        CHECK(rx_group_index(rx, "val") == 2 && rx_group_index(rx, "nope") == -1, "group index");
        CHECK(str_eq(rx_group_name(rx, 1), "key"), "group name");
        rx_free(rx);
    }
    free(err);
    rx = rx_compile("(a)|(b)", 0, NULL);
    rx_match_t m;
    CHECK(rx_search(rx, "xb", 2, 0, &m) && m.groups[1].start == -1 && m.groups[2].start == 1, "unset group is -1");
    rx_free(rx);
    rx = rx_compile("(a+)(b+)?", 0, NULL);
    CHECK(rx_search(rx, "xaab", 4, 0, &m) && m.groups[1].end == 3 && m.groups[2].start == 3 && m.groups[2].end == 4,
          "greedy groups");
    rx_free(rx);
    /* search from an offset sees context before it */
    rx = rx_compile("\\bfoo", 0, NULL);
    CHECK(!rx_search(rx, "afoo", 4, 1, &m), "word boundary respects context before start");
    rx_free(rx);
    rx = rx_compile("^b", RX_MULTILINE, NULL);
    CHECK(rx_search(rx, "a\nb", 3, 2, &m) && m.start == 2, "line start at offset");
    rx_free(rx);
}

static void test_regex_misc(void) {
    CHECK(rx_has_uppercase("Foo"), "smart case upper");
    CHECK(!rx_has_uppercase("foo\\S\\W\\D"), "escapes don't count");
    CHECK(!rx_has_uppercase("\\p{Lu}x"), "\\p{Lu} doesn't count");
    CHECK(!rx_has_uppercase("(?P<Name>x)"), "group names don't count");
    CHECK(rx_has_uppercase("Ünïcode"), "unicode upper");
    char *e = rx_escape("a.b*c(d)");
    CHECK(str_eq(e, "a\\.b\\*c\\(d\\)"), "escape: %s", e);
    rx_t *rx = rx_compile(e, 0, NULL);
    CHECK(rx && rx_matches(rx, "xa.b*c(d)"), "escaped pattern matches literally");
    rx_free(rx);
    free(e);

    rx = rx_compile("(\\w+)@(\\w+)", 0, NULL);
    char *r = rx_replace(rx, "a@b, c@d", 8, "$2@$1", true);
    CHECK(str_eq(r, "b@a, d@c"), "replace all: %s", r);
    free(r);
    r = rx_replace(rx, "a@b, c@d", 8, "[${1}]$$", false);
    CHECK(str_eq(r, "[a]$, c@d"), "replace first: %s", r);
    free(r);
    rx_free(rx);
    rx = rx_compile("x*", 0, NULL);
    r = rx_replace(rx, "abc", 3, "-", true);
    CHECK(str_eq(r, "-a-b-c-"), "empty-match replace: %s", r);
    free(r);
    rx_free(rx);

    /* lenient grep compile */
    char *err = NULL;
    rx = rx_compile_grep("fetchProvider(", 0, &err);
    CHECK(rx && rx_matches(rx, "x = fetchProvider(a)"), "grep compile escapes stray paren");
    rx_free(rx);
    rx = rx_compile_grep("foo(bar|baz", 0, &err);
    CHECK(rx && rx_matches(rx, "foo(bar|baz"), "grep compile literal fallback");
    rx_free(rx);
    rx = rx_compile_grep("[unclosed", 0, &err);
    CHECK(rx && rx_matches(rx, "a[unclosed"), "grep compile literal fallback for class");
    rx_free(rx);
    rx = rx_compile_grep("(?<=a)b", 0, &err);
    CHECK(rx && rx_matches(rx, "ab") && !rx_matches(rx, "cb"), "look-behind via backtracking fallback");
    rx_free(rx);
    free(err);

    /* invalid UTF-8 haystack doesn't break anything */
    rx = rx_compile(".+", 0, NULL);
    rx_match_t m;
    CHECK(rx_search(rx, "\xff\xfe" "ab\xff", 5, 0, &m) && m.start == 2 && m.end == 4, "invalid utf8 skipped by .");
    rx_free(rx);
}

static void test_regex_perf(void) {
    size_t n = 200000;
    char *s = xmalloc(n + 2);
    memset(s, 'a', n);
    s[n] = '!';
    s[n + 1] = 0;
    static const char *const pats[] = {"(a*)*(b|c)", "^(a+)+$", "(a|aa)*(b|c)", "(a|a)*\\d", "(?:a?){30}a{30}x?$"};
    for (size_t i = 0; i < ARRAY_LEN(pats); i++) {
        rx_t *rx = rx_compile(pats[i], 0, NULL);
        int64_t t0 = mono_ms();
        rx_match_t m;
        bool ok = rx_search(rx, s, n + 1, 0, &m);
        int64_t dt = mono_ms() - t0;
        CHECK(dt < 3000, "pathological '%s' took %lld ms", pats[i], (long long)dt);
        if (i < 4) CHECK(!ok, "pathological '%s' should not match", pats[i]);
        rx_free(rx);
    }
    /* many-match iteration over a large buffer */
    sb_t b = SB_INIT;
    for (int i = 0; i < 20000; i++) sb_appendf(&b, "line %d: value_%d = foo(%d);\n", i, i * 7, i);
    rx_t *rx = rx_compile("value_\\d+ = foo\\((\\d+)\\)", RX_LINE | RX_MULTILINE, NULL);
    size_t pos = 0, cnt = 0;
    rx_match_t m;
    int64_t t0 = mono_ms();
    while (rx_search(rx, b.s, b.len, pos, &m)) {
        cnt++;
        pos = m.end > m.start ? m.end : m.end + 1;
    }
    CHECK(cnt == 20000, "iterated %zu matches", cnt);
    printf("  regex: 20000 capture matches over %zu KB in %lld ms\n", b.len / 1024, (long long)(mono_ms() - t0));
    rx_free(rx);
    sb_free(&b);
    free(s);
}

/* ======================================================================
 * glob / gitignore
 * ====================================================================== */
static void test_glob(void) {
    struct {
        const char *p, *s;
        int flags;
        bool e;
    } c[] = {
        {"*.c", "main.c", GLOB_PATHNAME, true},
        {"*.c", "src/main.c", GLOB_PATHNAME, false},
        {"*.c", "src/main.c", 0, true},
        {"**/*.c", "src/a/main.c", GLOB_PATHNAME, true},
        {"**/*.c", "main.c", GLOB_PATHNAME, true},
        {"src/**", "src/a/b.c", GLOB_PATHNAME, true},
        {"src/**", "src", GLOB_PATHNAME, false},
        {"a/**/b", "a/b", GLOB_PATHNAME, true},
        {"a/**/b", "a/x/y/b", GLOB_PATHNAME, true},
        {"a/**/b", "a/x/y/c", GLOB_PATHNAME, false},
        {"*.{c,h}", "x.h", GLOB_PATHNAME, true},
        {"*.{c,h}", "x.cpp", GLOB_PATHNAME, false},
        {"{src,test}/**/*.ts", "test/a/b.ts", GLOB_PATHNAME, true},
        {"{a,b{c,d}}", "bd", GLOB_PATHNAME, true},
        {"?.c", "ab.c", GLOB_PATHNAME, false},
        {"?.c", "é.c", GLOB_PATHNAME, true},
        {"[ab]?.c", "bx.c", GLOB_PATHNAME, true},
        {"[!ab]*", "cat", GLOB_PATHNAME, true},
        {"[^ab]*", "bat", GLOB_PATHNAME, false},
        {"[a-c]x", "bx", GLOB_PATHNAME, true},
        {"a\\*b", "a*b", GLOB_PATHNAME, true},
        {"a\\*b", "axb", GLOB_PATHNAME, false},
        {"*.TS", "x.ts", GLOB_PATHNAME | GLOB_ICASE, true},
        {"*.TS", "x.ts", GLOB_PATHNAME, false},
        {"**", "a/b/c", GLOB_PATHNAME, true},
        {"./src/*.ts", "src/a.ts", GLOB_PATHNAME, true},
        {"src/*.ts", "src/x/a.ts", GLOB_PATHNAME, false},
        {"*a*b*c*", "xxaxxbxxcxx", GLOB_PATHNAME, true},
        {"*a*b*c*d", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabc", GLOB_PATHNAME, false},
        {"[", "[", GLOB_PATHNAME, true},
        {"{a", "{a", GLOB_PATHNAME, true},
    };
    for (size_t i = 0; i < ARRAY_LEN(c); i++)
        CHECK(glob_match(c[i].p, c[i].s, c[i].flags) == c[i].e, "glob '%s' vs '%s' expected %d", c[i].p, c[i].s, c[i].e);
    char *n = glob_normalize("*.ts", true);
    CHECK(str_eq(n, "**/*.ts"), "normalize: %s", n);
    free(n);
    n = glob_normalize("src/*.ts", true);
    CHECK(str_eq(n, "src/*.ts"), "normalize keeps paths: %s", n);
    free(n);
    n = glob_normalize("{src,lib}", true);
    CHECK(str_eq(n, "{src,lib}"), "normalize brace union: %s", n);
    free(n);
    n = glob_normalize("*.{ts,js", true);
    CHECK(str_eq(n, "**/*.{ts,js}"), "normalize closes brace: %s", n);
    free(n);
    CHECK(glob_depth_bound("dir/*.json") == 2 && glob_depth_bound("**/x") == -1 && glob_depth_bound("*") == 1, "depth bound");
    CHECK(glob_has_magic("a*") && !glob_has_magic("a\\*b") && !glob_has_magic("plain"), "has_magic");

    ignore_set_t *s = ignore_set_new();
    const char *gi = "# comment\n*.log\n!keep.log\nbuild/\n/root.txt\ndocs/**/*.md\nfoo/bar\n\\#hash\ntrailing   \n"
                     "\\!bang\n**/gen/**\n*.o\n!/special.o\n";
    ignore_set_add_text(s, gi, strlen(gi));
    struct {
        const char *p;
        bool dir;
        int e;
    } ic[] = {
        {"a.log", false, 1},         {"x/y/a.log", false, 1},  {"x/keep.log", false, -1}, {"build", true, 1},
        {"build", false, 0},         {"x/build", true, 1},     {"root.txt", false, 1},    {"x/root.txt", false, 0},
        {"docs/a/b.md", false, 1},   {"docs/b.md", false, 1},  {"foo/bar", false, 1},     {"x/foo/bar", false, 0},
        {"#hash", false, 1},         {"trailing", false, 1},   {"!bang", false, 1},       {"a/gen/x.c", false, 1},
        {"main.c", false, 0},        {"a.o", false, 1},        {"special.o", false, -1},  {"x/special.o", false, 1},
    };
    for (size_t i = 0; i < ARRAY_LEN(ic); i++)
        CHECK((int)ignore_set_match(s, ic[i].p, ic[i].dir) == ic[i].e, "gitignore '%s' expected %d", ic[i].p, ic[i].e);
    CHECK(ignore_set_match_parents(s, "build/x/y.c", false) == IGNORE_IGNORED, "parent dir ignored");
    CHECK(ignore_set_match_parents(s, "src/y.c", false) == IGNORE_NONE, "parent not ignored");
    ignore_set_free(s);
}

/* ======================================================================
 * walker
 * ====================================================================== */
static void wfile(const char *root, const char *rel, const char *content) {
    char *p = path_join(root, rel);
    char *d = path_dirname(p);
    fs_mkdir_p(d, 0755);
    fs_write(p, content, strlen(content));
    free(d);
    free(p);
}

static bool list_has(const walk_list_t *l, const char *p) {
    for (size_t i = 0; i < l->n; i++)
        if (strcmp(l->v[i].path, p) == 0) return true;
    return false;
}

static char *make_tree(void) {
    char *root = fs_mktempdir("omc-text-test");
    char *git = path_join(root, ".git");
    fs_mkdir_p(git, 0755);
    free(git);
    wfile(root, ".gitignore", "*.log\nbuild/\n/secret.txt\n!important.log\n");
    wfile(root, "a.c", "int main(void) { return 0; }\n");
    wfile(root, "b.log", "log line\n");
    wfile(root, "important.log", "keep me TODO\n");
    wfile(root, "secret.txt", "TODO secret\n");
    wfile(root, "build/out.c", "TODO built\n");
    wfile(root, "src/x.ts", "const a = 1; // TODO: one\nconst b = 2;\nfunction foo() {\n  return a + b;\n}\n");
    wfile(root, "src/y.py", "def foo():\n    pass  # todo lower\n");
    wfile(root, "src/sub/.gitignore", "*.tmp\n");
    wfile(root, "src/sub/z.tmp", "TODO tmp\n");
    wfile(root, "src/sub/deep.c", "/* TODO deep */\n");
    wfile(root, "src/sub/secret.txt", "not root-anchored TODO\n");
    wfile(root, ".hidden/h.c", "TODO hidden\n");
    wfile(root, "node_modules/pkg/index.js", "TODO nm\n");
    wfile(root, ".ignore", "node_modules/\n");
    char *bin = path_join(root, "bin.dat");
    fs_write(bin, "TODO\0binary", 11);
    free(bin);
    return root;
}

static void test_walker(const char *root) {
    walk_opts_t o;
    memset(&o, 0, sizeof(o));
    walk_list_t l = {0};
    CHECK(walk_collect(root, &o, &l) == 0, "walk ok");
    CHECK(list_has(&l, "a.c") && list_has(&l, "src/x.ts") && list_has(&l, "src/sub/deep.c"), "walk finds files");
    CHECK(!list_has(&l, "b.log") && list_has(&l, "important.log"), "gitignore + negation");
    CHECK(!list_has(&l, "build/out.c") && !list_has(&l, "secret.txt") && list_has(&l, "src/sub/secret.txt"), "dir rule + anchored");
    CHECK(!list_has(&l, "src/sub/z.tmp"), "nested .gitignore");
    CHECK(!list_has(&l, ".hidden/h.c") && !list_has(&l, ".gitignore"), "hidden skipped");
    CHECK(!list_has(&l, "node_modules/pkg/index.js"), ".ignore honored");
    for (size_t i = 0; i < l.n; i++) CHECK(strncmp(l.v[i].path, ".git/", 5) != 0, ".git skipped");
    bool sorted = true;
    for (size_t i = 1; i < l.n; i++)
        if (walk_path_cmp(l.v[i - 1].path, l.v[i].path) >= 0) sorted = false;
    CHECK(sorted, "collect sorted");
    walk_list_free(&l);

    o.include_hidden = true;
    o.no_ignore = true;
    CHECK(walk_collect(root, &o, &l) == 0, "walk no-ignore");
    CHECK(list_has(&l, "b.log") && list_has(&l, ".hidden/h.c") && list_has(&l, "build/out.c") &&
              list_has(&l, "node_modules/pkg/index.js") && !list_has(&l, ".git"),
          "no-ignore + hidden");
    walk_list_free(&l);

    memset(&o, 0, sizeof(o));
    o.max_depth = 1;
    o.types = WALK_FILE | WALK_DIR;
    walk_collect(root, &o, &l);
    CHECK(list_has(&l, "a.c") && list_has(&l, "src") && !list_has(&l, "src/x.ts"), "max depth 1 + dirs");
    walk_list_free(&l);

    memset(&o, 0, sizeof(o));
    globset_t inc = {0}, exc = {0};
    globset_add(&inc, "**/*.c", GLOB_PATHNAME);
    globset_add(&exc, "**/sub", GLOB_PATHNAME);
    o.include = &inc;
    o.exclude = &exc;
    o.threads = 1;
    walk_collect(root, &o, &l);
    CHECK(l.n == 1 && list_has(&l, "a.c"), "include/exclude globs (n=%zu)", l.n);
    walk_list_free(&l);
    globset_free(&inc);
    globset_free(&exc);

    /* explicitly walking an ignored directory still works */
    char *build = path_join(root, "build");
    memset(&o, 0, sizeof(o));
    walk_collect(build, &o, &l);
    CHECK(list_has(&l, "out.c"), "walk inside ignored root dir");
    walk_list_free(&l);
    free(build);

    /* symlink loop with follow */
    char *lnk = path_join(root, "src/sub/loop");
    char *target = path_join(root, "src");
    int sl = symlink(target, lnk);
#ifdef _WIN32
    if (sl != 0 && errno == EPERM) { /* no Developer Mode / privilege: Windows creates no symlinks */
        fprintf(stderr, "  symlink checks skipped (symlinks need Developer Mode or admin on Windows)\n");
        goto no_symlink;
    }
#endif
    CHECK(sl == 0, "symlink");
    memset(&o, 0, sizeof(o));
    o.follow_symlinks = true;
    CHECK(walk_collect(root, &o, &l) == 0, "walk follow");
    CHECK(l.n < 100 && list_has(&l, "src/x.ts"), "symlink loop terminates (n=%zu)", l.n);
    walk_list_free(&l);
    memset(&o, 0, sizeof(o));
    o.types = WALK_SYMLINK;
    walk_collect(root, &o, &l);
    CHECK(l.n == 1 && list_has(&l, "src/sub/loop") && l.v[0].is_link, "unfollowed symlink reported as link");
    walk_list_free(&l);
    unlink(lnk);
#ifdef _WIN32
no_symlink:
#endif
    free(lnk);
    free(target);

    /* file root */
    char *f = path_join(root, "a.c");
    walk_collect(f, NULL, &l);
    CHECK(l.n == 1 && list_has(&l, "a.c"), "file root");
    walk_list_free(&l);
    free(f);
}

/* ======================================================================
 * search
 * ====================================================================== */
static const search_hit_t *find_hit(const search_result_t *r, const char *path, uint32_t line) {
    for (size_t i = 0; i < r->hits.n; i++)
        if (str_eq(r->hits.v[i].path, path) && r->hits.v[i].line == line) return &r->hits.v[i];
    return NULL;
}

static void test_search(const char *root) {
    search_opts_t o;
    memset(&o, 0, sizeof(o));
    o.pattern = "TODO";
    o.path = root;
    search_result_t r;
    CHECK(search_run(&o, &r) == 0, "search ok");
    CHECK(find_hit(&r, "src/x.ts", 1) && find_hit(&r, "important.log", 1) && find_hit(&r, "src/sub/deep.c", 1),
          "search finds matches");
    CHECK(!find_hit(&r, "b.log", 1) && !find_hit(&r, "build/out.c", 1) && !find_hit(&r, "bin.dat", 1), "ignored/binary skipped");
    CHECK(!find_hit(&r, "src/y.py", 2), "case sensitive by default");
    const search_hit_t *h = find_hit(&r, "src/x.ts", 1);
    CHECK(h && h->column == 17 && !h->is_context && str_eq(h->text, "const a = 1; // TODO: one"), "hit details col=%u",
          h ? h->column : 0);
    for (size_t i = 1; i < r.hits.n; i++)
        CHECK(walk_path_cmp(r.hits.v[i - 1].path, r.hits.v[i].path) <= 0, "hits ordered by path");
    CHECK(r.files_with_matches == r.hits.n && r.total_matches == r.hits.n, "totals %u/%u", r.files_with_matches, r.total_matches);
    search_result_free(&r);

    o.smart_case = true;
    o.pattern = "todo";
    search_run(&o, &r);
    CHECK(find_hit(&r, "src/y.py", 2) && find_hit(&r, "src/x.ts", 1), "smart case lower -> insensitive");
    search_result_free(&r);
    o.pattern = "Todo";
    search_run(&o, &r);
    CHECK(r.hits.n == 0, "smart case upper -> sensitive");
    search_result_free(&r);
    o.smart_case = false;

    /* context */
    char *xts = path_join(root, "src/x.ts");
    memset(&o, 0, sizeof(o));
    o.path = xts;
    o.pattern = "return";
    o.context_before = 2;
    o.context_after = 1;
    search_run(&o, &r);
    CHECK(r.hits.n == 4, "context hits n=%zu", r.hits.n);
    if (r.hits.n == 4) {
        CHECK(r.hits.v[0].line == 2 && r.hits.v[0].is_context && r.hits.v[1].line == 3 && r.hits.v[2].line == 4 &&
                  !r.hits.v[2].is_context && r.hits.v[2].column == 3 && r.hits.v[3].line == 5 && r.hits.v[3].is_context,
              "context lines ordered");
        CHECK(str_eq(r.hits.v[0].path, xts), "single file path kept");
    }
    search_result_free(&r);
    /* overlapping contexts are not duplicated */
    o.pattern = "const";
    o.context_before = 1;
    o.context_after = 1;
    search_run(&o, &r);
    CHECK(r.hits.n == 3 && !r.hits.v[0].is_context && !r.hits.v[1].is_context && r.hits.v[2].is_context, "merged context n=%zu",
          r.hits.n);
    search_result_free(&r);

    /* multiline (auto via \n) */
    memset(&o, 0, sizeof(o));
    o.path = xts;
    o.pattern = "foo\\(\\) \\{\\n\\s+return";
    search_run(&o, &r);
    CHECK(r.hits.n == 1 && r.hits.v[0].line == 3 && strchr(r.hits.v[0].text, '\n'), "multiline match");
    search_result_free(&r);

    /* fixed strings, word, max columns */
    o.pattern = "a + b";
    o.fixed_strings = true;
    search_run(&o, &r);
    CHECK(r.hits.n == 1 && r.hits.v[0].line == 4, "fixed string");
    search_result_free(&r);
    o.fixed_strings = false;
    o.pattern = "const";
    o.word = true;
    o.max_columns = 10;
    search_run(&o, &r);
    CHECK(r.hits.n == 2 && r.hits.v[0].truncated && strlen(r.hits.v[0].text) <= 10 && str_ends(r.hits.v[0].text, "..."),
          "max columns: '%s'", r.hits.n ? r.hits.v[0].text : "");
    search_result_free(&r);
    o.pattern = "one";
    o.word = false;
    search_run(&o, &r);
    CHECK(r.hits.n == 1 && r.hits.v[0].truncated && strstr(r.hits.v[0].text, "one") && r.hits.v[0].column == 23,
          "window around late match: '%s'", r.hits.n ? r.hits.v[0].text : "");
    search_result_free(&r);
    free(xts);

    /* modes, limits, filters */
    memset(&o, 0, sizeof(o));
    o.path = root;
    o.pattern = "TODO";
    o.mode = SEARCH_FILES;
    search_run(&o, &r);
    CHECK(r.hits.n >= 3 && r.hits.v[0].line == 0 && r.hits.v[0].text == NULL, "files mode n=%zu", r.hits.n);
    size_t nfiles = r.hits.n;
    search_result_free(&r);
    o.max_count = 2;
    search_run(&o, &r);
    CHECK(r.hits.n == 2 && r.limit_reached, "files mode limit");
    search_result_free(&r);
    o.mode = SEARCH_COUNT;
    o.max_count = 0;
    o.hidden = true;
    o.no_ignore = true;
    search_run(&o, &r);
    CHECK(r.hits.n > nfiles && r.hits.v[0].count >= 1, "count mode with no-ignore n=%zu", r.hits.n);
    search_result_free(&r);
    memset(&o, 0, sizeof(o));
    o.path = root;
    o.pattern = "TODO";
    o.type = "c";
    search_run(&o, &r);
    CHECK(r.hits.n == 1 && str_eq(r.hits.v[0].path, "src/sub/deep.c"), "type filter");
    search_result_free(&r);
    o.type = NULL;
    o.glob = "*.ts";
    search_run(&o, &r);
    CHECK(r.hits.n == 1 && str_eq(r.hits.v[0].path, "src/x.ts"), "glob filter");
    search_result_free(&r);
    o.glob = "src/sub";
    search_run(&o, &r);
    CHECK(r.hits.n == 2, "glob naming a dir selects its files (n=%zu)", r.hits.n);
    search_result_free(&r);
    o.glob = NULL;
    const char *ex[] = {"sub", "*.log"};
    o.exclude = ex;
    o.nexclude = 2;
    search_run(&o, &r);
    CHECK(r.hits.n == 1 && str_eq(r.hits.v[0].path, "src/x.ts"), "exclude (n=%zu)", r.hits.n);
    search_result_free(&r);
    o.nexclude = 0;
    o.offset = 1;
    o.max_count = 1;
    search_run(&o, &r);
    CHECK(r.hits.n == 1 && r.limit_reached, "offset+limit");
    json_t *j = search_result_to_json(&r);
    CHECK(json_get_int(j, "totalMatches", 0) >= 2 && json_is_arr(json_get(j, "matches")), "json result");
    json_free(j);
    search_result_free(&r);
    o.offset = 0;
    o.max_count = 0;
    o.search_binary = true;
    search_run(&o, &r);
    CHECK(find_hit(&r, "bin.dat", 1) != NULL, "binary searched when asked");
    search_result_free(&r);

    o.pattern = "(?<=x)y";
    CHECK(search_run(&o, &r) == 0, "look-behind search runs via backtracking fallback");
    search_result_free(&r);

    /* in-memory buffer */
    rx_t *rx = rx_compile("b", RX_LINE | RX_MULTILINE, NULL);
    memset(&o, 0, sizeof(o));
    o.context_after = 1;
    const char *buf = "a\nb\nc\nb\n";
    search_buffer(rx, buf, strlen(buf), &o, &r);
    CHECK(r.hits.n == 3 && r.total_matches == 2 && r.hits.v[1].is_context && r.hits.v[1].line == 3 && r.hits.v[2].line == 4,
          "buffer search n=%zu", r.hits.n);
    search_result_free(&r);
    rx_free(rx);
    CHECK(search_type_matches("ts", "a/b.tsx") && !search_type_matches("ts", "a.js") &&
              search_type_matches("make", "Makefile") && search_type_matches("vue", "x.vue"),
          "type table");
}

/* ======================================================================
 * yaml
 * ====================================================================== */
static json_t *yp(const char *s) {
    char *err = NULL;
    json_t *j = yaml_parse(s, strlen(s), &err);
    if (!j) {
        printf("  yaml error: %s\n", err);
        free(err);
    }
    return j;
}

static void yexpect(const char *yaml, const char *json) {
    json_t *a = yp(yaml);
    json_t *b = json_parse_str(json);
    char *d = a ? json_dump(a, 0) : xstrdup("(null)");
    CHECK(a && b && json_equal(a, b), "yaml %s\n  got  %s\n  want %s", yaml, d, json);
    free(d);
    json_free(a);
    json_free(b);
}

static void yerror(const char *yaml, const char *needle) {
    char *err = NULL;
    json_t *j = yaml_parse(yaml, strlen(yaml), &err);
    CHECK(!j && err && (!needle || strstr(err, needle)), "yaml should fail: %s (err=%s)", yaml, err ? err : "none");
    json_free(j);
    free(err);
}

static void roundtrip(const json_t *j) {
    char *y = yaml_dump(j);
    char *err = NULL;
    json_t *b = yaml_parse(y, strlen(y), &err);
    char *d = json_dump(j, 0);
    CHECK(b && json_equal(j, b), "yaml round trip of %s via:\n%s(err=%s)", d, y, err ? err : "");
    free(d);
    free(err);
    free(y);
    json_free(b);
}

static void test_yaml(void) {
    yexpect("a: 1\nb: two\nc: 1.5\nd: true\ne: ~\nf:\ng: yes\nh: null\n",
            "{\"a\":1,\"b\":\"two\",\"c\":1.5,\"d\":true,\"e\":null,\"f\":null,\"g\":\"yes\",\"h\":null}");
    yexpect("- a\n- 2\n- - x\n  - y\n- k: v\n  k2: v2\n", "[\"a\",2,[\"x\",\"y\"],{\"k\":\"v\",\"k2\":\"v2\"}]");
    yexpect("list:\n- a\n- b\nnext: 1\n", "{\"list\":[\"a\",\"b\"],\"next\":1}");
    yexpect("list:\n  - a\n  -   b\n", "{\"list\":[\"a\",\"b\"]}");
    yexpect("a:\n  b:\n    c: d\n  e: f\ng: h\n", "{\"a\":{\"b\":{\"c\":\"d\"},\"e\":\"f\"},\"g\":\"h\"}");
    yexpect("hex: 0x1f\noct: 0o17\nneg: -42\nexp: 1e3\nplus: +5\nver: 1.2.3\n",
            "{\"hex\":31,\"oct\":15,\"neg\":-42,\"exp\":1000,\"plus\":5,\"ver\":\"1.2.3\"}");
    yexpect("s: 'it''s'\nd: \"tab\\there \\u00e9 \\x41 \\\"q\\\"\"\n", "{\"s\":\"it's\",\"d\":\"tab\\there é A \\\"q\\\"\"}");
    yexpect("d: \"line one\n  line two\n\n  para\"\n", "{\"d\":\"line one line two\\npara\"}");
    yexpect("d: \"join\\\n  ed\"\n", "{\"d\":\"joined\"}");
    yexpect("p: this is\n  a plain\n\n  scalar\nq: x\n", "{\"p\":\"this is a plain\\nscalar\",\"q\":\"x\"}");
    yexpect("l: |\n  one\n    two\n  three\n", "{\"l\":\"one\\n  two\\nthree\\n\"}");
    yexpect("l: |-\n  one\n\n", "{\"l\":\"one\"}");
    yexpect("l: |+\n  one\n\n\nn: 1\n", "{\"l\":\"one\\n\\n\\n\",\"n\":1}");
    yexpect("f: >\n  folded\n  text\n\n  para\n    more\n  end\n", "{\"f\":\"folded text\\npara\\n  more\\nend\\n\"}");
    yexpect("f: >2\n   lead\n  x\n", "{\"f\":\" lead\\nx\\n\"}");
    yexpect("- |\n  in seq\n- after\n", "[\"in seq\\n\",\"after\"]");
    yexpect("f: [1, two, \"three\", [4], {five: 5}, ]\n", "{\"f\":[1,\"two\",\"three\",[4],{\"five\":5}]}");
    yexpect("f: {a: 1, b: [x, y],\n  c: {d: e}, empty: }\n", "{\"f\":{\"a\":1,\"b\":[\"x\",\"y\"],\"c\":{\"d\":\"e\"},\"empty\":null}}");
    yexpect("f: [a: 1, b]\n", "{\"f\":[{\"a\":1},\"b\"]}");
    yexpect("url: http://x.com:80/a#frag # comment\n", "{\"url\":\"http://x.com:80/a#frag\"}");
    yexpect("\"quoted key\": 1\n'single': 2\n? explicit\n: 3\n", "{\"quoted key\":1,\"single\":2,\"explicit\":3}");
    yexpect("1: a\ntrue: b\nnull: c\n", "{\"1\":\"a\",\"true\":\"b\",\"null\":\"c\"}");
    yexpect("base: &b\n  x: 1\n  y: 2\nd:\n  <<: *b\n  y: 3\nr: *b\ns: &s str\nt: *s\n",
            "{\"base\":{\"x\":1,\"y\":2},\"d\":{\"x\":1,\"y\":3},\"r\":{\"x\":1,\"y\":2},\"s\":\"str\",\"t\":\"str\"}");
    yexpect("a: &a {p: 1}\nb: &b {q: 2, p: 9}\nc:\n  <<: [*a, *b]\n", "{\"a\":{\"p\":1},\"b\":{\"q\":2,\"p\":9},\"c\":{\"p\":1,\"q\":2}}");
    yexpect("t: !!str 123\nu: !custom x\n", "{\"t\":\"123\",\"u\":\"x\"}");
    yexpect("%YAML 1.2\n---\na: 1\n...\n", "{\"a\":1}");
    yexpect("--- 5\n", "5");
    yexpect("", "null");
    yexpect("# only comment\n", "null");
    yexpect("dup: 1\ndup: 2\n", "{\"dup\":2}");
    yexpect("a:    # comment\n  b: c\n", "{\"a\":{\"b\":\"c\"}}");
    yexpect("emoji: \"\\U0001F600\"\n", "{\"emoji\":\"\xf0\x9f\x98\x80\"}");
    yexpect("\xef\xbb\xbf" "bom: 1\r\nwin: 2\r\n", "{\"bom\":1,\"win\":2}");
    yexpect("- - - deep\n", "[[[\"deep\"]]]");
    yexpect("key:\n- a\n-\n- c\n", "{\"key\":[\"a\",null,\"c\"]}");

    char *err = NULL;
    json_t *all = yaml_parse_all("a: 1\n---\nb: 2\n--- |\n  lit\n", 26, &err);
    CHECK(all && json_len(all) == 3 && json_get_int(json_at(all, 1), "b", 0) == 2 && str_eq(json_sv(json_at(all, 2)), "lit\n"),
          "multi-doc stream");
    json_free(all);

    yerror("a: 1\n  b: 2\n", "line 2");
    yerror("a:\n\tb: 1\n", "tab");
    yerror("a: \"unterminated\n", "unterminated");
    yerror("a: [1, 2\n", "flow");
    yerror("x: *nope\n", "unknown alias");
    yerror("- a\nb: 1\n", NULL);
    yerror("a: b: c\n", NULL);
    yerror("a: \"bad \\q escape\"\n", "escape");
    yerror("[a]: 1\n", "complex");

    /* depth cap */
    sb_t deep = SB_INIT;
    for (int i = 0; i < 400; i++) sb_putc(&deep, '[');
    yerror(deep.s, "depth");
    sb_clear(&deep);
    for (int i = 0; i < 400; i++) {
        sb_repeat(&deep, " ", i);
        sb_append(&deep, "- \n");
    }
    err = NULL;
    json_t *dj = yaml_parse(deep.s, deep.len, &err);
    CHECK(dj || err, "deep block nesting handled");
    json_free(dj);
    free(err);
    sb_free(&deep);

    /* billion laughs */
    const char *laughs = "a: &a [x,x,x,x,x,x,x,x,x,x]\nb: &b [*a,*a,*a,*a,*a,*a,*a,*a,*a,*a]\nc: &c [*b,*b,*b,*b,*b,*b,*b,*b,*b,*b]\n"
                         "d: &d [*c,*c,*c,*c,*c,*c,*c,*c,*c,*c]\ne: &e [*d,*d,*d,*d,*d,*d,*d,*d,*d,*d]\n"
                         "f: &f [*e,*e,*e,*e,*e,*e,*e,*e,*e,*e]\ng: [*f,*f,*f,*f,*f,*f,*f,*f,*f,*f]\n";
    yerror(laughs, "alias expansion");

    /* frontmatter */
    const char *md = "---\ntitle: Hello\ntags: [a, b]\n---\n# Body\ntext\n";
    const char *body = NULL;
    json_t *fm = yaml_frontmatter(md, strlen(md), &body, &err);
    CHECK(fm && str_eq(json_get_str(fm, "title"), "Hello") && body && str_starts(body, "# Body"), "frontmatter");
    json_free(fm);
    fm = yaml_frontmatter("# no fm\n", 8, &body, &err);
    CHECK(!fm && body && str_starts(body, "# no"), "no frontmatter");
    fm = yaml_frontmatter("---\n---\nbody", 12, &body, &err);
    CHECK(fm && json_is_obj(fm) && str_eq(body, "body"), "empty frontmatter");
    json_free(fm);

    /* realistic models.yml */
    const char *models =
        "providers:\n"
        "  defaults: &defaults\n"
        "    timeout: 30000\n"
        "    retries: 3\n"
        "  anthropic:\n"
        "    <<: *defaults\n"
        "    baseUrl: https://api.anthropic.com\n"
        "    apiKey: ${ANTHROPIC_API_KEY}\n"
        "    models:\n"
        "      - id: claude-opus\n"
        "        name: \"Claude Opus\"\n"
        "        contextWindow: 200000\n"
        "        cost: {input: 15, output: 75.5}\n"
        "        reasoning: true\n"
        "      - id: claude-haiku\n"
        "        input: [text, image]\n"
        "  local:\n"
        "    <<: *defaults\n"
        "    timeout: 5000\n"
        "    system: |\n"
        "      You are helpful.\n"
        "      Be concise.\n";
    json_t *mj = yp(models);
    CHECK(mj && json_get_int(json_path(mj, "providers.anthropic"), "retries", 0) == 3 &&
              json_get_int(json_path(mj, "providers.local"), "timeout", 0) == 5000 &&
              str_eq(json_sv(json_path(mj, "providers.anthropic.models.0.name")), "Claude Opus") &&
              json_get_num(json_path(mj, "providers.anthropic.models.0.cost"), "output", 0) == 75.5 &&
              str_eq(json_sv(json_path(mj, "providers.local.system")), "You are helpful.\nBe concise.\n") &&
              str_eq(json_sv(json_path(mj, "providers.anthropic.apiKey")), "${ANTHROPIC_API_KEY}"),
          "models.yml");
    if (mj) roundtrip(mj);
    json_free(mj);

    /* round trips of tricky values */
    const char *rts[] = {
        "{\"a\":\"\",\"b\":\"true\",\"c\":\"123\",\"d\":\" lead\",\"e\":\"trail \",\"f\":\"a: b\",\"g\":\"x #y\",\"h\":\"- dash\","
        "\"i\":\"multi\\nline\\n\",\"j\":\"no trailing\\nnl\",\"k\":\"keep\\n\\n\\n\",\"l\":\"  indented\\nfirst\",\"m\":\"tab\\tin\","
        "\"n\":\"ctrl\\u0001\",\"o\":\"null\",\"p\":\"~\",\"q\":\"1.5\",\"r\":\"é ünï\",\"s\":\"#hash\",\"t\":\"'q'\",\"u\":\"\\\"dq\\\"\","
        "\"v\":\"a\\r\\nb\",\"w\":\"---\",\"x\":\"\\n\\nlead nl\",\"y\":\"\\n\",\"z\":\"0x10\",\"aa\":\".inf\",\"ab\":\"{x}\"}",
        "{\"nums\":[0,-1,1.5,1e300,-2.25e-10,123456789012345,0.1],\"bools\":[true,false],\"n\":null}",
        "[[],{},[[]],[{}],{\"a\":[]},{\"b\":{}},[1,[2,[3]]],[{\"a\":1,\"b\":[{\"c\":null}]}]]",
        "{\"key with: colon\":1,\"\":2,\"- k\":3,\"multi\\nkey\":4,\"123\":5,\"true\":6}",
        "\"top level string\"",
        "\"multi\\nline top\\n\"",
        "42",
        "null",
        "[\"a\\nb\",{\"x\":\"c\\nd\\n\"},[\"e\\n\"]]",
    };
    for (size_t i = 0; i < ARRAY_LEN(rts); i++) {
        json_t *j = json_parse_str(rts[i]);
        CHECK(j != NULL, "json parse %zu", i);
        if (j) roundtrip(j);
        json_free(j);
    }
    json_t *inf = json_obj();
    json_set_num(inf, "x", INFINITY);
    char *y = yaml_dump(inf);
    CHECK(strstr(y, ".inf") != NULL, "inf emitted: %s", y);
    free(y);
    json_free(inf);

    /* robustness: random garbage must not crash */
    const char alpha[] = "ab:- \n\t#&*!|>'\"[]{},?%@`\\0x123.~\r";
    uint64_t seed = 12345;
    for (int it = 0; it < 4000; it++) {
        char buf[96];
        int n = (int)(seed % 90);
        for (int k = 0; k < n; k++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            buf[k] = alpha[(seed >> 33) % (sizeof(alpha) - 1)];
        }
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        char *e = NULL;
        json_t *j = yaml_parse(buf, (size_t)n, &e);
        if (j) {
            char *d = yaml_dump(j);
            json_t *b = yaml_parse(d, strlen(d), NULL);
            /* NUL bytes inside strings are the one known non-round-trippable case */
            if (!b) CHECK(false, "re-parse of dumped garbage failed:\n%s", d);
            json_free(b);
            free(d);
        }
        json_free(j);
        free(e);
    }
}

/* ======================================================================
 * fuzzy
 * ====================================================================== */
static void test_fuzzy(void) {
    CHECK(fuzzy_score("abc", "xaxbxc", NULL) > 0, "subsequence matches");
    CHECK(fuzzy_score("abd", "abc", NULL) == -1, "non-subsequence fails");
    CHECK(fuzzy_score("", "anything", NULL) == 0, "empty pattern");
    CHECK(fuzzy_score("fb", "foo_bar", NULL) > fuzzy_score("fb", "foobar", NULL), "boundary bonus");
    CHECK(fuzzy_score("fb", "fooBar", NULL) > fuzzy_score("fb", "foobar", NULL), "camelCase bonus");
    CHECK(fuzzy_score("abc", "abc", NULL) > fuzzy_score("abc", "a_b_c", NULL), "consecutive bonus");
    CHECK(fuzzy_score("main", "src/main.c", NULL) > fuzzy_score("main", "src/domain.c", NULL), "path separator bonus");
    CHECK(fuzzy_score("FB", "foobar", NULL) == -1 && fuzzy_score("FB", "FooBar", NULL) > 0, "smart case");
    CHECK(fuzzy_score("src main", "src/main.c", NULL) > 0 && fuzzy_score("src zzz", "src/main.c", NULL) == -1, "terms AND");
    int pos[8];
    int s = fuzzy_score("mc", "src/main.c", pos);
    CHECK(s > 0 && pos[0] == 4 && pos[1] == 9, "positions %d %d", pos[0], pos[1]);
    s = fuzzy_score("oo", "foo boo", pos);
    CHECK(s > 0 && pos[0] + 1 == pos[1], "positions consecutive");
    const char *items[] = {"packages/ai/src/models.ts", "src/core/model.c", "README.md", "src/ai/models.c",
                           "test/models_test.c", "docs/m-o-d-e-l.md"};
    fuzzy_hit_t *hits = NULL;
    size_t n = fuzzy_rank("model", items, ARRAY_LEN(items), &hits);
    CHECK(n == 5, "rank count %zu", n);
    CHECK(n >= 2 && (hits[0].index == 1 || hits[0].index == 3) && hits[n - 1].index == 5, "rank order first=%zu last=%zu", n ? hits[0].index : 99,
          n ? hits[n - 1].index : 99);
    for (size_t i = 1; i < n; i++) CHECK(hits[i - 1].score >= hits[i].score, "rank sorted");
    free(hits);
    const char *models[] = {"claude-opus-4", "claude-sonnet-4", "gpt-4o", "gemini-2.5-pro", "o3-mini"};
    n = fuzzy_rank("sonnet", models, 5, &hits);
    CHECK(n == 1 && hits[0].index == 1, "model pick");
    free(hits);
    n = fuzzy_rank("4o", models, 5, &hits);
    CHECK(n >= 1 && hits[0].index == 2, "model pick 4o");
    free(hits);
}

/* ======================================================================
 * benchmark
 * ====================================================================== */
static void bench(void) {
    const char *dir = "/usr/include";
    if (!fs_is_dir(dir)) {
        printf("  bench: %s not present, skipped\n", dir);
        return;
    }
    walk_opts_t wo;
    memset(&wo, 0, sizeof(wo));
    wo.include_hidden = true;
    wo.no_ignore = true;
    walk_list_t l = {0};
    int64_t t0 = mono_ms();
    walk_collect(dir, &wo, &l);
    printf("  bench: walk %s: %zu files in %lld ms\n", dir, l.n, (long long)(mono_ms() - t0));
    walk_list_free(&l);
    struct {
        const char *pat;
        search_mode_t mode;
        bool icase;
    } q[] = {
        {"EXPORT_SYMBOL", SEARCH_COUNT, false},
        {"define", SEARCH_CONTENT, false},
        {"\\w+_t \\*\\w+\\(", SEARCH_COUNT, false},
        {"(?i)mutex_lock", SEARCH_COUNT, false},
        {"[A-Z]{3,}_[0-9]+X\\b", SEARCH_COUNT, false},
        {"\\w+\\s*\\(", SEARCH_CONTENT, false},
    };
    for (size_t i = 0; i < ARRAY_LEN(q); i++) {
        search_opts_t o;
        memset(&o, 0, sizeof(o));
        o.pattern = q[i].pat;
        o.path = dir;
        o.mode = q[i].mode;
        o.hidden = true;
        o.no_ignore = true;
        o.max_file_size = 64u << 20;
        search_result_t r;
        t0 = mono_ms();
        search_run(&o, &r);
        printf("  bench: grep %-24s %-7s %7u matching lines in %4u files (%u searched) %4lld ms\n", q[i].pat,
               q[i].mode == SEARCH_COUNT ? "count" : "content", r.total_matches, r.files_with_matches, r.files_searched,
               (long long)(mono_ms() - t0));
        search_result_free(&r);
    }
}

int main(int argc, char **argv) {
    printf("regex...\n");
    test_regex_cases();
    test_regex_groups();
    test_regex_misc();
    test_regex_perf();
    printf("glob...\n");
    test_glob();
    char *root = make_tree();
    printf("walker...\n");
    test_walker(root);
    printf("search...\n");
    test_search(root);
    fs_rm_rf(root);
    free(root);
    printf("yaml...\n");
    test_yaml();
    printf("fuzzy...\n");
    test_fuzzy();
    if (!(argc > 1 && strcmp(argv[1], "--no-bench") == 0)) bench();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
