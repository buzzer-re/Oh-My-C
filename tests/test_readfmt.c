/* readfmt unit tests: archives, SQLite, PDF and notebooks against Python-generated fixtures,
 * plus a deterministic mutation fuzzer (every parser must survive corrupted input). */
#include "core/core.h"
#include "core/fs.h"
#include "tools/readfmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails = 0;
static char *DIR_;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static char *fx(const char *name) { return path_join(DIR_, name); }

static void expect_has(const char *what, const char *got, const char *want) {
    CHECK(got && strstr(got, want), "%s: missing %s\n--- got:\n%s", what, want, got ? got : "(null)");
}

static char *sqlite(const char *sub, const char *query, bool *err) {
    char *p = fx("t.db");
    char *r = rf_sqlite_read(p, sub, query, err);
    free(p);
    return r;
}

static void test_candidates(void) {
    rf_splits_t s;
    rf_archive_candidates("a/b.zip:x/y.txt:3", &s);
    CHECK(s.n == 1 && !strcmp(s.v[0].container, "a/b.zip") && !strcmp(s.v[0].sub, "x/y.txt:3"), "zip split");
    rf_splits_free(&s);
    rf_archive_candidates("x.tar.gz:m", &s);
    CHECK(s.n == 1 && !strcmp(s.v[0].container, "x.tar.gz") && !strcmp(s.v[0].sub, "m"), "tar.gz longest ext");
    rf_splits_free(&s);
    rf_archive_candidates("outer.zip:inner.jar:a.txt", &s);
    CHECK(s.n == 2 && !strcmp(s.v[0].container, "outer.zip:inner.jar") && !strcmp(s.v[1].container, "outer.zip"),
          "nested candidates longest first");
    rf_splits_free(&s);
    rf_archive_candidates("notes.txt", &s);
    CHECK(s.n == 0, "no archive ext");
    rf_splits_free(&s);
    rf_sqlite_candidates("data/app.db:users?limit=5", &s);
    CHECK(s.n == 1 && !strcmp(s.v[0].container, "data/app.db") && !strcmp(s.v[0].sub, "users") &&
              !strcmp(s.v[0].query, "limit=5"),
          "sqlite split");
    rf_splits_free(&s);
    rf_sqlite_candidates("x.dbx", &s);
    CHECK(s.n == 0, "sqlite ext must end at : ? or end");
    rf_splits_free(&s);
}

static char *list_text(rf_archive_t *a, const char *sub) {
    rf_dirents_t d;
    char *err = NULL;
    if (rf_archive_list(a, sub, &d, &err) != 0) return err;
    char *t = rf_format_dirents(d.v, d.n);
    rf_dirents_free(&d);
    return t;
}

static char *member(rf_archive_t *a, const char *sub) {
    size_t n;
    char *err = NULL, *norm = NULL;
    char *d = rf_archive_read(a, sub, &n, &norm, &err);
    free(norm);
    return d ? d : err;
}

