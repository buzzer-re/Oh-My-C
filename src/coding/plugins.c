#include "plugins.h"

#include "config/settings.h"
#include "core/fs.h"
#include "core/json.h"

#include <stdlib.h>

#define AGENT_PLUGIN_SCHEMA_PREFIX "https://agent-plugins.org/schemas/"
#define AGENT_PLUGIN_MANIFEST_SCHEMA "https://agent-plugins.org/schemas/1.0.0/plugin.schema.json"

static json_t *read_json_file(const char *path) {
    if (!fs_is_file(path)) return NULL;
    size_t n;
    char *s = fs_read_limit(path, 4u << 20, &n, NULL);
    if (!s) return NULL;
    json_t *j = json_parse_str(s);
    free(s);
    return j;
}

/* ---------- provider opt-in (capability/index.ts isUserSourceEnabled) ---------- */

static bool setting_list_has(const char *key, const char *id) {
    json_t *arr = settings_get(key), *x;
    json_arr_foreach(arr, i, x) if (json_is_str(x) && !strcmp(json_sv(x), id)) return true;
    return false;
}

bool provider_user_source_enabled(const char *id) {
    if (*id == '.') id++;
    if (setting_list_has("disabledProviders", id)) return false;
    static const char *const foreign[] = {"cursor", "codex", "claude", "claude-plugins", "gemini", "opencode", "windsurf", "github"};
    bool is_foreign = false;
    for (size_t i = 0; i < ARRAY_LEN(foreign); i++)
        if (!strcmp(foreign[i], id)) is_foreign = true;
    if (!is_foreign) return true;
    if (setting_list_has("enabledProviders", id) || setting_list_has("enabledProviders", "*") ||
        setting_list_has("enabledProviders", "all"))
        return true;
    if (!strcmp(id, "claude-plugins") && setting_list_has("enabledProviders", "claude")) return true;
    if (!strcmp(id, "claude")) {
        const char *e = getenv("CLAUDE_CONFIG_DIR");
        if (e) {
            char *t = str_trim(e);
            bool set = *t != 0;
            free(t);
            if (set) return true;
        }
    }
    return false;
}

/* ---------- registry locations ---------- */

char *claude_config_dir(void) {
    const char *e = getenv("CLAUDE_CONFIG_DIR");
    if (e) {
        char *t = str_trim(e);
        if (*t) {
            char *cwd = get_cwd();
            char *r = path_abs(t, cwd);
            free(cwd);
            free(t);
            return r;
        }
        free(t);
    }
    return path_join(home_dir(), ".claude");
}

static bool is_ceiling(const char *dir) {
    if (!strcmp(dir, home_dir()) || !strcmp(dir, "/tmp") || !strcmp(dir, "/var/tmp")) return true;
    const char *t = getenv("TMPDIR");
    if (t && *t) {
        char *n = path_normalize(t);
        bool eq = !strcmp(n, dir);
        free(n);
        if (eq) return true;
    }
    return false;
}

/* <root>/.omc is the user's own config root, never a project anchor */
static bool is_user_config_root(const char *dir) {
    char *cfg = path_join(dir, ".omc");
    bool r = !strcmp(cfg, omc_config_dir());
    free(cfg);
    return r;
}

/* nearest project registry: <dir>/.omc/plugins/installed_plugins.json for the nearest .omc dir,
 * else the nearest git root; never at/above home or temp roots (resolveActiveProjectRegistryPath) */
static char *project_registry_path(const char *cwd) {
    char *git_anchor = NULL;
    char *dir = path_normalize(cwd);
    while (!is_ceiling(dir)) {
        char *g = path_join(dir, ".git");
        bool has = fs_exists(g);
        free(g);
        if (has && !is_user_config_root(dir)) {
            git_anchor = xstrdup(dir);
            break;
        }
        if (path_is_root(dir)) break;
        char *p = path_dirname(dir);
        free(dir);
        dir = p;
    }
    free(dir);
    dir = path_normalize(cwd);
    while (!is_ceiling(dir)) {
        char *c = path_join(dir, ".omc");
        bool has = fs_is_dir(c);
        free(c);
        if (has && !is_user_config_root(dir)) {
            char *r = path_join3(dir, ".omc", "plugins/installed_plugins.json");
            free(dir);
            free(git_anchor);
            return r;
        }
        if (path_is_root(dir)) break;
        char *p = path_dirname(dir);
        free(dir);
        dir = p;
    }
    free(dir);
    if (!git_anchor) return NULL;
    char *r = path_join3(git_anchor, ".omc", "plugins/installed_plugins.json");
    free(git_anchor);
    return r;
}

