/* rpc mode: JSONL commands on stdin, responses + agent events on stdout (omp RPC protocol subset). */
#include "modes.h"
#include "coding/btw.h"
#include "coding/goal.h"
#include "coding/slashcmd.h"
#include "session/export_html.h"

#include "config/auth.h"
#include "config/settings.h"
#include "core/proc.h"
#include "core/fs.h"
#include "tools/artifacts.h"
#include "tools/jobs.h"

#include <errno.h>
#include <pthread.h>
#include <unistd.h>

typedef struct {
    char *id;
    json_t *result;
    bool is_error;
    bool done;
} hostcall_t;

struct rpc {
    app_t *a;
    rpc_sink_fn sink; /* NULL => stdout */
    void *sink_ud;
    /* event filter: allowed event types; !filter_on => all */
    pthread_mutex_t filter_mu;
    strvec_t filter;
    bool filter_on;
    /* host tools implemented by the client */
    pthread_mutex_t host_mu;
    pthread_cond_t host_cv;
    VEC(hostcall_t *) host_calls;
    strvec_t host_tools;
    uint64_t host_seq;
    pthread_t th;
    bool running;
    char *prompt_id;
    char *text;
    json_t *images;
    const char *hidden; /* custom type when the run is a hidden wake-up (async-result) */
    bool slash;         /* the prompt is a builtin slash command (executeAcpBuiltinSlashCommand) */
    json_t *skill_msg;  /* the prompt invokes a skill (dispatchRpcSkillPrompt): its built custom message */
    pthread_mutex_t mu;
    /* /btw (rpc-btw.ts) */
    pthread_mutex_t btw_mu;
    btw_store_t *btw_store;
    char *btw_store_key;          /* session id \x01 artifacts dir the store was opened for */
    struct rpc_btw_run *btw_running;
    bool btw_starting;            /* a `btw` between validation and its first checkpoint */
    char *btw_starting_topic;     /* its recordId (NULL: a new topic) */
    int btw_epoch;                /* bumped by every cancel and session change */
    json_t *btw_unsaved;          /* id -> record whose terminal checkpoint failed */
    pthread_t btw_th;
    bool btw_th_live;
};

static void emit(rpc_t *rp, const json_t *j) {
    if (rp && rp->sink) rp->sink(rp->sink_ud, j);
    else out_json_line(j);
}

static void respond(rpc_t *rp, const char *id, const char *cmd, bool ok, json_t *data, const char *err) {
    json_t *r = json_obj();
    if (id) json_set_str(r, "id", id);
    json_set_str(r, "type", "response");
    json_set_str(r, "command", cmd ? cmd : "unknown");
    json_set_bool(r, "success", ok);
    if (data) json_set(r, "data", data);
    if (err) json_set_str(r, "error", err);
    emit(rp, r);
    json_free(r);
}

/* ---------- event filter ---------- */

static bool event_allowed(rpc_t *r, const char *type) {
    pthread_mutex_lock(&r->filter_mu);
    bool ok = !r->filter_on || strvec_contains(&r->filter, type ? type : "");
    pthread_mutex_unlock(&r->filter_mu);
    return ok;
}

/* ---------- host tools (implemented by the RPC client) ---------- */

static int host_tool_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    rpc_t *r = self->ud;
    hostcall_t *hc = xcalloc(1, sizeof(*hc));
    pthread_mutex_lock(&r->host_mu);
    hc->id = xasprintf("host-%llu", (unsigned long long)++r->host_seq);
    vec_push(&r->host_calls, hc);
    pthread_mutex_unlock(&r->host_mu);
    json_t *req = json_obj();
    json_set_str(req, "type", "host_tool_call");
    json_set_str(req, "id", hc->id);
    json_set_str(req, "toolCallId", ctx && ctx->tool_call_id ? ctx->tool_call_id : "");
    json_set_str(req, "toolName", self->name);
    json_set(req, "arguments", args ? json_clone(args) : json_obj());
    emit(r, req);
    json_free(req);
    bool cancelled = false;
    pthread_mutex_lock(&r->host_mu);
    while (!hc->done) {
        if (ctx && ctx->cancel && *ctx->cancel) {
            cancelled = true;
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&r->host_cv, &r->host_mu, &ts);
    }
    for (size_t i = 0; i < r->host_calls.n; i++)
        if (r->host_calls.v[i] == hc) {
            r->host_calls.v[i] = r->host_calls.v[--r->host_calls.n];
            break;
        }
    pthread_mutex_unlock(&r->host_mu);
    if (cancelled) {
        json_t *c = json_obj();
        json_set_str(c, "type", "host_tool_cancel");
        char *cid = xasprintf("%s-cancel", hc->id);
        json_set_str(c, "id", cid);
        free(cid);
        json_set_str(c, "targetId", hc->id);
        emit(r, c);
        json_free(c);
        tool_result_error(out, "Tool call cancelled");
    } else {
        json_t *blk;
        json_arr_foreach(json_get_arr(hc->result, "content"), i, blk) {
            if (str_eq(json_get_str(blk, "type"), "image")) tool_result_image(out, json_get_str(blk, "data"), json_get_str(blk, "mimeType"));
            else if (json_get_str(blk, "text")) tool_result_text(out, json_get_str(blk, "text"));
        }
        if (json_is_str(json_get(hc->result, "content"))) tool_result_text(out, json_sv(json_get(hc->result, "content")));
        if (json_get(hc->result, "details")) out->details = json_clone(json_get(hc->result, "details"));
        out->is_error = hc->is_error;
    }
    json_free(hc->result);
    free(hc->id);
    free(hc);
    return 0;
}

static void host_tool_complete(rpc_t *r, const json_t *cmd) {
    const char *id = json_get_str(cmd, "id");
    pthread_mutex_lock(&r->host_mu);
    for (size_t i = 0; i < r->host_calls.n; i++)
        if (str_eq(r->host_calls.v[i]->id, id)) {
            r->host_calls.v[i]->result = json_clone(json_get(cmd, "result"));
            r->host_calls.v[i]->is_error = json_get_bool(cmd, "isError", false);
            r->host_calls.v[i]->done = true;
        }
    pthread_cond_broadcast(&r->host_cv);
    pthread_mutex_unlock(&r->host_mu);
}

/* ---------- session tree ---------- */

static json_t *tree_node(const session_t *s, const json_t *e, const json_t *labels, int depth) {
    json_t *n = json_obj();
    json_set(n, "entry", json_clone(e));
    const char *lab = json_get_str(labels, json_get_str(e, "id"));
    if (lab) json_set_str(n, "label", lab);
    json_t *kids = json_arr();
    if (depth < 10000) {
        json_t *ch = session_children(s, json_get_str(e, "id")), *c;
        json_arr_foreach(ch, i, c) json_push(kids, tree_node(s, c, labels, depth + 1));
        json_free(ch);
    }
    json_set(n, "children", kids);
    return n;
}

static json_t *session_tree_json(const session_t *s) {
    json_t *labels = json_obj(), *e;
    json_arr_foreach(s->entries, i, e) {
        if (!str_eq(json_get_str(e, "type"), "label")) continue;
        const char *t = json_get_str(e, "targetId");
        if (!t) continue;
        if (json_get_str(e, "label") && *json_get_str(e, "label")) json_set_str(labels, t, json_get_str(e, "label"));
        else json_del(labels, t);
    }
    json_t *roots = json_arr();
    json_arr_foreach(s->entries, i, e) {
        const char *pid = json_get_str(e, "parentId");
        if (!pid || !session_get(s, pid)) json_push(roots, tree_node(s, e, labels, 0));
    }
    json_free(labels);
    return roots;
}

