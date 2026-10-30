#include "diff.h"

typedef struct {
    strvec_t a, b;
    char op_buf;
} dctx_t;

typedef enum { D_EQ, D_DEL, D_ADD } dop_t;
typedef struct {
    dop_t op;
    size_t ai, bi;
} dstep_t;
typedef VEC(dstep_t) dsteps_t;

/* Myers O(ND) with trace; falls back to a greedy diff when D gets very large */
static void myers(const strvec_t *a, const strvec_t *b, dsteps_t *out) {
    size_t n = a->n, m = b->n;
    /* trim common prefix/suffix */
    size_t pre = 0;
    while (pre < n && pre < m && strcmp(a->v[pre], b->v[pre]) == 0) pre++;
    size_t suf = 0;
    while (suf < n - pre && suf < m - pre && strcmp(a->v[n - 1 - suf], b->v[m - 1 - suf]) == 0) suf++;
    for (size_t i = 0; i < pre; i++) vec_push(out, ((dstep_t){D_EQ, i, i}));
    size_t N = n - pre - suf, M = m - pre - suf;
    if (N == 0 || M == 0 || (uint64_t)N * M > 4000000ULL * 4) {
        for (size_t i = 0; i < N; i++) vec_push(out, ((dstep_t){D_DEL, pre + i, 0}));
        for (size_t j = 0; j < M; j++) vec_push(out, ((dstep_t){D_ADD, 0, pre + j}));
    } else {
        size_t max = N + M;
        size_t off = max;
        int64_t *v = xcalloc(2 * max + 2, sizeof(int64_t));
        VEC(int64_t *) trace = {0};
        size_t dfound = 0;
        bool done = false;
        for (size_t d = 0; d <= max && !done; d++) {
            int64_t *snap = xmalloc((2 * max + 2) * sizeof(int64_t));
            memcpy(snap, v, (2 * max + 2) * sizeof(int64_t));
            vec_push(&trace, snap);
            for (int64_t k = -(int64_t)d; k <= (int64_t)d; k += 2) {
                int64_t x;
                if (k == -(int64_t)d || (k != (int64_t)d && v[off + k - 1] < v[off + k + 1])) x = v[off + k + 1];
                else x = v[off + k - 1] + 1;
                int64_t y = x - k;
                while ((size_t)x < N && (size_t)y < M && strcmp(a->v[pre + x], b->v[pre + y]) == 0) {
                    x++;
                    y++;
                }
                v[off + k] = x;
                if ((size_t)x >= N && (size_t)y >= M) {
                    dfound = d;
                    done = true;
                    break;
                }
            }
        }
        /* backtrack */
        dsteps_t rev = {0};
        int64_t x = (int64_t)N, y = (int64_t)M;
        for (size_t d = dfound; d > 0; d--) {
            int64_t *vv = trace.v[d];
            int64_t k = x - y;
            int64_t pk;
            if (k == -(int64_t)d || (k != (int64_t)d && vv[off + k - 1] < vv[off + k + 1])) pk = k + 1;
            else pk = k - 1;
            int64_t px = vv[off + pk], py = px - pk;
            while (x > px && y > py) {
                x--;
                y--;
                vec_push(&rev, ((dstep_t){D_EQ, pre + (size_t)x, pre + (size_t)y}));
            }
            if (x == px) vec_push(&rev, ((dstep_t){D_ADD, 0, pre + (size_t)(--y)}));
            else vec_push(&rev, ((dstep_t){D_DEL, pre + (size_t)(--x), 0}));
        }
        while (x > 0 && y > 0) {
            x--;
            y--;
            vec_push(&rev, ((dstep_t){D_EQ, pre + (size_t)x, pre + (size_t)y}));
        }
        for (size_t i = rev.n; i > 0; i--) vec_push(out, rev.v[i - 1]);
        vec_free(&rev);
        for (size_t i = 0; i < trace.n; i++) free(trace.v[i]);
        vec_free(&trace);
        free(v);
    }
    for (size_t i = 0; i < suf; i++) vec_push(out, ((dstep_t){D_EQ, n - suf + i, m - suf + i}));
}

