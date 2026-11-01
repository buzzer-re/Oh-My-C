/* auth_custom.c - whole-flow login / refresh hooks (`login "custom" hook=...`), ported from omp v18.14.2
 * packages/ai/src/registry/oauth/{github-copilot,cursor,openai-codex,alibaba-coding-plan,alibaba-token-plan,
 * xiaomi,perplexity,cloudflare-ai-gateway,cloudflare-workers-ai,snowflake,kilo}.ts and registry/snowflake.ts. */
#define _GNU_SOURCE
#include "config/auth_engine.h"

#include "ai/ai.h"
#include "core/proc.h"

#include <ctype.h>
#include <openssl/sha.h>
#include <pthread.h>

#define MAX_SAFE_INTEGER 9007199254740991LL

static int64_t scale_ms(void) {
    const char *s = env_get("OMC_AUTH_POLL_SCALE_MS"); /* test-only: milliseconds per provider "second" */
    long v = s ? strtol(s, NULL, 10) : 0;
    return v > 0 && v <= 1000 ? v : 1000;
}

static char *trimd(const char *s) { return str_trim(s ? s : ""); }

static void set_bearer(http_headers_t *h, const char *scheme, const char *tok) {
    char *b = xasprintf("%s %s", scheme, tok ? tok : "");
    http_headers_set(h, "Authorization", b);
    free(b);
}

static json_t *body_json(const http_resp_t *r) { return json_parse_str(r->body ? r->body : ""); }

static json_t *new_cred(const char *access, const char *refresh, int64_t expires) {
    json_t *c = json_obj();
    json_set_str(c, "type", "oauth");
    json_set_str(c, "access", access);
    json_set_str(c, "refresh", refresh ? refresh : "");
    json_set_int(c, "expires", expires);
    return c;
}

static int cancelled(const actx_t *c, char **err) {
    if (!actx_aborted(c)) return 0;
    *err = xstrdup("Login cancelled");
    return -1;
}

static int64_t jwt_expiry(const char *tok, int64_t skew, int64_t fallback) {
    json_t *p = auth_jwt_payload(tok);
    json_t *e = json_get(p, "exp");
    int64_t r = json_is_num(e) ? (int64_t)(e->u.num.d * 1000) - skew : fallback;
    json_free(p);
    return r;
}

/* ======================================================================================= */
/* GitHub Copilot (registry/oauth/github-copilot.ts + catalog/wire/github-copilot.ts)       */
/* ======================================================================================= */

#define COPILOT_CLI_UA "copilot/1.0.82"

static bool public_github_host(const char *h) {
    char *l = str_lower(h ? h : "");
    char *t = str_trim(l);
    bool r = str_eq(t, "api.github.com") || str_eq(t, "github.com") || str_eq(t, "www.github.com");
    free(l);
    free(t);
    return r;
}

/* normalizeDomain: the hostname of input (https:// assumed), NULL when blank or unparsable */
static char *normalize_domain(const char *input) {
    char *t = trimd(input);
    if (!*t) {
        free(t);
        return NULL;
    }
    char *u = strstr(t, "://") ? xstrdup(t) : xasprintf("https://%s", t);
    url_t p;
    char *host = NULL;
    if (url_parse(u, &p) == 0) {
        if (p.host && *p.host && !strpbrk(p.host, " \t")) host = str_lower(p.host);
        url_free(&p);
    }
    free(u);
    free(t);
    return host;
}

static char *copilot_endpoint_normalize(const char *in) {
    char *t = trimd(in);
    url_t p;
    char *r = NULL;
    if (str_starts(t, "https://") && url_parse(t, &p) == 0) {
        if (p.host && *p.host) {
            size_t n = strlen(t);
            while (n > 0 && t[n - 1] == '/') t[--n] = 0;
            r = xstrdup(t);
        }
        url_free(&p);
    }
    free(t);
    return r;
}

static char *copilot_discover_endpoint(const actx_t *c, const char *token) {
    http_headers_t h = {0};
    http_headers_set(&h, "Accept", "application/json");
    set_bearer(&h, "token", token);
    http_headers_set(&h, "User-Agent", COPILOT_CLI_UA);
    http_resp_t r;
    char *out = NULL;
    if (auth_http("GET", "https://api.github.com/copilot_internal/user", &h, NULL, 30000, c, &r) == 0 && r.status >= 200 &&
        r.status < 300) {
        json_t *j = body_json(&r);
        const char *api = json_get_str(json_get_obj(j, "endpoints"), "api");
        if (api) out = copilot_endpoint_normalize(api);
        json_free(j);
    }
    http_resp_free(&r);
    http_headers_free(&h);
    return out;
}

static int github_post(const actx_t *c, const char *url, const char *body, json_t **out, char **err) {
    http_headers_t h = {0};
    http_headers_set(&h, "Accept", "application/json");
    http_headers_set(&h, "Content-Type", "application/x-www-form-urlencoded");
    http_headers_set(&h, "User-Agent", "copilot-developer-action/0.0.1");
    http_resp_t r;
    int rc = auth_http("POST", url, &h, body, 30000, c, &r);
    http_headers_free(&h);
    *out = NULL;
    if (rc != 0) {
        *err = actx_aborted(c) ? xstrdup("Login cancelled") : xasprintf("request to %s failed: %s", url, r.error ? r.error : "?");
    } else if (r.status < 200 || r.status >= 300) {
        *err = xasprintf("%d: %s", r.status, r.body ? r.body : "");
        rc = -1;
    } else if (!(*out = body_json(&r))) {
        *err = xstrdup("Invalid device code response");
        rc = -1;
    }
    http_resp_free(&r);
    return rc;
}

typedef struct {
    const actx_t *c;
    const char *token, *base, *integration;
    const char *ids[5];
    int n;
} policy_batch_t;

static void *policy_worker(void *ud) {
    policy_batch_t *b = ud;
    for (int i = 0; i < b->n; i++) {
        char *url = xasprintf("%s/models/%s/policy", b->base, b->ids[i]);
        http_headers_t h = {0};
        http_headers_set(&h, "Content-Type", "application/json");
        set_bearer(&h, "Bearer", b->token);
        http_headers_set(&h, "User-Agent", COPILOT_CLI_UA);
        http_headers_set(&h, "Editor-Version", COPILOT_CLI_UA);
        http_headers_set(&h, "Copilot-Integration-Id", b->integration);
        http_headers_set(&h, "Copilot-Harness-Id", "copilot-sdk");
        http_headers_set(&h, "Openai-Intent", "chat-policy");
        http_headers_set(&h, "X-GitHub-Api-Version", "2026-08-01");
        http_headers_set(&h, "X-Initiator", "user");
        http_headers_set(&h, "X-Interaction-Type", "chat-policy");
        http_resp_t r;
        auth_http("POST", url, &h, "{\"state\":\"enabled\"}", 30000, b->c, &r);
        http_resp_free(&r);
        http_headers_free(&h);
        free(url);
    }
    return NULL;
}

/* enableAllGitHubCopilotModels: best-effort policy acceptance, 5 requests in flight */
static void copilot_enable_models(const actx_t *c, const char *token, const char *enterprise, const char *api_endpoint) {
    char *base = api_endpoint ? xstrdup(api_endpoint)
                 : enterprise  ? (str_starts(enterprise, "copilot-api.") ? xasprintf("https://%s", enterprise)
                                                                         : xasprintf("https://copilot-api.%s", enterprise))
                               : xstrdup("https://api.githubcopilot.com");
    char *integ = trimd(env_get("COPILOT_INTEGRATION_ID"));
    if (!*integ || strpbrk(integ, "\r\n")) {
        free(integ);
        integ = xstrdup("copilot-chat");
    }
    strvec_t ids = {0};
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (!str_eq(m->provider, "github-copilot")) continue;
        const char *w = json_get_str(m->raw, "requestModelId") ? json_get_str(m->raw, "requestModelId") : m->id;
        if (!strvec_contains(&ids, w)) strvec_push(&ids, w);
    }
    for (size_t i = 0; i < ids.n && !actx_aborted(c); i += 5) {
        policy_batch_t b[5];
        pthread_t th[5];
        int nt = 0;
        for (size_t j = i; j < ids.n && j < i + 5; j++, nt++) {
            b[nt] = (policy_batch_t){c, token, base, integ, {ids.v[j]}, 1};
            if (pthread_create(&th[nt], NULL, policy_worker, &b[nt]) != 0) {
                policy_worker(&b[nt]);
                th[nt] = 0;
            }
        }
        for (int j = 0; j < nt; j++)
            if (th[j]) pthread_join(th[j], NULL);
    }
    strvec_free(&ids);
    free(integ);
    free(base);
}

