#define _GNU_SOURCE
#include "mcp.h"
#include "mcperr.h"

#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "net/http.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define MCP_PROTOCOL "2025-06-18"

typedef enum { T_STDIO, T_HTTP, T_SSE } transport_t;

/* lifecycle of a managed server */
typedef enum { S_CONNECTING, S_READY, S_FAILED } sstate_t;

typedef struct {
    int64_t id;
    json_t *result; /* owned response object */
    bool done;
} pending_t;

struct mcp_server {
    char *name;
    json_t *cfg;
    /* per-session tool views (MCPManager #scopedTools): a tool added while a call to this server ran is visible only
     * to the scopes listed here (tool name -> [scope]); a tool absent here is visible to every scope */
    json_t *scoped;
    int inflight_calls;
    int tool_refreshes;           /* tools/list refreshes announced and not finished */
    bool call_provoked;           /* the pending refresh answers a list_changed received during a call */
    pthread_mutex_t refresh_mu;   /* one tools/list refresh at a time: a later list holds what an earlier notice announced */
    transport_t transport;
    char *url;          /* http/sse */
    char *post_url;     /* sse: endpoint for POST */
    char *session_id;   /* streamable http Mcp-Session-Id */
    http_headers_t headers;
    child_t child;
    bool child_started;
    pthread_t reader;
    bool reader_started;
    volatile bool closing;
    volatile sig_atomic_t cancel; /* aborts HTTP requests and the SSE stream when closing */
    volatile int *ext_cancel;     /* probe: caller's cancel flag */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    VEC(pending_t *) pending;
    int64_t next_id;
    /* catalogs (guarded by mu once the server is shared) */
    json_t *tools;      /* tools/list result array */
    json_t *cached_tools; /* MCPToolCache hit when the startup window closed with the server still connecting:
                           * registered as deferred tools while it is not connected (NULL: none) */
    json_t *prompts;    /* prompts/list result array (servers advertising the prompts capability), or NULL */
    json_t *resources;  /* resources/list (servers advertising resources), or NULL */
    json_t *templates;  /* resources/templates/list, or NULL */
    json_t *caps;       /* initialize capabilities */
    json_t *info;       /* initialize serverInfo */
    json_t *subscribed; /* URIs subscribed with resources/subscribe, or NULL */
    char *instructions;
    char *error;
    char *eof_error; /* stdio EOF before a response: the subprocess exit status (StdioTransport) */
    char *eof_code;  /* its MCPTransportError code: the exit code or the signal name, NULL when still running */
    char *www_auth; /* last 401 challenge (OAuth discovery) */
    int http_status; /* last HTTP error status */
    bool no_oauth;   /* probe without stored OAuth tokens (config key "_noOAuth") */
    bool connected;  /* transport alive */
    sstate_t state;
    int64_t timeout_ms;
    int refs;
    mcp_manager_t *mgr; /* NULL for probes */
};

struct mcp_manager {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    VEC(mcp_server_t *) servers;
    mcp_callbacks_t cb;
    bool notifications;
    bool tool_cache; /* MCPToolCache enabled (mcp_set_tool_cache) */
    json_t *tool_scopes; /* per-session views opened so far (scope -> true) */
    int bg; /* background threads still using the manager */
    bool closing;
};

static void srv_ref(mcp_server_t *s) { __atomic_add_fetch(&s->refs, 1, __ATOMIC_SEQ_CST); }
static void server_free(mcp_server_t *s);
static void srv_unref(mcp_server_t *s) {
    if (s && __atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST) == 0) server_free(s);
}

/* ---------- config ---------- */

static char *expand_env(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s; *p; p++) {
        if (p[0] == '$' && p[1] == '{') {
            const char *e = strchr(p, '}');
            if (e) {
                char *name = xstrndup(p + 2, (size_t)(e - p - 2));
                char *def = strstr(name, ":-");
                const char *val;
                if (def) {
                    *def = 0;
                    val = env_get(name);
                    if (!val) val = def + 2;
                } else {
                    val = env_get(name);
                }
                sb_append(&b, val ? val : "");
                free(name);
                p = e;
                continue;
            }
        }
        sb_putc(&b, *p);
    }
    return sb_detach(&b);
}

static void expand_tree(json_t *j) {
    if (json_is_str(j)) {
        char *e = expand_env(j->u.str.s);
        free(j->u.str.s);
        j->u.str.s = e;
        j->u.str.len = strlen(e);
    } else if (json_is_arr(j)) {
        for (size_t i = 0; i < j->u.arr.n; i++) expand_tree(j->u.arr.items[i]);
    } else if (json_is_obj(j)) {
        for (size_t i = 0; i < j->u.obj.n; i++) expand_tree(j->u.obj.vals[i]);
    }
}

static void add_servers(json_t *out, json_t *disabled, const char *path, const char *map_path, const char *provider) {
    size_t n;
    char *s = fs_read(path, &n);
    if (!s) return;
    json_t *j = json_parse(s, n, NULL);
    free(s);
    if (!json_is_obj(j)) {
        json_free(j);
        return;
    }
    json_t *dis = json_get_arr(j, "disabledServers"), *x;
    if (disabled) json_arr_foreach(dis, i, x) if (json_is_str(x)) json_push_str(disabled, json_sv(x));
    json_t *map = json_path(j, map_path);
    if (!json_is_obj(map) && strcmp(map_path, "mcpServers") == 0) map = json_get_obj(j, "servers");
    const char *k;
    json_t *v;
    json_obj_foreach(map, i, k, v) {
        if (!json_is_obj(v) || json_get(out, k)) continue;
        json_t *c = json_clone(v);
        json_set_str(c, "_source", path);
        json_set_str(c, "_provider", provider);
        json_set(out, k, c);
    }
    json_free(j);
}

json_t *mcp_discover_config(const char *cwd) {
    json_t *out = json_obj();
    json_t *disabled = json_arr();
    bool project = settings_bool("mcp.enableProjectConfig", true);
    static const struct {
        const char *rel, *provider;
    } proj[] = {{".omc/mcp.json", "OMP"},         {".omp/mcp.json", "OMP"},         {".omc/.mcp.json", "OMP"},
                {".omp/.mcp.json", "OMP"},        {"mcp.json", "MCP Config"},      {".mcp.json", "MCP Config"},
                {".claude/mcp.json", "Claude Code"}, {".claude/.mcp.json", "Claude Code"}, {".cursor/mcp.json", "Cursor"}};
    for (size_t i = 0; project && i < ARRAY_LEN(proj); i++) {
        char *p = path_join(cwd, proj[i].rel);
        add_servers(out, NULL, p, "mcpServers", proj[i].provider);
        free(p);
    }
    if (project) {
        char *vs = path_join(cwd, ".vscode/mcp.json");
        add_servers(out, NULL, vs, "servers", "VS Code");
        free(vs);
    }
    /* the user mcp.json owns the disabledServers denylist */
    char *up = omc_agent_path("mcp.json");
    add_servers(out, disabled, up, "mcpServers", "OMP");
    free(up);
    char *up2 = omc_agent_path(".mcp.json");
    add_servers(out, NULL, up2, "mcpServers", "OMP");
    free(up2);
    /* foreign user-level sources are opt-in (settings enabledProviders: [claude, cursor] or "*") */
    json_t *ep = settings_get("enabledProviders"), *e;
    bool want_claude = false, want_cursor = false;
    json_arr_foreach(ep, i, e) {
        const char *v = json_sv(e);
        if (str_eq(v, "*") || str_eq(v, "all")) want_claude = want_cursor = true;
        if (str_eq(v, "claude")) want_claude = true;
        if (str_eq(v, "cursor")) want_cursor = true;
    }
    if (env_get("CLAUDE_CONFIG_DIR")) want_claude = true;
    if (want_cursor) {
        char *p = path_join(home_dir(), ".cursor/mcp.json");
        add_servers(out, NULL, p, "mcpServers", "Cursor");
        free(p);
    }
    if (want_claude) {
        char *p = path_join(home_dir(), ".claude/mcp.json");
        add_servers(out, NULL, p, "mcpServers", "Claude Code");
        free(p);
        /* ~/.claude.json: global mcpServers + per-project projects.<cwd>.mcpServers */
        char *cj = path_join(home_dir(), ".claude.json");
        size_t n;
        char *s = fs_read_limit(cj, 32u << 20, &n, NULL);
        if (s) {
            json_t *j = json_parse(s, n, NULL);
            json_t *projs = json_get_obj(j, "projects");
            json_t *pm = project ? json_get_obj(json_get_obj(projs, cwd), "mcpServers") : NULL;
            const char *k;
            json_t *v;
            json_obj_foreach(pm, i, k, v) if (!json_get(out, k) && json_is_obj(v)) {
                json_t *c = json_clone(v);
                json_set_str(c, "_source", cj);
                json_set_str(c, "_provider", "Claude Code");
                json_set(out, k, c);
            }
            json_obj_foreach(json_get_obj(j, "mcpServers"), i, k, v) if (!json_get(out, k) && json_is_obj(v)) {
                json_t *c = json_clone(v);
                json_set_str(c, "_source", cj);
                json_set_str(c, "_provider", "Claude Code");
                json_set(out, k, c);
            }
            json_free(j);
            free(s);
        }
        free(cj);
    }
    json_t *x;
    json_arr_foreach(disabled, i, x) json_del(out, json_sv(x));
    json_t *sd = settings_get("mcp.disabledServers");
    json_arr_foreach(sd, i, x) if (json_is_str(x)) json_del(out, json_sv(x));
    /* drop explicitly disabled */
    for (size_t i = json_len(out); i > 0; i--) {
        json_t *c = out->u.obj.vals[i - 1];
        if (json_is_bool(json_get(c, "enabled")) && !json_get_bool(c, "enabled", true)) json_del(out, out->u.obj.keys[i - 1]);
    }
    json_free(disabled);
    expand_tree(out);
    return out;
}

/* ---------- JSON-RPC plumbing ---------- */

static void handle_notification(mcp_server_t *s, const char *method, const json_t *params);

static int send_msg(mcp_server_t *s, const json_t *msg, char **err);

static void deliver(mcp_server_t *s, json_t *msg) {
    if (!json_is_obj(msg)) {
        json_free(msg);
        return;
    }
    json_t *idj = json_get(msg, "id");
    if (json_is_num(idj) && (json_get(msg, "result") || json_get(msg, "error"))) {
        int64_t id = idj->is_int ? idj->u.num.i : json_d2i(idj->u.num.d);
        pthread_mutex_lock(&s->mu);
        for (size_t i = 0; i < s->pending.n; i++) {
            if (s->pending.v[i]->id == id && !s->pending.v[i]->done) {
                s->pending.v[i]->result = msg;
                s->pending.v[i]->done = true;
                msg = NULL;
                pthread_cond_broadcast(&s->cv);
                break;
            }
        }
        pthread_mutex_unlock(&s->mu);
        json_free(msg);
        return;
    }
    const char *method = json_get_str(msg, "method");
    if (method && !idj) { /* server notification */
        handle_notification(s, method, json_get(msg, "params"));
        json_free(msg);
        return;
    }
    /* server -> client request (e.g. ping, roots/list): answer minimal */
    if (method && idj) {
        json_t *resp = json_obj();
        json_set_str(resp, "jsonrpc", "2.0");
        json_set(resp, "id", json_clone(idj));
        if (!strcmp(method, "ping")) json_set(resp, "result", json_obj());
        else if (!strcmp(method, "roots/list")) {
            json_t *r = json_obj();
            json_set(r, "roots", json_arr());
            json_set(resp, "result", r);
        } else {
            json_t *e = json_obj();
            json_set_int(e, "code", -32601);
            json_set_str(e, "message", "Method not found");
            json_set(resp, "error", e);
        }
        if (s->transport == T_STDIO) {
            char *werr = NULL; /* send_msg writes the whole line (a large id could exceed one pipe write) */
            if (send_msg(s, resp, &werr) != 0) LOGW("mcp %s: reply to %s failed: %s", s->name, method, werr ? werr : "?");
            free(werr);
        }
        json_free(resp);
    }
    json_free(msg);
}

