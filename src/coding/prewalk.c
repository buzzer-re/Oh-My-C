/* prewalk.c - one-shot model handoff (session/prewalk.ts). See prewalk.h. */
#include "tools/toolcmd.h"
#include "coding/prewalk.h"

#include "coding/app.h"
#include "coding/slashcmd.h"
#include "config/model_resolver.h"
#include "config/settings.h"
#include "prompt/template.h"

#include <ctype.h>
#include <strings.h>

#define PLAN_TYPE "prewalk-plan"
#define CONTINUE_TYPE "prewalk-continue"
#define CHECKLIST_TYPE "prewalk-checklist"

struct prewalk {
    pthread_mutex_t mu;
    const ai_model_t *target; /* armed target, NULL when disarmed */
    char *level;              /* its explicit thinking selector, NULL = keep */
    bool plan_injected, continue_pending, todo_seen;
    /* the completed handoff (PrewalkHandoff), kept to return to the planning model on /new */
    const ai_model_t *h_source, *h_target;
    char *h_source_level, *h_target_level;
    bool settings_enabled; /* last prewalk.enabled seen by the settings listener */
};

prewalk_t *prewalk_new(void) {
    prewalk_t *p = xcalloc(1, sizeof *p);
    pthread_mutex_init(&p->mu, NULL);
    return p;
}

static void clear_handoff(prewalk_t *p) {
    p->h_source = p->h_target = NULL;
    free(p->h_source_level);
    free(p->h_target_level);
    p->h_source_level = p->h_target_level = NULL;
}

static void settings_changed(void *ud);

void prewalk_close(app_t *a) {
    if (!a->prewalk) return;
    settings_off_change(settings_changed, a);
    prewalk_free(a->prewalk);
    a->prewalk = NULL;
}

void prewalk_free(prewalk_t *p) {
    if (!p) return;
    free(p->level);
    clear_handoff(p);
    pthread_mutex_destroy(&p->mu);
    free(p);
}

bool prewalk_is_plan_nudge(const json_t *msg) { return str_eq(json_get_str(msg, "customType"), PLAN_TYPE); }

static json_t *custom_msg(const char *type, const char *content) {
    json_t *m = msg_developer_text(content ? content : "");
    json_set_str(m, "attribution", "agent");
    json_set_str(m, "customType", type);
    json_set_bool(m, "display", false);
    return m;
}

static void steer_prompt(app_t *a, const char *type, const char *file) {
    char *path = xasprintf("coding-agent/prompts/system/%s.md", file);
    char *t = prompt_load(path);
    free(path);
    agent_steer(a->agent, custom_msg(type, t));
    free(t);
}

/* configuredThinkingLevel(): auto, else the level effective on the active model */
static const char *configured_level(app_t *a) {
    if (a->auto_thinking) return "auto";
    return effort_name(model_clamp_effort(a->model, a->thinking));
}

/* prewalkWouldBeNoop */
static bool is_noop(app_t *a, const ai_model_t *target, const char *level) {
    if (a->model != target) return false;
    if (!level || str_eq(level, "inherit")) return true;
    bool ta = str_eq(level, "auto"), ca = a->auto_thinking;
    if (ta != ca) return false;
    if (ta) return true;
    ai_effort_t te, ce;
    if (effort_parse(level, &te) != 0) return true;
    if (effort_parse(configured_level(a), &ce) != 0) ce = EFFORT_OFF;
    ai_effort_t tr = te == EFFORT_OFF ? EFFORT_OFF : model_clamp_effort(target, te);
    ai_effort_t cr = ce == EFFORT_OFF ? EFFORT_OFF : model_clamp_effort(target, ce);
    return tr == cr;
}

static void clear_state(prewalk_t *p) {
    p->target = NULL;
    free(p->level);
    p->level = NULL;
    p->plan_injected = p->continue_pending = p->todo_seen = false;
}

