/* jpeg.c - from-scratch JPEG codec (ITU T.81).
 *
 * Decoder: Huffman baseline / extended sequential (SOF0, SOF1) and progressive (SOF2), 8-bit precision,
 * 1 (gray), 3 (YCbCr or RGB) or 4 (CMYK / YCCK) components, sampling factors 1..4, restart intervals.
 * Output follows libjpeg defaults so results sit within a level or two of Pillow: "fancy" triangle
 * upsampling for 2:1 chroma ratios (replication otherwise), IJG fixed-point YCbCr constants, and
 * Adobe-inverted CMYK converted the way Pillow does. Arithmetic coding, 12-bit and lossless are rejected.
 * Every input read is bounds-checked; a stream that ends before EOI, a bad Huffman code or an
 * out-of-range coefficient index fails with -1.
 *
 * Encoder: baseline JFIF, YCbCr 4:2:0, IJG-scaled Annex K quantization tables, Annex K Huffman tables.
 */
#include "img/img.h"
#include "core/core.h"
#include <math.h>
#include <string.h>

/* zigzag index -> natural (row-major) index */
static const uint8_t zz[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* c[u][x] = C(u)/2 * cos((2x+1)u*pi/16): the orthonormal 8-point DCT basis */
static void dct_basis(float c[8][8]) {
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++)
            c[u][x] = (float)((u ? 0.5 : sqrt(0.125)) * cos((2 * x + 1) * u * M_PI / 16.0));
}

static inline int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* ======================================================================== decoder */

#define J_MAX_SCANS 1000
#define J_WORK_PER_BLOCK 256 /* block visits allowed per coded block, summed over all scans */

typedef struct {
    uint16_t fast[512]; /* 9-bit lookahead: (len << 8) | symbol; 0 = longer code */
    int32_t mincode[17], maxcode[17], valoff[17];
    uint8_t vals[256];
    bool ok;
} jhuff_t;

/* Entropy-coded segment reader. Bytes past a marker or the end of data read as zero ("pad"); consuming
 * a pad bit sets overrun. */
typedef struct {
    const uint8_t *d;
    size_t n, pos;
    uint64_t buf; /* valid bits left-aligned */
    int nbits, pad;
    bool marker, eod, overrun;
} jbits_t;

typedef struct {
    int id, h, v, tq;
    int bw, bh;   /* allocated blocks (MCU-padded) */
    int nbw, nbh; /* blocks covering the component's own samples */
    int cw, ch;   /* component sample dimensions */
    uint8_t *plane; /* bw*8 x bh*8 samples */
    int16_t *coef;  /* progressive: bw*bh blocks of 64 coefficients (natural order) */
    uint16_t q[64]; /* natural order, latched at the component's first scan */
    bool qset;
    int pred, td, ta;
} jcomp_t;

typedef struct {
    const uint8_t *d;
    size_t n;
    const char *msg;
    uint16_t qt[4][64];
    bool qdef[4];
    jhuff_t hdc[4], hac[4];
    int w, h, nc, hmax, vmax, mcux, mcuy;
    bool frame, prog, jfif, adobe;
    int adobe_xf;
    jcomp_t c[4];
    int ri, nscans;
    jbits_t br;
    int eobrun;
    int ns, ss, se, ah, al;
    jcomp_t *sc[4];
    uint64_t work, work_cap;
    float cosk[8][8];
    img_t out;
} jdec_t;

#define JFAIL(j, m) return ((j)->msg = (m), -1)

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* ---- bit reader ---- */

static void br_init(jbits_t *b, const uint8_t *d, size_t n, size_t pos) {
    memset(b, 0, sizeof *b);
    b->d = d, b->n = n, b->pos = pos;
}

static void br_fill(jbits_t *b) {
    while (b->nbits <= 56) {
        unsigned v = 0;
        if (b->marker || b->eod) {
            b->pad += 8;
        } else if (b->pos >= b->n) {
            b->eod = true;
            b->pad += 8;
        } else if (b->d[b->pos] != 0xff) {
            v = b->d[b->pos++];
        } else {
            size_t q = b->pos + 1;
            while (q < b->n && b->d[q] == 0xff) q++; /* fill bytes */
            if (q >= b->n) {
                b->eod = true;
                b->pad += 8;
            } else if (b->d[q] == 0) { /* stuffed 0xFF */
                v = 0xff;
                b->pos = q + 1;
            } else { /* marker: stop here, pos stays on the 0xFF */
                b->marker = true;
                b->pad += 8;
            }
        }
        b->buf |= (uint64_t)v << (56 - b->nbits);
        b->nbits += 8;
    }
}

static inline void br_skip(jbits_t *b, int k) {
    b->buf <<= k;
    b->nbits -= k;
    if (b->nbits < b->pad) {
        b->overrun = true;
        b->pad = b->nbits;
    }
}

/* k in 1..16 */
static inline unsigned br_get(jbits_t *b, int k) {
    if (b->nbits < k) br_fill(b);
    unsigned v = (unsigned)(b->buf >> (64 - k));
    br_skip(b, k);
    return v;
}

