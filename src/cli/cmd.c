/* cmd.c - subcommand framework (port of pi-utils/cli.ts Command.parse + node:util parseArgs strict mode,
 * renderRootHelp / renderCommandHelp and the run() usage-error report). */
#include "cli/cmd.h"
#include "cli/cli_int.h"

#include <stdarg.h>
#include <unistd.h>

/* ---------- chalk ---------- */

static int color_level(void) {
    static int lvl = -1;
    if (lvl >= 0) return lvl;
    const char *f = getenv("FORCE_COLOR");
    if (f) {
        if (!*f || !strcmp(f, "true")) lvl = 1;
        else if (!strcmp(f, "false")) lvl = 0;
        else {
            char *end;
            long v = strtol(f, &end, 10);
            lvl = end == f ? 1 : (int)(v < 0 ? 0 : v > 3 ? 3 : v);
        }
        return lvl;
    }
    if (getenv("NO_COLOR") || str_eq(getenv("TERM"), "dumb") || !isatty(1)) return lvl = 0;
    return lvl = 1;
}

static char *wrap(const char *s, const char *open, const char *close) {
    return color_level() ? xasprintf("%s%s%s", open, s, close) : xstrdup(s);
}
char *cli_red(const char *s) { return wrap(s, "\x1b[31m", "\x1b[39m"); }
char *cli_yellow(const char *s) { return wrap(s, "\x1b[33m", "\x1b[39m"); }
char *cli_green(const char *s) { return wrap(s, "\x1b[32m", "\x1b[39m"); }
char *cli_cyan(const char *s) { return wrap(s, "\x1b[36m", "\x1b[39m"); }
char *cli_blue(const char *s) { return wrap(s, "\x1b[34m", "\x1b[39m"); }
char *cli_dim(const char *s) { return wrap(s, "\x1b[2m", "\x1b[22m"); }
char *cli_bold(const char *s) { return wrap(s, "\x1b[1m", "\x1b[22m"); }

/* ---------- accessors ---------- */

static int flag_index(const cli_cmd_t *c, const char *name) {
    for (int i = 0; c->flags && c->flags[i].name; i++)
        if (!strcmp(c->flags[i].name, name)) return i;
    return -1;
}

static int arg_index(const cli_cmd_t *c, const char *name) {
    for (int i = 0; c->args && c->args[i].name; i++)
        if (!strcmp(c->args[i].name, name)) return i;
    return -1;
}

bool cp_bool(const cparse_t *p, const char *name) {
    int i = flag_index(p->cmd, name);
    return i >= 0 && p->fv[i].set && p->fv[i].num;
}

const char *cp_str(const cparse_t *p, const char *name) {
    int i = flag_index(p->cmd, name);
    if (i < 0 || !p->fv[i].set || !p->fv[i].vals.n) return NULL;
    return p->fv[i].vals.v[p->fv[i].vals.n - 1];
}

const strvec_t *cp_strs(const cparse_t *p, const char *name) {
    int i = flag_index(p->cmd, name);
    return i >= 0 && p->fv[i].set && p->fv[i].vals.n ? &p->fv[i].vals : NULL;
}

bool cp_int(const cparse_t *p, const char *name, long long *out) {
    int i = flag_index(p->cmd, name);
    if (i < 0 || !p->fv[i].set) return false;
    *out = p->fv[i].num;
    return true;
}

const char *cp_arg(const cparse_t *p, const char *name) {
    int i = arg_index(p->cmd, name);
    return i >= 0 && p->argv_of[i].n ? p->argv_of[i].v[0] : NULL;
}

const strvec_t *cp_arg_multi(const cparse_t *p, const char *name) {
    int i = arg_index(p->cmd, name);
    return i >= 0 && p->argv_of[i].n ? &p->argv_of[i] : NULL;
}

/* ---------- usage line / help ---------- */

static char *upper(const char *s) {
    char *u = xstrdup(s);
    for (char *q = u; *q; q++)
        if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
    return u;
}

