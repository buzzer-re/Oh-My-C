/* modelbrowser.h - searchable model list (port of omp overlays/model-browser.ts ModelBrowser). */
#ifndef OMC_UI_MODELBROWSER_H
#define OMC_UI_MODELBROWSER_H

#include "ai/ai.h"
#include "tui/keys.h"

typedef struct {
    const ai_model_t *model;
    char *selector; /* provider/id */
} ui_model_item_t;

typedef struct {
    VEC(ui_model_item_t) items;
    VEC(size_t) visible;
    size_t selected; /* index into visible */
    int window;      /* first visible row */
    int max_visible;
    char *query;
    char *current;    /* current model selector (green dot) */
    bool focused, show_provider;
    bool single_pool; /* roles resolve over a single candidate (role chips) */
} ui_model_browser_t;

enum { UI_MB_IGNORED, UI_MB_HANDLED, UI_MB_ACTIVATE, UI_MB_CANCEL };

ui_model_browser_t *ui_model_browser_new(void);
void ui_model_browser_free(ui_model_browser_t *b);
void ui_model_browser_add(ui_model_browser_t *b, const ai_model_t *m);
void ui_model_browser_done_adding(ui_model_browser_t *b);
void ui_model_browser_load_available(ui_model_browser_t *b);
void ui_model_browser_set_current(ui_model_browser_t *b, const char *selector);
void ui_model_browser_set_max_visible(ui_model_browser_t *b, int rows);
const ui_model_item_t *ui_model_browser_selected(const ui_model_browser_t *b);
int ui_model_browser_key(ui_model_browser_t *b, const tui_key_t *k); /* UI_MB_* */
/* search row + blank + max_visible rows + blank + 2 detail rows */
void ui_model_browser_render(const ui_model_browser_t *b, int width, strvec_t *out);
char *ui_format_number(double n); /* upstream formatNumber: 4096 -> "4.1K" */

#endif
