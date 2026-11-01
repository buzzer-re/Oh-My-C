/* provider_gitlab_duo.c - GitLab Duo (non-agentic) models: provider "gitlab-duo" (port of omp
 * packages/ai/src/providers/gitlab-duo.ts).
 *
 * The GitLab credential (OAuth access token or PAT) is exchanged at
 * POST https://gitlab.com/api/v4/ai/third_party_agents/direct_access for a short-lived AI-gateway token plus
 * routing headers (cached 25 minutes per credential). The request then goes to GitLab's Cloud Connector proxy
 * (cloud.gitlab.com/ai/v1/proxy/{anthropic,openai}) through the regular Anthropic Messages / OpenAI Responses /
 * Chat Completions implementations, with the model's upstream id. Routing follows the catalog `api-routes` for
 * gitlab-duo: Claude aliases -> anthropic-messages (OAuth wire), the codex aliases -> openai-responses, the rest
 * -> openai-completions. */
#include "ai.h"
#include "provider_anthropic.h"

#include <pthread.h>

#define GITLAB_COM_URL "https://gitlab.com"
#define ANTHROPIC_PROXY_URL "https://cloud.gitlab.com/ai/v1/proxy/anthropic/"
#define OPENAI_PROXY_URL "https://cloud.gitlab.com/ai/v1/proxy/openai/v1"
#define DIRECT_ACCESS_TTL_MS (25LL * 60 * 1000)

/* catalog GITLAB_DUO_MODEL_IDENTITIES: alias -> upstream model id */
static const char *const IDENTITIES[][2] = {
    {"duo-chat-opus-4-6", "claude-opus-4-6"},
    {"duo-chat-sonnet-4-6", "claude-sonnet-4-6"},
    {"duo-chat-opus-4-5", "claude-opus-4-5-20251101"},
    {"duo-chat-sonnet-4-5", "claude-sonnet-4-5-20250929"},
    {"duo-chat-haiku-4-5", "claude-haiku-4-5-20251001"},
    {"duo-chat-gpt-5-1", "gpt-5.1-2025-11-13"},
    {"duo-chat-gpt-5-2", "gpt-5.2-2025-12-11"},
    {"duo-chat-gpt-5-mini", "gpt-5-mini-2025-08-07"},
    {"duo-chat-gpt-5-codex", "gpt-5-codex"},
    {"duo-chat-gpt-5-2-codex", "gpt-5.2-codex"},
};

/* resolveGitLabDuoModelIdentity: an alias or an upstream id; returns the upstream id (static) or NULL */
const char *gitlab_duo_upstream_model(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < ARRAY_LEN(IDENTITIES); i++)
        if (!strcmp(IDENTITIES[i][0], id)) return IDENTITIES[i][1];
    for (size_t i = 0; i < ARRAY_LEN(IDENTITIES); i++)
        if (!strcmp(IDENTITIES[i][1], id)) return IDENTITIES[i][1];
    return NULL;
}

/* behavior.kdl api-routes provider="gitlab-duo" default="openai-completions" */
const char *gitlab_duo_route(const char *id) {
    static const char *const anth[] = {"duo-chat-opus-", "duo-chat-sonnet-", "duo-chat-haiku-",
                                       "claude-opus-",   "claude-sonnet-",   "claude-haiku-"};
    static const char *const resp[] = {"duo-chat-gpt-5-codex", "duo-chat-gpt-5-2-codex", "gpt-5-codex", "gpt-5.2-codex"};
    for (size_t i = 0; i < ARRAY_LEN(anth); i++)
        if (str_starts(id, anth[i])) return "anthropic-messages";
    for (size_t i = 0; i < ARRAY_LEN(resp); i++)
        if (!strcmp(id, resp[i])) return "openai-responses";
    return "openai-completions";
}

/* ---------- direct access token cache ---------- */

typedef struct {
    uint64_t key; /* hash of the GitLab credential (never the raw token) */
    char *token;
    json_t *headers;
    int64_t expires_at;
} da_entry_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static VEC(da_entry_t) g_cache;

void gitlab_duo_clear_direct_access_cache(void) {
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_cache.n; i++) {
        free(g_cache.v[i].token);
        json_free(g_cache.v[i].headers);
    }
    g_cache.n = 0;
    pthread_mutex_unlock(&g_mu);
}

