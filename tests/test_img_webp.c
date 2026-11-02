/* test_img_webp.c - WebP decoder (VP8 / VP8L / ALPH / VP8X) against Pillow (libwebp) references, and the
 * lossy encoder: own-decoder PSNR, Pillow decodability, size vs libwebp, alpha, odd sizes, speed.
 * Fixtures come from tests/img_fixtures.py (Python + Pillow, test time only). */
#include "core/core.h"
#include "core/fs.h"
#include "img/img.h"
#include <math.h>
#include <time.h>
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

static bool load_rgba(const char *file, img_t *ref) {
    size_t n;
    uint8_t *d = slurp(file, &n);
    if (!d || n < 8) return free(d), false;
    int w = (int)(d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24), h = (int)(d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24);
    if (img_alloc(ref, w, h) < 0 || n != 8 + (size_t)w * h * 4) return free(d), false;
    memcpy(ref->px, d + 8, (size_t)w * h * 4);
    free(d);
    return true;
}

static bool load_ref(const char *base, img_t *ref) {
    char *nm = xasprintf("%s.rgba", base);
    bool ok = load_rgba(nm, ref);
    free(nm);
    return ok;
}

static double psnr_rgb(const img_t *a, const img_t *b) {
    double se = 0;
    size_t n = (size_t)a->w * a->h;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 3; c++) {
            double dd = (double)a->px[i * 4 + c] - b->px[i * 4 + c];
            se += dd * dd;
        }
    double mse = se / (n * 3.0);
    return mse <= 0 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
}

/* mean / max absolute channel difference */
static void diff_stats(const img_t *a, const img_t *b, double *mean, int *maxd) {
    size_t n = (size_t)a->w * a->h * 4;
    long sum = 0;
    *maxd = 0;
    for (size_t i = 0; i < n; i++) {
        int dd = abs(a->px[i] - b->px[i]);
        sum += dd;
        if (dd > *maxd) *maxd = dd;
    }
    *mean = (double)sum / n;
}

/* Decode a fixture and compare with Pillow's decode: exact (tol < 0) or within mean / max tolerance. */
static void cmp_fixture(const char *base, double mean_tol, int max_tol) {
    char *file = xasprintf("%s.webp", base);
    size_t n;
    uint8_t *d = slurp(file, &n);
    CHECK(d, "missing fixture %s", file);
    img_t ref = {0}, im = {0};
    CHECK(load_ref(base, &ref), "missing reference %s", base);
    char *err = NULL;
    img_fmt_t fmt = IMG_FMT_NONE;
    int rc = d ? img_decode(d, n, &im, &fmt, &err) : -1;
    CHECK(rc == 0 && fmt == IMG_FMT_WEBP, "%s: decode failed: %s", file, err ? err : "?");
    if (rc == 0 && ref.px) {
        CHECK(im.w == ref.w && im.h == ref.h, "%s: size %dx%d want %dx%d", file, im.w, im.h, ref.w, ref.h);
        if (im.w == ref.w && im.h == ref.h) {
            double mean;
            int maxd;
            diff_stats(&im, &ref, &mean, &maxd);
            if (mean_tol < 0) CHECK(maxd == 0, "%s: not exact (max diff %d)", file, maxd);
            else CHECK(mean <= mean_tol && maxd <= max_tol, "%s: mean diff %.3f max %d", file, mean, maxd);
            printf("  %-22s %4dx%-4d mean diff %.3f max %d\n", base, im.w, im.h, mean, maxd);
        }
    }
    free(err);
    img_free(&im);
    img_free(&ref);
    free(file);
    free(d);
}

/* Lossy decodes differ from libwebp only by chroma-upsampling rounding (and the loop filter must match). */
#define LOSSY_MEAN 0.6
#define LOSSY_MAX 6

