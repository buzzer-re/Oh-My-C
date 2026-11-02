/* vp8_dsp.c - VP8 intra prediction, inverse transforms (bit-exact with RFC 6386 section 12 / 14) and the
 * encoder's forward transforms. Shared by webp.c (decoder) and webp_enc.c (encoder). */
#include "img/vp8.h"
#include <string.h>

#define BPS VP8_BPS

static inline uint8_t clip255(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

void vp8_load_edges(vp8_mbbuf_t *b, const vp8_planes_t *p, int mbx, int mby) {
    uint8_t *y = VP8_YP(b), *u = VP8_UP(b), *v = VP8_VP(b);
    int x0 = mbx * 16, y0 = mby * 16, cx0 = mbx * 8, cy0 = mby * 8;
    if (mby == 0) {
        memset(y - BPS - 1, 127, 21);
        memset(u - BPS - 1, 127, 9);
        memset(v - BPS - 1, 127, 9);
    } else {
        const uint8_t *ya = p->y + (size_t)(y0 - 1) * p->ys + x0;
        const uint8_t *ua = p->u + (size_t)(cy0 - 1) * p->uvs + cx0, *va = p->v + (size_t)(cy0 - 1) * p->uvs + cx0;
        memcpy(y - BPS, ya, 16);
        memcpy(u - BPS, ua, 8);
        memcpy(v - BPS, va, 8);
        if (mbx == 0) {
            y[-BPS - 1] = u[-BPS - 1] = v[-BPS - 1] = 129;
        } else {
            y[-BPS - 1] = ya[-1];
            u[-BPS - 1] = ua[-1];
            v[-BPS - 1] = va[-1];
        }
        if (mbx < p->mbw - 1) memcpy(y - BPS + 16, ya + 16, 4);
        else memset(y - BPS + 16, ya[15], 4);
    }
    /* subblocks 7, 11, 15 use the macroblock's above-right pixels */
    for (int r = 3; r < 15; r += 4) memcpy(y + r * BPS + 16, y - BPS + 16, 4);
    if (mbx == 0) {
        for (int j = 0; j < 16; j++) y[j * BPS - 1] = 129;
        for (int j = 0; j < 8; j++) u[j * BPS - 1] = v[j * BPS - 1] = 129;
    } else {
        for (int j = 0; j < 16; j++) y[j * BPS - 1] = p->y[(size_t)(y0 + j) * p->ys + x0 - 1];
        for (int j = 0; j < 8; j++) {
            u[j * BPS - 1] = p->u[(size_t)(cy0 + j) * p->uvs + cx0 - 1];
            v[j * BPS - 1] = p->v[(size_t)(cy0 + j) * p->uvs + cx0 - 1];
        }
    }
}

void vp8_store_mb(const vp8_mbbuf_t *b, vp8_planes_t *p, int mbx, int mby) {
    const uint8_t *y = VP8_YP(b), *u = VP8_UP(b), *v = VP8_VP(b);
    for (int j = 0; j < 16; j++) memcpy(p->y + (size_t)(mby * 16 + j) * p->ys + mbx * 16, y + j * BPS, 16);
    for (int j = 0; j < 8; j++) {
        memcpy(p->u + (size_t)(mby * 8 + j) * p->uvs + mbx * 8, u + j * BPS, 8);
        memcpy(p->v + (size_t)(mby * 8 + j) * p->uvs + mbx * 8, v + j * BPS, 8);
    }
}

/* 16x16 and 8x8 predictors (n = block size) */
static void pred_block(uint8_t *dst, int n, int mode, bool has_top, bool has_left) {
    const uint8_t *top = dst - BPS;
    switch (mode) {
    case VP8_DC_PRED: {
        int sum = 0, shift = n == 16 ? 3 : 2, dc;
        if (has_top && has_left) {
            for (int i = 0; i < n; i++) sum += top[i] + dst[i * BPS - 1];
            dc = (sum + n) >> (shift + 2);
        } else if (has_top || has_left) {
            for (int i = 0; i < n; i++) sum += has_top ? top[i] : dst[i * BPS - 1];
            dc = (sum + n / 2) >> (shift + 1);
        } else {
            dc = 128;
        }
        for (int j = 0; j < n; j++) memset(dst + j * BPS, dc, n);
        break;
    }
    case VP8_V_PRED:
        for (int j = 0; j < n; j++) memcpy(dst + j * BPS, top, n);
        break;
    case VP8_H_PRED:
        for (int j = 0; j < n; j++) memset(dst + j * BPS, dst[j * BPS - 1], n);
        break;
    default: { /* TM_PRED */
        int p = top[-1];
        for (int j = 0; j < n; j++) {
            int l = dst[j * BPS - 1] - p;
            for (int i = 0; i < n; i++) dst[j * BPS + i] = clip255(l + top[i]);
        }
        break;
    }
    }
}

void vp8_pred16(uint8_t *dst, int mode, bool has_top, bool has_left) { pred_block(dst, 16, mode, has_top, has_left); }
void vp8_pred8(uint8_t *dst, int mode, bool has_top, bool has_left) { pred_block(dst, 8, mode, has_top, has_left); }

static inline uint8_t avg3(int x, int y, int z) { return (uint8_t)((x + 2 * y + z + 2) >> 2); }
static inline uint8_t avg2(int x, int y) { return (uint8_t)((x + y + 1) >> 1); }
#define B(r, c) dst[(r) * BPS + (c)]

void vp8_pred4(uint8_t *dst, int mode) {
    const uint8_t *A = dst - BPS; /* A[-1] = P, A[0..7] */
    int L[4], E[9];
    for (int i = 0; i < 4; i++) L[i] = dst[i * BPS - 1];
    E[0] = L[3], E[1] = L[2], E[2] = L[1], E[3] = L[0], E[4] = A[-1];
    E[5] = A[0], E[6] = A[1], E[7] = A[2], E[8] = A[3];
    switch (mode) {
    case B_DC_PRED: {
        int v = 4;
        for (int i = 0; i < 4; i++) v += A[i] + L[i];
        v >>= 3;
        for (int r = 0; r < 4; r++) memset(dst + r * BPS, v, 4);
        break;
    }
    case B_TM_PRED:
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) B(r, c) = clip255(L[r] + A[c] - A[-1]);
        break;
    case B_VE_PRED:
        for (int c = 0; c < 4; c++) {
            uint8_t v = avg3(A[c - 1], A[c], A[c + 1]);
            B(0, c) = B(1, c) = B(2, c) = B(3, c) = v;
        }
        break;
    case B_HE_PRED: {
        uint8_t h[4] = {avg3(A[-1], L[0], L[1]), avg3(L[0], L[1], L[2]), avg3(L[1], L[2], L[3]), avg3(L[2], L[3], L[3])};
        for (int r = 0; r < 4; r++) memset(dst + r * BPS, h[r], 4);
        break;
    }
    case B_LD_PRED:
        B(0, 0) = avg3(A[0], A[1], A[2]);
        B(0, 1) = B(1, 0) = avg3(A[1], A[2], A[3]);
        B(0, 2) = B(1, 1) = B(2, 0) = avg3(A[2], A[3], A[4]);
        B(0, 3) = B(1, 2) = B(2, 1) = B(3, 0) = avg3(A[3], A[4], A[5]);
        B(1, 3) = B(2, 2) = B(3, 1) = avg3(A[4], A[5], A[6]);
        B(2, 3) = B(3, 2) = avg3(A[5], A[6], A[7]);
        B(3, 3) = avg3(A[6], A[7], A[7]);
        break;
    case B_RD_PRED:
        B(3, 0) = avg3(E[0], E[1], E[2]);
        B(3, 1) = B(2, 0) = avg3(E[1], E[2], E[3]);
        B(3, 2) = B(2, 1) = B(1, 0) = avg3(E[2], E[3], E[4]);
        B(3, 3) = B(2, 2) = B(1, 1) = B(0, 0) = avg3(E[3], E[4], E[5]);
        B(2, 3) = B(1, 2) = B(0, 1) = avg3(E[4], E[5], E[6]);
        B(1, 3) = B(0, 2) = avg3(E[5], E[6], E[7]);
        B(0, 3) = avg3(E[6], E[7], E[8]);
        break;
    case B_VR_PRED:
        B(3, 0) = avg3(E[1], E[2], E[3]);
        B(2, 0) = avg3(E[2], E[3], E[4]);
        B(3, 1) = B(1, 0) = avg3(E[3], E[4], E[5]);
        B(2, 1) = B(0, 0) = avg2(E[4], E[5]);
        B(3, 2) = B(1, 1) = avg3(E[4], E[5], E[6]);
        B(2, 2) = B(0, 1) = avg2(E[5], E[6]);
        B(3, 3) = B(1, 2) = avg3(E[5], E[6], E[7]);
        B(2, 3) = B(0, 2) = avg2(E[6], E[7]);
        B(1, 3) = avg3(E[6], E[7], E[8]);
        B(0, 3) = avg2(E[7], E[8]);
        break;
    case B_VL_PRED:
        B(0, 0) = avg2(A[0], A[1]);
        B(1, 0) = avg3(A[0], A[1], A[2]);
        B(2, 0) = B(0, 1) = avg2(A[1], A[2]);
        B(1, 1) = B(3, 0) = avg3(A[1], A[2], A[3]);
        B(2, 1) = B(0, 2) = avg2(A[2], A[3]);
        B(3, 1) = B(1, 2) = avg3(A[2], A[3], A[4]);
        B(2, 2) = B(0, 3) = avg2(A[3], A[4]);
        B(3, 2) = B(1, 3) = avg3(A[3], A[4], A[5]);
        B(2, 3) = avg3(A[4], A[5], A[6]);
        B(3, 3) = avg3(A[5], A[6], A[7]);
        break;
    case B_HD_PRED:
        B(3, 0) = avg2(E[0], E[1]);
        B(3, 1) = avg3(E[0], E[1], E[2]);
        B(2, 0) = B(3, 2) = avg2(E[1], E[2]);
        B(2, 1) = B(3, 3) = avg3(E[1], E[2], E[3]);
        B(2, 2) = B(1, 0) = avg2(E[2], E[3]);
        B(2, 3) = B(1, 1) = avg3(E[2], E[3], E[4]);
        B(1, 2) = B(0, 0) = avg2(E[3], E[4]);
        B(1, 3) = B(0, 1) = avg3(E[3], E[4], E[5]);
        B(0, 2) = avg3(E[4], E[5], E[6]);
        B(0, 3) = avg3(E[5], E[6], E[7]);
        break;
    default: /* B_HU_PRED */
        B(0, 0) = avg2(L[0], L[1]);
        B(0, 1) = avg3(L[0], L[1], L[2]);
        B(0, 2) = B(1, 0) = avg2(L[1], L[2]);
        B(0, 3) = B(1, 1) = avg3(L[1], L[2], L[3]);
        B(1, 2) = B(2, 0) = avg2(L[2], L[3]);
        B(1, 3) = B(2, 1) = avg3(L[2], L[3], L[3]);
        B(2, 2) = B(2, 3) = B(3, 0) = B(3, 1) = B(3, 2) = B(3, 3) = (uint8_t)L[3];
        break;
    }
}
#undef B

