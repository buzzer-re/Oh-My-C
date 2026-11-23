/* kiro.c - the native Kiro provider (api "kiro-api"): routing and credential codec, the Kiro CLI's wire identity, the
 * management client and model discovery, the request transform and the event-stream decoder. See kiro.h. */
#define _GNU_SOURCE
#include "ai/kiro.h"

#include "ai/aws.h"
#include "coding/turn_recovery.h"
#include "core/fs.h"
#include "prompt/template.h"

#include <ctype.h>
#include <math.h>

extern char *auth_get_api_key(const char *provider) __attribute__((weak));

#define PROMPT_DIR "ai/providers/kiro/prompts/"

/* ======================================================================
 * routing + credential codec (wire/kiro.ts)
 * ====================================================================== */

const char *const KIRO_BOOTSTRAP_REGIONS[2] = {"us-east-1", "eu-central-1"};
static const char *const KIRO_GOVCLOUD_REGIONS[2] = {"us-gov-west-1", "us-gov-east-1"};

const char *const *kiro_profile_regions(const char *oidc_region) {
    return str_starts(oidc_region ? oidc_region : "", "us-gov-") ? KIRO_GOVCLOUD_REGIONS : KIRO_BOOTSTRAP_REGIONS;
}

/* /^[a-z]{2}(?:-[a-z0-9]+)+-\d+$/ over [s, s + n) */
static bool region_span(const char *s, size_t n) {
    if (n < 2 || !islower((unsigned char)s[0]) || !islower((unsigned char)s[1])) return false;
    size_t i = 2, parts = 0;
    const char *last = NULL;
    size_t last_len = 0;
    while (i < n) {
        if (s[i] != '-') return false;
        size_t st = ++i;
        while (i < n && (islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]))) i++;
        if (i == st) return false;
        last = s + st;
        last_len = i - st;
        parts++;
    }
    if (parts < 2) return false;
    for (size_t k = 0; k < last_len; k++)
        if (!isdigit((unsigned char)last[k])) return false;
    return true;
}

bool kiro_is_region(const char *s) { return s && region_span(s, strlen(s)); }

/* /^arn:aws(?:-us-gov)?:codewhisperer:(REGION):\d+:profile\/[A-Za-z0-9_-]+$/ */
char *kiro_profile_region(const char *arn) {
    if (!arn) return NULL;
    const char *p;
    if (str_starts(arn, "arn:aws:")) p = arn + 8;
    else if (str_starts(arn, "arn:aws-us-gov:")) p = arn + 15;
    else return NULL;
    if (!str_starts(p, "codewhisperer:")) return NULL;
    p += 14;
    const char *colon = strchr(p, ':');
    if (!colon || !region_span(p, (size_t)(colon - p))) return NULL;
    const char *q = colon + 1;
    const char *d = q;
    while (isdigit((unsigned char)*q)) q++;
    if (q == d || *q != ':') return NULL;
    q++;
    if (!str_starts(q, "profile/")) return NULL;
    q += 8;
    if (!*q) return NULL;
    for (const char *c = q; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') return NULL;
    return xstrndup(p, (size_t)(colon - p));
}

char *kiro_runtime_base_url(const char *region) { return xasprintf("https://runtime.%s.kiro.dev/", region); }
char *kiro_management_base_url(const char *region) { return xasprintf("https://management.%s.kiro.dev/", region); }

void kiro_cred_free(kiro_cred_t *c) {
    if (!c) return;
    free(c->token);
    free(c->region);
    free(c->login_id);
    free(c->profile_arn);
    memset(c, 0, sizeof *c);
}

char *kiro_encode_credential(const kiro_cred_t *c) {
    json_t *j = json_obj();
    json_set_str(j, "kind", c->oauth ? "oauth" : "api_key");
    json_set_str(j, "token", c->token ? c->token : "");
    if (c->region) json_set_str(j, "region", c->region);
    if (c->oauth) {
        json_set_str(j, "loginId", c->login_id ? c->login_id : "");
        if (c->profile_arn && *c->profile_arn) json_set_str(j, "profileArn", c->profile_arn);
    }
    char *s = json_dump_js(j);
    json_free(j);
    return s;
}

char *kiro_encode_oauth_row(const json_t *row, char **err) {
    const char *region = json_get_str(row, "region"), *acc = json_get_str(row, "accountId");
    if (!kiro_is_region(region) || !acc || !*acc) {
        *err = xstrdup("Kiro login is incomplete; run /login kiro again");
        return NULL;
    }
    const char *arn = json_get_str(row, "kiroProfileArn");
    kiro_cred_t c = {.oauth = true,
                     .token = (char *)(json_get_str(row, "access") ? json_get_str(row, "access") : ""),
                     .region = (char *)region,
                     .login_id = (char *)acc,
                     .profile_arn = (char *)(arn && *arn ? arn : NULL)};
    return kiro_encode_credential(&c);
}

static int cred_err(char **err, const char *msg) {
    *err = xstrdup(msg);
    return -1;
}

static int decode_encoded(const char *value, kiro_cred_t *out, char **err) {
    json_t *j = json_parse_str(value);
    if (!j) return cred_err(err, "Kiro credential is not valid JSON");
    int rc = -1;
    const char *token = json_get_str(j, "token"), *kind = json_get_str(j, "kind");
    json_t *region = json_get(j, "region"), *arn = json_get(j, "profileArn"), *login = json_get(j, "loginId");
    if (!json_is_obj(j) || !token || !*token) cred_err(err, "Kiro credential has no token");
    else if (region && !kiro_is_region(json_sv(region))) cred_err(err, "Kiro credential has an invalid region");
    else if (str_eq(kind, "api_key")) {
        if (!str_starts(token, KIRO_API_KEY_PREFIX)) cred_err(err, "Kiro API keys must use the ksk_… format");
        else {
            out->token = xstrdup(token);
            if (json_sv(region)) out->region = xstrdup(json_sv(region));
            rc = 0;
        }
    } else if (!str_eq(kind, "oauth")) cred_err(err, "Kiro credential has an unknown kind");
    else if (!json_sv(region) || !json_sv(login) || !*json_sv(login))
        cred_err(err, "Kiro login is incomplete; run /login kiro again");
    else {
        char *ar = arn ? kiro_profile_region(json_sv(arn)) : NULL;
        if (arn && !ar) cred_err(err, "Kiro credential has an invalid profile ARN");
        else {
            out->oauth = true;
            out->token = xstrdup(token);
            out->region = xstrdup(json_sv(region));
            out->login_id = xstrdup(json_sv(login));
            if (json_sv(arn) && *json_sv(arn)) out->profile_arn = xstrdup(json_sv(arn));
            rc = 0;
        }
        free(ar);
    }
    json_free(j);
    return rc;
}

int kiro_decode_credential(const char *value, kiro_cred_t *out, char **err) {
    memset(out, 0, sizeof *out);
    *err = NULL;
    char *t = str_trim(value ? value : "");
    int rc;
    if (!*t) rc = cred_err(err, "Kiro credential is missing");
    else if (t[0] != '{' && !str_starts(t, KIRO_API_KEY_PREFIX)) rc = cred_err(err, "Kiro API keys must use the ksk_… format");
    else if (t[0] == '{') rc = decode_encoded(t, out, err);
    else {
        out->token = xstrdup(t);
        rc = 0;
    }
    free(t);
    if (rc == 0 && !out->oauth && !out->region) {
        const char *pin = getenv("KIRO_API_REGION");
        if (pin) {
            if (!kiro_is_region(pin)) {
                kiro_cred_free(out);
                return cred_err(err, "KIRO_API_REGION is not a valid AWS region");
            }
            out->region = xstrdup(pin);
        }
    }
    if (rc != 0) kiro_cred_free(out);
    return rc;
}

static void base36(sb_t *b, uint64_t v) {
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
        v /= 36;
    } while (v);
    while (n) sb_putc(b, tmp[--n]);
}

/* kiroCredentialCacheScope (login id + route, or a hash of key + region), hashed into the kiro:models-v2 cache id */
char *kiro_credential_cache_key(const kiro_cred_t *c) {
    sb_t s = SB_INIT;
    if (c->oauth) {
        sb_appendf(&s, "oauth:%s", c->login_id ? c->login_id : "");
        sb_putc(&s, '\0');
        sb_append(&s, c->profile_arn ? c->profile_arn : "");
        sb_putc(&s, '\0');
        sb_append(&s, c->region ? c->region : "");
    } else {
        sb_t k = SB_INIT;
        sb_append(&k, c->token ? c->token : "");
        sb_putc(&k, '\0');
        sb_append(&k, c->region ? c->region : "");
        sb_append(&s, "api_key:");
        base36(&s, hash_xx64(k.s, k.len, 0));
        sb_free(&k);
    }
    sb_t out = SB_INIT;
    sb_append(&out, "kiro:models-v2:");
    base36(&out, hash_xx64(s.s, s.len, 0));
    sb_free(&s);
    return sb_detach(&out);
}

/* ======================================================================
 * the Kiro CLI's wire identity (wire/kiro-cli.ts, kiro-cli 3.2.0)
 * ====================================================================== */

#define KIRO_CLI_APP_VERSION "3.2.0"
#define AWS_SDK_RUST_UA "aws-sdk-rust/1.6.0 ua/2.1"
#define RUST_LANG "lang/rust/1.94.1"
#define KIRO_CLI_APP "app/AmazonQ-For-CLI"
/* content-sharing opt-out: the CLI sends false (its setting defaults to on); omp opts out */
#define KIRO_CODEWHISPERER_OPTOUT "true"
#if defined(__APPLE__)
#define OS_FAMILY "macos"
#elif defined(__linux__)
#define OS_FAMILY "linux"
#else
#define OS_FAMILY "other"
#endif