static strvec_t lines_of(const char *s) {
    strvec_t v = str_split_lines(s, strlen(s));
    return v;
}

char *diff_unified(const char *a, const char *b, const char *pa, const char *pb, int context) {
    strvec_t la = lines_of(a), lb = lines_of(b);
    dsteps_t st = {0};
    myers(&la, &lb, &st);
    sb_t out = SB_INIT;
    bool any = false;
    for (size_t i = 0; i < st.n; i++)
        if (st.v[i].op != D_EQ) any = true;
    if (any) {
        sb_appendf(&out, "--- %s\n+++ %s\n", pa, pb);
        size_t i = 0;
        while (i < st.n) {
            while (i < st.n && st.v[i].op == D_EQ) i++;
            if (i >= st.n) break;
            size_t start = i >= (size_t)context ? i - (size_t)context : 0;
            size_t end = i;
            /* extend hunk while changes are within 2*context */
            size_t last_change = i;
            for (size_t j = i; j < st.n; j++) {
                if (st.v[j].op != D_EQ) last_change = j;
                else if (j - last_change > (size_t)(2 * context)) break;
                end = j;
            }
            end = MIN(st.n - 1, last_change + (size_t)context);
            /* compute header numbers */
            size_t a_start = 0, b_start = 0, a_cnt = 0, b_cnt = 0;
            bool a_set = false, b_set = false;
            /* line numbers: track positions */
            size_t ai = 0, bi = 0;
            for (size_t j = 0; j < start; j++) {
                if (st.v[j].op != D_ADD) ai++;
                if (st.v[j].op != D_DEL) bi++;
            }
            for (size_t j = start; j <= end; j++) {
                if (st.v[j].op != D_ADD) {
                    if (!a_set) { a_start = ai; a_set = true; }
                    a_cnt++;
                    ai++;
                }
                if (st.v[j].op != D_DEL) {
                    if (!b_set) { b_start = bi; b_set = true; }
                    b_cnt++;
                    bi++;
                }
            }
            if (!a_set) a_start = ai;
            if (!b_set) b_start = bi;
            sb_appendf(&out, "@@ -%zu,%zu +%zu,%zu @@\n", a_cnt ? a_start + 1 : a_start, a_cnt, b_cnt ? b_start + 1 : b_start, b_cnt);
            for (size_t j = start; j <= end; j++) {
                dstep_t *s = &st.v[j];
                if (s->op == D_EQ) sb_appendf(&out, " %s\n", la.v[s->ai]);
                else if (s->op == D_DEL) sb_appendf(&out, "-%s\n", la.v[s->ai]);
                else sb_appendf(&out, "+%s\n", lb.v[s->bi]);
            }
            i = end + 1;
        }
    }
    vec_free(&st);
    strvec_free(&la);
    strvec_free(&lb);
    return sb_detach(&out);
}

int64_t diff_first_changed_line(const char *a, const char *b) {
    strvec_t la = lines_of(a), lb = lines_of(b);
    dsteps_t st = {0};
    myers(&la, &lb, &st);
    int64_t r = 0;
    size_t bi = 0;
    for (size_t i = 0; i < st.n; i++) {
        if (st.v[i].op != D_EQ) {
            r = (int64_t)(st.v[i].op == D_ADD ? st.v[i].bi : bi) + 1;
            break;
        }
        bi++;
    }
    vec_free(&st);
    strvec_free(&la);
    strvec_free(&lb);
    return r;
}

void diff_stats(const char *a, const char *b, int64_t *added, int64_t *removed) {
    strvec_t la = lines_of(a), lb = lines_of(b);
    dsteps_t st = {0};
    myers(&la, &lb, &st);
    *added = *removed = 0;
    for (size_t i = 0; i < st.n; i++) {
        if (st.v[i].op == D_ADD) (*added)++;
        else if (st.v[i].op == D_DEL) (*removed)++;
    }
    vec_free(&st);
    strvec_free(&la);
    strvec_free(&lb);
}
