#include "agent.h"

#include <math.h>

/* ---------- tool results ---------- */

json_t *text_block(const char *text) {
    json_t *b = json_obj();
    json_set_str(b, "type", "text");
    json_set_str(b, "text", text ? text : "");
    return b;
}

json_t *image_block(const char *b64, const char *mime) {
    json_t *b = json_obj();
    json_set_str(b, "type", "image");
    json_set_str(b, "data", b64);
    json_set_str(b, "mimeType", mime ? mime : "image/png");
    return b;
}

void tool_result_text(tool_result_t *r, const char *text) {
    if (!r->content) r->content = json_arr();
    json_push(r->content, text_block(text));
}

void tool_result_textf(tool_result_t *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = xvasprintf(fmt, ap);
    va_end(ap);
    tool_result_text(r, s);
    free(s);
}

void tool_result_image(tool_result_t *r, const char *b64, const char *mime) {
    if (!r->content) r->content = json_arr();
    json_push(r->content, image_block(b64, mime));
}

void tool_result_error(tool_result_t *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = xvasprintf(fmt, ap);
    va_end(ap);
    tool_result_text(r, s);
    free(s);
    r->is_error = true;
}

void tool_result_free(tool_result_t *r) {
    json_free(r->content);
    json_free(r->details);
    memset(r, 0, sizeof(*r));
}

char *tool_result_joined_text(const tool_result_t *r) {
    sb_t b = SB_INIT;
    json_t *blk;
    json_arr_foreach(r->content, i, blk) {
        if (str_eq(json_get_str(blk, "type"), "text")) {
            if (b.len) sb_putc(&b, '\n');
            sb_append(&b, json_get_str(blk, "text"));
        }
    }
    return sb_detach(&b);
}

/* ---------- tools ---------- */

agent_tool_t *agent_tool_new(const char *name, const char *label, const char *description, json_t *params,
                             int (*execute)(agent_tool_t *, tool_ctx_t *, const json_t *, tool_result_t *)) {
    agent_tool_t *t = xcalloc(1, sizeof(*t));
    t->name = xstrdup(name);
    t->label = xstrdup(label ? label : name);
    t->description = xstrdup(description ? description : "");
    t->parameters = params ? params : json_parse_str("{\"type\":\"object\",\"properties\":{}}");
    t->execute = execute;
    return t;
}

void agent_tool_free(agent_tool_t *t) {
    if (!t) return;
    if (t->destroy) t->destroy(t);
    free(t->name);
    free(t->label);
    free(t->description);
    json_free(t->parameters);
    free(t);
}

json_t *agent_tool_wire(const agent_tool_t *t) {
    json_t *o = json_obj();
    json_set_str(o, "name", t->name);
    json_set_str(o, "description", t->description);
    json_set(o, "parameters", json_clone(t->parameters));
    return o;
}

/* ---------- messages ---------- */

json_t *msg_user_content(json_t *content) {
    json_t *m = json_obj();
    json_set_str(m, "role", "user");
    json_set(m, "content", content);
    json_set_int(m, "timestamp", now_ms());
    return m;
}

json_t *msg_user_text(const char *text) {
    json_t *c = json_arr();
    json_push(c, text_block(text));
    return msg_user_content(c);
}

json_t *msg_developer_text(const char *text) {
    json_t *m = json_obj();
    json_set_str(m, "role", "developer");
    json_t *c = json_arr();
    json_push(c, text_block(text));
    json_set(m, "content", c);
    json_set_int(m, "timestamp", now_ms());
    return m;
}

json_t *msg_tool_result(const char *id, const char *name, const tool_result_t *r) {
    json_t *m = json_obj();
    json_set_str(m, "role", "toolResult");
    json_set_str(m, "toolCallId", id);
    json_set_str(m, "toolName", name);
    json_set(m, "content", r->content ? json_clone(r->content) : json_arr());
    if (r->details) json_set(m, "details", json_clone(r->details));
    json_set_bool(m, "isError", r->is_error);
    if (r->useless && !r->is_error) json_set_bool(m, "useless", true);
    json_set_int(m, "timestamp", now_ms());
    return m;
}

