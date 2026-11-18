/* fuzzy.c - edit-engine normalization + fuzzy matching (port of omp pi-edit text.rs / fuzzy.rs). */
#include "fuzzy.h"

#include <math.h>

/* ---------- UTF-8 / JS string helpers ---------- */

bool fz_is_js_ws(uint32_t c) {
    return c == '\t' || c == '\n' || c == 0x0b || c == 0x0c || c == '\r' || c == ' ' || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 ||
           c == 0xfeff;
}

/* decode the code point that ends right before byte offset `end`, never starting before `lo` */
static size_t prev_cp(const char *s, size_t lo, size_t end, uint32_t *cp) {
    size_t b = end - 1;
    while (b > lo && ((unsigned char)s[b] & 0xc0) == 0x80 && end - b < 4) b--;
    if ((size_t)utf8_decode(s + b, end - b, cp) != end - b) {
        /* no whole sequence ends at `end` (a stray continuation byte): the last byte stands alone (U+FFFD) */
        b = end - 1;
        utf8_decode(s + b, 1, cp);
    }
    return b;
}

void fz_trim_span(const char *s, size_t len, size_t *start, size_t *end) {
    size_t a = 0, e = len;
    while (a < e) {
        uint32_t cp;
        int k = utf8_decode(s + a, e - a, &cp);
        if (!fz_is_js_ws(cp)) break;
        a += (size_t)k;
    }
    while (e > a) {
        uint32_t cp;
        size_t b = prev_cp(s, a, e, &cp);
        if (!fz_is_js_ws(cp)) break;
        e = b;
    }
    *start = a;
    *end = e;
}

char *fz_trim(const char *s) {
    size_t a, e;
    fz_trim_span(s, strlen(s), &a, &e);
    return xstrndup(s + a, e - a);
}

char *fz_trim_end(const char *s) {
    size_t e = strlen(s);
    while (e > 0) {
        uint32_t cp;
        size_t b = prev_cp(s, 0, e, &cp);
        if (!fz_is_js_ws(cp)) break;
        e = b;
    }
    return xstrndup(s, e);
}

const char *fz_trim_start(const char *s) {
    size_t len = strlen(s), a = 0;
    while (a < len) {
        uint32_t cp;
        int k = utf8_decode(s + a, len - a, &cp);
        if (!fz_is_js_ws(cp)) break;
        a += (size_t)k;
    }
    return s + a;
}

bool fz_trim_eq(const char *a, const char *b) {
    size_t a0, a1, b0, b1;
    fz_trim_span(a, strlen(a), &a0, &a1);
    fz_trim_span(b, strlen(b), &b0, &b1);
    return a1 - a0 == b1 - b0 && memcmp(a + a0, b + b0, a1 - a0) == 0;
}

bool fz_blank(const char *s) {
    size_t a, e;
    fz_trim_span(s, strlen(s), &a, &e);
    return a == e;
}

size_t fz_leading_ws(const char *s) {
    size_t n = 0;
    while (s[n] == ' ' || s[n] == '\t') n++;
    return n;
}

size_t fz_utf16_len(const char *s) {
    size_t n = 0, len = strlen(s);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += (size_t)utf8_decode(s + i, len - i, &cp);
        n += cp >= 0x10000 ? 2 : 1;
    }
    return n;
}

size_t fz_char_count(const char *s) {
    size_t n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xc0) != 0x80) n++;
    return n;
}

bool fz_is_ascii(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return false;
    return true;
}

uint32_t *fz_decode(const char *s, size_t *n) {
    size_t len = strlen(s);
    uint32_t *v = xmalloc((len + 1) * sizeof(uint32_t));
    size_t k = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += (size_t)utf8_decode(s + i, len - i, &cp);
        v[k++] = cp;
    }
    *n = k;
    return v;
}

strvec_t fz_split(const char *s) {
    strvec_t v = {0};
    const char *p = s;
    for (;;) {
        const char *nl = strchr(p, '\n');
        if (!nl) {
            strvec_push(&v, p);
            break;
        }
        strvec_push_own(&v, xstrndup(p, (size_t)(nl - p)));
        p = nl + 1;
    }
    return v;
}

char *fz_normalize_to_lf(const char *s) {
    sb_t b = SB_INIT;
    for (; *s; s++) {
        if (*s == '\r') {
            if (s[1] == '\n') s++;
            sb_putc(&b, '\n');
        } else {
            sb_putc(&b, *s);
        }
    }
    return sb_detach(&b);
}

static const char *unicode_replacement(uint32_t cp) {
    if ((cp >= 0x2010 && cp <= 0x2015) || cp == 0x2212) return "-";
    if (cp >= 0x2018 && cp <= 0x201b) return "'";
    if (cp >= 0x201c && cp <= 0x201f) return "\"";
    if (cp == 0xa0 || (cp >= 0x2002 && cp <= 0x200a) || cp == 0x202f || cp == 0x205f || cp == 0x3000) return " ";
    if (cp == 0x2260) return "!=";
    if (cp == 0xbd) return "1/2";
    if ((cp >= 0x200b && cp <= 0x200d) || cp == 0x2016 || cp == 0x2017 || cp == 0xfeff) return "";
    return NULL;
}

char *fz_normalize_unicode(const char *s) {
    char *t = fz_trim(s);
    if (fz_is_ascii(t)) return t;
    sb_t b = SB_INIT;
    size_t len = strlen(t);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int k = utf8_decode(t + i, len - i, &cp);
        const char *r = unicode_replacement(cp);
        if (r) sb_append(&b, r);
        else sb_appendn(&b, t + i, (size_t)k);
        i += (size_t)k;
    }
    free(t);
    return sb_detach(&b);
}

