/* auth_custom.c - whole-flow login / refresh hooks (`login "custom" hook=...`), ported from omp v18.14.2
 * packages/ai/src/registry/oauth/{github-copilot,cursor,openai-codex,alibaba-coding-plan,alibaba-token-plan,
 * xiaomi,perplexity,cloudflare-ai-gateway,cloudflare-workers-ai,snowflake,kilo,mistral-vibe}.ts and registry/snowflake.ts. */
#define _GNU_SOURCE
#include "config/auth_engine.h"

#include "ai/ai.h"
#include "ai/pb.h"
#include "core/fs.h"
#include "core/proc.h"
#include "session/trace.h"

#include <ctype.h>
#include <errno.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <zlib.h>

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
    bool explicit_id;
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
        int rc = auth_http("POST", url, &h, "{\"state\":\"enabled\"}", 30000, b->c, &r);
        /* wrapFetchForCopilotFallback: a denied default chat identity retries once as the Copilot CLI */
        if (rc == 0 && !b->explicit_id && str_eq(b->integration, "copilot-chat") &&
            (r.status == 403 || (r.status == 400 && r.body && strstr(r.body, "\"model_not_supported\"")))) {
            http_resp_free(&r);
            http_headers_set(&h, "Copilot-Integration-Id", "copilot-developer-cli");
            auth_http("POST", url, &h, "{\"state\":\"enabled\"}", 30000, b->c, &r);
        }
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
    bool explicit_id = true;
    if (!*integ || strpbrk(integ, "\r\n")) {
        free(integ);
        integ = xstrdup("copilot-chat");
        explicit_id = false;
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
            b[nt] = (policy_batch_t){c, token, base, integ, {ids.v[j]}, 1, explicit_id};
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

/* api2.cursor.sh URL for path; OMC_TEST_CURSOR_API (test hook shared with the Cursor provider) replaces the origin */
static char *cursor_api(const char *path) {
    const char *b = env_get("OMC_TEST_CURSOR_API");
    return xasprintf("%s%s", b ? b : "https://api2.cursor.sh", path);
}

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
    char *gm = cursor_api("/aiserver.v1.DashboardService/GetMe");
    int grc = auth_http("POST", gm, &h, "{}", 3000, c, &r);
    free(gm);
    if (grc == 0 && r.status >= 200 && r.status < 300) {
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
    unsigned char vb[96], dig[SHA256_DIGEST_LENGTH]; /* generatePKCE: 96 random bytes */
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
    char *pp = xasprintf("/auth/poll?uuid=%s&verifier=%s", uuid, verifier);
    char *poll_url = cursor_api(pp);
    free(pp);
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
    char *tu = cursor_api("/oauth/token");
    int rc = auth_http("POST", tu, &h, body, 30000, NULL, &r);
    free(tu);
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

/* registry/snowflake.ts normalizeSnowflakeAccountUrl lives in src/ai/provider_transport.c (shared with requests) */
#define snowflake_account_url ai_snowflake_account_url

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
/* Mistral Vibe browser sign-in (registry/oauth/mistral-vibe.ts + oauth/device-code.ts)       */
/* ======================================================================================= */

#define MISTRAL_CONSOLE_ORIGIN "https://console.mistral.ai"
#define MISTRAL_REQUEST_TIMEOUT_MS 30000
#define MISTRAL_POLL_INTERVAL_S 3          /* the Vibe CLI's poll cadence ... */
#define MISTRAL_MAX_POLL_FAILURES 3        /* ... and failure budget */
#define MISTRAL_MAX_RETRY_AFTER_MS 30000   /* so one large value cannot hide an approval until expiry */
#define MISTRAL_WORKSPACE_DENIED 9004      /* console error_code: the workspace may not mint API keys */
#define MISTRAL_EXPIRED "Mistral sign-in expired; run /login again"

/* POST to the console API; r is filled on 0, and *err is set when the request itself failed */
static int mistral_post(const actx_t *c, const char *path, const json_t *body, http_resp_t *r, char **err) {
    if (cancelled(c, err)) return -1;
    http_headers_t h = {0};
    http_headers_set(&h, "Content-Type", "application/json");
    char *url = xasprintf(MISTRAL_CONSOLE_ORIGIN "/api%s", path), *js = json_dump(body, 0);
    int rc = auth_http("POST", url, &h, js, MISTRAL_REQUEST_TIMEOUT_MS, c, r);
    free(url);
    free(js);
    http_headers_free(&h);
    if (rc != 0) {
        if (!cancelled(c, err)) *err = xstrdup(r->error ? r->error : "request failed");
        http_resp_free(r);
        return -1;
    }
    return 0;
}

/* Accept only console URLs at or below base_path (segment-aware: /api rejects /api-x). */
static bool mistral_console_url(const char *value, const char *base_path) {
    url_t u;
    if (url_parse(value, &u) != 0) return false;
    size_t n = strlen(base_path);
    bool ok = str_eq(u.scheme, "https") && str_eq(u.host, "console.mistral.ai") && u.port == 443 && u.path &&
              (!n || (!strncmp(u.path, base_path, n) && (!u.path[n] || u.path[n] == '/' || u.path[n] == '?')));
    url_free(&u);
    return ok;
}

/* one poll of signInPoller: 1 complete (*token set), 0 pending, 2 slow_down (*retry_ms, -1 = none), -1 terminal */
static int mistral_poll(const actx_t *c, const char *poll_url, int *failures, int64_t timeout_ms, char **token,
                        int64_t *retry_ms, char **err) {
    http_resp_t r;
    int hr = auth_http("GET", poll_url, NULL, NULL, MIN(timeout_ms, (int64_t)MISTRAL_REQUEST_TIMEOUT_MS), c, &r);
    if (actx_aborted(c)) {
        http_resp_free(&r);
        return 0;
    }
    int status = hr == 0 ? r.status : 0;
    if (status == 410) {
        http_resp_free(&r);
        *err = xstrdup(MISTRAL_EXPIRED);
        return -1;
    }
    if (status == 429) {
        *failures = 0;
        int64_t ra = ai_retry_after_from_headers(&r.headers);
        *retry_ms = ra > 0 ? MIN(ra, (int64_t)MISTRAL_MAX_RETRY_AFTER_MS) : -1;
        http_resp_free(&r);
        return 2;
    }
    bool ok2xx = status >= 200 && status < 300;
    json_t *j = ok2xx ? body_json(&r) : NULL;
    const char *st = json_is_obj(j) ? json_get_str(j, "status") : NULL;
    json_t *xt = json_get(j, "exchange_token"), *msg = json_get(j, "message");
    bool valid = st && (!xt || json_is_str(xt)) && (!msg || json_is_str(msg));
    http_resp_free(&r);
    if (!valid) {
        json_free(j);
        if (++*failures < MISTRAL_MAX_POLL_FAILURES) return 0;
        char *reason = !status ? xstrdup("request failed") : ok2xx ? xstrdup("malformed response") : xasprintf("HTTP %d", status);
        *err = xasprintf("Mistral sign-in status unavailable: %s", reason);
        free(reason);
        return -1;
    }
    *failures = 0;
    int rc = -1;
    if (str_eq(st, "pending")) rc = 0;
    else if (str_eq(st, "completed")) {
        if (xt && *json_sv(xt)) {
            *token = xstrdup(json_sv(xt));
            rc = 1;
        } else *err = xstrdup("Mistral sign-in completed without an exchange token");
    } else if (str_eq(st, "expired")) *err = xstrdup(MISTRAL_EXPIRED);
    else if (str_eq(st, "denied")) *err = xstrdup("Mistral sign-in was denied");
    else if (str_eq(st, "error")) *err = xstrdup(msg && *json_sv(msg) ? json_sv(msg) : "Mistral sign-in failed");
    else *err = xasprintf("Mistral sign-in returned unknown state \"%s\"", st);
    json_free(j);
    return rc;
}

static int mistral_vibe_login(const actx_t *c, alogin_t *out, char **err) {
    unsigned char vb[96], dig[SHA256_DIGEST_LENGTH]; /* generatePKCE: 96 random bytes */
    rand_bytes(vb, sizeof vb);
    char *verifier = base64url_encode(vb, sizeof vb);
    SHA256((const unsigned char *)verifier, strlen(verifier), dig);
    char *challenge = base64url_encode(dig, sizeof dig);
    const char *start_msg = "Failed to start Mistral browser sign-in";
    json_t *sb = json_obj(), *started = NULL;
    json_set_str(sb, "code_challenge", challenge);
    json_set_str(sb, "code_challenge_method", "S256");
    free(challenge);
    http_resp_t r;
    int prc = mistral_post(c, "/vibe/sign-in", sb, &r, err);
    json_free(sb);
    char *token = NULL;
    int rc = -1;
    if (prc != 0) goto done;
    if (r.status < 200 || r.status >= 300) {
        *err = xasprintf("%s: HTTP %d", start_msg, r.status);
        http_resp_free(&r);
        goto done;
    }
    started = body_json(&r);
    http_resp_free(&r);
    const char *pid = json_get_str(started, "process_id"), *sign_url = json_get_str(started, "sign_in_url"),
               *poll_url = json_get_str(started, "poll_url"), *exp = json_get_str(started, "expires_at");
    if (!pid || !*pid || !sign_url || !poll_url || !exp) {
        *err = xasprintf("%s: malformed response", start_msg);
        goto done;
    }
    double exp_ms = 0;
    if (!trace_parse_iso_ms(exp, &exp_ms)) {
        *err = xasprintf("%s: invalid expires_at", start_msg);
        goto done;
    }
    if (!mistral_console_url(sign_url, "")) {
        *err = xasprintf("%s: unexpected URL %s", start_msg, sign_url);
        goto done;
    }
    if (!mistral_console_url(poll_url, "/api")) {
        *err = xasprintf("%s: unexpected URL %s", start_msg, poll_url);
        goto done;
    }
    actx_auth(c, sign_url, NULL, "Sign in with your Mistral account and approve Mistral Vibe access, then return here.");
    actx_progress(c, "Waiting for Mistral sign-in to complete...");

    /* pollOAuthDeviceCodeFlow; Retry-After stretches only the next wait, at least the normal interval */
    int64_t scale = scale_ms();
    int64_t interval_ms = MISTRAL_POLL_INTERVAL_S * scale;
    int64_t remaining_ms = (int64_t)exp_ms - now_ms();
    int64_t deadline = mono_ms() + MAX((int64_t)0, remaining_ms);
    int failures = 0;
    while (mono_ms() < deadline) {
        if (cancelled(c, err)) goto done;
        int64_t retry_ms = -1;
        int pr = mistral_poll(c, poll_url, &failures, deadline - mono_ms(), &token, &retry_ms, err);
        if (cancelled(c, err)) goto done;
        if (pr == 1) break;
        if (pr < 0) {
            if (mono_ms() >= deadline) { /* a failure at the deadline reads as the timeout */
                free(*err);
                *err = NULL;
            }
            break;
        }
        if (mono_ms() >= deadline) break;
        /* retry_ms arrives in real milliseconds; the test scale shrinks provider seconds */
        int64_t wait_ms = pr == 2 && retry_ms >= 0 ? MAX(interval_ms, retry_ms * scale / 1000) : interval_ms;
        int64_t left = deadline - mono_ms();
        if (left <= 0) break;
        if (!actx_sleep(c, MIN(wait_ms, left))) {
            cancelled(c, err);
            goto done;
        }
    }
    if (!token) {
        if (!*err) *err = xstrdup("Device flow timed out");
        goto done;
    }
    actx_progress(c, "Exchanging sign-in for a Mistral API key...");
    const char *ex_msg = "Failed to exchange Mistral sign-in for an API key";
    char *ep = url_encode(pid), *path = xasprintf("/vibe/sign-in/%s/exchange", ep);
    free(ep);
    json_t *xb = json_obj();
    json_set_str(xb, "exchange_token", token);
    json_set_str(xb, "code_verifier", verifier);
    prc = mistral_post(c, path, xb, &r, err);
    json_free(xb);
    free(path);
    if (prc != 0) goto done;
    json_t *x = body_json(&r);
    if (r.status == 403 && json_is_num(json_get(x, "error_code")) && json_get_int(x, "error_code", 0) == MISTRAL_WORKSPACE_DENIED) {
        *err = xstrdup("Your Mistral workspace can't be used for Mistral Vibe; run /login again and pick another workspace in the browser, or "
                       "ask your administrator");
    } else if (r.status < 200 || r.status >= 300) {
        *err = xasprintf("%s: HTTP %d", ex_msg, r.status);
    } else {
        const char *key = json_get_str(x, "api_key");
        if (key && *key) {
            out->api_key = xstrdup(key); /* /login stores it as a plain API-key credential */
            rc = 0;
        } else *err = xasprintf("%s: malformed response", ex_msg);
    }
    json_free(x);
    http_resp_free(&r);
done:
    json_free(started);
    free(token);
    free(verifier);
    return rc;
}

/* ======================================================================================= */
/* dispatch                                                                                  */
/* ======================================================================================= */

/* ---------- devin-cli (registry/oauth/devin-cli.ts) ---------- */

/* hosts verified to accept the same raw seat key; the key is sent to the provider's default host, so a CLI bound
 * elsewhere must not be adopted */
static const char *const DEVIN_KNOWN_API_HOSTS[] = {"server.codeium.com", "server.enterprise.windsurf.com"};

/* one TOML string value at *pp ('...' or "..." with escapes, single line). Owned, NULL when malformed. */
static char *toml_string(const char **pp) {
    const char *p = *pp;
    char q = *p++;
    sb_t b = SB_INIT;
    for (; *p && *p != '\n' && *p != q; p++) {
        if (q == '"' && *p == '\\') {
            p++;
            switch (*p) {
            case 'n': sb_putc(&b, '\n'); break;
            case 't': sb_putc(&b, '\t'); break;
            case 'r': sb_putc(&b, '\r'); break;
            case 'b': sb_putc(&b, '\b'); break;
            case 'f': sb_putc(&b, '\f'); break;
            case '"': sb_putc(&b, '"'); break;
            case '\\': sb_putc(&b, '\\'); break;
            default: sb_free(&b); return NULL; /* \u escapes do not occur in a seat key or URL */
            }
            continue;
        }
        sb_putc(&b, *p);
    }
    if (*p != q) {
        sb_free(&b);
        return NULL;
    }
    *pp = p + 1;
    return sb_detach(&b);
}

/* a bare TOML scalar: number, date, boolean, inf or nan */
static bool toml_bare_scalar(const char *p) {
    size_t n = strcspn(p, " \t#");
    static const char *const words[] = {"true", "false", "inf", "+inf", "-inf", "nan", "+nan", "-nan"};
    for (size_t i = 0; i < ARRAY_LEN(words); i++)
        if (strlen(words[i]) == n && !strncmp(p, words[i], n)) return true;
    if (!isdigit((unsigned char)*p) && *p != '+' && *p != '-') return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)p[i]) && !strchr("+-._:", p[i])) return false;
    return n > 0;
}

