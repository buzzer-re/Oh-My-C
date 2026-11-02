/* gif.c - GIF87a/89a decoder: first frame only (what omp's Bun.Image pipeline keeps), composited onto
 * a transparent logical screen, with the graphic-control transparent index honored. Palette colors of
 * transparent pixels are kept under alpha 0 (as Pillow reports them). */
#include "img/img.h"
#include "core/core.h"

static int fail(char **err, const char *msg) {
    if (err) *err = xstrdup(msg);
    return -1;
}

typedef struct {
    const uint8_t *d;
    size_t n, off;
} rd_t;

/* LZW over the sub-block stream. Writes up to `npx` palette indices into `idx`; returns pixels written,
 * or -1 when the input ends before the block terminator (truncated file) or a code is invalid. */
static long lzw_decode(rd_t *r, int min_code, uint8_t *idx, size_t npx) {
    if (min_code < 2 || min_code > 11) return -1;
    uint16_t prefix[4096];
    uint8_t suffix[4096], first[4096];
    uint8_t stack[4097];
    int clear = 1 << min_code, eoi = clear + 1;
    int code_size = min_code + 1, next = eoi + 1;
    int prev = -1;
    for (int i = 0; i < clear; i++) prefix[i] = 0xffff, suffix[i] = (uint8_t)i, first[i] = (uint8_t)i;
    uint32_t bits = 0;
    int nbits = 0;
    size_t out = 0;
    bool done = false;
    for (;;) {
        if (r->off >= r->n) return -1;
        size_t blen = r->d[r->off++];
        if (blen == 0) break; /* block terminator */
        if (blen > r->n - r->off) return -1;
        const uint8_t *b = r->d + r->off;
        r->off += blen;
        if (done) continue;
        for (size_t i = 0; i < blen && !done; i++) {
            bits |= (uint32_t)b[i] << nbits;
            nbits += 8;
            while (nbits >= code_size && !done) {
                int code = (int)(bits & ((1u << code_size) - 1));
                bits >>= code_size;
                nbits -= code_size;
                if (code == clear) {
                    code_size = min_code + 1;
                    next = eoi + 1;
                    prev = -1;
                    continue;
                }
                if (code == eoi) {
                    done = true;
                    break;
                }
                int cur = code, sp = 0;
                if (prev < 0) {
                    if (code >= clear) return -1;
                    stack[sp++] = suffix[code];
                } else {
                    if (code > next || (code == next && next >= 4096)) return -1;
                    if (code == next) { /* KwKwK */
                        stack[sp++] = first[prev];
                        cur = prev;
                    }
                    while (cur >= clear) {
                        if (sp >= 4096 || cur >= 4096) return -1;
                        stack[sp++] = suffix[cur];
                        cur = prefix[cur];
                    }
                    stack[sp++] = suffix[cur];
                    if (next < 4096) {
                        prefix[next] = (uint16_t)prev;
                        suffix[next] = stack[sp - 1]; /* first char of the current string */
                        first[next] = first[prev];
                        next++;
                        if (next == (1 << code_size) && code_size < 12) code_size++;
                    }
                }
                if (prev < 0 && code < clear) first[code] = (uint8_t)code;
                while (sp > 0) {
                    uint8_t v = stack[--sp];
                    if (out < npx) idx[out++] = v;
                }
                prev = code;
                if (out >= npx) done = true;
            }
        }
    }
    return (long)out;
}

