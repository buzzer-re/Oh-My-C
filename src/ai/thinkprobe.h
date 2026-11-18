/* thinkprobe.h - thinking-capability probes for models whose reasoning support is unknown (omp
 * packages/ai/src/thinking-probe.ts) and the learned-capability cache (packages/catalog/src/thinking-capability.ts plus the
 * `thinking_capability` table of models.db, kept here as <agent dir>/thinking-capability.json).
 *
 * A chat model nothing classifies (a models.yml row without `reasoning`, a discovered local model) gets a provisional
 * ladder: `thinking: {mode, efforts: all six, unverified: true}`. Before its first request with a thinking level, parallel
 * probes ("What is 17 times 23? ...", 1024 output tokens, 45 s, one HTTP attempt each) try every effort in the API's
 * request formats plus one reasoning-off control; the verdict (format, accepted efforts, observed reasoning) is cached per
 * provider, API, credential-free endpoint and wire model id for 30 days (1 day when nothing but the control worked) and
 * overlaid on the model. */
#ifndef OMC_AI_THINKPROBE_H
#define OMC_AI_THINKPROBE_H

#include "ai/ai.h"

/* the model carries a provisional (unverified) thinking ladder */
bool tp_unverified(const json_t *raw);

/* withProbedThinking: stream through the probed (or cached) capability of an unverified model with a requested effort */
json_t *tp_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb, void *ud);

/* withThinkingCapability for a model row: the row with its fresh cached capability applied (owned, resolved), or NULL
 * when the row is not unverified or nothing fresh is cached */
json_t *tp_learned_row(const json_t *raw);

/* ---------- exposed for tests ---------- */

/* isCapability */
bool tp_capability_valid(const json_t *cap);
/* the cache key's endpoint: baseUrl without userinfo, query, fragment or trailing slashes (owned; NULL when unparsable) */
char *tp_endpoint(const char *base_url);
/* readThinkingCapability: fresh, validated capability for this row (owned) or NULL */
json_t *tp_read(const json_t *raw);
/* writeThinkingCapability: validated capabilities only; storage failures are ignored */
void tp_write(const json_t *raw, const json_t *cap);
/* applyThinkingCapability over a row (owned, resolved) or NULL when not applicable */
json_t *tp_apply_raw(const json_t *raw, const json_t *cap);

#endif
