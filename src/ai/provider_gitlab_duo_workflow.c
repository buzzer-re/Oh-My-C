/* provider_gitlab_duo_workflow.c - "gitlab-duo-agent" API: GitLab Duo Agent Platform (Duo Workflow Service) over a
 * WebSocket (port of omp packages/ai/src/providers/gitlab-duo-workflow.ts + catalog discovery/gitlab-duo-workflow.ts).
 *
 * One stream = one model turn. A fresh turn resolves the root namespace (GITLAB_DUO_NAMESPACE_ID, project config,
 * the workspace git remote, or the token's top-level groups), best-effort enables the namespace's Duo agent-platform
 * settings, discovers/resolves a project, mints a workflow token (direct_access), creates a workflow and opens the
 * workflow WebSocket with an inline "ambient" flow whose system slot carries the agent's system prompt and whose goal
 * is the conversation rendered as a ChatML transcript. Checkpoints (full ui_chat_log snapshots) stream as text /
 * thinking deltas; a `runMCPTool` action becomes a toolCall and ends the turn with toolUse while the socket stays open
 * in a per-session slot, so the next turn returns the tool result as an actionResponse on the same connection.
 * Approval statuses restart with an approval start request; idle timeouts, the server step limit, its generic
 * processing error and stalled workflows restart on a fresh workflow (bounded). */
#define _GNU_SOURCE
#include "ai.h"
#include "core/embed.h"
#include "core/fs.h"
#include "net/ws.h"

#include <ctype.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>

#define DEFAULT_BASE "https://gitlab.com"
#define CLIENT_TYPE "node-websocket"
#define LS_VERSION "8.104.0"
#define DEFINITION "ambient"
#define IDLE_TIMEOUT_MS 90000
#define REST_TIMEOUT_MS 30000
#define MAX_STEP_LIMIT_RESTARTS 4
#define MAX_GENERIC_ERROR_RETRIES 1
#define MAX_STALL_RESTARTS 2
#define GOAL_SOFT_BYTES 1048576
#define GOAL_HARD_BYTES 2000000
#define MAX_GROUP_PAGES 50
#define INLINE_AGENT "omp_agent"
#define INLINE_PROMPT "omp_inline_prompt"
#define STALL_MESSAGE \
    "GitLab Duo Agent stopped making progress (the workflow's visible history did not advance after multiple restarts)."

enum { R_CONTINUE, R_CLOSED, R_TERMINAL, R_APPROVAL, R_ACTION, R_PAUSE, R_TIMEOUT, R_STEP_LIMIT, R_RETRYABLE, R_STALLED, R_ERROR };

static const char *const MODELS_QUERY =
    "query omp_gitlabDuoWorkflowAvailableModels($rootNamespaceId: GroupID!) {\n"
    "  aiChatAvailableModels(rootNamespaceId: $rootNamespaceId) {\n"
    "    defaultModel { name ref }\n"
    "    selectableModels { name ref }\n"
    "    pinnedModel { name ref }\n"
    "  }\n"
    "}";
static const char *const ROOT_NS_QUERY = "query omp_gitlabDuoWorkflowProjectRootNamespace($fullPath: ID!) {\n"
                                         "  project(fullPath: $fullPath) {\n"
                                         "    namespace {\n"
                                         "      id\n"
                                         "      rootAncestor { id }\n"
                                         "    }\n"
                                         "  }\n"
                                         "}";

/* ======================================================================
 * per-session state (omp providerSessionState): the live socket between turns
 * ====================================================================== */

typedef struct {
    char *key;     /* baseUrl \0 model \0 session */
    bool busy;     /* a stream currently owns it */
    bool active;   /* a workflow socket is live */
    ws_t *ws;
    char *workflow_id;
    json_t *start_payload;
    json_t *pending;   /* array of {requestID, name, args} or NULL */
    json_t *by_key;    /* checkpoint agent content by message key */
    json_t *sigs;      /* content signatures */
    bool paused;
    json_t *pause_buffer; /* array of raw frames */
    int64_t last_boundary_len; /* -1 unknown */
    char *stop_base, *stop_key; /* for a best-effort stop of this workflow */
} wsess_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static VEC(wsess_t *) g_sessions;

typedef struct {
    uint64_t key; /* hash(apiKey \0 base \0 cwd) */
    char *root, *ns_path, *project_path, *source;
    bool have_ns;
    bool settings_ensured;
} account_t;
static VEC(account_t *) g_accounts;

/* ======================================================================
 * small helpers
 * ====================================================================== */

static char *rstr(const json_t *o, const char *k) { /* getRecordString: string or number -> owned */
    json_t *v = json_is_obj(o) ? json_get(o, k) : NULL;
    if (json_is_str(v)) return xstrdup(json_sv(v));
    if (json_is_num(v)) return json_dump(v, 0);
    return NULL;
}
static json_t *robj(const json_t *o, const char *k) { return json_is_obj(o) ? json_get_obj(o, k) : NULL; }
static char *nonempty(const char *s) { /* normalizeIdentifier-ish trim */
    if (!s) return NULL;
    char *t = str_trim(s);
    if (!*t) {
        free(t);
        return NULL;
    }
    return t;
}
static char *ident(const json_t *o, const char *k) {
    char *s = rstr(o, k), *t = nonempty(s);
    free(s);
    return t;
}
static const char *envs(const char *name) {
    const char *v = env_get(name);
    if (!v) return NULL;
    for (const char *p = v; *p; p++)
        if (!strchr(" \t\r\n", *p)) return v;
    return NULL;
}

static char *norm_base(const char *b) {
    char *r = xstrdup(b && *b ? b : DEFAULT_BASE);
    size_t n = strlen(r);
    while (n > 0 && r[n - 1] == '/') r[--n] = 0;
    if (!*r) {
        free(r);
        r = xstrdup(DEFAULT_BASE);
    }
    return r;
}

static bool all_digits(const char *s) {
    if (!s || !*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}
static char *to_graphql_ns(const char *id) { return all_digits(id) ? xasprintf("gid://gitlab/Group/%s", id) : xstrdup(id); }
static char *to_rest_ns(const char *id) {
    const char *p = NULL;
    if (str_starts(id, "gid://gitlab/Group/")) p = id + 19;
    else if (str_starts(id, "gid://gitlab/Namespace/")) p = id + 23;
    return p && all_digits(p) ? xstrdup(p) : xstrdup(id);
}

/* encodeURIComponent */
static char *enc(const char *s) {
    sb_t b = SB_INIT;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-_.!~*'()", *c))
            sb_putc(&b, (char)*c);
        else sb_appendf(&b, "%%%02X", *c);
    }
    return sb_detach(&b);
}
/* URLSearchParams serialization */
static void form(sb_t *b, const char *s) {
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("*-._", *c))
            sb_putc(b, (char)*c);
        else if (*c == ' ') sb_putc(b, '+');
        else sb_appendf(b, "%%%02X", *c);
    }
}

/* ======================================================================
 * REST
 * ====================================================================== */

typedef struct {
    const char *base; /* normalized GitLab base */
    const char *key;  /* GitLab credential */
    volatile sig_atomic_t *cancel;
} gl_t;

/* status (0 on transport failure) and parsed body (owned, may be NULL); *hdr_next = x-next-page (owned) */
static int gl_call(const gl_t *g, const char *method, const char *path, const json_t *body, json_t **out, char **next_page,
                   bool use_cancel) {
    char *raw = xasprintf("%s%s%s", g->base, path[0] == '/' ? "" : "/", path);
    char *url = ai_rewrite_url(raw);
    free(raw);
    http_headers_t h = {0};
    char *auth = xasprintf("Bearer %s", g->key);
    http_headers_set(&h, "Authorization", auth);
    free(auth);
    http_headers_set(&h, "content-type", "application/json");
    http_headers_set(&h, "Accept", "application/json");
    char *b = body ? json_dump(body, 0) : NULL;
    http_req_t req = {.method = method, .url = url, .headers = &h, .body = b, .body_len = b ? strlen(b) : 0,
                      .timeout_ms = REST_TIMEOUT_MS, .cancel = use_cancel ? g->cancel : NULL};
    http_resp_t r;
    int rc = http_request(&req, &r);
    http_headers_free(&h);
    free(b);
    free(url);
    int st = rc == 0 ? r.status : 0;
    if (out) *out = rc == 0 && r.body ? json_parse_str(r.body) : NULL;
    if (next_page) {
        const char *np = rc == 0 ? http_headers_get(&r.headers, "x-next-page") : NULL;
        *next_page = nonempty(np);
    }
    http_resp_free(&r);
    return st;
}
static bool ok(int st) { return st >= 200 && st < 300; }

static json_t *graphql(const gl_t *g, const char *query, json_t *vars) {
    json_t *b = json_obj();
    json_set_str(b, "query", query);
    json_set(b, "variables", vars);
    json_t *out = NULL;
    int st = gl_call(g, "POST", "/api/graphql", b, &out, NULL, true);
    json_free(b);
    if (!ok(st)) {
        json_free(out);
        return NULL;
    }
    return out;
}

static void stop_workflow(const char *base, const char *key, const char *id) {
    if (!id) return;
    gl_t g = {base, key, NULL};
    char *e = enc(id), *p = xasprintf("/api/v4/ai/duo_workflows/workflows/%s", e);
    json_t *b = json_parse_str("{\"status_event\":\"stop\"}");
    gl_call(&g, "PATCH", p, b, NULL, NULL, false);
    json_free(b);
    free(p);
    free(e);
}

/* ---------- namespace discovery (catalog discovery/gitlab-duo-workflow.ts) ---------- */

typedef struct {
    char *root, *ns_path, *project_path;
    const char *source;
} nssel_t;
static void nssel_free(nssel_t *s) {
    free(s->root);
    free(s->ns_path);
    free(s->project_path);
    memset(s, 0, sizeof *s);
}

static char *explicit_root(const json_t *v, int depth) {
    if (!json_is_obj(v) || depth > 8) return NULL;
    char *d = ident(v, "root_namespace_id");
    if (!d) d = ident(v, "rootNamespaceId");
    if (d) return d;
    static const char *const keys[] = {"root_namespace", "rootNamespace", "root_ancestor", "rootAncestor"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        json_t *rn = robj(v, keys[i]);
        if (rn) {
            char *r = ident(rn, "id");
            if (!r) r = ident(rn, "full_path");
            if (!r) r = ident(rn, "fullPath");
            return r;
        }
    }
    json_t *ns = robj(v, "namespace");
    return ns ? explicit_root(ns, depth + 1) : NULL;
}

