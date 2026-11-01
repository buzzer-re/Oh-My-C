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
    bool hyperlinks;          /* wrap links in OSC 8 (default true); " (url)" follows text that differs */
    int code_indent;          /* code block body indent (default 2); 0 = literal rows outside the margins */
    int text_slot;            /* default prose color slot, -1 = terminal default */
    const char *text_sgr;     /* default prose SGR (overrides text_slot), e.g. a contrast fg on a bg */
    bool italic;              /* default prose italic (thinking blocks) */
    int padding_x;            /* left margin applied by tui_md_render (width is then the full width) */
    int bg_slot;              /* background slot for every line, padded to width; -1 = none (default) */
} tui_md_opts_t;

void tui_md_opts_default(tui_md_opts_t *o);
/* whether OSC 8 links are on by default for this terminal (upstream terminal-capabilities policy) */
bool tui_md_hyperlinks_supported(void);
/* Render markdown for `width` columns (upstream Markdown component: marked-style block tokens with the
 * same per-token spacing, list/table/quote layout and code rows); appends owned lines to out. */
void tui_md_render(const char *text, int width, const tui_md_opts_t *opts, strvec_t *out);
/* Render inline markdown (no wrapping) into an ANSI string (owned). */
char *tui_md_inline(const char *text, const tui_md_opts_t *opts);

/* Highlight source code (highlight.c); appends one owned line per source line, colored with the theme's
 * syntax* slots the way omp's syntect highlighter maps scopes. Unsupported languages come back verbatim.
 * Language names: c, cpp, js, ts, jsx, tsx, python/py, rust/rs, go, java, kotlin, sh/bash/zsh/shell, json,
 * yaml/yml, toml/ini, sql, ruby/rb, lua, diff, html/xml, css ... */
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
