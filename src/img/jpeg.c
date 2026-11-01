/* jpeg.c - JPEG decoder (baseline + progressive) and baseline encoder. (stub: being implemented) */
#include "img/img.h"
#include "core/core.h"

int img_jpeg_decode(const uint8_t *d, size_t n, img_t *out, char **err) {
    if (err) *err = xstrdup("JPEG decoder not implemented");
    return -1;
}

uint8_t *img_jpeg_encode(const img_t *im, int quality, size_t *len) {
    *len = 0;
    return NULL;
}
