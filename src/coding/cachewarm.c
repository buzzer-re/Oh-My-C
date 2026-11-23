/* cachewarm.c - prompt cache warmer. See cachewarm.h. */
#include "coding/cachewarm.h"
#include "coding/app.h"

#include "ai/compat_detect.h"
#include "ai/provider_anthropic.h"
#include "coding/compaction.h"
#include "config/settings.h"
#include "session/session.h"

#include <math.h>
#include <pthread.h>
#include <time.h>

#define MAX_IDLE_WARMING_AGE_MS (30LL * 60000)
#define MIN_EXPECTED_SAVINGS 0.05 /* a refresh or bridge compaction must save at least this many dollars */
#define IDLE_CONTINUATION_PROBABILITY 0.15
/* billed output tokens a replay is priced at before the session observed one for the model */
#define REPLAY_OUTPUT_PRIOR_TOKENS 512
#define BRIDGE_COMPACTION_LEAD_MS 60000 /* a bridge compaction is decided this long before its entry expires */
#define BRIDGE_FALLBACK_MARGIN_MS 10000 /* a refresh replacing an unaccepted bridge must go out this long before expiry */
#define BRIDGE_IN_PROGRESS_REASON "bridge compaction in progress"

/* CacheWarmingDecision: a priced warming decision */
typedef struct {
    bool idle, available, has_compact;
    const char *method; /* "prewarm" | "replay"; NULL when the request cannot be refreshed safely */
    double warm_cost, miss_cost, compact_cost, spent, probability, savings;
    const char *action; /* "warm" | "compact" | "stop" */
} decision_t;

typedef struct {
    bool active;
    uint64_t gen;
    const ai_model_t *model;
    json_t *system, *messages, *tools; /* the request's provider context (owned) */
    ai_stream_opts_t opts;             /* copy; strings / json below owned */
    char *api_key, *session_id, *prompt_cache_key, *cache_retention, *text_verbosity;
    json_t *headers, *tool_choice, *thinking_budgets, *metadata, *stop_sequences;
    const char *tier;   /* "short" | "long" */
    int64_t delay_ms, started_at, next_warm_at;
    bool idle;          /* phase */
    bool armed;         /* a refresh timer is set (false while refreshing) */
    size_t arm_count;   /* live messages at arm time */
    uint64_t arm_hash, arm_shape;
    uint64_t entry_gen;     /* the entry this run keeps warm */
    int64_t refresh_sent_at;/* a refresh in flight went out then (0: none) */
    bool replayable;        /* refreshing cannot change the request's cache key (isReplayable) */
    double spent;           /* dollars billed for this run's refreshes */
    int decisions;          /* decisions taken so far; the "compact" strategy compacts at the first */
    bool bridge_failed;     /* the run's only buy-out is letting the entry expire */
    bool bridging;          /* a bridge compaction holds the entry */
    decision_t bridging_dec;
} run_t;

/* TrackedCacheEntry + the request shape it was armed with (PromptCacheShape) */
typedef struct {
    bool active;
    uint64_t gen;
    const ai_model_t *model;
    const char *tier;
    int64_t touched_at, prompt_tokens;
    bool refresh_missed;
    char *model_key, *model_name, *thinking, *retention;
    uint64_t tools, system;
    const void **msgs; /* the armed messages, by identity: appends are fine, a rewrite or compaction is not */
    size_t nmsgs;
} centry_t;

/* BranchPromptCacheObservation: what a branch says about the prompt cache its newest cached reply left */
typedef struct {
    bool valid;
    char *leaf;      /* the branch view it was folded from */
    bool have;
    char *provider, *model;
    int64_t timestamp, prompt_tokens;
    const char *observed_tier;
    int64_t warmed_at;
    bool refresh_missed, compacted;
} observation_t;

struct cache_warmer {
    app_t *a;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    bool started, quit;
    uint64_t gen;
    run_t run;
    volatile sig_atomic_t refresh_cancel; /* aborts an in-flight refresh when its run ends */
    char *reason;          /* inactive status reason */
    bool have_stop_decision;
    decision_t stop_decision;
    json_t *usage;         /* pending model_usage records */
    centry_t entry;        /* the entry the latest confirmed request wrote */
    centry_t pending;      /* the entry the in-flight request will write */
    uint64_t entry_gen;
    char *arm_session;     /* the session the warmer's entries belong to */
    observation_t obs;     /* memo of the current branch's fold */
    const char *cap;       /* highest mode for this session: "idle", or "streaming" for task subagents */
    strvec_t prewarm_refused; /* models whose endpoint rejected a pre-warm this session: refreshed by replay */
    strvec_t replay_keys;  /* billed output of this session's replays, per model: the running mean prices the next */
    VEC(double) replay_tokens;
    VEC(int) replay_counts;
    volatile sig_atomic_t bridge_stop; /* ends a wait on a bridge compaction (run replaced, warmer stopping) */
};

static void centry_free(centry_t *e) {
    free(e->model_key), free(e->model_name), free(e->thinking), free(e->retention);
    free(e->msgs);
    memset(e, 0, sizeof *e);
}

/* observedPromptCacheTier: the tier a completed response wrote, from the per-TTL cache-write breakdown */
static const char *observed_tier(const json_t *usage) {
    const json_t *c = json_get_obj(usage, "cttl");
    if (json_get_int(c, "ephemeral1h", 0) > 0) return "long";
    if (json_get_int(c, "ephemeral5m", 0) > 0) return "short";
    return NULL;
}

/* promptTokensOf */
static int64_t prompt_tokens_of(const json_t *usage) {
    return json_get_int(usage, "input", 0) + json_get_int(usage, "cacheRead", 0) + json_get_int(usage, "cacheWrite", 0);
}

static int64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- pure helpers ---------- */

int64_t cachewarm_delay_ms(int64_t ttl_ms) {
    if (ttl_ms <= 10000) return -1;
    return MAX((int64_t)1, (int64_t)floor(MIN(ttl_ms * 0.9, (double)(ttl_ms - 10000))));
}

static bool supports_long(const ai_model_t *m) {
    if (str_eq(m->api, "anthropic-messages")) return model_compat_bool(m, "supportsLongCacheRetention", false);
    if (str_eq(m->api, "factory-droid-agent")) return !json_get(json_get_obj(m->raw, "promptCache"), "short");
    if (str_eq(m->api, "bedrock-converse-stream")) return model_compat_bool(m, "supportsLongPromptCacheRetention", false);
    return true;
}

const char *cachewarm_tier(const ai_model_t *m, const char *retention, bool oauth) {
    bool long_default = oauth && str_eq(m->api, "anthropic-messages") && supports_long(m);
    const char *r = retention;
    if (!r || !*r || str_eq(r, "auto")) { /* resolveCacheRetention: explicit, PI_CACHE_RETENTION, fallback */
        const char *env = env_get("PI_CACHE_RETENTION");
        r = env && (str_eq(env, "long") || str_eq(env, "short") || str_eq(env, "none")) ? env : long_default ? "long" : "short";
    }
    if (str_eq(r, "none")) return NULL;
    return str_eq(r, "long") && supports_long(m) ? "long" : "short";
}

static const char *thinking_mode(const ai_model_t *m) { return json_get_str(m->thinking, "mode"); }

bool cachewarm_replayable(const ai_model_t *m, const ai_stream_opts_t *o) {
    if (str_eq(m->api, "anthropic-messages")) {
        bool requested = m->reasoning && o->reasoning != EFFORT_OFF;
        return !requested || str_eq(thinking_mode(m), "anthropic-adaptive");
    }
    if (str_eq(m->api, "bedrock-converse-stream")) {
        bool requested = m->reasoning && ((o->reasoning != EFFORT_OFF && !o->disable_reasoning) ||
                                          (json_get_bool(m->thinking, "requiresEffort", false) &&
                                           !json_get_bool(m->thinking, "suppressWhenOff", false)));
        return !requested || str_eq(thinking_mode(m), "anthropic-adaptive");
    }
    return true;
}

