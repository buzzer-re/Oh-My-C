/* acp.c - Agent Client Protocol for editor clients (Zed, ...): JSON-RPC 2.0 over NDJSON stdio.
 * Port of omp modes/acp/{acp-agent,acp-event-mapper,acp-client-bridge,acp-mode,terminal-auth}.ts and
 * commands/acp.ts. Every ACP session is its own app_t; prompts run on a per-session worker thread chain
 * while the stdin reader keeps dispatching (cancel, steering, client responses). */
#include "coding/slashcmd.h"
#include "modes.h"

#include "ai/ai_images.h"
#include "config/model_resolver.h"
#include "config/settings.h"
#include "core/fs.h"
#include "mcp/mcp.h"
#include "tools/approval.h"
#include "ui/ui.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACP_PROTOCOL_VERSION 1
#define ACP_TEXT_LIMIT 4000
#define SESSION_PAGE_SIZE 50
#define BOOTSTRAP_RACE_GUARD_MS 50
#define TERMINAL_TAG "--acp-terminal-auth"
#define OTHER_OPTION "Other (type your own)"
#define APPROVE_OPTION "Approve and execute"
#define REFINE_OPTION "Refine plan"
#define STEERING_METHOD "_session/steering"

typedef struct acp acp_t;
typedef struct asess asess_t;

/* one accepted session/prompt (PromptTurnState + its dispatch cancellation carrier) */
typedef struct turn {
    asess_t *s;
    json_t *req_id;
    json_t *prompt;          /* content blocks (owned) */
    bool settled;            /* the session/prompt response went out */
    bool cancel_requested;
    bool error_text;         /* an assistant error message already reached the client */
    int64_t usage[5];        /* input, output, cacheRead, cacheWrite, totalTokens of this turn's assistant messages */
    json_t *last;            /* the turn's latest assistant message (owned clone) */
    pthread_t prev;          /* the previous prompt's thread (joined before this one runs) */
    bool has_prev;
} turn_t;

struct asess {
    char *id;
    app_t *app;
    acp_t *acp;
    pthread_mutex_t mu;
    turn_t *turn;            /* the turn owning the event stream */
    VEC(turn_t *) turns;     /* accepted, unfinished turns (queued + active) */
    pthread_t th;            /* the latest prompt thread */
    bool has_th;
    json_t *decisions;       /* permission cache key -> "allow_always" | "reject_always" (#acpPermissionDecisions) */
    mcp_manager_t *mcp;      /* client-declared MCP servers (owned) */
    json_t *mcp_chunks;      /* MCP startup failures flushed with the bootstrap updates */
    char *live_id;           /* liveMessageId */
    bool live, text_emitted, thought_emitted;
    json_t *tool_args;       /* toolCallId -> args (toolArgsById) */
    json_t *held;            /* toolCallId -> tool_call notification waiting for the permission gate */
    char *last_error;        /* latest error notice of the running turn */
    char *cfg_sent;          /* configOptions the client last saw (config_option_update / a response) */
    tool_bridge_t bridge;
    bool closed;
};

typedef struct {
    int64_t id;
    json_t *result;
    char *error;             /* error message of an error response (owned) */
    bool done;
} apend_t;

struct acp {
    VEC(asess_t *) sessions;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    VEC(apend_t *) pending;  /* outgoing requests awaiting client responses */
    int64_t next_id;
    const app_opts_t *base;
    json_t *client_caps;     /* clientCapabilities of initialize (owned) */
    bool closed;             /* stdin closed: no response will ever arrive */
};

/* ---------- wire ---------- */

static pthread_mutex_t g_wire = PTHREAD_MUTEX_INITIALIZER;
static int g_proto_fd = 1; /* the JSON-RPC transport; fd 1 is rerouted to stderr (isolateProtocolStdout) */

/* JSON.stringify(message) + "\n" */
static void wire_send(json_t *o) {
    sb_t b = SB_INIT;
    json_dump_js_sb(&b, o);
    sb_putc(&b, '\n');
    pthread_mutex_lock(&g_wire);
    size_t off = 0;
    while (off < b.len) {
        ssize_t w = write(g_proto_fd, b.s + off, b.len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (size_t)w;
    }
    pthread_mutex_unlock(&g_wire);
    sb_free(&b);
    json_free(o);
}

static json_t *frame(void) {
    json_t *o = json_obj();
    json_set_str(o, "jsonrpc", "2.0");
    return o;
}

static void reply(const json_t *id, json_t *result) {
    json_t *o = frame();
    json_set(o, "id", json_clone(id));
    json_set(o, "result", result ? result : json_obj());
    wire_send(o);
}

static void reply_error(const json_t *id, int code, const char *message, json_t *data) {
    json_t *o = frame(), *e = json_obj();
    json_set(o, "id", json_clone(id));
    json_set_int(e, "code", code);
    json_set_str(e, "message", message);
    if (data) json_set(e, "data", data);
    json_set(o, "error", e);
    wire_send(o);
}

/* a thrown Error: RequestError.internalError({details}) */
static void reply_internal(const json_t *id, const char *details) {
    json_t *d = json_obj();
    json_set_str(d, "details", details ? details : "");
    reply_error(id, -32603, "Internal error", d);
}

static void reply_internalf(const json_t *id, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply_internalf(const json_t *id, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *m = NULL;
    if (vasprintf(&m, fmt, ap) < 0) m = NULL;
    va_end(ap);
    reply_internal(id, m);
    free(m);
}

/* RequestError.resourceNotFound(uri) */
static void reply_not_found(const json_t *id, const char *uri) {
    char *m = xasprintf("Resource not found: %s", uri ? uri : "");
    json_t *d = json_obj();
    json_set_str(d, "uri", uri ? uri : "");
    reply_error(id, -32002, m, d);
    free(m);
}

/* RequestError.invalidParams({method}, msg) */
static void reply_invalid(const json_t *id, const char *method, const char *msg) {
    char *m = xasprintf("Invalid params: %s", msg);
    json_t *d = json_obj();
    json_set_str(d, "method", method);
    reply_error(id, -32602, m, d);
    free(m);
}

static void notify_update(const char *sid, json_t *update) {
    json_t *o = frame(), *p = json_obj();
    json_set_str(o, "method", "session/update");
    json_set_str(p, "sessionId", sid);
    json_set(p, "update", update);
    json_set(o, "params", p);
    wire_send(o);
}

/* ---------- outgoing requests (agent -> client) ---------- */

static apend_t *client_send(acp_t *a, const char *method, json_t *params) {
    apend_t *p = xcalloc(1, sizeof(*p));
    pthread_mutex_lock(&a->mu);
    p->id = a->next_id++;
    vec_push(&a->pending, p);
    pthread_mutex_unlock(&a->mu);
    json_t *o = frame();
    json_set_int(o, "id", p->id);
    json_set_str(o, "method", method);
    json_set(o, "params", params);
    wire_send(o);
    return p;
}

/* wait until answered (1), the timeout passed (0) or the wait was cancelled / the connection closed (-1) */
static int client_wait(acp_t *a, apend_t *p, int64_t timeout_ms, volatile sig_atomic_t *cancel) {
    int64_t deadline = timeout_ms > 0 ? mono_ms() + timeout_ms : 0;
    pthread_mutex_lock(&a->mu);
    int rc;
    for (;;) {
        if (p->done) {
            rc = 1;
            break;
        }
        if (a->closed || (cancel && *cancel)) {
            rc = -1;
            break;
        }
        if (deadline && mono_ms() >= deadline) {
            rc = 0;
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&a->cv, &a->mu, &ts);
    }
    pthread_mutex_unlock(&a->mu);
    return rc;
}

/* drop a request: its result (owned) or NULL with *err (owned: the client's error, or why it never came) */
static json_t *client_take(acp_t *a, apend_t *p, char **err) {
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->pending.n; i++)
        if (a->pending.v[i] == p) {
            memmove(a->pending.v + i, a->pending.v + i + 1, (a->pending.n - i - 1) * sizeof(void *));
            a->pending.n--;
            break;
        }
    pthread_mutex_unlock(&a->mu);
    json_t *r = NULL;
    if (p->done && !p->error) r = p->result ? p->result : json_obj();
    else {
        json_free(p->result);
        if (err) *err = p->error ? p->error : xstrdup(p->done ? "request failed" : "Connection closed");
        else free(p->error);
        p->error = NULL;
    }
    if (err && r) *err = NULL;
    free(p->error);
    free(p);
    return r;
}

/* abandon a request the caller no longer waits for: tell the client ($/cancel_request) */
static void client_cancel(acp_t *a, apend_t *p) {
    if (!p->done && !a->closed) {
        json_t *o = frame(), *pr = json_obj();
        json_set_str(o, "method", "$/cancel_request");
        json_set_int(pr, "requestId", p->id);
        json_set(o, "params", pr);
        wire_send(o);
    }
    client_take(a, p, NULL);
}

/* blocking request; after_send runs once the frame is out (ordering of tool_call notifications) */
static json_t *client_request(acp_t *a, const char *method, json_t *params, volatile sig_atomic_t *cancel,
                              void (*after_send)(void *), void *ud, char **err) {
    apend_t *p = client_send(a, method, params);
    if (after_send) after_send(ud);
    int rc = client_wait(a, p, 0, cancel);
    if (rc != 1) {
        if (err) *err = xstrdup(a->closed ? "Connection closed" : "Request cancelled");
        client_cancel(a, p);
        return NULL;
    }
    return client_take(a, p, err);
}

static bool caps_bool(acp_t *a, const char *path) {
    json_t *v = json_path(a->client_caps, path);
    return json_is_bool(v) && v->u.b;
}

static bool caps_form(acp_t *a) {
    json_t *f = json_path(a->client_caps, "elicitation.form");
    return f && !json_is_null(f);
}

/* ---------- small helpers ---------- */

static bool is_url_subject(const char *s) { /* /^[a-z][a-z0-9+.-]*:\/\//i */
    if (!s || !isalpha((unsigned char)*s)) return false;
    const char *p = s + 1;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '.' || *p == '-') p++;
    return p[0] == ':' && p[1] == '/' && p[2] == '/';
}

/* limitText: at most ACP_TEXT_LIMIT UTF-16 units ("…" replaces the tail) */
static char *limit_text(const char *s) {
    size_t units = 0, cut = 0;
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (p[i]) {
        size_t n = p[i] < 0x80 ? 1 : p[i] < 0xE0 ? 2 : p[i] < 0xF0 ? 3 : 4;
        size_t u = n == 4 ? 2 : 1;
        if (units + u <= ACP_TEXT_LIMIT - 1) cut = i + n;
        units += u;
        for (size_t k = 0; k < n && p[i]; k++) i++;
    }
    if (units <= ACP_TEXT_LIMIT) return xstrdup(s);
    return xasprintf("%.*s\xe2\x80\xa6", (int)cut, s);
}

