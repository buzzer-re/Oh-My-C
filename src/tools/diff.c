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

/* ======================================================================================
 * jsdiff-compatible line diff + numbered renderings (port of omp pi-diff / diff_string.rs)
 * ====================================================================================== */

typedef struct {
    uint32_t count;
    bool added, removed;
} drun_t;
typedef VEC(drun_t) druns_t;

#define NO_COMP UINT32_MAX

typedef struct {
    uint32_t count, prev, refs;
    bool added, removed;
} comp_t;

typedef struct {
    VEC(comp_t) nodes;
    uint32_t free_head;
} arena_t;

typedef struct {
    int64_t old_pos;
    uint32_t last;
    bool live;
} pstate_t;

static uint32_t ar_alloc(arena_t *a, uint32_t count, uint32_t prev, bool added, bool removed) {
    comp_t c = {count, prev, 1, added, removed};
    if (a->free_head == NO_COMP) {
        vec_push(&a->nodes, c);
        return (uint32_t)(a->nodes.n - 1);
    }
    uint32_t idx = a->free_head;
    a->free_head = a->nodes.v[idx].prev;
    a->nodes.v[idx] = c;
    return idx;
}

static uint32_t ar_ref(arena_t *a, uint32_t idx) {
    if (idx != NO_COMP) a->nodes.v[idx].refs++;
    return idx;
}

static void ar_release(arena_t *a, uint32_t idx) {
    while (idx != NO_COMP) {
        comp_t *n = &a->nodes.v[idx];
        if (--n->refs > 0) break;
        uint32_t prev = n->prev;
        n->prev = a->free_head;
        a->free_head = idx;
        idx = prev;
    }
}

static int64_t extract_common(arena_t *a, pstate_t *p, const uint32_t *nw, int64_t nn, const uint32_t *od, int64_t on,
                              int64_t diag) {
    int64_t op = p->old_pos, np = op - diag;
    uint32_t common = 0;
    while (np + 1 < nn && op + 1 < on && od[op + 1] == nw[np + 1]) {
        np++;
        op++;
        common++;
    }
    if (common) p->last = ar_alloc(a, common, p->last, false, false);
    p->old_pos = op;
    return np;
}

static pstate_t add_to_path(arena_t *a, pstate_t p, bool added, bool removed, int64_t inc) {
    uint32_t li = p.last, nl;
    if (li == NO_COMP) {
        nl = ar_alloc(a, 1, NO_COMP, added, removed);
    } else {
        comp_t last = a->nodes.v[li];
        if (last.added == added && last.removed == removed) {
            if (last.refs == 1) {
                a->nodes.v[li].count = last.count + 1;
                nl = li;
            } else {
                a->nodes.v[li].refs--;
                uint32_t prev = ar_ref(a, last.prev);
                nl = ar_alloc(a, last.count + 1, prev, added, removed);
            }
        } else {
            nl = ar_alloc(a, 1, li, added, removed);
        }
    }
    pstate_t r = {p.old_pos + inc, nl, true};
    return r;
}

static druns_t build_runs(arena_t *a, uint32_t last) {
    druns_t r = {0};
    for (uint32_t c = last; c != NO_COMP; c = a->nodes.v[c].prev) {
        drun_t d = {a->nodes.v[c].count, a->nodes.v[c].added, a->nodes.v[c].removed};
        vec_push(&r, d);
    }
    for (size_t i = 0; i < r.n / 2; i++) {
        drun_t t = r.v[i];
        r.v[i] = r.v[r.n - 1 - i];
        r.v[r.n - 1 - i] = t;
    }
    return r;
}

