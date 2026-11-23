/* usage_providers.c - per-provider usage fetchers (ports of omp packages/ai/src/usage/{claude,claude-api,
 * openai-codex,openai-codex-base-url,openai-codex-reset,github-copilot,zai,synthetic,ollama,kiro,shared}.ts).
 * Every request goes through auth_http, so OMC_AUTH_URL_REWRITE redirects the probes to mocks in tests. */
#define _GNU_SOURCE
#include "usage.h"

#include "ai/kiro.h"
#include "ai/provider_anthropic.h"
#include "config/auth_engine.h"
#include "session/trace.h"

#include <ctype.h>
#include <math.h>
#include <time.h>

#define HOUR_MS 3600000.0
#define DAY_MS (24 * HOUR_MS)
#define WEEK_MS (7 * DAY_MS)
#define OMP_USER_AGENT OMC_NAME "/" OMC_VERSION
#define CODEX_BASE_URL "https://chatgpt.com/backend-api"
#define COPILOT_CLI_UA "copilot/1.0.82"

/* ---------- shared ---------- */

static int uhttp(const char *method, const char *url, const http_headers_t *h, const char *body, const usage_params_t *p,
                 http_resp_t *r) {
    memset(r, 0, sizeof *r);
    return auth_http(method, url, h, body, p->timeout_ms > 0 ? p->timeout_ms : 10000, NULL, r);
}

static json_t *resp_json(const http_resp_t *r) { return r->body ? json_parse(r->body, r->body_len, NULL) : NULL; }

/* toNumber: a finite number, or a numeric string */
static bool to_num(const json_t *v, double *out) {
    if (json_is_num(v) && isfinite(v->u.num.d)) return *out = v->u.num.d, true;
    if (json_is_str(v)) {
        char *t = str_trim(json_sv(v));
        char *end = NULL;
        double d = *t ? strtod(t, &end) : NAN;
        bool ok = *t && end && !*end && isfinite(d);
        free(t);
        if (ok) return *out = d, true;
    }
    return false;
}

static bool get_num(const json_t *o, const char *k, double *out) { return to_num(json_get(o, k), out); }

static bool iso_ms(const char *s, double *out) { return s && *s && trace_parse_iso_ms(s, out); }

static const char *cstr(const json_t *c, const char *k) {
    const char *v = json_get_str(c, k);
    return v && *v ? v : NULL;
}

static char *slugify(const char *in) {
    sb_t b = SB_INIT;
    bool dash = false;
    for (const char *p = in; *p; p++) {
        char c = (char)tolower((unsigned char)*p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && b.len) sb_putc(&b, '-');
            dash = false;
            sb_putc(&b, c);
        } else dash = true;
    }
    return sb_detach(&b);
}

static const char *status_of(bool has, double f) { return usage_status_for(has, f); }

static json_t *window_new(const char *id, const char *label) {
    json_t *w = json_obj();
    json_set_str(w, "id", id);
    json_set_str(w, "label", label);
    return w;
}

/* shared.ts buildUsageAmount (key order: used, limit, remaining, usedFraction, remainingFraction, unit) */
static json_t *counter_amount(bool hu, double used, bool hl, double limit, bool hr, double remaining, bool hf, double uf,
                              const char *unit) {
    if (!hf && hu && hl && limit > 0) hf = true, uf = MIN(used / limit, 1.0);
    json_t *a = json_obj();
    if (hu) json_set_num(a, "used", used);
    if (hl) json_set_num(a, "limit", limit);
    if (hr) json_set_num(a, "remaining", remaining);
    if (hf) {
        json_set_num(a, "usedFraction", uf);
        json_set_num(a, "remainingFraction", MAX(1 - uf, 0.0));
    }
    json_set_str(a, "unit", unit);
    return a;
}

/* percent amount from a 0..100 utilization */
static json_t *percent_amount(double util) {
    double c = MIN(MAX(util, 0.0), 100.0), f = c / 100;
    json_t *a = json_obj();
    json_set_num(a, "used", c);
    json_set_num(a, "limit", 100);
    json_set_num(a, "remaining", MAX(0.0, 100 - c));
    json_set_num(a, "usedFraction", f);
    json_set_num(a, "remainingFraction", MAX(0.0, 1 - f));
    json_set_str(a, "unit", "percent");
    return a;
}

static json_t *report_new(const char *provider, int64_t fetched) {
    json_t *r = json_obj();
    json_set_str(r, "provider", provider);
    json_set_int(r, "fetchedAt", fetched);
    json_set(r, "limits", json_arr());
    return r;
}

static json_t *limit_new(const char *id, const char *label, json_t *scope, json_t *window, json_t *amount, const char *status) {
    json_t *l = json_obj();
    json_set_str(l, "id", id);
    json_set_str(l, "label", label);
    json_set(l, "scope", scope);
    if (window) json_set(l, "window", window);
    json_set(l, "amount", amount);
    if (status) json_set_str(l, "status", status);
    return l;
}

static json_t *scope_new(const char *provider) {
    json_t *s = json_obj();
    json_set_str(s, "provider", provider);
    return s;
}

/* ============================================================================================
 * Anthropic (usage/claude.ts + claude-api.ts)
 * ============================================================================================ */

#define CLAUDE_API_BASE "https://api.anthropic.com"
#define CLAUDE_OAUTH_BASE CLAUDE_API_BASE "/api/oauth"
#define CLAUDE_USAGE_BETAS                                                                                              \
    "claude-code-20250219,oauth-2025-04-20,interleaved-thinking-2025-05-14,redact-thinking-2026-02-12,context-"         \
    "management-2025-06-27,prompt-caching-scope-2026-01-05,advanced-tool-use-2025-11-20,effort-2025-11-24,extended-"   \
    "cache-ttl-2025-04-11"

/* normalizeClaudeApiBaseUrl */
static char *claude_api_base(const char *base) {
    char *t = base ? str_trim(base) : xstrdup("");
    url_t u;
    if (!*t || url_parse(t, &u) != 0) {
        free(t);
        return xstrdup(CLAUDE_API_BASE);
    }
    free(t);
    char *path = xstrdup(u.path ? u.path : "");
    char *q = strpbrk(path, "?#");
    if (q) *q = 0;
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') path[--n] = 0;
    if (n >= 10 && !strcasecmp(path + n - 10, "/api/oauth")) path[n - 10] = 0;
    else if (n >= 3 && !strcasecmp(path + n - 3, "/v1")) path[n - 3] = 0;
    bool def = (!strcmp(u.scheme, "https") && u.port == 443) || (!strcmp(u.scheme, "http") && u.port == 80);
    char *origin = def ? xasprintf("%s://%s", u.scheme, u.host) : xasprintf("%s://%s:%d", u.scheme, u.host, u.port);
    char *r = xasprintf("%s%s", origin, path);
    free(origin);
    free(path);
    url_free(&u);
    return r;
}

static void claude_headers(http_headers_t *h, const char *token, const char *beta) {
    http_headers_set(h, "accept", "application/json, text/plain, */*");
    http_headers_set(h, "accept-encoding", "gzip, compress, deflate, br");
    http_headers_set(h, "anthropic-beta", beta);
    http_headers_set(h, "content-type", "application/json");
    http_headers_set(h, "connection", "keep-alive");
    char *ua = xasprintf("claude-cli/%s (external, cli)", anthropic_claude_code_version());
    http_headers_set(h, "user-agent", ua);
    free(ua);
    char *auth = xasprintf("Bearer %s", token);
    http_headers_set(h, "authorization", auth);
    free(auth);
}

typedef struct {
    bool has_util, has_reset;
    double util, reset;
} bucket_t;

static bool parse_bucket(const json_t *b, bucket_t *out) {
    memset(out, 0, sizeof *out);
    if (!json_is_obj(b)) return false;
    out->has_util = get_num(b, "utilization", &out->util);
    out->has_reset = iso_ms(json_get_str(b, "resets_at"), &out->reset);
    return out->has_util || out->has_reset;
}

static json_t *claude_limit(const char *id, const char *label, const char *window_id, const char *window_label,
                            double duration, const bucket_t *b, const char *tier, int shared) {
    if (!b || !b->has_util) return NULL;
    json_t *amount = percent_amount(b->util);
    json_t *w = window_new(window_id, window_label);
    json_set_num(w, "durationMs", duration);
    if (b->has_reset) json_set_num(w, "resetsAt", b->reset);
    json_t *s = scope_new("anthropic");
    json_set_str(s, "windowId", window_id);
    if (tier) json_set_str(s, "tier", tier);
    if (shared >= 0) json_set_bool(s, "shared", shared);
    double f = json_get_num(amount, "usedFraction", 0);
    return limit_new(id, label, s, w, amount, status_of(true, f));
}

typedef struct {
    char *kind, *display;
    bucket_t b;
} api_entry_t;

typedef VEC(api_entry_t) api_vec_t;
static void parse_api_limits(const json_t *raw, api_vec_t *out) {
    json_t *e;
    json_arr_foreach(raw, i, e) {
        if (!json_is_obj(e) || !json_get_str(e, "kind")) continue;
        api_entry_t x = {0};
        x.b.has_util = get_num(e, "percent", &x.b.util);
        x.b.has_reset = iso_ms(json_get_str(e, "resets_at"), &x.b.reset);
        if (!x.b.has_util && !x.b.has_reset) continue;
        x.kind = xstrdup(json_get_str(e, "kind"));
        const char *dn = json_get_str(json_get_obj(json_get_obj(e, "scope"), "model"), "display_name");
        if (dn) {
            char *t = str_trim(dn);
            if (*t) x.display = t;
            else free(t);
        }
        vec_push(out, x);
    }
}

static bool dollars(const json_t *minor, const json_t *exp, const json_t *cur, bool cur_required, double *out) {
    if (!json_is_num(minor) || !json_is_num(exp)) return false;
    double m = minor->u.num.d, e = exp->u.num.d;
    if (m < 0 || e < 0 || m != floor(m) || e != floor(e)) return false;
    if (!cur) {
        if (cur_required) return false;
    } else if (!json_is_str(cur) || strcasecmp(json_sv(cur), "USD")) return false;
    double d = m / pow(10, e);
    if (!isfinite(d)) return false;
    *out = d;
    return true;
}

static json_t *claude_extra_limit(const json_t *payload) {
    const json_t *spend = json_get(payload, "spend");
    double used, limit;
    bool has_limit = false, ok = false;
    if (json_is_null(spend)) {
        const json_t *x = json_get(payload, "extra_usage");
        if (json_is_obj(x) && json_get(x, "is_enabled") && json_is_bool(json_get(x, "is_enabled")) &&
            json_get_bool(x, "is_enabled", false) && json_get(x, "monthly_limit")) {
            json_t *dp = json_get(x, "decimal_places");
            json_t *two = json_int(2);
            if (dollars(json_get(x, "used_credits"), dp ? dp : two, json_get(x, "currency"), false, &used)) {
                json_t *ml = json_get(x, "monthly_limit");
                if (json_is_null(ml)) ok = true;
                else if (dollars(ml, dp ? dp : two, json_get(x, "currency"), false, &limit) && limit > 0) ok = has_limit = true;
            }
            json_free(two);
        }
    } else if (json_is_obj(spend) && json_get_bool(spend, "enabled", false) && json_get(spend, "limit") &&
               json_get_obj(spend, "used")) {
        const json_t *u = json_get_obj(spend, "used");
        if (dollars(json_get(u, "amount_minor"), json_get(u, "exponent"), json_get(u, "currency"), true, &used)) {
            const json_t *l = json_get(spend, "limit");
            if (json_is_null(l)) ok = true;
            else if (json_is_obj(l) && dollars(json_get(l, "amount_minor"), json_get(l, "exponent"), json_get(l, "currency"), true, &limit) &&
                     limit > 0)
                ok = has_limit = true;
        }
    }
    if (!ok) return NULL;
    json_t *a = json_obj();
    json_set_num(a, "used", used);
    json_set_str(a, "unit", "usd");
    const char *status = NULL;
    if (has_limit) {
        json_set_num(a, "limit", limit);
        json_set_num(a, "remaining", MAX(0.0, limit - used));
        json_set_num(a, "usedFraction", used / limit);
        json_set_num(a, "remainingFraction", MAX(0.0, limit - used) / limit);
        status = used >= limit ? "exhausted" : status_of(true, used / limit);
    }
    json_t *s = scope_new("anthropic");
    json_set_str(s, "windowId", "extra");
    return limit_new("anthropic:extra", "Claude Extra Usage", s, NULL, a, status);
}

static const char *payload_str(const json_t *p, const char *k) {
    const char *v = json_get_str(p, k);
    if (!v) return NULL;
    while (*v && isspace((unsigned char)*v)) v++;
    return *v ? v : NULL;
}

static void claude_identity(const json_t *p, const char **acc, const char **email) {
    *acc = payload_str(p, "account_id");
    if (!*acc) *acc = payload_str(p, "accountId");
    if (!*acc) *acc = payload_str(p, "user_id");
    if (!*acc) *acc = payload_str(p, "userId");
    if (!*acc) *acc = payload_str(json_get_obj(p, "account"), "uuid");
    if (!*acc) *acc = payload_str(json_get_obj(p, "account"), "id");
    if (!*acc) *acc = payload_str(json_get_obj(p, "user"), "uuid");
    if (!*acc) *acc = payload_str(json_get_obj(p, "user"), "id");
    *email = payload_str(p, "email");
    if (!*email) *email = payload_str(p, "user_email");
    if (!*email) *email = payload_str(p, "userEmail");
    if (!*email) *email = payload_str(json_get_obj(p, "account"), "email");
    if (!*email) *email = payload_str(json_get_obj(p, "user"), "email");
}

