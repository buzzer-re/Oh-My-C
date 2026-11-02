/* changelog.h - `/changelog` (port of omp utils/changelog.ts): the bundled omp CHANGELOG.md
 * (assets/CHANGELOG.md.z, zlib) parsed into release entries, the `[full|last [N]]` views and the rendered
 * markdown. */
#ifndef OMC_CHANGELOG_H
#define OMC_CHANGELOG_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { CHANGELOG_RECENT, CHANGELOG_FULL, CHANGELOG_LAST } changelog_kind_t;

typedef struct {
    changelog_kind_t kind;
    int count; /* recent / last */
} changelog_view_t;

#define CHANGELOG_USAGE "Usage: /changelog [full|last [N]]"

/* parseChangelogView: false with *err (owned) for a usage error */
bool changelog_parse_view(const char *args, changelog_view_t *v, char **err);
/* the selected entries, oldest first, joined by blank lines (renderChangelogEntries); NULL when none.
 * *shown receives the number of entries (may be NULL). Owned. */
char *changelog_markdown(const changelog_view_t *v, size_t *shown);
/* the panel title for a view with `shown` entries (handleChangelogCommand); static */
const char *changelog_title(const changelog_view_t *v, size_t shown, char *buf, size_t bufsz);
/* number of release entries in the bundled changelog */
size_t changelog_entry_count(void);

#endif
