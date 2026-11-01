/* agents.h - process-wide agent registry (port of omp's AgentRegistry + IrcBus subset): the top-level session
 * ("Main") and every subagent the task tool spawned, their published outputs (agent://), transcripts
 * (history://), background task jobs, and peer messaging (`write agent://<id>`). Subagents stay alive and idle
 * after their run so they can be messaged; an idle agent wakes for a message and its answer reaches the sender. */
#ifndef OMC_AGENTS_H
#define OMC_AGENTS_H

#include "core/json.h"
#include "tools/tools.h"

#define MAIN_AGENT_ID "Main"

struct app;

void agents_register_main(struct app *a); /* top-level session */
/* session teardown: stops routing to `a`; after its jobs are joined call agents_release_children(a) */
void agents_unregister(struct app *a);
void agents_release_children(struct app *a); /* frees idle subagent sessions `a` spawned */
const char *agents_id_of(const struct app *a); /* registry id (borrowed) or NULL */

/* ---------- task cards ---------- */
/* Live omp TaskToolDetails snapshot of one task call ({results, progress[], async:{state,jobId,type}, ...}):
 * its async spawns update their progress rows (status, current tool, tool/request counts, context) and every
 * change is pushed to env->ui.tool_frame for the call's transcript card; the last frame is the settled one
 * (async.state completed/failed). Refcounted: the task tool and each spawn hold a reference. */
typedef struct task_card task_card_t;
task_card_t *task_card_new(tool_env_t *env, const char *tool_call_id, json_t *details /* owned */);
void task_card_release(task_card_t *c);
json_t *task_card_snapshot(task_card_t *c); /* owned clone */
void task_card_set_id(task_card_t *c, int index, const char *agent_id, bool primary); /* allocated agent id */
void task_card_settle(task_card_t *c, int index, const char *status); /* a spawn ended (or failed to start) */

/* ---------- task spawns ---------- */
typedef struct {
    const char *agent;          /* definition name */
    const char *name;           /* requested id (CamelCase) or NULL */
    const char *task;
    const char *context;
    const char *source_session; /* seance */
    const json_t *model;        /* string or [string] override (seance) */
    task_card_t *card;          /* progress card this spawn reports into (NULL: none) */
    int card_index;
} spawn_spec_t;

/* async spawn: registers the agent and starts a "task" job in env->jobs; returns 0 and the owned agent/job ids */
int agents_spawn(tool_env_t *env, const spawn_spec_t *spec, char **agent_id, char **job_id, char **err);
/* blocking spawn (async disabled): runs to completion, returns the rendered task result (owned) */
char *agents_run_blocking(tool_env_t *env, const spawn_spec_t *spec, volatile sig_atomic_t *cancel, bool *failed);

/* ---------- IRC ---------- */
/* `write agent://<to>` from the agent owning env: returns the receipt text (owned) and sets *is_error */
char *agents_send(tool_env_t *env, const char *to, const char *body, bool *is_error);
/* wait tool: next message queued for the agent owning env (owned "[id] from: body"), or NULL */
char *agents_take_message(tool_env_t *env);
void agents_wait_enter(tool_env_t *env, bool on); /* while waiting, incoming messages queue for the wait tool */
/* reply status for the wait tool: 0 nobody owes a reply, 1 a messaged agent is still working, 2 they stopped
 * without replying (*who owned, `a`, `b` list) */
int agents_reply_state(tool_env_t *env, char **who);
/* messages queued for a session that was idle without a wake hook (print mode); owned rendered text or NULL */
char *agents_take_pending_for(struct app *a);
bool agents_has_pending(struct app *a);

/* ---------- reads ---------- */
char *agents_read_output(const char *id_and_path, char **err); /* agent://<id>[/json/path] */
char *agents_history(const char *id, char **err);              /* history://[<id>] */
/* running subagents for proc:// listings: [{id, agent, ageMs, activity}] (owned) */
json_t *agents_running(void);
bool agents_cancel(const char *id); /* proc://<id>/kill for an agent */

#endif