/* parseClaudeUsagePayload */
static json_t *claude_parse(const json_t *payload, const json_t *cred, const char *endpoint) {
    if (!json_is_obj(payload)) return NULL;
    api_vec_t api = {0};
    parse_api_limits(json_get_arr(payload, "limits"), &api);
    bucket_t five, seven, opus, sonnet;
    bool h5 = parse_bucket(json_get(payload, "five_hour"), &five);
    bool h7 = parse_bucket(json_get(payload, "seven_day"), &seven);
    for (size_t i = 0; i < api.n && !h5; i++)
        if (!strcmp(api.v[i].kind, "session")) five = api.v[i].b, h5 = true;
    for (size_t i = 0; i < api.n && !h7; i++)
        if (!strcmp(api.v[i].kind, "weekly_all")) seven = api.v[i].b, h7 = true;
    bool ho = parse_bucket(json_get(payload, "seven_day_opus"), &opus);
    bool hs = parse_bucket(json_get(payload, "seven_day_sonnet"), &sonnet);
    json_t *limits = json_arr(), *l;
    if ((l = claude_limit("anthropic:5h", "Claude 5 Hour", "5h", "5 Hour", 5 * HOUR_MS, h5 ? &five : NULL, NULL, 1))) json_push(limits, l);
    if ((l = claude_limit("anthropic:7d", "Claude 7 Day", "7d", "7 Day", WEEK_MS, h7 ? &seven : NULL, NULL, 1))) json_push(limits, l);
    if ((l = claude_limit("anthropic:7d:opus", "Claude 7 Day (Opus)", "7d", "7 Day", WEEK_MS, ho ? &opus : NULL, "opus", -1)))
        json_push(limits, l);
    if ((l = claude_limit("anthropic:7d:sonnet", "Claude 7 Day (Sonnet)", "7d", "7 Day", WEEK_MS, hs ? &sonnet : NULL, "sonnet", -1)))
        json_push(limits, l);
    strvec_t seen = {0};
    for (size_t i = 0; i < api.n; i++) {
        if (strcmp(api.v[i].kind, "weekly_scoped") || !api.v[i].display) continue;
        char *slug = slugify(api.v[i].display);
        if (*slug && !strvec_contains(&seen, slug)) {
            strvec_push(&seen, slug);
            char *id = xasprintf("anthropic:7d:%s", slug), *label = xasprintf("Claude 7 Day (%s)", api.v[i].display);
            if ((l = claude_limit(id, label, "7d", "7 Day", WEEK_MS, &api.v[i].b, slug, -1))) json_push(limits, l);
            free(id);
            free(label);
        }
        free(slug);
    }
    strvec_free(&seen);
    if ((l = claude_extra_limit(payload))) json_push(limits, l);
    for (size_t i = 0; i < api.n; i++) free(api.v[i].kind), free(api.v[i].display);
    vec_free(&api);
    if (json_len(limits) == 0) {
        json_free(limits);
        return NULL;
    }
    const char *pa, *pe;
    claude_identity(payload, &pa, &pe);
    const char *acc = pa ? pa : cstr(cred, "accountId"), *email = pe ? pe : cstr(cred, "email");
    json_t *r = report_new("anthropic", now_ms());
    json_set(r, "limits", limits);
    json_t *md = json_obj();
    if (endpoint) json_set_str(md, "endpoint", endpoint);
    if (acc) json_set_str(md, "accountId", acc);
    if (email) json_set_str(md, "email", email);
    if (cstr(cred, "orgId")) json_set_str(md, "orgId", cstr(cred, "orgId"));
    json_set(r, "metadata", md);
    json_set(r, "raw", json_clone(payload));
    return r;
}

static bool claude_has_usage(const json_t *p) {
    bucket_t b;
    static const char *const keys[] = {"five_hour", "seven_day", "seven_day_opus", "seven_day_sonnet"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++)
        if (parse_bucket(json_get(p, keys[i]), &b) && b.has_util) return true;
    json_t *e;
    json_arr_foreach(json_get_arr(p, "limits"), i, e) {
        double d;
        if (json_is_obj(e) && json_get_str(e, "kind") && get_num(e, "percent", &d)) return true;
    }
    json_t *x = claude_extra_limit(p);
    bool has = x != NULL;
    json_free(x);
    return has;
}

static bool claude_looks_like_usage(const json_t *p) {
    static const char *const keys[] = {"five_hour", "seven_day", "limits", "extra_usage", "spend", "cedar_ember", "juniper_tide"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++)
        if (json_get(p, keys[i])) return true;
    return false;
}

static const char *claude_token(const json_t *cred) {
    if (str_eq(json_get_str(cred, "type"), "oauth")) return cstr(cred, "accessToken");
    const char *k = cstr(cred, "apiKey");
    return k && strstr(k, "sk-ant-oat") ? k : NULL;
}

static bool claude_supports(const usage_params_t *p) { return claude_token(p->cred) != NULL; }

/* fetchUsagePayload: up to 3 attempts; 429 and absent endpoints are not retried */
static json_t *claude_fetch_payload(const char *url, const http_headers_t *h, const usage_params_t *p, bool *absent,
                                    bool *rejected) {
    json_t *last = NULL;
    *absent = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        http_resp_t r;
        int rc = uhttp("GET", url, h, NULL, p, &r);
        if (rc != 0) {
            http_resp_free(&r);
            if (attempt < 2) sleep_ms(500LL << attempt);
            continue;
        }
        if (r.status < 200 || r.status >= 300) {
            int s = r.status;
            http_resp_free(&r);
            if (s == 404 || s == 405 || s == 410 || s == 501) {
                *absent = true;
                json_free(last);
                return NULL;
            }
            if (s == 401 || s == 403) *rejected = true;
            bool retry = s == 408 || s == 500 || s == 502 || s == 503 || s == 504 || s == 529;
            if (!retry || attempt >= 2) break;
            sleep_ms(500LL << attempt);
            continue;
        }
        bool empty = true;
        for (size_t i = 0; r.body && i < r.body_len; i++)
            if (!isspace((unsigned char)r.body[i])) empty = false;
        if (empty) {
            http_resp_free(&r);
            *absent = true;
            return last;
        }
        json_t *j = resp_json(&r);
        const char *ct = http_headers_get(&r.headers, "content-type");
        bool claims_json = ct && strcasestr(ct, "json");
        http_resp_free(&r);
        if (!j) {
            if (!claims_json) {
                *absent = true;
                return last;
            }
            if (attempt < 2) sleep_ms(500LL << attempt);
            continue;
        }
        if (json_is_obj(j)) {
            json_free(last);
            last = j;
            if (claude_has_usage(j)) {
                *absent = false;
                return last;
            }
            *absent = !claude_looks_like_usage(j);
        } else {
            json_free(j);
            *absent = true;
        }
        if (attempt < 2) sleep_ms(500LL << attempt);
    }
    return last;
}

/* ---------- Claude saved resets (usage/claude-reset.ts: Cedar grants, Juniper session reset) ---------- */

static bool re_match(const char *s, size_t max, bool upper_ok) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > max) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (upper_ok && c >= 'A' && c <= 'Z')))
            return false;
    }
    return true;
}

/* optionalString: 0 absent, 1 value (*out owned), -1 invalid */
static int opt_str(const json_t *v, char **out) {
    *out = NULL;
    if (!v || json_is_null(v)) return 0;
    if (!json_is_str(v)) return -1;
    char *t = str_trim(json_sv(v));
    if (!*t) {
        free(t);
        return -1;
    }
    *out = t;
    return 1;
}

static int opt_iso(const json_t *v, char **out) {
    char *s;
    int r = opt_str(v, &s);
    *out = NULL;
    if (r <= 0) return r;
    double ms;
    bool ok = trace_parse_iso_ms(s, &ms);
    free(s);
    if (!ok) return -1;
    *out = iso8601_from_ms((int64_t)ms);
    return 1;
}

static char *reset_window_id(const json_t *v) {
    if (!json_is_str(v) || !re_match(json_sv(v), 80, false)) return NULL;
    const char *s = json_sv(v);
    if (!strcmp(s, "five_hour")) return xstrdup("anthropic:5h");
    if (!strcmp(s, "seven_day")) return xstrdup("anthropic:7d");
    if (!strcmp(s, "seven_day_opus")) return xstrdup("anthropic:7d:opus");
    if (!strcmp(s, "seven_day_sonnet")) return xstrdup("anthropic:7d:sonnet");
    /* Fable launch resets clear the Fable weekly cap, reported as `seven_day_overage_included` */
    if (!strcmp(s, "seven_day_overage_included")) return xstrdup("anthropic:7d:fable");
    return xasprintf("anthropic:reset:%s", s);
}

static json_t *window_list(const json_t *v) {
    if (!json_is_arr(v)) return NULL;
    json_t *out = json_arr(), *x;
    strvec_t seen = {0};
    json_arr_foreach(v, i, x) {
        char *id = reset_window_id(x);
        if (!id) {
            json_free(out);
            strvec_free(&seen);
            return NULL;
        }
        if (!strvec_contains(&seen, id)) {
            strvec_push(&seen, id);
            json_push_str(out, id);
        }
        free(id);
    }
    strvec_free(&seen);
    return out;
}

static bool is_int(const json_t *v) { return json_is_num(v) && v->u.num.d == floor(v->u.num.d); }

/* parseCedarGrant -> credit-ready grant {id, label?, remainingCount, grantedAt?, expiresAt?, clears, paused, usableNow,
 * requiresLimit, usedFractions, blocking} or NULL */
static json_t *cedar_grant(const json_t *g) {
    if (!json_is_obj(g) || !re_match(json_get_str(g, "id"), 40, false)) return NULL;
    const json_t *left = json_get(g, "resets_left"), *total = json_get(g, "resets_total");
    if (!is_int(left) || left->u.num.d < 0) return NULL;
    if (total && (!is_int(total) || total->u.num.d < 0)) return NULL;
    const json_t *lab = json_get(g, "label");
    char *label = NULL;
    if (lab && !json_is_null(lab)) {
        if (!json_is_str(lab)) return NULL;
        label = str_trim(json_sv(lab));
    }
    char *granted, *expires;
    int rg = opt_iso(json_get(g, "starts_at"), &granted), re = opt_iso(json_get(g, "ends_at"), &expires);
    json_t *clears = window_list(json_get(g, "clears")), *blocking = window_list(json_get(g, "blocking"));
    json_t *used = json_obj();
    bool used_ok = true;
    const json_t *pu = json_get(g, "percent_used");
    if (pu) {
        if (!json_is_obj(pu)) used_ok = false;
        else {
            const char *k;
            json_t *v;
            json_obj_foreach(pu, i, k, v) {
                json_t *ks = json_str(k);
                char *id = reset_window_id(ks);
                json_free(ks);
                if (!id || !is_int(v) || v->u.num.d < 0 || v->u.num.d > 100) {
                    free(id);
                    used_ok = false;
                    break;
                }
                json_set_num(used, id, v->u.num.d / 100);
                free(id);
            }
        }
    }
    const json_t *paused = json_get(g, "paused"), *usable = json_get(g, "usable_now"), *req = json_get(g, "use_requires_limit");
    bool bad = rg < 0 || re < 0 || !clears || !blocking || !used_ok || (paused && !json_is_bool(paused)) ||
               (usable && !json_is_bool(usable)) || (req && !json_is_bool(req));
    if (bad) {
        free(label), free(granted), free(expires);
        json_free(clears), json_free(blocking), json_free(used);
        return NULL;
    }
    json_t *o = json_obj();
    json_set_str(o, "id", json_get_str(g, "id"));
    if (label && *label) json_set_str(o, "label", label);
    json_set_num(o, "remainingCount", left->u.num.d);
    if (granted) json_set_str(o, "grantedAt", granted);
    if (expires) json_set_str(o, "expiresAt", expires);
    json_set(o, "clears", clears);
    json_set_bool(o, "paused", paused ? paused->u.b : false);
    json_set_bool(o, "usableNow", usable ? usable->u.b : false);
    json_set_bool(o, "requiresLimit", req ? req->u.b : true);
    json_set(o, "usedFractions", used);
    json_set(o, "blocking", blocking);
    free(label), free(granted), free(expires);
    return o;
}

/* parseCedarStatus: 1 status (owned), 0 null, -1 malformed */
static int cedar_status(const json_t *v, json_t **out) {
    *out = NULL;
    if (!v || json_is_null(v)) return 0;
    if (!json_is_obj(v) || !json_is_bool(json_get(v, "eligible"))) return -1;
    char *reason, *cooldown;
    int rr = opt_str(json_get(v, "ineligible_reason"), &reason), rc = opt_iso(json_get(v, "cooldown_until"), &cooldown);
    bool bad = rr < 0 || rc < 0 || (json_get(v, "at_limit") && !json_is_bool(json_get(v, "at_limit"))) ||
               (json_get(v, "grants") && !json_is_arr(json_get(v, "grants")));
    json_t *ex = json_get(v, "exhausted") ? window_list(json_get(v, "exhausted")) : NULL;
    if (json_get(v, "exhausted") && !ex) bad = true;
    json_free(ex);
    json_t *grants = json_arr(), *g;
    json_arr_foreach(json_get_arr(v, "grants"), i, g) {
        json_t *x = bad ? NULL : cedar_grant(g);
        if (!x) {
            bad = true;
            break;
        }
        json_push(grants, x);
    }
    const json_t *ng = json_get(v, "next_grant_id");
    if (ng && !json_is_null(ng) && (!json_is_str(ng) || !re_match(json_sv(ng), 40, false))) bad = true;
    if (bad) {
        free(reason), free(cooldown);
        json_free(grants);
        return -1;
    }
    json_t *st = json_obj();
    json_set_bool(st, "eligible", json_get_bool(v, "eligible", false));
    if (reason) json_set_str(st, "reason", reason);
    json_set(st, "grants", grants);
    if (json_is_str(ng)) {
        json_t *x;
        json_arr_foreach(grants, i, x) if (str_eq(json_get_str(x, "id"), json_sv(ng))) {
            json_set_str(st, "nextGrantId", json_sv(ng));
            break;
        }
    }
    if (cooldown) json_set_str(st, "cooldownUntil", cooldown);
    free(reason), free(cooldown);
    *out = st;
    return 1;
}