static char *root_id(const json_t *v, int depth) {
    if (!json_is_obj(v) || depth > 8) return NULL;
    char *d = ident(v, "root_namespace_id");
    if (!d) d = ident(v, "rootNamespaceId");
    if (d) return d;
    static const char *const keys[] = {"root_namespace", "rootNamespace", "root_ancestor", "rootAncestor"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        json_t *rn = robj(v, keys[i]);
        if (rn) {
            char *r = ident(rn, "id");
            if (!r) r = ident(rn, "full_path");
            if (!r) r = ident(rn, "fullPath");
            if (r) return r;
            break;
        }
    }
    json_t *ns = robj(v, "namespace");
    if (ns) {
        char *r = root_id(ns, depth + 1);
        if (!r) r = ident(ns, "id");
        if (!r) r = ident(ns, "full_path");
        if (!r) r = ident(ns, "fullPath");
        return r;
    }
    char *r = ident(v, "id");
    if (!r) r = ident(v, "full_path");
    if (!r) r = ident(v, "fullPath");
    return r;
}

static char *ns_path_of(const json_t *v) {
    char *r = ident(v, "full_path");
    if (!r) r = ident(v, "fullPath");
    if (!r) r = ident(v, "path");
    return r;
}

static char *project_root_ns(const gl_t *g, const char *id_or_path) {
    char *e = enc(id_or_path), *p = xasprintf("/api/v4/projects/%s", e);
    json_t *pj = NULL;
    int st = gl_call(g, "GET", p, NULL, &pj, NULL, true);
    free(p);
    free(e);
    char *root = NULL, *full = NULL;
    if (ok(st) && json_is_obj(pj)) {
        root = explicit_root(pj, 0);
        full = ident(pj, "path_with_namespace");
        if (!full) full = ident(pj, "fullPath");
    }
    json_free(pj);
    if (root) {
        free(full);
        return root;
    }
    if (!full && strchr(id_or_path, '/')) full = xstrdup(id_or_path);
    if (!full) return NULL;
    json_t *vars = json_obj();
    json_set_str(vars, "fullPath", full);
    free(full);
    json_t *res = graphql(g, ROOT_NS_QUERY, vars);
    root = explicit_root(robj(robj(res, "data"), "project"), 0);
    json_free(res);
    return root;
}

/* .git/config of cwd or an ancestor (worktrees: commondir) */
static char *git_config_text(const char *start) {
    char *cur = realpath(start, NULL);
    if (!cur) cur = xstrdup(start);
    for (int guard = 0; guard < 256; guard++) {
        char *gp = xasprintf("%s/.git", cur);
        char *cfgp = xasprintf("%s/config", gp);
        size_t n;
        char *t = fs_read(cfgp, &n);
        free(cfgp);
        if (!t) {
            char *dotgit = fs_read(gp, &n);
            if (dotgit) {
                const char *gd = strstr(dotgit, "gitdir:");
                if (gd) {
                    char *line = xstrndup(gd + 7, strcspn(gd + 7, "\r\n")), *dir = str_trim(line);
                    char *gdp = dir[0] == '/' ? xstrdup(dir) : xasprintf("%s/%s", cur, dir);
                    char *cdf = xasprintf("%s/commondir", gdp);
                    char *cd = fs_read(cdf, &n);
                    if (cd) {
                        char *ct = str_trim(cd);
                        char *cdp = ct[0] == '/' ? xstrdup(ct) : xasprintf("%s/%s", gdp, ct);
                        char *cc = xasprintf("%s/config", cdp);
                        t = fs_read(cc, &n);
                        free(cc);
                        free(cdp);
                        free(ct);
                        free(cd);
                    }
                    if (!t) {
                        char *c2 = xasprintf("%s/config", gdp);
                        t = fs_read(c2, &n);
                        free(c2);
                    }
                    free(cdf);
                    free(gdp);
                    free(dir);
                    free(line);
                }
                free(dotgit);
            }
        }
        free(gp);
        if (t) {
            free(cur);
            return t;
        }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) {
            if (strcmp(cur, "/") == 0 || !slash) break;
            cur[1] = 0;
            continue;
        }
        *slash = 0;
    }
    free(cur);
    return NULL;
}

static char *url_host_port(const char *u, bool *ssh, char **path) {
    url_t p;
    if (url_parse(u, &p) != 0) return NULL;
    *ssh = str_eq(p.scheme, "ssh");
    bool dflt = p.port == (str_eq(p.scheme, "https") ? 443 : str_eq(p.scheme, "http") ? 80 : -1);
    const char *colon = strchr(u + strlen(p.scheme) + 3, '@') ? NULL : NULL;
    (void)colon;
    /* URL.host keeps an explicit non-default port */
    bool explicit_port = false;
    {
        const char *a = u + strlen(p.scheme) + 3;
        size_t al = strcspn(a, "/?#");
        const char *at = memchr(a, '@', al);
        if (at) {
            al -= (size_t)(at + 1 - a);
            a = at + 1;
        }
        const char *c = memchr(a, ':', al);
        explicit_port = c != NULL && a[0] != '[';
    }
    char *host = explicit_port && !dflt ? xasprintf("%s:%d", p.host, p.port) : xstrdup(p.host);
    if (path) *path = xstrdup(p.path);
    url_free(&p);
    return host;
}

static char *remote_project_path(const char *cwd, const char *base) {
    char *cfg = git_config_text(cwd);
    if (!cfg) return NULL;
    bool bssh;
    char *bpath = NULL;
    char *bhost = url_host_port(base, &bssh, &bpath);
    char *bp = bpath ? xstrdup(bpath + strspn(bpath, "/")) : xstrdup("");
    for (size_t n = strlen(bp); n > 0 && bp[n - 1] == '/';) bp[--n] = 0;
    free(bpath);
    char *result = NULL;
    strvec_t lines = str_split_lines(cfg, strlen(cfg));
    bool in_remote = false;
    for (size_t i = 0; i < lines.n && !result; i++) {
        char *l = str_trim(lines.v[i]);
        if (l[0] == '[') {
            char *rb = strchr(l, ']');
            char *sec = rb ? xstrndup(l + 1, (size_t)(rb - l - 1)) : xstrdup("");
            char *st = str_trim(sec);
            in_remote = str_starts(st, "remote \"") && str_ends(st, "\"") && strlen(st) > 9 && !strchr(st + 8, ' ');
            free(st);
            free(sec);
            free(l);
            continue;
        }
        if (in_remote && str_starts(l, "url")) {
            char *eq = strchr(l, '=');
            const char *pre = l + 3;
            while (*pre == ' ' || *pre == '\t') pre++;
            if (eq && pre == eq) {
                char *val = str_trim(eq + 1);
                char *host = NULL, *path = NULL;
                bool insensitive = false;
                if (strstr(val, "://")) {
                    bool ssh = false;
                    host = url_host_port(val, &ssh, &path);
                    insensitive = ssh;
                } else {
                    const char *at = strchr(val, '@');
                    const char *start = at ? at + 1 : val;
                    const char *c = strchr(start, ':');
                    if (c && c > start && c[1]) {
                        host = xstrndup(start, (size_t)(c - start));
                        path = xstrdup(c + 1);
                        insensitive = true;
                    }
                }
                bool match = host != NULL;
                if (host && bhost) {
                    if (!insensitive) match = strcasecmp(host, bhost) == 0;
                    else {
                        size_t hl = strcspn(host, ":"), bl = strcspn(bhost, ":");
                        match = hl == bl && strncasecmp(host, bhost, hl) == 0;
                    }
                }
                if (match && path) {
                    const char *pp = path + strspn(path, "/");
                    size_t bpl = strlen(bp);
                    if (bpl && (strcmp(pp, bp) == 0 || (str_starts(pp, bp) && pp[bpl] == '/'))) pp += bpl;
                    pp += strspn(pp, "/");
                    char *q = xstrdup(pp);
                    size_t n = strlen(q);
                    while (n > 0 && q[n - 1] == '/') q[--n] = 0;
                    if (n >= 4 && strcasecmp(q + n - 4, ".git") == 0) q[n - 4] = 0;
                    if (strchr(q, '/')) result = q;
                    else free(q);
                }
                free(host);
                free(path);
                free(val);
            }
        }
        free(l);
    }
    strvec_free(&lines);
    free(bp);
    free(bhost);
    free(cfg);
    return result;
}

static int resolve_namespace(const gl_t *g, const char *cwd, nssel_t *out, char **err) {
    memset(out, 0, sizeof *out);
    const char *ns = envs("GITLAB_DUO_NAMESPACE_ID");
    char *nst = nonempty(ns);
    if (nst) {
        char *rest = NULL;
        if (str_starts(nst, "gid://gitlab/")) {
            char *r = to_rest_ns(nst);
            if (all_digits(r)) rest = r;
            else free(r);
        } else if (all_digits(nst)) {
            rest = xstrdup(nst);
        }
        if (rest) {
            char *p = xasprintf("/api/v4/groups/%s", rest);
            json_t *gj = NULL;
            int st = gl_call(g, "GET", p, NULL, &gj, NULL, true);
            if (ok(st) && json_is_obj(gj)) {
                out->root = root_id(gj, 0);
                out->ns_path = ns_path_of(gj);
            }
            json_free(gj);
            free(p);
            free(rest);
        }
        if (!out->root) out->root = xstrdup(nst);
        out->source = "override";
        free(nst);
        return 0;
    }
    const char *pid = envs("GITLAB_DUO_PROJECT_ID");
    if (!pid) pid = envs("GITLAB_DUO_PROJECT_PATH");
    char *pt = nonempty(pid);
    if (pt) {
        char *root = project_root_ns(g, pt);
        if (root) {
            out->root = root;
            if (strchr(pt, '/')) out->project_path = xstrdup(pt);
            out->source = "project";
            free(pt);
            return 0;
        }
        free(pt);
    }
    char *remote = remote_project_path(cwd, g->base);
    if (remote) {
        char *root = project_root_ns(g, remote);
        if (root) {
            out->root = root;
            out->project_path = remote;
            out->source = "remote";
            return 0;
        }
        free(remote);
    }
    char *page = xstrdup("1");
    nssel_t pref = {0}, other = {0};
    for (int i = 0; i < MAX_GROUP_PAGES && page && !pref.root; i++) {
        char *p = xasprintf("/api/v4/groups?top_level_only=true&per_page=100&order_by=name&sort=asc&page=%s", page);
        free(page);
        page = NULL;
        json_t *arr = NULL;
        int st = gl_call(g, "GET", p, NULL, &arr, &page, true);
        free(p);
        if (!ok(st) || !json_is_arr(arr)) {
            json_free(arr);
            break;
        }
        json_t *grp;
        json_arr_foreach(arr, k, grp) {
            char *r = root_id(grp, 0);
            if (!r) continue;
            bool preferred = json_truthy(json_get(grp, "duo_features_enabled")) && json_is_bool(json_get(grp, "duo_features_enabled"));
            preferred = preferred || (json_is_bool(json_get(grp, "duo_core_features_enabled")) &&
                                      json_truthy(json_get(grp, "duo_core_features_enabled")));
            nssel_t *slot = preferred ? &pref : &other;
            if (!slot->root) {
                slot->root = r;
                slot->ns_path = ns_path_of(grp);
            } else {
                free(r);
            }
            if (pref.root) break;
        }
        json_free(arr);
    }
    free(page);
    nssel_t *pick = pref.root ? &pref : other.root ? &other : NULL;
    if (pick) {
        *out = *pick;
        out->source = "group";
        if (pick == &pref) nssel_free(&other);
        else nssel_free(&pref);
        return 0;
    }
    *err = xstrdup("GitLab Duo Workflow runtime namespace resolution failed: Unable to find a GitLab Duo Workflow namespace. Set "
                   "GITLAB_DUO_NAMESPACE_ID to a root namespace or GITLAB_DUO_PROJECT_ID to a GitLab project.");
    return -1;
}

