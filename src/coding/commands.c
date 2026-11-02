/* commands.c - file slash commands, prompt templates and /skill: invocations. See commands.h. */
#include "coding/commands.h"

#include "coding/plugins.h"
#include "config/settings.h"
#include "core/fs.h"
#include "prompt/template.h"

#include <ctype.h>

/* ---------- small helpers ---------- */

static bool list_setting_has(const char *key, const char *id) {
    json_t *arr = settings_get(key), *x;
    json_arr_foreach(arr, i, x) if (json_is_str(x) && !strcmp(json_sv(x), id)) return true;
    return false;
}

static bool dir_nonempty(const char *d) {
    dirlist_t dl;
    if (fs_list_dir(d, &dl) != 0) return false;
    bool r = dl.n > 0;
    dirlist_free(&dl);
    return r;
}

/* parseFrontmatter (CRLF normalized; a body after frontmatter is trimmed, a document without one is kept) */
static char *normalize_newlines(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s; *p; p++) {
        if (*p == '\r') {
            sb_putc(&b, '\n');
            if (p[1] == '\n') p++;
        } else sb_putc(&b, *p);
    }
    return sb_detach(&b);
}

static json_t *parse_fm(const char *raw, char **body) {
    char *n = normalize_newlines(raw);
    const char *b = n;
    json_t *fm = json_obj();
    if (str_starts(n, "---") && strstr(n + 3, "\n---")) {
        json_free(fm);
        fm = frontmatter_parse(n, &b);
        if (b != n) {
            *body = str_trim(b);
            free(n);
            return fm;
        }
    }
    *body = n;
    return fm;
}

/* optionalTrimmedString */
static char *fm_trimmed(const json_t *fm, const char *key) {
    const char *v = json_get_str(fm, key);
    if (!v) return NULL;
    char *t = str_trim(v);
    if (!*t) {
        free(t);
        return NULL;
    }
    return t;
}

/* first non-empty body line, 60 characters + "..." */
static char *first_line_description(const char *body) {
    for (const char *p = body; *p;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        bool blank = true;
        for (size_t i = 0; i < len; i++)
            if (!isspace((unsigned char)p[i])) blank = false;
        if (!blank) {
            size_t cut = 0, chars = 0;
            while (cut < len && chars < 60) {
                uint32_t cp;
                cut += (size_t)utf8_decode(p + cut, len - cut, &cp);
                chars++;
            }
            return cut < len ? xasprintf("%.*s...", (int)cut, p) : xstrndup(p, len);
        }
        if (!nl) break;
        p = nl + 1;
    }
    return xstrdup("");
}

/* ---------- discovery ---------- */

typedef struct {
    file_cmds_t *out;
} loader_t;

static void add_cmd(file_cmds_t *out, const char *name, const char *raw, const char *path, const char *provider,
                    const char *level, bool parse_name) {
    for (size_t i = 0; i < out->n; i++)
        if (!strcmp(out->v[i].name, name)) return; /* capability dedupe: first (highest priority) wins */
    char *dis = xasprintf("slash-command:%s", name);
    bool disabled = list_setting_has("disabledExtensions", dis);
    free(dis);
    if (disabled) return;
    char *body;
    json_t *fm = parse_fm(raw, &body);
    char *hint = fm_trimmed(fm, "argumentHint");
    if (!hint) hint = fm_trimmed(fm, "argument-hint");
    bool side = json_is_bool(json_get(fm, "side")) && json_get_bool(fm, "side", false);
    char *content = NULL;
    if (parse_name) {
        /* Codex / OpenCode strip the frontmatter at discovery: the description comes from the body's first line */
        content = body;
        body = NULL;
        json_free(fm);
        fm = json_obj();
        char *b2;
        json_t *fm2 = parse_fm(content, &b2);
        json_free(fm2);
        free(content);
        content = b2;
    } else {
        content = body;
        body = NULL;
    }
    char *desc = fm_trimmed(fm, "description");
    if (!desc) desc = first_line_description(content);
    file_cmd_t c = {xstrdup(name), desc, hint, content, NULL, path ? xstrdup(path) : NULL, side};
    if (provider) {
        char lv[16];
        snprintf(lv, sizeof lv, "%s", level);
        lv[0] = (char)toupper((unsigned char)lv[0]);
        c.source = xasprintf("via %s %s", provider, lv);
    } else c.source = xstrdup("bundled");
    vec_push(out, c);
    json_free(fm);
    free(body);
}

