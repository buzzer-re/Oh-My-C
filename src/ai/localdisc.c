/* localdisc.c - implicit local model servers (port of omp model-registry #addImplicitDiscoverableProviders +
 * model-discovery.ts discoverOllamaModels / discoverLlamaCppModels / lmStudioModelManagerOptions).
 *
 * Providers (skipped when models.yml configures them or disabledProviders lists them):
 *   ollama     OLLAMA_BASE_URL | OLLAMA_HOST | http://127.0.0.1:11434   GET /api/tags + POST /api/show per model
 *   llama.cpp  LLAMA_CPP_BASE_URL | http://127.0.0.1:8080, then :9931     GET /models + GET /props
 *   lm-studio  LM_STUDIO_BASE_URL | http://127.0.0.1:1234/v1            GET /models + GET /api/v0/models
 * All are keyless and "optional" unless their env var names the endpoint (an optional provider that answered
 * nothing stays hidden). Probes run in parallel threads with omp's timeouts (250 ms list / 150 ms metadata on
 * loopback, 10 s for a remote host). Results are cached in <agent>/local-models.json for 24 h like omp's model
 * cache: the startup refresh ("online-if-uncached") reuses a fresh cache except for llama.cpp (always probed,
 * its lazily loaded instances change); catalog_discover_local(true) (F5) always probes. Only a server that
 * answered updates the cache. */
#include "ai.h"
#include "kiro.h"
#include "core/fs.h"

#include <ctype.h>
#include <pthread.h>
#include <strings.h>

#define DISC_DEFAULT_CTX 128000
#define DISC_DEFAULT_MAX_TOKENS 32768
#define DISC_CACHE_TTL_MS (24LL * 60 * 60 * 1000)
#define DISC_REMOTE_TIMEOUT_MS 10000

typedef enum { D_OLLAMA, D_LLAMACPP, D_LMSTUDIO } disc_kind_t;

typedef struct {
    disc_kind_t kind;
    const char *provider;
    const char *api;
    char *base;      /* primary endpoint */
    char *fallback;  /* llama.cpp second default port */
    bool optional;
    char *api_key;   /* the provider's credential (omp sends it on discovery too), NULL when none */
    /* results */
    const char *endpoint_env; /* the env var that named this endpoint (omp #implicitProviderEndpointEnv), or NULL */
    char *display_url;        /* the endpoint as omp's baseUrl names it, for the warning */
    /* results */
    json_t *models;  /* array of model objects, NULL when nothing answered */
    char *error;
    char *detail;    /* the failed list request's error in omp's (Bun fetch) wording */
} probe_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *g_state; /* {provider: {status, optional, error?, fetchedAt?}} */
static bool g_started;
static json_t *g_last_warning;  /* {provider: error} (omp #lastDiscoveryWarnings: one warning per distinct error) */
static strvec_t g_config_warnings; /* omp #configWarnings, drained by the interactive startup */

/* redactUrlForLog: the URL without userinfo, serialized like WHATWG URL (a bare origin gains its trailing `/`) */
static char *redact_url(const char *url) {
    const char *sch = strstr(url, "://");
    if (!sch) return xstrdup(url);
    const char *host = sch + 3;
    const char *end = host + strcspn(host, "/?#");
    const char *at = NULL;
    for (const char *q = host; q < end; q++)
        if (*q == '@') at = q;
    if (at) host = at + 1;
    return xasprintf("%.*s%.*s%s%s", (int)(sch + 3 - url), url, (int)(end - host), host, *end == '/' ? "" : "/", end);
}

