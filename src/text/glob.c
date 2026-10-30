/* glob.c - glob matching and gitignore rule sets. See glob.h. */
#include "text/glob.h"

#include "core/fs.h"

#include <ctype.h>

#define GLOB_MAX_ALTS 4096
#define GLOB_MAX_DEPTH 32

typedef struct {
    char **segs;  /* pathname mode: segments; else segs[0] is the whole pattern */
    bool *dstar;  /* segment is exactly "**" */
    size_t n;
} galt_t;

struct glob_pat {
    int flags;
    galt_t *alts;
    size_t nalts;
};

/* ---------------- brace expansion ---------------- */

/* Index of the '}' matching the '{' at p[open], or -1. */
static ptrdiff_t brace_close(const char *p, size_t open) {
    int depth = 0;
    for (size_t i = open; p[i]; i++) {
        char c = p[i];
        if (c == '\\' && p[i + 1]) { i++; continue; }
        if (c == '[') {
            size_t j = i + 1;
            if (p[j] == '!' || p[j] == '^') j++;
            if (p[j] == ']') j++;
            while (p[j] && p[j] != ']') j++;
            if (p[j] == ']') { i = j; continue; }
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') {
            if (--depth == 0) return (ptrdiff_t)i;
        }
    }
    return -1;
}

static void brace_expand(const char *p, strvec_t *out, int depth) {
    if (out->n >= GLOB_MAX_ALTS) return;
    ptrdiff_t open = -1, close = -1;
    for (size_t i = 0; p[i]; i++) {
        if (p[i] == '\\' && p[i + 1]) { i++; continue; }
        if (p[i] == '[') {
            size_t j = i + 1;
            if (p[j] == '!' || p[j] == '^') j++;
            if (p[j] == ']') j++;
            while (p[j] && p[j] != ']') j++;
            if (p[j] == ']') i = j;
            continue;
        }
        if (p[i] == '{') {
            ptrdiff_t c = brace_close(p, i);
            if (c >= 0) { open = (ptrdiff_t)i; close = c; break; }
        }
    }
    if (open < 0 || depth > GLOB_MAX_DEPTH) {
        strvec_push(out, p);
        return;
    }
    /* split body on top-level commas */
    size_t start = (size_t)open + 1;
    int d = 0;
    for (size_t i = start; i <= (size_t)close; i++) {
        char c = p[i];
        if (c == '\\' && i + 1 < (size_t)close) { i++; continue; }
        if (c == '{') d++;
        else if (c == '}' && i < (size_t)close) d--;
        if ((c == ',' && d == 0) || i == (size_t)close) {
            char *alt = xasprintf("%.*s%.*s%s", (int)open, p, (int)(i - start), p + start, p + close + 1);
            brace_expand(alt, out, depth + 1);
            free(alt);
            start = i + 1;
            if (out->n >= GLOB_MAX_ALTS) return;
        }
    }
}

/* ---------------- single-segment wildcard matching ---------------- */

static inline int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static uint32_t dec(const char *s, size_t len, size_t *n) {
    uint32_t cp;
    int k = utf8_decode(s, len, &cp);
    *n = (size_t)(k > 0 ? k : 1);
    return cp;
}

