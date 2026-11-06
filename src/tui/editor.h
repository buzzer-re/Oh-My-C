/*
 * editor.h - multi-line input editor (pi-tui Editor).
 *
 * Keys (via the global keybindings, tui.editor.* / tui.input.*): arrows, ctrl+b/f,
 * alt+b/f / ctrl+left/right (word), home/end/ctrl+a/e, backspace/delete/ctrl+d,
 * ctrl+w / alt+backspace (delete word back), alt+d (word forward), ctrl+u / ctrl+k,
 * ctrl+y (yank) / alt+y (yank-pop) over an emacs kill ring (consecutive kills accumulate),
 * ctrl+] / ctrl+alt+] (jump to the next typed character forward / backward), ctrl+- / ctrl+_
 * (undo), enter (submit), shift+enter / ctrl+j (newline; alt+enter, ctrl+enter and legacy
 * shift+enter encodings are hard-coded newline fallbacks like upstream), tab (autocomplete),
 * up/down (history on the first/last visual line when the buffer is empty or while browsing
 * history), page up/down.
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
    bool args;         /* slash-command argument completions (Enter applies without submitting) */
    strvec_t ghosts;   /* per item: ghost text shown after the cursor while it is selected ("" = none) */
    bool first_desc_match; /* top item matched only by description: Enter submits the raw text */
    strvec_t actions;  /* per item: prompt action id ("" = a plain completion); see tui_editor_t.on_action */
} tui_ac_result_t;

void tui_ac_result_add(tui_ac_result_t *r, const char *value, const char *label, const char *description);
void tui_ac_result_add_icon(tui_ac_result_t *r, const char *value, const char *label, const char *description,
                            const char *icon);
void tui_ac_result_free(tui_ac_result_t *r);
/* a `#` prompt action row: accepting it removes the typed `#query` (kept for "undo") and runs the action */
void tui_ac_result_add_action(tui_ac_result_t *r, const char *label, const char *description, const char *action);

/* `#` prompt action (prompt-action-autocomplete.ts PromptActionDefinition) */
typedef struct {
    char *id, *label, *description, *keywords; /* keywords space-joined */
} tui_ac_action_t;

/* Provider: fill `out` for the text before the cursor on the current line. `first_line` is
 * true when every line above the cursor's is blank (the line starts the submitted prompt). `forced` is
 * true for Tab. Return
 * true if there are suggestions. Accepting a value ending in '/' keeps completion open. */
typedef bool (*tui_autocomplete_fn)(void *ud, const char *line_before, bool first_line, bool forced,
                                    tui_ac_result_t *out);

/* Built-in provider for "/commands" (first line) and "@path" / Tab path completion. */
typedef struct {
    strvec_t commands;     /* names without '/' */
    strvec_t descriptions; /* parallel to commands ("" if none) */
    strvec_t icons;        /* parallel to commands ("" if none) */
    strvec_t hints;        /* parallel to commands: argument hint ("" if none), shown as "<hint> - <description>" */
    /* optional live display description (owned result, NULL = static one); matching uses the static one */
    char *(*describe)(void *ud, const char *name);
    void *describe_ud;
    strvec_t alias_names, alias_targets; /* alias -> command name */
    /* argument completions for "/cmd <arg text>" (first line): 1 items added, 0 none (path completion
     * may follow on Tab), -1 suppress completion (command takes no arguments) */
    int (*args)(void *ud, const char *cmd, const char *arg_text, tui_ac_result_t *out);
    /* ghost hint after the cursor for "/cmd <arg text>" (owned or NULL) */
    char *(*inline_hint)(void *ud, const char *cmd, const char *arg_text);
    void *args_ud;
    char *cwd;             /* NULL = process cwd */
    int max_items;         /* default 50 (slash commands) */
    VEC(tui_ac_action_t) actions; /* `#` prompt actions, in definition order */
} tui_ac_default_t;

tui_ac_default_t *tui_ac_default_new(const char *cwd);
void tui_ac_default_add_command(tui_ac_default_t *p, const char *name, const char *description);
void tui_ac_default_add_command_icon(tui_ac_default_t *p, const char *name, const char *description, const char *icon);
/* a command with an argument hint (NULL = none). Commands named `skill:<name>` collapse into one `/skill:`
 * namespace row until the query commits to it (or a bare name breaks out), and are the only completions
 * offered for a mid-prompt `/token` (collapseSkillNamespace / buildMidPromptSkillCompletions). */