static int64_t ttl_seconds(const ai_model_t *m, const char *tier) {
    json_t *v = json_get(json_get_obj(m->raw, "promptCache"), tier);
    return json_is_num(v) ? (int64_t)(v->is_int ? v->u.num.i : v->u.num.d) : -1;
}

static int64_t delay_for(const ai_model_t *m, const char *tier) {
    int64_t s = ttl_seconds(m, tier);
    return s < 0 ? -1 : cachewarm_delay_ms(s * 1000);
}

static double price(const ai_model_t *m, int64_t input, int64_t output, int64_t cache_read, int64_t cache_write) {
    return (input * m->cost_input + output * m->cost_output + cache_read * m->cost_cache_read + cache_write * m->cost_cache_write) / 1e6;
}

/* ---------- live session fingerprints ---------- */

static uint64_t fnv(uint64_t h, const char *s) {
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) h = (h ^ *p) * 1099511628211ULL;
    return (h ^ 0xff) * 1099511628211ULL;
}

static uint64_t messages_hash(const json_t *msgs, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        char *d = json_dump(json_at(msgs, i), 0);
        h = fnv(h, d);
        free(d);
    }
    return h;
}

/* #cacheWarmingShape: model, system prompt, tool set, thinking level, retention setting */
static uint64_t live_shape(app_t *a) {
    agent_t *ag = a->agent;
    uint64_t h = 1469598103934665603ULL;
    h = fnv(h, a->model ? a->model->provider : "");
    h = fnv(h, a->model ? a->model->id : "");
    pthread_mutex_lock(&ag->mu);
    char *sp = json_dump(ag->system_prompt, 0);
    h = fnv(h, sp);
    free(sp);
    for (size_t i = 0; i < ag->tools.n; i++) h = fnv(h, ag->tools.v[i]->name);
    pthread_mutex_unlock(&ag->mu);
    h = fnv(h, effort_name(a->thinking));
    h = fnv(h, settings_str("providers.cacheRetention", "auto"));
    return h;
}

static size_t live_count(app_t *a, uint64_t *hash, size_t n) {
    pthread_mutex_lock(&a->agent->mu);
    size_t c = json_len(a->agent->messages);
    if (hash) *hash = c >= n ? messages_hash(a->agent->messages, n) : 0;
    pthread_mutex_unlock(&a->agent->mu);
    return c;
}

static bool run_current(cache_warmer_t *w, run_t *r) {
    uint64_t h = 0;
    if (live_count(w->a, &h, r->arm_count) < r->arm_count || h != r->arm_hash) return false;
    return live_shape(w->a) == r->arm_shape;
}

/* the specs as the wire carries them: a refreshed MCP tool keeps its name */
static uint64_t tools_fingerprint(agent_t *ag) {
    uint64_t h = 1469598103934665603ULL;
    pthread_mutex_lock(&ag->mu);
    for (size_t i = 0; i < ag->tools.n; i++) {
        agent_tool_t *t = ag->tools.v[i];
        h = fnv(h, t->name);
        h = fnv(h, t->description);
        char *sc = t->parameters ? json_dump(t->parameters, 0) : NULL;
        h = fnv(h, sc);
        free(sc);
    }
    pthread_mutex_unlock(&ag->mu);
    return h;
}

static uint64_t system_fingerprint(agent_t *ag) {
    uint64_t h = 1469598103934665603ULL;
    pthread_mutex_lock(&ag->mu);
    char *sp = json_dump(ag->system_prompt, 0);
    h = fnv(h, sp);
    free(sp);
    pthread_mutex_unlock(&ag->mu);
    return h;
}

static const char *setting_mode(void) {
    const char *m = settings_str("providers.cacheWarming", "idle");
    return str_eq(m, "off") || str_eq(m, "streaming") ? m : "idle";
}

/* capCacheWarmingMode: the lower of the setting and the session's cap (off < streaming < idle) */
static const char *mode_for(const cache_warmer_t *w) {
    const char *m = setting_mode();
    if (str_eq(m, "off") || !w || !w->cap || str_eq(w->cap, "idle")) return m;
    return str_eq(w->cap, "off") ? "off" : str_eq(m, "idle") ? "streaming" : m;
}

/* ---------- run lifecycle (call with w->mu held) ---------- */

static void run_free(run_t *r) {
    json_free(r->system), json_free(r->messages), json_free(r->tools);
    free(r->api_key), free(r->session_id), free(r->prompt_cache_key), free(r->cache_retention), free(r->text_verbosity);
    json_free(r->headers), json_free(r->tool_choice), json_free(r->thinking_budgets), json_free(r->metadata);
    json_free(r->stop_sequences);
    memset(r, 0, sizeof *r);
}

static json_t *clone_or_null(const json_t *j) { return j ? json_clone(j) : NULL; }
static char *dup_or_null(const char *s) { return s ? xstrdup(s) : NULL; }

/* deep copy of a run's request (the replay runs unlocked on its own copy) */
static void run_copy(run_t *dst, const run_t *src) {
    *dst = *src;
    dst->system = clone_or_null(src->system);
    dst->messages = clone_or_null(src->messages);
    dst->tools = clone_or_null(src->tools);
    dst->api_key = dup_or_null(src->api_key);
    dst->session_id = dup_or_null(src->session_id);
    dst->prompt_cache_key = dup_or_null(src->prompt_cache_key);
    dst->cache_retention = dup_or_null(src->cache_retention);
    dst->text_verbosity = dup_or_null(src->text_verbosity);
    dst->headers = clone_or_null(src->headers);
    dst->tool_choice = clone_or_null(src->tool_choice);
    dst->thinking_budgets = clone_or_null(src->thinking_budgets);
    dst->metadata = clone_or_null(src->metadata);
    dst->stop_sequences = clone_or_null(src->stop_sequences);
}

static void run_clear(cache_warmer_t *w) {
    if (!w->run.active) return;
    w->refresh_cancel = 1;
    w->bridge_stop = 1;
    run_free(&w->run);
}

static void set_reason(cache_warmer_t *w, const char *reason, const decision_t *d) {
    LOGD("cache warming inactive: %s", reason);
    free(w->reason);
    w->reason = xstrdup(reason);
    w->have_stop_decision = d != NULL;
    if (d) w->stop_decision = *d;
}

static void stop_locked(cache_warmer_t *w, const char *reason, const decision_t *d) {
    run_clear(w);
    set_reason(w, reason, d);
}

static const char *idle_stop_reason(run_t *r) {
    int64_t deadline = r->started_at + MAX_IDLE_WARMING_AGE_MS;
    if (wall_ms() >= deadline) return "30-minute idle safety limit reached";
    if (r->next_warm_at <= deadline) return NULL;
    return r->delay_ms >= MAX_IDLE_WARMING_AGE_MS ? "cache entry outlives the 30-minute idle window"
                                                 : "30-minute idle safety limit reached";
}

/* the send time of the latest touch of the run's entry */
static int64_t run_touched_at(cache_warmer_t *w, run_t *r) {
    if (w->entry.active && w->entry.gen == r->entry_gen) return w->entry.touched_at;
    if (w->pending.active && w->pending.gen == r->entry_gen) return w->pending.touched_at;
    return r->started_at;
}

/* #expiresAt: when the run's entry expires, counted from its latest touch; -1 without a declared lifetime */
static int64_t expires_at(cache_warmer_t *w, run_t *r) {
    int64_t sec = ttl_seconds(r->model, r->tier);
    return sec < 0 ? -1 : run_touched_at(w, r) + sec * 1000;
}