/* Bun's signalCode: the signal's SIG* name */
static const char *sig_name(int sig) {
    static const struct {
        int n;
        const char *name;
    } T[] = {{SIGHUP, "SIGHUP"}, {SIGINT, "SIGINT"}, {SIGQUIT, "SIGQUIT"}, {SIGILL, "SIGILL"}, {SIGABRT, "SIGABRT"},
             {SIGBUS, "SIGBUS"}, {SIGFPE, "SIGFPE"}, {SIGKILL, "SIGKILL"}, {SIGSEGV, "SIGSEGV"}, {SIGPIPE, "SIGPIPE"},
             {SIGALRM, "SIGALRM"}, {SIGTERM, "SIGTERM"}, {SIGUSR1, "SIGUSR1"}, {SIGUSR2, "SIGUSR2"}};
    for (size_t i = 0; i < ARRAY_LEN(T); i++)
        if (T[i].n == sig) return T[i].name;
    return "SIGKILL";
}

static void *stdio_reader(void *p) {
    mcp_server_t *s = p;
    sb_t line = SB_INIT;
    char buf[65536];
    while (!s->closing) {
        struct pollfd pf = {s->child.out_fd, POLLIN, 0};
        int r = poll(&pf, 1, 200);
        if (r <= 0) continue;
        ssize_t n = read(s->child.out_fd, buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (line.len < (64u << 20)) sb_putc(&line, buf[i]);
                continue;
            }
            if (line.len) deliver(s, json_parse(line.s, line.len, NULL));
            sb_clear(&line);
        }
    }
    sb_free(&line);
    /* StdioTransport EOF (omp b45330e867): the exit status, peeked without reaping (the child is reaped at close),
     * waited for up to 100 ms; a process still alive keeps the closed-stdout wording */
    char *eof = NULL, *code = NULL;
    pid_t pid = s->child.pid;
    if (!s->closing && pid > 0) {
        siginfo_t si;
        int64_t until = mono_ms() + 100;
        for (;;) {
            memset(&si, 0, sizeof si);
            if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == pid) break;
            si.si_pid = 0;
            if (mono_ms() >= until) break;
            sleep_ms(10);
        }
        if (si.si_pid == pid && si.si_code == CLD_EXITED) {
            eof = xasprintf("MCP subprocess exited with code %d before responding", si.si_status);
            code = xasprintf("%d", si.si_status);
        } else if (si.si_pid == pid && (si.si_code == CLD_KILLED || si.si_code == CLD_DUMPED)) {
            eof = xasprintf("MCP subprocess was killed by %s before responding", sig_name(si.si_status));
            code = xstrdup(sig_name(si.si_status));
        } else eof = xstrdup("MCP subprocess closed stdout before responding");
    }
    pthread_mutex_lock(&s->mu);
    s->connected = false;
    if (eof) {
        free(s->eof_error);
        free(s->eof_code);
        s->eof_error = eof;
        s->eof_code = code;
    }
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

/* SSE delivery for streamable HTTP POST responses and legacy SSE */
typedef struct {
    mcp_server_t *s;
    sse_parser_t sse;
    sb_t body;
    bool is_sse;
    int status;
} post_ctx_t;

static int post_sse_cb(void *ud, const char *event, const char *data, size_t len) {
    post_ctx_t *c = ud;
    UNUSED(event);
    deliver(c->s, json_parse(data, len, NULL));
    return 0;
}

static int post_hdr(void *ud, int status, const http_headers_t *h) {
    post_ctx_t *c = ud;
    c->status = status;
    if (status == 401 || status == 403) {
        const char *w = http_headers_get(h, "WWW-Authenticate");
        pthread_mutex_lock(&c->s->mu);
        free(c->s->www_auth);
        c->s->www_auth = w ? xstrdup(w) : NULL;
        pthread_mutex_unlock(&c->s->mu);
    }
    const char *ct = http_headers_get(h, "Content-Type");
    c->is_sse = ct && str_istr(ct, "text/event-stream");
    const char *sid = http_headers_get(h, "Mcp-Session-Id");
    if (sid) {
        pthread_mutex_lock(&c->s->mu);
        free(c->s->session_id);
        c->s->session_id = xstrdup(sid);
        pthread_mutex_unlock(&c->s->mu);
    }
    return 0;
}

static int post_data(void *ud, const char *d, size_t n) {
    post_ctx_t *c = ud;
    if (c->is_sse && c->status < 300) return sse_feed(&c->sse, d, n);
    if (c->body.len < (64u << 20)) sb_appendn(&c->body, d, n);
    return 0;
}

/* Bearer token from a stored MCP OAuth credential unless the config sets Authorization itself */
static void add_oauth(mcp_server_t *s, http_headers_t *h) {
    if (s->no_oauth || http_headers_get(h, "Authorization")) return;
    char *tok = mcp_oauth_token(s->url);
    if (tok && *tok) {
        char *v = xasprintf("Bearer %s", tok);
        http_headers_set(h, "Authorization", v);
        free(v);
    }
    free(tok);
}

static int http_send(mcp_server_t *s, const json_t *msg, char **err) {
    char *body = json_dump(msg, 0);
    http_headers_t h = {0};
    for (size_t i = 0; i < s->headers.n; i++) http_headers_add(&h, s->headers.v[i].name, s->headers.v[i].value);
    add_oauth(s, &h);
    http_headers_set(&h, "Content-Type", "application/json");
    http_headers_set(&h, "Accept", "application/json, text/event-stream");
    pthread_mutex_lock(&s->mu);
    if (s->session_id) http_headers_set(&h, "Mcp-Session-Id", s->session_id);
    if (s->connected) http_headers_set(&h, "MCP-Protocol-Version", MCP_PROTOCOL);
    pthread_mutex_unlock(&s->mu);
    post_ctx_t c = {.s = s};
    sse_init(&c.sse, post_sse_cb, &c);
    const char *url = s->transport == T_SSE ? s->post_url : s->url;
    http_req_t req = {.method = "POST", .url = url, .headers = &h, .body = body, .body_len = strlen(body),
                      .timeout_ms = s->timeout_ms ? s->timeout_ms + 5000 : 0, .on_headers = post_hdr, .on_data = post_data,
                      .ud = &c, .max_redirects = 5, .cancel = &s->cancel};
    http_resp_t r;
    int rc = http_request(&req, &r);
    if (rc == 0 && c.is_sse) sse_finish(&c.sse);
    if (rc != 0) {
        *err = xstrdup(r.error ? r.error : "request failed");
    } else if (c.status == 401) {
        s->http_status = 401;
        *err = xasprintf("HTTP 401: the server requires OAuth authorization; run `omc mcp login %s`", s->name);
        rc = -1;
    } else if (c.status >= 400) {
        s->http_status = c.status;
        *err = xasprintf("HTTP %d: %.300s", c.status, sb_cstr(&c.body));
        rc = -1;
    } else if (!c.is_sse && c.body.len && s->transport == T_HTTP) {
        json_t *j = json_parse(c.body.s, c.body.len, NULL);
        if (json_is_arr(j)) {
            for (size_t i = 0; i < json_len(j); i++) deliver(s, json_clone(json_at(j, i)));
            json_free(j);
        } else {
            deliver(s, j);
        }
    }
    sse_free(&c.sse);
    sb_free(&c.body);
    http_resp_free(&r);
    http_headers_free(&h);
    free(body);
    return rc;
}

static int send_msg(mcp_server_t *s, const json_t *msg, char **err) {
    if (s->transport == T_STDIO) {
        char *line = json_dump(msg, 0);
        sb_t b = SB_INIT;
        sb_append(&b, line);
        sb_putc(&b, '\n');
        free(line);
        pthread_mutex_lock(&s->mu);
        size_t off = 0;
        int rc = 0;
        while (off < b.len) {
            ssize_t w = write(s->child.in_fd, b.s + off, b.len - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                rc = -1;
                *err = xasprintf("write to %s failed: %s", s->name, strerror(errno));
                break;
            }
            off += (size_t)w;
        }
        pthread_mutex_unlock(&s->mu);
        sb_free(&b);
        return rc;
    }
    return http_send(s, msg, err);
}

static const char *transport_kind(const mcp_server_t *s) { return s->transport == T_STDIO ? "stdio" : "http"; }

/* a failed send as normalizeMCPTransportError sees Bun's error */
static void fail_from_send(mcp_server_t *s, const char *err, mcp_fail_t *fail) {
    if (!fail) return;
    const char *e = err ? err : "request failed";
    if (strstr(e, "Connection refused")) /* Bun: code ConnectionRefused */
        mcp_fail_normalize(fail, transport_kind(s), "send", "Unable to connect. Is the computer able to access the url?", "ConnectionRefused");
    else mcp_fail_normalize(fail, transport_kind(s), s->transport == T_STDIO ? "send" : "receive", e, NULL);
}

static json_t *mcp_request_ex(mcp_server_t *s, const char *method, json_t *params, int64_t timeout_ms, char **err,
                              mcp_fail_t *fail);

json_t *mcp_request(mcp_server_t *s, const char *method, json_t *params, int64_t timeout_ms, char **err) {
    return mcp_request_ex(s, method, params, timeout_ms, err, NULL);
}

static json_t *mcp_request_ex(mcp_server_t *s, const char *method, json_t *params, int64_t timeout_ms, char **err,
                              mcp_fail_t *fail) {
    *err = NULL;
    pending_t *p = xcalloc(1, sizeof(*p));
    pthread_mutex_lock(&s->mu);
    p->id = ++s->next_id;
    vec_push(&s->pending, p);
    pthread_mutex_unlock(&s->mu);
    json_t *msg = json_obj();
    json_set_str(msg, "jsonrpc", "2.0");
    json_set_int(msg, "id", p->id);
    json_set_str(msg, "method", method);
    if (params) json_set(msg, "params", params);
    int rc = send_msg(s, msg, err);
    json_free(msg);
    if (rc != 0) fail_from_send(s, *err, fail);
    json_t *result = NULL;
    pthread_mutex_lock(&s->mu);
    if (rc == 0) {
        int64_t deadline = timeout_ms > 0 ? mono_ms() + timeout_ms : 0;
        /* streamable HTTP delivers the response inside the POST; stdio/SSE responses arrive on the reader */
        while (!p->done && s->transport != T_HTTP && s->connected && !s->closing && !(s->ext_cancel && *s->ext_cancel)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&s->cv, &s->mu, &ts);
            if (deadline && mono_ms() > deadline) break;
        }
        if (p->done) {
            json_t *e = json_get(p->result, "error");
            if (e) {
                *err = xasprintf("%s (code %lld)", json_get_str(e, "message") ? json_get_str(e, "message") : "error",
                                 (long long)json_get_int(e, "code", 0));
                mcp_fail_json_rpc(fail, transport_kind(s), e);
            } else result = json_take(p->result, "result");
        } else if (s->ext_cancel && *s->ext_cancel) {
            *err = xstrdup("Aborted");
        } else if (s->connected || s->transport == T_HTTP) {
            *err = xasprintf("%s: %s timed out", s->name, method);
            char *m = xasprintf("Request timeout after %lldms", (long long)timeout_ms);
            mcp_fail_set(fail, transport_kind(s), "receive", "timeout", false, m, NULL);
            free(m);
        } else {
            /* the stdio transport's EOF (with the subprocess exit status): the request was written, so it is never
             * replayed */
            *err = s->eof_error ? xstrdup(s->eof_error) : xasprintf("%s: server disconnected", s->name);
            mcp_fail_set(fail, "stdio", "receive", "eof", false,
                         s->eof_error ? s->eof_error : "MCP subprocess closed stdout before responding", s->eof_code);
        }
    }
    for (size_t i = 0; i < s->pending.n; i++) {
        if (s->pending.v[i] == p) {
            memmove(s->pending.v + i, s->pending.v + i + 1, (s->pending.n - i - 1) * sizeof(void *));
            s->pending.n--;
            break;
        }
    }
    pthread_mutex_unlock(&s->mu);
    json_free(p->result);
    free(p);
    return result;
}