static inline int br_extend(jbits_t *b, int s) {
    if (!s) return 0;
    int v = (int)br_get(b, s);
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

/* index of the marker code byte at or after p (skipping stuffed bytes), or n */
static size_t find_marker(const uint8_t *d, size_t n, size_t p) {
    while (p + 1 < n) {
        if (d[p] != 0xff) {
            p++;
            continue;
        }
        size_t q = p + 1;
        while (q < n && d[q] == 0xff) q++;
        if (q >= n) return n;
        if (d[q] != 0) return q;
        p = q + 1;
    }
    return n;
}

/* ---- Huffman ---- */

static int huff_build(jhuff_t *h, const uint8_t *counts, const uint8_t *vals, int total) {
    memset(h, 0, sizeof *h);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        int cnt = counts[l - 1];
        if (code + cnt > (1 << l)) return -1; /* over-subscribed */
        h->mincode[l] = code;
        h->valoff[l] = k;
        h->maxcode[l] = cnt ? code + cnt - 1 : -1;
        if (l <= 9)
            for (int i = 0; i < cnt; i++) {
                int base = (code + i) << (9 - l), span = 1 << (9 - l);
                for (int s = 0; s < span; s++) h->fast[base + s] = (uint16_t)(l << 8 | vals[k + i]);
            }
        code = (code + cnt) << 1;
        k += cnt;
    }
    if (k != total) return -1;
    memcpy(h->vals, vals, (size_t)total);
    h->ok = true;
    return 0;
}

static int huff_decode(jbits_t *b, const jhuff_t *h) {
    if (b->nbits < 16) br_fill(b);
    unsigned e = h->fast[b->buf >> 55];
    if (e) {
        br_skip(b, (int)(e >> 8));
        return (int)(e & 0xff);
    }
    int32_t code = (int32_t)(b->buf >> 48);
    for (int l = 10; l <= 16; l++) {
        int32_t c = code >> (16 - l);
        if (c <= h->maxcode[l] && c >= h->mincode[l]) {
            int32_t idx = h->valoff[l] + c - h->mincode[l];
            if (idx < 0 || idx > 255) return -1;
            br_skip(b, l);
            return h->vals[idx];
        }
    }
    return -1;
}

/* ---- IDCT: separable float transform on dequantized natural-order coefficients ---- */

static void idct_block(const float c[8][8], const int32_t *in, uint8_t *out, size_t stride) {
    float t[64];
    for (int u = 0; u < 8; u++) {
        bool ac = false;
        for (int v = 1; v < 8; v++)
            if (in[v * 8 + u]) {
                ac = true;
                break;
            }
        if (!ac) {
            float dc = (float)in[u] * c[0][0];
            for (int y = 0; y < 8; y++) t[y * 8 + u] = dc;
            continue;
        }
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += c[v][y] * (float)in[v * 8 + u];
            t[y * 8 + u] = s;
        }
    }
    for (int y = 0; y < 8; y++) {
        const float *r = t + y * 8;
        uint8_t *o = out + (size_t)y * stride;
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += c[u][x] * r[u];
            s += 128.5f;
            o[x] = s <= 0 ? 0 : s >= 255 ? 255 : (uint8_t)s;
        }
    }
}

/* ---- marker segments ---- */

static int parse_dqt(jdec_t *j, const uint8_t *p, size_t n) {
    while (n > 0) {
        int pq = p[0] >> 4, tq = p[0] & 15;
        if (pq > 1 || tq > 3) JFAIL(j, "bad DQT");
        size_t need = 1 + 64 * (size_t)(pq + 1);
        if (n < need) JFAIL(j, "short DQT");
        for (int k = 0; k < 64; k++) j->qt[tq][zz[k]] = pq ? be16(p + 1 + 2 * k) : p[1 + k];
        j->qdef[tq] = true;
        p += need, n -= need;
    }
    return 0;
}

static int parse_dht(jdec_t *j, const uint8_t *p, size_t n) {
    while (n > 0) {
        if (n < 17) JFAIL(j, "short DHT");
        int tc = p[0] >> 4, th = p[0] & 15;
        if (tc > 1 || th > 3) JFAIL(j, "bad DHT");
        int total = 0;
        for (int i = 0; i < 16; i++) total += p[1 + i];
        if (total > 256 || n < 17 + (size_t)total) JFAIL(j, "bad DHT");
        if (huff_build(tc ? &j->hac[th] : &j->hdc[th], p + 1, p + 17, total) < 0) JFAIL(j, "bad Huffman table");
        p += 17 + total, n -= 17 + (size_t)total;
    }
    return 0;
}

static int parse_sof(jdec_t *j, int marker, const uint8_t *p, size_t n) {
    if (j->frame) JFAIL(j, "multiple frames");
    if (n < 6) JFAIL(j, "short SOF");
    if (p[0] != 8) JFAIL(j, "unsupported JPEG precision (only 8-bit)");
    j->h = be16(p + 1);
    j->w = be16(p + 3);
    j->nc = p[5];
    if (j->h == 0) JFAIL(j, "unsupported JPEG (DNL height)");
    if (j->nc != 1 && j->nc != 3 && j->nc != 4) JFAIL(j, "unsupported JPEG component count");
    if (n < 6 + 3 * (size_t)j->nc) JFAIL(j, "short SOF");
    j->prog = marker == 0xc2;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->nc; i++) {
        jcomp_t *c = &j->c[i];
        const uint8_t *q = p + 6 + 3 * i;
        c->id = q[0], c->h = q[1] >> 4, c->v = q[1] & 15, c->tq = q[2];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) JFAIL(j, "bad SOF component");
        for (int k = 0; k < i; k++)
            if (j->c[k].id == c->id) JFAIL(j, "duplicate component id");
        j->hmax = MAX(j->hmax, c->h);
        j->vmax = MAX(j->vmax, c->v);
    }
    if (img_alloc(&j->out, j->w, j->h) < 0) JFAIL(j, "JPEG dimensions out of range");
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    uint64_t blocks = 0;
    for (int i = 0; i < j->nc; i++) {
        jcomp_t *c = &j->c[i];
        c->cw = (int)(((int64_t)j->w * c->h + j->hmax - 1) / j->hmax);
        c->ch = (int)(((int64_t)j->h * c->v + j->vmax - 1) / j->vmax);
        c->nbw = (c->cw + 7) / 8;
        c->nbh = (c->ch + 7) / 8;
        c->bw = j->mcux * c->h;
        c->bh = j->mcuy * c->v;
        size_t nb = (size_t)c->bw * c->bh;
        blocks += nb;
        if (j->prog) c->coef = xcalloc(nb * 64, sizeof(int16_t));
        else c->plane = xcalloc(nb, 64);
    }
    j->work_cap = blocks * J_WORK_PER_BLOCK;
    j->frame = true;
    return 0;
}

