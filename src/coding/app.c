#include "app.h"
#include "coding/goal.h"
#include "coding/title.h"
#include "coding/skilldesc.h"
#include "coding/advisor.h"
#include "coding/evalhost.h"
#include "coding/autolearn.h"
#include "coding/memory.h"
#include "tools/artifacts.h"
#include "tools/jobs.h"
#include "tools/xdev.h"
#include "agents.h"

#include "config/auth.h"
#include "config/settings.h"
#include "config/compaction_point.h"
#include "config/model_resolver.h"
#include "core/fs.h"
#include "core/proc.h"
#include "coding/ttsr.h"
#include "dap/dap.h"
#include "lsp/lsp.h"
#include "tools/gh.h"
#include "mcp/mcp.h"
#include "prompt/template.h"

#include <ctype.h>
#include <math.h>

/* ---------- model resolution (config/model_resolver.c) ---------- */

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

static void maybe_title(app_t *a);

/* a background job settled: running sessions pick it up at the next turn boundary, idle ones are woken */
static void app_job_settled(void *ud) {
    app_t *a = ud;
    if (!agent_is_running(a->agent) && a->wake) a->wake(a->wake_ud);
}

/* omp async-result custom message details: {jobs: [{jobId, type, label, durationMs}]} (owned) */
static json_t *job_batch_details(const json_t *done) {
    json_t *d = json_obj(), *jobs = json_arr(), *j;
    json_arr_foreach(done, i, j) {
        json_t *e = json_obj();
        if (json_get_str(j, "jobId")) json_set_str(e, "jobId", json_get_str(j, "jobId"));
        if (json_get_str(j, "type")) json_set_str(e, "type", json_get_str(j, "type"));
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
    if (ev->type == AGENT_EV_MESSAGE_END && str_eq(json_get_str(ev->message, "role"), "assistant")) {
        const char *stop = json_get_str(ev->message, "stopReason");
        if (!str_eq(stop, "error") && !str_eq(stop, "aborted")) {
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
    if (ev->type == AGENT_EV_TURN_END && a->env && jobs_undelivered(a->env->jobs)) {
        json_t *m = job_results_message(a);
        if (m) agent_steer(a->agent, m);
    }
    app_goal_on_event(a, ev);
    title_on_event(a, ev);
    if (a->advisor) advisor_on_event(a->advisor, ev);
    autolearn_on_event(a->autolearn, ev);
    if (ev->type == AGENT_EV_TURN_START || ev->type == AGENT_EV_MESSAGE_START) {
        if (ev->type == AGENT_EV_TURN_START || str_eq(json_get_str(ev->message, "role"), "assistant")) ttsr_reset(a->ttsr);
    }
    if (ev->type == AGENT_EV_TURN_END && a->pending_rewind && a->checkpoint_index > 0) {
        /* cut the context back to the checkpoint and retain only the report */
        pthread_mutex_lock(&a->agent->mu);
        json_t *msgs = a->agent->messages;
        while ((int64_t)json_len(msgs) > a->checkpoint_index) json_free(json_arr_remove(msgs, json_len(msgs) - 1));
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
        if (role && a->session && ctype && str_eq(role, "user")) {
            /* hidden steering (goal context, continuations, budget limits) persists as custom_message like omp */
            char *txt = ai_message_text(ev->message);
            const json_t *det = json_get(ev->message, "details");
            if (det) { /* async-result job rows */
                json_t *e = json_obj();
                json_set_str(e, "type", "custom_message");
                json_set_str(e, "customType", ctype);
                json_set_str(e, "content", txt);
                json_set_bool(e, "display", false);
                json_set(e, "details", json_clone(det));
                session_append_entry(a->session, e);
            } else session_append_custom_message(a->session, ctype, txt, false);
            free(txt);
        } else if (role && a->session) {
            session_append_message(a->session, ev->message);
        }
        if (str_eq(role, "assistant")) {
            json_t *u = json_get(ev->message, "usage");
            a->total_cost += json_get_num(json_get(u, "cost"), "total", 0);
            __atomic_add_fetch(&a->output_tokens, json_get_int(u, "output", 0), __ATOMIC_SEQ_CST);
            int64_t ctx = json_get_int(u, "input", 0) + json_get_int(u, "cacheRead", 0) + json_get_int(u, "cacheWrite", 0) +
                          json_get_int(u, "output", 0);
            if (ctx > 0) a->last_context_tokens = ctx;
        }
    }
}

/* ---------- hooks ---------- */

static void hook_prepare(void *ud, agent_t *ag, ai_stream_opts_t *o) {
    app_t *a = ud;
    free(ag->api_key);
    ag->api_key = app_api_key_for(a->model);
    o->api_key = ag->api_key;
    if (a->plan_force_tool_choice) { /* plan-mode-decision: the reminder continuation must call a tool */
        static json_t *required;
        if (!required) required = json_str("required");
        o->tool_choice = required;
        a->plan_force_tool_choice = false;
    }
    /* omp resolveCacheRetention: explicit setting, else PI_CACHE_RETENTION (long|short|none), else "short" */
    const char *cr = settings_str("providers.cacheRetention", "auto");
    if (!cr || !*cr || str_eq(cr, "auto")) {
        const char *env = env_get("PI_CACHE_RETENTION");
        cr = env && (str_eq(env, "long") || str_eq(env, "short") || str_eq(env, "none")) ? env : "short";
    }
    o->cache_retention = cr;
    json_t *tb = settings_get("thinkingBudgets");
    if (json_is_obj(tb)) o->thinking_budgets = tb;
    double temp = settings_num("temperature", NAN);
    if (!isnan(temp)) o->temperature = temp;
    o->text_verbosity = settings_str("textVerbosity", NULL);
    const char *tier = settings_str("tier.openai", NULL);
    if (tier) o->service_tier = tier;
}

/* ---------- date/cwd reminder (port of omp session/date-cwd-reminder.ts) ----------
 * The system prompt stays byte-stable; the volatile date/cwd line rides on the first user turn of each
 * request. Turns keep the reminder they were first sent with, so the cached prefix never changes; a
 * changed reminder (new day / new cwd) attaches to the newest unsent user turn instead. */
static char *dcr_render(app_t *a) {
    char date[16];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof date, "%Y-%m-%d", &tm);
    json_t *c = json_obj();
    json_set_str(c, "date", date);
    json_set_str(c, "cwd", a->cwd);
    char *r = prompt_render("coding-agent/prompts/system/date-cwd-reminder.md", c);
    json_free(c);
    char *trim = str_trim(r ? r : "");
    free(r);
    return trim;
}

static json_t *hook_transform_locked(app_t *a, const json_t *messages);

/* the title fork runs the same transform from its own thread: serialize the reminder state */
static json_t *hook_transform(void *ud, agent_t *ag, const json_t *messages) {
    app_t *a = ud;
    UNUSED(ag);
    if (!settings_bool("dateCwdReminder", true)) return NULL;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    json_t *r = hook_transform_locked(a, messages);
    pthread_mutex_unlock(&mu);
    return r;
}

static json_t *hook_transform_locked(app_t *a, const json_t *messages) {
    ssize_t first = -1;
    json_t *m;
    json_arr_foreach(messages, i, m) if (str_eq(json_get_str(m, "role"), "user")) {
        first = (ssize_t)i;
        break;
    }
    if (first < 0) return NULL;
    char *rem = dcr_render(a);
    int64_t fts = json_get_int(json_at(messages, (size_t)first), "timestamp", 0);
    if (!a->dcr_sent || a->dcr_first_ts != fts) { /* new conversation root (new session, compaction, branch) */
        json_free(a->dcr_sent);
        a->dcr_sent = json_obj();
        a->dcr_first_ts = fts;
        char key[48];
        snprintf(key, sizeof key, "user:%lld", (long long)fts);
        json_set_str(a->dcr_sent, key, rem);
        free(a->dcr_last);
        a->dcr_last = xstrdup(rem);
    } else if (!str_eq(a->dcr_last, rem)) {
        const json_t *last = json_at(messages, json_len(messages) - 1);
        if (str_eq(json_get_str(last, "role"), "user")) {
            char key[48];
            snprintf(key, sizeof key, "user:%lld", (long long)json_get_int(last, "timestamp", 0));
            if (!json_get(a->dcr_sent, key)) {
                json_set_str(a->dcr_sent, key, rem);
                free(a->dcr_last);
                a->dcr_last = xstrdup(rem);
            }
        }
    }
    free(rem);
    json_t *out = json_arr();
    json_arr_foreach(messages, i, m) {
        json_t *c = json_clone(m);
        if (str_eq(json_get_str(m, "role"), "user")) {
            char key[48];
            snprintf(key, sizeof key, "user:%lld", (long long)json_get_int(m, "timestamp", 0));
            const char *r = json_get_str(a->dcr_sent, key);
            json_t *content = json_get(c, "content");
            if (r && json_is_str(content)) {
                char *t = xasprintf("%s\n\n%s", r, json_sv(content));
                json_set_str(c, "content", t);
                free(t);
            } else if (r && json_is_arr(content)) {
                const json_t *b0 = json_at(content, 0);
                if (!str_eq(json_get_str(b0, "text"), r)) json_arr_insert(content, 0, text_block(r));
            }
        }
        json_push(out, c);
    }
    return out;
}

static bool is_overflow(const char *err) {
    return err && (str_istr(err, "prompt is too long") || str_istr(err, "context length") || str_istr(err, "maximum context") ||
                   str_istr(err, "context_length_exceeded") || str_istr(err, "too many tokens") ||
                   str_istr(err, "exceeds the context window") || str_istr(err, "input is too long"));
}

static bool is_retryable(const char *err) {
    if (!err) return false;
    const char *pats[] = {"overloaded", "rate limit", "rate_limit", "429", "500", "502", "503", "504", "529",
                          "timed out", "timeout", "connection", "network", "reset", "closed before", "mid-body",
                          "internal server error", "service unavailable", "bad gateway", "temporarily", "try again",
                          "stream ended", "terminated"};
    for (size_t i = 0; i < ARRAY_LEN(pats); i++)
        if (str_istr(err, pats[i])) return true;
    return false;
}

static int hook_stream(void *ud, agent_t *ag, const ai_event_t *ev) {
    app_t *a = ud;
    UNUSED(ag);
    if (ttsr_on_event(a->ttsr, ev)) {
        a->ttsr_interrupting = true;
        return 1;
    }
    return 0;
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
    if (a->session->dir) a->env->local_dir = xasprintf("%s/%s/local", a->session->dir, sid);
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
    static const char *const keys[] = {"retry-after-ms:", "retry after ", "retry-after: ", "retry-after:", "try again in ",
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
        if (k == 0 || !strncasecmp(end, "ms", 2)) ms = v;
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
                           const char *err) {
    ai_effort_t prev = a->thinking, next = a->thinking;
    if (level && effort_parse(level, &next) != 0) next = a->thinking;
    char *from = xstrdup(cur);
    a->model = cand;
    agent_set_model(a->agent, cand);
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
    notice(a, "retry_fallback_applied", "%s\x1f%s\x1f%s\x1fRequest failed: %s", from, key, role,
           err ? err : "provider returned an error without details");
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
            apply_fallback(a, keys.v[k], cand, c[i].level, cur, err);
            done = true;
        }
        for (size_t i = 0; i < n; i++) mr_selector_free(&c[i]);
        free(c);
    }
    mr_pool_free(&avail);
    strvec_free(&keys);
    return done;
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
    if (pm && mr_provider_available(pm->provider)) {
        ai_effort_t th = a->thinking == a->fallback.last_thinking ? a->fallback.original_thinking : a->thinking;
        a->fallback.active = false;
        a->model = pm;
        agent_set_model(a->agent, pm);
        char *key = mr_model_key(pm);
        session_append_model_change_ex(a->session, key, "fallback", false);
        mr_usage_record(key);
        free(key);
        if (th != a->thinking) app_set_thinking(a, th);
        app_rebuild_system_prompt(a);
    }
    mr_selector_free(&orig);
}

static bool hook_should_retry(void *ud, agent_t *ag, const json_t *assistant, int attempt) {
    app_t *a = ud;
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
    if ((flagged || is_overflow(err)) && attempt == 0 && !a->compacting) {
        notice(a, "warning", "Context overflow; compacting and retrying");
        char *cerr = NULL;
        if (app_compact(a, NULL, &cerr) == 0) return true;
        notice(a, "error", "Compaction failed: %s", cerr ? cerr : "?");
        free(cerr);
        return false;
    }
    if (!settings_bool("retry.enabled", true)) return false;
    if (attempt == 0) a->retry_offset = 0;
    int max = (int)settings_int("retry.maxRetries", 10);
    int saga = attempt - a->retry_offset + 1; /* omp #retryAttempt after its increment */
    bool exhausted = saga > max;
    bool retryable = is_retryable(err);
    bool usage_limit = is_usage_limit(err);
    /* model fallback: consulted on every failed attempt, retryable or hard (hardErrorFallback) */
    bool switched = false;
    if (a->model && !is_abort_error(err) && settings_bool("retry.modelFallback", true) && mr_fallback_admits(usage_limit) &&
        !(is_overflow(err))) {
        char *cur = current_selector(a);
        int64_t hint = retry_after_ms(err);
        mr_suppress_selector(cur, now_ms() + (hint > 0 ? hint : mr_rate_limit_backoff_ms(err)));
        bool rate_limited = str_istr(err, "429") || str_eq(mr_rate_limit_reason(err), "RATE_LIMIT_EXCEEDED");
        bool same_model_retry = retryable && !exhausted && !(settings_bool("retry.rateLimitWalksChain", false) && rate_limited);
        switched = try_retry_fallback(a, ag, cur, assistant, err, same_model_retry);
        free(cur);
    }
    if (switched) {
        a->retry_offset = attempt + 1; /* a fallback model gets a fresh retry budget */
        strvec_push(&a->retry_notes, usage_limit ? "rate-limited; switched model; retried" : "switched model; retried");
        return true;
    }
    if (!retryable) return false;
    if (exhausted) {
        /* closeRetrySaga failure: the final turn reads "Retry budget exhausted …", superseded attempts hide,
         * then "Retry failed after N attempts: <error>" (fields: attempts \x1f final turn error \x1f error) */
        notice(a, "retry-failed", "%d\x1fRetry budget exhausted after %d %s: %s\x1f%s", saga - 1, saga - 1,
               saga - 1 == 1 ? "retry" : "retries", err ? err : "error", err ? err : "Unknown error");
        strvec_free(&a->retry_notes);
        return false;
    }
    int64_t delay = mr_retry_backoff_ms(settings_int("retry.baseDelayMs", 500), saga);
    const char *reason = mr_rate_limit_reason(err);
    int64_t hint = retry_after_ms(err);
    if (hint < 0 && (str_eq(reason, "CONCURRENT_LIMIT") || str_eq(reason, "RATE_LIMIT_EXCEEDED") ||
                     str_eq(reason, "MODEL_CAPACITY_EXHAUSTED"))) {
        int64_t rb = mr_rate_limit_backoff_ms(err);
        if (rb > delay) delay = rb;
    }
    if (hint > delay) delay = hint;
    int64_t maxd = settings_int("retry.maxDelayMs", 5 * 60 * 1000);
    if (maxd > 0 && delay > maxd) {
        notice(a, "error", "Provider requested %lldms wait, exceeds retry.maxDelayMs (%lldms). Original error: %s",
               (long long)delay, (long long)maxd, err ? err : "error");
        return false;
    }
    notice(a, "warning", "Retrying (%d/%d) in %llds: %s", saga, max, (long long)((delay + 999) / 1000), err ? err : "error");
    strvec_push(&a->retry_notes, usage_limit ? "rate-limited; waited; retried" : "error; retried");
    int64_t until = mono_ms() + delay;
    while (mono_ms() < until) {
        if (ag->cancel) return false;
        sleep_ms(50);
    }
    return true;
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

static void install_agent(app_t *a) {
    agent_t *ag = agent_new();
    a->agent = ag;
    ag->app = a;
    ag->max_turns = a->opts.max_turns;
    ag->hooks.prepare_request = hook_prepare;
    ag->hooks.should_retry = hook_should_retry;
    ag->hooks.transform_context = hook_transform;
    ag->hooks.on_stream_delta = hook_stream;
    ag->hooks.after_tool = hook_after_tool;
    ag->hooks.before_tool = hook_before_tool;
    ag->hooks.ud = a;
    free(ag->session_id);
    ag->session_id = a->session ? xstrdup(session_id(a->session)) : uuid_v7();
    agent_subscribe(ag, on_agent_event, a);
    agent_set_model(ag, a->model);
    ag->thinking = a->thinking;
    a->env->can_prompt_user = a->opts.has_ui;
    a->env->delegation_bias = model_delegation_bias(a->model);
    agent_set_intent_tracing(settings_bool("tools.intentTracing", true));
    if (!a->opts.no_tools) tools_register_builtin(ag, a->env, a->opts.tools);
    if (a->mcp && !a->opts.no_tools) mcp_register_tools(a->mcp, ag);
    bool lsp_ok = !a->opts.no_tools && settings_bool("lsp.enabled", true) &&
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
    if (!a->opts.no_tools) autolearn_register_tools(a, ag);
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
    char *xdev_docs = json_len(xdev_tools) ? xdev_prompt_docs(a->agent) : xstrdup("");
    sysprompt_opts_t so = {.cwd = a->cwd,
                           .model_name = label,
                           .tool_names = &names,
                           .disc = &a->disc,
                           .custom_prompt = a->sys_custom,
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
                           .generated_append = sb_cstr(&gen)};
    json_t *blocks = build_system_prompt(&so);
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
    char *mi = a->mcp ? mcp_instructions(a->mcp) : NULL;
    if (mi && *mi) json_push_str(blocks, mi);
    free(mi);
    agent_set_system_prompt_parts(a->agent, blocks);
    free(label);
    strvec_free(&names);
}

static void load_session_into_agent(app_t *a) {
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
    if (sc.thinking && !a->opts.thinking) {
        ai_effort_t e;
        if (effort_parse(sc.thinking, &e) == 0) {
            a->thinking = e;
            a->agent->thinking = e;
        }
    }
    session_context_free(&sc);
}

app_t *app_create(const app_opts_t *o, char **err) {
    *err = NULL;
    ai_providers_init();
    catalog_init();
    auth_init();
    catalog_discover_local(false); /* implicit local servers (ollama, llama.cpp, LM Studio); once per process */
    app_t *a = xcalloc(1, sizeof(*a));
    pthread_mutex_init(&a->mu, NULL);
    a->opts = *o;
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
    if (o->thinking) {
        if (effort_parse(o->thinking, &eff) != 0 && strcmp(o->thinking, "auto") != 0) {
            *err = xasprintf("Invalid thinking level: %s", o->thinking);
            app_free(a);
            return NULL;
        }
        has_eff = true;
    }
    if (!has_eff) effort_parse(settings_str("defaultThinkingLevel", "high"), &eff);
    a->thinking = eff;
    /* environment & discovery */
    a->env = tool_env_new(a->cwd);
    a->env->jobs = jobs_new();
    jobs_set_notify(a->env->jobs, app_job_settled, a);
    a->env->settings = settings_effective();
    a->env->subagent_depth = o->subagent_depth;
    a->env->session = a;
    if (!o->subagent) agents_register_main(a);
    a->env->run_subagent = app_run_subagent;
    a->env->judge = app_tool_judge;
    a->evalhost = evalhost_new(a);
    a->env->eval_bridge = app_eval_bridge;
    discover_all(a->cwd, &a->disc, o->no_context_files, o->no_skills, o->no_rules);
    a->env->disc = &a->disc;
    a->ttsr = ttsr_new(&a->disc);
    if (o->mcp) {
        a->mcp = o->mcp;
    } else if (!o->no_mcp && !o->no_tools && settings_bool("mcp.enabled", true)) {
        json_t *cfg = mcp_discover_config(a->cwd);
        if (json_len(cfg)) {
            a->mcp = mcp_manager_new();
            a->owns_mcp = true;
            mcp_connect_all(a->mcp, cfg, settings_int("mcp.startupTimeoutMs", 15000));
        }
        json_free(cfg);
    }
    /* session */
    if (o->no_session) {
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
    a->memory = memory_rt_new();
    if (!o->subagent) a->autolearn = autolearn_new(a);
    install_agent(a);
    if (json_len(a->session->entries)) load_session_into_agent(a);
    if (!o->subagent) app_goal_restore(a);
    app_rebuild_system_prompt(a);
    if (!json_len(a->session->entries) && a->session->persist && a->model) {
        char *label = app_model_label(a);
        session_append_model_change(a->session, label, NULL);
        session_append_thinking_change(a->session, effort_name(a->thinking));
        free(label);
    }
    a->title_done = a->session->title != NULL;
    if (settings_bool("advisor.enabled", false) && !o->subagent) app_set_advisor(a, true);
    memory_start(a);
    return a;
}

void app_free(app_t *a) {
    if (!a) return;
    agents_unregister(a);
    autolearn_free(a->autolearn); /* abort a private capture before the session it reads goes away */
    a->autolearn = NULL;
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
    title_free(a);
    skilldesc_free(a->skilldesc);
    json_free(a->dcr_sent);
    free(a->dcr_last);
    json_free(a->job_batch);
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
    a->model = m;
    agent_set_model(a->agent, m);
    a->fallback.active = false; /* an explicit model change clears fallback ownership */
    free(a->model_role);
    a->model_role = NULL;
    char *uk = mr_model_key(m);
    mr_usage_record(uk);
    free(uk);
    if (has_e) app_set_thinking(a, e);
    char *label = app_model_label(a);
    session_append_model_change(a->session, label, NULL);
    free(label);
    app_rebuild_system_prompt(a);
    return 0;
}

void app_set_thinking(app_t *a, ai_effort_t e) {
    a->thinking = e;
    a->agent->thinking = e;
    session_append_thinking_change(a->session, effort_name(e));
}

void app_abort(app_t *a) {
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

int app_cycle_role_model(app_t *a, bool backward) {
    if (!a->model) return -1;
    strvec_t order = app_cycle_order();
    mr_pool_t avail = {0};
    mr_pool_available(&avail, "chat");
    typedef struct {
        size_t role;
        const ai_model_t *m;
        const char *level;
        bool explicit_level;
    } ent_t;
    VEC(ent_t) es = {0};
    for (size_t i = 0; i < order.n && avail.n; i++) {
        char *cur = NULL;
        const char *v = mr_role_get(order.v[i]);
        if (!v && !strcmp(order.v[i], "default")) v = cur = mr_model_key(a->model);
        mr_result_t r = {0};
        if (v && mr_resolve_value(v, &avail, &r)) vec_push(&es, ((ent_t){i, r.model, r.level, r.explicit_level}));
        mr_result_clear(&r);
        free(cur);
    }
    int res = -1;
    if (es.n > 1) {
        long at = -1;
        for (size_t i = 0; i < es.n && a->model_role && at < 0; i++)
            if (!strcmp(order.v[es.v[i].role], a->model_role)) at = es.v[i].m == a->model ? (long)i : -2;
        if (at < 0)
            for (size_t i = 0; i < es.n && at < 0; i++)
                if (es.v[i].m == a->model) at = (long)i;
        if (at < 0) at = 0;
        long n = (long)es.n;
        ent_t nx = es.v[(at + (backward ? -1 : 1) + n) % n];
        a->model = nx.m;
        agent_set_model(a->agent, nx.m);
        a->fallback.active = false;
        free(a->model_role);
        a->model_role = xstrdup(order.v[nx.role]);
        char *k = mr_model_key(nx.m);
        session_append_model_change(a->session, k, order.v[nx.role]);
        mr_usage_record(k);
        free(k);
        ai_effort_t e;
        if (nx.explicit_level && nx.level && effort_parse(nx.level, &e) == 0) app_set_thinking(a, e);
        app_rebuild_system_prompt(a);
        res = (int)nx.role;
    }
    free(es.v);
    mr_pool_free(&avail);
    strvec_free(&order);
    return res;
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

void app_set_advisor(app_t *a, bool on) {
    if (on && !a->advisor) a->advisor = advisor_new(a);
    else if (!on && a->advisor) {
        advisor_free(a->advisor);
        a->advisor = NULL;
    }
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

char *app_side_question(app_t *a, const char *question, char **err) {
    json_t *c = json_obj();
    json_set_str(c, "question", question);
    char *q = prompt_render("coding-agent/prompts/system/btw-user.md", c);
    json_free(c);
    pthread_mutex_lock(&a->agent->mu);
    json_t *msgs = json_clone(a->agent->messages);
    json_t *sys = json_clone(a->agent->system_prompt);
    pthread_mutex_unlock(&a->agent->mu);
    json_push(msgs, msg_user_text(q));
    free(q);
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    char *key = app_api_key_for(a->model);
    o.api_key = key;
    o.reasoning = model_clamp_effort(a->model, a->thinking);
    o.disable_reasoning = a->thinking == EFFORT_OFF;
    volatile sig_atomic_t cancel = 0;
    o.cancel = &cancel;
    json_t *r = ai_complete(a->model, &ctx, &o);
    char *text = NULL;
    const char *stop = json_get_str(r, "stopReason");
    if (str_eq(stop, "error") || str_eq(stop, "aborted"))
        *err = xstrdup(json_get_str(r, "errorMessage") ? json_get_str(r, "errorMessage") : "side question failed");
    else text = ai_message_text(r);
    json_free(r);
    json_free(sys);
    json_free(msgs);
    free(key);
    return text;
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

int app_clear_context(app_t *a) {
    if (agent_is_running(a->agent)) return -1;
    if (a->session) session_append_reset_boundary(a->session);
    agent_replace_messages(a->agent, json_arr());
    a->last_context_tokens = 0;
    return 0;
}

int app_handoff(app_t *a, char **err) {
    char *doc = prompt_load("agent/compaction/prompts/handoff-document.md");
    int rc = app_compact(a, doc, err);
    free(doc);
    return rc;
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

size_t app_shake(app_t *a, size_t min_chars) {
    size_t count = 0;
    pthread_mutex_lock(&a->agent->mu);
    json_t *m;
    json_arr_foreach(a->agent->messages, i, m) {
        if (!str_eq(json_get_str(m, "role"), "toolResult")) continue;
        char *t = ai_message_text(m);
        size_t len = strlen(t);
        bool has_image = false;
        json_t *b;
        json_arr_foreach(json_get_arr(m, "content"), k, b) if (str_eq(json_get_str(b, "type"), "image")) has_image = true;
        if (len >= min_chars || has_image) {
            char *note = xasprintf("[%zu chars%s of %s output elided by /shake]", len, has_image ? " and images" : "",
                                   json_get_str(m, "toolName") ? json_get_str(m, "toolName") : "tool");
            json_t *c = json_arr();
            json_push(c, text_block(note));
            json_set(m, "content", c);
            free(note);
            count++;
        }
        free(t);
    }
    pthread_mutex_unlock(&a->agent->mu);
    return count;
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

/* ---------- compaction ---------- */

static int64_t estimate_tokens(const json_t *msg) {
    char *s = json_dump(json_get(msg, "content"), 0);
    int64_t n = (int64_t)strlen(s) / 4 + 4;
    free(s);
    return n;
}

static void serialize_for_summary(sb_t *b, const json_t *msg) {
    const char *role = json_get_str(msg, "role");
    json_t *c = json_get(msg, "content");
    if (str_eq(role, "user")) sb_append(b, "[User]: ");
    else if (str_eq(role, "assistant")) sb_append(b, "[Assistant]: ");
    else if (str_eq(role, "toolResult")) sb_appendf(b, "[Tool result %s]: ", json_get_str(msg, "toolName"));
    else sb_appendf(b, "[%s]: ", role ? role : "message");
    if (json_is_str(c)) {
        sb_append(b, json_sv(c));
    } else {
        json_t *blk;
        json_arr_foreach(c, i, blk) {
            const char *t = json_get_str(blk, "type");
            if (str_eq(t, "text")) {
                const char *tx = json_get_str(blk, "text");
                size_t n = strlen(tx);
                if (str_eq(role, "toolResult") && n > 4000) sb_appendf(b, "%.2000s\n…[%zu chars elided]…\n%s", tx, n - 4000, tx + n - 2000);
                else sb_append(b, tx);
            } else if (str_eq(t, "toolCall")) {
                char *args = json_dump(json_get(blk, "arguments"), 0);
                if (strlen(args) > 2000) args[2000] = 0;
                sb_appendf(b, "\n<tool_call name=\"%s\">%s</tool_call>", json_get_str(blk, "name"), args);
                free(args);
            } else if (str_eq(t, "image")) {
                sb_append(b, "[image]");
            }
        }
    }
    sb_append(b, "\n\n");
}

int app_compact(app_t *a, const char *instructions, char **err) {
    if (a->compacting) {
        *err = xstrdup("compaction already running");
        return -1;
    }
    a->compacting = true;
    int rc = -1;
    json_t *path = session_path_to(a->session, a->session->leaf_id);
    /* find message entries, pick cut point keeping ~keepRecentTokens at a user-message boundary */
    int64_t keep = settings_int("compaction.keepRecentTokens", 20000);
    ssize_t cut = -1;
    int64_t acc = 0;
    size_t n = json_len(path);
    for (size_t i = n; i > 0; i--) {
        json_t *e = json_at(path, i - 1);
        if (!str_eq(json_get_str(e, "type"), "message")) continue;
        json_t *m = json_get(e, "message");
        acc += estimate_tokens(m);
        if (acc >= keep && str_eq(json_get_str(m, "role"), "user")) {
            cut = (ssize_t)(i - 1);
            break;
        }
    }
    /* previous compaction summary on path */
    const char *prev_summary = NULL;
    ssize_t prev_idx = -1;
    for (size_t i = 0; i < n; i++)
        if (str_eq(json_get_str(json_at(path, i), "type"), "compaction")) {
            prev_summary = json_get_str(json_at(path, i), "summary");
            prev_idx = (ssize_t)i;
        }
    if (cut < 0) {
        /* keep only the last user turn */
        for (size_t i = n; i > 0 && cut < 0; i--) {
            json_t *e = json_at(path, i - 1);
            if (str_eq(json_get_str(e, "type"), "message") && str_eq(json_get_str(json_get(e, "message"), "role"), "user") &&
                (ssize_t)(i - 1) > prev_idx)
                cut = (ssize_t)(i - 1);
        }
    }
    if (cut <= prev_idx + 1) {
        *err = xstrdup("Nothing to compact");
        goto out;
    }
    sb_t conv = SB_INIT;
    sb_append(&conv, "<conversation>\n");
    size_t from = prev_idx >= 0 ? (size_t)prev_idx + 1 : 0;
    for (size_t i = from; i < (size_t)cut; i++) {
        json_t *e = json_at(path, i);
        if (str_eq(json_get_str(e, "type"), "message")) serialize_for_summary(&conv, json_get(e, "message"));
    }
    sb_append(&conv, "</conversation>\n\n");
    if (prev_summary) {
        sb_appendf(&conv, "<previous-summary>\n%s\n</previous-summary>\n\n", prev_summary);
        char *p = prompt_load_or("agent/compaction/prompts/compaction-update-summary.md", "Update the summary.");
        sb_append(&conv, p);
        free(p);
    } else {
        char *p = prompt_load_or("agent/compaction/prompts/compaction-summary.md", "Summarize the conversation.");
        sb_append(&conv, p);
        free(p);
    }
    if (instructions && *instructions) sb_appendf(&conv, "\n\nAdditional focus: %s", instructions);
    char *sys = prompt_load_or("agent/compaction/prompts/summarization-system.md", "Summarize.");
    notice(a, "info", "Compacting context…");
    mr_pool_t cands = {0};
    app_compaction_candidates(a, &cands);
    char *summary = NULL;
    for (size_t i = 0; i < cands.n && !summary && !a->agent->cancel; i++)
        summary = app_complete_text(a, cands.v[i], sys, sb_cstr(&conv), EFFORT_LOW);
    mr_pool_free(&cands);
    sb_free(&conv);
    free(sys);
    if (!summary || !*summary) {
        *err = xstrdup("summarization request failed");
        free(summary);
        goto out;
    }
    const char *first_kept = json_get_str(json_at(path, (size_t)cut), "id");
    session_append_compaction(a->session, summary, first_kept, a->last_context_tokens, NULL);
    free(summary);
    load_session_into_agent(a);
    a->last_context_tokens = 0;
    notice(a, "info", "Context compacted");
    rc = 0;
out:
    json_free(path);
    a->compacting = false;
    return rc;
}

/* omp's context-maintenance trigger: thresholdTokens (>0) overrides thresholdPercent (>0); both -1 =
 * legacy reserve-based budget (window - max(15% of window, reserveTokens)) */
int64_t app_compaction_threshold(const ai_model_t *m, int64_t window) { return cp_model_threshold(m, window); }

static void maybe_auto_compact(app_t *a) {
    if (!settings_bool("compaction.enabled", true) || !a->model) return;
    int64_t window = a->model->context_window;
    if (window <= 0) return;
    int64_t thresh = app_compaction_threshold(a->model, window);
    if (thresh <= 0 || a->last_context_tokens < thresh) return;
    char *err = NULL;
    if (app_compact(a, NULL, &err) != 0) {
        notice(a, "error", "Auto-compaction failed: %s", err ? err : "?");
        free(err);
    }
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

int app_prompt(app_t *a, const char *text, json_t *images) {
    app_plan_note_user_turn(a);
    app_plan_yolo_arm(a);
    memory_take_prompt_refresh(a);
    maybe_restore_primary(a);
    begin_turn_budget(a, text);
    json_t *content = json_arr();
    json_push(content, text_block(text));
    add_mentions(a, text, content);
    json_t *img;
    json_arr_foreach(images, i, img) json_push(content, json_clone(img));
    json_free(images);
    json_t *msgs = json_arr();
    json_t *pr = app_plan_reference_message(a);
    if (pr) json_push(msgs, pr);
    json_t *gm = app_goal_context_message(a);
    if (gm) json_push(msgs, gm);
    json_push(msgs, msg_user_content(content));
    int rc = agent_prompt(a->agent, msgs);
    rc = app_plan_settle(a, rc);
    if (rc == 0) {
        maybe_title(a);
        maybe_auto_compact(a);
    }
    return rc;
}

int app_prompt_hidden(app_t *a, const char *text, const char *custom_type) {
    maybe_restore_primary(a);
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
    int rc = agent_prompt(a->agent, msgs);
    rc = app_plan_settle(a, rc);
    if (rc == 0) maybe_auto_compact(a);
    return rc;
}

json_t *app_user_bash(app_t *a, const char *cmd, bool exclude) {
    proc_opts_t o = {.cwd = a->cwd, .timeout_ms = 300000, .merge_stderr = true, .new_session = true};
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
    if (a->session) session_append_message(a->session, m);
    if (!exclude) {
        char *t = out_bash_execution_text(m);
        agent_append_message(a->agent, msg_user_text(t));
        free(t);
    }
    /* BashResult counters for RPC callers */
    json_set_int(m, "totalLines", sum.total_lines);
    json_set_int(m, "totalBytes", sum.total_bytes);
    json_set_int(m, "outputLines", sum.output_lines);
    json_set_int(m, "outputBytes", sum.output_bytes);
    out_summary_free(&sum);
    proc_result_free(&r);
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
    free(a->agent->session_id);
    a->agent->session_id = xstrdup(session_id(ns));
    char *label = app_model_label(a);
    session_append_model_change(ns, label, NULL);
    session_append_thinking_change(ns, effort_name(a->thinking));
    free(label);
    a->title_done = false;
    a->total_cost = 0;
    a->last_context_tokens = 0;
    json_free(a->env->todo.phases);
    a->env->todo.phases = json_arr();
    app_goal_restore(a);
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
    app_opts_t o = {0};
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
    o.lsp = parent->lsp;
    strvec_t tools = {0};
    json_t *tl = json_get_arr(def, "tools"), *tn;
    json_arr_foreach(tl, i, tn) if (json_is_str(tn)) strvec_push(&tools, json_sv(tn));
    if (tools.n) o.tools = &tools;
    o.no_session = true;
    o.subagent = true;
    o.subagent_depth = parent->env->subagent_depth + 1;
    o.no_context_files = false;
    app_t *child = app_create(&o, err);
    free(label);
    free(chosen);
    strvec_free(&tools);
    json_free(defs);
    if (!child) return NULL;
    /* subagents share the parent's artifact id space and directory (omp adoptArtifactManager) */
    artifacts_unref(child->env->artifacts);
    child->env->artifacts = artifacts_ref(parent->env->artifacts);
    child->thinking = parent->thinking;
    child->agent->thinking = parent->thinking;
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
    app_prompt(child, sb_cstr(&p), NULL);
    wc.done = 1;
    if (watching) pthread_join(watcher, NULL);
    sb_free(&p);
    const json_t *last = last_assistant(child->agent->messages);
    char *text = last ? ai_message_text(last) : xstrdup("");
    if (details) {
        *details = app_stats(child);
        json_set_str(*details, "agent", agent_name ? agent_name : "task");
        if (last && json_get_str(last, "errorMessage")) json_set_str(*details, "error", json_get_str(last, "errorMessage"));
    }
    parent->total_cost += child->total_cost;
    app_free(child);
    return text;
}
