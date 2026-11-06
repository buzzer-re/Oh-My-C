/* costgate.h - session spend limits and the run deadline (port of omp session/cost-gate.ts and the agent loop's
 * `--max-time` deadline): `session.warnCost` warns once, `session.maxCost` refuses the next model request, the
 * deadline ends runs that would start a request past it. */
#ifndef OMC_COSTGATE_H
#define OMC_COSTGATE_H

#include "coding/app.h"

/* install the gate on a top-level app (seeded with the session's recorded spend) */
void costgate_attach(app_t *a);
/* a subagent passes the tree's gate before each of its requests */
void costgate_attach_subagent(app_t *child);
/* a /new or a switch to another saved session: the spend starts from that session's recorded spend */
void costgate_reseed(app_t *a);
/* a's spend grew: its total, and the tree's total when a is a subagent */
void costgate_add(app_t *a, double cost);
/* an inline subagent's total joins its parent's (its spend already reached the gate as it happened) */
void costgate_merge_child(app_t *parent, double child_total);
/* `--max-time`: wall-clock deadline in epoch ms (0 = none) */
void app_set_deadline(app_t *a, int64_t deadline_ms);
/* a model request was refused by the cost cap since the last costgate_take_refusal (print mode exits 3) */
bool costgate_take_refusal(app_t *a);
/* costGate.admit for a side request: false once the session's spend reached session.maxCost */
bool costgate_admits(void);
/* costGate.record for a background request whose cost the session's totals do not show */
void costgate_record(double cost);

#endif
