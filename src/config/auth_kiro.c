/* auth_kiro.c - Kiro login and refresh hooks (`login "custom" hook="kiro-login"`, `refresh hook="kiro-refresh"`), ported
 * from omp v19.1.0 packages/ai/src/registry/oauth/kiro.ts: AWS IAM Identity Center and Builder ID (SSO-OIDC device grant),
 * Google/GitHub (Kiro's social device flow) and ksk_ API keys. No Kiro sign-in returns an account claim, so each login
 * mints a random accountId; it survives refresh and keys the model cache. */
#define _GNU_SOURCE
#include "config/auth_engine.h"

#include "ai/kiro.h"
#include "prompt/template.h"
#include "session/trace.h"
#include "tools/outsink.h"

#include <ctype.h>

#define KIRO_REQUEST_TIMEOUT_MS 30000
#define KIRO_MAX_RESPONSE_BYTES (128 * 1024)
#define KIRO_MAX_ATTEMPTS 3
/* access-token lifetime assumed when a token response names none (the social service omits it) */
#define KIRO_DEFAULT_TOKEN_LIFETIME_MS 3600000LL
/* Builder ID: the public Builder portal in us-east-1, with no organization and so no profile */
#define BUILDER_ID_START_URL "https://view.awsapps.com/start"
#define BUILDER_ID_REGION "us-east-1"
/* Kiro's auth service behind Google/GitHub sign-in; it needs no client registration */
#define SOCIAL_AUTH_URL "https://prod.us-east-1.auth.desktop.kiro.dev"
/* the profile the Kiro CLI assigns a social account that lists none; the runtime rejects a bearer without one */
#define SOCIAL_DEFAULT_PROFILE_ARN "arn:aws:codewhisperer:us-east-1:699475941385:profile/EHGA3GRVQMUK"

static int64_t scale_ms(void) {
    const char *s = env_get("OMC_AUTH_POLL_SCALE_MS"); /* test-only: milliseconds per provider "second" */
    long v = s ? strtol(s, NULL, 10) : 0;
    return v > 0 && v <= 1000 ? v : 1000;
}

typedef enum { HDR_OIDC, HDR_SOCIAL } hdr_kind_t;

typedef struct {
    bool ok;
    int status;
    json_t *data; /* NULL when the body is not JSON */
} kresp_t;

static void kresp_free(kresp_t *r) {
    json_free(r->data);
    memset(r, 0, sizeof *r);
}

static volatile sig_atomic_t *abort_flag(const actx_t *c) {
    return c && c->h ? (volatile sig_atomic_t *)c->h->abort : NULL;
}

static bool aborted(const actx_t *c) { return c && actx_aborted(c); }

/* postJson: POST a JSON body and read a bounded JSON reply. Transport errors, 408/429 and 5xx retry (fetchWithRetry, 3
 * attempts, 500 ms·2^n); each attempt rebuilds its headers so amz-sdk-request carries the attempt number.
 * 0 ok; -1 + *err ("Kiro request timed out" / "Kiro request failed" / "Login cancelled"); *timed_out on a timeout. */
