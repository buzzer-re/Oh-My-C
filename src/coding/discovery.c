#include "discovery.h"
#include "agent/agent.h"

#include "config/settings.h"
#include "core/fs.h"
#include "prompt/template.h"
#include "text/glob.h"
#include "text/yaml.h"
#include "core/embed.h"
#include "coding/plugins.h"
#include "coding/memory.h"

#include <unistd.h>

#include <ctype.h>
#include <strings.h>
#include <sys/utsname.h>

/* ---------- frontmatter (flat YAML subset; nested values kept as strings) ---------- */

static json_t *scalar(const char *v) {
    char *t = str_trim(v);
    size_t n = strlen(t);
    json_t *r;
    if (n >= 2 && ((t[0] == '"' && t[n - 1] == '"') || (t[0] == '\'' && t[n - 1] == '\''))) {
        t[n - 1] = 0;
        r = json_str(t + 1);
    } else if (!strcmp(t, "true") || !strcmp(t, "false")) {
        r = json_bool(t[0] == 't');
    } else if (n && t[0] == '[' && t[n - 1] == ']') {
        r = json_arr();
        t[n - 1] = 0;
        strvec_t parts = str_split(t + 1, ",");
        for (size_t i = 0; i < parts.n; i++) {
            char *x = str_trim(parts.v[i]);
            size_t xl = strlen(x);
            if (xl >= 2 && (x[0] == '"' || x[0] == '\'') && x[xl - 1] == x[0]) {
                x[xl - 1] = 0;
                json_push_str(r, x + 1);
            } else if (xl) {
                json_push_str(r, x);
            }
            free(x);
        }
        strvec_free(&parts);
    } else {
        r = json_str(t);
    }
    free(t);
    return r;
}

static json_t *frontmatter_parse_flat(const char *text, const char **body);

json_t *frontmatter_parse(const char *text, const char **body) {
    *body = text;
    if (strncmp(text, "---", 3) != 0) return json_obj();
    char *err = NULL;
    const char *b2 = NULL;
    json_t *j = yaml_frontmatter(text, strlen(text), &b2, &err);
    if (j && json_is_obj(j) && !err) {
        if (b2) *body = b2;
        return j;
    }
    json_free(j);
    free(err);
    return frontmatter_parse_flat(text, body);
}

static json_t *frontmatter_parse_flat(const char *text, const char **body) {
    json_t *o = json_obj();
    *body = text;
    if (strncmp(text, "---", 3) != 0 || (text[3] != '\n' && text[3] != '\r')) return o;
    const char *start = strchr(text, '\n') + 1;
    const char *end = start;
    for (;;) {
        if (strncmp(end, "---", 3) == 0 && (end[3] == '\n' || end[3] == '\r' || end[3] == 0)) break;
        const char *nl = strchr(end, '\n');
        if (!nl) return o;
        end = nl + 1;
    }
    char *fm = xstrndup(start, (size_t)(end - start));
    const char *after = strchr(end, '\n');
    *body = after ? after + 1 : end + strlen(end);
    strvec_t lines = str_split_lines(fm, strlen(fm));
    char *cur_key = NULL;
    for (size_t i = 0; i < lines.n; i++) {
        char *l = lines.v[i];
        if (!*l || l[0] == '#') continue;
        if ((l[0] == ' ' || l[0] == '\t') && cur_key) {
            char *t = str_trim(l);
            if (t[0] == '-' && (t[1] == ' ' || !t[1])) {
                json_t *arr = json_get(o, cur_key);
                if (!json_is_arr(arr)) {
                    arr = json_arr();
                    json_set(o, cur_key, arr);
                }
                json_push(arr, scalar(t + 1));
            } else {
                /* continuation of folded/plain value */
                json_t *prev = json_get(o, cur_key);
                char *joined = xasprintf("%s%s%s", json_sv(prev) ? json_sv(prev) : "", json_sv(prev) && *json_sv(prev) ? " " : "", t);
                json_set_str(o, cur_key, joined);
                free(joined);
            }
            free(t);
            continue;
        }
        char *colon = strchr(l, ':');
        if (!colon) continue;
        *colon = 0;
        char *key = str_trim(l);
        const char *val = colon + 1;
        while (*val == ' ') val++;
        free(cur_key);
        cur_key = xstrdup(key);
        if (!*val || !strcmp(val, "|") || !strcmp(val, ">") || !strcmp(val, "|-") || !strcmp(val, ">-"))
            json_set_str(o, key, "");
        else json_set(o, key, scalar(val));
        free(key);
    }
    free(cur_key);
    strvec_free(&lines);
    free(fm);
    return o;
}

/* ---------- helpers ---------- */

char *find_repo_root(const char *cwd) {
    char *cur = xstrdup(cwd);
    for (;;) {
        char *g = path_join(cur, ".git");
        bool found = fs_exists(g);
        free(g);
        if (found) return cur;
        if (strcmp(cur, "/") == 0) break;
        char *p = path_dirname(cur);
        free(cur);
        cur = p;
    }
    free(cur);
    return NULL;
}

strvec_t discovery_project_ancestors(const char *cwd) {
    strvec_t out = {0};
    const char *home = home_dir();
    char *root = find_repo_root(cwd);
    char *cur = xstrdup(cwd);
    for (;;) {
        if (strcmp(cur, home) != 0) strvec_push(&out, cur);
        if (!strcmp(cur, root ? root : home)) break;
        char *parent = path_dirname(cur);
        bool top = !strcmp(parent, cur);
        free(cur);
        cur = parent;
        if (top) break;
    }
    free(cur);
    free(root);
    return out;
}

