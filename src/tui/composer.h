/*
 * composer.h - composer chrome styles (port of omp packages/tui/src/components/composer/).
 *
 * A style owns how the input editor's frame looks: top/bottom chrome, per-row side chrome,
 * default padding and prompt gutter, plus where the host attaches the status bar. The editor
 * (tui_editor_set_composer) renders through it.
 *
 * Built-in shapes: box (rounded frame, status in the top border), band (omp default: flush
 * status band above a `╰─ ` prompt), claude, pi, borderless, rule, field, rail.
 */
#ifndef OMC_TUI_COMPOSER_H
#define OMC_TUI_COMPOSER_H

#include "core/core.h"

typedef enum { TUI_ATTACH_TOP_BORDER, TUI_ATTACH_TOP_BAND, TUI_ATTACH_TOP_RULE_CHIP, TUI_ATTACH_NONE } tui_status_attach_t;
typedef enum { TUI_BOTTOM_NONE, TUI_BOTTOM_LEFT, TUI_BOTTOM_FULL } tui_bottom_bar_t;

/* Styling functions return owned strings; `ud` is tui_composer_ctx_t.ud. */
typedef char *(*tui_composer_color_fn)(void *ud, const char *s);

typedef struct {
    int width;     /* full width available to the composer */
    int padding_x; /* horizontal padding inside side chrome */
    tui_composer_color_fn border, accent, surface;
    void *ud;
    const char *top_content; /* status content for the top chrome (styled), NULL = none */
    int top_width;
} tui_composer_ctx_t;

typedef struct {
    const tui_composer_ctx_t *ctx;
    const char *text;   /* decorated row text */
    int pad;            /* spaces padding text to the content width */
    const char *gutter; /* prompt gutter cells ("" when none) */
    bool is_last_row;
    int cursor_overflow;
    bool ime_safe_tail;
    bool scrollbar_thumb;
} tui_composer_row_t;

typedef struct tui_composer_style {
    const char *id;
    bool filled_surface;
    bool side_borders;
    int vertical_chrome;
    tui_status_attach_t status_attachment;
    tui_bottom_bar_t bottom_bar;
    bool bottom_bar_gap;
    const char *default_gutter; /* NULL = none */
    int (*default_padding_x)(int theme_padding_x); /* theme_padding_x < 0: unset */
    int (*side_chrome_width)(int padding_x);
    char *(*render_top)(const tui_composer_ctx_t *ctx);    /* NULL result = no row */
    void (*render_row)(const tui_composer_row_t *row, strvec_t *out);
    char *(*render_bottom)(const tui_composer_ctx_t *ctx); /* NULL result = no row */
} tui_composer_style_t;

/* Style for a shape id; unknown ids fall back to box (upstream getComposerStyle). */
const tui_composer_style_t *tui_composer_style(const char *id);
bool tui_composer_is_builtin(const char *id);
/* Built-in ids in upstream order: box band claude pi borderless rule field rail. */
extern const char *const TUI_COMPOSER_SHAPES[8];

#endif
