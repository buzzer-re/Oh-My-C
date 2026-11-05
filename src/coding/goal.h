/* goal.h - goal mode: persistent per-session objective with budget accounting and auto-continuation */
#ifndef OMC_GOAL_H
#define OMC_GOAL_H

#include "coding/app.h"

typedef struct goal goal_t;

void goal_free(goal_t *g);
json_t *goal_json(const goal_t *g);       /* omp Goal shape or null (owned) */
json_t *goal_state_json(const goal_t *g); /* {enabled, mode, reason?, goal} or null (owned) */

const goal_t *app_goal(app_t *a); /* NULL when no goal */
const char *goal_status(const goal_t *g);
const char *goal_objective(const goal_t *g);
bool goal_enabled(const goal_t *g);
int64_t goal_used(const goal_t *g);
int64_t goal_budget(const goal_t *g);

int app_goal_create(app_t *a, const char *objective, int64_t budget, char **err);
int app_goal_replace(app_t *a, const char *objective, int64_t budget, char **err);
int app_goal_resume(app_t *a, char **err);
int app_goal_pause(app_t *a);
void app_goal_drop(app_t *a);
int app_goal_complete(app_t *a, char **err);
int app_goal_set_budget(app_t *a, int64_t budget, char **err); /* 0 = no budget */

/* runtime hooks (app.c) */
void app_goal_on_event(app_t *a, const agent_event_t *ev);
json_t *app_goal_context_message(app_t *a); /* hidden per-prompt context or NULL (owned) */
void app_goal_on_abort(app_t *a);           /* user interrupt pauses an active goal */
void app_goal_restore(app_t *a);            /* rebuild from the session's last mode_change */
bool app_goal_interview_active(app_t *a); /* /guided-goal asked its questions and no goal exists yet */
void app_goal_expose_tool(app_t *a);        /* /guided-goal: offer the goal tool before a goal exists */
/* after a run ends: returns the hidden continuation prompt (owned) when the goal should keep going.
 * mode is "interactive" / "rpc" / "print" (checked against goal.continuationModes). */
char *app_goal_after_run(app_t *a, const json_t *run_messages, bool was_continuation, const char *mode);

#endif
