/* sub_misc.c - `omc agents unpack` (cli/agents-cli.ts), `omc ssh add|remove|list` (cli/ssh-cli.ts),
 * `omc setup [python]` (commands/setup.ts, cli/setup-cli.ts), `omc completions <shell>` (cli/completion-gen.ts)
 * and the hidden `omc __complete <kind>` helper (commands/complete.ts). */
#include "cli/cli_int.h"

#include "ai/ai.h"
#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "prompt/template.h"
#include "session/session.h"
#include "text/yaml.h"
#include "tools/tools.h"
#include "tui/symbols.h"

#include <unistd.h>

static void out_line(char *colored) {
    printf("%s\n", colored);
    free(colored);
}

/* ---------- agents unpack ---------- */

/* parseAgentFields: comma lists become arrays, a model string a one-element list */
static json_t *split_list(const json_t *v) {
    json_t *arr = json_arr();
    if (json_is_arr(v)) {
        json_t *x;
        json_arr_foreach(v, i, x) if (json_is_str(x)) json_push_str(arr, json_sv(x));
    } else if (json_is_str(v)) {
        strvec_t parts = str_split(json_sv(v), ",");
        for (size_t i = 0; i < parts.n; i++) {
            char *t = str_trim(parts.v[i]);
            if (*t) json_push_str(arr, t);
            free(t);
        }
        strvec_free(&parts);
    }
    return arr;
}

/* one bundled agent as omp serializes it: `---\n<YAML frontmatter>\n---\n\n<body>\n` */
static char *serialize_agent(const json_t *fm_src, const char *body) {
    json_t *fm = json_obj();
    json_set_str(fm, "name", json_get_str(fm_src, "name"));
    json_set_str(fm, "description", json_get_str(fm_src, "description"));
    if (json_get(fm_src, "tools")) json_set(fm, "tools", split_list(json_get(fm_src, "tools")));
    const json_t *sp = json_get(fm_src, "spawns");
    if (json_is_str(sp) && !strcmp(json_sv(sp), "*")) json_set_str(fm, "spawns", "*");
    else if (sp) json_set(fm, "spawns", split_list(sp));
    const json_t *model = json_get(fm_src, "model");
    if (model) {
        json_t *ml = split_list(model);
        if (json_len(ml)) json_set(fm, "model", ml);
        else json_free(ml);
    }
    const char *tl = json_get_str(fm_src, "thinking-level");
    if (!tl) tl = json_get_str(fm_src, "thinkingLevel");
    if (tl) json_set_str(fm, "thinkingLevel", tl);
    if (json_get(fm_src, "output")) json_set(fm, "output", json_clone(json_get(fm_src, "output")));
    if (json_get_bool(fm_src, "blocking", false)) json_set_bool(fm, "blocking", true);
    if (json_get_bool(fm_src, "structured", false)) json_set_bool(fm, "structured", true);
    char *y = yaml_dump(fm);
    json_free(fm);
    size_t yl = strlen(y);
    while (yl && (y[yl - 1] == '\n' || y[yl - 1] == ' ')) y[--yl] = 0;
    char *b = str_trim(body);
    char *s = xasprintf("---\n%s\n---\n\n%s\n", y, b);
    free(b);
    free(y);
    return s;
}

/* loadBundledAgents: (frontmatter, rendered body) pairs, sorted by name */
static json_t *bundled_agents(void) {
    json_t *out = json_arr();
    static const char *const md[] = {"reviewer", "scout", "security-reviewer"};
    for (size_t i = 0; i < ARRAY_LEN(md); i++) {
        char *rel = xasprintf("coding-agent/prompts/agents/%s.md", md[i]);
        char *t = prompt_load(rel);
        free(rel);
        if (!t) continue;
        const char *body = NULL;
        json_t *fm = yaml_frontmatter(t, strlen(t), &body, NULL);
        char *rb = tpl_render(body ? body : "", NULL);
        json_t *e = json_obj();
        json_set(e, "fm", fm ? fm : json_obj());
        json_set_str(e, "body", rb);
        json_push(out, e);
        free(rb);
        free(t);
    }
    char *task = prompt_load("coding-agent/prompts/agents/task.md");
    char *tb = task ? tpl_render(task, NULL) : xstrdup("");
    static const struct {
        const char *name, *desc, *spawns, *model, *thinking;
    } synth[] = {{"sonic", "Low-reasoning agent for strictly mechanical updates or data collection only", NULL, "@smol", "medium"},
                 {"task", "General-purpose subagent with full capabilities for delegated multi-step tasks", "*", "@task", "auto"}};
    for (size_t i = 0; i < ARRAY_LEN(synth); i++) {
        json_t *fm = json_obj();
        json_set_str(fm, "name", synth[i].name);
        json_set_str(fm, "description", synth[i].desc);
        if (synth[i].spawns) json_set_str(fm, "spawns", synth[i].spawns);
        json_set_str(fm, "model", synth[i].model);
        json_set_str(fm, "thinkingLevel", synth[i].thinking);
        json_t *e = json_obj();
        json_set(e, "fm", fm);
        json_set_str(e, "body", tb);
        json_push(out, e);
    }
    free(tb);
    free(task);
    return out; /* already in name order: reviewer scout security-reviewer sonic task */
}

