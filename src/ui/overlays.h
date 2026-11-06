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
struct ai_model;

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
/* overlay-box `color`: recolor border and title of the ui_box_* calls that follow (-1 resets); returns the previous */
int ui_box_tone(int tc);

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

/* single-line Input (components/input.ts): value + byte cursor; the cursor cell renders inverted */
typedef struct {
    sb_t v;
    size_t cur;
} ui_input_t;
const char *ui_input_value(const ui_input_t *in);
void ui_input_set(ui_input_t *in, const char *s); /* cursor to the end */
void ui_input_free(ui_input_t *in);
bool ui_input_key(ui_input_t *in, const tui_key_t *k); /* editing keys; false when not handled */
/* prompt + windowed value + inverted cursor cell, padded to width; the cursor marker when focused. Owned */
char *ui_input_render(const ui_input_t *in, const char *prompt, int width, bool focused);
/* … with the hardware cursor (useTerminalCursor): the cursor cell is drawn plain */
char *ui_input_render_hw(const ui_input_t *in, const char *prompt, int width, bool focused, bool hw_cursor);

/* ---------- tree.c: /tree session tree selector (overlays/tree-selector.ts) ---------- */

typedef struct {
    /* Enter (summarize false) / Shift+Enter (summarize true) on an entry */
    void (*on_select)(void *ud, const char *entry_id, bool summarize);
    void (*on_cancel)(void *ud);
    /* Shift+L label edit saved; label NULL removes it */
    void (*on_label)(void *ud, const char *entry_id, const char *label);
    void *ud;
} ui_tree_cb_t;
/* `entries`: the session's entries (borrowed for the component's lifetime); `filter_mode` one of
 * default / no-tools / user-only / labeled-only / all (treeFilterMode). Callbacks run on the UI thread. */
tui_component_t *ui_tree_selector_new(const json_t *entries, const char *leaf_id, int term_rows,
                                      const char *filter_mode, const ui_tree_cb_t *cb);

/* ---------- rewind.c: Esc-Esc / /branch rewind selector (overlays/rewind-selector.ts) ---------- */

typedef struct {
    void (*on_select)(void *ud, const char *entry_id); /* rewind to that entry */
    void (*on_cancel)(void *ud);
    void *ud;
} ui_rewind_cb_t;
/* fullscreen transcript replica of the current branch with a dotted outline on the target; NULL when the
 * branch has no rewind target */
tui_component_t *ui_rewind_selector_new(ui_t *u, const ui_rewind_cb_t *cb);

/* The shared engine of the fullscreen transcript selectors (rewind, /copy): chat/transcript-outline.ts and
 * chat/transcript-browser.ts. Entries are session transcript entries (`message` / `custom_message`). */
typedef struct {
    char *entry_id, *turn_id; /* resolves to entry_id (extended over folded tool results); opened by turn_id */
    bool user;                /* isUserTurnEntry */
    size_t start, end;        /* transcript-container children rendered by the turn */
    VEC(size_t) ents;         /* indices (into the replayed entries) of the turn and its folded tool results */
} ui_tx_target_t;
typedef VEC(ui_tx_target_t) ui_tx_targets_t;
typedef VEC(strvec_t) ui_tx_rows_t; /* rendered rows per transcript child */
typedef struct {
    strvec_t lines;
    int sel_start, sel_end;
} ui_tx_composed_t;
/* scroll view of a TranscriptBrowser: offset plus the last revealed anchor */
typedef struct {
    int offset;
    char *anchor_id;
    int anchor_start, anchor_end, anchor_w, anchor_h, anchor_total;
} ui_tx_view_t;

bool ui_tx_is_transcript_entry(const json_t *e);
bool ui_tx_is_user_request(const json_t *e);
/* the current branch's transcript entries (owned), NULL when the session has none */
json_t *ui_tx_branch_entries(ui_t *u);
/* recentTranscriptEntries: the tail from a user-request boundary within the last 600 entries (owned) */
json_t *ui_tx_recent_entries(const json_t *entries);
/* appendOutlineEntries: replay `entries` into `c`, appending the selectable targets */
void ui_tx_replay(ui_t *u, tui_container_t *c, const json_t *entries, ui_tx_targets_t *out);
void ui_tx_targets_free(ui_tx_targets_t *t);
void ui_tx_rows_free(ui_tx_rows_t *r);
/* prompt-zone-free rows of c's children [from, to) at width w (tool output expanded when `expanded`) */
void ui_tx_render_children(ui_t *u, bool expanded, tui_container_t *c, size_t from, size_t to, int w, ui_tx_rows_t *out);
bool ui_tx_target_visible(const ui_tx_rows_t *rows, size_t base, const ui_tx_target_t *t);
/* outlineRows: rows wrapped in the dotted outline in `tc`, `caption` (may be NULL) bold in the top rule */
void ui_tx_outline_rows(const strvec_t *rows, size_t from, size_t to, int inner, tui_tc_t tc, const char *caption,
                        strvec_t *out);