static int post_json(const actx_t *c, const char *url, const json_t *body, hdr_kind_t kind, kresp_t *out, bool *timed_out,
                     char **err) {
    memset(out, 0, sizeof *out);
    if (timed_out) *timed_out = false;
    if (aborted(c)) {
        *err = xstrdup("Login cancelled");
        return -1;
    }
    char *b = json_dump_js(body);
    char *u = auth_rewrite_url(url);
    int64_t deadline = mono_ms() + KIRO_REQUEST_TIMEOUT_MS;
    int rc = -1;
    for (int attempt = 0; attempt < KIRO_MAX_ATTEMPTS; attempt++) {
        http_headers_t h = {0};
        if (kind == HDR_OIDC) kiro_oidc_headers(&h, attempt + 1, KIRO_MAX_ATTEMPTS);
        else kiro_social_headers(&h);
        int64_t left = deadline - mono_ms();
        http_req_t req = {.method = "POST",
                          .url = u,
                          .headers = &h,
                          .body = b,
                          .body_len = strlen(b),
                          .timeout_ms = left > 0 ? left : 1,
                          .max_body = KIRO_MAX_RESPONSE_BYTES,
                          .cancel = abort_flag(c)};
        http_resp_t r;
        int hr = http_request(&req, &r);
        http_headers_free(&h);
        bool transport = hr != 0 || r.error;
        int st = transport ? 0 : r.status;
        bool retry = transport || st == 408 || st == 429 || st >= 500;
        if (aborted(c)) {
            http_resp_free(&r);
            *err = xstrdup("Login cancelled");
            break;
        }
        if (!retry || attempt + 1 >= KIRO_MAX_ATTEMPTS || mono_ms() >= deadline) {
            if (transport) {
                bool to = mono_ms() >= deadline;
                if (timed_out) *timed_out = to;
                *err = xstrdup(to ? "Kiro request timed out" : "Kiro request failed");
            } else {
                out->status = st;
                out->ok = st >= 200 && st < 300;
                out->data = r.body && !r.truncated ? json_parse(r.body, r.body_len, NULL) : NULL;
                rc = 0;
            }
            http_resp_free(&r);
            break;
        }
        http_resp_free(&r);
        int64_t delay = MIN((int64_t)500 << attempt, deadline - mono_ms());
        if (c ? !actx_sleep(c, MAX((int64_t)0, delay)) : (sleep_ms(MAX((int64_t)0, delay)), false)) {
            *err = xstrdup("Login cancelled");
            break;
        }
    }
    free(u);
    free(b);
    return rc;
}

/* requireOk */
static json_t *require_ok(const kresp_t *r, const char *message, char **err) {
    if (!r->ok) {
        *err = xasprintf("%s: HTTP %d", message, r->status);
        return NULL;
    }
    if (!json_is_obj(r->data)) {
        *err = xstrdup("Kiro returned an invalid response");
        return NULL;
    }
    return r->data;
}

static const char *field(const json_t *d, const char *key, char **err) {
    const char *v = json_get_str(d, key);
    if (!v || !*v) {
        if (!*err) *err = xasprintf("Kiro response missing %s", key);
        return NULL;
    }
    return v;
}

static double positive(const json_t *d, const char *key, char **err) {
    json_t *v = json_get(d, key);
    if (!json_is_num(v) || !(v->u.num.d > 0)) {
        if (!*err) *err = xasprintf("Kiro response has invalid %s", key);
        return 0;
    }
    return v->u.num.d;
}

/* parseTokens: refresh replies may omit the refresh token (kept) and the social service the lifetime (expiresIn
 * seconds, else ISO expiresAt, else the default) */
static json_t *parse_tokens(const json_t *data, const char *current_refresh, char **err) {
    if (!json_is_obj(data)) {
        *err = xstrdup("Kiro returned invalid token data");
        return NULL;
    }
    const char *access = field(data, "accessToken", err);
    if (!access) return NULL;
    const char *refresh = json_get_str(data, "refreshToken");
    if (!refresh || !*refresh) refresh = current_refresh;
    if (!refresh || !*refresh) {
        *err = xstrdup("Kiro response missing refreshToken");
        return NULL;
    }
    int64_t now = now_ms();
    double at = 0;
    bool has_at = json_get_str(data, "expiresAt") && trace_parse_iso_ms(json_get_str(data, "expiresAt"), &at);
    json_t *ein = json_get(data, "expiresIn");
    int64_t expires = json_is_num(ein) && ein->u.num.d > 0 ? now + (int64_t)(ein->u.num.d * 1000)
                      : has_at && at > (double)now      ? (int64_t)at
                                                          : now + KIRO_DEFAULT_TOKEN_LIFETIME_MS;
    json_t *c = json_obj();
    json_set_str(c, "type", "oauth");
    json_set_str(c, "access", access);
    json_set_str(c, "refresh", refresh);
    json_set_int(c, "expires", expires);
    return c;
}