char *cli_usage_line(const cli_cmd_t *c) {
    sb_t b = SB_INIT;
    sb_appendf(&b, "$ %s %s", CLI_BIN, c->name);
    for (int i = 0; c->args && c->args[i].name; i++) {
        char *u = upper(c->args[i].name);
        const char *ell = c->args[i].multiple ? "..." : "";
        if (c->args[i].required) sb_appendf(&b, " %s%s", u, ell);
        else sb_appendf(&b, " [%s%s]", u, ell);
        free(u);
    }
    if (c->flags && c->flags[0].name) sb_append(&b, " [FLAGS]");
    return sb_detach(&b);
}

static void pad_to(sb_t *b, const char *s, size_t w) {
    sb_append(b, s);
    for (size_t n = strlen(s); n < w; n++) sb_putc(b, ' ');
}

static void render_body(sb_t *b, const cli_cmd_t *c) {
    if (c->args && c->args[0].name) {
        sb_append(b, "ARGUMENTS\n");
        size_t w = 0;
        for (int i = 0; c->args[i].name; i++) w = MAX(w, strlen(c->args[i].name));
        for (int i = 0; c->args[i].name; i++) {
            const carg_t *a = &c->args[i];
            char *u = upper(a->name);
            sb_append(b, "  ");
            pad_to(b, u, w + 2);
            free(u);
            if (a->desc) sb_appendf(b, " %s", a->desc);
            if (a->options) {
                sb_append(b, " (");
                for (int k = 0; a->options[k]; k++) sb_appendf(b, "%s%s", k ? "|" : "", a->options[k]);
                sb_putc(b, ')');
            }
            sb_putc(b, '\n');
        }
        sb_putc(b, '\n');
    }
    if (c->flags && c->flags[0].name) {
        sb_append(b, "FLAGS\n");
        strvec_t left = {0};
        size_t w = 0;
        for (int i = 0; c->flags[i].name; i++) {
            const cflag_t *f = &c->flags[i];
            char *l = xasprintf("  %s--%s%s", f->ch ? (char[]){'-', f->ch, ',', ' ', 0} : "    ", f->name,
                                f->kind == CF_BOOL ? "" : f->kind == CF_INT ? "=<int>" : "=<value>");
            w = MAX(w, strlen(l));
            strvec_push_own(&left, l);
        }
        for (int i = 0; c->flags[i].name; i++) {
            pad_to(b, left.v[i], w + 2);
            sb_appendf(b, "%s\n", c->flags[i].desc ? c->flags[i].desc : "");
        }
        strvec_free(&left);
        sb_putc(b, '\n');
    }
    if (c->examples && c->examples[0]) {
        sb_append(b, "EXAMPLES\n");
        for (int i = 0; c->examples[i]; i++) {
            const char *s = c->examples[i];
            for (;;) {
                const char *nl = strchr(s, '\n');
                sb_append(b, "  ");
                sb_appendn(b, s, nl ? (size_t)(nl - s) : strlen(s));
                sb_putc(b, '\n');
                if (!nl) break;
                s = nl + 1;
            }
        }
        sb_putc(b, '\n');
    }
}

void cli_render_command_help(FILE *f, const cli_cmd_t *c) {
    sb_t b = SB_INIT;
    if (c->desc) sb_appendf(&b, "%s\n\n", c->desc);
    char *u = cli_usage_line(c);
    sb_appendf(&b, "USAGE\n  %s\n\n", u);
    free(u);
    render_body(&b, c);
    /* lines.join("\n") with a trailing "" element: the last section's blank line is the final newline */
    if (b.len >= 2 && !strcmp(sb_cstr(&b) + b.len - 2, "\n\n")) sb_truncate(&b, b.len - 1);
    fputs(sb_cstr(&b), f);
    sb_free(&b);
}

static int cmd_name_cmp(const void *a, const void *b) {
    return strcmp((*(const cli_cmd_t *const *)a)->name, (*(const cli_cmd_t *const *)b)->name);
}

