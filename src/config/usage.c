/* usage.c - provider usage report collection, cache and shared display helpers (port of omp
 * packages/ai/src/auth/usage.ts + usage-cache.ts, usage.ts resolveUsedFraction, usage/shared.ts,
 * tui overlays/usage-display.ts, prompt/usage-amounts.ts, chrome/format.ts, pi-utils format.ts,
 * coding-agent slash-commands/helpers/{usage-accounts,usage-report,active-oauth-account}.ts). */
#define _GNU_SOURCE
#include "usage.h"

#include "ai/ai.h"
#include "config/auth.h"
#include "core/fs.h"
#include "session/trace.h"
#include "tools/outsink.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>

#define USAGE_REPORT_TTL_MS (5 * 60000)
#define USAGE_FAILURE_BACKOFF_MS 10000
#define USAGE_REQUEST_TIMEOUT_MS 10000

/* ---------- registry ---------- */

extern const usage_provider_t usage_provider_anthropic, usage_provider_codex, usage_provider_copilot,
    usage_provider_gemini_cli, usage_provider_zai, usage_provider_synthetic, usage_provider_ollama,
    usage_provider_ollama_cloud;

/* DEFAULT_USAGE_PROVIDERS probe order (the ported subset) */
static const usage_provider_t *const PROVIDERS[] = {
    &usage_provider_codex,     &usage_provider_gemini_cli, &usage_provider_ollama,   &usage_provider_ollama_cloud,
    &usage_provider_anthropic, &usage_provider_zai,        &usage_provider_copilot, &usage_provider_synthetic,
};

size_t usage_provider_count(void) { return ARRAY_LEN(PROVIDERS); }
const usage_provider_t *usage_provider_at(size_t i) { return i < ARRAY_LEN(PROVIDERS) ? PROVIDERS[i] : NULL; }

const usage_provider_t *usage_provider_for(const char *provider) {
    for (size_t i = 0; provider && i < ARRAY_LEN(PROVIDERS); i++)
        if (!strcmp(PROVIDERS[i]->id, provider)) return PROVIDERS[i];
    return NULL;
}

/* ---------- requests ---------- */

/* getProviderBaseUrl: the first catalog model of the provider with a base URL (models.yml overrides applied) */
static const char *provider_base_url(const char *provider) {
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (str_eq(m->provider, provider) && m->base_url && *m->base_url) return m->base_url;
    }
    return NULL;
}

/* buildUsageCredential from a stored auth.json entry */
static json_t *usage_credential(const json_t *e) {
    json_t *c = json_obj();
    if (str_eq(json_get_str(e, "type"), "oauth")) {
        json_set_str(c, "type", "oauth");
        static const char *const map[][2] = {{"access", "accessToken"}, {"refresh", "refreshToken"}};
        for (size_t i = 0; i < ARRAY_LEN(map); i++)
            if (json_get_str(e, map[i][0]) && *json_get_str(e, map[i][0])) json_set_str(c, map[i][1], json_get_str(e, map[i][0]));
        if (json_is_num(json_get(e, "expires"))) json_set(c, "expiresAt", json_clone(json_get(e, "expires")));
        static const char *const keep[] = {"accountId", "projectId", "email", "orgId", "orgName", "enterpriseUrl", "apiEndpoint"};
        for (size_t i = 0; i < ARRAY_LEN(keep); i++)
            if (json_get_str(e, keep[i]) && *json_get_str(e, keep[i])) json_set_str(c, keep[i], json_get_str(e, keep[i]));
    } else {
        json_set_str(c, "type", "api_key");
    }
    return c;
}

typedef struct {
    char *provider;
    json_t *cred;
    const char *base_url;
    char *key; /* cache key */
    const usage_provider_t *impl;
    json_t *previous;
    json_t *report; /* result */
    bool rejected;
} ureq_t;

/* usageCacheIdentity */
char *usage_account_key(const json_t *cred) {
    sb_t b = SB_INIT;
    sb_append(&b, json_get_str(cred, "type") ? json_get_str(cred, "type") : "api_key");
    char *acc = json_get_str(cred, "accountId") ? str_trim(json_get_str(cred, "accountId")) : xstrdup("");
    char *em_t = json_get_str(cred, "email") ? str_trim(json_get_str(cred, "email")) : xstrdup(""), *em = str_lower(em_t);
    char *org = json_get_str(cred, "orgId") ? str_trim(json_get_str(cred, "orgId")) : xstrdup("");
    char *proj = json_get_str(cred, "projectId") ? str_trim(json_get_str(cred, "projectId")) : xstrdup("");
    char *ent_t = json_get_str(cred, "enterpriseUrl") ? str_trim(json_get_str(cred, "enterpriseUrl")) : xstrdup(""), *ent = str_lower(ent_t);
    if (*acc) sb_appendf(&b, "|account:%s", acc);
    if (*em) sb_appendf(&b, "|email:%s", em);
    if (*org) sb_appendf(&b, "|org:%s", org);
    if (*proj) sb_appendf(&b, "|project:%s", proj);
    if (*ent) sb_appendf(&b, "|enterprise:%s", ent);
    if (!*acc && !*em && !*org) {
        const char *sec = json_get_str(cred, "apiKey");
        if (!sec || !*sec) sec = json_get_str(cred, "refreshToken");
        if (!sec || !*sec) sec = json_get_str(cred, "accessToken");
        if (sec && *sec) {
            char *t = str_trim(sec);
            sb_appendf(&b, "|secret:%llx", (unsigned long long)hash_xx64(t, strlen(t), 0));
            free(t);
        } else sb_append(&b, "|anonymous");
    }
    free(acc), free(em_t), free(em), free(org), free(proj), free(ent_t), free(ent);
    return sb_detach(&b);
}

static char *cache_key(const char *provider, const json_t *cred) {
    char *id = usage_account_key(cred);
    char *k = xasprintf("%s|%s", provider, id);
    free(id);
    return k;
}

/* ---------- history (usage-history.jsonl: one row per account, limit and hour) ---------- */

