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

#endif