void cli_render_root_help(FILE *f) {
    sb_t b = SB_INIT;
    sb_appendf(&b, "%s %s\n\nUSAGE\n  $ %s [COMMAND]\n\n", CLI_BIN, OMC_VERSION_LABEL, CLI_BIN);
    render_body(&b, cli_launch_command());
    size_t n;
    const cli_cmd_t *cmds = cli_commands(&n);
    VEC(const cli_cmd_t *) vis = {0};
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (!cmds[i].hidden) {
            vec_push(&vis, &cmds[i]);
            w = MAX(w, strlen(cmds[i].name));
        }
    xqsort(vis.v, vis.n, sizeof *vis.v, cmd_name_cmp);
    if (vis.n) {
        sb_append(&b, "COMMANDS\n");
        for (size_t i = 0; i < vis.n; i++) {
            sb_append(&b, "  ");
            pad_to(&b, vis.v[i]->name, w + 2);
            sb_appendf(&b, "%s\n", vis.v[i]->desc ? vis.v[i]->desc : "");
        }
        sb_putc(&b, '\n');
    }
    vec_free(&vis);
    if (b.len >= 2 && !strcmp(sb_cstr(&b) + b.len - 2, "\n\n")) sb_truncate(&b, b.len - 1);
    char *extra = cli_extra_help();
    sb_appendf(&b, "\n%s\n", extra);
    free(extra);
    fputs(sb_cstr(&b), f);
    sb_free(&b);
}

/* ---------- parse (node:util parseArgs, strict, allowPositionals) ---------- */

static const cflag_t *flag_by_short(const cli_cmd_t *c, char ch, int *idx) {
    for (int i = 0; c->flags && c->flags[i].name; i++)
        if (c->flags[i].ch == ch) {
            *idx = i;
            return &c->flags[i];
        }
    return NULL;
}

static char *short_and_long(const cflag_t *f) {
    return f->ch ? xasprintf("-%c, --%s", f->ch, f->name) : xasprintf("--%s", f->name);
}

typedef struct {
    int idx;          /* flag index, -1 unknown */
    char *raw;        /* rawName (`--foo`, `-f`) */
    char *value;      /* NULL when none */
    bool inline_value;
} tok_t;

static void tok_free(tok_t *t) {
    free(t->raw);
    free(t->value);
}

static char *perr(char **err, char *msg) {
    if (!*err) *err = msg;
    else free(msg);
    return NULL;
}

static int find_long(const cli_cmd_t *c, const char *name, size_t n) {
    for (int i = 0; c->flags && c->flags[i].name; i++)
        if (strlen(c->flags[i].name) == n && !strncmp(c->flags[i].name, name, n)) return i;
    return -1;
}

static bool is_string_flag(const cli_cmd_t *c, int idx) { return idx >= 0 && c->flags[idx].kind != CF_BOOL; }

/* parseInt(raw, 10): leading whitespace, sign, digits; false for NaN */
static bool js_parse_int(const char *s, long long *out) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') s++;
    int sign = 1;
    if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
    if (*s < '0' || *s > '9') return false;
    long long v = 0;
    while (*s >= '0' && *s <= '9') {
        if (v < 922337203685477580LL) v = v * 10 + (*s - '0');
        s++;
    }
    *out = sign * v;
    return true;
}

