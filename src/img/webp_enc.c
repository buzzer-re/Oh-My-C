/* webp_enc.c - lossy WebP encoder: one VP8 key frame (RFC 6386) in a RIFF container, plus VP8X + ALPH
 * (lossless VP8L-coded alpha plane) for translucent images.
 *
 * Pipeline (modelled on libwebp's default lossy settings, not on its code):
 *  - RGB -> YUV 4:2:0 (BT.601 studio swing, 2x2 chroma average), planes padded to whole macroblocks;
 *  - analysis: per-macroblock "susceptibility" from a DCT histogram, k-means into 4 segments whose
 *    quantizers follow libwebp's quality curve modulated by spatial noise shaping (strength 50);
 *  - per macroblock rate-distortion mode decision: I16 (DC/V/H/TM) vs B_PRED (10 modes per subblock),
 *    chroma DC/V/H/TM; cost = SSE + lambda * bits from the token probabilities;
 *  - two passes: the first gathers token statistics, the frame then carries probability updates where
 *    they pay off and the second pass decides with the updated costs;
 *  - skip flags, normal loop filter whose level follows the quantizer (filter strength 60, sharpness 0).
 * Prediction always uses the unfiltered reconstruction, exactly as the decoder does. */
#include "img/img.h"
#include "img/vp8.h"
#include "img/vp8_tables.h"
#include "core/core.h"
#include <math.h>

#define BPS VP8_BPS
#define NSEG 4
#define MAX_LEVEL 2047

/* ---------------------------------------------------------------- byte buffer */
typedef struct {
    uint8_t *d;
    size_t n, cap;
} buf_t;

static void buf_need(buf_t *b, size_t k) {
    if (b->n + k <= b->cap) return;
    b->cap = MAX(b->cap * 2, b->n + k + 256);
    b->d = xrealloc(b->d, b->cap);
}
static void buf_put(buf_t *b, const void *p, size_t k) {
    buf_need(b, k);
    memcpy(b->d + b->n, p, k);
    b->n += k;
}
static void buf_byte(buf_t *b, uint8_t c) {
    buf_need(b, 1);
    b->d[b->n++] = c;
}
static void buf_le(buf_t *b, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) buf_byte(b, (uint8_t)(v >> (8 * i)));
}
static void buf_chunk(buf_t *b, const char *tag, const uint8_t *p, size_t n) {
    buf_put(b, tag, 4);
    buf_le(b, (uint32_t)n, 4);
    buf_put(b, p, n);
    if (n & 1) buf_byte(b, 0);
}

/* ---------------------------------------------------------------- boolean encoder (RFC 6386 7.3) */
typedef struct {
    buf_t out;
    uint32_t range, bottom;
    int bit_count;
} benc_t;

static void be_init(benc_t *e) {
    memset(e, 0, sizeof *e);
    e->range = 255, e->bit_count = 24;
}

static void be_carry(benc_t *e) {
    size_t i = e->out.n;
    while (i > 0 && e->out.d[i - 1] == 255) e->out.d[--i] = 0;
    if (i > 0) e->out.d[i - 1]++;
}

static inline void be_put(benc_t *e, int prob, int bit) {
    uint32_t split = 1 + (((e->range - 1) * (uint32_t)prob) >> 8);
    if (bit) e->bottom += split, e->range -= split;
    else e->range = split;
    while (e->range < 128) {
        e->range <<= 1;
        if (e->bottom & (1u << 31)) be_carry(e);
        e->bottom <<= 1;
        if (!--e->bit_count) {
            buf_byte(&e->out, (uint8_t)(e->bottom >> 24));
            e->bottom &= (1u << 24) - 1;
            e->bit_count = 8;
        }
    }
}

static void be_lit(benc_t *e, uint32_t v, int n) {
    while (n-- > 0) be_put(e, 128, (v >> n) & 1);
}

static void be_signed(benc_t *e, int v, int n) {
    be_lit(e, (uint32_t)abs(v), n);
    be_put(e, 128, v < 0);
}

static void be_flush(benc_t *e) {
    int c = e->bit_count;
    uint32_t v = e->bottom;
    if (v & (1u << (32 - c))) be_carry(e);
    v <<= c & 7;
    c >>= 3;
    while (--c >= 0) v <<= 8;
    for (c = 0; c < 4; c++) {
        buf_byte(&e->out, (uint8_t)(v >> 24));
        v <<= 8;
    }
}

/* ---------------------------------------------------------------- encoder state */
typedef struct {
    int q;                /* quantizer index */
    int16_t dq[3][2];     /* decoder dequant factors: [y1, y2, uv][dc, ac] */
    uint32_t iq[3][2];    /* 2^16 / dq */
    uint32_t bias[3][2];  /* rounding offset (16.16) */
    int lam16, lam4, lamuv; /* lambdas: SSE units per bit */
    int alpha, beta, lf;
} segq_t;

typedef struct {
    uint8_t seg, skip, is_i4, ymode, uvmode, bmodes[16];
} mbm_t;

/* quantized levels of one macroblock in zigzag order: 0..15 Y, 16..19 U, 20..23 V, 24 Y2 */
typedef int16_t mblv_t[25][16];

typedef struct {
    int w, h, mbw, mbh;
    uint8_t *ys, *us, *vs; /* source planes, padded to macroblocks */
    int ystr, uvstr;
    vp8_planes_t rec;
    segq_t sq[NSEG];
    int nseg, uv_ac_delta;
    uint8_t seg_prob[3];
    mbm_t *mb;
    mblv_t *lv;
    uint8_t prob[4][8][3][11];
    bool upd[4][8][3][11];
    bool use_skip;
    int skip_prob;
    /* costs in 1/256 bit */
    uint16_t c0[256], c1[256]; /* bit 0 / bit 1 at probability p (of a 0) */
    uint16_t tokc[4][8][3][11];
    uint16_t eobc[4][8][3][2];
    uint16_t extra[MAX_LEVEL + 1];
    uint16_t bmc[10][10][10]; /* [above][left][mode] */
    uint16_t ymc[5], uvmc[4];
    uint32_t stats[4][8][3][11][2];
} enc_t;

static inline uint8_t clip8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* ---------------------------------------------------------------- color conversion */
/* BT.601 studio swing, the inverse of the decoder's YUV -> RGB (16.16 fixed point) */
static void to_yuv(enc_t *e, const img_t *im, bool alpha) {
    int W = e->mbw * 16, H = e->mbh * 16;
    e->ystr = W, e->uvstr = W / 2;
    e->ys = xmalloc((size_t)W * H);
    e->us = xmalloc((size_t)W * H / 4);
    e->vs = xmalloc((size_t)W * H / 4);
    for (int y = 0; y < H; y++) {
        const uint8_t *row = im->px + (size_t)MIN(y, im->h - 1) * im->w * 4;
        uint8_t *o = e->ys + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            const uint8_t *p = row + (size_t)MIN(x, im->w - 1) * 4;
            o[x] = (uint8_t)((16829 * p[0] + 33039 * p[1] + 6416 * p[2] + (16 << 16) + 32768) >> 16);
        }
    }
    for (int y = 0; y < H / 2; y++) {
        const uint8_t *r0 = im->px + (size_t)MIN(2 * y, im->h - 1) * im->w * 4;
        const uint8_t *r1 = im->px + (size_t)MIN(2 * y + 1, im->h - 1) * im->w * 4;
        for (int x = 0; x < W / 2; x++) {
            size_t a = (size_t)MIN(2 * x, im->w - 1) * 4, b = (size_t)MIN(2 * x + 1, im->w - 1) * 4;
            const uint8_t *q[4] = {r0 + a, r0 + b, r1 + a, r1 + b};
            int R = 0, G = 0, B = 0, sa = alpha ? q[0][3] + q[1][3] + q[2][3] + q[3][3] : 1020;
            if (sa > 0 && sa < 1020) { /* alpha-weighted: invisible pixels do not tint the chroma */
                for (int k = 0; k < 4; k++) R += q[k][0] * q[k][3], G += q[k][1] * q[k][3], B += q[k][2] * q[k][3];
                R = (R * 4 + sa / 2) / sa, G = (G * 4 + sa / 2) / sa, B = (B * 4 + sa / 2) / sa;
            } else {
                for (int k = 0; k < 4; k++) R += q[k][0], G += q[k][1], B += q[k][2];
            }
            e->us[(size_t)y * e->uvstr + x] = clip8((-9714 * R - 19071 * G + 28784 * B + (128 << 18) + (1 << 17)) >> 18);
            e->vs[(size_t)y * e->uvstr + x] = clip8((28784 * R - 24103 * G - 4681 * B + (128 << 18) + (1 << 17)) >> 18);
        }
    }
}