/* #compactDeadline: latest decision time at which a bridge compaction still reads the entry before it expires */
static int64_t compact_deadline(cache_warmer_t *w, run_t *r) {
    int64_t exp = expires_at(w, r);
    if (exp < 0 || exp - BRIDGE_COMPACTION_LEAD_MS <= run_touched_at(w, r)) return -1;
    return exp - BRIDGE_COMPACTION_LEAD_MS;
}

static char *model_key(const ai_model_t *m) { return xasprintf("%s/%s", m->provider, m->id); }

/* isPromptCachePrewarmSupported: the catalog's promptCachePrewarm on the provider's own HTTPS endpoint (the host of
 * its bundled base URL) */
static bool prewarm_supported(const ai_model_t *m) {
    const json_t *v = compat_catalog_policy(m->provider, m->api, m->id, "promptCachePrewarm");
    if (!json_is_bool(v) || !v->u.b || !m->base_url || !str_starts(m->base_url, "https://")) return false;
    const char *bundled = NULL;
    for (size_t i = 0, n = catalog_count(); i < n && !bundled; i++) {
        const ai_model_t *c = catalog_at(i);
        if (str_eq(c->provider, m->provider) && c->base_url) bundled = c->base_url;
    }
    if (!bundled) return false;
    const char *h1 = m->base_url + 8, *h2 = bundled + (str_starts(bundled, "https://") ? 8 : str_starts(bundled, "http://") ? 7 : 0);
    size_t l1 = strcspn(h1, "/:?#"), l2 = strcspn(h2, "/:?#");
    return l1 == l2 && !strncasecmp(h1, h2, l1);
}

/* #rentMethod: the safest refresh the request supports, NULL when refreshing could change its cache key */
static const char *rent_method(cache_warmer_t *w, run_t *r) {
    if (!r->replayable) return NULL;
    const char *tc = json_sv(r->tool_choice); /* the endpoint refuses max_tokens 0 with a forced tool choice */
    bool unforced = !r->tool_choice || str_eq(tc, "auto") || str_eq(tc, "none");
    char *k = model_key(r->model);
    bool refused = strvec_contains(&w->prewarm_refused, k);
    free(k);
    return prewarm_supported(r->model) && unforced && !refused ? "prewarm" : "replay";
}

static void observe_replay_output(cache_warmer_t *w, const ai_model_t *m, int64_t tokens) {
    char *k = model_key(m);
    size_t i = 0;
    while (i < w->replay_keys.n && !str_eq(w->replay_keys.v[i], k)) i++;
    if (i == w->replay_keys.n) {
        strvec_push(&w->replay_keys, k);
        vec_push(&w->replay_tokens, 0.0);
        vec_push(&w->replay_counts, 0);
    }
    w->replay_tokens.v[i] += (double)tokens;
    w->replay_counts.v[i]++;
    free(k);
}

/* #expectedReplayOutput: this session's running mean for the model, before that the prior */
static double expected_replay_output(cache_warmer_t *w, const ai_model_t *m) {
    char *k = model_key(m);
    double out = REPLAY_OUTPUT_PRIOR_TOKENS;
    for (size_t i = 0; i < w->replay_keys.n; i++)
        if (str_eq(w->replay_keys.v[i], k) && w->replay_counts.v[i] > 0) out = w->replay_tokens.v[i] / w->replay_counts.v[i];
    free(k);
    return out;
}

/* resolveBridgeCompactionThreshold: percent of the context window, capped at tokens; a window-less model uses tokens */
static int64_t bridge_threshold(const ai_model_t *m) {
    int64_t pct = settings_int("providers.cacheWarmingCompactPercent", 60);
    int64_t tokens = settings_int("providers.cacheWarmingCompactTokens", 150000);
    if (m->context_window <= 0) return tokens;
    return MIN((int64_t)floor((double)m->context_window * (double)pct / 100), tokens);
}

static const char *bridge_strategy(void) {
    const char *s = settings_str("providers.cacheWarmingStrategy", "auto");
    return str_eq(s, "keepalive") || str_eq(s, "compact") ? s : "auto";
}

/* writePrice: writing tokens into the cache; models without a write price bill the prompt as input */
static double write_price(const ai_model_t *m, int64_t tokens) {
    return m->cost_cache_write > 0 ? price(m, 0, 0, 0, tokens) : price(m, tokens, 0, 0, 0);
}

/* #compactCost: bridging with compaction (reading the prompt, the summary, re-writing the compacted context), or
 * false: no bridge-capable method, keepalive, a failed bridge, a prompt under the threshold, or an entry too
 * short-lived to compact before it expires */
static bool compact_cost(cache_warmer_t *w, run_t *r, int64_t tokens, double *cost) {
    if (r->bridge_failed || str_eq(bridge_strategy(), "keepalive")) return false;
    if (tokens < bridge_threshold(r->model) || compact_deadline(w, r) < 0) return false;
    int64_t summary, kept;
    if (!cmp_bridge_estimate(w->a, tokens, &summary, &kept)) return false;
    const ai_model_t *m = r->model;
    kept = MIN(kept, tokens);
    *cost = price(m, 0, 0, tokens, 0) + price(m, 0, summary, 0, 0) + MAX(0.0, write_price(m, kept) - price(m, 0, 0, kept, 0));
    return true;
}

/* #act: idle refreshes while the expected savings clear the floor; streaming (ski rental) refreshes while the spend
 * so far plus this refresh stays within the cheapest buy-out, then compacts when that saves at least the floor over
 * letting the entry expire, else stops. A compaction found too late to read the entry waits one more refresh. */
static const char *act(run_t *r, const decision_t *d, bool compact_now) {
    if (!d->available) return "stop";
    bool rent_pays = d->method && d->savings >= MIN_EXPECTED_SAVINGS;
    if (d->idle) return rent_pays ? "warm" : "stop";
    if (compact_now && r->decisions == 0 && str_eq(bridge_strategy(), "compact")) return "compact";
    double buy = d->has_compact ? MIN(d->miss_cost, d->compact_cost) : d->miss_cost;
    if (rent_pays && d->spent + d->warm_cost <= buy) return "warm";
    if (d->has_compact && d->miss_cost - d->compact_cost >= MIN_EXPECTED_SAVINGS) {
        if (compact_now) return "compact";
        if (rent_pays) return "warm";
    }
    return "stop";
}

/* #evaluate: prices the decision taken at decision_at and makes it (w->mu held) */
static decision_t evaluate(cache_warmer_t *w, run_t *r, int64_t decision_at, const char **stop_reason, bool *compact_now) {
    const ai_model_t *m = r->model;
    int64_t tokens = 0;
    pthread_mutex_lock(&w->a->agent->mu); /* lastPromptTokens: the latest assistant usage */
    json_t *msgs = w->a->agent->messages;
    for (size_t i = json_len(msgs); i-- > 0;) {
        json_t *x = json_at(msgs, i);
        if (!str_eq(json_get_str(x, "role"), "assistant")) continue;
        json_t *u = json_get(x, "usage");
        tokens = json_get_int(u, "input", 0) + json_get_int(u, "cacheRead", 0) + json_get_int(u, "cacheWrite", 0);
        break;
    }
    pthread_mutex_unlock(&w->a->agent->mu);
    decision_t d = {.idle = r->idle};
    double hit = price(m, 0, 0, tokens, 0);
    double miss = write_price(m, tokens);
    d.method = rent_method(w, r);
    d.warm_cost = !d.method ? 0 : str_eq(d.method, "prewarm") ? hit : hit + expected_replay_output(w, m) * m->cost_output / 1e6;
    d.miss_cost = MAX(0.0, miss - hit);
    d.spent = r->spent;
    d.probability = r->idle ? IDLE_CONTINUATION_PROBABILITY : 1;
    d.available = tokens > 0 && (hit > 0 || miss > 0);
    d.savings = d.probability * d.miss_cost - d.warm_cost;
    d.has_compact = !r->idle && compact_cost(w, r, tokens, &d.compact_cost);
    int64_t deadline = compact_deadline(w, r);
    bool now = d.has_compact && deadline >= 0 && decision_at <= deadline;
    if (stop_reason)
        *stop_reason = !d.available ? "cache economics unavailable"
                       : !d.method ? "request cannot be replayed safely"
                       : d.savings < MIN_EXPECTED_SAVINGS ? "expected savings below threshold"
                                                          : "refresh spend reached the cost of losing the cache";
    d.action = act(r, &d, now);
    if (compact_now) *compact_now = now;
    return d;
}