static void test_decode(void) {
    printf("decode vs Pillow:\n");
    cmp_fixture("webp_lossless", -1, 0);
    cmp_fixture("webp_lossless_alpha", -1, 0);
    cmp_fixture("webp_lossless_pal", -1, 0);
    cmp_fixture("webp_tiny", -1, 0);
    cmp_fixture("webp_lossy", LOSSY_MEAN, LOSSY_MAX);
    cmp_fixture("webp_lossy_q30", LOSSY_MEAN, LOSSY_MAX);
    cmp_fixture("webp_lossy_alpha", LOSSY_MEAN, LOSSY_MAX);
    cmp_fixture("webp_small_lossy", LOSSY_MEAN, LOSSY_MAX);
    cmp_fixture("webp_lossy_m0", LOSSY_MEAN, LOSSY_MAX);
    cmp_fixture("webp_lossy_m6", LOSSY_MEAN, LOSSY_MAX);
    cmp_fixture("webp_lossy_alpha_aq", LOSSY_MEAN, LOSSY_MAX);
    cmp_fixture("webp_meta", LOSSY_MEAN, LOSSY_MAX);
    cmp_fixture("webp_lossless_q0", -1, 0);
    cmp_fixture("webp_lossless_m6", -1, 0);
    cmp_fixture("webp_anim", -1, 0);
    cmp_fixture("webp_anim_lossy", LOSSY_MEAN, LOSSY_MAX);
    const char *bad[] = {"bad_webp_trunc.webp"};
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
}

/* byte flips and truncations over every fixture: must fail or succeed cleanly */
static void test_fuzz(void) {
    const char *src[] = {"webp_lossy.webp", "webp_lossy_q30.webp", "webp_lossless.webp", "webp_lossy_alpha.webp", "webp_lossless_alpha.webp",
                         "webp_lossless_pal.webp", "webp_tiny.webp", "webp_small_lossy.webp", "bad_webp_trunc.webp",
                         "webp_lossy_m6.webp", "webp_lossy_alpha_aq.webp", "webp_lossless_q0.webp", "webp_lossless_m6.webp",
                         "webp_meta.webp", "webp_anim.webp", "webp_anim_lossy.webp"};
    uint32_t seed = 4242;
    int ok = 0, total = 0;
    for (size_t s = 0; s < sizeof src / sizeof *src; s++) {
        size_t n;
        uint8_t *d = slurp(src[s], &n);
        if (!d) continue;
        uint8_t *m = xmalloc(n);
        for (int it = 0; it < 300; it++) {
            memcpy(m, d, n);
            size_t len = n;
            if (it % 4 == 0) len = (size_t)(it * 7919) % n;
            else
                for (int k = 0; k < 1 + it % 5; k++) {
                    seed = seed * 1103515245 + 12345;
                    size_t at = 12 + (seed >> 8) % (n > 12 ? n - 12 : 1); /* mostly past the RIFF header */
                    if (it % 9 == 1) at = (seed >> 8) % n;
                    m[at % n] ^= (uint8_t)(1 + (seed >> 3) % 255);
                }
            img_t im = {0};
            char *err = NULL;
            int rc = img_webp_decode(m, len, &im, &err);
            if (rc == 0) {
                CHECK(im.px && im.w > 0 && im.h > 0, "bogus success");
                ok++;
            } else {
                CHECK(err != NULL, "failure without message");
            }
            total++;
            free(err);
            img_free(&im);
        }
        free(m);
        free(d);
    }
    printf("fuzz: %d/%d mutated files still decoded\n", ok, total);
}

static int pillow_size(const char *src_rgba, int q, size_t *size) {
    /* encode the same raster with Pillow (libwebp) at quality q and report the size */
    char *out = xasprintf("%s/pil_q%d.webp", DIR_, q);
    char *cmd = xasprintf("python3 -I -c \"import struct,sys; from PIL import Image; d=open('%s/%s','rb').read(); w,h=struct.unpack('<II',d[:8]); "
                          "Image.frombytes('RGBA',(w,h),d[8:]).convert('RGB').save('%s',quality=%d)\"",
                          DIR_, src_rgba, out, q);
    int rc = system(cmd);
    free(cmd);
    size_t n = 0;
    char *b = rc == 0 ? fs_read_limit(out, 64u << 20, &n, NULL) : NULL;
    free(b);
    free(out);
    *size = n;
    return b ? 0 : -1;
}