static void record_history(const char *provider, const json_t *cred, const json_t *report) {
    json_t *limits = json_get_arr(report, "limits"), *l;
    if (json_len(limits) == 0) return;
    double at = json_get_num(report, "fetchedAt", 0);
    if (!(at > 0)) at = (double)now_ms();
    char *key = usage_account_key(cred);
    const json_t *md = json_get_obj(report, "metadata");
    sb_t b = SB_INIT;
    json_arr_foreach(limits, i, l) {
        json_t *e = json_obj();
        json_set_num(e, "recordedAt", at);
        json_set_str(e, "provider", provider);
        json_set_str(e, "accountKey", key);
        const char *email = json_get_str(cred, "email") ? json_get_str(cred, "email") : json_get_str(md, "email");
        const char *acc = json_get_str(cred, "accountId") ? json_get_str(cred, "accountId")
                          : json_get_str(json_get_obj(l, "scope"), "accountId") ? json_get_str(json_get_obj(l, "scope"), "accountId")
                                                                               : json_get_str(md, "accountId");
        if (email) json_set_str(e, "email", email);
        if (acc) json_set_str(e, "accountId", acc);
        json_set_str(e, "limitId", json_get_str(l, "id") ? json_get_str(l, "id") : "");
        json_set_str(e, "label", json_get_str(l, "label") ? json_get_str(l, "label") : "");
        const char *wl = json_get_str(json_get_obj(l, "window"), "label");
        if (!wl) wl = json_get_str(json_get_obj(l, "scope"), "windowId");
        if (wl) json_set_str(e, "windowLabel", wl);
        double f;
        if (usage_used_fraction(l, &f)) json_set_num(e, "usedFraction", f);
        if (json_get_str(l, "status")) json_set_str(e, "status", json_get_str(l, "status"));
        json_t *ra = json_get(json_get_obj(l, "window"), "resetsAt");
        if (json_is_num(ra)) json_set(e, "resetsAt", json_clone(ra));
        json_dump_sb(&b, e, 0);
        sb_putc(&b, '\n');
        json_free(e);
    }
    char *path = omc_agent_path("usage-history.jsonl");
    fs_append(path, b.s, b.len);
    free(path);
    sb_free(&b);
    free(key);
}

json_t *usage_history(const char *provider, int64_t since) {
    char *path = omc_agent_path("usage-history.jsonl");
    size_t n = 0;
    char *text = fs_read(path, &n);
    free(path);
    json_t *out = json_arr();
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        json_t *e = json_parse_str(line);
        const char *p = json_get_str(e, "provider");
        if (!json_is_obj(e) || !p || json_get_num(e, "recordedAt", 0) < (double)since || (provider && strcasecmp(p, provider))) {
            json_free(e);
            continue;
        }
        /* one row per (provider, account, limit, hour): the latest snapshot of the bucket wins */
        int64_t bucket = (int64_t)floor(json_get_num(e, "recordedAt", 0) / 3600000.0);
        bool replaced = false;
        for (size_t i = json_len(out); i-- > 0 && !replaced;) {
            json_t *o = json_at(out, i);
            if (str_eq(json_get_str(o, "provider"), p) && str_eq(json_get_str(o, "accountKey"), json_get_str(e, "accountKey")) &&
                str_eq(json_get_str(o, "limitId"), json_get_str(e, "limitId"))) {
                if ((int64_t)floor(json_get_num(o, "recordedAt", 0) / 3600000.0) == bucket) {
                    json_free(json_arr_remove(out, i));
                    json_arr_insert(out, i, e);
                    replaced = true;
                }
                break;
            }
        }
        if (!replaced) json_push(out, e);
    }
    free(text);
    return out;
}

/* #collectUsageRequests over omc's single credential per provider */
typedef VEC(ureq_t) ureq_vec_t;
static void collect(const char *filter, ureq_vec_t *out) {
    strvec_t provs = {0};
    json_t *stored = auth_list(), *e;
    json_arr_foreach(stored, i, e) {
        const char *p = json_get_str(e, "provider");
        if (p && !strvec_contains(&provs, p)) strvec_push(&provs, p);
    }
    json_free(stored);
    for (size_t i = 0; i < ARRAY_LEN(PROVIDERS); i++)
        if (!strvec_contains(&provs, PROVIDERS[i]->id)) strvec_push(&provs, PROVIDERS[i]->id);
    for (size_t i = 0; i < provs.n; i++) {
        const char *p = provs.v[i];
        if (filter && strcasecmp(filter, p)) continue;
        const usage_provider_t *impl = usage_provider_for(p);
        if (!impl) continue;
        json_t *entry = auth_get_credential(p);
        json_t *cred = NULL;
        if (entry) {
            cred = usage_credential(entry);
            if (str_eq(json_get_str(entry, "type"), "api_key")) {
                char *k = auth_get_api_key(p); /* resolves "!command" / env-name references */
                if (k && *k) json_set_str(cred, "apiKey", k);
                else {
                    json_free(cred);
                    cred = NULL;
                }
                free(k);
            }
            json_free(entry);
        } else {
            char *k = auth_get_api_key(p); /* runtime --api-key, else the provider's env key */
            if (k && *k) {
                cred = json_obj();
                json_set_str(cred, "type", "api_key");
                json_set_str(cred, "apiKey", k);
            }
            free(k);
        }
        if (!cred) continue;
        usage_params_t pp = {.provider = p, .cred = cred, .base_url = provider_base_url(p)};
        if (impl->supports && !impl->supports(&pp)) {
            json_free(cred);
            continue;
        }
        ureq_t r = {.provider = xstrdup(p), .cred = cred, .base_url = pp.base_url, .impl = impl};
        r.key = cache_key(p, cred);
        vec_push(out, r);
    }
    strvec_free(&provs);
}

/* ---------- cache ---------- */

typedef struct {
    char *key;
    char *provider;
    json_t *report;      /* last good (may be NULL) */
    int64_t expires_at;  /* report valid until */
    int64_t retry_after; /* failure backoff */
} centry_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static VEC(centry_t) g_cache;
static json_t *g_last; /* newest aggregate snapshot */

static void cache_clear_mem(void) {
    for (size_t i = 0; i < g_cache.n; i++) {
        free(g_cache.v[i].key);
        free(g_cache.v[i].provider);
        json_free(g_cache.v[i].report);
    }
    g_cache.n = 0;
}

/* the cache is shared with other omc processes through <agent>/usage-cache.json (omp keeps it in agent.db) */
static void cache_load(void) {
    cache_clear_mem();
    char *path = omc_agent_path("usage-cache.json");
    size_t n = 0;
    char *text = fs_read(path, &n);
    free(path);
    json_t *j = text ? json_parse(text, n, NULL) : NULL;
    free(text);
    const char *k;
    json_t *v;
    if (json_is_obj(j)) json_obj_foreach(j, i, k, v) {
            if (!json_is_obj(v) || !json_get_str(v, "provider")) continue;
            centry_t c = {.key = xstrdup(k), .provider = xstrdup(json_get_str(v, "provider")),
                          .report = json_is_obj(json_get(v, "report")) ? json_clone(json_get(v, "report")) : NULL,
                          .expires_at = json_get_int(v, "expiresAt", 0), .retry_after = json_get_int(v, "retryAfter", 0)};
            vec_push(&g_cache, c);
        }
    json_free(j);
}