typedef struct {
    const char *op, *target, *api, *features;
} kiro_op_t;

static const kiro_op_t OPERATIONS[] = {
    {"GenerateAssistantResponse", "AmazonCodeWhispererStreamingService.GenerateAssistantResponse",
     "api/codewhispererstreaming/0.1.19636", "F"},
    {"ListAvailableModels", "KiroControlPlaneBearerService.ListAvailableModels", "api/kirocontrolplanebearer/0.1.0", "F,C"},
    {"ListAvailableProfiles", "AmazonCodeWhispererService.ListAvailableProfiles", "api/codewhispererruntime/0.1.19636", "F"},
    {"GetUsageLimits", "AmazonCodeWhispererService.GetUsageLimits", "api/codewhispererruntime/0.1.19636", "F"},
};

static void user_agents(http_headers_t *h, const char *api, const char *features, bool app_version) {
    char *base = xasprintf("%s %s os/" OS_FAMILY " " RUST_LANG, AWS_SDK_RUST_UA, api);
    char *ua = app_version ? xasprintf("%s md/appVersion-" KIRO_CLI_APP_VERSION " " KIRO_CLI_APP, base)
                           : xasprintf("%s m/%s " KIRO_CLI_APP, base, features);
    char *xua = xasprintf("%s m/%s " KIRO_CLI_APP, base, features);
    http_headers_set(h, "user-agent", ua);
    http_headers_set(h, "x-amz-user-agent", xua);
    free(base);
    free(ua);
    free(xua);
}

static void sdk_request(http_headers_t *h, int attempt, int max_attempts) {
    char *a = xasprintf("attempt=%d; max=%d", attempt, max_attempts);
    char *id = uuid_v4();
    http_headers_set(h, "amz-sdk-request", a);
    http_headers_set(h, "amz-sdk-invocation-id", id);
    http_headers_set(h, "accept", "*/*");
    http_headers_set(h, "accept-encoding", "gzip");
    free(a);
    free(id);
}

void kiro_service_headers(http_headers_t *h, const char *operation, const char *token, int attempt, int max_attempts) {
    const kiro_op_t *op = &OPERATIONS[0];
    for (size_t i = 0; i < ARRAY_LEN(OPERATIONS); i++)
        if (str_eq(OPERATIONS[i].op, operation)) op = &OPERATIONS[i];
    http_headers_set(h, "content-type", "application/x-amz-json-1.0");
    http_headers_set(h, "x-amz-target", op->target);
    user_agents(h, op->api, op->features, true);
    http_headers_set(h, "x-amzn-codewhisperer-optout", KIRO_CODEWHISPERER_OPTOUT);
    if (op == &OPERATIONS[0]) {
        char *ka = xasprintf("%d;max=%d", attempt, max_attempts);
        http_headers_set(h, "x-amzn-kiro-attempt", ka);
        free(ka);
        http_headers_set(h, "x-amzn-kiro-fallback", "0;max=0"); /* omp never falls back to another model */
    }
    char *auth = xasprintf("Bearer %s", token ? token : "");
    http_headers_set(h, "authorization", auth);
    free(auth);
    /* a ksk_ key without tokentype is read as an OAuth bearer and rejected for lacking a profile ARN (#19689) */
    if (str_starts(token ? token : "", KIRO_API_KEY_PREFIX)) http_headers_set(h, "tokentype", "API_KEY");
    sdk_request(h, attempt, max_attempts);
}

void kiro_oidc_headers(http_headers_t *h, int attempt, int max_attempts) {
    http_headers_set(h, "content-type", "application/json");
    user_agents(h, "api/ssooidc/1.100.0", "E,N", false);
    sdk_request(h, attempt, max_attempts);
}

void kiro_social_headers(http_headers_t *h) {
    http_headers_set(h, "content-type", "application/json");
    http_headers_set(h, "user-agent", "Kiro-CLI");
    http_headers_set(h, "accept", "*/*");
    http_headers_set(h, "accept-encoding", "gzip");
}

/* ======================================================================
 * management client (discovery/kiro.ts)
 * ====================================================================== */

#define MGMT_TIMEOUT_MS 10000
#define MGMT_MAX_BYTES (1024 * 1024)

/* test hook: OMC_PROVIDER_URL_REWRITE, else OMC_AUTH_URL_REWRITE (the management host serves both discovery and usage) */
static char *rewrite(const char *url) {
    char *u = ai_rewrite_url(url);
    if (strcmp(u, url) || !env_get("OMC_AUTH_URL_REWRITE")) return u;
    free(u);
    strvec_t parts = str_split(env_get("OMC_AUTH_URL_REWRITE"), ",");
    char *out = NULL;
    for (size_t i = 0; i < parts.n && !out; i++) {
        char *eq = strchr(parts.v[i], '=');
        if (!eq) continue;
        *eq = 0;
        char *from = str_trim(parts.v[i]), *to = str_trim(eq + 1);
        if (*from && str_starts(url, from)) out = xasprintf("%s%s", to, url + strlen(from));
        free(from);
        free(to);
    }
    strvec_free(&parts);
    return out ? out : xstrdup(url);
}

json_t *kiro_management_request(const char *region, const char *token, const char *target, const json_t *body,
                                volatile sig_atomic_t *cancel, int *status, char **err) {
    *status = 0;
    *err = NULL;
    char *base = kiro_management_base_url(region);
    char *url = rewrite(base);
    free(base);
    http_headers_t h = {0};
    kiro_service_headers(&h, target, token, 1, 1);
    char *b = json_dump_js(body);
    http_req_t req = {.method = "POST",
                      .url = url,
                      .headers = &h,
                      .body = b,
                      .body_len = strlen(b),
                      .timeout_ms = MGMT_TIMEOUT_MS,
                      .max_body = MGMT_MAX_BYTES,
                      .cancel = cancel};
    http_resp_t r;
    int rc = http_request(&req, &r);
    json_t *out = NULL;
    if (rc != 0 || r.error) *err = xstrdup(r.error ? r.error : "request failed");
    else if (r.status < 200 || r.status >= 300) {
        *status = r.status;
        *err = xasprintf("%s: HTTP %d", target, r.status);
    } else if (r.truncated) *err = xasprintf("%s: response exceeds %d bytes", target, MGMT_MAX_BYTES);
    else if (!(out = r.body ? json_parse(r.body, r.body_len, NULL) : NULL)) *err = xasprintf("%s: invalid JSON", target);
    if (!out) *status = *status ? *status : (rc == 0 ? r.status : 0);
    http_resp_free(&r);
    http_headers_free(&h);
    free(b);
    free(url);
    return out;
}

/* listPages: nextToken-paged listing; a missing or repeated token ends it. `first` (owned) reuses a fetched page.
 * Calls each(page) per page; NULL + *err on failure. */
static int list_pages(const char *region, const char *token, const char *target, const json_t *body, json_t *first,
                      volatile sig_atomic_t *cancel, void (*each)(void *ud, const json_t *page), void *ud, char **err) {
    strvec_t seen = {0};
    json_t *page = first;
    char *next = NULL;
    int rc = 0;
    for (;;) {
        if (!page) {
            json_t *b = json_clone(body);
            if (next) json_set_str(b, "nextToken", next);
            int st;
            page = kiro_management_request(region, token, target, b, cancel, &st, err);
            json_free(b);
            if (!page) {
                rc = -1;
                break;
            }
        }
        if (!json_is_obj(page)) {
            *err = xasprintf("%s: invalid response", target);
            rc = -1;
            break;
        }
        each(ud, page);
        const char *t = json_get_str(page, "nextToken");
        if (!t || !*t || strvec_contains(&seen, t)) break;
        strvec_push(&seen, t);
        free(next);
        next = xstrdup(t);
        json_free(page);
        page = NULL;
    }
    json_free(page);
    free(next);
    strvec_free(&seen);
    return rc;
}

static void collect_profiles(void *ud, const json_t *page) {
    json_t *out = ud, *e;
    json_arr_foreach(json_get_arr(page, "profiles"), i, e) {
        char *r = kiro_profile_region(json_get_str(e, "arn"));
        if (!json_is_obj(e) || !r) {
            free(r);
            continue;
        }
        free(r);
        json_t *p = json_obj();
        json_set_str(p, "arn", json_get_str(e, "arn"));
        const char *name = json_get_str(e, "profileName");
        if (name && *name) json_set_str(p, "name", name);
        json_push(out, p);
    }
}

json_t *kiro_list_profiles(const char *region, const char *token, volatile sig_atomic_t *cancel, char **err) {
    json_t *out = json_arr(), *body = json_obj();
    int rc = list_pages(region, token, "ListAvailableProfiles", body, NULL, cancel, collect_profiles, out, err);
    json_free(body);
    if (rc != 0) {
        json_free(out);
        return NULL;
    }
    return out;
}

/* probeApiKeyRegion: the single bootstrap region whose ListAvailableModels succeeds, with its first page */
static char *probe_api_key_region(const char *token, volatile sig_atomic_t *cancel, json_t **first) {
    json_t *body = json_obj();
    json_set_str(body, "origin", "KIRO_CLI");
    char *hit = NULL;
    json_t *hit_page = NULL;
    int hits = 0;
    for (size_t i = 0; i < 2; i++) {
        int st;
        char *e = NULL;
        json_t *page = kiro_management_request(KIRO_BOOTSTRAP_REGIONS[i], token, "ListAvailableModels", body, cancel, &st, &e);
        free(e);
        if (!page) continue;
        hits++;
        if (!hit) {
            hit = xstrdup(KIRO_BOOTSTRAP_REGIONS[i]);
            hit_page = page;
        } else json_free(page);
    }
    json_free(body);
    if (hits != 1 || (cancel && *cancel)) {
        free(hit);
        json_free(hit_page);
        return NULL;
    }
    if (first) *first = hit_page;
    else json_free(hit_page);
    return hit;
}