/* ---------- roots ---------- */

static void root_free(plugin_root_t *r) {
    free(r->id);
    free(r->plugin);
    free(r->marketplace);
    free(r->path);
}

void plugin_roots_free(plugin_roots_t *r) {
    for (size_t i = 0; i < r->n; i++) root_free(&r->v[i]);
    vec_free(r);
}

static void root_push(plugin_roots_t *out, const char *id, const char *install, bool project, bool from_claude) {
    const char *at = strrchr(id, '@');
    plugin_root_t r = {xstrdup(id), xstrndup(id, (size_t)(at - id)), xstrdup(at + 1), xstrdup(install), project,
                       from_claude};
    vec_push(out, r);
}

static void roots_remove_id(plugin_roots_t *out, const char *id) {
    size_t w = 0;
    for (size_t i = 0; i < out->n; i++) {
        if (!strcmp(out->v[i].id, id)) root_free(&out->v[i]);
        else out->v[w++] = out->v[i];
    }
    out->n = w;
}

/* enabledPlugins overrides: <claude>/settings.json, then <dir>/.claude/settings(.local).json */
static json_t *claude_enabled_plugins(const char *claude_dir, const strvec_t *project_dirs) {
    json_t *map = json_obj();
    strvec_t files = {0};
    strvec_push_own(&files, path_join(claude_dir, "settings.json"));
    for (size_t i = 0; i < project_dirs->n; i++) {
        strvec_push_own(&files, path_join(project_dirs->v[i], ".claude/settings.json"));
        strvec_push_own(&files, path_join(project_dirs->v[i], ".claude/settings.local.json"));
    }
    for (size_t i = 0; i < files.n; i++) {
        json_t *j = read_json_file(files.v[i]);
        json_t *ep = json_get(j, "enabledPlugins");
        const char *k;
        json_t *v;
        json_obj_foreach(ep, n, k, v) if (json_is_bool(v)) json_set_bool(map, k, v->u.b);
        json_free(j);
    }
    strvec_free(&files);
    return map;
}

static json_t *registry_plugins(const char *path) {
    json_t *j = read_json_file(path);
    if (!json_is_obj(j) || !json_is_num(json_get(j, "version")) || !json_is_obj(json_get(j, "plugins"))) {
        json_free(j);
        return NULL;
    }
    return j;
}