/* Myers O(ND) replicating jsdiff v9's default path (tie-breaking, run coalescing) */
static druns_t js_myers(const uint32_t *od, size_t on_, const uint32_t *nw, size_t nn_) {
    int64_t on = (int64_t)on_, nn = (int64_t)nn_;
    int64_t max_edit = on + nn, offset = max_edit + 1;
    size_t slots = (size_t)(2 * max_edit + 3);
    pstate_t *best = xcalloc(slots, sizeof(pstate_t));
    arena_t ar = {{0}, NO_COMP};
    druns_t out = {0};
    pstate_t seed = {-1, NO_COMP, true};
    int64_t snp = extract_common(&ar, &seed, nw, nn, od, on, 0);
    if (seed.old_pos + 1 >= on && snp + 1 >= nn) {
        out = build_runs(&ar, seed.last);
        goto done;
    }
    best[offset] = seed;
    int64_t min_d = INT64_MIN, max_d = INT64_MAX;
    for (int64_t el = 1; el <= max_edit; el++) {
        for (int64_t d = MAX(min_d, -el); d <= MIN(max_d, el); d += 2) {
            size_t idx = (size_t)(d + offset);
            pstate_t rem = best[idx - 1];
            best[idx - 1].live = false;
            bool has_add = best[idx + 1].live;
            int64_t add_old = best[idx + 1].old_pos;
            bool can_add = has_add && add_old - d >= 0 && add_old - d < nn;
            bool can_rem = rem.live && rem.old_pos + 1 < on;
            if (!can_add && !can_rem) {
                if (rem.live) ar_release(&ar, rem.last);
                if (best[idx].live) {
                    ar_release(&ar, best[idx].last);
                    best[idx].live = false;
                }
                continue;
            }
            pstate_t base;
            if (!can_rem || (can_add && rem.live && rem.old_pos < add_old)) {
                if (rem.live) ar_release(&ar, rem.last);
                pstate_t cl = {add_old, ar_ref(&ar, best[idx + 1].last), true};
                base = add_to_path(&ar, cl, true, false, 0);
            } else {
                base = add_to_path(&ar, rem, false, true, 1);
            }
            int64_t np = extract_common(&ar, &base, nw, nn, od, on, d);
            if (base.old_pos + 1 >= on && np + 1 >= nn) {
                out = build_runs(&ar, base.last);
                goto done;
            }
            if (base.old_pos + 1 >= on) max_d = MIN(max_d, d - 1);
            if (np + 1 >= nn) min_d = MAX(min_d, d + 1);
            if (best[idx].live) ar_release(&ar, best[idx].last);
            best[idx] = base;
        }
    }
done:
    free(best);
    vec_free(&ar.nodes);
    return out;
}

/* jsdiff line tokens: split inclusive of '\n' */
typedef struct {
    const char *s;
    size_t len; /* including the '\n' if present */
} tok_t;
typedef VEC(tok_t) toks_t;

static toks_t line_tokens(const char *s) {
    toks_t t = {0};
    const char *p = s;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);
        tok_t k = {p, len};
        vec_push(&t, k);
        p += len;
    }
    return t;
}

static druns_t diff_tokens(const toks_t *a, const toks_t *b) {
    hmap_t ids;
    hmap_init(&ids);
    uint32_t *ia = xmalloc((a->n + 1) * sizeof(uint32_t)), *ib = xmalloc((b->n + 1) * sizeof(uint32_t));
    uintptr_t next = 1;
    for (int side = 0; side < 2; side++) {
        const toks_t *t = side ? b : a;
        uint32_t *out = side ? ib : ia;
        for (size_t i = 0; i < t->n; i++) {
            char *k = xstrndup(t->v[i].s, t->v[i].len);
            uintptr_t id = (uintptr_t)hmap_get(&ids, k);
            if (!id) {
                id = next++;
                hmap_put(&ids, k, (void *)id);
            }
            free(k);
            out[i] = (uint32_t)id;
        }
    }
    druns_t r = js_myers(ia, a->n, ib, b->n);
    free(ia);
    free(ib);
    hmap_free(&ids, NULL);
    return r;
}

/* row text of a token (without its '\n') */
static char *tok_row(const tok_t *t) {
    size_t len = t->len;
    if (len && t->s[len - 1] == '\n') len--;
    return xstrndup(t->s, len);
}

/* ---- numbered rows helpers ---- */

typedef struct {
    int prefix; /* '+', '-', ' ' */
    uint32_t line;
} nrow_t;