void tui_ac_default_add_command_hint(tui_ac_default_t *p, const char *name, const char *description, const char *icon,
                                     const char *hint);
void tui_ac_default_add_alias(tui_ac_default_t *p, const char *command, const char *alias);
void tui_ac_default_add_action(tui_ac_default_t *p, const char *id, const char *label, const char *description,
                               const char *keywords);
/* `@` completions for the token before the cursor (`@`, `@"…`): fuzzy project-wide search (port of omp's
 * CombinedAutocompleteProvider + the native fuzzyFind scorer), the directory listing for `@`, `@dir/` scopes
 * and paths outside the cwd. Values carry the trailing space a file accept inserts. */
bool tui_ac_at_suggestions(const char *cwd, const char *at_token, tui_ac_result_t *out);
/* start of the live `@` token in `text` (extractAtFilePrefix), or -1 */
long tui_ac_at_prefix_start(const char *text);
/* Tab on a plain path token: the listing of its directory (#getFileSuggestions) */
bool tui_ac_path_suggestions(const char *cwd, const char *token, tui_ac_result_t *out);
void tui_ac_default_free(tui_ac_default_t *p);
/* ghost hint for the text before the cursor (provider-level getInlineHint); owned or NULL */
char *tui_ac_default_inline_hint(void *ud, const char *line_before, bool first_line);
bool tui_ac_default_provider(void *ud, const char *line_before, bool first_line, bool forced, tui_ac_result_t *out);

/* ---------- editor ---------- */
typedef struct tui_editor tui_editor_t;
typedef void (*tui_editor_submit_fn)(tui_editor_t *ed, const char *text, void *ud);
typedef void (*tui_editor_change_fn)(tui_editor_t *ed, void *ud);

typedef struct {
    strvec_t lines;
    size_t cl, cc;
} tui_editor_snapshot_t;

/* An atomic token in the buffer: a large-paste marker or an attachment chip ("📷 #1"). It moves and
 * deletes as one unit and expands to `content` on submit; `style` (optional SGR) paints it in the composer. */
typedef struct {
    int id;
    char *marker;
    char *content;
    char *style;
} tui_editor_paste_t;

/* A staged text paste (CustomEditor.pendingTexts): its chip label (`📄 #N`) sits in the buffer as an atom; the
 * composer attachment band shows a card for each one still referenced. */
typedef struct {
    int n;
    char *label;
    char *content;
    size_t line_count, char_count;
} tui_editor_text_att_t;

struct tui_editor {
    tui_component_t base;
    strvec_t lines; /* always >= 1 line */
    size_t cl, cc;  /* cursor line / byte column */
    int preferred_col;
    VEC(tui_editor_snapshot_t) undo;
    int last_action;
    strvec_t kills;   /* kill ring (kill-ring.ts), most recent last, at most 60 entries */
    int kill_state;   /* previous key's kill/yank chaining: 0 none, 1 kill, 2 yank */
    int jump_mode;    /* character jump awaiting its target: 0 none, 1 forward, -1 backward */
    strvec_t history; /* oldest first */
    int hist_index;   /* -1 = editing draft */
    char *hist_draft;
    VEC(tui_editor_paste_t) pastes;
    int paste_counter;
    /* bracketed-paste interceptor (image paths, empty pastes): return true when it consumed the text */
    bool (*paste_hook)(void *ud, tui_editor_t *ed, const char *text);
    char *(*row_hook)(void *ud, tui_editor_t *ed, const char *row);
    void *row_hook_ud;
    void *paste_hook_ud;
    /* completed-token collapse (CustomEditor #collapseSkillTokens): in `line` of the buffer `text`, the first
     * token at or after byte `from` to turn into an atom (span, label, expansion, style: owned); false when
     * none. A token that ends the last line is still being typed and stays literal. */
    bool (*token_fn)(void *ud, const char *text, const char *line, size_t from, size_t *start, size_t *end, char **label,
                     char **expansion, char **style);
    void *token_ud;
    bool collapsing;
    /* onLargePaste: a marker-sized paste (> 10 lines or > 1000 chars, sanitized); true = the host took it and
     * the editor inserts nothing (no `[Paste #N]` marker, no undo step) */
    bool (*large_paste_hook)(void *ud, tui_editor_t *ed, const char *text, size_t line_count);
    void *large_paste_ud;
    VEC(tui_editor_text_att_t) texts; /* staged text attachments, reset with the draft (clearDraft) */
    int text_counter;

