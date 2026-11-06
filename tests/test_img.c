/* test_img.c - PNG / GIF codecs, header sniffing, resampling, premultiplication.
 * Fixtures come from tests/img_fixtures.py (Python, test time only). */
#include "core/core.h"
#include "core/fs.h"
#include "img/img.h"
#include "img/imgpipe.h"
#include <dirent.h>
#include <math.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

static char DIR_[512];

static uint8_t *slurp(const char *name, size_t *n) {
    char *p = xasprintf("%s/%s", DIR_, name);
    char *d = fs_read_limit(p, 64u << 20, n, NULL);
    free(p);
    return (uint8_t *)d;
}

static bool load_ref(const char *base, img_t *ref) {
    char *nm = xasprintf("%s.rgba", base);
    size_t n;
    uint8_t *d = slurp(nm, &n);
    free(nm);
    if (!d || n < 8) return free(d), false;
    int w = (int)(d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24), h = (int)(d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24);
    if (img_alloc(ref, w, h) < 0 || n != 8 + (size_t)w * h * 4) return free(d), false;
    memcpy(ref->px, d + 8, (size_t)w * h * 4);
    free(d);
    return true;
}

/* exact comparison (tol 0) or max-abs tolerance */
static void cmp_file(const char *file, int tol) {
    size_t n;
    uint8_t *d = slurp(file, &n);
    CHECK(d, "missing fixture %s", file);
    if (!d) return;
    char *base = xstrdup(file);
    *strrchr(base, '.') = 0;
    img_t ref = {0}, im = {0};
    CHECK(load_ref(base, &ref), "missing reference %s", base);
    char *err = NULL;
    img_fmt_t fmt;
    int rc = img_decode(d, n, &im, &fmt, &err);
    CHECK(rc == 0, "%s: decode failed: %s", file, err ? err : "?");
    if (rc == 0 && ref.px) {
        CHECK(im.w == ref.w && im.h == ref.h, "%s: size %dx%d want %dx%d", file, im.w, im.h, ref.w, ref.h);
        if (im.w == ref.w && im.h == ref.h) {
            int maxd = 0;
            size_t at = 0;
            for (size_t i = 0; i < (size_t)im.w * im.h * 4; i++) {
                int dd = abs(im.px[i] - ref.px[i]);
                if (dd > maxd) maxd = dd, at = i;
            }
            CHECK(maxd <= tol, "%s: max diff %d at px %zu ch %zu (got %d want %d)", file, maxd, at / 4, at % 4, im.px[at], ref.px[at]);
        }
    }
    free(err);
    img_free(&im);
    img_free(&ref);
    free(base);
    free(d);
}

static void test_fixtures(void) {
    DIR *dir = opendir(DIR_);
    CHECK(dir, "no fixture dir");
    if (!dir) return;
    struct dirent *e;
    int npng = 0, ngif = 0;
    while ((e = readdir(dir))) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (!strncmp(nm, "bad_", 4)) continue;
        if (l > 4 && !strcmp(nm + l - 4, ".png")) cmp_file(nm, 0), npng++;
        else if (l > 4 && !strcmp(nm + l - 4, ".gif")) cmp_file(nm, 0), ngif++;
    }
    closedir(dir);
    CHECK(npng >= 25 && ngif >= 5, "fixture count png=%d gif=%d", npng, ngif);
}

