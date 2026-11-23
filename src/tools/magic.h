/* magic.h - `edit.mode: magic`: the calling agent describes its changes in words under one `# path` heading per
 * file; an auxiliary model (edit.magic.model, else the smol role) reads each numbered file and answers with line
 * operations, which compile to one exact `replace` edit per file.
 * Port of omp edit/magic.ts, edit/magic-lines.ts, edit/magic-view.ts and tui tools/magic-sections.ts (v19.1.0).
 * Not ported: omp's native "does the file still parse" guard (pi-natives parseCodeStatus); every other rejection
 * path (block hints, the doubted-range confirmation, apply errors) is the same. */
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

/* ---------- file views (edit/magic-view.ts) ---------- */

typedef struct {
    int from, to; /* inclusive, zero-based */
} magic_span_t;

typedef struct {
    char *text;
    bool partial;
    magic_span_t *shown; /* partial views: the shown stretches */
    size_t nshown;
} magic_view_t;
typedef VEC(magic_view_t) magic_views_t;

/* fileViews: an excerpt of the lines `change` names (when it leaves out at least half of a file of 200+ lines), then
 * the whole numbered file; views longer than `limit` UTF-16 units are dropped (limit < 0: no limit) */
void magic_file_views(const char *content, const char *change, double limit, magic_views_t *out);
void magic_views_free(magic_views_t *v);

/* ---------- line operations (edit/magic-lines.ts) ---------- */

typedef struct {
    char *old_s, *new_s; /* the compiled replace edit (owned), or NULL with problem */
    int (*ranges)[2];    /* one-based inclusive ranges the reply deletes, moves or swaps */
    size_t nranges;
    char *problem;       /* worded for the apply model (owned) */
} magic_compiled_t;

/* compileLineEdits: `cut` marks a reply that ran into the output limit */
void magic_compile(const char *reply, const char *content, const magic_view_t *view, bool cut, magic_compiled_t *out);
void magic_compiled_free(magic_compiled_t *c);
/* blockHints: doubts about ranges that cut a block or run past it ("" when none; owned) */
char *magic_block_hints(const char *content, int (*ranges)[2], size_t n, const magic_view_t *view);

/* ---------- the described change (edit/magic.ts applyDescribedChange) ---------- */

typedef struct magic_io {
    /* one completion of at most max_tokens output tokens: the owned reply text with *truncated, or NULL with *err
     * (owned) for a failure that ends the call (a ToolError) */
    char *(*complete)(struct magic_io *io, const char *request, int64_t max_tokens, bool *truncated, char **err);
    /* apply one replace edit of the snapshot (check_snapshot: refuse a file that changed since it was read) */
    void (*apply)(struct magic_io *io, const char *old_s, const char *new_s, bool check_snapshot, pe_result_t *r);
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