/* x * sqrt(2) * cos(pi/8) and x * sqrt(2) * sin(pi/8) exactly as the reference decoder (16-bit fixed
 * point); 64-bit products keep corrupt (huge) coefficients free of overflow. */
static inline int mul_c(int a) { return (int)(((int64_t)a * 20091) >> 16) + a; }
static inline int mul_s(int a) { return (int)(((int64_t)a * 35468) >> 16); }

void vp8_idct_add(const int16_t *in, uint8_t *dst, int stride) {
    int t[16];
    for (int i = 0; i < 4; i++) {
        int a = in[i] + in[8 + i], b = in[i] - in[8 + i];
        int c = mul_s(in[4 + i]) - mul_c(in[12 + i]);
        int d = mul_c(in[4 + i]) + mul_s(in[12 + i]);
        t[i] = a + d, t[4 + i] = b + c, t[8 + i] = b - c, t[12 + i] = a - d;
    }
    for (int r = 0; r < 4; r++) {
        const int *s = t + 4 * r;
        int a = s[0] + s[2], b = s[0] - s[2];
        int c = mul_s(s[1]) - mul_c(s[3]);
        int d = mul_c(s[1]) + mul_s(s[3]);
        uint8_t *o = dst + r * stride;
        o[0] = clip255(o[0] + ((a + d + 4) >> 3));
        o[1] = clip255(o[1] + ((b + c + 4) >> 3));
        o[2] = clip255(o[2] + ((b - c + 4) >> 3));
        o[3] = clip255(o[3] + ((a - d + 4) >> 3));
    }
}