/* frontmatter `name` (Codex / OpenCode) */
static char *fm_name(const char *raw) {
    char *body;
    json_t *fm = parse_fm(raw, &body);
    free(body);
    const json_t *n = json_get(fm, "name");
    char *r = NULL;
    if (json_is_str(n) && *json_sv(n)) r = xstrdup(json_sv(n));
    json_free(fm);
    return r;
}

/* loadFilesFromDir(extensions: md): non-hidden *.md files (recursive: relative path kept for Claude names) */
static void collect_md(const char *dir, const char *rel, bool recursive, strvec_t *rels) {
    char *abs = *rel ? path_join(dir, rel) : xstrdup(dir);
    dirlist_t dl;
    if (fs_list_dir(abs, &dl) == 0) {
        for (size_t i = 0; i < dl.n; i++) {
            const char *nm = dl.v[i].name;
            if (nm[0] == '.') continue;
            char *r = *rel ? path_join(rel, nm) : xstrdup(nm);
            char *p = path_join(dir, r);
            if (fs_is_dir(p)) {
                if (recursive) collect_md(dir, r, true, rels);
            } else if (str_ends(nm, ".md") && fs_is_file(p)) {
                strvec_push(rels, r);
            }
            free(p);
            free(r);
        }
        dirlist_free(&dl);
    }
    free(abs);
}

static char *read_text(const char *path) {
    size_t n;
    return fs_read_limit(path, 1u << 20, &n, NULL);
}

/* builtin / agents / omp-plugin style: name = basename without .md, content kept whole */
static void load_flat(file_cmds_t *out, const char *dir, const char *provider, const char *level, const char *prefix,
                      bool frontmatter_name) {
    strvec_t rels = {0};
    collect_md(dir, "", false, &rels);
    for (size_t i = 0; i < rels.n; i++) {
        char *p = path_join(dir, rels.v[i]);
        char *raw = read_text(p);
        if (raw) {
            char *base = xstrndup(rels.v[i], strlen(rels.v[i]) - 3);
            char *fmn = frontmatter_name ? fm_name(raw) : NULL;
            char *name = prefix ? xasprintf("%s:%s", prefix, fmn ? fmn : base) : xstrdup(fmn ? fmn : base);
            add_cmd(out, name, raw, p, provider, level, frontmatter_name);
            free(name);
            free(fmn);
            free(base);
            free(raw);
        }
        free(p);
    }
    strvec_free(&rels);
}

/* claude.ts: recursive; root commands, then nested ones under their basename, then `ns:name` aliases */
static void load_claude_dir(file_cmds_t *out, const char *dir, const char *level) {
    strvec_t rels = {0};
    collect_md(dir, "", true, &rels);
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < rels.n; i++) {
            bool nested = strchr(rels.v[i], '/') != NULL;
            if ((pass == 0 && nested) || (pass > 0 && !nested)) continue;
            char *p = path_join(dir, rels.v[i]);
            char *raw = read_text(p);
            if (raw) {
                char *name;
                if (pass < 2) name = xstrndup(path_basename(rels.v[i]), strlen(path_basename(rels.v[i])) - 3);
                else {
                    name = xstrndup(rels.v[i], strlen(rels.v[i]) - 3);
                    for (char *c = name; *c; c++)
                        if (*c == '/') *c = ':';
                }
                add_cmd(out, name, raw, p, "Claude Code", level, false);
                free(name);
                free(raw);
            }
            free(p);
        }
        bool any_nested = false;
        for (size_t i = 0; i < rels.n; i++) any_nested |= strchr(rels.v[i], '/') != NULL;
        if (!any_nested) break;
    }
    strvec_free(&rels);
}

