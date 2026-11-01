/* provider_transport.c - provider-owned model/request shaping applied before API dispatch (port of omp
 * packages/ai/src/registry/{cloudflare-ai-gateway,muse-code,snowflake}.ts prepareModel/prepareRequest, the
 * provider branches of providers/openai-shared.ts resolveOpenAIRequestSetup (GitHub Copilot, QwenCloud Token
 * Plan) and providers/github-copilot-headers.ts). Login flows store structured credentials (JSON keys, key
 * bundles, account URLs); this layer turns them into the bearer, base URL and headers each request needs. */
#define _GNU_SOURCE
#include "provider_transport.h"

#include <ctype.h>
#include <pthread.h>
#include <strings.h>

/* catalog accessor; weak so unit tests that stub the catalog can link without it */
extern json_t *catalog_api_defaults(const char *api) __attribute__((weak));

static char *trim_dup(const char *s) { return str_trim(s ? s : ""); }

static void set_err(char **err, const char *msg) {
    if (err && !*err) *err = xstrdup(msg);
}

/* ---------- small header-object helpers (case-insensitive like Headers) ---------- */

static const char *hdr_get(const json_t *h, const char *name) {
    const char *k;
    json_t *v;
    const char *found = NULL;
    json_obj_foreach(h, i, k, v) {
        if (json_is_str(v) && strcasecmp(k, name) == 0) found = json_sv(v);
    }
    return found;
}

static void hdr_del(json_t *h, const char *name) {
    for (bool again = true; again;) {
        again = false;
        const char *k;
        json_t *v;
        json_obj_foreach(h, i, k, v) {
            (void)v;
            if (strcasecmp(k, name) == 0) {
                char *kk = xstrdup(k);
                json_del(h, kk);
                free(kk);
                again = true;
                break;
            }
        }
    }
}

static void hdr_set(json_t *h, const char *name, const char *value) {
    hdr_del(h, name);
    json_set_str(h, name, value);
}

static void hdr_set_if_absent(json_t *h, const char *name, const char *value) {
    if (!hdr_get(h, name)) json_set_str(h, name, value);
}

/* ======================================================================================= */
/* Snowflake (registry/snowflake.ts)                                                        */
/* ======================================================================================= */

#define SNOWFLAKE_PLACEHOLDER "https://snowflake-account.invalid"

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

