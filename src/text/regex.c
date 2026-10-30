/* regex.c - see regex.h. Thompson NFA over UTF-8 bytes + lazy DFA + Pike VM. */
#define _GNU_SOURCE
#include "text/regex.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>

#define CP_MAX 0x10FFFFu
#define RX_MAX_INSTS 400000
#define RX_MAX_REPEAT 10000
#define RX_MAX_NEST 250
#define RX_POOL 16
#define DFA_MEM_LIMIT (16u << 20)

/* ======================================================================
 * Code point classes: sorted, merged ranges
 * ====================================================================== */
typedef struct {
    uint32_t lo, hi;
} crange_t;
typedef VEC(crange_t) cls_t;

static void cls_add(cls_t *c, uint32_t lo, uint32_t hi) {
    if (lo > hi) return;
    crange_t r = {lo, hi};
    vec_push(c, r);
}

static int crange_cmp(const void *a, const void *b) {
    const crange_t *x = a, *y = b;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    return x->hi < y->hi ? -1 : x->hi > y->hi;
}

static void cls_norm(cls_t *c) {
    if (c->n < 2) return;
    qsort(c->v, c->n, sizeof(crange_t), crange_cmp);
    size_t out = 0;
    for (size_t i = 0; i < c->n; i++) {
        if (out > 0 && c->v[i].lo <= c->v[out - 1].hi + 1) {
            if (c->v[i].hi > c->v[out - 1].hi) c->v[out - 1].hi = c->v[i].hi;
        } else {
            c->v[out++] = c->v[i];
        }
    }
    c->n = out;
}

static void cls_negate(cls_t *c) {
    cls_norm(c);
    cls_t r = {0};
    uint32_t next = 0;
    for (size_t i = 0; i < c->n; i++) {
        if (c->v[i].lo > next) cls_add(&r, next, c->v[i].lo - 1);
        next = c->v[i].hi + 1;
    }
    if (next <= CP_MAX) cls_add(&r, next, CP_MAX);
    vec_free(c);
    *c = r;
}

static void cls_union(cls_t *a, const cls_t *b) {
    for (size_t i = 0; i < b->n; i++) vec_push(a, b->v[i]);
    cls_norm(a);
}

static void cls_intersect(cls_t *a, const cls_t *b) {
    cls_norm(a);
    cls_t r = {0};
    size_t i = 0, j = 0;
    while (i < a->n && j < b->n) {
        uint32_t lo = MAX(a->v[i].lo, b->v[j].lo), hi = MIN(a->v[i].hi, b->v[j].hi);
        if (lo <= hi) cls_add(&r, lo, hi);
        if (a->v[i].hi < b->v[j].hi) i++;
        else j++;
    }
    vec_free(a);
    *a = r;
}

static void cls_diff(cls_t *a, const cls_t *b) {
    cls_t nb = {0};
    for (size_t i = 0; i < b->n; i++) vec_push(&nb, b->v[i]);
    cls_negate(&nb);
    cls_intersect(a, &nb);
    vec_free(&nb);
}

static void cls_symdiff(cls_t *a, const cls_t *b) {
    cls_t in = {0};
    for (size_t i = 0; i < a->n; i++) vec_push(&in, a->v[i]);
    cls_intersect(&in, b);
    cls_union(a, b);
    cls_diff(a, &in);
    vec_free(&in);
}

static void cls_remove(cls_t *c, uint32_t cp) {
    cls_t x = {0};
    cls_add(&x, cp, cp);
    cls_diff(c, &x);
    vec_free(&x);
}

static void cls_add_table(cls_t *c, const uint32_t *t, size_t n) {
    for (size_t i = 0; i + 1 < n; i += 2) cls_add(c, t[i], t[i + 1]);
}

/* ======================================================================
 * Simple case folding (ASCII, Latin, Greek, Cyrillic, Armenian, ... approx)
 * ====================================================================== */
typedef struct {
    uint32_t lo, hi;
    int32_t delta; /* non-alt: lower = upper + delta */
    uint8_t alt;   /* alternating pairs: (c - lo) even = upper, +1 = lower */
} fold_ent_t;

static const fold_ent_t FOLDS[] = {
    {'A', 'Z', 32, 0},         {0xC0, 0xD6, 32, 0},       {0xD8, 0xDE, 32, 0},       {0x100, 0x12F, 1, 1},
    {0x132, 0x137, 1, 1},      {0x139, 0x148, 1, 1},      {0x14A, 0x177, 1, 1},      {0x178, 0x178, -121, 0},
    {0x179, 0x17E, 1, 1},      {0x182, 0x185, 1, 1},      {0x1A0, 0x1A5, 1, 1},      {0x1CD, 0x1DC, 1, 1},
    {0x1DE, 0x1EF, 1, 1},      {0x1F8, 0x21F, 1, 1},      {0x222, 0x233, 1, 1},      {0x246, 0x24F, 1, 1},
    {0x386, 0x386, 38, 0},     {0x388, 0x38A, 37, 0},     {0x38C, 0x38C, 64, 0},     {0x38E, 0x38F, 63, 0},
    {0x391, 0x3A1, 32, 0},     {0x3A3, 0x3AB, 32, 0},     {0x3D8, 0x3EF, 1, 1},      {0x400, 0x40F, 80, 0},
    {0x410, 0x42F, 32, 0},     {0x460, 0x481, 1, 1},      {0x48A, 0x4BF, 1, 1},      {0x4C1, 0x4CE, 1, 1},
    {0x4D0, 0x52F, 1, 1},      {0x531, 0x556, 48, 0},     {0x10A0, 0x10C5, 7264, 0}, {0x13A0, 0x13EF, 38864, 0},
    {0x1E00, 0x1E95, 1, 1},    {0x1EA0, 0x1EFF, 1, 1},    {0x1F08, 0x1F0F, -8, 0},   {0x1F18, 0x1F1D, -8, 0},
    {0x1F28, 0x1F2F, -8, 0},   {0x1F38, 0x1F3F, -8, 0},   {0x1F48, 0x1F4D, -8, 0},   {0x1F68, 0x1F6F, -8, 0},
    {0x2160, 0x216F, 16, 0},   {0x24B6, 0x24CF, 26, 0},   {0x2C00, 0x2C2F, 48, 0},   {0x2C80, 0x2CE3, 1, 1},
    {0xA640, 0xA66D, 1, 1},    {0xA680, 0xA69B, 1, 1},    {0xA722, 0xA72F, 1, 1},    {0xA732, 0xA76F, 1, 1},
    {0xFF21, 0xFF3A, 32, 0},   {0x10400, 0x10427, 40, 0}, {0x1E900, 0x1E921, 34, 0},
};

static const uint32_t FOLD_ORBITS[][4] = {
    {0x4B, 0x6B, 0x212A, 0},     {0x53, 0x73, 0x17F, 0},      {0xC5, 0xE5, 0x212B, 0},   {0xB5, 0x39C, 0x3BC, 0},
    {0x3A3, 0x3C2, 0x3C3, 0},    {0xDF, 0x1E9E, 0, 0},        {0x398, 0x3B8, 0x3D1, 0x3F4},
    {0x392, 0x3B2, 0x3D0, 0},    {0x395, 0x3B5, 0x3F5, 0},    {0x39A, 0x3BA, 0x3F0, 0},  {0x3A0, 0x3C0, 0x3D6, 0},
    {0x3A1, 0x3C1, 0x3F1, 0},    {0x3A6, 0x3C6, 0x3D5, 0},    {0x399, 0x3B9, 0x345, 0x1FBE},
    {0x1E60, 0x1E61, 0x1E9B, 0}, {0x3A9, 0x3C9, 0x2126, 0},
};

static void orbit_add(uint32_t *out, int *n, uint32_t c) {
    for (int i = 0; i < *n; i++)
        if (out[i] == c) return;
    if (*n < 8) out[(*n)++] = c;
}

/* All code points equivalent to c under simple case folding (incl. c). Returns count (<= 8). */
static int fold_orbit(uint32_t c, uint32_t out[8]) {
    int n = 0;
    out[n++] = c;
    for (size_t i = 0; i < ARRAY_LEN(FOLDS); i++) {
        const fold_ent_t *e = &FOLDS[i];
        if (c >= e->lo && c <= e->hi) {
            if (e->alt) {
                uint32_t o = ((c - e->lo) & 1) ? c - 1 : c + 1;
                if (o >= e->lo && o <= e->hi) orbit_add(out, &n, o);
            } else {
                orbit_add(out, &n, (uint32_t)((int64_t)c + e->delta));
            }
        } else if (!e->alt) {
            int64_t l = (int64_t)e->lo + e->delta, h = (int64_t)e->hi + e->delta;
            if ((int64_t)c >= l && (int64_t)c <= h) orbit_add(out, &n, (uint32_t)((int64_t)c - e->delta));
        }
    }
    for (size_t i = 0; i < ARRAY_LEN(FOLD_ORBITS); i++) {
        bool hit = false;
        for (int k = 0; k < 4 && FOLD_ORBITS[i][k]; k++)
            if (FOLD_ORBITS[i][k] == c) hit = true;
        if (!hit) {
            /* also join orbits through mapped members (e.g. 'k' reaches 'K' reaches KELVIN) */
            for (int j = 1; j < n && !hit; j++)
                for (int k = 0; k < 4 && FOLD_ORBITS[i][k]; k++)
                    if (FOLD_ORBITS[i][k] == out[j]) hit = true;
        }
        if (hit)
            for (int k = 0; k < 4 && FOLD_ORBITS[i][k]; k++) orbit_add(out, &n, FOLD_ORBITS[i][k]);
    }
    return n;
}

static void fold_iter(cls_t *add, uint32_t rlo, uint32_t rhi, int64_t a, int64_t b) {
    int64_t lo = MAX((int64_t)rlo, a), hi = MIN((int64_t)rhi, b);
    for (int64_t c = lo; c <= hi; c++) {
        uint32_t o[8];
        int n = fold_orbit((uint32_t)c, o);
        for (int i = 1; i < n; i++) cls_add(add, o[i], o[i]);
    }
}

static void cls_fold(cls_t *c) {
    cls_t add = {0};
    for (size_t i = 0; i < c->n; i++) {
        uint32_t lo = c->v[i].lo, hi = c->v[i].hi;
        for (size_t k = 0; k < ARRAY_LEN(FOLDS); k++) {
            const fold_ent_t *e = &FOLDS[k];
            fold_iter(&add, lo, hi, e->lo, e->hi);
            if (!e->alt) fold_iter(&add, lo, hi, (int64_t)e->lo + e->delta, (int64_t)e->hi + e->delta);
        }
        for (size_t k = 0; k < ARRAY_LEN(FOLD_ORBITS); k++)
            for (int j = 0; j < 4 && FOLD_ORBITS[k][j]; j++)
                fold_iter(&add, lo, hi, FOLD_ORBITS[k][j], FOLD_ORBITS[k][j]);
    }
    cls_union(c, &add);
    vec_free(&add);
}

static bool cp_is_upper(uint32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z';
    for (size_t i = 0; i < ARRAY_LEN(FOLDS); i++) {
        const fold_ent_t *e = &FOLDS[i];
        if (c >= e->lo && c <= e->hi) return e->alt ? ((c - e->lo) & 1) == 0 : e->delta > 0;
        if (!e->alt && e->delta < 0) {
            /* negative delta: upper side is lo..hi still (0x178 -> 0xFF); lower side is not upper */
        }
    }
    return c == 0x1E9E || c == 0x2126 || c == 0x212A || c == 0x212B;
}

/* ======================================================================
 * Unicode tables (approximate)
 * ====================================================================== */