static int juniper_status(const json_t *v, json_t **out) {
    *out = NULL;
    if (!v || json_is_null(v)) return 0;
    if (!json_is_obj(v) || !json_is_bool(json_get(v, "eligible"))) return -1;
    char *reason, *next, *weekly;
    int a = opt_str(json_get(v, "ineligible_reason"), &reason), b = opt_iso(json_get(v, "next_available_at"), &next),
        c = opt_iso(json_get(v, "weekly_resets_at"), &weekly);
    const json_t *arm = json_get(v, "arm"), *rpw = json_get(v, "resets_per_week");
    bool bad = a < 0 || b < 0 || c < 0 || (json_get(v, "available") && !json_is_bool(json_get(v, "available"))) ||
               (arm && !json_is_null(arm) && !str_eq(json_sv(arm), "control") && !str_eq(json_sv(arm), "reset")) ||
               (rpw && (!is_int(rpw) || rpw->u.num.d < 0));
    if (bad) {
        free(reason), free(next), free(weekly);
        return -1;
    }
    json_t *st = json_obj();
    json_set_bool(st, "eligible", json_get_bool(v, "eligible", false));
    if (reason) json_set_str(st, "reason", reason);
    if (str_eq(json_sv(arm), "control") || str_eq(json_sv(arm), "reset")) json_set_str(st, "arm", json_sv(arm));
    json_set_bool(st, "available", json_get_bool(v, "available", false));
    if (next) json_set_str(st, "nextAvailableAt", next);
    if (weekly) json_set_str(st, "weeklyResetsAt", weekly);
    free(reason), free(next), free(weekly);
    *out = st;
    return 1;
}

static void list_tail(json_t *l, const char *org, const char *base) {
    if (org) json_set_str(l, "orgId", org);
    if (base) json_set_str(l, "baseUrl", base);
}

static json_t *cedar_list(const json_t *st, const char *org, const char *base) {
    double now = (double)now_ms(), cd = 0;
    const char *cu = json_get_str(st, "cooldownUntil"), *next = json_get_str(st, "nextGrantId");
    bool cooldown = cu && trace_parse_iso_ms(cu, &cd) && cd > now, eligible = json_get_bool(st, "eligible", false);
    int64_t avail = 0;
    const json_t *selected = NULL, *g;
    json_t *credits = json_arr();
    json_arr_foreach(json_get_arr(st, "grants"), i, g) {
        double exp;
        bool expired = json_get_str(g, "expiresAt") && trace_parse_iso_ms(json_get_str(g, "expiresAt"), &exp) && exp <= now;
        bool sel = str_eq(json_get_str(g, "id"), next);
        double left = json_get_num(g, "remainingCount", 0);
        bool usable = eligible && sel && json_get_bool(g, "usableNow", false) && !json_get_bool(g, "paused", false) && !expired &&
                      !cooldown && left > 0;
        const char *status = expired ? "expired" : left == 0 ? "redeemed" : json_get_bool(g, "paused", false) ? "paused" : usable ? "available" : "unavailable";
        if (!expired && left > 0) avail += (int64_t)left;
        json_t *c = json_obj();
        json_set_str(c, "id", json_get_str(g, "id"));
        json_set_str(c, "title", json_get_str(g, "label") ? json_get_str(g, "label") : "Claude limit reset");
        json_set_str(c, "program", "cedar_ember");
        json_set_num(c, "remainingCount", left);
        json_set_bool(c, "usable", usable);
        json_set_bool(c, "requiresLimit", json_get_bool(g, "requiresLimit", true));
        json_set(c, "clears", json_clone(json_get(g, "clears")));
        json_set(c, "blocking", json_clone(json_get(g, "blocking")));
        json_set(c, "usedFractions", json_clone(json_get(g, "usedFractions")));
        if (json_get_str(g, "grantedAt")) json_set_str(c, "grantedAt", json_get_str(g, "grantedAt"));
        if (json_get_str(g, "expiresAt")) json_set_str(c, "expiresAt", json_get_str(g, "expiresAt"));
        json_set_str(c, "status", status);
        json_push(credits, c);
        if (sel) selected = c;
    }
    json_t *l = json_obj();
    json_set_int(l, "availableCount", avail);
    json_set_num(l, "redeemableCount", selected && json_get_bool(selected, "usable", false) ? json_get_num(selected, "remainingCount", 0) : 0);
    if (next && eligible) json_set_str(l, "nextCreditId", next);
    json_set_bool(l, "eligible", eligible);
    if (json_get_str(st, "reason")) json_set_str(l, "reason", json_get_str(st, "reason"));
    else if (cooldown) json_set_str(l, "reason", "cooldown");
    if (cu) json_set_str(l, "cooldownUntil", cu);
    json_set(l, "credits", credits);
    list_tail(l, org, base);
    return l;
}

static json_t *juniper_list(const json_t *st, const char *org, const char *base) {
    bool reset_arm = str_eq(json_get_str(st, "arm"), "reset"), st_eligible = json_get_bool(st, "eligible", false);
    bool eligible = st_eligible && reset_arm, usable = eligible && json_get_bool(st, "available", false);
    const char *reason = json_get_str(st, "reason");
    if (!reason) reason = !st_eligible ? "ineligible" : !reset_arm ? "control" : !json_get_bool(st, "available", false) ? "unavailable" : NULL;
    json_t *credits = json_arr();
    if (reset_arm) {
        json_t *c = json_obj();
        json_set_str(c, "id", "juniper_tide");
        json_set_str(c, "title", "Claude session limit reset");
        json_set_str(c, "program", "juniper_tide");
        json_set_int(c, "remainingCount", usable ? 1 : 0);
        json_set_bool(c, "usable", usable);
        json_set_bool(c, "requiresLimit", true);
        json_t *cl = json_arr();
        json_push_str(cl, "anthropic:5h");
        json_set(c, "clears", cl);
        json_set(c, "blocking", json_arr());
        json_set(c, "usedFractions", json_obj());
        if (json_get_str(st, "weeklyResetsAt")) json_set_str(c, "expiresAt", json_get_str(st, "weeklyResetsAt"));
        json_set_str(c, "status", usable ? "available" : "unavailable");
        json_push(credits, c);
    }
    json_t *l = json_obj();
    json_set_int(l, "availableCount", usable ? 1 : 0);
    json_set_int(l, "redeemableCount", usable ? 1 : 0);
    if (eligible) json_set_str(l, "nextCreditId", "juniper_tide");
    json_set_bool(l, "eligible", eligible);
    if (reason) json_set_str(l, "reason", reason);
    if (!usable && json_get_str(st, "nextAvailableAt")) json_set_str(l, "cooldownUntil", json_get_str(st, "nextAvailableAt"));
    json_set(l, "credits", credits);
    list_tail(l, org, base);
    return l;
}

static const char *safe_org(const char *org) {
    static __thread char buf[160];
    if (!org) return NULL;
    char *t = str_trim(org);
    bool ok = re_match(t, 128, true);
    snprintf(buf, sizeof buf, "%s", t);
    free(t);
    return ok ? buf : NULL;
}

/* parseClaudeResetCreditsFromUsagePayload */
static json_t *claude_resets_from_payload(const json_t *payload, const char *org_in, const char *base) {
    if (!json_is_obj(payload)) return NULL;
    const char *org = safe_org(org_in);
    json_t *cedar = NULL, *st;
    if (json_is_obj(json_get(payload, "cedar_ember"))) {
        if (cedar_status(json_get(payload, "cedar_ember"), &st) <= 0) return NULL;
        cedar = cedar_list(st, org, base);
        json_free(st);
        if (json_get_int(cedar, "availableCount", 0) > 0 || json_get_str(cedar, "nextCreditId")) return cedar;
    }
    if (json_is_obj(json_get(payload, "juniper_tide"))) {
        json_free(cedar);
        if (juniper_status(json_get(payload, "juniper_tide"), &st) <= 0) return NULL;
        json_t *l = juniper_list(st, org, base);
        json_free(st);
        return l;
    }
    return cedar;
}

static bool claude_envelope(const json_t *p) {
    static const char *const keys[] = {"five_hour", "seven_day", "limits", "extra_usage", "spend"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++)
        if (json_get(p, keys[i])) return true;
    return false;
}

/* discoverStatus: 1 answered (*status may be NULL; *base_out owned), 0 failed */
static int claude_discover(const char *token, const char *base_url, const char *query, const char *field, bool juniper,
                           json_t **status, char **base_out, int *rate_limited) {
    *status = NULL;
    *base_out = NULL;
    char *api = claude_api_base(base_url), *configured = xasprintf("%s/api/oauth", api);
    free(api);
    const char *cands[2] = {configured, strcmp(configured, CLAUDE_OAUTH_BASE) ? CLAUDE_OAUTH_BASE : NULL};
    http_headers_t h = {0};
    claude_headers(&h, token, "oauth-2025-04-20");
    usage_params_t p = {.timeout_ms = 5000};
    int result = 0;
    for (int i = 0; i < 2 && cands[i]; i++) {
        char *url = xasprintf("%s/usage?%s", cands[i], query);
        http_resp_t r;
        int rc = uhttp("GET", url, &h, NULL, &p, &r);
        free(url);
        if (rc != 0) {
            http_resp_free(&r);
            break;
        }
        if (r.status < 200 || r.status >= 300) {
            if (r.status == 429 && rate_limited) *rate_limited = 1;
            bool absent = r.status == 404 || r.status == 405 || r.status == 410 || r.status == 501;
            http_resp_free(&r);
            if (absent && strcmp(cands[i], CLAUDE_OAUTH_BASE)) continue;
            break;
        }
        json_t *pl = resp_json(&r);
        http_resp_free(&r);
        if (!json_is_obj(pl)) {
            json_free(pl);
            break;
        }
        if (json_get(pl, field)) {
            int ok = juniper ? juniper_status(json_get(pl, field), status) : cedar_status(json_get(pl, field), status);
            json_free(pl);
            if (ok < 0) break;
            *base_out = xstrdup(cands[i]);
            result = 1;
            break;
        }
        bool env = claude_envelope(pl);
        json_free(pl);
        if (env) {
            *base_out = xstrdup(cands[i]);
            result = 1;
            break;
        }
        if (!strcmp(cands[i], CLAUDE_OAUTH_BASE)) break;
    }
    http_headers_free(&h);
    free(configured);
    return result;
}

/* resolveOrganization: owned org id or NULL (*code set) */
static char *claude_org(const char *token, const char *org_in, const char *base_url, const char **code, int *status) {
    *code = NULL;
    *status = 0;
    if (org_in && *org_in) {
        const char *s = safe_org(org_in);
        if (s) return xstrdup(s);
        *code = "organization_unavailable";
        return NULL;
    }
    char *api = claude_api_base(base_url), *url = xasprintf("%s/api/oauth/profile", api);
    free(api);
    http_headers_t h = {0};
    claude_headers(&h, token, "oauth-2025-04-20");
    usage_params_t p = {.timeout_ms = 5000};
    http_resp_t r;
    char *org = NULL;
    if (uhttp("GET", url, &h, NULL, &p, &r) != 0) *code = "organization_unavailable";
    else {
        *status = r.status;
        json_t *pl = resp_json(&r);
        if (r.status < 200 || r.status >= 300) *code = r.status == 401 || r.status == 403 ? "auth_error" : "organization_unavailable";
        else {
            const char *c = json_get_str(json_get_obj(pl, "organization"), "uuid");
            if (!c) c = json_get_str(pl, "organization_uuid");
            const char *s = safe_org(c);
            if (s) org = xstrdup(s);
            else *code = "organization_unavailable";
        }
        json_free(pl);
    }
    http_resp_free(&r);
    http_headers_free(&h);
    free(url);
    return org;
}

json_t *usage_claude_list_resets(const char *token, const char *org_in, const char *base_url, bool *rate_limited) {
    const char *org = safe_org(org_in);
    char *orgc = org ? xstrdup(org) : NULL;
    json_t *st = NULL, *list = NULL;
    char *base = NULL;
    int rl = 0;
    if (!claude_discover(token, base_url, "cedar_ember=1&skip_spend=1", "cedar_ember", false, &st, &base, &rl)) goto done;
    json_t *cedar = NULL;
    if (st) {
        cedar = cedar_list(st, orgc, base);
        json_free(st);
        st = NULL;
        if (json_get_int(cedar, "availableCount", 0) > 0 || json_get_str(cedar, "nextCreditId")) {
            list = cedar;
            goto resolve;
        }
    }
    char *jbase = NULL;
    int answered = claude_discover(token, base_url, "at_wall=1&skip_spend=1", "juniper_tide", true, &st, &jbase, &rl);
    if (answered && st) {
        json_free(cedar);
        list = juniper_list(st, orgc, jbase);
        json_free(st);
    } else if (cedar && answered) list = cedar;
    else if (answered) {
        json_free(cedar);
        list = json_obj();
        json_set_int(list, "availableCount", 0);
        json_set_int(list, "redeemableCount", 0);
        json_set_bool(list, "eligible", false);
        json_set(list, "credits", json_arr());
        if (orgc) json_set_str(list, "orgId", orgc);
        json_set_str(list, "baseUrl", jbase);
    } else json_free(cedar);
    free(jbase);
resolve:
    if (list && !json_get_str(list, "orgId") && json_len(json_get_arr(list, "credits")) > 0) {
        const char *code;
        int status;
        char *o = claude_org(token, NULL, json_get_str(list, "baseUrl") ? json_get_str(list, "baseUrl") : base_url, &code, &status);
        if (o) json_set_str(list, "orgId", o);
        free(o);
    }
done:
    if (rate_limited) *rate_limited = rl != 0;
    free(base);
    free(orgc);
    return list;
}

