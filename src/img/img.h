/* img.h - from-scratch raster codecs and resampling for the model image pipeline.
 *
 * omp normalizes every image before it reaches a model (Bun.Image: decode, resize, re-encode as the
 * smallest of PNG / JPEG / WebP). omc does the same with these in-tree codecs; no third-party image
 * library is linked. All decoders parse untrusted bytes: every read is bounds-checked, dimensions are
 * capped by IMG_MAX_PIXELS / IMG_MAX_DIM, and recursion is never driven by input.
 *
 * Pixels are 8-bit RGBA, straight (non-premultiplied) alpha, rows top to bottom, stride w*4.
 * Decoders return 0 on success and fill *out (owned; release with img_free), or -1 with *err set to an
 * owned message when err is non-NULL. Encoders return an owned buffer (xmalloc) and its length.
 */
#ifndef OMC_IMG_H
#define OMC_IMG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int w, h;
    uint8_t *px; /* w*h*4 bytes RGBA */
} img_t;

/* Hard caps for decoded rasters (decompression-bomb guard): any side above IMG_MAX_DIM or an area above
 * IMG_MAX_PIXELS is rejected before pixel memory is allocated. */
#define IMG_MAX_DIM 65535
#define IMG_MAX_PIXELS (1u << 27) /* 134M px = 512 MiB RGBA */

typedef enum { IMG_FMT_NONE = 0, IMG_FMT_PNG, IMG_FMT_JPEG, IMG_FMT_GIF, IMG_FMT_WEBP } img_fmt_t;

/* Header metadata, a port of pi-utils parseImageMetadata (magic bytes only; no decode). Fields the
 * header does not carry are 0 (w/h/channels) or -1 (has_alpha: unknown). */
typedef struct {
    img_fmt_t fmt;
    int w, h;
    int channels;
    int has_alpha; /* 1 yes, 0 no, -1 unknown */
} img_meta_t;

bool img_sniff(const uint8_t *d, size_t n, img_meta_t *m); /* false when no supported magic matched */
const char *img_fmt_mime(img_fmt_t f);                     /* "image/png" ... or NULL */
img_fmt_t img_fmt_from_mime(const char *mime);             /* case-insensitive; IMG_FMT_NONE when unknown */

/* Allocate a w x h raster (zeroed). Returns -1 when the size is out of bounds. */
int img_alloc(img_t *im, int w, int h);
void img_free(img_t *im);
/* true when some pixel has alpha < 255 */
bool img_translucent(const img_t *im);

/* ---- decoders (first frame / still image only) ---- */
int img_png_decode(const uint8_t *d, size_t n, img_t *out, char **err);
int img_jpeg_decode(const uint8_t *d, size_t n, img_t *out, char **err);
int img_gif_decode(const uint8_t *d, size_t n, img_t *out, char **err);
int img_webp_decode(const uint8_t *d, size_t n, img_t *out, char **err);
/* Sniff + dispatch. *fmt (optional) receives the detected container. */
int img_decode(const uint8_t *d, size_t n, img_t *out, img_fmt_t *fmt, char **err);

/* ---- encoders ---- */
/* PNG: zlib level 0..9 (-1 = default 6); 8-bit RGB when every pixel is opaque, else RGBA. */
uint8_t *img_png_encode(const img_t *im, int level, size_t *len);
/* Baseline JPEG, quality 1..100 (IJG scaling), YCbCr 4:2:0, JFIF; alpha is ignored. */
uint8_t *img_jpeg_encode(const img_t *im, int quality, size_t *len);
/* Lossy WebP (VP8 keyframe, libwebp quality scale 0..100); a translucent image gets a VP8X container
 * with an ALPH chunk. */
uint8_t *img_webp_encode(const img_t *im, int quality, size_t *len);

/* ---- resampling ---- */
/* Resize src to w x h into *dst (owned). Separable filter: area-weighted triangle (bilinear support
 * widened by the scale factor) when shrinking, bilinear when enlarging. Channels are filtered as given:
 * callers premultiply translucent images first (img_premultiply / img_unpremultiply). */
int img_resize(const img_t *src, int w, int h, img_t *dst);
void img_premultiply(img_t *im);
void img_unpremultiply(img_t *im);

#endif