static void fill_block(uint8_t *p, int stride, int n, int v) {
    for (int y = 0; y < n; y++) memset(p + y * stride, v, (size_t)n);
}

static int block_mean(const uint8_t *p, int stride, int n) {
    int sum = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) sum += p[y * stride + x];
    return (sum + n * n / 2) / (n * n);
}

/* Pixels under alpha 0 are invisible (libwebp's default, non-"exact" mode does the same): in partly
 * transparent 8x8 blocks they take the mean luma of the visible pixels, and runs of fully transparent
 * blocks are flattened to one value, so neither costs bits. */
static void cleanup_transparent(enc_t *e, const img_t *im) {
    int W = e->mbw * 16, H = e->mbh * 16;
    for (int by = 0; by < H; by += 8) {
        int run_y = -1, run_u = 0, run_v = 0;
        for (int bx = 0; bx < W; bx += 8) {
            uint8_t *yp = e->ys + (size_t)by * W + bx;
            uint8_t *up = e->us + (size_t)(by / 2) * e->uvstr + bx / 2, *vp = e->vs + (size_t)(by / 2) * e->uvstr + bx / 2;
            int n = 0, sum = 0;
            bool clear[64];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int a = im->px[((size_t)MIN(by + y, im->h - 1) * im->w + MIN(bx + x, im->w - 1)) * 4 + 3];
                    clear[y * 8 + x] = !a;
                    if (a) n++, sum += yp[y * W + x];
                }
            if (n == 0) {
                if (run_y < 0) run_y = block_mean(yp, W, 8), run_u = block_mean(up, e->uvstr, 4), run_v = block_mean(vp, e->uvstr, 4);
                fill_block(yp, W, 8, run_y);
                fill_block(up, e->uvstr, 4, run_u);
                fill_block(vp, e->uvstr, 4, run_v);
                continue;
            }
            run_y = -1;
            if (n < 64)
                for (int k = 0; k < 64; k++)
                    if (clear[k]) yp[(k >> 3) * W + (k & 7)] = (uint8_t)(sum / n);
        }
    }
}

/* ---------------------------------------------------------------- costs */
static void tree_bits(const int8_t *tree, int leaf, uint32_t *bits, int *len) {
    for (uint32_t pat = 0; pat < 256; pat++) {
        int i = 0, l = 0;
        do i = tree[i + ((pat >> (7 - l)) & 1)], l++;
        while (i > 0 && l < 8);
        if (i <= 0 && -i == leaf) {
            *bits = pat >> (8 - l), *len = l;
            return;
        }
    }
    *bits = 0, *len = 0;
}

static const int8_t ymode_tree[8] = {-VP8_B_PRED, 2, 4, 6, -VP8_DC_PRED, -VP8_V_PRED, -VP8_H_PRED, -VP8_TM_PRED};
static const int8_t uvmode_tree[6] = {-VP8_DC_PRED, 2, -VP8_V_PRED, 4, -VP8_H_PRED, -VP8_TM_PRED};
static const int8_t bmode_tree[18] = {-B_DC_PRED, 2, -B_TM_PRED, 4, -B_VE_PRED, 6, 8, 12, -B_HE_PRED, 10, -B_RD_PRED, -B_VR_PRED, -B_LD_PRED, 14, -B_VL_PRED, 16, -B_HD_PRED, -B_HU_PRED};

static void tree_put(benc_t *b, const int8_t *tree, const uint8_t *probs, int leaf) {
    uint32_t bits;
    int len, i = 0;
    tree_bits(tree, leaf, &bits, &len);
    for (int k = len - 1; k >= 0; k--) {
        int bit = (bits >> k) & 1;
        be_put(b, probs[i >> 1], bit);
        i = tree[i + bit];
    }
}

static int tree_cost(const enc_t *e, const int8_t *tree, const uint8_t *probs, int leaf) {
    uint32_t bits;
    int len, i = 0, c = 0;
    tree_bits(tree, leaf, &bits, &len);
    for (int k = len - 1; k >= 0; k--) {
        int bit = (bits >> k) & 1;
        c += bit ? e->c1[probs[i >> 1]] : e->c0[probs[i >> 1]];
        i = tree[i + bit];
    }
    return c;
}

static const int cat_base[6] = {5, 7, 11, 19, 35, 67};

static inline int level_token(int v) {
    if (v <= 4) return v;
    if (v <= 6) return 5;
    if (v <= 10) return 6;
    if (v <= 18) return 7;
    if (v <= 34) return 8;
    if (v <= 66) return 9;
    return 10;
}

static void init_static_costs(enc_t *e) {
    for (int p = 0; p < 256; p++) {
        double pr = (p ? p : 1) / 256.0;
        e->c0[p] = (uint16_t)lrint(-log2(pr) * 256);
        e->c1[p] = (uint16_t)lrint(-log2(1 - pr) * 256);
    }
    for (int v = 0; v <= MAX_LEVEL; v++) {
        int t = level_token(v), c = 0;
        if (t >= 5) {
            const uint8_t *pc = vp8_pcat[t - 5];
            int nb = 0;
            while (pc[nb]) nb++;
            int x = v - cat_base[t - 5];
            for (int k = 0; k < nb; k++) c += ((x >> (nb - 1 - k)) & 1) ? e->c1[pc[k]] : e->c0[pc[k]];
        }
        e->extra[v] = (uint16_t)(c + (v ? 256 : 0)); /* + sign */
    }
    static const uint8_t kfy[4] = {145, 156, 163, 128};
    for (int m = 0; m <= VP8_B_PRED; m++) e->ymc[m] = (uint16_t)tree_cost(e, ymode_tree, kfy, m);
    for (int m = 0; m < 4; m++) e->uvmc[m] = (uint16_t)tree_cost(e, uvmode_tree, vp8_kf_uv_mode_prob, m);
    for (int a = 0; a < 10; a++)
        for (int l = 0; l < 10; l++)
            for (int m = 0; m < 10; m++) e->bmc[a][l][m] = (uint16_t)tree_cost(e, bmode_tree, vp8_kf_bmode_prob[a][l], m);
}

/* token path costs for the current probabilities */
static void init_token_costs(enc_t *e) {
    for (int t = 0; t < 4; t++)
        for (int b = 0; b < 8; b++)
            for (int c = 0; c < 3; c++) {
                const uint8_t *p = e->prob[t][b][c];
                uint16_t *o = e->tokc[t][b][c];
                const uint16_t *c0 = e->c0, *c1 = e->c1;
                e->eobc[t][b][c][0] = c0[p[0]], e->eobc[t][b][c][1] = c1[p[0]];
                int nz = c1[p[1]], big = nz + c1[p[2]], low = big + c0[p[3]], high = big + c1[p[3]];
                o[0] = c0[p[1]];
                o[1] = (uint16_t)(nz + c0[p[2]]);
                o[2] = (uint16_t)(low + c0[p[4]]);
                o[3] = (uint16_t)(low + c1[p[4]] + c0[p[5]]);
                o[4] = (uint16_t)(low + c1[p[4]] + c1[p[5]]);
                o[5] = (uint16_t)(high + c0[p[6]] + c0[p[7]]);
                o[6] = (uint16_t)(high + c0[p[6]] + c1[p[7]]);
                o[7] = (uint16_t)(high + c1[p[6]] + c0[p[8]] + c0[p[9]]);
                o[8] = (uint16_t)(high + c1[p[6]] + c0[p[8]] + c1[p[9]]);
                o[9] = (uint16_t)(high + c1[p[6]] + c1[p[8]] + c0[p[10]]);
                o[10] = (uint16_t)(high + c1[p[6]] + c1[p[8]] + c1[p[10]]);
            }
}

