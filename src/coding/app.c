#include "app.h"
#include "coding/costgate.h"
#include "coding/turn_recovery.h"
#include "coding/dcr.h"
#include "coding/autothink.h"
#include "coding/modelctl.h"
#include "coding/cachewarm.h"
#include "coding/power.h"
#include "coding/goal.h"
#include "coding/guards.h"
#include "coding/prewalk.h"
#include "coding/keywords.h"
#include "coding/thinkloop.h"
#include "coding/title.h"
#include "coding/branchsum.h"
#include "coding/compaction.h"
#include "coding/skilldesc.h"
#include "coding/advisor.h"
#include "coding/evalhost.h"
#include "coding/autolearn.h"
#include "coding/memory.h"
#include "coding/xdnotice.h"
#include "coding/imagegen.h"
#include "tools/approval.h"
#include "tools/artifacts.h"
#include "tools/jobs.h"
#include "coding/jobcaption.h"
#include "tools/patch.h"
#include "tools/xdev.h"
#include "agents.h"

#include "config/auth.h"
#include "config/settings.h"
#include "config/compaction_point.h"
#include "config/model_resolver.h"
#include "core/fs.h"
#include "core/proc.h"
#include "coding/ttsr.h"
#include "coding/extracmds.h"
#include "dap/dap.h"
#include "lsp/lsp.h"
#include "tools/gh.h"
#include "mcp/mcp.h"
#include "prompt/template.h"
#include "ai/anthropic_threads.h"
#include "ai/responses_ws.h"
#include "ai/provider_anthropic.h"
#include "ai/ai_images.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>

/* ---------- model resolution (config/model_resolver.c) ---------- */

static bool apply_chillax(app_t *a, const char *text);

static void level_out(const char *lv, ai_effort_t *effort_out, bool *has_effort) {
    ai_effort_t e;
    if (lv && effort_parse(lv, &e) == 0) {
        if (effort_out) *effort_out = e;
        if (has_effort) *has_effort = true;
    }
}

/* a user/config selector: role aliases, `provider/id`, bare/fuzzy ids, `:level` suffixes. Resolved
 * over the available chat models first (omp resolveModelRoleValue), then over the whole catalog so
 * an explicit selection of an uncredentialed model still names it (omp resolveCliModel). */
const ai_model_t *app_resolve_model_pattern(const char *pattern, ai_effort_t *effort_out, bool *has_effort) {
    if (has_effort) *has_effort = false;
    if (!pattern || !*pattern) return NULL;
    mr_result_t r;
    const ai_model_t *m = NULL;
    if (mr_resolve_value(pattern, NULL, &r)) m = r.model;
    else {
        mr_result_clear(&r);
        mr_pool_t all = {0};
        mr_pool_all(&all, "all");
        if (mr_resolve_value(pattern, &all, &r)) m = r.model;
        mr_pool_free(&all);
    }
    if (m) level_out(r.level, effort_out, has_effort);
    mr_result_clear(&r);
    return m;
}

/* a role's model (resolveModelRoleValue("@role") over the role's candidate pool) */
const ai_model_t *app_role_model(const char *role, ai_effort_t *effort_out, bool *has_effort) {
    if (has_effort) *has_effort = false;
    const char *lv = NULL;
    const ai_model_t *m = mr_resolve_role(role, &lv);
    if (m) level_out(lv, effort_out, has_effort);
    return m;
}

char *app_api_key_for(const ai_model_t *m) { return m ? auth_get_api_key(m->provider) : NULL; }

const ai_model_t *app_default_model(void) { return mr_default_model(NULL); }

char *app_model_label(app_t *a) {
    return a->model ? xasprintf("%s/%s", a->model->provider, a->model->id) : xstrdup("(no model)");
}

const char *app_session_file(app_t *a) { return a->session ? a->session->file : NULL; }

/* tool_env active_model: web_search's `web/hosted` searches through the session model */
static const ai_model_t *app_tool_active_model(tool_env_t *env, char **session_id) {
    app_t *a = env->session;
    if (!a) return NULL;
    if (session_id) *session_id = a->agent && a->agent->session_id ? xstrdup(a->agent->session_id) : NULL;
    return a->model;
}

/* ---------- persistence listener ---------- */

static void notice(app_t *a, const char *level, const char *fmt, ...) PRINTF_LIKE(3, 4);
static void notice(app_t *a, const char *level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = xvasprintf(fmt, ap);
    va_end(ap);
    LOGI("[%s] %s", level, s);
    if (a->notice) a->notice(a->notice_ud, level, s);
    free(s);
}

void app_notice(app_t *a, const char *level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = xvasprintf(fmt, ap);
    va_end(ap);
    LOGI("[%s] %s", level, s);
    if (a->notice) a->notice(a->notice_ud, level, s);
    free(s);
}

/* ---------- structured session events (AgentSessionEvent) ---------- */

bool app_emit(app_t *a, json_t *ev) {
    bool taken = a && a->session_event;
    if (taken) a->session_event(a->session_event_ud, ev);
    json_free(ev);
    return taken;
}

const char *app_wire_thinking(app_t *a) {
    if (!a->model || !a->model->reasoning) return NULL;
    return effort_name(model_clamp_effort(a->model, a->thinking));
}

/* model_changed / thinking_level_changed after a model or thinking mutation (only for real changes) */
void app_model_state_changed(app_t *a, const ai_model_t *prev_model, const char *prev_thinking) {
    if (!a->session_event) return;
    if (prev_model != a->model) {
        json_t *e = json_obj();
        json_set_str(e, "type", "model_changed");
        app_emit(a, e);
    }
    const char *now = app_wire_thinking(a);
    if (!str_eq(now, prev_thinking) || (!now) != (!prev_thinking)) {
        json_t *e = json_obj();
        json_set_str(e, "type", "thinking_level_changed");
        if (now) json_set_str(e, "thinkingLevel", now);
        if (a->auto_thinking) json_set_str(e, "configured", "auto");
        app_emit(a, e);
    }
}

json_t *app_queued_messages(app_t *a) {
    json_t *o = json_obj(), *st = json_arr(), *fu = json_arr(), *m;
    pthread_mutex_lock(&a->agent->mu);
    json_arr_foreach(a->agent->steering, i, m) if (agent_msg_user_authored(m)) {
        char *t = ai_message_text(m);
        json_push_str(st, t);
        free(t);
    }
    json_arr_foreach(a->agent->follow_up, i, m) if (agent_msg_user_authored(m)) {
        char *t = ai_message_text(m);
        json_push_str(fu, t);
        free(t);
    }
    pthread_mutex_unlock(&a->agent->mu);
    json_set(o, "steering", st);
    json_set(o, "followUp", fu);
    json_set_int(o, "liveSteered", 0);
    return o;
}

void app_queue_changed(app_t *a) {
    if (!a->session_event) return;
    json_t *snap = app_queued_messages(a);
    pthread_mutex_lock(&a->mu);
    bool same = a->queue_snapshot && json_equal(a->queue_snapshot, snap);
    if (!same) {
        json_free(a->queue_snapshot);
        a->queue_snapshot = json_clone(snap);
    }
    pthread_mutex_unlock(&a->mu);
    if (same) {
        json_free(snap);
        return;
    }
    json_t *e = json_obj();
    json_set_str(e, "type", "queue_update");
    json_set(e, "steering", json_clone(json_get(snap, "steering")));
    json_set(e, "followUp", json_clone(json_get(snap, "followUp")));
    json_set_int(e, "liveSteered", 0);
    json_free(snap);
    app_emit(a, e);
}

static void hook_queue_changed(void *ud, agent_t *ag) {
    UNUSED(ag);
    app_queue_changed(ud);
}

static void maybe_title(app_t *a);

/* a background job settled: running sessions pick it up at the next turn boundary, idle ones are woken */
static void app_job_settled(void *ud) {
    app_t *a = ud;
    if (!agent_is_running(a->agent) && a->wake) a->wake(a->wake_ud);
}

/* omp async-result custom message details: {jobs: [{jobId, type, status, agentId?, label, durationMs}]} (owned) */
static json_t *job_batch_details(const json_t *done) {
    json_t *d = json_obj(), *jobs = json_arr(), *j;
    json_arr_foreach(done, i, j) {
        json_t *e = json_obj();
        if (json_get_str(j, "jobId")) json_set_str(e, "jobId", json_get_str(j, "jobId"));
        if (json_get_str(j, "type")) json_set_str(e, "type", json_get_str(j, "type"));
        if (json_get_str(j, "status")) json_set_str(e, "status", json_get_str(j, "status"));
        if (json_get_str(j, "agentId")) json_set_str(e, "agentId", json_get_str(j, "agentId"));
        if (json_get_str(j, "label")) json_set_str(e, "label", json_get_str(j, "label"));
        if (json_get(j, "durationMs")) json_set_int(e, "durationMs", json_get_int(j, "durationMs", 0));
        json_push(jobs, e);
    }
    json_set(d, "jobs", jobs);
    return d;
}

static json_t *job_results_message(app_t *a) {
    json_t *done = jobs_take_finished(a->env->jobs);
    if (!json_len(done)) {
        json_free(done);
        return NULL;
    }
    char *txt = jobs_render_results(done);
    json_t *m = msg_user_text(txt);
    json_set_bool(m, "synthetic", true);
    json_set_str(m, "customType", "async-result");
    json_set(m, "details", job_batch_details(done));
    json_free(done);
    free(txt);
    return m;
}

static void on_agent_event(void *ud, const agent_event_t *ev) {
    app_t *a = ud;
    if (ev->type == AGENT_EV_AGENT_START) strvec_free(&a->retry_notes);
    if ((ev->type == AGENT_EV_AGENT_START || ev->type == AGENT_EV_TURN_START) && a->rewind_graveyard) {
        json_free(a->rewind_graveyard);
        a->rewind_graveyard = NULL;
    }
    /* power.sleepPrevention: held while a run is in flight (subagents run inside the parent's run) */
    if (ev->type == AGENT_EV_AGENT_START && !a->opts.subagent) power_acquire();
    if (ev->type == AGENT_EV_AGENT_END && !a->opts.subagent) power_release();
    /* #relaxOnDelivery: apply `chillax` when a queued message is delivered, so a steer relaxes the running turn from its next
     * request (the run's first prompt already applied it in apply_auto_thinking, and doing so again changes nothing) */
    if (ev->type == AGENT_EV_MESSAGE_END && str_eq(json_get_str(ev->message, "role"), "user") && !json_get_bool(ev->message, "synthetic", false) &&
        !a->opts.subagent) {
        const json_t *ct = json_get(ev->message, "content");
        const char *tx = json_is_str(ct) ? json_sv(ct) : NULL;
        const json_t *b;
        if (!tx) json_arr_foreach(ct, bi, b) {
            if (str_eq(json_get_str(b, "type"), "text")) {
                tx = json_get_str(b, "text");
                break;
            }
        }
        apply_chillax(a, tx);
    }
    if (ev->type == AGENT_EV_MESSAGE_END && str_eq(json_get_str(ev->message, "role"), "assistant")) {
        cachewarm_on_response(a->warmer, ev->message);
        /* completedAt: local completion time stamped by the session at message_end (persisted with the message) */
        if (ev->message) json_set_int((json_t *)ev->message, "completedAt", now_ms());
        const char *stop = json_get_str(ev->message, "stopReason");
        if (!str_eq(stop, "error") && !str_eq(stop, "aborted")) {
            if (a->retry_saga > 0) { /* closeRetrySaga success */
                json_t *e = json_obj();
                json_set_str(e, "type", "auto_retry_end");
                json_set_bool(e, "success", true);
                json_set_int(e, "attempt", a->retry_saga);
                a->retry_saga = 0;
                app_emit(a, e);
            }
            if (a->retry_notes.n) {
                char *notes = strvec_join(&a->retry_notes, "\n");
                notice(a, "retry-recovered", "%s", notes);
                free(notes);
                strvec_free(&a->retry_notes);
            }
            if (a->fallback.active && !a->fallback.served && a->model) {
                a->fallback.served = true;
                char *k = mr_model_key(a->model);
                notice(a, "retry_fallback_succeeded", "%s\x1f%s", k, a->fallback.role ? a->fallback.role : "");
                free(k);
            }
        }
    }
    if (ev->type == AGENT_EV_TOOL_END) app_plan_note_tool_end(a, ev->tool_name, ev->result, ev->is_error);
    guards_on_event(a, ev); /* tool-call loop redirect at the turn end, ahead of async results */
    if (ev->type == AGENT_EV_TURN_END) prewalk_on_turn_end(a, ev);
    if (ev->type == AGENT_EV_TURN_END && a->env && jobs_undelivered(a->env->jobs)) {
        json_t *m = job_results_message(a);
        if (m) agent_steer(a->agent, m);
    }
    guards_mid_run_nudge(a, ev);
    app_goal_on_event(a, ev);
    title_on_event(a, ev);
    if (a->advisor) advisor_on_event(a->advisor, ev);
    autolearn_on_event(a->autolearn, ev);
    if (ev->type == AGENT_EV_TURN_START || ev->type == AGENT_EV_MESSAGE_START) {
        if (ev->type == AGENT_EV_TURN_START || str_eq(json_get_str(ev->message, "role"), "assistant")) ttsr_reset(a->ttsr);
        if (ev->type == AGENT_EV_MESSAGE_START && str_eq(json_get_str(ev->message, "role"), "assistant"))
            thinkloop_reset(a->thinkloop, a->model);
    }
    if (ev->type == AGENT_EV_TURN_END && a->pending_rewind && a->checkpoint_index > 0) {
        /* cut the context back to the checkpoint and retain only the report */
        pthread_mutex_lock(&a->agent->mu);
        json_t *msgs = a->agent->messages;
        /* the cut includes this turn's assistant message, which ev->message (and the listeners after this one,
         * e.g. RPC) still read: park the removed messages instead of freeing them during the event */
        if (!a->rewind_graveyard) a->rewind_graveyard = json_arr();
        while ((int64_t)json_len(msgs) > a->checkpoint_index) json_push(a->rewind_graveyard, json_arr_remove(msgs, json_len(msgs) - 1));
        pthread_mutex_unlock(&a->agent->mu);
        json_t *ctx = json_obj();
        json_set_str(ctx, "report", a->pending_rewind);
        char *txt = prompt_render("coding-agent/prompts/system/rewind-report.md", ctx);
        json_free(ctx);
        json_t *m = msg_user_text(txt);
        json_set_bool(m, "synthetic", true);
        if (a->session && a->checkpoint_entry) session_branch(a->session, a->checkpoint_entry);
        if (a->session) session_append_message(a->session, m);
        agent_append_message(a->agent, m);
        free(txt);
        free(a->pending_rewind);
        a->pending_rewind = NULL;
        a->checkpoint_index = 0;
        free(a->checkpoint_entry);
        a->checkpoint_entry = NULL;
        notice(a, "info", "Rewound to checkpoint; exploration replaced by report");
        return;
    }
    if (ev->type == AGENT_EV_MESSAGE_END && ev->message) {
        const char *role = json_get_str(ev->message, "role");
        if (a->checkpoint_pending && str_eq(role, "toolResult") && str_eq(json_get_str(ev->message, "toolName"), "checkpoint") &&
            !json_get_bool(ev->message, "isError", false)) {
            a->checkpoint_pending = false;
            a->checkpoint_index = (int64_t)json_len(a->agent->messages); /* result already appended */
            if (a->session) {
                const char *eid = session_append_message(a->session, ev->message);
                free(a->checkpoint_entry);
                a->checkpoint_entry = eid ? xstrdup(eid) : NULL;
                return; /* persisted here */
            }
        }
        /* TTSR discard mode: the interrupted partial response is dropped, never persisted */
        if (a->ttsr_interrupting && str_eq(role, "assistant") && str_eq(json_get_str(ev->message, "stopReason"), "aborted"))
            return;
        if (str_eq(role, "assistant") && !str_eq(json_get_str(ev->message, "stopReason"), "error")) {
            char *def = ttsr_take_deferred(a->ttsr);
            if (def) {
                json_t *m = msg_user_text(def);
                json_set_bool(m, "synthetic", true);
                agent_follow_up(a->agent, m);
                free(def);
            }
        }
        const char *ctype = json_get_str(ev->message, "customType");
        if (prewalk_is_plan_nudge(ev->message)) {
            /* one-run instruction: never returns from persisted history */
        } else if (role && a->session && ctype && (str_eq(role, "user") || str_eq(role, "developer"))) {
            /* hidden steering (goal context, continuations, budget limits, guard notices) persists as custom_message
             * like omp */
            char *txt = ai_message_text(ev->message);
            const json_t *det = json_get(ev->message, "details");
            const char *attr = json_get_str(ev->message, "attribution");
            if (det || attr) { /* async-result job rows, guard details, attribution */
                json_t *e = json_obj();
                json_set_str(e, "type", "custom_message");
                json_set_str(e, "customType", ctype);
                const json_t *mc = json_get(ev->message, "content"), *blk;
                bool has_image = false;
                json_arr_foreach(mc, bi, blk) has_image |= str_eq(json_get_str(blk, "type"), "image");
                /* a user-invoked skill with attachments keeps its content blocks */
                if (has_image && str_eq(ctype, "skill-prompt")) json_set(e, "content", json_clone(mc));
                else json_set_str(e, "content", txt);
                json_set_bool(e, "display", json_get_bool(ev->message, "display", false));
                if (det) json_set(e, "details", json_clone(det));
                if (attr) json_set_str(e, "attribution", attr);
                session_append_entry(a->session, e);
            } else session_append_custom_message(a->session, ctype, txt, false);
            free(txt);
        } else if (role && a->session) {
            session_append_message(a->session, ev->message);
        }
        if (str_eq(role, "assistant")) {
            json_t *u = json_get(ev->message, "usage");
            costgate_add(a, json_get_num(json_get(u, "cost"), "total", 0));
            __atomic_add_fetch(&a->output_tokens, json_get_int(u, "output", 0), __ATOMIC_SEQ_CST);
            int64_t ctx = json_get_int(u, "input", 0) + json_get_int(u, "cacheRead", 0) + json_get_int(u, "cacheWrite", 0) +
                          json_get_int(u, "output", 0);
            if (ctx > 0) a->last_context_tokens = ctx;
        }
    }
}

/* ---------- hooks ---------- */

/* settings-stream-fn.ts timeoutSecondsToMs: < 0 -> provider/env defaults (-1), 0 disables */
static int64_t timeout_seconds_ms(double v) {
    if (!isfinite(v) || v < 0) return -1;
    if (v == 0) return 0;
    return MAX((int64_t)1, (int64_t)(v * 1000));
}

/* per-request options shared by the main loop and side requests (prepareSimpleStreamOptions) */
static void fill_request_opts(app_t *a, ai_stream_opts_t *o) {
    /* omp resolveCacheRetention: explicit setting, else PI_CACHE_RETENTION (long|short|none), else "short" */
    const char *cr = settings_str("providers.cacheRetention", "auto");
    if (!cr || !*cr || str_eq(cr, "auto")) {
        const char *env = env_get("PI_CACHE_RETENTION");
        cr = env && (str_eq(env, "long") || str_eq(env, "short") || str_eq(env, "none")) ? env : "short";
    }
    o->cache_retention = cr;
    /* cfgThinkingBudgets: every level has a budget, configured or default (lives until this thread's next request) */
    static _Thread_local json_t *budgets;
    static const struct {
        const char *key, *path;
        int64_t def;
    } TB[] = {{"minimal", "thinkingBudgets.minimal", 1024}, {"low", "thinkingBudgets.low", 2048},   {"medium", "thinkingBudgets.medium", 8192},
              {"high", "thinkingBudgets.high", 16384},      {"xhigh", "thinkingBudgets.xhigh", 32768}, {"max", "thinkingBudgets.max", 32768}};
    json_free(budgets);
    budgets = json_obj();
    for (size_t i = 0; i < ARRAY_LEN(TB); i++) json_set_int(budgets, TB[i].key, settings_int(TB[i].path, TB[i].def));
    o->thinking_budgets = budgets;
    /* cfgSampling: a negative sampling setting means the provider default */
    double temp = settings_num("temperature", -1), top_p = settings_num("topP", -1);
    if (temp >= 0) o->temperature = temp;
    if (top_p >= 0) o->top_p = top_p;
    o->text_verbosity = settings_str("textVerbosity", "low");
    app_tier_request_opts(a, o);
    o->slow_hooks = &a->slow_hooks;
    o->first_event_timeout_ms = timeout_seconds_ms(settings_num("providers.streamFirstEventTimeoutSeconds", -1));
    o->idle_timeout_ms = timeout_seconds_ms(settings_num("providers.streamIdleTimeoutSeconds", -1));
    /* settings-stream-fn.ts maxRetryDelayMs: retry.maxDelayMs caps a provider's server-directed transport wait */
    int64_t max_delay = settings_int("retry.maxDelayMs", 5 * 60 * 1000);
    o->max_retry_delay_ms = max_delay > 0 ? max_delay : -1;
    const json_t *lim = settings_get("providers.maxInFlightRequests"); /* read once when the request starts */
    o->max_in_flight = json_is_obj(lim) ? lim : NULL;
    /* resolveWebsocketPreference: the setting opts in for standard Responses models, the model's own false still vetoes */
    if (!o->prefer_websockets) o->prefer_websockets = settings_bool("providers.openaiResponsesWebsockets", false) ? 1 : -1;
}

void app_fill_request_opts(app_t *a, ai_stream_opts_t *o) { fill_request_opts(a, o); }

/* session/session-metadata.ts buildSessionMetadata: the per-request `metadata`, so the backend buckets requests under
 * one session; the Claude account stays off every other provider. Interned: providers borrow it for the whole request,
 * and the set is bounded by the sessions and accounts of the process. */
const json_t *app_session_metadata(app_t *a, const char *provider) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static VEC(json_t *) interned;
    const char *sid = a->agent ? a->agent->session_id : NULL;
    if (str_empty(sid)) return NULL;
    json_t *cred = str_eq(provider, "anthropic") ? auth_get_credential_raw("anthropic") : NULL;
    const char *account = str_eq(json_get_str(cred, "type"), "oauth") ? json_get_str(cred, "accountId") : NULL;
    char *uid = anthropic_session_user_id(sid, account);
    json_free(cred);
    json_t *found = NULL;
    pthread_mutex_lock(&mu);
    for (size_t i = 0; i < interned.n && !found; i++)
        if (str_eq(json_get_str(interned.v[i], "user_id"), uid)) found = interned.v[i];
    if (!found) {
        found = json_obj();
        json_set_str(found, "user_id", uid);
        vec_push(&interned, found);
    }
    pthread_mutex_unlock(&mu);
    free(uid);
    return found;
}

