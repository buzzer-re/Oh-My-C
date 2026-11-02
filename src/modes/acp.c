/* acp.c - Agent Client Protocol (JSON-RPC 2.0 over NDJSON stdio) for editor integration (Zed, ...). */
#include "modes.h"

#include "config/settings.h"
#include "core/fs.h"

#include <errno.h>
#include <pthread.h>
#include <unistd.h>

typedef struct acp acp_t;

typedef struct {
    char *id;
    app_t *app;
    acp_t *acp;
    pthread_t th;
    bool running;
    json_t *prompt_req_id;
    json_t *prompt_content;
    bool always_allow;
} asess_t;

typedef struct {
    int64_t id;
    json_t *result;
    bool done;
} apend_t;

struct acp {
    VEC(asess_t *) sessions;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    VEC(apend_t *) pending; /* outgoing requests awaiting client responses */
    int64_t next_id;
    const app_opts_t *base;
};

static void send_obj(json_t *o) {
    json_set_str(o, "jsonrpc", "2.0");
    out_json_line(o);
    json_free(o);
}

static void reply(const json_t *id, json_t *result) {
    json_t *o = json_obj();
    json_set(o, "id", json_clone(id));
    json_set(o, "result", result ? result : json_obj());
    send_obj(o);
}

static void reply_err(const json_t *id, int code, const char *msg) {
    json_t *o = json_obj();
    json_set(o, "id", json_clone(id));
    json_t *e = json_obj();
    json_set_int(e, "code", code);
    json_set_str(e, "message", msg);
    json_set(o, "error", e);
    send_obj(o);
}

static void notify_update(const char *sid, json_t *update) {
    json_t *o = json_obj();
    json_set_str(o, "method", "session/update");
    json_t *p = json_obj();
    json_set_str(p, "sessionId", sid);
    json_set(p, "update", update);
    json_set(o, "params", p);
    send_obj(o);
}

/* outgoing request to the client, blocking until its response */
static json_t *client_request(acp_t *a, const char *method, json_t *params, int64_t timeout_ms) {
    apend_t *p = xcalloc(1, sizeof(*p));
    pthread_mutex_lock(&a->mu);
    p->id = ++a->next_id;
    vec_push(&a->pending, p);
    pthread_mutex_unlock(&a->mu);
    json_t *o = json_obj();
    json_set_int(o, "id", p->id);
    json_set_str(o, "method", method);
    json_set(o, "params", params);
    send_obj(o);
    pthread_mutex_lock(&a->mu);
    int64_t deadline = mono_ms() + timeout_ms;
    while (!p->done && mono_ms() < deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&a->cv, &a->mu, &ts);
    }
    json_t *r = p->done ? p->result : NULL;
    for (size_t i = 0; i < a->pending.n; i++)
        if (a->pending.v[i] == p) {
            memmove(a->pending.v + i, a->pending.v + i + 1, (a->pending.n - i - 1) * sizeof(void *));
            a->pending.n--;
            break;
        }
    pthread_mutex_unlock(&a->mu);
    free(p);
    return r;
}

static const char *tool_kind(const char *name) {
    if (!name) return "other";
    if (!strcmp(name, "read") || !strcmp(name, "glob")) return "read";
    if (!strcmp(name, "write") || !strcmp(name, "edit") || !strcmp(name, "ast_edit")) return "edit";
    if (!strcmp(name, "grep") || !strcmp(name, "find") || !strcmp(name, "ast_grep") || !strcmp(name, "web_search")) return "search";
    if (!strcmp(name, "bash") || !strcmp(name, "eval")) return "execute";
    if (!strcmp(name, "think")) return "think";
    return "other";
}

static char *tool_title(const char *name, const json_t *args) {
    const char *keys[] = {"path", "command", "pattern", "query", "pat"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        const char *v = json_get_str(args, keys[i]);
        if (v) return xasprintf("%s %.*s", name, (int)MIN(strcspn(v, "\n"), (size_t)100), v);
    }
    return xstrdup(name ? name : "tool");
}

/* ---------- agent events -> session/update ---------- */

