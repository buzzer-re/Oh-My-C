/*
 * color.h - color math shared by the chrome (port of omp packages/utils/src/color.ts and
 * packages/tui/src/theme/session-color.ts): OKLCH conversions, sRGB gamut cusps, HSV
 * adjustment, luminance, and the per-session accent derived from a session title.
 *
 * Colors are 0xRRGGBB integers.
 */
#ifndef OMC_TUI_COLOR_H
#define OMC_TUI_COLOR_H

#include "core/core.h"
#include "tui/theme.h"

typedef struct {
    double l, c, h;
} tui_oklch_t;

tui_oklch_t tui_rgb_to_oklch(uint32_t rgb);
uint32_t tui_oklch_to_rgb(tui_oklch_t c); /* gamut-mapped by chroma reduction */
/* sRGB gamut cusp (lightness, chroma) for an OKLCH hue */
void tui_oklch_cusp(double h, double *l, double *c);
double tui_hue_distance(double a, double b);
/* HSV multiplier adjustment (upstream adjustHsv with s/v multipliers, h additive) */
uint32_t tui_adjust_hsv(uint32_t rgb, double dh, double ms, double mv);
double tui_color_luma(uint32_t rgb);          /* gamma-encoded BT.709, 0..1 */
double tui_relative_luminance(uint32_t rgb);  /* WCAG, 0..1 */
uint32_t tui_palette_rgb(int index);          /* xterm-256 palette entry */

/* Resolved hex of a theme slot: palette indices map to RGB, "" (terminal default) maps to
 * #e5e5e7 on dark themes and #000000 on light ones (upstream Theme.getColorHex). */
uint32_t tui_theme_rgb(const tui_theme_t *th, tui_tc_t slot);
/* status-line background luma > 0.5 (upstream Theme.isLight) */
bool tui_theme_is_light(const tui_theme_t *th);
/* foreground escape for an RGB color in the theme's color mode (writes into buf, >= 24 bytes) */
const char *tui_rgb_fg(const tui_theme_t *th, uint32_t rgb, char *buf, size_t n);

/* Stable accent color for a session title on the given theme (upstream getSessionAccentHex). */
uint32_t tui_session_accent(const char *name, const tui_theme_t *th);

#endif
