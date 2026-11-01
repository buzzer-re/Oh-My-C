/* hashline.h - the hashline edit language (`[path#TAG]` sections of PUT/CUT/REM/MV ops).
 * Port of omp pi-edit modes/hashline (tokenizer, parser, input sections, clipboard, block lowering, applier core,
 * snapshot mismatch / seen-line guard messages). Staging into files and the result rendering live in patch.c
 * (PE_HASHLINE); this module works on LF-normalized text plus the session's edit store. Message strings are
 * byte-identical to upstream. Tree-sitter driven repairs (boundary echoes, landing probes, parse-regression
 * warnings) are not ported: omc resolves `N*` blocks with a bracket/indent heuristic instead. */
#ifndef OMC_HASHLINE_H
#define OMC_HASHLINE_H

#include "core/core.h"

typedef struct hl_patch hl_patch_t;
typedef struct hl_clip hl_clip_t;

/* Split and coalesce `[path#TAG]` sections (input.rs Patch::parse). NULL + *err (owned) on a parse error; a patch
 * with zero sections is returned when headers carry no ops. */
hl_patch_t *hl_patch_parse(const char *input, const char *cwd, char **err);
void hl_patch_free(hl_patch_t *p);
size_t hl_patch_count(const hl_patch_t *p);
const char *hl_section_path(const hl_patch_t *p, size_t i);
const char *hl_section_tag(const hl_patch_t *p, size_t i); /* NULL when untagged */
/* path of the first header row (for header-only input), owned or NULL */
char *hl_first_header_path(const char *input, const char *cwd);
/* "Edit rejected for {path}: no ops follow the header; ..." */
char *hl_header_only_message(const char *path, const char *actual_tag);

/* Parse one section body. false + *err (owned); *range_err set when the failure is an inverted absolute range
 * (callers enrich it from the file text with hl_range_error_message). */
bool hl_section_parse(hl_patch_t *p, size_t i, char **err, bool *range_err);
char *hl_range_error_message(hl_patch_t *p, size_t i, const char *text);
/* 0 = none, 1 = REM, 2 = MV (dest borrowed into *dest) */
int hl_section_file_op(hl_patch_t *p, size_t i, const char **dest);

/* clipboard batch over the store's persisted named registers (store = edit_store_t *) */
hl_clip_t *hl_clip_start(void *store);
void hl_clip_commit(hl_clip_t *c, void *store);
void hl_clip_free(hl_clip_t *c);

typedef struct {
    char *text;              /* new LF text (owned) */
    int64_t first_changed;   /* 0 = none */
    strvec_t warnings;       /* parser + apply warnings */
    strvec_t block_preview;  /* "PUT 3*: → resolved lines 3-9 (7 lines)" */
} hl_applied_t;
void hl_applied_free(hl_applied_t *a);

/* apply_with_recovery: tag check, block lowering, clipboard, seen-line guard, apply. `canonical` is the absolute
 * path the store keys snapshots by. false + *err (owned). */
bool hl_section_apply(hl_patch_t *p, size_t i, const char *canonical, const char *text, void *store, hl_clip_t *clip,
                      bool enforce_seen, size_t max_columns, hl_applied_t *out, char **err);
/* mismatch rejection for an untagged section (current tag + anchored context) */
char *hl_mismatch_message(hl_patch_t *p, size_t i, const char *canonical, const char *text, const char *expected,
                          void *store);

/* no-op bookkeeping: returns the repeat count; *escalate when the hard limit is hit */
uint32_t hl_record_noop(void *store, const char *canonical, const char *input, bool *escalate);
void hl_reset_noop(void *store, const char *canonical);
char *hl_no_change_message(const char *path);
char *hl_no_change_loop_message(const char *path, uint32_t count);

/* After a committed write: record `after` under `key` with the seen lines its response body displays plus the
 * unshifted lines carried from the prior snapshot of `before` (under `before_key`). Returns the tag (owned). */
char *hl_record_after(void *store, const char *before_key, const char *key, const char *before, const char *after,
                      const char *body, bool track);
/* record a snapshot without touching its seen lines; returns the tag (owned) */
char *hl_record_plain(void *store, const char *key, const char *text);
void hl_store_invalidate(void *store, const char *key);

/* write-content.ts stripWriteContent (hashline mode): drop `[path#TAG]` headers / `N:` prefixes copied from read
 * output when every content row carries one; *stripped reports a change. Owned. */
char *hl_strip_write_content(const char *content, bool *stripped);

#endif
