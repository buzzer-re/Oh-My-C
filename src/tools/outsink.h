/* outsink.h - streaming tool-output capture (port of omp's OutputSink, packages/tui/src/tools/streaming-output.ts)
 * plus the output-meta notice formatting (packages/tui/src/tools/output-meta.ts,
 * packages/coding-agent/src/tools/output-meta.ts) and sanitizeText (packages/utils/src/sanitize-text.ts).
 *
 * Everything here works on UTF-8 bytes; omp's UTF-16 code-unit measures that only gate fast paths are
 * replaced by their byte equivalents. Meta objects are json trees in omp's `details.meta` shape. */
#ifndef OMC_OUTSINK_H
#define OMC_OUTSINK_H

#include "core/core.h"
#include "core/json.h"

#define OUT_DEFAULT_MAX_BYTES (50 * 1024)
#define OUT_DEFAULT_MAX_LINES 3000
#define OUT_ARTIFACT_DEFAULT_MAX_BYTES (16 * 1024 * 1024)
#define OUT_ARTIFACT_DEFAULT_HEAD_BYTES (3 * 1024 * 1024)
#define OUT_INLINE_CAP_SLACK_BYTES (2 * 1024)

/* ---------- text helpers ---------- */
/* sanitizeText: repair UTF-8 (WHATWG replacement), strip ANSI escapes when the text holds an ESC, drop
 * C0 controls except \t \n, DEL and C1 controls. Line endings are left alone (CR is a control: dropped). */
char *out_sanitize(const char *s, size_t len, size_t *out_len);
/* sanitize + collapse CRLF / lone CR to \n (the OutputSink input normalization) */
char *out_sanitize_lines(const char *s, size_t len, size_t *out_len);
char *out_format_bytes(int64_t bytes);        /* "512B", "1.5KB", "2.3MB" */
char *out_format_grouped(int64_t n);          /* toLocaleString: "20,562" */
char *out_middle_marker(int64_t elided_lines, int64_t elided_bytes); /* "[…Nln elided…]" / "[…NB elided…]" */
size_t out_count_nl(const char *s, size_t len);

/* ---------- truncation (truncateHead / truncateTail / truncateMiddle) ---------- */
enum { OUT_BY_NONE = 0, OUT_BY_LINES, OUT_BY_BYTES, OUT_BY_MIDDLE };
typedef struct {
    char *content; /* owned */
    size_t content_len;
    bool truncated;
    int by;        /* OUT_BY_* */
    int64_t total_lines, total_bytes;
    int64_t output_lines, output_bytes; /* = totals when not truncated */
    int64_t elided_bytes, elided_lines; /* -1 = absent */
    int64_t head_lines, tail_lines;     /* -1 = absent */
    bool partial_byte_windows, last_line_partial, first_line_exceeds_limit;
} out_trunc_t;
void out_truncate_head(const char *s, size_t len, int64_t max_bytes, int64_t max_lines, out_trunc_t *r);
void out_truncate_tail(const char *s, size_t len, int64_t max_bytes, int64_t max_lines, out_trunc_t *r);
void out_truncate_middle(const char *s, size_t len, int64_t max_bytes, int64_t max_lines, int64_t max_head_bytes,
                         int64_t max_head_lines, out_trunc_t *r);
void out_trunc_free(out_trunc_t *r);
/* byte windows on UTF-8 boundaries: returns the kept length (head) or the start offset (tail) */
size_t out_head_bytes(const char *s, size_t len, size_t max);
size_t out_tail_start(const char *s, size_t len, size_t max);
/* final-defense inline cap (enforceInlineByteCap); artifact_id appends `[raw output: artifact://<id>]` (owned result) */
char *out_enforce_inline_cap(const char *text, size_t len, size_t max_bytes, const char *artifact_id, bool *capped);

/* ---------- OutputSink ---------- */
typedef struct out_sink out_sink_t;
typedef struct {
    const char *artifact_path; /* full raw stream mirrored here once inline capture loses bytes (NULL: none) */
    const char *artifact_id;
    size_t spill_threshold;    /* inline budget (default OUT_DEFAULT_MAX_BYTES when 0) */
    size_t head_bytes;         /* > 0: keep a head window and elide the middle */
    size_t max_columns;        /* > 0: per-line byte cap with `…` */
    int64_t artifact_max_bytes; /* < 0: default 16 MiB; 0: unbounded */
    int64_t artifact_head_bytes; /* < 0: default */
} out_sink_opts_t;

typedef struct {
    char *output; /* owned */
    size_t output_len;
    bool truncated;
    int64_t total_lines, total_bytes, output_lines, output_bytes;
    int64_t elided_bytes, elided_lines;   /* -1 = absent */
    int64_t column_dropped_bytes, column_truncated_lines, column_max; /* 0 = absent */
    int64_t column_first, column_last;    /* 0 = absent */
    char *artifact_id;                    /* owned, NULL when no artifact was written */
    int64_t artifact_elided_bytes;        /* 0 = absent */
    const char *artifact_error;           /* "open" | "write" | "flush" | NULL */
} out_summary_t;

out_sink_t *out_sink_new(const out_sink_opts_t *o);
/* raw bytes (any chunking): decoded as UTF-8, CR-normalized, sanitized, then stored */
void out_sink_push(out_sink_t *s, const char *data, size_t len);
/* already-clean text (sanitized again, idempotent) */
void out_sink_push_str(out_sink_t *s, const char *text);
char *out_sink_preview(out_sink_t *s);          /* current inline body (owned) */
size_t out_sink_total_bytes(const out_sink_t *s);
/* finalize: `notice` (may be NULL) is prepended as "[notice]\n" */
void out_sink_dump(out_sink_t *s, const char *notice, out_summary_t *sum);
void out_sink_free(out_sink_t *s);
void out_summary_free(out_summary_t *sum);

/* ---------- output meta (details.meta) ---------- */
/* OutputMetaBuilder.truncationFromSummary: merges into *meta (created when NULL) */
void out_meta_from_summary(json_t **meta, const out_summary_t *sum, const char *direction, int64_t start_line,
                           int64_t total_file_lines /* <= 0: none */);
/* OutputMetaBuilder.truncation(result, options) */
void out_meta_from_trunc(json_t **meta, const out_trunc_t *r, const char *direction, int64_t start_line,
                         int64_t total_file_lines, const char *artifact_id, int64_t max_bytes, int64_t next_offset);
/* limits: matchLimit / resultLimit(suggestion < 0: none) / headLimit / columnTruncated */
void out_meta_match_limit(json_t **meta, int64_t reached, int64_t suggestion);
void out_meta_result_limit(json_t **meta, int64_t reached, int64_t suggestion);
void out_meta_head_limit(json_t **meta, int64_t reached, int64_t suggestion);
void out_meta_column_truncated(json_t **meta, int64_t max_column, const char *unit, const char *artifact_id,
                               int64_t artifact_elided_bytes, int64_t first, int64_t last, const char *selector_base);
/* formatOutputNotice: "\n\n[...]" (+ LSP diagnostics block) or "" (owned) */
char *out_format_notice(const json_t *meta);
/* formatTruncationMetaNotice + artifact error, unbracketed (styled warning text); NULL when none */
char *out_format_warning(const json_t *meta);
/* stripOutputNotice: text without the trailing notice for `meta` (owned) */
char *out_strip_notice(const char *text, const json_t *meta);
/* appendOutputNotice on a content array (json) */
void out_append_notice(json_t *content, const json_t *meta);

/* bashExecution message -> LLM text (bashExecutionToText) */
char *out_bash_execution_text(const json_t *msg);

#endif