/* #schedule: arms the next decision at the refresh point, or, while streaming, at the compaction deadline when the
 * decision taken then would compact; a streaming request that cannot be refreshed and has no compaction ahead stops */
static void schedule_locked(cache_warmer_t *w) {
    run_t *r = &w->run;
    int64_t now = wall_ms();
    r->next_warm_at = now + r->delay_ms;
    if (r->idle) {
        const char *reason = idle_stop_reason(r);
        if (reason) {
            stop_locked(w, reason, NULL);
            return;
        }
    } else {
        int64_t deadline = compact_deadline(w, r);
        if (deadline >= 0 && str_eq(evaluate(w, r, deadline, NULL, NULL).action, "compact")) {
            r->next_warm_at = MAX(now, deadline);
        } else if (!r->replayable) {
            stop_locked(w, "request cannot be replayed safely", NULL);
            return;
        }
    }
    r->armed = true;
    LOGD("cache warming: decision in %lldms (%s phase)", (long long)(r->next_warm_at - now), r->idle ? "idle" : "streaming");
    pthread_cond_broadcast(&w->cv);
}

static const char *mode_stop_reason(cache_warmer_t *w, run_t *r) {
    const char *m = mode_for(w);
    if (str_eq(m, "off")) return "cache warming disabled";
    if (str_eq(m, "streaming") && r->idle) return "agent run settled";
    return NULL;
}

/* ---------- public ---------- */

void cachewarm_start(cache_warmer_t *w, const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o) {
    if (!w || !m) return;
    bool oauth = str_eq(m->api, "anthropic-messages") && anthropic_model_is_oauth(m, o->api_key);
    const char *tier = cachewarm_tier(m, o->cache_retention, oauth);
    uint64_t hash = 0;
    size_t count = live_count(w->a, NULL, 0);
    live_count(w->a, &hash, count);
    uint64_t shape = live_shape(w->a);
    /* the entry this request writes, published once its response confirms it */
    agent_t *ag = w->a->agent;
    centry_t pe = {0};
    if (tier) {
        pe.active = true;
        pe.model = m;
        pe.tier = tier;
        pe.touched_at = wall_ms();
        pe.model_key = xasprintf("%s/%s", m->provider, m->id);
        pe.model_name = xstrdup(m->name ? m->name : m->id);
        pe.thinking = xstrdup(effort_name(w->a->thinking));
        pe.retention = xstrdup(settings_str("providers.cacheRetention", "auto"));
        pe.tools = tools_fingerprint(ag);
        pe.system = system_fingerprint(ag);
        pthread_mutex_lock(&ag->mu);
        pe.nmsgs = json_len(ag->messages);
        pe.msgs = xcalloc(pe.nmsgs + 1, sizeof *pe.msgs);
        for (size_t i = 0; i < pe.nmsgs; i++) pe.msgs[i] = json_at(ag->messages, i);
        pthread_mutex_unlock(&ag->mu);
    }
    const char *sid = w->a->session ? session_id(w->a->session) : NULL;
    pthread_mutex_lock(&w->mu);
    /* the warmer keeps its confirmed entry across requests until a response replaces it; another transcript's says nothing */
    if (!str_eq(sid, w->arm_session)) {
        centry_free(&w->entry);
        free(w->arm_session);
        w->arm_session = sid ? xstrdup(sid) : NULL;
    }
    centry_free(&w->pending);
    w->pending = pe;
    if (pe.active) w->pending.gen = ++w->entry_gen;
    run_clear(w);
    if (str_eq(mode_for(w), "off")) set_reason(w, "cache warming disabled", NULL);
    else if (!tier) set_reason(w, "request disabled prompt caching", NULL);
    else if (delay_for(m, tier) < 0) set_reason(w, "cache lifetime unavailable", NULL);
    else {
        run_t *r = &w->run;
        r->active = true;
        r->gen = ++w->gen;
        r->model = m;
        r->system = ctx->system_prompt ? json_clone(ctx->system_prompt) : NULL;
        r->messages = json_clone(ctx->messages);
        r->tools = ctx->tools ? json_clone(ctx->tools) : NULL;
        r->opts = *o;
        r->api_key = o->api_key ? xstrdup(o->api_key) : NULL;
        r->session_id = o->session_id ? xstrdup(o->session_id) : NULL;
        r->prompt_cache_key = o->prompt_cache_key ? xstrdup(o->prompt_cache_key) : NULL;
        r->cache_retention = o->cache_retention ? xstrdup(o->cache_retention) : NULL;
        r->text_verbosity = o->text_verbosity ? xstrdup(o->text_verbosity) : NULL;
        r->headers = o->headers ? json_clone(o->headers) : NULL;
        r->tool_choice = o->tool_choice ? json_clone(o->tool_choice) : NULL;
        r->thinking_budgets = o->thinking_budgets ? json_clone(o->thinking_budgets) : NULL;
        r->metadata = o->metadata ? json_clone(o->metadata) : NULL;
        r->stop_sequences = o->stop_sequences ? json_clone(o->stop_sequences) : NULL;
        r->tier = tier;
        r->delay_ms = delay_for(m, tier);
        r->started_at = wall_ms();
        r->arm_count = count;
        r->arm_hash = hash;
        r->arm_shape = shape;
        r->entry_gen = w->pending.gen;
        r->replayable = cachewarm_replayable(m, o);
        schedule_locked(w);
    }
    pthread_mutex_unlock(&w->mu);
}

void cachewarm_settled(cache_warmer_t *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    run_t *r = &w->run;
    if (r->active) {
        if (str_eq(mode_for(w), "streaming")) stop_locked(w, "agent run settled", NULL);
        else {
            r->idle = true;
            const char *reason = idle_stop_reason(r);
            if (reason) stop_locked(w, reason, NULL);
        }
    }
    pthread_mutex_unlock(&w->mu);
}

void cachewarm_mode_changed(cache_warmer_t *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    if (w->run.active) {
        const char *reason = mode_stop_reason(w, &w->run);
        if (reason) stop_locked(w, reason, NULL);
    }
    pthread_mutex_unlock(&w->mu);
}

void cachewarm_cancel(cache_warmer_t *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    centry_free(&w->entry);
    centry_free(&w->pending);
    stop_locked(w, "inactive", NULL);
    pthread_mutex_unlock(&w->mu);
}

/* answers: the response to a request sent to `m`; a server-side fallback retags it, its marker's `from` names the requested model */
static bool answers(const json_t *msg, const ai_model_t *m) {
    if (!str_eq(json_get_str(msg, "provider"), m->provider)) return false;
    if (str_eq(json_get_str(msg, "model"), m->id)) return true;
    json_t *b;
    json_arr_foreach(json_get_arr(msg, "content"), i, b) {
        if (str_eq(json_get_str(b, "type"), "fallback") && str_eq(json_get_str(json_get_obj(b, "from"), "model"), m->id)) return true;
    }
    return false;
}