static inline int last_nz(const int16_t *lv) {
    for (int n = 15; n >= 0; n--)
        if (lv[n]) return n;
    return -1;
}

/* bits (1/256) to code one block of zigzag levels */
static int block_cost(const enc_t *e, int type, int ctx, const int16_t *lv, int first) {
    int last = last_nz(lv);
    if (last < first) return e->eobc[type][vp8_bands[first]][ctx][0];
    int cost = 0;
    bool prev_zero = false;
    for (int n = first; n <= last; n++) {
        int b = vp8_bands[n], v = abs(lv[n]);
        if (!prev_zero) cost += e->eobc[type][b][ctx][1];
        cost += e->tokc[type][b][ctx][level_token(v > MAX_LEVEL ? MAX_LEVEL : v)] + e->extra[v > MAX_LEVEL ? MAX_LEVEL : v];
        ctx = v == 0 ? 0 : v == 1 ? 1 : 2;
        prev_zero = v == 0;
    }
    if (last < 15) cost += e->eobc[type][vp8_bands[last + 1]][ctx][0];
    return cost;
}

/* ---------------------------------------------------------------- token coding */
/* Write (b != NULL) or count (stats) the tokens of one block; returns the "has data" flag. */
static int put_block(enc_t *e, benc_t *b, int type, int ctx, const int16_t *lv, int first) {
    int last = last_nz(lv);
#define PUT(pi, bit)                                                                                                   \
    do {                                                                                                               \
        if (b) be_put(b, p[pi], bit);                                                                                  \
        else e->stats[type][band][ctx][pi][bit]++;                                                                     \
    } while (0)
    int n = first, band = vp8_bands[n];
    const uint8_t *p = e->prob[type][band][ctx];
    if (last < first) {
        PUT(0, 0);
        return 0;
    }
    bool prev_zero = false;
    for (; n <= last; n++) {
        band = vp8_bands[n];
        p = e->prob[type][band][ctx];
        int v = abs(lv[n]);
        if (!prev_zero) PUT(0, 1);
        if (v == 0) {
            PUT(1, 0);
            ctx = 0, prev_zero = true;
            continue;
        }
        PUT(1, 1);
        prev_zero = false;
        if (v == 1) {
            PUT(2, 0);
            ctx = 1;
        } else {
            PUT(2, 1);
            if (v <= 4) {
                PUT(3, 0);
                if (v == 2) PUT(4, 0);
                else {
                    PUT(4, 1);
                    PUT(5, v == 4);
                }
            } else {
                PUT(3, 1);
                int t = level_token(v);
                if (t <= 6) {
                    PUT(6, 0);
                    PUT(7, t == 6);
                } else {
                    PUT(6, 1);
                    if (t <= 8) {
                        PUT(8, 0);
                        PUT(9, t == 8);
                    } else {
                        PUT(8, 1);
                        PUT(10, t == 10);
                    }
                }
                if (b) {
                    const uint8_t *pc = vp8_pcat[t - 5];
                    int nb = 0;
                    while (pc[nb]) nb++;
                    int x = v - cat_base[t - 5];
                    for (int k = 0; k < nb; k++) be_put(b, pc[k], (x >> (nb - 1 - k)) & 1);
                }
            }
            ctx = 2;
        }
        if (b) be_put(b, 128, lv[n] < 0);
    }
    if (last < 15) {
        band = vp8_bands[last + 1];
        p = e->prob[type][band][ctx];
        PUT(0, 0);
    }
#undef PUT
    return 1;
}

/* Tokens of every macroblock in raster order (with the decoder's context rules). */
static void put_tokens(enc_t *e, benc_t *b) {
    uint8_t *tnz = xcalloc((size_t)e->mbw, 9), lnz[9];
    for (int mby = 0; mby < e->mbh; mby++) {
        memset(lnz, 0, sizeof lnz);
        for (int mbx = 0; mbx < e->mbw; mbx++) {
            size_t idx = (size_t)mby * e->mbw + mbx;
            const mbm_t *m = &e->mb[idx];
            int16_t(*lv)[16] = e->lv[idx];
            uint8_t *t = tnz + mbx * 9;
            if (m->skip && e->use_skip) {
                memset(t, 0, 8), memset(lnz, 0, 8);
                if (!m->is_i4) t[8] = lnz[8] = 0;
                continue;
            }
            int first = 0, type = 3;
            if (!m->is_i4) {
                t[8] = lnz[8] = (uint8_t)put_block(e, b, 1, t[8] + lnz[8], lv[24], 0);
                first = 1, type = 0;
            }
            for (int i = 0; i < 16; i++) {
                int x = i & 3, y = i >> 2;
                t[x] = lnz[y] = (uint8_t)put_block(e, b, type, t[x] + lnz[y], lv[i], first);
            }
            for (int i = 0; i < 8; i++) {
                int c = i >> 2, x = i & 1, y = (i >> 1) & 1;
                uint8_t *tt = t + 4 + c * 2 + x, *ll = lnz + 4 + c * 2 + y;
                *tt = *ll = (uint8_t)put_block(e, b, 2, *tt + *ll, lv[16 + i], 0);
            }
        }
    }
    free(tnz);
}

/* Choose the token probabilities (with update flags) from the gathered statistics. */
static void update_probs(enc_t *e) {
    memset(e->stats, 0, sizeof e->stats);
    put_tokens(e, NULL);
    for (int t = 0; t < 4; t++)
        for (int b = 0; b < 8; b++)
            for (int c = 0; c < 3; c++)
                for (int i = 0; i < 11; i++) {
                    uint32_t n0 = e->stats[t][b][c][i][0], n1 = e->stats[t][b][c][i][1], tot = n0 + n1;
                    int old = vp8_default_coeff_prob[t][b][c][i], up = vp8_coeff_update_prob[t][b][c][i];
                    e->prob[t][b][c][i] = (uint8_t)old, e->upd[t][b][c][i] = false;
                    if (!tot) continue;
                    int np = (int)((n0 * 256 + tot / 2) / tot);
                    np = np < 1 ? 1 : np > 255 ? 255 : np;
                    uint64_t keep = (uint64_t)n0 * e->c0[old] + (uint64_t)n1 * e->c1[old] + e->c0[up];
                    uint64_t chg = (uint64_t)n0 * e->c0[np] + (uint64_t)n1 * e->c1[np] + e->c1[up] + 8 * 256;
                    if (chg < keep) e->prob[t][b][c][i] = (uint8_t)np, e->upd[t][b][c][i] = true;
                }
    init_token_costs(e);
}

/* ---------------------------------------------------------------- quantization */
static void set_quant(segq_t *s, int q, int uv_ac_delta) {
#define QDC(v) vp8_dc_q[(v) < 0 ? 0 : (v) > 127 ? 127 : (v)]
#define QAC(v) vp8_ac_q[(v) < 0 ? 0 : (v) > 127 ? 127 : (v)]
    s->q = q;
    s->dq[0][0] = QDC(q), s->dq[0][1] = QAC(q);
    s->dq[1][0] = (int16_t)(QDC(q) * 2), s->dq[1][1] = (int16_t)MAX(QAC(q) * 155 / 100, 8);
    s->dq[2][0] = (int16_t)MIN(QDC(q - 2), 132), s->dq[2][1] = QAC(q + uv_ac_delta);
#undef QDC
#undef QAC
    /* dead-zone rounding (fractions of the step): coarser for luma AC than for DC and chroma */
    static const double rnd[3][2] = {{0.38, 0.42}, {0.38, 0.42}, {0.42, 0.44}};
    for (int t = 0; t < 3; t++)
        for (int k = 0; k < 2; k++) {
            s->iq[t][k] = (uint32_t)((1u << 16) / s->dq[t][k]);
            s->bias[t][k] = (uint32_t)(rnd[t][k] * (1 << 16));
        }
    int qa = s->dq[0][1];
    s->lam4 = MAX(1, qa * qa * 3 / 64);
    s->lam16 = MAX(1, qa * qa * 3 / 64);
    s->lamuv = MAX(1, s->dq[2][1] * s->dq[2][1] * 3 / 64);
}

