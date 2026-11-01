/* png.c - PNG decoder and encoder. (stub: being implemented) */
#include "img/img.h"
#include "core/core.h"

int img_png_decode(const uint8_t *d, size_t n, img_t *out, char **err) {
    if (err) *err = xstrdup("PNG decoder not implemented");
    return -1;
}

uint8_t *img_png_encode(const img_t *im, int level, size_t *len) {
    *len = 0;
    return NULL;
}