static int copilot_login(const actx_t *c, alogin_t *out, char **err) {
    char *input = actx_prompt(c, "GitHub Enterprise URL/domain (blank for github.com)", "company.ghe.com", true, err);
    if (!input) return -1;
    char *trimmed = trimd(input);
    char *nd = normalize_domain(input);
    free(input);
    if (*trimmed && !nd) {
        free(trimmed);
        *err = xstrdup("Invalid GitHub Enterprise URL/domain");
        return -1;
    }
    free(trimmed);
    bool pub = !nd || public_github_host(nd);
    char *enterprise = pub ? NULL : xstrdup(nd);
    const char *domain = pub ? "github.com" : nd;
    const char *client_id = pub ? "Ov23li8tweQw6odWQebz" : "Ov23ctDVkRmgkPke0Mmm";
    char *dev_url = xasprintf("https://%s/login/device/code", domain), *tok_url = xasprintf("https://%s/login/oauth/access_token", domain);
    char *body = xasprintf("client_id=%s&scope=read%%3Auser", client_id);
    json_t *dev = NULL;
    char *gh = NULL, *api = NULL;
    int rc = -1;
    if (github_post(c, dev_url, body, &dev, err) != 0) goto out;
    const char *device_code = json_get_str(dev, "device_code"), *user_code = json_get_str(dev, "user_code"),
               *vuri = json_get_str(dev, "verification_uri");
    json_t *iv = json_get(dev, "interval"), *ex = json_get(dev, "expires_in");
    if (!json_is_obj(dev)) {
        *err = xstrdup("Invalid device code response");
        goto out;
    }
    if (!device_code || !user_code || !vuri || !json_is_num(iv) || !json_is_num(ex)) {
        *err = xstrdup("Invalid device code response fields");
        goto out;
    }
    char *instr = xasprintf("Enter code: %s", user_code);
    actx_auth(c, vuri, NULL, instr);
    free(instr);
    int64_t scale = scale_ms();
    int64_t deadline = mono_ms() + (int64_t)(ex->u.num.d * 1000);
    int64_t interval = MAX(scale, (int64_t)(iv->u.num.d * scale));
    double mult = 1.2;
    int slow = 0;
    char *poll_body = xasprintf("client_id=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
                                client_id, device_code);
    bool done = false;
    while (mono_ms() < deadline) {
        if (cancelled(c, err)) break;
        int64_t wait = MIN((int64_t)((double)interval * mult + 0.999), deadline - mono_ms());
        if (!actx_sleep(c, MAX(0, wait))) {
            cancelled(c, err);
            break;
        }
        json_t *p = NULL;
        if (github_post(c, tok_url, poll_body, &p, err) != 0) break;
        if (json_get_str(p, "access_token")) {
            gh = xstrdup(json_get_str(p, "access_token"));
            json_free(p);
            done = true;
            break;
        }
        const char *e = json_get_str(p, "error");
        if (e) {
            if (str_eq(e, "authorization_pending")) {
            } else if (str_eq(e, "slow_down")) {
                slow++;
                json_t *ni = json_get(p, "interval");
                interval = json_is_num(ni) && ni->u.num.d > 0 ? MAX(scale, (int64_t)(ni->u.num.d * scale)) : MAX(scale, interval + 5 * scale);
                mult = 1.4;
            } else {
                const char *d = json_get_str(p, "error_description");
                *err = d ? xasprintf("Device flow failed: %s: %s", e, d) : xasprintf("Device flow failed: %s", e);
                json_free(p);
                break;
            }
        }
        json_free(p);
    }
    free(poll_body);
    if (!done) {
        if (!*err)
            *err = xstrdup(slow > 0 ? "Device flow timed out after one or more slow_down responses. This is often caused by clock drift in "
                                      "WSL or VM environments. Please sync or restart the VM clock and try again."
                                    : "Device flow timed out");
        goto out;
    }
    api = copilot_discover_endpoint(c, gh);
    json_t *cred = new_cred(gh, gh, now_ms() + (int64_t)(10 * 365.25 * 24 * 3600 * 1000.0));
    if (enterprise) json_set_str(cred, "enterpriseUrl", enterprise);
    if (api) json_set_str(cred, "apiEndpoint", api);
    actx_progress(c, "Enabling models...");
    copilot_enable_models(c, gh, enterprise, api);
    out->cred = cred;
    rc = 0;
out:
    json_free(dev);
    free(gh);
    free(api);
    free(dev_url);
    free(tok_url);
    free(body);
    free(enterprise);
    free(nd);
    return rc;
}

/* ======================================================================================= */
/* Cursor (registry/oauth/cursor.ts)                                                        */
/* ======================================================================================= */

static char *cursor_sub(const char *tok) {
    json_t *p = auth_jwt_payload(tok);
    char *s = json_get_str(p, "sub") ? trimd(json_get_str(p, "sub")) : NULL;
    json_free(p);
    if (s && !*s) {
        free(s);
        s = NULL;
    }
    return s;
}

/* withCursorAccountEmail: GetMe (Connect RPC); a failed lookup leaves the credential unchanged */
static void cursor_email(const actx_t *c, json_t *cred) {
    char *sub = cursor_sub(json_get_str(cred, "access"));
    if (!sub) return;
    http_headers_t h = {0};
    http_headers_set(&h, "Accept", "application/json");
    set_bearer(&h, "Bearer", json_get_str(cred, "access"));
    http_headers_set(&h, "Connect-Protocol-Version", "1");
    http_headers_set(&h, "Content-Type", "application/json");
    http_resp_t r;
    if (auth_http("POST", "https://api2.cursor.sh/aiserver.v1.DashboardService/GetMe", &h, "{}", 3000, c, &r) == 0 && r.status >= 200 &&
        r.status < 300) {
        json_t *j = body_json(&r);
        if (str_eq(json_get_str(j, "authId"), sub) && json_get_str(j, "email")) {
            char *e = trimd(json_get_str(j, "email"));
            if (*e) json_set_str(cred, "email", e);
            free(e);
        }
        json_free(j);
    }
    http_resp_free(&r);
    http_headers_free(&h);
    free(sub);
}

static int cursor_login(const actx_t *c, alogin_t *out, char **err) {
    unsigned char vb[32], dig[SHA256_DIGEST_LENGTH];
    rand_bytes(vb, sizeof vb);
    char *verifier = base64url_encode(vb, sizeof vb);
    SHA256((const unsigned char *)verifier, strlen(verifier), dig);
    char *challenge = base64url_encode(dig, sizeof dig);
    char *uuid = uuid_v4();
    char *url = xasprintf("https://cursor.com/loginDeepControl?challenge=%s&uuid=%s&mode=login&redirectTarget=cli", challenge, uuid);
    actx_auth(c, url, NULL, NULL);
    actx_progress(c, "Waiting for browser authentication...");
    int64_t scale = scale_ms();
    double delay = (double)scale;
    int errors = 0, rc = -1;
    char *poll_url = xasprintf("https://api2.cursor.sh/auth/poll?uuid=%s&verifier=%s", uuid, verifier);
    for (int attempt = 0; attempt < 150 && rc != 0 && !*err; attempt++) {
        if (!actx_sleep(c, (int64_t)delay)) {
            cancelled(c, err);
            break;
        }
        http_resp_t r;
        int hr = auth_http("GET", poll_url, NULL, NULL, 30000, c, &r);
        if (hr == 0 && r.status == 404) {
            errors = 0;
            delay = MIN(delay * 1.2, (double)(10 * scale));
        } else if (hr == 0 && r.status >= 200 && r.status < 300) {
            json_t *j = body_json(&r);
            const char *a = json_get_str(j, "accessToken");
            if (a) {
                json_t *cred = new_cred(a, json_get_str(j, "refreshToken"), jwt_expiry(a, 300000, now_ms() + 3600000));
                cursor_email(c, cred);
                out->cred = cred;
                rc = 0;
            } else if (++errors >= 3) {
                *err = xstrdup("Too many consecutive errors during Cursor auth polling");
            }
            json_free(j);
        } else if (cancelled(c, err) == 0 && ++errors >= 3) {
            *err = xstrdup("Too many consecutive errors during Cursor auth polling");
        }
        http_resp_free(&r);
    }
    if (rc != 0 && !*err) *err = xstrdup("Cursor authentication polling timeout");
    free(poll_url);
    free(url);
    free(uuid);
    free(verifier);
    free(challenge);
    return rc;
}