/* The top-level string keys of a TOML document as a JSON object (tables, arrays and other scalars are skipped). Owned,
 * NULL with *why (owned) on a syntax error. omp parses with Bun.TOML; the credentials file only has flat keys. */
static json_t *toml_top_strings(const char *text, char **why) {
    json_t *out = json_obj();
    bool in_table = false;
    int depth = 0; /* inside a multi-line array or inline table */
    for (const char *line = text; line && *line;) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char *row = xstrndup(line, len);
        char *t = str_trim(row);
        const char *p = t;
        line = nl ? nl + 1 : NULL;
        bool bad = false;
        if (depth > 0) {
            for (; *p && *p != '#'; p++) {
                if (*p == '"' || *p == '\'') {
                    char *s = toml_string(&p);
                    if (!s) {
                        bad = true;
                        break;
                    }
                    free(s);
                    p--;
                } else if (*p == '[' || *p == '{') {
                    depth++;
                } else if (*p == ']' || *p == '}') {
                    depth--;
                }
            }
        } else if (*p && *p != '#') {
            if (*p == '[') {
                in_table = true;
                if (!strchr(p, ']')) bad = true;
            } else {
                const char *k = p;
                char *key = NULL;
                if (*p == '"' || *p == '\'') {
                    key = toml_string(&p);
                } else {
                    while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.')) p++;
                    if (p > k) key = xstrndup(k, (size_t)(p - k));
                }
                while (*p == ' ' || *p == '\t') p++;
                if (!key || *p != '=') {
                    bad = true;
                } else {
                    p++;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p == '"' || *p == '\'') {
                        char *v = toml_string(&p);
                        if (!v) bad = true;
                        else if (!in_table) json_set(out, key, json_str_own(v));
                        else free(v);
                    } else if (*p != '[' && *p != '{' && !toml_bare_scalar(p)) {
                        *why = xasprintf("TOML Parse error: Expected a value but found '%c'", *p ? *p : ' ');
                        bad = true;
                    } else {
                        for (; *p && *p != '#'; p++) {
                            if (*p == '[' || *p == '{') depth++;
                            else if (*p == ']' || *p == '}') depth--;
                        }
                    }
                }
                free(key);
            }
        }
        free(t);
        free(row);
        if (bad) {
            if (!*why) *why = xstrdup("TOML Parse error: Unexpected token");
            json_free(out);
            return NULL;
        }
    }
    return out;
}