/* Try to match one pattern element at pat[p] against str[s]. */
static bool match_one(const char *pat, size_t plen, size_t p, const char *str, size_t slen, size_t s, int flags,
                      size_t *pn, size_t *sn) {
    if (s >= slen) return false;
    char pc = pat[p];
    size_t clen;
    if (pc == '?') {
        uint32_t c = dec(str + s, slen - s, &clen);
        if ((flags & GLOB_PATHNAME) && c == '/') return false;
        *pn = p + 1;
        *sn = s + clen;
        return true;
    }
    if (pc == '[') {
        size_t j = p + 1;
        bool neg = false;
        if (j < plen && (pat[j] == '!' || pat[j] == '^')) { neg = true; j++; }
        size_t body = j;
        if (j < plen && pat[j] == ']') j++;
        while (j < plen && pat[j] != ']') {
            if (pat[j] == '\\' && j + 1 < plen) j++;
            j++;
        }
        if (j < plen) {
            uint32_t c = dec(str + s, slen - s, &clen);
            if ((flags & GLOB_PATHNAME) && c == '/') return false;
            bool hit = false;
            size_t k = body;
            bool first = true;
            while (k < j && (first || pat[k] != ']')) {
                first = false;
                size_t n1;
                if (pat[k] == '\\' && k + 1 < j) k++;
                uint32_t lo = dec(pat + k, j - k, &n1);
                k += n1;
                uint32_t hi = lo;
                if (k + 1 < j && pat[k] == '-') {
                    size_t n2;
                    size_t q = k + 1;
                    if (pat[q] == '\\' && q + 1 < j) q++;
                    hi = dec(pat + q, j - q, &n2);
                    k = q + n2;
                }
                if (c >= lo && c <= hi) hit = true;
                else if ((flags & GLOB_ICASE) && c < 128) {
                    uint32_t a = (uint32_t)lc((int)c), b = (uint32_t)((c >= 'a' && c <= 'z') ? c - 32 : c);
                    if ((a >= lo && a <= hi) || (b >= lo && b <= hi)) hit = true;
                }
            }
            if (hit == neg) return false;
            *pn = j + 1;
            *sn = s + clen;
            return true;
        }
        /* unclosed '[' is literal */
    }
    if (pc == '\\' && p + 1 < plen) {
        p++;
        pc = pat[p];
    }
    char sc = str[s];
    if (sc == pc || ((flags & GLOB_ICASE) && lc((unsigned char)sc) == lc((unsigned char)pc))) {
        *pn = p + 1;
        *sn = s + 1;
        return true;
    }
    return false;
}

static bool wild_match(const char *pat, size_t plen, const char *str, size_t slen, int flags) {
    size_t p = 0, s = 0, sp = SIZE_MAX, ss = 0;
    while (s < slen) {
        if (p < plen && pat[p] == '*') {
            while (p < plen && pat[p] == '*') p++;
            sp = p;
            ss = s;
            continue;
        }
        size_t pn, sn;
        if (p < plen && match_one(pat, plen, p, str, slen, s, flags, &pn, &sn)) {
            p = pn;
            s = sn;
            continue;
        }
        if (sp != SIZE_MAX) {
            if ((flags & GLOB_PATHNAME) && str[ss] == '/') return false;
            size_t n;
            dec(str + ss, slen - ss, &n);
            ss += n;
            s = ss;
            p = sp;
            continue;
        }
        return false;
    }
    while (p < plen && pat[p] == '*') p++;
    return p == plen;
}

/* ---------------- compile / match ---------------- */

static void galt_build(galt_t *a, const char *pat, int flags) {
    memset(a, 0, sizeof(*a));
    if (!(flags & GLOB_PATHNAME)) {
        a->segs = xmalloc(sizeof(char *));
        a->dstar = xcalloc(1, sizeof(bool));
        a->segs[0] = xstrdup(pat);
        a->n = 1;
        return;
    }
    while (pat[0] == '.' && pat[1] == '/') pat += 2;
    strvec_t parts = str_split(pat, "/");
    a->segs = xmalloc((parts.n ? parts.n : 1) * sizeof(char *));
    a->dstar = xcalloc(parts.n ? parts.n : 1, sizeof(bool));
    for (size_t i = 0; i < parts.n; i++) {
        if (!parts.v[i][0]) continue;
        if (a->n > 0 && a->dstar[a->n - 1] && strcmp(parts.v[i], "**") == 0) continue; /* collapse ** / ** */
        a->dstar[a->n] = strcmp(parts.v[i], "**") == 0;
        a->segs[a->n++] = xstrdup(parts.v[i]);
    }
    strvec_free(&parts);
}

glob_pat_t *glob_compile(const char *pattern, int flags) {
    glob_pat_t *g = xcalloc(1, sizeof(*g));
    g->flags = flags;
    strvec_t alts = {0};
    if (flags & GLOB_NOBRACE) strvec_push(&alts, pattern ? pattern : "");
    else brace_expand(pattern ? pattern : "", &alts, 0);
    g->alts = xcalloc(alts.n ? alts.n : 1, sizeof(galt_t));
    for (size_t i = 0; i < alts.n; i++) galt_build(&g->alts[g->nalts++], alts.v[i], flags);
    strvec_free(&alts);
    return g;
}

void glob_pat_free(glob_pat_t *g) {
    if (!g) return;
    for (size_t i = 0; i < g->nalts; i++) {
        for (size_t k = 0; k < g->alts[i].n; k++) free(g->alts[i].segs[k]);
        free(g->alts[i].segs);
        free(g->alts[i].dstar);
    }
    free(g->alts);
    free(g);
}