static void test_archives(void) {
    char *err = NULL, *p = fx("t.zip");
    rf_archive_t *a = rf_archive_open(p, &err);
    CHECK(a, "zip open: %s", err);
    if (a) {
        char *t = list_text(a, "");
        CHECK(!strcmp(t, "bin.dat (10B)\ndir/\nhello.txt (16B)\nmany.txt (341B)\nZeta.txt (2B)"), "zip root: %s", t);
        free(t);
        t = list_text(a, "dir");
        CHECK(!strcmp(t, "inner.txt (11B)\nsub/"), "zip dir: %s", t);
        free(t);
        t = member(a, "hello.txt");
        CHECK(!strcmp(t, "hello zip\nline2\n"), "deflated member: %s", t);
        free(t);
        t = member(a, "./dir//inner.txt");
        CHECK(!strcmp(t, "inner text\n"), "stored member via unnormalized path: %s", t);
        free(t);
        size_t n;
        char *nm = NULL;
        char *b = rf_archive_read(a, "bin.dat", &n, &nm, &err);
        CHECK(b && n == 10 && !rf_is_text(b, n), "binary member");
        free(b);
        free(nm);
        t = member(a, "../etc/passwd");
        CHECK(strstr(t, "cannot contain '..'"), "dotdot rejected: %s", t);
        free(t);
        CHECK(rf_archive_stat(a, "dir/sub", NULL) == 2 && rf_archive_stat(a, "nope", NULL) == 0, "stat");
        rf_archive_free(a);
    }
    free(p);
    const char *tars[] = {"t.tar", "t.tgz", "t2.tar.gz"};
    for (size_t i = 0; i < 3; i++) {
        p = fx(tars[i]);
        a = rf_archive_open(p, &err);
        CHECK(a, "%s open: %s", tars[i], err);
        if (a) {
            char *t = list_text(a, "");
            CHECK(!strcmp(t, "a.txt (10B)\nd/\nlong/"), "%s root: %s", tars[i], t);
            free(t);
            t = member(a, "d/link.txt");
            CHECK(!strcmp(t, "alpha tar\n"), "%s symlink: %s", tars[i], t);
            free(t);
            char *ln = xasprintf("long/%0120d.txt", 0);
            for (char *q = ln + 5; *q != '.'; q++) *q = 'n';
            t = member(a, ln);
            CHECK(!strcmp(t, "long name member\n"), "%s long name: %s", tars[i], t);
            free(t);
            free(ln);
            rf_archive_free(a);
        }
        free(p);
    }
    p = fx("notes.txt.gz");
    a = rf_archive_open(p, &err);
    CHECK(a, "gz open");
    if (a) {
        char *t = member(a, "notes.txt");
        CHECK(!strcmp(t, "gzip single member\nsecond\n"), "gz member: %s", t);
        free(t);
        rf_archive_free(a);
    }
    free(p);
    err = NULL;
    a = rf_archive_open("/nonexistent/x.7z", &err);
    CHECK(!a && err && strstr(err, "Unsupported archive format"), "7z unsupported: %s", err ? err : "");
    free(err);
}