json_t *usage_claude_consume_reset(const char *token, const char *org_in, const char *base_url, const json_t *credit) {
    json_t *out = json_obj();
    const char *program = json_get_str(credit, "program"), *id = json_get_str(credit, "id");
    bool cedar = str_eq(program, "cedar_ember"), juniper = str_eq(program, "juniper_tide");
#define FAIL(c, st)                                                                                                    \
    do {                                                                                                               \
        json_set_bool(out, "ok", false);                                                                               \
        json_set_str(out, "code", c);                                                                                  \
        json_set_int(out, "status", st);                                                                               \
        return out;                                                                                                    \
    } while (0)
    if (!cedar && !juniper) FAIL("unknown_program", 0);
    double exp, left = json_get_num(credit, "remainingCount", -1);
    const char *es = json_get_str(credit, "expiresAt");
    if (!json_get_bool(credit, "usable", false) || !is_int(json_get(credit, "remainingCount")) || left <= 0 ||
        (es && (!trace_parse_iso_ms(es, &exp) || exp <= (double)now_ms()))) {
        json_set_str(out, "reason", "credit_not_usable");
        FAIL("unavailable", 0);
    }
    if ((cedar && !re_match(id, 40, false)) || (juniper && !str_eq(id, "juniper_tide"))) FAIL("invalid_credit", 0);
    const char *code;
    int ostatus;
    char *org = claude_org(token, org_in, base_url, &code, &ostatus);
    if (!org) FAIL(code ? code : "organization_unavailable", ostatus);
    json_t *body = json_obj();
    if (cedar) {
        json_set_str(body, "program", "cedar_ember");
        json_set_str(body, "grant_id", id);
        char *rid = uuid_v4();
        json_set_str(body, "request_id", rid);
        free(rid);
    } else json_set_str(body, "program", "juniper_tide");
    char *bs = json_dump(body, 0), *api = claude_api_base(base_url), *eo = url_encode(org);
    char *url = xasprintf("%s/api/organizations/%s/reset_rate_limits", api, eo);
    free(api), free(eo), free(org);
    json_free(body);
    http_headers_t h = {0};
    claude_headers(&h, token, "oauth-2025-04-20");
    usage_params_t p = {.timeout_ms = 25000};
    http_resp_t r;
    int rc = uhttp("POST", url, &h, bs, &p, &r);
    free(url), free(bs);
    http_headers_free(&h);
    if (rc != 0) {
        http_resp_free(&r);
        FAIL("network_error", 0);
    }
    int status = r.status;
    json_t *pl = resp_json(&r);
    http_resp_free(&r);
    if (status < 200 || status >= 300) {
        char c[32];
        if (status == 401 || status == 403) snprintf(c, sizeof c, "auth_error");
        else if (status == 429) snprintf(c, sizeof c, "rate_limited");
        else snprintf(c, sizeof c, "http_%d", status);
        json_free(pl);
        FAIL(c, status);
    }
    char *result = NULL, *reason = NULL;
    int rr = json_is_obj(pl) ? opt_str(json_get(pl, "result"), &result) : -1;
    int rs = json_is_obj(pl) ? opt_str(json_get(pl, "reason"), &reason) : 0;
    json_t *cleared = NULL;
    bool malformed = rr <= 0 || rs < 0;
    if (!malformed && json_get(pl, "cleared")) {
        cleared = window_list(json_get(pl, "cleared"));
        if (!cleared) malformed = true;
    } else if (!malformed && !strcmp(result, "reset")) {
        if (juniper) {
            cleared = json_arr();
            json_push_str(cleared, "anthropic:5h");
        } else cleared = json_clone(json_get(credit, "clears"));
    }
    json_free(pl);
    if (malformed) {
        free(result), free(reason);
        json_free(cleared);
        FAIL("malformed_response", status);
    }
    const char *c = !strcmp(result, "already_used") ? "already_redeemed" : !strcmp(result, "not_limited") ? "nothing_to_reset" : result;
    json_set_bool(out, "ok", !strcmp(result, "reset"));
    json_set_str(out, "code", c);
    json_set_int(out, "status", status);
    if (cleared) json_set(out, "cleared", cleared);
    if (reason) json_set_str(out, "reason", reason);
    free(result), free(reason);
    return out;
#undef FAIL
}

/* claudeProfilePlan: "Max 5x" / "Max 20x" from organization.rate_limit_tier, else the organization type (Max, Pro, Team,
 * Enterprise); NULL when the profile names no recognised plan. Owned. */
char *claude_profile_plan(const json_t *profile) {
    json_t *org = json_get_obj(profile, "organization");
    if (!org) return NULL;
    const char *tier = json_get_str(org, "rate_limit_tier");
    for (const char *q = tier; q && (q = str_istr(q, "max_")); q++) {
        const char *d = q + 4;
        size_t n = 0;
        while (isdigit((unsigned char)d[n])) n++;
        if (n && (d[n] == 'x' || d[n] == 'X')) return xasprintf("Max %.*sx", (int)n, d);
    }
    char *type = json_get_str(org, "organization_type") ? str_lower(json_get_str(org, "organization_type")) : NULL;
    static const char *const plans[][2] = {{"claude_max", "Max"}, {"claude_pro", "Pro"}, {"claude_team", "Team"},
                                           {"claude_enterprise", "Enterprise"}};
    char *out = NULL;
    for (size_t i = 0; type && i < ARRAY_LEN(plans) && !out; i++)
        if (!strcmp(type, plans[i][0])) out = xstrdup(plans[i][1]);
    free(type);
    return out;
}

static json_t *claude_fetch(const usage_params_t *p, bool *rejected) {
    const char *token = claude_token(p->cred);
    if (!token) return NULL;
    http_headers_t h = {0};
    claude_headers(&h, token, CLAUDE_USAGE_BETAS);
    char *api = claude_api_base(p->base_url);
    char *configured = xasprintf("%s/api/oauth", api);
    free(api);
    const char *cands[2] = {configured, strcmp(configured, CLAUDE_OAUTH_BASE) ? CLAUDE_OAUTH_BASE : NULL};
    json_t *payload = NULL;
    const char *base = NULL;
    for (int i = 0; i < 2 && cands[i]; i++) {
        char *url = xasprintf("%s/usage", cands[i]);
        bool absent = false;
        json_t *pl = claude_fetch_payload(url, &h, p, &absent, rejected);
        free(url);
        if (pl && claude_has_usage(pl)) {
            json_free(payload);
            payload = pl;
            base = cands[i];
            break;
        }
        if (pl && !payload) payload = pl, base = cands[i];
        else json_free(pl);
        if (!absent) break;
    }
    json_t *report = NULL;
    if (payload && base) {
        char *endpoint = xasprintf("%s/usage", base);
        report = claude_parse(payload, p->cred, endpoint);
        json_t *resets = claude_resets_from_payload(payload, cstr(p->cred, "orgId"), base);
        if (!resets) resets = usage_claude_list_resets(token, cstr(p->cred, "orgId"), p->base_url, NULL);
        bool inventory = resets && (json_get_int(resets, "availableCount", 0) > 0 || json_get_str(resets, "nextCreditId") ||
                                    json_len(json_get_arr(resets, "credits")) > 0);
        if (!report && inventory) {
            report = report_new("anthropic", now_ms());
            json_t *md = json_obj();
            json_set_str(md, "endpoint", endpoint);
            const char *pa, *pe;
            claude_identity(payload, &pa, &pe);
            if (pa) json_set_str(md, "accountId", pa);
            if (pe) json_set_str(md, "email", pe);
            json_set(report, "metadata", md);
            json_set(report, "raw", json_clone(payload));
        }
        free(endpoint);
        if (report) {
            json_t *md = json_get_obj(report, "metadata");
            const char *acc = json_get_str(md, "accountId") ? json_get_str(md, "accountId") : cstr(p->cred, "accountId");
            const char *email = json_get_str(md, "email") ? json_get_str(md, "email") : cstr(p->cred, "email");
            char *pacc = NULL, *pemail = NULL, *plan = NULL;
            /* fetchProfile, every time: the plan can change under a long-lived credential and /usage never reports it;
             * a failed probe keeps the plan the previous report carried (omp d5ea5dccf8) */
            {
                char *url = xasprintf("%s/profile", base);
                http_resp_t r;
                if (uhttp("GET", url, &h, NULL, p, &r) == 0 && r.status >= 200 && r.status < 300) {
                    json_t *prof = resp_json(&r);
                    if (!acc || !email) {
                        const char *u = payload_str(prof, "uuid");
                        if (!u) u = payload_str(json_get_obj(prof, "account"), "uuid");
                        const char *e = payload_str(prof, "email");
                        if (!e) e = payload_str(json_get_obj(prof, "account"), "email");
                        if (u) pacc = str_trim(u);
                        if (e) pemail = str_trim(e);
                    }
                    plan = claude_profile_plan(prof);
                    json_free(prof);
                }
                http_resp_free(&r);
                free(url);
            }
            if (!plan) {
                const char *prev = json_get_str(json_get_obj(p->previous, "metadata"), "subscriptionPlan");
                if (prev) {
                    plan = str_trim(prev);
                    if (!*plan) {
                        free(plan);
                        plan = NULL;
                    }
                }
            }
            if (resets) {
                json_t *rc = json_clone(resets);
                const char *org = json_get_str(resets, "orgId");
                json_del(rc, "orgId");
                json_del(rc, "baseUrl");
                json_del(rc, "report");
                json_set(report, "resetCredits", rc);
                if (org) json_set_str(md, "orgId", org);
            } else if (json_get_obj(p->previous, "resetCredits")) {
                json_set(report, "resetCredits", json_clone(json_get_obj(p->previous, "resetCredits")));
            }
            char *a2 = acc ? xstrdup(acc) : pacc ? xstrdup(pacc) : NULL, *e2 = email ? xstrdup(email) : pemail ? xstrdup(pemail) : NULL;
            if (a2) json_set_str(md, "accountId", a2);
            if (e2) json_set_str(md, "email", e2);
            if (cstr(p->cred, "orgId")) json_set_str(md, "orgId", cstr(p->cred, "orgId"));
            if (plan) json_set_str(md, "subscriptionPlan", plan);
            free(a2), free(e2), free(pacc), free(pemail), free(plan);
        }
        json_free(resets);
    }
    json_free(payload);
    free(configured);
    http_headers_free(&h);
    return report;
}

const usage_provider_t usage_provider_anthropic = {"anthropic", claude_supports, claude_fetch, 60000};

/* ============================================================================================
 * OpenAI Codex (usage/openai-codex.ts, openai-codex-base-url.ts, openai-codex-reset.ts)
 * ============================================================================================ */

/* normalizeCodexBaseUrl: only the canonical ChatGPT origins are honored */
char *usage_codex_base_url(const char *base) {
    char *t = base ? str_trim(base) : xstrdup("");
    size_t n = strlen(t);
    while (n > 0 && t[n - 1] == '/') t[--n] = 0;
    url_t u;
    if (!*t || url_parse(t, &u) != 0) {
        free(t);
        return xstrdup(CODEX_BASE_URL);
    }
    free(t);
    char *host = str_lower(u.host);
    char *r = NULL;
    if (!strcmp(host, "chatgpt.com") || !strcmp(host, "chat.openai.com")) {
        bool def = (!strcmp(u.scheme, "https") && u.port == 443) || (!strcmp(u.scheme, "http") && u.port == 80);
        r = def ? xasprintf("%s://%s/backend-api", u.scheme, host) : xasprintf("%s://%s:%d/backend-api", u.scheme, host, u.port);
    }
    free(host);
    url_free(&u);
    return r ? r : xstrdup(CODEX_BASE_URL);
}

static char *jwt_claim(const char *token, const char *claim, const char *key) {
    json_t *p = token ? auth_jwt_payload(token) : NULL;
    const char *v = json_get_str(json_get_obj(p, claim), key);
    char *r = v ? xstrdup(v) : NULL;
    json_free(p);
    return r;
}

static char *norm_email(const char *e) {
    if (!e) return NULL;
    char *t = str_trim(e), *l = str_lower(t);
    free(t);
    if (!*l) {
        free(l);
        return NULL;
    }
    return l;
}

typedef struct {
    bool has_used, has_win, has_after, has_at;
    double used, win, after, at;
} cwin_t;

static bool parse_cwin(const json_t *o, cwin_t *w) {
    memset(w, 0, sizeof *w);
    if (!json_is_obj(o)) return false;
    w->has_used = get_num(o, "used_percent", &w->used);
    w->has_win = get_num(o, "limit_window_seconds", &w->win);
    w->has_after = get_num(o, "reset_after_seconds", &w->after);
    w->has_at = get_num(o, "reset_at", &w->at);
    return w->has_used || w->has_win || w->has_after || w->has_at;
}

static bool reset_time(const cwin_t *w, double now, double *out) {
    if (w->has_at) {
        double ms = w->at > 1e12 ? w->at : w->at * 1000;
        if (isfinite(ms)) return *out = ms, true;
    }
    if (w->has_after) return *out = now + w->after * 1000, true;
    return false;
}

static json_t *codex_window(const cwin_t *w, const char *key, double now) {
    double ra;
    bool has_ra = reset_time(w, now, &ra);
    json_t *win;
    if (w->has_win) {
        char id[32], label[32];
        if (w->win >= 86400) {
            long d = (long)floor(w->win / 86400 + 0.5);
            snprintf(id, sizeof id, "%ldd", d);
            snprintf(label, sizeof label, "%ld %s", d, d == 1 ? "day" : "days");
        } else {
            long h = MAX(1L, (long)floor(w->win / 3600 + 0.5));
            snprintf(id, sizeof id, "%ldh", h);
            snprintf(label, sizeof label, "%ld %s", h, h == 1 ? "hour" : "hours");
        }
        win = window_new(id, label);
        json_set_num(win, "durationMs", w->win * 1000);
    } else win = window_new(key, !strcmp(key, "primary") ? "Primary window" : "Secondary window");
    if (has_ra) json_set_num(win, "resetsAt", ra);
    return win;
}

static json_t *codex_amount(const cwin_t *w) {
    if (!w->has_used) {
        json_t *a = json_obj();
        json_set_str(a, "unit", "percent");
        return a;
    }
    return percent_amount(w->used);
}

/* 0 = undefined, 1 = false, 2 = true */
static int tri(const json_t *o, const char *k) {
    json_t *v = json_get(o, k);
    return json_is_bool(v) ? (v->u.b ? 2 : 1) : 0;
}

static bool codex_allowed(int allowed, int reached, bool credit_overage) {
    if (credit_overage && reached == 2) return true;
    return allowed == 2 && reached == 1;
}

static const char *codex_status(const json_t *amount, bool explicitly_allowed) {
    double f;
    bool has = json_is_num(json_get(amount, "usedFraction"));
    f = json_get_num(amount, "usedFraction", 0);
    const char *s = status_of(has, f);
    return !strcmp(s, "exhausted") && explicitly_allowed ? "warning" : s;
}