static char *trim_dup(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

/* normalizeText: trimmed and limited, NULL when empty */
static char *normalize_text(const char *s) {
    if (!s) return NULL;
    char *t = trim_dup(s);
    if (!*t) {
        free(t);
        return NULL;
    }
    char *r = limit_text(t);
    free(t);
    return r;
}

static const char *str_prop(const json_t *o, const char *key) { /* extractStringProperty: non-empty string */
    const char *v = json_is_obj(o) ? json_get_str(o, key) : NULL;
    return v && *v ? v : NULL;
}

static json_t *text_content(const char *text) { /* {type:"content", content:{type:"text", text}} */
    json_t *c = json_obj(), *in = json_obj();
    json_set_str(c, "type", "content");
    json_set_str(in, "type", "text");
    json_set_str(in, "text", text);
    json_set(c, "content", in);
    return c;
}

static bool has_text(const json_t *content, const char *text) {
    json_t *it;
    json_arr_foreach(content, i, it) {
        json_t *c = json_get(it, "content");
        if (str_eq(json_get_str(it, "type"), "content") && str_eq(json_get_str(c, "type"), "text") &&
            str_eq(json_get_str(c, "text"), text))
            return true;
    }
    return false;
}

/* ---------- event mapper (acp-event-mapper.ts) ---------- */

static bool is_command_tool(const char *t) { return str_eq(t, "bash") || str_eq(t, "shell") || str_eq(t, "exec"); }

/* mapToolKind */
static const char *tool_kind(const char *name, const json_t *args) {
    if (str_eq(name, "write") && str_starts(str_prop(args, "path") ? str_prop(args, "path") : "", "xd://")) return "execute";
    if (str_eq(name, "read")) return "read";
    if (str_eq(name, "write") || str_eq(name, "edit")) return "edit";
    if (str_eq(name, "delete")) return "delete";
    if (str_eq(name, "move")) return "move";
    if (is_command_tool(name) || str_eq(name, "eval")) return "execute";
    if (str_eq(name, "grep") || str_eq(name, "glob") || str_eq(name, "ast_grep")) return "search";
    if (str_eq(name, "web_search")) return "fetch";
    if (str_eq(name, "todo")) return "think";
    return "other";
}

static char *eval_start_text(const json_t *args) {
    if (!json_is_obj(args)) return NULL;
    json_t *cells = json_get_arr(args, "cells"), *one = NULL;
    if (!cells && json_get_str(args, "code")) {
        one = json_arr();
        json_push(one, json_clone(args));
        cells = one;
    }
    sb_t b = SB_INIT;
    json_t *c;
    json_arr_foreach(cells, i, c) {
        if (!json_is_obj(c)) continue;
        const char *code = str_prop(c, "code");
        if (!code) continue;
        const char *lang = str_prop(c, "language") ? str_prop(c, "language") : "?", *title = str_prop(c, "title");
        if (b.len) sb_putc(&b, '\n');
        if (title) sb_appendf(&b, "[%s] %s\n%s", lang, title, code);
        else sb_appendf(&b, "[%s]\n%s", lang, code);
    }
    json_free(one);
    if (!b.len) {
        sb_free(&b);
        return NULL;
    }
    char *r = limit_text(sb_cstr(&b));
    sb_free(&b);
    return r;
}

/* buildToolStartText */
static char *start_text(const char *name, const json_t *args) {
    if (is_command_tool(name)) {
        const char *cmd = str_prop(args, "command");
        if (!cmd) return NULL;
        char *t = xasprintf("$ %s", cmd), *r = limit_text(t);
        free(t);
        return r;
    }
    if (str_eq(name, "eval")) return eval_start_text(args);
    return NULL;
}

static json_t *start_content(const char *name, const json_t *args) {
    json_t *c = json_arr();
    char *t = start_text(name, args);
    if (t) json_push(c, text_content(t));
    free(t);
    return c;
}

/* buildToolTitle */
static char *tool_title(const char *name, const json_t *args, const char *intent) {
    if (is_command_tool(name) || str_eq(name, "eval")) {
        char *t = start_text(name, args);
        if (t) return t;
    }
    if (intent) {
        char *t = trim_dup(intent);
        if (*t) return t;
        free(t);
    }
    const char *keys[] = {"path", "command", "pattern", "query"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        const char *v = str_prop(args, keys[i]);
        if (v) return is_url_subject(v) ? xstrdup(v) : xasprintf("%s: %s", name, v);
    }
    return xstrdup(name ? name : "");
}

static bool regular_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* existingFileLocationPath */
static char *existing_file(const char *raw, const char *cwd) {
    if (!raw || is_url_subject(raw)) return NULL;
    char *abs = cwd ? path_abs(raw, cwd) : xstrdup(raw);
    if (regular_file(abs)) return abs;
    free(abs);
    return NULL;
}

static void push_location(json_t *locs, const char *path) {
    json_t *l;
    json_arr_foreach(locs, i, l) if (str_eq(json_get_str(l, "path"), path)) return;
    l = json_obj();
    json_set_str(l, "path", path);
    json_push(locs, l);
}

static void push_raw_location(json_t *locs, const char *raw, const char *cwd) {
    if (!raw || !*raw || is_url_subject(raw)) return;
    char *p = cwd ? path_abs(raw, cwd) : xstrdup(raw);
    push_location(locs, p);
    free(p);
}

/* extractToolLocations: read locations only name an existing regular file (a `:selector` suffix is split off
 * unless the literal name exists) */
static json_t *tool_locations(const json_t *args, const char *cwd, const char *tool) {
    json_t *locs = json_arr();
    const char *path = str_prop(args, "path");
    if (path && str_eq(tool, "read")) {
        char *f = NULL;
        if (cwd && !is_url_subject(path)) {
            f = existing_file(path, cwd);
            const char *colon = strrchr(path, ':');
            if (!f && colon && colon != path) {
                char *base = xstrndup(path, (size_t)(colon - path));
                f = existing_file(base, cwd);
                free(base);
            }
        }
        if (f) push_location(locs, f);
        free(f);
    } else push_raw_location(locs, path, cwd);
    push_raw_location(locs, str_prop(args, "oldPath"), cwd);
    push_raw_location(locs, str_prop(args, "newPath"), cwd);
    return locs;
}

/* extractToolLocationsFromResult */
static json_t *result_locations(const json_t *result, const char *cwd) {
    json_t *details = json_get_obj(result, "details");
    if (!details) return json_arr();
    json_t *locs = tool_locations(details, cwd, NULL);
    char *rf = existing_file(str_prop(details, "resolvedPath"), cwd);
    if (rf) push_location(locs, rf);
    free(rf);
    json_t *e;
    json_arr_foreach(json_get_arr(details, "perFileResults"), i, e) push_raw_location(locs, str_prop(e, "path"), cwd);
    return locs;
}

/* extractDiffToolCallContent */
static json_t *diff_content(const json_t *result) {
    json_t *out = json_arr(), *details = json_get_obj(result, "details");
    if (!details) return out;
    json_t *per = json_get_arr(details, "perFileResults"), *one = NULL;
    if (!per) {
        one = json_arr();
        json_push(one, json_clone(details));
        per = one;
    }
    json_t *e;
    json_arr_foreach(per, i, e) {
        if (!json_is_obj(e) || json_get_bool(e, "isError", false)) continue;
        const char *path = str_prop(e, "path");
        const char *ot = json_get_str(e, "oldText"), *nt = json_get_str(e, "newText");
        if (!path || (!ot && !nt)) continue;
        json_t *d = json_obj();
        json_set_str(d, "type", "diff");
        json_set_str(d, "path", path);
        json_set(d, "oldText", ot ? json_str(ot) : json_null());
        json_set_str(d, "newText", nt ? nt : "");
        json_push(out, d);
    }
    json_free(one);
    return out;
}

static json_t *binary_content(const char *type, const json_t *v) {
    const char *data = str_prop(v, "data"), *mime = str_prop(v, "mimeType");
    if (!data || !mime) return NULL;
    json_t *c = json_obj(), *in = json_obj();
    json_set_str(c, "type", "content");
    json_set_str(in, "type", type);
    json_set_str(in, "data", data);
    json_set_str(in, "mimeType", mime);
    json_set(c, "content", in);
    return c;
}

/* toToolCallContent */
static json_t *block_content(const json_t *v) {
    const char *type = json_is_obj(v) ? json_get_str(v, "type") : NULL;
    if (str_eq(type, "text")) {
        const char *t = str_prop(v, "text");
        if (!t) return NULL;
        char *l = limit_text(t);
        json_t *c = text_content(l);
        free(l);
        return c;
    }
    if (str_eq(type, "image") || str_eq(type, "audio")) return binary_content(type, v);
    if (str_eq(type, "resource_link")) {
        const char *uri = str_prop(v, "uri"), *name = str_prop(v, "name");
        if (!uri || !name) return NULL;
        json_t *in = json_obj();
        json_set_str(in, "type", "resource_link");
        json_set_str(in, "uri", uri);
        json_set_str(in, "name", name);
        static const char *const opt[] = {"title", "description", "mimeType"};
        for (size_t i = 0; i < 3; i++)
            if (str_prop(v, opt[i])) json_set_str(in, opt[i], str_prop(v, opt[i]));
        json_t *sz = json_get(v, "size");
        if (json_is_num(sz)) json_set(in, "size", json_clone(sz));
        json_t *c = json_obj();
        json_set_str(c, "type", "content");
        json_set(c, "content", in);
        return c;
    }
    if (str_eq(type, "resource")) {
        json_t *r = json_get_obj(v, "resource");
        const char *uri = str_prop(r, "uri");
        if (!uri) return NULL;
        json_t *res = json_obj();
        json_set_str(res, "uri", uri);
        const char *text = str_prop(r, "text"), *blob = str_prop(r, "blob");
        if (text) json_set_str(res, "text", text);
        else if (blob) json_set_str(res, "blob", blob);
        else {
            json_free(res);
            return NULL;
        }
        if (str_prop(r, "mimeType")) json_set_str(res, "mimeType", str_prop(r, "mimeType"));
        json_t *in = json_obj(), *c = json_obj();
        json_set_str(in, "type", "resource");
        json_set(in, "resource", res);
        json_set_str(c, "type", "content");
        json_set(c, "content", in);
        return c;
    }
    return NULL;
}

static const char *terminal_id(const json_t *v) {
    const char *t = str_prop(v, "terminalId");
    return t ? t : str_prop(json_get_obj(v, "details"), "terminalId");
}

/* extractReadableText */
static char *readable_text(const json_t *v) {
    if (json_is_str(v)) return normalize_text(json_sv(v));
    if (!json_is_obj(v)) return NULL;
    const char *direct = str_prop(v, "text");
    if (!direct) direct = str_prop(v, "errorMessage");
    if (!direct) direct = str_prop(v, "message");
    if (direct) return normalize_text(direct);
    json_t *blocks = json_get_arr(v, "content");
    if (blocks) {
        sb_t b = SB_INIT;
        json_t *x;
        json_arr_foreach(blocks, i, x) {
            const char *t = str_prop(x, "text");
            if (!t) continue;
            char *l = limit_text(t);
            if (b.len) sb_putc(&b, '\n');
            sb_append(&b, l);
            free(l);
        }
        char *r = b.len ? normalize_text(sb_cstr(&b)) : NULL;
        sb_free(&b);
        return r;
    }
    json_t *imgs = json_get_arr(json_get_obj(v, "details"), "images");
    if (json_len(imgs)) return NULL;
    if (terminal_id(v)) return NULL;
    sb_t b = SB_INIT;
    json_dump_js_sb(&b, v);
    char *r = normalize_text(sb_cstr(&b));
    sb_free(&b);
    return r;
}

/* extractToolCallContent */
static json_t *result_content(const json_t *v) {
    json_t *out = json_arr(), *x;
    json_arr_foreach(json_get_arr(v, "content"), i, x) {
        json_t *c = block_content(x);
        if (c) json_push(out, c);
    }
    /* details.images not already present */
    json_arr_foreach(json_get_arr(json_get_obj(v, "details"), "images"), i, x) {
        json_t *c = binary_content("image", x);
        if (!c) continue;
        bool dup = false;
        json_t *y;
        json_arr_foreach(out, k, y) {
            json_t *a = json_get(y, "content"), *b = json_get(c, "content");
            if (str_eq(json_get_str(a, "type"), "image") && str_eq(json_get_str(a, "data"), json_get_str(b, "data")) &&
                str_eq(json_get_str(a, "mimeType"), json_get_str(b, "mimeType")))
                dup = true;
        }
        if (dup) json_free(c);
        else json_push(out, c);
    }
    const char *tid = terminal_id(v);
    if (tid) {
        bool have = false;
        json_arr_foreach(out, i, x) have |= str_eq(json_get_str(x, "type"), "terminal") && str_eq(json_get_str(x, "terminalId"), tid);
        if (!have) {
            json_t *t = json_obj();
            json_set_str(t, "type", "terminal");
            json_set_str(t, "terminalId", tid);
            json_push(out, t);
        }
    }
    char *fb = readable_text(v);
    if (fb && !has_text(out, fb)) json_push(out, text_content(fb));
    free(fb);
    return out;
}

/* mergeToolUpdateContent: start content first; result text equal to text already present is dropped */
static json_t *merge_content(json_t *start, json_t *result) {
    if (!json_len(start)) {
        json_free(start);
        return result;
    }
    json_t *it;
    while (json_len(result)) {
        it = json_arr_remove(result, 0);
        json_t *c = json_get(it, "content");
        if (str_eq(json_get_str(it, "type"), "content") && str_eq(json_get_str(c, "type"), "text") &&
            has_text(start, json_get_str(c, "text")))
            json_free(it);
        else json_push(start, it);
    }
    json_free(result);
    return start;
}

static const char *todo_status(const char *st) {
    if (str_eq(st, "in_progress")) return "in_progress";
    if (str_eq(st, "completed") || str_eq(st, "abandoned")) return "completed";
    return "pending";
}

static json_t *plan_entry(const char *content, const char *status) {
    json_t *e = json_obj();
    json_set_str(e, "content", content);
    json_set_str(e, "priority", "medium");
    json_set_str(e, "status", todo_status(status));
    return e;
}

/* tool_execution_start -> tool_call (buildToolCallStartUpdate) */
static json_t *tool_call_update(const char *id, const char *name, const json_t *args, const char *intent, const char *cwd) {
    json_t *u = json_obj();
    json_set_str(u, "sessionUpdate", "tool_call");
    json_set_str(u, "toolCallId", id ? id : "");
    json_set_str(u, "name", name ? name : "");
    char *t = tool_title(name, args, intent);
    json_set_str(u, "title", t);
    free(t);
    json_set_str(u, "kind", tool_kind(name, args));
    json_set_str(u, "status", "pending");
    if (args) json_set(u, "rawInput", json_clone(args));
    json_t *c = start_content(name, args);
    if (json_len(c)) json_set(u, "content", c);
    else json_free(c);
    json_t *l = tool_locations(args, cwd, name);
    if (json_len(l)) json_set(u, "locations", l);
    else json_free(l);
    return u;
}

static json_t *raw_output(const tool_result_t *r) {
    json_t *o = json_obj();
    json_set(o, "content", r && r->content ? json_clone(r->content) : json_arr());
    if (r && r->details) json_set(o, "details", json_clone(r->details));
    return o;
}

/* tool_execution_end -> tool_call_update (+ plan for todo) */
static void tool_end_updates(const char *sid, const char *id, const char *name, const json_t *args, json_t *result,
                             bool is_error, const char *cwd) {
    json_t *res = diff_content(result), *rc = result_content(result);
    while (json_len(rc)) json_push(res, json_arr_remove(rc, 0));
    json_free(rc);
    json_t *content = merge_content(start_content(name, args), res);
    json_t *u = json_obj();
    json_set_str(u, "sessionUpdate", "tool_call_update");
    json_set_str(u, "toolCallId", id ? id : "");
    json_set_str(u, "status", is_error ? "failed" : "completed");
    json_set(u, "rawOutput", json_clone(result));
    if (json_len(content)) json_set(u, "content", content);
    else json_free(content);
    json_t *l = result_locations(result, cwd);
    if (json_len(l)) json_set(u, "locations", l);
    else json_free(l);
    notify_update(sid, u);
    json_t *phases = json_get_arr(json_get_obj(result, "details"), "phases");
    if (str_eq(name, "todo") && !is_error && phases) {
        json_t *pl = json_obj(), *entries = json_arr(), *ph, *t;
        json_set_str(pl, "sessionUpdate", "plan");
        json_arr_foreach(phases, i, ph) json_arr_foreach(json_get_arr(ph, "tasks"), k, t) {
            const char *c = str_prop(t, "content");
            if (c) json_push(entries, plan_entry(c, json_get_str(t, "status")));
        }
        json_set(pl, "entries", entries);
        notify_update(sid, pl);
    }
}

/* extractAssistantMessageText */
static char *assistant_text(const json_t *msg) {
    sb_t b = SB_INIT;
    json_t *x;
    json_arr_foreach(json_get_arr(msg, "content"), i, x) {
        const char *t = str_prop(x, "text");
        if (!t) continue;
        char *l = limit_text(t);
        if (b.len) sb_putc(&b, '\n');
        sb_append(&b, l);
        free(l);
    }
    return sb_detach(&b);
}

static json_t *chunk(const char *kind, const char *text, const char *message_id) {
    json_t *u = json_obj(), *c = json_obj();
    json_set_str(u, "sessionUpdate", kind);
    json_set_str(c, "type", "text");
    json_set_str(c, "text", text);
    json_set(u, "content", c);
    if (message_id) json_set_str(u, "messageId", message_id);
    return u;
}

/* ---------- sessions: config options, modes, info ---------- */

static json_t *available_modes(void) {
    json_t *modes = json_parse_str("[{\"id\":\"default\",\"name\":\"Default\",\"description\":\"Standard ACP headless mode\"}]");
    if (settings_bool("plan.enabled", true))
        json_push(modes, json_parse_str("{\"id\":\"plan\",\"name\":\"Plan\",\"description\":\"Read-only planning mode that drafts a "
                                        "plan to a markdown file before any code changes\"}"));
    return modes;
}

static const char *mode_id(asess_t *s) { return s->app->plan_mode ? "plan" : "default"; }

static json_t *mode_state(asess_t *s) {
    json_t *m = json_obj();
    json_set(m, "availableModes", available_modes());
    json_set_str(m, "currentModeId", mode_id(s));
    return m;
}

/* getAvailableModels: chat models of available providers, narrowed by enabledModels */
static void available_models(mr_pool_t *out) {
    mr_pool_t all = {0};
    mr_pool_available(&all, "chat");
    json_t *pats = settings_get("enabledModels");
    if (!json_len(pats)) {
        *out = all;
        return;
    }
    for (size_t i = 0; i < all.n; i++) {
        json_t *p;
        bool keep = false;
        json_arr_foreach(pats, k, p) if (json_is_str(p) && mr_resolve_string(json_sv(p), &all) == all.v[i]) keep = true;
        if (keep) mr_pool_push(out, all.v[i]);
    }
    mr_pool_free(&all);
}

static char *model_id(const ai_model_t *m) { return xasprintf("%s/%s", m->provider, m->id); }

static json_t *config_options(asess_t *s) {
    app_t *a = s->app;
    json_t *opts = json_arr(), *o = json_obj(), *mo = json_arr(), *m;
    json_set_str(o, "id", "mode");
    json_set_str(o, "name", "Mode");
    json_set_str(o, "category", "mode");
    json_set_str(o, "type", "select");
    json_set_str(o, "currentValue", mode_id(s));
    json_t *modes = available_modes();
    json_arr_foreach(modes, i, m) {
        json_t *x = json_obj();
        json_set_str(x, "value", json_get_str(m, "id"));
        json_set_str(x, "name", json_get_str(m, "name"));
        json_set_str(x, "description", json_get_str(m, "description"));
        json_push(mo, x);
    }
    json_free(modes);
    json_set(o, "options", mo);
    json_push(opts, o);
    mr_pool_t pool = {0};
    available_models(&pool);
    if (pool.n) {
        o = json_obj();
        json_set_str(o, "id", "model");
        json_set_str(o, "name", "Model");
        json_set_str(o, "category", "model");
        json_set_str(o, "type", "select");
        char *cur = model_id(a->model ? a->model : pool.v[0]);
        json_set_str(o, "currentValue", cur);
        free(cur);
        json_t *list = json_arr();
        for (size_t i = 0; i < pool.n; i++) {
            json_t *x = json_obj();
            char *v = model_id(pool.v[i]);
            json_set_str(x, "value", v);
            json_set_str(x, "name", pool.v[i]->name ? pool.v[i]->name : pool.v[i]->id);
            json_set_str(x, "description", v);
            free(v);
            json_push(list, x);
        }
        json_set(o, "options", list);
        json_push(opts, o);
    }
    mr_pool_free(&pool);
    o = json_obj();
    json_set_str(o, "id", "thinking");
    json_set_str(o, "name", "Thinking");
    json_set_str(o, "category", "thought_level");
    json_set_str(o, "type", "select");
    const char *cur = "off";
    if (a->model && a->model->reasoning) cur = a->auto_thinking ? "auto" : effort_name(a->thinking);
    json_set_str(o, "currentValue", cur && *cur && !str_eq(cur, "inherit") ? cur : "off");
    json_t *tl = json_parse_str("[{\"value\":\"off\",\"name\":\"Off\"},{\"value\":\"auto\",\"name\":\"Auto\",\"description\":"
                                "\"Auto-detect per prompt\"}]");
    for (int e = EFFORT_MINIMAL; a->model && a->model->reasoning && e <= EFFORT_MAX; e++) {
        if (!model_supports_effort(a->model, (ai_effort_t)e)) continue;
        json_t *x = json_obj();
        json_set_str(x, "value", effort_name((ai_effort_t)e));
        json_set_str(x, "name", effort_name((ai_effort_t)e));
        json_push(tl, x);
    }
    json_set(o, "options", tl);
    json_push(opts, o);
    return opts;
}

static void push_config_update(asess_t *s) {
    json_t *u = json_obj(), *opts = config_options(s);
    json_set_str(u, "sessionUpdate", "config_option_update");
    char *d = json_dump(opts, 0);
    pthread_mutex_lock(&s->mu);
    free(s->cfg_sent);
    s->cfg_sent = d;
    pthread_mutex_unlock(&s->mu);
    json_set(u, "configOptions", opts);
    notify_update(s->id, u);
}

static char *config_snapshot(asess_t *s) {
    json_t *c = config_options(s);
    char *d = json_dump(c, 0);
    json_free(c);
    return d;
}

/* a model / thinking / mode change the client was not told about yet (AgentSession lifetime events) */
static void push_config_if_changed(asess_t *s, const char *before) {
    char *cur = config_snapshot(s);
    pthread_mutex_lock(&s->mu);
    bool told = str_eq(cur, s->cfg_sent);
    pthread_mutex_unlock(&s->mu);
    if (!str_eq(cur, before) && !told) push_config_update(s);
    free(cur);
}

static void push_mode_update(asess_t *s) {
    json_t *u = json_obj();
    json_set_str(u, "sessionUpdate", "current_mode_update");
    json_set_str(u, "currentModeId", mode_id(s));
    notify_update(s->id, u);
}


static void push_session_info(asess_t *s, const char *updated_at) {
    json_t *u = json_obj();
    json_set_str(u, "sessionUpdate", "session_info_update");
    if (s->app->session && s->app->session->title && *s->app->session->title) json_set_str(u, "title", s->app->session->title);
    char *now = updated_at ? NULL : iso8601_now();
    if (updated_at || now) json_set_str(u, "updatedAt", updated_at ? updated_at : now);
    free(now);
    notify_update(s->id, u);
}

static void push_available_commands(asess_t *s) {
    json_t *u = json_obj();
    json_set_str(u, "sessionUpdate", "available_commands_update");
    json_set(u, "availableCommands", slash_acp_available_commands(s->app));
    notify_update(s->id, u);
}

/* #emitBootstrapUpdates: MCP startup failures, commands, session info (behind the bootstrap race guard) */
static void bootstrap_updates(asess_t *s) {
    usleep(BOOTSTRAP_RACE_GUARD_MS * 1000);
    json_t *c;
    json_arr_foreach(s->mcp_chunks, i, c) {
        char *t = xasprintf("MCP server \"%s\" failed to connect: %s. Its tools are unavailable until the client restarts the "
                            "session.",
                            json_get_str(c, "name"), json_get_str(c, "error"));
        json_t *u = json_obj(), *cc = json_obj();
        json_set_str(u, "sessionUpdate", "agent_message_chunk");
        json_set_str(cc, "type", "text");
        json_set_str(cc, "text", t);
        json_set(u, "content", cc);
        notify_update(s->id, u);
        free(t);
    }
    json_free(s->mcp_chunks);
    s->mcp_chunks = json_arr();
    push_available_commands(s);
    push_session_info(s, s->app->session ? json_get_str(s->app->session->header, "timestamp") : NULL);
}

/* ---------- live event forwarding ---------- */

static void new_live(asess_t *s) {
    free(s->live_id);
    s->live_id = uuid_v4();
    s->live = true;
    s->text_emitted = s->thought_emitted = false;
}

static void ensure_live(asess_t *s) {
    if (!s->live || !s->live_id) new_live(s);
}

static void clear_live(asess_t *s) {
    free(s->live_id);
    s->live_id = NULL;
    s->live = false;
}

static void flush_held_locked(asess_t *s, const char *id) {
    json_t *h = id ? json_take(s->held, id) : NULL;
    if (h) notify_update(s->id, h);
}

static void flush_held(asess_t *s, const char *id) {
    pthread_mutex_lock(&s->mu);
    flush_held_locked(s, id);
    pthread_mutex_unlock(&s->mu);
}

static bool all_space(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

static void on_message_update(asess_t *s, const agent_event_t *ev) {
    const ai_event_t *ai = ev->ai;
    ensure_live(s);
    if (ai->type == AI_EV_TEXT_DELTA) {
        if (!ai->delta_len) return;
        char *t = xstrndup(ai->delta, ai->delta_len);
        s->text_emitted = true;
        notify_update(s->id, chunk("agent_message_chunk", t, s->live_id));
        free(t);
    } else if (ai->type == AI_EV_THINKING_DELTA) {
        json_t *blk = json_at(json_get_arr(ai->partial, "content"), (size_t)MAX(0, ai->content_index));
        if (str_eq(json_get_str(blk, "type"), "thinking") && all_space(json_get_str(blk, "thinking"))) return;
        if (!ai->delta_len) return;
        char *t = xstrndup(ai->delta, ai->delta_len);
        s->thought_emitted = true;
        notify_update(s->id, chunk("agent_thought_chunk", t, s->live_id));
        free(t);
    } else if (ai->type == AI_EV_THINKING_END) {
        /* narration streams as thinking; once its block closes as narration it is the reply */
        if (!json_get_bool(ai->block, "narration", false)) return;
        char *t = trim_dup(json_get_str(ai->block, "thinking") ? json_get_str(ai->block, "thinking") : "");
        if (*t) {
            s->text_emitted = true;
            notify_update(s->id, chunk("agent_message_chunk", t, s->live_id));
        }
        free(t);
    } else if (ai->type == AI_EV_DONE) {
        if (s->text_emitted) return;
        char *t = assistant_text(ai->partial);
        if (*t) {
            s->text_emitted = true;
            notify_update(s->id, chunk("agent_message_chunk", t, s->live_id));
        }
        free(t);
    }
}

static void acp_listener(void *ud, const agent_event_t *ev) {
    asess_t *s = ud;
    pthread_mutex_lock(&s->mu);
    turn_t *t = s->turn;
    bool assistant = ev->message && str_eq(json_get_str(ev->message, "role"), "assistant");
    if (t && ev->type == AGENT_EV_MESSAGE_END && assistant) {
        json_t *u = json_get(ev->message, "usage");
        static const char *const keys[] = {"input", "output", "cacheRead", "cacheWrite", "totalTokens"};
        for (int i = 0; i < 5; i++) t->usage[i] += json_get_int(u, keys[i], 0);
        json_free(t->last);
        t->last = json_clone(ev->message);
    }
    if (!t || t->settled || t->cancel_requested) {
        pthread_mutex_unlock(&s->mu);
        return;
    }
    const char *cwd = s->app->cwd;
    switch (ev->type) {
    case AGENT_EV_MESSAGE_START:
        if (assistant) new_live(s);
        break;
    case AGENT_EV_MESSAGE_UPDATE:
        if (assistant && ev->ai) on_message_update(s, ev);
        break;
    case AGENT_EV_MESSAGE_END:
        if (!assistant) break;
        ensure_live(s);
        if (!s->text_emitted) {
            char *txt = assistant_text(ev->message);
            if (*txt) {
                s->text_emitted = true;
                notify_update(s->id, chunk("agent_message_chunk", txt, s->live_id));
            }
            free(txt);
        }
        clear_live(s);
        break;
    case AGENT_EV_TOOL_START: {
        if (ev->tool_call_id) json_set(s->tool_args, ev->tool_call_id, ev->args ? json_clone(ev->args) : json_obj());
        json_t *u = tool_call_update(ev->tool_call_id, ev->tool_name, ev->args, ev->intent, cwd);
        /* held until the permission gate ran: a permission request precedes its tool card (#wrapToolForAcpPermission) */
        if (ev->tool_call_id) json_set(s->held, ev->tool_call_id, u);
        else notify_update(s->id, u);
        break;
    }
    case AGENT_EV_TOOL_UPDATE: {
        flush_held_locked(s, ev->tool_call_id);
        if (ev->tool_call_id && ev->args) json_set(s->tool_args, ev->tool_call_id, json_clone(ev->args));
        json_t *partial = raw_output(ev->result);
        json_t *content = merge_content(start_content(ev->tool_name, ev->args), result_content(partial));
        json_t *u = json_obj();
        json_set_str(u, "sessionUpdate", "tool_call_update");
        json_set_str(u, "toolCallId", ev->tool_call_id ? ev->tool_call_id : "");
        /* toolUpdateStatus: a settled background job frame settles the call */
        const char *st = json_get_str(json_get_obj(json_get_obj(partial, "details"), "async"), "state");
        json_set_str(u, "status", str_eq(st, "completed") ? "completed" : str_eq(st, "failed") || str_eq(st, "cancelled") ? "failed" : "in_progress");
        json_set(u, "rawOutput", partial);
        if (json_len(content)) json_set(u, "content", content);
        else json_free(content);
        json_t *l = tool_locations(ev->args, cwd, ev->tool_name);
        if (json_len(l)) json_set(u, "locations", l);
        else json_free(l);
        notify_update(s->id, u);
        break;
    }
    case AGENT_EV_TOOL_END: {
        flush_held_locked(s, ev->tool_call_id);
        json_t *args = ev->tool_call_id ? json_take(s->tool_args, ev->tool_call_id) : NULL;
        json_t *result = raw_output(ev->result);
        tool_end_updates(s->id, ev->tool_call_id, ev->tool_name, args ? args : ev->args, result, ev->is_error, cwd);
        json_free(result);
        json_free(args);
        break;
    }
    default:
        break;
    }
    pthread_mutex_unlock(&s->mu);
}

/* app notices: todo reminders become plan updates; error notices explain a failed prompt */
static void acp_notice(void *ud, const char *level, const char *text) {
    asess_t *s = ud;
    if (str_eq(level, "todo-reminder")) {
        json_t *j = json_parse(text, strlen(text), NULL), *td;
        pthread_mutex_lock(&s->mu);
        if (s->turn && !s->turn->settled && json_is_obj(j)) {
            json_t *pl = json_obj(), *entries = json_arr();
            json_set_str(pl, "sessionUpdate", "plan");
            json_arr_foreach(json_get_arr(j, "todos"), i, td)
                json_push(entries, plan_entry(json_get_str(td, "content") ? json_get_str(td, "content") : "", json_get_str(td, "status")));
            json_set(pl, "entries", entries);
            notify_update(s->id, pl);
        }
        pthread_mutex_unlock(&s->mu);
        json_free(j);
    } else if (str_eq(level, "error")) {
        pthread_mutex_lock(&s->mu);
        free(s->last_error);
        s->last_error = xstrdup(text);
        pthread_mutex_unlock(&s->mu);
    }
}

/* ---------- permission gate (#wrapToolForAcpPermission, acp-permission-gate.ts) ---------- */

static const char *const PERMISSION_OPTIONS =
    "[{\"optionId\":\"allow_once\",\"name\":\"Allow once\",\"kind\":\"allow_once\"},"
    "{\"optionId\":\"allow_always\",\"name\":\"Always allow\",\"kind\":\"allow_always\"},"
    "{\"optionId\":\"reject_once\",\"name\":\"Reject\",\"kind\":\"reject_once\"},"
    "{\"optionId\":\"reject_always\",\"name\":\"Always reject\",\"kind\":\"reject_always\"}]";

/* #isExplicitAutoApproveMode: --auto-approve, or a configured (not defaulted) tools.approvalMode of yolo */
static bool explicit_auto_approve(app_t *app) {
    return app->opts.auto_approve ||
           (settings_is_configured("tools.approvalMode") && str_eq(settings_str("tools.approvalMode", "yolo"), "yolo"));
}

/* extractPermissionLocations: absolute editor locations of the explicit paths, else the call's path arguments */
static json_t *permission_locations(app_t *app, const json_t *args, const strvec_t *paths) {
    json_t *out = json_arr();
    strvec_t cand = {0};
    if (paths->n) {
        for (size_t i = 0; i < paths->n; i++) strvec_push(&cand, paths->v[i]);
    } else if (json_is_obj(args)) {
        const char *keys1[] = {"path", "file"};
        for (size_t i = 0; i < 2; i++)
            if (json_get_str(args, keys1[i])) strvec_push(&cand, json_get_str(args, keys1[i]));
        json_t *e;
        json_arr_foreach(json_get_arr(args, "paths"), i, e) if (json_is_str(e)) strvec_push(&cand, json_sv(e));
        const char *keys2[] = {"oldPath", "newPath", "from", "to", "source", "destination"};
        for (size_t i = 0; i < ARRAY_LEN(keys2); i++)
            if (json_get_str(args, keys2[i])) strvec_push(&cand, json_get_str(args, keys2[i]));
    }
    for (size_t i = 0; i < cand.n; i++) {
        if (!*cand.v[i] || strstr(cand.v[i], "://")) continue;
        char *abs = path_abs(cand.v[i], app->cwd);
        push_location(out, abs);
        free(abs);
    }
    strvec_free(&cand);
    return out;
}

typedef struct {
    asess_t *s;
    const char *id;
} held_ref_t;

static void flush_held_cb(void *ud) {
    held_ref_t *h = ud;
    flush_held(h->s, h->id);
}

static tool_gate_t gate_decide(asess_t *s, app_t *app, const json_t *call, const json_t *args, bool *client_approved,
                               char **reason, held_ref_t *href, bool *flushed) {
    acp_t *a = s->acp;
    const char *name = json_get_str(call, "name");
    if (!approval_acp_tool_gated(name)) return TOOL_ALLOW;
    /* default-config sessions still ask for executable calls even though the schema defaults to yolo; explicit
     * yolo is an intentional opt-in (a non-allow tools.approval.<tool> keeps the gate) */
    if (!str_eq(name, "bash") && explicit_auto_approve(app)) {
        json_t *pol = json_get(settings_get("tools.approval"), name);
        if (!json_truthy(pol) || (json_is_str(pol) && !strcmp(json_sv(pol), "allow"))) return TOOL_ALLOW;
    }
    approval_call_t c = {app->env, app->agent, name, args};
    char *restricted = approval_restricted_reason(&c, settings_get("tools.restrictedFolders"), app->cwd);
    bool cacheable = restricted == NULL; /* a restricted call is approved per call */
    free(restricted);
    if (str_eq(name, "bash")) {
        approval_ctx_t ctx;
        approval_ctx_from_settings(&ctx, app->opts.auto_approve, app->session ? session_id(app->session) : NULL, app->cwd);
        if (ctx.mode == AMODE_YOLO && !explicit_auto_approve(app)) ctx.mode = AMODE_ALWAYS_ASK;
        ctx.use_grants = ctx.mode == AMODE_ALWAYS_ASK_CACHED;
        approval_resolved_t r;
        approval_resolve(&c, &ctx, &r);
        if (r.policy == APOL_DENY) {
            *reason = approval_deny_error(&r, name);
            approval_resolved_free(&r);
            return TOOL_BLOCK;
        }
        bool allow = r.policy == APOL_ALLOW;
        cacheable = !r.override; /* a critical command or explicit pattern stays per-call */
        approval_resolved_free(&r);
        if (allow) return TOOL_ALLOW;
    }
    char *title = NULL, *cache_key = NULL;
    strvec_t paths = {0};
    if (!approval_acp_intent(app->env, name, args, &title, &cache_key, &paths)) {
        strvec_free(&paths);
        return TOOL_ALLOW;
    }
    tool_gate_t g = TOOL_ALLOW;
    pthread_mutex_lock(&s->mu);
    const char *persisted = cacheable ? json_get_str(s->decisions, cache_key) : NULL;
    bool allow_always = str_eq(persisted, "allow_always"), reject_always = str_eq(persisted, "reject_always");
    pthread_mutex_unlock(&s->mu);
    if (allow_always) {
        *client_approved = true;
        goto out;
    }
    if (reject_always) {
        *reason = xstrdup("Tool call rejected by user (preference)");
        g = TOOL_BLOCK;
        goto out;
    }
    if (app->agent->cancel) {
        *reason = xstrdup("Permission request cancelled");
        g = TOOL_BLOCK;
        goto out;
    }
    json_t *p = json_obj();
    json_set_str(p, "sessionId", s->id);
    json_t *tc = json_obj();
    json_set_str(tc, "toolCallId", json_get_str(call, "id"));
    json_set_str(tc, "title", title);
    if (str_eq(name, "bash")) json_set_str(tc, "kind", "execute");
    json_set_str(tc, "status", "pending");
    json_set(tc, "rawInput", args ? json_clone(args) : json_obj());
    const char *cmd = str_eq(name, "bash") ? json_get_str(args, "command") : NULL;
    if (cmd && *cmd) {
        char *t = xasprintf("$ %s", cmd);
        json_t *content = json_arr();
        json_push(content, text_content(t));
        json_set(tc, "content", content);
        free(t);
    }
    json_set(tc, "locations", permission_locations(app, args, &paths));
    json_set(p, "toolCall", tc);
    json_set(p, "options", json_parse_str(PERMISSION_OPTIONS));
    *flushed = true;
    json_t *r = client_request(a, "session/request_permission", p, &app->agent->cancel, flush_held_cb, href, NULL);
    json_t *outcome = json_get(r, "outcome");
    const char *oc = json_get_str(outcome, "outcome");
    const char *opt = json_get_str(outcome, "optionId");
    if (!r || !str_eq(oc, "selected") || app->agent->cancel) {
        *reason = xstrdup("Permission request cancelled");
        g = TOOL_BLOCK;
    } else if (!str_eq(opt, "allow_once") && !str_eq(opt, "allow_always") && !str_eq(opt, "reject_once") &&
               !str_eq(opt, "reject_always")) {
        *reason = xasprintf("Tool permission response used unknown option ID: %s", opt ? opt : "undefined");
        g = TOOL_BLOCK;
    } else {
        if (cacheable && (str_eq(opt, "allow_always") || str_eq(opt, "reject_always"))) {
            pthread_mutex_lock(&s->mu);
            json_set_str(s->decisions, cache_key, opt);
            pthread_mutex_unlock(&s->mu);
        }
        if (str_starts(opt, "reject")) {
            *reason = xasprintf("Tool call rejected by user (%s)", name);
            g = TOOL_BLOCK;
        } else {
            *client_approved = true;
        }
    }
    json_free(r);
out:
    free(title);
    free(cache_key);
    strvec_free(&paths);
    return g;
}

static tool_gate_t acp_permission_gate(void *ud, app_t *app, const json_t *call, const json_t *args, bool *client_approved,
                                       char **reason) {
    asess_t *s = ud;
    held_ref_t href = {s, json_get_str(call, "id")};
    bool flushed = false;
    tool_gate_t g = gate_decide(s, app, call, args, client_approved, reason, &href, &flushed);
    /* the tool card goes out now, except for `ask`, whose elicitation leads (it is flushed once that is sent) */
    if (!flushed && (g == TOOL_BLOCK || !str_eq(json_get_str(call, "name"), "ask"))) flush_held(s, href.id);
    return g;
}

/* ---------- client bridges (acp-client-bridge.ts) ---------- */

static char *bridge_read(tool_bridge_t *b, const char *abs, char **err) {
    asess_t *s = b->ud;
    json_t *p = json_obj();
    json_set_str(p, "sessionId", s->id);
    json_set_str(p, "path", abs);
    json_t *r = client_request(s->acp, "fs/read_text_file", p, NULL, NULL, NULL, err);
    const char *c = json_get_str(r, "content");
    char *out = r ? xstrdup(c ? c : "") : NULL;
    json_free(r);
    return out;
}

static int bridge_write(tool_bridge_t *b, const char *abs, const char *content, char **err) {
    asess_t *s = b->ud;
    json_t *p = json_obj();
    json_set_str(p, "sessionId", s->id);
    json_set_str(p, "path", abs);
    json_set_str(p, "content", content);
    json_t *r = client_request(s->acp, "fs/write_text_file", p, NULL, NULL, NULL, err);
    if (!r) return -1;
    json_free(r);
    return 0;
}

static json_t *terminal_params(asess_t *s, const char *tid) {
    json_t *p = json_obj();
    json_set_str(p, "sessionId", s->id);
    json_set_str(p, "terminalId", tid);
    return p;
}

/* one terminal request (output, kill, release) bounded by `grace_ms` */
static json_t *terminal_call(asess_t *s, const char *method, const char *tid, int64_t grace_ms) {
    apend_t *p = client_send(s->acp, method, terminal_params(s, tid));
    if (client_wait(s->acp, p, grace_ms, NULL) != 1) {
        client_take(s->acp, p, NULL);
        return NULL;
    }
    return client_take(s->acp, p, NULL);
}

static void terminal_partial(tool_ctx_t *ctx, const char *tid, const char *text) {
    if (!ctx || !ctx->update) return;
    tool_result_t pr = {0};
    pr.content = json_arr();
    if (text) json_push(pr.content, text_block(text));
    pr.details = json_obj();
    json_set_str(pr.details, "terminalId", tid);
    ctx->update(ctx, &pr);
    tool_result_free(&pr);
}

static void terminal_take_output(json_t *o, tool_terminal_res_t *res) {
    const char *out = json_get_str(o, "output");
    free(res->output);
    res->output = xstrdup(out ? out : "");
    res->truncated = json_get_bool(o, "truncated", false);
}

static int bridge_terminal(tool_bridge_t *b, tool_ctx_t *ctx, const tool_terminal_req_t *req, tool_terminal_res_t *res,
                           char **err) {
    asess_t *s = b->ud;
    acp_t *a = s->acp;
    volatile sig_atomic_t *cancel = ctx ? ctx->cancel : NULL;
    if (ctx && ctx->tool_call_id) flush_held(s, ctx->tool_call_id);
    if (cancel && *cancel) {
        res->aborted = true;
        return 0;
    }
    json_t *p = json_obj(), *args = json_arr(), *env = json_arr();
    json_set_str(p, "sessionId", s->id);
    json_set_str(p, "command", req->command);
    for (size_t i = 0; req->args && i < req->args->n; i++) json_push_str(args, req->args->v[i]);
    json_set(p, "args", args);
    /* the session identity the command runs under (BashTool #sessionEnv) */
    const char *names[] = {"PI_SESSION_ID", "PI_SESSION_FILE", "PI_TOOL_CALL_ID"};
    const char *vals[] = {s->id, app_session_file(s->app), ctx ? ctx->tool_call_id : NULL};
    for (int i = 0; i < 3; i++) {
        json_t *e = json_obj();
        json_set_str(e, "name", names[i]);
        json_set_str(e, "value", vals[i] ? vals[i] : "");
        json_push(env, e);
    }
    for (size_t i = 0; req->env && i < req->env->n; i++) {
        const char *eq = strchr(req->env->v[i], '=');
        if (!eq) continue;
        json_t *e = json_obj();
        char *n = xstrndup(req->env->v[i], (size_t)(eq - req->env->v[i]));
        json_set_str(e, "name", n);
        json_set_str(e, "value", eq + 1);
        free(n);
        json_push(env, e);
    }
    json_set(p, "env", env);
    if (req->cwd) json_set_str(p, "cwd", req->cwd);
    if (req->output_limit > 0) json_set_int(p, "outputByteLimit", req->output_limit);
    int64_t t0 = mono_ms();
    int64_t deadline = req->timeout_ms > 0 ? t0 + req->timeout_ms : 0;
    apend_t *cp = client_send(a, "terminal/create", p);
    for (;;) {
        int64_t slice = deadline ? MAX(1, MIN(250, deadline - mono_ms())) : 250;
        int rc = client_wait(a, cp, slice, cancel);
        if (rc == 1) break;
        if (rc < 0 || (deadline && mono_ms() >= deadline)) {
            /* abandoned before the client answered: kill + release a terminal it still creates (cleanupLateCreate) */
            bool timed = rc == 0;
            json_t *late = client_wait(a, cp, 1000, NULL) == 1 ? client_take(a, cp, NULL) : (client_take(a, cp, NULL), NULL);
            const char *ltid = json_get_str(late, "terminalId");
            if (ltid) {
                json_free(terminal_call(s, "terminal/kill", ltid, 1000));
                json_free(terminal_call(s, "terminal/release", ltid, 1000));
            }
            json_free(late);
            if (timed) res->timed_out = true;
            else res->aborted = true;
            res->output = xstrdup("");
            return 0;
        }
    }
    char *cerr = NULL;
    json_t *created = client_take(a, cp, &cerr);
    if (!created) {
        *err = cerr ? cerr : xstrdup("terminal/create failed");
        return -1;
    }
    char *tid = xstrdup(json_get_str(created, "terminalId") ? json_get_str(created, "terminalId") : "");
    json_free(created);
    apend_t *wp = client_send(a, "terminal/wait_for_exit", terminal_params(s, tid));
    terminal_partial(ctx, tid, NULL); /* the editor embeds the live terminal card */
    int rcode = 0;
    bool exited = false;
    json_t *exit_status = NULL;
    for (;;) {
        int64_t slice = deadline ? MAX(1, MIN(250, deadline - mono_ms())) : 250;
        int rc = client_wait(a, wp, slice, cancel);
        if (rc == 1) {
            char *werr = NULL;
            exit_status = client_take(a, wp, &werr);
            wp = NULL;
            if (!exit_status) {
                *err = werr ? werr : xstrdup("terminal/wait_for_exit failed");
                rcode = -1;
            }
            free(exit_status ? werr : NULL);
            exited = exit_status != NULL;
            break;
        }
        if (rc < 0) {
            res->aborted = true;
            break;
        }
        if (deadline && mono_ms() >= deadline) {
            res->timed_out = true;
            break;
        }
        json_t *o = terminal_call(s, "terminal/output", tid, 2000); /* poll tick */
        if (o) {
            terminal_take_output(o, res);
            terminal_partial(ctx, tid, res->output);
            json_free(o);
        }
    }
    if (wp) client_take(a, wp, NULL);
    if (res->aborted || res->timed_out) json_free(terminal_call(s, "terminal/kill", tid, 1000)); /* kill before reading */
    if (!res->aborted && rcode == 0) {
        json_t *o = terminal_call(s, "terminal/output", tid, 2000);
        if (o) {
            terminal_take_output(o, res);
            json_free(o);
        }
    }
    if (exited) {
        json_t *code = json_get(exit_status, "exitCode");
        res->exited = true;
        res->exit_code = json_is_num(code) ? (int)json_get_int(exit_status, "exitCode", 0) : -1;
        res->signaled = !json_is_num(code) && json_get_str(exit_status, "signal") != NULL;
    }
    json_free(exit_status);
    json_free(terminal_call(s, "terminal/release", tid, 1000));
    free(tid);
    if (!res->output) res->output = xstrdup("");
    return rcode;
}

/* ---------- elicitation (createAcpExtensionUiContext) ---------- */

static void flush_all_held(void *ud) {
    asess_t *s = ud;
    pthread_mutex_lock(&s->mu);
    while (json_len(s->held)) {
        const char *k = NULL;
        json_t *v;
        json_obj_foreach(s->held, i, k, v) break;
        if (!k) break;
        char *key = xstrdup(k);
        flush_held_locked(s, key);
        free(key);
    }
    pthread_mutex_unlock(&s->mu);
}

/* unstable_createElicitation (form): accepted content (owned) or NULL */
static json_t *elicit_form(asess_t *s, const char *message, json_t *properties, json_t *required) {
    json_t *p = json_obj(), *schema = json_obj();
    json_set_str(p, "mode", "form");
    json_set_str(p, "sessionId", s->id);
    json_set_str(p, "message", message);
    json_set_str(schema, "type", "object");
    json_set(schema, "properties", properties);
    if (required) json_set(schema, "required", required);
    json_set(p, "requestedSchema", schema);
    json_t *r = client_request(s->acp, "elicitation/create", p, &s->app->agent->cancel, flush_all_held, s, NULL);
    json_t *content = str_eq(json_get_str(r, "action"), "accept") ? json_take(r, "content") : NULL;
    json_free(r);
    if (content && !json_is_obj(content)) {
        json_free(content);
        content = NULL;
    }
    return content;
}

/* askDialog: one form for all questions (q<i>, q<i>__other) */
static json_t *acp_ask(void *ud, const json_t *questions) {
    asess_t *s = ud;
    json_t *props = json_obj(), *q, *o;
    size_t nq = json_len(questions);
    json_arr_foreach(questions, i, q) {
        char *key = xasprintf("q%zu", i), *okey = xasprintf("q%zu__other", i);
        json_t *entries = json_arr();
        json_arr_foreach(json_get_arr(q, "options"), k, o) {
            const char *label = json_is_str(o) ? json_sv(o) : json_get_str(o, "label");
            json_t *e = json_obj();
            json_set_str(e, "const", label ? label : "");
            json_set_str(e, "title", label ? label : "");
            const char *d = json_get_str(o, "description");
            char *dt = d ? trim_dup(d) : NULL;
            if (dt && *dt) json_set_str(e, "description", dt);
            free(dt);
            json_push(entries, e);
        }
        const char *hdr = json_get_str(q, "header");
        char *desc = hdr ? trim_dup(hdr) : NULL;
        if (json_len(entries)) {
            json_t *pr = json_obj();
            if (json_get_bool(q, "multi", false)) {
                json_set_str(pr, "type", "array");
                json_set_str(pr, "title", json_get_str(q, "question") ? json_get_str(q, "question") : "");
                if (desc && *desc) json_set_str(pr, "description", desc);
                json_t *items = json_obj();
                json_set(items, "anyOf", entries);
                json_set(pr, "items", items);
            } else {
                json_set_str(pr, "type", "string");
                json_set_str(pr, "title", json_get_str(q, "question") ? json_get_str(q, "question") : "");
                if (desc && *desc) json_set_str(pr, "description", desc);
                int64_t rec = json_get_int(q, "recommended", -1);
                json_t *opts = json_get_arr(q, "options");
                json_set(pr, "oneOf", entries);
                if (json_get(q, "recommended") && rec >= 0 && (size_t)rec < json_len(opts)) {
                    json_t *ro = json_at(opts, (size_t)rec);
                    const char *rl = json_is_str(ro) ? json_sv(ro) : json_get_str(ro, "label");
                    if (rl) json_set_str(pr, "default", rl);
                }
            }
            json_set(props, key, pr);
        } else json_free(entries);
        free(desc);
        json_t *other = json_obj();
        json_set_str(other, "type", "string");
        json_set_str(other, "title", OTHER_OPTION);
        json_set(props, okey, other);
        free(key);
        free(okey);
    }
    char *msg = nq == 1 ? xstrdup(json_get_str(json_at(questions, 0), "question") ? json_get_str(json_at(questions, 0), "question") : "")
                        : xasprintf("Answer %zu questions", nq);
    json_t *content = elicit_form(s, msg, props, NULL);
    free(msg);
    if (!content) return NULL;
    json_t *ans = json_obj(), *results = json_arr();
    json_arr_foreach(questions, i, q) {
        char *key = xasprintf("q%zu", i), *okey = xasprintf("q%zu__other", i);
        json_t *r = json_obj(), *sel = json_arr(), *labels = json_arr();
        json_arr_foreach(json_get_arr(q, "options"), k, o) {
            const char *l = json_is_str(o) ? json_sv(o) : json_get_str(o, "label");
            if (l) json_push_str(labels, l);
        }
        const char *ov = json_get_str(content, okey);
        char *custom = ov ? trim_dup(ov) : NULL;
        if (custom && !*custom) {
            free(custom);
            custom = NULL;
        }
        json_t *v = json_get(content, key), *x, *y;
        if (json_get_bool(q, "multi", false)) {
            json_arr_foreach(v, k, x) {
                bool known = false;
                json_arr_foreach(labels, m, y) known |= json_is_str(x) && str_eq(json_sv(x), json_sv(y));
                if (known) json_push_str(sel, json_sv(x));
            }
        } else if (!custom && json_is_str(v)) {
            bool known = false;
            json_arr_foreach(labels, m, y) known |= str_eq(json_sv(v), json_sv(y));
            if (known) json_push_str(sel, json_sv(v));
        }
        json_set_str(r, "id", json_get_str(q, "id") ? json_get_str(q, "id") : "");
        json_set(r, "selectedOptions", sel);
        if (custom) json_set_str(r, "customInput", custom);
        free(custom);
        json_free(labels);
        json_push(results, r);
        free(key);
        free(okey);
    }
    json_free(content);
    json_set(ans, "__results", results);
    return ans;
}

/* #requestAcpPlanApprovalChoice: approve only on the explicit approve selection */
static bool acp_plan_choice(void *ud, const char *title, const char *content) {
    asess_t *s = ud;
    strvec_t lines = str_split(content ? content : "", "\n");
    sb_t prev = SB_INIT;
    for (size_t i = 0; i < lines.n && i < 12; i++) {
        if (i) sb_putc(&prev, '\n');
        sb_append(&prev, lines.v[i]);
    }
    char *msg = xasprintf("Approve plan \"%s\" and start implementation?\n\n%s%s", title ? title : "", sb_cstr(&prev),
                          lines.n > 12 ? "\n\xe2\x80\xa6" : "");
    strvec_free(&lines);
    sb_free(&prev);
    json_t *props = json_obj(), *v = json_obj(), *en = json_arr(), *req = json_arr();
    json_set_str(v, "type", "string");
    json_push_str(en, APPROVE_OPTION);
    json_push_str(en, REFINE_OPTION);
    json_set(v, "enum", en);
    json_set(props, "value", v);
    json_push_str(req, "value");
    json_t *c = elicit_form(s, msg, props, req);
    free(msg);
    bool ok = c && str_eq(json_get_str(c, "value"), APPROVE_OPTION);
    json_free(c);
    return ok;
}

/* plan approved (xd://propose): current_mode_update + config_option_update */
static void acp_plan_mode_changed(void *ud) {
    asess_t *s = ud;
    push_mode_update(s);
    push_config_update(s);
}

/* ---------- session records ---------- */

static asess_t *find_session(acp_t *a, const char *id) {
    asess_t *r = NULL;
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; id && i < a->sessions.n; i++)
        if (!strcmp(a->sessions.v[i]->id, id)) r = a->sessions.v[i];
    pthread_mutex_unlock(&a->mu);
    return r;
}

/* McpServer -> MCPServerConfig */
static json_t *mcp_config(const json_t *servers, char **err) {
    json_t *cfg = json_obj(), *sv;
    json_arr_foreach(servers, i, sv) {
        const char *name = json_get_str(sv, "name");
        json_t *c = json_obj(), *map = json_obj(), *nv;
        bool stdio = json_get(sv, "command") != NULL;
        const char *type = json_get_str(sv, "type");
        json_t *pairs = json_get_arr(sv, stdio ? "env" : "headers");
        json_arr_foreach(pairs, k, nv) if (json_get_str(nv, "name")) json_set_str(map, json_get_str(nv, "name"), json_get_str(nv, "value") ? json_get_str(nv, "value") : "");
        if (stdio) {
            json_set_str(c, "type", "stdio");
            json_set_str(c, "command", json_get_str(sv, "command") ? json_get_str(sv, "command") : "");
            json_set(c, "args", json_get_arr(sv, "args") ? json_clone(json_get_arr(sv, "args")) : json_arr());
            json_set(c, "env", map);
        } else if (str_eq(type, "http") || str_eq(type, "sse")) {
            json_set_str(c, "type", type);
            json_set_str(c, "url", json_get_str(sv, "url") ? json_get_str(sv, "url") : "");
            json_set(c, "headers", map);
        } else {
            *err = xasprintf("Unsupported MCP server transport: %s", type ? type : "undefined");
            json_free(map);
            json_free(c);
            json_free(cfg);
            return NULL;
        }
        json_set(cfg, name ? name : "", c);
    }
    return cfg;
}

/* connect client-declared servers; NULL manager when none (failures are kept for the bootstrap chunks) */
static int mcp_connect(const json_t *servers, mcp_manager_t **out, json_t **failed, char **err) {
    *out = NULL;
    *failed = json_arr();
    if (!json_len(servers)) return 0;
    json_t *cfg = mcp_config(servers, err);
    if (!cfg) return -1;
    mcp_manager_t *m = mcp_manager_new();
    mcp_connect_all(m, cfg, settings_int("mcp.startupTimeoutMs", 15000));
    json_free(cfg);
    json_free(*failed);
    *failed = mcp_failed(m);
    *out = m;
    return 0;
}

/* #configureMcpServers on a loaded record: the client's servers replace the previous set */
static int reconfigure_mcp(asess_t *s, const json_t *servers, char **err) {
    mcp_manager_t *m = NULL;
    json_t *failed = NULL;
    if (mcp_connect(servers, &m, &failed, err) != 0) {
        json_free(failed);
        return -1;
    }
    app_t *app = s->app;
    agent_t *ag = app->agent;
    strvec_t names = {0};
    pthread_mutex_lock(&ag->mu);
    for (size_t i = 0; i < ag->all_tools.n; i++)
        if (str_starts(ag->all_tools.v[i]->name, "mcp__")) strvec_push(&names, ag->all_tools.v[i]->name);
    pthread_mutex_unlock(&ag->mu);
    for (size_t i = 0; i < names.n; i++) agent_remove_tool(ag, names.v[i]);
    strvec_free(&names);
    mcp_manager_t *old = s->mcp;
    app->mcp = m;
    s->mcp = m;
    if (m && !app->opts.no_tools) {
        mcp_register_tools(m, ag);
        for (size_t i = 0; i < ag->all_tools.n; i++)
            if (str_starts(ag->all_tools.v[i]->name, "mcp__")) ag->all_tools.v[i]->discoverable = true;
    }
    tools_finalize(ag, app->opts.tools);
    app_rebuild_system_prompt(app);
    if (old) mcp_manager_free(old);
    json_t *f;
    json_arr_foreach(failed, i, f) json_push(s->mcp_chunks, json_clone(f));
    json_free(failed);
    return 0;
}

/* createSession + #registerPreparedSession; exactly one of resume / fork may be set */
static asess_t *open_record(acp_t *a, const char *cwd, const char *resume, const char *fork, const json_t *servers, char **err) {
    mcp_manager_t *m = NULL;
    json_t *failed = NULL;
    if (mcp_connect(servers, &m, &failed, err) != 0) {
        json_free(failed);
        return NULL;
    }
    app_opts_t o = *a->base;
    o.cwd = cwd;
    o.resume = resume;
    o.fork = fork;
    o.continue_recent = false;
    o.no_session = false;
    o.no_mcp = true; /* MCP in ACP sessions belongs to the client (session/new mcpServers) */
    o.mcp = m;
    o.interactive_prompts = caps_form(a);
    app_t *app = app_create(&o, err);
    if (!app) {
        if (m) mcp_manager_free(m);
        json_free(failed);
        return NULL;
    }
    if (app->session) session_ensure_on_disk(app->session);
    asess_t *s = xcalloc(1, sizeof(*s));
    pthread_mutex_init(&s->mu, NULL);
    s->app = app;
    s->acp = a;
    s->mcp = m;
    s->mcp_chunks = failed;
    s->decisions = json_obj();
    s->tool_args = json_obj();
    s->held = json_obj();
    s->id = xstrdup(session_id(app->session));
    app->plan_mode_changed = acp_plan_mode_changed;
    app->plan_mode_changed_ud = s;
    if (caps_form(a)) {
        app->plan_approval_choice = acp_plan_choice;
        app->env->ui.ask = acp_ask;
        app->env->ui.ud = s;
    }
    app->notice = acp_notice;
    app->notice_ud = s;
    s->bridge.ud = s;
    if (caps_bool(a, "fs.readTextFile")) s->bridge.read_text = bridge_read;
    if (caps_bool(a, "fs.writeTextFile")) s->bridge.write_text = bridge_write;
    if (caps_bool(a, "terminal")) s->bridge.run_terminal = bridge_terminal;
    if (s->bridge.read_text || s->bridge.write_text || s->bridge.run_terminal) app->env->bridge = &s->bridge;
    agent_subscribe(app->agent, acp_listener, s);
    app->permission_gate = acp_permission_gate;
    app->permission_gate_ud = s;
    pthread_mutex_lock(&a->mu);
    vec_push(&a->sessions, s);
    pthread_mutex_unlock(&a->mu);
    return s;
}

static void turn_free(turn_t *t) {
    json_free(t->last);
    json_free(t->req_id);
    json_free(t->prompt);
    free(t);
}

/* #finishPrompt: the response goes out once */
static bool settle(turn_t *t, json_t *result) {
    asess_t *s = t->s;
    pthread_mutex_lock(&s->mu);
    if (t->settled) {
        pthread_mutex_unlock(&s->mu);
        json_free(result);
        return false;
    }
    t->settled = true;
    json_t *id = json_clone(t->req_id);
    pthread_mutex_unlock(&s->mu);
    reply(id, result);
    json_free(id);
    return true;
}

static void settle_error(turn_t *t, const char *msg) {
    asess_t *s = t->s;
    pthread_mutex_lock(&s->mu);
    bool was = t->settled;
    t->settled = true;
    pthread_mutex_unlock(&s->mu);
    if (!was) reply_internal(t->req_id, msg);
}

/* #buildTurnUsage */
static json_t *turn_usage(const turn_t *t) {
    if (t->usage[4] <= 0) return NULL;
    json_t *u = json_obj();
    json_set_int(u, "inputTokens", t->usage[0]);
    json_set_int(u, "outputTokens", t->usage[1]);
    json_set_int(u, "totalTokens", t->usage[4]);
    if (t->usage[2] > 0) json_set_int(u, "cachedReadTokens", t->usage[2]);
    if (t->usage[3] > 0) json_set_int(u, "cachedWriteTokens", t->usage[3]);
    return u;
}

static json_t *stop_result(const char *reason, const turn_t *t) {
    json_t *r = json_obj();
    json_set_str(r, "stopReason", reason);
    json_t *u = t ? turn_usage(t) : NULL;
    if (u) json_set(r, "usage", u);
    return r;
}

/* #beginCancelCleanup: answer `cancelled` right away (the caller aborts the run). `only` limits it to that turn
 * (the implicit cancel of a new prompt), NULL cancels every accepted turn (session/cancel, close). The turns are
 * marked under the lock (their threads free them) and answered after it. */
static void cancel_turns(asess_t *s, const turn_t *only) {
    json_t *ids = json_arr(), *results = json_arr();
    pthread_mutex_lock(&s->mu);
    for (size_t i = 0; i < s->turns.n; i++) {
        turn_t *t = s->turns.v[i];
        if (only && t != only) continue;
        t->cancel_requested = true;
        if (t->settled) continue;
        t->settled = true;
        json_push(ids, json_clone(t->req_id));
        json_push(results, stop_result("cancelled", t));
    }
    pthread_mutex_unlock(&s->mu);
    for (size_t i = 0; i < json_len(ids); i++) reply(json_at(ids, i), json_clone(json_at(results, i)));
    json_free(ids);
    json_free(results);
}

static double session_cost(app_t *a) {
    double c = 0;
    json_t *e;
    if (!a->session) return a->total_cost;
    json_arr_foreach(a->session->entries, i, e) {
        json_t *m = json_get(e, "message");
        if (str_eq(json_get_str(e, "type"), "message") && str_eq(json_get_str(m, "role"), "assistant"))
            c += json_get_num(json_get(json_get(m, "usage"), "cost"), "total", 0);
    }
    return c;
}

/* #emitEndOfTurnUpdates */
static void end_of_turn_updates(asess_t *s) {
    app_t *a = s->app;
    if (a->model && a->model->context_window > 0) {
        json_t *u = json_obj();
        json_set_str(u, "sessionUpdate", "usage_update");
        json_set_int(u, "size", a->model->context_window);
        json_set_int(u, "used", MAX(0, ui_context_used(a)));
        double cost = session_cost(a);
        if (cost > 0) {
            json_t *c = json_obj();
            json_set_num(c, "amount", cost);
            json_set_str(c, "currency", "USD");
            json_set(u, "cost", c);
        }
        notify_update(s->id, u);
    }
    push_session_info(s, NULL);
}

static bool silent_abort(const json_t *m) { return str_eq(json_get_str(m, "errorMessage"), "__omp.silent_abort__"); }

/* #resolveStopReason */
static const char *stop_reason(const json_t *last, bool cancel_requested) {
    if (cancel_requested) return "cancelled";
    const char *r = json_get_str(last, "stopReason");
    if (str_eq(r, "aborted")) return "cancelled";
    if (str_eq(r, "length")) return "max_tokens";
    if (str_eq(r, "error")) {
        const char *m = json_get_str(last, "errorMessage");
        char *low = xstrdup(m ? m : "");
        for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
        /* /content[_ ]?filter|refus(al|ed)/i */
        bool refusal = strstr(low, "contentfilter") || strstr(low, "content_filter") || strstr(low, "content filter") ||
                       strstr(low, "refusal") || strstr(low, "refused");
        free(low);
        return refusal ? "refusal" : "end_turn";
    }
    return "end_turn";
}

/* #convertPromptBlocks */
static char *convert_prompt(const json_t *blocks, json_t *images) {
    strvec_t parts = {0};
    json_t *b;
    json_arr_foreach(blocks, i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "text")) strvec_push(&parts, json_get_str(b, "text") ? json_get_str(b, "text") : "");
        else if (str_eq(t, "image")) {
            if (json_get_str(b, "data") && json_get_str(b, "mimeType"))
                json_push(images, image_block(json_get_str(b, "data"), json_get_str(b, "mimeType")));
        }
        else if (str_eq(t, "resource")) {
            json_t *r = json_get(b, "resource");
            const char *text = json_get_str(r, "text"), *blob = json_get_str(r, "blob"), *mime = json_get_str(r, "mimeType");
            if (text) strvec_push(&parts, text);
            else if (blob && mime && str_starts(mime, "image/")) json_push(images, image_block(blob, mime));
            else strvec_push_own(&parts, xasprintf("[embedded resource: %s]", json_get_str(r, "uri") ? json_get_str(r, "uri") : "undefined"));
        } else if (str_eq(t, "resource_link")) {
            const char *v = json_get_str(b, "title");
            if (!v) v = json_get_str(b, "name");
            if (!v) v = json_get_str(b, "uri");
            strvec_push(&parts, v ? v : "undefined");
        } else if (str_eq(t, "audio")) strvec_push(&parts, "[audio omitted]");
    }
    sb_t s = SB_INIT;
    for (size_t i = 0; i < parts.n; i++) {
        if (i) sb_append(&s, "\n\n");
        sb_append(&s, parts.v[i]);
    }
    strvec_free(&parts);
    char *r = trim_dup(sb_cstr(&s));
    sb_free(&s);
    return r;
}

