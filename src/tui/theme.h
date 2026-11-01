/*
 * theme.h - named color slots (upstream omp theme keys), built-in dark/light themes and
 * JSON theme loading (`~/.omc/agent/themes/<name>.json`, upstream theme format:
 * {"name", "vars": {...}, "colors": {...}, "symbols": {"spinnerFrames": [...]}}).
 *
 * Color values: "#RRGGBB", a 0..255 palette index, a `vars` reference, or "" (terminal
 * default). Truecolor escapes are used when the terminal supports them (COLORTERM etc.),
 * otherwise hex colors are mapped to the nearest xterm-256 color.
 */
#ifndef OMC_TUI_THEME_H
#define OMC_TUI_THEME_H

#include "core/core.h"
#include "core/json.h"

#define TUI_THEME_SLOTS(X)                                                                          \
    X(ACCENT, "accent") X(BORDER, "border") X(BORDER_ACCENT, "borderAccent")                       \
    X(BORDER_MUTED, "borderMuted") X(SUCCESS, "success") X(ERROR, "error") X(WARNING, "warning")   \
    X(MUTED, "muted") X(DIM, "dim") X(TEXT, "text") X(THINKING_TEXT, "thinkingText")               \
    X(SELECTED_BG, "selectedBg") X(USER_MESSAGE_BG, "userMessageBg")                               \
    X(USER_MESSAGE_TEXT, "userMessageText") X(CUSTOM_MESSAGE_BG, "customMessageBg")                \
    X(CUSTOM_MESSAGE_TEXT, "customMessageText") X(CUSTOM_MESSAGE_LABEL, "customMessageLabel")      \
    X(TOOL_PENDING_BG, "toolPendingBg") X(TOOL_SUCCESS_BG, "toolSuccessBg")                        \
    X(TOOL_ERROR_BG, "toolErrorBg") X(TOOL_TITLE, "toolTitle") X(TOOL_OUTPUT, "toolOutput")        \
    X(ASSISTANT_MESSAGE_TEXT, "assistantMessageText") X(MD_HEADING, "mdHeading")                   \
    X(MD_LINK, "mdLink") X(MD_LINK_URL, "mdLinkUrl") X(MD_CODE, "mdCode")                          \
    X(MD_CODE_BLOCK, "mdCodeBlock") X(MD_CODE_BLOCK_BORDER, "mdCodeBlockBorder")                   \
    X(MD_QUOTE, "mdQuote") X(MD_QUOTE_BORDER, "mdQuoteBorder") X(MD_HR, "mdHr")                    \
    X(MD_LIST_BULLET, "mdListBullet") X(TOOL_DIFF_ADDED, "toolDiffAdded")                          \
    X(TOOL_DIFF_REMOVED, "toolDiffRemoved") X(TOOL_DIFF_CONTEXT, "toolDiffContext")                \
    X(SYNTAX_COMMENT, "syntaxComment") X(SYNTAX_KEYWORD, "syntaxKeyword")                          \
    X(SYNTAX_FUNCTION, "syntaxFunction") X(SYNTAX_VARIABLE, "syntaxVariable")                      \
    X(SYNTAX_STRING, "syntaxString") X(SYNTAX_NUMBER, "syntaxNumber") X(SYNTAX_TYPE, "syntaxType") \
    X(SYNTAX_OPERATOR, "syntaxOperator") X(SYNTAX_PUNCTUATION, "syntaxPunctuation")                \
    X(THINKING_OFF, "thinkingOff") X(THINKING_MINIMAL, "thinkingMinimal")                          \
    X(THINKING_LOW, "thinkingLow") X(THINKING_MEDIUM, "thinkingMedium")                            \
    X(THINKING_HIGH, "thinkingHigh") X(THINKING_XHIGH, "thinkingXhigh")                            \
    X(THINKING_MAX, "thinkingMax") X(BASH_MODE, "bashMode") X(PYTHON_MODE, "pythonMode")           \
    X(STATUS_LINE_BG, "statusLineBg") X(STATUS_LINE_SEP, "statusLineSep")                          \
    X(STATUS_LINE_MODEL, "statusLineModel") X(STATUS_LINE_PATH, "statusLinePath")                  \
    X(STATUS_LINE_GIT_CLEAN, "statusLineGitClean") X(STATUS_LINE_GIT_DIRTY, "statusLineGitDirty")  \
    X(STATUS_LINE_CONTEXT, "statusLineContext") X(STATUS_LINE_SPEND, "statusLineSpend")            \
    X(STATUS_LINE_STAGED, "statusLineStaged") X(STATUS_LINE_DIRTY, "statusLineDirty")              \
    X(STATUS_LINE_UNTRACKED, "statusLineUntracked") X(STATUS_LINE_OUTPUT, "statusLineOutput")      \
    X(STATUS_LINE_COST, "statusLineCost") X(STATUS_LINE_SUBAGENTS, "statusLineSubagents")          \
    X(LINK, "link")

