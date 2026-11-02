/* img.c - raster helpers shared by the codecs: header sniffing (port of pi-utils parseImageMetadata),
 * allocation, decode dispatch, premultiplication and separable resampling. */
#include "img/img.h"
#include "core/core.h"
#include <math.h>
#include <strings.h>

static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rd16be(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static bool magic(const uint8_t *d, size_t n, size_t off, const char *m, size_t mn) {
    return n >= off + mn && !memcmp(d + off, m, mn);
}

static bool sniff_png(const uint8_t *d, size_t n, img_meta_t *m) {
    if (!magic(d, n, 0, "\x89PNG\r\n\x1a\n", 8)) return false;
    m->fmt = IMG_FMT_PNG;
    if (!magic(d, n, 12, "IHDR", 4) || n < 26) return true;
    m->w = (int)MIN(rd32be(d + 16), (uint32_t)INT32_MAX);
    m->h = (int)MIN(rd32be(d + 20), (uint32_t)INT32_MAX);
    switch (d[25]) {
    case 0: m->channels = 1, m->has_alpha = 0; break;
    case 2: m->channels = 3, m->has_alpha = 0; break;
    case 3: m->channels = 3; break;
    case 4: m->channels = 2, m->has_alpha = 1; break;
    case 6: m->channels = 4, m->has_alpha = 1; break;
    default: break;
    }
    return true;
}

static bool sniff_jpeg(const uint8_t *d, size_t n, img_meta_t *m) {
    if (!magic(d, n, 0, "\xff\xd8\xff", 3)) return false;
    m->fmt = IMG_FMT_JPEG;
    size_t off = 2;
    while (off + 9 < n) {
        if (d[off] != 0xff) {
            off++;
            continue;
        }
        size_t mo = off + 1;
        while (mo < n && d[mo] == 0xff) mo++;
        if (mo >= n) break;
        uint8_t mk = d[mo];
        size_t so = mo + 1;
        if (mk == 0xd8 || mk == 0xd9 || mk == 0x01 || (mk >= 0xd0 && mk <= 0xd7)) {
            off = so;
            continue;
        }
        if (so + 1 >= n) break;
        uint16_t len = rd16be(d + so);
        if (len < 2) break;
        if (mk >= 0xc0 && mk <= 0xcf && mk != 0xc4 && mk != 0xc8 && mk != 0xcc) {
            if (so + 7 >= n) break;
            m->h = rd16be(d + so + 3);
            m->w = rd16be(d + so + 5);
            m->channels = d[so + 7];
            m->has_alpha = 0;
            return true;
        }
        off = so + len;
    }
    return true;
}

static bool sniff_gif(const uint8_t *d, size_t n, img_meta_t *m) {
    if (!magic(d, n, 0, "GIF87a", 6) && !magic(d, n, 0, "GIF89a", 6)) return false;
    m->fmt = IMG_FMT_GIF;
    if (n < 10) return true;
    m->w = d[6] | d[7] << 8;
    m->h = d[8] | d[9] << 8;
    m->channels = 3;
    return true;
}

static bool sniff_webp(const uint8_t *d, size_t n, img_meta_t *m) {
    if (!magic(d, n, 0, "RIFF", 4) || !magic(d, n, 8, "WEBP", 4)) return false;
    m->fmt = IMG_FMT_WEBP;
    if (n < 30) return true;
    if (magic(d, n, 12, "VP8X", 4)) {
        int a = (d[20] & 0x10) != 0;
        m->w = (d[24] | d[25] << 8 | d[26] << 16) + 1;
        m->h = (d[27] | d[28] << 8 | d[29] << 16) + 1;
        m->channels = a ? 4 : 3;
        m->has_alpha = a;
    } else if (magic(d, n, 12, "VP8L", 4)) {
        uint32_t bits = (uint32_t)d[21] | (uint32_t)d[22] << 8 | (uint32_t)d[23] << 16 | (uint32_t)d[24] << 24;
        m->w = (int)(bits & 0x3fff) + 1;
        m->h = (int)((bits >> 14) & 0x3fff) + 1;
        m->has_alpha = (bits >> 28) & 1;
        m->channels = m->has_alpha ? 4 : 3;
    } else if (magic(d, n, 12, "VP8 ", 4)) {
        m->w = (d[26] | d[27] << 8) & 0x3fff;
        m->h = (d[28] | d[29] << 8) & 0x3fff;
        m->channels = 3;
        m->has_alpha = 0;
    }
    return true;
}

bool img_sniff(const uint8_t *d, size_t n, img_meta_t *m) {
    img_meta_t z = {IMG_FMT_NONE, 0, 0, 0, -1};
    *m = z;
    if (!d) return false;
    return sniff_png(d, n, m) || sniff_jpeg(d, n, m) || sniff_gif(d, n, m) || sniff_webp(d, n, m);
}

const char *img_fmt_mime(img_fmt_t f) {
    switch (f) {
    case IMG_FMT_PNG: return "image/png";
    case IMG_FMT_JPEG: return "image/jpeg";
    case IMG_FMT_GIF: return "image/gif";
    case IMG_FMT_WEBP: return "image/webp";
    default: return NULL;
    }
}

img_fmt_t img_fmt_from_mime(const char *mime) {
    if (!mime) return IMG_FMT_NONE;
    if (!strcasecmp(mime, "image/png")) return IMG_FMT_PNG;
    if (!strcasecmp(mime, "image/jpeg") || !strcasecmp(mime, "image/jpg")) return IMG_FMT_JPEG;
    if (!strcasecmp(mime, "image/gif")) return IMG_FMT_GIF;
    if (!strcasecmp(mime, "image/webp")) return IMG_FMT_WEBP;
    return IMG_FMT_NONE;
}

int img_alloc(img_t *im, int w, int h) {
    im->w = im->h = 0;
    im->px = NULL;
    if (w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM) return -1;
    if ((uint64_t)w * (uint64_t)h > IMG_MAX_PIXELS) return -1;
    im->px = xcalloc((size_t)w * (size_t)h, 4);
    im->w = w;
    im->h = h;
    return 0;
}

void img_free(img_t *im) {
    if (!im) return;
    free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

bool img_translucent(const img_t *im) {
    size_t n = (size_t)im->w * im->h * 4;
    for (size_t i = 3; i < n; i += 4)
        if (im->px[i] != 255) return true;
    return false;
}

int img_decode(const uint8_t *d, size_t n, img_t *out, img_fmt_t *fmt, char **err) {
    img_meta_t m;
    out->w = out->h = 0;
    out->px = NULL;
    if (err) *err = NULL;
    if (!img_sniff(d, n, &m)) {
        if (fmt) *fmt = IMG_FMT_NONE;
        if (err) *err = xstrdup("unsupported image format");
        return -1;
    }
    if (fmt) *fmt = m.fmt;
    switch (m.fmt) {
    case IMG_FMT_PNG: return img_png_decode(d, n, out, err);
    case IMG_FMT_JPEG: return img_jpeg_decode(d, n, out, err);
    case IMG_FMT_GIF: return img_gif_decode(d, n, out, err);
    case IMG_FMT_WEBP: return img_webp_decode(d, n, out, err);
    default: break;
    }
    if (err) *err = xstrdup("unsupported image format");
    return -1;
}

/* round(c * a / 255) */
void img_premultiply(img_t *im) {
    size_t n = (size_t)im->w * im->h * 4;
    for (size_t i = 0; i < n; i += 4) {
        unsigned a = im->px[i + 3];
        if (a == 255) continue;
        for (int c = 0; c < 3; c++) im->px[i + c] = (uint8_t)((im->px[i + c] * a + 127) / 255);
    }
}

/* min(255, round(c * 255 / a)); a channel the kernel left above alpha clamps to full */
void img_unpremultiply(img_t *im) {
    size_t n = (size_t)im->w * im->h * 4;
    for (size_t i = 0; i < n; i += 4) {
        unsigned a = im->px[i + 3];
        if (a == 255) continue;
        for (int c = 0; c < 3; c++) {
            unsigned v = a == 0 ? 0 : (im->px[i + c] * 255u + a / 2) / a;
            im->px[i + c] = (uint8_t)MIN(255u, v);
        }
    }
}

/* ---------- separable resampling ---------- */

#define RS_BITS 22

typedef struct {
    int *start, *count; /* per output index */
    int32_t *w;         /* count weights per output, stride ksize */
    int ksize;
} rs_coef_t;

static double tri(double x) {
    x = fabs(x);
    return x < 1.0 ? 1.0 - x : 0.0;
}

/* Weights for mapping `in` samples onto `out` samples (pixel centers aligned, PIL-style). */
static void rs_coef(int in, int out, rs_coef_t *c) {
    double scale = (double)in / out;
    double fscale = scale > 1.0 ? scale : 1.0;
    double support = 1.0 * fscale;
    int ksize = (int)ceil(support) * 2 + 1;
    c->ksize = ksize;
    c->start = xmalloc(sizeof(int) * out);
    c->count = xmalloc(sizeof(int) * out);
    c->w = xcalloc((size_t)out * ksize, sizeof(int32_t));
    double *tmp = xmalloc(sizeof(double) * ksize);
    for (int o = 0; o < out; o++) {
        double center = (o + 0.5) * scale;
        int lo = (int)floor(center - support), hi = (int)ceil(center + support);
        if (lo < 0) lo = 0;
        if (hi > in) hi = in;
        int cnt = hi - lo;
        if (cnt > ksize) cnt = ksize;
        double sum = 0;
        for (int k = 0; k < cnt; k++) {
            double v = tri((lo + k + 0.5 - center) / fscale);
            tmp[k] = v;
            sum += v;
        }
        if (sum <= 0) { /* degenerate: nearest sample */
            int nn = (int)center;
            if (nn >= in) nn = in - 1;
            if (nn < 0) nn = 0;
            lo = nn, cnt = 1, tmp[0] = 1, sum = 1;
        }
        c->start[o] = lo;
        c->count[o] = cnt;
        for (int k = 0; k < cnt; k++) c->w[(size_t)o * ksize + k] = (int32_t)lround(tmp[k] / sum * (1 << RS_BITS));
    }
    free(tmp);
}

static void rs_coef_free(rs_coef_t *c) {
    free(c->start);
    free(c->count);
    free(c->w);
}

static inline uint8_t rs_clip(int64_t v) {
    v = (v + (1 << (RS_BITS - 1))) >> RS_BITS;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

int img_resize(const img_t *src, int w, int h, img_t *dst) {
    if (!src || !src->px || src->w <= 0 || src->h <= 0) return -1;
    if (img_alloc(dst, w, h) < 0) return -1;
    if (w == src->w && h == src->h) {
        memcpy(dst->px, src->px, (size_t)w * h * 4);
        return 0;
    }
    /* horizontal pass into an 8-bit intermediate of src->h rows */
    const img_t *hsrc = src;
    img_t mid = {0};
    if (w != src->w) {
        rs_coef_t c;
        rs_coef(src->w, w, &c);
        mid.w = w, mid.h = src->h;
        mid.px = xmalloc((size_t)w * src->h * 4);
        for (int y = 0; y < src->h; y++) {
            const uint8_t *row = src->px + (size_t)y * src->w * 4;
            uint8_t *orow = mid.px + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                int64_t acc[4] = {0, 0, 0, 0};
                const int32_t *k = c.w + (size_t)x * c.ksize;
                const uint8_t *p = row + (size_t)c.start[x] * 4;
                for (int i = 0; i < c.count[x]; i++, p += 4)
                    for (int ch = 0; ch < 4; ch++) acc[ch] += (int64_t)p[ch] * k[i];
                for (int ch = 0; ch < 4; ch++) orow[x * 4 + ch] = rs_clip(acc[ch]);
            }
        }
        rs_coef_free(&c);
        hsrc = &mid;
    }
    if (h != hsrc->h) {
        rs_coef_t c;
        rs_coef(hsrc->h, h, &c);
        size_t stride = (size_t)w * 4;
        int64_t *acc = xmalloc(sizeof(int64_t) * stride);
        for (int y = 0; y < h; y++) {
            memset(acc, 0, sizeof(int64_t) * stride);
            const int32_t *k = c.w + (size_t)y * c.ksize;
            for (int i = 0; i < c.count[y]; i++) {
                const uint8_t *row = hsrc->px + (size_t)(c.start[y] + i) * stride;
                int32_t kw = k[i];
                for (size_t x = 0; x < stride; x++) acc[x] += (int64_t)row[x] * kw;
            }
            uint8_t *orow = dst->px + (size_t)y * stride;
            for (size_t x = 0; x < stride; x++) orow[x] = rs_clip(acc[x]);
        }
        free(acc);
        rs_coef_free(&c);
    } else {
        memcpy(dst->px, hsrc->px, (size_t)w * h * 4);
    }
    free(mid.px);
    return 0;
}
