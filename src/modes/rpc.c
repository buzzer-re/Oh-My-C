/* rpc mode: JSONL commands on stdin, responses + agent events on stdout (omp RPC protocol subset). */
#include "modes.h"
#include "coding/goal.h"

#include "config/auth.h"
#include "config/settings.h"
#include "core/proc.h"

#include <errno.h>
#include <pthread.h>
#include <unistd.h>

typedef struct {
    app_t *a;
    pthread_t th;
    bool running;
    char *prompt_id;
    char *text;
    json_t *images;
    pthread_mutex_t mu;
} rpc_t;

static void respond(const char *id, const char *cmd, bool ok, json_t *data, const char *err) {
    json_t *r = json_obj();
    if (id) json_set_str(r, "id", id);
    json_set_str(r, "type", "response");
    json_set_str(r, "command", cmd ? cmd : "unknown");
    json_set_bool(r, "success", ok);
    if (data) json_set(r, "data", data);
    if (err) json_set_str(r, "error", err);
    out_json_line(r);
    json_free(r);
}

static void rpc_listener(void *ud, const agent_event_t *ev) {
    UNUSED(ud);
    json_t *j = agent_event_to_json(ev);
    if (ev->type == AGENT_EV_AGENT_END) {
        json_set_bool(j, "isTerminal", true);
        json_set_bool(j, "yielded", true);
    }
    out_json_line(j);
    json_free(j);
}

static void notice_rpc(void *ud, const char *level, const char *text) {
    UNUSED(ud);
    json_t *j = json_obj();
    json_set_str(j, "type", "notice");
    json_set_str(j, "level", level);
    json_set_str(j, "message", text);
    out_json_line(j);
    json_free(j);
}