char *fz_normalize_for_fuzzy(const char *s) {
    size_t a, e;
    fz_trim_span(s, strlen(s), &a, &e);
    sb_t b = SB_INIT;
    bool in_space = false;
    for (size_t i = a; i < e;) {
        uint32_t cp;
        int k = utf8_decode(s + i, e - i, &cp);
        uint32_t m = cp;
        if (cp == '"' || cp == 0x201e || cp == 0x201f || cp == 0xab || cp == 0xbb) m = '"';
        else if (cp == '\'' || cp == 0x201a || cp == 0x201b || cp == '`' || cp == 0xb4) m = '\'';
        else if ((cp >= 0x2010 && cp <= 0x2014) || cp == 0x2212) m = '-';
        else if (cp == ' ' || cp == '\t') m = ' ';
        if (m == ' ') {
            if (!in_space) sb_putc(&b, ' ');
            in_space = true;
        } else {
            in_space = false;
            if (m != cp) sb_append_utf8(&b, m);
            else sb_appendn(&b, s + i, (size_t)k);
        }
        i += (size_t)k;
    }
    return sb_detach(&b);
}

char *fz_convert_leading_tabs(const char *text, size_t spaces_per_tab) {
    if (!spaces_per_tab) return xstrdup(text);
    strvec_t lines = fz_split(text);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < lines.n; i++) {
        const char *line = lines.v[i];
        if (i) sb_putc(&b, '\n');
        const char *trimmed = fz_trim_start(line);
        size_t lw = fz_leading_ws(line);
        bool has_tab = memchr(line, '\t', lw) != NULL, has_sp = memchr(line, ' ', lw) != NULL;
        if (!*trimmed || !has_tab || has_sp) {
            sb_append(&b, line);
            continue;
        }
        for (size_t k = 0; k < lw * spaces_per_tab; k++) sb_putc(&b, ' ');
        sb_append(&b, trimmed);
    }
    strvec_free(&lines);
    return sb_detach(&b);
}

/* ---------- indentation profile / adjust_indentation ---------- */

typedef struct {
    strvec_t lines;
    int ch; /* 0 = none */
    bool space_only, tab_only, mixed;
    size_t unit, non_empty;
} iprof_t;