/* builtin finished: changes it made (title, model/thinking/mode) reach the client */
static void builtin_side_effects(asess_t *s, const char *title_before, const char *cfg_before) {
    const char *title = s->app->session ? s->app->session->title : NULL;
    if (!str_eq(title ? title : "", title_before ? title_before : "")) push_session_info(s, NULL); /* notifyTitleChanged */
    push_config_if_changed(s, cfg_before);
}


/* #emitCommandOutput: builtin output as an agent message chunk */
static void command_output(void *ud, const char *text) {
    asess_t *s = ud;
    if (!text || !*text) return;
    char *mid = uuid_v4();
    notify_update(s->id, chunk("agent_message_chunk", text, mid));
    free(mid);
}

/* #runPromptOrCommand + the agent_end handling of #handlePromptEvent */
static void *turn_main(void *ud) {
    turn_t *t = ud;
    asess_t *s = t->s;
    app_t *a = s->app;
    if (t->has_prev) pthread_join(t->prev, NULL);
    pthread_mutex_lock(&s->mu);
    bool cancelled = t->cancel_requested || s->closed;
    if (!cancelled) {
        s->turn = t;
        free(s->last_error);
        s->last_error = NULL;
        clear_live(s);
    }
    pthread_mutex_unlock(&s->mu);
    if (cancelled) { /* stopped before it started: answered `cancelled`, never run */
        settle(t, stop_result("cancelled", t));
        goto out;
    }
    json_t *images = json_arr();
    char *text = convert_prompt(t->prompt, images);
    char *title_before = a->session && a->session->title ? xstrdup(a->session->title) : NULL;
    char *cfg_before = config_snapshot(s);
    bool full_turn = true;
    int rc = 0;
    skill_invocation_t inv;
    if (app_skill_command(a, text, &inv)) { /* #tryRunSkillCommand */
        skill_invocation_free(&inv);
        char *serr = NULL;
        json_t *msg = app_skill_message(a, text, json_len(images) ? json_clone(images) : NULL, &serr);
        if (!msg) {
            settle_error(t, serr ? serr : "Failed to load skill");
            free(serr);
            full_turn = false;
        } else rc = app_prompt_skill(a, msg);
    } else {
        slash_result_t sr = {0};
        if (text[0] == '/') slash_execute_text(a, text, command_output, s, &sr); /* executeAcpBuiltinSlashCommand */
        if (sr.kind == SLASH_CONSUMED && !sr.agent_invoked) {
            builtin_side_effects(s, title_before, cfg_before);
            settle(t, stop_result("end_turn", t));
            full_turn = false;
        } else if (sr.kind == SLASH_CONSUMED) {
            builtin_side_effects(s, title_before, cfg_before); /* e.g. /retry: its agent turn ran */
        } else {
            char *expanded = app_expand_input(a, sr.kind == SLASH_PROMPT && sr.prompt ? sr.prompt : text, true);
            if (!expanded) { /* resolved locally (MCP prompt / custom command without text): no agent turn */
                settle(t, stop_result("end_turn", NULL));
                full_turn = false;
            } else {
                rc = app_prompt(a, expanded, json_len(images) ? json_clone(images) : NULL);
                free(expanded);
            }
        }
        slash_result_free(&sr);
    }
    json_free(images);
    if (full_turn) {
        pthread_mutex_lock(&s->mu);
        bool settled = t->settled;
        json_t *last = json_clone(t->last);
        bool error_text = t->error_text;
        char *live = s->live_id ? xstrdup(s->live_id) : NULL;
        char *err = s->last_error ? xstrdup(s->last_error) : NULL;
        pthread_mutex_unlock(&s->mu);
        if (!settled && !last && rc != 0) {
            settle_error(t, err ? err : "Prompt failed");
        } else if (!settled) {
            /* #flushUnreportedTurnError: a turn-fatal provider error the stream never surfaced */
            const char *em = json_get_str(last, "errorMessage");
            if (!error_text && str_eq(json_get_str(last, "stopReason"), "error") && em && *em && !silent_abort(last)) {
                char *mid = live ? xstrdup(live) : uuid_v4();
                notify_update(s->id, chunk("agent_message_chunk", em, mid));
                free(mid);
            }
            push_config_if_changed(s, cfg_before); /* model_changed / thinking_level_changed during the turn */
            end_of_turn_updates(s);
            settle(t, stop_result(stop_reason(last, t->cancel_requested), t));
        }
        json_free(last);
        free(live);
        free(err);
    }
    free(title_before);
    free(cfg_before);
    free(text);
out:
    pthread_mutex_lock(&s->mu);
    if (s->turn == t) {
        s->turn = NULL;
        clear_live(s);
    }
    for (size_t i = 0; i < s->turns.n; i++)
        if (s->turns.v[i] == t) {
            memmove(s->turns.v + i, s->turns.v + i + 1, (s->turns.n - i - 1) * sizeof(void *));
            s->turns.n--;
            break;
        }
    pthread_mutex_unlock(&s->mu);
    turn_free(t);
    return NULL;
}