int cmd_agents(cparse_t *p) {
    if (!cp_arg(p, "action")) {
        cli_render_command_help(stdout, p->cmd);
        return 0;
    }
    settings_init(NULL, NULL);
    char *cwd = get_cwd();
    char *dir = NULL;
    const char *d = cp_str(p, "dir");
    char *dt = d ? str_trim(d) : NULL;
    if (dt && *dt) dir = path_abs(dt, cwd);
    else if (cp_bool(p, "user") && cp_bool(p, "project")) {
        cli_err_line("Error: Choose either --user or --project, not both.");
        free(dt);
        free(cwd);
        return 1;
    } else if (cp_bool(p, "project")) dir = path_join3(cwd, ".omc", "agents");
    else dir = path_join(omc_agent_dir(), "agents");
    free(dt);
    free(cwd);
    fs_mkdir_p(dir, 0755);
    json_t *agents = bundled_agents(), *e;
    strvec_t written = {0}, skipped = {0};
    json_arr_foreach(agents, i, e) {
        const json_t *fm = json_get(e, "fm");
        char *fp = xasprintf("%s/%s.md", dir, json_get_str(fm, "name"));
        if (!cp_bool(p, "force") && fs_exists(fp)) {
            strvec_push_own(&skipped, fp);
            continue;
        }
        char *s = serialize_agent(fm, json_get_str(e, "body"));
        fs_write(fp, s, strlen(s));
        free(s);
        strvec_push_own(&written, fp);
    }
    size_t total = json_len(agents);
    json_free(agents);
    if (cp_bool(p, "json")) {
        json_t *o = json_obj(), *w = json_arr(), *k = json_arr();
        json_set_str(o, "targetDir", dir);
        json_set_num(o, "total", (double)total);
        for (size_t i = 0; i < written.n; i++) json_push_str(w, written.v[i]);
        for (size_t i = 0; i < skipped.n; i++) json_push_str(k, skipped.v[i]);
        json_set(o, "written", w);
        json_set(o, "skipped", k);
        char *s = cli_json_pretty(o);
        printf("%s\n", s);
        free(s);
        json_free(o);
    } else {
        char *t = xasprintf("Bundled agents: %zu", total);
        out_line(cli_bold(t));
        free(t);
        t = xasprintf("Target directory: %s", dir);
        out_line(cli_dim(t));
        free(t);
        t = xasprintf("%s Written: %zu", tui_sym("status.success"), written.n);
        out_line(cli_green(t));
        free(t);
        if (skipped.n) {
            t = xasprintf("%s Skipped existing: %zu (use --force to overwrite)", tui_sym("status.warning"), skipped.n);
            out_line(cli_yellow(t));
            free(t);
        }
        for (size_t i = 0; i < written.n; i++) {
            t = xasprintf("  + %s", written.v[i]);
            out_line(cli_dim(t));
            free(t);
        }
        for (size_t i = 0; i < skipped.n; i++) {
            t = xasprintf("  = %s", skipped.v[i]);
            out_line(cli_dim(t));
            free(t);
        }
    }
    strvec_free(&written);
    strvec_free(&skipped);
    free(dir);
    return 0;
}

/* ---------- ssh ---------- */

/* the shared ssh.json writer (tools/ssh.c) */
static char *cli_ssh_path(const char *scope) {
    char *cwd = get_cwd();
    char *p = ssh_config_path(scope, cwd);
    free(cwd);
    return p;
}

static void ssh_error(const char *msg) {
    char *m = xasprintf("Error: %s\n", msg), *r = cli_red(m);
    fputs(r, stdout);
    free(r);
    free(m);
}

static void print_hosts(const json_t *hosts) {
    const char *k;
    json_t *v;
    json_obj_foreach(hosts, i, k, v) {
        sb_t b = SB_INIT;
        char *c = cli_cyan(k);
        sb_appendf(&b, "  %s  %s", c, json_get_str(v, "host") ? json_get_str(v, "host") : "");
        free(c);
        const char *parts[4] = {0};
        char *port = NULL;
        parts[0] = json_get_str(v, "username");
        int64_t pn = json_d2i(json_get_num(v, "port", 0));
        if (pn && pn != 22) parts[1] = port = xasprintf("port:%lld", (long long)pn);
        parts[2] = json_get_str(v, "keyPath");
        char *desc = json_get_str(v, "description") ? xasprintf("- %s", json_get_str(v, "description")) : NULL;
        parts[3] = desc;
        for (int q = 0; q < 4; q++)
            if (parts[q]) {
                char *d = cli_dim(parts[q]);
                sb_appendf(&b, "  %s", d);
                free(d);
            }
        printf("%s\n", sb_cstr(&b));
        sb_free(&b);
        free(port);
        free(desc);
    }
}