static char *read_nonempty(const char *path) {
    if (!fs_is_file(path)) return NULL;
    size_t n;
    char *s = fs_read_limit(path, 1u << 20, &n, NULL);
    if (!s) return NULL;
    char *t = str_trim(s);
    bool empty = !*t;
    free(t);
    if (empty) {
        free(s);
        return NULL;
    }
    return s;
}

static bool dir_nonempty(const char *d) {
    dirlist_t dl;
    if (fs_list_dir(d, &dl) != 0) return false;
    bool r = dl.n > 0;
    dirlist_free(&dl);
    return r;
}

static void add_file(discovery_t *d, const char *path, int depth) {
    for (size_t i = 0; i < d->files.n; i++)
        if (!strcmp(d->files.v[i].path, path)) return;
    char *c = read_nonempty(path);
    if (!c) return;
    ctx_file_t f = {xstrdup(path), c, depth};
    vec_push(&d->files, f);
}

/* chain of directories from root (or home boundary) down to cwd */
static strvec_t dir_chain(const char *cwd) {
    strvec_t chain = {0};
    char *root = find_repo_root(cwd);
    const char *home = home_dir();
    char *cur = xstrdup(cwd);
    strvec_t rev = {0};
    for (;;) {
        strvec_push(&rev, cur);
        if (root && !strcmp(cur, root)) break;
        if (!root && !strcmp(cur, home)) break;
        if (!strcmp(cur, "/")) break;
        char *p = path_dirname(cur);
        free(cur);
        cur = p;
        if (!root && !str_starts(cwd, home)) {
            /* outside home without a repo: only cwd */
            break;
        }
    }
    free(cur);
    free(root);
    for (size_t i = rev.n; i > 0; i--) strvec_push(&chain, rev.v[i - 1]);
    strvec_free(&rev);
    return chain;
}

/* ---------- skills ---------- */

bool discovery_add_skill(discovery_t *d, const char *name, const char *description, const char *path, const char *dir,
                         bool hide) {
    for (size_t k = 0; k < d->skills.n; k++)
        if (!strcmp(d->skills.v[k].name, name)) return false;
    skill_t s = {xstrdup(name), xstrdup(description ? description : ""), xstrdup(path), xstrdup(dir), hide, false};
    vec_push(&d->skills, s);
    return true;
}

static void scan_skills_dir_opt(discovery_t *d, const char *dir, bool require_description) {
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return;
    for (size_t i = 0; i < dl.n; i++) {
        if (!dl.v[i].is_dir || dl.v[i].name[0] == '.') continue;
        char *sd = path_join(dir, dl.v[i].name);
        char *sf = path_join(sd, "SKILL.md");
        char *text = read_nonempty(sf);
        if (text) {
            const char *body;
            json_t *fm = frontmatter_parse(text, &body);
            json_t *en = json_get(fm, "enabled");
            const char *raw = json_get_str(fm, "name");
            char *name = raw ? str_trim(raw) : xstrdup("");
            if (!*name) {
                free(name);
                name = xstrdup(dl.v[i].name);
            }
            bool has_desc = json_get_str(fm, "description") && *json_get_str(fm, "description");
            if (!(json_is_bool(en) && !en->u.b) && !(require_description && !has_desc) && !strchr(name, '/') &&
                !strchr(name, '\\'))
                discovery_add_skill(d, name, json_get_str(fm, "description"), sf, sd,
                                    json_get_bool(fm, "hide", false) || json_get_bool(fm, "disableModelInvocation", false) ||
                                        json_get_bool(fm, "disable-model-invocation", false));
            free(name);
            json_free(fm);
            free(text);
        }
        free(sf);
        free(sd);
    }
    dirlist_free(&dl);
}

static void scan_skills_dir(discovery_t *d, const char *dir) { scan_skills_dir_opt(d, dir, false); }

static bool managed_name_ok(const char *n) {
    size_t len = strlen(n);
    if (len < 1 || len > 64) return false;
    for (size_t i = 0; i < len; i++)
        if (!((n[i] >= 'a' && n[i] <= 'z') || (n[i] >= '0' && n[i] <= '9') || (i > 0 && n[i] == '-'))) return false;
    return true;
}

/* auto-learn managed skills: lowest priority, valid names only, descriptions re-sanitized on read
 * (discovery/builtin.ts loadManagedSkills + extensibility/skills.ts managed pass) */
static void scan_managed_skills(discovery_t *d) {
    char *dir = omc_agent_path("managed-skills");
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) == 0) {
        for (size_t i = 0; i < dl.n; i++) {
            if (!dl.v[i].is_dir || dl.v[i].name[0] == '.') continue;
            char *sd = path_join(dir, dl.v[i].name);
            char *sf = path_join(sd, "SKILL.md");
            char *text = read_nonempty(sf);
            if (text) {
                const char *body;
                json_t *fm = frontmatter_parse(text, &body);
                const char *raw = json_get_str(fm, "name");
                char *name = raw && *raw ? str_trim(raw) : xstrdup(dl.v[i].name);
                const char *desc = json_get_str(fm, "description");
                if (desc && *desc && managed_name_ok(name)) {
                    char *clean = memory_neutralize(desc);
                    if (discovery_add_skill(d, name, clean, sf, sd,
                                            json_get_bool(fm, "hide", false) || json_get_bool(fm, "disableModelInvocation", false)))
                        d->skills.v[d->skills.n - 1].managed = true;
                    free(clean);
                }
                free(name);
                json_free(fm);
                free(text);
            }
            free(sf);
            free(sd);
        }
        dirlist_free(&dl);
    }
    free(dir);
}

