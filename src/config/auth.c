#define _GNU_SOURCE
#include "auth.h"
#include "auth_engine.h"

#include "ai/ai.h"
#include "ai/aws.h"
#include "ai/google_auth.h"
#include "core/fs.h"
#include "core/proc.h"
#include "net/http.h"

#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include "core/compat_sys.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *g_store;   /* contents of auth.json */
static hmap_t g_runtime;  /* provider -> char* */
static bool g_init;

static char *auth_path(void) { return omc_agent_path("auth.json"); }

static void load(void) {
    if (g_init) return;
    g_init = true;
    hmap_init(&g_runtime);
    char *p = auth_path();
    size_t n;
    char *s = fs_read(p, &n);
    g_store = s ? json_parse(s, n, NULL) : NULL;
    if (!json_is_obj(g_store)) {
        json_free(g_store);
        g_store = json_obj();
    }
    free(s);
    free(p);
}

void auth_init(void) {
    pthread_mutex_lock(&g_mu);
    load();
    pthread_mutex_unlock(&g_mu);
}

void auth_reload(void) {
    pthread_mutex_lock(&g_mu);
    load();
    char *p = auth_path();
    size_t n;
    char *s = fs_read(p, &n);
    json_t *j = s ? json_parse(s, n, NULL) : NULL;
    json_free(g_store);
    g_store = json_is_obj(j) ? j : json_obj();
    if (!json_is_obj(j)) json_free(j);
    free(s);
    free(p);
    pthread_mutex_unlock(&g_mu);
}

static int save(void) {
    char *p = auth_path();
    char *s = json_dump(g_store, 2);
    int rc = fs_write_atomic(p, s, strlen(s), 0600);
    free(s);
    free(p);
    return rc;
}

void auth_set_runtime_key(const char *provider, const char *key) {
    pthread_mutex_lock(&g_mu);
    load();
    free(hmap_put(&g_runtime, provider, xstrdup(key)));
    pthread_mutex_unlock(&g_mu);
}

int auth_set_api_key(const char *provider, const char *key) {
    pthread_mutex_lock(&g_mu);
    load();
    json_t *e = json_obj();
    json_set_str(e, "type", "api_key");
    json_set_str(e, "key", key);
    json_set(g_store, provider, e);
    int rc = save();
    pthread_mutex_unlock(&g_mu);
    return rc;
}

int auth_logout(const char *provider) {
    pthread_mutex_lock(&g_mu);
    load();
    json_del(g_store, provider);
    int rc = save();
    pthread_mutex_unlock(&g_mu);
    return rc;
}

json_t *auth_list(void) {
    pthread_mutex_lock(&g_mu);
    load();
    json_t *out = json_arr();
    const char *k;
    json_t *v;
    json_obj_foreach(g_store, i, k, v) {
        json_t *e = json_obj();
        json_set_str(e, "provider", k);
        json_set_str(e, "type", json_get_str(v, "type"));
        if (json_get(v, "expires")) json_set(e, "expires", json_clone(json_get(v, "expires")));
        if (json_get_str(v, "email")) json_set_str(e, "email", json_get_str(v, "email"));
        json_push(out, e);
    }
    pthread_mutex_unlock(&g_mu);
    return out;
}

/* ---------- descriptors ---------- */

/* ---------- descriptors ---------- */

static json_t *descriptor(const char *provider) {
    json_t *arr = catalog_auth_providers(), *d;
    json_arr_foreach(arr, i, d) if (str_eq(json_get_str(d, "id"), provider)) return d;
    return NULL;
}

json_t *auth_oauth_providers(void) { return catalog_auth_providers(); }

const char *auth_store_id(const char *provider) {
    const char *s = json_get_str(descriptor(provider), "storeAs");
    return s ? s : provider;
}

/* ---------- refresh ---------- */

