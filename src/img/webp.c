/* webp.c - WebP decoder: RIFF container (simple lossy, simple lossless, extended VP8X with ICCP / EXIF /
 * XMP / unknown chunks skipped), the ALPH plane (raw or lossless, all filters), VP8 key frames (RFC 6386)
 * and, for animations, the first ANMF frame composited on a transparent canvas (as libwebp's
 * WebPAnimDecoder renders it). VP8L bitstreams are decoded by vp8l.c.
 * Lossy output follows libwebp: BT.601 YUV -> RGB with "fancy" (bilinear 9-3-3-1) chroma upsampling. */
#include "img/img.h"
#include "img/vp8.h"
#include "img/vp8_tables.h"
#include "core/core.h"

#define BPS VP8_BPS

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd24le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }

static int fail(char **err, const char *msg) {
    if (err && !*err) *err = xstrdup(msg);
    return -1;
}

/* ---------------------------------------------------------------- boolean decoder (RFC 6386 7) */
typedef struct {
    const uint8_t *p, *end;
    uint64_t value; /* bits left-aligned: the top 8 bits are the comparison window */
    uint32_t range; /* 128..255 after normalization */
    int count;      /* valid bits below the window */
    int zeros;      /* virtual zero bytes read past the end */
} bdec_t;

static void bd_fill(bdec_t *b) {
    for (int shift = 48 - b->count; shift >= 0; shift -= 8) {
        uint64_t byte = 0;
        if (b->p < b->end) byte = *b->p++;
        else b->zeros++;
        b->value |= byte << shift;
        b->count += 8;
    }
}

static void bd_init(bdec_t *b, const uint8_t *p, size_t n) {
    b->p = p, b->end = p + n, b->value = 0, b->range = 255, b->count = -8, b->zeros = 0;
    bd_fill(b);
}

static inline int bd_get(bdec_t *b, int prob) {
    if (b->count < 8) bd_fill(b);
    uint32_t split = 1 + (((b->range - 1) * (uint32_t)prob) >> 8);
    uint64_t big = (uint64_t)split << 56;
    int bit;
    if (b->value >= big) b->range -= split, b->value -= big, bit = 1;
    else b->range = split, bit = 0;
    int shift = __builtin_clz(b->range) - 24;
    b->range <<= shift, b->value <<= shift, b->count -= shift;
    return bit;
}

static int bd_lit(bdec_t *b, int n) {
    int v = 0;
    while (n-- > 0) v = v << 1 | bd_get(b, 128);
    return v;
}

static int bd_signed(bdec_t *b, int n) {
    int v = bd_lit(b, n);
    return bd_get(b, 128) ? -v : v;
}

static int bd_opt_signed(bdec_t *b, int n) { return bd_get(b, 128) ? bd_signed(b, n) : 0; }

/* true when the decoder consumed more than a byte of the zero padding past the end of its data */
static bool bd_overrun(const bdec_t *b) { return b->zeros * 8 - (8 + b->count) > 8; }

/* ---------------------------------------------------------------- VP8 frame */
typedef struct {
    uint8_t level, inner; /* loop filter level of the macroblock, filter inner edges */
} mbf_t;

typedef struct {
    int w, h, mbw, mbh;
    bool seg_on, seg_map, seg_abs;
    int seg_q[4], seg_lf[4];
    uint8_t seg_prob[3];
    bool simple, lf_delta;
    int level, sharp, ref_delta[4], mode_delta[4];
    int16_t dq[4][3][2]; /* [segment][y1, y2, uv][dc, ac] */
    uint8_t prob[4][8][3][11];
    bool use_skip;
    int skip_prob;
    int nparts;
    bdec_t br, parts[8];
    vp8_planes_t pl;
    mbf_t *mbf;
    uint8_t *top_modes; /* 4 per macroblock column */
    uint8_t *top_nz;    /* 9 per macroblock column: 4 y, 2 u, 2 v, y2 */
    uint8_t left_modes[4], left_nz[9];
} vp8dec_t;

static const int8_t bmode_tree[18] = {-B_DC_PRED, 2, -B_TM_PRED, 4, -B_VE_PRED, 6, 8, 12, -B_HE_PRED, 10, -B_RD_PRED, -B_VR_PRED, -B_LD_PRED, 14, -B_VL_PRED, 16, -B_HD_PRED, -B_HU_PRED};

static int read_bmode(bdec_t *b, const uint8_t *p) {
    int i = 0;
    while ((i = bmode_tree[i + bd_get(b, p[i >> 1])]) > 0) {}
    return -i;
}