/* #recordCacheWarmUsage: paid warm requests as off-transcript model_usage entries (session totals include them) */
void app_flush_warm_usage(app_t *a) {
    json_t *u = cachewarm_take_usage(a->warmer), *e;
    if (!a->opts.subagent) { /* job captions (off-transcript like warm requests) go on the root session */
        json_t *cap = job_captions_take_usage();
        if (cap) {
            if (!u) u = json_arr();
            json_arr_foreach(cap, i, e) json_push(u, json_clone(e));
            json_free(cap);
        }
    }
    json_arr_foreach(u, i, e) {
        costgate_add(a, json_get_num(json_get(json_get(e, "usage"), "cost"), "total", 0));
        if (!a->session) continue;
        json_t *entry = json_obj();
        json_set_str(entry, "type", "model_usage");
        const char *k;
        json_t *v;
        json_obj_foreach(e, j, k, v) json_set(entry, k, json_clone(v));
        session_append_entry(a->session, entry);
    }
    json_free(u);
}

static void hook_on_request(void *ud, agent_t *ag, const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o) {
    UNUSED(ag);
    app_t *a = ud;
    cachewarm_start(a->warmer, m, ctx, o); /* every main-loop request owns the warmer */
}

static void warm_settings_changed(void *ud) { cachewarm_mode_changed(((app_t *)ud)->warmer); }

/* the provider session state a request to the current model keeps (omp providerSessionState keys: per-endpoint
 * strict-tools / reasoning fallbacks, Responses chains, the Codex and Anthropic anchors, Antigravity, Duo
 * Workflow); /fresh prunes them and reports how many there were */
static void app_note_provider_state(app_t *a) {
    const ai_model_t *m = a->model;
    if (!m || !m->api) return;
    char *key = NULL;
    if (str_eq(m->api, "openai-completions"))
        key = xasprintf("openai-completions:%s:%s:%s", m->provider, m->base_url ? m->base_url : "", m->id);
    else if (str_eq(m->api, "openai-responses") || str_eq(m->api, "azure-openai-responses"))
        key = xasprintf("openai-responses:%s", m->provider);
    else if (str_eq(m->api, "openai-codex-responses")) key = xstrdup("openai-codex");
    else if (str_eq(m->api, "anthropic-messages")) key = xasprintf("anthropic:%s:%s", m->base_url ? m->base_url : "", m->id);
    else if (str_eq(m->provider, "google-antigravity")) key = xstrdup("google-antigravity-session-state");
    else if (str_eq(m->api, "gitlab-duo-workflow")) key = xasprintf("gitlab-duo-workflow:%s", m->provider);
    if (!key) return;
    pthread_mutex_lock(&a->mu);
    if (!strvec_contains(&a->provider_states, key)) strvec_push(&a->provider_states, key);
    pthread_mutex_unlock(&a->mu);
    free(key);
}

/* externalThinking with the `think` tool present on a model that can suppress native reasoning */
static bool external_thinking_active(app_t *a) {
    if (!settings_bool("externalThinking", false) || !think_supported(a->model)) return false;
    agent_tool_t *t = agent_find_tool(a->agent, "think");
    return t && !t->hidden;
}

/* session-tools reconcileThinkTool: the private scratchpad follows externalThinking and the active model */
static void think_reconcile(app_t *a) {
    if (!a->agent || a->opts.no_tools) return;
    bool want = settings_bool("externalThinking", false) && think_supported(a->model);
    agent_tool_t *t = agent_find_tool(a->agent, "think");
    if (want == (t != NULL)) return;
    if (want) agent_add_tool(a->agent, tool_think_new());
    else agent_remove_tool(a->agent, "think");
    tools_finalize(a->agent, a->opts.tools);
}

static void hook_prepare(void *ud, agent_t *ag, ai_stream_opts_t *o) {
    app_t *a = ud;
    app_flush_warm_usage(a);
    free(ag->api_key);
    ag->api_key = app_api_key_for(a->model);
    o->api_key = ag->api_key;
    if (external_thinking_active(a)) { /* sdk.ts: forceReasoningOff while the scratchpad replaces reasoning */
        o->force_reasoning_off = true;
        o->disable_reasoning = true;
        if (a->think_force) { /* the "external-thinking" tool choice: once, on the turn's first request */
            static __thread json_t *forced;
            json_free(forced);
            forced = extracmd_named_choice(a->model, "think");
            if (forced) o->tool_choice = forced;
        }
    }
    a->think_force = false;
    if (a->plan_force_tool_choice) { /* plan-mode-decision: the reminder continuation must call a tool */
        static json_t *required;
        if (!required) required = json_str("required");
        o->tool_choice = required;
        a->plan_force_tool_choice = false;
    }
    extracmd_prepare_request(a, o); /* /force */
    fill_request_opts(a, o);
    if (!o->metadata && ag->model) o->metadata = (json_t *)app_session_metadata(a, ag->model->provider);
    app_note_provider_state(a);
}

/* omp messages.ts wrapSteeringForModel: every steered user message reaches the model inside the
 * user-interjection envelope (text parts joined, images kept); NULL when there is none */
static json_t *wrap_steering(const json_t *messages) {
    json_t *out = NULL, *m;
    json_arr_foreach(messages, i, m) {
        if (!str_eq(json_get_str(m, "role"), "user") || !json_get_bool(m, "steering", false)) continue;
        const json_t *content = json_get(m, "content");
        sb_t text = SB_INIT;
        json_t *images = json_arr();
        bool any = false;
        if (json_is_str(content)) sb_append(&text, json_sv(content)), any = true;
        else {
            const json_t *b;
            json_arr_foreach(content, k, b) {
                if (str_eq(json_get_str(b, "type"), "text")) {
                    if (any) sb_putc(&text, '\n');
                    sb_append(&text, json_get_str(b, "text") ? json_get_str(b, "text") : "");
                    any = true;
                } else if (str_eq(json_get_str(b, "type"), "image")) {
                    json_push(images, json_clone(b));
                }
            }
        }
        if (!out) out = json_clone(messages);
        json_t *w = json_at(out, i);
        json_del(w, "steering");
        if (text.len) {
            json_t *ctx = json_obj();
            json_set_str(ctx, "message", sb_cstr(&text));
            char *env = prompt_render("coding-agent/prompts/steering/user-interjection.md", ctx);
            json_free(ctx);
            if (json_is_str(content)) json_set_str(w, "content", env ? env : sb_cstr(&text));
            else {
                json_t *c = json_arr(), *img;
                json_push(c, text_block(env ? env : sb_cstr(&text)));
                json_arr_foreach(images, k, img) json_push(c, json_clone(img));
                json_set(w, "content", c);
            }
            free(env);
        }
        json_free(images);
        sb_free(&text);
    }
    return out;
}

/* a stray sloppy payload in plain text becomes a real edit tool call, so the normal pipeline executes it (omp sdk.ts);
 * only the sloppy edit tool recovers */
/* AIError.finalize: errorStatus, errorId, errorMessage, in that order on the wire */
static void hook_finalize_error(void *ud, agent_t *ag, json_t *msg) {
    UNUSED(ud);
    UNUSED(ag);
    json_set_int(msg, "errorId", aie_classify_message(msg));
    json_t *em = json_get(msg, "errorMessage") ? json_clone(json_get(msg, "errorMessage")) : NULL;
    json_del(msg, "errorMessage");
    if (em) json_set(msg, "errorMessage", em);
}

static void hook_transform_assistant(void *ud, agent_t *ag, json_t *assistant) {
    app_t *a = ud;
    if (!settings_bool("edit.recoverInlineEdits", true)) return;
    if (a->env && str_eq(a->env->edit_mode, "sloppy") && agent_find_tool(ag, "edit")) sloppy_recover_inline(assistant);
}

/* transformProviderContext's date/cwd step (coding/dcr.c): one injector per conversation. Only the main loop journals its
 * reminders in the session; another agent on this hook (the auto-learn capture) keeps an unjournaled one. A side request
 * runs through the main loop's injector from its own thread, so the state is serialized. */
/* sdk.ts convertToLlmWithBlockImages + replaceLlmImagesWithText: a model that takes no image input sees one placeholder per
 * run of images. NULL when nothing changes. */
static json_t *omit_images_for_model(const json_t *messages, const ai_model_t *m) {
    static const char PLACEHOLDER[] = "[image omitted: the active model does not support image input]";
    if (!m || m->input_image) return NULL;
    json_t *out = NULL, *msg;
    json_arr_foreach((json_t *)messages, i, msg) {
        const char *role = json_get_str(msg, "role");
        if (!str_eq(role, "user") && !str_eq(role, "developer") && !str_eq(role, "toolResult")) continue;
        const json_t *content = json_get(msg, "content");
        if (!json_is_arr(content)) continue;
        bool any = false;
        json_t *part;
        json_arr_foreach((json_t *)content, k, part) any = any || str_eq(json_get_str(part, "type"), "image");
        if (!any) continue;
        if (!out) out = json_clone(messages);
        json_t *replaced = json_arr();
        json_arr_foreach((json_t *)content, k2, part) {
            if (!str_eq(json_get_str(part, "type"), "image")) {
                json_push(replaced, json_clone(part));
                continue;
            }
            size_t n = json_len(replaced);
            const json_t *prev = n ? json_at(replaced, n - 1) : NULL;
            if (prev && str_eq(json_get_str(prev, "type"), "text") && str_eq(json_get_str(prev, "text"), PLACEHOLDER)) continue;
            json_t *t = json_obj();
            json_set_str(t, "type", "text");
            json_set_str(t, "text", PLACEHOLDER);
            json_push(replaced, t);
        }
        json_set(json_at(out, i), "content", replaced);
    }
    return out;
}

static json_t *hook_transform_inner(void *ud, agent_t *ag, const json_t *messages);

static json_t *hook_transform(void *ud, agent_t *ag, const json_t *messages) {
    json_t *r = hook_transform_inner(ud, ag, messages);
    json_t *o = omit_images_for_model(r ? r : messages, ag->model);
    if (!o) return r;
    json_free(r);
    return o;
}

static json_t *hook_transform_inner(void *ud, agent_t *ag, const json_t *messages) {
    app_t *a = ud;
    json_t *w = wrap_steering(messages);
    if (!settings_bool("dateCwdReminder", true)) return w;
    pthread_mutex_lock(&ag->mu);
    bool has_system = json_len(ag->system_prompt) > 0;
    pthread_mutex_unlock(&ag->mu);
    if (!has_system) return w;
    char date[16];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof date, "%Y-%m-%d", &tm);
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    dcr_t **slot = ag == a->agent ? &a->dcr : &a->dcr_side;
    if (!*slot && ag == a->agent) {
        dcr_journal_t j = dcr_session_journal(&a->session);
        *slot = dcr_new(&j);
    } else if (!*slot) {
        *slot = dcr_new(NULL);
    }
    json_t *r = dcr_transform(*slot, w ? w : messages, date, a->cwd);
    pthread_mutex_unlock(&mu);
    if (!r) return w;
    json_free(w);
    return r;
}

/* isContextOverflow's text arm: a 429/402 status (field, raw-status id, or the text's own "429") vetoes it, since
 * "tokens" wording on a rate-limit status is a throughput cap (Bedrock: "429 Too many tokens, please wait…") */
static bool is_overflow(const json_t *assistant) {
    const char *err = json_get_str(assistant, "errorMessage");
    if (!err) return false;
    int st = (int)json_get_int(assistant, "errorStatus", 0);
    int64_t id = json_get_int(assistant, "errorId", 0);
    if (!st && id && !(id & AIE_CLASS)) st = (int)id;
    if (!st) st = aie_status_from_text(err);
    if (st == 429 || st == 402) return false;
    return (str_istr(err, "prompt is too long") || str_istr(err, "context length") || str_istr(err, "maximum context") ||
                   str_istr(err, "context_length_exceeded") || str_istr(err, "too many tokens") ||
                   str_istr(err, "exceeds the context window") || str_istr(err, "input is too long"));
}

/* AIError.retriable over an error text (RPC prompt_result error.retryable) */
static int hook_stream(void *ud, agent_t *ag, const ai_event_t *ev) {
    app_t *a = ud;
    if (ev->type != AI_EV_DONE && ttsr_on_event(a->ttsr, ev)) {
        a->ttsr_interrupting = true;
        return AGENT_STREAM_INTERRUPT;
    }
    switch (thinkloop_on_event(a->thinkloop, ev)) {
    case THINKLOOP_LOOP:
        free(ag->stream_error);
        ag->stream_error = xstrdup(thinkloop_error(a->thinkloop));
        return AGENT_STREAM_ERROR;
    case THINKLOOP_GEMINI_HEADERS: /* interrupt; should_retry(-1) discards the turn and reminds the model */
        a->gemini_interrupt = thinkloop_header_count(a->thinkloop);
        a->ttsr_interrupting = true;
        return AGENT_STREAM_INTERRUPT;
    default: return AGENT_STREAM_CONTINUE;
    }
}

/* hidden agent-attributed custom message: live context (developer) + persisted custom_message entry */
static void append_custom_notice(app_t *a, agent_t *ag, const char *type, const char *content, json_t *details) {
    json_t *m = msg_developer_text(content);
    json_set_str(m, "attribution", "agent");
    json_set_str(m, "customType", type);
    json_set_bool(m, "display", false);
    agent_append_message(ag, m);
    if (a->session) {
        json_t *e = json_obj();
        json_set_str(e, "type", "custom_message");
        json_set_str(e, "customType", type);
        json_set_str(e, "content", content);
        json_set_bool(e, "display", false);
        if (details) json_set(e, "details", details);
        json_set_str(e, "attribution", "agent");
        session_append_entry(a->session, e);
    } else json_free(details);
}

/* a thinking/response loop retried into identical context loops again: the retry carries the redirect */
static void maybe_thinking_loop_redirect(app_t *a, agent_t *ag, const char *err) {
    if (!str_starts(err ? err : "", "Thinking loop detected") || !settings_bool("model.loopGuard.enabled", true)) return;
    char *t = prompt_load("coding-agent/prompts/system/thinking-loop-redirect.md");
    append_custom_notice(a, ag, "thinking-loop-redirect", t ? t : "", NULL);
    free(t);
}

static bool cmd_mutates(const char *cmd) {
    static const char *const pats[] = {"rm ", "rm\t", "rmdir", "mv ", "cp ", "mkdir", "touch ", "chmod", "chown", "ln ",
                                       "git commit", "git push", "git checkout", "git reset", "git merge", "git rebase",
                                       "git add", "git rm", "git mv", "git stash", "git apply", "git switch", "git restore",
                                       "npm install", "npm i ", "pnpm add", "pnpm install", "yarn add", "pip install",
                                       "cargo add", "go get", "apt ", "brew install", "sed -i", "tee ", "truncate",
                                       "dd ", "make install", "patch "};
    for (size_t i = 0; i < ARRAY_LEN(pats); i++)
        if (strstr(cmd, pats[i])) return true;
    /* output redirection to a file (allow 2>&1 and >/dev/null) */
    for (const char *p = strchr(cmd, '>'); p; p = strchr(p + 1, '>')) {
        const char *t = p + 1;
        if (*t == '>') t++;
        while (*t == ' ') t++;
        if (*t == '&' || str_starts(t, "/dev/null")) continue;
        if (p > cmd && p[-1] == '2' && str_starts(t, "/dev/")) continue;
        return true;
    }
    return false;
}

static tool_gate_t hook_before_tool(void *ud, agent_t *ag, const json_t *call, char **reason) {
    app_t *a = ud;
    UNUSED(ag);
    if (!a->plan_mode) return TOOL_ALLOW;
    const char *name = json_get_str(call, "name");
    json_t *args = json_get(call, "arguments");
    if (str_eq(name, "write")) {
        const char *p = json_get_str(args, "path");
        if (p && (str_starts(p, "local://") || !strcmp(p, "xd://propose"))) return TOOL_ALLOW;
        *reason = xstrdup("Plan mode is active: the working tree is read-only. Write plans to local://<slug>-plan.md and "
                          "propose with xd://propose.");
        return TOOL_BLOCK;
    }
    if (str_eq(name, "edit")) {
        const char *in = json_get_str(args, "input");
        const char *p = json_get_str(args, "path");
        bool ok = p && str_starts(p, "local://");
        if (in && !p) {
            ok = true;
            bool any = false;
            for (const char *s2 = in; (s2 = strchr(s2, '[')); s2++) {
                if (s2 != in && s2[-1] != '\n') continue;
                const char *h = strchr(s2, '#'), *e = strchr(s2, ']');
                if (!h || !e || h > e) continue;
                char *hp = xstrndup(s2 + 1, (size_t)(h - s2 - 1));
                char *abs = tool_resolve_path(a->env, hp);
                if (!a->env->local_dir || !str_starts(abs, a->env->local_dir)) ok = false;
                any = true;
                free(abs);
                free(hp);
            }
            if (!any) ok = false;
        }
        if (ok) return TOOL_ALLOW;
        *reason = xstrdup("Plan mode is active: only the plan file (local://) may be edited.");
        return TOOL_BLOCK;
    }
    if (str_eq(name, "bash")) {
        const char *c = json_get_str(args, "command");
        if (c && cmd_mutates(c)) {
            *reason = xasprintf("Plan mode is active: refusing a state-changing command (`%s`). Explore read-only.", c);
            return TOOL_BLOCK;
        }
    }
    return TOOL_ALLOW;
}

/* local:// root of the current session */
static void app_bind_local(app_t *a) {
    const char *sid = session_id(a->session);
    free(a->env->local_dir);
    char *adir = a->session->persist ? artifacts_dir_for_session(a->session->file) : NULL;
    if (adir) { /* resolveLocalRoot: <artifacts dir>/local; sessions from before this keep <sessions>/<id>/local */
        a->env->local_dir = path_join(adir, "local");
        free(adir);
        char *old = a->session->dir ? xasprintf("%s/%s/local", a->session->dir, sid) : NULL;
        if (old && !fs_exists(a->env->local_dir) && fs_exists(old)) {
            free(a->env->local_dir);
            a->env->local_dir = old;
        } else {
            free(old);
        }
    } else if (a->session->dir) a->env->local_dir = xasprintf("%s/%s/local", a->session->dir, sid);
    else {
        char *base = omc_agent_path("local");
        a->env->local_dir = xasprintf("%s/%s", base, sid);
        free(base);
    }
}

/* point the tool env at the session's artifact directory (session file minus `.jsonl`) */
static void app_bind_artifacts(app_t *a, const char *copy_from_dir) {
    char *dir = artifacts_dir_for_session(a->session ? a->session->file : NULL);
    if (dir && copy_from_dir && strcmp(copy_from_dir, dir) != 0) artifacts_copy_dir(copy_from_dir, dir);
    artifacts_unref(a->env->artifacts);
    a->env->artifacts = artifacts_new(dir);
    free(dir);
}

static void hook_after_tool(void *ud, agent_t *ag, const json_t *call, tool_result_t *res) {
    app_t *a = ud;
    UNUSED(ag);
    tool_postprocess_result(a->env, json_get_str(call, "name"), res);
    char *rem = ttsr_take_tool_reminder(a->ttsr, json_get_str(call, "id"));
    if (rem) {
        tool_result_text(res, rem);
        notice(a, "info", "Rule reminder attached to %s result", json_get_str(call, "name"));
        free(rem);
    }
}

/* ---------- retry model fallback (session/turn-recovery.ts) ---------- */

static bool is_usage_limit(const char *err) {
    static const char *const pats[] = {"usage limit", "usage_limit", "limit_reached", "quota exceeded", "quota reached",
                                       "quota_exceeded", "insufficient_quota", "insufficient quota", "resource exhausted",
                                       "resource_exhausted", "exhausted your capacity", "quota will reset",
                                       "insufficient balance", "balance exhausted", "out of credits", "spending limit",
                                       "spending-limit", "out of usage", "access_terminated_error", "402"};
    for (size_t i = 0; err && i < ARRAY_LEN(pats); i++)
        if (str_istr(err, pats[i])) return true;
    return false;
}

static bool is_abort_error(const char *err) {
    return err && (str_istr(err, "aborted") || str_istr(err, "cancelled by user") || str_istr(err, "interrupted"));
}

/* retry-after hints in the error text ("retry after 12s", "retry-after: 30", "try again in 2m"): ms or -1 */
static int64_t retry_after_ms(const char *err) {
    if (!err) return -1;
    static const char *const keys[] = {"retry-after-ms:", "retry-after-ms=", "retry after ", "retry-after: ", "retry-after:", "try again in ",
                                       "retry in ", "reset after ", "resets in "};
    for (size_t k = 0; k < ARRAY_LEN(keys); k++) {
        const char *p = str_istr(err, keys[k]);
        if (!p) continue;
        p += strlen(keys[k]);
        char *end;
        double v = strtod(p, &end);
        if (end == p || v < 0) continue;
        while (*end == ' ') end++;
        double ms = v * 1000;
        if (k <= 1 || !strncasecmp(end, "ms", 2)) ms = v;
        else if (!strncasecmp(end, "m", 1) && strncasecmp(end, "ms", 2)) ms = v * 60000;
        else if (!strncasecmp(end, "h", 1)) ms = v * 3600000;
        return (int64_t)ms;
    }
    return -1;
}

char *app_fallback_notice_text(const char *fields) {
    strvec_t f = {0};
    for (const char *s = fields; s;) {
        const char *sep = strchr(s, '\x1f');
        char *t = sep ? xstrndup(s, (size_t)(sep - s)) : xstrdup(s);
        strvec_push(&f, t);
        free(t);
        s = sep ? sep + 1 : NULL;
    }
    char *r = f.n >= 4 && *f.v[3] ? xasprintf("Fallback: %s -> %s\n%s", f.v[0], f.v[1], f.v[3])
                                   : xasprintf("Fallback: %s -> %s", f.n > 0 ? f.v[0] : "", f.n > 1 ? f.v[1] : "");
    strvec_free(&f);
    return r;
}

/* formatRetryFallbackSelector: provider/id plus the configured thinking level */
static char *current_selector(app_t *a) {
    char *k = mr_model_key(a->model);
    char *s = mr_selector_with_level(k, a->model->reasoning ? effort_name(a->thinking) : NULL);
    free(k);
    return s;
}