static void rpc_listener(void *ud, const agent_event_t *ev) {
    rpc_t *r = ud;
    json_t *j = agent_event_to_json(ev);
    if (!event_allowed(r, json_get_str(j, "type"))) {
        json_free(j);
        return;
    }
    if (ev->type == AGENT_EV_AGENT_END) {
        json_set_bool(j, "isTerminal", true);
        json_set_bool(j, "yielded", true);
    }
    emit(r, j);
    json_free(j);
}

static void notice_rpc(void *ud, const char *level, const char *text) {
    rpc_t *r = ud;
    if (!strcmp(level, "retry-recovered")) return;
    if (!strcmp(level, "todo-reminder")) { /* {type:"todo_reminder", todos, attempt, maxAttempts} */
        json_t *j = json_parse(text, strlen(text), NULL);
        if (!json_is_obj(j)) {
            json_free(j);
            return;
        }
        json_t *o = json_obj();
        json_set_str(o, "type", "todo_reminder");
        const char *k;
        json_t *v;
        json_obj_foreach(j, i, k, v) json_set(o, k, json_clone(v));
        json_free(j);
        emit(r, o);
        json_free(o);
        return;
    }
    if (!strcmp(level, "retry-failed")) {
        const char *f1 = strchr(text, '\x1f'), *f2 = f1 ? strchr(f1 + 1, '\x1f') : NULL;
        if (!f1 || !f2) return;
        json_t *j = json_obj();
        json_set_str(j, "type", "auto_retry_end");
        json_set_bool(j, "success", false);
        json_set_int(j, "attempt", atoi(text));
        json_set_str(j, "finalError", f2 + 1);
        emit(r, j);
        json_free(j);
        return;
    }
    if (!strcmp(level, "retry_fallback_applied")) {
        static const char *const keys[] = {"from", "to", "role", "reason"};
        json_t *j = json_obj();
        json_set_str(j, "type", "retry_fallback_applied");
        const char *s = text;
        for (size_t k = 0; k < 4 && s; k++) {
            const char *sep = strchr(s, '\x1f');
            char *v = sep ? xstrndup(s, (size_t)(sep - s)) : xstrdup(s);
            json_set_str(j, keys[k], v);
            free(v);
            s = sep ? sep + 1 : NULL;
        }
        emit(r, j);
        json_free(j);
        return;
    }
    if (!strcmp(level, "retry_fallback_succeeded")) {
        const char *sep = strchr(text, '\x1f');
        json_t *j = json_obj();
        json_set_str(j, "type", "retry_fallback_succeeded");
        char *model = sep ? xstrndup(text, (size_t)(sep - text)) : xstrdup(text);
        json_set_str(j, "model", model);
        json_set_str(j, "role", sep ? sep + 1 : "");
        free(model);
        emit(r, j);
        json_free(j);
        return;
    }
    json_t *j = json_obj();
    json_set_str(j, "type", "notice");
    json_set_str(j, "level", level);
    json_set_str(j, "message", text);
    emit(r, j);
    json_free(j);
}

static void rpc_command_output(void *ud, const char *text) {
    json_t *o = json_obj();
    json_set_str(o, "type", "command_output");
    json_set_str(o, "text", text);
    emit(ud, o);
    json_free(o);
}

