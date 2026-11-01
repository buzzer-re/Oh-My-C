#define _GNU_SOURCE
#include "mcp.h"

#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "net/http.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#define MCP_PROTOCOL "2025-06-18"

typedef enum { T_STDIO, T_HTTP, T_SSE } transport_t;

typedef struct {
    int64_t id;
    json_t *result; /* owned response object */
    bool done;
} pending_t;

struct mcp_server {
    char *name;
    json_t *cfg;
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
    pthread_mutex_t mu;
    pthread_cond_t cv;
    VEC(pending_t *) pending;
    int64_t next_id;
    json_t *tools;      /* tools/list result array */
    char *instructions;
    char *error;
    char *www_auth; /* last 401 challenge (OAuth discovery) */
    bool connected;
    int64_t timeout_ms;
};

struct mcp_manager {
    VEC(mcp_server_t *) servers;
};

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

static void add_servers(json_t *out, json_t *disabled, const char *path, const char *map_path) {
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
    json_arr_foreach(dis, i, x) if (json_is_str(x)) json_push_str(disabled, json_sv(x));
    json_t *map = json_path(j, map_path);
    if (!json_is_obj(map) && strcmp(map_path, "mcpServers") == 0) map = json_get_obj(j, "servers");
    const char *k;
    json_t *v;
    json_obj_foreach(map, i, k, v) {
        if (!json_is_obj(v) || json_get(out, k)) continue;
        json_t *c = json_clone(v);
        json_set_str(c, "_source", path);
        json_set(out, k, c);
    }
    json_free(j);
}

