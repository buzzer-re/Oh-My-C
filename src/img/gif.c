/* gif.c - GIF decoder (first frame). (stub: being implemented) */
#include "img/img.h"
#include "core/core.h"

int img_gif_decode(const uint8_t *d, size_t n, img_t *out, char **err) {
    if (err) *err = xstrdup("GIF decoder not implemented");
    return -1;
}