void plugin_roots_list(const char *cwd, plugin_roots_t *out) {
    memset(out, 0, sizeof(*out));
    char *claude_dir = claude_config_dir();
    char *omc_registry = path_join3(omc_config_dir(), "plugins", "installed_plugins.json");
    char *project_registry = project_registry_path(cwd);
    if (project_registry && !strcmp(project_registry, omc_registry)) {
        free(project_registry);
        project_registry = NULL;
    }
    /* active project = the project registry's anchor dir, else cwd (canonical) */
    char *anchor = NULL;
    if (project_registry) {
        char *a = path_dirname(project_registry), *b = path_dirname(a), *c = path_dirname(b);
        anchor = c;
        free(a);
        free(b);
    }
    char *active_project = path_real(anchor ? anchor : cwd);
    char *canon_cwd = path_real(cwd);
    strvec_t settings_dirs = {0};
    if (active_project) strvec_push(&settings_dirs, active_project);
    if (canon_cwd && !(active_project && !strcmp(active_project, canon_cwd))) strvec_push(&settings_dirs, canon_cwd);
    json_t *overrides = claude_enabled_plugins(claude_dir, &settings_dirs);

    /* Claude Code registry */
    char *reg_path = path_join(claude_dir, "plugins/installed_plugins.json");
    json_t *reg = registry_plugins(reg_path);
    const char *id;
    json_t *entries;
    json_obj_foreach(json_get(reg, "plugins"), i, id, entries) {
        if (!json_is_arr(entries) || !strrchr(id, '@')) continue;
        json_t *e;
        json_arr_foreach(entries, k, e) {
            const char *install = json_get_str(e, "installPath");
            if (!install || !*install) continue;
            if (json_is_bool(json_get(e, "enabled")) && !json_get_bool(e, "enabled", true)) continue;
            json_t *ov = json_get(overrides, id);
            if (json_is_bool(ov) && !ov->u.b) continue;
            const char *scope = json_get_str(e, "scope");
            bool project = scope && (!strcmp(scope, "local") || !strcmp(scope, "project"));
            if (project && !(json_is_bool(ov) && ov->u.b)) {
                const char *pp = json_get_str(e, "projectPath");
                if (!pp || !active_project) continue;
                char *real = path_real(pp);
                bool same = real && !strcmp(real, active_project);
                free(real);
                if (!same) continue;
            }
            root_push(out, id, install, project, true);
        }
    }
    json_free(reg);
    free(reg_path);

    /* omc user registry: authoritative per plugin id */
    reg = registry_plugins(omc_registry);
    json_obj_foreach(json_get(reg, "plugins"), i, id, entries) {
        if (!json_is_arr(entries) || !json_len(entries) || !strrchr(id, '@')) continue;
        roots_remove_id(out, id);
        json_t *e;
        json_arr_foreach(entries, k, e) {
            const char *install = json_get_str(e, "installPath");
            if (!install || !*install) continue;
            if (json_is_bool(json_get(e, "enabled")) && !json_get_bool(e, "enabled", true)) continue;
            bool dup = false;
            for (size_t m = 0; m < out->n; m++)
                if (!strcmp(out->v[m].id, id) && !strcmp(out->v[m].path, install)) dup = true;
            if (dup) continue;
            const char *scope = json_get_str(e, "scope");
            root_push(out, id, install, scope && (!strcmp(scope, "local") || !strcmp(scope, "project")), false);
        }
    }
    json_free(reg);

    /* project registry: shadows user entries for the same id, listed first */
    if (project_registry) {
        plugin_roots_t proj = {0};
        reg = registry_plugins(project_registry);
        json_obj_foreach(json_get(reg, "plugins"), i, id, entries) {
            if (!json_is_arr(entries) || !strrchr(id, '@')) continue;
            json_t *e;
            json_arr_foreach(entries, k, e) {
                const char *install = json_get_str(e, "installPath");
                if (!install || !*install) continue;
                if (json_is_bool(json_get(e, "enabled")) && !json_get_bool(e, "enabled", true)) continue;
                root_push(&proj, id, install, true, false);
            }
        }
        json_free(reg);
        if (proj.n) {
            for (size_t i = 0; i < proj.n; i++) roots_remove_id(out, proj.v[i].id);
            plugin_roots_t merged = {0};
            for (size_t i = 0; i < proj.n; i++) vec_push(&merged, proj.v[i]);
            for (size_t i = 0; i < out->n; i++) vec_push(&merged, out->v[i]);
            vec_free(&proj);
            vec_free(out);
            *out = merged;
        } else {
            vec_free(&proj);
        }
    }
    json_free(overrides);
    strvec_free(&settings_dirs);
    free(active_project);
    free(canon_cwd);
    free(anchor);
    free(project_registry);
    free(omc_registry);
    free(claude_dir);
}

/* ---------- Agent Plugins standard (agent-plugin-format.ts) ---------- */

static bool valid_plugin_name(const char *n) {
    size_t len = strlen(n);
    if (len < 1 || len > 64) return false;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || ((c == '.' || c == '-') && i > 0 && i + 1 < len);
        if (!ok) return false;
    }
    return !strstr(n, "--") && !strstr(n, "..");
}

typedef enum { ROOT_NONE, ROOT_STANDARD, ROOT_INVALID } root_kind_t;