/* composeOutlineColumn over rows[from, to) (rows indexed from `base`), outlining targets[sel] (sel < 0: none) */
void ui_tx_compose(const ui_tx_rows_t *rows, size_t base, size_t from, size_t to, const ui_tx_target_t *targets,
                   ssize_t sel, int col_w, const strvec_t *header, tui_tc_t tc, const char *caption,
                   ui_tx_composed_t *out);
void ui_tx_view_free(ui_tx_view_t *v);
/* TranscriptBrowser frame: rule, ` header`, rule, body viewport (anchor revealed), ` footer`, rule */
void ui_tx_frame(ui_tx_view_t *v, int width, int term_rows, const char *header, const char *footer, const strvec_t *body,
                 const char *anchor, int sel_start, int sel_end, strvec_t *out);
int ui_tx_body_height(int term_rows);
/* ScrollView keys (shift+arrows, page up/down, home/end); false when not a scroll key */
bool ui_tx_scroll_key(ui_tx_view_t *v, int term_rows, const tui_key_t *k);
/* SGR mouse: wheel scrolls the view; returns true and the 0-based frame cell of a left press in *row / *col */
bool ui_tx_mouse(ui_tx_view_t *v, const tui_key_t *k, int *row, int *col);

/* ---------- copysel.c: /copy picker (overlays/copy-selector.ts, copy-targets.ts) ---------- */

typedef struct {
    /* the picked text and its label ("assistant message", "bash command", "link · docs", ...) */
    void (*on_pick)(void *ud, const char *content, const char *label);
    void (*on_open)(void *ud, const char *href, const char *label); /* `o` / ↗ open on a link block */
    void (*on_cancel)(void *ud);
    void *ud;
} ui_copy_cb_t;
/* fullscreen picker over the current branch; NULL when nothing can be copied */
tui_component_t *ui_copy_selector_new(ui_t *u, const ui_copy_cb_t *cb);

/* copy-targets.ts over agent messages (assistant markdown and bash/eval tool calls). Owned results; NULL
 * when the transcript has none. */
typedef struct {
    char *kind; /* "code" | "quote" */
    char *lang; /* code: info string */
    char *text;
} ui_md_block_t;
typedef VEC(ui_md_block_t) ui_md_blocks_t;
void ui_md_extract_blocks(const char *text, ui_md_blocks_t *out); /* extractBlocks */
void ui_md_blocks_free(ui_md_blocks_t *b);
/* extractLinks: http(s) links the renderer draws, deduplicated by href (labels[i] names hrefs[i]) */
void ui_md_extract_links(const char *text, strvec_t *hrefs, strvec_t *labels);
char *ui_copy_last_code(const json_t *messages);
/* *kind: "bash" or "eval" */
char *ui_copy_last_command(const json_t *messages, const char **kind);
char *ui_copy_last_link(const json_t *messages);

/* ---------- setup.c ---------- */

#define UI_SETUP_VERSION 2
/* scenes newer than the stored setupVersion that should run now (0 when none / not a TTY / skipped) */
int ui_setup_pending(bool resuming);
/* show the wizard (fullscreen overlay); `done` runs on the UI thread once it finished (after the outro) */
void ui_setup_run(ui_t *u, void (*done)(void *ud), void *ud);
bool ui_setup_active(void);
/* /setup [providers] (runProviderSetupWizard): the providers scene only, without advancing setupVersion or
 * replaying the welcome intro */
void ui_setup_run_providers(ui_t *u);

/* ---------- overlays.c ---------- */

