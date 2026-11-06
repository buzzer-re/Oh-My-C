/* find tool: semantic grep over the workspace driven by the judge role (port of omp tools/jfind: index.ts,
 * cascade.ts, tree.ts, passages.ts, lexical.ts, keywords.ts, questions.ts, text.ts).
 *
 * 1. Lexical scan ranks every eligible file by query keywords (one grep pass, IDF-weighted).
 * 2. Filename ranking judges the top candidates by name and place in the tree.
 * 3. Passage scoring reads the strongest files, cuts them into byte-bounded windows and judges a budgeted
 *    verbatim sketch of each window.
 * 4. Verification judges the complete text of the sketches that survived.
 * Each judged phase runs its requests through a bounded-parallel dispatcher and drains before the next. */
#include "tools.h"
#include "prompt/template.h"

#include "core/fs.h"
#include "text/search.h"
#include "text/walker.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIND_PARALLEL 16
#define FIND_NAME_BATCH 64
#define FIND_CANDIDATES 128
#define FIND_FILES 20
#define FIND_WINDOWS 24
#define FIND_WINDOW_BYTES 8192
#define FIND_SKETCH_BYTES 384
#define FIND_FULL_LIMIT 40
#define FIND_CUTOFF 0.45
#define FIND_THRESHOLD 0.2
#define FIND_READ_LIMIT (4 * 1024 * 1024)
#define FIND_SKETCH_STATE_BYTES 18000
#define FIND_SKETCH_CARDS_MAX 48
#define FIND_VERIFY_STATE_BYTES (24 * 1024)
#define FIND_FAILURES_KEPT 5
#define FIND_RANGES_SHOWN 3
#define FIND_TIMEOUT_DEFAULT_MS 20000
/* OMC_FIND_TIMEOUT_MS: test hook */
static double find_timeout_ms(void) {
    const char *v = env_get("OMC_FIND_TIMEOUT_MS");
    return v && atof(v) > 0 ? atof(v) : FIND_TIMEOUT_DEFAULT_MS;
}
#define FIND_TIMEOUT_MS find_timeout_ms()
#define FIND_BINARY_PROBE 8192

/* ---------- text primitives (jfind/text.ts) ---------- */

/* Rust `str::lines`: `\n`-separated, trailing `\r` stripped, no phantom line after a final newline */
static strvec_t text_lines(const char *s, size_t n) {
    strvec_t out = {0};
    if (!n) return out;
    size_t st = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '\n') {
            if (i == n && st == n) break;
            size_t e = i;
            if (e > st && s[e - 1] == '\r') e--;
            strvec_push_own(&out, xstrndup(s + st, e - st));
            st = i + 1;
        }
    }
    return out;
}

/* length of the longest prefix of s fitting in `bytes` without splitting a code point */
static size_t clip_bytes(const char *s, size_t bytes) {
    size_t n = strlen(s);
    if (n <= bytes) return n;
    size_t i = bytes;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

/* byte length of the first `count` code points */
static size_t take_chars(const char *s, size_t count) {
    size_t i = 0, taken = 0;
    while (s[i] && taken < count) {
        i++;
        while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
        taken++;
    }
    return i;
}

static size_t count_occ(const char *hay, const char *needle) {
    size_t nl = strlen(needle), c = 0;
    if (!nl) return 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += nl) c++;
    return c;
}

/* byte length of the JS whitespace character (String#trim set) starting at s, 0 when none */
static size_t ws_len(const char *s) {
    unsigned char c = (unsigned char)s[0];
    if (c == ' ' || (c >= 0x09 && c <= 0x0d)) return 1;
    if (c < 0x80) return 0;
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] == 0xC2 && u[1] == 0xA0) return 2;                                 /* U+00A0 */
    if (u[0] == 0xE1 && u[1] == 0x9A && u[2] == 0x80) return 3;                 /* U+1680 */
    if (u[0] == 0xE2 && u[1] == 0x80 && ((u[2] >= 0x80 && u[2] <= 0x8A) || u[2] == 0xA8 || u[2] == 0xA9 || u[2] == 0xAF))
        return 3;                                                               /* U+2000-200A, 2028, 2029, 202F */
    if (u[0] == 0xE2 && u[1] == 0x81 && u[2] == 0x9F) return 3;                 /* U+205F */
    if (u[0] == 0xE3 && u[1] == 0x80 && u[2] == 0x80) return 3;                 /* U+3000 */
    if (u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) return 3;                 /* U+FEFF */
    return 0;
}

/* length of the trailing JS whitespace run of s[0..n) */
static size_t ws_tail(const char *s, size_t n) {
    size_t t = 0;
    while (n > t) {
        size_t k = n - t, st = k - 1;
        while (st > 0 && ((unsigned char)s[st] & 0xC0) == 0x80) st--;
        size_t l = ws_len(s + st);
        if (!l || st + l != k) break;
        t += l;
    }
    return t;
}

static bool blank(const char *s) {
    for (size_t l; *s; s += l)
        if (!(l = ws_len(s))) return false;
    return true;
}

static char *trim_dup(const char *s) {
    for (size_t l; *s && (l = ws_len(s));) s += l;
    size_t n = strlen(s);
    n -= ws_tail(s, n);
    return xstrndup(s, n);
}

