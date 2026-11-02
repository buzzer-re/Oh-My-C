/* restore.c - interrupted-turn recovery for `omc --restore` (omp session/exit-diagnostics.ts with assumeExit:
 * omc records no session_exit markers, so the branch's writer is always taken as gone). Subagent revival
 * (task/restore.ts restoreSubagents) is not ported. */
#include "coding/restore.h"

static bool is_tool_call(const json_t *b) {
    return str_eq(json_get_str(b, "type"), "toolCall") && (json_get_str(b, "name") || json_get_str(b, "id"));
}

static bool has_tool_calls(const json_t *msg) {
    json_t *b;
    json_arr_foreach(json_get(msg, "content"), i, b) if (is_tool_call(b)) return true;
    return false;
}

/* isInterruptedTurnAbort */
static bool is_interrupted_abort(const json_t *m) {
    return str_eq(json_get_str(m, "role"), "assistant") && str_eq(json_get_str(m, "stopReason"), "aborted") &&
           str_eq(json_get_str(m, "errorMessage"), INTERRUPTED_TURN_ERROR);
}

bool app_restore_interrupted_turn(app_t *a) {
    if (!a->model || agent_is_running(a->agent)) return false;
    pthread_mutex_lock(&a->agent->mu);
    json_t *msgs = json_clone(a->agent->messages);
    pthread_mutex_unlock(&a->agent->mu);
    size_t n = json_len(msgs);
    bool ok = false;
    json_t *tail = n ? json_at(msgs, n - 1) : NULL;
    const char *role = json_get_str(tail, "role");
    /* hasRestorableTurn: an abort record recovery already wrote, with nothing after it, goes on as well */
    if (tail && is_interrupted_abort(tail)) {
        json_free(msgs);
        return true;
    }
    /* findUnfinishedTurn: a terminal assistant tail leaves nothing unfinished */
    if (!tail || (str_eq(role, "assistant") && !has_tool_calls(tail))) goto done;
    const json_t *prev = NULL; /* the last assistant message (the tail itself when it is one) */
    for (size_t i = n; i > 0 && !prev; i--)
        if (str_eq(json_get_str(json_at(msgs, i - 1), "role"), "assistant")) prev = json_at(msgs, i - 1);
    const char *pstop = json_get_str(prev, "stopReason");
    if (str_eq(role, "toolResult") && (str_eq(pstop, "error") || str_eq(pstop, "aborted"))) goto done;
    /* createInterruptedToolResults: the latest assistant's calls that have no result */
    json_t *add = json_arr();
    if (prev && (str_eq(role, "assistant") || str_eq(role, "toolResult"))) {
        json_t *b;
        json_arr_foreach(json_get(prev, "content"), i, b) {
            if (!str_eq(json_get_str(b, "type"), "toolCall")) continue;
            const char *id = json_get_str(b, "id");
            bool answered = false;
            json_t *m;
            json_arr_foreach(msgs, k, m) {
                if (str_eq(json_get_str(m, "role"), "toolResult") && str_eq(json_get_str(m, "toolCallId"), id)) answered = true;
            }
            if (answered) continue;
            json_t *r = json_obj();
            json_set_str(r, "role", "toolResult");
            json_set_str(r, "toolCallId", id ? id : "");
            json_set_str(r, "toolName", json_get_str(b, "name") ? json_get_str(b, "name") : "");
            json_t *content = json_arr();
            json_push(content, text_block("Previous OMC process exited before this tool returned; its outcome is unknown."));
            json_set(r, "content", content);
            json_set_bool(r, "isError", true);
            json_set_num(r, "timestamp", (double)now_ms());
            json_push(add, r);
        }
    }
    /* createInterruptedTurnAbortMessage */
    json_t *abort = json_obj();
    json_set_str(abort, "role", "assistant");
    json_set(abort, "content", json_arr());
    json_set_str(abort, "api", prev && json_get_str(prev, "api") ? json_get_str(prev, "api") : a->model->api);
    json_set_str(abort, "provider", prev && json_get_str(prev, "provider") ? json_get_str(prev, "provider") : a->model->provider);
    json_set_str(abort, "model", prev && json_get_str(prev, "model") ? json_get_str(prev, "model") : a->model->id);
    json_t *usage = json_obj(), *cost = json_obj();
    static const char *const U[] = {"input", "output", "cacheRead", "cacheWrite", "totalTokens"};
    for (size_t i = 0; i < ARRAY_LEN(U); i++) json_set_num(usage, U[i], 0);
    static const char *const C[] = {"input", "output", "cacheRead", "cacheWrite", "total"};
    for (size_t i = 0; i < ARRAY_LEN(C); i++) json_set_num(cost, C[i], 0);
    json_set(usage, "cost", cost);
    json_set(abort, "usage", usage);
    json_set_str(abort, "stopReason", "aborted");
    json_set_str(abort, "errorMessage", INTERRUPTED_TURN_ERROR);
    json_set_num(abort, "timestamp", (double)now_ms());
    json_push(add, abort);
    json_t *m;
    json_arr_foreach(add, i, m) {
        if (a->session) session_append_message(a->session, m);
        agent_append_message(a->agent, json_clone(m));
    }
    json_free(add);
    ok = true;
done:
    json_free(msgs);
    return ok;
}
