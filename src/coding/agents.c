/* agents.c - agent registry, task spawns, peer messaging, agent:// and history:// (see agents.h). */
#include "agents.h"

#include "app.h"
#include "config/settings.h"
#include "core/fs.h"
#include "prompt/template.h"
#include "session/session.h"
#include "tools/jobs.h"

#include <ctype.h>
#include <pthread.h>

typedef struct aref {
    char *id, *agent, *parent_id;
    app_t *app;     /* live session (owned for subagents, borrowed for Main) */
    app_t *spawner; /* session that spawned it (its jobs receive the results) */
    bool owned, running, aborted, gone, is_main;
    char *output;
    bool has_output;
    int64_t started_at, published_at, run_started;
    json_t *transcript; /* retained messages after the session is released */
    strvec_t inbox;     /* "[mN] from: body" for the wait tool */
    strvec_t pending;   /* rendered IRC texts for an idle top-level session */
    strvec_t owed;      /* agents this one messaged during its current run */
    int waiting;
    char *activity;
    char *job_id;
    volatile sig_atomic_t *cancel; /* running job's cancel flag */
    task_card_t *card;             /* progress card of the spawning task call (first run only) */
    int card_idx;
} aref_t;

/* ---------- task cards ---------- */

struct task_card {
    pthread_mutex_t mu;
    int refs;
    tool_env_t *env;
    char *call_id;
    json_t *details;
    int64_t started_at;
    int nspawn, settled, failed;
};

task_card_t *task_card_new(tool_env_t *env, const char *tool_call_id, json_t *details) {
    task_card_t *c = xcalloc(1, sizeof *c);
    pthread_mutex_init(&c->mu, NULL);
    c->refs = 1;
    c->env = env;
    c->call_id = xstrdup(tool_call_id ? tool_call_id : "");
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
    free(c->call_id);
    free(c);
}

json_t *task_card_snapshot(task_card_t *c) {
    pthread_mutex_lock(&c->mu);
    json_t *d = json_clone(c->details);
    pthread_mutex_unlock(&c->mu);
    return d;
}

