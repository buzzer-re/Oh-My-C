#define _GNU_SOURCE
#include "auth.h"

#include "ai/ai.h"
#include "ai/aws.h"
#include "core/fs.h"
#include "core/proc.h"
#include "net/http.h"

#include <errno.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

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

static json_t *descriptor(const char *provider) {
    json_t *arr = catalog_auth_providers(), *d;
    json_arr_foreach(arr, i, d) if (str_eq(json_get_str(d, "id"), provider)) return d;
    return NULL;
}

json_t *auth_oauth_providers(void) { return catalog_auth_providers(); }

static char *value_of(const json_t *v) {
    if (!v) return NULL;
    if (json_is_str(v)) return xstrdup(json_sv(v));
    const char *val = json_get_str(v, "value");
    if (!val) return NULL;
    if (str_eq(json_get_str(v, "encoding"), "base64")) {
        size_t n;
        unsigned char *d = base64_decode(val, strlen(val), &n);
        if (!d) return NULL;
        char *r = xstrndup((char *)d, n);
        free(d);
        return r;
    }
    return xstrdup(val);
}

static char *subst(const char *s, const char *state) {
    char *a = str_replace(s, "{state}", state ? state : "");
    char *b = str_replace(a, "{claude_code_sdk_version}", "0.60.0");
    free(a);
    return b;
}

/* extract credential fields from token response per descriptor "credential" map */
static json_t *extract_cred(const json_t *cred_desc, const json_t *resp, const json_t *prev) {
    json_t *c = json_obj();
    json_set_str(c, "type", "oauth");
    const char *k;
    json_t *v;
    json_obj_foreach(cred_desc, i, k, v) {
        const char *path = json_get_str(v, "path");
        json_t *val = path ? json_path(resp, path) : NULL;
        if (!val) continue;
        if (!strcmp(k, "expires")) {
            double secs = json_is_num(val) ? val->u.num.d : json_is_str(val) ? strtod(json_sv(val), NULL) : 0;
            const char *mode = json_get_str(v, "mode");
            int64_t skew = json_get_int(v, "skewMs", 0);
            int64_t ms = str_eq(mode, "seconds") ? now_ms() + (int64_t)(secs * 1000) : (int64_t)secs;
            json_set_int(c, "expires", ms - skew);
        } else if (json_is_str(val)) {
            json_set_str(c, k, json_sv(val));
        } else {
            json_set(c, k, json_clone(val));
        }
    }
    /* keep previous refresh token when the provider doesn't rotate it */
    if (prev && !json_get(c, "refresh") && json_get_str(prev, "refresh")) json_set_str(c, "refresh", json_get_str(prev, "refresh"));
    if (prev) {
        const char *keep[] = {"accountId", "email", "orgId", "orgName"};
        for (size_t i = 0; i < ARRAY_LEN(keep); i++)
            if (!json_get(c, keep[i]) && json_get(prev, keep[i])) json_set(c, keep[i], json_clone(json_get(prev, keep[i])));
    }
    return c;
}

static int token_request(const json_t *tok, json_t *params, const char *state, json_t **resp_out, char **err) {
    char *url = value_of(json_get(tok, "url"));
    if (!url) {
        *err = xstrdup("descriptor has no token url");
        return -1;
    }
    /* extra params */
    const char *k;
    json_t *v;
    json_obj_foreach(json_get_obj(tok, "params"), i, k, v) {
        if (json_is_str(v)) json_set_str(params, k, subst(json_sv(v), state)), free(NULL);
    }
    /* fix: subst returns owned; re-run to avoid leak */
    json_obj_foreach(json_get_obj(tok, "params"), i, k, v) {
        if (json_is_str(v)) {
            char *s = subst(json_sv(v), state);
            json_set_str(params, k, s);
            free(s);
        }
    }
    http_headers_t h = {0};
    json_obj_foreach(json_get_obj(tok, "headers"), i, k, v) {
        if (json_is_str(v)) {
            char *s = subst(json_sv(v), state);
            http_headers_add(&h, k, s);
            free(s);
        }
    }
    bool form = str_eq(json_get_str(tok, "body"), "form");
    char *body;
    if (form) {
        sb_t b = SB_INIT;
        json_obj_foreach(params, i, k, v) {
            char *ek = url_encode(k);
            sb_t vs = SB_INIT;
            if (json_is_str(v)) sb_append(&vs, json_sv(v));
            else json_dump_sb(&vs, v, 0);
            char *ev = url_encode(sb_cstr(&vs));
            sb_appendf(&b, "%s%s=%s", b.len ? "&" : "", ek, ev);
            free(ek);
            free(ev);
            sb_free(&vs);
        }
        body = sb_detach(&b);
        http_headers_set(&h, "Content-Type", "application/x-www-form-urlencoded");
    } else {
        body = json_dump(params, 0);
        http_headers_set(&h, "Content-Type", "application/json");
    }
    http_headers_set(&h, "Accept", "application/json");
    http_req_t req = {.method = "POST", .url = url, .headers = &h, .body = body, .body_len = strlen(body),
                      .timeout_ms = json_get_int(tok, "timeoutMs", 30000)};
    http_resp_t r;
    int rc = http_request(&req, &r);
    free(body);
    free(url);
    http_headers_free(&h);
    if (rc != 0 || r.status < 200 || r.status >= 300) {
        *err = rc != 0 ? xstrdup(r.error ? r.error : "request failed") : ai_extract_error(r.status, r.body);
        http_resp_free(&r);
        return -1;
    }
    *resp_out = json_parse_str(r.body);
    http_resp_free(&r);
    if (!*resp_out) {
        *err = xstrdup("token endpoint returned invalid JSON");
        return -1;
    }
    return 0;
}

