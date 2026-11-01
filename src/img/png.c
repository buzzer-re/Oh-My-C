/* png.c - PNG decoder (all color types and bit depths, palette + tRNS, Adam7) and encoder.
 * zlib (a permitted system dependency) inflates/deflates the IDAT stream; everything else is here.
 * Critical-chunk CRCs are verified; ancillary chunks are skipped. 16-bit samples keep their high byte. */
#include "img/img.h"
#include "core/core.h"
#include <zlib.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int fail(char **err, const char *msg) {
    if (err) *err = xstrdup(msg);
    return -1;
}

static const int adam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};

static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    return (uint8_t)(pb <= pc ? b : c);
}

/* Undo the filter of one scanline in place; prev is the previous (already unfiltered) line or NULL. */
static int unfilter(uint8_t ft, uint8_t *row, const uint8_t *prev, size_t len, size_t bpp) {
    switch (ft) {
    case 0: break;
    case 1:
        for (size_t i = bpp; i < len; i++) row[i] = (uint8_t)(row[i] + row[i - bpp]);
        break;
    case 2:
        if (prev)
            for (size_t i = 0; i < len; i++) row[i] = (uint8_t)(row[i] + prev[i]);
        break;
    case 3:
        for (size_t i = 0; i < len; i++) {
            int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0;
            row[i] = (uint8_t)(row[i] + ((a + b) >> 1));
        }
        break;
    case 4:
        for (size_t i = 0; i < len; i++) {
            int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
            row[i] = (uint8_t)(row[i] + paeth(a, b, c));
        }
        break;
    default: return -1;
    }
    return 0;
}

typedef struct {
    int ct, depth, ch;
    const uint8_t *plte;
    int nplte;
    uint8_t trns[256];
    int ntrns;
    int tkey[3]; /* gray / rgb transparent key (raw sample values), -1 none */
} pinfo_t;

static unsigned sample(const uint8_t *row, size_t idx, int depth) {
    switch (depth) {
    case 16: return (unsigned)row[idx * 2] << 8 | row[idx * 2 + 1];
    case 8: return row[idx];
    default: {
        size_t bit = idx * depth;
        unsigned v = row[bit >> 3] >> (8 - depth - (bit & 7));
        return v & ((1u << depth) - 1);
    }
    }
}

static uint8_t to8(unsigned s, int depth) {
    if (depth == 16) return (uint8_t)(s >> 8);
    if (depth == 8) return (uint8_t)s;
    return (uint8_t)(s * 255 / ((1u << depth) - 1));
}

/* Convert one unfiltered scanline of `w` pixels into RGBA at x0, x0+dx, ... of output row `orow`. */
static void emit_row(const pinfo_t *pi, const uint8_t *row, int w, uint8_t *orow, int x0, int dx) {
    for (int i = 0; i < w; i++) {
        uint8_t *o = orow + (size_t)(x0 + i * dx) * 4;
        size_t si = (size_t)i * pi->ch;
        switch (pi->ct) {
        case 0: {
            unsigned g = sample(row, si, pi->depth);
            o[0] = o[1] = o[2] = to8(g, pi->depth);
            o[3] = pi->tkey[0] >= 0 && (int)g == pi->tkey[0] ? 0 : 255;
            break;
        }
        case 2: {
            unsigned r = sample(row, si, pi->depth), g = sample(row, si + 1, pi->depth), b = sample(row, si + 2, pi->depth);
            o[0] = to8(r, pi->depth), o[1] = to8(g, pi->depth), o[2] = to8(b, pi->depth);
            o[3] = pi->tkey[0] >= 0 && (int)r == pi->tkey[0] && (int)g == pi->tkey[1] && (int)b == pi->tkey[2] ? 0 : 255;
            break;
        }
        case 3: {
            unsigned ix = sample(row, si, pi->depth);
            if ((int)ix < pi->nplte) {
                o[0] = pi->plte[ix * 3], o[1] = pi->plte[ix * 3 + 1], o[2] = pi->plte[ix * 3 + 2];
            } else {
                o[0] = o[1] = o[2] = 0; /* out-of-range index: black (libpng-compatible) */
            }
            o[3] = (int)ix < pi->ntrns ? pi->trns[ix] : 255;
            break;
        }
        case 4: {
            uint8_t g = to8(sample(row, si, pi->depth), pi->depth);
            o[0] = o[1] = o[2] = g;
            o[3] = to8(sample(row, si + 1, pi->depth), pi->depth);
            break;
        }
        default:
            for (int c = 0; c < 4; c++) o[c] = to8(sample(row, si + c, pi->depth), pi->depth);
            break;
        }
    }
}

