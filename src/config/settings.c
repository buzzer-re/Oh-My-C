#include <ctype.h>
#include <math.h>
#include <strings.h>
#include "tui/motion.h"
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
    "\"textVerbosity\":\"low\","
    "\"retry\":{\"enabled\":true,\"maxRetries\":10,\"baseDelayMs\":500,\"maxDelayMs\":300000},"
    "\"tools\":{\"approvalMode\":\"yolo\",\"maxTimeout\":0,\"intentTracing\":true,\"outputMaxColumns\":768,"
    "\"artifactSpillThreshold\":50,\"artifactTailBytes\":20,\"artifactTailLines\":500,\"artifactHeadBytes\":20,\"artifactMaxBytes\":16},"
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
static json_t *g_defaults, *g_global, *g_project, *g_overlay, *g_env, *g_runtime, *g_effective;
/* g_project as merged (#resolveProjectRead: disabledProviders may drop the native items) and the native project
 * config.yml's own modelRoles, which stay either way */
static json_t *g_project_resolved, *g_native_roles;
static char *g_global_path;
static char *g_project_dir;
static char *g_cwd;

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

#define SPINNER_INTERVAL_MIN_MS 50
#define MIN_MAX_FPS 1
#define MAX_MAX_FPS 120

static void set_path(json_t *root, const char *path, json_t *val);

/* Number(value.trim() || NaN) restricted to integers: a JS numeric literal such as "80", "8e1" or "0x50" */
static bool parse_whole_number(const char *s, double *out) {
    if (!s) return false;
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (!n) return false;
    char *t = xstrndup(s, n);
    char *end = NULL;
    double v = strtod(t, &end);
    bool ok = end && !*end && isfinite(v) && !strchr(t, 'n') && !strchr(t, 'N');
    free(t);
    if (!ok || v != floor(v)) return false;
    *out = v;
    return true;
}

static void set_err(char **err, char *msg) {
    if (err) *err = msg;
    else free(msg);
}

bool settings_parse_spinner_interval(const char *s, int *out, char **err) {
    double v;
    if (parse_whole_number(s, &v) && (v == TUI_SPINNER_INTERVAL_STATIC || (v >= SPINNER_INTERVAL_MIN_MS && v <= 2147483647.0))) {
        *out = (int)v;
        return true;
    }
    set_err(err, xasprintf("Spinner interval must be %d (static) or a whole number of milliseconds \xe2\x89\xa5 %d, got \"%s\".",
                           TUI_SPINNER_INTERVAL_STATIC, SPINNER_INTERVAL_MIN_MS, s ? s : ""));
    return false;
}

bool settings_parse_max_fps(const char *s, int *out, char **err) {
    double v;
    if (parse_whole_number(s, &v) && v >= MIN_MAX_FPS && v <= MAX_MAX_FPS) {
        *out = (int)v;
        return true;
    }
    set_err(err, xasprintf("Max frames per second must be a whole number from %d to %d, got \"%s\".", MIN_MAX_FPS, MAX_MAX_FPS, s ? s : ""));
    return false;
}

bool settings_parse_motion_mode(const char *s, tui_motion_mode_t *out, char **err) {
    const char *p = s ? s : "";
    while (isspace((unsigned char)*p)) p++;
    size_t n = strlen(p);
    while (n && isspace((unsigned char)p[n - 1])) n--;
    if (n == 4 && !strncasecmp(p, "full", 4)) return *out = TUI_MOTION_FULL, true;
    if (n == 7 && !strncasecmp(p, "reduced", 7)) return *out = TUI_MOTION_REDUCED, true;
    if (n == 4 && !strncasecmp(p, "none", 4)) return *out = TUI_MOTION_NONE, true;
    set_err(err, xasprintf("Motion mode must be one of full, reduced, none, got \"%s\".", s ? s : ""));
    return false;
}