static void *prompt_thread(void *p) {
    rpc_t *r = p;
    size_t before = json_len(r->a->agent->messages);
    app_prompt(r->a, r->text, r->images);
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
    for (;;) {
        json_t *run = json_arr();
        pthread_mutex_lock(&r->a->agent->mu);
        for (size_t i = before; i < json_len(r->a->agent->messages); i++) json_push(run, json_clone(json_at(r->a->agent->messages, i)));
        pthread_mutex_unlock(&r->a->agent->mu);
        char *cont = app_goal_after_run(r->a, run, was_cont, "rpc");
        json_free(run);
        if (!cont) break;
        if (!was_cont) {
            json_set_bool(res, "sessionSettled", false);
            out_json_line(res);
        }
        was_cont = true;
        before = json_len(r->a->agent->messages);
        app_prompt_hidden(r->a, cont, "goal-continuation");
        free(cont);
    }
    json_set_bool(res, "sessionSettled", true);
    if (!was_cont) out_json_line(res);
    json_free(res);
    json_t *settled = json_obj();
    json_set_str(settled, "type", "session_settled");
    out_json_line(settled);
    json_free(settled);
    pthread_mutex_lock(&r->mu);
    free(r->text);
    r->text = NULL;
    free(r->prompt_id);
    r->prompt_id = NULL;
    r->running = false;
    pthread_mutex_unlock(&r->mu);
    return NULL;
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

static void handle(rpc_t *r, json_t *cmd) {
    app_t *a = r->a;
    const char *id = json_get_str(cmd, "id");
    const char *type = json_get_str(cmd, "type");
    if (!type) {
        respond(id, "unknown", false, NULL, "missing type");
        return;
    }
    if (!strcmp(type, "negotiate_protocol")) {
        json_t *d = json_obj();
        json_set_int(d, "protocolVersion", 1);
        respond(id, type, true, d, NULL);
    } else if (!strcmp(type, "prompt") || !strcmp(type, "abort_and_prompt")) {
        const char *msg = json_get_str(cmd, "message");
        if (!strcmp(type, "abort_and_prompt") && r->running) {
            agent_abort(a->agent);
            pthread_join(r->th, NULL);
        }
        if (r->running) {
            const char *behav = json_get_str(cmd, "streamingBehavior");
            json_t *m = msg_user_content(content_with_images(msg, json_get_arr(cmd, "images")));
            if (str_eq(behav, "followUp")) agent_follow_up(a->agent, m);
            else agent_steer(a->agent, m);
            respond(id, type, true, NULL, NULL);
            return;
        }
        if (r->th) pthread_join(r->th, NULL);
        r->th = 0;
        r->running = true;
        r->text = xstrdup(msg ? msg : "");
        r->prompt_id = id ? xstrdup(id) : NULL;
        json_t *imgs = json_get_arr(cmd, "images");
        r->images = imgs ? json_clone(imgs) : NULL;
        respond(id, type, true, NULL, NULL);
        if (pthread_create(&r->th, NULL, prompt_thread, r) != 0) {
            r->running = false;
            respond(id, type, false, NULL, "failed to start agent thread");
        }
    } else if (!strcmp(type, "steer") || !strcmp(type, "follow_up")) {
        json_t *m = msg_user_content(content_with_images(json_get_str(cmd, "message"), json_get_arr(cmd, "images")));
        if (!strcmp(type, "steer")) agent_steer(a->agent, m);
        else agent_follow_up(a->agent, m);
        respond(id, type, true, NULL, NULL);
    } else if (!strcmp(type, "abort")) {
        agent_abort(a->agent);
        respond(id, type, true, NULL, NULL);
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
            respond(id, type, false, NULL, err ? err : "goal operation failed");
        } else {
            json_t *d = json_obj();
            json_set(d, "goal", goal_json(app_goal(a)));
            json_set(d, "state", goal_state_json(app_goal(a)));
            respond(id, type, true, d, NULL);
        }
        free(err);
    } else if (!strcmp(type, "get_state")) {
        respond(id, type, true, state_json(a), NULL);
    } else if (!strcmp(type, "get_messages")) {
        json_t *d = json_obj();
        json_set(d, "messages", json_clone(a->agent->messages));
        respond(id, type, true, d, NULL);
    } else if (!strcmp(type, "get_entries")) {
        json_t *d = json_obj();
        json_set(d, "entries", json_clone(a->session->entries));
        respond(id, type, true, d, NULL);
    } else if (!strcmp(type, "set_model")) {
        const char *p = json_get_str(cmd, "provider"), *mid = json_get_str(cmd, "modelId");
        if (!mid) mid = json_get_str(cmd, "model");
        char *pat = p && mid ? xasprintf("%s/%s", p, mid) : xstrdup(mid ? mid : "");
        char *err = NULL;
        if (app_set_model(a, pat, &err) == 0) respond(id, type, true, state_json(a), NULL);
        else respond(id, type, false, NULL, err);
        free(err);
        free(pat);
    } else if (!strcmp(type, "set_thinking_level")) {
        ai_effort_t e;
        if (effort_parse(json_get_str(cmd, "level"), &e) == 0) {
            app_set_thinking(a, e);
            respond(id, type, true, NULL, NULL);
        } else {
            respond(id, type, false, NULL, "invalid level");
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
        respond(id, type, true, d, NULL);
    } else if (!strcmp(type, "compact")) {
        char *err = NULL;
        if (r->running) respond(id, type, false, NULL, "agent is busy");
        else if (app_compact(a, json_get_str(cmd, "customInstructions"), &err) == 0) respond(id, type, true, NULL, NULL);
        else respond(id, type, false, NULL, err);
        free(err);
    } else if (!strcmp(type, "new_session")) {
        if (r->running) respond(id, type, false, NULL, "agent is busy");
        else {
            app_new_session(a);
            respond(id, type, true, state_json(a), NULL);
        }
    } else if (!strcmp(type, "switch_session") || !strcmp(type, "open_session")) {
        const char *p = json_get_str(cmd, "sessionPath");
        if (!p) p = json_get_str(cmd, "path");
        char *err = NULL;
        if (r->running) respond(id, type, false, NULL, "agent is busy");
        else if (p && app_switch_session(a, p, &err) == 0) respond(id, type, true, state_json(a), NULL);
        else respond(id, type, false, NULL, err ? err : "sessionPath required");
        free(err);
    } else if (!strcmp(type, "fork") || !strcmp(type, "branch")) {
        char *err = NULL;
        if (app_fork_from(a, json_get_str(cmd, "entryId"), &err) == 0) respond(id, type, true, state_json(a), NULL);
        else respond(id, type, false, NULL, err);
        free(err);
    } else if (!strcmp(type, "set_session_name") || !strcmp(type, "set_title")) {
        session_set_title(a->session, json_get_str(cmd, "name") ? json_get_str(cmd, "name") : json_get_str(cmd, "title"), "user");
        respond(id, type, true, NULL, NULL);
    } else if (!strcmp(type, "bash")) {
        const char *c = json_get_str(cmd, "command");
        proc_opts_t po = {.cwd = a->cwd, .timeout_ms = 300000, .merge_stderr = true, .new_session = true};
        proc_result_t pr;
        if (c && proc_shell(c, &po, &pr) == 0) {
            json_t *d = json_obj();
            json_set_str(d, "output", pr.out);
            json_set_int(d, "exitCode", pr.exit_code);
            json_set_bool(d, "cancelled", pr.cancelled);
            respond(id, type, true, d, NULL);
            proc_result_free(&pr);
        } else {
            respond(id, type, false, NULL, "command required");
        }
    } else if (!strcmp(type, "get_session_stats")) {
        respond(id, type, true, app_stats(a), NULL);
    } else if (!strcmp(type, "set_steering_mode") || !strcmp(type, "set_follow_up_mode") || !strcmp(type, "set_interrupt_mode")) {
        const char *key = !strcmp(type, "set_steering_mode") ? "steeringMode" : !strcmp(type, "set_follow_up_mode") ? "followUpMode" : "interruptMode";
        settings_set_runtime(key, json_str(json_get_str(cmd, "mode")));
        respond(id, type, true, NULL, NULL);
    } else {
        respond(id, type, false, NULL, "unknown command");
    }
}

int run_rpc_mode(app_t *a) {
    rpc_t r = {.a = a};
    pthread_mutex_init(&r.mu, NULL);
    agent_subscribe(a->agent, rpc_listener, NULL);
    a->notice = notice_rpc;
    json_t *ready = json_parse_str("{\"type\":\"ready\",\"protocolVersion\":1,\"supportedProtocolVersions\":[1],"
                                   "\"maxFrameBytes\":1048576}");
    out_json_line(ready);
    json_free(ready);
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
            char *t = str_trim(sb_cstr(&line));
            sb_clear(&line);
            if (*t) {
                json_t *cmd = json_parse_str(t);
                if (!json_is_obj(cmd)) respond(NULL, "parse", false, NULL, "malformed JSON");
                else handle(&r, cmd);
                json_free(cmd);
            }
            free(t);
        }
    }
    if (r.th) pthread_join(r.th, NULL);
    sb_free(&line);
    pthread_mutex_destroy(&r.mu);
    return 0;
}