static void show_code(const actx_t *c, const char *url, const char *instructions) {
    actx_auth(c, url, NULL, instructions);
    actx_progress(c, "Waiting for device authorization...");
}

/* ---------- pollOAuthDeviceCodeFlow (registry/oauth/device-code.ts) ---------- */

typedef enum { POLL_COMPLETE, POLL_PENDING, POLL_SLOW_DOWN, POLL_ERROR } poll_t;
typedef poll_t (*poll_fn)(const actx_t *c, void *ud, json_t **value, char **err);

static json_t *poll_device(const actx_t *c, double interval_s, double expires_s, poll_fn fn, void *ud, char **err) {
    int64_t scale = scale_ms();
    int64_t deadline = mono_ms() + (int64_t)(expires_s * (double)scale);
    int64_t interval = MAX((int64_t)1000 * scale / 1000, (int64_t)(interval_s * (double)scale));
    int slow = 0;
    while (mono_ms() < deadline) {
        if (aborted(c)) {
            *err = xstrdup("Login cancelled");
            return NULL;
        }
        json_t *value = NULL;
        char *perr = NULL;
        poll_t r = fn(c, ud, &value, &perr);
        if (aborted(c)) {
            free(perr);
            json_free(value);
            *err = xstrdup("Login cancelled");
            return NULL;
        }
        if (r == POLL_COMPLETE) return value;
        if (r == POLL_ERROR) {
            if (mono_ms() >= deadline) {
                free(perr);
                break;
            }
            *err = perr;
            return NULL;
        }
        if (mono_ms() >= deadline) break;
        if (r == POLL_SLOW_DOWN) {
            slow++;
            interval = MAX((int64_t)scale, interval + 5 * scale);
        }
        int64_t remaining = deadline - mono_ms();
        if (remaining <= 0) break;
        if (!actx_sleep(c, MIN(interval, remaining))) {
            *err = xstrdup("Login cancelled");
            return NULL;
        }
    }
    *err = xstrdup(slow > 0 ? "Device flow timed out after one or more slow_down responses. This is often caused by clock drift "
                              "in WSL or VM environments. Please sync or restart the VM clock and try again."
                            : "Device flow timed out");
    return NULL;
}

/* ---------- profiles ---------- */

/* profileLabel: the ARN embeds the AWS account id, so a profile is labelled by its name, else fallback, else the ARN's
 * trailing segment */
static char *profile_label(const json_t *profile, const char *fallback) {
    const char *name = json_get_str(profile, "name");
    size_t n = 0;
    char *s = out_sanitize(name ? name : "", name ? strlen(name) : 0, &n);
    char *t = str_trim(s);
    free(s);
    if (*t) return t;
    free(t);
    if (fallback && *fallback) return xstrdup(fallback);
    const char *arn = json_get_str(profile, "arn") ? json_get_str(profile, "arn") : "";
    const char *slash = strrchr(arn, '/');
    return xstrdup(slash ? slash + 1 : arn);
}

static void profile_row(json_t *cred, const char *arn, const char *org_name) {
    json_set_str(cred, "kiroProfileArn", arn);
    char *region = kiro_profile_region(arn);
    if (region) json_set_str(cred, "region", region);
    free(region);
    json_set_str(cred, "orgName", org_name);
}

/* listProfiles: the profiles the bearer sees across `regions`, deduplicated by ARN; an empty result is trusted only when
 * every region answered (a failed region may hold the account's only profile) */