char *ai_snowflake_account_url(const char *input, char **err) {
    char *v = str_trim(input ? input : "");
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

/* origin (scheme://host[:port]) of an absolute URL, lower-cased host; NULL when it does not parse */
static char *url_origin(const char *u) {
    const char *s = u ? strstr(u, "://") : NULL;
    if (!s || s == u) return NULL;
    const char *h = s + 3;
    size_t hl = strcspn(h, "/?#");
    if (!hl) return NULL;
    char *scheme = xstrndup(u, (size_t)(s - u)), *host = xstrndup(h, hl);
    for (char *p = scheme; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
    /* default ports are dropped like URL.origin */
    if ((str_eq(scheme, "https") && str_ends(host, ":443")) || (str_eq(scheme, "http") && str_ends(host, ":80")))
        *strrchr(host, ':') = 0;
    char *o = xasprintf("%s://%s", scheme, host);
    free(scheme);
    free(host);
    return o;
}

static int snowflake_prepare(ai_transport_t *t, char **err) {
    char *raw = trim_dup(t->api_key);
    if (!*raw) {
        free(raw);
        raw = trim_dup(env_get("SNOWFLAKE_PAT"));
    }
    if (!*raw) {
        free(raw);
        return 0;
    }
    char *token = NULL, *account = NULL;
    if (raw[0] != '{') {
        token = xstrdup(raw);
    } else {
        json_t *j = json_parse_str(raw);
        const char *tk = json_get_str(j, "token"), *eu = json_get_str(j, "enterpriseUrl");
        char *tt = trim_dup(tk), *et = trim_dup(eu);
        if (!json_is_obj(j) || !tk || !*tt || !eu || !*et) {
            set_err(err, "Invalid Snowflake credential; run /login snowflake again");
        } else {
            char *aerr = NULL;
            account = ai_snowflake_account_url(eu, &aerr);
            if (!account) *err = xasprintf("Invalid Snowflake credential account: %s; run /login snowflake again", aerr ? aerr : "?");
            else token = xstrdup(tt);
            free(aerr);
        }
        free(tt);
        free(et);
        json_free(j);
    }
    free(raw);
    if (!token) {
        free(account);
        return -1;
    }
    const char *base = t->model.base_url;
    size_t pl = strlen(SNOWFLAKE_PLACEHOLDER);
    if (str_eq(base, SNOWFLAKE_PLACEHOLDER) || (str_starts(base, SNOWFLAKE_PLACEHOLDER) && base[pl] == '/')) {
        char *acct = account;
        if (!acct) {
            acct = ai_snowflake_account_url(env_get("SNOWFLAKE_ACCOUNT"), err);
            if (!acct) {
                free(token);
                return -1;
            }
        }
        char *nb = xasprintf("%s%s", acct, base + pl);
        if (acct != account) free(acct);
        free(t->base_url);
        t->base_url = nb;
        t->model.base_url = nb;
    } else if (account) {
        char *o = url_origin(base);
        bool same = o && str_eq(o, account);
        free(o);
        if (!same) {
            set_err(err, "Snowflake OAuth account does not match the model endpoint");
            free(token);
            free(account);
            return -1;
        }
    }
    free(account);
    free(t->api_key);
    t->api_key = token;
    return 0;
}

/* ======================================================================================= */
/* Muse Code (registry/muse-code.ts)                                                        */
/* ======================================================================================= */

static int muse_prepare(ai_transport_t *t, char **err) {
    if (str_empty(t->api_key)) return 0;
    json_t *j = json_parse_str(t->api_key);
    const char *o = json_get_str(j, "oauthAccessToken"), *k = json_get_str(j, "apiKey");
    char *ot = trim_dup(o), *kt = trim_dup(k);
    bool ok = json_is_obj(j) && o && k && *ot && *kt;
    free(ot);
    free(kt);
    if (!ok) {
        json_free(j);
        set_err(err, "Muse Code credential is invalid; sign in again");
        return -1;
    }
    free(t->api_key);
    t->api_key = xstrdup(k);
    json_free(j);
    /* {"x-api-version": "1.0.0", ...options.headers} */
    json_t *h = json_obj();
    json_set_str(h, "x-api-version", "1.0.0");
    const char *hk;
    json_t *hv;
    json_obj_foreach(t->opt_headers, i, hk, hv) {
        if (json_is_str(hv)) hdr_set(h, hk, json_sv(hv));
    }
    json_free(t->opt_headers);
    t->opt_headers = h;
    t->opts.headers = h;
    return 0;
}

/* ======================================================================================= */
/* Cloudflare AI Gateway (registry/cloudflare-ai-gateway.ts + catalog wire/cloudflare*.ts)  */
/* ======================================================================================= */

#define CF_GATEWAY_BASE "https://gateway.ai.cloudflare.com/v1/<account>/<gateway>"

typedef struct {
    char *token, *account, *gateway;
} cf_cred_t;

static void cf_free(cf_cred_t *c) {
    free(c->token);
    free(c->account);
    free(c->gateway);
}

/* parseCloudflareCredential: structured login credential or a legacy plain token; false when empty/malformed */
bool ai_cloudflare_credential(const char *value, char **token, char **account, char **gateway) {
    *token = *account = *gateway = NULL;
    char *v = trim_dup(value);
    bool ok = false;
    if (!*v) {
    } else if (v[0] != '{') {
        *token = xstrdup(v);
        ok = true;
    } else {
        json_t *j = json_parse_str(v);
        json_t *tk = json_get(j, "token"), *ac = json_get(j, "accountId"), *gw = json_get(j, "gatewayId");
        char *tt = json_is_str(tk) ? trim_dup(json_sv(tk)) : NULL;
        if (json_is_obj(j) && tt && *tt && (!ac || json_is_str(ac)) && (!gw || json_is_str(gw))) {
            *token = tt;
            tt = NULL;
            char *a = ac ? trim_dup(json_sv(ac)) : NULL, *g = gw ? trim_dup(json_sv(gw)) : NULL;
            if (a && *a) *account = a;
            else free(a);
            if (g && *g) *gateway = g;
            else free(g);
            ok = true;
        }
        free(tt);
        json_free(j);
    }
    free(v);
    return ok;
}

/* catalog runtime/behavior.kdl api-routes provider="cloudflare-ai-gateway" */
static void cf_prepare_model(ai_transport_t *t) {
    const char *id = t->model.id;
    const char *api = NULL, *base = NULL;
    char *req_id = NULL;
    bool ph = strstr(t->model.base_url, "<account>") || strstr(t->model.base_url, "<gateway>");
    if (str_starts(id, "anthropic/")) {
        api = "anthropic-messages";
        base = ph ? CF_GATEWAY_BASE "/anthropic" : NULL;
        const char *rq = json_get_str(t->model.raw, "requestModelId");
        req_id = xstrdup(rq ? rq : id + strlen("anthropic/"));
        for (char *p = req_id; *p; p++)
            if (*p == '.') *p = '-';
    } else if (str_starts(id, "openai/")) {
        api = "openai-completions";
        base = ph ? CF_GATEWAY_BASE "/openai" : NULL;
        req_id = xstrdup(id + strlen("openai/"));
    } else if (str_starts(id, "workers-ai/")) {
        api = "openai-completions";
        base = ph ? CF_GATEWAY_BASE "/compat" : NULL;
    } else {
        return;
    }
    if (base && !str_eq(base, t->model.base_url)) {
        free(t->base_url);
        t->base_url = xstrdup(base);
        t->model.base_url = t->base_url;
    }
    if (!str_eq(api, t->model.api)) {
        free(t->api);
        t->api = xstrdup(api);
        t->model.api = t->api;
        t->model.compat_defaults = catalog_api_defaults ? catalog_api_defaults(api) : NULL;
    }
    if (req_id && !str_eq(req_id, json_get_str(t->model.raw, "requestModelId"))) {
        ai_transport_own_raw(t);
        json_set_str(t->raw, "requestModelId", req_id);
    }
    free(req_id);
}

static int cf_prepare_request(ai_transport_t *t, char **err) {
    cf_cred_t c = {0};
    char *envk = NULL;
    const char *src = t->api_key;
    if (!src) src = envk = env_get("CLOUDFLARE_AI_GATEWAY_API_KEY") ? xstrdup(env_get("CLOUDFLARE_AI_GATEWAY_API_KEY")) : NULL;
    bool ok = ai_cloudflare_credential(src ? src : "", &c.token, &c.account, &c.gateway);
    free(envk);
    if (!ok) return 0;
    const char *acct = c.account ? c.account : env_get("CLOUDFLARE_ACCOUNT_ID");
    const char *gw = c.gateway ? c.gateway : env_get("CLOUDFLARE_GATEWAY_ID");
    if (str_starts(t->model.base_url, CF_GATEWAY_BASE)) {
        if (str_empty(acct)) {
            set_err(err, "Cloudflare account ID is required");
            cf_free(&c);
            return -1;
        }
        if (str_empty(gw)) {
            set_err(err, "Cloudflare AI Gateway ID is required");
            cf_free(&c);
            return -1;
        }
        sb_t b = SB_INIT;
        const char *p = t->model.base_url;
        const char *a = strstr(p, "<account>");
        sb_appendn(&b, p, (size_t)(a - p));
        sb_append(&b, acct);
        p = a + strlen("<account>");
        const char *g = strstr(p, "<gateway>");
        if (g) {
            sb_appendn(&b, p, (size_t)(g - p));
            sb_append(&b, gw);
            p = g + strlen("<gateway>");
        }
        sb_append(&b, p);
        free(t->base_url);
        t->base_url = sb_detach(&b);
        t->model.base_url = t->base_url;
    }
    bool anthropic = str_eq(t->model.api, "anthropic-messages");
    if (!anthropic) {
        json_t *h = t->model.headers ? json_clone(t->model.headers) : json_obj();
        hdr_del(h, "authorization");
        hdr_del(h, "x-api-key");
        char *bearer = xasprintf("Bearer %s", c.token);
        json_set_str(h, "cf-aig-authorization", bearer);
        free(bearer);
        json_free(t->model_headers);
        t->model_headers = h;
        t->model.headers = h;
    }
    free(t->api_key);
    t->api_key = anthropic ? xstrdup(c.token) : xstrdup(AI_NO_AUTH_SENTINEL);
    cf_free(&c);
    return 0;
}

/* ======================================================================================= */
/* QwenCloud Token Plan (openai-shared.ts + catalog wire/alibaba-token-plan.ts)             */
/* ======================================================================================= */

/* ^sk-[A-Za-z0-9._~+/-]+={0,2}$ */
static bool token_plan_token(const char *s) {
    if (!str_starts(s, "sk-")) return false;
    const char *p = s + 3, *q = p;
    while (*q && (isalnum((unsigned char)*q) || strchr("._~+/-", *q))) q++;
    if (q == p) return false;
    int eq = 0;
    while (*q == '=' && eq < 2) q++, eq++;
    return !*q;
}

static int alibaba_token_plan_prepare(ai_transport_t *t, bool explicit_key, char **err) {
    if (!explicit_key) {
        set_err(err, "No API key for provider: alibaba-token-plan");
        return -1;
    }
    char *v = trim_dup(t->api_key), *token = NULL, *base = NULL;
    if (v[0] != '{') {
        if (token_plan_token(v)) token = xstrdup(v);
    } else {
        json_t *j = json_parse_str(v);
        json_t *tk = json_get(j, "token"), *ck = json_get(j, "cookie"), *bu = json_get(j, "baseUrl");
        char *tt = json_is_str(tk) ? trim_dup(json_sv(tk)) : NULL;
        if (json_is_obj(j) && tt && token_plan_token(tt) && (!ck || json_is_str(ck)) && (!bu || json_is_str(bu))) {
            token = tt;
            tt = NULL;
            char *b = bu ? trim_dup(json_sv(bu)) : NULL;
            if (b && *b) base = b;
            else free(b);
        }
        free(tt);
        json_free(j);
    }
    free(v);
    if (!token) {
        set_err(err, "Invalid QwenCloud Token Plan credential");
        return -1;
    }
    free(t->api_key);
    t->api_key = token;
    if (base) {
        free(t->base_url);
        t->base_url = base;
        t->model.base_url = base;
    }
    return 0;
}

/* ======================================================================================= */
/* Xiaomi MiMo Token Plan keys (catalog provider-models/openai-compat.ts xiaomiModelManagerOptions) */
/* ======================================================================================= */

static void xiaomi_prepare(ai_transport_t *t) {
    const char *k = t->api_key;
    if (!k || !(str_starts(k, "tp-") || str_starts(k, "ttp-"))) return;
    /* Token Plan keys authenticate only on the regional clusters; discovery pins the first (SGP) */
    const char *std = "https://api.xiaomimimo.com/v1";
    if (!str_starts(t->model.base_url, std)) return;
    char *nb = xasprintf("https://token-plan-sgp.xiaomimimo.com/v1%s", t->model.base_url + strlen(std));
    free(t->base_url);
    t->base_url = nb;
    t->model.base_url = nb;
}

/* ======================================================================================= */
/* GitHub Copilot (catalog wire/github-copilot.ts + providers/github-copilot-headers.ts)     */
/* ======================================================================================= */

#define COPILOT_UA "copilot/1.0.82"
#define COPILOT_CLI_INTEGRATION "copilot-developer-cli"
#define COPILOT_CHAT_INTEGRATION "copilot-chat"

/* normalizeCopilotIntegrationId: trimmed, non-empty, no CR/LF; owned or NULL */
static char *copilot_integration_id(const char *v) {
    if (!v) return NULL;
    char *t = trim_dup(v);
    if (!*t || strpbrk(t, "\r\n")) {
        free(t);
        return NULL;
    }
    return t;
}

/* normalizeDomain: hostname of the input (https:// assumed); owned or NULL */
static char *copilot_domain(const char *in) {
    char *t = trim_dup(in);
    if (!*t) {
        free(t);
        return NULL;
    }
    const char *h = strstr(t, "://");
    h = h ? h + 3 : t;
    const char *at = NULL;
    size_t hl = strcspn(h, "/?#");
    for (const char *p = h; p < h + hl; p++)
        if (*p == '@') at = p;
    if (at) {
        hl -= (size_t)(at + 1 - h);
        h = at + 1;
    }
    size_t nl = hl;
    if (*h == '[') {
        const char *rb = memchr(h, ']', hl);
        nl = rb ? (size_t)(rb + 1 - h) : hl;
    } else {
        const char *colon = memchr(h, ':', hl);
        if (colon) nl = (size_t)(colon - h);
    }
    char *out = nl ? xstrndup(h, nl) : NULL;
    if (out)
        for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
    free(t);
    return out;
}

static bool public_github_host(const char *h) {
    return str_eq(h, "api.github.com") || str_eq(h, "github.com") || str_eq(h, "www.github.com");
}

void ai_copilot_parse_key(const char *raw, char **token, char **enterprise, char **api_endpoint) {
    *token = *enterprise = *api_endpoint = NULL;
    json_t *j = raw ? json_parse_str(raw) : NULL;
    if (json_is_obj(j) && json_is_str(json_get(j, "token"))) {
        *token = xstrdup(json_get_str(j, "token"));
        const char *e = json_get_str(j, "enterpriseUrl");
        if (e) {
            char *d = copilot_domain(e);
            if (!d) d = trim_dup(e);
            for (char *p = d; *p; p++) *p = (char)tolower((unsigned char)*p);
            if (*d && !public_github_host(d)) *enterprise = d;
            else free(d);
        }
        const char *a = json_get_str(j, "apiEndpoint");
        char *at = a ? trim_dup(a) : NULL;
        if (at && str_starts(at, "https://") && at[8] && !strchr("/?#", at[8])) {
            for (size_t n = strlen(at); n > 0 && at[n - 1] == '/';) at[--n] = 0;
            *api_endpoint = at;
            at = NULL;
        }
        free(at);
    } else {
        *token = xstrdup(raw ? raw : "");
    }
    json_free(j);
}

/* resolveGitHubCopilotBaseUrl; owned */
static char *copilot_base_url(const char *base, const char *enterprise, const char *api_endpoint) {
    if (api_endpoint && (str_empty(base) || strstr(base, "githubcopilot.com"))) return xstrdup(api_endpoint);
    if (!enterprise) return xstrdup(base ? base : "");
    if (!str_empty(base) && !strstr(base, "githubcopilot.com")) return xstrdup(base);
    return str_starts(enterprise, "copilot-api.") ? xasprintf("https://%s", enterprise) : xasprintf("https://copilot-api.%s", enterprise);
}

/* working Copilot-Integration-Id learned per credential+host (process-local, bounded) */
#define COPILOT_CACHE_MAX 50
static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char *key, *id;
    uint64_t tick;
} g_ccache[COPILOT_CACHE_MAX];
static uint64_t g_ctick;

char *ai_copilot_cache_get(const char *key) {
    if (!key) return NULL;
    char *r = NULL;
    pthread_mutex_lock(&g_cmu);
    for (int i = 0; i < COPILOT_CACHE_MAX; i++)
        if (g_ccache[i].key && str_eq(g_ccache[i].key, key)) r = xstrdup(g_ccache[i].id);
    pthread_mutex_unlock(&g_cmu);
    return r;
}

void ai_copilot_cache_set(const char *key, const char *id) {
    if (!key) return;
    pthread_mutex_lock(&g_cmu);
    int slot = -1;
    for (int i = 0; i < COPILOT_CACHE_MAX; i++)
        if (g_ccache[i].key && str_eq(g_ccache[i].key, key)) slot = i;
    if (slot < 0) {
        for (int i = 0; i < COPILOT_CACHE_MAX && slot < 0; i++)
            if (!g_ccache[i].key) slot = i;
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < COPILOT_CACHE_MAX; i++)
            if (g_ccache[i].tick < g_ccache[slot].tick) slot = i;
    }
    if (id) {
        if (!g_ccache[slot].key || !str_eq(g_ccache[slot].key, key)) {
            free(g_ccache[slot].key);
            g_ccache[slot].key = xstrdup(key);
        }
        free(g_ccache[slot].id);
        g_ccache[slot].id = xstrdup(id);
        g_ccache[slot].tick = ++g_ctick;
    } else if (g_ccache[slot].key && str_eq(g_ccache[slot].key, key)) {
        free(g_ccache[slot].key);
        free(g_ccache[slot].id);
        g_ccache[slot].key = g_ccache[slot].id = NULL;
    }
    pthread_mutex_unlock(&g_cmu);
}