/* #warnProviderDiscoveryFailure: a built-in provider an env var redirected failed discovery */
static void warn_discovery_failure(const probe_t *p) {
    const char *err = p->detail ? p->detail : p->error ? p->error : "unknown error";
    pthread_mutex_lock(&g_mu);
    if (!g_last_warning) g_last_warning = json_obj();
    bool repeat = str_eq(json_get_str(g_last_warning, p->provider), err);
    if (!repeat) json_set_str(g_last_warning, p->provider, err);
    pthread_mutex_unlock(&g_mu);
    if (repeat || !p->endpoint_env || !p->display_url || !*p->display_url) return;
    char *url = redact_url(p->display_url);
    /* the endpoint may embed `user:pass@`, and errors can echo it; whitespace runs collapse to one space */
    char *d = str_replace(err, p->display_url, url); /* replaceAll */
    sb_t b = {0};
    bool sp = false;
    for (const char *q = d; *q; q++) {
        if (isspace((unsigned char)*q)) {
            sp = b.len > 0;
            continue;
        }
        if (sp) sb_putc(&b, ' ');
        sp = false;
        sb_putc(&b, *q);
    }
    char *w = xasprintf("Built-in %s provider: model discovery at %s (from %s) failed. Fix or unset %s, or add \"%s\" to "
                        "disabledProviders. Error: %s",
                        p->provider, url, p->endpoint_env, p->endpoint_env, p->provider, b.s ? b.s : "");
    pthread_mutex_lock(&g_mu);
    strvec_push(&g_config_warnings, w);
    pthread_mutex_unlock(&g_mu);
    free(w);
    sb_free(&b);
    free(d);
    free(url);
}

/* omp #publishModelsFileWarnings: a (re)load of models.yml replaces the findings of the previous load still pending */
static strvec_t g_models_file_warnings;
void catalog_publish_models_file_warnings(const strvec_t *w) {
    pthread_mutex_lock(&g_mu);
    if (g_models_file_warnings.n) {
        strvec_t keep = {0};
        for (size_t i = 0; i < g_config_warnings.n; i++) {
            bool published = false;
            for (size_t j = 0; j < g_models_file_warnings.n && !published; j++)
                published = !strcmp(g_config_warnings.v[i], g_models_file_warnings.v[j]);
            if (!published) strvec_push(&keep, g_config_warnings.v[i]);
        }
        strvec_free(&g_config_warnings);
        g_config_warnings = keep;
        strvec_free(&g_models_file_warnings);
    }
    for (size_t i = 0; w && i < w->n; i++) {
        strvec_push(&g_models_file_warnings, w->v[i]);
        strvec_push(&g_config_warnings, w->v[i]);
    }
    pthread_mutex_unlock(&g_mu);
}

/* the models.yml findings still pending, taken out of the config warnings (main.ts reports them at launch: stderr
 * without a TUI, warning notices in it), so the session's config-warning header keeps the discovery ones */
void catalog_take_models_file_warnings(strvec_t *out) {
    pthread_mutex_lock(&g_mu);
    strvec_t keep = {0};
    for (size_t i = 0; i < g_config_warnings.n; i++) {
        bool mine = false;
        for (size_t j = 0; j < g_models_file_warnings.n && !mine; j++)
            mine = !strcmp(g_config_warnings.v[i], g_models_file_warnings.v[j]);
        strvec_push(mine ? out : &keep, g_config_warnings.v[i]);
    }
    strvec_free(&g_config_warnings);
    g_config_warnings = keep;
    strvec_free(&g_models_file_warnings);
    pthread_mutex_unlock(&g_mu);
}

void catalog_drain_config_warnings(strvec_t *out) {
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_config_warnings.n; i++) strvec_push(out, g_config_warnings.v[i]);
    strvec_free(&g_config_warnings);
    pthread_mutex_unlock(&g_mu);
}

extern json_t *settings_get(const char *path) __attribute__((weak));
extern char *auth_get_api_key(const char *provider) __attribute__((weak));

/* stored login or env key; a server that requires auth answers discovery only with it */
static char *probe_key(const char *provider) {
    return auth_get_api_key ? auth_get_api_key(provider) : ai_env_api_key(provider);
}

static bool provider_disabled(const char *p) {
    json_t *d = settings_get ? settings_get("disabledProviders") : NULL, *x;
    json_arr_foreach(d, i, x) if (str_eq(json_sv(x), p)) return true;
    return false;
}