/* Pillow decodes data and the result is compared with our own decode of it. */
static void pillow_check(const uint8_t *data, size_t n, const img_t *own, const char *tag) {
    char *p = xasprintf("%s/enc_%s.webp", DIR_, tag), *o = xasprintf("%s/enc_%s.rgba", DIR_, tag);
    fs_write(p, (const char *)data, n);
    char *cmd = xasprintf("python3 -I -c \"import struct; from PIL import Image; im=Image.open('%s'); im.load(); im=im.convert('RGBA'); "
                          "open('%s','wb').write(struct.pack('<II',*im.size)+im.tobytes())\"",
                          p, o);
    CHECK(system(cmd) == 0, "%s: Pillow cannot decode our WebP", tag);
    img_t pil = {0};
    char *rel = xasprintf("enc_%s.rgba", tag);
    if (load_rgba(rel, &pil)) {
        CHECK(pil.w == own->w && pil.h == own->h, "%s: Pillow size %dx%d", tag, pil.w, pil.h);
        if (pil.w == own->w && pil.h == own->h) {
            double mean;
            int maxd;
            diff_stats(&pil, own, &mean, &maxd);
            CHECK(mean <= LOSSY_MEAN && maxd <= LOSSY_MAX, "%s: Pillow vs own decode mean %.3f max %d", tag, mean, maxd);
        }
    } else {
        CHECK(0, "%s: no Pillow output", tag);
    }
    img_free(&pil);
    free(rel), free(cmd), free(p), free(o);
}

static void test_encode(void) {
    printf("encode:\n");
    const char *srcs[] = {"src_photo", "src_ui"};
    for (int s = 0; s < 2; s++) {
        img_t src = {0};
        CHECK(load_ref(srcs[s], &src), "missing %s", srcs[s]);
        if (!src.px) continue;
        for (int qi = 0; qi < 2; qi++) {
            int q = qi ? 40 : 80;
            size_t n = 0;
            uint8_t *enc = img_webp_encode(&src, q, &n);
            CHECK(enc && n > 20, "%s q%d: encode failed", srcs[s], q);
            if (!enc) continue;
            img_t back = {0};
            char *err = NULL;
            int rc = img_webp_decode(enc, n, &back, &err);
            CHECK(rc == 0 && back.w == src.w && back.h == src.h, "%s q%d: own decode failed: %s", srcs[s], q, err ? err : "");
            double ps = rc == 0 ? psnr_rgb(&src, &back) : 0;
            char tag[64];
            snprintf(tag, sizeof tag, "%s_q%d", srcs[s], q);
            if (rc == 0) pillow_check(enc, n, &back, tag);
            size_t pn = 0;
            char rel[64];
            snprintf(rel, sizeof rel, "%s.rgba", srcs[s]);
            double pps = 0;
            if (pillow_size(rel, q, &pn) == 0) {
                char pf[80];
                snprintf(pf, sizeof pf, "pil_q%d.webp", q);
                size_t pl;
                uint8_t *pd = slurp(pf, &pl);
                img_t pim = {0};
                if (pd && img_webp_decode(pd, pl, &pim, NULL) == 0) pps = psnr_rgb(&src, &pim);
                img_free(&pim);
                free(pd);
            }
            printf("  %-10s q%d: %6zu bytes, PSNR %.2f dB | libwebp %6zu bytes, PSNR %.2f dB | size ratio %.3f\n", srcs[s], q, n, ps, pn,
                   pps, pn ? (double)n / pn : 0.0);
            /* src_ui's tiny saturated rectangles lose ~26 dB to 4:2:0 chroma in libwebp too: compare there */
            if (q == 80 && s == 0) CHECK(ps >= 30.0, "%s q80: PSNR %.2f dB", srcs[s], ps);
            if (pn) {
                CHECK(ps >= pps - 1.0, "%s q%d: PSNR %.2f dB, libwebp %.2f dB", srcs[s], q, ps, pps);
                CHECK((double)n / pn <= 1.3, "%s q%d: %.2fx the size of libwebp's output", srcs[s], q, (double)n / pn);
            }
            free(err);
            img_free(&back);
            free(enc);
        }
        /* alpha round trip */
        if (s == 1) {
            for (size_t i = 0; i < (size_t)src.w * src.h; i++) src.px[i * 4 + 3] = (uint8_t)((i % src.w) * 255 / (src.w - 1));
            size_t n = 0;
            uint8_t *enc = img_webp_encode(&src, 80, &n);
            img_t back = {0};
            CHECK(enc && img_webp_decode(enc, n, &back, NULL) == 0, "alpha encode/decode");
            if (back.px) {
                bool same = true;
                for (size_t i = 0; i < (size_t)src.w * src.h; i++) same &= back.px[i * 4 + 3] == src.px[i * 4 + 3];
                CHECK(same, "alpha not preserved");
                pillow_check(enc, n, &back, "alpha");
                img_meta_t m;
                CHECK(img_sniff(enc, n, &m) && m.has_alpha == 1, "VP8X alpha flag");
                printf("  alpha      q80: %6zu bytes\n", n);
            }
            img_free(&back);
            free(enc);
        }
        img_free(&src);
    }
    /* odd sizes */
    int sizes[][2] = {{1, 1}, {15, 15}, {17, 9}, {2, 31}, {33, 1}};
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        img_t im = {0};
        img_alloc(&im, sizes[k][0], sizes[k][1]);
        for (int i = 0; i < im.w * im.h; i++) {
            im.px[i * 4] = (uint8_t)(i * 37), im.px[i * 4 + 1] = (uint8_t)(200 - i), im.px[i * 4 + 2] = (uint8_t)(i * 5), im.px[i * 4 + 3] = 255;
        }
        size_t n = 0;
        uint8_t *enc = img_webp_encode(&im, 90, &n);
        img_t back = {0};
        CHECK(enc && img_webp_decode(enc, n, &back, NULL) == 0 && back.w == im.w && back.h == im.h, "size %dx%d", im.w, im.h);
        char tag[32];
        snprintf(tag, sizeof tag, "sz%dx%d", im.w, im.h);
        if (back.px) pillow_check(enc, n, &back, tag);
        img_free(&back);
        free(enc);
        img_free(&im);
    }
}

