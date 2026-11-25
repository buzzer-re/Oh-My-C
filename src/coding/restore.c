/* restore.c - interrupted-turn recovery for `omc --restore` (omp session/exit-diagnostics.ts findTailIndex /
 * findUnfinishedTurn / hasRestorableTurn / createInterruptedToolResults / createInterruptedTurnAbortMessage /
 * restoreTerminatedBashJobs, read with assumeExit as AgentSession.restoreInterruptedTurn does). The branch's entries
 * are read like upstream reads them, so a session another omp or omc process wrote (eval runtime snapshots,
 * `session_exit` records) restores alike; omc itself records no exits. Subagent revival (task/restore.ts
 * restoreSubagents) is not ported. */
#include "coding/restore.h"

#include "session/trace.h"
#include "tools/jobs.h"
#include "tools/jobtext.h"

#include <ctype.h>
#include <math.h>

static bool is_type(const json_t *e, const char *t) { return str_eq(json_get_str(e, "type"), t); }
static const json_t *msg_of(const json_t *e) { return is_type(e, "message") ? json_get_obj(e, "message") : NULL; }
static bool has_role(const json_t *m, const char *r) { return str_eq(json_get_str(m, "role"), r); }

#define EVAL_STATE_CONTEXT_TYPE "eval-state-context"

/* isEvalStateContext: an eval runtime snapshot, context for the next request rather than part of a turn */
static bool is_eval_state(const json_t *m) { return has_role(m, "custom") && str_eq(json_get_str(m, "customType"), EVAL_STATE_CONTEXT_TYPE); }

/* isEvalSnapshotEntry: a persisted snapshot in either entry form */
static bool is_eval_snapshot_entry(const json_t *e) {
    if (is_type(e, "message")) return is_eval_state(msg_of(e));
    return is_type(e, "custom_message") && str_eq(json_get_str(e, "customType"), EVAL_STATE_CONTEXT_TYPE);
}

/* isPendingToolCallDiagnostic */
static bool pending_diag_ok(const json_t *v) {
    if (!json_is_obj(v) || !json_is_str(json_get(v, "toolName"))) return false;
    const json_t *x;
    if ((x = json_get(v, "toolCallId")) && !json_is_str(x)) return false;
    if ((x = json_get(v, "intent")) && !json_is_str(x)) return false;
    if ((x = json_get(v, "assistantTimestamp")) && !json_is_num(x)) return false;
    if ((x = json_get(v, "startedAt")) && !json_is_str(x)) return false;
    return true;
}

typedef struct {
    const char *kind;      /* normal | signal | fatal | process_exit */
    const char *recorded;  /* recordedAt (ISO) */
    bool pending;          /* pendingToolCalls is a valid, non-empty list */
} session_exit_t;

/* readSessionExit: a `session_exit` custom entry's record */
static bool read_exit(const json_t *e, session_exit_t *out) {
    if (!is_type(e, "custom") || !str_eq(json_get_str(e, "customType"), "session_exit")) return false;
    const json_t *d = json_get_obj(e, "data");
    const char *kind = json_get_str(d, "kind");
    if (!json_get_str(d, "reason") || !json_get_str(d, "recordedAt") ||
        !(str_eq(kind, "normal") || str_eq(kind, "signal") || str_eq(kind, "fatal") || str_eq(kind, "process_exit")))
        return false;
    out->kind = kind;
    out->recorded = json_get_str(d, "recordedAt");
    const json_t *p = json_get(d, "pendingToolCalls"), *x;
    bool ok = json_is_arr(p);
    json_arr_foreach((json_t *)p, i, x) ok = ok && pending_diag_ok(x);
    out->pending = ok && json_len(p) > 0;
    return true;
}

/* isToolCallContent */
static bool is_tool_call(const json_t *b) {
    return str_eq(json_get_str(b, "type"), "toolCall") && (json_is_str(json_get(b, "name")) || json_is_str(json_get(b, "id")));
}

static bool has_tool_calls(const json_t *msg) {
    json_t *b;
    json_arr_foreach(json_get(msg, "content"), i, b) if (is_tool_call(b)) return true;
    return false;
}