static char *trim_slashes(const char *s) {
    char *r = xstrdup(s);
    size_t n = strlen(r);
    while (n > 0 && (r[n - 1] == '/' || r[n - 1] == ' ')) r[--n] = 0;
    return r;
}

/* OLLAMA_HOST may be "host", "host:port" or a URL; default port 11434 for http */
static char *ollama_host_url(const char *h) {
    if (!h || !*h) return NULL;
    char *t = trim_slashes(h);
    char *u = strstr(t, "://") ? xstrdup(t) : xasprintf("http://%s", t);
    free(t);
    url_t pu;
    if (url_parse(u, &pu) != 0) {
        free(u);
        return NULL;
    }
    bool has_port = strchr(strstr(u, "://") + 3, ':') != NULL;
    char *r = has_port || !str_starts(u, "http://") ? xstrdup(u) : xasprintf("%s:11434", u);
    url_free(&pu);
    free(u);
    return r;
}

/* omp discoveryProbeTimeoutMs: tight cap on loopback, 10 s for anything else */
static int64_t probe_timeout(const char *base, int64_t loopback_ms) {
    url_t u;
    if (url_parse(base, &u) != 0) return loopback_ms;
    const char *h = u.host ? u.host : "";
    bool lo = !strcmp(h, "localhost") || !strcmp(h, "0.0.0.0") || !strcmp(h, "::1") ||
              str_starts(h, "127.");
    url_free(&u);
    return lo ? loopback_ms : DISC_REMOTE_TIMEOUT_MS;
}

/* GET/POST returning parsed JSON on 2xx; *answered is set when any HTTP status came back */
/* JavaScriptCore's JSON.parse message for the start of an unparsable body (the cases a local server produces) */
static char *jsc_json_error(const char *b, size_t n) {
    size_t i = 0;
    while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r')) i++;
    if (i >= n) return xstrdup("JSON Parse error: Unexpected EOF");
    if (isalpha((unsigned char)b[i]) || b[i] == '_' || b[i] == '$') {
        size_t j = i;
        while (j < n && (isalnum((unsigned char)b[j]) || b[j] == '_' || b[j] == '$')) j++;
        return xasprintf("JSON Parse error: Unexpected identifier \"%.*s\"", (int)(j - i), b + i);
    }
    return xasprintf("JSON Parse error: Unrecognized token '%c'", b[i]);
}

/* omc's transport errors in the wording of omp's fetch (Bun) / withTimeoutSignal, which the discovery warning shows */
static char *bun_fetch_error(const char *err) {
    if (!err) return xstrdup("Unable to connect. Is the computer able to access the url?");
    if (strstr(err, "Connection refused")) return xstrdup("ECONNREFUSED: Unable to connect. Is the computer able to access the url?");
    if (strstr(err, "timed out")) return xstrdup("The operation timed out.");
    if (!strncmp(err, "ECONNRESET", 10) || strstr(err, "connection closed before response") || strstr(err, "Connection reset"))
        return xstrdup("ECONNRESET: The socket connection was closed unexpectedly. For more information, pass `verbose: true` "
                       "in the second argument to fetch()");
    return xstrdup(err);
}

/* `detail` (optional) receives omp's error message for a failed request: the Bun fetch error, `HTTP <status> from
 * <url>` (DiscoveryHttpError) or the JSON.parse error */