/* tokenize + strict checks + store; returns false with *err set */
static bool parse_argv(cparse_t *p, int argc, char **argv, char **err) {
    const cli_cmd_t *c = p->cmd;
    VEC(tok_t) toks = {0};
    bool ok = true;
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        size_t al = strlen(arg);
        if (!strcmp(arg, "--")) {
            for (i++; i < argc; i++) strvec_push(&p->pos, argv[i]);
            break;
        }
        if (al == 2 && arg[0] == '-' && arg[1] != '-') { /* lone short option */
            int idx = -1;
            flag_by_short(c, arg[1], &idx);
            tok_t t = {.idx = idx, .raw = xstrdup(arg)};
            if (is_string_flag(c, idx) && next) {
                t.value = xstrdup(next);
                i++;
            }
            vec_push(&toks, t);
            continue;
        }
        if (al > 2 && arg[0] == '-' && arg[1] != '-') {
            int idx = -1;
            flag_by_short(c, arg[1], &idx);
            if (is_string_flag(c, idx)) { /* -fVALUE */
                vec_push(&toks, ((tok_t){.idx = idx, .raw = xasprintf("-%c", arg[1]), .value = xstrdup(arg + 2), .inline_value = true}));
                continue;
            }
            /* short option group: -abc / -abfFILE */
            for (size_t k = 1; k < al; k++) {
                int gi = -1;
                flag_by_short(c, arg[k], &gi);
                if (!is_string_flag(c, gi) || k == al - 1) {
                    tok_t t = {.idx = gi, .raw = xasprintf("-%c", arg[k])};
                    if (is_string_flag(c, gi) && k == al - 1 && next) {
                        t.value = xstrdup(next);
                        i++;
                    }
                    vec_push(&toks, t);
                } else {
                    vec_push(&toks, ((tok_t){.idx = gi, .raw = xasprintf("-%c", arg[k]), .value = xstrdup(arg + k + 1), .inline_value = true}));
                    break;
                }
            }
            continue;
        }
        if (al > 2 && arg[0] == '-' && arg[1] == '-') {
            const char *eq = strchr(arg, '=');
            if (eq) {
                int idx = find_long(c, arg + 2, (size_t)(eq - arg - 2));
                vec_push(&toks, ((tok_t){.idx = idx, .raw = xstrndup(arg, (size_t)(eq - arg)), .value = xstrdup(eq + 1), .inline_value = true}));
            } else {
                int idx = find_long(c, arg + 2, al - 2);
                tok_t t = {.idx = idx, .raw = xstrdup(arg)};
                if (is_string_flag(c, idx) && next) {
                    t.value = xstrdup(next);
                    i++;
                }
                vec_push(&toks, t);
            }
            continue;
        }
        vec_push(&toks, ((tok_t){.idx = -2, .value = xstrdup(arg)})); /* positional */
    }
    for (size_t k = 0; k < toks.n && ok; k++) {
        tok_t *t = &toks.v[k];
        if (t->idx == -2) {
            strvec_push(&p->pos, t->value);
            continue;
        }
        if (t->idx < 0) {
            perr(err, xasprintf("Unknown option '%s'. To specify a positional argument starting with a '-', place it at the end "
                                "of the command after '--', as in '-- \"%s\"",
                                t->raw, t->raw));
            ok = false;
            break;
        }
        const cflag_t *f = &c->flags[t->idx];
        char *sl = short_and_long(f);
        if (f->kind != CF_BOOL && !t->value) {
            perr(err, xasprintf("Option '%s <value>' argument missing", sl));
            ok = false;
        } else if (f->kind == CF_BOOL && t->value) {
            perr(err, xasprintf("Option '%s' does not take an argument", sl));
            ok = false;
        } else if (!t->inline_value && t->value && strlen(t->value) > 1 && t->value[0] == '-') {
            char *ex = str_starts(t->raw, "--") ? xasprintf("'%s=-XYZ'", t->raw)
                                                : xasprintf("'--%s=-XYZ' or '%s-XYZ'", f->name, t->raw);
            perr(err, xasprintf("Option '%s' argument is ambiguous.\nDid you forget to specify the option argument for '%s'?\n"
                                "To specify an option argument starting with a dash use %s.",
                                t->raw, t->raw, ex));
            free(ex);
            ok = false;
        }
        free(sl);
        if (!ok) break;
        struct cli_fv *v = &p->fv[t->idx];
        v->set = v->given = true;
        if (f->kind == CF_BOOL) v->num = 1;
        else {
            if (!f->multiple) {
                strvec_free(&v->vals);
                v->vals = (strvec_t){0};
            }
            strvec_push(&v->vals, t->value);
        }
    }
    for (size_t k = 0; k < toks.n; k++) tok_free(&toks.v[k]);
    vec_free(&toks);
    if (!ok) return false;
    /* convert + validate in declaration order (Command.parse) */
    for (size_t i = 0; i < p->nflags; i++) {
        const cflag_t *f = &c->flags[i];
        struct cli_fv *v = &p->fv[i];
        if (f->kind == CF_INT) {
            if (v->given) {
                long long n;
                if (!js_parse_int(v->vals.v[v->vals.n - 1], &n)) {
                    perr(err, xasprintf("Expected integer for --%s, got \"%s\"", f->name, v->vals.v[v->vals.n - 1]));
                    return false;
                }
                v->num = n;
            } else if (f->def) {
                v->set = true;
                v->num = strtoll(f->def, NULL, 10);
            }
        } else if (f->kind == CF_BOOL) {
            if (!v->given && f->def) {
                v->set = true;
                v->num = str_eq(f->def, "true");
            }
        } else {
            if (!v->given && f->def) {
                v->set = true;
                strvec_push(&v->vals, f->def);
            }
            if (v->set && f->options && !f->multiple) {
                const char *val = v->vals.v[v->vals.n - 1];
                bool found = false;
                for (int k = 0; f->options[k]; k++) found |= !strcmp(f->options[k], val);
                if (!found) {
                    sb_t o = SB_INIT;
                    for (int k = 0; f->options[k]; k++) sb_appendf(&o, "%s%s", k ? ", " : "", f->options[k]);
                    perr(err, xasprintf("Expected --%s to be one of: %s; got \"%s\"", f->name, sb_cstr(&o), val));
                    sb_free(&o);
                    return false;
                }
            }
        }
        if (f->required && !v->set) {
            perr(err, xasprintf("Missing required flag: --%s", f->name));
            return false;
        }
    }
    size_t pi = 0;
    for (size_t i = 0; i < p->nargs; i++) {
        const carg_t *a = &c->args[i];
        if (a->multiple) {
            for (; pi < p->pos.n; pi++) strvec_push(&p->argv_of[i], p->pos.v[pi]);
        } else if (pi < p->pos.n) {
            strvec_push(&p->argv_of[i], p->pos.v[pi++]);
        } else pi++;
        if (a->required && !p->argv_of[i].n) {
            perr(err, xasprintf("Missing required argument: %s", a->name));
            return false;
        }
        if (p->argv_of[i].n && a->options && !a->multiple) {
            const char *val = p->argv_of[i].v[0];
            bool found = false;
            for (int k = 0; a->options[k]; k++) found |= !strcmp(a->options[k], val);
            if (!found) {
                sb_t o = SB_INIT;
                for (int k = 0; a->options[k]; k++) sb_appendf(&o, "%s%s", k ? ", " : "", a->options[k]);
                perr(err, xasprintf("Expected %s to be one of: %s; got \"%s\"", a->name, sb_cstr(&o), val));
                sb_free(&o);
                return false;
            }
        }
    }
    return true;
}

