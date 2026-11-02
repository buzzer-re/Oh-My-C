/* cachewarm.h - prompt cache warmer (port of omp session/cache-warmer.ts, `providers.cacheWarming`).
 *
 * The main loop's latest request is replayed with a one-token output budget at 90% of the model's declared
 * prompt-cache lifetime (at least ten seconds before expiry), only while the conversation still matches it and
 * only when the expected avoided cache-miss cost minus the refresh cost is at least $0.05. Warming runs in a
 * "streaming" phase while the agent run is active and an "idle" phase after it settles (mode "idle"), and stops on
 * context change, mode change, a refresh that misses the cache, or the 60 min streaming / 30 min idle windows.
 * A model is warmed only when its catalog entry declares `promptCache` lifetimes for the retention tier used. */
#ifndef OMC_CACHEWARM_H
#define OMC_CACHEWARM_H

#include "ai/ai.h"

typedef struct cache_warmer cache_warmer_t;
typedef struct app app_t;

cache_warmer_t *cachewarm_new(app_t *a); /* starts the timer thread */
void cachewarm_stop(cache_warmer_t *w);  /* stops and joins the thread (pending usage is kept) */
void cachewarm_free(cache_warmer_t *w);  /* stops, joins and frees */

/* start(): keep the entry written by this main-loop request warm (replaces any previous run) */
void cachewarm_start(cache_warmer_t *w, const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o);
void cachewarm_settled(cache_warmer_t *w); /* onAgentSettled */
void cachewarm_mode_changed(cache_warmer_t *w); /* onModeChanged */
void cachewarm_cancel(cache_warmer_t *w);

/* paid warm responses not yet recorded: [{purpose, api, provider, model, usage, stopReason}] (owned) or NULL;
 * the session records them as model_usage entries from its own thread */
json_t *cachewarm_take_usage(cache_warmer_t *w);

/* formatCacheWarmingStatus of the current status (owned) */
char *cachewarm_status_text(cache_warmer_t *w);

/* pure helpers (exported for tests) */
int64_t cachewarm_delay_ms(int64_t ttl_ms); /* getCacheWarmingDelayMs; -1 when ttl <= 10 s */
/* resolvePromptCacheTier: "short" | "long" | NULL (caching off) */
const char *cachewarm_tier(const ai_model_t *m, const char *cache_retention, bool oauth);
bool cachewarm_replayable(const ai_model_t *m, const ai_stream_opts_t *o); /* isReplayable */

#endif
