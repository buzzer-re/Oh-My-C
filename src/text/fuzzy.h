/*
 * fuzzy.h - fzf-style fuzzy matching (optimal subsequence alignment with bonuses for word
 * boundaries, path separators, camelCase and consecutive runs). Smart case per term: a term with
 * an uppercase letter matches case-sensitively. Space-separated terms must all match (AND).
 */
#ifndef OMC_TEXT_FUZZY_H
#define OMC_TEXT_FUZZY_H

#include "core/core.h"

/* Score `text` against `pattern`; -1 = no match, otherwise >= 0 (higher is better; empty pattern = 0).
 * positions_out (may be NULL) receives the byte offsets in `text` of each matched pattern character
 * (non-space pattern chars in order); it must hold utf8_strlen(pattern) ints. */
int fuzzy_score(const char *pattern, const char *text, int *positions_out);

typedef struct {
    size_t index; /* index into the input array */
    int score;
} fuzzy_hit_t;

/* Score all items, keep matches, sort by score desc, then shorter text, then index.
 * Returns the number of hits written to *out (owned array, free()). */
size_t fuzzy_rank(const char *pattern, const char *const *items, size_t n, fuzzy_hit_t **out);

#endif
