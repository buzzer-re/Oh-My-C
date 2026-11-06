#include "agent.h"
#include "ai/ai_images.h"

#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* agent-loop.ts: a call refused before running carries details {isError, error} */
static void tool_result_validation_error(tool_result_t *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = xvasprintf(fmt, ap);
    va_end(ap);
    tool_result_text(r, s);
    json_free(r->details);
    r->details = json_obj();
    json_set_bool(r->details, "isError", true);
    json_set_str(r->details, "error", s);
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
    t->intent_mode = INTENT_REQUIRE;
    return t;
}

void agent_tool_free(agent_tool_t *t) {
    if (!t) return;
    if (t->destroy) t->destroy(t);
    free(t->name);
    free(t->label);
    free(t->description);
    free(t->summary);
    free(t->mcp_server);
    free(t->mcp_tool);
    json_free(t->examples);
    json_free(t->parameters);
    free(t);
}

/* ---------- intent field (`i`) ----------
 * Port of agent-loop.ts injectIntentIntoSchema/extractIntent: every top-level tool schema gets a leading
 * `i: {type: string, description: "concise intent"}` property (required unless the tool opts for optional/omit);
 * the harness strips it before validation/execution and reports it on tool_execution_start. */

static int g_intent_tracing = -1; /* -1 = default (on unless PI_NO_INTENT=1) */

void agent_set_intent_tracing(bool on) { g_intent_tracing = on ? 1 : 0; }

bool agent_intent_tracing(void) {
    if (g_intent_tracing == 0) return false;
    const char *no = getenv("PI_NO_INTENT");
    if (no && !strcmp(no, "1")) return false;
    no = getenv("OMC_NO_INTENT");
    if (no && !strcmp(no, "1")) return false;
    return true;
}

static bool props_have(const json_t *schema, const char *name, int depth) {
    if (!json_is_obj(schema) || depth > 16) return false;
    json_t *props = json_get_obj(schema, "properties");
    if (props && json_get(props, name)) return true;
    static const char *const UNIONS[] = {"anyOf", "oneOf", "allOf"};
    for (size_t u = 0; u < 3; u++) {
        json_t *arr = json_get_arr(schema, UNIONS[u]), *v;
        json_arr_foreach(arr, i, v) if (props_have(v, name, depth + 1)) return true;
    }
    return false;
}

bool schema_declares_property(const json_t *schema, const char *name) { return props_have(schema, name, 0); }

static json_t *inject_intent(const json_t *schema, int mode, int depth) {
    json_t *out = json_clone(schema);
    if (!json_is_obj(out) || depth > 8) return out;
    json_t *props = json_get_obj(out, "properties");
    if (!props) {
        static const char *const UNIONS[] = {"anyOf", "oneOf"};
        for (size_t u = 0; u < 2; u++) {
            json_t *arr = json_get_arr(out, UNIONS[u]);
            if (!arr) continue;
            for (size_t i = 0; i < json_len(arr); i++) {
                json_t *alt = json_arr_remove(arr, i);
                json_arr_insert(arr, i, inject_intent(alt, mode, depth + 1));
                json_free(alt);
            }
            return out;
        }
    }
    json_t *np = json_obj();
    json_t *ip = json_obj();
    json_set_str(ip, "type", "string");
    json_set_str(ip, "description", "concise intent");
    json_set(np, INTENT_FIELD, ip);
    const char *k;
    json_t *v;
    json_obj_foreach(props, i, k, v) json_set(np, k, json_clone(v));
    json_set(out, "properties", np); /* replaces in place (keeps key position) or appends */
    if (mode == INTENT_REQUIRE) {
        json_t *req = json_get_arr(out, "required");
        if (req) json_push_str(req, INTENT_FIELD);
        else {
            req = json_arr();
            json_push_str(req, INTENT_FIELD);
            json_set(out, "required", req);
        }
    }
    return out;
}

json_t *agent_schema_inject_intent(const json_t *schema, int mode) { return inject_intent(schema, mode, 0); }

/* ---------- tool examples (port of ai/dialect/examples.ts + rendering.ts pyCall) ---------- */

static void py_string(sb_t *b, const char *s) {
    sb_putc(b, '"');
    for (; *s; s++) {
        switch (*s) {
        case '\\': sb_append(b, "\\\\"); break;
        case '"': sb_append(b, "\\\""); break;
        case '\n': sb_append(b, "\\n"); break;
        case '\r': sb_append(b, "\\r"); break;
        case '\t': sb_append(b, "\\t"); break;
        default: sb_putc(b, *s);
        }
    }
    sb_putc(b, '"');
}

static void py_value(sb_t *b, const json_t *v) {
    if (!v || json_is_null(v)) sb_append(b, "None");
    else if (json_is_bool(v)) sb_append(b, v->u.b ? "True" : "False");
    else if (json_is_num(v)) {
        char *n = json_dump(v, 0);
        sb_append(b, n);
        free(n);
    } else if (json_is_str(v)) py_string(b, json_sv(v));
    else if (json_is_arr(v)) {
        sb_putc(b, '[');
        json_t *e;
        json_arr_foreach(v, i, e) {
            if (i) sb_append(b, ", ");
            py_value(b, e);
        }
        sb_putc(b, ']');
    } else if (json_is_obj(v)) {
        sb_putc(b, '{');
        const char *k;
        json_t *e;
        json_obj_foreach(v, i, k, e) {
            if (i) sb_append(b, ", ");
            py_string(b, k);
            sb_append(b, ": ");
            py_value(b, e);
        }
        sb_putc(b, '}');
    }
}

static void py_arg_value(sb_t *b, const json_t *v) {
    const char *s = json_sv(v);
    if (s && strchr(s, '\n')) {
        size_t n = strlen(s);
        bool fence_safe = !strstr(s, "\"\"\"") && s[0] != '"' && s[n - 1] != '"' && s[n - 1] != '\\';
        if (fence_safe) {
            sb_appendf(b, "\"\"\"%s\"\"\"", s);
            return;
        }
    }
    py_value(b, v);
}