/* #scrubPlanNudge: drop the plan nudge from the live context and pending queue (and a pending continuation) */
static void scrub_plan_nudge(app_t *a, bool include_continuation) {
    agent_t *ag = a->agent;
    bool changed = false;
    pthread_mutex_lock(&ag->mu);
    for (size_t i = json_len(ag->messages); i-- > 0;)
        if (prewalk_is_plan_nudge(json_at(ag->messages, i))) json_free(json_arr_remove(ag->messages, i));
    for (size_t i = json_len(ag->steering); i-- > 0;) {
        const json_t *m = json_at(ag->steering, i);
        if (prewalk_is_plan_nudge(m) || (include_continuation && str_eq(json_get_str(m, "customType"), CONTINUE_TYPE))) {
            json_free(json_arr_remove(ag->steering, i));
            changed = true;
        }
    }
    pthread_mutex_unlock(&ag->mu);
    if (changed) agent_queue_changed(ag);
}

static void disarm_noop(app_t *a, const ai_model_t *target) {
    clear_state(a->prewalk);
    app_notice(a, "info", "Prewalk: target %s/%s already matches the active model and thinking level; nothing to switch.",
               target->provider, target->id);
}

static bool switch_model(app_t *a, const ai_model_t *m, const char *level) {
    char *err = NULL;
    int rc = app_set_model_temporary(a, m, level, "prewalk", &err);
    if (rc != 0) {
        app_notice(a, "warning", "Prewalk: %s", err ? err : "switch failed");
        free(err);
    }
    return rc == 0;
}

static void set_level(app_t *a, const char *level) {
    if (str_eq(level, "auto")) app_set_thinking_auto(a);
    else if (level && strcmp(level, "inherit")) app_set_thinking_level(a, level);
}

/* #armPending: arm and return the plan nudge for the caller to deliver (NULL for a no-op target) */
static json_t *arm_pending(app_t *a, const ai_model_t *target, const char *level) {
    prewalk_t *p = a->prewalk;
    if (is_noop(a, target, level)) {
        disarm_noop(a, target);
        return NULL;
    }
    p->target = target;
    free(p->level);
    p->level = level ? xstrdup(level) : NULL;
    p->plan_injected = p->continue_pending = true;
    p->todo_seen = false;
    app_notice(a, "info", "Prewalk: armed for %s/%s \xe2\x80\x94 will switch at the first edit/write once the todo list exists.",
               target->provider, target->id);
    char *t = prompt_load("coding-agent/prompts/system/prewalk-plan.md");
    json_t *m = custom_msg(PLAN_TYPE, t);
    free(t);
    return m;
}

bool prewalk_arm(app_t *a, const ai_model_t *target, const char *level) {
    prewalk_t *p = a->prewalk;
    if (!p) return false;
    pthread_mutex_lock(&p->mu);
    bool ok;
    if (p->target) {
        app_notice(a, "info", "Prewalk: already armed for %s/%s, waiting for the first edit/write.", p->target->provider,
                   p->target->id);
        ok = p->target == target && ((!p->level && !level) || (p->level && level && !strcmp(p->level, level)));
    } else {
        json_t *m = arm_pending(a, target, level);
        ok = m != NULL;
        if (m) agent_steer(a->agent, m);
    }
    pthread_mutex_unlock(&p->mu);
    return ok;
}

bool prewalk_disarm(app_t *a) {
    prewalk_t *p = a->prewalk;
    if (!p) return false;
    pthread_mutex_lock(&p->mu);
    const ai_model_t *t = p->target;
    if (t) {
        scrub_plan_nudge(a, true);
        clear_state(p);
        app_notice(a, "info", "Prewalk: disarmed; staying on the active model instead of switching to %s/%s.", t->provider, t->id);
    }
    pthread_mutex_unlock(&p->mu);
    return t != NULL;
}

const ai_model_t *prewalk_target(app_t *a) { return a->prewalk ? a->prewalk->target : NULL; }

void prewalk_release_handoff(app_t *a) {
    if (!a->prewalk) return;
    pthread_mutex_lock(&a->prewalk->mu);
    clear_handoff(a->prewalk);
    pthread_mutex_unlock(&a->prewalk->mu);
}