static int refresh_oauth(const char *provider, json_t *entry, char **err) {
    json_t *d = descriptor(provider);
    json_t *rf = json_get_obj(d, "refresh");
    if (!rf || !str_eq(json_get_str(rf, "kind"), "request")) {
        *err = xasprintf("no refresh flow for %s", provider);
        return -1;
    }
    const char *rt = json_get_str(entry, "refresh");
    if (!rt) {
        *err = xstrdup("no refresh token stored");
        return -1;
    }
    json_t *tok = json_get_obj(rf, "token");
    json_t *params = json_obj();
    json_set_str(params, "grant_type", "refresh_token");
    json_set_str(params, "refresh_token", rt);
    char *cid = value_of(json_get(json_get_obj(d, "login"), "clientId"));
    if (cid) json_set_str(params, "client_id", cid);
    free(cid);
    json_t *resp = NULL;
    int rc = token_request(tok, params, NULL, &resp, err);
    json_free(params);
    if (rc) return -1;
    json_t *cred_desc = json_get_obj(rf, "credential");
    if (!cred_desc) cred_desc = json_get_obj(json_get_obj(d, "login"), "credential");
    json_t *c = extract_cred(cred_desc, resp, entry);
    json_free(resp);
    json_set(g_store, provider, c);
    save();
    return 0;
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
            int64_t exp = json_get_int(e, "expires", 0);
            if (exp && exp - 60000 < now_ms()) {
                char *err = NULL;
                if (refresh_oauth(provider, e, &err) != 0) {
                    LOGW("oauth refresh for %s failed: %s", provider, err ? err : "?");
                    free(err);
                }
                e = json_get_obj(g_store, provider);
            }
            const char *acc = json_get_str(e, "access");
            if (acc) r = xstrdup(acc);
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
    return ok;
}

/* ---------- login (oauth-code + PKCE) ---------- */

static char *pkce_challenge(const char *verifier) {
    unsigned char dig[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)verifier, strlen(verifier), dig);
    return base64url_encode(dig, sizeof dig);
}

static int listen_port(int port) {
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
        *state = xstrdup(h + 1);
    } else {
        *code = xstrdup(t);
    }
    free(t);
}