/* omp compareSkillOrder: case-insensitive name, then exact name, then path */
static int cmp_skill(const void *a, const void *b) {
    const skill_t *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    if (c) return c;
    c = strcmp(x->name, y->name);
    if (c) return c;
    return strcmp(x->path, y->path);
}

/* ---------- rules ---------- */

static rule_t *add_rule_text(discovery_t *d, char *text, const char *path, const char *default_name);

static void add_rule(discovery_t *d, const char *path, const char *default_name) {
    char *text = read_nonempty(path);
    if (text) add_rule_text(d, text, path, default_name);
}

/* takes ownership of text; returns the added rule or NULL (duplicate) */
static rule_t *add_rule_text(discovery_t *d, char *text, const char *path, const char *default_name) {
    const char *body;
    json_t *fm = frontmatter_parse(text, &body);
    rule_t r = {0};
    json_t *en = json_get(fm, "enabled");
    if (json_is_bool(en) && !en->u.b) { /* frontmatter-disabled rule */
        json_free(fm);
        free(text);
        return NULL;
    }
    r.name = xstrdup(default_name); /* omp names rules after their file, never frontmatter `name` */
    for (size_t k = 0; k < d->rules.n; k++)
        if (!strcmp(d->rules.v[k].name, r.name)) {
            free(r.name);
            json_free(fm);
            free(text);
            return NULL;
        }
    r.description = xstrdup(json_get_str(fm, "description") ? json_get_str(fm, "description") : "");
    r.path = xstrdup(path);
    /* omp parseFrontmatter: the body after frontmatter is trimmed; a file without frontmatter is kept
     * verbatim (CRLF normalized) */
    if (body != text) r.content = str_trim(body);
    else r.content = str_replace(text, "\r\n", "\n");
    r.always = json_get_bool(fm, "alwaysApply", false);
    json_t *g = json_get(fm, "globs");
    if (!g) g = json_get(fm, "applyTo");
    if (json_is_arr(g)) {
        json_t *x;
        json_arr_foreach(g, i, x) if (json_is_str(x)) strvec_push(&r.globs, json_sv(x));
    } else if (json_is_str(g) && *json_sv(g) && !str_ends(path, ".instructions.md")) {
        strvec_push(&r.globs, json_sv(g)); /* a string glob is one pattern (omp buildRule) */
    } else if (json_is_str(g) && *json_sv(g)) {
        strvec_t parts = str_split(json_sv(g), ",");
        for (size_t i = 0; i < parts.n; i++) {
            char *t = str_trim(parts.v[i]);
            if (*t) strvec_push_own(&r.globs, t);
            else free(t);
        }
        strvec_free(&parts);
    }
    for (size_t i = 0; str_ends(path, ".instructions.md") && i < r.globs.n; i++)
        if (!strcmp(r.globs.v[i], "*") || !strcmp(r.globs.v[i], "**") || !strcmp(r.globs.v[i], "**/*")) r.always = true;
    const char *tt = json_get_str(fm, "condition");
    if (!tt) tt = json_get_str(fm, "ttsr");
    if (!tt) tt = json_get_str(fm, "ttsrTrigger");
    if (tt) r.ttsr = xstrdup(tt);
    const char *ckeys[] = {"condition", "ttsr_trigger", "ttsrTrigger"};
    for (size_t k = 0; k < ARRAY_LEN(ckeys); k++) {
        json_t *c = json_get(fm, ckeys[k]), *x;
        if (json_is_str(c) && *json_sv(c)) r.has_condition = true;
        json_arr_foreach(c, i, x) if (json_is_str(x) && *json_sv(x)) r.has_condition = true;
        if (c) break;
    }
    if (json_get_str(fm, "scope")) r.scope = xstrdup(json_get_str(fm, "scope"));
    if (json_get_str(fm, "interruptMode")) r.interrupt_mode = xstrdup(json_get_str(fm, "interruptMode"));
    json_t *ast = json_get(fm, "astCondition");
    if (json_is_str(ast)) {
        char *t = str_trim(json_sv(ast));
        r.has_ast_condition = *t != 0;
        free(t);
    } else if (json_is_arr(ast)) {
        json_t *x;
        json_arr_foreach(ast, i, x) if (json_is_str(x) && *json_sv(x)) r.has_ast_condition = true;
    }
    const char *q = json_get_str(fm, "question");
    if (q) {
        char *t = str_trim(q);
        r.has_question = *t != 0;
        free(t);
    }
    vec_push(&d->rules, r);
    json_free(fm);
    free(text);
    return &d->rules.v[d->rules.n - 1];
}

static void scan_rules_dir(discovery_t *d, const char *dir, const char *ext) {
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return;
    for (size_t i = 0; i < dl.n; i++) {
        if (dl.v[i].is_dir || !str_ends(dl.v[i].name, ext)) continue;
        char *p = path_join(dir, dl.v[i].name);
        char *nm = xstrndup(dl.v[i].name, strlen(dl.v[i].name) - strlen(ext));
        add_rule(d, p, nm);
        free(nm);
        free(p);
    }
    dirlist_free(&dl);
}

/* ---------- main discovery ---------- */

static void filter_skills(discovery_t *d);