/* ---------- history replay (#replaySessionHistory) ---------- */

/* #extractReplayContent: text and image blocks of an array content */
static json_t *replay_content(const json_t *content) {
    json_t *out = json_arr(), *it;
    json_arr_foreach(json_is_arr(content) ? content : NULL, i, it) {
        const char *type = json_get_str(it, "type");
        if (str_eq(type, "text") && str_prop(it, "text")) {
            json_t *b = json_obj();
            json_set_str(b, "type", "text");
            json_set_str(b, "text", json_get_str(it, "text"));
            json_push(out, b);
        } else if (str_eq(type, "image") && json_get_str(it, "data") && json_get_str(it, "mimeType")) {
            json_t *b = json_obj();
            json_set_str(b, "type", "image");
            json_set_str(b, "data", json_get_str(it, "data"));
            json_set_str(b, "mimeType", json_get_str(it, "mimeType"));
            json_push(out, b);
        }
    }
    return out;
}

static void replay_wrap(asess_t *s, json_t *blocks, const char *kind) {
    char *mid = uuid_v4();
    json_t *b;
    json_arr_foreach(blocks, i, b) {
        json_t *u = json_obj();
        json_set_str(u, "sessionUpdate", kind);
        json_set(u, "content", json_clone(b));
        json_set_str(u, "messageId", mid);
        notify_update(s->id, u);
    }
    free(mid);
    json_free(blocks);
}

