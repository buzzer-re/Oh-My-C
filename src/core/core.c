#define _GNU_SOURCE
#include "core.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/* ---------- allocation ---------- */

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "omc: fatal: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(70);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory (%zu bytes)", n);
    return q;
}

char *xstrdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *xvasprintf(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) die("vsnprintf failed");
    char *p = xmalloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    return p;
}

char *xasprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *p = xvasprintf(fmt, ap);
    va_end(ap);
    return p;
}

/* ---------- string builder ---------- */

void sb_reserve(sb_t *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
}

void sb_appendn(sb_t *b, const char *s, size_t n) {
    sb_reserve(b, n);
    if (n) memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_append(sb_t *b, const char *s) {
    if (s) sb_appendn(b, s, strlen(s));
}

void sb_putc(sb_t *b, char c) {
    sb_reserve(b, 1);
    b->s[b->len++] = c;
    b->s[b->len] = 0;
}

void sb_vappendf(sb_t *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n <= 0) return;
    sb_reserve(b, (size_t)n);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void sb_appendf(sb_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_vappendf(b, fmt, ap);
    va_end(ap);
}

void sb_clear(sb_t *b) {
    b->len = 0;
    if (b->s) b->s[0] = 0;
}

void sb_free(sb_t *b) {
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

char *sb_detach(sb_t *b) {
    char *s = b->s ? b->s : xstrdup("");
    b->s = NULL;
    b->len = b->cap = 0;
    return s;
}

const char *sb_cstr(sb_t *b) { return b->s ? b->s : ""; }

void sb_truncate(sb_t *b, size_t len) {
    if (len < b->len) {
        b->len = len;
        b->s[len] = 0;
    }
}

void sb_append_utf8(sb_t *b, uint32_t cp) {
    char buf[4];
    int n = utf8_encode(cp, buf);
    sb_appendn(b, buf, (size_t)n);
}

void sb_repeat(sb_t *b, const char *s, int n) {
    for (int i = 0; i < n; i++) sb_append(b, s);
}

/* ---------- strvec ---------- */

void strvec_push(strvec_t *v, const char *s) { vec_push(v, xstrdup(s)); }
void strvec_push_own(strvec_t *v, char *s) { vec_push(v, s); }

void strvec_free(strvec_t *v) {
    for (size_t i = 0; i < v->n; i++) free(v->v[i]);
    vec_free(v);
}

bool strvec_contains(const strvec_t *v, const char *s) {
    for (size_t i = 0; i < v->n; i++)
        if (strcmp(v->v[i], s) == 0) return true;
    return false;
}

char *strvec_join(const strvec_t *v, const char *sep) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < v->n; i++) {
        if (i) sb_append(&b, sep);
        sb_append(&b, v->v[i]);
    }
    return sb_detach(&b);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void strvec_sort(strvec_t *v) {
    if (v->n > 1) qsort(v->v, v->n, sizeof(char *), cmp_str);
}

strvec_t str_split(const char *s, const char *sep) {
    strvec_t out = {0};
    size_t sl = strlen(sep);
    if (!sl) {
        strvec_push(&out, s);
        return out;
    }
    const char *p = s;
    for (;;) {
        const char *q = strstr(p, sep);
        if (!q) {
            strvec_push(&out, p);
            break;
        }
        strvec_push_own(&out, xstrndup(p, (size_t)(q - p)));
        p = q + sl;
    }
    return out;
}

strvec_t str_split_lines(const char *s, size_t len) {
    strvec_t out = {0};
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s[i] == '\n') {
            size_t end = i;
            if (end > start && s[end - 1] == '\r') end--;
            if (i == len && start == len) break;
            strvec_push_own(&out, xstrndup(s + start, end - start));
            start = i + 1;
        }
    }
    return out;
}

/* ---------- string helpers ---------- */

bool str_starts(const char *s, const char *p) { return s && p && strncmp(s, p, strlen(p)) == 0; }

bool str_ends(const char *s, const char *suf) {
    if (!s || !suf) return false;
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && memcmp(s + a - b, suf, b) == 0;
}

bool str_eq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

bool str_ieq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcasecmp(a, b) == 0;
}

bool str_empty(const char *s) { return !s || !*s; }