char *kiro_resolve_api_key_region(const char *token, volatile sig_atomic_t *cancel) {
    return probe_api_key_region(token, cancel, NULL);
}

static bool positive_int(const json_t *v) {
    return json_is_num(v) && v->u.num.d > 0 && v->u.num.d == floor(v->u.num.d) && v->u.num.d <= 9007199254740991.0;
}

/* schema.properties[k0].properties[k1]... */
static const json_t *schema_at(const json_t *schema, const char *k0, const char *k1) {
    const json_t *node = schema;
    const char *keys[2] = {k0, k1};
    for (int i = 0; i < 2 && keys[i]; i++) node = json_is_obj(node) ? json_get(json_get_obj(node, "properties"), keys[i]) : NULL;
    return json_is_obj(node) ? node : NULL;
}

static bool enum_has(const json_t *schema, const char *v) {
    json_t *e;
    json_arr_foreach(json_get_arr(schema, "enum"), i, e) if (str_eq(json_sv(e), v)) return true;
    return false;
}

/* mapThinking: the thinking controls `additionalModelRequestFieldsSchema` advertises, or NULL for an unknown shape.
 * Kiro applies the schema's default effort when the fields are omitted, so a model with an advertised off value must
 * send it (suppressWhenOff); one without it cannot turn thinking off (requiresEffort). */
static json_t *map_thinking(const json_t *schema, int64_t *max_tokens) {
    static const char *const anthropic[] = {"low", "medium", "high", "xhigh", "max"};
    static const char *const gpt[][2] = {{"none", "minimal"}, {"low", "low"},     {"medium", "medium"},
                                         {"high", "high"},    {"xhigh", "xhigh"}, {"max", "max"}};
    *max_tokens = 0;
    const json_t *aeff = schema_at(schema, "output_config", "effort");
    const json_t *ttype = schema_at(schema, "thinking", "type");
    if (enum_has(ttype, "adaptive") && aeff) {
        json_t *efforts = json_arr();
        const char *def = NULL;
        for (size_t i = 0; i < ARRAY_LEN(anthropic); i++)
            if (enum_has(aeff, anthropic[i])) {
                json_push_str(efforts, anthropic[i]);
                if (str_eq(json_get_str(aeff, "default"), anthropic[i])) def = anthropic[i];
            }
        if (json_len(efforts) == 0) {
            json_free(efforts);
            return NULL;
        }
        json_t *t = json_obj();
        json_set_str(t, "mode", "anthropic-adaptive");
        json_t *map = json_obj();
        json_set_str(map, "minimal", json_sv(json_at(efforts, 0)));
        json_set(t, "efforts", efforts);
        if (def) json_set_str(t, "defaultLevel", def);
        json_set(t, "effortMap", map);
        json_set_bool(t, "supportsDisplay", true);
        json_set_bool(t, enum_has(ttype, "disabled") ? "suppressWhenOff" : "requiresEffort", true);
        const json_t *mt = schema_at(schema, "max_tokens", NULL);
        json_t *mx = mt ? json_get(mt, "maximum") : NULL;
        if (positive_int(mx)) *max_tokens = (int64_t)mx->u.num.d;
        return t;
    }
    const json_t *geff = schema_at(schema, "reasoning", "effort");
    if (!geff) return NULL;
    json_t *efforts = json_arr();
    const char *def = NULL;
    for (size_t i = 0; i < ARRAY_LEN(gpt); i++)
        if (enum_has(geff, gpt[i][0])) {
            json_push_str(efforts, gpt[i][1]);
            if (str_eq(json_get_str(geff, "default"), gpt[i][0])) def = gpt[i][1];
        }
    if (json_len(efforts) == 0) {
        json_free(efforts);
        return NULL;
    }
    json_t *t = json_obj();
    json_set_str(t, "mode", "effort");
    json_set(t, "efforts", efforts);
    if (def) json_set_str(t, "defaultLevel", def);
    if (enum_has(geff, "none")) {
        json_t *map = json_obj();
        json_set_str(map, "minimal", "none");
        json_set(t, "effortMap", map);
        json_set_bool(t, "suppressWhenOff", true);
    } else json_set_bool(t, "requiresEffort", true);
    return t;
}

json_t *kiro_map_model(const json_t *raw, const char *base_url) {
    const char *id = json_get_str(raw, "modelId");
    json_t *limits = json_get_obj(raw, "tokenLimits");
    if (!json_is_obj(raw) || !id || !*id || !limits || !positive_int(json_get(limits, "maxInputTokens")) ||
        !positive_int(json_get(limits, "maxOutputTokens"))) {
        LOGW("Kiro model skipped: missing id or token limits (%s)", id ? id : "?");
        return NULL;
    }
    json_t *schema = json_get(raw, "additionalModelRequestFieldsSchema");
    int64_t max_tokens = 0;
    json_t *thinking = map_thinking(schema, &max_tokens);
    if (!thinking && schema) LOGW("Kiro model request schema unrecognised; listing without thinking controls (%s)", id);
    json_t *spec = json_obj();
    json_set_str(spec, "id", id);
    const char *name = json_get_str(raw, "modelName");
    json_set_str(spec, "name", name && *name ? name : id);
    json_set_str(spec, "api", "kiro-api");
    json_set_str(spec, "provider", "kiro");
    json_set_str(spec, "baseUrl", base_url);
    json_set_bool(spec, "reasoning", thinking != NULL);
    if (thinking) json_set(spec, "thinking", thinking);
    json_t *input = json_arr();
    json_push_str(input, "text");
    json_t *it;
    json_arr_foreach(json_get_arr(raw, "supportedInputTypes"), i, it) if (str_eq(json_sv(it), "IMAGE")) {
        json_push_str(input, "image");
        break;
    }
    json_set(spec, "input", input);
    json_t *cost = json_obj();
    json_set_int(cost, "input", 0);
    json_set_int(cost, "output", 0);
    json_set_int(cost, "cacheRead", 0);
    json_set_int(cost, "cacheWrite", 0);
    json_set(spec, "cost", cost);
    json_t *rm = json_get(raw, "rateMultiplier");
    if (str_eq(json_get_str(raw, "rateUnit"), "Credit") && json_is_num(rm) && rm->u.num.d >= 0 && isfinite(rm->u.num.d))
        json_set_num(spec, "premiumMultiplier", rm->u.num.d);
    json_set_int(spec, "contextWindow", (int64_t)json_get_num(limits, "maxInputTokens", 0));
    json_set_int(spec, "maxTokens", max_tokens > 0 ? max_tokens : (int64_t)json_get_num(limits, "maxOutputTokens", 0));
    return spec;
}

typedef struct {
    json_t *models;
    const char *base_url;
} collect_models_t;

static void collect_models(void *ud, const json_t *page) {
    collect_models_t *c = ud;
    json_t *raw;
    json_arr_foreach(json_get_arr(page, "models"), i, raw) {
        json_t *m = kiro_map_model(raw, c->base_url);
        if (!m) continue;
        bool dup = false;
        json_t *x;
        json_arr_foreach(c->models, k, x) if (str_eq(json_get_str(x, "id"), json_get_str(m, "id"))) dup = true;
        if (dup) json_free(m);
        else json_push(c->models, m);
    }
}

json_t *kiro_fetch_models(const kiro_cred_t *c, volatile sig_atomic_t *cancel) {
    char *region = c->region ? xstrdup(c->region) : NULL;
    json_t *first = NULL;
    if (!region) {
        region = probe_api_key_region(c->token, cancel, &first);
        if (!region) {
            LOGD("Kiro model discovery skipped: no single bootstrap region accepts the API key");
            return NULL;
        }
    }
    char *base = kiro_runtime_base_url(region);
    json_t *body = json_obj();
    json_set_str(body, "origin", "KIRO_CLI");
    if (c->oauth && c->profile_arn) json_set_str(body, "profileArn", c->profile_arn);
    collect_models_t cm = {json_arr(), base};
    char *err = NULL;
    int rc = list_pages(region, c->token, "ListAvailableModels", body, first, cancel, collect_models, &cm, &err);
    json_free(body);
    if (rc != 0) {
        if (cancel && *cancel) LOGD("Kiro model discovery aborted: %s", err ? err : "?");
        else LOGW("Kiro model discovery failed: %s", err ? err : "?");
        json_free(cm.models);
        cm.models = NULL;
    } else if (json_len(cm.models) == 0) {
        LOGW("Kiro model discovery returned no usable models");
        json_free(cm.models);
        cm.models = NULL;
    }
    free(err);
    free(base);
    free(region);
    return cm.models;
}

/* ---------- per-credential discovery + cache (kiroModelManagerOptions, model-manager.ts 2 h TTL) ---------- */

#define KIRO_CACHE_TTL_MS (2LL * 60 * 60 * 1000)

static char *cache_path(void) { return omc_agent_path("kiro-models.json"); }

static json_t *cache_load(void) {
    char *path = cache_path();
    size_t n;
    char *s = fs_read(path, &n);
    free(path);
    json_t *j = s ? json_parse(s, n, NULL) : NULL;
    free(s);
    if (!json_is_obj(j)) {
        json_free(j);
        j = json_obj();
    }
    return j;
}

static void cache_save(const json_t *c) {
    char *path = cache_path();
    char *dir = path_dirname(path);
    fs_mkdir_p(dir, 0700);
    char *s = json_dump(c, 0);
    fs_write_atomic(path, s, strlen(s), 0600);
    free(s);
    free(dir);
    free(path);
}