/* Quantize raster coefficients `in` into zigzag levels; `deq` receives the dequantized raster block.
 * Returns the last non-zero scan position (or -1). */
static int quantize(const int16_t *in, int16_t *lv, int16_t *deq, const segq_t *s, int t, int first) {
    int last = -1;
    for (int n = 0; n < 16; n++) {
        int j = vp8_zigzag[n];
        if (n < first) {
            lv[n] = 0, deq[j] = 0;
            continue;
        }
        int k = n > 0, c = in[j], a = abs(c);
        int l = (int)(((uint32_t)a * s->iq[t][k] + s->bias[t][k]) >> 16);
        if (l > MAX_LEVEL) l = MAX_LEVEL;
        lv[n] = (int16_t)(c < 0 ? -l : l);
        deq[j] = (int16_t)(lv[n] * s->dq[t][k]);
        if (l) last = n;
    }
    return last;
}

static inline int sse_block(const uint8_t *a, int as, const uint8_t *b, int bs, int w, int h) {
    int s = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int d = a[y * as + x] - b[y * bs + x];
            s += d * d;
        }
    return s;
}

static inline void copy_block(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h) {
    for (int y = 0; y < h; y++) memcpy(dst + y * ds, src + y * ss, (size_t)w);
}

/* ---------------------------------------------------------------- mode decision */
typedef struct {
    uint8_t *tnz;   /* 9 per macroblock column */
    uint8_t lnz[9];
    uint8_t *tmodes; /* 4 per column */
    uint8_t lmodes[4];
} ctx_t;

static const uint8_t implied_bmode[4] = {B_DC_PRED, B_VE_PRED, B_HE_PRED, B_TM_PRED};

static void decide_mb(enc_t *e, vp8_mbbuf_t *wb, ctx_t *cx, int mbx, int mby) {
    size_t idx = (size_t)mby * e->mbw + mbx;
    mbm_t *m = &e->mb[idx];
    int16_t(*lv)[16] = e->lv[idx];
    const segq_t *s = &e->sq[m->seg];
    uint8_t *tnz = cx->tnz + mbx * 9, *lnz = cx->lnz;
    vp8_load_edges(wb, &e->rec, mbx, mby);
    uint8_t *ydst = VP8_YP(wb);
    const uint8_t *ysrc = e->ys + (size_t)mby * 16 * e->ystr + mbx * 16;
    bool top = mby > 0, left = mbx > 0;

    /* ---- I16 ---- */
    int64_t best16 = INT64_MAX;
    int16_t blv16[17][16];
    uint8_t brec16[256];
    int bmode16 = 0;
    for (int mode = 0; mode < 4; mode++) {
        int16_t coef[16][16], dc[16 * 16], deq[16], y2deq[16], dcq[16 * 16], tlv[17][16];
        uint8_t rec[256];
        vp8_pred16(ydst, mode, top, left);
        copy_block(rec, 16, ydst, BPS, 16, 16);
        for (int i = 0; i < 16; i++) {
            int bx = (i & 3) * 4, by = (i >> 2) * 4;
            vp8_fdct(ysrc + by * e->ystr + bx, e->ystr, ydst + by * BPS + bx, BPS, coef[i]);
            dc[i * 16] = coef[i][0];
        }
        int16_t y2[16];
        vp8_fwht(dc, y2);
        quantize(y2, tlv[16], y2deq, s, 1, 0);
        vp8_iwht(y2deq, dcq);
        int rate = e->ymc[mode] + block_cost(e, 1, tnz[8] + lnz[8], tlv[16], 0);
        uint8_t t4[4], l4[4];
        memcpy(t4, tnz, 4), memcpy(l4, lnz, 4);
        for (int i = 0; i < 16; i++) {
            int bx = (i & 3) * 4, by = (i >> 2) * 4;
            int last = quantize(coef[i], tlv[i], deq, s, 0, 1);
            deq[0] = dcq[i * 16];
            rate += block_cost(e, 0, t4[i & 3] + l4[i >> 2], tlv[i], 1);
            t4[i & 3] = l4[i >> 2] = last >= 1;
            if (last >= 1 || deq[0]) vp8_idct_add(deq, rec + by * 16 + bx, 16);
        }
        int64_t j = (int64_t)sse_block(ysrc, e->ystr, rec, 16, 16, 16) * 256 + (int64_t)s->lam16 * rate;
        if (j < best16) {
            best16 = j, bmode16 = mode;
            memcpy(blv16, tlv, sizeof blv16);
            memcpy(brec16, rec, sizeof brec16);
        }
    }

    /* ---- B_PRED ---- */
    int64_t j4 = (int64_t)s->lam4 * e->ymc[VP8_B_PRED];
    uint8_t t4[4], l4[4], tm[4], lm[4], bm[16];
    int16_t blv4[16][16];
    memcpy(t4, tnz, 4), memcpy(l4, lnz, 4);
    memcpy(tm, cx->tmodes + mbx * 4, 4), memcpy(lm, cx->lmodes, 4);
    for (int i = 0; i < 16 && j4 < best16; i++) {
        int x = i & 3, y = i >> 2;
        uint8_t *dst = ydst + y * 4 * BPS + x * 4;
        const uint8_t *src = ysrc + y * 4 * e->ystr + x * 4;
        int64_t bj = INT64_MAX;
        uint8_t brec[16];
        int bmode = 0;
        for (int mode = 0; mode < VP8_NUM_BMODES; mode++) {
            int16_t coef[16], deq[16], tlv[16];
            uint8_t rec[16];
            vp8_pred4(dst, mode);
            copy_block(rec, 4, dst, BPS, 4, 4);
            vp8_fdct(src, e->ystr, dst, BPS, coef);
            int last = quantize(coef, tlv, deq, s, 0, 0);
            if (last >= 0) vp8_idct_add(deq, rec, 4);
            int rate = e->bmc[tm[x]][lm[y]][mode] + block_cost(e, 3, t4[x] + l4[y], tlv, 0);
            int64_t j = (int64_t)sse_block(src, e->ystr, rec, 4, 4, 4) * 256 + (int64_t)s->lam4 * rate;
            if (j < bj) {
                bj = j, bmode = mode;
                memcpy(brec, rec, 16);
                memcpy(blv4[i], tlv, sizeof tlv);
            }
        }
        copy_block(dst, BPS, brec, 4, 4, 4);
        t4[x] = l4[y] = last_nz(blv4[i]) >= 0;
        tm[x] = lm[y] = bm[i] = (uint8_t)bmode;
        j4 += bj;
    }
    if (j4 < best16) {
        m->is_i4 = 1, m->ymode = VP8_B_PRED;
        memcpy(m->bmodes, bm, 16);
        memcpy(lv, blv4, 16 * sizeof lv[0]);
        memset(lv[24], 0, sizeof lv[24]);
        memcpy(cx->tmodes + mbx * 4, tm, 4), memcpy(cx->lmodes, lm, 4);
    } else {
        m->is_i4 = 0, m->ymode = (uint8_t)bmode16;
        copy_block(ydst, BPS, brec16, 16, 16, 16);
        memcpy(lv, blv16, 16 * sizeof lv[0]);
        memcpy(lv[24], blv16[16], sizeof lv[24]);
        memset(cx->tmodes + mbx * 4, implied_bmode[bmode16], 4), memset(cx->lmodes, implied_bmode[bmode16], 4);
    }

    /* ---- chroma ---- */
    uint8_t *udst = VP8_UP(wb), *vdst = VP8_VP(wb);
    const uint8_t *usrc = e->us + (size_t)mby * 8 * e->uvstr + mbx * 8, *vsrc = e->vs + (size_t)mby * 8 * e->uvstr + mbx * 8;
    int64_t bestuv = INT64_MAX;
    int16_t blvuv[8][16];
    uint8_t brecu[64], brecv[64];
    int bmodeuv = 0;
    for (int mode = 0; mode < 4; mode++) {
        int16_t tlv[8][16];
        uint8_t recu[64], recv[64];
        vp8_pred8(udst, mode, top, left);
        vp8_pred8(vdst, mode, top, left);
        copy_block(recu, 8, udst, BPS, 8, 8);
        copy_block(recv, 8, vdst, BPS, 8, 8);
        int rate = e->uvmc[mode], dist = 0;
        uint8_t tt[4], ll[4];
        memcpy(tt, tnz + 4, 4), memcpy(ll, lnz + 4, 4);
        for (int i = 0; i < 8; i++) {
            int c = i >> 2, x = i & 1, y = (i >> 1) & 1;
            const uint8_t *src = (c ? vsrc : usrc) + y * 4 * e->uvstr + x * 4;
            const uint8_t *pred = (c ? vdst : udst) + y * 4 * BPS + x * 4;
            uint8_t *rec = (c ? recv : recu) + y * 4 * 8 + x * 4;
            int16_t coef[16], deq[16];
            vp8_fdct(src, e->uvstr, pred, BPS, coef);
            int last = quantize(coef, tlv[i], deq, s, 2, 0);
            if (last >= 0) vp8_idct_add(deq, rec, 8);
            uint8_t *a = tt + c * 2 + x, *l = ll + c * 2 + y;
            rate += block_cost(e, 2, *a + *l, tlv[i], 0);
            *a = *l = last >= 0;
        }
        dist = sse_block(usrc, e->uvstr, recu, 8, 8, 8) + sse_block(vsrc, e->uvstr, recv, 8, 8, 8);
        int64_t j = (int64_t)dist * 256 + (int64_t)s->lamuv * rate;
        if (j < bestuv) {
            bestuv = j, bmodeuv = mode;
            memcpy(blvuv, tlv, sizeof blvuv);
            memcpy(brecu, recu, 64), memcpy(brecv, recv, 64);
        }
    }
    m->uvmode = (uint8_t)bmodeuv;
    memcpy(lv[16], blvuv, sizeof blvuv);
    copy_block(udst, BPS, brecu, 8, 8, 8);
    copy_block(vdst, BPS, brecv, 8, 8, 8);

    /* ---- contexts for the next macroblocks ---- */
    bool any = false;
    int first = m->is_i4 ? 0 : 1;
    for (int i = 0; i < 16; i++) {
        bool nz = last_nz(lv[i]) >= first;
        tnz[i & 3] = lnz[i >> 2] = nz;
        any |= nz;
    }
    for (int i = 0; i < 8; i++) {
        int c = i >> 2, x = i & 1, y = (i >> 1) & 1;
        bool nz = last_nz(lv[16 + i]) >= 0;
        tnz[4 + c * 2 + x] = lnz[4 + c * 2 + y] = nz;
        any |= nz;
    }
    if (!m->is_i4) {
        bool nz = last_nz(lv[24]) >= 0;
        tnz[8] = lnz[8] = nz;
        any |= nz;
    }
    m->skip = !any;
    vp8_store_mb(wb, &e->rec, mbx, mby);
}