/* the settings with an `env` in their definition that omc reads through this layer: an unparseable value is ignored */
static json_t *env_layer(void) {
    json_t *e = NULL;
    const char *v;
    int n;
    tui_motion_mode_t m;
    if ((v = env_get("PI_SPINNER_INTERVAL")) && settings_parse_spinner_interval(v, &n, NULL)) {
        if (!e) e = json_obj();
        set_path(e, "tui.spinnerInterval", json_num(n));
    }
    if ((v = env_get("PI_MAX_FPS")) && settings_parse_max_fps(v, &n, NULL)) {
        if (!e) e = json_obj();
        set_path(e, "tui.maxFps", json_num(n));
    }
    if ((v = env_get("PI_MOTION")) && settings_parse_motion_mode(v, &m, NULL)) {
        if (!e) e = json_obj();
        set_path(e, "tui.motion", json_str(m == TUI_MOTION_NONE ? "none" : m == TUI_MOTION_REDUCED ? "reduced" : "full"));
    }
    return e;
}

/* the disabledProviders strings the layers resolve to with `project` as the project layer */
static strvec_t disabled_providers(const json_t *project) {
    json_t *m = json_obj();
    const json_t *layers[] = {g_global, project, g_overlay, g_env, g_runtime};
    for (size_t i = 0; i < ARRAY_LEN(layers); i++)
        if (layers[i]) json_merge(m, layers[i]);
    strvec_t out = {0};
    json_t *x;
    json_arr_foreach(json_get(m, "disabledProviders"), i, x) if (json_is_str(x) && !strvec_contains(&out, json_sv(x)))
        strvec_push(&out, json_sv(x));
    json_free(m);
    return out;
}

static bool same_set(const strvec_t *a, const strvec_t *b) {
    if (a->n != b->n) return false;
    for (size_t i = 0; i < a->n; i++)
        if (!strvec_contains(b, a->v[i])) return false;
    return true;
}

/* #resolveProjectRead: `disabledProviders`, from any layer (the project's own native config included), drops the
 * native provider's project items; the native config.yml's modelRoles stay. A project config that disables itself
 * keeps only that switch, so removing its other settings cannot undo the decision. Owned. */
static json_t *resolve_project(void) {
    if (!g_project || !g_project_dir) return g_project ? json_clone(g_project) : NULL;
    strvec_t disabled = disabled_providers(g_project);
    json_t *out;
    if (!strvec_contains(&disabled, "native")) {
        out = json_clone(g_project);
    } else {
        out = json_obj();
        if (g_native_roles) json_set(out, "modelRoles", json_clone(g_native_roles));
        const json_t *sw = json_get(g_project, "disabledProviders");
        if (sw) {
            strvec_t remaining = disabled_providers(out);
            if (!same_set(&disabled, &remaining)) json_set(out, "disabledProviders", json_clone(sw));
            strvec_free(&remaining);
        }
    }
    strvec_free(&disabled);
    return out;
}

static void rebuild(void) {
    json_free(g_effective);
    g_effective = json_clone(g_defaults);
    json_free(g_env);
    g_env = env_layer();
    json_free(g_project_resolved);
    g_project_resolved = resolve_project();
    /* projectLayerForMerge: a cleared (null) project model role falls back to the global one */
    json_t *project = g_project_resolved ? json_clone(g_project_resolved) : NULL;
    json_t *proles = json_get_obj(project, "modelRoles");
    for (size_t i = 0; proles && i < proles->u.obj.n;) {
        json_t *v = proles->u.obj.vals[i];
        if (!json_is_str(v) && !json_is_arr(v)) json_del(proles, proles->u.obj.keys[i]);
        else i++;
    }
    /* precedence: runtime override (flags, session-only writes), the definition's environment variable, overlay, project, global */
    json_t *layers[] = {g_global, project, g_overlay, g_env, g_runtime};
    for (size_t i = 0; i < ARRAY_LEN(layers); i++)
        if (layers[i]) json_merge(g_effective, layers[i]);
    json_free(project);
}