int cmd_ssh(cparse_t *p) {
    const char *action = cp_arg(p, "action") ? cp_arg(p, "action") : "list";
    const strvec_t *targets = cp_arg_multi(p, "targets");
    const char *name = targets && targets->n ? targets->v[0] : NULL;
    const char *scope = cp_str(p, "scope") ? cp_str(p, "scope") : "project";
    settings_init(NULL, NULL);
    if (!strcmp(action, "list")) {
        char *pp = cli_ssh_path("project"), *up = cli_ssh_path("user"), *err = NULL;
        json_t *pc = ssh_config_read(pp, &err), *uc = err ? NULL : ssh_config_read(up, &err);
        int rc = 0;
        if (err) {
            ssh_error(err);
            rc = 1;
        } else {
            const json_t *ph = json_get_obj(pc, "hosts"), *uh = json_get_obj(uc, "hosts");
            if (cp_bool(p, "json")) {
                json_t *o = json_obj();
                json_set(o, "project", ph ? json_clone(ph) : json_obj());
                json_set(o, "user", uh ? json_clone(uh) : json_obj());
                char *s = cli_json_pretty(o);
                printf("%s\n", s);
                free(s);
                json_free(o);
            } else if (!json_len(ph) && !json_len(uh)) {
                out_line(cli_dim("No SSH hosts configured"));
                out_line(cli_dim("Add one with: omc ssh add <name> --host <address>"));
            } else {
                if (json_len(ph)) {
                    char *t = xasprintf("Project SSH Hosts (%s):", pp);
                    out_line(cli_bold(t));
                    free(t);
                    print_hosts(ph);
                }
                if (json_len(ph) && json_len(uh)) printf("\n");
                if (json_len(uh)) {
                    char *t = xasprintf("User SSH Hosts (%s):", up);
                    out_line(cli_bold(t));
                    free(t);
                    print_hosts(uh);
                }
            }
        }
        json_free(pc);
        json_free(uc);
        free(err);
        free(pp);
        free(up);
        return rc;
    }
    if (!name) {
        ssh_error("Host name required");
        char *u = !strcmp(action, "add") ? xstrdup("Usage: omc ssh add <name> --host <address> [--user <user>] [--port <port>] [--key <path>]\n")
                                          : xstrdup("Usage: omc ssh remove <name> [--scope project|user]\n");
        char *d = cli_dim(u);
        fputs(d, stdout);
        free(d);
        free(u);
        return 1;
    }
    char *path = cli_ssh_path(scope), *err = NULL;
    int rc = 0;
    if (!strcmp(action, "add")) {
        const char *host = cp_str(p, "host");
        if (!host) {
            ssh_error("--host is required");
            char *d = cli_dim("Usage: omc ssh add <name> --host <address>\n");
            fputs(d, stdout);
            free(d);
            free(path);
            return 1;
        }
        const char *port = cp_str(p, "port");
        long pn = 0;
        if (port) {
            char *end;
            pn = strtol(port, &end, 10);
            if (end == port || pn < 1 || pn > 65535) {
                ssh_error("Port must be an integer between 1 and 65535");
                free(path);
                return 1;
            }
        }
        json_t *h = json_obj();
        json_set_str(h, "host", host);
        if (cp_str(p, "user")) json_set_str(h, "username", cp_str(p, "user"));
        if (port) json_set_num(h, "port", (double)pn);
        if (cp_str(p, "key")) json_set_str(h, "keyPath", cp_str(p, "key"));
        if (cp_str(p, "desc")) json_set_str(h, "description", cp_str(p, "desc"));
        if (cp_bool(p, "compat")) json_set_bool(h, "compat", true);
        ssh_config_add(path, name, h, &err);
        json_free(h);
        if (err) {
            ssh_error(err);
            rc = 1;
        } else {
            char *m = xasprintf("Added SSH host \"%s\" to %s config", name, scope);
            out_line(cli_green(m));
            free(m);
        }
    } else {
        ssh_config_remove(path, name, &err);
        if (err) {
            ssh_error(err);
            rc = 1;
        } else {
            char *m = xasprintf("Removed SSH host \"%s\" from %s config", name, scope);
            out_line(cli_green(m));
            free(m);
        }
    }
    free(err);
    free(path);
    return rc;
}

/* ---------- setup ---------- */



static char *find_python(void) {
    const char *cfg = settings_str("python.interpreter", NULL);
    char *t = cfg ? str_trim(cfg) : NULL;
    if (t && *t) {
        char *r = which(t);
        if (!r && fs_is_exec(t)) r = xstrdup(t);
        free(t);
        return r;
    }
    free(t);
    const char *names[] = {"python3", "python"};
    for (size_t i = 0; i < ARRAY_LEN(names); i++) {
        char *p = which(names[i]);
        if (p) return p;
    }
    return NULL;
}

int cmd_setup(cparse_t *p) {
    const char *component = cp_arg(p, "component");
    bool json = cp_bool(p, "json"), check = cp_bool(p, "check");
    if (!component) {
        if (check || json) return cp_usage_error(p, "setup --check/--json requires a COMPONENT (python)");
        if (!isatty(0) || !isatty(1)) {
            fprintf(stderr, "omc setup requires an interactive TTY.\n");
            return 1;
        }
        return cmd_launch_setup();
    }
    char *cwd = get_cwd();
    settings_init(cwd, NULL);
    free(cwd);
    char *py = find_python();
    bool ok = false;
    if (py) {
        const char *argv[] = {py, "-c", "import sys;sys.exit(0)", NULL};
        proc_opts_t po = {.timeout_ms = 10000};
        proc_result_t pr;
        if (proc_run(argv, &po, &pr) == 0) {
            ok = pr.exit_code == 0;
            proc_result_free(&pr);
        }
    }
    char *managed = omc_agent_path("../python-env");
    char *mn = path_normalize(managed);
    free(managed);
    int rc = 0;
    if (json) {
        json_t *o = json_obj();
        json_set_bool(o, "available", ok);
        if (py) json_set_str(o, "pythonPath", py);
        json_set_bool(o, "usingManagedEnv", false);
        json_set_str(o, "managedEnvPath", mn);
        char *s = cli_json_pretty(o);
        printf("%s\n", s);
        free(s);
        json_free(o);
        rc = ok ? 0 : 1;
    } else if (!py) {
        char *m = xasprintf("%s Python not found", tui_sym("status.error"));
        cli_err_line(m);
        free(m);
        char *d = cli_dim("Install Python 3.8+ or set python.interpreter to its executable path");
        fprintf(stderr, "%s\n", d);
        free(d);
        rc = 1;
    } else {
        char *l = xasprintf("Python: %s", py);
        out_line(cli_dim(l));
        free(l);
        if (ok) {
            l = xasprintf("\n%s Python execution is ready", tui_sym("status.success"));
            out_line(cli_green(l));
            free(l);
        } else {
            l = xasprintf("\n%s Python interpreter reported failure", tui_sym("status.error"));
            cli_err_line(l);
            free(l);
            rc = 1;
        }
    }
    free(mn);
    free(py);
    return rc;
}