static json_t *list_profiles(const actx_t *c, const char *token, const char *const *regions, char **err) {
    json_t *all = json_arr();
    bool failed = false;
    for (int i = 0; i < 2; i++) {
        char *e = NULL;
        json_t *p = kiro_list_profiles(regions[i], token, abort_flag(c), &e);
        free(e);
        if (!p) {
            failed = true;
            continue;
        }
        json_t *x;
        json_arr_foreach(p, k, x) {
            bool dup = false;
            json_t *y;
            json_arr_foreach(all, j, y) {
                if (str_eq(json_get_str(y, "arn"), json_get_str(x, "arn"))) {
                    json_free(json_arr_remove(all, j));
                    json_arr_insert(all, j, json_clone(x)); /* Map.set: the later entry, at the first position */
                    dup = true;
                    break;
                }
            }
            if (!dup) json_push(all, json_clone(x));
        }
        json_free(p);
    }
    if (aborted(c)) {
        json_free(all);
        *err = xstrdup("Login cancelled");
        return NULL;
    }
    if (json_len(all) == 0 && failed) {
        json_free(all);
        *err = xstrdup("Unable to discover Kiro profiles");
        return NULL;
    }
    return all;
}

static int pick_profile(const actx_t *c, const json_t *profiles, const char *fallback, json_t *cred, char **err) {
    const json_t *profile = json_at(profiles, 0);
    if (json_len(profiles) > 1) {
        json_t *ctx = json_obj(), *entries = json_arr(), *p;
        json_arr_foreach(profiles, i, p) {
            json_t *e = json_obj();
            json_set_int(e, "number", (int64_t)i + 1);
            char *l = profile_label(p, NULL);
            json_set_str(e, "label", l);
            free(l);
            json_push(entries, e);
        }
        json_set(ctx, "profiles", entries);
        char *msg = prompt_render("ai/registry/oauth/prompts/kiro-profile-select.md", ctx);
        json_free(ctx);
        char *answer = actx_prompt(c, msg ? msg : "", "1", false, err);
        free(msg);
        if (!answer) return -1;
        char *t = str_trim(answer);
        free(answer);
        char *end = NULL;
        long n = *t ? strtol(t, &end, 10) : 0;
        bool ok = *t && end && !*end && n >= 1 && (size_t)n <= json_len(profiles);
        free(t);
        if (!ok) {
            *err = xstrdup("Invalid Kiro profile selection");
            return -1;
        }
        profile = json_at(profiles, (size_t)n - 1);
    }
    char *label = profile_label(profile, fallback);
    profile_row(cred, json_get_str(profile, "arn"), label);
    free(label);
    return 0;
}

/* ---------- IAM Identity Center / Builder ID ---------- */

typedef struct {
    char *url;
    json_t *grant;
} oidc_poll_t;

static poll_t oidc_poll(const actx_t *c, void *ud, json_t **value, char **err) {
    oidc_poll_t *p = ud;
    kresp_t r;
    bool to = false;
    char *perr = NULL;
    /* a timed-out poll request is retried on the next interval rather than failing the login */
    if (post_json(c, p->url, p->grant, HDR_OIDC, &r, &to, &perr) != 0) {
        if (to) {
            free(perr);
            return POLL_PENDING;
        }
        *err = perr;
        return POLL_ERROR;
    }
    const char *e = json_get_str(r.data, "error");
    poll_t out;
    if (str_eq(e, "authorization_pending")) out = POLL_PENDING;
    else if (str_eq(e, "slow_down")) out = POLL_SLOW_DOWN;
    else {
        const json_t *d = require_ok(&r, "Kiro device authorization rejected", err);
        *value = d ? parse_tokens(d, NULL, err) : NULL;
        out = *value ? POLL_COMPLETE : POLL_ERROR;
    }
    kresp_free(&r);
    return out;
}