static void cache_save(void) {
    json_t *j = json_obj();
    int64_t now = now_ms();
    for (size_t i = 0; i < g_cache.n; i++) {
        centry_t *c = &g_cache.v[i];
        /* last-good reports are kept for a day */
        if (!c->report && c->retry_after <= now) continue;
        if (c->report && json_get_num(c->report, "fetchedAt", 0) < now - 86400000.0 && c->retry_after <= now) continue;
        json_t *e = json_obj();
        json_set_str(e, "provider", c->provider);
        if (c->report) json_set(e, "report", json_clone(c->report));
        json_set_int(e, "expiresAt", c->expires_at);
        json_set_int(e, "retryAfter", c->retry_after);
        json_set(j, c->key, e);
    }
    char *path = omc_agent_path("usage-cache.json"), *s = json_dump(j, 0);
    fs_write_atomic(path, s, strlen(s), 0600);
    free(path);
    free(s);
    json_free(j);
}

static centry_t *cache_find(const char *key) {
    for (size_t i = 0; i < g_cache.n; i++)
        if (!strcmp(g_cache.v[i].key, key)) return &g_cache.v[i];
    return NULL;
}

static void *probe_thread(void *arg) {
    ureq_t *r = arg;
    usage_params_t pp = {.provider = r->provider, .cred = r->cred, .base_url = r->base_url,
                         .timeout_ms = USAGE_REQUEST_TIMEOUT_MS, .previous = r->previous};
    r->report = r->impl->fetch(&pp, &r->rejected);
    /* attribute the report to the credential's organization */
    const char *org = json_get_str(r->cred, "orgId");
    if (r->report && org) {
        json_t *md = json_get_obj(r->report, "metadata");
        if (!md) {
            md = json_obj();
            json_set(r->report, "metadata", md);
        }
        bool same = !json_get_str(md, "orgId") || str_eq(json_get_str(md, "orgId"), org);
        if (!json_get_str(md, "orgId")) json_set_str(md, "orgId", org);
        if (same && json_get_str(r->cred, "orgName") && !json_get_str(md, "orgName"))
            json_set_str(md, "orgName", json_get_str(r->cred, "orgName"));
    }
    return NULL;
}

json_t *usage_fetch_reports(const char *provider) {
    ureq_vec_t reqs = {0};
    collect(provider, &reqs);
    int64_t now = now_ms();
    VEC(pthread_t) th = {0};
    VEC(size_t) th_idx = {0};
    pthread_mutex_lock(&g_mu);
    cache_load();
    for (size_t i = 0; i < reqs.n; i++) {
        centry_t *c = cache_find(reqs.v[i].key);
        if (c && c->report && c->expires_at > now) {
            reqs.v[i].report = json_clone(c->report);
            continue;
        }
        if (c && c->retry_after > now) { /* failure cool-down: serve last good */
            reqs.v[i].report = c->report ? json_clone(c->report) : NULL;
            continue;
        }
        reqs.v[i].previous = c && c->report ? json_clone(c->report) : NULL;
        pthread_t t;
        if (pthread_create(&t, NULL, probe_thread, &reqs.v[i]) == 0) {
            vec_push(&th, t);
            vec_push(&th_idx, i);
        }
    }
    pthread_mutex_unlock(&g_mu);
    for (size_t i = 0; i < th.n; i++) pthread_join(th.v[i], NULL);
    now = now_ms();
    pthread_mutex_lock(&g_mu);
    if (th_idx.n) cache_load();
    for (size_t k = 0; k < th_idx.n; k++) {
        ureq_t *r = &reqs.v[th_idx.v[k]];
        centry_t *c = cache_find(r->key);
        if (!c) {
            vec_push(&g_cache, ((centry_t){.key = xstrdup(r->key), .provider = xstrdup(r->provider)}));
            c = &g_cache.v[g_cache.n - 1];
        }
        if (r->report) {
            record_history(r->provider, r->cred, r->report);
            json_free(c->report);
            c->report = json_clone(r->report);
            c->expires_at = now + USAGE_REPORT_TTL_MS;
            c->retry_after = 0;
        } else {
            if (r->rejected) { /* definitive auth failure: never replay the last good report */
                json_free(c->report);
                c->report = NULL;
            }
            c->expires_at = 0;
            c->retry_after = now + (r->impl->failure_backoff_ms ? r->impl->failure_backoff_ms : USAGE_FAILURE_BACKOFF_MS);
            if (c->report) r->report = json_clone(c->report); /* last-good retention */
        }
    }
    if (th_idx.n) cache_save();
    json_t *out = json_arr();
    for (size_t i = 0; i < reqs.n; i++) {
        if (reqs.v[i].report) json_push(out, reqs.v[i].report);
        reqs.v[i].report = NULL;
    }
    json_free(g_last);
    g_last = json_clone(out);
    pthread_mutex_unlock(&g_mu);
    for (size_t i = 0; i < reqs.n; i++) {
        free(reqs.v[i].provider);
        free(reqs.v[i].key);
        json_free(reqs.v[i].cred);
        json_free(reqs.v[i].previous);
    }
    vec_free(&reqs);
    vec_free(&th);
    vec_free(&th_idx);
    return out;
}

void usage_invalidate(const char *provider) {
    pthread_mutex_lock(&g_mu);
    cache_load();
    for (size_t i = 0; i < g_cache.n;) {
        centry_t *c = &g_cache.v[i];
        if (!provider || !strcasecmp(c->provider, provider)) {
            free(c->key);
            free(c->provider);
            json_free(c->report);
            g_cache.v[i] = g_cache.v[--g_cache.n];
        } else i++;
    }
    cache_save();
    pthread_mutex_unlock(&g_mu);
}

json_t *usage_cached_reports(void) {
    pthread_mutex_lock(&g_mu);
    json_t *r = g_last ? json_clone(g_last) : NULL;
    pthread_mutex_unlock(&g_mu);
    return r;
}

void usage_reset_cache(void) {
    usage_invalidate(NULL);
    pthread_mutex_lock(&g_mu);
    json_free(g_last);
    g_last = NULL;
    pthread_mutex_unlock(&g_mu);
}

/* ---------- accounts ---------- */

json_t *usage_stored_accounts(const char *provider) {
    json_t *out = json_arr(), *all = auth_list(), *e;
    int64_t id = 0;
    json_arr_foreach(all, i, e) {
        const char *p = json_get_str(e, "provider");
        id++;
        if (!p) continue;
        if (provider ? strcasecmp(provider, p) != 0 : !usage_provider_for(p)) continue;
        json_t *a = json_obj();
        json_set_str(a, "provider", p);
        bool oauth = str_eq(json_get_str(e, "type"), "oauth");
        json_set_str(a, "type", oauth ? "oauth" : "api_key");
        if (oauth) {
            json_t *cred = auth_get_credential_raw(p);
            static const char *const keys[] = {"email", "accountId", "projectId", "enterpriseUrl", "orgId", "orgName"};
            for (size_t k = 0; k < ARRAY_LEN(keys); k++)
                if (json_get_str(cred, keys[k]) && *json_get_str(cred, keys[k])) json_set_str(a, keys[k], json_get_str(cred, keys[k]));
            json_free(cred);
        }
        json_set_int(a, "credentialId", id);
        json_push(out, a);
    }
    json_free(all);
    return out;
}

