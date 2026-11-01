#include "discovery.h"

#include "config/settings.h"
#include "core/fs.h"
#include "prompt/template.h"
#include "text/yaml.h"
#include "core/embed.h"

#include <unistd.h>

#include <ctype.h>
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

static void scan_skills_dir(discovery_t *d, const char *dir) {
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return;
    for (size_t i = 0; i < dl.n; i++) {
        if (!dl.v[i].is_dir) continue;
        char *sd = path_join(dir, dl.v[i].name);
        char *sf = path_join(sd, "SKILL.md");
        char *text = read_nonempty(sf);
        if (text) {
            const char *body;
            json_t *fm = frontmatter_parse(text, &body);
            const char *name = json_get_str(fm, "name");
            if (!name) name = dl.v[i].name;
            bool dup = false;
            for (size_t k = 0; k < d->skills.n; k++)
                if (!strcmp(d->skills.v[k].name, name)) dup = true;
            if (!dup) {
                skill_t s = {xstrdup(name), xstrdup(json_get_str(fm, "description") ? json_get_str(fm, "description") : ""),
                             xstrdup(sf), xstrdup(sd), json_get_bool(fm, "hide", false)};
                vec_push(&d->skills, s);
            }
            json_free(fm);
            free(text);
        }
        free(sf);
        free(sd);
    }
    dirlist_free(&dl);
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
    const char *nm = json_get_str(fm, "name");
    r.name = xstrdup(nm ? nm : default_name);
    for (size_t k = 0; k < d->rules.n; k++)
        if (!strcmp(d->rules.v[k].name, r.name)) {
            free(r.name);
            json_free(fm);
            free(text);
            return NULL;
        }
    r.description = xstrdup(json_get_str(fm, "description") ? json_get_str(fm, "description") : "");
    r.path = xstrdup(path);
    r.content = str_trim(body);
    r.always = json_get_bool(fm, "alwaysApply", false);
    json_t *g = json_get(fm, "globs");
    if (!g) g = json_get(fm, "applyTo");
    if (json_is_arr(g)) {
        json_t *x;
        json_arr_foreach(g, i, x) if (json_is_str(x)) strvec_push(&r.globs, json_sv(x));
    } else if (json_is_str(g) && *json_sv(g)) {
        strvec_t parts = str_split(json_sv(g), ",");
        for (size_t i = 0; i < parts.n; i++) {
            char *t = str_trim(parts.v[i]);
            if (*t) strvec_push_own(&r.globs, t);
            else free(t);
        }
        strvec_free(&parts);
    }
    for (size_t i = 0; i < r.globs.n; i++)
        if (!strcmp(r.globs.v[i], "*") || !strcmp(r.globs.v[i], "**") || !strcmp(r.globs.v[i], "**/*")) r.always = true;
    const char *tt = json_get_str(fm, "condition");
    if (!tt) tt = json_get_str(fm, "ttsr");
    if (!tt) tt = json_get_str(fm, "ttsrTrigger");
    if (tt) r.ttsr = xstrdup(tt);
    if (json_get_str(fm, "scope")) r.scope = xstrdup(json_get_str(fm, "scope"));
    if (json_get_str(fm, "interruptMode")) r.interrupt_mode = xstrdup(json_get_str(fm, "interruptMode"));
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
        char *u = path_join(agent, "RULES.md");
        add_rule(d, u, "user-rules");
        free(u);
        if (d->rules.n) d->rules.v[d->rules.n - 1].always = true;
        if (native) {
            char *p = path_join(native, "RULES.md");
            size_t before = d->rules.n;
            add_rule(d, p, "project-rules");
            if (d->rules.n > before) d->rules.v[d->rules.n - 1].always = true;
            free(p);
            char *rd = path_join(native, "rules");
            scan_rules_dir(d, rd, ".md");
            free(rd);
        }
        char *ur = path_join(agent, "rules");
        scan_rules_dir(d, ur, ".md");
        free(ur);
        char *cur = path_join(cwd, ".cursor/rules");
        scan_rules_dir(d, cur, ".mdc");
        free(cur);
        const char *legacy[] = {".cursorrules", ".windsurfrules", ".clinerules"};
        for (size_t i = 0; i < ARRAY_LEN(legacy); i++) {
            char *p = path_join(cwd, legacy[i]);
            if (fs_is_file(p)) {
                size_t before = d->rules.n;
                add_rule(d, p, legacy[i] + 1);
                if (d->rules.n > before) d->rules.v[d->rules.n - 1].always = true;
            }
            free(p);
        }
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
        char *gh = path_join(cwd, ".github/instructions");
        scan_rules_dir(d, gh, ".instructions.md");
        free(gh);
    }
    if (!no_skills) {
        if (native) {
            char *p = path_join(native, "skills");
            scan_skills_dir(d, p);
            free(p);
        }
        for (size_t i = chain.n; i > 0; i--) {
            const char *dirs[] = {".agents/skills", ".agent/skills", ".claude/skills"};
            for (size_t k = 0; k < ARRAY_LEN(dirs); k++) {
                char *p = path_join(chain.v[i - 1], dirs[k]);
                scan_skills_dir(d, p);
                free(p);
            }
        }
        char *u = path_join(agent, "skills");
        scan_skills_dir(d, u);
        free(u);
        const char *uskills[] = {".agents/skills", ".agent/skills"};
        for (size_t i = 0; i < ARRAY_LEN(uskills); i++) {
            char *p = path_join(home, uskills[i]);
            scan_skills_dir(d, p);
            free(p);
        }
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
        json_set_str(refs, o->tool_names->v[i], o->tool_names->v[i]);
        json_t *ti = json_obj();
        json_set_str(ti, "name", o->tool_names->v[i]);
        json_set_str(ti, "label", "");
        json_push(info, ti);
    }
    json_set(data, "tools", tools);
    json_set(data, "toolRefs", refs);
    json_set(data, "toolInfo", info);
    json_set_bool(data, "toolListMode", true);
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
        char *disp = path_display(o->disc->files.v[i].path, NULL);
        json_set_str(f, "path", disp);
        free(disp);
        json_set_str(f, "content", o->disc->files.v[i].content);
        json_push(cf, f);
    }
    json_set(data, "contextFiles", cf);
    json_t *skills = json_arr(), *rules = json_arr(), *always = json_arr();
    bool has_read = o->tool_names && strvec_contains(o->tool_names, "read");
    for (size_t i = 0; o->disc && has_read && i < o->disc->skills.n; i++) {
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
        if (r->ttsr) continue; /* stream-triggered rules are not listed in the prompt */
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
    const char *url_docs[] = {"skill", "rule", "artifact", "agent", "local", "issue", "ssh"};
    for (size_t i = 0; i < ARRAY_LEN(url_docs); i++) {
        if (!strcmp(url_docs[i], "skill") && !json_len(skills)) continue;
        if (!strcmp(url_docs[i], "rule") && !json_len(rules)) continue;
        if (!strcmp(url_docs[i], "agent") && !(o->tool_names && strvec_contains(o->tool_names, "task"))) continue;
        if (!strcmp(url_docs[i], "ssh")) {
            char *ssh = which("ssh");
            free(ssh);
            if (!ssh) continue;
        }
        char *rel = xasprintf("coding-agent/prompts/internal-urls/%s.md", url_docs[i]);
        char *doc = prompt_load(rel);
        free(rel);
        if (doc) {
            char *t = str_trim(doc);
            json_push_str(urls, t);
            free(t);
            free(doc);
        }
    }
    json_set(data, "internalUrls", urls);
    json_set_str(data, "cwd", o->cwd);
    json_set_str(data, "model", settings_bool("includeModelInPrompt", true) && o->model_name ? o->model_name : "");
    json_set_str(data, "delegationBias", settings_str("task.delegationBias", "gated"));
    json_set_int(data, "MAX_CONCURRENCY", settings_int("task.maxConcurrency", 8));
    json_set_bool(data, "renderMermaid", false);
    json_set_bool(data, "subagent", o->subagent);
    json_set_bool(data, "taskBatch", false);
    /* personality */
    const char *pers = settings_str("personality", "default");
    char *pfile = load_override(o->cwd, "PERSONALITY.md");
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
    json_set_str(data, "appendPrompt", append ? append : "");
    char *custom = maybe_file(o->custom_prompt);
    if (!custom) custom = load_override(o->cwd, "SYSTEM.md");
    json_t *blocks = json_arr();
    char *rendered;
    if (custom) {
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