/* the roster replaces the earlier discovered kiro models (dynamicModelsAuthoritative): unlisted ones are gone */
static int register_models(const json_t *models) {
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (!str_eq(m->provider, "kiro") || !json_get_bool(m->raw, "discovered", false)) continue;
        bool keep = false;
        json_t *x;
        json_arr_foreach(models, k, x) if (str_eq(json_get_str(x, "id"), m->id)) keep = true;
        if (!keep) json_set_bool(m->raw, "gone", true);
    }
    int n = 0;
    json_t *x;
    json_arr_foreach(models, k, x) {
        json_t *spec = json_clone(x);
        json_set_bool(spec, "discovered", true);
        if (catalog_register(spec)) n++;
    }
    return n;
}

int kiro_discover(bool force) {
    char *key = auth_get_api_key ? auth_get_api_key("kiro") : ai_env_api_key("kiro");
    if (!key) return 0;
    kiro_cred_t c;
    char *err = NULL;
    int rc = kiro_decode_credential(key, &c, &err);
    free(key);
    if (rc != 0) {
        LOGW("Kiro model discovery skipped: %s", err ? err : "?");
        free(err);
        return 0;
    }
    char *ck = kiro_credential_cache_key(&c);
    json_t *cache = cache_load();
    json_t *entry = json_get_obj(cache, ck);
    int64_t fetched = (int64_t)json_get_num(entry, "fetchedAt", 0);
    int n = 0;
    if (!force && entry && now_ms() - fetched < KIRO_CACHE_TTL_MS) {
        n = register_models(json_get_arr(entry, "models"));
    } else {
        json_t *models = kiro_fetch_models(&c, NULL);
        if (models) {
            json_t *e = json_obj();
            json_set_int(e, "fetchedAt", now_ms());
            json_set(e, "models", models);
            json_set(cache, ck, e);
            cache_save(cache);
            n = register_models(models);
        } else if (entry) {
            n = register_models(json_get_arr(entry, "models")); /* keep the last cache */
        }
    }
    json_free(cache);
    free(ck);
    kiro_cred_free(&c);
    return n;
}

/* ======================================================================
 * request transform (providers/kiro/transform.ts)
 * ====================================================================== */

/* per-result UTF-8 cap, marker included: the Kiro service's hard limit (kiro-cli chat/consts.rs) */
#define TOOL_RESULT_MAX_BYTES 800000
/* what a tool result shrinks to when the request is over budget (kiro-cli's max_message_length fallback) */
#define TOOL_RESULT_SHRUNK_BYTES 25000
/* serialized history plus current user input allowed per BUDGET_BASE_TOKENS of context window */
#define BUDGET_BASE_BYTES 850000
#define BUDGET_BASE_TOKENS 200000
#define CURRENT_TOOL_RESULT_IMAGE_PLACEHOLDER "(see attached image)"
#define HISTORICAL_IMAGE_PLACEHOLDER "(image omitted from history)"

static size_t json_bytes(const json_t *v) {
    sb_t b = SB_INIT;
    json_dump_js_sb(&b, v);
    size_t n = b.len;
    sb_free(&b);
    return n;
}

static size_t str_json_bytes(const char *s) {
    sb_t b = SB_INIT;
    json_js_quote_sb(&b, s, strlen(s));
    size_t n = b.len;
    sb_free(&b);
    return n;
}

static char *render_prompt(const char *name, json_t *ctx) {
    char *rel = xasprintf(PROMPT_DIR "%s", name);
    char *s = prompt_render(rel, ctx);
    free(rel);
    json_free(ctx);
    return s ? s : xstrdup("");
}

static char *join_prompt_content(const char *prefix, const char *content) {
    json_t *ctx = json_obj();
    json_set_str(ctx, "prefix", prefix);
    json_set_str(ctx, "content", content);
    return render_prompt("joined-content.md", ctx);
}

static char *truncated_marker(int64_t kept, int64_t total) {
    json_t *ctx = json_obj();
    json_set_int(ctx, "keptBytes", kept);
    json_set_int(ctx, "totalBytes", total);
    return render_prompt("tool-result-truncated.md", ctx);
}

/* cut text of total UTF-8 bytes to max, ending with a marker that tells the model so */
static char *truncate_tool_result(const char *text, size_t total, size_t max) {
    if (total <= max) return xstrdup(text);
    char *wide = truncated_marker((int64_t)max, (int64_t)total);
    size_t budget = max > strlen(wide) + 2 ? max - strlen(wide) - 2 : 0;
    free(wide);
    size_t cut = MIN(budget, total);
    while (cut > 0 && cut < total && ((unsigned char)text[cut] & 0xC0) == 0x80) cut--; /* UTF-8 safe */
    char *marker = truncated_marker((int64_t)cut, (int64_t)total);
    char *out = xasprintf("%.*s\n\n%s", (int)cut, text, marker);
    free(marker);
    return out;
}

static char *text_of(const json_t *msg) {
    json_t *c = json_get(msg, "content");
    if (json_is_str(c)) return xstrdup(json_sv(c));
    sb_t b = SB_INIT;
    json_t *p;
    json_arr_foreach(c, i, p) if (str_eq(json_get_str(p, "type"), "text") && json_get_str(p, "text")) sb_append(&b, json_get_str(p, "text"));
    return sb_detach(&b);
}

static bool has_images(const json_t *msg) {
    json_t *p;
    json_arr_foreach(json_get_arr(msg, "content"), i, p) if (str_eq(json_get_str(p, "type"), "image")) return true;
    return false;
}

static char *normalize_id(const char *id) {
    if (!id) id = "";
    size_t n = MIN(strlen(id), (size_t)64);
    char *o = xmalloc(n + 1);
    for (size_t i = 0; i < n; i++) o[i] = (isalnum((unsigned char)id[i]) || id[i] == '_' || id[i] == '-') ? id[i] : '_';
    o[n] = 0;
    return o;
}

typedef struct {
    bool user;
    VEC(const json_t *) msgs;    /* user/developer messages (borrowed) */
    VEC(const json_t *) results; /* tool results (borrowed) */
    sb_t text;                   /* assistant text */
    json_t *tool_uses;           /* assistant toolUses array (owned) */
} turn_t;

typedef VEC(turn_t *) turns_t;

typedef struct {
    json_t *part; /* {text} inside the request (borrowed) */
    char *full;
    size_t full_bytes;
} budgeted_t;

typedef VEC(budgeted_t) budget_vec_t;

static turn_t *user_turn(turns_t *t) {
    if (t->n && t->v[t->n - 1]->user) return t->v[t->n - 1];
    turn_t *u = xcalloc(1, sizeof *u);
    u->user = true;
    vec_push(t, u);
    return u;
}

static void turns_free(turns_t *t) {
    for (size_t i = 0; i < t->n; i++) {
        vec_free(&t->v[i]->msgs);
        vec_free(&t->v[i]->results);
        sb_free(&t->v[i]->text);
        json_free(t->v[i]->tool_uses);
        free(t->v[i]);
    }
    vec_free(t);
}

/* toTurns: strictly alternating user/assistant turns, opening and closing on a user turn */
static void to_turns(const json_t *msgs, turns_t *t) {
    const json_t *m;
    json_arr_foreach(msgs, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "toolResult")) vec_push(&user_turn(t)->results, m);
        else if (!str_eq(role, "assistant")) vec_push(&user_turn(t)->msgs, m);
        else {
            sb_t text = SB_INIT;
            json_t *uses = json_arr(), *p;
            json_arr_foreach(json_get_arr(m, "content"), k, p) {
                const char *ty = json_get_str(p, "type");
                if (str_eq(ty, "text") && json_get_str(p, "text")) sb_append(&text, json_get_str(p, "text"));
                else if (str_eq(ty, "toolCall")) {
                    json_t *u = json_obj();
                    json_set_str(u, "name", json_get_str(p, "name") ? json_get_str(p, "name") : "");
                    char *id = normalize_id(json_get_str(p, "id"));
                    json_set_str(u, "toolUseId", id);
                    free(id);
                    json_t *a = json_get(p, "arguments");
                    json_set(u, "input", json_is_obj(a) ? json_clone(a) : json_obj());
                    json_push(uses, u);
                }
            }
            if (!text.len && json_len(uses) == 0) {
                sb_free(&text);
                json_free(uses);
                continue;
            }
            turn_t *last = t->n ? t->v[t->n - 1] : NULL;
            if (last && !last->user) {
                sb_appendn(&last->text, text.s, text.len);
                json_t *u;
                while (json_len(uses)) {
                    u = json_arr_remove(uses, 0);
                    json_push(last->tool_uses, u);
                }
                sb_free(&text);
                json_free(uses);
            } else {
                if (!last) user_turn(t);
                turn_t *a = xcalloc(1, sizeof *a);
                a->text = text;
                a->tool_uses = uses;
                vec_push(t, a);
            }
        }
    }
    user_turn(t);
}

static json_t *encode_results(const turn_t *turn, const char *placeholder, budget_vec_t *budget) {
    json_t *out = json_arr();
    for (size_t i = 0; i < turn->results.n; i++) {
        const json_t *msg = turn->results.v[i];
        char *full = text_of(msg);
        if (!*full && has_images(msg)) {
            free(full);
            full = xstrdup(placeholder);
        }
        size_t fb = strlen(full);
        json_t *part = json_obj();
        char *t = truncate_tool_result(full, fb, TOOL_RESULT_MAX_BYTES);
        json_set_str(part, "text", t);
        free(t);
        budgeted_t bt = {part, full, fb};
        vec_push(budget, bt);
        json_t *r = json_obj(), *content = json_arr();
        json_push(content, part);
        json_set(r, "content", content);
        json_set_str(r, "status", json_get_bool(msg, "isError", false) ? "error" : "success");
        char *id = normalize_id(json_get_str(msg, "toolCallId"));
        json_set_str(r, "toolUseId", id);
        free(id);
        json_push(out, r);
    }
    return out;
}