static size_t gcd_sz(size_t a, size_t b) {
    while (b) {
        size_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

static void iprof_build(iprof_t *p, const char *text) {
    memset(p, 0, sizeof(*p));
    p->lines = fz_split(text);
    p->space_only = p->tab_only = true;
    VEC(size_t) counts = {0};
    for (size_t i = 0; i < p->lines.n; i++) {
        const char *l = p->lines.v[i];
        if (fz_blank(l)) continue;
        p->non_empty++;
        size_t w = fz_leading_ws(l);
        vec_push(&counts, w);
        bool sp = memchr(l, ' ', w) != NULL, tb = memchr(l, '\t', w) != NULL;
        if (sp) p->tab_only = false;
        if (tb) p->space_only = false;
        if (sp && tb) p->mixed = true;
        if (w) {
            if (!p->ch) p->ch = l[0];
            else if (p->ch != l[0]) p->mixed = true;
        }
    }
    if (p->space_only && p->non_empty) {
        size_t cur = 0;
        for (size_t i = 0; i < counts.n; i++) {
            if (!counts.v[i]) continue;
            cur = cur ? gcd_sz(cur, counts.v[i]) : counts.v[i];
        }
        p->unit = cur;
    }
    if (p->tab_only && p->non_empty) p->unit = 1;
    vec_free(&counts);
}

static char *apply_indent_delta_text(const char *text, long delta, char ch) {
    strvec_t lines = fz_split(text);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        if (i) sb_putc(&b, '\n');
        if (fz_blank(l)) {
            sb_append(&b, l);
            continue;
        }
        if (delta > 0) {
            for (long k = 0; k < delta; k++) sb_putc(&b, ch);
            sb_append(&b, l);
        } else {
            size_t rm = MIN((size_t)(-delta), fz_leading_ws(l));
            sb_append(&b, l + rm);
        }
    }
    strvec_free(&lines);
    return sb_detach(&b);
}

char *fz_adjust_indentation(const char *old_text, const char *actual_text, const char *new_text) {
    if (!strcmp(old_text, actual_text)) return xstrdup(new_text);
    {
        strvec_t ol = fz_split(old_text), nl = fz_split(new_text);
        bool only = ol.n == nl.n;
        for (size_t i = 0; only && i < ol.n; i++) only = fz_trim_eq(ol.v[i], nl.v[i]);
        strvec_free(&ol);
        strvec_free(&nl);
        if (only) return xstrdup(new_text);
    }
    iprof_t o, a, n;
    iprof_build(&o, old_text);
    iprof_build(&a, actual_text);
    iprof_build(&n, new_text);
    char *res = NULL;
    if (!o.non_empty || !a.non_empty || !n.non_empty || o.mixed || a.mixed || n.mixed) goto keep;
    if (o.ch && a.ch && o.ch != a.ch) {
        if (!a.space_only || !o.tab_only || !n.tab_only || !a.unit) goto keep;
        for (size_t i = 0; i < o.lines.n && i < a.lines.n; i++) {
            const char *ol = o.lines.v[i], *al = a.lines.v[i];
            if (fz_blank(ol) || fz_blank(al)) continue;
            size_t ow = fz_leading_ws(ol);
            if (!ow) continue;
            if (fz_leading_ws(al) != ow * a.unit) goto keep;
        }
        res = fz_convert_leading_tabs(new_text, a.unit);
        goto done;
    }
    {
        bool have = false;
        long delta = 0;
        for (size_t i = 0; i < o.lines.n && i < a.lines.n; i++) {
            const char *ol = o.lines.v[i], *al = a.lines.v[i];
            if (fz_blank(ol) || fz_blank(al)) continue;
            long cur = (long)fz_leading_ws(al) - (long)fz_leading_ws(ol);
            if (!have) {
                have = true;
                delta = cur;
            } else if (delta != cur) {
                goto keep;
            }
        }
        if (!have || delta == 0) goto keep;
        if (n.ch && a.ch && n.ch != a.ch) goto keep;
        char ch = a.ch ? (char)a.ch : o.ch ? (char)o.ch : ' ';
        if (!a.ch && !o.ch) {
            /* detect_indent_char(actual_text) */
            for (size_t i = 0; i < a.lines.n; i++)
                if (fz_leading_ws(a.lines.v[i])) {
                    ch = a.lines.v[i][0];
                    break;
                }
        }
        res = apply_indent_delta_text(new_text, delta, ch);
        goto done;
    }
keep:
    res = xstrdup(new_text);
done:
    strvec_free(&o.lines);
    strvec_free(&a.lines);
    strvec_free(&n.lines);
    return res;
}

/* ---------- Levenshtein / similarity ---------- */

size_t fz_levenshtein(const uint32_t *a, size_t na, const uint32_t *b, size_t nb) {
    size_t start = 0, lim = MIN(na, nb);
    while (start < lim && a[start] == b[start]) start++;
    size_t ae = na, be = nb;
    while (ae > start && be > start && a[ae - 1] == b[be - 1]) {
        ae--;
        be--;
    }
    const uint32_t *lo = a + start, *sh = b + start;
    size_t ln = ae - start, sn = be - start;
    if (!ln) return sn;
    if (!sn) return ln;
    if (sn > ln) {
        const uint32_t *t = lo;
        lo = sh;
        sh = t;
        size_t tn = ln;
        ln = sn;
        sn = tn;
    }
    size_t *row = xmalloc((sn + 1) * sizeof(size_t));
    for (size_t i = 0; i <= sn; i++) row[i] = i;
    for (size_t i = 0; i < ln; i++) {
        size_t diag = row[0];
        row[0] = i + 1;
        for (size_t j = 0; j < sn; j++) {
            size_t above = row[j + 1];
            if (lo[i] == sh[j]) row[j + 1] = diag;
            else row[j + 1] = MIN(MIN(above + 1, row[j] + 1), diag + 1);
            diag = above;
        }
    }
    size_t d = row[sn];
    free(row);
    return d;
}

double fz_similarity(const char *a, const char *b) {
    if (!strcmp(a, b)) return 1.0;
    if (!*a || !*b) return 0.0;
    size_t na, nb;
    uint32_t *ua = fz_decode(a, &na), *ub = fz_decode(b, &nb);
    size_t d = fz_levenshtein(ua, na, ub, nb);
    free(ua);
    free(ub);
    return 1.0 - (double)d / (double)MAX(na, nb);
}

/* ---------- find_match ---------- */

static void format_preview_window(sb_t *b, char *const *lines, size_t n, size_t center) {
    size_t start = center >= 5 ? center - 5 : 0;
    size_t end = MIN(n, center + 5 + 1);
    for (size_t i = start; i < end; i++) {
        if (i > start) sb_putc(b, '\n');
        const char *line = lines[i];
        sb_appendf(b, "  %zu | ", i + 1);
        if (fz_utf16_len(line) > 80) {
            size_t units = 0, len = strlen(line);
            for (size_t k = 0; k < len;) {
                uint32_t cp;
                int w = utf8_decode(line + k, len - k, &cp);
                size_t u = cp >= 0x10000 ? 2 : 1;
                if (units + u > 79) break;
                sb_appendn(b, line + k, (size_t)w);
                units += u;
                k += (size_t)w;
            }
            sb_append(b, "…");
        } else {
            sb_append(b, line);
        }
    }
}

static bool overlaps(size_t s, size_t e, const fz_range_t *r, size_t nr) {
    for (size_t i = 0; i < nr; i++)
        if (s < r[i].end && e > r[i].start) return true;
    return false;
}

static uint32_t line_of(const char *content, size_t idx) {
    uint32_t n = 1;
    for (size_t i = 0; i < idx; i++)
        if (content[i] == '\n') n++;
    return n;
}

static fz_match_t *match_new(const char *text, size_t len, size_t start, uint32_t line, double conf) {
    fz_match_t *m = xcalloc(1, sizeof(*m));
    m->actual_text = xstrndup(text, len);
    m->start_index = start;
    m->start_line = line;
    m->confidence = conf;
    return m;
}

static void match_free(fz_match_t *m) {
    if (!m) return;
    free(m->actual_text);
    free(m);
}

static fz_match_t *match_clone(const fz_match_t *m) {
    return m ? match_new(m->actual_text, strlen(m->actual_text), m->start_index, m->start_line, m->confidence) : NULL;
}

void fz_outcome_free(fz_outcome_t *o) {
    match_free(o->matched);
    match_free(o->closest);
    strvec_free(&o->occurrence_previews);
    memset(o, 0, sizeof(*o));
    o->occurrences = o->fuzzy_matches = -1;
}

static bool exact_outcome(const char *content, const char *target, const fz_range_t *ex, size_t nex, fz_outcome_t *out) {
    size_t clen = strlen(content), tlen = strlen(target);
    bool have = false;
    size_t first = 0, occ = 0, rec[FZ_MAX_RECORDED], nrec = 0;
    size_t ss = 0;
    while (ss <= (clen > tlen ? clen - tlen : 0)) {
        const char *f = strstr(content + ss, target);
        if (!f) break;
        size_t idx = (size_t)(f - content), end = idx + tlen;
        if (!overlaps(idx, end, ex, nex)) {
            if (!have) {
                have = true;
                first = idx;
            }
            occ++;
            if (nrec < FZ_MAX_RECORDED) rec[nrec++] = idx;
        }
        ss = end;
        if (!tlen) break;
    }
    if (!have) return false;
    if (occ > 1) {
        strvec_t cl = fz_split(content);
        out->occurrences = (int64_t)occ;
        for (size_t i = 0; i < nrec; i++) {
            uint32_t ln = line_of(content, rec[i]);
            sb_t b = SB_INIT;
            format_preview_window(&b, cl.v, cl.n, ln - 1);
            strvec_push_own(&out->occurrence_previews, sb_detach(&b));
        }
        strvec_free(&cl);
        return true;
    }
    out->matched = match_new(target, tlen, first, line_of(content, first), 1.0);
    return true;
}

typedef struct {
    fz_match_t *best;
    size_t above;
    double second;
} bestfz_t;

static void best_core(char *const *cl, size_t cn, char *const *norm_c, char *const *tl, size_t tn, char *const *norm_t,
                      const size_t *offsets, double threshold, bool depth, const fz_range_t *ex, size_t nex,
                      bestfz_t *r) {
    /* target normalized lines */
    char **tnorm = xmalloc(tn * sizeof(char *));
    size_t *tdepth = NULL;
    size_t *wdepth = xmalloc(tn * sizeof(size_t));
    if (depth) tdepth = xmalloc(tn * sizeof(size_t));
#define DEPTHS(lines, normv, out)                                                                \
    do {                                                                                         \
        size_t mi = SIZE_MAX, unit = SIZE_MAX;                                                   \
        for (size_t q = 0; q < tn; q++)                                                          \
            if (!fz_blank((lines)[q])) mi = MIN(mi, fz_leading_ws((lines)[q]));                  \
        if (mi == SIZE_MAX) mi = 0;                                                              \
        for (size_t q = 0; q < tn; q++)                                                          \
            if (!fz_blank((lines)[q])) {                                                         \
                size_t st = fz_leading_ws((lines)[q]) - mi;                                      \
                if (st > 0) unit = MIN(unit, st);                                                \
            }                                                                                    \
        if (unit == SIZE_MAX) unit = 1;                                                          \
        for (size_t q = 0; q < tn; q++)                                                          \
            (out)[q] = fz_blank((lines)[q]) ? 0                                                  \
                                            : (size_t)llround((double)(fz_leading_ws((lines)[q]) - mi) / (double)unit); \
    } while (0)
    if (depth) DEPTHS(tl, norm_t, tdepth);
    for (size_t q = 0; q < tn; q++)
        tnorm[q] = depth ? xasprintf("%zu|%s", tdepth[q], norm_t[q]) : xasprintf("|%s", norm_t[q]);
    double best_score = -1, second = -1;
    for (size_t s = 0; s + tn <= cn; s++) {
        size_t si = offsets[s], el = s + tn - 1;
        size_t ei = MAX(offsets[el] + strlen(cl[el]), si + 1);
        if (overlaps(si, ei, ex, nex)) continue;
        char *const *win = cl + s;
        if (depth) DEPTHS(win, NULL, wdepth);
        double sum = 0;
        for (size_t q = 0; q < tn; q++) {
            char *w = depth ? xasprintf("%zu|%s", wdepth[q], norm_c[s + q]) : xasprintf("|%s", norm_c[s + q]);
            sum += fz_similarity(tnorm[q], w);
            free(w);
        }
        double score = sum / (double)tn;
        if (score >= threshold) r->above++;
        if (score > best_score) {
            second = best_score;
            best_score = score;
            sb_t b = SB_INIT;
            for (size_t q = 0; q < tn; q++) {
                if (q) sb_putc(&b, '\n');
                sb_append(&b, win[q]);
            }
            match_free(r->best);
            r->best = xcalloc(1, sizeof(fz_match_t));
            r->best->actual_text = sb_detach(&b);
            r->best->start_index = si;
            r->best->start_line = (uint32_t)s + 1;
            r->best->confidence = score;
        } else if (score > second) {
            second = score;
        }
    }
#undef DEPTHS
    r->second = second;
    for (size_t q = 0; q < tn; q++) free(tnorm[q]);
    free(tnorm);
    free(tdepth);
    free(wdepth);
}

static void best_fuzzy(const char *content, const char *target, double threshold, const fz_range_t *ex, size_t nex,
                       bestfz_t *r) {
    memset(r, 0, sizeof(*r));
    strvec_t cl = fz_split(content), tl = fz_split(target);
    if (!*target || tl.n > cl.n) {
        strvec_free(&cl);
        strvec_free(&tl);
        return;
    }
    size_t *offsets = xmalloc(cl.n * sizeof(size_t));
    size_t off = 0;
    for (size_t i = 0; i < cl.n; i++) {
        offsets[i] = off;
        off += strlen(cl.v[i]) + (i + 1 < cl.n);
    }
    char **nc = xmalloc(cl.n * sizeof(char *)), **nt = xmalloc(tl.n * sizeof(char *));
    for (size_t i = 0; i < cl.n; i++) nc[i] = fz_normalize_for_fuzzy(cl.v[i]);
    for (size_t i = 0; i < tl.n; i++) nt[i] = fz_normalize_for_fuzzy(tl.v[i]);
    best_core(cl.v, cl.n, nc, tl.v, tl.n, nt, offsets, threshold, true, ex, nex, r);
    if (r->best && r->best->confidence < threshold && r->best->confidence >= FZ_FALLBACK_THRESHOLD) {
        bestfz_t w = {0};
        best_core(cl.v, cl.n, nc, tl.v, tl.n, nt, offsets, threshold, false, ex, nex, &w);
        if (w.best && (!r->best || w.best->confidence > r->best->confidence)) {
            match_free(r->best);
            *r = w;
        } else {
            match_free(w.best);
        }
    }
    for (size_t i = 0; i < cl.n; i++) free(nc[i]);
    for (size_t i = 0; i < tl.n; i++) free(nt[i]);
    free(nc);
    free(nt);
    free(offsets);
    strvec_free(&cl);
    strvec_free(&tl);
}

void fz_find_match(const char *content, const char *target, bool allow_fuzzy, double threshold,
                   const fz_range_t *ex, size_t nex, fz_outcome_t *out) {
    memset(out, 0, sizeof(*out));
    out->occurrences = out->fuzzy_matches = -1;
    if (!*target) return;
    if (exact_outcome(content, target, ex, nex, out)) return;
    bestfz_t r;
    best_fuzzy(content, target, threshold, ex, nex, &r);
    if (!r.best) return;
    if (allow_fuzzy && r.best->confidence >= threshold) {
        if (r.above == 1) {
            out->matched = r.best;
            out->closest = match_clone(r.best);
            return;
        }
        if (r.above > 1 && r.best->confidence >= FZ_DOMINANT_MIN && r.best->confidence - r.second >= FZ_DOMINANT_DELTA) {
            out->matched = r.best;
            out->closest = match_clone(r.best);
            out->fuzzy_matches = (int64_t)r.above;
            out->dominant_fuzzy = true;
            return;
        }
    }
    out->closest = r.best;
    out->fuzzy_matches = (int64_t)r.above;
}

/* ---------- sequence search ---------- */

const char *fz_seq_label(int s) {
    static const char *const L[] = {"exact",  "trim-trailing", "trim",  "comment-prefix", "unicode",
                                    "prefix", "substring",     "fuzzy", "fuzzy-dominant", "character"};
    return s >= 0 && s < (int)ARRAY_LEN(L) ? L[s] : "";
}

const char *fz_ctx_label(int s) {
    static const char *const L[] = {"exact", "trim", "unicode", "prefix", "substring", "fuzzy"};
    return s >= 0 && s < (int)ARRAY_LEN(L) ? L[s] : "";
}

static fz_seq_t seq_none(double conf, int64_t count) {
    fz_seq_t r;
    memset(&r, 0, sizeof(r));
    r.index = -1;
    r.confidence = conf;
    r.match_count = count;
    r.strategy = -1;
    return r;
}

static const char *strip_comment_prefix(const char *line) {
    const char *t = fz_trim_start(line);
    if (str_starts(t, "/*") || str_starts(t, "*/") || str_starts(t, "//")) t += 2;
    else if (*t == '*' || *t == '#' || *t == ';') t += 1;
    else if (str_starts(t, "/ ")) t += 2;
    return fz_trim_start(t);
}

enum { XF_TRIM_END = 1, XF_TRIM, XF_COMMENT, XF_UNICODE };

static char **xform(char *const *v, size_t n, int kind) {
    char **o = xmalloc((n ? n : 1) * sizeof(char *));
    for (size_t i = 0; i < n; i++) {
        switch (kind) {
        case XF_TRIM_END: o[i] = fz_trim_end(v[i]); break;
        case XF_TRIM: o[i] = fz_trim(v[i]); break;
        case XF_COMMENT: o[i] = xstrdup(strip_comment_prefix(v[i])); break;
        default: o[i] = fz_normalize_unicode(v[i]); break;
        }
    }
    return o;
}

static void xfree(char **v, size_t n) {
    if (!v) return;
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

enum { CMP_EQ, CMP_PREFIX, CMP_INCLUDES };

static bool norm_includes(const char *line, const char *pat) {
    if (!*pat) return !*line;
    size_t pl = fz_char_count(pat), ll = fz_char_count(line);
    return pl >= FZ_PARTIAL_MIN_LENGTH && strstr(line, pat) && (double)pl / (double)MAX(ll, (size_t)1) >= FZ_PARTIAL_MIN_RATIO;
}

static bool cmp_line(const char *a, const char *b, int how) {
    switch (how) {
    case CMP_PREFIX: return *b ? str_starts(a, b) : !*a;
    case CMP_INCLUDES: return norm_includes(a, b);
    default: return !strcmp(a, b);
    }
}

typedef struct {
    bool have;
    size_t first, count, n;
    size_t idx[FZ_MAX_RECORDED];
} imatch_t;

static imatch_t collect(char *const *L, char *const *P, size_t m, size_t from, size_t to, int how) {
    imatch_t r;
    memset(&r, 0, sizeof(r));
    if (from > to) return r;
    for (size_t i = from; i <= to; i++) {
        bool ok = true;
        for (size_t k = 0; k < m && ok; k++) ok = cmp_line(L[i + k], P[k], how);
        if (!ok) continue;
        if (!r.have) {
            r.have = true;
            r.first = i;
        }
        r.count++;
        if (r.n < FZ_MAX_RECORDED) r.idx[r.n++] = i;
    }
    return r;
}

static bool seq_result(const imatch_t *m, double conf, int strategy, bool ambiguous, fz_seq_t *out) {
    if (!m->have) return false;
    *out = seq_none(conf, -1);
    out->index = (int64_t)m->first;
    out->strategy = strategy;
    if (ambiguous) {
        out->match_count = (int64_t)m->count;
        out->has_indices = true;
        out->nindices = m->n;
        memcpy(out->indices, m->idx, sizeof(m->idx));
    }
    return true;
}

typedef struct {
    char *const *lines;
    size_t n;
    char *const *pat;
    size_t m;
    char **lx[5], **px[5];
    char **ln, **pn; /* normalize_for_fuzzy */
} seqctx_t;

static void seqctx_need(seqctx_t *c, int kind) {
    if (!c->lx[kind]) {
        c->lx[kind] = xform(c->lines, c->n, kind);
        c->px[kind] = xform(c->pat, c->m, kind);
    }
}

static void seqctx_free(seqctx_t *c) {
    for (int k = 0; k < 5; k++) {
        xfree(c->lx[k], c->n);
        xfree(c->px[k], c->m);
    }
    xfree(c->ln, c->n);
    xfree(c->pn, c->m);
}

static bool run_passes(seqctx_t *c, size_t from, size_t to, bool allow_fuzzy, fz_seq_t *out) {
    imatch_t r = collect(c->lines, c->pat, c->m, from, to, CMP_EQ);
    if (seq_result(&r, 1.0, SEQ_EXACT, false, out)) return true;
    static const struct {
        int kind, strat;
        double conf;
    } P[] = {{XF_TRIM_END, SEQ_TRIM_TRAILING, 0.99},
             {XF_TRIM, SEQ_TRIM, 0.98},
             {XF_COMMENT, SEQ_COMMENT_PREFIX, 0.975},
             {XF_UNICODE, SEQ_UNICODE, 0.97}};
    for (size_t i = 0; i < ARRAY_LEN(P); i++) {
        seqctx_need(c, P[i].kind);
        r = collect(c->lx[P[i].kind], c->px[P[i].kind], c->m, from, to, CMP_EQ);
        if (seq_result(&r, P[i].conf, P[i].strat, false, out)) return true;
    }
    if (!allow_fuzzy) return false;
    r = collect(c->ln, c->pn, c->m, from, to, CMP_PREFIX);
    if (seq_result(&r, 0.965, SEQ_PREFIX, true, out)) return true;
    r = collect(c->ln, c->pn, c->m, from, to, CMP_INCLUDES);
    return seq_result(&r, 0.94, SEQ_SUBSTRING, true, out);
}

static double fuzzy_score_at(char *const *lines, char *const *pat, size_t m, size_t index, double min_score) {
    double total = 0;
    for (size_t off = 0; off < m; off++) {
        const char *line = lines[index + off], *p = pat[off];
        if (!strcmp(line, p)) {
            total += 1.0;
            continue;
        }
        double remaining = (double)(m - off - 1);
        size_t ll = fz_char_count(line), pl = fz_char_count(p), mx = MAX(ll, pl);
        double ub = mx == 0 ? 1.0 : 1.0 - (double)(ll > pl ? ll - pl : pl - ll) / (double)mx;
        if ((total + ub + remaining) / (double)m < min_score) return total / (double)m;
        if (ub > 0.0) total += fz_similarity(line, p);
        if ((total + remaining) / (double)m < min_score) return total / (double)m;
    }
    return total / (double)m;
}

static char *join_lines(char *const *v, size_t n, const char *sep) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        if (i) sb_append(&b, sep);
        sb_append(&b, v[i]);
    }
    return sb_detach(&b);
}