/* isInterruptedTurnAbort */
static bool is_interrupted_abort(const json_t *m) {
    return has_role(m, "assistant") && str_eq(json_get_str(m, "stopReason"), "aborted") &&
           str_eq(json_get_str(m, "errorMessage"), INTERRUPTED_TURN_ERROR);
}

/* findTailIndex: the branch's last `message` entry, or -1. An eval snapshot that follows a finished answer (an assistant
 * `stop`) with nothing after it that could start a turn is not the tail: an idle context rebuild persists one after a
 * finished turn without starting another (omp 97c79a0673). Any other snapshot stays the tail, and so does one with a
 * custom message prompt (sent to the model, not display-only) before or after it (omp 2da0223c61). */
static ssize_t find_tail(json_t *const *ents, size_t n) {
    ssize_t snapshot = -1;
    bool custom_prompt = false;
    for (size_t i = n; i-- > 0;) {
        const json_t *e = ents[i];
        if (is_type(e, "custom_message")) {
            if (!str_eq(json_get_str(e, "customType"), EVAL_STATE_CONTEXT_TYPE) && !json_get_bool(e, "excludeFromContext", false))
                custom_prompt = true;
            continue;
        }
        const json_t *m = msg_of(e);
        if (!m) continue;
        if (is_eval_state(m)) {
            if (snapshot < 0) snapshot = (ssize_t)i;
            continue;
        }
        bool finished = has_role(m, "assistant") && str_eq(json_get_str(m, "stopReason"), "stop");
        return snapshot >= 0 && (custom_prompt || !finished) ? snapshot : (ssize_t)i;
    }
    return snapshot;
}

typedef struct {
    const json_t *tail, *prev_assistant;
    char *recorded; /* owned ISO time of the (assumed) exit */
} unfinished_t;

/* findUnfinishedTurn with assumeExit: a non-terminal tail (a prompt or tool results the model never answered, tool calls
 * without results) whose writer is gone; a recorded normal exit with nothing pending, a terminal assistant tail and tool
 * results that close an aborted or failed turn leave nothing unfinished */
static bool find_unfinished(json_t *const *ents, size_t n, unfinished_t *out) {
    memset(out, 0, sizeof *out);
    ssize_t exit_at = -1;
    session_exit_t ex = {0};
    for (size_t i = n; i-- > 0;)
        if (read_exit(ents[i], &ex)) {
            exit_at = (ssize_t)i;
            break;
        }
    ssize_t tail_at = find_tail(ents, n);
    const json_t *tail = tail_at >= 0 ? msg_of(ents[tail_at]) : NULL;
    if (!tail) return false;
    bool assumed = exit_at < 0 || tail_at > exit_at;
    if (!assumed && str_eq(ex.kind, "normal") && !ex.pending) return false;
    if (has_role(tail, "assistant") && !has_tool_calls(tail)) return false;
    const json_t *prev = NULL;
    for (ssize_t i = tail_at; i >= 0 && !prev; i--) {
        const json_t *m = msg_of(ents[i]);
        if (has_role(m, "assistant")) prev = m;
    }
    const char *pstop = json_get_str(prev, "stopReason");
    if (has_role(tail, "toolResult") && (str_eq(pstop, "error") || str_eq(pstop, "aborted"))) return false;
    out->tail = tail;
    out->prev_assistant = prev;
    out->recorded = assumed ? NULL : xstrdup(ex.recorded);
    return true;
}

static bool has_restorable(json_t *const *ents, size_t n);

/* isFalseAbortAfterSnapshot: an interrupted-turn abort written by exit recovery (no completedAt, which every live
 * assistant message gets) of a release that read a trailing idle eval snapshot as an unfinished tail */
static bool false_abort_after_snapshot(json_t *const *ents, size_t abort_at) {
    const json_t *abort = msg_of(ents[abort_at]);
    if (!abort || !has_role(abort, "assistant") || json_get(abort, "completedAt")) return false;
    for (size_t i = abort_at; i-- > 0;) {
        const json_t *e = ents[i];
        if (!is_type(e, "message") && !is_type(e, "custom_message")) continue;
        if (!is_eval_snapshot_entry(e)) return false;
        return !has_restorable(ents, i + 1);
    }
    return false;
}