static void test_bad(void) {
    const char *bad[] = {"bad_png_trunc.png", "bad_png_corrupt.png", "bad_png_hugedim.png"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        size_t n;
        uint8_t *d = slurp(bad[i], &n);
        img_t im = {0};
        char *err = NULL;
        CHECK(d && img_decode(d, n, &im, NULL, &err) < 0 && err, "%s should fail", bad[i]);
        free(err);
        img_free(&im);
        free(d);
    }
    /* fuzz: byte flips and truncations over valid files must fail or succeed cleanly */
    const char *src[] = {"png_rgba8.png", "png_i_rgba8_33x17.png", "png_p8_trns.png", "gif_basic.gif", "gif_interlaced.gif", "gif_anim.gif"};
    uint32_t seed = 12345;
    for (size_t s = 0; s < sizeof src / sizeof *src; s++) {
        size_t n;
        uint8_t *d = slurp(src[s], &n);
        if (!d) continue;
        uint8_t *m = xmalloc(n);
        for (int it = 0; it < 400; it++) {
            memcpy(m, d, n);
            size_t len = n;
            if (it % 3 == 0) len = (size_t)(it * 97) % n;
            else
                for (int k = 0; k < 1 + it % 7; k++) {
                    seed = seed * 1103515245 + 12345;
                    m[(seed >> 8) % n] ^= (uint8_t)(1 + (seed >> 3) % 255);
                }
            img_t im = {0};
            char *err = NULL;
            if (img_decode(m, len, &im, NULL, &err) == 0) CHECK(im.px && im.w > 0 && im.h > 0, "bogus success");
            free(err);
            img_free(&im);
        }
        free(m);
        free(d);
    }
}

static void test_sniff(void) {
    size_t n;
    uint8_t *d = slurp("png_rgba8.png", &n);
    img_meta_t m;
    CHECK(img_sniff(d, n, &m) && m.fmt == IMG_FMT_PNG && m.w == 33 && m.h == 21 && m.channels == 4 && m.has_alpha == 1, "png sniff");
    free(d);
    d = slurp("png_p8_trns.png", &n);
    CHECK(img_sniff(d, n, &m) && m.channels == 3 && m.has_alpha == -1, "palette sniff");
    free(d);
    d = slurp("jpg_420.jpg", &n);
    CHECK(img_sniff(d, n, &m) && m.fmt == IMG_FMT_JPEG && m.w == 97 && m.h == 61 && m.channels == 3 && m.has_alpha == 0, "jpeg sniff %d %d", m.w, m.h);
    free(d);
    d = slurp("gif_basic.gif", &n);
    CHECK(img_sniff(d, n, &m) && m.fmt == IMG_FMT_GIF && m.w == 97 && m.h == 61, "gif sniff");
    free(d);
    const char *wp[] = {"webp_lossy.webp", "webp_lossless.webp", "webp_lossy_alpha.webp"};
    for (int i = 0; i < 3; i++) {
        d = slurp(wp[i], &n);
        CHECK(img_sniff(d, n, &m) && m.fmt == IMG_FMT_WEBP && m.w == 203 && m.h == 129, "webp sniff %s %dx%d", wp[i], m.w, m.h);
        CHECK(m.has_alpha == (i == 2), "webp alpha %s", wp[i]);
        free(d);
    }
    CHECK(!img_sniff((const uint8_t *)"hello world", 11, &m) && m.fmt == IMG_FMT_NONE, "text sniff");
    CHECK(img_sniff((const uint8_t *)"\x89PNG\r\n\x1a\n", 8, &m) && m.fmt == IMG_FMT_PNG && m.w == 0, "short png");
    CHECK(img_fmt_from_mime("IMAGE/JPEG") == IMG_FMT_JPEG && !strcmp(img_fmt_mime(IMG_FMT_WEBP), "image/webp"), "mime");
}