fz_seq_t fz_seek_sequence(char *const *lines, size_t n, char *const *pattern, size_t m, size_t start, bool eof,
                          bool allow_fuzzy) {
    if (!m) {
        fz_seq_t r = seq_none(1.0, -1);
        r.index = (int64_t)start;
        r.strategy = SEQ_EXACT;
        return r;
    }
    if (m > n) return seq_none(0.0, -1);
    size_t max_start = n - m, search_start = eof ? max_start : start;
    seqctx_t c;
    memset(&c, 0, sizeof(c));
    c.lines = lines;
    c.n = n;
    c.pat = pattern;
    c.m = m;
    c.ln = xmalloc(n * sizeof(char *));
    c.pn = xmalloc(m * sizeof(char *));
    for (size_t i = 0; i < n; i++) c.ln[i] = fz_normalize_for_fuzzy(lines[i]);
    for (size_t i = 0; i < m; i++) c.pn[i] = fz_normalize_for_fuzzy(pattern[i]);
    fz_seq_t out;
    if (run_passes(&c, search_start, max_start, allow_fuzzy, &out)) goto done;
    if (eof && search_start > start && run_passes(&c, start, max_start, allow_fuzzy, &out)) goto done;
    if (!allow_fuzzy) {
        out = seq_none(0.0, -1);
        goto done;
    }
    {
        double best = 0, second = 0;
        int64_t best_index = -1;
        imatch_t fm;
        memset(&fm, 0, sizeof(fm));
        double bail = FZ_SEQUENCE_THRESHOLD - FZ_DOMINANT_DELTA;
        for (int pass = 0; pass < 2; pass++) {
            size_t from = pass ? start : search_start, to = pass ? search_start - 1 : max_start;
            if (pass && !(eof && search_start > start)) break;
            if (from > to) continue;
            for (size_t i = from; i <= to; i++) {
                double s = fuzzy_score_at(c.ln, c.pn, m, i, bail);
                if (s >= FZ_SEQUENCE_THRESHOLD) {
                    if (!fm.have) {
                        fm.have = true;
                        fm.first = i;
                    }
                    fm.count++;
                    if (fm.n < FZ_MAX_RECORDED) fm.idx[fm.n++] = i;
                }
                if (s > best) {
                    second = best;
                    best = s;
                    best_index = (int64_t)i;
                } else if (s > second) {
                    second = s;
                }
            }
        }
        if (best_index >= 0 && best >= FZ_SEQUENCE_THRESHOLD) {
            bool dominant = fm.count > 1 && best >= FZ_DOMINANT_MIN && best - second >= FZ_DOMINANT_DELTA;
            out = seq_none(best, dominant ? 1 : (int64_t)fm.count);
            out.index = best_index;
            out.has_indices = true;
            out.nindices = fm.n;
            memcpy(out.indices, fm.idx, sizeof(fm.idx));
            out.strategy = dominant ? SEQ_FUZZY_DOMINANT : SEQ_FUZZY;
            goto done;
        }
        char *pt = join_lines(pattern, m, "\n");
        char *ct = join_lines(lines + MIN(start, n), start < n ? n - start : 0, "\n");
        fz_outcome_t o;
        fz_find_match(ct, pt, true, FZ_CHARACTER_THRESHOLD, NULL, 0, &o);
        int64_t cnt = o.occurrences >= 0 ? o.occurrences : o.fuzzy_matches;
        if (o.matched) {
            out = seq_none(o.matched->confidence, cnt >= 0 ? cnt : 1);
            size_t nl = 0;
            for (size_t i = 0; i < o.matched->start_index; i++)
                if (ct[i] == '\n') nl++;
            out.index = (int64_t)(start + nl);
            out.strategy = SEQ_CHARACTER;
        } else {
            out = seq_none(best, cnt);
        }
        fz_outcome_free(&o);
        free(pt);
        free(ct);
    }
done:
    seqctx_free(&c);
    return out;
}