static int cursor_refresh(const json_t *stored, json_t **out, char **err) {
    json_t *b = json_obj();
    json_set_str(b, "grant_type", "refresh_token");
    json_set_str(b, "client_id", "KbZUR41cY7W6zRSdpSUJ7I7mLYBKOCmB");
    json_set_str(b, "refresh_token", json_get_str(stored, "refresh") ? json_get_str(stored, "refresh") : "");
    char *body = json_dump(b, 0);
    json_free(b);
    http_headers_t h = {0};
    http_headers_set(&h, "Content-Type", "application/json");
    http_resp_t r;
    int rc = auth_http("POST", "https://api2.cursor.sh/oauth/token", &h, body, 30000, NULL, &r);
    http_headers_free(&h);
    free(body);
    if (rc != 0 || r.status < 200 || r.status >= 300) {
        *err = xasprintf("Cursor token refresh failed: %s", rc != 0 ? (r.error ? r.error : "request failed") : r.body ? r.body : "");
        http_resp_free(&r);
        return -1;
    }
    json_t *j = body_json(&r);
    http_resp_free(&r);
    rc = -1;
    const char *a = json_get_str(j, "access_token");
    if (json_get(j, "shouldLogout") && json_truthy(json_get(j, "shouldLogout")) && json_is_bool(json_get(j, "shouldLogout")))
        *err = xstrdup("invalid_grant: Cursor ended this session; run /login cursor again");
    else if (!a || !*a)
        *err = xstrdup("Cursor token refresh returned no access token");
    else {
        const char *nr = json_get_str(j, "refresh_token");
        json_t *cred = new_cred(a, nr && *nr ? nr : json_get_str(stored, "refresh"), jwt_expiry(a, 300000, now_ms() + 3600000));
        if (!json_get_str(stored, "email")) cursor_email(NULL, cred);
        *out = cred;
        rc = 0;
    }
    json_free(j);
    return rc;
}

/* ======================================================================================= */
/* OpenAI Codex device flow (registry/oauth/openai-codex.ts loginOpenAICodexDevice)         */
/* ======================================================================================= */

static char *codex_token_error(int status, const char *text) {
    char *t = trimd(text);
    if (!*t) {
        free(t);
        return xasprintf("%d", status);
    }
    json_t *j = json_parse_str(t);
    char *r;
    if (!json_is_obj(j)) {
        r = xasprintf("%d %s", status, t);
    } else {
        json_t *e = json_get(j, "error"), *d = json_get(j, "error_description");
        const char *es = json_is_str(e) ? json_sv(e) : json_is_obj(e) ? (json_get_str(e, "code") ? json_get_str(e, "code") : json_get_str(e, "message")) : NULL;
        const char *ds = json_sv(d);
        if (es && ds && strcmp(es, ds)) r = xasprintf("%d %s: %s", status, es, ds);
        else r = xasprintf("%d %s", status, es ? es : ds ? ds : json_get_str(j, "message") ? json_get_str(j, "message") : t);
    }
    json_free(j);
    free(t);
    return r;
}

static int codex_device_login(const actx_t *c, alogin_t *out, char **err) {
    actx_progress(c, "Initiating device authorization…");
    http_headers_t h = {0};
    http_headers_set(&h, "Content-Type", "application/json");
    http_resp_t r;
    int rc = auth_http("POST", "https://auth.openai.com/api/accounts/deviceauth/usercode", &h, "{\"client_id\":\"app_EMoamEEZ73f0CkXaXp7hrann\"}",
                       15000, c, &r);
    if (rc != 0 || r.status < 200 || r.status >= 300) {
        *err = rc != 0 ? xasprintf("Device authorization initiation failed: %s", r.error ? r.error : "request failed")
                       : xasprintf("Device authorization initiation failed: %d", r.status);
        http_resp_free(&r);
        http_headers_free(&h);
        return -1;
    }
    json_t *init = body_json(&r);
    http_resp_free(&r);
    const char *dev_id = json_get_str(init, "device_auth_id"), *uc = json_get_str(init, "user_code");
    if (!dev_id || !*dev_id || !uc || !*uc) {
        json_free(init);
        http_headers_free(&h);
        *err = xstrdup("Device authorization response missing required fields");
        return -1;
    }
    int64_t scale = scale_ms();
    json_t *iv = json_get(init, "interval");
    double secs = json_is_num(iv) ? iv->u.num.d : json_is_str(iv) ? strtol(json_sv(iv), NULL, 10) : 5;
    if (!json_is_num(iv) && secs <= 0) secs = 5;
    int64_t poll_ms = (int64_t)(secs * scale) + 3 * scale;
    char *instr = xasprintf("Enter code: %s", uc);
    actx_auth(c, "https://auth.openai.com/codex/device", NULL, instr);
    free(instr);
    char *wait = xasprintf("Waiting for browser authorization (code: %s)…", uc);
    actx_progress(c, wait);
    free(wait);
    json_t *pb = json_obj();
    json_set_str(pb, "device_auth_id", dev_id);
    json_set_str(pb, "user_code", uc);
    char *poll_body = json_dump(pb, 0);
    json_free(pb);
    rc = -1;
    char *code = NULL, *verifier = NULL;
    for (int poll = 0; poll < 120 && !code && !*err; poll++) {
        actx_sleep(c, poll == 0 ? MIN(poll_ms, 5 * scale) : poll_ms);
        if (actx_aborted(c)) {
            *err = xstrdup("Device authorization cancelled");
            break;
        }
        if (auth_http("POST", "https://auth.openai.com/api/accounts/deviceauth/token", &h, poll_body, 15000, c, &r) != 0) {
            *err = actx_aborted(c) ? xstrdup("Device authorization cancelled")
                                   : xasprintf("Device token polling failed: %s", r.error ? r.error : "request failed");
        } else if (r.status == 403 || r.status == 404) {
            /* authorization pending */
        } else if (r.status < 200 || r.status >= 300) {
            *err = xasprintf("Device token polling failed: %d", r.status);
        } else {
            json_t *j = body_json(&r);
            const char *ac = json_get_str(j, "authorization_code"), *cv = json_get_str(j, "code_verifier");
            if (!ac || !*ac || !cv || !*cv) *err = xstrdup("Device token response missing authorization_code or code_verifier");
            else {
                code = xstrdup(ac);
                verifier = xstrdup(cv);
            }
            json_free(j);
        }
        http_resp_free(&r);
    }
    free(poll_body);
    json_free(init);
    if (!code) {
        if (!*err) *err = xstrdup("Device authorization timed out — user did not complete login in time");
        http_headers_free(&h);
        return -1;
    }
    actx_progress(c, "Exchanging authorization code for tokens…");
    sb_t b = SB_INIT;
    char *ec = url_encode(code), *ev = url_encode(verifier);
    sb_appendf(&b, "grant_type=authorization_code&client_id=app_EMoamEEZ73f0CkXaXp7hrann&code=%s&code_verifier=%s&redirect_uri=%s", ec, ev,
               "https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback");
    free(ec);
    free(ev);
    http_headers_set(&h, "Content-Type", "application/x-www-form-urlencoded");
    int hr = auth_http("POST", "https://auth.openai.com/oauth/token", &h, sb_cstr(&b), 15000, c, &r);
    sb_free(&b);
    http_headers_free(&h);
    free(code);
    free(verifier);
    if (hr != 0 || r.status < 200 || r.status >= 300) {
        char *f = hr != 0 ? xstrdup(r.error ? r.error : "request failed") : codex_token_error(r.status, r.body);
        *err = xasprintf("Token exchange failed: %s", f);
        free(f);
        http_resp_free(&r);
        return -1;
    }
    json_t *tok = body_json(&r);
    http_resp_free(&r);
    const char *at = json_get_str(tok, "access_token"), *rt = json_get_str(tok, "refresh_token");
    json_t *ein = json_get(tok, "expires_in");
    if (!at || !*at || !rt || !*rt || !json_is_num(ein)) {
        *err = xstrdup("Token response missing required fields");
        json_free(tok);
        return -1;
    }
    json_t *cred = new_cred(at, rt, now_ms() + (int64_t)(ein->u.num.d * 1000));
    rc = auth_after_exchange("openai-codex-profile", c, "login", &cred, tok, NULL, err);
    json_free(tok);
    if (rc != 0) {
        json_free(cred);
        return -1;
    }
    out->cred = cred;
    return 0;
}

