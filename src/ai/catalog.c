/* catalog.c - bundled model catalog (assets/models.json.z) + provider metadata + custom models. */
#include "ai.h"
#include "ai/compat_detect.h"
#include "core/embed.h"
#include "core/fs.h"
#include "core/proc.h"

#include <ctype.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

static VEC(ai_model_t *) g_models;
static hmap_t g_index; /* "provider/id" -> ai_model_t* */
static json_t *g_providers; /* {providers:{id:{...}}, auth:[...]} */
static json_t *g_defaults;  /* per-api compat defaults */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
/* models.yml can be re-read at runtime (catalog_refresh_if_stale), so the model list and index are guarded.
 * Model objects are never freed: sessions keep pointers to them across a reload. */
static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;
static int64_t g_cfg_mtime = -1; /* mtime (ns) of the models config last applied; -1 = none existed */
static strvec_t g_yml_keys;      /* "provider\x1fid" of the models the last applied config defined */
static size_t g_bundled;         /* the first g_bundled entries of g_models are bundled rows */

static char *inflate_buf(const unsigned char *data, size_t len, size_t *out_len) {
    z_stream zs = {0};
    if (inflateInit(&zs) != Z_OK) return NULL;
    sb_t b = SB_INIT;
    char out[65536];
    zs.next_in = (Bytef *)data;
    zs.avail_in = (uInt)len;
    int r;
    do {
        zs.next_out = (Bytef *)out;
        zs.avail_out = sizeof out;
        r = inflate(&zs, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) {
            inflateEnd(&zs);
            sb_free(&b);
            return NULL;
        }
        sb_appendn(&b, out, sizeof out - zs.avail_out);
    } while (r != Z_STREAM_END);
    inflateEnd(&zs);
    *out_len = b.len;
    return sb_detach(&b);
}

static json_t *load_embedded_json(const char *name) {
    size_t len;
    const char *z = embed_get(name, &len);
    if (!z) return NULL;
    size_t n;
    char *raw = inflate_buf((const unsigned char *)z, len, &n);
    if (!raw) return NULL;
    json_t *j = json_parse(raw, n, NULL);
    free(raw);
    return j;
}

static bool g_extended; /* extendedContext */

/* #resolveContextWindowPolicies for extended off/on (model-registry.ts): the standard window caps a premium
 * long-context price tier at its input threshold (unless longContextWindowUncapped); the extended one widens to
 * the advertised maximum. Authoritative windows and pinned overrides keep the catalog window. */
static void context_tiers(ai_model_t *m) {
    json_t *r = m->raw;
    int64_t cw = m->context_window;
    m->context_window_std = m->context_window_ext = cw;
    if (m->context_window_pinned || json_get_bool(r, "contextWindowAuthoritative", false) || !json_get(r, "contextWindow"))
        return;
    int64_t mx = json_get_int(r, "maxContextWindow", 0);
    if (mx > 0) m->context_window_ext = MAX(cw, mx);
    json_t *lc = json_get_obj(json_get_obj(r, "cost"), "longContext");
    int64_t th = json_get_int(lc, "inputThreshold", 0);
    if (th > 0 && !json_get_bool(r, "longContextWindowUncapped", false) && cw > th) m->context_window_std = th;
}

static void fill_model(ai_model_t *m) {
    json_t *r = m->raw;
    m->id = json_get_str(r, "id");
    m->name = json_get_str(r, "name");
    if (!m->name) m->name = m->id;
    m->api = json_get_str(r, "api");
    m->provider = json_get_str(r, "provider");
    m->base_url = json_get_str(r, "baseUrl");
    if (!m->base_url) m->base_url = "";
    m->reasoning = json_get_bool(r, "reasoning", false);
    m->input_image = false;
    json_t *in = json_get(r, "input"), *it;
    json_arr_foreach(in, i, it) {
        if (str_eq(json_sv(it), "image")) m->input_image = true;
    }
    json_t *cost = json_get(r, "cost");
    m->cost_input = json_get_num(cost, "input", 0);
    m->cost_output = json_get_num(cost, "output", 0);
    m->cost_cache_read = json_get_num(cost, "cacheRead", 0);
    m->cost_cache_write = json_get_num(cost, "cacheWrite", 0);
    m->context_window = json_get_int(r, "contextWindow", 128000);
    context_tiers(m);
    m->context_window = g_extended ? m->context_window_ext : m->context_window_std;
    m->max_tokens = json_get_int(r, "maxTokens", 0);
    if (m->max_tokens <= 0) m->max_tokens = MIN(m->context_window, (int64_t)32000);
    m->compat = json_get_obj(r, "compat");
    m->headers = json_get_obj(r, "headers");
    m->thinking = json_get_obj(r, "thinking");
}