/* rendering.ts pyCall: name(key=value, ...) */
static void py_call(sb_t *b, const char *tool, const json_t *args, const char *intent_field) {
    sb_appendf(b, "%s(", tool);
    bool first = true;
    if (intent_field) {
        sb_appendf(b, "%s=\"…\"", intent_field);
        first = false;
    }
    const char *k;
    json_t *v;
    json_obj_foreach(args, i, k, v) {
        sb_appendf(b, "%s%s=", first ? "" : ", ", k);
        py_arg_value(b, v);
        first = false;
    }
    sb_putc(b, ')');
}

/* examples.ts renderToolExamplesJsdoc: `@example "caption"` + the call (bare sole string payload) */
static void jsdoc_call(sb_t *b, const char *tool, const json_t *args) {
    if (json_len(args) == 1 && json_is_str(args->u.obj.vals[0])) sb_append(b, json_sv(args->u.obj.vals[0]));
    else py_call(b, tool, args, NULL);
}

char *agent_tool_examples_jsdoc(const agent_tool_t *t) {
    if (!json_len(t->examples)) return NULL;
    sb_t b = SB_INIT;
    json_t *ex;
    json_arr_foreach(t->examples, i, ex) {
        if (i) sb_putc(&b, '\n');
        const char *cap = json_get_str(ex, "caption");
        if (cap) {
            char *q = json_quote(cap);
            sb_appendf(&b, "@example %s", q);
            free(q);
        } else sb_append(&b, "@example");
        if (json_get(ex, "call")) {
            sb_putc(&b, '\n');
            jsdoc_call(&b, t->name, json_get(ex, "call"));
        } else if (json_get(ex, "good")) {
            sb_append(&b, "\nWRONG:\n");
            jsdoc_call(&b, t->name, json_get(ex, "bad"));
            sb_append(&b, "\nRIGHT:\n");
            jsdoc_call(&b, t->name, json_get(ex, "good"));
        } else if (json_get_str(ex, "note") && *json_get_str(ex, "note")) {
            sb_appendf(&b, "\n%s", json_get_str(ex, "note"));
        }
    }
    return sb_detach(&b);
}

static void render_example_call(sb_t *b, const char *tool, const json_t *args, const char *intent_field) {
    if (json_len(args) == 1 && json_is_str(args->u.obj.vals[0])) {
        if (intent_field) sb_appendf(b, "<example %s=\"…\">\n", intent_field);
        else sb_append(b, "<example>\n");
        sb_append(b, json_sv(args->u.obj.vals[0]));
        sb_append(b, "\n</example>");
        return;
    }
    sb_append(b, "<example>\n");
    py_call(b, tool, args, intent_field);
    sb_append(b, "\n</example>");
}

static char *render_tool_examples(const agent_tool_t *t, const char *intent_field) {
    if (!json_len(t->examples)) return NULL;
    sb_t b = SB_INIT;
    sb_append(&b, "<examples>\n");
    json_t *ex;
    json_arr_foreach(t->examples, i, ex) {
        if (i) sb_putc(&b, '\n');
        const char *cap = json_get_str(ex, "caption");
        sb_t head = SB_INIT;
        if (cap) sb_appendf(&head, "# %s\n", cap);
        if (json_get(ex, "call")) {
            sb_append(&b, head.s ? head.s : "");
            render_example_call(&b, t->name, json_get(ex, "call"), intent_field);
        } else if (json_get(ex, "good")) {
            sb_append(&b, head.s ? head.s : "");
            sb_append(&b, "WRONG:\n");
            render_example_call(&b, t->name, json_get(ex, "bad"), intent_field);
            sb_append(&b, "\nRIGHT:\n");
            render_example_call(&b, t->name, json_get(ex, "good"), intent_field);
        } else {
            if (head.len) sb_appendn(&b, head.s, head.len - 1); /* trimEnd */
            const char *note = json_get_str(ex, "note");
            if (note) sb_appendf(&b, "\n%s", note);
        }
        sb_free(&head);
    }
    sb_append(&b, "\n</examples>");
    return sb_detach(&b);
}

json_t *agent_tool_wire(const agent_tool_t *t) {
    json_t *o = json_obj();
    json_set_str(o, "name", t->name);
    bool intent = agent_intent_tracing() && t->intent_mode != INTENT_OMIT &&
                  !schema_declares_property(t->parameters, INTENT_FIELD);
    char *ex = render_tool_examples(t, intent ? INTENT_FIELD : NULL);
    if (ex) {
        json_set(o, "description", json_str_own(xasprintf("%s\n\n%s", t->description ? t->description : "", ex)));
        free(ex);
    } else {
        json_set_str(o, "description", t->description);
    }
    if (intent)
        json_set(o, "parameters", inject_intent(t->parameters, t->intent_mode, 0));
    else
        json_set(o, "parameters", json_clone(t->parameters));
    if (t->strict) json_set_bool(o, "strict", true);
    return o;
}

/* Strip a harness `i` from args (unless the schema owns it). Returns the trimmed intent (owned) or NULL;
 * *too_long is set when the value exceeds INTENT_MAX_LENGTH. */
static char *extract_intent(const agent_tool_t *tool, json_t *args, size_t *too_long) {
    *too_long = 0;
    if (!agent_intent_tracing() || !json_is_obj(args)) return NULL;
    if (tool && schema_declares_property(tool->parameters, INTENT_FIELD)) return NULL;
    json_t *iv = json_take(args, INTENT_FIELD);
    if (!iv) return NULL;
    char *r = NULL;
    if (json_is_str(iv)) {
        char *t = str_trim(json_sv(iv));
        size_t n = strlen(t);
        while (n && t[n - 1] == '.') t[--n] = 0; /* /\s*\.+$/ */
        char *t2 = str_trim(t);
        free(t);
        size_t cps = utf8_strlen(t2);
        if (cps > INTENT_MAX_LENGTH) *too_long = cps;
        if (*t2) r = t2;
        else free(t2);
    }
    json_free(iv);
    return r;
}

/* ---------- messages ---------- */

json_t *msg_user_content(json_t *content) {
    json_t *m = json_obj();
    json_set_str(m, "role", "user");
    json_set(m, "content", content);
    json_set_int(m, "timestamp", now_ms());
    return m;
}