/* ======================================================================================= */
/* API-key style custom logins                                                              */
/* ======================================================================================= */

static char *ask_key(const actx_t *c, const char *msg, const char *ph, char **err) {
    char *a = actx_prompt(c, msg, ph, false, err);
    if (!a) return NULL;
    char *t = str_trim(a);
    free(a);
    if (!*t) {
        free(t);
        *err = xstrdup("API key is required");
        return NULL;
    }
    return t;
}

static char *strip_trailing(char *s) {
    for (size_t n = strlen(s); n > 0 && s[n - 1] == '/';) s[--n] = 0;
    return s;
}

static int alibaba_coding_plan(const actx_t *c, alogin_t *out, char **err) {
    char *ch = actx_prompt(c, "Select Alibaba Coding Plan endpoint: 1=International (default), 2=China, 3=Custom — enter 1, 2, or 3", "1",
                           false, err);
    if (!ch) return -1;
    char *choice = str_trim(ch);
    free(ch);
    char *base;
    const char *auth_url, *instr;
    if (str_eq(choice, "2")) {
        base = xstrdup("https://coding.dashscope.aliyuncs.com/v1");
        auth_url = "https://bailian.console.aliyun.com/?tab=model#/api-key";
        instr = "Copy your API key from the Alibaba Cloud Bailian console (China mainland)";
    } else if (str_eq(choice, "3")) {
        char *cu = actx_prompt(c, "Enter custom base URL", "https://your-proxy.com/v1", false, err);
        if (!cu) {
            free(choice);
            return -1;
        }
        base = strip_trailing(str_trim(cu));
        free(cu);
        if (!*base) {
            *err = xstrdup("Custom URL is required for option 3");
            free(base);
            free(choice);
            return -1;
        }
        auth_url = "https://modelstudio.console.alibabacloud.com/";
        instr = "Copy your API key from the Alibaba Cloud DashScope console";
    } else {
        base = xstrdup("https://coding-intl.dashscope.aliyuncs.com/v1");
        auth_url = "https://modelstudio.console.alibabacloud.com/";
        instr = "Copy your API key from the Alibaba Cloud DashScope console (International)";
    }
    actx_auth(c, auth_url, NULL, instr);
    char *key = ask_key(c, "Paste your Alibaba Coding Plan API key", "sk-...", err);
    int rc = -1;
    if (key) {
        actx_progress(c, "Validating API key...");
        int st = 0;
        if (str_eq(choice, "3")) {
            char *mu = xasprintf("%s/models", base);
            rc = auth_validate_models("Alibaba Coding Plan", key, mu, NULL, c, &st, err);
            free(mu);
        } else {
            rc = auth_validate_chat("Alibaba Coding Plan", key, base, "qwen3.5-plus", NULL, 1, false, c, &st, err);
        }
        if (rc == 0) {
            json_t *cred = new_cred(key, key, MAX_SAFE_INTEGER);
            json_set_str(cred, "enterpriseUrl", base);
            out->cred = cred;
        }
    }
    free(key);
    free(base);
    free(choice);
    return rc;
}

#define TOKEN_PLAN_INTL "https://token-plan.ap-southeast-1.maas.aliyuncs.com/compatible-mode/v1"
#define TOKEN_PLAN_CN "https://token-plan.cn-beijing.maas.aliyuncs.com/compatible-mode/v1"

static int alibaba_token_plan(const actx_t *c, alogin_t *out, char **err) {
    char *ch = actx_prompt(c, "Select QwenCloud Token Plan region: 1=International (default), 2=China (Beijing), 3=Custom — enter 1, 2, or 3",
                           "1", false, err);
    if (!ch) return -1;
    char *choice = str_trim(ch);
    free(ch);
    char *base;
    const char *auth_url, *instr;
    if (str_eq(choice, "2")) {
        base = xstrdup(TOKEN_PLAN_CN);
        auth_url = "https://www.aliyun.com/benefit/scene/tokenplan";
        instr = "Subscribe to the China (Beijing) 百炼 Token Plan and copy its dedicated API key";
    } else if (str_eq(choice, "3")) {
        char *cu = actx_prompt(c, "Enter custom Token Plan base URL", "https://token-plan.<region>.maas.aliyuncs.com/compatible-mode/v1", false, err);
        if (!cu) {
            free(choice);
            return -1;
        }
        base = strip_trailing(str_trim(cu));
        free(cu);
        if (!*base) {
            *err = xstrdup("Custom URL is required for option 3");
            free(base);
            free(choice);
            return -1;
        }
        auth_url = "https://home.qwencloud.com/billing/subscription/token-plan-individual";
        instr = "Copy your Token Plan API key for the custom endpoint";
    } else {
        base = xstrdup(TOKEN_PLAN_INTL);
        auth_url = "https://home.qwencloud.com/billing/subscription/token-plan-individual";
        instr = "Subscribe to Token Plan Individual and copy its dedicated API key. Keep this page open; the next prompt explains how to "
                "enable optional quota reporting.";
    }
    free(choice);
    actx_auth(c, auth_url, NULL, instr);
    char *key = ask_key(c, "Paste your QwenCloud Token Plan API key", "sk-sp-...", err);
    int rc = -1;
    char *cookie = NULL;
    if (!key) goto out;
    actx_progress(c, "Validating API key...");
    char *mu = xasprintf("%s/models", base);
    int st = 0;
    rc = auth_validate_models("QwenCloud Token Plan", key, mu, NULL, c, &st, err);
    free(mu);
    if (rc != 0) goto out;
    rc = -1;
    bool cn = str_eq(base, TOKEN_PLAN_CN);
    const char *host = cn ? "bailian-cs.console.aliyun.com" : "cs-data.qwencloud.com";
    char *msg = xasprintf("Optional quota reporting: open browser DevTools → Network, reload the Token Plan page, filter for api.json, and "
                          "select the %s/data/api.json request whose api query ends in /tokenplan/personal/api/v2/usage. Copy Request "
                          "Headers → Cookie, then paste the complete name=value; ... value here, or press Enter to skip.",
                          host);
    char *raw = actx_prompt(c, msg, "name=value; name=value; ...", true, err);
    free(msg);
    if (!raw) goto out;
    char *t = str_trim(raw);
    free(raw);
    if (strncasecmp(t, "cookie:", 7) == 0) {
        char *t2 = str_trim(t + 7);
        free(t);
        t = t2;
    }
    cookie = t;
    if (*cookie) {
        bool valid = false;
        strvec_t segs = str_split(cookie, ";");
        for (size_t i = 0; i < segs.n && !valid; i++) {
            char *eq = strchr(segs.v[i], '=');
            if (!eq || eq == segs.v[i]) continue;
            char *n = xstrndup(segs.v[i], (size_t)(eq - segs.v[i])), *nt = str_trim(n), *vt = str_trim(eq + 1);
            valid = *nt && *vt;
            free(n);
            free(nt);
            free(vt);
        }
        strvec_free(&segs);
        if (!valid) {
            *err = xasprintf("Invalid QwenCloud Cookie header. Copy the complete Cookie request header from the %s usage request, not a "
                             "single cookie value.",
                             host);
            goto out;
        }
    }
    /* serializeAlibabaTokenPlanCredential: bare token unless a cookie or non-default region is stored */
    bool region = !str_eq(base, TOKEN_PLAN_INTL);
    if (!*cookie && !region) {
        out->api_key = xstrdup(key);
    } else {
        json_t *p = json_obj();
        json_set_str(p, "token", key);
        if (*cookie) json_set_str(p, "cookie", cookie);
        if (region) json_set_str(p, "baseUrl", base);
        out->api_key = json_dump(p, 0);
        json_free(p);
    }
    rc = 0;
out:
    free(cookie);
    free(key);
    free(base);
    return rc;
}