void cachewarm_on_response(cache_warmer_t *w, const json_t *msg) {
    if (!w || !msg) return;
    const json_t *u = json_get_obj(msg, "usage");
    const char *observed = observed_tier(u);
    pthread_mutex_lock(&w->mu);
    if (w->pending.active && answers(msg, w->pending.model)) {
        if (!str_eq(json_get_str(msg, "stopReason"), "error") &&
            (json_get_int(u, "cacheRead", 0) > 0 || json_get_int(u, "cacheWrite", 0) > 0)) {
            if (observed) w->pending.tier = observed;
            w->pending.prompt_tokens = prompt_tokens_of(u);
            centry_free(&w->entry);
            w->entry = w->pending;
            memset(&w->pending, 0, sizeof w->pending);
        } else centry_free(&w->pending);
    }
    run_t *r = &w->run;
    if (r->active && r->armed && answers(msg, r->model) && observed && !str_eq(observed, r->tier)) {
        int64_t delay = delay_for(r->model, observed);
        if (delay < 0) stop_locked(w, "cache lifetime unavailable", NULL);
        else {
            r->tier = observed;
            r->delay_ms = delay;
            schedule_locked(w);
        }
    }
    pthread_mutex_unlock(&w->mu);
}

/* #touch: a refresh sent at sent_at left the run's entry in the cache: its lifetime restarts then, at the replay's size */
static void touch_locked(cache_warmer_t *w, uint64_t entry_gen, int64_t sent_at, const json_t *usage) {
    if (w->pending.active && w->pending.gen == entry_gen) {
        centry_free(&w->entry);
        w->entry = w->pending;
        memset(&w->pending, 0, sizeof w->pending);
    }
    if (w->entry.active && w->entry.gen == entry_gen) {
        w->entry.touched_at = sent_at;
        w->entry.prompt_tokens = prompt_tokens_of(usage);
        w->entry.refresh_missed = false;
    }
}

json_t *cachewarm_take_usage(cache_warmer_t *w) {
    if (!w) return NULL;
    pthread_mutex_lock(&w->mu);
    json_t *u = w->usage;
    w->usage = NULL;
    pthread_mutex_unlock(&w->mu);
    return u;
}

/* ---------- replay ---------- */

typedef struct {
    volatile sig_atomic_t *cancel;
    bool terminal;  /* Converse runs to terminal usage under its one-token cap */
    json_t *partial; /* last partial (owned clone) */
    bool cut;
} replay_t;

static bool generation_event(const ai_event_t *ev) {
    switch (ev->type) {
    case AI_EV_TEXT_START:
    case AI_EV_THINKING_START:
    case AI_EV_TOOLCALL_START: return true;
    case AI_EV_TEXT_DELTA:
    case AI_EV_THINKING_DELTA:
    case AI_EV_TOOLCALL_DELTA: return ev->delta_len > 0;
    default: return false;
    }
}

static void replay_cb(void *ud, const ai_event_t *ev) {
    replay_t *rp = ud;
    if (ev->type == AI_EV_DONE || ev->type == AI_EV_ERROR || rp->cut) return;
    if (ev->partial) {
        json_free(rp->partial);
        rp->partial = json_clone(ev->partial);
    }
    if (generation_event(ev) && !rp->terminal) {
        rp->cut = true;
        *rp->cancel = 1;
    }
}

/* #replay: one output token; #prewarm (pre): the request with max_tokens 0, no output billed. Owned message (usage
 * of the accepted request) or NULL */
static json_t *replay(run_t *r, volatile sig_atomic_t *cancel, bool pre) {
    ai_stream_opts_t o = r->opts;
    o.api_key = r->api_key;
    o.session_id = r->session_id;
    o.prompt_cache_key = r->prompt_cache_key;
    o.cache_retention = r->cache_retention;
    o.text_verbosity = r->text_verbosity;
    o.headers = r->headers;
    o.tool_choice = r->tool_choice;
    o.thinking_budgets = r->thinking_budgets;
    o.metadata = r->metadata;
    o.stop_sequences = r->stop_sequences;
    if (!pre) o.max_tokens = 1;
    o.prewarm = pre;
    o.cancel = cancel;
    o.on_payload = NULL;
    o.cursor_exec = NULL;
    o.anthropic_compaction = NULL;
    json_t *budgets = NULL;
    if (str_eq(r->model->api, "bedrock-converse-stream") && str_eq(thinking_mode(r->model), "anthropic-adaptive")) {
        budgets = r->thinking_budgets ? json_clone(r->thinking_budgets) : json_obj();
        json_t *e;
        json_arr_foreach(json_get_arr(r->model->thinking, "efforts"), i, e) if (json_is_str(e)) json_set_int(budgets, json_sv(e), 0);
        o.thinking_budgets = budgets;
    }
    ai_context_t ctx = {.system_prompt = r->system, .messages = r->messages, .tools = r->tools};
    replay_t rp = {.cancel = cancel, .terminal = str_eq(r->model->api, "bedrock-converse-stream")};
    json_t *msg = ai_stream(r->model, &ctx, &o, replay_cb, &rp);
    json_free(budgets);
    const char *st = json_get_str(msg, "stopReason");
    if (!rp.cut && !str_eq(st, "aborted")) {
        json_free(rp.partial);
        return msg; /* done, or a provider error */
    }
    json_free(msg);
    if (!rp.partial) return NULL;
    json_t *u = json_get(rp.partial, "usage");
    if (u) ai_usage_compute_cost(r->model, u);
    json_set_str(rp.partial, "stopReason", "aborted");
    return rp.partial;
}

static void record_usage(cache_warmer_t *w, const json_t *msg, const json_t *refresh) {
    json_t *e = json_obj();
    json_set_str(e, "purpose", "cache-warm");
    json_set_str(e, "api", json_get_str(msg, "api"));
    json_set_str(e, "provider", json_get_str(msg, "provider"));
    json_set_str(e, "model", json_get_str(msg, "model"));
    json_set(e, "usage", json_clone(json_get(msg, "usage")));
    json_set_str(e, "stopReason", json_get_str(msg, "stopReason"));
    if (json_get_str(msg, "errorMessage")) json_set_str(e, "errorMessage", json_get_str(msg, "errorMessage")); /* omp 5f25874081 */
    if (refresh) json_set(e, "cacheRefresh", json_clone(refresh));
    if (!w->usage) w->usage = json_arr();
    json_push(w->usage, e);
}

/* a session event for RPC clients (app notice level = event type, text = JSON; the TUI ignores it); owns ev
 * only when it is the end event (the start event is reused for the end) */
static void emit_event(cache_warmer_t *w, json_t *ev) {
    char *t = json_dump(ev, 0);
    app_notice(w->a, json_get_str(ev, "type"), "%s", t);
    free(t);
    if (str_eq(json_get_str(ev, "type"), "cache_warming_end")) json_free(ev);
}

static bool run_live(cache_warmer_t *w, run_t *r, uint64_t gen) { return r->active && r->gen == gen; }

/* #validateRun (w->mu held on entry and exit; released while the live conversation is compared) */
static bool validate_locked(cache_warmer_t *w, uint64_t gen) {
    run_t *r = &w->run;
    if (!run_live(w, r, gen)) return false;
    const char *reason = mode_stop_reason(w, r);
    if (!reason) {
        pthread_mutex_unlock(&w->mu);
        bool current = run_current(w, r);
        pthread_mutex_lock(&w->mu);
        if (!run_live(w, r, gen)) return false;
        if (!current) reason = "conversation context changed";
    }
    if (reason) {
        stop_locked(w, reason, NULL);
        return false;
    }
    return true;
}

