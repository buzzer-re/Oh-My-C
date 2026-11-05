#include "tools.h"
#include "coding/memory.h"
#include "jobs.h"
#include "artifacts.h"
#include "config/settings.h"

#include "core/fs.h"
#include "prompt/template.h"
#include "shell.h"
#include "iurl.h"

/* ---------- env ---------- */

tool_env_t *tool_env_new(const char *cwd) {
    tool_env_t *e = xcalloc(1, sizeof(*e));
    e->cwd = cwd ? path_abs(cwd, NULL) : get_cwd();
    edit_store_init(&e->store);
    e->todo.phases = json_arr();
    e->edit_mode = xstrdup("hashline");
    hmap_init(&e->read_repeats);
    pthread_mutex_init(&e->mu, NULL);
    return e;
}

void tool_env_free(tool_env_t *e) {
    if (!e) return;
    free(e->cwd);
    edit_store_free(&e->store);
    json_free(e->todo.phases);
    free(e->edit_mode);
    artifacts_unref(e->artifacts);
    free(e->local_dir);
    shell_free(e->shell);
    jobs_free(e->jobs);
    conflict_history_free(e->conflicts);
    hmap_free(&e->read_repeats, free);
    pthread_mutex_destroy(&e->mu);
    free(e);
}

char *tool_resolve_path(tool_env_t *env, const char *p) {
    if (p && str_starts(p, "local://")) {
        const char *rest = p + 8;
        char *root = env->local_dir ? xstrdup(env->local_dir) : xasprintf("%s/.omc-local", env->cwd);
        fs_mkdir_p(root, 0700);
        char *j = path_join(root, rest);
        char *n = path_normalize(j);
        free(j);
        if (!str_starts(n, root)) { /* escape attempt: clamp to root */
            free(n);
            n = xstrdup(root);
        }
        free(root);
        return n;
    }
    if (p && str_starts(p, "ssh://")) return xstrdup(p); /* remote: resolved by ssh.c */
    if (!p || !*p || strcmp(p, "/") == 0) return xstrdup(env->cwd); /* omp: bare "/" means session cwd */
    if (str_starts(p, "file://")) p += 7;
    return path_abs(p, env->cwd);
}

char *tool_resolve_internal_url(tool_env_t *env, const char *url, char **err) {
    *err = NULL;
    if (str_starts(url, "attachment://")) { /* attachment-protocol.ts: the image file behind `attachment://N` */
        char *uri = xstrndup(url, strcspn(url, "?#"));
        json_t *list = env->image_attachments ? env->image_attachments(env) : NULL, *e;
        char *path = NULL;
        sb_t avail = SB_INIT;
        json_arr_foreach(list, i, e) {
            if (!path && str_eq(json_get_str(e, "uri"), uri)) path = xstrdup(json_get_str(e, "sourcePath"));
            sb_appendf(&avail, "%s%s", avail.len ? ", " : "", json_get_str(e, "uri"));
        }
        if (!path)
            *err = xasprintf("Could not resolve image attachment '%s'. Available attachment URIs: %s. Use one of the listed "
                             "attachment URIs, or attach an image first when none are available.",
                             uri, avail.len ? avail.s : "none");
        sb_free(&avail);
        json_free(list);
        free(uri);
        return path;
    }
    if (str_starts(url, "memory://")) { /* file-backed project memory (coding/memory.c) */
        const char *colon = strchr(url + 9, ':');
        char *u = colon ? xstrndup(url, (size_t)(colon - url)) : xstrdup(url);
        char *p = memory_url_resolve(env->cwd, u, err);
        free(u);
        if (!p) return NULL;
        char *r = xasprintf("%s%s", p, colon ? colon : "");
        free(p);
        return r;
    }
    const char *schemes[] = {"skill://", "rule://", "artifact://"};
    size_t si = ARRAY_LEN(schemes);
    for (size_t i = 0; i < ARRAY_LEN(schemes); i++)
        if (str_starts(url, schemes[i])) si = i;
    if (si == ARRAY_LEN(schemes)) return NULL;
    const char *rest = url + strlen(schemes[si]);
    /* split off a trailing :selector */
    const char *colon = strchr(rest, ':');
    char *target = colon ? xstrndup(rest, (size_t)(colon - rest)) : xstrdup(rest);
    const char *sel = colon ? colon : "";
    char *path = NULL;
    if (si == 0) {
        char *slash = strchr(target, '/');
        if (slash) *slash = 0;
        const skill_t *sk = discovery_find_skill(env->disc, target);
        if (!sk) *err = xasprintf("Unknown skill: %s", target);
        else if (slash && slash[1]) {
            char *p = path_join(sk->dir, slash + 1);
            char *n = path_normalize(p);
            free(p);
            if (!str_starts(n, sk->dir)) *err = xasprintf("Path escapes skill directory: %s", url);
            else path = xstrdup(n);
            free(n);
        } else path = xstrdup(sk->path);
    } else if (si == 1) {
        const rule_t *r = discovery_find_rule(env->disc, target);
        if (!r) *err = xasprintf("Unknown rule: %s", target);
        else path = xstrdup(r->path);
    } else {
        if (strchr(target, '/') || strstr(target, "..")) *err = xasprintf("Invalid artifact id: %s", target);
        else {
            path = artifacts_find(env->artifacts, target);
            if (!path) *err = xasprintf("Artifact not found: %s", target);
        }
    }
    free(target);
    if (!path) return NULL;
    char *r = xasprintf("%s%s", path, sel);
    free(path);
    return r;
}