static void plugin_cmd_cb(void *ud, const char *path, const char *plugin, bool project) {
    loader_t *l = ud;
    const char *level = project ? "project" : "user";
    if (fs_is_file(path)) {
        if (!str_ends(path, ".md")) return;
        char *raw = read_text(path);
        if (!raw) return;
        const char *b = path_basename(path);
        char *base = xstrndup(b, strlen(b) - 3);
        char *name = plugin ? xasprintf("%s:%s", plugin, base) : xstrdup(base);
        add_cmd(l->out, name, raw, path, "Claude Code Marketplace", level, false);
        free(name);
        free(base);
        free(raw);
        return;
    }
    load_flat(l->out, path, "Claude Code Marketplace", level, plugin, false);
}

void commands_load(const char *cwd, file_cmds_t *out) {
    memset(out, 0, sizeof *out);
    const char *home = home_dir();
    bool cwd_is_home = !strcmp(cwd, home);
    /* native (priority 100): <cwd>/.omc|.omp (non-empty), then the agent dir */
    if (!list_setting_has("disabledProviders", "native")) {
        if (!cwd_is_home) {
            const char *pd[] = {".omc", ".omp"};
            for (size_t k = 0; k < 2; k++) {
                char *d = path_join(cwd, pd[k]);
                bool ok = fs_is_dir(d) && dir_nonempty(d);
                if (ok) {
                    char *cd = path_join(d, "commands");
                    load_flat(out, cd, "OMP", "project", NULL, false);
                    free(cd);
                }
                free(d);
                if (ok) break;
            }
        }
        const char *agent = omc_agent_dir();
        if (fs_is_dir(agent) && dir_nonempty(agent)) {
            char *cd = path_join(agent, "commands");
            load_flat(out, cd, "OMP", "user", NULL, false);
            free(cd);
        }
    }
    /* claude (80): ~/.claude/commands (opt-in) and <cwd>/.claude/commands, recursive with ns aliases */
    if (!list_setting_has("disabledProviders", "claude")) {
        if (settings_bool("commands.enableClaudeUser", false) || provider_user_source_enabled("claude")) {
            char *cd = claude_config_dir();
            char *d = path_join(cd, "commands");
            load_claude_dir(out, d, "user");
            free(d);
            free(cd);
        }
        if (settings_bool("commands.enableClaudeProject", true)) {
            char *d = path_join(cwd, ".claude/commands");
            load_claude_dir(out, d, "project");
            free(d);
        }
    }
    /* priority 70, registration order: claude-plugins, agents, codex */
    if (!list_setting_has("disabledProviders", "claude-plugins")) {
        plugin_roots_t roots;
        plugin_roots_list(cwd, &roots);
        loader_t l = {out};
        plugin_commands_each(&roots, plugin_cmd_cb, &l);
        plugin_roots_free(&roots);
    }
    if (!list_setting_has("disabledProviders", "agents")) {
        /* project walk-up (cwd .. repo root, else home), skipping home; then ~/.agent[s] */
        const char *bd[] = {".agent", ".agents"};
        strvec_t walk = discovery_project_ancestors(cwd);
        for (size_t i = 0; i < walk.n; i++) {
            for (size_t k = 0; k < 2; k++) {
                char *d = path_join3(walk.v[i], bd[k], "commands");
                load_flat(out, d, "Agent Dirs (.agent/.agents)", "project", NULL, false);
                free(d);
            }
        }
        strvec_free(&walk);
        for (size_t k = 0; k < 2; k++) {
            char *d = path_join3(home, bd[k], "commands");
            load_flat(out, d, "Agent Dirs (.agent/.agents)", "user", NULL, false);
            free(d);
        }
    }
    if (!list_setting_has("disabledProviders", "codex")) {
        if (provider_user_source_enabled("codex")) {
            char *d = path_join(home, ".codex/commands");
            load_flat(out, d, "OpenAI Codex", "user", NULL, true);
            free(d);
        }
        char *d = path_join(cwd, ".codex/commands");
        load_flat(out, d, "OpenAI Codex", "project", NULL, true);
        free(d);
    }
    /* opencode (55) */
    if (!list_setting_has("disabledProviders", "opencode")) {
        if (settings_bool("commands.enableOpencodeUser", false) || provider_user_source_enabled("opencode")) {
            char *d = path_join(home, ".config/opencode/commands");
            load_flat(out, d, "OpenCode", "user", NULL, true);
            free(d);
        }
        if (settings_bool("commands.enableOpencodeProject", true)) {
            char *d = path_join(cwd, ".opencode/commands");
            load_flat(out, d, "OpenCode", "project", NULL, true);
            free(d);
        }
    }
    /* bundled (EMBEDDED_COMMAND_TEMPLATES): init.md, rendered once */
    if (!commands_find(out, "init")) {
        char *raw = prompt_load("coding-agent/prompts/agents/init.md");
        if (raw) {
            json_t *ctx = json_obj();
            char *r = tpl_render(raw, ctx);
            json_free(ctx);
            add_cmd(out, "init", r ? r : raw, NULL, NULL, NULL, false);
            free(r);
            free(raw);
        }
    }
}