/* ---------- completions (cli/completion-gen.ts) ---------- */

typedef enum { VS_FLAG, VS_VALUE, VS_ENUM, VS_LIST, VS_MODELS, VS_MODELS_MULTI, VS_SESSIONS, VS_FILE, VS_DIR } vs_kind_t;

static const char *const TOOL_NAMES[] = {
    "read", "bash", "edit", "ast_grep", "ast_edit", "ask", "debug", "eval", "github", "glob", "grep", "find", "lsp",
    "checkpoint", "rewind", "context_notes", "new_context", "task", "wait", "todo", "web_search", "write",
    "memory_edit", "retain", "recall", "reflect", "learn", "manage_skill", NULL};

static vs_kind_t flag_value(const cflag_t *f) {
    if (f->kind == CF_BOOL) return VS_FLAG;
    if (f->options) return VS_ENUM;
    static const char *const MODEL[] = {"model", "smol", "slow", "plan", NULL}, *const SESS[] = {"resume", "restore", "fork", "session", NULL};
    for (int i = 0; MODEL[i]; i++)
        if (!strcmp(f->name, MODEL[i])) return VS_MODELS;
    if (!strcmp(f->name, "models")) return VS_MODELS_MULTI;
    for (int i = 0; SESS[i]; i++)
        if (!strcmp(f->name, SESS[i])) return VS_SESSIONS;
    if (!strcmp(f->name, "tools")) return VS_LIST;
    if (!strcmp(f->name, "session-dir") || !strcmp(f->name, "plugin-dir")) return VS_DIR;
    if (f->kind == CF_INT) return VS_VALUE;
    return VS_FILE;
}

static void words(sb_t *b, const char *const *v) {
    for (int i = 0; v[i]; i++) {
        if (i) sb_putc(b, ' ');
        for (const char *c = v[i]; *c; c++) {
            if (*c == '"') sb_putc(b, '\\');
            sb_putc(b, *c);
        }
    }
}

static void bash_value_branch(sb_t *b, const cflag_t *f) {
    switch (flag_value(f)) {
    case VS_FLAG:
    case VS_VALUE: sb_append(b, "return 0"); break;
    case VS_ENUM:
        sb_append(b, "COMPREPLY=( $(compgen -W \"");
        words(b, f->options);
        sb_append(b, "\" -- \"$cur\") ); return 0");
        break;
    case VS_LIST:
        sb_append(b, "_omc_comma \"");
        words(b, TOOL_NAMES);
        sb_append(b, "\"; return 0");
        break;
    case VS_MODELS_MULTI: sb_appendf(b, "_omc_comma \"$(command %s __complete models 2>/dev/null | cut -f1)\"; return 0", CLI_BIN); break;
    case VS_MODELS:
        sb_appendf(b, "COMPREPLY=( $(compgen -W \"$(command %s __complete models -- \"$cur\" 2>/dev/null | cut -f1)\" -- \"$cur\") ); return 0", CLI_BIN);
        break;
    case VS_SESSIONS:
        sb_appendf(b, "COMPREPLY=( $(compgen -W \"$(command %s __complete sessions -- \"$cur\" 2>/dev/null | cut -f1)\" -- \"$cur\") ); return 0", CLI_BIN);
        break;
    case VS_FILE: sb_append(b, "COMPREPLY=( $(compgen -f -- \"$cur\") ); compopt -o filenames; return 0"); break;
    case VS_DIR: sb_append(b, "COMPREPLY=( $(compgen -d -- \"$cur\") ); compopt -o filenames; return 0"); break;
    }
}

static void bash_flag_case(sb_t *b, const cflag_t *flags) {
    bool first = true;
    for (int i = 0; flags && flags[i].name; i++) {
        if (flags[i].kind == CF_BOOL) continue;
        if (!first) sb_putc(b, '\n');
        first = false;
        sb_appendf(b, "\t\t--%s", flags[i].name);
        if (flags[i].ch) sb_appendf(b, "|-%c", flags[i].ch);
        sb_append(b, ")\n\t\t\t");
        bash_value_branch(b, &flags[i]);
        sb_append(b, "\n\t\t\t;;");
    }
}

static void bash_flag_words(sb_t *b, const cflag_t *flags) {
    for (int i = 0; flags && flags[i].name; i++) {
        if (i) sb_putc(b, ' ');
        sb_appendf(b, "--%s", flags[i].name);
        if (flags[i].ch) sb_appendf(b, " -%c", flags[i].ch);
    }
}

