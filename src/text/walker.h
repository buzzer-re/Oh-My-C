/*
 * walker.h - fast parallel recursive directory walker (ripgrep/ignore-crate style).
 *
 * Honors .gitignore (inside git repos), .ignore (always), .git/info/exclude and the global
 * core.excludesFile, including ignore files in ancestor directories up to the repo root (rules
 * that would exclude the walk root itself are dropped, so explicitly walking an ignored dir works).
 * Always skips `.git`. Hidden entries (leading '.') skipped unless include_hidden.
 */
#ifndef OMC_TEXT_WALKER_H
#define OMC_TEXT_WALKER_H

#include "core/core.h"
#include "text/glob.h"

enum { WALK_FILE = 1, WALK_DIR = 2, WALK_SYMLINK = 4 };

typedef struct {
    bool include_hidden;
    bool no_ignore;         /* don't read ignore files */
    bool follow_symlinks;   /* follow links (dir loops / repeats prevented by dev+inode set) */
    bool skip_node_modules; /* prune node_modules directories */
    int max_depth;          /* 0 = unlimited; 1 = root's direct children only */
    int types;              /* mask of WALK_* to report; 0 = WALK_FILE. Unfollowed links report as WALK_SYMLINK */
    const globset_t *include; /* if set: report only entries whose relative path matches */
    const globset_t *exclude; /* matching entries are skipped; matching dirs are pruned */
    int threads;            /* 0 = auto */
    bool want_stat;         /* fill size / mtime_ms */
} walk_opts_t;

typedef struct {
    const char *path; /* relative to root, '/'-separated (for a file root: its basename) */
    const char *abs;  /* root joined with path */
    int type;         /* WALK_FILE, WALK_DIR or WALK_SYMLINK */
    bool is_link;     /* entry is a symlink (type is the target's type when following) */
    int depth;        /* 1 = direct child of root */
    int64_t size, mtime_ms; /* -1 unless want_stat */
} walk_entry_t;

/* Called concurrently from worker threads. Return false to stop the walk. */
typedef bool (*walk_cb_t)(const walk_entry_t *e, void *ud);

/* Returns 0, or -errno if the root can't be accessed. */
int walk(const char *root, const walk_opts_t *opts, walk_cb_t cb, void *ud);

typedef struct {
    char *path; /* relative */
    int type;
    bool is_link;
    int depth;
    int64_t size, mtime_ms;
} walk_item_t;
typedef VEC(walk_item_t) walk_list_t;

/* Collect all reported entries sorted by path (component-wise). Returns 0 or -errno. */
int walk_collect(const char *root, const walk_opts_t *opts, walk_list_t *out);
void walk_list_free(walk_list_t *l);
/* Component-wise path comparison ("a/b" < "a.c"), suitable for qsort on strings. */
int walk_path_cmp(const char *a, const char *b);

#endif