    tui_autocomplete_fn ac_fn;
    void *ac_ud;
    tui_select_list_t *ac_list; /* non-NULL while autocomplete is open */
    size_t ac_prefix_len;
    bool ac_enter_accepts;
    bool ac_args;          /* the open list completes a slash-command argument */
    strvec_t ac_ghosts;    /* ghost text per list item */
    int ac_max_visible;
    /* inline ghost hint for the text before the cursor (end of line only); owned result or NULL */
    char *(*hint_fn)(void *ud, const char *line_before, bool first_line);
    void *hint_ud;

    strvec_t ac_actions;   /* prompt action id per list item ("" = plain completion) */
    /* runs an accepted `#` prompt action; `prefix` is the typed `#query` */
    void (*on_action)(tui_editor_t *ed, const char *action, const char *prefix, void *ud);
    void *action_ud;

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
/* insert an atomic token at the cursor (one undo step); expands to `expansion` on submit */
void tui_editor_insert_atom(tui_editor_t *ed, const char *label, const char *expansion, const char *style);
/* decorate one composer row just before it is drawn (the magic-keyword glow); returns the row (owned) or NULL to keep it */
void tui_editor_set_row_hook(tui_editor_t *ed, char *(*fn)(void *ud, tui_editor_t *ed, const char *row), void *ud);
void tui_editor_set_paste_hook(tui_editor_t *ed, bool (*fn)(void *ud, tui_editor_t *ed, const char *text), void *ud);
void tui_editor_set_token_hook(tui_editor_t *ed,
                               bool (*fn)(void *ud, const char *text, const char *line, size_t from, size_t *start, size_t *end,
                                          char **label, char **expansion, char **style),
                               void *ud);
void tui_editor_set_large_paste_hook(tui_editor_t *ed,
                                     bool (*fn)(void *ud, tui_editor_t *ed, const char *text, size_t line_count),
                                     void *ud);
/* insertTextAttachment: stage `content` as text attachment #N, N = text_counter + 1 (label `<chip.paste> #N`,
 * painted with the SGR `style`), and insert the atom; it expands to `expansion` (NULL = the content) on submit.
 * Returns N. */
int tui_editor_insert_text_attachment(tui_editor_t *ed, const char *content, const char *expansion, const char *style);
/* undoPastTransientText: drop `transient` just before the cursor plus the undo snapshots it produced, then undo */
void tui_editor_undo_past_transient(tui_editor_t *ed, const char *transient);
void tui_editor_move_to_message_start(tui_editor_t *ed);
void tui_editor_move_to_message_end(tui_editor_t *ed);
void tui_editor_move_to_line_start(tui_editor_t *ed);
void tui_editor_move_to_line_end(tui_editor_t *ed);
bool tui_editor_is_empty(const tui_editor_t *ed);
void tui_editor_clear(tui_editor_t *ed);
void tui_editor_undo(tui_editor_t *ed);
void tui_editor_add_history(tui_editor_t *ed, const char *text);
/* Push a non-blank draft onto the in-memory up/down history (Editor.rememberDraft). */
void tui_editor_remember_draft(tui_editor_t *ed);
/* Submit the buffer as Enter would (history, clear, on_submit). */
void tui_editor_submit(tui_editor_t *ed);
/* Owned copy of the line the cursor is on. */
char *tui_editor_current_line(const tui_editor_t *ed);
void tui_editor_set_on_submit(tui_editor_t *ed, tui_editor_submit_fn fn, void *ud);
void tui_editor_set_on_change(tui_editor_t *ed, tui_editor_change_fn fn);
void tui_editor_set_autocomplete(tui_editor_t *ed, tui_autocomplete_fn fn, void *ud);
void tui_editor_set_inline_hint(tui_editor_t *ed, char *(*fn)(void *ud, const char *line_before, bool first_line), void *ud);
char *tui_editor_ghost(tui_editor_t *ed); /* the ghost text shown after the cursor now (owned) or NULL */
void tui_editor_set_placeholder(tui_editor_t *ed, const char *text);
void tui_editor_set_autocomplete_max_visible(tui_editor_t *ed, double n); /* autocompleteMaxVisible: clamped to 3..20 */
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