typedef struct {
    const char *s;
    size_t len;
} span_t;

static bool seg_match_rec(const galt_t *a, size_t i, const span_t *ps, size_t np, size_t j, int flags, int8_t *memo) {
    int8_t *m = &memo[i * (np + 1) + j];
    if (*m >= 0) return *m;
    bool r;
    if (i == a->n) {
        r = j == np;
    } else if (a->dstar[i]) {
        r = false;
        bool trailing = i + 1 == a->n && a->n > 1;
        for (size_t k = trailing ? j + 1 : j; k <= np && !r; k++) r = seg_match_rec(a, i + 1, ps, np, k, flags, memo);
    } else {
        r = j < np && wild_match(a->segs[i], strlen(a->segs[i]), ps[j].s, ps[j].len, flags) &&
            seg_match_rec(a, i + 1, ps, np, j + 1, flags, memo);
    }
    *m = r;
    return r;
}

bool glob_pat_match(const glob_pat_t *g, const char *path) {
    if (!g || !path) return false;
    if (!(g->flags & GLOB_PATHNAME)) {
        size_t pl = strlen(path);
        for (size_t i = 0; i < g->nalts; i++)
            if (wild_match(g->alts[i].segs[0], strlen(g->alts[i].segs[0]), path, pl, g->flags)) return true;
        return false;
    }
    while (path[0] == '.' && path[1] == '/') path += 2;
    span_t stackbuf[64];
    span_t *ps = stackbuf;
    size_t np = 0, cap = 64;
    for (const char *p = path; *p;) {
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l) {
            if (np == cap) {
                cap *= 2;
                if (ps == stackbuf) {
                    ps = xmalloc(cap * sizeof(span_t));
                    memcpy(ps, stackbuf, sizeof(stackbuf));
                } else {
                    ps = xrealloc(ps, cap * sizeof(span_t));
                }
            }
            ps[np].s = p;
            ps[np].len = l;
            np++;
        }
        p += l;
        if (*p == '/') p++;
    }
    bool ok = false;
    for (size_t i = 0; i < g->nalts && !ok; i++) {
        const galt_t *a = &g->alts[i];
        if (a->n == 0) { ok = np == 0; continue; }
        /* quick reject: last non-** segment must match the basename */
        if (!a->dstar[a->n - 1] && np > 0 &&
            !wild_match(a->segs[a->n - 1], strlen(a->segs[a->n - 1]), ps[np - 1].s, ps[np - 1].len, g->flags))
            continue;
        size_t msz = (a->n + 1) * (np + 1);
        int8_t mstack[512];
        int8_t *memo = msz <= sizeof(mstack) ? mstack : xmalloc(msz);
        memset(memo, -1, msz);
        ok = seg_match_rec(a, 0, ps, np, 0, g->flags, memo);
        if (memo != mstack) free(memo);
    }
    if (ps != stackbuf) free(ps);
    return ok;
}

bool glob_match(const char *pattern, const char *path, int flags) {
    glob_pat_t *g = glob_compile(pattern, flags);
    bool r = glob_pat_match(g, path);
    glob_pat_free(g);
    return r;
}

bool glob_has_magic(const char *p) {
    for (; p && *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '*' || *p == '?' || *p == '[' || *p == '{') return true;
    }
    return false;
}

char *glob_normalize(const char *glob, bool recursive) {
    char *g = str_replace(glob ? glob : "", "\\", "/");
    size_t n = strlen(g);
    bool exact_union = n >= 2 && g[0] == '{' && brace_close(g, 0) == (ptrdiff_t)(n - 1);
    char *out;
    if (!recursive || strchr(g, '/') || str_starts(g, "**") || exact_union) out = g;
    else {
        out = xasprintf("**/%s", g);
        free(g);
    }
    int depth = 0;
    for (const char *p = out; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}' && depth > 0) depth--;
    }
    if (depth > 0) {
        sb_t b = SB_INIT;
        sb_append(&b, out);
        for (int i = 0; i < depth; i++) sb_putc(&b, '}');
        free(out);
        out = sb_detach(&b);
    }
    return out;
}