const json_t *last_assistant(const json_t *messages) {
    for (size_t i = json_len(messages); i > 0; i--) {
        json_t *m = json_at(messages, i - 1);
        if (str_eq(json_get_str(m, "role"), "assistant")) return m;
    }
    return NULL;
}

/* ---------- events ---------- */

static const char *ev_names[] = {"agent_start",   "agent_end",      "turn_start",           "turn_end",
                                 "message_start", "message_update", "message_end",          "tool_execution_start",
                                 "tool_execution_update", "tool_execution_end"};

const char *agent_event_name(agent_event_type_t t) { return ev_names[t]; }

static json_t *result_json(const tool_result_t *r) {
    json_t *o = json_obj();
    json_set(o, "content", r && r->content ? json_clone(r->content) : json_arr());
    if (r && r->details) json_set(o, "details", json_clone(r->details));
    if (r && r->is_error) json_set_bool(o, "isError", true);
    return o;
}

json_t *agent_event_to_json(const agent_event_t *ev) {
    json_t *o = json_obj();
    json_set_str(o, "type", agent_event_name(ev->type));
    switch (ev->type) {
    case AGENT_EV_AGENT_END:
        json_set(o, "messages", ev->messages ? json_clone(ev->messages) : json_arr());
        break;
    case AGENT_EV_TURN_END:
        json_set(o, "message", json_clone(ev->message));
        json_set(o, "toolResults", ev->tool_results ? json_clone(ev->tool_results) : json_arr());
        break;
    case AGENT_EV_MESSAGE_START:
    case AGENT_EV_MESSAGE_END: json_set(o, "message", json_clone(ev->message)); break;
    case AGENT_EV_MESSAGE_UPDATE: {
        json_set(o, "message", json_clone(ev->message));
        if (ev->ai) {
            json_t *ae = json_obj();
            json_set_str(ae, "type", ai_event_name(ev->ai->type));
            if (ev->ai->content_index >= 0) json_set_int(ae, "contentIndex", ev->ai->content_index);
            if (ev->ai->delta) json_set(ae, "delta", json_strn(ev->ai->delta, ev->ai->delta_len));
            if (ev->ai->type == AI_EV_TEXT_END) json_set_str(ae, "content", json_get_str(ev->ai->block, "text"));
            if (ev->ai->type == AI_EV_THINKING_END) json_set_str(ae, "content", json_get_str(ev->ai->block, "thinking"));
            if (ev->ai->type == AI_EV_TOOLCALL_END) json_set(ae, "toolCall", json_clone(ev->ai->block));
            json_set(o, "assistantMessageEvent", ae);
        }
        break;
    }
    case AGENT_EV_TOOL_START:
        json_set_str(o, "toolCallId", ev->tool_call_id);
        json_set_str(o, "toolName", ev->tool_name);
        json_set(o, "args", ev->args ? json_clone(ev->args) : json_obj());
        break;
    case AGENT_EV_TOOL_UPDATE:
        json_set_str(o, "toolCallId", ev->tool_call_id);
        json_set_str(o, "toolName", ev->tool_name);
        json_set(o, "args", ev->args ? json_clone(ev->args) : json_obj());
        json_set(o, "partialResult", result_json(ev->result));
        break;
    case AGENT_EV_TOOL_END:
        json_set_str(o, "toolCallId", ev->tool_call_id);
        json_set_str(o, "toolName", ev->tool_name);
        json_set(o, "result", result_json(ev->result));
        json_set_bool(o, "isError", ev->is_error);
        break;
    default: break;
    }
    return o;
}

static void emit(agent_t *a, const agent_event_t *ev) {
    pthread_mutex_lock(&a->ev_mu);
    for (size_t i = 0; i < a->listeners.n; i++) a->listeners.v[i].fn(a->listeners.v[i].ud, ev);
    pthread_mutex_unlock(&a->ev_mu);
}

static void emit_simple(agent_t *a, agent_event_type_t t) {
    agent_event_t ev = {.type = t};
    emit(a, &ev);
}

static void emit_message(agent_t *a, agent_event_type_t t, const json_t *msg) {
    agent_event_t ev = {.type = t, .message = msg};
    emit(a, &ev);
}

