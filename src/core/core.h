/*
 * omc - oh-my-c
 * core.h: base types, allocation, string builder, vectors, misc utilities.
 *
 * Conventions:
 *   - All allocation goes through x* helpers which abort on OOM.
 *   - Strings are NUL-terminated UTF-8 unless a length is passed explicitly.
 *   - Functions returning `char *` hand ownership to the caller (free()).
 *   - Functions returning `const char *` return borrowed memory.
 */
#ifndef OMC_CORE_H
#define OMC_CORE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "core/compat.h"

/* the omp release whose behaviour this build matches (UPDATE.md pin); release tags are v<OMC_VERSION> */
#define OMC_VERSION "19.1.1"
#define OMC_VERSION_LABEL "v" OMC_VERSION " (OMP equivalent)"
#define OMC_NAME "omc"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define UNUSED(x) ((void)(x))

#if defined(__GNUC__) && defined(_WIN32)
/* MinGW: `printf` names the Microsoft CRT's format checker; omc builds with the C99 stdio (__USE_MINGW_ANSI_STDIO) */
#define PRINTF_LIKE(f, a) __attribute__((format(gnu_printf, f, a)))
#define NORETURN __attribute__((noreturn))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(__GNUC__)
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#define NORETURN __attribute__((noreturn))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define PRINTF_LIKE(f, a)
#define NORETURN
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* ---------- allocation ---------- */
NORETURN void die(const char *fmt, ...) PRINTF_LIKE(1, 2);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
/* qsort that accepts an empty array: VEC storage is NULL until the first push, and qsort(NULL, 0, ...) is undefined
 * (glibc declares the base nonnull) */
static inline void xqsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    if (n > 1) qsort(base, n, size, cmp);
}
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) PRINTF_LIKE(1, 2);
char *xvasprintf(const char *fmt, va_list ap);

/* ---------- string builder ---------- */
typedef struct {
    char *s;
    size_t len;
    size_t cap;
} sb_t;

#define SB_INIT {NULL, 0, 0}
void sb_reserve(sb_t *b, size_t extra);
void sb_append(sb_t *b, const char *s);
void sb_appendn(sb_t *b, const char *s, size_t n);
void sb_putc(sb_t *b, char c);
void sb_appendf(sb_t *b, const char *fmt, ...) PRINTF_LIKE(2, 3);
void sb_vappendf(sb_t *b, const char *fmt, va_list ap);
void sb_clear(sb_t *b);
void sb_free(sb_t *b);
char *sb_detach(sb_t *b); /* returns owned string ("" if empty), resets builder */
const char *sb_cstr(sb_t *b); /* never NULL */
void sb_truncate(sb_t *b, size_t len);
void sb_append_utf8(sb_t *b, uint32_t cp);
void sb_repeat(sb_t *b, const char *s, int n);

/* ---------- generic vector ---------- */
#define VEC(T) struct { T *v; size_t n, cap; }
#define vec_push(vec, x)                                                         \
    do {                                                                         \
        if ((vec)->n == (vec)->cap) {                                            \
            (vec)->cap = (vec)->cap ? (vec)->cap * 2 : 8;                        \
            (vec)->v = xrealloc((vec)->v, (vec)->cap * sizeof(*(vec)->v));       \
        }                                                                        \
        (vec)->v[(vec)->n++] = (x);                                              \
    } while (0)
#define vec_free(vec) do { free((vec)->v); (vec)->v = NULL; (vec)->n = (vec)->cap = 0; } while (0)
#define vec_pop(vec) ((vec)->v[--(vec)->n])
#define vec_last(vec) ((vec)->v[(vec)->n - 1])

typedef VEC(char *) strvec_t;
void strvec_push(strvec_t *v, const char *s);    /* copies */
void strvec_push_own(strvec_t *v, char *s);      /* takes ownership */
void strvec_free(strvec_t *v);
bool strvec_contains(const strvec_t *v, const char *s);
char *strvec_join(const strvec_t *v, const char *sep);
void strvec_sort(strvec_t *v);
strvec_t str_split(const char *s, const char *sep); /* empty fields preserved */
strvec_t str_split_lines(const char *s, size_t len); /* splits on \n, strips \r */

