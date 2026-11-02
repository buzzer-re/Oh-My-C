/* costgate.c - port of omp session/cost-gate.ts (CostGate: warn once at session.warnCost, refuse the next model
 * request at session.maxCost) and the agent loop's `--max-time` deadline check, wired through the agent's
 * before_model_call hook. One gate per top-level session: subagent and side-request spend reaches it through
 * app->total_cost. */
#include "coding/costgate.h"

#include "config/settings.h"

#include <math.h>

static struct {
    app_t *app;
    double recorded;   /* the session file's spend when the gate was seeded */
    double base_total; /* app->total_cost at that moment */
    bool warned;
    bool refused;      /* a request was refused since the last take */
    int64_t deadline;
} G;

static double spent(void) { return G.recorded + (G.app->total_cost - G.base_total); }

static bool threshold(const char *key, double *out) {
    json_t *v = settings_get(key);
    if (!json_is_num(v)) return false;
    double d = v->u.num.d;
    if (!isfinite(d) || d <= 0) return false;
    *out = d;
    return true;
}

static char *usd(double amount) { return xasprintf("$%.*f", amount < 0.01 ? 4 : 2, amount); }

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
    char *s = usd(spent()), *c = usd(cap);
    char *m = xasprintf("Cost cap reached: this session spent %s of its %s cap (--max-cost / session.maxCost), so the "
                        "next model request was not sent.",
                        s, c);
    G.refused = true;
    notice(a, "error", m);
    free(m);
    free(s);
    free(c);
    return true;
}

void costgate_attach(app_t *a) {
    G.app = a;
    G.recorded = 0;
    json_t *m;
    json_arr_foreach(a->agent->messages, i, m) {
        if (str_eq(json_get_str(m, "role"), "assistant"))
            G.recorded += json_get_num(json_get(json_get(m, "usage"), "cost"), "total", 0);
    }
    G.base_total = a->total_cost;
    double warn;
    /* a session resumed past its warning threshold was warned in the run that crossed it */
    G.warned = threshold("session.warnCost", &warn) && G.recorded >= warn;
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
