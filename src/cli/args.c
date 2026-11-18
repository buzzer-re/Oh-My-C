/* args.c - launch argv parsing, a port of omp cli/args.ts parseArgs and the flag tables of cli/flag-tables.ts,
 * plus resolveCliArgv (cli-commands.ts). Only the flags omc implements are in the tables; the rest of omp's
 * launch surface (extensions, profiles, foreign imports, …) is reported as an unknown flag. */
#include "config/settings.h"
#include "tui/motion.h"
#include "cli/args.h"
#include "cli/cmd.h"
#include "cli/cli_int.h"
#include "core/json.h"

#include <math.h>
#include <stdarg.h>

/* ---------- flag tables (flag-tables.ts) ---------- */

static const char *const STRING_FLAGS[] = {
    "--cwd", "--config", "--add-dir", "--mode", "--fork", "--provider", "--model", "--smol", "--slow", "--goal",
    "--plan", "--plan-yolo-into", "--prewalk-into", "--spinner-interval", "--max-fps", "--motion", "--max-time", "--warn-cost", "--max-cost", "--api-key", "--system-prompt",
    "--system-prompt-template", "--append-system-prompt", "--session-dir", "--models", "--tools", "--thinking",
    "--export", "--skills", "--approval-mode", "--service-tier",
    "--max-turns", /* omc: stop after n turns */
    NULL};

static const char *const OPTIONAL_FLAGS[] = {"--resume", "-r", "--restore", "--session", NULL};

static const char *const VALUELESS_FLAGS[] = {
    "--help", "--version", "--continue", "--no-session", "--no-tools", "--no-lsp", "--hide-thinking", "--advisor", "--external-thinking",
    "--plan-yolo", "--prewalk", "--no-prewalk", "--print", "--print-thoughts", "--no-skills", "--no-rules", "--no-context-files", "--no-title",
    "--auto-approve", "--yolo",
    "--plan-mode", /* omc: start in read-only plan mode */
    NULL};

static bool in_list(const char *const *l, const char *s) {
    for (; *l; l++)
        if (!strcmp(*l, s)) return true;
    return false;
}

static bool is_string_flag(const char *s) { return in_list(STRING_FLAGS, s); }
static bool is_optional_flag(const char *s) { return in_list(OPTIONAL_FLAGS, s); }
static bool is_valueless_flag(const char *s) { return in_list(VALUELESS_FLAGS, s); }

/* isUnknownLongValueCandidate */
static bool unknown_long_value_candidate(const char *a) {
    return str_starts(a, "--") && !strchr(a, '=') && !is_string_flag(a) && !is_optional_flag(a) && !is_valueless_flag(a);
}

/* flagConsumesValue */
static bool flag_consumes_value(const char *flag, const char *next) {
    if (str_starts(flag, "--") && strchr(flag, '=')) return false;
    if (!next) return false;
    if (is_string_flag(flag)) return true;
    bool value_like = next[0] != '-';
    if (is_optional_flag(flag)) return value_like && next[0];
    if (unknown_long_value_candidate(flag)) return value_like;
    return false;
}

/* ---------- value parsers ---------- */

static char *js_quote(const char *s) { return json_quote(s); }

/* JS Number(): trimmed decimal / exponent / hex / Infinity; empty string is 0 */
static bool js_number(const char *s, double *out) {
    char *t = str_trim(s);
    bool ok = true;
    if (!*t) *out = 0;
    else if (!strcmp(t, "Infinity") || !strcmp(t, "+Infinity")) *out = INFINITY;
    else if (!strcmp(t, "-Infinity")) *out = -INFINITY;
    else if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X' || t[1] == 'o' || t[1] == 'O' || t[1] == 'b' || t[1] == 'B')) {
        int base = (t[1] == 'x' || t[1] == 'X') ? 16 : (t[1] == 'o' || t[1] == 'O') ? 8 : 2;
        char *end;
        *out = (double)strtoull(t + 2, &end, base);
        ok = t[2] && !*end;
    } else {
        for (const char *p = t; *p; p++)
            if (!strchr("0123456789.eE+-", *p)) ok = false;
        if (ok) {
            char *end;
            *out = strtod(t, &end);
            ok = !*end;
        }
    }
    free(t);
    return ok;
}