static json_t *codex_plain_limit(const char *key, const cwin_t *w, int allowed, int reached, bool overage, double now) {
    json_t *win = codex_window(w, key, now), *amount = codex_amount(w);
    char *id = xasprintf("openai-codex:%s", key);
    json_t *s = scope_new("openai-codex");
    json_set_str(s, "windowId", json_get_str(win, "id"));
    json_set_bool(s, "shared", true);
    json_t *l = limit_new(id, json_get_str(win, "label"), s, win, amount, codex_status(amount, codex_allowed(allowed, reached, overage)));
    free(id);
    return l;
}

static char *codex_extra_slug(const char *name, const char *feature) {
    char *probe = xasprintf("%s %s", name ? name : "", feature ? feature : "");
    char *lp = str_lower(probe);
    free(probe);
    bool spark = strstr(lp, "spark") || strstr(lp, "bengalfox");
    free(lp);
    if (spark) return xstrdup("spark");
    char *src = str_lower(feature ? feature : name ? name : "extra");
    const char *s = src;
    if (!strncmp(s, "codex-", 6) || !strncmp(s, "codex_", 6)) s += 6;
    char *slug = slugify(s);
    free(src);
    if (!*slug) {
        free(slug);
        return xstrdup("extra");
    }
    return slug;
}

static char *codex_extra_name(const char *slug, const char *name) {
    if (!strcmp(slug, "spark")) return xstrdup("Spark");
    if (name) return xstrdup(name);
    sb_t b = SB_INIT;
    bool up = true;
    for (const char *p = slug; *p; p++) {
        if (*p == '-') {
            sb_putc(&b, ' ');
            up = true;
            continue;
        }
        sb_putc(&b, up && *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p);
        up = false;
    }
    return sb_detach(&b);
}

static json_t *codex_extra_limit(const char *key, const char *slug, const char *display, const cwin_t *w, const char *acc,
                                 const char *name, const char *feature, int allowed, int reached, double now) {
    json_t *win = codex_window(w, key, now), *amount = codex_amount(w);
    char *id = xasprintf("openai-codex:%s:%s", slug, key);
    char *label = xasprintf("%s (%s)", json_get_str(win, "label"), display);
    json_t *s = scope_new("openai-codex");
    if (acc) json_set_str(s, "accountId", acc);
    json_set_str(s, "tier", slug);
    /* the meter is per model even when the payload names only its feature codename (omp 98c0a96770) */
    if (name) json_set_str(s, "modelId", name);
    else if (feature) json_set_str(s, "modelId", feature);
    json_set_str(s, "windowId", json_get_str(win, "id"));
    json_set_bool(s, "shared", true);
    json_t *l = limit_new(id, label, s, win, amount, codex_status(amount, codex_allowed(allowed, reached, false)));
    free(id);
    free(label);
    return l;
}

static void codex_headers(http_headers_t *h, const char *token, const char *acc, bool json_body) {
    char *a = xasprintf("Bearer %s", token);
    http_headers_set(h, "Authorization", a);
    free(a);
    http_headers_set(h, "User-Agent", OMP_USER_AGENT);
    if (acc) http_headers_set(h, "ChatGPT-Account-Id", acc);
    if (json_body) http_headers_set(h, "Content-Type", "application/json");
}

/* listCodexResetCredits: owned {credits: [...], availableCount} or NULL */
json_t *usage_codex_list_resets(const char *token, const char *acc, const char *base_url, int64_t timeout_ms) {
    char *base = usage_codex_base_url(base_url);
    char *url = xasprintf("%s/wham/rate-limit-reset-credits", base);
    http_headers_t h = {0};
    codex_headers(&h, token, acc, false);
    usage_params_t p = {.timeout_ms = timeout_ms};
    http_resp_t r;
    json_t *payload = NULL;
    if (uhttp("GET", url, &h, NULL, &p, &r) == 0 && r.status >= 200 && r.status < 300) payload = resp_json(&r);
    http_resp_free(&r);
    http_headers_free(&h);
    free(url);
    free(base);
    if (!json_is_obj(payload)) {
        json_free(payload);
        return NULL;
    }
    json_t *out = json_obj(), *credits = json_arr(), *c;
    int64_t avail_count = 0;
    json_arr_foreach(json_get_arr(payload, "credits"), i, c) {
        if (!json_is_obj(c) || !json_get_str(c, "id")) continue;
        json_t *x = json_obj();
        json_set_str(x, "id", json_get_str(c, "id"));
        static const char *const map[][2] = {{"reset_type", "resetType"}, {"status", "status"}, {"granted_at", "grantedAt"},
                                             {"expires_at", "expiresAt"}, {"title", "title"}, {"description", "description"}};
        for (size_t k = 0; k < ARRAY_LEN(map); k++)
            if (json_get_str(c, map[k][0])) json_set_str(x, map[k][1], json_get_str(c, map[k][0]));
        if (!json_get_str(x, "status") || str_eq(json_get_str(x, "status"), "available")) avail_count++;
        json_push(credits, x);
    }
    double reported;
    json_set(out, "credits", credits);
    json_set_int(out, "availableCount", get_num(payload, "available_count", &reported) ? MAX(0, (int64_t)trunc(reported)) : avail_count);
    json_free(payload);
    return out;
}

/* consumeCodexResetCredit: owned {ok, code, status} */
json_t *usage_codex_consume_reset(const char *token, const char *acc, const char *base_url, const char *credit_id) {
    char *base = usage_codex_base_url(base_url);
    char *url = xasprintf("%s/wham/rate-limit-reset-credits/consume", base);
    http_headers_t h = {0};
    codex_headers(&h, token, acc, true);
    json_t *body = json_obj();
    json_set_str(body, "credit_id", credit_id);
    char *rid = uuid_v4();
    json_set_str(body, "redeem_request_id", rid);
    free(rid);
    if (acc) json_set_str(body, "account_id", acc);
    char *bs = json_dump(body, 0);
    usage_params_t p = {.timeout_ms = 15000};
    http_resp_t r;
    int rc = uhttp("POST", url, &h, bs, &p, &r);
    json_t *out = json_obj();
    if (rc != 0) {
        json_set_bool(out, "ok", false);
        json_set_str(out, "code", "network_error");
        json_set_int(out, "status", 0);
    } else {
        json_t *j = resp_json(&r);
        char *code = json_get_str(j, "code") ? xstrdup(json_get_str(j, "code"))
                     : r.status >= 200 && r.status < 300 ? xstrdup("reset") : xasprintf("http_%d", r.status);
        json_set_bool(out, "ok", !strcmp(code, "reset"));
        json_set_str(out, "code", code);
        json_set_int(out, "status", r.status);
        free(code);
        json_free(j);
    }
    http_resp_free(&r);
    free(bs);
    json_free(body);
    http_headers_free(&h);
    free(url);
    free(base);
    return out;
}

static bool codex_supports(const usage_params_t *p) { return str_eq(json_get_str(p->cred, "type"), "oauth"); }

static bool daybreak_access(const json_t *payload) {
    json_t *pr;
    json_arr_foreach(json_get_arr(payload, "programs"), i, pr) {
        if (!json_is_obj(pr) || !str_eq(json_get_str(pr, "program"), "cyber")) continue;
        if (!str_eq(json_get_str(pr, "state"), "inactive") || json_len(json_get_arr(pr, "grants")) > 0) return true;
    }
    return false;
}

static json_t *codex_fetch(const usage_params_t *p, bool *rejected) {
    const char *token = cstr(p->cred, "accessToken");
    if (!token) return NULL;
    double now = (double)now_ms();
    double exp;
    if (get_num(p->cred, "expiresAt", &exp) && exp <= now) return NULL;
    char *base = usage_codex_base_url(p->base_url);
    char *acc = cstr(p->cred, "accountId") ? xstrdup(cstr(p->cred, "accountId")) : jwt_claim(token, "https://api.openai.com/auth", "chatgpt_account_id");
    char *jwt_email = cstr(p->cred, "email") ? NULL : jwt_claim(token, "https://api.openai.com/profile", "email");
    char *email = norm_email(cstr(p->cred, "email") ? cstr(p->cred, "email") : jwt_email);
    free(jwt_email);
    http_headers_t h = {0};
    codex_headers(&h, token, acc, false);
    char *url = xasprintf("%s/wham/usage", base);
    http_resp_t r;
    json_t *payload = NULL;
    int rc = uhttp("GET", url, &h, NULL, p, &r);
    free(url);
    if (rc == 0 && (r.status == 401 || r.status == 403)) *rejected = true;
    if (rc == 0 && r.status >= 200 && r.status < 300) payload = resp_json(&r);
    http_resp_free(&r);
    json_t *report = NULL;
    if (payload) {
        /* parseUsagePayload */
        const char *plan = json_get_str(payload, "plan_type");
        const json_t *rl = json_get_obj(payload, "rate_limit");
        cwin_t prim, sec;
        bool hp = rl && parse_cwin(json_get(rl, "primary_window"), &prim);
        bool hs = rl && parse_cwin(json_get(rl, "secondary_window"), &sec);
        int allowed = rl ? tri(rl, "allowed") : 0, reached = rl ? tri(rl, "limit_reached") : 0;
        /* spend cap */
        const json_t *sc = json_get_obj(payload, "spend_control"), *ind = json_get_obj(sc, "individual_limit");
        bool has_cap = false, cu_h = false, cl_h = false, cr_h = false, cf_h = false, ca_h = false, caf_h = false;
        double cu = 0, cl = 0, crem = 0, cf = 0, ca = 0, caf = 0, cp;
        int cap_reached = 0;
        if (sc && ind) {
            cu_h = get_num(ind, "used", &cu);
            cl_h = get_num(ind, "limit", &cl);
            cr_h = get_num(ind, "remaining", &crem);
            bool hpct = get_num(ind, "used_percent", &cp);
            if (cu_h && cl_h && cl > 0) cf = cu / cl, cf_h = true;
            else if (hpct) cf = cp / 100, cf_h = true;
            else if (cr_h && crem <= 0) cf = 1, cf_h = true;
            if (cu_h || cl_h || cr_h || cf_h) {
                has_cap = true;
                if (cf_h) cf = MIN(MAX(cf, 0.0), 1.0);
                cap_reached = tri(sc, "reached");
                ca_h = get_num(ind, "reset_at", &ca);
                caf_h = get_num(ind, "reset_after_seconds", &caf);
            }
        }
        /* additional meters */
        json_t *extras = json_arr(), *x;
        json_arr_foreach(json_get_arr(payload, "additional_rate_limits"), i, x) {
            const json_t *xr = json_get_obj(x, "rate_limit");
            if (!xr) continue;
            cwin_t a, b;
            bool ha = parse_cwin(json_get(xr, "primary_window"), &a), hb = parse_cwin(json_get(xr, "secondary_window"), &b);
            if (!ha && !hb && !tri(xr, "allowed") && !tri(xr, "limit_reached")) continue;
            json_push(extras, json_clone(x));
        }
        if (rl || json_len(extras) > 0 || has_cap) {
            if (!rl && has_cap && cap_reached) allowed = cap_reached == 1 ? 2 : 1, reached = cap_reached;
            bool overage = false;
            if (rl && tri(rl, "limit_reached") == 2) {
                const json_t *cr = json_get_obj(payload, "credits");
                if (cr && (json_get_bool(cr, "unlimited", false) || json_get_bool(cr, "has_credits", false)) &&
                    !json_get_bool(cr, "overage_limit_reached", false) && !(sc && json_get_bool(sc, "reached", false)) &&
                    !(has_cap && cap_reached == 0 && cf_h && cf >= 1))
                    overage = true;
            }
            report = report_new("openai-codex", (int64_t)now);
            json_t *limits = json_get_arr(report, "limits");
            json_t *meters = json_obj(), *chat = json_obj();
            if (overage) {
                json_set_bool(chat, "allowed", true);
                json_set_bool(chat, "limitReached", false);
            } else {
                if (allowed) json_set_bool(chat, "allowed", allowed == 2);
                if (reached) json_set_bool(chat, "limitReached", reached == 2);
            }
            json_set(meters, "chat", chat);
            if (hp) json_push(limits, codex_plain_limit("primary", &prim, allowed, reached, overage, now));
            if (hs) json_push(limits, codex_plain_limit("secondary", &sec, allowed, reached, overage, now));
            json_arr_foreach(extras, i, x) {
                const char *name = json_get_str(x, "limit_name"), *feat = json_get_str(x, "metered_feature");
                const json_t *xr = json_get_obj(x, "rate_limit");
                char *slug = codex_extra_slug(name, feat), *disp = codex_extra_name(slug, name);
                int xa = tri(xr, "allowed"), xre = tri(xr, "limit_reached");
                json_t *ms = json_obj();
                if (xa) json_set_bool(ms, "allowed", xa == 2);
                if (xre) json_set_bool(ms, "limitReached", xre == 2);
                json_set(meters, slug, ms);
                cwin_t a, b;
                if (parse_cwin(json_get(xr, "primary_window"), &a))
                    json_push(limits, codex_extra_limit("primary", slug, disp, &a, acc, name, feat, xa, xre, now));
                if (parse_cwin(json_get(xr, "secondary_window"), &b))
                    json_push(limits, codex_extra_limit("secondary", slug, disp, &b, acc, name, feat, xa, xre, now));
                free(slug);
                free(disp);
            }
            if (has_cap) {
                cwin_t rw = {.has_at = ca_h, .at = ca, .has_after = caf_h, .after = caf};
                double ra;
                bool has_ra = reset_time(&rw, now, &ra);
                json_t *amount = counter_amount(cu_h, cu, cl_h, cl, cr_h, crem, cf_h, cf, "credits");
                json_t *s = scope_new("openai-codex");
                if (acc) json_set_str(s, "accountId", acc);
                json_set_str(s, "tier", "credits");
                json_set_str(s, "windowId", "spend-cap");
                json_set_bool(s, "shared", true);
                json_t *w = window_new("spend-cap", "Spend cap");
                if (has_ra) json_set_num(w, "resetsAt", ra);
                const char *st = cap_reached == 2 ? "exhausted" : cap_reached == 1 ? codex_status(amount, true) : status_of(cf_h, cf);
                json_push(limits, limit_new("openai-codex:spend-cap", "Spend cap", s, w, amount, st));
            }
            json_t *md = json_obj();
            if (plan) json_set_str(md, "planType", plan);
            if (json_get(chat, "allowed")) json_set(md, "allowed", json_clone(json_get(chat, "allowed")));
            if (json_get(chat, "limitReached")) json_set(md, "limitReached", json_clone(json_get(chat, "limitReached")));
            if (email) json_set_str(md, "email", email);
            if (acc) json_set_str(md, "accountId", acc);
            json_set(md, "meterStates", meters);
            json_set(report, "metadata", md);
        } else {
            report = report_new("openai-codex", (int64_t)now);
            json_t *md = json_obj();
            if (plan) json_set_str(md, "planType", plan);
            if (email) json_set_str(md, "email", email);
            if (acc) json_set_str(md, "accountId", acc);
            json_t *meters = json_obj();
            json_set(meters, "chat", json_obj());
            json_set(md, "meterStates", meters);
            json_set(report, "metadata", md);
        }
        json_free(extras);
        /* saved resets */
        const json_t *rcb = json_get_obj(payload, "rate_limit_reset_credits");
        double ac;
        if (rcb && get_num(rcb, "available_count", &ac)) {
            json_t *rcj = json_obj();
            json_set_int(rcj, "availableCount", MAX(0, (int64_t)trunc(ac)));
            if (ac > 0) {
                json_t *list = usage_codex_list_resets(token, acc, p->base_url, p->timeout_ms);
                if (list) {
                    json_t *credits = json_arr(), *c;
                    json_arr_foreach(json_get_arr(list, "credits"), i, c) {
                        const char *st = json_get_str(c, "status");
                        if (st && strcmp(st, "available")) continue;
                        json_t *d = json_obj();
                        if (json_get_str(c, "grantedAt")) json_set_str(d, "grantedAt", json_get_str(c, "grantedAt"));
                        if (json_get_str(c, "expiresAt")) json_set_str(d, "expiresAt", json_get_str(c, "expiresAt"));
                        if (st) json_set_str(d, "status", st);
                        json_push(credits, d);
                    }
                    if (json_len(credits)) json_set(rcj, "credits", credits);
                    else json_free(credits);
                    json_set_int(rcj, "availableCount", json_get_int(list, "availableCount", 0));
                    json_free(list);
                }
            }
            /* resetCredits sits between limits and metadata */
            json_t *md = json_take(report, "metadata");
            json_set(report, "resetCredits", rcj);
            json_set(report, "metadata", md);
        }
        /* daybreak badge (accounts/verified_access) */
        char *vurl = xasprintf("%s/accounts/verified_access", base);
        http_resp_t vr;
        if (uhttp("GET", vurl, &h, NULL, p, &vr) == 0 && vr.status >= 200 && vr.status < 300) {
            json_t *vj = resp_json(&vr);
            if (daybreak_access(vj)) json_set_bool(json_get_obj(report, "metadata"), "daybreak", true);
            json_free(vj);
        }
        http_resp_free(&vr);
        free(vurl);
        json_set(report, "raw", payload);
        payload = NULL;
    }
    json_free(payload);
    http_headers_free(&h);
    free(base);
    free(acc);
    free(email);
    return report;
}

