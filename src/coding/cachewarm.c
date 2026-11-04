/* cachewarm.c - prompt cache warmer. See cachewarm.h. */
#include "coding/cachewarm.h"
#include "coding/app.h"

#include "ai/provider_anthropic.h"
#include "config/settings.h"

#include <math.h>
#include <pthread.h>
#include <time.h>

#define MAX_WARMING_AGE_MS (60LL * 60000)
#define MAX_IDLE_WARMING_AGE_MS (30LL * 60000)
#define MIN_EXPECTED_SAVINGS 0.05
#define IDLE_CONTINUATION_PROBABILITY 0.15

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
} run_t;

typedef struct {
    bool idle, available, warm;
    double warm_cost, miss_cost, probability, savings;
} decision_t;

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
};

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

static const char *mode(void) {
    const char *m = settings_str("providers.cacheWarming", "idle");
    return str_eq(m, "off") || str_eq(m, "streaming") ? m : "idle";
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

static void schedule_locked(cache_warmer_t *w) {
    run_t *r = &w->run;
    r->next_warm_at = wall_ms() + r->delay_ms;
    const char *reason = r->idle ? idle_stop_reason(r)
                                 : r->next_warm_at > r->started_at + MAX_WARMING_AGE_MS ? "one-hour safety limit reached" : NULL;
    if (reason) {
        stop_locked(w, reason, NULL);
        return;
    }
    r->armed = true;
    LOGD("cache warming: refresh in %lldms (%s phase)", (long long)r->delay_ms, r->idle ? "idle" : "streaming");
    pthread_cond_broadcast(&w->cv);
}

static const char *mode_stop_reason(run_t *r) {
    const char *m = mode();
    if (str_eq(m, "off")) return "cache warming disabled";
    if (str_eq(m, "streaming") && r->idle) return "agent run settled";
    return NULL;
}

static decision_t evaluate(cache_warmer_t *w, run_t *r) {
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
    double miss = m->cost_cache_write > 0 ? price(m, 0, 0, 0, tokens) : price(m, tokens, 0, 0, 0);
    d.warm_cost = price(m, 0, 1, tokens, 0);
    d.miss_cost = MAX(0.0, miss - hit);
    d.probability = r->idle ? IDLE_CONTINUATION_PROBABILITY : 1;
    d.available = tokens > 0 && (hit > 0 || miss > 0);
    d.savings = d.probability * d.miss_cost - d.warm_cost;
    d.warm = d.savings >= MIN_EXPECTED_SAVINGS;
    return d;
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
    pthread_mutex_lock(&w->mu);
    run_clear(w);
    if (str_eq(mode(), "off")) set_reason(w, "cache warming disabled", NULL);
    else if (!cachewarm_replayable(m, o)) set_reason(w, "request cannot be replayed safely", NULL);
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
        schedule_locked(w);
    }
    pthread_mutex_unlock(&w->mu);
}

void cachewarm_settled(cache_warmer_t *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    run_t *r = &w->run;
    if (r->active) {
        if (str_eq(mode(), "streaming")) stop_locked(w, "agent run settled", NULL);
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
        const char *reason = mode_stop_reason(&w->run);
        if (reason) stop_locked(w, reason, NULL);
    }
    pthread_mutex_unlock(&w->mu);
}

