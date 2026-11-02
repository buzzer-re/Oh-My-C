/* restore.h - `omc --restore`: go on with the turn a session's last process left unfinished (port of omp
 * session/exit-diagnostics.ts findUnfinishedTurn / createInterruptedToolResults /
 * createInterruptedTurnAbortMessage and AgentSession.restoreInterruptedTurn's preparation). */
#ifndef OMC_RESTORE_H
#define OMC_RESTORE_H

#include "coding/app.h"

#define INTERRUPTED_TURN_ERROR "Previous OMC process exited before completing the turn."

/* When the session's tail is unfinished (a prompt or tool results the model never answered, tool calls without
 * results), append "outcome unknown" results for the unresolved calls plus the interrupted-turn abort record to
 * the session and the agent, and return true: an app_retry() then goes on with the turn. */
bool app_restore_interrupted_turn(app_t *a);

#endif