/* getCopilotIntegrationCacheKey: enterprise \0 endpoint \0 normalized host \0 token fingerprint */
static char *copilot_cache_key(const char *token, const char *enterprise, const char *endpoint, const char *base) {
    if (str_empty(token)) return NULL;
    char *b = trim_dup(base);
    for (size_t n = strlen(b); n > 0 && b[n - 1] == '/';) b[--n] = 0;
    for (char *p = b; *p && *p != '/'; p++) *p = (char)tolower((unsigned char)*p);
    char *k = xasprintf("%s|%s|%s|%016llx", enterprise ? enterprise : "", endpoint ? endpoint : "", b,
                        (unsigned long long)hash_xx64(token, strlen(token), 0x6f6d63));
    free(b);
    return k;
}

static bool msg_has_image(const json_t *m) {
    const char *role = json_get_str(m, "role");
    if (!str_eq(role, "user") && !str_eq(role, "toolResult")) return false;
    json_t *c = json_get(m, "content"), *b;
    json_arr_foreach(c, i, b) if (str_eq(json_get_str(b, "type"), "image")) return true;
    return false;
}

/* inferCopilotInitiator over omp-shaped messages */
static const char *copilot_initiator(const json_t *messages, const json_t *caller) {
    const char *ov = NULL;
    const char *k;
    json_t *v;
    json_obj_foreach(caller, i, k, v) {
        if (strcasecmp(k, "x-initiator") || !json_is_str(v)) continue;
        char *n = trim_dup(json_sv(v));
        for (char *p = n; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (str_eq(n, "user")) ov = "user";
        else if (str_eq(n, "agent")) ov = "agent";
        free(n);
    }
    if (ov) return ov;
    size_t n = json_len(messages);
    if (!n) return "user";
    const json_t *last = json_at(messages, n - 1);
    const char *attr = json_get_str(last, "attribution");
    if (attr) {
        char *a = trim_dup(attr);
        for (char *p = a; *p; p++) *p = (char)tolower((unsigned char)*p);
        const char *r = str_eq(a, "user") ? "user" : str_eq(a, "agent") ? "agent" : NULL;
        free(a);
        if (r) return r;
    }
    const char *role = json_get_str(last, "role");
    if (!role) return "user";
    if (!str_eq(role, "user")) return "agent";
    json_t *c = json_get(last, "content");
    if (json_is_arr(c) && json_len(c) > 0 && str_eq(json_get_str(json_at(c, json_len(c) - 1), "type"), "tool_result")) return "agent";
    return "user";
}

static int copilot_prepare(ai_transport_t *t, const ai_context_t *ctx) {
    char *token, *enterprise, *endpoint;
    ai_copilot_parse_key(t->api_key ? t->api_key : "", &token, &enterprise, &endpoint);
    char *base = copilot_base_url(t->model.base_url, enterprise, endpoint);
    free(t->base_url);
    t->base_url = base;
    t->model.base_url = base;
    bool anthropic = str_eq(t->model.api, "anthropic-messages");
    /* explicit identity: caller headers, then COPILOT_INTEGRATION_ID */
    char *explicit_id = copilot_integration_id(hdr_get(t->opt_headers, "copilot-integration-id"));
    if (!explicit_id) explicit_id = copilot_integration_id(env_get("COPILOT_INTEGRATION_ID"));
    char *ckey = copilot_cache_key(token, enterprise, endpoint, base);
    char *craw = ai_copilot_cache_get(ckey);
    char *cached = copilot_integration_id(craw);
    free(craw);
    const char *init = copilot_initiator(ctx ? ctx->messages : NULL, t->opt_headers);
    json_t *dyn = json_obj();
    json_set_str(dyn, "User-Agent", COPILOT_UA);
    json_set_str(dyn, "Editor-Version", COPILOT_UA);
    json_set_str(dyn, "Copilot-Integration-Id", explicit_id ? explicit_id
                                                : cached    ? cached
                                                : enterprise ? COPILOT_CLI_INTEGRATION
                                                             : COPILOT_CHAT_INTEGRATION);
    json_set_str(dyn, "Copilot-Harness-Id", "copilot-sdk");
    json_set_str(dyn, "Openai-Intent", "conversation-agent");
    json_set_str(dyn, "X-Initiator", init);
    char *it = xasprintf("conversation-%s", init);
    json_set_str(dyn, "X-Interaction-Type", it);
    free(it);
    bool vision = false;
    json_t *m;
    json_arr_foreach(ctx ? ctx->messages : NULL, i, m) vision = vision || msg_has_image(m);
    if (vision) json_set_str(dyn, "Copilot-Vision-Request", "true");
    /* openai: dynamic headers win over caller headers; anthropic: caller headers win */
    json_t *h = t->opt_headers ? json_clone(t->opt_headers) : json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(dyn, i, k, v) {
        if (anthropic) hdr_set_if_absent(h, k, json_sv(v));
        else hdr_set(h, k, json_sv(v));
    }
    json_free(dyn);
    json_free(t->opt_headers);
    t->opt_headers = h;
    t->opts.headers = h;
    /* premium requests: agent turns are free; a 0 multiplier still costs one on the (assumed) free plan */
    if (str_eq(init, "agent")) t->premium_requests = 0;
    else {
        json_t *pm = json_get(t->model.raw, "premiumMultiplier");
        double mult = json_is_num(pm) ? pm->u.num.d : 1;
        if (json_is_num(pm) && pm->is_int) mult = (double)pm->u.num.i;
        t->premium_requests = mult == 0 ? 1 : mult;
    }
    t->has_premium = true;
    t->opts.copilot_identity_retry = !explicit_id;
    t->copilot_cache_key = ckey;
    t->opts.copilot_cache_key = ckey;
    t->copilot_snapshot = cached;
    t->opts.copilot_cache_snapshot = cached;
    free(t->api_key);
    t->api_key = token;
    free(explicit_id);
    free(enterprise);
    free(endpoint);
    return 0;
}

/* ======================================================================================= */
/* entry points                                                                             */
/* ======================================================================================= */

void ai_transport_own_raw(ai_transport_t *t) {
    if (t->raw) return;
    t->raw = json_clone(t->model.raw);
    t->model.raw = t->raw;
    /* borrowed views into raw follow the clone (headers may already be a transport-owned object) */
    t->model.compat = json_get_obj(t->raw, "compat");
    if (!t->model_headers) t->model.headers = json_get_obj(t->raw, "headers");
    t->model.thinking = json_get_obj(t->raw, "thinking");
}

int ai_transport_prepare(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_transport_t *t,
                         char **err) {
    memset(t, 0, sizeof *t);
    t->model = *m;
    if (!t->model.base_url) t->model.base_url = "";
    t->opts = *o;
    t->opt_headers = json_is_obj(o->headers) ? json_clone(o->headers) : NULL;
    t->opts.headers = t->opt_headers;
    bool explicit_key = !str_empty(o->api_key);
    t->api_key = explicit_key ? xstrdup(o->api_key) : NULL;
    *err = NULL;
    const char *p = m->provider ? m->provider : "";
    int rc = 0;
    if (str_eq(p, "cloudflare-ai-gateway")) {
        cf_prepare_model(t);
        rc = cf_prepare_request(t, err);
    } else if (str_eq(p, "muse-code")) {
        rc = muse_prepare(t, err);
    } else if (str_eq(p, "snowflake")) {
        rc = snowflake_prepare(t, err);
    } else if (str_eq(p, "alibaba-token-plan") && str_starts(m->api, "openai-")) {
        rc = alibaba_token_plan_prepare(t, explicit_key, err);
    } else if (str_eq(p, "xiaomi")) {
        if (!t->api_key) t->api_key = ai_env_api_key(p);
        xiaomi_prepare(t);
    } else if (str_eq(p, "github-copilot")) {
        if (!t->api_key) t->api_key = ai_env_api_key(p);
        if (t->api_key) rc = copilot_prepare(t, ctx);
    }
    t->opts.api_key = t->api_key;
    return rc;
}

void ai_transport_free(ai_transport_t *t) {
    free(t->api_key);
    free(t->base_url);
    free(t->api);
    json_free(t->raw);
    json_free(t->model_headers);
    json_free(t->opt_headers);
    free(t->copilot_cache_key);
    free(t->copilot_snapshot);
    memset(t, 0, sizeof *t);
}

void ai_transport_finish(const ai_transport_t *t, json_t *msg) {
    if (!t->has_premium || !msg) return;
    json_t *u = json_get_obj(msg, "usage");
    if (u) json_set_num(u, "premiumRequests", t->premium_requests);
}
