/* fuzzy.h - edit-engine text normalization and fuzzy matching.
 * Port of omp's pi-edit text.rs + fuzzy.rs (replace / patch / apply_patch / sloppy matching ladders).
 * Byte offsets index UTF-8 text; character counts are Unicode scalar values like upstream. */
#ifndef OMC_FUZZY_H
#define OMC_FUZZY_H

#include "core/core.h"

#define FZ_DEFAULT_THRESHOLD 0.95
#define FZ_SEQUENCE_THRESHOLD 0.92
#define FZ_FALLBACK_THRESHOLD 0.8
#define FZ_CONTEXT_THRESHOLD 0.8
#define FZ_PARTIAL_MIN_LENGTH 6
#define FZ_PARTIAL_MIN_RATIO 0.3
#define FZ_MAX_RECORDED 5
#define FZ_DOMINANT_MIN 0.97
#define FZ_DOMINANT_DELTA 0.08
#define FZ_CHARACTER_THRESHOLD 0.92

/* ---------- text helpers (JS String semantics) ---------- */
bool fz_is_js_ws(uint32_t cp);
/* JS trim span of s[0..len): start/end byte offsets */
void fz_trim_span(const char *s, size_t len, size_t *start, size_t *end);
char *fz_trim(const char *s);                 /* owned trim() */
char *fz_trim_end(const char *s);             /* owned trimEnd() */
const char *fz_trim_start(const char *s);     /* borrowed trimStart() */
bool fz_trim_eq(const char *a, const char *b);
bool fz_blank(const char *s);                 /* trim() is empty */
size_t fz_leading_ws(const char *s);          /* count of leading ' '/'\t' bytes */
size_t fz_utf16_len(const char *s);
size_t fz_char_count(const char *s);
bool fz_is_ascii(const char *s);
char *fz_normalize_unicode(const char *s);    /* trim, fold typographic punctuation (no NFC tables) */
char *fz_normalize_for_fuzzy(const char *s);  /* trim, fold quotes/dashes, collapse spaces/tabs */
char *fz_normalize_to_lf(const char *s);
char *fz_convert_leading_tabs(const char *text, size_t spaces_per_tab);
char *fz_adjust_indentation(const char *old_text, const char *actual_text, const char *new_text);
double fz_similarity(const char *a, const char *b);
size_t fz_levenshtein(const uint32_t *a, size_t na, const uint32_t *b, size_t nb);
uint32_t *fz_decode(const char *s, size_t *n); /* owned code points */

/* JS split("\n") into owned lines (always >= 1 element) */
strvec_t fz_split(const char *s);

/* ---------- whole-text matching (find_match) ---------- */
typedef struct {
    char *actual_text;
    size_t start_index; /* byte offset */
    uint32_t start_line;
    double confidence;
} fz_match_t;

typedef struct {
    fz_match_t *matched;
    fz_match_t *closest;
    int64_t occurrences;   /* -1 = none */
    strvec_t occurrence_previews;
    int64_t fuzzy_matches; /* -1 = none */
    bool dominant_fuzzy;
} fz_outcome_t;

typedef struct {
    size_t start, end;
} fz_range_t;

void fz_find_match(const char *content, const char *target, bool allow_fuzzy, double threshold,
                   const fz_range_t *excluded, size_t nexcluded, fz_outcome_t *out);
void fz_outcome_free(fz_outcome_t *o);

/* ---------- line-sequence / context-line matching ---------- */
enum {
    SEQ_EXACT,
    SEQ_TRIM_TRAILING,
    SEQ_TRIM,
    SEQ_COMMENT_PREFIX,
    SEQ_UNICODE,
    SEQ_PREFIX,
    SEQ_SUBSTRING,
    SEQ_FUZZY,
    SEQ_FUZZY_DOMINANT,
    SEQ_CHARACTER
};
enum { CTX_EXACT, CTX_TRIM, CTX_UNICODE, CTX_PREFIX, CTX_SUBSTRING, CTX_FUZZY };

typedef struct {
    int64_t index;        /* -1 = none */
    double confidence;
    int64_t match_count;  /* -1 = none */
    bool has_indices;
    size_t nindices;
    size_t indices[FZ_MAX_RECORDED];
    int strategy;         /* -1 = none */
} fz_seq_t;

const char *fz_seq_label(int strategy);
const char *fz_ctx_label(int strategy);

fz_seq_t fz_seek_sequence(char *const *lines, size_t n, char *const *pattern, size_t m, size_t start, bool eof,
                          bool allow_fuzzy);
/* best placement regardless of threshold: *index (-1 none), *score */
void fz_closest_sequence(char *const *lines, size_t n, char *const *pattern, size_t m, size_t start, bool eof,
                         int64_t *index, double *score);
fz_seq_t fz_find_context_line(char *const *lines, size_t n, const char *context, size_t start_from, bool allow_fuzzy,
                              bool skip_function_fallback);

/* ---------- replace-mode helpers ---------- */
char *fz_format_match_error(const char *path, const char *search_text, const fz_match_t *closest, bool allow_fuzzy,
                            double threshold, int64_t fuzzy_matches);
char *fz_format_occurrence_error(const char *path, const fz_outcome_t *o);
/* replace_text: returns true and *out (owned) + *count when replaced; false with *miss filled otherwise.
 * *err (owned) set for invalid input. */
bool fz_replace_text(const char *content, const char *old_text, const char *new_text, bool fuzzy, bool all,
                     double threshold, char **out, size_t *count, fz_outcome_t *miss, char **err);

#endif