static void test_sqlite(void) {
    bool e;
    char *r = sqlite("", "", &e);
    CHECK(!e && !strcmp(r, "empty (0 rows)\nkv (500 rows)\nodd name (1 rows)\nplain (4 rows)\nusers (3000 rows)"), "list: %s", r);
    free(r);
    r = sqlite("users", "", &e);
    expect_has("schema", r, "CREATE TABLE users(id INTEGER PRIMARY KEY");
    expect_has("schema", r, "\n\nSample rows:\n| id  | name     | score | data      | note   |\n| --- | -------- |");
    expect_has("schema", r, "| 2   | user0002 | 3     | <BLOB 3B> | note 2 |");
    expect_has("schema", r, "[2995 more rows; append :users?limit=20&offset=5 to the database path to continue]");
    free(r);
    r = sqlite("users:42", "", &e);
    CHECK(!e && str_starts(r, "id: 42\nname: user0042\nscore: 63\ndata: <BLOB 3B>\nnote: xxxx"), "row: %s", r);
    free(r);
    r = sqlite("users:9999", "", &e);
    CHECK(!e && !strcmp(r, "No row found in table 'users' for key '9999'."), "missing row: %s", r);
    free(r);
    r = sqlite("kv:key499", "", &e);
    CHECK(!e && !strcmp(r, "k: key499\nv: 249001"), "without rowid pk: %s", r);
    free(r);
    r = sqlite("plain:2", "", &e);
    CHECK(!e && !strcmp(r, "a: 2\nb: two   with tab\nc: dflt"), "rowid lookup + default: %s", r);
    free(r);
    r = sqlite("plain", "where=a%3E1&order=a:desc&limit=2", &e);
    CHECK(!e && !strcmp(r, "| a   | b           | c     |\n| --- | ----------- | ----- |\n| 4   | four        | given |\n"
                           "| 3   | multi\\nline | dflt  |\n"
                           "[1 more rows; append :plain?limit=2&offset=2&where=a%3E1&order=a%3Adesc to the database path to continue]"),
          "query: %s", r);
    free(r);
    r = sqlite("", "q=SELECT count(*) AS n, sum(v) FROM kv WHERE k LIKE 'KEY1%25'", &e);
    CHECK(!e && !strcmp(r, "| n   | sum(v)  |\n| --- | ------- |\n| 100 | 2318350 |"), "raw aggregate: %s", r);
    free(r);
    r = sqlite("", "q=SELECT name FROM users ORDER BY score DESC LIMIT 2", &e);
    CHECK(!e && !strcmp(r, "| name     |\n| -------- |\n| user3000 |\n| user2999 |"), "raw order: %s", r);
    free(r);
    r = sqlite("", "q=SELECT id FROM users WHERE id BETWEEN 10 AND 20&limit=3&offset=2", &e);
    expect_has("raw page", r, "| 12  |\n| 13  |\n| 14  |\n[More rows; append ?q=SELECT+id+FROM+users+WHERE+id+BETWEEN+10+AND+20&limit=3&offset=5");
    free(r);
    r = sqlite("", "q=SELECT length(note), substr(name, 5), upper('x'), coalesce(NULL, 3) FROM users WHERE id = 42", &e);
    expect_has("raw funcs", r, "| 10000        | 0042            | X          | 3                 |");
    free(r);
    r = sqlite("odd name", "", &e);
    expect_has("quoted names", r, "| col one | n   |");
    free(r);
    /* hostile expression shapes: long chains stay under the token cap, deeper ones are rejected, never recursed */
    sb_t q = SB_INIT;
    sb_append(&q, "q=SELECT 1");
    for (int i = 0; i < 1500; i++) sb_append(&q, "%2B1");
    r = sqlite("", sb_cstr(&q), &e);
    CHECK(!e && strstr(r, "| 1501 "), "long chain: %.200s", r);
    free(r);
    for (int i = 0; i < 1500; i++) sb_append(&q, "%2B1");
    r = sqlite("", sb_cstr(&q), &e);
    CHECK(e && strstr(r, "SQL is too long"), "token cap: %.200s", r);
    free(r);
    sb_clear(&q);
    sb_append(&q, "q=SELECT ");
    for (int i = 0; i < 1000; i++) sb_putc(&q, '(');
    sb_putc(&q, '1');
    for (int i = 0; i < 1000; i++) sb_putc(&q, ')');
    r = sqlite("", sb_cstr(&q), &e);
    CHECK(e && strstr(r, "nested too deeply"), "paren depth: %.200s", r);
    free(r);
    sb_free(&q);
    struct {
        const char *sub, *q, *want;
    } errs[] = {
        {"users", "bogus=1", "Unsupported SQLite query parameter 'bogus'"},
        {"users", "limit=0", "SQLite limit must be a positive integer; got '0'"},
        {"users", "offset=-1", "SQLite offset must be a non-negative integer; got '-1'"},
        {"users", "where=1%3B", "must not contain comments or statement terminators"},
        {"users", "where=1 union select 1", "must not contain LIMIT/OFFSET/UNION"},
        {"users", "order=nope", "SQLite order column 'nope' not found in table schema"},
        {"users", "order=id:up", "SQLite order direction must be 'asc' or 'desc'; got 'up'"},
        {"users", "q=SELECT 1", "drop ':users' from the path"},
        {"", "q=", "SQLite query parameter 'q' cannot be empty"},
        {"", "limit=3", "SQLite query parameters require a table selector"},
        {"users:1", "limit=3", "SQLite row lookups cannot be combined with query parameters"},
        {"nope", "", "SQLite table 'nope' not found"},
        {"sqlite_master", "", "SQLite table 'sqlite_master' not found"},
        {"users:x", "", "Primary key 'x' must be an integer; got 'x'"},
        {"", "q=SELECT * FROM nope", "no such table: nope"},
        {"", "q=SELECT zz FROM users", "no such column: zz"},
        {"", "q=SELECT a FROM plain GROUP BY a", "single-table queries only"},
        {"", "q=UPDATE users SET name = 1", "readonly"},
    };
    for (size_t i = 0; i < ARRAY_LEN(errs); i++) {
        r = sqlite(errs[i].sub, errs[i].q, &e);
        CHECK(e && strstr(r, errs[i].want), "error %s?%s: %s", errs[i].sub, errs[i].q, r);
        free(r);
    }
    char *w = fx("wal.db");
    r = rf_sqlite_read(w, "events", "where=id%3E3", &e);
    CHECK(!e && strstr(r, "| 4   | wal row 4 |") && strstr(r, "| 5   | wal row 5 |"), "wal overlay: %s", r);
    free(r);
    free(w);
    CHECK(!rf_sqlite_sniff("/nonexistent"), "sniff missing");
}

