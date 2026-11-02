/* vp8l.c - WebP lossless (VP8L) bitstream decoder, RFC 9649 section 3: all four transforms (predictor,
 * color, subtract-green, color indexing with pixel bundling), color cache, LZ77 with the 2-D distance map,
 * meta prefix codes (entropy image), simple and normal code-length codes.
 * Input is untrusted: every code is validated (complete prefix codes, alphabet bounds), back-references
 * must stay inside the decoded pixels, recursion depth is fixed (sub-images have no transforms), and the
 * prefix-code tables are kept under a memory budget. */
#include "img/img.h"
#include "img/vp8.h"
#include "core/core.h"

/* (xi, yi) offsets of distance codes 1..120 (RFC 9649 Figure 20, extracted from the RFC text) */
const int8_t vp8l_dist_map[120][2] = {
    {0, 1}, {1, 0}, {1, 1}, {-1, 1}, {0, 2}, {2, 0}, {1, 2}, {-1, 2}, {2, 1}, {-2, 1},
    {2, 2}, {-2, 2}, {0, 3}, {3, 0}, {1, 3}, {-1, 3}, {3, 1}, {-3, 1}, {2, 3}, {-2, 3},
    {3, 2}, {-3, 2}, {0, 4}, {4, 0}, {1, 4}, {-1, 4}, {4, 1}, {-4, 1}, {3, 3}, {-3, 3},
    {2, 4}, {-2, 4}, {4, 2}, {-4, 2}, {0, 5}, {3, 4}, {-3, 4}, {4, 3}, {-4, 3}, {5, 0},
    {1, 5}, {-1, 5}, {5, 1}, {-5, 1}, {2, 5}, {-2, 5}, {5, 2}, {-5, 2}, {4, 4}, {-4, 4},
    {3, 5}, {-3, 5}, {5, 3}, {-5, 3}, {0, 6}, {6, 0}, {1, 6}, {-1, 6}, {6, 1}, {-6, 1},
    {2, 6}, {-2, 6}, {6, 2}, {-6, 2}, {4, 5}, {-4, 5}, {5, 4}, {-5, 4}, {3, 6}, {-3, 6},
    {6, 3}, {-6, 3}, {0, 7}, {7, 0}, {1, 7}, {-1, 7}, {5, 5}, {-5, 5}, {7, 1}, {-7, 1},
    {4, 6}, {-4, 6}, {6, 4}, {-6, 4}, {2, 7}, {-2, 7}, {7, 2}, {-7, 2}, {3, 7}, {-3, 7},
    {7, 3}, {-7, 3}, {5, 6}, {-5, 6}, {6, 5}, {-6, 5}, {8, 0}, {4, 7}, {-4, 7}, {7, 4},
    {-7, 4}, {8, 1}, {8, 2}, {6, 6}, {-6, 6}, {8, 3}, {5, 7}, {-5, 7}, {7, 5}, {-7, 5},
    {8, 4}, {6, 7}, {-6, 7}, {7, 6}, {-7, 6}, {8, 5}, {7, 7}, {-7, 7}, {8, 6}, {8, 7},
};