/* Extra-bit probabilities and bases of DCT_CAT1..6 */
static const int cat_base[6] = {5, 7, 11, 19, 35, 67};

static int read_large(bdec_t *b, const uint8_t *p) {
    if (!bd_get(b, p[3])) {
        if (!bd_get(b, p[4])) return 2;
        return 3 + bd_get(b, p[5]);
    }
    int cat;
    if (!bd_get(b, p[6])) cat = bd_get(b, p[7]);
    else if (!bd_get(b, p[8])) cat = 2 + bd_get(b, p[9]);
    else cat = 4 + bd_get(b, p[10]);
    int v = 0;
    for (const uint8_t *q = vp8_pcat[cat]; *q; q++) v = v << 1 | bd_get(b, *q);
    return cat_base[cat] + v;
}

/* Decode the tokens of one block into out[] (dequantized, raster order). Returns the position after the
 * last decoded token: the block "has data" for the neighbour contexts when it is > first. */
static int read_coeffs(bdec_t *b, const uint8_t (*prob)[3][11], int ctx, const int16_t *dq, int n, int16_t *out) {
    const uint8_t *p = prob[vp8_bands[n]][ctx];
    if (!bd_get(b, p[0])) return n;
    for (;;) {
        while (!bd_get(b, p[1])) {
            if (++n == 16) return 16;
            p = prob[vp8_bands[n]][0];
        }
        int v, next;
        if (!bd_get(b, p[2])) v = 1, next = 1;
        else v = read_large(b, p), next = 2;
        if (bd_get(b, 128)) v = -v;
        out[vp8_zigzag[n]] = (int16_t)(v * dq[n > 0]);
        if (++n == 16) return 16;
        p = prob[vp8_bands[n]][next];
        if (!bd_get(b, p[0])) return n;
    }
}

static int parse_header(vp8dec_t *d, const uint8_t *data, size_t n, char **err) {
    if (n < 10) return fail(err, "VP8: truncated frame header");
    uint32_t bits = rd24le(data);
    if (bits & 1) return fail(err, "VP8: not a key frame (interframes are not supported)");
    if (((bits >> 1) & 7) > 3) return fail(err, "VP8: unknown profile");
    if (!((bits >> 4) & 1)) return fail(err, "VP8: frame not displayable");
    size_t part0 = bits >> 5;
    if (data[3] != 0x9d || data[4] != 0x01 || data[5] != 0x2a) return fail(err, "VP8: bad start code");
    d->w = (data[6] | data[7] << 8) & 0x3fff;
    d->h = (data[8] | data[9] << 8) & 0x3fff;
    if (!d->w || !d->h) return fail(err, "VP8: zero dimension");
    data += 10, n -= 10;
    if (part0 > n) return fail(err, "VP8: truncated first partition");
    bdec_t *b = &d->br;
    bd_init(b, data, part0);
    bd_get(b, 128); /* color space */
    bd_get(b, 128); /* clamping type: output is always clamped */
    d->seg_on = bd_get(b, 128);
    memset(d->seg_prob, 255, 3);
    if (d->seg_on) {
        d->seg_map = bd_get(b, 128);
        if (bd_get(b, 128)) { /* update segment feature data */
            d->seg_abs = bd_get(b, 128);
            for (int i = 0; i < 4; i++) d->seg_q[i] = bd_opt_signed(b, 7);
            for (int i = 0; i < 4; i++) d->seg_lf[i] = bd_opt_signed(b, 6);
        }
        if (d->seg_map)
            for (int i = 0; i < 3; i++) d->seg_prob[i] = bd_get(b, 128) ? (uint8_t)bd_lit(b, 8) : 255;
    }
    d->simple = bd_get(b, 128);
    d->level = bd_lit(b, 6);
    d->sharp = bd_lit(b, 3);
    d->lf_delta = bd_get(b, 128);
    if (d->lf_delta && bd_get(b, 128)) {
        for (int i = 0; i < 4; i++) d->ref_delta[i] = bd_opt_signed(b, 6);
        for (int i = 0; i < 4; i++) d->mode_delta[i] = bd_opt_signed(b, 6);
    }
    /* token partitions follow the first partition */
    d->nparts = 1 << bd_lit(b, 2);
    const uint8_t *sz = data + part0, *end = data + n;
    size_t left = n - part0;
    if (left < 3 * (size_t)(d->nparts - 1)) return fail(err, "VP8: truncated partition table");
    const uint8_t *ps = sz + 3 * (d->nparts - 1);
    left -= 3 * (d->nparts - 1);
    for (int i = 0; i < d->nparts - 1; i++, sz += 3) {
        size_t psz = MIN((size_t)rd24le(sz), left);
        bd_init(&d->parts[i], ps, psz);
        ps += psz, left -= psz;
    }
    if (ps >= end) return fail(err, "VP8: truncated token partition");
    bd_init(&d->parts[d->nparts - 1], ps, left);
    /* quantizer indices */
    int q = bd_lit(b, 7);
    int dy1dc = bd_opt_signed(b, 4), dy2dc = bd_opt_signed(b, 4), dy2ac = bd_opt_signed(b, 4);
    int duvdc = bd_opt_signed(b, 4), duvac = bd_opt_signed(b, 4);
#define QI(v) vp8_dc_q[(v) < 0 ? 0 : (v) > 127 ? 127 : (v)]
#define QA(v) vp8_ac_q[(v) < 0 ? 0 : (v) > 127 ? 127 : (v)]
    for (int s = 0; s < 4; s++) {
        int sq = q;
        if (d->seg_on) sq = d->seg_abs ? d->seg_q[s] : q + d->seg_q[s];
        sq = sq < 0 ? 0 : sq > 127 ? 127 : sq;
        int16_t(*m)[2] = d->dq[s];
        m[0][0] = QI(sq + dy1dc), m[0][1] = QA(sq);
        m[1][0] = (int16_t)(QI(sq + dy2dc) * 2), m[1][1] = (int16_t)MAX(QA(sq + dy2ac) * 155 / 100, 8);
        m[2][0] = (int16_t)MIN(QI(sq + duvdc), 132), m[2][1] = QA(sq + duvac);
    }
#undef QI
#undef QA
    bd_get(b, 128); /* refresh_entropy_probs: irrelevant for a single frame */
    memcpy(d->prob, vp8_default_coeff_prob, sizeof d->prob);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 3; k++)
                for (int l = 0; l < 11; l++)
                    if (bd_get(b, vp8_coeff_update_prob[i][j][k][l])) d->prob[i][j][k][l] = (uint8_t)bd_lit(b, 8);
    d->use_skip = bd_get(b, 128);
    d->skip_prob = d->use_skip ? bd_lit(b, 8) : 0;
    if (bd_overrun(b)) return fail(err, "VP8: truncated frame header");
    return 0;
}

