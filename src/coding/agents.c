/* agents.c - agent registry, task spawns, peer messaging, agent:// and history:// (see agents.h). */
#include "agents.h"

#include "app.h"
#include "config/settings.h"
#include "core/fs.h"
#include "prompt/template.h"
#include "session/session.h"
#include "tools/artifacts.h"
#include "tools/jobs.h"
#include "tools/tools.h"
#include "coding/jobcaption.h"

#include <ctype.h>
#include <pthread.h>

typedef struct {
    char *to;
    bool promised;   /* progress arrived: only an explicit message answers now */
    int64_t sent_at; /* mono_ms */
} owed_t;

typedef struct aref {
    char *id, *agent, *parent_id;
    app_t *app;     /* live session (owned for subagents, borrowed for Main) */
    app_t *spawner; /* session that spawned it (its jobs receive the results) */
    bool owned, running, aborted, gone, is_main;
    /* the session is routable but its owner has not prompted the assignment yet: an IRC message must not wake a turn
     * without it. The parent's ride the assignment's turn; a peer's keep a wake turn of their own after the run. */
    bool awaiting_assignment;
    strvec_t early_asides; /* rendered IRC texts */
    strvec_t early_peers;  /* from, body pairs */
    char *output;
    bool has_output;
    int64_t started_at, published_at, run_started;
    json_t *transcript; /* retained messages after the session is released */
    json_t *inbox;      /* IrcMessage rows queued for the wait tool */
    strvec_t pending;   /* rendered IRC texts for an idle top-level session */
    VEC(owed_t) owed;   /* agents this one messaged directly and that have not answered (IrcBus.unansweredSends) */
    strvec_t sent_run;  /* agents this one sent anything this run, progress included (IrcBus.sentSince) */
    int waiting;
    strvec_t wait_asked;   /* with `waiting`: agents whose broadcast answers the wait */
    strvec_t wait_watched; /* with `waiting`: the interruptible awaits hold of a wait only these agents' results end */
    int64_t wait_since;
    int user_waits;        /* open dialogs waiting on the user (USER_AWAIT_ID hold) */
    char *activity;
    char *job_id;
    volatile sig_atomic_t *cancel; /* running job's cancel flag */
    task_card_t *card;             /* progress card of the spawning task call (first run only) */
    int card_idx;
    /* Agent Hub / RPC observability (omp AgentRef + observer AgentProgress) */
    json_t *progress;      /* AgentProgress of the latest executor run (owned) */
    char *description;     /* spawn description (NULL = none) */
    char *parent_call;     /* spawning task call id */
    char *sys_append;      /* system prompt append the session was built with (revive) */
    char *model;           /* resolved "provider/id" */
    char *recent_tail;     /* streamed text tail behind progress.recentOutput */
    int index;             /* spawn index within its task call */
    int64_t last_activity; /* ms */
    int64_t active_ms;     /* executor run time of finished runs */
    bool killed;           /* released as an `aborted` tombstone (hub x / cancel_subagent) */
    bool executor_run;     /* the current run is an executor run (task spawn / IRC wake): progress tracks it */
    bool lifecycle_open;   /* a `started` lifecycle frame went out without its terminal one */
    char *gist;            /* hub activity: latest intent or `running <tool>` of the current run */
    char *session_file;    /* persisted transcript `<artifacts>/<id>.jsonl` (NULL: in memory only) */
    char *output_path;     /* published output artifact `<artifacts>/<id>.md` (NULL: none written yet) */
    bool read_only;        /* its definition is read-only (hub "Read-only · 0 LoC") */
    json_t *model_override; /* progress.modelOverride: the spawn's model patterns (agent definition or request) */
    int pins;              /* a UI view holds `app` (session focus): never freed underneath it */
} aref_t;

/* ---------- task cards ---------- */

struct task_card {
    pthread_mutex_t mu;
    int refs;
    tool_env_t *env;
    char *call_id;
    json_t *args;
    json_t *details;
    int64_t started_at;
    int nspawn, settled, failed;
};

task_card_t *task_card_new(tool_env_t *env, const char *tool_call_id, const json_t *args, json_t *details) {
    task_card_t *c = xcalloc(1, sizeof *c);
    pthread_mutex_init(&c->mu, NULL);
    c->refs = 1;
    c->env = env;
    c->call_id = xstrdup(tool_call_id ? tool_call_id : "");
    c->args = args ? json_clone(args) : json_obj();
    c->details = details;
    c->started_at = now_ms();
    c->nspawn = (int)json_len(json_get_arr(details, "progress"));
    return c;
}

static task_card_t *card_retain(task_card_t *c) {
    if (!c) return NULL;
    pthread_mutex_lock(&c->mu);
    c->refs++;
    pthread_mutex_unlock(&c->mu);
    return c;
}

void task_card_release(task_card_t *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    bool last = --c->refs == 0;
    pthread_mutex_unlock(&c->mu);
    if (!last) return;
    pthread_mutex_destroy(&c->mu);
    json_free(c->details);
    json_free(c->args);
    free(c->call_id);
    free(c);
}

json_t *task_card_snapshot(task_card_t *c) {
    pthread_mutex_lock(&c->mu);
    json_t *d = json_clone(c->details);
    pthread_mutex_unlock(&c->mu);
    return d;
}

/* push the current snapshot with its progress text (omp reportProgress / onProgress) to the UI or the RPC
 * stream (called without c->mu held) */
static void card_emit(task_card_t *c, const char *text) {
    tool_env_t *env = c->env;
    if (!env || !env->ui.tool_frame) return;
    env->ui.tool_frame(env->ui.ud, c->call_id, "task", c->args, text, task_card_snapshot(c));
}

static json_t *card_row_locked(task_card_t *c, int idx) { return json_at(json_get_arr(c->details, "progress"), (size_t)idx); }

void task_card_set_id(task_card_t *c, int index, const char *agent_id, bool primary) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    json_t *row = card_row_locked(c, index);
    if (row) json_set_str(row, "id", agent_id);
    if (primary) json_set_str(json_get_obj(c->details, "async"), "jobId", agent_id);
    pthread_mutex_unlock(&c->mu);
}

void task_card_settle(task_card_t *c, int index, const char *status) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    json_t *row = card_row_locked(c, index);
    if (row) {
        json_set_str(row, "status", status);
        json_del(row, "currentTool");
        json_del(row, "currentToolArgs");
        json_del(row, "currentToolIntent");
        json_del(row, "currentToolStartMs");
    }
    c->settled++;
    if (!str_eq(status, "completed")) c->failed++;
    json_set_int(c->details, "totalDurationMs", now_ms() - c->started_at);
    if (c->settled >= c->nspawn)
        json_set_str(json_get_obj(c->details, "async"), "state", c->failed ? "failed" : "completed");
    pthread_mutex_unlock(&c->mu);
}

/* one subagent event folded into an AgentProgress row (task card row or the registry's copy); true when it changed */
static bool row_on_event(json_t *row, int64_t started, const agent_event_t *ev, int64_t context_window) {
    bool changed = false;
    {
        if (ev->type == AGENT_EV_TOOL_START && ev->tool_name) {
            json_set_str(row, "currentTool", ev->tool_name);
            static const char *const KEYS[] = {"path", "command", "pattern", "query", "url", "file", "op"};
            const char *argv = NULL;
            for (size_t k = 0; k < ARRAY_LEN(KEYS) && !argv; k++) argv = json_get_str(ev->args, KEYS[k]);
            if (argv) json_set_str(row, "currentToolArgs", argv);
            else json_del(row, "currentToolArgs");
            if (ev->intent) json_set_str(row, "currentToolIntent", ev->intent);
            else json_del(row, "currentToolIntent");
            if (ev->intent) json_set_str(row, "lastIntent", ev->intent);
            json_set_int(row, "currentToolStartMs", now_ms());
            json_set_int(row, "toolCount", json_get_int(row, "toolCount", 0) + 1);
            changed = true;
        } else if (ev->type == AGENT_EV_TOOL_END && ev->tool_name) {
            json_t *recent = json_get_arr(row, "recentTools");
            json_t *e = json_obj();
            json_set_str(e, "tool", ev->tool_name);
            if (json_get_str(row, "currentToolArgs")) json_set_str(e, "args", json_get_str(row, "currentToolArgs"));
            if (json_get_str(row, "currentToolIntent")) json_set_str(e, "intent", json_get_str(row, "currentToolIntent"));
            if (ev->is_error) json_set_bool(e, "isError", true);
            json_set_int(e, "endMs", now_ms());
            json_t *nr = json_arr();
            json_push(nr, e);
            json_t *x;
            json_arr_foreach(recent, i, x) if (i < 4) json_push(nr, json_clone(x));
            json_set(row, "recentTools", nr);
            json_del(row, "currentTool");
            json_del(row, "currentToolArgs");
            json_del(row, "currentToolIntent");
            json_del(row, "currentToolStartMs");
            changed = true;
        } else if (ev->type == AGENT_EV_MESSAGE_END && str_eq(json_get_str(ev->message, "role"), "assistant")) {
            const json_t *u = json_get_obj(ev->message, "usage");
            json_set_int(row, "requests", json_get_int(row, "requests", 0) + 1);
            int64_t tot = json_get_int(u, "totalTokens", 0);
            if (!tot)
                tot = json_get_int(u, "input", 0) + json_get_int(u, "output", 0) + json_get_int(u, "cacheRead", 0) +
                      json_get_int(u, "cacheWrite", 0);
            json_set_int(row, "tokens", json_get_int(row, "tokens", 0) + tot);
            /* usageContextTokens (omp c7588c9257, 7162cde3a8): the occupied context, as compaction counts it: a
             * reported contextTokens, else the billable total minus provider-side orchestration */
            int64_t occupied;
            if (json_get(u, "contextTokens")) occupied = MAX(0, json_get_int(u, "contextTokens", 0));
            else {
                const json_t *orc = json_get_obj(u, "orchestration");
                int64_t ot = orc ? json_get_int(orc, "input", 0) + json_get_int(orc, "output", 0) + json_get_int(orc, "cacheRead", 0) : 0;
                occupied = MAX(0, tot - ot);
            }
            if (occupied > 0) json_set_int(row, "contextTokens", occupied);
            if (context_window > 0) json_set_int(row, "contextWindow", context_window);
            double cost = json_get_num(json_get_obj(u, "cost"), "total", 0);
            json_set_num(row, "cost", json_get_num(row, "cost", 0) + cost);
            changed = true;
        }
        if (changed && started) json_set_int(row, "durationMs", now_ms() - started);
    }
    return changed;
}

static bool card_on_event(task_card_t *c, int idx, int64_t started, const agent_event_t *ev, int64_t context_window) {
    pthread_mutex_lock(&c->mu);
    json_t *row = card_row_locked(c, idx);
    bool changed = row && row_on_event(row, started, ev, context_window);
    pthread_mutex_unlock(&c->mu);
    return changed;
}

