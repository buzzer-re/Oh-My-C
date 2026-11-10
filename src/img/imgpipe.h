/* imgpipe.h - omp's model image pipeline on top of the src/img codecs.
 *
 * Ports of packages/coding-agent/src/utils/image-resize.ts (resizeImage, formatDimensionNote),
 * packages/tui/src/chat/image-loading.ts (MAX_IMAGE_INPUT_BYTES, imageDecodeFailureReason,
 * ImageInputTooLargeError / InvalidImageDataError messages) and utils/image-loading.ts (loadImageInput's
 * in-memory core). Byte-level and model-agnostic: model policy (WebP exclusion for STB-backed servers,
 * provider budgets) lives in src/ai/ai_images.c.
 */
#ifndef OMC_IMGPIPE_H
#define OMC_IMGPIPE_H
#include "img/img.h"

#define IMG_MAX_INPUT_BYTES (20u * 1024 * 1024) /* MAX_IMAGE_INPUT_BYTES */

/* resizeImage options; zero fields take omp's defaults (1568x1568, min edge 200, 500 KiB, JPEG q80). */
typedef struct {
    int max_w, max_h;
    int min_dim;
    size_t max_bytes;
    int jpeg_quality;
    int exclude_webp; /* 0 honor OMP_NO_WEBP (default), 1 exclude, -1 allow */
} img_resize_opts_t;

typedef struct {
    uint8_t *buf; /* owned encoded bytes (a copy of the input when unchanged) */
    size_t len;
    const char *mime; /* static string; caller mime / octet-stream when undecodable */
    int orig_w, orig_h, w, h;
    bool was_resized;
    bool decode_failed;
    /* paddedContent: where the source content sits on the canvas when an extreme aspect ratio required padding */
    bool padded;
    int pad_left, pad_top, content_w, content_h;
} img_resized_t;

/* resizeImage: fit within the max dimensions and byte budget (fast path keeps a compact in-bounds input;
 * else the smallest of PNG / JPEG / WebP at the target size, then the quality and scale ladders).
 * caller_mime may be NULL. Returns 0 (out filled) or -1 with *err when the input cannot be decoded while
 * WebP exclusion was demanded for a possibly-WebP source (omp throws there). */
int img_resize_image(const uint8_t *in, size_t n, const char *caller_mime, const img_resize_opts_t *o, img_resized_t *out,
                     char **err);
void img_resized_free(img_resized_t *r);
/* formatDimensionNote: "[Image: original WxH, displayed at wxh. Multiply coordinates by S to map to original image.]"
 * (a padded image names its padding and both factors), or NULL when the image was not resized. Owned. */
char *img_dimension_note(const img_resized_t *r);
/* OMP_NO_WEBP (or OMC_NO_WEBP) = "1" / "true" */
bool img_webp_excluded_by_env(void);

/* imageDecodeFailureReason: NULL when the base64 payload decodes as the image it claims to be, else an
 * owned reason ("invalid base64 image data", "empty image data", "declared X but contains Y",
 * "Image: decode failed"). Verdicts are cached by payload hash. */
char *img_decode_failure_reason(const char *b64, const char *mime);
/* jpegStreamIsComplete: the JPEG marker walk reaches an EOI (truncated scans are rejected) */
bool img_jpeg_stream_complete(const unsigned char *bytes, size_t n);
/* Strict standard-base64 validation (pi-utils base64ImageDataFailureReason); NULL when valid. Static. */
const char *img_b64_failure(const char *b64, size_t len);

/* formatBytes (pi-utils): "512B", "1.5KB", "2.3MB", "1.0GB". Owned. */
char *img_format_bytes(uint64_t n);

/* loadImageInput / loadImageAttachmentInput core (loadInMemoryImageInput). */
typedef struct {
    char *mime;      /* output mime */
    char *data;      /* output base64 */
    char *text_note; /* "Read image file [image/png] (displayed as image/webp)\n[Image: ...]" */
    char *dim_note;  /* may be NULL */
    size_t bytes;    /* output byte length */
} img_loaded_t;

/* Load raw image bytes (already read) as a model input. label_prefix is e.g. "Read image file";
 * resolved is the path used in error messages. mime is the sniffed / declared mime. Returns 0, or -1 with
 * *err set to omp's message ("Image file too large: ..." / "<path> is not a decodable <mime> image: <why>"). */
int img_load_input(const uint8_t *bytes, size_t n, const char *mime, const char *resolved, const char *label_prefix,
                   bool auto_resize, int exclude_webp, img_loaded_t *out, char **err);
void img_loaded_free(img_loaded_t *l);

/* 64-bit FNV-1a, used for payload-keyed caches */
uint64_t img_hash(const void *d, size_t n);

#endif
