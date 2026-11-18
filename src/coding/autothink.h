/* autothink.h - `auto` thinking level (port of omp auto-thinking/classifier.ts, tiny/message-preproc.ts and the
 * auto helpers of pi-tui thinking.ts / session/model-controls.ts).
 *
 * `auto` keeps a provisional concrete effort (the model's defaultLevel, else high, clamped below max) until the
 * first user turn; each user turn is then classified by the judge role chain into low..xhigh (max only with
 * providers.autoThinkingMaxEffort=max, or the `ultrathink` magic keyword), clamped to the model's ladder. */
#ifndef OMC_AUTOTHINK_H
#define OMC_AUTOTHINK_H

#include "ai/ai.h"

struct app;

/* the model's controllable effort ladder (omp getSupportedEfforts: reasoning ? thinking.efforts : []) */
size_t autothink_supported(const ai_model_t *m, ai_effort_t out[6]);
/* clampAutoThinkingEffort: never below low, snapped down into the pool at or under `ceiling`; false = undefined */
bool autothink_clamp(const ai_model_t *m, ai_effort_t effort, ai_effort_t ceiling, ai_effort_t *out);
/* resolveProvisionalAutoLevel; false for non-reasoning models or an empty pool */
bool autothink_provisional(const ai_model_t *m, ai_effort_t *out);
/* applyAutoThinkingLevel's classification: ultrathink (when enabled) -> max clamp, else the judge (4 s budget).
 * Returns true and the effort to use; false when the model has nothing to pick (caller keeps its level). */
bool autothink_resolve(struct app *a, const char *prompt, ai_effort_t *out);

/* preprocessTinyMessage: strip ANSI/fences/paired XML, shorten hashes, collapse whitespace, cap at 2000 (owned) */
char *tiny_preprocess(const char *message);

#endif