void fz_closest_sequence(char *const *lines, size_t n, char *const *pattern, size_t m, size_t start, bool eof,
                         int64_t *index, double *score) {
    *index = -1;
    *score = 0;
    if (!m) {
        *index = (int64_t)start;
        *score = 1.0;
        return;
    }
    if (m > n) return;
    size_t max_start = n - m, search_start = eof ? max_start : start;
    char **ln = xmalloc(n * sizeof(char *)), **pn = xmalloc(m * sizeof(char *));
    for (size_t i = 0; i < n; i++) ln[i] = fz_normalize_for_fuzzy(lines[i]);
    for (size_t i = 0; i < m; i++) pn[i] = fz_normalize_for_fuzzy(pattern[i]);
    double best = 0;
    for (size_t i = search_start; i <= max_start; i++) {
        double s = fuzzy_score_at(ln, pn, m, i, best);
        if (s > best) {
            best = s;
            *index = (int64_t)i;
        }
    }
    if (eof && search_start > start)
        for (size_t i = start; i < search_start; i++) {
            double s = fuzzy_score_at(ln, pn, m, i, best);
            if (s > best) {
                best = s;
                *index = (int64_t)i;
            }
        }
    *score = best;
    xfree(ln, n);
    xfree(pn, m);
}

static bool ctx_result(const imatch_t *m, double conf, int strategy, fz_seq_t *out) {
    if (!m->have) return false;
    *out = seq_none(conf, (int64_t)m->count);
    out->index = (int64_t)m->first;
    out->has_indices = true;
    out->nindices = m->n;
    memcpy(out->indices, m->idx, sizeof(m->idx));
    out->strategy = strategy;
    return true;
}