/* ---- scans ---- */

static int blk_seq(jdec_t *j, jcomp_t *c, int bx, int by) {
    int32_t blk[64];
    memset(blk, 0, sizeof blk);
    jbits_t *b = &j->br;
    int s = huff_decode(b, &j->hdc[c->td]);
    if (s < 0 || s > 15) JFAIL(j, "corrupt JPEG data (bad Huffman code)");
    c->pred = clamp16(c->pred + br_extend(b, s));
    blk[0] = c->pred * c->q[0];
    const jhuff_t *ac = &j->hac[c->ta];
    for (int k = 1; k < 64; k++) {
        int rs = huff_decode(b, ac);
        if (rs < 0) JFAIL(j, "corrupt JPEG data (bad Huffman code)");
        int r = rs >> 4;
        s = rs & 15;
        if (!s) {
            if (r != 15) break;
            k += 15;
            continue;
        }
        k += r;
        if (k > 63) JFAIL(j, "corrupt JPEG data (coefficient index)");
        blk[zz[k]] = br_extend(b, s) * c->q[zz[k]];
    }
    if (bx < c->nbw && by < c->nbh) {
        size_t stride = (size_t)c->bw * 8;
        idct_block(j->cosk, blk, c->plane + (size_t)by * 8 * stride + (size_t)bx * 8, stride);
    }
    return 0;
}

static inline int16_t *coef_at(jcomp_t *c, int bx, int by) { return c->coef + ((size_t)by * c->bw + bx) * 64; }

static int blk_dc_first(jdec_t *j, jcomp_t *c, int bx, int by) {
    int s = huff_decode(&j->br, &j->hdc[c->td]);
    if (s < 0 || s > 15) JFAIL(j, "corrupt JPEG data (bad Huffman code)");
    c->pred = clamp16(c->pred + br_extend(&j->br, s));
    coef_at(c, bx, by)[0] = clamp16(c->pred * (1 << j->al));
    return 0;
}

static int blk_dc_refine(jdec_t *j, jcomp_t *c, int bx, int by) {
    if (br_get(&j->br, 1)) {
        int16_t *p = coef_at(c, bx, by);
        p[0] = (int16_t)(p[0] | (1 << j->al));
    }
    return 0;
}

static int blk_ac_first(jdec_t *j, jcomp_t *c, int bx, int by) {
    if (j->eobrun > 0) {
        j->eobrun--;
        return 0;
    }
    int16_t *blk = coef_at(c, bx, by);
    const jhuff_t *ac = &j->hac[c->ta];
    for (int k = j->ss; k <= j->se; k++) {
        int rs = huff_decode(&j->br, ac);
        if (rs < 0) JFAIL(j, "corrupt JPEG data (bad Huffman code)");
        int r = rs >> 4, s = rs & 15;
        if (s) {
            k += r;
            if (k > j->se) JFAIL(j, "corrupt JPEG data (coefficient index)");
            blk[zz[k]] = clamp16(br_extend(&j->br, s) * (1 << j->al));
        } else if (r == 15) {
            k += 15;
        } else {
            j->eobrun = (1 << r) - 1;
            if (r) j->eobrun += (int)br_get(&j->br, r);
            break;
        }
    }
    return 0;
}

static inline void refine_bit(jbits_t *b, int16_t *cf, int p1) {
    if (br_get(b, 1) && (*cf & p1) == 0) *cf = clamp16(*cf + (*cf >= 0 ? p1 : -p1));
}

static int blk_ac_refine(jdec_t *j, jcomp_t *c, int bx, int by) {
    int16_t *blk = coef_at(c, bx, by);
    jbits_t *b = &j->br;
    const jhuff_t *ac = &j->hac[c->ta];
    int p1 = 1 << j->al, k = j->ss;
    if (j->eobrun == 0) {
        for (; k <= j->se; k++) {
            int rs = huff_decode(b, ac);
            if (rs < 0) JFAIL(j, "corrupt JPEG data (bad Huffman code)");
            int r = rs >> 4, s = rs & 15, val = 0;
            if (s) {
                if (s != 1) JFAIL(j, "corrupt JPEG data (refinement)");
                val = br_get(b, 1) ? p1 : -p1;
            } else if (r != 15) {
                j->eobrun = 1 << r;
                if (r) j->eobrun += (int)br_get(b, r);
                break; /* the rest of this block is refined below */
            }
            /* skip r zero-history coefficients, refining nonzero ones on the way */
            while (k <= j->se) {
                int16_t *cf = &blk[zz[k]];
                if (*cf) refine_bit(b, cf, p1);
                else if (--r < 0) break;
                k++;
            }
            if (val) {
                if (k > j->se) JFAIL(j, "corrupt JPEG data (coefficient index)");
                blk[zz[k]] = (int16_t)val;
            }
        }
    }
    if (j->eobrun > 0) {
        for (; k <= j->se; k++)
            if (blk[zz[k]]) refine_bit(b, &blk[zz[k]], p1);
        j->eobrun--;
    }
    return 0;
}