static void notify(mcp_server_t *s, const char *method) {
    json_t *msg = json_obj();
    json_set_str(msg, "jsonrpc", "2.0");
    json_set_str(msg, "method", method);
    char *err = NULL;
    send_msg(s, msg, &err);
    free(err);
    json_free(msg);
}

/* legacy SSE: background GET stream; first "endpoint" event gives the POST url */
typedef struct {
    mcp_server_t *s;
    sse_parser_t sse;
} sse_bg_t;

static int sse_bg_event(void *ud, const char *event, const char *data, size_t len) {
    sse_bg_t *c = ud;
    if (!strcmp(event, "endpoint")) {
        char *ep = xstrndup(data, len);
        char *abs = url_resolve(c->s->url, ep);
        pthread_mutex_lock(&c->s->mu);
        free(c->s->post_url);
        c->s->post_url = abs;
        pthread_cond_broadcast(&c->s->cv);
        pthread_mutex_unlock(&c->s->mu);
        free(ep);
    } else {
        deliver(c->s, json_parse(data, len, NULL));
    }
    return c->s->closing ? 1 : 0;
}

static int sse_bg_data(void *ud, const char *d, size_t n) { return sse_feed(&((sse_bg_t *)ud)->sse, d, n); }

static void *sse_reader(void *p) {
    mcp_server_t *s = p;
    sse_bg_t c = {.s = s};
    sse_init(&c.sse, sse_bg_event, &c);
    http_headers_t h = {0};
    for (size_t i = 0; i < s->headers.n; i++) http_headers_add(&h, s->headers.v[i].name, s->headers.v[i].value);
    http_headers_set(&h, "Accept", "text/event-stream");
    add_oauth(s, &h);
    http_req_t req = {.method = "GET", .url = s->url, .headers = &h, .on_data = sse_bg_data, .ud = &c,
                      .idle_timeout_ms = 24 * 3600 * 1000LL, .cancel = &s->cancel};
    http_resp_t r;
    http_request(&req, &r);
    http_resp_free(&r);
    http_headers_free(&h);
    sse_free(&c.sse);
    pthread_mutex_lock(&s->mu);
    s->connected = false;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

/* ---------- server lifecycle ---------- */

static mcp_server_t *server_new(const char *name, const json_t *cfg) {
    mcp_server_t *s = xcalloc(1, sizeof(*s));
    s->name = xstrdup(name);
    s->cfg = json_clone(cfg);
    s->refs = 1;
    s->state = S_CONNECTING;
    pthread_mutex_init(&s->mu, NULL);
    pthread_mutex_init(&s->refresh_mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->child.in_fd = s->child.out_fd = s->child.err_fd = -1;
    const char *type = json_get_str(cfg, "type");
    const char *url = json_get_str(cfg, "url");
    if (!url) url = json_get_str(cfg, "serverUrl");
    if (str_eq(type, "sse")) s->transport = T_SSE;
    else if (str_eq(type, "http") || str_eq(type, "streamable-http") || (!json_get_str(cfg, "command") && url)) s->transport = T_HTTP;
    else s->transport = T_STDIO;
    if (url) s->url = xstrdup(url);
    s->no_oauth = json_get_bool(cfg, "_noOAuth", false);
    const char *k;
    json_t *v;
    json_obj_foreach(json_get_obj(cfg, "headers"), i, k, v) if (json_is_str(v)) http_headers_add(&s->headers, k, json_sv(v));
    const char *env_to = env_get("OMP_MCP_TIMEOUT_MS");
    if (!env_to) env_to = env_get("OMC_MCP_TIMEOUT_MS");
    s->timeout_ms = env_to ? strtoll(env_to, NULL, 10) : json_get_int(cfg, "timeout", 30000);
    return s;
}

/* stop the transport: pending requests fail, readers end */
static void server_close(mcp_server_t *s) {
    s->closing = true;
    s->cancel = 1;
    pthread_mutex_lock(&s->mu);
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    if (s->child_started) child_kill(&s->child, SIGTERM);
}

static void server_free(mcp_server_t *s) {
    server_close(s);
    if (s->child_started) child_close(&s->child);
    if (s->reader_started) pthread_join(s->reader, NULL);
    free(s->eof_error);
    free(s->eof_code);
    json_free(s->cfg);
    json_free(s->tools);
    json_free(s->cached_tools);
    json_free(s->prompts);
    json_free(s->resources);
    json_free(s->templates);
    json_free(s->caps);
    json_free(s->info);
    json_free(s->subscribed);
    http_headers_free(&s->headers);
    free(s->name);
    free(s->url);
    free(s->post_url);
    free(s->session_id);
    free(s->instructions);
    free(s->www_auth);
    free(s->error);
    vec_free(&s->pending);
    pthread_mutex_destroy(&s->mu);
    pthread_mutex_destroy(&s->refresh_mu);
    json_free(s->scoped);
    pthread_cond_destroy(&s->cv);
    free(s);
}

static json_t *list_all(mcp_server_t *s, const char *method, const char *key, char **err_out);

/* serverSupportsResources / serverSupportsPrompts */
static bool cap(mcp_server_t *s, const char *name) { return json_get(s->caps, name) != NULL; }

static void subscribe_uris(mcp_server_t *s, const json_t *uris, bool on) {
    json_t *u;
    json_arr_foreach(uris, i, u) {
        if (!json_is_str(u)) continue;
        json_t *p = json_obj();
        json_set_str(p, "uri", json_sv(u));
        char *err = NULL;
        json_t *r = mcp_request(s, on ? "resources/subscribe" : "resources/unsubscribe", p, s->timeout_ms, &err);
        json_free(r);
        free(err);
    }
}

static json_t *resource_uris(const json_t *resources) {
    json_t *uris = json_arr(), *r;
    json_arr_foreach(resources, i, r) if (json_get_str(r, "uri")) json_push_str(uris, json_get_str(r, "uri"));
    return uris;
}

static int server_connect(mcp_server_t *s) {
    char *err = NULL;
    if (s->transport == T_STDIO) {
        const char *cmd = json_get_str(s->cfg, "command");
        if (!cmd) {
            s->error = xstrdup("missing command");
            return -1;
        }
        strvec_t argv = {0};
        strvec_push(&argv, cmd);
        json_t *a;
        json_arr_foreach(json_get_arr(s->cfg, "args"), i, a) if (json_is_str(a)) strvec_push(&argv, json_sv(a));
        vec_push(&argv, NULL);
        strvec_t envv = {0};
        const char *k;
        json_t *v;
        json_obj_foreach(json_get_obj(s->cfg, "env"), i, k, v) if (json_is_str(v)) strvec_push_own(&envv, xasprintf("%s=%s", k, json_sv(v)));
        vec_push(&envv, NULL);
        const char *cwd = json_get_str(s->cfg, "cwd");
        int rc = child_spawn((const char *const *)argv.v, cwd, (const char *const *)envv.v, false, &s->child);
        argv.n--;
        envv.n--;
        strvec_free(&argv);
        strvec_free(&envv);
        if (rc != 0) {
            /* the runtime's spawn error, as omp shows it */
            if (errno == ENOENT) s->error = xasprintf("ENOENT: no such file or directory, posix_spawn '%s'", cmd);
            else if (errno == EACCES) s->error = xasprintf("EACCES: permission denied, posix_spawn '%s'", cmd);
            else s->error = xasprintf("failed to start %s: %s", cmd, strerror(errno));
            return -1;
        }
        s->child_started = true;
        s->connected = true;
        if (pthread_create(&s->reader, NULL, stdio_reader, s) == 0) s->reader_started = true;
    } else if (s->transport == T_SSE) {
        s->connected = true;
        if (pthread_create(&s->reader, NULL, sse_reader, s) == 0) s->reader_started = true;
        pthread_mutex_lock(&s->mu);
        int64_t deadline = mono_ms() + s->timeout_ms;
        while (!s->post_url && s->connected && !s->closing && mono_ms() < deadline &&
               !(s->ext_cancel && *s->ext_cancel)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&s->cv, &s->mu, &ts);
        }
        bool ok = s->post_url != NULL;
        pthread_mutex_unlock(&s->mu);
        if (!ok) {
            s->error = xstrdup("SSE endpoint event not received");
            return -1;
        }
    }
    json_t *params = json_obj();
    json_set_str(params, "protocolVersion", MCP_PROTOCOL);
    json_t *caps = json_obj();
    json_set(caps, "roots", json_parse_str("{\"listChanged\":false}"));
    json_set(params, "capabilities", caps);
    json_t *ci = json_obj();
    json_set_str(ci, "name", "omc");
    json_set_str(ci, "version", OMC_VERSION);
    json_set(params, "clientInfo", ci);
    json_t *init = mcp_request(s, "initialize", params, s->timeout_ms, &err);
    if (!init) {
        s->error = err ? err : xstrdup("initialize failed");
        return -1;
    }
    char *instr = json_get_str(init, "instructions") && json_get_bool(s->cfg, "instructions", true)
                      ? xstrdup(json_get_str(init, "instructions"))
                      : NULL;
    json_t *scaps = json_get_obj(init, "capabilities") ? json_clone(json_get_obj(init, "capabilities")) : json_obj();
    json_t *info = json_get_obj(init, "serverInfo") ? json_clone(json_get_obj(init, "serverInfo")) : json_obj();
    json_free(init);
    pthread_mutex_lock(&s->mu);
    s->instructions = instr;
    s->caps = scaps;
    s->info = info;
    s->connected = true;
    pthread_mutex_unlock(&s->mu);
    notify(s, "notifications/initialized");
    /* list tools (paginated) */
    json_t *tools = list_all(s, "tools/list", "tools", &err);
    if (!tools) {
        s->error = err ? err : xstrdup("tools/list failed");
        return -1;
    }
    pthread_mutex_lock(&s->mu);
    s->tools = tools;
    pthread_mutex_unlock(&s->mu);
    /* #loadServerResourcesAndPrompts: best-effort resource / prompt catalogs (MCP prompts become slash commands) */
    if (cap(s, "resources")) {
        json_t *res = list_all(s, "resources/list", "resources", NULL);
        json_t *tpl = list_all(s, "resources/templates/list", "resourceTemplates", NULL);
        pthread_mutex_lock(&s->mu);
        s->resources = res ? res : json_arr();
        s->templates = tpl ? tpl : json_arr();
        pthread_mutex_unlock(&s->mu);
    }
    if (cap(s, "prompts")) {
        json_t *pr = list_all(s, "prompts/list", "prompts", NULL);
        pthread_mutex_lock(&s->mu);
        s->prompts = pr ? pr : json_arr();
        pthread_mutex_unlock(&s->mu);
    }
    return 0;
}

/* ---------- manager ---------- */

/* the most recently created live manager serves mcp:// reads from tools */
static pthread_mutex_t g_default_mu = PTHREAD_MUTEX_INITIALIZER;
static mcp_manager_t *g_default;

mcp_manager_t *mcp_manager_new(void) {
    mcp_manager_t *m = xcalloc(1, sizeof(mcp_manager_t));
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);
    pthread_mutex_lock(&g_default_mu);
    g_default = m;
    pthread_mutex_unlock(&g_default_mu);
    return m;
}

static void bg_enter(mcp_manager_t *m) {
    pthread_mutex_lock(&m->mu);
    m->bg++;
    pthread_mutex_unlock(&m->mu);
}

static void bg_leave(mcp_manager_t *m) {
    pthread_mutex_lock(&m->mu);
    m->bg--;
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mu);
}

void mcp_manager_free(mcp_manager_t *m) {
    if (!m) return;
    pthread_mutex_lock(&g_default_mu);
    if (g_default == m) g_default = NULL;
    pthread_mutex_unlock(&g_default_mu);
    pthread_mutex_lock(&m->mu);
    m->closing = true;
    memset(&m->cb, 0, sizeof m->cb);
    for (size_t i = 0; i < m->servers.n; i++) server_close(m->servers.v[i]);
    /* background connects / refreshes notice the closing transports; give them a moment to finish */
    int64_t deadline = mono_ms() + 3000;
    while (m->bg > 0 && mono_ms() < deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&m->cv, &m->mu, &ts);
    }
    bool busy = m->bg > 0;
    pthread_mutex_unlock(&m->mu);
    for (size_t i = 0; i < m->servers.n; i++) srv_unref(m->servers.v[i]);
    vec_free(&m->servers);
    if (busy) return; /* a stuck thread still references the manager: leak it rather than free under it */
    pthread_mutex_destroy(&m->mu);
    pthread_cond_destroy(&m->cv);
    free(m);
}