static void *prompt_thread(void *p) {
    rpc_t *r = p;
again:;
    size_t before = json_len(r->a->agent->messages);
    if (r->slash) {
        /* builtin: output as command_output frames; only an agent-invoking one (/retry) reports a
         * prompt_result (the prompt response already said agentInvoked) */
        r->slash = false;
        slash_result_t sr;
        slash_execute_text(r->a, r->text, rpc_command_output, r, &sr);
        bool invoked = sr.agent_invoked;
        slash_result_free(&sr);
        json_free(r->images);
        r->images = NULL;
        if (!invoked) {
            pthread_mutex_lock(&r->mu);
            free(r->text);
            r->text = NULL;
            free(r->prompt_id);
            r->prompt_id = NULL;
            r->running = false;
            pthread_mutex_unlock(&r->mu);
            return NULL;
        }
    } else if (r->hidden) app_prompt_hidden(r->a, r->text, r->hidden);
    else if (r->skill_msg) {
        json_t *m = r->skill_msg;
        r->skill_msg = NULL;
        app_prompt_skill(r->a, m);
    } else {
        /* AgentSession.prompt: custom (MCP prompt) / file commands / prompt templates expand first */
        char *x = app_expand_input(r->a, r->text, true);
        if (!x) { /* consumed locally: prompt_result agentInvoked false (completeLocal) */
            json_free(r->images);
            r->images = NULL;
            json_t *res = json_obj();
            json_set_str(res, "type", "prompt_result");
            if (r->prompt_id) json_set_str(res, "id", r->prompt_id);
            json_set_bool(res, "agentInvoked", false);
            json_set_str(res, "status", "completed");
            emit(r, res);
            json_free(res);
            pthread_mutex_lock(&r->mu);
            free(r->text);
            r->text = NULL;
            free(r->prompt_id);
            r->prompt_id = NULL;
            r->running = false;
            pthread_mutex_unlock(&r->mu);
            return NULL;
        }
        app_prompt(r->a, x, r->images);
        free(x);
    }
    r->images = NULL;
    const json_t *last = last_assistant(r->a->agent->messages);
    const char *stop = json_get_str(last, "stopReason");
    json_t *res = json_obj();
    json_set_str(res, "type", "prompt_result");
    if (r->prompt_id) json_set_str(res, "id", r->prompt_id);
    json_set_bool(res, "agentInvoked", true);
    const char *status = str_eq(stop, "aborted") ? "aborted" : str_eq(stop, "error") ? "error" : "completed";
    json_set_str(res, "status", status);
    if (!strcmp(status, "error")) {
        json_t *e = json_obj();
        json_set_str(e, "message", json_get_str(last, "errorMessage") ? json_get_str(last, "errorMessage") : "error");
        json_set_str(e, "provider", json_get_str(last, "provider"));
        json_set_str(e, "model", json_get_str(last, "model"));
        json_set_bool(e, "retryable", false);
        json_set(res, "error", e);
    }
    /* goal mode: keep going with hidden continuation turns while the goal is active and making progress */
    bool was_cont = false;
    const char *prev_type = r->hidden;
    for (;;) {
        json_t *run = json_arr();
        pthread_mutex_lock(&r->a->agent->mu);
        for (size_t i = before; i < json_len(r->a->agent->messages); i++) json_push(run, json_clone(json_at(r->a->agent->messages, i)));
        pthread_mutex_unlock(&r->a->agent->mu);
        const char *ctype = NULL;
        char *cont = app_after_run(r->a, run, prev_type, "rpc", &ctype);
        json_free(run);
        if (!cont) break;
        if (!was_cont) {
            json_set_bool(res, "sessionSettled", false);
            emit(r, res);
        }
        was_cont = true;
        before = json_len(r->a->agent->messages);
        prev_type = ctype;
        app_prompt_hidden(r->a, cont, ctype);
        free(cont);
    }
    json_set_bool(res, "sessionSettled", true);
    if (!was_cont) emit(r, res);
    json_free(res);
    json_t *settled = json_obj();
    json_set_str(settled, "type", "session_settled");
    emit(r, settled);
    json_free(settled);
    pthread_mutex_lock(&r->mu);
    free(r->text);
    r->text = NULL;
    free(r->prompt_id);
    r->prompt_id = NULL;
    /* a job may have settled between the run's last check and now: deliver it in this same thread */
    char *late = jobs_undelivered(r->a->env->jobs) ? app_job_results_text(r->a) : NULL;
    if (late) {
        r->text = late;
        r->hidden = "async-result";
        r->images = NULL;
        pthread_mutex_unlock(&r->mu);
        goto again;
    }
    r->running = false;
    r->hidden = NULL;
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

/* background job settled while idle: run its result as a hidden turn */
static void rpc_wake(void *ud) {
    rpc_t *r = ud;
    pthread_mutex_lock(&r->mu);
    if (r->running) {
        pthread_mutex_unlock(&r->mu);
        return;
    }
    char *t = app_job_results_text(r->a);
    if (!t) {
        pthread_mutex_unlock(&r->mu);
        return;
    }
    if (r->th) pthread_join(r->th, NULL);
    r->th = 0;
    r->running = true;
    r->hidden = "async-result";
    r->text = t;
    r->prompt_id = NULL;
    r->images = NULL;
    if (pthread_create(&r->th, NULL, prompt_thread, r) != 0) {
        r->running = false;
        r->hidden = NULL;
    }
    pthread_mutex_unlock(&r->mu);
}

static json_t *state_json(app_t *a) {
    json_t *s = json_obj();
    json_t *m = json_obj();
    if (a->model) {
        json_set_str(m, "provider", a->model->provider);
        json_set_str(m, "id", a->model->id);
        json_set_str(m, "name", a->model->name);
        json_set_str(m, "api", a->model->api);
        json_set_int(m, "contextWindow", a->model->context_window);
        json_set_bool(m, "reasoning", a->model->reasoning);
    }
    json_set(s, "model", m);
    json_set_str(s, "thinkingLevel", effort_name(a->thinking));
    json_set_bool(s, "isStreaming", agent_is_running(a->agent));
    json_set_bool(s, "isCompacting", a->compacting);
    json_set_str(s, "sessionId", session_id(a->session));
    if (a->session->file) json_set_str(s, "sessionFile", a->session->file);
    if (a->session->title) json_set_str(s, "sessionName", a->session->title);
    json_set_int(s, "messageCount", (int64_t)json_len(a->agent->messages));
    json_set_str(s, "steeringMode", settings_str("steeringMode", "one-at-a-time"));
    json_set_str(s, "followUpMode", settings_str("followUpMode", "one-at-a-time"));
    json_set_str(s, "interruptMode", settings_str("interruptMode", "immediate"));
    json_set(s, "stats", app_stats(a));
    json_set(s, "goal", goal_state_json(app_goal(a)));
    return s;
}

static json_t *content_with_images(const char *text, json_t *images) {
    json_t *c = json_arr();
    json_push(c, text_block(text ? text : ""));
    json_t *img;
    json_arr_foreach(images, i, img) {
        if (str_eq(json_get_str(img, "type"), "image")) json_push(c, json_clone(img));
    }
    return c;
}

/* ---------- /btw side questions (port of modes/rpc/rpc-btw.ts) ----------
 * One side question runs at a time against the main session; answers stream as `btw_delta` frames, every
 * lifecycle change is a full `btw_record` frame, and each turn is checkpointed into the session's BTW history
 * sidecar so the TUI and RPC hosts read and continue the same topics. */

#define BTW_CANCELLED_WHILE_STARTING "The /btw question was cancelled before it started"

typedef struct rpc_btw_run {
    rpc_t *r;
    json_t *record;
    btw_store_t *store;
    bool owns_store; /* the store was replaced while this turn ran: the turn frees it */
    char *question;
    json_t *history;
    char *conversation_key;
    volatile sig_atomic_t cancel;
} rpc_btw_run_t;

static void btw_emit_record(rpc_t *r, const json_t *record) {
    json_t *f = json_obj();
    json_set_str(f, "type", "btw_record");
    json_set(f, "record", btw_wire_record(record));
    emit(r, f);
    json_free(f);
}

static void btw_notice(rpc_t *r, const char *message) {
    json_t *j = json_obj();
    json_set_str(j, "type", "notice");
    json_set_str(j, "level", "error");
    json_set_str(j, "message", message);
    json_set_str(j, "source", "btw-history");
    emit(r, j);
    json_free(j);
}

/* #finish: the turn's terminal state, once; caller holds btw_mu */
static void btw_finish_locked(rpc_t *r, rpc_btw_run_t *run, const char *status, const char *answer, const char *error) {
    if (r->btw_running != run) return;
    r->btw_running = NULL;
    btw_set_status(run->record, status, answer, error);
    btw_emit_record(r, run->record);
    char *err = NULL;
    if (btw_store_upsert(run->store, run->record, &err) != 0) {
        if (run->store == r->btw_store) {
            if (!r->btw_unsaved) r->btw_unsaved = json_obj();
            json_set(r->btw_unsaved, json_get_str(run->record, "id"), json_clone(run->record));
        }
        char *m = xasprintf("Could not save /btw history: %s", err ? err : "write failed");
        log_msg(LOG_ERROR, "%s", m);
        btw_notice(r, m);
        free(m);
    }
    free(err);
}

static void btw_delta(void *ud, const char *delta) {
    rpc_btw_run_t *run = ud;
    rpc_t *r = run->r;
    pthread_mutex_lock(&r->btw_mu);
    if (r->btw_running == run) {
        json_t *t = btw_latest_turn(run->record);
        char *ans = xasprintf("%s%s", json_get_str(t, "answer") ? json_get_str(t, "answer") : "", delta);
        json_set_str(t, "answer", ans);
        json_set_int(t, "updatedAt", now_ms());
        free(ans);
        json_t *f = json_obj();
        json_set_str(f, "type", "btw_delta");
        json_set_str(f, "recordId", json_get_str(run->record, "id"));
        json_set_str(f, "delta", delta);
        emit(r, f);
        json_free(f);
    }
    pthread_mutex_unlock(&r->btw_mu);
}

static void *btw_thread(void *p) {
    rpc_btw_run_t *run = p;
    rpc_t *r = run->r;
    char *reply = NULL, *err = NULL;
    json_t *assistant = NULL;
    int rc = btw_run_turn(r->a, run->question, NULL, run->history, run->conversation_key, NULL, btw_delta, run,
                          &run->cancel, &reply, &assistant, &err);
    pthread_mutex_lock(&r->btw_mu);
    if (rc == 0) btw_finish_locked(r, run, "complete", reply, NULL);
    else btw_finish_locked(r, run, "error", NULL, err ? err : "side question failed");
    if (run->owns_store) btw_store_free(run->store);
    pthread_mutex_unlock(&r->btw_mu);
    json_free(assistant);
    free(reply);
    free(err);
    json_free(run->record);
    json_free(run->history);
    free(run->question);
    free(run->conversation_key);
    free(run);
    return NULL;
}

/* cancel(recordId?): the running or starting question (only topic `record_id` when given); caller holds btw_mu */
static bool btw_cancel_locked(rpc_t *r, const char *record_id) {
    rpc_btw_run_t *run = r->btw_running;
    if (run) {
        if (record_id && !str_eq(json_get_str(run->record, "id"), record_id)) return false;
        r->btw_epoch++;
        btw_finish_locked(r, run, "cancelled", NULL, NULL);
        run->cancel = 1;
        return true;
    }
    if (!r->btw_starting || (record_id && !str_eq(r->btw_starting_topic, record_id))) return false;
    r->btw_epoch++;
    return true;
}

/* #settleWrites: retry failed terminal checkpoints once; a conflict drops the record with a notice */
static int btw_settle_locked(rpc_t *r, char **err) {
    if (!r->btw_unsaved || !r->btw_store) return 0;
    while (json_len(r->btw_unsaved)) {
        const char *id = r->btw_unsaved->u.obj.keys[0];
        char *e = NULL;
        if (btw_store_retry(r->btw_store, r->btw_unsaved->u.obj.vals[0], &e) == 0 || (e && strstr(e, "conflict"))) {
            if (e) {
                char *m = xasprintf("/btw answer %s was not saved: %s; it changed or was removed on disk", id, e);
                log_msg(LOG_ERROR, "%s", m);
                btw_notice(r, m);
                free(m);
            }
            free(e);
            json_del(r->btw_unsaved, id);
            continue;
        }
        *err = xasprintf("/btw history could not be saved: %s. Fix the storage and retry; unsaved answers remain in this "
                         "session.",
                         e ? e : "write failed");
        free(e);
        return -1;
    }
    return 0;
}

static char *btw_artifacts_dir(app_t *a) {
    return a->session && a->session->file && a->session->persist ? artifacts_dir_for_session(a->session->file) : NULL;
}

/* #openStore: the current session's store; `fresh` re-reads it from disk (only while nothing runs here) */
static btw_store_t *btw_open_store_locked(rpc_t *r, bool fresh, char **err) {
    app_t *a = r->a;
    char *dir = btw_artifacts_dir(a);
    char *key = xasprintf("%s%c%s", a->session ? session_id(a->session) : "", 1, dir ? dir : "");
    bool same = r->btw_store && str_eq(r->btw_store_key, key);
    if (same && (!fresh || !dir || (r->btw_unsaved && json_len(r->btw_unsaved)))) {
        free(key);
        free(dir);
        return r->btw_store;
    }
    if (!same && r->btw_running) btw_cancel_locked(r, NULL); /* the old session's question must not stream on */
    btw_store_t *s = btw_store_open(dir, NULL, err);
    free(dir);
    if (!s) {
        free(key);
        return NULL;
    }
    if (r->btw_store) {
        if (r->btw_running && r->btw_running->store == r->btw_store) r->btw_running->owns_store = true;
        else btw_store_free(r->btw_store);
    }
    r->btw_store = s;
    free(r->btw_store_key);
    r->btw_store_key = key;
    return s;
}

/* ask(question, recordId?): checkpointed as running, then the turn starts after the response */
static json_t *btw_ask(rpc_t *r, const char *question, const char *record_id, char **err) {
    app_t *a = r->a;
    char *trimmed = str_trim(question ? question : "");
    json_t *out = NULL;
    pthread_mutex_lock(&r->btw_mu);
    if (!*trimmed) {
        *err = xstrdup("btw requires a non-empty question");
        goto done;
    }
    if (r->btw_starting || r->btw_running) {
        *err = xstrdup("A /btw question is still running; cancel it first");
        goto done;
    }
    r->btw_starting = true;
    free(r->btw_starting_topic);
    r->btw_starting_topic = record_id ? xstrdup(record_id) : NULL;
    int epoch = r->btw_epoch;
    if (btw_settle_locked(r, err) != 0) goto starting_done;
    btw_store_t *store = btw_open_store_locked(r, true, err);
    if (!store) goto starting_done;
    if (epoch != r->btw_epoch) {
        *err = xstrdup(BTW_CANCELLED_WHILE_STARTING);
        goto starting_done;
    }
    json_t *previous = record_id ? btw_store_find(store, record_id) : NULL;
    if (record_id && !previous) {
        *err = xasprintf("Unknown /btw topic: %s", record_id);
        goto starting_done;
    }
    if (!a->model) {
        json_free(previous);
        *err = xstrdup("No active model available for /btw.");
        goto starting_done;
    }
    if (a->session) session_ensure_on_disk(a->session);
    btw_begin_t b;
    btw_begin_turn(trimmed, a->session ? a->session->leaf_id : NULL, previous, NULL, NULL, &b);
    json_free(previous);
    char *werr = NULL;
    if (btw_store_upsert(store, b.record, &werr) != 0) {
        *err = xasprintf("Could not save /btw history: %s", werr ? werr : "write failed");
        free(werr);
        btw_begin_free(&b);
        goto starting_done;
    }
    rpc_btw_run_t *run = xcalloc(1, sizeof *run);
    run->r = r;
    run->record = b.record;
    run->history = b.history;
    run->conversation_key = b.conversation_key;
    run->store = store;
    run->question = xstrdup(trimmed);
    r->btw_running = run;
    btw_emit_record(r, run->record);
    out = btw_wire_record(run->record);
    if (r->btw_th_live) pthread_join(r->btw_th, NULL); /* the previous turn already finished */
    r->btw_th_live = false;
    if (pthread_create(&r->btw_th, NULL, btw_thread, run) == 0) r->btw_th_live = true;
    else btw_finish_locked(r, run, "error", NULL, "failed to start the /btw thread");
starting_done:
    r->btw_starting = false;
    free(r->btw_starting_topic);
    r->btw_starting_topic = NULL;
done:
    pthread_mutex_unlock(&r->btw_mu);
    free(trimmed);
    return out;
}

/* history(): newest first, re-read from disk when idle; a running topic carries its live partial answer */
static json_t *btw_history(rpc_t *r, char **err) {
    pthread_mutex_lock(&r->btw_mu);
    btw_store_t *s = btw_open_store_locked(r, !r->btw_running && !r->btw_starting, err);
    json_t *out = NULL;
    if (s) {
        json_t *recs = btw_store_records(s), *rec;
        json_t *keyed = json_obj();
        json_arr_foreach(recs, i, rec) json_set(keyed, json_get_str(rec, "id"), json_clone(rec));
        if (r->btw_unsaved && s == r->btw_store) {
            for (size_t i = 0; i < json_len(r->btw_unsaved); i++)
                json_set(keyed, r->btw_unsaved->u.obj.keys[i], json_clone(r->btw_unsaved->u.obj.vals[i]));
        }
        if (r->btw_running && r->btw_running->store == s)
            json_set(keyed, json_get_str(r->btw_running->record, "id"), json_clone(r->btw_running->record));
        json_t *all = json_arr();
        for (size_t i = 0; i < json_len(keyed); i++) json_push(all, json_clone(keyed->u.obj.vals[i]));
        /* sort newest first by createdAt, then id */
        size_t n = json_len(all);
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j > 0; j--) {
                json_t *x = all->u.arr.items[j - 1], *y = all->u.arr.items[j];
                int64_t cx = json_get_int(x, "createdAt", 0), cy = json_get_int(y, "createdAt", 0);
                if (cx > cy || (cx == cy && strcmp(json_get_str(x, "id"), json_get_str(y, "id")) <= 0)) break;
                all->u.arr.items[j - 1] = y;
                all->u.arr.items[j] = x;
            }
        out = json_arr();
        json_arr_foreach(all, i, rec) json_push(out, btw_wire_record(rec));
        json_free(all);
        json_free(keyed);
        json_free(recs);
    }
    pthread_mutex_unlock(&r->btw_mu);
    return out;
}