static const uint8_t cl_order[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

#define MAX_CODE_LEN 15
#define FAST_BITS 9
#define HUFF_BUDGET (64u << 20) /* bytes of prefix-code tables per image */

/* ---------------------------------------------------------------- bit reader (LSB first) */
typedef struct {
    const uint8_t *p, *end;
    uint64_t val;
    int nbits;
    size_t over; /* zero bytes supplied past the end */
} lbr_t;

static void br_fill(lbr_t *b) {
    while (b->nbits <= 56) {
        uint64_t byte = 0;
        if (b->p < b->end) byte = *b->p++;
        else b->over++;
        b->val |= byte << b->nbits;
        b->nbits += 8;
    }
}

static inline uint32_t br_read(lbr_t *b, int n) {
    if (b->nbits < n) br_fill(b);
    uint32_t v = (uint32_t)(b->val & ((1ull << n) - 1));
    b->val >>= n, b->nbits -= n;
    return v;
}

/* consumed bits beyond the real data? */
static bool br_eos(const lbr_t *b) { return b->over * 8 > (size_t)b->nbits; }

/* ---------------------------------------------------------------- prefix codes */
typedef struct {
    int single;       /* >= 0: the only symbol (zero-length code) */
    int fbits;        /* bits indexed by fast[] */
    uint16_t *fast;   /* reversed code -> sym << 4 | len (len 0: longer code) */
    uint16_t *sorted; /* symbols ordered by (length, value) */
    uint16_t count[MAX_CODE_LEN + 1];
} huff_t;

typedef struct {
    huff_t h[5];
} hgroup_t;

typedef struct {
    lbr_t br;
    const char *err;
    size_t budget; /* remaining prefix-table bytes */
} vl_t;

static int vfail(vl_t *v, const char *msg) {
    if (!v->err) v->err = msg;
    return -1;
}

static void huff_free(huff_t *h) {
    free(h->fast), free(h->sorted);
    h->fast = NULL, h->sorted = NULL;
}

static int huff_build(vl_t *v, huff_t *h, const uint8_t *lens, int n) {
    memset(h, 0, sizeof *h);
    h->single = -1;
    int nz = 0, last = 0, maxlen = 0;
    for (int i = 0; i < n; i++)
        if (lens[i]) h->count[lens[i]]++, nz++, last = i, maxlen = MAX(maxlen, lens[i]);
    if (nz == 0) return vfail(v, "VP8L: empty prefix code");
    if (nz == 1) {
        h->single = last;
        return 0;
    }
    int left = 1;
    for (int l = 1; l <= MAX_CODE_LEN; l++) {
        left = (left << 1) - h->count[l];
        if (left < 0) return vfail(v, "VP8L: over-subscribed prefix code");
    }
    if (left) return vfail(v, "VP8L: incomplete prefix code");
    h->fbits = MIN(maxlen, FAST_BITS);
    size_t need = ((size_t)1 << h->fbits) * 2 + (size_t)nz * 2;
    if (need > v->budget) return vfail(v, "VP8L: prefix codes exceed the memory budget");
    v->budget -= need;
    h->fast = xcalloc((size_t)1 << h->fbits, 2);
    h->sorted = xmalloc((size_t)nz * 2);
    int offs[MAX_CODE_LEN + 2];
    offs[1] = 0;
    for (int l = 1; l <= MAX_CODE_LEN; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (lens[i]) h->sorted[offs[lens[i]]++] = (uint16_t)i;
    /* canonical codes: consecutive within a length, first code of length l = (prev first + count) << 1 */
    int code = 0, k = 0;
    for (int l = 1; l <= MAX_CODE_LEN; l++) {
        for (int j = 0; j < h->count[l]; j++, k++, code++) {
            if (l > h->fbits) continue;
            int rev = 0;
            for (int b = 0; b < l; b++) rev |= ((code >> b) & 1) << (l - 1 - b);
            for (int f = rev; f < (1 << h->fbits); f += 1 << l) h->fast[f] = (uint16_t)(h->sorted[k] << 4 | l);
        }
        code <<= 1;
    }
    return 0;
}

static inline int huff_read(lbr_t *b, const huff_t *h) {
    if (h->single >= 0) return h->single;
    if (b->nbits < MAX_CODE_LEN) br_fill(b);
    uint16_t e = h->fast[b->val & ((1u << h->fbits) - 1)];
    if (e & 15) {
        b->val >>= e & 15, b->nbits -= e & 15;
        return e >> 4;
    }
    /* long code: canonical decode one bit at a time */
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= MAX_CODE_LEN; l++) {
        code |= (int)(b->val & 1);
        b->val >>= 1, b->nbits--;
        int cnt = h->count[l];
        if (code - first < cnt) return h->sorted[index + code - first];
        index += cnt, first = (first + cnt) << 1, code <<= 1;
    }
    return 0; /* unreachable for a complete code */
}

static int read_code(vl_t *v, huff_t *h, int alphabet) {
    lbr_t *b = &v->br;
    uint8_t lens[256 + 24 + 2048] = {0};
    if (br_read(b, 1)) { /* simple code: one or two symbols */
        int nsym = (int)br_read(b, 1) + 1;
        int s0 = (int)br_read(b, br_read(b, 1) ? 8 : 1);
        lens[s0] = 1;
        if (nsym == 2) lens[br_read(b, 8)] = 1;
    } else {
        uint8_t cll[19] = {0};
        int ncl = (int)br_read(b, 4) + 4;
        for (int i = 0; i < ncl; i++) cll[cl_order[i]] = (uint8_t)br_read(b, 3);
        huff_t clh;
        size_t keep = v->budget;
        if (huff_build(v, &clh, cll, 19) < 0) return -1;
        v->budget = keep; /* temporary table */
        int max_sym = alphabet;
        if (br_read(b, 1)) {
            int nb = 2 + 2 * (int)br_read(b, 3);
            max_sym = 2 + (int)br_read(b, nb);
            if (max_sym > alphabet) {
                huff_free(&clh);
                return vfail(v, "VP8L: bad code length count");
            }
        }
        int sym = 0, prev = 8;
        while (sym < alphabet && max_sym-- > 0) {
            int c = huff_read(b, &clh);
            if (c < 16) {
                lens[sym++] = (uint8_t)c;
                if (c) prev = c;
                continue;
            }
            int rep = c == 16 ? 3 + (int)br_read(b, 2) : c == 17 ? 3 + (int)br_read(b, 3) : 11 + (int)br_read(b, 7);
            if (sym + rep > alphabet) {
                huff_free(&clh);
                return vfail(v, "VP8L: code lengths overflow the alphabet");
            }
            memset(lens + sym, c == 16 ? prev : 0, (size_t)rep);
            sym += rep;
        }
        huff_free(&clh);
    }
    if (br_eos(b)) return vfail(v, "VP8L: truncated prefix code");
    return huff_build(v, h, lens, alphabet);
}

static int read_group(vl_t *v, hgroup_t *g, int cache_bits) {
    static const int alpha[5] = {256 + 24, 256, 256, 256, 40};
    memset(g, 0, sizeof *g);
    for (int i = 0; i < 5; i++)
        if (read_code(v, &g->h[i], alpha[i] + (i == 0 && cache_bits ? 1 << cache_bits : 0)) < 0) {
            for (int j = 0; j <= i; j++) huff_free(&g->h[j]);
            return -1;
        }
    return 0;
}

static void group_free(hgroup_t *g) {
    for (int i = 0; i < 5; i++) huff_free(&g->h[i]);
}

/* ---------------------------------------------------------------- entropy-coded image data */
static inline uint32_t prefix_value(lbr_t *b, int code) {
    if (code < 4) return (uint32_t)code + 1;
    int extra = (code - 2) >> 1;
    uint32_t off = (uint32_t)(2 + (code & 1)) << extra;
    return off + br_read(b, extra) + 1;
}

static inline uint32_t cache_hash(uint32_t c, int bits) { return (0x1e35a7bdu * c) >> (32 - bits); }

/* Decode w x h pixels. main: the image is the ARGB image (may carry meta prefix codes). */
static int decode_image(vl_t *v, int w, int h, bool main, uint32_t **out);

static int decode_data(vl_t *v, uint32_t *px, int w, int h, int cache_bits, hgroup_t *groups, const uint32_t *meta, int meta_bits,
                       int meta_w, const int32_t *map) {
    lbr_t *b = &v->br;
    size_t total = (size_t)w * h, pos = 0;
    uint32_t *cache = cache_bits ? xcalloc((size_t)1 << cache_bits, 4) : NULL;
    int x = 0, y = 0, rc = -1;
    const hgroup_t *g = groups;
    int mask = meta ? (1 << meta_bits) - 1 : -1;
    while (pos < total) {
        if (meta && (x & mask) == 0) g = &groups[map[(meta[(size_t)(y >> meta_bits) * meta_w + (x >> meta_bits)] >> 8) & 0xffff]];
        int s = huff_read(b, &g->h[0]);
        if (s < 256) {
            int r = huff_read(b, &g->h[1]), bl = huff_read(b, &g->h[2]), a = huff_read(b, &g->h[3]);
            uint32_t c = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)s << 8 | (uint32_t)bl;
            px[pos++] = c;
            if (cache) cache[cache_hash(c, cache_bits)] = c;
            if (++x == w) {
                x = 0, y++;
                if (br_eos(b)) break;
            }
        } else if (s < 256 + 24) {
            uint32_t len = prefix_value(b, s - 256);
            int dc = huff_read(b, &g->h[4]);
            uint32_t dcode = prefix_value(b, dc);
            size_t dist;
            if (dcode > 120) dist = dcode - 120;
            else {
                int64_t dd = vp8l_dist_map[dcode - 1][0] + (int64_t)vp8l_dist_map[dcode - 1][1] * w;
                dist = dd < 1 ? 1 : (size_t)dd;
            }
            if (dist > pos || len > total - pos) {
                vfail(v, "VP8L: invalid back-reference");
                goto done;
            }
            for (uint32_t i = 0; i < len; i++, pos++) {
                uint32_t c = px[pos - dist];
                px[pos] = c;
                if (cache) cache[cache_hash(c, cache_bits)] = c;
            }
            x += (int)(len % (uint32_t)w), y += (int)(len / (uint32_t)w);
            if (x >= w) x -= w, y++;
            if (br_eos(b)) break;
            if (meta && pos < total) g = &groups[map[(meta[(size_t)(y >> meta_bits) * meta_w + (x >> meta_bits)] >> 8) & 0xffff]];
        } else {
            int idx = s - 256 - 24;
            if (!cache || idx >= (1 << cache_bits)) {
                vfail(v, "VP8L: bad color cache index");
                goto done;
            }
            uint32_t c = cache[idx];
            px[pos++] = c;
            cache[cache_hash(c, cache_bits)] = c;
            if (++x == w) {
                x = 0, y++;
                if (br_eos(b)) break;
            }
        }
    }
    if (pos < total || br_eos(b)) vfail(v, "VP8L: truncated image data");
    else rc = 0;
done:
    free(cache);
    return rc;
}