void mcp_set_callbacks(mcp_manager_t *m, const mcp_callbacks_t *cb) {
    if (!m) return;
    pthread_mutex_lock(&m->mu);
    if (cb) m->cb = *cb;
    else memset(&m->cb, 0, sizeof m->cb);
    pthread_mutex_unlock(&m->mu);
}

size_t mcp_server_count(mcp_manager_t *m) {
    if (!m) return 0;
    pthread_mutex_lock(&m->mu);
    size_t n = m->servers.n;
    pthread_mutex_unlock(&m->mu);
    return n;
}

/* a referenced snapshot of the servers (release with snap_free) */
typedef VEC(mcp_server_t *) srv_vec_t;
static void snapshot(mcp_manager_t *m, srv_vec_t *out) {
    memset(out, 0, sizeof *out);
    if (!m) return;
    pthread_mutex_lock(&m->mu);
    for (size_t i = 0; i < m->servers.n; i++) {
        srv_ref(m->servers.v[i]);
        vec_push(out, m->servers.v[i]);
    }
    pthread_mutex_unlock(&m->mu);
}
static void snap_free(srv_vec_t *v) {
    for (size_t i = 0; i < v->n; i++) srv_unref(v->v[i]);
    vec_free(v);
}

static mcp_server_t *find_ref(mcp_manager_t *m, const char *name) {
    if (!m || !name) return NULL;
    mcp_server_t *s = NULL;
    pthread_mutex_lock(&m->mu);
    for (size_t i = 0; i < m->servers.n && !s; i++)
        if (!strcmp(m->servers.v[i]->name, name)) {
            s = m->servers.v[i];
            srv_ref(s);
        }
    pthread_mutex_unlock(&m->mu);
    return s;
}

static sstate_t sstate(mcp_server_t *s) {
    pthread_mutex_lock(&s->mu);
    sstate_t st = s->state;
    pthread_mutex_unlock(&s->mu);
    return st;
}

static bool ready(mcp_server_t *s) { return sstate(s) == S_READY && s->connected && !s->closing; }

static void fire_tools_changed(mcp_server_t *s) {
    mcp_manager_t *m = s->mgr;
    if (!m) return;
    pthread_mutex_lock(&m->mu);
    mcp_callbacks_t cb = m->cb;
    bool live = !m->closing && !s->closing;
    pthread_mutex_unlock(&m->mu);
    if (live && cb.tools_changed) cb.tools_changed(cb.ud, s->name);
}

static bool cache_on(mcp_manager_t *m) {
    if (!m) return false;
    pthread_mutex_lock(&m->mu);
    bool on = m->tool_cache;
    pthread_mutex_unlock(&m->mu);
    return on;
}

/* toolCache.set after a successful tools/list */
static void cache_tools(mcp_server_t *s) {
    if (!cache_on(s->mgr)) return;
    pthread_mutex_lock(&s->mu);
    json_t *tools = json_clone(s->tools);
    pthread_mutex_unlock(&s->mu);
    mcp_tool_cache_set(s->name, s->cfg, tools);
    json_free(tools);
}

void mcp_set_tool_cache(mcp_manager_t *m, bool on) {
    if (!m) return;
    pthread_mutex_lock(&m->mu);
    m->tool_cache = on;
    pthread_mutex_unlock(&m->mu);
}

typedef struct {
    mcp_server_t *s;
    pthread_t th;
    volatile bool done;
    bool waited; /* the caller still waits for it (no late tools_changed) */
} conn_job_t;

static void *conn_thread(void *p) {
    conn_job_t *j = p;
    mcp_server_t *s = j->s;
    int rc = server_connect(s);
    if (rc != 0) LOGW("mcp %s: %s", s->name, s->error ? s->error : "failed");
    if (rc == 0) cache_tools(s);
    pthread_mutex_lock(&s->mu);
    s->state = rc == 0 ? S_READY : S_FAILED;
    pthread_cond_broadcast(&s->cv); /* deferred tool calls wait for the connection (waitForConnection) */
    pthread_mutex_unlock(&s->mu);
    /* mcp.notifications: subscribe to the new server's resources */
    if (rc == 0 && s->mgr) {
        pthread_mutex_lock(&s->mgr->mu);
        bool want = s->mgr->notifications;
        pthread_mutex_unlock(&s->mgr->mu);
        json_t *sub = json_get_obj(s->caps, "resources");
        if (want && json_get_bool(sub, "subscribe", false) && s->resources) {
            json_t *uris = resource_uris(s->resources);
            subscribe_uris(s, uris, true);
            pthread_mutex_lock(&s->mu);
            json_free(s->subscribed);
            s->subscribed = uris;
            pthread_mutex_unlock(&s->mu);
        }
    }
    mcp_manager_t *m = s->mgr;
    bool late;
    pthread_mutex_lock(&m->mu);
    late = !j->waited;
    j->done = true;
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mu);
    if (late) {
        /* nobody waits any more: the job is ours; announce a late connection */
        if (rc == 0) {
            pthread_mutex_lock(&m->mu);
            mcp_callbacks_t cb = m->cb;
            bool live = !m->closing && !s->closing;
            pthread_mutex_unlock(&m->mu);
            if (live && cb.connected) cb.connected(cb.ud, s->name);
            else fire_tools_changed(s);
        }
        srv_unref(s);
        free(j);
        bg_leave(m);
    }
    return NULL;
}

/* resolveMCPStartupTimeoutMs: the non-blocking discovery window for the initial connections. Startup waits at
 * most this long; servers still connecting finish in the background and announce their tools (omp issue #2100:
 * one unresponsive server used to block startup for the whole request timeout). 0 waits until all settle. */
int64_t mcp_startup_timeout_ms(void) {
    const char *raw = env_get("OMP_MCP_STARTUP_TIMEOUT_MS");
    if (!raw) raw = env_get("OMC_MCP_STARTUP_TIMEOUT_MS");
    if (raw && *raw) {
        char *end;
        double v = strtod(raw, &end);
        if (end != raw && !*end && v >= 0) return (int64_t)v;
        LOGW("Ignoring invalid OMP_MCP_STARTUP_TIMEOUT_MS env value; expected a non-negative number");
    }
    int64_t v = settings_int("mcp.startupTimeoutMs", 250);
    return v < 0 ? 250 : v;
}

/* resolveMCPTimeoutMs: OMP_MCP_TIMEOUT_MS, else 30 s */
int64_t mcp_timeout_ms(void) {
    const char *raw = env_get("OMP_MCP_TIMEOUT_MS");
    if (!raw) raw = env_get("OMC_MCP_TIMEOUT_MS");
    if (raw && *raw) {
        char *end;
        double v = strtod(raw, &end);
        if (end != raw && !*end && v >= 0) return (int64_t)v;
        LOGW("Ignoring invalid OMP_MCP_TIMEOUT_MS env value; expected a non-negative number");
    }
    return 30000;
}

void mcp_wait_startup(mcp_manager_t *m, int64_t timeout_ms) {
    if (!m) return;
    int64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        srv_vec_t v;
        snapshot(m, &v);
        bool pending = false;
        for (size_t i = 0; i < v.n && !pending; i++) pending = sstate(v.v[i]) == S_CONNECTING;
        snap_free(&v);
        if (!pending || (timeout_ms > 0 && mono_ms() >= deadline)) return;
        usleep(20000);
    }
}

/* start connecting `s` (already in the manager); waits up to timeout_ms (0 = until every job settles). A job
 * still running after the wait finishes on its own. */
static void connect_wait(mcp_manager_t *m, mcp_server_t **srv, size_t n, int64_t timeout_ms) {
    conn_job_t **jobs = xcalloc(n ? n : 1, sizeof(conn_job_t *));
    for (size_t i = 0; i < n; i++) {
        jobs[i] = xcalloc(1, sizeof(conn_job_t));
        jobs[i]->s = srv[i];
        jobs[i]->waited = true;
        srv_ref(srv[i]);
        bg_enter(m);
        if (pthread_create(&jobs[i]->th, NULL, conn_thread, jobs[i]) != 0) {
            pthread_mutex_lock(&srv[i]->mu);
            srv[i]->state = S_FAILED;
            if (!srv[i]->error) srv[i]->error = xstrdup("cannot start connection thread");
            pthread_mutex_unlock(&srv[i]->mu);
            jobs[i]->done = true;
            jobs[i]->th = 0;
        }
    }
    int64_t deadline = mono_ms() + timeout_ms;
    pthread_mutex_lock(&m->mu);
    for (;;) {
        bool all = true;
        for (size_t i = 0; i < n; i++) all = all && jobs[i]->done;
        if (all || (timeout_ms > 0 && mono_ms() >= deadline)) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&m->cv, &m->mu, &ts);
    }
    bool *finished = xcalloc(n ? n : 1, sizeof(bool));
    for (size_t i = 0; i < n; i++) {
        finished[i] = jobs[i]->done;
        if (!finished[i]) jobs[i]->waited = false; /* the thread owns the job now */
    }
    pthread_mutex_unlock(&m->mu);
    /* connectServers: a server still pending when the window closes gets its cached tools as DeferredMCPTools */
    bool use_cache = cache_on(m);
    for (size_t i = 0; i < n && use_cache; i++) {
        if (finished[i]) continue;
        json_t *cached = mcp_tool_cache_get(srv[i]->name, srv[i]->cfg);
        if (!cached) continue;
        pthread_mutex_lock(&srv[i]->mu);
        json_free(srv[i]->cached_tools);
        srv[i]->cached_tools = cached;
        pthread_mutex_unlock(&srv[i]->mu);
    }
    for (size_t i = 0; i < n; i++) {
        if (!finished[i]) {
            pthread_detach(jobs[i]->th);
            continue;
        }
        if (jobs[i]->th) pthread_join(jobs[i]->th, NULL);
        srv_unref(jobs[i]->s);
        free(jobs[i]);
        bg_leave(m);
    }
    free(finished);
    free(jobs);
}

static void remove_server_locked(mcp_manager_t *m, size_t i) {
    memmove(m->servers.v + i, m->servers.v + i + 1, (m->servers.n - i - 1) * sizeof(void *));
    m->servers.n--;
}

void mcp_connect_all(mcp_manager_t *m, const json_t *config, int64_t timeout_ms) {
    size_t n = json_len(config);
    if (!n || !m) return;
    mcp_server_t **srv = xcalloc(n, sizeof(mcp_server_t *));
    srv_vec_t old = {0};
    pthread_mutex_lock(&m->mu);
    for (size_t i = 0; i < n; i++) {
        const char *name = config->u.obj.keys[i];
        srv[i] = server_new(name, config->u.obj.vals[i]);
        srv[i]->mgr = m;
        for (size_t k = 0; k < m->servers.n; k++)
            if (!strcmp(m->servers.v[k]->name, name)) {
                mcp_server_t *o = m->servers.v[k];
                server_close(o);
                /* deferred tools stay registered until live ones replace them (#replaceServerTools) */
                pthread_mutex_lock(&o->mu);
                if (o->cached_tools && json_equal(o->cfg, srv[i]->cfg)) srv[i]->cached_tools = json_clone(o->cached_tools);
                pthread_mutex_unlock(&o->mu);
                vec_push(&old, o);
                remove_server_locked(m, k);
                break;
            }
        vec_push(&m->servers, srv[i]);
    }
    pthread_mutex_unlock(&m->mu);
    snap_free(&old); /* outside the lock: freeing joins reader threads that may take it */
    connect_wait(m, srv, n, timeout_ms);
    free(srv);
}

mcp_state_t mcp_connect_server(mcp_manager_t *m, const char *name, const json_t *cfg, int64_t timeout_ms) {
    if (!m || !name || !cfg) return MCP_DISCONNECTED;
    json_t *one = json_obj();
    json_set(one, name, json_clone(cfg));
    mcp_connect_all(m, one, timeout_ms);
    json_free(one);
    return mcp_server_state(m, name);
}