static char *user_content(const turn_t *turn, size_t index, const char *placeholder, const ai_context_t *ctx) {
    char *content = xstrdup("");
    bool images = false;
    for (size_t i = 0; i < turn->msgs.n; i++) {
        char *text = text_of(turn->msgs.v[i]);
        if (has_images(turn->msgs.v[i])) images = true;
        if (*text) {
            char *n = *content ? join_prompt_content(content, text) : xstrdup(text);
            free(content);
            content = n;
        }
        free(text);
    }
    if (!*content && images) {
        free(content);
        content = xstrdup(placeholder);
    }
    /* the first user turn carries the system prompt */
    if (index == 0 && json_len(ctx->system_prompt) > 0) {
        sb_t sp = SB_INIT;
        json_t *s;
        json_arr_foreach(ctx->system_prompt, i, s) {
            if (i) sb_putc(&sp, '\n');
            if (json_sv(s)) sb_append(&sp, json_sv(s));
        }
        char *n = join_prompt_content(sb_cstr(&sp), content);
        sb_free(&sp);
        free(content);
        content = n;
    }
    char *trimmed = str_trim(content);
    bool keep = *trimmed || turn->results.n > 0;
    free(trimmed);
    if (keep) return content;
    free(content);
    return render_prompt("continuation.md", NULL);
}

static size_t shrink_tool_results(budget_vec_t *results, size_t total, size_t limit) {
    for (size_t i = 0; i < results->n; i++) {
        if (total <= limit) break;
        budgeted_t *r = &results->v[i];
        if (r->full_bytes <= TOOL_RESULT_SHRUNK_BYTES) continue;
        size_t before = str_json_bytes(json_get_str(r->part, "text"));
        char *t = truncate_tool_result(r->full, r->full_bytes, TOOL_RESULT_SHRUNK_BYTES);
        json_set_str(r->part, "text", t);
        free(t);
        total = total + str_json_bytes(json_get_str(r->part, "text")) - before;
    }
    return total;
}

static const char *image_format(const char *mime) {
    char *l = str_lower(mime ? mime : "");
    const char *f = NULL;
    if (!strcmp(l, "image/jpeg") || !strcmp(l, "image/jpg")) f = "jpeg";
    else if (!strcmp(l, "image/png")) f = "png";
    else if (!strcmp(l, "image/gif")) f = "gif";
    else if (!strcmp(l, "image/webp")) f = "webp";
    free(l);
    return f;
}

static bool efforts_contain(const json_t *arr, const char *e) {
    json_t *it;
    json_arr_foreach(arr, i, it) if (str_eq(json_sv(it), e)) return true;
    return false;
}

/* requestFields: no effort means thinking off. Kiro applies the schema default when these fields are absent, so a
 * model that advertises an off value is sent it; discovery marks the others requiresEffort and the caller clamps. */
static json_t *request_fields(const ai_model_t *m, const ai_stream_opts_t *o) {
    const json_t *th = m->thinking;
    json_t *efforts = th ? json_get_arr(th, "efforts") : NULL;
    if (!th || json_len(efforts) == 0) return NULL;
    const char *mode = json_get_str(th, "mode");
    ai_effort_t e = o->disable_reasoning || o->force_reasoning_off ? EFFORT_OFF : model_clamp_effort(m, o->reasoning);
    if (e == EFFORT_OFF) {
        if (!json_get_bool(th, "suppressWhenOff", false)) return NULL;
        json_t *f = json_obj();
        if (str_eq(mode, "anthropic-adaptive")) {
            json_t *t = json_obj();
            json_set_str(t, "type", "disabled");
            json_set(f, "thinking", t);
            return f;
        }
        if (str_eq(mode, "effort")) {
            json_t *r = json_obj();
            json_set_str(r, "mode", "standard");
            json_set_str(r, "effort", "none");
            json_set(f, "reasoning", r);
            return f;
        }
        json_free(f);
        return NULL;
    }
    json_t *map = json_get_obj(th, "effortMap");
    if (str_eq(mode, "anthropic-adaptive")) {
        /* mapEffortToAnthropicAdaptiveEffort: requireSupportedEffort (clamped), effortMap, minimal -> low */
        if (!efforts_contain(efforts, effort_name(e))) {
            const char *al = json_get_str(map, effort_name(e));
            if (al && efforts_contain(efforts, al)) effort_parse(al, &e);
        }
        const char *w = json_get_str(map, effort_name(e));
        if (!w) w = effort_name(e);
        if (!strcmp(w, "minimal")) w = "low";
        json_t *f = json_obj(), *t = json_obj(), *oc = json_obj();
        json_set_str(t, "type", "adaptive");
        json_set_str(t, "display", o->hide_thinking_summary ? "omitted" : "summarized");
        json_set(f, "thinking", t);
        json_set_str(oc, "effort", w);
        json_set(f, "output_config", oc);
        /* adaptive thinking has no separate budget: max_tokens bounds the whole output */
        int64_t limit = o->max_tokens > 0 ? o->max_tokens : 0;
        if (m->max_tokens > 0 && (!limit || m->max_tokens < limit)) limit = m->max_tokens;
        if (limit > 0) json_set_int(f, "max_tokens", limit);
        return f;
    }
    if (str_eq(mode, "effort")) {
        const char *w = json_get_str(map, effort_name(e));
        json_t *f = json_obj(), *r = json_obj();
        json_set_str(r, "mode", "standard");
        json_set_str(r, "effort", w ? w : effort_name(e));
        json_set(f, "reasoning", r);
        return f;
    }
    return NULL;
}

json_t *kiro_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, char **err,
                           bool *overflow) {
    *err = NULL;
    *overflow = false;
    const char *rid = json_get_str(m->raw, "requestModelId");
    const char *model_id = rid && *rid ? rid : m->id;
    json_t *msgs = ai_transform_messages(ctx->messages, m);
    turns_t turns = {0};
    to_turns(msgs, &turns);
    turn_t *current = turns.v[--turns.n];
    budget_vec_t hist_results = {0}, cur_results = {0};
    json_t *history = json_arr();
    for (size_t i = 0; i < turns.n; i++) {
        turn_t *t = turns.v[i];
        json_t *e = json_obj();
        if (!t->user) {
            json_t *a = json_obj();
            json_set_str(a, "content", sb_cstr(&t->text));
            if (json_len(t->tool_uses)) json_set(a, "toolUses", json_clone(t->tool_uses));
            json_set(e, "assistantResponseMessage", a);
        } else {
            json_t *results = encode_results(t, HISTORICAL_IMAGE_PLACEHOLDER, &hist_results);
            json_t *u = json_obj();
            char *c = user_content(t, i, HISTORICAL_IMAGE_PLACEHOLDER, ctx);
            json_set_str(u, "content", c);
            free(c);
            json_set_str(u, "modelId", model_id);
            json_set_str(u, "origin", "KIRO_CLI");
            if (json_len(results)) {
                json_t *uc = json_obj();
                json_set(uc, "toolResults", results);
                json_set(u, "userInputMessageContext", uc);
            } else json_free(results);
            json_set(e, "userInputMessage", u);
        }
        json_push(history, e);
    }
    /* images go only on the current turn; tool-result images ride along with it */
    char *current_content = user_content(current, turns.n, "", ctx);
    json_t *images = json_arr();
    json_t *req = NULL;
    for (int pass = 0; pass < 2 && !*err; pass++) {
        size_t n = pass ? current->results.n : current->msgs.n;
        for (size_t i = 0; i < n && !*err; i++) {
            const json_t *msg = pass ? current->results.v[i] : current->msgs.v[i];
            json_t *p;
            json_arr_foreach(json_get_arr(msg, "content"), k, p) {
                if (!str_eq(json_get_str(p, "type"), "image")) continue;
                const char *fmt = image_format(json_get_str(p, "mimeType"));
                if (!fmt) {
                    *err = xasprintf("Kiro does not support image type: %s", json_get_str(p, "mimeType") ? json_get_str(p, "mimeType") : "");
                    break;
                }
                json_t *ib = json_obj(), *src = json_obj();
                json_set_str(ib, "format", fmt);
                json_set_str(src, "bytes", json_get_str(p, "data") ? json_get_str(p, "data") : "");
                json_set(ib, "source", src);
                json_push(images, ib);
            }
        }
    }
    if (*err) goto done;
    json_t *tool_results = encode_results(current, CURRENT_TOOL_RESULT_IMAGE_PLACEHOLDER, &cur_results);
    json_t *tools = json_arr(), *tool;
    strvec_t declared = {0};
    json_arr_foreach(ctx->tools, i, tool) {
        json_t *spec = json_obj(), *ts = json_obj(), *is = json_obj();
        const char *name = json_get_str(tool, "name") ? json_get_str(tool, "name") : "";
        json_set_str(ts, "name", name);
        json_set_str(ts, "description", json_get_str(tool, "description") ? json_get_str(tool, "description") : "");
        json_t *params = json_get(tool, "parameters");
        json_set(is, "json", json_is_obj(params) ? json_clone(params) : json_obj());
        json_set(ts, "inputSchema", is);
        json_set(spec, "toolSpecification", ts);
        json_push(tools, spec);
        strvec_push(&declared, name);
    }
    /* Kiro rejects history tool uses whose tool is not declared; declare retired ones as placeholders */
    char *hist_desc = NULL;
    for (size_t i = 0; i < turns.n; i++) {
        if (turns.v[i]->user) continue;
        json_t *u;
        json_arr_foreach(turns.v[i]->tool_uses, k, u) {
            const char *name = json_get_str(u, "name");
            if (strvec_contains(&declared, name)) continue;
            strvec_push(&declared, name);
            if (!hist_desc) hist_desc = render_prompt("historical-tool-description.md", NULL);
            json_t *spec = json_obj(), *ts = json_obj(), *is = json_obj(), *schema = json_obj();
            json_set_str(ts, "name", name);
            json_set_str(ts, "description", hist_desc);
            json_set_str(schema, "type", "object");
            json_set(schema, "properties", json_obj());
            json_set(is, "json", schema);
            json_set(ts, "inputSchema", is);
            json_set(spec, "toolSpecification", ts);
            json_push(tools, spec);
        }
    }
    free(hist_desc);
    strvec_free(&declared);

    req = json_obj();
    json_set_str(req, "agentMode", "vibe");
    json_t *cs = json_obj();
    json_set_str(cs, "chatTriggerType", "MANUAL");
    json_set_str(cs, "agentTaskType", "vibe");
    char *conv = o->session_id && *o->session_id ? xstrdup(o->session_id) : uuid_v4();
    json_set_str(cs, "conversationId", conv);
    free(conv);
    json_t *uim = json_obj();
    json_set_str(uim, "content", current_content);
    json_set_str(uim, "modelId", model_id);
    json_set_str(uim, "origin", "KIRO_CLI");
    if (json_len(images)) json_set(uim, "images", json_clone(images));
    if (json_len(tool_results) || json_len(tools)) {
        json_t *uc = json_obj();
        if (json_len(tool_results)) json_set(uc, "toolResults", tool_results);
        else json_free(tool_results);
        if (json_len(tools)) json_set(uc, "tools", tools);
        else json_free(tools);
        json_set(uim, "userInputMessageContext", uc);
    } else {
        json_free(tool_results);
        json_free(tools);
    }
    json_t *cm = json_obj();
    json_set(cm, "userInputMessage", uim);
    json_set(cs, "currentMessage", cm);
    /* budget: serialized history plus the current text, images and tool results; over budget, historical results
     * shrink oldest first, then current ones; what still does not fit is a context overflow */
    int64_t window = m->context_window > 0 ? m->context_window : BUDGET_BASE_TOKENS;
    size_t max_bytes = (size_t)floor((double)BUDGET_BASE_BYTES * ((double)window / BUDGET_BASE_TOKENS));
    size_t bytes = json_bytes(history) + str_json_bytes(current_content) + json_bytes(images);
    for (size_t i = 0; i < cur_results.n; i++) bytes += str_json_bytes(json_get_str(cur_results.v[i].part, "text"));
    bytes = shrink_tool_results(&hist_results, bytes, max_bytes);
    bytes = shrink_tool_results(&cur_results, bytes, max_bytes);
    if (json_len(history)) json_set(cs, "history", history);
    else json_free(history);
    history = NULL;
    json_set(req, "conversationState", cs);
    json_t *fields = request_fields(m, o);
    if (fields) json_set(req, "additionalModelRequestFields", fields);
    if (bytes > max_bytes) {
        *err = xasprintf("Kiro request exceeds local limit of %zu UTF-8 bytes", max_bytes);
        *overflow = true;
        json_free(req);
        req = NULL;
    }
done:
    json_free(history);
    json_free(images);
    free(current_content);
    for (size_t i = 0; i < hist_results.n; i++) free(hist_results.v[i].full);
    for (size_t i = 0; i < cur_results.n; i++) free(cur_results.v[i].full);
    vec_free(&hist_results);
    vec_free(&cur_results);
    vec_free(&current->msgs);
    vec_free(&current->results);
    sb_free(&current->text);
    json_free(current->tool_uses);
    free(current);
    turns_free(&turns);
    json_free(msgs);
    return req;
}