/* the last assistant turn before the failed one carries Anthropic signed thinking from another model */
static bool signed_thinking_blocks(agent_t *ag, const json_t *failed, const ai_model_t *cand) {
    json_t *msgs = ag->messages, *last = NULL;
    for (size_t i = json_len(msgs); i-- > 0;) {
        json_t *m = json_at(msgs, i);
        if (m == failed || !str_eq(json_get_str(m, "role"), "assistant")) continue;
        last = m;
        break;
    }
    if (!last || !str_eq(json_get_str(last, "api"), "anthropic-messages") || !str_eq(cand->api, "anthropic-messages"))
        return false;
    if (!str_eq(json_get_str(last, "provider"), cand->provider) || str_eq(json_get_str(last, "model"), cand->id))
        return false;
    json_t *b;
    json_arr_foreach(json_get_arr(last, "content"), i, b) {
        const char *t = json_get_str(b, "type");
        const char *sig = json_get_str(b, "thinkingSignature");
        if ((str_eq(t, "thinking") && sig && *sig) || str_eq(t, "redactedThinking")) return true;
    }
    return false;
}

static void apply_fallback(app_t *a, const char *role, const ai_model_t *cand, const char *level, const char *cur,
                           const char *reason) {
    ai_effort_t prev = a->thinking, next = a->thinking;
    if (level && effort_parse(level, &next) != 0) next = a->thinking;
    char *from = xstrdup(cur);
    const ai_model_t *prev_model = a->model;
    const char *prev_wire = app_wire_thinking(a);
    a->model = cand;
    agent_set_model(a->agent, cand);
    app_model_state_changed(a, prev_model, prev_wire);
    char *key = mr_model_key(cand);
    session_append_model_change_ex(a->session, key, "fallback", true);
    mr_usage_record(key);
    if (next != a->thinking) app_set_thinking(a, next);
    if (!a->fallback.active) {
        a->fallback.active = true;
        free(a->fallback.role);
        free(a->fallback.original);
        a->fallback.role = xstrdup(role);
        a->fallback.original = xstrdup(from);
        a->fallback.original_thinking = prev;
        a->fallback.pinned = false;
    }
    a->fallback.served = false;
    a->fallback.last_thinking = next;
    app_rebuild_system_prompt(a);
    /* retry_fallback_applied {from, to, role, reason}: fields separated by \x1f */
    notice(a, "retry_fallback_applied", "%s\x1f%s\x1f%s\x1f%s", from, key, role, reason);
    free(key);
    free(from);
}

/* #tryRetryModelFallback: walk the pinned chain, then the current model's own chain */
static bool try_retry_fallback(app_t *a, agent_t *ag, const char *cur, const json_t *failed, const char *err,
                               bool same_model_retry) {
    strvec_t keys = {0};
    char *own = mr_fallback_chain_key(cur, a->model, NULL);
    if (a->fallback.active && a->fallback.role) strvec_push(&keys, a->fallback.role);
    if (own && !strvec_contains(&keys, own)) strvec_push(&keys, own);
    free(own);
    mr_pool_t avail = {0};
    mr_pool_available(&avail, "all");
    bool done = false;
    for (size_t k = 0; k < keys.n && !done; k++) {
        mr_selector_t *c = NULL;
        size_t n = mr_fallback_candidates(keys.v[k], cur, a->model, false, &c);
        for (size_t i = 0; i < n; i++) {
            if (done || mr_selector_suppressed(c[i].raw)) continue;
            mr_result_t r;
            const ai_model_t *cand = mr_resolve_value(c[i].raw, &avail, &r) ? r.model : NULL;
            mr_result_clear(&r);
            if (!cand) cand = catalog_find(c[i].provider, c[i].id);
            if (!cand || (cand == a->model && (!c[i].level || !strcmp(c[i].level, effort_name(a->thinking))))) continue;
            if (same_model_retry && signed_thinking_blocks(ag, failed, cand)) continue;
            if (a->last_context_tokens > 0 && cand->context_window > 0 && a->last_context_tokens > cand->context_window)
                continue;
            if (!mr_provider_available(cand->provider)) continue;
            char *reason = xasprintf("Request failed: %s", err ? err : "provider returned an error without details");
            apply_fallback(a, keys.v[k], cand, c[i].level, cur, reason);
            free(reason);
            done = true;
        }
        for (size_t i = 0; i < n; i++) mr_selector_free(&c[i]);
        free(c);
    }
    mr_pool_free(&avail);
    strvec_free(&keys);
    return done;
}

/* getApiKey at prompt time: keyless providers need none */
static bool model_has_usable_credential(const ai_model_t *m) {
    return catalog_provider_keyless(m->provider) || auth_has_usable_credential(m->provider);
}

/* applyMissingCredentialFallback + #findKeyedRetryFallback: move off a model whose provider has no usable credential
 * onto the first chain candidate with one that fits the live context. Unpinned: once the primary's credential returns
 * maybe_restore_primary switches back at the next prompt. */
static bool missing_credential_fallback(app_t *a) {
    if (!settings_bool("retry.modelFallback", true)) return false;
    char *cur = current_selector(a);
    strvec_t keys = {0};
    char *own = mr_fallback_chain_key(cur, a->model, NULL);
    if (a->fallback.active && a->fallback.role) strvec_push(&keys, a->fallback.role);
    if (own && !strvec_contains(&keys, own)) strvec_push(&keys, own);
    free(own);
    mr_pool_t avail = {0};
    mr_pool_available(&avail, "all");
    bool done = false;
    for (size_t k = 0; k < keys.n && !done; k++) {
        mr_selector_t *c = NULL;
        size_t n = mr_fallback_candidates(keys.v[k], cur, a->model, false, &c);
        for (size_t i = 0; i < n; i++) {
            if (done || mr_selector_suppressed(c[i].raw)) continue;
            mr_result_t r;
            const ai_model_t *cand = mr_resolve_value(c[i].raw, &avail, &r) ? r.model : NULL;
            mr_result_clear(&r);
            if (!cand) cand = catalog_find(c[i].provider, c[i].id);
            if (!cand || cand == a->model) continue;
            if (a->last_context_tokens > 0 && cand->context_window > 0 && a->last_context_tokens > cand->context_window)
                continue;
            if (!mr_provider_available(cand->provider) || !model_has_usable_credential(cand)) continue;
            char *reason = xasprintf("no credential for %s", a->model->provider);
            apply_fallback(a, keys.v[k], cand, c[i].level, cur, reason);
            free(reason);
            done = true;
        }
        for (size_t i = 0; i < n; i++) mr_selector_free(&c[i]);
        free(c);
    }
    mr_pool_free(&avail);
    strvec_free(&keys);
    free(cur);
    return done;
}

/* #promptWithMessage: "Validate API key"; a provider without one walks the retry fallback chain first. False refuses
 * the prompt (the error is reported). */
static bool credential_preflight(app_t *a) {
    if (!a->model || model_has_usable_credential(a->model) || missing_credential_fallback(a)) return true;
    char *agent_dir = omc_agent_path("");
    free(a->prompt_refusal);
    a->prompt_refusal = xasprintf("No API key found for %s.\n\nUse /login, set an API key environment variable, or create %s/models.yml",
                                  a->model->provider, agent_dir);
    free(agent_dir);
    notice(a, "error", "%s", a->prompt_refusal);
    return false;
}

/* #maybeRestoreRetryFallbackPrimary: back to the primary once its cooldown ends (retry.fallbackRevertPolicy) */
static void maybe_restore_primary(app_t *a) {
    if (!a->fallback.active || a->fallback.pinned || !a->model) return;
    if (str_eq(settings_str("retry.fallbackRevertPolicy", "cooldown-expiry"), "never")) return;
    mr_selector_t orig;
    if (!mr_parse_selector(a->fallback.original, &orig)) {
        a->fallback.active = false;
        return;
    }
    char *cur = current_selector(a);
    bool same = !strcmp(cur, orig.raw);
    free(cur);
    if (mr_selector_suppressed(orig.raw)) {
        mr_selector_free(&orig);
        return;
    }
    if (same) {
        a->fallback.active = false;
        mr_selector_free(&orig);
        return;
    }
    const ai_model_t *pm = catalog_find(orig.provider, orig.id);
    if (pm && mr_provider_available(pm->provider) && model_has_usable_credential(pm)) {
        ai_effort_t th = a->thinking == a->fallback.last_thinking ? a->fallback.original_thinking : a->thinking;
        a->fallback.active = false;
        const ai_model_t *prev_model = a->model;
        const char *prev_wire = app_wire_thinking(a);
        a->model = pm;
        agent_set_model(a->agent, pm);
        app_model_state_changed(a, prev_model, prev_wire);
        char *key = mr_model_key(pm);
        session_append_model_change_ex(a->session, key, "fallback", false);
        mr_usage_record(key);
        free(key);
        if (th != a->thinking) app_set_thinking(a, th);
        app_rebuild_system_prompt(a);
    }
    mr_selector_free(&orig);
}

/* maintainContextMidRun between tool-loop turns */
static void hook_turn_boundary(void *ud, agent_t *ag, const json_t *assistant) {
    app_t *a = ud;
    cmp_mid_turn(ud, assistant);
    if (ag->mcp_tools_dirty && a->mcp) { /* the call that loaded tools returned: they reach this session's next request */
        ag->mcp_tools_dirty = 0;
        mcp_sync_tools(a->mcp, ag);
        tools_finalize(ag, a->opts.tools);
        app_rebuild_system_prompt(a);
        xdnotice_sync(a, NULL);
    }
    /* #steerAnthropicWrapUp: once per wrap-up window, tell the model to checkpoint when nothing (low priority, extra
     * usage) carries the work past the allowance */
    if (app_anthropic_wrap_up_key(a) >= 0) {
        char *txt = prompt_render("coding-agent/prompts/system/anthropic-usage-wrap-up.md", NULL);
        json_t *m = msg_developer_text(txt ? txt : "");
        json_set_str(m, "attribution", "agent");
        json_set_str(m, "customType", "anthropic-usage-wrap-up");
        json_set_bool(m, "display", false);
        agent_steer(ag, m);
        free(txt);
    }
}

static int hook_should_retry(void *ud, agent_t *ag, const json_t *assistant, int attempt, json_t **append) {
    app_t *a = ud;
    if (attempt < 0 && a->gemini_interrupt > 0) { /* LoopGuards #interruptGeminiHeaderRunaway */
        int n = a->gemini_interrupt;
        a->gemini_interrupt = 0;
        a->ttsr_interrupting = false;
        notice(a, "warning", "Interrupted %d planning headers with no tool call; reminded the model to issue one.", n);
        json_t *ctx = json_obj();
        json_set_int(ctx, "count", n);
        char *txt = prompt_render("coding-agent/prompts/system/gemini-tool-call-reminder.md", ctx);
        json_free(ctx);
        json_t *det = json_obj();
        json_set_int(det, "headers", n);
        append_custom_notice(a, ag, "gemini-tool-call-reminder", txt ? txt : "", det);
        free(txt);
        return true;
    }
    if (attempt < 0) {
        json_t *names = NULL;
        char *inj = ttsr_take_interrupt(a->ttsr, &names);
        a->ttsr_interrupting = false;
        if (!inj) return false;
        sb_t nl = SB_INIT;
        json_t *x;
        json_arr_foreach(names, i, x) sb_appendf(&nl, "%s%s", i ? ", " : "", json_sv(x));
        notice(a, "warning", "Injecting rule: %s", sb_cstr(&nl));
        sb_free(&nl);
        json_t *m = json_obj();
        json_set_str(m, "role", "custom");
        json_set_str(m, "customType", "ttsr-injection");
        json_set_str(m, "content", inj);
        json_set_bool(m, "display", false);
        json_t *det = json_obj();
        json_set(det, "rules", names);
        json_set(m, "details", det);
        json_set_int(m, "timestamp", now_ms());
        /* the agent sends custom messages as user content (see session to_llm) */
        json_t *llm = msg_user_text(inj);
        json_set_bool(llm, "synthetic", true);
        agent_append_message(ag, llm);
        if (a->session) {
            json_t *e = json_obj();
            json_set_str(e, "type", "custom_message");
            json_set_str(e, "customType", "ttsr-injection");
            json_set_str(e, "content", inj);
            json_set_bool(e, "display", false);
            json_set(e, "details", json_clone(det));
            session_append_entry(a->session, e);
        }
        json_free(m);
        free(inj);
        return true;
    }
    const char *stop = json_get_str(assistant, "stopReason");
    if (str_eq(stop, "aborted") || ag->cancel) return false;
    const char *err = json_get_str(assistant, "errorMessage");
    bool flagged = false; /* providers mark opaque overflow errors (Devin large-history invalid_argument) */
    json_t *fl;
    json_arr_foreach(json_get(assistant, "errorFlags"), fi, fl) flagged = flagged || str_eq(json_sv(fl), "contextOverflow");
    if ((flagged || is_overflow(assistant)) && !a->compacting) {
        /* overflow recovery: the method ladder with a retry of the failed turn (session-maintenance.ts) */
        int64_t billed = 0;
        const json_t *u = json_get_obj(assistant, "usage");
        if (u) billed = json_get_int(u, "input", 0) + json_get_int(u, "cacheRead", 0) + json_get_int(u, "cacheWrite", 0);
        return (cmp_auto(a, CMP_OVERFLOW, true, billed) & CMP_COMMITTED) != 0;
    }
    /* turn-recovery.ts (agent_end): a tool turn interrupted by a mid-stream transport failure resumes with its
     * synthetic results; otherwise a retryable error replays the turn unless it already showed text or ran
     * tools; a transport failure after committed text keeps the partial turn and continues with a reminder */
    bool committed = !a->text_buffered;
    bool resolved_stall = tr_resolved_interrupted_tool_turn(assistant, ag->cancel);
    bool retryable = resolved_stall || tr_is_retryable_error(assistant, committed);
    if (!retryable) {
        bool reset = false;
        bool stall = settings_bool("retry.enabled", true) && tr_committed_text_stall(assistant, committed, ag->cancel, &reset);
        if (reset) a->stall_continues = 0;
        if (stall) {
            if (++a->stall_continues <= TR_STREAM_STALL_CONTINUE_MAX) {
                LOGI("Stream failed after committed text; continuing with resume reminder (attempt %d): %s",
                     a->stall_continues, err ? err : "");
                *append = tr_stream_stall_reminder(a->stall_continues);
                return AGENT_RETRY_CONTINUE;
            }
            LOGW("Stream kept stalling after committed text past retry cap (%d attempts)", a->stall_continues - 1);
            a->stall_continues = 0;
        }
    }
    /* resolved stream-stall tools and proven-unexecuted calls keep their assistant/result pair */
    uint32_t eid = aie_classify_message(assistant);
    bool preserve = resolved_stall || ((aie_retriable(eid) || (eid & AIE_MALFORMED_FUNCTION_CALL)) &&
                                       tr_unexecuted_tool_calls_replay_safe(assistant, committed, 0));
    int again = preserve ? AGENT_RETRY_PRESERVE : AGENT_RETRY_REPLAY;
    if (!settings_bool("retry.enabled", true)) return false;
    if (attempt == 0) a->retry_offset = 0;
    int max = (int)settings_int("retry.maxRetries", 10);
    int saga = attempt - a->retry_offset + 1; /* omp #retryAttempt after its increment */
    bool exhausted = saga > max;
    bool usage_limit = is_usage_limit(err);
    /* an account out of money has no reset to wait for: fallbacks still run, then the provider's error surfaces and
     * the error frame's Retry re-attempts at once */
    int status = (int)json_get_int(assistant, "errorStatus", 0);
    bool billing_cap = tr_billing_cap_usage_limit(status ? status : aie_status_from_text(err), err);
    /* model fallback: consulted on every failed attempt, retryable or hard (hardErrorFallback) */
    bool switched = false;
    if (a->model && !is_abort_error(err) && settings_bool("retry.modelFallback", true) && mr_fallback_admits(usage_limit) &&
        !(is_overflow(assistant))) {
        char *cur = current_selector(a);
        int64_t hint = retry_after_ms(err);
        mr_suppress_selector(cur, now_ms() + (hint > 0 ? hint : mr_rate_limit_backoff_ms(err)));
        bool rate_limited = str_istr(err, "429") || str_eq(mr_rate_limit_reason(err), "RATE_LIMIT_EXCEEDED");
        bool same_model_retry = retryable && !exhausted && !billing_cap && !(settings_bool("retry.rateLimitWalksChain", false) && rate_limited);
        switched = try_retry_fallback(a, ag, cur, assistant, err, same_model_retry);
        free(cur);
    }
    if (switched) {
        maybe_thinking_loop_redirect(a, ag, err);
        a->retry_offset = attempt + 1; /* a fallback model gets a fresh retry budget */
        strvec_push(&a->retry_notes, usage_limit ? "rate-limited; switched model; retried" : "switched model; retried");
        return again;
    }
    if (!retryable) return false;
    if (exhausted) {
        /* closeRetrySaga failure: the final turn reads "Retry budget exhausted …", superseded attempts hide,
         * then "Retry failed after N attempts: <error>" (fields: attempts \x1f final turn error \x1f error) */
        a->retry_saga = 0;
        notice(a, "retry-failed", "%d\x1fRetry budget exhausted after %d %s: %s\x1f%s", saga - 1, saga - 1,
               saga - 1 == 1 ? "retry" : "retries", err ? err : "error", err ? err : "Unknown error");
        strvec_free(&a->retry_notes);
        return false;
    }
    double delay = mr_retry_backoff_ms(settings_int("retry.baseDelayMs", 500), saga);
    int64_t hint = retry_after_ms(err);
    int64_t window = mr_reason_window_ms(err, mr_error_status(err), usage_limit, hint >= 0);
    if ((double)window > delay) delay = (double)window;
    if ((double)hint > delay) delay = (double)hint;
    int64_t maxd = settings_int("retry.maxDelayMs", 5 * 60 * 1000);
    if (billing_cap || (maxd > 0 && delay > (double)maxd)) {
        char *fe = billing_cap ? xstrdup(err)
                               : xasprintf("Provider requested %.0fms wait, exceeds retry.maxDelayMs (%lldms). Original error: %s",
                                           ceil(delay), (long long)maxd, err ? err : "error");
        json_t *ev = json_obj();
        json_set_str(ev, "type", "auto_retry_end");
        json_set_bool(ev, "success", false);
        json_set_int(ev, "attempt", saga);
        json_set_str(ev, "finalError", fe);
        a->retry_saga = 0;
        if (!app_emit(a, ev)) notice(a, "error", "%s", fe);
        free(fe);
        return false;
    }
    /* captured before event delivery, so delivery counts toward the announced wait */
    int64_t usage_reset_deadline = now_ms() + (int64_t)ceil(delay);
    {
        json_t *ev = json_obj();
        json_set_str(ev, "type", "auto_retry_start");
        json_set_int(ev, "attempt", saga);
        json_set_int(ev, "maxAttempts", max);
        json_set_num(ev, "delayMs", delay);
        json_set_str(ev, "errorMessage", err ? err : "error");
        if (json_get(assistant, "errorId")) json_set(ev, "errorId", json_clone(json_get(assistant, "errorId")));
        a->retry_saga = saga;
        if (!app_emit(a, ev))
            notice(a, "warning", "Retrying (%d/%d) in %llds: %s", saga, max, (long long)ceil(delay / 1000),
                   err ? err : "error");
    }
    maybe_thinking_loop_redirect(a, ag, err);
    strvec_push(&a->retry_notes, usage_limit ? "rate-limited; waited; retried" : "error; retried");
    /* ordinary backoff keeps its monotonic duration; a quota reset elapses while the machine sleeps too */
    int64_t until = mono_ms() + (int64_t)ceil(delay);
    while (usage_limit ? now_ms() < usage_reset_deadline : mono_ms() < until) {
        if (ag->cancel) return false;
        sleep_ms(50);
    }
    return again;
}

/* ---------- checkpoint / rewind ---------- */

static int checkpoint_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    app_t *a = self->ud;
    UNUSED(ctx);
    if (a->checkpoint_index > 0 || a->checkpoint_pending) {
        tool_result_error(out, "Checkpoint already active.");
        return 0;
    }
    a->checkpoint_pending = true;
    char *ts = iso8601_now();
    const char *goal = json_get_str(args, "goal");
    tool_result_textf(out, "Checkpoint started%s%s. Explore, then call rewind with a concise report before yielding.",
                      goal ? ": " : "", goal ? goal : "");
    out->details = json_obj();
    json_set_str(out->details, "startedAt", ts);
    if (goal) json_set_str(out->details, "goal", goal);
    free(ts);
    return 0;
}

static int rewind_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    app_t *a = self->ud;
    UNUSED(ctx);
    const char *report = json_get_str(args, "report");
    if (a->checkpoint_index <= 0) {
        tool_result_error(out, "No active checkpoint.");
        return 0;
    }
    if (!report || !*report) {
        tool_result_error(out, "report is required");
        return 0;
    }
    free(a->pending_rewind);
    a->pending_rewind = xstrdup(report);
    tool_result_text(out, "Rewinding to checkpoint; exploration will be replaced by the report.");
    return 0;
}

static void add_checkpoint_tools(app_t *a, agent_t *ag) {
    char *d1 = tool_prompt("checkpoint", NULL), *d2 = tool_prompt("rewind", NULL);
    json_t *s1 = schema_lit("{\"type\":\"object\",\"properties\":{\"goal\":{\"type\":\"string\",\"description\":\"investigati"
        "on goal\"}},\"required\":[\"goal\"],\"additionalProperties\":false}");
    agent_tool_t *t1 = agent_tool_new("checkpoint", "Checkpoint", d1, s1, checkpoint_execute);
    t1->ud = a;
    t1->exclusive = true;
    t1->intent_mode = INTENT_OMIT; /* checkpoint.ts derives its intent from the goal */
    json_t *s2 = schema_lit("{\"type\":\"object\",\"properties\":{\"report\":{\"type\":\"string\",\"description\":\"investiga"
        "tion findings\"}},\"required\":[\"report\"],\"additionalProperties\":false}");
    agent_tool_t *t2 = agent_tool_new("rewind", "Rewind", d2, s2, rewind_execute);
    t2->ud = a;
    t2->exclusive = true;
    t2->intent_mode = INTENT_OMIT;
    agent_add_tool(ag, t1);
    agent_add_tool(ag, t2);
    free(d1);
    free(d2);
}

/* ---------- setup ---------- */