static int decode_image(vl_t *v, int w, int h, bool main, uint32_t **out) {
    lbr_t *b = &v->br;
    *out = NULL;
    int cache_bits = 0;
    if (br_read(b, 1)) {
        cache_bits = (int)br_read(b, 4);
        if (cache_bits < 1 || cache_bits > 11) return vfail(v, "VP8L: bad color cache size");
    }
    uint32_t *meta = NULL;
    int meta_bits = 0, meta_w = 0, ngroups = 1;
    int32_t *map = NULL;
    if (main && br_read(b, 1)) {
        meta_bits = (int)br_read(b, 3) + 2;
        meta_w = (w + (1 << meta_bits) - 1) >> meta_bits;
        int meta_h = (h + (1 << meta_bits) - 1) >> meta_bits;
        if (decode_image(v, meta_w, meta_h, false, &meta) < 0) return -1;
        /* compact the used meta codes; unused groups are parsed and dropped */
        map = xmalloc(65536 * sizeof *map);
        for (int i = 0; i < 65536; i++) map[i] = -1;
        int maxc = 0;
        for (size_t i = 0; i < (size_t)meta_w * meta_h; i++) maxc = MAX(maxc, (int)((meta[i] >> 8) & 0xffff));
        ngroups = 0;
        for (size_t i = 0; i < (size_t)meta_w * meta_h; i++) {
            int c = (meta[i] >> 8) & 0xffff;
            if (map[c] < 0) map[c] = ngroups++;
        }
        hgroup_t *groups = xcalloc((size_t)ngroups, sizeof *groups);
        int rc = 0;
        for (int c = 0; c <= maxc && rc == 0; c++) {
            if (map[c] >= 0) rc = read_group(v, &groups[map[c]], cache_bits);
            else {
                hgroup_t tmp;
                size_t keep = v->budget;
                rc = read_group(v, &tmp, cache_bits);
                if (rc == 0) group_free(&tmp);
                v->budget = keep;
            }
        }
        uint32_t *px = NULL;
        if (rc == 0) {
            px = xmalloc((size_t)w * h * sizeof *px);
            rc = decode_data(v, px, w, h, cache_bits, groups, meta, meta_bits, meta_w, map);
        }
        for (int i = 0; i < ngroups; i++) group_free(&groups[i]);
        free(groups), free(map), free(meta);
        if (rc < 0) {
            free(px);
            return -1;
        }
        *out = px;
        return 0;
    }
    hgroup_t g;
    if (read_group(v, &g, cache_bits) < 0) return -1;
    uint32_t *px = xmalloc((size_t)w * h * sizeof *px);
    int rc = decode_data(v, px, w, h, cache_bits, &g, NULL, 0, 0, NULL);
    group_free(&g);
    if (rc < 0) {
        free(px);
        return -1;
    }
    *out = px;
    return 0;
}

