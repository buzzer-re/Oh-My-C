/* rxbt.h - bounded backtracking regex engine for features the linear engine rejects:
 * look-ahead (?= ?!), look-behind (?<= ?<!) and backreferences (\1, \k<name>).
 * Same syntax as regex.h otherwise; a step budget bounds pathological patterns. */
#ifndef OMC_RXBT_H
#define OMC_RXBT_H

#include "core/core.h"

typedef struct rxbt rxbt_t;

/* flags: RX_ICASE, RX_MULTILINE, RX_DOTALL, RX_EXTENDED, RX_UNGREEDY, RX_LINE (see regex.h) */
rxbt_t *rxbt_compile(const char *pattern, int flags, char **err);
void rxbt_free(rxbt_t *r);
int rxbt_ngroups(const rxbt_t *r); /* including group 0 */
int rxbt_group_index(const rxbt_t *r, const char *name);
const char *rxbt_group_name(const rxbt_t *r, int i);
/* search from start; caps (2*ngroups entries, -1 when unset) may be NULL. Returns true on match.
 * Exceeding the step budget is treated as no match. */
bool rxbt_search(const rxbt_t *r, const char *s, size_t len, size_t start, ptrdiff_t *caps, int ncaps);

#endif
