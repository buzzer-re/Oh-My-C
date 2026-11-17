/*
 * search.h - ripgrep-like content search on top of regex + walker (omp `grep` native).
 *
 * Results are flat, ordered lines (match lines and context lines) per file, files sorted by path,
 * so callers can format them like upstream. Directory searches run in parallel; output ordering is
 * deterministic. In directory walks, files containing NUL bytes are treated as binary and skipped (unless
 * search_binary), only probed for a match; a single named file is searched as text.
 */
#ifndef OMC_TEXT_SEARCH_H
#define OMC_TEXT_SEARCH_H

#include "core/core.h"
#include "core/json.h"
#include "text/regex.h"

typedef enum {
    SEARCH_CONTENT = 0, /* matched lines (+ context) */
    SEARCH_COUNT,       /* one entry per file with the number of matching lines */
    SEARCH_FILES,       /* one entry per file with at least one match */
} search_mode_t;

typedef struct {
    const char *pattern;
    const char *path;     /* file or directory; NULL = "." */
    const char *glob;     /* include filter (omp semantics: `*.ts` matches at any depth; a matched dir selects its files) */
    bool glob_anchored;   /* glob is relative to path (omp `recursive: false`): `*.ts` matches direct children only */
    const char *const *exclude; /* globs to leave out: no '/' = any depth, leading '/' = anchored to path */
    size_t nexclude;
    const char *type;     /* file type filter ("ts", "py", "c", "rust", ...; unknown = extension) */
    bool ignore_case;
    bool smart_case;      /* case-insensitive unless the pattern has an uppercase literal */
    bool fixed_strings;
    bool word;            /* match whole words only */
    bool multiline;       /* allow matches across lines (auto-enabled if the pattern contains \n) */
    bool hidden;          /* include hidden files */
    bool no_ignore;       /* don't respect .gitignore/.ignore */
    bool follow_symlinks;
    bool search_binary;   /* don't skip files containing NUL */
    int context_before, context_after;
    int max_count;          /* global limit on matches (files in SEARCH_FILES mode); 0 = none */
    int max_count_per_file; /* 0 = none */
    int offset;             /* skip the first N matches */
    int max_columns;        /* truncate lines longer than this many bytes; 0 = never */
    search_mode_t mode;
    int threads;            /* 0 = auto */
    size_t max_file_size;   /* directory walks skip larger files; 0 = 4 MiB */
} search_opts_t;

typedef struct {
    char *path;        /* relative to the searched dir, or the path as given for a single file */
    uint32_t line;     /* 1-based; 0 for count / files entries */
    uint32_t column;   /* 1-based character column of the first match on the line (0 for context) */
    char *text;        /* line text, trailing whitespace trimmed, valid UTF-8, maybe truncated;
                          multiline matches contain '\n'. NULL for count/files entries */
    bool is_context;
    bool truncated;
    uint32_t count;    /* SEARCH_COUNT: matching lines in the file */
    uint32_t block;    /* ordinal of the match this line belongs to within its file */
} search_hit_t;

typedef struct {
    VEC(search_hit_t) hits;
    uint32_t total_matches;      /* matching lines (content/count) or matched files (files mode) */
    uint32_t files_with_matches;
    uint32_t files_searched;
    uint32_t skipped_oversized;
    strvec_t binary_matches;     /* searched-root-relative paths, in path order, of files skipped as binary although
                                    their content matches (an explicitly named file is searched as text) */
    bool limit_reached;
    char *error;                 /* set when search_run returns -1 */
} search_result_t;

int search_run(const search_opts_t *opts, search_result_t *out); /* 0 ok, -1 error (out->error) */
/* Search an in-memory buffer with a compiled regex (path fields are NULL). */
void search_buffer(const rx_t *rx, const char *buf, size_t len, const search_opts_t *opts, search_result_t *out);
void search_result_free(search_result_t *r);
/* {matches:[{path,lineNumber,line,column,truncated,isContext,matchCount}],totalMatches,...} */
json_t *search_result_to_json(const search_result_t *r);

/* Compile a pattern the way search_run does (smart case, fixed strings, word, line mode). */
rx_t *search_compile(const search_opts_t *opts, char **err);
/* File type filter check on a path's basename. */
bool search_type_matches(const char *type, const char *path);

#endif