typedef struct {
    uint8_t seg, skip, is_i4, ymode, uvmode, bmodes[16];
} mbhdr_t;

static void parse_modes(vp8dec_t *d, int mbx, mbhdr_t *m) {
    bdec_t *b = &d->br;
    m->seg = 0;
    if (d->seg_map) m->seg = !bd_get(b, d->seg_prob[0]) ? (uint8_t)bd_get(b, d->seg_prob[1]) : (uint8_t)(2 + bd_get(b, d->seg_prob[2]));
    m->skip = d->use_skip ? (uint8_t)bd_get(b, d->skip_prob) : 0;
    uint8_t *top = d->top_modes + mbx * 4, *left = d->left_modes;
    m->is_i4 = !bd_get(b, 145);
    if (!m->is_i4) {
        int ym = bd_get(b, 156) ? (bd_get(b, 128) ? VP8_TM_PRED : VP8_H_PRED) : (bd_get(b, 163) ? VP8_V_PRED : VP8_DC_PRED);
        static const uint8_t implied[4] = {B_DC_PRED, B_VE_PRED, B_HE_PRED, B_TM_PRED};
        m->ymode = (uint8_t)ym;
        memset(top, implied[ym], 4);
        memset(left, implied[ym], 4);
    } else {
        m->ymode = VP8_B_PRED;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int mode = read_bmode(b, vp8_kf_bmode_prob[top[x]][left[y]]);
                m->bmodes[y * 4 + x] = (uint8_t)(top[x] = left[y] = (uint8_t)mode);
            }
    }
    m->uvmode = !bd_get(b, 142) ? VP8_DC_PRED : !bd_get(b, 114) ? VP8_V_PRED : bd_get(b, 183) ? VP8_TM_PRED : VP8_H_PRED;
}

/* Tokens of one macroblock; returns a 24-bit mask of blocks needing an inverse transform (bit 24: any
 * block carried token data, which enables inner-edge loop filtering). */
