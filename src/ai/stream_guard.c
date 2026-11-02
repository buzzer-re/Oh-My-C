/* stream_guard.c - per-request stream watchdog and provider in-flight caps around every provider stream.
 *
 * Watchdog (port of omp pi-ai register-builtins.ts forwardStream + utils/idle-iterator.ts and the per-provider
 * timeout wrappers): a request that produces no semantic event (anything but `start`) within the first-event
 * budget, or goes silent longer than the idle budget afterwards, is aborted locally and ends as an error with the
 * provider's timeout message. Budgets come from the request (providers.streamFirstEventTimeoutSeconds /
 * streamIdleTimeoutSeconds), else PI_STREAM_* / PI_OPENAI_STREAM_* env vars, else the model's compat
 * streamIdleTimeoutMs / streamFirstEventTimeoutMs, else 300 s; 0 disables.
 *
 * In-flight caps (port of omp pi-ai stream.ts provider in-flight leases, providers.maxInFlightRequests): each
 * request holds one of N slot files under <config root>/run/provider-inflight/<sha256(provider)>/ (flock), so the
 * cap is shared by every local omc process with that config root; a lock dies with its process. */
#define _GNU_SOURCE
#include "ai.h"
#include "core/fs.h"

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <sys/file.h>
#include <unistd.h>

#define DEFAULT_STREAM_TIMEOUT_MS 300000

/* normalizeIdleTimeoutMs: unset -> fallback, non-numeric -> fallback, <= 0 -> disabled (0) */
static int64_t env_timeout(const char *name, int64_t fallback, bool *set) {
    const char *v = getenv(name);
    if (set) *set = v != NULL;
    if (!v) return fallback;
    char *end;
    double d = strtod(v, &end);
    if (end == v || !isfinite(d)) return fallback;
    return d <= 0 ? 0 : (int64_t)d;
}

static double numv(const json_t *j) { return j->is_int ? (double)j->u.num.i : j->u.num.d; }

static int64_t compat_ms(const ai_model_t *m, const char *key) {
    json_t *v = model_compat_get(m, key);
    return json_is_num(v) && numv(v) > 0 ? (int64_t)numv(v) : -1;
}

typedef struct {
    const char *first_msg, *idle_msg;
    bool openai_env;     /* OpenAI-family env precedence (PI_OPENAI_STREAM_*) */
    bool provider_first; /* the provider owns first-event detection (Cloud Code Assist) */
    bool none;           /* no shared watchdog (provider-owned idle handling, local tool bridges) */
} limits_t;

static limits_t limits_for(const char *api) {
    limits_t l = {"Provider stream timed out while waiting for the first event",
                  "Provider stream stalled while waiting for the next event", false, false, false};
    if (str_eq(api, "anthropic-messages")) {
        l.first_msg = "Anthropic stream timed out while waiting for the first event";
        l.idle_msg = "Anthropic stream stalled while waiting for the next event";
    } else if (str_eq(api, "openai-completions")) {
        l.first_msg = "OpenAI completions stream timed out while waiting for the first event";
        l.idle_msg = "OpenAI completions stream stalled while waiting for the next event";
        l.openai_env = true;
    } else if (str_eq(api, "openai-responses")) {
        l.first_msg = "OpenAI responses stream timed out while waiting for the first event";
        l.idle_msg = "OpenAI responses stream stalled while waiting for the next event";
        l.openai_env = true;
    } else if (str_eq(api, "azure-openai-responses")) {
        l.first_msg = "Azure OpenAI responses stream timed out while waiting for the first event";
        l.idle_msg = "Azure OpenAI responses stream stalled while waiting for the next event";
        l.openai_env = true;
    } else if (str_eq(api, "openai-codex-responses")) {
        l.first_msg = "OpenAI Codex SSE stream timed out while waiting for the first event";
        l.idle_msg = "OpenAI Codex SSE stream stalled while waiting for the next event";
        l.openai_env = true;
    } else if (str_eq(api, "ollama-chat") || str_eq(api, "ollama")) {
        l.openai_env = true;
    } else if (str_eq(api, "google-gemini-cli")) {
        l.provider_first = true;
    } else if (str_eq(api, "cursor-agent") || str_starts(api ? api : "", "gitlab-duo")) {
        l.none = true;
    }
    return l;
}