/* ======================================================================
 * request building
 * ====================================================================== */

static char *content_text(const json_t *msg, bool assistant) {
    json_t *c = json_get(msg, "content");
    if (json_is_str(c)) return xstrdup(json_sv(c));
    sb_t b = SB_INIT;
    json_t *it;
    json_arr_foreach(c, i, it) {
        if (i) sb_putc(&b, '\n');
        const char *t = json_get_str(it, "type");
        if (str_eq(t, "text")) sb_append(&b, json_get_str(it, "text") ? json_get_str(it, "text") : "");
        else if (!assistant && str_eq(t, "image"))
            sb_appendf(&b, "[%s image]", json_get_str(it, "mimeType") ? json_get_str(it, "mimeType") : "");
    }
    return sb_detach(&b);
}


static char *latest_user_prompt(const json_t *msgs) {
    for (size_t i = json_len(msgs); i > 0; i--) {
        const json_t *m = json_at(msgs, i - 1);
        const char *r = json_get_str(m, "role");
        if (str_eq(r, "user") || str_eq(r, "developer")) return content_text(m, false);
    }
    return xstrdup("");
}

/* replay transcript: array of {role, content, calls?, isError, tool} */
static json_t *history(const json_t *msgs) {
    json_t *out = json_arr();
    json_t *m;
    json_arr_foreach(msgs, i, m) {
        const char *role = json_get_str(m, "role");
        json_t *e = json_obj();
        if (str_eq(role, "toolResult")) {
            json_set_str(e, "role", "tool");
            json_set(e, "content", json_str_own(content_text(m, false)));
            json_set_bool(e, "hdr", json_get_str(m, "toolCallId") || json_get_str(m, "toolName"));
            json_set_bool(e, "isError", json_get_bool(m, "isError", false));
        } else if (str_eq(role, "assistant")) {
            char *txt = content_text(m, true);
            json_t *calls = json_arr(), *blk;
            json_arr_foreach(json_get(m, "content"), k, blk) {
                if (!str_eq(json_get_str(blk, "type"), "toolCall")) continue;
                json_t *c = json_obj();
                json_set_str(c, "name", json_get_str(blk, "name") ? json_get_str(blk, "name") : "");
                json_t *args = json_is_obj(json_get(blk, "arguments")) ? json_clone(json_get(blk, "arguments")) : json_obj();
                json_del(args, "i");
                json_set(c, "arguments", args);
                json_push(calls, c);
            }
            if (!*txt && json_len(calls) == 0) {
                free(txt);
                json_free(calls);
                json_free(e);
                continue;
            }
            json_set_str(e, "role", "assistant");
            json_set(e, "content", json_str_own(txt));
            if (json_len(calls)) json_set(e, "calls", calls);
            else json_free(calls);
        } else {
            char *txt = content_text(m, false);
            if (!*txt) {
                free(txt);
                json_free(e);
                continue;
            }
            json_set_str(e, "role", "user");
            json_set(e, "content", json_str_own(txt));
        }
        json_push(out, e);
    }
    return out;
}

static char *render_chatml(const json_t *hist) {
    sb_t b = SB_INIT;
    json_t *e;
    json_arr_foreach(hist, i, e) {
        if (i) sb_putc(&b, '\n');
        const char *role = json_get_str(e, "role"), *content = json_get_str(e, "content");
        sb_appendf(&b, "<|im_start|>%s\n", role);
        if (str_eq(role, "tool")) {
            if (json_get_bool(e, "hdr", false))
                sb_appendf(&b, "<ran:result%s>\n", json_get_bool(e, "isError", false) ? " status=error" : "");
            sb_appendf(&b, "%s\n", content);
        } else {
            bool any = false;
            if (*content) {
                sb_append(&b, content);
                any = true;
            }
            json_t *c;
            json_arr_foreach(json_get(e, "calls"), k, c) {
                if (any) sb_putc(&b, '\n');
                char *a = json_dump(json_get(c, "arguments"), 0);
                sb_appendf(&b, "<ran %s>%s</ran>", json_get_str(c, "name"), a);
                free(a);
                any = true;
            }
            sb_putc(&b, '\n');
        }
        sb_append(&b, "<|im_end|>");
    }
    return sb_detach(&b);
}

static char *build_goal(const json_t *msgs, bool *chatml) {
    json_t *h = history(msgs);
    *chatml = json_len(h) > 1;
    char *g = *chatml ? render_chatml(h) : latest_user_prompt(msgs);
    json_free(h);
    return g;
}

/* ---------- flow prompt sanitization (sanitizeGitLabDuoWorkflowFlowPrompt) ---------- */

/* JS /\s/u plus \x1c-\x1f and U+0085; returns bytes of the whitespace char at p, 0 if none */
static int ws_at(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f)) return 1;
    if (c < 0x80) return 0;
    uint32_t cp;
    int n = utf8_decode(p, strlen(p), &cp);
    if (cp == 0x85 || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202f || cp == 0x205f || cp == 0x3000 || cp == 0xfeff)
        return n;
    return 0;
}
static const char *skip_ws(const char *p) {
    int n;
    while ((n = ws_at(p))) p += n;
    return p;
}
/* case-insensitive tag name with s~ſ and i~ı/İ; returns end or NULL */
static const char *match_name(const char *p, const char *name) {
    for (const char *n = name; *n; n++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) {
            if (tolower(c) != *n) return NULL;
            p++;
            continue;
        }
        uint32_t cp;
        int k = utf8_decode(p, strlen(p), &cp);
        if ((*n == 's' && cp == 0x17f) || (*n == 'i' && (cp == 0x131 || cp == 0x130))) p += k;
        else return NULL;
    }
    return p;
}
static const char *match_esc(const char *p, const char *what) { /* \\{1,2}u003c / u003e */
    if (*p != '\\') return NULL;
    p++;
    if (*p == '\\') p++;
    if (strncasecmp(p, what, 5) != 0) return NULL;
    return p + 5;
}

static const char *const TAGS[] = {"goal", "system", "truncated_tool_output", "truncation_notice"};

/* (^|\\n|\n)\s*\[(?://|comment)\]:\s*(?:<[^>]*>|#|\S*)\s*\((?:[^)\\]|\\.)*\) at p (prefix already consumed);
 * returns end or NULL */
static const char *md_comment_at(const char *p) {
    p = skip_ws(p);
    if (strncmp(p, "[//]:", 5) == 0) p += 5;
    else if (strncasecmp(p, "[comment]:", 10) == 0) p += 10;
    else return NULL;
    p = skip_ws(p);
    /* candidate positions after the middle group, most greedy first */
    const char *cands[3];
    int nc = 0;
    if (*p == '<') {
        const char *gt = strchr(p, '>');
        if (gt) cands[nc++] = gt + 1;
    }
    if (*p == '#') cands[nc++] = p + 1;
    const char *s_end = p;
    while (*s_end && !ws_at(s_end)) s_end++;
    for (int ci = 0; ci < nc; ci++) {
        const char *q = skip_ws(cands[ci]);
        if (*q != '(') continue;
        q++;
        while (*q && *q != ')') {
            if (*q == '\\' && q[1]) q += 2;
            else q++;
        }
        if (*q == ')') return q + 1;
    }
    for (const char *e = s_end; e >= p; e--) {
        const char *q = skip_ws(e);
        if (*q == '(') {
            q++;
            while (*q && *q != ')') {
                if (*q == '\\' && q[1]) q += 2;
                else q++;
            }
            if (*q == ')') return q + 1;
        }
        if (e == p) break;
    }
    return NULL;
}

static char *sanitize_pass(const char *t) {
    /* 1. unicode tag / variation selector supplement code points */
    sb_t a = SB_INIT;
    for (const char *p = t; *p;) {
        uint32_t cp;
        int n = utf8_decode(p, strlen(p), &cp);
        if (!((cp >= 0xE0000 && cp <= 0xE007F) || (cp >= 0xE0100 && cp <= 0xE01EF))) sb_appendn(&a, p, (size_t)n);
        p += n;
    }
    /* 2. escaped surrogate text \udb40\udcXX..\udfXX */
    sb_t b = SB_INIT;
    for (const char *p = a.s ? a.s : ""; *p;) {
        if (strncasecmp(p, "\\udb40\\ud", 9) == 0 && strchr("cdefCDEF", p[9]) && p[9] && isxdigit((unsigned char)p[10]) &&
            isxdigit((unsigned char)p[11])) {
            p += 12;
            continue;
        }
        sb_putc(&b, *p++);
    }
    sb_free(&a);
    /* 3. <!-- ... --> removed */
    sb_t c = SB_INIT;
    for (const char *p = b.s ? b.s : ""; *p;) {
        if (strncmp(p, "<!--", 4) == 0) {
            const char *e = strstr(p + 4, "-->");
            if (e) {
                p = e + 3;
                continue;
            }
        }
        sb_putc(&c, *p++);
    }
    sb_free(&b);
    /* 4. \\+u003c!-- | <!-- -> &lt;!-- */
    sb_t d = SB_INIT;
    for (const char *p = c.s ? c.s : ""; *p;) {
        if (*p == '\\') {
            const char *q = p;
            while (*q == '\\') q++;
            if (strncasecmp(q, "u003c!--", 8) == 0) {
                sb_append(&d, "&lt;!--");
                p = q + 8;
                continue;
            }
        }
        if (strncasecmp(p, "<!--", 4) == 0) {
            sb_append(&d, "&lt;!--");
            p += 4;
            continue;
        }
        sb_putc(&d, *p++);
    }
    sb_free(&c);
    char *cur = sb_detach(&d);
    /* 5. markdown comments */
    bool has_marker = false;
    for (const char *p = cur; *p && !has_marker; p++)
        if (*p == '[' && (strncmp(p, "[//]:", 5) == 0 || strncasecmp(p, "[comment]:", 10) == 0)) has_marker = true;
    if (has_marker) {
        sb_t e = SB_INIT;
        const char *p = cur;
        bool at_start = true;
        while (*p) {
            const char *pre_end = NULL;
            if (at_start) pre_end = p;
            else if (*p == '\n') pre_end = p + 1;
            else if (p[0] == '\\' && p[1] == 'n') pre_end = p + 2;
            at_start = false;
            if (pre_end) {
                const char *end = md_comment_at(pre_end);
                if (end) {
                    sb_appendn(&e, p, (size_t)(pre_end - p));
                    p = end;
                    continue;
                }
            }
            sb_putc(&e, *p++);
        }
        free(cur);
        cur = sb_detach(&e);
        sb_t f = SB_INIT;
        for (p = cur; *p;) {
            if (strncmp(p, "[//]:", 5) == 0) {
                sb_append(&f, "[//]\u200b:");
                p += 5;
            } else if (strncasecmp(p, "[comment]:", 10) == 0) {
                sb_putc(&f, '[');
                sb_appendn(&f, p + 1, 7);
                sb_append(&f, "]\u200b:");
                p += 10;
            } else {
                sb_putc(&f, *p++);
            }
        }
        free(cur);
        cur = sb_detach(&f);
    }
    /* 6. dangerous tags, plain then escaped, per tag */
    for (size_t ti = 0; ti < ARRAY_LEN(TAGS); ti++) {
        for (int esc = 0; esc < 2; esc++) {
            sb_t g = SB_INIT;
            for (const char *p = cur; *p;) {
                const char *q = esc ? match_esc(p, "u003c") : (*p == '<' ? p + 1 : NULL);
                if (q) {
                    q = skip_ws(q);
                    bool slash = *q == '/';
                    if (slash) q++;
                    q = skip_ws(q);
                    const char *ne = match_name(q, TAGS[ti]);
                    if (ne) {
                        ne = skip_ws(ne);
                        const char *close = esc ? match_esc(ne, "u003e") : (*ne == '>' ? ne + 1 : NULL);
                        if (close) {
                            sb_appendf(&g, "&lt;%s%s&gt;", slash ? "/" : "", TAGS[ti]);
                            p = close;
                            continue;
                        }
                    }
                }
                sb_putc(&g, *p++);
            }
            free(cur);
            cur = sb_detach(&g);
        }
    }
    return cur;
}

