/* costgate.h - session spend limits and the run deadline (port of omp session/cost-gate.ts and the agent loop's
 * `--max-time` deadline): `session.warnCost` warns once, `session.maxCost` refuses the next model request, the
 * deadline ends runs that would start a request past it. */
#ifndef OMC_COSTGATE_H
#define OMC_COSTGATE_H

#include "coding/app.h"

/* install the gate on a top-level app (seeded with the session's recorded spend) */
void costgate_attach(app_t *a);
/* `--max-time`: wall-clock deadline in epoch ms (0 = none) */
void app_set_deadline(app_t *a, int64_t deadline_ms);
/* a model request was refused by the cost cap since the last costgate_take_refusal (print mode exits 3) */
bool costgate_take_refusal(app_t *a);
/* costGate.admit for a side request: false once the session's spend reached session.maxCost */
bool costgate_admits(void);

#endif