static void acp_listener(void *ud, const agent_event_t *ev) {
    asess_t *s = ud;
    if (ev->type == AGENT_EV_MESSAGE_UPDATE && ev->ai) {
        if (ev->ai->type == AI_EV_TEXT_DELTA || ev->ai->type == AI_EV_THINKING_DELTA) {
            json_t *u = json_obj();
            json_set_str(u, "sessionUpdate", ev->ai->type == AI_EV_TEXT_DELTA ? "agent_message_chunk" : "agent_thought_chunk");
            json_t *c = json_obj();
            json_set_str(c, "type", "text");
            json_set(c, "text", json_strn(ev->ai->delta, ev->ai->delta_len));
            json_set(u, "content", c);
            notify_update(s->id, u);
        }
    } else if (ev->type == AGENT_EV_TOOL_START) {
        json_t *u = json_obj();
        json_set_str(u, "sessionUpdate", "tool_call");
        json_set_str(u, "toolCallId", ev->tool_call_id);
        char *t = tool_title(ev->tool_name, ev->args);
        json_set_str(u, "title", t);
        free(t);
        json_set_str(u, "kind", tool_kind(ev->tool_name));
        json_set_str(u, "status", "in_progress");
        if (ev->args) json_set(u, "rawInput", json_clone(ev->args));
        const char *p = json_get_str(ev->args, "path");
        if (p) {
            json_t *locs = json_arr(), *l = json_obj();
            char *abs = path_abs(p, s->app->cwd);
            json_set_str(l, "path", abs);
            free(abs);
            json_push(locs, l);
            json_set(u, "locations", locs);
        }
        notify_update(s->id, u);
    } else if (ev->type == AGENT_EV_TOOL_END) {
        json_t *u = json_obj();
        json_set_str(u, "sessionUpdate", "tool_call_update");
        json_set_str(u, "toolCallId", ev->tool_call_id);
        json_set_str(u, "status", ev->is_error ? "failed" : "completed");
        json_t *content = json_arr();
        const char *diff = ev->result && ev->result->details ? json_get_str(ev->result->details, "diff") : NULL;
        char *text = ev->result ? tool_result_joined_text(ev->result) : xstrdup("");
        json_t *c = json_obj(), *inner = json_obj();
        json_set_str(c, "type", "content");
        json_set_str(inner, "type", "text");
        json_set_str(inner, "text", diff && *diff ? diff : text);
        json_set(c, "content", inner);
        json_push(content, c);
        json_set(u, "content", content);
        free(text);
        notify_update(s->id, u);
        if (str_eq(ev->tool_name, "todo") && ev->result && ev->result->details) {
            json_t *pl = json_obj(), *entries = json_arr(), *ph, *t2;
            json_set_str(pl, "sessionUpdate", "plan");
            json_arr_foreach(json_get_arr(ev->result->details, "phases"), i, ph) {
                json_arr_foreach(json_get_arr(ph, "tasks"), k, t2) {
                    json_t *e = json_obj();
                    json_set_str(e, "content", json_get_str(t2, "content"));
                    const char *st = json_get_str(t2, "status");
                    json_set_str(e, "status", str_eq(st, "completed") ? "completed" : str_eq(st, "in_progress") ? "in_progress" : "pending");
                    json_set_str(e, "priority", "medium");
                    json_push(entries, e);
                }
            }
            json_set(pl, "entries", entries);
            notify_update(s->id, pl);
        }
    } else if (ev->type == AGENT_EV_MESSAGE_END && str_eq(json_get_str(ev->message, "role"), "assistant")) {
        json_t *usage = json_get(ev->message, "usage");
        if (usage) {
            json_t *u = json_obj();
            json_set_str(u, "sessionUpdate", "usage_update");
            json_set(u, "usage", json_clone(usage));
            notify_update(s->id, u);
        }
    }
}

/* permission gate for mutating tools (client decides) */
static tool_gate_t (*g_prev_before)(void *, agent_t *, const json_t *, char **);