/* resolved budgets (0 = disabled) */
static void resolve_budgets(const ai_model_t *m, const ai_stream_opts_t *o, const limits_t *l, int64_t *first, int64_t *idle) {
    int64_t compat_idle = compat_ms(m, "streamIdleTimeoutMs");
    int64_t idle_fb = compat_idle > 0 ? compat_idle : DEFAULT_STREAM_TIMEOUT_MS;
    if (o->idle_timeout_ms >= 0) *idle = o->idle_timeout_ms;
    else if (l->openai_env) {
        const char *v = getenv("PI_OPENAI_STREAM_IDLE_TIMEOUT_MS") ? "PI_OPENAI_STREAM_IDLE_TIMEOUT_MS" : "PI_STREAM_IDLE_TIMEOUT_MS";
        *idle = env_timeout(v, idle_fb, NULL);
    } else {
        const char *v = getenv("PI_STREAM_IDLE_TIMEOUT_MS") ? "PI_STREAM_IDLE_TIMEOUT_MS" : "PI_OPENAI_STREAM_IDLE_TIMEOUT_MS";
        *idle = env_timeout(v, idle_fb, NULL);
    }
    if (l->provider_first) {
        *first = 0;
        return;
    }
    if (o->first_event_timeout_ms >= 0) {
        *first = o->first_event_timeout_ms;
        return;
    }
    int64_t compat_first = compat_ms(m, "streamFirstEventTimeoutMs");
    int64_t first_fb = compat_first > 0 ? compat_first : DEFAULT_STREAM_TIMEOUT_MS;
    if (l->openai_env) { /* getOpenAIStreamFirstEventTimeoutMs */
        bool set;
        int64_t v = env_timeout("PI_OPENAI_STREAM_FIRST_EVENT_TIMEOUT_MS", first_fb, &set);
        if (set) {
            *first = v;
            return;
        }
        int64_t base = env_timeout("PI_STREAM_FIRST_EVENT_TIMEOUT_MS", first_fb, NULL);
        *first = base <= 0 ? 0 : *idle <= 0 ? base : MAX(base, *idle);
        return;
    }
    /* getStreamFirstEventTimeoutMs: the fallback never undershoots the idle budget */
    int64_t fb = *idle > 0 ? MAX(first_fb, *idle) : first_fb;
    *first = env_timeout("PI_STREAM_FIRST_EVENT_TIMEOUT_MS", fb, NULL);
}

/* ---------- watchdog ---------- */

typedef struct {
    ai_event_cb cb;
    void *ud;
    const ai_model_t *model;
    volatile sig_atomic_t local_cancel;
    volatile sig_atomic_t *caller_cancel;
    int64_t first_ms, idle_ms;
    int64_t started, last;
    bool progressed;
    int fired; /* 1 first-event, 2 idle */
    const char *msg, *first_msg, *idle_msg;
    bool done;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} watch_t;

/* the request the watchdog cancelled ends as an error that keeps what streamed so far (omp: the partial output
 * stays on the failed turn, so turn recovery can continue after committed text) */
static json_t *stalled_message(const ai_model_t *m, const json_t *partial, const char *msg) {
    if (!json_is_obj(partial)) return ai_error_message(m, "error", msg);
    json_t *r = json_clone(partial);
    json_set_str(r, "stopReason", "error");
    json_set_str(r, "errorMessage", msg);
    return r;
}

static void watch_cb(void *ud, const ai_event_t *ev) {
    watch_t *w = ud;
    pthread_mutex_lock(&w->mu);
    if (ev->type != AI_EV_START) {
        w->progressed = true;
        w->last = mono_ms();
    }
    int fired = w->fired;
    pthread_mutex_unlock(&w->mu);
    if (fired && (ev->type == AI_EV_ERROR || ev->type == AI_EV_DONE)) {
        json_t *m = stalled_message(w->model, ev->partial, w->msg);
        ai_event_t e2 = {.type = AI_EV_ERROR, .content_index = -1, .partial = m, .reason = "error"};
        if (w->cb) w->cb(w->ud, &e2);
        json_free(m);
        return;
    }
    if (w->cb) w->cb(w->ud, ev);
}