static json_t *login_device(const actx_t *c, const char *start_url, const char *oidc_region, char **err) {
    char *oidc = xasprintf("https://oidc.%s.amazonaws.com", oidc_region);
    char *url = xasprintf("%s/client/register", oidc);
    json_t *reg = json_parse_str("{\"clientName\":\"Kiro CLI\",\"clientType\":\"public\",\"scopes\":[\"codewhisperer:completions\","
                                 "\"codewhisperer:analysis\",\"codewhisperer:conversations\"]}");
    kresp_t r = {0}, d = {0};
    json_t *cred = NULL, *tokens = NULL, *body = NULL;
    char *client_id = NULL, *client_secret = NULL;
    int rc = post_json(c, url, reg, HDR_OIDC, &r, NULL, err);
    json_free(reg);
    free(url);
    if (rc != 0) goto out;
    const json_t *client = require_ok(&r, "Kiro client registration failed", err);
    const char *cid = client ? field(client, "clientId", err) : NULL;
    const char *csec = cid ? field(client, "clientSecret", err) : NULL;
    if (!csec) goto out;
    client_id = xstrdup(cid);
    client_secret = xstrdup(csec);
    body = json_obj();
    json_set_str(body, "clientId", client_id);
    json_set_str(body, "clientSecret", client_secret);
    json_set_str(body, "startUrl", start_url);
    url = xasprintf("%s/device_authorization", oidc);
    rc = post_json(c, url, body, HDR_OIDC, &d, NULL, err);
    free(url);
    json_free(body);
    body = NULL;
    if (rc != 0) goto out;
    const json_t *dev = require_ok(&d, "Kiro device authorization failed", err);
    const char *device_code = dev ? field(dev, "deviceCode", err) : NULL;
    const char *uri = device_code ? field(dev, "verificationUriComplete", err) : NULL;
    const char *user_code = uri ? field(dev, "userCode", err) : NULL;
    if (!user_code) goto out;
    char *instr = xasprintf("Confirm code %s in the browser", user_code);
    show_code(c, uri, instr);
    free(instr);
    double interval = positive(dev, "interval", err);
    double expires = *err ? 0 : positive(dev, "expiresIn", err);
    if (*err) goto out;
    oidc_poll_t p = {xasprintf("%s/token", oidc), json_obj()};
    json_set_str(p.grant, "clientId", client_id);
    json_set_str(p.grant, "clientSecret", client_secret);
    json_set_str(p.grant, "deviceCode", device_code);
    json_set_str(p.grant, "grantType", "urn:ietf:params:oauth:grant-type:device_code");
    tokens = poll_device(c, interval, expires, oidc_poll, &p, err);
    free(p.url);
    json_free(p.grant);
    if (!tokens) goto out;
    char *acc = uuid_v4();
    json_set_str(tokens, "accountId", acc);
    free(acc);
    json_set_str(tokens, "kiroClientId", client_id);
    json_set_str(tokens, "kiroClientSecret", client_secret);
    json_set_str(tokens, "kiroOidcRegion", oidc_region);
    if (!strcmp(start_url, BUILDER_ID_START_URL)) {
        json_set_str(tokens, "region", BUILDER_ID_REGION);
        cred = tokens;
        tokens = NULL;
        goto out;
    }
    json_t *profiles = list_profiles(c, json_get_str(tokens, "access"), kiro_profile_regions(oidc_region), err);
    if (!profiles) goto out;
    if (json_len(profiles) == 0) *err = xstrdup("No Kiro profiles are available");
    else if (pick_profile(c, profiles, NULL, tokens, err) == 0) {
        cred = tokens;
        tokens = NULL;
    }
    json_free(profiles);
out:
    kresp_free(&r);
    kresp_free(&d);
    json_free(tokens);
    free(client_id);
    free(client_secret);
    free(oidc);
    return cred;
}

/* ---------- Google / GitHub (Kiro's social device flow) ---------- */

typedef struct {
    json_t *body;
    const char *label;
} social_poll_t;