char *str_trim(const char *s) {
    if (!s) return xstrdup("");
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

char *str_replace(const char *s, const char *from, const char *to) {
    sb_t b = SB_INIT;
    size_t fl = strlen(from);
    if (!fl) return xstrdup(s);
    const char *p = s;
    for (;;) {
        const char *q = strstr(p, from);
        if (!q) {
            sb_append(&b, p);
            break;
        }
        sb_appendn(&b, p, (size_t)(q - p));
        sb_append(&b, to);
        p = q + fl;
    }
    return sb_detach(&b);
}

char *str_lower(const char *s) {
    char *r = xstrdup(s);
    for (char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}

const char *str_istr(const char *hay, const char *needle) {
    if (!*needle) return hay;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, n) == 0) return p;
    return NULL;
}

size_t str_count(const char *s, const char *needle) {
    size_t c = 0, n = strlen(needle);
    if (!n) return 0;
    for (const char *p = strstr(s, needle); p; p = strstr(p + n, needle)) c++;
    return c;
}

char *str_indent(const char *s, const char *prefix) {
    sb_t b = SB_INIT;
    bool bol = true;
    for (const char *p = s; *p; p++) {
        if (bol && *p != '\n') sb_append(&b, prefix);
        sb_putc(&b, *p);
        bol = *p == '\n';
    }
    return sb_detach(&b);
}

