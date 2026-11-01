/* fuzzy.c - port of omp's word-local fuzzy matcher (packages/tui/src/fuzzy.ts). */
#include "tui/fuzzy.h"

#include <ctype.h>
#include <math.h>

#define ALNUM_SWAP_PENALTY 5.0
#define COMPACT_PHRASE_BONUS 1200.0
#define PHRASE_BONUS 1000.0
#define MIN_SHADOW_RESCAN_LENGTH 3

static bool is_up(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_lo(char c) { return c >= 'a' && c <= 'z'; }
static bool is_dig(char c) { return c >= '0' && c <= '9'; }
/* \p{Letter}\p{Mark}\p{Number}: ASCII alnum, and every non-ASCII byte (treated as a letter) */
static bool is_word(unsigned char c) { return c >= 0x80 || is_up((char)c) || is_lo((char)c) || is_dig((char)c); }

char *tui_fuzzy_normalize(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    /* 1. ([A-Z]+)([A-Z][a-z]) -> "$1 $2" */
    sb_t a = SB_INIT;
    for (size_t i = 0; i < n;) {
        if (is_up(s[i])) {
            size_t k = i;
            while (k < n && is_up(s[k])) k++;
            if (k - i >= 2 && k < n && is_lo(s[k])) {
                sb_appendn(&a, s + i, k - 1 - i);
                sb_putc(&a, ' ');
                sb_appendn(&a, s + k - 1, 2);
                i = k + 1;
                continue;
            }
            sb_appendn(&a, s + i, k - i);
            i = k;
            continue;
        }
        sb_putc(&a, s[i++]);
    }
    /* 2. ([a-z0-9])([A-Z]) -> "$1 $2" */
    const char *t = sb_cstr(&a);
    size_t tn = a.len;
    sb_t b = SB_INIT;
    for (size_t i = 0; i < tn;) {
        if (i + 1 < tn && (is_lo(t[i]) || is_dig(t[i])) && is_up(t[i + 1])) {
            sb_putc(&b, t[i]);
            sb_putc(&b, ' ');
            sb_putc(&b, t[i + 1]);
            i += 2;
            continue;
        }
        sb_putc(&b, t[i++]);
    }
    sb_free(&a);
    /* 3. lowercase, non-word runs -> single space, trim */
    sb_t c = SB_INIT;
    bool pending_space = false;
    for (size_t i = 0; i < b.len; i++) {
        unsigned char ch = (unsigned char)b.s[i];
        if (!is_word(ch)) {
            pending_space = c.len > 0;
            continue;
        }
        if (pending_space) sb_putc(&c, ' ');
        pending_space = false;
        sb_putc(&c, is_up((char)ch) ? (char)(ch + 32) : (char)ch);
    }
    sb_free(&b);
    return sb_detach(&c);
}

typedef struct {
    const char *text; /* points into idx->normalized (NUL-terminated copies) */
    size_t len;
    size_t index;   /* offset in normalized */
    size_t ordinal;
} word_t;

typedef struct {
    char *normalized;
    char *compact;
    VEC(size_t) starts; /* compact word starts */
    VEC(word_t) words;
    char *wordbuf; /* normalized with spaces replaced by NULs */
} sindex_t;

static void index_build(sindex_t *x, const char *text) {
    memset(x, 0, sizeof *x);
    x->normalized = tui_fuzzy_normalize(text);
    size_t n = strlen(x->normalized);
    x->wordbuf = xstrdup(x->normalized);
    sb_t comp = SB_INIT;
    if (n) {
        size_t start = 0, ord = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || x->normalized[i] == ' ') {
                x->wordbuf[i] = 0;
                word_t w = {x->wordbuf + start, i - start, start, ord++};
                vec_push(&x->words, w);
                vec_push(&x->starts, comp.len);
                sb_appendn(&comp, x->normalized + start, i - start);
                start = i + 1;
            }
        }
    }
    x->compact = sb_detach(&comp);
}

static void index_free(sindex_t *x) {
    free(x->normalized);
    free(x->compact);
    free(x->wordbuf);
    vec_free(&x->starts);
    vec_free(&x->words);
}

static bool has_start(const sindex_t *x, size_t at) {
    for (size_t i = 0; i < x->starts.n; i++)
        if (x->starts.v[i] == at) return true;
    return false;
}

typedef struct {
    bool matches;
    double score;
} fm_t;

static double with_pos(double score, size_t index) { return score + (double)index * 0.01; }