static char *sanitize_flow_prompt(const char *prompt, char **err) {
    char *text = xstrdup(prompt);
    size_t max = strlen(text) + 2;
    for (size_t pass = 0; pass < max; pass++) {
        char *next = sanitize_pass(text);
        if (strcmp(next, text) == 0) {
            free(next);
            return text;
        }
        free(text);
        text = next;
    }
    free(text);
    *err = xstrdup("GitLab Duo flow prompt sanitization did not converge");
    return NULL;
}

static json_t *build_flow_config(const char *system_prompt, char **err) {
    char *sp = sanitize_flow_prompt(system_prompt, err);
    if (!sp) return NULL;
    json_t *f = json_parse_str(
        "{\"version\":\"v1\",\"environment\":\"ambient\",\"flow\":{\"entry_point\":\"" INLINE_AGENT "\"},"
        "\"components\":[{\"name\":\"" INLINE_AGENT "\",\"type\":\"AgentComponent\",\"prompt_id\":\"" INLINE_PROMPT "\","
        "\"toolset\":[],\"inputs\":[{\"from\":\"context:goal\",\"as\":\"goal\"}],\"ui_log_events\":[\"on_agent_reasoning\","
        "\"on_agent_final_answer\",\"on_tool_execution_success\",\"on_tool_execution_failed\"]}],"
        "\"routers\":[{\"from\":\"" INLINE_AGENT "\",\"to\":\"end\"}],"
        "\"prompts\":[{\"name\":\"" INLINE_PROMPT "\",\"prompt_id\":\"" INLINE_PROMPT "\",\"unit_primitives\":[\"duo_agent_platform\"],"
        "\"prompt_template\":{\"system\":\"\",\"user\":\"{{goal}}\",\"placeholder\":\"history\"}}]}");
    json_set(json_get_obj(json_at(json_get(f, "prompts"), 0), "prompt_template"), "system", json_str_own(sp));
    return f;
}

static char *system_prompt(const ai_context_t *ctx, bool chatml) {
    sb_t b = SB_INIT;
    json_t *s;
    size_t k = 0;
    json_arr_foreach(ctx->system_prompt, i, s) {
        if (!json_is_str(s) || !*json_sv(s)) continue;
        if (k++) sb_append(&b, "\n\n");
        sb_append(&b, json_sv(s));
    }
    if (chatml) {
        size_t n;
        const char *note = embed_get("prompts/ai/providers/gitlab-duo-workflow-chatml-note.md", &n);
        char *t = note ? str_trim(note) : xstrdup("");
        if (b.len) sb_append(&b, "\n\n");
        sb_append(&b, t);
        free(t);
    }
    return sb_detach(&b);
}

static json_t *mcp_tools(const json_t *tools) {
    json_t *out = json_arr(), *t;
    json_arr_foreach(tools, i, t) {
        const char *name = json_get_str(t, "name");
        if (!name) continue;
        json_t *d = json_obj();
        json_set_str(d, "name", name);
        json_set_str(d, "originalToolName", name);
        json_set_str(d, "serverName", "omp");
        json_set_str(d, "description", json_get_str(t, "description") ? json_get_str(t, "description") : "");
        json_t *p = json_get(t, "parameters");
        json_set(d, "inputSchema",
                 json_str_own(json_is_obj(p) ? json_dump(p, 0) : xstrdup("{\"type\":\"object\",\"properties\":{},\"required\":[]}")));
        json_set_bool(d, "isApproved", true);
        json_push(out, d);
    }
    return out;
}

static json_t *build_start(const char *workflow_id, const char *definition, const char *goal, const char *project_id,
                           const char *ns_rest, const char *selected, const ai_context_t *ctx, bool advertise, bool chatml,
                           char **err) {
    char *sp = system_prompt(ctx, chatml);
    json_t *flow = build_flow_config(sp, err);
    free(sp);
    if (!flow) return NULL;
    json_t *md = json_obj();
    json_set_str(md, "environment", "ide");
    json_set_str(md, "client_type", CLIENT_TYPE);
    if (project_id) json_set_str(md, "projectId", project_id);
    if (ns_rest) {
        json_set_str(md, "namespaceId", ns_rest);
        json_set_str(md, "rootNamespaceId", ns_rest);
    }
    json_set_str(md, "selectedModelIdentifier", selected);
    json_t *s = json_obj();
    json_set_str(s, "workflowID", workflow_id);
    json_set_str(s, "clientVersion", "1.0");
    json_set_str(s, "workflowDefinition", definition);
    json_set_str(s, "goal", goal);
    json_set(s, "workflowMetadata", json_str_own(json_dump(md, 0)));
    json_free(md);
    json_set(s, "additional_context", json_arr());
    json_set(s, "clientCapabilities",
             json_parse_str("[\"incremental_streaming\",\"read_file_chunked\",\"shell_command\",\"command_timeout\",\"tool_call_approval\"]"));
    json_t *tools = mcp_tools(advertise ? ctx->tools : NULL);
    json_t *pre = json_arr(), *t;
    json_arr_foreach(tools, i, t) json_push_str(pre, json_get_str(t, "name"));
    json_set(s, "mcpTools", tools);
    json_set(s, "preapproved_tools", pre);
    json_set_str(s, "flowConfigSchemaVersion", "v1");
    json_set(s, "flowConfig", flow);
    return s;
}

/* ======================================================================
 * stream state
 * ====================================================================== */

typedef struct {
    ai_builder_t b;
    json_t *final;   /* finished message (owned) once done */
    wsess_t *sess;   /* slot (may be a private, non-resumable one) */
    char *active_key; /* message key of the open checkpoint segment */
    bool pause_requested;
    int64_t last_cp_len; /* -1 */
    char *last_approval;
    char *goal_overflow;
    char *stashed_error; /* retryable FAILED message */
    volatile sig_atomic_t *cancel;
} st_t;

static bool done(const st_t *s) { return s->final != NULL; }
static void finish(st_t *s, const char *reason) {
    if (done(s)) return;
    s->final = aib_finish(&s->b, reason, NULL);
}
static void finish_error(st_t *s, const char *msg) {
    if (done(s)) return;
    if (s->cancel && *s->cancel) s->final = aib_finish(&s->b, "aborted", "Request was aborted");
    else s->final = aib_finish(&s->b, "error", msg);
}

static void sess_drop_socket(wsess_t *w) {
    if (w->ws) ws_close(w->ws, 1000, NULL);
    w->ws = NULL;
    w->active = false;
    free(w->workflow_id);
    w->workflow_id = NULL;
    json_free(w->start_payload);
    w->start_payload = NULL;
    json_free(w->pending);
    w->pending = NULL;
    w->paused = false;
    json_free(w->pause_buffer);
    w->pause_buffer = NULL;
    w->last_boundary_len = -1;
}

/* ---------- checkpoints ---------- */

typedef struct {
    int kind; /* 0 boundary, 1 text, 2 thinking */
    char *key;
    char *content;
} cpe_t;

typedef struct {
    VEC(cpe_t) entries;
    int64_t len;
    bool has_usage;
    int64_t used;
} cp_t;

static void cp_free(cp_t *c) {
    for (size_t i = 0; i < c->entries.n; i++) {
        free(c->entries.v[i].key);
        free(c->entries.v[i].content);
    }
    vec_free(&c->entries);
}

static bool usage_of(const json_t *v, int64_t *used) {
    if (!json_is_obj(v)) return false;
    json_t *t = json_get(v, "total_tokens"), *m = json_get(v, "max_tokens");
    if (!json_is_num(t) || !json_is_num(m)) return false;
    double td = t->is_int ? (double)t->u.num.i : t->u.num.d, md = m->is_int ? (double)m->u.num.i : m->u.num.d;
    if (td < 0 || md <= 0) return false;
    *used = (int64_t)td;
    return true;
}

static bool context_usage(int64_t *used, const json_t *a, const json_t *b, const json_t *c) {
    const json_t *src[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        json_t *map = robj(src[i], "agent_context_usage");
        if (!map) continue;
        if (usage_of(json_get(map, "Chat Agent"), used) || usage_of(json_get(map, "context_builder"), used)) return true;
        const char *k;
        json_t *v;
        json_obj_foreach(map, j, k, v) if (usage_of(v, used)) return true;
    }
    return false;
}

