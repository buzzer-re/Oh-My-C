/* auth_engine.c - declarative login engines and hooks, ported from omp v18.14.2
 * packages/ai/src/registry/engine/{common,device-code,api-key,refresh}.ts, registry/api-key-validation.ts,
 * registry/oauth/device-code.ts and the device-code / oauth-code hooks of registry/hooks/. */
#define _GNU_SOURCE
#include "config/auth_engine.h"

#include "ai/ai.h"
#include "ai/google_cca.h"
#include "ai/provider_anthropic.h"
#include "core/fs.h"

#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <strings.h>
#include <sys/utsname.h>
#include <unistd.h>

_Static_assert(sizeof(sig_atomic_t) == sizeof(int), "abort flag doubles as the http cancel flag");

#define OMP_UPSTREAM_VERSION "18.14.2" /* pi-ai package version omp v18.14.2 reports (Kimi fingerprint) */

/* ---------- login context ---------- */

bool actx_aborted(const actx_t *c) { return c && c->h && c->h->abort && *c->h->abort; }

void actx_progress(const actx_t *c, const char *msg) {
    if (c && c->h && c->h->on_progress) c->h->on_progress(c->h->ud, msg);
}

void actx_auth(const actx_t *c, const char *url, const char *launch, const char *instructions) {
    if (c && c->h && c->h->on_auth) c->h->on_auth(c->h->ud, url, launch, instructions);
}

char *actx_prompt(const actx_t *c, const char *message, const char *placeholder, bool allow_empty, char **err) {
    if (!c->h || !c->h->prompt) {
        *err = xasprintf("%s login requires onPrompt callback", c->label);
        return NULL;
    }
    auth_prompt_t p = {message, placeholder, allow_empty, false};
    char *a = c->h->prompt(c->h->ud, &p);
    if (!a || actx_aborted(c)) {
        free(a);
        *err = xstrdup("Login cancelled");
        return NULL;
    }
    return a;
}

bool actx_sleep(const actx_t *c, int64_t ms) {
    int64_t end = mono_ms() + ms;
    for (;;) {
        if (actx_aborted(c)) return false;
        int64_t left = end - mono_ms();
        if (left <= 0) return true;
        sleep_ms(MIN(left, 50));
    }
}

static char *trim_dup(const char *s) { return str_trim(s ? s : ""); }

static char *rtrim_slashes(char *s) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') s[--n] = 0;
    return s;
}

/* ---------- HTTP ---------- */

char *auth_rewrite_url(const char *url) {
    const char *rw = env_get("OMC_AUTH_URL_REWRITE");
    char *out = NULL;
    if (rw && url) {
        strvec_t parts = str_split(rw, ",");
        for (size_t i = 0; i < parts.n && !out; i++) {
            char *eq = strstr(parts.v[i], "=");
            if (!eq) continue;
            *eq = 0;
            char *from = str_trim(parts.v[i]), *to = str_trim(eq + 1);
            if (*from && str_starts(url, from)) out = xasprintf("%s%s", to, url + strlen(from));
            free(from);
            free(to);
        }
        strvec_free(&parts);
    }
    return out ? out : xstrdup(url ? url : "");
}

int auth_http(const char *method, const char *url, const http_headers_t *h, const char *body, int64_t timeout_ms,
              const actx_t *c, http_resp_t *r) {
    char *u = auth_rewrite_url(url);
    http_req_t req = {.method = method,
                      .url = u,
                      .headers = h,
                      .body = body,
                      .body_len = body ? strlen(body) : 0,
                      .timeout_ms = timeout_ms > 0 ? timeout_ms : 30000,
                      .cancel = c && c->h ? (volatile sig_atomic_t *)c->h->abort : NULL};
    int rc = http_request(&req, r);
    free(u);
    return rc;
}

/* application/x-www-form-urlencoded (URLSearchParams) */
static void form_encode(sb_t *b, const char *s) {
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (isalnum(*c) || *c == '*' || *c == '-' || *c == '.' || *c == '_') sb_putc(b, (char)*c);
        else if (*c == ' ') sb_putc(b, '+');
        else sb_appendf(b, "%%%02X", *c);
    }
}

static char *form_body(const json_t *params) {
    sb_t b = SB_INIT;
    const char *k;
    json_t *v;
    json_obj_foreach(params, i, k, v) {
        if (b.len) sb_putc(&b, '&');
        form_encode(&b, k);
        sb_putc(&b, '=');
        form_encode(&b, json_sv(v) ? json_sv(v) : "");
    }
    return sb_detach(&b);
}

static json_t *parse_body(const http_resp_t *r) {
    const char *t = r->body ? r->body : "";
    json_t *j = *t ? json_parse_str(t) : NULL;
    return j ? j : json_str(t);
}

static char *excerpt(const char *s, size_t n) {
    s = s ? s : "";
    size_t l = strlen(s);
    if (l <= n) return xstrdup(s);
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return xstrndup(s, n);
}

/* ---------- values, templates, JWT ---------- */

static char *value_hook(const char *name, const actx_t *c, char **err);

char *auth_resolve_value(const json_t *v, const actx_t *c, char **err) {
    if (!v) return NULL;
    if (json_is_str(v)) return xstrdup(json_sv(v));
    json_t *e;
    json_arr_foreach(json_get_arr(v, "env"), i, e) {
        const char *ev = json_is_str(e) ? env_get(json_sv(e)) : NULL;
        char *t = ev ? str_trim(ev) : NULL;
        if (t && *t) return t;
        free(t);
    }
    if (json_get_str(v, "hook")) return value_hook(json_get_str(v, "hook"), c, err);
    const char *val = json_get_str(v, "value");
    if (!val) return xstrdup("");
    if (str_eq(json_get_str(v, "encoding"), "base64")) {
        size_t n;
        unsigned char *d = base64_decode(val, strlen(val), &n);
        if (!d) return xstrdup("");
        char *r = xstrndup((char *)d, n);
        free(d);
        return r;
    }
    return xstrdup(val);
}