/* URL.host: the default port is elided. Owned, NULL when the URL does not parse. */
static char *url_host_of(const char *u) {
    url_t parsed;
    if (url_parse(u, &parsed) != 0) return NULL;
    bool def = (str_eq(parsed.scheme, "https") && parsed.port == 443) || (str_eq(parsed.scheme, "http") && parsed.port == 80);
    char *h = def ? xstrdup(parsed.host) : xasprintf("%s:%d", parsed.host, parsed.port);
    url_free(&parsed);
    for (char *p = h; *p; p++) *p = (char)tolower((unsigned char)*p);
    return h;
}

/* adopts the Windsurf seat key the official Devin CLI persists as a stored `devin` API key; never prompts and never
 * touches the network */
static int devin_cli_login(alogin_t *out, char **err) {
    strvec_t files = {0};
    const char *xdg = env_get("XDG_DATA_HOME");
    char *dirs[3] = {!str_empty(xdg) ? xstrdup(xdg) : NULL, xasprintf("%s/.local/share", home_dir()),
                     xasprintf("%s/Library/Application Support", home_dir())};
    for (int i = 0; i < 3; i++) {
        if (!dirs[i]) continue;
        char *f = xasprintf("%s/devin/credentials.toml", dirs[i]);
        if (!strvec_contains(&files, f)) strvec_push(&files, f);
        free(f);
        free(dirs[i]);
    }
    int rc = -1;
    char *text = NULL;
    const char *file = NULL;
    for (size_t i = 0; i < files.n && !file; i++) {
        size_t n = 0;
        errno = 0;
        text = fs_read(files.v[i], &n);
        if (text) file = files.v[i];
        else if (errno != ENOENT && errno != ENOTDIR) {
            *err = xasprintf("Cannot read Devin CLI credentials at %s: %s", files.v[i], strerror(errno));
            goto done;
        }
    }
    if (!file) {
        char *probed = strvec_join(&files, ", ");
        *err = xasprintf("No Devin CLI credentials found (probed: %s). Run `devin auth login`, then retry.", probed);
        free(probed);
        goto done;
    }
    char *why = NULL;
    json_t *record = toml_top_strings(text, &why);
    if (!record) {
        *err = xasprintf("Devin CLI credentials at %s are not valid TOML: %s", file, why);
        free(why);
        goto done;
    }
    char *key = str_trim(json_get_str(record, "windsurf_api_key") ? json_get_str(record, "windsurf_api_key") : "");
    const char *server = json_get_str(record, "api_server_url");
    if (!*key) {
        *err = xasprintf("Devin CLI credentials at %s have no windsurf_api_key. Run `devin auth login`, then retry.", file);
    } else if (!str_empty(server)) {
        char *host = url_host_of(server);
        bool known = false;
        for (size_t i = 0; host && i < ARRAY_LEN(DEVIN_KNOWN_API_HOSTS); i++) known = known || !strcmp(host, DEVIN_KNOWN_API_HOSTS[i]);
        if (!known)
            *err = xasprintf("Devin CLI credentials at %s point at unsupported API server \"%s\" (supported: %s, %s).", file, server,
                             DEVIN_KNOWN_API_HOSTS[0], DEVIN_KNOWN_API_HOSTS[1]);
        free(host);
    }
    if (!*err) {
        out->api_key = key;
        key = NULL;
        rc = 0;
    }
    free(key);
    json_free(record);
done:
    free(text);
    strvec_free(&files);
    return rc;
}

