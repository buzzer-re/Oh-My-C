/* overlays.h - setup wizard, selectors and dialogs of the interactive UI (ports of omp
 * packages/tui/src/setup and packages/tui/src/overlays), plus the small widgets they share.
 *
 *   widgets.c    box chrome, key hints, gradient logo, titled panel, wizard-step layout
 *   setup.c      first-run setup wizard (splash, 5 scenes, outro) + persistence
 *   overlays.c   model / session / thinking / theme / settings / hotkeys / ask / login overlays
 */
#ifndef OMC_UI_OVERLAYS_H
#define OMC_UI_OVERLAYS_H

#include "tui/components.h"
#include "tui/editor.h"
#include "tui/render.h"
#include "tui/symbols.h"

typedef struct ui ui_t;

/* ---------- widgets.c ---------- */

/* first key bound to an action ("tui.select.confirm"), formatted; "" when unbound. Owned. */
char *ui_editor_key(const char *action);
/* centerLine: pad left by floor((width - w) / 2) and right to width (truncate when too wide). Owned. */
char *ui_center_line(const char *line, int width);
/* pad (no truncation) to width; truncates when wider. Owned. */
char *ui_fit_line(const char *line, int width);

/* rounded-box chrome in the border colour (upstream chrome/overlay-box.ts). Owned. */
char *ui_box_top(int width, const char *title);
char *ui_box_row(const char *content, int width);
char *ui_box_divider(int width);
char *ui_box_bottom(int width);

/* brand mark (PI_LOGO) with the omp diagonal gradient + optional shine. Lines appended (owned). */
extern const char *const UI_PI_LOGO[5];
char *ui_gradient_escape(double t, double shine_pos, double shine_strength);
void ui_gradient_logo(const char *const *lines, size_t n, double phase, double shine_pos, double shine_strength,
                      strvec_t *out);

/* text block helpers: wrap `text` (ANSI ok) to width and append; blank text appends nothing (upstream Text) */
void ui_push_text(strvec_t *out, const char *text, int width);

/* OverlayPanel: a rounded box titled `title` around the lines of its children. */
typedef struct {
    tui_container_t c;
    char *title;
} ui_panel_t;
ui_panel_t *ui_panel_new(const char *title);
void ui_panel_set_title(ui_panel_t *p, const char *title);

/* WizardStep layout (components/wizard-step.ts): slots separated by `gap` blank rows; an optional
 * preview collapses when it would squeeze content below min_content rows; content is clipped to
 * the remaining budget. Any slot may be NULL/empty. fit(budget) is called before rendering content. */
typedef struct {
    const strvec_t *heading, *intro, *preview, *status, *footer;
    bool preview_optional;
    int gap;         /* default 1 when < 0 */
    int min_content; /* default 1 */
    int max_height;  /* < 0: unbounded */
    void (*fit)(void *ud, int budget); /* budget < 0 when unbounded */
    void (*content)(void *ud, int width, strvec_t *out);
    void *ud;
} ui_wizard_step_t;
void ui_wizard_step_render(const ui_wizard_step_t *s, int width, strvec_t *out);

/* ScrollView (scrollbar "auto") over pre-rendered lines: exactly `height` rows, each truncated (…) to
 * width, one column narrower plus a muted │ track / accent █ thumb when the lines overflow. *offset is
 * clamped. */
void ui_scroll_view(const strvec_t *lines, int height, int *offset, int width, strvec_t *out);

/* TabBar with omp's tab-bar theme: optional bold accent "label:", ` tab ` chips (active bold on
 * selectedBg, others muted, muted tabs dim) two spaces apart, optional "(⇥ to cycle)" hint; tabs with a
 * short label collapse to it (farthest from the active tab first) when the row is too wide; wraps. */
typedef struct {
    const char *label;
    const char *short_label; /* NULL = never collapses */
    bool muted;
} ui_tab_t;
void ui_tab_bar_render(const char *label, const ui_tab_t *tabs, size_t n, size_t active, bool show_hint, int width,
                       strvec_t *out);

/* ---------- setup.c ---------- */

#define UI_SETUP_VERSION 2
/* scenes newer than the stored setupVersion that should run now (0 when none / not a TTY / skipped) */
int ui_setup_pending(bool resuming);
/* show the wizard (fullscreen overlay); `done` runs on the UI thread once it finished (after the outro) */
void ui_setup_run(ui_t *u, void (*done)(void *ud), void *ud);
bool ui_setup_active(void);

/* ---------- overlays.c ---------- */

/* apply the configured theme / glyph preset / color-blind mode (startup and after changes) */
void ui_apply_appearance(void);
/* swap the composer area for a selector (upstream SelectorController.showSelector) */
void ui_show_selector(ui_t *u, tui_component_t *comp, tui_component_t *focus);
void ui_close_selector(ui_t *u);
bool ui_selector_open(const ui_t *u);