json_t *msg_user_typed(json_t *content) {
    json_t *m = json_obj();
    json_set_str(m, "role", "user");
    json_set(m, "content", content);
    json_set_str(m, "attribution", "user");
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
    /* a failed call (omp: the tool threw) carries empty details */
    if (r->details) json_set(m, "details", json_clone(r->details));
    else if (r->is_error) json_set(m, "details", json_obj());
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
    /* AgentToolResult {content, details}; the error flag rides on the event (isError) */
    if (r && r->details) json_set(o, "details", json_clone(r->details));
    else if (r && r->is_error) json_set(o, "details", json_obj());
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
        if (ev->ai) {
            json_t *ae = json_obj();
            json_set_str(ae, "type", ai_event_name(ev->ai->type));
            if (ev->ai->content_index >= 0) json_set_int(ae, "contentIndex", ev->ai->content_index);
            if (ev->ai->delta) json_set(ae, "delta", json_strn(ev->ai->delta, ev->ai->delta_len));
            if (ev->ai->type == AI_EV_TEXT_END) json_set_str(ae, "content", json_get_str(ev->ai->block, "text"));
            if (ev->ai->type == AI_EV_THINKING_END) json_set_str(ae, "content", json_get_str(ev->ai->block, "thinking"));
            if (ev->ai->type == AI_EV_TOOLCALL_END) json_set(ae, "toolCall", json_clone(ev->ai->block));
            /* omp: the accumulated snapshot rides along as `partial` (RPC "delta" projection drops it) */
            if (ev->message) json_set(ae, "partial", json_clone(ev->message));
            json_set(o, "assistantMessageEvent", ae);
        }
        json_set(o, "message", json_clone(ev->message)); /* omp key order: assistantMessageEvent, message */
        break;
    }
    case AGENT_EV_TOOL_START:
        json_set_str(o, "toolCallId", ev->tool_call_id);
        json_set_str(o, "toolName", ev->tool_name);
        json_set(o, "args", ev->args ? json_clone(ev->args) : json_obj());
        if (ev->intent) json_set_str(o, "intent", ev->intent);
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
        /* agent-loop.ts: a tool that failed marks its result too; a call refused before running carries details instead */
        if (ev->is_error && !json_get_bool(json_get(o, "result") ? json_get(json_get(o, "result"), "details") : NULL, "isError", false))
            json_set_bool(json_get(o, "result"), "isError", true);
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
    a->asides = json_arr();
    ai_stream_opts_init(&a->base_opts);
    return a;
}

void agent_free(agent_t *a) {
    if (!a) return;
    free(a->mcp_scope);
    for (size_t i = 0; i < a->all_tools.n; i++) agent_tool_free(a->all_tools.v[i]);
    vec_free(&a->all_tools);
    vec_free(&a->tools);
    vec_free(&a->listeners);
    json_free(a->messages);
    json_free(a->system_prompt);
    json_free(a->steering);
    json_free(a->follow_up);
    json_free(a->asides);
    free(a->api_key);
    free(a->session_id);
    free(a->stream_error);
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
void agent_abort_next_silently(agent_t *a) { a->silent_abort_next = 1; }

void agent_abort_deliver_queued(agent_t *a) {
    a->deliver_queued = 1;
    a->cancel = 1;
}

bool agent_take_forced_delivery(agent_t *a) {
    if (!a->deliver_queued) return false;
    a->deliver_queued = 0;
    /* #promoteForcedFollowUps: the steering mode's boundary poll picks them up; they stay unmarked */
    bool moved = false;
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < json_len(a->follow_up);) {
        if (agent_msg_user_authored(json_at(a->follow_up, i))) {
            json_push(a->steering, json_arr_remove(a->follow_up, i));
            moved = true;
        } else i++;
    }
    pthread_mutex_unlock(&a->mu);
    if (moved) agent_queue_changed(a);
    return true;
}

void agent_queue_changed(agent_t *a) {
    if (a->hooks.queue_changed) a->hooks.queue_changed(a->hooks.ud, a);
}

void agent_steer(agent_t *a, json_t *msg) {
    /* omp AgentSession.steer: user input delivered mid-run is marked so the model sees it as an interjection */
    if (agent_msg_user_authored(msg)) json_set_bool(msg, "steering", true);
    pthread_mutex_lock(&a->mu);
    json_push(a->steering, msg);
    pthread_mutex_unlock(&a->mu);
    agent_queue_changed(a);
}

void agent_follow_up(agent_t *a, json_t *msg) {
    pthread_mutex_lock(&a->mu);
    json_push(a->follow_up, msg);
    pthread_mutex_unlock(&a->mu);
    agent_queue_changed(a);
}

void agent_aside(agent_t *a, json_t *msg) {
    pthread_mutex_lock(&a->mu);
    json_push(a->asides, msg);
    pthread_mutex_unlock(&a->mu);
}

json_t *agent_take_records(agent_t *a, bool (*is_record)(const json_t *msg), bool (*wakes)(const json_t *msg)) {
    json_t *out = json_arr();
    pthread_mutex_lock(&a->mu);
    json_t *queues[2] = {a->steering, a->asides};
    bool any = !wakes;
    for (int q = 0; q < 2 && !any; q++)
        for (size_t i = 0; i < json_len(queues[q]) && !any; i++) {
            const json_t *m = json_at(queues[q], i);
            any = is_record(m) && wakes(m);
        }
    bool steering_changed = false;
    if (any)
        for (int q = 0; q < 2; q++)
            for (size_t i = 0; i < json_len(queues[q]);) {
                if (is_record(json_at(queues[q], i))) {
                    json_push(out, json_arr_remove(queues[q], i));
                    steering_changed |= q == 0;
                } else i++;
            }
    pthread_mutex_unlock(&a->mu);
    if (steering_changed) agent_queue_changed(a);
    return out;
}

void agent_clear_queues(agent_t *a) {
    pthread_mutex_lock(&a->mu);
    json_free(a->steering);
    json_free(a->follow_up);
    json_free(a->asides);
    a->steering = json_arr();
    a->follow_up = json_arr();
    a->asides = json_arr();
    pthread_mutex_unlock(&a->mu);
    agent_queue_changed(a);
}

bool agent_is_running(agent_t *a) { return a->running; }

bool agent_msg_user_authored(const json_t *msg) {
    return str_eq(json_get_str(msg, "role"), "user") && !json_get_bool(msg, "synthetic", false) &&
           !str_eq(json_get_str(msg, "attribution"), "agent");
}

