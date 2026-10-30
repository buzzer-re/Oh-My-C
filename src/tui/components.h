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

/* ---------- SelectList: filterable, scrolling list ---------- */
typedef struct {
    char *value;
    char *label;       /* NULL -> value */
    char *description; /* optional */
} tui_select_item_t;

typedef struct tui_select_list tui_select_list_t;
typedef void (*tui_select_fn)(tui_select_list_t *l, const tui_select_item_t *item, void *ud);
typedef void (*tui_select_cancel_fn)(tui_select_list_t *l, void *ud);

struct tui_select_list {
    tui_component_t base;
    VEC(tui_select_item_t) items;
    VEC(size_t) visible; /* indices into items after filtering */
    size_t selected;     /* index into visible */
    int max_visible;
    char *filter;
    bool filter_enabled; /* typed text edits the filter (default true) */
    tui_select_fn on_select;
    tui_select_fn on_change;
    tui_select_cancel_fn on_cancel;
    void *ud;
};

tui_select_list_t *tui_select_list_new(int max_visible);
void tui_select_list_add(tui_select_list_t *l, const char *value, const char *label, const char *description);
void tui_select_list_clear(tui_select_list_t *l);
void tui_select_list_set_filter(tui_select_list_t *l, const char *filter);
const tui_select_item_t *tui_select_list_selected(const tui_select_list_t *l);
void tui_select_list_move(tui_select_list_t *l, int delta, bool wrap);
size_t tui_select_list_visible_count(const tui_select_list_t *l);
/* Subsequence fuzzy score (higher is better); -1 when `query` doesn't match `text`. */
int tui_fuzzy_score(const char *query, const char *text);

#endif