/* ---------- devin-windsurf (registry/oauth/devin-windsurf.ts) ---------- */

#define WINDSURF_EXCHANGE_URL "https://server.codeium.com/exa.seat_management_pb.SeatManagementService/ExchangePKCEAuthorizationCode"

/* field 1 (apiKey) of ExchangePKCEAuthorizationCodeResponse; owned, NULL when the bytes are no such message */
static char *windsurf_api_key_field(const unsigned char *p, size_t n) {
    char *key = NULL;
    for (size_t pos = 0; pos < n;) {
        uint64_t tag = 0, len = 0;
        if (pb_get_varint(p, n, &pos, &tag) != 0) goto bad;
        switch (tag & 7) {
        case 0:
            if (pb_get_varint(p, n, &pos, &len) != 0) goto bad;
            break;
        case 1:
            if (n - pos < 8) goto bad;
            pos += 8;
            break;
        case 2:
            if (pb_get_varint(p, n, &pos, &len) != 0 || len > n - pos) goto bad;
            if (tag >> 3 == 1) {
                free(key);
                key = xstrndup((const char *)p + pos, (size_t)len);
            }
            pos += (size_t)len;
            break;
        case 5:
            if (n - pos < 4) goto bad;
            pos += 4;
            break;
        default: goto bad;
        }
    }
    return key ? key : xstrdup("");
bad:
    free(key);
    return NULL;
}