static uint32_t parse_residuals(vp8dec_t *d, bdec_t *tb, int mbx, const mbhdr_t *m, int16_t *coef) {
    uint8_t *tnz = d->top_nz + mbx * 9, *lnz = d->left_nz;
    memset(coef, 0, 25 * 16 * sizeof *coef);
    if (m->skip) {
        memset(tnz, 0, 8), memset(lnz, 0, 8);
        if (!m->is_i4) tnz[8] = lnz[8] = 0;
        return 0;
    }
    const int16_t(*q)[2] = d->dq[m->seg];
    uint32_t mask = 0, data = 0;
    int first = 0, type = 3;
    if (!m->is_i4) {
        int16_t y2[16] = {0};
        int nz = read_coeffs(tb, d->prob[1], tnz[8] + lnz[8], q[1], 0, y2);
        tnz[8] = lnz[8] = nz > 0;
        data |= nz > 0;
        vp8_iwht(y2, coef);
        for (int i = 0; i < 16; i++)
            if (coef[i * 16]) mask |= 1u << i;
        first = 1, type = 0;
    }
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int i = y * 4 + x;
            int nz = read_coeffs(tb, d->prob[type], tnz[x] + lnz[y], q[0], first, coef + i * 16);
            tnz[x] = lnz[y] = nz > first;
            data |= nz > first;
            if (nz > first) mask |= 1u << i;
        }
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++) {
                int i = 16 + c * 4 + y * 2 + x;
                uint8_t *t = tnz + 4 + c * 2 + x, *l = lnz + 4 + c * 2 + y;
                int nz = read_coeffs(tb, d->prob[2], *t + *l, q[2], 0, coef + i * 16);
                *t = *l = nz > 0;
                data |= nz > 0;
                if (nz > 0) mask |= 1u << i;
            }
    return mask | (data ? 1u << 24 : 0);
}

static void reconstruct(vp8dec_t *d, vp8_mbbuf_t *wb, int mbx, int mby, const mbhdr_t *m, const int16_t *coef, uint32_t mask) {
    vp8_load_edges(wb, &d->pl, mbx, mby);
    uint8_t *y = VP8_YP(wb), *u = VP8_UP(wb), *v = VP8_VP(wb);
    if (m->is_i4) {
        for (int i = 0; i < 16; i++) {
            uint8_t *dst = y + (i >> 2) * 4 * BPS + (i & 3) * 4;
            vp8_pred4(dst, m->bmodes[i]);
            if (mask & (1u << i)) vp8_idct_add(coef + i * 16, dst, BPS);
        }
    } else {
        vp8_pred16(y, m->ymode, mby > 0, mbx > 0);
        for (int i = 0; i < 16; i++)
            if (mask & (1u << i)) vp8_idct_add(coef + i * 16, y + (i >> 2) * 4 * BPS + (i & 3) * 4, BPS);
    }
    vp8_pred8(u, m->uvmode, mby > 0, mbx > 0);
    vp8_pred8(v, m->uvmode, mby > 0, mbx > 0);
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1u << (16 + i)))) continue;
        uint8_t *base = i < 4 ? u : v;
        int k = i & 3;
        vp8_idct_add(coef + (16 + i) * 16, base + (k >> 1) * 4 * BPS + (k & 1) * 4, BPS);
    }
    vp8_store_mb(wb, &d->pl, mbx, mby);
}

/* ---------------------------------------------------------------- loop filter (RFC 6386 15) */
static inline int s8(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static inline uint8_t u8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static inline bool lf_simple_ok(const uint8_t *p, int s, int limit) {
    return 2 * abs(p[-s] - p[0]) + (abs(p[-2 * s] - p[s]) >> 1) <= limit;
}

static inline bool lf_normal_ok(const uint8_t *p, int s, int limit, int il) {
    int p3 = p[-4 * s], p2 = p[-3 * s], p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s], q2 = p[2 * s], q3 = p[3 * s];
    return lf_simple_ok(p, s, limit) && abs(p3 - p2) <= il && abs(p2 - p1) <= il && abs(p1 - p0) <= il && abs(q3 - q2) <= il &&
           abs(q2 - q1) <= il && abs(q1 - q0) <= il;
}

static inline bool lf_hev(const uint8_t *p, int s, int t) { return abs(p[-2 * s] - p[-s]) > t || abs(p[s] - p[0]) > t; }

static inline void lf_common(uint8_t *p, int s, bool outer) {
    int p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s];
    int a = 3 * (q0 - p0);
    if (outer) a += s8(p1 - q1);
    a = s8(a);
    int f1 = s8(a + 4) >> 3, f2 = s8(a + 3) >> 3;
    p[-s] = u8(p0 + f2), p[0] = u8(q0 - f1);
    if (!outer) {
        a = (f1 + 1) >> 1;
        p[-2 * s] = u8(p1 + a), p[s] = u8(q1 - a);
    }
}