/* ---------- agent ---------- */

agent_t *agent_new(void) {
    agent_t *a = xcalloc(1, sizeof(*a));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&a->mu, &attr);
    pthread_mutex_init(&a->ev_mu, &attr);
    pthread_mutexattr_destroy(&attr);
    a->messages = json_arr();
    a->system_prompt = json_arr();
    a->steering = json_arr();
    a->follow_up = json_arr();
    ai_stream_opts_init(&a->base_opts);
    return a;
}

void agent_free(agent_t *a) {
    if (!a) return;
    for (size_t i = 0; i < a->all_tools.n; i++) agent_tool_free(a->all_tools.v[i]);
    vec_free(&a->all_tools);
    vec_free(&a->tools);
    vec_free(&a->listeners);
    json_free(a->messages);
    json_free(a->system_prompt);
    json_free(a->steering);
    json_free(a->follow_up);
    free(a->api_key);
    free(a->session_id);
    pthread_mutex_destroy(&a->mu);
    pthread_mutex_destroy(&a->ev_mu);
    free(a);
}

void agent_set_model(agent_t *a, const ai_model_t *m) {
    pthread_mutex_lock(&a->mu);
    a->model = m;
    pthread_mutex_unlock(&a->mu);
}

void agent_set_system_prompt(agent_t *a, const char *prompt) {
    json_t *arr = json_arr();
    if (prompt) json_push_str(arr, prompt);
    agent_set_system_prompt_parts(a, arr);
}

void agent_set_system_prompt_parts(agent_t *a, json_t *parts) {
    pthread_mutex_lock(&a->mu);
    json_free(a->system_prompt);
    a->system_prompt = parts ? parts : json_arr();
    pthread_mutex_unlock(&a->mu);
}

void agent_add_tool(agent_t *a, agent_tool_t *t) {
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->all_tools.n; i++) {
        if (strcmp(a->all_tools.v[i]->name, t->name) == 0) {
            /* replace */
            agent_tool_t *old = a->all_tools.v[i];
            a->all_tools.v[i] = t;
            for (size_t k = 0; k < a->tools.n; k++)
                if (a->tools.v[k] == old) a->tools.v[k] = t;
            agent_tool_free(old);
            pthread_mutex_unlock(&a->mu);
            return;
        }
    }
    vec_push(&a->all_tools, t);
    if (!t->hidden) vec_push(&a->tools, t);
    pthread_mutex_unlock(&a->mu);
}

agent_tool_t *agent_find_tool(agent_t *a, const char *name) {
    for (size_t i = 0; i < a->all_tools.n; i++)
        if (strcmp(a->all_tools.v[i]->name, name) == 0) return a->all_tools.v[i];
    return NULL;
}

void agent_remove_tool(agent_t *a, const char *name) {
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        if (strcmp(t->name, name) != 0) continue;
        for (size_t k = 0; k < a->tools.n; k++) {
            if (a->tools.v[k] == t) {
                memmove(a->tools.v + k, a->tools.v + k + 1, (a->tools.n - k - 1) * sizeof(void *));
                a->tools.n--;
                break;
            }
        }
        memmove(a->all_tools.v + i, a->all_tools.v + i + 1, (a->all_tools.n - i - 1) * sizeof(void *));
        a->all_tools.n--;
        agent_tool_free(t);
        break;
    }
    pthread_mutex_unlock(&a->mu);
}

void agent_set_active_tools(agent_t *a, const strvec_t *names) {
    pthread_mutex_lock(&a->mu);
    a->tools.n = 0;
    for (size_t i = 0; i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        if (names ? strvec_contains(names, t->name) : !t->hidden) vec_push(&a->tools, t);
    }
    pthread_mutex_unlock(&a->mu);
}

void agent_subscribe(agent_t *a, agent_listener_fn fn, void *ud) {
    pthread_mutex_lock(&a->ev_mu);
    __typeof__(a->listeners.v[0]) l = {fn, ud};
    vec_push(&a->listeners, l);
    pthread_mutex_unlock(&a->ev_mu);
}

