/* magic.h - `edit.mode: magic`: the calling agent describes its changes in words under one `# path` heading per
 * file; an auxiliary model (edit.magic.model, else the smol role) carries each file's change out on an in-memory draft
 * through a multi-turn `replace` / `read` / `search` tool loop, and the finished draft is committed as one exact
 * `replace` edit per file.
 * Port of omp edit/magic.ts, edit/magic-tools.ts, edit/magic-calls.ts, edit/magic-view.ts and tui
 * tools/magic-sections.ts (v19.1.1). omp's syntax guard (native parseCodeStatus, tree-sitter) is ported for the
 * language omc can parse, JSON; other languages are not checked (omp checks every tree-sitter language). */
#ifndef OMC_MAGIC_H
#define OMC_MAGIC_H

#include "core/core.h"
#include "core/json.h"
#include "patch.h"
#include "tools.h"

/* ---------- payload sections (tui tools/magic-sections.ts) ---------- */

/* one file's run of described changes, in payload order; runs naming the same path stay apart */
typedef struct {
    char *path;
    char *text;
} magic_run_t;
typedef VEC(magic_run_t) magic_runs_t;

/* splitMagicSections: `(path, text)` runs in order; *outside (owned, may be NULL) is whatever precedes the first
 * `# path` line (lines joined without separators). A `# ` line inside a code fence is change text. Also the hook
 * the TUI's live Markdown view of a streaming payload reads (stream c). */
void magic_split_sections(const char *input, magic_runs_t *runs, char **outside);
void magic_runs_free(magic_runs_t *r);
/* magicEditPaths: files a payload names so far, in order and without repeats (tolerates a streaming payload) */
void magic_edit_paths(const char *input, strvec_t *out);

/* parseMagicInput: per-file sections, same-path sections merged in order (bodies trimmed, joined by a blank line) */
typedef struct {
    char *path;
    char *change;
} magic_section_t;
typedef VEC(magic_section_t) magic_sections_t;
bool magic_parse_input(const char *input, magic_sections_t *out, char **err);
void magic_sections_free(magic_sections_t *s);

/* ---------- initial file views (edit/magic-view.ts) ---------- */

typedef struct {
    char *text;      /* `N│text` lines; `⋮ …` marks omitted stretches */
    int total_lines;
    bool partial;
} magic_view_t;
typedef VEC(magic_view_t) magic_views_t;

/* fileViews: an excerpt of the lines `change` names (when it leaves out at least half of a file of 200+ lines), then
 * the whole numbered file; with neither within `limit` UTF-16 units (limit < 0: no limit), a head+tail preview the
 * model navigates with `read` and `search`. Never empty. */
void magic_file_views(const char *content, const char *change, double limit, magic_views_t *out);
void magic_views_free(magic_views_t *v);

/* ---------- draft tools (edit/magic-tools.ts) ---------- */

/* APPLY_TOOLS: [{name, description, parameters}] for read, search and replace (owned) */
json_t *magic_apply_tools(void);

/* one staged exact replacement (AppliedReplace), replayable against the draft it was made on */
typedef struct {
    char *old_s, *rep;
    bool all;
    size_t at; /* byte offset of the (first) match in the pre-edit draft */
    int line;  /* its 1-based line */
} magic_step_t;
typedef VEC(magic_step_t) magic_steps_t;
void magic_steps_free(magic_steps_t *s);

/* applyReplace: true with *content (the new draft), *step and *message (all owned); false with *error (owned), the
 * draft untouched */
bool magic_apply_replace(const char *text, const json_t *args, char **content, magic_step_t *step, char **message,
                         char **error);
/* readBufferLines / searchBuffer: the owned result text; *is_error for a model-facing error */
char *magic_read_lines(const char *text, const json_t *args, bool *is_error);
char *magic_search(const char *text, const json_t *args, bool *is_error);
/* diagnoseBufferSyntax: the replace after which the draft first stopped parsing, with the current lines near it
 * (owned). parses(code, ud): the language's parse check. */
char *magic_diagnose_syntax(const char *original, const char *working, const magic_step_t *applied, size_t n,
                            bool (*parses)(const char *code, void *ud), void *ud);

/* ---------- written tool calls (edit/magic-calls.ts) ---------- */

/* normalizeApplyArguments: a call's arguments with schema-compatible strings coerced and null options dropped (owned),
 * or NULL with *error (owned) for malformed arguments; *repaired when they were recovered from duplicated raw JSON
 * (`__rawJson`), so the reply replays them instead */
json_t *magic_normalize_args(const json_t *call, bool *repaired, char **error);
/* parseWrittenToolCalls: [{name, arguments}] from fenced JSON / Python and bare Python call lines (owned); *attempted
 * when the text looked like a call */
json_t *magic_parse_written_calls(const char *text, bool *attempted);

/* ---------- syntax (parseCodeStatus) ---------- */

/* 1: `code` parses as the language `path` names; 0: it does not; -1: no language omc checks (only JSON) */
int magic_parse_status(const char *path, const char *code);

/* ---------- the described change (edit/magic.ts applyDescribedChange) ---------- */

typedef struct magic_io {
    /* one completion turn over the conversation so far with the apply tools: the owned assistant message, or NULL
     * with *err (owned) for a failure that ends the call (a ToolError) */
    json_t *(*complete)(struct magic_io *io, const json_t *messages, const json_t *tools, char **err);
    /* commit the finished draft: one `replace_all` edit of the whole snapshot, refused when the file changed since it
     * was read */
    void (*apply)(struct magic_io *io, const char *old_s, const char *new_s, pe_result_t *r);
    void *ud;
} magic_io_t;

/* true with *res (the applied result, owned) or false with *err (owned ToolError text); the file is untouched on
 * false. views: narrowest first, at least one. */
bool magic_apply_described(const char *path, const char *change, const char *content, const magic_views_t *views,
                           magic_io_t *io, pe_result_t *res, char **err);

/* runMagicEdit: one `edit` call in magic mode. Every section is applied on its own through the `replace` engine
 * (base: the edit tool's engine options); a failed section leaves its file untouched and does not stop the others.
 * The result is an error only when no section applied (or the payload / apply model is unusable). */
void magic_edit_run(tool_env_t *env, volatile sig_atomic_t *cancel, const char *input, const pe_opts_t *base,
                    tool_result_t *out);

#endif