static bool parse_nrow(const char *row, nrow_t *out) {
    if (row[0] != '+' && row[0] != '-' && row[0] != ' ') return false;
    const char *body = row + 1, *bar = strchr(body, '|');
    if (!bar) return false;
    const char *p = body;
    if (*p == '+') p++;
    if (p == bar) return false;
    uint64_t v = 0;
    for (; p < bar; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) return false;
    }
    out->prefix = row[0];
    out->line = (uint32_t)v;
    return true;
}

static bool is_change_row(const strvec_t *rows, size_t i) {
    return i < rows->n && (rows->v[i][0] == '+' || rows->v[i][0] == '-');
}

static bool source_line(const char *row, uint32_t *ln) {
    nrow_t r;
    if (!parse_nrow(row, &r) || r.prefix == '+') return false;
    *ln = r.line;
    return true;
}

/* keep a gap row only between source rows that are not contiguous */
static void normalize_gap_rows(strvec_t *rows) {
    strvec_t out = {0};
    for (size_t r = 0; r < rows->n; r++) {
        if (*rows->v[r]) {
            strvec_push(&out, rows->v[r]);
            continue;
        }
        if (!out.n || !*out.v[out.n - 1]) continue;
        bool hb = false, ha = false;
        uint32_t before = 0, after = 0;
        for (size_t k = out.n; k > 0 && !hb; k--) hb = source_line(out.v[k - 1], &before);
        for (size_t k = r + 1; k < rows->n && !ha; k++)
            if (*rows->v[k]) ha = source_line(rows->v[k], &after);
        if (hb && ha && after > before + 1) strvec_push(&out, "");
    }
    strvec_free(rows);
    *rows = out;
}

static void insert_rows(strvec_t *rows, size_t at, char **chunk, size_t n) {
    for (size_t i = 0; i < n; i++) vec_push(rows, NULL);
    memmove(rows->v + at + n, rows->v + at, (rows->n - n - at) * sizeof(char *));
    for (size_t i = 0; i < n; i++) rows->v[at + i] = chunk[i];
}

typedef struct {
    uint32_t line;
    char *text;
} ctxline_t;
typedef VEC(ctxline_t) ctxlines_t;

/* sorted insert; keeps the existing entry unless overwrite */
static void ctx_put(ctxlines_t *c, uint32_t line, const char *text, bool overwrite) {
    for (size_t i = 0; i < c->n; i++)
        if (c->v[i].line == line) {
            if (overwrite) {
                free(c->v[i].text);
                c->v[i].text = xstrdup(text);
            }
            return;
        }
    size_t at = c->n;
    while (at > 0 && c->v[at - 1].line > line) at--;
    ctxline_t e = {line, xstrdup(text)};
    vec_push(c, e);
    memmove(c->v + at + 1, c->v + at, (c->n - 1 - at) * sizeof(ctxline_t));
    c->v[at] = e;
}

static bool u32_in(const uint32_t *v, size_t n, uint32_t x) {
    for (size_t i = 0; i < n; i++)
        if (v[i] == x) return true;
    return false;
}

typedef struct {
    int opener;
    uint32_t line;
    const char *text;
    bool visible;
} sent_t;

