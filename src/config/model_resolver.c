/* model_resolver.c - omp's model selection (config/model-resolver.ts, model-roles.ts,
 * tiny/online-candidates.ts, session/retry-fallback-chains.ts). See model_resolver.h. */
#include "model_resolver.h"

#include "config/auth.h"
#include "config/settings.h"
#include "core/fs.h"

#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <strings.h>

/* provided by the catalog when it carries omp's role-default chains */
extern json_t *catalog_role_defaults(void) __attribute__((weak));

/* ---------- pools ---------- */

void mr_pool_free(mr_pool_t *p) {
    free(p->v);
    memset(p, 0, sizeof *p);
}

void mr_pool_push(mr_pool_t *p, const ai_model_t *m) {
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->v = xrealloc(p->v, p->cap * sizeof *p->v);
    }
    p->v[p->n++] = m;
}

bool mr_pool_contains(const mr_pool_t *p, const ai_model_t *m) {
    for (size_t i = 0; p && i < p->n; i++)
        if (p->v[i] == m) return true;
    return false;
}

static bool list_has(const char *path, const char *v) {
    json_t *a = settings_get(path), *x;
    json_arr_foreach(a, i, x) if (json_is_str(x) && !strcasecmp(json_sv(x), v)) return true;
    return false;
}

bool mr_provider_disabled(const char *provider) { return provider && list_has("disabledProviders", provider); }

bool mr_provider_known(const char *provider) {
    if (!provider) return false;
    if (catalog_provider_info(provider)) return true;
    for (size_t i = 0; i < catalog_count(); i++)
        if (!strcasecmp(catalog_at(i)->provider, provider)) return true;
    return false;
}

bool mr_provider_available(const char *provider) {
    if (!provider || mr_provider_disabled(provider)) return false;
    if (json_get_bool(catalog_provider_info(provider), "keyless", false)) return true;
    return auth_has_credentials(provider);
}

const char *mr_model_kind(const ai_model_t *m) {
    const char *k = m && m->raw ? json_get_str(m->raw, "kind") : NULL;
    return k && *k ? k : "chat";
}

static bool kind_ok(const ai_model_t *m, const char *kind) {
    return !kind || str_eq(kind, "all") || str_eq(mr_model_kind(m), kind);
}

void mr_pool_available(mr_pool_t *p, const char *kind) {
    /* availability is per provider: cache the answer for runs of the same provider */
    const char *last = NULL;
    bool last_ok = false;
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (!last || strcmp(last, m->provider)) {
            last = m->provider;
            last_ok = mr_provider_available(last);
        }
        if (last_ok && kind_ok(m, kind)) mr_pool_push(p, m);
    }
}

void mr_pool_all(mr_pool_t *p, const char *kind) {
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (kind_ok(m, kind) && !mr_provider_disabled(m->provider)) mr_pool_push(p, m);
    }
}

void mr_pool_role(mr_pool_t *p, const char *role) {
    mr_pool_t all = {0};
    mr_pool_available(&all, "all");
    for (size_t i = 0; i < all.n; i++)
        if (mr_role_accepts(role, all.v[i])) mr_pool_push(p, all.v[i]);
    mr_pool_free(&all);
}

/* ---------- roles ---------- */

const char *const MR_ROLE_IDS[] = {"default", "smol",   "slow",   "vision", "plan", "commit",
                                   "tiny",    "memory", "task",   "advisor", "widget", "image",
                                   "web",     "speech", "dictation", "judge"};
const size_t MR_ROLE_COUNT = ARRAY_LEN(MR_ROLE_IDS);
const size_t MR_CHAT_ROLE_COUNT = 11;

bool mr_is_builtin_role(const char *role) {
    for (size_t i = 0; role && i < MR_ROLE_COUNT; i++)
        if (!strcmp(MR_ROLE_IDS[i], role)) return true;
    return false;
}

bool mr_is_kind_role(const char *role) {
    for (size_t i = MR_CHAT_ROLE_COUNT; role && i < MR_ROLE_COUNT; i++)
        if (!strcmp(MR_ROLE_IDS[i], role)) return true;
    return false;
}

bool mr_role_accepts(const char *role, const ai_model_t *m) {
    const char *k = mr_model_kind(m);
    if (str_eq(role, "tiny") || str_eq(role, "memory")) return str_eq(k, "tiny") || str_eq(k, "chat");
    if (str_eq(role, "image")) return str_eq(k, "image");
    if (str_eq(role, "web"))
        return str_eq(k, "search") || (str_eq(k, "chat") && m->raw && json_get(m->raw, "webSearch"));
    if (str_eq(role, "speech")) return str_eq(k, "tts");
    if (str_eq(role, "dictation")) return str_eq(k, "stt");
    if (str_eq(role, "judge")) return str_eq(k, "judge") || str_eq(k, "tiny") || str_eq(k, "chat");
    return str_eq(k, "chat");
}

/* modelRoleValueFromUnknown: a string, or an array of strings joined with "," */
static const char *role_value_from(json_t *v) {
    if (json_is_str(v)) return json_sv(v);
    if (!json_is_arr(v) || !json_len(v)) return NULL;
    static __thread char *joined;
    sb_t b = SB_INIT;
    json_t *x;
    json_arr_foreach(v, i, x) {
        if (!json_is_str(x)) {
            sb_free(&b);
            return NULL;
        }
        if (i) sb_putc(&b, ',');
        sb_append(&b, json_sv(x));
    }
    free(joined);
    joined = sb_detach(&b);
    return joined;
}

const char *mr_role_get(const char *role) {
    if (!role || !*role) return NULL;
    json_t *roles = settings_get("modelRoles");
    if (!json_is_obj(roles)) return NULL;
    return role_value_from(json_get(roles, role));
}

strvec_t mr_known_roles(void) {
    strvec_t v = {0};
    for (size_t i = 0; i < MR_ROLE_COUNT; i++) strvec_push(&v, MR_ROLE_IDS[i]);
    json_t *c = settings_get("cycleOrder"), *x;
    json_arr_foreach(c, i, x) if (json_is_str(x) && !strvec_contains(&v, json_sv(x))) strvec_push(&v, json_sv(x));
    const char *k;
    json_t *roles = settings_get("modelRoles"), *val;
    json_obj_foreach(roles, i2, k, val) if (role_value_from(val) && !strvec_contains(&v, k)) strvec_push(&v, k);
    json_t *tags = settings_get("modelTags");
    json_obj_foreach(tags, i3, k, val) if (!strvec_contains(&v, k)) strvec_push(&v, k);
    /* hidden built-ins (modelTags.<role>.hidden) are dropped from the visible list */
    for (size_t i = 0; i < v.n;) {
        json_t *t = json_get(tags, v.v[i]);
        if (mr_is_builtin_role(v.v[i]) && json_get_bool(t, "hidden", false)) {
            free(v.v[i]);
            memmove(v.v + i, v.v + i + 1, (v.n - i - 1) * sizeof *v.v);
            v.n--;
        } else
            i++;
    }
    return v;
}

/* ---------- selectors ---------- */

const char *mr_parse_level(const char *s, bool allow_guarded) {
    static const char *const strict[] = {"inherit", "off", "minimal", "low", "medium", "high", "xhigh"};
    if (!s) return NULL;
    for (size_t i = 0; i < ARRAY_LEN(strict); i++)
        if (!strcmp(s, strict[i])) return strict[i];
    if (allow_guarded && !strcmp(s, "max")) return "max";
    if (allow_guarded && !strcmp(s, "auto")) return "auto";
    return NULL;
}

const char *mr_split_level(const char *pattern, int min_colon, bool allow_guarded, char **base) {
    const char *c = strrchr(pattern, ':');
    if (!c || (int)(c - pattern) <= min_colon) {
        *base = xstrdup(pattern);
        return NULL;
    }
    const char *lv = mr_parse_level(c + 1, allow_guarded);
    *base = lv ? xstrndup(pattern, (size_t)(c - pattern)) : xstrdup(pattern);
    return lv;
}

char *mr_model_key(const ai_model_t *m) { return m ? xasprintf("%s/%s", m->provider, m->id) : NULL; }

char *mr_selector_with_level(const char *selector, const char *level) {
    return level && strcmp(level, "inherit") ? xasprintf("%s:%s", selector, level) : xstrdup(selector);
}

