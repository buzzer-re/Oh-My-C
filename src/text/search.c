/* search.c - ripgrep-like content search. See search.h. */
#define _GNU_SOURCE
#include "text/search.h"

#include "core/fs.h"
#include "text/glob.h"
#include "text/walker.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEARCH_DEFAULT_MAX_FILE (4u << 20)
#define SEARCH_SINGLE_FILE_MAX ((size_t)2 << 30)

/* ---------------- file types ---------------- */
typedef struct {
    const char *names; /* '|'-separated aliases */
    const char *exts;  /* '|'-separated extensions */
    const char *files; /* '|'-separated lowercase basenames */
} ftype_t;

static const ftype_t FTYPES[] = {
    {"js|javascript", "js|jsx|mjs|cjs", ""},
    {"ts|typescript", "ts|tsx|mts|cts", ""},
    {"json", "json|jsonc|json5", ""},
    {"yaml|yml", "yaml|yml", ""},
    {"toml", "toml", ""},
    {"md|markdown", "md|markdown|mdx", ""},
    {"py|python", "py|pyi", ""},
    {"rs|rust", "rs", ""},
    {"go", "go", ""},
    {"java", "java", ""},
    {"kt|kotlin", "kt|kts", ""},
    {"c", "c|h", ""},
    {"cpp|cxx", "cpp|cc|cxx|hpp|hxx|hh", ""},
    {"cs|csharp", "cs|csx", ""},
    {"php", "php|phtml", ""},
    {"rb|ruby", "rb|rake|gemspec", ""},
    {"sh|bash", "sh|bash|zsh", ""},
    {"zsh", "zsh", ""},
    {"fish", "fish", ""},
    {"html", "html|htm", ""},
    {"css", "css", ""},
    {"scss", "scss", ""},
    {"sass", "sass", ""},
    {"less", "less", ""},
    {"xml", "xml", ""},
    {"docker|dockerfile", "", "dockerfile"},
    {"make|makefile", "", "makefile"},
};

static bool in_list(const char *list, const char *s, size_t n) {
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, '|');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && strncmp(p, s, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

bool search_type_matches(const char *type, const char *path) {
    if (!type) return true;
    while (*type == ' ' || *type == '.') type++;
    if (!*type) return true;
    char *t = str_lower(type);
    size_t tl = strlen(t);
    while (tl && t[tl - 1] == ' ') t[--tl] = 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    const char *ext = dot ? dot + 1 : "";
    bool r = false;
    const ftype_t *ft = NULL;
    for (size_t i = 0; i < ARRAY_LEN(FTYPES); i++)
        if (in_list(FTYPES[i].names, t, tl)) { ft = &FTYPES[i]; break; }
    if (ft) {
        char *lb = str_lower(base);
        if (ft->files[0] && in_list(ft->files, lb, strlen(lb))) r = true;
        free(lb);
        if (!r && *ext) r = in_list(ft->exts, ext, strlen(ext));
    } else {
        r = *ext && strcmp(ext, t) == 0;
    }
    free(t);
    return r;
}

/* ---------------- compile ---------------- */
rx_t *search_compile(const search_opts_t *o, char **err) {
    const char *pat = o->pattern ? o->pattern : "";
    bool upper = false;
    if (o->fixed_strings) {
        for (const char *p = pat; *p; p++)
            if (*p >= 'A' && *p <= 'Z') upper = true;
    } else {
        upper = rx_has_uppercase(pat);
    }
    bool icase = o->ignore_case || (o->smart_case && !upper);
    bool multiline = o->multiline || strchr(pat, '\n') || (!o->fixed_strings && strstr(pat, "\\n"));
    int flags = RX_MULTILINE;
    if (icase) flags |= RX_ICASE;
    if (!multiline) flags |= RX_LINE;
    char *owned = NULL;
    if (o->word) {
        char *inner = o->fixed_strings ? rx_escape(pat) : xstrdup(pat);
        owned = xasprintf("\\b(?:%s)\\b", inner);
        free(inner);
        pat = owned;
    } else if (o->fixed_strings) {
        flags |= RX_LITERAL;
    }
    rx_t *rx = o->fixed_strings && !o->word ? rx_compile(pat, flags, err) : rx_compile_grep(pat, flags, err);
    free(owned);
    return rx;
}

/* ---------------- per-buffer search ---------------- */
typedef struct {
    int before, after;
    uint32_t cap; /* 0 = unlimited */
    int max_columns;
    search_mode_t mode;
    bool multiline;
} sparams_t;

typedef VEC(search_hit_t) hitvec_t;

typedef struct {
    hitvec_t hits;
    uint32_t nmatch;
    bool more; /* stopped at cap while more matches exist */
    bool processed;
    bool oversized;
    bool binary_match; /* skipped as binary although its content matches (pi-walker ScanOutcome.binary_match) */
} fileres_t;

static void *mem_rchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--)
        if (*--p == (unsigned char)c) return (void *)p;
    return NULL;
}