/* #send: one refresh, settled on its outcome; true when the endpoint refused a pre-warm (w->mu held) */
static bool send_locked(cache_warmer_t *w, const char *method) {
    run_t *r = &w->run;
    uint64_t gen = r->gen;
    w->refresh_cancel = 0;
    int64_t sent_at = r->refresh_sent_at = wall_ms();
    uint64_t entry_gen = r->entry_gen;
    bool pre = str_eq(method, "prewarm");
    run_t req;
    run_copy(&req, r);
    pthread_mutex_unlock(&w->mu);
    /* onRefreshStart / onRefreshEnd: RPC cache_warming_start / cache_warming_end session events */
    json_t *ev = json_obj();
    json_set_str(ev, "type", "cache_warming_start");
    json_set_str(ev, "phase", req.idle ? "idle" : "streaming");
    json_set_str(ev, "method", method);
    json_set_str(ev, "provider", req.model->provider);
    json_set_str(ev, "model", req.model->id);
    emit_event(w, ev);
    json_t *msg = replay(&req, &w->refresh_cancel, pre);
    bool still_current = msg && !str_eq(json_get_str(msg, "stopReason"), "error") && run_current(w, &req);
    const ai_model_t *model = req.model;
    run_free(&req);
    pthread_mutex_lock(&w->mu);
    if (run_live(w, r, gen)) r->refresh_sent_at = 0;
    const char *outcome = "error";
    bool refused = false;
    /* a refresh cancelled or replaced after acceptance was still billed: spend tracking sees it; only one that answered
     * for the conversation as it still stands says anything about that conversation's cache entry */
    if (msg && json_get_int(json_get(msg, "usage"), "totalTokens", 0) > 0) {
        json_t *u = json_get(msg, "usage");
        if (run_live(w, r, gen)) r->spent += json_get_num(json_get_obj(u, "cost"), "total", 0);
        if (!pre) observe_replay_output(w, model, json_get_int(u, "output", 0));
        json_t *refresh = NULL;
        if (still_current && run_live(w, r, gen)) {
            refresh = json_obj();
            json_set_int(refresh, "sentAt", sent_at);
            json_set_bool(refresh, "hit", json_get_int(u, "cacheRead", 0) > 0 && json_get_int(u, "cacheWrite", 0) == 0);
        }
        record_usage(w, msg, refresh);
        json_free(refresh);
        json_set(ev, "usage", json_clone(u));
    }
    const char *stop = NULL;
    if (!run_live(w, r, gen)) {
        outcome = "aborted";
    } else if (pre && str_eq(json_get_str(msg, "stopReason"), "error") && json_get_int(msg, "errorStatus", 0) == 400) {
        /* this endpoint does not take a pre-warm for the model: replay instead, now and for the session */
        char *k = model_key(model);
        if (!strvec_contains(&w->prewarm_refused, k)) strvec_push(&w->prewarm_refused, k);
        free(k);
        refused = true;
    } else if (!msg || str_eq(json_get_str(msg, "stopReason"), "error")) {
        stop = "refresh failed";
    } else {
        json_t *u = json_get(msg, "usage");
        if (json_get_int(u, "cacheRead", 0) <= 0 || json_get_int(u, "cacheWrite", 0) > 0) {
            outcome = "miss";
            /* what it wrote, though, the next request reads: a new lifetime from the refresh's send */
            if (json_get_int(u, "cacheWrite", 0) > 0) touch_locked(w, entry_gen, sent_at, u);
            else if (w->entry.active && w->entry.gen == entry_gen) w->entry.refresh_missed = true;
            stop = "refresh missed the cache";
        } else {
            outcome = "hit";
            /* a hit proves the run's entry exists even before its own response confirmed it, and restarts its lifetime */
            touch_locked(w, entry_gen, sent_at, u);
            stop = mode_stop_reason(w, r);
        }
    }
    json_free(msg);
    if (stop) stop_locked(w, stop, NULL);
    else if (!strcmp(outcome, "hit")) {
        schedule_locked(w);
        if (!w->run.active) stop = w->reason; /* the schedule ended warming */
    }
    /* {...refresh, outcome, usage?, warmingStopReason?} */
    json_set_str(ev, "type", "cache_warming_end");
    json_t *usage = json_take(ev, "usage");
    json_set_str(ev, "outcome", outcome);
    if (usage) json_set(ev, "usage", usage);
    if (stop) json_set_str(ev, "warmingStopReason", stop);
    pthread_mutex_unlock(&w->mu);
    emit_event(w, ev);
    pthread_mutex_lock(&w->mu);
    return refused;
}

/* #refresh: a pre-warm the endpoint refuses is replayed in the same slot */
static void refresh_send_locked(cache_warmer_t *w, const char *method) {
    uint64_t gen = w->run.gen;
    if (send_locked(w, method) && validate_locked(w, gen)) send_locked(w, "replay");
}

/* #bridge: hands the wait to the session's bridge compaction. Its request reads the entry, so it holds the entry one
 * more lifetime either way; one that never reached the provider is replaced by a single refresh while the entry can
 * still be refreshed. */
static void bridge_locked(cache_warmer_t *w, const decision_t *d) {
    run_t *r = &w->run;
    uint64_t gen = r->gen;
    r->bridging = true;
    r->bridging_dec = *d;
    w->bridge_stop = 0;
    pthread_mutex_unlock(&w->mu);
    int outcome = cmp_bridge_start(w->a, &w->bridge_stop);
    pthread_mutex_lock(&w->mu);
    if (!run_live(w, r, gen)) return;
    r->bridging = false;
    if (outcome == CMP_BRIDGE_ARMED) {
        stop_locked(w, "bridged by compaction", d);
        return;
    }
    r->bridge_failed = true;
    if (outcome == CMP_BRIDGE_FAILED_UNACCEPTED && validate_locked(w, gen)) {
        const char *method = rent_method(w, r);
        int64_t exp = expires_at(w, r);
        if (method && exp >= 0 && wall_ms() <= exp - BRIDGE_FALLBACK_MARGIN_MS) {
            refresh_send_locked(w, method);
            return;
        }
    }
    if (run_live(w, r, gen)) stop_locked(w, "bridge compaction failed", d);
}

/* #decideAt, on the warmer thread with w->mu held on entry and exit */
static void decide_locked(cache_warmer_t *w) {
    run_t *r = &w->run;
    r->armed = false;
    r->refresh_sent_at = 0;
    if (!validate_locked(w, r->gen)) return;
    const char *stop_reason = NULL;
    decision_t d = evaluate(w, r, r->next_warm_at, &stop_reason, NULL);
    r->decisions++;
    if (str_eq(d.action, "stop")) stop_locked(w, stop_reason, &d);
    else if (str_eq(d.action, "compact")) bridge_locked(w, &d);
    else if (d.method) refresh_send_locked(w, d.method);
    else stop_locked(w, "request cannot be replayed safely", &d);
}