/* ---------------------------------------------------------------- inverse transforms */
enum { T_PREDICT, T_COLOR, T_SUBGREEN, T_INDEX };

typedef struct {
    int type, bits, xsize; /* xsize: image width before this transform was applied by the encoder */
    int ncolors;
    uint32_t *data;
} xform_t;

static inline uint32_t add_px(uint32_t a, uint32_t b) {
    return (((a & 0xff00ff00u) + (b & 0xff00ff00u)) & 0xff00ff00u) | (((a & 0x00ff00ffu) + (b & 0x00ff00ffu)) & 0x00ff00ffu);
}

static inline uint32_t avg2(uint32_t a, uint32_t b) { return (((a ^ b) & 0xfefefefeu) >> 1) + (a & b); }

static inline int ch(uint32_t c, int s) { return (int)((c >> s) & 0xff); }

static inline uint32_t select_px(uint32_t L, uint32_t T, uint32_t TL) {
    int pl = 0, pt = 0;
    for (int s = 0; s < 32; s += 8) {
        int p = ch(L, s) + ch(T, s) - ch(TL, s);
        pl += abs(p - ch(L, s)), pt += abs(p - ch(T, s));
    }
    return pl < pt ? L : T;
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline uint32_t clamp_full(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) r |= (uint32_t)clamp255(ch(a, s) + ch(b, s) - ch(c, s)) << s;
    return r;
}