int img_png_decode(const uint8_t *d, size_t n, img_t *out, char **err) {
    out->w = out->h = 0;
    out->px = NULL;
    if (err) *err = NULL;
    if (n < 8 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return fail(err, "not a PNG file");
    pinfo_t pi = {.tkey = {-1, -1, -1}};
    uint32_t w = 0, h = 0;
    int interlace = 0;
    bool have_ihdr = false, have_iend = false;
    sb_t idat = SB_INIT;
    size_t off = 8;
    while (off + 12 <= n) {
        uint32_t len = rd32(d + off);
        const uint8_t *type = d + off + 4;
        if (len > 0x7fffffffu || len > n - off - 12) {
            sb_free(&idat);
            return fail(err, "truncated PNG chunk");
        }
        const uint8_t *cd = d + off + 8;
        bool critical = !(type[0] & 0x20);
        if (critical) {
            uint32_t crc = (uint32_t)crc32(0, type, 4);
            crc = (uint32_t)crc32(crc, cd, len);
            if (crc != rd32(cd + len)) {
                sb_free(&idat);
                return fail(err, "PNG chunk CRC mismatch");
            }
        }
        if (!memcmp(type, "IHDR", 4)) {
            if (have_ihdr || len != 13) goto bad;
            w = rd32(cd), h = rd32(cd + 4);
            pi.depth = cd[8], pi.ct = cd[9];
            if (cd[10] != 0 || cd[11] != 0 || cd[12] > 1) goto bad;
            interlace = cd[12];
            int dp = pi.depth;
            bool ok = (pi.ct == 0 && (dp == 1 || dp == 2 || dp == 4 || dp == 8 || dp == 16)) ||
                      (pi.ct == 3 && (dp == 1 || dp == 2 || dp == 4 || dp == 8)) ||
                      ((pi.ct == 2 || pi.ct == 4 || pi.ct == 6) && (dp == 8 || dp == 16));
            if (!ok) goto bad;
            pi.ch = pi.ct == 0 || pi.ct == 3 ? 1 : pi.ct == 4 ? 2 : pi.ct == 2 ? 3 : 4;
            if (w == 0 || h == 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM || (uint64_t)w * h > IMG_MAX_PIXELS) {
                sb_free(&idat);
                return fail(err, "PNG dimensions out of range");
            }
            have_ihdr = true;
        } else if (!have_ihdr) {
            goto bad;
        } else if (!memcmp(type, "PLTE", 4)) {
            if (len % 3 || len / 3 > 256 || len == 0) {
                if (pi.ct == 3) goto bad;
            } else {
                pi.plte = cd;
                pi.nplte = (int)(len / 3);
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            if (pi.ct == 3) {
                pi.ntrns = (int)MIN(len, 256u);
                memcpy(pi.trns, cd, (size_t)pi.ntrns);
            } else if (pi.ct == 0 && len >= 2) {
                pi.tkey[0] = (cd[0] << 8 | cd[1]) & (pi.depth == 16 ? 0xffff : (1 << pi.depth) - 1);
            } else if (pi.ct == 2 && len >= 6) {
                int mask = pi.depth == 16 ? 0xffff : 0xff;
                for (int c = 0; c < 3; c++) pi.tkey[c] = (cd[c * 2] << 8 | cd[c * 2 + 1]) & mask;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            sb_appendn(&idat, (const char *)cd, len);
        } else if (!memcmp(type, "IEND", 4)) {
            have_iend = true;
            break;
        } else if (critical) {
            sb_free(&idat);
            return fail(err, "unknown critical PNG chunk");
        }
        off += 12 + (size_t)len;
    }
    (void)have_iend;
    if (!have_ihdr) goto bad;
    if (pi.ct == 3 && !pi.plte) {
        sb_free(&idat);
        return fail(err, "PNG palette missing");
    }
    /* expected inflated size */
    size_t bits_pp = (size_t)pi.ch * pi.depth;
    size_t bpp = MAX((size_t)1, bits_pp / 8);
    uint64_t need = 0;
    int npass = interlace ? 7 : 1;
    for (int p = 0; p < npass; p++) {
        uint64_t pw = interlace ? (w > (uint32_t)adam7[p][0] ? (w - adam7[p][0] + adam7[p][2] - 1) / adam7[p][2] : 0) : w;
        uint64_t ph = interlace ? (h > (uint32_t)adam7[p][1] ? (h - adam7[p][1] + adam7[p][3] - 1) / adam7[p][3] : 0) : h;
        if (!pw || !ph) continue;
        need += ph * (1 + (pw * bits_pp + 7) / 8);
    }
    if (need > ((uint64_t)1 << 32)) {
        sb_free(&idat);
        return fail(err, "PNG image too large");
    }
    uint8_t *raw = xmalloc((size_t)need ? (size_t)need : 1);
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit(&zs) != Z_OK) {
        free(raw);
        sb_free(&idat);
        return fail(err, "inflate init failed");
    }
    zs.next_in = (Bytef *)idat.s;
    size_t inleft = idat.len;
    size_t got = 0;
    int zr = Z_OK;
    while (got < need) {
        uInt chunk_in = (uInt)MIN(inleft, (size_t)1 << 30);
        zs.avail_in = chunk_in;
        uInt chunk_out = (uInt)MIN((size_t)need - got, (size_t)1 << 30);
        zs.next_out = raw + got;
        zs.avail_out = chunk_out;
        zr = inflate(&zs, Z_NO_FLUSH);
        got += chunk_out - zs.avail_out;
        inleft -= chunk_in - zs.avail_in;
        if (zr == Z_STREAM_END) break;
        if (zr != Z_OK || (zs.avail_out && !inleft)) break;
    }
    inflateEnd(&zs);
    sb_free(&idat);
    if (got < need) {
        free(raw);
        return fail(err, zr == Z_DATA_ERROR ? "corrupt PNG image data" : "truncated PNG image data");
    }
    if (img_alloc(out, (int)w, (int)h) < 0) {
        free(raw);
        return fail(err, "PNG dimensions out of range");
    }
    uint8_t *p = raw;
    for (int pass = 0; pass < npass; pass++) {
        int x0 = interlace ? adam7[pass][0] : 0, y0 = interlace ? adam7[pass][1] : 0;
        int dx = interlace ? adam7[pass][2] : 1, dy = interlace ? adam7[pass][3] : 1;
        uint32_t pw = (uint32_t)x0 < w ? (w - x0 + dx - 1) / dx : 0;
        uint32_t ph = (uint32_t)y0 < h ? (h - y0 + dy - 1) / dy : 0;
        if (!pw || !ph) continue;
        size_t rowlen = ((size_t)pw * bits_pp + 7) / 8;
        uint8_t *prev = NULL;
        for (uint32_t y = 0; y < ph; y++) {
            uint8_t ft = *p;
            uint8_t *row = p + 1;
            if (unfilter(ft, row, prev, rowlen, bpp) < 0) {
                free(raw);
                img_free(out);
                return fail(err, "bad PNG filter type");
            }
            emit_row(&pi, row, (int)pw, out->px + (size_t)(y0 + y * dy) * w * 4, x0, dx);
            prev = row;
            p += 1 + rowlen;
        }
    }
    free(raw);
    return 0;
bad:
    sb_free(&idat);
    return fail(err, "malformed PNG header");
}

/* ---------- encoder ---------- */

static void put32(sb_t *b, uint32_t v) {
    char c[4] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
    sb_appendn(b, c, 4);
}

static void put_chunk(sb_t *b, const char *type, const uint8_t *data, size_t len) {
    put32(b, (uint32_t)len);
    sb_appendn(b, type, 4);
    if (len) sb_appendn(b, (const char *)data, len);
    uint32_t crc = (uint32_t)crc32(0, (const Bytef *)type, 4);
    if (len) crc = (uint32_t)crc32(crc, data, (uInt)len);
    put32(b, crc);
}

uint8_t *img_png_encode(const img_t *im, int level, size_t *len) {
    *len = 0;
    if (!im || !im->px || im->w <= 0 || im->h <= 0) return NULL;
    bool alpha = img_translucent(im);
    size_t ch = alpha ? 4 : 3, rowlen = (size_t)im->w * ch;
    size_t rawlen = (rowlen + 1) * im->h;
    uint8_t *raw = xmalloc(rawlen);
    uint8_t *cur = xmalloc(rowlen), *prev = xcalloc(rowlen, 1), *cand = xmalloc(rowlen), *best = xmalloc(rowlen);
    for (int y = 0; y < im->h; y++) {
        const uint8_t *src = im->px + (size_t)y * im->w * 4;
        for (int x = 0; x < im->w; x++)
            for (size_t c = 0; c < ch; c++) cur[x * ch + c] = src[x * 4 + c];
        /* adaptive filter choice: minimum sum of absolute signed residuals (libpng heuristic) */
        uint64_t best_cost = UINT64_MAX;
        uint8_t best_ft = 0;
        for (uint8_t ft = 0; ft < 5; ft++) {
            uint64_t cost = 0;
            for (size_t i = 0; i < rowlen; i++) {
                int a = i >= ch ? cur[i - ch] : 0, b = y ? prev[i] : 0, c = y && i >= ch ? prev[i - ch] : 0;
                uint8_t v;
                switch (ft) {
                case 0: v = cur[i]; break;
                case 1: v = (uint8_t)(cur[i] - a); break;
                case 2: v = (uint8_t)(cur[i] - b); break;
                case 3: v = (uint8_t)(cur[i] - ((a + b) >> 1)); break;
                default: v = (uint8_t)(cur[i] - paeth(a, b, c)); break;
                }
                cand[i] = v;
                cost += v < 128 ? v : 256 - v;
            }
            if (cost < best_cost) {
                best_cost = cost;
                best_ft = ft;
                memcpy(best, cand, rowlen);
            }
        }
        uint8_t *o = raw + (size_t)y * (rowlen + 1);
        o[0] = best_ft;
        memcpy(o + 1, best, rowlen);
        uint8_t *t = prev;
        prev = cur;
        cur = t;
    }
    free(cur), free(prev), free(cand), free(best);
    uLongf zcap = compressBound((uLong)rawlen);
    uint8_t *z = xmalloc(zcap);
    if (compress2(z, &zcap, raw, (uLong)rawlen, level < 0 || level > 9 ? 6 : level) != Z_OK) {
        free(z), free(raw);
        return NULL;
    }
    free(raw);
    sb_t b = SB_INIT;
    sb_appendn(&b, "\x89PNG\r\n\x1a\n", 8);
    uint8_t ihdr[13];
    uint32_t w = (uint32_t)im->w, h = (uint32_t)im->h;
    for (int i = 0; i < 4; i++) ihdr[i] = (uint8_t)(w >> (24 - 8 * i)), ihdr[4 + i] = (uint8_t)(h >> (24 - 8 * i));
    ihdr[8] = 8, ihdr[9] = alpha ? 6 : 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    put_chunk(&b, "IHDR", ihdr, 13);
    put_chunk(&b, "IDAT", z, zcap);
    put_chunk(&b, "IEND", NULL, 0);
    free(z);
    *len = b.len;
    return (uint8_t *)sb_detach(&b);
}
