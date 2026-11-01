/* fuzzy.h - word-local fuzzy matching and ranking (port of omp packages/tui/src/fuzzy.ts).
 *
 * Lower score = better match. A query is split into space-separated tokens which must all
 * match; camelCase/punctuation in the text split words. Ranking is stable (equal scores keep
 * input order), exactly like upstream's `fuzzyFilter`.
 */
#ifndef OMC_TUI_FUZZY_H
#define OMC_TUI_FUZZY_H

#include "core/core.h"

typedef struct tui_fuzzy_query tui_fuzzy_query_t;

/* Prepare a query once for matching many texts. Never NULL (a blank query matches everything). */
tui_fuzzy_query_t *tui_fuzzy_query_new(const char *query);
void tui_fuzzy_query_free(tui_fuzzy_query_t *q);
/* true when `text` matches; *score (optional) receives the score. */
bool tui_fuzzy_query_match(const tui_fuzzy_query_t *q, const char *text, double *score);

/* Rank texts[0..n) against `query`: writes matching indices, best first, into out_idx (capacity n)
 * and returns their count. A blank query returns every index in input order. */
size_t tui_fuzzy_rank(const char *query, const char *const *texts, size_t n, size_t *out_idx);

/* Upstream normalizeForSearch (camelCase split, lowercase, punctuation -> single spaces). Owned. */
char *tui_fuzzy_normalize(const char *s);

#endif
