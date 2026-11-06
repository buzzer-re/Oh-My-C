/* judgment_native.h - native judgment APIs: catalog judge models (typesafe, openrouter-decisions,
 * openai-decisions, workers-ai-systemone) that answer typed questions directly. */
#ifndef OMC_JUDGMENT_NATIVE_H
#define OMC_JUDGMENT_NATIVE_H

#include "ai.h"

bool ai_is_judgment_api(const char *api);
/* Answer `questions` ({id: {type, instructions, criteria}}) about `state` with model m and credential key; images
 * are {type:"image", data, mimeType[, detail]} blocks. Returns owned answers keyed by question id (wire answers
 * checked against each question's type), adds the billed cost to *cost and sets *model_out (owned
 * "provider/model"), or NULL with *err (owned). */
json_t *ai_native_judge(const ai_model_t *m, const char *key, const json_t *state, const json_t *questions,
                        const json_t *images, volatile sig_atomic_t *cancel, double *cost, int64_t tokens[2], char **model_out, char **err);

#endif