static ai_model_t *add_model(json_t *obj) {
    const char *api = json_get_str(obj, "api");
    ai_model_t *m = xcalloc(1, sizeof(*m));
    m->raw = obj;
    fill_model(m);
    m->compat_defaults = api ? json_get_obj(g_defaults, api) : NULL;
    if (!m->id || !m->api || !m->provider) {
        json_free(obj);
        free(m);
        return NULL;
    }
    /* replace existing same provider/id */
    char *key = xasprintf("%s\x1f%s", m->provider, m->id);
    ai_model_t *o = hmap_get(&g_index, key);
    if (o) {
        /* keep old allocation alive (callers may hold pointers): swap contents; old raw intentionally leaked
           because borrowed strings may still be referenced */
        *o = *m;
        free(m);
        free(key);
        return o;
    }
    hmap_put(&g_index, key, m);
    free(key);
    vec_push(&g_models, m);
    return m;
}

/* models.yml / models.json custom providers (upstream shape) */
extern json_t *yaml_parse_file_json(const char *path, char **err) __attribute__((weak));

static const char *const g_cfg_names[] = {"models.yml", "models.yaml", "models.json"};

/* mtime (ns) of the models config that would be loaded now, -1 when none exists (ModelsConfigFile.getMtimeMs) */
static int64_t cfg_mtime(void) {
    for (size_t i = 0; i < ARRAY_LEN(g_cfg_names); i++) {
        char *p = omc_agent_path(g_cfg_names[i]);
        struct stat st;
        bool ok = stat(p, &st) == 0 && S_ISREG(st.st_mode);
        free(p);
        if (ok) return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    }
    return -1;
}

static json_t *load_custom_models_file(void) {
    g_cfg_mtime = cfg_mtime();
    for (size_t i = 0; i < ARRAY_LEN(g_cfg_names); i++) {
        char *p = omc_agent_path(g_cfg_names[i]);
        if (!fs_is_file(p)) {
            free(p);
            continue;
        }
        json_t *j = NULL;
        if (str_ends(p, ".json")) {
            size_t n;
            char *s = fs_read(p, &n);
            if (s) j = json_parse(s, n, NULL);
            free(s);
        } else if (yaml_parse_file_json) {
            char *err = NULL;
            j = yaml_parse_file_json(p, &err);
            if (err) {
                LOGW("models config %s: %s", p, err);
                free(err);
            }
        }
        free(p);
        if (j) return j;
    }
    return NULL;
}

/* proxy reference lookup over the bundled rows; built before the first custom model joins the list. Caller holds the
 * init once or g_lock. */
static const compat_ref_index_t *reference_index(void) {
    static compat_ref_index_t *idx;
    if (!idx) {
        idx = compat_ref_index_new();
        for (size_t i = 0; i < g_bundled; i++) {
            const ai_model_t *m = g_models.v[i];
            /* omp discovers these two providers' rows online; they are not bundled references */
            if (str_eq(m->provider, "charm-hyper") || str_eq(m->provider, "factory-droid")) continue;
            compat_ref_index_add(idx, m->raw);
        }
        compat_ref_index_finish(idx);
    }
    return idx;
}

/* buildModel({...toModelSpec(row), baseUrl, compat}): the row's policy follows its endpoint. The old object stays
 * allocated: other threads and the reference index may hold it. */
static void row_install(ai_model_t *m, json_t *o) {
    compat_resolve_model(o);
    m->raw = o;
    fill_model(m);
    if (compat_api_has_record(m->api)) m->compat_defaults = NULL;
}

static void row_rebuild(ai_model_t *m, const char *url, const json_t *pcompat) {
    json_t *o = json_clone(m->raw);
    json_t *compat = compat_merge(json_get_obj(o, "compatConfig"), pcompat);
    json_del(o, "compat");
    json_del(o, "compatConfig");
    if (compat) json_set(o, "compat", compat);
    if (url) json_set_str(o, "baseUrl", url);
    row_install(m, o);
}

/* the provider's compat with disableStrictTools folded in (owned or NULL) */
static json_t *provider_compat(const json_t *pv) {
    json_t *c = json_get_obj(pv, "compat") ? json_clone(json_get(pv, "compat")) : NULL;
    if (json_get_bool(pv, "disableStrictTools", false)) {
        if (!c) c = json_obj();
        json_set_bool(c, "disableStrictTools", true);
    }
    return c;
}