static const uint32_t UT_DIGIT[] = {
    0x30,   0x39,   0x660,  0x669,  0x6F0,  0x6F9,  0x7C0,  0x7C9,  0x966,  0x96F,  0x9E6,  0x9EF,
    0xA66,  0xA6F,  0xAE6,  0xAEF,  0xB66,  0xB6F,  0xBE6,  0xBEF,  0xC66,  0xC6F,  0xCE6,  0xCEF,
    0xD66,  0xD6F,  0xDE6,  0xDEF,  0xE50,  0xE59,  0xED0,  0xED9,  0xF20,  0xF29,  0x1040, 0x1049,
    0x1090, 0x1099, 0x17E0, 0x17E9, 0x1810, 0x1819, 0xFF10, 0xFF19, 0x1D7CE, 0x1D7FF,
};
static const uint32_t UT_SPACE[] = {
    0x09, 0x0D, 0x20, 0x20, 0x85, 0x85, 0xA0, 0xA0, 0x1680, 0x1680, 0x2000, 0x200A,
    0x2028, 0x2029, 0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000,
};
static const uint32_t UT_ZS[] = {
    0x20, 0x20, 0xA0, 0xA0, 0x1680, 0x1680, 0x2000, 0x200A, 0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000,
};
static const uint32_t UT_LETTER[] = {
    'A',     'Z',     'a',     'z',     0xAA,    0xAA,    0xB5,    0xB5,    0xBA,    0xBA,    0xC0,    0xD6,
    0xD8,    0xF6,    0xF8,    0x2C1,   0x2C6,   0x2D1,   0x2E0,   0x2E4,   0x2EC,   0x2EC,   0x2EE,   0x2EE,
    0x370,   0x374,   0x376,   0x377,   0x37A,   0x37D,   0x37F,   0x37F,   0x386,   0x386,   0x388,   0x3F5,
    0x3F7,   0x481,   0x48A,   0x52F,   0x531,   0x556,   0x559,   0x559,   0x560,   0x588,   0x5D0,   0x5EA,
    0x5EF,   0x5F2,   0x620,   0x64A,   0x66E,   0x66F,   0x671,   0x6D3,   0x6D5,   0x6D5,   0x6E5,   0x6E6,
    0x6EE,   0x6EF,   0x6FA,   0x6FC,   0x6FF,   0x6FF,   0x710,   0x710,   0x712,   0x72F,   0x74D,   0x7A5,
    0x7B1,   0x7B1,   0x7CA,   0x7EA,   0x800,   0x815,   0x840,   0x858,   0x8A0,   0x8C9,   0x904,   0x939,
    0x93D,   0x93D,   0x950,   0x950,   0x958,   0x961,   0x971,   0x980,   0x985,   0x9B9,   0x9BD,   0x9BD,
    0x9CE,   0x9CE,   0x9DC,   0x9E1,   0xA05,   0xA39,   0xA85,   0xAB9,   0xB05,   0xB39,   0xB85,   0xBB9,
    0xC05,   0xC39,   0xC85,   0xCB9,   0xD05,   0xD3A,   0xD85,   0xDC6,   0xE01,   0xE30,   0xE32,   0xE33,
    0xE40,   0xE46,   0xE81,   0xEB0,   0xEB2,   0xEB3,   0xEBD,   0xEC6,   0xF00,   0xF00,   0xF40,   0xF6C,
    0x1000,  0x102A,  0x10A0,  0x10C5,  0x10D0,  0x10FA,  0x10FC,  0x1248,  0x1250,  0x135A,  0x13A0,  0x13F5,
    0x13F8,  0x13FD,  0x1401,  0x166C,  0x166F,  0x167F,  0x1681,  0x169A,  0x16A0,  0x16EA,  0x1780,  0x17B3,
    0x1820,  0x1878,  0x1E00,  0x1F15,  0x1F18,  0x1F1D,  0x1F20,  0x1F45,  0x1F48,  0x1F4D,  0x1F50,  0x1F7D,
    0x1F80,  0x1FBC,  0x1FBE,  0x1FBE,  0x1FC2,  0x1FCC,  0x1FD0,  0x1FDB,  0x1FE0,  0x1FEC,  0x1FF2,  0x1FFC,
    0x2071,  0x2071,  0x207F,  0x207F,  0x2090,  0x209C,  0x2102,  0x2102,  0x2107,  0x2107,  0x210A,  0x2113,
    0x2115,  0x2115,  0x2119,  0x211D,  0x2124,  0x2124,  0x2126,  0x2126,  0x2128,  0x2128,  0x212A,  0x212D,
    0x212F,  0x2139,  0x213C,  0x213F,  0x2145,  0x2149,  0x214E,  0x214E,  0x2183,  0x2184,  0x2C00,  0x2CE4,
    0x2CEB,  0x2CEE,  0x2D00,  0x2D25,  0x2D30,  0x2D67,  0x2D80,  0x2DDE,  0x3005,  0x3006,  0x3031,  0x3035,
    0x303B,  0x303C,  0x3041,  0x3096,  0x309D,  0x309F,  0x30A1,  0x30FA,  0x30FC,  0x30FF,  0x3105,  0x312F,
    0x3131,  0x318E,  0x31A0,  0x31BF,  0x31F0,  0x31FF,  0x3400,  0x4DBF,  0x4E00,  0xA48C,  0xA4D0,  0xA4FD,
    0xA500,  0xA60C,  0xA610,  0xA61F,  0xA62A,  0xA62B,  0xA640,  0xA66E,  0xA67F,  0xA69D,  0xA6A0,  0xA6E5,
    0xA717,  0xA71F,  0xA722,  0xA788,  0xA78B,  0xA7CA,  0xA7F2,  0xA801,  0xAC00,  0xD7A3,  0xF900,  0xFA6D,
    0xFA70,  0xFAD9,  0xFB00,  0xFB06,  0xFB13,  0xFB17,  0xFB1D,  0xFB1D,  0xFB1F,  0xFB28,  0xFB2A,  0xFB4F,
    0xFB50,  0xFBB1,  0xFBD3,  0xFD3D,  0xFD50,  0xFDC7,  0xFDF0,  0xFDFB,  0xFE70,  0xFEFC,  0xFF21,  0xFF3A,
    0xFF41,  0xFF5A,  0xFF66,  0xFFDC,  0x10000, 0x100FA, 0x10280, 0x1031F, 0x10330, 0x10349, 0x10400, 0x1049D,
    0x1D400, 0x1D7CB, 0x1E900, 0x1E943, 0x20000, 0x2A6DF, 0x2A700, 0x2EBE0, 0x2F800, 0x2FA1D, 0x30000, 0x3134A,
};
static const uint32_t UT_MARK[] = {
    0x300,  0x36F,  0x483,  0x489,  0x591,  0x5BD,  0x5BF,  0x5BF,  0x5C1,  0x5C2,  0x5C4,  0x5C5,
    0x610,  0x61A,  0x64B,  0x65F,  0x670,  0x670,  0x6D6,  0x6DC,  0x6DF,  0x6E4,  0x900,  0x903,
    0x93A,  0x93C,  0x93E,  0x94F,  0x951,  0x957,  0x962,  0x963,  0x981,  0x983,  0x9BC,  0x9BC,
    0x9BE,  0x9CD,  0xE31,  0xE31,  0xE34,  0xE3A,  0xE47,  0xE4E,  0x102B, 0x103E, 0x1AB0, 0x1AFF,
    0x1DC0, 0x1DFF, 0x20D0, 0x20F0, 0x302A, 0x302F, 0x3099, 0x309A, 0xFE00, 0xFE0F, 0xFE20, 0xFE2F,
};
static const uint32_t UT_CONNECT[] = {'_', '_', 0x203F, 0x2040, 0x2054, 0x2054, 0xFE33, 0xFE34, 0xFE4D, 0xFE4F, 0xFF3F, 0xFF3F};
static const uint32_t UT_PUNCT[] = {
    0x21,   0x23,   0x25,   0x2A,   0x2C,   0x2F,   0x3A,   0x3B,   0x3F,   0x40,   0x5B,   0x5D,
    0x5F,   0x5F,   0x7B,   0x7B,   0x7D,   0x7D,   0xA1,   0xA1,   0xA7,   0xA7,   0xAB,   0xAB,
    0xB6,   0xB7,   0xBB,   0xBB,   0xBF,   0xBF,   0x37E,  0x37E,  0x387,  0x387,  0x55A,  0x55F,
    0x589,  0x58A,  0x5BE,  0x5BE,  0x60C,  0x60D,  0x61B,  0x61B,  0x61D,  0x61F,  0x66A,  0x66D,
    0x964,  0x965,  0x2010, 0x2027, 0x2030, 0x2043, 0x2045, 0x2051, 0x2053, 0x205E, 0x2308, 0x230B,
    0x2329, 0x232A, 0x2E00, 0x2E4F, 0x3001, 0x3003, 0x3008, 0x3011, 0x3014, 0x301F, 0xFE10, 0xFE19,
    0xFE30, 0xFE52, 0xFE54, 0xFE61, 0xFF01, 0xFF03, 0xFF05, 0xFF0A, 0xFF0C, 0xFF0F, 0xFF1A, 0xFF1B,
    0xFF1F, 0xFF20, 0xFF3B, 0xFF3D, 0xFF3F, 0xFF3F, 0xFF5B, 0xFF5B, 0xFF5D, 0xFF5D, 0xFF5F, 0xFF65,
};
static const uint32_t UT_SYMBOL[] = {
    0x24, 0x24, 0x2B, 0x2B, 0x3C, 0x3E, 0x5E, 0x5E, 0x60, 0x60, 0x7C, 0x7C, 0x7E, 0x7E, 0xA2, 0xA6,
    0xA8, 0xA9, 0xAC, 0xAC, 0xAE, 0xB1, 0xB4, 0xB4, 0xB8, 0xB8, 0xD7, 0xD7, 0xF7, 0xF7, 0x2044, 0x2044,
    0x20A0, 0x20C0, 0x2100, 0x214F, 0x2190, 0x2307, 0x230C, 0x2328, 0x232B, 0x23FF, 0x2500, 0x2767,
    0x2794, 0x27C4, 0x27C7, 0x27E5, 0x27F0, 0x2982, 0x2B00, 0x2BFF, 0x1F300, 0x1FAFF,
};

typedef struct {
    const char *name;
    const uint32_t *t;
    size_t n;
} script_t;
static const uint32_t SC_GREEK[] = {0x370, 0x373, 0x375, 0x377, 0x37A, 0x37D, 0x37F, 0x37F, 0x384, 0x3E1, 0x3F0, 0x3FF, 0x1D26, 0x1D2A, 0x1F00, 0x1FFE, 0x2126, 0x2126};
static const uint32_t SC_CYRILLIC[] = {0x400, 0x52F, 0x1C80, 0x1C88, 0x2DE0, 0x2DFF, 0xA640, 0xA69F};
static const uint32_t SC_LATIN[] = {'A', 'Z', 'a', 'z', 0xAA, 0xAA, 0xBA, 0xBA, 0xC0, 0xD6, 0xD8, 0xF6, 0xF8, 0x2B8, 0x1E00, 0x1EFF, 0x2C60, 0x2C7F, 0xA722, 0xA7FF, 0xFB00, 0xFB06, 0xFF21, 0xFF3A, 0xFF41, 0xFF5A};
static const uint32_t SC_HAN[] = {0x2E80, 0x2E99, 0x2E9B, 0x2EF3, 0x2F00, 0x2FD5, 0x3005, 0x3005, 0x3007, 0x3007, 0x3021, 0x3029, 0x3038, 0x303B, 0x3400, 0x4DBF, 0x4E00, 0x9FFF, 0xF900, 0xFAFF, 0x20000, 0x3134F};
static const uint32_t SC_HIRAGANA[] = {0x3041, 0x3096, 0x309D, 0x309F};
static const uint32_t SC_KATAKANA[] = {0x30A1, 0x30FA, 0x30FD, 0x30FF, 0x31F0, 0x31FF, 0x32D0, 0x32FE, 0x3300, 0x3357, 0xFF66, 0xFF6F, 0xFF71, 0xFF9D};
static const uint32_t SC_HANGUL[] = {0x1100, 0x11FF, 0x302E, 0x302F, 0x3131, 0x318E, 0x3200, 0x321E, 0xA960, 0xA97C, 0xAC00, 0xD7A3, 0xD7B0, 0xD7FB};
static const uint32_t SC_ARABIC[] = {0x600, 0x604, 0x606, 0x60B, 0x60D, 0x61A, 0x61C, 0x61E, 0x620, 0x63F, 0x641, 0x64A, 0x656, 0x66F, 0x671, 0x6DC, 0x6DE, 0x6FF, 0x750, 0x77F, 0x8A0, 0x8FF, 0xFB50, 0xFDFF, 0xFE70, 0xFEFC};
static const uint32_t SC_HEBREW[] = {0x591, 0x5C7, 0x5D0, 0x5EA, 0x5EF, 0x5F4, 0xFB1D, 0xFB4F};
static const uint32_t SC_THAI[] = {0xE01, 0xE3A, 0xE40, 0xE5B};
static const uint32_t SC_DEVANAGARI[] = {0x900, 0x950, 0x955, 0x963, 0x966, 0x97F, 0xA8E0, 0xA8FF};
static const uint32_t SC_EMOJI[] = {0x23, 0x23, 0x2A, 0x2A, 0x30, 0x39, 0xA9, 0xA9, 0xAE, 0xAE, 0x203C, 0x203C, 0x2049, 0x2049, 0x2122, 0x2122, 0x2139, 0x2139, 0x2194, 0x21AA, 0x231A, 0x23FF, 0x24C2, 0x24C2, 0x25AA, 0x27BF, 0x2934, 0x2935, 0x2B05, 0x2B55, 0x3030, 0x3030, 0x303D, 0x303D, 0x3297, 0x3299, 0x1F000, 0x1FAFF};

static const script_t SCRIPTS[] = {
    {"greek", SC_GREEK, ARRAY_LEN(SC_GREEK)},          {"cyrillic", SC_CYRILLIC, ARRAY_LEN(SC_CYRILLIC)},
    {"latin", SC_LATIN, ARRAY_LEN(SC_LATIN)},          {"han", SC_HAN, ARRAY_LEN(SC_HAN)},
    {"hiragana", SC_HIRAGANA, ARRAY_LEN(SC_HIRAGANA)}, {"katakana", SC_KATAKANA, ARRAY_LEN(SC_KATAKANA)},
    {"hangul", SC_HANGUL, ARRAY_LEN(SC_HANGUL)},       {"arabic", SC_ARABIC, ARRAY_LEN(SC_ARABIC)},
    {"hebrew", SC_HEBREW, ARRAY_LEN(SC_HEBREW)},       {"thai", SC_THAI, ARRAY_LEN(SC_THAI)},
    {"devanagari", SC_DEVANAGARI, ARRAY_LEN(SC_DEVANAGARI)},
    {"emoji", SC_EMOJI, ARRAY_LEN(SC_EMOJI)},
};

static void cls_perl_digit(cls_t *c) { cls_add_table(c, UT_DIGIT, ARRAY_LEN(UT_DIGIT)); }
static void cls_perl_space(cls_t *c) { cls_add_table(c, UT_SPACE, ARRAY_LEN(UT_SPACE)); }
static void cls_perl_word(cls_t *c) {
    cls_add_table(c, UT_LETTER, ARRAY_LEN(UT_LETTER));
    cls_add_table(c, UT_MARK, ARRAY_LEN(UT_MARK));
    cls_add_table(c, UT_DIGIT, ARRAY_LEN(UT_DIGIT));
    cls_add_table(c, UT_CONNECT, ARRAY_LEN(UT_CONNECT));
}