const usage_provider_t usage_provider_codex = {"openai-codex", codex_supports, codex_fetch, 0};

/* ============================================================================================
 * GitHub Copilot (usage/github-copilot.ts)
 * ============================================================================================ */

static char *copilot_api_base(const usage_params_t *p) {
    if (p->base_url && *p->base_url && !strstr(p->base_url, "githubcopilot.com")) {
        char *b = xstrdup(p->base_url);
        size_t n = strlen(b);
        if (n && b[n - 1] == '/') b[n - 1] = 0;
        return b;
    }
    const char *ent = cstr(p->cred, "enterpriseUrl");
    char *e = ent ? str_trim(ent) : NULL;
    char *r;
    if (!e || !*e) r = xstrdup("https://api.github.com");
    else if (str_starts(e, "http://") || str_starts(e, "https://")) {
        r = xstrdup(e);
        size_t n = strlen(r);
        if (n && r[n - 1] == '/') r[n - 1] = 0;
    } else if (str_starts(e, "api.")) r = xasprintf("https://%s", e);
    else r = xasprintf("https://api.%s", e);
    free(e);
    return r;
}

/* copilot buildAmount: key order used, limit, remaining, usedFraction, remainingFraction, unit (undefined dropped) */
static json_t *copilot_amount(bool hu, double used, bool hl, double limit, const char *unit) {
    json_t *a = json_obj();
    if (hu) json_set_num(a, "used", used);
    if (hl) json_set_num(a, "limit", limit);
    if (hu && hl) {
        double rem = MAX(0.0, limit - used);
        json_set_num(a, "remaining", rem);
        if (limit > 0) {
            json_set_num(a, "usedFraction", used / limit);
            json_set_num(a, "remainingFraction", rem / limit);
        }
    }
    json_set_str(a, "unit", unit);
    return a;
}

static const char *copilot_status(const json_t *amount, bool unlimited) {
    if (unlimited) return "ok";
    json_t *rf = json_get(amount, "remainingFraction");
    if (!json_is_num(rf)) return "unknown";
    if (rf->u.num.d <= 0) return "exhausted";
    if (rf->u.num.d <= 0.1) return "warning";
    return "ok";
}

static int copilot_get(const char *url, const char *token, bool github_api, const usage_params_t *p, json_t **out, int *status) {
    http_headers_t h = {0};
    char *a = xasprintf("Bearer %s", token);
    if (github_api) {
        http_headers_set(&h, "Accept", "application/vnd.github+json");
        http_headers_set(&h, "Authorization", a);
        http_headers_set(&h, "X-GitHub-Api-Version", "2022-11-28");
    } else {
        http_headers_set(&h, "Content-Type", "application/json");
        http_headers_set(&h, "Accept", "application/json");
        http_headers_set(&h, "Authorization", a);
        http_headers_set(&h, "User-Agent", COPILOT_CLI_UA);
    }
    free(a);
    http_resp_t r;
    int rc = uhttp("GET", url, &h, NULL, p, &r);
    *status = rc == 0 ? r.status : 0;
    *out = rc == 0 && r.status >= 200 && r.status < 300 ? resp_json(&r) : NULL;
    http_resp_free(&r);
    http_headers_free(&h);
    return *out ? 0 : -1;
}

static json_t *copilot_quota_limit(const char *key, const char *label, const json_t *q, const char *plan, const json_t *window,
                                   const char *acc) {
    double ent, rem, pct;
    if (!get_num(q, "entitlement", &ent) || !get_num(q, "remaining", &rem) || !get_num(q, "percent_remaining", &pct) ||
        !json_is_bool(json_get(q, "unlimited")))
        return NULL;
    bool unlimited = json_get_bool(q, "unlimited", false);
    double over = 0;
    get_num(q, "overage_count", &over);
    json_t *amount = unlimited ? copilot_amount(false, 0, false, 0, "requests")
                               : copilot_amount(true, MAX(0.0, ent - rem), true, ent, "requests");
    char *id = xasprintf("copilot:%s", key);
    json_t *s = scope_new("github-copilot");
    if (acc) json_set_str(s, "accountId", acc);
    if (plan) json_set_str(s, "tier", plan);
    if (window) json_set_str(s, "windowId", json_get_str(window, "id"));
    json_t *l = limit_new(id, label, s, window ? json_clone(window) : NULL, amount, copilot_status(amount, unlimited));
    free(id);
    json_t *notes = json_arr();
    if (unlimited) json_push_str(notes, "Unlimited");
    if (over > 0) {
        char *n = xasprintf("Overage requests: %g", over);
        json_push_str(notes, n);
        free(n);
    }
    if (json_len(notes)) json_set(l, "notes", notes);
    else json_free(notes);
    return l;
}

static json_t *copilot_from_internal(const json_t *data, const char *acc, const char *email, bool api_key) {
    json_t *window = NULL;
    double ra;
    if (iso_ms(json_get_str(data, "quota_reset_date"), &ra)) {
        window = window_new("monthly", "Monthly");
        json_set_num(window, "resetsAt", ra);
    }
    const json_t *snaps = json_get_obj(data, "quota_snapshots");
    const char *plan = json_get_str(data, "copilot_plan");
    json_t *r = report_new("github-copilot", now_ms());
    json_t *limits = json_get_arr(r, "limits"), *l;
    if ((l = copilot_quota_limit("premium", "Premium Requests", json_get_obj(snaps, "premium_interactions"), plan, window, acc))) json_push(limits, l);
    const json_t *chat = json_get_obj(snaps, "chat");
    if (chat && !json_get_bool(chat, "unlimited", false) && (l = copilot_quota_limit("chat", "Chat Requests", chat, plan, window, acc)))
        json_push(limits, l);
    const json_t *comp = json_get_obj(snaps, "completions");
    if (comp && !json_get_bool(comp, "unlimited", false) && (l = copilot_quota_limit("completions", "Completions", comp, plan, window, acc)))
        json_push(limits, l);
    json_free(window);
    json_t *md = json_obj();
    if (acc) json_set_str(md, "accountId", acc);
    if (!api_key && email) json_set_str(md, "email", email);
    if (plan) json_set_str(md, "plan", plan);
    if (json_get_str(data, "quota_reset_date")) json_set_str(md, "quotaResetDate", json_get_str(data, "quota_reset_date"));
    json_set(r, "metadata", md);
    json_set(r, "raw", json_clone(data));
    return r;
}

static char *copilot_username(const char *base, const char *token, const usage_params_t *p, int *status) {
    char *url = xasprintf("%s/user", base);
    json_t *j;
    copilot_get(url, token, true, p, &j, status);
    free(url);
    char *login = json_get_str(j, "login") ? xstrdup(json_get_str(j, "login")) : NULL;
    json_free(j);
    return login;
}

static bool copilot_supports(const usage_params_t *p) {
    if (str_eq(json_get_str(p->cred, "type"), "oauth")) return cstr(p->cred, "refreshToken") || cstr(p->cred, "accessToken");
    return cstr(p->cred, "apiKey") != NULL;
}

static bool auth_status(int s) { return s == 401 || s == 403; }

static json_t *copilot_fetch(const usage_params_t *p, bool *rejected) {
    if (!copilot_supports(p)) return NULL;
    char *base = copilot_api_base(p);
    json_t *report = NULL;
    int failures = 0, auth_failures = 0, st = 0;
    if (str_eq(json_get_str(p->cred, "type"), "api_key")) {
        const char *key = cstr(p->cred, "apiKey");
        char *user = cstr(p->cred, "accountId") ? xstrdup(cstr(p->cred, "accountId")) : NULL;
        if (!user) {
            user = copilot_username(base, key, p, &st);
            if (!user) failures++, auth_failures += auth_status(st);
        }
        if (user) {
            char *eu = url_encode(user);
            char *url = xasprintf("%s/users/%s/settings/billing/premium_request/usage", base, eu);
            free(eu);
            json_t *billing;
            if (copilot_get(url, key, true, p, &billing, &st) == 0 && json_is_obj(billing)) {
                const json_t *tp = json_get_obj(billing, "timePeriod");
                char period[32];
                double month;
                if (get_num(tp, "month", &month) && month)
                    snprintf(period, sizeof period, "%lld-%02d", (long long)json_get_int(tp, "year", 0), (int)month);
                else snprintf(period, sizeof period, "%lld", (long long)json_get_int(tp, "year", 0));
                json_t *window = window_new("monthly", period);
                const json_t *items = json_get_arr(billing, "usageItems");
                const json_t *it;
                double total_used = 0, total_limit = 0;
                const char *unit_type = NULL;
                bool unit_set = false;
                json_arr_foreach(items, i, it) {
                    const char *sku = json_get_str(it, "sku");
                    if (!sku || !(str_eq(sku, "Copilot Premium Request") || strstr(sku, "Premium"))) continue;
                    if (!unit_set && json_get(it, "unitType")) unit_type = json_get_str(it, "unitType"), unit_set = true;
                    total_used += json_get_num(it, "grossQuantity", 0);
                    total_limit += json_get_num(it, "limit", 0);
                }
                bool credits = false;
                if (unit_type) {
                    char *up = str_lower(unit_type);
                    credits = strstr(up, "credit") != NULL;
                    free(up);
                }
                report = report_new("github-copilot", now_ms());
                json_t *limits = json_get_arr(report, "limits");
                json_t *amount = copilot_amount(true, total_used, total_limit != 0, total_limit, credits ? "credits" : "requests");
                json_t *s = scope_new("github-copilot");
                if (json_get_str(billing, "user")) json_set_str(s, "accountId", json_get_str(billing, "user"));
                json_set_str(s, "windowId", "monthly");
                json_push(limits, limit_new("copilot:premium", credits ? "AI Credits" : "Premium Requests", s, json_clone(window), amount,
                                            copilot_status(amount, false)));
                json_arr_foreach(items, i, it) {
                    const char *model = json_get_str(it, "model");
                    double gq = json_get_num(it, "grossQuantity", 0), lim;
                    if (!model || gq <= 0) continue;
                    bool hl = get_num(it, "limit", &lim);
                    const char *ut = json_get_str(it, "unitType");
                    bool ic = false;
                    if (ut) {
                        char *lo = str_lower(ut);
                        ic = strstr(lo, "credit") != NULL;
                        free(lo);
                    }
                    json_t *am = copilot_amount(true, gq, hl, lim, ic ? "credits" : "requests");
                    json_t *ms = scope_new("github-copilot");
                    if (json_get_str(billing, "user")) json_set_str(ms, "accountId", json_get_str(billing, "user"));
                    json_set_str(ms, "modelId", model);
                    json_set_str(ms, "windowId", "monthly");
                    char *id = xasprintf("copilot:model:%s", model), *label = xasprintf("Model %s", model);
                    json_push(limits, limit_new(id, label, ms, json_clone(window), am, copilot_status(am, false)));
                    free(id);
                    free(label);
                }
                json_free(window);
                json_t *md = json_obj();
                if (json_get_str(billing, "user")) {
                    json_set_str(md, "accountId", json_get_str(billing, "user"));
                    json_set_str(md, "account", json_get_str(billing, "user"));
                }
                if (tp) json_set(md, "period", json_clone(tp));
                json_set(report, "metadata", md);
            } else failures++, auth_failures += auth_status(st);
            json_free(billing);
            free(url);
        }
        if (!report) {
            char *url = xasprintf("%s/copilot_internal/user", base);
            json_t *usage;
            if (copilot_get(url, key, false, p, &usage, &st) == 0 && json_is_obj(usage))
                report = copilot_from_internal(usage, user, NULL, true);
            else failures++, auth_failures += auth_status(st);
            json_free(usage);
            free(url);
        }
        free(user);
    } else {
        const char *rt = cstr(p->cred, "refreshToken"), *at = cstr(p->cred, "accessToken");
        const char *gh = rt ? rt : at;
        char *url = xasprintf("%s/copilot_internal/user", base);
        json_t *usage;
        if (copilot_get(url, gh, false, p, &usage, &st) == 0 && json_is_obj(usage)) {
            char *acc = cstr(p->cred, "accountId") ? xstrdup(cstr(p->cred, "accountId")) : NULL;
            int s2;
            if (!acc && rt) acc = copilot_username(base, rt, p, &s2);
            if (!acc && at) acc = copilot_username(base, at, p, &s2);
            report = copilot_from_internal(usage, acc, cstr(p->cred, "email"), false);
            free(acc);
        } else failures++, auth_failures += auth_status(st);
        json_free(usage);
        free(url);
    }
    if (!report && failures > 0 && failures == auth_failures) *rejected = true;
    free(base);
    return report;
}