static void fn_name(sb_t *b, const char *n) {
    for (const char *c = n; *c; c++) sb_putc(b, ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) ? *c : '_');
}

static int cmd_ptr_cmp(const void *x, const void *y) {
    return strcmp((*(const cli_cmd_t *const *)x)->name, (*(const cli_cmd_t *const *)y)->name);
}

static int str_ptr_cmp(const void *x, const void *y) { return strcmp(*(const char *const *)x, *(const char *const *)y); }

/* visible subcommands, sorted (buildSpec) */
static size_t spec_commands(const cli_cmd_t ***out) {
    size_t n;
    const cli_cmd_t *all = cli_commands(&n);
    const cli_cmd_t **v = xcalloc(n + 1, sizeof *v);
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (!all[i].hidden) v[k++] = &all[i];
    xqsort(v, k, sizeof *v, cmd_ptr_cmp);
    *out = v;
    return k;
}

static void tokens(sb_t *b, const cli_cmd_t *c, const char *sep) {
    sb_append(b, c->name);
    for (int i = 0; c->aliases && c->aliases[i]; i++) sb_appendf(b, "%s%s", sep, c->aliases[i]);
}

static const carg_t *first_enum_arg(const cli_cmd_t *c) {
    for (int i = 0; c->args && c->args[i].name; i++)
        if (c->args[i].options) return &c->args[i];
    return NULL;
}

static bool has_file_arg(const cli_cmd_t *c) {
    for (int i = 0; c->args && c->args[i].name; i++)
        if (!c->args[i].options) return true;
    return false;
}

static char *gen_bash(void) {
    const cli_cmd_t **cmds;
    size_t n = spec_commands(&cmds);
    const cli_cmd_t *root = cli_launch_command();
    sb_t b = SB_INIT;
    sb_appendf(&b, "# bash completion for %s — generated by `%s completions bash`\n\n", CLI_BIN, CLI_BIN);
    sb_append(&b, "_omc_comma() {\n\tlocal words=\"$1\" realcur prefix\n\trealcur=\"${cur##*,}\"\n\tprefix=\"${cur%\"$realcur\"}\"\n"
                  "\tlocal -a matches\n\tmatches=( $(compgen -W \"$words\" -- \"$realcur\") )\n\tlocal i\n"
                  "\tfor (( i=0; i < ${#matches[@]}; i++ )); do matches[i]=\"$prefix${matches[i]}\"; done\n"
                  "\tCOMPREPLY=( \"${matches[@]}\" )\n\tcompopt -o nospace 2>/dev/null\n}\n\n");
    /* subcommand tokens, sorted */
    VEC(const char *) toks = {0};
    for (size_t i = 0; i < n; i++) {
        vec_push(&toks, cmds[i]->name);
        for (int k = 0; cmds[i]->aliases && cmds[i]->aliases[k]; k++) vec_push(&toks, cmds[i]->aliases[k]);
    }
    xqsort(toks.v, toks.n, sizeof *toks.v, str_ptr_cmp);
    sb_append(&b, "_omc_root() {\n\tcase \"$prev\" in\n");
    bash_flag_case(&b, root->flags);
    sb_append(&b, "\n\tesac\n\tif [[ \"$cur\" == -* ]]; then\n\t\tCOMPREPLY=( $(compgen -W \"");
    bash_flag_words(&b, root->flags);
    sb_append(&b, "\" -- \"$cur\") )\n\telse\n\t\tCOMPREPLY=( $(compgen -W \"");
    for (size_t i = 0; i < toks.n; i++) sb_appendf(&b, "%s%s", i ? " " : "", toks.v[i]);
    sb_putc(&b, ' ');
    bash_flag_words(&b, root->flags);
    sb_append(&b, "\" -- \"$cur\") )\n\tfi\n}\n\n");
    vec_free(&toks);
    for (size_t i = 0; i < n; i++) {
        const cli_cmd_t *c = cmds[i];
        sb_append(&b, "_omc_cmd_");
        fn_name(&b, c->name);
        sb_append(&b, "() {\n\tcase \"$prev\" in\n");
        bash_flag_case(&b, c->flags);
        sb_append(&b, "\n\tesac\n\tif [[ \"$cur\" == -* ]]; then\n\t\tCOMPREPLY=( $(compgen -W \"");
        bash_flag_words(&b, c->flags);
        sb_append(&b, "\" -- \"$cur\") )\n\telse\n\t\t");
        const carg_t *ea = first_enum_arg(c);
        if (ea) {
            sb_append(&b, "COMPREPLY=( $(compgen -W \"");
            words(&b, ea->options);
            sb_append(&b, "\" -- \"$cur\") )");
        } else if (has_file_arg(c)) sb_append(&b, "COMPREPLY=( $(compgen -f -- \"$cur\") ); compopt -o filenames");
        else sb_append(&b, ":");
        sb_append(&b, "\n\tfi\n}\n\n");
    }
    sb_append(&b, "_omc() {\n\tlocal cur prev cmd i\n\tcur=\"${COMP_WORDS[COMP_CWORD]}\"\n\tprev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
                  "\tcmd=\"\"\n\tfor (( i=1; i < COMP_CWORD; i++ )); do\n\t\tcase \"${COMP_WORDS[i]}\" in\n\t\t\t-*) ;;\n"
                  "\t\t\t*) cmd=\"${COMP_WORDS[i]}\"; break ;;\n\t\tesac\n\tdone\n\tcase \"$cmd\" in\n");
    for (size_t i = 0; i < n; i++) {
        sb_append(&b, "\t\t");
        tokens(&b, cmds[i], "|");
        sb_append(&b, ")\n\t\t\t_omc_cmd_");
        fn_name(&b, cmds[i]->name);
        sb_append(&b, "\n\t\t\t;;\n");
    }
    sb_appendf(&b, "\t\t*) _omc_root ;;\n\tesac\n}\ncomplete -F _omc %s\n\n", CLI_BIN);
    free(cmds);
    return sb_detach(&b);
}

