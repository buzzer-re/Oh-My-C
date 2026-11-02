/* vp8.h - internal: pieces shared by the WebP decoder (webp.c, vp8l.c) and the lossy encoder (webp_enc.c).
 * VP8 intra prediction / transforms follow RFC 6386; VP8L (lossless) follows RFC 9649. */
#ifndef OMC_IMG_VP8_H
#define OMC_IMG_VP8_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 16x16 / chroma modes (RFC 6386 11.2) */
enum { VP8_DC_PRED, VP8_V_PRED, VP8_H_PRED, VP8_TM_PRED, VP8_B_PRED };
/* 4x4 subblock modes */
enum { B_DC_PRED, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_LD_PRED, B_RD_PRED, B_VR_PRED, B_VL_PRED, B_HD_PRED, B_HU_PRED, VP8_NUM_BMODES };

/* Macroblock work buffer: each plane carries its top row (incl. top-left and, for luma, 4 top-right
 * pixels) and left column so prediction reads its edges in place. Stride VP8_BPS. */
#define VP8_BPS 32
typedef struct {
    uint8_t y[VP8_BPS * 17], u[VP8_BPS * 9], v[VP8_BPS * 9];
} vp8_mbbuf_t;
#define VP8_YP(b) ((b)->y + VP8_BPS + 8)
#define VP8_UP(b) ((b)->u + VP8_BPS + 8)
#define VP8_VP(b) ((b)->v + VP8_BPS + 8)

/* Reconstruction planes (unfiltered), sized to whole macroblocks. */
typedef struct {
    uint8_t *y, *u, *v;
    int ys, uvs;    /* strides */
    int mbw, mbh;   /* macroblocks */
} vp8_planes_t;

/* Fill the edges of b for macroblock (mbx, mby) from the planes; out-of-frame pixels are 127 above and
 * 129 left (RFC 6386 12.2/12.3); the top-right pixels are replicated down for subblocks 7, 11, 15. */
void vp8_load_edges(vp8_mbbuf_t *b, const vp8_planes_t *p, int mbx, int mby);
/* Copy the reconstructed macroblock back into the planes. */
void vp8_store_mb(const vp8_mbbuf_t *b, vp8_planes_t *p, int mbx, int mby);

void vp8_pred16(uint8_t *dst, int mode, bool has_top, bool has_left);
void vp8_pred8(uint8_t *dst, int mode, bool has_top, bool has_left);
void vp8_pred4(uint8_t *dst, int mode);

/* Inverse DCT of in[16] (raster order, dequantized) added to the 4x4 block at dst. */
void vp8_idct_add(const int16_t *in, uint8_t *dst, int stride);
/* Inverse WHT: in[16] -> out[i * 16] (the DC of luma block i). */
void vp8_iwht(const int16_t *in, int16_t *out);
/* Forward DCT of (src - pred) for one 4x4 block. */
void vp8_fdct(const uint8_t *src, int sstride, const uint8_t *pred, int pstride, int16_t *out);
/* Forward WHT of the 16 luma DCs in[i * 16]. */
void vp8_fwht(const int16_t *in, int16_t *out);

/* VP8L distance codes 1..120: (xi, yi) offsets of the 2-D neighbourhood (RFC 9649 3.6.2.2.1) */
extern const int8_t vp8l_dist_map[120][2];

/* VP8L (lossless) image stream decoder. headerless: an ALPH payload (no signature / size header; *w, *h
 * are inputs). Otherwise *w, *h receive the header size. Returns 0 and an owned ARGB raster
 * (0xAARRGGBB, w*h) or -1 with *err (static message). */
int vp8l_decode(const uint8_t *d, size_t n, bool headerless, int *w, int *h, uint32_t **argb, const char **err);

#endif