/* ---------- truncation ---------- */

char *tool_truncate_tail(const char *s, size_t len, size_t max_bytes, size_t max_lines, bool *truncated) {
    *truncated = false;
    size_t start = 0;
    if (max_lines) {
        size_t lines = 0;
        for (size_t i = len; i > 0; i--) {
            if (s[i - 1] == '\n' && i != len) {
                if (++lines >= max_lines) {
                    start = i;
                    *truncated = true;
                    break;
                }
            }
        }
    }
    if (max_bytes && len - start > max_bytes) {
        start = len - max_bytes;
        while (start < len && ((unsigned char)s[start] & 0xC0) == 0x80) start++;
        const char *nl = memchr(s + start, '\n', len - start);
        if (nl && (size_t)(nl - s) < len - 1) start = (size_t)(nl - s) + 1;
        *truncated = true;
    }
    return xstrndup(s + start, len - start);
}

char *tool_truncate_head(const char *s, size_t len, size_t max_bytes, size_t max_lines, bool *truncated) {
    *truncated = false;
    size_t end = len;
    if (max_lines) {
        size_t lines = 0;
        for (size_t i = 0; i < len; i++) {
            if (s[i] == '\n' && ++lines >= max_lines) {
                end = i;
                *truncated = i + 1 < len;
                break;
            }
        }
    }
    if (max_bytes && end > max_bytes) {
        end = max_bytes;
        while (end > 0 && ((unsigned char)s[end] & 0xC0) == 0x80) end--;
        *truncated = true;
    }
    return xstrndup(s, end);
}

/* ---------- schemas ---------- */

json_t *schema_obj(void) {
    json_t *s = json_obj();
    json_set_str(s, "type", "object");
    json_set(s, "properties", json_obj());
    json_set(s, "required", json_arr());
    return s;
}

json_t *schema_lit(const char *json) {
    json_t *s = json_parse_str(json);
    if (!s) {
        fprintf(stderr, "omc: internal error: malformed tool schema literal: %s\n", json);
        abort();
    }
    return s;
}

void schema_prop_json(json_t *schema, const char *name, json_t *prop, bool required) {
    json_set(json_get(schema, "properties"), name, prop);
    if (required) json_push_str(json_get(schema, "required"), name);
}

void schema_prop(json_t *schema, const char *name, const char *type, const char *desc, bool required) {
    json_t *p = json_obj();
    json_set_str(p, "type", type);
    if (desc) json_set_str(p, "description", desc);
    schema_prop_json(schema, name, p, required);
}

char *tool_prompt(const char *name, const json_t *ctx) {
    char *rel = xasprintf("coding-agent/prompts/tools/%s.md", name);
    char *r = prompt_render(rel, ctx);
    free(rel);
    /* product name: omp's harness docs scheme is served as omc:// here */
    if (r && strstr(r, "omp://")) {
        char *x = str_replace(r, "omp://", "omc://");
        free(r);
        r = x;
    }
    return r;
}

void format_hashline_header(sb_t *b, const char *display_path, const char *tag) {
    sb_appendf(b, "[%s#%s]\n", display_path, tag);
}

/* ---------- registration ---------- */

/* Built-in tool order (upstream tools/index.ts BUILTIN_TOOLS key order); the request's tool list and the
 * xd:// device listing follow it. Names omc does not implement are kept so the order stays canonical. */
