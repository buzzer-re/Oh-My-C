/* fuzz_markdown - the markdown renderer (src/tui/markdown.c: blocks, inline spans, tables, HTML via
 * normalizeHtmlForTerminal, code highlighting) on model output. Input: <width byte><flags byte><markdown>.
 * Properties: every rendered line fits the width (no line wider than requested), streaming (append in chunks) never
 * crashes, and the code-block extractor and inline renderer accept anything. */
#include "core/core.h"
#include "tui/markdown.h"
#include "tui/ansi.h"
#include "tui/theme.h"
#include "fuzz.h"

FUZZ_SETUP { tui_theme_set(tui_theme_builtin("dark", TUI_COLOR_TRUECOLOR)); }

static void on_block(void *ud, bool code, const char *lang, char *text) {
    (void)ud;
    (void)code;
    (void)lang;
    free(text);
}

FUZZ_ENTRY {
    if (size > 1 << 15) return 0;
    int width = (int)(fuzz_byte(&data, &size) % 160) + 1;
    unsigned fl = fuzz_byte(&data, &size);
    char *s = fuzz_cstr(data, size);
    tui_md_opts_t o;
    tui_md_opts_default(&o);
    o.hyperlinks = fl & 1;
    o.italic = fl & 2;
    o.padding_x = (fl >> 2) & 3;
    o.code_indent = (fl >> 4) & 3;
    if (fl & 0x40) o.bg_slot = 0;
    strvec_t out = {0};
    tui_md_render(s, width, &o, &out);
    for (size_t i = 0; i < out.n; i++) {
        int w = tui_visible_width(out.v[i]);
        if (w > width && width > 2 * o.padding_x + 4) {
            fprintf(stderr, "line %zu is %d columns wide, width %d: %s\n", i, w, width, out.v[i]);
            FUZZ_ASSERT(w <= width);
        }
    }
    strvec_free(&out);
    char *inl = tui_md_inline(s, &o);
    free(inl);
    tui_md_extract_blocks(s, on_block, NULL);
    (void)tui_md_ends_in_paragraph(s);
    if (fl & 0x80) {
        tui_markdown_t *m = tui_markdown_new("", 1, 0);
        size_t step = (fl & 0x3f) + 1;
        for (size_t i = 0; i < size; i += step) {
            char c = s[MIN(i + step, size)];
            s[MIN(i + step, size)] = 0;
            tui_markdown_append(m, s + i);
            s[MIN(i + step, size)] = c;
            if ((i / step) % 8 == 0) {
                strvec_t o2 = {0};
                tui_component_render(&m->base, width, &o2);
                strvec_free(&o2);
            }
        }
        strvec_t o2 = {0};
        tui_component_render(&m->base, width, &o2);
        strvec_free(&o2);
        tui_component_destroy(&m->base);
    }
    char *hold = xstrdup(s);
    tui_md_hold_pending_bullet(hold);
    free(hold);
    free(s);
    return 0;
}