static int refresh_oauth(const char *provider, json_t *entry, char **err) {
    /* credentials minted from a runtime descriptor (MCP OAuth) carry it along */
    json_t *d = json_get_obj(entry, "descriptor");
    if (!d) d = descriptor(provider);
    json_t *fresh = NULL;
    if (auth_refresh_credential(provider, d, entry, &fresh, err) != 0) return -1;
    /* identity fields the refresh response omits are kept from the stored row */
    json_t *c = json_clone(entry);
    json_merge(c, fresh);
    json_free(fresh);
    json_set(g_store, provider, c);
    save();
    return 0;
}

/* `api-key-format "structured"`: request-time credential metadata travels as the JSON-encoded credential */
static char *structured_key(const json_t *e) {
    json_t *k = json_obj();
    static const char *const fields[][2] = {{"apiEndpoint", "apiEndpoint"}, {"token", "access"},
                                            {"enterpriseUrl", "enterpriseUrl"}, {"projectId", "projectId"},
                                            {"refreshToken", "refresh"}};
    for (size_t i = 0; i < ARRAY_LEN(fields); i++) {
        json_t *v = json_get(e, fields[i][1]);
        if (v && !json_is_null(v)) json_set(k, fields[i][0], json_clone(v));
    }
    if (json_get(e, "expires")) json_set(k, "expiresAt", json_clone(json_get(e, "expires")));
    if (json_get_str(e, "email")) json_set_str(k, "email", json_get_str(e, "email"));
    if (json_get_str(e, "accountId")) json_set_str(k, "accountId", json_get_str(e, "accountId"));
    char *s = json_dump(k, 0);
    json_free(k);
    return s;
}

/* `expiry "jwt-or-never"` (registry/oauth/index.ts normalizeOAuthCredentialExpiry) */
static int64_t normalized_expiry(const json_t *d, const json_t *e) {
    int64_t exp = json_get_int(e, "expires", 0);
    if (!str_eq(json_get_str(d, "expiry"), "jwt-or-never")) return exp;
    if (exp > 0 && exp < 10000000000LL) exp *= 1000;
    json_t *p = auth_jwt_payload(json_get_str(e, "access"));
    json_t *je = json_get(p, "exp");
    int64_t r = json_is_num(je) ? (int64_t)(je->u.num.d * 1000) - 300000 : MAX(exp, AUTH_NEVER_EXPIRES);
    json_free(p);
    return r;
}

char *auth_get_api_key(const char *provider) {
    pthread_mutex_lock(&g_mu);
    load();
    char *r = NULL;
    const char *rt = hmap_get(&g_runtime, provider);
    if (rt) r = xstrdup(rt);
    json_t *e = r ? NULL : json_get_obj(g_store, provider);
    if (e) {
        const char *type = json_get_str(e, "type");
        if (str_eq(type, "api_key")) {
            const char *k = json_get_str(e, "key");
            if (k) {
                /* "!cmd" runs a command, env var names resolve */
                if (k[0] == '!') r = proc_shell_capture(k + 1, NULL, 15000);
                else if (env_get(k)) r = xstrdup(env_get(k));
                else r = xstrdup(k);
            }
        } else if (str_eq(type, "oauth")) {
            json_t *dd = json_get_obj(e, "descriptor");
            if (!dd) dd = descriptor(provider);
            int64_t exp = normalized_expiry(dd, e);
            if (exp && exp - 60000 < now_ms()) {
                char *err = NULL;
                if (refresh_oauth(provider, e, &err) != 0) {
                    LOGW("oauth refresh for %s failed: %s", provider, err ? err : "?");
                    free(err);
                }
                e = json_get_obj(g_store, provider);
                dd = json_get_obj(e, "descriptor") ? json_get_obj(e, "descriptor") : descriptor(provider);
            }
            const char *acc = json_get_str(e, "access");
            if (acc) r = str_eq(json_get_str(dd, "apiKeyFormat"), "structured") ? structured_key(e) : xstrdup(acc);
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (!r) r = ai_env_api_key(provider);
    return r;
}

bool auth_has_credentials(const char *provider) {
    pthread_mutex_lock(&g_mu);
    load();
    bool ok = hmap_get(&g_runtime, provider) || json_get_obj(g_store, provider);
    pthread_mutex_unlock(&g_mu);
    if (ok) return true;
    char *k = ai_env_api_key(provider);
    ok = k != NULL;
    free(k);
    /* Bedrock authenticates outside the API-key path (SigV4 credential chain or AWS_BEARER_TOKEN_BEDROCK) */
    if (!ok && str_eq(provider, "amazon-bedrock")) ok = aws_has_credential_source();
    /* env hook google-vertex-adc: GOOGLE_CLOUD_API_KEY, or an ADC file plus project and location */
    if (!ok && str_eq(provider, "google-vertex")) ok = google_vertex_has_credentials();
    return ok;
}

/* ---------- oauth-code flow (registry/engine/oauth-code.ts + oauth/callback-server.ts) ---------- */

static char *pkce_challenge(const char *verifier) {
    unsigned char dig[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)verifier, strlen(verifier), dig);
    return base64url_encode(dig, sizeof dig);
}

/* loopback listener; port 0 picks a free one. *bound receives the actual port. */
static int listen_port(int port, int *bound) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    socklen_t al = sizeof a;
    *bound = getsockname(fd, (struct sockaddr *)&a, &al) == 0 ? ntohs(a.sin_port) : port;
    return fd;
}

