/* webp.c - WebP decoder (VP8 lossy, VP8L lossless, VP8X + ALPH). (stub: being implemented) */
#include "img/img.h"
#include "core/core.h"

int img_webp_decode(const uint8_t *d, size_t n, img_t *out, char **err) {
    if (err) *err = xstrdup("WebP decoder not implemented");
    return -1;
}
