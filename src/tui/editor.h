/*
 * editor.h - multi-line input editor (pi-tui Editor).
 *
 * Keys (via the global keybindings, tui.editor.* / tui.input.*): arrows, ctrl+b/f,
 * alt+b/f / ctrl+left/right (word), home/end/ctrl+a/e, backspace/delete/ctrl+d,
 * ctrl+w / alt+backspace (delete word back), alt+d (word forward), ctrl+u / ctrl+k,
 * ctrl+y (yank last kill), ctrl+- / ctrl+_ (undo), enter (submit), shift+enter /
 * alt+enter / ctrl+j (newline), tab (autocomplete), up/down (history on the first/last
 * visual line when the buffer is empty or while browsing history), page up/down.
 *
 * Large pastes (> 10 lines or > 1000 chars) are collapsed into "[Paste #N, +L lines]" /
 * "[Paste #N, C chars]" markers that delete atomically and expand on submit.
 *
 * Rendering: top border, word-wrapped content (cursor drawn in reverse video plus
 * TUI_CURSOR_MARKER when focused), bottom border, then the autocomplete list if open. With a
 * composer style set (tui_editor_set_composer) the frame, prompt gutter, status attachment and
 * right-aligned placeholder follow that omp composer shape instead.
 */
#ifndef OMC_TUI_EDITOR_H
#define OMC_TUI_EDITOR_H

#include "tui/components.h"
#include "tui/composer.h"

/* ---------- autocomplete ---------- */
typedef struct {
    size_t prefix_len; /* bytes before the cursor (current line) replaced by an accepted value */
    VEC(tui_select_item_t) items;
    bool slash;        /* slash-command completions (popup uses the slash-command layout) */
    bool first_desc_match; /* top item matched only by description: Enter submits the raw text */
} tui_ac_result_t;

void tui_ac_result_add(tui_ac_result_t *r, const char *value, const char *label, const char *description);
void tui_ac_result_add_icon(tui_ac_result_t *r, const char *value, const char *label, const char *description,
                            const char *icon);
void tui_ac_result_free(tui_ac_result_t *r);

/* Provider: fill `out` for the text before the cursor on the current line. `first_line` is
 * true when the cursor is on the buffer's first line. `forced` is true for Tab. Return
 * true if there are suggestions. Accepting a value ending in '/' keeps completion open. */
typedef bool (*tui_autocomplete_fn)(void *ud, const char *line_before, bool first_line, bool forced,
                                    tui_ac_result_t *out);

/* Built-in provider for "/commands" (first line) and "@path" / Tab path completion. */
typedef struct {
    strvec_t commands;     /* names without '/' */
    strvec_t descriptions; /* parallel to commands ("" if none) */
    strvec_t icons;        /* parallel to commands ("" if none) */
    /* optional live display description (owned result, NULL = static one); matching uses the static one */
    char *(*describe)(void *ud, const char *name);
    void *describe_ud;
    char *cwd;             /* NULL = process cwd */
    int max_items;         /* default 50 */
} tui_ac_default_t;

tui_ac_default_t *tui_ac_default_new(const char *cwd);
void tui_ac_default_add_command(tui_ac_default_t *p, const char *name, const char *description);
void tui_ac_default_add_command_icon(tui_ac_default_t *p, const char *name, const char *description, const char *icon);
void tui_ac_default_free(tui_ac_default_t *p);
bool tui_ac_default_provider(void *ud, const char *line_before, bool first_line, bool forced, tui_ac_result_t *out);

/* ---------- editor ---------- */
typedef struct tui_editor tui_editor_t;
typedef void (*tui_editor_submit_fn)(tui_editor_t *ed, const char *text, void *ud);
typedef void (*tui_editor_change_fn)(tui_editor_t *ed, void *ud);

typedef struct {
    strvec_t lines;
    size_t cl, cc;
} tui_editor_snapshot_t;

typedef struct {
    int id;
    char *marker;
    char *content;
} tui_editor_paste_t;