static void *watch_main(void *arg) {
    watch_t *w = arg;
    pthread_mutex_lock(&w->mu);
    while (!w->done) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
        pthread_cond_timedwait(&w->cv, &w->mu, &ts);
        if (w->done) break;
        if (w->caller_cancel && *w->caller_cancel) w->local_cancel = 1;
        if (w->fired || w->local_cancel) continue;
        int64_t now = mono_ms();
        if (!w->progressed && w->first_ms > 0 && now - w->started >= w->first_ms) w->fired = 1;
        else if (w->progressed && w->idle_ms > 0 && now - w->last >= w->idle_ms) w->fired = 2;
        if (w->fired) {
            w->msg = w->fired == 1 ? w->first_msg : w->idle_msg;
            w->local_cancel = 1;
        }
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

/* ---------- in-flight slots ---------- */

static const json_t *g_max_in_flight; /* process default (settings), borrowed */

void ai_set_max_in_flight(const json_t *limits) { g_max_in_flight = limits; }

static int64_t in_flight_limit(const char *provider, const ai_stream_opts_t *o) {
    const json_t *lim = o->max_in_flight ? o->max_in_flight : g_max_in_flight;
    json_t *v = json_is_obj(lim) && provider ? json_get(lim, provider) : NULL;
    if (!json_is_num(v) || !isfinite(numv(v)) || numv(v) <= 0) return 0;
    return MAX((int64_t)1, (int64_t)floor(numv(v)));
}

static char *slot_dir(const char *provider) {
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)provider, strlen(provider), d);
    char *seg = base64url_encode(d, sizeof d);
    char *root = path_join(omc_config_dir(), "run/provider-inflight");
    char *dir = path_join(root, seg);
    free(root);
    free(seg);
    return dir;
}

/* fd of the held slot, -1 when unlimited or unavailable, -2 when cancelled while waiting */
static int acquire_slot(const char *provider, int64_t limit, volatile sig_atomic_t *cancel) {
    char *dir = slot_dir(provider);
    if (fs_mkdir_p(dir, 0700) != 0) {
        free(dir);
        return -1; /* cannot coordinate: do not block the request */
    }
    int fd = -1;
    for (;;) {
        for (int64_t i = 0; i < limit && fd < 0; i++) {
            char name[32];
            snprintf(name, sizeof name, "slot-%lld", (long long)i);
            char *p = path_join(dir, name);
            int f = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            free(p);
            if (f < 0) continue;
            if (flock(f, LOCK_EX | LOCK_NB) == 0) fd = f;
            else close(f);
        }
        if (fd >= 0) break;
        if (cancel && *cancel) {
            fd = -2;
            break;
        }
        sleep_ms(250);
    }
    free(dir);
    return fd;
}

/* ---------- entry ---------- */

json_t *ai_stream_guarded(ai_stream_fn fn, const ai_model_t *m, const ai_context_t *ctx, ai_stream_opts_t *o, ai_event_cb cb,
                          void *ud) {
    int slot = -1;
    int64_t limit = in_flight_limit(m->provider, o);
    if (limit > 0) {
        slot = acquire_slot(m->provider, limit, o->cancel);
        if (slot == -2) {
            json_t *msg = ai_error_message(m, "aborted", "Request was aborted");
            if (cb) {
                ai_event_t ev = {.type = AI_EV_ERROR, .content_index = -1, .partial = msg, .reason = "aborted"};
                cb(ud, &ev);
            }
            return msg;
        }
    }
    limits_t l = limits_for(m->api);
    int64_t first = 0, idle = 0;
    if (!l.none) resolve_budgets(m, o, &l, &first, &idle);
    /* the byte-level HTTP read timeout must never undercut the watchdog; a provider that owns first-event
     * detection keeps the request option */
    if (!l.provider_first && !l.none) o->first_event_timeout_ms = first;
    if (!l.none) o->idle_timeout_ms = idle;
    json_t *r;
    if (first <= 0 && idle <= 0) {
        r = fn(m, ctx, o, cb, ud);
    } else {
        watch_t w = {.cb = cb, .ud = ud, .model = m, .caller_cancel = o->cancel, .first_ms = first, .idle_ms = idle,
                     .first_msg = l.first_msg, .idle_msg = l.idle_msg};
        w.started = w.last = mono_ms();
        pthread_mutex_init(&w.mu, NULL);
        pthread_cond_init(&w.cv, NULL);
        pthread_t th;
        bool threaded = pthread_create(&th, NULL, watch_main, &w) == 0;
        ai_stream_opts_t o2 = *o;
        if (threaded) o2.cancel = &w.local_cancel;
        r = fn(m, ctx, &o2, watch_cb, &w);
        pthread_mutex_lock(&w.mu);
        w.done = true;
        pthread_cond_signal(&w.cv);
        int fired = w.fired;
        pthread_mutex_unlock(&w.mu);
        if (threaded) pthread_join(th, NULL);
        pthread_cond_destroy(&w.cv);
        pthread_mutex_destroy(&w.mu);
        if (fired && !(o->cancel && *o->cancel)) {
            json_t *e = stalled_message(m, r, fired == 1 ? l.first_msg : l.idle_msg);
            json_free(r);
            r = e;
        }
    }
    if (slot >= 0) {
        flock(slot, LOCK_UN);
        close(slot);
    }
    return r;
}