static const json_t *auth_descriptor(const char *id) {
    json_t *d;
    json_arr_foreach(catalog_auth_providers(), i, d) if (str_eq(json_get_str(d, "id"), id)) return d;
    return NULL;
}

/* registry/oauth/xiaomi.ts validateXiaomiApiKey: Token Plan keys probe SGP -> AMS -> CN, 401 falls through */
static int xiaomi_validate(const actx_t *c, const char *key, char **err) {
    bool tp = str_starts(key, "tp-") || str_starts(key, "ttp-");
    const char *regions[] = {"xiaomi-token-plan-sgp", "xiaomi-token-plan-ams", "xiaomi-token-plan-cn"};
    const char *bases[3], *models[3];
    size_t n = 0;
    if (tp) {
        for (size_t i = 0; i < 3; i++) {
            const json_t *v = json_get_obj(json_get_obj(auth_descriptor(regions[i]), "login"), "validate");
            if (!str_eq(json_get_str(v, "kind"), "chat-completions")) {
                *err = xasprintf("Missing chat-completions validation policy for %s", regions[i]);
                return -1;
            }
            bases[n] = json_get_str(v, "baseUrl");
            models[n++] = json_get_str(v, "model");
        }
    } else {
        bases[n] = "https://api.xiaomimimo.com/v1";
        models[n++] = catalog_provider_default_model("xiaomi") ? catalog_provider_default_model("xiaomi") : "mimo-v2-flash";
    }
    char *last = NULL;
    for (size_t i = 0; i < n; i++) {
        char *url = xasprintf("%s/chat/completions", bases[i]);
        json_t *b = json_obj();
        json_set_str(b, "model", models[i]);
        json_set_int(b, "max_tokens", 1);
        json_set(b, "messages", json_parse_str("[{\"role\":\"user\",\"content\":\"ping\"}]"));
        char *body = json_dump(b, 0);
        json_free(b);
        http_headers_t h = {0};
        http_headers_set(&h, "Content-Type", "application/json");
        set_bearer(&h, "Bearer", key);
        http_resp_t r;
        int rc = auth_http("POST", url, &h, body, 15000, c, &r);
        http_headers_free(&h);
        free(body);
        free(url);
        if (rc != 0) {
            if (actx_aborted(c)) {
                http_resp_free(&r);
                free(last);
                *err = xstrdup("Login cancelled");
                return -1;
            }
            free(last);
            last = xstrdup(r.error ? r.error : "request failed");
            http_resp_free(&r);
            continue;
        }
        if (r.status >= 200 && r.status < 300) {
            http_resp_free(&r);
            free(last);
            return 0;
        }
        char *d = trimd(r.body);
        char *e = *d ? xasprintf("Xiaomi MiMo API key validation failed (%d): %s", r.status, d)
                     : xasprintf("Xiaomi MiMo API key validation failed (%d)", r.status);
        free(d);
        int st = r.status;
        http_resp_free(&r);
        free(last);
        last = e;
        if (st != 401) break;
    }
    *err = last ? last : xstrdup("Xiaomi MiMo API key validation failed");
    return -1;
}

static int xiaomi_login(const actx_t *c, alogin_t *out, char **err) {
    actx_auth(c, "https://platform.xiaomimimo.com/#/console/api-keys", NULL, "Copy your API key from the Xiaomi MiMo console");
    char *key = ask_key(c, "Paste your Xiaomi API key (sk-... or Token Plan tp-... / ttp-...)", "sk-..., tp-..., or ttp-...", err);
    if (!key) return -1;
    actx_progress(c, "Validating xiaomi API key...");
    if (xiaomi_validate(c, key, err) != 0) {
        free(key);
        return -1;
    }
    out->api_key = key;
    return 0;
}

static int cloudflare_login(const actx_t *c, bool gateway, alogin_t *out, char **err) {
    if (gateway)
        actx_auth(c, "https://developers.cloudflare.com/ai-gateway/configuration/authentication/", NULL,
                  "Create an AI Gateway token with Run permission, then copy it here.");
    else
        actx_auth(c, "https://dash.cloudflare.com/profile/api-tokens", NULL,
                  "Create an API token with Workers AI Read and Edit permissions, then copy it here.");
    char *tok = ask_key(c, gateway ? "Paste your Cloudflare AI Gateway token/API key" : "Paste your Cloudflare API token",
                        gateway ? "cfut_..." : "Workers AI API token", err);
    if (!tok) return -1;
    char *acct = actx_prompt(c, "Enter your Cloudflare account ID", "32-character account ID", false, err);
    char *at = acct ? str_trim(acct) : NULL, *gw = NULL, *gt = NULL;
    free(acct);
    int rc = -1;
    if (!at) goto out;
    if (!*at) {
        *err = xstrdup("Cloudflare account ID is required");
        goto out;
    }
    if (gateway) {
        gw = actx_prompt(c, "Enter your Cloudflare AI Gateway ID", "default", false, err);
        if (!gw) goto out;
        gt = str_trim(gw);
        if (!*gt) {
            *err = xstrdup("Cloudflare AI Gateway ID is required");
            goto out;
        }
    }
    json_t *p = json_obj();
    json_set_str(p, "token", tok);
    json_set_str(p, "accountId", at);
    if (gt) json_set_str(p, "gatewayId", gt);
    out->api_key = json_dump(p, 0);
    json_free(p);
    rc = 0;
out:
    free(tok);
    free(at);
    free(gw);
    free(gt);
    return rc;
}

/* ======================================================================================= */
/* Perplexity (registry/oauth/perplexity.ts): desktop-app borrow, then HTTP email OTP          */
/* ======================================================================================= */

#define PPLX_UA "Perplexity/641 CFNetwork/1568 Darwin/25.2.0"

typedef struct {
    strvec_t names, values;
} cookies_t;

static void cookies_remember(cookies_t *ck, const http_resp_t *r) {
    for (size_t i = 0; i < r->headers.n; i++) {
        if (strcasecmp(r->headers.v[i].name, "set-cookie")) continue;
        const char *sc = r->headers.v[i].value;
        size_t nv = strcspn(sc, ";");
        char *pair = xstrndup(sc, nv);
        char *eq = strchr(pair, '=');
        if (!eq) {
            free(pair);
            continue;
        }
        *eq = 0;
        char *name = str_trim(pair), *value = str_trim(eq + 1);
        bool expired = false;
        char *attrs = str_lower(sc + nv);
        const char *ma = strstr(attrs, "max-age=");
        if (ma && strtol(ma + 8, NULL, 10) <= 0) expired = true;
        free(attrs);
        size_t k = 0;
        while (k < ck->names.n && strcmp(ck->names.v[k], name)) k++;
        if (k < ck->names.n) {
            free(ck->names.v[k]);
            free(ck->values.v[k]);
            ck->names.v[k] = ck->names.v[ck->names.n - 1];
            ck->values.v[k] = ck->values.v[ck->values.n - 1];
            ck->names.n--;
            ck->values.n--;
        }
        if (!expired) {
            strvec_push_own(&ck->names, name);
            strvec_push_own(&ck->values, value);
        } else {
            free(name);
            free(value);
        }
        free(pair);
    }
}

