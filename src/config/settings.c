#include "settings.h"

#include "core/fs.h"

#include <pthread.h>

/* Built-in defaults mirroring omp's registered setting defaults (subset that omc reads). */
static const char *DEFAULTS_JSON =
    "{"
    "\"defaultThinkingLevel\":\"high\","
    "\"hideThinkingBlock\":false,"
    "\"steeringMode\":\"one-at-a-time\","
    "\"followUpMode\":\"one-at-a-time\","
    "\"interruptMode\":\"immediate\","
    "\"personality\":\"default\","
    "\"includeModelInPrompt\":true,"
    "\"retry\":{\"enabled\":true,\"maxRetries\":10,\"baseDelayMs\":2000,\"maxDelayMs\":60000},"
    "\"tools\":{\"approvalMode\":\"yolo\",\"maxTimeout\":0,\"intentTracing\":true,\"outputMaxColumns\":768,"
    "\"artifactSpillThreshold\":50,\"artifactTailBytes\":20,\"artifactTailLines\":500,\"artifactHeadBytes\":20},"
    "\"bash\":{\"enabled\":true},"
    "\"edit\":{\"mode\":\"hashline\",\"fuzzyMatch\":true,\"fuzzyThreshold\":0.95},"
    "\"read\":{\"defaultLimit\":300},"
    "\"readLineNumbers\":false,"
    "\"compaction\":{\"enabled\":true,\"thresholdPercent\":-1,\"thresholdTokens\":-1,\"reserveTokens\":16384,\"keepRecentTokens\":20000,"
    "\"autoContinue\":true},"
    "\"theme\":{\"dark\":\"titanium\",\"light\":\"light\"},"
    "\"todo\":{\"enabled\":true},"
    "\"task\":{\"enabled\":true,\"maxConcurrency\":32,\"maxRecursionDepth\":2},"
    "\"web\":{\"enabled\":true},"
    "\"lsp\":{\"enabled\":true,\"diagnosticsOnWrite\":true,\"diagnosticsOnEdit\":false},"
    "\"startup\":{\"quiet\":false},"
    "\"ask\":{\"timeout\":0},"
    "\"providers\":{\"cacheRetention\":\"auto\",\"webSearchTimeoutSeconds\":60,\"streamIdleTimeoutSeconds\":-1},"
    "\"shellCommandPrefix\":\"\","
    "\"contextFiles\":{\"extra\":[]}"
    "}";

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *g_defaults, *g_global, *g_project, *g_overlay, *g_runtime, *g_effective;
static char *g_global_path;
static char *g_project_dir;

extern json_t *yaml_parse_file_json(const char *path, char **err) __attribute__((weak));
extern char *yaml_dump(const json_t *j) __attribute__((weak));

static json_t *load_file(const char *path) {
    if (!fs_is_file(path)) return NULL;
    json_t *j = NULL;
    if (str_ends(path, ".json") || str_ends(path, ".jsonc")) {
        size_t n;
        char *s = fs_read(path, &n);
        if (s) j = json_parse(s, n, NULL);
        free(s);
    } else if (yaml_parse_file_json) {
        char *err = NULL;
        j = yaml_parse_file_json(path, &err);
        if (err) {
            fprintf(stderr, "omc: warning: %s: %s\n", path, err);
            free(err);
        }
    }
    if (j && !json_is_obj(j)) {
        json_free(j);
        j = NULL;
    }
    return j;
}

static void rebuild(void) {
    json_free(g_effective);
    g_effective = json_clone(g_defaults);
    json_t *layers[] = {g_global, g_project, g_overlay, g_runtime};
    for (size_t i = 0; i < ARRAY_LEN(layers); i++)
        if (layers[i]) json_merge(g_effective, layers[i]);
}

