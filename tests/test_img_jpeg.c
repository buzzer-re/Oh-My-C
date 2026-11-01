/* test_img_jpeg.c - JPEG decoder (vs Pillow references), robustness against malformed input, encoder
 * round trips (own decoder + Pillow).
 *
 * Decode tolerance vs Pillow/libjpeg-turbo: mean absolute difference per channel <= 2.0 and max
 * absolute difference <= 40 (our float IDCT differs from libjpeg's islow by about one level; the
 * upsampling and color conversion mirror libjpeg's). */
#include "core/core.h"
#include "core/fs.h"
#include "img/img.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint8_t *img_jpeg_encode_ex(const img_t *im, int quality, int hs, int vs, int restart, size_t *len);

static char *DIR_;
static int fails;

#define CHECK(c, ...)                                                                                       \
    do {                                                                                                    \
        if (!(c)) {                                                                                         \
            fails++;                                                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            fprintf(stderr, __VA_ARGS__);                                                                   \
            fprintf(stderr, "\n");                                                                          \
        }                                                                                                   \
    } while (0)

static uint8_t *readf(const char *name, size_t *len) {
    char *p = xasprintf("%s/%s", DIR_, name);
    char *d = fs_read(p, len);
    free(p);
    return (uint8_t *)d;
}

static bool load_ref(const char *name, img_t *im) {
    size_t n;
    char *fn = xasprintf("%s.rgba", name);
    uint8_t *d = readf(fn, &n);
    free(fn);
    if (!d || n < 8) {
        free(d);
        return false;
    }
    uint32_t w = d[0] | d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    uint32_t h = d[4] | d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
    if (img_alloc(im, (int)w, (int)h) < 0 || n != 8 + (size_t)w * h * 4) {
        free(d);
        img_free(im);
        return false;
    }
    memcpy(im->px, d + 8, (size_t)w * h * 4);
    free(d);
    return true;
}

static void diff_stats(const img_t *a, const img_t *b, double *mean, int *maxd) {
    double sum[3] = {0, 0, 0};
    *maxd = 0;
    size_t np = (size_t)a->w * a->h;
    for (size_t i = 0; i < np; i++)
        for (int c = 0; c < 3; c++) {
            int d = abs(a->px[i * 4 + c] - b->px[i * 4 + c]);
            sum[c] += d;
            if (d > *maxd) *maxd = d;
        }
    *mean = 0;
    for (int c = 0; c < 3; c++) *mean = MAX(*mean, sum[c] / np);
}

