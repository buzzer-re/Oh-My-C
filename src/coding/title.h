/* title.h - automatic session titles (fork side request with card titles; title-model fallback) */
#ifndef OMC_TITLE_H
#define OMC_TITLE_H

#include "coding/app.h"

typedef struct title_state title_state_t;

/* pure helpers (ports of omp tiny/text.ts and utils/title-card.ts) */
bool title_low_signal(const char *msg);                          /* greetings, acks, gibberish */
char *title_normalize(const char *value);                        /* NULL when no usable title */
char *title_parse_card_reply(const char *reply, const char *icons); /* "🧪 FLAKY: Title" / plain / NULL */

/* runtime (app.c): note the operator message, start the side request when the reply begins,
 * apply a finished title on the agent thread */
void title_on_event(app_t *a, const agent_event_t *ev);
void title_apply(app_t *a);
void title_wait(app_t *a, int64_t timeout_ms);
void title_free(app_t *a);

#endif