/* close(): before the session is replaced or the process exits */
static int btw_close(rpc_t *r, char **err) {
    pthread_mutex_lock(&r->btw_mu);
    r->btw_epoch++;
    btw_cancel_locked(r, NULL);
    int rc = btw_settle_locked(r, err);
    pthread_mutex_unlock(&r->btw_mu);
    return rc;
}

static void handle(rpc_t *r, json_t *cmd) {
    app_t *a = r->a;
    const char *id = json_get_str(cmd, "id");
    const char *type = json_get_str(cmd, "type");
    if (!type) {
        respond(r, id, "unknown", false, NULL, "missing type");
        return;
    }
    if (!strcmp(type, "negotiate_protocol")) {
        json_t *d = json_obj();
        json_set_int(d, "protocolVersion", 1);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "prompt") || !strcmp(type, "abort_and_prompt")) {
        const char *msg = json_get_str(cmd, "message");
        if (!strcmp(type, "abort_and_prompt")) {
            app_abort(a);
            for (int k = 0; k < 600; k++) { /* wait for the run to unwind (max ~60s) */
                pthread_mutex_lock(&r->mu);
                bool busy = r->running;
                pthread_mutex_unlock(&r->mu);
                if (!busy) break;
                usleep(100 * 1000);
            }
        }
        /* dispatchRpcSkillPrompt: a runnable /skill: invocation is read before the command is acknowledged */
        json_t *skill_msg = NULL;
        {
            skill_invocation_t inv;
            if (app_skill_command(a, msg, &inv)) {
                skill_invocation_free(&inv);
                char *serr = NULL;
                json_t *imgs = json_get_arr(cmd, "images");
                skill_msg = app_skill_message(a, msg, imgs ? json_clone(imgs) : NULL, &serr);
                if (!skill_msg) {
                    respond(r, id, type, false, NULL, serr ? serr : "Failed to load skill");
                    free(serr);
                    return;
                }
            }
        }
        pthread_mutex_lock(&r->mu);
        if (r->running) {
            pthread_mutex_unlock(&r->mu);
            const char *behav = json_get_str(cmd, "streamingBehavior");
            json_t *m = skill_msg;
            if (!m) {
                char *x = app_expand_input(a, msg ? msg : "", true);
                if (!x) {
                    respond(r, id, type, true, NULL, NULL);
                    return;
                }
                m = msg_user_content(content_with_images(x, json_get_arr(cmd, "images")));
                free(x);
            }
            if (str_eq(behav, "followUp")) agent_follow_up(a->agent, m);
            else agent_steer(a->agent, m);
            respond(r, id, type, true, NULL, NULL);
            return;
        }
        if (r->th) pthread_join(r->th, NULL);
        r->th = 0;
        r->running = true;
        r->hidden = NULL;
        r->text = xstrdup(msg ? msg : "");
        r->prompt_id = id ? xstrdup(id) : NULL;
        json_t *imgs = json_get_arr(cmd, "images");
        r->images = imgs && !skill_msg ? json_clone(imgs) : NULL;
        r->skill_msg = skill_msg;
        slash_parsed_t sp = {0};
        const slash_cmd_t *sc = !skill_msg && slash_parse(r->text, &sp) ? slash_lookup(sp.name) : NULL;
        if (sp.name) slash_parsed_free(&sp);
        r->slash = sc && sc->handle;
        pthread_mutex_unlock(&r->mu);
        if (r->slash) {
            json_t *d = json_obj();
            json_set_bool(d, "agentInvoked", slash_invokes_agent(a, sc->name));
            respond(r, id, type, true, d, NULL);
        } else respond(r, id, type, true, NULL, NULL);
        if (pthread_create(&r->th, NULL, prompt_thread, r) != 0) {
            r->running = false;
            respond(r, id, type, false, NULL, "failed to start agent thread");
        }
    } else if (!strcmp(type, "steer") || !strcmp(type, "follow_up")) {
        /* steer() / followUp(): prompt templates expand (no slash or skill commands) */
        const char *raw = json_get_str(cmd, "message");
        char *x = app_expand_templates(a, raw ? raw : "");
        json_t *m = msg_user_content(content_with_images(x, json_get_arr(cmd, "images")));
        free(x);
        if (!strcmp(type, "steer")) agent_steer(a->agent, m);
        else agent_follow_up(a->agent, m);
        respond(r, id, type, true, NULL, NULL);
    } else if (!strcmp(type, "abort")) {
        agent_abort(a->agent);
        respond(r, id, type, true, NULL, NULL);
    } else if (!strcmp(type, "goal")) {
        const char *op = json_get_str(cmd, "op");
        char *err = NULL;
        int rc = 0;
        int64_t budget = json_get_int(cmd, "token_budget", 0);
        if (str_eq(op, "create")) rc = app_goal_create(a, json_get_str(cmd, "objective"), budget, &err);
        else if (str_eq(op, "resume")) rc = app_goal_resume(a, &err);
        else if (str_eq(op, "pause")) rc = app_goal(a) ? app_goal_pause(a) : (err = xstrdup("No goal."), -1);
        else if (str_eq(op, "drop")) app_goal_drop(a);
        else if (!str_eq(op, "get")) rc = (err = xasprintf("unknown goal op '%s'", op ? op : ""), -1);
        if (rc) {
            respond(r, id, type, false, NULL, err ? err : "goal operation failed");
        } else {
            json_t *d = json_obj();
            json_set(d, "goal", goal_json(app_goal(a)));
            json_set(d, "state", goal_state_json(app_goal(a)));
            respond(r, id, type, true, d, NULL);
        }
        free(err);
    } else if (!strcmp(type, "host_tool_result")) {
        host_tool_complete(r, cmd);
    } else if (!strcmp(type, "host_tool_update")) {
        /* partial results are informational for the host UI; nothing to do server-side */
    } else if (!strcmp(type, "set_host_tools")) {
        for (size_t i = 0; i < r->host_tools.n; i++) agent_remove_tool(a->agent, r->host_tools.v[i]);
        strvec_free(&r->host_tools);
        json_t *names = json_arr(), *t;
        json_arr_foreach(json_get_arr(cmd, "tools"), i, t) {
            const char *nm = json_get_str(t, "name");
            if (!nm || !*nm) continue;
            if (agent_find_tool(a->agent, nm)) agent_remove_tool(a->agent, nm);
            json_t *params = json_get(t, "parameters");
            agent_tool_t *tool = agent_tool_new(nm, json_get_str(t, "label") ? json_get_str(t, "label") : nm,
                                                json_get_str(t, "description") ? json_get_str(t, "description") : "",
                                                params ? json_clone(params) : json_parse_str("{\"type\":\"object\",\"properties\":{}}"),
                                                host_tool_execute);
            tool->hidden = json_get_bool(t, "hidden", false);
            tool->ud = r;
            agent_add_tool(a->agent, tool);
            strvec_push(&r->host_tools, nm);
            json_push_str(names, nm);
        }
        app_rebuild_system_prompt(a);
        json_t *d = json_obj();
        json_set(d, "toolNames", names);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "set_event_filter")) {
        pthread_mutex_lock(&r->filter_mu);
        strvec_free(&r->filter);
        json_t *ev = json_get(cmd, "events"), *x;
        r->filter_on = json_is_arr(ev);
        json_arr_foreach(ev, i, x) if (json_is_str(x)) strvec_push(&r->filter, json_sv(x));
        pthread_mutex_unlock(&r->filter_mu);
        json_t *d = json_obj();
        json_set(d, "events", r->filter_on ? json_clone(ev) : json_null());
        json_set_str(d, "messageUpdates", json_get_str(cmd, "messageUpdates") ? json_get_str(cmd, "messageUpdates") : "full");
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "set_todos")) {
        json_t *ph = json_get_arr(cmd, "phases");
        json_free(a->env->todo.phases);
        a->env->todo.phases = ph ? json_clone(ph) : json_arr();
        json_t *d = json_obj();
        json_set(d, "todoPhases", json_clone(a->env->todo.phases));
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_tree")) {
        json_t *d = json_obj();
        json_set(d, "tree", session_tree_json(a->session));
        json_set(d, "leafId", a->session->leaf_id ? json_str(a->session->leaf_id) : json_null());
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "navigate_tree")) {
        const char *eid = json_get_str(cmd, "entryId");
        json_t *e = eid ? session_get(a->session, eid) : NULL;
        if (r->running) respond(r, id, type, false, NULL, "agent is busy");
        else if (!e) respond(r, id, type, false, NULL, "unknown entry");
        else {
            app_nav_result_t nav;
            char *err = NULL;
            int rc = app_navigate_tree(a, eid, json_get_bool(cmd, "summarize", false), json_get_str(cmd, "customInstructions"),
                                       &nav, &err);
            if (rc < 0) respond(r, id, type, false, NULL, err);
            else {
                json_t *d = json_obj();
                if (nav.editor_text) json_set_str(d, "editorText", nav.editor_text);
                json_set_bool(d, "cancelled", rc == 1);
                if (rc == 1) json_set_bool(d, "aborted", true);
                respond(r, id, type, true, d, NULL);
            }
            free(nav.editor_text);
            free(err);
        }
    } else if (!strcmp(type, "set_label")) {
        const char *eid = json_get_str(cmd, "entryId");
        if (!eid || !session_get(a->session, eid)) respond(r, id, type, false, NULL, "unknown entry");
        else {
            session_append_label(a->session, eid, json_get_str(cmd, "label") ? json_get_str(cmd, "label") : "");
            respond(r, id, type, true, NULL, NULL);
        }
    } else if (!strcmp(type, "get_branch_messages")) {
        json_t *path = session_path_to(a->session, a->session->leaf_id), *e, *arr = json_arr();
        json_arr_foreach(path, i, e) {
            json_t *m = json_get(e, "message");
            if (!str_eq(json_get_str(e, "type"), "message") || !str_eq(json_get_str(m, "role"), "user") || json_get_bool(m, "synthetic", false))
                continue;
            json_t *x = json_obj();
            json_set_str(x, "entryId", json_get_str(e, "id"));
            char *t = ai_message_text(m);
            json_set_str(x, "text", t);
            free(t);
            json_push(arr, x);
        }
        json_free(path);
        json_t *d = json_obj();
        json_set(d, "messages", arr);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_last_assistant_text")) {
        const json_t *last = last_assistant(a->agent->messages);
        json_t *d = json_obj();
        char *t = last ? ai_message_text(last) : NULL;
        json_set(d, "text", t ? json_str(t) : json_null());
        free(t);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_messages_page")) {
        int64_t limit = json_get_int(cmd, "limit", 50);
        if (limit < 1) limit = 1;
        const char *cur = json_get_str(cmd, "cursor");
        pthread_mutex_lock(&a->agent->mu);
        int64_t total = (int64_t)json_len(a->agent->messages);
        int64_t end = cur ? strtoll(cur, NULL, 10) : total;
        if (end > total || end < 0) end = total;
        int64_t start = end - limit < 0 ? 0 : end - limit;
        json_t *page = json_arr();
        for (int64_t i = start; i < end; i++) json_push(page, json_clone(json_at(a->agent->messages, (size_t)i)));
        pthread_mutex_unlock(&a->agent->mu);
        json_t *d = json_obj();
        json_set(d, "messages", page);
        if (start > 0) {
            char *c = xasprintf("%lld", (long long)start);
            json_set_str(d, "nextCursor", c);
            free(c);
        } else json_set(d, "nextCursor", json_null());
        json_set_bool(d, "hasMore", start > 0);
        json_set_int(d, "total", total);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_available_thinking_levels")) {
        json_t *lv = json_arr();
        for (int i = EFFORT_OFF; i <= EFFORT_MAX; i++)
            if (model_supports_effort(a->model, (ai_effort_t)i)) json_push_str(lv, effort_name((ai_effort_t)i));
        json_t *d = json_obj();
        json_set(d, "levels", lv);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "cycle_thinking_level")) {
        /* cycleThinkingLevel: off -> auto -> supported efforts -> off */
        if (!a->model || !a->model->reasoning) {
            respond(r, id, type, true, json_null(), NULL);
        } else {
            bool auto_ok = !json_get_bool(a->model->thinking, "unverified", false);
            if (a->auto_thinking) {
                int e = EFFORT_MINIMAL;
                while (e <= EFFORT_MAX && !model_supports_effort(a->model, (ai_effort_t)e)) e++;
                app_set_thinking(a, e > EFFORT_MAX ? EFFORT_OFF : (ai_effort_t)e);
            } else if (a->thinking == EFFORT_OFF && auto_ok) {
                app_set_thinking_auto(a);
            } else {
                int cur = (int)a->thinking, next = cur;
                for (int k = 1; k <= EFFORT_MAX + 1; k++) {
                    int c = (cur + k) % (EFFORT_MAX + 1);
                    if (model_supports_effort(a->model, (ai_effort_t)c)) {
                        next = c;
                        break;
                    }
                }
                app_set_thinking(a, (ai_effort_t)next);
            }
            json_t *d = json_obj();
            json_set_str(d, "level", a->auto_thinking ? "auto" : effort_name(a->thinking));
            respond(r, id, type, true, d, NULL);
        }
    } else if (!strcmp(type, "cycle_model")) {
        /* cycle through settings.enabledModels (scoped) or models with credentials */
        json_t *scope = settings_get("enabledModels");
        strvec_t cands = {0};
        json_t *x;
        if (json_len(scope)) {
            json_arr_foreach(scope, i, x) if (json_is_str(x)) strvec_push(&cands, json_sv(x));
        } else {
            for (size_t i = 0; i < catalog_count() && cands.n < 200; i++) {
                const ai_model_t *m = catalog_at(i);
                if (!auth_has_credentials(m->provider)) continue;
                char *l = xasprintf("%s/%s", m->provider, m->id);
                strvec_push_own(&cands, l);
            }
        }
        char *curl = app_model_label(a);
        size_t at = 0;
        for (size_t i = 0; i < cands.n; i++)
            if (!strcmp(cands.v[i], curl)) at = i + 1;
        char *err = NULL;
        if (cands.n < 2 || app_set_model(a, cands.v[at % cands.n], &err) != 0) {
            respond(r, id, type, true, json_null(), NULL);
        } else {
            json_t *d = json_obj(), *m = json_obj();
            json_set_str(m, "provider", a->model->provider);
            json_set_str(m, "id", a->model->id);
            json_set_str(m, "name", a->model->name);
            json_set(d, "model", m);
            json_set_str(d, "thinkingLevel", effort_name(a->thinking));
            json_set_bool(d, "isScoped", json_len(scope) > 0);
            respond(r, id, type, true, d, NULL);
        }
        free(err);
        free(curl);
        strvec_free(&cands);
    } else if (!strcmp(type, "export_html")) {
        /* session.exportToHtml(outputPath): the path as written */
        char *written = NULL, *err = NULL;
        if (app_export_html(a, json_get_str(cmd, "outputPath"), false, &written, &err) != 0)
            respond(r, id, type, false, NULL, err ? err : "export failed");
        else {
            json_t *d = json_obj();
            json_set_str(d, "path", written);
            respond(r, id, type, true, d, NULL);
        }
        free(written);
        free(err);
    } else if (!strcmp(type, "handoff")) {
        if (r->running) respond(r, id, type, false, NULL, "agent is busy");
        else {
            char *err = NULL;
            if (app_handoff(a, json_get_str(cmd, "customInstructions"), &err) != 0) respond(r, id, type, false, NULL, err ? err : "handoff failed");
            else respond(r, id, type, true, json_obj(), NULL);
            free(err);
        }
    } else if (!strcmp(type, "btw")) {
        /* Side questions (/btw): answers stream as btw_delta / btw_record frames */
        char *err = NULL;
        json_t *rec = btw_ask(r, json_get_str(cmd, "question"), json_get_str(cmd, "recordId"), &err);
        if (!rec) respond(r, id, type, false, NULL, err ? err : "btw failed");
        else {
            json_t *d = json_obj();
            json_set(d, "record", rec);
            respond(r, id, type, true, d, NULL);
        }
        free(err);
    } else if (!strcmp(type, "btw_cancel")) {
        pthread_mutex_lock(&r->btw_mu);
        bool c = btw_cancel_locked(r, json_get_str(cmd, "recordId"));
        pthread_mutex_unlock(&r->btw_mu);
        json_t *d = json_obj();
        json_set_bool(d, "cancelled", c);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_btw_history")) {
        char *err = NULL;
        json_t *recs = btw_history(r, &err);
        if (!recs) respond(r, id, type, false, NULL, err ? err : "btw history unavailable");
        else {
            json_t *d = json_obj();
            json_set(d, "records", recs);
            respond(r, id, type, true, d, NULL);
        }
        free(err);
    } else if (!strcmp(type, "abort_and_restore_queue")) {
        json_t *d = json_obj();
        pthread_mutex_lock(&a->agent->mu);
        json_t *st = a->agent->steering ? a->agent->steering : json_arr();
        json_t *fu = a->agent->follow_up ? a->agent->follow_up : json_arr();
        json_t *sr = json_arr(), *fr = json_arr(), *m;
        json_arr_foreach(st, i, m) {
            json_t *x = json_obj();
            char *t = ai_message_text(m);
            json_set_str(x, "text", t);
            free(t);
            json_push(sr, x);
        }
        json_arr_foreach(fu, i, m) {
            json_t *x = json_obj();
            char *t = ai_message_text(m);
            json_set_str(x, "text", t);
            free(t);
            json_push(fr, x);
        }
        pthread_mutex_unlock(&a->agent->mu);
        agent_clear_queues(a->agent);
        app_abort(a);
        json_set(d, "steering", sr);
        json_set(d, "followUp", fr);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "remove_queued_message") || !strcmp(type, "promote_queued_message")) {
        const char *msg = json_get_str(cmd, "message");
        bool promote = !strcmp(type, "promote_queued_message");
        bool from_follow = promote || str_eq(json_get_str(cmd, "queue"), "followUp");
        bool done = false;
        pthread_mutex_lock(&a->agent->mu);
        json_t *q = from_follow ? a->agent->follow_up : a->agent->steering;
        for (size_t i = 0; q && i < json_len(q) && !done; i++) {
            char *t = ai_message_text(json_at(q, i));
            if (str_eq(t, msg)) {
                json_t *m = json_arr_remove(q, i);
                if (promote) {
                    if (!a->agent->steering) a->agent->steering = json_arr();
                    json_push(a->agent->steering, m);
                } else json_free(m);
                done = true;
            }
            free(t);
        }
        pthread_mutex_unlock(&a->agent->mu);
        json_t *d = json_obj();
        json_set_bool(d, promote ? "promoted" : "removed", done);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "set_auto_compaction") || !strcmp(type, "set_auto_retry")) {
        settings_set_runtime(!strcmp(type, "set_auto_compaction") ? "compaction.enabled" : "retry.enabled",
                             json_bool(json_get_bool(cmd, "enabled", true)));
        respond(r, id, type, true, NULL, NULL);
    } else if (!strcmp(type, "abort_retry") || !strcmp(type, "abort_bash") || !strcmp(type, "abort_branch_summary")) {
        if (!strcmp(type, "abort_retry")) app_abort(a);
        if (!strcmp(type, "abort_branch_summary")) app_abort_branch_summary(a);
        respond(r, id, type, true, NULL, NULL);
    } else if (!strcmp(type, "get_available_commands")) {
        /* buildAvailableSlashCommands: builtins with a text-mode handler, skill, MCP prompt and file commands */
        json_t *arr = slash_available_commands(a);
        json_t *d = json_obj();
        json_set(d, "commands", arr);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_login_providers")) {
        json_t *arr = json_arr();
        json_t *provs = auth_oauth_providers();
        json_t *p;
        json_arr_foreach(provs, i, p) {
            if (!json_get_obj(p, "login")) continue; /* env-only providers have no login flow */
            json_t *x = json_obj();
            json_set_str(x, "id", json_get_str(p, "id"));
            json_set_str(x, "name", json_get_str(p, "name") ? json_get_str(p, "name") : json_get_str(p, "id"));
            json_set_bool(x, "available", true);
            json_set_bool(x, "authenticated", auth_has_credentials(auth_store_id(json_get_str(p, "id"))));
            json_push(arr, x);
        }
        json_t *d = json_obj();
        json_set(d, "providers", arr);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "set_advisor")) {
        app_set_advisor(a, json_get_bool(cmd, "enabled", true));
        respond(r, id, type, true, NULL, NULL);
    } else if (!strcmp(type, "get_state")) {
        respond(r, id, type, true, state_json(a), NULL);
    } else if (!strcmp(type, "get_messages")) {
        json_t *d = json_obj();
        json_set(d, "messages", json_clone(a->agent->messages));
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_entries")) {
        json_t *d = json_obj();
        json_set(d, "entries", json_clone(a->session->entries));
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "set_model")) {
        const char *p = json_get_str(cmd, "provider"), *mid = json_get_str(cmd, "modelId");
        if (!mid) mid = json_get_str(cmd, "model");
        char *pat = p && mid ? xasprintf("%s/%s", p, mid) : xstrdup(mid ? mid : "");
        char *err = NULL;
        if (app_set_model(a, pat, &err) == 0) respond(r, id, type, true, state_json(a), NULL);
        else respond(r, id, type, false, NULL, err);
        free(err);
        free(pat);
    } else if (!strcmp(type, "set_thinking_level")) {
        if (app_set_thinking_level(a, json_get_str(cmd, "level"))) {
            respond(r, id, type, true, NULL, NULL);
        } else {
            respond(r, id, type, false, NULL, "invalid level");
        }
    } else if (!strcmp(type, "get_available_models")) {
        json_t *arr = json_arr();
        for (size_t i = 0; i < catalog_count(); i++) {
            const ai_model_t *m = catalog_at(i);
            if (!auth_has_credentials(m->provider)) continue;
            json_t *x = json_obj();
            json_set_str(x, "provider", m->provider);
            json_set_str(x, "id", m->id);
            json_set_str(x, "name", m->name);
            json_set_int(x, "contextWindow", m->context_window);
            json_set_bool(x, "reasoning", m->reasoning);
            json_push(arr, x);
        }
        json_t *d = json_obj();
        json_set(d, "models", arr);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "compact")) {
        char *err = NULL;
        if (r->running) respond(r, id, type, false, NULL, "agent is busy");
        else if (app_compact(a, json_get_str(cmd, "customInstructions"), &err) == 0) respond(r, id, type, true, NULL, NULL);
        else respond(r, id, type, false, NULL, err);
        free(err);
    } else if (!strcmp(type, "new_session")) {
        char *berr = NULL;
        if (r->running) respond(r, id, type, false, NULL, "agent is busy");
        else if (btw_close(r, &berr) != 0) respond(r, id, type, false, NULL, berr);
        else {
            app_new_session(a);
            respond(r, id, type, true, state_json(a), NULL);
        }
        free(berr);
    } else if (!strcmp(type, "switch_session") || !strcmp(type, "open_session")) {
        const char *p = json_get_str(cmd, "sessionPath");
        if (!p) p = json_get_str(cmd, "path");
        char *err = NULL;
        if (r->running) respond(r, id, type, false, NULL, "agent is busy");
        else if (btw_close(r, &err) != 0) respond(r, id, type, false, NULL, err);
        else if (p && app_switch_session(a, p, &err) == 0) respond(r, id, type, true, state_json(a), NULL);
        else respond(r, id, type, false, NULL, err ? err : "sessionPath required");
        free(err);
    } else if (!strcmp(type, "fork") || !strcmp(type, "branch")) {
        char *err = NULL;
        if (btw_close(r, &err) != 0) respond(r, id, type, false, NULL, err);
        else if (app_fork_from(a, json_get_str(cmd, "entryId"), &err) == 0) respond(r, id, type, true, state_json(a), NULL);
        else respond(r, id, type, false, NULL, err);
        free(err);
    } else if (!strcmp(type, "set_session_name") || !strcmp(type, "set_title")) {
        session_set_title(a->session, json_get_str(cmd, "name") ? json_get_str(cmd, "name") : json_get_str(cmd, "title"), "user");
        respond(r, id, type, true, NULL, NULL);
    } else if (!strcmp(type, "bash")) {
        json_t *m = app_user_bash(a, json_get_str(cmd, "command"), json_get_bool(cmd, "excludeFromContext", false));
        if (m) {
            json_t *d = json_obj();
            static const char *keys[] = {"output", "exitCode", "cancelled", "truncated", "totalLines", "totalBytes",
                                         "outputLines", "outputBytes"};
            for (size_t i = 0; i < ARRAY_LEN(keys); i++)
                if (json_get(m, keys[i])) json_set(d, keys[i], json_clone(json_get(m, keys[i])));
            respond(r, id, type, true, d, NULL);
            json_free(m);
        } else {
            respond(r, id, type, false, NULL, "command required");
        }
    } else if (!strcmp(type, "get_session_stats")) {
        respond(r, id, type, true, app_stats(a), NULL);
    } else if (!strcmp(type, "set_steering_mode") || !strcmp(type, "set_follow_up_mode") || !strcmp(type, "set_interrupt_mode")) {
        const char *key = !strcmp(type, "set_steering_mode") ? "steeringMode" : !strcmp(type, "set_follow_up_mode") ? "followUpMode" : "interruptMode";
        settings_set_runtime(key, json_str(json_get_str(cmd, "mode")));
        app_sync_queue_modes(a);
        respond(r, id, type, true, NULL, NULL);
    } else {
        respond(r, id, type, false, NULL, "unknown command");
    }
}