int prewalk_restart(app_t *a, const ai_model_t *source, const char *source_level, const ai_model_t *target,
                    const char *target_level) {
    prewalk_t *p = a->prewalk;
    if (!p) return 2;
    const ai_model_t *active = p->target;
    if (active && (active != target || !str_eq(p->level, target_level))) {
        prewalk_arm(a, target, target_level);
        return 2;
    }
    if (!switch_model(a, source, source_level)) return 2;
    if (!active) return prewalk_arm(a, target, target_level) ? 0 : 1;
    pthread_mutex_lock(&p->mu);
    int r = 0;
    if (is_noop(a, p->target, p->level)) {
        const ai_model_t *t = p->target;
        scrub_plan_nudge(a, false);
        disarm_noop(a, t);
        r = 1;
    }
    pthread_mutex_unlock(&p->mu);
    return r;
}

/* prewalkImplementationTool: a direct edit/write (unless it touched no files), or a `bash` run through a tool command
 * whose wrapped tool resolved to the `write` approval tier (not an MCP tool declared read-only); exec-tier commands
 * count no more than a plain shell command (omp 60ef4a2e80) */
static bool implementation_action(const json_t *r) {
    const char *tn = json_get_str(r, "toolName");
    if (str_eq(tn, "edit") || str_eq(tn, "write")) {
        /* a write that touched no files (a peer message, a reply, a cancellation) is coordination, not implementation */
        return !json_get_bool(json_get(r, "details"), "touchesNoFiles", false);
    }
    const json_t *d = toolcmd_dispatches(tn, json_get(r, "details")), *x;
    json_arr_foreach(d, i, x) {
        if (str_eq(json_get_str(x, "mode"), "execute") && str_eq(json_get_str(x, "tier"), "write") &&
            !json_get_bool(x, "readOnlyHint", false))
            return true;
    }
    return false;
}

static bool todo_active(app_t *a) {
    for (size_t i = 0; i < a->agent->tools.n; i++)
        if (str_eq(a->agent->tools.v[i]->name, "todo")) return true;
    return false;
}

void prewalk_on_turn_end(app_t *a, const agent_event_t *ev) {
    prewalk_t *p = a->prewalk;
    if (!p || ev->type != AGENT_EV_TURN_END || !str_eq(json_get_str(ev->message, "role"), "assistant")) return;
    pthread_mutex_lock(&p->mu);
    const ai_model_t *target = p->target;
    if (!target) goto out;
    if (is_noop(a, target, p->level)) {
        scrub_plan_nudge(a, false);
        disarm_noop(a, target);
        goto out;
    }
    const json_t *r;
    json_arr_foreach(ev->tool_results, i, r) if (str_eq(json_get_str(r, "toolName"), "todo") && !json_get_bool(r, "isError", false))
        p->todo_seen = true;
    bool has_results = json_len(ev->tool_results) > 0;
    if (p->plan_injected && has_results) p->continue_pending = true;
    else if (p->continue_pending) {
        p->continue_pending = false;
        steer_prompt(a, CONTINUE_TYPE, "prewalk-continue");
    }
    bool gate = p->todo_seen || !todo_active(a);
    const json_t *action = NULL;
    if (gate) json_arr_foreach(ev->tool_results, i, r) if (!action && implementation_action(r)) action = r;
    if (!action) {
        if (!p->plan_injected) {
            p->plan_injected = p->continue_pending = true;
            steer_prompt(a, PLAN_TYPE, "prewalk-plan");
            app_notice(a, "info", "Prewalk: injected deep-plan nudge.");
        }
        goto out;
    }
    scrub_plan_nudge(a, false);
    const ai_model_t *source = a->model;
    char *source_level = xstrdup(configured_level(a));
    char *level = p->level ? xstrdup(p->level) : NULL;
    char *tool = xstrdup(json_get_str(action, "toolName"));
    bool ok = switch_model(a, target, level);
    clear_state(p);
    if (ok) {
        clear_handoff(p);
        if (source) {
            p->h_source = source;
            p->h_source_level = source_level;
            source_level = NULL;
            p->h_target = target;
            p->h_target_level = xstrdup(configured_level(a));
        }
        app_notice(a, "info", "Prewalk: switched to %s/%s after first %s call.", target->provider, target->id, tool);
        steer_prompt(a, CHECKLIST_TYPE, "prewalk-checklist");
    }
    free(tool);
    free(level);
    free(source_level);
out:
    pthread_mutex_unlock(&p->mu);
}

