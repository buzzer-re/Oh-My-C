/* dib.c - Windows packed DIB (CF_DIB clipboard payload) to PNG.
 *
 * Port of omp's natives clipboard.rs dib_to_png, which hands the payload to a BMP decoder: a BITMAPCOREHEADER /
 * BITMAPINFOHEADER / V4 / V5 header, optional bitfield masks and palette, then the pixel array. The payload comes from
 * whatever program owns the clipboard, so it is untrusted: every offset is bounds-checked and the raster obeys the
 * IMG_MAX_* caps before memory is allocated. Supported: 1/4/8-bit palettes, 16/24/32-bit BI_RGB, BI_BITFIELDS and
 * BI_ALPHABITFIELDS, top-down and bottom-up rows, and BI_PNG (the PNG is passed through). RLE and BI_JPEG fail. */
#include "img/img.h"
#include "core/core.h"

#include <stdlib.h>
#include <string.h>

enum { BI_RGB_ = 0, BI_BITFIELDS_ = 3, BI_JPEG_ = 4, BI_PNG_ = 5, BI_ALPHABITFIELDS_ = 6 };

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void fail(char **err, const char *m) {
    if (err) *err = xstrdup(m);
}

/* a channel from a bitfield mask, scaled to 8 bits (a mask of 0 yields `def`) */
typedef struct {
    uint32_t mask;
    int shift, bits;
} chan_t;

static chan_t chan(uint32_t mask) {
    chan_t c = {mask, 0, 0};
    if (!mask) return c;
    while (!(mask & 1)) mask >>= 1, c.shift++;
    while (mask & 1) mask >>= 1, c.bits++;
    if (c.bits > 8) c.shift += c.bits - 8, c.bits = 8; /* keep the top 8 bits */
    return c;
}

static uint8_t chan_get(const chan_t *c, uint32_t v, uint8_t def) {
    if (!c->mask) return def;
    uint32_t x = (v & c->mask) >> c->shift;
    if (c->bits >= 8) return (uint8_t)(x & 0xFF);
    uint32_t max = (1u << c->bits) - 1;
    return (uint8_t)((x * 255 + max / 2) / max);
}