void agent_unsubscribe(agent_t *a, agent_listener_fn fn, void *ud) {
    pthread_mutex_lock(&a->ev_mu);
    for (size_t i = 0; i < a->listeners.n; i++) {
        if (a->listeners.v[i].fn == fn && a->listeners.v[i].ud == ud) {
            memmove(a->listeners.v + i, a->listeners.v + i + 1, (a->listeners.n - i - 1) * sizeof(a->listeners.v[0]));
            a->listeners.n--;
            break;
        }
    }
    pthread_mutex_unlock(&a->ev_mu);
}

void agent_replace_messages(agent_t *a, json_t *messages) {
    pthread_mutex_lock(&a->mu);
    json_free(a->messages);
    a->messages = messages ? messages : json_arr();
    pthread_mutex_unlock(&a->mu);
}

void agent_append_message(agent_t *a, json_t *msg) {
    pthread_mutex_lock(&a->mu);
    json_push(a->messages, msg);
    pthread_mutex_unlock(&a->mu);
}

void agent_abort(agent_t *a) { a->cancel = 1; }

void agent_steer(agent_t *a, json_t *msg) {
    pthread_mutex_lock(&a->mu);
    json_push(a->steering, msg);
    pthread_mutex_unlock(&a->mu);
}

void agent_follow_up(agent_t *a, json_t *msg) {
    pthread_mutex_lock(&a->mu);
    json_push(a->follow_up, msg);
    pthread_mutex_unlock(&a->mu);
}

void agent_clear_queues(agent_t *a) {
    pthread_mutex_lock(&a->mu);
    json_free(a->steering);
    json_free(a->follow_up);
    a->steering = json_arr();
    a->follow_up = json_arr();
    pthread_mutex_unlock(&a->mu);
}

bool agent_is_running(agent_t *a) { return a->running; }

static json_t *take_queue(agent_t *a, json_t **q) {
    pthread_mutex_lock(&a->mu);
    json_t *out = *q;
    *q = json_arr();
    pthread_mutex_unlock(&a->mu);
    if (!json_len(out)) {
        json_free(out);
        return NULL;
    }
    return out;
}

static bool queue_nonempty(agent_t *a, json_t *q) {
    pthread_mutex_lock(&a->mu);
    bool r = json_len(q) > 0;
    pthread_mutex_unlock(&a->mu);
    return r;
}

/* adds message to history and to the run list, emitting start/end */
static void add_message(agent_t *a, json_t *run_msgs, json_t *msg) {
    emit_message(a, AGENT_EV_MESSAGE_START, msg);
    pthread_mutex_lock(&a->mu);
    json_push(a->messages, msg);
    pthread_mutex_unlock(&a->mu);
    json_push(run_msgs, json_clone(msg));
    emit_message(a, AGENT_EV_MESSAGE_END, msg);
}

/* ---------- streaming ---------- */

typedef struct {
    agent_t *a;
    bool started;
    int abort_requested;
} stream_ud_t;

static void on_ai_event(void *ud, const ai_event_t *ev) {
    stream_ud_t *s = ud;
    agent_t *a = s->a;
    if (ev->type == AI_EV_START || !s->started) {
        if (!s->started) {
            s->started = true;
            emit_message(a, AGENT_EV_MESSAGE_START, ev->partial);
        }
        if (ev->type == AI_EV_START) return;
    }
    if (ev->type == AI_EV_DONE || ev->type == AI_EV_ERROR) return; /* message_end emitted by caller */
    agent_event_t ae = {.type = AGENT_EV_MESSAGE_UPDATE, .message = ev->partial, .ai = ev};
    emit(a, &ae);
    if (a->hooks.on_stream_delta && (ev->type == AI_EV_TEXT_DELTA || ev->type == AI_EV_THINKING_DELTA ||
                                     ev->type == AI_EV_TOOLCALL_DELTA)) {
        if (a->hooks.on_stream_delta(a->hooks.ud, a, ev)) {
            s->abort_requested = 1;
            a->cancel = 1;
        }
    }
}

