/*
 * glob.h - glob matching (`*`, `**`, `?`, `[...]`, `{a,b}`) and gitignore rule sets.
 *
 * Glob semantics (globset-like with literal_separator when GLOB_PATHNAME is set):
 *   `*`  any run of characters except '/'      `?` one character except '/'
 *   `**` as a whole path segment: zero or more segments (trailing `**` needs at least one segment)
 *   `[abc]` `[a-z]` `[!x]` `[^x]` classes, `\x` escapes, `{a,b,{c,d}}` brace alternation.
 * Without GLOB_PATHNAME, `*`/`?` also match '/'.
 */
#ifndef OMC_TEXT_GLOB_H
#define OMC_TEXT_GLOB_H

#include "core/core.h"

enum {
    GLOB_PATHNAME = 1 << 0, /* '/' is only matched literally (default for path globs) */
    GLOB_ICASE = 1 << 1,    /* ASCII case-insensitive */
    GLOB_NOBRACE = 1 << 2,  /* treat { } literally */
};

typedef struct glob_pat glob_pat_t;

glob_pat_t *glob_compile(const char *pattern, int flags);
bool glob_pat_match(const glob_pat_t *g, const char *path);
void glob_pat_free(glob_pat_t *g);
/* One-shot convenience (compiles each call). */
bool glob_match(const char *pattern, const char *path, int flags);
/* True if the pattern contains glob metacharacters (* ? [ {). */
bool glob_has_magic(const char *pattern);

/* omp `build_glob_pattern`: '\\' -> '/', prepend `**` + '/' to simple patterns when `recursive`
 * (no '/', not starting with `**`), close unclosed '{'. Owned. */
char *glob_normalize(const char *glob, bool recursive);
/* Max path depth (components) a normalized pattern can match; -1 = unbounded (`**` or braces). */
int glob_depth_bound(const char *pattern);

/* A list of globs; matches if any matches. */
typedef struct {
    VEC(glob_pat_t *) pats;
} globset_t;
void globset_add(globset_t *s, const char *pattern, int flags);
bool globset_match(const globset_t *s, const char *path);
void globset_free(globset_t *s);

/* ---------------- gitignore ---------------- */
typedef struct ignore_set ignore_set_t;

typedef enum { IGNORE_NONE = 0, IGNORE_IGNORED = 1, IGNORE_WHITELISTED = -1 } ignore_result_t;

ignore_set_t *ignore_set_new(void);
void ignore_set_free(ignore_set_t *s);
/* Add one gitignore line (comments/blank lines ignored). */
void ignore_set_add_line(ignore_set_t *s, const char *line, size_t len);
/* Add all lines of a buffer / file. add_file returns false if the file can't be read. */
void ignore_set_add_text(ignore_set_t *s, const char *text, size_t len);
bool ignore_set_add_file(ignore_set_t *s, const char *path);
size_t ignore_set_count(const ignore_set_t *s);
/* Match a path relative to the directory holding the rules ('/'-separated, no leading "./").
 * Last matching rule wins. */
ignore_result_t ignore_set_match(const ignore_set_t *s, const char *relpath, bool is_dir);
/* Like ignore_set_match, but a path is also ignored when one of its parent directories is. */
ignore_result_t ignore_set_match_parents(const ignore_set_t *s, const char *relpath, bool is_dir);

#endif