static const char *const BUILTIN_ORDER[] = {
    "read",     "security_scan", "bash",       "edit",    "ast_grep",  "ast_edit", "ask",    "debug",
    "ida",      "eval",          "github",     "glob",    "grep",      "find",     "lsp",    "checkpoint",
    "rewind",   "context_notes", "new_context", "task",   "wait",      "todo",     "web_search", "write",
    "memory_edit", "retain",     "recall",     "reflect", "learn",     "manage_skill", NULL};

/* upstream loadMode "discoverable" built-ins (mountable under xd:// unless kept top-level) */
static const char *const DISCOVERABLE[] = {"security_scan", "ast_grep", "ast_edit", "ask",      "debug",  "ida",
                                           "github",        "grep",     "lsp",      "checkpoint", "rewind", "todo",
                                           "web_search",    "memory_edit", "retain", "recall", "reflect", NULL};
/* discoverable built-ins that stay top-level (xdev.ts XDEV_KEEP_TOP_LEVEL) + the read/write transport */
static const char *const KEEP_TOP_LEVEL[] = {"todo", "yield", "ask", "grep", "web_search", "read", "write", NULL};

static bool in_list(const char *const *l, const char *n) {
    for (; *l; l++)
        if (!strcmp(*l, n)) return true;
    return false;
}

const char *const *tools_default_names(void) { return BUILTIN_ORDER; }

static bool setting_on(const char *path, bool def) { return settings_bool(path, def); }

/* upstream resolveBuiltinToolPlan().isToolAllowed for the tools constructed here */
static bool builtin_allowed(tool_env_t *env, const char *name) {
    if (!strcmp(name, "bash")) return setting_on("bash.enabled", true);
    if (!strcmp(name, "todo")) return setting_on("todo.enabled", true);
    if (!strcmp(name, "glob")) return setting_on("glob.enabled", true);
    if (!strcmp(name, "grep")) return setting_on("grep.enabled", true);
    if (!strcmp(name, "ast_grep")) return setting_on("astGrep.enabled", false);
    if (!strcmp(name, "ast_edit")) return setting_on("astEdit.enabled", true);
    if (!strcmp(name, "web_search")) return setting_on("web_search.enabled", true);
    if (!strcmp(name, "ask")) return setting_on("ask.enabled", true) && (env->can_prompt_user || env->ask_ui);
    if (!strcmp(name, "find")) {
        /* "auto" needs a native judgment backend (TypeSafe jev / Decisions APIs), which omc lacks */
        json_t *v = settings_get("find.enabled");
        if (json_is_bool(v)) return v->u.b;
        return str_eq(json_sv(v), "on");
    }
    if (!strcmp(name, "wait"))
        return env->jobs && (setting_on("async.enabled", true) || setting_on("launch.enabled", true));
    if (!strcmp(name, "task"))
        return env->subagent_depth < (int)settings_int("task.maxRecursionDepth", 2) ||
               settings_int("task.maxRecursionDepth", 2) < 0;
    return true;
}

typedef agent_tool_t *(*tool_ctor)(tool_env_t *);
static const struct {
    const char *name;
    tool_ctor ctor;
} CTORS[] = {
    {"read", tool_read_new},       {"bash", tool_bash_new},   {"edit", tool_edit_new},
    {"ast_grep", tool_ast_grep_new}, {"ast_edit", tool_ast_edit_new}, {"ask", tool_ask_new},
    {"eval", tool_eval_new},       {"glob", tool_glob_new},   {"grep", tool_grep_new},
    {"find", tool_find_new},       {"task", tool_task_new},   {"wait", tool_wait_new},
    {"todo", tool_todo_new},       {"web_search", tool_web_search_new}, {"write", tool_write_new},
};

bool tools_scout_available(void) {
    json_t *dis = settings_get("task.disabledAgents"), *d;
    json_arr_foreach(dis, i, d) if (str_eq(json_sv(d), "scout")) return false;
    return true;
}

bool tools_eager_delegation(const tool_env_t *env) {
    return !env->delegation_bias || str_eq(env->delegation_bias, "eager");
}

void tools_register_builtin(agent_t *a, tool_env_t *env, const strvec_t *enabled) {
    env->find_enabled = (!enabled || strvec_contains(enabled, "find")) && builtin_allowed(env, "find");
    for (size_t i = 0; i < ARRAY_LEN(CTORS); i++) {
        if (enabled && !strvec_contains(enabled, CTORS[i].name)) continue;
        if (!builtin_allowed(env, CTORS[i].name)) continue;
        agent_tool_t *t = CTORS[i].ctor(env);
        if (t) {
            t->discoverable = in_list(DISCOVERABLE, t->name);
            agent_add_tool(a, t);
        }
    }
}