static void card_set_status(task_card_t *c, int idx, const char *status) {
    pthread_mutex_lock(&c->mu);
    json_t *row = card_row_locked(c, idx);
    if (row) json_set_str(row, "status", status);
    pthread_mutex_unlock(&c->mu);
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static VEC(aref_t *) g_refs;
static uint64_t g_msg_seq;
static int g_permits_used;

/* ---------- change listeners (Agent Hub, status badge) and observers (RPC subagent frames) ---------- */

typedef struct {
    agents_change_fn fn;
    void *ud;
} listener_t;
typedef struct {
    agents_observe_fn fn;
    void *ud;
    bool events;
} observer_t;
static pthread_mutex_t g_lmu = PTHREAD_MUTEX_INITIALIZER; /* guards the two lists; never held with g_mu */
static VEC(listener_t) g_listeners;
static VEC(observer_t) g_observers;
static volatile int g_event_observers;

void agents_listen(agents_change_fn fn, void *ud) {
    pthread_mutex_lock(&g_lmu);
    vec_push(&g_listeners, ((listener_t){fn, ud}));
    pthread_mutex_unlock(&g_lmu);
}

void agents_unlisten(agents_change_fn fn, void *ud) {
    pthread_mutex_lock(&g_lmu);
    for (size_t i = 0; i < g_listeners.n; i++)
        if (g_listeners.v[i].fn == fn && g_listeners.v[i].ud == ud) {
            g_listeners.v[i] = g_listeners.v[--g_listeners.n];
            break;
        }
    pthread_mutex_unlock(&g_lmu);
}

void agents_observe(agents_observe_fn fn, void *ud, bool events) {
    pthread_mutex_lock(&g_lmu);
    for (size_t i = 0; i < g_observers.n; i++)
        if (g_observers.v[i].fn == fn && g_observers.v[i].ud == ud) {
            if (g_observers.v[i].events != events) g_event_observers += events ? 1 : -1;
            g_observers.v[i].events = events;
            pthread_mutex_unlock(&g_lmu);
            return;
        }
    vec_push(&g_observers, ((observer_t){fn, ud, events}));
    if (events) g_event_observers++;
    pthread_mutex_unlock(&g_lmu);
}

void agents_unobserve(agents_observe_fn fn, void *ud) {
    pthread_mutex_lock(&g_lmu);
    for (size_t i = 0; i < g_observers.n; i++)
        if (g_observers.v[i].fn == fn && g_observers.v[i].ud == ud) {
            if (g_observers.v[i].events) g_event_observers--;
            g_observers.v[i] = g_observers.v[--g_observers.n];
            break;
        }
    pthread_mutex_unlock(&g_lmu);
}

/* called without g_mu held */
static void notify_change(void) {
    pthread_mutex_lock(&g_lmu);
    size_t n = g_listeners.n;
    listener_t *copy = n ? xmalloc(n * sizeof *copy) : NULL;
    if (n) memcpy(copy, g_listeners.v, n * sizeof *copy);
    pthread_mutex_unlock(&g_lmu);
    for (size_t i = 0; i < n; i++) copy[i].fn(copy[i].ud);
    free(copy);
}

/* hand `payload` (owned) to every observer of `channel` ("lifecycle", "progress", "event"); without g_mu */
static void emit_obs(const char *channel, json_t *payload) {
    if (!payload) return;
    bool ev = str_eq(channel, "event");
    pthread_mutex_lock(&g_lmu);
    size_t n = g_observers.n;
    observer_t *copy = n ? xmalloc(n * sizeof *copy) : NULL;
    if (n) memcpy(copy, g_observers.v, n * sizeof *copy);
    pthread_mutex_unlock(&g_lmu);
    for (size_t i = 0; i < n; i++)
        if (!ev || copy[i].events) copy[i].fn(copy[i].ud, channel, payload);
    free(copy);
    json_free(payload);
}

static bool has_observers(void) {
    pthread_mutex_lock(&g_lmu);
    bool any = g_observers.n > 0;
    pthread_mutex_unlock(&g_lmu);
    return any;
}

static const char *agent_source_of(const aref_t *r) {
    const char *s = json_get_str(r->progress, "agentSource");
    return s ? s : "bundled";
}

/* SubagentLifecyclePayload */
static json_t *lifecycle_payload_locked(const aref_t *r, const char *status) {
    json_t *p = json_obj();
    json_set_str(p, "id", r->id); /* omp executor key order */
    json_set_str(p, "agent", r->agent);
    if (r->parent_call) json_set_str(p, "parentToolCallId", r->parent_call);
    json_set_bool(p, "detached", true);
    json_set_str(p, "agentSource", agent_source_of(r));
    if (r->description) json_set_str(p, "description", r->description);
    json_set_str(p, "status", status);
    if (r->session_file) json_set_str(p, "sessionFile", r->session_file);
    json_set_int(p, "index", r->index);
    return p;
}

/* SubagentProgressPayload */
static json_t *progress_payload_locked(const aref_t *r) {
    json_t *p = json_obj();
    json_set_int(p, "index", r->index);
    json_set_str(p, "agent", r->agent);
    json_set_str(p, "agentSource", agent_source_of(r));
    json_set_str(p, "task", json_get_str(r->progress, "task") ? json_get_str(r->progress, "task") : "");
    if (r->parent_call) json_set_str(p, "parentToolCallId", r->parent_call);
    json_set_bool(p, "detached", true);
    if (json_get_str(r->progress, "assignment")) json_set_str(p, "assignment", json_get_str(r->progress, "assignment"));
    json_set(p, "progress", json_clone(r->progress));
    if (r->session_file) json_set_str(p, "sessionFile", r->session_file);
    return p;
}

/* the registry's progress copy: durationMs is the executor's active time; the activity gist follows */
static void progress_touch_locked(aref_t *r) {
    if (!r->progress) return;
    int64_t now = now_ms();
    json_set_int(r->progress, "durationMs", r->active_ms + (r->executor_run && r->running ? now - r->run_started : 0));
    r->last_activity = now;
    const char *li = json_get_str(r->progress, "lastIntent"), *ct = json_get_str(r->progress, "currentTool");
    if (li || ct) {
        free(r->gist);
        r->gist = li ? xstrdup(li) : xasprintf("running %s", ct);
    }
}

static void owed_clear(aref_t *r) {
    for (size_t i = 0; i < r->owed.n; i++) free(r->owed.v[i].to);
    r->owed.n = 0;
}

/* IrcBus.recordReply: a settled job of `agent` owned by `owner` delivers its outcome, so it answers what `owner` asked
 * it; after progress only an explicit message does. Without g_mu. */
static void owed_job_reply(const char *agent, const char *owner);

static aref_t *find_locked(const char *id) {
    for (size_t i = 0; i < g_refs.n; i++)
        if (!g_refs.v[i]->gone && str_eq(g_refs.v[i]->id, id)) return g_refs.v[i];
    return NULL;
}

static aref_t *find_any_locked(const char *id) {
    for (size_t i = g_refs.n; i > 0; i--)
        if (str_eq(g_refs.v[i - 1]->id, id)) return g_refs.v[i - 1];
    return NULL;
}

static aref_t *find_app_locked(const app_t *a) {
    for (size_t i = 0; i < g_refs.n; i++)
        if (!g_refs.v[i]->gone && g_refs.v[i]->app == a) return g_refs.v[i];
    return NULL;
}

__attribute__((unused)) static void ref_free(aref_t *r) {
    task_card_release(r->card);
    json_free(r->progress);
    free(r->description);
    free(r->parent_call);
    free(r->sys_append);
    free(r->model);
    free(r->recent_tail);
    free(r->gist);
    free(r->session_file);
    free(r->output_path);
    json_free(r->model_override);
    free(r->id);
    free(r->agent);
    free(r->parent_id);
    free(r->output);
    json_free(r->transcript);
    json_free(r->inbox);
    strvec_free(&r->pending);
    owed_clear(r);
    vec_free(&r->owed);
    strvec_free(&r->sent_run);
    strvec_free(&r->wait_asked);
    strvec_free(&r->wait_watched);
    free(r->activity);
    free(r->job_id);
    free(r);
}

const char *agents_id_of(const app_t *a) {
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_app_locked(a);
    const char *id = r ? r->id : NULL;
    pthread_mutex_unlock(&g_mu);
    return id;
}

static const char *env_id(tool_env_t *env) { return env && env->session ? agents_id_of(env->session) : NULL; }

void agents_register_main(app_t *a) {
    pthread_mutex_lock(&g_mu);
    aref_t *r = xcalloc(1, sizeof *r);
    r->id = find_locked(MAIN_AGENT_ID) ? xasprintf("%s-%zu", MAIN_AGENT_ID, g_refs.n + 1) : xstrdup(MAIN_AGENT_ID);
    r->agent = xstrdup("main");
    r->app = a;
    r->is_main = true;
    r->started_at = now_ms();
    vec_push(&g_refs, r);
    pthread_mutex_unlock(&g_mu);
}

void agents_unregister(app_t *a) {
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_app_locked(a);
    if (r) {
        if (r->is_main) {
            r->gone = true;
            r->app = NULL;
        } else {
            /* a subagent session is being freed: keep its transcript readable */
            if (!r->transcript && a->agent) r->transcript = json_clone(a->agent->messages);
            r->app = NULL;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void agents_release_children(app_t *a) {
    VEC(app_t *) kill = {0};
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_refs.n; i++) {
        aref_t *r = g_refs.v[i];
        if (r->spawner != a) continue;
        r->spawner = NULL;
        if (r->app && r->owned && !r->running && !r->pins) {
            if (!r->transcript) r->transcript = json_clone(r->app->agent->messages);
            vec_push(&kill, r->app);
            r->app = NULL;
        }
        r->gone = r->gone || !r->app || r->pins; /* a viewed session goes once its view lets go */
    }
    pthread_mutex_unlock(&g_mu);
    for (size_t i = 0; i < kill.n; i++) app_free(kill.v[i]);
    vec_free(&kill);
    notify_change();
}

/* ---------- names ---------- */

static char *gen_name(void) {
    static const char *adj[] = {"Swift", "Quiet", "Bright", "Keen", "Bold", "Calm", "Clever", "Eager", "Lucky", "Nimble",
                                "Brave", "Sharp", "Steady", "Witty", "Gentle", "Rapid", "Silent", "Sunny", "Vivid", "Wise"};
    static const char *noun[] = {"Falcon", "Otter", "Badger", "Heron", "Lynx", "Panda", "Raven", "Tiger", "Wolf", "Fox",
                                 "Marten", "Puffin", "Bison", "Gecko", "Koala", "Lemur", "Moose", "Orca", "Quail", "Yak"};
    uint32_t r[2];
    rand_bytes(r, sizeof r);
    return xasprintf("%s%s", adj[r[0] % ARRAY_LEN(adj)], noun[r[1] % ARRAY_LEN(noun)]);
}

static char *clean_name(const char *n) {
    sb_t b = SB_INIT;
    for (const char *p = n ? n : ""; *p && b.len < 32; p++)
        if (isalnum((unsigned char)*p) || *p == '_' || *p == '-') sb_putc(&b, *p);
    if (!b.len) {
        sb_free(&b);
        return gen_name();
    }
    return sb_detach(&b);
}

/* ---------- permits (task.maxConcurrency) ---------- */

static bool permit_acquire(volatile sig_atomic_t *cancel) {
    pthread_mutex_lock(&g_mu);
    for (;;) {
        int64_t maxc = settings_int("task.maxConcurrency", 32);
        if (maxc <= 0 || g_permits_used < maxc) {
            g_permits_used++;
            pthread_mutex_unlock(&g_mu);
            return true;
        }
        if (cancel && *cancel) {
            pthread_mutex_unlock(&g_mu);
            return false;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
}

static void permit_release(void) {
    pthread_mutex_lock(&g_mu);
    if (g_permits_used > 0) g_permits_used--;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

/* ---------- running a subagent ---------- */

/* recentOutput: the last 8 non-empty lines of the run's latest assistant text, newest first */
static void recent_output_locked(aref_t *r) {
    json_t *out = json_arr();
    strvec_t lines = str_split(r->recent_tail ? r->recent_tail : "", "\n");
    for (size_t i = lines.n; i > 0 && json_len(out) < 8; i--) {
        char *t = str_trim(lines.v[i - 1]);
        if (*t) json_push_str(out, lines.v[i - 1]);
        free(t);
    }
    strvec_free(&lines);
    json_set(r->progress, "recentOutput", out);
}

static void on_child_event(void *ud, const agent_event_t *ev) {
    aref_t *r = ud;
    bool progress = false;
    json_t *pp = NULL, *evp = NULL;
    if (ev->type == AGENT_EV_TOOL_START || ev->type == AGENT_EV_TOOL_END || ev->type == AGENT_EV_MESSAGE_END) {
        pthread_mutex_lock(&g_mu);
        task_card_t *card = card_retain(r->card);
        int idx = r->card_idx;
        int64_t started = r->run_started;
        int64_t win = r->app && r->app->model ? r->app->model->context_window : 0;
        if (r->progress && r->executor_run && row_on_event(r->progress, 0, ev, win)) progress = true;
        pthread_mutex_unlock(&g_mu);
        if (card) {
            if (card_on_event(card, idx, started, ev, win)) {
                char *t = xasprintf("Running agent %s...", r->id);
                card_emit(card, t);
                free(t);
            }
            task_card_release(card);
        }
    }
    pthread_mutex_lock(&g_mu);
    if (r->progress && r->executor_run && ev->message && str_eq(json_get_str(ev->message, "role"), "assistant") &&
        (ev->type == AGENT_EV_MESSAGE_START || ev->type == AGENT_EV_MESSAGE_END)) {
        /* replaceRecentOutputFromContent / resetRecentOutput */
        free(r->recent_tail);
        r->recent_tail = ev->type == AGENT_EV_MESSAGE_END ? ai_message_text(ev->message) : NULL;
        recent_output_locked(r);
        progress = true;
    }
    if (r->progress && r->executor_run && ev->type == AGENT_EV_TURN_START) progress = true;
    if (ev->type == AGENT_EV_TOOL_START || ev->type == AGENT_EV_TURN_START) {
        free(r->activity);
        r->activity = ev->type == AGENT_EV_TOOL_START && ev->tool_name ? xasprintf("running %s", ev->tool_name) : xstrdup("thinking");
    }
    r->last_activity = now_ms();
    if (progress) progress_touch_locked(r);
    bool observed = has_observers();
    if (progress && observed) pp = progress_payload_locked(r);
    if (g_event_observers > 0) {
        evp = json_obj();
        json_set_str(evp, "id", r->id);
        json_set(evp, "event", agent_event_to_json(ev));
    }
    pthread_mutex_unlock(&g_mu);
    if (pp) emit_obs("progress", pp);
    if (evp) emit_obs("event", evp);
    if (progress || ev->type == AGENT_EV_TOOL_START || ev->type == AGENT_EV_TURN_START) notify_change();
}

typedef struct {
    volatile sig_atomic_t *flag;
    agent_t *ag;
    volatile int done;
} watch_t;

static void *cancel_watch(void *p) {
    watch_t *w = p;
    while (!w->done) {
        if (*w->flag) {
            agent_abort(w->ag);
            break;
        }
        sleep_ms(50);
    }
    return NULL;
}

/* a fresh AgentProgress for an executor run of `r` (task = the prompt the run was given) */
static json_t *progress_new_locked(const aref_t *r, const char *task, const char *assignment) {
    json_t *p = json_obj();
    json_set_int(p, "index", r->index);
    json_set_str(p, "id", r->id);
    json_set_str(p, "agent", r->agent);
    json_set_str(p, "agentSource", agent_source_of(r));
    json_set_str(p, "status", "running");
    json_set_str(p, "task", task ? task : "");
    if (assignment) json_set_str(p, "assignment", assignment);
    if (r->description) json_set_str(p, "description", r->description);
    json_set(p, "recentTools", json_arr());
    json_set(p, "recentOutput", json_arr());
    json_set_int(p, "toolCount", 0);
    json_set_int(p, "requests", 0);
    json_set_int(p, "tokens", 0);
    json_set_int(p, "cost", 0);
    json_set_int(p, "durationMs", 0);
    return p;
}

/* task/executor.ts quiescence barrier: a subagent's run completes only after the background jobs it owns settle.
 * Each settled batch is folded into the run as an async-result follow-up turn (the session's own delivery), so the
 * run's answer is the last turn after every result; a turn that ends stopped (abort) or the caller's cancel ends
 * the wait (teardown reaps the jobs). */
static void settle_owned_jobs(app_t *child, volatile sig_atomic_t *cancel) {
    jobs_t *jobs = child->env ? child->env->jobs : NULL;
    if (!jobs) return;
    for (;;) {
        if (cancel && *cancel) return;
        const char *stop = json_get_str(last_assistant(child->agent->messages), "stopReason");
        if (str_eq(stop, "aborted")) return;
        if (jobs_undelivered(jobs) > 0) {
            char *t = app_job_results_text(child);
            if (t) {
                app_prompt_hidden(child, t, "async-result");
                free(t);
            }
            continue;
        }
        if (!jobs_running(jobs)) return;
        jobs_wait_any(jobs, cancel, 500);
    }
}

/* run one prompt on a child session; returns the last assistant text (owned). `executor`: a task spawn or IRC wake
 * run (lifecycle + progress are reported); a user chat turn from the hub (agents_prompt) is not. */
static char *run_turn_ex(aref_t *r, app_t *child, const char *prompt, volatile sig_atomic_t *cancel, bool *aborted, char **error,
                         bool executor, const char *assignment) {
    *aborted = false;
    *error = NULL;
    json_t *lc = NULL;
    pthread_mutex_lock(&g_mu);
    r->running = true;
    r->run_started = now_ms();
    r->last_activity = r->run_started;
    r->cancel = cancel;
    r->executor_run = executor;
    if (executor) {
        if (!r->model && child->model) r->model = xasprintf("%s/%s", child->model->provider, child->model->id);
        json_t *old = r->progress;
        const char *keep_assign = assignment ? assignment : NULL;
        r->progress = progress_new_locked(r, prompt, keep_assign);
        if (old && json_get_str(old, "agentSource")) json_set_str(r->progress, "agentSource", json_get_str(old, "agentSource"));
        json_free(old);
        /* the model the run resolved: its identity and context window (omp sets them before the first request) */
        if (r->model_override) json_set(r->progress, "modelOverride", json_clone(r->model_override));
        if (child->model && child->model->context_window > 0)
            json_set_int(r->progress, "contextWindow", child->model->context_window);
        if (r->model) json_set_str(r->progress, "resolvedModelIdentity", r->model);
        if (r->model) json_set_str(r->progress, "resolvedModel", r->model);
        free(r->recent_tail);
        r->recent_tail = NULL;
        r->active_ms = 0; /* progress (and its active time) is per executor run */
        r->lifecycle_open = true;
        if (has_observers()) lc = lifecycle_payload_locked(r, "started");
    }
    owed_clear(r);
    strvec_free(&r->sent_run);
    pthread_mutex_unlock(&g_mu);
    if (lc) emit_obs("lifecycle", lc);
    notify_change();
    pthread_t wt;
    watch_t w = {cancel, child->agent, 0};
    bool watching = cancel && pthread_create(&wt, NULL, cancel_watch, &w) == 0;
    agent_subscribe(child->agent, on_child_event, r);
    free(child->cost_refusal);
    child->cost_refusal = NULL;
    bool refused = app_prompt(child, prompt, NULL) == APP_PROMPT_REFUSED;
    settle_owned_jobs(child, cancel);
    /* a focused view's empty Enter forced the queue into the continuation */
    while (agent_take_forced_delivery(child->agent) && agent_run_queued(child->agent) == 0) settle_owned_jobs(child, cancel);
    agent_unsubscribe(child->agent, on_child_event, r);
    w.done = 1;
    if (watching) pthread_join(wt, NULL);
    const json_t *last = last_assistant(child->agent->messages);
    char *text = last ? ai_message_text(last) : xstrdup("");
    const char *stop = json_get_str(last, "stopReason");
    *aborted = str_eq(stop, "aborted") || (cancel && *cancel);
    if (refused) *error = xstrdup(child->prompt_refusal ? child->prompt_refusal : "error");
    else if (str_eq(stop, "error") && json_get_str(last, "errorMessage")) *error = xstrdup(json_get_str(last, "errorMessage"));
    /* executor.ts: the cap refused a request mid-run (the loop stops before the model call without throwing): its
     * message is why the run produced no result */
    else if (child->cost_refusal && !*aborted) *error = xstrdup(child->cost_refusal);
    json_t *pp = NULL;
    lc = NULL;
    pthread_mutex_lock(&g_mu);
    int64_t now = now_ms();
    if (executor) {
        r->active_ms += now - r->run_started;
        const char *st = *aborted || r->killed ? "aborted" : *error ? "failed" : "completed";
        if (r->progress) {
            json_set_str(r->progress, "status", st);
            json_del(r->progress, "currentTool");
            json_del(r->progress, "currentToolArgs");
            json_del(r->progress, "currentToolIntent");
            json_del(r->progress, "currentToolStartMs");
            json_set_int(r->progress, "durationMs", r->active_ms);
        }
        if (has_observers()) {
            pp = progress_payload_locked(r);
            lc = lifecycle_payload_locked(r, st);
        }
        r->lifecycle_open = false;
    }
    r->running = false;
    r->executor_run = false;
    r->last_activity = now;
    free(r->gist);
    r->gist = NULL;
    r->cancel = NULL;
    r->aborted = *aborted;
    free(r->activity);
    r->activity = NULL;
    char *trim = str_trim(text);
    if (*trim) {
        free(r->output);
        r->output = xstrdup(trim);
        r->has_output = true;
        r->published_at = now_ms();
    }
    free(trim);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    if (pp) emit_obs("progress", pp);
    if (lc) emit_obs("lifecycle", lc);
    notify_change();
    return text;
}

static char *run_turn(aref_t *r, app_t *child, const char *prompt, volatile sig_atomic_t *cancel, bool *aborted, char **error) {
    return run_turn_ex(r, child, prompt, cancel, aborted, error, true, NULL);
}

/* task/executor.ts finalize: the run's output lands in `<artifactsDir>/<id>.md` next to its transcript — always for
 * the spawn's own run, for a follow-up turn only when it is a successful, non-empty reply the user or the spawner
 * asked for (publishMessageResult) */
static void publish_output(aref_t *r, const char *text, bool initial, bool ok) {
    if (!initial && (!ok || !text || !*text)) return;
    pthread_mutex_lock(&g_mu);
    char *path = r->session_file && str_ends(r->session_file, ".jsonl")
                     ? xasprintf("%.*s.md", (int)(strlen(r->session_file) - 6), r->session_file)
                     : NULL;
    pthread_mutex_unlock(&g_mu);
    if (!path) return;
    const char *t = text ? text : "";
    if (fs_write(path, t, strlen(t)) != 0) {
        free(path);
        return;
    }
    pthread_mutex_lock(&g_mu);
    free(r->output_path);
    r->output_path = path;
    pthread_mutex_unlock(&g_mu);
}

static char *fmt_duration(int64_t ms) {
    if (ms < 1000) return xasprintf("%lldms", (long long)ms);
    int64_t s = ms / 1000;
    if (s < 60) return xasprintf("%.1fs", (double)ms / 1000.0);
    if (s < 3600) return xasprintf("%lldm %llds", (long long)(s / 60), (long long)(s % 60));
    return xasprintf("%lldh %lldm", (long long)(s / 3600), (long long)((s % 3600) / 60));
}

#define INLINE_OUTPUT_CHARS 7500
#define PREVIEW_CHARS 5000

static char *render_summary(const char *id, const char *agent, const char *status, int64_t dur_ms, const char *output,
                            const char *error, bool aborted) {
    json_t *c = json_obj();
    json_set_str(c, "id", id);
    json_set_str(c, "agentName", agent);
    json_set_str(c, "status", status);
    char *d = fmt_duration(dur_ms);
    json_set_str(c, "duration", d);
    free(d);
    char *out = str_trim(output ? output : "");
    if (!*out) {
        free(out);
        out = xstrdup("(no output)");
    }
    size_t olen = utf8_strlen(out);
    bool truncated = olen > INLINE_OUTPUT_CHARS;
    if (truncated) {
        size_t i = 0, cps = 0;
        while (out[i] && cps < PREVIEW_CHARS) {
            uint32_t cp;
            i += (size_t)utf8_decode(out + i, strlen(out + i), &cp);
            cps++;
        }
        json_set_str(c, "preview", "");
        char *head = xstrndup(out, i);
        json_set_str(c, "preview", head);
        free(head);
        json_t *meta = json_obj();
        json_set_int(meta, "lineCount", (int64_t)str_count(out, "\n") + 1);
        char *sz = xasprintf("%.1fKB", (double)strlen(out) / 1024.0);
        json_set_str(meta, "charSize", sz);
        free(sz);
        json_set(c, "meta", meta);
    } else {
        json_set_str(c, "preview", out);
    }
    json_set_bool(c, "truncated", truncated);
    if (error) json_set_str(c, "error", error);
    if (aborted) json_set_str(c, "abortReason", "Cancelled");
    json_set_bool(c, "resumable", aborted);
    char *s = prompt_render("coding-agent/prompts/tools/task-summary.md", c);
    json_free(c);
    free(out);
    char *t = str_trim(s);
    free(s);
    return t;
}

static char *follow_up_hint(const char *id, bool aborted) {
    json_t *c = json_obj();
    json_set_str(c, "agentId", id);
    json_set_bool(c, "aborted", aborted);
    json_set_bool(c, "isolated", false);
    json_set_bool(c, "ircEnabled", true);
    json_set_bool(c, "resumable", aborted);
    json_set_bool(c, "transcriptAvailable", true);
    char *s = prompt_render("coding-agent/prompts/tools/task-follow-up.md", c);
    json_free(c);
    char *t = str_trim(s);
    free(s);
    return t;
}

typedef struct {
    tool_env_t *env;
    app_t *parent;
    aref_t *ref;
    char *agent, *task, *context, *model;
} spawn_t;

static void spawn_free(void *p) {
    spawn_t *s = p;
    free(s->agent);
    free(s->task);
    free(s->context);
    free(s->model);
    free(s);
}

/* a ref's registry status: the main session is `running` while its agent loop runs (syncSessionStatus) */
static bool ref_live_running(const aref_t *r) { return r->is_main ? r->app && agent_is_running(r->app->agent) : r->running; }

static char *peer_roster_prompt(aref_t *self, const char *agent_body, const char *context) {
    json_t *c = json_obj();
    json_set_str(c, "agent", agent_body ? agent_body : "");
    if (context && *context) json_set_str(c, "context", context);
    json_set_str(c, "ircSelfId", self->id);
    json_set_bool(c, "structured", false);
    json_t *peers = json_arr();
    /* collectIrcPeerRoster: live refs, running before idle, then newest activity first; at most 20 named */
    pthread_mutex_lock(&g_mu);
    VEC(aref_t *) live = {0};
    for (size_t i = 0; i < g_refs.n; i++) {
        aref_t *r = g_refs.v[i];
        if (r == self || r->gone || !r->app) continue;
        vec_push(&live, r);
    }
    for (size_t i = 1; i < live.n; i++) /* stable insertion sort (tiny lists) */
        for (size_t k = i; k > 0; k--) {
            aref_t *x = live.v[k - 1], *y = live.v[k];
            int ox = ref_live_running(x) ? 0 : 1, oy = ref_live_running(y) ? 0 : 1;
            if (ox < oy || (ox == oy && x->last_activity >= y->last_activity)) break;
            live.v[k - 1] = y;
            live.v[k] = x;
        }
    size_t shown = 0, omitted = 0;
    for (size_t i = 0; i < live.n; i++) {
        aref_t *r = live.v[i];
        if (shown >= 20) {
            omitted++;
            continue;
        }
        json_t *p = json_obj();
        json_set_str(p, "id", r->id);
        /* sdk.ts registers `displayName: agentDisplayName ?? agentKind` ("main" / the agent type), kind main|sub */
        json_set_str(p, "displayName", r->is_main ? "main" : r->agent);
        json_set_str(p, "kind", r->is_main ? "main" : "sub");
        json_set_str(p, "status", ref_live_running(r) ? "running" : "idle");
        if (r->gist) json_set_str(p, "activity", r->gist); /* AgentRegistry.setActivity: the progress gist */
        json_push(peers, p);
        shown++;
    }
    vec_free(&live);
    pthread_mutex_unlock(&g_mu);
    json_set(c, "ircPeers", peers);
    if (omitted) json_set_int(c, "ircOmittedCount", (int64_t)omitted);
    char *s = prompt_render("coding-agent/prompts/system/subagent-system-prompt.md", c);
    json_free(c);
    return s;
}

/* builds the child session (on the job thread); NULL + *err on failure */
static app_t *make_child(spawn_t *sp, char **err) {
    json_t *defs = agent_defs_load(sp->env->cwd);
    json_t *def = json_get(defs, sp->agent);
    json_t *msgs = NULL;
    char *model = sp->model ? xstrdup(sp->model) : NULL;
    {
        const char *pat = model ? model : json_get_str(def, "model");
        json_t *mo = NULL;
        if (pat && *pat) { /* role aliases (`@task`) expand to their configured patterns */
            strvec_t parts = mr_configured_patterns(pat);
            if (parts.n) {
                mo = json_arr();
                for (size_t i = 0; i < parts.n; i++) json_push_str(mo, parts.v[i]);
            }
            strvec_free(&parts);
        }
        pthread_mutex_lock(&g_mu);
        json_free(sp->ref->model_override);
        sp->ref->model_override = mo;
        pthread_mutex_unlock(&g_mu);
    }
    char *append = peer_roster_prompt(sp->ref, json_get_str(def, "body"), sp->context);
    bool ro = agent_def_read_only(def);
    /* the transcript persists next to the parent's artifacts (omp `<artifactsDir>/<id>.jsonl`) */
    const char *adir = sp->parent && sp->parent->env ? artifacts_dir(sp->parent->env->artifacts) : NULL;
    char *sfile = adir ? xasprintf("%s/%s.jsonl", adir, sp->ref->id) : NULL;
    app_t *child = app_subagent_new_at(sp->parent, sp->agent, model, append, sfile, err);
    pthread_mutex_lock(&g_mu);
    free(sp->ref->session_file);
    sp->ref->session_file = child ? sfile : NULL;
    if (!child) free(sfile);
    free(sp->ref->sys_append);
    sp->ref->sys_append = append;
    sp->ref->read_only = ro;
    free(sp->ref->model);
    sp->ref->model = child && child->model ? xasprintf("%s/%s", child->model->provider, child->model->id) : NULL;
    /* a role alias with no configured patterns (`@task` on the default role) reports the model it resolved to */
    if (child && sp->ref->model && !sp->ref->model_override && (model || json_get_str(def, "model"))) {
        sp->ref->model_override = json_arr();
        json_push_str(sp->ref->model_override, sp->ref->model);
    }
    pthread_mutex_unlock(&g_mu);
    free(model);
    json_free(defs);
    if (child && msgs) {
        json_t *m;
        json_arr_foreach(msgs, i, m) agent_append_message(child->agent, json_clone(m));
    }
    json_free(msgs);
    return child;
}

typedef struct {
    bool broadcast, progress, wake_relay;
} irc_flags_t;
static void deliver(const char *from, const char *to, const char *body, irc_flags_t fl, const char **outcome, char **err);

static char *task_body_run(bgjob_t *job, void *ud, bool *failed) {
    spawn_t *sp = ud;
    aref_t *r = sp->ref;
    volatile sig_atomic_t *cancel = job_cancel_flag(job);
    int64_t t0 = mono_ms();
    job_set_output(job, "queued");
    if (!permit_acquire(cancel)) {
        *failed = true;
        pthread_mutex_lock(&g_mu);
        r->aborted = true;
        pthread_mutex_unlock(&g_mu);
        return xasprintf("%s was aborted before it started.", r->id);
    }
    job_set_output(job, "starting");
    char *err = NULL;
    app_t *child = NULL;
    pthread_mutex_lock(&g_mu);
    child = r->app;
    pthread_mutex_unlock(&g_mu);
    if (!child) {
        child = make_child(sp, &err);
        pthread_mutex_lock(&g_mu);
        r->app = child;
        r->owned = child != NULL;
        r->awaiting_assignment = child != NULL;
        pthread_mutex_unlock(&g_mu);
    }
    if (!child) {
        permit_release();
        *failed = true;
        char *s = render_summary(r->id, sp->agent, "failed", mono_ms() - t0, "", err ? err : "failed to start", false);
        free(err);
        return s;
    }
    char *up = tool_subagent_user_prompt(sp->task);
    job_set_output(job, "running");
    pthread_mutex_lock(&g_mu);
    task_card_t *card = card_retain(r->card);
    int cidx = r->card_idx;
    r->run_started = now_ms();
    pthread_mutex_unlock(&g_mu);
    if (card) {
        card_set_status(card, cidx, "running");
        char *t = xasprintf("Running background task %s...", r->id);
        card_emit(card, t);
        free(t);
        task_card_release(card);
    }
    bool aborted;
    char *error = NULL;
    /* this turn carries the assignment: IRC asides parked for it fold in ahead of the message */
    pthread_mutex_lock(&g_mu);
    r->awaiting_assignment = false;
    strvec_t asides = r->early_asides, peers = r->early_peers;
    memset(&r->early_asides, 0, sizeof r->early_asides);
    memset(&r->early_peers, 0, sizeof r->early_peers);
    pthread_mutex_unlock(&g_mu);
    for (size_t i = 0; i < asides.n; i++) {
        json_t *m = msg_user_text(asides.v[i]);
        json_set_bool(m, "synthetic", true);
        json_set_str(m, "customType", "irc");
        agent_append_message(child->agent, m);
    }
    strvec_free(&asides);
    char *text = run_turn_ex(r, child, up, cancel, &aborted, &error, true, sp->task);
    free(up);
    permit_release();
    publish_output(r, text, true, !aborted && !error);
    owed_job_reply(r->id, r->parent_id);
    for (size_t i = 0; i + 1 < peers.n; i += 2) {
        const char *oc;
        deliver(peers.v[i], r->id, peers.v[i + 1], (irc_flags_t){0}, &oc, NULL);
    }
    strvec_free(&peers);
    const char *status = aborted ? "aborted" : error ? "failed" : "completed";
    *failed = aborted || error;
    char *sum = render_summary(r->id, sp->agent, status, mono_ms() - t0, text, error, aborted);
    char *hint = follow_up_hint(r->id, aborted);
    char *res = xasprintf("%s\n\n%s", sum, hint);
    free(sum);
    free(hint);
    free(text);
    free(error);
    return res;
}

/* the job body: one subagent run, reported into the spawning task call's card */
static char *task_body(bgjob_t *job, void *ud, bool *failed) {
    spawn_t *sp = ud;
    aref_t *r = sp->ref;
    char *res = task_body_run(job, ud, failed);
    pthread_mutex_lock(&g_mu);
    task_card_t *card = r->card;
    int idx = r->card_idx;
    bool aborted = r->aborted;
    r->card = NULL;
    pthread_mutex_unlock(&g_mu);
    if (card) {
        task_card_settle(card, idx, aborted ? "aborted" : *failed ? "failed" : "completed");
        char *t = xasprintf(*failed ? "Background task %s failed." : "Background task %s complete.", r->id);
        card_emit(card, t);
        free(t);
        task_card_release(card);
    }
    return res;
}

static aref_t *new_ref(tool_env_t *env, const spawn_spec_t *spec, char **err) {
    app_t *parent = env->session;
    const char *pid = parent ? agents_id_of(parent) : NULL;
    char *base = clean_name(spec->name);
    pthread_mutex_lock(&g_mu);
    char *id = (!pid || !strcmp(pid, MAIN_AGENT_ID)) ? xstrdup(base) : xasprintf("%s.%s", pid, base);
    for (int n = 2; find_locked(id) || str_eq(id, "all") || str_eq(id, MAIN_AGENT_ID); n++) {
        free(id);
        id = (!pid || !strcmp(pid, MAIN_AGENT_ID)) ? xasprintf("%s%d", base, n) : xasprintf("%s.%s%d", pid, base, n);
    }
    aref_t *r = xcalloc(1, sizeof *r);
    r->id = id;
    r->agent = xstrdup(spec->agent);
    r->parent_id = pid ? xstrdup(pid) : NULL;
    r->spawner = parent;
    r->running = true;
    r->started_at = now_ms();
    r->card = card_retain(spec->card);
    r->card_idx = spec->card_index;
    r->index = spec->card_index;
    r->last_activity = r->started_at;
    if (spec->card) r->parent_call = xstrdup(spec->card->call_id);
    /* the pending progress row the task call announced (agentSource, task, assignment) */
    if (spec->card) {
        pthread_mutex_lock(&spec->card->mu);
        json_t *row = card_row_locked(spec->card, spec->card_index);
        r->progress = row ? json_clone(row) : NULL;
        pthread_mutex_unlock(&spec->card->mu);
    }
    if (!r->progress) {
        char *up = tool_subagent_user_prompt(spec->task);
        r->progress = progress_new_locked(r, up, spec->task);
        json_set_str(r->progress, "status", "pending");
        free(up);
    }
    json_set_str(r->progress, "id", id);
    vec_push(&g_refs, r);
    if (spec->card) task_card_set_id(spec->card, spec->card_index, id, false);
    pthread_mutex_unlock(&g_mu);
    free(base);
    UNUSED(err);
    notify_change();
    return r;
}

static char *model_of(const json_t *m) {
    if (json_is_str(m)) return xstrdup(json_sv(m));
    if (json_is_arr(m)) {
        json_t *x;
        json_arr_foreach(m, i, x) if (json_is_str(x) && app_resolve_model_pattern(json_sv(x), NULL, NULL)) return xstrdup(json_sv(x));
        if (json_len(m) && json_is_str(json_at(m, 0))) return xstrdup(json_sv(json_at(m, 0)));
    }
    return NULL;
}

static spawn_t *make_spawn(tool_env_t *env, const spawn_spec_t *spec, aref_t *r) {
    spawn_t *sp = xcalloc(1, sizeof *sp);
    sp->env = env;
    sp->parent = env->session;
    sp->ref = r;
    sp->agent = xstrdup(spec->agent);
    sp->task = xstrdup(spec->task ? spec->task : "");
    sp->context = spec->context ? xstrdup(spec->context) : NULL;
    sp->model = model_of(spec->model);
    return sp;
}

int agents_spawn(tool_env_t *env, const spawn_spec_t *spec, char **agent_id, char **job_id, char **err) {
    *agent_id = *job_id = NULL;
    *err = NULL;
    if (!env->jobs || !env->session) {
        *err = xstrdup("Subagents are unavailable in this context");
        return -1;
    }
    aref_t *r = new_ref(env, spec, err);
    spawn_t *sp = make_spawn(env, spec, r);
    char *label = xasprintf("%s", r->id);
    char *jid = jobs_start(env->jobs, "task", r->id, label, r->id, false, task_body, sp, spawn_free);
    free(label);
    pthread_mutex_lock(&g_mu);
    r->job_id = xstrdup(jid);
    *agent_id = xstrdup(r->id);
    pthread_mutex_unlock(&g_mu);
    *job_id = jid;
    return 0;
}

char *agents_run_blocking(tool_env_t *env, const spawn_spec_t *spec, volatile sig_atomic_t *cancel, bool *failed) {
    char *err = NULL;
    aref_t *r = new_ref(env, spec, &err);
    spawn_t *sp = make_spawn(env, spec, r);
    /* reuse the job body on a stack job-less path: drive through a private job manager */
    jobs_t *tmp = jobs_new();
    char *jid = jobs_start(tmp, "task", r->id, r->id, r->id, false, task_body, sp, spawn_free);
    while (!jobs_wait_any(tmp, NULL, 200) && jobs_running(tmp)) {
        if (cancel && *cancel) jobs_cancel(tmp, NULL);
    }
    json_t *done = jobs_take_finished(tmp);
    json_t *j0 = json_at(done, 0);
    char *res = xstrdup(j0 ? json_get_str(j0, "result") : "(no output)");
    *failed = j0 ? json_get_bool(j0, "failed", false) : true;
    json_free(done);
    free(jid);
    jobs_free(tmp);
    return res;
}

/* ---------- IRC ---------- */

static char *render_irc(const char *from, const char *body, const char *reply_to, bool progress, bool interrupting, bool relay) {
    json_t *c = json_obj();
    json_set_str(c, "from", from);
    json_set_str(c, "message", body);
    if (reply_to) json_set_str(c, "replyTo", reply_to);
    json_set_bool(c, "progress", progress);
    json_set_bool(c, "interrupting", interrupting);
    json_set_bool(c, "relayOnStop", relay);
    char *s = prompt_render("coding-agent/prompts/system/irc-incoming.md", c);
    json_free(c);
    char *t = str_trim(s);
    free(s);
    return t;
}

typedef struct {
    app_t *child;
    aref_t *ref;
    char *from, *text;
    bool to_spawner;
    bool relay; /* the turn's output answers `from` (relayOnStop) */
} wake_t;

static void wake_free(void *p) {
    wake_t *w = p;
    free(w->from);
    free(w->text);
    free(w);
}

static char *wake_body(bgjob_t *job, void *ud, bool *failed) {
    wake_t *w = ud;
    bool aborted;
    char *error = NULL;
    int64_t t0 = mono_ms();
    char *text = run_turn(w->ref, w->child, w->text, job_cancel_flag(job), &aborted, &error);
    *failed = aborted || error;
    if (w->to_spawner) publish_output(w->ref, text, false, !aborted && !error);
    if (w->to_spawner) owed_job_reply(w->ref->id, w->from);
    char *res;
    if (w->to_spawner) {
        char *sum = render_summary(w->ref->id, w->ref->agent, aborted ? "aborted" : error ? "failed" : "completed", mono_ms() - t0,
                                   text, error, aborted);
        char *hint = follow_up_hint(w->ref->id, aborted);
        res = xasprintf("%s\n\n%s", sum, hint);
        free(sum);
        free(hint);
    } else {
        /* relayWakeTurnOutput: answer the sender, unless this turn already answered or promised it an answer */
        char *t = str_trim(text);
        pthread_mutex_lock(&g_mu);
        bool sent = strvec_contains(&w->ref->sent_run, w->from);
        pthread_mutex_unlock(&g_mu);
        if (*t && w->relay && !sent) {
            const char *oc;
            deliver(w->ref->id, w->from, t, (irc_flags_t){.wake_relay = true}, &oc, NULL);
        }
        free(t);
        res = xstrdup("");
    }
    free(text);
    free(error);
    return res;
}

static void owed_job_reply(const char *agent, const char *owner) {
    pthread_mutex_lock(&g_mu);
    aref_t *o = owner ? find_locked(owner) : NULL;
    for (size_t i = 0; o && i < o->owed.n; i++)
        if (str_eq(o->owed.v[i].to, agent)) {
            if (!o->owed.v[i].promised) {
                free(o->owed.v[i].to);
                o->owed.v[i] = o->owed.v[--o->owed.n];
            }
            break;
        }
    pthread_mutex_unlock(&g_mu);
}

/* the records agents_take_messages can take back out of a session's queues: a peer's message, never a parent steer */
static bool is_irc_record(const json_t *m) { return str_eq(json_get_str(m, "customType"), "irc") && json_get_obj(m, "details"); }

static bool irc_record_direct(const json_t *m) {
    const json_t *d = json_get_obj(m, "details");
    return !json_get_bool(d, "broadcast", false) && !json_get_bool(d, "progress", false);
}

static json_t *irc_row(const char *id, const char *from, const char *to, const char *body, int64_t ts, irc_flags_t fl) {
    json_t *m = json_obj();
    json_set_str(m, "id", id);
    json_set_str(m, "from", from);
    json_set_str(m, "to", to);
    json_set_str(m, "body", body);
    json_set_int(m, "ts", ts);
    if (fl.wake_relay) json_set_bool(m, "wakeRelay", true);
    if (fl.broadcast) json_set_bool(m, "broadcast", true);
    if (fl.progress) json_set_bool(m, "progress", true);
    return m;
}

/* the sender's id is an ancestor's descendant: msg.from's parents reach msg.to */
static bool from_descendant_locked(const char *from, const char *to) {
    strvec_t seen = {0};
    bool r = false;
    aref_t *x = find_locked(from);
    for (const char *id = x ? x->parent_id : NULL; id && !strvec_contains(&seen, id); id = (x = find_locked(id)) ? x->parent_id : NULL) {
        if (str_eq(id, to)) {
            r = true;
            break;
        }
        strvec_push(&seen, id);
    }
    strvec_free(&seen);
    return r;
}

/* deliver one message; caller does not hold g_mu. *outcome NULL on failure, with *err (owned, if asked) the reason. */
static void deliver(const char *from, const char *to, const char *body, irc_flags_t fl, const char **outcome, char **err) {
    bool quiet = fl.broadcast || fl.progress;
    int64_t ts = now_ms();
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_locked(to);
    if (!r || !r->app || r->gone || r->killed) {
        bool known = r != NULL;
        pthread_mutex_unlock(&g_mu);
        *outcome = NULL;
        if (err) {
            *err = known ? xasprintf("Agent \"%s\" has no live session.", to)
                         : xasprintf("Unknown agent \"%s\" \xE2\x80\x94 check the subagent roster or read history:// for known peers.", to);
        }
        return;
    }
    /* IrcBus.unansweredSends: a message back answers what `to` asked `from`; progress only promises an answer */
    aref_t *sender = find_locked(from);
    if (sender && !strvec_contains(&sender->sent_run, to)) strvec_push(&sender->sent_run, to);
    /* a broadcast leg interrupts a reply wait but answers nothing (omp 131c4981f8: #lastReply skips broadcasts) */
    for (size_t i = 0; i < r->owed.n && !fl.broadcast; i++)
        if (str_eq(r->owed.v[i].to, from)) {
            if (fl.progress) {
                r->owed.v[i].promised = true;
                break;
            }
            free(r->owed.v[i].to);
            r->owed.v[i] = r->owed.v[--r->owed.n];
            break;
        }
    char *mid = xasprintf("m%llu", (unsigned long long)++g_msg_seq);
    /* ircWaitAccepts: a pending wait consumes direct messages, and a broadcast from an agent it asked */
    if (r->waiting && (!quiet || (!fl.progress && strvec_contains(&r->wait_asked, from)))) {
        if (!r->inbox) r->inbox = json_arr();
        json_push(r->inbox, irc_row(mid, from, to, body, ts, fl));
        free(mid);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        *outcome = "injected";
        return;
    }
    if (r->awaiting_assignment) {
        free(mid);
        if (str_eq(from, r->parent_id)) {
            strvec_push_own(&r->early_asides, render_irc(from, body, NULL, fl.progress, false, false));
            *outcome = "injected";
        } else {
            strvec_push(&r->early_peers, from);
            strvec_push(&r->early_peers, body);
            *outcome = "woken";
        }
        pthread_mutex_unlock(&g_mu);
        return;
    }
    app_t *app = r->app;
    bool running = r->running || agent_is_running(app->agent);
    if (running) {
        bool from_parent = r->parent_id && str_eq(from, r->parent_id);
        bool peer = r->parent_id && (fl.broadcast || (!from_parent && !from_descendant_locked(from, to)));
        pthread_mutex_unlock(&g_mu);
        json_t *m;
        if (from_parent && !peer) {
            /* a parent's message steers: the template, not the incoming envelope; not a record a wait can take */
            json_t *c = json_obj();
            json_set_str(c, "from", from);
            json_set_str(c, "message", body);
            char *txt = prompt_render("coding-agent/prompts/steering/parent-irc.md", c);
            json_free(c);
            m = msg_user_text(txt);
            free(txt);
            /* original sender and body, for transcript presentation only (omp db90c0d96e) */
            json_t *pi = json_obj();
            json_set_str(pi, "from", from);
            json_set_str(pi, "body", body);
            json_set(m, "parentIrc", pi);
            json_set_str(m, "attribution", "agent");
        } else {
            /* a broadcast leg or progress note asks nothing: a busy recipient reads it at its next step boundary */
            char *txt = render_irc(from, body, NULL, fl.progress, !quiet, false);
            m = msg_user_text(txt);
            free(txt);
            json_t *d = irc_row(mid, from, to, body, ts, fl);
            json_set(m, "details", d);
        }
        json_set_bool(m, "synthetic", true);
        if (!(from_parent && !peer)) json_set_str(m, "customType", "irc");
        if (quiet) agent_aside(app->agent, m);
        else agent_steer(app->agent, m);
        free(mid);
        *outcome = "injected";
        return;
    }
    free(mid);
    if (r->is_main) {
        char *txt = render_irc(from, body, NULL, fl.progress, false, false);
        strvec_push_own(&r->pending, txt);
        void (*wake)(void *) = app->wake;
        void *wud = app->wake_ud;
        pthread_mutex_unlock(&g_mu);
        if (wake) wake(wud);
        *outcome = "woken";
        return;
    }
    /* idle subagent: wake it with a turn of its own */
    app_t *spawner = r->spawner;
    if (!spawner || !spawner->env || !spawner->env->jobs) {
        pthread_mutex_unlock(&g_mu);
        *outcome = NULL;
        if (err) *err = xasprintf("Agent \"%s\" has no live session.", to);
        return;
    }
    const char *spawner_id = NULL;
    aref_t *sp = find_app_locked(spawner);
    if (sp) spawner_id = sp->id;
    wake_t *w = xcalloc(1, sizeof *w);
    w->child = app;
    w->ref = r;
    w->from = xstrdup(from);
    /* relays, broadcast legs and progress notes draw no automatic answer */
    w->relay = !fl.wake_relay && !quiet;
    w->to_spawner = !quiet && !fl.wake_relay && str_eq(from, spawner_id);
    w->text = render_irc(from, body, NULL, fl.progress, false, w->relay);
    r->running = true; /* claimed before the job thread starts */
    char *id = xstrdup(r->id);
    pthread_mutex_unlock(&g_mu);
    char *jid = jobs_start(spawner->env->jobs, "task", id, id, id, false, wake_body, w, wake_free);
    if (!w->to_spawner) jobs_set_quiet(spawner->env->jobs, jid);
    free(jid);
    free(id);
    *outcome = "woken";
}

const char *agents_id_of_env(tool_env_t *env) { return env_id(env); }

char *agents_send(tool_env_t *env, const char *to, const char *body, bool progress, bool *is_error) {
    *is_error = true;
    const char *self = env_id(env);
    if (!self) return xstrdup("Peer messaging is unavailable in this session.");
    char *from = xstrdup(self);
    char *dest = str_trim(to ? to : "");
    char *trimmed = str_trim(body ? body : "");
    char *res = NULL;
    if (!*dest) res = xstrdup("A recipient is required.");
    else if (progress && str_eq(dest, "all")) res = xstrdup("agent://all broadcasts cannot be ?progress updates.");
    else if (!*trimmed) res = xstrdup("agent:// messages require non-empty content.");
    else if (str_eq(dest, from)) res = xstrdup("Cannot send a message to yourself.");
    free(trimmed);
    bool all = str_eq(dest, "all");
    if (!res && all) {
        /* a subagent's broadcast wakes every live peer for news that is almost never theirs */
        pthread_mutex_lock(&g_mu);
        aref_t *me = find_locked(from);
        bool is_main = me && me->is_main;
        pthread_mutex_unlock(&g_mu);
        if (!is_main) res = xstrdup("Only the main agent can broadcast. Message the one agent that can answer via `agent://<id>`.");
    }
    if (res) {
        free(from);
        free(dest);
        return res;
    }
    if (all) {
        strvec_t targets = {0};
        pthread_mutex_lock(&g_mu);
        for (size_t i = 0; i < g_refs.n; i++)
            if (!g_refs.v[i]->gone && g_refs.v[i]->app && !str_eq(g_refs.v[i]->id, from)) strvec_push(&targets, g_refs.v[i]->id);
        pthread_mutex_unlock(&g_mu);
        sb_t b = SB_INIT;
        size_t ok = 0;
        sb_t rows = SB_INIT;
        for (size_t i = 0; i < targets.n; i++) {
            const char *oc;
            char *err = NULL;
            deliver(from, targets.v[i], body, (irc_flags_t){.broadcast = true}, &oc, &err);
            if (oc) ok++;
            if (oc) sb_appendf(&rows, "\n- %s: %s", targets.v[i], oc);
            else sb_appendf(&rows, "\n- %s: failed \xE2\x80\x94 %s", targets.v[i], err ? err : "not running");
            free(err);
        }
        if (!targets.n) sb_append(&b, "No live peers to broadcast to.");
        else sb_appendf(&b, "Broadcast delivered to %zu of %zu peer(s).", ok, targets.n);
        sb_append(&b, sb_cstr(&rows));
        sb_free(&rows);
        *is_error = targets.n > 0 && ok == 0;
        strvec_free(&targets);
        free(from);
        free(dest);
        return sb_detach(&b);
    }
    /* the send's mark is taken before delivery: a reply sent while this is delivered still answers it */
    if (!progress) {
        pthread_mutex_lock(&g_mu);
        aref_t *me = find_locked(from);
        if (me) {
            size_t i = 0;
            while (i < me->owed.n && !str_eq(me->owed.v[i].to, dest)) i++;
            if (i == me->owed.n) vec_push(&me->owed, ((owed_t){.to = xstrdup(dest)}));
            me->owed.v[i].promised = false;
            me->owed.v[i].sent_at = mono_ms();
        }
        pthread_mutex_unlock(&g_mu);
    }
    const char *oc;
    char *err = NULL;
    deliver(from, dest, body, (irc_flags_t){.progress = progress}, &oc, &err);
    if (oc) {
        *is_error = false;
        res = xasprintf("Delivered to %s.", dest);
    } else {
        pthread_mutex_lock(&g_mu);
        aref_t *me = find_locked(from);
        for (size_t i = 0; me && i < me->owed.n; i++)
            if (str_eq(me->owed.v[i].to, dest)) {
                free(me->owed.v[i].to);
                me->owed.v[i] = me->owed.v[--me->owed.n];
                break;
            }
        pthread_mutex_unlock(&g_mu);
        res = xasprintf("Failed: %s is not running. %s", dest, err ? err : "");
        char *t = str_trim(res); /* trimEnd */
        free(res);
        res = t;
    }
    free(err);
    free(from);
    free(dest);
    return res;
}

static int cmp_irc_ts(const void *a, const void *b) {
    const json_t *x = *(const json_t *const *)a, *y = *(const json_t *const *)b;
    int64_t tx = json_get_int(x, "ts", 0), ty = json_get_int(y, "ts", 0);
    if (tx != ty) return tx < ty ? -1 : 1;
    /* mN ids number sends within one millisecond */
    const char *ix = json_get_str(x, "id"), *iy = json_get_str(y, "id");
    size_t lx = ix ? strlen(ix) : 0, ly = iy ? strlen(iy) : 0;
    if (lx != ly) return lx < ly ? -1 : 1;
    return strcmp(ix ? ix : "", iy ? iy : "");
}

/* the mail a wait's bus waiter took (oldest first), NULL when none */
static json_t *agents_take_inbox(tool_env_t *env) {
    const char *self = env_id(env);
    if (!self) return NULL;
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_locked(self);
    json_t *rows = r && r->inbox && json_len(r->inbox) ? r->inbox : NULL;
    if (rows) r->inbox = NULL;
    pthread_mutex_unlock(&g_mu);
    if (!rows) return NULL;
    /* oldest first */
    size_t n = json_len(rows);
    json_t **v = xcalloc(n, sizeof *v);
    for (size_t i = 0; i < n; i++) v[i] = json_clone(json_at(rows, i));
    qsort(v, n, sizeof *v, cmp_irc_ts);
    json_free(rows);
    rows = json_arr();
    for (size_t i = 0; i < n; i++) json_push(rows, v[i]);
    free(v);
    return rows;
}

/* wait.ts handOffMail (omp 7b42b1be60): the messages a wait consumed become user turns of the caller's own session,
 * injected right after the wait returns (IrcBridge.deliverIrcMessage while streaming: a parent's message steers through
 * the parent template, any other one is an agent-attributed irc-incoming record; broadcast legs and progress notes
 * join as asides). True when a message that ends a wait now waits for the caller: mail handed over here, or a direct
 * message already queued in its session (IrcBridge.hasInterrupts). */
bool agents_wait_hand_off(tool_env_t *env) {
    json_t *rows = agents_take_inbox(env);
    const char *self = env_id(env);
    pthread_mutex_lock(&g_mu);
    aref_t *r = self ? find_locked(self) : NULL;
    app_t *app = r ? r->app : NULL;
    char *parent = r && r->parent_id ? xstrdup(r->parent_id) : NULL;
    pthread_mutex_unlock(&g_mu);
    if (!app || !app->agent) {
        free(parent);
        bool any = json_len(rows) > 0;
        json_free(rows);
        return any;
    }
    const json_t *row;
    json_arr_foreach(rows, i, row) {
        const char *from = json_get_str(row, "from"), *body = json_get_str(row, "body");
        irc_flags_t fl = {.wake_relay = json_get_bool(row, "wakeRelay", false),
                          .broadcast = json_get_bool(row, "broadcast", false),
                          .progress = json_get_bool(row, "progress", false)};
        bool quiet = fl.broadcast || fl.progress;
        pthread_mutex_lock(&g_mu);
        bool from_parent = parent && str_eq(from, parent);
        bool peer = parent && (fl.broadcast || (!from_parent && !from_descendant_locked(from, self)));
        pthread_mutex_unlock(&g_mu);
        json_t *m;
        if (from_parent && !peer) {
            json_t *c = json_obj();
            json_set_str(c, "from", from);
            json_set_str(c, "message", body);
            char *txt = prompt_render("coding-agent/prompts/steering/parent-irc.md", c);
            json_free(c);
            m = msg_user_text(txt);
            free(txt);
            json_t *pi = json_obj();
            json_set_str(pi, "from", from);
            json_set_str(pi, "body", body);
            json_set(m, "parentIrc", pi);
        } else {
            char *txt = render_irc(from, body, json_get_str(row, "replyTo"), fl.progress, !quiet, false);
            m = msg_user_text(txt);
            free(txt);
            json_set(m, "details", json_clone(row));
            json_set_str(m, "customType", "irc");
        }
        json_set_str(m, "attribution", "agent");
        json_set_bool(m, "synthetic", true);
        if (quiet) agent_aside(app->agent, m);
        else agent_steer(app->agent, m);
    }
    free(parent);
    bool handed = json_len(rows) > 0;
    json_free(rows);
    if (handed) return true;
    json_t *direct = agent_peek_records(app->agent, is_irc_record, irc_record_direct);
    bool queued = json_len(direct) > 0;
    json_free(direct);
    return queued;
}

void agents_wait_enter(tool_env_t *env, const strvec_t *asked, const strvec_t *watched) {
    const char *self = env_id(env);
    if (!self) return;
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_locked(self);
    if (r) {
        r->waiting++;
        strvec_free(&r->wait_asked);
        strvec_free(&r->wait_watched);
        for (size_t i = 0; asked && i < asked->n; i++) strvec_push(&r->wait_asked, asked->v[i]);
        for (size_t i = 0; watched && i < watched->n; i++) strvec_push(&r->wait_watched, watched->v[i]);
        r->wait_since = mono_ms();
    }
    pthread_mutex_unlock(&g_mu);
}

void agents_wait_leave(tool_env_t *env) {
    const char *self = env_id(env);
    if (!self) return;
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_locked(self);
    if (r && r->waiting > 0) r->waiting--;
    if (r && !r->waiting) {
        strvec_free(&r->wait_asked);
        strvec_free(&r->wait_watched);
    }
    pthread_mutex_unlock(&g_mu);
}

void agents_user_wait(tool_env_t *env, bool on) {
    const char *self = env_id(env);
    if (!self) return;
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_locked(self);
    if (r) r->user_waits = on ? r->user_waits + 1 : MAX(0, r->user_waits - 1);
    pthread_mutex_unlock(&g_mu);
}

/* AgentAwaits.#reaches: whether `id` finishes only after `target` does */
static bool await_reaches_locked(const char *id, const char *target, strvec_t *path) {
    if (str_eq(id, target)) return true;
    if (strvec_contains(path, id)) return false;
    aref_t *r = find_locked(id);
    if (!r || !r->waiting || !r->wait_watched.n) return false;
    strvec_push(path, id);
    bool all = true;
    for (size_t i = 0; i < r->wait_watched.n && all; i++) all = await_reaches_locked(r->wait_watched.v[i], target, path);
    free(path->v[--path->n]);
    return all;
}

/* AgentAwaits.blocksOn: `agent` cannot act on a message `target` sent at `sent_at` until `target` finishes */
static bool await_blocks_locked(aref_t *agent, const char *target, int64_t sent_at) {
    if (!agent->waiting || !agent->wait_watched.n || agent->wait_since < sent_at) return false; /* interruptible */
    strvec_t path = {0};
    strvec_push(&path, agent->id);
    bool all = true;
    for (size_t i = 0; i < agent->wait_watched.n && all; i++) all = await_reaches_locked(agent->wait_watched.v[i], target, &path);
    strvec_free(&path);
    return all;
}

void agents_pending_replies(tool_env_t *env, agents_replies_t *out) {
    memset(out, 0, sizeof *out);
    const char *self = env_id(env);
    if (!self) return;
    pthread_mutex_lock(&g_mu);
    aref_t *me = find_locked(self);
    int64_t since = me && me->app && me->app->agent ? me->app->agent->run_started_mono : 0;
    for (size_t i = 0; me && i < me->owed.n; i++) {
        const owed_t *o = &me->owed.v[i];
        if (o->sent_at < since) continue;
        aref_t *p = find_locked(o->to);
        bool session = p && p->app && !p->gone && !p->killed;
        if (session && await_blocks_locked(p, self, o->sent_at)) strvec_push(&out->blocked, o->to);
        else if (session && p->user_waits) strvec_push(&out->on_user, o->to);
        else if (session && (o->promised || ref_live_running(p))) strvec_push(&out->from, o->to);
        else strvec_push(&out->stopped, o->to);
    }
    pthread_mutex_unlock(&g_mu);
}

void agents_replies_free(agents_replies_t *r) {
    strvec_free(&r->from);
    strvec_free(&r->blocked);
    strvec_free(&r->on_user);
    strvec_free(&r->stopped);
}

char *agents_take_pending_for(app_t *a) {
    char *r = NULL;
    pthread_mutex_lock(&g_mu);
    aref_t *ref = find_app_locked(a);
    if (ref && ref->pending.n) {
        r = strvec_join(&ref->pending, "\n\n");
        strvec_free(&ref->pending);
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

bool agents_has_pending(app_t *a) {
    pthread_mutex_lock(&g_mu);
    aref_t *ref = find_app_locked(a);
    bool r = ref && ref->pending.n;
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* ---------- reads ---------- */

static char *json_path_extract(const char *raw, const char *path, char **err) {
    json_t *j = json_parse_str(raw);
    if (!j) {
        *err = xstrdup("output is not valid JSON");
        return NULL;
    }
    strvec_t segs = str_split(path, "/");
    const json_t *cur = j;
    for (size_t i = 0; i < segs.n && cur; i++) {
        if (!*segs.v[i]) continue;
        if (json_is_arr(cur)) {
            char *end;
            long k = strtol(segs.v[i], &end, 10);
            cur = *end || k < 0 ? NULL : json_at(cur, (size_t)k);
        } else if (json_is_obj(cur)) {
            cur = json_get(cur, segs.v[i]);
        } else {
            cur = NULL;
        }
    }
    strvec_free(&segs);
    char *r = !cur ? xstrdup("null") : json_is_str(cur) ? xstrdup(json_sv(cur)) : json_dump(cur, 2);
    json_free(j);
    return r;
}

static const json_t *ref_messages(aref_t *r) { return r->app ? r->app->agent->messages : r->transcript; }

static char *last_text_of(const json_t *msgs) {
    for (size_t i = json_len(msgs); i > 0; i--) {
        const json_t *m = json_at(msgs, i - 1);
        if (!str_eq(json_get_str(m, "role"), "assistant")) continue;
        char *t = ai_message_text(m);
        char *tt = str_trim(t);
        free(t);
        if (*tt) return tt;
        free(tt);
    }
    return NULL;
}

char *agents_read_output(const char *id_and_path, char **err) {
    *err = NULL;
    const char *slash = strchr(id_and_path, '/');
    char *id = slash ? xstrndup(id_and_path, (size_t)(slash - id_and_path)) : xstrdup(id_and_path);
    const char *path = slash && slash[1] ? slash + 1 : NULL;
    char *res = NULL;
    if (!*id) *err = xstrdup("agent:// URL requires an output ID: agent://<id>");
    else if (str_eq(id, "all")) *err = xstrdup("agent://all is write-only; use it to broadcast a message.");
    if (*err) {
        free(id);
        return NULL;
    }
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_any_locked(id);
    if (!r || r->is_main) {
        sb_t b = SB_INIT;
        size_t k = 0;
        for (size_t i = 0; i < g_refs.n && k < 5; i++)
            if (!g_refs.v[i]->is_main) sb_appendf(&b, "%s%s", k++ ? ", " : "", g_refs.v[i]->id);
        *err = b.len ? xasprintf("Not found: %s\nDid you mean: %s", id, sb_cstr(&b)) : xasprintf("Not found: %s\nList agents with history://", id);
        sb_free(&b);
    } else if (r->has_output && !(r->running && !path)) {
        res = path ? json_path_extract(r->output, path, err) : xstrdup(r->output);
    } else if (r->has_output && r->running) {
        char *age = fmt_duration(now_ms() - r->published_at);
        res = xasprintf("[%s is running a newer turn; this output was published %s ago]\n\n%s", r->id, age, r->output);
        free(age);
    } else if (path) {
        *err = xasprintf("Output %s is not published yet (status: %s); read agent://%s for its progress.", r->id,
                         r->running ? "running" : "idle", r->id);
    } else {
        json_t *c = json_obj();
        json_set_str(c, "id", r->id);
        json_set_str(c, "status", r->running ? "running" : r->aborted ? "aborted" : "idle");
        json_set(c, "sections", json_arr());
        char *lt = last_text_of(ref_messages(r));
        if (lt) json_set_str(c, "lastText", lt);
        json_set_bool(c, "empty", !lt);
        free(lt);
        char *s = prompt_render("coding-agent/prompts/tools/agent-url-progress.md", c);
        json_free(c);
        res = s;
    }
    pthread_mutex_unlock(&g_mu);
    free(id);
    return res;
}

static void render_block_text(sb_t *b, const json_t *content) {
    if (json_is_str(content)) {
        sb_append(b, json_sv(content));
        return;
    }
    json_t *x;
    json_arr_foreach(content, i, x) {
        const char *t = json_get_str(x, "type");
        if (str_eq(t, "text")) sb_append(b, json_get_str(x, "text") ? json_get_str(x, "text") : "");
        else if (str_eq(t, "image")) sb_append(b, "[image]");
    }
}

static char *render_transcript(const char *id, const json_t *msgs) {
    sb_t b = SB_INIT;
    sb_appendf(&b, "# history://%s\n", id);
    json_t *m;
    json_arr_foreach(msgs, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "user")) {
            sb_append(&b, "\n## User\n\n");
            render_block_text(&b, json_get(m, "content"));
            sb_putc(&b, '\n');
        } else if (str_eq(role, "assistant")) {
            sb_append(&b, "\n## Assistant\n\n");
            json_t *x;
            json_arr_foreach(json_get(m, "content"), k, x) {
                const char *t = json_get_str(x, "type");
                if (str_eq(t, "text")) sb_appendf(&b, "%s\n", json_get_str(x, "text") ? json_get_str(x, "text") : "");
                else if (str_eq(t, "toolCall")) {
                    json_t *args = json_clone(json_get(x, "arguments"));
                    if (json_is_obj(args)) json_del(args, "i");
                    char *a = json_dump(args, 0);
                    sb_appendf(&b, "→ %s %s\n", json_get_str(x, "name"), a);
                    free(a);
                    json_free(args);
                }
            }
        } else if (str_eq(role, "toolResult")) {
            sb_appendf(&b, "\n### %s result%s\n\n", json_get_str(m, "toolName") ? json_get_str(m, "toolName") : "tool",
                       json_get_bool(m, "isError", false) ? " (error)" : "");
            sb_t t = SB_INIT;
            render_block_text(&t, json_get(m, "content"));
            if (t.len > 4000) {
                sb_truncate(&t, 4000);
                sb_append(&t, "…");
            }
            sb_append(&b, sb_cstr(&t));
            sb_putc(&b, '\n');
            sb_free(&t);
        }
    }
    return sb_detach(&b);
}

char *agents_history(const char *id, char **err) {
    *err = NULL;
    char *res = NULL;
    pthread_mutex_lock(&g_mu);
    if (!id || !*id) {
        sb_t b = SB_INIT;
        for (size_t i = 0; i < g_refs.n; i++) {
            aref_t *r = g_refs.v[i];
            if (r->is_main) continue;
            const char *st = r->running ? "running" : r->aborted ? "aborted" : r->app ? "idle" : "parked";
            sb_appendf(&b, "%shistory://%s — %s (%s)%s%s", b.len ? "\n" : "", r->id, r->agent, st,
                       r->parent_id ? " — spawned by " : "", r->parent_id ? r->parent_id : "");
        }
        res = b.len ? sb_detach(&b) : (sb_free(&b), xstrdup("No registered agents."));
    } else {
        aref_t *r = find_any_locked(id);
        if (!r || r->is_main) *err = xasprintf("Not found: %s\nList agents with history://", id);
        else res = render_transcript(r->id, ref_messages(r));
    }
    pthread_mutex_unlock(&g_mu);
    return res;
}

json_t *agents_running(void) {
    json_t *arr = json_arr();
    pthread_mutex_lock(&g_mu);
    int64_t now = now_ms();
    for (size_t i = 0; i < g_refs.n; i++) {
        aref_t *r = g_refs.v[i];
        if (r->is_main || r->gone || !r->running) continue;
        json_t *o = json_obj();
        json_set_str(o, "id", r->id);
        json_set_str(o, "agent", r->agent);
        json_set_int(o, "ageMs", now - r->run_started);
        if (r->activity) json_set_str(o, "activity", r->activity);
        if (r->job_id) json_set_str(o, "jobId", r->job_id);
        json_push(arr, o);
    }
    pthread_mutex_unlock(&g_mu);
    return arr;
}

bool agents_cancel(const char *id) {
    bool ok = false;
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_locked(id);
    if (r && !r->is_main && r->running && r->cancel) {
        *r->cancel = 1;
        ok = true;
    } else if (r && !r->is_main && r->running && r->app) {
        agent_abort(r->app->agent);
        ok = true;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

/* ---------- Agent Hub / session focus / RPC (omp AgentRegistry + AgentLifecycleManager subset) ---------- */

static const char *ref_status_locked(const aref_t *r) {
    if (r->killed) return "aborted";
    if (r->running) return "running";
    if (!r->app) return "parked";
    return "idle";
}

/* listed refs: every subagent of the live roster (Main and refs released with their session excluded) */
static bool listed_locked(const aref_t *r) { return !r->is_main && !r->gone; }

static json_t *ref_json_locked(const aref_t *r) {
    json_t *o = json_obj();
    json_set_str(o, "id", r->id);
    json_set_str(o, "displayName", r->agent);
    json_set_str(o, "kind", "sub");
    if (r->parent_id) json_set_str(o, "parentId", r->parent_id);
    json_set_str(o, "status", ref_status_locked(r));
    json_set_int(o, "createdAt", r->started_at);
    json_set_int(o, "lastActivity", r->last_activity ? r->last_activity : r->started_at);
    if (r->gist && r->running) json_set_str(o, "activity", r->gist);
    json_set_str(o, "agent", r->agent);
    if (r->model) json_set_str(o, "model", r->model);
    if (r->app) {
        json_set_str(o, "thinking", effort_name(r->app->thinking));
        if (r->app->model) {
            json_set_bool(o, "reasoning", r->app->model->reasoning);
            json_set_int(o, "contextWindow", r->app->model->context_window);
        }
    }
    json_set_bool(o, "live", r->app != NULL);
    if (r->session_file) json_set_str(o, "sessionFile", r->session_file);
    if (r->output_path) json_set_str(o, "outputPath", r->output_path);
    if (r->read_only) json_set_bool(o, "readOnly", true);
    if (r->parent_call) json_set_str(o, "parentToolCallId", r->parent_call);
    if (r->job_id) json_set_str(o, "jobId", r->job_id);
    if (r->progress) {
        json_t *p = json_clone(r->progress);
        /* a running executor's active time as of now (omp reports it as of its last coalesced progress tick) */
        if (r->executor_run && r->running) json_set_int(p, "durationMs", r->active_ms + now_ms() - r->run_started);
        json_set(o, "progress", p);
    }
    return o;
}

json_t *agents_list(void) {
    json_t *arr = json_arr();
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_refs.n; i++)
        if (listed_locked(g_refs.v[i])) json_push(arr, ref_json_locked(g_refs.v[i]));
    pthread_mutex_unlock(&g_mu);
    return arr;
}

json_t *agents_get(const char *id) {
    pthread_mutex_lock(&g_mu);
    aref_t *r = id ? find_locked(id) : NULL;
    json_t *o = r && listed_locked(r) ? ref_json_locked(r) : NULL;
    pthread_mutex_unlock(&g_mu);
    return o;
}

void agents_counts(size_t *running, size_t *inspectable) {
    size_t run = 0, all = 0;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_refs.n; i++) {
        aref_t *r = g_refs.v[i];
        if (!listed_locked(r)) continue;
        all++;
        if (str_eq(ref_status_locked(r), "running")) run++;
    }
    pthread_mutex_unlock(&g_mu);
    if (running) *running = run;
    if (inspectable) *inspectable = all;
}

json_t *agents_messages(const char *id) {
    json_t *out = NULL;
    pthread_mutex_lock(&g_mu);
    aref_t *r = id ? find_any_locked(id) : NULL;
    if (r && !r->is_main) {
        if (r->app) {
            pthread_mutex_lock(&r->app->agent->mu);
            out = json_clone(r->app->agent->messages);
            pthread_mutex_unlock(&r->app->agent->mu);
        } else if (r->transcript) {
            out = json_clone(r->transcript);
        }
    }
    pthread_mutex_unlock(&g_mu);
    return out;
}

static char *unknown_agent(const char *id) {
    return xasprintf("Unknown agent \"%s\" — it was never registered or has been released. If a transcript exists, read "
                     "history://%s.",
                     id ? id : "", id ? id : "");
}

bool agents_kill(const char *id, char **err) {
    *err = NULL;
    pthread_mutex_lock(&g_mu);
    aref_t *r = id ? find_locked(id) : NULL;
    if (!r || r->is_main) {
        pthread_mutex_unlock(&g_mu);
        *err = unknown_agent(id);
        return false;
    }
    bool was = r->killed;
    r->killed = true;
    r->last_activity = now_ms();
    if (r->running) {
        if (r->cancel) *r->cancel = 1;
        else if (r->app) agent_abort(r->app->agent);
    }
    pthread_mutex_unlock(&g_mu);
    if (!was) notify_change();
    return true;
}

/* ensureLive: a killed agent reopens, a parked one (session released) is rebuilt from its transcript */
bool agents_revive(const char *id, char **err) {
    *err = NULL;
    pthread_mutex_lock(&g_mu);
    aref_t *r = id ? find_locked(id) : NULL;
    if (!r || r->is_main) {
        pthread_mutex_unlock(&g_mu);
        *err = unknown_agent(id);
        return false;
    }
    if (r->killed && r->running) {
        pthread_mutex_unlock(&g_mu);
        *err = xasprintf("Agent \"%s\" is still shutting down after its kill; retry once it has stopped.", r->id);
        return false;
    }
    if (r->app) {
        bool changed = r->killed;
        r->killed = false;
        if (changed) r->last_activity = now_ms();
        pthread_mutex_unlock(&g_mu);
        if (changed) notify_change();
        return true;
    }
    app_t *spawner = r->spawner;
    char *agent = xstrdup(r->agent), *append = r->sys_append ? xstrdup(r->sys_append) : NULL;
    char *model = r->model ? xstrdup(r->model) : NULL;
    json_t *msgs = r->transcript ? json_clone(r->transcript) : NULL;
    pthread_mutex_unlock(&g_mu);
    if (!spawner) {
        *err = xasprintf("Agent \"%s\" was killed and cannot be revived (no reviver registered). Read history://%s for what it did.",
                         id, id);
        free(agent), free(append), free(model), json_free(msgs);
        return false;
    }
    app_t *child = app_subagent_new(spawner, agent, model, append, err);
    if (child && msgs) {
        json_t *m;
        json_arr_foreach(msgs, i, m) agent_append_message(child->agent, json_clone(m));
    }
    free(agent), free(append), free(model), json_free(msgs);
    if (!child) return false;
    pthread_mutex_lock(&g_mu);
    r = find_locked(id);
    if (!r || r->app) {
        pthread_mutex_unlock(&g_mu);
        app_free(child);
        if (!r) *err = xasprintf("Agent \"%s\" changed before its persisted session could attach.", id);
        return r != NULL;
    }
    r->app = child;
    r->owned = true;
    r->killed = false;
    r->last_activity = now_ms();
    json_free(r->transcript);
    r->transcript = NULL;
    pthread_mutex_unlock(&g_mu);
    notify_change();
    return true;
}

typedef struct {
    aref_t *ref;
    app_t *child;
    char *text;
} chat_t;

static void chat_free(void *p) {
    chat_t *c = p;
    free(c->text);
    free(c);
}

static char *chat_body(bgjob_t *job, void *ud, bool *failed) {
    chat_t *c = ud;
    bool aborted;
    char *error = NULL;
    char *text = run_turn_ex(c->ref, c->child, c->text, job_cancel_flag(job), &aborted, &error, true, NULL);
    *failed = aborted || error;
    publish_output(c->ref, text, false, !aborted && !error);
    free(text);
    free(error);
    return xstrdup("");
}

/* session.prompt(text, {streamingBehavior: "steer"}) on a subagent: revive it, then steer a running turn or start an
 * idle one (the user's own message: the reply stays in the agent's transcript, nothing is delivered to its parent) */
bool agents_prompt(const char *id, const char *text, char **err) {
    *err = NULL;
    if (!agents_revive(id, err)) return false;
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_locked(id);
    if (!r || !r->app) {
        pthread_mutex_unlock(&g_mu);
        *err = xasprintf("Subagent not running: %s", id);
        return false;
    }
    app_t *app = r->app;
    if (r->running || agent_is_running(app->agent)) {
        pthread_mutex_unlock(&g_mu);
        agent_steer(app->agent, msg_user_text(text));
        return true;
    }
    app_t *spawner = r->spawner;
    if (!spawner || !spawner->env || !spawner->env->jobs) {
        pthread_mutex_unlock(&g_mu);
        *err = xasprintf("Subagent not running: %s", id);
        return false;
    }
    chat_t *c = xcalloc(1, sizeof *c);
    c->ref = r;
    c->child = app;
    c->text = xstrdup(text);
    r->running = true; /* claimed before the job thread starts */
    char *rid = xstrdup(r->id);
    pthread_mutex_unlock(&g_mu);
    /* the user's own turn on the agent: no background job of its parent (hidden, never delivered) */
    char *jid = jobs_start(spawner->env->jobs, "task", rid, rid, rid, true, chat_body, c, chat_free);
    jobs_set_quiet(spawner->env->jobs, jid);
    free(jid);
    free(rid);
    notify_change();
    return true;
}

app_t *agents_pin(const char *id) {
    pthread_mutex_lock(&g_mu);
    aref_t *r = id ? find_locked(id) : NULL;
    app_t *a = r && !r->is_main && r->app ? r->app : NULL;
    if (a) r->pins++;
    pthread_mutex_unlock(&g_mu);
    return a;
}

void agents_unpin(app_t *a) {
    if (!a) return;
    app_t *kill = NULL;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_refs.n; i++) {
        aref_t *r = g_refs.v[i];
        if (r->app != a || r->pins <= 0) continue;
        r->pins--;
        /* its spawner released it while a view held it: finish the release now */
        if (!r->pins && !r->spawner && r->owned && !r->running) {
            if (!r->transcript) r->transcript = json_clone(a->agent->messages);
            r->app = NULL;
            r->gone = true;
            kill = a;
        }
        break;
    }
    pthread_mutex_unlock(&g_mu);
    if (kill) {
        app_free(kill);
        notify_change();
    }
}

char *agents_parent_of(const char *id) {
    char *res = NULL;
    pthread_mutex_lock(&g_mu);
    aref_t *r = id ? find_locked(id) : NULL;
    if (r && r->parent_id) res = xstrdup(r->parent_id);
    pthread_mutex_unlock(&g_mu);
    return res;
}

/* getAsyncJobTree: jobs of `root` and of every live subagent below it (each subagent session keeps its own job
 * manager here, so a job's owner is the agent whose manager holds it); running ones plus the `recent_limit` newest
 * settled ones, with the owning agents and the ancestors linking them to `root`. Owned. */
static int job_newest_first(const void *a, const void *b) {
    int64_t x = json_get_int(*(const json_t *const *)a, "startedAt", 0), y = json_get_int(*(const json_t *const *)b, "startedAt", 0);
    return x > y ? -1 : x < y;
}

void agents_request_job_captions(app_t *root) {
    const char *root_id = agents_id_of(root);
    VEC(aref_t *) refs = {0};
    json_t *parents = json_obj();
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_refs.n; i++) {
        aref_t *r = g_refs.v[i];
        if (!r->is_main && r->parent_id) json_set_str(parents, r->id, r->parent_id);
    }
    for (size_t i = 0; i < g_refs.n; i++) {
        aref_t *r = g_refs.v[i];
        if (r->is_main || r->gone || !r->app || !r->app->env || !r->app->env->jobs) continue;
        bool below = false;
        const char *p = r->parent_id;
        for (int guard = 0; p && guard < 64 && !below; guard++) {
            if (root_id && str_eq(p, root_id)) below = true;
            else p = json_get_str(parents, p);
        }
        if (!below) continue;
        r->pins++; /* held while its jobs are read */
        vec_push(&refs, r);
    }
    pthread_mutex_unlock(&g_mu);
    job_captions_request(root);
    for (size_t i = 0; i < refs.n; i++) job_captions_request(refs.v[i]->app);
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < refs.n; i++) refs.v[i]->pins--;
    pthread_mutex_unlock(&g_mu);
    vec_free(&refs);
    json_free(parents);
}

json_t *agents_job_tree(app_t *root, size_t recent_limit) {
    json_t *out = json_obj(), *running = json_arr(), *recent_all = json_arr(), *agents = json_arr();
    const char *root_id = agents_id_of(root);
    json_set_str(out, "rootId", root_id ? root_id : MAIN_AGENT_ID);
    /* the managers in the tree: root's, then each live subagent whose lineage reaches root */
    VEC(app_t *) apps = {0};
    strvec_t owners = {0};
    json_t *parents = json_obj();
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_refs.n; i++) {
        aref_t *r = g_refs.v[i];
        if (!r->is_main && r->parent_id) json_set_str(parents, r->id, r->parent_id);
    }
    for (size_t i = 0; i < g_refs.n; i++) {
        aref_t *r = g_refs.v[i];
        if (r->is_main || r->gone || !r->app || !r->app->env || !r->app->env->jobs) continue;
        bool below = false;
        const char *p = r->parent_id;
        for (int guard = 0; p && guard < 64 && !below; guard++) {
            if (root_id && str_eq(p, root_id)) below = true;
            else p = json_get_str(parents, p);
        }
        if (!below) continue;
        r->pins++; /* held while its jobs are read */
        vec_push(&apps, r->app);
        strvec_push(&owners, r->id);
    }
    pthread_mutex_unlock(&g_mu);
    for (size_t k = 0; k <= apps.n; k++) {
        app_t *a = k == 0 ? root : apps.v[k - 1];
        if (!a || !a->env || !a->env->jobs) continue;
        json_t *snap = jobs_snapshot(a->env->jobs), *j;
        json_arr_foreach(snap, i, j) {
            json_t *c = json_clone(j);
            if (k > 0) json_set_str(c, "ownerId", owners.v[k - 1]);
            json_push(str_eq(json_get_str(j, "status"), "running") ? running : recent_all, c);
        }
        json_free(snap);
    }
    for (size_t k = 0; k < apps.n; k++) agents_unpin(apps.v[k]);
    vec_free(&apps);
    strvec_free(&owners);
    /* newest settled first, capped */
    size_t n = json_len(recent_all);
    json_t **v = xcalloc(n + 1, sizeof *v);
    for (size_t i = 0; i < n; i++) v[i] = json_at(recent_all, i);
    qsort(v, n, sizeof *v, job_newest_first);
    json_t *recent = json_arr();
    for (size_t i = 0; i < n && i < recent_limit; i++) json_push(recent, json_clone(v[i]));
    free(v);
    json_free(recent_all);
    /* agents: every owner and its ancestors below root; a task job's agent links to its owner when unregistered */
    json_t *seen = json_obj(), *j;
    for (int pass = 0; pass < 2; pass++) {
        json_arr_foreach(pass ? recent : running, i, j) {
            const char *aid = json_get_str(j, "agentId");
            if (aid && !json_get(parents, aid)) json_set_str(parents, aid, json_get_str(j, "ownerId") ? json_get_str(j, "ownerId") : (root_id ? root_id : MAIN_AGENT_ID));
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        json_arr_foreach(pass ? recent : running, i, j) {
            const char *id = json_get_str(j, "ownerId");
            for (int guard = 0; id && guard < 64 && !(root_id && str_eq(id, root_id)) && !json_get(seen, id); guard++) {
                json_set_bool(seen, id, true);
                json_t *ag = json_obj();
                json_set_str(ag, "id", id);
                const char *p = json_get_str(parents, id);
                if (p) json_set_str(ag, "parentId", p);
                json_push(agents, ag);
                id = p;
            }
        }
    }
    json_free(seen);
    json_free(parents);
    json_set(out, "running", running);
    json_set(out, "recent", recent);
    json_set(out, "agents", agents);
    return out;
}
