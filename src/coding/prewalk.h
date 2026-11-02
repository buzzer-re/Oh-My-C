/* prewalk.h - one-shot model handoff (port of omp session/prewalk.ts PrewalkCoordinator, the prewalk paths of
 * session/agent-session.ts and `/prewalk` of slash-commands/builtin-modes.ts).
 *
 * An armed prewalk keeps the session on its planning model, steers the hidden `prewalk-plan` nudge (plan, then a
 * 5-9 item todo list), and at the end of the first turn whose tool results include a direct edit/write (once a
 * todo call succeeded, when the todo tool is active) switches session-only to the target model (model_change role
 * "prewalk") and steers the `prewalk-checklist` reminder. `prewalk.enabled` arms the `prewalk.into` target for
 * fresh top-level sessions (startup, /new, toggling the setting); `--prewalk[-into]` / `--no-prewalk` override it.
 * prewalk.auto (the judge-role classifier) is not ported. */
#ifndef OMC_PREWALK_H
#define OMC_PREWALK_H

#include "agent/agent.h"

struct app;
typedef struct prewalk prewalk_t;

prewalk_t *prewalk_new(void);
void prewalk_free(prewalk_t *p);
/* unregister the prewalk.enabled listener and free a->prewalk */
void prewalk_close(struct app *a);

/* startup (top-level sessions): cli < 0 --no-prewalk, > 0 --prewalk / --prewalk-into (`into` overrides
 * prewalk.into), 0 the prewalk.enabled setting unless the session is being restored */
void prewalk_startup(struct app *a, int cli, const char *into, bool restoring);
/* the armed target (borrowed) or NULL */
const ai_model_t *prewalk_target(struct app *a);
/* PrewalkCoordinator.arm: false when a different prewalk is already armed or the target is a no-op */
bool prewalk_arm(struct app *a, const ai_model_t *target, const char *level);
/* disarmPrewalk: drops a pending handoff (notice when one was armed); true when one was armed */
bool prewalk_disarm(struct app *a);
/* restart: 0 armed, 1 reset (the target is a no-op on the restored model), 2 rejected (another arm active) */
int prewalk_restart(struct app *a, const ai_model_t *source, const char *source_level, const ai_model_t *target,
                    const char *target_level);
/* advanceAtTurnEnd (agent thread, AGENT_EV_TURN_END) */
void prewalk_on_turn_end(struct app *a, const agent_event_t *ev);
/* resetForNewSession (/new) */
void prewalk_new_session(struct app *a);
/* a later explicit model selection owns the active model */
void prewalk_release_handoff(struct app *a);
/* hidden plan nudges are consumed within the live run: never persisted */
bool prewalk_is_plan_nudge(const json_t *msg);

/* `/prewalk [into] | restart [from] [into] | off` (text output; *failed for usage errors) */
char *prewalk_command(struct app *a, const char *args, bool *failed);

#endif