/* hasRestorableTurn: a turn the last process left unfinished, or one exit recovery already closed that nothing followed
 * (not even a normal exit), unless that abort only closed an idle eval snapshot */
static bool has_restorable(json_t *const *ents, size_t n) {
    ssize_t tail_at = find_tail(ents, n);
    session_exit_t ex;
    for (size_t i = n; i-- > 0 && (ssize_t)i > tail_at;)
        if (read_exit(ents[i], &ex) && str_eq(ex.kind, "normal")) return false;
    const json_t *tail = tail_at >= 0 ? msg_of(ents[tail_at]) : NULL;
    if (tail && is_interrupted_abort(tail)) return !false_abort_after_snapshot(ents, (size_t)tail_at);
    unfinished_t u;
    bool r = find_unfinished(ents, n, &u);
    free(u.recorded);
    return r;
}

/* createInterruptedToolResults (process-exit): the latest assistant's calls still pending at the branch's end, when the
 * last message is that assistant or one of its tool results */
static json_t *interrupted_tool_results(json_t *const *ents, size_t n) {
    json_t *out = json_arr();
    const json_t *tail = NULL, *assistant = NULL;
    for (size_t i = n; i-- > 0 && !assistant;) {
        const json_t *m = msg_of(ents[i]);
        if (!m) continue;
        if (!tail) tail = m;
        if (has_role(m, "assistant")) assistant = m;
    }
    if (!assistant || (!has_role(tail, "assistant") && !has_role(tail, "toolResult"))) return out;
    /* collectPendingToolCalls: each assistant replaces the pending set, each result settles its call */
    strvec_t pending = {0};
    for (size_t i = 0; i < n; i++) {
        const json_t *m = msg_of(ents[i]);
        if (!m) continue;
        if (has_role(m, "toolResult")) {
            const char *id = json_get_str(m, "toolCallId");
            for (size_t k = 0; id && k < pending.n; k++)
                if (str_eq(pending.v[k], id)) {
                    free(pending.v[k]);
                    memmove(pending.v + k, pending.v + k + 1, (pending.n - k - 1) * sizeof(char *));
                    pending.n--;
                    break;
                }
        } else if (has_role(m, "assistant")) {
            strvec_free(&pending);
            json_t *b;
            json_arr_foreach(json_get(m, "content"), k, b) if (is_tool_call(b) && json_get_str(b, "id")) strvec_push(&pending, json_get_str(b, "id"));
        }
    }
    json_t *b;
    json_arr_foreach(json_get(assistant, "content"), i, b) {
        if (!str_eq(json_get_str(b, "type"), "toolCall") || !json_get_str(b, "id") || !strvec_contains(&pending, json_get_str(b, "id")))
            continue;
        json_t *r = json_obj();
        json_set_str(r, "role", "toolResult");
        json_set_str(r, "toolCallId", json_get_str(b, "id"));
        json_set_str(r, "toolName", json_get_str(b, "name") ? json_get_str(b, "name") : "");
        json_t *content = json_arr();
        json_push(content, text_block("Previous OMC process exited before this tool returned; its outcome is unknown."));
        json_set(r, "content", content);
        json_set_bool(r, "isError", true);
        json_set_num(r, "timestamp", (double)now_ms());
        json_push(out, r);
    }
    strvec_free(&pending);
    return out;
}

