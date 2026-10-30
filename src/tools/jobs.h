/* jobs.h - async background jobs shared by the bash tool, the wait tool and the session runtime */
#ifndef OMC_JOBS_H
#define OMC_JOBS_H

#include "core/core.h"
#include "core/json.h"

#include <signal.h>

typedef struct jobs jobs_t;

jobs_t *jobs_new(void);
void jobs_free(jobs_t *j); /* cancels and joins running jobs */
/* fn runs on the job's thread right after a job settles */
void jobs_set_notify(jobs_t *j, void (*fn)(void *ud), void *ud);
char *jobs_start_bash(jobs_t *j, const char *cmd, const char *cwd, int64_t timeout_ms, const char *label); /* owned id */
size_t jobs_running(jobs_t *j);
size_t jobs_undelivered(jobs_t *j);
/* settled, not yet delivered jobs as [{jobId,label,type,result,failed,durationMs}]; marks them delivered (owned) */
json_t *jobs_take_finished(jobs_t *j);
/* block until a job settles (returns undelivered count), nothing runs, cancel, or max_ms */
int jobs_wait_any(jobs_t *j, volatile sig_atomic_t *cancel, int64_t max_ms);
bool jobs_cancel(jobs_t *j, const char *id); /* NULL = all */
json_t *jobs_snapshot(jobs_t *j);            /* [{id,type,label,status,durationMs}] (owned) */

#endif