static json_t *fetch_json_d(const char *url, const char *body, const char *api_key, int64_t timeout, bool *answered,
                            char **detail) {
    http_headers_t h = {0};
    http_headers_add(&h, "Accept", "application/json");
    if (api_key && *api_key) {
        char *bearer = xasprintf("Bearer %s", api_key);
        http_headers_add(&h, "Authorization", bearer);
        free(bearer);
    }
    if (body) http_headers_add(&h, "Content-Type", "application/json");
    http_req_t rq = {.method = body ? "POST" : "GET", .url = url, .headers = &h, .body = body,
                     .body_len = body ? strlen(body) : 0, .timeout_ms = timeout, .connect_timeout_ms = timeout,
                     .idle_timeout_ms = timeout, .no_proxy = true, .max_body = 16 << 20};
    http_resp_t rs = {0};
    json_t *j = NULL;
    if (http_request(&rq, &rs) == 0 && !rs.error) {
        if (answered) *answered = true;
        if (rs.status >= 200 && rs.status < 300 && rs.body) j = json_parse(rs.body, rs.body_len, NULL);
        if (detail && !j)
            *detail = rs.status >= 200 && rs.status < 300 ? jsc_json_error(rs.body ? rs.body : "", rs.body ? rs.body_len : 0)
                                                          : xasprintf("HTTP %d from %s", rs.status, url);
    } else if (detail) {
        *detail = bun_fetch_error(rs.error);
    }
    http_resp_free(&rs);
    http_headers_free(&h);
    return j;
}

static json_t *fetch_json(const char *url, const char *body, const char *api_key, int64_t timeout, bool *answered) {
    return fetch_json_d(url, body, api_key, timeout, answered, NULL);
}

static int64_t pos_int_str(const char *s) {
    if (!s) return 0;
    char *end;
    long long n = strtoll(s, &end, 10);
    return end != s && !*end && n > 0 ? n : 0;
}

static int64_t pos_int(const json_t *v) {
    if (json_is_num(v) && v->u.num.d > 0) return json_d2i(v->u.num.d);
    return pos_int_str(json_sv(v));
}

static json_t *new_model(const char *id, const char *name, const char *api, const char *provider, const char *base_url,
                         int reasoning, bool image, int64_t ctx, int64_t max_tokens) {
    json_t *m = json_obj();
    json_set_str(m, "id", id);
    json_set_str(m, "name", name ? name : id);
    json_set_str(m, "api", api);
    json_set_str(m, "provider", provider);
    json_set_str(m, "baseUrl", base_url);
    if (reasoning >= 0) json_set_bool(m, "reasoning", reasoning); /* < 0: no capability evidence (probed on first use) */
    json_t *in = json_arr();
    json_push_str(in, "text");
    if (image) json_push_str(in, "image");
    json_set(m, "input", in);
    json_t *cost = json_obj();
    json_set_int(cost, "input", 0);
    json_set_int(cost, "output", 0);
    json_set_int(cost, "cacheRead", 0);
    json_set_int(cost, "cacheWrite", 0);
    json_set(m, "cost", cost);
    json_set_int(m, "contextWindow", ctx > 0 ? ctx : DISC_DEFAULT_CTX);
    json_set_int(m, "maxTokens", max_tokens);
    json_set_bool(m, "discovered", true);
    return m;
}

/* ---------- ollama ---------- */

static int64_t ollama_ctx(const json_t *show) {
    /* runtime num_ctx from the modelfile parameters wins, then model_info.*context_length */
    const char *params = json_get_str(show, "parameters");
    for (const char *p = params; p && (p = strstr(p, "num_ctx")); p += 7) {
        if (p != params && p[-1] != '\n' && p[-1] != ' ') continue;
        const char *q = p + 7;
        while (*q == ' ' || *q == '\t') q++;
        long long n = strtoll(q, NULL, 10);
        if (n > 0) return n;
    }
    json_t *info = json_get_obj(show, "model_info");
    const char *k;
    json_t *v;
    json_obj_foreach(info, i, k, v) {
        if (!strcmp(k, "context_length") || str_ends(k, ".context_length")) {
            int64_t n = pos_int(v);
            if (n) return n;
        }
    }
    return 0;
}