static char *query_param(const char *q, const char *name) {
    size_t nl = strlen(name);
    const char *p = q;
    while (p && *p) {
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            const char *v = p + nl + 1;
            char *raw = xstrndup(v, strcspn(v, "& #"));
            char *dec = url_decode(raw);
            free(raw);
            return dec;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

/* parse pasted text: full redirect URL, "code#state", or bare code */
static void parse_pasted(const char *in, char **code, char **state) {
    char *t = str_trim(in);
    const char *q = strchr(t, '?');
    if (q && strstr(t, "code=")) {
        *code = query_param(q + 1, "code");
        *state = query_param(q + 1, "state");
    } else if (strchr(t, '#')) {
        char *h = strchr(t, '#');
        *code = xstrndup(t, (size_t)(h - t));
        *state = h[1] ? xstrdup(h + 1) : NULL;
    } else {
        *code = xstrdup(t);
    }
    free(t);
}

static void form_append(sb_t *u, const char *s) {
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (isalnum(*c) || *c == '*' || *c == '-' || *c == '.' || *c == '_') sb_putc(u, (char)*c);
        else if (*c == ' ') sb_putc(u, '+');
        else sb_appendf(u, "%%%02X", *c);
    }
}

static void strip_trailing_slashes(char *s) {
    if (!s) return;
    for (size_t n = strlen(s); n > 0 && s[n - 1] == '/';) s[--n] = 0;
}

int auth_oauth_code_flow(const actx_t *c, const json_t *login, json_t **cred_out, char **err) {
    const auth_login_hooks_t *hooks = c->h;
    *cred_out = NULL;
    char *client_id = json_get(login, "clientId") ? auth_resolve_value(json_get(login, "clientId"), c, err) : NULL;
    char *client_secret = json_get(login, "clientSecret") ? auth_resolve_value(json_get(login, "clientSecret"), c, err) : NULL;
    char *base = json_get(login, "baseUrl") ? auth_resolve_value(json_get(login, "baseUrl"), c, err) : NULL;
    char *authb = json_get(login, "authUrl") ? auth_resolve_value(json_get(login, "authUrl"), c, err) : NULL;
    char *auth_url_raw = auth_resolve_value(json_get(login, "authorizeUrl"), c, err);
    strip_trailing_slashes(base);
    strip_trailing_slashes(authb);
    const json_t *cb = json_get_obj(login, "callback");
    int port = (int)json_get_int(cb, "port", 0);
    int preferred = port;
    char *redirect = json_get(cb, "redirectUri") ? auth_resolve_value(json_get(cb, "redirectUri"), c, err) : NULL;
    if (redirect && !*redirect) {
        free(redirect);
        redirect = NULL;
    }
    /* bind before building the redirect: a busy preferred port falls back to a free one when allowed */
    int lfd = -1;
    if (port && !json_get_bool(cb, "manualOnly", false)) {
        int bound = port;
        lfd = listen_port(port, &bound);
        if (lfd < 0 && json_get_bool(cb, "portFallback", false) && !redirect) lfd = listen_port(0, &bound);
        if (lfd >= 0) port = bound;
    }
    if (!redirect)
        redirect = xasprintf("http://%s:%d%s", json_get_str(cb, "hostname") ? json_get_str(cb, "hostname") : "localhost",
                             port, json_get_str(cb, "path") ? json_get_str(cb, "path") : "/callback");
    if (lfd >= 0 && preferred && port != preferred) {
        char *m = xasprintf("Preferred port %d unavailable, using port %d", preferred, port);
        actx_progress(c, m);
        free(m);
    }
    bool pkce = json_get_bool(login, "pkce", true);
    unsigned char vb[96]; /* generatePKCE: 96 random bytes -> 128-char verifier */
    rand_bytes(vb, sizeof vb);
    char *verifier = base64url_encode(vb, sizeof vb);
    char *challenge = pkce_challenge(verifier);
    const char *smode = json_get_str(login, "state");
    char *state = str_eq(smode, "none") ? xstrdup("") : str_eq(smode, "uuid") ? uuid_v4() : rand_hex(16);
    json_t *scopes = json_get_arr(login, "scopes"), *s;
    const char *sep = json_get_str(login, "scopeSeparator");
    sb_t sc = SB_INIT;
    json_arr_foreach(scopes, i, s) sb_appendf(&sc, "%s%s", i ? (sep ? sep : " ") : "", json_sv(s) ? json_sv(s) : "");
    const char *scope_s = sb_cstr(&sc);
    json_t *vars = json_obj();
    if (client_id) json_set_str(vars, "client_id", client_id);
    json_set_str(vars, "redirect_uri", redirect);
    if (*scope_s) json_set_str(vars, "scope", scope_s);
    json_set_str(vars, "state", state);
    if (pkce) json_set_str(vars, "code_challenge", challenge);
    if (base) json_set_str(vars, "base", base);
    if (authb) json_set_str(vars, "auth", authb);
    /* authorize URL exactly like upstream generateAuthUrl: URLSearchParams order (standard params, then
     * templated authorizeParams), form encoding (space -> '+') */
    strvec_t pk = {0}, pv = {0};
#define PSET(K, V)                                                                                                     \
    do {                                                                                                               \
        bool found_ = false;                                                                                           \
        for (size_t q_ = 0; q_ < pk.n; q_++)                                                                           \
            if (!strcmp(pk.v[q_], (K))) {                                                                              \
                free(pv.v[q_]);                                                                                        \
                pv.v[q_] = xstrdup(V);                                                                                 \
                found_ = true;                                                                                         \
            }                                                                                                          \
        if (!found_) {                                                                                                 \
            strvec_push(&pk, (K));                                                                                     \
            strvec_push(&pv, (V));                                                                                     \
        }                                                                                                              \
    } while (0)
    if (json_get_bool(login, "standardAuthorizeParams", true)) {
        if (client_id && *client_id) PSET("client_id", client_id);
        PSET("response_type", "code");
        PSET("redirect_uri", redirect);
        if (*scope_s) PSET("scope", scope_s);
        if (pkce) {
            PSET("code_challenge", challenge);
            PSET("code_challenge_method", "S256");
        }
        if (*state) PSET("state", state);
    }
    const char *k;
    json_t *v;
    json_obj_foreach(json_get_obj(login, "authorizeParams"), i, k, v) {
        char *t = auth_template(json_sv(v) ? json_sv(v) : "", vars);
        PSET(k, t);
        free(t);
    }
#undef PSET
    char *auth_url = auth_template(auth_url_raw ? auth_url_raw : "", vars);
    sb_t u = SB_INIT;
    sb_append(&u, auth_url);
    sb_putc(&u, '?');
    for (size_t q = 0; q < pk.n; q++) {
        if (q) sb_putc(&u, '&');
        form_append(&u, pk.v[q]);
        sb_putc(&u, '=');
        form_append(&u, pv.v[q]);
    }
    strvec_free(&pk);
    strvec_free(&pv);
    char *launch = NULL;
    if (lfd >= 0 && (str_starts(redirect, "http://localhost:") || str_starts(redirect, "http://127.0.0.1:")))
        launch = xasprintf("http://localhost:%d/launch", port);
    bool read_stdin = hooks && hooks->read_stdin;
    actx_auth(c, sb_cstr(&u), launch, json_get_str(login, "instructions"));
    actx_progress(c, read_stdin ? "Waiting for the browser redirect (or paste the code / redirect URL and press Enter)..."
                     : lfd >= 0 ? "Waiting for browser authentication..."
                                : "Waiting for pasted authorization code...");
    char *code = NULL, *got_state = NULL;
    int64_t deadline = mono_ms() + 300000;
    sb_t typed = SB_INIT;
    while (!code && mono_ms() < deadline) {
        if (actx_aborted(c)) break;
        if (hooks && hooks->poll_code) {
            char *pasted = hooks->poll_code(hooks->ud);
            if (pasted) {
                char *t = str_trim(pasted);
                if (*t) parse_pasted(t, &code, &got_state);
                free(t);
                free(pasted);
                if (code) break;
            }
        }
        struct pollfd pf[2];
        int np = 0;
        pf[np++] = (struct pollfd){read_stdin ? 0 : -1, POLLIN, 0};
        if (lfd >= 0) pf[np++] = (struct pollfd){lfd, POLLIN, 0};
        int pr = poll(pf, (nfds_t)np, read_stdin ? 500 : 100);
        if (pr <= 0) continue;
        if (read_stdin && (pf[0].revents & (POLLIN | POLLHUP))) {
            char buf[4096];
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) {
                read_stdin = false; /* stdin closed: keep waiting for the browser */
                if (lfd < 0) break;
                continue;
            }
            sb_appendn(&typed, buf, (size_t)n);
            if (strchr(sb_cstr(&typed), '\n')) {
                char *t = str_trim(sb_cstr(&typed));
                if (*t) parse_pasted(t, &code, &got_state);
                free(t);
                sb_clear(&typed);
            }
        }
        if (np > 1 && (pf[1].revents & POLLIN)) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd < 0) continue;
            char req[8192];
            ssize_t n = recv(cfd, req, sizeof req - 1, 0);
            if (n > 0 && launch && !strncmp(req, "GET /launch", 11) && (req[11] == ' ' || req[11] == '?')) {
                /* local shortcut: redirect to the authorize URL */
                char *resp = xasprintf("HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                                       sb_cstr(&u));
                if (send(cfd, resp, strlen(resp), MSG_NOSIGNAL) < 0) {}
                free(resp);
                close(cfd);
                continue;
            }
            if (n > 0) {
                req[n] = 0;
                char *q = strchr(req, '?');
                char *sp = q ? strchr(q, ' ') : NULL;
                if (q && sp) {
                    *sp = 0;
                    code = query_param(q + 1, "code");
                    got_state = query_param(q + 1, "state");
                }
            }
            const char *html = code ? "<html><body><h3>omc: login complete. You can close this tab.</h3></body></html>"
                                    : "<html><body><h3>omc: login failed (no code).</h3></body></html>";
            char *resp = xasprintf("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                                   strlen(html), html);
            if (send(cfd, resp, strlen(resp), MSG_NOSIGNAL) < 0) {}
            free(resp);
            close(cfd);
        }
    }
    sb_free(&typed);
    free(launch);
    if (lfd >= 0) close(lfd);
    int rc = -1;
    const json_t *pk_rule = json_get_obj(login, "pasteKey");
    if (!code) {
        *err = xstrdup(actx_aborted(c) ? "Login cancelled" : "login timed out or was cancelled");
    } else if (pk_rule && json_get_str(pk_rule, "prefix") && str_starts(code, json_get_str(pk_rule, "prefix"))) {
        /* a pasted API key (e.g. OpenRouter sk-or-...) is validated and kept as-is */
        int st = 0;
        if (auth_validate_models(c->label, code, json_get_str(pk_rule, "validateUrl"), NULL, c, &st, err) == 0) {
            json_t *cr = json_obj();
            json_set_str(cr, "type", "oauth");
            json_set_str(cr, "access", code);
            json_set_str(cr, "refresh", "");
            json_set_int(cr, "expires", AUTH_NEVER_EXPIRES);
            *cred_out = cr;
            rc = 0;
        }
    } else if (got_state && *state && strcmp(got_state, state) != 0) {
        *err = xstrdup("OAuth state mismatch; aborting");
    } else {
        json_t *xv = json_clone(vars);
        json_set_str(xv, "code", code);
        json_set_str(xv, "state", got_state && *got_state ? got_state : state);
        if (pkce) json_set_str(xv, "code_verifier", verifier);
        if (client_secret) json_set_str(xv, "client_secret", client_secret);
        json_t *std = json_obj();
        json_set_str(std, "grant_type", "authorization_code");
        if (client_id) json_set_str(std, "client_id", client_id);
        if (client_secret) json_set_str(std, "client_secret", client_secret);
        json_set_str(std, "code", code);
        json_set_str(std, "redirect_uri", redirect);
        if (pkce) json_set_str(std, "code_verifier", verifier);
        json_t *body = NULL;
        if (auth_post_token(json_get_obj(login, "token"), std, xv, NULL, c->provider, "token-exchange", c, NULL, &body, err) == 0) {
            json_t *cr = auth_map_credentials(json_get_obj(login, "credential"), body, c->provider, NULL, err);
            if (cr) {
                auth_apply_userinfo(json_get_obj(login, "userinfo"), cr, NULL, xv, c);
                if (actx_aborted(c)) {
                    *err = xstrdup("Login cancelled");
                } else if (auth_after_exchange(json_get_str(login, "afterExchange"), c, "login", &cr, body, NULL, err) == 0) {
                    *cred_out = cr;
                    cr = NULL;
                    rc = 0;
                }
                json_free(cr);
            }
        }
        json_free(body);
        json_free(std);
        json_free(xv);
    }
    json_free(vars);
    free(code);
    free(got_state);
    free(client_id);
    free(client_secret);
    free(base);
    free(authb);
    free(auth_url_raw);
    free(auth_url);
    free(redirect);
    free(verifier);
    free(challenge);
    free(state);
    sb_free(&sc);
    sb_free(&u);
    return rc;
}