static bool extract_checkpoint(const json_t *ev, cp_t *out) {
    memset(out, 0, sizeof *out);
    json_t *action = robj(ev, "action");
    json_t *cp = robj(action, "newCheckpoint");
    if (!cp) cp = robj(ev, "newCheckpoint");
    if (!cp) cp = robj(ev, "checkpoint");
    if (!cp) return false;
    out->has_usage = context_usage(&out->used, ev, action, cp);
    char *direct = rstr(cp, "message");
    if (!direct) direct = rstr(cp, "text");
    if (!direct) direct = rstr(cp, "content");
    if (!direct) direct = rstr(robj(cp, "checkpoint"), "message");
    if (!direct) direct = rstr(robj(cp, "checkpoint"), "text");
    if (direct && *direct) {
        cpe_t e = {1, xstrdup("direct:text"), direct};
        out->len = (int64_t)strlen(direct);
        vec_push(&out->entries, e);
        return true;
    }
    free(direct);
    const char *cj = json_get_str(cp, "checkpoint");
    json_t *parsed = cj ? json_parse_str(cj) : NULL;
    json_t *log = json_get_arr(robj(parsed, "channel_values"), "ui_chat_log");
    if (log) {
        json_t *ent;
        json_arr_foreach(log, i, ent) {
            if (!json_is_obj(ent)) continue;
            char *mt = rstr(ent, "message_type");
            if (str_eq(mt, "agent")) {
                char *content = rstr(ent, "content");
                if (content && *content) {
                    char *mid = rstr(ent, "message_id"), *sub = rstr(ent, "message_sub_type");
                    bool reasoning = str_eq(sub, "reasoning");
                    char *key = mid && *mid ? xasprintf("agent:%s", mid) : xasprintf("%s:%zu", reasoning ? "reasoning" : "agent", i);
                    cpe_t e = {reasoning ? 2 : 1, key, content};
                    vec_push(&out->entries, e);
                    content = NULL;
                    free(mid);
                    free(sub);
                }
                free(content);
            } else if (str_eq(mt, "request") || str_eq(mt, "tool")) {
                cpe_t e = {0, NULL, NULL};
                vec_push(&out->entries, e);
            }
            free(mt);
        }
        out->len = (int64_t)strlen(cj);
        json_free(parsed);
        return true;
    }
    json_free(parsed);
    if (out->has_usage) return true;
    return false;
}

static void end_blocks(st_t *s) { aib_end_block(&s->b); }

static void emit_checkpoint(st_t *s, const cp_t *cp) {
    wsess_t *w = s->sess;
    if (cp->has_usage) aib_usage(&s->b, cp->used, 0, 0, 0);
    s->last_cp_len = cp->len;
    bool delta_here = false;
    int turn = 0;
    if (!w->by_key) w->by_key = json_obj();
    if (!w->sigs) w->sigs = json_obj();
    for (size_t i = 0; i < cp->entries.n; i++) {
        const cpe_t *e = &cp->entries.v[i];
        if (e->kind == 0) {
            if (delta_here && w->active) {
                s->pause_requested = true;
                return;
            }
            end_blocks(s);
            turn++;
            continue;
        }
        const char *prev = json_get_str(w->by_key, e->key);
        uint64_t h = hash_xx64(e->content, strlen(e->content), 0);
        char *sig = xasprintf("%d\x1f%s\x1f%016llx", turn, e->kind == 2 ? "thinking" : "text", (unsigned long long)h);
        char *sig2 = xasprintf("%d\x1f" "content\x1f%016llx", turn, (unsigned long long)h);
        bool dup = !prev && (json_get(w->sigs, sig) || json_get(w->sigs, sig2));
        bool rewrote = prev && !str_starts(e->content, prev) && strcmp(prev, e->content) != 0;
        size_t pl = prev ? strlen(prev) : 0;
        const char *delta = dup || rewrote ? "" : prev ? e->content + pl : e->content;
        char *delta_c = xstrdup(delta);
        bool was_new = prev == NULL;
        json_set_str(w->by_key, e->key, e->content);
        json_set_bool(w->sigs, sig, true);
        json_set_bool(w->sigs, sig2, true);
        free(sig);
        free(sig2);
        if (*delta_c) {
            if (s->active_key && strcmp(s->active_key, e->key) != 0 && was_new) end_blocks(s);
            if (e->kind == 2) aib_thinking_delta(&s->b, delta_c, strlen(delta_c));
            else aib_text_delta(&s->b, delta_c, strlen(delta_c));
            free(s->active_key);
            s->active_key = xstrdup(e->key);
            delta_here = true;
        }
        free(delta_c);
    }
}

/* ---------- actions ---------- */

static json_t *extract_action(const json_t *ev, char **err) {
    json_t *wrapped = robj(ev, "action");
    if (!wrapped) wrapped = robj(ev, "workflowAction");
    if (!wrapped) wrapped = robj(ev, "toolCall");
    char *name = NULL, *rid = NULL;
    json_t *args = NULL;
    const json_t *src = NULL;
    if (wrapped) {
        if (robj(wrapped, "newCheckpoint")) return NULL;
        name = rstr(wrapped, "name");
        if (!name) name = rstr(wrapped, "action");
        if (!name) name = rstr(wrapped, "type");
        if (!name) name = rstr(ev, "actionName");
        if (!name) return NULL;
        rid = rstr(wrapped, "requestID");
        if (!rid) rid = rstr(wrapped, "requestId");
        if (!rid) rid = rstr(wrapped, "id");
        if (!rid) rid = rstr(ev, "requestID");
        if (!rid) rid = rstr(ev, "requestId");
        args = robj(wrapped, "args");
        if (!args) args = robj(wrapped, "arguments");
        if (!args) args = wrapped;
        src = wrapped;
    } else {
        static const char *const names[] = {"runMCPTool", "run_mcp_tool"};
        for (size_t i = 0; i < 2 && !name; i++) {
            json_t *a = robj(ev, names[i]);
            if (a) {
                name = xstrdup(names[i]);
                args = a;
                rid = rstr(ev, "requestID");
                if (!rid) rid = rstr(ev, "requestId");
                src = ev;
            }
        }
        if (!name) return NULL;
    }
    if (!rid || !*rid) {
        sb_t keys = SB_INIT;
        const char *k;
        json_t *v;
        size_t n = 0;
        json_obj_foreach(src, i, k, v) {
            if (n++ >= 20) break;
            if (keys.len) sb_append(&keys, ", ");
            sb_append(&keys, k);
        }
        *err = xasprintf("GitLab Duo Workflow action \"%s\" missing requestID (keys: %s)", name, sb_cstr(&keys));
        sb_free(&keys);
        free(name);
        free(rid);
        return NULL;
    }
    json_t *a = json_clone(args);
    if (!json_get_str(a, "toolCallId") && !json_get_str(a, "tool_call_id")) {
        json_set_str(a, "toolCallId", rid);
        json_set_str(a, "tool_call_id", rid);
    }
    json_t *d = json_obj();
    json_set(d, "requestID", json_str_own(rid));
    json_set(d, "name", json_str_own(name));
    json_set(d, "args", a);
    return d;
}

static void emit_tool_call(st_t *s, const json_t *action) {
    const char *aname = json_get_str(action, "name");
    json_t *args = json_get(action, "args");
    char *tname = NULL;
    json_t *targs = NULL;
    if (str_eq(aname, "runMCPTool") || str_eq(aname, "run_mcp_tool")) {
        char *raw = nonempty(json_get_str(args, "toolName"));
        if (!raw) raw = nonempty(json_get_str(args, "tool_name"));
        if (!raw) raw = nonempty(json_get_str(args, "name"));
        if (!raw) raw = xstrdup("");
        tname = xstrdup(str_starts(raw, "mcp__omp__") ? raw + 10 : raw);
        free(raw);
        json_t *v = json_get(args, "args");
        if (!v) v = json_get(args, "arguments");
        if (json_is_str(v)) {
            json_t *p = json_parse_str(json_sv(v));
            if (!p) p = json_parse_partial(json_sv(v), strlen(json_sv(v)));
            targs = json_is_obj(p) ? p : (json_free(p), json_obj());
        } else {
            targs = json_is_obj(v) ? json_clone(v) : json_obj();
        }
        if (str_eq(tname, "edit") && json_is_str(json_get(targs, "input"))) {
            json_t *only = json_obj();
            json_set_str(only, "input", json_get_str(targs, "input"));
            json_free(targs);
            targs = only;
        }
    } else {
        tname = xstrdup(aname ? aname : "");
        targs = json_is_obj(args) ? json_clone(args) : json_obj();
    }
    end_blocks(s);
    aib_toolcall_start(&s->b, json_get_str(action, "requestID"), tname);
    char *js = json_dump(targs, 0);
    aib_toolcall_delta(&s->b, js, strlen(js));
    free(js);
    aib_toolcall_set_args(&s->b, targs);
    finish(s, "toolUse");
    if (s->sess->active) {
        json_free(s->sess->pending);
        s->sess->pending = json_arr();
        json_push(s->sess->pending, json_clone(action));
    }
    free(tname);
}

static bool stall_detected(st_t *s) {
    wsess_t *w = s->sess;
    if (!w->active || s->last_cp_len < 0) return false;
    bool stalled = w->last_boundary_len >= 0 && s->last_cp_len == w->last_boundary_len;
    w->last_boundary_len = s->last_cp_len;
    return stalled;
}

/* handleGitLabDuoWorkflowSocketMessage */
static int handle_message(st_t *s, const char *data, size_t len, char **err) {
    json_t *ev = json_parse(data, len, NULL);
    if (!json_is_obj(ev) && !json_is_arr(ev)) {
        json_free(ev);
        return R_CONTINUE;
    }
    char *status = rstr(ev, "status");
    if (!status) status = rstr(robj(ev, "workflowStatus"), "status");
    if (!status) status = rstr(robj(ev, "newCheckpoint"), "status");
    cp_t cp;
    if (extract_checkpoint(ev, &cp)) {
        emit_checkpoint(s, &cp);
        cp_free(&cp);
    }
    int r = R_CONTINUE;
    if (s->pause_requested) {
        s->pause_requested = false;
        r = R_PAUSE;
    } else if (str_eq(status, "PLAN_APPROVAL_REQUIRED") || str_eq(status, "TOOL_CALL_APPROVAL_REQUIRED")) {
        free(s->last_approval);
        s->last_approval = xstrdup(status);
        r = R_APPROVAL;
    } else if (str_eq(status, "INPUT_REQUIRED") || str_eq(status, "FINISHED")) {
        finish(s, "stop");
        r = R_TERMINAL;
    } else if (str_eq(status, "FAILED") || str_eq(status, "STOPPED")) {
        char *msg = rstr(ev, "error");
        if (!msg) msg = rstr(ev, "message");
        if (!msg) msg = xstrdup(status);
        char *low = str_lower(msg);
        if (str_eq(status, "FAILED") && strstr(low, "reached its maximum step limit")) {
            r = R_STEP_LIMIT;
        } else if (str_eq(status, "FAILED") && strstr(low, "error processing your request in the duo agent platform")) {
            free(s->stashed_error);
            s->stashed_error = xstrdup(msg);
            r = R_RETRYABLE;
        } else {
            finish_error(s, s->goal_overflow ? s->goal_overflow : msg);
            r = R_TERMINAL;
        }
        free(low);
        free(msg);
    } else {
        char *aerr = NULL;
        json_t *action = json_is_obj(ev) ? extract_action(ev, &aerr) : NULL;
        if (aerr) {
            *err = aerr;
            r = R_ERROR;
        } else if (action) {
            if (stall_detected(s)) r = R_STALLED;
            else {
                emit_tool_call(s, action);
                r = R_ACTION;
            }
            json_free(action);
        }
    }
    free(status);
    json_free(ev);
    return r;
}