/* resolveProviderBaseUrl + the baseUrlApis scope: the effective APIs of the custom models inheriting the URL, plus a
 * provider `api` when there are none; no scope covers the whole provider. Owned, NULL when the row keeps its URL. */
static char *provider_row_base_url(const json_t *pv, const ai_model_t *m, const char *base) {
    if (!base) return NULL;
    const char *papi = json_get_str(pv, "api");
    json_t *models = json_get_arr(pv, "models"), *md;
    bool scoped = false, covered = false;
    json_arr_foreach(models, i, md) {
        if (json_get_str(md, "baseUrl")) continue;
        const char *a = json_get_str(md, "api");
        if (!a) a = papi;
        if (a) {
            scoped = true;
            if (str_eq(a, m->api)) covered = true;
        }
    }
    if (papi && json_len(models) == 0) {
        scoped = true;
        if (str_eq(papi, m->api)) covered = true;
    }
    if (scoped && !covered) return NULL;
    if (!scoped && str_eq(m->api, "openrouter-decisions")) { /* openrouterDecisionsBaseUrl: the /alpha sibling of the chat root */
        char *u = xstrdup(base);
        size_t n = strlen(u);
        while (n && u[n - 1] == '/') u[--n] = 0;
        if (n >= 3 && str_eq(u + n - 3, "/v1")) u[n -= 3] = 0;
        char *r = xasprintf("%s/alpha", u);
        free(u);
        return r;
    }
    return xstrdup(base);
}

/* applyModelPatch for a `modelOverrides` entry: a compat patch merges into the row's configured compat and rebuilds;
 * limits and pricing stay as already resolved, explicit input and thinking win */
static void row_override(ai_model_t *m, json_t *mv) {
    const json_t *pc = json_get_obj(mv, "compat");
    if (!pc) {
        json_merge(m->raw, mv);
        fill_model(m);
        return;
    }
    json_t *o = json_clone(m->raw);
    json_t *compat = compat_merge(json_get_obj(o, "compatConfig"), pc);
    json_merge(o, mv);
    json_del(o, "compatConfig");
    json_set(o, "compat", compat);
    json_t *ctx = json_clone(json_get(o, "contextWindow")), *max = json_clone(json_get(o, "maxTokens"));
    json_t *cost = json_clone(json_get(o, "cost"));
    json_t *thinking = json_get_obj(mv, "thinking") ? json_clone(json_get(mv, "thinking")) : NULL;
    json_t *input = json_get(mv, "input") ? json_clone(json_get(mv, "input")) : NULL;
    compat_resolve_model(o);
    json_set(o, "contextWindow", ctx);
    json_set(o, "maxTokens", max);
    json_set(o, "cost", cost);
    if (thinking) {
        if (json_get_obj(o, "thinking")) json_set(o, "thinking", thinking);
        else json_free(thinking);
    }
    if (input) json_set(o, "input", input);
    m->raw = o;
    fill_model(m);
    if (compat_api_has_record(m->api)) m->compat_defaults = NULL;
}

