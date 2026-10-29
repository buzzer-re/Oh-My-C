/*
 * ansi.h - ANSI-aware text measurement and layout helpers for the TUI.
 *
 * All helpers share one width model: escape sequences (CSI, OSC, DCS, APC, PM, SOS and
 * two/three byte escapes) are zero width, code points use wcwidth_cp(), tabs count
 * TUI_TAB_WIDTH columns. Returned `char *` values are owned by the caller.
 */
#ifndef OMC_TUI_ANSI_H
#define OMC_TUI_ANSI_H

#include "core/core.h"

#define TUI_TAB_WIDTH 3
/* Zero-width APC marker a focused component emits where the hardware cursor belongs. */
#define TUI_CURSOR_MARKER "\x1b_pi:c\x07"
#define TUI_RESET "\x1b[0m"
#define TUI_LINK_CLOSE "\x1b]8;;\x07"

/* Length of the escape sequence starting at s[0] == ESC; 0 when incomplete. */
size_t tui_esc_len(const char *s, size_t n);

/* Column width of a code point (tab = TUI_TAB_WIDTH). */
int tui_cp_width(uint32_t cp);
int tui_visible_width(const char *s);
int tui_visible_width_n(const char *s, size_t n);

/* SGR (graphic rendition) state, used to carry styling across wrapped/sliced lines. */
typedef struct {
    bool bold, dim, italic, underline, blink, inverse, hidden, strike;
    char fg[40], bg[40], ulc[40]; /* raw parameter text, "" when default */
    char link[512];               /* active OSC 8 hyperlink params+uri ("" none) */
} tui_sgr_t;

void tui_sgr_reset(tui_sgr_t *st);
/* Update state from an escape sequence (only SGR and OSC 8 matter). */
void tui_sgr_feed(tui_sgr_t *st, const char *seq, size_t n);
bool tui_sgr_active(const tui_sgr_t *st); /* any non-default attribute (excluding link) */
void tui_sgr_emit(const tui_sgr_t *st, sb_t *out);      /* re-establish SGR (+link) state */
void tui_sgr_emit_close(const tui_sgr_t *st, sb_t *out); /* reset + close link if needed */

/* Truncate to `width` columns. If truncation happens, `ellipsis` (NULL -> "…", "" -> none)
 * is appended after a reset. Escape sequences are preserved. */
char *tui_truncate_to_width(const char *s, int width, const char *ellipsis);
/* ANSI-aware column slice [start, start+len). Active SGR state at `start` is re-emitted as a
 * prefix; wide characters straddling a boundary become spaces. Result is not padded. */
char *tui_slice_cols(const char *s, int start, int len);
/* Word wrap preserving SGR/hyperlink state across lines. Splits on '\n' first. Appends owned
 * lines to `out`; always appends at least one line. */
void tui_wrap_text_with_ansi(const char *s, int width, strvec_t *out);
/* Pad with spaces to `width` columns (no truncation). */
char *tui_pad_to_width(const char *s, int width);
void tui_sb_spaces(sb_t *b, int n);
char *tui_replace_tabs(const char *s);
char *tui_strip_ansi(const char *s);

#endif