/* the project layer (<cwd>/.omc or <cwd>/.omp config files); caller holds g_mu */
static void load_project(const char *cwd) {
    json_free(g_project);
    g_project = json_obj();
    json_free(g_native_roles);
    g_native_roles = NULL;
    free(g_project_dir);
    g_project_dir = NULL;
    char *c = cwd ? xstrdup(cwd) : get_cwd();
    free(g_cwd);
    g_cwd = xstrdup(c);
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
                    if (str_eq(pf[k], "config.yml") && json_get(j, "modelRoles"))
                        g_native_roles = json_clone(json_get(j, "modelRoles"));
                    json_merge(g_project, j);
                    json_free(j);
                }
                free(p);
            }
        }
        free(d);
    }
    free(c);
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
    load_project(cwd);
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

static void ensure(void);

void settings_reload_for_cwd(const char *cwd) {
    ensure();
    pthread_mutex_lock(&g_mu);
    load_project(cwd);
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

bool settings_is_configured(const char *path) {
    ensure();
    pthread_mutex_lock(&g_mu);
    json_t *layers[] = {g_global, g_project_resolved, g_overlay, g_runtime};
    bool r = false;
    for (size_t i = 0; i < ARRAY_LEN(layers) && !r; i++) r = layers[i] && json_path(layers[i], path) != NULL;
    pthread_mutex_unlock(&g_mu);
    return r;
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

/* change listeners (omp Setting.listen): called after a write, outside the lock, on the writing thread */
#define MAX_LISTENERS 8
static struct {
    void (*fn)(void *ud);
    void *ud;
} g_listeners[MAX_LISTENERS];
static size_t g_nlisteners;

void settings_on_change(void (*fn)(void *ud), void *ud) {
    pthread_mutex_lock(&g_mu);
    if (g_nlisteners < MAX_LISTENERS) g_listeners[g_nlisteners++] = (typeof(g_listeners[0])){fn, ud};
    pthread_mutex_unlock(&g_mu);
}

void settings_off_change(void (*fn)(void *ud), void *ud) {
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_nlisteners; i++) {
        if (g_listeners[i].fn != fn || g_listeners[i].ud != ud) continue;
        memmove(&g_listeners[i], &g_listeners[i + 1], (g_nlisteners - i - 1) * sizeof g_listeners[0]);
        g_nlisteners--;
        break;
    }
    pthread_mutex_unlock(&g_mu);
}

static void notify_change(void) {
    typeof(g_listeners[0]) copy[MAX_LISTENERS];
    pthread_mutex_lock(&g_mu);
    size_t n = g_nlisteners;
    memcpy(copy, g_listeners, n * sizeof copy[0]);
    pthread_mutex_unlock(&g_mu);
    for (size_t i = 0; i < n; i++) copy[i].fn(copy[i].ud);
}

void settings_set_runtime(const char *path, json_t *val) {
    ensure();
    pthread_mutex_lock(&g_mu);
    set_path(g_runtime, path, val);
    rebuild();
    pthread_mutex_unlock(&g_mu);
    notify_change();
}

void settings_set_runtime_quiet(const char *path, json_t *val) {
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
    notify_change();
}

/* Bun's YAML.stringify (omp stringifyYamlConfig) ends the document without a trailing newline */
static char *yaml_text(const json_t *j) {
    char *text = yaml_dump(j);
    size_t n = strlen(text);
    while (n && text[n - 1] == '\n') text[--n] = 0;
    return text;
}

static int save_global(void) {
    char *text;
    if (yaml_dump && !str_ends(g_global_path, ".json")) text = yaml_text(g_global);
    else text = json_dump(g_global, 2);
    int rc = fs_write_atomic(g_global_path, text, strlen(text), 0600);
    free(text);
    return rc;
}

int settings_set_global(const char *path, json_t *val) {
    ensure();
    pthread_mutex_lock(&g_mu);
    /* #globalWriteIsNoop: re-setting the persisted value stages no config.yml rewrite */
    json_t *cur = json_path(g_global, path);
    if (cur && json_equal(cur, val)) {
        json_free(val);
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    set_path(g_global, path, val);
    rebuild();
    int rc = save_global();
    pthread_mutex_unlock(&g_mu);
    notify_change();
    return rc;
}

int settings_unset_global(const char *path) {
    ensure();
    pthread_mutex_lock(&g_mu);
    del_path(g_global, path);
    rebuild();
    int rc = save_global();
    pthread_mutex_unlock(&g_mu);
    notify_change();
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

/* the json object at a dotted parent path inside `root`, created on demand */
static json_t *parent_obj(json_t *root, const char *parent, bool create) {
    strvec_t parts = str_split(parent, ".");
    json_t *cur = root;
    for (size_t i = 0; cur && i < parts.n; i++) {
        json_t *next = json_get(cur, parts.v[i]);
        if (!json_is_obj(next)) {
            if (!create) {
                cur = NULL;
                break;
            }
            next = json_obj();
            json_set(cur, parts.v[i], next);
        }
        cur = next;
    }
    strvec_free(&parts);
    return cur;
}

int settings_set_global_entry(const char *parent, const char *key, json_t *val) {
    ensure();
    pthread_mutex_lock(&g_mu);
    if (val) {
        json_t *o = parent_obj(g_global, parent, false), *cur = o ? json_get(o, key) : NULL;
        if (cur && json_equal(cur, val)) { /* no-op write: no rewrite */
            json_free(val);
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
        json_set(parent_obj(g_global, parent, true), key, val);
    } else {
        json_t *o = parent_obj(g_global, parent, false);
        if (!o || !json_get(o, key)) {
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
        json_del(o, key);
    }
    rebuild();
    int rc = save_global();
    pthread_mutex_unlock(&g_mu);
    return rc;
}

json_t *settings_project(void) {
    ensure();
    return g_project_resolved ? g_project_resolved : g_project;
}

char *settings_project_config_path(void) {
    ensure();
    if (g_project_dir) return path_join(g_project_dir, "config.yml");
    char *d = path_join(g_cwd ? g_cwd : ".", ".omc");
    char *p = path_join(d, "config.yml");
    free(d);
    return p;
}

/* omp #saveProjectNow: re-read the project config.yml, set this one entry, write it back */
int settings_set_project_entry(const char *parent, const char *key, json_t *val) {
    ensure();
    pthread_mutex_lock(&g_mu);
    char *path = settings_project_config_path();
    json_t *file = load_file(path);
    if (!file) file = json_obj();
    if (val) {
        json_set(parent_obj(file, parent, true), key, json_clone(val));
        json_set(parent_obj(g_project, parent, true), key, val);
    } else {
        json_t *o = parent_obj(file, parent, false);
        if (o) json_del(o, key);
        o = parent_obj(g_project, parent, false);
        if (o) json_del(o, key);
    }
    json_free(g_native_roles);
    g_native_roles = json_get(file, "modelRoles") ? json_clone(json_get(file, "modelRoles")) : NULL;
    char *dir = path_dirname(path);
    fs_mkdir_p(dir, 0755);
    if (!g_project_dir) g_project_dir = xstrdup(dir);
    free(dir);
    char *text = yaml_dump ? yaml_text(file) : json_dump(file, 2);
    int rc = fs_write_atomic(path, text, strlen(text), 0644);
    free(text);
    free(path);
    json_free(file);
    rebuild();
    pthread_mutex_unlock(&g_mu);
    return rc;
}

/* omp Settings.getProvenance: the highest layer holding a non-null value at `path` */
const char *settings_provenance(const char *path) {
    ensure();
    pthread_mutex_lock(&g_mu);
    const char *r = "default";
    const json_t *layers[] = {g_runtime, g_env, g_overlay, g_project_resolved, g_global};
    static const char *const names[] = {"runtime", "env", "overlay", "project", "global"};
    for (size_t i = 0; i < ARRAY_LEN(layers); i++) {
        const json_t *v = layers[i] ? json_path(layers[i], path) : NULL;
        if (v && !json_is_null(v)) {
            r = names[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

bool settings_configured(const char *path) { return strcmp(settings_provenance(path), "default") != 0; }