void discover_all(const char *cwd, discovery_t *d, bool no_context, bool no_skills, bool no_rules) {
    memset(d, 0, sizeof(*d));
    const char *agent = omc_agent_dir();
    const char *home = home_dir();
    strvec_t chain = dir_chain(cwd);
    /* nearest non-empty native dir (.omc preferred, .omp accepted) */
    char *native = NULL;
    for (size_t i = chain.n; i > 0 && !native; i--) {
        const char *names[] = {".omc", ".omp"};
        for (size_t k = 0; k < 2 && !native; k++) {
            char *p = path_join(chain.v[i - 1], names[k]);
            if (fs_is_dir(p) && dir_nonempty(p) && strcmp(chain.v[i - 1], home) != 0) native = p;
            else free(p);
        }
    }
    if (!no_context) {
        char *u = path_join(agent, "AGENTS.md");
        add_file(d, u, 0);
        free(u);
        const char *uagents[] = {".agent/AGENTS.md", ".agents/AGENTS.md"};
        for (size_t i = 0; i < 2; i++) {
            char *p = path_join(home, uagents[i]);
            add_file(d, p, 0);
            free(p);
        }
        if (native) {
            char *p = path_join(native, "AGENTS.md");
            add_file(d, p, 1000);
            free(p);
        }
        /* standalone walk: AGENTS.md beats CLAUDE.md at the same depth */
        for (size_t i = 0; i < chain.n; i++) {
            const char *dirp = chain.v[i];
            const char *per_depth[] = {".agents/AGENTS.md", ".agent/AGENTS.md", "AGENTS.md", "CLAUDE.md"};
            bool taken = false;
            for (size_t k = 0; k < ARRAY_LEN(per_depth) && !taken; k++) {
                if (k < 2 && !strcmp(dirp, home)) continue;
                char *p = path_join(dirp, per_depth[k]);
                size_t before = d->files.n;
                add_file(d, p, (int)i + 1);
                if (d->files.n > before) taken = true;
                free(p);
            }
            json_t *extra = settings_get("contextFiles.extra"), *x;
            json_arr_foreach(extra, k, x) {
                if (!json_is_str(x) || strchr(json_sv(x), '/')) continue;
                char *p = path_join(dirp, json_sv(x));
                add_file(d, p, (int)i + 1);
                free(p);
            }
        }
        const char *cwd_cfg[] = {".claude/CLAUDE.md", ".gemini/GEMINI.md", ".github/copilot-instructions.md"};
        for (size_t i = 0; i < ARRAY_LEN(cwd_cfg); i++) {
            char *p = path_join(cwd, cwd_cfg[i]);
            add_file(d, p, (int)chain.n + 1);
            free(p);
        }
    }
    if (!no_rules) {
        /* builtin provider (priority 100): project rules/, user rules/, sticky RULES.md files */
        /* project rules/ come from <cwd>/.omc or <cwd>/.omp only (omp builtin getConfigDirs) */
        if (strcmp(cwd, home) != 0) {
            const char *pd[] = {".omc", ".omp"};
            for (size_t k = 0; k < 2; k++) {
                char *cd = path_join(cwd, pd[k]);
                if (fs_is_dir(cd) && dir_nonempty(cd)) {
                    char *rd = path_join(cd, "rules");
                    scan_rules_dir(d, rd, ".md");
                    free(rd);
                    free(cd);
                    break;
                }
                free(cd);
            }
        }
        char *ur = path_join(agent, "rules");
        scan_rules_dir(d, ur, ".md");
        free(ur);
        char *u = path_join(agent, "RULES.md");
        size_t before = d->rules.n;
        add_rule(d, u, "RULES");
        if (d->rules.n > before) d->rules.v[d->rules.n - 1].always = true;
        free(u);
        if (native) {
            char *p = path_join(native, "RULES.md");
            before = d->rules.n;
            add_rule(d, p, "RULES@project");
            if (d->rules.n > before) d->rules.v[d->rules.n - 1].always = true;
            free(p);
        }
        char *cur = path_join(cwd, ".cursor/rules");
        scan_rules_dir(d, cur, ".mdc");
        free(cur);
        const char *legacy[] = {".cursorrules", ".windsurfrules", ".clinerules"};
        for (size_t i = 0; i < ARRAY_LEN(legacy); i++) {
            char *p = path_join(cwd, legacy[i]);
            if (fs_is_file(p)) {
                before = d->rules.n;
                add_rule(d, p, legacy[i] + 1);
                if (d->rules.n > before) d->rules.v[d->rules.n - 1].always = true;
            }
            free(p);
        }
        char *gh = path_join(cwd, ".github/instructions");
        scan_rules_dir(d, gh, ".instructions.md");
        free(gh);
        /* builtin-defaults provider (priority 1): every other source wins a name conflict */
        if (settings_bool("ttsr.builtinRules", true)) {
            const char *pre = "prompts/coding-agent/discovery/builtin-rules/";
            for (size_t i = 0; i < omc_embedded_count; i++) {
                const char *nm = omc_embedded[i].name;
                if (!str_starts(nm, pre) || !str_ends(nm, ".md")) continue;
                char *base = xstrndup(nm + strlen(pre), strlen(nm) - strlen(pre) - 3);
                char *text = xstrndup((const char *)omc_embedded[i].data, omc_embedded[i].len);
                char *label = xasprintf("builtin://%s", base);
                rule_t *r = add_rule_text(d, text, label, base);
                if (r) r->builtin = true;
                free(label);
                free(base);
            }
        }
    }
    /* skills.ts loadSkills: `skills.enabled` is the master switch (managed skills included); the per-source
     * toggles gate native (.omc/.omp) and .agents/.agent user/project skills */
    if (!no_skills && settings_bool("skills.enabled", true)) {
        bool pi_project = settings_bool("skills.enablePiProject", true), pi_user = settings_bool("skills.enablePiUser", true);
        bool agents_project = settings_bool("skills.enableAgentsProject", true);
        bool agents_user = settings_bool("skills.enableAgentsUser", true);
        /* first wins, in omp provider-priority order: native (.omc/.omp project + agent dir, 100),
         * Claude project dirs (80), Agent Plugins (75), .agents/.agent (70), Claude Code plugins (70) */
        for (size_t i = chain.n; i > 0 && pi_project; i--) { /* every ancestor's .omc/.omp skills, closest first */
            if (!strcmp(chain.v[i - 1], home)) continue;
            const char *pd[] = {".omc/skills", ".omp/skills"};
            for (size_t k = 0; k < 2; k++) {
                char *p = path_join(chain.v[i - 1], pd[k]);
                scan_skills_dir_opt(d, p, true);
                free(p);
            }
        }
        if (pi_user) {
            char *u = path_join(agent, "skills");
            scan_skills_dir_opt(d, u, true);
            free(u);
        }
        /* Claude provider: user dir (foreign opt-in) then project .claude/skills up to the repo root */
        if (provider_user_source_enabled("claude") || settings_bool("skills.enableClaudeUser", false)) {
            char *cd = claude_config_dir();
            char *p = path_join(cd, "skills");
            scan_skills_dir(d, p);
            free(p);
            free(cd);
        }
        for (size_t i = chain.n; i > 0 && settings_bool("skills.enableClaudeProject", true); i--) {
            if (!strcmp(chain.v[i - 1], home)) continue; /* $HOME/.claude is the user source */
            char *p = path_join(chain.v[i - 1], ".claude/skills");
            scan_skills_dir(d, p);
            free(p);
        }
        plugin_roots_t roots;
        plugin_roots_list(cwd, &roots);
        plugin_skills_standard(&roots, d);
        /* agents.ts getProjectPathCandidates: cwd up to the repo root (else home, else /), home skipped */
        strvec_t walk = discovery_project_ancestors(cwd);
        for (size_t i = 0; i < walk.n && agents_project; i++) {
            const char *dirs[] = {".agent/skills", ".agents/skills"};
            for (size_t k = 0; k < ARRAY_LEN(dirs); k++) {
                char *p = path_join(walk.v[i], dirs[k]);
                scan_skills_dir(d, p);
                free(p);
            }
        }
        strvec_free(&walk);
        const char *uskills[] = {".agent/skills", ".agents/skills"};
        for (size_t i = 0; i < ARRAY_LEN(uskills) && agents_user; i++) {
            char *p = path_join(home, uskills[i]);
            scan_skills_dir(d, p);
            free(p);
        }
        plugin_skills_legacy(&roots, d);
        plugin_roots_free(&roots);
        /* `skills.customDirectories` (~ expanded, description required), after every provider */
        json_t *custom = settings_get("skills.customDirectories"), *cx;
        json_arr_foreach(custom, ci, cx) {
            if (!json_is_str(cx) || !*json_sv(cx)) continue;
            char *p = path_expand_home(json_sv(cx));
            scan_skills_dir_opt(d, p, true);
            free(p);
        }
        scan_managed_skills(d);
        filter_skills(d);
        if (d->skills.n > 1) qsort(d->skills.v, d->skills.n, sizeof(skill_t), cmp_skill);
    }
    free(native);
    strvec_free(&chain);
}