static void add_id(strvec_t *ids, const char *v) {
    if (!v || !*v) return;
    char *l = str_lower(v);
    if (!strvec_contains(ids, l)) strvec_push(ids, l);
    free(l);
}

static void report_identifiers(const json_t *r, strvec_t *ids) {
    const json_t *md = json_get_obj(r, "metadata");
    add_id(ids, json_get_str(md, "email"));
    add_id(ids, json_get_str(md, "accountId"));
    add_id(ids, json_get_str(md, "projectId"));
    add_id(ids, json_get_str(md, "orgId"));
    json_t *l;
    json_arr_foreach(json_get_arr(r, "limits"), i, l) {
        const json_t *sc = json_get_obj(l, "scope");
        add_id(ids, json_get_str(sc, "accountId"));
        add_id(ids, json_get_str(sc, "projectId"));
        add_id(ids, json_get_str(sc, "orgId"));
    }
}

static bool covers(const json_t *r, const char *email, const strvec_t *ids) {
    const char *re = json_get_str(json_get_obj(r, "metadata"), "email");
    if (email && re && *re) return !strcasecmp(re, email);
    strvec_t have = {0};
    report_identifiers(r, &have);
    bool hit = false;
    for (size_t i = 0; i < ids->n && !hit; i++) hit = strvec_contains(&have, ids->v[i]);
    strvec_free(&have);
    return hit;
}

json_t *usage_unreported_accounts(const json_t *reports, const json_t *accounts) {
    json_t *out = json_arr(), *a;
    json_arr_foreach(accounts, i, a) {
        const char *prov = json_get_str(a, "provider");
        VEC(const json_t *) pr = {0};
        json_t *r;
        json_arr_foreach(reports, k, r) if (str_eq(json_get_str(r, "provider"), prov)) vec_push(&pr, r);
        bool unreported;
        if (pr.n == 0) unreported = true;
        else if (str_eq(json_get_str(a, "type"), "api_key")) unreported = false;
        else {
            const char *org = json_get_str(a, "orgId"), *email = json_get_str(a, "email");
            strvec_t ids = {0};
            add_id(&ids, email);
            add_id(&ids, json_get_str(a, "accountId"));
            add_id(&ids, json_get_str(a, "projectId"));
            if (!org && ids.n == 0) unreported = false;
            else {
                bool saw_org = false;
                VEC(const json_t *) same = {0}, no_org = {0};
                for (size_t k = 0; k < pr.n; k++) {
                    const char *mo = json_get_str(json_get_obj(pr.v[k], "metadata"), "orgId");
                    if (mo && *mo) {
                        saw_org = true;
                        if (org && !strcasecmp(mo, org)) vec_push(&same, pr.v[k]);
                    } else vec_push(&no_org, pr.v[k]);
                }
                if (org || saw_org) {
                    VEC(const json_t *) *cand = org ? (void *)&same : (void *)&no_org;
                    if (cand->n == 0) unreported = true;
                    else if (ids.n == 0) unreported = false;
                    else {
                        unreported = true;
                        for (size_t k = 0; k < cand->n && unreported; k++)
                            if (covers(cand->v[k], email, &ids)) unreported = false;
                    }
                } else {
                    bool any = false;
                    for (size_t k = 0; k < pr.n && !any; k++) {
                        strvec_t have = {0};
                        report_identifiers(pr.v[k], &have);
                        any = have.n > 0;
                        strvec_free(&have);
                    }
                    if (!any) unreported = false;
                    else {
                        unreported = true;
                        for (size_t k = 0; k < pr.n && unreported; k++)
                            if (covers(pr.v[k], email, &ids)) unreported = false;
                    }
                }
                vec_free(&same);
                vec_free(&no_org);
            }
            strvec_free(&ids);
        }
        vec_free(&pr);
        if (unreported) json_push(out, json_clone(a));
    }
    return out;
}

char *usage_account_label(const json_t *a) {
    if (str_eq(json_get_str(a, "type"), "api_key")) return xstrdup("API key");
    static const char *const keys[] = {"email", "accountId", "projectId", "enterpriseUrl"};
    const char *base = NULL;
    for (size_t i = 0; i < ARRAY_LEN(keys) && !base; i++) base = json_get_str(a, keys[i]);
    if (!base) base = "OAuth account";
    const char *org = json_get_str(a, "orgName") ? json_get_str(a, "orgName") : json_get_str(a, "orgId");
    if (!org || !strcmp(org, base)) return xstrdup(base);
    return xasprintf("%s \xc2\xb7 %s", base, org);
}

/* ---------- amounts ---------- */

static bool num(const json_t *o, const char *k, double *v) {
    json_t *x = json_get(o, k);
    if (!json_is_num(x) || !isfinite(x->u.num.d)) return false;
    *v = x->u.num.d;
    return true;
}

bool usage_used_fraction(const json_t *limit, double *out) {
    const json_t *a = json_get_obj(limit, "amount");
    double uf, u, l, rf;
    if (num(a, "usedFraction", &uf)) return *out = uf, true;
    if (num(a, "used", &u) && num(a, "limit", &l) && l > 0) return *out = u / l, true;
    if (str_eq(json_get_str(a, "unit"), "percent") && num(a, "used", &u)) return *out = u / 100, true;
    if (num(a, "remainingFraction", &rf)) return *out = MAX(0.0, 1 - rf), true;
    return false;
}

const char *usage_status_for(bool has, double f) {
    if (!has) return "unknown";
    if (f >= 1) return "exhausted";
    if (f >= 0.9) return "warning";
    return "ok";
}

char *usage_limit_title(const json_t *limit) {
    const char *label = json_get_str(limit, "label");
    if (!label) label = "";
    const char *tier = json_get_str(json_get_obj(limit, "scope"), "tier");
    if (tier && *tier && !str_istr(label, tier)) return xasprintf("%s (%s)", label, tier);
    return xstrdup(label);
}

json_t *usage_collapse_shared(const json_t *reports) {
    json_t *out = json_clone(reports), *r;
    json_arr_foreach(out, i, r) {
        json_t *limits = json_get_arr(r, "limits");
        strvec_t seen = {0};
        for (size_t k = 0; limits && k < json_len(limits);) {
            const char *g = json_get_str(json_get_obj(json_at(limits, k), "scope"), "sharedGroup");
            if (g && strvec_contains(&seen, g)) {
                json_free(json_arr_remove(limits, k));
                continue;
            }
            if (g) strvec_push(&seen, g);
            k++;
        }
        strvec_free(&seen);
    }
    return out;
}