/* classify a plugin root by its root plugin.json; real_root and name are owned on ROOT_STANDARD */
static root_kind_t classify_root(const char *path, char **real_root, char **name) {
    *real_root = NULL;
    *name = NULL;
    char *real = path_real(path);
    if (!real) return ROOT_NONE;
    char *mp = path_join(real, "plugin.json");
    char *mreal = path_real(mp);
    free(mp);
    if (!mreal) {
        free(real);
        return ROOT_NONE;
    }
    if (!str_starts(mreal, real) || mreal[strlen(real)] != '/') {
        free(mreal);
        free(real);
        return ROOT_INVALID;
    }
    json_t *j = read_json_file(mreal);
    free(mreal);
    root_kind_t kind = ROOT_NONE;
    const char *schema = json_get_str(j, "$schema");
    if (json_is_obj(j) && schema && str_starts(schema, AGENT_PLUGIN_SCHEMA_PREFIX)) {
        kind = ROOT_INVALID;
        const char *nm = json_get_str(j, "name");
        bool ok = !strcmp(schema, AGENT_PLUGIN_MANIFEST_SCHEMA) && nm && valid_plugin_name(nm);
        const char *strs[] = {"version", "description", "homepage", "repository", "license"};
        for (size_t i = 0; ok && i < ARRAY_LEN(strs); i++) {
            json_t *v = json_get(j, strs[i]);
            if (v && !json_is_str(v)) ok = false;
        }
        json_t *kw = json_get(j, "keywords"), *x;
        if (kw) {
            if (!json_is_arr(kw)) ok = false;
            json_arr_foreach(kw, i, x) if (!json_is_str(x)) ok = false;
        }
        json_t *author = json_get(j, "author");
        if (author) {
            if (!json_is_obj(author)) ok = false;
            const char *k;
            json_obj_foreach(author, i, k, x) {
                if ((strcmp(k, "name") && strcmp(k, "email") && strcmp(k, "url")) || !json_is_str(x)) ok = false;
            }
        }
        if (ok) {
            kind = ROOT_STANDARD;
            *name = xstrdup(nm);
            *real_root = real;
            real = NULL;
        }
    }
    json_free(j);
    free(real);
    return kind;
}