static json_t *app_tool_judge(tool_env_t *env, const json_t *state, const json_t *questions,
                              volatile sig_atomic_t *cancel, double usage[3], char **err) {
    return evalhost_judge(env->session, state, questions, cancel, usage, err);
}

static json_t *app_eval_bridge(tool_env_t *env, const char *name, const json_t *args, int64_t cell,
                               volatile sig_atomic_t *cancel, char **err) {
    app_t *a = env->session;
    if (!a || !a->evalhost) {
        *err = xstrdup("eval helpers are unavailable in this session");
        return NULL;
    }
    return evalhost_call(a->evalhost, name, args, cell, cancel, err);
}

static char *app_after_write(tool_env_t *env, const char *abs) {
    app_t *a = env->session;
    if (!a || !a->lsp || !settings_bool("lsp.diagnosticsOnWrite", true)) return NULL;
    return lsp_after_write(a->lsp, abs, settings_int("lsp.diagnosticsWaitMs", 3000));
}

/* edits report diagnostics only with lsp.diagnosticsOnEdit (omp default: off) */
static char *app_after_edit(tool_env_t *env, const char *abs) {
    app_t *a = env->session;
    if (!a || !a->lsp || !settings_bool("lsp.diagnosticsOnEdit", false)) return NULL;
    return lsp_after_write(a->lsp, abs, settings_int("lsp.diagnosticsWaitMs", 3000));
}

void app_sync_queue_modes(app_t *a) {
    if (!a || !a->agent) return;
    a->agent->steer_all = str_eq(settings_str("steeringMode", "one-at-a-time"), "all");
    a->agent->follow_up_all = str_eq(settings_str("followUpMode", "one-at-a-time"), "all");
    a->agent->interrupt_wait = str_eq(settings_str("interruptMode", "immediate"), "wait");
}

/* the extension wrapper's approval gate (extensibility/extensions/wrapper.ts execute): resolve against the
 * validated input, deny, or prompt through the UI (requestApproval) and remember what the answer covers */
static tool_gate_t hook_approve_tool(void *ud, agent_t *ag, const json_t *call, const json_t *args, char **reason) {
    app_t *a = ud;
    const char *name = json_get_str(call, "name");
    if (!name) return TOOL_ALLOW;
    bool client_approved = false;
    if (a->permission_gate && a->permission_gate(a->permission_gate_ud, a, call, args, &client_approved, reason) == TOOL_BLOCK)
        return TOOL_BLOCK;
    const char *sid = a->session ? session_id(a->session) : NULL;
    approval_ctx_t ctx;
    approval_ctx_from_settings(&ctx, a->opts.auto_approve, sid, a->cwd);
    approval_call_t c = {a->env, ag, name, args};
    approval_resolved_t r;
    approval_resolve(&c, &ctx, &r);
    tool_gate_t gate = TOOL_ALLOW;
    char *scope_subject = NULL, *scope_key = NULL;
    strvec_t targets = {0};
    if (r.policy == APOL_DENY) {
        *reason = approval_deny_error(&r, name);
        gate = TOOL_BLOCK;
        goto done;
    }
    if (r.policy != APOL_PROMPT || client_approved) goto done;
    /* only a prompt always-ask-cached itself raised can be remembered by target; otherwise a prompt the mode
     * raised can be approved for the rest of the session */
    bool grantable = ctx.use_grants && r.source == ASRC_MODE;
    if (grantable) approval_cache_targets(&c, &targets);
    bool cache_targets = grantable && targets.n > 0;
    bool scoped = !cache_targets && approval_session_scope(&r, name, &scope_subject, &scope_key);
    if (scoped && approval_session_has(sid, scope_key)) goto done;
    if (!a->env->ui.approve) {
        if (r.source == ASRC_RESTRICTION)
            *reason = xasprintf("Tool \"%s\" requires approval but no interactive UI available.\nReason: %s\nA restricted "
                                "folder (tools.restrictedFolders) asks in every approval mode.",
                                name, r.reason ? r.reason : "");
        else
            *reason = xasprintf("Tool \"%s\" requires approval but no interactive UI available.\nOptions:\n  1. Set "
                                "tools.approvalMode: yolo in /settings\n  2. Add tools.approval.%s: allow to config\n  3. "
                                "Use an interactive UI to approve the tool call",
                                name, name);
        gate = TOOL_BLOCK;
        goto done;
    }
    approval_prompt_t p;
    approval_prompt_build(&c, r.reason, scoped ? scope_subject : NULL, cache_targets, &p);
    char *status = approval_format_prompt(&c, r.reason);
    int grant = 0;
    bool always = false;
    agents_user_wait(a->env, true);
    int answer = a->env->ui.approve(a->env->ui.ud, json_get_str(call, "id"), name, status, &p, &grant, &always, &ag->cancel);
    agents_user_wait(a->env, false);
    free(status);
    if (answer == APPROVE_SESSION && scoped) approval_session_grant(sid, scope_key);
    else if (answer == APPROVE_GRANT && grant >= 0 && (size_t)grant < p.ngrants)
        approval_record_grant(&c, sid, r.tier, p.grants[grant].folder, always);
    else if (answer != APPROVE_ONCE) {
        *reason = xasprintf("Tool call denied by user: %s", name);
        gate = TOOL_BLOCK;
    }
    approval_prompt_free(&p);
done:
    strvec_free(&targets);
    free(scope_subject);
    free(scope_key);
    approval_resolved_free(&r);
    return gate;
}

static void install_agent(app_t *a) {
    agent_t *ag = agent_new();
    a->agent = ag;
    ag->app = a;
    app_sync_queue_modes(a);
    ag->max_turns = a->opts.max_turns;
    ag->hooks.prepare_request = hook_prepare;
    ag->hooks.on_request = hook_on_request;
    ag->hooks.should_retry = hook_should_retry;
    ag->hooks.transform_context = hook_transform;
    ag->hooks.finalize_error = hook_finalize_error;
    ag->hooks.transform_assistant = hook_transform_assistant;
    ag->hooks.on_stream_delta = hook_stream;
    ag->hooks.after_tool = hook_after_tool;
    ag->hooks.before_tool = hook_before_tool;
    ag->hooks.approve_tool = hook_approve_tool;
    ag->hooks.turn_boundary = hook_turn_boundary;
    ag->hooks.queue_changed = hook_queue_changed;
    ag->hooks.ud = a;
    free(ag->session_id);
    ag->session_id = a->session ? xstrdup(session_id(a->session)) : uuid_v7();
    agent_subscribe(ag, on_agent_event, a);
    agent_set_model(ag, a->model);
    ag->thinking = a->thinking;
    a->env->can_prompt_user = a->opts.has_ui;
    a->env->ask_ui = a->opts.rpc_ui || a->opts.interactive_prompts;
    a->env->delegation_bias = model_delegation_bias(a->model);
    agent_set_intent_tracing(settings_bool("tools.intentTracing", true));
    if (!a->opts.no_tools) tools_register_builtin(ag, a->env, a->opts.tools);
    if (a->mcp && a->opts.subagent) { /* subagents share the parent's connections, each with its own view */
        free(ag->mcp_scope);
        ag->mcp_scope = xstrdup(ag->session_id);
        mcp_open_tool_scope(a->mcp, ag->mcp_scope, a->opts.mcp_seed_scope);
    }
    if (a->mcp && !a->opts.no_tools) mcp_register_tools(a->mcp, ag);
    /* subagents get lsp only with task.enableLsp (task/index.ts enableLsp, off by default) */
    bool lsp_ok = !a->opts.no_tools && settings_bool("lsp.enabled", true) &&
                  (!a->opts.subagent || settings_bool("task.enableLsp", false)) &&
                  (!a->opts.tools || strvec_contains(a->opts.tools, "lsp"));
    if (lsp_ok) {
        if (!a->lsp && a->opts.lsp) a->lsp = a->opts.lsp;
        if (!a->lsp) {
            a->lsp = lsp_manager_new(a->cwd);
            a->owns_lsp = true;
        }
        agent_add_tool(ag, tool_lsp_new(a->env, a->lsp));
        a->env->after_write = app_after_write;
        a->env->after_edit = app_after_edit;
    }
    /* checkpoint/rewind: off by default (checkpoint.enabled); top-level sessions or explicit lists; paired */
    if (!a->opts.no_tools && settings_bool("checkpoint.enabled", false) &&
        (a->opts.tools ? strvec_contains(a->opts.tools, "checkpoint") || strvec_contains(a->opts.tools, "rewind")
                       : !a->opts.subagent))
        add_checkpoint_tools(a, ag);
    /* github: discoverable; only with the gh CLI on PATH and github.enabled (tools/index.ts) */
    if (!a->opts.no_tools && settings_bool("github.enabled", false) && (!a->opts.tools || strvec_contains(a->opts.tools, "github"))) {
        agent_tool_t *gt = tool_github_new(a->env);
        if (gt) agent_add_tool(ag, gt);
    }
    if (!a->opts.no_tools && settings_bool("debug.enabled", true) && (!a->opts.tools || strvec_contains(a->opts.tools, "debug"))) {
        if (!a->dap) a->dap = dap_manager_new(a->cwd);
        agent_add_tool(ag, tool_debug_new(a->env, a->dap));
    }
    if (a->opts.subagent && a->opts.subagent_depth >= (int)settings_int("task.maxRecursionDepth", 2))
        agent_remove_tool(ag, "task");
    /* todos are parent-owned bookkeeping, stripped from subagents (task/executor.ts isParentOwnedTool) */
    if (a->opts.subagent) agent_remove_tool(ag, "todo");
    if (!a->opts.no_tools) autolearn_register_tools(a, ag);
    if (!a->opts.no_tools && settings_bool("externalThinking", false) && think_supported(a->model))
        agent_add_tool(ag, tool_think_new());
    /* settings-gated custom tools: generate_image (honors an explicit tool list) */
    if (!a->opts.no_tools && !a->opts.subagent && settings_bool("generate_image.enabled", false) &&
        (!a->opts.tools || strvec_contains(a->opts.tools, "generate_image")))
        agent_add_tool(ag, imagegen_tool_new(a));
    /* discoverable (xd://-mountable) tools added outside tools_register_builtin; MCP tools too */
    for (size_t i = 0; i < ag->all_tools.n; i++) {
        agent_tool_t *t = ag->all_tools.v[i];
        if (str_eq(t->name, "lsp") || str_eq(t->name, "debug") || str_eq(t->name, "github") || str_eq(t->name, "checkpoint") ||
            str_eq(t->name, "rewind") || str_starts(t->name, "mcp__"))
            t->discoverable = true;
    }
    tools_finalize(ag, a->opts.tools);
}

void app_rebuild_system_prompt(app_t *a) {
    think_reconcile(a); /* model / externalThinking changes rebuild the prompt */
    strvec_t names = {0};
    json_t *labels = json_obj();
    json_t *refs = json_obj();
    /* edit's wire name in apply_patch mode (edit/index.ts customWireName) is how prompts reference it */
    if (str_eq(a->env->edit_mode, "apply_patch")) json_set_str(refs, "edit", "apply_patch");
    for (size_t i = 0; i < a->agent->tools.n; i++) {
        strvec_push(&names, a->agent->tools.v[i]->name);
        if (a->agent->tools.v[i]->label) json_set_str(labels, a->agent->tools.v[i]->name, a->agent->tools.v[i]->label);
    }
    char *label = app_model_label(a);
    json_t *xdev_tools = xdev_entries(a->agent);
    /* sdk.ts appendParts: memory backend instructions, then auto-learn guidance (driven by the tools present) */
    sb_t gen = SB_INIT;
    char *mem = memory_instructions(a);
    char *al = autolearn_instructions(a->agent);
    if (mem) sb_append(&gen, mem);
    if (al) sb_appendf(&gen, "%s%s", gen.len ? "\n\n" : "", al);
    free(mem);
    free(al);
    /* mounted MCP route guidance (a routed tool carries its catalog summary on the route line, no catalog line),
     * then the connected servers' instructions */
    strvec_t routed = {0};
    char *routes = xdev_mcp_route_guidance(a->agent, &routed);
    if (routes) sb_appendf(&gen, "%s%s", gen.len ? "\n\n" : "", routes);
    free(routes);
    char *mi = a->mcp ? mcp_instructions(a->mcp) : NULL;
    if (mi && *mi) sb_appendf(&gen, "%s%s", gen.len ? "\n\n" : "", mi);
    free(mi);
    char *xdev_docs = json_len(xdev_tools) ? xdev_prompt_docs_ex(a->agent, &routed) : xstrdup("");
    strvec_free(&routed);
    strvec_t roots = {0};
    if (a->session) roots = session_additional_dirs(a->session);
    sysprompt_opts_t so = {.cwd = a->cwd,
                           .additional_roots = &roots,
                           .model_name = label,
                           .tool_names = &names,
                           .disc = &a->disc,
                           .custom_prompt = a->sys_custom,
                           .custom_template = a->opts.subagent ? NULL : a->opts.system_prompt_template,
                           .append_prompt = a->sys_append,
                           .subagent = a->opts.subagent,
                           .skill_desc = skilldesc_render,
                           .skill_desc_ud = a->skilldesc,
                           .tool_labels = labels,
                           .xdev_tools = xdev_tools,
                           .xdev_docs = xdev_docs,
                           .auto_qa = !a->opts.tools && xdev_autoqa_enabled(),
                           .tool_refs = refs,
                           .delegation_bias = model_delegation_bias(a->model),
                           .irc_enabled = !a->opts.tools && agent_find_tool(a->agent, "task") != NULL,
                           .generated_append = sb_cstr(&gen),
                           .tui_transcript = a->opts.has_ui && !a->opts.subagent};
    json_t *blocks = build_system_prompt(&so);
    strvec_free(&roots);
    sb_free(&gen);
    json_free(labels);
    json_free(refs);
    json_free(xdev_tools);
    free(xdev_docs);
    if (a->plan_mode) {
        json_t *pc = json_obj();
        json_set_str(pc, "writeToolName", "write");
        json_set_str(pc, "editToolName", "edit");
        json_set_str(pc, "askToolName", "ask");
        json_set_bool(pc, "askAvailable", agent_find_tool(a->agent, "ask") != NULL);
        json_set_bool(pc, "taskAvailable", agent_find_tool(a->agent, "task") != NULL);
        json_set_bool(pc, "scoutAvailable", agent_find_tool(a->agent, "task") != NULL);
        json_set_bool(pc, "isHashlineEditMode", str_eq(a->env->edit_mode, "hashline"));
        char *pabs = a->plan_file ? app_plan_path(a, a->plan_file) : NULL;
        json_set_bool(pc, "planExists", pabs && fs_exists(pabs));
        free(pabs);
        if (a->plan_file) json_set_str(pc, "planFilePath", a->plan_file);
        json_set_bool(pc, "reentry", a->plan_reentry);
        char *pm = prompt_render("coding-agent/prompts/system/plan-mode-active.md", pc);
        json_push_str(blocks, pm);
        free(pm);
        json_free(pc);
    }
    /* xdevCatalogNames: the mounted devices count as delivered when block 0 carries the xd:// section */
    const char *b0 = json_sv(json_at(blocks, 0));
    xdnotice_base_rebuilt(a, !a->sys_custom && b0 && strstr(b0, "xd://") && xdev_mounted_count(a->agent) > 0);
    agent_set_system_prompt_parts(a->agent, blocks);
    free(label);
    strvec_free(&names);
}

static void load_session_into_agent(app_t *a) {
    xdnotice_reset_announced(a); /* the replaced transcript's notices no longer apply */
    session_context_t sc;
    session_build_context(a->session, &sc);
    agent_replace_messages(a->agent, sc.messages);
    sc.messages = NULL;
    if (sc.model && !a->opts.model) {
        const ai_model_t *m = catalog_resolve(sc.model);
        if (m) {
            a->model = m;
            agent_set_model(a->agent, m);
        }
    }
    /* restoreServiceTiers: the session's latest service_tier_change, else the configured tier.* settings */
    ai_service_tiers_t tiers;
    if (sc.has_service_tier) modelctl_tiers_from_json(sc.service_tier, &tiers);
    else modelctl_tiers_from_settings(&tiers);
    app_restore_service_tiers(a, &tiers);
    if (sc.thinking && !a->opts.thinking) {
        ai_effort_t e;
        if (str_eq(sc.thinking_configured, "auto")) { /* restoreThinkingLevel(auto): provisional, unresolved */
            a->auto_thinking = true;
            a->auto_resolved = -1;
            a->thinking = autothink_provisional(a->model, &e) ? e : EFFORT_OFF;
            a->agent->thinking = a->thinking;
        } else if (effort_parse(sc.thinking, &e) == 0) {
            a->auto_thinking = false;
            a->auto_resolved = -1;
            a->thinking = e;
            a->agent->thinking = e;
        }
    }
    session_context_free(&sc);
}

void app_reload_context(app_t *a) { load_session_into_agent(a); }

app_t *app_create(const app_opts_t *o, char **err) {
    *err = NULL;
    ai_providers_init();
    catalog_init();
    auth_init();
    catalog_discover_local(false); /* implicit local servers (ollama, llama.cpp, LM Studio); once per process */
    app_t *a = xcalloc(1, sizeof(*a));
    pthread_mutex_init(&a->mu, NULL);
    pthread_mutex_init(&a->tier_mu, NULL);
    app_slow_hooks_init(a);
    a->opts = *o;
    a->text_buffered = o->subagent; /* a subagent's streamed text reaches no sink until the run settles */
    if (o->service_tier && !ai_service_tier_intern(o->service_tier) && !str_eq(o->service_tier, "none")) {
        *err = xasprintf("Invalid --service-tier value: \"%s\". Expected one of: none, auto, default, flex, scale, priority, ultrafast.",
                         o->service_tier);
        app_free(a);
        return NULL;
    }
    if (!o->subagent) {
        modelctl_apply_context_policy();
        modelctl_watch_settings();
        /* prompt-cache warmer for the main agent loop only (spawned sessions opt out) */
        a->warmer = cachewarm_new(a);
        settings_on_change(warm_settings_changed, a);
    }
    a->cwd = o->cwd ? path_abs(o->cwd, NULL) : get_cwd();
    a->sys_custom = o->system_prompt ? xstrdup(o->system_prompt) : NULL;
    a->sys_append = o->append_system_prompt ? xstrdup(o->append_system_prompt) : NULL;
    /* model */
    ai_effort_t eff = EFFORT_OFF;
    bool has_eff = false;
    if (o->model) {
        char *pat = o->provider && !strchr(o->model, '/') ? xasprintf("%s/%s", o->provider, o->model) : xstrdup(o->model);
        a->model = app_resolve_model_pattern(pat, &eff, &has_eff);
        free(pat);
        if (!a->model) {
            *err = xasprintf("Unknown model: %s (see `omc models`)", o->model);
            app_free(a);
            return NULL;
        }
    } else if (o->provider) {
        const char *dm = catalog_provider_default_model(o->provider);
        a->model = dm ? catalog_find(o->provider, dm) : NULL;
        if (!a->model) {
            for (size_t i = 0; i < catalog_count() && !a->model; i++)
                if (!strcmp(catalog_at(i)->provider, o->provider)) a->model = catalog_at(i);
        }
    }
    if (o->api_key && (a->model || o->provider)) auth_set_runtime_key(a->model ? a->model->provider : o->provider, o->api_key);
    a->auto_resolved = -1;
    if (o->thinking) {
        if (!strcmp(o->thinking, "auto")) a->auto_thinking = true;
        else if (effort_parse(o->thinking, &eff) != 0) {
            *err = xasprintf("Invalid thinking level: %s", o->thinking);
            app_free(a);
            return NULL;
        }
        has_eff = true;
    }
    if (!has_eff) {
        const char *dl = settings_str("defaultThinkingLevel", "high");
        if (str_eq(dl, "auto")) a->auto_thinking = true;
        else effort_parse(dl, &eff);
    }
    a->thinking = eff;
    /* main.ts overrideModelRoles: `--model` is the session's default role (roles that inherit it, like smol, follow), with
     * the effort it starts at; ephemeral, never persisted */
    if (o->model && a->model && !o->subagent) {
        char *key = mr_model_key(a->model);
        char *val = has_eff && !a->auto_thinking ? mr_selector_with_level(key, effort_name(eff)) : xstrdup(key);
        settings_set_runtime_quiet("modelRoles.default", json_str_own(val));
        free(key);
    }
    a->guards = guards_new();
    a->thinkloop = thinkloop_new();
    /* environment & discovery */
    a->env = tool_env_new(a->cwd);
    a->env->jobs = jobs_new();
    jobs_set_notify(a->env->jobs, app_job_settled, a);
    a->env->settings = settings_effective();
    a->env->subagent_depth = o->subagent_depth;
    a->env->session = a;
    app_bind_image_attachments(a); /* attachment://N */
    if (!o->subagent) agents_register_main(a);
    a->env->run_subagent = app_run_subagent;
    a->env->judge = app_tool_judge;
    a->env->active_model = app_tool_active_model;
    a->evalhost = evalhost_new(a);
    a->env->eval_bridge = app_eval_bridge;
    discover_all(a->cwd, &a->disc, o->no_context_files, o->no_skills, o->no_rules);
    if (!o->subagent) { /* discoverSlashCommands / discoverPromptTemplates (top-level sessions take user input) */
        commands_load(a->cwd, &a->file_cmds);
        prompt_templates_load(a->cwd, &a->prompt_tpls);
    }
    a->env->disc = &a->disc;
    a->ttsr = ttsr_new(&a->disc);
    if (o->mcp) {
        a->mcp = o->mcp;
    } else if (!o->no_mcp && !o->no_tools && settings_bool("mcp.enabled", true)) {
        json_t *cfg = mcp_discover_config(a->cwd);
        if (json_len(cfg)) {
            a->mcp = mcp_manager_new();
            a->owns_mcp = true;
            mcp_set_tool_cache(a->mcp, true); /* sdk.ts: MCPManager(cwd, new MCPToolCache(storage)) */
            mcp_connect_all(a->mcp, cfg, mcp_startup_timeout_ms());
        }
        json_free(cfg);
    }
    /* session */
    if (o->session_file) {
        a->session = session_create_file(a->cwd, o->session_file);
    } else if (o->no_session) {
        a->session = session_in_memory(a->cwd);
    } else if (o->resume) {
        char *path = session_resolve(o->resume, a->cwd);
        if (!path) {
            *err = xasprintf("Session not found: %s", o->resume);
            app_free(a);
            return NULL;
        }
        a->session = session_open(path, err);
        free(path);
        if (!a->session) {
            app_free(a);
            return NULL;
        }
    } else if (o->fork) {
        char *path = session_resolve(o->fork, a->cwd);
        session_t *src = path ? session_open(path, err) : NULL;
        free(path);
        if (!src) {
            if (!*err) *err = xasprintf("Session not found: %s", o->fork);
            app_free(a);
            return NULL;
        }
        a->session = session_fork(src, NULL, o->session_dir);
        session_free(src);
    } else if (o->continue_recent) {
        a->session = session_continue_recent(a->cwd, o->session_dir);
        if (!a->session) a->session = session_create(a->cwd, o->session_dir);
    } else {
        a->session = session_create(a->cwd, o->session_dir);
    }
    if (!a->model) {
        session_context_t sc;
        session_build_context(a->session, &sc);
        if (sc.model) a->model = catalog_resolve(sc.model);
        session_context_free(&sc);
    }
    /* workspace roots: --add-dir / workspace.additionalDirectories (a subagent: its parent's), merged with the
     * ones the session header restored (sdk.ts configuredDirs) */
    {
        strvec_t conf = {0};
        if (o->add_dirs) {
            for (size_t i = 0; i < o->add_dirs->n; i++) strvec_push(&conf, o->add_dirs->v[i]);
        } else {
            json_t *x;
            json_arr_foreach(settings_get("workspace.additionalDirectories"), i, x) if (json_is_str(x)) strvec_push(&conf, json_sv(x));
        }
        if (conf.n) {
            strvec_t merged = session_additional_dirs(a->session);
            for (size_t i = 0; i < conf.n; i++)
                if (!strvec_contains(&merged, conf.v[i])) strvec_push(&merged, conf.v[i]);
            session_set_additional_dirs(a->session, &merged);
            strvec_free(&merged);
        }
        strvec_free(&conf);
    }
    if (!a->model) a->model = app_default_model();
    if (!a->model && !o->allow_no_model) {
        *err = xstrdup("No model available. Set an API key (e.g. ANTHROPIC_API_KEY or OPENAI_API_KEY), run `omc login`, "
                       "or pass --model provider/id with --api-key.");
        app_free(a);
        return NULL;
    }
    if (!o->subagent) app_bind_artifacts(a, NULL);
    app_bind_local(a);
    a->skilldesc = skilldesc_new();
    if (o->subagent) skilldesc_start_compression(a->skilldesc); /* the tree's cost gate already exists */
    a->memory = memory_rt_new();
    if (!o->subagent) a->autolearn = autolearn_new(a);
    if (a->auto_thinking) { /* keep auto pending until the first turn while exposing a valid wire effort */
        ai_effort_t pe;
        a->thinking = autothink_provisional(a->model, &pe) ? pe : EFFORT_OFF;
    }
    install_agent(a);
    modelctl_tiers_from_settings(&a->tiers);
    bool resumed = json_len(a->session->entries) > 0;
    if (resumed) load_session_into_agent(a);
    /* sdk.ts initial service tiers: a resolver (subagent tier.subagent) or --service-tier overrides the session's /
     * configured map and is persisted */
    bool persist_tiers = o->service_tier || o->service_tiers;
    if (o->service_tiers) app_restore_service_tiers(a, o->service_tiers);
    if (o->service_tier) {
        ai_service_tiers_t t = a->tiers;
        t.openai = ai_service_tier_intern(o->service_tier);
        app_restore_service_tiers(a, &t);
    }
    if (resumed && persist_tiers) app_append_service_tier_entry(a);
    if (!o->subagent) app_goal_restore(a);
    app_rebuild_system_prompt(a);
    xdnotice_init(a);
    if (!json_len(a->session->entries) && a->session->persist && a->model) {
        char *label = app_model_label(a);
        session_append_model_change(a->session, label, NULL);
        /* `auto` is not written before the first turn resolves (sdk.ts) */
        if (!a->auto_thinking) session_append_thinking_change(a->session, effort_name(a->thinking));
        if (persist_tiers || !modelctl_tiers_empty(&a->tiers)) app_append_service_tier_entry(a);
        free(label);
    }
    a->title_done = a->session->title != NULL;
    if (settings_bool("advisor.enabled", false) && !o->subagent) app_set_advisor(a, true);
    if (!o->subagent) {
        a->prewalk = prewalk_new();
        prewalk_startup(a, o->prewalk, o->prewalk_into, o->resume || o->continue_recent);
    }
    memory_start(a);
    return a;
}