static void replay_assistant(asess_t *s, const json_t *m, json_t *replayed) {
    char *mid = uuid_v4();
    int n = 0;
    json_t *it;
    json_arr_foreach(json_get_arr(m, "content"), i, it) {
        const char *type = json_get_str(it, "type");
        if (str_eq(type, "text") && str_prop(it, "text")) {
            notify_update(s->id, chunk("agent_message_chunk", json_get_str(it, "text"), mid));
            n++;
        } else if (str_eq(type, "image") && json_get_str(it, "data") && json_get_str(it, "mimeType")) {
            json_t *u = json_obj(), *c = json_obj();
            json_set_str(u, "sessionUpdate", "agent_message_chunk");
            json_set_str(c, "type", "image");
            json_set_str(c, "data", json_get_str(it, "data"));
            json_set_str(c, "mimeType", json_get_str(it, "mimeType"));
            json_set(u, "content", c);
            json_set_str(u, "messageId", mid);
            notify_update(s->id, u);
            n++;
        } else if (str_eq(type, "thinking") && json_get_str(it, "thinking")) {
            char *th = trim_dup(json_get_str(it, "thinking"));
            if (*th) {
                notify_update(s->id, chunk(json_get_bool(it, "narration", false) ? "agent_message_chunk" : "agent_thought_chunk", th, mid));
                n++;
            }
            free(th);
        } else if ((str_eq(type, "toolCall") || str_eq(type, "tool_use")) && json_get_str(it, "id") && json_get_str(it, "name")) {
            json_t *args;
            json_t *raw = json_get(it, "arguments");
            if (raw) {
                if (json_is_str(raw)) {
                    args = json_parse_str(json_sv(raw));
                    if (!args) args = json_clone(raw);
                } else args = json_is_null(raw) ? json_obj() : json_clone(raw);
            } else if (str_eq(type, "tool_use") && json_get(it, "input")) args = json_clone(json_get(it, "input"));
            else args = json_obj();
            notify_update(s->id, tool_call_update(json_get_str(it, "id"), json_get_str(it, "name"), args, NULL, s->app->cwd));
            json_set(replayed, json_get_str(it, "id"), args);
            n++;
        }
    }
    const char *em = json_get_str(m, "errorMessage");
    if (!n && em && *em && !silent_abort(m)) notify_update(s->id, chunk("agent_message_chunk", em, mid));
    free(mid);
}

