/* fuzz_ansi - ANSI/UTF-8 width, slicing, wrapping, truncation and SGR tracking (src/tui/ansi.c) on arbitrary bytes
 * (tool output and model text reach these). Input: <width byte><start byte><text>.
 * Properties: wrapped lines and truncations fit the width; slices never exceed the requested length. */
#include "core/core.h"
#include "tui/ansi.h"
#include "fuzz.h"

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 1 << 14) return 0;
    int width = (int)(fuzz_byte(&data, &size) % 120) + 1;
    int start = (int)(fuzz_byte(&data, &size) % 64);
    char *s = fuzz_cstr(data, size);
    (void)tui_visible_width(s);
    (void)tui_visible_width_n(s, size);
    for (size_t i = 0; i < size;) {
        size_t k = tui_esc_len(s + i, size - i);
        FUZZ_ASSERT(k <= size - i);
        i += k ? k : 1;
    }
    char *t = tui_truncate_to_width(s, width, "…");
    int tw = tui_visible_width(t);
    if (tw > width) {
        fprintf(stderr, "truncate(%d) gave %d columns\n", width, tw);
        FUZZ_ASSERT(tw <= width);
    }
    free(t);
    char *sl = tui_slice_cols(s, start, width);
    int sw = tui_visible_width(sl);
    if (sw > width) {
        fprintf(stderr, "slice(%d,%d) gave %d columns\n", start, width, sw);
        FUZZ_ASSERT(sw <= width);
    }
    free(sl);
    strvec_t out = {0};
    tui_wrap_text_with_ansi(s, width, &out);
    for (size_t i = 0; i < out.n; i++) {
        int w = tui_visible_width(out.v[i]);
        if (w > width && width >= 2) {
            fprintf(stderr, "wrap(%d) line %zu has %d columns\n", width, i, w);
            FUZZ_ASSERT(w <= width);
        }
    }
    strvec_free(&out);
    char *p = tui_pad_to_width(s, width);
    free(p);
    char *r = tui_replace_tabs(s);
    free(r);
    char *st = tui_strip_ansi(s);
    free(st);
    tui_sgr_t g;
    tui_sgr_reset(&g);
    for (size_t i = 0; i < size;) {
        size_t k = tui_esc_len(s + i, size - i);
        if (k) tui_sgr_feed(&g, s + i, k);
        i += k ? k : 1;
    }
    sb_t b = SB_INIT;
    tui_sgr_emit(&g, &b);
    tui_sgr_emit_close(&g, &b);
    sb_free(&b);
    free(s);
    return 0;
}