/* ======================================================================
 * stream (providers/kiro/stream.ts)
 * ====================================================================== */

#define MAX_ATTEMPTS 2 /* the first request plus at most one pre-output retry */
#define RETRY_DELAY_MS 500
#define INSUFFICIENT_MODEL_CAPACITY "INSUFFICIENT_MODEL_CAPACITY"
/* body text of Kiro's context-window rejection, an overflow whatever the status (kiro-cli classify_error_kind) */
#define INPUT_TOO_LONG "Input is too long."
#define CONTENT_LENGTH_EXCEEDS_THRESHOLD "CONTENT_LENGTH_EXCEEDS_THRESHOLD"

/* HTTP-equivalent status of in-stream exception shapes (they ride inside an HTTP 200) */
static int stream_exception_status(const char *code) {
    static const struct {
        const char *code;
        int status;
    } MAP[] = {{"accessdeniedexception", 403},     {"internalserverexception", 500}, {"internalservererror", 500},
               {"modeltimeoutexception", 408},     {"serviceunavailableexception", 503},
               {"throttlingerror", 429},           {"throttlingexception", 429}};
    char *l = str_lower(code ? code : "");
    int st = 0;
    for (size_t i = 0; i < ARRAY_LEN(MAP); i++)
        if (!strcmp(l, MAP[i].code)) st = MAP[i].status;
    free(l);
    return st;
}

static const char *nonempty(const json_t *v) { return json_is_str(v) && *json_sv(v) ? json_sv(v) : NULL; }

/* KiroApiError.replayable: a model-capacity throttle, or the fleet's transient "Invalid tool use format." */
static bool replayable(const char *message, int status, const char *code, const char *reason) {
    if (str_eq(reason, INSUFFICIENT_MODEL_CAPACITY)) return true;
    if (status != 400 || !str_eq(code, "ValidationException") || !message) return false;
    size_t n = strlen(message);
    while (n > 0 && (message[n - 1] == '.' || isspace((unsigned char)message[n - 1]))) n--;
    static const char tail[] = "invalid tool use format";
    size_t t = sizeof tail - 1;
    return n >= t && strncasecmp(message + n - t, tail, t) == 0;
}

typedef struct {
    char *id;
    int index;
    sb_t frag;
    json_t *snapshot;
    bool done;
} ktool_t;

typedef struct {
    ai_builder_t b;
    const ai_model_t *m;
    bool visible, metered;
    char *stop_reason;
    json_t *refusal, *token_usage;
    bool has_ctx;
    double ctx_pct;
    int emitted_tools;
    int text_idx, think_idx;
    sb_t text, think;
    VEC(ktool_t) tools;
    aws_es_decoder_t dec;
    char *error;
    int err_status;
    bool err_replayable, err_truncated;
    char *request_id;
} katt_t;

static json_t *kcontent(katt_t *a) { return json_get(a->b.msg, "content"); }

static void kemit(katt_t *a, ai_event_type_t t, int idx, const char *d, size_t n, const json_t *blk) {
    if (!a->b.cb) return;
    ai_event_t ev = {.type = t, .content_index = idx, .delta = d, .delta_len = n, .block = blk, .partial = a->b.msg};
    a->b.cb(a->b.ud, &ev);
}

static void set_err(katt_t *a, char *msg, int status, bool repl) {
    if (a->error) {
        free(msg);
        return;
    }
    a->error = msg;
    a->err_status = status;
    a->err_replayable = repl;
}

static void mark_visible(katt_t *a) {
    if (!a->visible) aib_start(&a->b);
    a->visible = true;
    if (!a->b.t_first_token) a->b.t_first_token = mono_ms();
}

static void close_thinking(katt_t *a) {
    if (a->think_idx < 0) return;
    int i = a->think_idx;
    a->think_idx = -1;
    kemit(a, AI_EV_THINKING_END, i, NULL, 0, json_at(kcontent(a), (size_t)i));
}

static void close_text(katt_t *a) {
    if (a->text_idx < 0) return;
    int i = a->text_idx;
    a->text_idx = -1;
    kemit(a, AI_EV_TEXT_END, i, NULL, 0, json_at(kcontent(a), (size_t)i));
}

static void on_text(katt_t *a, const char *s, size_t n) {
    if (!n) return;
    mark_visible(a);
    close_thinking(a);
    if (a->text_idx < 0) {
        json_t *blk = json_obj();
        json_set_str(blk, "type", "text");
        json_set_str(blk, "text", "");
        json_push(kcontent(a), blk);
        a->text_idx = (int)json_len(kcontent(a)) - 1;
        sb_clear(&a->text);
        kemit(a, AI_EV_TEXT_START, a->text_idx, NULL, 0, NULL);
    }
    sb_appendn(&a->text, s, n);
    json_set(json_at(kcontent(a), (size_t)a->text_idx), "text", json_strn(a->text.s, a->text.len));
    kemit(a, AI_EV_TEXT_DELTA, a->text_idx, s, n, NULL);
}

static void on_thinking(katt_t *a, const char *s, size_t n, const char *sig) {
    if (n) {
        mark_visible(a);
        if (a->think_idx < 0) {
            json_t *blk = json_obj();
            json_set_str(blk, "type", "thinking");
            json_set_str(blk, "thinking", "");
            json_push(kcontent(a), blk);
            a->think_idx = (int)json_len(kcontent(a)) - 1;
            sb_clear(&a->think);
            kemit(a, AI_EV_THINKING_START, a->think_idx, NULL, 0, NULL);
        }
    }
    if (a->think_idx < 0) return;
    json_t *blk = json_at(kcontent(a), (size_t)a->think_idx);
    sb_appendn(&a->think, s, n);
    json_set(blk, "thinking", json_strn(a->think.s, a->think.len));
    if (sig && *sig) json_set_str(blk, "thinkingSignature", sig);
    if (n) kemit(a, AI_EV_THINKING_DELTA, a->think_idx, s, n, NULL);
}

static int finish_tool(katt_t *a, ktool_t *t) {
    json_t *blk = json_at(kcontent(a), (size_t)t->index);
    json_t *args = t->snapshot ? json_clone(t->snapshot) : NULL;
    if (!args) {
        char *trimmed = xstrndup(t->frag.s ? t->frag.s : "", t->frag.len);
        char *tt = str_trim(trimmed);
        args = json_parse_str(*tt ? tt : "{}");
        free(tt);
        free(trimmed);
        if (!json_is_obj(args)) {
            json_free(args);
            set_err(a, xasprintf("Kiro tool %s completed with malformed JSON input", t->id), 0, false);
            return -1;
        }
    }
    json_set(blk, "arguments", args);
    t->done = true;
    a->emitted_tools++;
    kemit(a, AI_EV_TOOLCALL_END, t->index, NULL, 0, blk);
    return 0;
}

