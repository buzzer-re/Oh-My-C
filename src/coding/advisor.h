/* advisor.h - shadow reviewer model that injects <advisory> notes into the primary agent */
#ifndef OMC_ADVISOR_H
#define OMC_ADVISOR_H

#include "coding/app.h"

typedef struct advisor advisor_t;

advisor_t *advisor_new(app_t *a); /* starts the review thread */
void advisor_free(advisor_t *ad);
void advisor_on_event(advisor_t *ad, const agent_event_t *ev); /* primary agent events */
/* after a primary run: wait for the final review; returns an advisory batch (owned) that should start a
 * continuation turn, or NULL. Lower-severity notes are surfaced via the app notice callback. */
char *advisor_after_run(advisor_t *ad, bool *is_continuation);
void advisor_reset(advisor_t *ad); /* context replaced (new session, branch) */
const ai_model_t *advisor_role_model(void); /* resolveAdvisorRoleSelection (`@advisor`), NULL = no model */
/* AdvisorStats for /advisor status: {model?:{provider,id}, contextTokens, contextWindow,
 * messages:{user,assistant,total}, tokens:{input,output,cacheRead,cacheWrite}, cost} (owned) */
json_t *advisor_stats(advisor_t *ad);

#endif