char *agent_queued_text(const json_t *msg) {
    /* a queued /skill: invocation shows its chip text (queueChipText), never the expanded skill body */
    const char *chip = json_get_str(json_get(msg, "details"), "prompt");
    if (chip && str_eq(json_get_str(msg, "customType"), "skill-prompt")) return xstrdup(chip);
    const json_t *c = json_get(msg, "content");
    if (json_is_str(c)) return xstrdup(json_sv(c));
    const json_t *b;
    bool image = false;
    json_arr_foreach(c, i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "text")) return xstrdup(json_get_str(b, "text") ? json_get_str(b, "text") : "");
        if (str_eq(t, "image")) image = true;
    }
    return xstrdup(image ? "[Image]" : "");
}

void agent_queue_texts(agent_t *a, strvec_t *steering, strvec_t *follow_up) {
    pthread_mutex_lock(&a->mu);
    const json_t *m;
    if (steering) json_arr_foreach(a->steering, i, m) if (agent_msg_user_authored(m)) strvec_push_own(steering, agent_queued_text(m));
    if (follow_up) json_arr_foreach(a->follow_up, i, m) if (agent_msg_user_authored(m)) strvec_push_own(follow_up, agent_queued_text(m));
    pthread_mutex_unlock(&a->mu);
}

json_t *agent_pop_last_queued(agent_t *a) {
    json_t *out = NULL;
    pthread_mutex_lock(&a->mu);
    json_t *queues[2] = {a->steering, a->follow_up};
    for (int q = 0; q < 2 && !out; q++) {
        for (size_t i = json_len(queues[q]); i > 0; i--) {
            if (agent_msg_user_authored(json_at(queues[q], i - 1))) {
                out = json_arr_remove(queues[q], i - 1);
                break;
            }
        }
    }
    pthread_mutex_unlock(&a->mu);
    return out;
}

json_t *agent_take_user_queued(agent_t *a) {
    json_t *r = json_obj();
    pthread_mutex_lock(&a->mu);
    json_t *queues[2] = {a->steering, a->follow_up};
    const char *keys[2] = {"steering", "followUp"};
    for (int q = 0; q < 2; q++) {
        json_t *taken = json_arr();
        for (size_t i = 0; i < json_len(queues[q]);) {
            if (agent_msg_user_authored(json_at(queues[q], i))) json_push(taken, json_arr_remove(queues[q], i));
            else i++;
        }
        json_set(r, keys[q], taken);
    }
    pthread_mutex_unlock(&a->mu);
    return r;
}

size_t agent_queued_count(agent_t *a) {
    pthread_mutex_lock(&a->mu);
    size_t n = json_len(a->steering) + json_len(a->follow_up);
    pthread_mutex_unlock(&a->mu);
    return n;
}

bool agent_has_interruptible_input(agent_t *a) {
    bool r = false;
    pthread_mutex_lock(&a->mu);
    json_t *queues[] = {a->steering, a->follow_up}, *m;
    for (int q = 0; q < 2 && !r; q++) json_arr_foreach(queues[q], i, m) if (agent_msg_user_authored(m)) r = true;
    pthread_mutex_unlock(&a->mu);
    return r;
}

size_t agent_steering_count(agent_t *a) {
    pthread_mutex_lock(&a->mu);
    size_t n = json_len(a->steering);
    pthread_mutex_unlock(&a->mu);
    return n;
}

void agent_prioritize_steering(agent_t *a, size_t from) {
    pthread_mutex_lock(&a->mu);
    size_t n = json_len(a->steering);
    for (size_t i = from, at = 0; i < n; i++, at++) json_arr_insert(a->steering, at, json_arr_remove(a->steering, i));
    pthread_mutex_unlock(&a->mu);
}

bool agent_steering_has(agent_t *a, const json_t *msg) {
    pthread_mutex_lock(&a->mu);
    bool r = false;
    for (size_t i = 0; i < json_len(a->steering) && !r; i++) r = json_at(a->steering, i) == msg;
    pthread_mutex_unlock(&a->mu);
    return r;
}

const json_t *agent_steering_at(agent_t *a, size_t i) {
    pthread_mutex_lock(&a->mu);
    const json_t *m = json_at(a->steering, i);
    pthread_mutex_unlock(&a->mu);
    return m;
}

/* Dequeue per mode: everything ("all") or the head message ("one-at-a-time", omp's default). */
static json_t *take_queue(agent_t *a, json_t **q) {
    pthread_mutex_lock(&a->mu);
    json_t *out;
    bool all = q == &a->steering ? a->steer_all : a->follow_up_all;
    if (all || json_len(*q) <= 1) {
        out = *q;
        *q = json_arr();
    } else {
        out = json_arr();
        json_push(out, json_arr_remove(*q, 0));
    }
    pthread_mutex_unlock(&a->mu);
    if (!json_len(out)) {
        json_free(out);
        return NULL;
    }
    agent_queue_changed(a);
    return out;
}

/* every queued aside (omp polls them at each step boundary; they have no one-at-a-time mode) */
static json_t *take_asides(agent_t *a) {
    pthread_mutex_lock(&a->mu);
    json_t *out = NULL;
    if (json_len(a->asides)) {
        out = a->asides;
        a->asides = json_arr();
    }
    pthread_mutex_unlock(&a->mu);
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
    json_t *cursor_results; /* cursor-agent: tool results the provider resolved during the stream */
} stream_ud_t;

static json_t *cursor_bridge_run(void *ud, const char *id, const char *name, const json_t *args);
static void cursor_bridge_result(void *ud, json_t *tool_result);

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
    if (a->hooks.on_stream_delta && !s->abort_requested &&
        (ev->type == AI_EV_TEXT_DELTA || ev->type == AI_EV_THINKING_DELTA || ev->type == AI_EV_TOOLCALL_DELTA ||
         ev->type == AI_EV_TEXT_START || ev->type == AI_EV_THINKING_START || ev->type == AI_EV_THINKING_END ||
         ev->type == AI_EV_TOOLCALL_START)) {
        int r = a->hooks.on_stream_delta(a->hooks.ud, a, ev);
        if (r) {
            s->abort_requested = r;
            a->cancel = 1;
        }
    }
}

