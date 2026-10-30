/*
 * markdown.h - Markdown -> themed ANSI lines, plus a keyword-table syntax highlighter.
 *
 * Supported: ATX headings (# .. ######), paragraphs (source line breaks preserved),
 * **bold** / __bold__, *italic* / _italic_, ~~strike~~, `code`, fenced code blocks
 * (``` / ~~~, unclosed fences render to the end: streaming-safe) with highlighting,
 * ordered/unordered/nested lists, blockquotes (nested), GFM tables (box drawing, column
 * alignment, cell wrapping), [links](url) / <autolinks> / bare URLs (OSC 8), horizontal
 * rules, backslash escapes. Nesting depth is capped (MD_MAX_DEPTH).
 */
#ifndef OMC_TUI_MARKDOWN_H
#define OMC_TUI_MARKDOWN_H

#include "tui/render.h"

typedef struct {
    const tui_theme_t *theme; /* NULL = current theme */
    bool hyperlinks;          /* emit OSC 8 links (default true); else append " (url)" */
    int code_indent;          /* code block body indent (default 2) */
    int text_slot;            /* default prose color slot, -1 = terminal default */
} tui_md_opts_t;

void tui_md_opts_default(tui_md_opts_t *o);
/* Render markdown for `width` columns; appends owned lines to out. */
void tui_md_render(const char *text, int width, const tui_md_opts_t *opts, strvec_t *out);
/* Render inline markdown (no wrapping) into an ANSI string (owned). */
char *tui_md_inline(const char *text, const tui_md_opts_t *opts);

/* Highlight source code; appends one owned line per source line. Unknown language ->
 * mdCodeBlock color. Language names: c, cpp, js, ts, jsx, tsx, python/py, rust/rs, go,
 * java, kotlin, sh/bash/zsh/shell, json, yaml/yml, sql, ruby/rb, lua, diff, ... */
void tui_highlight_code(const char *code, const char *lang, const tui_theme_t *th, strvec_t *out);
bool tui_highlight_supported(const char *lang);

/* Component wrapper with padding and a (text, width) render cache. */
typedef struct {
    tui_component_t base;
    char *text;
    int padding_x, padding_y;
    tui_md_opts_t opts;
    char *cache_text;
    int cache_width;
    strvec_t cache;
} tui_markdown_t;

tui_markdown_t *tui_markdown_new(const char *text, int padding_x, int padding_y);
void tui_markdown_set_text(tui_markdown_t *m, const char *text);
void tui_markdown_append(tui_markdown_t *m, const char *delta); /* streaming */

#endif