void discovery_free(discovery_t *d) {
    for (size_t i = 0; i < d->files.n; i++) {
        free(d->files.v[i].path);
        free(d->files.v[i].content);
    }
    vec_free(&d->files);
    for (size_t i = 0; i < d->skills.n; i++) {
        free(d->skills.v[i].name);
        free(d->skills.v[i].description);
        free(d->skills.v[i].path);
        free(d->skills.v[i].dir);
    }
    vec_free(&d->skills);
    for (size_t i = 0; i < d->rules.n; i++) {
        rule_t *r = &d->rules.v[i];
        free(r->name);
        free(r->description);
        free(r->path);
        free(r->content);
        free(r->ttsr);
        free(r->scope);
        free(r->interrupt_mode);
        strvec_free(&r->globs);
    }
    vec_free(&d->rules);
}

const skill_t *discovery_find_skill(const discovery_t *d, const char *name) {
    for (size_t i = 0; d && i < d->skills.n; i++)
        if (!strcmp(d->skills.v[i].name, name)) return &d->skills.v[i];
    return NULL;
}

const rule_t *discovery_find_rule(const discovery_t *d, const char *name) {
    for (size_t i = 0; d && i < d->rules.n; i++)
        if (!strcmp(d->rules.v[i].name, name)) return &d->rules.v[i];
    return NULL;
}

/* ---------- system prompt ---------- */

bool discovery_rule_listed(const rule_t *r) {
    json_t *disabled = settings_get("ttsr.disabledRules"), *x;
    json_arr_foreach(disabled, i, x) {
        if (!json_is_str(x)) continue;
        char *t = str_trim(json_sv(x));
        bool hit = *t && !strcmp(t, r->name);
        free(t);
        if (hit) return false;
    }
    if (r->builtin && !settings_bool("ttsr.builtinRules", true)) return false;
    bool ttsr = (r->ttsr && *r->ttsr) || r->has_condition || r->has_ast_condition || r->has_question;
    if (ttsr && settings_bool("ttsr.enabled", true)) return false;
    return r->always || (r->description && *r->description);
}