const usage_provider_t usage_provider_copilot = {"github-copilot", copilot_supports, copilot_fetch, 0};

/* ============================================================================================
 * Z.AI (usage/zai.ts)
 * ============================================================================================ */

static bool zai_supports(const usage_params_t *p) {
    return str_eq(json_get_str(p->cred, "type"), "oauth") ? cstr(p->cred, "accessToken") != NULL : cstr(p->cred, "apiKey") != NULL;
}

static json_t *zai_window(const json_t *item) {
    double unit = -1, cnt = 1, nrt;
    bool has_unit = get_num(item, "unit", &unit);
    double nv;
    if (get_num(item, "number", &nv) && nv > 0) cnt = nv;
    char id[48], label[48];
    double dur = -1;
    long c = (long)cnt;
    switch (has_unit ? (int)unit : -1) {
    case 3:
        snprintf(id, sizeof id, "%ldh", c), snprintf(label, sizeof label, "%ld Hour%s", c, c == 1 ? "" : "s"), dur = cnt * HOUR_MS;
        break;
    case 4:
        snprintf(id, sizeof id, "%ldd", c), snprintf(label, sizeof label, "%ld Day%s", c, c == 1 ? "" : "s"), dur = cnt * DAY_MS;
        break;
    case 5:
        snprintf(id, sizeof id, "%ldmo", c);
        if (c == 1) snprintf(label, sizeof label, "Monthly");
        else snprintf(label, sizeof label, "%ld Months", c);
        dur = cnt * 30 * DAY_MS;
        break;
    case 6:
        snprintf(id, sizeof id, "1w"), snprintf(label, sizeof label, "Weekly"), dur = WEEK_MS;
        break;
    default:
        if (has_unit) snprintf(id, sizeof id, "%ldu%g", c, unit);
        else snprintf(id, sizeof id, "quota");
        snprintf(label, sizeof label, "Quota");
    }
    json_t *w = window_new(id, label);
    if (dur >= 0) json_set_num(w, "durationMs", dur);
    if (get_num(item, "nextResetTime", &nrt)) json_set_num(w, "resetsAt", nrt > 1e12 ? nrt : nrt * 1000);
    return w;
}

static bool zai_feature(const json_t *item) {
    bool a = false, b = false, c = false;
    json_t *d;
    json_arr_foreach(json_get_arr(item, "usageDetails"), i, d) {
        const char *m = json_get_str(d, "modelCode");
        a = a || str_eq(m, "search-prime");
        b = b || str_eq(m, "web-reader");
        c = c || str_eq(m, "zread");
    }
    return a && b && c;
}

static json_t *zai_fetch(const usage_params_t *p, bool *rejected) {
    (void)rejected;
    const char *token = str_eq(json_get_str(p->cred, "type"), "oauth") ? cstr(p->cred, "accessToken") : cstr(p->cred, "apiKey");
    if (!token) return NULL;
    char *base = NULL;
    url_t u;
    char *bt = p->base_url ? str_trim(p->base_url) : NULL;
    if (bt && *bt && url_parse(bt, &u) == 0) {
        bool def = (!strcmp(u.scheme, "https") && u.port == 443) || (!strcmp(u.scheme, "http") && u.port == 80);
        base = def ? xasprintf("%s://%s", u.scheme, u.host) : xasprintf("%s://%s:%d", u.scheme, u.host, u.port);
        url_free(&u);
    }
    free(bt);
    if (!base) base = xstrdup("https://api.z.ai");
    char *url = xasprintf("%s/api/monitor/usage/quota/limit", base);
    http_headers_t h = {0};
    http_headers_set(&h, "Authorization", token);
    http_headers_set(&h, "Content-Type", "application/json");
    http_headers_set(&h, "User-Agent", OMP_USER_AGENT);
    http_resp_t r;
    json_t *payload = NULL;
    if (uhttp("GET", url, &h, NULL, p, &r) == 0 && r.status >= 200 && r.status < 300) payload = resp_json(&r);
    http_resp_free(&r);
    json_t *report = NULL;
    if (payload && json_get_bool(payload, "success", false)) {
        report = report_new("zai", now_ms());
        json_t *limits = json_get_arr(report, "limits"), *it;
        json_arr_foreach(json_get_arr(json_get_obj(payload, "data"), "limits"), i, it) {
            const char *type = json_get_str(it, "type");
            if (!json_is_obj(it) || !type || !*type) continue;
            double cv, us, rem, pct;
            bool hcv = get_num(it, "currentValue", &cv), hus = get_num(it, "usage", &us), hrem = get_num(it, "remaining", &rem),
                 hpct = get_num(it, "percentage", &pct);
            double pf = hpct ? MIN(MAX(pct / 100, 0.0), 1.0) : 0;
            json_t *w = zai_window(it);
            const char *wid = json_get_str(w, "id"), *wl = json_get_str(w, "label");
            if (!strcmp(type, "TOKENS_LIMIT") || !strcmp(type, "CREDIT_LIMIT")) {
                bool credit = !strcmp(type, "CREDIT_LIMIT");
                bool abs_meter = credit && hcv && hus && us > 0;
                json_t *amount = counter_amount(hcv, cv, hus, us, hrem, rem, hpct && !abs_meter, pf, credit ? "credits" : "tokens");
                char *id = xasprintf("zai:%s:%s", credit ? "credits" : "tokens", wid);
                char *label = xasprintf("ZAI %s %s Quota", wl, credit ? "Credit" : "Token");
                json_t *s = scope_new("zai");
                json_set_str(s, "windowId", wid);
                json_set_bool(s, "shared", true);
                bool hf = json_is_num(json_get(amount, "usedFraction"));
                json_push(limits, limit_new(id, label, s, w, amount, hf ? status_of(true, json_get_num(amount, "usedFraction", 0)) : NULL));
                free(id);
                free(label);
            } else if (!strcmp(type, "TIME_LIMIT")) {
                json_t *amount = counter_amount(hcv, cv, hus, us, hrem, rem, hpct, pf, "requests");
                bool feat = zai_feature(it);
                char *id = feat ? xasprintf("zai:features:zread:%s", wid) : xasprintf("zai:requests:%s", wid);
                json_t *s = scope_new("zai");
                json_set_str(s, "windowId", wid);
                json_set_bool(s, "shared", !feat);
                if (feat) json_set_str(s, "tier", "zread");
                bool hf = json_is_num(json_get(amount, "usedFraction"));
                json_push(limits, limit_new(id, feat ? "ZAI Zread Quota" : "ZAI Request Quota", s, w, amount,
                                            hf ? status_of(true, json_get_num(amount, "usedFraction", 0)) : NULL));
                free(id);
            } else json_free(w);
        }
        if (json_len(limits) == 0) {
            json_free(report);
            report = NULL;
        } else {
            json_t *md = json_obj();
            json_set_str(md, "endpoint", url);
            if (cstr(p->cred, "accountId")) json_set_str(md, "accountId", cstr(p->cred, "accountId"));
            if (cstr(p->cred, "email")) json_set_str(md, "email", cstr(p->cred, "email"));
            const char *level = json_get_str(json_get_obj(payload, "data"), "level");
            if (level && *level) json_set_str(md, "planType", level);
            json_set(report, "metadata", md);
            json_set(report, "raw", json_clone(payload));
            /* model-usage over the last week */
            time_t nw = time(NULL), st = nw - 7 * 86400;
            struct tm a, b2;
            localtime_r(&st, &a);
            localtime_r(&nw, &b2);
            char s1[32], s2[32];
            strftime(s1, sizeof s1, "%Y-%m-%d+%H:%M:%S", &a);
            strftime(s2, sizeof s2, "%Y-%m-%d+%H:%M:%S", &b2);
            char *e1 = url_encode(s1), *e2 = url_encode(s2);
            char *murl = xasprintf("%s/api/monitor/usage/model-usage?startTime=%s&endTime=%s", base, e1, e2);
            free(e1);
            free(e2);
            if (uhttp("GET", murl, &h, NULL, p, &r) == 0 && r.status >= 200 && r.status < 300) {
                json_t *mu = resp_json(&r);
                if (json_is_obj(mu)) json_set(md, "modelUsage", mu);
                else json_free(mu);
            }
            http_resp_free(&r);
            free(murl);
        }
    }
    json_free(payload);
    http_headers_free(&h);
    free(url);
    free(base);
    return report;
}

const usage_provider_t usage_provider_zai = {"zai", zai_supports, zai_fetch, 0};

/* ============================================================================================
 * Synthetic (usage/synthetic.ts)
 * ============================================================================================ */

static bool syn_dollars(const json_t *v, double *out) {
    if (!json_is_str(v)) return false;
    const char *s = json_sv(v);
    if (*s == '$') s++;
    char *t = str_trim(s);
    char *end = NULL;
    double d = *t ? strtod(t, &end) : 0;
    bool ok = !*t || (end && !*end);
    if (!*t) d = 0; /* Number("") === 0 */
    free(t);
    if (!ok || !isfinite(d)) return false;
    *out = d;
    return true;
}

static bool syn_supports(const usage_params_t *p) { return str_eq(json_get_str(p->cred, "type"), "api_key"); }

static json_t *syn_fetch(const usage_params_t *p, bool *rejected) {
    (void)rejected;
    const char *key = cstr(p->cred, "apiKey");
    if (!key) return NULL;
    const char *url = "https://api.synthetic.new/v2/quotas";
    http_headers_t h = {0};
    char *a = xasprintf("Bearer %s", key);
    http_headers_set(&h, "Authorization", a);
    free(a);
    http_headers_set(&h, "Content-Type", "application/json");
    http_resp_t r;
    json_t *payload = NULL;
    if (uhttp("GET", url, &h, NULL, p, &r) == 0 && r.status >= 200 && r.status < 300) payload = resp_json(&r);
    http_resp_free(&r);
    http_headers_free(&h);
    if (!json_is_obj(payload)) {
        json_free(payload);
        return NULL;
    }
    json_t *report = report_new("synthetic", now_ms());
    json_t *limits = json_get_arr(report, "limits");
    const json_t *f = json_get_obj(payload, "rollingFiveHourLimit");
    if (f && (json_is_num(json_get(f, "remaining")) || json_is_num(json_get(f, "max")))) {
        bool hr = json_is_num(json_get(f, "remaining")), hm = json_is_num(json_get(f, "max"));
        double rem = json_get_num(f, "remaining", 0), mx = json_get_num(f, "max", 0), tick, nt;
        bool ht = json_is_num(json_get(f, "tickPercent"));
        tick = json_get_num(f, "tickPercent", 0);
        char label[64];
        if (ht) {
            char *pc = xasprintf("%.2f", tick * 100);
            double v = strtod(pc, NULL);
            free(pc);
            char *t = xasprintf("%.15g", v);
            snprintf(label, sizeof label, "5h \xc2\xb7 regen %s%%/tick", t);
            free(t);
        } else snprintf(label, sizeof label, "5h");
        json_t *w = window_new("5h", label);
        json_set_num(w, "durationMs", 5 * HOUR_MS);
        if (iso_ms(json_get_str(f, "nextTickAt"), &nt)) {
            json_set_num(w, "resetsAt", nt);
            json_set_str(w, "resetLabel", "tick");
        }
        json_t *amount = counter_amount(hm && hr, mx - rem, hm, mx, hr, rem, false, 0, "requests");
        json_t *s = scope_new("synthetic");
        json_set_str(s, "windowId", "5h");
        json_set_bool(s, "shared", true);
        const char *st = json_get_bool(f, "limited", false) ? "exhausted" : status_of(true, json_get_num(amount, "usedFraction", 0));
        json_push(limits, limit_new("synthetic:requests:5h", "Synthetic Requests", s, w, amount, st));
    }
    const json_t *wk = json_get_obj(payload, "weeklyTokenLimit");
    double remc, maxc;
    bool hrc = wk && syn_dollars(json_get(wk, "remainingCredits"), &remc), hmc = wk && syn_dollars(json_get(wk, "maxCredits"), &maxc);
    if (hrc || hmc) {
        bool hp = json_is_num(json_get(wk, "percentRemaining"));
        double uf = hp ? MIN(MAX(1 - json_get_num(wk, "percentRemaining", 0) / 100, 0.0), 1.0) : 0, nrc, nr;
        char label[64];
        if (syn_dollars(json_get(wk, "nextRegenCredits"), &nrc)) snprintf(label, sizeof label, "7d \xc2\xb7 regen $%.2f/tick", nrc);
        else snprintf(label, sizeof label, "7d");
        json_t *w = window_new("7d", label);
        json_set_num(w, "durationMs", WEEK_MS);
        if (iso_ms(json_get_str(wk, "nextRegenAt"), &nr)) {
            json_set_num(w, "resetsAt", nr);
            json_set_str(w, "resetLabel", "regen");
        }
        json_t *amount = counter_amount(hp && hmc, uf * maxc, hmc, maxc, hrc, remc, hp, uf, "usd");
        json_t *s = scope_new("synthetic");
        json_set_str(s, "windowId", "7d");
        json_set_bool(s, "shared", true);
        bool hf = json_is_num(json_get(amount, "usedFraction"));
        json_push(limits, limit_new("synthetic:usd:7d", "Synthetic Credits", s, w, amount,
                                    hf ? status_of(true, json_get_num(amount, "usedFraction", 0)) : NULL));
    }
    if (json_len(limits) == 0) {
        json_free(report);
        json_free(payload);
        return NULL;
    }
    json_t *md = json_obj();
    json_set_str(md, "endpoint", url);
    json_set(report, "metadata", md);
    json_set(report, "raw", payload);
    return report;
}