/* screenshot-like 1568x882 at q80 must encode quickly */
static void test_speed(void) {
    img_t im = {0};
    img_alloc(&im, 1568, 882);
    uint32_t seed = 99;
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            uint8_t *p = im.px + ((size_t)y * im.w + x) * 4;
            int bg = (y / 40) % 2 ? 250 : 240;
            p[0] = p[1] = p[2] = (uint8_t)bg, p[3] = 255;
            if (x < 220) p[0] = 40, p[1] = 44, p[2] = 52; /* sidebar */
            if ((y % 20) > 5 && (y % 20) < 15 && ((x / 7) % 9) < 6 && x > 240 && x < 1400) {
                seed = seed * 1103515245 + 12345;
                if ((seed >> 16) % 3) p[0] = p[1] = p[2] = 30; /* "text" */
            }
            if (y > 700 && x > 300 && x < 1200) p[0] = (uint8_t)x, p[1] = (uint8_t)y, p[2] = 128; /* gradient */
        }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    size_t n = 0;
    uint8_t *enc = img_webp_encode(&im, 80, &n);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double t = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    CHECK(enc != NULL, "big encode");
    img_t back = {0};
    clock_gettime(CLOCK_MONOTONIC, &a);
    int rc = enc ? img_webp_decode(enc, n, &back, NULL) : -1;
    clock_gettime(CLOCK_MONOTONIC, &b);
    double td = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    CHECK(rc == 0, "big decode");
    printf("speed: 1568x882 q80 encode %.3f s (%zu bytes, PSNR %.2f dB), decode %.3f s\n", t, n, rc == 0 ? psnr_rgb(&im, &back) : 0.0, td);
    CHECK(t < 1.5, "encode too slow: %.3f s", t);
    img_free(&back);
    free(enc);
    img_free(&im);
}

int main(int argc, char **argv) {
    const char *tmp = getenv("TMPDIR");
    snprintf(DIR_, sizeof DIR_, "%s/omc-webp-test-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    char *cmd = xasprintf("python3 -I tests/img_fixtures.py '%s' >/dev/null", DIR_);
    if (system(cmd) != 0) {
        fprintf(stderr, "fixture generation failed\n");
        return 1;
    }
    free(cmd);
    bool fuzz_only = argc > 1 && !strcmp(argv[1], "--fuzz");
    if (!fuzz_only) test_decode();
    test_fuzz();
    if (!fuzz_only) {
        test_encode();
        test_speed();
    }
    cmd = xasprintf("rm -rf '%s'", DIR_);
    if (system(cmd)) {}
    free(cmd);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("img_webp: all ok\n");
    return failures != 0;
}