static poll_t social_poll(const actx_t *c, void *ud, json_t **value, char **err) {
    social_poll_t *p = ud;
    kresp_t r;
    bool to = false;
    char *perr = NULL;
    if (post_json(c, SOCIAL_AUTH_URL "/oauth/device/poll", p->body, HDR_SOCIAL, &r, &to, &perr) != 0) {
        if (to) {
            free(perr);
            return POLL_PENDING;
        }
        *err = perr;
        return POLL_ERROR;
    }
    /* the poll answers HTTP 200 with its state in `status` */
    const json_t *d = require_ok(&r, "Kiro device poll failed", err);
    poll_t out = POLL_ERROR;
    if (d) {
        const char *st = json_get_str(d, "status");
        if (str_eq(st, "authorization_pending")) out = POLL_PENDING;
        else if (str_eq(st, "slow_down")) out = POLL_SLOW_DOWN;
        else if (!json_truthy(json_get(d, "accessToken"))) *err = xasprintf("Kiro %s sign-in was not completed", p->label);
        else {
            *value = json_clone(d);
            out = POLL_COMPLETE;
        }
    }
    kresp_free(&r);
    return out;
}

/* the poll's profile ARN wins; otherwise a listed profile is picked, and only an account that lists none (every region
 * answered) takes the profile the Kiro CLI assigns social users */
static json_t *login_social(const actx_t *c, const char *login_provider, char **err) {
    const char *label = !strcmp(login_provider, "Github") ? "GitHub" : login_provider;
    json_t *body = json_obj();
    json_set_str(body, "clientId", KIRO_SOCIAL_CLIENT_ID);
    json_set_str(body, "loginProvider", login_provider);
    kresp_t r;
    json_t *cred = NULL, *completed = NULL;
    int rc = post_json(c, SOCIAL_AUTH_URL "/oauth/device/authorization", body, HDR_SOCIAL, &r, NULL, err);
    json_free(body);
    if (rc != 0) return NULL;
    const json_t *dev = require_ok(&r, "Kiro device authorization failed", err);
    const char *device_code = dev ? field(dev, "deviceCode", err) : NULL;
    const char *user_code = device_code ? field(dev, "userCode", err) : NULL;
    const char *uri = user_code ? field(dev, "verificationUriComplete", err) : NULL;
    if (!uri) goto out;
    char *instr = xasprintf("Confirm code %s in the browser and sign in with %s", user_code, label);
    show_code(c, uri, instr);
    free(instr);
    double interval = positive(dev, "intervalInMilliseconds", err) / 1000;
    double expires = *err ? 0 : positive(dev, "expiresInMilliseconds", err) / 1000;
    if (*err) goto out;
    social_poll_t p = {json_obj(), label};
    json_set_str(p.body, "deviceCode", device_code);
    json_set_str(p.body, "clientId", KIRO_SOCIAL_CLIENT_ID);
    completed = poll_device(c, interval, expires, social_poll, &p, err);
    json_free(p.body);
    if (!completed) goto out;
    cred = parse_tokens(completed, NULL, err);
    if (!cred) goto out;
    char *acc = uuid_v4();
    json_set_str(cred, "accountId", acc);
    free(acc);
    const char *arn = json_get_str(completed, "profileArn");
    char *region = kiro_profile_region(arn);
    if (region) {
        free(region);
        profile_row(cred, arn, label);
        goto out;
    }
    json_t *profiles = list_profiles(c, json_get_str(cred, "access"), KIRO_BOOTSTRAP_REGIONS, err);
    if (!profiles || (json_len(profiles) > 0 && pick_profile(c, profiles, label, cred, err) != 0)) {
        json_free(cred);
        cred = NULL;
    } else if (json_len(profiles) == 0) profile_row(cred, SOCIAL_DEFAULT_PROFILE_ARN, label);
    json_free(profiles);
out:
    json_free(completed);
    kresp_free(&r);
    return cred;
}

/* ---------- API key ---------- */