void settings_init(const char *cwd, const strvec_t *overlay_files) {
    pthread_mutex_lock(&g_mu);
    if (!g_defaults) g_defaults = json_parse_str(DEFAULTS_JSON);
    const char *names[] = {"config.yml", "config.yaml", "settings.json"};
    free(g_global_path);
    g_global_path = NULL;
    json_free(g_global);
    g_global = NULL;
    for (size_t i = 0; i < ARRAY_LEN(names) && !g_global; i++) {
        char *p = omc_agent_path(names[i]);
        g_global = load_file(p);
        if (g_global || i == 0) {
            free(g_global_path);
            g_global_path = xstrdup(p);
        }
        free(p);
    }
    if (!g_global) g_global = json_obj();
    if (str_ends(g_global_path, "settings.json")) {
        free(g_global_path);
        g_global_path = omc_agent_path("config.yml");
    }
    json_free(g_project);
    g_project = json_obj();
    free(g_project_dir);
    g_project_dir = NULL;
    char *c = cwd ? xstrdup(cwd) : get_cwd();
    const char *pdirs[] = {".omc", ".omp"};
    for (size_t i = 0; i < ARRAY_LEN(pdirs) && !g_project_dir; i++) {
        char *d = path_join(c, pdirs[i]);
        if (fs_is_dir(d)) {
            g_project_dir = xstrdup(d);
            const char *pf[] = {"settings.json", "config.yml", "config.yaml"};
            for (size_t k = 0; k < ARRAY_LEN(pf); k++) {
                char *p = path_join(d, pf[k]);
                json_t *j = load_file(p);
                if (j) {
                    json_merge(g_project, j);
                    json_free(j);
                }
                free(p);
            }
        }
        free(d);
    }
    free(c);
    json_free(g_overlay);
    g_overlay = json_obj();
    if (overlay_files) {
        for (size_t i = 0; i < overlay_files->n; i++) {
            json_t *j = load_file(overlay_files->v[i]);
            if (j) {
                json_merge(g_overlay, j);
                json_free(j);
            } else {
                fprintf(stderr, "omc: warning: cannot load config %s\n", overlay_files->v[i]);
            }
        }
    }
    if (!g_runtime) g_runtime = json_obj();
    rebuild();
    pthread_mutex_unlock(&g_mu);
}

static void ensure(void) {
    if (!g_effective) settings_init(NULL, NULL);
}

json_t *settings_get(const char *path) {
    ensure();
    return json_path(g_effective, path);
}

const char *settings_str(const char *path, const char *def) {
    json_t *v = settings_get(path);
    return json_is_str(v) ? json_sv(v) : def;
}

int64_t settings_int(const char *path, int64_t def) {
    json_t *v = settings_get(path);
    if (json_is_num(v)) return v->is_int ? v->u.num.i : (int64_t)v->u.num.d;
    if (json_is_str(v)) return strtoll(json_sv(v), NULL, 10);
    return def;
}

double settings_num(const char *path, double def) {
    json_t *v = settings_get(path);
    if (json_is_num(v)) return v->u.num.d;
    if (json_is_str(v)) return strtod(json_sv(v), NULL);
    return def;
}

bool settings_bool(const char *path, bool def) {
    json_t *v = settings_get(path);
    if (json_is_bool(v)) return v->u.b;
    if (json_is_str(v)) return str_ieq(json_sv(v), "true") || !strcmp(json_sv(v), "1");
    return def;
}

/* set by dotted path creating intermediate objects */
static void set_path(json_t *root, const char *path, json_t *val) {
    strvec_t parts = str_split(path, ".");
    json_t *cur = root;
    for (size_t i = 0; i + 1 < parts.n; i++) {
        json_t *next = json_get(cur, parts.v[i]);
        if (!json_is_obj(next)) {
            next = json_obj();
            json_set(cur, parts.v[i], next);
        }
        cur = next;
    }
    json_set(cur, parts.v[parts.n - 1], val);
    strvec_free(&parts);
}

static void del_path(json_t *root, const char *path) {
    strvec_t parts = str_split(path, ".");
    json_t *cur = root;
    for (size_t i = 0; cur && i + 1 < parts.n; i++) cur = json_get_obj(cur, parts.v[i]);
    if (cur) json_del(cur, parts.v[parts.n - 1]);
    strvec_free(&parts);
}

void settings_set_runtime(const char *path, json_t *val) {
    ensure();
    pthread_mutex_lock(&g_mu);
    set_path(g_runtime, path, val);
    rebuild();
    pthread_mutex_unlock(&g_mu);
}

void settings_unset_runtime(const char *path) {
    ensure();
    pthread_mutex_lock(&g_mu);
    del_path(g_runtime, path);
    rebuild();
    pthread_mutex_unlock(&g_mu);
}

static int save_global(void) {
    char *text;
    if (yaml_dump && !str_ends(g_global_path, ".json")) text = yaml_dump(g_global);
    else text = json_dump(g_global, 2);
    int rc = fs_write_atomic(g_global_path, text, strlen(text), 0600);
    free(text);
    return rc;
}

int settings_set_global(const char *path, json_t *val) {
    ensure();
    pthread_mutex_lock(&g_mu);
    set_path(g_global, path, val);
    rebuild();
    int rc = save_global();
    pthread_mutex_unlock(&g_mu);
    return rc;
}

int settings_unset_global(const char *path) {
    ensure();
    pthread_mutex_lock(&g_mu);
    del_path(g_global, path);
    rebuild();
    int rc = save_global();
    pthread_mutex_unlock(&g_mu);
    return rc;
}

json_t *settings_effective(void) {
    ensure();
    return g_effective;
}

json_t *settings_global(void) {
    ensure();
    return g_global;
}

const char *settings_global_path(void) {
    ensure();
    return g_global_path;
}

const char *settings_project_dir(void) {
    ensure();
    return g_project_dir;
}