static void cls_case_side(cls_t *c, bool upper) {
    for (size_t i = 0; i < ARRAY_LEN(FOLDS); i++) {
        const fold_ent_t *e = &FOLDS[i];
        if (e->alt) {
            for (uint32_t x = e->lo + (upper ? 0 : 1); x <= e->hi; x += 2) cls_add(c, x, x);
        } else if (upper) {
            cls_add(c, e->lo, e->hi);
        } else {
            cls_add(c, (uint32_t)((int64_t)e->lo + e->delta), (uint32_t)((int64_t)e->hi + e->delta));
        }
    }
    if (upper) {
        cls_add(c, 0x1E9E, 0x1E9E);
    } else {
        cls_add(c, 0xDF, 0xDF);
        cls_add(c, 0xB5, 0xB5);
        cls_add(c, 0x17F, 0x17F);
        cls_add(c, 0x3C2, 0x3C2);
    }
    cls_norm(c);
}

/* Unicode property for \p{name}. Returns false if unknown. */
static bool unicode_prop(const char *raw, size_t rawlen, cls_t *c) {
    char buf[64];
    size_t n = 0;
    for (size_t i = 0; i < rawlen && n + 1 < sizeof(buf); i++) {
        char ch = raw[i];
        if (ch == ' ' || ch == '_' || ch == '-') continue;
        buf[n++] = (char)tolower((unsigned char)ch);
    }
    buf[n] = 0;
    const char *name = buf;
    const char *eq = strchr(buf, '=');
    if (!eq) eq = strchr(buf, ':');
    if (eq) name = eq + 1;
#define IS(s) (strcmp(name, s) == 0)
    if (IS("l") || IS("letter") || IS("alphabetic") || IS("alpha")) {
        cls_add_table(c, UT_LETTER, ARRAY_LEN(UT_LETTER));
    } else if (IS("lu") || IS("uppercaseletter") || IS("uppercase") || IS("upper")) {
        cls_case_side(c, true);
    } else if (IS("ll") || IS("lowercaseletter") || IS("lowercase") || IS("lower")) {
        cls_case_side(c, false);
    } else if (IS("n") || IS("number") || IS("nd") || IS("decimalnumber") || IS("digit")) {
        cls_perl_digit(c);
    } else if (IS("p") || IS("punctuation") || IS("punct")) {
        cls_add_table(c, UT_PUNCT, ARRAY_LEN(UT_PUNCT));
    } else if (IS("s") || IS("symbol")) {
        cls_add_table(c, UT_SYMBOL, ARRAY_LEN(UT_SYMBOL));
    } else if (IS("z") || IS("separator") || IS("zs") || IS("spaceseparator")) {
        cls_add_table(c, UT_ZS, ARRAY_LEN(UT_ZS));
        if (IS("z") || IS("separator")) cls_add(c, 0x2028, 0x2029);
    } else if (IS("whitespace") || IS("space") || IS("wspace")) {
        cls_perl_space(c);
    } else if (IS("m") || IS("mark") || IS("mn") || IS("combiningmark")) {
        cls_add_table(c, UT_MARK, ARRAY_LEN(UT_MARK));
    } else if (IS("c") || IS("cc") || IS("control") || IS("other")) {
        cls_add(c, 0, 0x1F);
        cls_add(c, 0x7F, 0x9F);
    } else if (IS("any")) {
        cls_add(c, 0, CP_MAX);
    } else if (IS("ascii")) {
        cls_add(c, 0, 0x7F);
    } else if (IS("word") || IS("w")) {
        cls_perl_word(c);
    } else {
        bool found = false;
        for (size_t i = 0; i < ARRAY_LEN(SCRIPTS); i++) {
            if (IS(SCRIPTS[i].name) || (strcmp(SCRIPTS[i].name, "emoji") == 0 && IS("extendedpictographic"))) {
                cls_add_table(c, SCRIPTS[i].t, SCRIPTS[i].n);
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
#undef IS
    cls_norm(c);
    return true;
}

static bool posix_class(const char *name, size_t n, cls_t *c) {
#define PN(s) (n == strlen(s) && memcmp(name, s, n) == 0)
    if (PN("alnum")) { cls_add(c, '0', '9'); cls_add(c, 'A', 'Z'); cls_add(c, 'a', 'z'); }
    else if (PN("alpha")) { cls_add(c, 'A', 'Z'); cls_add(c, 'a', 'z'); }
    else if (PN("ascii")) cls_add(c, 0, 0x7F);
    else if (PN("blank")) { cls_add(c, '\t', '\t'); cls_add(c, ' ', ' '); }
    else if (PN("cntrl")) { cls_add(c, 0, 0x1F); cls_add(c, 0x7F, 0x7F); }
    else if (PN("digit")) cls_add(c, '0', '9');
    else if (PN("graph")) cls_add(c, '!', '~');
    else if (PN("lower")) cls_add(c, 'a', 'z');
    else if (PN("print")) cls_add(c, ' ', '~');
    else if (PN("punct")) { cls_add(c, '!', '/'); cls_add(c, ':', '@'); cls_add(c, '[', '`'); cls_add(c, '{', '~'); }
    else if (PN("space")) { cls_add(c, '\t', '\r'); cls_add(c, ' ', ' '); }
    else if (PN("upper")) cls_add(c, 'A', 'Z');
    else if (PN("word")) { cls_add(c, '0', '9'); cls_add(c, 'A', 'Z'); cls_add(c, 'a', 'z'); cls_add(c, '_', '_'); }
    else if (PN("xdigit")) { cls_add(c, '0', '9'); cls_add(c, 'A', 'F'); cls_add(c, 'a', 'f'); }
    else return false;
#undef PN
    cls_norm(c);
    return true;
}

/* ======================================================================
 * AST + parser
 * ====================================================================== */
enum { NK_EMPTY, NK_LIT, NK_CLASS, NK_CAT, NK_ALT, NK_REP, NK_GROUP, NK_ASSERT };
enum {
    A_TEXT_START, A_TEXT_END, A_LINE_START, A_LINE_END, A_WORDB, A_NWORDB,
    A_WORD_START, A_WORD_END, A_WORD_START_HALF, A_WORD_END_HALF,
};
enum { PF_I = 1, PF_M = 2, PF_S = 4, PF_X = 8, PF_U = 16 };

typedef struct node node_t;
struct node {
    int kind;
    uint32_t cp;
    bool fold;
    cls_t cls;
    VEC(node_t *) kids;
    node_t *sub;
    int min, max;
    bool greedy;
    int cap;
    int akind;
};

typedef struct {
    const char *pat;
    size_t len, pos;
    int flags;
    bool line;
    int ncap;
    VEC(char *) names; /* names.v[i] = name of group i (index 0 unused) */
    char *err;
    bool unsupported;
    int depth;
} parser_t;

static node_t *nnew(int kind) {
    node_t *n = xcalloc(1, sizeof(*n));
    n->kind = kind;
    n->cap = -1;
    return n;
}

static void nfree(node_t *n) {
    if (!n) return;
    vec_free(&n->cls);
    for (size_t i = 0; i < n->kids.n; i++) nfree(n->kids.v[i]);
    vec_free(&n->kids);
    nfree(n->sub);
    free(n);
}

static void perr(parser_t *ps, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void perr(parser_t *ps, const char *fmt, ...) {
    if (ps->err) return;
    va_list ap;
    va_start(ap, fmt);
    char *m = xvasprintf(fmt, ap);
    va_end(ap);
    ps->err = xasprintf("regex parse error at position %zu: %s", ps->pos, m);
    free(m);
}

static uint32_t pdecode(parser_t *ps, int *nb) {
    uint32_t cp;
    *nb = utf8_decode(ps->pat + ps->pos, ps->len - ps->pos, &cp);
    if (*nb < 1) *nb = 1;
    return cp;
}

static void skip_x(parser_t *ps) {
    if (!(ps->flags & PF_X)) return;
    while (ps->pos < ps->len) {
        char c = ps->pat[ps->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            ps->pos++;
        } else if (c == '#') {
            while (ps->pos < ps->len && ps->pat[ps->pos] != '\n') ps->pos++;
        } else {
            break;
        }
    }
}

static node_t *mk_class(parser_t *ps, cls_t *cls, bool already_folded) {
    node_t *n = nnew(NK_CLASS);
    n->cls = *cls;
    memset(cls, 0, sizeof(*cls));
    cls_norm(&n->cls);
    if ((ps->flags & PF_I) && !already_folded) cls_fold(&n->cls);
    if (ps->line) cls_remove(&n->cls, '\n');
    return n;
}

static node_t *mk_lit(parser_t *ps, uint32_t cp) {
    if (ps->line && cp == '\n') {
        cls_t empty = {0};
        return mk_class(ps, &empty, true);
    }
    node_t *n = nnew(NK_LIT);
    n->cp = cp;
    if (ps->flags & PF_I) {
        uint32_t o[8];
        n->fold = fold_orbit(cp, o) > 1;
    }
    return n;
}

static node_t *mk_assert(int kind) {
    node_t *n = nnew(NK_ASSERT);
    n->akind = kind;
    return n;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

enum { E_ERR, E_CHAR, E_CLASS, E_ASSERT };

static bool parse_hex(parser_t *ps, int fixed, uint32_t *out) {
    uint32_t v = 0;
    if (ps->pos < ps->len && ps->pat[ps->pos] == '{') {
        ps->pos++;
        int nd = 0;
        while (ps->pos < ps->len && ps->pat[ps->pos] != '}') {
            int h = hexval(ps->pat[ps->pos]);
            if (h < 0 || nd >= 8) { perr(ps, "invalid hexadecimal escape"); return false; }
            v = v * 16 + (uint32_t)h;
            nd++;
            ps->pos++;
        }
        if (ps->pos >= ps->len || nd == 0) { perr(ps, "invalid hexadecimal escape"); return false; }
        ps->pos++;
    } else {
        for (int i = 0; i < fixed; i++) {
            int h = ps->pos < ps->len ? hexval(ps->pat[ps->pos]) : -1;
            if (h < 0) { perr(ps, "invalid hexadecimal escape (expected %d digits)", fixed); return false; }
            v = v * 16 + (uint32_t)h;
            ps->pos++;
        }
    }
    if (v > CP_MAX || (v >= 0xD800 && v <= 0xDFFF)) { perr(ps, "invalid Unicode scalar value"); return false; }
    *out = v;
    return true;
}

/* Parse an escape at ps->pos ('\\'). */
static int parse_esc(parser_t *ps, uint32_t *cp, cls_t *cls, int *akind) {
    ps->pos++;
    if (ps->pos >= ps->len) { perr(ps, "incomplete escape sequence"); return E_ERR; }
    int nb;
    uint32_t ch = pdecode(ps, &nb);
    ps->pos += nb;
    switch (ch) {
    case 'n': *cp = '\n'; return E_CHAR;
    case 't': *cp = '\t'; return E_CHAR;
    case 'r': *cp = '\r'; return E_CHAR;
    case 'f': *cp = '\f'; return E_CHAR;
    case 'v': *cp = '\v'; return E_CHAR;
    case 'a': *cp = 7; return E_CHAR;
    case 'e': *cp = 27; return E_CHAR;
    case '0': *cp = 0; return E_CHAR;
    case 'x': return parse_hex(ps, 2, cp) ? E_CHAR : E_ERR;
    case 'u': return parse_hex(ps, 4, cp) ? E_CHAR : E_ERR;
    case 'U': return parse_hex(ps, 8, cp) ? E_CHAR : E_ERR;
    case 'd': case 'D': cls_perl_digit(cls); if (ch == 'D') cls_negate(cls); return E_CLASS;
    case 's': case 'S': cls_perl_space(cls); if (ch == 'S') cls_negate(cls); return E_CLASS;
    case 'w': case 'W': cls_perl_word(cls); cls_norm(cls); if (ch == 'W') cls_negate(cls); return E_CLASS;
    case 'p': case 'P': {
        const char *name;
        size_t nlen;
        if (ps->pos >= ps->len) { perr(ps, "incomplete Unicode class"); return E_ERR; }
        if (ps->pat[ps->pos] == '{') {
            const char *end = memchr(ps->pat + ps->pos, '}', ps->len - ps->pos);
            if (!end) { perr(ps, "unclosed Unicode class"); return E_ERR; }
            name = ps->pat + ps->pos + 1;
            nlen = (size_t)(end - name);
            ps->pos = (size_t)(end - ps->pat) + 1;
        } else {
            name = ps->pat + ps->pos;
            nlen = 1;
            ps->pos++;
        }
        bool neg = ch == 'P';
        if (nlen > 0 && name[0] == '^') { neg = !neg; name++; nlen--; }
        if (!unicode_prop(name, nlen, cls)) { perr(ps, "unrecognized Unicode class '%.*s'", (int)nlen, name); return E_ERR; }
        if (neg) cls_negate(cls);
        return E_CLASS;
    }
    case 'b':
        if (ps->pos < ps->len && ps->pat[ps->pos] == '{') {
            static const struct { const char *s; int k; } bk[] = {
                {"{start}", A_WORD_START}, {"{end}", A_WORD_END},
                {"{start-half}", A_WORD_START_HALF}, {"{end-half}", A_WORD_END_HALF},
            };
            for (size_t i = 0; i < ARRAY_LEN(bk); i++) {
                size_t l = strlen(bk[i].s);
                if (ps->len - ps->pos >= l && memcmp(ps->pat + ps->pos, bk[i].s, l) == 0) {
                    ps->pos += l;
                    *akind = bk[i].k;
                    return E_ASSERT;
                }
            }
        }
        *akind = A_WORDB;
        return E_ASSERT;
    case 'B': *akind = A_NWORDB; return E_ASSERT;
    case 'A': *akind = A_TEXT_START; return E_ASSERT;
    case 'z': *akind = A_TEXT_END; return E_ASSERT;
    case '<': *akind = A_WORD_START; return E_ASSERT;
    case '>': *akind = A_WORD_END; return E_ASSERT;
    case 'k':
        ps->unsupported = true;
        perr(ps, "backreferences are not supported");
        return E_ERR;
    default:
        if (ch >= '1' && ch <= '9') {
            ps->unsupported = true;
            perr(ps, "backreferences are not supported");
            return E_ERR;
        }
        if (ch < 0x80 && isalnum((int)ch)) {
            perr(ps, "unrecognized escape sequence '\\%c'", (char)ch);
            return E_ERR;
        }
        *cp = ch;
        return E_CHAR;
    }
}

static void bracket_combine(cls_t *res, cls_t *acc, int op) {
    cls_norm(acc);
    switch (op) {
    case 0: cls_union(res, acc); break;
    case '&': cls_intersect(res, acc); break;
    case '-': cls_diff(res, acc); break;
    case '~': cls_symdiff(res, acc); break;
    }
    acc->n = 0;
}

/* Parse a bracket class; ps->pos is just after '['. Result folded (if (?i)) and negated. */
static bool parse_bracket(parser_t *ps, cls_t *out, int depth) {
    if (depth > RX_MAX_NEST) { perr(ps, "class nesting limit exceeded"); return false; }
    bool neg = false;
    if (ps->pos < ps->len && ps->pat[ps->pos] == '^') { neg = true; ps->pos++; }
    cls_t res = {0}, acc = {0};
    int op = 0;
    bool first = true;
    for (;;) {
        skip_x(ps);
        if (ps->pos >= ps->len) { perr(ps, "unclosed character class"); goto fail; }
        char c = ps->pat[ps->pos];
        if (c == ']' && !first) { ps->pos++; break; }
        first = false;
        if (ps->pos + 1 < ps->len && (c == '&' || c == '-' || c == '~') && ps->pat[ps->pos + 1] == c) {
            if (ps->flags & PF_I) cls_fold(&acc);
            bracket_combine(&res, &acc, op);
            op = c;
            ps->pos += 2;
            continue;
        }
        if (c == '[') {
            if (ps->pos + 1 < ps->len && ps->pat[ps->pos + 1] == ':') {
                const char *start = ps->pat + ps->pos + 2;
                const char *end = NULL;
                for (const char *q = start; q + 1 < ps->pat + ps->len; q++) {
                    if (q[0] == ':' && q[1] == ']') { end = q; break; }
                    if (!isalpha((unsigned char)*q) && *q != '^') break;
                }
                if (end) {
                    bool pneg = start < end && *start == '^';
                    cls_t pc = {0};
                    if (posix_class(start + pneg, (size_t)(end - start - pneg), &pc)) {
                        if (pneg) cls_negate(&pc);
                        cls_union(&acc, &pc);
                        vec_free(&pc);
                        ps->pos = (size_t)(end - ps->pat) + 2;
                        continue;
                    }
                    vec_free(&pc);
                    perr(ps, "unrecognized POSIX class");
                    goto fail;
                }
            }
            ps->pos++;
            cls_t sub = {0};
            if (!parse_bracket(ps, &sub, depth + 1)) { vec_free(&sub); goto fail; }
            cls_union(&acc, &sub);
            vec_free(&sub);
            continue;
        }
        uint32_t lo;
        if (c == '\\') {
            cls_t ec = {0};
            int ak;
            int t = parse_esc(ps, &lo, &ec, &ak);
            if (t == E_ERR) { vec_free(&ec); goto fail; }
            if (t == E_ASSERT) { perr(ps, "assertions are not allowed in a character class"); vec_free(&ec); goto fail; }
            if (t == E_CLASS) { cls_union(&acc, &ec); vec_free(&ec); continue; }
        } else {
            int nb;
            lo = pdecode(ps, &nb);
            ps->pos += nb;
        }
        if (ps->pos + 1 < ps->len && ps->pat[ps->pos] == '-' && ps->pat[ps->pos + 1] != ']' &&
            ps->pat[ps->pos + 1] != '-') {
            ps->pos++;
            uint32_t hi;
            if (ps->pat[ps->pos] == '\\') {
                cls_t ec = {0};
                int ak;
                int t = parse_esc(ps, &hi, &ec, &ak);
                vec_free(&ec);
                if (t == E_ERR) goto fail;
                if (t != E_CHAR) { perr(ps, "invalid range boundary"); goto fail; }
            } else {
                int nb;
                hi = pdecode(ps, &nb);
                ps->pos += nb;
            }
            if (hi < lo) { perr(ps, "invalid character class range"); goto fail; }
            cls_add(&acc, lo, hi);
        } else {
            cls_add(&acc, lo, lo);
        }
    }
    if (ps->flags & PF_I) cls_fold(&acc);
    bracket_combine(&res, &acc, op);
    vec_free(&acc);
    cls_norm(&res);
    if (neg) cls_negate(&res);
    *out = res;
    return true;
fail:
    vec_free(&acc);
    vec_free(&res);
    return false;
}

static node_t *parse_alt(parser_t *ps);

static bool valid_group_name(const char *s, size_t n) {
    if (n == 0 || isdigit((unsigned char)s[0])) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '.' || c == '[' || c == ']')) return false;
    }
    return true;
}

static node_t *parse_group(parser_t *ps, bool *flag_only) {
    ps->pos++; /* '(' */
    int saved = ps->flags;
    int cap = -1;
    char *name = NULL;
    if (ps->pos < ps->len && ps->pat[ps->pos] == '?') {
        ps->pos++;
        const char *p = ps->pat + ps->pos;
        size_t rem = ps->len - ps->pos;
        if ((rem >= 2 && p[0] == 'P' && p[1] == '<') || (rem >= 2 && p[0] == '<' && p[1] != '=' && p[1] != '!')) {
            size_t off = p[0] == 'P' ? 2 : 1;
            const char *gt = memchr(p + off, '>', rem - off);
            if (!gt) { perr(ps, "unclosed capture group name"); return NULL; }
            size_t nlen = (size_t)(gt - (p + off));
            if (!valid_group_name(p + off, nlen)) { perr(ps, "invalid capture group name"); return NULL; }
            name = xstrndup(p + off, nlen);
            for (size_t i = 1; i < ps->names.n; i++) {
                if (ps->names.v[i] && strcmp(ps->names.v[i], name) == 0) {
                    perr(ps, "duplicate capture group name '%s'", name);
                    free(name);
                    return NULL;
                }
            }
            ps->pos = (size_t)(gt - ps->pat) + 1;
            cap = ++ps->ncap;
            vec_push(&ps->names, name);
        } else if (rem >= 2 && p[0] == 'P' && p[1] == '=') {
            ps->unsupported = true;
            perr(ps, "backreferences are not supported");
            return NULL;
        } else if (rem >= 1 && (p[0] == '=' || p[0] == '!' || (p[0] == '<' && rem >= 2))) {
            ps->unsupported = true;
            perr(ps, "look-around, including look-ahead and look-behind, is not supported");
            return NULL;
        } else {
            int f = ps->flags;
            bool neg = false;
            while (ps->pos < ps->len) {
                char ch = ps->pat[ps->pos];
                if (ch == ':' || ch == ')') break;
                int bit;
                switch (ch) {
                case '-':
                    if (neg) { perr(ps, "repeated negation in flags"); return NULL; }
                    neg = true;
                    ps->pos++;
                    continue;
                case 'i': bit = PF_I; break;
                case 'm': bit = PF_M; break;
                case 's': bit = PF_S; break;
                case 'x': bit = PF_X; break;
                case 'U': bit = PF_U; break;
                case 'u': case 'R': bit = 0; break;
                default: perr(ps, "unrecognized flag '%c'", ch); return NULL;
                }
                if (neg) f &= ~bit;
                else f |= bit;
                ps->pos++;
            }
            if (ps->pos >= ps->len) { perr(ps, "unclosed group"); return NULL; }
            if (ps->pat[ps->pos] == ')') {
                ps->pos++;
                ps->flags = f;
                *flag_only = true;
                return NULL;
            }
            ps->pos++; /* ':' */
            ps->flags = f;
        }
    } else {
        cap = ++ps->ncap;
        vec_push(&ps->names, NULL);
    }
    if (++ps->depth > RX_MAX_NEST) { perr(ps, "exceeds group nesting limit"); ps->flags = saved; return NULL; }
    node_t *sub = parse_alt(ps);
    ps->depth--;
    ps->flags = saved;
    if (!sub) return NULL;
    if (ps->pos >= ps->len || ps->pat[ps->pos] != ')') {
        perr(ps, "unclosed group");
        nfree(sub);
        return NULL;
    }
    ps->pos++;
    node_t *g = nnew(NK_GROUP);
    g->sub = sub;
    g->cap = cap;
    return g;
}

static node_t *parse_atom(parser_t *ps, bool *flag_only) {
    char c = ps->pat[ps->pos];
    switch (c) {
    case '(':
        return parse_group(ps, flag_only);
    case '[': {
        ps->pos++;
        cls_t cls = {0};
        if (!parse_bracket(ps, &cls, 0)) { vec_free(&cls); return NULL; }
        return mk_class(ps, &cls, true);
    }
    case '.': {
        ps->pos++;
        cls_t cls = {0};
        if (ps->flags & PF_S) cls_add(&cls, 0, CP_MAX);
        else { cls_add(&cls, 0, 9); cls_add(&cls, 11, CP_MAX); }
        return mk_class(ps, &cls, true);
    }
    case '^':
        ps->pos++;
        return mk_assert((ps->flags & PF_M) ? A_LINE_START : A_TEXT_START);
    case '$':
        ps->pos++;
        return mk_assert((ps->flags & PF_M) ? A_LINE_END : A_TEXT_END);
    case '\\': {
        uint32_t cp = 0;
        cls_t cls = {0};
        int ak = 0;
        int t = parse_esc(ps, &cp, &cls, &ak);
        if (t == E_ERR) { vec_free(&cls); return NULL; }
        if (t == E_CLASS) return mk_class(ps, &cls, false);
        vec_free(&cls);
        if (t == E_ASSERT) return mk_assert(ak);
        return mk_lit(ps, cp);
    }
    case '*': case '+': case '?':
        perr(ps, "repetition operator missing expression");
        return NULL;
    default: {
        int nb;
        uint32_t cp = pdecode(ps, &nb);
        ps->pos += nb;
        return mk_lit(ps, cp);
    }
    }
}

/* {n}, {n,}, {n,m}; returns 1 if parsed, 0 if not a counted repetition (literal '{'), -1 error. */
static int parse_counted(parser_t *ps, int *mn, int *mx) {
    size_t i = ps->pos + 1;
    const char *p = ps->pat;
    while (i < ps->len && p[i] == ' ') i++;
    if (i >= ps->len || !isdigit((unsigned char)p[i])) return 0;
    long n = 0, m;
    while (i < ps->len && isdigit((unsigned char)p[i])) { if (n < 1000000) n = n * 10 + (p[i] - '0'); i++; }
    while (i < ps->len && p[i] == ' ') i++;
    if (i >= ps->len) return 0;
    if (p[i] == '}') {
        m = n;
    } else if (p[i] == ',') {
        i++;
        while (i < ps->len && p[i] == ' ') i++;
        if (i < ps->len && isdigit((unsigned char)p[i])) {
            m = 0;
            while (i < ps->len && isdigit((unsigned char)p[i])) { if (m < 1000000) m = m * 10 + (p[i] - '0'); i++; }
        } else {
            m = -1;
        }
        while (i < ps->len && p[i] == ' ') i++;
        if (i >= ps->len || p[i] != '}') return 0;
    } else {
        return 0;
    }
    ps->pos = i + 1;
    if (n > RX_MAX_REPEAT || m > RX_MAX_REPEAT) { perr(ps, "repetition count exceeds limit of %d", RX_MAX_REPEAT); return -1; }
    if (m >= 0 && n > m) { perr(ps, "invalid repetition count range (min > max)"); return -1; }
    *mn = (int)n;
    *mx = (int)m;
    return 1;
}

static node_t *parse_concat(parser_t *ps) {
    node_t *cat = nnew(NK_CAT);
    for (;;) {
        skip_x(ps);
        if (ps->pos >= ps->len) break;
        char c = ps->pat[ps->pos];
        if (c == '|' || c == ')') break;
        bool flag_only = false;
        node_t *atom = parse_atom(ps, &flag_only);
        if (ps->err) { nfree(atom); nfree(cat); return NULL; }
        if (flag_only) continue;
        if (!atom) { perr(ps, "invalid expression"); nfree(cat); return NULL; }
        int reps = 0;
        for (;;) {
            skip_x(ps);
            if (ps->pos >= ps->len) break;
            char q = ps->pat[ps->pos];
            int mn, mx;
            if (q == '*') { mn = 0; mx = -1; ps->pos++; }
            else if (q == '+') { mn = 1; mx = -1; ps->pos++; }
            else if (q == '?') { mn = 0; mx = 1; ps->pos++; }
            else if (q == '{') {
                int r = parse_counted(ps, &mn, &mx);
                if (r < 0) { nfree(atom); nfree(cat); return NULL; }
                if (r == 0) break;
            } else break;
            bool greedy = true;
            if (ps->pos < ps->len && ps->pat[ps->pos] == '?') { greedy = false; ps->pos++; }
            if (ps->flags & PF_U) greedy = !greedy;
            if (++reps > 64) { perr(ps, "too many stacked repetition operators"); nfree(atom); nfree(cat); return NULL; }
            node_t *r = nnew(NK_REP);
            r->sub = atom;
            r->min = mn;
            r->max = mx;
            r->greedy = greedy;
            atom = r;
        }
        vec_push(&cat->kids, atom);
    }
    if (cat->kids.n == 1) {
        node_t *k = cat->kids.v[0];
        cat->kids.n = 0;
        nfree(cat);
        return k;
    }
    if (cat->kids.n == 0) cat->kind = NK_EMPTY;
    return cat;
}

static node_t *parse_alt(parser_t *ps) {
    node_t *first = parse_concat(ps);
    if (!first) return NULL;
    if (ps->pos >= ps->len || ps->pat[ps->pos] != '|') return first;
    node_t *alt = nnew(NK_ALT);
    vec_push(&alt->kids, first);
    while (ps->pos < ps->len && ps->pat[ps->pos] == '|') {
        ps->pos++;
        node_t *k = parse_concat(ps);
        if (!k) { nfree(alt); return NULL; }
        vec_push(&alt->kids, k);
    }
    return alt;
}

/* ======================================================================
 * Program / compiler
 * ====================================================================== */
enum { OP_RANGE, OP_SET, OP_SPLIT, OP_JMP, OP_SAVE, OP_ASSERT, OP_MATCH };

typedef struct {
    uint8_t op, lo, hi, kind;
    int32_t out, out1; /* SET: out1 = set index; SAVE: out1 = slot */
} inst_t;

typedef struct {
    uint64_t w[4];
} bset_t;

typedef struct {
    VEC(inst_t) code;
    VEC(bset_t) sets;
    int start;
} prog_t;

typedef struct {
    prog_t *p;
    bool rev;
    bool overflow;
} comp_t;

static int emit(comp_t *c, int op) {
    inst_t in = {0};
    in.op = (uint8_t)op;
    in.out = (int32_t)c->p->code.n + 1;
    vec_push(&c->p->code, in);
    if (c->p->code.n > RX_MAX_INSTS) c->overflow = true;
    return (int)c->p->code.n - 1;
}

static inline bool bset_has(const bset_t *b, uint8_t x) { return (b->w[x >> 6] >> (x & 63)) & 1; }
static inline void bset_add(bset_t *b, uint8_t x) { b->w[x >> 6] |= (uint64_t)1 << (x & 63); }

static int add_set(comp_t *c, const bset_t *b) {
    for (size_t i = 0; i < c->p->sets.n; i++)
        if (memcmp(&c->p->sets.v[i], b, sizeof(*b)) == 0) return (int)i;
    vec_push(&c->p->sets, *b);
    return (int)c->p->sets.n - 1;
}

typedef struct {
    uint8_t n, lo[4], hi[4];
} useq_t;
typedef VEC(useq_t) useqs_t;

static int enc_utf8(uint32_t cp, uint8_t *o) {
    if (cp < 0x80) { o[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { o[0] = (uint8_t)(0xC0 | (cp >> 6)); o[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (uint8_t)(0xE0 | (cp >> 12)); o[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (uint8_t)(0xF0 | (cp >> 18)); o[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

static void utf8_seqs(uint32_t lo, uint32_t hi, useqs_t *out) {
    if (lo > hi) return;
    if (lo <= 0xDFFF && hi >= 0xD800) {
        if (lo < 0xD800) utf8_seqs(lo, 0xD7FF, out);
        if (hi > 0xDFFF) utf8_seqs(0xE000, hi, out);
        return;
    }
    static const uint32_t maxes[] = {0x7F, 0x7FF, 0xFFFF};
    for (int i = 0; i < 3; i++) {
        if (lo <= maxes[i] && hi > maxes[i]) {
            utf8_seqs(lo, maxes[i], out);
            utf8_seqs(maxes[i] + 1, hi, out);
            return;
        }
    }
    if (hi <= 0x7F) {
        useq_t s = {1, {(uint8_t)lo}, {(uint8_t)hi}};
        vec_push(out, s);
        return;
    }
    for (int i = 1; i < 4; i++) {
        uint32_t mask = (1u << (6 * i)) - 1;
        if ((lo & ~mask) != (hi & ~mask)) {
            if ((lo & mask) != 0) {
                utf8_seqs(lo, lo | mask, out);
                utf8_seqs((lo | mask) + 1, hi, out);
                return;
            }
            if ((hi & mask) != mask) {
                utf8_seqs(lo, (hi & ~mask) - 1, out);
                utf8_seqs(hi & ~mask, hi, out);
                return;
            }
        }
    }
    uint8_t a[4], b[4];
    int n = enc_utf8(lo, a);
    enc_utf8(hi, b);
    useq_t s;
    memset(&s, 0, sizeof(s));
    s.n = (uint8_t)n;
    for (int i = 0; i < n; i++) { s.lo[i] = a[i]; s.hi[i] = b[i]; }
    vec_push(out, s);
}

typedef struct {
    uint8_t lo, hi;
    int first, next; /* child list */
} tnode_t;
typedef VEC(tnode_t) trie_t;

static int trie_child(trie_t *t, int parent, uint8_t lo, uint8_t hi) {
    int last = -1;
    for (int k = t->v[parent].first; k >= 0; k = t->v[k].next) {
        if (t->v[k].lo == lo && t->v[k].hi == hi) return k;
        last = k;
    }
    tnode_t nn = {lo, hi, -1, -1};
    vec_push(t, nn);
    int id = (int)t->n - 1;
    if (last < 0) t->v[parent].first = id;
    else t->v[last].next = id;
    return id;
}

typedef VEC(int) intvec_t;

static void emit_trie(comp_t *c, trie_t *t, int node, intvec_t *ends) {
    for (int k = t->v[node].first; k >= 0; k = t->v[k].next) {
        bool last = t->v[k].next < 0;
        int sp = -1;
        if (!last) sp = emit(c, OP_SPLIT);
        int r = emit(c, OP_RANGE);
        c->p->code.v[r].lo = t->v[k].lo;
        c->p->code.v[r].hi = t->v[k].hi;
        if (t->v[k].first >= 0) {
            emit_trie(c, t, k, ends);
        } else {
            int j = emit(c, OP_JMP);
            vec_push(ends, j);
        }
        if (sp >= 0) c->p->code.v[sp].out1 = (int32_t)c->p->code.n;
    }
}

static void compile_class(comp_t *c, const cls_t *cls) {
    bset_t single;
    memset(&single, 0, sizeof(single));
    bool any_single = false;
    useqs_t seqs = {0};
    for (size_t i = 0; i < cls->n; i++) utf8_seqs(cls->v[i].lo, cls->v[i].hi, &seqs);
    trie_t t = {0};
    tnode_t root = {0, 0, -1, -1};
    vec_push(&t, root);
    for (size_t i = 0; i < seqs.n; i++) {
        useq_t *s = &seqs.v[i];
        if (s->n == 1) {
            for (unsigned b = s->lo[0]; b <= s->hi[0]; b++) bset_add(&single, (uint8_t)b);
            any_single = true;
            continue;
        }
        int cur = 0;
        for (int k = 0; k < s->n; k++) {
            int idx = c->rev ? s->n - 1 - k : k;
            cur = trie_child(&t, cur, s->lo[idx], s->hi[idx]);
        }
    }
    bool any_multi = t.v[0].first >= 0;
    intvec_t ends = {0};
    if (!any_multi || !any_single) {
        if (!any_multi) {
            int r = emit(c, OP_SET);
            c->p->code.v[r].out1 = add_set(c, &single);
        } else {
            emit_trie(c, &t, 0, &ends);
        }
    } else {
        int sp = emit(c, OP_SPLIT);
        int r = emit(c, OP_SET);
        c->p->code.v[r].out1 = add_set(c, &single);
        int j = emit(c, OP_JMP);
        vec_push(&ends, j);
        c->p->code.v[sp].out1 = (int32_t)c->p->code.n;
        emit_trie(c, &t, 0, &ends);
    }
    for (size_t i = 0; i < ends.n; i++) c->p->code.v[ends.v[i]].out = (int32_t)c->p->code.n;
    vec_free(&ends);
    vec_free(&t);
    vec_free(&seqs);
}

static int mirror_assert(int k) {
    switch (k) {
    case A_TEXT_START: return A_TEXT_END;
    case A_TEXT_END: return A_TEXT_START;
    case A_LINE_START: return A_LINE_END;
    case A_LINE_END: return A_LINE_START;
    case A_WORD_START: return A_WORD_END;
    case A_WORD_END: return A_WORD_START;
    case A_WORD_START_HALF: return A_WORD_END_HALF;
    case A_WORD_END_HALF: return A_WORD_START_HALF;
    default: return k;
    }
}

static void compile_node(comp_t *c, const node_t *n, int depth);

static void compile_rep(comp_t *c, const node_t *n, int depth) {
    const node_t *s = n->sub;
    if (n->max < 0) {
        if (n->min == 0) {
            int sp = emit(c, OP_SPLIT);
            compile_node(c, s, depth + 1);
            int j = emit(c, OP_JMP);
            c->p->code.v[j].out = sp;
            int end = (int)c->p->code.n;
            if (n->greedy) { c->p->code.v[sp].out = sp + 1; c->p->code.v[sp].out1 = end; }
            else { c->p->code.v[sp].out = end; c->p->code.v[sp].out1 = sp + 1; }
        } else {
            for (int i = 0; i < n->min - 1 && !c->overflow; i++) compile_node(c, s, depth + 1);
            int l1 = (int)c->p->code.n;
            compile_node(c, s, depth + 1);
            int sp = emit(c, OP_SPLIT);
            if (n->greedy) { c->p->code.v[sp].out = l1; c->p->code.v[sp].out1 = sp + 1; }
            else { c->p->code.v[sp].out = sp + 1; c->p->code.v[sp].out1 = l1; }
        }
        return;
    }
    for (int i = 0; i < n->min && !c->overflow; i++) compile_node(c, s, depth + 1);
    intvec_t holes = {0};
    for (int i = n->min; i < n->max && !c->overflow; i++) {
        int sp = emit(c, OP_SPLIT);
        vec_push(&holes, sp);
        compile_node(c, s, depth + 1);
    }
    int end = (int)c->p->code.n;
    for (size_t i = 0; i < holes.n; i++) {
        inst_t *in = &c->p->code.v[holes.v[i]];
        if (n->greedy) { in->out = holes.v[i] + 1; in->out1 = end; }
        else { in->out = end; in->out1 = holes.v[i] + 1; }
    }
    vec_free(&holes);
}

static void compile_node(comp_t *c, const node_t *n, int depth) {
    if (c->overflow || depth > 2000) { c->overflow = true; return; }
    switch (n->kind) {
    case NK_EMPTY:
        break;
    case NK_LIT:
        if (n->fold) {
            uint32_t o[8];
            int k = fold_orbit(n->cp, o);
            cls_t cls = {0};
            for (int i = 0; i < k; i++) cls_add(&cls, o[i], o[i]);
            cls_norm(&cls);
            compile_class(c, &cls);
            vec_free(&cls);
        } else {
            uint8_t b[4];
            int k = enc_utf8(n->cp, b);
            for (int i = 0; i < k; i++) {
                int r = emit(c, OP_RANGE);
                uint8_t x = b[c->rev ? k - 1 - i : i];
                c->p->code.v[r].lo = c->p->code.v[r].hi = x;
            }
        }
        break;
    case NK_CLASS:
        compile_class(c, &n->cls);
        break;
    case NK_CAT:
        for (size_t i = 0; i < n->kids.n; i++) compile_node(c, n->kids.v[c->rev ? n->kids.n - 1 - i : i], depth + 1);
        break;
    case NK_ALT: {
        intvec_t jmps = {0};
        for (size_t i = 0; i < n->kids.n; i++) {
            bool last = i + 1 == n->kids.n;
            int sp = -1;
            if (!last) sp = emit(c, OP_SPLIT);
            compile_node(c, n->kids.v[i], depth + 1);
            if (!last) {
                int j = emit(c, OP_JMP);
                vec_push(&jmps, j);
                c->p->code.v[sp].out1 = (int32_t)c->p->code.n;
            }
        }
        for (size_t i = 0; i < jmps.n; i++) c->p->code.v[jmps.v[i]].out = (int32_t)c->p->code.n;
        vec_free(&jmps);
        break;
    }
    case NK_REP:
        compile_rep(c, n, depth);
        break;
    case NK_GROUP:
        if (!c->rev && n->cap >= 0) {
            int s = emit(c, OP_SAVE);
            c->p->code.v[s].out1 = 2 * n->cap;
        }
        compile_node(c, n->sub, depth + 1);
        if (!c->rev && n->cap >= 0) {
            int s = emit(c, OP_SAVE);
            c->p->code.v[s].out1 = 2 * n->cap + 1;
        }
        break;
    case NK_ASSERT: {
        int a = emit(c, OP_ASSERT);
        c->p->code.v[a].kind = (uint8_t)(c->rev ? mirror_assert(n->akind) : n->akind);
        break;
    }
    }
}

/* ======================================================================
 * rx_t
 * ====================================================================== */
struct rx {
    int flags;
    int ngroups;
    char **names;
    prog_t fwd, rev;
    uint8_t cmap[256];
    int nclasses;
    bool uses_ctx;
    /* literal analysis */
    bool pure;         /* whole regex is the literal lit */
    char *lit;
    size_t litlen;
    bool lit_icase;
    char *req;         /* required literal (prefilter) */
    size_t reqlen;
    bool req_icase;
    bool req_prefix;   /* every match starts with req */
    void *volatile pool[RX_POOL];
};

#define CTX_NONE 0
#define CTX_NL 1
#define CTX_WORD 2
#define CTX_OTHER 3

static const uint8_t *bctx_table(void) {
    static uint8_t t[256];
    static volatile int init = 0;
    if (!__atomic_load_n(&init, __ATOMIC_ACQUIRE)) {
        uint8_t tmp[256];
        for (int b = 0; b < 256; b++) {
            if (b == '\n') tmp[b] = CTX_NL;
            else if (b >= 0x80 || isalnum(b) || b == '_') tmp[b] = CTX_WORD;
            else tmp[b] = CTX_OTHER;
        }
        memcpy(t, tmp, sizeof(t));
        __atomic_store_n(&init, 1, __ATOMIC_RELEASE);
    }
    return t;
}

static inline bool assert_ok(int kind, int prev, int next) {
    switch (kind) {
    case A_TEXT_START: return prev == CTX_NONE;
    case A_TEXT_END: return next == CTX_NONE;
    case A_LINE_START: return prev == CTX_NONE || prev == CTX_NL;
    case A_LINE_END: return next == CTX_NONE || next == CTX_NL;
    case A_WORDB: return (prev == CTX_WORD) != (next == CTX_WORD);
    case A_NWORDB: return (prev == CTX_WORD) == (next == CTX_WORD);
    case A_WORD_START: return prev != CTX_WORD && next == CTX_WORD;
    case A_WORD_END: return prev == CTX_WORD && next != CTX_WORD;
    case A_WORD_START_HALF: return prev != CTX_WORD;
    case A_WORD_END_HALF: return next != CTX_WORD;
    }
    return false;
}

static inline bool inst_accepts(const prog_t *p, const inst_t *in, uint8_t b) {
    if (in->op == OP_RANGE) return b >= in->lo && b <= in->hi;
    return bset_has(&p->sets.v[in->out1], b);
}

/* ---------------- lazy DFA ---------------- */
typedef struct {
    uint32_t koff, klen;
    uint8_t ctx;
} dstate_t;

typedef struct {
    uint32_t *sparse, *dense;
    uint32_t n;
} sset_t;

static void sset_init(sset_t *s, size_t m) {
    s->sparse = xcalloc(m ? m : 1, sizeof(uint32_t));
    s->dense = xcalloc(m ? m : 1, sizeof(uint32_t));
    s->n = 0;
}
static void sset_free(sset_t *s) { free(s->sparse); free(s->dense); }
static inline bool sset_has(const sset_t *s, uint32_t x) { return s->sparse[x] < s->n && s->dense[s->sparse[x]] == x; }
static inline void sset_add(sset_t *s, uint32_t x) { s->dense[s->n] = x; s->sparse[x] = s->n++; }

typedef struct {
    const rx_t *rx;
    const prog_t *prog;
    bool longest;
    int stride;
    VEC(dstate_t) st;
    VEC(int32_t) kpool;
    int32_t *trans;
    size_t trans_cap;
    int32_t *ht;
    size_t htcap, htn;
    int32_t starts[4][2];
    sset_t vis, nk;
    VEC(int32_t) stack, cons, newk, tmpk;
} dfa_t;

static uint64_t khash(int ctx, const int32_t *k, size_t n) {
    uint64_t h = 1469598103934665603ULL ^ (uint64_t)ctx;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)(uint32_t)k[i];
        h *= 1099511628211ULL;
    }
    return h ^ (h >> 29);
}

static void dfa_ht_insert(dfa_t *d, int32_t sid) {
    dstate_t *s = &d->st.v[sid];
    size_t mask = d->htcap - 1;
    size_t i = khash(s->ctx, d->kpool.v + s->koff, s->klen) & mask;
    while (d->ht[i] >= 0) i = (i + 1) & mask;
    d->ht[i] = sid;
    d->htn++;
}

static void dfa_reset(dfa_t *d) {
    d->st.n = 0;
    d->kpool.n = 0;
    dstate_t dead = {0, 0, 0};
    vec_push(&d->st, dead);
    for (int i = 0; i < d->stride; i++) d->trans[i] = 0; /* dead -> dead, no match */
    for (size_t i = 0; i < d->htcap; i++) d->ht[i] = -1;
    d->htn = 0;
    dfa_ht_insert(d, 0);
    for (int i = 0; i < 4; i++) d->starts[i][0] = d->starts[i][1] = -1;
}

static void dfa_init(dfa_t *d, const rx_t *rx, const prog_t *p, bool longest) {
    memset(d, 0, sizeof(*d));
    d->rx = rx;
    d->prog = p;
    d->longest = longest;
    d->stride = rx->nclasses + 1;
    d->trans_cap = (size_t)d->stride * 64;
    d->trans = xmalloc(d->trans_cap * sizeof(int32_t));
    d->htcap = 256;
    d->ht = xmalloc(d->htcap * sizeof(int32_t));
    sset_init(&d->vis, p->code.n);
    sset_init(&d->nk, p->code.n);
    dfa_reset(d);
}

static void dfa_free(dfa_t *d) {
    vec_free(&d->st);
    vec_free(&d->kpool);
    free(d->trans);
    free(d->ht);
    sset_free(&d->vis);
    sset_free(&d->nk);
    vec_free(&d->stack);
    vec_free(&d->cons);
    vec_free(&d->newk);
    vec_free(&d->tmpk);
}

static int32_t dfa_intern(dfa_t *d, int ctx, const int32_t *k, size_t n) {
    size_t mask = d->htcap - 1;
    size_t i = khash(ctx, k, n) & mask;
    while (d->ht[i] >= 0) {
        dstate_t *s = &d->st.v[d->ht[i]];
        if (s->ctx == ctx && s->klen == n && memcmp(d->kpool.v + s->koff, k, n * sizeof(int32_t)) == 0) return d->ht[i];
        i = (i + 1) & mask;
    }
    int32_t sid = (int32_t)d->st.n;
    dstate_t ns = {(uint32_t)d->kpool.n, (uint32_t)n, (uint8_t)ctx};
    vec_push(&d->st, ns);
    for (size_t j = 0; j < n; j++) vec_push(&d->kpool, k[j]);
    size_t need = (size_t)(sid + 1) * d->stride;
    if (need > d->trans_cap) {
        while (d->trans_cap < need) d->trans_cap *= 2;
        d->trans = xrealloc(d->trans, d->trans_cap * sizeof(int32_t));
    }
    for (int j = 0; j < d->stride; j++) d->trans[(size_t)sid * d->stride + j] = -1;
    if ((d->htn + 1) * 2 > d->htcap) {
        free(d->ht);
        d->htcap *= 2;
        d->ht = xmalloc(d->htcap * sizeof(int32_t));
        for (size_t j = 0; j < d->htcap; j++) d->ht[j] = -1;
        d->htn = 0;
        for (int32_t s = 0; s <= sid; s++) dfa_ht_insert(d, s);
    } else {
        dfa_ht_insert(d, sid);
    }
    return sid;
}

static int32_t dfa_start(dfa_t *d, int ctx, bool anchored) {
    if (!d->rx->uses_ctx) ctx = 0;
    int32_t s = d->starts[ctx][anchored];
    if (s >= 0) return s;
    int32_t pc = anchored ? d->prog->start : 0;
    s = dfa_intern(d, ctx, &pc, 1);
    d->starts[ctx][anchored] = s;
    return s;
}

/* Compute transition from *sidp on byte class cls (byte b, or -1 for end of input). */
static int32_t dfa_compute(dfa_t *d, int32_t *sidp, int cls, int b) {
    int32_t sid = *sidp;
    size_t mem = d->st.n * (size_t)d->stride * 4 + d->kpool.n * 4 + d->st.n * sizeof(dstate_t);
    if (mem > DFA_MEM_LIMIT) {
        dstate_t s = d->st.v[sid];
        d->tmpk.n = 0;
        for (uint32_t i = 0; i < s.klen; i++) vec_push(&d->tmpk, d->kpool.v[s.koff + i]);
        dfa_reset(d);
        sid = dfa_intern(d, s.ctx, d->tmpk.v, d->tmpk.n);
        *sidp = sid;
    }
    const prog_t *p = d->prog;
    const uint8_t *bctx = bctx_table();
    dstate_t S = d->st.v[sid];
    int next = b < 0 ? CTX_NONE : bctx[b];
    bool matched = false;
    d->vis.n = 0;
    d->cons.n = 0;
    d->stack.n = 0;
    for (uint32_t ki = 0; ki < S.klen && !(matched && !d->longest); ki++) {
        vec_push(&d->stack, d->kpool.v[S.koff + ki]);
        while (d->stack.n) {
            int32_t pc = d->stack.v[--d->stack.n];
            if (sset_has(&d->vis, (uint32_t)pc)) continue;
            sset_add(&d->vis, (uint32_t)pc);
            const inst_t *in = &p->code.v[pc];
            switch (in->op) {
            case OP_JMP: case OP_SAVE: vec_push(&d->stack, in->out); break;
            case OP_SPLIT: vec_push(&d->stack, in->out1); vec_push(&d->stack, in->out); break;
            case OP_ASSERT: if (assert_ok(in->kind, S.ctx, next)) vec_push(&d->stack, in->out); break;
            case OP_MATCH:
                matched = true;
                if (!d->longest) d->stack.n = 0;
                break;
            default: vec_push(&d->cons, pc); break;
            }
        }
    }
    int32_t nid = 0;
    if (b >= 0) {
        d->nk.n = 0;
        d->newk.n = 0;
        for (size_t i = 0; i < d->cons.n; i++) {
            const inst_t *in = &p->code.v[d->cons.v[i]];
            if (inst_accepts(p, in, (uint8_t)b) && !sset_has(&d->nk, (uint32_t)in->out)) {
                sset_add(&d->nk, (uint32_t)in->out);
                vec_push(&d->newk, in->out);
            }
        }
        if (d->newk.n) {
            if (d->longest) {
                /* order is irrelevant for longest-match: sort to share states */
                for (size_t i = 1; i < d->newk.n; i++) {
                    int32_t x = d->newk.v[i];
                    size_t j = i;
                    while (j > 0 && d->newk.v[j - 1] > x) { d->newk.v[j] = d->newk.v[j - 1]; j--; }
                    d->newk.v[j] = x;
                }
            }
            int nctx = d->rx->uses_ctx ? bctx[b] : 0;
            nid = dfa_intern(d, nctx, d->newk.v, d->newk.n);
        }
    }
    int32_t t = (nid << 1) | (matched ? 1 : 0);
    d->trans[(size_t)sid * d->stride + cls] = t;
    return t;
}

/* Forward scan of s[from..to); returns end of leftmost-first match or -1. */
static ptrdiff_t dfa_fwd(dfa_t *d, const char *s, size_t len, size_t from, size_t to, bool anchored, bool earliest) {
    const uint8_t *bctx = bctx_table();
    const uint8_t *cmap = d->rx->cmap;
    int32_t sid = dfa_start(d, from > 0 ? bctx[(uint8_t)s[from - 1]] : CTX_NONE, anchored);
    ptrdiff_t last = -1;
    const uint8_t *u = (const uint8_t *)s;
    size_t stride = (size_t)d->stride;
    for (size_t i = from; i < to; i++) {
        int cl = cmap[u[i]];
        int32_t t = d->trans[(size_t)sid * stride + cl];
        if (UNLIKELY(t < 0)) t = dfa_compute(d, &sid, cl, u[i]);
        if (t & 1) {
            last = (ptrdiff_t)i;
            if (earliest) return last;
        }
        sid = t >> 1;
        if (sid == 0) return last;
    }
    int cl, b;
    if (to < len) { b = u[to]; cl = cmap[b]; }
    else { b = -1; cl = d->stride - 1; }
    int32_t t = d->trans[(size_t)sid * stride + cl];
    if (t < 0) t = dfa_compute(d, &sid, cl, b);
    if (t & 1) last = (ptrdiff_t)to;
    return last;
}

/* Reverse scan from `end` down to `lo`; returns smallest start of a match ending at end, or -1. */
static ptrdiff_t dfa_rev(dfa_t *d, const char *s, size_t len, size_t lo, size_t end) {
    const uint8_t *bctx = bctx_table();
    const uint8_t *cmap = d->rx->cmap;
    const uint8_t *u = (const uint8_t *)s;
    int32_t sid = dfa_start(d, end < len ? bctx[u[end]] : CTX_NONE, true);
    ptrdiff_t best = -1;
    size_t stride = (size_t)d->stride;
    for (size_t i = end; i > lo; i--) {
        int cl = cmap[u[i - 1]];
        int32_t t = d->trans[(size_t)sid * stride + cl];
        if (UNLIKELY(t < 0)) t = dfa_compute(d, &sid, cl, u[i - 1]);
        if (t & 1) best = (ptrdiff_t)i;
        sid = t >> 1;
        if (sid == 0) return best;
    }
    int cl, b;
    if (lo > 0) { b = u[lo - 1]; cl = cmap[b]; }
    else { b = -1; cl = d->stride - 1; }
    int32_t t = d->trans[(size_t)sid * stride + cl];
    if (t < 0) t = dfa_compute(d, &sid, cl, b);
    if (t & 1) best = (ptrdiff_t)lo;
    return best;
}

/* ---------------- Pike VM ---------------- */
typedef struct {
    int32_t pc;
    int32_t slot; /* >= 0: restore frame */
    ptrdiff_t val;
} pframe_t;

typedef struct {
    sset_t set;
    ptrdiff_t *caps;
} tlist_t;

typedef struct rxcache {
    dfa_t fwd, rev;
    bool pike_ready;
    int nslots;
    tlist_t a, b;
    ptrdiff_t *cur;
    VEC(pframe_t) frames;
} rxcache_t;

static void pike_add(const rx_t *rx, rxcache_t *c, tlist_t *l, int32_t pc0, size_t pos, const char *s, size_t len) {
    const prog_t *p = &rx->fwd;
    const uint8_t *bctx = bctx_table();
    int prev = pos > 0 ? bctx[(uint8_t)s[pos - 1]] : CTX_NONE;
    int next = pos < len ? bctx[(uint8_t)s[pos]] : CTX_NONE;
    int ns = c->nslots;
    c->frames.n = 0;
    pframe_t f0 = {pc0, -1, 0};
    vec_push(&c->frames, f0);
    while (c->frames.n) {
        pframe_t f = c->frames.v[--c->frames.n];
        if (f.slot >= 0) { c->cur[f.slot] = f.val; continue; }
        int32_t pc = f.pc;
        if (sset_has(&l->set, (uint32_t)pc)) continue;
        sset_add(&l->set, (uint32_t)pc);
        const inst_t *in = &p->code.v[pc];
        pframe_t nf = {in->out, -1, 0};
        switch (in->op) {
        case OP_JMP: vec_push(&c->frames, nf); break;
        case OP_SPLIT: {
            pframe_t f1 = {in->out1, -1, 0};
            vec_push(&c->frames, f1);
            vec_push(&c->frames, nf);
            break;
        }
        case OP_SAVE:
            if (in->out1 < ns) {
                pframe_t r = {0, in->out1, c->cur[in->out1]};
                vec_push(&c->frames, r);
                c->cur[in->out1] = (ptrdiff_t)pos;
            }
            vec_push(&c->frames, nf);
            break;
        case OP_ASSERT:
            if (assert_ok(in->kind, prev, next)) vec_push(&c->frames, nf);
            break;
        default:
            memcpy(l->caps + (size_t)pc * ns, c->cur, (size_t)ns * sizeof(ptrdiff_t));
            break;
        }
    }
}

static bool pike(const rx_t *rx, rxcache_t *c, const char *s, size_t len, size_t from, size_t to, bool anchored,
                 ptrdiff_t *out) {
    const prog_t *p = &rx->fwd;
    if (!c->pike_ready) {
        int ng = MIN(rx->ngroups, RX_MAX_GROUPS);
        c->nslots = 2 * ng;
        size_t m = p->code.n;
        sset_init(&c->a.set, m);
        sset_init(&c->b.set, m);
        c->a.caps = xmalloc(m * (size_t)c->nslots * sizeof(ptrdiff_t));
        c->b.caps = xmalloc(m * (size_t)c->nslots * sizeof(ptrdiff_t));
        c->cur = xmalloc((size_t)c->nslots * sizeof(ptrdiff_t));
        c->pike_ready = true;
    }
    int ns = c->nslots;
    tlist_t *cl = &c->a, *nl = &c->b;
    cl->set.n = nl->set.n = 0;
    bool matched = false;
    const uint8_t *u = (const uint8_t *)s;
    for (size_t pos = from;; pos++) {
        if (!matched && (!anchored || pos == from)) {
            for (int i = 0; i < ns; i++) c->cur[i] = -1;
            pike_add(rx, c, cl, p->start, pos, s, len);
        }
        if (cl->set.n == 0) break;
        for (uint32_t i = 0; i < cl->set.n; i++) {
            int32_t pc = (int32_t)cl->set.dense[i];
            const inst_t *in = &p->code.v[pc];
            ptrdiff_t *tc = cl->caps + (size_t)pc * ns;
            if (in->op == OP_MATCH) {
                matched = true;
                memcpy(out, tc, (size_t)ns * sizeof(ptrdiff_t));
                break;
            }
            if (in->op != OP_RANGE && in->op != OP_SET) continue;
            if (pos < to && inst_accepts(p, in, u[pos])) {
                memcpy(c->cur, tc, (size_t)ns * sizeof(ptrdiff_t));
                pike_add(rx, c, nl, in->out, pos + 1, s, len);
            }
        }
        tlist_t *t = cl;
        cl = nl;
        nl = t;
        nl->set.n = 0;
        if (pos >= to) break;
    }
    return matched;
}

static rxcache_t *cache_get(const rx_t *rx) {
    rx_t *m = (rx_t *)rx;
    for (int i = 0; i < RX_POOL; i++) {
        void *c = __atomic_exchange_n(&m->pool[i], NULL, __ATOMIC_ACQUIRE);
        if (c) return c;
    }
    rxcache_t *c = xcalloc(1, sizeof(*c));
    dfa_init(&c->fwd, rx, &rx->fwd, false);
    dfa_init(&c->rev, rx, &rx->rev, true);
    return c;
}

static void cache_free(rxcache_t *c) {
    dfa_free(&c->fwd);
    dfa_free(&c->rev);
    if (c->pike_ready) {
        sset_free(&c->a.set);
        sset_free(&c->b.set);
        free(c->a.caps);
        free(c->b.caps);
        free(c->cur);
    }
    vec_free(&c->frames);
    free(c);
}

static void cache_put(const rx_t *rx, rxcache_t *c) {
    rx_t *m = (rx_t *)rx;
    for (int i = 0; i < RX_POOL; i++) {
        void *exp = NULL;
        if (__atomic_compare_exchange_n(&m->pool[i], &exp, c, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;
    }
    cache_free(c);
}

/* ---------------- literals ---------------- */
static const uint8_t *lower_table(void) {
    static uint8_t t[256];
    static volatile int init = 0;
    if (!__atomic_load_n(&init, __ATOMIC_ACQUIRE)) {
        uint8_t tmp[256];
        for (int i = 0; i < 256; i++) tmp[i] = (uint8_t)((i >= 'A' && i <= 'Z') ? i + 32 : i);
        memcpy(t, tmp, sizeof(t));
        __atomic_store_n(&init, 1, __ATOMIC_RELEASE);
    }
    return t;
}

static void *mem_rchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) {
        if (*--p == (unsigned char)c) return (void *)p;
    }
    return NULL;
}

/* needle must be lowercase when icase */
static ptrdiff_t lit_find(const char *h, size_t hl, size_t from, const char *nd, size_t nl, bool icase) {
    if (from > hl) return -1;
    if (nl == 0) return (ptrdiff_t)from;
    if (hl - from < nl) return -1;
    if (!icase) {
        const char *p = nl == 1 ? memchr(h + from, nd[0], hl - from) : memmem(h + from, hl - from, nd, nl);
        return p ? p - h : -1;
    }
    const uint8_t *lt = lower_table();
    uint8_t c0 = (uint8_t)nd[0];
    bool alpha = c0 >= 'a' && c0 <= 'z';
    uint8_t c1 = alpha ? (uint8_t)(c0 - 32) : c0;
    size_t last = hl - nl;
    const char *pa = NULL, *pb = NULL;
    size_t i = from;
    while (i <= last) {
        if (!pa || pa < h + i) pa = memchr(h + i, c0, last + 1 - i);
        if (alpha && (!pb || pb < h + i)) pb = memchr(h + i, c1, last + 1 - i);
        const char *p = pa;
        if (alpha && pb && (!p || pb < p)) p = pb;
        if (!p) return -1;
        size_t k = 1;
        while (k < nl && lt[(uint8_t)p[k]] == (uint8_t)nd[k]) k++;
        if (k == nl) return p - h;
        i = (size_t)(p - h) + 1;
        if (!pa) pa = h + hl; /* exhausted */
        if (alpha && !pb) pb = h + hl;
    }
    return -1;
}

typedef VEC(const node_t *) nlist_t;
static void flatten(const node_t *n, nlist_t *out, int depth) {
    if (depth < 100 && n->kind == NK_CAT) {
        for (size_t i = 0; i < n->kids.n; i++) flatten(n->kids.v[i], out, depth + 1);
    } else if (depth < 100 && n->kind == NK_GROUP) {
        flatten(n->sub, out, depth + 1);
    } else if (n->kind != NK_EMPTY) {
        vec_push(out, n);
    }
}

static void analyze_literals(rx_t *rx, const node_t *root) {
    nlist_t items = {0};
    flatten(root, &items, 0);
    sb_t cur = SB_INIT, best = SB_INIT;
    bool cur_icase = false, cur_exact_alpha = false, best_icase = false, best_prefix = false;
    size_t cur_start = 0;
    bool any_nonlit = false, any_assert = false, mixed = false;
    size_t lead = 0; /* number of leading asserts */
    while (lead < items.n && items.v[lead]->kind == NK_ASSERT) lead++;
    for (size_t i = 0; i <= items.n; i++) {
        const node_t *n = i < items.n ? items.v[i] : NULL;
        bool ok = false;
        if (n && n->kind == NK_ASSERT) {
            any_assert = true;
            continue;
        }
        if (n && n->kind == NK_LIT) {
            uint32_t cp = n->cp;
            if (!n->fold) {
                ok = true;
            } else if (cp < 0x80 && isalpha((int)cp) && tolower((int)cp) != 'k' && tolower((int)cp) != 's') {
                ok = true;
            }
        }
        if (n && !ok) any_nonlit = true;
        if (ok) {
            if (cur.len == 0) cur_start = i;
            if (n->fold) cur_icase = true;
            else if (n->cp < 0x80 && isalpha((int)n->cp)) cur_exact_alpha = true;
            uint8_t b[4];
            int k = enc_utf8(n->cp, b);
            if (n->fold) b[0] = (uint8_t)tolower(b[0]);
            sb_appendn(&cur, (const char *)b, (size_t)k);
            continue;
        }
        if (cur.len) {
            bool better = cur.len > best.len || (cur.len == best.len && best_icase && !cur_icase);
            if (better) {
                sb_clear(&best);
                sb_appendn(&best, cur.s, cur.len);
                best_icase = cur_icase;
                best_prefix = cur_start == lead;
                mixed = cur_icase && cur_exact_alpha;
            }
        }
        sb_clear(&cur);
        cur_icase = cur_exact_alpha = false;
    }
    if (best.len) {
        if (best_icase) {
            const uint8_t *lt = lower_table();
            for (size_t i = 0; i < best.len; i++) best.s[i] = (char)lt[(uint8_t)best.s[i]];
        }
        if (!any_nonlit && !any_assert && !mixed && rx->ngroups == 1 && items.n > 0) {
            rx->pure = true;
            rx->lit = xstrndup(best.s, best.len);
            rx->litlen = best.len;
            rx->lit_icase = best_icase;
        }
        rx->reqlen = best.len;
        rx->req_icase = best_icase;
        rx->req_prefix = best_prefix;
        rx->req = sb_detach(&best);
    }
    vec_free(&items);
    sb_free(&cur);
    sb_free(&best);
}

static void prog_free(prog_t *p) {
    vec_free(&p->code);
    vec_free(&p->sets);
}

void rx_free(rx_t *rx) {
    if (!rx) return;
    for (int i = 0; i < RX_POOL; i++)
        if (rx->pool[i]) cache_free(rx->pool[i]);
    prog_free(&rx->fwd);
    prog_free(&rx->rev);
    if (rx->names) {
        for (int i = 0; i < rx->ngroups; i++) free(rx->names[i]);
        free(rx->names);
    }
    free(rx->lit);
    free(rx->req);
    free(rx);
}

static void build_cmap(rx_t *rx) {
    bool bnd[257];
    memset(bnd, 0, sizeof(bnd));
    const prog_t *ps[2] = {&rx->fwd, &rx->rev};
    for (int k = 0; k < 2; k++) {
        const prog_t *p = ps[k];
        for (size_t i = 0; i < p->code.n; i++) {
            const inst_t *in = &p->code.v[i];
            if (in->op == OP_RANGE) {
                bnd[in->lo] = true;
                bnd[in->hi + 1] = true;
            } else if (in->op == OP_ASSERT) {
                rx->uses_ctx = true;
            }
        }
        for (size_t i = 0; i < p->sets.n; i++) {
            const bset_t *b = &p->sets.v[i];
            for (int x = 1; x < 256; x++)
                if (bset_has(b, (uint8_t)x) != bset_has(b, (uint8_t)(x - 1))) bnd[x] = true;
        }
    }
    if (rx->uses_ctx) {
        static const int cb[] = {'\n', '\n' + 1, '0', '9' + 1, 'A', 'Z' + 1, '_', '_' + 1, 'a', 'z' + 1, 0x80};
        for (size_t i = 0; i < ARRAY_LEN(cb); i++) bnd[cb[i]] = true;
    }
    int cls = 0;
    for (int b = 0; b < 256; b++) {
        if (b > 0 && bnd[b]) cls++;
        rx->cmap[b] = (uint8_t)cls;
    }
    rx->nclasses = cls + 1;
}

static rx_t *compile_impl(const char *pattern, int flags, char **err, bool *unsupported) {
    if (err) *err = NULL;
    if (unsupported) *unsupported = false;
    if (!pattern) pattern = "";
    parser_t ps;
    memset(&ps, 0, sizeof(ps));
    ps.pat = pattern;
    ps.len = strlen(pattern);
    ps.line = (flags & RX_LINE) != 0;
    if (flags & RX_ICASE) ps.flags |= PF_I;
    if (flags & RX_MULTILINE) ps.flags |= PF_M;
    if (flags & RX_DOTALL) ps.flags |= PF_S;
    if (flags & RX_EXTENDED) ps.flags |= PF_X;
    if (flags & RX_UNGREEDY) ps.flags |= PF_U;
    vec_push(&ps.names, NULL);
    node_t *root;
    if (flags & RX_LITERAL) {
        root = nnew(NK_CAT);
        while (ps.pos < ps.len) {
            int nb;
            uint32_t cp = pdecode(&ps, &nb);
            ps.pos += nb;
            vec_push(&root->kids, mk_lit(&ps, cp));
        }
    } else {
        root = parse_alt(&ps);
        if (!ps.err && ps.pos < ps.len) {
            ps.err = xasprintf("regex parse error at position %zu: unopened group", ps.pos);
        }
    }
    if (ps.err) {
        if (unsupported) *unsupported = ps.unsupported;
        if (err) *err = ps.err;
        else free(ps.err);
        nfree(root);
        for (size_t i = 0; i < ps.names.n; i++) free(ps.names.v[i]);
        vec_free(&ps.names);
        return NULL;
    }
    rx_t *rx = xcalloc(1, sizeof(*rx));
    rx->flags = flags;
    rx->ngroups = ps.ncap + 1;
    rx->names = ps.names.v; /* length ngroups */
    comp_t c = {&rx->fwd, false, false};
    bset_t all;
    memset(&all, 0xFF, sizeof(all));
    add_set(&c, &all);
    int sp = emit(&c, OP_SPLIT); /* 0 */
    rx->fwd.code.v[sp].out = 2;
    rx->fwd.code.v[sp].out1 = 1;
    int any = emit(&c, OP_SET); /* 1 */
    rx->fwd.code.v[any].out1 = 0;
    rx->fwd.code.v[any].out = 0;
    int s0 = emit(&c, OP_SAVE); /* 2 */
    rx->fwd.code.v[s0].out1 = 0;
    rx->fwd.start = 2;
    compile_node(&c, root, 0);
    int s1 = emit(&c, OP_SAVE);
    rx->fwd.code.v[s1].out1 = 1;
    emit(&c, OP_MATCH);
    comp_t rc = {&rx->rev, true, false};
    compile_node(&rc, root, 0);
    emit(&rc, OP_MATCH);
    rx->rev.start = 0;
    if (c.overflow || rc.overflow) {
        if (err) *err = xstrdup("regex too large (compiled program exceeds size limit)");
        nfree(root);
        rx_free(rx);
        return NULL;
    }
    build_cmap(rx);
    analyze_literals(rx, root);
    nfree(root);
    return rx;
}

rx_t *rx_compile(const char *pattern, int flags, char **err) { return compile_impl(pattern, flags, err, NULL); }

rx_t *rx_compile_grep(const char *pattern, int flags, char **err) {
    char *e = NULL;
    bool unsup = false;
    rx_t *rx = compile_impl(pattern, flags, &e, &unsup);
    if (rx) { if (err) *err = NULL; return rx; }
    if (unsup) {
        if (err) *err = e;
        else free(e);
        return NULL;
    }
    if (e && (strstr(e, "unclosed group") || strstr(e, "unopened group"))) {
        sb_t b = SB_INIT;
        for (const char *p = pattern; *p; p++) {
            if (*p == '\\' && p[1]) { sb_putc(&b, *p++); sb_putc(&b, *p); continue; }
            if (*p == '(' || *p == ')') sb_putc(&b, '\\');
            sb_putc(&b, *p);
        }
        rx = compile_impl(sb_cstr(&b), flags, NULL, NULL);
        sb_free(&b);
        if (rx) { free(e); if (err) *err = NULL; return rx; }
    }
    rx = compile_impl(pattern, flags | RX_LITERAL, NULL, NULL);
    if (rx) { free(e); if (err) *err = NULL; return rx; }
    if (err) *err = e;
    else free(e);
    return NULL;
}

/* ---------------- search ---------------- */
static bool find_impl(const rx_t *rx, rxcache_t *c, const char *s, size_t len, size_t start, size_t *ms, size_t *me,
                      bool earliest) {
    if (rx->reqlen && (rx->flags & RX_LINE)) {
        size_t pos = start;
        while (pos <= len) {
            ptrdiff_t p = lit_find(s, len, pos, rx->req, rx->reqlen, rx->req_icase);
            if (p < 0) return false;
            const char *nl = mem_rchr(s + start, '\n', (size_t)p - start);
            size_t ls = nl ? (size_t)(nl - s) + 1 : start;
            const char *ne = memchr(s + p + rx->reqlen, '\n', len - (size_t)p - rx->reqlen);
            size_t le = ne ? (size_t)(ne - s) : len;
            ptrdiff_t e = dfa_fwd(&c->fwd, s, len, ls, le, false, earliest);
            if (e >= 0) {
                *me = (size_t)e;
                if (earliest) return true;
                ptrdiff_t b = dfa_rev(&c->rev, s, len, ls, (size_t)e);
                if (b < 0) {
                    ptrdiff_t caps[2 * RX_MAX_GROUPS];
                    if (!pike(rx, c, s, len, ls, le, false, caps)) return false;
                    *ms = (size_t)caps[0];
                    *me = (size_t)caps[1];
                    return true;
                }
                *ms = (size_t)b;
                return true;
            }
            pos = le + 1;
        }
        return false;
    }
    size_t from = start;
    if (rx->reqlen) {
        ptrdiff_t p = lit_find(s, len, from, rx->req, rx->reqlen, rx->req_icase);
        if (p < 0) return false;
        if (rx->req_prefix) from = (size_t)p;
    }
    ptrdiff_t e = dfa_fwd(&c->fwd, s, len, from, len, false, earliest);
    if (e < 0) return false;
    *me = (size_t)e;
    if (earliest) return true;
    ptrdiff_t b = dfa_rev(&c->rev, s, len, from, (size_t)e);
    if (b < 0) {
        ptrdiff_t caps[2 * RX_MAX_GROUPS];
        if (!pike(rx, c, s, len, from, len, false, caps)) return false;
        *ms = (size_t)caps[0];
        *me = (size_t)caps[1];
        return true;
    }
    *ms = (size_t)b;
    return true;
}

bool rx_find(const rx_t *rx, const char *s, size_t len, size_t start, size_t *mstart, size_t *mend) {
    if (!rx || start > len) return false;
    size_t ms = 0, me = 0;
    if (rx->pure) {
        ptrdiff_t p = lit_find(s, len, start, rx->lit, rx->litlen, rx->lit_icase);
        if (p < 0) return false;
        if (mstart) *mstart = (size_t)p;
        if (mend) *mend = (size_t)p + rx->litlen;
        return true;
    }
    rxcache_t *c = cache_get(rx);
    bool ok = find_impl(rx, c, s, len, start, &ms, &me, false);
    cache_put(rx, c);
    if (ok) {
        if (mstart) *mstart = ms;
        if (mend) *mend = me;
    }
    return ok;
}

bool rx_is_match(const rx_t *rx, const char *s, size_t len, size_t start) {
    if (!rx || start > len) return false;
    if (rx->pure) return lit_find(s, len, start, rx->lit, rx->litlen, rx->lit_icase) >= 0;
    rxcache_t *c = cache_get(rx);
    size_t ms, me;
    bool ok = find_impl(rx, c, s, len, start, &ms, &me, true);
    cache_put(rx, c);
    return ok;
}

bool rx_search(const rx_t *rx, const char *s, size_t len, size_t start, rx_match_t *m) {
    if (!rx || start > len) return false;
    size_t ms = 0, me = 0;
    rxcache_t *c = NULL;
    bool ok;
    if (rx->pure) {
        ptrdiff_t p = lit_find(s, len, start, rx->lit, rx->litlen, rx->lit_icase);
        ok = p >= 0;
        if (ok) { ms = (size_t)p; me = ms + rx->litlen; }
    } else {
        c = cache_get(rx);
        ok = find_impl(rx, c, s, len, start, &ms, &me, false);
    }
    if (ok && m) {
        m->start = ms;
        m->end = me;
        int ng = MIN(rx->ngroups, RX_MAX_GROUPS);
        m->ngroups = ng;
        m->groups[0].start = (ptrdiff_t)ms;
        m->groups[0].end = (ptrdiff_t)me;
        for (int i = 1; i < ng; i++) m->groups[i].start = m->groups[i].end = -1;
        if (ng > 1) {
            if (!c) c = cache_get(rx);
            ptrdiff_t caps[2 * RX_MAX_GROUPS];
            if (pike(rx, c, s, len, ms, me, true, caps)) {
                for (int i = 1; i < ng; i++) {
                    m->groups[i].start = caps[2 * i];
                    m->groups[i].end = caps[2 * i + 1];
                    if (m->groups[i].start < 0 || m->groups[i].end < 0) m->groups[i].start = m->groups[i].end = -1;
                }
            }
        }
    }
    if (c) cache_put(rx, c);
    return ok;
}

int rx_ngroups(const rx_t *rx) { return rx ? rx->ngroups : 0; }
int rx_flags(const rx_t *rx) { return rx ? rx->flags : 0; }

int rx_group_index(const rx_t *rx, const char *name) {
    if (!rx || !name) return -1;
    for (int i = 1; i < rx->ngroups; i++)
        if (rx->names[i] && strcmp(rx->names[i], name) == 0) return i;
    return -1;
}

const char *rx_group_name(const rx_t *rx, int i) {
    if (!rx || i <= 0 || i >= rx->ngroups) return NULL;
    return rx->names[i];
}

bool rx_has_uppercase(const char *p) {
    if (!p) return false;
    size_t n = strlen(p), i = 0;
    while (i < n) {
        char c = p[i];
        if (c == '\\') {
            i++;
            if (i >= n) break;
            char e = p[i++];
            if ((e == 'p' || e == 'P' || e == 'x' || e == 'u' || e == 'U') && i < n && p[i] == '{') {
                while (i < n && p[i] != '}') i++;
                if (i < n) i++;
            } else if (e == 'p' || e == 'P') {
                if (i < n) i++;
            } else if (e == 'x') {
                for (int k = 0; k < 2 && i < n && isxdigit((unsigned char)p[i]); k++) i++;
            } else if (e == 'u' || e == 'U') {
                int lim = e == 'u' ? 4 : 8;
                for (int k = 0; k < lim && i < n && isxdigit((unsigned char)p[i]); k++) i++;
            } else if (e == 'b' && i < n && p[i] == '{') {
                while (i < n && p[i] != '}') i++;
                if (i < n) i++;
            } else if ((unsigned char)e >= 0x80) {
                uint32_t cp;
                int nb = utf8_decode(p + i - 1, n - (i - 1), &cp);
                if (cp_is_upper(cp)) return true;
                i += (size_t)(nb > 0 ? nb - 1 : 0);
            }
            continue;
        }
        if (c == '(' && i + 1 < n && p[i + 1] == '?') {
            i += 2;
            if (i < n && p[i] == 'P') i++;
            if (i < n && p[i] == '<') {
                while (i < n && p[i] != '>') i++;
                if (i < n) i++;
                continue;
            }
            while (i < n && p[i] != ':' && p[i] != ')') i++;
            continue;
        }
        if (c == '[' && i + 1 < n && p[i + 1] == ':') {
            const char *end = strstr(p + i + 2, ":]");
            if (end) { i = (size_t)(end - p) + 2; continue; }
        }
        uint32_t cp;
        int nb = utf8_decode(p + i, n - i, &cp);
        if (cp_is_upper(cp)) return true;
        i += (size_t)(nb > 0 ? nb : 1);
    }
    return false;
}

char *rx_escape(const char *s) {
    sb_t b = SB_INIT;
    for (; s && *s; s++) {
        if (strchr("\\.+*?()|[]{}^$#&-~", *s)) sb_putc(&b, '\\');
        sb_putc(&b, *s);
    }
    return sb_detach(&b);
}

static void expand_rep(sb_t *out, const rx_t *rx, const rx_match_t *m, const char *s, const char *rep) {
    for (const char *p = rep; *p; p++) {
        if (*p != '$') { sb_putc(out, *p); continue; }
        p++;
        if (*p == '$') { sb_putc(out, '$'); continue; }
        const char *name;
        size_t nlen;
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (!e) { sb_append(out, "${"); continue; }
            name = p + 1;
            nlen = (size_t)(e - name);
            p = e;
        } else {
            name = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            nlen = (size_t)(p - name);
            p--;
            if (nlen == 0) { sb_putc(out, '$'); continue; }
        }
        int gi = -1;
        bool digits = nlen > 0;
        for (size_t i = 0; i < nlen; i++)
            if (!isdigit((unsigned char)name[i])) digits = false;
        if (digits) {
            gi = nlen < 6 ? atoi(name) : -1;
        } else {
            char *nm = xstrndup(name, nlen);
            gi = rx_group_index(rx, nm);
            free(nm);
        }
        if (gi >= 0 && gi < m->ngroups && m->groups[gi].start >= 0)
            sb_appendn(out, s + m->groups[gi].start, (size_t)(m->groups[gi].end - m->groups[gi].start));
    }
}

char *rx_replace(const rx_t *rx, const char *s, size_t len, const char *rep, bool all) {
    sb_t out = SB_INIT;
    size_t pos = 0, copied = 0;
    ptrdiff_t last_end = -1;
    rx_match_t m;
    while (pos <= len && rx_search(rx, s, len, pos, &m)) {
        if (m.start == m.end && (ptrdiff_t)m.start == last_end) {
            /* skip empty match adjacent to the previous match */
            if (m.start >= len) break;
            uint32_t cp;
            pos = m.start + (size_t)utf8_decode(s + m.start, len - m.start, &cp);
            continue;
        }
        sb_appendn(&out, s + copied, m.start - copied);
        expand_rep(&out, rx, &m, s, rep ? rep : "");
        copied = m.end;
        last_end = (ptrdiff_t)m.end;
        if (!all) break;
        if (m.end == m.start) {
            if (m.end >= len) break;
            uint32_t cp;
            pos = m.end + (size_t)utf8_decode(s + m.end, len - m.end, &cp);
            sb_appendn(&out, s + copied, pos - copied);
            copied = pos;
        } else {
            pos = m.end;
        }
    }
    if (copied < len) sb_appendn(&out, s + copied, len - copied);
    return sb_detach(&out);
}