static fm_t score_chars(const char *q, size_t ql, const char *t, size_t tl, long *span) {
    fm_t r = {false, 0};
    *span = 0;
    if (!ql) return (fm_t){true, 0};
    if (ql > tl) return r;
    size_t qi = 0;
    double score = 0;
    long first = -1, last = -1;
    int consec = 0;
    for (size_t i = 0; i < tl && qi < ql; i++) {
        if (t[i] == q[qi]) {
            if (first < 0) first = (long)i;
            if (last == (long)i - 1) {
                consec++;
                score -= consec * 5;
            } else {
                consec = 0;
                if (last >= 0) score += ((long)i - last - 1) * 2;
            }
            score += (double)i * 0.1;
            last = (long)i;
            qi++;
        }
    }
    if (qi < ql) return r;
    *span = last - first + 1;
    return (fm_t){true, score};
}

static bool boundary_phrase(const char *norm, size_t nl, size_t index, size_t len) {
    bool before = index == 0 || norm[index - 1] == ' ';
    size_t after_i = index + len;
    bool after = after_i == nl || norm[after_i] == ' ';
    return before && after;
}

static const char *find_from(const char *hay, size_t from, const char *needle) {
    if (from > strlen(hay)) return NULL;
    return strstr(hay + from, needle);
}

static long find_word_boundary_phrase(const char *norm, const char *phrase) {
    size_t pl = strlen(phrase), nl = strlen(norm);
    if (!pl) return -1;
    const char *f = strstr(norm, phrase);
    if (!f) return -1;
    size_t first = (size_t)(f - norm);
    if (boundary_phrase(norm, nl, first, pl)) return (long)first;
    if (first == 0 || norm[first - 1] == ' ') return -1;
    if (pl < MIN_SHADOW_RESCAN_LENGTH) return -1;
    for (const char *p = find_from(norm, first + 1, phrase); p; p = find_from(norm, (size_t)(p - norm) + 1, phrase))
        if (boundary_phrase(norm, nl, (size_t)(p - norm), pl)) return (long)(p - norm);
    return -1;
}

static long find_compact_word_start(const sindex_t *x, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return -1;
    const char *f = strstr(x->compact, needle);
    if (!f) return -1;
    size_t first = (size_t)(f - x->compact);
    if (has_start(x, first)) return (long)first;
    if (nl < MIN_SHADOW_RESCAN_LENGTH) return -1;
    for (const char *p = find_from(x->compact, first + 1, needle); p; p = find_from(x->compact, (size_t)(p - x->compact) + 1, needle))
        if (has_start(x, (size_t)(p - x->compact))) return (long)(p - x->compact);
    return -1;
}

static bool score_token_word(const char *tok, const word_t *w, fm_t *out) {
    size_t tl = strlen(tok);
    if (w->len == tl && !memcmp(w->text, tok, tl)) {
        *out = (fm_t){true, with_pos(-200, w->index)};
        return true;
    }
    if (w->len >= tl && !memcmp(w->text, tok, tl)) {
        *out = (fm_t){true, with_pos(-170 + (double)(w->len - tl) * 0.5, w->index)};
        return true;
    }
    if (tl >= w->len && !memcmp(tok, w->text, w->len) && tl - w->len <= 2) {
        *out = (fm_t){true, with_pos(-150 + (double)(tl - w->len), w->index)};
        return true;
    }
    const char *sub = strstr(w->text, tok);
    if (sub) {
        *out = (fm_t){true, with_pos(-20 + (double)(sub - w->text), w->index)};
        return true;
    }
    long span;
    fm_t cm = score_chars(tok, tl, w->text, w->len, &span);
    if (!cm.matches) return false;
    long max_span = MAX((long)tl + 2, (long)ceil((double)tl * 1.8));
    if (span > max_span) return false;
    *out = (fm_t){true, with_pos(-40 + cm.score, w->index)};
    return true;
}

static bool score_acronym(const char *tok, const sindex_t *x, fm_t *out) {
    size_t tl = strlen(tok);
    if (tl < 2 || tl > 4 || !x->words.n) return false;
    size_t qi = 0;
    long first_ord = -1, last_ord = -1;
    size_t first_idx = 0;
    for (size_t i = 0; i < x->words.n; i++) {
        const word_t *w = &x->words.v[i];
        if (!w->len || w->text[0] != tok[qi]) continue;
        if (first_ord < 0) {
            first_ord = (long)w->ordinal;
            first_idx = w->index;
        }
        last_ord = (long)w->ordinal;
        qi++;
        if (qi == tl) break;
    }
    if (qi < tl || first_ord < 0 || last_ord < 0) return false;
    long span = last_ord - first_ord + 1;
    if (span > (long)tl + 2) return false;
    *out = (fm_t){true, with_pos(-30 + (double)span * 4 - (double)tl * 2, first_idx)};
    return true;
}

static fm_t score_token_direct(const char *tok, const sindex_t *x) {
    if (!*tok) return (fm_t){true, 0};
    fm_t best = {false, 0};
    long ci = find_compact_word_start(x, tok);
    if (ci >= 0) best = (fm_t){true, with_pos(-140, (size_t)ci)};
    for (size_t i = 0; i < x->words.n; i++) {
        fm_t m;
        if (score_token_word(tok, &x->words.v[i], &m) && (!best.matches || m.score < best.score)) best = m;
    }
    fm_t ac;
    if (score_acronym(tok, x, &ac) && (!best.matches || ac.score < best.score)) best = ac;
    return best;
}