static void usage_errorf(const char *fmt, ...) PRINTF_LIKE(1, 2);
static void usage_errorf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *m = xvasprintf(fmt, ap);
    va_end(ap);
    launch_report_usage_error(m);
    free(m);
}

/* parseMaxTimeSeconds: "600", "10m", "1h", "5s" */
static bool parse_max_time(const char *value, double *out) {
    char *t = str_trim(value);
    size_t n = strlen(t);
    double secs = NAN;
    bool dur = false;
    if (n >= 2 && strchr("smh", t[n - 1])) {
        dur = true;
        for (size_t i = 0; i < n - 1; i++)
            if (!((t[i] >= '0' && t[i] <= '9') || t[i] == '.')) dur = false;
        /* ^(\d+(?:\.\d+)?)[smh]$ */
        const char *dot = strchr(t, '.');
        if (dur && (t[0] == '.' || (dot && (dot == t + n - 2 || strchr(dot + 1, '.'))))) dur = false;
        if (dur) secs = strtod(t, NULL) * (t[n - 1] == 'h' ? 3600 : t[n - 1] == 'm' ? 60 : 1);
    }
    if (!dur && !js_number(t, &secs)) secs = NAN;
    free(t);
    if (isfinite(secs) && secs > 0) {
        *out = secs;
        return true;
    }
    return false;
}

static bool parse_cost(const char *value, double *out) {
    double usd;
    if (js_number(value, &usd) && isfinite(usd) && usd > 0) {
        *out = usd;
        return true;
    }
    return false;
}

static const char *const THINKING[] = {"off", "minimal", "low", "medium", "high", "xhigh", "max", "auto", NULL};

/* normalizeToolName: built-in ids lower-cased, legacy `search` -> grep */
static char *normalize_tool_name(const char *name) {
    static const char *const BUILTIN[] = {"read", "bash", "edit", "ast_grep", "ast_edit", "ask", "debug", "ida", "eval",
                                          "github", "glob", "grep", "find", "lsp", "checkpoint", "rewind",
                                          "context_notes", "new_context", "security_scan", "task", "wait", "todo",
                                          "web_search", "write", "memory_edit", "retain", "recall", "reflect", "learn",
                                          "manage_skill", "yield", "goal", "think", NULL};
    char *lower = str_lower(name);
    if (!strcmp(lower, "search")) {
        free(lower);
        return xstrdup("grep");
    }
    if (in_list(BUILTIN, lower)) return lower;
    free(lower);
    return xstrdup(name);
}

static strvec_t split_trim(const char *v, bool drop_empty) {
    strvec_t parts = str_split(v, ","), out = {0};
    for (size_t i = 0; i < parts.n; i++) {
        char *t = str_trim(parts.v[i]);
        if (drop_empty && !*t) free(t);
        else strvec_push_own(&out, t);
    }
    strvec_free(&parts);
    return out;
}