static const char *cookies_get(const cookies_t *ck, const char *name) {
    for (size_t i = 0; i < ck->names.n; i++)
        if (!strcmp(ck->names.v[i], name)) return ck->values.v[i];
    return NULL;
}

static int pplx_request(const actx_t *c, cookies_t *ck, const char *method, const char *url, const json_t *body, http_resp_t *r) {
    http_headers_t h = {0};
    if (body) http_headers_set(&h, "Content-Type", "application/json");
    http_headers_set(&h, "User-Agent", PPLX_UA);
    http_headers_set(&h, "X-App-ApiVersion", "2.18");
    if (ck->names.n) {
        sb_t b = SB_INIT;
        for (size_t i = 0; i < ck->names.n; i++) sb_appendf(&b, "%s%s=%s", i ? "; " : "", ck->names.v[i], ck->values.v[i]);
        http_headers_set(&h, "Cookie", sb_cstr(&b));
        sb_free(&b);
    }
    char *bs = body ? json_dump(body, 0) : NULL;
    int rc = auth_http(method, url, &h, bs, 30000, c, r);
    free(bs);
    http_headers_free(&h);
    if (rc == 0) cookies_remember(ck, r);
    return rc;
}

static json_t *pplx_cred(const char *jwt, const char *email) {
    json_t *c = new_cred(jwt, jwt, jwt_expiry(jwt, 300000, AUTH_NEVER_EXPIRES));
    if (email) json_set_str(c, "email", email);
    return c;
}

static const char *pplx_reason(const json_t *v, const char *dflt) {
    if (json_get_str(v, "text")) return json_get_str(v, "text");
    if (json_get_str(v, "error_code")) return json_get_str(v, "error_code");
    if (json_get_str(v, "status")) return json_get_str(v, "status");
    return dflt;
}

static int perplexity_email(const actx_t *c, alogin_t *out, char **err) {
    char *em = actx_prompt(c, "Enter your Perplexity email address", "user@example.com", false, err);
    if (!em) return -1;
    char *email = str_trim(em);
    free(em);
    cookies_t ck = {0};
    json_t *b = NULL, *verify = NULL;
    char *csrf = NULL, *otp = NULL;
    int rc = -1;
    http_resp_t r = {0};
    if (!*email) {
        *err = xstrdup("Email is required for Perplexity login");
        goto out;
    }
    if (cancelled(c, err)) goto out;
    actx_progress(c, "Fetching Perplexity CSRF token...");
    if (pplx_request(c, &ck, "GET", "https://www.perplexity.ai/api/auth/csrf", NULL, &r) != 0 || r.status < 200 || r.status >= 300) {
        *err = r.status ? xasprintf("Perplexity CSRF request failed: %d", r.status)
                        : xasprintf("Perplexity CSRF request failed: %s", r.error ? r.error : "request failed");
        goto out;
    }
    json_t *cj = body_json(&r);
    csrf = json_get_str(cj, "csrfToken") ? xstrdup(json_get_str(cj, "csrfToken")) : NULL;
    json_free(cj);
    http_resp_free(&r);
    memset(&r, 0, sizeof r);
    if (!csrf) {
        *err = xstrdup("Perplexity CSRF response missing csrfToken");
        goto out;
    }
    actx_progress(c, "Sending login code to your email...");
    b = json_obj();
    json_set_str(b, "email", email);
    json_set_str(b, "csrfToken", csrf);
    if (pplx_request(c, &ck, "POST", "https://www.perplexity.ai/api/auth/signin-email", b, &r) != 0 || r.status < 200 || r.status >= 300) {
        *err = xasprintf("Perplexity send login code failed (%d): %s", r.status, r.body ? r.body : r.error ? r.error : "");
        goto out;
    }
    http_resp_free(&r);
    memset(&r, 0, sizeof r);
    json_free(b);
    b = NULL;
    char *o = actx_prompt(c, "Enter the code sent to your email", "123456", false, err);
    if (!o) goto out;
    otp = str_trim(o);
    free(o);
    if (!*otp) {
        *err = xstrdup("OTP code is required");
        goto out;
    }
    if (cancelled(c, err)) goto out;
    actx_progress(c, "Verifying login code...");
    b = json_obj();
    json_set_str(b, "email", email);
    json_set_str(b, "otp", otp);
    json_set_str(b, "csrfToken", csrf);
    if (pplx_request(c, &ck, "POST", "https://www.perplexity.ai/api/auth/signin-otp", b, &r) != 0) {
        *err = xasprintf("Perplexity OTP verification failed: %s", r.error ? r.error : "request failed");
        goto out;
    }
    verify = body_json(&r);
    if (r.status < 200 || r.status >= 300) {
        *err = xasprintf("Perplexity OTP verification failed: %s", pplx_reason(verify, "OTP verification failed"));
        goto out;
    }
    http_resp_free(&r);
    memset(&r, 0, sizeof r);
    if (str_eq(json_get_str(verify, "status"), "totp_challenge_required") && json_get_str(verify, "challenge_token")) {
        char *tt = actx_prompt(c, "Enter the code from your authenticator app", "123456", false, err);
        if (!tt) goto out;
        char *totp = str_trim(tt);
        free(tt);
        if (!*totp) {
            free(totp);
            *err = xstrdup("Authenticator code is required");
            goto out;
        }
        actx_progress(c, "Verifying authenticator code...");
        json_free(b);
        b = json_obj();
        json_set_str(b, "token", json_get_str(verify, "challenge_token"));
        json_set_str(b, "code", totp);
        free(totp);
        if (pplx_request(c, &ck, "POST", "https://www.perplexity.ai/api/auth/totp/challenge-verify", b, &r) != 0) {
            *err = xasprintf("Perplexity authenticator verification failed: %s", r.error ? r.error : "request failed");
            goto out;
        }
        json_free(verify);
        verify = body_json(&r);
        if (r.status < 200 || r.status >= 300) {
            *err = xasprintf("Perplexity authenticator verification failed: %s", pplx_reason(verify, "TOTP verification failed"));
            goto out;
        }
        if (!json_get_str(verify, "token")) {
            const char *st = cookies_get(&ck, "__Secure-next-auth.session-token");
            if (!st) st = cookies_get(&ck, "next-auth.session-token");
            json_free(verify);
            verify = json_obj();
            if (st) json_set_str(verify, "token", st);
            json_set_str(verify, "status", "success");
        }
    }
    const char *token = json_get_str(verify, "token") ? json_get_str(verify, "token") : json_get_str(verify, "challenge_token");
    const char *vst = json_get_str(verify, "status");
    if (!token || !*token || json_get_str(verify, "error_code") || (vst && !str_eq(vst, "success"))) {
        *err = xasprintf("Perplexity OTP verification response rejected: %s", pplx_reason(verify, "missing token"));
        goto out;
    }
    out->cred = pplx_cred(token, email);
    rc = 0;
out:
    http_resp_free(&r);
    json_free(b);
    json_free(verify);
    strvec_free(&ck.names);
    strvec_free(&ck.values);
    free(email);
    free(csrf);
    free(otp);
    return rc;
}