/* skills-ref frontmatter validation (closed schema) */
static bool skill_frontmatter_valid(const json_t *fm, const char *dir_name) {
    static const char *const fields[] = {"name", "description", "license", "allowed-tools", "metadata", "compatibility"};
    const char *k;
    json_t *v;
    json_obj_foreach(fm, i, k, v) {
        bool known = false;
        for (size_t f = 0; f < ARRAY_LEN(fields); f++)
            if (!strcmp(fields[f], k)) known = true;
        if (!known) return false;
    }
    const char *nm = json_get_str(fm, "name");
    if (!nm) return false;
    char *name = str_trim(nm);
    bool ok = *name && utf8_strlen(name) <= 64 && name[0] != '-' && name[strlen(name) - 1] != '-' &&
              !strstr(name, "--") && !strcmp(name, dir_name);
    for (const char *p = name; ok && *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') ok = false;
        else if (c < 0x80 && !(c == '-' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) ok = false;
    }
    free(name);
    if (!ok) return false;
    const char *desc = json_get_str(fm, "description");
    if (!desc) return false;
    char *dt = str_trim(desc);
    ok = *dt && utf8_strlen(desc) <= 1024;
    free(dt);
    if (!ok) return false;
    json_t *x = json_get(fm, "license");
    if (x && !json_is_str(x)) return false;
    x = json_get(fm, "compatibility");
    if (x && (!json_is_str(x) || utf8_strlen(json_sv(x)) > 500)) return false;
    x = json_get(fm, "allowed-tools");
    if (x && !json_is_str(x)) return false;
    x = json_get(fm, "metadata");
    if (x) {
        if (!json_is_obj(x)) return false;
        json_obj_foreach(x, i, k, v) if (!json_is_str(v)) return false;
    }
    return true;
}

static bool fm_hidden(const json_t *fm) {
    return json_get_bool(fm, "hide", false) || json_get_bool(fm, "disableModelInvocation", false) ||
           json_get_bool(fm, "disable-model-invocation", false);
}

void plugin_skills_standard(const plugin_roots_t *roots, discovery_t *d) {
    strvec_t seen = {0};
    for (size_t r = 0; r < roots->n; r++) {
        if (strvec_contains(&seen, roots->v[r].path)) continue;
        strvec_push(&seen, roots->v[r].path);
        char *real_root, *pname;
        if (classify_root(roots->v[r].path, &real_root, &pname) != ROOT_STANDARD) continue;
        char *sd = path_join(real_root, "skills");
        char *sd_real = path_real(sd);
        free(sd);
        dirlist_t dl;
        if (sd_real && str_starts(sd_real, real_root) && sd_real[strlen(real_root)] == '/' &&
            fs_list_dir(sd_real, &dl) == 0) {
            for (size_t i = 0; i < dl.n; i++) {
                char *sk = path_join3(sd_real, dl.v[i].name, "SKILL.md");
                char *sk_real = path_real(sk);
                free(sk);
                if (sk_real && str_starts(sk_real, real_root) && sk_real[strlen(real_root)] == '/' && fs_is_file(sk_real)) {
                    size_t n;
                    char *text = fs_read_limit(sk_real, 1u << 20, &n, NULL);
                    const char *body;
                    json_t *fm = text ? frontmatter_parse(text, &body) : NULL;
                    if (fm && skill_frontmatter_valid(fm, dl.v[i].name)) {
                        char *dir = path_dirname(sk_real);
                        discovery_add_skill(d, dl.v[i].name, json_get_str(fm, "description"), sk_real, dir, fm_hidden(fm));
                        free(dir);
                    }
                    json_free(fm);
                    free(text);
                }
                free(sk_real);
            }
            dirlist_free(&dl);
        }
        free(sd_real);
        free(real_root);
        free(pname);
    }
    strvec_free(&seen);
}

/* ---------- legacy Claude Code plugin skills (claude-plugins.ts) ---------- */

static void manifest_paths(const json_t *manifest, const char *key, strvec_t *out) {
    json_t *v = json_get(manifest, key);
    if (json_is_str(v)) {
        char *t = str_trim(json_sv(v));
        if (*t) strvec_push_own(out, t);
        else free(t);
    } else if (json_is_arr(v)) {
        json_t *x;
        json_arr_foreach(v, i, x) {
            if (!json_is_str(x)) continue;
            char *t = str_trim(json_sv(x));
            if (*t) strvec_push_own(out, t);
            else free(t);
        }
    }
}

/* marketplace entry for a plugin whose source is the marketplace root ("./") */
static json_t *marketplace_root_manifest(const plugin_root_t *root) {
    const char *cats[] = {"marketplace.json", ".omp-plugin/marketplace.json", ".claude-plugin/marketplace.json"};
    for (size_t c = 0; c < ARRAY_LEN(cats); c++) {
        char *p = path_join(root->path, cats[c]);
        json_t *j = read_json_file(p);
        free(p);
        json_t *plugins = json_get(j, "plugins"), *e;
        json_t *found = NULL;
        json_arr_foreach(plugins, i, e) {
            if (!found && json_is_obj(e) && !strcmp(json_get_str(e, "name") ? json_get_str(e, "name") : "", root->plugin) &&
                !strcmp(json_get_str(e, "source") ? json_get_str(e, "source") : "", "./")) {
                found = json_obj();
                json_t *sk = json_get(e, "skills");
                if (json_is_str(sk) || json_is_arr(sk)) json_set(found, "skills", json_clone(sk));
            }
        }
        json_free(j);
        if (found) return found;
    }
    return NULL;
}

static void scan_legacy_dir(discovery_t *d, const char *dir) {
    VEC(skill_t) items = {0};
    strvec_t paths = {0};
    char *self = path_join(dir, "SKILL.md");
    if (fs_is_file(self)) strvec_push(&paths, self);
    free(self);
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) == 0) {
        for (size_t i = 0; i < dl.n; i++) {
            if (dl.v[i].name[0] == '.' || !(dl.v[i].is_dir || dl.v[i].is_link)) continue;
            char *p = path_join3(dir, dl.v[i].name, "SKILL.md");
            if (fs_is_file(p)) strvec_push(&paths, p);
            free(p);
        }
        dirlist_free(&dl);
    }
    for (size_t i = 0; i < paths.n; i++) {
        size_t n;
        char *text = fs_read_limit(paths.v[i], 1u << 20, &n, NULL);
        if (!text || !*text) {
            free(text);
            continue;
        }
        const char *body;
        json_t *fm = frontmatter_parse(text, &body);
        json_t *en = json_get(fm, "enabled");
        char *sdir = path_dirname(paths.v[i]);
        const char *raw = json_get_str(fm, "name");
        char *name = raw ? str_trim(raw) : xstrdup("");
        if (!*name) {
            free(name);
            name = xstrdup(path_basename(sdir));
        }
        if (!(json_is_bool(en) && !en->u.b) && !strchr(name, '/') && !strchr(name, '\\')) {
            skill_t s = {name, xstrdup(json_get_str(fm, "description") ? json_get_str(fm, "description") : ""),
                         xstrdup(paths.v[i]), sdir, fm_hidden(fm), false};
            vec_push(&items, s);
        } else {
            free(name);
            free(sdir);
        }
        json_free(fm);
        free(text);
    }
    for (size_t i = 0; i < items.n; i++) {
        skill_t *s = &items.v[i];
        discovery_add_skill(d, s->name, s->description, s->path, s->dir, s->hide);
        free(s->name);
        free(s->description);
        free(s->path);
        free(s->dir);
    }
    vec_free(&items);
    strvec_free(&paths);
}

