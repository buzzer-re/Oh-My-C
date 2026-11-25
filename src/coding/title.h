/* title.h - automatic session titles (fork side request with card titles; title-model fallback) */
#ifndef OMC_TITLE_H
#define OMC_TITLE_H

#include "coding/app.h"

typedef struct title_state title_state_t;

/* pure helpers (ports of omp tiny/text.ts and utils/title-card.ts) */
bool title_low_signal(const char *msg);                          /* greetings, acks, gibberish */
char *title_normalize(const char *value);                        /* normalizeGeneratedTitle: NULL when no usable title */
bool title_is_no_title(const char *value);                       /* isNoTitleSentinel: `none`, unquoted, one `.!?` dropped */
/* extractGeneratedTitle of the title model's reply text: 1 with *out (owned, not yet normalized), 0 for a clean decline
 * (a closed empty marker `<title/>` / `<title></title>` or the `none` sentinel), -1 for a reply that fails outright */
int title_extract_generated(const char *text, char **out);
/* generateTitleOnlineWithModels' verdict on one reply: TITLE_OK (*title owned, normalized), TITLE_DECLINED (the model
 * declined: ask no other model), TITLE_INVALID (unusable: the next candidate model is asked) */
enum { TITLE_OK, TITLE_DECLINED, TITLE_INVALID };
int title_judge_reply(const char *reply, char **title);
char *title_parse_card_reply(const char *reply, const char *icons); /* "🧪 FLAKY: Title" / plain / NULL */

/* runtime (app.c): note the operator message, start the side request when the reply begins,
 * apply a finished title on the agent thread */
void title_on_event(app_t *a, const agent_event_t *ev);
void title_apply(app_t *a);
void title_wait(app_t *a, int64_t timeout_ms);
void title_free(app_t *a);

#endif