/* upstream tool `label` / `summary` (catalog line for xd:// devices) */
static const struct {
    const char *name, *label, *summary;
} TOOL_META[] = {
    {"read", "Read", NULL},
    {"bash", "Bash", NULL},
    {"edit", "Edit", NULL},
    {"ast_grep", "AST Grep", "Search code with AST patterns (structural grep)"},
    {"ast_edit", "AST Edit", "Perform AST-aware code edits (structural refactoring)"},
    {"ask", "Ask", "Ask the user a clarifying question"},
    {"debug", "Debug", "Debug a running process with DAP (debugger adapter protocol)"},
    {"eval", "Eval", NULL},
    {"github", "GitHub", "Interact with GitHub repositories, files, pull requests, and Actions"},
    {"glob", "Glob", NULL},
    {"grep", "Grep", "Search file contents by regex"},
    {"find", "Find", "Semantic grep: find files and line ranges by describing what they do"},
    {"lsp", "LSP", "Query LSP (language server) for diagnostics, hover info, and references"},
    {"checkpoint", "Checkpoint", "Create a git-based checkpoint to save and restore session state"},
    {"rewind", "Rewind", "Rewind to a previously created checkpoint"},
    {"task", "Task", NULL},
    {"wait", "Wait", "Wait for the next result of a background job or service you started, or a reply you asked for"},
    {"todo", "Todo", "Write a structured todo list to track progress within a session"},
    {"web_search", "Web Search", NULL},
    {"write", "Write", NULL},
};

static void apply_meta(agent_tool_t *t) {
    for (size_t i = 0; i < ARRAY_LEN(TOOL_META); i++) {
        if (strcmp(TOOL_META[i].name, t->name) != 0) continue;
        free(t->label);
        t->label = xstrdup(TOOL_META[i].label);
        if (TOOL_META[i].summary && !t->summary) t->summary = xstrdup(TOOL_META[i].summary);
        return;
    }
}

static int order_index(const agent_tool_t *t) {
    for (int i = 0; BUILTIN_ORDER[i]; i++)
        if (!strcmp(BUILTIN_ORDER[i], t->name)) return i;
    return 1000; /* MCP / host / hidden tools keep their relative order after the built-ins */
}

void tools_finalize(agent_t *a, const strvec_t *requested) {
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->all_tools.n; i++) apply_meta(a->all_tools.v[i]);
    /* stable sort all_tools by canonical order */
    for (size_t i = 1; i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        int k = order_index(t);
        size_t j = i;
        while (j > 0 && order_index(a->all_tools.v[j - 1]) > k) {
            a->all_tools.v[j] = a->all_tools.v[j - 1];
            j--;
        }
        a->all_tools.v[j] = t;
    }
    /* xd:// mounting: needs both transport halves; explicitly requested tools stay top-level */
    bool have_read = false, have_write = false;
    for (size_t i = 0; i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        if (t->hidden && !t->mounted) continue;
        if (!strcmp(t->name, "read")) have_read = true;
        if (!strcmp(t->name, "write")) have_write = true;
    }
    bool xdev = settings_bool("tools.xdev", true) && have_read && have_write;
    for (size_t i = 0; i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        if (t->mounted) {
            t->mounted = false;
            t->hidden = false;
        }
        if (t->hidden) continue;
        bool pinned = requested && strvec_contains(requested, t->name);
        if (xdev && t->discoverable && !pinned && !in_list(KEEP_TOP_LEVEL, t->name)) {
            t->mounted = true;
            t->hidden = true;
        }
    }
    a->tools.n = 0;
    for (size_t i = 0; i < a->all_tools.n; i++)
        if (!a->all_tools.v[i]->hidden) vec_push(&a->tools, a->all_tools.v[i]);
    /* eval's bridge reaches every session tool: top-level ones, then xd:// devices */
    agent_tool_t *ev = NULL;
    json_t *names = json_arr();
    for (size_t i = 0; i < a->tools.n; i++) {
        json_push_str(names, a->tools.v[i]->name);
        if (!strcmp(a->tools.v[i]->name, "eval")) ev = a->tools.v[i];
    }
    for (size_t i = 0; i < a->all_tools.n; i++)
        if (a->all_tools.v[i]->mounted) json_push_str(names, a->all_tools.v[i]->name);
    if (ev) eval_set_tool_names(ev, names);
    json_free(names);
    pthread_mutex_unlock(&a->mu);
}