/* decodeDevinUnaryMessage: the plain message, else the gunzipped one */
static char *windsurf_decode_key(const char *body, size_t len) {
    char *key = windsurf_api_key_field((const unsigned char *)body, len);
    if (key) return key;
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return NULL;
    sb_t out = SB_INIT;
    unsigned char buf[4096];
    zs.next_in = (unsigned char *)body;
    zs.avail_in = (uInt)len;
    int r;
    do {
        zs.next_out = buf;
        zs.avail_out = sizeof buf;
        r = inflate(&zs, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) break;
        sb_appendn(&out, (const char *)buf, sizeof buf - zs.avail_out);
    } while (r != Z_STREAM_END && out.len < (1u << 24));
    inflateEnd(&zs);
    if (r == Z_STREAM_END) key = windsurf_api_key_field((const unsigned char *)out.s, out.len);
    sb_free(&out);
    return key;
}

/* exchangeWindsurfCode: the authorization code for the seat's raw API key. Owned, NULL with *err. */
char *auth_windsurf_exchange(const actx_t *c, const char *code, const char *verifier, char **err) {
    sb_t b = SB_INIT;
    pb_put_len_bytes(&b, 1, code, strlen(code));
    pb_put_len_bytes(&b, 2, verifier, strlen(verifier));
    http_headers_t h = {0};
    /* any other content type is answered with 200 and an empty body */
    http_headers_set(&h, "content-type", "application/proto");
    http_headers_set(&h, "connect-protocol-version", "1");
    http_headers_set(&h, "accept", "*/*");
    http_resp_t r;
    int rc = auth_http("POST", WINDSURF_EXCHANGE_URL, &h, sb_cstr(&b), 30000, c, &r);
    http_headers_free(&h);
    sb_free(&b);
    char *key = NULL;
    if (rc != 0) {
        *err = xasprintf("Windsurf code exchange failed: %s", r.error ? r.error : "request failed");
    } else if (r.status < 200 || r.status >= 300) {
        char *msg = xasprintf("Windsurf code exchange failed: %d %s", r.status, r.body ? r.body : "");
        *err = str_trim(msg);
        free(msg);
    } else {
        key = windsurf_decode_key(r.body ? r.body : "", r.body_len);
        if (str_empty(key)) {
            free(key);
            key = NULL;
            *err = xstrdup("Windsurf code exchange returned no API key");
        } else if (str_starts(key, "gsk-")) {
            /* the exchange returns the `g`-prefixed wire form (`gsk-ws-…`); RPCs authenticate with the raw key */
            memmove(key, key + 1, strlen(key));
        }
    }
    http_resp_free(&r);
    return key;
}