void cachewarm_cancel(cache_warmer_t *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    stop_locked(w, "inactive", NULL);
    pthread_mutex_unlock(&w->mu);
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

/* #replay: one output token; owned message (usage of the accepted request) or NULL */
static json_t *replay(run_t *r, volatile sig_atomic_t *cancel) {
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
    o.max_tokens = 1;
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

static void record_usage(cache_warmer_t *w, const json_t *msg) {
    json_t *e = json_obj();
    json_set_str(e, "purpose", "cache-warm");
    json_set_str(e, "api", json_get_str(msg, "api"));
    json_set_str(e, "provider", json_get_str(msg, "provider"));
    json_set_str(e, "model", json_get_str(msg, "model"));
    json_set(e, "usage", json_clone(json_get(msg, "usage")));
    json_set_str(e, "stopReason", json_get_str(msg, "stopReason"));
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

/* #refresh, on the warmer thread with w->mu held on entry and exit */
static void refresh_locked(cache_warmer_t *w) {
    run_t *r = &w->run;
    r->armed = false;
    uint64_t gen = r->gen;
    const char *reason = mode_stop_reason(r);
    if (!reason) {
        pthread_mutex_unlock(&w->mu);
        bool current = run_current(w, r);
        pthread_mutex_lock(&w->mu);
        if (!r->active || r->gen != gen) return;
        if (!current) reason = "conversation context changed";
    }
    if (reason) {
        stop_locked(w, reason, NULL);
        return;
    }
    decision_t d = evaluate(w, r);
    if (!d.warm) {
        stop_locked(w, d.available ? "expected savings below threshold" : "cache economics unavailable", &d);
        return;
    }
    w->refresh_cancel = 0;
    run_t req;
    run_copy(&req, r);
    pthread_mutex_unlock(&w->mu);
    /* onRefreshStart / onRefreshEnd: RPC cache_warming_start / cache_warming_end session events */
    json_t *ev = json_obj();
    json_set_str(ev, "type", "cache_warming_start");
    json_set_str(ev, "phase", req.idle ? "idle" : "streaming");
    json_set_str(ev, "provider", req.model->provider);
    json_set_str(ev, "model", req.model->id);
    emit_event(w, ev);
    json_t *msg = replay(&req, &w->refresh_cancel);
    run_free(&req);
    pthread_mutex_lock(&w->mu);
    const char *outcome = "error";
    /* a refresh cancelled or replaced after acceptance was still billed */
    if (msg && json_get_int(json_get(msg, "usage"), "totalTokens", 0) > 0) {
        record_usage(w, msg);
        json_set(ev, "usage", json_clone(json_get(msg, "usage")));
    }
    const char *stop = NULL;
    if (!r->active || r->gen != gen) {
        outcome = "aborted";
    } else if (!msg || str_eq(json_get_str(msg, "stopReason"), "error")) {
        stop = "refresh failed";
    } else {
        json_t *u = json_get(msg, "usage");
        if (json_get_int(u, "cacheRead", 0) <= 0 || json_get_int(u, "cacheWrite", 0) > 0) {
            outcome = "miss";
            stop = "refresh missed the cache";
        } else {
            outcome = "hit";
            stop = mode_stop_reason(r);
        }
    }
    json_free(msg);
    if (stop) stop_locked(w, stop, NULL);
    else if (!strcmp(outcome, "hit")) {
        schedule_locked(w);
        if (!w->run.active) stop = w->reason; /* a safety window ended warming */
    }
    json_set_str(ev, "type", "cache_warming_end");
    json_set_str(ev, "outcome", outcome);
    if (stop) json_set_str(ev, "warmingStopReason", stop);
    pthread_mutex_unlock(&w->mu);
    emit_event(w, ev);
    pthread_mutex_lock(&w->mu);
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
        refresh_locked(w);
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

cache_warmer_t *cachewarm_new(app_t *a) {
    cache_warmer_t *w = xcalloc(1, sizeof *w);
    w->a = a;
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
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    free(w);
}

/* ---------- status (formatCacheWarmingStatus) ---------- */

static char *dollars(double v) { return v < 0 ? xasprintf("-$%.3f", fabs(v)) : xasprintf("$%.3f", v); }

static char *economics(const decision_t *d) {
    if (!d->available) return xstrdup("cache economics unavailable");
    long p = lround(d->probability * 100);
    char *s = dollars(d->savings);
    char *r = xasprintf("%ld%% continuation probability%s, expected savings %s %s $%.3f", p,
                        d->idle ? "" : " while agent is running", s, d->warm ? ">=" : "<", MIN_EXPECTED_SAVINGS);
    free(s);
    return r;
}

char *cachewarm_status_text(cache_warmer_t *w) {
    if (!w) return xstrdup("Inactive (unknown reason)");
    if (str_eq(mode(), "off")) return xstrdup("Inactive (cache warming disabled)");
    pthread_mutex_lock(&w->mu);
    run_t *r = &w->run;
    char *out;
    if (!r->active) {
        if (w->have_stop_decision && w->stop_decision.available) {
            char *e = economics(&w->stop_decision);
            out = xasprintf("Stopped (%s -> %s)", e, w->stop_decision.warm ? "warm" : "stop");
            free(e);
        } else {
            out = xasprintf("Inactive (%s)", w->reason ? w->reason : "unknown reason");
        }
        pthread_mutex_unlock(&w->mu);
        return out;
    }
    run_t snap = *r;
    pthread_mutex_unlock(&w->mu);
    if (!run_current(w, &snap)) return xstrdup("Inactive (conversation context changed)");
    decision_t d = evaluate(w, &snap);
    if (!d.available && snap.armed) return xstrdup("Inactive (cache economics unavailable)");
    char *e = economics(&d);
    if (!snap.armed) {
        out = xasprintf("Warming cache (%s -> %s)", e, d.warm ? "warm" : "stop");
    } else {
        int64_t now = wall_ms();
        sb_t t = SB_INIT;
        if (snap.next_warm_at <= now) sb_append(&t, "Decision now");
        else {
            int64_t s = (snap.next_warm_at - now + 999) / 1000;
            int64_t h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
            sb_append(&t, "Decision in ");
            bool any = false;
            if (h > 0) sb_appendf(&t, "%lldh", (long long)h), any = true;
            if (m > 0) sb_appendf(&t, "%s%lldm", any ? " " : "", (long long)m), any = true;
            if (sec > 0 || !any) sb_appendf(&t, "%s%llds", any ? " " : "", (long long)sec);
        }
        out = xasprintf("%s (%s -> %s)", sb_cstr(&t), e, d.warm ? "warm" : "stop");
        sb_free(&t);
    }
    free(e);
    return out;
}