void app_free(app_t *a) {
    if (!a) return;
    json_free(a->rewind_graveyard);
    xdnotice_free(a);
    json_free(a->queue_snapshot);
    a->queue_snapshot = NULL;
    if (a->warmer) {
        settings_off_change(warm_settings_changed, a);
        cachewarm_stop(a->warmer); /* stop a refresh before the session state it reads goes away */
        app_flush_warm_usage(a);
        cachewarm_free(a->warmer);
        a->warmer = NULL;
    }
    agents_unregister(a);
    cmp_free(a); /* join a background speculative compaction */
    autolearn_free(a->autolearn); /* abort a private capture before the session it reads goes away */
    a->autolearn = NULL;
    prewalk_close(a);
    memory_rt_free(a->memory);   /* cancel + join the memory startup pipeline */
    a->memory = NULL;
    evalhost_shutdown(a->evalhost); /* cancel completions / judge batches before their jobs are joined */
    if (a->env && a->env->jobs) {
        /* join background jobs (task runs, auto-backgrounded commands) before their session state goes away */
        jobs_set_notify(a->env->jobs, NULL, NULL);
        jobs_free(a->env->jobs);
        a->env->jobs = NULL;
    }
    agents_release_children(a);
    agent_free(a->agent); /* the eval tool's bridge workers finish here */
    evalhost_free(a->evalhost);
    session_free(a->session);
    tool_env_free(a->env);
    ttsr_free(a->ttsr);
    if (a->owns_mcp) mcp_manager_free(a->mcp);
    if (a->owns_lsp) lsp_manager_free(a->lsp);
    dap_manager_free(a->dap);
    discovery_free(&a->disc);
    commands_free(&a->file_cmds);
    prompt_templates_free(&a->prompt_tpls);
    free(a->cwd);
    free(a->sys_custom);
    free(a->sys_append);
    free(a->plan_file);
    free(a->plan_reference_path);
    free(a->fallback.role);
    free(a->fallback.original);
    strvec_free(&a->retry_notes);
    free(a->model_role);
    free(a->checkpoint_entry);
    free(a->pending_rewind);
    advisor_free(a->advisor);
    goal_free(a->goal);
    guards_free(a->guards);
    thinkloop_free(a->thinkloop);
    title_free(a);
    skilldesc_free(a->skilldesc);
    json_free(a->forced_choice);
    dcr_free(a->dcr);
    dcr_free(a->dcr_side);
    json_free(a->job_batch);
    json_free(a->fast_disabled);
    free(a->slow_lane);
    pthread_mutex_destroy(&a->tier_mu);
    strvec_free(&a->provider_states);
    pthread_mutex_destroy(&a->mu);
    free(a);
}

int app_set_model(app_t *a, const char *pattern, char **err) {
    ai_effort_t e;
    bool has_e;
    const ai_model_t *m = app_resolve_model_pattern(pattern, &e, &has_e);
    if (!m) {
        *err = xasprintf("Unknown model: %s", pattern);
        return -1;
    }
    app_apply_model(a, m, has_e, e);
    return 0;
}

void app_use_model(app_t *a, const ai_model_t *m) { app_apply_model(a, m, false, EFFORT_OFF); }

void app_apply_model(app_t *a, const ai_model_t *m, bool has_e, ai_effort_t e) {
    const ai_model_t *prev_model = a->model;
    const char *prev_wire = app_wire_thinking(a);
    a->model = m;
    agent_set_model(a->agent, m);
    a->fallback.active = false; /* an explicit model change clears fallback ownership */
    prewalk_release_handoff(a);
    free(a->model_role);
    a->model_role = NULL;
    char *uk = mr_model_key(m);
    mr_usage_record(uk);
    free(uk);
    if (has_e) app_set_thinking(a, e);
    else if (a->auto_thinking) app_set_thinking_auto(a); /* #reapplyThinkingLevel keeps auto, re-clamped */
    char *label = app_model_label(a);
    session_append_model_change(a->session, label, NULL);
    free(label);
    app_rebuild_system_prompt(a);
    app_model_state_changed(a, prev_model, prev_wire);
}

void app_set_thinking(app_t *a, ai_effort_t e) {
    const char *prev_wire = app_wire_thinking(a);
    bool was_auto = a->auto_thinking;
    a->auto_thinking = false;
    a->auto_resolved = -1;
    a->thinking = e;
    a->agent->thinking = e;
    session_append_thinking_change(a->session, effort_name(e));
    if (was_auto && str_eq(prev_wire, app_wire_thinking(a)) && a->session_event) {
        /* leaving auto is a change even when the effort stays (model-controls.ts isChanging) */
        json_t *ev = json_obj();
        json_set_str(ev, "type", "thinking_level_changed");
        if (app_wire_thinking(a)) json_set_str(ev, "thinkingLevel", app_wire_thinking(a));
        app_emit(a, ev);
    } else app_model_state_changed(a, a->model, prev_wire);
}

void app_set_thinking_auto(app_t *a) {
    ai_effort_t pe;
    ai_effort_t prov = autothink_provisional(a->model, &pe) ? pe : EFFORT_OFF;
    bool was = a->auto_thinking;
    ai_effort_t prev = a->thinking;
    a->auto_thinking = true;
    a->auto_resolved = -1;
    a->thinking = prov;
    a->agent->thinking = prov;
    if (!was || prev != prov) {
        session_append_thinking_change_ex(a->session, a->model && a->model->reasoning ? effort_name(prov) : NULL, "auto");
        json_t *ev = json_obj();
        json_set_str(ev, "type", "thinking_level_changed");
        if (app_wire_thinking(a)) json_set_str(ev, "thinkingLevel", app_wire_thinking(a));
        json_set_str(ev, "configured", "auto");
        app_emit(a, ev);
    }
}

bool app_set_thinking_level(app_t *a, const char *level) {
    if (str_eq(level, "auto")) {
        app_set_thinking_auto(a);
        return true;
    }
    ai_effort_t e;
    if (!level || effort_parse(level, &e) != 0) return false;
    app_set_thinking(a, e);
    return true;
}

void app_abort(app_t *a) {
    __atomic_add_fetch(&a->abort_gen, 1, __ATOMIC_SEQ_CST);
    agent_abort(a->agent);
    app_goal_on_abort(a);
}

/* ---------- one-shot completion ---------- */

char *app_complete_text(app_t *a, const ai_model_t *m, const char *system, const char *user, ai_effort_t effort) {
    if (!m) m = a->model;
    json_t *sys = json_arr();
    if (system) json_push_str(sys, system);
    json_t *msgs = json_arr();
    json_push(msgs, msg_user_text(user));
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    char *key = app_api_key_for(m);
    o.api_key = key;
    o.reasoning = model_clamp_effort(m, effort);
    o.disable_reasoning = effort == EFFORT_OFF;
    o.cancel = &a->agent->cancel;
    json_t *r = ai_complete(m, &ctx, &o);
    char *text = NULL;
    const char *stop = json_get_str(r, "stopReason");
    if (!str_eq(stop, "error") && !str_eq(stop, "aborted")) text = ai_message_text(r);
    else LOGW("one-shot completion failed: %s", json_get_str(r, "errorMessage"));
    json_free(r);
    json_free(sys);
    json_free(msgs);
    free(key);
    return text;
}

strvec_t app_cycle_order(void) {
    strvec_t v = {0};
    json_t *c = settings_get("cycleOrder"), *x;
    if (json_is_arr(c)) {
        json_arr_foreach(c, i, x) if (json_is_str(x)) strvec_push(&v, json_sv(x));
    } else {
        strvec_push(&v, "smol");
        strvec_push(&v, "default");
        strvec_push(&v, "slow");
    }
    return v;
}

/* #reapplyThinkingLevel after a model change: keep auto (re-clamped), else the new model's
 * thinking.defaultLevel, else the level that was effective on the previous model, clamped to the new one
 * (an unverified ladder stays as it is until the user picks) */
static void reapply_thinking(app_t *a, const ai_model_t *prev) {
    if (a->auto_thinking) {
        app_set_thinking_auto(a);
        return;
    }
    const ai_model_t *m = a->model;
    if (!m || json_get_bool(m->thinking, "unverified", false)) return;
    ai_effort_t base = prev ? model_clamp_effort(prev, a->thinking) : a->thinking, d;
    const char *dl = json_get_str(m->thinking, "defaultLevel");
    if (dl && effort_parse(dl, &d) == 0) base = d;
    ai_effort_t eff = base == EFFORT_OFF ? EFFORT_OFF : model_clamp_effort(m, base);
    if (eff == a->thinking) return;
    if (prev && eff == model_clamp_effort(prev, a->thinking)) {
        /* omp already stores the level clamped to the previous model: nothing changes there */
        a->thinking = eff;
        a->agent->thinking = eff;
        return;
    }
    app_set_thinking(a, eff);
}

size_t app_role_cycle(app_t *a, app_role_entry_t **out, int *current) {
    *out = NULL;
    if (current) *current = 0;
    if (!a->model) return 0;
    strvec_t order = app_cycle_order();
    mr_pool_t avail = {0};
    mr_pool_available(&avail, "chat");
    VEC(app_role_entry_t) es = {0};
    for (size_t i = 0; i < order.n && avail.n; i++) {
        char *cur = NULL;
        const char *v = mr_role_get(order.v[i]);
        if (!v && !strcmp(order.v[i], "default")) v = cur = mr_model_key(a->model);
        mr_result_t r = {0};
        if (v && mr_resolve_value(v, &avail, &r))
            vec_push(&es, ((app_role_entry_t){xstrdup(order.v[i]), (int)i, r.model, r.level, r.explicit_level}));
        mr_result_clear(&r);
        free(cur);
    }
    /* the recorded role counts only while its resolved model is the live one */
    long at = -1;
    for (size_t i = 0; i < es.n && a->model_role && at < 0; i++)
        if (!strcmp(es.v[i].role, a->model_role)) at = es.v[i].model == a->model ? (long)i : -2;
    if (at < 0)
        for (size_t i = 0; i < es.n && at < 0; i++)
            if (es.v[i].model == a->model) at = (long)i;
    if (at < 0) at = 0;
    if (current) *current = (int)at;
    mr_pool_free(&avail);
    strvec_free(&order);
    *out = es.v;
    return es.n;
}

void app_role_cycle_free(app_role_entry_t *v, size_t n) {
    for (size_t i = 0; i < n; i++) free(v[i].role);
    free(v);
}

void app_apply_role_entry(app_t *a, const app_role_entry_t *e) {
    const ai_model_t *prev_model = a->model;
    const char *prev_wire = app_wire_thinking(a);
    a->model = e->model;
    agent_set_model(a->agent, e->model);
    app_model_state_changed(a, prev_model, prev_wire);
    a->fallback.active = false;
    free(a->model_role);
    a->model_role = xstrdup(e->role);
    char *k = mr_model_key(e->model);
    mr_clear_suppressed_selector(k);
    session_append_model_change(a->session, k, e->role);
    mr_usage_record(k);
    free(k);
    ai_effort_t ef;
    reapply_thinking(a, prev_model);
    if (e->explicit_level && e->level && effort_parse(e->level, &ef) == 0) app_set_thinking(a, ef);
    app_rebuild_system_prompt(a);
}

int app_cycle_role_model(app_t *a, bool backward) {
    int at;
    app_role_entry_t *es;
    size_t n = app_role_cycle(a, &es, &at);
    int res = -1;
    if (n > 1) {
        const app_role_entry_t *nx = &es[(at + (backward ? -1 : 1) + (long)n) % (long)n];
        app_apply_role_entry(a, nx);
        res = nx->order_index;
    }
    app_role_cycle_free(es, n);
    return res;
}

/* resolveTemporaryModelThinkingLevel: the explicit level of a configured role resolving to `m` (static) */
const char *app_temporary_thinking(const ai_model_t *m) {
    mr_pool_t avail = {0};
    mr_pool_available(&avail, "all");
    const char *lv = NULL;
    strvec_t roles = mr_known_roles();
    for (size_t i = 0; i < roles.n && !lv && avail.n; i++) {
        const char *v = mr_role_get(roles.v[i]);
        if (!v) continue;
        mr_result_t r = {0};
        if (mr_resolve_value(v, &avail, &r) && r.explicit_level && r.level && r.model == m) lv = r.level;
        mr_result_clear(&r);
    }
    strvec_free(&roles);
    mr_pool_free(&avail);
    return lv;
}

int app_set_model_temporary(app_t *a, const ai_model_t *m, const char *level, const char *role, char **err) {
    char *key = app_api_key_for(m);
    if (!key && !catalog_provider_keyless(m->provider)) {
        *err = xasprintf("No API key for %s/%s", m->provider, m->id);
        return -1;
    }
    free(key);
    const ai_model_t *prev_model = a->model;
    const char *prev_wire = app_wire_thinking(a);
    char *k = mr_model_key(m);
    mr_clear_suppressed_selector(k);
    a->fallback.active = false;  /* clearActiveRetryFallback */
    a->model = m;
    agent_set_model(a->agent, m);
    free(a->model_role);
    a->model_role = NULL;
    session_append_model_change(a->session, k, role ? role : "temporary");
    if (!role) prewalk_release_handoff(a); /* an explicit selection owns the active model */
    mr_usage_record(k);
    free(k);
    if (str_eq(level, "auto")) app_set_thinking_auto(a);
    else {
        ai_effort_t e;
        if (level && strcmp(level, "inherit") && effort_parse(level, &e) == 0) app_set_thinking(a, e);
        else reapply_thinking(a, prev_model);
    }
    app_rebuild_system_prompt(a);
    app_model_state_changed(a, prev_model, prev_wire);
    return 0;
}

/* try each model (then the session model) until one answers */
char *app_complete_first(app_t *a, const mr_pool_t *models, const char *system, const char *user, ai_effort_t effort) {
    for (size_t i = 0; models && i < models->n; i++) {
        if (models->v[i] == a->model) continue;
        char *t = app_complete_text(a, models->v[i], system, user, effort);
        if (t) return t;
        if (a->agent->cancel) return NULL;
    }
    return a->model ? app_complete_text(a, a->model, system, user, effort) : NULL;
}

/* ---------- advisor ---------- */

/* setAdvisorEnabled: the setting flips; a runtime exists only when the `advisor` role resolves */
bool app_set_advisor(app_t *a, bool on) {
    a->advisor_enabled = on;
    if (on && !a->advisor && advisor_role_model()) a->advisor = advisor_new(a);
    else if (!on && a->advisor) {
        advisor_free(a->advisor);
        a->advisor = NULL;
    }
    return a->advisor != NULL;
}

char *app_job_results_text(app_t *a) {
    json_t *done = jobs_take_finished(a->env->jobs);
    char *txt = json_len(done) ? jobs_render_results(done) : NULL;
    json_free(a->job_batch);
    a->job_batch = json_len(done) ? job_batch_details(done) : NULL;
    json_free(done);
    /* peer messages that arrived while this session was idle */
    char *irc = agents_take_pending_for(a);
    if (irc) {
        char *both = txt ? xasprintf("%s\n\n%s", txt, irc) : xstrdup(irc);
        free(txt);
        free(irc);
        txt = both;
    }
    return txt;
}

json_t *app_fold_job_results(app_t *a) {
    if (!a->env || !a->env->jobs) return NULL;
    json_t *m = job_results_message(a);
    if (!m) return NULL;
    if (a->session) { /* persisted like a delivered batch (on_agent_event's hidden custom_message entry) */
        char *txt = ai_message_text(m);
        json_t *e = json_obj();
        json_set_str(e, "type", "custom_message");
        json_set_str(e, "customType", "async-result");
        json_set_str(e, "content", txt);
        json_set_bool(e, "display", false);
        if (json_get(m, "details")) json_set(e, "details", json_clone(json_get(m, "details")));
        session_append_entry(a->session, e);
        free(txt);
    }
    json_t *view = json_clone(m);
    agent_append_message(a->agent, m);
    return view;
}

bool app_pending_async_work(app_t *a) {
    return a && a->env && a->env->jobs && (jobs_running(a->env->jobs) > 0 || jobs_undelivered(a->env->jobs) > 0);
}

char *app_after_run(app_t *a, const json_t *run_messages, const char *prev_type, const char *mode, const char **custom_type) {
    *custom_type = NULL;
    /* print mode has no idle wake-up: finish outstanding background jobs before the final answer */
    if (!strcmp(mode, "print") && jobs_running(a->env->jobs) && !str_eq(json_get_str(last_assistant(a->agent->messages), "stopReason"), "aborted")) {
        int64_t deadline = mono_ms() + settings_int("async.printWaitMs", 30 * 60 * 1000);
        while (mono_ms() < deadline && !agents_has_pending(a) &&
               !jobs_wait_any(a->env->jobs, NULL, MIN(500, MAX(1, deadline - mono_ms()))) && jobs_running(a->env->jobs)) {}
    }
    if (jobs_undelivered(a->env->jobs) || agents_has_pending(a)) {
        char *t = app_job_results_text(a);
        if (t) {
            *custom_type = "async-result";
            return t;
        }
    }
    const char *stop = json_get_str(last_assistant(a->agent->messages), "stopReason");
    if (a->advisor && !str_eq(stop, "aborted")) {
        bool cont = false;
        char *t = advisor_after_run(a->advisor, &cont);
        if (t && cont) {
            *custom_type = "advisor";
            return t;
        }
        free(t);
    }
    char *g = app_goal_after_run(a, run_messages, str_eq(prev_type, "goal-continuation"), mode);
    if (g) *custom_type = "goal-continuation";
    return g;
}

/* ---------- side questions, branching, maintenance ---------- */

/* dedupeEphemeralReply (session/messages.ts): runs of more than three identical lines collapse to the line and
 * `[…N×]`; a positive max_bytes bounds the result on a UTF-8 boundary with a `\n[…truncated]` suffix. Owned. */