/* pin a ksk_ key to its region (KIRO_API_REGION, else the one bootstrap region that accepts it) and probe its models */
static char *login_api_key(const actx_t *c, char **err) {
    auth_prompt_t q = {"Paste your Kiro API key", "ksk_...", false, true};
    char *input = actx_prompt_ex(c, &q, err);
    if (!input) return NULL;
    char *token = str_trim(input);
    free(input);
    char *out = NULL;
    kiro_cred_t pinned = {0};
    char *region = NULL;
    if (!str_starts(token, KIRO_API_KEY_PREFIX)) {
        *err = xstrdup("Kiro API keys must use the ksk_… format");
        goto done;
    }
    if (kiro_decode_credential(token, &pinned, err) != 0) goto done;
    region = pinned.region ? xstrdup(pinned.region) : kiro_resolve_api_key_region(token, abort_flag(c));
    if (aborted(c)) {
        *err = xstrdup("Login cancelled");
        goto done;
    }
    if (!region) {
        *err = xstrdup("Kiro API key did not resolve to one region; set KIRO_API_REGION to the key's AWS region");
        goto done;
    }
    kiro_cred_t cred = {.oauth = false, .token = token, .region = region};
    json_t *models = kiro_fetch_models(&cred, abort_flag(c));
    if (aborted(c)) *err = xstrdup("Login cancelled");
    else if (!models) *err = xasprintf("Kiro API key lists no models in %s", region);
    else out = kiro_encode_credential(&cred);
    json_free(models);
done:
    kiro_cred_free(&pinned);
    free(region);
    free(token);
    return out;
}

int auth_kiro_login(const actx_t *c, alogin_t *out, char **err) {
    char *msg = prompt_render("ai/registry/oauth/prompts/kiro-login-method.md", NULL);
    char *raw = actx_prompt(c, msg ? msg : "", "1", false, err);
    free(msg);
    if (!raw) return -1;
    char *t = str_trim(raw);
    free(raw);
    char *answer = str_lower(t);
    free(t);
    static const char *const methods[] = {"idc", "builder", "api", "google", "github"};
    const char *method = NULL;
    char *end = NULL;
    long n = *answer ? strtol(answer, &end, 10) : 0;
    if (*answer && end && !*end && n >= 1 && n <= 5) method = methods[n - 1];
    for (size_t i = 0; !method && i < ARRAY_LEN(methods); i++)
        if (!strcmp(answer, methods[i])) method = methods[i];
    free(answer);
    json_t *cred = NULL;
    if (str_eq(method, "idc")) {
        char *su = actx_prompt(c, "Enter Start URL", NULL, false, err);
        if (!su) return -1;
        char *start = str_trim(su);
        free(su);
        url_t pu;
        bool ok = url_parse(start, &pu) == 0;
        if (ok) {
            ok = str_starts(start, "https://") && pu.host && *pu.host;
            url_free(&pu);
        }
        if (!ok) {
            free(start);
            *err = xstrdup("Invalid AWS access portal URL");
            return -1;
        }
        char *rg = actx_prompt(c, "Enter Region", NULL, false, err);
        if (!rg) {
            free(start);
            return -1;
        }
        char *region = str_trim(rg);
        free(rg);
        if (!kiro_is_region(region)) *err = xstrdup("Invalid AWS Identity Center Region");
        else cred = login_device(c, start, region, err);
        free(start);
        free(region);
    } else if (str_eq(method, "builder")) {
        cred = login_device(c, BUILDER_ID_START_URL, BUILDER_ID_REGION, err);
    } else if (str_eq(method, "api")) {
        char *key = login_api_key(c, err);
        if (!key) return -1;
        out->api_key = key;
        return 0;
    } else if (str_eq(method, "google")) {
        cred = login_social(c, "Google", err);
    } else if (str_eq(method, "github")) {
        cred = login_social(c, "Github", err);
    } else {
        *err = xstrdup("Invalid Kiro authentication selection");
        return -1;
    }
    if (!cred) {
        if (!*err) *err = xstrdup("Kiro login failed");
        return -1;
    }
    out->cred = cred;
    return 0;
}

