/* costgate.c - port of omp session/cost-gate.ts (CostGate: warn once at session.warnCost, refuse the next model
 * request at session.maxCost) and the agent loop's `--max-time` deadline check, wired through the agent's
 * before_model_call hook. One gate per session tree: subagents and background requests check it and add their spend. */
#include "coding/costgate.h"

#include "config/settings.h"

#include <math.h>
#include <pthread.h>

static pthread_mutex_t g_side_mu = PTHREAD_MUTEX_INITIALIZER;

static struct {
    app_t *app;
    double recorded;   /* the session file's spend when the gate was seeded */
    double base_total; /* app->total_cost at that moment */
    double side;       /* background spend the session's totals do not show (skill description compression) */
    bool warned;
    bool refused;      /* a request was refused since the last take */
    int64_t deadline;
} G;

static double spent(void) {
    pthread_mutex_lock(&g_side_mu);
    double side = G.side;
    pthread_mutex_unlock(&g_side_mu);
    return G.recorded + side + (G.app->total_cost - G.base_total);
}

static bool threshold(const char *key, double *out) {
    json_t *v = settings_get(key);
    if (!json_is_num(v)) return false;
    double d = v->u.num.d;
    if (!isfinite(d) || d <= 0) return false;
    *out = d;
    return true;
}

static char *usd(double amount) { return xasprintf("$%.*f", amount < 0.01 ? 4 : 2, amount); }

static char *refusal_text(double cap) {
    char *s = usd(spent()), *c = usd(cap);
    char *m = xasprintf("Cost cap reached: this session spent %s of its %s cap (--max-cost / session.maxCost), so the "
                        "next model request was not sent.",
                        s, c);
    free(s);
    free(c);
    return m;
}

static void notice(app_t *a, const char *level, const char *text) {
    if (a->notice) a->notice(a->notice_ud, level, text);
    else fprintf(stderr, "%s\n", text);
}

/* record(): warn once when the total first reaches session.warnCost */
static void check_warn(app_t *a) {
    double warn;
    if (G.warned || !threshold("session.warnCost", &warn) || spent() < warn) return;
    G.warned = true;
    char *s = usd(spent()), *w = usd(warn);
    char *m = xasprintf("Cost warning: this session has spent %s, past its %s warning threshold (--warn-cost / "
                        "session.warnCost).",
                        s, w);
    notice(a, "warning", m);
    free(m);
    free(s);
    free(w);
}

/* the tree's gate for a subagent: the cap is read on each check, so one set on the main thread applies to the next
 * request of every running subagent (the warning stays the root's, the refusal notice is the subagent's own) */
static bool subagent_before_model_call(void *ud, agent_t *ag) {
    UNUSED(ag);
    app_t *a = ud;
    if (!G.app) return false;
    check_warn(G.app);
    double cap;
    if (!threshold("session.maxCost", &cap) || spent() < cap) return false;
    if (a->notice) {
        char *m = refusal_text(cap);
        a->notice(a->notice_ud, "error", m);
        free(m);
    }
    return true;
}

static void listener(void *ud, const agent_event_t *ev) {
    app_t *a = ud;
    if (ev->type == AGENT_EV_MESSAGE_END && str_eq(json_get_str(ev->message, "role"), "assistant")) check_warn(a);
}

static bool before_model_call(void *ud, agent_t *ag) {
    UNUSED(ag);
    app_t *a = ud;
    if (a != G.app) return false;
    if (G.deadline && now_ms() >= G.deadline) return true;
    check_warn(a);
    double cap;
    if (!threshold("session.maxCost", &cap) || spent() < cap) return false;
    char *m = refusal_text(cap);
    G.refused = true;
    notice(a, "error", m);
    free(m);
    return true;
}

bool costgate_admits(void) {
    double cap;
    return !G.app || !threshold("session.maxCost", &cap) || spent() < cap;
}

void costgate_record(double cost) {
    if (!(cost > 0) || !isfinite(cost)) return;
    pthread_mutex_lock(&g_side_mu);
    G.side += cost;
    pthread_mutex_unlock(&g_side_mu);
}

void costgate_add(app_t *a, double cost) {
    a->total_cost += cost;
    if (a != G.app) costgate_record(cost);
}

void costgate_merge_child(app_t *parent, double child_total) {
    parent->total_cost += child_total;
    if (parent == G.app) G.base_total += child_total; /* already counted when the child spent it */
}

void costgate_reseed(app_t *a) {
    if (a != G.app) return;
    G.recorded = 0;
    pthread_mutex_lock(&g_side_mu);
    G.side = 0;
    pthread_mutex_unlock(&g_side_mu);
    json_t *m;
    pthread_mutex_lock(&a->agent->mu);
    json_arr_foreach(a->agent->messages, i, m) {
        if (str_eq(json_get_str(m, "role"), "assistant"))
            G.recorded += json_get_num(json_get(json_get(m, "usage"), "cost"), "total", 0);
    }
    pthread_mutex_unlock(&a->agent->mu);
    G.base_total = a->total_cost;
    double warn;
    /* a session resumed past its warning threshold was warned in the run that crossed it */
    G.warned = threshold("session.warnCost", &warn) && G.recorded >= warn;
}

void costgate_attach_subagent(app_t *child) { child->agent->hooks.before_model_call = subagent_before_model_call; }

void costgate_attach(app_t *a) {
    G.app = a;
    costgate_reseed(a);
    a->agent->hooks.before_model_call = before_model_call;
    agent_subscribe(a->agent, listener, a);
}

void app_set_deadline(app_t *a, int64_t deadline_ms) {
    if (a == G.app) G.deadline = deadline_ms;
}


bool costgate_take_refusal(app_t *a) {
    if (a != G.app || !G.refused) return false;
    G.refused = false;
    return true;
}