int str_cmp_natural(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *sa = a, *sb2 = b;
            while (isdigit((unsigned char)*a)) a++;
            while (isdigit((unsigned char)*b)) b++;
            size_t la = (size_t)(a - sa), lb = (size_t)(b - sb2);
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(sa, sb2, la);
            if (c) return c;
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca - cb;
            a++;
            b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ---------- utf-8 ---------- */

int utf8_decode(const char *s, size_t len, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (!len) {
        *cp = 0;
        return 0;
    }
    unsigned c = u[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    int n;
    uint32_t v, min;
    if ((c & 0xE0) == 0xC0) { n = 2; v = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07; min = 0x10000; }
    else { *cp = 0xFFFD; return 1; }
    if ((size_t)n > len) { *cp = 0xFFFD; return 1; }
    for (int i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (u[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) { *cp = 0xFFFD; return 1; }
    *cp = v;
    return n;
}

int utf8_encode(uint32_t cp, char out[4]) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

bool utf8_valid(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        if (cp == 0xFFFD && n == 1 && (unsigned char)s[i] >= 0x80) {
            /* could legitimately be an encoded U+FFFD (3 bytes) - that path returns n=3 */
            return false;
        }
        i += (size_t)n;
    }
    return true;
}

char *utf8_sanitize(const char *s, size_t len) {
    sb_t b = SB_INIT;
    sb_reserve(&b, len);
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        if (n == 1 && (unsigned char)s[i] >= 0x80) sb_append(&b, "\xEF\xBF\xBD");
        else sb_appendn(&b, s + i, (size_t)n);
        i += (size_t)n;
    }
    return sb_detach(&b);
}

size_t utf8_strlen(const char *s) {
    size_t n = 0, len = strlen(s), i = 0;
    while (i < len) {
        uint32_t cp;
        i += (size_t)utf8_decode(s + i, len - i, &cp);
        n++;
    }
    return n;
}

struct range { uint32_t lo, hi; };

/* zero-width: combining marks, ZWJ, variation selectors, etc. */
static const struct range zero_width[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
    {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670},
    {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711},
    {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x0816, 0x082D}, {0x0900, 0x0902}, {0x093A, 0x093A},
    {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0957}, {0x0962, 0x0963},
    {0x0981, 0x0981}, {0x09BC, 0x09BC}, {0x09C1, 0x09C4}, {0x09CD, 0x09CD}, {0x0A01, 0x0A02},
    {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC},
    {0x0EC8, 0x0ECD}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E},
    {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF},
    {0x1F3FB, 0x1F3FF}, {0xE0001, 0xE007F}, {0xE0100, 0xE01EF},
};

static const struct range wide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0},
    {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5},
    {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728},
    {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
    {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
    {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F},
    {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4}, {0x17000, 0x18CFF}, {0x1B000, 0x1B2FF},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
    {0x1F200, 0x1F202}, {0x1F210, 0x1F23B}, {0x1F240, 0x1F248}, {0x1F250, 0x1F251},
    {0x1F260, 0x1F265}, {0x1F300, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C},
    {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A},
    {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF},
    {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0},
    {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

static bool in_ranges(uint32_t cp, const struct range *r, size_t n) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < r[mid].lo) hi = mid;
        else if (cp > r[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

int wcwidth_cp(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (cp < 0x300) return 1;
    if (in_ranges(cp, zero_width, ARRAY_LEN(zero_width))) return 0;
    if (in_ranges(cp, wide, ARRAY_LEN(wide))) return 2;
    return 1;
}

size_t str_display_width(const char *s) {
    size_t w = 0, len = strlen(s), i = 0;
    while (i < len) {
        if (s[i] == '\x1b') {
            i++;
            if (i < len && s[i] == '[') {
                i++;
                while (i < len && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7E)) i++;
                if (i < len) i++;
            } else if (i < len && (s[i] == ']' || s[i] == 'P' || s[i] == '_' || s[i] == '^' || s[i] == 'X')) {
                /* OSC: terminated by BEL or ST */
                while (i < len && s[i] != '\a' && !(s[i] == '\x1b' && i + 1 < len && s[i + 1] == '\\')) i++;
                if (i < len && s[i] == '\a') i++;
                else if (i < len) i += 2;
            } else if (i < len) {
                i++;
            }
            continue;
        }
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        if (cp == '\t') w += 3;
        else w += (size_t)wcwidth_cp(cp);
        i += (size_t)n;
    }
    return w;
}

/* ---------- hashing ---------- */

uint64_t hash_fnv1a(const void *data, size_t len) {
    const unsigned char *p = data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* xxHash64 (public domain algorithm by Yann Collet), implemented from spec */
#define XX_P1 0x9E3779B185EBCA87ULL
#define XX_P2 0xC2B2AE3D27D4EB4FULL
#define XX_P3 0x165667B19E3779F9ULL
#define XX_P4 0x85EBCA77C2B2AE63ULL
#define XX_P5 0x27D4EB2F165667C5ULL

static inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t rd64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t rd32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t xx_round(uint64_t acc, uint64_t in) {
    acc += in * XX_P2;
    acc = rotl64(acc, 31);
    return acc * XX_P1;
}
static inline uint64_t xx_merge(uint64_t acc, uint64_t v) {
    v = xx_round(0, v);
    acc ^= v;
    return acc * XX_P1 + XX_P4;
}

uint64_t hash_xx64(const void *data, size_t len, uint64_t seed) {
    const unsigned char *p = data, *end = p + len;
    uint64_t h;
    if (len >= 32) {
        const unsigned char *limit = end - 32;
        uint64_t v1 = seed + XX_P1 + XX_P2, v2 = seed + XX_P2, v3 = seed, v4 = seed - XX_P1;
        do {
            v1 = xx_round(v1, rd64(p)); p += 8;
            v2 = xx_round(v2, rd64(p)); p += 8;
            v3 = xx_round(v3, rd64(p)); p += 8;
            v4 = xx_round(v4, rd64(p)); p += 8;
        } while (p <= limit);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = xx_merge(h, v1);
        h = xx_merge(h, v2);
        h = xx_merge(h, v3);
        h = xx_merge(h, v4);
    } else {
        h = seed + XX_P5;
    }
    h += (uint64_t)len;
    while (p + 8 <= end) {
        h ^= xx_round(0, rd64(p));
        h = rotl64(h, 27) * XX_P1 + XX_P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)rd32(p) * XX_P1;
        h = rotl64(h, 23) * XX_P2 + XX_P3;
        p += 4;
    }
    while (p < end) {
        h ^= (*p) * XX_P5;
        h = rotl64(h, 11) * XX_P1;
        p++;
    }
    h ^= h >> 33;
    h *= XX_P2;
    h ^= h >> 29;
    h *= XX_P3;
    h ^= h >> 32;
    return h;
}

/* xxHash32 */
#define X32_P1 2654435761U
#define X32_P2 2246822519U
#define X32_P3 3266489917U
#define X32_P4 668265263U
#define X32_P5 374761393U
static inline uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

uint32_t hash_xx32(const void *data, size_t len, uint32_t seed) {
    const unsigned char *p = data, *end = p + len;
    uint32_t h;
    if (len >= 16) {
        const unsigned char *limit = end - 16;
        uint32_t v1 = seed + X32_P1 + X32_P2, v2 = seed + X32_P2, v3 = seed, v4 = seed - X32_P1;
        do {
            v1 = rotl32(v1 + rd32(p) * X32_P2, 13) * X32_P1; p += 4;
            v2 = rotl32(v2 + rd32(p) * X32_P2, 13) * X32_P1; p += 4;
            v3 = rotl32(v3 + rd32(p) * X32_P2, 13) * X32_P1; p += 4;
            v4 = rotl32(v4 + rd32(p) * X32_P2, 13) * X32_P1; p += 4;
        } while (p <= limit);
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + X32_P5;
    }
    h += (uint32_t)len;
    while (p + 4 <= end) {
        h += rd32(p) * X32_P3;
        h = rotl32(h, 17) * X32_P4;
        p += 4;
    }
    while (p < end) {
        h += (*p) * X32_P5;
        h = rotl32(h, 11) * X32_P1;
        p++;
    }
    h ^= h >> 15;
    h *= X32_P2;
    h ^= h >> 13;
    h *= X32_P3;
    h ^= h >> 16;
    return h;
}

/* ---------- hmap (open addressing, linear probing) ---------- */

void hmap_init(hmap_t *m) { memset(m, 0, sizeof(*m)); }

static size_t hmap_slot(const hmap_t *m, const char *key, uint64_t h) {
    size_t mask = m->cap - 1, i = (size_t)h & mask;
    while (m->e[i].key) {
        if (m->e[i].hash == h && strcmp(m->e[i].key, key) == 0) return i;
        i = (i + 1) & mask;
    }
    return i;
}

static void hmap_grow(hmap_t *m) {
    size_t ncap = m->cap ? m->cap * 2 : 16;
    hmap_entry_t *old = m->e;
    size_t ocap = m->cap;
    m->e = xcalloc(ncap, sizeof(hmap_entry_t));
    m->cap = ncap;
    for (size_t i = 0; i < ocap; i++) {
        if (!old[i].key) continue;
        size_t s = hmap_slot(m, old[i].key, old[i].hash);
        m->e[s] = old[i];
    }
    free(old);
}

void *hmap_get(const hmap_t *m, const char *key) {
    if (!m->cap) return NULL;
    uint64_t h = hash_fnv1a(key, strlen(key));
    size_t s = hmap_slot(m, key, h);
    return m->e[s].key ? m->e[s].val : NULL;
}

bool hmap_has(const hmap_t *m, const char *key) {
    if (!m->cap) return false;
    uint64_t h = hash_fnv1a(key, strlen(key));
    return m->e[hmap_slot(m, key, h)].key != NULL;
}

void *hmap_put(hmap_t *m, const char *key, void *val) {
    if ((m->n + 1) * 4 >= m->cap * 3) hmap_grow(m);
    uint64_t h = hash_fnv1a(key, strlen(key));
    size_t s = hmap_slot(m, key, h);
    if (m->e[s].key) {
        void *old = m->e[s].val;
        m->e[s].val = val;
        return old;
    }
    m->e[s].key = xstrdup(key);
    m->e[s].hash = h;
    m->e[s].val = val;
    m->n++;
    return NULL;
}

void *hmap_del(hmap_t *m, const char *key) {
    if (!m->cap) return NULL;
    uint64_t h = hash_fnv1a(key, strlen(key));
    size_t mask = m->cap - 1, i = hmap_slot(m, key, h);
    if (!m->e[i].key) return NULL;
    void *val = m->e[i].val;
    free(m->e[i].key);
    m->e[i].key = NULL;
    m->n--;
    /* backward-shift deletion to keep probe chains intact */
    size_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!m->e[j].key) break;
        size_t k = (size_t)m->e[j].hash & mask;
        if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
            m->e[i] = m->e[j];
            m->e[j].key = NULL;
            i = j;
        }
    }
    return val;
}