static void replay_tool_result(asess_t *s, const json_t *m, json_t *replayed) {
    const char *id = json_get_str(m, "toolCallId"), *name = json_get_str(m, "toolName");
    const char *cwd = s->app->cwd;
    json_t *details = json_get(m, "details");
    if (!json_get(replayed, id)) { /* the assistant call was not replayed: start from the result */
        json_t *args = json_obj();
        const char *p = str_prop(details, "path");
        if (p) json_set_str(args, "path", p);
        notify_update(s->id, tool_call_update(id, name, args, NULL, cwd));
        json_free(args);
    }
    json_t *result = json_obj();
    if (json_get(m, "content")) json_set(result, "content", json_clone(json_get(m, "content")));
    if (details) json_set(result, "details", json_clone(details));
    if (json_get_str(m, "errorMessage")) json_set_str(result, "errorMessage", json_get_str(m, "errorMessage"));
    tool_end_updates(s->id, id, name, json_get(replayed, id), result, json_get_bool(m, "isError", false), cwd);
    json_free(result);
}

static void replay_history(asess_t *s) {
    json_t *replayed = json_obj(), *m;
    json_t *msgs = json_clone(s->app->agent->messages);
    json_arr_foreach(msgs, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) replay_assistant(s, m, replayed);
        else if (str_eq(role, "user") || str_eq(role, "developer") || str_eq(role, "custom") || str_eq(role, "hookMessage") ||
                 str_eq(role, "bashExecution") || str_eq(role, "pythonExecution") || str_eq(role, "compactionSummary"))
            replay_wrap(s, replay_content(json_get(m, "content")), "user_message_chunk");
        else if (str_eq(role, "toolResult") && json_get_str(m, "toolCallId") && json_get_str(m, "toolName"))
            replay_tool_result(s, m, replayed);
    }
    json_free(msgs);
    json_free(replayed);
}