/* createInterruptedTurnAbortMessage */
static json_t *interrupted_abort(const unfinished_t *u, const ai_model_t *fallback) {
    const json_t *prev = u->prev_assistant;
    if (!prev && !fallback) return NULL;
    json_t *abort = json_obj();
    json_set_str(abort, "role", "assistant");
    json_set(abort, "content", json_arr());
    json_set_str(abort, "api", prev ? json_get_str(prev, "api") : fallback->api);
    json_set_str(abort, "provider", prev ? json_get_str(prev, "provider") : fallback->provider);
    json_set_str(abort, "model", prev ? json_get_str(prev, "model") : fallback->id);
    json_t *usage = json_obj(), *cost = json_obj();
    static const char *const U[] = {"input", "output", "cacheRead", "cacheWrite", "totalTokens"};
    for (size_t i = 0; i < ARRAY_LEN(U); i++) json_set_num(usage, U[i], 0);
    static const char *const C[] = {"input", "output", "cacheRead", "cacheWrite", "total"};
    for (size_t i = 0; i < ARRAY_LEN(C); i++) json_set_num(cost, C[i], 0);
    json_set(usage, "cost", cost);
    json_set(abort, "usage", usage);
    json_set_str(abort, "stopReason", "aborted");
    json_set_str(abort, "errorMessage", INTERRUPTED_TURN_ERROR);
    double at = 0; /* Date.parse(recordedAt), now when not finite */
    bool parsed = u->recorded && trace_parse_iso_ms(u->recorded, &at) && isfinite(at);
    json_set_num(abort, "timestamp", parsed ? at : (double)now_ms());
    return abort;
}

bool restore_branch_restorable(const json_t *entries) {
    size_t n = json_len(entries);
    json_t **ents = n ? xcalloc(n, sizeof *ents) : NULL;
    for (size_t i = 0; i < n; i++) ents[i] = json_at(entries, i);
    bool r = has_restorable(ents, n);
    free(ents);
    return r;
}

bool app_restore_interrupted_turn(app_t *a, json_t **branch) {
    if (branch) *branch = NULL;
    if (agent_is_running(a->agent) || !a->session) return false;
    json_t *path = session_current_path(a->session);
    size_t n = json_len(path);
    json_t **ents = n ? xcalloc(n, sizeof *ents) : NULL;
    for (size_t i = 0; i < n; i++) ents[i] = json_at(path, i);
    bool ok = has_restorable(ents, n) && a->model;
    unfinished_t u;
    if (ok && find_unfinished(ents, n, &u)) {
        json_t *abort = interrupted_abort(&u, a->model);
        if (abort) {
            json_t *add = interrupted_tool_results(ents, n), *m;
            json_push(add, abort);
            json_arr_foreach(add, i, m) {
                session_append_message(a->session, m);
                agent_append_message(a->agent, json_clone(m));
            }
            json_free(add);
        }
        free(u.recorded);
    }
    free(ents);
    if (ok && branch) *branch = path;
    else json_free(path);
    return ok;
}

/* ---------- background bash jobs (restoreTerminatedBashJobs) ---------- */

typedef struct {
    strvec_t ids, labels; /* running jobs in first-seen order (a Map: re-adding keeps the place) */
} running_t;

static ssize_t running_find(const running_t *r, const char *id) {
    for (size_t i = 0; i < r->ids.n; i++)
        if (str_eq(r->ids.v[i], id)) return (ssize_t)i;
    return -1;
}

static void running_set(running_t *r, const char *id, const char *label) {
    ssize_t at = running_find(r, id);
    if (at >= 0) {
        free(r->labels.v[at]);
        r->labels.v[at] = xstrdup(label);
        return;
    }
    strvec_push(&r->ids, id);
    strvec_push(&r->labels, label);
}

static void running_delete(running_t *r, const char *id) {
    ssize_t at = running_find(r, id);
    if (at < 0) return;
    free(r->ids.v[at]);
    free(r->labels.v[at]);
    memmove(r->ids.v + at, r->ids.v + at + 1, (r->ids.n - (size_t)at - 1) * sizeof(char *));
    memmove(r->labels.v + at, r->labels.v + at + 1, (r->labels.n - (size_t)at - 1) * sizeof(char *));
    r->ids.n--;
    r->labels.n--;
}

static bool settled_status(const char *s) { return str_eq(s, "completed") || str_eq(s, "failed") || str_eq(s, "cancelled"); }

/* forgetSettledJob */
static void forget_job(const json_t *v, running_t *r, bool delivered) {
    if (!json_is_obj(v)) return;
    const char *id = json_get_str(v, "id") ? json_get_str(v, "id") : json_get_str(v, "jobId");
    if (id && (delivered || settled_status(json_get_str(v, "status")))) running_delete(r, id);
}