static void encode_pass(enc_t *e) {
    vp8_mbbuf_t *wb = xmalloc(sizeof *wb);
    ctx_t cx = {.tnz = xcalloc((size_t)e->mbw, 9), .tmodes = xcalloc((size_t)e->mbw, 4)};
    for (int mby = 0; mby < e->mbh; mby++) {
        memset(cx.lnz, 0, sizeof cx.lnz);
        memset(cx.lmodes, 0, sizeof cx.lmodes);
        for (int mbx = 0; mbx < e->mbw; mbx++) decide_mb(e, wb, &cx, mbx, mby);
    }
    free(cx.tnz), free(cx.tmodes), free(wb);
}

/* ---------------------------------------------------------------- analysis / segments */
/* Susceptibility of a block set: 255 for perfectly predicted (flat) content, lower for texture. The
 * DCT magnitudes of the residual are histogrammed; a spread-out histogram means a busy block. */
static int hist_alpha(const int *hist) {
    int maxv = 0, last = 0;
    for (int k = 0; k < 32; k++)
        if (hist[k]) {
            maxv = MAX(maxv, hist[k]);
            last = k;
        }
    return maxv > 1 ? 256 * last / maxv : 0;
}

static void collect_hist(const uint8_t *src, int ss, const uint8_t *pred, int ps, int bw, int bh, int *hist) {
    for (int by = 0; by < bh; by += 4)
        for (int bx = 0; bx < bw; bx += 4) {
            int16_t c[16];
            vp8_fdct(src + by * ss + bx, ss, pred + by * ps + bx, ps, c);
            for (int k = 0; k < 16; k++) hist[MIN(abs(c[k]) >> 3, 31)]++;
        }
}

static void analyze(enc_t *e, int quality) {
    int nmb = e->mbw * e->mbh;
    uint8_t *alphas = xmalloc((size_t)nmb);
    vp8_mbbuf_t *wb = xmalloc(sizeof *wb);
    /* predictions come from the source itself (no reconstruction yet) */
    vp8_planes_t src = {.y = e->ys, .u = e->us, .v = e->vs, .ys = e->ystr, .uvs = e->uvstr, .mbw = e->mbw, .mbh = e->mbh};
    long uv_sum = 0;
    for (int mby = 0; mby < e->mbh; mby++)
        for (int mbx = 0; mbx < e->mbw; mbx++) {
            vp8_load_edges(wb, &src, mbx, mby);
            const uint8_t *ys = e->ys + (size_t)mby * 16 * e->ystr + mbx * 16;
            const uint8_t *us = e->us + (size_t)mby * 8 * e->uvstr + mbx * 8, *vs = e->vs + (size_t)mby * 8 * e->uvstr + mbx * 8;
            int ay = 1 << 30, auv = 1 << 30;
            for (int mode = 0; mode < 4; mode += 3) { /* DC and TM */
                int hist[32] = {0}, hu[32] = {0};
                vp8_pred16(VP8_YP(wb), mode, mby > 0, mbx > 0);
                collect_hist(ys, e->ystr, VP8_YP(wb), BPS, 16, 16, hist);
                ay = MIN(ay, hist_alpha(hist));
                vp8_pred8(VP8_UP(wb), mode, mby > 0, mbx > 0);
                vp8_pred8(VP8_VP(wb), mode, mby > 0, mbx > 0);
                collect_hist(us, e->uvstr, VP8_UP(wb), BPS, 8, 8, hu);
                collect_hist(vs, e->uvstr, VP8_VP(wb), BPS, 8, 8, hu);
                auv = MIN(auv, hist_alpha(hu));
            }
            uv_sum += auv;
            int a = (3 * ay + auv + 2) >> 2;
            a = 255 - MIN(a, 255);
            alphas[(size_t)mby * e->mbw + mbx] = (uint8_t)a;
        }
    free(wb);
    /* k-means on the alpha histogram */
    int hist[256] = {0}, mn = 255, mx = 0;
    for (int i = 0; i < nmb; i++) hist[alphas[i]]++, mn = MIN(mn, alphas[i]), mx = MAX(mx, alphas[i]);
    int ns = NSEG;
    if (mx == mn) ns = 1;
    double centers[NSEG];
    int assign[256];
    for (int k = 0; k < ns; k++) centers[k] = mn + (mx - mn) * (k + 0.5) / ns;
    double wavg = 0;
    for (int it = 0; it < 8; it++) {
        double sum[NSEG] = {0}, cnt[NSEG] = {0};
        wavg = 0;
        for (int a = mn; a <= mx; a++) {
            if (!hist[a]) continue;
            int best = 0;
            for (int k = 1; k < ns; k++)
                if (fabs(a - centers[k]) < fabs(a - centers[best])) best = k;
            assign[a] = best;
            sum[best] += (double)a * hist[a], cnt[best] += hist[a];
        }
        for (int k = 0; k < ns; k++) {
            if (cnt[k]) centers[k] = sum[k] / cnt[k];
            wavg += centers[k] * cnt[k];
        }
        wavg /= nmb;
    }
    for (int a = mn; a <= mx; a++)
        if (hist[a]) {
            int best = 0;
            for (int k = 1; k < ns; k++)
                if (fabs(a - centers[k]) < fabs(a - centers[best])) best = k;
            assign[a] = best;
        }
    int cnt[NSEG] = {0};
    for (int i = 0; i < nmb; i++) {
        e->mb[i].seg = (uint8_t)assign[alphas[i]];
        cnt[e->mb[i].seg]++;
    }
    e->nseg = ns;
    /* quality curve (libwebp): Q -> linear compressibility -> cube root; SNS modulates the exponent */
    double Q = quality / 100.0;
    double lin = Q < 0.75 ? Q * (2.0 / 3.0) : 2.0 * Q - 1.0;
    double cbase = pow(lin, 1.0 / 3.0);
    const double amp = 0.9 * 50 / 100.0 / 128.0;
    /* chroma AC delta from the chroma susceptibility: flat chroma is quantized finer */
    int uva = (int)(uv_sum / nmb);
    int duv = (uva - 64) * 10 / 70;
    duv = duv * 50 / 100;
    e->uv_ac_delta = duv < -4 ? -4 : duv > 6 ? 6 : duv;
    for (int k = 0; k < ns; k++) {
        segq_t *s = &e->sq[k];
        double span = mx > mn ? (double)(mx - mn) : 1;
        s->alpha = ns > 1 ? (int)(255 * (centers[k] - wavg) / span) : 0;
        s->beta = ns > 1 ? (int)(255 * (centers[k] - mn) / span) : 0;
        s->alpha = s->alpha < -127 ? -127 : s->alpha > 127 ? 127 : s->alpha;
        s->beta = s->beta < 0 ? 0 : s->beta > 255 ? 255 : s->beta;
        double c = pow(cbase, 1.0 - amp * s->alpha);
        int q = (int)(127.0 * (1.0 - c));
        set_quant(s, q < 0 ? 0 : q > 127 ? 127 : q, e->uv_ac_delta);
        /* loop filter: strength ~ AC step / 4, weakened for smooth segments (strength 60) */
        int delta = vp8_ac_q[s->q] >> 2;
        int f = delta * 5 * 60 / (256 + s->beta);
        s->lf = f < 2 ? 0 : f > 63 ? 63 : f;
    }
    /* segment map probabilities */
    if (ns > 1) {
        int n01 = cnt[0] + cnt[1], n23 = cnt[2] + cnt[3];
        e->seg_prob[0] = (uint8_t)(nmb ? MAX(1, MIN(255, (n01 * 255 + nmb / 2) / nmb)) : 255);
        e->seg_prob[1] = (uint8_t)(n01 ? MAX(1, MIN(255, (cnt[0] * 255 + n01 / 2) / n01)) : 255);
        e->seg_prob[2] = (uint8_t)(n23 ? MAX(1, MIN(255, (cnt[2] * 255 + n23 / 2) / n23)) : 255);
    }
    free(alphas);
}