char *app_dedupe_ephemeral_reply(const char *text, size_t max_bytes) {
    if (!text || !*text) return xstrdup(text ? text : "");
    strvec_t lines = {0};
    for (const char *p = text;;) {
        const char *nl = strchr(p, '\n');
        strvec_push_own(&lines, nl ? xstrndup(p, (size_t)(nl - p)) : xstrdup(p));
        if (!nl) break;
        p = nl + 1;
    }
    sb_t b = SB_INIT;
    bool first = true;
    for (size_t i = 0; i < lines.n;) {
        size_t j = i + 1;
        while (j < lines.n && !strcmp(lines.v[j], lines.v[i])) j++;
        size_t run = j - i;
        size_t emit = run > 3 ? 1 : run;
        for (size_t k = 0; k < emit; k++) {
            if (!first) sb_putc(&b, '\n');
            sb_append(&b, lines.v[i]);
            first = false;
        }
        if (run > 3) sb_appendf(&b, "\n[\xe2\x80\xa6%zu\xc3\x97]", run);
        i = j;
    }
    strvec_free(&lines);
    if (!max_bytes || b.len <= max_bytes) return sb_detach(&b);
    static const char suffix[] = "\n[\xe2\x80\xa6truncated]";
    size_t cut = max_bytes > sizeof suffix - 1 ? max_bytes - (sizeof suffix - 1) : 0;
    while (cut > 0 && ((unsigned char)b.s[cut] & 0xc0) == 0x80) cut--;
    sb_truncate(&b, cut);
    sb_append(&b, suffix);
    return sb_detach(&b);
}

typedef struct {
    const app_ephemeral_t *e;
    sb_t text; /* provider reply text so far */
} eph_ud_t;

static void eph_event(void *ud, const ai_event_t *ev) {
    eph_ud_t *x = ud;
    if (ev->type != AI_EV_TEXT_DELTA || !ev->delta) return;
    size_t n = ev->delta_len ? ev->delta_len : strlen(ev->delta);
    if (!n) return;
    sb_appendn(&x->text, ev->delta, n);
    if (x->e->on_text_delta) {
        char *d = xstrndup(ev->delta, n);
        x->e->on_text_delta(x->e->ud, d);
        free(d);
    }
}

/* AgentSession.runEphemeralTurn: a single side-channel turn against the session's current model, system
 * prompt, tool catalog (kept for the prompt cache; the model is told tools are unavailable and any call is
 * dropped) and history. Never touches the session or the agent's messages. */
json_t *app_ephemeral_turn(app_t *a, const app_ephemeral_t *e, char **reply, char **err) {
    *reply = NULL;
    *err = NULL;
    const ai_model_t *model = a->model;
    if (!model) {
        *err = xstrdup("No active model on session");
        return NULL;
    }
    pthread_mutex_lock(&a->agent->mu);
    json_t *msgs = json_clone(a->agent->messages);
    json_t *sys = json_clone(a->agent->system_prompt);
    json_t *tools = json_arr();
    for (size_t i = 0; i < a->agent->tools.n; i++) json_push(tools, agent_tool_wire(a->agent->tools.v[i]));
    char *main_sid = a->agent->session_id ? xstrdup(a->agent->session_id) : NULL;
    pthread_mutex_unlock(&a->agent->mu);
    /* the in-flight streaming reply: its thinking blocks and joined text replace (or follow) the last message */
    const json_t *st = e->streaming;
    bool per_call_message = false; /* markPerCallContextMessage: rebuilt next request, so no thread can continue past it */
    if (st && str_eq(json_get_str(st, "role"), "assistant") && json_is_arr(json_get(st, "content"))) {
        json_t *blocks = json_arr(), *blk;
        sb_t txt = SB_INIT;
        json_arr_foreach(json_get(st, "content"), i, blk) {
            const char *t = json_get_str(blk, "type");
            if (str_eq(t, "thinking")) json_push(blocks, json_clone(blk));
            else if (str_eq(t, "text") && json_get_str(blk, "text")) sb_append(&txt, json_get_str(blk, "text"));
        }
        if (txt.len) json_push(blocks, text_block(sb_cstr(&txt)));
        sb_free(&txt);
        if (json_len(blocks)) {
            json_t *m = json_clone(st);
            json_set(m, "content", blocks);
            size_t n = json_len(msgs);
            if (n && str_eq(json_get_str(json_at(msgs, n - 1), "role"), "assistant")) json_free(json_arr_remove(msgs, n - 1));
            json_push(msgs, m);
            per_call_message = true;
        } else {
            json_free(blocks);
        }
    }
    char *reminder = prompt_load("coding-agent/prompts/system/side-channel-no-tools.md");
    json_t *dev = msg_developer_text(reminder ? reminder : "");
    json_set_str(dev, "attribution", "agent");
    json_push(msgs, dev);
    free(reminder);
    json_t *h;
    json_arr_foreach(e->history, i, h) json_push(msgs, json_clone(h));
    json_t *content = json_arr();
    json_push(content, text_block(e->prompt_text ? e->prompt_text : ""));
    json_arr_foreach(e->prompt_images, i, h) json_push(content, json_clone(h));
    json_t *um = msg_user_content(content);
    json_set_str(um, "attribution", "agent");
    json_push(msgs, um);
    dcr_set_side_thread(true); /* runs next to the turn that writes the session */
    json_t *tx = a->agent->hooks.transform_context ? a->agent->hooks.transform_context(a, a->agent, msgs) : NULL;
    dcr_set_side_thread(false);
    json_t *wire = ai_transform_messages(tx ? tx : msgs, model);
    json_free(tx);
    json_free(msgs);
    ai_context_t ctx = {.system_prompt = sys, .messages = wire, .tools = json_len(tools) ? tools : NULL};
    ai_stream_opts_t o = a->agent->base_opts;
    char *key = app_api_key_for(model);
    o.api_key = key;
    fill_request_opts(a, &o);
    /* side requests never share provider conversation state with the main turn; a keyed topic keeps one lineage,
     * while the prompt-cache key stays the main session's */
    char *rid = e->conversation_key ? NULL : short_id();
    char *sid = e->conversation_key ? xasprintf("%s:side:conversation:%s", main_sid ? main_sid : "", e->conversation_key)
                                    : xasprintf("%s:side:%s", main_sid ? main_sid : "", rid);
    free(rid);
    o.session_id = sid;
    o.provider_session_state = !per_call_message;
    o.metadata = (json_t *)app_session_metadata(a, model->provider);
    o.prompt_cache_key = main_sid;
    o.reasoning = model_clamp_effort(model, a->thinking);
    o.disable_reasoning = a->thinking == EFFORT_OFF;
    o.cancel = e->cancel;
    o.on_payload = NULL;
    o.cursor_exec = NULL;
    eph_ud_t x = {.e = e};
    json_t *r = ai_stream(model, &ctx, &o, eph_event, &x);
    if (!e->conversation_key) { /* a one-shot side id: its chain would pin the conversation, its socket stay open */
        anth_threads_release_session(sid);
        rws_release_session(sid);
    }
    json_free(wire);
    json_free(tools);
    json_free(sys);
    free(key);
    free(sid);
    free(main_sid);
    const char *stop = json_get_str(r, "stopReason");
    bool cancelled = e->cancel && *e->cancel;
    if (!r || str_eq(stop, "error") || str_eq(stop, "aborted") || cancelled) {
        const char *em = json_get_str(r, "errorMessage");
        *err = xstrdup(cancelled || str_eq(stop, "aborted") ? "Request was aborted"
                       : em && *em                          ? em
                                                            : "Ephemeral turn failed");
        json_free(r);
        sb_free(&x.text);
        return NULL;
    }
    /* text the provider produced but never streamed as deltas */
    char *full = ai_message_text(r);
    const char *got = sb_cstr(&x.text);
    if (e->on_text_delta && full && strlen(full) > x.text.len && !strncmp(full, got, x.text.len))
        e->on_text_delta(e->ud, full + x.text.len);
    char *trim = str_trim(full && strlen(full) > x.text.len ? full : got);
    *reply = e->dedupe ? app_dedupe_ephemeral_reply(trim, e->reply_max_bytes) : xstrdup(trim);
    free(trim);
    free(full);
    sb_free(&x.text);
    json_t *blocks = json_arr(), *blk;
    json_arr_foreach(json_get(r, "content"), i, blk) {
        if (!str_eq(json_get_str(blk, "type"), "toolCall")) json_push(blocks, json_clone(blk));
    }
    json_set(r, "content", blocks);
    return r;
}

int app_branch_to(app_t *a, const char *entry_id) {
    if (!a->session || agent_is_running(a->agent)) return -1;
    if (entry_id) session_branch(a->session, entry_id);
    else session_reset_leaf(a->session);
    load_session_into_agent(a);
    if (!entry_id) agent_replace_messages(a->agent, json_arr());
    advisor_reset(a->advisor);
    return 0;
}

/* ---------- tree navigation (AgentSession.navigateTree + generateBranchSummary) ---------- */

static volatile sig_atomic_t g_branch_summary_cancel;

void app_abort_branch_summary(app_t *a) {
    UNUSED(a);
    g_branch_summary_cancel = 1;
}

static bool is_user_request_entry(const json_t *e) {
    const char *t = json_get_str(e, "type");
    const json_t *m = str_eq(t, "message") ? json_get(e, "message") : e;
    const char *role = str_eq(t, "message") ? json_get_str(m, "role") : str_eq(t, "custom_message") ? "custom" : NULL;
    if (str_eq(role, "user")) return true;
    const char *ct = json_get_str(m, "customType");
    return str_eq(role, "custom") && (str_eq(ct, "skill-prompt") || str_eq(ct, "collab-prompt")) &&
           str_eq(json_get_str(m, "attribution"), "user");
}

