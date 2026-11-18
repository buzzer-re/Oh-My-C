/* fuzzy.c - fzf v2-like fuzzy scorer. See fuzzy.h. */
#include "text/fuzzy.h"

#define SCORE_MATCH 16
#define GAP_START (-3)
#define GAP_EXT (-1)
#define BONUS_BOUNDARY 8
#define BONUS_NONWORD 8
#define BONUS_CAMEL 7
#define BONUS_CONSECUTIVE 4
#define BONUS_WHITE 10
#define BONUS_DELIM 9
#define FIRST_MULT 2
#define NEG_INF (-1000000000)
#define FUZZY_MAX_CELLS (1u << 20)
#define FUZZY_MAX_CHARS 65536

enum { C_WHITE, C_NONWORD, C_DELIM, C_LOWER, C_UPPER, C_LETTER, C_NUMBER };

static int char_class(uint32_t c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return C_WHITE;
    if (c >= 'a' && c <= 'z') return C_LOWER;
    if (c >= 'A' && c <= 'Z') return C_UPPER;
    if (c >= '0' && c <= '9') return C_NUMBER;
    if (c == '/' || c == ',' || c == ':' || c == ';' || c == '|' || c == '\\') return C_DELIM;
    if (c >= 0x80) return C_LETTER;
    return C_NONWORD;
}

static int bonus_for(int prev, int cls) {
    if (cls >= C_LOWER) {
        if (prev == C_WHITE) return BONUS_WHITE;
        if (prev == C_DELIM) return BONUS_DELIM;
        if (prev == C_NONWORD) return BONUS_BOUNDARY;
        if ((prev == C_LOWER && cls == C_UPPER) || (prev != C_NUMBER && cls == C_NUMBER)) return BONUS_CAMEL;
        return 0;
    }
    if (cls == C_NONWORD || cls == C_DELIM) return BONUS_NONWORD;
    if (cls == C_WHITE) return BONUS_WHITE;
    return 0;
}

static uint32_t fold(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    return c;
}

typedef struct {
    uint32_t *cp;
    int *off;
    int n;
} decoded_t;

static void decode(const char *s, size_t len, decoded_t *d) {
    d->cp = xmalloc((len + 1) * sizeof(uint32_t));
    d->off = xmalloc((len + 1) * sizeof(int));
    d->n = 0;
    size_t i = 0;
    while (i < len && d->n < FUZZY_MAX_CHARS) {
        uint32_t c;
        int k = utf8_decode(s + i, len - i, &c);
        if (k < 1) k = 1;
        d->cp[d->n] = c;
        d->off[d->n] = (int)i;
        d->n++;
        i += (size_t)k;
    }
}

static void decoded_free(decoded_t *d) {
    free(d->cp);
    free(d->off);
}

/* Score one term (pc[0..m)) against text; fills pos (char indices) if non-NULL. */
static int score_term(const uint32_t *pc, int m, const decoded_t *t, const int *bonus, bool icase, int *pos) {
    int n = t->n;
    if (m == 0) return 0;
    /* subsequence check + window */
    int first = -1, last = -1, k = 0;
    for (int j = 0; j < n && k < m; j++) {
        uint32_t c = icase ? fold(t->cp[j]) : t->cp[j];
        if (c == pc[k]) {
            if (k == 0) first = j;
            k++;
            if (k == m) last = j;
        }
    }
    if (k < m) return -1;
    /* extend last to the last occurrence of the final pattern char */
    for (int j = n - 1; j > last; j--) {
        uint32_t c = icase ? fold(t->cp[j]) : t->cp[j];
        if (c == pc[m - 1]) { last = j; break; }
    }
    int w = last - first + 1;
    if ((size_t)w * (size_t)m > FUZZY_MAX_CELLS) {
        /* greedy fallback */
        int s = 0, prevj = -2, kk = 0;
        for (int j = first; j <= last && kk < m; j++) {
            uint32_t c = icase ? fold(t->cp[j]) : t->cp[j];
            if (c != pc[kk]) continue;
            s += SCORE_MATCH + (kk == 0 ? bonus[j] * FIRST_MULT : bonus[j]);
            if (prevj >= 0 && j != prevj + 1) s += GAP_START + (j - prevj - 2) * GAP_EXT;
            if (prevj >= 0 && j == prevj + 1) s += BONUS_CONSECUTIVE;
            if (pos) pos[kk] = j;
            prevj = j;
            kk++;
        }
        return s < 0 ? 0 : s;
    }
    size_t cells = (size_t)w * (size_t)m;
    int *M = xmalloc(cells * sizeof(int));
    int *C = xmalloc(cells * sizeof(int));
    int *G = xmalloc(cells * sizeof(int));
    int *GF = pos ? xmalloc(cells * sizeof(int)) : NULL; /* gap back-pointer: column of M */
    uint8_t *FROM = pos ? xmalloc(cells) : NULL;         /* 0 start, 1 diag, 2 gap */
    for (int i = 0; i < m; i++) {
        for (int jj = 0; jj < w; jj++) {
            int j = first + jj;
            size_t idx = (size_t)i * w + jj;
            uint32_t c = icase ? fold(t->cp[j]) : t->cp[j];
            int best = NEG_INF, cons = 0;
            uint8_t from = 0;
            if (c == pc[i]) {
                if (i == 0) {
                    best = SCORE_MATCH + bonus[j] * FIRST_MULT;
                    cons = 1;
                    from = 0;
                } else if (jj > 0) {
                    size_t pidx = (size_t)(i - 1) * w + jj - 1;
                    if (M[pidx] > NEG_INF) {
                        int cc = C[pidx] + 1;
                        int b = bonus[j];
                        int bs = bonus[j - cc + 1];
                        if (bs > b) b = bs;
                        if (BONUS_CONSECUTIVE > b) b = BONUS_CONSECUTIVE;
                        best = M[pidx] + SCORE_MATCH + b;
                        cons = cc;
                        from = 1;
                    }
                    if (G[pidx] > NEG_INF) {
                        int s = G[pidx] + SCORE_MATCH + bonus[j];
                        if (s > best) {
                            best = s;
                            cons = 1;
                            from = 2;
                        }
                    }
                }
            }
            M[idx] = best;
            C[idx] = cons;
            if (FROM) FROM[idx] = from;
            /* gap state: P[i] matched somewhere before j, chars after it skipped up to j */
            int g = NEG_INF, gf = -1;
            if (jj > 0) {
                size_t l = idx - 1;
                if (M[l] > NEG_INF) { g = M[l] + GAP_START; gf = jj - 1; }
                if (G[l] > NEG_INF && G[l] + GAP_EXT > g) { g = G[l] + GAP_EXT; gf = GF ? GF[l] : -1; }
            }
            G[idx] = g;
            if (GF) GF[idx] = gf;
        }
    }
    int best = NEG_INF, bj = -1;
    for (int jj = 0; jj < w; jj++) {
        int v = M[(size_t)(m - 1) * w + jj];
        if (v > best) { best = v; bj = jj; }
    }
    if (pos && bj >= 0) {
        int jj = bj;
        for (int i = m - 1; i >= 0 && jj >= 0; i--) {
            pos[i] = first + jj;
            size_t idx = (size_t)i * w + jj;
            if (i == 0) break;
            if (FROM[idx] == 1) jj = jj - 1;
            else jj = GF[(size_t)(i - 1) * w + jj - 1];
        }
    }
    free(M);
    free(C);
    free(G);
    free(GF);
    free(FROM);
    if (best <= NEG_INF) return -1;
    return best < 0 ? 0 : best;
}