static void test_png_roundtrip(void) {
    img_t ref = {0};
    CHECK(load_ref("src_ui", &ref), "src_ui");
    for (int alpha = 0; alpha < 2; alpha++) {
        if (alpha)
            for (size_t i = 3; i < (size_t)ref.w * ref.h * 4; i += 4 * 7) ref.px[i] = (uint8_t)(i % 251);
        size_t n;
        uint8_t *png = img_png_encode(&ref, 6, &n);
        CHECK(png && n > 50, "encode");
        img_meta_t m;
        CHECK(img_sniff(png, n, &m) && m.channels == (alpha ? 4 : 3), "encoded channels");
        img_t back = {0};
        CHECK(img_png_decode(png, n, &back, NULL) == 0 && back.w == ref.w && !memcmp(back.px, ref.px, (size_t)ref.w * ref.h * 4), "roundtrip alpha=%d", alpha);
        img_free(&back);
        /* Pillow must read it too */
        char *p = xasprintf("%s/rt%d.png", DIR_, alpha);
        fs_write(p, (const char *)png, n);
        char *cmd = xasprintf("python3 -I -c \"from PIL import Image; im=Image.open('%s'); im.load(); assert im.size==(%d,%d)\"", p, ref.w, ref.h);
        CHECK(system(cmd) == 0, "pillow reads encoded png");
        free(cmd), free(p), free(png);
    }
    img_free(&ref);
    img_t one = {0};
    img_alloc(&one, 1, 1);
    memset(one.px, 200, 4);
    size_t n;
    uint8_t *png = img_png_encode(&one, 0, &n);
    img_t back = {0};
    CHECK(png && img_png_decode(png, n, &back, NULL) == 0 && back.w == 1 && back.px[0] == 200, "1x1");
    free(png), img_free(&back), img_free(&one);
}

static void test_resize(void) {
    img_t src = {0}, dst = {0};
    img_alloc(&src, 100, 50);
    for (int y = 0; y < 50; y++)
        for (int x = 0; x < 100; x++) {
            uint8_t *p = src.px + ((size_t)y * 100 + x) * 4;
            p[0] = (uint8_t)(x * 255 / 99), p[1] = (uint8_t)(y * 5), p[2] = 77, p[3] = 255;
        }
    CHECK(img_resize(&src, 40, 20, &dst) == 0 && dst.w == 40 && dst.h == 20, "down");
    /* gradients stay monotonic, constants stay constant */
    bool mono = true, flat = true;
    for (int x = 1; x < 40; x++) mono &= dst.px[x * 4] >= dst.px[(x - 1) * 4];
    for (size_t i = 0; i < 40 * 20; i++) flat &= dst.px[i * 4 + 2] == 77 && dst.px[i * 4 + 3] == 255;
    CHECK(mono && flat, "down quality");
    img_free(&dst);
    CHECK(img_resize(&src, 333, 200, &dst) == 0 && dst.w == 333, "up");
    flat = true;
    for (size_t i = 0; i < 333 * 200; i++) flat &= dst.px[i * 4 + 2] == 77;
    CHECK(flat, "up flat");
    img_free(&dst);
    CHECK(img_resize(&src, 1, 1, &dst) == 0 && abs(dst.px[0] - 127) < 4, "to 1x1 avg %d", dst.px[0]);
    img_free(&dst);
    /* 2x2 box average when halving */
    img_t c = {0};
    img_alloc(&c, 2, 2);
    uint8_t vals[4] = {0, 100, 200, 40};
    for (int i = 0; i < 4; i++) c.px[i * 4] = vals[i], c.px[i * 4 + 3] = 255;
    CHECK(img_resize(&c, 1, 1, &dst) == 0 && abs(dst.px[0] - 85) <= 1, "box %d", dst.px[0]);
    img_free(&dst), img_free(&c), img_free(&src);
    /* premultiply round trip keeps opaque pixels and zeroes fully transparent color */
    img_alloc(&c, 2, 1);
    uint8_t px[8] = {200, 100, 50, 255, 200, 100, 50, 0};
    memcpy(c.px, px, 8);
    img_premultiply(&c);
    CHECK(c.px[0] == 200 && c.px[4] == 0, "premul");
    img_unpremultiply(&c);
    CHECK(c.px[1] == 100 && c.px[5] == 0, "unpremul");
    CHECK(img_translucent(&c), "translucent");
    img_free(&c);
    CHECK(img_alloc(&c, 70000, 1) < 0 && img_alloc(&c, 20000, 20000) < 0, "caps");
}