rpc_t *rpc_attach(app_t *a, rpc_sink_fn sink, void *ud) {
    rpc_t *r = xcalloc(1, sizeof(*r));
    r->a = a;
    r->sink = sink;
    r->sink_ud = ud;
    pthread_mutex_init(&r->mu, NULL);
    pthread_mutex_init(&r->filter_mu, NULL);
    pthread_mutex_init(&r->host_mu, NULL);
    pthread_cond_init(&r->host_cv, NULL);
    pthread_mutex_init(&r->btw_mu, NULL);
    agent_subscribe(a->agent, rpc_listener, r);
    a->notice = notice_rpc;
    a->notice_ud = r;
    a->wake = rpc_wake;
    a->wake_ud = r;
    json_t *ready = json_parse_str("{\"type\":\"ready\",\"protocolVersion\":1,\"supportedProtocolVersions\":[1],"
                                   "\"maxFrameBytes\":1048576}");
    emit(r, ready);
    json_free(ready);
    return r;
}

void rpc_handle_json(rpc_t *r, json_t *cmd) {
    if (!json_is_obj(cmd)) respond(r, NULL, "parse", false, NULL, "malformed JSON");
    else handle(r, cmd);
}

void rpc_handle_line(rpc_t *r, const char *line) {
    char *t = str_trim(line);
    if (*t) {
        json_t *cmd = json_parse_str(t);
        rpc_handle_json(r, cmd);
        json_free(cmd);
    }
    free(t);
}