void commands_free(file_cmds_t *c) {
    for (size_t i = 0; i < c->n; i++) {
        file_cmd_t *f = &c->v[i];
        free(f->name);
        free(f->description);
        free(f->argument_hint);
        free(f->content);
        free(f->source);
        free(f->path);
    }
    vec_free(c);
}

const file_cmd_t *commands_find(const file_cmds_t *c, const char *name) {
    for (size_t i = 0; c && name && i < c->n; i++)
        if (!strcmp(c->v[i].name, name)) return &c->v[i];
    return NULL;
}

/* ---------- prompt templates ---------- */

static int cmp_depth(const void *a, const void *b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    size_t dx = 0, dy = 0;
    for (const char *p = x; *p; p++) dx += *p == '/';
    for (const char *p = y; *p; p++) dy += *p == '/';
    if (dx != dy) return dx < dy ? -1 : 1;
    return strcmp(x, y);
}

static void load_templates_dir(prompt_tpls_t *out, const char *dir, const char *source) {
    strvec_t rels = {0};
    collect_md(dir, "", true, &rels);
    if (rels.n > 1) qsort(rels.v, rels.n, sizeof(char *), cmp_depth);
    for (size_t i = 0; i < rels.n; i++) {
        char *p = path_join(dir, rels.v[i]);
        char *raw = read_text(p);
        free(p);
        if (!raw) continue;
        char *body;
        json_t *fm = parse_fm(raw, &body);
        free(raw);
        const char *base = path_basename(rels.v[i]);
        char *name = xstrndup(base, strlen(base) - 3);
        char *sub = path_dirname(rels.v[i]);
        if (!strcmp(sub, ".")) sub[0] = 0;
        for (char *c = sub; *c; c++)
            if (*c == '/') *c = ':';
        char *src = *sub ? xasprintf("(%s:%s)", source, sub) : xasprintf("(%s)", source);
        const json_t *d = json_get(fm, "description");
        char *desc = json_is_str(d) && *json_sv(d) ? xstrdup(json_sv(d)) : first_line_description(body);
        char *full = *desc ? xasprintf("%s %s", desc, src) : xstrdup(src);
        prompt_tpl_t t = {name, full, body};
        vec_push(out, t);
        free(desc);
        free(src);
        free(sub);
        json_free(fm);
    }
    strvec_free(&rels);
}