static json_t *stream_assistant(agent_t *a, int *ttsr_abort) {
    json_t *ctx_msgs = NULL;
    pthread_mutex_lock(&a->mu);
    json_t *snapshot = json_clone(a->messages);
    pthread_mutex_unlock(&a->mu);
    if (a->hooks.transform_context) {
        json_t *t = a->hooks.transform_context(a->hooks.ud, a, snapshot);
        if (t) {
            json_free(snapshot);
            snapshot = t;
        }
    }
    ctx_msgs = ai_transform_messages(snapshot, a->model);
    json_free(snapshot);

    json_t *tools = json_arr();
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->tools.n; i++) json_push(tools, agent_tool_wire(a->tools.v[i]));
    json_t *sys = json_clone(a->system_prompt);
    pthread_mutex_unlock(&a->mu);

    ai_context_t ctx = {.system_prompt = sys, .messages = ctx_msgs, .tools = json_len(tools) ? tools : NULL};
    ai_stream_opts_t opts = a->base_opts;
    opts.api_key = a->api_key;
    opts.session_id = a->session_id;
    opts.reasoning = model_clamp_effort(a->model, a->thinking);
    opts.cancel = &a->cancel;
    if (a->hooks.prepare_request) a->hooks.prepare_request(a->hooks.ud, a, &opts);

    stream_ud_t s = {.a = a};
    json_t *msg = ai_stream(a->model, &ctx, &opts, on_ai_event, &s);
    if (!s.started) emit_message(a, AGENT_EV_MESSAGE_START, msg);
    *ttsr_abort = s.abort_requested;
    json_free(ctx_msgs);
    json_free(tools);
    json_free(sys);
    return msg;
}

/* ---------- tool execution ---------- */

typedef struct {
    agent_t *a;
    const json_t *call;
    agent_tool_t *tool;
    json_t *args;
    tool_result_t res;
    tool_ctx_t ctx;
    volatile sig_atomic_t cancel;
    pthread_t th;
    bool skip;
} tool_job_t;

static void tool_update_cb(tool_ctx_t *ctx, const tool_result_t *partial) {
    tool_job_t *j = (tool_job_t *)((char *)ctx - offsetof(tool_job_t, ctx));
    agent_event_t ev = {.type = AGENT_EV_TOOL_UPDATE, .tool_call_id = ctx->tool_call_id, .tool_name = ctx->tool_name,
                        .args = j->args, .result = partial};
    emit(j->a, &ev);
}

static void run_tool(tool_job_t *j) {
    agent_t *a = j->a;
    const char *name = json_get_str(j->call, "name");
    const char *id = json_get_str(j->call, "id");
    agent_event_t st = {.type = AGENT_EV_TOOL_START, .tool_call_id = id, .tool_name = name, .args = j->args};
    emit(a, &st);
    if (j->skip) {
        tool_result_error(&j->res, "Skipped due to queued user message.");
    } else if (!j->tool) {
        tool_result_error(&j->res, "Tool %s not found", name ? name : "(null)");
    } else {
        char *reason = NULL;
        tool_gate_t gate = TOOL_ALLOW;
        if (a->hooks.before_tool) gate = a->hooks.before_tool(a->hooks.ud, a, j->call, &reason);
        if (gate == TOOL_BLOCK) {
            tool_result_error(&j->res, "%s", reason ? reason : "Tool call was blocked");
        } else {
            char *verr = j->tool->lenient_args ? NULL : schema_validate(j->tool->parameters, j->args);
            if (verr) {
                char *argstr = json_dump(j->args, 0);
                tool_result_error(&j->res, "Validation failed for tool \"%s\":\n%s\n\nReceived arguments:\n%s", name, verr, argstr);
                free(argstr);
                free(verr);
            } else {
                j->ctx.agent = a;
                j->ctx.tool_call_id = id;
                j->ctx.tool_name = name;
                j->ctx.cancel = &a->cancel;
                j->ctx.update = tool_update_cb;
                j->ctx.app = a->app;
                j->tool->execute(j->tool, &j->ctx, j->args, &j->res);
                if (a->cancel && !j->res.content) tool_result_error(&j->res, "Tool execution was aborted");
            }
        }
        free(reason);
        if (a->hooks.after_tool) a->hooks.after_tool(a->hooks.ud, a, j->call, &j->res);
    }
    if (!j->res.content) j->res.content = json_arr();
    agent_event_t en = {.type = AGENT_EV_TOOL_END, .tool_call_id = id, .tool_name = name, .result = &j->res,
                        .is_error = j->res.is_error};
    emit(a, &en);
}