mcp_state_t mcp_reconnect_server(mcp_manager_t *m, const char *name, int64_t timeout_ms, char **err) {
    if (err) *err = NULL;
    mcp_server_t *s = find_ref(m, name);
    if (!s) {
        if (err) *err = xasprintf("Server \"%s\" not found", name);
        return MCP_DISCONNECTED;
    }
    json_t *cfg = json_clone(s->cfg);
    srv_unref(s);
    mcp_state_t st = mcp_connect_server(m, name, cfg, timeout_ms);
    json_free(cfg);
    if (st == MCP_DISCONNECTED && err) {
        mcp_server_t *n = find_ref(m, name);
        if (n) {
            pthread_mutex_lock(&n->mu);
            *err = n->error ? xstrdup(n->error) : NULL;
            pthread_mutex_unlock(&n->mu);
            srv_unref(n);
        }
    }
    return st;
}

bool mcp_disconnect_server(mcp_manager_t *m, const char *name) {
    if (!m || !name) return false;
    mcp_server_t *s = NULL;
    pthread_mutex_lock(&m->mu);
    for (size_t i = 0; i < m->servers.n; i++)
        if (!strcmp(m->servers.v[i]->name, name)) {
            s = m->servers.v[i];
            remove_server_locked(m, i);
            break;
        }
    pthread_mutex_unlock(&m->mu);
    if (!s) return false;
    server_close(s);
    srv_unref(s);
    return true;
}

void mcp_disconnect_all(mcp_manager_t *m) {
    if (!m) return;
    pthread_mutex_lock(&m->mu);
    srv_vec_t all = {0};
    for (size_t i = 0; i < m->servers.n; i++) vec_push(&all, m->servers.v[i]);
    m->servers.n = 0;
    pthread_mutex_unlock(&m->mu);
    for (size_t i = 0; i < all.n; i++) {
        server_close(all.v[i]);
        srv_unref(all.v[i]);
    }
    vec_free(&all);
}

mcp_state_t mcp_server_state(mcp_manager_t *m, const char *name) {
    mcp_server_t *s = find_ref(m, name);
    if (!s) return MCP_DISCONNECTED;
    sstate_t st = sstate(s);
    mcp_state_t r = st == S_CONNECTING ? MCP_CONNECTING : st == S_READY && s->connected ? MCP_CONNECTED : MCP_DISCONNECTED;
    srv_unref(s);
    return r;
}

bool mcp_has_server(mcp_manager_t *m, const char *name) {
    mcp_server_t *s = find_ref(m, name);
    srv_unref(s);
    return s != NULL;
}

json_t *mcp_server_names(mcp_manager_t *m) {
    json_t *out = json_arr();
    if (!m) return out;
    pthread_mutex_lock(&m->mu);
    for (size_t i = 0; i < m->servers.n; i++) json_push_str(out, m->servers.v[i]->name);
    pthread_mutex_unlock(&m->mu);
    return out;
}

json_t *mcp_connected_names(mcp_manager_t *m) {
    json_t *out = json_arr();
    srv_vec_t v;
    snapshot(m, &v);
    for (size_t i = 0; i < v.n; i++)
        if (ready(v.v[i])) json_push_str(out, v.v[i]->name);
    snap_free(&v);
    return out;
}

json_t *mcp_server_config(mcp_manager_t *m, const char *name) {
    mcp_server_t *s = find_ref(m, name);
    json_t *c = s ? json_clone(s->cfg) : NULL;
    srv_unref(s);
    return c;
}

/* clone a catalog field of a connected server under its lock */
static json_t *catalog(mcp_manager_t *m, const char *name, size_t off, bool empty_when_null) {
    mcp_server_t *s = find_ref(m, name);
    if (!s) return NULL;
    json_t *out = NULL;
    if (ready(s)) {
        pthread_mutex_lock(&s->mu);
        json_t *f = *(json_t **)((char *)s + off);
        out = f ? json_clone(f) : empty_when_null ? json_arr() : NULL;
        pthread_mutex_unlock(&s->mu);
    }
    srv_unref(s);
    return out;
}

json_t *mcp_server_tools(mcp_manager_t *m, const char *name) { return catalog(m, name, offsetof(mcp_server_t, tools), true); }
json_t *mcp_server_prompts(mcp_manager_t *m, const char *name) { return catalog(m, name, offsetof(mcp_server_t, prompts), true); }
json_t *mcp_server_capabilities(mcp_manager_t *m, const char *name) {
    return catalog(m, name, offsetof(mcp_server_t, caps), false);
}

json_t *mcp_server_resources(mcp_manager_t *m, const char *name) {
    mcp_server_t *s = find_ref(m, name);
    if (!s) return NULL;
    json_t *out = NULL;
    if (ready(s)) {
        out = json_obj();
        pthread_mutex_lock(&s->mu);
        json_set(out, "resources", s->resources ? json_clone(s->resources) : json_arr());
        json_set(out, "templates", s->templates ? json_clone(s->templates) : json_arr());
        pthread_mutex_unlock(&s->mu);
    }
    srv_unref(s);
    return out;
}

json_t *mcp_subscriptions(mcp_manager_t *m, const char *name) {
    mcp_server_t *s = find_ref(m, name);
    if (!s) return NULL;
    pthread_mutex_lock(&s->mu);
    json_t *out = s->subscribed && json_len(s->subscribed) ? json_clone(s->subscribed) : NULL;
    pthread_mutex_unlock(&s->mu);
    srv_unref(s);
    return out;
}

bool mcp_notifications_enabled(mcp_manager_t *m) {
    if (!m) return false;
    pthread_mutex_lock(&m->mu);
    bool on = m->notifications;
    pthread_mutex_unlock(&m->mu);
    return on;
}

typedef struct {
    mcp_server_t *s;
    bool on;
    json_t *uris;
} sub_job_t;

static void *sub_thread(void *p) {
    sub_job_t *j = p;
    mcp_server_t *s = j->s;
    subscribe_uris(s, j->uris, j->on);
    pthread_mutex_lock(&s->mu);
    json_free(s->subscribed);
    s->subscribed = j->on ? j->uris : NULL;
    pthread_mutex_unlock(&s->mu);
    if (!j->on) json_free(j->uris);
    mcp_manager_t *m = s->mgr;
    srv_unref(s);
    free(j);
    bg_leave(m);
    return NULL;
}

void mcp_set_notifications(mcp_manager_t *m, bool enabled) {
    if (!m) return;
    pthread_mutex_lock(&m->mu);
    bool was = m->notifications;
    m->notifications = enabled;
    pthread_mutex_unlock(&m->mu);
    if (was == enabled) return;
    srv_vec_t v;
    snapshot(m, &v);
    for (size_t i = 0; i < v.n; i++) {
        mcp_server_t *s = v.v[i];
        if (!ready(s)) continue;
        json_t *uris = NULL;
        pthread_mutex_lock(&s->mu);
        if (enabled && json_get_bool(json_get_obj(s->caps, "resources"), "subscribe", false) && s->resources)
            uris = resource_uris(s->resources);
        else if (!enabled && s->subscribed)
            uris = json_clone(s->subscribed);
        if (!enabled) {
            json_free(s->subscribed);
            s->subscribed = NULL;
        }
        pthread_mutex_unlock(&s->mu);
        if (!uris) continue;
        sub_job_t *j = xcalloc(1, sizeof *j);
        j->s = s;
        j->on = enabled;
        j->uris = uris;
        srv_ref(s);
        bg_enter(m);
        pthread_t th;
        if (pthread_create(&th, NULL, sub_thread, j) == 0) pthread_detach(th);
        else sub_thread(j);
    }
    snap_free(&v);
}

/* ---------- server notifications (#handleServerNotification) ---------- */

typedef struct {
    mcp_server_t *s;
    char *kind; /* tools / prompts / resources */
} refresh_job_t;

static const char *scope_key(const char *scope) { return scope ? scope : "owner"; }

static bool scope_listed(const json_t *scopes, const char *key) {
    const json_t *x;
    json_arr_foreach(scopes, i, x) if (str_eq(json_sv(x), key)) return true;
    return false;
}

/* #scopeAddedTools: tools the server does not serve yet start visible to no scope */
static void scope_added_tools_locked(mcp_server_t *s, const json_t *tools) {
    const json_t *t, *old;
    json_arr_foreach(tools, i, t) {
        const char *name = json_get_str(t, "name");
        bool known = !name;
        json_arr_foreach(s->tools, k, old) known = known || str_eq(json_get_str(old, "name"), name);
        if (known) continue;
        if (!s->scoped) s->scoped = json_obj();
        json_set(s->scoped, name, json_arr());
    }
}

/* #replaceServerTools: a scoped tool the server stopped serving is forgotten */
static void prune_scoped_locked(mcp_server_t *s, const json_t *tools) {
    for (size_t i = 0; s->scoped && i < s->scoped->u.obj.n;) {
        const char *name = s->scoped->u.obj.keys[i];
        bool present = false;
        const json_t *t;
        json_arr_foreach(tools, k, t) present = present || str_eq(json_get_str(t, "name"), name);
        if (present) i++;
        else json_del(s->scoped, name);
    }
}

static void *refresh_thread(void *p) {
    refresh_job_t *j = p;
    mcp_server_t *s = j->s;
    mcp_manager_t *m = s->mgr;
    if (!strcmp(j->kind, "tools")) {
        pthread_mutex_lock(&s->refresh_mu);
        /* claimed before the request: only a list requested after a call's notice can hold what that call added */
        pthread_mutex_lock(&s->mu);
        bool provoked = s->call_provoked;
        s->call_provoked = false;
        pthread_mutex_unlock(&s->mu);
        json_t *t = list_all(s, "tools/list", "tools", NULL);
        pthread_mutex_lock(&s->mu);
        if (t) {
            if (provoked) scope_added_tools_locked(s, t);
            prune_scoped_locked(s, t);
            json_free(s->tools);
            s->tools = t;
        } else if (provoked) {
            s->call_provoked = true;
        }
        pthread_mutex_unlock(&s->mu);
        if (t) {
            cache_tools(s);
            fire_tools_changed(s);
        }
        pthread_mutex_unlock(&s->refresh_mu);
        pthread_mutex_lock(&s->mu);
        s->tool_refreshes--;
        pthread_cond_broadcast(&s->cv);
        pthread_mutex_unlock(&s->mu);
    } else if (!strcmp(j->kind, "prompts")) {
        json_t *pr = list_all(s, "prompts/list", "prompts", NULL);
        if (pr) {
            pthread_mutex_lock(&s->mu);
            json_free(s->prompts);
            s->prompts = pr;
            pthread_mutex_unlock(&s->mu);
            pthread_mutex_lock(&m->mu);
            mcp_callbacks_t cb = m->cb;
            bool live = !m->closing && !s->closing;
            pthread_mutex_unlock(&m->mu);
            if (live && cb.prompts_changed) cb.prompts_changed(cb.ud, s->name);
        }
    } else {
        json_t *res = list_all(s, "resources/list", "resources", NULL);
        json_t *tpl = list_all(s, "resources/templates/list", "resourceTemplates", NULL);
        json_t *add = NULL, *drop = NULL;
        pthread_mutex_lock(&m->mu);
        bool notif = m->notifications;
        pthread_mutex_unlock(&m->mu);
        pthread_mutex_lock(&s->mu);
        if (res) {
            json_free(s->resources);
            s->resources = res;
        }
        if (tpl) {
            json_free(s->templates);
            s->templates = tpl;
        }
        /* keep the subscription set in step with the catalog */
        if (res && notif && json_get_bool(json_get_obj(s->caps, "resources"), "subscribe", false)) {
            json_t *now = resource_uris(res), *u;
            drop = json_arr();
            json_arr_foreach(s->subscribed, i, u) {
                bool keep = false;
                json_t *w;
                json_arr_foreach(now, k, w) keep = keep || str_eq(json_sv(u), json_sv(w));
                if (!keep) json_push(drop, json_clone(u));
            }
            add = json_clone(now);
            json_free(s->subscribed);
            s->subscribed = now;
        }
        pthread_mutex_unlock(&s->mu);
        if (drop && json_len(drop)) subscribe_uris(s, drop, false);
        if (add) subscribe_uris(s, add, true);
        json_free(add);
        json_free(drop);
    }
    free(j->kind);
    free(j);
    srv_unref(s);
    bg_leave(m);
    return NULL;
}