/* STRING_SETTERS; false after a thrown CliUsageError (already reported) */
static bool set_string(launch_args_t *r, const char *flag, const char *value) {
    if (!strcmp(flag, "--cwd")) r->cwd = value;
    else if (!strcmp(flag, "--config")) strvec_push(&r->config, value);
    else if (!strcmp(flag, "--add-dir")) strvec_push(&r->add_dir, value);
    else if (!strcmp(flag, "--mode")) {
        static const char *const MODES[] = {"text", "json", "rpc", "acp", "rpc-ui", NULL};
        if (in_list(MODES, value)) r->mode = value;
        else {
            char *q = js_quote(value);
            strvec_push_own(&r->invalid_values,
                            xasprintf("Invalid --mode value: %s. Expected one of: text, json, rpc, rpc-ui, acp.", q));
            free(q);
        }
    } else if (!strcmp(flag, "--fork")) r->fork = value;
    else if (!strcmp(flag, "--provider")) r->provider = value;
    else if (!strcmp(flag, "--model")) r->model = value;
    else if (!strcmp(flag, "--smol")) r->smol = value;
    else if (!strcmp(flag, "--slow")) r->slow = value;
    else if (!strcmp(flag, "--goal")) {
        char *t = str_trim(value);
        if (!*t) {
            free(t);
            launch_report_usage_error("--goal requires a non-empty objective.");
            return false;
        }
        free(t);
        r->goal = value; /* trimmed by the caller (goal_create trims) */
    } else if (!strcmp(flag, "--plan")) r->plan = value;
    else if (!strcmp(flag, "--plan-yolo-into")) r->plan_yolo_into = value;
    else if (!strcmp(flag, "--prewalk-into")) r->prewalk_into = value;
    else if (!strcmp(flag, "--spinner-interval")) {
        char *e = NULL;
        if (!settings_parse_spinner_interval(value, &r->spinner_interval, &e)) {
            usage_errorf("--spinner-interval: %s", e);
            free(e);
            return false;
        }
        r->has_spinner_interval = true;
    } else if (!strcmp(flag, "--max-fps")) {
        char *e = NULL;
        if (!settings_parse_max_fps(value, &r->max_fps, &e)) {
            usage_errorf("--max-fps: %s", e);
            free(e);
            return false;
        }
        r->has_max_fps = true;
    } else if (!strcmp(flag, "--motion")) {
        char *e = NULL;
        tui_motion_mode_t m;
        if (!settings_parse_motion_mode(value, &m, &e)) {
            usage_errorf("--motion: %s", e);
            free(e);
            return false;
        }
        r->motion = m == TUI_MOTION_NONE ? "none" : m == TUI_MOTION_REDUCED ? "reduced" : "full";
    } else if (!strcmp(flag, "--max-time")) {
        if (!parse_max_time(value, &r->max_time)) {
            char *q = js_quote(value);
            usage_errorf("Invalid --max-time value: %s. Expected a positive number of seconds or duration like \"5s\", "
                         "\"10m\", \"1h\".",
                         q);
            free(q);
            return false;
        }
        r->has_max_time = true;
    } else if (!strcmp(flag, "--warn-cost") || !strcmp(flag, "--max-cost")) {
        double *dst = flag[2] == 'w' ? &r->warn_cost : &r->max_cost;
        if (!parse_cost(value, dst)) {
            char *q = js_quote(value);
            usage_errorf("Invalid %s value: %s. Expected a positive USD amount, e.g. 0.50.", flag, q);
            free(q);
            return false;
        }
        if (flag[2] == 'w') r->has_warn_cost = true;
        else r->has_max_cost = true;
    } else if (!strcmp(flag, "--api-key")) r->api_key = value;
    else if (!strcmp(flag, "--system-prompt")) r->system_prompt = value;
    else if (!strcmp(flag, "--system-prompt-template")) r->system_prompt_template = value;
    else if (!strcmp(flag, "--append-system-prompt")) r->append_system_prompt = value;
    else if (!strcmp(flag, "--session-dir")) r->session_dir = value;
    else if (!strcmp(flag, "--models")) {
        strvec_free(&r->models);
        r->models = split_trim(value, false);
        r->has_models = true;
    } else if (!strcmp(flag, "--tools")) {
        strvec_t names = split_trim(value, true);
        strvec_free(&r->tools);
        r->tools = (strvec_t){0};
        for (size_t i = 0; i < names.n; i++) {
            char *n = normalize_tool_name(names.v[i]);
            if (strvec_contains(&r->tools, n)) free(n);
            else strvec_push_own(&r->tools, n);
        }
        strvec_free(&names);
        r->has_tools = true;
    } else if (!strcmp(flag, "--thinking")) {
        if (in_list(THINKING, value)) r->thinking = value;
        else {
            char *q = js_quote(value);
            strvec_push_own(&r->invalid_values,
                            xasprintf("Invalid --thinking value: %s. Expected one of: off, minimal, low, medium, high, "
                                      "xhigh, max, auto.",
                                      q));
            free(q);
        }
    } else if (!strcmp(flag, "--export")) r->export_path = value;
    else if (!strcmp(flag, "--skills")) {
        strvec_free(&r->skills);
        r->skills = split_trim(value, false);
        r->has_skills = true;
    } else if (!strcmp(flag, "--approval-mode")) {
        static const char *const MODES[] = {"always-ask", "always-ask-cached", "write", "yolo", NULL};
        if (in_list(MODES, value)) r->approval_mode = value;
        else {
            char *q = js_quote(value);
            strvec_push_own(&r->invalid_values,
                            xasprintf("Invalid --approval-mode value: %s. Expected one of: always-ask, "
                                      "always-ask-cached, write, yolo.",
                                      q));
            free(q);
        }
    } else if (!strcmp(flag, "--service-tier")) {
        static const char *const TIERS[] = {"none", "auto", "default", "flex", "scale", "priority", "ultrafast", NULL};
        if (!in_list(TIERS, value)) { /* flag-tables.ts throws CliUsageError */
            char *q = js_quote(value);
            usage_errorf("Invalid --service-tier value: %s. Expected one of: none, auto, default, flex, scale, priority, "
                         "ultrafast.",
                         q);
            free(q);
            return false;
        }
        r->service_tier = value;
    } else if (!strcmp(flag, "--max-turns")) r->max_turns = atoi(value);
    return true;
}