static double psnr(const img_t *a, const img_t *b) {
    double se = 0;
    size_t np = (size_t)a->w * a->h;
    for (size_t i = 0; i < np; i++)
        for (int c = 0; c < 3; c++) {
            double d = a->px[i * 4 + c] - b->px[i * 4 + c];
            se += d * d;
        }
    double mse = se / (np * 3.0);
    return mse == 0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

/* decode <name>.jpg (or a given file name) and compare against <name>.rgba */
static void cmp_file(const char *jpg, const char *ref) {
    size_t n;
    uint8_t *d = readf(jpg, &n);
    CHECK(d, "missing fixture %s", jpg);
    if (!d) return;
    img_t im, rf;
    char *err = NULL;
    int rc = img_jpeg_decode(d, n, &im, &err);
    CHECK(rc == 0, "%s: decode failed: %s", jpg, err ? err : "?");
    free(err);
    free(d);
    if (rc) return;
    bool ok = load_ref(ref, &rf);
    CHECK(ok, "%s: missing reference", ref);
    if (!ok) {
        img_free(&im);
        return;
    }
    CHECK(im.w == rf.w && im.h == rf.h, "%s: size %dx%d vs %dx%d", jpg, im.w, im.h, rf.w, rf.h);
    if (im.w == rf.w && im.h == rf.h) {
        double mean;
        int maxd;
        diff_stats(&im, &rf, &mean, &maxd);
        bool alpha = true;
        for (size_t i = 3; i < (size_t)im.w * im.h * 4; i += 4) alpha &= im.px[i] == 255;
        printf("  %-24s %4dx%-4d mean %.3f max %d\n", jpg, im.w, im.h, mean, maxd);
        CHECK(mean <= 2.0 && maxd <= 40, "%s: mean %.3f max %d", jpg, mean, maxd);
        CHECK(alpha, "%s: alpha not opaque", jpg);
    }
    img_free(&im);
    img_free(&rf);
}

static void cmp_fixture(const char *name) {
    char *jpg = xasprintf("%s.jpg", name);
    cmp_file(jpg, name);
    free(jpg);
}

static void test_decode(void) {
    const char *names[] = {"jpg_444",   "jpg_444_prog", "jpg_422",     "jpg_422_prog",     "jpg_420",
                           "jpg_420_prog", "jpg_gray", "jpg_gray_prog", "jpg_restart", "jpg_restart_prog",
                           "jpg_cmyk",  "jpg_q20",      "jpg_large_prog"};
    for (size_t i = 0; i < ARRAY_LEN(names); i++) cmp_fixture(names[i]);
}

static int expect_fail(const uint8_t *d, size_t n, const char *what) {
    img_t im;
    char *err = NULL;
    int rc = img_jpeg_decode(d, n, &im, &err);
    CHECK(rc == -1, "%s: expected failure", what);
    CHECK(rc != -1 || (err && *err), "%s: no error message", what);
    CHECK(rc != -1 || (!im.px && !im.w && !im.h), "%s: output not cleared", what);
    if (rc == 0) img_free(&im);
    free(err);
    return rc;
}

static uint32_t rng = 0x12345678;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* find the first SOFn segment; returns offset of its marker byte (after 0xFF) or 0 */
static size_t find_sof(const uint8_t *d, size_t n) {
    for (size_t i = 2; i + 9 < n; i++)
        if (d[i] == 0xff && d[i + 1] >= 0xc0 && d[i + 1] <= 0xc2) return i + 1;
    return 0;
}

static void test_malformed(void) {
    size_t n;
    uint8_t *d = readf("bad_jpg_trunc.jpg", &n);
    CHECK(d, "missing bad_jpg_trunc.jpg");
    if (d) expect_fail(d, n, "bad_jpg_trunc");
    free(d);
    expect_fail(NULL, 0, "NULL");
    expect_fail((const uint8_t *)"", 0, "empty");
    expect_fail((const uint8_t *)"\xff\xd8", 2, "SOI only");
    expect_fail((const uint8_t *)"\xff\xd8\xff\xd9", 4, "SOI EOI");
    expect_fail((const uint8_t *)"\x89PNG\r\n\x1a\n", 8, "PNG magic");

    const char *fz[] = {"jpg_420.jpg", "jpg_420_prog.jpg", "jpg_restart.jpg", "jpg_restart_prog.jpg",
                        "jpg_gray_prog.jpg", "jpg_cmyk.jpg", "jpg_444_prog.jpg"};
    int iters = getenv("JPEG_FUZZ") ? atoi(getenv("JPEG_FUZZ")) : 300;
    for (size_t f = 0; f < ARRAY_LEN(fz); f++) {
        uint8_t *orig = readf(fz[f], &n);
        CHECK(orig, "missing %s", fz[f]);
        if (!orig) continue;
        /* every truncation removes EOI (at least) and must fail */
        for (size_t cut = 0; cut < n; cut += 97) {
            char what[96];
            snprintf(what, sizeof what, "%s cut %zu", fz[f], cut);
            expect_fail(orig, cut, what);
        }
        expect_fail(orig, n - 1, "cut before last byte");
        /* huge / zero dimensions */
        size_t so = find_sof(orig, n);
        CHECK(so, "%s: no SOF", fz[f]);
        if (so) {
            uint8_t *b = xmalloc(n);
            memcpy(b, orig, n);
            b[so + 4] = b[so + 5] = b[so + 6] = b[so + 7] = 0xff;
            expect_fail(b, n, "65535x65535");
            memcpy(b, orig, n);
            b[so + 4] = b[so + 5] = 0;
            expect_fail(b, n, "zero height");
            memcpy(b, orig, n);
            b[so + 3] = 12;
            expect_fail(b, n, "12-bit");
            memcpy(b, orig, n);
            b[so] = 0xc9;
            expect_fail(b, n, "arithmetic");
            free(b);
        }
        /* random byte flips: must not crash (result may be 0 or -1); odd iterations hit only the
         * header segments (up to the first SOS) */
        size_t hdr = n;
        for (size_t i = 0; i + 1 < n; i++)
            if (orig[i] == 0xff && orig[i + 1] == 0xda) {
                hdr = MIN(n, i + 16);
                break;
            }
        uint8_t *b = xmalloc(n);
        int ok = 0;
        for (int it = 0; it < iters; it++) {
            memcpy(b, orig, n);
            int flips = 1 + (int)(rnd() % 8);
            for (int k = 0; k < flips; k++) {
                size_t at = rnd() % ((it & 1) ? hdr : n);
                b[at] = (rnd() & 1) ? (uint8_t)rnd() : (uint8_t)(b[at] ^ (1u << (rnd() % 8)));
            }
            img_t im;
            char *err = NULL;
            if (img_jpeg_decode(b, n, &im, &err) == 0) {
                ok++;
                CHECK(im.px && im.w > 0 && im.h > 0, "fuzz: success without pixels");
                img_free(&im);
            } else {
                CHECK(err != NULL, "fuzz: failure without message");
            }
            free(err);
        }
        free(b);
        free(orig);
        printf("  fuzz %-22s %d/%d decoded\n", fz[f], ok, iters);
    }
}

static bool write_tmp(const char *name, const uint8_t *d, size_t n) {
    char *p = xasprintf("%s/%s", DIR_, name);
    FILE *f = fopen(p, "wb");
    free(p);
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* Pillow opens + fully decodes the file; optionally writes its decode as <out>.rgba and/or checks
 * our size against Pillow's own encode of <src>.rgba at quality q (ratio printed, must be 0.8..1.2). */
static bool pillow_check(const char *jpg, const char *out_ref, const char *src, int q) {
    char *cmd = xasprintf(
        "cd '%s' && python3 -I -c '\n"
        "import io, struct, sys\n"
        "from PIL import Image\n"
        "im = Image.open(sys.argv[1]); im.load(); assert im.format == \"JPEG\"\n"
        "if sys.argv[2]:\n"
        "    r = im.convert(\"RGBA\")\n"
        "    open(sys.argv[2] + \".rgba\", \"wb\").write(struct.pack(\"<II\", *r.size) + r.tobytes())\n"
        "if sys.argv[3]:\n"
        "    d = open(sys.argv[3] + \".rgba\", \"rb\").read(); w, h = struct.unpack(\"<II\", d[:8])\n"
        "    s = Image.frombytes(\"RGBA\", (w, h), d[8:]).convert(\"RGB\"); b = io.BytesIO()\n"
        "    s.save(b, \"JPEG\", quality=int(sys.argv[4]))\n"
        "    ratio = len(open(sys.argv[1], \"rb\").read()) / len(b.getvalue())\n"
        "    print(\"  size vs Pillow q%%s: %%.3f\" %% (sys.argv[4], ratio))\n"
        "    sys.exit(0 if 0.8 <= ratio <= 1.2 else 3)\n"
        "' '%s' '%s' '%s' %d",
        DIR_, jpg, out_ref ? out_ref : "", src ? src : "", q);
    fflush(stdout);
    int rc = system(cmd);
    free(cmd);
    return rc == 0;
}

/* Minimum PSNR at q80 / q40. src_ui (sharp colored UI edges) loses chroma detail to 4:2:0: Pillow's own
 * encoder scores 25.1 / 22.5 dB on it, and 35.2 / 33.5 dB on the photo. */
static void test_encode_src(const char *src, double min80, double min40) {
    img_t im;
    bool ok = load_ref(src, &im);
    CHECK(ok, "missing %s.rgba", src);
    if (!ok) return;
    int qs[] = {80, 40};
    for (int qi = 0; qi < 2; qi++) {
        size_t len;
        uint8_t *enc = img_jpeg_encode(&im, qs[qi], &len);
        CHECK(enc && len > 100, "%s q%d: encode failed", src, qs[qi]);
        if (!enc) continue;
        img_t dec;
        char *err = NULL;
        int rc = img_jpeg_decode(enc, len, &dec, &err);
        CHECK(rc == 0, "%s q%d: own decode failed: %s", src, qs[qi], err ? err : "?");
        free(err);
        if (rc == 0) {
            CHECK(dec.w == im.w && dec.h == im.h, "%s: size mismatch", src);
            double p = psnr(&im, &dec);
            printf("  encode %-10s q%d: %zu bytes, PSNR %.2f dB\n", src, qs[qi], len, p);
            CHECK(p >= (qi == 0 ? min80 : min40), "%s q%d: PSNR %.2f", src, qs[qi], p);
            img_free(&dec);
        }
        char *fn = xasprintf("enc_%s_q%d.jpg", src, qs[qi]);
        char *rn = xasprintf("enc_%s_q%d_pil", src, qs[qi]);
        CHECK(write_tmp(fn, enc, len), "write %s", fn);
        CHECK(pillow_check(fn, rn, src, qs[qi]), "%s: Pillow rejected or size off", fn);
        /* our decoder vs Pillow's decode of our own stream */
        cmp_file(fn, rn);
        free(fn);
        free(rn);
        free(enc);
    }
    img_free(&im);
}

static void fill_pattern(img_t *im, int seed) {
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            uint8_t *p = im->px + ((size_t)y * im->w + x) * 4;
            p[0] = (uint8_t)(x * 255 / MAX(1, im->w - 1));
            p[1] = (uint8_t)(y * 255 / MAX(1, im->h - 1));
            p[2] = (uint8_t)((x * 7 + y * 13 + seed) & 255);
            p[3] = (uint8_t)(x + y); /* alpha must be ignored */
        }
}

static void test_encode_sizes(void) {
    int sz[][2] = {{1, 1}, {3, 2}, {17, 9}, {16, 16}, {33, 1}, {1, 40}};
    for (size_t i = 0; i < ARRAY_LEN(sz); i++) {
        img_t im;
        img_alloc(&im, sz[i][0], sz[i][1]);
        fill_pattern(&im, (int)i);
        size_t len;
        uint8_t *enc = img_jpeg_encode(&im, 90, &len);
        CHECK(enc, "encode %dx%d", im.w, im.h);
        if (!enc) {
            img_free(&im);
            continue;
        }
        img_t dec;
        char *err = NULL;
        int rc = img_jpeg_decode(enc, len, &dec, &err);
        CHECK(rc == 0 && dec.w == im.w && dec.h == im.h, "%dx%d: decode: %s", im.w, im.h, err ? err : "size");
        free(err);
        if (rc == 0) img_free(&dec);
        char *fn = xasprintf("enc_%dx%d.jpg", im.w, im.h);
        char *rn = xasprintf("enc_%dx%d_pil", im.w, im.h);
        CHECK(write_tmp(fn, enc, len) && pillow_check(fn, rn, NULL, 0), "%s: Pillow rejected", fn);
        cmp_file(fn, rn);
        free(fn);
        free(rn);
        free(enc);
        img_free(&im);
    }
    /* solid color survives within a few levels */
    img_t im;
    img_alloc(&im, 1, 1);
    memcpy(im.px, (uint8_t[]){200, 30, 90, 255}, 4);
    size_t len;
    uint8_t *enc = img_jpeg_encode(&im, 95, &len);
    img_t dec;
    if (enc && img_jpeg_decode(enc, len, &dec, NULL) == 0) {
        int d = 0;
        for (int c = 0; c < 3; c++) d = MAX(d, abs(dec.px[c] - im.px[c]));
        CHECK(d <= 4, "1x1 solid color off by %d", d);
        img_free(&dec);
    } else CHECK(0, "1x1 solid round trip");
    free(enc);
    img_free(&im);
}

/* Other sampling layouts / restart intervals produced by our encoder, checked against Pillow's decode. */
static void test_sampling(void) {
    img_t src;
    if (!load_ref("src_photo", &src)) {
        CHECK(0, "missing src_photo.rgba");
        return;
    }
    /* crop to an odd size for partial MCUs */
    img_t im;
    img_alloc(&im, 101, 77);
    for (int y = 0; y < im.h; y++) memcpy(im.px + (size_t)y * im.w * 4, src.px + (size_t)y * src.w * 4, (size_t)im.w * 4);
    struct { int h, v, ri; const char *tag; } cfg[] = {
        {1, 1, 0, "444"}, {2, 1, 0, "422"}, {1, 2, 0, "440"}, {4, 1, 0, "411"}, {1, 4, 0, "414"},
        {2, 2, 5, "420_ri5"}, {4, 1, 1, "411_ri1"}, {3, 1, 0, "311"},
    };
    for (size_t i = 0; i < ARRAY_LEN(cfg); i++) {
        size_t len;
        uint8_t *enc = img_jpeg_encode_ex(&im, 85, cfg[i].h, cfg[i].v, cfg[i].ri, &len);
        CHECK(enc, "encode %s", cfg[i].tag);
        if (!enc) continue;
        char *fn = xasprintf("samp_%s.jpg", cfg[i].tag);
        char *rn = xasprintf("samp_%s_pil", cfg[i].tag);
        bool ok = write_tmp(fn, enc, len) && pillow_check(fn, rn, NULL, 0);
        if (cfg[i].h == 3) { /* libjpeg rejects fractional ratios; just decode ourselves */
            img_t dec;
            CHECK(img_jpeg_decode(enc, len, &dec, NULL) == 0, "%s own decode", fn);
            if (dec.px) {
                printf("  %-24s PSNR %.2f dB (own decode only)\n", fn, psnr(&im, &dec));
                img_free(&dec);
            }
        } else {
            CHECK(ok, "%s: Pillow rejected", fn);
            if (ok) cmp_file(fn, rn);
        }
        free(fn);
        free(rn);
        free(enc);
    }
    img_free(&im);
    img_free(&src);
}

int main(void) {
    if (system("python3 -I -c 'import PIL' >/dev/null 2>&1") != 0) {
        printf("test_img_jpeg: python3 with Pillow not found, skipping\n");
        return 0;
    }
    DIR_ = fs_mktempdir("omc-jpeg");
    char *cmd = xasprintf("python3 -I tests/img_fixtures.py '%s' >/dev/null", DIR_);
    if (!DIR_ || system(cmd) != 0) {
        fprintf(stderr, "fixture generation failed\n");
        return 1;
    }
    free(cmd);
    printf("decode vs Pillow:\n");
    test_decode();
    printf("malformed input:\n");
    test_malformed();
    printf("encoder:\n");
    test_encode_src("src_photo", 30.0, 28.0);
    test_encode_src("src_ui", 24.0, 21.5);
    test_encode_sizes();
    printf("sampling layouts:\n");
    test_sampling();
    if (!getenv("JPEG_KEEP")) fs_rm_rf(DIR_);
    else printf("kept %s\n", DIR_);
    free(DIR_);
    if (fails) fprintf(stderr, "test_img_jpeg: %d failure(s)\n", fails);
    else printf("test_img_jpeg: ok\n");
    return fails ? 1 : 0;
}