static char *pdf_text(const char *name, int first, int last) {
    char *p = fx(name);
    size_t n;
    char *data = fs_read(p, &n);
    free(p);
    if (!data) return xstrdup("(unreadable)");
    strvec_t pages;
    char *err = NULL, *out;
    if (rf_pdf_extract(data, n, &pages, &err) != 0) out = err;
    else {
        out = rf_pdf_render(&pages, first, last, &err);
        if (!out) out = err;
        strvec_free(&pages);
    }
    free(data);
    return out;
}

static void test_pdf(void) {
    char *t = pdf_text("simple.pdf", 0, 0);
    CHECK(!strcmp(t, "<!-- Page 1 -->\nHello PDF world\nSecond line\n\n<!-- Page 2 -->\nPage two\n\nFrom the form"), "simple: %s", t);
    free(t);
    t = pdf_text("simple.pdf", 2, 2);
    CHECK(!strcmp(t, "<!-- Page 2 -->\nPage two\n\nFrom the form"), "page select: %s", t);
    free(t);
    t = pdf_text("objstm.pdf", 0, 0);
    CHECK(!strcmp(t, "Ünïcode ✓ ok"), "objstm/ToUnicode: %s", t);
    free(t);
    t = pdf_text("blank.pdf", 0, 0);
    CHECK(str_starts(t, "Text extraction is incomplete for PDF page 1."), "blank: %s", t);
    free(t);
    strvec_t pages;
    char *err = NULL;
    CHECK(rf_pdf_extract("hello", 5, &pages, &err) != 0 && strstr(err, "not a PDF"), "not a pdf");
    free(err);
}

static void test_ipynb(void) {
    char *p = fx("t.ipynb");
    size_t n;
    char *data = fs_read(p, &n), *err = NULL;
    char *t = rf_ipynb_text(data, n, "t.ipynb", &err);
    CHECK(t && !strcmp(t, "# %% [markdown] cell:0\n# Title\nSome *markdown*.\n# %% [code] cell:1\nx = 1\n# %%% [code] cell:9\n"
                          "print(x)\n# %% [raw] cell:2"),
          "notebook: %s", t ? t : err);
    free(t);
    free(data);
    free(p);
    const struct {
        const char *json, *want;
    } bad[] = {
        {"{", "Invalid JSON in notebook: x.ipynb"},
        {"[]", "Invalid notebook structure (expected object): x.ipynb"},
        {"{\"cells\": 1}", "Invalid notebook structure (missing cells array): x.ipynb"},
        {"{\"cells\": [{\"cell_type\": \"code\"}, {\"cell_type\": \"weird\"}]}", "Invalid notebook cell 1 in x.ipynb"},
    };
    for (size_t i = 0; i < ARRAY_LEN(bad); i++) {
        err = NULL;
        t = rf_ipynb_text(bad[i].json, strlen(bad[i].json), "x.ipynb", &err);
        CHECK(!t && err && !strcmp(err, bad[i].want), "bad notebook %zu: %s", i, err ? err : t);
        free(err);
        free(t);
    }
}

/* ---------- fuzz ---------- */

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void mutate(char *b, size_t *n) {
    int k = 1 + (int)(rnd() % 8);
    for (int i = 0; i < k && *n; i++) {
        switch (rnd() % 4) {
        case 0: b[rnd() % *n] = (char)rnd(); break;
        case 1: b[rnd() % *n] ^= (char)(1 << (rnd() % 8)); break;
        case 2: { /* 0xFF / 0x00 runs hit length and count fields */
            size_t at = rnd() % *n, want = (size_t)(1 + rnd() % 8), len = MIN(want, *n - at);
            memset(b + at, rnd() % 2 ? 0xff : 0, len);
            break;
        }
        default:
            if (rnd() % 4 == 0) *n = rnd() % *n;
        }
    }
}

