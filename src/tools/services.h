/* services.h - named long-lived bash services (port of omp's launch/services): each service runs under a small
 * detached supervisor process that owns its PTY (or pipes), appends output to a log and forwards a stdin FIFO, so
 * services survive the session when set to `persist`/`detached`. Per-project registry under
 * <agent_dir>/services/<project-hash>/<name>/. Inspected and controlled through proc://. */
#ifndef OMC_SERVICES_H
#define OMC_SERVICES_H

#include "core/json.h"
#include "jobs.h"

typedef struct {
    const char *log;   /* readiness regex over the output (NULL = none) */
    int port;          /* readiness TCP port (0 = none) */
    const char *host;  /* default 127.0.0.1 */
    double timeout;    /* seconds, default 30 */
} service_ready_t;

/* Start (or replace) service `name` in project `project` (the session cwd); waits for readiness when `ready` is
 * given; `notify` (a regex, NULL: none) reports matching log lines to the owner's session while it runs (notify.h). Snapshot: {name,state,pid,startedAt,readyAt?,exitedAt?,exitCode?,persist,detached}. */
int services_start(jobs_t *owner, const char *project, const char *name, const char *command, const char *cwd, bool pty,
                   const service_ready_t *ready, const char *notify, volatile sig_atomic_t *cancel, json_t **snapshot,
                   bool *ready_timed_out, char **err);
json_t *services_list(const char *project);                   /* owned array of snapshots, by name */
json_t *services_find(const char *project, const char *name); /* owned snapshot or NULL */
char *services_status_line(const json_t *snap);               /* "<name> [service] — <state> — up 3s — pid N…" */
char *services_logs(const char *project, const char *name);   /* last 1000 rendered lines (owned) */
char *services_log_path(const char *project, const char *name);
int services_send(const char *project, const char *name, const char *content, json_t **snap, char **err);
int services_stop(jobs_t *owner, const char *project, const char *name, json_t **snap, char **err);
int services_mode(jobs_t *owner, const char *project, const char *name, const char *mode, json_t **snap, char **err);
bool services_state_terminal(const char *state);
void services_stop_owned(jobs_t *j); /* session end: stop owned non-persistent services */

#endif
