/*
 * components.h - basic components: Text, Box, Spacer, Loader, SelectList.
 * All are tui_component_t derivatives (embed `base` first); destroy with
 * tui_component_destroy(&x->base) unless owned by a container.
 */
#ifndef OMC_TUI_COMPONENTS_H
#define OMC_TUI_COMPONENTS_H

#include "tui/render.h"

/* ---------- Text: word-wrapped text with padding and optional background ---------- */
typedef struct {
    tui_component_t base;
    char *text;
    int padding_x, padding_y;
    int bg_slot; /* theme slot for background, -1 = none */
    /* cache */
    char *cache_text;
    int cache_width;
    strvec_t cache;
} tui_text_t;

tui_text_t *tui_text_new(const char *text, int padding_x, int padding_y);
/* tui.tight (setTuiTight / getPaddingX): components that honor it drop one column of horizontal padding */
void tui_set_tight(bool on);
bool tui_tight(void);
int tui_padding_x(int base);
void tui_text_set(tui_text_t *t, const char *text);
void tui_text_set_bg(tui_text_t *t, int slot);

/* ---------- Truncated text: single line, truncated with ellipsis ---------- */
typedef struct {
    tui_component_t base;
    char *text;
    int padding_x;
} tui_trunc_text_t;
tui_trunc_text_t *tui_trunc_text_new(const char *text, int padding_x);
void tui_trunc_text_set(tui_trunc_text_t *t, const char *text);

/* ---------- Box: container with padding and optional background ---------- */
typedef struct {
    tui_container_t c; /* children rendered at width - 2*padding_x */
    int padding_x, padding_y;
    int bg_slot; /* -1 none */
} tui_box_t;

tui_box_t *tui_box_new(int padding_x, int padding_y, int bg_slot);

/* ---------- Spacer ---------- */
typedef struct {
    tui_component_t base;
    int lines;
} tui_spacer_t;
tui_spacer_t *tui_spacer_new(int lines);

/* ---------- Loader: spinner + message, animated by a TUI timer ---------- */
typedef struct {
    tui_component_t base;
    tui_t *tui;
    char *message;
    int frame;
    int timer_id;
    int interval_ms;
    int spinner_slot;  /* default accent */
    int message_slot;  /* default muted */
    bool leading_blank; /* render an empty line above (pi-tui style), default true */
} tui_loader_t;

tui_loader_t *tui_loader_new(tui_t *tui, const char *message);
void tui_loader_set_message(tui_loader_t *l, const char *message);
void tui_loader_start(tui_loader_t *l); /* starts the animation timer (UI thread) */
void tui_loader_stop(tui_loader_t *l);

/* ---------- SelectList: filterable, scrolling list (port of omp components/select-list.ts) ----------
 * Rows: `<cursor> [icon] label<pad to primary column>description`, the selected row in accent, the
 * description muted; a scrollbar column (muted track │ / accent thumb █) appears when rows overflow
 * max_visible; typing edits a search query (fuzzy, upstream ranking) when the list overflows
 * (search policy "overflow", default) and a status row shows "  Type to search" / "  Search: q". */
typedef struct {
    char *value;
    char *label;       /* NULL -> value */
    char *description; /* optional */
    char *icon;        /* optional glyph column */
    char *hint;        /* optional extra search text */
    bool disabled;     /* rendered muted, skipped by navigation */
} tui_select_item_t;

enum { TUI_SELECT_SEARCH_OVERFLOW, TUI_SELECT_SEARCH_ALWAYS, TUI_SELECT_SEARCH_NEVER };

typedef struct {
    int min_primary, max_primary; /* primary (label) column bounds; 0 -> 32 */
    int search;                   /* TUI_SELECT_SEARCH_* */
    bool no_wrap_navigation;      /* up/down stop at the edges */
    bool wrap_description;        /* wrap long descriptions onto extra rows */
    int max_description_rows;     /* cap for wrapped descriptions (0 = none) */
    const char *empty_text;       /* default "No items" */
    const char *no_match_text;    /* default "No matching items" */
} tui_select_layout_t;

typedef struct tui_select_list tui_select_list_t;
typedef void (*tui_select_fn)(tui_select_list_t *l, const tui_select_item_t *item, void *ud);
typedef void (*tui_select_cancel_fn)(tui_select_list_t *l, void *ud);

struct tui_select_list {
    tui_component_t base;
    VEC(tui_select_item_t) items;
    VEC(size_t) visible; /* indices into items after filtering */
    size_t selected;     /* index into visible */
    int max_visible;
    char *filter;        /* current search query (NULL = none) */
    bool filter_enabled; /* false forces search policy "never" (editor autocomplete) */
    tui_select_layout_t layout;
    char *cursor;        /* cursor glyph captured at creation (nav.cursor) */
    tui_select_fn on_select;
    tui_select_fn on_change;
    tui_select_cancel_fn on_cancel;
    void *ud;
};

tui_select_list_t *tui_select_list_new(int max_visible);
void tui_select_list_add(tui_select_list_t *l, const char *value, const char *label, const char *description);
/* add with an icon and disabled flag; returns the item (owned by the list) for further fields */
tui_select_item_t *tui_select_list_add_item(tui_select_list_t *l, const char *value, const char *label,
                                            const char *description, const char *icon, bool disabled);
void tui_select_list_clear(tui_select_list_t *l);
void tui_select_list_set_filter(tui_select_list_t *l, const char *filter);
const tui_select_item_t *tui_select_list_selected(const tui_select_list_t *l);
void tui_select_list_move(tui_select_list_t *l, int delta, bool wrap);
void tui_select_list_set_selected(tui_select_list_t *l, size_t visible_index);
bool tui_select_list_select_value(tui_select_list_t *l, const char *value);
void tui_select_list_set_max_visible(tui_select_list_t *l, int rows);
size_t tui_select_list_visible_count(const tui_select_list_t *l);
/* Subsequence fuzzy score (higher is better); -1 when `query` doesn't match `text`. */
int tui_fuzzy_score(const char *query, const char *text);

#endif
