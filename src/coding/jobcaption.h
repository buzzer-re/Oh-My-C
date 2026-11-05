/* jobcaption.h - job captions (port of omp job-widgets/caption.ts): one plain-English line on what a background command
 * does and when it ends, written by the `widget` role's model once per job from the command and the intent it was
 * started with; it never sees the job's output. */
#ifndef OMC_JOBCAPTION_H
#define OMC_JOBCAPTION_H

#include "coding/app.h"

/* asks for the captions of `a`'s running commands (`/jobs full`); they show on a later listing */
void job_captions_request(app_t *a);
/* finished caption requests as model_usage rows [{purpose, api, provider, model, usage, stopReason}] (owned) or NULL */
json_t *job_captions_take_usage(void);

#endif