static void probe_ollama(probe_t *p) {
    char *url = xasprintf("%s/api/tags", p->base);
    bool answered = false;
    json_t *tags = fetch_json_d(url, NULL, p->api_key, probe_timeout(p->base, 250), &answered, &p->detail);
    free(url);
    if (!tags) {
        if (!answered) p->error = xstrdup("unreachable");
        else p->error = xstrdup("bad response");
        return;
    }
    p->models = json_arr();
    char *v1 = xasprintf("%s/v1", p->base);
    json_t *it;
    json_arr_foreach(json_get_arr(tags, "models"), i, it) {
        const char *id = json_get_str(it, "model");
        if (!id || !*id) id = json_get_str(it, "name");
        if (!id || !*id) continue;
        const char *name = json_get_str(it, "name");
        char *surl = xasprintf("%s/api/show", p->base);
        json_t *req = json_obj();
        json_set_str(req, "model", id);
        char *body = json_dump(req, 0);
        json_free(req);
        json_t *show = fetch_json(surl, body, p->api_key, probe_timeout(p->base, 150), NULL);
        free(body);
        free(surl);
        int reasoning = -1;
        bool image = false;
        int64_t ctx = 0;
        if (show) {
            ctx = ollama_ctx(show);
            json_t *caps = json_get(show, "capabilities"), *c;
            if (json_is_arr(caps)) {
                reasoning = 0;
                json_arr_foreach(caps, k2, c) {
                    const char *s = json_sv(c);
                    if (!s) continue;
                    if (!strcasecmp(s, "thinking")) reasoning = true;
                    if (!strcasecmp(s, "vision") || !strcasecmp(s, "image")) image = true;
                }
            } else if (json_is_obj(caps)) {
                if (json_is_bool(json_get(caps, "thinking"))) reasoning = json_get_bool(caps, "thinking", false);
                image = json_get_bool(caps, "vision", false) || json_get_bool(caps, "image", false);
            }
            json_free(show);
        }
        int64_t max_tokens = ctx > 0 ? MIN(ctx, (int64_t)DISC_DEFAULT_MAX_TOKENS) : DISC_DEFAULT_MAX_TOKENS;
        json_push(p->models, new_model(id, name && *name ? name : id, p->api, p->provider, v1, reasoning, image,
                                       ctx ? ctx : DISC_DEFAULT_CTX, max_tokens));
    }
    free(v1);
    json_free(tags);
}

/* ---------- llama.cpp ---------- */

static int64_t llamacpp_ctx_from_args(const json_t *args) {
    json_t *a;
    json_arr_foreach(args, i, a) {
        const char *s = json_sv(a);
        if (!s) continue;
        const char *eq = strchr(s, '=');
        size_t fl = eq ? (size_t)(eq - s) : strlen(s);
        if (!((fl == 10 && !strncmp(s, "--ctx-size", 10)) || (fl == 2 && !strncmp(s, "-c", 2)))) continue;
        int64_t n = eq ? pos_int_str(eq + 1) : pos_int(json_at(args, i + 1));
        if (n) return n;
    }
    return 0;
}

