/* evalhost.h - host side of the eval kernel helpers that reach the session (ports of coding-agent/src/eval/
 * completion-bridge.ts, handle-bridge.ts, judgment-bridge.ts, judgment-batch-bridge.ts, budget-bridge.ts):
 *   __completion__  {prompt, model?: default|smol|slow, system?, schema?}  -> {id}   (background one-shot completion)
 *   __wait__        {items:[{kind,id}], timeoutMs?}                        -> {items:[snapshot]}
 *   __status__ / __cancel__ {item:{kind,id}}                               -> snapshot / {cancelled}
 *   __judge__       {state, questions, images?}                            -> {answers, model}
 *   __judge_batch__ {op: create|attach|status|drain|results|failed|cancel|close, ...}
 *   __budget__      {}                                                     -> {total, spent, hard}
 *   __output__      {id}                                                   -> {content}  (agent:// output)
 *   __interrupt__   {cell}                                                 -> cancels completions started by the cell
 * Model requests share a process-wide ceiling of 32 in flight. Judgments use the `judge` role chain
 * (modelRoles.judge, then tiny -> smol -> default, then the session model) through keyword text prompts. */
#ifndef OMC_EVALHOST_H
#define OMC_EVALHOST_H

#include "core/json.h"

#include <signal.h>

struct app;
typedef struct evalhost evalhost_t;

evalhost_t *evalhost_new(struct app *a);
/* cancel every running completion / batch and refuse new work (call before the session's jobs are joined) */
void evalhost_shutdown(evalhost_t *h);
void evalhost_free(evalhost_t *h); /* joins completion threads */
/* one bridge request; cell = serial of the calling cell; cancel = this call's abort flag. Owned value, or NULL with
 * *err (owned). */
json_t *evalhost_call(evalhost_t *h, const char *name, const json_t *args, int64_t cell, volatile sig_atomic_t *cancel,
                      char **err);
/* One judgment through the judge role chain (text bridge or native judge): normalized questions
 * {id: {type, instructions[, criteria]}} about `state`. Returns owned answers keyed by id, or NULL with *err (owned).
 * usage (optional) accumulates [input tokens, output tokens, cost] of the answering completion. */
json_t *evalhost_judge(struct app *a, const json_t *state, const json_t *questions, volatile sig_atomic_t *cancel,
                       double usage[3], char **err);
/* evalhost_judge with `lead` (a model selector, may be NULL) tried before the judge role chain (JudgeDeps.leadModel) */
json_t *evalhost_judge_lead(struct app *a, const char *lead, const json_t *state, const json_t *questions,
                            volatile sig_atomic_t *cancel, double usage[3], char **err);

#endif