static json_t *stream_assistant(agent_t *a, int *ttsr_abort, json_t **cursor_results) {
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
    json_t *tools = json_arr();
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->tools.n; i++) json_push(tools, agent_tool_wire(a->tools.v[i]));
    json_t *sys = json_clone(a->system_prompt);
    pthread_mutex_unlock(&a->mu);
    /* provider-boundary image policy (omp transformProviderContext): count/byte budgets, STB WebP rewrite,
     * undecodable images degraded to text; the session history itself is never modified */
    ai_prepare_context_images(snapshot, a->model, sys, tools);
    ctx_msgs = ai_transform_messages(snapshot, a->model);
    json_free(snapshot);

    ai_context_t ctx = {.system_prompt = sys, .messages = ctx_msgs, .tools = json_len(tools) ? tools : NULL};
    ai_stream_opts_t opts = a->base_opts;
    opts.api_key = a->api_key;
    opts.session_id = a->session_id;
    opts.provider_session_state = true;
    opts.reasoning = model_clamp_effort(a->model, a->thinking);
    opts.cancel = &a->cancel;
    if (a->hooks.prepare_request) a->hooks.prepare_request(a->hooks.ud, a, &opts);

    stream_ud_t s = {.a = a, .cursor_results = json_arr()};
    /* Cursor runs its tools through exec frames mid-stream; they execute here as local tools */
    ai_cursor_exec_t bridge = {.run = cursor_bridge_run, .result = cursor_bridge_result, .ud = &s};
    if (a->model->api && strcmp(a->model->api, "cursor-agent") == 0 && !opts.cursor_exec) opts.cursor_exec = &bridge;
    bool silent = a->silent_abort_next;
    if (silent) {
        a->silent_abort_next = 0;
        a->cancel = 1;
    }
    if (a->hooks.on_request) a->hooks.on_request(a->hooks.ud, a, a->model, &ctx, &opts);
    json_t *msg = ai_stream(a->model, &ctx, &opts, on_ai_event, &s);
    if (silent && str_eq(json_get_str(msg, "stopReason"), "aborted")) {
        /* SILENT_ABORT_MARKER + AIError.create(AIError.Flag.SilentAbort) */
        json_set_str(msg, "errorMessage", "__omp.silent_abort__");
        json_set_int(msg, "errorId", 33558528);
    }
    if (silent) a->cancel = 0; /* internal transition: later side requests must not inherit the abort */
    if (!s.started) emit_message(a, AGENT_EV_MESSAGE_START, msg);
    if (!s.abort_requested && a->hooks.on_stream_delta) { /* the finished stream (loop guard flush) */
        const char *st = json_get_str(msg, "stopReason");
        if (!str_eq(st, "error") && !str_eq(st, "aborted")) {
            ai_event_t done = {.type = AI_EV_DONE, .content_index = -1, .partial = msg, .reason = st};
            s.abort_requested = a->hooks.on_stream_delta(a->hooks.ud, a, &done) == AGENT_STREAM_ERROR ? AGENT_STREAM_ERROR : 0;
        }
    }
    *ttsr_abort = s.abort_requested;
    *cursor_results = s.cursor_results;
    json_free(ctx_msgs);
    json_free(tools);
    json_free(sys);
    return msg;
}

/* ---------- process-wide pause gate (pause.ts) ---------- */

static pthread_mutex_t g_pause_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pause_cv = PTHREAD_COND_INITIALIZER;
static bool g_paused;
static int64_t g_paused_at;
static int g_live_loops; /* running loops not parked */

bool agent_pause_gate_pause(void) {
    pthread_mutex_lock(&g_pause_mu);
    bool ok = !g_paused;
    if (ok) {
        g_paused = true;
        g_paused_at = now_ms();
    }
    pthread_mutex_unlock(&g_pause_mu);
    return ok;
}

int64_t agent_pause_gate_resume(void) {
    pthread_mutex_lock(&g_pause_mu);
    int64_t held = -1;
    if (g_paused) {
        g_paused = false;
        held = now_ms() - g_paused_at;
        g_paused_at = 0;
        pthread_cond_broadcast(&g_pause_cv);
    }
    pthread_mutex_unlock(&g_pause_mu);
    return held;
}

bool agent_pause_gate_paused(void) {
    pthread_mutex_lock(&g_pause_mu);
    bool p = g_paused;
    pthread_mutex_unlock(&g_pause_mu);
    return p;
}

int64_t agent_pause_gate_paused_at(void) {
    pthread_mutex_lock(&g_pause_mu);
    int64_t t = g_paused ? g_paused_at : 0;
    pthread_mutex_unlock(&g_pause_mu);
    return t;
}

int agent_unsettled_loops(void) {
    pthread_mutex_lock(&g_pause_mu);
    int n = g_live_loops;
    pthread_mutex_unlock(&g_pause_mu);
    return n;
}

static void live_loops_add(int d) {
    pthread_mutex_lock(&g_pause_mu);
    g_live_loops += d;
    pthread_mutex_unlock(&g_pause_mu);
}

/* waitUntilResumed: park while the gate is engaged; the run's cancel releases only this wait. The loop counts
 * as settled while any of its waits (turn boundary or a not-yet-started tool) is parked. */
static void pause_park(agent_t *a) {
    pthread_mutex_lock(&g_pause_mu);
    if (!g_paused || a->cancel) {
        pthread_mutex_unlock(&g_pause_mu);
        return;
    }
    if (a->pause_parked++ == 0) g_live_loops--;
    while (g_paused && !a->cancel) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_pause_cv, &g_pause_mu, &ts);
    }
    if (--a->pause_parked == 0) g_live_loops++;
    pthread_mutex_unlock(&g_pause_mu);
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
    char *intent;          /* extracted `i` (owned) */
    size_t intent_too_long; /* >0: length of an over-long `i` (tool not run) */
} tool_job_t;

static void tool_update_cb(tool_ctx_t *ctx, const tool_result_t *partial) {
    tool_job_t *j = (tool_job_t *)((char *)ctx - offsetof(tool_job_t, ctx));
    agent_event_t ev = {.type = AGENT_EV_TOOL_UPDATE, .tool_call_id = ctx->tool_call_id, .tool_name = ctx->tool_name,
                        .args = j->args, .result = partial};
    emit(j->a, &ev);
}