/* runGitLabDuoWorkflowSocket: send the start / resume, then read until the turn settles */
static int run_socket(st_t *s, const json_t *start, const json_t *responses, json_t *replay, char **err) {
    wsess_t *w = s->sess;
    ws_t *ws = w->ws;
    if (replay && json_len(replay) > 0) {
        w->paused = true;
        for (size_t i = 0; i < json_len(replay); i++) {
            const char *d = json_sv(json_at(replay, i));
            if (!d) continue;
            int r = handle_message(s, d, strlen(d), err);
            if (r == R_ERROR) return R_ERROR;
            if (r == R_PAUSE) {
                json_t *buf = json_arr();
                for (size_t k = i; k < json_len(replay); k++) json_push(buf, json_clone(json_at(replay, k)));
                json_free(w->pause_buffer);
                w->pause_buffer = buf;
                aib_set(&s->b, "stopDetails", json_parse_str("{\"type\":\"pause_turn\"}"));
                finish(s, "stop");
                return R_PAUSE;
            }
            if (r == R_ACTION) {
                w->paused = false;
                return R_ACTION;
            }
            if (r != R_CONTINUE) {
                ws_close(w->ws, 1000, NULL);
                w->ws = NULL;
                return r;
            }
        }
        w->paused = false;
    } else if (responses) {
        json_t *rsp;
        json_arr_foreach(responses, i, rsp) {
            char *t = json_dump(rsp, 0);
            int rc = ws_send_text(ws, t, strlen(t));
            free(t);
            if (rc != 0) {
                *err = xstrdup("GitLab Duo Workflow WebSocket error: send failed");
                return R_ERROR;
            }
        }
    } else {
        json_t *m = json_obj();
        json_set(m, "startRequest", json_clone(start));
        char *t = json_dump(m, 0);
        json_free(m);
        int rc = ws_send_text(ws, t, strlen(t));
        free(t);
        if (rc != 0) {
            *err = xstrdup("GitLab Duo Workflow WebSocket error: send failed");
            return R_ERROR;
        }
    }
    const char *it = env_get("OMC_GITLAB_DUO_IDLE_TIMEOUT_MS"); /* test hook */
    int64_t idle = it && atoll(it) > 0 ? atoll(it) : IDLE_TIMEOUT_MS;
    for (;;) {
        ws_msg_t m;
        char *rerr = NULL;
        int rc = ws_recv(w->ws, idle, s->cancel, &m, &rerr);
        if (rc == -2) {
            free(rerr);
            ws_close(w->ws, 1000, NULL);
            w->ws = NULL;
            return R_TIMEOUT;
        }
        if (rc == -4) {
            ws_close(w->ws, 1000, NULL);
            w->ws = NULL;
            *err = xstrdup("GitLab Duo Workflow request aborted");
            return R_ERROR;
        }
        if (rc != 1) {
            *err = xasprintf("GitLab Duo Workflow WebSocket error: %s", rerr ? rerr : "read failed");
            free(rerr);
            return R_ERROR;
        }
        if (m.opcode == WS_OP_CLOSE) {
            int code = m.close_code;
            char *reason = m.close_reason ? xstrdup(m.close_reason) : NULL;
            ws_msg_free(&m);
            if (code >= 4000 && code < 5000) {
                int hs = code - 4000;
                *err = hs >= 100 && hs <= 599
                           ? xasprintf("GitLab Duo Workflow closed the session with HTTP %d (WebSocket %d)%s%s", hs, code,
                                       reason ? ": " : "", reason ? reason : "")
                           : xasprintf("GitLab Duo Workflow closed the session (WebSocket %d)%s%s", code, reason ? ": " : "",
                                       reason ? reason : "");
                free(reason);
                return R_ERROR;
            }
            free(reason);
            ws_close(w->ws, 0, NULL);
            w->ws = NULL;
            return s->last_approval ? R_APPROVAL : R_CLOSED;
        }
        int r = handle_message(s, m.data, m.len, err);
        if (r == R_PAUSE) {
            json_free(w->pause_buffer);
            w->pause_buffer = json_arr();
            json_push(w->pause_buffer, json_strn(m.data, m.len));
            aib_set(&s->b, "stopDetails", json_parse_str("{\"type\":\"pause_turn\"}"));
            finish(s, "stop");
            w->paused = true;
        }
        ws_msg_free(&m);
        if (r == R_CONTINUE) continue;
        if (r == R_PAUSE || r == R_ACTION || r == R_ERROR) return r;
        ws_close(w->ws, 1000, NULL);
        w->ws = NULL;
        return r;
    }
}

/* ======================================================================
 * workflow setup
 * ====================================================================== */

typedef struct {
    char *token, *dws_base;
    json_t *headers;
    bool service;
} conn_t;
static void conn_free(conn_t *c) {
    free(c->token);
    free(c->dws_base);
    json_free(c->headers);
    memset(c, 0, sizeof *c);
}

static int direct_access(const gl_t *g, const char *root, const char *project, const char *definition, conn_t *out, char **err) {
    memset(out, 0, sizeof *out);
    json_t *b = json_obj();
    json_set_str(b, "workflow_definition", definition);
    json_set(b, "root_namespace_id", json_str_own(to_graphql_ns(root)));
    if (project) json_set_str(b, "project_id", project);
    json_t *p = NULL;
    int st = gl_call(g, "POST", "/api/v4/ai/duo_workflows/direct_access", b, &p, NULL, true);
    json_free(b);
    if (!ok(st)) {
        char *msg = nonempty(json_get_str(p, "message"));
        if (!msg) msg = nonempty(json_get_str(p, "error"));
        *err = st == 0 ? xstrdup("GitLab Duo Workflow direct_access failed: network error")
               : msg   ? xasprintf("GitLab Duo Workflow direct_access failed with HTTP %d: %s", st, msg)
                       : xasprintf("GitLab Duo Workflow direct_access failed with HTTP %d", st);
        free(msg);
        json_free(p);
        return -1;
    }
    json_t *rails = robj(p, "gitlab_rails"), *dws = robj(p, "duo_workflow_service");
    static const char *const flat[] = {"duo_workflow_access_token", "workflow_token", "token", "access_token", "jwt"};
    const char *tok = json_get_str(rails, "token");
    if (!tok) tok = json_get_str(dws, "token");
    for (size_t i = 0; !tok && i < ARRAY_LEN(flat); i++) tok = json_get_str(p, flat[i]);
    if (!tok) {
        *err = xstrdup("GitLab Duo Workflow direct_access did not return credentials");
        json_free(p);
        return -1;
    }
    out->token = xstrdup(tok);
    const char *dbase = json_get_str(dws, "base_url");
    out->service = !json_get_str(rails, "token") && dbase && *dbase;
    out->headers = json_obj();
    if (out->service) {
        char *t = str_trim(dbase);
        char *abs = str_starts(t, "http://") || str_starts(t, "https://") || str_starts(t, "HTTP") ? xstrdup(t) : xasprintf("https://%s", t);
        out->dws_base = norm_base(abs);
        free(abs);
        free(t);
        const char *k;
        json_t *v;
        json_obj_foreach(robj(dws, "headers"), i, k, v) if (json_is_str(v)) json_set_str(out->headers, k, json_sv(v));
    }
    json_free(p);
    return 0;
}

static char *create_workflow(const gl_t *g, const char *ns, const char *project, const char *definition, char **err) {
    json_t *b = json_obj();
    json_set_str(b, "workflow_definition", definition);
    json_set_str(b, "environment", "ide");
    json_set_bool(b, "allow_agent_to_request_user", false);
    json_set(b, "agent_privileges", json_parse_str("[6]"));
    json_set(b, "pre_approved_agent_privileges", json_parse_str("[6]"));
    json_set_bool(b, "requires_duo_cli_enabled", false);
    if (ns && !project) json_set_str(b, "namespace_id", ns);
    if (project) json_set_str(b, "project_id", project);
    json_set_str(b, "goal", ""); /* inline flows carry the goal in the start request */
    json_t *p = NULL;
    int st = gl_call(g, "POST", "/api/v4/ai/duo_workflows/workflows", b, &p, NULL, true);
    json_free(b);
    if (!ok(st)) {
        *err = xasprintf("GitLab Duo Workflow create failed with HTTP %d", st);
        json_free(p);
        return NULL;
    }
    char *id = rstr(p, "id");
    if (!id) id = rstr(p, "workflow_id");
    if (!id) id = rstr(p, "workflowId");
    json_free(p);
    if (!id) *err = xasprintf("GitLab Duo Workflow create response missing workflow id (HTTP %d)", st);
    return id;
}

static char *pinned_model(const gl_t *g, const char *root) {
    json_t *vars = json_obj();
    json_set(vars, "rootNamespaceId", json_str_own(to_graphql_ns(root)));
    json_t *res = graphql(g, MODELS_QUERY, vars);
    char *ref = rstr(robj(robj(robj(res, "data"), "aiChatAvailableModels"), "pinnedModel"), "ref");
    json_free(res);
    if (ref && !*ref) {
        free(ref);
        ref = NULL;
    }
    return ref;
}

static char *numeric_project_id(const gl_t *g, const char *path) {
    char *e = enc(path), *p = xasprintf("/api/v4/projects/%s", e);
    json_t *pj = NULL;
    int st = gl_call(g, "GET", p, NULL, &pj, NULL, true);
    free(p);
    free(e);
    char *id = ok(st) ? rstr(pj, "id") : NULL;
    json_free(pj);
    return id;
}