static void push_url_doc(strvec_t *out, const char *name, const char *from, const char *to) {
    char *rel = xasprintf("coding-agent/prompts/internal-urls/%s.md", name);
    char *doc = prompt_load(rel);
    free(rel);
    if (!doc) return;
    char *t = str_trim(doc);
    if (from) {
        char *r = str_replace(t, from, to);
        free(t);
        t = r;
    }
    strvec_push_own(out, t);
    free(doc);
}

strvec_t internal_url_docs(const sysprompt_url_host_t *h) {
    /* omp InternalUrlRouter registration order: skill rule memory out agent history artifact local proc cfg
     * ssh security vault issue/pr mr mcp omp. security/vault have no omc handler. */
    strvec_t out = {0};
    if (h->skill_uri_access) push_url_doc(&out, "skill", NULL, NULL);
    if (h->rule_count > 0) push_url_doc(&out, "rule", NULL, NULL);
    if (h->memory_local) push_url_doc(&out, "memory", NULL, NULL);
    if (h->out_replies) push_url_doc(&out, "out", NULL, NULL);
    push_url_doc(&out, "agent", NULL, NULL);
    push_url_doc(&out, "history", NULL, NULL);
    push_url_doc(&out, "artifact", NULL, NULL);
    push_url_doc(&out, "local", NULL, NULL);
    push_url_doc(&out, "proc", NULL, NULL);
    if (h->settings_approval) push_url_doc(&out, "cfg", "omp settings", "omc settings");
    char *ssh = which("ssh");
    if (ssh) push_url_doc(&out, "ssh", NULL, NULL);
    free(ssh);
    char *gh = which("gh"); /* omp advertises issue:// / pr:// only when the gh CLI is installed */
    if (gh) push_url_doc(&out, "issue", NULL, NULL);
    free(gh);
    char *glab = which("glab"); /* mr:// only when the glab CLI is installed */
    if (glab) push_url_doc(&out, "mr", NULL, NULL);
    free(glab);
    push_url_doc(&out, "mcp", NULL, NULL);
    push_url_doc(&out, "omp", "`omp://`", "`omc://`");
    return out;
}


static char *load_override(const char *cwd, const char *name) {
    const char *dirs[] = {".omc", ".omp"};
    for (size_t i = 0; i < 2; i++) {
        char *p = path_join3(cwd, dirs[i], name);
        char *c = read_nonempty(p);
        free(p);
        if (c) return c;
    }
    char *p = omc_agent_path(name);
    char *c = read_nonempty(p);
    free(p);
    return c;
}

/* skills.ts loadSkills: `skills.ignoredSkills` drops matching authored skills, `skills.includeSkills` (the
 * `--skills` flag) keeps only matching ones; managed skills are exempt (Bun.Glob name matching) */
static bool skill_name_matches(const json_t *patterns, const char *name) {
    json_t *p;
    json_arr_foreach(patterns, i, p) if (json_is_str(p) && glob_match(json_sv(p), name, 0)) return true;
    return false;
}

static void filter_skills(discovery_t *d) {
    json_t *inc = settings_get("skills.includeSkills"), *ign = settings_get("skills.ignoredSkills");
    if (!json_len(inc) && !json_len(ign)) return;
    size_t w = 0;
    for (size_t i = 0; i < d->skills.n; i++) {
        skill_t *s = &d->skills.v[i];
        bool keep = s->managed || ((!json_len(ign) || !skill_name_matches(ign, s->name)) &&
                                   (!json_len(inc) || skill_name_matches(inc, s->name)));
        if (keep) d->skills.v[w++] = *s;
        else {
            free(s->name);
            free(s->description);
            free(s->path);
            free(s->dir);
        }
    }
    d->skills.n = w;
}

/* discoverSystemPromptOverride: project (.omc/.omp) then user (agent dir); a blank template is ignored */
static void discover_system_override(const char *cwd, char **text, char **templ) {
    for (int level = 0; level < 2; level++) {
        const char *dirs[] = {".omc", ".omp"};
        for (size_t k = 0; k < (level ? 1u : 2u); k++) {
            char *tp = level ? omc_agent_path("SYSTEM.md") : path_join3(cwd, dirs[k], "SYSTEM.md");
            char *t = read_nonempty(tp);
            free(tp);
            if (t) {
                *text = t;
                return;
            }
        }
        for (size_t k = 0; k < (level ? 1u : 2u); k++) {
            char *tp = level ? omc_agent_path("SYSTEM_TEMPLATE.md") : path_join3(cwd, dirs[k], "SYSTEM_TEMPLATE.md");
            char *t = read_nonempty(tp);
            free(tp);
            if (t) {
                *templ = t;
                return;
            }
        }
    }
}

static char *maybe_file(const char *s) {
    if (!s) return NULL;
    char *p = path_expand_home(s);
    if (fs_is_file(p)) {
        char *c = fs_read(p, NULL);
        free(p);
        return c;
    }
    free(p);
    return xstrdup(s);
}