/* refresh hook: a row with an OIDC client refreshes against oidc.<kiroOidcRegion>; one without is a social login
 * refreshed by the Kiro auth service, which may move it to another profile. A failure no retry can repair says so
 * (omp also disables the row; omc has no credential lifecycle for that). */
int auth_kiro_refresh(const json_t *current, json_t **out, char **err) {
    const char *refresh = json_get_str(current, "refresh");
    const char *client_id = json_get_str(current, "kiroClientId");
    if (!refresh || !*refresh) {
        *err = xstrdup("Kiro refresh state is incomplete; run /login kiro again");
        return -1;
    }
    kresp_t r;
    json_t *body = json_obj();
    int rc;
    if (!client_id || !*client_id) {
        json_set_str(body, "refreshToken", refresh);
        rc = post_json(NULL, SOCIAL_AUTH_URL "/refreshToken", body, HDR_SOCIAL, &r, NULL, err);
        json_free(body);
        if (rc != 0) return -1;
        if (!r.ok) {
            bool definitive = r.status == 400 || r.status == 401;
            *err = definitive ? xasprintf("Kiro sign-in expired (HTTP %d); run /login kiro again", r.status)
                              : xasprintf("Kiro refresh failed: HTTP %d", r.status);
            kresp_free(&r);
            return -1;
        }
        const char *moved = json_get_str(r.data, "profileArn");
        char *mr = kiro_profile_region(moved);
        const char *arn = mr ? moved : json_get_str(current, "kiroProfileArn");
        free(mr);
        char *region = kiro_profile_region(arn);
        if (!region) {
            kresp_free(&r);
            *err = xstrdup("Kiro refresh state is incomplete; run /login kiro again");
            return -1;
        }
        json_t *t = parse_tokens(r.data, refresh, err);
        if (!t) {
            free(region);
            kresp_free(&r);
            return -1;
        }
        json_t *c = json_clone(current);
        json_merge(c, t);
        json_free(t);
        json_set_str(c, "kiroProfileArn", arn);
        json_set_str(c, "region", region);
        free(region);
        kresp_free(&r);
        *out = c;
        return 0;
    }
    const char *secret = json_get_str(current, "kiroClientSecret"), *oidc = json_get_str(current, "kiroOidcRegion");
    if (!secret || !*secret || !oidc || !*oidc) {
        json_free(body);
        *err = xstrdup("Kiro refresh state is incomplete; run /login kiro again");
        return -1;
    }
    json_set_str(body, "grantType", "refresh_token");
    json_set_str(body, "refreshToken", refresh);
    json_set_str(body, "clientId", client_id);
    json_set_str(body, "clientSecret", secret);
    char *url = xasprintf("https://oidc.%s.amazonaws.com/token", oidc);
    rc = post_json(NULL, url, body, HDR_OIDC, &r, NULL, err);
    free(url);
    json_free(body);
    if (rc != 0) return -1;
    if (!r.ok) {
        const char *raw = json_get_str(r.data, "error");
        if (!raw) raw = json_get_str(r.data, "__type");
        const char *code = raw ? (strrchr(raw, '#') ? strrchr(raw, '#') + 1 : raw) : "";
        static const char *const definitive[] = {"invalid_grant",  "InvalidGrantException", "expired_token",
                                                 "ExpiredTokenException", "invalid_client", "InvalidClientException"};
        bool def = false;
        for (size_t i = 0; i < ARRAY_LEN(definitive); i++)
            if (!strcmp(code, definitive[i])) def = true;
        *err = def ? xasprintf("Kiro refresh was rejected (%s); run /login kiro again", code)
                   : xasprintf("Kiro refresh failed: HTTP %d", r.status);
        kresp_free(&r);
        return -1;
    }
    json_t *t = parse_tokens(r.data, refresh, err);
    kresp_free(&r);
    if (!t) return -1;
    json_t *c = json_clone(current);
    json_merge(c, t);
    json_free(t);
    *out = c;
    return 0;
}