static tool_gate_t acp_before_tool(void *ud, agent_t *ag, const json_t *call, char **reason) {
    app_t *app = ud;
    asess_t *s = NULL;
    acp_t *a = NULL;
    /* find our session by app */
    extern acp_t *g_acp;
    a = g_acp;
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->sessions.n; i++)
        if (a->sessions.v[i]->app == app) s = a->sessions.v[i];
    pthread_mutex_unlock(&a->mu);
    if (g_prev_before) {
        tool_gate_t g = g_prev_before(ud, ag, call, reason);
        if (g == TOOL_BLOCK) return g;
    }
    const char *name = json_get_str(call, "name");
    const char *kind = tool_kind(name);
    if (!s || s->always_allow || !(str_eq(kind, "edit") || str_eq(kind, "execute"))) return TOOL_ALLOW;
    if (str_eq(settings_str("tools.approvalMode", "yolo"), "yolo") && !settings_bool("acp.askPermission", true)) return TOOL_ALLOW;
    json_t *p = json_obj();
    json_set_str(p, "sessionId", s->id);
    json_t *tc = json_obj();
    json_set_str(tc, "toolCallId", json_get_str(call, "id"));
    char *t = tool_title(name, json_get(call, "arguments"));
    json_set_str(tc, "title", t);
    free(t);
    json_set_str(tc, "kind", kind);
    json_set(tc, "rawInput", json_clone(json_get(call, "arguments")));
    json_set(p, "toolCall", tc);
    json_set(p, "options", json_parse_str("[{\"optionId\":\"allow_once\",\"name\":\"Allow\",\"kind\":\"allow_once\"},"
                                          "{\"optionId\":\"allow_always\",\"name\":\"Always allow\",\"kind\":\"allow_always\"},"
                                          "{\"optionId\":\"reject_once\",\"name\":\"Reject\",\"kind\":\"reject_once\"}]"));
    json_t *r = client_request(a, "session/request_permission", p, 30 * 60 * 1000);
    json_t *outcome = json_get(r, "outcome");
    const char *oc = json_get_str(outcome, "outcome");
    const char *opt = json_get_str(outcome, "optionId");
    tool_gate_t g = TOOL_ALLOW;
    if (!r || str_eq(oc, "cancelled") || str_starts(opt ? opt : "", "reject")) {
        *reason = xstrdup("The user rejected this tool call.");
        g = TOOL_BLOCK;
    } else if (str_eq(opt, "allow_always")) {
        s->always_allow = true;
    }
    json_free(r);
    return g;
}

acp_t *g_acp;

/* ---------- sessions ---------- */

static asess_t *find_session(acp_t *a, const char *id) {
    asess_t *r = NULL;
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->sessions.n; i++)
        if (!strcmp(a->sessions.v[i]->id, id)) r = a->sessions.v[i];
    pthread_mutex_unlock(&a->mu);
    return r;
}

/* plan approved by xd://propose (no elicitation surface: approved automatically): report the mode change */
static void acp_plan_mode_changed(void *ud) {
    asess_t *s = ud;
    json_t *u = json_obj();
    json_set_str(u, "sessionUpdate", "current_mode_update");
    json_set_str(u, "currentModeId", s->app->plan_mode ? "plan" : "default");
    notify_update(s->id, u);
}

static asess_t *open_session(acp_t *a, const char *cwd, const char *resume, char **err) {
    app_opts_t o = *a->base;
    o.cwd = cwd;
    o.resume = resume;
    o.continue_recent = false;
    app_t *app = app_create(&o, err);
    if (!app) return NULL;
    asess_t *s = xcalloc(1, sizeof(*s));
    s->app = app;
    s->acp = a;
    s->id = xstrdup(session_id(app->session));
    app->plan_mode_changed = acp_plan_mode_changed;
    app->plan_mode_changed_ud = s;
    agent_subscribe(app->agent, acp_listener, s);
    g_prev_before = app->agent->hooks.before_tool;
    app->agent->hooks.before_tool = acp_before_tool;
    pthread_mutex_lock(&a->mu);
    vec_push(&a->sessions, s);
    pthread_mutex_unlock(&a->mu);
    return s;
}

static json_t *modes_json(asess_t *s) {
    json_t *m = json_obj();
    json_set_str(m, "currentModeId", s->app->plan_mode ? "plan" : "default");
    json_t *modes = json_parse_str("[{\"id\":\"default\",\"name\":\"Default\",\"description\":\"Standard ACP headless mode\"}]");
    if (settings_bool("plan.enabled", true))
        json_push(modes, json_parse_str("{\"id\":\"plan\",\"name\":\"Plan\",\"description\":\"Read-only planning mode that drafts a "
                                        "plan to a markdown file before any code changes\"}"));
    json_set(m, "availableModes", modes);
    return m;
}