const char *usage_reset_window_name(const char *id) {
    if (str_eq(id, "anthropic:5h")) return "Claude 5h";
    if (str_eq(id, "anthropic:7d")) return "Claude weekly";
    if (str_eq(id, "anthropic:7d:opus")) return "Claude Opus weekly";
    if (str_eq(id, "anthropic:7d:sonnet")) return "Claude Sonnet weekly";
    return id;
}

static bool banked_expiry(const json_t *credit, double *ms) {
    const char *exp = json_get_str(credit, "expiresAt");
    if (!exp || !*exp || json_get_num(credit, "remainingCount", 1) == 0 || str_eq(json_get_str(credit, "status"), "redeemed"))
        return false;
    double v;
    if (!trace_parse_iso_ms(exp, &v)) return false;
    *ms = v;
    return true;
}

bool usage_reset_summary(const json_t *rc, int64_t now, usage_reset_summary_t *s) {
    memset(s, 0, sizeof *s);
    if (!json_is_obj(rc)) return false;
    double avail = json_get_num(rc, "availableCount", 0);
    s->banked = MAX(0, (int64_t)trunc(avail));
    s->redeemable = MAX(0, (int64_t)trunc(json_is_num(json_get(rc, "redeemableCount")) ? json_get_num(rc, "redeemableCount", 0) : avail));
    const char *soon = NULL, *late = NULL;
    double soon_ms = INFINITY, late_ms = -INFINITY;
    json_t *c, *credits = json_get_arr(rc, "credits");
    json_arr_foreach(credits, i, c) {
        double ms;
        if (!banked_expiry(c, &ms)) continue;
        if (ms > now && ms < soon_ms) soon_ms = ms, soon = json_get_str(c, "expiresAt");
        else if (ms <= now && ms > late_ms) late_ms = ms, late = json_get_str(c, "expiresAt");
    }
    if (!soon) soon = late;
    s->soonest_expiry = soon ? xstrdup(soon) : NULL;
    const json_t *sel = NULL;
    const char *next = json_get_str(rc, "nextCreditId");
    if (next) {
        json_arr_foreach(credits, i, c) if (str_eq(json_get_str(c, "id"), next)) {
            sel = c;
            break;
        }
    } else {
        json_arr_foreach(credits, i, c) if (!json_is_bool(json_get(c, "usable")) || json_get_bool(c, "usable", true)) {
            sel = c;
            break;
        }
        if (!sel) sel = json_at(credits, 0);
    }
    if (json_get_str(rc, "reason")) s->unavailable_reason = xstrdup(json_get_str(rc, "reason"));
    else if (json_get_str(rc, "cooldownUntil")) s->unavailable_reason = xasprintf("cooldown until %s", json_get_str(rc, "cooldownUntil"));
    else if (sel && json_len(json_get_arr(sel, "blocking")) > 0) {
        sb_t b = SB_INIT;
        sb_append(&b, "blocked by ");
        json_t *w;
        json_arr_foreach(json_get_arr(sel, "blocking"), i, w) {
            if (i) sb_append(&b, ", ");
            sb_append(&b, usage_reset_window_name(json_sv(w)) ? usage_reset_window_name(json_sv(w)) : "");
        }
        s->unavailable_reason = sb_detach(&b);
    } else if (sel && json_get_str(sel, "status") && !str_eq(json_get_str(sel, "status"), "available"))
        s->unavailable_reason = xstrdup(json_get_str(sel, "status"));
    else if (json_is_bool(json_get(rc, "eligible")) && !json_get_bool(rc, "eligible", true))
        s->unavailable_reason = xstrdup("not eligible");
    else if (s->banked > 0 && s->redeemable == 0)
        s->unavailable_reason = xstrdup("not usable right now");
    return true;
}

void usage_reset_summary_free(usage_reset_summary_t *s) {
    free(s->soonest_expiry);
    free(s->unavailable_reason);
    memset(s, 0, sizeof *s);
}

static bool absolute_unit(const json_t *limit) {
    const char *u = json_get_str(json_get_obj(limit, "amount"), "unit");
    return u && strcmp(u, "percent") && strcmp(u, "unknown");
}

bool usage_used_only_amount(const json_t *limit) {
    const json_t *a = json_get_obj(limit, "amount");
    double d;
    return absolute_unit(limit) && num(a, "used", &d) && !json_get(a, "limit") && !json_get(a, "remaining") &&
           !usage_used_fraction(limit, &d);
}

bool usage_remaining_only_amount(const json_t *limit) {
    const json_t *a = json_get_obj(limit, "amount");
    double d;
    return absolute_unit(limit) && num(a, "remaining", &d) && !json_get(a, "limit") && !json_get(a, "used") &&
           !usage_used_fraction(limit, &d);
}

static char *format_quantity(double v, const char *unit, const char *suffix) {
    if (str_eq(unit, "usd")) return xasprintf("$%.2f %s", v, suffix);
    char *c = usage_fmt_compact(v);
    char *r = xasprintf("%s %s %s", c, unit, suffix);
    free(c);
    return r;
}

char *usage_remaining_only_total(const json_t *const *limits, size_t n) {
    if (n == 0) return NULL;
    const char *unit = json_get_str(json_get_obj(limits[0], "amount"), "unit");
    for (size_t i = 0; i < n; i++)
        if (!usage_remaining_only_amount(limits[i]) || !str_eq(json_get_str(json_get_obj(limits[i], "amount"), "unit"), unit))
            return NULL;
    double total = 0, shared = -1;
    for (size_t i = 0; i < n; i++) {
        double r = json_get_num(json_get_obj(limits[i], "amount"), "remaining", 0);
        if (json_get_bool(json_get_obj(limits[i], "scope"), "shared", false)) shared = MAX(shared, r);
        else total += r;
    }
    return format_quantity(total + (shared < 0 ? 0 : shared), unit, "left");
}

char *usage_absolute_only_amount(const json_t *const *limits, size_t n) {
    if (n == 0) return NULL;
    bool all_used = true;
    for (size_t i = 0; i < n; i++) all_used = all_used && usage_used_only_amount(limits[i]);
    if (all_used) {
        const char *unit = json_get_str(json_get_obj(limits[0], "amount"), "unit");
        double used = 0;
        for (size_t i = 0; i < n; i++) {
            if (!str_eq(json_get_str(json_get_obj(limits[i], "amount"), "unit"), unit)) return NULL;
            used = MAX(used, json_get_num(json_get_obj(limits[i], "amount"), "used", 0));
        }
        return format_quantity(used, unit, "used");
    }
    return usage_remaining_only_total(limits, n);
}

/* ---------- formatting ---------- */