/* lexical enclosing-bracket context (upstream prefers tree-sitter boundaries, unavailable here) */
static void lexical_context(char *const *lines, size_t n, const uint32_t *vis, size_t nvis, ctxlines_t *out) {
    size_t distinct = 0;
    for (size_t i = 0; i < nvis; i++)
        if (!u32_in(vis, i, vis[i])) distinct++;
    if (!distinct || (n && distinct >= n)) return;
    VEC(sent_t) st = {0};
    enum { M_CODE, M_SINGLE, M_DOUBLE, M_TEMPLATE, M_BLOCK } mode = M_CODE;
    bool esc = false;
    for (size_t li = 0; li < n; li++) {
        const char *line = lines[li];
        uint32_t ln = (uint32_t)li + 1;
        bool lv = u32_in(vis, nvis, ln);
        size_t len = strlen(line);
        for (size_t i = 0; i < len; i++) {
            char c = line[i], nx = i + 1 < len ? line[i + 1] : 0;
            if (mode == M_BLOCK) {
                if (c == '*' && nx == '/') {
                    mode = M_CODE;
                    i++;
                }
                continue;
            }
            if (mode == M_SINGLE || mode == M_DOUBLE || mode == M_TEMPLATE) {
                if (esc) {
                    esc = false;
                    continue;
                }
                if (c == '\\') {
                    esc = true;
                    continue;
                }
                if ((mode == M_SINGLE && c == '\'') || (mode == M_DOUBLE && c == '"') || (mode == M_TEMPLATE && c == '`'))
                    mode = M_CODE;
                continue;
            }
            if (c == '/' && nx == '/') break;
            if (c == '/' && nx == '*') {
                mode = M_BLOCK;
                i++;
                continue;
            }
            if (c == '#') {
                size_t k = 0;
                while (k < i && (line[k] == ' ' || line[k] == '\t')) k++;
                if (k == i) break;
            }
            if (c == '\'' || c == '"' || c == '`') {
                mode = c == '\'' ? M_SINGLE : c == '"' ? M_DOUBLE : M_TEMPLATE;
                esc = false;
                continue;
            }
            if (c == '(' || c == '[' || c == '{') {
                sent_t e = {c, ln, line, lv};
                vec_push(&st, e);
                continue;
            }
            int op = c == ')' ? '(' : c == ']' ? '[' : c == '}' ? '{' : 0;
            if (!op) continue;
            size_t mi = st.n;
            while (mi > 0 && st.v[mi - 1].opener != op) mi--;
            if (!mi) continue;
            sent_t m = st.v[mi - 1];
            st.n = mi - 1;
            if (lv && !m.visible) ctx_put(out, m.line, m.text, true);
            if (m.visible && !lv) ctx_put(out, ln, line, true);
        }
        if (mode == M_SINGLE || mode == M_DOUBLE) {
            mode = M_CODE;
            esc = false;
        }
    }
    vec_free(&st);
    for (size_t i = 0; i < out->n;) {
        if (u32_in(vis, nvis, out->v[i].line)) {
            free(out->v[i].text);
            memmove(out->v + i, out->v + i + 1, (out->n - i - 1) * sizeof(ctxline_t));
            out->n--;
        } else {
            i++;
        }
    }
}

static size_t adjusted_insert_index(const strvec_t *rows, size_t index) {
    size_t s = index, e = index;
    while (s > 0 && is_change_row(rows, s - 1)) s--;
    while (e < rows->n && is_change_row(rows, e)) e++;
    return index > s && index < e ? e : index;
}

static void insert_context_rows(strvec_t *rows, const ctxlines_t *ctx) {
    for (size_t c = 0; c < ctx->n; c++) {
        uint32_t ln = ctx->v[c].line;
        char *row = xasprintf(" %u|%s", ln, ctx->v[c].text);
        if (strvec_contains(rows, row)) {
            free(row);
            continue;
        }
        size_t at = rows->n;
        bool hp = false, hn = false;
        uint32_t prev = 0, next = 0;
        for (size_t i = 0; i < rows->n; i++) {
            nrow_t p;
            if (!parse_nrow(rows->v[i], &p) || p.prefix == '+') continue;
            if (p.line < ln) {
                hp = true;
                prev = p.line;
                continue;
            }
            hn = true;
            next = p.line;
            at = i;
            break;
        }
        char *chunk[3];
        size_t k = 0;
        if (hp && ln > prev + 1) chunk[k++] = xstrdup("");
        chunk[k++] = row;
        if (hn && next > ln + 1) chunk[k++] = xstrdup("");
        insert_rows(rows, adjusted_insert_index(rows, at), chunk, k);
    }
}