int fuzzy_score(const char *pattern, const char *text, int *positions_out) {
    if (!pattern) pattern = "";
    if (!text) text = "";
    decoded_t t;
    decode(text, strlen(text), &t);
    int *bonus = xmalloc(((size_t)t.n + 1) * sizeof(int));
    int prev = C_WHITE;
    for (int j = 0; j < t.n; j++) {
        int cls = char_class(t.cp[j]);
        bonus[j] = bonus_for(prev, cls);
        prev = cls;
    }
    decoded_t p;
    decode(pattern, strlen(pattern), &p);
    int total = 0, outk = 0;
    int *pos = xmalloc(((size_t)p.n + 1) * sizeof(int));
    int i = 0;
    bool ok = true;
    while (i < p.n && ok) {
        while (i < p.n && (p.cp[i] == ' ' || p.cp[i] == '\t')) i++;
        int s = i;
        while (i < p.n && p.cp[i] != ' ' && p.cp[i] != '\t') i++;
        int m = i - s;
        if (m == 0) break;
        bool icase = true;
        for (int k = s; k < i; k++)
            if (fold(p.cp[k]) != p.cp[k]) icase = false;
        int sc = score_term(p.cp + s, m, &t, bonus, icase, positions_out ? pos : NULL);
        if (sc < 0) { ok = false; break; }
        total += sc;
        if (positions_out)
            for (int k = 0; k < m; k++) positions_out[outk++] = t.off[pos[k]];
    }
    free(pos);
    free(bonus);
    decoded_free(&t);
    decoded_free(&p);
    return ok ? total : -1;
}

typedef struct {
    fuzzy_hit_t h;
    size_t len;
} rank_t;

static int rank_cmp(const void *a, const void *b) {
    const rank_t *x = a, *y = b;
    if (x->h.score != y->h.score) return y->h.score - x->h.score;
    if (x->len != y->len) return x->len < y->len ? -1 : 1;
    return x->h.index < y->h.index ? -1 : x->h.index > y->h.index;
}

size_t fuzzy_rank(const char *pattern, const char *const *items, size_t n, fuzzy_hit_t **out) {
    rank_t *r = xmalloc((n ? n : 1) * sizeof(rank_t));
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (!items[i]) continue;
        int s = fuzzy_score(pattern, items[i], NULL);
        if (s < 0) continue;
        r[k].h.index = i;
        r[k].h.score = s;
        r[k].len = strlen(items[i]);
        k++;
    }
    xqsort(r, k, sizeof(rank_t), rank_cmp);
    fuzzy_hit_t *o = xmalloc((k ? k : 1) * sizeof(fuzzy_hit_t));
    for (size_t i = 0; i < k; i++) o[i] = r[i].h;
    free(r);
    *out = o;
    return k;
}