static void zsh_desc(sb_t *b, const char *s) {
    char *t = s ? str_trim(s) : xstrdup("");
    for (const char *c = t; *c; c++) {
        if (*c == '\'') sb_append(b, "’");
        else if (*c == '[') sb_putc(b, '(');
        else if (*c == ']') sb_putc(b, ')');
        else if (*c == '\r' || *c == '\n' || *c == ':') sb_putc(b, ' ');
        else sb_putc(b, *c);
    }
    free(t);
}

static void zsh_action(sb_t *b, const cflag_t *f) {
    switch (flag_value(f)) {
    case VS_FLAG: break;
    case VS_VALUE: sb_append(b, ":value:"); break;
    case VS_ENUM:
        sb_append(b, ":value:(");
        for (int i = 0; f->options[i]; i++) sb_appendf(b, "%s%s", i ? " " : "", f->options[i]);
        sb_putc(b, ')');
        break;
    case VS_LIST: sb_append(b, ":value:_omc_tools"); break;
    case VS_MODELS_MULTI: sb_append(b, ":models:_omc_models_list"); break;
    case VS_MODELS: sb_append(b, ":model:_omc_call models"); break;
    case VS_SESSIONS: sb_append(b, ":session:_omc_call sessions"); break;
    case VS_FILE: sb_append(b, ":file:_files"); break;
    case VS_DIR: sb_append(b, ":dir:_files -/"); break;
    }
}

static void zsh_flag_spec(sb_t *b, const cflag_t *f) {
    sb_t body = SB_INIT;
    sb_putc(&body, '[');
    zsh_desc(&body, f->desc);
    sb_putc(&body, ']');
    zsh_action(&body, f);
    if (f->ch && f->multiple) sb_appendf(b, "'*'{-%c,--%s}'%s'", f->ch, f->name, sb_cstr(&body));
    else if (f->ch) sb_appendf(b, "'(-%c --%s)'{-%c,--%s}'%s'", f->ch, f->name, f->ch, f->name, sb_cstr(&body));
    else if (f->multiple) sb_appendf(b, "'*--%s%s'", f->name, sb_cstr(&body));
    else sb_appendf(b, "'--%s%s'", f->name, sb_cstr(&body));
    sb_free(&body);
}