static void *warmer_main(void *arg) {
    cache_warmer_t *w = arg;
    pthread_mutex_lock(&w->mu);
    while (!w->quit) {
        run_t *r = &w->run;
        if (!r->active || !r->armed) {
            pthread_cond_wait(&w->cv, &w->mu);
            continue;
        }
        int64_t now = wall_ms();
        if (now < r->next_warm_at) {
            int64_t until = r->next_warm_at;
            struct timespec ts = {.tv_sec = until / 1000, .tv_nsec = (until % 1000) * 1000000L};
            pthread_cond_timedwait(&w->cv, &w->mu, &ts);
            continue;
        }
        decide_locked(w);
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

cache_warmer_t *cachewarm_new(app_t *a, const char *cap) {
    cache_warmer_t *w = xcalloc(1, sizeof *w);
    w->a = a;
    w->cap = str_eq(cap, "streaming") || str_eq(cap, "off") ? cap : "idle";
    w->reason = xstrdup("waiting for first request");
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    w->started = pthread_create(&w->th, NULL, warmer_main, w) == 0;
    return w;
}

void cachewarm_stop(cache_warmer_t *w) {
    if (!w || !w->started) return;
    pthread_mutex_lock(&w->mu);
    w->quit = true;
    w->refresh_cancel = 1;
    w->bridge_stop = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->th, NULL);
    w->started = false;
}

void cachewarm_free(cache_warmer_t *w) {
    if (!w) return;
    cachewarm_stop(w);
    run_clear(w);
    json_free(w->usage);
    free(w->reason);
    centry_free(&w->entry);
    centry_free(&w->pending);
    free(w->arm_session);
    free(w->obs.leaf), free(w->obs.provider), free(w->obs.model);
    strvec_free(&w->prewarm_refused);
    strvec_free(&w->replay_keys);
    vec_free(&w->replay_tokens);
    vec_free(&w->replay_counts);
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    free(w);
}

/* ---------- status (formatCacheWarmingStatus) ---------- */

static char *dollars(double v) { return v < 0 ? xasprintf("-$%.3f", fabs(v)) : xasprintf("$%.3f", v); }

/* formatCacheWarmingEconomics */
static char *economics(const decision_t *d) {
    if (!d->available) return xstrdup("cache economics unavailable");
    char *wc = dollars(d->warm_cost);
    char *rent = d->method ? xasprintf("%s %s", d->method, wc) : xstrdup("no safe refresh");
    free(wc);
    char *r;
    if (d->idle) {
        long p = lround(d->probability * 100);
        char *sv = dollars(d->savings);
        r = xasprintf("%s, %ld%% continuation probability, expected savings %s %s $%.3f", rent, p, sv,
                      d->savings >= MIN_EXPECTED_SAVINGS ? ">=" : "<", MIN_EXPECTED_SAVINGS);
        free(sv);
    } else {
        bool compact = d->has_compact && d->compact_cost < d->miss_cost;
        char *sp = dollars(d->spent), *by = dollars(compact ? d->compact_cost : d->miss_cost);
        r = xasprintf("%s, spent %s of buy-out %s %s", rent, sp, compact ? "compact" : "expire", by);
        free(sp), free(by);
    }
    free(rent);
    return r;
}

/* #stopsAt: the idle window's end, or the streaming decision at which renting stops paying if prices hold; -1
 * while streaming refreshes are free */
static int64_t stops_at(const run_t *r, const decision_t *d) {
    if (r->idle) return r->started_at + MAX_IDLE_WARMING_AGE_MS;
    if (!str_eq(d->action, "warm")) return r->next_warm_at;
    if (d->warm_cost <= 0) return -1;
    double buy = d->has_compact ? MIN(d->miss_cost, d->compact_cost) : d->miss_cost;
    int64_t affordable = MAX((int64_t)1, (int64_t)floor((buy - d->spent) / d->warm_cost));
    return r->next_warm_at + affordable * r->delay_ms;
}

char *cachewarm_status_text(cache_warmer_t *w) {
    if (!w) return xstrdup("Inactive (unknown reason)");
    if (str_eq(mode_for(w), "off")) return xstrdup("Inactive (cache warming disabled)");
    pthread_mutex_lock(&w->mu);
    run_t *r = &w->run;
    char *out;
    if (!r->active || r->bridging) {
        bool bridging = r->active && r->bridging;
        const decision_t *d = bridging ? &r->bridging_dec : w->have_stop_decision ? &w->stop_decision : NULL;
        const char *reason = bridging ? BRIDGE_IN_PROGRESS_REASON : w->reason;
        if (d && d->available) {
            char *e = economics(d);
            const char *verb = bridging ? "Compacting" : "Stopped";
            out = reason ? xasprintf("%s (%s; %s -> %s)", verb, reason, e, d->action) : xasprintf("%s (%s -> %s)", verb, e, d->action);
            free(e);
        } else {
            out = xasprintf("Inactive (%s)", reason ? reason : "unknown reason");
        }
        pthread_mutex_unlock(&w->mu);
        return out;
    }
    run_t snap = *r;
    decision_t d = evaluate(w, r, r->next_warm_at, NULL, NULL);
    pthread_mutex_unlock(&w->mu);
    if (!run_current(w, &snap)) return xstrdup("Inactive (conversation context changed)");
    if (!d.available && snap.armed) return xstrdup("Inactive (cache economics unavailable)");
    char *e = economics(&d);
    if (!snap.armed) {
        out = xasprintf("Warming cache (%s -> %s)", e, d.action);
    } else {
        int64_t now = wall_ms();
        sb_t t = SB_INIT;
        if (snap.next_warm_at <= now) sb_append(&t, "Decision now");
        else {
            int64_t sec_all = (snap.next_warm_at - now + 999) / 1000;
            int64_t h = sec_all / 3600, m = (sec_all % 3600) / 60, sec = sec_all % 60;
            sb_append(&t, "Decision in ");
            bool any = false;
            if (h > 0) sb_appendf(&t, "%lldh", (long long)h), any = true;
            if (m > 0) sb_appendf(&t, "%s%lldm", any ? " " : "", (long long)m), any = true;
            if (sec > 0 || !any) sb_appendf(&t, "%s%llds", any ? " " : "", (long long)sec);
        }
        out = xasprintf("%s (%s -> %s)", sb_cstr(&t), e, d.action);
        sb_free(&t);
    }
    free(e);
    return out;
}

/* ---------- prompt cache status (session/prompt-cache-status.ts, AgentSession.getPromptCacheStatus) ---------- */

/* isPromptCachePersistent: the `prompt-cache-persistent` axis, set for openai-codex on the provider's own endpoint */
static bool prompt_cache_persistent(const ai_model_t *m) {
    return str_eq(m->provider, "openai-codex") && (!m->base_url || strstr(m->base_url, "chatgpt.com"));
}

void cache_status_free(cache_status_t *st) {
    free(st->reason_from), free(st->reason_to), free(st->warming_reason);
    memset(st, 0, sizeof *st);
}

/* foldBranch: the branch's newest reply that used the prompt cache, with what followed it (scanBranchPromptCache) */
static void scan_branch(cache_warmer_t *w, session_t *s, observation_t *ob) {
    const char *leaf = s->leaf_id;
    if (ob->valid && str_eq(ob->leaf, leaf)) return;
    free(ob->leaf), free(ob->provider), free(ob->model);
    memset(ob, 0, sizeof *ob);
    ob->valid = true;
    ob->leaf = leaf ? xstrdup(leaf) : NULL;
    if (!leaf) return;
    json_t *path = session_path_to(s, leaf), *e;
    json_arr_foreach(path, i, e) {
        const char *t = json_get_str(e, "type");
        if (str_eq(t, "compaction")) {
            if (ob->have) ob->compacted = true;
        } else if (str_eq(t, "reset_boundary")) {
            /* /clear starts a new transcript: nothing before the boundary is a prefix of the next request */
            ob->have = false;
        } else if (str_eq(t, "model_usage")) {
            const json_t *cr = json_get_obj(e, "cacheRefresh");
            if (!cr || !ob->have) continue;
            /* a refresh that wrote the prefix again left it cached from its send, as a hit does */
            bool cached = json_get_bool(cr, "hit", false) || json_get_int(json_get_obj(e, "usage"), "cacheWrite", 0) > 0;
            ob->warmed_at = cached ? json_get_int(cr, "sentAt", 0) : 0;
            ob->refresh_missed = !cached;
        } else if (str_eq(t, "message")) {
            const json_t *m = json_get_obj(e, "message");
            if (!str_eq(json_get_str(m, "role"), "assistant") || str_eq(json_get_str(m, "stopReason"), "error")) continue;
            const json_t *u = json_get_obj(m, "usage");
            if (json_get_int(u, "cacheRead", 0) + json_get_int(u, "cacheWrite", 0) <= 0) continue;
            free(ob->provider), free(ob->model);
            ob->have = true;
            ob->provider = xstrdup(json_get_str(m, "provider") ? json_get_str(m, "provider") : "");
            const char *mid = json_get_str(m, "model");
            /* a server-side fallback retags the reply with the served model; the entry is the requested one's */
            json_t *b;
            json_arr_foreach(json_get_arr(m, "content"), k, b) {
                if (str_eq(json_get_str(b, "type"), "fallback") && json_get_str(json_get_obj(b, "from"), "model"))
                    mid = json_get_str(json_get_obj(b, "from"), "model");
            }
            ob->model = xstrdup(mid ? mid : "");
            ob->timestamp = json_get_int(m, "timestamp", 0);
            ob->prompt_tokens = prompt_tokens_of(u);
            ob->observed_tier = observed_tier(u);
            ob->warmed_at = 0;
            ob->refresh_missed = false;
            ob->compacted = false;
        }
    }
    json_free(path);
    UNUSED(w);
}

bool cachewarm_status(app_t *a, int64_t now, int64_t next_tokens, cache_status_t *out) {
    cache_warmer_t *w = a->warmer;
    memset(out, 0, sizeof *out);
    out->ttl_ms = out->next_warm_at = out->stops_at = -1;
    if (!w) return false;
    agent_t *ag = a->agent;
    /* the next request's shape: what it would send differently from the armed one */
    const ai_model_t *cur = a->model;
    char *cur_key = cur ? xasprintf("%s/%s", cur->provider, cur->id) : xstrdup("");
    uint64_t cur_tools = tools_fingerprint(ag), cur_system = system_fingerprint(ag);
    const char *cur_thinking = effort_name(a->thinking), *cur_retention = settings_str("providers.cacheRetention", "auto");
    const char *sid = a->session ? session_id(a->session) : NULL;
    bool live = false;
    const ai_model_t *model = NULL;
    const char *tier = NULL;
    int64_t touched = 0, prompt_tokens = 0;
    bool refresh_missed = false, heldoff_possible = false;
    int64_t sent_at = 0;
    const char *change = NULL;
    char *from = NULL, *to = NULL;
    pthread_mutex_lock(&w->mu);
    if (w->entry.active && str_eq(w->arm_session, sid)) {
        centry_t *e = &w->entry;
        bool history = false;
        pthread_mutex_lock(&ag->mu);
        size_t cn = json_len(ag->messages);
        if (cn < e->nmsgs) history = true;
        for (size_t i = 0; i < e->nmsgs && !history; i++) history = (const void *)json_at(ag->messages, i) != e->msgs[i];
        pthread_mutex_unlock(&ag->mu);
        /* a replaced transcript (rewind, branch switch, reload, compaction) no longer extends the armed request; the branch now
         * on screen says what its own replies cached */
        if (!history) {
            live = true;
            model = e->model;
            tier = e->tier;
            touched = e->touched_at;
            prompt_tokens = e->prompt_tokens;
            refresh_missed = e->refresh_missed;
            if (strcmp(e->model_key, cur_key)) {
                change = "model";
                from = xstrdup(e->model_name);
                to = xstrdup(cur ? (cur->name ? cur->name : cur->id) : "no model");
            } else if (!str_eq(e->thinking, cur_thinking)) {
                change = "thinking";
                from = xstrdup(e->thinking);
                to = xstrdup(cur_thinking);
            } else if (e->tools != cur_tools) change = "tools";
            else if (e->system != cur_system) change = "system-prompt";
            else if (!str_eq(e->retention, cur_retention)) change = "retention";
            /* warming: formatted the way #statusOf reports it */
            run_t *r = &w->run;
            out->has_warming = true;
            if (str_eq(mode_for(w), "off")) {
                out->warming_state = "inactive";
                out->warming_reason = xstrdup("cache warming disabled");
            } else if (!r->active) {
                out->warming_state = "inactive";
                out->warming_reason = w->reason ? xstrdup(w->reason) : NULL;
            } else if (r->bridging) {
                out->warming_state = "inactive";
                out->warming_reason = xstrdup(BRIDGE_IN_PROGRESS_REASON);
            } else if (change) {
                out->warming_state = "inactive";
                out->warming_reason = xstrdup("conversation context changed");
            } else {
                bool refreshing = !r->armed;
                decision_t d = evaluate(w, r, r->next_warm_at, NULL, NULL);
                if (!d.available && !refreshing) {
                    out->warming_state = "inactive";
                    out->warming_reason = xstrdup("cache economics unavailable");
                } else {
                    sent_at = refreshing ? r->refresh_sent_at : 0;
                    heldoff_possible = sent_at != 0;
                    out->warming_state = sent_at ? "refreshing" : "scheduled";
                    if (!sent_at) out->next_warm_at = r->next_warm_at;
                    out->stops_at = stops_at(r, &d);
                }
            }
        }
    }
    pthread_mutex_unlock(&w->mu);
    if (!live) {
        /* #inferredPromptCacheEntry: thinking level, tools and credential at the time are unknown */
        pthread_mutex_lock(&w->mu);
        if (a->session) scan_branch(w, a->session, &w->obs);
        observation_t ob = w->obs;
        bool have = a->session && ob.have;
        char *prov = have ? xstrdup(ob.provider) : NULL, *mid = have ? xstrdup(ob.model) : NULL;
        pthread_mutex_unlock(&w->mu);
        const ai_model_t *m = have ? catalog_find(prov, mid) : NULL;
        if (!m) {
            free(prov), free(mid), free(cur_key);
            return false;
        }
        bool oauth = str_eq(m->api, "anthropic-messages") && anthropic_model_is_oauth(m, NULL);
        const char *requested = cachewarm_tier(m, str_eq(cur_retention, "auto") ? NULL : cur_retention, oauth);
        /* a reply that used the cache wrote some tier even when the next request asks for none; a provider that reports no split
         * wrote the default one */
        const char *t = ob.observed_tier ? ob.observed_tier : requested ? requested : cachewarm_tier(m, NULL, oauth);
        if (!t) {
            free(prov), free(mid), free(cur_key);
            return false;
        }
        model = m;
        tier = t;
        touched = MAX(ob.timestamp, ob.warmed_at);
        prompt_tokens = ob.prompt_tokens;
        refresh_missed = ob.refresh_missed;
        if (!cur || !str_eq(cur->provider, m->provider) || !str_eq(cur->id, m->id)) {
            change = "model";
            from = xstrdup(m->name ? m->name : m->id);
            to = xstrdup(cur ? (cur->name ? cur->name : cur->id) : "no model");
        } else if (!str_eq(requested, t)) {
            change = "retention"; /* the next request asks for another tier, or for no caching at all */
        } else if (ob.compacted) {
            change = "compaction";
        }
        free(prov), free(mid);
        out->inferred = true;
    }
    free(cur_key);
    int64_t secs = ttl_seconds(model, tier);
    bool persistent = prompt_cache_persistent(model);
    int64_t ttl = persistent || secs < 0 ? -1 : secs * 1000;
    if (!persistent && secs < 0) {
        free(from), free(to);
        cache_status_free(out);
        return false;
    }
    if (persistent) {
        free(out->warming_reason);
        out->warming_reason = NULL;
        out->has_warming = false;
        out->next_warm_at = out->stops_at = -1;
    }
    /* a lapsed lifetime is the reason whenever it holds; a refresh in flight the provider sent before the entry lapsed holds
     * expiry off: it moves the touch once it hits */
    const char *reason = NULL;
    if (ttl >= 0) {
        int64_t expires = touched + ttl;
        bool held = heldoff_possible && sent_at < expires;
        if (now >= expires && !held) {
            reason = "expired";
            out->reason_at = expires;
        }
    }
    if (!reason) reason = change ? change : refresh_missed ? "refresh-missed" : NULL;
    out->cold = reason != NULL;
    out->reason = reason;
    if (reason && (!strcmp(reason, "model") || !strcmp(reason, "thinking"))) {
        out->reason_from = from;
        out->reason_to = to;
    } else {
        free(from), free(to);
    }
    out->tier = tier;
    out->touched_at = touched;
    out->ttl_ms = ttl;
    out->cached_tokens = prompt_tokens;
    out->next_tokens = next_tokens;
    return true;
}