/* ---------- stored sessions ---------- */

static void list_sessions(acp_t *a, const char *cwd, session_list_t *out) {
    memset(out, 0, sizeof(*out));
    if (cwd) {
        char *dir = a->base->session_dir ? xstrdup(a->base->session_dir) : session_dir_for_cwd(cwd);
        session_list(dir, out);
        free(dir);
    } else session_list_all(out);
    /* newest first */
    for (size_t i = 1; i < out->n; i++) {
        session_info_t x = out->v[i];
        size_t j = i;
        while (j > 0 && out->v[j - 1].mtime_ms < x.mtime_ms) {
            out->v[j] = out->v[j - 1];
            j--;
        }
        out->v[j] = x;
    }
}

static json_t *session_info_json(const session_info_t *e) {
    json_t *o = json_obj(), *meta = json_obj();
    json_set_str(o, "sessionId", e->id);
    json_set_str(o, "cwd", e->cwd ? e->cwd : "");
    if (e->title) json_set_str(o, "title", e->title);
    char *ts = iso8601_from_ms(e->mtime_ms);
    json_set_str(o, "updatedAt", ts);
    free(ts);
    struct stat st;
    json_set_int(meta, "messageCount", e->message_count);
    json_set_int(meta, "size", e->path && stat(e->path, &st) == 0 ? (int64_t)st.st_size : 0);
    json_set(o, "_meta", meta);
    return o;
}

/* #findStoredSession: the cwd's sessions first, then a global by-id scan; owned path or NULL */
static char *find_stored(acp_t *a, const char *id, const char *cwd) {
    char *path = NULL;
    for (int pass = 0; pass < 2 && !path; pass++) {
        if (pass == 0 && !cwd) continue;
        session_list_t l;
        list_sessions(a, pass == 0 ? cwd : NULL, &l);
        for (size_t i = 0; i < l.n && !path; i++)
            if (str_eq(l.v[i].id, id)) path = xstrdup(l.v[i].path);
        session_list_free(&l);
    }
    return path;
}

/* ---------- requests ---------- */

static bool absolute_cwd(const json_t *id, const char *cwd) {
    if (cwd && cwd[0] == '/') return true;
    reply_internalf(id, "ACP cwd must be absolute: %s", cwd ? cwd : "undefined");
    return false;
}

static char *norm_path(const char *p) { return path_abs(p, "/"); }

static void session_response(asess_t *s, json_t *r) {
    json_set(r, "configOptions", config_options(s));
    json_set(r, "modes", mode_state(s));
}

static bool assert_cwd(const json_t *id, asess_t *s, const char *cwd) {
    char *want = norm_path(cwd), *have = norm_path(s->app->cwd);
    bool ok = str_eq(want, have);
    if (!ok) reply_internalf(id, "ACP session %s is already loaded for %s, not %s", s->id, have, want);
    free(want);
    free(have);
    return ok;
}

static bool turn_in_flight(asess_t *s) {
    pthread_mutex_lock(&s->mu);
    bool r = s->turns.n > 0;
    pthread_mutex_unlock(&s->mu);
    return r;
}

/* session/load and session/resume */
static void load_or_resume(acp_t *a, const json_t *id, const json_t *params, bool load) {
    const char *cwd = json_get_str(params, "cwd"), *sid = json_get_str(params, "sessionId");
    if (!absolute_cwd(id, cwd)) return;
    json_t *servers = json_get_arr(params, "mcpServers");
    asess_t *s = find_session(a, sid);
    char *err = NULL;
    if (s) {
        if (!assert_cwd(id, s, cwd)) return;
        if (reconfigure_mcp(s, servers, &err) != 0) {
            reply_internal(id, err);
            free(err);
            return;
        }
    } else {
        char *path = sid ? find_stored(a, sid, cwd) : NULL;
        if (!path) {
            reply_internalf(id, "ACP session not found: %s", sid ? sid : "undefined");
            return;
        }
        char *ncwd = norm_path(cwd);
        s = open_record(a, ncwd, path, NULL, servers, &err);
        free(ncwd);
        free(path);
        if (!s) {
            reply_internal(id, err ? err : "failed to open session");
            free(err);
            return;
        }
    }
    if (load) replay_history(s);
    json_t *r = json_obj();
    session_response(s, r);
    reply(id, r);
    bootstrap_updates(s);
}

/* #closeManagedSession: drop the record, stop its prompts, dispose */
static void close_record(acp_t *a, asess_t *s) {
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->sessions.n; i++)
        if (a->sessions.v[i] == s) {
            memmove(a->sessions.v + i, a->sessions.v + i + 1, (a->sessions.n - i - 1) * sizeof(void *));
            a->sessions.n--;
            break;
        }
    pthread_mutex_unlock(&a->mu);
    pthread_mutex_lock(&s->mu);
    s->closed = true;
    pthread_mutex_unlock(&s->mu);
    cancel_turns(s, NULL);
    app_abort(s->app);
    if (s->has_th) pthread_join(s->th, NULL);
    app_free(s->app);
    if (s->mcp) mcp_manager_free(s->mcp);
    json_free(s->decisions);
    json_free(s->tool_args);
    json_free(s->held);
    json_free(s->mcp_chunks);
    free(s->live_id);
    free(s->last_error);
    free(s->cfg_sent);
    free(s->id);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

/* toSteeringPromptBlocks: NULL with *msg when the blocks are malformed */
static json_t *steering_blocks(const json_t *prompt, const char **msg) {
    if (json_is_str(prompt)) {
        json_t *arr = json_arr(), *b = json_obj();
        json_set_str(b, "type", "text");
        json_set_str(b, "text", json_sv(prompt));
        json_push(arr, b);
        return arr;
    }
    static char buf[256];
    if (!json_is_arr(prompt)) return (*msg = "prompt must be content blocks", NULL);
    json_t *b;
    json_arr_foreach(prompt, i, b) {
        if (!json_is_obj(b)) return (*msg = "prompt blocks must be objects", NULL);
        const char *type = json_get_str(b, "type");
        if (!type) return (*msg = "prompt block requires a string type", NULL);
        const char *req[2] = {NULL, NULL};
        if (str_eq(type, "text")) req[0] = "text";
        else if (str_eq(type, "image") || str_eq(type, "audio")) req[0] = "data", req[1] = "mimeType";
        else if (str_eq(type, "resource_link")) req[0] = "uri";
        else if (!str_eq(type, "resource")) {
            snprintf(buf, sizeof buf, "unsupported prompt block type: %.200s", type);
            return (*msg = buf, NULL);
        }
        for (int k = 0; k < 2; k++)
            if (req[k] && !json_is_str(json_get(b, req[k]))) {
                snprintf(buf, sizeof buf, "prompt block %.100s requires string %s", type, req[k]);
                return (*msg = buf, NULL);
            }
        if (str_eq(type, "resource")) {
            json_t *r = json_get(b, "resource");
            if (!json_is_obj(r) || !json_is_str(json_get(r, "uri")))
                return (*msg = "resource block requires a resource object with a string uri", NULL);
            if (json_get(r, "text")) {
                if (!json_is_str(json_get(r, "text"))) return (*msg = "resource block with a text arm requires string text", NULL);
            } else if (json_get(r, "blob")) {
                if (!json_is_str(json_get(r, "blob"))) return (*msg = "resource block with a blob arm requires string blob", NULL);
            } else return (*msg = "resource block requires string text or string blob", NULL);
            if (json_get(r, "mimeType") && !json_is_str(json_get(r, "mimeType")))
                return (*msg = "resource block mimeType must be a string when present", NULL);
        }
        if (str_eq(type, "resource_link")) {
            static const char *const f[] = {"title", "name", "mimeType", "description"};
            for (int k = 0; k < 4; k++) {
                json_t *v = json_get(b, f[k]);
                if (v && !json_is_null(v) && !json_is_str(v)) {
                    snprintf(buf, sizeof buf, "resource_link %s must be a string or null when present", f[k]);
                    return (*msg = buf, NULL);
                }
            }
            json_t *sz = json_get(b, "size");
            if (sz && !json_is_null(sz) && !json_is_num(sz)) return (*msg = "resource_link size must be a number or null when present", NULL);
        }
    }
    return json_clone(prompt);
}

/* `_session/steering` (#handleSessionSteering): inject into a live client turn, never start one */
static void handle_steering(acp_t *a, const json_t *id, const json_t *params) {
    if (!json_is_obj(params)) return reply_invalid(id, STEERING_METHOD, "params must be an object");
    const char *sid = json_get_str(params, "sessionId");
    if (!sid || !*sid) return reply_invalid(id, STEERING_METHOD, "sessionId required");
    asess_t *s = find_session(a, sid);
    if (!s) {
        char *m = xasprintf("unknown session: %s", sid);
        reply_invalid(id, STEERING_METHOD, m);
        free(m);
        return;
    }
    const char *msg = NULL;
    json_t *blocks = steering_blocks(json_get(params, "prompt"), &msg);
    if (!blocks) return reply_invalid(id, STEERING_METHOD, msg);
    json_t *images = json_arr();
    char *text = convert_prompt(blocks, images);
    json_free(blocks);
    if (!*text && !json_len(images)) {
        free(text);
        json_free(images);
        return reply_invalid(id, STEERING_METHOD, "prompt is empty");
    }
    json_t *steer = json_get(json_get(params, "_meta"), "steering");
    const char *queue = json_is_obj(steer) && str_eq(json_get_str(steer, "queue"), "followUp") ? "followUp" : "steering";
    pthread_mutex_lock(&s->mu);
    bool live = s->turn && !s->turn->settled && !s->turn->cancel_requested;
    pthread_mutex_unlock(&s->mu);
    json_t *r = json_obj();
    if (live) {
        char *x = app_expand_templates(s->app, text);
        json_t *content = json_arr(), *img;
        json_push(content, text_block(x));
        json_t *norm = ai_normalize_images(images, s->app->model);
        json_arr_foreach(norm, i, img) json_push(content, json_clone(img));
        json_free(norm);
        free(x);
        json_t *m = msg_user_content(content);
        if (str_eq(queue, "followUp")) agent_follow_up(s->app->agent, m);
        else agent_steer(s->app->agent, m);
        json_set_str(r, "outcome", "injected");
    } else json_set_str(r, "outcome", "promptRequired");
    json_set_str(r, "queue", queue);
    free(text);
    json_free(images);
    reply(id, r);
}

static json_t *list_page(acp_t *a, const char *cwd, int64_t offset) {
    session_list_t l;
    list_sessions(a, cwd, &l);
    json_t *arr = json_arr();
    size_t end = MIN(l.n, (size_t)offset + SESSION_PAGE_SIZE);
    for (size_t i = (size_t)offset; i < end; i++) json_push(arr, session_info_json(&l.v[i]));
    json_t *r = json_obj();
    json_set(r, "sessions", arr);
    if (end < l.n) {
        char *c = xasprintf("%zu", end);
        json_set_str(r, "nextCursor", c);
        free(c);
    }
    session_list_free(&l);
    return r;
}

static int64_t clamp_limit(const json_t *params, int64_t lo, int64_t hi, int64_t def) {
    json_t *v = json_get(params, "limit");
    if (!json_is_num(v)) return def;
    double d = json_get_num(params, "limit", (double)def);
    return (int64_t)MAX((double)lo, MIN((double)hi, d));
}

