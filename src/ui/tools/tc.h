/* tc.h - tool-call cards for the transcript (port of omp packages/tui/src/tools renderers, the
 * render/ primitives they share - output-block, status-line, tree-list, output-pane - and
 * chat/tool-execution.ts, read-tool-group.ts, bash-execution.ts).
 *
 * Pure functions: a card is rendered from a tc_call_t snapshot (args, result content/details,
 * lifecycle flags) to ANSI lines at a fixed width with the current theme (tui_theme()) and glyph
 * preset (tui_sym()). Nothing here touches the UI controller, so cards can be unit-tested.
 * Every returned/appended string is owned by the caller. */
#ifndef OMC_UI_TC_H
#define OMC_UI_TC_H

#include "core/core.h"
#include "core/json.h"
#include "tui/theme.h"

typedef struct {
    const char *name;     /* tool name (renderer key) */
    const char *label;    /* display label for the generic card (defaults to name) */
    const json_t *args;   /* call arguments (may be NULL while streaming) */
    const json_t *content; /* result content blocks (NULL: no result yet) */
    const json_t *details; /* result details (may be NULL) */
    bool has_result;      /* a (partial or final) result arrived */
    bool partial;         /* still running: no final result yet */
    bool is_error;
    bool expanded;        /* ctrl+o */
    int spinner;          /* spinner frame index, -1 when not animating */
    int64_t elapsed_ms;   /* since execution start (live) or total; -1 unknown */
    bool exec_started;
    const char *cwd;      /* project dir (path shortening, `cd` prefixes) */
    int term_rows;        /* terminal height (preview windows); 0 = unknown */
    int64_t now_ms;       /* wall clock for live elapsed facts (0 = now) */
    /* registered tool label lookup (xd:// device writes delegate to the mounted tool's card); may be NULL */
    const char *(*label_of)(const char *tool);
} tc_call_t;

/* Render one tool call (any tool; falls back to the generic card). Appends lines to `out`. */
void tc_render(const tc_call_t *c, int width, strvec_t *out);
/* Grouped `read` calls (omp ReadToolGroupComponent): ` ● Read path` or ` • Read (N)` + tree. */
void tc_render_read_group(const tc_call_t *calls, size_t n, int width, strvec_t *out);
/* User `!cmd` / `!!cmd` shell run (omp BashExecutionComponent). exit_code < 0: unknown. */
typedef struct {
    const char *command;
    const char *output;
    bool running, cancelled, exclude; /* exclude: `!!` (not sent to the model) */
    int exit_code;
    bool expanded;
    int spinner;
    int term_rows;
} tc_bang_t;
void tc_render_bang(const tc_bang_t *b, int width, strvec_t *out);
/* Whether a read call collapses into a read group (filesystem targets, not internal URLs). */
bool tc_read_groupable(const json_t *args);
/* Whether a tool's live card needs periodic repaints (spinner / elapsed clock). */
bool tc_wants_tick(const char *tool_name);

/* ---------- shared primitives (tc_core.c) ---------- */

/* theme */
char *tc_fg(tui_tc_t slot, const char *s);                    /* fg(slot) + s + \e[39m */
void tc_fg_sb(sb_t *b, tui_tc_t slot, const char *s);
char *tc_bold(const char *s);
const char *tc_sym(const char *key);                          /* tui_sym() */
char *tc_styled_sym(const char *key, tui_tc_t slot);          /* fg(slot, sym) */
const char *tc_spinner_frame(int frame);

typedef enum {
    TC_ST_NONE, TC_ST_SUCCESS, TC_ST_DONE, TC_ST_ERROR, TC_ST_WARNING, TC_ST_INFO, TC_ST_PENDING,
    TC_ST_RUNNING, TC_ST_ABORTED
} tc_status_t;
char *tc_status_icon(tc_status_t st, int spinner);

/* renderStatusLine: `icon title: description [badge] meta·meta` */
typedef struct {
    tc_status_t icon;
    const char *icon_override; /* pre-rendered glyph (wins over icon) */
    int spinner;
    const char *title;
    int title_slot;            /* -1 = accent */
    const char *description;   /* rendered muted */
    const char *const *meta;   /* rendered dim, joined by sep.dot */
    size_t nmeta;
    const char *badge;         /* `[badge]` after the description (NULL: none) */
    tui_tc_t badge_slot;
} tc_status_line_t;
char *tc_status_line(const tc_status_line_t *o);

/* renderOutputBlock */
typedef enum { TC_STATE_NONE, TC_STATE_PENDING, TC_STATE_RUNNING, TC_STATE_SUCCESS, TC_STATE_ERROR, TC_STATE_WARNING } tc_state_t;
typedef struct {
    const char *label;     /* section bar label (NULL: none) */
    bool separator;
    strvec_t lines;
} tc_section_t;
typedef struct {
    const char *header;
    const char *header_meta;
    const char *footer_meta;
    tc_state_t state;
    tc_section_t *sections;
    size_t nsections;
    int width;
    bool no_bg;            /* applyBg = false */
    int pad_left, pad_right; /* -1 = default (1) */
    int border_slot;       /* -1 = derived from state */
} tc_block_t;
void tc_output_block(const tc_block_t *o, strvec_t *out);
int tc_block_content_width(int width, int pad_left, int pad_right);