/* 0 with owned *token / *headers, else -1 and *err */
static int direct_access(const char *gitlab_token, const ai_stream_opts_t *o, char **token, json_t **headers, char **err) {
    uint64_t key = hash_xx64(gitlab_token, strlen(gitlab_token), 0x6c616264756fULL);
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_cache.n; i++) {
        if (g_cache.v[i].key == key && g_cache.v[i].expires_at > now_ms()) {
            *token = xstrdup(g_cache.v[i].token);
            *headers = json_clone(g_cache.v[i].headers);
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_mu);
    char *url = ai_rewrite_url(GITLAB_COM_URL "/api/v4/ai/third_party_agents/direct_access");
    http_headers_t h = {0};
    char *auth = xasprintf("Bearer %s", gitlab_token);
    http_headers_set(&h, "Authorization", auth);
    http_headers_set(&h, "Content-Type", "application/json");
    free(auth);
    const char *body = "{\"feature_flags\":{\"DuoAgentPlatformNext\":true}}";
    http_req_t req = {.method = "POST", .url = url, .headers = &h, .body = body, .body_len = strlen(body),
                      .timeout_ms = 30000, .cancel = o ? o->cancel : NULL};
    http_resp_t r;
    int rc = http_request(&req, &r);
    http_headers_free(&h);
    free(url);
    if (rc != 0) {
        *err = xasprintf("Failed to get GitLab Duo direct access token: %s", r.error ? r.error : "network error");
        http_resp_free(&r);
        return -1;
    }
    if (r.status < 200 || r.status >= 300) {
        const char *detail = r.body ? r.body : "";
        size_t dl = MIN(strlen(detail), (size_t)2000);
        *err = r.status == 403 ? xasprintf("GitLab Duo access denied. Ensure Duo is enabled for this account. %.*s", (int)dl, detail)
                               : xasprintf("Failed to get GitLab Duo direct access token: %d %.*s", r.status, (int)dl, detail);
        http_resp_free(&r);
        return -1;
    }
    json_t *j = json_parse_str(r.body ? r.body : "");
    http_resp_free(&r);
    const char *tok = json_get_str(j, "token");
    json_t *hd = json_get_obj(j, "headers");
    if (!tok) {
        *err = xstrdup("GitLab Duo direct access response missing token");
        json_free(j);
        return -1;
    }
    if (!hd) {
        *err = xstrdup("GitLab Duo direct access response missing headers");
        json_free(j);
        return -1;
    }
    /* only string header values are usable */
    json_t *clean = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(hd, i, k, v) if (json_is_str(v)) json_set_str(clean, k, json_sv(v));
    *token = xstrdup(tok);
    *headers = json_clone(clean);
    pthread_mutex_lock(&g_mu);
    da_entry_t e = {key, xstrdup(tok), clean, now_ms() + DIRECT_ACCESS_TTL_MS};
    bool replaced = false;
    for (size_t i = 0; i < g_cache.n && !replaced; i++) {
        if (g_cache.v[i].key == key) {
            free(g_cache.v[i].token);
            json_free(g_cache.v[i].headers);
            g_cache.v[i] = e;
            replaced = true;
        }
    }
    if (!replaced) vec_push(&g_cache, e);
    pthread_mutex_unlock(&g_mu);
    json_free(j);
    return 0;
}

static json_t *fail(const ai_model_t *m, ai_event_cb cb, void *ud, const char *msg) {
    ai_builder_t b;
    aib_init(&b, m, cb, ud);
    json_t *r = aib_finish(&b, "error", msg);
    aib_free(&b);
    return r;
}

/* per-api compat defaults borrowed from any catalog row on that api (the catalog keeps them per api) */
static json_t *api_defaults(const char *api, const ai_model_t *fallback) {
    if (fallback && str_eq(fallback->api, api)) return fallback->compat_defaults;
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *c = catalog_at(i);
        if (str_eq(c->api, api)) return c->compat_defaults;
    }
    return NULL;
}

json_t *provider_gitlab_duo_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                                   ai_event_cb cb, void *ud) {
    ai_stream_opts_t def;
    if (!o) {
        ai_stream_opts_init(&def);
        o = &def;
    }
    char *env_key = NULL;
    const char *key = o->api_key;
    if (str_empty(key)) key = env_key = ai_env_api_key(m->provider);
    if (str_empty(key)) {
        free(env_key);
        return fail(m, cb, ud, "Missing GitLab access token. Run /login gitlab-duo or set GITLAB_TOKEN.");
    }
    const char *upstream = gitlab_duo_upstream_model(m->id);
    if (!upstream) {
        char *e = xasprintf("Unsupported GitLab Duo model: %s", m->id);
        json_t *r = fail(m, cb, ud, e);
        free(e);
        free(env_key);
        return r;
    }
    const char *api = gitlab_duo_route(m->id);
    char *token = NULL, *err = NULL;
    json_t *hdrs = NULL;
    if (direct_access(key, o, &token, &hdrs, &err) != 0) {
        json_t *r = fail(m, cb, ud, err);
        free(err);
        free(env_key);
        return r;
    }
    free(env_key);
    /* headers: direct-access routing headers, then the caller's */
    if (json_is_obj(o->headers)) {
        const char *k;
        json_t *v;
        json_obj_foreach(o->headers, i, k, v) if (json_is_str(v)) json_set_str(hdrs, k, json_sv(v));
    }
    ai_model_t pm = *m;
    pm.id = upstream;
    pm.api = api;
    pm.base_url = str_eq(api, "anthropic-messages") ? ANTHROPIC_PROXY_URL : OPENAI_PROXY_URL;
    pm.compat_defaults = api_defaults(api, m);
    ai_stream_opts_t po = *o;
    po.api_key = token;
    po.headers = hdrs;
    if (po.max_tokens <= 0 && m->max_tokens > 0) po.max_tokens = m->max_tokens;
    json_t *msg;
    if (str_eq(api, "anthropic-messages")) msg = provider_anthropic_stream_ex(&pm, ctx, &po, 1, cb, ud);
    else if (str_eq(api, "openai-responses")) msg = provider_openai_responses_stream(&pm, ctx, &po, cb, ud);
    else msg = provider_openai_completions_stream(&pm, ctx, &po, cb, ud);
    json_free(hdrs);
    free(token);
    return msg;
}