static void put_utf8(sb_t *b, uint32_t cp) {
    if (cp < 0x80) sb_putc(b, (char)cp);
    else if (cp < 0x800) {
        sb_putc(b, (char)(0xC0 | (cp >> 6)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(b, (char)(0xE0 | (cp >> 12)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(b, (char)(0xF0 | (cp >> 18)));
        sb_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* decode one code point (invalid bytes read as themselves) */
static uint32_t next_cp(const char *s, size_t *len) {
    const unsigned char *u = (const unsigned char *)s;
    size_t l = u[0] < 0x80 ? 1 : (u[0] >> 5) == 6 ? 2 : (u[0] >> 4) == 14 ? 3 : (u[0] >> 3) == 30 ? 4 : 1;
    for (size_t k = 1; k < l; k++)
        if ((u[k] & 0xC0) != 0x80) l = 1;
    uint32_t cp = l == 1 ? u[0] : l == 2 ? u[0] & 0x1F : l == 3 ? u[0] & 0x0F : u[0] & 0x07;
    for (size_t k = 1; k < l; k++) cp = (cp << 6) | (u[k] & 0x3F);
    *len = l;
    return cp;
}

/* String#toLowerCase for ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic capitals */
static char *js_lower(const char *s) {
    sb_t b = SB_INIT;
    for (size_t l; *s; s += l) {
        uint32_t c = next_cp(s, &l);
        if (l == 1 && c >= 0x80) { /* invalid byte: copy through */
            sb_putc(&b, *s);
            continue;
        }
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c >= 0xC0 && c <= 0xDE && c != 0xD7) c += 32;
        else if (c == 0x130) {
            sb_putc(&b, 'i');
            c = 0x307;
        } else if ((c >= 0x100 && c <= 0x137 && !(c & 1)) || (c >= 0x14A && c <= 0x177 && !(c & 1))) c += 1;
        else if ((c >= 0x139 && c <= 0x148 && (c & 1)) || (c >= 0x179 && c <= 0x17E && (c & 1))) c += 1;
        else if (c == 0x178) c = 0xFF;
        else if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) c += 32;
        else if (c == 0x386) c = 0x3AC;
        else if (c >= 0x388 && c <= 0x38A) c += 37;
        else if (c == 0x38C) c = 0x3CC;
        else if (c == 0x38E || c == 0x38F) c += 63;
        else if (c >= 0x410 && c <= 0x42F) c += 32;
        else if (c >= 0x400 && c <= 0x40F) c += 80;
        put_utf8(&b, c);
    }
    return sb_detach(&b);
}

/* \p{L}, \p{N} or '_' (approximation outside ASCII: everything but common punctuation/symbol/space blocks) */
static bool token_cp(uint32_t c) {
    if (c < 0x80) return isalnum((int)c) || c == '_';
    if (c <= 0xBF) return c == 0xAA || c == 0xB2 || c == 0xB3 || c == 0xB5 || c == 0xB9 || c == 0xBA || (c >= 0xBC && c <= 0xBE);
    if (c == 0xD7 || c == 0xF7) return false;
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x2190 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x3003) ||
        (c >= 0x3008 && c <= 0x3020) || c == 0xFEFF || (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
        (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65) || (c >= 0x1F000 && c <= 0x1FAFF) ||
        (c >= 0xE000 && c <= 0xF8FF))
        return false;
    return true;
}

/* JS Number#toFixed: round half away from zero on the exact decimal value of the double */
static char *js_fixed(double x, int digits) {
    char buf[512];
    snprintf(buf, sizeof buf, "%.60f", fabs(x));
    char *dot = strchr(buf, '.');
    size_t ip = (size_t)(dot - buf);
    /* digits kept: integer part + `digits` fraction digits */
    char d[512];
    size_t nd = 0;
    for (size_t i = 0; i < ip; i++) d[nd++] = buf[i];
    for (int i = 0; i < digits; i++) d[nd++] = dot[1 + i];
    bool up = dot[1 + digits] >= '5';
    if (up) {
        ssize_t k = (ssize_t)nd - 1;
        while (k >= 0 && d[k] == '9') d[k--] = '0';
        if (k >= 0) d[k]++;
        else {
            memmove(d + 1, d, nd);
            d[0] = '1';
            nd++;
            ip++;
        }
    }
    sb_t b = SB_INIT;
    bool neg = x < 0;
    bool zero = true;
    for (size_t i = 0; i < nd; i++)
        if (d[i] != '0') zero = false;
    if (neg && !zero) sb_putc(&b, '-');
    sb_appendn(&b, d, ip);
    if (digits > 0) {
        sb_putc(&b, '.');
        sb_appendn(&b, d + ip, (size_t)digits);
    }
    return sb_detach(&b);
}

/* pi-utils formatBytes */
static char *fmt_bytes(int64_t b) {
    if (b < 1024) return xasprintf("%lldB", (long long)b);
    double v;
    const char *u;
    if (b < 1024 * 1024) v = (double)b / 1024, u = "KB";
    else if (b < 1024LL * 1024 * 1024) v = (double)b / (1024 * 1024), u = "MB";
    else v = (double)b / (1024.0 * 1024 * 1024), u = "GB";
    char *f = js_fixed(v, 1);
    char *r = xasprintf("%s%s", f, u);
    free(f);
    return r;
}

static char *trim1(double n) {
    char *s = js_fixed(n, 1);
    size_t l = strlen(s);
    if (l > 2 && !strcmp(s + l - 2, ".0")) s[l - 2] = 0;
    return s;
}

/* pi-utils formatNumber (integers) */
static char *fmt_number(double n) {
    if (n < 1000) {
        if (n == floor(n)) return xasprintf("%.0f", n);
        return xasprintf("%g", n);
    }
    char *t;
    char *r;
    if (n < 10000) { t = trim1(n / 1000); r = xasprintf("%sK", t); free(t); return r; }
    if (n < 1000000) return xasprintf("%.0fK", round(n / 1000));
    if (n < 10000000) { t = trim1(n / 1000000); r = xasprintf("%sM", t); free(t); return r; }
    if (n < 1000000000) return xasprintf("%.0fM", round(n / 1000000));
    if (n < 10000000000.0) { t = trim1(n / 1000000000); r = xasprintf("%sB", t); free(t); return r; }
    return xasprintf("%.0fB", round(n / 1000000000));
}

/* pi-utils formatDuration */
static char *fmt_duration(double ms) {
    const double SEC = 1000, MIN = 60000, HOUR = 3600000, DAY = 86400000;
    if (!isfinite(ms) || ms <= 0) return xstrdup("0ms");
    if (ms < SEC) return xasprintf("%.0fms", floor(ms));
    if (ms < MIN) {
        double tenths = round(ms / 100);
        if (tenths >= 600) return xstrdup("1m");
        char *f = js_fixed(tenths / 10, 1);
        char *r = xasprintf("%ss", f);
        free(f);
        return r;
    }
    if (ms < HOUR) {
        long mins = (long)floor(ms / MIN), secs = (long)floor(fmod(ms, MIN) / SEC);
        return secs > 0 ? xasprintf("%ldm%lds", mins, secs) : xasprintf("%ldm", mins);
    }
    if (ms < DAY) {
        long h = (long)floor(ms / HOUR), mins = (long)floor(fmod(ms, HOUR) / MIN);
        return mins > 0 ? xasprintf("%ldh%ldm", h, mins) : xasprintf("%ldh", h);
    }
    long days = (long)floor(ms / DAY), h = (long)floor(fmod(ms, DAY) / HOUR);
    return h > 0 ? xasprintf("%ldd%ldh", days, h) : xasprintf("%ldd", days);
}

static double now_ms_f(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ---------- keywords (jfind/keywords.ts) ---------- */

static const char *const STOPWORDS[] = {
    "the", "a", "an", "or", "and", "to", "is", "are", "be", "when", "where", "how", "that", "this", "of", "in", "on",
    "at", "for", "with", "by", "its", "it", "as", "from", "into", "like", "gets", "get", "up", "which", "what", "does",
    "do", "code", "file", "files", "over", "all", "user", "using", "then", "than", "there", "their", "they", "them",
    "you", "your", "we", "our", "has", "have", "had", "was", "were", "been", "being", "will", "would", "should", "can",
    "could", "not", "but", "if", "so", "such", "via", "per", "any", "some", "each", "every", "also", "just", "only",
    "more", "most", "other", "out", "off", "about", "after", "before", "between", "through", "during", "without",
    "within", "one", "two", "new", "used", "use", "make", "makes", "made", "run", "runs", "way", "thing", "things",
    "something", "actually", "really", "still", "yet", NULL};

static bool stopword(const char *w) {
    for (size_t i = 0; STOPWORDS[i]; i++)
        if (!strcmp(STOPWORDS[i], w)) return true;
    return false;
}

static size_t utf16_units(const char *s, size_t n) {
    size_t u = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80) u += c >= 0xF0 ? 2 : 1;
    }
    return u;
}

/* cheap stem: spawned -> spawn, compacted -> compact (base must keep >= 4 UTF-16 units) */
static void stem(char *t) {
    static const char *const SUF[] = {"ing", "ed", "es", "s"};
    size_t n = strlen(t);
    for (size_t i = 0; i < 4; i++) {
        size_t l = strlen(SUF[i]);
        if (n >= l && !strcmp(t + n - l, SUF[i]) && utf16_units(t, n - l) >= 4) {
            t[n - l] = 0;
            return;
        }
    }
}

static void derive_keywords(const char *query, const json_t *extra, strvec_t *out) {
    sb_t rest = SB_INIT;
    for (size_t i = 0; query[i];) {
        char c = query[i];
        if (c != '"' && c != '\'') {
            sb_putc(&rest, c);
            i++;
            continue;
        }
        size_t j = i + 1;
        while (query[j] && query[j] != c) j++;
        bool closed = query[j] == c;
        char *raw = xstrndup(query + i + 1, j - i - 1);
        char *t = trim_dup(raw);
        char *phrase = js_lower(t);
        free(raw);
        free(t);
        if (closed && strlen(phrase) >= 3) {
            strvec_push(out, phrase);
            sb_putc(&rest, ' ');
        } else {
            sb_appendf(&rest, "%s ", phrase);
        }
        free(phrase);
        i = closed ? j + 1 : j;
    }
    const char *s = sb_cstr(&rest);
    for (size_t i = 0, l;;) {
        while (s[i] && !token_cp(next_cp(s + i, &l))) i += l;
        if (!s[i]) break;
        size_t st = i;
        while (s[i] && token_cp(next_cp(s + i, &l))) i += l;
        char *tok = xstrndup(s + st, i - st);
        char *lower = js_lower(tok);
        free(tok);
        if (strlen(lower) >= 3 && !stopword(lower) && strspn(lower, "0123456789") != strlen(lower)) {
            stem(lower);
            if (!strvec_contains(out, lower)) strvec_push(out, lower);
        }
        free(lower);
    }
    sb_free(&rest);
    json_t *x;
    json_arr_foreach(extra, k, x) {
        if (!json_is_str(x)) continue;
        char *t = trim_dup(json_sv(x));
        char *l = js_lower(t);
        free(t);
        if (*l && !strvec_contains(out, l)) strvec_push(out, l);
        free(l);
    }
}

/* ---------- file listing (jfind/tree.ts) ---------- */

typedef struct {
    char *path; /* absolute */
    char *rel;  /* root-relative ('/'), or the file root's own path */
    int64_t size;
} fentry_t;
typedef VEC(fentry_t) fentries_t;

static const char *const DENY_DIRS[] = {".git", "node_modules", "target", "dist", "build", "out", ".next", ".nuxt",
                                        ".turbo", ".cache", "__pycache__", ".venv", "venv", ".tox", "coverage", ".idea",
                                        ".vscode", ".gradle", ".mypy_cache", ".pytest_cache", ".ruff_cache",
                                        ".parcel-cache", NULL};
static const char *const DENY_FILES[] = {"Cargo.lock", "package-lock.json", "yarn.lock", "pnpm-lock.yaml", "bun.lock",
                                         "bun.lockb", "poetry.lock", "Pipfile.lock", "composer.lock", "Gemfile.lock",
                                         "go.sum", "flake.lock", ".DS_Store", "Thumbs.db", NULL};
static const char *const SECRET_FILES[] = {".env", ".envrc", ".netrc", ".npmrc", ".pypirc", ".pgpass", ".boto", ".s3cfg",
                                           ".dockercfg", ".git-credentials", ".htpasswd", "htpasswd", "credentials",
                                           "credentials.json", "client_secret.json", "service-account.json", "id_rsa",
                                           "id_dsa", "id_ecdsa", "id_ed25519", NULL};
static const char *const SECRET_EXT[] = {"pem", "key", "p12", "pfx", "jks", "keystore", "bks", "ppk", "kdbx", "gpg",
                                         "pgp", "asc", "der", "crt", "cer", "tfvars", "tfvars.json", "tfstate",
                                         "tfstate.backup", NULL};
static const char *const BINARY_EXT[] = {
    "png", "jpg", "jpeg", "gif", "webp", "avif", "ico", "bmp", "tiff", "psd", "svg", "woff", "woff2", "ttf", "otf",
    "eot", "zip", "gz", "tgz", "tar", "bz2", "xz", "zst", "7z", "rar", "pdf", "mp3", "mp4", "mov", "avi", "mkv", "wav",
    "ogg", "flac", "wasm", "so", "dylib", "dll", "exe", "o", "a", "class", "jar", "pyc", "pyo", "bin", "dat", "db",
    "sqlite", "sqlite3", "lock", "map", "min.js", "min.css", "snap", "pb", "onnx", "safetensors", "parquet", "arrow",
    "ipynb", NULL};
static const char *const ENV_TEMPLATES[] = {".env.example", ".env.sample", ".env.template", ".env.dist", NULL};

static bool in_list(const char *const *l, const char *s) {
    for (size_t i = 0; l[i]; i++)
        if (!strcmp(l[i], s)) return true;
    return false;
}

static bool has_ext(const char *lower, const char *const *exts) {
    size_t n = strlen(lower);
    for (size_t i = 0; exts[i]; i++) {
        size_t l = strlen(exts[i]);
        if (n > l && lower[n - l - 1] == '.' && !strcmp(lower + n - l, exts[i])) return true;
    }
    return false;
}

static bool secret_name(const char *name) {
    if (in_list(SECRET_FILES, name)) return true;
    if (str_starts(name, ".env.")) return !in_list(ENV_TEMPLATES, name);
    char *l = js_lower(name);
    bool r = has_ext(l, SECRET_EXT);
    free(l);
    return r;
}

static bool eligible_file(const char *rel, int64_t size, bool include_hidden) {
    if (size <= 0) return false;
    strvec_t segs = str_split(rel, "/");
    const char *name = segs.v[segs.n - 1];
    bool ok = true;
    for (size_t i = 0; i + 1 < segs.n && ok; i++)
        if (in_list(DENY_DIRS, segs.v[i]) || (!include_hidden && segs.v[i][0] == '.')) ok = false;
    if (ok && !include_hidden && name[0] == '.') ok = false;
    if (ok) {
        char *l = js_lower(name);
        ok = !in_list(DENY_FILES, name) && !secret_name(name) && !has_ext(l, BINARY_EXT);
        free(l);
    }
    strvec_free(&segs);
    return ok;
}

typedef struct {
    pthread_mutex_t mu;
    fentries_t *out;
    const char *root;
} listctx_t;

static bool list_cb(const walk_entry_t *e, void *ud) {
    listctx_t *c = ud;
    int64_t size = e->size;
    if (e->type == WALK_SYMLINK) { /* a link counts as the file it points to */
        struct stat st;
        if (stat(e->abs, &st) != 0 || !S_ISREG(st.st_mode)) return true;
        size = st.st_size;
    } else if (e->type != WALK_FILE) {
        return true;
    }
    if (!eligible_file(e->path, size, false)) return true;
    fentry_t f = {path_join(c->root, e->path), xstrdup(e->path), size};
    pthread_mutex_lock(&c->mu);
    vec_push(c->out, f);
    pthread_mutex_unlock(&c->mu);
    return true;
}

static int cmp_fentry(const void *a, const void *b) { return strcmp(((const fentry_t *)a)->rel, ((const fentry_t *)b)->rel); }

/* pi-tui humanSize: `812 B`, `3.4 KB` */
static char *human_size(int64_t bytes) {
    static const char *const U[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    if (!u) return xasprintf("%lld B", (long long)bytes);
    char *f = js_fixed(v, 1);
    char *r = xasprintf("%s %s", f, U[u]);
    free(f);
    return r;
}

/* path tree over the batch (pi-utils buildPathTree/walkPathTree), rendered with entry tags */
typedef struct tnode tnode_t;
struct tnode {
    strvec_t fnames;
    VEC(size_t) fidx;
    strvec_t dnames;
    VEC(tnode_t *) dirs;
};

static tnode_t *tnode_child(tnode_t *n, const char *name) {
    for (size_t i = 0; i < n->dirs.n; i++)
        if (!strcmp(n->dnames.v[i], name)) return n->dirs.v[i];
    tnode_t *c = xcalloc(1, sizeof *c);
    strvec_push(&n->dnames, name);
    vec_push(&n->dirs, c);
    return c;
}

static void tnode_free(tnode_t *n) {
    for (size_t i = 0; i < n->dirs.n; i++) tnode_free(n->dirs.v[i]);
    strvec_free(&n->fnames);
    vec_free(&n->fidx);
    strvec_free(&n->dnames);
    vec_free(&n->dirs);
    free(n);
}

static void entry_key(char *buf, size_t n, size_t i) { snprintf(buf, n, "e%03u", (unsigned)(i % 100000)); }
static void passage_key(char *buf, size_t n, size_t k) { snprintf(buf, n, "p%02u", (unsigned)(k % 100000)); }

static void tnode_render(const tnode_t *n, int depth, const fentry_t *const *entries, bool *emitted, sb_t *out) {
    for (size_t i = 0; i < n->fnames.n; i++) {
        if (*emitted && depth == 0) sb_putc(out, '\n');
        *emitted = true;
        for (int k = 0; k <= depth; k++) sb_putc(out, '#');
        char key[16];
        entry_key(key, sizeof key, n->fidx.v[i]);
        char *hs = human_size(entries[n->fidx.v[i]]->size);
        sb_appendf(out, " %s %s (%s)\n", key, n->fnames.v[i], hs);
        free(hs);
    }
    for (size_t i = 0; i < n->dirs.n; i++) {
        const tnode_t *d = n->dirs.v[i];
        sb_t name = SB_INIT;
        sb_append(&name, n->dnames.v[i]);
        while (d->fnames.n == 0 && d->dirs.n == 1) {
            sb_appendf(&name, "/%s", d->dnames.v[0]);
            d = d->dirs.v[0];
        }
        if (*emitted) sb_putc(out, '\n');
        *emitted = true;
        for (int k = 0; k <= depth; k++) sb_putc(out, '#');
        sb_appendf(out, " %s/\n", sb_cstr(&name));
        sb_free(&name);
        tnode_render(d, depth + 1, entries, emitted, out);
    }
}

static char *render_tree(const fentry_t *const *entries, size_t n) {
    tnode_t *root = xcalloc(1, sizeof *root);
    for (size_t i = 0; i < n; i++) {
        const char *rel = entries[i]->rel;
        if (strstr(rel, "://")) {
            strvec_push(&root->fnames, rel);
            vec_push(&root->fidx, i);
            continue;
        }
        strvec_t segs = str_split(rel, "/");
        tnode_t *t = root;
        for (size_t k = 0; k + 1 < segs.n; k++) t = tnode_child(t, segs.v[k]);
        bool dup = false;
        for (size_t k = 0; k < t->fnames.n && !dup; k++) dup = !strcmp(t->fnames.v[k], segs.v[segs.n - 1]);
        if (!dup) {
            strvec_push(&t->fnames, segs.v[segs.n - 1]);
            vec_push(&t->fidx, i);
        }
        strvec_free(&segs);
    }
    sb_t out = SB_INIT;
    bool emitted = false;
    tnode_render(root, 0, entries, &emitted, &out);
    tnode_free(root);
    return sb_detach(&out);
}

/* ---------- lexical prior (jfind/lexical.ts) ---------- */

typedef struct {
    strvec_t rels;
    VEC(double *) counts;
    int64_t files_scanned;
} gindex_t;

static char *escape_keyword(const char *k) {
    sb_t b = SB_INIT;
    for (; *k; k++) {
        if (strchr("\\.+*?()|[]{}^$#&-~", *k)) sb_putc(&b, '\\');
        sb_putc(&b, *k);
    }
    return sb_detach(&b);
}

static void grep_index(const char *root, const strvec_t *kw, gindex_t *ix) {
    memset(ix, 0, sizeof *ix);
    if (!kw->n) return;
    sb_t pat = SB_INIT;
    for (size_t k = 0; k < kw->n; k++) {
        char *e = escape_keyword(kw->v[k]);
        if (k) sb_putc(&pat, '|');
        sb_append(&pat, e);
        free(e);
    }
    search_opts_t so = {0};
    so.pattern = sb_cstr(&pat);
    so.path = root;
    so.ignore_case = true;
    so.hidden = false;
    search_result_t sr;
    if (search_run(&so, &sr) == 0) {
        for (size_t i = 0; i < sr.hits.n; i++) {
            const search_hit_t *h = &sr.hits.v[i];
            if (h->is_context || !h->text) continue;
            size_t f = ix->rels.n;
            if (f && !strcmp(ix->rels.v[f - 1], h->path)) f--;
            else {
                for (f = 0; f < ix->rels.n && strcmp(ix->rels.v[f], h->path); f++) {}
            }
            if (f == ix->rels.n) {
                strvec_push(&ix->rels, h->path);
                vec_push(&ix->counts, xcalloc(kw->n, sizeof(double)));
            }
            char *line = js_lower(h->text);
            for (size_t k = 0; k < kw->n; k++) ix->counts.v[f][k] += (double)count_occ(line, kw->v[k]);
            free(line);
        }
        ix->files_scanned = sr.files_searched + sr.skipped_oversized;
    }
    search_result_free(&sr);
    sb_free(&pat);
}

static const double *index_counts(const gindex_t *ix, const char *rel) {
    for (size_t i = 0; i < ix->rels.n; i++)
        if (!strcmp(ix->rels.v[i], rel)) return ix->counts.v[i];
    return NULL;
}

static void gindex_free(gindex_t *ix) {
    for (size_t i = 0; i < ix->counts.n; i++) free(ix->counts.v[i]);
    vec_free(&ix->counts);
    strvec_free(&ix->rels);
}

/* ---------- passages (jfind/passages.ts) ---------- */

typedef struct {
    int64_t start, end;
    char *text; /* `L<n>| line\n` tagged */
    double score;
} passage_t;
typedef VEC(passage_t) passages_t;

static void make_windows(const strvec_t *src, size_t bytes, const strvec_t *kw, const double *w, passages_t *out) {
    size_t start = 0;
    while (start < src->n) {
        size_t end = start, used = 0;
        sb_t content = SB_INIT;
        while (end < src->n) {
            const char *line = src->v[end];
            char prefix[32];
            int pl = snprintf(prefix, sizeof prefix, "L%zu| ", end + 1);
            size_t overhead = (size_t)pl + 1;
            if (end > start && used + strlen(line) + overhead > bytes) break;
            size_t room = bytes > used + overhead ? bytes - (used + overhead) : 0;
            size_t keep = clip_bytes(line, room);
            size_t before = content.len;
            sb_append(&content, prefix);
            sb_appendn(&content, line, keep);
            sb_putc(&content, '\n');
            used += content.len - before;
            end++;
            if (used >= bytes) break;
        }
        char *lower = js_lower(sb_cstr(&content));
        double score = 0;
        for (size_t k = 0; k < kw->n; k++) score += w[k] * log1p((double)count_occ(lower, kw->v[k]));
        free(lower);
        passage_t p = {(int64_t)start + 1, (int64_t)end, sb_detach(&content), score};
        vec_push(out, p);
        start = end;
    }
}

static int cmp_pass_score(const void *a, const void *b) {
    const passage_t *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? 1 : -1;
    return x->start < y->start ? -1 : x->start > y->start;
}

static int cmp_pass_start(const void *a, const void *b) {
    const passage_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static void select_windows(passages_t *ps, size_t limit) {
    if (ps->n > limit) {
        bool all_zero = true;
        for (size_t i = 0; i < ps->n; i++)
            if (ps->v[i].score != 0) all_zero = false;
        bool *keep = xcalloc(ps->n, sizeof(bool));
        if (all_zero) {
            size_t len = ps->n, step = limit > 2 ? limit - 1 : 1;
            for (size_t k = 0; k < limit; k++) keep[(k * (len - 1)) / step] = true;
        } else {
            passages_t tmp = {0};
            for (size_t i = 0; i < ps->n; i++) vec_push(&tmp, ps->v[i]);
            qsort(tmp.v, tmp.n, sizeof(passage_t), cmp_pass_score);
            for (size_t i = 0; i < limit; i++)
                for (size_t j = 0; j < ps->n; j++)
                    if (ps->v[j].start == tmp.v[i].start) keep[j] = true;
            vec_free(&tmp);
        }
        size_t w = 0;
        for (size_t i = 0; i < ps->n; i++) {
            if (keep[i]) ps->v[w++] = ps->v[i];
            else free(ps->v[i].text);
        }
        ps->n = w;
        free(keep);
    }
    if (ps->n > 1) qsort(ps->v, ps->n, sizeof(passage_t), cmp_pass_start);
}

/* passage text with the `L<n>| ` tags stripped */
static char *plain_content(const passage_t *p) {
    strvec_t tagged = text_lines(p->text, strlen(p->text));
    sb_t out = SB_INIT;
    for (size_t i = 0; i < tagged.n; i++) {
        char prefix[32];
        snprintf(prefix, sizeof prefix, "L%lld| ", (long long)(p->start + (int64_t)i));
        const char *line = tagged.v[i];
        sb_append(&out, str_starts(line, prefix) ? line + strlen(prefix) : line);
        sb_putc(&out, '\n');
    }
    strvec_free(&tagged);
    return sb_detach(&out);
}

typedef struct {
    size_t index;
    double score;
} rline_t;

static int cmp_rline(const void *a, const void *b) {
    const rline_t *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? 1 : -1;
    return x->index < y->index ? -1 : x->index > y->index;
}

static int cmp_rline_index(const void *a, const void *b) {
    const rline_t *x = a, *y = b;
    return x->index < y->index ? -1 : x->index > y->index;
}

/* a budgeted map of verbatim source lines ranked by keyword weight, emitted in file order */
static char *make_sketch(const passage_t *p, const strvec_t *kw, const double *w, size_t budget) {
    char *pc = plain_content(p);
    strvec_t plain = text_lines(pc, strlen(pc));
    free(pc);
    VEC(rline_t) ranked = {0};
    for (size_t i = 0; i < plain.n; i++) {
        const char *line = plain.v[i];
        if (blank(line)) continue;
        char *lower = js_lower(line);
        double score = strchr(line, '(') ? 0.1 : 0;
        for (size_t k = 0; k < kw->n; k++)
            if (strstr(lower, kw->v[k])) score += w[k];
        free(lower);
        rline_t r = {i, score};
        vec_push(&ranked, r);
    }
    if (ranked.n > 1) qsort(ranked.v, ranked.n, sizeof(rline_t), cmp_rline);
    VEC(rline_t) sel = {0};
    strvec_t texts = {0};
    size_t used = 0;
    for (size_t i = 0; i < ranked.n; i++) {
        size_t avail = budget > used + 12 ? budget - (used + 12) : 0;
        if (avail < 24) break;
        char *line = trim_dup(plain.v[ranked.v[i].index]);
        size_t keep = clip_bytes(line, MIN(avail, (size_t)180));
        char *text = xasprintf("%lld: %.*s", (long long)(p->start + (int64_t)ranked.v[i].index), (int)keep, line);
        free(line);
        used += strlen(text) + 1;
        rline_t r = {ranked.v[i].index, (double)texts.n};
        vec_push(&sel, r);
        strvec_push_own(&texts, text);
    }
    if (sel.n > 1) qsort(sel.v, sel.n, sizeof(rline_t), cmp_rline_index);
    sb_t out = SB_INIT;
    for (size_t i = 0; i < sel.n; i++) {
        if (i) sb_putc(&out, '\n');
        sb_append(&out, texts.v[(size_t)sel.v[i].score]);
    }
    vec_free(&ranked);
    vec_free(&sel);
    strvec_free(&texts);
    strvec_free(&plain);
    return sb_detach(&out);
}

typedef struct {
    int64_t start, end;
    double p;
    char *snippet;
} heat_t;
typedef VEC(heat_t) heats_t;

static int cmp_heat_pos(const void *a, const void *b) {
    const heat_t *x = a, *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->end < y->end ? -1 : x->end > y->end;
}

static int cmp_heat_rank(const void *a, const void *b) {
    const heat_t *x = a, *y = b;
    if (x->p != y->p) return x->p < y->p ? 1 : -1;
    return x->start < y->start ? -1 : x->start > y->start;
}

/* union judged-positive spans (never bridging an unjudged gap), strongest first */
static void merge_heat(const heats_t *in, double threshold, heats_t *out) {
    heats_t kept = {0};
    for (size_t i = 0; i < in->n; i++)
        if (in->v[i].p >= threshold && in->v[i].p > 0 && in->v[i].start <= in->v[i].end) vec_push(&kept, in->v[i]);
    if (kept.n > 1) qsort(kept.v, kept.n, sizeof(heat_t), cmp_heat_pos);
    for (size_t i = 0; i < kept.n; i++) {
        if (out->n && kept.v[i].start <= out->v[out->n - 1].end + 1) {
            heat_t *last = &out->v[out->n - 1];
            if (kept.v[i].end > last->end) last->end = kept.v[i].end;
            if (kept.v[i].p > last->p) last->p = kept.v[i].p;
            continue;
        }
        heat_t h = kept.v[i];
        h.snippet = xstrdup(h.snippet);
        vec_push(out, h);
    }
    vec_free(&kept);
    if (out->n > 1) qsort(out->v, out->n, sizeof(heat_t), cmp_heat_rank);
}

/* ---------- judge requests (jfind/questions.ts, prompts/judge/find.json) ---------- */

static const char *const TASK =
    "Semantic grep over a source tree: locate files whose content matches the search description. Entries are judged by "
    "name, size, position in the tree, and (for folders) a sample of what they contain.";
static const char *const TREE_FORMAT =
    "`tree` is a directory listing. Lines starting with # are headers: `# dir/` is a folder; more #s means deeper nesting "
    "under the header above; a header may fold several levels (`# a/b/c/`). Every judgeable entry carries a tag like e017 "
    "right after the #s: files as `e017 name (size)`, folders as `e017 name/ \xe2\x80\x94 N entries: sample of names`. "
    "Untagged header lines are only structure.";
static const char *const Q_NAME = "Is the file tagged {{key}} (\"{{name}}\") likely to contain what this search is looking "
                                  "for: \"{{query}}\"? Judge by its name, size, and place in `tree`; apply `criteria.file`.";
static const char *const Q_SKETCH = "Could passage {{key}} implement a requested step of search? Apply criteria.";
static const char *const Q_PASSAGE =
    "Does `passages.{{key}}` substantively implement, define, or explain part of \"{{query}}\"? Apply `criteria`.";

static json_t *criteria(const char *no, const char *yes) {
    json_t *c = json_obj();
    json_set_str(c, "no", no);
    json_set_str(c, "yes", yes);
    return c;
}

static json_t *question(const char *tpl, const char *key, const char *name, const char *query) {
    json_t *ctx = json_obj();
    json_set_str(ctx, "key", key);
    if (name) json_set_str(ctx, "name", name);
    if (query) json_set_str(ctx, "query", query);
    json_t *q = json_obj();
    json_set_str(q, "type", "bool");
    json_set(q, "instructions", json_str_own(tpl_render(tpl, ctx)));
    json_free(ctx);
    return q;
}

/* ---------- cascade ---------- */

typedef struct {
    json_t *state, *questions; /* owned */
    json_t *answers;           /* outcome (owned) or NULL */
    char *error;               /* outcome error (owned) */
    double ms;
    size_t a, b;               /* job payload (batch offset / file index) */
    VEC(size_t) items;
} job_t;
typedef VEC(job_t) jobs_t;

typedef struct {
    tool_env_t *env;
    jobs_t *jobs;
    size_t next;
    pthread_mutex_t mu;
    volatile sig_atomic_t *stop;
    double usage[3];
} dispatch_t;

static void *dispatch_worker(void *arg) {
    dispatch_t *d = arg;
    for (;;) {
        if (*d->stop) break;
        pthread_mutex_lock(&d->mu);
        size_t i = d->next++;
        pthread_mutex_unlock(&d->mu);
        if (i >= d->jobs->n) break;
        job_t *j = &d->jobs->v[i];
        double t0 = now_ms_f();
        double u[3] = {0, 0, 0};
        j->answers = d->env->judge(d->env, j->state, j->questions, d->stop, u, &j->error);
        j->ms = now_ms_f() - t0;
        /* a judge the abort ended reaches nothing: the call is over */
        if (!j->answers && *d->stop) {
            free(j->error);
            j->error = NULL;
            continue;
        }
        if (!j->answers && !j->error) j->error = xstrdup("judgment failed");
        pthread_mutex_lock(&d->mu);
        for (int k = 0; k < 3; k++) d->usage[k] += u[k];
        pthread_mutex_unlock(&d->mu);
    }
    return NULL;
}

typedef struct {
    int64_t listed, requests, failed, unanswered, judged, files_read, file_bytes, windows_judged, windows_pruned, map_cards;
    double input_tokens, output_tokens, cost, api_ms;
    strvec_t failures;
} fstats_t;

/* run every job with at most FIND_PARALLEL in flight; outcomes are settled afterwards in job order */
static void dispatch(tool_env_t *env, jobs_t *jobs, volatile sig_atomic_t *stop, fstats_t *st) {
    if (!jobs->n) return;
    dispatch_t d = {.env = env, .jobs = jobs, .stop = stop};
    pthread_mutex_init(&d.mu, NULL);
    size_t nt = MIN((size_t)FIND_PARALLEL, jobs->n);
    pthread_t th[FIND_PARALLEL];
    bool started[FIND_PARALLEL] = {0};
    for (size_t i = 1; i < nt; i++) started[i] = pthread_create(&th[i], NULL, dispatch_worker, &d) == 0;
    dispatch_worker(&d);
    for (size_t i = 1; i < nt; i++)
        if (started[i]) pthread_join(th[i], NULL);
    pthread_mutex_destroy(&d.mu);
    for (size_t i = 0; i < jobs->n; i++) {
        if (!jobs->v[i].answers && !jobs->v[i].error) continue; /* never ran (stopped) */
        st->requests++;
        st->api_ms += jobs->v[i].ms;
        if (!jobs->v[i].answers) st->failed++;
    }
    st->input_tokens += d.usage[0];
    st->output_tokens += d.usage[1];
    st->cost += d.usage[2];
}

static void stat_fail(fstats_t *st, const char *phase, const char *err) {
    char *m = xasprintf("%s: %s", phase, err ? err : "unknown error");
    if (st->failures.n < FIND_FAILURES_KEPT && !strvec_contains(&st->failures, m)) strvec_push(&st->failures, m);
    free(m);
}

/* a finite probability for `key`, or NAN when the judge produced nothing usable */
static double noul(const job_t *j, const char *key) {
    if (!j->answers) return NAN;
    const json_t *a = json_get(j->answers, key);
    const json_t *v = json_get(a, "bool");
    if (!v) v = json_get(a, "noul");
    if (!json_is_num(v)) return NAN;
    double p = v->u.num.d;
    return isfinite(p) && p >= 0 && p <= 1 ? p : NAN;
}

static void jobs_free(jobs_t *jobs) {
    for (size_t i = 0; i < jobs->n; i++) {
        json_free(jobs->v[i].state);
        json_free(jobs->v[i].questions);
        json_free(jobs->v[i].answers);
        free(jobs->v[i].error);
        vec_free(&jobs->v[i].items);
    }
    vec_free(jobs);
}

typedef struct {
    size_t node;
    double lex;
} rank_t;

typedef struct {
    size_t node;
    int64_t total;
    bool truncated;
    passages_t passages;
} plan_t;

typedef struct {
    size_t f, p;
    double score;
} cand_t;

typedef struct {
    size_t f;
    double score;
    heats_t heat;
    int64_t lines, bytes;
} fresult_t;

typedef struct {
    tool_env_t *env;
    volatile sig_atomic_t *user_cancel;
    volatile sig_atomic_t stop;
    bool timed_out, aborted, done;
    double t0;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} watch_t;

static void *watchdog(void *arg) {
    watch_t *w = arg;
    pthread_mutex_lock(&w->mu);
    while (!w->done) {
        if (w->user_cancel && *w->user_cancel) {
            w->aborted = true;
            w->stop = 1;
            break;
        }
        if (now_ms_f() - w->t0 >= FIND_TIMEOUT_MS) {
            w->timed_out = true;
            w->stop = 1;
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&w->cv, &w->mu, &ts);
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

static const fentry_t *G_cmp_entries; /* qsort context for rank ordering */
static int cmp_rank_lex(const void *a, const void *b) {
    const rank_t *x = a, *y = b;
    if (x->lex != y->lex) return x->lex < y->lex ? 1 : -1;
    return strcmp(G_cmp_entries[x->node].rel, G_cmp_entries[y->node].rel);
}

static int find_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    char *query = trim_dup(json_get_str(args, "query") ? json_get_str(args, "query") : "");
    if (!*query) {
        free(query);
        tool_result_error(out, "`query` must be a non-empty description");
        return 0;
    }
    /* resolveSearchRoot */
    const char *pin = json_get_str(args, "path");
    char *input = NULL;
    if (pin) {
        char *t = trim_dup(pin);
        size_t n = strlen(t);
        if (n > 1 && t[0] == '"' && t[n - 1] == '"') {
            input = xstrndup(t + 1, n - 2);
            free(t);
        } else {
            input = t;
        }
    } else {
        input = xstrdup("");
    }
    char *root = NULL;
    bool root_is_dir = true;
    int64_t root_size = 0;
    if (!*input) {
        root = xstrdup(env->cwd);
    } else {
        root = tool_resolve_path(env, input);
        struct stat st;
        if (stat(root, &st) != 0) {
            tool_result_error(out, "Path not found: %s", input);
            goto early;
        }
        if (S_ISDIR(st.st_mode)) root_is_dir = true;
        else if (S_ISREG(st.st_mode)) {
            root_is_dir = false;
            root_size = st.st_size;
        } else {
            tool_result_error(out, "Path is neither a file nor a directory: %s", input);
            goto early;
        }
    }
    if (!env->judge) {
        tool_result_error(out, "find has no model registry to resolve a judge from");
        goto early;
    }
    char *scope = NULL;
    if (strcmp(root, env->cwd) != 0) {
        char *rel = path_relative(env->cwd, root);
        if (str_starts(rel, "..")) {
            free(rel);
            rel = xstrdup(root);
        }
        scope = root_is_dir && strcmp(rel, ".") && !str_ends(rel, "/") ? xasprintf("%s/", rel) : xstrdup(rel);
        free(rel);
    }

    watch_t w = {.env = env, .user_cancel = ctx ? ctx->cancel : NULL, .t0 = now_ms_f()};
    pthread_mutex_init(&w.mu, NULL);
    pthread_cond_init(&w.cv, NULL);
    pthread_t wt;
    bool wt_ok = pthread_create(&wt, NULL, watchdog, &w) == 0;
    double started = w.t0;
    char *phase = xstrdup("lexical scan"); /* the phase the deadline interrupts, named when nothing was verified before it */

    fstats_t st;
    memset(&st, 0, sizeof st);
    VEC(plan_t) files = {0};
    VEC(cand_t) cards = {0};
    VEC(cand_t) cands = {0};
    strvec_t kw = {0};
    derive_keywords(query, json_get_arr(args, "grep_keywords"), &kw);

    /* listing + lexical index */
    fentries_t entries = {0};
    if (!root_is_dir) {
        if (eligible_file(path_basename(root), root_size, true)) {
            fentry_t f = {xstrdup(root), xstrdup(root), root_size};
            vec_push(&entries, f);
        }
    } else {
        listctx_t lc = {.out = &entries, .root = root};
        pthread_mutex_init(&lc.mu, NULL);
        walk_opts_t wo = {0};
        wo.include_hidden = false;
        wo.skip_node_modules = true;
        wo.types = WALK_FILE | WALK_SYMLINK;
        wo.want_stat = true;
        walk(root, &wo, list_cb, &lc);
        pthread_mutex_destroy(&lc.mu);
        if (entries.n > 1) qsort(entries.v, entries.n, sizeof(fentry_t), cmp_fentry);
    }
    gindex_t ix;
    grep_index(root, &kw, &ix);
    st.listed = (int64_t)entries.n;
    double *weights = xcalloc(kw.n + 1, sizeof(double));
    for (size_t k = 0; k < kw.n; k++) {
        int64_t df = 0;
        for (size_t i = 0; i < ix.counts.n; i++)
            if (ix.counts.v[i][k] > 0) df++;
        double wgt = log((double)(ix.files_scanned + 1) / (double)(df + 1));
        weights[k] = MIN(6.0, MAX(0.5, wgt));
    }
    VEC(rank_t) ranked = {0};
    for (size_t i = 0; i < entries.n; i++) {
        const double *c = index_counts(&ix, entries.v[i].rel);
        char *lower = js_lower(entries.v[i].rel);
        double score = 0;
        for (size_t k = 0; k < kw.n; k++) {
            double in_path = strstr(lower, kw.v[k]) ? 1 : 0;
            score += weights[k] * (2 * in_path + log1p(c ? c[k] : 0));
        }
        free(lower);
        rank_t r = {i, score};
        vec_push(&ranked, r);
    }
    G_cmp_entries = entries.v;
    if (ranked.n > 1) qsort(ranked.v, ranked.n, sizeof(rank_t), cmp_rank_lex);
    if (ranked.n > FIND_CANDIDATES) ranked.n = FIND_CANDIDATES;
    double *name_score = xmalloc((entries.n + 1) * sizeof(double));
    for (size_t i = 0; i <= entries.n; i++) name_score[i] = NAN;

    /* wave 1: filename ranking over the lexical shortlist */
    const char *project = path_basename(root);
    jobs_t jobs = {0};
    for (size_t s = 0; s < ranked.n; s += FIND_NAME_BATCH) {
        job_t j = {0};
        size_t e = MIN(ranked.n, s + FIND_NAME_BATCH);
        const fentry_t **batch = xmalloc((e - s) * sizeof(fentry_t *));
        j.questions = json_obj();
        for (size_t i = s; i < e; i++) {
            vec_push(&j.items, ranked.v[i].node);
            batch[i - s] = &entries.v[ranked.v[i].node];
            char key[16];
            entry_key(key, sizeof key, i - s);
            json_set(j.questions, key, question(Q_NAME, key, path_basename(batch[i - s]->rel), query));
        }
        j.state = json_obj();
        json_t *crit = json_obj();
        json_set(crit, "file", criteria("The file is unrelated by name and location; generated executable implementation can "
                                        "still be relevant.",
                                        "A file at this path plausibly contains code, text, or data matching the search."));
        json_set(crit, "folder", criteria("Nothing about the folder's name, location, or sampled contents suggests it holds a "
                                          "match.",
                                          "The folder plausibly contains, at any depth, at least one file matching the search."));
        json_set(j.state, "criteria", crit);
        json_set_str(j.state, "format", TREE_FORMAT);
        json_set_str(j.state, "project", project);
        json_set_str(j.state, "search", query);
        json_set_str(j.state, "task", TASK);
        json_set(j.state, "tree", json_str_own(render_tree((const fentry_t *const *)batch, e - s)));
        free(batch);
        vec_push(&jobs, j);
    }
    free(phase);
    phase = xasprintf("filename ranking 0/%zu", ranked.n);
    dispatch(env, &jobs, &w.stop, &st);
    if (w.stop) {
        size_t named = 0;
        for (size_t i = 0; i < jobs.n; i++)
            if (jobs.v[i].answers || jobs.v[i].error) named += jobs.v[i].items.n;
        free(phase);
        phase = xasprintf("filename ranking %zu/%zu", named, ranked.n);
        goto stopped;
    }
    for (size_t i = 0; i < jobs.n; i++) {
        job_t *j = &jobs.v[i];
        if (!j->answers) stat_fail(&st, "filenames", j->error);
        for (size_t k = 0; k < j->items.n; k++) {
            char key[16];
            entry_key(key, sizeof key, k);
            double p = noul(j, key);
            name_score[j->items.v[k]] = p;
            if (isnan(p)) {
                if (j->answers) st.unanswered++;
            } else {
                st.judged++;
            }
        }
    }
    jobs_free(&jobs);

    /* the two strongest lexical candidates are read regardless; then name score, then lexical rank (stable) */
    VEC(size_t) selected = {0};
    for (size_t i = 0; i < ranked.n && i < MIN((size_t)FIND_FILES, (size_t)2); i++) vec_push(&selected, ranked.v[i].node);
    for (size_t i = 1; i < ranked.n; i++) { /* stable insertion sort */
        rank_t x = ranked.v[i];
        double xs = isnan(name_score[x.node]) ? 0 : name_score[x.node];
        size_t k = i;
        while (k > 0) {
            rank_t y = ranked.v[k - 1];
            double ys = isnan(name_score[y.node]) ? 0 : name_score[y.node];
            double d = (xs - ys) != 0 ? xs - ys : x.lex - y.lex;
            if (d <= 0) break;
            ranked.v[k] = y;
            k--;
        }
        ranked.v[k] = x;
    }
    for (size_t i = 0; i < ranked.n && selected.n < FIND_FILES; i++) {
        bool dup = false;
        for (size_t k = 0; k < selected.n && !dup; k++) dup = selected.v[k] == ranked.v[i].node;
        if (!dup) vec_push(&selected, ranked.v[i].node);
    }
    free(phase);
    phase = xasprintf("reading %zu files", selected.n);
    for (size_t s = 0; s < selected.n; s++) {
        const fentry_t *e = &entries.v[selected.v[s]];
        int fd = open(e->path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            char *ph = xasprintf("read %s", e->rel);
            stat_fail(&st, ph, strerror(errno));
            free(ph);
            continue;
        }
        size_t cap = FIND_READ_LIMIT + 1, n = 0;
        char *buf = xmalloc(cap + 1);
        for (;;) {
            ssize_t r = read(fd, buf + n, cap - n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            n += (size_t)r;
            if (n >= cap) break;
        }
        close(fd);
        if (memchr(buf, 0, MIN(n, (size_t)FIND_BINARY_PROBE))) {
            free(buf);
            continue;
        }
        bool truncated = n > FIND_READ_LIMIT;
        if (truncated) {
            n = FIND_READ_LIMIT;
            char *nl = NULL;
            for (size_t i = n; i > 0; i--)
                if (buf[i - 1] == '\n') {
                    nl = buf + i - 1;
                    break;
                }
            if (nl) n = (size_t)(nl - buf) + 1;
        }
        const char *data = buf;
        size_t dlen = n;
        if (dlen >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
            data += 3;
            dlen -= 3;
        }
        char *text = utf8_valid(data, dlen) ? xstrndup(data, dlen) : utf8_sanitize(data, dlen);
        free(buf);
        strvec_t ls = text_lines(text, strlen(text));
        free(text);
        bool all_blank = true;
        for (size_t i = 0; i < ls.n && all_blank; i++) all_blank = blank(ls.v[i]);
        if (all_blank) {
            strvec_free(&ls);
            continue;
        }
        plan_t pl = {selected.v[s], (int64_t)ls.n, truncated, {0}};
        make_windows(&ls, FIND_WINDOW_BYTES, &kw, weights, &pl.passages);
        strvec_free(&ls);
        select_windows(&pl.passages, FIND_WINDOWS);
        if (!pl.passages.n) {
            vec_free(&pl.passages);
            continue;
        }
        vec_push(&files, pl);
    }
    vec_free(&selected);

    /* wave 2: sketch routing over mixed-file cards */
    for (size_t f = 0; f < files.n; f++)
        for (size_t p = 0; p < files.v[f].passages.n; p++) {
            cand_t c = {f, p, 0};
            vec_push(&cards, c);
        }
    int64_t sketch_bytes = 0;
    size_t per = MIN((size_t)FIND_SKETCH_CARDS_MAX, MAX((size_t)1, (size_t)(FIND_SKETCH_STATE_BYTES / FIND_SKETCH_BYTES)));
    for (size_t s = 0; s < cards.n; s += per) {
        job_t j = {0};
        size_t e = MIN(cards.n, s + per);
        j.a = s;
        j.b = e;
        j.questions = json_obj();
        json_t *fmap = json_obj(), *pmap = json_obj();
        strvec_t fkeys = {0};
        strvec_t frels = {0};
        for (size_t i = s; i < e; i++) {
            const plan_t *pl = &files.v[cards.v[i].f];
            char *sk = make_sketch(&pl->passages.v[cards.v[i].p], &kw, weights, FIND_SKETCH_BYTES);
            sketch_bytes += (int64_t)strlen(sk);
            char fkey[24], key[16];
            snprintf(fkey, sizeof fkey, "f%zu", cards.v[i].f);
            passage_key(key, sizeof key, i - s);
            if (!strvec_contains(&fkeys, fkey)) {
                strvec_push(&fkeys, fkey);
                strvec_push(&frels, entries.v[pl->node].rel);
            }
            json_t *pair = json_arr();
            json_push_str(pair, fkey);
            json_push(pair, json_str_own(sk));
            json_set(pmap, key, pair);
            json_set(j.questions, key, question(Q_SKETCH, key, NULL, NULL));
        }
        /* files: keys in lexicographic order */
        size_t *ord = xmalloc((fkeys.n + 1) * sizeof(size_t));
        for (size_t i = 0; i < fkeys.n; i++) ord[i] = i;
        for (size_t i = 1; i < fkeys.n; i++)
            for (size_t k = i; k > 0 && strcmp(fkeys.v[ord[k - 1]], fkeys.v[ord[k]]) > 0; k--) {
                size_t t = ord[k];
                ord[k] = ord[k - 1];
                ord[k - 1] = t;
            }
        for (size_t i = 0; i < fkeys.n; i++) json_set_str(fmap, fkeys.v[ord[i]], frels.v[ord[i]]);
        free(ord);
        strvec_free(&fkeys);
        strvec_free(&frels);
        j.state = json_obj();
        json_set(j.state, "criteria",
                 criteria("Unrelated code; mere mentions, declarations, call sites, tests or configuration without "
                          "implementation.",
                          "Likely substantive implementation, definition or explanation of any part of the requested "
                          "behavior. A matching helper for one step counts. Excerpts omit most source: favor recall."));
        json_set(j.state, "files", fmap);
        json_set(j.state, "passages", pmap);
        json_set_str(j.state, "search", query);
        vec_push(&jobs, j);
    }
    st.map_cards += (int64_t)cards.n;
    free(phase);
    phase = xasprintf("scoring %zu passage sketches across %zu files", cards.n, files.n);
    dispatch(env, &jobs, &w.stop, &st);
    if (w.stop) goto stopped;
    for (size_t i = 0; i < jobs.n; i++) {
        job_t *j = &jobs.v[i];
        if (!j->answers) stat_fail(&st, "sketches", j->error);
        for (size_t c = j->a; c < j->b; c++) {
            char key[16];
            passage_key(key, sizeof key, c - j->a);
            double sc = noul(j, key);
            if (isnan(sc) && j->answers) st.unanswered++;
            cand_t cd = {cards.v[c].f, cards.v[c].p, isnan(sc) ? 1 : sc};
            vec_push(&cands, cd);
        }
    }
    jobs_free(&jobs);
    /* strongest first: sketch score, lexical window score, path, position */
    for (size_t i = 1; i < cands.n; i++) {
        cand_t x = cands.v[i];
        size_t k = i;
        while (k > 0) {
            cand_t y = cands.v[k - 1];
            const passage_t *px = &files.v[x.f].passages.v[x.p], *py = &files.v[y.f].passages.v[y.p];
            int c;
            if (x.score != y.score) c = x.score > y.score ? -1 : 1;
            else if (px->score != py->score) c = px->score > py->score ? -1 : 1;
            else {
                c = strcmp(entries.v[files.v[x.f].node].rel, entries.v[files.v[y.f].node].rel);
                if (!c) c = px->start < py->start ? -1 : px->start > py->start;
            }
            if (c >= 0) break;
            cands.v[k] = y;
            k--;
        }
        cands.v[k] = x;
    }
    size_t nsurv = 0;
    for (size_t i = 0; i < cands.n && nsurv < FIND_FULL_LIMIT; i++)
        if (cands.v[i].score >= FIND_CUTOFF) cands.v[nsurv++] = cands.v[i];
    cands.n = nsurv;
    st.windows_pruned += (int64_t)cards.n - (int64_t)nsurv;
    vec_free(&cards);

    /* wave 3: verification of complete passages, grouped per file */
    size_t chosen_files = 0;
    size_t vgroup = MAX((size_t)1, (size_t)(FIND_VERIFY_STATE_BYTES / FIND_WINDOW_BYTES));
    for (size_t f = 0; f < files.n; f++) {
        VEC(size_t) ps = {0};
        for (size_t i = 0; i < cands.n; i++)
            if (cands.v[i].f == f) vec_push(&ps, cands.v[i].p);
        if (!ps.n) continue;
        chosen_files++;
        for (size_t a = 1; a < ps.n; a++)
            for (size_t b = a; b > 0 && ps.v[b - 1] > ps.v[b]; b--) {
                size_t t = ps.v[b];
                ps.v[b] = ps.v[b - 1];
                ps.v[b - 1] = t;
            }
        const char *rel = entries.v[files.v[f].node].rel;
        for (size_t s = 0; s < ps.n; s += vgroup) {
            job_t j = {0};
            j.a = f;
            j.questions = json_obj();
            json_t *pm = json_obj();
            for (size_t i = s; i < MIN(ps.n, s + vgroup); i++) {
                vec_push(&j.items, ps.v[i]);
                char key[16];
                passage_key(key, sizeof key, i - s);
                json_set(pm, key, json_str_own(plain_content(&files.v[f].passages.v[ps.v[i]])));
                json_set(j.questions, key, question(Q_PASSAGE, key, NULL, query));
            }
            j.state = json_obj();
            json_set(j.state, "criteria",
                     criteria("This passage only mentions, calls, imports, tests, or configures the subject, or contains "
                              "unrelated code sharing keywords.",
                              "This passage contains an implementation, definition, or substantive explanation of an "
                              "important part of the search. A helper implementing one requested step counts even when "
                              "other steps are elsewhere."));
            json_set_str(j.state, "file", rel);
            json_set(j.state, "passages", pm);
            json_set_str(j.state, "search", query);
            vec_push(&jobs, j);
        }
        vec_free(&ps);
    }
    vec_free(&cands);
    free(phase);
    phase = xasprintf("verifying %zu passages in %zu files", nsurv, chosen_files);
    dispatch(env, &jobs, &w.stop, &st);
    if (w.aborted) goto stopped;
    /* the deadline during verification ends the search with the hits verified so far */
    bool deadline_cut = w.timed_out;
    VEC(fresult_t) results = {0};
    for (size_t i = 0; i < jobs.n; i++) {
        job_t *j = &jobs.v[i];
        if (!j->answers && !j->error) continue; /* stopped by the deadline before it answered */
        if (!j->answers) {
            stat_fail(&st, "verification", j->error);
            continue;
        }
        fresult_t *r = NULL;
        for (size_t k = 0; k < results.n && !r; k++)
            if (results.v[k].f == j->a) r = &results.v[k];
        if (!r) {
            fresult_t nr = {j->a, 0, {0}, 0, 0};
            vec_push(&results, nr);
            r = &results.v[results.n - 1];
        }
        for (size_t k = 0; k < j->items.n; k++) {
            char key[16];
            passage_key(key, sizeof key, k);
            double sc = noul(j, key);
            if (isnan(sc)) {
                st.unanswered++;
                continue;
            }
            const passage_t *pg = &files.v[j->a].passages.v[j->items.v[k]];
            char *text = plain_content(pg);
            r->score = MAX(r->score, sc);
            strvec_t tl = text_lines(text, strlen(text));
            const char *first = "";
            for (size_t q = 0; q < tl.n; q++)
                if (!blank(tl.v[q])) {
                    first = tl.v[q];
                    break;
                }
            heat_t h = {pg->start, pg->end, sc, xstrndup(first, take_chars(first, 100))};
            strvec_free(&tl);
            vec_push(&r->heat, h);
            r->lines += pg->end - pg->start + 1;
            r->bytes += (int64_t)strlen(text);
            free(text);
            st.windows_judged++;
        }
    }
    jobs_free(&jobs);
    int64_t verified_windows = st.windows_judged;

    /* hits */
    json_t *hits_j = json_arr();
    VEC(size_t) hit_order = {0}; /* results indices, strongest first (stable) */
    for (size_t i = 0; i < results.n; i++) {
        fresult_t *r = &results.v[i];
        st.files_read++;
        st.file_bytes += r->bytes;
        if (r->score < FIND_THRESHOLD) continue;
        size_t k = hit_order.n;
        vec_push(&hit_order, i);
        while (k > 0 && results.v[hit_order.v[k - 1]].score < r->score) {
            hit_order.v[k] = hit_order.v[k - 1];
            k--;
        }
        hit_order.v[k] = i;
    }
    st.file_bytes += sketch_bytes;
    st.files_read += (int64_t)files.n - (int64_t)results.n;
    if (deadline_cut && !hit_order.n) {
        vec_free(&hit_order);
        for (size_t i = 0; i < results.n; i++) {
            for (size_t k = 0; k < results.v[i].heat.n; k++) free(results.v[i].heat.v[k].snippet);
            vec_free(&results.v[i].heat);
        }
        vec_free(&results);
        json_free(hits_j);
        goto stopped_no_jobs;
    }
    const char *coverage = st.failed > 0 && st.failed == st.requests ? "unavailable" : st.failed > 0 || deadline_cut ? "partial" : "complete";
    double elapsed = now_ms_f() - started;
    sb_t b = SB_INIT;
    char *where = scope ? xasprintf(" in %s", scope) : xstrdup("");
    char *thr = js_fixed(FIND_THRESHOLD, 2);
    if (str_eq(coverage, "unavailable"))
        sb_appendf(&b, "find unavailable for \"%s\"%s: all %lld judge requests failed, nothing was judged. This is not "
                       "evidence of absence; use grep.",
                   query, where, (long long)st.requests);
    else if (!hit_order.n) sb_appendf(&b, "no hits for \"%s\"%s (\xcf\x84 %s)", query, where, thr);
    else sb_appendf(&b, "%zu hit(s) for \"%s\"%s (\xcf\x84 %s), strongest first", hit_order.n, query, where, thr);
    if (deadline_cut) {
        char *d = fmt_duration(FIND_TIMEOUT_MS);
        sb_appendf(&b, "\npartial: timed out after %s with %lld of %zu passages verified; unverified files may still match. Narrow `path` for a complete search.",
                   d, (long long)verified_windows, nsurv);
        free(d);
    }
    if (str_eq(coverage, "partial") && st.failed > 0)
        sb_appendf(&b, "\npartial: %lld of %lld judge requests failed; unjudged files may still match", (long long)st.failed,
                   (long long)st.requests);
    if (hit_order.n) sb_putc(&b, '\n');
    for (size_t i = 0; i < hit_order.n; i++) {
        fresult_t *r = &results.v[hit_order.v[i]];
        const plan_t *pl = &files.v[r->f];
        const fentry_t *e = &entries.v[pl->node];
        char *abs = strstr(e->rel, "://") || e->rel[0] == '/' ? xstrdup(e->rel) : path_join(root, e->rel);
        char *rel = path_relative(env->cwd, abs);
        if (str_starts(rel, "..")) {
            free(rel);
            rel = xstrdup(abs);
        }
        free(abs);
        bool truncated = pl->truncated || r->lines < pl->total;
        char *cs = js_fixed(r->score, 2);
        sb_appendf(&b, "\n%s  %s  %lld lines judged%s", rel, cs, (long long)r->lines, truncated ? ", partial" : "");
        free(cs);
        heats_t merged = {0};
        merge_heat(&r->heat, FIND_THRESHOLD, &merged);
        json_t *h = json_obj();
        json_set_str(h, "rel", rel);
        if (!isnan(name_score[pl->node])) json_set_num(h, "nameScore", name_score[pl->node]);
        json_set_num(h, "contentScore", r->score);
        json_t *rj = json_arr();
        for (size_t q = 0; q < merged.n; q++) {
            json_t *o = json_obj();
            json_set_int(o, "start", merged.v[q].start);
            json_set_int(o, "end", merged.v[q].end);
            json_set_num(o, "p", merged.v[q].p);
            json_set_str(o, "snippet", merged.v[q].snippet);
            json_push(rj, o);
        }
        json_set(h, "ranges", rj);
        json_set_int(h, "linesSeen", r->lines);
        json_set_bool(h, "truncated", truncated);
        json_push(hits_j, h);
        /* rankedHeat: p > 0, strongest first, then earliest */
        size_t shown = 0;
        for (size_t q = 0; q < merged.n && shown < FIND_RANGES_SHOWN; q++) {
            if (!(merged.v[q].p > 0)) continue;
            shown++;
            char *ps = js_fixed(merged.v[q].p, 2);
            if (merged.v[q].start == merged.v[q].end)
                sb_appendf(&b, "\n  %s:%lld  %s  %s", rel, (long long)merged.v[q].start, ps, merged.v[q].snippet);
            else
                sb_appendf(&b, "\n  %s:%lld-%lld  %s  %s", rel, (long long)merged.v[q].start, (long long)merged.v[q].end, ps,
                           merged.v[q].snippet);
            free(ps);
        }
        for (size_t q = 0; q < merged.n; q++) free(merged.v[q].snippet);
        vec_free(&merged);
        free(rel);
    }
    {
        char *fb = fmt_bytes(st.file_bytes), *tok = fmt_number(st.input_tokens), *cost = js_fixed(st.cost, 4);
        char *wall = fmt_duration(elapsed), *api = fmt_duration(st.api_ms);
        sb_appendf(&b, "\n\nlisted %lld \xc2\xb7 judged %lld \xc2\xb7 read %lld files (%s) \xc2\xb7 %lld requests", (long long)st.listed,
                   (long long)st.judged, (long long)st.files_read, fb, (long long)st.requests);
        if (st.unanswered > 0) sb_appendf(&b, " \xc2\xb7 %lld unanswered", (long long)st.unanswered);
        sb_appendf(&b, " \xc2\xb7 %s tokens \xc2\xb7 $%s \xc2\xb7 %s wall / %s api", tok, cost, wall, api);
        free(fb);
        free(tok);
        free(cost);
        free(wall);
        free(api);
    }
    if (st.failures.n) {
        sb_append(&b, "\nfailures:");
        for (size_t i = 0; i < st.failures.n; i++) sb_appendf(&b, "\n  %s", st.failures.v[i]);
    }
    tool_result_text(out, sb_cstr(&b));
    if (str_eq(coverage, "unavailable")) out->is_error = true;
    else if (!hit_order.n) out->useless = true;
    out->details = json_obj();
    json_set_str(out->details, "query", query);
    json_t *kj = json_arr();
    for (size_t k = 0; k < kw.n; k++) json_push_str(kj, kw.v[k]);
    json_set(out->details, "keywords", kj);
    json_set_num(out->details, "threshold", FIND_THRESHOLD);
    json_set(out->details, "hits", hits_j);
    json_set_str(out->details, "coverage", coverage);
    json_t *sj = json_obj();
    json_set_int(sj, "listed", st.listed);
    json_set_int(sj, "requests", st.requests);
    json_set_int(sj, "failed", st.failed);
    json_set_int(sj, "unanswered", st.unanswered);
    json_set_int(sj, "judged", st.judged);
    json_set_int(sj, "filesRead", st.files_read);
    json_set_int(sj, "fileBytes", st.file_bytes);
    json_set_num(sj, "inputTokens", st.input_tokens);
    json_set_num(sj, "outputTokens", st.output_tokens);
    json_set_num(sj, "cost", st.cost);
    json_set_num(sj, "apiMs", st.api_ms);
    json_set_int(sj, "windowsJudged", st.windows_judged);
    json_set_int(sj, "windowsPruned", st.windows_pruned);
    json_set_int(sj, "mapCards", st.map_cards);
    json_t *fj = json_arr();
    for (size_t i = 0; i < st.failures.n; i++) json_push_str(fj, st.failures.v[i]);
    json_set(sj, "failures", fj);
    json_set(out->details, "stats", sj);
    if (deadline_cut) {
        json_t *tj = json_obj();
        json_set_int(tj, "verified", verified_windows);
        json_set_int(tj, "passages", (int64_t)nsurv);
        json_set(out->details, "timeout", tj);
    }
    json_set_num(out->details, "elapsedMs", elapsed);
    json_set_str(out->details, "cwd", env->cwd);
    if (scope) json_set_str(out->details, "scopePath", scope);
    free(where);
    free(thr);
    sb_free(&b);
    vec_free(&hit_order);
    for (size_t i = 0; i < results.n; i++) {
        for (size_t k = 0; k < results.v[i].heat.n; k++) free(results.v[i].heat.v[k].snippet);
        vec_free(&results.v[i].heat);
    }
    vec_free(&results);
    goto finish;
stopped:
    jobs_free(&jobs);
stopped_no_jobs:
    if (!w.aborted) {
        char *d = fmt_duration(FIND_TIMEOUT_MS);
        tool_result_error(out, "find timed out after %s at \"%s\", before any hit was verified. Narrow `path` to a smaller tree, or use grep.", d, phase);
        free(d);
    } else {
        tool_result_error(out, "Operation aborted");
    }
finish:
    free(phase);
    pthread_mutex_lock(&w.mu);
    w.done = true;
    pthread_cond_signal(&w.cv);
    pthread_mutex_unlock(&w.mu);
    if (wt_ok) pthread_join(wt, NULL);
    pthread_mutex_destroy(&w.mu);
    pthread_cond_destroy(&w.cv);
    for (size_t f = 0; f < files.n; f++) {
        for (size_t p = 0; p < files.v[f].passages.n; p++) free(files.v[f].passages.v[p].text);
        vec_free(&files.v[f].passages);
    }
    vec_free(&files);
    vec_free(&cards);
    vec_free(&cands);
    vec_free(&ranked);
    free(name_score);
    free(weights);
    gindex_free(&ix);
    for (size_t i = 0; i < entries.n; i++) {
        free(entries.v[i].path);
        free(entries.v[i].rel);
    }
    vec_free(&entries);
    strvec_free(&kw);
    strvec_free(&st.failures);
    free(scope);
early:
    free(root);
    free(input);
    free(query);
    return 0;
}

agent_tool_t *tool_find_new(tool_env_t *env) {
    /* jfind uses the raw markdown (no render pass: the trailing newline stays); omp:// is served as omc:// */
    char *raw = prompt_load("coding-agent/prompts/tools/find.md");
    char *desc = str_replace(raw ? raw : "", "omp://", "omc://");
    free(raw);
    json_t *s = schema_lit(
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"grep_keywords\":{\"type\""
        ":\"array\",\"items\":{\"type\":\"string\"}},\"path\":{\"type\":\"string\"}},\"required\":[\"quer"
        "y\",\"grep_keywords\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("find", "Find", desc, s, find_execute);
    free(desc);
    t->ud = env;
    return t;
}