static bool set_valueless(launch_args_t *r, const char *a) {
    if (!strcmp(a, "--help") || !strcmp(a, "-h")) r->help = true;
    else if (!strcmp(a, "--version") || !strcmp(a, "-v")) r->version = true;
    else if (!strcmp(a, "--continue") || !strcmp(a, "-c")) r->continue_recent = true;
    else if (!strcmp(a, "--no-session")) r->no_session = true;
    else if (!strcmp(a, "--no-tools")) r->no_tools = true;
    else if (!strcmp(a, "--no-lsp")) r->no_lsp = true;
    else if (!strcmp(a, "--no-judge")) r->no_judge = true;
    else if (!strcmp(a, "--no-spotlight")) r->no_spotlight = true;
    else if (!strcmp(a, "--offline")) r->offline = true;
    else if (!strcmp(a, "--hide-thinking")) r->hide_thinking = true;
    else if (!strcmp(a, "--advisor")) r->advisor = true;
    else if (!strcmp(a, "--external-thinking")) r->external_thinking = true;
    else if (!strcmp(a, "--plan-yolo")) r->plan_yolo = true;
    else if (!strcmp(a, "--prewalk")) r->prewalk = true;
    else if (!strcmp(a, "--no-prewalk")) r->no_prewalk = true;
    else if (!strcmp(a, "--print") || !strcmp(a, "-p")) r->print = true;
    else if (!strcmp(a, "--print-thoughts")) r->print_thoughts = true;
    else if (!strcmp(a, "--no-skills")) r->no_skills = true;
    else if (!strcmp(a, "--no-rules")) r->no_rules = true;
    else if (!strcmp(a, "--no-context-files")) r->no_context_files = true;
    else if (!strcmp(a, "--no-title")) r->no_title = true;
    else if (!strcmp(a, "--auto-approve") || !strcmp(a, "--yolo")) r->auto_approve = true;
    else if (!strcmp(a, "--plan-mode")) r->plan_mode = true;
    else return false;
    return true;
}

void launch_report_usage_error(const char *msg) {
    char *e = xasprintf("Error: %s", msg);
    char *r = cli_red(e);
    fprintf(stderr, "%s\nRun `%s --help` for available flags.\n", r, CLI_BIN);
    free(r);
    free(e);
}

