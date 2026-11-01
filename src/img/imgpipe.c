/* imgpipe.c - omp's model image pipeline (resizeImage, formatDimensionNote, imageDecodeFailureReason,
 * loadImageInput) on the in-tree codecs. See imgpipe.h. */
#include "img/imgpipe.h"
#include "core/core.h"
#include <math.h>
#include <pthread.h>
#include <strings.h>

#define DEFAULT_MAX_BYTES (500u * 1024)
#define DEFAULT_MIN_DIMENSION 200
#define DEFAULT_MAX_EDGE 1568
#define DEFAULT_JPEG_QUALITY 80

uint64_t img_hash(const void *d, size_t n) {
    const uint8_t *p = d;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

bool img_webp_excluded_by_env(void) {
    const char *names[] = {"OMP_NO_WEBP", "OMC_NO_WEBP"};
    for (int i = 0; i < 2; i++) {
        const char *v = getenv(names[i]);
        if (v && (!strcmp(v, "1") || !strcasecmp(v, "true"))) return true;
    }
    return false;
}

static long js_round(double x) { return (long)floor(x + 0.5); }

/* Number.prototype.toFixed: ties (on the exact binary value) round away from zero */
static char *js_to_fixed(double x, int digits) {
    char exact[128];
    snprintf(exact, sizeof exact, "%.40f", x);
    char *dot = strchr(exact, '.');
    bool tie = false;
    if (dot && (int)strlen(dot + 1) > digits && dot[1 + digits] == '5') {
        tie = true;
        for (const char *q = dot + 2 + digits; *q; q++)
            if (*q != '0') tie = false;
    }
    if (tie) x += (x < 0 ? -1 : 1) * pow(10, -digits) / 4; /* nudge off the tie, upward in magnitude */
    return xasprintf("%.*f", digits, x);
}

char *img_format_bytes(uint64_t n) {
    if (n < 1024) return xasprintf("%lluB", (unsigned long long)n);
    char *v;
    const char *u;
    if (n < 1024 * 1024) v = js_to_fixed(n / 1024.0, 1), u = "KB";
    else if (n < 1024ull * 1024 * 1024) v = js_to_fixed(n / (1024.0 * 1024), 1), u = "MB";
    else v = js_to_fixed(n / (1024.0 * 1024 * 1024), 1), u = "GB";
    char *r = xasprintf("%s%s", v, u);
    free(v);
    return r;
}

/* ---------- resizeImage ---------- */

typedef struct {
    uint8_t *buf;
    size_t len;
    const char *mime;
} cand_t;

static void cand_free(cand_t *c) {
    free(c->buf);
    c->buf = NULL;
    c->len = 0;
}

/* keep the smaller of *best and c (strictly smaller wins; earlier candidates win ties) */
static void keep_smallest(cand_t *best, cand_t *c) {
    if (!c->buf) return;
    if (!best->buf || c->len < best->len) {
        cand_free(best);
        *best = *c;
    } else {
        cand_free(c);
    }
    c->buf = NULL;
}

static bool png_may_have_alpha(const uint8_t *in, size_t n) {
    if (n >= 2 && in[0] == 0xff && in[1] == 0xd8) return false;
    if (n < 26 || memcmp(in, "\x89PNG\r\n\x1a\n", 8) || memcmp(in + 12, "IHDR", 4)) return true;
    uint8_t ct = in[25];
    if (ct == 4 || ct == 6) return true;
    for (size_t off = 33; off + 8 <= n;) {
        uint32_t len = (uint32_t)in[off] << 24 | (uint32_t)in[off + 1] << 16 | (uint32_t)in[off + 2] << 8 | in[off + 3];
        if (!memcmp(in + off + 4, "tRNS", 4)) return true;
        if (!memcmp(in + off + 4, "IDAT", 4)) return false;
        if ((uint64_t)off + 12 + len > n) break;
        off += 12 + (size_t)len;
    }
    return false;
}

/* readImageHeaderDimensions: PNG or JPEG headers only */
static bool header_dims(const uint8_t *in, size_t n, img_meta_t *m) {
    img_meta_t t;
    if (!img_sniff(in, n, &t) || (t.fmt != IMG_FMT_PNG && t.fmt != IMG_FMT_JPEG) || t.w <= 0 || t.h <= 0) return false;
    *m = t;
    return true;
}

/* lossy candidates at the raster's size: JPEG unless translucent, WebP unless excluded */
static cand_t encode_lossy(const img_t *im, int q, bool translucent, bool exclude_webp) {
    cand_t best = {0};
    if (!translucent) {
        cand_t j = {0};
        j.buf = img_jpeg_encode(im, q, &j.len);
        j.mime = "image/jpeg";
        keep_smallest(&best, &j);
    }
    if (!exclude_webp) {
        cand_t w = {0};
        w.buf = img_webp_encode(im, q, &w.len);
        w.mime = "image/webp";
        keep_smallest(&best, &w);
    }
    return best;
}

/* the source raster at w x h with straight alpha (premultiplied resampling for translucent sources) */
static int resized_raster(const img_t *src, bool translucent, int w, int h, img_t *out) {
    if (!translucent) return img_resize(src, w, h, out);
    img_t pm = {0};
    if (img_alloc(&pm, src->w, src->h) < 0) return -1;
    memcpy(pm.px, src->px, (size_t)src->w * src->h * 4);
    img_premultiply(&pm);
    int rc = img_resize(&pm, w, h, out);
    img_free(&pm);
    if (rc == 0) img_unpremultiply(out);
    return rc;
}

static void set_result(img_resized_t *r, cand_t *c, int ow, int oh, int w, int h) {
    r->buf = c->buf;
    r->len = c->len;
    r->mime = c->mime;
    r->orig_w = ow, r->orig_h = oh, r->w = w, r->h = h;
    r->was_resized = true;
    r->decode_failed = false;
    c->buf = NULL;
}

int img_resize_image(const uint8_t *in, size_t n, const char *caller_mime, const img_resize_opts_t *o, img_resized_t *out,
                     char **err) {
    memset(out, 0, sizeof *out);
    if (err) *err = NULL;
    img_resize_opts_t def = {0};
    if (!o) o = &def;
    bool exclude_webp = o->exclude_webp > 0 || (o->exclude_webp == 0 && img_webp_excluded_by_env());
    int max_w = o->max_w > 0 ? o->max_w : DEFAULT_MAX_EDGE, max_h = o->max_h > 0 ? o->max_h : DEFAULT_MAX_EDGE;
    size_t max_bytes = o->max_bytes ? o->max_bytes : DEFAULT_MAX_BYTES;
    int quality = o->jpeg_quality > 0 ? o->jpeg_quality : DEFAULT_JPEG_QUALITY;
    int min_dim = MIN(o->min_dim > 0 ? o->min_dim : DEFAULT_MIN_DIMENSION, MIN(max_w, max_h));

    img_meta_t meta;
    img_t src = {0};
    if (!img_sniff(in, n, &meta) || meta.w <= 0 || meta.h <= 0) goto undecodable;
    const char *source_mime = img_fmt_mime(meta.fmt);
    int ow = meta.w, oh = meta.h;
    /* fast path: in bounds and well under budget (1/4) */
    if (ow >= min_dim && oh >= min_dim && ow <= max_w && oh <= max_h && n <= max_bytes / 4 &&
        !(exclude_webp && meta.fmt == IMG_FMT_WEBP)) {
        out->buf = xmalloc(n ? n : 1);
        memcpy(out->buf, in, n);
        out->len = n;
        out->mime = source_mime;
        out->orig_w = out->w = ow;
        out->orig_h = out->h = oh;
        return 0;
    }
    if (img_decode(in, n, &src, NULL, NULL) < 0) goto undecodable;
    ow = src.w, oh = src.h;

    int tw = ow, th = oh;
    if (tw > max_w) {
        th = (int)js_round((double)th * max_w / tw);
        tw = max_w;
    }
    if (th > max_h) {
        tw = (int)js_round((double)tw * max_h / th);
        th = max_h;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (tw < min_dim || th < min_dim) {
        int short_edge = MIN(tw, th);
        double up = MIN((double)min_dim / short_edge, MIN((double)max_w / tw, (double)max_h / th));
        if (up > 1) {
            tw = (int)MIN((long)max_w, js_round(tw * up));
            th = (int)MIN((long)max_h, js_round(th * up));
        }
    }

    bool translucent = png_may_have_alpha(in, n) && img_translucent(&src);
    bool lossy = !(translucent && exclude_webp);

    img_t target = {0};
    if (resized_raster(&src, translucent, tw, th, &target) < 0) {
        img_free(&src);
        goto undecodable;
    }
    int fw = tw, fh = th;
    cand_t best = {0};
    best.buf = img_png_encode(&target, -1, &best.len);
    best.mime = "image/png";
    if (lossy) {
        cand_t l = encode_lossy(&target, quality, translucent, exclude_webp);
        keep_smallest(&best, &l);
    }
    static const int quality_steps[] = {70, 60, 50, 40};
    static const double scale_steps[] = {1.0, 0.75, 0.5, 0.35, 0.25};
    if (best.buf && best.len <= max_bytes) goto done;
    if (lossy)
        for (size_t i = 0; i < 4; i++) {
            cand_t l = encode_lossy(&target, quality_steps[i], translucent, exclude_webp);
            if (l.buf) cand_free(&best), best = l;
            if (best.buf && best.len <= max_bytes) goto done;
        }
    for (size_t s = 0; s < 5; s++) {
        double scale = scale_steps[s];
        if (!lossy && scale == 1.0) continue;
        int w = (int)js_round(tw * scale), h = (int)js_round(th * scale);
        if (w < 100 || h < 100) break;
        fw = w, fh = h;
        img_t sized = {0};
        if (scale == 1.0) {
            sized = target;
        } else if (resized_raster(&src, translucent, w, h, &sized) < 0) {
            break;
        }
        if (!lossy) {
            cand_t p = {0};
            p.buf = img_png_encode(&sized, -1, &p.len);
            p.mime = "image/png";
            if (p.buf) cand_free(&best), best = p;
            img_free(&sized);
            if (best.buf && best.len <= max_bytes) goto done;
            continue;
        }
        bool fit = false;
        for (size_t i = 0; i < 4 && !fit; i++) {
            cand_t l = encode_lossy(&sized, quality_steps[i], translucent, exclude_webp);
            if (l.buf) cand_free(&best), best = l;
            fit = best.buf && best.len <= max_bytes;
        }
        if (scale != 1.0) img_free(&sized);
        if (fit) goto done;
    }
done:
    img_free(&target);
    img_free(&src);
    if (!best.buf) goto undecodable_nofree;
    set_result(out, &best, ow, oh, fw, fh);
    return 0;

undecodable:
undecodable_nofree: {
    img_meta_t hd = {0};
    bool have_hd = header_dims(in, n, &hd);
    const char *fallback = caller_mime && *caller_mime ? caller_mime : have_hd ? img_fmt_mime(hd.fmt) : "application/octet-stream";
    if (exclude_webp && (!strcasecmp(fallback, "image/webp") || ((!caller_mime || !*caller_mime) && !have_hd))) {
        if (err) *err = xstrdup("resizeImage: failed to decode image and cannot honor excludeWebP for a WebP source");
        return -1;
    }
    out->buf = xmalloc(n ? n : 1);
    memcpy(out->buf, in, n);
    out->len = n;
    out->mime = fallback;
    out->orig_w = out->w = have_hd ? hd.w : 0;
    out->orig_h = out->h = have_hd ? hd.h : 0;
    out->decode_failed = true;
    return 0;
}
}

void img_resized_free(img_resized_t *r) {
    if (!r) return;
    free(r->buf);
    r->buf = NULL;
    r->len = 0;
}

char *img_dimension_note(const img_resized_t *r) {
    if (!r->was_resized || !r->orig_w || !r->orig_h || !r->w || !r->h) return NULL;
    if (r->w == r->orig_w && r->h == r->orig_h) return NULL;
    char *s = js_to_fixed((double)r->orig_w / r->w, 2);
    char *note = xasprintf("[Image: original %dx%d, displayed at %dx%d. Multiply coordinates by %s to map to original image.]",
                           r->orig_w, r->orig_h, r->w, r->h, s);
    free(s);
    return note;
}

/* ---------- decode verdicts ---------- */

const char *img_b64_failure(const char *b64, size_t len) {
    static const char *invalid = "invalid base64 image data";
    if (!b64 || len == 0) return "empty image data";
    size_t pad = 0;
    while (pad < 2 && len - pad > 0 && b64[len - 1 - pad] == '=') pad++;
    size_t enc = len - pad;
    if (enc == 0) return invalid;
    for (size_t i = 0; i < enc; i++) {
        char c = b64[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')) return invalid;
    }
    size_t rem = enc % 4;
    if (rem == 1 || (pad == 1 && rem != 3) || (pad == 2 && rem != 2)) return invalid;
    char lc = b64[enc - 1];
    int sext = lc >= 'A' && lc <= 'Z' ? lc - 'A' : lc >= 'a' && lc <= 'z' ? lc - 'a' + 26 : lc >= '0' && lc <= '9' ? lc - '0' + 52 : lc == '+' ? 62 : 63;
    if ((rem == 2 && (sext & 15)) || (rem == 3 && (sext & 3))) return invalid;
    return NULL;
}

#define VERDICT_SLOTS 512
typedef struct {
    uint64_t hash;
    size_t len;
    char mime[48];
    bool used;
    char *reason; /* NULL = decodes */
} verdict_t;
static verdict_t g_verdicts[VERDICT_SLOTS];
static pthread_mutex_t g_verdict_mu = PTHREAD_MUTEX_INITIALIZER;

static char *decode_failure_uncached(const char *b64, size_t blen, const char *mime) {
    const char *bad = img_b64_failure(b64, blen);
    if (bad) return xstrdup(bad);
    size_t n = 0;
    unsigned char *bytes = base64_decode(b64, blen, &n);
    if (!bytes || n == 0) {
        free(bytes);
        return xstrdup("invalid base64 image data");
    }
    img_meta_t m;
    if (img_sniff(bytes, n, &m)) {
        const char *det = img_fmt_mime(m.fmt);
        if (det && mime && strcasecmp(det, mime)) {
            free(bytes);
            return xasprintf("declared %s but contains %s", mime, det);
        }
    }
    img_t im = {0};
    int rc = img_decode(bytes, n, &im, NULL, NULL);
    img_free(&im);
    free(bytes);
    return rc == 0 ? NULL : xstrdup("Image: decode failed");
}

char *img_decode_failure_reason(const char *b64, const char *mime) {
    size_t blen = b64 ? strlen(b64) : 0;
    uint64_t h = img_hash(b64 ? b64 : "", blen);
    const char *mm = mime ? mime : "";
    size_t slot = (size_t)(h % VERDICT_SLOTS);
    pthread_mutex_lock(&g_verdict_mu);
    verdict_t *v = &g_verdicts[slot];
    if (v->used && v->hash == h && v->len == blen && !strcasecmp(v->mime, mm)) {
        char *r = v->reason ? xstrdup(v->reason) : NULL;
        pthread_mutex_unlock(&g_verdict_mu);
        return r;
    }
    pthread_mutex_unlock(&g_verdict_mu);
    char *reason = decode_failure_uncached(b64 ? b64 : "", blen, mime);
    pthread_mutex_lock(&g_verdict_mu);
    free(v->reason);
    v->used = true;
    v->hash = h;
    v->len = blen;
    snprintf(v->mime, sizeof v->mime, "%s", mm);
    v->reason = reason ? xstrdup(reason) : NULL;
    pthread_mutex_unlock(&g_verdict_mu);
    return reason;
}

/* ---------- loadImageInput ---------- */

int img_load_input(const uint8_t *bytes, size_t n, const char *mime, const char *resolved, const char *label_prefix,
                   bool auto_resize, int exclude_webp, img_loaded_t *out, char **err) {
    memset(out, 0, sizeof *out);
    if (err) *err = NULL;
    if (n > IMG_MAX_INPUT_BYTES) {
        char *a = img_format_bytes(n), *b = img_format_bytes(IMG_MAX_INPUT_BYTES);
        if (err) *err = xasprintf("Image file too large: %s exceeds %s limit.", a, b);
        free(a), free(b);
        return -1;
    }
    char *b64 = base64_encode(bytes, n);
    char *why = img_decode_failure_reason(b64, mime);
    if (why) {
        if (err) *err = xasprintf("%s is not a decodable %s image: %s", resolved ? resolved : "image", mime, why);
        free(why);
        free(b64);
        return -1;
    }
    out->mime = xstrdup(mime);
    out->data = b64;
    out->bytes = n;
    bool reencode_webp = exclude_webp > 0 && !strcasecmp(mime, "image/webp");
    if (auto_resize || reencode_webp) {
        img_resize_opts_t o = {.exclude_webp = exclude_webp > 0 ? 1 : 0};
        img_resized_t r;
        if (img_resize_image(bytes, n, mime, &o, &r, NULL) == 0) {
            if (r.buf != NULL && (r.was_resized || r.len != n || memcmp(r.buf, bytes, n))) {
                free(out->data);
                out->data = base64_encode(r.buf, r.len);
            }
            free(out->mime);
            out->mime = xstrdup(r.mime);
            out->bytes = r.len;
            out->dim_note = img_dimension_note(&r);
            img_resized_free(&r);
        }
    }
    sb_t t = SB_INIT;
    sb_appendf(&t, "%s [%s]", label_prefix, mime);
    if (strcmp(out->mime, mime)) sb_appendf(&t, " (displayed as %s)", out->mime);
    if (out->dim_note) sb_appendf(&t, "\n%s", out->dim_note);
    out->text_note = sb_detach(&t);
    return 0;
}

void img_loaded_free(img_loaded_t *l) {
    if (!l) return;
    free(l->mime);
    free(l->data);
    free(l->text_note);
    free(l->dim_note);
    memset(l, 0, sizeof *l);
}
