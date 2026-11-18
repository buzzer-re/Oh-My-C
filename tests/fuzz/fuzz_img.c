/* fuzz_img - image sniffing and the PNG / JPEG / GIF / WebP decoders (src/img), then resize and re-encode, on files the
 * read tool and pasted images feed in. Input: raw image bytes (the decoder is picked by magic, like the read tool). */
#include "core/core.h"
#include "img/img.h"
#include "fuzz.h"

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 1 << 20) return 0;
    img_meta_t meta;
    /* the decoders allow up to IMG_MAX_PIXELS (512 MiB); keep the fuzzer under its RSS limit */
    if (img_sniff(data, size, &meta) && (int64_t)meta.w * meta.h > (1 << 22)) return 0;
    img_t im;
    memset(&im, 0, sizeof im);
    img_fmt_t fmt = IMG_FMT_NONE;
    char *err = NULL;
    if (img_decode(data, size, &im, &fmt, &err) == 0) {
        FUZZ_ASSERT(im.w > 0 && im.h > 0);
        (void)img_translucent(&im);
        if ((int64_t)im.w * im.h <= 1 << 16) {
            img_t r;
            memset(&r, 0, sizeof r);
            if (img_resize(&im, im.w / 2 + 1, im.h / 3 + 1, &r) == 0) img_free(&r);
            size_t n = 0;
            uint8_t *png = img_png_encode(&im, 1, &n);
            if (png) {
                img_t back;
                memset(&back, 0, sizeof back);
                char *e2 = NULL;
                FUZZ_ASSERT(img_png_decode(png, n, &back, &e2) == 0);
                FUZZ_ASSERT(back.w == im.w && back.h == im.h);
                img_free(&back);
                free(e2);
                free(png);
            }
        }
        img_free(&im);
    }
    free(err);
    return 0;
}