uint8_t *img_dib_to_png(const uint8_t *d, size_t n, size_t *out_len, char **err) {
    if (err) *err = NULL;
    *out_len = 0;
    if (!d || n < 12) {
        fail(err, "Clipboard DIB shorter than its header");
        return NULL;
    }
    uint32_t hsize = rd32(d);
    int64_t w, h;
    unsigned bpp, comp = BI_RGB_;
    uint32_t clr_used = 0, masks[4] = {0, 0, 0, 0};
    bool have_masks = false, core = hsize == 12;
    if (core) {
        w = rd16(d + 4), h = (int16_t)rd16(d + 6), bpp = rd16(d + 10);
    } else {
        if (hsize < 40 || hsize > 124 || hsize > n) {
            fail(err, "Clipboard DIB header size out of range");
            return NULL;
        }
        w = (int32_t)rd32(d + 4), h = (int32_t)rd32(d + 8), bpp = rd16(d + 14), comp = rd32(d + 16);
        clr_used = rd32(d + 32);
        if (hsize >= 52) masks[0] = rd32(d + 40), masks[1] = rd32(d + 44), masks[2] = rd32(d + 48), have_masks = true;
        if (hsize >= 56) masks[3] = rd32(d + 52);
    }
    size_t off = hsize;
    /* a plain BITMAPINFOHEADER keeps its masks right after the header (Qt writes 32-bit content this way) */
    if (!core && hsize == 40 && (comp == BI_BITFIELDS_ || comp == BI_ALPHABITFIELDS_)) {
        size_t k = comp == BI_ALPHABITFIELDS_ ? 4 : 3;
        if (n < off + 4 * k) {
            fail(err, "Clipboard DIB truncated in its bitfield masks");
            return NULL;
        }
        for (size_t i = 0; i < k; i++) masks[i] = rd32(d + off + 4 * i);
        off += 4 * k;
        have_masks = true;
    }
    if (comp == BI_PNG_) { /* the pixel data is a PNG */
        if (off >= n) {
            fail(err, "Clipboard DIB has no PNG data");
            return NULL;
        }
        uint8_t *p = xmalloc(n - off);
        memcpy(p, d + off, n - off);
        *out_len = n - off;
        return p;
    }
    if (comp != BI_RGB_ && comp != BI_BITFIELDS_ && comp != BI_ALPHABITFIELDS_) {
        fail(err, comp == BI_JPEG_ ? "Clipboard DIB holds JPEG data" : "Clipboard DIB compression not supported");
        return NULL;
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        fail(err, "Clipboard DIB bit depth not supported");
        return NULL;
    }
    if (comp != BI_RGB_ && bpp != 16 && bpp != 32) {
        fail(err, "Clipboard DIB bitfields need 16 or 32 bits per pixel");
        return NULL;
    }
    bool top_down = h < 0;
    if (top_down) h = -h;
    if (w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM || (uint64_t)w * (uint64_t)h > IMG_MAX_PIXELS) {
        fail(err, "Clipboard DIB dimensions out of range");
        return NULL;
    }
    /* palette */
    uint8_t pal[256][4];
    unsigned npal = 0;
    if (bpp <= 8) {
        npal = clr_used && clr_used <= (1u << bpp) ? clr_used : 1u << bpp;
        size_t esz = core ? 3 : 4;
        if (n < off + (size_t)npal * esz) {
            fail(err, "Clipboard DIB truncated in its palette");
            return NULL;
        }
        for (unsigned i = 0; i < npal; i++) {
            const uint8_t *e = d + off + i * esz;
            pal[i][0] = e[2], pal[i][1] = e[1], pal[i][2] = e[0], pal[i][3] = 255;
        }
        off += (size_t)npal * esz;
    } else if (!core && clr_used && comp == BI_RGB_) {
        /* an optional color table for >8-bit pixels: skipped when it fits */
        size_t skip = (size_t)clr_used * 4;
        if (clr_used <= 256 && n >= off + skip) off += skip;
    }
    uint64_t stride = (((uint64_t)w * bpp + 31) / 32) * 4;
    if (off > n || stride * (uint64_t)h > (uint64_t)(n - off)) {
        fail(err, "Clipboard DIB truncated in its pixels");
        return NULL;
    }
    if (!have_masks) {
        if (bpp == 16) masks[0] = 0x7C00, masks[1] = 0x03E0, masks[2] = 0x001F;
        else if (bpp == 32) masks[0] = 0x00FF0000, masks[1] = 0x0000FF00, masks[2] = 0x000000FF;
    }
    chan_t cr = chan(masks[0]), cg = chan(masks[1]), cb = chan(masks[2]), ca = chan(masks[3]);
    /* 32-bit pixels with no alpha mask: the fourth byte is alpha only when some pixel sets it (screenshots often
     * leave it 0 everywhere, which would read as fully transparent) */
    bool byte_alpha = false;
    if (bpp == 32 && !masks[3] && comp == BI_RGB_) {
        for (int64_t y = 0; y < h && !byte_alpha; y++) {
            const uint8_t *row = d + off + (size_t)(stride * (uint64_t)y);
            for (int64_t x = 0; x < w; x++)
                if (row[x * 4 + 3]) {
                    byte_alpha = true;
                    break;
                }
        }
    }
    img_t im;
    if (img_alloc(&im, (int)w, (int)h) != 0) {
        fail(err, "Clipboard DIB too large");
        return NULL;
    }
    bool any_alpha = false;
    for (int64_t y = 0; y < h; y++) {
        const uint8_t *row = d + off + (size_t)(stride * (uint64_t)(top_down ? y : h - 1 - y));
        uint8_t *o = im.px + (size_t)y * (size_t)w * 4;
        for (int64_t x = 0; x < w; x++, o += 4) {
            if (bpp <= 8) {
                unsigned idx;
                if (bpp == 8) idx = row[x];
                else if (bpp == 4) idx = (row[x / 2] >> (x % 2 ? 0 : 4)) & 0xF;
                else idx = (row[x / 8] >> (7 - x % 8)) & 1;
                if (idx >= npal) idx = 0;
                memcpy(o, pal[idx], 4);
            } else if (bpp == 24) {
                const uint8_t *p = row + x * 3;
                o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = 255;
            } else {
                uint32_t v = bpp == 16 ? rd16(row + x * 2) : rd32(row + x * 4);
                o[0] = chan_get(&cr, v, 0), o[1] = chan_get(&cg, v, 0), o[2] = chan_get(&cb, v, 0);
                o[3] = byte_alpha ? row[x * 4 + 3] : chan_get(&ca, v, 255);
                if (masks[3] && o[3]) any_alpha = true;
            }
        }
    }
    /* an alpha mask whose channel is 0 everywhere is unused, not a transparent image */
    if (masks[3] && !any_alpha)
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) im.px[i * 4 + 3] = 255;
    size_t len = 0;
    uint8_t *png = img_png_encode(&im, 6, &len);
    img_free(&im);
    if (!png) {
        fail(err, "Failed to encode clipboard image");
        return NULL;
    }
    *out_len = len;
    return png;
}