json_t *build_system_prompt(const sysprompt_opts_t *o) {
    json_t *data = json_obj();
    json_t *tools = json_arr();
    json_t *refs = json_obj();
    json_t *info = json_arr();
    for (size_t i = 0; o->tool_names && i < o->tool_names->n; i++) {
        json_push_str(tools, o->tool_names->v[i]);
        const char *ref = o->tool_refs ? json_get_str(o->tool_refs, o->tool_names->v[i]) : NULL;
        if (!ref) ref = o->tool_names->v[i];
        json_set_str(refs, o->tool_names->v[i], ref);
        json_t *ti = json_obj();
        json_set_str(ti, "name", ref);
        const char *lbl = o->tool_labels ? json_get_str(o->tool_labels, o->tool_names->v[i]) : NULL;
        json_set_str(ti, "label", lbl ? lbl : "");
        json_push(info, ti);
    }
    /* xd://-mounted tools count as present for prompt gates and resolve to their xd:// URL */
    json_t *xe;
    json_arr_foreach(o->xdev_tools, xi, xe) {
        const char *n = json_get_str(xe, "name");
        if (!n || json_get(refs, n)) continue;
        json_push_str(tools, n);
        char *u = xasprintf("xd://%s", n);
        json_set_str(refs, n, u);
        free(u);
    }
    json_set(data, "tools", tools);
    json_set(data, "toolRefs", refs);
    json_set(data, "toolInfo", info);
    json_set_bool(data, "toolListMode", true);
    json_set(data, "xdevTools", o->xdev_tools ? json_clone(o->xdev_tools) : json_arr());
    json_set_str(data, "xdevDocs", o->xdev_docs ? o->xdev_docs : "");
    json_set_bool(data, "autoQaEnabled", o->auto_qa);
    json_set_bool(data, "intentTracing", agent_intent_tracing());
    json_set_str(data, "intentField", agent_intent_tracing() ? INTENT_FIELD : "");
    json_t *env = json_arr();
    struct utsname un;
    if (uname(&un) == 0) {
        json_t *e1 = json_obj();
        json_set_str(e1, "label", "OS");
        char *v = xasprintf("%s %s", !strcmp(un.sysname, "Linux") ? "linux" : !strcmp(un.sysname, "Darwin") ? "darwin" : un.sysname,
                            un.release);
        json_set_str(e1, "value", v);
        free(v);
        json_push(env, e1);
        json_t *e2 = json_obj();
        json_set_str(e2, "label", "Arch");
        json_set_str(e2, "value", !strcmp(un.machine, "aarch64") ? "arm64" : !strcmp(un.machine, "x86_64") ? "x64" : un.machine);
        json_push(env, e2);
    }
    json_set(data, "environment", env);
    json_t *cf = json_arr();
    for (size_t i = 0; o->disc && !o->subagent && i < o->disc->files.n; i++) {
        json_t *f = json_obj();
        json_set_str(f, "path", o->disc->files.v[i].path); /* absolute, as omp renders it */
        json_set_str(f, "content", o->disc->files.v[i].content);
        json_push(cf, f);
    }
    /* each additional workspace root contributes its own context files (AGENTS.md, ...), deduped against the
     * ones already loaded (system-prompt.ts loadProjectContextFiles per root + dedupeContainedContextFiles) */
    for (size_t r = 0; o->additional_roots && o->disc && !o->subagent && r < o->additional_roots->n; r++) {
        if (str_eq(o->additional_roots->v[r], o->cwd)) continue;
        discovery_t extra;
        discover_all(o->additional_roots->v[r], &extra, false, true, true);
        for (size_t i = 0; i < extra.files.n; i++) {
            bool dup = false;
            json_t *f;
            json_arr_foreach(cf, k, f) dup |= str_eq(json_get_str(f, "path"), extra.files.v[i].path) ||
                                             str_eq(json_get_str(f, "content"), extra.files.v[i].content);
            if (dup) continue;
            json_t *nf = json_obj();
            json_set_str(nf, "path", extra.files.v[i].path);
            json_set_str(nf, "content", extra.files.v[i].content);
            json_push(cf, nf);
        }
        discovery_free(&extra);
    }
    json_set(data, "contextFiles", cf);
    json_t *roots = json_arr();
    for (size_t r = 0; o->additional_roots && r < o->additional_roots->n; r++)
        if (!str_eq(o->additional_roots->v[r], o->cwd)) json_push_str(roots, o->additional_roots->v[r]);
    json_set(data, "additionalWorkspaceRoots", roots);
    json_t *skills = json_arr(), *rules = json_arr(), *always = json_arr();
    bool has_read = o->tool_names && strvec_contains(o->tool_names, "read");
    /* sdk.ts: skills: cfgSkillful ? skills : [] (the `skillful` setting, /skillful session override) */
    bool skillful = settings_bool("skillful", true);
    for (size_t i = 0; o->disc && has_read && skillful && i < o->disc->skills.n; i++) {
        if (o->disc->skills.v[i].hide) continue;
        json_t *s = json_obj();
        json_set_str(s, "name", o->disc->skills.v[i].name);
        char *hint = o->skill_desc ? o->skill_desc(o->skill_desc_ud, o->disc->skills.v[i].name, o->disc->skills.v[i].description) : NULL;
        json_set_str(s, "description", hint ? hint : o->disc->skills.v[i].description);
        free(hint);
        json_push(skills, s);
    }
    for (size_t i = 0; o->disc && i < o->disc->rules.n; i++) {
        const rule_t *r = &o->disc->rules.v[i];
        if (!discovery_rule_listed(r)) continue;
        json_t *x = json_obj();
        json_set_str(x, "name", r->name);
        json_set_str(x, "description", r->description);
        json_set_str(x, "content", r->content);
        json_t *g = json_arr();
        for (size_t k = 0; k < r->globs.n; k++) json_push_str(g, r->globs.v[k]);
        json_set(x, "globs", g);
        json_push(r->always ? always : rules, x);
    }
    json_set(data, "skills", skills);
    json_set(data, "rules", rules);
    json_set(data, "alwaysApplyRules", always);
    json_t *urls = json_arr();
    sysprompt_url_host_t host = {
        .skill_uri_access = json_len(skills) > 0,
        .rule_count = json_len(rules),
        .memory_local = str_eq(memory_backend(), "local"),
        .out_replies = settings_configured("out.enabled") ? settings_bool("out.enabled", true) : o->tui_transcript,
        .settings_approval = o->tui_transcript,
    };
    strvec_t docs = internal_url_docs(&host);
    for (size_t i = 0; i < docs.n; i++) json_push_str(urls, docs.v[i]);
    strvec_free(&docs);
    json_set(data, "internalUrls", urls);
    json_set_str(data, "cwd", o->cwd);
    json_set_str(data, "model", settings_bool("includeModelInPrompt", true) && o->model_name ? o->model_name : "");
    /* delegation (sdk.ts buildSystemPrompt options): model catalog bias, task.eager/batch/maxConcurrency, IRC */
    const char *bias = o->delegation_bias ? o->delegation_bias : "eager";
    const char *eager = settings_str("task.eager", "default");
    bool eager_tasks = !str_eq(eager, "default");
    json_set_str(data, "delegationBias", bias);
    json_set_bool(data, "eagerTasks", eager_tasks);
    json_set_bool(data, "eagerTasksAlways", str_eq(eager, "always"));
    json_set_bool(data, "inlineFirstDelegation", str_eq(bias, "restrained") && !eager_tasks);
    int64_t maxc = settings_int("task.maxConcurrency", 32);
    json_set_int(data, "MAX_CONCURRENCY", maxc > 0 ? maxc : 0);
    json_set_bool(data, "taskBatch", settings_bool("task.batch", true));
    json_set_bool(data, "taskIrcEnabled", o->irc_enabled);
    bool scout = true;
    json_t *dis = settings_get("task.disabledAgents"), *dv;
    json_arr_foreach(dis, di, dv) if (str_eq(json_sv(dv), "scout")) scout = false;
    json_set_bool(data, "scoutAvailable", scout);
    json_set_bool(data, "renderMermaid", false);
    json_set_bool(data, "subagent", o->subagent);
    /* personality: "none" (and every subagent: sdk.ts passes "none" for agentKind "sub") skips the file lookup */
    const char *pers = o->subagent ? "none" : settings_str("personality", "default");
    char *pfile = pers && strcmp(pers, "none") != 0 ? load_override(o->cwd, "PERSONALITY.md") : NULL;
    if (pfile) {
        json_set_str(data, "personality", pfile);
        free(pfile);
    } else if (pers && strcmp(pers, "none") != 0) {
        char *rel = xasprintf("coding-agent/prompts/system/personalities/%s.md", pers);
        char *pt = prompt_load(rel);
        free(rel);
        if (pt) {
            char *t = str_trim(pt);
            json_set_str(data, "personality", t);
            free(t);
            free(pt);
        }
    }
    char *append = maybe_file(o->append_prompt);
    if (!append) append = load_override(o->cwd, "APPEND_SYSTEM.md");
    /* composeAppendPrompt: generated blocks (memory, auto-learn) then the user's append in its own section */
    if (o->generated_append && *o->generated_append) {
        char *ua = append ? str_trim(append) : NULL;
        if (!ua || !*ua) {
            free(append);
            append = xstrdup(o->generated_append);
        } else {
            json_t *uc = json_obj();
            json_set_str(uc, "generatedAppend", o->generated_append);
            json_set_str(uc, "userAppend", append);
            char *w = prompt_render("coding-agent/prompts/system/user-append.md", uc);
            json_free(uc);
            free(append);
            append = w;
        }
        free(ua);
    }
    json_set_str(data, "appendPrompt", append ? append : "");
    char *custom = maybe_file(o->custom_prompt);
    /* explicit prompt inputs win over discovered SYSTEM.md / SYSTEM_TEMPLATE.md (project level first; at one
     * level the literal SYSTEM.md wins over the template) */
    char *templ = o->custom_template && !custom ? xstrdup(o->custom_template) : NULL;
    if (!custom && !templ) discover_system_override(o->cwd, &custom, &templ);
    json_t *blocks = json_arr();
    char *rendered;
    if (templ) {
        rendered = tpl_render(templ, data);
        free(templ);
    } else if (custom) {
        json_set_str(data, "customPrompt", custom);
        char *tpl = prompt_load("coding-agent/prompts/system/custom-system-prompt.md");
        rendered = tpl ? tpl_render(tpl, data) : xstrdup(custom);
        free(tpl);
    } else {
        rendered = prompt_render("coding-agent/prompts/system/system-prompt.md", data);
        char *fixed = str_replace(rendered, "You are omp's trusted coding assistant.", "You are omc's trusted coding assistant.");
        free(rendered);
        rendered = fixed;
    }
    json_push_str(blocks, rendered);
    free(rendered);
    char *proj = prompt_render("coding-agent/prompts/system/project-prompt.md", data);
    char *pt = str_trim(proj);
    if (*pt) json_push_str(blocks, pt);
    free(pt);
    free(proj);
    free(custom);
    free(append);
    json_free(data);
    return blocks;
}
