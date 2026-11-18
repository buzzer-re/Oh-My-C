/*
 * test_fuzz_regress - replay every input saved under tests/fuzz/regress/<harness>/ through the harness it was found
 * with (the harness sources are built here without libFuzzer, see tests/fuzz/fuzz.h). Each file is an input that once
 * crashed, hung or tripped a sanitizer; the test passes when every one runs to completion. Runs from the repo root.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define FUZZ_REGRESS 1 /* declarations only: the harness objects carry the entry points */
#include "../tests/fuzz/fuzz.h"
#include "core/core.h"
#include "core/json.h"
#include "core/rpcframe.h"
#include "tools/fuzzy.h"
#include "text/yaml.h"

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                      \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* direct checks of what fuzzing found, beside the replayed inputs */
static rpcframe_rc_t frame(sb_t *b, const char *in, char **body, char **err) {
    sb_clear(b);
    sb_append(b, in);
    const char *p;
    size_t n, used;
    *body = NULL;
    rpcframe_rc_t rc = rpcframe_next(b, &p, &n, &used, err);
    if (rc == RPCFRAME_MSG) {
        *body = xstrndup(p, n);
        rpcframe_consume(b, used);
    }
    return rc;
}

static void unit_checks(void) {
    sb_t b = SB_INIT;
    char *body, *err;
    CHECK(frame(&b, "Content-Length: 2\r\n\r\n{}tail", &body, &err) == RPCFRAME_MSG && !strcmp(body, "{}") && !strcmp(sb_cstr(&b), "tail"));
    free(body);
    CHECK(frame(&b, "content-length:\t3 \r\nContent-Type: x\r\n\r\n[1]", &body, &err) == RPCFRAME_MSG && !strcmp(body, "[1]"));
    free(body);
    CHECK(frame(&b, "Content-Length: 5\r\n\r\n{}", &body, &err) == RPCFRAME_NEED);
    CHECK(frame(&b, "hello\r\n\r\nContent-Length: 2\r\n\r\n{}", &body, &err) == RPCFRAME_RESYNC && b.len == 23);
    /* the DAP reader used to take these lengths through strtoull: -1 wrapped the frame end and read past the buffer */
    CHECK(frame(&b, "Content-Length: -1\r\n\r\n{}", &body, &err) == RPCFRAME_ERROR && err && !strcmp(err, "Invalid or duplicate JSON-RPC Content-Length"));
    free(err);
    CHECK(frame(&b, "Content-Length: 18446744073709551615\r\n\r\n{}", &body, &err) == RPCFRAME_ERROR && err && !strcmp(err, "JSON-RPC Content-Length exceeds 268435456-byte limit"));
    free(err);
    CHECK(frame(&b, "Content-Length: 1\r\nContent-Length: 1\r\n\r\n{", &body, &err) == RPCFRAME_ERROR);
    free(err);
    sb_clear(&b);
    for (int i = 0; i < 2000; i++) sb_append(&b, "X-Junk: 0123456789\r\n");
    const char *p;
    size_t n, used;
    CHECK(rpcframe_next(&b, &p, &n, &used, &err) == RPCFRAME_ERROR && err && !strcmp(err, "JSON-RPC header exceeds 16384-byte limit"));
    free(err);
    sb_free(&b);

    /* double -> int64 conversions of untrusted numbers saturate instead of being undefined */
    CHECK(json_d2i(1e300) == INT64_MAX && json_d2i(-1e300) == INT64_MIN && json_d2i(0.0 / 0.0) == 0 && json_d2i(-2.9) == -2);
    json_t *j = json_parse_str("{\"a\":1e999,\"b\":7.77e25,\"c\":-1e30}");
    CHECK(json_get_int(j, "a", 0) == INT64_MAX && json_get_int(j, "b", 0) == INT64_MAX && json_get_int(j, "c", 0) == INT64_MIN);
    json_free(j);
    CHECK(time_add_ms(INT64_MAX - 5, 10) == INT64_MAX && time_add_ms(100, 1e300) == INT64_MAX && time_add_ms(100, 0.0 / 0.0) == 100);
    CHECK(time_add_ms(1000, -250.0) == 750 && time_add_ms(-5, 1e30) == INT64_MAX && time_add_ms(5, -1e30) == INT64_MIN);

    /* YAML collection keys read as JS String(key), like Bun.YAML in omp (nested flow keys used to grow exponentially) */
    json_t *y = yaml_parse("{{a: 1}: x, [p, [q, ~], 2.50]: y}", 33, NULL);
    char *yd = json_dump(y, 0);
    CHECK(!strcmp(yd, "{\"[object Object]\":\"x\",\"p,q,,2.5\":\"y\"}"));
    free(yd);
    json_free(y);

    /* trim never crosses its own start on a stray continuation byte (" \x80" used to give a negative length) */
    char *t = fz_trim(" \x80");
    CHECK(!strcmp(t, "\x80"));
    free(t);
    t = fz_trim_end("\x80 ");
    CHECK(!strcmp(t, "\x80"));
    free(t);
    t = fz_trim("\xe3\x80\x80x\xe3\x80\x80"); /* U+3000 is JS whitespace */
    CHECK(!strcmp(t, "x"));
    free(t);
}

#define DECL(n)                                                                                                        \
    int fuzz_one_##n(const uint8_t *d, size_t s);                                                                      \
    void fuzz_setup_##n(void);
FUZZ_TARGETS(DECL)

typedef struct {
    const char *name;
    int (*one)(const uint8_t *, size_t);
    void (*setup)(void);
} target_t;

#define ROW(n) {#n, fuzz_one_##n, fuzz_setup_##n},
static const target_t targets[] = {FUZZ_TARGETS(ROW)};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
    unit_checks();
    const char *root = argc > 1 ? argv[1] : "tests/fuzz/regress";
    int total = 0, slow = 0;
    for (size_t t = 0; t < sizeof targets / sizeof *targets; t++) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/%s", root, targets[t].name);
        DIR *dp = opendir(dir);
        if (!dp) continue;
        char **names = NULL;
        size_t n = 0;
        struct dirent *e;
        while ((e = readdir(dp))) {
            if (e->d_name[0] == '.') continue;
            names = realloc(names, (n + 1) * sizeof *names);
            names[n++] = strdup(e->d_name);
        }
        closedir(dp);
        if (n > 1) qsort(names, n, sizeof *names, cmp);
        if (n) targets[t].setup();
        for (size_t i = 0; i < n; i++) {
            char path[2048];
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
            FILE *f = fopen(path, "rb");
            if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
            struct stat st;
            fstat(fileno(f), &st);
            uint8_t *buf = malloc((size_t)st.st_size + 1);
            size_t got = fread(buf, 1, (size_t)st.st_size, f);
            fclose(f);
            double t0 = now_s();
            targets[t].one(buf, got);
            double dt = now_s() - t0;
            /* a hang regression must now finish quickly (generous: valgrind / slow CI) */
            if (dt > 10.0) { fprintf(stderr, "SLOW %s: %.1fs\n", path, dt); slow++; }
            printf("ok %s/%s (%.3fs)\n", targets[t].name, names[i], dt);
            free(buf);
            free(names[i]);
            total++;
        }
        free(names);
    }
    printf("%d fuzz regression inputs replayed\n", total);
    if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
    return slow || failures ? 1 : 0;
}