typedef int (*blk_fn)(jdec_t *j, jcomp_t *c, int bx, int by);

/* Consume an expected RSTn marker and reset the entropy decoder state. */
static int do_restart(jdec_t *j) {
    size_t q = find_marker(j->d, j->n, j->br.pos);
    if (q >= j->n) JFAIL(j, "truncated JPEG data");
    if (j->d[q] < 0xd0 || j->d[q] > 0xd7) JFAIL(j, "missing JPEG restart marker");
    br_init(&j->br, j->d, j->n, q + 1);
    for (int i = 0; i < j->ns; i++) j->sc[i]->pred = 0;
    j->eobrun = 0;
    return 0;
}

static int run_scan(jdec_t *j, blk_fn fn) {
    int mx_n, my_n, per_mcu = 0;
    if (j->ns == 1) {
        mx_n = j->sc[0]->nbw, my_n = j->sc[0]->nbh, per_mcu = 1;
    } else {
        mx_n = j->mcux, my_n = j->mcuy;
        for (int i = 0; i < j->ns; i++) per_mcu += j->sc[i]->h * j->sc[i]->v;
    }
    int left = j->ri;
    for (int my = 0; my < my_n; my++)
        for (int mx = 0; mx < mx_n; mx++) {
            if (j->ri) {
                if (left == 0) {
                    if (do_restart(j) < 0) return -1;
                    left = j->ri;
                }
                left--;
            }
            if (j->ns == 1) {
                if (fn(j, j->sc[0], mx, my) < 0) return -1;
            } else {
                for (int i = 0; i < j->ns; i++) {
                    jcomp_t *c = j->sc[i];
                    for (int by = 0; by < c->v; by++)
                        for (int bx = 0; bx < c->h; bx++)
                            if (fn(j, c, mx * c->h + bx, my * c->v + by) < 0) return -1;
                }
            }
            if (j->br.overrun && j->br.eod) JFAIL(j, "truncated JPEG data");
            j->work += (uint64_t)per_mcu;
            if (j->work > j->work_cap) JFAIL(j, "JPEG scan work limit exceeded");
        }
    return 0;
}

/* SOS header at p[0..n); entropy data starts at *pos. On return *pos is past the scan data. */
static int parse_sos(jdec_t *j, const uint8_t *p, size_t n, size_t *pos) {
    if (!j->frame) JFAIL(j, "SOS before SOF");
    if (++j->nscans > J_MAX_SCANS) JFAIL(j, "too many JPEG scans");
    if (n < 1) JFAIL(j, "short SOS");
    int ns = p[0];
    if (ns < 1 || ns > j->nc || n < 4 + 2 * (size_t)ns) JFAIL(j, "bad SOS");
    int blocks = 0;
    for (int i = 0; i < ns; i++) {
        int id = p[1 + 2 * i], t = p[2 + 2 * i];
        jcomp_t *c = NULL;
        for (int k = 0; k < j->nc; k++)
            if (j->c[k].id == id) c = &j->c[k];
        if (!c) JFAIL(j, "SOS references unknown component");
        for (int k = 0; k < i; k++)
            if (j->sc[k] == c) JFAIL(j, "duplicate SOS component");
        c->td = t >> 4, c->ta = t & 15;
        if (c->td > 3 || c->ta > 3) JFAIL(j, "bad SOS table selector");
        if (!c->qset) {
            if (!j->qdef[c->tq]) JFAIL(j, "missing quantization table");
            memcpy(c->q, j->qt[c->tq], sizeof c->q);
            c->qset = true;
        }
        c->pred = 0;
        j->sc[i] = c;
        blocks += c->h * c->v;
    }
    if (ns > 1 && blocks > 10) JFAIL(j, "bad SOS (MCU too large)");
    const uint8_t *q = p + 1 + 2 * ns;
    j->ns = ns, j->ss = q[0], j->se = q[1], j->ah = q[2] >> 4, j->al = q[2] & 15;
    j->eobrun = 0;
    blk_fn fn;
    if (!j->prog) {
        for (int i = 0; i < ns; i++)
            if (!j->hdc[j->sc[i]->td].ok || !j->hac[j->sc[i]->ta].ok) JFAIL(j, "missing Huffman table");
        fn = blk_seq;
    } else {
        if (j->al > 13 || j->ah > 13) JFAIL(j, "bad progressive scan parameters");
        if (j->ss == 0) {
            if (j->se != 0) JFAIL(j, "bad progressive scan parameters");
            if (!j->ah)
                for (int i = 0; i < ns; i++)
                    if (!j->hdc[j->sc[i]->td].ok) JFAIL(j, "missing Huffman table");
            fn = j->ah ? blk_dc_refine : blk_dc_first;
        } else {
            if (ns != 1 || j->se < j->ss || j->se > 63) JFAIL(j, "bad progressive scan parameters");
            if (!j->hac[j->sc[0]->ta].ok) JFAIL(j, "missing Huffman table");
            fn = j->ah ? blk_ac_refine : blk_ac_first;
        }
    }
    br_init(&j->br, j->d, j->n, *pos);
    if (run_scan(j, fn) < 0) return -1;
    size_t m = find_marker(j->d, j->n, j->br.pos);
    *pos = m >= j->n ? j->n : m - 1;
    return 0;
}