static void add_bracket_context(strvec_t *rows, const char *old_text, const char *new_text) {
    strvec_t ol = str_split(old_text, "\n"), nl = str_split(new_text, "\n");
    VEC(uint32_t) ov = {0}, nv = {0};
    VEC(int64_t) cpos = {0};
    VEC(int) cdel = {0};
    int64_t offset = 0;
    for (size_t i = 0; i < rows->n; i++) {
        nrow_t p;
        if (!parse_nrow(rows->v[i], &p)) continue;
        if (p.prefix == '-') {
            vec_push(&ov, p.line);
            vec_push(&cpos, (int64_t)p.line + offset);
            vec_push(&cdel, -1);
            offset--;
        } else if (p.prefix == '+') {
            vec_push(&nv, p.line);
            vec_push(&cpos, (int64_t)p.line);
            vec_push(&cdel, 1);
            offset++;
        } else {
            vec_push(&ov, p.line);
            int64_t sh = (int64_t)p.line + offset;
            if (sh >= 0 && sh <= UINT32_MAX) vec_push(&nv, (uint32_t)sh);
        }
    }
    ctxlines_t ctx = {0}, nctx = {0};
    lexical_context(ol.v, ol.n, ov.v, ov.n, &ctx);
    lexical_context(nl.v, nl.n, nv.v, nv.n, &nctx);
    for (size_t i = 0; i < nctx.n; i++) {
        int64_t nln = nctx.v[i].line, shift = 0;
        for (size_t k = 0; k < cpos.n; k++)
            if (cpos.v[k] <= nln) shift += cdel.v[k];
        int64_t o = nln - shift;
        if (o >= 0 && o <= UINT32_MAX) ctx_put(&ctx, (uint32_t)o, nctx.v[i].text, false);
        free(nctx.v[i].text);
    }
    vec_free(&nctx);
    insert_context_rows(rows, &ctx);
    for (size_t i = 0; i < ctx.n; i++) free(ctx.v[i].text);
    vec_free(&ctx);
    vec_free(&ov);
    vec_free(&nv);
    vec_free(&cpos);
    vec_free(&cdel);
    strvec_free(&ol);
    strvec_free(&nl);
    normalize_gap_rows(rows);
}

char *diff_numbered(const char *old_text, const char *new_text, int context, int64_t *first_changed) {
    size_t ctxn = context < 0 ? 2 : (size_t)context;
    toks_t ot = line_tokens(old_text), nt = line_tokens(new_text);
    druns_t runs = diff_tokens(&ot, &nt);
    strvec_t out = {0};
    uint32_t oln = 1, nln = 1;
    bool last_change = false;
    int64_t first = 0;
    size_t op = 0, np = 0;
    for (size_t ri = 0; ri < runs.n; ri++) {
        drun_t *run = &runs.v[ri];
        size_t cnt = run->count;
        const tok_t *raw;
        if (run->removed) {
            raw = ot.v + op;
            op += cnt;
        } else {
            raw = nt.v + np;
            np += cnt;
            if (!run->added) op += cnt;
        }
        if (run->added || run->removed) {
            if (!first) first = nln;
            for (size_t i = 0; i < cnt; i++) {
                char *t = tok_row(&raw[i]);
                if (run->added) strvec_push_own(&out, xasprintf("+%u|%s", nln++, t));
                else strvec_push_own(&out, xasprintf("-%u|%s", oln++, t));
                free(t);
            }
            last_change = true;
            continue;
        }
        bool next_change = ri + 1 < runs.n && (runs.v[ri + 1].added || runs.v[ri + 1].removed);
        if (last_change || next_change) {
            size_t lead = 0, mid = 0, trail = 0, show_from = 0, show_n = cnt;
            if (last_change && next_change) {
                if (cnt > ctxn * 2) mid = cnt - ctxn * 2;
            } else if (next_change) {
                lead = cnt > ctxn ? cnt - ctxn : 0;
                show_from = lead;
                show_n = cnt - lead;
            } else {
                trail = cnt > ctxn ? cnt - ctxn : 0;
                show_n = MIN(cnt, ctxn);
            }
            oln += (uint32_t)lead;
            nln += (uint32_t)lead;
            size_t first_chunk = mid ? ctxn : show_n;
            for (size_t i = 0; i < first_chunk; i++) {
                char *t = tok_row(&raw[show_from + i]);
                strvec_push_own(&out, xasprintf(" %u|%s", oln, t));
                free(t);
                oln++;
                nln++;
            }
            if (mid) {
                oln += (uint32_t)mid;
                nln += (uint32_t)mid;
                for (size_t i = cnt - ctxn; i < cnt; i++) {
                    char *t = tok_row(&raw[i]);
                    strvec_push_own(&out, xasprintf(" %u|%s", oln, t));
                    free(t);
                    oln++;
                    nln++;
                }
            }
            oln += (uint32_t)trail;
            nln += (uint32_t)trail;
        } else {
            oln += (uint32_t)cnt;
            nln += (uint32_t)cnt;
        }
        last_change = false;
    }
    add_bracket_context(&out, old_text, new_text);
    char *r = strvec_join(&out, "\n");
    strvec_free(&out);
    vec_free(&runs);
    vec_free(&ot);
    vec_free(&nt);
    if (first_changed) *first_changed = first;
    return r;
}