void ui_overlay_model(ui_t *u);
void ui_overlay_session(ui_t *u);
void ui_resume_session(ui_t *u, const char *path); /* interactive.c: switch session + reload transcript */
void ui_overlay_thinking(ui_t *u);
void ui_overlay_theme(ui_t *u);
void ui_overlay_settings(ui_t *u);
void ui_overlay_hotkeys(ui_t *u);
/* titled command report (overlays/report-panel.ts): fullscreen page, scrolls, Esc closes; takes `body` */
void ui_show_report(ui_t *u, const char *title, tui_component_t *body);
void ui_overlay_login(ui_t *u, bool logout);
/* ---------- ask.c ---------- */

/* ask tool dialog in the composer's place (overlays/ask-dialog.ts). `questions` is the tool's questions
 * array (copied). `done` runs once on the UI thread with the answers object (id -> label | [labels]; a
 * custom answer is its text; unanswered ids left out; owned by the callee) or NULL when cancelled. */
typedef void (*ui_ask_done_fn)(void *ud, json_t *answers);
void ui_ask_dialog_show(ui_t *u, const json_t *questions, ui_ask_done_fn done, void *ud);

/* ---------- login.c ---------- */

enum { UI_SEL_IGNORED, UI_SEL_HANDLED, UI_SEL_PICK, UI_SEL_CANCEL };
enum { UI_LOGIN_SUCCESS, UI_LOGIN_ERROR, UI_LOGIN_CANCELLED };

/* provider list in a "Select provider to login/logout" panel (overlays/oauth-selector.ts) */
typedef struct ui_oauth_sel ui_oauth_sel_t;
ui_oauth_sel_t *ui_oauth_sel_new(bool logout);
void ui_oauth_sel_free(ui_oauth_sel_t *s);
void ui_oauth_sel_refresh(ui_oauth_sel_t *s); /* after a credential change */
void ui_oauth_sel_set_max_height(ui_oauth_sel_t *s, int lines);
void ui_oauth_sel_render(ui_oauth_sel_t *s, int width, strvec_t *out);
int ui_oauth_sel_key(ui_oauth_sel_t *s, const tui_key_t *k, const char **picked); /* UI_SEL_*; picked borrowed */

/* "Login to <provider>" dialog running the flow on a worker (overlays/login-dialog.ts). `done` runs on
 * the UI thread once: success / error (err) / cancelled (Esc). */
typedef struct ui_login ui_login_t;
typedef void (*ui_login_done_fn)(void *ud, int outcome, const char *provider_name, const char *err);
ui_login_t *ui_login_start(ui_t *u, const char *provider, ui_login_done_fn done, void *ud);
void ui_login_free(ui_login_t *l); /* aborts a running flow */
bool ui_login_active(const ui_login_t *l);
void ui_login_render(ui_login_t *l, int width, strvec_t *out);
bool ui_login_key(ui_login_t *l, const tui_key_t *k);

/* ---------- slash.c ---------- */

/* register slash commands for autocomplete in upstream registry order with icons and (live) descriptions */
void ui_slash_register(ui_t *u, tui_ac_default_t *ac, const char *const *names, const char *const *descs, size_t n);

/* ---------- appearance.c ---------- */

/* the app name shown in chrome (upstream APP_NAME, "omp" there) */
#define UI_APP_NAME "omc"

const char *ui_theme_current_name(void);
/* load + activate a theme (built-in or <agent>/themes/<name>.json); false + *err (owned) on failure */
bool ui_theme_preview(const char *name, char **err);
/* activate theme.dark / theme.light for the detected terminal appearance (titanium / light) */
void ui_theme_auto(void);
void ui_set_color_blind(bool on);
bool ui_theme_is_light(const char *name);
/* built-in + custom theme names, sorted */
void ui_theme_list(strvec_t *out);

/* status rows used by the composer-shape preview, rendered by statusline.c's live status line.
 * All owned; `title` is upstream's stand-in session title (unused: the live title applies). */
char *ui_status_top_border(ui_t *u, int width, const char *title, int *out_width);
char *ui_status_band_top(ui_t *u, int width, const char *title, int *out_width);
char *ui_status_rule_chip(ui_t *u, int width, const char *title, int *out_width);
char *ui_status_bottom_bar(ui_t *u, int width, bool full, const char *title);

/* composer shapes (overlays/composer-shape-registry.ts) and their preview */
typedef struct {
    const char *value, *label, *description;
} ui_composer_shape_t;
extern const ui_composer_shape_t UI_COMPOSER_SHAPES[8];
void ui_composer_preview(ui_t *u, const char *shape, int width, strvec_t *out);

#endif