static bool probe_llamacpp_at(probe_t *p, const char *base) {
    char *url = xasprintf("%s/models", base), *purl = xasprintf("%s/props", base);
    bool answered = false;
    free(p->detail);
    p->detail = NULL;
    json_t *list = fetch_json_d(url, NULL, p->api_key, probe_timeout(base, 250), &answered, &p->detail);
    json_t *props = list ? fetch_json(purl, NULL, p->api_key, probe_timeout(base, 150), NULL) : NULL;
    free(url);
    free(purl);
    if (!list) {
        if (answered && !p->error) p->error = xstrdup("bad response");
        return answered;
    }
    int64_t srv_ctx = pos_int(json_get(json_get_obj(props, "default_generation_settings"), "n_ctx"));
    if (!srv_ctx) srv_ctx = pos_int(json_get(props, "n_ctx"));
    json_t *params = json_get_obj(json_get_obj(props, "default_generation_settings"), "params");
    /* extractLlamaCppMaxTokens: only the -1 "unlimited" sentinel matters (output may fill the window) */
    json_t *gs = json_get_obj(props, "default_generation_settings");
    const json_t *cands[] = {json_get(params, "max_tokens"), json_get(params, "n_predict"), json_get(gs, "max_tokens"),
                             json_get(gs, "n_predict"), json_get(props, "max_tokens"), json_get(props, "n_predict")};
    bool unlimited = false;
    for (size_t c = 0; c < ARRAY_LEN(cands); c++)
        if ((json_is_num(cands[c]) && cands[c]->u.num.d == -1) || (json_sv(cands[c]) && !strcmp(json_sv(cands[c]), "-1")))
            unlimited = true;
    bool srv_image = false;
    json_t *mods = json_get_obj(props, "modalities");
    if (mods) srv_image = json_get_bool(mods, "vision", false);
    char *v1 = str_ends(base, "/v1") ? xstrdup(base) : xasprintf("%s/v1", base);
    p->models = json_arr();
    json_t *it;
    json_arr_foreach(json_get_arr(list, "data"), i, it) {
        const char *id = json_get_str(it, "id");
        if (!id || !*id) continue;
        json_t *meta = json_get_obj(it, "meta");
        int64_t ctx = pos_int(json_get(meta, "n_ctx"));
        if (!ctx) ctx = llamacpp_ctx_from_args(json_get_arr(json_get_obj(it, "status"), "args"));
        if (!ctx) ctx = srv_ctx;
        if (!ctx) ctx = pos_int(json_get(meta, "n_ctx_train"));
        if (!ctx) ctx = DISC_DEFAULT_CTX;
        bool image = srv_image;
        json_t *im = json_get_arr(json_get_obj(it, "architecture"), "input_modalities"), *x;
        if (im) {
            image = false;
            json_arr_foreach(im, k, x) if (json_sv(x) && !strcasecmp(json_sv(x), "image")) image = true;
        }
        int64_t max_tokens = unlimited ? ctx : MIN(ctx, (int64_t)DISC_DEFAULT_MAX_TOKENS);
        json_push(p->models, new_model(id, id, p->api, p->provider, v1, -1, image, ctx, max_tokens));
    }
    free(v1);
    json_free(list);
    json_free(props);
    return true;
}

static void probe_llamacpp(probe_t *p) {
    if (probe_llamacpp_at(p, p->base) || p->models) return;
    if (p->fallback) probe_llamacpp_at(p, p->fallback);
    if (!p->models && !p->error) p->error = xstrdup("unreachable");
}

/* ---------- LM Studio ---------- */

static void probe_lmstudio(probe_t *p) {
    char *url = xasprintf("%s/models", p->base);
    bool answered = false;
    json_t *list = fetch_json_d(url, NULL, p->api_key, probe_timeout(p->base, 250), &answered, &p->detail);
    free(url);
    if (!list) {
        p->error = xstrdup(answered ? "bad response" : "unreachable");
        return;
    }
    char *native = str_ends(p->base, "/v1") ? xstrndup(p->base, strlen(p->base) - 3) : xstrdup(p->base);
    char *nurl = xasprintf("%s/api/v0/models", native);
    json_t *v0 = fetch_json(nurl, NULL, p->api_key, probe_timeout(p->base, 250), NULL);
    free(nurl);
    free(native);
    p->models = json_arr();
    json_t *it;
    json_arr_foreach(json_get_arr(list, "data"), i, it) {
        const char *id = json_get_str(it, "id");
        if (!id || !*id) continue;
        json_t *nm = NULL, *x;
        json_arr_foreach(json_get_arr(v0, "data"), k, x) if (str_eq(json_get_str(x, "id"), id)) nm = x;
        int64_t ctx = 0;
        bool image = false;
        const char *type = nm ? json_get_str(nm, "type") : NULL;
        if (nm) {
            if (str_eq(json_get_str(nm, "state"), "loaded")) ctx = pos_int(json_get(nm, "loaded_context_length"));
            if (!ctx) ctx = pos_int(json_get(nm, "max_context_length"));
            if (!ctx) ctx = pos_int(json_get(nm, "context_length"));
            image = str_eq(type, "vlm");
        }
        /* mapWithBundledReference: limits the /models row reports, under the native metadata's context */
        if (!ctx) ctx = pos_int(json_get(it, "context_length"));
        if (!ctx) ctx = DISC_DEFAULT_CTX;
        int64_t max_tokens = pos_int(json_get(it, "max_completion_tokens"));
        if (!max_tokens) max_tokens = MIN(ctx, (int64_t)DISC_DEFAULT_MAX_TOKENS);
        json_t *m = new_model(id, id, p->api, p->provider, p->base, -1, image, ctx, max_tokens);
        if (str_eq(type, "embeddings") || str_eq(type, "embedding")) json_set_str(m, "kind", "embedding");
        json_push(p->models, m);
    }
    json_free(v0);
    json_free(list);
}