static void *prompt_thread(void *ud) {
    asess_t *s = ud;
    sb_t text = SB_INIT;
    json_t *images = json_arr();
    json_t *blk;
    json_arr_foreach(s->prompt_content, i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "text")) {
            if (text.len) sb_putc(&text, '\n');
            sb_append(&text, json_get_str(blk, "text"));
        } else if (str_eq(t, "image")) {
            json_push(images, image_block(json_get_str(blk, "data"), json_get_str(blk, "mimeType")));
        } else if (str_eq(t, "resource")) {
            json_t *r = json_get(blk, "resource");
            sb_appendf(&text, "\n<file path=\"%s\">\n%s\n</file>", json_get_str(r, "uri") ? json_get_str(r, "uri") : "",
                       json_get_str(r, "text") ? json_get_str(r, "text") : "");
        } else if (str_eq(t, "resource_link")) {
            sb_appendf(&text, "\n@%s", json_get_str(blk, "uri") ? json_get_str(blk, "uri") : "");
        }
    }
    app_prompt(s->app, sb_cstr(&text), json_len(images) ? images : (json_free(images), NULL));
    sb_free(&text);
    const json_t *last = last_assistant(s->app->agent->messages);
    const char *stop = json_get_str(last, "stopReason");
    json_t *r = json_obj();
    json_set_str(r, "stopReason", str_eq(stop, "aborted") ? "cancelled" : str_eq(stop, "length") ? "max_tokens" : "end_turn");
    if (str_eq(stop, "error")) {
        reply_err(s->prompt_req_id, -32603, json_get_str(last, "errorMessage") ? json_get_str(last, "errorMessage") : "model error");
        json_free(r);
    } else {
        reply(s->prompt_req_id, r);
    }
    json_free(s->prompt_req_id);
    json_free(s->prompt_content);
    s->prompt_req_id = NULL;
    s->prompt_content = NULL;
    s->running = false;
    return NULL;
}

static void replay_history(asess_t *s) {
    json_t *m;
    json_arr_foreach(s->app->agent->messages, i, m) {
        const char *role = json_get_str(m, "role");
        if (!str_eq(role, "user") && !str_eq(role, "assistant")) continue;
        char *t = ai_message_text(m);
        if (*t) {
            json_t *u = json_obj();
            json_set_str(u, "sessionUpdate", str_eq(role, "user") ? "user_message_chunk" : "agent_message_chunk");
            json_t *c = json_obj();
            json_set_str(c, "type", "text");
            json_set_str(c, "text", t);
            json_set(u, "content", c);
            notify_update(s->id, u);
        }
        free(t);
    }
}