/* toolUseEvent: a string input streams argument JSON fragments; an object input is a full snapshot */
static int on_tool(katt_t *a, const json_t *ev) {
    const char *id = nonempty(json_get(ev, "toolUseId"));
    if (!id) {
        set_err(a, xstrdup("Kiro tool event has no toolUseId"), 0, false);
        return -1;
    }
    ktool_t *t = NULL;
    for (size_t i = 0; i < a->tools.n; i++)
        if (!strcmp(a->tools.v[i].id, id)) t = &a->tools.v[i];
    if (t && t->done) {
        set_err(a, xasprintf("Kiro tool %s received data after completion", id), 0, false);
        return -1;
    }
    if (!t) {
        const char *name = nonempty(json_get(ev, "name"));
        if (!name) {
            set_err(a, xasprintf("Kiro tool %s started without a name", id), 0, false);
            return -1;
        }
        mark_visible(a);
        close_thinking(a);
        close_text(a);
        json_t *blk = json_obj();
        json_set_str(blk, "type", "toolCall");
        json_set_str(blk, "id", id);
        json_set_str(blk, "name", name);
        json_set(blk, "arguments", json_obj());
        json_push(kcontent(a), blk);
        ktool_t nt = {.id = xstrdup(id), .index = (int)json_len(kcontent(a)) - 1, .frag = SB_INIT};
        vec_push(&a->tools, nt);
        t = &vec_last(&a->tools);
        kemit(a, AI_EV_TOOLCALL_START, t->index, NULL, 0, NULL);
    }
    json_t *in = json_get(ev, "input");
    if (json_is_str(in) && in->u.str.len) {
        sb_appendn(&t->frag, in->u.str.s, in->u.str.len);
        json_t *partial = json_parse_partial(t->frag.s, t->frag.len);
        if (json_is_obj(partial)) json_set(json_at(kcontent(a), (size_t)t->index), "arguments", partial);
        else json_free(partial);
        kemit(a, AI_EV_TOOLCALL_DELTA, t->index, in->u.str.s, in->u.str.len, NULL);
    } else if (json_is_obj(in)) {
        json_free(t->snapshot);
        t->snapshot = json_clone(in);
    }
    if (json_get_bool(ev, "stop", false) && json_is_bool(json_get(ev, "stop"))) return finish_tool(a, t);
    return 0;
}

/* an exception/error frame: the failure is in its headers, a payload adds the shape's fields */
static void frame_error(katt_t *a, const aws_es_msg_t *f) {
    json_t *p = f->payload_len ? json_parse((const char *)f->payload, f->payload_len, NULL) : NULL;
    if (!json_is_obj(p)) {
        json_free(p);
        p = json_obj();
    }
    const char *code = http_headers_get(&f->headers, ":exception-type");
    if (!code) code = http_headers_get(&f->headers, ":error-code");
    if (!code) code = nonempty(json_get(p, "code"));
    if (!code) code = nonempty(json_get(p, "__type"));
    if (!code) code = "KIRO_EVENTSTREAM_ERROR";
    const char *detail = nonempty(json_get(p, "message"));
    if (!detail) detail = http_headers_get(&f->headers, ":error-message");
    char *msg = detail ? xasprintf("%s: %s", code, detail) : xstrdup(code);
    const char *reason = json_get_str(p, "reason");
    int st = stream_exception_status(code);
    if (!st && str_eq(reason, INSUFFICIENT_MODEL_CAPACITY)) st = 429; /* a capacity reason is a throttle */
    set_err(a, msg, st, st ? replayable(msg, st, code, reason) : false);
    json_free(p);
}

static bool handled_event(const char *t) {
    static const char *const H[] = {"initial-response",    "metadataEvent",     "reasoningContentEvent", "assistantResponseEvent",
                                    "toolUseEvent",        "contextUsageEvent", "meteringEvent"};
    for (size_t i = 0; i < ARRAY_LEN(H); i++)
        if (str_eq(t, H[i])) return true;
    return false;
}

static int on_frame(void *ud, const aws_es_msg_t *f) {
    katt_t *a = ud;
    const char *mt = http_headers_get(&f->headers, ":message-type");
    if (str_eq(mt, "exception") || str_eq(mt, "error")) {
        frame_error(a, f);
        return 2;
    }
    const char *type = http_headers_get(&f->headers, ":event-type");
    if (!str_eq(mt, "event") || !type || !handled_event(type)) return 0;
    json_t *ev = f->payload_len ? json_parse((const char *)f->payload, f->payload_len, NULL) : NULL;
    if (!json_is_obj(ev)) {
        json_free(ev);
        set_err(a, xasprintf("Kiro sent a malformed %s payload", type), 0, false);
        return 2;
    }
    int rc = 0;
    if (!strcmp(type, "initial-response") || !strcmp(type, "metadataEvent")) {
        /* the runtime reports stopReason, classifier details and token usage here */
        const char *rq = nonempty(json_get(ev, "requestId"));
        if (rq) {
            free(a->request_id);
            a->request_id = xstrdup(rq);
        }
        const char *sr = nonempty(json_get(ev, "stopReason"));
        if (sr) {
            free(a->stop_reason);
            a->stop_reason = xstrdup(sr);
        }
        json_t *refusal = json_get_obj(json_get_obj(ev, "stopDetails"), "refusal");
        if (refusal) {
            json_free(a->refusal);
            a->refusal = json_clone(refusal);
        }
        json_t *tu = json_get_obj(ev, "tokenUsage");
        if (tu) {
            json_free(a->token_usage);
            a->token_usage = json_clone(tu);
        }
    } else if (!strcmp(type, "reasoningContentEvent")) {
        const char *t = nonempty(json_get(ev, "text"));
        on_thinking(a, t ? t : "", t ? strlen(t) : 0, nonempty(json_get(ev, "signature")));
    } else if (!strcmp(type, "assistantResponseEvent")) {
        json_t *c = json_get(ev, "content");
        if (!json_is_str(c)) {
            set_err(a, xstrdup("Kiro assistant event has no content"), 0, false);
            rc = 2;
        } else on_text(a, c->u.str.s, c->u.str.len);
    } else if (!strcmp(type, "toolUseEvent")) {
        if (on_tool(a, ev) != 0) rc = 2;
    } else if (!strcmp(type, "contextUsageEvent")) {
        json_t *p = json_get(ev, "contextUsagePercentage");
        if (json_is_num(p) && p->u.num.d >= 0 && p->u.num.d <= 100) {
            a->has_ctx = true;
            a->ctx_pct = p->u.num.d;
        }
    } else if (!strcmp(type, "meteringEvent")) {
        a->metered = true;
    }
    json_free(ev);
    return rc;
}

static int on_data(void *ud, const char *data, size_t len) {
    katt_t *a = ud;
    int rc = aws_es_feed(&a->dec, data, len);
    if (rc == -1) {
        set_err(a, xstrdup(a->dec.error ? a->dec.error : "event stream framing error"), 0, false);
        return 2;
    }
    return rc;
}

static void on_resp_headers(void *ud, int status, const http_headers_t *h) {
    (void)status;
    katt_t *a = ud;
    const char *rq = http_headers_get(h, "x-amzn-requestid");
    if (!rq) rq = http_headers_get(h, "x-amz-request-id");
    if (rq && !a->request_id) a->request_id = xstrdup(rq);
}

static void katt_init(katt_t *a, const ai_model_t *m, ai_event_cb cb, void *ud) {
    memset(a, 0, sizeof *a);
    aib_init(&a->b, m, cb, ud);
    json_set_str(a->b.msg, "api", "kiro-api");
    a->m = m;
    a->text_idx = a->think_idx = -1;
    a->text = a->think = (sb_t)SB_INIT;
    aws_es_init(&a->dec, on_frame, a);
}

static void katt_free(katt_t *a) {
    for (size_t i = 0; i < a->tools.n; i++) {
        free(a->tools.v[i].id);
        sb_free(&a->tools.v[i].frag);
        json_free(a->tools.v[i].snapshot);
    }
    vec_free(&a->tools);
    sb_free(&a->text);
    sb_free(&a->think);
    aws_es_free(&a->dec);
    free(a->error);
    free(a->stop_reason);
    free(a->request_id);
    json_free(a->refusal);
    json_free(a->token_usage);
    aib_free(&a->b);
}

/* close open blocks and finalize tools that never got `stop`; a truncated or filtered turn's unfinished calls carry
 * cut-off arguments, so they are dropped instead and never reach the agent loop */
static void katt_finish(katt_t *a, bool drop_unfinished) {
    close_thinking(a);
    close_text(a);
    for (size_t i = a->tools.n; i > 0; i--) {
        ktool_t *t = &a->tools.v[i - 1];
        if (t->done || !drop_unfinished) continue;
        json_free(json_arr_remove(kcontent(a), (size_t)t->index));
    }
    if (!drop_unfinished)
        for (size_t i = 0; i < a->tools.n; i++)
            if (!a->tools.v[i].done && finish_tool(a, &a->tools.v[i]) != 0) break;
}

/* usage from metadataEvent.tokenUsage (uncachedInputTokens excludes cache reads and writes); contextUsageEvent
 * reports occupancy as a share of the model's window */