void vp8_iwht(const int16_t *in, int16_t *out) {
    int t[16];
    for (int i = 0; i < 4; i++) {
        int a = in[i] + in[12 + i], b = in[4 + i] + in[8 + i];
        int c = in[4 + i] - in[8 + i], d = in[i] - in[12 + i];
        t[i] = a + b, t[4 + i] = c + d, t[8 + i] = a - b, t[12 + i] = d - c;
    }
    for (int r = 0; r < 4; r++) {
        const int *s = t + 4 * r;
        int a = s[0] + s[3], b = s[1] + s[2], c = s[1] - s[2], d = s[0] - s[3];
        out[(4 * r + 0) * 16] = (int16_t)((a + b + 3) >> 3);
        out[(4 * r + 1) * 16] = (int16_t)((c + d + 3) >> 3);
        out[(4 * r + 2) * 16] = (int16_t)((a - b + 3) >> 3);
        out[(4 * r + 3) * 16] = (int16_t)((d - c + 3) >> 3);
    }
}

/* Forward DCT: the exact inverse (up to rounding) of vp8_idct_add's basis,
 * X[k][l] = 1/2 * sum_r sum_c C_k(r) C_l(c) x[r][c] with C_0 = 1, C_k(n) = sqrt(2) cos((2n+1) k pi / 8).
 * 12-bit constants: 5352 = sqrt(2) cos(pi/8), 2217 = sqrt(2) sin(pi/8). */
