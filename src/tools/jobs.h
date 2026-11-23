/* jobs.h - async background jobs (port of omp's AsyncJobManager subset) shared by the bash, task and wait tools,
 * proc:// and the session runtime: explicit `async` bash jobs, auto-backgrounded foreground bash commands,
 * subagent task jobs and service watchers. Settled results are delivered to the owning agent as a follow-up
 * (or collected by the `wait` tool); delivered rows stay readable via proc:// for ~30s, others for ~5min. */
#ifndef OMC_JOBS_H
#define OMC_JOBS_H

#include "core/core.h"
#include "core/json.h"
#include "core/proc.h"

#include <pthread.h>
#include <signal.h>

typedef struct jobs jobs_t;
typedef struct job bgjob_t;
struct shell;

/* job body, runs on the job's own thread; returns the owned result text and sets *failed */
typedef char *(*job_fn)(bgjob_t *job, void *ud, bool *failed);

jobs_t *jobs_new(void);
void jobs_free(jobs_t *j); /* cancels and joins running jobs, stops owned session-mode services */
/* fn runs on the job's thread right after a (visible) job settles */
void jobs_set_notify(jobs_t *j, void (*fn)(void *ud), void *ud);

/* explicit `async: true` bash job (fresh shell); returns owned job id (bg_N) */
char *job_command_identity(const char *command);    /* commandIdentity (owned) */
char *job_escape_harness_tags(const char *text);    /* escapeHarnessTags (owned) */
struct toolcmd_run;
/* run: the call's tool commands (owned by the job, ended when its shell run ends; NULL: none) */
char *jobs_start_bash(jobs_t *j, const char *cmd, const char *cwd, int64_t timeout_ms, const char *label, const char *intent,
                      struct toolcmd_run *run);
/* generic job of `type` ("task", "service", ...). id_hint NULL => bg_N; a taken id gets a -N suffix.
 * agent_id: subagent behind a task job (NULL otherwise). hidden: never listed (service watchers).
 * ud_free (may be NULL) releases ud after the job is reaped. Returns the owned job id. */
char *jobs_start(jobs_t *j, const char *type, const char *id_hint, const char *label, const char *agent_id, bool hidden,
                 job_fn fn, void *ud, void (*ud_free)(void *));

/* the job's settlement is not delivered to the agent (still listed by proc://) */
void jobs_set_quiet(jobs_t *j, const char *id);

/* a caption being written for a job: shared with the writer's thread, which may outlive the job */
typedef struct job_caption {
    pthread_mutex_t mu;
    int refs;
    char *text; /* set once written */
    volatile sig_atomic_t cancel; /* the job is gone: the request is pointless */
} job_caption_t;
void job_caption_unref(job_caption_t *c);
/* JobWidgets.requestCaption: ask once per job for the caption of each running command that has run `after_ms`. The
 * command needs no caption or `busy()` says the writers are full: the former is decided for good, the latter retried
 * by a later listing. `start` takes a reference on the caption it keeps. */
void jobs_request_captions(jobs_t *j, int64_t after_ms, bool (*busy)(void), void (*start)(job_caption_t *c, const char *command, const char *intent, int64_t timeout_sec));

/* job-side helpers (call from the job body) */
const char *job_id(const bgjob_t *job);
volatile sig_atomic_t *job_cancel_flag(bgjob_t *job);
void job_set_output(bgjob_t *job, const char *text); /* live output/progress shown by proc://<id> */

/* ---------- foreground bash with auto-background ---------- */
/* Start `cmd` as an attached (unlisted) foreground job; output is forwarded to cb while attached. `shell` is the
 * session's persistent shell (NULL => fresh shell); a busy shell falls back to a temporary one. Owned handle id. */
char *jobs_start_bash_fg(jobs_t *j, const char *cmd, const char *cwd, int64_t timeout_ms, const char *intent, struct shell *shell,
                         proc_output_cb cb, void *cb_ud, struct toolcmd_run *run);
/* wait up to ms for an attached job; returns true once it finished. A set *cancel cancels the command. */
bool jobs_fg_wait(jobs_t *j, const char *id, int64_t ms, volatile sig_atomic_t *cancel);
/* take a finished attached job's raw result (removes the job); *notice (owned, may be NULL) explains a temporary shell */
bool jobs_fg_take(jobs_t *j, const char *id, proc_result_t *out, char **notice);
/* detach a running attached job into a regular background job; returns its owned bg_N id (NULL if the job already
 * finished — then take it). Output forwarding stops before this returns. */
char *jobs_fg_detach(jobs_t *j, const char *id);

/* ---------- queries ---------- */
size_t jobs_running(jobs_t *j);     /* running visible jobs (service watchers excluded) */
size_t jobs_running_all(jobs_t *j); /* running jobs incl. service watchers */
/* running jobs a `wait` watches: visible and delivering a result (a peer's wake turn is not one) */
size_t jobs_running_watchable(jobs_t *j);
size_t jobs_undelivered(jobs_t *j);
/* settled, not yet delivered jobs as [{jobId,label,type,result,failed,durationMs,agentId?}]; marks them delivered */
json_t *jobs_take_finished(jobs_t *j);
/* settledJobIds: ids of the jobs that have already settled (a call's entry state) */
strvec_t jobs_settled_ids(jobs_t *j);
/* block until a job settles (returns undelivered count), nothing runs, cancel, or max_ms; service watchers count
 * only with include_services */
int jobs_wait_any(jobs_t *j, volatile sig_atomic_t *cancel, int64_t max_ms);
int jobs_wait_any_ex(jobs_t *j, volatile sig_atomic_t *cancel, int64_t max_ms, bool include_services);
bool jobs_cancel(jobs_t *j, const char *id); /* NULL = all visible jobs */
/* [{id,type,label,status,durationMs,startedAt,endedAt?,agentId?,quiet?}] of listed jobs (owned) */
json_t *jobs_snapshot(jobs_t *j);
/* one listed job: snapshot fields + result/output; NULL if unknown (owned) */
json_t *jobs_get(jobs_t *j, const char *id);
/* proc://<id>/streams: n < 0 lists the job's pipe streams, else reads stream n; owned text or NULL + *err */
char *jobs_streams(jobs_t *j, const char *id, int n, char **err);

/* services this job manager started (stopped on jobs_free unless persistent/detached) */
void jobs_own_service(jobs_t *j, const char *name);
bool jobs_owns_service(jobs_t *j, const char *name);
/* mark a service watcher's next settlement as expected (explicit stop): it is not delivered */
void jobs_service_stopping(jobs_t *j, const char *name);

/* rewrite top-level pipes of a shell command to tap each pipe into fifo `<dir>/<n>`; owned or NULL (no taps);
 * producers/consumers receive the stage texts. Exposed for tests. */
char *pipe_tap_rewrite(const char *cmd, const char *dir, strvec_t *producers, strvec_t *consumers);

#endif