static void im_add(imatch_t *r, size_t i) {
    if (!r->have) {
        r->have = true;
        r->first = i;
    }
    r->count++;
    if (r->n < FZ_MAX_RECORDED) r->idx[r->n++] = i;
}

fz_seq_t fz_find_context_line(char *const *lines, size_t n, const char *context, size_t start_from, bool allow_fuzzy,
                              bool skip_function_fallback) {
    if (!n || start_from >= n) return seq_none(0.0, -1);
    fz_seq_t out;
    imatch_t r;
    memset(&r, 0, sizeof(r));
    for (size_t i = start_from; i < n; i++)
        if (!strcmp(lines[i], context)) im_add(&r, i);
    if (ctx_result(&r, 1.0, CTX_EXACT, &out)) return out;
    char *tctx = fz_trim(context);
    memset(&r, 0, sizeof(r));
    for (size_t i = start_from; i < n; i++)
        if (fz_trim_eq(lines[i], tctx)) im_add(&r, i);
    if (ctx_result(&r, 0.99, CTX_TRIM, &out)) goto done;
    {
        char *nc = fz_normalize_unicode(context);
        memset(&r, 0, sizeof(r));
        for (size_t i = start_from; i < n; i++) {
            char *nl = fz_normalize_unicode(lines[i]);
            if (!strcmp(nl, nc)) im_add(&r, i);
            free(nl);
        }
        free(nc);
        if (ctx_result(&r, 0.98, CTX_UNICODE, &out)) goto done;
    }
    if (!allow_fuzzy) {
        out = seq_none(0.0, -1);
        goto done;
    }
    {
        char *cn = fz_normalize_for_fuzzy(context);
        char **ln = xmalloc(n * sizeof(char *));
        for (size_t i = 0; i < n; i++) ln[i] = i >= start_from ? fz_normalize_for_fuzzy(lines[i]) : NULL;
        bool found = false;
        if (*cn) {
            memset(&r, 0, sizeof(r));
            for (size_t i = start_from; i < n; i++)
                if (str_starts(ln[i], cn)) im_add(&r, i);
            found = ctx_result(&r, 0.96, CTX_PREFIX, &out);
        }
        size_t clen = fz_char_count(cn);
        if (!found && clen >= FZ_PARTIAL_MIN_LENGTH) {
            VEC(size_t) all = {0};
            VEC(double) ratio = {0};
            for (size_t i = start_from; i < n; i++)
                if (strstr(ln[i], cn)) {
                    vec_push(&all, i);
                    vec_push(&ratio, (double)clen / (double)MAX(fz_char_count(ln[i]), (size_t)1));
                }
            fz_seq_t s = seq_none(0.94, -1);
            s.has_indices = true;
            s.strategy = CTX_SUBSTRING;
            for (size_t i = 0; i < all.n && s.nindices < FZ_MAX_RECORDED; i++) s.indices[s.nindices++] = all.v[i];
            if (all.n == 1) {
                s.index = (int64_t)all.v[0];
                s.match_count = 1;
                out = s;
                found = true;
            } else {
                size_t q = 0, qfirst = 0;
                for (size_t i = 0; i < all.n; i++)
                    if (ratio.v[i] >= FZ_PARTIAL_MIN_RATIO) {
                        if (!q) qfirst = all.v[i];
                        q++;
                    }
                if (q) {
                    s.index = (int64_t)qfirst;
                    s.match_count = (int64_t)q;
                    out = s;
                    found = true;
                } else if (all.n > 1) {
                    s.index = (int64_t)all.v[0];
                    s.match_count = (int64_t)all.n;
                    out = s;
                    found = true;
                }
            }
            vec_free(&all);
            vec_free(&ratio);
        }
        if (!found) {
            double best = 0;
            int64_t bi = -1;
            memset(&r, 0, sizeof(r));
            for (size_t i = start_from; i < n; i++) {
                double sc = fz_similarity(ln[i], cn);
                if (sc >= FZ_CONTEXT_THRESHOLD) im_add(&r, i);
                if (sc > best) {
                    best = sc;
                    bi = (int64_t)i;
                }
            }
            if (bi >= 0 && best >= FZ_CONTEXT_THRESHOLD) {
                out = seq_none(best, (int64_t)r.count);
                out.index = bi;
                out.has_indices = true;
                out.nindices = r.n;
                memcpy(out.indices, r.idx, sizeof(r.idx));
                out.strategy = CTX_FUZZY;
                found = true;
            } else if (!skip_function_fallback && str_ends(tctx, "()")) {
                char *base = xstrndup(tctx, strlen(tctx) - 2);
                char *wp = xasprintf("%s(", base);
                out = fz_find_context_line(lines, n, wp, start_from, allow_fuzzy, true);
                if (!(out.index >= 0 || out.match_count > 0)) out = fz_find_context_line(lines, n, base, start_from, allow_fuzzy, true);
                free(wp);
                free(base);
                found = true;
            } else {
                out = seq_none(best, -1);
                found = true;
            }
        }
        for (size_t i = 0; i < n; i++) free(ln[i]);
        free(ln);
        free(cn);
    }
done:
    free(tctx);
    return out;
}

