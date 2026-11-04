/* record.h - session recordings: `/record` and `omc play` (port of omp stream/recording.ts, paint-encoder.ts,
 * protocol.ts, redactor.ts and player.ts, v18.14.2).
 *
 * A recording (`.ompcast`, JSON Lines) is the painted screen: line 1 the header
 * {"ompcast":1,"cols","rows","title","createdAt"}, then `[ms, frame]` events where frame is
 * {"t":"viewport","rows":[…]} | {"t":"patch","ops":[[i,"row"]…],"rows":N} | {"t":"history","rows":[…]} |
 * {"t":"resize","cols","rows"} | {"t":"reset"}. Rows are reduced to the stream-safe ANSI subset (SGR + OSC 8)
 * and redacted before they are written. */
#ifndef OMC_RECORD_H
#define OMC_RECORD_H

#include "core/core.h"
#include "core/json.h"

#define RECORD_VERSION 1
#define RECORD_EXTENSION ".ompcast"
#define RECORD_FLUSH_MS 50          /* STREAM_FLUSH_INTERVAL_MS */
#define RECORD_HISTORY_LIMIT 2000   /* STREAM_HISTORY_LIMIT */

/* normalizeStreamRow: keep printable text, SGR and OSC 8; OSC 66 keeps its payload; image rows become
 * "[image]" (owned) */
char *record_normalize_row(const char *row);

/* ---------- redaction (StreamRedactor) ---------- */
typedef struct record_redactor record_redactor_t;
/* builtin credential shapes + prefix introducers + `extra` regex sources (invalid ones skipped), plain secrets
 * from <cwd>/.omp/secrets.yml, <agent>/secrets.yml, secret-named env values and dotenv values (>= 8 chars) */
record_redactor_t *record_redactor_load(const char *cwd, const json_t *extra);
/* a redactor over explicit plain secrets only plus the builtin patterns (tests) */
record_redactor_t *record_redactor_new(const char *const *plain, size_t n);
void record_redactor_free(record_redactor_t *r);
/* redactRow: the row unchanged, or its plain text with each sensitive span replaced by "••••••" (owned) */
char *record_redact_row(record_redactor_t *r, const char *row);

/* ---------- encoder (StreamPaintEncoder) ---------- */
typedef struct record_encoder record_encoder_t;
typedef struct {
    const char *const *history; size_t nhistory; /* rows committed above the viewport by this paint */
    const char *const *viewport; size_t nviewport;
    bool reset, alt;
    int columns, rows;
} record_paint_t;
record_encoder_t *record_encoder_new(int columns, int rows, record_redactor_t *redactor); /* redactor borrowed */
void record_encoder_free(record_encoder_t *e);
void record_encoder_push(record_encoder_t *e, const record_paint_t *p);
/* drain: the frames since the last drain, in order (owned JSON array) */
json_t *record_encoder_drain(record_encoder_t *e);

/* ---------- files ---------- */
char *record_dir(void);                       /* $TMPDIR/omp-recordings (owned) */
char *record_new_path(const char *session_id); /* <dir>/<utc-stamp>-<id8>.ompcast (owned) */
char *record_latest(void);                    /* newest .ompcast in record_dir() or NULL (owned) */

/* parseRecording: {header, events:[[at, frame]…]} (owned) or NULL with *err (owned). A truncated final
 * line is ignored. */
json_t *record_parse(const char *text, size_t len, char **err);

/* the live recorder */
typedef struct record_writer record_writer_t;
record_writer_t *record_writer_open(const char *path, int columns, int rows, const char *title,
                                    record_redactor_t *redactor, char **err); /* takes the redactor */
void record_writer_paint(record_writer_t *w, const record_paint_t *p, int64_t now_ms);
void record_writer_flush(record_writer_t *w);
int64_t record_writer_elapsed(const record_writer_t *w, int64_t now_ms);
const char *record_writer_path(const record_writer_t *w);
void record_writer_close(record_writer_t *w); /* flushes pending frames, closes, frees */

/* playRecording on the controlling terminal: space pauses, q / Esc / Ctrl-C quit. speed > 0; idle_limit_ms
 * < 0 = none. Returns 0. */
int record_play(const json_t *recording, double speed, int64_t idle_limit_ms);

#endif