static void *probe_thread(void *arg) {
    probe_t *p = arg;
    switch (p->kind) {
    case D_OLLAMA: probe_ollama(p); break;
    case D_LLAMACPP: probe_llamacpp(p); break;
    case D_LMSTUDIO: probe_lmstudio(p); break;
    }
    return NULL;
}

/* ---------- cache + registration ---------- */

static char *cache_path(void) { return omc_agent_path("local-models.json"); }

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

/* register a provider's model list; earlier discovered models no longer listed are marked gone */
static void register_models(const char *provider, const json_t *models) {
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (!str_eq(m->provider, provider) || !json_get_bool(m->raw, "discovered", false)) continue;
        bool keep = false;
        json_t *x;
        json_arr_foreach(models, k, x) if (str_eq(json_get_str(x, "id"), m->id)) keep = true;
        if (!keep) json_set_bool(m->raw, "gone", true);
    }
    json_t *x;
    json_arr_foreach(models, k, x) catalog_register(json_clone(x));
}

static void set_state(const char *provider, const char *status, bool optional, const char *error, int64_t fetched) {
    pthread_mutex_lock(&g_mu);
    if (!g_state) g_state = json_obj();
    json_t *s = json_obj();
    json_set_str(s, "status", status);
    json_set_bool(s, "optional", optional);
    if (error) json_set_str(s, "error", error);
    if (fetched) json_set_int(s, "fetchedAt", fetched);
    json_set(g_state, provider, s);
    pthread_mutex_unlock(&g_mu);
}

