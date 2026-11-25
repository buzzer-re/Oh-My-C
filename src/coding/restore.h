/* restore.h - `omc --restore`: go on with the turn a session's last process left unfinished (port of omp
 * session/exit-diagnostics.ts findUnfinishedTurn / createInterruptedToolResults /
 * createInterruptedTurnAbortMessage and AgentSession.restoreInterruptedTurn's preparation). */
#ifndef OMC_RESTORE_H
#define OMC_RESTORE_H

#include "coding/app.h"

#define INTERRUPTED_TURN_ERROR "Previous OMC process exited before completing the turn."

/* hasRestorableTurn on the current branch: the turn the last process left unfinished (a prompt or tool results the model
 * never answered, tool calls without results; a trailing eval snapshot after a finished answer does not count), or one
 * exit recovery already closed. A cut-off turn is closed first: "outcome unknown" results for its unresolved calls and
 * the interrupted-turn abort are appended to the session and the agent. True when an app_retry() should go on with it;
 * *branch (may be NULL) then receives the branch's entries as they were before (owned). */
bool app_restore_interrupted_turn(app_t *a, json_t **branch);
/* restoreTerminatedBashJobs, once the restored turn's retry is under way: a background bash job the branch shows running
 * with no recorded outcome, unknown to this process, is published as a failed job ("its outcome is unknown") so `wait`,
 * proc:// and the delivery see it instead of an id that never existed */
void app_restore_terminated_jobs(app_t *a, const json_t *branch);

/* pure parts (tests): hasRestorableTurn over root..leaf entries (assumeExit); the bash jobs a branch leaves running
 * without an outcome, [{id, label}] in first-seen order (owned) */
bool restore_branch_restorable(const json_t *entries);
json_t *restore_unsettled_bash_jobs(const json_t *branch);

#endif