int img_gif_decode(const uint8_t *d, size_t n, img_t *out, char **err) {
    out->w = out->h = 0;
    out->px = NULL;
    if (err) *err = NULL;
    if (n < 13 || (memcmp(d, "GIF87a", 6) && memcmp(d, "GIF89a", 6))) return fail(err, "not a GIF file");
    int sw = d[6] | d[7] << 8, sh = d[8] | d[9] << 8;
    uint8_t flags = d[10];
    rd_t r = {d, n, 13};
    const uint8_t *gct = NULL;
    int ngct = 0;
    if (flags & 0x80) {
        ngct = 2 << (flags & 7);
        if ((size_t)ngct * 3 > n - r.off) return fail(err, "truncated GIF color table");
        gct = d + r.off;
        r.off += (size_t)ngct * 3;
    }
    int trns = -1;
    for (;;) {
        if (r.off >= n) return fail(err, "truncated GIF");
        uint8_t intro = d[r.off++];
        if (intro == 0x3b) return fail(err, "GIF has no image");
        if (intro == 0x21) {
            if (r.off >= n) return fail(err, "truncated GIF");
            uint8_t label = d[r.off++];
            bool first_block = true;
            for (;;) {
                if (r.off >= n) return fail(err, "truncated GIF");
                size_t blen = d[r.off++];
                if (!blen) break;
                if (blen > n - r.off) return fail(err, "truncated GIF");
                if (label == 0xf9 && first_block && blen >= 4) trns = (d[r.off] & 1) ? d[r.off + 3] : -1;
                first_block = false;
                r.off += blen;
            }
            continue;
        }
        if (intro != 0x2c) return fail(err, "malformed GIF block");
        if (n - r.off < 9) return fail(err, "truncated GIF");
        const uint8_t *id = d + r.off;
        int fx = id[0] | id[1] << 8, fy = id[2] | id[3] << 8, fw = id[4] | id[5] << 8, fh = id[6] | id[7] << 8;
        uint8_t ifl = id[8];
        r.off += 9;
        const uint8_t *pal = gct;
        int npal = ngct;
        if (ifl & 0x80) {
            npal = 2 << (ifl & 7);
            if ((size_t)npal * 3 > n - r.off) return fail(err, "truncated GIF color table");
            pal = d + r.off;
            r.off += (size_t)npal * 3;
        }
        /* a frame larger than the logical screen grows it (browsers and Pillow do the same) */
        if (sw < fx + fw) sw = fx + fw;
        if (sh < fy + fh) sh = fy + fh;
        if (fw == 0 || fh == 0 || sw == 0 || sh == 0) return fail(err, "empty GIF frame");
        if (img_alloc(out, sw, sh) < 0) return fail(err, "GIF dimensions out of range");
        if (r.off >= n) {
            img_free(out);
            return fail(err, "truncated GIF");
        }
        int min_code = d[r.off++];
        size_t npx = (size_t)fw * fh;
        uint8_t *idx = xmalloc(npx);
        long got = lzw_decode(&r, min_code, idx, npx);
        if (got < 0) {
            free(idx);
            img_free(out);
            return fail(err, "corrupt or truncated GIF image data");
        }
        uint8_t gray[768];
        if (!pal) {
            for (int i = 0; i < 256; i++) gray[i * 3] = gray[i * 3 + 1] = gray[i * 3 + 2] = (uint8_t)i;
            pal = gray, npal = 256;
        }
        bool interlaced = (ifl & 0x40) != 0;
        size_t k = 0;
        for (int pass = 0; pass < (interlaced ? 4 : 1); pass++) {
            static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
            int y0 = interlaced ? start[pass] : 0, dy = interlaced ? step[pass] : 1;
            for (int y = y0; y < fh; y += dy) {
                uint8_t *row = out->px + ((size_t)(fy + y) * sw + fx) * 4;
                for (int x = 0; x < fw; x++, k++) {
                    if (k >= (size_t)got) continue; /* stream ended early: leave transparent */
                    int ci = idx[k];
                    uint8_t *o = row + (size_t)x * 4;
                    if (ci < npal) o[0] = pal[ci * 3], o[1] = pal[ci * 3 + 1], o[2] = pal[ci * 3 + 2];
                    o[3] = ci == trns ? 0 : 255;
                }
            }
        }
        free(idx);
        return 0;
    }
}