static void *tool_thread(void *p) {
    run_tool(p);
    return NULL;
}

static void execute_tools(agent_t *a, json_t *run_msgs, const json_t *assistant, json_t *results_out) {
    json_t *content = json_get(assistant, "content");
    VEC(tool_job_t *) jobs = {0};
    json_t *blk;
    json_arr_foreach(content, i, blk) {
        if (!str_eq(json_get_str(blk, "type"), "toolCall")) continue;
        tool_job_t *j = xcalloc(1, sizeof(*j));
        j->a = a;
        j->call = blk;
        const char *name = json_get_str(blk, "name");
        j->tool = name ? agent_find_tool(a, name) : NULL;
        json_t *args = json_get(blk, "arguments");
        j->args = json_is_obj(args) ? json_clone(args) : json_obj();
        vec_push(&jobs, j);
    }
    size_t i = 0;
    while (i < jobs.n) {
        if (a->cancel) {
            for (size_t k = i; k < jobs.n; k++) {
                tool_result_error(&jobs.v[k]->res, "Tool execution was aborted");
                agent_event_t st = {.type = AGENT_EV_TOOL_START, .tool_call_id = json_get_str(jobs.v[k]->call, "id"),
                                    .tool_name = json_get_str(jobs.v[k]->call, "name"), .args = jobs.v[k]->args};
                emit(a, &st);
                agent_event_t en = {.type = AGENT_EV_TOOL_END, .tool_call_id = st.tool_call_id, .tool_name = st.tool_name,
                                    .result = &jobs.v[k]->res, .is_error = true};
                emit(a, &en);
            }
            break;
        }
        if (queue_nonempty(a, a->steering)) {
            for (size_t k = i; k < jobs.n; k++) jobs.v[k]->skip = true;
        }
        bool excl = !jobs.v[i]->tool || jobs.v[i]->tool->exclusive || jobs.v[i]->skip;
        if (excl) {
            run_tool(jobs.v[i]);
            i++;
            continue;
        }
        /* batch of consecutive shared tools, run in parallel (cap 8) */
        size_t end = i;
        while (end < jobs.n && end - i < 8 && jobs.v[end]->tool && !jobs.v[end]->tool->exclusive) end++;
        if (end - i == 1) {
            run_tool(jobs.v[i]);
        } else {
            for (size_t k = i; k < end; k++) {
                if (pthread_create(&jobs.v[k]->th, NULL, tool_thread, jobs.v[k]) != 0) {
                    jobs.v[k]->th = 0;
                    run_tool(jobs.v[k]);
                }
            }
            for (size_t k = i; k < end; k++)
                if (jobs.v[k]->th) pthread_join(jobs.v[k]->th, NULL);
        }
        i = end;
    }
    for (size_t k = 0; k < jobs.n; k++) {
        tool_job_t *j = jobs.v[k];
        if (!j->res.content) tool_result_error(&j->res, "Tool execution was aborted");
        json_t *m = msg_tool_result(json_get_str(j->call, "id"), json_get_str(j->call, "name"), &j->res);
        json_push(results_out, json_clone(m));
        add_message(a, run_msgs, m);
        tool_result_free(&j->res);
        json_free(j->args);
        free(j);
    }
    vec_free(&jobs);
}

/* ---------- loop ---------- */