void hmap_free(hmap_t *m, void (*free_val)(void *)) {
    for (size_t i = 0; i < m->cap; i++) {
        if (m->e[i].key) {
            free(m->e[i].key);
            if (free_val) free_val(m->e[i].val);
        }
    }
    free(m->e);
    memset(m, 0, sizeof(*m));
}

/* ---------- time ---------- */

int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

char *iso8601_from_ms(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ms % 1000));
    return xstrdup(buf);
}

char *iso8601_now(void) { return iso8601_from_ms(now_ms()); }

void sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

/* ---------- random / ids ---------- */

void rand_bytes(void *buf, size_t n) {
    unsigned char *p = buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            die("getrandom failed: %s", strerror(errno));
        }
        p += r;
        n -= (size_t)r;
    }
}

static char *fmt_uuid(unsigned char b[16]) {
    char out[37];
    static const char hx[] = "0123456789abcdef";
    int j = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[j++] = '-';
        out[j++] = hx[b[i] >> 4];
        out[j++] = hx[b[i] & 15];
    }
    out[j] = 0;
    return xstrdup(out);
}

char *uuid_v4(void) {
    unsigned char b[16];
    rand_bytes(b, 16);
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    return fmt_uuid(b);
}

char *uuid_v7(void) {
    unsigned char b[16];
    rand_bytes(b, 16);
    uint64_t ms = (uint64_t)now_ms();
    for (int i = 0; i < 6; i++) b[i] = (unsigned char)(ms >> (40 - 8 * i));
    b[6] = (b[6] & 0x0F) | 0x70;
    b[8] = (b[8] & 0x3F) | 0x80;
    return fmt_uuid(b);
}