/* ---------------------------------------------------------------- bitstream */
static void put_header(enc_t *e, benc_t *b) {
    be_put(b, 128, 0); /* color space */
    be_put(b, 128, 0); /* clamping required */
    be_put(b, 128, e->nseg > 1);
    if (e->nseg > 1) {
        be_put(b, 128, 1); /* update map */
        be_put(b, 128, 1); /* update data */
        be_put(b, 128, 1); /* absolute values */
        for (int k = 0; k < 4; k++) {
            be_put(b, 128, 1);
            be_signed(b, e->sq[k].q, 7);
        }
        for (int k = 0; k < 4; k++) {
            be_put(b, 128, 1);
            be_signed(b, e->sq[k].lf, 6);
        }
        for (int k = 0; k < 3; k++) {
            be_put(b, 128, 1);
            be_lit(b, e->seg_prob[k], 8);
        }
    }
    be_put(b, 128, 0); /* normal loop filter */
    be_lit(b, (uint32_t)e->sq[0].lf, 6);
    be_lit(b, 0, 3);   /* sharpness */
    be_put(b, 128, 0); /* no mode / reference deltas */
    be_lit(b, 0, 2);   /* one token partition */
    be_lit(b, (uint32_t)e->sq[0].q, 7);
    be_put(b, 128, 0); /* y dc delta */
    be_put(b, 128, 0); /* y2 dc */
    be_put(b, 128, 0); /* y2 ac */
    be_put(b, 128, 1); /* uv dc: -2 */
    be_signed(b, -2, 4);
    be_put(b, 128, e->uv_ac_delta != 0);
    if (e->uv_ac_delta) be_signed(b, e->uv_ac_delta, 4);
    be_put(b, 128, 0); /* refresh entropy probs */
    for (int t = 0; t < 4; t++)
        for (int bb = 0; bb < 8; bb++)
            for (int c = 0; c < 3; c++)
                for (int i = 0; i < 11; i++) {
                    bool u = e->upd[t][bb][c][i];
                    be_put(b, vp8_coeff_update_prob[t][bb][c][i], u);
                    if (u) be_lit(b, e->prob[t][bb][c][i], 8);
                }
    be_put(b, 128, e->use_skip);
    if (e->use_skip) be_lit(b, (uint32_t)e->skip_prob, 8);
}

static void put_modes(enc_t *e, benc_t *b) {
    static const uint8_t kfy[4] = {145, 156, 163, 128};
    uint8_t *tm = xcalloc((size_t)e->mbw, 4), lm[4];
    for (int mby = 0; mby < e->mbh; mby++) {
        memset(lm, 0, 4);
        for (int mbx = 0; mbx < e->mbw; mbx++) {
            const mbm_t *m = &e->mb[(size_t)mby * e->mbw + mbx];
            if (e->nseg > 1) {
                be_put(b, e->seg_prob[0], m->seg >= 2);
                if (m->seg < 2) be_put(b, e->seg_prob[1], m->seg == 1);
                else be_put(b, e->seg_prob[2], m->seg == 3);
            }
            if (e->use_skip) be_put(b, e->skip_prob, m->skip);
            uint8_t *t = tm + mbx * 4;
            tree_put(b, ymode_tree, kfy, m->ymode);
            if (m->is_i4) {
                for (int i = 0; i < 16; i++) {
                    int x = i & 3, y = i >> 2;
                    tree_put(b, bmode_tree, vp8_kf_bmode_prob[t[x]][lm[y]], m->bmodes[i]);
                    t[x] = lm[y] = m->bmodes[i];
                }
            } else {
                memset(t, implied_bmode[m->ymode], 4), memset(lm, implied_bmode[m->ymode], 4);
            }
            tree_put(b, uvmode_tree, vp8_kf_uv_mode_prob, m->uvmode);
        }
    }
    free(tm);
}

/* Decide skip signalling: on when the flags cost less than the empty blocks' end-of-block tokens. */
static void choose_skip(enc_t *e) {
    int nmb = e->mbw * e->mbh, nskip = 0;
    for (int i = 0; i < nmb; i++) nskip += e->mb[i].skip;
    e->use_skip = false;
    if (!nskip) return;
    int p = (int)(((int64_t)(nmb - nskip) * 256 + nmb / 2) / nmb);
    p = p < 1 ? 1 : p > 255 ? 255 : p;
    int64_t with = 8 * 256 + (int64_t)nskip * e->c1[p] + (int64_t)(nmb - nskip) * e->c0[p];
    /* an empty macroblock still codes ~25 end-of-block tokens in the lowest context */
    int64_t eob = 16 * e->eobc[3][0][0][0] + 8 * e->eobc[2][0][0][0] + e->eobc[1][0][0][0];
    int64_t without = (int64_t)nskip * eob;
    if (with < without) e->use_skip = true, e->skip_prob = p;
}

static void encode_frame(enc_t *e, buf_t *out) {
    benc_t p0, p1;
    be_init(&p0), be_init(&p1);
    put_header(e, &p0);
    put_modes(e, &p0);
    be_flush(&p0);
    put_tokens(e, &p1);
    be_flush(&p1);
    uint32_t part0 = (uint32_t)p0.out.n;
    uint32_t tag = 0 | (0 << 1) | (1 << 4) | (part0 << 5); /* key frame, version 0, shown */
    buf_le(out, tag, 3);
    buf_put(out, "\x9d\x01\x2a", 3);
    buf_le(out, (uint32_t)e->w, 2);
    buf_le(out, (uint32_t)e->h, 2);
    buf_put(out, p0.out.d, p0.out.n);
    buf_put(out, p1.out.d, p1.out.n);
    free(p0.out.d), free(p1.out.d);
}