static void katt_usage(katt_t *a) {
    int64_t v[4];
    static const char *const keys[] = {"uncachedInputTokens", "outputTokens", "cacheReadInputTokens", "cacheWriteInputTokens"};
    for (int i = 0; i < 4; i++) {
        json_t *n = json_get(a->token_usage, keys[i]);
        v[i] = json_is_num(n) && n->u.num.d >= 0 ? (int64_t)n->u.num.d : 0;
    }
    aib_usage(&a->b, v[0], v[1], v[2], v[3]);
    if (a->has_ctx && a->m->context_window > 0)
        json_set_int(json_get(a->b.msg, "usage"), "contextTokens", (int64_t)llround(a->ctx_pct / 100 * (double)a->m->context_window));
}

static bool has_visible_content(const json_t *msg) {
    json_t *b;
    json_arr_foreach(json_get_arr(msg, "content"), i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "image") || str_eq(t, "toolCall")) return true;
        if (str_eq(t, "text"))
            for (const char *p = json_get_str(b, "text"); p && *p; p++)
                if (!isspace((unsigned char)*p)) return true;
    }
    return false;
}

/* ProviderResponseError kinds attach AIError flags: content-blocked is terminal, an incomplete stream transient, an
 * empty output transient + EmptyResponse */
static void set_error_id(katt_t *a, uint32_t kinds) { aib_set(&a->b, "errorId", json_int((int64_t)(AIE_CLASS | kinds))); }

static json_t *kfail(katt_t *a, const char *stop, const char *msg) {
    katt_usage(a);
    if (a->request_id) aib_set(&a->b, "responseId", json_str(a->request_id));
    json_t *r = aib_finish(&a->b, stop, msg);
    if (a->err_status && str_eq(stop, "error")) json_set_int(r, "errorStatus", a->err_status);
    return r;
}

static json_t *fail_plain(const ai_model_t *m, ai_event_cb cb, void *ud, const char *msg, bool overflow, int status) {
    ai_builder_t b;
    aib_init(&b, m, cb, ud);
    json_set_str(b.msg, "api", "kiro-api");
    if (overflow) aib_set(&b, "errorFlags", json_parse_str("[\"contextOverflow\"]"));
    json_t *r = aib_finish(&b, "error", msg);
    if (status) json_set_int(r, "errorStatus", status);
    aib_free(&b);
    return r;
}

/* httpError: "Kiro HTTP <status>[ <code>][: <detail>]"; 413, CONTENT_LENGTH_EXCEEDS_THRESHOLD or "Input is too long."
 * mark a context overflow */
static char *http_error(int status, const char *body, bool *overflow, bool *repl) {
    json_t *p = body ? json_parse_str(body) : NULL;
    if (!json_is_obj(p)) {
        json_free(p);
        p = json_obj();
    }
    const char *raw_code = nonempty(json_get(p, "__type"));
    if (!raw_code) raw_code = nonempty(json_get(p, "code"));
    const char *code = raw_code ? (strrchr(raw_code, '#') ? strrchr(raw_code, '#') + 1 : raw_code) : NULL;
    if (code && !*code) code = NULL;
    const char *detail = nonempty(json_get(p, "message"));
    if (!detail) detail = nonempty(json_get(p, "reason"));
    sb_t m = SB_INIT;
    sb_appendf(&m, "Kiro HTTP %d", status);
    if (code) sb_appendf(&m, " %s", code);
    if (detail) sb_appendf(&m, ": %s", detail);
    const char *reason = json_get_str(p, "reason");
    *repl = replayable(sb_cstr(&m), status, code, reason);
    *overflow = status == 413 || str_eq(reason, CONTENT_LENGTH_EXCEEDS_THRESHOLD) || (body && strstr(body, INPUT_TOO_LONG));
    json_free(p);
    return sb_detach(&m);
}

json_t *provider_kiro_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb,
                             void *ud) {
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    if (!o->api_key || !*o->api_key) return fail_plain(m, cb, ud, "Kiro credential is missing", false, 401);
    kiro_cred_t cred;
    char *err = NULL;
    if (kiro_decode_credential(o->api_key, &cred, &err) != 0) {
        json_t *r = fail_plain(m, cb, ud, err, false, 0);
        free(err);
        return r;
    }
    /* an API key without a pinned region uses the runtime discovery resolved for this model */
    char *url = cred.region ? kiro_runtime_base_url(cred.region) : xstrdup(m->base_url ? m->base_url : "");
    bool overflow = false;
    json_t *payload = kiro_build_request(m, ctx, o, &err, &overflow);
    if (!payload) {
        json_t *r = fail_plain(m, cb, ud, err, overflow, 0);
        free(err);
        free(url);
        kiro_cred_free(&cred);
        return r;
    }
    if (cred.oauth && cred.profile_arn) json_set_str(payload, "profileArn", cred.profile_arn);
    if (o->on_payload) o->on_payload(o->payload_ud, payload);
    /* built once: a retry replays byte-identical bytes */
    char *body = json_dump_js(payload);
    json_free(payload);
    ai_stream_opts_t so = *o;
    so.max_retries = 0; /* plain fetch: only Kiro's own pre-output replay below */
    json_t *result = NULL;
    for (int attempt = 1;; attempt++) {
        katt_t a;
        katt_init(&a, m, cb, ud);
        http_headers_t h = {0};
        ai_apply_headers(&h, m, o);
        kiro_service_headers(&h, "GenerateAssistantResponse", cred.token, attempt, MAX_ATTEMPTS);
        int status = 0;
        char *perr = NULL;
        ai_post_ex_t ex = {.on_headers = on_resp_headers, .hook_ud = &a};
        ex.on_headers = on_resp_headers;
        int rc = ai_stream_post_ex(url, &h, body, &so, on_data, &a, &status, &perr, &ex);
        http_headers_free(&h);
        bool cancelled = o->cancel && *o->cancel;
        bool http_fail = rc == -1 && status > 0 && (status < 200 || status >= 300);
        bool flag_overflow = false;
        if (!cancelled && rc != -2) {
            if (http_fail) {
                bool repl = false;
                char *text = http_error(status, ex.err_body, &flag_overflow, &repl);
                char *msg = ai_rewrap_http_error(text, perr);
                free(text);
                set_err(&a, msg, status, repl);
            } else if (rc == -1 && !a.error) {
                set_err(&a, xstrdup(perr ? perr : "request failed"), 0, false);
            } else if (rc == 0 && !a.error && aws_es_finish(&a.dec) != 0) {
                set_err(&a, xstrdup(a.dec.error ? a.dec.error : "truncated message at end of stream"), 0, false);
                a.err_truncated = true;
            }
        }
        free(ex.err_body);
        free(perr);
        if (cancelled || rc == -2) {
            result = kfail(&a, "aborted", "Request was aborted");
            katt_free(&a);
            break;
        }
        if (a.error) {
            /* one replay, only while nothing is visible or billed, for failures the identical request is known to
             * survive: a capacity throttle, a transient tool-format rejection, or a body cut off inside a frame */
            if (attempt < MAX_ATTEMPTS && !a.visible && !a.metered && (a.err_replayable || a.err_truncated)) {
                katt_free(&a);
                int64_t until = mono_ms() + RETRY_DELAY_MS;
                while (mono_ms() < until && !(o->cancel && *o->cancel)) sleep_ms(MIN((int64_t)50, MAX((int64_t)1, until - mono_ms())));
                if (o->cancel && *o->cancel) {
                    result = fail_plain(m, cb, ud, "Request was aborted", false, 0);
                    json_set_str(result, "stopReason", "aborted");
                    break;
                }
                continue;
            }
            if (flag_overflow) aib_set(&a.b, "errorFlags", json_parse_str("[\"contextOverflow\"]"));
            char *msg = xstrdup(a.error);
            result = kfail(&a, "error", msg);
            free(msg);
            katt_free(&a);
            break;
        }
        bool filtered = str_eq(a.stop_reason, "CONTENT_FILTERED") || a.refusal;
        bool truncated = str_eq(a.stop_reason, "MAX_TOKENS") || str_eq(a.stop_reason, "LENGTH");
        katt_finish(&a, filtered || truncated);
        if (a.error) {
            char *msg = xstrdup(a.error);
            result = kfail(&a, "error", msg);
            free(msg);
            katt_free(&a);
            break;
        }
        if (filtered) {
            const char *cat = nonempty(json_get(a.refusal, "category")), *expl = nonempty(json_get(a.refusal, "explanation"));
            json_t *sd = json_obj();
            json_set_str(sd, "type", "refusal");
            json_set(sd, "category", cat ? json_str(cat) : json_null());
            json_set(sd, "explanation", expl ? json_str(expl) : json_null());
            aib_set(&a.b, "stopDetails", sd);
            set_error_id(&a, AIE_CONTENT_BLOCKED);
            char *label = cat ? xasprintf("Refusal (%s)", cat) : xstrdup("Refusal");
            char *msg = xasprintf("%s: %s", label, expl ? expl : "content was filtered by Kiro");
            result = kfail(&a, "error", msg);
            free(label);
            free(msg);
            katt_free(&a);
            break;
        }
        if (!a.visible) {
            set_error_id(&a, AIE_TRANSIENT);
            result = kfail(&a, "error", "Kiro stream ended without visible output");
            katt_free(&a);
            break;
        }
        if (!has_visible_content(a.b.msg) && !o->accept_empty_response) {
            set_error_id(&a, AIE_TRANSIENT | AIE_EMPTY_RESPONSE);
            result = kfail(&a, "error", "Kiro stream ended with reasoning but without final output");
            katt_free(&a);
            break;
        }
        /* truncation wins over an emitted tool call: its arguments may be cut off */
        const char *stop = truncated ? "length" : a.emitted_tools > 0 ? "toolUse" : "stop";
        result = kfail(&a, stop, NULL);
        katt_free(&a);
        break;
    }
    free(body);
    free(url);
    kiro_cred_free(&cred);
    return result;
}