static void apply_custom(json_t *cfg) {
    strvec_t keys = {0};
    json_t *provs = json_get_obj(cfg, "providers");
    const char *pid;
    json_t *pv;
    json_obj_foreach(provs, i, pid, pv) {
        const char *base = json_get_str(pv, "baseUrl");
        const char *api = json_get_str(pv, "api");
        json_t *headers = json_get_obj(pv, "headers");
        /* #loadBuiltInModels: a provider override rebuilds its bundled rows under the new URL and compat */
        json_t *pover = provider_compat(pv);
        if (base || pover) {
            (void)reference_index(); /* identity/reference.ts reads the rows as bundled */
            for (size_t k = 0; k < g_bundled; k++) {
                ai_model_t *m = g_models.v[k];
                if (!str_eq(m->provider, pid)) continue;
                char *url = provider_row_base_url(pv, m, base);
                row_rebuild(m, url, pover);
                free(url);
            }
        }
        json_free(pover);
        json_t *ov = json_get_obj(pv, "modelOverrides");
        const char *mid;
        json_t *mv;
        json_obj_foreach(ov, k2, mid, mv) {
            /* direct index lookup: this runs inside catalog_init's pthread_once (catalog_find would re-enter it) */
            char *okey = xasprintf("%s\x1f%s", pid, mid);
            ai_model_t *m = hmap_get(&g_index, okey);
            free(okey);
            if (m) {
                if (json_get(mv, "contextWindow")) m->context_window_pinned = true;
                row_override(m, mv);
            }
        }
        json_t *models = json_get_arr(pv, "models"), *mm;
        json_arr_foreach(models, k3, mm) {
            json_t *o = json_clone(mm);
            if (!json_get_str(o, "provider")) json_set_str(o, "provider", pid);
            if (!json_get_str(o, "api")) json_set_str(o, "api", api ? api : "openai-completions");
            if (!json_get_str(o, "baseUrl") && base) json_set_str(o, "baseUrl", base);
            if (headers && !json_get(o, "headers")) json_set(o, "headers", json_clone(headers));
            /* buildCustomModelOverlay: the provider's compat under the model's */
            json_t *pcompat = json_get_obj(pv, "compat") ? json_clone(json_get(pv, "compat")) : NULL;
            if (json_get_bool(pv, "disableStrictTools", false)) {
                if (!pcompat) pcompat = json_obj();
                json_set_bool(pcompat, "disableStrictTools", true);
            }
            if (pcompat) {
                json_set(o, "compat", compat_merge(pcompat, json_get_obj(o, "compat")));
                json_free(pcompat);
            }
            compat_finalize_custom(o, compat_ref_lookup(reference_index(), json_get_str(o, "id")));
            /* resolveCustomModelIsOAuth: `auth: oauth`, or an unset `auth` on an Anthropic-wire endpoint (typically a
             * Claude-Code-style proxy expecting the cloaked request shape) */
            const char *pauth = json_get_str(pv, "auth");
            if (str_eq(pauth, "oauth") || (!pauth && str_eq(json_get_str(o, "api"), "anthropic-messages")))
                json_set_bool(o, "isOAuth", true);
            ai_model_t *added = add_model(o);
            if (added) {
                if (compat_api_has_record(added->api)) added->compat_defaults = NULL;
                /* omp's inferred context-window tiers cover catalog rows; a custom model keeps its window */
                added->context_window_pinned = true;
                fill_model(added);
                strvec_push_own(&keys, xasprintf("%s\x1f%s", added->provider, added->id));
            }
        }
        /* register provider info for key resolution */
        json_t *pmap = json_get_obj(g_providers, "providers");
        if (pmap && !json_get(pmap, pid)) {
            json_t *info = json_obj();
            json_set_str(info, "id", pid);
            json_set(pmap, pid, info);
        }
        json_t *info = json_get(pmap, pid);
        if (info && !json_get_bool(info, "modelsYml", false)) json_set_bool(info, "modelsYml", true);
        /* `auth: none`: an endpoint that takes no credential (keylessProviders) */
        if (info && str_eq(json_get_str(pv, "auth"), "none") && !json_get_bool(info, "keyless", false))
            json_set_bool(info, "keyless", true);
        /* other threads may hold the previous key string (ai_env_api_key): only touch it when it changed, and
         * leak the replaced value instead of freeing it */
        const char *key = json_get_str(pv, "apiKey");
        if (info && key && !str_eq(json_get_str(info, "configApiKey"), key)) {
            (void)json_take(info, "configApiKey");
            json_set_str(info, "configApiKey", key);
        }
        if (info && headers) json_set(info, "headers", json_clone(headers));
    }
    /* a reload retires the models the previous config defined and this one no longer does (omp drops them):
     * they leave the index and are marked gone, but stay allocated for sessions that still point at them */
    for (size_t i = 0; i < g_yml_keys.n; i++) {
        if (strvec_contains(&keys, g_yml_keys.v[i])) continue;
        ai_model_t *m = hmap_del(&g_index, g_yml_keys.v[i]);
        if (m) json_set_bool(m->raw, "gone", true);
    }
    strvec_free(&g_yml_keys);
    g_yml_keys = keys;
}

bool catalog_refresh_if_stale(void) {
    catalog_init();
    int64_t now = cfg_mtime();
    if (now == g_cfg_mtime) return false;
    /* parse outside the lock; an empty config (file deleted) still retires the previous models */
    json_t *custom = load_custom_models_file();
    pthread_rwlock_wrlock(&g_lock);
    json_t *empty = custom ? NULL : json_obj();
    apply_custom(custom ? custom : empty);
    pthread_rwlock_unlock(&g_lock);
    json_free(custom);
    json_free(empty);
    LOGI("models config changed on disk: catalog reloaded");
    return true;
}