/* ---------------------------------------------------------------- alpha (ALPH, lossless) */
static uint8_t *alpha_encode(const img_t *im, size_t *len);

/* ---------------------------------------------------------------- entry point */
uint8_t *img_webp_encode(const img_t *im, int quality, size_t *len) {
    *len = 0;
    if (!im || !im->px || im->w < 1 || im->h < 1 || im->w > 16383 || im->h > 16383) return NULL;
    quality = quality < 0 ? 0 : quality > 100 ? 100 : quality;
    enc_t *e = xcalloc(1, sizeof *e);
    e->w = im->w, e->h = im->h;
    e->mbw = (im->w + 15) / 16, e->mbh = (im->h + 15) / 16;
    int nmb = e->mbw * e->mbh;
    bool alpha = img_translucent(im);
    to_yuv(e, im, alpha);
    if (alpha) cleanup_transparent(e, im);
    e->rec = (vp8_planes_t){.ys = e->mbw * 16, .uvs = e->mbw * 8, .mbw = e->mbw, .mbh = e->mbh};
    e->rec.y = xmalloc((size_t)e->rec.ys * e->mbh * 16);
    e->rec.u = xmalloc((size_t)e->rec.uvs * e->mbh * 8);
    e->rec.v = xmalloc((size_t)e->rec.uvs * e->mbh * 8);
    e->mb = xcalloc((size_t)nmb, sizeof *e->mb);
    e->lv = xcalloc((size_t)nmb, sizeof *e->lv);
    init_static_costs(e);
    memcpy(e->prob, vp8_default_coeff_prob, sizeof e->prob);
    init_token_costs(e);
    analyze(e, quality);
    encode_pass(e);
    update_probs(e);
    encode_pass(e);
    update_probs(e);
    choose_skip(e);
    buf_t vp8 = {0};
    encode_frame(e, &vp8);

    buf_t out = {0};
    buf_put(&out, "RIFF\0\0\0\0WEBP", 12);
    if (alpha) {
        uint8_t x[10] = {0x10, 0, 0, 0};
        uint32_t cw = (uint32_t)im->w - 1, chh = (uint32_t)im->h - 1;
        x[4] = (uint8_t)cw, x[5] = (uint8_t)(cw >> 8), x[6] = (uint8_t)(cw >> 16);
        x[7] = (uint8_t)chh, x[8] = (uint8_t)(chh >> 8), x[9] = (uint8_t)(chh >> 16);
        buf_chunk(&out, "VP8X", x, 10);
        size_t an = 0;
        uint8_t *alph = alpha_encode(im, &an);
        buf_chunk(&out, "ALPH", alph, an);
        free(alph);
    }
    buf_chunk(&out, "VP8 ", vp8.d, vp8.n);
    uint32_t riff = (uint32_t)(out.n - 8);
    for (int i = 0; i < 4; i++) out.d[4 + i] = (uint8_t)(riff >> (8 * i));
    free(vp8.d);
    free(e->ys), free(e->us), free(e->vs);
    free(e->rec.y), free(e->rec.u), free(e->rec.v);
    free(e->mb), free(e->lv), free(e);
    *len = out.n;
    return out.d;
}


/* VP8L bit writer (LSB first) */
typedef struct {
    buf_t out;
    uint64_t acc;
    int n;
} lbw_t;

static void lw_bits(lbw_t *w, uint32_t v, int n) {
    w->acc |= (uint64_t)v << w->n;
    w->n += n;
    while (w->n >= 8) {
        buf_byte(&w->out, (uint8_t)w->acc);
        w->acc >>= 8, w->n -= 8;
    }
}

#define LSYM 280 /* green alphabet: 256 literals + 24 length prefixes (no color cache) */

typedef struct {
    uint8_t len[LSYM];
    uint16_t code[LSYM]; /* bit-reversed canonical code, written LSB first */
    int single;          /* >= 0: zero-bit code */
} hcode_t;

/* Huffman code lengths limited to maxlen (frequencies are halved until the tree fits). */
static void huff_lengths(const uint32_t *freq, int n, int maxlen, uint8_t *len) {
    int syms[LSYM], cnt = 0;
    memset(len, 0, (size_t)n);
    for (int i = 0; i < n; i++)
        if (freq[i]) syms[cnt++] = i;
    if (cnt == 0) return;
    if (cnt == 1) {
        len[syms[0]] = 1;
        return;
    }
    uint32_t f[LSYM];
    for (int k = 0; k < cnt; k++) f[k] = freq[syms[k]];
    for (;;) {
        uint64_t wt[2 * LSYM];
        int par[2 * LSYM];
        bool used[2 * LSYM] = {0};
        int nn = cnt;
        for (int k = 0; k < cnt; k++) wt[k] = f[k];
        for (int step = 0; step < cnt - 1; step++) {
            int a = -1, b = -1;
            for (int k = 0; k < nn; k++) {
                if (used[k]) continue;
                if (a < 0 || wt[k] < wt[a]) b = a, a = k;
                else if (b < 0 || wt[k] < wt[b]) b = k;
            }
            wt[nn] = wt[a] + wt[b];
            par[a] = par[b] = nn;
            used[a] = used[b] = true;
            nn++;
        }
        par[nn - 1] = -1;
        int maxd = 0;
        for (int k = 0; k < cnt; k++) {
            int d = 0;
            for (int j = k; par[j] >= 0; j = par[j]) d++;
            len[syms[k]] = (uint8_t)d;
            maxd = MAX(maxd, d);
        }
        if (maxd <= maxlen) return;
        for (int k = 0; k < cnt; k++) f[k] = (f[k] >> 1) | 1;
    }
}

static void huff_codes(hcode_t *h, int n) {
    int cnt[16] = {0}, nz = 0, last = 0, next[16];
    for (int i = 0; i < n; i++)
        if (h->len[i]) cnt[h->len[i]]++, nz++, last = i;
    h->single = nz <= 1 ? last : -1;
    int code = 0;
    cnt[0] = 0;
    for (int l = 1; l < 16; l++) code = (code + cnt[l - 1]) << 1, next[l] = code;
    for (int i = 0; i < n; i++) {
        int l = h->len[i];
        if (!l) continue;
        int c = next[l]++, r = 0;
        for (int b = 0; b < l; b++) r |= ((c >> b) & 1) << (l - 1 - b);
        h->code[i] = (uint16_t)r;
    }
}

static inline void put_sym(lbw_t *w, const hcode_t *h, int s) {
    if (h->single < 0) lw_bits(w, h->code[s], h->len[s]);
}