/* apply the configured theme / glyph preset / color-blind mode (startup and after changes) */
void ui_apply_appearance(void);
void ui_apply_motion(void); /* tui.motion, tui.spinnerInterval, tui.maxFps and display.shimmer -> the shared knobs */
/* swap the composer area for a selector (upstream SelectorController.showSelector) */
void ui_show_selector(ui_t *u, tui_component_t *comp, tui_component_t *focus);
void ui_close_selector(ui_t *u);
/* Open a selector over the one holding the editor slot; closing it restores that selector and its focus. */
void ui_show_selector_over(ui_t *u, tui_component_t *comp, tui_component_t *focus);
void ui_close_nested_selector(ui_t *u);
tui_component_t *ui_selector_top(const ui_t *u);
bool ui_selector_open(const ui_t *u);

void ui_overlay_model(ui_t *u); /* modelhub.c: the Models hub */
void ui_agents_hub_open(ui_t *u); /* agentshub.c: the /agents hub (agent definitions) */
/* modelpicker.c: the session-only model picker (Alt+P, `/switch`) */
void ui_overlay_model_picker(ui_t *u);
/* switchSessionModel: session-only switch (compacting first when the transcript exceeds the model's window);
 * `level` an explicit thinking selector or NULL (the role's explicit level for that model, else kept) */
void ui_switch_session_model(ui_t *u, const struct ai_model *m, const char *level);
void ui_overlay_model_provider(ui_t *u, const char *provider); /* … with that provider's entry preselected */
void ui_overlay_session(ui_t *u);
void ui_resume_session(ui_t *u, const char *path); /* interactive.c: switch session + reload transcript */
/* interactive.c: the session picker closed; when it was opened by `omc --resume` (no value) and nothing was
 * chosen, the TUI quits like omp's standalone startup picker. True when it handled the close. */
bool ix_startup_picker_closed(ui_t *u, bool resumed);
void ui_overlay_thinking(ui_t *u);
void ui_overlay_theme(ui_t *u);
/* settings.c: the /settings menu (fullscreen, alternate screen) */
void ui_overlay_settings(ui_t *u);
/* the status bar as the active composer shape renders it ("\n"-joined rows), for previews. Owned. */
char *ui_settings_status_preview(ui_t *u, int cols);
size_t ui_settings_def_count(void);
void ui_overlay_hotkeys(ui_t *u);
/* /hotkeys markdown built from the live keybindings (buildHotkeysMarkdown); owned */
char *ui_hotkeys_markdown(void);
/* ---------- report.c ---------- */
/* titled read-only command report (overlays/report-panel.ts, CommandController.showCommandReport); takes `body`.
 * Docks above the editor when it fits there (Esc dismisses), else a focused full-screen page that scrolls. */
void ui_show_report(ui_t *u, const char *title, tui_component_t *body);
void ui_show_markdown_report(ui_t *u, const char *title, const char *markdown);
tui_component_t *ui_report_slot_new(ui_t *u); /* the docking slot (omp reportContainer) */
int ui_report_rows(ui_t *u);                  /* commandReportRows: screen rows above the chrome below the slot; -1 unknown */
bool ui_report_dismiss(ui_t *u);              /* Esc: drop the docked report; false when none */
void ui_report_clear(ui_t *u);                /* transcript/session reset: drop the docked report and any sheet */
/* focused full-screen page on the alternate screen (#showFullscreenSheet); takes `c`, closing any previous */
void ui_show_sheet(ui_t *u, tui_component_t *c);
void ui_close_sheet(ui_t *u);
/* ---------- panels.c / context.c / todocmd.c (WP-7 command panels) ---------- */
void ui_context_command(ui_t *u);                 /* /context viewer */
void ui_session_command(ui_t *u, const char *arg); /* /session [info|delete|pin [account]] */
void ui_tools_command(ui_t *u);                   /* /tools */
void ui_jobs_command(ui_t *u, const char *arg);   /* /jobs [full|kill <id>|kill all] */
void ui_todo_command(ui_t *u, const char *arg);   /* /todo [verb ...] */
void ui_overlay_login(ui_t *u, bool logout);
void ui_overlay_login_provider(ui_t *u, const char *provider, bool reopen_hub);
/* ---------- ask.c ---------- */

/* ask tool dialog in the composer's place (overlays/ask-dialog.ts). `questions` is the tool's questions
 * array (copied). `done` runs once on the UI thread with the answers object (id -> label | [labels]; a
 * custom answer is its text; unanswered ids left out; owned by the callee) or NULL when cancelled. */
typedef void (*ui_ask_done_fn)(void *ud, json_t *answers);
void ui_ask_dialog_show(ui_t *u, const json_t *questions, ui_ask_done_fn done, void *ud);

/* ---------- approval.c ---------- */