char *usage_provider_name(const char *provider) {
    sb_t b = SB_INIT;
    bool start = true;
    for (const char *p = provider ? provider : ""; *p; p++) {
        if (*p == '-' || *p == '_') {
            sb_putc(&b, ' ');
            start = true;
            continue;
        }
        sb_putc(&b, start ? (char)toupper((unsigned char)*p) : *p);
        start = false;
    }
    return sb_detach(&b);
}

char *usage_fmt_duration(double ms) {
    const double SEC = 1000, MIN = 60 * SEC, HOUR = 60 * MIN, DAY = 24 * HOUR;
    if (!isfinite(ms) || ms <= 0) return xstrdup("0ms");
    if (ms < SEC) return xasprintf("%.0fms", floor(ms));
    if (ms < MIN) {
        double tenths = floor(ms / 100 + 0.5);
        return tenths < 600 ? xasprintf("%.1fs", tenths / 10) : xstrdup("1m");
    }
    if (ms < HOUR) {
        long m = (long)floor(ms / MIN), s = (long)floor(fmod(ms, MIN) / SEC);
        return s > 0 ? xasprintf("%ldm%lds", m, s) : xasprintf("%ldm", m);
    }
    if (ms < DAY) {
        long h = (long)floor(ms / HOUR), m = (long)floor(fmod(ms, HOUR) / MIN);
        return m > 0 ? xasprintf("%ldh%ldm", h, m) : xasprintf("%ldh", h);
    }
    long d = (long)floor(ms / DAY), h = (long)floor(fmod(ms, DAY) / HOUR);
    return h > 0 ? xasprintf("%ldd%ldh", d, h) : xasprintf("%ldd", d);
}

static double js_round(double v) { return floor(v + 0.5); }

char *usage_fmt_coarse(double ms) {
    double s = MAX(0.0, js_round(ms / 1000));
    if (s < 60) return xasprintf("%.0fs", s);
    double m = js_round(s / 60);
    if (m < 60) return xasprintf("%.0fm", m);
    double h = js_round(m / 60);
    if (h < 48) return xasprintf("%.0fh", h);
    return xasprintf("%.0fd", js_round(h / 24));
}

/* toFixed(d) then Number(...).toString(): trailing zeros dropped */
static char *fixed_trim(double v, int d) {
    char *s = xasprintf("%.*f", d, v);
    if (strchr(s, '.')) {
        size_t n = strlen(s);
        while (n > 0 && s[n - 1] == '0') s[--n] = 0;
        if (n > 0 && s[n - 1] == '.') s[--n] = 0;
    }
    if (!strcmp(s, "-0")) strcpy(s, "0");
    return s;
}

static char *trim1(double v) {
    char *s = xasprintf("%.1f", v);
    size_t n = strlen(s);
    if (n >= 2 && !strcmp(s + n - 2, ".0")) s[n - 2] = 0;
    return s;
}

char *usage_fmt_number(double n, int max_frac) {
    if (n < 1000) {
        if (max_frac < 0 || n == floor(n)) return fixed_trim(n, n == floor(n) ? 0 : 15);
        return fixed_trim(n, max_frac);
    }
    double div;
    const char *suf;
    bool one;
    if (n < 1e4) div = 1e3, suf = "K", one = true;
    else if (n < 1e6) div = 1e3, suf = "K", one = false;
    else if (n < 1e7) div = 1e6, suf = "M", one = true;
    else if (n < 1e9) div = 1e6, suf = "M", one = false;
    else if (n < 1e10) div = 1e9, suf = "B", one = true;
    else div = 1e9, suf = "B", one = false;
    char *v = one ? trim1(n / div) : xasprintf("%.0f", js_round(n / div));
    char *r = xasprintf("%s%s", v, suf);
    free(v);
    return r;
}

char *usage_fmt_grouped(double n, int max_frac) {
    double p = pow(10, max_frac);
    double v = js_round(fabs(n) * p) / p;
    char *s = fixed_trim(v, max_frac);
    char *dot = strchr(s, '.');
    size_t ilen = dot ? (size_t)(dot - s) : strlen(s);
    sb_t b = SB_INIT;
    if (n < 0 && v != 0) sb_putc(&b, '-');
    for (size_t i = 0; i < ilen; i++) {
        if (i && (ilen - i) % 3 == 0) sb_putc(&b, ',');
        sb_putc(&b, s[i]);
    }
    if (dot) sb_append(&b, dot);
    free(s);
    return sb_detach(&b);
}

char *usage_fmt_compact(double n) {
    double a = fabs(n);
    if (a < 1000) return fixed_trim(js_round(n * 10) / 10, 1);
    double div = a < 1e6 ? 1e3 : a < 1e9 ? 1e6 : a < 1e12 ? 1e9 : 1e12;
    const char *suf = a < 1e6 ? "K" : a < 1e9 ? "M" : a < 1e12 ? "B" : "T";
    double v = n / div;
    /* Intl rounds to 2 significant digits under 100, then caps at 1 fraction digit */
    char *s = fixed_trim(js_round(v * 10) / 10, fabs(v) >= 100 ? 0 : 1);
    if (fabs(v) >= 100) {
        free(s);
        s = xasprintf("%.0f", js_round(v));
    }
    char *r = xasprintf("%s%s", s, suf);
    free(s);
    return r;
}

char *usage_one_line(const char *s) {
    if (!s) return xstrdup("");
    sb_t b = SB_INIT;
    bool ws = false;
    for (const char *p = s; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            if (!ws) sb_putc(&b, ' ');
            ws = true;
            continue;
        }
        ws = false;
        sb_putc(&b, *p);
    }
    size_t n;
    char *r = out_sanitize(sb_cstr(&b), b.len, &n);
    sb_free(&b);
    return r;
}

/* ---------- /usage text (ACP / non-TUI) ---------- */

static const char *meta_str(const json_t *r, const char *k) {
    const char *v = json_get_str(json_get_obj(r, "metadata"), k);
    return v && *v ? v : NULL;
}

static char *codex_plan(const json_t *r) {
    if (!str_eq(json_get_str(r, "provider"), "openai-codex")) return NULL;
    const char *p = meta_str(r, "planType");
    if (!p) return NULL;
    char *t = str_trim(p);
    if (!*t) {
        free(t);
        return NULL;
    }
    char *o = usage_one_line(t);
    free(t);
    return o;
}