json_t *mcp_discover_config(const char *cwd) {
    json_t *out = json_obj();
    json_t *disabled = json_arr();
    const char *proj[] = {".omc/mcp.json", ".omp/mcp.json", ".omc/.mcp.json", ".omp/.mcp.json", "mcp.json",
                          ".mcp.json", ".claude/mcp.json", ".claude/.mcp.json", ".cursor/mcp.json"};
    for (size_t i = 0; i < ARRAY_LEN(proj); i++) {
        char *p = path_join(cwd, proj[i]);
        add_servers(out, disabled, p, "mcpServers");
        free(p);
    }
    char *vs = path_join(cwd, ".vscode/mcp.json");
    add_servers(out, disabled, vs, "servers");
    free(vs);
    const char *agent_files[] = {"mcp.json", ".mcp.json"};
    for (size_t i = 0; i < ARRAY_LEN(agent_files); i++) {
        char *p = omc_agent_path(agent_files[i]);
        add_servers(out, disabled, p, "mcpServers");
        free(p);
    }
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
        add_servers(out, disabled, p, "mcpServers");
        free(p);
    }
    if (want_claude) {
        char *p = path_join(home_dir(), ".claude/mcp.json");
        add_servers(out, disabled, p, "mcpServers");
        free(p);
        /* ~/.claude.json: global mcpServers + per-project projects.<cwd>.mcpServers */
        char *cj = path_join(home_dir(), ".claude.json");
        size_t n;
        char *s = fs_read_limit(cj, 32u << 20, &n, NULL);
        if (s) {
            json_t *j = json_parse(s, n, NULL);
            json_t *projs = json_get_obj(j, "projects");
            json_t *pm = json_get_obj(json_get_obj(projs, cwd), "mcpServers");
            const char *k;
            json_t *v;
            json_obj_foreach(pm, i, k, v) if (!json_get(out, k)) json_set(out, k, json_clone(v));
            json_obj_foreach(json_get_obj(j, "mcpServers"), i, k, v) if (!json_get(out, k)) json_set(out, k, json_clone(v));
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

static void deliver(mcp_server_t *s, json_t *msg) {
    if (!json_is_obj(msg)) {
        json_free(msg);
        return;
    }
    json_t *idj = json_get(msg, "id");
    if (json_is_num(idj) && (json_get(msg, "result") || json_get(msg, "error"))) {
        int64_t id = idj->is_int ? idj->u.num.i : (int64_t)idj->u.num.d;
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
    /* server -> client request (e.g. ping, roots/list): answer minimal */
    const char *method = json_get_str(msg, "method");
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
            char *line = json_dump(resp, 0);
            sb_t b = SB_INIT;
            sb_append(&b, line);
            sb_putc(&b, '\n');
            pthread_mutex_lock(&s->mu);
            if (write(s->child.in_fd, b.s, b.len) < 0) {}
            pthread_mutex_unlock(&s->mu);
            sb_free(&b);
            free(line);
        }
        json_free(resp);
    }
    json_free(msg);
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
    pthread_mutex_lock(&s->mu);
    s->connected = false;
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
    if (status == 401) {
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
    if (http_headers_get(h, "Authorization")) return;
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
                      .ud = &c, .max_redirects = 5};
    http_resp_t r;
    int rc = http_request(&req, &r);
    if (rc == 0 && c.is_sse) sse_finish(&c.sse);
    if (rc != 0) {
        *err = xstrdup(r.error ? r.error : "request failed");
    } else if (c.status == 401) {
        *err = xasprintf("HTTP 401: the server requires OAuth authorization; run `omc mcp login %s`", s->name);
        rc = -1;
    } else if (c.status >= 400) {
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

json_t *mcp_request(mcp_server_t *s, const char *method, json_t *params, int64_t timeout_ms, char **err) {
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
    json_t *result = NULL;
    pthread_mutex_lock(&s->mu);
    if (rc == 0) {
        int64_t deadline = timeout_ms > 0 ? mono_ms() + timeout_ms : 0;
        /* streamable HTTP delivers the response inside the POST; stdio/SSE responses arrive on the reader */
        while (!p->done && s->transport != T_HTTP && s->connected && !s->closing) {
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
            if (e) *err = xasprintf("%s (code %lld)", json_get_str(e, "message") ? json_get_str(e, "message") : "error",
                                    (long long)json_get_int(e, "code", 0));
            else result = json_take(p->result, "result");
        } else {
            *err = s->connected || s->transport == T_HTTP ? xasprintf("%s: %s timed out", s->name, method)
                                                           : xasprintf("%s: server disconnected", s->name);
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
    volatile sig_atomic_t cancel = 0;
    http_req_t req = {.method = "GET", .url = s->url, .headers = &h, .on_data = sse_bg_data, .ud = &c,
                      .idle_timeout_ms = 24 * 3600 * 1000LL, .cancel = &cancel};
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
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->child.in_fd = s->child.out_fd = s->child.err_fd = -1;
    const char *type = json_get_str(cfg, "type");
    const char *url = json_get_str(cfg, "url");
    if (!url) url = json_get_str(cfg, "serverUrl");
    if (str_eq(type, "sse")) s->transport = T_SSE;
    else if (str_eq(type, "http") || str_eq(type, "streamable-http") || (!json_get_str(cfg, "command") && url)) s->transport = T_HTTP;
    else s->transport = T_STDIO;
    if (url) s->url = xstrdup(url);
    const char *k;
    json_t *v;
    json_obj_foreach(json_get_obj(cfg, "headers"), i, k, v) if (json_is_str(v)) http_headers_add(&s->headers, k, json_sv(v));
    const char *env_to = env_get("OMP_MCP_TIMEOUT_MS");
    if (!env_to) env_to = env_get("OMC_MCP_TIMEOUT_MS");
    s->timeout_ms = env_to ? strtoll(env_to, NULL, 10) : json_get_int(cfg, "timeout", 30000);
    return s;
}

static void server_free(mcp_server_t *s) {
    s->closing = true;
    if (s->child_started) {
        child_close(&s->child);
    }
    if (s->reader_started) pthread_join(s->reader, NULL);
    json_free(s->cfg);
    json_free(s->tools);
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
    pthread_cond_destroy(&s->cv);
    free(s);
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
            s->error = xasprintf("failed to start %s: %s", cmd, strerror(errno));
            return -1;
        }
        s->child_started = true;
        s->connected = true;
        if (pthread_create(&s->reader, NULL, stdio_reader, s) == 0) s->reader_started = true;
    } else if (s->transport == T_SSE) {
        if (pthread_create(&s->reader, NULL, sse_reader, s) == 0) s->reader_started = true;
        s->connected = true;
        pthread_mutex_lock(&s->mu);
        int64_t deadline = mono_ms() + s->timeout_ms;
        while (!s->post_url && s->connected && mono_ms() < deadline) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
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
    if (json_get_str(init, "instructions") && json_get_bool(s->cfg, "instructions", true))
        s->instructions = xstrdup(json_get_str(init, "instructions"));
    s->connected = true;
    json_free(init);
    notify(s, "notifications/initialized");
    /* list tools (paginated) */
    s->tools = json_arr();
    char *cursor = NULL;
    for (int page = 0; page < 50; page++) {
        json_t *p = json_obj();
        if (cursor) json_set_str(p, "cursor", cursor);
        json_t *res = mcp_request(s, "tools/list", p, s->timeout_ms, &err);
        free(cursor);
        cursor = NULL;
        if (!res) {
            s->error = err;
            return -1;
        }
        json_t *t;
        json_arr_foreach(json_get_arr(res, "tools"), i, t) json_push(s->tools, json_clone(t));
        if (json_get_str(res, "nextCursor")) cursor = xstrdup(json_get_str(res, "nextCursor"));
        json_free(res);
        if (!cursor) break;
    }
    free(cursor);
    return 0;
}

mcp_manager_t *mcp_manager_new(void) { return xcalloc(1, sizeof(mcp_manager_t)); }

void mcp_manager_free(mcp_manager_t *m) {
    if (!m) return;
    for (size_t i = 0; i < m->servers.n; i++) server_free(m->servers.v[i]);
    vec_free(&m->servers);
    free(m);
}

size_t mcp_server_count(mcp_manager_t *m) { return m ? m->servers.n : 0; }

typedef struct {
    mcp_server_t *s;
    pthread_t th;
    volatile bool done;
} conn_job_t;

static void *conn_thread(void *p) {
    conn_job_t *j = p;
    if (server_connect(j->s) != 0) LOGW("mcp %s: %s", j->s->name, j->s->error ? j->s->error : "failed");
    j->done = true;
    return NULL;
}

void mcp_connect_all(mcp_manager_t *m, const json_t *config, int64_t timeout_ms) {
    size_t n = json_len(config);
    if (!n) return;
    conn_job_t *jobs = xcalloc(n, sizeof(conn_job_t));
    for (size_t i = 0; i < n; i++) {
        jobs[i].s = server_new(config->u.obj.keys[i], config->u.obj.vals[i]);
        vec_push(&m->servers, jobs[i].s);
        if (pthread_create(&jobs[i].th, NULL, conn_thread, &jobs[i]) != 0) conn_thread(&jobs[i]);
    }
    int64_t deadline = mono_ms() + timeout_ms;
    for (size_t i = 0; i < n; i++) {
        while (!jobs[i].done && mono_ms() < deadline) sleep_ms(10);
        if (jobs[i].done) pthread_join(jobs[i].th, NULL);
        else {
            jobs[i].s->error = xstrdup("connection timed out");
            pthread_detach(jobs[i].th);
        }
    }
    free(jobs);
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

typedef struct {
    mcp_server_t *server;
    char *tool;
} bridge_t;

static int mcp_tool_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    bridge_t *b = self->ud;
    UNUSED(ctx);
    if (!b->server->connected) {
        tool_result_error(out, "MCP server %s is not connected%s%s", b->server->name, b->server->error ? ": " : "",
                          b->server->error ? b->server->error : "");
        return 0;
    }
    json_t *p = json_obj();
    json_set_str(p, "name", b->tool);
    json_set(p, "arguments", args ? json_clone(args) : json_obj());
    char *err = NULL;
    json_t *res = mcp_request(b->server, "tools/call", p, b->server->timeout_ms, &err);
    if (!res) {
        tool_result_error(out, "MCP %s/%s failed: %s", b->server->name, b->tool, err ? err : "error");
        free(err);
        return 0;
    }
    json_t *c;
    json_arr_foreach(json_get_arr(res, "content"), i, c) {
        const char *t = json_get_str(c, "type");
        if (str_eq(t, "text")) tool_result_text(out, json_get_str(c, "text"));
        else if (str_eq(t, "image")) tool_result_image(out, json_get_str(c, "data"), json_get_str(c, "mimeType"));
        else if (str_eq(t, "resource")) {
            json_t *r = json_get(c, "resource");
            const char *txt = json_get_str(r, "text");
            tool_result_textf(out, "[resource %s]\n%s", json_get_str(r, "uri") ? json_get_str(r, "uri") : "", txt ? txt : "(binary)");
        } else if (str_eq(t, "resource_link")) {
            tool_result_textf(out, "[resource link %s] %s", json_get_str(c, "uri"), json_get_str(c, "name") ? json_get_str(c, "name") : "");
        } else {
            char *d = json_dump(c, 0);
            tool_result_text(out, d);
            free(d);
        }
    }
    json_t *sc = json_get(res, "structuredContent");
    if (sc && !out->content) {
        char *d = json_dump(sc, 2);
        tool_result_text(out, d);
        free(d);
    }
    if (!out->content) tool_result_text(out, "(no content)");
    out->is_error = json_get_bool(res, "isError", false);
    out->details = json_obj();
    json_set_str(out->details, "server", b->server->name);
    json_set_str(out->details, "tool", b->tool);
    json_free(res);
    return 0;
}

static void bridge_destroy(agent_tool_t *t) {
    bridge_t *b = t->ud;
    free(b->tool);
    free(b);
}

int mcp_register_tools(mcp_manager_t *m, agent_t *a) {
    int count = 0;
    for (size_t i = 0; m && i < m->servers.n; i++) {
        mcp_server_t *s = m->servers.v[i];
        json_t *t;
        json_arr_foreach(s->tools, k, t) {
            const char *tn = json_get_str(t, "name");
            if (!tn) continue;
            char *name = mcp_tool_name(s->name, tn);
            json_t *schema = json_get_obj(t, "inputSchema");
            json_t *params = schema ? json_clone(schema) : json_parse_str("{\"type\":\"object\",\"properties\":{}}");
            if (!json_get(params, "type")) json_set_str(params, "type", "object");
            if (!json_get(params, "properties")) json_set(params, "properties", json_obj());
            const char *desc = json_get_str(t, "description");
            char *d = xasprintf("%s\n\n(MCP tool `%s` from server `%s`)", desc ? desc : "", tn, s->name);
            agent_tool_t *at = agent_tool_new(name, json_get_str(t, "title") ? json_get_str(t, "title") : tn, d, params,
                                              mcp_tool_execute);
            bridge_t *b = xcalloc(1, sizeof(*b));
            b->server = s;
            b->tool = xstrdup(tn);
            at->ud = b;
            at->destroy = bridge_destroy;
            at->lenient_args = true;
            agent_add_tool(a, at);
            free(d);
            free(name);
            count++;
        }
    }
    return count;
}

char *mcp_instructions(mcp_manager_t *m) {
    sb_t b = SB_INIT;
    for (size_t i = 0; m && i < m->servers.n; i++) {
        mcp_server_t *s = m->servers.v[i];
        if (!s->instructions || !*s->instructions) continue;
        if (!b.len) sb_append(&b, "# MCP Server Instructions\n");
        sb_appendf(&b, "\n## %s\n%s\n", s->name, s->instructions);
    }
    return sb_detach(&b);
}

char *mcp_status(mcp_manager_t *m) {
    sb_t b = SB_INIT;
    if (!m || !m->servers.n) return xstrdup("No MCP servers configured.");
    for (size_t i = 0; i < m->servers.n; i++) {
        mcp_server_t *s = m->servers.v[i];
        const char *tr = s->transport == T_STDIO ? "stdio" : s->transport == T_HTTP ? "http" : "sse";
        if (s->error) sb_appendf(&b, "✗ %s (%s): %s\n", s->name, tr, s->error);
        else sb_appendf(&b, "✓ %s (%s): %zu tools\n", s->name, tr, json_len(s->tools));
    }
    if (b.len) sb_truncate(&b, b.len - 1);
    return sb_detach(&b);
}

const char *mcp_config_url(const json_t *cfg) {
    if (json_get_str(cfg, "command")) return NULL;
    const char *u = json_get_str(cfg, "url");
    return u ? u : json_get_str(cfg, "serverUrl");
}