static void handle_notification(mcp_server_t *s, const char *method, const json_t *params) {
    if (!s->mgr || s->closing) return;
    const char *kind = NULL;
    if (!strcmp(method, "notifications/tools/list_changed")) kind = "tools";
    else if (!strcmp(method, "notifications/prompts/list_changed")) kind = "prompts";
    else if (!strcmp(method, "notifications/resources/list_changed")) kind = "resources";
    else if (!strcmp(method, "notifications/resources/updated")) {
        const char *uri = json_get_str(params, "uri");
        bool sub = false;
        pthread_mutex_lock(&s->mu);
        json_t *u;
        json_arr_foreach(s->subscribed, i, u) sub = sub || str_eq(json_sv(u), uri);
        pthread_mutex_unlock(&s->mu);
        if (!uri || !sub) return;
        mcp_manager_t *m = s->mgr;
        pthread_mutex_lock(&m->mu);
        mcp_callbacks_t cb = m->cb;
        bool live = !m->closing;
        pthread_mutex_unlock(&m->mu);
        if (live && cb.resource_updated) cb.resource_updated(cb.ud, s->name, uri);
        return;
    }
    if (!kind || sstate(s) != S_READY) return;
    if (!strcmp(kind, "tools")) {
        pthread_mutex_lock(&s->mu);
        /* a change announced during a call belongs to the sessions whose calls see it through */
        if (s->inflight_calls > 0) s->call_provoked = true;
        s->tool_refreshes++; /* before the call's reply can be read: settle_tool_call waits for this refresh */
        pthread_mutex_unlock(&s->mu);
    }
    /* the reader thread delivers responses: refresh on a thread of its own */
    refresh_job_t *j = xcalloc(1, sizeof *j);
    j->s = s;
    j->kind = xstrdup(kind);
    srv_ref(s);
    bg_enter(s->mgr);
    pthread_t th;
    if (pthread_create(&th, NULL, refresh_thread, j) == 0) pthread_detach(th);
    else {
        if (!strcmp(kind, "tools")) {
            pthread_mutex_lock(&s->mu);
            s->tool_refreshes--;
            pthread_mutex_unlock(&s->mu);
        }
        free(j->kind);
        free(j);
        srv_unref(s);
        bg_leave(s->mgr);
    }
}

/* ---------- one-off probe ---------- */

int mcp_probe(const char *name, const json_t *cfg, int64_t timeout_ms, volatile int *cancel, json_t **info, json_t **tools,
              char **err, bool *auth_required, char **www_auth) {
    if (info) *info = NULL;
    if (tools) *tools = NULL;
    if (err) *err = NULL;
    if (auth_required) *auth_required = false;
    if (www_auth) *www_auth = NULL;
    json_t *c = json_clone(cfg);
    expand_tree(c);
    mcp_server_t *s = server_new(name, c);
    json_free(c);
    if (timeout_ms > 0) s->timeout_ms = timeout_ms;
    s->ext_cancel = cancel;
    int rc = server_connect(s);
    if (cancel && *cancel) rc = -1;
    if (rc == 0) {
        if (info) *info = json_clone(s->info);
        if (tools) *tools = json_clone(s->tools);
    } else {
        if (err) *err = xstrdup(cancel && *cancel ? "Aborted" : s->error ? s->error : "connection failed");
        if (auth_required) *auth_required = s->http_status == 401 || s->http_status == 403;
        if (www_auth && s->www_auth) *www_auth = xstrdup(s->www_auth);
    }
    srv_unref(s);
    return rc;
}

json_t *mcp_probe_catalog(const char *name, const json_t *cfg, int64_t timeout_ms, char **err) {
    if (err) *err = NULL;
    json_t *c = json_clone(cfg);
    expand_tree(c);
    mcp_server_t *s = server_new(name, c);
    json_free(c);
    if (timeout_ms > 0) s->timeout_ms = timeout_ms;
    json_t *out = NULL;
    if (server_connect(s) == 0) {
        out = json_obj();
        json_set(out, "info", json_clone(s->info));
        json_set(out, "tools", json_clone(s->tools));
        json_set(out, "resources", s->resources ? json_clone(s->resources) : json_arr());
        json_set(out, "prompts", s->prompts ? json_clone(s->prompts) : json_arr());
    } else if (err) {
        *err = xstrdup(s->error ? s->error : "connection failed");
    }
    srv_unref(s);
    return out;
}

/* ---------- tool bridge ---------- */

static char *sanitize_part(const char *v, const char *fallback) {
    sb_t b = SB_INIT;
    bool us = false;
    for (const char *p = v; *p; p++) {
        char c = (char)tolower((unsigned char)*p);
        if (isalnum((unsigned char)c) || c == '_') {
            if (c == '_') {
                if (!us && b.len) sb_putc(&b, '_');
                us = true;
            } else {
                sb_putc(&b, c);
                us = false;
            }
        } else if (!us && b.len) {
            sb_putc(&b, '_');
            us = true;
        }
    }
    while (b.len && b.s[b.len - 1] == '_') sb_truncate(&b, b.len - 1);
    if (!b.len) {
        sb_free(&b);
        return xstrdup(fallback);
    }
    return sb_detach(&b);
}

char *mcp_tool_name(const char *server, const char *tool) {
    char *s = sanitize_part(server, "server"), *t = sanitize_part(tool, "tool");
    char *prefix = xasprintf("%s_", s);
    const char *tn = str_starts(t, prefix) ? t + strlen(prefix) : t;
    char *name = xasprintf("mcp__%s_%s", s, tn);
    if (strlen(name) > 64) {
        uint64_t h = hash_xx64(name, strlen(name), 0);
        char hs[16];
        int k = 0;
        const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";
        while (h && k < 8) {
            hs[k++] = digits[h % 36];
            h /= 36;
        }
        hs[k] = 0;
        name[64 - k - 1] = '_';
        memcpy(name + 64 - k, hs, (size_t)k + 1);
    }
    free(prefix);
    free(s);
    free(t);
    return name;
}

/* the bridge names its server: a reconnect / reload replaces the server object, the tool stays valid */
typedef struct {
    mcp_manager_t *m;
    char *server;
    char *tool;
    char *scope; /* the session's view of the manager (NULL: the owning session) */
} bridge_t;

/* #observeToolCall: a call completes only after the tool refresh it provoked lands, then its session sees every tool
 * the server now serves */
static void settle_tool_call(mcp_server_t *s, const bridge_t *b, tool_ctx_t *ctx) {
    volatile sig_atomic_t *cancel = ctx ? ctx->cancel : NULL;
    int64_t until = mono_ms() + (s->timeout_ms > 0 ? s->timeout_ms : 30000);
    bool granted = false;
    const char *key = scope_key(b->scope);
    pthread_mutex_lock(&s->mu);
    s->inflight_calls--;
    while (s->tool_refreshes > 0 && !(cancel && *cancel) && mono_ms() < until) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&s->cv, &s->mu, &ts);
    }
    for (size_t i = 0; s->scoped && i < s->scoped->u.obj.n; i++) {
        json_t *scopes = s->scoped->u.obj.vals[i];
        if (scope_listed(scopes, key)) continue;
        json_push_str(scopes, key);
        granted = true;
    }
    pthread_mutex_unlock(&s->mu);
    if (!granted) return;
    if (ctx && ctx->agent) ctx->agent->mcp_tools_dirty = 1;
    if (!b->scope) fire_tools_changed(s);
}

/* waitForConnection: wait while `s` is still connecting, up to its request timeout or until *cancel */
static void wait_connected(mcp_server_t *s, volatile sig_atomic_t *cancel) {
    int64_t deadline = mono_ms() + (s->timeout_ms > 0 ? s->timeout_ms : 30000);
    pthread_mutex_lock(&s->mu);
    while (s->state == S_CONNECTING && !s->closing && mono_ms() < deadline && !(cancel && *cancel)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&s->cv, &s->mu, &ts);
    }
    pthread_mutex_unlock(&s->mu);
}


/* textCarries: a trimmed text block carries `v` as a verbatim string echo or as JSON deep-equal to it */
static bool text_carries(const char *trimmed, const json_t *parsed, const json_t *v) {
    return (json_is_str(v) && !strcmp(trimmed, json_sv(v))) || (parsed && json_equal(parsed, v));
}

/* structuredContentAlreadyInText (omp 3e2ba2dd2c, 9c44c75c2e): a text block already carries the payload, verbatim
 * or as the MCP SDK's `{result: value}` wrapper for non-object returns (a wrapped list matches item by item) */
static bool structured_in_text(const json_t *sc, const json_t *content) {
    const json_t *wrapped = json_is_obj(sc) && sc->u.obj.n == 1 && !strcmp(sc->u.obj.keys[0], "result") ? sc->u.obj.vals[0] : NULL;
    size_t nb = 0, cap = json_len(content);
    char **trims = cap ? xcalloc(cap, sizeof *trims) : NULL;
    json_t **parsed = cap ? xcalloc(cap, sizeof *parsed) : NULL;
    bool hit = false;
    const json_t *c;
    json_arr_foreach(content, i, c) {
        if (hit || !str_eq(json_get_str(c, "type"), "text")) continue;
        char *t = str_trim(json_get_str(c, "text") ? json_get_str(c, "text") : "");
        json_t *p = json_parse_str(t);
        if (text_carries(t, p, sc) || (wrapped && text_carries(t, p, wrapped))) hit = true;
        trims[nb] = t;
        parsed[nb++] = p;
    }
    if (!hit && json_is_arr(wrapped) && nb > 0 && json_len(wrapped) == nb) {
        hit = true;
        for (size_t i = 0; i < nb && hit; i++) hit = text_carries(trims[i], parsed[i], json_at(wrapped, i));
    }
    for (size_t i = 0; i < nb; i++) {
        free(trims[i]);
        json_free(parsed[i]);
    }
    free(trims);
    free(parsed);
    return hit;
}