typedef struct {
    int kind; /* ' ', '-', '+', '\\' */
    const tok_t *tok;
} hrow_t;
typedef VEC(hrow_t) hrows_t;

static void push_hrow(hrows_t *rows, int kind, const tok_t *t) {
    hrow_t r = {kind, t};
    vec_push(rows, r);
    if (!(t->len && t->s[t->len - 1] == '\n')) {
        hrow_t m = {'\\', NULL};
        vec_push(rows, m);
    }
}

typedef struct {
    bool added, removed;
    const tok_t *lines;
    size_t n;
} chg_t;

char *diff_numbered_unified(const char *old_text, const char *new_text, int context, int64_t *first_changed) {
    size_t ctxn = context < 0 ? 3 : (size_t)context;
    toks_t ot = line_tokens(old_text), nt = line_tokens(new_text);
    druns_t runs = diff_tokens(&ot, &nt);
    chg_t *list = xmalloc((runs.n + 1) * sizeof(chg_t));
    size_t op = 0, np = 0;
    for (size_t i = 0; i < runs.n; i++) {
        size_t c = runs.v[i].count;
        list[i].added = runs.v[i].added;
        list[i].removed = runs.v[i].removed;
        list[i].n = c;
        if (runs.v[i].removed) {
            list[i].lines = ot.v + op;
            op += c;
        } else {
            list[i].lines = nt.v + np;
            np += c;
            if (!runs.v[i].added) op += c;
        }
    }
    size_t nl = runs.n + 1;
    list[runs.n] = (chg_t){false, false, NULL, 0};
    strvec_t out = {0};
    int64_t first = 0;
    hrows_t cur = {0};
    size_t ors = 0, nrs = 0, ol = 1, nwl = 1;
    for (size_t i = 0; i < nl; i++) {
        chg_t *c = &list[i];
        if (c->added || c->removed) {
            if (!ors) {
                ors = ol;
                nrs = nwl;
                if (i > 0 && ctxn > 0) {
                    chg_t *p = &list[i - 1];
                    size_t take = MIN(p->n, ctxn);
                    for (size_t k = p->n - take; k < p->n; k++) push_hrow(&cur, ' ', &p->lines[k]);
                    ors -= take;
                    nrs -= take;
                }
            }
            for (size_t k = 0; k < c->n; k++) push_hrow(&cur, c->added ? '+' : '-', &c->lines[k]);
            if (c->added) nwl += c->n;
            else ol += c->n;
            continue;
        }
        if (ors) {
            if (c->n <= ctxn * 2 && i + 2 < nl) {
                for (size_t k = 0; k < c->n; k++) push_hrow(&cur, ' ', &c->lines[k]);
            } else {
                size_t cs = MIN(c->n, ctxn);
                for (size_t k = 0; k < cs; k++) push_hrow(&cur, ' ', &c->lines[k]);
                strvec_push_own(&out, xasprintf("@@ -%zu,%zu +%zu,%zu @@", ors, ol - ors + cs, nrs, nwl - nrs + cs));
                size_t o = ors, n = nrs;
                for (size_t k = 0; k < cur.n; k++) {
                    hrow_t *h = &cur.v[k];
                    if (h->kind == '\\') {
                        strvec_push(&out, "\\ No newline at end of file");
                        continue;
                    }
                    char *t = tok_row(h->tok);
                    if (h->kind == '-') {
                        if (!first) first = (int64_t)n;
                        strvec_push_own(&out, xasprintf("-%zu|%s", o++, t));
                    } else if (h->kind == '+') {
                        if (!first) first = (int64_t)n;
                        strvec_push_own(&out, xasprintf("+%zu|%s", n++, t));
                    } else {
                        strvec_push_own(&out, xasprintf(" %zu|%s", o++, t));
                        n++;
                    }
                    free(t);
                }
                cur.n = 0;
                ors = nrs = 0;
            }
        }
        ol += c->n;
        nwl += c->n;
    }
    vec_free(&cur);
    free(list);
    add_bracket_context(&out, old_text, new_text);
    char *r = strvec_join(&out, "\n");
    strvec_free(&out);
    vec_free(&runs);
    vec_free(&ot);
    vec_free(&nt);
    if (first_changed) *first_changed = first;
    return r;
}