bool launch_report_unrecognized(const launch_args_t *a) {
    if (!a->unrecognized.n) return false;
    char *j = strvec_join(&a->unrecognized, ", ");
    char *e = xasprintf("Error: unknown flag%s: %s", a->unrecognized.n == 1 ? "" : "s", j);
    char *r = cli_red(e);
    fprintf(stderr, "%s\nRun `%s --help` for available flags.\n", r, CLI_BIN);
    free(r);
    free(e);
    free(j);
    return true;
}

bool launch_report_invalid(const launch_args_t *a) {
    for (size_t i = 0; i < a->invalid_values.n; i++) {
        char *e = xasprintf("Error: %s", a->invalid_values.v[i]);
        char *r = cli_red(e);
        fprintf(stderr, "%s\n", r);
        free(r);
        free(e);
    }
    return a->invalid_values.n > 0;
}

/* argv values must outlive the struct: parse copies spliced `--flag=value` values into a pool */
static strvec_t g_pool;

int launch_parse_args(int argc, char **argv, launch_args_t *r) {
    memset(r, 0, sizeof *r);
    const char *sd = env_get("PI_CODING_AGENT_SESSION_DIR");
    if (sd) r->session_dir = sd;
    /* work on a copy: `--flag=value` splices the value in as the next token */
    strvec_t args = {0};
    for (int i = 0; i < argc; i++) strvec_push(&args, argv[i]);
    bool saw_sep = false;
    for (size_t i = 0; i < args.n; i++) {
        if (saw_sep) {
            strvec_push(&r->messages, args.v[i]);
            continue;
        }
        char *arg = xstrdup(args.v[i]);
        size_t flag_index = i;
        long eq_index = -1;
        if (str_starts(arg, "--") && strchr(arg, '=')) {
            char *eq = strchr(arg, '=');
            char *val = xstrdup(eq + 1);
            *eq = 0;
            strvec_push_own(&args, val); /* splice at i + 1 */
            memmove(&args.v[i + 2], &args.v[i + 1], (args.n - i - 2) * sizeof *args.v);
            args.v[i + 1] = val;
            eq_index = (long)(i + 1);
        }
        if (is_string_flag(arg)) {
            if (!strcmp(arg, "--goal") &&
                (i + 1 >= args.n || (eq_index == -1 && args.v[i + 1][0] == '-'))) {
                strvec_push(&r->invalid_values, "--goal requires an objective.");
                if (i + 1 < args.n) i++;
                free(arg);
                continue;
            }
            if (i + 1 < args.n) {
                strvec_push(&g_pool, args.v[++i]);
                if (!set_string(r, arg, g_pool.v[g_pool.n - 1])) {
                    free(arg);
                    strvec_free(&args);
                    return 2;
                }
            }
        } else if (is_optional_flag(arg)) {
            const char *next = i + 1 < args.n ? args.v[i + 1] : NULL;
            bool consume = next && next[0] != '-' && next[0];
            bool restore = !strcmp(arg, "--restore");
            if (consume) {
                strvec_push(&g_pool, args.v[++i]);
                r->resume = g_pool.v[g_pool.n - 1];
                r->resume_set = true;
            } else if (!restore || !r->resume) { /* a bare --restore keeps a session --session/--resume named */
                r->resume = NULL;
                r->resume_set = true;
            }
            if (restore) r->restore = true;
        } else if (set_valueless(r, arg)) {
        } else if (arg[0] == '@') {
            const char *fp = arg + 1;
            size_t n = strlen(fp);
            if (n > 1 && ((fp[0] == '"' && fp[n - 1] == '"') || (fp[0] == '\'' && fp[n - 1] == '\'')))
                strvec_push_own(&r->file_args, xstrndup(fp + 1, n - 2));
            else strvec_push(&r->file_args, fp);
        } else if (arg[0] != '-' || !strcmp(arg, "-")) {
            strvec_push(&r->messages, arg);
        } else if (!strcmp(arg, "--")) {
            saw_sep = true;
        } else {
            strvec_push(&r->unrecognized, arg);
        }
        if (eq_index != -1 && i == flag_index) {
            free(args.v[eq_index]);
            memmove(&args.v[eq_index], &args.v[eq_index + 1], (args.n - (size_t)eq_index - 1) * sizeof *args.v);
            args.n--;
        }
        free(arg);
    }
    strvec_free(&args);
    if (r->system_prompt && r->system_prompt_template) {
        launch_report_usage_error("--system-prompt and --system-prompt-template cannot be combined");
        return 2;
    }
    return 0;
}