/* ---------- login dispatch (auth/oauth.ts AuthStorage.oauth.login) ---------- */

static int login_run(const char *provider, const json_t *d, bool embed, const auth_login_hooks_t *hooks, char **err) {
    *err = NULL;
    if (!d) {
        *err = xasprintf("Unknown OAuth provider: %s", provider);
        return -1;
    }
    const json_t *login = json_get_obj(d, "login");
    const char *kind = json_get_str(login, "kind");
    actx_t c = {provider, json_get_str(d, "name") ? json_get_str(d, "name") : provider, d, hooks};
    alogin_t r = {0};
    int rc;
    if (str_eq(kind, "oauth-code")) rc = auth_oauth_code_flow(&c, login, &r.cred, err);
    else if (str_eq(kind, "device-code")) rc = auth_device_code_login(&c, &r.cred, err);
    else if (str_eq(kind, "api-key")) rc = auth_api_key_login(&c, &r.api_key, err);
    else if (str_eq(kind, "custom")) rc = auth_custom_hook_login(&c, json_get_str(login, "hook"), &r, err);
    else {
        *err = xasprintf("%s has no interactive login; set an API key with `omc auth set %s <key>`", provider, provider);
        rc = -1;
    }
    if (rc == 0 && r.cred && str_eq(json_get_str(d, "result"), "api-key")) {
        r.api_key = xstrdup(json_get_str(r.cred, "access") ? json_get_str(r.cred, "access") : "");
        json_free(r.cred);
        r.cred = NULL;
    }
    if (rc == 0 && r.api_key) {
        /* an empty optional key (local servers) stores nothing */
        if (*r.api_key && auth_set_api_key(provider, r.api_key) != 0) {
            *err = xstrdup("failed to save credentials");
            rc = -1;
        }
    } else if (rc == 0 && r.cred) {
        json_set_int(r.cred, "authorizedAt", now_ms());
        if (embed) json_set(r.cred, "descriptor", json_clone(d));
        const char *store = json_get_str(d, "storeAs") ? json_get_str(d, "storeAs") : provider;
        pthread_mutex_lock(&g_mu);
        load();
        json_set(g_store, store, r.cred);
        r.cred = NULL;
        rc = save() == 0 ? 0 : -1;
        pthread_mutex_unlock(&g_mu);
        if (rc) *err = xstrdup("failed to save credentials");
    }
    free(r.api_key);
    json_free(r.cred);
    if (rc && !*err) *err = xstrdup("login failed");
    return rc;
}

