/* modelbrowser.h - searchable model list (port of omp overlays/model-browser.ts ModelBrowser). */
#ifndef OMC_UI_MODELBROWSER_H
#define OMC_UI_MODELBROWSER_H

#include "ai/ai.h"
#include "tui/keys.h"

typedef struct {
    const ai_model_t *model;
    char *selector;  /* provider/id, or the virtual `@role` of a quick-role row */
    char *label;     /* shown in place of the model id (quick-role rows), NULL = model id */
    int label_color; /* tui_tc_t of the label (ModelBrowserItem.labelColor), -1 = default styling */
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
    /* hub mode (ModelBrowser.setRoles / setMruOrder): role chips from these assignments, a separator
     * after the recent/role-assigned rows, hover band, perf + intelligence columns */
    struct ui_role_assign *roles;
    size_t nroles;
    bool hub;
    strvec_t mru;
    int hovered;     /* visible index under the pointer, -1 none */
    int window_count;
    char *empty_text; /* overrides "No models available in this scope" when set */
    /* model picker (markOverContext): rows whose context window is below the session's tokens are grayed */
    bool mark_over_context;
    int64_t ctx_tokens;
    int64_t next_credit_change; /* earliest dated credit-rate change among the items (ms), 0 none */
} ui_model_browser_t;

/* a role resolved to a model (RoleAssignment) with its chip label/color (getRoleInfo) */
typedef struct ui_role_assign {
    char *role;
    const ai_model_t *model;
    const char *level; /* thinking level selector (static) */
    bool auto_selected;
    char *label; /* (tag ?? name ?? role).toLowerCase() */
    int color;   /* tui_tc_t */
    bool hidden;
} ui_role_assign_t;

#define UI_MB_SEPARATOR ((size_t)-1)
bool ui_model_is_free(const ai_model_t *m); /* isFreeModel: the `free` label and search word */
/* hub data: copies `roles` and `mru` */
void ui_model_browser_set_roles(ui_model_browser_t *b, const ui_role_assign_t *roles, size_t n, const strvec_t *mru);
/* omp sortModelItems: role rank (MODEL_ROLE_IDS order), MRU, provider, priority, version, date */
void ui_model_sort_items(const ai_model_t **models, size_t n, const ui_role_assign_t *roles, size_t nroles,
                         const strvec_t *mru);
/* mouse (routeMouse): wheel pans, motion hovers, click selects / activates; line is browser-local */
int ui_model_browser_mouse(ui_model_browser_t *b, int line, int wheel, bool motion, bool click);
void ui_model_browser_set_query(ui_model_browser_t *b, const char *q);
bool ui_model_browser_select(ui_model_browser_t *b, const char *selector);

enum { UI_MB_IGNORED, UI_MB_HANDLED, UI_MB_ACTIVATE, UI_MB_CANCEL };

ui_model_browser_t *ui_model_browser_new(void);
void ui_model_browser_free(ui_model_browser_t *b);
void ui_model_browser_add(ui_model_browser_t *b, const ai_model_t *m);
/* a virtual row (quick role): `selector` and `label` are copied; color -1 = default styling */
void ui_model_browser_add_labeled(ui_model_browser_t *b, const ai_model_t *m, const char *selector, const char *label,
                                  int color);
/* setItems: drop every item (query, roles and MRU stay); re-add, then ui_model_browser_done_adding */
void ui_model_browser_clear_items(ui_model_browser_t *b);
/* isOverContext */
bool ui_model_browser_over_context(const ui_model_browser_t *b, const ui_model_item_t *it);
void ui_model_browser_done_adding(ui_model_browser_t *b);
void ui_model_browser_load_available(ui_model_browser_t *b);
void ui_model_browser_set_current(ui_model_browser_t *b, const char *selector);
void ui_model_browser_set_max_visible(ui_model_browser_t *b, int rows);
const ui_model_item_t *ui_model_browser_selected(const ui_model_browser_t *b);
int ui_model_browser_key(ui_model_browser_t *b, const tui_key_t *k); /* UI_MB_* */
/* search row + blank + max_visible rows + blank + 2 detail rows */
void ui_model_browser_render(const ui_model_browser_t *b, int width, strvec_t *out);
char *ui_format_number(double n);
/* modelhub.c buildSessionModelScope: a browser over the available chat models (sortModelItems order) with the
 * role assignments (chips) and MRU order; call ui_model_browser_done_adding after adjusting it */
ui_model_browser_t *ui_session_model_browser(void); /* upstream formatNumber: 4096 -> "4.1K" */

#endif