static const uint8_t cl_order[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

/* Build a prefix code for freq[0..n) and write its description (RFC 9649 3.7.2.1). */
static void put_code(lbw_t *w, hcode_t *h, const uint32_t *freq, int n) {
    memset(h, 0, sizeof *h);
    huff_lengths(freq, n, 15, h->len);
    huff_codes(h, n);
    int used = 0, s0 = -1, s1 = -1;
    for (int i = 0; i < n; i++)
        if (h->len[i]) {
            used++;
            if (s0 < 0) s0 = i;
            else s1 = i;
        }
    if (used == 0 || (used <= 2 && s0 < 256 && s1 < 256)) { /* simple code */
        if (used == 0) s0 = 0, h->single = 0;
        lw_bits(w, 1, 1);
        lw_bits(w, used == 2, 1);
        if (s0 < 2) lw_bits(w, 0, 1), lw_bits(w, (uint32_t)s0, 1);
        else lw_bits(w, 1, 1), lw_bits(w, (uint32_t)s0, 8);
        if (used == 2) lw_bits(w, (uint32_t)s1, 8);
        return;
    }
    /* normal code: run-length tokens over the code lengths */
    uint8_t tok[LSYM], xb[LSYM];
    int nt = 0;
    for (int i = 0; i < n;) {
        int v = h->len[i], run = 1;
        while (i + run < n && h->len[i + run] == v) run++;
        i += run;
        if (v == 0) {
            while (run >= 11) {
                int r = MIN(run, 138);
                tok[nt] = 18, xb[nt++] = (uint8_t)(r - 11), run -= r;
            }
            if (run >= 3) tok[nt] = 17, xb[nt++] = (uint8_t)(run - 3), run = 0;
        } else {
            tok[nt] = (uint8_t)v, xb[nt++] = 0, run--;
            while (run >= 3) {
                int r = MIN(run, 6);
                tok[nt] = 16, xb[nt++] = (uint8_t)(r - 3), run -= r;
            }
        }
        while (run-- > 0) tok[nt] = (uint8_t)v, xb[nt++] = 0;
    }
    uint32_t cf[19] = {0};
    for (int k = 0; k < nt; k++) cf[tok[k]]++;
    hcode_t cl;
    memset(&cl, 0, sizeof cl);
    huff_lengths(cf, 19, 7, cl.len);
    huff_codes(&cl, 19);
    int ncl = 19;
    while (ncl > 4 && !cl.len[cl_order[ncl - 1]]) ncl--;
    lw_bits(w, 0, 1);
    lw_bits(w, (uint32_t)(ncl - 4), 4);
    for (int k = 0; k < ncl; k++) lw_bits(w, cl.len[cl_order[k]], 3);
    lw_bits(w, 0, 1); /* code lengths for the whole alphabet */
    static const int xbits[3] = {2, 3, 7};
    for (int k = 0; k < nt; k++) {
        put_sym(w, &cl, tok[k]);
        if (tok[k] >= 16) lw_bits(w, xb[k], xbits[tok[k] - 16]);
    }
}

/* LZ77 prefix coding of a length / distance value (RFC 9649 3.6.2.2) */
static void prefix_of(uint32_t v, int *code, int *nextra, uint32_t *extra) {
    if (v <= 4) {
        *code = (int)v - 1, *nextra = 0, *extra = 0;
        return;
    }
    uint32_t x = v - 1;
    int hb = 31 - __builtin_clz(x);
    int second = (x >> (hb - 1)) & 1;
    *code = 2 * hb + second, *nextra = hb - 1, *extra = x & ((1u << (hb - 1)) - 1);
}

/* Encode the (filtered) alpha plane as a headerless VP8L image stream carrying it in green: literals and
 * LZ77 copies found through hash chains, distances coded with the 2-D neighbourhood codes when they apply. */
#define AHBITS 16
#define MAX_DVAL 1048576 /* largest distance value of the 40 prefix codes */

typedef struct {
    uint32_t a;    /* literal value, or copy length */
    uint32_t dval; /* 0: literal; else the coded distance value */
} litem_t;

static inline uint32_t ahash(const uint8_t *p) { return ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 0x9e3779b1u >> (32 - AHBITS); }

static void alpha_stream(const uint8_t *res, int w, int h, buf_t *out) {
    size_t np = (size_t)w * h, nitems = 0;
    litem_t *items = xmalloc(np * sizeof *items);
    int32_t *head = xmalloc(((size_t)1 << AHBITS) * sizeof *head), *prev = xmalloc(np * sizeof *prev);
    memset(head, 0xff, ((size_t)1 << AHBITS) * sizeof *head);
    /* linear distance -> neighbourhood code (1..120) */
    size_t nd = (size_t)w * 7 + 9;
    uint8_t *dcode = xcalloc(nd, 1);
    for (int k = 119; k >= 0; k--) {
        long d = vp8l_dist_map[k][0] + (long)vp8l_dist_map[k][1] * w;
        if (d >= 1 && (size_t)d < nd) dcode[d] = (uint8_t)(k + 1);
    }
#define DVAL(d) ((d) < nd && dcode[d] ? dcode[d] : (uint32_t)(d) + 120)
    for (size_t i = 0; i < np;) {
        size_t best = 0, bd = 0;
        size_t cand[2 + 32];
        int nc = 0;
        cand[nc++] = 1, cand[nc++] = (size_t)w;
        if (i + 2 < np)
            for (int32_t j = head[ahash(res + i)]; j >= 0 && nc < 2 + 32; j = prev[j]) {
                size_t d = i - (size_t)j;
                if (d + 120 > MAX_DVAL) break;
                cand[nc++] = d;
            }
        for (int k = 0; k < nc; k++) {
            size_t d = cand[k];
            if (d > i || d == 0) continue;
            size_t run = 0, lim = MIN(np - i, (size_t)4096);
            while (run < lim && res[i + run] == res[i + run - d]) run++;
            if (run > best || (run == best && run && DVAL(d) < DVAL(bd))) best = run, bd = d;
            if (best == lim) break;
        }
        /* far copies need to be longer to beat literals */
        size_t need = DVAL(bd) <= 120 || bd < 1024 ? 3 : 6;
        size_t adv = best >= need ? best : 1;
        if (best >= need) items[nitems++] = (litem_t){(uint32_t)best, (uint32_t)DVAL(bd)};
        else items[nitems++] = (litem_t){res[i], 0};
        for (size_t k = 0; k < adv; k++, i++)
            if (i + 2 < np) {
                uint32_t hh = ahash(res + i);
                prev[i] = head[hh], head[hh] = (int32_t)i;
            }
    }
#undef DVAL
    free(head), free(prev), free(dcode);
    uint32_t fg[LSYM] = {0}, fz[256] = {0}, fd[40] = {0};
    fz[0] = 1;
    for (size_t k = 0; k < nitems; k++) {
        if (!items[k].dval) {
            fg[items[k].a]++;
            continue;
        }
        int c, nx;
        uint32_t x;
        prefix_of(items[k].a, &c, &nx, &x);
        fg[256 + c]++;
        prefix_of(items[k].dval, &c, &nx, &x);
        fd[c]++;
    }
    lbw_t lw = {0};
    lw_bits(&lw, 0, 1); /* no transform */
    lw_bits(&lw, 0, 1); /* no color cache */
    lw_bits(&lw, 0, 1); /* single prefix code group */
    hcode_t hg, hz, hd;
    put_code(&lw, &hg, fg, LSYM);
    put_code(&lw, &hz, fz, 256); /* red, blue, alpha: always 0 */
    put_code(&lw, &hz, fz, 256);
    put_code(&lw, &hz, fz, 256);
    put_code(&lw, &hd, fd, 40);
    for (size_t k = 0; k < nitems; k++) {
        if (!items[k].dval) {
            put_sym(&lw, &hg, (int)items[k].a);
            continue;
        }
        int c, nx;
        uint32_t x;
        prefix_of(items[k].a, &c, &nx, &x);
        put_sym(&lw, &hg, 256 + c);
        if (nx) lw_bits(&lw, x, nx);
        prefix_of(items[k].dval, &c, &nx, &x);
        put_sym(&lw, &hd, c);
        if (nx) lw_bits(&lw, x, nx);
    }
    if (lw.n > 0) buf_byte(&lw.out, (uint8_t)lw.acc);
    free(items);
    buf_put(out, lw.out.d, lw.out.n);
    free(lw.out.d);
}

/* ALPH chunk payload: the smallest of the four prediction filters, losslessly coded. */
static uint8_t *alpha_encode(const img_t *im, size_t *len) {
    int w = im->w, h = im->h;
    size_t np = (size_t)w * h;
    uint8_t *a = xmalloc(np), *res = xmalloc(np);
    for (size_t i = 0; i < np; i++) a[i] = im->px[i * 4 + 3];
    buf_t best = {0};
    for (int f = 0; f < 4; f++) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                int pred;
                if (y == 0) pred = x ? a[i - 1] : 0;
                else if (x == 0) pred = a[i - w];
                else if (f == 0) pred = 0;
                else if (f == 1) pred = a[i - 1];
                else if (f == 2) pred = a[i - w];
                else {
                    int g = a[i - 1] + a[i - w] - a[i - w - 1];
                    pred = g < 0 ? 0 : g > 255 ? 255 : g;
                }
                if (f == 0) pred = 0;
                res[i] = (uint8_t)(a[i] - pred);
            }
        buf_t cur = {0};
        buf_byte(&cur, (uint8_t)(1 | f << 2)); /* lossless, filter f, no preprocessing */
        alpha_stream(res, w, h, &cur);
        if (!best.d || cur.n < best.n) {
            free(best.d);
            best = cur;
        } else {
            free(cur.d);
        }
    }
    free(a), free(res);
    *len = best.n;
    return best.d;
}
