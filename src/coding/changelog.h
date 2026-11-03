/* changelog.h - `/changelog` (port of omp utils/changelog.ts): the bundled omp CHANGELOG.md
 * (assets/CHANGELOG.md.z, zlib) parsed into release entries, the `[full|last [N]]` views and the rendered
 * markdown. */
#ifndef OMC_CHANGELOG_H
#define OMC_CHANGELOG_H

#include "core/core.h"

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

/* ---- startup release notes (startup.changelogMode; resolveStartupChangelogForDisplay) ---- */

/* StartupChangelogSelection */
typedef struct {
    char *markdown;          /* the selected entries, newest first, capped at 64 KB (owned) */
    bool truncated;
    int selected;            /* entries shown (at most 3) */
    int total_unseen;        /* entries newer than the marker */
    char latest[32];         /* version of the newest shown entry */
    int change_count;
    strvec_t categories;     /* `###` section names in first-seen order ("Other" before any heading) */
    VEC(int) counts;         /* top-level list items per category */
} changelog_startup_t;

/* the bundled changelog's newest version: what the marker records (omc ships omp's changelog). Static. */
const char *changelog_current_version(void);
/* readLastChangelogVersion at `marker_path` (`<agent>/last-changelog-version`), then omp's decision: no marker or
 * an unparsable one records the current version and shows nothing; the current version shows nothing; `hidden`
 * advances the marker only for an upgrade; otherwise the up to 3 entries newer than the marker are selected and
 * the marker advances. True when there is something to show. */
bool changelog_startup_resolve(const char *mode, const char *marker_path, changelog_startup_t *out);
void changelog_startup_free(changelog_startup_t *s);
/* formatStartupChangelogSummary (owned) */
char *changelog_startup_summary(const changelog_startup_t *s);

#endif