struct tui_fuzzy_query {
    bool empty; /* prepareQuery returned null */
    char *normalized, *compact;
    strvec_t tokens;
    VEC(strvec_t) swaps;
};

static void swap_variants(const char *q, strvec_t *out) {
    size_t n = strlen(q);
    for (size_t i = 0; i + 1 < n; i++) {
        char c = q[i], nx = q[i + 1];
        bool sw = (is_lo(c) && is_dig(nx)) || (is_dig(c) && is_lo(nx));
        if (!sw) continue;
        char *v = xstrdup(q);
        v[i] = nx;
        v[i + 1] = c;
        bool dup = false;
        for (size_t k = 0; k < out->n; k++)
            if (!strcmp(out->v[k], v)) dup = true;
        if (dup) free(v);
        else strvec_push_own(out, v);
    }
}

tui_fuzzy_query_t *tui_fuzzy_query_new(const char *query) {
    tui_fuzzy_query_t *q = xcalloc(1, sizeof *q);
    q->normalized = tui_fuzzy_normalize(query);
    if (!*q->normalized) {
        q->empty = true;
        return q;
    }
    sb_t c = SB_INIT;
    const char *p = q->normalized;
    while (*p) {
        size_t l = strcspn(p, " ");
        strvec_push_own(&q->tokens, xstrndup(p, l));
        sb_appendn(&c, p, l);
        p += l;
        if (*p) p++;
    }
    q->compact = sb_detach(&c);
    for (size_t i = 0; i < q->tokens.n; i++) {
        strvec_t sv = {0};
        swap_variants(q->tokens.v[i], &sv);
        vec_push(&q->swaps, sv);
    }
    return q;
}

void tui_fuzzy_query_free(tui_fuzzy_query_t *q) {
    if (!q) return;
    free(q->normalized);
    free(q->compact);
    strvec_free(&q->tokens);
    for (size_t i = 0; i < q->swaps.n; i++) strvec_free(&q->swaps.v[i]);
    vec_free(&q->swaps);
    free(q);
}

static fm_t match_core(const tui_fuzzy_query_t *q, const sindex_t *x) {
    if (q->empty) return (fm_t){true, 0};
    if (!x->words.n) return (fm_t){false, 0};
    double total = 0;
    long pi = find_word_boundary_phrase(x->normalized, q->normalized);
    if (pi >= 0) total += -PHRASE_BONUS + (double)pi * 0.01;
    long cpi = find_compact_word_start(x, q->compact);
    if (cpi >= 0) total += -COMPACT_PHRASE_BONUS + (double)cpi * 0.01;
    for (size_t i = 0; i < q->tokens.n; i++) {
        fm_t best = score_token_direct(q->tokens.v[i], x);
        if (!best.matches) {
            const strvec_t *sv = &q->swaps.v[i];
            for (size_t k = 0; k < sv->n; k++) {
                fm_t m = score_token_direct(sv->v[k], x);
                if (!m.matches) continue;
                double s = m.score + ALNUM_SWAP_PENALTY;
                if (!best.matches || s < best.score) best = (fm_t){true, s};
            }
        }
        if (!best.matches) return (fm_t){false, 0};
        total += best.score;
    }
    return (fm_t){true, total};
}

bool tui_fuzzy_query_match(const tui_fuzzy_query_t *q, const char *text, double *score) {
    sindex_t x;
    index_build(&x, text);
    fm_t m = match_core(q, &x);
    index_free(&x);
    if (score) *score = m.score;
    return m.matches;
}

typedef struct {
    size_t idx;
    double score;
} ranked_t;

static int cmp_ranked(const void *a, const void *b) {
    const ranked_t *x = a, *y = b;
    if (x->score < y->score) return -1;
    if (x->score > y->score) return 1;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

size_t tui_fuzzy_rank(const char *query, const char *const *texts, size_t n, size_t *out_idx) {
    bool blank = true;
    for (const char *p = query ? query : ""; *p; p++)
        if (!isspace((unsigned char)*p)) blank = false;
    if (blank) {
        for (size_t i = 0; i < n; i++) out_idx[i] = i;
        return n;
    }
    tui_fuzzy_query_t *q = tui_fuzzy_query_new(query);
    ranked_t *r = xmalloc((n ? n : 1) * sizeof *r);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        double s;
        if (tui_fuzzy_query_match(q, texts[i] ? texts[i] : "", &s)) r[m++] = (ranked_t){i, s};
    }
    qsort(r, m, sizeof *r, cmp_ranked);
    for (size_t i = 0; i < m; i++) out_idx[i] = r[i].idx;
    free(r);
    tui_fuzzy_query_free(q);
    return m;
}