/* ---- compact preview ---- */

static bool is_sep(const char *l) { return !strcmp(l, "…") || !*l; }

static void preview_push(strvec_t *out, const char *line) {
    const char *norm = (!strcmp(line, "...") || !strcmp(line, "…") || !strcmp(line, "+…")) ? "…" : line;
    if (is_sep(norm) && (!out->n || is_sep(out->v[out->n - 1]))) return;
    strvec_push(out, norm);
}

static void push_added_run(strvec_t *out, strvec_t *run, size_t edge) {
    if (!run->n) return;
    if (run->n <= edge * 2 + 1) {
        for (size_t i = 0; i < run->n; i++) preview_push(out, run->v[i]);
    } else {
        for (size_t i = 0; i < edge; i++) preview_push(out, run->v[i]);
        preview_push(out, "…");
        for (size_t i = run->n - edge; i < run->n; i++) preview_push(out, run->v[i]);
    }
    strvec_free(run);
}

static bool parse_compact(const char *line, int *kind, int64_t *num, const char **content) {
    if (line[0] != '+' && line[0] != '-' && line[0] != ' ') return false;
    const char *body = line + 1, *bar = strchr(body, '|');
    if (!bar) return false;
    const char *p = body;
    while (p < bar && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v')) p++;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (p >= bar || *p < '0' || *p > '9') return false;
    int64_t v = 0;
    while (p < bar && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    *kind = line[0];
    *num = neg ? -v : v;
    *content = bar + 1;
    return true;
}

char *diff_compact_preview(const char *diff, size_t *added_out, size_t *removed_out) {
    strvec_t lines = {0};
    if (*diff) lines = str_split(diff, "\n");
    size_t added = 0, removed = 0;
    strvec_t out = {0}, run = {0};
    for (size_t i = 0; i < lines.n; i++) {
        int kind;
        int64_t num;
        const char *content;
        if (!parse_compact(lines.v[i], &kind, &num, &content)) {
            push_added_run(&out, &run, 2);
            preview_push(&out, lines.v[i]);
            continue;
        }
        if (kind == '+') {
            added++;
            strvec_push_own(&run, xasprintf("%lld:%s", (long long)num, content));
        } else if (kind == '-') {
            push_added_run(&out, &run, 2);
            removed++;
        } else {
            push_added_run(&out, &run, 2);
            char *row = xasprintf("%lld:%s", (long long)(num + (int64_t)added - (int64_t)removed), content);
            preview_push(&out, row);
            free(row);
        }
    }
    push_added_run(&out, &run, 2);
    while (out.n && is_sep(out.v[out.n - 1])) free(out.v[--out.n]);
    char *r = strvec_join(&out, "\n");
    strvec_free(&out);
    strvec_free(&lines);
    if (added_out) *added_out = added;
    if (removed_out) *removed_out = removed;
    return r;
}