/* ---- output: upsampling and color conversion ---- */

static void upsample_row(const jdec_t *j, const jcomp_t *c, int y, uint8_t *out) {
    const size_t st = (size_t)c->bw * 8;
    const int w = j->w, cw = c->cw, ch = c->ch;
    if (c->h == j->hmax && c->v == j->vmax) {
        memcpy(out, c->plane + (size_t)y * st, (size_t)w);
    } else if (2 * c->h == j->hmax && c->v == j->vmax && cw > 2) { /* h2v1 fancy */
        const uint8_t *r = c->plane + (size_t)y * st;
        for (int x = 0; x < w; x++) {
            int i = x >> 1;
            if (!(x & 1)) out[x] = i == 0 ? r[0] : (uint8_t)((3 * r[i] + r[i - 1] + 1) >> 2);
            else out[x] = i == cw - 1 ? r[i] : (uint8_t)((3 * r[i] + r[i + 1] + 2) >> 2);
        }
    } else if (2 * c->h == j->hmax && 2 * c->v == j->vmax && cw > 2) { /* h2v2 fancy */
        int cy = y >> 1, fy = (y & 1) ? MIN(cy + 1, ch - 1) : MAX(cy - 1, 0);
        const uint8_t *nr = c->plane + (size_t)cy * st, *fr = c->plane + (size_t)fy * st;
#define CS(i) (3 * nr[i] + fr[i])
        for (int x = 0; x < w; x++) {
            int i = x >> 1;
            if (!(x & 1)) out[x] = (uint8_t)(i == 0 ? (CS(0) * 4 + 8) >> 4 : (3 * CS(i) + CS(i - 1) + 8) >> 4);
            else out[x] = (uint8_t)(i == cw - 1 ? (CS(i) * 4 + 7) >> 4 : (3 * CS(i) + CS(i + 1) + 7) >> 4);
        }
#undef CS
    } else if (c->h == j->hmax && 2 * c->v == j->vmax) { /* h1v2 fancy */
        int cy = y >> 1, fy = (y & 1) ? MIN(cy + 1, ch - 1) : MAX(cy - 1, 0), bias = (y & 1) ? 2 : 1;
        const uint8_t *nr = c->plane + (size_t)cy * st, *fr = c->plane + (size_t)fy * st;
        for (int x = 0; x < w; x++) out[x] = (uint8_t)((3 * nr[x] + fr[x] + bias) >> 2);
    } else { /* replicate */
        int sy = MIN((int)((int64_t)y * c->v / j->vmax), ch - 1);
        const uint8_t *r = c->plane + (size_t)sy * st;
        for (int x = 0; x < w; x++) out[x] = r[MIN((int)((int64_t)x * c->h / j->hmax), cw - 1)];
    }
}

/* floor((v + 32768) / 65536) without relying on signed right shift */
static inline int fix_round(int v) { return ((v + 32768 + (512 << 16)) >> 16) - 512; }

static inline void ycc_rgb(int y, int cb, int cr, uint8_t *o) {
    cb -= 128, cr -= 128;
    o[0] = clamp8(y + fix_round(91881 * cr));
    o[1] = clamp8(y + fix_round(-22554 * cb - 46802 * cr));
    o[2] = clamp8(y + fix_round(116130 * cb));
}

/* Pillow's CMYK -> RGB on Adobe-inverted samples s (true ink = 255 - s) */
static inline uint8_t cmyk_ch(int s, int sk) {
    int t = (255 - s) * sk + 128;
    return clamp8(sk - ((t + (t >> 8)) >> 8));
}

static void emit_rgba(jdec_t *j) {
    int w = j->w;
    uint8_t *rows = xmalloc((size_t)w * j->nc);
    enum { GRAY, YCC, RGB, CMYK, YCCK } cs;
    if (j->nc == 1) cs = GRAY;
    else if (j->nc == 3) {
        bool rgb_ids = j->c[0].id == 'R' && j->c[1].id == 'G' && j->c[2].id == 'B';
        cs = j->jfif ? YCC : j->adobe ? (j->adobe_xf == 0 ? RGB : YCC) : rgb_ids ? RGB : YCC;
    } else cs = j->adobe && j->adobe_xf == 2 ? YCCK : CMYK;
    for (int y = 0; y < j->h; y++) {
        for (int i = 0; i < j->nc; i++) upsample_row(j, &j->c[i], y, rows + (size_t)i * w);
        const uint8_t *r0 = rows, *r1 = rows + w, *r2 = rows + 2 * (size_t)w, *r3 = rows + 3 * (size_t)w;
        uint8_t *o = j->out.px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, o += 4) {
            uint8_t rgb[3];
            switch (cs) {
            case GRAY: rgb[0] = rgb[1] = rgb[2] = r0[x]; break;
            case YCC: ycc_rgb(r0[x], r1[x], r2[x], rgb); break;
            case RGB: rgb[0] = r0[x], rgb[1] = r1[x], rgb[2] = r2[x]; break;
            case CMYK:
                for (int k = 0; k < 3; k++) rgb[k] = cmyk_ch(rows[(size_t)k * w + x], r3[x]);
                break;
            case YCCK: { /* libjpeg emits 255 - YCC->RGB as the (inverted) CMY samples */
                uint8_t t[3];
                ycc_rgb(r0[x], r1[x], r2[x], t);
                for (int k = 0; k < 3; k++) rgb[k] = cmyk_ch(255 - t[k], r3[x]);
                break;
            }
            }
            o[0] = rgb[0], o[1] = rgb[1], o[2] = rgb[2], o[3] = 255;
        }
    }
    free(rows);
}