/* ---------- console callbacks (omp cli/oauth-terminal.ts runTerminalOAuthLogin) ---------- */

static void con_print(const auth_io_t *io, const char *m) {
    if (io && io->print) io->print(io->ud, m);
    else fprintf(stderr, "%s\n", m);
}

static void con_auth(void *ud, const char *url, const char *launch, const char *instructions) {
    const auth_io_t *io = ud;
    sb_t b = SB_INIT;
    sb_appendf(&b, "\nOpen this URL in your browser:\n%s", url);
    if (launch && strcmp(launch, url)) sb_appendf(&b, "\nLocal shortcut (this machine only): %s", launch);
    if (instructions && *instructions) sb_appendf(&b, "\n%s", instructions);
    sb_append(&b, "\n");
    con_print(io, sb_cstr(&b));
    sb_free(&b);
    char *opener = env_truthy("OMC_NO_BROWSER") ? NULL : which("xdg-open");
    if (!opener && !env_truthy("OMC_NO_BROWSER")) opener = which("open");
    if (opener) {
        const char *argv[] = {opener, url, NULL};
        proc_opts_t po = {.timeout_ms = 5000, .new_session = true};
        proc_result_t pr;
        if (proc_run(argv, &po, &pr) == 0) proc_result_free(&pr);
        free(opener);
    }
}