static int perplexity_login(const actx_t *c, alogin_t *out, char **err) {
    if (!c->h || !c->h->prompt) {
        *err = xstrdup("Perplexity login requires onPrompt callback");
        return -1;
    }
    if (cancelled(c, err)) return -1;
    if (!env_get("PI_AUTH_NO_BORROW")) {
        actx_progress(c, "Checking for Perplexity desktop app...");
#ifdef __APPLE__
        char *tok = proc_shell_capture("defaults read ai.perplexity.mac authToken 2>/dev/null", NULL, 10000);
        char *t = tok ? str_trim(tok) : NULL;
        free(tok);
        if (cancelled(c, err)) {
            free(t);
            return -1;
        }
        if (t && *t && !str_eq(t, "(null)")) {
            actx_progress(c, "Found Perplexity JWT from native app");
            out->cred = pplx_cred(t, NULL);
            free(t);
            return 0;
        }
        free(t);
#endif
    }
    /* browser SSO needs a host-managed browser session (onBrowserSession), which the terminal has not */
    return perplexity_email(c, out, err);
}

/* ======================================================================================= */
/* Snowflake (registry/oauth/snowflake.ts + registry/snowflake.ts)                          */
/* ======================================================================================= */

static bool dns_label(const char *s, size_t n) {
    if (n == 0 || n > 63) return false;
    if (!isalnum((unsigned char)s[0]) || !isalnum((unsigned char)s[n - 1])) return false;
    for (size_t i = 0; i < n; i++)
        if (!(islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '-')) return false;
    return true;
}

static bool legacy_region(const char *s) {
    /* ^[a-z]{2}(?:-gov)?-[a-z]+-\d+$ */
    if (!(islower((unsigned char)s[0]) && islower((unsigned char)s[1]) && s[2] == '-')) return false;
    const char *p = s + 3;
    if (!strncmp(p, "gov-", 4)) p += 4;
    const char *q = p;
    while (islower((unsigned char)*q)) q++;
    if (q == p || *q != '-') return false;
    q++;
    const char *d = q;
    while (isdigit((unsigned char)*q)) q++;
    return q > d && !*q;
}

static char *lower_dash(const char *s, size_t n) {
    char *r = xstrndup(s, n);
    for (char *p = r; *p; p++) *p = *p == '_' ? '-' : (char)tolower((unsigned char)*p);
    return r;
}

#define SNOWFLAKE_INVALID "Paste your Snowflake account identifier (orgname-accountname) or account URL"

static char *snowflake_account_url(const char *input, char **err) {
    char *v = trimd(input);
    char *host = NULL, *out = NULL;
    if (!*v) {
        *err = xstrdup("Snowflake account is required: run /login snowflake or set SNOWFLAKE_ACCOUNT");
        goto done;
    }
    const char *sch = strstr(v, "://");
    bool scheme = false;
    if (sch && isalpha((unsigned char)v[0])) {
        scheme = true;
        for (const char *p = v; p < sch; p++)
            if (!(isalnum((unsigned char)*p) || *p == '+' || *p == '.' || *p == '-')) scheme = false;
    }
    if (scheme && strncasecmp(v, "https://", 8)) {
        *err = xstrdup("Snowflake account URL must use https");
        goto done;
    }
    if (strchr(v, '\\')) {
        *err = xstrdup(SNOWFLAKE_INVALID);
        goto done;
    }
    if (scheme) {
        const char *a = v + 8;
        size_t al = strcspn(a, "/?#");
        if (!al) {
            *err = xstrdup(SNOWFLAKE_INVALID);
            goto done;
        }
        host = lower_dash(a, al);
        if (str_ends(host, ":443")) host[strlen(host) - 4] = 0;
        if (str_eq(host, "app.snowflake.com")) {
            const char *rest = a + al;
            size_t rl = strcspn(rest, "?#");
            char *path = xstrndup(rest, rl);
            strvec_t segs = str_split(path, "/");
            free(path);
            char *org = segs.n > 1 ? lower_dash(segs.v[1], strlen(segs.v[1])) : xstrdup("");
            char *acc = segs.n > 2 ? lower_dash(segs.v[2], strlen(segs.v[2])) : xstrdup("");
            strvec_free(&segs);
            if (!dns_label(org, strlen(org)) || !dns_label(acc, strlen(acc))) *err = xstrdup(SNOWFLAKE_INVALID);
            else if (legacy_region(org))
                *err = xstrdup("Legacy Snowsight links don't identify the account host; paste your account identifier (orgname-accountname) or "
                               "account URL");
            else {
                free(host);
                host = xasprintf("%s-%s.snowflakecomputing.com", org, acc);
            }
            free(org);
            free(acc);
            if (*err) goto done;
        }
    } else {
        if (strpbrk(v, "/?#")) {
            *err = xstrdup(SNOWFLAKE_INVALID);
            goto done;
        }
        host = lower_dash(v, strlen(v));
        if (str_ends(host, ":443")) host[strlen(host) - 4] = 0;
        if (!str_ends(host, ".snowflakecomputing.com") && !str_ends(host, ".snowflakecomputing.cn")) {
            char *h2 = xasprintf("%s.snowflakecomputing.com", host);
            free(host);
            host = h2;
        }
    }
    if (str_ends(host, ".snowflakecomputing.cn")) {
        *err = xstrdup("Snowflake Cortex REST API is not available in China-region accounts (.snowflakecomputing.cn)");
        goto done;
    }
    const char *suffix = ".snowflakecomputing.com";
    bool ok = str_ends(host, suffix) && strlen(host) > strlen(suffix);
    for (const char *p = host; ok && *p;) {
        size_t n = strcspn(p, ".");
        ok = dns_label(p, n);
        p += n;
        if (*p) p++;
    }
    if (!ok) *err = xstrdup(SNOWFLAKE_INVALID);
    else out = xasprintf("https://%s", host);
done:
    free(v);
    free(host);
    return out;
}

static int snowflake_login(const actx_t *c, alogin_t *out, char **err) {
    if (cancelled(c, err)) return -1;
    char *acct = actx_prompt(c, "Enter your Snowflake account identifier or URL", "myorg-myaccount", false, err);
    if (!acct) return -1;
    char *url = snowflake_account_url(acct, err);
    free(acct);
    if (!url) return -1;
    json_t *login = json_obj();
    json_set_str(login, "kind", "oauth-code");
    json_set_str(login, "clientId", "LOCAL_APPLICATION");
    json_set_str(login, "authorizeUrl", "{auth}/oauth/authorize");
    json_set_str(login, "authUrl", url);
    json_set_bool(login, "pkce", true);
    json_set_bool(login, "standardAuthorizeParams", false);
    json_set(login, "authorizeParams",
             json_parse_str("{\"client_id\":\"{client_id}\",\"response_type\":\"code\",\"redirect_uri\":\"{redirect_uri}\","
                            "\"scope\":\"refresh_token\",\"state\":\"{state}\",\"code_challenge\":\"{code_challenge}\","
                            "\"code_challenge_method\":\"S256\"}"));
    json_t *cb = json_obj();
    json_set_int(cb, "port", json_get_int(c->d, "callbackPort", 54551));
    json_set_str(cb, "path", "/");
    json_set_str(cb, "hostname", "127.0.0.1");
    json_set_bool(cb, "portFallback", true);
    json_set(login, "callback", cb);
    json_set_str(login, "instructions", "Sign in to Snowflake in your browser and approve access for omc.");
    json_set(login, "token", json_parse_str("{\"url\":\"{auth}/oauth/token-request\",\"body\":\"form\",\"standard\":true}"));
    json_set(login, "credential", json_parse_str("{\"access\":{\"path\":\"access_token\"},\"refresh\":{\"path\":\"refresh_token\"},"
                                                 "\"expires\":{\"mode\":\"seconds\",\"path\":\"expires_in\",\"skewMs\":0}}"));
    json_t *cred = NULL;
    int rc = auth_oauth_code_flow(c, login, &cred, err);
    json_free(login);
    if (rc == 0) {
        json_set_str(cred, "enterpriseUrl", url);
        out->cred = cred;
    }
    free(url);
    return rc;
}