/* extMethod: `_session/steering` and the omp session-browser helpers */
static void handle_ext(acp_t *a, const char *method, const json_t *id, const json_t *params) {
    if (!strcmp(method, STEERING_METHOD)) return handle_steering(a, id, params);
    if (!strcmp(method, "_omp/sessions/listAll") || !strcmp(method, "_omp/chats/byCwd")) {
        bool by_cwd = !strcmp(method, "_omp/chats/byCwd");
        const char *cwd = json_get_str(params, "cwd");
        if (by_cwd && !cwd) return reply_internal(id, "cwd required");
        int64_t limit = by_cwd ? clamp_limit(params, 1, 500, 100) : clamp_limit(params, 1, 5000, 1000);
        session_list_t l;
        list_sessions(a, by_cwd ? cwd : NULL, &l);
        json_t *arr = json_arr(), *r = json_obj();
        for (size_t i = 0; i < l.n && (int64_t)i < limit; i++) json_push(arr, session_info_json(&l.v[i]));
        json_set(r, "sessions", arr);
        if (!by_cwd) json_set_int(r, "total", (int64_t)l.n);
        session_list_free(&l);
        return reply(id, r);
    }
    if (!strcmp(method, "_omp/projects/list")) {
        session_list_t l;
        list_sessions(a, NULL, &l);
        json_t *projects = json_arr(), *p;
        for (size_t i = 0; i < l.n; i++) {
            if (!l.v[i].cwd || !*l.v[i].cwd) continue;
            json_t *hit = NULL;
            json_arr_foreach(projects, k, p) if (str_eq(json_get_str(p, "cwd"), l.v[i].cwd)) hit = p;
            if (hit) {
                json_set_int(hit, "sessionCount", json_get_int(hit, "sessionCount", 0) + 1);
                if (l.v[i].mtime_ms > json_get_int(hit, "lastActivityAt", 0)) {
                    json_set_int(hit, "lastActivityAt", l.v[i].mtime_ms);
                    json_set_str(hit, "lastTitle", l.v[i].title ? l.v[i].title : "");
                }
            } else {
                json_t *o = json_obj();
                json_set_str(o, "cwd", l.v[i].cwd);
                json_set_int(o, "sessionCount", 1);
                json_set_int(o, "lastActivityAt", l.v[i].mtime_ms);
                json_set_str(o, "lastTitle", l.v[i].title ? l.v[i].title : "");
                json_push(projects, o);
            }
        }
        /* stable sort by lastActivityAt desc */
        size_t n = json_len(projects);
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j > 0 && json_get_int(json_at(projects, j - 1), "lastActivityAt", 0) <
                                           json_get_int(json_at(projects, j), "lastActivityAt", 0);
                 j--) {
                json_t *x = json_arr_remove(projects, j);
                json_arr_insert(projects, j - 1, x);
            }
        json_t *r = json_obj();
        json_set(r, "projects", projects);
        json_set_int(r, "totalSessions", (int64_t)l.n);
        session_list_free(&l);
        return reply(id, r);
    }
    reply_internalf(id, "Unknown ACP ext method: %s", method);
}

static void handle_prompt(acp_t *a, const json_t *id, const json_t *params) {
    const char *sid = json_get_str(params, "sessionId");
    asess_t *s = find_session(a, sid);
    if (!s) return reply_not_found(id, sid);
    turn_t *t = xcalloc(1, sizeof(*t));
    t->s = s;
    t->req_id = json_clone(id);
    t->prompt = json_clone(json_get_arr(params, "prompt"));
    pthread_mutex_lock(&s->mu);
    turn_t *active = s->turn && !s->turn->settled ? s->turn : NULL;
    vec_push(&s->turns, t);
    t->prev = s->th;
    t->has_prev = s->has_th;
    pthread_mutex_unlock(&s->mu);
    /* a prompt while the previous turn still runs stops that turn first (the client pressed stop or sent anew) */
    if (active) {
        cancel_turns(s, active);
        app_abort(s->app);
    }
    pthread_t th;
    if (pthread_create(&th, NULL, turn_main, t) != 0) {
        pthread_mutex_lock(&s->mu);
        s->turns.n--;
        pthread_mutex_unlock(&s->mu);
        reply_internal(id, "failed to start the prompt");
        turn_free(t);
        return;
    }
    s->th = th;
    s->has_th = true;
}

/* session/cancel: stop everything accepted on the session */
static void handle_cancel(asess_t *s) {
    cancel_turns(s, NULL);
    app_abort(s->app);
}

static void handle(acp_t *a, json_t *msg) {
    const json_t *id = json_get(msg, "id");
    const char *method = json_get_str(msg, "method");
    json_t *params = json_get(msg, "params");
    if (!method) { /* a response to one of our requests */
        if (json_is_num(id)) {
            int64_t iv = id->is_int ? id->u.num.i : (int64_t)id->u.num.d;
            json_t *err = json_get(msg, "error");
            pthread_mutex_lock(&a->mu);
            for (size_t i = 0; i < a->pending.n; i++)
                if (a->pending.v[i]->id == iv && !a->pending.v[i]->done) {
                    if (err) a->pending.v[i]->error = xstrdup(json_get_str(err, "message") ? json_get_str(err, "message") : "error");
                    else a->pending.v[i]->result = json_take(msg, "result");
                    a->pending.v[i]->done = true;
                    pthread_cond_broadcast(&a->cv);
                }
            pthread_mutex_unlock(&a->mu);
        }
        return;
    }
    bool notification = !id;
    if (!strcmp(method, "$/cancel_request")) return;
    if (!strcmp(method, "session/cancel")) {
        asess_t *s = find_session(a, json_get_str(params, "sessionId"));
        if (s) handle_cancel(s);
        else if (!notification) reply_not_found(id, json_get_str(params, "sessionId"));
        else return;
        if (!notification && s) reply(id, NULL);
        return;
    }
    if (notification) return; /* extNotification: nothing to do */
    if (!strcmp(method, "initialize")) {
        json_free(a->client_caps);
        a->client_caps = json_clone(json_get(params, "clientCapabilities"));
        json_t *r = json_obj(), *info = json_obj();
        json_set_int(r, "protocolVersion", ACP_PROTOCOL_VERSION);
        json_set_str(info, "name", "omc");
        json_set_str(info, "title", "omc");
        json_set_str(info, "version", OMC_VERSION);
        json_set(r, "agentInfo", info);
        json_t *auth = json_parse_str("[{\"id\":\"agent\",\"name\":\"Use existing local credentials\",\"description\":"
                                      "\"Authenticate via the provider keys/OAuth state already configured under ~/.omc.\"}]");
        if (caps_bool(a, "auth.terminal"))
            json_push(auth, json_parse_str("{\"type\":\"terminal\",\"id\":\"terminal\",\"name\":\"Set up omc in terminal\","
                                           "\"description\":\"Launch the omc TUI to add provider keys and select models.\","
                                           "\"args\":[\"" TERMINAL_TAG "\"]}"));
        json_set(r, "authMethods", auth);
        json_set(r, "agentCapabilities",
                 json_parse_str("{\"loadSession\":true,\"mcpCapabilities\":{\"http\":true,\"sse\":true},"
                                "\"promptCapabilities\":{\"embeddedContext\":true,\"image\":true},"
                                "\"sessionCapabilities\":{\"list\":{},\"fork\":{},\"resume\":{},\"close\":{}}}"));
        json_set(r, "_meta", json_parse_str("{\"steering\":{\"supported\":true}}"));
        reply(id, r);
    } else if (!strcmp(method, "authenticate")) {
        const char *m = json_get_str(params, "methodId");
        if (str_eq(m, "agent") || (str_eq(m, "terminal") && caps_bool(a, "auth.terminal"))) reply(id, NULL);
        else reply_internalf(id, "Unknown ACP auth method: %s", m ? m : "undefined");
    } else if (!strcmp(method, "session/new")) {
        const char *cwd = json_get_str(params, "cwd");
        if (!absolute_cwd(id, cwd)) return;
        char *err = NULL, *ncwd = norm_path(cwd);
        asess_t *s = open_record(a, ncwd, NULL, NULL, json_get_arr(params, "mcpServers"), &err);
        free(ncwd);
        if (!s) {
            reply_internal(id, err ? err : "failed to create session");
            free(err);
            return;
        }
        json_t *r = json_obj();
        json_set_str(r, "sessionId", s->id);
        session_response(s, r);
        reply(id, r);
        bootstrap_updates(s);
    } else if (!strcmp(method, "session/load") || !strcmp(method, "session/resume")) {
        load_or_resume(a, id, params, !strcmp(method, "session/load"));
    } else if (!strcmp(method, "session/fork")) {
        const char *cwd = json_get_str(params, "cwd"), *sid = json_get_str(params, "sessionId");
        if (!absolute_cwd(id, cwd)) return;
        asess_t *src = find_session(a, sid);
        char *path = NULL;
        if (src) {
            if (turn_in_flight(src)) return reply_internalf(id, "ACP session fork is unavailable while a prompt is in progress: %s", sid);
            const char *f = app_session_file(src->app);
            if (!f || !*f) return reply_internalf(id, "ACP session cannot be forked before it is persisted: %s", sid);
            path = xstrdup(f);
        } else path = sid ? find_stored(a, sid, NULL) : NULL;
        if (!path) return reply_internalf(id, "ACP session not found: %s", sid ? sid : "undefined");
        char *err = NULL, *ncwd = norm_path(cwd);
        asess_t *s = open_record(a, ncwd, NULL, path, json_get_arr(params, "mcpServers"), &err);
        free(ncwd);
        free(path);
        if (!s) {
            reply_internal(id, err ? err : "ACP session fork failed");
            free(err);
            return;
        }
        json_t *r = json_obj();
        json_set_str(r, "sessionId", s->id);
        session_response(s, r);
        reply(id, r);
        bootstrap_updates(s);
    } else if (!strcmp(method, "session/close")) {
        asess_t *s = find_session(a, json_get_str(params, "sessionId"));
        if (s) close_record(a, s);
        reply(id, NULL);
    } else if (!strcmp(method, "session/list")) {
        const char *cwd = json_get_str(params, "cwd");
        if (cwd && !absolute_cwd(id, cwd)) return;
        const char *cursor = json_get_str(params, "cursor");
        int64_t off = 0;
        if (cursor && *cursor) {
            char *end = NULL;
            errno = 0;
            long long v = strtoll(cursor, &end, 10);
            if (end == cursor || v < 0) return reply_internalf(id, "Invalid ACP session cursor: %s", cursor);
            off = v;
        }
        reply(id, list_page(a, cwd, off));
    } else if (!strcmp(method, "session/prompt")) {
        handle_prompt(a, id, params);
    } else if (!strcmp(method, "session/set_mode") || !strcmp(method, "session/set_config_option")) {
        bool cfg = !strcmp(method, "session/set_config_option");
        const char *sid = json_get_str(params, "sessionId");
        asess_t *s = find_session(a, sid);
        if (!s) return reply_not_found(id, sid);
        const char *cid = cfg ? json_get_str(params, "configId") : "mode";
        json_t *val = json_get(params, cfg ? "value" : "modeId");
        if (cfg && json_is_bool(val)) return reply_internalf(id, "Unsupported boolean ACP config option: %s", cid ? cid : "undefined");
        const char *v = json_sv(val);
        if (str_eq(cid, "mode")) {
            bool plan_ok = settings_bool("plan.enabled", true);
            if (!str_eq(v, "default") && !(str_eq(v, "plan") && plan_ok))
                return reply_internalf(id, "Unsupported ACP mode: %s", v ? v : "undefined");
            app_set_plan_mode(s->app, str_eq(v, "plan"));
            push_mode_update(s);
            push_config_update(s);
        } else if (str_eq(cid, "model") || str_eq(cid, "thinking")) {
            char *before = config_snapshot(s);
            if (str_eq(cid, "model")) {
                mr_pool_t pool = {0};
                available_models(&pool);
                const ai_model_t *hit = NULL;
                for (size_t i = 0; i < pool.n && !hit; i++) {
                    char *mid = model_id(pool.v[i]);
                    if (str_eq(mid, v)) hit = pool.v[i];
                    free(mid);
                }
                mr_pool_free(&pool);
                char *err = NULL;
                if (!hit) {
                    free(before);
                    return reply_internalf(id, "Unknown ACP model: %s", v ? v : "undefined");
                }
                char *sel = model_id(hit);
                if (app_set_model(s->app, sel, &err) != 0) {
                    free(sel);
                    free(before);
                    reply_internal(id, err ? err : "failed to set model");
                    free(err);
                    return;
                }
                free(sel);
            } else {
                bool known = str_eq(v, "auto") || str_eq(v, "off");
                for (int e = EFFORT_MINIMAL; !known && v && e <= EFFORT_MAX; e++) known = str_eq(v, effort_name((ai_effort_t)e));
                if (!known || !app_set_thinking_level(s->app, v)) {
                    free(before);
                    return reply_internalf(id, "Unknown ACP thinking level: %s", v ? v : "undefined");
                }
            }
            char *after = config_snapshot(s); /* model_changed / thinking_level_changed only on a change */
            if (!str_eq(before, after)) push_config_update(s);
            free(before);
            free(after);
        } else {
            return reply_internalf(id, "Unknown ACP config option: %s", cid ? cid : "undefined");
        }
        json_t *r = json_obj();
        if (cfg) json_set(r, "configOptions", config_options(s));
        reply(id, r);
    } else {
        handle_ext(a, method, id, params);
    }
}

int run_acp_mode(const app_opts_t *base) {
    acp_t a = {0};
    pthread_mutex_init(&a.mu, NULL);
    pthread_cond_init(&a.cv, NULL);
    a.base = base;
    if (isatty(0))
        fprintf(stderr, "omc acp: ACP server speaking JSON-RPC over stdio.\nThis command is meant to be spawned by an ACP client "
                        "(e.g. Zed's \"agent_servers\" config), not run directly.\nWaiting for protocol frames on stdin.\n");
    /* fd 1 is the JSON-RPC transport: anything else written to stdout goes to stderr (isolateProtocolStdout) */
    fflush(stdout);
    int pfd = dup(1);
    if (pfd >= 0) {
        g_proto_fd = pfd;
        dup2(2, 1);
    }
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
                if (json_is_obj(msg)) handle(&a, msg);
                json_free(msg);
            }
            sb_clear(&line);
        }
    }
    /* the client is gone: no response will arrive; cancel and dispose every session */
    pthread_mutex_lock(&a.mu);
    a.closed = true;
    pthread_cond_broadcast(&a.cv);
    pthread_mutex_unlock(&a.mu);
    while (a.sessions.n) close_record(&a, a.sessions.v[a.sessions.n - 1]);
    vec_free(&a.sessions);
    vec_free(&a.pending);
    json_free(a.client_caps);
    sb_free(&line);
    return 0;
}