static size_t count_nl(const char *s, size_t n) {
    size_t c = 0;
    const char *e = s + n;
    while (s < e) {
        const char *p = memchr(s, '\n', (size_t)(e - s));
        if (!p) break;
        c++;
        s = p + 1;
    }
    return c;
}

static size_t floor_cb(const char *s, size_t len, size_t i) {
    if (i >= len) return len;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static size_t ceil_cb(const char *s, size_t len, size_t i) {
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static uint32_t char_count(const char *s, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

/* Build display text for line bytes [s, s+n); ms/me = match byte range within (or -1). */
static char *line_text(const char *s, size_t n, ptrdiff_t ms, ptrdiff_t me, int maxcol, bool *truncated) {
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\v' ||
                     s[n - 1] == '\f'))
        n--;
    /* a named file is searched as text: a NUL byte becomes another C0 control, which the tool's sanitizeText drops
     * like omp's (keeping the line's byte length for the column window) */
    char *nul_free = NULL;
    if (memchr(s, 0, n)) {
        nul_free = xmalloc(n + 1);
        for (size_t i = 0; i < n; i++) nul_free[i] = s[i] ? s[i] : '\x01';
        nul_free[n] = 0;
        s = nul_free;
    }
    char *text;
    size_t tl;
    if (utf8_valid(s, n)) {
        text = xstrndup(s, n);
        tl = n;
    } else {
        text = utf8_sanitize(s, n);
        tl = strlen(text);
        if (ms >= 0) {
            char *a = utf8_sanitize(s, (size_t)MIN((size_t)ms, n));
            char *b = utf8_sanitize(s, (size_t)MIN((size_t)(me >= 0 ? me : ms), n));
            ms = (ptrdiff_t)strlen(a);
            me = (ptrdiff_t)strlen(b);
            free(a);
            free(b);
        }
    }
    free(nul_free);
    *truncated = false;
    if (maxcol <= 0 || tl <= (size_t)maxcol) return text;
    *truncated = true;
    size_t max = (size_t)maxcol;
    size_t head_end = floor_cb(text, tl, max > 3 ? max - 3 : 0);
    sb_t b = SB_INIT;
    if (ms < 0) {
        sb_appendn(&b, text, head_end);
        sb_append(&b, "...");
    } else {
        size_t start = floor_cb(text, tl, MIN((size_t)ms, tl));
        size_t end = MIN((size_t)(me >= ms ? me : ms), tl);
        size_t body = max > 6 ? max - 6 : 0;
        if (end <= head_end || body == 0) {
            sb_appendn(&b, text, head_end);
            sb_append(&b, "...");
        } else {
            size_t mlen = end - start;
            size_t lead = body > mlen ? (body - mlen) / 2 : 0;
            size_t from = start > lead ? start - lead : 0;
            if (from + body >= tl) {
                size_t f2 = ceil_cb(text, tl, tl - (max > 3 ? max - 3 : 0));
                sb_append(&b, "...");
                sb_appendn(&b, text + f2, tl - f2);
            } else {
                from = ceil_cb(text, tl, from);
                size_t to = floor_cb(text, tl, from + body);
                if (from == 0) {
                    sb_appendn(&b, text, to);
                    sb_append(&b, "...");
                } else {
                    sb_append(&b, "...");
                    sb_appendn(&b, text + from, to - from);
                    sb_append(&b, "...");
                }
            }
        }
    }
    free(text);
    return sb_detach(&b);
}

static void push_line(fileres_t *fr, const char *buf, size_t ls, size_t le, uint32_t lineno, bool ctx, ptrdiff_t ms,
                      ptrdiff_t me, const sparams_t *p, uint32_t block) {
    search_hit_t h;
    memset(&h, 0, sizeof(h));
    h.line = lineno;
    h.is_context = ctx;
    h.block = block;
    h.text = line_text(buf + ls, le - ls, ctx ? -1 : ms - (ptrdiff_t)ls, ctx ? -1 : me - (ptrdiff_t)ls, p->max_columns,
                       &h.truncated);
    if (!ctx) h.column = char_count(buf + ls, (size_t)(ms - (ptrdiff_t)ls)) + 1;
    vec_push(&fr->hits, h);
}

typedef struct {
    size_t after_left, after_off;
    uint32_t after_line, last_emitted;
} ctxstate_t;

static void flush_after(fileres_t *fr, const char *buf, size_t len, ctxstate_t *cs, uint32_t limit_line,
                        const sparams_t *p, uint32_t block) {
    while (cs->after_left > 0 && cs->after_line < limit_line && cs->after_off < len) {
        const char *e = memchr(buf + cs->after_off, '\n', len - cs->after_off);
        size_t le = e ? (size_t)(e - buf) : len;
        push_line(fr, buf, cs->after_off, le, cs->after_line, true, -1, -1, p, block);
        cs->last_emitted = cs->after_line;
        cs->after_line++;
        cs->after_off = le + 1;
        cs->after_left--;
    }
    if (cs->after_line >= limit_line) cs->after_left = 0;
}

static void search_bytes(const rx_t *rx, const char *buf, size_t len, const sparams_t *p, fileres_t *fr) {
    if (p->mode == SEARCH_FILES) {
        if (rx_is_match(rx, buf, len, 0)) {
            /* an empty match after the final newline doesn't count */
            size_t ms, me;
            if (rx_find(rx, buf, len, 0, &ms, &me) && !(ms >= len && len > 0 && buf[len - 1] == '\n')) fr->nmatch = 1;
        }
        return;
    }
    bool content = p->mode == SEARCH_CONTENT;
    size_t pos = 0, cur_off = 0;
    uint32_t cur_line = 1;
    ctxstate_t cs = {0, 0, 0, 0};
    while (pos <= len) {
        size_t ms, me;
        if (!rx_find(rx, buf, len, pos, &ms, &me)) break;
        if (ms >= len && len > 0 && buf[len - 1] == '\n') break;
        const char *nl = ms > pos ? mem_rchr(buf + pos, '\n', ms - pos) : NULL;
        size_t ls = nl ? (size_t)(nl - buf) + 1 : pos;
        size_t le;
        if (p->multiline && me > ms && buf[me - 1] == '\n') {
            le = me - 1;
        } else {
            size_t from = MAX(me, ms);
            const char *ne = from < len ? memchr(buf + from, '\n', len - from) : NULL;
            le = ne ? (size_t)(ne - buf) : len;
        }
        cur_line += (uint32_t)count_nl(buf + cur_off, ls - cur_off);
        cur_off = ls;
        uint32_t L = cur_line;
        uint32_t span = p->multiline ? (uint32_t)count_nl(buf + ls, le - ls) + 1 : 1;
        if (content) {
            uint32_t block = fr->nmatch;
            if (fr->nmatch > 0) flush_after(fr, buf, len, &cs, L, p, block - 1);
            if (p->before > 0 && L > 1) {
                uint32_t avail = L - 1 - cs.last_emitted;
                uint32_t n = MIN((uint32_t)p->before, avail);
                size_t starts[64];
                size_t *st = n <= 64 ? starts : xmalloc(n * sizeof(size_t));
                size_t s = ls;
                for (uint32_t k = 0; k < n; k++) {
                    const char *q = s >= 2 ? mem_rchr(buf, '\n', s - 1) : NULL;
                    s = q ? (size_t)(q - buf) + 1 : 0;
                    st[n - 1 - k] = s;
                }
                for (uint32_t k = 0; k < n; k++) {
                    size_t a = st[k];
                    const char *e = memchr(buf + a, '\n', len - a);
                    size_t b = e ? (size_t)(e - buf) : len;
                    push_line(fr, buf, a, b, L - n + k, true, -1, -1, p, block);
                }
                if (st != starts) free(st);
            }
            push_line(fr, buf, ls, le, L, false, (ptrdiff_t)ms, (ptrdiff_t)me, p, block);
            cs.last_emitted = L + span - 1;
            cs.after_left = (size_t)p->after;
            cs.after_line = L + span;
            cs.after_off = le + 1;
        }
        fr->nmatch++;
        if (p->cap && fr->nmatch >= p->cap) {
            if (le < len) {
                size_t a, b;
                if (rx_find(rx, buf, len, le + 1, &a, &b) && !(a >= len && buf[len - 1] == '\n')) fr->more = true;
            }
            break;
        }
        if (le >= len) break;
        if (p->multiline) {
            cur_line = L + span;
            cur_off = le + 1;
        }
        pos = le + 1;
    }
    if (content && fr->nmatch > 0) flush_after(fr, buf, len, &cs, UINT32_MAX, p, fr->nmatch - 1);
}

/* ---------------- aggregation ---------------- */
static void hit_free(search_hit_t *h) {
    free(h->path);
    free(h->text);
}

static void fileres_free(fileres_t *fr) {
    for (size_t i = 0; i < fr->hits.n; i++) hit_free(&fr->hits.v[i]);
    vec_free(&fr->hits);
}

static void aggregate(const search_opts_t *o, char **paths, fileres_t *res, size_t n, search_result_t *out) {
    uint32_t skip = o->offset > 0 ? (uint32_t)o->offset : 0;
    uint32_t limit = o->max_count > 0 ? (uint32_t)o->max_count : 0;
    uint32_t taken = 0;
    bool full = false;
    for (size_t i = 0; i < n; i++) {
        fileres_t *fr = &res[i];
        if (!fr->processed) {
            if (full) out->limit_reached = true;
            continue;
        }
        if (fr->oversized) { out->skipped_oversized++; continue; }
        out->files_searched++;
        if (fr->binary_match && paths[i]) strvec_push(&out->binary_matches, paths[i]);
        if (fr->nmatch == 0) continue;
        out->files_with_matches++;
        out->total_matches += o->mode == SEARCH_FILES ? 1 : fr->nmatch;
        if (full) { out->limit_reached = true; continue; }
        if (o->mode == SEARCH_FILES) {
            if (skip) { skip--; continue; }
            search_hit_t h;
            memset(&h, 0, sizeof(h));
            h.path = xstrdup(paths[i] ? paths[i] : "");
            vec_push(&out->hits, h);
            if (limit && ++taken >= limit) full = true;
            continue;
        }
        if (o->mode == SEARCH_COUNT) {
            uint32_t c = fr->nmatch;
            if (skip >= c) { skip -= c; continue; }
            c -= skip;
            skip = 0;
            if (limit && taken + c >= limit) {
                if (taken + c > limit || fr->more) out->limit_reached = true;
                c = limit - taken;
                full = true;
            }
            taken += c;
            search_hit_t h;
            memset(&h, 0, sizeof(h));
            h.path = xstrdup(paths[i] ? paths[i] : "");
            h.count = c;
            vec_push(&out->hits, h);
            continue;
        }
        /* content: decide which blocks are included */
        uint8_t inc_stack[256];
        uint8_t *inc = fr->nmatch <= sizeof(inc_stack) ? inc_stack : xmalloc(fr->nmatch);
        for (uint32_t b = 0; b < fr->nmatch; b++) {
            if (skip) { skip--; inc[b] = 0; continue; }
            if (limit && taken >= limit) { inc[b] = 0; out->limit_reached = true; full = true; continue; }
            inc[b] = 1;
            taken++;
        }
        if (limit && taken >= limit) {
            full = true;
            if (fr->more) out->limit_reached = true;
        }
        for (size_t k = 0; k < fr->hits.n; k++) {
            search_hit_t *h = &fr->hits.v[k];
            if (h->block < fr->nmatch && inc[h->block]) {
                search_hit_t c = *h;
                c.path = xstrdup(paths[i] ? paths[i] : "");
                vec_push(&out->hits, c);
                h->text = NULL; /* moved */
            }
        }
        if (inc != inc_stack) free(inc);
    }
}

static sparams_t make_params(const search_opts_t *o, bool multiline) {
    sparams_t p;
    memset(&p, 0, sizeof(p));
    p.before = MAX(0, o->context_before);
    p.after = MAX(0, o->context_after);
    p.max_columns = o->max_columns;
    p.mode = o->mode;
    p.multiline = multiline;
    uint32_t cap = 0;
    if (o->max_count > 0) cap = (uint32_t)o->max_count + (uint32_t)MAX(0, o->offset);
    if (o->max_count_per_file > 0 && (cap == 0 || (uint32_t)o->max_count_per_file < cap)) cap = (uint32_t)o->max_count_per_file;
    p.cap = cap;
    return p;
}

void search_buffer(const rx_t *rx, const char *buf, size_t len, const search_opts_t *o, search_result_t *out) {
    memset(out, 0, sizeof(*out));
    sparams_t p = make_params(o, !(rx_flags(rx) & RX_LINE));
    fileres_t fr;
    memset(&fr, 0, sizeof(fr));
    fr.processed = true;
    search_bytes(rx, buf, len, &p, &fr);
    char *paths[1] = {NULL};
    aggregate(o, paths, &fr, 1, out);
    for (size_t i = 0; i < out->hits.n; i++) {
        free(out->hits.v[i].path);
        out->hits.v[i].path = NULL;
    }
    fileres_free(&fr);
}

/* ---------------- file reading ---------------- */
typedef struct {
    char *buf;
    size_t cap;
} rbuf_t;

/* 1 ok, 0 skip (unreadable / not regular), 2 oversized */
static int read_file(const char *path, size_t max, rbuf_t *rb, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return 0;
    }
    size_t size = (size_t)st.st_size;
    if (max && size > max) {
        close(fd);
        return 2;
    }
    if (size + 1 > rb->cap) {
        rb->cap = size + 1 < 65536 ? 65536 : size + 1;
        free(rb->buf);
        rb->buf = xmalloc(rb->cap);
    }
    size_t n = 0;
    while (n < size) {
        ssize_t r = read(fd, rb->buf + n, size - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    rb->buf[n] = 0;
    *len = n;
    return 1;
}

/* ---------------- directory driver ---------------- */
typedef struct {
    const search_opts_t *o;
    const rx_t *rx;
    sparams_t p;
    const char *root;
    char **paths;
    fileres_t *res;
    size_t n;
    size_t max_size;
    size_t next;
    pthread_mutex_t mu;
    size_t prefix_idx;
    uint64_t prefix_sum;
    uint64_t need; /* 0 = unlimited */
    size_t stop_at;
} sdrv_t;

static void *search_worker(void *arg) {
    sdrv_t *d = arg;
    rbuf_t rb = {NULL, 0};
    for (;;) {
        size_t i = __atomic_fetch_add(&d->next, 1, __ATOMIC_RELAXED);
        if (i >= d->n || i >= __atomic_load_n(&d->stop_at, __ATOMIC_RELAXED)) break;
        fileres_t *fr = &d->res[i];
        char *abs = d->root ? path_join(d->root, d->paths[i]) : xstrdup(d->paths[i]);
        size_t len = 0;
        int r = read_file(abs, d->max_size, &rb, &len);
        free(abs);
        if (r == 2) fr->oversized = true;
        else if (r == 1) {
            bool binary = !d->o->search_binary && memchr(rb.buf, 0, len) != NULL;
            /* a binary input is not searched, only probed for a first match */
            if (binary) fr->binary_match = rx_is_match(d->rx, rb.buf, len, 0);
            else search_bytes(d->rx, rb.buf, len, &d->p, fr);
        }
        pthread_mutex_lock(&d->mu);
        fr->processed = true;
        while (d->prefix_idx < d->n && d->res[d->prefix_idx].processed) {
            fileres_t *f = &d->res[d->prefix_idx];
            d->prefix_sum += d->o->mode == SEARCH_FILES ? (f->nmatch > 0) : f->nmatch;
            d->prefix_idx++;
            if (d->need && d->prefix_sum >= d->need && d->stop_at > d->prefix_idx) {
                /* keep one extra file so limit_reached can be detected */
                __atomic_store_n(&d->stop_at, d->prefix_idx + 1, __ATOMIC_RELAXED);
            }
        }
        pthread_mutex_unlock(&d->mu);
    }
    free(rb.buf);
    return NULL;
}

typedef struct {
    pthread_mutex_t mu;
    strvec_t files;
    const char *type;
} fcollect_t;

static bool file_cb(const walk_entry_t *e, void *ud) {
    fcollect_t *c = ud;
    if (c->type && !search_type_matches(c->type, e->path)) return true;
    pthread_mutex_lock(&c->mu);
    strvec_push(&c->files, e->path);
    pthread_mutex_unlock(&c->mu);
    return true;
}

/* pi-walker grep sorts hits by path bytes */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int search_run(const search_opts_t *o, search_result_t *out) {
    memset(out, 0, sizeof(*out));
    char *err = NULL;
    rx_t *rx = search_compile(o, &err);
    if (!rx) {
        out->error = err ? err : xstrdup("invalid pattern");
        return -1;
    }
    const char *path = o->path && *o->path ? o->path : ".";
    struct stat st;
    if (stat(path, &st) != 0) {
        out->error = xasprintf("%s: %s", path, strerror(errno));
        rx_free(rx);
        return -1;
    }
    bool multiline = !(rx_flags(rx) & RX_LINE);
    sparams_t p = make_params(o, multiline);
    if (!S_ISDIR(st.st_mode)) {
        /* single explicitly named file: no size limit, searched as text even when it holds NUL bytes (pi-walker
         * ScanParams.text for an explicit file root) */
        fileres_t fr;
        memset(&fr, 0, sizeof(fr));
        fr.processed = true;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            size_t len = (size_t)st.st_size;
            if (len > SEARCH_SINGLE_FILE_MAX) len = SEARCH_SINGLE_FILE_MAX;
            void *map = len ? mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0) : NULL;
            if (map == MAP_FAILED) map = NULL;
            const char *buf = map ? map : "";
            if (!len || map) {
                search_bytes(rx, buf, len, &p, &fr);
            }
            if (map) munmap(map, len);
            close(fd);
        }
        char *paths[1] = {(char *)path};
        aggregate(o, paths, &fr, 1, out);
        fileres_free(&fr);
        rx_free(rx);
        return 0;
    }
    /* directory: walk */
    walk_opts_t wo;
    memset(&wo, 0, sizeof(wo));
    wo.include_hidden = o->hidden;
    wo.no_ignore = o->no_ignore;
    wo.follow_symlinks = o->follow_symlinks;
    wo.types = WALK_FILE;
    wo.threads = o->threads;
    wo.skip_node_modules = !(o->glob && strstr(o->glob, "node_modules"));
    globset_t inc = {0}, exc = {0};
    if (o->glob && *o->glob) {
        char *g = glob_normalize(o->glob, !o->glob_anchored);
        globset_add(&inc, g, GLOB_PATHNAME);
        size_t gl = strlen(g);
        while (gl > 0 && g[gl - 1] == '/') g[--gl] = 0;
        if (strcmp(g, "**") != 0 && !str_ends(g, "/**")) {
            char *d = xasprintf("%s/**", g);
            globset_add(&inc, d, GLOB_PATHNAME);
            free(d);
        }
        free(g);
        wo.include = &inc;
    }
    for (size_t i = 0; i < o->nexclude; i++) {
        const char *e = o->exclude[i];
        if (!e || !*e) continue;
        while (e[0] == '.' && e[1] == '/') e += 2;
        char *pat;
        if (e[0] == '/') pat = xstrdup(e + 1);
        else if (!strchr(e, '/')) pat = xasprintf("**/%s", e);
        else pat = xstrdup(e);
        size_t pl = strlen(pat);
        while (pl > 0 && pat[pl - 1] == '/') pat[--pl] = 0;
        globset_add(&exc, pat, GLOB_PATHNAME);
        free(pat);
    }
    if (exc.pats.n) wo.exclude = &exc;
    fcollect_t fc;
    memset(&fc, 0, sizeof(fc));
    pthread_mutex_init(&fc.mu, NULL);
    fc.type = o->type && *o->type ? o->type : NULL;
    walk(path, &wo, file_cb, &fc);
    pthread_mutex_destroy(&fc.mu);
    globset_free(&inc);
    globset_free(&exc);
    if (fc.files.n > 1) xqsort(fc.files.v, fc.files.n, sizeof(char *), cmp_str);

    sdrv_t d;
    memset(&d, 0, sizeof(d));
    d.o = o;
    d.rx = rx;
    d.p = p;
    d.root = path;
    d.paths = fc.files.v;
    d.n = fc.files.n;
    d.res = xcalloc(d.n ? d.n : 1, sizeof(fileres_t));
    d.max_size = o->max_file_size ? o->max_file_size : SEARCH_DEFAULT_MAX_FILE;
    d.stop_at = SIZE_MAX;
    if (o->max_count > 0) d.need = (uint64_t)o->max_count + (uint64_t)MAX(0, o->offset);
    pthread_mutex_init(&d.mu, NULL);
    int nt = o->threads > 0 ? o->threads : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nt < 1) nt = 1;
    if (nt > 16) nt = 16;
    if ((size_t)nt > d.n) nt = d.n ? (int)d.n : 1;
    pthread_t th[16];
    bool started[16] = {0};
    for (int i = 1; i < nt; i++) started[i] = pthread_create(&th[i], NULL, search_worker, &d) == 0;
    search_worker(&d);
    for (int i = 1; i < nt; i++)
        if (started[i]) pthread_join(th[i], NULL);
    pthread_mutex_destroy(&d.mu);
    aggregate(o, d.paths, d.res, d.n, out);
    for (size_t i = 0; i < d.n; i++) fileres_free(&d.res[i]);
    free(d.res);
    strvec_free(&fc.files);
    rx_free(rx);
    return 0;
}