/* #resolvePrewalkTarget: a model with credentials, else the reason (owned) */
static const ai_model_t *resolve_target(const char *selector, char **level, char **why) {
    *level = NULL;
    *why = NULL;
    const ai_model_t *m = slash_resolve_session_model(selector, level);
    if (!m) {
        *why = xasprintf("model \"%s\" not found", selector);
        return NULL;
    }
    if (mr_provider_disabled(m->provider)) {
        *why = xasprintf("provider \"%s\" is disabled", m->provider);
        free(*level);
        *level = NULL;
        return NULL;
    }
    char *key = app_api_key_for(m);
    bool ok = key || catalog_provider_keyless(m->provider);
    free(key);
    if (!ok) {
        *why = xasprintf("no API key for %s/%s", m->provider, m->id);
        free(*level);
        *level = NULL;
        return NULL;
    }
    return m;
}

/* #resolveDefaultPrewalk: prewalk.into, with the "Prewalk not armed" warning */
static const ai_model_t *default_target(app_t *a, const char *into, char **level) {
    char *why = NULL;
    const ai_model_t *m = resolve_target(into ? into : settings_str("prewalk.into", "@smol"), level, &why);
    if (!m) app_notice(a, "warning", "Prewalk not armed: %s.", why);
    free(why);
    return m;
}

static void settings_changed(void *ud) {
    app_t *a = ud;
    prewalk_t *p = a->prewalk;
    bool en = settings_bool("prewalk.enabled", false);
    if (en == p->settings_enabled) return;
    p->settings_enabled = en;
    if (!en) {
        prewalk_disarm(a);
        return;
    }
    if (p->target) return;
    char *lv = NULL;
    const ai_model_t *m = default_target(a, NULL, &lv);
    if (m) prewalk_arm(a, m, lv);
    free(lv);
}

void prewalk_startup(app_t *a, int cli, const char *into, bool restoring) {
    prewalk_t *p = a->prewalk;
    if (!p) return;
    p->settings_enabled = settings_bool("prewalk.enabled", false);
    settings_on_change(settings_changed, a);
    bool enabled = cli < 0 ? false : cli > 0 ? true : !restoring && p->settings_enabled;
    if (!enabled) return;
    char *lv = NULL;
    const ai_model_t *m = default_target(a, into, &lv);
    if (!m) return;
    /* like startup in omp: the guidance goes in at the first completed turn, not into an empty transcript; a
     * revived session already on the target its recorded handoff switched to has spent the prewalk */
    pthread_mutex_lock(&p->mu);
    if (is_noop(a, m, lv)) disarm_noop(a, m);
    else {
        p->target = m;
        p->level = lv;
        lv = NULL;
    }
    pthread_mutex_unlock(&p->mu);
    free(lv);
}

void prewalk_new_session(app_t *a) {
    prewalk_t *p = a->prewalk;
    if (!p) return;
    bool enabled = settings_bool("prewalk.enabled", false);
    pthread_mutex_lock(&p->mu);
    bool restore = enabled && p->h_source && p->h_target == a->model &&
                   str_eq(p->h_target_level, configured_level(a));
    const ai_model_t *src = p->h_source;
    char *src_level = p->h_source_level ? xstrdup(p->h_source_level) : NULL;
    scrub_plan_nudge(a, true);
    clear_state(p);
    clear_handoff(p);
    pthread_mutex_unlock(&p->mu);
    if (restore) {
        char *err = NULL;
        if (app_set_model_temporary(a, src, src_level, "prewalk", &err) != 0) {
            app_notice(a, "warning", "Prewalk: could not restore %s/%s: %s", src->provider, src->id, err ? err : "");
            free(err);
        } else
            set_level(a, src_level);
    }
    free(src_level);
    if (!enabled) return;
    char *lv = NULL;
    const ai_model_t *m = default_target(a, NULL, &lv);
    if (m) {
        pthread_mutex_lock(&p->mu);
        if (is_noop(a, m, lv)) disarm_noop(a, m);
        else {
            p->target = m;
            p->level = lv;
            lv = NULL;
        }
        pthread_mutex_unlock(&p->mu);
    }
    free(lv);
}