/* Progressive: dequantize + IDCT the accumulated coefficients into sample planes. */
static void prog_finish(jdec_t *j) {
    for (int i = 0; i < j->nc; i++) {
        jcomp_t *c = &j->c[i];
        if (!c->qset && j->qdef[c->tq]) memcpy(c->q, j->qt[c->tq], sizeof c->q);
        size_t stride = (size_t)c->bw * 8;
        c->plane = xcalloc((size_t)c->bw * c->bh, 64);
        int32_t blk[64];
        for (int by = 0; by < c->nbh; by++)
            for (int bx = 0; bx < c->nbw; bx++) {
                const int16_t *s = coef_at(c, bx, by);
                for (int k = 0; k < 64; k++) blk[k] = s[k] * c->q[k];
                idct_block(j->cosk, blk, c->plane + (size_t)by * 8 * stride + (size_t)bx * 8, stride);
            }
    }
}

static int decode_markers(jdec_t *j) {
    const uint8_t *d = j->d;
    size_t n = j->n;
    if (n < 4 || d[0] != 0xff || d[1] != 0xd8) JFAIL(j, "not a JPEG file");
    size_t pos = 2;
    for (;;) {
        if (pos >= n) JFAIL(j, "truncated JPEG (missing EOI)");
        if (d[pos] != 0xff) { /* stray bytes between segments */
            pos++;
            continue;
        }
        while (pos < n && d[pos] == 0xff) pos++;
        if (pos >= n) JFAIL(j, "truncated JPEG (missing EOI)");
        int m = d[pos++];
        if (m == 0x00 || m == 0x01 || (m >= 0xd0 && m <= 0xd7)) continue; /* TEM / stray RSTn */
        if (m == 0xd8) JFAIL(j, "unexpected SOI");
        if (m == 0xd9) {
            if (!j->frame || !j->nscans) JFAIL(j, "JPEG has no image data");
            return 0;
        }
        if (pos + 2 > n) JFAIL(j, "truncated JPEG segment");
        size_t len = be16(d + pos);
        if (len < 2 || pos + len > n) JFAIL(j, "truncated JPEG segment");
        const uint8_t *p = d + pos + 2;
        size_t pn = len - 2;
        pos += len;
        switch (m) {
        case 0xc0:
        case 0xc1:
        case 0xc2:
            if (parse_sof(j, m, p, pn) < 0) return -1;
            break;
        case 0xc3:
        case 0xc5:
        case 0xc6:
        case 0xc7:
        case 0xcb:
        case 0xcd:
        case 0xce:
        case 0xcf: JFAIL(j, "unsupported JPEG process (lossless / hierarchical)");
        case 0xc9:
        case 0xca: JFAIL(j, "unsupported JPEG (arithmetic coding)");
        case 0xc4:
            if (parse_dht(j, p, pn) < 0) return -1;
            break;
        case 0xdb:
            if (parse_dqt(j, p, pn) < 0) return -1;
            break;
        case 0xdd:
            if (pn < 2) JFAIL(j, "short DRI");
            j->ri = be16(p);
            break;
        case 0xda:
            if (parse_sos(j, p, pn, &pos) < 0) return -1;
            break;
        case 0xe0:
            if (pn >= 5 && !memcmp(p, "JFIF\0", 5)) j->jfif = true;
            break;
        case 0xee:
            if (pn >= 12 && !memcmp(p, "Adobe", 5)) j->adobe = true, j->adobe_xf = p[11];
            break;
        default: break; /* APPn, COM, DNL, ... */
        }
    }
}

int img_jpeg_decode(const uint8_t *d, size_t n, img_t *out, char **err) {
    out->w = out->h = 0;
    out->px = NULL;
    if (err) *err = NULL;
    jdec_t *j = xcalloc(1, sizeof *j);
    j->d = d, j->n = d ? n : 0;
    dct_basis(j->cosk);
    int rc = decode_markers(j);
    if (rc == 0) {
        if (j->prog) prog_finish(j);
        emit_rgba(j);
        *out = j->out;
        j->out.px = NULL;
    } else {
        if (err) *err = xstrdup(j->msg ? j->msg : "JPEG decode error");
        img_free(&j->out);
    }
    for (int i = 0; i < 4; i++) {
        free(j->c[i].plane);
        free(j->c[i].coef);
    }
    free(j);
    return rc;
}

/* ======================================================================== encoder */

