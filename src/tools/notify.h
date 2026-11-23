/* notify.h - mid-run reports of a background process's output (port of omp async/notify.ts and
 * session/async-notify.ts): the lines matching the `notify` regex a `bash` call gave.
 *
 * A notifier reads a process's output as it grows, groups matching lines that arrive within 200 ms of a batch's first
 * one into a batch (at most 50 lines of 400 characters, further matches counted), and hands each batch to its owner.
 * A process that matches too often loses its notices, not its run: once its allowance of 30 batches is spent the last
 * batch is marked `stopped`. A job's allowance refills one batch per 20 s; a service, which has no deadline, gets the
 * burst once per start. Batches queue on the owning jobs_t (jobs_queue_notify) and reach the session as one
 * `async-notify` custom message per boundary (notify_build_message). */
#ifndef OMC_NOTIFY_H
#define OMC_NOTIFY_H

#include "core/core.h"
#include "core/json.h"

#define NOTIFY_MAX_LINES 50
#define NOTIFY_BURST 30

typedef struct notifier notifier_t;

/* one batch: lines (owned array of strings), omitted count, stopped flag */
typedef void (*notify_emit_fn)(void *ud, json_t *lines, int64_t omitted, bool stopped);

/* compileNotifyPattern: false with *err (owned, "Invalid notify regex: …") when `source` does not compile */
bool notify_pattern_ok(const char *source, char **err);
/* refill: a process with a deadline gets its allowance back over time */
notifier_t *notifier_new(const char *source, bool refill, notify_emit_fn emit, void *ud);
/* feed output (any chunking); complete lines are matched, a partial one waits */
void notifier_feed(notifier_t *n, const char *data, size_t len);
/* the process exited: matches not sent yet go out now, the unterminated last line included */
void notifier_finish(notifier_t *n);
/* stop and free (pending matches dropped); safe after finish */
void notifier_free(notifier_t *n);

/* buildAsyncNotifyBatchMessage over queued entries [{id, type: "bash"|"service", command?, pattern, lines, omitted,
 * stopped, running}]: the merged notice text (owned) and its details {reports: [...]} (owned); NULL when none */
char *notify_build_message(const json_t *entries, json_t **details);

#endif