/* formatCodexUsageReportLabel(report, peers, base, undefined, includePlan, "parenthesized") */
char *usage_codex_label(const json_t *r, const json_t *peers, const char *base, bool include_plan) {
    const char *email = meta_str(r, "email");
    bool collision = false;
    size_t idx = 0;
    json_t *p;
    json_arr_foreach(peers, i, p) {
        if (p == r) idx = i;
        else if (email && str_eq(meta_str(p, "email"), email)) collision = true;
    }
    char *org = NULL;
    if (collision) {
        const char *o = meta_str(r, "orgId") ? meta_str(r, "orgId") : meta_str(r, "accountId");
        org = o ? xstrdup(o) : xasprintf("account %zu", idx + 1);
    }
    char *ident = usage_one_line(base);
    sb_t b = SB_INIT;
    sb_append(&b, ident);
    if (org && *org && strcmp(org, base)) {
        char *c = usage_one_line(org);
        sb_appendf(&b, " (%s)", c);
        free(c);
    }
    char *plan = include_plan ? codex_plan(r) : NULL;
    if (plan) sb_appendf(&b, " \xc2\xb7 plan: %s", plan);
    free(plan);
    free(org);
    free(ident);
    return sb_detach(&b);
}

static char *window_suffix(const char *label, const char *win) {
    if (!win) return xstrdup("");
    if (!strcasecmp(win, "quota window") || str_istr(label, win)) return xstrdup("");
    return xasprintf(" \xe2\x80\x94 %s", win);
}

static char *amount_text(const json_t *limit) {
    const json_t *a = json_get_obj(limit, "amount");
    double used, uf, rf;
    bool has_used = num(a, "used", &used);
    bool has_uf = num(a, "usedFraction", &uf);
    if (!has_used && has_uf) used = uf * 100, has_used = true;
    bool has_rf = num(a, "remainingFraction", &rf);
    if (!has_rf && has_uf) rf = MAX(0.0, 1 - uf), has_rf = true;
    const char *unit = json_get_str(a, "unit");
    char *u = str_eq(unit, "percent") ? xstrdup("%") : xasprintf(" %s", unit ? unit : "unknown");
    char *r = has_used ? xasprintf("%.2f%s used", used, u) : xstrdup("unknown used");
    if (has_rf) {
        char *t = xasprintf("%s (%.1f%% left)", r, rf * 100);
        free(r);
        r = t;
    }
    free(u);
    return r;
}

static char *report_account(const json_t *r, const json_t *peers, const json_t *limit, size_t index) {
    bool codex = str_eq(json_get_str(r, "provider"), "openai-codex");
    const char *org = meta_str(r, "orgName") ? meta_str(r, "orgName") : meta_str(r, "orgId");
    const json_t *sc = json_get_obj(limit, "scope");
    const char *ident;
    bool with_org = true;
    char *fallback = NULL;
    if (meta_str(r, "email")) ident = meta_str(r, "email");
    else if (meta_str(r, "accountId") || (json_get_str(sc, "accountId") && *json_get_str(sc, "accountId")))
        ident = meta_str(r, "accountId") ? meta_str(r, "accountId") : json_get_str(sc, "accountId");
    else {
        with_org = false;
        ident = meta_str(r, "projectId") ? meta_str(r, "projectId") : json_get_str(sc, "projectId");
        if (!ident || !*ident) ident = fallback = xasprintf("account %zu", index + 1);
    }
    char *out;
    if (codex) out = usage_codex_label(r, peers, ident, true);
    else if (with_org && org && strcmp(org, ident)) out = xasprintf("%s (%s)", ident, org);
    else out = xstrdup(ident);
    free(fallback);
    return out;
}

static bool identity_matches(const json_t *r, const json_t *id); /* reportMatchesActiveAccount */