static void report_usage_error(const cli_cmd_t *c, const char *msg) {
    char *u = cli_usage_line(c);
    fprintf(stderr, "error: %s\n\nUSAGE\n  %s\n\nRun `%s %s --help` for details.\n", msg, u, CLI_BIN, c->name);
    free(u);
}

int cp_usage_error(const cparse_t *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *m = xvasprintf(fmt, ap);
    va_end(ap);
    report_usage_error(p->cmd, m);
    free(m);
    return 1;
}

int cli_run_command(const cli_cmd_t *c, int argc, char **argv) {
    /* per-command help: `<cmd> ... --help` anywhere after the id */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            cli_render_command_help(stdout, c);
            return 0;
        }
    if (c->run_raw) return c->run_raw(argc - 1, argv + 1);
    cparse_t p = {.cmd = c};
    for (p.nflags = 0; c->flags && c->flags[p.nflags].name; p.nflags++) {}
    for (p.nargs = 0; c->args && c->args[p.nargs].name; p.nargs++) {}
    p.fv = xcalloc(p.nflags + 1, sizeof *p.fv);
    p.argv_of = xcalloc(p.nargs + 1, sizeof *p.argv_of);
    char *err = NULL;
    int rc;
    if (!parse_argv(&p, argc - 1, argv + 1, &err)) {
        report_usage_error(c, err);
        rc = 1;
    } else {
        fflush(stdout);
        rc = c->run(&p);
    }
    free(err);
    for (size_t i = 0; i < p.nflags; i++) strvec_free(&p.fv[i].vals);
    for (size_t i = 0; i < p.nargs; i++) strvec_free(&p.argv_of[i]);
    free(p.fv);
    free(p.argv_of);
    strvec_free(&p.pos);
    fflush(stdout);
    return rc;
}