static const uint8_t std_q_lum[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,  14, 13, 16, 24,  40,  57,
    69, 56, 14, 17, 22,  29,  51,  87,  80, 62, 18, 22, 37,  56,  68,  109, 103, 77, 24, 35,  55,  64,
    81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
static const uint8_t std_q_chr[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

/* Annex K.3 Huffman tables: 16 code-length counts followed by the symbols */
static const uint8_t std_dc_lum[16 + 12] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
                                            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t std_dc_chr[16 + 12] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
                                            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t std_ac_lum[16 + 162] = {
    0,    2,    1,    3,    3,    2,    4,    3,    5,    5,    4,    4,    0,    0,    1,    0x7d,
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};
static const uint8_t std_ac_chr[16 + 162] = {
    0,    2,    1,    2,    4,    4,    3,    4,    7,    5,    4,    4,    0,    1,    2,    0x77,
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

typedef struct {
    uint16_t code[256];
    uint8_t size[256];
} jehuff_t;

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    uint32_t acc;
    int nacc;
} jw_t;

static void jw_byte(jw_t *w, uint8_t b) {
    if (w->len == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 4096;
        w->buf = xrealloc(w->buf, w->cap);
    }
    w->buf[w->len++] = b;
}

static void jw_bytes(jw_t *w, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) jw_byte(w, p[i]);
}

static void jw_bits(jw_t *w, uint32_t v, int size) {
    if (!size) return;
    w->acc = (w->acc << size) | (v & ((1u << size) - 1));
    w->nacc += size;
    while (w->nacc >= 8) {
        uint8_t b = (uint8_t)(w->acc >> (w->nacc - 8));
        jw_byte(w, b);
        if (b == 0xff) jw_byte(w, 0);
        w->nacc -= 8;
    }
    w->acc &= (1u << w->nacc) - 1;
}

/* pad the last byte with 1 bits */
static void jw_flush(jw_t *w) {
    if (w->nacc) jw_bits(w, 0x7f, 8 - w->nacc);
}

static void jw_seg(jw_t *w, uint8_t marker, size_t len) {
    uint8_t h[4] = {0xff, marker, (uint8_t)(len >> 8), (uint8_t)len};
    jw_bytes(w, h, 4);
}

static void ehuff_build(jehuff_t *e, const uint8_t *tab) {
    memset(e, 0, sizeof *e);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < tab[l - 1]; i++, k++) {
            e->code[tab[16 + k]] = (uint16_t)code++;
            e->size[tab[16 + k]] = (uint8_t)l;
        }
        code <<= 1;
    }
}

static int nbits_of(int v) {
    int n = 0;
    for (unsigned a = (unsigned)(v < 0 ? -v : v); a; a >>= 1) n++;
    return n;
}

typedef struct {
    jw_t w;
    jehuff_t dc[2], ac[2];
    float qdiv[2][64]; /* natural order */
    float cosk[8][8];
} jenc_t;

/* FDCT + quantize + entropy-code one 8x8 block of samples (stride sw). */
static void enc_block(jenc_t *e, const uint8_t *s, size_t sw, int tbl, int *pred) {
    float t[64];
    for (int y = 0; y < 8; y++) /* horizontal pass */
        for (int u = 0; u < 8; u++) {
            float a = 0;
            for (int x = 0; x < 8; x++) a += e->cosk[u][x] * ((float)s[(size_t)y * sw + x] - 128.0f);
            t[y * 8 + u] = a;
        }
    int q[64];
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++) {
            float a = 0;
            for (int y = 0; y < 8; y++) a += e->cosk[v][y] * t[y * 8 + u];
            int c = (int)lroundf(a / e->qdiv[tbl][v * 8 + u]);
            q[v * 8 + u] = c < -1023 ? -1023 : c > 1023 ? 1023 : c;
        }
    int diff = q[0] - *pred;
    *pred = q[0];
    int nb = nbits_of(diff);
    jw_bits(&e->w, e->dc[tbl].code[nb], e->dc[tbl].size[nb]);
    jw_bits(&e->w, (uint32_t)(diff < 0 ? diff - 1 : diff), nb);
    int run = 0;
    const jehuff_t *ac = &e->ac[tbl];
    for (int k = 1; k < 64; k++) {
        int v = q[zz[k]];
        if (!v) {
            run++;
            continue;
        }
        while (run > 15) {
            jw_bits(&e->w, ac->code[0xf0], ac->size[0xf0]);
            run -= 16;
        }
        nb = nbits_of(v);
        int sym = run << 4 | nb;
        jw_bits(&e->w, ac->code[sym], ac->size[sym]);
        jw_bits(&e->w, (uint32_t)(v < 0 ? v - 1 : v), nb);
        run = 0;
    }
    if (run) jw_bits(&e->w, ac->code[0], ac->size[0]);
}