/* tool approval prompt in the composer's place (overlays/approval-dialog.ts): installs the tool env's `approve`
 * bridge (tool threads block until the dialog answers); shutdown denies a waiting call once the UI loop ends */
void ui_approval_install(ui_t *u);
void ui_approval_shutdown(ui_t *u);

/* ---------- planreview.c ---------- */

/* fullscreen plan review (overlays/plan-review-overlay.ts): the plan rendered by section, a Contents
 * sidebar, section deletes/undo and annotations, the approval options and an optional model-tier slider */
typedef struct ui_plan_review ui_plan_review_t;
typedef struct {
    const char *prompt_title;          /* "Plan mode - next step" */
    const char *const *options;
    size_t noptions;
    const int *disabled;               /* indices into options that render dimmed and cannot be picked */
    size_t ndisabled;
    const char *help_text;             /* trailing footer hint (the cancel hint) */
    int initial_index;
    const char *slider_caption;        /* model-tier slider ("continue with"); nsegments 0 = none */
    const char *const *segment_labels;
    const char *const *segment_details;
    size_t nsegments;
    int slider_index;
    const char *external_editor_label; /* "Ctrl+G" */
    const json_t *annotation_state;    /* PlanReviewAnnotationState to restore (borrowed) */
} ui_plan_review_opts_t;
typedef struct {
    void (*on_pick)(void *ud, const char *label); /* never a disabled option; input locks afterwards */
    void (*on_cancel)(void *ud);
    void (*on_copy)(void *ud, const char *content);          /* NULL: no copy key */
    void (*on_external_editor)(void *ud);                     /* NULL: no editor key */
    void (*on_annotation_editor)(void *ud, const char *draft); /* answer with ui_plan_review_set_annotation_draft */
    void (*on_plan_edited)(void *ud, const char *content);    /* after an in-overlay delete / undo */
    void (*on_feedback)(void *ud, const char *feedback);      /* Refine feedback markdown ("" = none) */
    void (*on_annotation_state)(void *ud, const json_t *state);
    void (*on_slider)(void *ud, int index);
    void *ud;
} ui_plan_review_cb_t;
ui_plan_review_t *ui_plan_review_new(tui_t *tui, const char *plan, const ui_plan_review_opts_t *o,
                                     const ui_plan_review_cb_t *cb);
tui_component_t *ui_plan_review_component(ui_plan_review_t *p);
void ui_plan_review_set_plan(ui_plan_review_t *p, const char *plan); /* external-editor round trip */
void ui_plan_review_set_annotation_draft(ui_plan_review_t *p, const char *text);
void ui_plan_review_set_slider(ui_plan_review_t *p, int index);
bool ui_plan_review_select(ui_plan_review_t *p, const char *label); /* commit like Enter; false if refused */

/* showHookConfirm: `title` box with `message` and Yes / No in the composer's place; cb(yes) runs on the
 * UI thread after it closed (cancel = No) */
void ui_hook_confirm(ui_t *u, const char *title, const char *message, void (*cb)(void *ud, bool yes), void *ud);
/* showHookSelector: `title` box with the options; cb(index) on the UI thread after it closed (-1 = cancelled) */
void ui_hook_select(ui_t *u, const char *title, const char *const *options, size_t n, void (*cb)(void *ud, int index),
                    void *ud);
/* showHookEditor (hook style): multi-line editor, Ctrl+Q/Ctrl+Enter submits, Esc cancels; cb(text or NULL) */
void ui_hook_editor(ui_t *u, const char *title, void (*cb)(void *ud, const char *text), void *ud);
tui_editor_t *ui_hook_editor_of(tui_component_t *c); /* its editor when c is a hook editor, else NULL */
/* the ask dialog as a selector; its custom-answer / note prompt editor while one is open, else NULL */
bool ui_ask_is_dialog(const tui_component_t *c);
tui_editor_t *ui_ask_prompt_editor(tui_component_t *c);
/* "Save and quit" path prompt (overlays/plan-save-overlay.ts); done(NULL) on cancel */
typedef struct ui_plan_save ui_plan_save_t;
ui_plan_save_t *ui_plan_save_new(const char *suggested, void (*done)(void *ud, const char *path), void *ud);
tui_component_t *ui_plan_save_component(ui_plan_save_t *s);
void ui_plan_save_set_suggested(ui_plan_save_t *s, const char *path);

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
void ui_slash_register(ui_t *u, tui_ac_default_t *ac); /* commands, aliases, hints from coding/slashcmd.c */

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