/* ---------- /prewalk (builtin-modes.ts) ---------- */

static strvec_t split_ws(const char *s) {
    strvec_t v = {0};
    while (s && *s) {
        while (*s && isspace((unsigned char)*s)) s++;
        const char *b = s;
        while (*s && !isspace((unsigned char)*s)) s++;
        if (s > b) {
            char *t = xstrndup(b, (size_t)(s - b));
            strvec_push(&v, t);
            free(t);
        }
    }
    return v;
}

/* resolveSessionModelSelector + the auth check: the model, or NULL with *msg the usage text */
static const ai_model_t *resolve_cmd(const char *sel, char **level, char **msg) {
    *level = NULL;
    const ai_model_t *m = slash_resolve_session_model(sel, level);
    if (!m) {
        *msg = xasprintf("Model \"%s\" not found. Run \"omc models\" to see available models.", sel); /* resolveCliModel */
        return NULL;
    }
    char *key = app_api_key_for(m);
    bool ok = key || catalog_provider_keyless(m->provider);
    free(key);
    if (!ok) {
        *msg = xasprintf("No API key for %s/%s", m->provider, m->id);
        free(*level);
        *level = NULL;
        return NULL;
    }
    return m;
}

char *prewalk_command(app_t *a, const char *args, bool *failed) {
    *failed = false;
    strvec_t w = split_ws(args);
    char *kw = w.n ? xstrdup(w.v[0]) : xstrdup("");
    for (char *c = kw; *c; c++) *c = (char)tolower((unsigned char)*c);
    bool restart = !strcmp(kw, "restart"), off = !strcmp(kw, "off");
    size_t skip = restart || off ? 1 : 0, n = w.n - skip;
    char *out = NULL;
    if (n > (restart ? 2u : off ? 0u : 1u)) {
        out = xstrdup("Usage: /prewalk [into] | restart [from] [into] | off");
        *failed = true;
        goto done;
    }
    if (off) {
        bool armed = prewalk_disarm(a);
        out = xstrdup(armed ? "Prewalk off: canceled this session's pending handoff; keeping the active model."
                            : "Prewalk already off for this session; keeping the active model.");
        goto done;
    }
    const char *into = (restart ? (n > 1 ? w.v[skip + 1] : NULL) : (n > 0 ? w.v[skip] : NULL));
    if (!into) into = settings_str("prewalk.into", "@smol");
    char *tlv = NULL, *msg = NULL;
    const ai_model_t *t = resolve_cmd(into, &tlv, &msg);
    if (!t) {
        out = msg;
        *failed = true;
        goto done;
    }
    char *tlabel = xasprintf("%s (%s/%s)", into, t->provider, t->id);
    if (restart) {
        const char *from = n > 0 ? w.v[skip] : settings_str("prewalk.from", "@default");
        char *slv = NULL;
        const ai_model_t *s = resolve_cmd(from, &slv, &msg);
        if (!s) {
            out = msg;
            *failed = true;
        } else {
            int r = prewalk_restart(a, s, slv, t, tlv);
            char *slabel = xasprintf("%s (%s/%s)", from, s->provider, s->id);
            if (r == 0)
                out = xasprintf("Prewalk restarted: using %s for planning, then switching to %s at the next edit/write "
                                "(todo-gated).",
                                slabel, tlabel);
            else if (r == 1)
                out = xasprintf("Prewalk reset: using %s; %s resolves to the same model and thinking level, so no handoff "
                                "was armed.",
                                slabel, into);
            free(slabel);
        }
        free(slv);
    } else if (prewalk_arm(a, t, tlv))
        out = xasprintf("Prewalk on: switching to %s/%s at the next edit/write (todo-gated).", t->provider, t->id);
    free(tlabel);
    free(tlv);
done:
    free(kw);
    strvec_free(&w);
    return out;
}