static int snowflake_refresh(const json_t *stored, json_t **out, char **err) {
    char *rt = trimd(json_get_str(stored, "refresh"));
    if (!*rt) {
        free(rt);
        if (now_ms() < json_get_int(stored, "expires", 0)) {
            *out = json_clone(stored);
            return 0;
        }
        *err = xstrdup("Snowflake did not issue a refresh token; run /login snowflake again or use SNOWFLAKE_ACCOUNT and SNOWFLAKE_PAT");
        return -1;
    }
    char *aerr = NULL;
    char *acct = snowflake_account_url(json_get_str(stored, "enterpriseUrl"), &aerr);
    if (!acct) {
        free(aerr);
        free(rt);
        *err = xstrdup("Invalid Snowflake account; run /login snowflake again");
        return -1;
    }
    char *url = xasprintf("%s/oauth/token-request", acct);
    char *er = url_encode(json_get_str(stored, "refresh"));
    char *body = xasprintf("grant_type=refresh_token&refresh_token=%s&client_id=LOCAL_APPLICATION", er);
    free(er);
    http_headers_t h = {0};
    http_headers_set(&h, "Content-Type", "application/x-www-form-urlencoded");
    http_resp_t r;
    int rc = auth_http("POST", url, &h, body, 30000, NULL, &r);
    http_headers_free(&h);
    free(body);
    free(url);
    free(acct);
    free(rt);
    if (rc != 0 || r.status < 200 || r.status >= 300) {
        *err = rc != 0 ? xasprintf("Snowflake token refresh failed (%s)", r.error ? r.error : "request failed")
                       : xasprintf("Snowflake token refresh failed (%d)", r.status);
        http_resp_free(&r);
        return -1;
    }
    json_t *j = body_json(&r);
    http_resp_free(&r);
    json_t *ein = json_get(j, "expires_in");
    const char *at = json_get_str(j, "access_token");
    json_t *nrt = json_get(j, "refresh_token");
    char *att = trimd(at);
    if (!at || !*att || !json_is_num(ein) || ein->u.num.d <= 0 || (nrt && !json_is_null(nrt) && !json_sv(nrt))) {
        free(att);
        json_free(j);
        *err = xstrdup("Snowflake returned an invalid token response");
        return -1;
    }
    free(att);
    json_t *c = json_clone(stored);
    json_set_str(c, "access", at);
    if (json_sv(nrt) && *json_sv(nrt)) json_set_str(c, "refresh", json_sv(nrt));
    json_set_int(c, "expires", now_ms() + (int64_t)(ein->u.num.d * 1000));
    json_free(j);
    *out = c;
    return 0;
}

/* ======================================================================================= */
/* Kilo Gateway (registry/oauth/kilo.ts)                                                     */
/* ======================================================================================= */

static int kilo_login(const actx_t *c, alogin_t *out, char **err) {
    http_headers_t h = {0};
    http_headers_set(&h, "Content-Type", "application/json");
    http_resp_t r;
    int rc = auth_http("POST", "https://api.kilo.ai/api/device-auth/codes", &h, NULL, 30000, c, &r);
    http_headers_free(&h);
    if (rc != 0 || r.status < 200 || r.status >= 300) {
        *err = rc != 0 ? xasprintf("Failed to initiate device authorization: %s", r.error ? r.error : "request failed")
               : r.status == 429 ? xstrdup("Too many pending authorization requests. Please try again later.")
                                 : xasprintf("Failed to initiate device authorization: %d", r.status);
        http_resp_free(&r);
        return -1;
    }
    json_t *j = body_json(&r);
    http_resp_free(&r);
    const char *code = json_get_str(j, "code"), *vurl = json_get_str(j, "verificationUrl");
    json_t *ein = json_get(j, "expiresIn");
    if (!code || !*code || !vurl || !*vurl || !json_is_num(ein) || ein->u.num.d <= 0) {
        json_free(j);
        *err = xstrdup("Kilo device authorization response missing required fields");
        return -1;
    }
    char *instr = xasprintf("Enter code: %s", code);
    actx_auth(c, vurl, NULL, instr);
    free(instr);
    char *ec = url_encode(code), *purl = xasprintf("https://api.kilo.ai/api/device-auth/codes/%s", ec);
    free(ec);
    int64_t scale = scale_ms();
    int64_t deadline = mono_ms() + (int64_t)(ein->u.num.d * 1000);
    rc = -1;
    while (mono_ms() < deadline && rc != 0 && !*err) {
        if (cancelled(c, err)) break;
        if (auth_http("GET", purl, NULL, NULL, 30000, c, &r) != 0) {
            *err = actx_aborted(c) ? xstrdup("Login cancelled") : xasprintf("Failed to poll device authorization: %s", r.error ? r.error : "?");
        } else if (r.status == 202) {
            /* pending */
        } else if (r.status == 403) {
            *err = xstrdup("Authorization was denied");
        } else if (r.status == 410) {
            *err = xstrdup("Authorization code expired. Please try again.");
        } else if (r.status < 200 || r.status >= 300) {
            *err = xasprintf("Failed to poll device authorization: %d", r.status);
        } else {
            json_t *p = body_json(&r);
            const char *st = json_get_str(p, "status"), *tok = json_get_str(p, "token");
            if (str_eq(st, "approved") && tok && *tok) {
                out->cred = new_cred(tok, "", now_ms() + 365LL * 24 * 3600 * 1000);
                rc = 0;
            } else if (str_eq(st, "denied")) *err = xstrdup("Authorization was denied");
            else if (str_eq(st, "expired")) *err = xstrdup("Authorization code expired. Please try again.");
            json_free(p);
        }
        http_resp_free(&r);
        if (rc != 0 && !*err && !actx_sleep(c, 5 * scale)) cancelled(c, err);
    }
    if (rc != 0 && !*err) *err = xstrdup("Authentication timed out. Please try again.");
    free(purl);
    json_free(j);
    return rc;
}

/* ======================================================================================= */
/* dispatch                                                                                  */
/* ======================================================================================= */

int auth_custom_hook_login(const actx_t *c, const char *hook, alogin_t *out, char **err) {
    if (str_eq(hook, "github-copilot")) return copilot_login(c, out, err);
    if (str_eq(hook, "cursor")) return cursor_login(c, out, err);
    if (str_eq(hook, "openai-codex-device")) return codex_device_login(c, out, err);
    if (str_eq(hook, "alibaba-coding-plan")) return alibaba_coding_plan(c, out, err);
    if (str_eq(hook, "alibaba-token-plan")) return alibaba_token_plan(c, out, err);
    if (str_eq(hook, "xiaomi")) return xiaomi_login(c, out, err);
    if (str_eq(hook, "perplexity")) return perplexity_login(c, out, err);
    if (str_eq(hook, "cloudflare-ai-gateway")) return cloudflare_login(c, true, out, err);
    if (str_eq(hook, "cloudflare-workers-ai")) return cloudflare_login(c, false, out, err);
    if (str_eq(hook, "snowflake")) return snowflake_login(c, out, err);
    if (str_eq(hook, "kilo")) return kilo_login(c, out, err);
    *err = xasprintf("auth rules reference unknown login hook \"%s\"", hook ? hook : "");
    return -1;
}

int auth_custom_hook_refresh(const char *hook, const char *provider, const json_t *stored, json_t **out, char **err) {
    if (str_eq(hook, "github-copilot")) {
        /* GitHub OAuth tokens stay directly usable: same token, far-future expiry */
        const char *rt = json_get_str(stored, "refresh") ? json_get_str(stored, "refresh") : "";
        json_t *c = new_cred(rt, rt, now_ms() + (int64_t)(10 * 365.25 * 24 * 3600 * 1000.0));
        if (json_get_str(stored, "enterpriseUrl")) json_set_str(c, "enterpriseUrl", json_get_str(stored, "enterpriseUrl"));
        if (json_get_str(stored, "apiEndpoint")) json_set_str(c, "apiEndpoint", json_get_str(stored, "apiEndpoint"));
        *out = c;
        return 0;
    }
    if (str_eq(hook, "cursor")) return cursor_refresh(stored, out, err);
    if (str_eq(hook, "snowflake")) return snowflake_refresh(stored, out, err);
    *err = xasprintf("auth rules reference unknown refresh hook \"%s\" (%s)", hook ? hook : "", provider);
    return -1;
}