void vp8_fdct(const uint8_t *src, int sstride, const uint8_t *pred, int pstride, int16_t *out) {
    int t[16];
    for (int r = 0; r < 4; r++) {
        const uint8_t *s = src + r * sstride, *p = pred + r * pstride;
        int d0 = s[0] - p[0], d1 = s[1] - p[1], d2 = s[2] - p[2], d3 = s[3] - p[3];
        int a = d0 + d3, b = d1 + d2, c = d1 - d2, e = d0 - d3;
        t[4 * r + 0] = (a + b) * 16;
        t[4 * r + 2] = (a - b) * 16;
        t[4 * r + 1] = (e * 5352 + c * 2217 + 128) >> 8;
        t[4 * r + 3] = (e * 2217 - c * 5352 + 128) >> 8;
    }
    for (int l = 0; l < 4; l++) {
        int a = t[l] + t[12 + l], b = t[4 + l] + t[8 + l], c = t[4 + l] - t[8 + l], e = t[l] - t[12 + l];
        out[l] = (int16_t)((a + b + 16) >> 5);
        out[8 + l] = (int16_t)((a - b + 16) >> 5);
        out[4 + l] = (int16_t)((e * 5352 + c * 2217 + (1 << 16)) >> 17);
        out[12 + l] = (int16_t)((e * 2217 - c * 5352 + (1 << 16)) >> 17);
    }
}

/* Forward WHT, X = W^T x W / 2 for the inverse x = W X W^T / 8 of vp8_iwht. */
void vp8_fwht(const int16_t *in, int16_t *out) {
    int t[16];
    for (int r = 0; r < 4; r++) {
        int v0 = in[(4 * r + 0) * 16], v1 = in[(4 * r + 1) * 16], v2 = in[(4 * r + 2) * 16], v3 = in[(4 * r + 3) * 16];
        int a = v0 + v3, b = v1 + v2, c = v1 - v2, e = v0 - v3;
        t[4 * r + 0] = a + b, t[4 * r + 1] = e + c, t[4 * r + 2] = a - b, t[4 * r + 3] = e - c;
    }
    for (int l = 0; l < 4; l++) {
        int v0 = t[l], v1 = t[4 + l], v2 = t[8 + l], v3 = t[12 + l];
        int a = v0 + v3, b = v1 + v2, c = v1 - v2, e = v0 - v3;
        out[l] = (int16_t)((a + b + 1) >> 1);
        out[4 + l] = (int16_t)((e + c + 1) >> 1);
        out[8 + l] = (int16_t)((a - b + 1) >> 1);
        out[12 + l] = (int16_t)((e - c + 1) >> 1);
    }
}