static inline void rgb_ycc(const uint8_t *p, int *y, int *cb, int *cr) {
    int r = p[0], g = p[1], b = p[2];
    *y = (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;
    *cb = (-11059 * r - 21709 * g + 32768 * b + (128 << 16) + 32767) >> 16;
    *cr = (32768 * r - 27439 * g - 5329 * b + (128 << 16) + 32767) >> 16;
}

/* Baseline encoder with luma sampling hs x vs (chroma 1x1) and an optional restart interval. Exposed
 * (undeclared in img.h) so tests can produce 4:2:2 / 4:4:0 / 4:1:1 streams. */
uint8_t *img_jpeg_encode_ex(const img_t *im, int quality, int hs, int vs, int restart, size_t *len);
uint8_t *img_jpeg_encode_ex(const img_t *im, int quality, int hs, int vs, int restart, size_t *len) {
    *len = 0;
    if (!im || !im->px || im->w <= 0 || im->h <= 0 || im->w > 65535 || im->h > 65535) return NULL;
    if (hs < 1 || hs > 4 || vs < 1 || vs > 4 || restart < 0 || restart > 65535) return NULL;
    quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
    int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    jenc_t *e = xcalloc(1, sizeof *e);
    dct_basis(e->cosk);
    uint8_t qz[2][64]; /* zigzag order for DQT */
    for (int t = 0; t < 2; t++)
        for (int k = 0; k < 64; k++) {
            int v = ((t ? std_q_chr : std_q_lum)[zz[k]] * scale + 50) / 100;
            v = v < 1 ? 1 : v > 255 ? 255 : v;
            qz[t][k] = (uint8_t)v;
            e->qdiv[t][zz[k]] = (float)v;
        }
    ehuff_build(&e->dc[0], std_dc_lum);
    ehuff_build(&e->dc[1], std_dc_chr);
    ehuff_build(&e->ac[0], std_ac_lum);
    ehuff_build(&e->ac[1], std_ac_chr);
    jw_t *w = &e->w;

    static const uint8_t jfif[16] = {0xff, 0xd8, 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1};
    jw_bytes(w, jfif, sizeof jfif);
    jw_bytes(w, (const uint8_t[]){0, 1, 0, 0}, 4);
    jw_seg(w, 0xdb, 2 + 2 * 65);
    for (int t = 0; t < 2; t++) {
        jw_byte(w, (uint8_t)t);
        jw_bytes(w, qz[t], 64);
    }
    jw_seg(w, 0xc0, 17);
    uint8_t sof[15] = {8, (uint8_t)(im->h >> 8), (uint8_t)im->h, (uint8_t)(im->w >> 8), (uint8_t)im->w, 3,
                       1, (uint8_t)(hs << 4 | vs), 0, 2, 0x11, 1, 3, 0x11, 1};
    jw_bytes(w, sof, sizeof sof);
    jw_seg(w, 0xc4, 2 + 4 * 17 + 2 * 12 + 2 * 162);
    const uint8_t *tabs[4] = {std_dc_lum, std_ac_lum, std_dc_chr, std_ac_chr};
    static const uint8_t tc[4] = {0x00, 0x10, 0x01, 0x11};
    for (int t = 0; t < 4; t++) {
        jw_byte(w, tc[t]);
        jw_bytes(w, tabs[t], 16 + (t & 1 ? 162 : 12));
    }
    if (restart) {
        jw_seg(w, 0xdd, 4);
        jw_bytes(w, (const uint8_t[]){(uint8_t)(restart >> 8), (uint8_t)restart}, 2);
    }
    jw_seg(w, 0xda, 12);
    jw_bytes(w, (const uint8_t[]){3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0}, 10);

    int mw = 8 * hs, mh = 8 * vs;
    int mcux = (im->w + mw - 1) / mw, mcuy = (im->h + mh - 1) / mh;
    size_t yw = (size_t)mcux * mw, cwid = (size_t)mcux * 8;
    uint8_t *yb = xmalloc(yw * mh), *cb = xmalloc(cwid * 8), *cr = xmalloc(cwid * 8);
    int *ycc = xmalloc(sizeof(int) * 3 * yw * mh);
    int pred[3] = {0, 0, 0}, left = restart, rst = 0;
    for (int my = 0; my < mcuy; my++) {
        /* convert one MCU row with edge replication */
        for (int y = 0; y < mh; y++) {
            int sy = MIN(my * mh + y, im->h - 1);
            const uint8_t *row = im->px + (size_t)sy * im->w * 4;
            for (size_t x = 0; x < yw; x++) {
                int *o = ycc + 3 * ((size_t)y * yw + x);
                rgb_ycc(row + (size_t)MIN((int)x, im->w - 1) * 4, &o[0], &o[1], &o[2]);
                yb[(size_t)y * yw + x] = (uint8_t)o[0];
            }
        }
        int n = hs * vs;
        for (int y = 0; y < 8; y++)
            for (size_t x = 0; x < cwid; x++) {
                int sb = 0, sr = 0;
                for (int dy = 0; dy < vs; dy++)
                    for (int dx = 0; dx < hs; dx++) {
                        const int *o = ycc + 3 * ((size_t)(y * vs + dy) * yw + x * hs + dx);
                        sb += o[1], sr += o[2];
                    }
                cb[(size_t)y * cwid + x] = (uint8_t)((sb + n / 2) / n);
                cr[(size_t)y * cwid + x] = (uint8_t)((sr + n / 2) / n);
            }
        for (int mx = 0; mx < mcux; mx++) {
            if (restart) {
                if (left == 0) {
                    jw_flush(w);
                    jw_byte(w, 0xff);
                    jw_byte(w, (uint8_t)(0xd0 + (rst++ & 7)));
                    pred[0] = pred[1] = pred[2] = 0;
                    left = restart;
                }
                left--;
            }
            for (int by = 0; by < vs; by++)
                for (int bx = 0; bx < hs; bx++)
                    enc_block(e, yb + (size_t)by * 8 * yw + (size_t)mx * mw + bx * 8, yw, 0, &pred[0]);
            enc_block(e, cb + (size_t)mx * 8, cwid, 1, &pred[1]);
            enc_block(e, cr + (size_t)mx * 8, cwid, 1, &pred[2]);
        }
    }
    jw_flush(w);
    jw_byte(w, 0xff);
    jw_byte(w, 0xd9);
    free(yb);
    free(cb);
    free(cr);
    free(ycc);
    uint8_t *out = w->buf;
    *len = w->len;
    free(e);
    return out;
}

uint8_t *img_jpeg_encode(const img_t *im, int quality, size_t *len) {
    return img_jpeg_encode_ex(im, quality, 2, 2, 0, len);
}