/* ---------- replace helpers ---------- */

char *fz_format_match_error(const char *path, const char *search_text, const fz_match_t *closest, bool allow_fuzzy,
                            double threshold, int64_t fuzzy_matches) {
    if (!closest) {
        if (allow_fuzzy) return xasprintf("Could not find a close enough match in %s.", path);
        return xasprintf("Could not find the exact text in %s. The old text must match exactly including all whitespace "
                         "and newlines.",
                         path);
    }
    long long sim = llround(closest->confidence * 100.0), thr = llround(threshold * 100.0);
    strvec_t sl = fz_split(search_text), al = fz_split(closest->actual_text);
    const char *ol = NULL, *nl = NULL;
    for (size_t i = 0; i < MAX(sl.n, al.n); i++) {
        const char *o = i < sl.n ? sl.v[i] : "", *w = i < al.n ? al.v[i] : "";
        if (strcmp(o, w) != 0) {
            ol = o;
            nl = w;
            break;
        }
    }
    if (!ol) {
        ol = sl.n ? sl.v[0] : "";
        nl = al.n ? al.v[0] : "";
    }
    char *hint;
    if (allow_fuzzy) {
        if (fuzzy_matches > 1)
            hint = xasprintf("Found %lld high-confidence matches. Provide more context to make it unique.",
                             (long long)fuzzy_matches);
        else hint = xasprintf("Closest match was below the %lld%% similarity threshold.", thr);
    } else {
        hint = xstrdup("Fuzzy matching is disabled. Enable 'Edit fuzzy match' in settings to accept high-confidence "
                       "matches.");
    }
    char *r = xasprintf("%s %s.\n\nClosest match (%lld%% similar) at line %u:\n  - %s\n  + %s\n%s",
                        allow_fuzzy ? "Could not find a close enough match in" : "Could not find the exact text in", path,
                        sim, closest->start_line, ol, nl, hint);
    free(hint);
    strvec_free(&sl);
    strvec_free(&al);
    return r;
}