void search_result_free(search_result_t *r) {
    if (!r) return;
    for (size_t i = 0; i < r->hits.n; i++) hit_free(&r->hits.v[i]);
    vec_free(&r->hits);
    strvec_free(&r->binary_matches);
    free(r->error);
    r->error = NULL;
}

json_t *search_result_to_json(const search_result_t *r) {
    json_t *o = json_obj();
    json_t *arr = json_arr();
    for (size_t i = 0; i < r->hits.n; i++) {
        const search_hit_t *h = &r->hits.v[i];
        json_t *m = json_obj();
        if (h->path) json_set_str(m, "path", h->path);
        json_set_int(m, "lineNumber", h->line);
        json_set_str(m, "line", h->text ? h->text : "");
        if (h->column) json_set_int(m, "column", h->column);
        if (h->truncated) json_set_bool(m, "truncated", true);
        if (h->is_context) json_set_bool(m, "isContext", true);
        if (!h->text && h->line == 0 && h->count) json_set_int(m, "matchCount", h->count);
        json_push(arr, m);
    }
    json_set(o, "matches", arr);
    json_set_int(o, "totalMatches", r->total_matches);
    json_set_int(o, "filesWithMatches", r->files_with_matches);
    json_set_int(o, "filesSearched", r->files_searched);
    if (r->limit_reached) json_set_bool(o, "limitReached", true);
    if (r->skipped_oversized) json_set_int(o, "skippedOversized", r->skipped_oversized);
    if (r->error) json_set_str(o, "error", r->error);
    return o;
}