/* ---------- string helpers ---------- */
bool str_starts(const char *s, const char *prefix);
bool str_ends(const char *s, const char *suffix);
bool str_eq(const char *a, const char *b); /* NULL-safe */
bool str_ieq(const char *a, const char *b);
bool str_empty(const char *s);
char *str_trim(const char *s);              /* owned copy */
char *str_replace(const char *s, const char *from, const char *to);
char *str_lower(const char *s);
const char *str_istr(const char *hay, const char *needle);
size_t str_count(const char *s, const char *needle);
char *str_indent(const char *s, const char *prefix);
int str_cmp_natural(const char *a, const char *b);

/* ---------- utf-8 ---------- */
int utf8_decode(const char *s, size_t len, uint32_t *cp); /* returns bytes consumed (>=1); invalid -> U+FFFD, 1 */
int utf8_encode(uint32_t cp, char out[4]);
bool utf8_valid(const char *s, size_t len);
size_t utf8_strlen(const char *s);
int wcwidth_cp(uint32_t cp); /* terminal column width of a code point */
size_t str_display_width(const char *s); /* ANSI-aware */
char *utf8_sanitize(const char *s, size_t len); /* replace invalid sequences with U+FFFD */

/* ---------- hashing ---------- */
uint64_t hash_fnv1a(const void *data, size_t len);
uint64_t hash_xx64(const void *data, size_t len, uint64_t seed);
uint32_t hash_xx32(const void *data, size_t len, uint32_t seed);

/* ---------- string -> pointer hash map ---------- */
typedef struct {
    char *key;
    void *val;
    uint64_t hash;
} hmap_entry_t;

typedef struct {
    hmap_entry_t *e;
    size_t cap;
    size_t n;
} hmap_t;

void hmap_init(hmap_t *m);
void *hmap_get(const hmap_t *m, const char *key);
bool hmap_has(const hmap_t *m, const char *key);
void *hmap_put(hmap_t *m, const char *key, void *val); /* returns old value or NULL */
void *hmap_del(hmap_t *m, const char *key);
void hmap_free(hmap_t *m, void (*free_val)(void *));
#define hmap_foreach(m, it) for (hmap_entry_t *it = (m)->e; (m)->e && it < (m)->e + (m)->cap; it++) if (it->key)

/* ---------- time ---------- */
int64_t now_ms(void);      /* wall clock */
int64_t mono_ms(void);     /* monotonic */
/* base + ms for deadlines and expiries, saturating instead of overflowing (ms may be huge, infinite or NaN = 0 when
 * it comes from a server or the model) */
int64_t time_add_ms(int64_t base, double ms);
char *iso8601_now(void);   /* owned, UTC with ms */
char *iso8601_from_ms(int64_t ms);
void sleep_ms(int64_t ms);

/* ---------- random / ids ---------- */
void rand_bytes(void *buf, size_t n);
char *uuid_v4(void);
/* pi-utils Snowflake.next(): 42-bit ms since 2015-01-01 << 22 | a random-seeded 22-bit sequence, 16 hex chars */
char *snowflake_next(void);
char *uuid_v7(void);
char *rand_hex(size_t nbytes);
char *short_id(void); /* 8 hex chars */

/* ---------- base64 / hex ---------- */
char *base64_encode(const void *data, size_t len);
char *base64url_encode(const void *data, size_t len);
unsigned char *base64_decode(const char *s, size_t len, size_t *out_len);
char *hex_encode(const void *data, size_t len);

/* ---------- logging ---------- */
typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;
void log_init(const char *path, log_level_t level);
void log_msg(log_level_t lvl, const char *fmt, ...) PRINTF_LIKE(2, 3);
#define LOGD(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGE(...) log_msg(LOG_ERROR, __VA_ARGS__)

/* ---------- env ---------- */
const char *env_get(const char *name); /* NULL if unset or empty */
bool env_truthy(const char *name);
/* the platform the UI presents (macOS key glyphs, Cmd+click, the macOS-only settings and shortcuts): the build's,
 * unless OMC_PLATFORM=darwin|linux says otherwise (tests set linux: their fixtures are captures of omp on Linux) */
bool omc_platform_darwin(void);

#endif