/* renderAsciiBar(fraction, 24) unstyled */
static char *ascii_bar(bool has, double f) {
    sb_t b = SB_INIT;
    sb_putc(&b, '[');
    if (!has) {
        for (int i = 0; i < 24; i++) sb_append(&b, "\xc2\xb7");
        sb_putc(&b, ']');
        return sb_detach(&b);
    }
    double c = MIN(MAX(f, 0.0), 1.0);
    int filled = (int)js_round(c * 24);
    for (int i = 0; i < 24; i++) sb_append(&b, i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91");
    sb_appendf(&b, "] %.0f%%", js_round(c * 100));
    return sb_detach(&b);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

char *usage_report_text(const json_t *reports, int64_t now, const char *active_provider, const json_t *active_identity) {
    json_t *disp = usage_collapse_shared(reports), *r;
    double latest = -INFINITY;
    json_arr_foreach(disp, i, r) latest = MAX(latest, json_get_num(r, "fetchedAt", 0));
    sb_t b = SB_INIT;
    sb_append(&b, "```\nUsage");
    if (latest > 0) {
        char *d = usage_fmt_coarse((double)now - latest);
        sb_appendf(&b, " (%s ago)", d);
        free(d);
    }
    strvec_t provs = {0};
    json_arr_foreach(disp, i, r) if (!strvec_contains(&provs, json_get_str(r, "provider"))) strvec_push(&provs, json_get_str(r, "provider"));
    qsort(provs.v, provs.n, sizeof *provs.v, cmp_str);
    for (size_t pi = 0; pi < provs.n; pi++) {
        const char *prov = provs.v[pi];
        char *name = usage_provider_name(prov);
        sb_appendf(&b, "\n\n%s", name);
        free(name);
        json_t *peers = json_arr();
        json_arr_foreach(disp, i, r) if (str_eq(json_get_str(r, "provider"), prov)) json_push(peers, json_clone(r));
        strvec_t notes = {0};
        json_arr_foreach(peers, i, r) {
            json_t *n;
            json_arr_foreach(json_get_arr(r, "notes"), k, n) if (json_sv(n) && !strvec_contains(&notes, json_sv(n))) strvec_push(&notes, json_sv(n));
        }
        for (size_t k = 0; k < notes.n; k++) {
            char *t = usage_one_line(notes.v[k]);
            sb_appendf(&b, "\n  %s", t);
            free(t);
        }
        strvec_free(&notes);
        const json_t *active = str_eq(prov, active_provider) ? active_identity : NULL;
        json_arr_foreach(peers, ri, r) {
            bool in_use = active && identity_matches(r, active);
            usage_reset_summary_t rs;
            if (usage_reset_summary(json_get_obj(r, "resetCredits"), now, &rs) && rs.banked > 0) {
                const char *idv = meta_str(r, "email") ? meta_str(r, "email") : meta_str(r, "accountId") ? meta_str(r, "accountId") : "account";
                char *label;
                if (str_eq(prov, "openai-codex")) label = usage_codex_label(r, peers, idv, true);
                else {
                    const char *org = meta_str(r, "orgName") ? meta_str(r, "orgName") : json_get_str(json_get_obj(r, "metadata"), "orgId");
                    char *raw = org && strcmp(org, idv) ? xasprintf("%s (%s)", idv, org) : xstrdup(idv);
                    label = usage_one_line(raw);
                    free(raw);
                }
                char *avail = rs.redeemable == rs.banked ? xstrdup("available") : xasprintf("%lld usable now", (long long)rs.redeemable);
                sb_appendf(&b, "\n- %s: %lld saved rate-limit reset%s \xe2\x80\x94 %s \xe2\x80\x94 /usage reset to spend", label,
                           (long long)rs.banked, rs.banked == 1 ? "" : "s", avail);
                free(avail);
                free(label);
                if (rs.soonest_expiry) {
                    double ems;
                    char day[11] = {0};
                    snprintf(day, sizeof day, "%.10s", rs.soonest_expiry);
                    if (trace_parse_iso_ms(rs.soonest_expiry, &ems) && ems - (double)now > 0) {
                        char *d = usage_fmt_coarse(ems - (double)now);
                        sb_appendf(&b, "\n  soonest expires in %s (%s)", d, day);
                        free(d);
                    } else sb_appendf(&b, "\n  expired (%s)", day);
                }
                if (rs.redeemable == 0 && rs.unavailable_reason) {
                    char *t = usage_one_line(rs.unavailable_reason);
                    sb_appendf(&b, "\n  unavailable: %s", t);
                    free(t);
                }
            }
            usage_reset_summary_free(&rs);
            json_t *limits = json_get_arr(r, "limits");
            if (json_len(limits) == 0) {
                const char *email = meta_str(r, "email") ? meta_str(r, "email") : "account";
                char *label = str_eq(prov, "openai-codex") ? usage_codex_label(r, peers, email, true) : xstrdup(email);
                sb_appendf(&b, "\n- %s: no limits reported", label);
                free(label);
                continue;
            }
            json_t *l;
            json_arr_foreach(limits, li, l) {
                const json_t *w = json_get_obj(l, "window");
                const char *win = json_get_str(w, "label") ? json_get_str(w, "label") : json_get_str(json_get_obj(l, "scope"), "windowId");
                char *title = usage_limit_title(l);
                char *suf = window_suffix(json_get_str(l, "label") ? json_get_str(l, "label") : "", win);
                sb_appendf(&b, "\n- %s%s", title, suf);
                free(title);
                free(suf);
                char *acct = report_account(r, peers, l, li), *amt = amount_text(l);
                sb_appendf(&b, "\n  %s: %s%s", acct, amt, in_use ? "  \xe2\x86\x90 in use by this session" : "");
                free(acct);
                free(amt);
                double uf;
                bool has = num(json_get_obj(l, "amount"), "usedFraction", &uf);
                char *bar = ascii_bar(has, uf);
                sb_appendf(&b, "\n  %s", bar);
                free(bar);
                double ra;
                if (num(w, "resetsAt", &ra) && ra > now) {
                    char *d = usage_fmt_coarse(ra - (double)now);
                    sb_appendf(&b, "\n  %s in %s", json_get_str(w, "resetLabel") ? json_get_str(w, "resetLabel") : "resets", d);
                    free(d);
                }
                json_t *notes2 = json_get_arr(l, "notes"), *n;
                if (json_len(notes2) > 0) {
                    sb_append(&b, "\n  ");
                    json_arr_foreach(notes2, k, n) {
                        char *t = usage_one_line(json_sv(n));
                        sb_appendf(&b, "%s%s", k ? " \xe2\x80\xa2 " : "", t);
                        free(t);
                    }
                }
            }
        }
        json_free(peers);
    }
    sb_append(&b, "\n```");
    strvec_free(&provs);
    json_free(disp);
    return sb_detach(&b);
}

char *usage_session_text(const json_t *st) {
    return xasprintf("Usage\nInput tokens: %lld\nOutput tokens: %lld\nCache read tokens: %lld\nCache write tokens: %lld\n"
                     "Total tokens: %lld\nPremium requests: %lld\nCost: $%.6f",
                     (long long)json_get_int(st, "input", 0), (long long)json_get_int(st, "output", 0),
                     (long long)json_get_int(st, "cacheRead", 0), (long long)json_get_int(st, "cacheWrite", 0),
                     (long long)json_get_int(st, "totalTokens", 0), (long long)json_get_int(st, "premiumRequests", 0),
                     json_get_num(st, "cost", 0));
}

/* ---------- active account ---------- */

static char *norm(const char *v) {
    if (!v) return NULL;
    char *t = str_trim(v);
    if (!*t) {
        free(t);
        return NULL;
    }
    char *l = str_lower(t);
    free(t);
    return l;
}

static bool eqn(const char *a, const char *b) { return a && b && !strcmp(a, b); }

bool usage_limit_matches_active(const json_t *r, const json_t *limit, const json_t *id) {
    if (!id) return false;
    const json_t *md = json_get_obj(r, "metadata");
    char *aa = norm(json_get_str(id, "accountId")), *ae = norm(json_get_str(id, "email")),
         *ap = norm(json_get_str(id, "projectId")), *ao = norm(json_get_str(id, "orgId")),
         *ro = norm(json_get_str(md, "orgId")), *re = norm(json_get_str(md, "email"));
    bool res = false, done = false;
    if (ao || ro) {
        if (!eqn(ao, ro) && !(ao == NULL && ro == NULL)) done = true, res = false;
        else if (!aa && !ae && !ap) done = true, res = true;
    }
    if (!done && ae && re) done = true, res = !strcmp(ae, re);
    if (!done && aa) {
        char *ra = norm(json_get_str(md, "accountId"));
        if (!ra) ra = norm(json_get_str(md, "account_id"));
        char *sa = norm(json_get_str(json_get_obj(limit, "scope"), "accountId"));
        if (eqn(ra, aa) || eqn(sa, aa)) done = true, res = true;
        free(ra);
        free(sa);
    }
    if (!done && ap) {
        char *rp = norm(json_get_str(md, "projectId")), *sp = norm(json_get_str(json_get_obj(limit, "scope"), "projectId"));
        if (eqn(rp, ap) || eqn(sp, ap)) res = true;
        free(rp);
        free(sp);
    }
    free(aa), free(ae), free(ap), free(ao), free(ro), free(re);
    return res;
}

static bool identity_matches(const json_t *r, const json_t *id) {
    json_t *l;
    json_arr_foreach(json_get_arr(r, "limits"), i, l) if (usage_limit_matches_active(r, l, id)) return true;
    return false;
}

bool usage_report_matches_active(const json_t *r, const json_t *id) { return identity_matches(r, id); }

json_t *usage_active_identity(const char *provider) {
    if (!provider) return NULL;
    json_t *e = auth_get_credential_raw(provider);
    if (!e || !str_eq(json_get_str(e, "type"), "oauth")) {
        json_free(e);
        return NULL;
    }
    json_t *id = json_obj();
    static const char *const keys[] = {"email", "accountId", "projectId", "orgId", "orgName"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++)
        if (json_get_str(e, keys[i]) && *json_get_str(e, keys[i])) json_set_str(id, keys[i], json_get_str(e, keys[i]));
    json_free(e);
    return id;
}