json_t *catalog_local_discovery_state(const char *provider) {
    pthread_mutex_lock(&g_mu);
    json_t *r = g_state && provider ? json_clone(json_get(g_state, provider)) : NULL;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static bool configured_in_models_yml(const char *provider) {
    return json_get_bool(catalog_provider_info(provider), "modelsYml", false);
}

/* `only`: probe just that provider (a login), leaving the others' models and cache rows alone */
static int discover(bool force, const char *only) {
    if (env_truthy("OMC_NO_LOCAL_DISCOVERY")) return 0;
    probe_t probes[3];
    size_t np = 0;
#define WANTED(id) ((!only || !strcmp(only, id)) && !configured_in_models_yml(id) && !provider_disabled(id))
    const char *oenv = env_get("OLLAMA_BASE_URL");
    char *ohost = oenv ? NULL : ollama_host_url(env_get("OLLAMA_HOST"));
    if (WANTED("ollama"))
        probes[np++] = (probe_t){.kind = D_OLLAMA, .provider = "ollama", .api = "openai-responses",
                                 .base = trim_slashes(oenv ? oenv : ohost ? ohost : "http://127.0.0.1:11434"),
                                 .optional = !oenv && !ohost,
                                 .endpoint_env = oenv ? "OLLAMA_BASE_URL" : ohost ? "OLLAMA_HOST" : NULL,
                                 .display_url = oenv ? xstrdup(oenv) : ohost ? xstrdup(ohost) : NULL};
    free(ohost);
    const char *lenv = env_get("LLAMA_CPP_BASE_URL");
    if (WANTED("llama.cpp")) {
        char *b = trim_slashes(lenv ? lenv : "http://127.0.0.1:8080");
        if (str_ends(b, "/v1")) b[strlen(b) - 3] = 0;
        probes[np++] = (probe_t){.kind = D_LLAMACPP, .provider = "llama.cpp", .api = "openai-responses", .base = b,
                                 .fallback = lenv ? NULL : xstrdup("http://127.0.0.1:9931"), .optional = !lenv,
                                 .endpoint_env = lenv ? "LLAMA_CPP_BASE_URL" : NULL, .display_url = lenv ? xstrdup(lenv) : NULL};
    }
    const char *senv = env_get("LM_STUDIO_BASE_URL");
    if (WANTED("lm-studio"))
        probes[np++] = (probe_t){.kind = D_LMSTUDIO, .provider = "lm-studio", .api = "openai-completions",
                                 .base = trim_slashes(senv ? senv : "http://127.0.0.1:1234/v1"), .optional = !senv,
                                 .endpoint_env = senv ? "LM_STUDIO_BASE_URL" : NULL, .display_url = senv ? xstrdup(senv) : NULL};
#undef WANTED
    for (size_t i = 0; i < np; i++) probes[i].api_key = probe_key(probes[i].provider);
    json_t *cache = cache_load();
    int64_t now = now_ms();
    bool online[3] = {0};
    pthread_t th[3];
    bool started[3] = {0};
    for (size_t i = 0; i < np; i++) {
        json_t *row = json_get_obj(cache, probes[i].provider);
        bool fresh = row && now - json_get_int(row, "updatedAt", 0) < DISC_CACHE_TTL_MS &&
                     str_eq(json_get_str(row, "baseUrl"), probes[i].base);
        online[i] = force || !fresh || probes[i].kind == D_LLAMACPP;
        if (online[i]) started[i] = pthread_create(&th[i], NULL, probe_thread, &probes[i]) == 0;
        if (online[i] && !started[i]) probe_thread(&probes[i]);
    }
    bool dirty = false;
    int total = 0;
    for (size_t i = 0; i < np; i++) {
        probe_t *p = &probes[i];
        if (started[i]) pthread_join(th[i], NULL);
        json_t *row = json_get_obj(cache, p->provider);
        bool cached_ok = row && str_eq(json_get_str(row, "baseUrl"), p->base);
        if (p->models) {
            pthread_mutex_lock(&g_mu);
            if (g_last_warning) json_del(g_last_warning, p->provider);
            pthread_mutex_unlock(&g_mu);
            register_models(p->provider, p->models);
            total += (int)json_len(p->models);
            json_t *r = json_obj();
            json_set_str(r, "baseUrl", p->base);
            json_set_int(r, "updatedAt", now);
            json_set(r, "models", json_clone(p->models));
            json_set(cache, p->provider, r);
            dirty = true;
            set_state(p->provider, json_len(p->models) ? "ok" : "empty", p->optional, NULL, now);
        } else if (cached_ok) {
            json_t *ms = json_get_arr(row, "models");
            register_models(p->provider, ms);
            total += (int)json_len(ms);
            set_state(p->provider, online[i] ? "cached" : (json_len(ms) ? "ok" : "empty"), p->optional,
                      online[i] ? p->error : NULL, json_get_int(row, "updatedAt", 0));
        } else {
            set_state(p->provider, "unavailable", p->optional, p->error, 0);
        }
        if (online[i] && !p->models) warn_discovery_failure(p);
        json_free(p->models);
        free(p->error);
        free(p->detail);
        free(p->display_url);
        free(p->base);
        free(p->fallback);
        free(p->api_key);
    }
    if (dirty) cache_save(cache);
    json_free(cache);
    return total;
}

int catalog_discover_local(bool force) {
    catalog_init();
    if (!force) {
        if (g_started) return 0;
        g_started = true;
    }
    return discover(force, NULL);
}

int catalog_discover_provider(const char *provider) {
    catalog_init();
    if (str_eq(provider, "kiro")) return kiro_discover(true); /* a Kiro login lists its own roster */
    if (!provider || (strcmp(provider, "ollama") && strcmp(provider, "llama.cpp") && strcmp(provider, "lm-studio"))) return 0;
    return discover(true, provider);
}
