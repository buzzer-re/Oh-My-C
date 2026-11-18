/*
 * fuzz.h - shared glue for the libFuzzer harnesses in tests/fuzz/.
 *
 * Every harness is one file, fuzz_<name>.c, that defines its entry point with FUZZ_ENTRY and an optional one-time
 * setup with FUZZ_SETUP. Built by `make fuzz` (clang + -fsanitize=fuzzer,address,undefined) each one is a libFuzzer
 * binary, build/fuzz/fuzz_<name>. Built by the normal gcc build with -DFUZZ_REGRESS the same file becomes an object
 * exporting fuzz_one_<name>() / fuzz_setup_<name>(), which tests/test_fuzz_regress.c runs over every saved input in
 * tests/fuzz/regress/<name>/ (crashes and hangs found by fuzzing, kept as regressions).
 *
 * Harnesses must never touch the real home or agent dir (FUZZ_SETUP points both at a throwaway dir), never use the
 * network beyond loopback, and never abort on an input that is merely invalid.
 */
#ifndef OMC_FUZZ_H
#define OMC_FUZZ_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* the harness list (test_fuzz_regress walks it) */
#define FUZZ_TARGETS(X)                                                                                                \
    X(json) X(yaml) X(regex) X(glob) X(sse) X(bedrock) X(http) X(codec) X(markdown) X(ansi) X(htmlform) X(img)         \
    X(edit) X(session) X(rpcframe) X(approval) X(pb)

#define FUZZ_CAT2(a, b) a##b
#define FUZZ_CAT(a, b) FUZZ_CAT2(a, b)

#ifdef FUZZ_REGRESS
#define FUZZ_ENTRY int FUZZ_CAT(fuzz_one_, FUZZ_NAME)(const uint8_t *data, size_t size)
#define FUZZ_SETUP void FUZZ_CAT(fuzz_setup_, FUZZ_NAME)(void)
#else
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);
static void fuzz_setup_(void);
#define FUZZ_ENTRY int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
#define FUZZ_SETUP                                                                                                     \
    int LLVMFuzzerInitialize(int *argc, char ***argv) {                                                                \
        fuzz_isolate();                                                                                                \
        fuzz_setup_();                                                                                                 \
        return 0;                                                                                                      \
    }                                                                                                                  \
    static void fuzz_setup_(void)
#endif

/* point HOME and the agent dirs at a fresh throwaway dir (never the user's ~/.omc / ~/.omp) */
static inline void fuzz_isolate(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *t = getenv("TMPDIR");
    char tmpl[512];
    snprintf(tmpl, sizeof tmpl, "%s/omc-fuzz-XXXXXX", t && *t ? t : "/tmp");
    char *d = mkdtemp(tmpl);
    if (!d) return;
    char buf[600];
    setenv("HOME", d, 1);
    snprintf(buf, sizeof buf, "%s/.omc/agent", d);
    setenv("OMC_CODING_AGENT_DIR", buf, 1);
    snprintf(buf, sizeof buf, "%s/.omp/agent", d);
    setenv("PI_CODING_AGENT_DIR", buf, 1);
    setenv("OMC_NO_CRASH_HANDLER", "1", 1);
}

/* NUL-terminated owned copy of the input */
static inline char *fuzz_cstr(const uint8_t *d, size_t n) {
    char *s = malloc(n + 1);
    if (!s) abort();
    memcpy(s, d, n);
    s[n] = 0;
    return s;
}

/* consume the first byte as a selector (0 when empty) */
static inline unsigned fuzz_byte(const uint8_t **d, size_t *n) {
    if (!*n) return 0;
    unsigned b = **d;
    (*d)++;
    (*n)--;
    return b;
}

/* split the input at the first "\n\x01\n" (or the first NUL when `nul`): head and tail as owned C strings */
static inline void fuzz_split(const uint8_t *d, size_t n, char **head, size_t *hl, char **tail, size_t *tl) {
    const uint8_t *sep = NULL;
    size_t sl = 0;
    for (size_t i = 0; i + 2 < n; i++)
        if (d[i] == '\n' && d[i + 1] == 1 && d[i + 2] == '\n') { sep = d + i; sl = 3; break; }
    size_t h = sep ? (size_t)(sep - d) : n;
    *head = fuzz_cstr(d, h);
    *hl = h;
    size_t rest = sep ? n - h - sl : 0;
    *tail = fuzz_cstr(sep ? sep + sl : d + n, rest);
    *tl = rest;
}

#define FUZZ_ASSERT(c)                                                                                                 \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: fuzz property failed: %s\n", __FILE__, __LINE__, #c);                             \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)

#endif