static void con_progress(void *ud, const char *msg) { con_print(ud, msg); }

/* one line from fd 0 without stdio buffering (the oauth-code paste reader shares the descriptor) */
static char *read_line_fd0(void) {
    sb_t b = SB_INIT;
    char ch;
    for (;;) {
        ssize_t n = read(0, &ch, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (!b.len) {
                sb_free(&b);
                return NULL;
            }
            break;
        }
        if (ch == '\n') break;
        sb_putc(&b, ch);
        if (b.len > 65536) break;
    }
    char *t = str_trim(sb_cstr(&b));
    sb_free(&b);
    return t;
}

static char *con_prompt(void *ud, const auth_prompt_t *p) {
    const auth_io_t *io = ud;
    char *q = p->placeholder && *p->placeholder ? xasprintf("%s (%s):", p->message, p->placeholder) : xasprintf("%s:", p->message);
    char *a;
    if (io && io->prompt) {
        a = io->prompt(io->ud, q);
    } else {
        fprintf(stderr, "%s ", q);
        fflush(stderr);
        a = read_line_fd0();
    }
    free(q);
    return a;
}

static int login_console(const char *provider, const json_t *d, bool embed, const auth_io_t *io, char **err) {
    auth_login_hooks_t h = {0};
    h.on_auth = con_auth;
    h.on_progress = con_progress;
    h.prompt = con_prompt;
    h.read_stdin = true;
    h.ud = (void *)io;
    h.legacy_io = io;
    return login_run(provider, d, embed, &h, err);
}

int auth_login(const char *provider, const auth_io_t *io, char **err) {
    return login_console(provider, descriptor(provider), false, io, err);
}

int auth_login_descriptor(const char *key, const json_t *d, const auth_io_t *io, char **err) {
    return login_console(key, d, true, io, err);
}

int auth_login_hooked(const char *provider, const auth_login_hooks_t *hooks, char **err) {
    return login_run(provider, descriptor(provider), false, hooks, err);
}