static int mcp_tool_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    bridge_t *b = self->ud;
    volatile sig_atomic_t *cancel = ctx ? ctx->cancel : NULL;
    mcp_server_t *s = find_ref(b->m, b->server);
    /* DeferredMCPTool.execute: a server still connecting is waited for; one replaced meanwhile (reconnect, reload)
     * is followed by name; a failed connection gets one reconnect before the call */
    bool reconnected = false;
    for (int pass = 0; s && !ready(s) && pass < 4; pass++) {
        if (sstate(s) == S_CONNECTING) wait_connected(s, cancel);
        if (cancel && *cancel) {
            srv_unref(s);
            tool_result_error(out, "Aborted");
            return 0;
        }
        if (ready(s)) break;
        if (s->closing) {
            srv_unref(s);
            s = find_ref(b->m, b->server);
            continue;
        }
        if (sstate(s) == S_CONNECTING || reconnected) break; /* the wait ran out */
        reconnected = true;
        srv_unref(s);
        char *rerr = NULL;
        mcp_reconnect_server(b->m, b->server, 0, &rerr);
        free(rerr);
        s = find_ref(b->m, b->server);
    }
    if (!s || !ready(s)) {
        const char *e = NULL;
        if (s) pthread_mutex_lock(&s->mu);
        e = s ? s->error : NULL;
        tool_result_error(out, "MCP server %s is not connected%s%s", b->server, e ? ": " : "", e ? e : "");
        if (s) pthread_mutex_unlock(&s->mu);
        srv_unref(s);
        return 0;
    }
    json_t *p = json_obj();
    json_set_str(p, "name", b->tool);
    json_set(p, "arguments", args ? json_clone(args) : json_obj());
    char *err = NULL;
    pthread_mutex_lock(&s->mu);
    s->inflight_calls++;
    pthread_mutex_unlock(&s->mu);
    mcp_fail_t fail = {0};
    json_t *res = mcp_request_ex(s, "tools/call", json_clone(p), s->timeout_ms, &err, &fail);
    settle_tool_call(s, b, ctx);
    if (!res && !(cancel && *cancel) && mcp_fail_reconnectable(&fail)) {
        /* isRetriableConnectionError: a stale connection gets one reconnect and replay */
        srv_unref(s);
        char *rerr = NULL;
        mcp_reconnect_server(b->m, b->server, 0, &rerr);
        free(rerr);
        s = find_ref(b->m, b->server);
        if (s && ready(s)) {
            free(err);
            err = NULL;
            mcp_fail_free(&fail);
            pthread_mutex_lock(&s->mu);
            s->inflight_calls++;
            pthread_mutex_unlock(&s->mu);
            res = mcp_request_ex(s, "tools/call", json_clone(p), s->timeout_ms, &err, &fail);
            settle_tool_call(s, b, ctx);
        }
    }
    json_free(p);
    if (!res) {
        if ((cancel && *cancel) || str_eq(err, "Aborted")) tool_result_error(out, "Aborted");
        else if (fail.set) {
            char *m = mcp_format_tool_failure(&fail, b->server, b->tool);
            tool_result_error(out, "%s", m);
            free(m);
        } else {
            mcp_fail_normalize(&fail, s ? transport_kind(s) : "unknown", "protocol", err ? err : "MCP tool call returned no result", NULL);
            char *m = mcp_format_tool_failure(&fail, b->server, b->tool);
            tool_result_error(out, "%s", m);
            free(m);
        }
        mcp_fail_free(&fail);
        free(err);
        srv_unref(s);
        return 0;
    }
    mcp_fail_free(&fail);
    srv_unref(s);
    /* formatMCPContent: text (and resource text) joined with blank lines into one block, images as their own
     * blocks; other item types are dropped; an empty result is one empty text block */
    json_t *rc = json_get_arr(res, "content"), *c;
    sb_t text = SB_INIT;
#define MCP_APPEND(S_)                                                                                    \
    do {                                                                                                  \
        if (text.len) sb_append(&text, "\n\n");                                                           \
        sb_append(&text, (S_));                                                                           \
    } while (0)
#define MCP_FLUSH()                                                                                       \
    do {                                                                                                  \
        if (text.len) tool_result_text(out, text.s);                                                      \
        text.len = 0;                                                                                     \
        if (text.s) text.s[0] = 0;                                                                        \
    } while (0)
    json_arr_foreach(rc, i, c) {
        const char *t = json_get_str(c, "type");
        if (str_eq(t, "text")) MCP_APPEND(json_get_str(c, "text") ? json_get_str(c, "text") : "");
        else if (str_eq(t, "image")) {
            MCP_FLUSH();
            tool_result_image(out, json_get_str(c, "data"), json_get_str(c, "mimeType"));
        } else if (str_eq(t, "resource")) {
            json_t *r = json_get(c, "resource");
            const char *uri = json_get_str(r, "uri") ? json_get_str(r, "uri") : "";
            const char *txt = json_get_str(r, "text");
            char *line = txt && *txt ? xasprintf("[Resource: %s]\n%s", uri, txt) : xasprintf("[Resource: %s]", uri);
            MCP_APPEND(line);
            free(line);
        }
    }
    MCP_FLUSH();
#undef MCP_APPEND
#undef MCP_FLUSH
    sb_free(&text);
    if (!out->content) tool_result_text(out, "");
    out->is_error = json_get_bool(res, "isError", false);
    if (out->is_error) { /* buildResult: an error result leads with "Error: " */
        json_t *first = json_at(out->content, 0);
        if (str_eq(json_get_str(first, "type"), "text")) {
            char *t = xasprintf("Error: %s", json_get_str(first, "text") ? json_get_str(first, "text") : "");
            json_set_str(first, "text", t);
            free(t);
        } else {
            json_t *e = json_obj();
            json_set_str(e, "type", "text");
            json_set_str(e, "text", "Error:");
            json_arr_insert(out->content, 0, e);
        }
    }
    json_t *sc = json_get(res, "structuredContent");
    if (json_is_obj(sc) && !structured_in_text(sc, rc)) { /* formatStructuredContent: a fenced JSON block */
        char *d = json_dump(sc, 2);
        char *fenced = xasprintf("```json\n%s\n```", d);
        tool_result_text(out, fenced);
        free(fenced);
        free(d);
    }

    out->details = json_obj();
    json_set_str(out->details, "server", b->server);
    json_set_str(out->details, "tool", b->tool);
    json_set_str(out->details, "serverName", b->server); /* omp MCPToolDetails */
    json_set_str(out->details, "mcpToolName", b->tool);
    if (out->is_error) json_set_bool(out->details, "isError", true);
    json_free(res);
    return 0;
}

static void bridge_destroy(agent_tool_t *t) {
    bridge_t *b = t->ud;
    free(b->server);
    free(b->tool);
    free(b->scope);
    free(b);
}

static int tool_name_cmp(const void *x, const void *y) {
    return strcmp((*(agent_tool_t *const *)x)->name, (*(agent_tool_t *const *)y)->name);
}

void mcp_open_tool_scope(mcp_manager_t *m, const char *scope, const char *seed) {
    if (!m || !scope) return;
    pthread_mutex_lock(&m->mu);
    if (!m->tool_scopes) m->tool_scopes = json_obj();
    bool known = json_get(m->tool_scopes, scope) != NULL;
    if (!known) json_set_bool(m->tool_scopes, scope, true); /* never forgotten: a revived session reopens its own */
    pthread_mutex_unlock(&m->mu);
    if (known) return;
    srv_vec_t v;
    snapshot(m, &v);
    for (size_t i = 0; i < v.n; i++) {
        mcp_server_t *s = v.v[i];
        pthread_mutex_lock(&s->mu);
        for (size_t k = 0; s->scoped && k < s->scoped->u.obj.n; k++) {
            json_t *scopes = s->scoped->u.obj.vals[k];
            if (scope_listed(scopes, scope_key(seed))) json_push_str(scopes, scope);
        }
        pthread_mutex_unlock(&s->mu);
    }
    snap_free(&v);
}

int mcp_register_tools(mcp_manager_t *m, agent_t *a) {
    int count = 0;
    VEC(agent_tool_t *) out = {0};
    srv_vec_t v;
    snapshot(m, &v);
    for (size_t i = 0; i < v.n; i++) {
        mcp_server_t *s = v.v[i];
        /* MCPTool for a connected server; DeferredMCPTool (the same bridge: the executor waits for the connection)
         * from the cached catalog while it is not */
        bool live = ready(s);
        pthread_mutex_lock(&s->mu);
        json_t *tools = live ? json_clone(s->tools) : s->cached_tools ? json_clone(s->cached_tools) : NULL;
        pthread_mutex_unlock(&s->mu);
        if (!tools) continue;
        json_t *t;
        json_arr_foreach(tools, k, t) {
            const char *tn = json_get_str(t, "name");
            if (!tn) continue;
            /* MCPManager.getTools(scope) */
            pthread_mutex_lock(&s->mu);
            const json_t *scopes = json_get(s->scoped, tn);
            bool visible = !scopes || scope_listed(scopes, scope_key(a->mcp_scope));
            pthread_mutex_unlock(&s->mu);
            if (!visible) continue;
            char *name = mcp_tool_name(s->name, tn);
            json_t *schema = json_get_obj(t, "inputSchema");
            json_t *params = schema ? json_clone(schema) : json_parse_str("{\"type\":\"object\",\"properties\":{}}");
            if (!json_get(params, "type")) json_set_str(params, "type", "object");
            if (!json_get(params, "properties")) json_set(params, "properties", json_obj());
            const char *desc = json_get_str(t, "description");
            char *d = xasprintf("%s\n\n(MCP tool `%s` from server `%s`)", desc ? desc : "", tn, s->name);
            char *label = xasprintf("%s/%s", s->name, tn); /* omp MCPTool label */
            agent_tool_t *at = agent_tool_new(name, label, d, params, mcp_tool_execute);
            free(label);
            bridge_t *b = xcalloc(1, sizeof(*b));
            b->m = m;
            b->server = xstrdup(s->name);
            b->tool = xstrdup(tn);
            b->scope = a->mcp_scope ? xstrdup(a->mcp_scope) : NULL;
            at->ud = b;
            at->destroy = bridge_destroy;
            at->lenient_args = true;
            at->mcp_server = xstrdup(s->name);
            at->mcp_tool = xstrdup(tn);
            vec_push(&out, at);
            free(d);
            free(name);
            count++;
        }
        json_free(tools);
    }
    snap_free(&v);
    /* sortMCPToolsByName: a stable total order independent of connection sequence (prompt-cache stability) */
    if (out.n > 1) xqsort(out.v, out.n, sizeof(out.v[0]), tool_name_cmp);
    for (size_t i = 0; i < out.n; i++) agent_add_tool(a, out.v[i]);
    free(out.v);
    return count;
}

int mcp_sync_tools(mcp_manager_t *m, agent_t *a) {
    strvec_t names = {0};
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->all_tools.n; i++)
        if (str_starts(a->all_tools.v[i]->name, "mcp__")) strvec_push(&names, a->all_tools.v[i]->name);
    pthread_mutex_unlock(&a->mu);
    for (size_t i = 0; i < names.n; i++) agent_remove_tool(a, names.v[i]);
    strvec_free(&names);
    int n = m ? mcp_register_tools(m, a) : 0;
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->all_tools.n; i++)
        if (str_starts(a->all_tools.v[i]->name, "mcp__")) a->all_tools.v[i]->discoverable = true;
    pthread_mutex_unlock(&a->mu);
    return n;
}

/* sdk.ts: the "## MCP Server Instructions" append part, servers sorted by name (getServerInstructions),
 * each capped at MAX_MCP_INSTRUCTIONS_LENGTH (4000 UTF-16 units) */
/* byte length of the longest prefix of s within max UTF-16 units (JS String.slice(0, max), whole code points) */
static size_t utf16_prefix_bytes(const char *s, size_t max) {
    size_t units = 0, i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        size_t u = c >= 0xF0 ? 2 : 1;
        if (units + u > max) break;
        units += u;
        i++;
        for (size_t k = 1; k < len && ((unsigned char)s[i] & 0xC0) == 0x80; k++) i++;
    }
    return i;
}