static void run_tool(tool_job_t *j) {
    agent_t *a = j->a;
    if (!j->skip) pause_park(a); /* tools already running are unaffected (pausing never aborts) */
    const char *name = json_get_str(j->call, "name");
    const char *id = json_get_str(j->call, "id");
    agent_event_t st = {.type = AGENT_EV_TOOL_START, .tool_call_id = id, .tool_name = name, .args = j->args,
                        .intent = j->intent};
    emit(a, &st);
    if (j->skip) {
        tool_result_error(&j->res, "Skipped due to queued user message.");
    } else if (!j->tool) {
        tool_result_validation_error(&j->res, "Tool %s not found", name ? name : "(null)");
    } else if (j->intent_too_long) {
        tool_result_error(&j->res,
                          "`%s` is a short intent label (at most %d chars); the value you sent is %zu chars. The tool was "
                          "not run. Put that content in the tool's own parameters and retry with a brief `%s`.",
                          INTENT_FIELD, INTENT_MAX_LENGTH, j->intent_too_long, INTENT_FIELD);
    } else {
        char *reason = NULL;
        tool_gate_t gate = TOOL_ALLOW;
        if (a->hooks.before_tool) gate = a->hooks.before_tool(a->hooks.ud, a, j->call, &reason);
        if (gate == TOOL_BLOCK) {
            tool_result_error(&j->res, "%s", reason ? reason : "Tool call was blocked");
        } else {
            /* omp validateToolArguments: normalized/coerced args replace the call's; a lenient tool gets its raw
             * args when they still fail */
            char *recv = NULL;
            char *verr = schema_validate_args(j->tool->parameters, j->args, &recv);
            if (verr && j->tool->lenient_args) {
                free(verr);
                verr = NULL;
            }
            if (verr) {
                tool_result_validation_error(&j->res, "Validation failed for tool \"%s\":\n%s\n\nReceived arguments:\n%s", name, verr, recv);
                free(recv);
                free(verr);
            } else if (a->hooks.approve_tool &&
                       a->hooks.approve_tool(a->hooks.ud, a, j->call, j->args, &reason) == TOOL_BLOCK) {
                free(recv);
                tool_result_error(&j->res, "%s", reason ? reason : "Tool call was blocked");
            } else {
                free(recv);
                j->ctx.agent = a;
                j->ctx.tool_call_id = id;
                j->ctx.tool_name = name;
                j->ctx.cancel = &a->cancel;
                j->ctx.update = tool_update_cb;
                j->ctx.app = a->app;
                j->ctx.intent = j->intent;
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

/* ---------- cursor-agent exec bridge (omp coding-agent cursor.ts CursorExecHandlers) ---------- */

static json_t *cursor_unavailable(agent_t *a, const char *id, const char *name) {
    sb_t names = SB_INIT;
    pthread_mutex_lock(&a->mu);
    size_t n = a->tools.n;
    for (size_t i = 0; i < n && i < 12; i++) sb_appendf(&names, "%s%s", i ? ", " : "", a->tools.v[i]->name);
    pthread_mutex_unlock(&a->mu);
    char *extra = n > 12 ? xasprintf(" and %zu more", n - 12) : xstrdup("");
    tool_result_t r = {0};
    tool_result_textf(&r, "Tool \"%s\" not available to this agent; calling it again fails the same way. Use one of: %s%s.",
                      name, names.len ? names.s : "none", extra);
    r.is_error = true;
    r.details = json_obj();
    json_t *m = msg_tool_result(id, name, &r);
    tool_result_free(&r);
    free(extra);
    sb_free(&names);
    return m;
}

/* Cursor's native delete frame removes the file directly (no registry tool backs it) */
static json_t *cursor_delete(agent_t *a, const char *id, const json_t *args) {
    const char *path = json_get_str(args, "path");
    agent_event_t st = {.type = AGENT_EV_TOOL_START, .tool_call_id = id, .tool_name = "delete", .args = args};
    emit(a, &st);
    tool_result_t r = {0};
    json_t *call = json_obj();
    json_set_str(call, "type", "toolCall");
    json_set_str(call, "id", id);
    json_set_str(call, "name", "delete");
    json_set(call, "arguments", json_clone(args));
    char *reason = NULL;
    if (a->hooks.before_tool && a->hooks.before_tool(a->hooks.ud, a, call, &reason) == TOOL_BLOCK) {
        tool_result_error(&r, "%s", reason ? reason : "Tool call was blocked");
    } else {
        struct stat sbuf;
        if (!path || stat(path, &sbuf) != 0) {
            tool_result_error(&r, "File not found: %s", path ? path : "");
        } else if (!S_ISREG(sbuf.st_mode)) {
            tool_result_error(&r, "Path is not a file: %s", path);
        } else if (unlink(path) != 0) {
            tool_result_error(&r, "%s", strerror(errno));
        } else {
            if (sbuf.st_size) tool_result_textf(&r, "Deleted %s (%lld bytes)", path, (long long)sbuf.st_size);
            else tool_result_textf(&r, "Deleted %s", path);
            r.details = json_obj();
            json_set_int(r.details, "fileSize", (int64_t)sbuf.st_size);
        }
    }
    free(reason);
    json_free(call);
    if (r.is_error && !r.details) r.details = json_obj();
    agent_event_t en = {.type = AGENT_EV_TOOL_END, .tool_call_id = id, .tool_name = "delete", .result = &r,
                        .is_error = r.is_error};
    emit(a, &en);
    json_t *m = msg_tool_result(id, "delete", &r);
    tool_result_free(&r);
    return m;
}

static json_t *cursor_bridge_run(void *ud, const char *id, const char *name, const json_t *args) {
    stream_ud_t *s = ud;
    agent_t *a = s->a;
    agent_tool_t *tool = agent_find_tool(a, name);
    if (!tool && strcmp(name, "delete") == 0) return cursor_delete(a, id, args);
    if (!tool) return cursor_unavailable(a, id, name);
    tool_job_t j = {0};
    json_t *call = json_obj();
    json_set_str(call, "type", "toolCall");
    json_set_str(call, "id", id);
    json_set_str(call, "name", name);
    json_set(call, "arguments", json_is_obj(args) ? json_clone(args) : json_obj());
    j.a = a;
    j.call = call;
    j.tool = tool;
    j.args = json_is_obj(args) ? json_clone(args) : json_obj();
    run_tool(&j);
    json_t *m = msg_tool_result(id, name, &j.res);
    tool_result_free(&j.res);
    json_free(j.args);
    json_free(call);
    return m;
}

static void cursor_bridge_result(void *ud, json_t *tool_result) {
    stream_ud_t *s = ud;
    /* a re-dispatched call (same id) keeps only its newest result */
    const char *id = json_get_str(tool_result, "toolCallId");
    for (size_t i = 0; i < json_len(s->cursor_results); i++) {
        if (str_eq(json_get_str(json_at(s->cursor_results, i), "toolCallId"), id)) {
            json_free(json_arr_remove(s->cursor_results, i));
            break;
        }
    }
    json_push(s->cursor_results, tool_result);
}

static bool resolved_call(const json_t *resolved, const char *id) {
    json_t *r;
    json_arr_foreach(resolved, i, r) if (str_eq(json_get_str(r, "toolCallId"), id)) return true;
    return false;
}

static void execute_tools(agent_t *a, json_t *run_msgs, const json_t *assistant, json_t *results_out,
                          const json_t *resolved) {
    json_t *content = json_get(assistant, "content");
    VEC(tool_job_t *) jobs = {0};
    json_t *blk;
    json_arr_foreach(content, i, blk) {
        if (!str_eq(json_get_str(blk, "type"), "toolCall")) continue;
        if (resolved_call(resolved, json_get_str(blk, "id"))) continue;
        tool_job_t *j = xcalloc(1, sizeof(*j));
        j->a = a;
        j->call = blk;
        const char *name = json_get_str(blk, "name");
        j->tool = name ? agent_find_tool(a, name) : NULL;
        json_t *args = json_get(blk, "arguments");
        j->args = json_is_obj(args) ? json_clone(args) : json_obj();
        j->intent = extract_intent(j->tool, j->args, &j->intent_too_long);
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
        if (!a->interrupt_wait && queue_nonempty(a, a->steering)) {
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
        free(j->intent);
        free(j);
    }
    vec_free(&jobs);
}

/* ---------- loop ---------- */

/* agent-loop.ts createAbortedToolResult: each tool call of a turn that ended in error/aborted gets a synthetic
 * result (the call never ran), emitted like a tool execution; the caller adds them to the history */
static json_t *failed_turn_results(agent_t *a, const json_t *assistant, const json_t *cursor_results) {
    const char *stop = json_get_str(assistant, "stopReason");
    const char *err = json_get_str(assistant, "errorMessage");
    bool aborted = str_eq(stop, "aborted");
    json_t *out = NULL;
    json_t *blk;
    json_arr_foreach(json_get(assistant, "content"), i, blk) {
        const char *id = json_get_str(blk, "id");
        if (!str_eq(json_get_str(blk, "type"), "toolCall") || resolved_call(cursor_results, id)) continue;
        const char *base = aborted ? "Tool execution was aborted"
                                   : "Tool call was not executed because the provider stream ended with an error before the "
                                     "tool could run";
        tool_result_t r = {.content = json_arr(), .details = json_obj(), .is_error = true};
        char *txt = err ? xasprintf("%s: %s", base, err) : xasprintf("%s.", base);
        json_push(r.content, text_block(txt));
        free(txt);
        json_set_bool(r.details, "__synthetic", true);
        json_set_str(r.details, "source", aborted ? "assistant_stop_aborted" : "assistant_stop_error");
        json_set_bool(r.details, "executed", false);
        if (!aborted && err) json_set_str(r.details, "upstreamError", err);
        const char *name = json_get_str(blk, "name");
        agent_event_t st = {.type = AGENT_EV_TOOL_START, .tool_call_id = id, .tool_name = name,
                            .args = json_get(blk, "arguments")};
        emit(a, &st);
        agent_event_t en = {.type = AGENT_EV_TOOL_END, .tool_call_id = id, .tool_name = name, .result = &r, .is_error = true};
        emit(a, &en);
        json_t *m = msg_tool_result(id ? id : "", name ? name : "", &r);
        emit_message(a, AGENT_EV_MESSAGE_START, m);
        emit_message(a, AGENT_EV_MESSAGE_END, m);
        if (!out) out = json_arr();
        json_push(out, m);
        tool_result_free(&r);
    }
    return out;
}

static void run_loop(agent_t *a, json_t *new_messages, bool poll_steering) {
    a->running = true;
    a->cancel = 0;
    a->run_started_mono = mono_ms();
    live_loops_add(1);
    json_t *run_msgs = json_arr();
    emit_simple(a, AGENT_EV_AGENT_START);
    json_t *m;
    /* agent-loop.ts runLoop: the first turn opens before its input messages are emitted */
    bool turn_open = false;
    json_t *early = poll_steering ? take_queue(a, &a->steering) : NULL;
    if (json_len(new_messages) || early) {
        emit_simple(a, AGENT_EV_TURN_START);
        turn_open = true;
    }
    if (new_messages) {
        json_arr_foreach(new_messages, i, m) add_message(a, run_msgs, json_clone(m));
    }
    /* steering typed while the run was starting joins its first turn (agent-loop.ts) */
    if (early) {
        json_arr_foreach(early, i, m) add_message(a, run_msgs, json_clone(m));
        json_free(early);
    }
    int turns = 0;
    int retry_attempt = 0;
    int paused_continuations = 0; /* MAX_PAUSED_TURN_CONTINUATIONS (agent-loop.ts) */
    for (;;) {
        if (a->hooks.before_model_call && a->hooks.before_model_call(a->hooks.ud, a)) break;
        pause_park(a); /* the turn boundary: park while /pause holds the process */
        if (!turn_open) emit_simple(a, AGENT_EV_TURN_START);
        turn_open = false;
        int ttsr = 0;
        json_t *cursor_results = NULL;
        json_t *assistant = stream_assistant(a, &ttsr, &cursor_results);
        model_perf_record_message(assistant); /* per-model t/s + TTFT (omp recordModelPerf at message_end) */
        if (ttsr == AGENT_STREAM_ERROR) {
            /* the stream guard cut the response off: an empty, retryable error turn (thinking-loop guard) */
            json_set(assistant, "content", json_arr());
            json_set_str(assistant, "stopReason", "error");
            json_set_str(assistant, "errorMessage", a->stream_error ? a->stream_error : "Stream aborted by guard");
            a->cancel = 0;
            ttsr = 0;
        }
        if (!ttsr && a->hooks.transform_assistant) a->hooks.transform_assistant(a->hooks.ud, a, assistant);
        const char *stop = json_get_str(assistant, "stopReason");
        bool failed = stop && (strcmp(stop, "error") == 0 || strcmp(stop, "aborted") == 0);
        json_t *synth = NULL; /* synthetic results of a failed turn's calls: emitted, not yet in the history */
        int outcome = AGENT_RETRY_NONE;
        json_t *append = NULL;
        if (ttsr && a->hooks.should_retry) {
            /* stream deliberately aborted by a rule: let the hook inject and retry */
            emit_message(a, AGENT_EV_MESSAGE_END, assistant);
            a->cancel = 0;
            if (a->hooks.should_retry(a->hooks.ud, a, assistant, -1, &append)) {
                json_free(append);
                json_free(assistant);
                json_free(cursor_results);
                agent_event_t te = {.type = AGENT_EV_TURN_END, .message = NULL, .tool_results = NULL};
                emit(a, &te);
                continue;
            }
            failed = true;
        } else if (failed && !a->cancel && a->hooks.should_retry) {
            /* the failed attempt ends before the retry policy runs (its notices follow it, as in omp) */
            emit_message(a, AGENT_EV_MESSAGE_END, assistant);
            synth = failed_turn_results(a, assistant, cursor_results);
            outcome = a->hooks.should_retry(a->hooks.ud, a, assistant, retry_attempt, &append);
            if (outcome == AGENT_RETRY_REPLAY) {
                retry_attempt++;
                json_free(append);
                json_free(synth);
                json_free(assistant);
                json_free(cursor_results);
                continue;
            }
            if (outcome == AGENT_RETRY_PRESERVE) retry_attempt++;
        } else {
            emit_message(a, AGENT_EV_MESSAGE_END, assistant);
            if (failed) synth = failed_turn_results(a, assistant, cursor_results);
        }
        bool preserved = outcome == AGENT_RETRY_PRESERVE || outcome == AGENT_RETRY_CONTINUE;
        /* the session stamps completedAt on the message_end snapshot it persists; the live history keeps the
         * provider's message (omp: agent state and the message_end event are separate snapshots) */
        json_del(assistant, "completedAt");
        if (!preserved) retry_attempt = 0;
        pthread_mutex_lock(&a->mu);
        json_push(a->messages, assistant);
        pthread_mutex_unlock(&a->mu);
        json_push(run_msgs, json_clone(assistant));
        json_t *results = json_arr();
        /* results of calls Cursor resolved mid-stream follow the assistant message */
        json_t *cr;
        json_arr_foreach(cursor_results, i, cr) {
            json_push(results, json_clone(cr));
            add_message(a, run_msgs, json_clone(cr));
        }
        json_arr_foreach(synth, i, cr) {
            json_push(results, json_clone(cr));
            pthread_mutex_lock(&a->mu);
            json_push(a->messages, json_clone(cr));
            pthread_mutex_unlock(&a->mu);
            json_push(run_msgs, json_clone(cr));
        }
        json_free(synth);
        if (preserved) {
            /* the failed turn stays in context (its calls carry `executed: false` results) and the run goes on */
            json_free(cursor_results);
            agent_event_t te = {.type = AGENT_EV_TURN_END, .message = assistant, .tool_results = results};
            emit(a, &te);
            json_free(results);
            if (append) { /* Agent.appendMessage: joins the context without message events (not persisted) */
                json_push(run_msgs, json_clone(append));
                pthread_mutex_lock(&a->mu);
                json_push(a->messages, append);
                pthread_mutex_unlock(&a->mu);
            }
            if (a->cancel) break;
            continue;
        }
        json_free(append);
        bool has_tools = false;
        json_t *blk;
        json_arr_foreach(json_get(assistant, "content"), i, blk) {
            if (str_eq(json_get_str(blk, "type"), "toolCall") && !resolved_call(cursor_results, json_get_str(blk, "id")))
                has_tools = true;
        }
        if (!failed && has_tools) execute_tools(a, run_msgs, assistant, results, cursor_results);
        json_free(cursor_results);
        agent_event_t te = {.type = AGENT_EV_TURN_END, .message = assistant, .tool_results = results};
        emit(a, &te);
        json_free(results);
        turns++;
        if (failed || a->cancel) break;
        if (a->max_turns && turns >= a->max_turns) break;
        json_t *steer = take_queue(a, &a->steering), *asides = take_asides(a);
        if (steer || asides) {
            if (steer) json_arr_foreach(steer, i, m) add_message(a, run_msgs, json_clone(m));
            if (asides) json_arr_foreach(asides, i, m) add_message(a, run_msgs, json_clone(m));
            json_free(steer);
            json_free(asides);
            continue;
        }
        if (has_tools) {
            paused_continuations = 0;
            if (a->hooks.turn_boundary && !a->cancel) a->hooks.turn_boundary(a->hooks.ud, a, assistant);
            continue;
        }
        /* non-terminal stop (stopDetails.type pause_turn): re-sample so the provider continues the turn */
        if (!failed && str_eq(json_get_str(assistant, "stopReason"), "stop") &&
            str_eq(json_get_str(json_get_obj(assistant, "stopDetails"), "type"), "pause_turn") && paused_continuations < 8) {
            paused_continuations++;
            continue;
        }
        json_t *fu = take_queue(a, &a->follow_up);
        if (fu) {
            json_arr_foreach(fu, i, m) add_message(a, run_msgs, json_clone(m));
            json_free(fu);
            continue;
        }
        break;
    }
    /* IrcBridge.flushPending: asides that missed their step boundary (an aborted or capped run) still reach the transcript */
    json_t *late = take_asides(a);
    if (late) {
        json_arr_foreach(late, i, m) add_message(a, run_msgs, json_clone(m));
        json_free(late);
    }
    agent_event_t ee = {.type = AGENT_EV_AGENT_END, .messages = run_msgs};
    a->silent_abort_next = 0;
    a->running = false;
    live_loops_add(-1);
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
    run_loop(a, new_messages, true);
    json_free(new_messages);
    return 0;
}

int agent_run_queued(agent_t *a) {
    if (a->running || !a->model) return -1;
    json_t *q = take_queue(a, &a->steering);
    if (!q) q = take_queue(a, &a->follow_up);
    if (!q) return -1;
    run_loop(a, q, false);
    json_free(q);
    return 0;
}

int agent_continue(agent_t *a) {
    if (a->running || !a->model) return -1;
    run_loop(a, NULL, false);
    return 0;
}