static bool discover_project(const gl_t *g, const char *rest_ns, char **id, char **path) {
    const char *q = "per_page=1&min_access_level=30&order_by=last_activity_at&sort=desc";
    char *e = enc(rest_ns);
    char *eps[2] = {xasprintf("/api/v4/groups/%s/projects?include_subgroups=true&%s", e, q),
                    xasprintf("/api/v4/projects?membership=true&%s", q)};
    free(e);
    bool found = false;
    for (int i = 0; i < 2 && !found; i++) {
        json_t *arr = NULL;
        int st = gl_call(g, "GET", eps[i], NULL, &arr, NULL, true);
        if (ok(st) && json_is_arr(arr)) {
            json_t *first = json_at(arr, 0);
            char *pid = rstr(first, "id"), *pp = rstr(first, "path_with_namespace");
            if (pid && pp) {
                *id = pid;
                *path = pp;
                found = true;
            } else {
                free(pid);
                free(pp);
            }
        }
        json_free(arr);
    }
    free(eps[0]);
    free(eps[1]);
    return found;
}

static ws_t *open_socket(const char *gitlab_base, const conn_t *c, const char *project_id, const char *ns_rest,
                         const char *selected, const char *definition, char **err) {
    const char *base = c->service ? c->dws_base : gitlab_base;
    sb_t u = SB_INIT;
    if (c->service) {
        url_t p;
        if (url_parse(base, &p) == 0) {
            bool dflt = p.port == (str_eq(p.scheme, "http") ? 80 : 443);
            if (dflt) sb_appendf(&u, "%s://%s/", p.scheme, p.host);
            else sb_appendf(&u, "%s://%s:%d/", p.scheme, p.host, p.port);
            url_free(&p);
        } else {
            sb_appendf(&u, "%s/", base);
        }
    } else {
        sb_appendf(&u, "%s/api/v4/ai/duo_workflows/ws", base);
    }
    char sep = '?';
    struct {
        const char *k, *v;
    } qs[] = {{"project_id", project_id},
              {"namespace_id", c->service ? NULL : ns_rest},
              {"root_namespace_id", ns_rest},
              {"user_selected_model_identifier", selected},
              {"workflow_definition", definition}};
    for (size_t i = 0; i < ARRAY_LEN(qs); i++) {
        if (!qs[i].v || !*qs[i].v) continue;
        sb_putc(&u, sep);
        sep = '&';
        form(&u, qs[i].k);
        sb_putc(&u, '=');
        form(&u, qs[i].v);
    }
    char *http_url = ai_rewrite_url(sb_cstr(&u));
    sb_free(&u);
    char *wsurl = str_starts(http_url, "http://")    ? xasprintf("ws://%s", http_url + 7)
                  : str_starts(http_url, "https://") ? xasprintf("wss://%s", http_url + 8)
                                                     : xstrdup(http_url);
    free(http_url);
    http_headers_t h = {0};
    const char *k;
    json_t *v;
    json_obj_foreach(c->headers, i, k, v) if (json_is_str(v)) http_headers_set(&h, k, json_sv(v));
    char *auth = xasprintf("Bearer %s", c->token);
    http_headers_set(&h, "authorization", auth);
    free(auth);
    http_headers_set(&h, "x-gitlab-client-type", CLIENT_TYPE);
    http_headers_set(&h, "x-gitlab-language-server-version", LS_VERSION);
    http_headers_set(&h, "user-agent", "unknown/unknown unknown/unknown gitlab-language-server/" LS_VERSION);
    url_t ob;
    if (url_parse(gitlab_base, &ob) == 0) {
        bool dflt = ob.port == (str_eq(ob.scheme, "http") ? 80 : 443);
        char *origin = dflt ? xasprintf("%s://%s", ob.scheme, ob.host) : xasprintf("%s://%s:%d", ob.scheme, ob.host, ob.port);
        http_headers_set(&h, "origin", origin);
        free(origin);
        url_free(&ob);
    }
    if (project_id) http_headers_set(&h, "x-gitlab-project-id", project_id);
    if (ns_rest) {
        http_headers_set(&h, "x-gitlab-namespace-id", ns_rest);
        http_headers_set(&h, "x-gitlab-root-namespace-id", ns_rest);
    }
    int status = 0;
    char *werr = NULL;
    ws_t *ws = ws_connect(wsurl, &h, REST_TIMEOUT_MS, &status, &werr);
    http_headers_free(&h);
    free(wsurl);
    if (!ws) {
        *err = xasprintf("GitLab Duo Workflow WebSocket error: %s", werr ? werr : "connect failed");
        free(werr);
    }
    return ws;
}

/* ======================================================================
 * session slots + accounts
 * ====================================================================== */

static wsess_t *sess_acquire(const char *key, bool *private_slot) {
    pthread_mutex_lock(&g_mu);
    wsess_t *w = NULL;
    for (size_t i = 0; i < g_sessions.n; i++)
        if (!strcmp(g_sessions.v[i]->key, key)) w = g_sessions.v[i];
    if (w && w->busy) w = NULL; /* concurrent side request on the same session: run unattached */
    else if (!w) {
        w = xcalloc(1, sizeof *w);
        w->key = xstrdup(key);
        w->last_boundary_len = -1;
        vec_push(&g_sessions, w);
    }
    *private_slot = w == NULL;
    if (!w) {
        w = xcalloc(1, sizeof *w);
        w->key = xstrdup(key);
        w->last_boundary_len = -1;
    }
    w->busy = true;
    pthread_mutex_unlock(&g_mu);
    return w;
}

static void sess_release(wsess_t *w, bool private_slot) {
    if (private_slot) {
        sess_drop_socket(w);
        json_free(w->by_key);
        json_free(w->sigs);
        free(w->stop_base);
        free(w->stop_key);
        free(w->key);
        free(w);
        return;
    }
    pthread_mutex_lock(&g_mu);
    w->busy = false;
    pthread_mutex_unlock(&g_mu);
}

static account_t *account_for(const char *key, const char *base, const char *cwd) {
    char *k = xasprintf("%s%c%s%c%s", key, 0x1f, base, 0x1f, cwd);
    uint64_t h = hash_xx64(k, strlen(k), 0x67646177ULL);
    free(k);
    pthread_mutex_lock(&g_mu);
    account_t *a = NULL;
    for (size_t i = 0; i < g_accounts.n && !a; i++)
        if (g_accounts.v[i]->key == h) a = g_accounts.v[i];
    if (!a) {
        a = xcalloc(1, sizeof *a);
        a->key = h;
        vec_push(&g_accounts, a);
    }
    pthread_mutex_unlock(&g_mu);
    return a;
}

/* ======================================================================
 * entry point
 * ====================================================================== */

typedef struct {
    char *root, *rest_ns, *create_ns, *rest_project, *ws_project, *project_path, *workflow_id, *selected;
    conn_t conn;
    json_t *start;
} setup_t;
static void setup_free(setup_t *s) {
    free(s->root);
    free(s->rest_ns);
    free(s->create_ns);
    free(s->rest_project);
    free(s->ws_project);
    free(s->project_path);
    free(s->workflow_id);
    free(s->selected);
    conn_free(&s->conn);
    json_free(s->start);
    memset(s, 0, sizeof *s);
}

typedef struct {
    const ai_model_t *m;
    const ai_context_t *ctx;
    const ai_stream_opts_t *o;
    gl_t g;
    const char *cwd;
    const char *definition;
    account_t *acct;
} run_t;

static int setup_namespace(const run_t *R, const nssel_t *sel, const char *goal, bool chatml, setup_t *S, char **err) {
    memset(S, 0, sizeof *S);
    const gl_t *g = &R->g;
    S->root = xstrdup(sel->root);
    S->rest_ns = to_rest_ns(sel->root);
    S->create_ns = xstrdup(sel->ns_path ? sel->ns_path : S->rest_ns);
    if (!R->acct->settings_ensured) {
        json_t *b = json_parse_str("{\"experiment_features_enabled\":true,\"ai_settings_attributes\":{\"duo_agent_platform_enabled\":"
                                   "true,\"duo_workflow_mcp_enabled\":true}}");
        char *e = enc(S->rest_ns), *p = xasprintf("/api/v4/groups/%s", e);
        int st = gl_call(g, "PUT", p, b, NULL, NULL, true);
        free(p);
        free(e);
        json_free(b);
        if (st != 0 && st < 500) R->acct->settings_ensured = true;
    }
    char *cfg_path = nonempty(envs("GITLAB_DUO_PROJECT_PATH")), *cfg_id = nonempty(envs("GITLAB_DUO_PROJECT_ID"));
    char *disc_id = NULL, *disc_path = NULL;
    if (!cfg_path && !cfg_id) {
        if (sel->project_path) disc_path = xstrdup(sel->project_path);
        else discover_project(g, S->rest_ns, &disc_id, &disc_path);
    }
    bool id_is_path = cfg_id && strchr(cfg_id, '/');
    const char *numeric_cfg = id_is_path ? NULL : cfg_id;
    const char *path_cfg = id_is_path ? cfg_id : NULL;
    const char *ppath = cfg_path ? cfg_path : path_cfg ? path_cfg : disc_path;
    const char *pid = numeric_cfg ? numeric_cfg : disc_id;
    const char *rest_project = cfg_path ? cfg_path : cfg_id ? cfg_id : disc_path;
    S->rest_project = rest_project ? xstrdup(rest_project) : NULL;
    S->project_path = ppath ? xstrdup(ppath) : NULL;
    S->ws_project = pid ? xstrdup(pid) : ppath ? numeric_project_id(g, ppath) : NULL;
    free(cfg_path);
    free(cfg_id);
    free(disc_id);
    free(disc_path);
    if (direct_access(g, S->root, S->rest_project, R->definition, &S->conn, err) != 0) return -1;
    S->workflow_id = create_workflow(g, S->create_ns, S->rest_project, R->definition, err);
    if (!S->workflow_id) return -1;
    char *pinned = pinned_model(g, S->root);
    S->selected = pinned ? pinned : xstrdup(R->m->id);
    bool advertise = !(json_is_str(R->o->tool_choice) && str_eq(json_sv(R->o->tool_choice), "none"));
    S->start = build_start(S->workflow_id, R->definition, goal, S->ws_project, S->rest_ns, S->selected, R->ctx, advertise, chatml, err);
    return S->start ? 0 : -1;
}

/* the tool result for each pending action, or NULL unless all are present */
static json_t *resolve_batch(const json_t *msgs, const json_t *pending, bool *steer) {
    json_t *out = json_arr(), *a;
    ssize_t last = -1;
    json_arr_foreach(pending, i, a) {
        const char *rid = json_get_str(a, "requestID");
        const json_t *found = NULL;
        for (size_t k = json_len(msgs); k > 0 && !found; k--) {
            const json_t *m = json_at(msgs, k - 1);
            if (str_eq(json_get_str(m, "role"), "toolResult") && str_eq(json_get_str(m, "toolCallId"), rid)) {
                found = m;
                if ((ssize_t)(k - 1) > last) last = (ssize_t)(k - 1);
            }
        }
        if (!found) {
            json_free(out);
            return NULL;
        }
        char *text = content_text(found, false);
        json_t *ptr = json_obj();
        json_set(ptr, json_get_bool(found, "isError", false) ? "error" : "response", json_str_own(text));
        json_t *ar = json_obj();
        json_set_str(ar, "requestID", rid);
        json_set(ar, "plainTextResponse", ptr);
        json_t *wrap = json_obj();
        json_set(wrap, "actionResponse", ar);
        json_push(out, wrap);
    }
    *steer = false;
    for (size_t k = (size_t)(last + 1); last >= 0 && k < json_len(msgs); k++) {
        const char *r = json_get_str(json_at(msgs, k), "role");
        if (str_eq(r, "user") || str_eq(r, "developer")) *steer = true;
    }
    return out;
}