char *auth_template(const char *text, const json_t *vars) {
    sb_t b = SB_INIT;
    for (const char *p = text ? text : ""; *p;) {
        if (*p == '{') {
            const char *q = p + 1;
            while ((*q >= 'a' && *q <= 'z') || *q == '_') q++;
            if (*q == '}' && q > p + 1) {
                char *name = xstrndup(p + 1, (size_t)(q - p - 1));
                const char *v = json_get_str(vars, name);
                if (v) sb_append(&b, v);
                free(name);
                p = q + 1;
                continue;
            }
        }
        sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

json_t *auth_jwt_payload(const char *token) {
    if (!token) return NULL;
    const char *a = strchr(token, '.');
    const char *b = a ? strchr(a + 1, '.') : NULL;
    if (!a || !b || b == a + 1 || strchr(b + 1, '.')) return NULL;
    size_t n;
    unsigned char *d = base64_decode(a + 1, (size_t)(b - a - 1), &n);
    if (!d) return NULL;
    json_t *j = json_parse((const char *)d, n, NULL);
    free(d);
    if (!json_is_obj(j)) {
        json_free(j);
        return NULL;
    }
    return j;
}

static void vset(json_t *vars, const char *k, const char *v) {
    if (v) json_set_str(vars, k, v);
}

/* ---------- token requests ---------- */

int auth_post_token(const json_t *req, const json_t *std_params, const json_t *vars, const http_headers_t *extra,
                    const char *provider, const char *kind, const actx_t *c, int *status, json_t **body_out, char **err) {
    *body_out = NULL;
    if (status) *status = 0;
    char *raw_url = auth_resolve_value(json_get(req, "url"), c, err);
    if (!raw_url) {
        if (!*err) *err = xasprintf("%s auth rule has no request url", provider);
        return -1;
    }
    char *url = auth_template(raw_url, vars);
    free(raw_url);
    json_t *params = json_obj();
    const char *k;
    json_t *v;
    if (json_get_bool(req, "standard", true)) json_obj_foreach(std_params, i, k, v) if (json_sv(v)) json_set_str(params, k, json_sv(v));
    json_obj_foreach(json_get_obj(req, "params"), i, k, v) {
        char *t = auth_template(json_sv(v) ? json_sv(v) : "", vars);
        json_set_str(params, k, t);
        free(t);
    }
    http_headers_t h = {0};
    if (extra)
        for (size_t i = 0; i < extra->n; i++) http_headers_set(&h, extra->v[i].name, extra->v[i].value);
    json_obj_foreach(json_get_obj(req, "headers"), i, k, v) {
        char *t = auth_template(json_sv(v) ? json_sv(v) : "", vars);
        http_headers_set(&h, k, t);
        free(t);
    }
    bool js = str_eq(json_get_str(req, "body"), "json");
    http_headers_set(&h, "Content-Type", js ? "application/json" : "application/x-www-form-urlencoded");
    char *body = js ? json_dump(params, 0) : form_body(params);
    json_free(params);
    http_resp_t r;
    int rc = auth_http("POST", url, &h, body, json_get_int(req, "timeoutMs", 30000), c, &r);
    free(body);
    http_headers_free(&h);
    if (rc != 0) {
        if (actx_aborted(c)) *err = xstrdup("Login cancelled");
        else if (r.error && strstr(r.error, "timed out")) *err = xasprintf("Timed out waiting for %s", url);
        else *err = xasprintf("%s request to %s failed: %s", provider, url, r.error ? r.error : "request failed");
        http_resp_free(&r);
        free(url);
        return -1;
    }
    free(url);
    if (status) *status = r.status;
    bool ok = r.status >= 200 && r.status < 300;
    if (!ok && !str_eq(kind, "device-auth")) {
        char *ex = excerpt(r.body, 500);
        *err = xasprintf("%s %s failed: %d %s", provider, str_eq(kind, "token-refresh") ? "token refresh" : "token exchange",
                         r.status, ex);
        free(ex);
        http_resp_free(&r);
        return -1;
    }
    *body_out = parse_body(&r);
    http_resp_free(&r);
    return 0;
}

/* ---------- credential projection ---------- */

static char *scalar_string(const json_t *v) {
    if (json_is_str(v)) return *json_sv(v) ? xstrdup(json_sv(v)) : NULL;
    if (json_is_num(v)) {
        double d = v->u.num.d;
        if (d != d || d > 1e300 || d < -1e300) return NULL;
        if (d == (double)(int64_t)d && d < 9.007199254740992e15 && d > -9.007199254740992e15) return xasprintf("%lld", (long long)d);
        return xasprintf("%.17g", d);
    }
    return NULL;
}

static char *read_field(const json_t *field, const json_t *body, const json_t *claims) {
    if (!json_is_obj(field)) return NULL;
    const char *path = json_get_str(field, "path");
    if (path) {
        char *s = scalar_string(json_path(body, path));
        if (s) return s;
    }
    json_t *cl;
    json_arr_foreach(json_get_arr(field, "claim"), i, cl) {
        char *s = json_sv(cl) ? scalar_string(json_get(claims, json_sv(cl))) : NULL;
        if (s) return s;
    }
    return json_get_str(field, "literal") ? xstrdup(json_get_str(field, "literal")) : NULL;
}

static const char *const OPTIONAL_FIELDS[] = {"email", "accountId", "orgId", "orgName", "projectId", "apiEndpoint", "enterpriseUrl"};

json_t *auth_map_credentials(const json_t *map, const json_t *body, const char *provider, const json_t *prev, char **err) {
    char *access = read_field(json_get(map, "access"), body, NULL);
    json_t *claims = access ? auth_jwt_payload(access) : NULL;
    if (!access) {
        char *dump = json_is_str(body) ? xstrdup(json_sv(body)) : json_dump(body, 0);
        char *ex = excerpt(dump, 500);
        *err = xasprintf("%s token response missing access token: %s", provider, ex);
        free(ex);
        free(dump);
        return NULL;
    }
    int64_t expires;
    json_t *ex = json_get_obj(map, "expires");
    const char *mode = ex ? json_get_str(ex, "mode") : "never";
    int64_t skew = json_get_int(ex, "skewMs", 0);
    if (str_eq(mode, "never")) {
        expires = AUTH_NEVER_EXPIRES;
    } else if (str_eq(mode, "jwt")) {
        json_t *exp = json_get(claims, "exp");
        expires = json_is_num(exp) ? (int64_t)(exp->u.num.d * 1000) - skew : now_ms() + json_get_int(ex, "fallbackMs", 3600000);
    } else {
        const char *path = json_get_str(ex, "path") ? json_get_str(ex, "path") : "expires_in";
        json_t *secs = json_path(body, path);
        if (json_is_num(secs)) {
            json_t *from = json_get_str(ex, "fromPath") ? json_path(body, json_get_str(ex, "fromPath")) : NULL;
            int64_t base = json_is_num(from) ? (int64_t)(from->u.num.d * 1000) : now_ms();
            expires = base + (int64_t)(secs->u.num.d * 1000) - skew;
        } else if (json_get(ex, "fallbackMs")) {
            expires = now_ms() + json_get_int(ex, "fallbackMs", 0);
        } else {
            *err = xasprintf("%s token response missing %s", provider, path);
            free(access);
            json_free(claims);
            return NULL;
        }
    }
    json_t *c = json_obj();
    json_set_str(c, "type", "oauth");
    json_set_str(c, "access", access);
    char *refresh = read_field(json_get(map, "refresh"), body, claims);
    json_set_str(c, "refresh", refresh ? refresh : json_get_str(prev, "refresh") ? json_get_str(prev, "refresh") : "");
    free(refresh);
    json_set_int(c, "expires", expires);
    for (size_t i = 0; i < ARRAY_LEN(OPTIONAL_FIELDS); i++) {
        char *s = read_field(json_get(map, OPTIONAL_FIELDS[i]), body, claims);
        if (s) json_set_str(c, OPTIONAL_FIELDS[i], s);
        free(s);
    }
    free(access);
    json_free(claims);
    return c;
}

void auth_apply_userinfo(const json_t *ui, json_t *cred, const http_headers_t *extra, const json_t *vars, const actx_t *c) {
    const char *u = json_get_str(ui, "url");
    const char *acc = json_get_str(cred, "access");
    if (!u || !acc) return;
    char *url = auth_template(u, vars);
    http_headers_t h = {0};
    if (extra)
        for (size_t i = 0; i < extra->n; i++) http_headers_set(&h, extra->v[i].name, extra->v[i].value);
    char *bearer = xasprintf("Bearer %s", acc);
    http_headers_set(&h, "Authorization", bearer);
    free(bearer);
    http_resp_t r;
    if (auth_http("GET", url, &h, NULL, 30000, c, &r) == 0 && r.status >= 200 && r.status < 300) {
        json_t *j = json_parse_str(r.body ? r.body : "");
        static const char *const fields[] = {"email", "accountId"};
        for (size_t i = 0; i < ARRAY_LEN(fields); i++) {
            const char *path = json_get_str(ui, fields[i]);
            char *s = path ? scalar_string(json_path(j, path)) : NULL;
            if (s) json_set_str(cred, fields[i], s);
            free(s);
        }
        json_free(j);
    }
    http_resp_free(&r);
    http_headers_free(&h);
    free(url);
}

/* ---------- headers hooks ---------- */

static char *sanitize_header(const char *v, const char *fallback) {
    sb_t b = SB_INIT;
    for (const unsigned char *p = (const unsigned char *)(v ? v : ""); *p; p++)
        if (*p >= 0x20 && *p <= 0x7E) sb_putc(&b, (char)*p);
    char *t = str_trim(sb_cstr(&b));
    sb_free(&b);
    if (!*t) {
        free(t);
        return xstrdup(fallback);
    }
    return t;
}

static const char *node_arch(const char *machine) {
    if (str_eq(machine, "x86_64") || str_eq(machine, "amd64")) return "x64";
    if (str_eq(machine, "aarch64") || str_eq(machine, "arm64")) return "arm64";
    if (str_eq(machine, "i386") || str_eq(machine, "i686")) return "ia32";
    if (str_starts(machine, "arm")) return "arm";
    return machine;
}

/* registry/oauth/kimi.ts getKimiCommonHeaders (process-stable) */
static pthread_mutex_t g_kimi_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_kimi_device_id;

static const char *kimi_device_id(void) {
    pthread_mutex_lock(&g_kimi_mu);
    if (!g_kimi_device_id) {
        char *path = omc_agent_path("kimi-device-id");
        size_t n;
        char *s = fs_read(path, &n);
        char *t = s ? str_trim(s) : NULL;
        free(s);
        if (t && *t) {
            g_kimi_device_id = t;
        } else {
            free(t);
            char *u = uuid_v4(), *w = str_replace(u, "-", "");
            free(u);
            g_kimi_device_id = w;
            char *line = xasprintf("%s\n", w);
            fs_write_atomic(path, line, strlen(line), 0600); /* best effort */
            free(line);
        }
        free(path);
    }
    pthread_mutex_unlock(&g_kimi_mu);
    return g_kimi_device_id;
}

static void kimi_headers(http_headers_t *h) {
    struct utsname un;
    memset(&un, 0, sizeof un);
    uname(&un);
    char host[256] = "";
    gethostname(host, sizeof host - 1);
    const char *arch = node_arch(un.machine);
#ifdef __APPLE__
    char *model = xasprintf("macOS %s %s", un.release, arch);
#else
    char *model = str_eq(un.sysname, "Linux") ? xasprintf("Linux %s %s", un.release, arch)
                                              : xasprintf("%s %s %s", un.sysname, un.release, arch);
    for (char *p = model; !str_eq(un.sysname, "Linux") && *p && *p != ' '; p++) *p = (char)tolower((unsigned char)*p);
#endif
    char *ua = xasprintf("KimiCLI/%s", OMP_UPSTREAM_VERSION);
    http_headers_set(h, "User-Agent", ua);
    http_headers_set(h, "X-Msh-Platform", "kimi_cli");
    http_headers_set(h, "X-Msh-Version", OMP_UPSTREAM_VERSION);
    char *a = sanitize_header(host, "unknown"), *b = sanitize_header(model, "unknown"), *c = sanitize_header(un.version, "unknown"),
         *d = sanitize_header(kimi_device_id(), "unknown");
    http_headers_set(h, "X-Msh-Device-Name", a);
    http_headers_set(h, "X-Msh-Device-Model", b);
    http_headers_set(h, "X-Msh-Os-Version", c);
    http_headers_set(h, "X-Msh-Device-Id", d);
    free(a);
    free(b);
    free(c);
    free(d);
    free(ua);
    free(model);
}

/* catalog/wire/coreweave.ts resolveCoreWeaveProject */
static char *coreweave_project(void) {
    const char *names[] = {"COREWEAVE_PROJECT", "WANDB_INFERENCE_PROJECT"};
    for (size_t i = 0; i < ARRAY_LEN(names); i++) {
        char *t = trim_dup(env_get(names[i]));
        if (*t) return t;
        free(t);
    }
    char *proj = trim_dup(env_get("WANDB_PROJECT"));
    if (!*proj) {
        free(proj);
        return NULL;
    }
    if (strchr(proj, '/')) return proj;
    char *ent = trim_dup(env_get("WANDB_ENTITY"));
    char *r = *ent ? xasprintf("%s/%s", ent, proj) : NULL;
    free(ent);
    free(proj);
    return r;
}

int auth_headers_hook(const char *name, http_headers_t *h, char **err) {
    if (!name) return 0;
    if (str_eq(name, "kimi-fingerprint")) {
        kimi_headers(h);
        return 0;
    }
    if (str_eq(name, "coreweave-project")) {
        char *p = coreweave_project();
        if (!p) {
            *err = xstrdup("CoreWeave Serverless Inference requires OpenAI-Project. Set COREWEAVE_PROJECT=<team>/<project> before "
                           "running /login coreweave. To persist it, add export COREWEAVE_PROJECT=<team>/<project> to your "
                           "shell startup file (for example ~/.zshrc, ~/.bashrc, or your shell's profile/rc file).");
            return -1;
        }
        http_headers_set(h, "OpenAI-Project", p);
        free(p);
        return 0;
    }
    if (str_eq(name, "cline-pass-client")) {
        http_headers_set(h, "HTTP-Referer", "https://cline.bot");
        http_headers_set(h, "X-Title", "Cline");
        http_headers_set(h, "X-IS-MULTIROOT", "false");
        http_headers_set(h, "X-CLIENT-TYPE", "cline-sdk");
        http_headers_set(h, "User-Agent", "Cline/3.0.58");
        http_headers_set(h, "X-CLIENT-VERSION", "3.0.58");
#ifdef __APPLE__
        http_headers_set(h, "X-PLATFORM", "darwin");
#else
        http_headers_set(h, "X-PLATFORM", "linux");
#endif
        http_headers_set(h, "X-PLATFORM-VERSION", "3.0.54");
        http_headers_set(h, "X-CORE-VERSION", "0.0.79");
        return 0;
    }
    *err = xasprintf("auth rules reference unknown headers hook \"%s\"", name);
    return -1;
}

/* ---------- value hooks ---------- */

static bool host_is(const char *url, const char *domain) {
    url_t u;
    if (url_parse(url, &u) != 0) return false;
    bool ok = str_eq(u.scheme, "https") && u.host && *u.host;
    if (ok) {
        char *h = str_lower(u.host);
        size_t hl = strlen(h), dl = strlen(domain);
        ok = str_eq(h, domain) || (hl > dl + 1 && h[hl - dl - 1] == '.' && str_eq(h + hl - dl, domain));
        free(h);
    }
    url_free(&u);
    return ok;
}

/* registry/oauth/xai-oauth.ts getXAITokenEndpoint: OIDC discovery pinned to https *.x.ai */
static char *xai_token_endpoint(const actx_t *c, char **err) {
    http_headers_t h = {0};
    http_headers_set(&h, "Accept", "application/json");
    http_resp_t r;
    int rc = auth_http("GET", "https://auth.x.ai/.well-known/openid-configuration", &h, NULL, 15000, c, &r);
    http_headers_free(&h);
    char *out = NULL;
    if (rc != 0) {
        *err = xasprintf("xAI OIDC discovery failed: %s", r.error ? r.error : "request failed");
    } else if (r.status != 200) {
        *err = xasprintf("xAI OIDC discovery returned status %d.", r.status);
    } else {
        json_t *j = json_parse_str(r.body ? r.body : "");
        char *te = json_is_obj(j) ? trim_dup(json_get_str(j, "token_endpoint")) : NULL;
        if (!j) *err = xstrdup("xAI OIDC discovery returned invalid JSON");
        else if (!json_is_obj(j)) *err = xstrdup("xAI OIDC discovery response was not a JSON object.");
        else if (!*te) *err = xstrdup("xAI OIDC discovery response was missing token_endpoint.");
        else if (!host_is(te, "x.ai")) *err = xasprintf("Invalid xAI token_endpoint: %s", te);
        else out = xstrdup(te);
        free(te);
        json_free(j);
    }
    http_resp_free(&r);
    return out;
}

static char *value_hook(const char *name, const actx_t *c, char **err) {
    if (str_eq(name, "xai-token-endpoint")) return xai_token_endpoint(c, err);
    *err = xasprintf("auth rules reference unknown value hook \"%s\"", name);
    return NULL;
}

/* ---------- after-exchange hooks ---------- */

static void cca_progress(void *ud, const char *msg) { actx_progress(ud, msg); }

static json_t *jget2(const json_t *o, const char *a, const char *b) { return json_get(json_get(o, a), b); }

static char *lower_trim(const char *s) {
    char *t = trim_dup(s), *l = str_lower(t);
    free(t);
    return l;
}

/* registry/oauth/openai-codex.ts openAICodexProfileHook */
static int codex_profile(const actx_t *c, const char *phase, json_t *cred, const json_t *raw, const json_t *stored, char **err) {
    json_t *p = auth_jwt_payload(json_get_str(cred, "access"));
    json_t *ip = auth_jwt_payload(json_get_str(raw, "id_token"));
    const char *AUTH = "https://api.openai.com/auth", *PROF = "https://api.openai.com/profile";
    const char *acct = json_sv(jget2(p, AUTH, "chatgpt_account_id"));
    if (!acct) acct = json_sv(jget2(ip, AUTH, "chatgpt_account_id"));
    const char *em = json_sv(jget2(p, PROF, "email"));
    if (!em) em = json_sv(jget2(ip, PROF, "email"));
    const char *plan = json_sv(jget2(p, AUTH, "chatgpt_plan_type"));
    if (!plan) plan = json_sv(jget2(ip, AUTH, "chatgpt_plan_type"));
    char *email = em ? lower_trim(em) : NULL, *planl = plan ? lower_trim(plan) : NULL;
    if (acct && !*acct) acct = NULL;
    if (email && !*email) {
        free(email);
        email = NULL;
    }
    if (planl && !*planl) {
        free(planl);
        planl = NULL;
    }
    bool login = str_eq(phase, "login");
    int rc = 0;
    if (login && !acct && !email) {
        *err = xstrdup("Failed to extract account identity from token");
        rc = -1;
    } else {
        const char *ra = acct ? acct : json_get_str(stored, "accountId");
        if (ra) json_set_str(cred, "accountId", ra);
        if (email) json_set_str(cred, "email", email);
        else if (json_get_str(stored, "email")) json_set_str(cred, "email", json_get_str(stored, "email"));
        if (login && ra) json_set_str(cred, "orgId", ra);
        if (login && planl) json_set_str(cred, "orgName", planl);
        if (!login && json_get_str(stored, "orgId")) json_set_str(cred, "orgId", json_get_str(stored, "orgId"));
        if (!login && json_get_str(stored, "orgName")) json_set_str(cred, "orgName", json_get_str(stored, "orgName"));
    }
    free(email);
    free(planl);
    json_free(p);
    json_free(ip);
    return rc;
}

/* registry/oauth/anthropic.ts anthropicIdentityHook (bootstrap identity; failures keep the credential) */
static void anthropic_identity(const actx_t *c, const char *phase, json_t *cred) {
    bool include_org = str_eq(phase, "login");
    if (json_get_str(cred, "accountId") && json_get_str(cred, "email") && (!include_org || json_get_str(cred, "orgId"))) return;
    http_headers_t h = {0};
    char *bearer = xasprintf("Bearer %s", json_get_str(cred, "access"));
    char *ua = xasprintf("claude-code/%s", anthropic_claude_code_version());
    http_headers_set(&h, "Accept", "application/json, text/plain, */*");
    http_headers_set(&h, "Authorization", bearer);
    http_headers_set(&h, "Content-Type", "application/json");
    http_headers_set(&h, "User-Agent", ua);
    http_headers_set(&h, "anthropic-beta", "oauth-2025-04-20");
    http_resp_t r;
    if (auth_http("GET", "https://api.anthropic.com/api/claude_cli/bootstrap?entrypoint=cli&model=claude-opus-4-8", &h, NULL,
                  30000, c, &r) == 0 &&
        r.status >= 200 && r.status < 300) {
        json_t *j = json_parse_str(r.body ? r.body : "");
        json_t *oa = json_get_obj(j, "oauth_account");
        static const char *const map[][2] = {{"accountId", "account_uuid"}, {"email", "account_email"},
                                             {"orgId", "organization_uuid"}, {"orgName", "organization_name"}};
        for (size_t i = 0; i < ARRAY_LEN(map); i++) {
            if (i >= 2 && !include_org) break;
            const char *v = json_get_str(oa, map[i][1]);
            if (!json_get_str(cred, map[i][0]) && v && *v) json_set_str(cred, map[i][0], v);
        }
        json_free(j);
    }
    http_resp_free(&r);
    http_headers_free(&h);
    free(bearer);
    free(ua);
}

/* registry/oauth/zai.ts: business login -> default org/project -> find/create the "oh-my-pi" key -> copy secret */
static json_t *zai_unwrap(json_t *body, const char *op, char **err) {
    if (json_is_obj(body) && (json_get(body, "code") || json_get(body, "success"))) {
        json_t *code = json_get(body, "code");
        bool ok_code = json_is_null(code) || (json_is_num(code) && (code->u.num.d == 0 || code->u.num.d == 200)) ||
                       (json_is_str(code) && (str_eq(json_sv(code), "0") || str_eq(json_sv(code), "200")));
        json_t *succ = json_get(body, "success");
        if ((json_is_bool(succ) && !json_truthy(succ)) || !ok_code) {
            const char *msg = json_get_str(body, "msg");
            char *cs = code ? json_dump(code, 0) : xstrdup("undefined");
            *err = msg ? xasprintf("Z.ai %s failed: %s", op, msg) : xasprintf("Z.ai %s failed: code %s", op, cs);
            free(cs);
            return NULL;
        }
        return json_get(body, "data") ? json_get(body, "data") : body;
    }
    return body;
}

static json_t *zai_req(const actx_t *c, const char *method, const char *url, const char *bearer, const char *body, char **err) {
    http_headers_t h = {0};
    if (bearer) {
        char *b = xasprintf("Bearer %s", bearer);
        http_headers_set(&h, "Authorization", b);
        free(b);
    }
    if (body) http_headers_set(&h, "Content-Type", "application/json");
    http_resp_t r;
    int rc = auth_http(method, url, &h, body, 30000, c, &r);
    http_headers_free(&h);
    json_t *j = NULL;
    if (rc != 0) *err = xasprintf("HTTP request failed. url=%s; error=%s", url, r.error ? r.error : "?");
    else if (r.status < 200 || r.status >= 300)
        *err = xasprintf("HTTP request failed. status=%d; url=%s; body=%s", r.status, url, r.body ? r.body : "");
    else if (!(j = r.body && *r.body ? json_parse_str(r.body) : json_null()))
        *err = xasprintf("Z.ai returned invalid JSON from %s", url);
    http_resp_free(&r);
    return j;
}

static const json_t *pick_default(const json_t *arr, const char *flag) {
    json_t *e;
    json_arr_foreach(arr, i, e) if (json_truthy(json_get(e, flag))) return e;
    return json_at(arr, 0);
}

static int zai_mint(const actx_t *c, json_t *cred, char **err) {
    const char *biz = env_get("ZAI_BIZ_BASE") ? env_get("ZAI_BIZ_BASE") : "https://api.z.ai";
    const char *login_url = env_get("ZAI_BUSINESS_LOGIN_URL") ? env_get("ZAI_BUSINESS_LOGIN_URL") : "https://api.z.ai/api/auth/z/login";
    json_t *lb = json_obj();
    json_set_str(lb, "token", json_get_str(cred, "access"));
    char *lbs = json_dump(lb, 0);
    json_free(lb);
    json_t *resp = zai_req(c, "POST", login_url, NULL, lbs, err), *d;
    free(lbs);
    int rc = -1;
    char *biz_token = NULL, *keys_url = NULL, *api_key = NULL;
    json_t *cust = NULL, *list = NULL, *created = NULL, *copied = NULL;
    if (!resp || !(d = zai_unwrap(resp, "business login", err))) goto out;
    biz_token = trim_dup(json_get_str(d, "access_token") ? json_get_str(d, "access_token") : json_get_str(d, "accessToken"));
    if (!*biz_token) {
        *err = xstrdup("Z.ai business login returned no access token");
        goto out;
    }
    char *cu = xasprintf("%s/api/biz/customer/getCustomerInfo", biz);
    cust = zai_req(c, "GET", cu, biz_token, NULL, err);
    free(cu);
    if (!cust || !(d = zai_unwrap(cust, "customer lookup", err))) goto out;
    const json_t *org = pick_default(json_get_arr(d, "organizations"), "isDefault");
    const json_t *proj = pick_default(json_get_arr(org, "projects"), "isDefault");
    char *oid = trim_dup(json_get_str(org, "organizationId")), *pid = trim_dup(json_get_str(proj, "projectId"));
    if (!*oid || !*pid) {
        *err = xstrdup("Z.ai key provisioning failed: no organization/project on account");
        free(oid);
        free(pid);
        goto out;
    }
    keys_url = xasprintf("%s/api/biz/v1/organization/%s/projects/%s/api_keys", biz, oid, pid);
    free(oid);
    free(pid);
    list = zai_req(c, "GET", keys_url, biz_token, NULL, err);
    if (!list || !(d = zai_unwrap(list, "api key list", err))) goto out;
    const json_t *arr = json_is_arr(d) ? d : NULL;
    static const char *const wrappers[] = {"list", "keys", "apiKeys", "records"};
    for (size_t i = 0; !arr && json_is_obj(d) && i < ARRAY_LEN(wrappers); i++) arr = json_get_arr(d, wrappers[i]);
    const json_t *rec = NULL, *e;
    json_arr_foreach(arr, i, e) if (!rec && str_eq(json_get_str(e, "name"), "oh-my-pi")) rec = e;
    if (!rec) {
        created = zai_req(c, "POST", keys_url, biz_token, "{\"name\":\"oh-my-pi\"}", err);
        if (!created || !(rec = zai_unwrap(created, "api key create", err))) goto out;
    }
    api_key = trim_dup(json_get_str(rec, "apiKey"));
    if (!*api_key) {
        *err = xstrdup("Z.ai key provisioning returned no apiKey");
        goto out;
    }
    char *ek = url_encode(api_key), *copy_url = xasprintf("%s/copy/%s", keys_url, ek);
    free(ek);
    copied = zai_req(c, "GET", copy_url, biz_token, NULL, err);
    free(copy_url);
    if (!copied || !(d = zai_unwrap(copied, "api key copy", err))) goto out;
    char *secret = trim_dup(json_get_str(d, "secretKey"));
    if (!*secret) {
        *err = xstrdup("Z.ai key provisioning returned no secretKey");
        free(secret);
        goto out;
    }
    char *key = xasprintf("%s.%s", api_key, secret);
    json_set_str(cred, "access", key);
    free(key);
    free(secret);
    rc = 0;
out:
    json_free(resp);
    json_free(cust);
    json_free(list);
    json_free(created);
    json_free(copied);
    free(biz_token);
    free(keys_url);
    free(api_key);
    return rc;
}

/* registry/oauth/muse-code.ts attachMuseCodeApiKey: {"oauthAccessToken","apiKey"} bearer */
static int muse_code_key(const actx_t *c, json_t *cred, char **err) {
    json_t *existing = json_parse_str(json_get_str(cred, "access") ? json_get_str(cred, "access") : "");
    bool have = json_is_obj(existing) && json_get_str(existing, "oauthAccessToken") && json_get_str(existing, "apiKey");
    if (have) {
        char *a = trim_dup(json_get_str(existing, "apiKey")), *o = trim_dup(json_get_str(existing, "oauthAccessToken"));
        have = *a && *o;
        free(a);
        free(o);
    }
    json_free(existing);
    if (have) return 0;
    http_headers_t h = {0};
    char *bearer = xasprintf("Bearer %s", json_get_str(cred, "access"));
    http_headers_set(&h, "Accept", "application/json");
    http_headers_set(&h, "Authorization", bearer);
    http_headers_set(&h, "Content-Type", "application/json");
    http_headers_set(&h, "x-api-version", "1.0.0");
    free(bearer);
    http_resp_t r;
    int rc = auth_http("POST", "https://api.meta.ai/muse-code/key", &h, "{\"onboard\":true}", 20000, c, &r);
    http_headers_free(&h);
    if (rc != 0) {
        *err = actx_aborted(c) ? xstrdup("Login cancelled") : xasprintf("Muse Code key exchange failed: %s", r.error ? r.error : "?");
        http_resp_free(&r);
        return -1;
    }
    if (r.status < 200 || r.status >= 300) {
        char *t = r.body ? trim_dup(r.body) : xstrdup(""), *ex = excerpt(t, 500), *ext = trim_dup(ex);
        *err = *t ? xasprintf("Muse Code key exchange failed: %d %s", r.status, ext) : xasprintf("Muse Code key exchange failed: %d", r.status);
        free(t);
        free(ex);
        free(ext);
        http_resp_free(&r);
        return -1;
    }
    json_t *p = json_parse_str(r.body ? r.body : "");
    http_resp_free(&r);
    rc = -1;
    if (!json_is_obj(p)) {
        *err = xstrdup(p ? "Invalid Muse Code key response: must be an object" : "Muse Code key exchange returned invalid JSON");
        json_free(p);
        return -1;
    }
    json_t *active = json_get(p, "is_subs_active");
    char *key = trim_dup(json_get_str(p, "api_key"));
    if (json_is_bool(active) && !json_truthy(active)) {
        *err = xstrdup("invalid_grant: Muse Code subscription is inactive");
    } else if (!*key) {
        char *au = trim_dup(json_get_str(p, "action_url"));
        if (!*au) {
            free(au);
            au = trim_dup(json_get_str(p, "require_payment_action_url"));
        }
        if (json_truthy(json_get(p, "require_payment")) || *au)
            *err = *au ? xasprintf("Muse Code subscription is required: %s", au) : xstrdup("Muse Code subscription is required");
        else *err = xstrdup("Muse Code key response is missing api_key");
        free(au);
    } else {
        char *email = json_get_str(p, "user_email") ? lower_trim(json_get_str(p, "user_email")) : NULL;
        char *uid = trim_dup(json_get_str(p, "user_id"));
        const char *acct = *uid ? uid : email && *email ? email : NULL;
        if (!acct) {
            *err = xstrdup("Muse Code key response is missing a stable account identity");
        } else {
            json_t *bundle = json_obj();
            json_set_str(bundle, "oauthAccessToken", json_get_str(cred, "access"));
            json_set_str(bundle, "apiKey", key);
            char *bs = json_dump(bundle, 0);
            json_free(bundle);
            json_set_str(cred, "access", bs);
            free(bs);
            json_set_str(cred, "accountId", acct);
            if (email) json_set_str(cred, "email", email);
            rc = 0;
        }
        free(email);
        free(uid);
    }
    free(key);
    json_free(p);
    return rc;
}

/* ---------- factory-droid-region (registry/oauth/factory-droid.ts) ---------- */

static const char *factory_base(const char *region) { return str_eq(region, "eu") ? "https://api.eu.factory.ai" : "https://api.factory.ai"; }

static char *factory_workos_org(const actx_t *c, const json_t *cred, const char *region) {
    char *url = xasprintf("%s/api/cli/org", factory_base(region));
    http_headers_t h = {0};
    char *b = xasprintf("Bearer %s", json_get_str(cred, "access"));
    http_headers_set(&h, "Authorization", b);
    http_headers_set(&h, "Accept", "application/json");
    free(b);
    http_resp_t r;
    char *out = NULL;
    if (auth_http("GET", url, &h, NULL, 15000, c, &r) == 0 && r.status >= 200 && r.status < 300) {
        json_t *j = json_parse_str(r.body ? r.body : "");
        const char *o = json_sv(json_at(json_get_arr(j, "workosOrgIds"), 0));
        if (o && *o) out = xstrdup(o);
        json_free(j);
    }
    http_resp_free(&r);
    http_headers_free(&h);
    free(url);
    return out;
}

/* readWhoami: *identity updated in place; returns accepted; *failure set for an unresolved login */
static bool factory_whoami(const actx_t *c, const char *phase, json_t *identity, char **failure, char **fatal) {
    bool login = str_eq(phase, "login");
    char *url = xasprintf("%s/api/cli/whoami", factory_base(json_get_str(identity, "region")));
    http_headers_t h = {0};
    char *b = xasprintf("Bearer %s", json_get_str(identity, "access"));
    http_headers_set(&h, "Authorization", b);
    http_headers_set(&h, "Accept", "application/json");
    if (json_get_str(identity, "orgId")) http_headers_set(&h, "X-Factory-Org-Id", json_get_str(identity, "orgId"));
    free(b);
    http_resp_t r;
    int rc = auth_http("GET", url, &h, NULL, 15000, c, &r);
    http_headers_free(&h);
    free(url);
    bool accepted = false, decided = false;
    if (rc != 0) {
        if (actx_aborted(c)) *fatal = xstrdup("Login cancelled");
        else if (login) *fatal = xasprintf("Factory identity check failed: %s", r.error ? r.error : "request failed");
    } else if ((r.status < 200 || r.status >= 300) && login) {
        *failure = xasprintf("Factory identity check failed (%d): %s", r.status, r.body ? r.body : "");
        decided = true;
    } else if (r.status >= 200 && r.status < 300) {
        json_t *j = json_parse_str(r.body ? r.body : "");
        if (json_is_obj(j)) {
            const char *bo = json_get_str(j, "orgId");
            const char *cur = json_get_str(identity, "orgId");
            char *org = bo && *bo ? xstrdup(bo) : cur ? xstrdup(cur) : NULL;
            bool carried = !org || !cur || str_eq(org, cur);
            const char *br = json_get_str(j, "region");
            const char *region = str_eq(br, "eu") || str_eq(br, "global") ? br : carried ? json_get_str(identity, "region") : NULL;
            char *region_s = region ? xstrdup(region) : NULL;
            const char *bi = json_get_str(j, "inferenceRegion");
            const char *ci = carried ? json_get_str(identity, "inferenceRegion") : NULL;
            const char *inf = str_eq(bi, "global") || str_eq(bi, "eu") || str_eq(bi, "us") ? bi : ci ? ci : str_eq(region_s, "eu") ? "eu" : "global";
            char *inf_s = xstrdup(inf);
            if (org) json_set_str(identity, "orgId", org);
            else json_del(identity, "orgId");
            if (region_s) json_set_str(identity, "region", region_s);
            else json_del(identity, "region");
            json_set_str(identity, "inferenceRegion", inf_s);
            accepted = org != NULL;
            if (!org && login) *failure = xstrdup("Factory login did not resolve an organization");
            decided = true;
            free(org);
            free(region_s);
            free(inf_s);
        }
        json_free(j);
    }
    http_resp_free(&r);
    if (!decided && !*fatal && login && !json_get_str(identity, "orgId")) *failure = xstrdup("Factory login did not resolve an organization");
    return accepted;
}

static int factory_region(const actx_t *c, const char *phase, json_t **credp, const json_t *raw, const json_t *stored, char **err) {
    json_t *cred = *credp;
    if (actx_aborted(c)) {
        *err = xstrdup("Login cancelled");
        return -1;
    }
    if (!json_get_str(raw, "refresh_token") || !*json_get_str(cred, "refresh")) {
        *err = xstrdup("Factory token response missing refresh token");
        return -1;
    }
    const char *sel = json_get_str(raw, "organization_id") ? json_get_str(raw, "organization_id") : json_get_str(stored, "activeOrganizationId");
    const char *corg = json_get_str(cred, "orgId"), *sorg = json_get_str(stored, "orgId");
    bool changed = corg && sorg && strcmp(corg, sorg);
    const char *sact = json_get_str(stored, "activeOrganizationId");
    bool same = !changed && (!sel || !sact || str_eq(sel, sact));
    json_t *id = json_clone(cred);
    if (sel) json_set_str(id, "activeOrganizationId", sel);
    if (!corg && same && sorg) json_set_str(id, "orgId", sorg);
    static const char *const scoped_fields[] = {"region", "inferenceRegion"};
    for (size_t i = 0; i < ARRAY_LEN(scoped_fields); i++) {
        json_del(id, scoped_fields[i]);
        if (same && json_get_str(stored, scoped_fields[i])) json_set_str(id, scoped_fields[i], json_get_str(stored, scoped_fields[i]));
    }
    char *failure = NULL, *fatal = NULL;
    bool accepted = factory_whoami(c, phase, id, &failure, &fatal);
    if (fatal) {
        *err = fatal;
        free(failure);
        json_free(id);
        return -1;
    }
    if (!corg && !accepted && !sel) {
        char *wo = factory_workos_org(c, cred, json_get_str(id, "region"));
        if (wo) {
            json_t *scoped_in = json_clone(id), *scoped = NULL;
            json_set_str(scoped_in, "activeOrganizationId", wo);
            char *rerr = NULL;
            const json_t *d = c->d;
            if (auth_refresh_credential("factory-droid", d, scoped_in, &scoped, &rerr) == 0 && scoped) {
                json_t *merged = json_clone(scoped_in);
                json_merge(merged, scoped);
                json_free(scoped);
                scoped = merged;
            }
            free(rerr);
            json_free(scoped_in);
            free(wo);
            if (actx_aborted(c)) {
                json_free(scoped);
                free(failure);
                json_free(id);
                *err = xstrdup("Login cancelled");
                return -1;
            }
            if (scoped) {
                free(failure);
                json_free(id);
                if (str_eq(phase, "refresh")) {
                    json_free(*credp);
                    *credp = scoped;
                    return 0;
                }
                char *f2 = NULL, *x2 = NULL;
                factory_whoami(c, phase, scoped, &f2, &x2);
                if (x2 || f2) {
                    *err = x2 ? x2 : f2;
                    if (x2) free(f2);
                    json_free(scoped);
                    return -1;
                }
                if (!json_get_str(scoped, "email") && json_get_str(cred, "email")) json_set_str(scoped, "email", json_get_str(cred, "email"));
                if (!json_get_str(scoped, "accountId") && json_get_str(cred, "accountId"))
                    json_set_str(scoped, "accountId", json_get_str(cred, "accountId"));
                json_free(*credp);
                *credp = scoped;
                return 0;
            }
        }
    }
    if (failure) {
        *err = failure;
        json_free(id);
        return -1;
    }
    json_free(*credp);
    *credp = id;
    return 0;
}

int auth_after_exchange(const char *hook, const actx_t *c, const char *phase, json_t **cred, const json_t *raw,
                        const json_t *stored, char **err) {
    if (!hook) return 0;
    if (str_eq(hook, "google-gemini-cli-project") || str_eq(hook, "google-antigravity-project")) {
        if (!str_eq(phase, "login")) return 0; /* refresh keeps the stored projectId (merged by the caller) */
        const char *rt = json_get_str(raw, "refresh_token");
        if (!rt || !*rt) {
            *err = xstrdup("No refresh token received. Please try again.");
            return -1;
        }
        char *project = NULL, *derr = NULL;
        if (cca_discover_project(c->provider, json_get_str(*cred, "access"), NULL, cca_progress, (void *)c, &project, &derr) != 0) {
            char *vu = cca_extract_validation_url(derr);
            if (vu) {
                *err = cca_validation_message(vu, "sign in again", json_get_str(*cred, "email"));
                free(vu);
                free(derr);
            } else {
                *err = derr;
            }
            return -1;
        }
        json_set_str(*cred, "projectId", project);
        free(project);
        return 0;
    }
    if (str_eq(hook, "openai-codex-profile")) return codex_profile(c, phase, *cred, raw, stored, err);
    if (str_eq(hook, "anthropic-identity")) {
        anthropic_identity(c, phase, *cred);
        return 0;
    }
    if (str_eq(hook, "zai-mint-key")) return zai_mint(c, *cred, err);
    if (str_eq(hook, "muse-code-key")) return muse_code_key(c, *cred, err);
    if (str_eq(hook, "factory-droid-region")) return factory_region(c, phase, cred, raw, stored, err);
    if (str_eq(hook, "gitlab-duo-clear-cache")) return 0; /* omc keeps no GitLab Duo direct-access cache */
    *err = xasprintf("auth rules reference unknown after-exchange hook \"%s\"", hook);
    return -1;
}

/* ---------- device-code engine (registry/engine/device-code.ts + oauth/device-code.ts) ---------- */

static int64_t poll_scale_ms(void) {
    const char *s = env_get("OMC_AUTH_POLL_SCALE_MS"); /* test-only: milliseconds per provider "second" */
    long v = s ? strtol(s, NULL, 10) : 0;
    return v > 0 && v <= 1000 ? v : 1000;
}

static char *str_at(const json_t *body, const char *path) {
    json_t *v = path ? json_path(body, path) : NULL;
    return json_is_str(v) && *json_sv(v) ? xstrdup(json_sv(v)) : NULL;
}

static bool num_at(const json_t *body, const char *path, double *out) {
    json_t *v = path ? json_path(body, path) : NULL;
    if (!json_is_num(v)) return false;
    *out = v->u.num.d;
    return true;
}

int auth_device_code_login(const actx_t *c, json_t **cred_out, char **err) {
    const json_t *rule = json_get_obj(c->d, "login");
    const char *provider = c->provider;
    *cred_out = NULL;
    if (actx_aborted(c)) {
        *err = xstrdup("Login cancelled");
        return -1;
    }
    char *client_id = auth_resolve_value(json_get(rule, "clientId"), c, err);
    char *base = json_get(rule, "baseUrl") ? auth_resolve_value(json_get(rule, "baseUrl"), c, err) : NULL;
    sb_t sc = SB_INIT;
    json_t *s;
    const char *sep = json_get_str(rule, "scopeSeparator") ? json_get_str(rule, "scopeSeparator") : " ";
    json_arr_foreach(json_get_arr(rule, "scopes"), i, s) sb_appendf(&sc, "%s%s", i ? sep : "", json_sv(s) ? json_sv(s) : "");
    http_headers_t hh = {0};
    int rc = -1;
    json_t *vars = json_obj(), *std = json_obj(), *dev = NULL, *body = NULL, *poll_vars = NULL, *token_req = NULL;
    char *user_code = NULL, *device_code = NULL, *vuri = NULL, *vuri_c = NULL, *instr = NULL, *token_url = NULL;
    if (*err || (json_get_str(rule, "headersHook") && auth_headers_hook(json_get_str(rule, "headersHook"), &hh, err) != 0)) goto out;
    vset(vars, "client_id", client_id);
    if (sc.len) vset(vars, "scope", sb_cstr(&sc));
    vset(vars, "base", base);
    vset(std, "client_id", client_id);
    if (sc.len) vset(std, "scope", sb_cstr(&sc));
    actx_progress(c, "Requesting device authorization...");
    int status = 0;
    if (auth_post_token(json_get_obj(rule, "device"), std, vars, &hh, provider, "device-auth", c, &status, &dev, err) != 0) goto out;
    if (status < 200 || status >= 300) {
        char *d = json_is_str(dev) ? xstrdup(json_sv(dev)) : json_dump(dev, 0);
        *err = xasprintf("%s device authorization failed: %d %s", provider, status, d);
        free(d);
        goto out;
    }
    const json_t *resp = json_get_obj(rule, "response");
    user_code = str_at(dev, json_get_str(resp, "userCode"));
    device_code = str_at(dev, json_get_str(resp, "deviceCode"));
    vuri = str_at(dev, json_get_str(resp, "verificationUri"));
    vuri_c = str_at(dev, json_get_str(resp, "verificationUriComplete"));
    if (!user_code || !device_code || !vuri) {
        *err = xasprintf("%s device authorization response missing required fields", provider);
        goto out;
    }
    json_t *iv = json_obj();
    json_set_str(iv, "user_code", user_code);
    instr = auth_template(json_get_str(rule, "instructions") ? json_get_str(rule, "instructions") : "", iv);
    json_free(iv);
    actx_auth(c, vuri_c ? vuri_c : vuri, NULL, instr);
    actx_progress(c, "Waiting for device authorization...");
    poll_vars = json_clone(vars);
    json_set_str(poll_vars, "device_code", device_code);
    token_req = json_clone(json_get_obj(rule, "token"));
    token_url = auth_resolve_value(json_get(token_req, "url"), c, err);
    if (!token_url) goto out;
    json_set_str(token_req, "url", token_url);
    double interval_s = 5, expires_s = 0;
    num_at(dev, json_get_str(resp, "interval"), &interval_s);
    bool has_exp = num_at(dev, json_get_str(resp, "expiresIn"), &expires_s);
    int64_t scale = poll_scale_ms();
    int64_t deadline = has_exp ? mono_ms() + (int64_t)(expires_s * scale) : INT64_MAX;
    int64_t interval = MAX(scale, (int64_t)(interval_s * scale));
    int slow = 0;
    json_t *pstd = json_obj();
    json_set_str(pstd, "grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    vset(pstd, "client_id", client_id);
    json_set_str(pstd, "device_code", device_code);
    bool timed_out = true;
    while (mono_ms() < deadline) {
        if (actx_aborted(c)) {
            *err = xstrdup("Login cancelled");
            timed_out = false;
            break;
        }
        json_t *pb = NULL;
        char *perr = NULL;
        int pstatus = 0;
        if (auth_post_token(token_req, pstd, poll_vars, &hh, provider, "device-auth", c, &pstatus, &pb, &perr) != 0) {
            if (actx_aborted(c)) {
                free(perr);
                *err = xstrdup("Login cancelled");
                timed_out = false;
                break;
            }
            if (mono_ms() >= deadline) {
                free(perr);
                break;
            }
            *err = perr;
            timed_out = false;
            break;
        }
        json_t *e = json_path(pb, "error");
        if (pstatus >= 200 && pstatus < 300 && !e) {
            body = pb;
            timed_out = false;
            break;
        }
        if (mono_ms() >= deadline) {
            json_free(pb);
            break;
        }
        const char *es = json_sv(e);
        if (str_eq(es, "authorization_pending")) {
        } else if (str_eq(es, "slow_down")) {
            slow++;
            interval = MAX(scale, interval + 5 * scale);
        } else {
            if (str_eq(es, "expired_token")) *err = xasprintf("%s device code expired; restart the login", provider);
            else if (str_eq(es, "access_denied")) *err = xasprintf("%s device authorization was denied", provider);
            else {
                const char *desc = json_get_str(pb, "error_description");
                char *m = xasprintf("%s device token request failed: %d %s", provider, pstatus, desc ? desc : es ? es : "");
                *err = trim_dup(m);
                free(m);
            }
            json_free(pb);
            timed_out = false;
            break;
        }
        json_free(pb);
        int64_t left = deadline - mono_ms();
        if (left <= 0) break;
        if (!actx_sleep(c, MIN(interval, left))) {
            *err = xstrdup("Login cancelled");
            timed_out = false;
            break;
        }
    }
    json_free(pstd);
    if (!body) {
        if (timed_out)
            *err = xstrdup(slow > 0 ? "Device flow timed out after one or more slow_down responses. This is often caused by clock drift in "
                                      "WSL or VM environments. Please sync or restart the VM clock and try again."
                                    : "Device flow timed out");
        goto out;
    }
    if (actx_aborted(c)) {
        *err = xstrdup("Login cancelled");
        goto out;
    }
    json_t *cred = auth_map_credentials(json_get_obj(rule, "credential"), body, provider, NULL, err);
    if (!cred) goto out;
    auth_apply_userinfo(json_get_obj(rule, "userinfo"), cred, &hh, NULL, c);
    if (actx_aborted(c)) {
        json_free(cred);
        *err = xstrdup("Login cancelled");
        goto out;
    }
    if (auth_after_exchange(json_get_str(rule, "afterExchange"), c, "login", &cred, body, NULL, err) != 0) {
        json_free(cred);
        goto out;
    }
    *cred_out = cred;
    rc = 0;
out:
    http_headers_free(&hh);
    json_free(vars);
    json_free(std);
    json_free(dev);
    json_free(body);
    json_free(poll_vars);
    json_free(token_req);
    free(client_id);
    free(base);
    free(user_code);
    free(device_code);
    free(vuri);
    free(vuri_c);
    free(instr);
    free(token_url);
    sb_free(&sc);
    return rc;
}

/* ---------- API key validation (registry/api-key-validation.ts) ---------- */

static int validation_failed(const char *label, http_resp_t *r, int *status, char **err, const char **code_out, json_t **env_out) {
    char *details = trim_dup(r->body);
    json_t *j = *details ? json_parse_str(details) : NULL;
    if (status) *status = r->status;
    *err = *details ? xasprintf("%s API key validation failed (%d): %s", label, r->status, details)
                    : xasprintf("%s API key validation failed (%d)", label, r->status);
    if (code_out) {
        json_t *e = json_get(j, "error");
        *code_out = json_is_obj(e) ? (json_get_str(e, "code") ? json_get_str(e, "code") : json_get_str(e, "type")) : NULL;
    }
    if (env_out) *env_out = j;
    else json_free(j);
    free(details);
    return -1;
}

static int transport_failed(const char *label, const actx_t *c, http_resp_t *r, int *status, char **err) {
    if (status) *status = 0;
    *err = actx_aborted(c) ? xstrdup("Login cancelled") : xasprintf("%s API key validation failed: %s", label, r->error ? r->error : "request failed");
    return -1;
}

int auth_validate_chat(const char *label, const char *key, const char *base_url, const char *model, const char *max_tokens_field,
                       int max_tokens, bool tolerate_model_denied, const actx_t *c, int *status, char **err) {
    char *url = xasprintf("%s/chat/completions", base_url);
    json_t *b = json_obj();
    json_set_str(b, "model", model);
    json_set(b, "messages", json_parse_str("[{\"role\":\"user\",\"content\":\"ping\"}]"));
    json_set_int(b, max_tokens_field ? max_tokens_field : "max_tokens", max_tokens > 0 ? max_tokens : 1);
    json_set_int(b, "temperature", 0);
    char *body = json_dump(b, 0);
    json_free(b);
    http_headers_t h = {0};
    char *bearer = xasprintf("Bearer %s", key);
    http_headers_set(&h, "Content-Type", "application/json");
    http_headers_set(&h, "Authorization", bearer);
    free(bearer);
    http_resp_t r;
    int rc = auth_http("POST", url, &h, body, 15000, c, &r);
    http_headers_free(&h);
    free(body);
    free(url);
    if (rc != 0) rc = transport_failed(label, c, &r, status, err);
    else if (r.status < 200 || r.status >= 300) {
        const char *code = NULL;
        json_t *env = NULL;
        rc = validation_failed(label, &r, status, err, &code, &env);
        if (tolerate_model_denied && r.status == 401 && str_eq(code, "invalid_model")) {
            free(*err);
            *err = NULL;
            rc = 0;
        }
        json_free(env);
    }
    http_resp_free(&r);
    return rc;
}

int auth_validate_anthropic(const char *label, const char *key, const char *base_url, const char *model, const actx_t *c,
                            int *status, char **err) {
    char *base = rtrim_slashes(trim_dup(base_url));
    if (str_ends(base, "/v1")) base[strlen(base) - 3] = 0;
    char *url = xasprintf("%s/v1/messages", base);
    free(base);
    json_t *b = json_obj();
    json_set_str(b, "model", model);
    json_set(b, "messages", json_parse_str("[{\"role\":\"user\",\"content\":\"ping\"}]"));
    json_set_int(b, "max_tokens", 1);
    char *body = json_dump(b, 0);
    json_free(b);
    http_headers_t h = {0};
    http_headers_set(&h, "Content-Type", "application/json");
    http_headers_set(&h, "anthropic-version", "2023-06-01");
    http_headers_set(&h, "x-api-key", key);
    http_resp_t r;
    int rc = auth_http("POST", url, &h, body, 15000, c, &r);
    http_headers_free(&h);
    free(body);
    free(url);
    if (rc != 0) rc = transport_failed(label, c, &r, status, err);
    else if (r.status < 200 || r.status >= 300) rc = validation_failed(label, &r, status, err, NULL, NULL);
    http_resp_free(&r);
    return rc;
}

int auth_validate_models(const char *label, const char *key, const char *url, const http_headers_t *extra, const actx_t *c,
                         int *status, char **err) {
    http_headers_t h = {0};
    if (extra)
        for (size_t i = 0; i < extra->n; i++) http_headers_set(&h, extra->v[i].name, extra->v[i].value);
    char *bearer = xasprintf("Bearer %s", key);
    http_headers_set(&h, "Authorization", bearer);
    free(bearer);
    http_resp_t r;
    int rc = auth_http("GET", url, &h, NULL, 15000, c, &r);
    http_headers_free(&h);
    if (rc != 0) rc = transport_failed(label, c, &r, status, err);
    else if (r.status < 200 || r.status >= 300) rc = validation_failed(label, &r, status, err, NULL, NULL);
    http_resp_free(&r);
    return rc;
}

/* ---------- api-key engine (registry/engine/api-key.ts) ---------- */

static int run_validation(const json_t *v, const char *label, const char *key, const actx_t *c, int *status, char **err) {
    const char *kind = json_get_str(v, "kind");
    const char *lab = json_get_str(v, "label") ? json_get_str(v, "label") : label;
    if (str_eq(kind, "chat-completions"))
        return auth_validate_chat(lab, key, json_get_str(v, "baseUrl"), json_get_str(v, "model"), json_get_str(v, "maxTokensField"),
                                  (int)json_get_int(v, "maxTokens", 1), json_get_bool(v, "tolerateModelDenied", false), c, status, err);
    if (str_eq(kind, "anthropic-messages"))
        return auth_validate_anthropic(lab, key, json_get_str(v, "baseUrl"), json_get_str(v, "model"), c, status, err);
    if (str_eq(kind, "models-endpoint")) {
        char *env_base = json_get_str(v, "baseUrlEnv") ? trim_dup(env_get(json_get_str(v, "baseUrlEnv"))) : NULL;
        char *url = env_base && *env_base ? xasprintf("%s%s", rtrim_slashes(env_base),
                                                      json_get_str(v, "baseUrlPath") ? json_get_str(v, "baseUrlPath") : "/models")
                                          : xstrdup(json_get_str(v, "url") ? json_get_str(v, "url") : "");
        free(env_base);
        http_headers_t h = {0};
        int rc = 0;
        if (json_get_str(v, "headersHook") && auth_headers_hook(json_get_str(v, "headersHook"), &h, err) != 0) {
            if (status) *status = 0;
            rc = -1;
        }
        if (!rc) rc = auth_validate_models(lab, key, url, &h, c, status, err);
        http_headers_free(&h);
        free(url);
        return rc;
    }
    return 0;
}

static char *strip_bearer(const char *s) {
    if (strncasecmp(s, "bearer", 6) == 0 && !(isalnum((unsigned char)s[6]) || s[6] == '_')) {
        s += 6;
        while (isspace((unsigned char)*s)) s++;
    }
    return xstrdup(s);
}

int auth_api_key_login(const actx_t *c, char **key_out, char **err) {
    const json_t *rule = json_get_obj(c->d, "login");
    *key_out = NULL;
    const char *url = json_get_str(rule, "authUrl"), *instr = json_get_str(rule, "instructions");
    if (!c->h || !c->h->prompt) {
        *err = xasprintf("%s login requires onPrompt callback", c->label);
        return -1;
    }
    if (url && instr) actx_auth(c, url, NULL, instr);
    bool has_fallback = json_get(rule, "emptyFallback") != NULL;
    char *dflt = json_get_str(rule, "prompt") ? NULL : xasprintf("Paste your %s API key", c->label);
    char *answer = actx_prompt(c, dflt ? dflt : json_get_str(rule, "prompt"), json_get_str(rule, "placeholder"), has_fallback, err);
    free(dflt);
    if (!answer) return -1;
    char *t = str_trim(answer);
    free(answer);
    if (str_eq(json_get_str(rule, "normalize"), "strip-bearer") && *t) {
        char *s = strip_bearer(t);
        free(t);
        t = s;
        if (!*t) {
            *err = xasprintf("%s API key is empty after stripping Bearer prefix", c->label);
            free(t);
            return -1;
        }
    }
    if (!*t) {
        free(t);
        if (has_fallback) {
            *key_out = xstrdup(json_get_str(rule, "emptyFallback") ? json_get_str(rule, "emptyFallback") : "");
            return 0;
        }
        *err = xstrdup("API key is required");
        return -1;
    }
    const json_t *v = json_get_obj(rule, "validate");
    if (v) {
        bool optional = json_get_bool(v, "optional", false);
        actx_progress(c, optional ? "Validating API key (optional)..." : "Validating API key...");
        int status = 0;
        char *verr = NULL;
        if (run_validation(v, c->label, t, c, &status, &verr) != 0) {
            bool trusts_forbidden = json_get_bool(v, "trustForbidden", false) && status == 403;
            bool auth_failed = status == 401 || status == 403;
            if (!optional || (auth_failed && !trusts_forbidden) || actx_aborted(c)) {
                *err = verr;
                free(t);
                return -1;
            }
            free(verr);
            char *m = xasprintf("Skipping %s validation endpoint; continuing with provided API key.", c->label);
            actx_progress(c, m);
            free(m);
        }
    }
    *key_out = t;
    return 0;
}

/* ---------- refresh (registry/engine/refresh.ts) ---------- */

int auth_refresh_credential(const char *provider, const json_t *d, const json_t *stored, json_t **out, char **err) {
    *out = NULL;
    const json_t *rf = json_get_obj(d, "refresh");
    const char *kind = json_get_str(rf, "kind");
    if (str_eq(kind, "hook")) return auth_custom_hook_refresh(json_get_str(rf, "hook"), provider, stored, out, err);
    if (!rf || !str_eq(kind, "request")) {
        *err = xasprintf("no refresh flow for %s", provider);
        return -1;
    }
    json_t *req;
    json_arr_foreach(json_get_arr(rf, "require"), i, req) {
        const char *field = json_sv(req);
        if (field && !json_truthy(json_get(stored, field))) {
            *err = xasprintf("%s credentials are missing %s; sign in again", provider, field);
            return -1;
        }
    }
    actx_t c = {provider, json_get_str(d, "name") ? json_get_str(d, "name") : provider, d, NULL};
    const json_t *login = json_get_obj(d, "login");
    const char *lk = json_get_str(login, "kind");
    char *client_id = NULL, *client_secret = NULL, *redirect = NULL, *base = NULL, *authb = NULL;
    if (str_eq(lk, "oauth-code") || str_eq(lk, "device-code")) {
        client_id = json_get(login, "clientId") ? auth_resolve_value(json_get(login, "clientId"), &c, err) : NULL;
        base = json_get(login, "baseUrl") ? auth_resolve_value(json_get(login, "baseUrl"), &c, err) : NULL;
        if (str_eq(lk, "oauth-code")) {
            client_secret = json_get(login, "clientSecret") ? auth_resolve_value(json_get(login, "clientSecret"), &c, err) : NULL;
            const json_t *cb = json_get_obj(login, "callback");
            redirect = json_get(cb, "redirectUri") ? auth_resolve_value(json_get(cb, "redirectUri"), &c, err) : NULL;
            if (!redirect || !*redirect) {
                free(redirect);
                redirect = xasprintf("http://%s:%lld%s", json_get_str(cb, "hostname") ? json_get_str(cb, "hostname") : "localhost",
                                     (long long)json_get_int(cb, "port", 0), json_get_str(cb, "path") ? json_get_str(cb, "path") : "/callback");
            }
            if (base) rtrim_slashes(base);
            authb = json_get(login, "authUrl") ? auth_resolve_value(json_get(login, "authUrl"), &c, err) : NULL;
            if (authb) rtrim_slashes(authb);
        }
    }
    http_headers_t hh = {0};
    int rc = -1;
    json_t *vars = json_obj(), *std = json_obj(), *body = NULL;
    const char *rt = json_get_str(stored, "refresh");
    if (*err || (json_get_str(rf, "headersHook") && auth_headers_hook(json_get_str(rf, "headersHook"), &hh, err) != 0)) goto out;
    vset(vars, "refresh_token", rt);
    vset(vars, "client_id", client_id);
    vset(vars, "client_secret", client_secret);
    vset(vars, "redirect_uri", redirect);
    vset(vars, "base", base);
    vset(vars, "auth", authb);
    vset(vars, "claude_code_sdk_version", "0.60.0");
    vset(std, "grant_type", "refresh_token");
    vset(std, "client_id", client_id);
    vset(std, "client_secret", client_secret);
    vset(std, "refresh_token", rt);
    vset(std, "organization_id", json_get_str(stored, "activeOrganizationId"));
    if (auth_post_token(json_get_obj(rf, "token"), std, vars, &hh, provider, "token-refresh", &c, NULL, &body, err) != 0) goto out;
    const json_t *cmap = json_get_obj(rf, "credential") ? json_get_obj(rf, "credential") : json_get_obj(login, "credential");
    json_t *cred = auth_map_credentials(cmap, body, provider, stored, err);
    if (!cred) goto out;
    auth_apply_userinfo(json_get_obj(rf, "userinfo"), cred, &hh, NULL, &c);
    if (auth_after_exchange(json_get_str(rf, "afterRefresh"), &c, "refresh", &cred, body, stored, err) != 0) {
        json_free(cred);
        goto out;
    }
    *out = cred;
    rc = 0;
out:
    http_headers_free(&hh);
    json_free(vars);
    json_free(std);
    json_free(body);
    free(client_id);
    free(client_secret);
    free(redirect);
    free(base);
    free(authb);
    return rc;
}