static char *content_text(const json_t *content) {
    if (json_is_str(content)) return xstrdup(json_sv(content));
    sb_t b = SB_INIT;
    const json_t *blk;
    json_arr_foreach(content, i, blk) if (str_eq(json_get_str(blk, "type"), "text"))
        sb_append(&b, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
    return sb_detach(&b);
}

/* generateBranchSummary: 0 ok (*summary, *details owned), 1 aborted, -1 error */
static int branch_summary(app_t *a, const json_t *entries, const char *custom, char **summary, json_t **details,
                          char **err) {
    const ai_model_t *m = a->model;
    int64_t window = m->context_window > 0 ? m->context_window : 128000;
    json_t *ops = NULL;
    const json_t *identity = m->raw ? json_get_obj(m->raw, "identity") : NULL;
    const char *dialect = branchsum_dialect(identity ? json_get_str(identity, "class") : NULL, m->id);
    char *prompt = branchsum_prompt(entries, window - settings_int("branchSummary.reserveTokens", 16384), custom, dialect, &ops);
    if (!prompt) {
        json_free(ops);
        *summary = xstrdup("No content to summarize");
        *details = json_parse_str("{\"readFiles\":[],\"modifiedFiles\":[]}");
        return 0;
    }
    char *sys = prompt_load_or("agent/compaction/prompts/summarization-system.md", "Summarize.");
    json_t *sysarr = json_arr();
    json_push_str(sysarr, sys);
    json_t *msgs = json_arr();
    json_push(msgs, msg_user_text(prompt));
    ai_context_t ctx = {.system_prompt = sysarr, .messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    char *key = app_api_key_for(m);
    o.api_key = key;
    o.max_tokens = 2048;
    o.disable_reasoning = true;
    o.cancel = &g_branch_summary_cancel;
    json_t *r = ai_complete(m, &ctx, &o);
    const char *stop = json_get_str(r, "stopReason");
    int rc = 0;
    if (str_eq(stop, "aborted") || g_branch_summary_cancel) rc = 1;
    else if (str_eq(stop, "error")) {
        const char *em = json_get_str(r, "errorMessage");
        *err = xstrdup(em && *em ? em : "Summarization failed");
        rc = -1;
    } else if (str_eq(stop, "length")) {
        *err = xstrdup("Summarization failed: model output ended incomplete (stopReason: length)");
        rc = -1;
    } else {
        sb_t text = SB_INIT;
        const json_t *blk;
        bool first = true;
        json_arr_foreach(json_get_arr(r, "content"), i, blk) {
            if (!str_eq(json_get_str(blk, "type"), "text")) continue;
            if (!first) sb_putc(&text, '\n');
            sb_append(&text, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
            first = false;
        }
        json_t *rf = NULL, *mf = NULL;
        *summary = branchsum_finish(sb_cstr(&text), ops, &rf, &mf);
        *details = json_obj();
        json_set(*details, "readFiles", rf);
        json_set(*details, "modifiedFiles", mf);
        sb_free(&text);
    }
    json_free(r);
    json_free(sysarr);
    json_free(msgs);
    json_free(ops);
    free(key);
    free(sys);
    free(prompt);
    return rc;
}

int app_navigate_tree(app_t *a, const char *target_id, bool summarize, const char *custom, app_nav_result_t *out,
                      char **err) {
    memset(out, 0, sizeof *out);
    *err = NULL;
    if (!a->session) {
        *err = xstrdup("No session");
        return -1;
    }
    if (agent_is_running(a->agent)) {
        *err = xstrdup("Cannot switch branches while the agent is running.");
        return -1;
    }
    json_t *target = session_get(a->session, target_id);
    if (!target) {
        *err = xasprintf("Entry %s not found", target_id);
        return -1;
    }
    bool user_target = str_eq(json_get_str(target, "type"), "message") &&
                       str_eq(json_get_str(json_get(target, "message"), "role"), "user");
    char *old_leaf = a->session->leaf_id ? xstrdup(a->session->leaf_id) : NULL;
    if (str_eq(target_id, old_leaf) && !user_target) {
        free(old_leaf);
        return 0;
    }
    if (summarize && !a->model) {
        free(old_leaf);
        *err = xstrdup("No model available for summarization");
        return -1;
    }
    char *summary = NULL;
    json_t *details = NULL;
    json_t *entries = branchsum_collect(a->session, old_leaf, target_id, NULL);
    if (summarize && json_len(entries)) {
        g_branch_summary_cancel = 0;
        int rc = branch_summary(a, entries, custom, &summary, &details, err);
        g_branch_summary_cancel = 0;
        if (rc != 0) {
            json_free(entries);
            free(old_leaf);
            return rc;
        }
    }
    json_free(entries);
    free(old_leaf);
    /* the new leaf: a user request (or another non-skill custom message) rewinds past itself */
    const char *new_leaf;
    const char *ttype = json_get_str(target, "type");
    if (is_user_request_entry(target)) {
        new_leaf = json_get_str(target, "parentId");
        const json_t *m = str_eq(ttype, "message") ? json_get(target, "message") : target;
        out->editor_text = content_text(json_get(m, "content"));
    } else if (str_eq(ttype, "custom_message") && !str_eq(json_get_str(target, "customType"), "skill-prompt")) {
        new_leaf = json_get_str(target, "parentId");
        out->editor_text = content_text(json_get(target, "content"));
    } else new_leaf = target_id;
    char *leaf = new_leaf ? xstrdup(new_leaf) : NULL;
    if (leaf) session_branch(a->session, leaf);
    else session_reset_leaf(a->session);
    if (summary) {
        json_t *e = json_obj();
        json_set_str(e, "type", "branch_summary");
        json_set_str(e, "fromId", leaf ? leaf : "root");
        json_set_str(e, "summary", summary);
        if (details) json_set(e, "details", details);
        details = NULL;
        json_set_bool(e, "fromExtension", false); /* the default summarizer (no extension hook) */
        session_append_entry(a->session, e);
        out->summarized = true;
    }
    json_free(details);
    free(summary);
    free(leaf);
    load_session_into_agent(a);
    if (!a->session->leaf_id) agent_replace_messages(a->agent, json_arr());
    advisor_reset(a->advisor);
    /* the context gauge follows the new branch: its last assistant usage */
    a->last_context_tokens = 0;
    pthread_mutex_lock(&a->agent->mu);
    for (size_t i = json_len(a->agent->messages); i-- > 0;) {
        const json_t *msg = json_at(a->agent->messages, i);
        if (!str_eq(json_get_str(msg, "role"), "assistant")) continue;
        const json_t *u = json_get(msg, "usage");
        int64_t ctx = json_get_int(u, "input", 0) + json_get_int(u, "cacheRead", 0) + json_get_int(u, "cacheWrite", 0) +
                      json_get_int(u, "output", 0);
        if (ctx > 0) {
            a->last_context_tokens = ctx;
            break;
        }
    }
    pthread_mutex_unlock(&a->agent->mu);
    out->moved = true;
    return 0;
}

int app_clear_context(app_t *a) {
    if (agent_is_running(a->agent)) return -1;
    if (a->session) session_append_reset_boundary(a->session);
    agent_replace_messages(a->agent, json_arr());
    a->last_context_tokens = 0;
    return 0;
}

int app_retry(app_t *a, char **err) {
    pthread_mutex_lock(&a->agent->mu);
    json_t *msgs = a->agent->messages;
    size_t n = json_len(msgs);
    json_t *last = n ? json_at(msgs, n - 1) : NULL;
    bool failed = last && str_eq(json_get_str(last, "role"), "assistant") &&
                  (str_eq(json_get_str(last, "stopReason"), "error") || str_eq(json_get_str(last, "stopReason"), "aborted"));
    if (failed) json_free(json_arr_remove(msgs, n - 1));
    pthread_mutex_unlock(&a->agent->mu);
    if (!failed) {
        *err = xstrdup("Nothing to retry: the last turn did not fail.");
        return -1;
    }
    a->retry_count = 0;
    return agent_continue(a->agent);
}

/* session-maintenance.ts resolveCompactionModelCandidates: the model's compactionModel target, the
 * session model, each configured chat role, then the largest-context available model */
void app_compaction_candidates(app_t *a, mr_pool_t *out) {
    mr_pool_t avail = {0};
    mr_pool_available(&avail, "chat");
    const ai_model_t *pm = a->model;
#define ADD(M_)                                                                                          \
    do {                                                                                                 \
        const ai_model_t *m_ = (M_);                                                                     \
        if (m_ && !mr_pool_contains(out, m_)) mr_pool_push(out, m_);                                     \
    } while (0)
    if (pm) {
        const char *target = json_get_str(pm->raw, "compactionModel");
        if (target && *target) {
            char *base;
            mr_split_level(target, -1, true, &base);
            const ai_model_t *t = NULL;
            for (size_t i = 0; i < avail.n && !t; i++) {
                char *k = mr_model_key(avail.v[i]);
                if (!strcmp(k, base)) t = avail.v[i];
                free(k);
            }
            for (size_t i = 0; i < avail.n && !t; i++)
                if (!strcmp(avail.v[i]->provider, pm->provider) && !strcmp(avail.v[i]->id, target)) t = avail.v[i];
            free(base);
            ADD(t);
        }
        ADD(pm);
    }
    for (size_t r = 0; r < MR_CHAT_ROLE_COUNT; r++) {
        const char *v = mr_role_get(MR_ROLE_IDS[r]);
        char *dflt = NULL;
        if (!v && !strcmp(MR_ROLE_IDS[r], "default") && pm) v = dflt = mr_model_key(pm);
        mr_result_t res = {0};
        if (v && mr_resolve_value(v, &avail, &res)) ADD(res.model);
        mr_result_clear(&res);
        free(dflt);
    }
    const ai_model_t *big = NULL;
    for (size_t i = 0; i < avail.n; i++)
        if (!mr_pool_contains(out, avail.v[i]) && (!big || avail.v[i]->context_window > big->context_window)) big = avail.v[i];
    ADD(big);
#undef ADD
    mr_pool_free(&avail);
}

/* ---------- title ---------- */

char *app_generate_title_from(app_t *a, const char *text) { return app_generate_title_with(a, text, NULL); }

char *app_generate_title_with(app_t *a, const char *text, const char *system_prompt) {
    if (!text || !*text) return NULL;
    char *sys = system_prompt ? xstrdup(system_prompt)
                              : prompt_load_or("coding-agent/prompts/system/title-system.md",
                                               "Generate a concise 3-6 word title for this conversation. Output only the title.");
    /* title-generator.ts getTitleModels: tiny/commit/smol role primaries + their retry chains, then
     * the session model and its own chain */
    static const char *const roles[] = {"tiny", "commit", "smol"};
    mr_pool_t models = {0};
    mr_online_tiny_candidates(roles, ARRAY_LEN(roles), a->model, &models);
    char *snippet = xasprintf("%.2000s", text);
    char *title = NULL;
    for (size_t i = 0; i < models.n && !title && !a->agent->cancel; i++)
        title = app_complete_text(a, models.v[i], sys, snippet, EFFORT_OFF);
    if (!models.n && a->model) title = app_complete_text(a, a->model, sys, snippet, EFFORT_OFF);
    mr_pool_free(&models);
    free(snippet);
    free(sys);
    return title;
}

char *app_generate_title(app_t *a) {
    const char *first = NULL;
    json_t *m;
    json_arr_foreach(a->agent->messages, i, m) {
        if (!first && str_eq(json_get_str(m, "role"), "user") && !json_get_bool(m, "synthetic", false)) {
            json_t *c = json_get(m, "content");
            if (json_is_str(c)) first = json_sv(c);
            else first = json_get_str(json_at(c, 0), "text");
        }
    }
    if (!first || title_low_signal(first)) return NULL;
    char *raw = app_generate_title_from(a, first);
    char *t = raw ? title_normalize(raw) : NULL;
    free(raw);
    return t;
}

static void maybe_title(app_t *a) {
    /* the side request started when the reply began; give it a moment, then apply */
    title_wait(a, 3000);
    title_apply(a);
}

/* omp's context-maintenance trigger: thresholdTokens (>0) overrides thresholdPercent (>0); both -1 =
 * legacy reserve-based budget (window - max(15% of window, reserveTokens)) */
int64_t app_compaction_threshold(const ai_model_t *m, int64_t window) { return cp_model_threshold(m, window); }

/* post-turn context maintenance (pruning + the compaction.methodOrder ladder; coding/compaction.c) */
static void maybe_auto_compact(app_t *a) { cmp_after_turn(a); }

json_t *app_compaction_result(app_t *a) {
    json_t *path = session_path_to(a->session, a->session->leaf_id), *e, *last = NULL;
    json_arr_foreach(path, i, e) if (str_eq(json_get_str(e, "type"), "compaction")) last = e;
    json_t *r = json_obj();
    if (last) {
        json_set_str(r, "summary", json_get_str(last, "summary") ? json_get_str(last, "summary") : "");
        if (json_get_str(last, "shortSummary")) json_set_str(r, "shortSummary", json_get_str(last, "shortSummary"));
        json_set_str(r, "firstKeptEntryId", json_get_str(last, "firstKeptEntryId") ? json_get_str(last, "firstKeptEntryId") : "");
        json_set_int(r, "tokensBefore", json_get_int(last, "tokensBefore", 0));
        if (json_get(last, "details")) json_set(r, "details", json_clone(json_get(last, "details")));
        if (json_get(last, "preserveData")) json_set(r, "preserveData", json_clone(json_get(last, "preserveData")));
    }
    json_free(path);
    return r;
}

/* ---------- prompting ---------- */

/* omp `@path` mentions: inline existing files (bounded) after the prompt text */
static void add_mentions(app_t *a, const char *text, json_t *content) {
    size_t budget = 256u * 1024;
    strvec_t seen = {0};
    for (const char *p = text; (p = strchr(p, '@')); p++) {
        if (p > text && !isspace((unsigned char)p[-1]) && p[-1] != '(' && p[-1] != '"') continue;
        size_t n = 0;
        while (p[1 + n] && !isspace((unsigned char)p[1 + n]) && !strchr("\"'`),;", p[1 + n])) n++;
        while (n && strchr(".:!?", p[n])) n--; /* trailing punctuation */
        if (!n) continue;
        char *rel = xstrndup(p + 1, n);
        char *abs = path_abs(rel, a->cwd);
        if (fs_is_file(abs) && !strvec_contains(&seen, abs)) {
            size_t len;
            bool trunc = false;
            char *data = fs_read_limit(abs, MIN(budget, (size_t)(64u * 1024)), &len, &trunc);
            if (data && !fs_is_binary(data, len)) {
                char *t = xasprintf("<file path=\"%s\">\n%s%s\n</file>", rel, data, trunc ? "\n[…truncated]" : "");
                json_push(content, text_block(t));
                free(t);
                budget = budget > len ? budget - len : 0;
                strvec_push(&seen, abs);
            }
            free(data);
        }
        free(abs);
        free(rel);
        if (!budget) break;
    }
    strvec_free(&seen);
}

/* `+Nk` turn budget directive (modes/turn-budget.ts): a standalone `+<number>[k|m][!]` token */
static void begin_turn_budget(app_t *a, const char *text) {
    int64_t total = 0;
    bool hard = false;
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p != '+' || (p > text && !isspace((unsigned char)p[-1]))) continue;
        const char *q = p + 1;
        if (!isdigit((unsigned char)*q)) continue;
        char *end;
        double v = strtod(q, &end);
        if (end == q || (end[-1] == '.')) continue;
        for (const char *d = q; d < end; d++)
            if (!isdigit((unsigned char)*d) && *d != '.') v = -1;
        if (v < 0) continue;
        q = end;
        double mul = 1;
        if (*q == 'k' || *q == 'K') mul = 1e3, q++;
        else if (*q == 'm' || *q == 'M') mul = 1e6, q++;
        bool h = *q == '!';
        if (h) q++;
        if (*q && !isspace((unsigned char)*q)) continue;
        if (v > 0 && isfinite(v * mul)) {
            total = (int64_t)llround(v * mul);
            hard = h;
        }
        break;
    }
    a->turn_budget_total = total;
    a->turn_budget_hard = hard;
    a->turn_output_baseline = __atomic_load_n(&a->output_tokens, __ATOMIC_SEQ_CST);
}

/* run the agent, then the settle passes in omp's agent_end order: empty/unexpected-stop retries, the plan-mode
 * decision reminder, the stop-time todo reminder; each may append a hidden message and continue the agent */
static int run_guarded_inner(app_t *a, json_t *msgs);

/* run state idle: idle cache warming takes over (or streaming warming stops), paid warms are recorded */
static int run_guarded(app_t *a, json_t *msgs) {
    a->stall_continues = 0; /* TurnRecovery.resetForNewPrompt */
    int rc = run_guarded_inner(a, msgs);
    cachewarm_settled(a->warmer);
    app_flush_warm_usage(a);
    return rc;
}

static int run_guarded_inner(app_t *a, json_t *msgs) {
    int rc = agent_prompt(a->agent, msgs);
    while (rc == 0 && !a->agent->cancel) {
        int rec = guards_stop_recovery(a);
        if (rec == 1) {
            rc = agent_continue(a->agent);
            continue;
        }
        if (rec == 2) break; /* a capped empty stop is terminal: no reminder restarts it */
        size_t before = json_len(a->agent->messages);
        rc = app_plan_settle(a, rc);
        if (rc != 0 || json_len(a->agent->messages) != before) continue; /* the plan reminder ran a turn */
        if (!guards_todo_reminder(a)) break;
        rc = agent_continue(a->agent);
    }
    return rc;
}

/* applyAutoThinkingLevel: classify this user turn; false when an abort superseded the prompt meanwhile */
/* #resolveAutoThinkingLevel: publish `e` as the turn's auto resolution; the transcript records only a changed level */
static void resolve_auto_thinking(app_t *a, ai_effort_t e) {
    bool changed = a->thinking != e;
    a->auto_resolved = (int)e;
    a->thinking = e;
    a->agent->thinking = e;
    if (changed) session_append_thinking_change_ex(a->session, effort_name(e), "auto");
    if (a->session_event) { /* applyAutoThinkingLevel: thinking_level_changed {configured: auto, resolved} */
        json_t *ev = json_obj();
        json_set_str(ev, "type", "thinking_level_changed");
        if (app_wire_thinking(a)) {
            json_set_str(ev, "thinkingLevel", app_wire_thinking(a));
            json_set_str(ev, "configured", "auto");
            json_set_str(ev, "resolved", app_wire_thinking(a));
        } else json_set_str(ev, "configured", "auto");
        app_emit(a, ev);
    }
}

/* applyChillax: drop effort to the model's lowest supported level from the next request on. Only models that take per-message
 * effort qualify (they switch effort between steps without rewriting the cached prefix, so a steer relaxes the running turn). In
 * auto mode the floor is this turn's resolution; a pinned level is re-pinned lower, never raised, and off stays off. Whether the
 * keyword applied, so auto classification must not run for the turn. */
static bool apply_chillax(app_t *a, const char *text) {
    if (!text || !keywords_enabled("chillax") || !keywords_contains(text, "chillax")) return false;
    const ai_model_t *m = a->model;
    ai_effort_t sup[6];
    if (!m || !m->reasoning || json_get_bool(m->thinking, "unverified", false) || !model_compat_bool(m, "supportsPerMessageEffort", false) ||
        !autothink_supported(m, sup))
        return false;
    ai_effort_t floor_e = sup[0];
    if (a->auto_thinking) {
        resolve_auto_thinking(a, floor_e);
        return true;
    }
    if (a->thinking != EFFORT_OFF && floor_e < a->thinking) app_set_thinking(a, floor_e);
    return true;
}

static bool apply_auto_thinking(app_t *a, const char *text) {
    /* `chillax` settles the turn's effort itself, so classification is skipped */
    if (apply_chillax(a, text)) return true;
    if (!a->auto_thinking) return true;
    int gen = __atomic_load_n(&a->abort_gen, __ATOMIC_SEQ_CST);
    ai_effort_t e;
    bool ok = autothink_resolve(a, text, &e);
    if (__atomic_load_n(&a->abort_gen, __ATOMIC_SEQ_CST) != gen) return false;
    if (!ok || !a->auto_thinking) return true;
    resolve_auto_thinking(a, e);
    return true;
}

/* renderAttachmentSourceNotice: the hidden custom message naming the file behind `[Image #index]` */
static json_t *attachment_notice(int index, const char *path) {
    json_t *c = json_obj();
    char idx[16];
    snprintf(idx, sizeof idx, "%d", index);
    json_set_str(c, "index", idx);
    json_set_str(c, "path", path);
    char *r = prompt_render("coding-agent/prompts/system/image-attachment.md", c);
    json_free(c);
    char *t = str_trim(r ? r : "");
    free(r);
    json_t *m = msg_developer_text(t); /* custom messages reach the model as developer turns (convertMessageToLlm) */
    free(t);
    json_set_bool(m, "synthetic", true);
    json_set_str(m, "customType", "image-attachment");
    json_set_str(m, "attribution", "user");
    json_t *d = json_obj();
    json_set_int(d, "index", index);
    json_set_str(d, "path", path);
    json_set(m, "details", d);
    return m;
}

int app_prompt_attachments(app_t *a, const char *text, json_t *attachments) {
    json_t *images = json_arr(), *notices = json_arr(), *e;
    json_arr_foreach(attachments, i, e) {
        json_t *img = json_get(e, "image");
        if (!img) continue;
        json_push(images, json_clone(img));
        const char *path = json_get_str(e, "path");
        if (path && *path) json_push(notices, attachment_notice((int)json_len(images), path));
    }
    json_free(attachments);
    if (!json_len(images)) {
        json_free(images);
        images = NULL;
    }
    return app_prompt_ex(a, text, images, notices);
}

int app_prompt(app_t *a, const char *text, json_t *images) { return app_prompt_ex(a, text, images, NULL); }

void app_steer_attachments(app_t *a, const char *text, json_t *attachments) {
    /* prependNotices: magic keywords ride ahead of a queued message as they do a direct prompt */
    {
        json_t *kn = keywords_notices(a, text), *k;
        json_arr_foreach(kn, ki, k) agent_steer(a->agent, json_clone(k));
        json_free(kn);
    }
    json_t *images = json_arr(), *e;
    json_arr_foreach(attachments, i, e) {
        json_t *img = json_get(e, "image");
        if (!img) continue;
        json_push(images, json_clone(img));
        const char *path = json_get_str(e, "path");
        if (path && *path) agent_steer(a->agent, attachment_notice((int)json_len(images), path));
    }
    json_free(attachments);
    json_t *content = json_arr();
    json_push(content, text_block(text));
    json_t *norm = ai_normalize_images(images, a->model), *img;
    json_arr_foreach(norm, i, img) json_push(content, json_clone(img));
    json_free(norm);
    json_free(images);
    agent_steer(a->agent, msg_user_content(content));
}

int app_prompt_ex(app_t *a, const char *text, json_t *images, json_t *notices) {
    app_plan_note_user_turn(a);
    app_plan_yolo_arm(a);
    memory_take_prompt_refresh(a);
    maybe_restore_primary(a);
    if (!credential_preflight(a)) {
        json_free(images);
        json_free(notices);
        return APP_PROMPT_REFUSED;
    }
    begin_turn_budget(a, text);
    guards_reset_cycle(a);
    json_t *content = json_arr();
    json_push(content, text_block(text));
    add_mentions(a, text, content);
    /* normalizeModelContextImages: every attached image is resized / re-encoded for the active model */
    json_t *norm = ai_normalize_images(images, a->model), *img;
    json_arr_foreach(norm, i, img) json_push(content, json_clone(img));
    json_free(norm);
    json_free(images);
    json_t *msgs = json_arr();
    json_t *pr = app_plan_reference_message(a);
    if (pr) json_push(msgs, pr);
    json_t *gm = app_goal_context_message(a);
    if (gm) json_push(msgs, gm);
    /* magic keywords: hidden user-attributed notices ride ahead of the prompt (and its attachment notices) */
    json_t *kn = keywords_notices(a, text), *k;
    json_arr_foreach(kn, i, k) json_push(msgs, json_clone(k));
    json_free(kn);
    json_arr_foreach(notices, i, img) json_push(msgs, json_clone(img));
    json_free(notices);
    a->think_force = a->forced_step == 0; /* the turn's first request (a pending /force directive wins) */
    /* the pending xd:// mount delta rides with the next user prompt, right before it */
    json_t *xn = xdnotice_take(a);
    if (xn) json_push(msgs, xn);
    json_push(msgs, msg_user_typed(content));
    if (!apply_auto_thinking(a, text)) {
        json_free(msgs);
        return 0;
    }
    cmp_before_prompt(a, msgs); /* the pending prompt may push the context over the trigger */
    int rc = run_guarded(a, msgs);
    if (rc == 0) {
        maybe_title(a);
        maybe_auto_compact(a);
    }
    return rc;
}

/* ---------- non-builtin slash commands ---------- */

/* the MCP prompt command (buildMCPPromptCommands entry, owned) named by "/<name> ..." */
static json_t *mcp_command_for(app_t *a, const char *text, char **args) {
    *args = NULL;
    if (!a->mcp || !text || text[0] != '/') return NULL;
    char *name;
    if (!commands_parse_invocation(text, &name, args)) return NULL;
    json_t *all = mcp_prompt_commands(a->mcp), *c, *hit = NULL;
    json_arr_foreach(all, i, c) if (!hit && str_eq(json_get_str(c, "name"), name)) hit = json_clone(c);
    json_free(all);
    free(name);
    if (!hit) {
        free(*args);
        *args = NULL;
    }
    return hit;
}

bool app_is_mcp_prompt_command(app_t *a, const char *text) {
    char *args;
    json_t *c = mcp_command_for(a, text, &args);
    free(args);
    json_free(c);
    return c != NULL;
}

char *app_expand_input(app_t *a, const char *text, bool mcp) {
    char *cur = xstrdup(text ? text : "");
    if (cur[0] == '/' && mcp) {
        /* #tryExecuteCustomCommand: `key=value` args become the prompt's arguments; a failure or empty
         * result consumes the input */
        char *args;
        json_t *c = mcp_command_for(a, cur, &args);
        if (c) {
            strvec_t av = commands_parse_args(args);
            json_t *pa = json_obj();
            for (size_t i = 0; i < av.n; i++) {
                const char *eq = strchr(av.v[i], '=');
                if (!eq || eq == av.v[i]) continue;
                char *k = xstrndup(av.v[i], (size_t)(eq - av.v[i]));
                json_set_str(pa, k, eq + 1);
                free(k);
            }
            strvec_free(&av);
            char *err = NULL;
            char *r = mcp_run_prompt(a->mcp, json_get_str(c, "server"), json_get_str(c, "prompt"), pa, &err);
            if (err) notice(a, "error", "Extension \"custom-command:%s\" error: %s", json_get_str(c, "name"), err);
            free(err);
            free(args);
            json_free(c);
            free(cur);
            if (!r || !*r) {
                free(r);
                return NULL;
            }
            cur = r;
        }
    }
    if (cur[0] == '/') {
        char *r = commands_expand(&a->file_cmds, cur);
        if (r) {
            free(cur);
            cur = r;
        }
    }
    char *t = prompt_templates_expand(&a->prompt_tpls, cur);
    if (t) {
        free(cur);
        cur = t;
    }
    return cur;
}

char *app_expand_templates(app_t *a, const char *text) {
    char *t = prompt_templates_expand(&a->prompt_tpls, text);
    return t ? t : xstrdup(text ? text : "");
}

const skill_t *app_skill_command(app_t *a, const char *text, skill_invocation_t *inv) {
    memset(inv, 0, sizeof *inv);
    if (!settings_bool("skills.enableSkillCommands", true)) return NULL;
    if (!skill_parse_invocation(text, inv)) return NULL;
    const skill_t *s = discovery_find_skill(&a->disc, inv->name);
    if (!s) skill_invocation_free(inv);
    return s;
}

json_t *app_skill_message(app_t *a, const char *text, json_t *images, char **err) {
    *err = NULL;
    skill_invocation_t inv;
    const skill_t *s = app_skill_command(a, text, &inv);
    if (!s) {
        json_free(images);
        return NULL;
    }
    char *message;
    json_t *details;
    if (skill_build_user_prompt(s, &inv, &message, &details, err) != 0) {
        skill_invocation_free(&inv);
        json_free(images);
        return NULL;
    }
    json_t *content = json_arr();
    json_push(content, text_block(message));
    json_t *norm = ai_normalize_images(images, a->model), *img;
    json_arr_foreach(norm, i, img) json_push(content, json_clone(img));
    json_free(norm);
    json_free(images);
    json_t *m = msg_user_content(content);
    json_set_str(m, "customType", "skill-prompt");
    json_set_bool(m, "display", true);
    json_set(m, "details", details);
    json_set_str(m, "attribution", "user");
    free(message);
    skill_invocation_free(&inv);
    return m;
}

int app_prompt_skill(app_t *a, json_t *msg) {
    app_plan_note_user_turn(a);
    app_plan_yolo_arm(a);
    memory_take_prompt_refresh(a);
    maybe_restore_primary(a);
    if (!credential_preflight(a)) {
        json_free(msg);
        return APP_PROMPT_REFUSED;
    }
    guards_reset_cycle(a);
    char *text = ai_message_text(msg);
    const char *args = json_get_str(json_get(msg, "details"), "args");
    json_t *msgs = json_arr();
    json_t *pr = app_plan_reference_message(a);
    if (pr) json_push(msgs, pr);
    json_t *gm = app_goal_context_message(a);
    if (gm) json_push(msgs, gm);
    /* magic keywords read the skill's user args (#dispatchCustomPrompt) */
    json_t *kn = keywords_notices(a, args ? args : ""), *k;
    json_arr_foreach(kn, i, k) json_push(msgs, json_clone(k));
    json_free(kn);
    json_push(msgs, msg);
    if (!apply_auto_thinking(a, text)) {
        free(text);
        json_free(msgs);
        return 0;
    }
    free(text);
    int rc = run_guarded(a, msgs);
    if (rc == 0) {
        maybe_title(a);
        maybe_auto_compact(a);
    }
    return rc;
}

int app_prompt_hidden(app_t *a, const char *text, const char *custom_type) {
    maybe_restore_primary(a);
    if (!credential_preflight(a)) return APP_PROMPT_REFUSED;
    json_t *msgs = json_arr();
    json_t *gm = app_goal_context_message(a);
    if (gm) json_push(msgs, gm);
    json_t *m = msg_user_text(text);
    json_set_bool(m, "synthetic", true);
    json_set_str(m, "customType", custom_type);
    if (str_eq(custom_type, "async-result") && a->job_batch) {
        json_set(m, "details", a->job_batch);
        a->job_batch = NULL;
    }
    json_push(msgs, m);
    guards_reset_cycle(a);
    int rc = run_guarded(a, msgs);
    if (rc == 0) maybe_auto_compact(a);
    return rc;
}

/* isCutOffAssistant: an assistant turn cut off before any text or tool call; no provider replays it */
static bool is_cut_off_assistant(const json_t *m) {
    if (!str_eq(json_get_str(m, "role"), "assistant")) return false;
    const char *stop = json_get_str(m, "stopReason");
    if (!str_eq(stop, "error") && !str_eq(stop, "aborted") && !str_eq(stop, "length")) return false;
    const json_t *blk;
    json_arr_foreach(json_get(m, "content"), j, blk) {
        const char *bt = json_get_str(blk, "type");
        if (str_eq(bt, "text") || str_eq(bt, "toolCall")) return false;
    }
    return true;
}

/* conversationEnd + the cut-off tail + unansweredPrompt (turn-recovery.ts). Sets the tail range; caller holds the lock. */
static bool unanswered_prompt_locked(const json_t *msgs, size_t *tail_out, size_t *end_out) {
    /* conversationEnd: trailing custom messages the user did not author */
    size_t end = json_len(msgs);
    while (end > 0) {
        const json_t *m = json_at(msgs, end - 1);
        if (!str_eq(json_get_str(m, "role"), "custom") || str_eq(json_get_str(m, "attribution"), "user")) break;
        end--;
    }
    size_t tail = end;
    while (tail > 0 && is_cut_off_assistant(json_at(msgs, tail - 1))) tail--;
    *tail_out = tail;
    *end_out = end;
    for (size_t i = tail; i-- > 0;) {
        const json_t *m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            if (!is_cut_off_assistant(m)) return false;
        } else if (json_get_bool(m, "sessionSummary", false)) {
            continue; /* a handoff or compaction can leave only the summary: the directive resumes it */
        } else if (str_eq(role, "user") || (str_eq(role, "custom") && str_eq(json_get_str(m, "attribution"), "user") && json_get_bool(m, "display", true)) ||
                   (str_eq(role, "developer") && json_get_bool(m, "userInitiated", false))) {
            return true;
        }
    }
    return false;
}

bool app_has_unanswered_prompt(app_t *a) {
    size_t tail, end;
    pthread_mutex_lock(&a->agent->mu);
    bool r = unanswered_prompt_locked(a->agent->messages, &tail, &end);
    pthread_mutex_unlock(&a->agent->mu);
    return r;
}

/* AgentSession.resume, the `.`/`c` continue shortcut. A prompt the model has not answered yet runs again as is;
 * otherwise the hidden manual-continue directive asks the model to pick its work back up. omc's summaries and
 * user-invoked /skill: prompts are `user` messages, so has_user below covers omp's resumable check. */
int app_resume(app_t *a, char **err) {
    maybe_restore_primary(a);
    pthread_mutex_lock(&a->agent->mu);
    json_t *msgs = a->agent->messages;
    size_t tail, end;
    bool unanswered = unanswered_prompt_locked(msgs, &tail, &end);
    if (unanswered)
        for (size_t i = end; i-- > tail;) json_free(json_arr_remove(msgs, i));
    bool has_user = false;
    const json_t *m;
    json_arr_foreach(msgs, i, m) if (str_eq(json_get_str(m, "role"), "user")) has_user = true;
    pthread_mutex_unlock(&a->agent->mu);
    guards_reset_cycle(a);
    if (unanswered) {
        a->retry_count = 0;
        return agent_continue(a->agent);
    }
    if (!has_user) {
        *err = xstrdup("Nothing to resume");
        return -1;
    }
    char *text = prompt_load("coding-agent/prompts/system/manual-continue.md");
    json_t *dev = msg_developer_text(text ? text : "");
    free(text);
    json_set_str(dev, "attribution", "agent");
    json_set_bool(dev, "synthetic", true);
    json_set_bool(dev, "userInitiated", true);
    json_t *run = json_arr();
    json_push(run, dev);
    int rc = run_guarded(a, run);
    if (rc == 0) maybe_auto_compact(a);
    return rc;
}