static void run_loop(agent_t *a, json_t *new_messages) {
    a->running = true;
    a->cancel = 0;
    json_t *run_msgs = json_arr();
    emit_simple(a, AGENT_EV_AGENT_START);
    json_t *m;
    if (new_messages) {
        json_arr_foreach(new_messages, i, m) add_message(a, run_msgs, json_clone(m));
    }
    int turns = 0;
    int retry_attempt = 0;
    for (;;) {
        emit_simple(a, AGENT_EV_TURN_START);
        int ttsr = 0;
        json_t *assistant = stream_assistant(a, &ttsr);
        const char *stop = json_get_str(assistant, "stopReason");
        bool failed = stop && (strcmp(stop, "error") == 0 || strcmp(stop, "aborted") == 0);
        if (ttsr && a->hooks.should_retry) {
            /* stream deliberately aborted by a rule: let the hook inject and retry */
            emit_message(a, AGENT_EV_MESSAGE_END, assistant);
            a->cancel = 0;
            if (a->hooks.should_retry(a->hooks.ud, a, assistant, -1)) {
                json_free(assistant);
                agent_event_t te = {.type = AGENT_EV_TURN_END, .message = NULL, .tool_results = NULL};
                emit(a, &te);
                continue;
            }
            failed = true;
        } else if (failed && !a->cancel && a->hooks.should_retry &&
                   a->hooks.should_retry(a->hooks.ud, a, assistant, retry_attempt)) {
            retry_attempt++;
            emit_message(a, AGENT_EV_MESSAGE_END, assistant);
            json_free(assistant);
            continue;
        } else {
            emit_message(a, AGENT_EV_MESSAGE_END, assistant);
        }
        retry_attempt = 0;
        pthread_mutex_lock(&a->mu);
        json_push(a->messages, assistant);
        pthread_mutex_unlock(&a->mu);
        json_push(run_msgs, json_clone(assistant));
        json_t *results = json_arr();
        bool has_tools = false;
        json_t *blk;
        json_arr_foreach(json_get(assistant, "content"), i, blk) {
            if (str_eq(json_get_str(blk, "type"), "toolCall")) has_tools = true;
        }
        if (!failed && has_tools) execute_tools(a, run_msgs, assistant, results);
        agent_event_t te = {.type = AGENT_EV_TURN_END, .message = assistant, .tool_results = results};
        emit(a, &te);
        json_free(results);
        turns++;
        if (failed || a->cancel) break;
        if (a->max_turns && turns >= a->max_turns) break;
        json_t *steer = take_queue(a, &a->steering);
        if (steer) {
            json_arr_foreach(steer, i, m) add_message(a, run_msgs, json_clone(m));
            json_free(steer);
            continue;
        }
        if (has_tools) continue;
        json_t *fu = take_queue(a, &a->follow_up);
        if (fu) {
            json_arr_foreach(fu, i, m) add_message(a, run_msgs, json_clone(m));
            json_free(fu);
            continue;
        }
        break;
    }
    agent_event_t ee = {.type = AGENT_EV_AGENT_END, .messages = run_msgs};
    a->running = false;
    emit(a, &ee);
    json_free(run_msgs);
}

int agent_prompt(agent_t *a, json_t *new_messages) {
    if (a->running) {
        json_free(new_messages);
        return -1;
    }
    if (!a->model) {
        json_free(new_messages);
        return -1;
    }
    run_loop(a, new_messages);
    json_free(new_messages);
    return 0;
}

int agent_continue(agent_t *a) {
    if (a->running || !a->model) return -1;
    run_loop(a, NULL);
    return 0;
}

/* ---------- schema validation ---------- */

static bool type_matches(const char *type, const json_t *v) {
    if (!type) return true;
    if (strcmp(type, "string") == 0) return json_is_str(v);
    if (strcmp(type, "number") == 0) return json_is_num(v);
    if (strcmp(type, "integer") == 0) return json_is_num(v) && v->u.num.d == floor(v->u.num.d);
    if (strcmp(type, "boolean") == 0) return json_is_bool(v);
    if (strcmp(type, "object") == 0) return json_is_obj(v);
    if (strcmp(type, "array") == 0) return json_is_arr(v);
    if (strcmp(type, "null") == 0) return v && v->type == JSON_NULL;
    return true;
}