static inline void lf_mbedge(uint8_t *p, int s) {
    int p2 = p[-3 * s], p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s], q2 = p[2 * s];
    int w = s8(s8(p1 - q1) + 3 * (q0 - p0));
    int a = (27 * w + 63) >> 7;
    p[-s] = u8(p0 + a), p[0] = u8(q0 - a);
    a = (18 * w + 63) >> 7;
    p[-2 * s] = u8(p1 + a), p[s] = u8(q1 - a);
    a = (9 * w + 63) >> 7;
    p[-3 * s] = u8(p2 + a), p[2 * s] = u8(q2 - a);
}

/* Filter n pixels of an edge: p points at the first q0 pixel, s crosses the edge, adv walks along it. */
static void lf_edge(uint8_t *p, int s, int adv, int n, int limit, int il, int hev_t, bool mb) {
    for (int i = 0; i < n; i++, p += adv) {
        if (!lf_normal_ok(p, s, limit, il)) continue;
        if (lf_hev(p, s, hev_t)) lf_common(p, s, true);
        else if (mb) lf_mbedge(p, s);
        else lf_common(p, s, false);
    }
}

static void lf_edge_simple(uint8_t *p, int s, int adv, int limit) {
    for (int i = 0; i < 16; i++, p += adv)
        if (lf_simple_ok(p, s, limit)) lf_common(p, s, true);
}

static void loop_filter(vp8dec_t *d) {
    vp8_planes_t *pl = &d->pl;
    int ys = pl->ys, uvs = pl->uvs;
    for (int mby = 0; mby < d->mbh; mby++)
        for (int mbx = 0; mbx < d->mbw; mbx++) {
            const mbf_t *f = &d->mbf[(size_t)mby * d->mbw + mbx];
            int level = f->level;
            if (!level) continue;
            int il = level;
            if (d->sharp > 0) {
                il >>= d->sharp > 4 ? 2 : 1;
                if (il > 9 - d->sharp) il = 9 - d->sharp;
            }
            if (il < 1) il = 1;
            int hev_t = level >= 40 ? 2 : level >= 15 ? 1 : 0;
            int mbl = 2 * (level + 2) + il, sbl = 2 * level + il;
            uint8_t *y = pl->y + (size_t)mby * 16 * ys + mbx * 16;
            uint8_t *u = pl->u + (size_t)mby * 8 * uvs + mbx * 8, *v = pl->v + (size_t)mby * 8 * uvs + mbx * 8;
            if (d->simple) {
                if (mbx > 0) lf_edge_simple(y, 1, ys, mbl);
                if (f->inner)
                    for (int k = 4; k < 16; k += 4) lf_edge_simple(y + k, 1, ys, sbl);
                if (mby > 0) lf_edge_simple(y, ys, 1, mbl);
                if (f->inner)
                    for (int k = 4; k < 16; k += 4) lf_edge_simple(y + k * ys, ys, 1, sbl);
                continue;
            }
            if (mbx > 0) {
                lf_edge(y, 1, ys, 16, mbl, il, hev_t, true);
                lf_edge(u, 1, uvs, 8, mbl, il, hev_t, true);
                lf_edge(v, 1, uvs, 8, mbl, il, hev_t, true);
            }
            if (f->inner) {
                for (int k = 4; k < 16; k += 4) lf_edge(y + k, 1, ys, 16, sbl, il, hev_t, false);
                lf_edge(u + 4, 1, uvs, 8, sbl, il, hev_t, false);
                lf_edge(v + 4, 1, uvs, 8, sbl, il, hev_t, false);
            }
            if (mby > 0) {
                lf_edge(y, ys, 1, 16, mbl, il, hev_t, true);
                lf_edge(u, uvs, 1, 8, mbl, il, hev_t, true);
                lf_edge(v, uvs, 1, 8, mbl, il, hev_t, true);
            }
            if (f->inner) {
                for (int k = 4; k < 16; k += 4) lf_edge(y + k * ys, ys, 1, 16, sbl, il, hev_t, false);
                lf_edge(u + 4 * uvs, uvs, 1, 8, sbl, il, hev_t, false);
                lf_edge(v + 4 * uvs, uvs, 1, 8, sbl, il, hev_t, false);
            }
        }
}

/* ---------------------------------------------------------------- YUV -> RGB (BT.601, libwebp scaling) */
/* 14-bit fixed point: 19077 = 1.164 (255/219), 26149 = 1.596, 6419 = 0.391, 13320 = 0.813, 33050 = 2.018;
 * results carry 6 fractional bits. */
static inline uint8_t clip6(int v) { return (v & ~16383) == 0 ? (uint8_t)(v >> 6) : v < 0 ? 0 : 255; }
static inline int mhi(int v, int c) { return (v * c) >> 8; }