static void walk_archive(rf_archive_t *a, const char *dir, int depth) {
    rf_dirents_t d;
    char *err = NULL;
    if (depth > 6 || rf_archive_list(a, dir, &d, &err) != 0) {
        free(err);
        return;
    }
    for (size_t i = 0; i < d.n; i++) {
        char *p = *dir ? xasprintf("%s/%s", dir, d.v[i].name) : xstrdup(d.v[i].name);
        if (d.v[i].is_dir) walk_archive(a, p, depth + 1);
        else free(member(a, p));
        free(p);
    }
    rf_dirents_free(&d);
}

static void fuzz_one(const char *name, int iters) {
    char *src = fx(name);
    size_t n0;
    char *orig = fs_read(src, &n0);
    free(src);
    if (!orig) {
        CHECK(0, "fuzz fixture %s missing", name);
        return;
    }
    char *tmp = xasprintf("%s/fuzz-%s", DIR_, name);
    char *buf = xmalloc(n0 + 1);
    static const char *const SQ_SUBS[][2] = {{"", ""}, {"users", ""}, {"users:42", ""}, {"kv:key007", ""}, {"plain", ""},
                                             {"users", "limit=5&offset=3&where=id%3E3 AND note IS NOT NULL&order=name:desc"},
                                             {"", "q=SELECT count(*), max(name) FROM users WHERE name LIKE 'u%25'"},
                                             {"", "q=SELECT * FROM kv ORDER BY v DESC LIMIT 3"}};
    for (int it = 0; it < iters; it++) {
        memcpy(buf, orig, n0);
        size_t n = n0;
        mutate(buf, &n);
        if (str_ends(name, ".pdf")) {
            strvec_t pages;
            char *err = NULL;
            if (rf_pdf_extract(buf, n, &pages, &err) == 0) {
                char *t = rf_pdf_render(&pages, 0, 0, &err);
                free(t);
                strvec_free(&pages);
            }
            free(err);
            continue;
        }
        if (str_ends(name, ".ipynb")) {
            char *err = NULL;
            free(rf_ipynb_text(buf, n, name, &err));
            free(err);
            continue;
        }
        fs_write(tmp, buf, n);
        if (str_ends(name, ".db")) {
            for (size_t k = 0; k < ARRAY_LEN(SQ_SUBS); k++) {
                bool e;
                free(rf_sqlite_read(tmp, SQ_SUBS[k][0], SQ_SUBS[k][1], &e));
            }
            continue;
        }
        char *err = NULL;
        rf_archive_t *a = rf_archive_open(tmp, &err);
        free(err);
        if (a) {
            walk_archive(a, "", 0);
            rf_archive_free(a);
        }
    }
    unlink(tmp);
    free(tmp);
    free(buf);
    free(orig);
}

int main(void) {
    if (system("command -v python3 >/dev/null 2>&1") != 0) {
        printf("test_readfmt: python3 not found, skipping\n");
        return 0;
    }
    DIR_ = fs_mktempdir("omc-readfmt");
    char *cmd = xasprintf("python3 -I tests/readfmt_fixtures.py '%s' >/dev/null", DIR_);
    if (system(cmd) != 0) {
        fprintf(stderr, "fixture generation failed\n");
        return 1;
    }
    free(cmd);
    test_candidates();
    test_archives();
    test_sqlite();
    test_pdf();
    test_ipynb();
    int iters = getenv("READFMT_FUZZ") ? atoi(getenv("READFMT_FUZZ")) : 150;
    const char *fz[] = {"t.zip", "t.tar", "t.tgz", "notes.txt.gz", "t.db", "simple.pdf", "objstm.pdf", "t.ipynb", "docs.zip"};
    for (size_t i = 0; i < ARRAY_LEN(fz); i++) fuzz_one(fz[i], iters);
    fs_rm_rf(DIR_);
    free(DIR_);
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    else printf("test_readfmt: ok\n");
    return fails ? 1 : 0;
}
