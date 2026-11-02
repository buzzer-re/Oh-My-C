/* patch.h - edit modes: replace, patch (unified-diff hunks), apply_patch (Codex envelope), sloppy, hashline.
 * Port of omp's pi-edit mode engines + session apply: stage every file in memory (all-or-nothing), then write
 * in payload order and render the model-facing result text and details like upstream. */
#ifndef OMC_PATCH_H
#define OMC_PATCH_H

#include "core/core.h"
#include "core/json.h"

typedef enum { PE_REPLACE, PE_PATCH, PE_APPLY_PATCH, PE_SLOPPY, PE_HASHLINE } pe_mode_t;

typedef struct {
    const char *cwd;
    bool allow_fuzzy;       /* edit.fuzzyMatch */
    double fuzzy_threshold; /* edit.fuzzyThreshold */
    /* resolve an authored path to an absolute one (owned); NULL + *err (owned) on failure.
     * NULL callback => lexical join with cwd (~ expanded). */
    char *(*resolve)(void *ud, const char *display, char **err);
    /* called after a file lands; returns owned diagnostics text or NULL */
    char *(*after_write)(void *ud, const char *abs_path);
    /* optional remote I/O (ssh://): when remote(abs) is true, reads and writes go through these hooks.
     * remote_read returns 0 (owned *data), ENOENT when missing, else an error with *err (owned);
     * EISDIR for directories. remote_write returns 0 or non-zero with *err. Deletes/moves are refused. */
    bool (*remote)(void *ud, const char *abs_path);
    int (*remote_read)(void *ud, const char *abs_path, char **data, size_t *len, char **err);
    int (*remote_write)(void *ud, const char *abs_path, const char *data, size_t len, char **err);
    /* ACP client bridge (routeWriteThroughBridge) for creates / updates: 0 written, -1 failed (*err owned),
     * 1 not routed (local write). NULL = always local. */
    int (*bridge_write)(void *ud, const char *abs_path, const char *data, size_t len, char **err);
    void *ud;
    /* hashline mode: the session edit store (edit_store_t *), edit.enforceSeenLines, display column cap */
    void *store;
    bool enforce_seen;
    size_t max_columns;
} pe_opts_t;

typedef struct {
    bool is_error;
    char *text;             /* model-facing text (owned) */
    const char *error_kind; /* stable class on error ("parse", "apply", "no_match", ...) */
    json_t *details;        /* owned */
    int writes;             /* number of filesystem writes performed */
} pe_result_t;

/* "replace" | "patch" | "apply_patch" | "sloppy" | "hashline" -> mode, else -1 */
int pe_mode_parse(const char *name);
const char *pe_mode_name(pe_mode_t m);
/* Stage + apply one edit call. args: replace {path, old_string, new_string, replace_all?, edits?},
 * patch {path, edits:[{op?, rename?, diff?}]}, apply_patch / sloppy / hashline {input}. */
void pe_run(pe_mode_t mode, const json_t *args, const pe_opts_t *o, pe_result_t *out);
void pe_result_free(pe_result_t *r);

/* ---- sloppy engine (sloppy.c) ---- */
typedef struct {
    char *path;
    char *body;
} sloppy_section_t;
typedef VEC(sloppy_section_t) sloppy_sections_t;
/* split a payload into per-file sections; false + *err (owned, parse error) on malformed input */
bool sloppy_split_sections(const char *input, sloppy_sections_t *out, char **err);
void sloppy_sections_free(sloppy_sections_t *s);
/* apply one section body to LF text; returns owned new text or NULL + *err (owned) / *err_kind.
 * notes receives post-apply advisories. */
char *sloppy_apply(const char *text, const char *body, const char *path, strvec_t *notes, char **err,
                   const char **err_kind);

#endif