const usage_provider_t usage_provider_synthetic = {"synthetic", syn_supports, syn_fetch, 0};

/* ============================================================================================
 * Ollama (usage/ollama.ts): no quota API, a note-only report
 * ============================================================================================ */

static json_t *ollama_report(const usage_params_t *p) {
    json_t *r = report_new(p->provider, now_ms());
    json_t *notes = json_arr();
    json_push_str(notes, "Ollama does not expose a standalone quota usage API; per-response token usage is reported during requests.");
    json_set(r, "notes", notes);
    json_t *md = json_obj();
    static const char *const keys[] = {"email", "accountId", "projectId"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++)
        if (cstr(p->cred, keys[i])) json_set_str(md, keys[i], cstr(p->cred, keys[i]));
    if (json_len(md)) json_set(r, "metadata", md);
    else json_free(md);
    return r;
}

static json_t *ollama_fetch(const usage_params_t *p, bool *rejected) {
    (void)rejected;
    return ollama_report(p);
}

static bool ollama_supports(const usage_params_t *p) { return str_eq(p->provider, "ollama"); }
static bool ollama_cloud_supports(const usage_params_t *p) { return str_eq(p->provider, "ollama-cloud"); }

const usage_provider_t usage_provider_ollama = {"ollama", ollama_supports, ollama_fetch, 0};
const usage_provider_t usage_provider_ollama_cloud = {"ollama-cloud", ollama_cloud_supports, ollama_fetch, 0};

/* ============================================================================================
 * Kiro (usage/kiro.ts): GetUsageLimits on the credential's management region: monthly plan credits, active bonus
 * grants, purchased add-ons and overage. Timestamps arrive as epoch seconds; the …WithPrecision counters carry the
 * fractional credits the dashboard shows. (kiroRankingStrategy needs credential pools, which omc does not have.)
 * ============================================================================================ */

#define KIRO_MONTH_MS (30 * DAY_MS)

/* parsePositiveTimestamp: seconds below 1e12 become milliseconds */
static bool pos_ts(const json_t *v, double *out) {
    double d;
    if (!to_num(v, &d) || d <= 0) return false;
    *out = d < 1e12 ? d * 1000 : d;
    return true;
}

/* prefer the fractional …WithPrecision counter over its rounded twin */
static bool precise(const json_t *o, const char *key, double *out) {
    char *k = xasprintf("%sWithPrecision", key);
    bool ok = get_num(o, k, out) || get_num(o, key, out);
    free(k);
    return ok;
}

static char *trimmed_text(const json_t *v) {
    if (!json_is_str(v)) return NULL;
    char *t = str_trim(json_sv(v));
    if (*t) return t;
    free(t);
    return NULL;
}

/* credits used of limit; a zero-credit allowance counts as fully used */
static json_t *kiro_credits(double used, double limit) {
    return counter_amount(true, used, true, limit, true, MAX(0.0, limit - used), true, limit > 0 ? MIN(used / limit, 1.0) : 1.0,
                          "credits");
}

static json_t *kiro_scope(const char *window_id) {
    json_t *s = scope_new("kiro");
    json_set_str(s, "windowId", window_id);
    json_set_bool(s, "shared", true);
    return s;
}

/* a prepaid credit grant (bonus or purchased add-on): it expires rather than resetting monthly */
static json_t *kiro_grant(bool bonus, size_t index, const char *label, double used, double limit, bool has_exp, double exp) {
    char *wid = xasprintf("%s-%zu", bonus ? "bonus" : "add-on", index);
    json_t *w = window_new(wid, bonus ? "Bonus" : "Add-on");
    json_set_str(w, "resetLabel", "expires");
    if (has_exp) json_set_num(w, "resetsAt", exp);
    char *id = xasprintf("%s%zu", bonus ? "kiro:bonus:" : "kiro:add-on:", index);
    json_t *amount = kiro_credits(used, limit);
    double f = json_get_num(amount, "usedFraction", 0);
    json_t *l = limit_new(id, label, kiro_scope(wid), w, amount, status_of(true, f));
    free(id);
    free(wid);
    return l;
}

json_t *usage_kiro_parse(const json_t *payload, int64_t fetched_at) {
    json_t *list = json_get_arr(payload, "usageBreakdownList");
    if (!json_is_obj(payload) || !list) return NULL;
    const json_t *credit = NULL, *e;
    json_arr_foreach(list, i, e) if (!credit && json_is_obj(e) && str_eq(json_get_str(e, "resourceType"), "CREDIT")) credit = e;
    double plan_used, plan_limit;
    if (!credit || !precise(credit, "currentUsage", &plan_used) || !precise(credit, "usageLimit", &plan_limit) || plan_limit < 0)
        return NULL;
    char *title = trimmed_text(json_get(json_get_obj(payload, "subscriptionInfo"), "subscriptionTitle"));
    /* "KIRO PRO" -> "Kiro Pro" */
    char *plan = title ? str_lower(title) : xstrdup("Kiro");
    if (title)
        for (size_t i = 0; plan[i]; i++) {
            bool w = isalnum((unsigned char)plan[i]) || plan[i] == '_';
            bool pw = i > 0 && (isalnum((unsigned char)plan[i - 1]) || plan[i - 1] == '_');
            if (w && !pw) plan[i] = (char)toupper((unsigned char)plan[i]);
        }
    json_t *monthly = window_new("monthly", "Monthly");
    json_set_num(monthly, "durationMs", KIRO_MONTH_MS);
    double resets;
    if (pos_ts(json_get(credit, "nextDateReset"), &resets) || pos_ts(json_get(payload, "nextDateReset"), &resets))
        json_set_num(monthly, "resetsAt", resets);

    json_t *bonuses = json_arr();
    json_arr_foreach(json_get_arr(credit, "bonuses"), i, e) {
        double used, limit, exp = 0;
        if (!json_is_obj(e) || !str_eq(json_get_str(e, "status"), "ACTIVE")) continue;
        if (!get_num(e, "currentUsage", &used) || !get_num(e, "usageLimit", &limit) || limit <= 0) continue;
        char *label = trimmed_text(json_get(e, "displayName"));
        bool he = pos_ts(json_get(e, "expiresAt"), &exp);
        json_push(bonuses, kiro_grant(true, json_len(bonuses), label ? label : "Bonus credits", used, limit, he, exp));
        free(label);
    }
    /* the two lists project the same purchases: the legacy list only when the current one is missing or empty */
    json_t *current = json_get_arr(json_get_obj(credit, "addOnMetadata"), "addOns");
    json_t *purchases = json_len(current) > 0 ? current : json_get_arr(credit, "overageCredits");
    json_t *addons = json_arr();
    json_arr_foreach(purchases, i, e) {
        double used, limit, exp = 0;
        if (!json_is_obj(e)) continue;
        bool he = pos_ts(json_get(e, "expiresAt"), &exp);
        if (!precise(e, "currentUsage", &used) || !precise(e, "usageLimit", &limit) || limit <= 0) continue;
        if (he && exp <= (double)fetched_at) continue;
        json_push(addons, kiro_grant(false, json_len(addons), "Add-on credits", used, limit, he, exp));
    }

    double overage_used = 0, overage_cap = 0;
    if (!precise(credit, "currentOverages", &overage_used)) overage_used = 0;
    if (!precise(credit, "overageCap", &overage_cap)) overage_cap = 0;
    bool overage_on = str_eq(json_get_str(json_get_obj(payload, "overageConfiguration"), "overageStatus"), "ENABLED") && overage_cap > 0;

    json_t *plan_amount = kiro_credits(plan_used, plan_limit);
    double pf = json_get_num(plan_amount, "usedFraction", 0);
    bool spent = pf == 1;
    bool bonus_left = false, addon_left = false;
    json_arr_foreach(bonuses, i, e) if (!str_eq(json_get_str(e, "status"), "exhausted")) bonus_left = true;
    json_arr_foreach(addons, i, e) if (!str_eq(json_get_str(e, "status"), "exhausted")) addon_left = true;
    bool grant_left = bonus_left || addon_left;
    const char *plan_status = status_of(true, pf);
    if (spent && (grant_left || (overage_on && overage_used < overage_cap))) plan_status = "warning";
    char *unit = trimmed_text(json_get(credit, "displayNamePlural"));
    char *unit_l = unit ? str_lower(unit) : xstrdup("credits");
    char *plan_label = xasprintf("%s %s", plan, unit_l);
    json_t *report = report_new("kiro", fetched_at);
    json_t *limits = json_get(report, "limits");
    json_t *pl = limit_new("kiro:credits", plan_label, kiro_scope("monthly"), json_clone(monthly), plan_amount, plan_status);
    if (spent && grant_left) {
        json_t *notes = json_arr();
        char *n = xasprintf("Plan credits used up; drawing on %s credits",
                            bonus_left && addon_left ? "bonus and add-on" : bonus_left ? "bonus" : "add-on");
        json_push_str(notes, n);
        free(n);
        json_set(pl, "notes", notes);
    }
    json_push(limits, pl);
    while (json_len(bonuses)) json_push(limits, json_arr_remove(bonuses, 0));
    while (json_len(addons)) json_push(limits, json_arr_remove(addons, 0));
    if (overage_on) {
        json_t *amount = kiro_credits(overage_used, overage_cap);
        json_t *notes = json_arr();
        if (spent && !grant_left) json_push_str(notes, "Free credits used up; drawing paid overage");
        double charges, rate;
        if (get_num(credit, "overageCharges", &charges)) {
            char *per = get_num(credit, "overageRate", &rate) ? xasprintf(" ($%.2f per credit)", rate) : xstrdup("");
            char *n = xasprintf("Overage charges: $%.2f%s", charges, per);
            json_push_str(notes, n);
            free(n);
            free(per);
        }
        double of = json_get_num(amount, "usedFraction", 0);
        json_t *ol = limit_new("kiro:overage", "Overage credits", kiro_scope("monthly"), json_clone(monthly), amount, status_of(true, of));
        if (json_len(notes)) json_set(ol, "notes", notes);
        else json_free(notes);
        json_push(limits, ol);
    }
    /* the response's userInfo (the account email) is deliberately not carried over */
    if (title) {
        json_t *md = json_obj();
        json_set_str(md, "planType", plan);
        json_set(report, "metadata", md);
    }
    json_free(monthly);
    json_free(bonuses);
    json_free(addons);
    free(title);
    free(plan);
    free(unit);
    free(unit_l);
    free(plan_label);
    return report;
}

/* region, bearer and (OAuth only) profile ARN to ask for usage; false when the credential has no region */
static bool kiro_route(const json_t *cred, char **region, char **token, char **arn, bool *api_key) {
    *region = *token = *arn = NULL;
    if (str_eq(json_get_str(cred, "type"), "oauth")) {
        if (!cstr(cred, "accessToken") || !cstr(cred, "region")) return false;
        *region = xstrdup(cstr(cred, "region"));
        *token = xstrdup(cstr(cred, "accessToken"));
        if (cstr(cred, "kiroProfileArn")) *arn = xstrdup(cstr(cred, "kiroProfileArn"));
        *api_key = false;
        return true;
    }
    kiro_cred_t c;
    char *err = NULL;
    if (kiro_decode_credential(json_get_str(cred, "apiKey") ? json_get_str(cred, "apiKey") : "", &c, &err) != 0) {
        free(err);
        return false;
    }
    /* an env key without KIRO_API_REGION has no region; usage never probes */
    bool ok = c.region != NULL;
    if (ok) {
        *region = xstrdup(c.region);
        *token = xstrdup(c.token);
        *api_key = true;
    }
    kiro_cred_free(&c);
    return ok;
}

static bool kiro_supports(const usage_params_t *p) {
    char *r, *t, *a;
    bool k;
    bool ok = str_eq(p->provider, "kiro") && kiro_route(p->cred, &r, &t, &a, &k);
    if (ok) free(r), free(t), free(a);
    return ok;
}

static json_t *kiro_fetch(const usage_params_t *p, bool *rejected) {
    char *region, *token, *arn;
    bool api_key;
    if (!str_eq(p->provider, "kiro") || !kiro_route(p->cred, &region, &token, &arn, &api_key)) return NULL;
    json_t *report = NULL;
    /* an expired bearer whose refresh failed would only 401 and drop the last good report */
    json_t *exp = json_get(p->cred, "expiresAt");
    if (!(json_is_num(exp) && exp->u.num.d <= (double)now_ms())) {
        json_t *body = json_obj();
        json_set_str(body, "origin", "KIRO_CLI");
        if (arn) json_set_str(body, "profileArn", arn);
        json_set_str(body, "resourceType", "AGENTIC_REQUEST");
        int status = 0;
        char *err = NULL;
        json_t *payload = kiro_management_request(region, token, "GetUsageLimits", body, NULL, &status, &err);
        json_free(body);
        if (!payload) {
            /* a rejected login has lost access: drop its cached quota; an API key the usage route refuses has no report */
            if (!api_key && (status == 401 || status == 403)) *rejected = true;
            else LOGW("Kiro usage request failed: %s", err ? err : "?");
        } else {
            report = usage_kiro_parse(payload, now_ms());
            /* attribute the report to the login it measured: logins sharing a profile stay separate accounts */
            if (report && cstr(p->cred, "accountId")) {
                json_t *md = json_get_obj(report, "metadata");
                if (!md) json_set(report, "metadata", md = json_obj());
                json_set_str(md, "accountId", cstr(p->cred, "accountId"));
            }
            json_free(payload);
        }
        free(err);
    }
    free(region);
    free(token);
    free(arn);
    return report;
}

const usage_provider_t usage_provider_kiro = {"kiro", kiro_supports, kiro_fetch, 0};
