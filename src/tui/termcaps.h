/*
 * termcaps.h - terminal capabilities beyond keyboard/notifications (port of omp packages/tui
 * terminal-capabilities.ts hyperlink + image-protocol policy, render/hyperlink.ts, kitty-graphics.ts,
 * sixel.ts and components/image.ts).
 *
 * Hyperlinks: OSC 8 is detected per terminal (shouldEnableHyperlinksByDefault) and then gated by the
 * `tui.hyperlinks` policy the host pushes (off / auto / always). Before a policy is pushed the detected
 * capability stands, as omp's TERMINAL.hyperlinks does.
 *
 * Images: the protocol is Kitty graphics, iTerm2 inline images (OSC 1337) or SIXEL, resolved from the
 * terminal id (PI_FORCE_IMAGE_PROTOCOL overrides) and upgraded to SIXEL at runtime when DA1 reports
 * attribute 4 / XTSMGRAPHICS reports a geometry. Kitty images are transmitted once per image id (queued
 * and written before the next frame) and displayed by Unicode placeholders where the terminal supports
 * them, else by a direct placement on the image block's last row. SIXEL is encoded here from scratch
 * (decode with src/img, resize, median-cut palette, Floyd-Steinberg dither).
 */
#ifndef OMC_TUI_TERMCAPS_H
#define OMC_TUI_TERMCAPS_H

#include "core/core.h"

#include <stdint.h>

/* ---------- hyperlinks (tui.hyperlinks) ---------- */
bool tui_hyperlinks_detected(void);                 /* shouldEnableHyperlinksByDefault (cached) */
bool tui_hyperlinks_detect(const char *terminal_id); /* the same, uncached, for a given id (tests) */
void tui_hyperlinks_apply_setting(const char *mode); /* applyHyperlinkSetting: "off" | "auto" | "always" */
bool tui_hyperlinks_enabled(void);                  /* TERMINAL.hyperlinks */

/* ---------- image protocol ---------- */
typedef enum { TUI_IMG_NONE = 0, TUI_IMG_KITTY, TUI_IMG_ITERM2, TUI_IMG_SIXEL } tui_img_proto_t;

tui_img_proto_t tui_image_protocol(void);
void tui_set_image_protocol(tui_img_proto_t p);
bool tui_image_protocol_forced(void); /* PI_FORCE_IMAGE_PROTOCOL set (incl. off) */
/* resolveImageProtocol for a terminal id (tmux passthrough and multiplexer carve-outs included) */
tui_img_proto_t tui_resolve_image_protocol(const char *terminal_id, bool is_tty);
bool tui_kitty_placeholders(void);                       /* Unicode placeholders in use */
bool tui_kitty_placeholders_detect(const char *terminal_id); /* detectKittyUnicodePlaceholdersSupport */
void tui_cell_dims(int *w_px, int *h_px);                /* default 9x18 until the terminal answers CSI 16 t */
void tui_set_cell_dims(int w_px, int h_px);
uint32_t tui_terminal_background(void);                  /* 0xRRGGBB for SIXEL alpha compositing */
void tui_set_terminal_background_light(bool light);

/* Terminal replies the image layer consumes: CSI 6;h;w t (cell size), DA1 (SIXEL attribute 4),
 * XTSMGRAPHICS (CSI ? 2 ; status ; geometry S), iTerm2 ReportCellSize. Returns true when consumed;
 * *changed is set when the protocol or cell size changed (repaint). */
bool tui_termcaps_response(const char *seq, size_t n, bool *changed);
/* Startup probes to write (owned, may be ""): XTSMGRAPHICS when no protocol is known, CSI 16 t when one is */
char *tui_termcaps_probes(void);

/* ---------- encoders ---------- */
char *tui_kitty_encode(const char *b64, int cols, int rows);  /* a=T transmit-and-display */
char *tui_kitty_transmit(const char *b64, uint32_t image_id); /* a=t, chunked */
char *tui_kitty_placement(uint32_t image_id, uint32_t placement_id, int cols, int rows);
char *tui_kitty_delete_all(void);
/* placeholder grid: `rows` lines, the virtual placement APC heading line 0 */
void tui_kitty_placeholder_lines(uint32_t image_id, uint32_t placement_id, int cols, int rows, strvec_t *out);
char *tui_iterm2_encode(const char *b64, int width_cells, int height_cells, bool preserve_aspect);
/* SIXEL DCS for an encoded image (PNG/JPEG/GIF/WebP bytes) at w x h pixels; NULL on decode failure */
char *tui_sixel_encode(const uint8_t *data, size_t n, int w_px, int h_px, uint32_t background_rgb);
char *tui_sixel_ensure_raster_width(char *seq); /* ensureSixelRasterWidth (takes ownership) */

/* calculateImageFit */
void tui_image_fit(int img_w, int img_h, int max_cols, int max_rows, int cell_w, int cell_h, int *cols, int *rows);
/* imageFallback: `[Image: name [mime] WxH]` (w/h <= 0: no dimensions) */
char *tui_image_fallback(const char *mime, int w, int h, const char *filename);
/* the line carries an image marker (isImageLine) */
bool tui_is_image_line(const char *line);
/* rows an image direct-placement line paints above its own row (directPlacementLift) */
int tui_image_lift(const char *line);

/* ---------- Image component ---------- */
typedef struct {
    int max_cols; /* 0: no cap (beyond the width) */
    int max_rows; /* 0: no cap */
} tui_image_opts_t;
/* resolveImageOptions: tui.maxInlineImageColumns / tui.maxInlineImageRows (defaults 100 / 20; rows 0 = 60% of
 * the viewport) against the viewport height */
tui_image_opts_t tui_image_default_opts(int viewport_rows, int max_cols_setting, int max_rows_setting);
/* Image.render: the rows of one inline image at `width` columns (graphics when a protocol is active,
 * else the text fallback colored by `fallback_sgr`). `key` names the logical image so repaints reuse
 * its Kitty id. A needed Kitty transmit is queued (tui_image_take_pending). */
void tui_image_render(const char *b64, const char *mime, const char *key, const char *filename, int width,
                      const tui_image_opts_t *o, const char *fallback_sgr, strvec_t *out);
/* queued one-time Kitty transmits, written before the next frame (owned; NULL when none) */
char *tui_image_take_pending(void);
/* destructive display reset: the Kitty delete-all sequence (NULL without Kitty images) and every transmitted
 * image queued again */
char *tui_image_reset_display(void);

#endif