void prompt_templates_load(const char *cwd, prompt_tpls_t *out) {
    memset(out, 0, sizeof *out);
    char *u = omc_agent_path("prompts");
    load_templates_dir(out, u, "user");
    free(u);
    const char *pd[] = {".omc", ".omp"};
    for (size_t k = 0; k < 2; k++) {
        char *d = path_join(cwd, pd[k]);
        bool ok = fs_is_dir(d);
        if (ok) {
            char *pp = path_join(d, "prompts");
            load_templates_dir(out, pp, "project");
            free(pp);
        }
        free(d);
        if (ok) break;
    }
}

void prompt_templates_free(prompt_tpls_t *t) {
    for (size_t i = 0; i < t->n; i++) {
        free(t->v[i].name);
        free(t->v[i].description);
        free(t->v[i].content);
    }
    vec_free(t);
}

/* ---------- argument substitution ---------- */

strvec_t commands_parse_args(const char *s) {
    strvec_t args = {0};
    sb_t cur = SB_INIT;
    char quote = 0;
    for (const char *p = s ? s : ""; *p; p++) {
        char c = *p;
        if (quote) {
            if (c == quote) quote = 0;
            else sb_putc(&cur, c);
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == ' ' || c == '\t') {
            if (cur.len) {
                strvec_push(&args, sb_cstr(&cur));
                sb_clear(&cur);
            }
        } else sb_putc(&cur, c);
    }
    if (cur.len) strvec_push(&args, sb_cstr(&cur));
    sb_free(&cur);
    return args;
}

static void join_args(sb_t *b, const strvec_t *args, size_t from, size_t to) {
    for (size_t i = from; i < to && i < args->n; i++) {
        if (i > from) sb_putc(b, ' ');
        sb_append(b, args->v[i]);
    }
}