struct tui_editor {
    tui_component_t base;
    strvec_t lines; /* always >= 1 line */
    size_t cl, cc;  /* cursor line / byte column */
    int preferred_col;
    VEC(tui_editor_snapshot_t) undo;
    int last_action;
    char *kill;
    strvec_t history; /* oldest first */
    int hist_index;   /* -1 = editing draft */
    char *hist_draft;
    VEC(tui_editor_paste_t) pastes;
    int paste_counter;

    tui_autocomplete_fn ac_fn;
    void *ac_ud;
    tui_select_list_t *ac_list; /* non-NULL while autocomplete is open */
    size_t ac_prefix_len;
    bool ac_enter_accepts;
    int ac_max_visible;

    tui_editor_submit_fn on_submit;
    tui_editor_change_fn on_change;
    void *cb_ud;

    int padding_x;
    int max_height;   /* max visible content rows, 0 = unlimited */
    int scroll;       /* first visible visual row */
    int layout_width; /* from the last render (used for up/down) */
    int border_slot;  /* theme slot for borders */
    bool disable_submit;
    char *placeholder;

    /* composer chrome (tui/composer.h); NULL = classic rules above and below */
    const tui_composer_style_t *composer;
    tui_composer_color_fn chrome_border, chrome_accent, chrome_surface;
    void *chrome_ud;
    /* status content for the composer's top chrome; returns owned styled text and its width */
    char *(*top_fn)(void *ud, int avail, int *width);
    void *top_ud;
    /* right-aligned styled hint painted after the cursor while the composer is empty */
    char *(*placeholder_fn)(void *ud);
    void *placeholder_ud;
    char *gutter;        /* prompt gutter override (NULL = the style's default) */
    char text_fg[32];    /* foreground escape for typed text ("" = terminal default) */
    int theme_padding_x; /* theme's editor padding request (-1 = unset) */
    bool use_terminal_cursor; /* park the hardware cursor instead of drawing a cursor cell */
    const char *eol_cursor;   /* one-column end-of-line caret glyph (theme inputCursor); NULL = reverse-video cell */
};

tui_editor_t *tui_editor_new(void);
void tui_editor_set_text(tui_editor_t *ed, const char *text); /* cursor to end */
char *tui_editor_get_text(const tui_editor_t *ed);            /* owned, raw (markers kept) */
char *tui_editor_get_expanded_text(const tui_editor_t *ed);   /* owned, paste markers expanded */
void tui_editor_insert_text(tui_editor_t *ed, const char *text); /* at cursor, one undo step */
void tui_editor_paste(tui_editor_t *ed, const char *text);       /* same path as bracketed paste */
bool tui_editor_is_empty(const tui_editor_t *ed);
void tui_editor_clear(tui_editor_t *ed);
void tui_editor_undo(tui_editor_t *ed);
void tui_editor_add_history(tui_editor_t *ed, const char *text);
void tui_editor_set_on_submit(tui_editor_t *ed, tui_editor_submit_fn fn, void *ud);
void tui_editor_set_on_change(tui_editor_t *ed, tui_editor_change_fn fn);
void tui_editor_set_autocomplete(tui_editor_t *ed, tui_autocomplete_fn fn, void *ud);
void tui_editor_set_placeholder(tui_editor_t *ed, const char *text);
bool tui_editor_autocomplete_open(const tui_editor_t *ed);
/* Composer chrome: render through the named composer shape (box, band, claude, pi, borderless,
 * rule, field, rail; NULL restores the classic rules). */
void tui_editor_set_composer(tui_editor_t *ed, const char *shape);
void tui_editor_set_chrome_colors(tui_editor_t *ed, tui_composer_color_fn border, tui_composer_color_fn accent,
                                  tui_composer_color_fn surface, void *ud);
void tui_editor_set_top_provider(tui_editor_t *ed, char *(*fn)(void *ud, int avail, int *width), void *ud);
void tui_editor_set_placeholder_fn(tui_editor_t *ed, char *(*fn)(void *ud), void *ud);
/* Feed a key (same as the component's handle_key). */
bool tui_editor_handle_key(tui_editor_t *ed, const tui_key_t *k);

#endif
