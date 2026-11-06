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
    bool show_images;     /* result images draw as terminal graphics below the card (no text indicator) */
    bool args_complete;   /* arguments are final (omp argsComplete); false while a write/edit body streams */
    bool frozen;          /* renderContext.frozen: a live Task row is a dimmed snapshot, not in flight */
} tc_call_t;

/* Render one tool call (any tool; falls back to the generic card). Appends lines to `out`. */
void tc_render(const tc_call_t *c, int width, strvec_t *out);
/* Grouped `read` calls (omp ReadToolGroupComponent): ` ✔ Read path` or ` • Read (N)` + tree. */
void tc_render_read_group(const tc_call_t *calls, size_t n, int width, strvec_t *out);
/* User `!cmd` / `!!cmd` shell run (omp BashExecutionComponent) or `$ code` / `$$ code` Python run
 * (EvalExecutionComponent). exit_code < 0: unknown. */
typedef struct {
    const char *command;
    const char *output;
    bool running, cancelled, exclude; /* exclude: `!!` (not sent to the model) */
    bool python;                      /* `$ code` (omp EvalExecutionComponent): `>>>` + highlighted code */
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
    TC_ST_NONE, TC_ST_SUCCESS, TC_ST_ERROR, TC_ST_WARNING, TC_ST_INFO, TC_ST_PENDING,
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
/* mcp.renderMarkdownResults (setMcpRenderMarkdownResults): non-JSON MCP text results render as Markdown */
extern bool tc_mcp_markdown;
char *tc_shorten_path(const char *p);                         /* $HOME -> ~ */
/* color of a file's name, or a whole URL, in tool bodies (omp FILE_NAME_COLOR; bronze in titanium) */
#define TC_FILE_NAME_COLOR TUI_TC_CUSTOM_MESSAGE_LABEL
char *tc_styled_path(const char *path);                       /* formatStyledPath: dim directory + FILE_NAME_COLOR name */
bool tc_is_url_like(const char *path);                        /* isUrlLikePath: `scheme://` */
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
/* renderTreeList with trailingSummary: exactly `items`, then `summary` (NULL: none) as the final muted row */
void tc_tree_list_summary(const strvec_t *items, const char *summary, strvec_t *out);
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
char *tc_scoped_subject(const char *subject, const char *scope); /* formatScopedSubject; NULL when both are absent */
char *tc_file_hyperlink(const char *path, const char *text);  /* fileHyperlink */
char *tc_paths_of(const json_t *args);                       /* path/paths arg joined ", " (NULL none) */
/* A grouped output (grep / ast-grep / ast-edit display content, omp tools/grouped-file-output.ts): lines
 * classified as `# dir/`, `## file#TAG (suffix)` or body. File rows carry their full path; directory rows are
 * dropped when rendering, and every file gets its own group. */
typedef enum { TC_GROW_CONTENT, TC_GROW_DIR, TC_GROW_FILE } tc_grow_kind_t;
typedef struct {
    tc_grow_kind_t kind;
    char *display_path;  /* file rows: the folded directory chain + name, without `#` markers or the hashline tag */
    char *suffix;        /* file rows: a trailing `(2 replacements)` note */
    bool url;            /* file rows: the header names a `scheme://` target */
    const char *lang;    /* body rows: the highlight language of the owning file */
} tc_grow_t;
typedef struct {
    size_t *v;
    size_t n;
} tc_ggroup_t;
typedef struct {
    strvec_t lines;
    tc_grow_t *rows;
    tc_ggroup_t *groups; /* groupsByFile(groupLineIndicesByBlank) */
    size_t ngroups;
    char **frames;       /* memoized code-frame rows */
    const char *lang;    /* highlight language override (ast_grep's `lang`) */
} tc_grouped_t;
/* scope: the search's `searchPath` (owns the lines before any header); lang: language override or NULL */
void tc_grouped_parse(tc_grouped_t *g, const char *text, const char *scope, const char *lang);
void tc_grouped_free(tc_grouped_t *g);
bool tc_grouped_is_match(const char *line);                     /* isMatchRow: `*12│...` */
char *tc_grouped_code_frame(tc_grouped_t *g, size_t index);     /* createCodeFrameStyler: toolOutput gutter + highlighted source; NULL when no frame */
char *tc_grouped_file_row(const tc_grow_t *row);                /* createGroupedRowStyler's file row */
typedef bool (*tc_gunit_fn)(const tc_grouped_t *g, size_t index, void *ud);
/* fitGroupsToBudget: fit groups into `budget` rows (< 0: unlimited), the summary row included. `shown` receives
 * one (borrowing) group per shown group; `summary` is the `… N more` text or NULL. */
void tc_grouped_fit(const tc_grouped_t *g, const tc_ggroup_t *groups, size_t ngroups, int budget, tc_gunit_fn is_unit, void *ud,
                    const char *unit, int64_t total, int gap, tc_ggroup_t **shown, size_t *nshown, char **summary);
/* renderTreeList with a trailing summary: every group a branch, its rows from `style`, the summary last */
typedef char *(*tc_gstyle_fn)(tc_grouped_t *g, size_t index, void *ud);
void tc_grouped_tree(tc_grouped_t *g, const tc_ggroup_t *shown, size_t nshown, const char *summary, tc_gstyle_fn style, void *ud, strvec_t *out);
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
