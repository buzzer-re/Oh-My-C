/* readfmt.h - structured readers for the read tool: archives (zip, tar, tar.gz, gz), SQLite databases
 * (read-only on-disk format parser), PDF text extraction and Jupyter notebooks.
 * Everything here parses untrusted bytes: all lengths are bounds-checked and recursion is capped.
 * Depends only on src/core and zlib so it can be unit-tested standalone. */
#ifndef OMC_READFMT_H
#define OMC_READFMT_H

#include "core/core.h"
#include "core/json.h"

/* "x.zip:inner/path" or "x.db:table?limit=5" split at a recognized extension. */
typedef struct {
    char *container; /* path up to and including the extension (owned) */
    char *sub;       /* remainder with leading ':' stripped (owned; may be "") */
    char *query;     /* sqlite only: text after '?' (owned; "" when absent) */
} rf_split_t;
typedef VEC(rf_split_t) rf_splits_t;

void rf_splits_free(rf_splits_t *s);
/* every plausible split, longest container prefix first (upstream parseArchivePathCandidates) */
void rf_archive_candidates(const char *path, rf_splits_t *out);
/* upstream parseSqlitePathCandidates: .sqlite/.sqlite3/.db/.db3 followed by ':', '?' or end */
void rf_sqlite_candidates(const char *path, rf_splits_t *out);

/* "123B" / "1.5KB" / "2.0MB" / "1.0GB" (upstream formatBytes); owned */
char *rf_fmt_bytes(int64_t n);
/* true when bytes decode as strict UTF-8 without NUL (upstream decodeUtf8Text) */
bool rf_is_text(const char *data, size_t len);

/* ---------- archives ---------- */
typedef struct rf_archive rf_archive_t;
typedef struct {
    char *name; /* child name relative to the listed directory (owned) */
    bool is_dir;
    int64_t size;
} rf_dirent_t;
typedef VEC(rf_dirent_t) rf_dirents_t;

rf_archive_t *rf_archive_open(const char *abs_path, char **err);
void rf_archive_free(rf_archive_t *a);
const char *rf_archive_format(const rf_archive_t *a);
/* 0 = absent (or escapes the root), 1 = file, 2 = directory */
int rf_archive_stat(rf_archive_t *a, const char *sub, int64_t *size);
/* children of a directory sorted case-insensitively; returns 0 or -1 with *err */
int rf_archive_list(rf_archive_t *a, const char *sub, rf_dirents_t *out, char **err);
void rf_dirents_free(rf_dirents_t *d);
/* "name/" or "name (size)" lines (upstream formatArchiveEntryLines); owned */
char *rf_format_dirents(const rf_dirent_t *v, size_t n);
/* extract one member's bytes (owned, NUL-terminated); *norm_path gets the normalized member path */
char *rf_archive_read(rf_archive_t *a, const char *sub, size_t *len, char **norm_path, char **err);

/* ---------- SQLite ---------- */
bool rf_sqlite_sniff(const char *abs_path);
/* run an upstream-style selector against a database: sub = "" | "table" | "table:key",
 * query = URL query string ("limit=..&offset=..&order=..&where=.." or "q=SELECT ...").
 * Returns owned output text; *is_error is set when the text is an error message. */
char *rf_sqlite_read(const char *abs_path, const char *sub, const char *query, bool *is_error);

/* every row of `table` as an array of {column: value} objects (TEXT/INTEGER/REAL; BLOBs become null).
 * Reads the on-disk file plus its -wal like the reader above. Returns 0, or -1 with *err (owned). */
int rf_sqlite_table_json(const char *abs_path, const char *table, json_t **rows, char **err);

/* ---------- PDF ---------- */
/* extract per-page text; returns 0 and fills pages (one string per page) or -1 with *err */
int rf_pdf_extract(const char *data, size_t len, strvec_t *pages, char **err);
/* render pages [first,last] (1-based, inclusive; 0,0 = all) as text with page markers and the
 * incomplete-extraction notice; NULL with *err when nothing could be extracted */
char *rf_pdf_render(const strvec_t *pages, int first, int last, char **err);

/* ---------- Jupyter notebooks ---------- */
/* upstream notebookToEditableText: "# %% [type] cell:N" markers + cell sources; owned or NULL + *err */
char *rf_ipynb_text(const char *json, size_t len, const char *display, char **err);

#endif