static char *gen_zsh(void) {
    const cli_cmd_t **cmds;
    size_t n = spec_commands(&cmds);
    const cli_cmd_t *root = cli_launch_command();
    sb_t b = SB_INIT;
    sb_appendf(&b, "#compdef %s\n# zsh completion for %s — generated by `%s completions zsh`\n\n", CLI_BIN, CLI_BIN, CLI_BIN);
    sb_appendf(&b,
               "_omc_call() {\n\t# zsh's _arguments invokes an action function with the compadd options it\n"
               "\t# computed prepended ($subopts, then $expl — _arguments:465), so $1 is\n"
               "\t# \"-J\" in a stock setup and the kind arrives last. Read it from the end:\n"
               "\t# the prepended options can be either flags or option/value pairs.\n"
               "\tlocal kind=${argv[-1]}\n\tlocal -a items\n\tlocal line\n"
               "\tfor line in \"${(@f)$(command %s __complete $kind -- \"$PREFIX\" 2>/dev/null)}\"; do\n"
               "\t\t[[ -z $line ]] && continue\n\t\titems+=( \"${line//$'\\t'/:}\" )\n\tdone\n"
               "\t_describe -t \"$kind\" \"$kind\" items\n}\n"
               "_omc_models_list() {\n\tlocal -a items\n\tlocal line\n"
               "\tfor line in \"${(@f)$(command %s __complete models 2>/dev/null)}\"; do\n"
               "\t\t[[ -z $line ]] && continue\n\t\titems+=( \"${line%%%%$'\\t'*}\" )\n\tdone\n"
               "\t_values -s , 'models' $items\n}\n_omc_tools() { _values -s , 'tools' ",
               CLI_BIN, CLI_BIN);
    for (int i = 0; TOOL_NAMES[i]; i++) sb_appendf(&b, "%s%s", i ? " " : "", TOOL_NAMES[i]);
    sb_append(&b, " }\n\n_omc_commands() {\n\tlocal -a commands\n\tcommands=(\n");
    for (size_t i = 0; i < n; i++) {
        sb_appendf(&b, "\t\t'%s:", cmds[i]->name);
        zsh_desc(&b, cmds[i]->desc);
        sb_append(&b, "'\n");
    }
    sb_append(&b, "\t)\n\t_describe -t commands 'command' commands\n}\n\n");
    for (size_t i = 0; i < n; i++) {
        const cli_cmd_t *c = cmds[i];
        sb_append(&b, "_omc_cmd_");
        fn_name(&b, c->name);
        sb_append(&b, "() {\n\t_arguments -s \\\n\t\t'(-h --help)'{-h,--help}'[Show help]'");
        for (int k = 0; c->flags && c->flags[k].name; k++) {
            sb_append(&b, " \\\n\t\t");
            zsh_flag_spec(&b, &c->flags[k]);
        }
        for (int k = 0; c->args && c->args[k].name; k++) {
            sb_append(&b, " \\\n\t\t");
            if (c->args[k].options) {
                sb_appendf(&b, "':%s:(", c->args[k].name);
                for (int q = 0; c->args[k].options[q]; q++) sb_appendf(&b, "%s%s", q ? " " : "", c->args[k].options[q]);
                sb_append(&b, ")'");
            } else sb_appendf(&b, "':%s:_files'", c->args[k].name);
        }
        sb_append(&b, "\n}\n\n");
    }
    sb_append(&b, "_omc() {\n\tlocal curcontext=\"$curcontext\" state line\n\ttypeset -A opt_args\n\t_arguments -C -s \\\n"
                  "\t\t'(-h --help)'{-h,--help}'[Show help]' \\\n\t\t'(-v --version)'{-v,--version}'[Show version]'");
    for (int k = 0; root->flags[k].name; k++) {
        sb_append(&b, " \\\n\t\t");
        zsh_flag_spec(&b, &root->flags[k]);
    }
    sb_append(&b, " \\\n\t\t'1: :_omc_commands' \\\n\t\t'*::arg:->args'\n\tcase $state in\n\t\targs)\n\t\t\tcase $line[1] in\n");
    for (size_t i = 0; i < n; i++) {
        sb_append(&b, "\t\t\t");
        tokens(&b, cmds[i], "|");
        sb_append(&b, ") _omc_cmd_");
        fn_name(&b, cmds[i]->name);
        sb_append(&b, " ;;\n");
    }
    sb_appendf(&b, "\t\t\tesac\n\t\t\t;;\n\tesac\n}\n"
                   "# Works both ways: autoloaded from $fpath (file named _omc) or eval'd from a\n"
                   "# startup file. When autoloaded, funcstack[1] is _omc and we invoke it; when\n"
                   "# sourced/eval'd we register it with compdef instead.\n"
                   "if [ \"$funcstack[1]\" = \"_omc\" ]; then\n\t_omc \"$@\"\nelse\n\tcompdef _omc %s\nfi\n\n",
               CLI_BIN);
    free(cmds);
    return sb_detach(&b);
}

static void fish_desc(sb_t *b, const char *s) {
    char *t = s ? str_trim(s) : xstrdup("");
    for (const char *c = t; *c; c++) {
        if (*c == '\'') sb_append(b, "’");
        else if (*c == '\r' || *c == '\n') sb_putc(b, ' ');
        else sb_putc(b, *c);
    }
    free(t);
}

static void fish_flag_line(sb_t *b, const char *cond, const cflag_t *f) {
    sb_appendf(b, "complete -c %s -n '%s'", CLI_BIN, cond);
    if (f->ch) sb_appendf(b, " -s %c", f->ch);
    sb_appendf(b, " -l %s", f->name);
    if (f->desc && *f->desc) {
        sb_append(b, " -d '");
        fish_desc(b, f->desc);
        sb_putc(b, '\'');
    }
    switch (flag_value(f)) {
    case VS_FLAG: break;
    case VS_VALUE: sb_append(b, " -x"); break;
    case VS_ENUM:
        sb_append(b, " -x -a '");
        for (int i = 0; f->options[i]; i++) sb_appendf(b, "%s%s", i ? " " : "", f->options[i]);
        sb_putc(b, '\'');
        break;
    case VS_LIST:
        sb_append(b, " -x -a '");
        for (int i = 0; TOOL_NAMES[i]; i++) sb_appendf(b, "%s%s", i ? " " : "", TOOL_NAMES[i]);
        sb_putc(b, '\'');
        break;
    case VS_MODELS:
    case VS_MODELS_MULTI: sb_appendf(b, " -x -a '(command %s __complete models -- (commandline -ct))'", CLI_BIN); break;
    case VS_SESSIONS: sb_appendf(b, " -x -a '(command %s __complete sessions -- (commandline -ct))'", CLI_BIN); break;
    case VS_FILE: sb_append(b, " -r -F"); break;
    case VS_DIR: sb_append(b, " -x -a '(__fish_complete_directories (commandline -ct))'"); break;
    }
    sb_putc(b, '\n');
}