char *hex_encode(const void *data, size_t len) {
    static const char hx[] = "0123456789abcdef";
    const unsigned char *p = data;
    char *out = xmalloc(len * 2 + 1);
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = hx[p[i] >> 4];
        out[2 * i + 1] = hx[p[i] & 15];
    }
    out[len * 2] = 0;
    return out;
}

char *rand_hex(size_t nbytes) {
    unsigned char buf[64];
    if (nbytes > sizeof buf) nbytes = sizeof buf;
    rand_bytes(buf, nbytes);
    return hex_encode(buf, nbytes);
}

char *short_id(void) { return rand_hex(4); }

/* ---------- base64 ---------- */

static char *b64(const void *data, size_t len, const char *alpha, bool pad) {
    const unsigned char *p = data;
    sb_t b = SB_INIT;
    sb_reserve(&b, (len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        sb_putc(&b, alpha[v >> 18]);
        sb_putc(&b, alpha[(v >> 12) & 63]);
        sb_putc(&b, alpha[(v >> 6) & 63]);
        sb_putc(&b, alpha[v & 63]);
    }
    if (i < len) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < len) v |= (uint32_t)p[i + 1] << 8;
        sb_putc(&b, alpha[v >> 18]);
        sb_putc(&b, alpha[(v >> 12) & 63]);
        if (i + 1 < len) sb_putc(&b, alpha[(v >> 6) & 63]);
        else if (pad) sb_putc(&b, '=');
        if (pad) sb_putc(&b, '=');
    }
    return sb_detach(&b);
}

char *base64_encode(const void *data, size_t len) {
    return b64(data, len, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

char *base64url_encode(const void *data, size_t len) {
    return b64(data, len, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false);
}

unsigned char *base64_decode(const char *s, size_t len, size_t *out_len) {
    unsigned char *out = xmalloc(len / 4 * 3 + 4);
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int c = (unsigned char)s[i], v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else { free(out); return NULL; }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)(acc >> bits);
        }
    }
    *out_len = o;
    out[o] = 0;
    return out;
}

/* ---------- logging ---------- */

static FILE *g_log;
static log_level_t g_log_level = LOG_INFO;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

void log_init(const char *path, log_level_t level) {
    g_log_level = level;
    if (path) {
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd >= 0) g_log = fdopen(fd, "a");
    }
}

void log_msg(log_level_t lvl, const char *fmt, ...) {
    if (lvl < g_log_level || !g_log) return;
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char *ts = iso8601_now();
    pthread_mutex_lock(&g_log_mu);
    fprintf(g_log, "%s [%s] ", ts, names[lvl]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    pthread_mutex_unlock(&g_log_mu);
    free(ts);
}

/* ---------- env ---------- */

const char *env_get(const char *name) {
    const char *v = getenv(name);
    return (v && *v) ? v : NULL;
}

bool env_truthy(const char *name) {
    const char *v = env_get(name);
    if (!v) return false;
    return strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
           strcasecmp(v, "on") == 0;
}