/* push the current snapshot to the UI (called without c->mu held) */
static void card_emit(task_card_t *c) {
    tool_env_t *env = c->env;
    if (!env || !env->ui.tool_frame) return;
    env->ui.tool_frame(env->ui.ud, c->call_id, task_card_snapshot(c));
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

/* one subagent event folded into its progress row; returns true when the row changed */
static bool card_on_event(task_card_t *c, int idx, int64_t started, const agent_event_t *ev, int64_t context_window) {
    bool changed = false;
    pthread_mutex_lock(&c->mu);
    json_t *row = card_row_locked(c, idx);
    if (row) {
        if (ev->type == AGENT_EV_TOOL_START && ev->tool_name) {
            json_set_str(row, "currentTool", ev->tool_name);
            static const char *const KEYS[] = {"path", "command", "pattern", "query", "url", "file", "op"};
            const char *argv = NULL;
            for (size_t k = 0; k < ARRAY_LEN(KEYS) && !argv; k++) argv = json_get_str(ev->args, KEYS[k]);
            if (argv) json_set_str(row, "currentToolArgs", argv);
            else json_del(row, "currentToolArgs");
            if (ev->intent) json_set_str(row, "currentToolIntent", ev->intent);
            else json_del(row, "currentToolIntent");
            json_set_int(row, "currentToolStartMs", now_ms());
            json_set_int(row, "toolCount", json_get_int(row, "toolCount", 0) + 1);
            changed = true;
        } else if (ev->type == AGENT_EV_TOOL_END && ev->tool_name) {
            json_t *recent = json_get_arr(row, "recentTools");
            json_t *e = json_obj();
            json_set_str(e, "tool", ev->tool_name);
            if (json_get_str(row, "currentToolArgs")) json_set_str(e, "args", json_get_str(row, "currentToolArgs"));
            if (json_get_str(row, "currentToolIntent")) json_set_str(e, "intent", json_get_str(row, "currentToolIntent"));
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
            if (tot) json_set_int(row, "contextTokens", tot);
            if (context_window > 0) json_set_int(row, "contextWindow", context_window);
            double cost = json_get_num(json_get_obj(u, "cost"), "total", 0);
            json_set_num(row, "cost", json_get_num(row, "cost", 0) + cost);
            changed = true;
        }
        if (changed) json_set_int(row, "durationMs", now_ms() - started);
    }
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
    free(r->id);
    free(r->agent);
    free(r->parent_id);
    free(r->output);
    json_free(r->transcript);
    strvec_free(&r->inbox);
    strvec_free(&r->pending);
    strvec_free(&r->owed);
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
        if (r->app && r->owned && !r->running) {
            if (!r->transcript) r->transcript = json_clone(r->app->agent->messages);
            vec_push(&kill, r->app);
            r->app = NULL;
        }
        r->gone = r->gone || !r->app;
    }
    pthread_mutex_unlock(&g_mu);
    for (size_t i = 0; i < kill.n; i++) app_free(kill.v[i]);
    vec_free(&kill);
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

static void on_child_event(void *ud, const agent_event_t *ev) {
    aref_t *r = ud;
    if (ev->type == AGENT_EV_TOOL_START || ev->type == AGENT_EV_TOOL_END || ev->type == AGENT_EV_MESSAGE_END) {
        pthread_mutex_lock(&g_mu);
        task_card_t *card = card_retain(r->card);
        int idx = r->card_idx;
        int64_t started = r->run_started;
        int64_t win = r->app && r->app->model ? r->app->model->context_window : 0;
        pthread_mutex_unlock(&g_mu);
        if (card) {
            if (card_on_event(card, idx, started, ev, win)) card_emit(card);
            task_card_release(card);
        }
    }
    if (ev->type != AGENT_EV_TOOL_START && ev->type != AGENT_EV_TURN_START) return;
    pthread_mutex_lock(&g_mu);
    free(r->activity);
    r->activity = ev->type == AGENT_EV_TOOL_START && ev->tool_name ? xasprintf("running %s", ev->tool_name) : xstrdup("thinking");
    pthread_mutex_unlock(&g_mu);
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

/* run one prompt on a child session; returns the last assistant text (owned) */
static char *run_turn(aref_t *r, app_t *child, const char *prompt, volatile sig_atomic_t *cancel, bool *aborted, char **error) {
    *aborted = false;
    *error = NULL;
    pthread_mutex_lock(&g_mu);
    r->running = true;
    r->run_started = now_ms();
    r->cancel = cancel;
    strvec_free(&r->owed);
    pthread_mutex_unlock(&g_mu);
    pthread_t wt;
    watch_t w = {cancel, child->agent, 0};
    bool watching = cancel && pthread_create(&wt, NULL, cancel_watch, &w) == 0;
    agent_subscribe(child->agent, on_child_event, r);
    app_prompt(child, prompt, NULL);
    agent_unsubscribe(child->agent, on_child_event, r);
    w.done = 1;
    if (watching) pthread_join(wt, NULL);
    const json_t *last = last_assistant(child->agent->messages);
    char *text = last ? ai_message_text(last) : xstrdup("");
    const char *stop = json_get_str(last, "stopReason");
    *aborted = str_eq(stop, "aborted") || (cancel && *cancel);
    if (str_eq(stop, "error") && json_get_str(last, "errorMessage")) *error = xstrdup(json_get_str(last, "errorMessage"));
    pthread_mutex_lock(&g_mu);
    r->running = false;
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
    return text;
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
    char *agent, *task, *context, *source, *model;
} spawn_t;

static void spawn_free(void *p) {
    spawn_t *s = p;
    free(s->agent);
    free(s->task);
    free(s->context);
    free(s->source);
    free(s->model);
    free(s);
}

static char *peer_roster_prompt(aref_t *self, const char *agent_body, const char *context, const char *source,
                                const char *model) {
    json_t *c = json_obj();
    json_set_str(c, "agent", agent_body ? agent_body : "");
    if (context && *context) json_set_str(c, "context", context);
    json_set_str(c, "ircSelfId", self->id);
    json_set_bool(c, "structured", false);
    json_t *peers = json_arr();
    pthread_mutex_lock(&g_mu);
    size_t shown = 0, omitted = 0;
    for (size_t i = 0; i < g_refs.n; i++) {
        aref_t *r = g_refs.v[i];
        if (r == self || r->gone || !r->app) continue;
        if (shown >= 20) {
            omitted++;
            continue;
        }
        json_t *p = json_obj();
        json_set_str(p, "id", r->id);
        json_set_str(p, "displayName", r->is_main ? "Main" : r->agent);
        json_set_str(p, "kind", r->is_main ? "main" : "subagent");
        json_set_str(p, "status", r->running ? "running" : "idle");
        if (r->activity) json_set_str(p, "activity", r->activity);
        json_push(peers, p);
        shown++;
    }
    pthread_mutex_unlock(&g_mu);
    json_set(c, "ircPeers", peers);
    if (omitted) json_set_int(c, "ircOmittedCount", (int64_t)omitted);
    char *s = prompt_render("coding-agent/prompts/system/subagent-system-prompt.md", c);
    json_free(c);
    if (source) {
        json_t *sc = json_obj();
        json_set_str(sc, "sourceSession", source);
        json_set_str(sc, "model", model ? model : "");
        char *seance = prompt_render("coding-agent/prompts/system/seance-assignment.md", sc);
        json_free(sc);
        char *both = xasprintf("%s\n\n%s", s, seance);
        free(s);
        free(seance);
        s = both;
    }
    return s;
}

/* builds the child session (on the job thread); NULL + *err on failure */
static app_t *make_child(spawn_t *sp, char **err) {
    json_t *defs = agent_defs_load(sp->env->cwd);
    json_t *def = json_get(defs, sp->agent);
    json_t *msgs = NULL;
    char *model = sp->model ? xstrdup(sp->model) : NULL;
    if (sp->source) {
        char *file = session_resolve(sp->source, sp->env->cwd);
        session_t *src = file ? session_open(file, err) : NULL;
        if (!file && !*err) *err = xasprintf("Source session not found: %s", sp->source);
        free(file);
        if (!src) {
            json_free(defs);
            free(model);
            return NULL;
        }
        session_context_t sc;
        session_build_context(src, &sc);
        msgs = sc.messages;
        sc.messages = NULL;
        if (!model && sc.model) model = xstrdup(sc.model);
        session_context_free(&sc);
        session_free(src);
    }
    char *append = peer_roster_prompt(sp->ref, json_get_str(def, "body"), sp->context, sp->source, model);
    app_t *child = app_subagent_new(sp->parent, sp->agent, model, append, err);
    free(append);
    free(model);
    json_free(defs);
    if (child && msgs) {
        json_t *m;
        json_arr_foreach(msgs, i, m) agent_append_message(child->agent, json_clone(m));
    }
    json_free(msgs);
    return child;
}

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
        pthread_mutex_unlock(&g_mu);
    }
    if (!child) {
        permit_release();
        *failed = true;
        char *s = render_summary(r->id, sp->agent, "failed", mono_ms() - t0, "", err ? err : "failed to start", false);
        free(err);
        return s;
    }
    json_t *uc = json_obj();
    json_set_str(uc, "assignment", sp->task);
    char *up = prompt_render("coding-agent/prompts/system/subagent-user-prompt.md", uc);
    json_free(uc);
    job_set_output(job, "running");
    pthread_mutex_lock(&g_mu);
    task_card_t *card = card_retain(r->card);
    int cidx = r->card_idx;
    r->run_started = now_ms();
    pthread_mutex_unlock(&g_mu);
    if (card) {
        card_set_status(card, cidx, "running");
        card_emit(card);
        task_card_release(card);
    }
    bool aborted;
    char *error = NULL;
    char *text = run_turn(r, child, up, cancel, &aborted, &error);
    free(up);
    permit_release();
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
        card_emit(card);
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
    vec_push(&g_refs, r);
    if (spec->card) task_card_set_id(spec->card, spec->card_index, id, false);
    pthread_mutex_unlock(&g_mu);
    free(base);
    UNUSED(err);
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
    sp->source = spec->source_session ? xstrdup(spec->source_session) : NULL;
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

static char *render_irc(const char *from, const char *body, const char *reply_to, bool interrupting, bool relay) {
    json_t *c = json_obj();
    json_set_str(c, "from", from);
    json_set_str(c, "message", body);
    if (reply_to) json_set_str(c, "replyTo", reply_to);
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
} wake_t;

static void wake_free(void *p) {
    wake_t *w = p;
    free(w->from);
    free(w->text);
    free(w);
}

static void deliver(const char *from, const char *to, const char *body, const char **outcome);

static char *wake_body(bgjob_t *job, void *ud, bool *failed) {
    wake_t *w = ud;
    bool aborted;
    char *error = NULL;
    int64_t t0 = mono_ms();
    char *text = run_turn(w->ref, w->child, w->text, job_cancel_flag(job), &aborted, &error);
    *failed = aborted || error;
    char *res;
    if (w->to_spawner) {
        char *sum = render_summary(w->ref->id, w->ref->agent, aborted ? "aborted" : error ? "failed" : "completed", mono_ms() - t0,
                                   text, error, aborted);
        char *hint = follow_up_hint(w->ref->id, aborted);
        res = xasprintf("%s\n\n%s", sum, hint);
        free(sum);
        free(hint);
    } else {
        /* answer the sender directly */
        char *t = str_trim(text);
        if (*t) {
            const char *oc;
            deliver(w->ref->id, w->from, t, &oc);
        }
        free(t);
        res = xstrdup("");
    }
    free(text);
    free(error);
    return res;
}

/* deliver one message; caller does not hold g_mu */
static void deliver(const char *from, const char *to, const char *body, const char **outcome) {
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_locked(to);
    if (!r || !r->app || r->gone) {
        pthread_mutex_unlock(&g_mu);
        *outcome = NULL;
        return;
    }
    /* a message from `to`'s awaited peer settles that debt */
    for (size_t i = 0; i < r->owed.n; i++)
        if (str_eq(r->owed.v[i], from)) {
            free(r->owed.v[i]);
            r->owed.v[i] = r->owed.v[--r->owed.n];
            break;
        }
    char *mid = xasprintf("m%llu", (unsigned long long)++g_msg_seq);
    if (r->waiting) {
        strvec_push_own(&r->inbox, xasprintf("[%s] %s: %s", mid, from, body));
        free(mid);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        *outcome = "injected";
        return;
    }
    free(mid);
    app_t *app = r->app;
    bool running = r->running || agent_is_running(app->agent);
    if (running) {
        pthread_mutex_unlock(&g_mu);
        char *txt = render_irc(from, body, NULL, true, false);
        json_t *m = msg_user_text(txt);
        json_set_bool(m, "synthetic", true);
        json_set_str(m, "customType", "irc");
        agent_steer(app->agent, m);
        free(txt);
        *outcome = "injected";
        return;
    }
    if (r->is_main) {
        char *txt = render_irc(from, body, NULL, false, false);
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
        return;
    }
    const char *spawner_id = NULL;
    aref_t *sp = find_app_locked(spawner);
    if (sp) spawner_id = sp->id;
    wake_t *w = xcalloc(1, sizeof *w);
    w->child = app;
    w->ref = r;
    w->from = xstrdup(from);
    w->to_spawner = str_eq(from, spawner_id);
    w->text = render_irc(from, body, NULL, false, true);
    r->running = true; /* claimed before the job thread starts */
    char *id = xstrdup(r->id);
    pthread_mutex_unlock(&g_mu);
    char *jid = jobs_start(spawner->env->jobs, "task", id, id, id, false, wake_body, w, wake_free);
    if (!w->to_spawner) jobs_set_quiet(spawner->env->jobs, jid);
    free(jid);
    free(id);
    *outcome = "woken";
}

char *agents_send(tool_env_t *env, const char *to, const char *body, bool *is_error) {
    *is_error = true;
    const char *self = env_id(env);
    if (!self) return xstrdup("Peer messaging is unavailable in this session.");
    char *from = xstrdup(self);
    char *dest = str_trim(to ? to : "");
    char *res = NULL;
    if (!*dest) res = xstrdup("A recipient is required.");
    else if (!body || !*body) res = xstrdup("agent:// messages require non-empty content.");
    else if (str_eq(dest, from)) res = xstrdup("Cannot send a message to yourself.");
    if (res) {
        free(from);
        free(dest);
        return res;
    }
    if (str_eq(dest, "all")) {
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
            deliver(from, targets.v[i], body, &oc);
            if (oc) ok++;
            sb_appendf(&rows, "\n- %s: %s", targets.v[i], oc ? oc : "failed — not running");
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
    const char *oc;
    deliver(from, dest, body, &oc);
    if (oc) {
        pthread_mutex_lock(&g_mu);
        aref_t *me = find_locked(from);
        if (me && !strvec_contains(&me->owed, dest)) strvec_push(&me->owed, dest);
        pthread_mutex_unlock(&g_mu);
        *is_error = false;
        res = str_eq(oc, "woken") ? xasprintf("Delivered to %s (was idle; woken).", dest) : xasprintf("Delivered to %s.", dest);
    } else {
        res = xasprintf("Failed: %s is not running.", dest);
    }
    free(from);
    free(dest);
    return res;
}

char *agents_take_message(tool_env_t *env) {
    const char *self = env_id(env);
    if (!self) return NULL;
    char *m = NULL;
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_locked(self);
    if (r && r->inbox.n) {
        m = r->inbox.v[0];
        memmove(r->inbox.v, r->inbox.v + 1, (r->inbox.n - 1) * sizeof(char *));
        r->inbox.n--;
    }
    pthread_mutex_unlock(&g_mu);
    return m;
}

void agents_wait_enter(tool_env_t *env, bool on) {
    const char *self = env_id(env);
    if (!self) return;
    pthread_mutex_lock(&g_mu);
    aref_t *r = find_locked(self);
    if (r) r->waiting += on ? 1 : -1;
    if (r && r->waiting < 0) r->waiting = 0;
    pthread_mutex_unlock(&g_mu);
}

int agents_reply_state(tool_env_t *env, char **who) {
    *who = NULL;
    const char *self = env_id(env);
    if (!self) return 0;
    int st = 0;
    sb_t b = SB_INIT;
    pthread_mutex_lock(&g_mu);
    aref_t *me = find_locked(self);
    for (size_t i = 0; me && i < me->owed.n; i++) {
        aref_t *p = find_locked(me->owed.v[i]);
        if (p && p->running) st = 1;
    }
    if (me && st == 0 && me->owed.n) {
        st = 2;
        for (size_t i = 0; i < me->owed.n; i++) sb_appendf(&b, "%s`%s`", i ? ", " : "", me->owed.v[i]);
    }
    pthread_mutex_unlock(&g_mu);
    if (st == 2) *who = sb_detach(&b);
    else sb_free(&b);
    return st;
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