static inline void yuv_px(int y, int u, int v, uint8_t *o) {
    int yy = mhi(y, 19077);
    o[0] = clip6(yy + mhi(v, 26149) - 14234);
    o[1] = clip6(yy - mhi(u, 6419) - mhi(v, 13320) + 8708);
    o[2] = clip6(yy + mhi(u, 33050) - 17685);
}

/* chroma at full resolution for one output row: 9-3-3-1 interpolation of the four nearest samples */
static void upsample_row(const uint8_t *near, const uint8_t *far, int cw, int w, uint8_t *out) {
    for (int x = 0; x < w; x++) {
        int c = x >> 1, f = (x & 1) ? MIN(c + 1, cw - 1) : MAX(c - 1, 0);
        if (x == 0 || ((x == w - 1) && !(w & 1))) {
            out[x] = (uint8_t)((3 * near[c] + far[c] + 2) >> 2);
            continue;
        }
        int n = near[c], h = near[f], vv = far[c], dg = far[f];
        out[x] = (uint8_t)((((n + 3 * h + 3 * vv + dg + 8) >> 3) + n) >> 1);
    }
}

static void yuv_to_rgba(const vp8_planes_t *pl, int w, int h, uint8_t *out) {
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint8_t *ur = xmalloc((size_t)w * 2);
    uint8_t *vr = ur + w;
    for (int y = 0; y < h; y++) {
        int c = y >> 1, f = (y & 1) ? MIN(c + 1, ch - 1) : MAX(c - 1, 0);
        if (y == 0) f = c;
        upsample_row(pl->u + (size_t)c * pl->uvs, pl->u + (size_t)f * pl->uvs, cw, w, ur);
        upsample_row(pl->v + (size_t)c * pl->uvs, pl->v + (size_t)f * pl->uvs, cw, w, vr);
        const uint8_t *yr = pl->y + (size_t)y * pl->ys;
        uint8_t *o = out + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, o += 4) yuv_px(yr[x], ur[x], vr[x], o), o[3] = 255;
    }
    free(ur);
}

/* Decode a VP8 key frame into out (allocated, opaque). want_w/want_h: canvas size to match (0: any). */
static int vp8_decode(const uint8_t *data, size_t n, int want_w, int want_h, img_t *out, char **err) {
    vp8dec_t *d = xcalloc(1, sizeof *d);
    int rc = -1;
    int16_t *coef = xmalloc(25 * 16 * sizeof *coef);
    vp8_mbbuf_t *wb = xmalloc(sizeof *wb);
    if (parse_header(d, data, n, err) < 0) goto done;
    if (want_w && (d->w != want_w || d->h != want_h)) {
        fail(err, "WebP: VP8 frame size does not match the canvas");
        goto done;
    }
    if (img_alloc(out, d->w, d->h) < 0) {
        fail(err, "WebP: image too large");
        goto done;
    }
    d->mbw = (d->w + 15) / 16, d->mbh = (d->h + 15) / 16;
    vp8_planes_t *pl = &d->pl;
    pl->mbw = d->mbw, pl->mbh = d->mbh;
    pl->ys = d->mbw * 16, pl->uvs = d->mbw * 8;
    pl->y = xmalloc((size_t)pl->ys * d->mbh * 16);
    pl->u = xmalloc((size_t)pl->uvs * d->mbh * 8);
    pl->v = xmalloc((size_t)pl->uvs * d->mbh * 8);
    d->mbf = xmalloc((size_t)d->mbw * d->mbh * sizeof *d->mbf);
    d->top_modes = xcalloc((size_t)d->mbw, 4);
    d->top_nz = xcalloc((size_t)d->mbw, 9);
    /* per-segment base filter levels (libwebp order: segment value, then mode deltas, clamped once) */
    int lvl[4][2];
    for (int s = 0; s < 4; s++) {
        int base = d->level;
        if (d->seg_on) base = d->seg_abs ? d->seg_lf[s] : d->level + d->seg_lf[s];
        for (int i4 = 0; i4 < 2; i4++) {
            int l = base;
            if (d->lf_delta) l += d->ref_delta[0] + (i4 ? d->mode_delta[0] : 0);
            lvl[s][i4] = l < 0 ? 0 : l > 63 ? 63 : l;
        }
    }
    for (int mby = 0; mby < d->mbh; mby++) {
        bdec_t *tb = &d->parts[mby & (d->nparts - 1)];
        memset(d->left_modes, 0, sizeof d->left_modes);
        memset(d->left_nz, 0, sizeof d->left_nz);
        for (int mbx = 0; mbx < d->mbw; mbx++) {
            mbhdr_t m;
            parse_modes(d, mbx, &m);
            uint32_t mask = parse_residuals(d, tb, mbx, &m, coef);
            reconstruct(d, wb, mbx, mby, &m, coef, mask);
            mbf_t *f = &d->mbf[(size_t)mby * d->mbw + mbx];
            f->level = (uint8_t)(d->level ? lvl[m.seg][m.is_i4] : 0);
            f->inner = m.is_i4 || (mask >> 24);
            if (bd_overrun(tb)) {
                fail(err, "VP8: premature end of token partition");
                goto done;
            }
        }
        if (bd_overrun(&d->br)) {
            fail(err, "VP8: premature end of first partition");
            goto done;
        }
    }
    if (d->level) loop_filter(d);
    yuv_to_rgba(pl, d->w, d->h, out->px);
    rc = 0;
done:
    if (rc < 0) img_free(out);
    free(d->pl.y), free(d->pl.u), free(d->pl.v);
    free(d->mbf), free(d->top_modes), free(d->top_nz);
    free(d), free(coef), free(wb);
    return rc;
}