static void do_init(void) {
    json_t *cat = load_embedded_json("assets/models.json.z");
    g_providers = load_embedded_json("assets/providers.json.z");
    if (!g_providers) g_providers = json_obj();
    if (!json_get(g_providers, "providers")) json_set(g_providers, "providers", json_obj());
    if (cat) {
        g_defaults = json_take(cat, "defaults");
        json_t *provs = json_get_obj(cat, "providers");
        const char *pid;
        json_t *pv;
        json_obj_foreach(provs, i, pid, pv) {
            const char *mid;
            json_t *mv;
            json_obj_foreach(pv, j, mid, mv) {
                /* steal the object out of the tree to avoid a deep copy */
                pv->u.obj.vals[j] = json_null();
                add_model(mv);
            }
        }
        json_free(cat);
        g_bundled = g_models.n;
    } else {
        LOGE("bundled model catalog missing or corrupt");
    }
    json_t *custom = load_custom_models_file();
    if (custom) {
        apply_custom(custom);
        json_free(custom);
    }
}

void catalog_init(void) { pthread_once(&g_once, do_init); }

size_t catalog_count(void) {
    catalog_init();
    pthread_rwlock_rdlock(&g_lock);
    size_t n = g_models.n;
    pthread_rwlock_unlock(&g_lock);
    return n;
}

const ai_model_t *catalog_at(size_t i) {
    catalog_init();
    pthread_rwlock_rdlock(&g_lock);
    const ai_model_t *m = i < g_models.n ? g_models.v[i] : NULL;
    pthread_rwlock_unlock(&g_lock);
    return m;
}

const ai_model_t *catalog_register(json_t *obj) {
    catalog_init();
    bool resolved = json_is_obj(obj) && !json_get(obj, "compat"); /* discovered local models */
    if (resolved) compat_resolve_model(obj);
    pthread_rwlock_wrlock(&g_lock);
    ai_model_t *m = add_model(obj);
    if (m && resolved && compat_api_has_record(m->api)) m->compat_defaults = NULL;
    pthread_rwlock_unlock(&g_lock);
    return m;
}

/* callers hold g_lock (read) */
static const ai_model_t *find_unlocked(const char *provider, const char *id) {
    if (!id) return NULL;
    if (provider) {
        char *key = xasprintf("%s\x1f%s", provider, id);
        ai_model_t *m = hmap_get(&g_index, key);
        free(key);
        return m;
    }
    for (size_t i = 0; i < g_models.n; i++) {
        ai_model_t *m = g_models.v[i];
        if (strcmp(m->id, id) == 0) return m;
    }
    return NULL;
}

const ai_model_t *catalog_find(const char *provider, const char *id) {
    catalog_init();
    pthread_rwlock_rdlock(&g_lock);
    const ai_model_t *m = find_unlocked(provider, id);
    pthread_rwlock_unlock(&g_lock);
    return m;
}

/* provider preference order when an id is ambiguous */
static int provider_rank(const char *p) {
    static const char *order[] = {"anthropic", "openai", "google", "openai-codex", "xai", "groq", "cerebras",
                                  "mistral", "deepseek", "openrouter"};
    for (size_t i = 0; i < ARRAY_LEN(order); i++)
        if (strcmp(order[i], p) == 0) return (int)i;
    return 100;
}

static bool provider_has_credentials(const char *p);

static const ai_model_t *resolve_unlocked(const char *pattern);

const ai_model_t *catalog_resolve(const char *pattern) {
    catalog_init();
    if (!pattern || !*pattern) return NULL;
    pthread_rwlock_rdlock(&g_lock);
    const ai_model_t *m = resolve_unlocked(pattern);
    pthread_rwlock_unlock(&g_lock);
    return m;
}

/* callers hold g_lock (read) */
static const ai_model_t *resolve_unlocked(const char *pattern) {
    const char *slash = strchr(pattern, '/');
    if (slash) {
        char *prov = xstrndup(pattern, (size_t)(slash - pattern));
        const ai_model_t *m = find_unlocked(prov, slash + 1);
        if (!m) {
            /* fuzzy within provider */
            const ai_model_t *best = NULL;
            for (size_t i = 0; i < g_models.n; i++) {
                ai_model_t *c = g_models.v[i];
                if (strcmp(c->provider, prov) != 0) continue;
                if (str_istr(c->id, slash + 1) && (!best || strlen(c->id) < strlen(best->id))) best = c;
            }
            m = best;
        }
        free(prov);
        if (m) return m;
        /* ids like "openrouter-style/vendor/model" */
        const ai_model_t *exact = find_unlocked(NULL, pattern);
        if (exact) return exact;
    }
    /* exact id across providers: prefer authenticated, then provider rank */
    const ai_model_t *best = NULL;
    int best_score = 1 << 30;
    for (size_t i = 0; i < g_models.n; i++) {
        ai_model_t *m = g_models.v[i];
        if (strcasecmp(m->id, pattern) != 0) continue;
        int score = provider_rank(m->provider) + (provider_has_credentials(m->provider) ? 0 : 1000);
        if (score < best_score) {
            best = m;
            best_score = score;
        }
    }
    if (best) return best;
    /* substring match: shortest id, authenticated first */
    for (size_t i = 0; i < g_models.n; i++) {
        ai_model_t *m = g_models.v[i];
        if (!str_istr(m->id, pattern) && !str_istr(m->name, pattern)) continue;
        int score = (int)strlen(m->id) * 4 + provider_rank(m->provider) + (provider_has_credentials(m->provider) ? 0 : 5000);
        if (score < best_score) {
            best = m;
            best_score = score;
        }
    }
    return best;
}