static inline uint32_t clamp_half(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        int x = ch(a, s);
        r |= (uint32_t)clamp255(x + (x - ch(b, s)) / 2) << s;
    }
    return r;
}

static uint32_t predict(int mode, const uint32_t *p, int w) {
    uint32_t L = p[-1], T = p[-w], TL = p[-w - 1], TR = p[-w + 1];
    switch (mode) {
    case 1: return L;
    case 2: return T;
    case 3: return TR;
    case 4: return TL;
    case 5: return avg2(avg2(L, TR), T);
    case 6: return avg2(L, TL);
    case 7: return avg2(L, T);
    case 8: return avg2(TL, T);
    case 9: return avg2(T, TR);
    case 10: return avg2(avg2(L, TL), avg2(T, TR));
    case 11: return select_px(L, T, TL);
    case 12: return clamp_full(L, T, TL);
    case 13: return clamp_half(avg2(L, T), TL);
    default: return 0xff000000u; /* 0, and the out-of-range 14 / 15 as libwebp */
    }
}

static void inv_predict(const xform_t *t, uint32_t *px, int w, int h) {
    int bw = (w + (1 << t->bits) - 1) >> t->bits;
    px[0] = add_px(px[0], 0xff000000u);
    for (int x = 1; x < w; x++) px[x] = add_px(px[x], px[x - 1]);
    for (int y = 1; y < h; y++) {
        uint32_t *row = px + (size_t)y * w;
        const uint32_t *modes = t->data + (size_t)(y >> t->bits) * bw;
        row[0] = add_px(row[0], row[-w]);
        for (int x = 1; x < w; x++) row[x] = add_px(row[x], predict((modes[x >> t->bits] >> 8) & 0xf, row + x, w));
    }
}

static inline int ctd(int8_t t, int8_t c) { return ((int)t * c) >> 5; }

static void inv_color(const xform_t *t, uint32_t *px, int w, int h) {
    int bw = (w + (1 << t->bits) - 1) >> t->bits;
    for (int y = 0; y < h; y++) {
        uint32_t *row = px + (size_t)y * w;
        const uint32_t *el = t->data + (size_t)(y >> t->bits) * bw;
        for (int x = 0; x < w; x++) {
            uint32_t e = el[x >> t->bits], c = row[x];
            int8_t g2r = (int8_t)(e & 0xff), g2b = (int8_t)((e >> 8) & 0xff), r2b = (int8_t)((e >> 16) & 0xff);
            int8_t g = (int8_t)((c >> 8) & 0xff);
            int r = (int)((c >> 16) & 0xff) + ctd(g2r, g);
            int bl = (int)(c & 0xff) + ctd(g2b, g) + ctd(r2b, (int8_t)(r & 0xff));
            row[x] = (c & 0xff00ff00u) | (uint32_t)(r & 0xff) << 16 | (uint32_t)(bl & 0xff);
        }
    }
}