void plugin_skills_legacy(const plugin_roots_t *roots, discovery_t *d) {
    /* user-scope roots from the Claude registry need the foreign opt-in: allowedRoots() admits
     * them for `claude` or `claude-plugins`, and loadSkills' isSourceEnabled for `claude-plugins` */
    bool user_enabled = provider_user_source_enabled("claude-plugins");
    for (size_t r = 0; r < roots->n; r++) {
        const plugin_root_t *root = &roots->v[r];
        if (!user_enabled && !root->project && root->from_claude) continue;
        char *real_root, *pname;
        root_kind_t kind = classify_root(root->path, &real_root, &pname);
        free(real_root);
        free(pname);
        if (kind != ROOT_NONE) continue; /* standard roots belong to agent-plugins; invalid ones are rejected */
        char *mp = path_join(root->path, ".claude-plugin/plugin.json");
        json_t *manifest = read_json_file(mp);
        free(mp);
        json_t *market = marketplace_root_manifest(root);
        strvec_t configured = {0};
        manifest_paths(manifest, "skills", &configured);
        manifest_paths(market, "skills", &configured);
        char *fallback = path_join(root->path, "skills");
        strvec_t dirs = {0};
        if (!configured.n) {
            strvec_push(&dirs, fallback);
        } else {
            if (!market) strvec_push(&dirs, fallback);
            for (size_t i = 0; i < configured.n; i++) {
                char *abs = path_abs(configured.v[i], root->path);
                bool inside = !strcmp(abs, root->path) || (str_starts(abs, root->path) && abs[strlen(root->path)] == '/');
                if (inside && !strvec_contains(&dirs, abs)) strvec_push(&dirs, abs);
                free(abs);
            }
        }
        for (size_t i = 0; i < dirs.n; i++) scan_legacy_dir(d, dirs.v[i]);
        strvec_free(&dirs);
        strvec_free(&configured);
        free(fallback);
        json_free(manifest);
        json_free(market);
    }
}

void plugin_commands_each(const plugin_roots_t *roots, plugin_cmd_fn fn, void *ud) {
    /* claude-plugins loadSlashCommands: allowedRoots(ctx, "other") (standard roots allowed, invalid ones not),
     * resolvePluginDir(manifestKeys: commands, slash-commands; fallback commands/, includeFallback false) */
    bool user_enabled = provider_user_source_enabled("claude-plugins") || provider_user_source_enabled("claude");
    for (size_t r = 0; r < roots->n; r++) {
        const plugin_root_t *root = &roots->v[r];
        if (!user_enabled && !root->project && root->from_claude) continue;
        char *real_root, *pname;
        root_kind_t kind = classify_root(root->path, &real_root, &pname);
        free(real_root);
        free(pname);
        if (kind == ROOT_INVALID) continue;
        char *mp = path_join(root->path, ".claude-plugin/plugin.json");
        json_t *manifest = read_json_file(mp);
        free(mp);
        strvec_t configured = {0};
        manifest_paths(manifest, "commands", &configured);
        if (!configured.n) manifest_paths(manifest, "slash-commands", &configured);
        strvec_t dirs = {0};
        if (!configured.n) {
            strvec_push_own(&dirs, path_join(root->path, "commands"));
        } else {
            for (size_t i = 0; i < configured.n; i++) {
                char *abs = path_abs(configured.v[i], root->path);
                bool inside = !strcmp(abs, root->path) || (str_starts(abs, root->path) && abs[strlen(root->path)] == '/');
                if (inside && !strvec_contains(&dirs, abs)) strvec_push(&dirs, abs);
                free(abs);
            }
        }
        for (size_t i = 0; i < dirs.n; i++) fn(ud, dirs.v[i], root->plugin, root->project);
        strvec_free(&dirs);
        strvec_free(&configured);
        json_free(manifest);
    }
}