json_t *app_user_bash_run(app_t *a, const char *cmd, bool exclude, proc_output_cb on_output, void *ud,
                          volatile sig_atomic_t *cancel) {
    proc_opts_t o = {.cwd = a->cwd, .timeout_ms = 300000, .merge_stderr = true, .new_session = true,
                     .cancel = cancel, .on_output = on_output, .ud = ud};
    proc_result_t r;
    if (!cmd || proc_shell(cmd, &o, &r) != 0) return NULL;
    /* omp executeBash without an artifact: sanitized, column-capped, head/tail elided capture */
    out_sink_opts_t so;
    out_sink_opts_from_settings(&so);
    out_sink_t *sink = out_sink_new(&so);
    out_sink_push(sink, r.out ? r.out : "", r.out_len);
    out_summary_t sum;
    bool cancelled = r.cancelled || r.timed_out;
    out_sink_dump(sink, r.timed_out ? "Command timed out after 300 seconds" : r.cancelled ? "Command cancelled" : NULL, &sum);
    out_sink_free(sink);
    int code = r.term_signal ? 128 + r.term_signal : r.exit_code;
    json_t *m = json_obj();
    json_set_str(m, "role", "bashExecution");
    json_set_str(m, "command", cmd);
    json_set_str(m, "output", sum.output);
    if (!cancelled) json_set_int(m, "exitCode", code);
    json_set_bool(m, "cancelled", cancelled);
    json_set_bool(m, "truncated", sum.truncated);
    json_t *meta = NULL;
    out_meta_from_summary(&meta, &sum, "tail", 1, 0);
    if (meta) json_set(m, "meta", meta);
    if (exclude) json_set_bool(m, "excludeFromContext", true);
    json_set_int(m, "timestamp", now_ms());
    /* BashResult counters for RPC callers (dropped when recorded) */
    json_set_int(m, "totalLines", sum.total_lines);
    json_set_int(m, "totalBytes", sum.total_bytes);
    json_set_int(m, "outputLines", sum.output_lines);
    json_set_int(m, "outputBytes", sum.output_bytes);
    if (sum.elided_bytes >= 0) json_set_int(m, "elidedBytes", sum.elided_bytes);
    if (sum.elided_lines >= 0) json_set_int(m, "elidedLines", sum.elided_lines);
    if (sum.column_dropped_bytes > 0) json_set_int(m, "columnDroppedBytes", sum.column_dropped_bytes);
    if (sum.column_truncated_lines > 0) json_set_int(m, "columnTruncatedLines", sum.column_truncated_lines);
    if (sum.column_first > 0) {
        json_t *range = json_obj();
        json_set_int(range, "first", sum.column_first);
        json_set_int(range, "last", sum.column_last);
        json_set(m, "columnTruncatedRange", range);
    }
    if (sum.column_max > 0) json_set_int(m, "columnMax", sum.column_max);
    out_summary_free(&sum);
    proc_result_free(&r);
    return m;
}

void app_record_bash(app_t *a, const json_t *result) {
    json_t *m = json_clone(result);
    static const char *const counters[] = {"totalLines", "totalBytes", "outputLines", "outputBytes", "elidedBytes", "elidedLines",
                                           "columnDroppedBytes", "columnTruncatedLines", "columnTruncatedRange", "columnMax"};
    for (size_t i = 0; i < ARRAY_LEN(counters); i++) json_del(m, counters[i]);
    if (a->session) session_append_message(a->session, m);
    if (!json_get_bool(m, "excludeFromContext", false)) {
        char *t = out_bash_execution_text(m);
        agent_append_message(a->agent, msg_user_text(t));
        free(t);
    }
    json_free(m);
}

json_t *app_user_python_run(app_t *a, const char *code, bool exclude, volatile sig_atomic_t *cancel) {
    agent_tool_t *t = agent_find_tool(a->agent, "eval");
    if (!t || !t->execute) return NULL;
    /* the eval tool's shared kernel (omp EvalRunner.executePython uses the session's eval executor) */
    json_t *args = json_obj();
    json_set_str(args, "language", "py");
    json_set_str(args, "code", code);
    json_set_int(args, "timeout", 0);
    tool_ctx_t ctx = {.agent = a->agent, .tool_name = "eval", .cancel = cancel, .app = a};
    tool_result_t r = {0};
    t->execute(t, &ctx, args, &r);
    json_free(args);
    const json_t *cell = json_at(json_get_arr(r.details, "cells"), 0);
    bool cancelled = cancel && *cancel;
    json_t *m = json_obj();
    json_set_str(m, "role", "pythonExecution");
    json_set_str(m, "code", code);
    if (cell) json_set_str(m, "output", json_get_str(cell, "output") ? json_get_str(cell, "output") : "");
    else {
        char *txt = tool_result_joined_text(&r);
        json_set_str(m, "output", txt);
        free(txt);
    }
    if (!cancelled) json_set_int(m, "exitCode", r.is_error ? 1 : 0);
    json_set_bool(m, "cancelled", cancelled);
    json_set_bool(m, "truncated", false);
    const json_t *meta = json_get_obj(r.details, "meta");
    if (meta) json_set(m, "meta", json_clone(meta));
    if (exclude) json_set_bool(m, "excludeFromContext", true);
    json_set_int(m, "timestamp", now_ms());
    tool_result_free(&r);
    return m;
}

void app_record_python(app_t *a, const json_t *result) {
    if (a->session) session_append_message(a->session, result);
    if (!json_get_bool(result, "excludeFromContext", false)) {
        char *t = out_python_execution_text(result);
        agent_append_message(a->agent, msg_user_text(t));
        free(t);
    }
}

json_t *app_user_bash(app_t *a, const char *cmd, bool exclude) {
    json_t *m = app_user_bash_run(a, cmd, exclude, NULL, NULL, NULL);
    if (m) app_record_bash(a, m);
    return m;
}

int app_new_session(app_t *a) {
    session_t *ns = a->opts.no_session ? session_in_memory(a->cwd) : session_create(a->cwd, a->opts.session_dir);
    session_free(a->session);
    a->session = ns;
    app_bind_artifacts(a, NULL);
    app_bind_local(a);
    /* newSession: plan reference back to the default, unsent */
    a->plan_reference_sent = false;
    free(a->plan_reference_path);
    a->plan_reference_path = NULL;
    agent_replace_messages(a->agent, json_arr());
    xdnotice_reset_announced(a);
    free(a->agent->session_id);
    a->agent->session_id = xstrdup(session_id(ns));
    char *label = app_model_label(a);
    session_append_model_change(ns, label, NULL);
    session_append_thinking_change(ns, effort_name(a->thinking));
    app_append_service_tier_entry(a);
    free(label);
    a->title_done = false;
    a->total_cost = 0;
    costgate_reseed(a);
    a->last_context_tokens = 0;
    json_free(a->env->todo.phases);
    a->env->todo.phases = json_arr();
    app_goal_restore(a);
    prewalk_new_session(a);
    return 0;
}

int app_switch_session(app_t *a, const char *path, char **err) {
    session_t *s = session_open(path, err);
    if (!s) return -1;
    session_free(a->session);
    a->session = s;
    app_bind_artifacts(a, NULL);
    app_bind_local(a);
    free(a->agent->session_id);
    a->agent->session_id = xstrdup(session_id(s));
    load_session_into_agent(a);
    a->title_done = s->title != NULL;
    costgate_reseed(a);
    app_goal_restore(a);
    return 0;
}

int app_fork_from(app_t *a, const char *entry_id, char **err) {
    session_t *ns = session_fork(a->session, entry_id, a->opts.session_dir);
    if (!ns) {
        *err = xstrdup("fork failed");
        return -1;
    }
    char *old_dir = a->env->artifacts && artifacts_dir(a->env->artifacts) ? xstrdup(artifacts_dir(a->env->artifacts)) : NULL;
    session_free(a->session);
    a->session = ns;
    app_bind_artifacts(a, old_dir);
    free(old_dir);
    load_session_into_agent(a);
    return 0;
}

/* AgentSession.branchFromBtw: a new session file forked at `leaf_id` holding the /btw question and its answer */
int app_branch_from_btw(app_t *a, const char *question, const json_t *images, const json_t *assistant,
                        const char *leaf_id, const char *session_id_, char **err) {
    if (!a->session || !a->session->file || !a->session->persist) {
        *err = xstrdup("session is not persisted");
        return -1;
    }
    if (!leaf_id || !str_eq(session_id(a->session), session_id_) || !str_eq(a->session->leaf_id, leaf_id)) {
        *err = xstrdup("session changed since /btw started");
        return -1;
    }
    if (agent_is_running(a->agent)) {
        *err = xstrdup("Cannot branch /btw while the agent is running");
        return -1;
    }
    agent_clear_queues(a->agent);
    session_t *ns = session_fork(a->session, leaf_id, a->opts.session_dir);
    if (!ns) {
        *err = xstrdup("fork failed");
        return -1;
    }
    char *old_dir = a->env->artifacts && artifacts_dir(a->env->artifacts) ? xstrdup(artifacts_dir(a->env->artifacts)) : NULL;
    session_free(a->session);
    a->session = ns;
    app_bind_artifacts(a, old_dir);
    free(old_dir);
    json_t *content = json_arr(), *img;
    json_push(content, text_block(question));
    json_arr_foreach(images, i, img) json_push(content, json_clone(img));
    json_t *um = msg_user_content(content);
    json_set_str(um, "attribution", "user");
    session_append_message(a->session, um);
    json_free(um);
    /* sanitizeAssistantForReparentedHistory: replay-bound signatures and provider payloads go */
    json_t *am = json_clone(assistant), *blocks = json_arr(), *blk;
    json_arr_foreach(json_get(assistant, "content"), i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "redactedThinking") || str_eq(t, "anthropicServerTool")) continue;
        if (str_eq(t, "thinking")) {
            json_t *b = json_obj();
            json_set_str(b, "type", "thinking");
            json_set_str(b, "thinking", json_get_str(blk, "thinking") ? json_get_str(blk, "thinking") : "");
            if (json_get(blk, "summary")) json_set(b, "summary", json_clone(json_get(blk, "summary")));
            if (json_get_bool(blk, "narration", false)) json_set_bool(b, "narration", true);
            json_push(blocks, b);
            continue;
        }
        json_push(blocks, json_clone(blk));
    }
    json_set(am, "content", blocks);
    json_del(am, "providerPayload");
    session_append_message(a->session, am);
    json_t *u = json_get(am, "usage");
    int64_t ctx = json_get_int(u, "input", 0) + json_get_int(u, "cacheRead", 0) + json_get_int(u, "cacheWrite", 0) +
                  json_get_int(u, "output", 0);
    a->last_context_tokens = ctx > 0 ? ctx : 0; /* the branched answer's usage is the context gauge */
    json_free(am);
    load_session_into_agent(a);
    advisor_reset(a->advisor);
    return 0;
}

json_t *app_stats(app_t *a) {
    json_t *o = json_obj();
    int64_t in = 0, out = 0, cr = 0, cw = 0;
    int user = 0, asst = 0, tools = 0;
    json_t *m;
    json_arr_foreach(a->agent->messages, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "user")) user++;
        else if (str_eq(role, "toolResult")) tools++;
        else if (str_eq(role, "assistant")) {
            asst++;
            json_t *u = json_get(m, "usage");
            in += json_get_int(u, "input", 0);
            out += json_get_int(u, "output", 0);
            cr += json_get_int(u, "cacheRead", 0);
            cw += json_get_int(u, "cacheWrite", 0);
        }
    }
    json_set_int(o, "userMessages", user);
    json_set_int(o, "assistantMessages", asst);
    json_set_int(o, "toolResults", tools);
    json_set_int(o, "input", in);
    json_set_int(o, "output", out);
    json_set_int(o, "cacheRead", cr);
    json_set_int(o, "cacheWrite", cw);
    json_set_num(o, "cost", a->total_cost);
    json_set_int(o, "contextTokens", a->last_context_tokens);
    json_set_int(o, "contextWindow", a->model ? a->model->context_window : 0);
    json_set_num(o, "contextPercent",
                 a->model && a->model->context_window ? 100.0 * (double)a->last_context_tokens / (double)a->model->context_window : 0);
    if (a->session->file) json_set_str(o, "sessionFile", a->session->file);
    json_set_str(o, "sessionId", session_id(a->session));
    return o;
}

/* ---------- subagents ---------- */

typedef struct {
    volatile sig_atomic_t *pc;
    agent_t *ag;
    volatile int done;
} watch_ctx_t;

static void *cancel_watch(void *x) {
    watch_ctx_t *w = x;
    while (!w->done) {
        if (*w->pc) {
            agent_abort(w->ag);
            break;
        }
        sleep_ms(50);
    }
    return NULL;
}

app_t *app_subagent_new(app_t *parent, const char *agent_name, const char *model_override, const char *append_prompt,
                        char **err) {
    return app_subagent_new_at(parent, agent_name, model_override, append_prompt, NULL, err);
}

app_t *app_subagent_new_at(app_t *parent, const char *agent_name, const char *model_override, const char *append_prompt,
                           const char *session_file, char **err) {
    app_opts_t o = {0};
    o.session_file = session_file;
    o.cwd = parent->cwd;
    char *label = app_model_label(parent);
    json_t *defs = agent_defs_load(parent->cwd);
    json_t *def = json_get(defs, agent_name ? agent_name : "task");
    /* resolveAgentModelSelection: request model, task.agentModelOverrides[agent], the agent's own
     * `model` (bar session-inheriting aliases), else the parent's active model; then the first
     * pattern that resolves over the available models (resolveModelOverrideWithAuthFallback) */
    const char *an = agent_name ? agent_name : "task";
    char *agent_model = NULL;
    json_t *dmj = json_get(def, "model");
    if (json_is_str(dmj)) agent_model = xstrdup(json_sv(dmj));
    else if (json_is_arr(dmj)) {
        strvec_t v = {0};
        json_t *x;
        json_arr_foreach(dmj, i, x) if (json_is_str(x)) strvec_push(&v, json_sv(x));
        agent_model = strvec_join(&v, ",");
        strvec_free(&v);
    }
    json_t *ovj = json_get(json_get_obj(settings_get("task"), "agentModelOverrides"), an);
    const char *override = json_is_str(ovj) ? json_sv(ovj) : NULL;
    strvec_t pats = mr_configured_patterns(model_override);
    if (!pats.n) pats = mr_configured_patterns(override);
    if (!pats.n && agent_model) {
        strvec_t single = mr_pattern_list(agent_model);
        const char *one = single.n == 1 ? single.v[0] : NULL;
        bool inherits = one && (!strcmp(one, "default") || !strcmp(one, "@default") || !strcmp(one, "*") ||
                                !strcmp(one, "pi/default") || !strcmp(one, "@task") || !strcmp(one, "pi/task"));
        bool task_alias = one && (!strcmp(one, "@task") || !strcmp(one, "pi/task"));
        strvec_t cp = mr_configured_patterns(agent_model);
        if (cp.n && (task_alias || !inherits)) {
            strvec_free(&pats);
            pats = cp;
        } else
            strvec_free(&cp);
        strvec_free(&single);
    }
    if (!pats.n) {
        strvec_free(&pats);
        pats = mr_configured_patterns(label);
    }
    mr_result_t sel;
    char *chosen = NULL;
    if (mr_resolve_override(&pats, &sel)) {
        char *k = mr_model_key(sel.model);
        chosen = sel.explicit_level ? mr_selector_with_level(k, sel.level) : xstrdup(k);
        free(k);
    }
    mr_result_clear(&sel);
    strvec_free(&pats);
    free(agent_model);
    o.model = chosen ? chosen : label;
    o.append_system_prompt = append_prompt ? append_prompt : json_get_str(def, "body");
    o.mcp = parent->mcp;
    o.mcp_seed_scope = parent->agent->mcp_scope;
    o.lsp = parent->lsp;
    strvec_t tools = {0};
    json_t *tl = json_get_arr(def, "tools"), *tn;
    json_arr_foreach(tl, i, tn) if (json_is_str(tn)) strvec_push(&tools, json_sv(tn));
    if (tools.n) o.tools = &tools;
    o.no_session = true;
    o.subagent = true;
    o.auto_approve = parent->opts.auto_approve;
    o.subagent_depth = parent->env->subagent_depth + 1;
    o.no_context_files = false;
    /* tier.subagent: inherit the parent's live per-family tiers (tracks /fast), or one tier for every family */
    ai_service_tiers_t parent_tiers, child_tiers;
    pthread_mutex_lock(&parent->tier_mu);
    parent_tiers = parent->tiers;
    pthread_mutex_unlock(&parent->tier_mu);
    modelctl_tiers_for_setting(settings_str("tier.subagent", "inherit"), &parent_tiers, &child_tiers);
    o.service_tiers = &child_tiers;
    strvec_t roots = session_additional_dirs(parent->session); /* forwarded to subagents (ToolSession) */
    o.add_dirs = &roots;
    app_t *child = app_create(&o, err);
    strvec_free(&roots);
    free(label);
    free(chosen);
    strvec_free(&tools);
    json_free(defs);
    if (!child) return NULL;
    costgate_attach_subagent(child);
    /* subagents share the parent's artifact id space and directory (omp adoptArtifactManager) */
    artifacts_unref(child->env->artifacts);
    child->env->artifacts = artifacts_ref(parent->env->artifacts);
    child->thinking = parent->thinking;
    child->agent->thinking = parent->thinking;
    child->auto_thinking = parent->auto_thinking;
    child->auto_resolved = parent->auto_resolved;
    return child;
}

char *app_run_subagent(tool_env_t *env, const char *agent_name, const char *assignment, const char *context,
                       json_t **details, volatile sig_atomic_t *cancel) {
    app_t *parent = env->session;
    char *err = NULL;
    app_t *child = app_subagent_new(parent, agent_name, NULL, NULL, &err);
    if (!child) {
        char *r = xasprintf("Failed to start subagent: %s", err ? err : "?");
        free(err);
        return r;
    }
    sb_t p = SB_INIT;
    if (agent_name && *agent_name) sb_appendf(&p, "You are the `%s` subagent.\n\n", agent_name);
    if (context && *context) sb_appendf(&p, "<context>\n%s\n</context>\n\n", context);
    sb_append(&p, assignment ? assignment : "");
    /* propagate cancellation */
    volatile sig_atomic_t *pc = cancel;
    pthread_t watcher;
    watch_ctx_t wc = {pc, child->agent, 0};
    bool watching = pc && pthread_create(&watcher, NULL, cancel_watch, &wc) == 0;
    bool refused = app_prompt(child, sb_cstr(&p), NULL) == APP_PROMPT_REFUSED;
    wc.done = 1;
    if (watching) pthread_join(watcher, NULL);
    sb_free(&p);
    const json_t *last = last_assistant(child->agent->messages);
    char *text = last ? ai_message_text(last) : xstrdup("");
    if (details) {
        *details = app_stats(child);
        json_set_str(*details, "agent", agent_name ? agent_name : "task");
        if (refused && child->prompt_refusal) json_set_str(*details, "error", child->prompt_refusal);
        else if (last && json_get_str(last, "errorMessage")) json_set_str(*details, "error", json_get_str(last, "errorMessage"));
    }
    costgate_merge_child(parent, child->total_cost);
    app_free(child);
    return text;
}

/* ---------- workspace and session lifecycle (WP-21) ---------- */

int app_fresh_session(app_t *a) {
    if (agent_is_running(a->agent)) return -1;
    pthread_mutex_lock(&a->mu);
    int n = (int)a->provider_states.n;
    strvec_free(&a->provider_states);
    memset(&a->provider_states, 0, sizeof a->provider_states);
    pthread_mutex_unlock(&a->mu);
    /* a fresh provider session id: prompt-cache keys and server-side conversations start over */
    free(a->agent->session_id);
    a->agent->session_id = uuid_v7();
    return n;
}

char *app_resolve_user_path(app_t *a, const char *p) {
    const char *s = p ? p : "";
    /* expandPath: a stray leading ':' before a path shape, an '@' before an absolute / ~ path, file:// */
    if (s[0] == ':' && (s[1] == '/' || s[1] == '~' || str_starts(s + 1, "./") || str_starts(s + 1, "../"))) s++;
    if (s[0] == '@' && (s[1] == '/' || s[1] == '~')) s++;
    if (str_starts(s, "file://")) s += 7;
    bool slashes = *s != 0;
    for (const char *q = s; *q; q++) slashes &= *q == '/';
    if (slashes) return xstrdup(a->cwd);
    return path_abs(s, a->cwd);
}

char *app_workspace_dirs_text(app_t *a, const char *note) {
    sb_t b = SB_INIT;
    if (note) sb_appendf(&b, "%s\n", note);
    sb_appendf(&b, "Workspace directories:\n  %s (working directory)", a->cwd);
    strvec_t roots = session_additional_dirs(a->session);
    for (size_t i = 0; i < roots.n; i++) sb_appendf(&b, "\n  %s", roots.v[i]);
    strvec_free(&roots);
    return sb_detach(&b);
}

int app_add_workspace_dir(app_t *a, const char *dir, char **resolved, char **err) {
    int rc = session_add_workspace_dir(a->session, dir, resolved, err);
    if (rc > 0) app_rebuild_system_prompt(a); /* refreshBaseSystemPrompt */
    return rc;
}

int app_remove_workspace_dir(app_t *a, const char *dir, char **resolved) {
    int rc = session_remove_workspace_dir(a->session, dir, resolved);
    if (rc > 0) app_rebuild_system_prompt(a);
    return rc;
}

/* applyCwdChange: everything derived from the working directory follows it */
static void app_apply_cwd(app_t *a, const char *cwd) {
    free(a->cwd);
    a->cwd = xstrdup(cwd);
    free(a->env->cwd);
    a->env->cwd = xstrdup(cwd);
    settings_reload_for_cwd(cwd);
    a->env->settings = settings_effective();
    discovery_free(&a->disc);
    discover_all(cwd, &a->disc, a->opts.no_context_files, a->opts.no_skills, a->opts.no_rules);
    a->env->disc = &a->disc;
    ttsr_free(a->ttsr);
    a->ttsr = ttsr_new(&a->disc);
    if (!a->opts.subagent) {
        commands_free(&a->file_cmds);
        commands_load(cwd, &a->file_cmds);
        prompt_templates_free(&a->prompt_tpls);
        prompt_templates_load(cwd, &a->prompt_tpls);
        app_bind_artifacts(a, NULL);
    }
    app_bind_local(a);
    app_rebuild_system_prompt(a);
}

int app_move(app_t *a, const char *dir, char **err) {
    *err = NULL;
    char *target = path_abs(dir, a->cwd);
    char *prev = xstrdup(a->cwd);
    int rc = -1;
    char *merr = NULL;
    if (session_move(a->session, target, &merr) != 0) {
        *err = xasprintf("Move failed: %s", merr ? merr : "unknown error");
        goto out;
    }
    if (chdir(target) != 0) {
        *err = xasprintf("Cannot change working directory to %s: %s", target, strerror(errno));
        char *rerr = NULL;
        session_move(a->session, prev, &rerr); /* nothing committed: the session goes back */
        free(rerr);
        goto out;
    }
    app_apply_cwd(a, target);
    rc = 0;
out:
    free(merr);
    free(target);
    free(prev);
    return rc;
}