typedef enum {
#define TUI_TC_ENUM(id, name) TUI_TC_##id,
    TUI_THEME_SLOTS(TUI_TC_ENUM)
#undef TUI_TC_ENUM
    TUI_TC__COUNT
} tui_tc_t;

typedef enum { TUI_COLOR_TRUECOLOR, TUI_COLOR_256 } tui_color_mode_t;

#define TUI_MAX_SPINNER_FRAMES 32

typedef struct {
    char *name;
    tui_color_mode_t mode;
    char fg[TUI_TC__COUNT][24]; /* full escape, e.g. "\x1b[38;2;1;2;3m" or "\x1b[39m" */
    char bg[TUI_TC__COUNT][24];
    int64_t rgb[TUI_TC__COUNT]; /* 0xRRGGBB (palette indices resolved), or -1 for terminal default */
    char *spinner[TUI_MAX_SPINNER_FRAMES];
    int nspinner;
} tui_theme_t;

const char *tui_theme_slot_name(tui_tc_t slot);
int tui_theme_slot_from_name(const char *name); /* -1 if unknown */

tui_color_mode_t tui_detect_color_mode(void);
/* "dark" or "light" from COLORFGBG (default dark). */
const char *tui_detect_theme_name(void);

/* Build from a parsed upstream-format theme JSON. NULL + *err (owned) on failure. */
tui_theme_t *tui_theme_from_json(const json_t *j, tui_color_mode_t mode, char **err);
/* Built-in "dark"/"light", else <agent_dir>/themes/<name>.json. */
tui_theme_t *tui_theme_load(const char *name, tui_color_mode_t mode, char **err);
tui_theme_t *tui_theme_builtin(const char *name, tui_color_mode_t mode); /* NULL if not builtin */
void tui_theme_free(tui_theme_t *t);

/* Process-wide current theme (lazily the detected built-in). */
const tui_theme_t *tui_theme(void);
void tui_theme_set(tui_theme_t *t); /* takes ownership */

/* Styling helpers. `th` may be NULL for the current theme. */
void tui_fg(sb_t *b, const tui_theme_t *th, tui_tc_t slot, const char *text);
void tui_bg(sb_t *b, const tui_theme_t *th, tui_tc_t slot, const char *text);
char *tui_fgs(const tui_theme_t *th, tui_tc_t slot, const char *text); /* owned */
char *tui_bgs(const tui_theme_t *th, tui_tc_t slot, const char *text); /* owned */
const char *tui_fg_seq(const tui_theme_t *th, tui_tc_t slot);
const char *tui_bg_seq(const tui_theme_t *th, tui_tc_t slot);
/* Simple attribute wrappers (owned results). */
char *tui_bold(const char *text);
char *tui_italic(const char *text);
char *tui_underline(const char *text);
char *tui_dim(const char *text);
char *tui_inverse(const char *text);
/* nearest xterm-256 index for an RGB color */
int tui_rgb_to_256(int r, int g, int b);

#endif