static void handle(acp_t *a, json_t *msg) {
    const json_t *id = json_get(msg, "id");
    const char *method = json_get_str(msg, "method");
    json_t *params = json_get(msg, "params");
    if (!method) {
        /* response to one of our requests */
        if (json_is_num(id)) {
            int64_t iv = id->is_int ? id->u.num.i : (int64_t)id->u.num.d;
            pthread_mutex_lock(&a->mu);
            for (size_t i = 0; i < a->pending.n; i++)
                if (a->pending.v[i]->id == iv) {
                    a->pending.v[i]->result = json_take((json_t *)msg, "result");
                    a->pending.v[i]->done = true;
                    pthread_cond_broadcast(&a->cv);
                }
            pthread_mutex_unlock(&a->mu);
        }
        return;
    }
    if (!strcmp(method, "initialize")) {
        json_t *r = json_obj();
        json_set_int(r, "protocolVersion", json_get_int(params, "protocolVersion", 1) > 0 ? 1 : 1);
        json_set(r, "agentCapabilities",
                 json_parse_str("{\"loadSession\":true,\"mcpCapabilities\":{\"http\":true,\"sse\":true},"
                                "\"promptCapabilities\":{\"embeddedContext\":true,\"image\":true},"
                                "\"sessionCapabilities\":{\"list\":{}}}"));
        json_set(r, "authMethods", json_arr());
        json_t *info = json_obj();
        json_set_str(info, "name", "omc");
        json_set_str(info, "title", "omc (oh-my-c)");
        json_set_str(info, "version", OMC_VERSION);
        json_set(r, "agentInfo", info);
        reply(id, r);
    } else if (!strcmp(method, "authenticate")) {
        reply(id, json_obj());
    } else if (!strcmp(method, "session/new") || !strcmp(method, "session/load")) {
        const char *cwd = json_get_str(params, "cwd");
        const char *sid = json_get_str(params, "sessionId");
        char *err = NULL;
        asess_t *s = open_session(a, cwd ? cwd : a->base->cwd, !strcmp(method, "session/load") ? sid : NULL, &err);
        if (!s) {
            reply_err(id, -32603, err ? err : "failed to create session");
            free(err);
            return;
        }
        if (!strcmp(method, "session/load")) replay_history(s);
        json_t *r = json_obj();
        if (!strcmp(method, "session/new")) json_set_str(r, "sessionId", s->id);
        json_set(r, "modes", modes_json(s));
        reply(id, r);
    } else if (!strcmp(method, "session/list")) {
        session_list_t l;
        session_list_all(&l);
        json_t *arr = json_arr();
        for (size_t i = 0; i < l.n && i < 100; i++) {
            json_t *e = json_obj();
            json_set_str(e, "sessionId", l.v[i].id);
            json_set_str(e, "cwd", l.v[i].cwd);
            if (l.v[i].title) json_set_str(e, "title", l.v[i].title);
            char *ts = iso8601_from_ms(l.v[i].mtime_ms);
            json_set_str(e, "updatedAt", ts);
            free(ts);
            json_push(arr, e);
        }
        session_list_free(&l);
        json_t *r = json_obj();
        json_set(r, "sessions", arr);
        reply(id, r);
    } else if (!strcmp(method, "session/prompt")) {
        asess_t *s = find_session(a, json_get_str(params, "sessionId") ? json_get_str(params, "sessionId") : "");
        if (!s) {
            reply_err(id, -32602, "unknown session");
            return;
        }
        if (s->running) {
            /* steer the running turn */
            char *t = NULL;
            json_t *b0 = json_at(json_get_arr(params, "prompt"), 0);
            t = xstrdup(json_get_str(b0, "text") ? json_get_str(b0, "text") : "");
            agent_steer(s->app->agent, msg_user_text(t));
            free(t);
            json_t *r = json_obj();
            json_set_str(r, "stopReason", "end_turn");
            reply(id, r);
            return;
        }
        if (s->th) pthread_join(s->th, NULL);
        s->running = true;
        s->prompt_req_id = json_clone(id);
        s->prompt_content = json_clone(json_get_arr(params, "prompt"));
        if (pthread_create(&s->th, NULL, prompt_thread, s) != 0) {
            s->running = false;
            reply_err(id, -32603, "failed to start");
        }
    } else if (!strcmp(method, "session/cancel")) {
        asess_t *s = find_session(a, json_get_str(params, "sessionId") ? json_get_str(params, "sessionId") : "");
        if (s) app_abort(s->app);
    } else if (!strcmp(method, "session/set_mode")) {
        asess_t *s = find_session(a, json_get_str(params, "sessionId") ? json_get_str(params, "sessionId") : "");
        if (!s) {
            reply_err(id, -32602, "unknown session");
            return;
        }
        const char *mid = json_get_str(params, "modeId");
        if (!str_eq(mid, "default") && !(str_eq(mid, "plan") && settings_bool("plan.enabled", true))) {
            char *m = xasprintf("Unsupported ACP mode: %s", mid ? mid : "");
            reply_err(id, -32603, m);
            free(m);
            return;
        }
        app_set_plan_mode(s->app, str_eq(mid, "plan"));
        json_t *u = json_obj();
        json_set_str(u, "sessionUpdate", "current_mode_update");
        json_set_str(u, "currentModeId", s->app->plan_mode ? "plan" : "default");
        notify_update(s->id, u);
        reply(id, json_obj());
    } else if (id) {
        reply_err(id, -32601, "method not found");
    }
}

int run_acp_mode(const app_opts_t *base) {
    acp_t a = {0};
    pthread_mutex_init(&a.mu, NULL);
    pthread_cond_init(&a.cv, NULL);
    a.base = base;
    g_acp = &a;
    setenv("OMC_NO_TITLE", "1", 0);
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
            if (line.len) {
                json_t *msg = json_parse(line.s, line.len, NULL);
                if (json_is_obj(msg)) {
                    /* responses must be handled even while handlers block, so dispatch responses inline */
                    handle(&a, msg);
                }
                json_free(msg);
            }
            sb_clear(&line);
        }
    }
    for (size_t i = 0; i < a.sessions.n; i++) {
        asess_t *s = a.sessions.v[i];
        if (s->running) app_abort(s->app);
        if (s->th) pthread_join(s->th, NULL);
        app_free(s->app);
        free(s->id);
        free(s);
    }
    vec_free(&a.sessions);
    sb_free(&line);
    return 0;
}