static uint8_t *gradient_png(int w, int h, bool alpha, size_t *n) {
    img_t im = {0};
    img_alloc(&im, w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = im.px + ((size_t)y * w + x) * 4;
            p[0] = (uint8_t)(x * 13 + y), p[1] = (uint8_t)(y * 7), p[2] = (uint8_t)((x ^ y) * 3), p[3] = alpha ? (uint8_t)(x % 256) : 255;
        }
    uint8_t *png = img_png_encode(&im, 1, n);
    img_free(&im);
    return png;
}

/* resizeImage / formatDimensionNote / imageDecodeFailureReason / loadImageInput ports */
static void test_pipeline(void) {
    size_t n;
    /* oversized: fits 1568 on the long edge, keeps aspect (Math.round), reports a dimension note */
    uint8_t *png = gradient_png(3200, 1800, false, &n);
    img_resized_t r;
    CHECK(img_resize_image(png, n, "image/png", NULL, &r, NULL) == 0 && r.was_resized && r.w == 1568 && r.h == 882 &&
              r.orig_w == 3200 && r.len <= 500 * 1024,
          "resize big %dx%d len %zu", r.w, r.h, r.len);
    char *note = img_dimension_note(&r);
    CHECK(note && !strcmp(note, "[Image: original 3200x1800, displayed at 1568x882. Multiply coordinates by 2.04 to map to "
                                 "original image.]"),
          "note %s", note ? note : "(null)");
    free(note);
    img_meta_t m;
    CHECK(img_sniff(r.buf, r.len, &m) && m.w == 1568 && m.h == 882, "resized decodable header");
    img_resized_free(&r);
    free(png);
    /* tiny: lifted toward the 200px floor without distorting (150x100 -> 300x200), scale note 0.50 */
    png = gradient_png(150, 100, false, &n);
    CHECK(img_resize_image(png, n, NULL, NULL, &r, NULL) == 0 && r.w == 300 && r.h == 200, "upscale %dx%d", r.w, r.h);
    note = img_dimension_note(&r);
    CHECK(note && strstr(note, "by 0.50 to map"), "upscale note");
    free(note);
    img_resized_free(&r);
    free(png);
    /* fast path: in bounds and under budget/4 -> untouched bytes, same mime */
    png = gradient_png(400, 300, false, &n);
    { /* omp image-resize.test: a strip no uniform scale can lift over the floor is padded, not stretched */
        img_t strip = {.w = 4000, .h = 10, .px = xcalloc((size_t)4000 * 10, 4)};
        memset(strip.px, 0x80, (size_t)4000 * 10 * 4);
        size_t sl = 0;
        uint8_t *sp = img_png_encode(&strip, -1, &sl);
        img_resized_t t = {0};
        CHECK(img_resize_image(sp, sl, NULL, NULL, &t, NULL) == 0 && t.padded && t.w == 1568 && t.h == 200 && t.content_w == 1568 && t.content_h == 4 && t.pad_top == 98,
              "padded strip");
        char *tn = img_dimension_note(&t);
        CHECK(tn && !strcmp(tn, "[Image: original 4000x10, displayed at 1568x200 with padding. Subtract padding offset (0, 98), then multiply x by 2.55 and y by 2.50 to map to original image coordinates.]"), "padded note: %s", tn ? tn : "");
        free(tn);
        img_resized_free(&t);
        free(sp);
        img_free(&strip);
    }
    CHECK(img_resize_image(png, n, NULL, NULL, &r, NULL) == 0 && !r.was_resized && r.len == n && !memcmp(r.buf, png, n) &&
              !strcmp(r.mime, "image/png") && !img_dimension_note(&r),
          "fast path");
    img_resized_free(&r);
    free(png);
    /* translucent: never JPEG */
    png = gradient_png(2000, 300, true, &n);
    CHECK(img_resize_image(png, n, NULL, NULL, &r, NULL) == 0 && strcmp(r.mime, "image/jpeg") && r.w <= 1568 &&
              abs(r.w * 300 - r.h * 2000) <= 2000,
          "translucent %s %dx%d", r.mime, r.w, r.h);
    img_resized_free(&r);
    free(png);
    /* undecodable: original bytes back, decode_failed; with WebP exclusion and no type info it throws */
    const uint8_t junk[] = "not an image at all";
    CHECK(img_resize_image(junk, sizeof junk, "image/png", NULL, &r, NULL) == 0 && r.decode_failed && !strcmp(r.mime, "image/png"),
          "undecodable");
    img_resized_free(&r);
    img_resize_opts_t ex = {.exclude_webp = 1};
    char *err = NULL;
    CHECK(img_resize_image(junk, sizeof junk, NULL, &ex, &r, &err) < 0 && err, "exclude webp throws");
    free(err);
    /* decode verdicts */
    CHECK(img_b64_failure("", 0) && !strcmp(img_b64_failure("", 0), "empty image data"), "empty b64");
    CHECK(img_b64_failure("ab$=", 4), "bad char");
    CHECK(img_b64_failure("QQ=", 3), "bad padding"); /* "QQ==" required */
    CHECK(img_b64_failure("QR==", 4), "non-zero trailing bits");
    CHECK(!img_b64_failure("QQ==", 4) && !img_b64_failure("QQ", 2), "valid / unpadded");
    png = gradient_png(64, 64, false, &n);
    char *b64 = base64_encode(png, n);
    char *why = img_decode_failure_reason(b64, "image/png");
    CHECK(!why, "decodes");
    free(why);
    why = img_decode_failure_reason(b64, "image/jpeg");
    CHECK(why && !strcmp(why, "declared image/jpeg but contains image/png"), "mismatch %s", why ? why : "");
    free(why);
    b64[strlen(b64) / 2] = 0; /* truncated stream, still valid base64 */
    size_t bl = strlen(b64);
    b64[bl - bl % 4] = 0;
    why = img_decode_failure_reason(b64, "image/png");
    CHECK(why && !strcmp(why, "Image: decode failed"), "truncated %s", why ? why : "");
    free(why);
    free(b64);
    /* loadImageInput messages */
    img_loaded_t l;
    CHECK(img_load_input(png, n, "image/png", "/x/a.png", "Read image file", true, 0, &l, &err) == 0 &&
              str_starts(l.text_note, "Read image file [image/png]") &&
              strstr(l.text_note, "\n[Image: original 64x64, displayed at 200x200. Multiply coordinates by 0.32 to map to "
                                  "original image.]"),
          "load note %s", l.text_note ? l.text_note : "");
    img_loaded_free(&l);
    CHECK(img_load_input(png, n / 2, "image/png", "/x/a.png", "Read image file", true, 0, &l, &err) < 0 &&
              !strcmp(err, "/x/a.png is not a decodable image/png image: Image: decode failed"),
          "load undecodable %s", err ? err : "");
    free(err);
    free(png);
    char *fb = img_format_bytes(21u * 1024 * 1024 + 5);
    CHECK(!strcmp(fb, "21.0MB"), "formatBytes %s", fb);
    free(fb);
    fb = img_format_bytes(1536);
    CHECK(!strcmp(fb, "1.5KB"), "formatBytes KB %s", fb);
    free(fb);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(DIR_, sizeof DIR_, "%s/omc-img-test-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    char *cmd = xasprintf("python3 -I tests/img_fixtures.py '%s' >/dev/null", DIR_);
    if (system(cmd) != 0) {
        fprintf(stderr, "fixture generation failed\n");
        return 1;
    }
    free(cmd);
    test_sniff();
    test_fixtures();
    test_bad();
    test_png_roundtrip();
    test_resize();
    test_pipeline();
    cmd = xasprintf("rm -rf '%s'", DIR_);
    if (system(cmd)) {}
    free(cmd);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("img: all ok\n");
    return failures != 0;
}