void launch_args_free(launch_args_t *a) {
    strvec_free(&a->add_dir);
    strvec_free(&a->config);
    strvec_free(&a->models);
    strvec_free(&a->tools);
    strvec_free(&a->skills);
    strvec_free(&a->messages);
    strvec_free(&a->file_args);
    strvec_free(&a->unrecognized);
    strvec_free(&a->invalid_values);
}

/* ---------- resolveCliArgv (cli-commands.ts) ---------- */

/* RESERVED_TOP_LEVEL_WORDS: plugin/marketplace verbs omp reserves for `plugin <action>`. omp's hint points at
 * `omp plugin <action>`; omc has no plugin manager, so only the prompt escape is offered. */
static const char *const RESERVED_WORDS[] = {"extensions", "list", "remove", "uninstall", "marketplace",
                                              "discover", "upgrade", "enable", "disable", NULL};

static char *reserved_hint(const char *w) {
    return xasprintf("`%s %s` is not a %s command. Run `%s launch %s` if you meant to send \"%s\" as a prompt.", CLI_BIN,
                     w, !strcmp(w, "extensions") ? "management" : "top-level", CLI_BIN, w, w);
}

/* omp top-level commands omc does not port: routing them to a prompt would send e.g. `omc stats` to the model */
static const char *const OMP_ONLY_COMMANDS[] = {
    "auth-broker", "auth-gateway", "anonymize", "bench", "browser-relay", "cleanse", "collab", "compress",
    "dry-balance", "find", "gc", "gallery", "git", "grievances", "images", "img", "if-bench", "install", "join",
    "plugin", "plugins", "predict", "ps", "say", "clip", "share", "shell", "render", "skill", "skills",
    "stats", "stream", "update", "tiny-models", "toks", "commit",
#ifndef OMC_CLI_SESSION
    "session",
#endif
#ifndef OMC_CLI_TTSR
    "ttsr",
#endif
#ifndef OMC_CLI_WORKTREE
    "worktree", "wt",
#endif
    NULL};

static bool omp_only_command(const char *w) { return in_list(OMP_ONLY_COMMANDS, w); }

/* reservedTopLevelWordMessage */
static char *reserved_message(int argc, char **argv) {
    if (argc < 1) return NULL;
    const char *first = argv[0];
    if (first[0] == '-' || first[0] == '@' || !in_list(RESERVED_WORDS, first)) return NULL;
    bool hit = argc < 2;
    if (!hit && !strcmp(first, "marketplace")) {
        static const char *const SUB[] = {"add", "remove", "rm", "update", "list", NULL};
        hit = in_list(SUB, argv[1]);
    }
    for (int i = 1; i < argc && !hit; i++)
        if (argv[i][0] != '-' && strchr(argv[i], '@')) hit = true;
    return hit ? reserved_hint(first) : NULL;
}

static bool is_subcommand(const char *w) {
    if (!w || w[0] == '-' || w[0] == '@') return false;
    return cli_find_command(w) != NULL;
}

static bool is_launch_global_flag(const char *arg) {
    const char *eq = str_starts(arg, "--") ? strchr(arg, '=') : NULL;
    char *name = eq ? xstrndup(arg, (size_t)(eq - arg)) : xstrdup(arg);
    bool r = is_string_flag(name) || is_optional_flag(name) || is_valueless_flag(name);
    free(name);
    return r;
}