static char *trim_dup(const char *s, size_t n) {
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

static void push_split(strvec_t *v, const char *s) {
    while (s) {
        const char *c = strchr(s, ',');
        size_t n = c ? (size_t)(c - s) : strlen(s);
        char *t = trim_dup(s, n);
        if (*t) strvec_push(v, t);
        free(t);
        s = c ? c + 1 : NULL;
    }
}

strvec_t mr_pattern_list(const char *value) {
    strvec_t v = {0};
    if (value) push_split(&v, value);
    return v;
}

static void strvec_extend(strvec_t *dst, const strvec_t *src) {
    for (size_t i = 0; i < src->n; i++) strvec_push(dst, src->v[i]);
}

/* ---------- role aliases ---------- */

#define DEFAULT_ROLE "default"

/* modelRoleAliasPrefixLength: -1 when not alias-shaped */
static int alias_prefix_len(const char *v) {
    if (!strcmp(v, "*") || str_starts(v, "*:")) return 0;
    if (v[0] == '@') return 1;
    if (str_starts(v, "pi/")) return 3;
    return -1;
}

/* getModelRoleAlias: role name (owned) or NULL */
static char *role_alias(const char *value) {
    char *n = trim_dup(value, strlen(value));
    int pl = alias_prefix_len(n);
    char *role = NULL;
    if (pl >= 0) {
        const char *cand = !strcmp(n, "*") ? DEFAULT_ROLE : n + pl;
        if (mr_is_builtin_role(cand) || mr_role_get(cand)) role = xstrdup(cand);
    }
    free(n);
    return role;
}

char *mr_explicit_role(const char *value) {
    strvec_t pats = mr_pattern_list(value);
    char *found = NULL;
    for (size_t i = 0; i < pats.n && !found; i++) {
        int pl = alias_prefix_len(pats.v[i]);
        if (pl < 0) continue;
        char *base;
        mr_split_level(pats.v[i], pl, true, &base);
        found = role_alias(base);
        free(base);
    }
    strvec_free(&pats);
    return found;
}

static const char *priority_alias(const char *role) {
    if (str_eq(role, "advisor")) return "slow";
    if (str_eq(role, "memory") || str_eq(role, "tiny") || str_eq(role, "widget")) return "smol";
    return role;
}

/* catalog role-defaults (omp behavior.kdl), used when the catalog does not carry them */
static const char *const DEF_SMOL[] = {
    "claude-haiku-5-5", "claude-haiku-5.5", "haiku-5-5", "haiku-5.5", "google-antigravity/gemini-3.8-flash",
    "gemini-3.8-flash", "gemini-3-8-flash", "gemini-3.7-flash", "gemini-3-7-flash", "gemini-3.6-flash",
    "gemini-3-6-flash", "gemini-3.5-flash", "gemini-3-5-flash", "openai-codex/gpt-5.3-codex-spark",
    "gpt-5.3-codex-spark", "5.3-spark", "spark", "zai/glm-5.3-flash", "glm-5.3-flash", "gemini-3.1-flash-lite",
    "gemini-3-1-flash-lite", "flash-lite", "cerebras/gpt-oss-120b", "gpt-oss-120b", "oss-120b",
    "cerebras/zai-glm-4.7", "cerebras/zai-glm-4.6", "claude-haiku-4-5", "claude-haiku-4.5", "haiku-4-5",
    "haiku-4.5", "haiku", "openai-codex/gpt-5.6-luna", "gpt-5.6-luna", "openai-codex/gpt-5.4-mini", "gpt-5.4-mini",
    "-mini", NULL};
static const char *const DEF_SLOW[] = {
    "openai-codex/gpt-5.6-sol", "gpt-5.6-sol", "anthropic/claude-fable-5-1", "claude-fable-5-1", "claude-fable-5.1",
    "fable-5.1", "fable-5-1", "anthropic/claude-fable-5", "claude-fable-5", "fable-5", "kimi-code/k3", "kimi-k3",
    "moonshotai/kimi-k3", "k3", "zai/glm-5.3", "glm-5.3", "anthropic/claude-opus-5-5", "claude-opus-5-5", "opus-5-5",
    "anthropic/claude-opus-5", "claude-opus-5", "opus-5", "gpt-5.5", "claude-opus-4-8", "claude-opus-4.8", "opus-4-8",
    "opus-4.8", NULL};
static const char *const DEF_IMAGE[] = {"openai/gpt-image-2", "openai-codex/gpt-image-2",
                                        "google-antigravity/gemini-3-pro-image", "xai/grok-imagine-image",
                                        "xai-oauth/grok-imagine-image", "openrouter/google/gemini-3-pro-image",
                                        "google/gemini-3-pro-image", "deepinfra/black-forest-labs/FLUX-2-pro",
                                        "factory-droid/gpt-image-2.5-sunburst", "factory-droid/gpt-image-2.5-flare",
                                        "factory-droid/gemini-3-pro-image-preview", NULL};
static const char *const DEF_WEB[] = {"web/parallel", "web/hosted", "web/exa", "web/firecrawl", "web/searxng",
                                      "web/startpage", "web/duckduckgo", "web/ecosia", "web/google", "web/mojeek",
                                      "web/public", NULL};
static const char *const DEF_SPEECH[] = {"local/kokoro", "xai/grok-tts", "xai-oauth/grok-tts", NULL};
static const char *const DEF_DICTATION[] = {"local/parakeet-tdt-0.6b-v3", NULL};
static const char *const DEF_JUDGE[] = {"typesafe/jev-1.14.0", "openrouter/typesafe/jev-1.14", "@tiny", "@smol",
                                        "@default", NULL};

static const char *const *embedded_defaults(const char *key) {
    if (str_eq(key, "smol")) return DEF_SMOL;
    if (str_eq(key, "slow")) return DEF_SLOW;
    if (str_eq(key, "image")) return DEF_IMAGE;
    if (str_eq(key, "web")) return DEF_WEB;
    if (str_eq(key, "speech")) return DEF_SPEECH;
    if (str_eq(key, "dictation")) return DEF_DICTATION;
    if (str_eq(key, "judge")) return DEF_JUDGE;
    return NULL;
}

strvec_t mr_role_priority_defaults(const char *role) {
    strvec_t v = {0};
    const char *key = priority_alias(role);
    json_t *cat = catalog_role_defaults ? catalog_role_defaults() : NULL;
    if (json_is_obj(cat)) {
        json_t *arr = json_get(cat, key), *x;
        json_arr_foreach(arr, i, x) if (json_is_str(x)) push_split(&v, json_sv(x));
        return v;
    }
    const char *const *d = embedded_defaults(key);
    for (; d && *d; d++) strvec_push(&v, *d);
    return v;
}

static bool configured_patterns_rec(const char *value, strvec_t *visited, strvec_t *out);

/* resolveNestedRolePatterns */
static void nested_patterns(const char *value, const strvec_t *role_defaults, strvec_t *visited, strvec_t *out) {
    strvec_t pats = mr_pattern_list(value);
    for (size_t i = 0; i < pats.n; i++) {
        const char *p = pats.v[i];
        int pl = alias_prefix_len(p);
        char *base;
        const char *lv = mr_split_level(p, pl >= 0 ? pl : 3, true, &base);
        char *ar = role_alias(base);
        free(base);
        if (!ar) {
            strvec_push(out, p);
            continue;
        }
        if (strvec_contains(visited, ar)) {
            for (size_t k = 0; k < role_defaults->n; k++) {
                char *s = lv ? xasprintf("%s:%s", role_defaults->v[k], lv) : xstrdup(role_defaults->v[k]);
                strvec_push(out, s);
                free(s);
            }
        } else {
            strvec_t vis = {0};
            strvec_extend(&vis, visited);
            configured_patterns_rec(p, &vis, out);
            strvec_free(&vis);
        }
        free(ar);
    }
    strvec_free(&pats);
}

/* resolveConfiguredRolePattern: appends to out; false = undefined */
static bool configured_patterns_rec(const char *value, strvec_t *visited, strvec_t *out) {
    char *n = trim_dup(value, strlen(value));
    if (!*n) {
        free(n);
        return false;
    }
    int pl = alias_prefix_len(n);
    char *base;
    const char *lv = mr_split_level(n, pl >= 0 ? pl : 3, true, &base);
    char *role = role_alias(base);
    free(base);
    if (!role) {
        strvec_push(out, n);
        free(n);
        return true;
    }
    free(n);
    if (strvec_contains(visited, role)) {
        free(role);
        return false;
    }
    strvec_push(visited, role);
    const char *cfg_raw = mr_role_get(role);
    char *configured = cfg_raw ? trim_dup(cfg_raw, strlen(cfg_raw)) : NULL;
    if (configured && !*configured) {
        free(configured);
        configured = NULL;
    }
    const char *def_raw = mr_role_get(DEFAULT_ROLE);
    char *configured_default = def_raw ? trim_dup(def_raw, strlen(def_raw)) : NULL;
    if (configured_default && !*configured_default) {
        free(configured_default);
        configured_default = NULL;
    }
    bool builtin = mr_is_builtin_role(role);
    strvec_t role_defaults = {0};
    if (builtin) {
        strvec_t pri = mr_role_priority_defaults(role);
        char *joined = strvec_join(&pri, ",");
        strvec_t empty = {0};
        if (pri.n) nested_patterns(joined, &empty, visited, &role_defaults);
        free(joined);
        strvec_free(&pri);
    }
    /* ROLE_CONFIGURED_FALLBACK */
    const char *fb_role = NULL;
    bool fb_configured_only = false;
    if (str_eq(role, "advisor")) fb_role = "slow", fb_configured_only = true;
    else if (str_eq(role, "memory")) fb_role = "tiny";
    else if (str_eq(role, "tiny") || str_eq(role, "widget")) fb_role = "smol";
    strvec_t resolved = {0};
    bool have_fallback = false;
    if (!configured && fb_role) {
        const char *fr = mr_role_get(fb_role);
        bool fr_set = fr && *fr;
        if (!(fb_configured_only && !fr_set)) {
            char *alias = xasprintf("@%s", fb_role);
            strvec_t vis = {0};
            strvec_extend(&vis, visited);
            have_fallback = configured_patterns_rec(alias, &vis, &resolved);
            strvec_free(&vis);
            free(alias);
        }
    }
    if (configured) {
        nested_patterns(configured, &role_defaults, visited, &resolved);
    } else if (!have_fallback && builtin) {
        /* resolveDefaultInheritedPatterns: smol/slow inherit a configured default first */
        if ((str_eq(role, "smol") || str_eq(role, "slow")) && configured_default)
            nested_patterns(configured_default, &role_defaults, visited, &resolved);
    } else if (!have_fallback) {
        strvec_extend(&resolved, &role_defaults);
    }
    if (!resolved.n) strvec_extend(&resolved, &role_defaults);
    bool ok = resolved.n > 0;
    for (size_t i = 0; i < resolved.n; i++) {
        if (lv) {
            char *s = xasprintf("%s:%s", resolved.v[i], lv);
            strvec_push(out, s);
            free(s);
        } else
            strvec_push(out, resolved.v[i]);
    }
    strvec_free(&resolved);
    strvec_free(&role_defaults);
    free(configured);
    free(configured_default);
    free(role);
    return ok;
}

strvec_t mr_configured_patterns(const char *value) {
    strvec_t out = {0};
    strvec_t pats = mr_pattern_list(value);
    for (size_t i = 0; i < pats.n; i++) {
        strvec_t visited = {0};
        configured_patterns_rec(pats.v[i], &visited, &out);
        strvec_free(&visited);
    }
    strvec_free(&pats);
    return out;
}

char *mr_expand_role_alias(const char *value) {
    char *n = trim_dup(value, strlen(value));
    if (!strcmp(n, DEFAULT_ROLE)) {
        const char *d = mr_role_get(DEFAULT_ROLE);
        free(n);
        return xstrdup(d ? d : value);
    }
    free(n);
    strvec_t out = {0}, visited = {0};
    configured_patterns_rec(value, &visited, &out);
    char *r = xstrdup(out.n ? out.v[0] : value);
    strvec_free(&out);
    strvec_free(&visited);
    return r;
}

/* ---------- preferences ---------- */

static const char *const PROVIDER_ORDER[] = {
    "openai-codex", "anthropic", "openai", "google-gemini-cli", "google", "google-vertex", "kimi-code", "moonshot",
    "qwen-portal", "alibaba-token-plan", "zai", "xai-oauth", "xai", "mistral", "deepseek", "groq", "fireworks",
    "cerebras", "baseten", "deepinfra", "openrouter", "aimlapi", "together", "alibaba-coding-plan", "umans",
    "google-antigravity", "opencode-zen", "gitlab-duo", "opencode-go", "kilo", "vercel-ai-gateway",
    "cloudflare-ai-gateway", "nanogpt", "github-copilot"};

typedef struct {
    strvec_t usage;     /* MRU model keys */
    strvec_t prio;      /* modelProviderOrder then DEFAULT_MODEL_PROVIDER_ORDER (lowercase) */
    const mr_pool_t *pool;
} prefs_t;

static void prefs_init(prefs_t *p, const mr_pool_t *pool) {
    memset(p, 0, sizeof *p);
    p->pool = pool;
    p->usage = mr_usage_order();
    json_t *po = settings_get("modelProviderOrder"), *x;
    json_arr_foreach(po, i, x) if (json_is_str(x)) {
        char *l = trim_dup(json_sv(x), strlen(json_sv(x)));
        for (char *c = l; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (*l && !strvec_contains(&p->prio, l)) strvec_push(&p->prio, l);
        free(l);
    }
    for (size_t i = 0; i < ARRAY_LEN(PROVIDER_ORDER); i++)
        if (!strvec_contains(&p->prio, PROVIDER_ORDER[i])) strvec_push(&p->prio, PROVIDER_ORDER[i]);
}

static void prefs_free(prefs_t *p) {
    strvec_free(&p->usage);
    strvec_free(&p->prio);
}

static long rank_of(const strvec_t *v, const char *s, bool ci) {
    for (size_t i = 0; i < v->n; i++)
        if (ci ? !strcasecmp(v->v[i], s) : !strcmp(v->v[i], s)) return (long)i;
    return -1;
}

static long provider_usage_rank(const prefs_t *p, const char *provider) {
    size_t pl = strlen(provider);
    for (size_t i = 0; i < p->usage.n; i++) {
        const char *k = p->usage.v[i];
        const char *sl = strchr(k, '/');
        if (sl && sl > k && (size_t)(sl - k) == pl && !strncmp(k, provider, pl)) return (long)i;
    }
    return -1;
}

static long pool_order(const prefs_t *p, const ai_model_t *m) {
    long last = 0;
    for (size_t i = 0; p->pool && i < p->pool->n; i++)
        if (p->pool->v[i] == m) last = (long)i;
    return last;
}

/* compare per pickPreferredModel */
static int pref_cmp(const prefs_t *p, const ai_model_t *a, const ai_model_t *b) {
    char *ak = mr_model_key(a), *bk = mr_model_key(b);
    long au = rank_of(&p->usage, ak, false), bu = rank_of(&p->usage, bk, false);
    free(ak);
    free(bk);
    if (au >= 0 || bu >= 0) return (au < 0 ? LONG_MAX : au) < (bu < 0 ? LONG_MAX : bu) ? -1 : (au == bu ? 0 : 1);
    long ap = rank_of(&p->prio, a->provider, true), bp = rank_of(&p->prio, b->provider, true);
    if (ap >= 0 || bp >= 0) {
        long x = ap < 0 ? LONG_MAX : ap, y = bp < 0 ? LONG_MAX : bp;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    long apu = provider_usage_rank(p, a->provider), bpu = provider_usage_rank(p, b->provider);
    if (apu >= 0 || bpu >= 0) {
        long x = apu < 0 ? LONG_MAX : apu, y = bpu < 0 ? LONG_MAX : bpu;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    long ao = pool_order(p, a), bo = pool_order(p, b);
    return ao < bo ? -1 : ao > bo ? 1 : 0;
}

static const ai_model_t *pick_preferred(const prefs_t *p, const mr_pool_t *cands) {
    if (!cands->n) return NULL;
    const ai_model_t *best = cands->v[0];
    for (size_t i = 1; i < cands->n; i++)
        if (pref_cmp(p, cands->v[i], best) < 0) best = cands->v[i];
    return best;
}

/* ---------- matching ---------- */

static char *lower_dup(const char *s) {
    char *r = xstrdup(s);
    for (char *c = r; *c; c++) *c = (char)tolower((unsigned char)*c);
    return r;
}

/* revisionSpellingKey: lowercase, digit-bounded dots become dashes */
static char *spelling_key(const char *id) {
    char *r = lower_dup(id);
    for (size_t i = 1; r[i]; i++)
        if (r[i] == '.' && isdigit((unsigned char)r[i - 1]) && isdigit((unsigned char)r[i + 1])) r[i] = '-';
    return r;
}

/* unique model in pool with provider (ci) and key(id) == want; *ambiguous set when more than one */
static const ai_model_t *provider_lookup(const mr_pool_t *pool, const char *provider, const char *want, bool spelling,
                                         bool *ambiguous) {
    const ai_model_t *found = NULL;
    *ambiguous = false;
    for (size_t i = 0; i < pool->n; i++) {
        const ai_model_t *m = pool->v[i];
        if (strcasecmp(m->provider, provider)) continue;
        char *k = spelling ? spelling_key(m->id) : lower_dup(m->id);
        bool eq = !strcmp(k, want);
        free(k);
        if (!eq) continue;
        if (found) {
            *ambiguous = true;
            return NULL;
        }
        found = m;
    }
    return found;
}

/* reverse index over thinking.effortRouting wire ids */
static const ai_model_t *wire_route_lookup(const mr_pool_t *pool, const char *provider, const char *want) {
    const ai_model_t *found = NULL;
    for (size_t i = 0; i < pool->n; i++) {
        const ai_model_t *m = pool->v[i];
        if (strcasecmp(m->provider, provider)) continue;
        json_t *routing = json_get_obj(m->thinking, "effortRouting");
        const char *k;
        json_t *v;
        json_obj_foreach(routing, j, k, v) {
            (void)k;
            if (!json_is_str(v) || strcasecmp(json_sv(v), want)) continue;
            if (found && found != m) return NULL;
            found = m;
        }
    }
    return found;
}

static bool is_date_suffix(const char *s) { /* "-YYYYMMDD" */
    if (strlen(s) < 9 || s[0] != '-') return false;
    for (int i = 1; i <= 8; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

/* resolveProviderModelReference */
static const ai_model_t *provider_reference(const char *provider_in, const char *id_in, const mr_pool_t *pool) {
    char *provider = trim_dup(provider_in, strlen(provider_in)), *id = trim_dup(id_in, strlen(id_in));
    const ai_model_t *r = NULL;
    bool amb = false;
    if (!*provider || !*id) goto done;
    char *lid = lower_dup(id);
    r = provider_lookup(pool, provider, lid, false, &amb);
    if (r || amb) {
        free(lid);
        goto done;
    }
    /* X-thinking -> X (auto-derived thinking variant pairs) */
    if (str_ends(lid, "-thinking")) {
        lid[strlen(lid) - 9] = 0;
        r = provider_lookup(pool, provider, lid, false, &amb);
        if (r) {
            free(lid);
            goto done;
        }
        free(lid);
        lid = lower_dup(id);
    }
    r = wire_route_lookup(pool, provider, lid);
    if (r) {
        free(lid);
        goto done;
    }
    char *sk = spelling_key(id);
    r = provider_lookup(pool, provider, sk, true, &amb);
    free(sk);
    free(lid);
    if (r) goto done;
    if (strcasecmp(provider, "openrouter")) goto done;
    /* OpenRouter routed (`:suffix`) / dated fallbacks */
    {
        strvec_t queue = {0}, seen = {0};
        strvec_push(&queue, id);
        for (size_t qi = 0; qi < queue.n && !r; qi++) {
            const char *c = queue.v[qi];
            if (strvec_contains(&seen, c)) continue;
            strvec_push(&seen, c);
            if (qi > 0) {
                char *lc = lower_dup(c);
                r = provider_lookup(pool, provider, lc, false, &amb);
                free(lc);
                if (amb) break;
                if (r) break;
            }
            const char *colon = strrchr(c, ':');
            if (colon && colon[1] && !mr_parse_level(colon + 1, true)) {
                char *b = xstrndup(c, (size_t)(colon - c));
                strvec_push(&queue, b);
                free(b);
            }
            for (const char *d = strchr(c, '-'); d; d = strchr(d + 1, '-')) {
                if (is_date_suffix(d) && (d[9] == 0 || d[9] == ':')) {
                    char *s = xasprintf("%.*s%s", (int)(d - c), c, d + 9);
                    strvec_push(&queue, s);
                    free(s);
                    break;
                }
            }
        }
        strvec_free(&queue);
        strvec_free(&seen);
    }
done:
    free(provider);
    free(id);
    return r;
}

static bool is_alias_id(const char *id) {
    if (str_ends(id, "-latest")) return true;
    size_t n = strlen(id);
    return !(n >= 9 && is_date_suffix(id + n - 9));
}

/* isProviderLockedCrossMatch (bundled catalog = whole catalog here) */
static bool provider_locked_cross(const char *pattern, const ai_model_t *matched) {
    const char *sl = strchr(pattern, '/');
    if (!sl || sl == pattern) return false;
    char *provider = xstrndup(pattern, (size_t)(sl - pattern));
    bool locked = false;
    if (strcasecmp(matched->provider, provider)) {
        char *want = spelling_key(sl + 1);
        bool matched_bundled = false;
        for (size_t i = 0; i < catalog_count() && !locked; i++) {
            const ai_model_t *m = catalog_at(i);
            if (!strcasecmp(m->provider, matched->provider)) matched_bundled = true;
            if (strcasecmp(m->provider, provider)) continue;
            char *k = spelling_key(m->id);
            if (!strcmp(k, want)) locked = true;
            free(k);
        }
        free(want);
        if (!matched_bundled) locked = false;
    }
    free(provider);
    return locked;
}

static const ai_model_t *match_with(const char *pattern, const mr_pool_t *pool, const prefs_t *pf, bool exact_only) {
    if (!pattern || !*pattern || !pool) return NULL;
    /* 1. exact provider/id reference */
    const char *sl = strchr(pattern, '/');
    if (sl) {
        char *prov = xstrndup(pattern, (size_t)(sl - pattern));
        const ai_model_t *r = provider_reference(prov, sl + 1, pool);
        free(prov);
        if (r) return r;
    }
    /* 2. exact bare id */
    char *lp = lower_dup(pattern);
    mr_pool_t ex = {0}, unlocked = {0};
    for (size_t i = 0; i < pool->n; i++)
        if (!strcasecmp(pool->v[i]->id, pattern)) mr_pool_push(&ex, pool->v[i]);
    const ai_model_t *r = NULL;
    if (ex.n) {
        for (size_t i = 0; i < ex.n; i++)
            if (!provider_locked_cross(pattern, ex.v[i])) mr_pool_push(&unlocked, ex.v[i]);
        r = pick_preferred(pf, &unlocked);
        mr_pool_free(&ex);
        mr_pool_free(&unlocked);
        free(lp);
        return r;
    }
    /* 3. retired `X-thinking` variant alias */
    if (str_ends(lp, "-thinking")) {
        char *t = xstrndup(lp, strlen(lp) - 9);
        for (size_t i = 0; i < pool->n; i++)
            if (!strcasecmp(pool->v[i]->id, t)) mr_pool_push(&ex, pool->v[i]);
        free(t);
        r = pick_preferred(pf, &ex);
        mr_pool_free(&ex);
        if (r) {
            free(lp);
            return r;
        }
    }
    if (exact_only) {
        free(lp);
        return NULL;
    }
    /* a provider/... selector naming a known provider resolves exactly or not at all */
    if (sl) {
        char *prov = xstrndup(pattern, (size_t)(sl - pattern));
        bool known = mr_provider_known(prov);
        for (size_t i = 0; i < pool->n && !known; i++)
            if (!strcasecmp(pool->v[i]->provider, prov)) known = true;
        free(prov);
        if (known) {
            free(lp);
            return NULL;
        }
    }
    /* 4. substring match: aliases preferred, else the newest dated id */
    mr_pool_t aliases = {0}, dated = {0};
    for (size_t i = 0; i < pool->n; i++) {
        const ai_model_t *m = pool->v[i];
        char *li = lower_dup(m->id);
        char *ln = m->name ? lower_dup(m->name) : NULL;
        bool hit = strstr(li, lp) || (ln && strstr(ln, lp));
        free(li);
        free(ln);
        if (!hit) continue;
        mr_pool_push(is_alias_id(m->id) ? &aliases : &dated, m);
    }
    free(lp);
    if (aliases.n) r = pick_preferred(pf, &aliases);
    else if (dated.n == 1) r = dated.v[0];
    else if (dated.n) {
        const char *top = NULL;
        for (size_t i = 0; i < dated.n; i++)
            if (!top || strcmp(dated.v[i]->id, top) > 0) top = dated.v[i]->id;
        mr_pool_t tops = {0};
        for (size_t i = 0; i < dated.n; i++)
            if (!strcmp(dated.v[i]->id, top)) mr_pool_push(&tops, dated.v[i]);
        r = pick_preferred(pf, &tops);
        mr_pool_free(&tops);
    }
    mr_pool_free(&aliases);
    mr_pool_free(&dated);
    return r;
}

const ai_model_t *mr_match(const char *pattern, const mr_pool_t *pool, bool exact_only) {
    prefs_t pf;
    prefs_init(&pf, pool);
    const ai_model_t *m = match_with(pattern, pool, &pf, exact_only);
    prefs_free(&pf);
    return m;
}

void mr_result_clear(mr_result_t *r) {
    free(r->warning);
    memset(r, 0, sizeof *r);
}

/* inferWireRouteThinkingLevel */
static const char *wire_route_level(const char *pattern, const ai_model_t *m) {
    json_t *routing = json_get_obj(m->thinking, "effortRouting");
    if (!routing) return NULL;
    char *n = lower_dup(pattern);
    char *pp = xasprintf("%s/", m->provider);
    char *lpp = lower_dup(pp);
    const char *wire = str_starts(n, lpp) ? n + strlen(lpp) : n;
    const char *found = NULL;
    int count = 0;
    if (strcasecmp(wire, m->id)) {
        const char *k;
        json_t *v;
        json_obj_foreach(routing, i, k, v) {
            if (json_is_str(v) && !strcasecmp(json_sv(v), wire)) {
                found = mr_parse_level(k, true);
                count++;
            }
        }
    }
    free(n);
    free(pp);
    free(lpp);
    return count == 1 ? found : NULL;
}

static bool parse_with(const char *pattern, const mr_pool_t *pool, const prefs_t *pf, mr_result_t *out) {
    memset(out, 0, sizeof *out);
    const ai_model_t *exact = match_with(pattern, pool, pf, true);
    if (exact) {
        out->model = exact;
        out->level = wire_route_level(pattern, exact);
        out->explicit_level = out->level != NULL;
        return true;
    }
    char *base;
    const char *lv = mr_split_level(pattern, -1, true, &base);
    if (lv) {
        const ai_model_t *lit = match_with(pattern, pool, pf, false);
        char *suffix = xasprintf(":%s", lv);
        bool lit_ok = lit && str_ends(lit->id, suffix);
        free(suffix);
        if (lit_ok) {
            free(base);
            out->model = lit;
            return true;
        }
        mr_result_t in;
        parse_with(base, pool, pf, &in);
        free(base);
        *out = in;
        if (in.model) {
            out->explicit_level = !in.warning;
            out->level = out->explicit_level ? lv : NULL;
            return true;
        }
        return false;
    }
    free(base);
    const ai_model_t *fb = match_with(pattern, pool, pf, false);
    if (fb) {
        out->model = fb;
        return true;
    }
    const char *colon = strrchr(pattern, ':');
    if (!colon) return false;
    char *prefix = xstrndup(pattern, (size_t)(colon - pattern));
    mr_result_t in;
    parse_with(prefix, pool, pf, &in);
    free(prefix);
    if (in.model) {
        free(in.warning);
        out->model = in.model;
        out->warning = xasprintf("Invalid thinking level \"%s\" in pattern \"%s\". Using default instead.", colon + 1,
                                 pattern);
        return true;
    }
    *out = in;
    return false;
}

/* splitUpstreamRouting: base owned, upstream owned */
static bool split_upstream(const char *pattern, char **base, char **upstream) {
    const char *at = strrchr(pattern, '@');
    if (!at || at == pattern) return false;
    const char *rest = at + 1;
    const char *colon = strchr(rest, ':');
    size_t ul = colon ? (size_t)(colon - rest) : strlen(rest);
    if (!ul) return false;
    /* slug: [a-z0-9](?:[a-z0-9-]*[a-z0-9])? segments separated by '/' */
    bool ok = true;
    size_t seg = 0;
    for (size_t i = 0; i < ul && ok; i++) {
        char c = rest[i];
        if (c == '/') {
            if (!seg || rest[i - 1] == '-') ok = false;
            seg = 0;
        } else if (isalnum((unsigned char)c) || (c == '-' && seg)) {
            seg++;
        } else
            ok = false;
    }
    if (!ok || !seg || rest[ul - 1] == '-') return false;
    *upstream = xstrndup(rest, ul);
    *base = xasprintf("%.*s%s", (int)(at - pattern), pattern, colon ? colon : "");
    return true;
}

static bool supports_upstream(const ai_model_t *m) {
    return str_eq(m->provider, "openrouter") || str_eq(m->provider, "vercel-ai-gateway") ||
           (m->base_url && (strstr(m->base_url, "openrouter.ai") || strstr(m->base_url, "ai-gateway.vercel")));
}

static bool match_pattern(const char *pattern, const mr_pool_t *pool, const prefs_t *pf, mr_result_t *out) {
    if (parse_with(pattern, pool, pf, out)) return true;
    char *base, *up;
    if (split_upstream(pattern, &base, &up)) {
        mr_result_t routed;
        if (parse_with(base, pool, pf, &routed) && supports_upstream(routed.model)) {
            /* omc has no per-request routing pin for a resolved model: the base model serves */
            mr_result_clear(out);
            *out = routed;
            free(base);
            free(up);
            return true;
        }
        mr_result_clear(&routed);
        free(base);
        free(up);
    }
    return false;
}

bool mr_parse_pattern(const char *pattern, const mr_pool_t *pool, mr_result_t *out) {
    prefs_t pf;
    prefs_init(&pf, pool);
    bool ok = match_pattern(pattern, pool, &pf, out);
    prefs_free(&pf);
    return ok;
}

/* resolveThinkingLevelForModel (clamp a concrete level to the model) */
static const char *level_for_model(const ai_model_t *m, const char *lv) {
    if (!lv || !strcmp(lv, "auto") || !strcmp(lv, "inherit")) return lv;
    ai_effort_t e;
    if (effort_parse(lv, &e) != 0) return lv;
    if (e == EFFORT_OFF) return "off";
    if (!m->reasoning) return "off";
    return effort_name(model_clamp_effort(m, e));
}

bool mr_resolve_value(const char *value, const mr_pool_t *pool_in, mr_result_t *out) {
    memset(out, 0, sizeof *out);
    if (!value) return false;
    char *n = trim_dup(value, strlen(value));
    if (!*n || !strcmp(n, DEFAULT_ROLE)) {
        free(n);
        return false;
    }
    mr_pool_t own = {0};
    const mr_pool_t *pool = pool_in;
    if (!pool) {
        mr_pool_available(&own, "chat");
        pool = &own;
    }
    strvec_t pats = mr_configured_patterns(n);
    free(n);
    prefs_t pf;
    prefs_init(&pf, pool);
    char *warning = NULL;
    bool ok = false;
    for (size_t i = 0; i < pats.n; i++) {
        mr_result_t r;
        if (match_pattern(pats.v[i], pool, &pf, &r)) {
            *out = r;
            out->matched_index = (int)i;
            if (r.explicit_level) out->level = level_for_model(r.model, r.level);
            ok = true;
            break;
        }
        if (!warning && r.warning) warning = xstrdup(r.warning);
        mr_result_clear(&r);
    }
    if (!ok) out->warning = warning;
    else free(warning);
    prefs_free(&pf);
    strvec_free(&pats);
    mr_pool_free(&own);
    return ok;
}

const ai_model_t *mr_resolve_string(const char *value, const mr_pool_t *pool) {
    if (!value) return NULL;
    for (size_t i = 0; i < pool->n; i++) {
        char *k = mr_model_key(pool->v[i]);
        bool eq = !strcmp(k, value);
        free(k);
        if (eq) return pool->v[i];
    }
    mr_result_t r;
    mr_parse_pattern(value, pool, &r);
    const ai_model_t *m = r.model;
    mr_result_clear(&r);
    return m;
}

bool mr_resolve_override(const strvec_t *patterns, mr_result_t *out) {
    memset(out, 0, sizeof *out);
    mr_pool_t pool = {0};
    mr_pool_available(&pool, "chat");
    char *warning = NULL;
    bool ok = false;
    for (size_t i = 0; i < patterns->n && !ok; i++) {
        mr_result_t r;
        if (mr_resolve_value(patterns->v[i], &pool, &r)) {
            *out = r;
            ok = true;
        } else {
            if (!warning && r.warning) warning = xstrdup(r.warning);
            mr_result_clear(&r);
        }
    }
    if (!ok) out->warning = warning;
    else free(warning);
    mr_pool_free(&pool);
    return ok;
}

const ai_model_t *mr_role_selection(const char *role, const mr_pool_t *pool, const char **level) {
    if (level) *level = NULL;
    const char *v = mr_role_get(role);
    if (!v) return NULL;
    mr_result_t r;
    const ai_model_t *m = mr_resolve_value(v, pool, &r) ? r.model : NULL;
    if (m && level) *level = r.level;
    mr_result_clear(&r);
    return m;
}

const ai_model_t *mr_resolve_role(const char *role, const char **level) {
    if (level) *level = NULL;
    mr_pool_t pool = {0};
    mr_pool_role(&pool, role);
    char *alias = xasprintf("@%s", role);
    mr_result_t r;
    const ai_model_t *m = mr_resolve_value(alias, &pool, &r) ? r.model : NULL;
    if (m && level) *level = r.level;
    mr_result_clear(&r);
    free(alias);
    mr_pool_free(&pool);
    return m;
}

const ai_model_t *mr_pick_default_available(const mr_pool_t *pool) {
    mr_pool_t autos = {0}, concrete = {0};
    for (size_t i = 0; i < pool->n; i++)
        if (json_get_bool(catalog_provider_info(pool->v[i]->provider), "automaticDefault", true))
            mr_pool_push(&autos, pool->v[i]);
    /* providers with concrete credentials win over ambient/keyless ones */
    for (size_t i = 0; i < autos.n; i++)
        if (auth_has_credentials(autos.v[i]->provider)) mr_pool_push(&concrete, autos.v[i]);
    const mr_pool_t *models = concrete.n ? &concrete : &autos;
    const ai_model_t *first = NULL;
    for (size_t i = 0; i < models->n && !first; i++) {
        const ai_model_t *m = models->v[i];
        bool acct = json_get_bool(m->raw, "isProviderDefault", false);
        const char *dm = catalog_provider_default_model(m->provider);
        if (acct || (dm && !strcmp(dm, m->id))) first = m;
    }
    const ai_model_t *r = NULL;
    if (!first) r = models->n ? models->v[0] : NULL;
    else {
        strvec_t prio = {0};
        for (size_t i = 0; i < ARRAY_LEN(PROVIDER_ORDER); i++) strvec_push(&prio, PROVIDER_ORDER[i]);
        long best = LONG_MAX;
        r = first;
        for (size_t i = 0; i < models->n; i++) {
            const ai_model_t *m = models->v[i];
            const char *dm = catalog_provider_default_model(m->provider);
            if (strcmp(m->id, first->id) || !(json_get_bool(m->raw, "isProviderDefault", false) || (dm && !strcmp(dm, m->id))))
                continue;
            long rk = rank_of(&prio, m->provider, true);
            if (rk < 0) rk = LONG_MAX - 1;
            if (rk < best) {
                best = rk;
                r = m;
            }
        }
        strvec_free(&prio);
    }
    mr_pool_free(&autos);
    mr_pool_free(&concrete);
    return r;
}

const ai_model_t *mr_default_model(const char **level) {
    if (level) *level = NULL;
    mr_pool_t pool = {0};
    mr_pool_available(&pool, "chat");
    const ai_model_t *m = NULL;
    const char *def = mr_role_get(DEFAULT_ROLE);
    if (def) {
        mr_result_t r;
        if (mr_resolve_value(def, &pool, &r)) {
            m = r.model;
            if (level) *level = r.level;
        }
        mr_result_clear(&r);
    }
    if (!m) m = mr_pick_default_available(&pool);
    mr_pool_free(&pool);
    return m;
}

/* ---------- role chains ---------- */

void mr_chain_free(mr_chain_t *c) {
    for (size_t i = 0; i < c->n; i++) free(c->v[i].replaces);
    free(c->v);
    memset(c, 0, sizeof *c);
}

static json_t *fallback_chains(void) {
    json_t *c = settings_get("retry.fallbackChains");
    return json_is_obj(c) ? c : NULL;
}

void mr_role_chain(const char *role, const mr_pool_t *pool_in, mr_chain_t *out) {
    memset(out, 0, sizeof *out);
    mr_pool_t own = {0};
    const mr_pool_t *pool = pool_in;
    if (!pool) {
        mr_pool_role(&own, role);
        pool = &own;
    }
    const char *configured = mr_role_get(role);
    char *ct = configured ? trim_dup(configured, strlen(configured)) : NULL;
    char *primary = ct && *ct ? xstrdup(ct) : xasprintf("@%s", role);
    free(ct);
    json_t *roles = settings_get("modelRoles");
    bool primary_explicit = json_is_obj(roles) && json_get(roles, role) != NULL;
    json_t *cfg_chain = json_get(fallback_chains(), role);
    bool has_cfg_chain = json_is_arr(cfg_chain);
    strvec_t sels = {0};
    strvec_push(&sels, primary);
    free(primary);
    if (has_cfg_chain) {
        json_t *x;
        json_arr_foreach(cfg_chain, i, x) if (json_is_str(x)) strvec_push(&sels, json_sv(x));
    } else {
        strvec_t d = mr_role_priority_defaults(role);
        strvec_extend(&sels, &d);
        strvec_free(&d);
    }
    char *unresolved = NULL;
    strvec_t keys = {0};
    for (size_t i = 0; i < sels.n; i++) {
        bool expl = i == 0 ? primary_explicit : has_cfg_chain;
        mr_result_t r;
        bool ok = mr_resolve_value(sels.v[i], pool, &r);
        if (expl && (!ok || r.matched_index > 0)) {
            strvec_t pats = mr_configured_patterns(sels.v[i]);
            if (!pats.n) strvec_push(&pats, sels.v[i]);
            size_t lim = ok ? (size_t)r.matched_index : pats.n;
            for (size_t k = 0; k < lim && k < pats.n; k++)
                if (strchr(pats.v[k], '/')) {
                    if (!unresolved) unresolved = xstrdup(pats.v[k]);
                    break;
                }
            strvec_free(&pats);
        }
        if (!ok) {
            mr_result_clear(&r);
            continue;
        }
        char *key = mr_model_key(r.model);
        long idx = rank_of(&keys, key, false);
        if (idx >= 0) {
            if (expl) out->v[idx].explicit_ = true;
            free(key);
            mr_result_clear(&r);
            continue;
        }
        strvec_push(&keys, key);
        free(key);
        if (out->n == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 8;
            out->v = xrealloc(out->v, out->cap * sizeof *out->v);
        }
        mr_candidate_t c = {r.model, expl, r.level, NULL};
        if (unresolved && out->n == 0) c.replaces = xstrdup(unresolved);
        out->v[out->n++] = c;
        mr_result_clear(&r);
    }
    free(unresolved);
    strvec_free(&keys);
    strvec_free(&sels);
    mr_pool_free(&own);
}

/* ---------- retry fallback chains ---------- */

void mr_selector_free(mr_selector_t *s) {
    free(s->raw);
    free(s->provider);
    free(s->id);
    memset(s, 0, sizeof *s);
}

static bool parse_selector_lookup(const char *selector, mr_selector_t *out) {
    memset(out, 0, sizeof *out);
    char *t = trim_dup(selector, strlen(selector));
    const char *sl = strchr(t, '/');
    if (!*t || !sl || sl == t) {
        free(t);
        return false;
    }
    out->raw = t;
    out->provider = xstrndup(t, (size_t)(sl - t));
    const char *id = sl + 1;
    char *base;
    const char *lv = mr_split_level(id, -1, false, &base);
    if (lv) {
        out->id = base;
        out->level = lv;
        return true;
    }
    free(base);
    lv = mr_split_level(id, -1, true, &base);
    if (lv && !catalog_find(out->provider, id)) {
        out->id = base;
        out->level = lv;
        return true;
    }
    free(base);
    out->id = xstrdup(id);
    return true;
}

bool mr_parse_selector(const char *selector, mr_selector_t *out) { return parse_selector_lookup(selector, out); }

static bool is_model_key(const char *k) { return strchr(k, '/') != NULL; }
static bool is_wildcard_key(const char *k) { return str_ends(k, "/*"); }

static bool provider_registered(const char *p) {
    for (size_t i = 0; i < catalog_count(); i++)
        if (!strcmp(catalog_at(i)->provider, p)) return true;
    return catalog_provider_info(p) != NULL;
}

/* parseRetryFallbackWildcard: provider + optional id prefix (owned) */
static void parse_wildcard(const char *key, char **provider, char **prefix) {
    char *tpl = xstrndup(key, strlen(key) - 2);
    char *sl = strchr(tpl, '/');
    *prefix = NULL;
    if (!sl || provider_registered(tpl)) {
        *provider = tpl;
        return;
    }
    *provider = xstrndup(tpl, (size_t)(sl - tpl));
    *prefix = xstrdup(sl + 1);
    free(tpl);
}

/* the effective chains: configured + the default chain applied to configured roles without their own */
static json_t *effective_chains(void) {
    json_t *cfg = fallback_chains();
    json_t *c = cfg ? json_clone(cfg) : json_obj();
    json_t *dflt = json_get(c, "default");
    if (json_is_arr(dflt)) {
        json_t *roles = settings_get("modelRoles"), *v;
        const char *k;
        json_obj_foreach(roles, i, k, v) {
            (void)v;
            if (str_eq(k, "default") || mr_is_kind_role(k) || json_get(c, k)) continue;
            json_set(c, k, json_clone(dflt));
        }
    }
    return c;
}

static bool primary_selector(const char *key, mr_selector_t *out) {
    memset(out, 0, sizeof *out);
    if (is_wildcard_key(key)) return false;
    if (is_model_key(key)) return parse_selector_lookup(key, out);
    const char *cs = mr_role_get(key);
    return cs && parse_selector_lookup(cs, out);
}

enum { MK_NONE, MK_EFFORT, MK_BASE, MK_NORMALIZED, MK_EXACT };

static int match_kind(const mr_selector_t *primary, bool has_primary, const mr_selector_t *cur, const mr_selector_t *plain,
                      const ai_model_t *model) {
    if (!has_primary) return MK_NONE;
    const mr_selector_t *mc = NULL;
    if (str_eq(primary->provider, cur->provider) && str_eq(primary->id, cur->id)) mc = cur;
    else if (plain && str_eq(primary->provider, plain->provider) && str_eq(primary->id, plain->id)) mc = plain;
    if (!mc) return MK_NONE;
    if (primary->level == mc->level || (primary->level && mc->level && !strcmp(primary->level, mc->level)))
        return MK_EXACT;
    if (!primary->level || !strcmp(primary->level, "auto")) return MK_BASE;
    if (model) {
        const char *a = level_for_model(model, primary->level);
        const char *b = level_for_model(model, mc->level && strcmp(mc->level, "auto") ? mc->level : "high");
        if (a && b && !strcmp(a, b)) return MK_NORMALIZED;
    }
    return MK_EFFORT;
}

static bool current_selectors(const char *current_selector, const ai_model_t *model, mr_selector_t *cur,
                              mr_selector_t *plain, bool *has_plain) {
    *has_plain = false;
    bool ok = parse_selector_lookup(current_selector, cur);
    char *plain_s = NULL;
    if (model) {
        char *k = mr_model_key(model);
        plain_s = mr_selector_with_level(k, ok ? cur->level : NULL);
        free(k);
    }
    if (!ok) {
        if (!plain_s || !parse_selector_lookup(plain_s, cur)) {
            free(plain_s);
            return false;
        }
    } else if (plain_s && strcmp(plain_s, current_selector)) {
        *has_plain = parse_selector_lookup(plain_s, plain);
    }
    free(plain_s);
    return true;
}

static char *chain_key_in(json_t *chains, const char *current_selector, const ai_model_t *model, const char *role_hint) {
    mr_selector_t cur, plain;
    bool has_plain;
    char *result = NULL;
    if (!current_selectors(current_selector, model, &cur, &plain, &has_plain)) {
        if (role_hint && json_is_arr(json_get(chains, role_hint))) result = xstrdup(role_hint);
        return result;
    }
    const char *k;
    json_t *v;
    const char *normalized = NULL, *base = NULL;
    json_obj_foreach(chains, i, k, v) {
        if (!is_model_key(k) || is_wildcard_key(k)) continue;
        mr_selector_t p;
        bool hp = primary_selector(k, &p);
        int mk = match_kind(&p, hp, &cur, has_plain ? &plain : NULL, model);
        mr_selector_free(&p);
        if (mk == MK_EXACT) {
            result = xstrdup(k);
            goto out;
        }
        if (mk == MK_NORMALIZED && !normalized) normalized = k;
        if (mk == MK_BASE && !base) base = k;
    }
    if (normalized || base) {
        result = xstrdup(normalized ? normalized : base);
        goto out;
    }
    {
        const char *wild = NULL;
        long wlen = -1;
        json_obj_foreach(chains, i2, k, v) {
            if (!is_wildcard_key(k) || !json_is_arr(v)) continue;
            char *prov, *prefix;
            parse_wildcard(k, &prov, &prefix);
            bool ok = str_eq(prov, cur.provider);
            if (ok && prefix) {
                char *pp = xasprintf("%s/", prefix);
                ok = str_starts(cur.id, pp);
                free(pp);
            }
            long pl = prefix ? (long)strlen(prefix) : 0;
            if (ok && pl > wlen) {
                wild = k;
                wlen = pl;
            }
            free(prov);
            free(prefix);
        }
        if (wild) {
            result = xstrdup(wild);
            goto out;
        }
    }
    if (role_hint && json_is_arr(json_get(chains, role_hint))) {
        result = xstrdup(role_hint);
        goto out;
    }
    {
        const char *matched = NULL, *effort = NULL;
        json_obj_foreach(chains, i3, k, v) {
            if (is_model_key(k)) continue;
            mr_selector_t p;
            bool hp = primary_selector(k, &p);
            int mk = match_kind(&p, hp, &cur, has_plain ? &plain : NULL, model);
            mr_selector_free(&p);
            if (mk == MK_NONE) continue;
            if (mk == MK_EFFORT) {
                if (str_eq(k, "default") || !effort) effort = k;
                continue;
            }
            if (str_eq(k, "default")) {
                result = xstrdup("default");
                goto out;
            }
            if (!matched) matched = k;
        }
        if (matched || effort) {
            result = xstrdup(matched ? matched : effort);
            goto out;
        }
    }
    if (json_len(json_get_arr(chains, "default"))) result = xstrdup("default");
out:
    mr_selector_free(&cur);
    if (has_plain) mr_selector_free(&plain);
    return result;
}

char *mr_fallback_chain_key(const char *current_selector, const ai_model_t *model, const char *role_hint) {
    json_t *chains = effective_chains();
    char *r = chain_key_in(chains, current_selector, model, role_hint);
    json_free(chains);
    return r;
}

/* parseRetryFallbackChainEntry */
static bool parse_chain_entry(const char *entry, const mr_selector_t *cur, mr_selector_t *out) {
    if (!is_wildcard_key(entry)) return parse_selector_lookup(entry, out);
    memset(out, 0, sizeof *out);
    if (!cur) return false;
    char *prov, *prefix;
    parse_wildcard(entry, &prov, &prefix);
    const char *ls = strrchr(cur->id, '/');
    const char *bare = ls ? ls + 1 : cur->id;
    char *id;
    if (prefix) id = xasprintf("%s/%s", prefix, bare);
    else if (bare != cur->id && !catalog_find(prov, cur->id) && catalog_find(prov, bare)) id = xstrdup(bare);
    else id = xstrdup(cur->id);
    out->raw = xasprintf("%s/%s", prov, id);
    out->provider = prov;
    out->id = id;
    out->level = NULL;
    free(prefix);
    return true;
}

static void sel_vec_push(mr_selector_t **v, size_t *n, size_t *cap, mr_selector_t s) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *v = xrealloc(*v, *cap * sizeof **v);
    }
    (*v)[(*n)++] = s;
}

static bool sel_seen(const mr_selector_t *v, size_t n, const char *raw) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(v[i].raw, raw)) return true;
    return false;
}

static void sel_copy(mr_selector_t *dst, const mr_selector_t *src) {
    dst->raw = xstrdup(src->raw);
    dst->provider = xstrdup(src->provider);
    dst->id = xstrdup(src->id);
    dst->level = src->level;
}

static size_t candidates_in(json_t *chains, const char *key, const char *current_selector, const ai_model_t *model,
                            bool wrap, bool allow_missing, mr_selector_t **out) {
    *out = NULL;
    mr_selector_t cur = {0};
    bool has_cur = parse_selector_lookup(current_selector, &cur);
    if (!has_cur && model) {
        char *k = mr_model_key(model);
        has_cur = parse_selector_lookup(k, &cur);
        free(k);
    }
    mr_selector_t *chain = NULL;
    size_t n = 0, cap = 0;
    bool aborted = false;
    if (is_wildcard_key(key)) {
        if (has_cur) {
            mr_selector_t c;
            sel_copy(&c, &cur);
            sel_vec_push(&chain, &n, &cap, c);
        }
    } else {
        mr_selector_t p;
        if (primary_selector(key, &p)) sel_vec_push(&chain, &n, &cap, p);
        else if (has_cur && (allow_missing || str_eq(key, "default"))) {
            mr_selector_t c;
            sel_copy(&c, &cur);
            sel_vec_push(&chain, &n, &cap, c);
        } else
            aborted = true;
    }
    if (!aborted) {
        json_t *entries = json_get_arr(chains, key), *x;
        json_arr_foreach(entries, i, x) {
            if (!json_is_str(x)) continue;
            mr_selector_t s;
            if (!parse_chain_entry(json_sv(x), has_cur ? &cur : NULL, &s)) continue;
            if (sel_seen(chain, n, s.raw)) {
                mr_selector_free(&s);
                continue;
            }
            sel_vec_push(&chain, &n, &cap, s);
        }
    }
    /* position of the current selector */
    mr_selector_t cs = {0};
    bool has_cs = parse_selector_lookup(current_selector, &cs);
    char *plain = NULL;
    if (model) {
        char *k = mr_model_key(model);
        plain = mr_selector_with_level(k, has_cs ? cs.level : NULL);
        free(k);
    }
    if (!has_cs && plain) has_cs = parse_selector_lookup(plain, &cs);
    size_t start = 0, count = n;
    long at = -1;
    if (has_cs) {
        if (n <= 1) count = 0;
        else {
            for (size_t i = 0; i < n && at < 0; i++)
                if (!strcmp(chain[i].raw, current_selector) || (plain && !strcmp(chain[i].raw, plain))) at = (long)i;
            if (at < 0) {
                char *cb = xasprintf("%s/%s", cs.provider, cs.id);
                char *pb = NULL;
                if (plain && strcmp(plain, current_selector)) {
                    mr_selector_t ps;
                    if (parse_selector_lookup(plain, &ps)) {
                        pb = xasprintf("%s/%s", ps.provider, ps.id);
                        mr_selector_free(&ps);
                    }
                }
                for (size_t i = 0; i < n && at < 0; i++) {
                    char *b = xasprintf("%s/%s", chain[i].provider, chain[i].id);
                    if (!strcmp(b, cb) || (pb && !strcmp(b, pb))) at = (long)i;
                    free(b);
                }
                free(cb);
                free(pb);
            }
        }
    }
    mr_selector_t *res = NULL;
    size_t rn = 0, rcap = 0;
    if (has_cs && n > 1 && at >= 0) {
        for (size_t i = (size_t)at + 1; i < n; i++) {
            mr_selector_t c;
            sel_copy(&c, &chain[i]);
            sel_vec_push(&res, &rn, &rcap, c);
        }
        if (wrap)
            for (size_t i = 0; i < (size_t)at; i++) {
                mr_selector_t c;
                sel_copy(&c, &chain[i]);
                sel_vec_push(&res, &rn, &rcap, c);
            }
    } else if (!has_cs || (n > 1 && at < 0)) {
        for (size_t i = start; i < count; i++) {
            mr_selector_t c;
            sel_copy(&c, &chain[i]);
            sel_vec_push(&res, &rn, &rcap, c);
        }
    }
    for (size_t i = 0; i < n; i++) mr_selector_free(&chain[i]);
    free(chain);
    if (has_cs) mr_selector_free(&cs);
    if (has_cur) mr_selector_free(&cur);
    free(plain);
    *out = res;
    return rn;
}

size_t mr_fallback_candidates(const char *key, const char *current_selector, const ai_model_t *model, bool wrap,
                              mr_selector_t **out) {
    json_t *chains = effective_chains();
    size_t n = candidates_in(chains, key, current_selector, model, wrap, false, out);
    json_free(chains);
    return n;
}

/* ---------- online tiny candidates (tiny/online-candidates.ts) ---------- */

static bool model_in(const mr_pool_t *p, const char *provider, const char *id, const ai_model_t **found) {
    for (size_t i = 0; i < p->n; i++)
        if (!strcmp(p->v[i]->provider, provider) && !strcmp(p->v[i]->id, id)) {
            *found = p->v[i];
            return true;
        }
    return false;
}

/* expandFallbackCandidates: transitive walk from the seeds through their chain keys */
static void expand_fallbacks(json_t *chains, const ai_model_t *seed, const char *selector, const char *role_hint,
                             const mr_pool_t *pool, mr_pool_t *out) {
    typedef struct {
        const ai_model_t *m;
        char *sel, *hint;
    } item_t;
    VEC(item_t) q = {0};
    item_t first = {seed, xstrdup(selector), role_hint ? xstrdup(role_hint) : NULL};
    vec_push(&q, first);
    strvec_t expanded = {0};
    for (size_t qi = 0; qi < q.n; qi++) {
        item_t it = q.v[qi];
        char *key = chain_key_in(chains, it.sel, it.m, it.hint);
        if (key) {
            char *mk = mr_model_key(it.m);
            char *ek = xasprintf("%s%c%s", key, 1, mk);
            if (!strvec_contains(&expanded, ek)) {
                strvec_push(&expanded, ek);
                mr_selector_t *c = NULL;
                size_t n = candidates_in(chains, key, mk, it.m, false, true, &c);
                for (size_t k = 0; k < n; k++) {
                    strvec_t one = {0};
                    strvec_push(&one, c[k].raw);
                    mr_result_t r;
                    const ai_model_t *fb = NULL;
                    if (mr_resolve_value(c[k].raw, pool, &r)) fb = r.model;
                    mr_result_clear(&r);
                    strvec_free(&one);
                    if (!fb) model_in(pool, c[k].provider, c[k].id, &fb);
                    if (fb && !mr_pool_contains(out, fb)) {
                        mr_pool_push(out, fb);
                        item_t nx = {fb, mr_model_key(fb), NULL};
                        vec_push(&q, nx);
                    }
                    mr_selector_free(&c[k]);
                }
                free(c);
            }
            free(ek);
            free(mk);
            free(key);
        }
    }
    for (size_t i = 0; i < q.n; i++) {
        free(q.v[i].sel);
        free(q.v[i].hint);
    }
    free(q.v);
    strvec_free(&expanded);
}

void mr_online_tiny_candidates(const char *const *roles, size_t nroles, const ai_model_t *current, mr_pool_t *out) {
    mr_pool_t pool = {0};
    mr_pool_role(&pool, "tiny");
    if (!pool.n) {
        mr_pool_free(&pool);
        return;
    }
    bool fallback = settings_bool("retry.modelFallback", true);
    mr_pool_t prim = {0};
    strvec_t prim_roles = {0};
    for (size_t r = 0; r < nroles; r++) {
        const ai_model_t *m = mr_role_selection(roles[r], &pool, NULL);
        if (!m) continue;
        if (!mr_pool_contains(out, m)) mr_pool_push(out, m);
        if (!fallback) break;
        mr_pool_push(&prim, m);
        strvec_push(&prim_roles, roles[r]);
    }
    json_t *cfg = fallback_chains();
    if (fallback && cfg) {
        /* expandDefaultRetryFallbackChains(configured, roles) */
        json_t *chains = json_clone(cfg);
        json_t *dflt = json_get(chains, "default");
        if (json_is_arr(dflt))
            for (size_t r = 0; r < nroles; r++)
                if (!str_eq(roles[r], "default") && !mr_is_kind_role(roles[r]) && !json_get(chains, roles[r]))
                    json_set(chains, roles[r], json_clone(dflt));
        for (size_t i = 0; i < prim.n; i++) {
            const char *rv = mr_role_get(prim_roles.v[i]);
            char *mk = mr_model_key(prim.v[i]);
            expand_fallbacks(chains, prim.v[i], rv ? rv : mk, prim_roles.v[i], &pool, out);
            free(mk);
        }
        json_free(chains);
    }
    /* title-generator.ts: the current model, then its own chain (configured keys only) */
    if (current && (!out->n || fallback) && !mr_pool_contains(out, current)) {
        mr_pool_push(out, current);
        if (fallback && cfg) {
            char *mk = mr_model_key(current);
            expand_fallbacks(cfg, current, mk, NULL, &pool, out);
            free(mk);
        }
    }
    mr_pool_free(&prim);
    strvec_free(&prim_roles);
    mr_pool_free(&pool);
}

size_t mr_fallback_validate(strvec_t *out) {
    json_t *cfg = settings_get("retry.fallbackChains");
    if (!cfg) return 0;
    size_t before = out->n;
    if (!json_is_obj(cfg)) {
        strvec_push(out, "retry.fallbackChains must be a mapping of role names or model selectors to selector arrays.");
        return out->n - before;
    }
    const char *key;
    json_t *chain;
    json_obj_foreach(cfg, i, key, chain) {
        bool model_key = is_model_key(key);
        const char *kind = model_key ? "model" : "role";
        if (model_key) {
            if (is_wildcard_key(key)) {
                char *prov, *prefix;
                parse_wildcard(key, &prov, &prefix);
                if (!provider_registered(prov)) {
                    char *m = xasprintf("retry.fallbackChains wildcard key references unknown provider: %s", key);
                    strvec_push(out, m);
                    free(m);
                }
                free(prov);
                free(prefix);
            } else {
                mr_selector_t s;
                if (!parse_selector_lookup(key, &s)) {
                    char *m = xasprintf("Invalid model selector key in retry.fallbackChains: %s", key);
                    strvec_push(out, m);
                    free(m);
                } else {
                    if (!catalog_find(s.provider, s.id)) {
                        char *m = xasprintf("retry.fallbackChains key references unknown model: %s", key);
                        strvec_push(out, m);
                        free(m);
                    }
                    mr_selector_free(&s);
                }
            }
        }
        if (!json_is_arr(chain)) {
            char *m = xasprintf("Fallback chain for %s '%s' must be an array of selector strings.", kind, key);
            strvec_push(out, m);
            free(m);
            continue;
        }
        bool kind_role = !model_key && mr_is_kind_role(key);
        json_t *x;
        json_arr_foreach(chain, j, x) {
            if (!json_is_str(x)) {
                char *m = xasprintf("Fallback chain for %s '%s' contains a non-string selector.", kind, key);
                strvec_push(out, m);
                free(m);
                continue;
            }
            const char *sel = json_sv(x);
            if (kind_role) {
                mr_pool_t pool = {0}, acc = {0};
                mr_pool_all(&pool, "all");
                for (size_t k = 0; k < pool.n; k++)
                    if (mr_role_accepts(key, pool.v[k])) mr_pool_push(&acc, pool.v[k]);
                mr_result_t r;
                bool ok = mr_resolve_value(sel, &acc, &r);
                mr_result_clear(&r);
                mr_pool_free(&pool);
                mr_pool_free(&acc);
                if (!ok) {
                    char *m = xasprintf("Fallback chain for role '%s' does not resolve to a compatible model: %s", key, sel);
                    strvec_push(out, m);
                    free(m);
                }
                continue;
            }
            if (is_wildcard_key(sel)) {
                char *prov, *prefix;
                parse_wildcard(sel, &prov, &prefix);
                if (!provider_registered(prov)) {
                    char *m = xasprintf("Fallback chain for %s '%s' references unknown provider: %s", kind, key, sel);
                    strvec_push(out, m);
                    free(m);
                }
                free(prov);
                free(prefix);
                continue;
            }
            mr_selector_t s;
            if (!parse_selector_lookup(sel, &s)) {
                char *m = xasprintf("Invalid fallback selector format in %s '%s': %s", kind, key, sel);
                strvec_push(out, m);
                free(m);
                continue;
            }
            if (!catalog_find(s.provider, s.id)) {
                char *m = xasprintf("Fallback chain for %s '%s' references unknown model: %s", kind, key, sel);
                strvec_push(out, m);
                free(m);
            }
            mr_selector_free(&s);
        }
    }
    return out->n - before;
}

/* selector cooldowns */
static pthread_mutex_t g_sup_mu = PTHREAD_MUTEX_INITIALIZER;
static strvec_t g_sup_keys;
static VEC(int64_t) g_sup_until;

void mr_suppress_selector(const char *selector, int64_t until_ms) {
    pthread_mutex_lock(&g_sup_mu);
    long i = rank_of(&g_sup_keys, selector, false);
    if (i >= 0) {
        if (until_ms > g_sup_until.v[i]) g_sup_until.v[i] = until_ms;
    } else {
        strvec_push(&g_sup_keys, selector);
        vec_push(&g_sup_until, until_ms);
    }
    pthread_mutex_unlock(&g_sup_mu);
}

bool mr_selector_suppressed(const char *selector) {
    pthread_mutex_lock(&g_sup_mu);
    long i = rank_of(&g_sup_keys, selector, false);
    bool s = i >= 0 && g_sup_until.v[i] > now_ms();
    pthread_mutex_unlock(&g_sup_mu);
    return s;
}

bool mr_fallback_admits(bool usage_limit) {
    const char *on = settings_str("retry.fallbackOn", "any");
    if (str_eq(on, "usage-limit")) return usage_limit;
    if (str_eq(on, "except-usage-limit")) return !usage_limit;
    return true;
}

int64_t mr_retry_backoff_ms(int64_t base_ms, int attempt) {
    if (base_ms < 0) base_ms = 0;
    int sh = attempt > 1 ? attempt - 1 : 0;
    int64_t d = sh > 20 ? 8000 : base_ms << sh;
    if (d > 8000) d = 8000;
    double jitter = 1.0 - ((double)(rand() % 1000) / 1000.0) * 0.25;
    return (int64_t)((double)d * jitter);
}

int64_t mr_rate_limit_backoff_ms(const char *msg) {
    if (!msg) return 30 * 60 * 1000;
    char *l = lower_dup(msg);
    int64_t r;
    if (strstr(l, "quota will reset") || strstr(l, "exhausted your capacity")) r = 30 * 60 * 1000;
    else if (strstr(l, "concurren")) r = 5000;
    else if (strstr(l, "capacity") || strstr(l, "overloaded") || strstr(l, "529") || strstr(l, "503"))
        r = 45000 + rand() % 30000;
    else if (strstr(l, "per minute") || strstr(l, "rate limit") || strstr(l, "too many requests")) r = 30000;
    else if (strstr(l, "exhausted") || strstr(l, "quota") || strstr(l, "usage limit") || strstr(l, "out of credits") ||
             strstr(l, "spending limit"))
        r = 30 * 60 * 1000;
    else if (strstr(l, "500") || strstr(l, "internal error") || strstr(l, "internal server error")) r = 20000;
    else r = 5 * 60 * 1000; /* UNKNOWN: noteRetryFallbackCooldown's 5 minutes */
    free(l);
    return r;
}

/* ---------- MRU usage (omp AgentStorage model_usage; JSON here) ---------- */

static pthread_mutex_t g_use_mu = PTHREAD_MUTEX_INITIALIZER;

static char *usage_path(void) { return omc_agent_path("model-usage.json"); }

strvec_t mr_usage_order(void) {
    strvec_t v = {0};
    pthread_mutex_lock(&g_use_mu);
    char *p = usage_path();
    size_t n;
    char *s = fs_read(p, &n);
    free(p);
    json_t *j = s ? json_parse(s, n, NULL) : NULL;
    free(s);
    json_t *x;
    json_arr_foreach(j, i, x) {
        const char *k = json_is_str(x) ? json_sv(x) : json_get_str(x, "model");
        if (k && !strvec_contains(&v, k)) strvec_push(&v, k);
    }
    json_free(j);
    pthread_mutex_unlock(&g_use_mu);
    return v;
}

void mr_usage_record(const char *model_key) {
    if (!model_key || !*model_key) return;
    strvec_t cur = mr_usage_order();
    pthread_mutex_lock(&g_use_mu);
    json_t *arr = json_arr();
    json_t *e = json_obj();
    json_set_str(e, "model", model_key);
    json_set_int(e, "lastUsed", now_ms());
    json_push(arr, e);
    for (size_t i = 0; i < cur.n && json_len(arr) < 200; i++) {
        if (!strcmp(cur.v[i], model_key)) continue;
        json_t *o = json_obj();
        json_set_str(o, "model", cur.v[i]);
        json_push(arr, o);
    }
    char *p = usage_path();
    char *s = json_dump(arr, 0);
    fs_write_atomic(p, s, strlen(s), 0644);
    free(s);
    free(p);
    json_free(arr);
    pthread_mutex_unlock(&g_use_mu);
    strvec_free(&cur);
}