static json_t *coerce(const char *type, const json_t *v) {
    if (!type || !v) return NULL;
    if ((strcmp(type, "number") == 0 || strcmp(type, "integer") == 0) && json_is_str(v)) {
        char *end;
        double d = strtod(json_sv(v), &end);
        if (end != json_sv(v) && *end == 0) return json_num(d);
    }
    if (strcmp(type, "boolean") == 0 && json_is_str(v)) {
        if (strcmp(json_sv(v), "true") == 0) return json_bool(true);
        if (strcmp(json_sv(v), "false") == 0) return json_bool(false);
    }
    if (strcmp(type, "string") == 0 && json_is_num(v)) {
        char *s = json_dump(v, 0);
        return json_str_own(s);
    }
    if ((strcmp(type, "array") == 0 || strcmp(type, "object") == 0) && json_is_str(v)) {
        /* models sometimes stringify nested JSON */
        json_t *p = json_parse_str(json_sv(v));
        if (p && type_matches(type, p)) return p;
        json_free(p);
    }
    return NULL;
}

static void validate_node(const json_t *schema, json_t *parent, const char *key, size_t idx, json_t *v, const char *path,
                          sb_t *errs, int depth) {
    if (!json_is_obj(schema) || depth > 32) return;
    json_t *anyof = json_get_arr(schema, "anyOf");
    if (!anyof) anyof = json_get_arr(schema, "oneOf");
    if (anyof) {
        json_t *alt;
        json_arr_foreach(anyof, i, alt) {
            sb_t sub = SB_INIT;
            validate_node(alt, parent, key, idx, v, path, &sub, depth + 1);
            bool ok = sub.len == 0;
            sb_free(&sub);
            if (ok) return;
        }
        sb_appendf(errs, "  - %s: does not match any allowed schema\n", *path ? path : "root");
        return;
    }
    json_t *tj = json_get(schema, "type");
    const char *type = json_sv(tj);
    if (json_is_arr(tj)) {
        bool any = false;
        json_t *t;
        json_arr_foreach(tj, i, t) if (type_matches(json_sv(t), v)) any = true;
        if (!any) sb_appendf(errs, "  - %s: wrong type\n", *path ? path : "root");
        return;
    }
    if (type && !type_matches(type, v)) {
        json_t *c = coerce(type, v);
        if (c && parent) {
            if (key) json_set(parent, key, c);
            else {
                json_free(parent->u.arr.items[idx]);
                parent->u.arr.items[idx] = c;
            }
            v = c;
        } else {
            json_free(c);
            sb_appendf(errs, "  - %s: must be %s\n", *path ? path : "root", type);
            return;
        }
    }
    json_t *en = json_get_arr(schema, "enum");
    if (en) {
        bool found = false;
        json_t *e;
        json_arr_foreach(en, i, e) if (json_equal(e, v)) found = true;
        if (!found) {
            char *allowed = json_dump(en, 0);
            sb_appendf(errs, "  - %s: must be one of %s\n", *path ? path : "root", allowed);
            free(allowed);
        }
    }
    if (json_is_obj(v)) {
        json_t *props = json_get_obj(schema, "properties");
        json_t *req = json_get_arr(schema, "required"), *r;
        json_arr_foreach(req, i, r) {
            if (json_is_str(r) && !json_get(v, json_sv(r)))
                sb_appendf(errs, "  - %s%s%s: is required\n", path, *path ? "." : "", json_sv(r));
        }
        const char *k;
        json_t *pv;
        json_obj_foreach(props, i, k, pv) {
            json_t *child = json_get(v, k);
            if (!child) continue;
            char *cp = *path ? xasprintf("%s.%s", path, k) : xstrdup(k);
            validate_node(pv, v, k, 0, child, cp, errs, depth + 1);
            free(cp);
        }
    } else if (json_is_arr(v)) {
        json_t *items = json_get_obj(schema, "items");
        if (items) {
            for (size_t i = 0; i < json_len(v); i++) {
                char *cp = xasprintf("%s[%zu]", *path ? path : "", i);
                validate_node(items, v, NULL, i, json_at(v, i), cp, errs, depth + 1);
                free(cp);
            }
        }
    }
}

char *schema_validate(const json_t *schema, json_t *args) {
    sb_t errs = SB_INIT;
    validate_node(schema, NULL, NULL, 0, args, "", &errs, 0);
    if (!errs.len) return NULL;
    if (errs.s[errs.len - 1] == '\n') sb_truncate(&errs, errs.len - 1);
    return sb_detach(&errs);
}