json_t *catalog_provider_info(const char *provider) {
    catalog_init();
    return json_get(json_get_obj(g_providers, "providers"), provider);
}

json_t *catalog_api_defaults(const char *api) {
    catalog_init();
    return api ? json_get_obj(g_defaults, api) : NULL;
}

json_t *catalog_auth_providers(void) {
    catalog_init();
    return json_get_arr(g_providers, "auth");
}

/* providerPauseReason: why the auth rules turn `provider` off (`disabled "<reason>"`), else NULL. omc has no
 * extension-registered logins that could take the id back. Borrowed. */
const char *catalog_provider_pause_reason(const char *provider) {
    if (!provider) return NULL;
    json_t *d;
    json_arr_foreach(catalog_auth_providers(), i, d) if (str_eq(json_get_str(d, "id"), provider)) {
        const char *r = json_get_str(d, "disabled");
        return r && *r ? r : NULL;
    }
    return NULL;
}

const char *catalog_provider_login_warning(const char *provider) {
    if (!provider) return NULL;
    json_t *d;
    json_arr_foreach(catalog_auth_providers(), i, d) if (str_eq(json_get_str(d, "id"), provider)) {
        const char *w = json_get_str(d, "loginWarning");
        return w && *w ? w : NULL;
    }
    return NULL;
}

char *catalog_provider_disabled_error(const char *provider) {
    const char *r = catalog_provider_pause_reason(provider);
    return r ? xasprintf("%s is turned off: %s", provider, r) : NULL;
}

json_t *catalog_role_defaults(void) {
    catalog_init();
    return json_get_obj(g_providers, "roleDefaults");
}

bool catalog_provider_keyless(const char *provider) {
    return provider && json_get_bool(catalog_provider_info(provider), "keyless", false);
}

const char *catalog_model_kind(const ai_model_t *m) {
    const char *k = m ? json_get_str(m->raw, "kind") : NULL;
    return k && *k ? k : "chat";
}

const char *catalog_provider_default_model(const char *provider) {
    return json_get_str(catalog_provider_info(provider), "defaultModel");
}

bool model_supports_effort(const ai_model_t *m, ai_effort_t e) {
    if (e == EFFORT_OFF) return true;
    if (!m || !m->reasoning) return false;
    json_t *efforts = json_get_arr(m->thinking, "efforts");
    if (!efforts) return e <= EFFORT_HIGH;
    json_t *it;
    json_arr_foreach(efforts, i, it) {
        if (str_eq(json_sv(it), effort_name(e))) return true;
    }
    return false;
}

ai_effort_t model_clamp_effort(const ai_model_t *m, ai_effort_t e) {
    if (e == EFFORT_OFF || !m || !m->reasoning) return m && m->reasoning ? e : EFFORT_OFF;
    if (model_supports_effort(m, e)) return e;
    /* nearest lower supported, else nearest higher */
    for (int k = (int)e - 1; k >= EFFORT_MINIMAL; k--)
        if (model_supports_effort(m, (ai_effort_t)k)) return (ai_effort_t)k;
    for (int k = (int)e + 1; k <= EFFORT_MAX; k++)
        if (model_supports_effort(m, (ai_effort_t)k)) return (ai_effort_t)k;
    return e;
}

/* ---------- env api keys ---------- */

static const struct {
    const char *provider;
    const char *env;
} legacy_env[] = {
    {"azure-openai-responses", "AZURE_OPENAI_API_KEY"},
    {"jina", "JINA_API_KEY"},
    {"brave", "BRAVE_API_KEY"},
    {"tavily", "TAVILY_API_KEY"},
    {"perplexity", "PERPLEXITY_API_KEY"},
    {"tinyfish", "TINYFISH_API_KEY"},
    {"keenable", "KEENABLE_API_KEY"},
    {"firecrawl", "FIRECRAWL_API_KEY"},
    {"anthropic", "ANTHROPIC_OAUTH_TOKEN"},
    {"google", "GOOGLE_API_KEY"},
    {"github-copilot", "COPILOT_GITHUB_TOKEN"},
};