int auth_login(const char *provider, const auth_io_t *io, char **err) {
    *err = NULL;
    json_t *d = descriptor(provider);
    json_t *login = json_get_obj(d, "login");
    if (!login || !str_eq(json_get_str(login, "kind"), "oauth-code")) {
        *err = xasprintf("%s does not support browser OAuth login in omc; set an API key with `omc auth set %s <key>`",
                         provider, provider);
        return -1;
    }
    char *client_id = value_of(json_get(login, "clientId"));
    char *auth_url = value_of(json_get(login, "authorizeUrl"));
    json_t *cb = json_get_obj(login, "callback");
    int port = (int)json_get_int(cb, "port", 0);
    char *redirect = value_of(json_get(cb, "redirectUri"));
    if (!redirect)
        redirect = xasprintf("http://%s:%d%s", json_get_str(cb, "hostname") ? json_get_str(cb, "hostname") : "localhost",
                             port, json_get_str(cb, "path") ? json_get_str(cb, "path") : "/callback");
    unsigned char vb[32];
    rand_bytes(vb, sizeof vb);
    char *verifier = base64url_encode(vb, sizeof vb);
    char *challenge = pkce_challenge(verifier);
    char *state = rand_hex(16);
    /* scopes */
    json_t *scopes = json_get_arr(login, "scopes"), *s;
    const char *sep = json_get_str(login, "scopeSeparator");
    sb_t sc = SB_INIT;
    json_arr_foreach(scopes, i, s) sb_appendf(&sc, "%s%s", i ? (sep ? sep : " ") : "", json_sv(s));
    sb_t u = SB_INIT;
    sb_append(&u, auth_url);
    sb_putc(&u, strchr(auth_url, '?') ? '&' : '?');
    const char *k;
    json_t *v;
    json_obj_foreach(json_get_obj(login, "authorizeParams"), i, k, v) {
        char *ev = url_encode(json_sv(v) ? json_sv(v) : "");
        sb_appendf(&u, "%s=%s&", k, ev);
        free(ev);
    }
    char *e_cid = url_encode(client_id ? client_id : ""), *e_red = url_encode(redirect), *e_sc = url_encode(sb_cstr(&sc));
    sb_appendf(&u, "response_type=code&client_id=%s&redirect_uri=%s&scope=%s&code_challenge=%s&code_challenge_method=S256&state=%s",
               e_cid, e_red, e_sc, challenge, state);
    free(e_cid);
    free(e_red);
    free(e_sc);
    sb_free(&sc);
    if (io && io->print) {
        char *m = xasprintf("Open this URL to log in to %s:\n\n%s\n\n%s", json_get_str(d, "name") ? json_get_str(d, "name") : provider,
                            sb_cstr(&u), json_get_str(login, "instructions") ? json_get_str(login, "instructions") : "");
        io->print(io->ud, m);
        free(m);
    }
    /* try to open the browser */
    const char *opener = which("xdg-open") ? "xdg-open" : which("open") ? "open" : NULL;
    if (opener) {
        const char *argv[] = {opener, sb_cstr(&u), NULL};
        proc_opts_t po = {.timeout_ms = 5000, .new_session = true};
        proc_result_t pr;
        if (proc_run(argv, &po, &pr) == 0) proc_result_free(&pr);
    }
    char *code = NULL, *got_state = NULL;
    int lfd = port ? listen_port(port) : -1;
    if (lfd >= 0 && io && io->print) io->print(io->ud, "Waiting for the browser redirect (or paste the code / redirect URL and press Enter)...");
    int64_t deadline = mono_ms() + 300000;
    sb_t typed = SB_INIT;
    while (!code && mono_ms() < deadline) {
        struct pollfd pf[2];
        int np = 0;
        pf[np++] = (struct pollfd){0, POLLIN, 0};
        if (lfd >= 0) pf[np++] = (struct pollfd){lfd, POLLIN, 0};
        int pr = poll(pf, (nfds_t)np, 500);
        if (pr <= 0) continue;
        if (pf[0].revents & POLLIN) {
            char buf[4096];
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) break;
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
    if (lfd >= 0) close(lfd);
    int rc = -1;
    if (!code) {
        *err = xstrdup("login timed out or was cancelled");
    } else if (got_state && strcmp(got_state, state) != 0) {
        *err = xstrdup("OAuth state mismatch; aborting");
    } else {
        json_t *tok = json_get_obj(login, "token");
        json_t *params = json_obj();
        json_set_str(params, "grant_type", "authorization_code");
        json_set_str(params, "code", code);
        json_set_str(params, "redirect_uri", redirect);
        if (client_id) json_set_str(params, "client_id", client_id);
        json_set_str(params, "code_verifier", verifier);
        json_t *resp = NULL;
        if (token_request(tok, params, state, &resp, err) == 0) {
            json_t *c = extract_cred(json_get_obj(login, "credential"), resp, NULL);
            json_free(resp);
            pthread_mutex_lock(&g_mu);
            load();
            json_set(g_store, provider, c);
            rc = save() == 0 ? 0 : -1;
            pthread_mutex_unlock(&g_mu);
            if (rc) *err = xstrdup("failed to save credentials");
        }
        json_free(params);
    }
    free(code);
    free(got_state);
    free(client_id);
    free(auth_url);
    free(redirect);
    free(verifier);
    free(challenge);
    free(state);
    sb_free(&u);
    return rc;
}