bool rpc_busy(rpc_t *r) {
    pthread_mutex_lock(&r->mu);
    bool b = r->running;
    pthread_mutex_unlock(&r->mu);
    return b;
}

void rpc_detach(rpc_t *r) {
    if (!r) return;
    app_t *a = r->a;
    a->wake = NULL;
    pthread_mutex_lock(&r->mu);
    bool running = r->running;
    pthread_mutex_unlock(&r->mu);
    if (running) agent_abort(a->agent);
    pthread_mutex_lock(&r->mu);
    pthread_t th = r->th;
    pthread_mutex_unlock(&r->mu);
    if (th) pthread_join(th, NULL);
    char *berr = NULL;
    btw_close(r, &berr);
    free(berr);
    if (r->btw_th_live) pthread_join(r->btw_th, NULL);
    btw_store_free(r->btw_store);
    free(r->btw_store_key);
    free(r->btw_starting_topic);
    json_free(r->btw_unsaved);
    pthread_mutex_destroy(&r->btw_mu);
    agent_unsubscribe(a->agent, rpc_listener, r);
    if (a->notice == notice_rpc) {
        a->notice = NULL;
        a->notice_ud = NULL;
    }
    for (size_t i = 0; i < r->host_tools.n; i++) agent_remove_tool(a->agent, r->host_tools.v[i]);
    strvec_free(&r->host_tools);
    vec_free(&r->host_calls);
    strvec_free(&r->filter);
    pthread_mutex_destroy(&r->mu);
    pthread_mutex_destroy(&r->filter_mu);
    pthread_mutex_destroy(&r->host_mu);
    pthread_cond_destroy(&r->host_cv);
    free(r);
}

int run_rpc_mode(app_t *a) {
    rpc_t *r = rpc_attach(a, NULL, NULL);
    sb_t line = SB_INIT;
    char buf[65536];
    for (;;) {
        ssize_t n = read(0, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                sb_putc(&line, buf[i]);
                continue;
            }
            rpc_handle_line(r, sb_cstr(&line));
            sb_clear(&line);
        }
    }
    /* stdin closed: let an in-flight run finish (scripted clients pipe commands then EOF) */
    pthread_mutex_lock(&r->mu);
    pthread_t th = r->th;
    pthread_mutex_unlock(&r->mu);
    if (th) pthread_join(th, NULL);
    pthread_mutex_lock(&r->mu);
    r->th = 0;
    r->running = false;
    pthread_mutex_unlock(&r->mu);
    rpc_detach(r);
    sb_free(&line);
    return 0;
}
