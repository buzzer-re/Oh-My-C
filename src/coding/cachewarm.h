/* cachewarm.h - prompt cache warmer (port of omp session/cache-warmer.ts, `providers.cacheWarming`).
 *
 * The main loop's latest request is refreshed at 90% of the model's declared prompt-cache lifetime (at least ten
 * seconds before expiry), only while the conversation still matches it: a `max_tokens: 0` pre-warm where the catalog
 * declares the endpoint takes one (first-party Anthropic; a 400 falls back to replay for the session), else a
 * one-token replay priced with the session's observed replay output (512-token prior). Idle phase: while the expected
 * savings clear $0.05 (15% continuation, 30 min window). Streaming phase (ski rental): while the refresh spend since
 * the last request plus the next refresh stays within the cheapest buy-out: letting the entry expire, or, per
 * `providers.cacheWarmingStrategy`, a bridge compaction a minute before expiry (cmp_bridge_start) for prompts past
 * min(cacheWarmingCompactPercent of the window, cacheWarmingCompactTokens). Stops on context change, mode change, or
 * a refresh that misses the cache.
 * A model is warmed only when its catalog entry declares `promptCache` lifetimes for the retention tier used. */
#ifndef OMC_CACHEWARM_H
#define OMC_CACHEWARM_H

#include "ai/ai.h"

typedef struct cache_warmer cache_warmer_t;
typedef struct app app_t;

/* starts the timer thread; cap: the highest mode for this session ("idle"; task subagents "streaming") */
cache_warmer_t *cachewarm_new(app_t *a, const char *cap);
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

/* onResponse: confirm the in-flight request's entry and adopt the tier its response wrote (a failed response, or one that neither
 * read nor wrote the cache, never publishes its entry) */
void cachewarm_on_response(cache_warmer_t *w, const json_t *message);

/* What the next request finds in the prompt cache (PromptCacheStatus): the `cache` segment's marker and /session's rows */
typedef struct {
    bool cold;
    const char *reason;  /* "expired", "refresh-missed", "model", "thinking", "tools", "system-prompt", "retention", "compaction"; NULL when warm */
    char *reason_from, *reason_to; /* model and thinking changes */
    int64_t reason_at;   /* expired */
    bool inferred;       /* read off the transcript of a reopened session, not a response this process saw */
    const char *tier;    /* "short" | "long" */
    int64_t touched_at;  /* send time of the last request that read or wrote the entry */
    int64_t ttl_ms;      /* -1: the provider never expires the entry */
    int64_t cached_tokens, next_tokens;
    bool has_warming;
    const char *warming_state; /* "scheduled" | "refreshing" | "inactive" */
    int64_t next_warm_at, stops_at; /* -1: absent */
    char *warming_reason;
} cache_status_t;

/* getPromptCacheStatus; false when there is nothing to reason about (no reply that used the cache, no declared lifetime) */
bool cachewarm_status(app_t *a, int64_t now, int64_t next_tokens, cache_status_t *out);
void cache_status_free(cache_status_t *st);

/* /session's Prompt Cache rows: label, value, tone ("w": warning) triples (promptCacheDetails; src/ui/statusline.c) */
void ui_prompt_cache_details(const cache_status_t *st, int64_t now, strvec_t *out);

/* formatCacheWarmingStatus of the current status (owned) */
char *cachewarm_status_text(cache_warmer_t *w);

/* pure helpers (exported for tests) */
int64_t cachewarm_delay_ms(int64_t ttl_ms); /* getCacheWarmingDelayMs; -1 when ttl <= 10 s */
/* resolvePromptCacheTier: "short" | "long" | NULL (caching off) */
const char *cachewarm_tier(const ai_model_t *m, const char *cache_retention, bool oauth);
bool cachewarm_replayable(const ai_model_t *m, const ai_stream_opts_t *o); /* isReplayable */

#endif