/* text helpers */
void tc_split_lines(const char *s, strvec_t *out);           /* split on \n (keeps empty tail) */
char *tc_replace_tabs(const char *s);
char *tc_trim_end(const char *s);
char *tc_sanitize_line(const char *s);                        /* tabs->spaces, CR overwrite, ctrl chars */
char *tc_shorten_path(const char *p);                         /* $HOME -> ~ */
char *tc_rel_path(const char *p, const char *cwd);            /* cwd-relative when inside, else shortened */
const char *tc_lang_from_path(const char *p);                 /* highlight language id or NULL */
const char *tc_lang_icon(const char *lang);
void tc_highlight(const char *code, const char *lang, strvec_t *out); /* plain lines when lang unknown */
char *tc_pluralize(const char *label, int64_t n);
char *tc_format_count(const char *label, int64_t n);
char *tc_format_duration(int64_t ms);
char *tc_expand_hint(void);                                   /* dim "⟦Ctrl+O: Expand⟧" */
char *tc_more_items(int64_t n, const char *item);            /* "… N more items" */
int tc_preview_window_rows(int term_rows);
/* capPreviewLines: tail window with a dim "… N earlier lines ⟦Ctrl+O: Expand⟧" marker */
void tc_cap_preview(strvec_t *lines, bool expanded, int max);
/* formatOutputPaneLines (subset): styled + capped rows; visual tail capping at `width`. */
typedef struct {
    const strvec_t *lines;
    bool expanded;
    int collapsed_max;     /* < 0: no cap */
    bool tail;             /* keep the end */
    bool visual;           /* cap wrapped rows (needs width) */
    int width;
    tui_tc_t style;        /* applied to each line */
    bool style_plain;      /* don't style */
    bool hint_when_uncapped;
    int expanded_max;      /* cap while expanded (0 = none) */
} tc_pane_t;
void tc_output_pane(const tc_pane_t *p, strvec_t *out);
/* renderTreeList over pre-rendered single/multi-line items */
void tc_tree_list(const strvec_t *items, bool expanded, int max_collapsed, const char *item_type, strvec_t *out);
/* ... with a collapsed row budget (maxCollapsedLines; < 0 = none) */
void tc_tree_list_ex(const strvec_t *items, bool expanded, int max_collapsed, int max_lines, const char *item_type, strvec_t *out);
/* wrap header + truncate rows (fitHeaderedRows) */
void tc_fit_headered(const char *header, strvec_t *rows, int width, strvec_t *out);
char *tc_error_message(const char *msg);                     /* "✘ Error: msg" */
char *tc_error_detail(const char *msg);                      /* "  msg" in error color */
char *tc_empty_message(const char *msg);                     /* "⚠ msg" */
char *tc_text_of(const json_t *content);                     /* first text block ("" none) */
char *tc_json_inline(const json_t *args, int budget);         /* formatArgsInline */
void tc_json_tree(const json_t *v, int max_depth, int max_lines, int max_scalar, bool hide_meta, strvec_t *out, bool *truncated);
void tc_push_wrapped(strvec_t *out, const char *s, int width);

/* per-tool renderers (internal) */
void tc_bash(const tc_call_t *c, int width, strvec_t *out);
void tc_write(const tc_call_t *c, int width, strvec_t *out);
void tc_edit(const tc_call_t *c, int width, strvec_t *out);
void tc_read(const tc_call_t *c, int width, strvec_t *out);
void tc_grep(const tc_call_t *c, int width, strvec_t *out);
void tc_glob(const tc_call_t *c, int width, strvec_t *out);
void tc_todo(const tc_call_t *c, int width, strvec_t *out);
void tc_generic(const tc_call_t *c, int width, strvec_t *out);
void tc_tinted_box(const strvec_t *body, int width, tui_tc_t bg_slot, strvec_t *out);
void tc_mcp(const tc_call_t *c, int width, strvec_t *out);
void tc_render_diff(const char *diff, const char *path, strvec_t *out);
void tc_web_search(const tc_call_t *c, int width, strvec_t *out);
void tc_ast_grep(const tc_call_t *c, int width, strvec_t *out);
void tc_ast_edit(const tc_call_t *c, int width, strvec_t *out);
void tc_debug(const tc_call_t *c, int width, strvec_t *out);
void tc_task(const tc_call_t *c, int width, strvec_t *out);
void tc_eval(const tc_call_t *c, int width, strvec_t *out);
void tc_wait(const tc_call_t *c, int width, strvec_t *out);
void tc_github(const tc_call_t *c, int width, strvec_t *out);
void tc_lsp(const tc_call_t *c, int width, strvec_t *out);

/* grouped-file-output helpers (tc_search.c) */
char *tc_paths_of(const json_t *args);                       /* path/paths arg joined ", " (NULL none) */
bool tc_is_search_header(const char *l);                     /* `# dir/`, `## file#TAG` */
const char *tc_search_header_lang(const char *line);
char *tc_style_search_line(const char *line, const char *lang); /* header tint / code-frame highlight */
void tc_ask(const tc_call_t *c, int width, strvec_t *out);
void tc_goal(const tc_call_t *c, int width, strvec_t *out);
void tc_find(const tc_call_t *c, int width, strvec_t *out);
char *tc_progress_bar(double frac, int width, tui_tc_t filled_slot, tui_tc_t empty_slot);
char *tc_format_number(double n);                           /* formatNumber: 1.2K, 34K, 5.6M */
char *tc_italic(const char *s);
/* wrapPrefixedLine: `prefix` + wrapped content, continuation rows behind `cont` */
void tc_wrap_prefixed(const char *prefix, const char *cont, const char *content, int width, strvec_t *out);

/* web helpers (tc_web.c) */
char *tc_url_domain(const char *url);                        /* hostname without www. */
char *tc_hyperlink(const char *url, const char *text);       /* OSC 8 when the terminal supports it */
char *tc_format_age(int64_t secs);                           /* "2h ago" ("" for 0) */

#endif