int glob_depth_bound(const char *pattern) {
    if (strstr(pattern, "**") || strchr(pattern, '{')) return -1;
    int n = 0;
    for (const char *p = pattern; *p;) {
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l) n++;
        p += l;
        if (*p == '/') p++;
    }
    return n > 0 ? n : 1;
}

void globset_add(globset_t *s, const char *pattern, int flags) { vec_push(&s->pats, glob_compile(pattern, flags)); }

bool globset_match(const globset_t *s, const char *path) {
    for (size_t i = 0; i < s->pats.n; i++)
        if (glob_pat_match(s->pats.v[i], path)) return true;
    return false;
}

void globset_free(globset_t *s) {
    for (size_t i = 0; i < s->pats.n; i++) glob_pat_free(s->pats.v[i]);
    vec_free(&s->pats);
}

/* ---------------- gitignore ---------------- */

typedef struct {
    glob_pat_t *g;
    bool negate, dir_only, basename;
} irule_t;

struct ignore_set {
    VEC(irule_t) rules;
};

ignore_set_t *ignore_set_new(void) { return xcalloc(1, sizeof(ignore_set_t)); }

void ignore_set_free(ignore_set_t *s) {
    if (!s) return;
    for (size_t i = 0; i < s->rules.n; i++) glob_pat_free(s->rules.v[i].g);
    vec_free(&s->rules);
    free(s);
}

size_t ignore_set_count(const ignore_set_t *s) { return s ? s->rules.n : 0; }

void ignore_set_add_line(ignore_set_t *s, const char *line, size_t len) {
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (len == 0 || line[0] == '#') return;
    while (len > 0 && line[len - 1] == ' ' && !(len >= 2 && line[len - 2] == '\\')) len--;
    if (len == 0) return;
    irule_t r = {0};
    size_t i = 0;
    if (line[0] == '!') { r.negate = true; i = 1; }
    else if (line[0] == '\\' && len > 1 && (line[1] == '!' || line[1] == '#')) i = 1;
    char *pat = xstrndup(line + i, len - i);
    size_t pl = strlen(pat);
    while (pl > 0 && pat[pl - 1] == '/') { r.dir_only = true; pat[--pl] = 0; }
    if (pl == 0) { free(pat); return; }
    const char *body = pat;
    bool anchored = strchr(pat, '/') != NULL;
    if (body[0] == '/') body++;
    if (!*body) { free(pat); return; }
    r.basename = !anchored;
    r.g = glob_compile(body, GLOB_PATHNAME | GLOB_NOBRACE);
    free(pat);
    vec_push(&s->rules, r);
}

void ignore_set_add_text(ignore_set_t *s, const char *text, size_t len) {
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text += 3;
        len -= 3;
    }
    size_t i = 0;
    while (i < len) {
        const char *nl = memchr(text + i, '\n', len - i);
        size_t e = nl ? (size_t)(nl - text) : len;
        ignore_set_add_line(s, text + i, e - i);
        i = e + 1;
    }
}

bool ignore_set_add_file(ignore_set_t *s, const char *path) {
    size_t len;
    char *data = fs_read_limit(path, 8u << 20, &len, NULL);
    if (!data) return false;
    ignore_set_add_text(s, data, len);
    free(data);
    return true;
}

ignore_result_t ignore_set_match(const ignore_set_t *s, const char *relpath, bool is_dir) {
    if (!s || !relpath) return IGNORE_NONE;
    while (relpath[0] == '.' && relpath[1] == '/') relpath += 2;
    const char *base = strrchr(relpath, '/');
    base = base ? base + 1 : relpath;
    for (size_t i = s->rules.n; i-- > 0;) {
        const irule_t *r = &s->rules.v[i];
        if (r->dir_only && !is_dir) continue;
        if (glob_pat_match(r->g, r->basename ? base : relpath)) return r->negate ? IGNORE_WHITELISTED : IGNORE_IGNORED;
    }
    return IGNORE_NONE;
}

ignore_result_t ignore_set_match_parents(const ignore_set_t *s, const char *relpath, bool is_dir) {
    if (!s || !relpath) return IGNORE_NONE;
    size_t n = strlen(relpath);
    char *buf = xstrdup(relpath);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        ignore_result_t r = ignore_set_match(s, buf, true);
        buf[i] = '/';
        if (r == IGNORE_IGNORED) { free(buf); return r; }
    }
    free(buf);
    return ignore_set_match(s, relpath, is_dir);
}