/* resumeGitLabDuoWorkflowSocket + finalize; returns the socket result */
static int resume(st_t *s, const run_t *R, const json_t *responses, json_t *replay, char **err) {
    wsess_t *w = s->sess;
    char *wid = xstrdup(w->workflow_id ? w->workflow_id : "");
    int r = run_socket(s, w->start_payload, responses, replay, err);
    if (r == R_ERROR) {
        sess_drop_socket(w);
        stop_workflow(R->g.base, R->g.key, wid);
        free(wid);
        return r;
    }
    if (r == R_STALLED) {
        sess_drop_socket(w);
        stop_workflow(R->g.base, R->g.key, wid);
        free(wid);
        return r;
    }
    if (r != R_ACTION && r != R_PAUSE) {
        sess_drop_socket(w);
        if (r != R_TERMINAL) finish(s, "stop");
    }
    if (r == R_CLOSED || r == R_TIMEOUT) stop_workflow(R->g.base, R->g.key, wid);
    free(wid);
    return r;
}

static void fresh_run(st_t *s, const run_t *R, char **err) {
    wsess_t *w = s->sess;
    const gl_t *g = &R->g;
    bool explicit_ns = envs("GITLAB_DUO_NAMESPACE_ID") || envs("GITLAB_DUO_PROJECT_ID") || envs("GITLAB_DUO_PROJECT_PATH");
    bool chatml = false;
    char *goal = build_goal(R->ctx->messages, &chatml);
    nssel_t sel = {0};
    setup_t S = {0};
    int rc = -1;
    account_t *a = R->acct;
    if (!explicit_ns && a->have_ns) {
        sel.root = xstrdup(a->root);
        sel.ns_path = a->ns_path ? xstrdup(a->ns_path) : NULL;
        sel.project_path = a->project_path ? xstrdup(a->project_path) : NULL;
        rc = setup_namespace(R, &sel, goal, chatml, &S, err);
        if (rc != 0) {
            free(*err);
            *err = NULL;
            setup_free(&S);
            nssel_free(&sel);
            a->have_ns = false;
        }
    }
    if (rc != 0) {
        if (resolve_namespace(g, R->cwd, &sel, err) != 0) goto out;
        rc = setup_namespace(R, &sel, goal, chatml, &S, err);
        if (rc != 0) goto out;
        if (!explicit_ns) {
            free(a->root);
            free(a->ns_path);
            free(a->project_path);
            a->root = xstrdup(sel.root);
            a->ns_path = sel.ns_path ? xstrdup(sel.ns_path) : NULL;
            a->project_path = sel.project_path ? xstrdup(sel.project_path) : NULL;
            a->have_ns = true;
        }
    }
    size_t goal_bytes = strlen(json_get_str(S.start, "goal"));
    if (goal_bytes >= GOAL_HARD_BYTES) {
        char *m = xasprintf("prompt is too long: %zu bytes exceeds the GitLab Duo Agent goal byte budget (soft %d, hard %d)", goal_bytes,
                            GOAL_SOFT_BYTES, GOAL_HARD_BYTES);
        finish_error(s, m);
        free(m);
        sess_drop_socket(w);
        stop_workflow(g->base, g->key, S.workflow_id);
        goto out;
    }
    if (goal_bytes >= GOAL_SOFT_BYTES)
        s->goal_overflow = xasprintf("prompt is too long: %zu bytes exceeds the GitLab Duo Agent goal byte budget (soft %d, hard %d)",
                                     goal_bytes, GOAL_SOFT_BYTES, GOAL_HARD_BYTES);
    int last = R_CLOSED;
    bool timeout_reconnected = false, settled = false;
    int step_restarts = 0, generic_retries = 0, stall_restarts = 0;
    json_t *start = json_clone(S.start);
    char *wid = xstrdup(S.workflow_id);
    for (int attempt = 0; attempt < 12; attempt++) {
        ws_t *ws = open_socket(g->base, &S.conn, S.ws_project, S.rest_ns, S.selected, R->definition, err);
        if (!ws) break;
        sess_drop_socket(w);
        w->active = true;
        w->ws = ws;
        w->workflow_id = xstrdup(wid);
        w->start_payload = json_clone(start);
        last = run_socket(s, start, NULL, NULL, err);
        if (last == R_ERROR) break;
        if (last == R_APPROVAL) {
            json_set_str(start, "goal", "");
            json_set(start, "additional_context", json_arr());
            json_set(start, "approval", json_parse_str("{\"approval\":{}}"));
            free(s->last_approval);
            s->last_approval = NULL;
            continue;
        }
        bool restart = false;
        if (last == R_TIMEOUT && !timeout_reconnected) timeout_reconnected = restart = true;
        else if (last == R_STEP_LIMIT && step_restarts < MAX_STEP_LIMIT_RESTARTS) step_restarts++, restart = true;
        else if (last == R_STALLED && stall_restarts < MAX_STALL_RESTARTS) stall_restarts++, restart = true;
        else if (last == R_RETRYABLE && generic_retries < MAX_GENERIC_ERROR_RETRIES) {
            generic_retries++;
            restart = true;
            free(s->stashed_error);
            s->stashed_error = NULL;
        }
        if (restart) {
            sess_drop_socket(w);
            stop_workflow(g->base, g->key, wid);
            free(wid);
            wid = create_workflow(g, S.create_ns, S.rest_project, R->definition, err);
            if (!wid) break;
            json_set_str(start, "workflowID", wid);
            continue;
        }
        if (last == R_RETRYABLE && !done(s))
            finish_error(s, s->goal_overflow ? s->goal_overflow : s->stashed_error ? s->stashed_error : "GitLab Duo Workflow failed");
        if (last == R_STALLED && !done(s)) finish_error(s, s->goal_overflow ? s->goal_overflow : STALL_MESSAGE);
        break;
    }
    if (!*err) {
        settled = true;
        /* finalizeGitLabDuoWorkflowResumeResult */
        if (last != R_ACTION && last != R_PAUSE) {
            if (last != R_TERMINAL) finish(s, "stop");
        }
    }
    bool aborted = s->cancel && *s->cancel;
    if (aborted || !settled || last == R_CLOSED || last == R_TIMEOUT || last == R_STALLED ||
        (last != R_ACTION && last != R_PAUSE)) {
        sess_drop_socket(w);
        if (aborted || !settled || last == R_CLOSED || last == R_TIMEOUT || last == R_STALLED) stop_workflow(g->base, g->key, wid);
    }
    if (w->active) {
        free(w->stop_base);
        free(w->stop_key);
        w->stop_base = xstrdup(g->base);
        w->stop_key = xstrdup(g->key);
    }
    json_free(start);
    free(wid);
out:
    nssel_free(&sel);
    setup_free(&S);
    free(goal);
}

json_t *provider_gitlab_duo_workflow_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                                            ai_event_cb cb, void *ud) {
    ai_stream_opts_t def;
    if (!o) {
        ai_stream_opts_init(&def);
        o = &def;
    }
    st_t s = {0};
    s.last_cp_len = -1;
    s.cancel = o->cancel;
    aib_init(&s.b, m, cb, ud);
    aib_start(&s.b);
    char *env_key = NULL;
    const char *key = o->api_key;
    if (str_empty(key)) key = env_key = ai_env_api_key(m->provider);
    if (str_empty(key)) {
        json_t *r = aib_finish(&s.b, "error", "No API key for provider: gitlab-duo-agent");
        aib_free(&s.b);
        free(env_key);
        return r;
    }
    char *base = norm_base(m->base_url);
    char cwdbuf[4096];
    const char *cwd = getcwd(cwdbuf, sizeof cwdbuf) ? cwdbuf : ".";
    const char *defn = envs("GITLAB_DUO_WORKFLOW_DEFINITION");
    char *defn_t = defn ? str_trim(defn) : xstrdup(DEFINITION);
    run_t R = {m, ctx, o, {base, key, o->cancel}, cwd, defn_t, account_for(key, base, cwd)};
    char *skey = xasprintf("gitlab-duo-agent:%s%c%s%c%s", base, 0x1f, m->id, 0x1f, o->session_id ? o->session_id : "");
    bool priv = false;
    wsess_t *w = sess_acquire(skey, &priv);
    free(skey);
    s.sess = w;
    char *err = NULL;
    bool need_fresh = true;
    if (w->active && w->pending && json_len(w->pending) > 0) {
        bool steer = false;
        json_t *batch = resolve_batch(ctx->messages, w->pending, &steer);
        if (batch && !steer) {
            json_free(w->pending);
            w->pending = NULL;
            int r = resume(&s, &R, batch, NULL, &err);
            need_fresh = r == R_STALLED;
            if (r == R_STALLED) {
                /* the stalled turn's partial content stays; seed a fresh workflow */
            }
        } else {
            /* steer or unpaired action: abandon the live workflow, seed a fresh one */
            char *wid = w->workflow_id ? xstrdup(w->workflow_id) : NULL;
            sess_drop_socket(w);
            stop_workflow(base, key, wid);
            free(wid);
        }
        json_free(batch);
    } else if (w->active && w->paused) {
        json_t *replay = w->pause_buffer ? w->pause_buffer : json_arr();
        w->pause_buffer = NULL;
        w->paused = false;
        int r = resume(&s, &R, NULL, replay, &err);
        json_free(replay);
        need_fresh = r == R_STALLED;
    } else if (w->active) {
        /* a live socket nobody can resume (e.g. an aborted turn): stop it */
        char *wid = w->workflow_id ? xstrdup(w->workflow_id) : NULL;
        sess_drop_socket(w);
        stop_workflow(base, key, wid);
        free(wid);
    }
    if (need_fresh && !err && !done(&s)) fresh_run(&s, &R, &err);
    if (err) finish_error(&s, s.goal_overflow ? s.goal_overflow : err);
    if (!done(&s)) finish(&s, "stop");
    json_t *msg = s.final;
    s.final = NULL;
    aib_free(&s.b);
    sess_release(w, priv);
    free(err);
    free(s.active_key);
    free(s.last_approval);
    free(s.goal_overflow);
    free(s.stashed_error);
    free(defn_t);
    free(base);
    free(env_key);
    return msg;
}