static void inv_subgreen(uint32_t *px, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t c = px[i], g = (c >> 8) & 0xff;
        px[i] = (c & 0xff00ff00u) | ((((c >> 16) + g) & 0xff) << 16) | ((c + g) & 0xff);
    }
}

/* src: w_packed x h indices -> returns the w x h colors */
static uint32_t *inv_index(const xform_t *t, const uint32_t *src, int wp, int h) {
    int w = t->xsize, bits = t->bits, bpp = 8 >> bits, m = (1 << bpp) - 1;
    uint32_t *dst = xmalloc((size_t)w * h * sizeof *dst);
    for (int y = 0; y < h; y++) {
        const uint32_t *s = src + (size_t)y * wp;
        uint32_t *o = dst + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int g = (s[x >> bits] >> 8) & 0xff;
            int idx = bits ? (g >> ((x & ((1 << bits) - 1)) * bpp)) & m : g;
            o[x] = idx < t->ncolors ? t->data[idx] : 0;
        }
    }
    return dst;
}

int vp8l_decode(const uint8_t *d, size_t n, bool headerless, int *w, int *h, uint32_t **argb, const char **err) {
    vl_t v = {.br = {.p = d, .end = d + n}, .budget = HUFF_BUDGET};
    lbr_t *b = &v.br;
    *argb = NULL;
    if (!headerless) {
        if (n < 5 || d[0] != 0x2f) return *err = "VP8L: bad signature", -1;
        b->p++;
        *w = (int)br_read(b, 14) + 1;
        *h = (int)br_read(b, 14) + 1;
        br_read(b, 1); /* alpha hint */
        if (br_read(b, 3) != 0) return *err = "VP8L: unknown version", -1;
    }
    int W = *w, H = *h;
    if (W < 1 || H < 1 || (uint64_t)W * H > IMG_MAX_PIXELS) return *err = "VP8L: image too large", -1;
    xform_t xf[4];
    int nx = 0, xsize = W, seen = 0, rc = -1;
    uint32_t *px = NULL;
    while (br_read(b, 1)) {
        int type = (int)br_read(b, 2);
        if (seen & (1 << type)) {
            vfail(&v, "VP8L: repeated transform");
            goto done;
        }
        seen |= 1 << type;
        xform_t *t = &xf[nx++];
        *t = (xform_t){.type = type, .xsize = xsize};
        if (type == T_PREDICT || type == T_COLOR) {
            t->bits = (int)br_read(b, 3) + 2;
            int bw = (xsize + (1 << t->bits) - 1) >> t->bits, bh = (H + (1 << t->bits) - 1) >> t->bits;
            if (decode_image(&v, bw, bh, false, &t->data) < 0) goto done;
        } else if (type == T_INDEX) {
            int nc = (int)br_read(b, 8) + 1;
            t->ncolors = nc;
            t->bits = nc > 16 ? 0 : nc > 4 ? 1 : nc > 2 ? 2 : 3;
            uint32_t *pal = NULL;
            if (decode_image(&v, nc, 1, false, &pal) < 0) goto done;
            t->data = xcalloc(256, sizeof *t->data); /* out-of-range indices read 0 */
            t->data[0] = pal[0];
            for (int i = 1; i < nc; i++) t->data[i] = add_px(pal[i], t->data[i - 1]);
            free(pal);
            xsize = (xsize + (1 << t->bits) - 1) >> t->bits;
        }
    }
    if (decode_image(&v, xsize, H, true, &px) < 0) goto done;
    for (int i = nx - 1; i >= 0; i--) {
        xform_t *t = &xf[i];
        switch (t->type) {
        case T_PREDICT: inv_predict(t, px, xsize, H); break;
        case T_COLOR: inv_color(t, px, xsize, H); break;
        case T_SUBGREEN: inv_subgreen(px, (size_t)xsize * H); break;
        default: {
            uint32_t *full = inv_index(t, px, xsize, H);
            free(px);
            px = full;
            xsize = t->xsize;
            break;
        }
        }
    }
    *argb = px;
    px = NULL;
    rc = 0;
done:
    for (int i = 0; i < nx; i++) free(xf[i].data);
    free(px);
    if (rc < 0) *err = v.err ? v.err : "VP8L: corrupt bitstream";
    return rc;
}