const char *ai_env_api_key_name(const char *provider) {
    json_t *info = catalog_provider_info(provider);
    json_t *vars = json_get_arr(info, "envVars"), *v;
    json_arr_foreach(vars, i, v) {
        if (json_is_str(v) && env_get(json_sv(v))) return json_sv(v);
    }
    if (vars && json_len(vars)) return json_sv(json_at(vars, 0));
    for (size_t i = 0; i < ARRAY_LEN(legacy_env); i++)
        if (strcmp(legacy_env[i].provider, provider) == 0) return legacy_env[i].env;
    return NULL;
}

char *ai_env_api_key(const char *provider) {
    json_t *info = catalog_provider_info(provider);
    json_t *vars = json_get_arr(info, "envVars"), *v;
    json_arr_foreach(vars, i, v) {
        const char *val = json_is_str(v) ? env_get(json_sv(v)) : NULL;
        if (val) return xstrdup(val);
    }
    for (size_t i = 0; i < ARRAY_LEN(legacy_env); i++) {
        if (strcmp(legacy_env[i].provider, provider) == 0) {
            const char *val = env_get(legacy_env[i].env);
            if (val) return xstrdup(val);
        }
    }
    /* models.yml apiKey: env var name or literal */
    const char *ck = json_get_str(info, "configApiKey");
    if (ck) {
        const char *ev = env_get(ck);
        if (ev) return xstrdup(ev);
        if (ck[0] == '!') return proc_shell_capture(ck + 1, NULL, 10000);
        return xstrdup(ck);
    }
    return NULL;
}

/* hook implemented by the auth module; weak so ai can link standalone */
extern bool auth_has_credentials(const char *provider) __attribute__((weak));

static bool provider_has_credentials(const char *p) {
    if (auth_has_credentials && auth_has_credentials(p)) return true;
    if (catalog_provider_keyless(p)) return true;
    char *k = ai_env_api_key(p);
    bool ok = k != NULL;
    free(k);
    return ok;
}

bool catalog_model_available(const ai_model_t *m) {
    return m && !json_get_bool(m->raw, "gone", false) && provider_has_credentials(m->provider);
}

/* Port of catalog compat delegation (rules.json cascade: classes/openai.kdl): openai-class revision 5.6.0 is
 * "gated", >= 6.0.0 "restrained", everything else "eager". Identity comes from the catalog row; custom models
 * without one are classified from a `gpt-N[.M]` id like upstream's lenient classifier. */
static void parse_revision(const char *s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    for (int i = 0; i < 3 && s && *s; i++) {
        if (!isdigit((unsigned char)*s)) break;
        v[i] = (int)strtol(s, (char **)&s, 10);
        if (*s != '.') break;
        s++;
    }
}

/* compat/delegation.ts resolveDelegationBias: `balanced` when no rule assigns one (task/prompt-policy.ts: also before
 * a model is bound) */
const char *model_delegation_bias(const ai_model_t *m) {
    if (!m) return "balanced";
    json_t *ident = m->raw ? json_get_obj(m->raw, "identity") : NULL;
    const char *cls = json_get_str(ident, "class");
    const char *rev = json_get_str(ident, "revision");
    int v[3] = {0, 0, 0};
    bool have = false;
    if (cls) {
        if (rev) parse_revision(rev, v), have = true;
    } else if (m->id) {
        const char *id = strrchr(m->id, '/');
        id = id ? id + 1 : m->id;
        if (!strncasecmp(id, "gpt-", 4) && isdigit((unsigned char)id[4])) {
            cls = "openai";
            parse_revision(id + 4, v);
            have = true;
        }
    }
    if (!cls || strcmp(cls, "openai") != 0 || !have) return "balanced";
    if (v[0] == 5 && v[1] == 6 && v[2] == 0) return "gated";
    if (v[0] >= 6) return "restrained";
    return "balanced";
}

/* catalog compat cascade `web-search` / `web-search-model` (classes/{anthropic,gemini,openai,xai}.kdl and
 * providers/{anthropic,google,google-antigravity,openai-codex,openrouter,xai,xai-oauth}.kdl; provider rules run after
 * the class rules). A model's own `webSearch` / `webSearchModel` field wins. */
static bool class_rev(const ai_model_t *m, const char *cls, int major) {
    json_t *ident = m->raw ? json_get_obj(m->raw, "identity") : NULL;
    if (!str_eq(json_get_str(ident, "class"), cls)) return false;
    if (major <= 0) return true;
    const char *rev = json_get_str(ident, "revision");
    if (!rev) return false;
    int v[3];
    parse_revision(rev, v);
    return v[0] >= major;
}