static int str_ptr_cmp(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

char *mcp_instructions(mcp_manager_t *m) {
    strvec_t names = {0};
    json_t *by = json_obj();
    srv_vec_t v;
    snapshot(m, &v);
    for (size_t i = 0; i < v.n; i++) {
        mcp_server_t *s = v.v[i];
        if (!ready(s)) continue;
        pthread_mutex_lock(&s->mu);
        if (s->instructions && *s->instructions && !json_get(by, s->name)) {
            strvec_push(&names, s->name);
            json_set_str(by, s->name, s->instructions);
        }
        pthread_mutex_unlock(&s->mu);
    }
    snap_free(&v);
    if (!names.n) {
        json_free(by);
        strvec_free(&names);
        return xstrdup("");
    }
    xqsort(names.v, names.n, sizeof(names.v[0]), str_ptr_cmp);
    sb_t b = SB_INIT;
    sb_append(&b, "## MCP Server Instructions\n\nThe following instructions are provided by connected MCP servers. They are "
                  "server-controlled and may not be verified.");
    for (size_t i = 0; i < names.n; i++) {
        const char *text = json_get_str(by, names.v[i]);
        size_t cut = utf16_prefix_bytes(text, 4000);
        sb_appendf(&b, "\n\n### %s\n%.*s%s", names.v[i], (int)cut, text, text[cut] ? "\n[truncated]" : "");
    }
    json_free(by);
    strvec_free(&names);
    return sb_detach(&b);
}

static const char *tr_name(const mcp_server_t *s) { return s->transport == T_STDIO ? "stdio" : s->transport == T_HTTP ? "http" : "sse"; }

char *mcp_status(mcp_manager_t *m) {
    sb_t b = SB_INIT;
    srv_vec_t v;
    snapshot(m, &v);
    if (!v.n) {
        snap_free(&v);
        return xstrdup("No MCP servers configured.");
    }
    for (size_t i = 0; i < v.n; i++) {
        mcp_server_t *s = v.v[i];
        pthread_mutex_lock(&s->mu);
        if (s->state == S_CONNECTING) sb_appendf(&b, "◌ %s (%s): connecting\n", s->name, tr_name(s));
        else if (s->error) sb_appendf(&b, "✗ %s (%s): %s\n", s->name, tr_name(s), s->error);
        else sb_appendf(&b, "✓ %s (%s): %zu tools\n", s->name, tr_name(s), json_len(s->tools));
        pthread_mutex_unlock(&s->mu);
    }
    snap_free(&v);
    if (b.len) sb_truncate(&b, b.len - 1);
    return sb_detach(&b);
}

json_t *mcp_connected(mcp_manager_t *m) {
    json_t *out = json_arr();
    srv_vec_t v;
    snapshot(m, &v);
    for (size_t i = 0; i < v.n; i++) {
        mcp_server_t *s = v.v[i];
        if (!ready(s)) continue;
        json_t *o = json_obj();
        json_set_str(o, "name", s->name);
        pthread_mutex_lock(&s->mu);
        json_set_int(o, "tools", (int64_t)json_len(s->tools));
        pthread_mutex_unlock(&s->mu);
        json_push(out, o);
    }
    snap_free(&v);
    return out;
}

json_t *mcp_failed(mcp_manager_t *m) {
    json_t *out = json_arr();
    srv_vec_t v;
    snapshot(m, &v);
    for (size_t i = 0; i < v.n; i++) {
        mcp_server_t *s = v.v[i];
        pthread_mutex_lock(&s->mu);
        const char *e = s->state == S_CONNECTING ? "connection timed out" : s->state == S_FAILED ? (s->error ? s->error : "failed") : NULL;
        if (e) {
            json_t *o = json_obj();
            json_set_str(o, "name", s->name);
            json_set_str(o, "error", e);
            json_set_str(o, "transport", tr_name(s));
            json_push(out, o);
        }
        pthread_mutex_unlock(&s->mu);
    }
    snap_free(&v);
    return out;
}

const char *mcp_config_url(const json_t *cfg) {
    if (json_get_str(cfg, "command")) return NULL;
    const char *u = json_get_str(cfg, "url");
    return u ? u : json_get_str(cfg, "serverUrl");
}

/* ---------- resources (mcp:// internal URLs; port of internal-urls/mcp-protocol.ts) ---------- */

/* match a URI template ({expr} matches any substring); reports literal chars and expression count */
static bool template_match(const char *uri, const char *tpl, size_t *literal, size_t *exprs) {
    *literal = 0;
    *exprs = 0;
    const char *u = uri, *t = tpl;
    const char *star_t = NULL, *star_u = NULL;
    while (*u) {
        if (*t == '{') {
            const char *e = strchr(t, '}');
            if (!e) return false;
            star_t = e + 1;
            star_u = u;
            t = e + 1;
            continue;
        }
        if (*t && *t == *u) {
            t++;
            u++;
            continue;
        }
        if (star_t) {
            t = star_t;
            u = ++star_u;
            continue;
        }
        return false;
    }
    while (*t == '{') {
        const char *e = strchr(t, '}');
        if (!e) return false;
        t = e + 1;
    }
    if (*t) return false;
    for (const char *p = tpl; *p; p++) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (!e) break;
            (*exprs)++;
            p = e;
        } else {
            (*literal)++;
        }
    }
    return true;
}

/* every page of a list method; NULL (and *err_out) when the first page fails */
static json_t *list_all(mcp_server_t *s, const char *method, const char *key, char **err_out) {
    json_t *all = json_arr();
    char *cursor = NULL;
    if (err_out) *err_out = NULL;
    for (int page = 0; page < 50; page++) {
        json_t *p = json_obj();
        if (cursor) json_set_str(p, "cursor", cursor);
        char *err = NULL;
        json_t *res = mcp_request(s, method, p, s->timeout_ms, &err);
        free(cursor);
        cursor = NULL;
        if (!res) {
            if (page == 0 && err_out) {
                *err_out = err;
                err = NULL;
                json_free(all);
                all = NULL;
            }
            free(err);
            break;
        }
        free(err);
        json_t *items = json_get(res, key), *x;
        json_arr_foreach(items, i, x) json_push(all, json_clone(x));
        const char *next = json_get_str(res, "nextCursor");
        if (next && *next) cursor = xstrdup(next);
        json_free(res);
        if (!cursor) break;
    }
    free(cursor);
    return all;
}

mcp_manager_t *mcp_default_manager(void) {
    pthread_mutex_lock(&g_default_mu);
    mcp_manager_t *m = g_default;
    pthread_mutex_unlock(&g_default_mu);
    return m;
}

int mcp_read_resource(mcp_manager_t *m, const char *uri, char **text, char **server, char **err) {
    *text = NULL;
    *server = NULL;
    *err = NULL;
    if (!m) {
        *err = xstrdup("No MCP manager available. MCP servers may not be configured.");
        return -1;
    }
    mcp_server_t *target = NULL, *tpl_target = NULL;
    size_t best_lit = 0, best_expr = 0;
    bool have_tpl = false;
    sb_t avail = SB_INIT;
    srv_vec_t v;
    snapshot(m, &v);
    for (size_t i = 0; i < v.n; i++) {
        mcp_server_t *s = v.v[i];
        if (!ready(s)) continue;
        json_t *res = list_all(s, "resources/list", "resources", NULL), *x;
        json_arr_foreach(res, k, x) {
            const char *u = json_get_str(x, "uri");
            if (!u) continue;
            sb_appendf(&avail, "  %s (%s)\n", u, s->name);
            if (!target && !strcmp(u, uri)) target = s;
        }
        json_free(res);
        json_t *tpls = list_all(s, "resources/templates/list", "resourceTemplates", NULL);
        json_arr_foreach(tpls, k, x) {
            const char *t = json_get_str(x, "uriTemplate");
            if (!t) continue;
            sb_appendf(&avail, "  %s (%s, template)\n", t, s->name);
            size_t lit, ex;
            if (template_match(uri, t, &lit, &ex) &&
                (!have_tpl || lit > best_lit || (lit == best_lit && ex < best_expr))) {
                have_tpl = true;
                best_lit = lit;
                best_expr = ex;
                tpl_target = s;
            }
        }
        json_free(tpls);
    }
    if (!target) target = tpl_target;
    if (!target) {
        snap_free(&v);
        char *list = sb_detach(&avail);
        size_t ll = strlen(list);
        if (ll && list[ll - 1] == '\n') list[ll - 1] = 0;
        *err = xasprintf("No MCP server has resource \"%s\".\n\nAvailable resources:\n%s", uri, *list ? list : "  (none)");
        free(list);
        return -1;
    }
    char *drop = sb_detach(&avail);
    free(drop);
    json_t *p = json_obj();
    json_set_str(p, "uri", uri);
    char *rerr = NULL;
    json_t *res = mcp_request(target, "resources/read", p, target->timeout_ms, &rerr);
    if (!res) {
        *err = xasprintf("MCP resource read error: %s", rerr ? rerr : "request failed");
        free(rerr);
        snap_free(&v);
        return -1;
    }
    free(rerr);
    sb_t b = SB_INIT;
    int parts = 0;
    json_t *contents = json_get(res, "contents"), *x;
    json_arr_foreach(contents, i, x) {
        const char *t = json_get_str(x, "text");
        const char *blob = json_get_str(x, "blob");
        if (!t && !blob) continue;
        if (parts++) sb_append(&b, "\n---\n");
        if (t) sb_append(&b, t);
        else {
            const char *mt = json_get_str(x, "mimeType");
            sb_appendf(&b, "[Binary content: %s, base64 length %zu]", mt ? mt : "unknown", strlen(blob));
        }
    }
    json_free(res);
    if (!parts) sb_append(&b, "(empty resource)");
    *text = sb_detach(&b);
    *server = xstrdup(target->name);
    snap_free(&v);
    return 0;
}

/* ---------- prompts as slash commands (sdk.ts buildMCPPromptCommands) ---------- */

/* prompt-args.ts mapPromptArguments: a `key=value` token lands under its own name; the positional tokens fill the
 * declared arguments in declaration order, skipping names a `key=value` token already set; positionals past the
 * last free declared argument are ignored */
json_t *mcp_map_prompt_arguments(const json_t *declared, const strvec_t *tokens) {
    json_t *named = json_obj();
    strvec_t positional = {0};
    for (size_t i = 0; i < tokens->n; i++) {
        const char *t = tokens->v[i], *eq = strchr(t, '=');
        if (eq && eq > t) {
            char *k = xstrndup(t, (size_t)(eq - t));
            json_set_str(named, k, eq + 1);
            free(k);
        } else if (*t) {
            strvec_push(&positional, t);
        }
    }
    size_t slot = 0, nslots = json_len(declared);
    for (size_t i = 0; i < positional.n; i++) {
        const char *name = NULL;
        while (slot < nslots) {
            const char *n = json_get_str(json_at(declared, slot), "name");
            if (n && !json_get(named, n)) {
                name = n;
                break;
            }
            slot++;
        }
        if (!name) break;
        json_set_str(named, name, positional.v[i]);
        slot++;
    }
    strvec_free(&positional);
    return named;
}

json_t *mcp_prompt_commands(mcp_manager_t *m) {
    json_t *out = json_arr();
    srv_vec_t v;
    snapshot(m, &v);
    for (size_t i = 0; i < v.n; i++) {
        mcp_server_t *s = v.v[i];
        if (!ready(s)) continue;
        pthread_mutex_lock(&s->mu);
        json_t *p;
        json_arr_foreach(s->prompts, k, p) {
            const char *pn = json_get_str(p, "name");
            if (!pn) continue;
            json_t *c = json_obj();
            char *name = xasprintf("%s:%s", s->name, pn);
            json_set_str(c, "name", name);
            free(name);
            const char *d = json_get_str(p, "description");
            char *desc = d ? xstrdup(d) : xasprintf("MCP prompt from %s", s->name);
            json_set_str(c, "description", desc);
            free(desc);
            json_set_str(c, "server", s->name);
            json_set_str(c, "prompt", pn);
            if (json_is_arr(json_get(p, "arguments"))) json_set(c, "arguments", json_clone(json_get(p, "arguments")));
            json_push(out, c);
        }
        pthread_mutex_unlock(&s->mu);
    }
    snap_free(&v);
    return out;
}

char *mcp_run_prompt(mcp_manager_t *m, const char *server, const char *prompt, json_t *args, char **err) {
    *err = NULL;
    mcp_server_t *s = find_ref(m, server);
    if (!s || !ready(s)) {
        srv_unref(s);
        json_free(args);
        return NULL; /* executePrompt: no connection, no result */
    }
    json_t *p = json_obj();
    json_set_str(p, "name", prompt);
    if (args && json_len(args)) json_set(p, "arguments", args);
    else json_free(args);
    json_t *res = mcp_request(s, "prompts/get", p, s->timeout_ms, err);
    srv_unref(s);
    if (!res) return NULL;
    sb_t b = SB_INIT;
    int parts = 0;
    json_t *msg;
    json_arr_foreach(json_get_arr(res, "messages"), i, msg) {
        json_t *content = json_get(msg, "content");
        json_t *one = NULL;
        if (json_is_obj(content)) {
            one = json_arr();
            json_push(one, json_clone(content));
            content = one;
        }
        json_t *item;
        json_arr_foreach(content, k, item) {
            const char *t = json_get_str(item, "type");
            const char *text = NULL;
            if (str_eq(t, "text")) text = json_get_str(item, "text");
            else if (str_eq(t, "resource")) text = json_get_str(json_get(item, "resource"), "text");
            if (!text || (str_eq(t, "resource") && !*text)) continue;
            if (parts++) sb_append(&b, "\n\n");
            sb_append(&b, text);
        }
        json_free(one);
    }
    json_free(res);
    return sb_detach(&b);
}