/* forgetSettledJobDetails: rows a delivery or a wait/cancel/proc snapshot already settled */
static void forget_details(const json_t *d, running_t *r, bool delivered) {
    if (!json_is_obj(d)) return;
    const json_t *x;
    json_arr_foreach(json_get_arr(d, "jobs"), i, x) forget_job(x, r, delivered);
    if (delivered) forget_job(d, r, true);
    const json_t *proc = json_get_obj(d, "proc");
    if (!proc) return;
    json_arr_foreach(json_get_arr(proc, "jobs"), i, x) forget_job(x, r, false);
    forget_job(json_get(proc, "job"), r, false);
}

json_t *restore_unsettled_bash_jobs(const json_t *branch) {
    strvec_t call_ids = {0}, call_labels = {0};
    running_t run = {{0}, {0}};
    const json_t *e;
    json_arr_foreach((json_t *)branch, i, e) {
        if (is_type(e, "custom_message")) {
            if (str_eq(json_get_str(e, "customType"), "async-result")) forget_details(json_get(e, "details"), &run, true);
            continue;
        }
        const json_t *m = msg_of(e);
        if (!m) continue;
        if (has_role(m, "assistant")) {
            const json_t *b;
            json_arr_foreach(json_get(m, "content"), k, b) {
                const json_t *args = json_get(b, "arguments");
                if (!str_eq(json_get_str(b, "type"), "toolCall") || !str_eq(json_get_str(b, "name"), "bash") || !json_is_obj(args))
                    continue;
                const char *cmd = json_get_str(args, "command");
                if (!cmd) continue;
                char *label = job_format_label(cmd);
                if (*label && json_get_str(b, "id")) {
                    strvec_push(&call_ids, json_get_str(b, "id"));
                    strvec_push_own(&call_labels, label);
                } else {
                    free(label);
                }
            }
            continue;
        }
        if (has_role(m, "toolResult")) {
            const json_t *d = json_get(m, "details"), *as = json_get_obj(d, "async");
            if (str_eq(json_get_str(m, "toolName"), "bash") && as && str_eq(json_get_str(as, "type"), "bash") && json_get_str(as, "jobId")) {
                const char *jid = json_get_str(as, "jobId"), *state = json_get_str(as, "state");
                if (str_eq(state, "running")) {
                    const char *label = jid, *tcid = json_get_str(m, "toolCallId");
                    for (size_t k = call_ids.n; tcid && k-- > 0;) /* Map#get: the latest call with that id */
                        if (str_eq(call_ids.v[k], tcid)) {
                            label = call_labels.v[k];
                            break;
                        }
                    running_set(&run, jid, label);
                } else if (settled_status(state)) {
                    running_delete(&run, jid);
                }
            }
            forget_details(d, &run, false);
            continue;
        }
        if (has_role(m, "custom") && str_eq(json_get_str(m, "customType"), "async-result")) forget_details(json_get(m, "details"), &run, true);
    }
    json_t *out = json_arr();
    for (size_t i = 0; i < run.ids.n; i++) {
        json_t *o = json_obj();
        json_set_str(o, "id", run.ids.v[i]);
        json_set_str(o, "label", run.labels.v[i]);
        json_push(out, o);
    }
    strvec_free(&run.ids);
    strvec_free(&run.labels);
    strvec_free(&call_ids);
    strvec_free(&call_labels);
    return out;
}

void app_restore_terminated_jobs(app_t *a, const json_t *branch) {
    if (!a->env || !a->env->jobs) return;
    json_t *jobs = restore_unsettled_bash_jobs(branch), *x;
    json_arr_foreach(jobs, i, x) {
        const char *id = json_get_str(x, "id");
        json_t *known = jobs_get(a->env->jobs, id);
        if (known) {
            json_free(known);
            continue;
        }
        char *text = xasprintf("Previous OMC process exited before background job %s delivered its final result; its outcome is unknown. "
                               "Its subprocess may still be running or may have partially completed \xe2\x80\x94 verify before rerunning.",
                               id);
        free(jobs_publish_failed(a->env->jobs, "bash", id, json_get_str(x, "label"), text));
        free(text);
    }
    json_free(jobs);
}