const char *model_web_search(const ai_model_t *m) {
    if (!m) return NULL;
    const char *own = m->raw ? json_get_str(m->raw, "webSearch") : NULL;
    if (own) return own;
    const char *r = NULL;
    if (class_rev(m, "anthropic", 4) && str_eq(m->api, "anthropic-messages")) r = "anthropic";
    if (class_rev(m, "gemini", 2) && (str_eq(m->api, "google-generative-ai") || str_eq(m->api, "google-gemini-cli")))
        r = "gemini";
    if (class_rev(m, "openai", 5) && str_eq(m->api, "openai-responses")) r = "openai";
    const char *p = m->provider ? m->provider : "";
    if (!strcmp(p, "anthropic")) r = "anthropic";
    else if (!strcmp(p, "google") || !strcmp(p, "google-antigravity")) r = "gemini";
    else if (!strcmp(p, "openai-codex")) r = "codex";
    else if (!strcmp(p, "openrouter")) r = "openrouter";
    else if (!strcmp(p, "xai") || !strcmp(p, "xai-oauth")) r = "xai";
    return r;
}

const char *model_web_search_model(const ai_model_t *m) {
    if (!m) return NULL;
    const char *own = m->raw ? json_get_str(m->raw, "webSearchModel") : NULL;
    if (own) return own;
    if (class_rev(m, "anthropic", 4) && str_eq(m->api, "anthropic-messages")) return "claude-haiku-4-5";
    if (class_rev(m, "gemini", 2) && (str_eq(m->api, "google-generative-ai") || str_eq(m->api, "google-gemini-cli")))
        return "gemini-2.5-flash";
    if (class_rev(m, "openai", 5) && (str_eq(m->api, "openai-responses") || str_eq(m->api, "openai-codex-responses")))
        return "gpt-6-luna";
    if (class_rev(m, "xai", 0)) return "grok-4.5";
    return NULL;
}

void catalog_apply_context_policy(bool extended, bool (*opt_in)(const ai_model_t *m, int64_t standard)) {
    catalog_init();
    pthread_rwlock_wrlock(&g_lock);
    g_extended = extended;
    for (size_t i = 0; i < g_models.n; i++) {
        ai_model_t *m = g_models.v[i];
        bool ext = extended || (opt_in && m->context_window_ext > m->context_window_std && opt_in(m, m->context_window_std));
        m->context_window = ext ? m->context_window_ext : m->context_window_std;
    }
    pthread_rwlock_unlock(&g_lock);
}

bool catalog_context_tiers(const ai_model_t *m, int64_t *standard, int64_t *extended) {
    if (!m || m->context_window_ext <= m->context_window_std) return false;
    if (standard) *standard = m->context_window_std;
    if (extended) *extended = m->context_window_ext;
    return true;
}

/* ---------- Factory Droid credit rates (catalog/src/models.ts getFactoryDroidCreditRate) ---------- */

static int64_t parse_utc_instant(const char *s) {
    struct tm tm = {0};
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return -1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (int64_t)timegm(&tm) * 1000;
}

bool ai_factory_droid_credit_rate(const ai_model_t *m, int64_t now, ai_credit_rate_t *out) {
    memset(out, 0, sizeof *out);
    const json_t *basev = m && m->raw ? json_get(m->raw, "factoryDroidCredits") : NULL;
    if (!basev || !json_is_num(basev)) return false;
    double base = json_get_num(m->raw, "factoryDroidCredits", 0);
    out->rate = base;
    const json_t *cards = str_eq(m->provider, "factory-droid") ? compat_factory_droid_credit_cards(m->id) : NULL;
    if (!cards) return true;
    /* the card due now: the latest at or before it, as for cost cards */
    int64_t due_at = -1;
    for (size_t i = 0; i < cards->u.obj.n; i++) {
        const json_t *card = cards->u.obj.vals[i];
        int64_t at = parse_utc_instant(json_get_str(card, "effectiveFrom"));
        if (at >= 0 && at <= now && at > due_at) {
            due_at = at;
            out->rate = json_get_num(card, "input", base);
        }
    }
    /* the earliest later card that changes the rate */
    for (size_t i = 0; i < cards->u.obj.n; i++) {
        const json_t *card = cards->u.obj.vals[i];
        int64_t at = parse_utc_instant(json_get_str(card, "effectiveFrom"));
        double r = json_get_num(card, "input", base);
        if (at > now && r != out->rate && (!out->has_next || at < out->next_at)) {
            out->has_next = true;
            out->next_at = at;
            out->next_rate = r;
        }
    }
    return true;
}