char *fz_format_occurrence_error(const char *path, const fz_outcome_t *o) {
    int64_t occ = o->occurrences > 0 ? o->occurrences : 0;
    char *prev = strvec_join(&o->occurrence_previews, "\n\n");
    char *more = occ > FZ_MAX_RECORDED ? xasprintf(" (showing first %d of %lld)", FZ_MAX_RECORDED, (long long)occ)
                                       : xstrdup("");
    char *r = xasprintf("Found %lld occurrences in %s%s:\n\n%s\n\nAdd more context lines to disambiguate.",
                        (long long)occ, path, more, prev);
    free(prev);
    free(more);
    return r;
}

typedef struct {
    size_t start, end;
    char *text;
} repl_t;

static int cmp_repl(const void *a, const void *b) {
    const repl_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

bool fz_replace_text(const char *content, const char *old_text, const char *new_text, bool fuzzy, bool all,
                     double threshold, char **out, size_t *count, fz_outcome_t *miss, char **err) {
    *out = NULL;
    *count = 0;
    *err = NULL;
    memset(miss, 0, sizeof(*miss));
    miss->occurrences = miss->fuzzy_matches = -1;
    if (!*old_text) {
        *err = xstrdup("oldText must not be empty.");
        return false;
    }
    char *nc = fz_normalize_to_lf(content), *no = fz_normalize_to_lf(old_text), *nn = fz_normalize_to_lf(new_text);
    bool ok = false;
    if (all) {
        size_t ex = str_count(nc, no);
        if (ex > 0) {
            *out = str_replace(nc, no, nn);
            *count = ex;
            ok = true;
            goto done;
        }
        VEC(repl_t) reps = {0};
        size_t clen = strlen(nc);
        for (;;) {
            fz_range_t *rg = xmalloc((reps.n + 1) * sizeof(fz_range_t));
            for (size_t i = 0; i < reps.n; i++) {
                rg[i].start = reps.v[i].start;
                rg[i].end = reps.v[i].end;
            }
            fz_outcome_t o;
            fz_find_match(nc, no, fuzzy, threshold, rg, reps.n, &o);
            free(rg);
            bool use_closest = fuzzy && o.closest && o.closest->confidence >= threshold && o.fuzzy_matches <= 1;
            const fz_match_t *m = o.matched ? o.matched : use_closest ? o.closest : NULL;
            char *adj = m ? fz_adjust_indentation(no, m->actual_text, nn) : NULL;
            if (!m || !strcmp(adj, m->actual_text)) {
                free(adj);
                if (!reps.n) {
                    *miss = o;
                    vec_free(&reps);
                    goto done;
                }
                fz_outcome_free(&o);
                break;
            }
            repl_t rp = {m->start_index, MIN(m->start_index + MAX(strlen(m->actual_text), (size_t)1), clen), adj};
            vec_push(&reps, rp);
            fz_outcome_free(&o);
        }
        xqsort(reps.v, reps.n, sizeof(repl_t), cmp_repl);
        sb_t b = SB_INIT;
        size_t si = 0;
        for (size_t i = 0; i < reps.n; i++) {
            sb_appendn(&b, nc + si, reps.v[i].start - si);
            sb_append(&b, reps.v[i].text);
            si = reps.v[i].end;
            free(reps.v[i].text);
        }
        sb_append(&b, nc + si);
        *out = sb_detach(&b);
        *count = reps.n;
        vec_free(&reps);
        ok = true;
        goto done;
    }
    {
        fz_outcome_t o;
        fz_find_match(nc, no, fuzzy, threshold, NULL, 0, &o);
        if (!o.matched || o.occurrences > 1) {
            *miss = o;
            goto done;
        }
        char *adj = fz_adjust_indentation(no, o.matched->actual_text, nn);
        sb_t b = SB_INIT;
        sb_appendn(&b, nc, o.matched->start_index);
        sb_append(&b, adj);
        sb_append(&b, nc + o.matched->start_index + strlen(o.matched->actual_text));
        free(adj);
        *out = sb_detach(&b);
        *count = 1;
        ok = true;
        fz_outcome_free(&o);
    }
done:
    free(nc);
    free(no);
    free(nn);
    return ok;
}
