/* diff.h - line diff (Myers) producing unified diff text */
#ifndef OMC_DIFF_H
#define OMC_DIFF_H

#include "core/core.h"

/* unified diff of two texts; context lines around changes. Returns owned text ("" if equal). */
char *diff_unified(const char *a, const char *b, const char *path_a, const char *path_b, int context);
/* first changed line (1-based in b) or 0 */
int64_t diff_first_changed_line(const char *a, const char *b);
/* counts of added / removed lines */
void diff_stats(const char *a, const char *b, int64_t *added, int64_t *removed);
/* omp's compact numbered diff (`-N|old`, `+N|new`, ` N|ctx`, `context` lines around changes; 2 when < 0).
 * Owned; `first_changed` (may be NULL) gets the first changed line of b (0 when equal). */
char *diff_numbered(const char *a, const char *b, int context, int64_t *first_changed);

/* ---- jsdiff-compatible renderings used by the edit engines (port of omp pi-edit diff_string.rs) ---- */
/* Numbered diff rows ("+N|text", "-N|text", " N|text", "" gaps) with nearby (context, default 2 when < 0) and
 * enclosing-bracket context. *first_changed = first changed line in the new text (0 = none). Owned. */
char *diff_numbered(const char *old_text, const char *new_text, int context, int64_t *first_changed);
/* Numbered unified hunks ("@@ -a,b +c,d @@" + numbered rows), context default 3 when < 0. Owned. */
char *diff_numbered_unified(const char *old_text, const char *new_text, int context, int64_t *first_changed);
/* Compact current-file preview ("N:text" rows, long added runs elided with "…") of a numbered diff. Owned. */
char *diff_compact_preview(const char *numbered, size_t *added, size_t *removed);

#endif
