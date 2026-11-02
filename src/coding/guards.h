/* guards.h - default-on agent guards of omp's AgentSession:
 *   - cross-turn tool-call loop guard (pi-ai utils/tool-call-loop-guard.ts, session/stream-guards.ts LoopGuards,
 *     session/tool-call-loop-redirect.ts; model.toolCallLoopGuard.*)
 *   - todo tracker reminders (session/todo-tracker.ts: stop-time completion reminder + mid-run nudge;
 *     todo.reminders, todo.remindersMax)
 *   - empty / unexpected stop recovery (session/turn-recovery.ts handleEmptyAssistantStop /
 *     handleUnexpectedAssistantStop, session/unexpected-stop-classifier.ts; features.unexpectedStopDetection)
 * The stream-level thinking-loop guard lives in coding/thinkloop.c. */
#ifndef OMC_GUARDS_H
#define OMC_GUARDS_H

#include "agent/agent.h"

struct app;
typedef struct guards guards_t;

guards_t *guards_new(void);
void guards_free(guards_t *g);

/* a new prompt cycle (user or hidden prompt): todo reminder/nudge budgets and stop-retry counters restart */
void guards_reset_cycle(struct app *a);
/* print mode: stop-time todo reminders never start extra turns (omp disableStopTimeTodoReminders) */
void guards_disable_stop_reminders(struct app *a);
/* agent events: tool results feed the todo tracker; turn ends run the tool-call loop guard (redirect steered in) */
void guards_on_event(struct app *a, const agent_event_t *ev);
/* turn end after background-job delivery: the hidden mid-run todo nudge (steered in) */
void guards_mid_run_nudge(struct app *a, const agent_event_t *ev);
/* after a run settles (omp agent_end order: these first, then the plan-mode decision reminder, then the todo
 * reminder): the empty-stop / unexpected-stop retries. 1 = a hidden retry message was appended, agent_continue();
 * 2 = capped empty stop (terminal, no reminders); 0 = nothing */
int guards_stop_recovery(struct app *a);
/* the stop-time todo completion reminder; true when it was appended and the caller must agent_continue() */
bool guards_todo_reminder(struct app *a);

/* exposed for tests: todo-tracker isAwaitingUserAnswer over assistant text */
bool guards_awaiting_user_answer(const char *text);
/* exposed for tests: tool-call canonical hash / arguments summary (JSON.stringify of key-sorted args minus `i`) */
char *guards_canonical_args(const json_t *args);

#endif