/* ---------------------------------------------------------------- ALPH */
static int decode_alpha(const uint8_t *d, size_t n, img_t *im, char **err) {
    if (n < 1) return fail(err, "WebP: empty ALPH chunk");
    int method = d[0] & 3, filter = (d[0] >> 2) & 3, pre = (d[0] >> 4) & 3, rsv = d[0] >> 6;
    if (method > 1 || pre > 1 || rsv) return fail(err, "WebP: unsupported ALPH header");
    int w = im->w, h = im->h;
    size_t np = (size_t)w * h;
    uint8_t *a = xmalloc(np);
    if (method == 0) {
        if (n - 1 < np) return free(a), fail(err, "WebP: truncated ALPH data");
        memcpy(a, d + 1, np);
    } else {
        uint32_t *argb = NULL;
        const char *e = NULL;
        int lw = w, lh = h;
        if (vp8l_decode(d + 1, n - 1, true, &lw, &lh, &argb, &e) < 0) return free(a), fail(err, e ? e : "WebP: bad ALPH data");
        for (size_t i = 0; i < np; i++) a[i] = (uint8_t)(argb[i] >> 8);
        free(argb);
    }
    /* unfilter (RFC 9649 2.7.1.2): first pixel predicted by 0, first row by the left, first column by the top */
    if (filter)
        for (int y = 0; y < h; y++) {
            uint8_t *r = a + (size_t)y * w;
            const uint8_t *up = y ? r - w : NULL;
            for (int x = 0; x < w; x++) {
                int pred;
                if (!up) pred = x ? r[x - 1] : 0;
                else if (x == 0) pred = up[0];
                else if (filter == 1) pred = r[x - 1];
                else if (filter == 2) pred = up[x];
                else {
                    int g = r[x - 1] + up[x] - up[x - 1];
                    pred = g < 0 ? 0 : g > 255 ? 255 : g;
                }
                r[x] = (uint8_t)(r[x] + pred);
            }
        }
    for (size_t i = 0; i < np; i++) im->px[i * 4 + 3] = a[i];
    free(a);
    return 0;
}

/* ---------------------------------------------------------------- container */
static int lossless_decode(const uint8_t *d, size_t n, int want_w, int want_h, img_t *out, char **err) {
    int w = 0, h = 0;
    uint32_t *argb = NULL;
    const char *e = NULL;
    if (vp8l_decode(d, n, false, &w, &h, &argb, &e) < 0) return fail(err, e ? e : "WebP: bad VP8L data");
    if ((want_w && (w != want_w || h != want_h)) || img_alloc(out, w, h) < 0) {
        free(argb);
        return fail(err, want_w ? "WebP: VP8L size does not match the canvas" : "WebP: image too large");
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t c = argb[i];
        uint8_t *o = out->px + i * 4;
        o[0] = (uint8_t)(c >> 16), o[1] = (uint8_t)(c >> 8), o[2] = (uint8_t)c, o[3] = (uint8_t)(c >> 24);
    }
    free(argb);
    return 0;
}

typedef struct {
    const uint8_t *alph, *bits;
    size_t alph_n, bits_n;
    bool lossless;
} frame_t;