static char *unsupported_message(const char *w) {
    return xasprintf("`%s %s` is not available in omc (omp's `%s` command is not ported). Run `%s launch %s` if you "
                     "meant to send \"%s\" as a prompt.",
                     CLI_BIN, w, w, CLI_BIN, w, w);
}

bool resolve_cli_argv(int argc, char **argv, strvec_t *out, char **err) {
    *out = (strvec_t){0};
    *err = NULL;
    char *rm = reserved_message(argc, argv);
    if (rm) {
        *err = rm;
        return false;
    }
    const char *first = argc > 0 ? argv[0] : NULL;
    if (first && (!strcmp(first, "--help") || !strcmp(first, "-h") || !strcmp(first, "--version") ||
                  !strcmp(first, "-v") || !strcmp(first, "help"))) {
        for (int i = 0; i < argc; i++) strvec_push(out, argv[i]);
        return true;
    }
    if (first && first[0] != '-' && first[0] != '@' && omp_only_command(first)) {
        *err = unsupported_message(first);
        return false;
    }
    if (is_subcommand(first)) {
        for (int i = 0; i < argc; i++) strvec_push(out, argv[i]);
        return true;
    }
    /* leadingSubcommandIndex */
    int sub = -1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) break;
        if (a[0] != '-') {
            if (is_subcommand(a)) sub = i;
            else if (omp_only_command(a) && a[0] != '@') {
                *err = unsupported_message(a);
                return false;
            }
            break;
        }
        if (flag_consumes_value(a, i + 1 < argc ? argv[i + 1] : NULL)) i++;
    }
    if (sub >= 0) {
        const char *name = argv[sub];
        const cli_cmd_t *c = cli_find_command(name);
        bool launch_like = c && (!strcmp(c->name, "launch") || !strcmp(c->name, "acp"));
        strvec_push(out, name);
        for (int i = 0; i < sub; i++) {
            if (!launch_like && is_launch_global_flag(argv[i])) {
                if (flag_consumes_value(argv[i], i + 1 < sub ? argv[i + 1] : NULL)) i++;
                continue;
            }
            strvec_push(out, argv[i]);
        }
        for (int i = sub + 1; i < argc; i++) strvec_push(out, argv[i]);
        return true;
    }
    strvec_push(out, "launch");
    for (int i = 0; i < argc; i++) strvec_push(out, argv[i]);
    return true;
}

/* ---------- /restart (flag-tables.ts restartArgv) ---------- */

static int g_proc_argc;
static char **g_proc_argv;

void launch_remember_argv(int argc, char **argv) {
    g_proc_argc = argc;
    g_proc_argv = argv;
}

strvec_t launch_restart_argv(const char *resume_source) {
    /* ONE_SHOT_LAUNCH_FLAGS: the relaunch supplies its own session source */
    static const char *const ONE_SHOT[] = {"--resume", "-r", "--restore", "--session", "--continue", "-c", "--fork",
                                           "--from-claude", "--from-codex", "--goal", NULL};
    strvec_t kept = {0};
    for (int i = 1; i < g_proc_argc; i++) {
        const char *arg = g_proc_argv[i];
        if (!strcmp(arg, "--")) break; /* the rest is literal prompt text */
        if (arg[0] != '-') continue;   /* positional: prompt message, @file, or subcommand */
        bool consumes = flag_consumes_value(arg, i + 1 < g_proc_argc ? g_proc_argv[i + 1] : NULL);
        char *flag = str_starts(arg, "--") ? xstrndup(arg, strcspn(arg, "=")) : xstrdup(arg);
        bool drop = in_list(ONE_SHOT, flag);
        free(flag);
        if (drop) {
            if (consumes) i++;
            continue;
        }
        strvec_push(&kept, arg);
        if (consumes) strvec_push(&kept, g_proc_argv[++i]);
    }
    if (resume_source) { /* --restore: a turn the teardown cut off goes on in the relaunched process */
        strvec_push(&kept, "--restore");
        strvec_push(&kept, resume_source);
    }
    return kept;
}