static char *gen_fish(void) {
    const cli_cmd_t **cmds;
    size_t n = spec_commands(&cmds);
    const cli_cmd_t *root = cli_launch_command();
    sb_t b = SB_INIT;
    sb_appendf(&b, "# fish completion for %s — generated by `%s completions fish`\n\n", CLI_BIN, CLI_BIN);
    sb_append(&b, "function __fish_omc_no_subcommand\n\tfor i in (commandline -opc)\n\t\tif contains -- $i");
    for (size_t i = 0; i < n; i++) {
        sb_putc(&b, ' ');
        tokens(&b, cmds[i], " ");
    }
    sb_append(&b, "\n\t\t\treturn 1\n\t\tend\n\tend\n\treturn 0\nend\n\n");
    const char *rc = "__fish_omc_no_subcommand";
    for (size_t i = 0; i < n; i++) {
        const cli_cmd_t *c = cmds[i];
        for (int k = -1; k < 0 || (c->aliases && c->aliases[k]); k++) {
            sb_appendf(&b, "complete -c %s -f -n '%s' -a '%s' -d '", CLI_BIN, rc, k < 0 ? c->name : c->aliases[k]);
            fish_desc(&b, c->desc);
            sb_append(&b, "'\n");
        }
    }
    sb_putc(&b, '\n');
    for (int k = 0; root->flags[k].name; k++) fish_flag_line(&b, rc, &root->flags[k]);
    sb_putc(&b, '\n');
    for (size_t i = 0; i < n; i++) {
        const cli_cmd_t *c = cmds[i];
        sb_t cond = SB_INIT;
        sb_append(&cond, "__fish_seen_subcommand_from ");
        tokens(&cond, c, " ");
        for (int k = 0; c->flags && c->flags[k].name; k++) fish_flag_line(&b, sb_cstr(&cond), &c->flags[k]);
        bool any_enum = false;
        for (int k = 0; c->args && c->args[k].name; k++) {
            if (!c->args[k].options) continue;
            any_enum = true;
            sb_appendf(&b, "complete -c %s -f -n '%s' -a '", CLI_BIN, sb_cstr(&cond));
            for (int q = 0; c->args[k].options[q]; q++) sb_appendf(&b, "%s%s", q ? " " : "", c->args[k].options[q]);
            sb_append(&b, "' -d '");
            fish_desc(&b, c->args[k].desc);
            sb_append(&b, "'\n");
        }
        if (!any_enum && has_file_arg(c)) sb_appendf(&b, "complete -c %s -F -n '%s'\n", CLI_BIN, sb_cstr(&cond));
        sb_free(&cond);
    }
    sb_putc(&b, '\n');
    free(cmds);
    return sb_detach(&b);
}

int cmd_completions_raw(int argc, char **argv) {
    const char *shell = argc > 0 ? argv[0] : NULL;
    char *s = NULL;
    if (str_eq(shell, "bash")) s = gen_bash();
    else if (str_eq(shell, "zsh")) s = gen_zsh();
    else if (str_eq(shell, "fish")) s = gen_fish();
    if (!s) {
        fprintf(stderr, "Usage: %s completions <bash|zsh|fish>\n", CLI_BIN);
        return 1;
    }
    fputs(s, stdout);
    free(s);
    return 0;
}

/* __complete <kind> [-- <prefix>]: `value\tdescription` lines */
static char *clean(const char *s) {
    sb_t b = SB_INIT;
    bool ws = false;
    for (const char *c = s ? s : ""; *c; c++) {
        if (*c == '\t' || *c == '\r' || *c == '\n') {
            if (!ws) sb_putc(&b, ' ');
            ws = true;
        } else {
            sb_putc(&b, *c);
            ws = false;
        }
    }
    char *r = str_trim(sb_cstr(&b));
    sb_free(&b);
    return r;
}

int cmd_complete(int argc, char **argv) {
    strvec_t a = {0};
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--")) strvec_push(&a, argv[i]);
    const char *kind = a.n ? a.v[0] : NULL;
    const char *prefix = a.n > 1 ? a.v[a.n - 1] : "";
    strvec_t lines = {0};
    if (str_eq(kind, "models")) {
        ai_providers_init();
        catalog_init();
        char *needle = str_lower(prefix);
        hmap_t seen;
        hmap_init(&seen);
        for (size_t i = 0; i < catalog_count(); i++) {
            const ai_model_t *m = catalog_at(i);
            char *cands[2] = {xasprintf("%s/%s", m->provider, m->id), xstrdup(m->id)};
            for (int k = 0; k < 2; k++) {
                if (!hmap_has(&seen, cands[k])) {
                    hmap_put(&seen, cands[k], (void *)1);
                    if (!*needle || str_istr(cands[k], needle)) strvec_push_own(&lines, xasprintf("%s\t%s", cands[k], m->provider));
                }
                free(cands[k]);
            }
        }
        hmap_free(&seen, NULL);
        free(needle);
        strvec_sort(&lines);
    } else if (str_eq(kind, "sessions")) {
        settings_init(NULL, NULL);
        char *cwd = get_cwd();
        char *dir = session_dir_for_cwd(cwd);
        session_list_t l;
        session_list(dir, &l);
        for (size_t i = 0; i < l.n; i++) {
            const session_info_t *s = &l.v[i];
            if (*prefix && !str_starts(s->id, prefix)) continue;
            char *lab = clean(s->title ? s->title : s->first_message ? s->first_message : "");
            if (utf8_strlen(lab) > 72) {
                size_t off = 0, k = 0;
                while (lab[off] && k < 72) {
                    uint32_t cp;
                    off += (size_t)utf8_decode(lab + off, strlen(lab + off), &cp);
                    k++;
                }
                lab[off] = 0;
            }
            strvec_push_own(&lines, xasprintf("%s\t%s", s->id, lab));
            free(lab);
        }
        session_list_free(&l);
        free(dir);
        free(cwd);
    }
    for (size_t i = 0; i < lines.n; i++) printf("%s\n", lines.v[i]);
    strvec_free(&lines);
    strvec_free(&a);
    return 0;
}