char *commands_substitute_args(const char *content, const strvec_t *args) {
    sb_t b = SB_INIT;
    for (const char *p = content; *p;) {
        if (*p != '$') {
            sb_putc(&b, *p++);
            continue;
        }
        /* $@[start(:len?)?] */
        if (p[1] == '@' && p[2] == '[' && isdigit((unsigned char)p[3])) {
            const char *q = p + 3;
            long start = 0;
            while (isdigit((unsigned char)*q)) start = start * 10 + (*q++ - '0');
            bool has_len = false, len_empty = false;
            long len = 0;
            bool ok = true;
            if (*q == ':') {
                q++;
                has_len = true;
                if (!isdigit((unsigned char)*q)) len_empty = true;
                while (isdigit((unsigned char)*q)) len = len * 10 + (*q++ - '0');
            }
            if (*q != ']') ok = false;
            if (ok) {
                if (start >= 1 && (size_t)(start - 1) < args->n) {
                    if (!has_len || len_empty) join_args(&b, args, (size_t)start - 1, args->n);
                    else if (len > 0) join_args(&b, args, (size_t)start - 1, (size_t)(start - 1 + len));
                }
                p = q + 1;
                continue;
            }
        }
        if (!strncmp(p, "$ARGUMENTS", 10)) {
            join_args(&b, args, 0, args->n);
            p += 10;
            continue;
        }
        if (p[1] == '@') {
            join_args(&b, args, 0, args->n);
            p += 2;
            continue;
        }
        if (isdigit((unsigned char)p[1])) {
            const char *q = p + 1;
            long n = 0;
            while (isdigit((unsigned char)*q)) {
                if (n < 100000000) n = n * 10 + (*q - '0');
                q++;
            }
            if (n >= 1 && (size_t)n <= args->n) sb_append(&b, args->v[n - 1]);
            p = q;
            continue;
        }
        sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

/* INLINE_ARG_SHELL_PATTERN / INLINE_ARG_TEMPLATE_PATTERN */
static bool word_at(const char *s, const char *start, const char *w) {
    size_t n = strlen(w);
    if (strncmp(s, w, n)) return false;
    bool lb = s == start || !(isalnum((unsigned char)s[-1]) || s[-1] == '_');
    bool rb = !(isalnum((unsigned char)s[n]) || s[n] == '_');
    return lb && rb;
}

static bool uses_inline_placeholders(const char *t) {
    for (const char *p = t; (p = strchr(p, '$')); p++) {
        if (!strncmp(p + 1, "ARGUMENTS", 9) || p[1] == '@' || isdigit((unsigned char)p[1])) return true;
    }
    for (const char *p = t; (p = strstr(p, "{{")); p += 2) {
        const char *e = strstr(p + 2, "}}");
        if (!e) break;
        for (const char *q = p + 2; q < e; q++) {
            if (word_at(q, p + 2, "arguments") || word_at(q, p + 2, "ARGUMENTS") || word_at(q, p + 2, "args")) return true;
            if (word_at(q, p + 2, "arg") && isspace((unsigned char)q[3])) {
                const char *r = q + 3;
                while (r < e && isspace((unsigned char)*r)) r++;
                if (r < e && *r != '}') return true;
            }
        }
    }
    return false;
}

char *commands_render(const char *content, const char *args_string) {
    strvec_t args = commands_parse_args(args_string);
    sb_t at = SB_INIT;
    join_args(&at, &args, 0, args.n);
    bool inline_ph = uses_inline_placeholders(content);
    char *sub = commands_substitute_args(content, &args);
    json_t *ctx = json_obj(), *arr = json_arr();
    for (size_t i = 0; i < args.n; i++) json_push_str(arr, args.v[i]);
    json_set(ctx, "args", arr);
    json_set_str(ctx, "ARGUMENTS", sb_cstr(&at));
    json_set_str(ctx, "arguments", sb_cstr(&at));
    char *r = tpl_render(sub, ctx);
    json_free(ctx);
    free(sub);
    if (!r) r = xstrdup("");
    char *out;
    if (!at.len || inline_ph) out = r;
    else if (!*r) {
        out = xstrdup(sb_cstr(&at));
        free(r);
    } else {
        out = xasprintf("%s\n\n%s", r, sb_cstr(&at));
        free(r);
    }
    sb_free(&at);
    strvec_free(&args);
    return out;
}

bool commands_parse_invocation(const char *text, char **name, char **args) {
    if (!text || text[0] != '/') return false;
    const char *sp = strchr(text, ' ');
    *name = sp ? xstrndup(text + 1, (size_t)(sp - text - 1)) : xstrdup(text + 1);
    *args = xstrdup(sp ? sp + 1 : "");
    return true;
}

char *commands_expand(const file_cmds_t *c, const char *text) {
    char *name, *args;
    if (!commands_parse_invocation(text, &name, &args)) return NULL;
    const file_cmd_t *f = commands_find(c, name);
    char *r = f ? commands_render(f->content, args) : NULL;
    free(name);
    free(args);
    return r;
}

char *prompt_templates_expand(const prompt_tpls_t *t, const char *text) {
    char *name, *args;
    if (!commands_parse_invocation(text, &name, &args)) return NULL;
    char *r = NULL;
    for (size_t i = 0; t && i < t->n && !r; i++)
        if (!strcmp(t->v[i].name, name)) r = commands_render(t->v[i].content, args);
    free(name);
    free(args);
    return r;
}

/* ---------- /skill:<name> ---------- */

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* startsWithLocalExecutionPrefix (on left-trimmed text) */
static bool local_exec_prefix(const char *t) {
    if (t[0] == '!') return true;
    if (t[0] != '$') return false;
    if (t[1] == '{') return false;
    size_t sig = t[1] == '$' ? 2 : 1;
    char next = t[sig];
    return !next || next == ' ' || next == '\t' || next == '\n' || next == '\r';
}

bool skill_tokens_allowed(const char *text) {
    const char *t = skip_ws(text ? text : "");
    if (str_starts(t, "/skill:")) return true;
    if (t[0] == '/') return false;
    return !local_exec_prefix(t);
}

/* SKILL_TOKEN_RE = /(^|\s)\/skill:([^\s/]+(?:\/[^\s/]+)?)(?=\s|$)/ */
bool skill_token_next(const char *text, size_t from, size_t *start, size_t *end, char **name) {
    size_t len = strlen(text);
    for (size_t i = from; i + 7 <= len; i++) {
        if (strncmp(text + i, "/skill:", 7)) continue;
        if (i > 0 && !isspace((unsigned char)text[i - 1])) continue;
        size_t j = i + 7, seg = j;
        while (j < len && !isspace((unsigned char)text[j]) && text[j] != '/') j++;
        if (j == seg) continue;
        if (j < len && text[j] == '/') {
            size_t k = j + 1;
            while (k < len && !isspace((unsigned char)text[k]) && text[k] != '/') k++;
            if (k == j + 1 || (k < len && text[k] == '/')) continue;
            j = k;
        }
        if (j < len && !isspace((unsigned char)text[j])) continue;
        *start = i;
        *end = j;
        *name = xstrndup(text + i + 7, j - i - 7);
        return true;
    }
    return false;
}

static char *trim_end_dup(const char *s, size_t n) {
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

bool skill_parse_invocation(const char *text, skill_invocation_t *out) {
    memset(out, 0, sizeof *out);
    if (!text) return false;
    const char *ts = skip_ws(text);
    if (str_starts(ts, "/skill:")) {
        size_t sp = 0;
        while (ts[sp] && !isspace((unsigned char)ts[sp])) sp++;
        if (sp == 7) return false;
        out->name = xstrndup(ts + 7, sp - 7);
        out->args = ts[sp] ? str_trim(ts + sp + 1) : xstrdup("");
        out->prompt = trim_end_dup(ts, strlen(ts));
        return true;
    }
    if (!skill_tokens_allowed(ts)) return false;
    size_t s, e;
    char *name;
    if (!skill_token_next(text, 0, &s, &e, &name)) return false;
    char *before = trim_end_dup(text, s);
    const char *after = skip_ws(text + e);
    char *joined = xasprintf("%s%s%s", before, *before && *after ? " " : "", after);
    out->name = name;
    out->args = str_trim(joined);
    out->prompt = trim_end_dup(ts, strlen(ts));
    free(joined);
    free(before);
    return true;
}

void skill_invocation_free(skill_invocation_t *inv) {
    free(inv->name);
    free(inv->args);
    free(inv->prompt);
    memset(inv, 0, sizeof *inv);
}

int skill_build_user_prompt(const skill_t *s, const skill_invocation_t *inv, char **message, json_t **details, char **err) {
    *message = NULL;
    *details = NULL;
    size_t n;
    char *raw = fs_read(s->path, &n);
    if (!raw) {
        if (err) *err = xasprintf("ENOENT: no such file or directory, open '%s'", s->path);
        return -1;
    }
    /* loadSkillPromptBody: parseFrontmatter(repair: false).body.trim() */
    char *nb;
    json_t *fm = parse_fm(raw, &nb);
    json_free(fm);
    free(raw);
    char *body = str_trim(nb);
    free(nb);
    char *args = str_trim(inv->args ? inv->args : "");
    json_t *ctx = json_obj();
    json_set_str(ctx, "name", s->name);
    json_set_str(ctx, "body", body);
    json_set_str(ctx, "baseDir", s->dir);
    if (*args) json_set_str(ctx, "userArgs", args);
    char *tpl = prompt_load("coding-agent/prompts/skills/user-invocation.md");
    char *r = tpl ? tpl_render(tpl, ctx) : xstrdup(body);
    free(tpl);
    json_free(ctx);
    *message = str_trim(r ? r : "");
    free(r);
    json_t *d = json_obj();
    json_set_str(d, "name", s->name);
    json_set_str(d, "path", s->path);
    if (*args) json_set_str(d, "args", args);
    if (inv->prompt) json_set_str(d, "prompt", inv->prompt);
    int lines = 0;
    if (*body) {
        lines = 1;
        for (const char *p = body; *p; p++) lines += *p == '\n';
    }
    json_set_int(d, "lineCount", lines);
    *details = d;
    free(args);
    free(body);
    return 0;
}