/* Windsurf Enterprise seats cannot finish the Devin-native flow: the Windsurf editor authorize page (PKCE + loopback
 * callback on the port the official CLI uses) and the seat key from the code exchange */
static int devin_windsurf_login(const actx_t *c, alogin_t *out, char **err) {
    json_t *login = json_parse_str(
        "{\"scopes\":[],\"pkce\":true,\"state\":\"uuid\",\"standardAuthorizeParams\":false,"
        "\"authorizeUrl\":{\"value\":\"https://windsurf.com/editor/signin\"},"
        "\"authorizeParams\":{\"response_type\":\"code\",\"redirect_uri\":\"{redirect_uri}\",\"state\":\"{state}\","
        "\"code_challenge\":\"{code_challenge}\",\"code_challenge_method\":\"S256\",\"prompt\":\"login\","
        "\"redirect_parameters_type\":\"query\"},"
        "\"instructions\":\"Sign in with your Windsurf (Enterprise) account in your browser.\","
        "\"callback\":{\"port\":59654,\"path\":\"/callback\",\"hostname\":\"127.0.0.1\",\"portFallback\":true},"
        "\"exchangeHook\":\"devin-windsurf\"}");
    json_t *cred = NULL;
    int rc = auth_oauth_code_flow(c, login, &cred, err);
    json_free(login);
    if (rc == 0) out->api_key = xstrdup(json_get_str(cred, "access") ? json_get_str(cred, "access") : "");
    json_free(cred);
    return rc;
}

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
    if (str_eq(hook, "mistral-vibe-sign-in")) return mistral_vibe_login(c, out, err);
    if (str_eq(hook, "devin-cli")) return devin_cli_login(out, err);
    if (str_eq(hook, "devin-windsurf")) return devin_windsurf_login(c, out, err);
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