/* Find the optional ALPH and the VP8 / VP8L chunk in [p, end). Unknown chunks are skipped. */
static int find_frame(const uint8_t *p, const uint8_t *end, frame_t *f, char **err) {
    memset(f, 0, sizeof *f);
    while (end - p >= 8) {
        uint32_t sz = rd32le(p + 4);
        if (sz > (size_t)(end - p) - 8) return fail(err, "WebP: truncated chunk");
        const uint8_t *pay = p + 8;
        if (!memcmp(p, "ALPH", 4)) {
            if (!f->alph) f->alph = pay, f->alph_n = sz;
        } else if (!memcmp(p, "VP8 ", 4) || !memcmp(p, "VP8L", 4)) {
            f->bits = pay, f->bits_n = sz, f->lossless = p[3] == 'L';
            return 0;
        } else if (!memcmp(p, "ANMF", 4)) {
            return fail(err, "WebP: unexpected ANMF chunk");
        }
        size_t adv = 8 + (size_t)sz + (sz & 1);
        if (adv > (size_t)(end - p)) adv = (size_t)(end - p);
        p += adv;
    }
    return fail(err, "WebP: no image data");
}

static int decode_frame(const frame_t *f, int w, int h, img_t *out, char **err) {
    if (f->lossless) return lossless_decode(f->bits, f->bits_n, w, h, out, err);
    if (vp8_decode(f->bits, f->bits_n, w, h, out, err) < 0) return -1;
    if (f->alph && decode_alpha(f->alph, f->alph_n, out, err) < 0) {
        img_free(out);
        return -1;
    }
    return 0;
}

int img_webp_decode(const uint8_t *d, size_t n, img_t *out, char **err) {
    out->w = out->h = 0, out->px = NULL;
    if (err) *err = NULL;
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WEBP", 4)) return fail(err, "WebP: not a RIFF/WEBP file");
    uint32_t riff = rd32le(d + 4);
    if (riff < 12 || riff > n - 8) return fail(err, "WebP: truncated file");
    const uint8_t *p = d + 12, *end = d + 8 + riff;
    if (end - p < 8) return fail(err, "WebP: no chunks");
    uint32_t sz = rd32le(p + 4);
    if (sz > (size_t)(end - p) - 8) return fail(err, "WebP: truncated chunk");
    frame_t f;
    if (!memcmp(p, "VP8 ", 4) || !memcmp(p, "VP8L", 4)) {
        f = (frame_t){.bits = p + 8, .bits_n = sz, .lossless = p[3] == 'L'};
        return decode_frame(&f, 0, 0, out, err);
    }
    if (memcmp(p, "VP8X", 4)) return fail(err, "WebP: unknown first chunk");
    if (sz < 10) return fail(err, "WebP: bad VP8X chunk");
    uint8_t flags = p[8];
    int cw = (int)rd24le(p + 12) + 1, ch = (int)rd24le(p + 15) + 1;
    if (cw > IMG_MAX_DIM || ch > IMG_MAX_DIM || (uint64_t)cw * ch > IMG_MAX_PIXELS) return fail(err, "WebP: image too large");
    p += 8 + (size_t)sz + (sz & 1);
    if (p > end) p = end;
    if (!(flags & 0x02)) {
        if (find_frame(p, end, &f, err) < 0) return -1;
        return decode_frame(&f, cw, ch, out, err);
    }
    /* animation: render the first frame on a transparent canvas */
    while (end - p >= 8) {
        sz = rd32le(p + 4);
        if (sz > (size_t)(end - p) - 8) return fail(err, "WebP: truncated chunk");
        if (!memcmp(p, "ANMF", 4)) {
            if (sz < 16) return fail(err, "WebP: bad ANMF chunk");
            const uint8_t *a = p + 8;
            int fx = 2 * (int)rd24le(a), fy = 2 * (int)rd24le(a + 3);
            int fw = (int)rd24le(a + 6) + 1, fh = (int)rd24le(a + 9) + 1;
            bool blend = !(a[15] & 2);
            if ((int64_t)fx + fw > cw || (int64_t)fy + fh > ch) return fail(err, "WebP: animation frame outside the canvas");
            if (find_frame(a + 16, a + sz, &f, err) < 0) return -1;
            img_t fr = {0};
            if (decode_frame(&f, fw, fh, &fr, err) < 0) return -1;
            if (img_alloc(out, cw, ch) < 0) {
                img_free(&fr);
                return fail(err, "WebP: image too large");
            }
            for (int y = 0; y < fh; y++)
                for (int x = 0; x < fw; x++) {
                    const uint8_t *s = fr.px + ((size_t)y * fw + x) * 4;
                    uint8_t *o = out->px + ((size_t)(fy + y) * cw + fx + x) * 4;
                    if (!blend || s[3]) memcpy(o, s, 4);
                }
            img_free(&fr);
            return 0;
        }
        size_t adv = 8 + (size_t)sz + (sz & 1);
        if (adv > (size_t)(end - p)) adv = (size_t)(end - p);
        p += adv;
    }
    return fail(err, "WebP: animation without frames");
}
