/* cli.c - the omc command line (omc_main): argv routing (cli-commands.ts resolveCliArgv + pi-utils run), the
 * launch command (omp main.ts runRootCommand: flags -> settings overrides -> session -> mode) and the omc-only
 * extra commands (migrate, mcp, export, sessions, logout, glob, bash). */
#include "cli/args.h"
#include "cli/cli_int.h"

#include "ai/ai.h"
#include "coding/app.h"
#include "coding/modelctl.h"
#include "coding/costgate.h"
#include "coding/goal.h"
#include "coding/migrate.h"
#include "config/auth.h"
#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "img/imgpipe.h"
#include "mcp/mcp.h"
#include "modes/modes.h"
#include "session/export_html.h"
#include "tools/approval.h"
#include "tools/tools.h"

#include <errno.h>
#include <execinfo.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- shared helpers ---------- */

/* InteractiveMode.restart: replace this process with the launch argv rewritten by restartArgv (execv keeps the
 * PID and the terminal); prints the failure and returns when the exec fails */
static void restart_exec(const char *resume) {
    strvec_t args = launch_restart_argv(resume);
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) n = 0;
    exe[n] = 0;
    strvec_t argv = {0};
    strvec_push(&argv, CLI_BIN);
    for (size_t i = 0; i < args.n; i++) strvec_push(&argv, args.v[i]);
    vec_push(&argv, NULL);
    fflush(stdout);
    fflush(stderr);
    if (n) execv(exe, argv.v);
    else execvp(CLI_BIN, argv.v);
    char *m = xasprintf("Restart exec failed: %s", strerror(errno));
    cli_err_line(m);
    free(m);
    argv.n--;
    strvec_free(&argv);
    strvec_free(&args);
}

void cli_err_line(const char *msg) {
    char *r = cli_red(msg);
    fprintf(stderr, "%s\n", r);
    free(r);
}

void cli_errf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *m = xvasprintf(fmt, ap);
    va_end(ap);
    cli_err_line(m);
    free(m);
}

void cli_init_runtime(const strvec_t *configs) {
    char *cwd = get_cwd();
    settings_init(cwd, configs);
    free(cwd);
    ai_providers_init();
    catalog_init();
    modelctl_apply_context_policy(); /* extendedContext windows for listings */
    auth_init();
}

char *cli_json_pretty(const json_t *j) { return json_dump(j, 2); }

static void io_print(void *ud, const char *msg) {
    UNUSED(ud);
    fprintf(stderr, "%s\n", msg);
}

/* ---------- omc extras ---------- */

/* omc migrate [--from <omp agent dir>] [--dry-run] [--force] [--no-sessions] */
int cmd_migrate_raw(int argc, char **argv) {
    migrate_opts_t mo = {.sessions = true};
    const char *from = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dry-run")) mo.dry_run = true;
        else if (!strcmp(argv[i], "--force")) mo.force = true;
        else if (!strcmp(argv[i], "--no-sessions")) mo.sessions = false;
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = argv[++i];
        else if (!strncmp(argv[i], "--from=", 7)) from = argv[i] + 7;
        else {
            fprintf(stderr, "Usage: omc migrate [--from <omp agent dir>] [--dry-run] [--force] [--no-sessions]\n");
            return 2;
        }
    }
    char *src = from ? path_expand_home(from) : xstrdup(migrate_omp_dir());
    sb_t rep = SB_INIT;
    char *err = NULL;
    int rc = omc_migrate(src, omc_agent_dir(), &mo, &rep, &err);
    if (rc < 0) fprintf(stderr, "omc: %s\n", err ? err : "migration failed");
    else fputs(sb_cstr(&rep), stdout);
    free(err);
    free(src);
    sb_free(&rep);
    return rc < 0 ? 1 : rc;
}

/* omc mcp list | login <server> | logout <server> */
int cmd_mcp_raw(int argc, char **argv) {
    auth_init();
    char *cwd = get_cwd();
    settings_init(cwd, NULL);
    json_t *cfg = mcp_discover_config(cwd);
    free(cwd);
    const char *sub = argc > 0 ? argv[0] : "list";
    int rc = 0;
    if (!strcmp(sub, "list")) {
        const char *k;
        json_t *v;
        json_obj_foreach(cfg, i, k, v) {
            const char *url = mcp_config_url(v);
            char *key = url ? mcp_oauth_key(url) : NULL;
            bool authed = key && auth_has_credentials(key);
            printf("%-24s %s%s\n", k, url ? url : json_get_str(v, "command") ? json_get_str(v, "command") : "?",
                   url ? (authed ? "  [oauth: logged in]" : "") : "  [stdio]");
            free(key);
        }
    } else if ((!strcmp(sub, "login") || !strcmp(sub, "logout")) && argc > 1) {
        json_t *srv = json_get_obj(cfg, argv[1]);
        const char *url = mcp_config_url(srv);
        if (!srv) {
            fprintf(stderr, "omc: no MCP server named %s\n", argv[1]);
            rc = 1;
        } else if (!url) {
            fprintf(stderr, "omc: %s is a stdio server; OAuth applies to remote (HTTP/SSE) servers\n", argv[1]);
            rc = 1;
        } else if (!strcmp(sub, "logout")) {
            rc = mcp_oauth_logout(url) ? 1 : 0;
            if (!rc) fprintf(stderr, "Logged out of %s.\n", argv[1]);
        } else {
            auth_io_t io = {.print = io_print};
            char *err = NULL;
            if (mcp_oauth_login(url, srv, NULL, &io, &err) != 0) {
                fprintf(stderr, "omc: MCP login failed: %s\n", err ? err : "?");
                rc = 1;
            } else {
                fprintf(stderr, "Authorized %s.\n", argv[1]);
            }
            free(err);
        }
    } else {
        fprintf(stderr, "Usage: omc mcp list | login <server> | logout <server>\n");
        rc = 2;
    }
    json_free(cfg);
    return rc;
}

int cmd_logout_raw(int argc, char **argv) {
    auth_init();
    if (argc < 1) {
        fprintf(stderr, "Usage: omc logout <provider>\n");
        return 2;
    }
    return auth_logout(argv[0]) ? 1 : 0;
}

int cmd_sessions_raw(int argc, char **argv) {
    bool all = argc > 0 && !strcmp(argv[0], "--all");
    session_list_t l;
    if (all) session_list_all(&l);
    else {
        char *cwd = get_cwd();
        char *dir = session_dir_for_cwd(cwd);
        session_list(dir, &l);
        free(dir);
        free(cwd);
    }
    for (size_t i = 0; i < l.n; i++) {
        session_info_t *s = &l.v[i];
        char *when = iso8601_from_ms(s->mtime_ms);
        const char *title = s->title ? s->title : s->first_message ? s->first_message : "(empty)";
        char *t1 = xstrndup(title, strcspn(title, "\n"));
        if (strlen(t1) > 70) strcpy(t1 + 67, "...");
        printf("%s  %.19s  %3d msgs  %s%s%s\n", session_short_id(s->id), when, s->message_count, t1, all ? "  " : "",
               all && s->cwd ? s->cwd : "");
        free(t1);
        free(when);
    }
    if (!l.n) fprintf(stderr, "No sessions.\n");
    session_list_free(&l);
    return 0;
}

/* `omc export <session> out.md`: the markdown transcript ("Exported to: <path>") */
static int export_markdown(const char *source, const char *out) {
    char *cwd = get_cwd();
    char *path = session_resolve(source, cwd);
    free(cwd);
    char *err = NULL;
    if (!path) {
        cli_errf("Error: File not found: %s", source);
        return 1;
    }
    session_t *s = session_open(path, &err);
    free(path);
    if (!s) {
        cli_errf("Error: %s", err ? err : "Failed to export session");
        free(err);
        return 1;
    }
    char *text = session_export_markdown(s);
    int rc = fs_write(out, text, strlen(text));
    if (rc == 0) printf("Exported to: %s\n", out);
    else cli_errf("Error: %s", strerror(-rc));
    free(text);
    session_free(s);
    return rc ? 1 : 0;
}

static int export_html_print(const char *source, const char *out) {
    char *cwd = get_cwd();
    char *path = session_resolve(source, cwd);
    free(cwd);
    char *written = NULL, *err = NULL;
    int rc = export_html_from_file(path ? path : source, out, &written, &err);
    free(path);
    if (rc != 0) {
        cli_errf("Error: %s", err ? err : "Failed to export session");
        free(err);
        return 1;
    }
    printf("Exported to: %s\n", written);
    free(written);
    return 0;
}

int cmd_export_raw(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: omc export <session> [out.html|out.md]\n");
        return 2;
    }
    const char *out = argc > 1 ? argv[1] : NULL;
    if (out && str_ends(out, ".md")) return export_markdown(argv[0], out);
    return export_html_print(argv[0], out);
}

/* run a builtin tool directly: omc glob [pattern] | bash <command> [timeout] */
static int run_tool(const char *name, json_t *args) {
    settings_init(NULL, NULL);
    char *cwd = get_cwd();
    tool_env_t *env = tool_env_new(cwd);
    free(cwd);
    agent_tool_t *t = !strcmp(name, "read") ? tool_read_new(env) : !strcmp(name, "grep") ? tool_grep_new(env)
                      : !strcmp(name, "bash") ? tool_bash_new(env) : tool_glob_new(env);
    tool_result_t r = {0};
    tool_ctx_t ctx = {.tool_name = name};
    t->execute(t, &ctx, args, &r);
    char *text = tool_result_joined_text(&r);
    fputs(text, r.is_error ? stderr : stdout);
    fputc('\n', r.is_error ? stderr : stdout);
    int rc = r.is_error ? 1 : 0;
    free(text);
    tool_result_free(&r);
    json_free(args);
    agent_tool_free(t);
    tool_env_free(env);
    return rc;
}

int cmd_glob_raw(int argc, char **argv) {
    json_t *args = json_obj();
    if (argc > 0) json_set_str(args, "path", argv[0]);
    return run_tool("glob", args);
}

int cmd_bash_raw(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: omc bash <command> [timeout-seconds]\n");
        return 2;
    }
    json_t *args = json_obj();
    json_set_str(args, "command", argv[0]);
    if (argc > 1) json_set_num(args, "timeout", atof(argv[1]));
    return run_tool("bash", args);
}

int cmd_version_raw(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);
    printf("%s/%s\n", CLI_BIN, OMC_VERSION);
    return 0;
}

/* ---------- launch: @file arguments (cli/file-processor.ts) ---------- */

#define MAX_CLI_TEXT_BYTES (5 * 1024 * 1024)
#define MAX_CLI_IMAGE_BYTES (25 * 1024 * 1024)

/* resolveReadPath's CLI subset: `~` expansion, then resolved against the cwd */
static char *resolve_file_arg(const char *arg) {
    char *e = path_expand_home(arg);
    if (e[0] == '/') return e;
    char *cwd = get_cwd();
    char *abs = path_join(cwd, e);
    char *norm = path_normalize(abs);
    free(cwd);
    free(abs);
    free(e);
    return norm;
}

/* processFileArguments; exits 1 on a missing file like omp */
static int process_file_args(const strvec_t *files, sb_t *text, json_t **images) {
    bool auto_resize = settings_bool("images.autoResize", true);
    for (size_t i = 0; i < files->n; i++) {
        char *abs = resolve_file_arg(files->v[i]);
        struct stat st;
        if (stat(abs, &st) != 0) {
            cli_errf("Error: File not found: %s", abs);
            free(abs);
            return 1;
        }
        size_t n = 0;
        unsigned char head[64];
        size_t hn = 0;
        FILE *f = fopen(abs, "rb");
        if (f) {
            hn = fread(head, 1, sizeof head, f);
            fclose(f);
        }
        img_meta_t meta;
        const char *mime = hn && img_sniff((const uint8_t *)head, hn, &meta) ? img_fmt_mime(meta.fmt) : NULL;
        size_t max = mime ? MAX_CLI_IMAGE_BYTES : MAX_CLI_TEXT_BYTES;
        if ((size_t)st.st_size > max) {
            char *b = img_format_bytes((uint64_t)st.st_size);
            char *w = xasprintf("Warning: Skipping file contents (too large: %s): %s", b, abs);
            char *y = cli_yellow(w);
            fprintf(stderr, "%s\n", y);
            free(y);
            free(w);
            sb_appendf(text, "<file name=\"%s\">(skipped: too large, %s)</file>\n", abs, b);
            free(b);
            free(abs);
            continue;
        }
        char *buf = fs_read(abs, &n);
        if (!buf) {
            if (S_ISDIR(st.st_mode)) {
                cli_errf("Error: Could not read file %s: EISDIR: illegal operation on a directory, read", abs);
                free(abs);
                return 1;
            }
            cli_errf("Error: File not found: %s", abs);
            free(abs);
            return 1;
        }
        if (n == 0) {
            free(buf);
            free(abs);
            continue;
        }
        if (mime) {
            char *note = NULL;
            json_t *img = json_obj();
            json_set_str(img, "type", "image");
            bool done = false;
            if (auto_resize) {
                img_resized_t r = {0};
                char *rerr = NULL;
                if (img_resize_image((const uint8_t *)buf, n, mime, NULL, &r, &rerr) == 0 && !r.decode_failed) {
                    note = img_dimension_note(&r);
                    char *b64 = base64_encode(r.buf, r.len);
                    json_set_str(img, "data", b64);
                    json_set_str(img, "mimeType", r.mime);
                    free(b64);
                    done = true;
                }
                img_resized_free(&r);
                free(rerr);
            }
            if (!done) {
                char *b64 = base64_encode(buf, n);
                json_set_str(img, "data", b64);
                json_set_str(img, "mimeType", mime);
                free(b64);
            }
            if (!*images) *images = json_arr();
            json_push(*images, img);
            if (note) sb_appendf(text, "<file name=\"%s\">%s</file>\n", abs, note);
            else sb_appendf(text, "<file name=\"%s\"></file>\n", abs);
            free(note);
        } else {
            char *t = utf8_sanitize(buf, n);
            sb_appendf(text, "<file name=\"%s\">\n%s\n</file>\n", abs, t);
            free(t);
        }
        free(buf);
        free(abs);
    }
    return 0;
}

/* ---------- launch: helpers ---------- */

/* resolvePromptInput-style file read for --system-prompt-template (strict: a missing file fails) */
static char *load_template_file(const char *path, int *rc) {
    char *p = path_expand_home(path);
    size_t n;
    char *t = fs_read(p, &n);
    if (!t) {
        fprintf(stderr, "%s: no such file or directory, open '%s'\n", errno == EISDIR ? "EISDIR" : "ENOENT", path);
        *rc = 1;
        free(p);
        return NULL;
    }
    char *tr = str_trim(t);
    if (!*tr) {
        cli_errf("Error: System prompt template must not be empty: %s", path);
        *rc = 1;
        free(tr);
        free(t);
        free(p);
        return NULL;
    }
    free(tr);
    free(p);
    return t;
}

/* exitWithoutTerminal */
static int exit_without_terminal(void) {
    cli_err_line("Error: interactive mode requires a terminal, but stdin is not a TTY.");
    fprintf(stderr, "Pass a prompt (`%s -p \"…\"`), pipe one on stdin, or use `--mode rpc`.\n", CLI_BIN);
    return 2;
}

static int session_error(const char *msg, bool hint) {
    cli_errf("Error: %s", msg);
    if (hint) {
        char *h = cli_dim("Run `omc --resume` without an argument to pick from recent sessions, or `omc` to start a new one.");
        fprintf(stderr, "%s\n", h);
        free(h);
    }
    return 1;
}

/* readPipedInput: all of stdin when it is not a terminal; NULL when blank */
static char *read_piped_input(void) {
    if (isatty(0)) return NULL;
    struct stat st;
    if (fstat(0, &st) != 0) return NULL;
    /* /dev/null and closed stdin read as empty */
    sb_t sin = SB_INIT;
    char buf[65536];
    ssize_t r;
    bool noticed = false;
    for (;;) {
        if (!noticed && S_ISFIFO(st.st_mode)) {
            struct pollfd pf = {0, POLLIN, 0};
            if (poll(&pf, 1, 1000) == 0) {
                char *d = cli_dim("Reading prompt from piped stdin (waiting for EOF; Ctrl+C to abort)…");
                fprintf(stderr, "%s\n", d);
                free(d);
            }
            noticed = true;
        }
        r = read(0, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        sb_appendn(&sin, buf, (size_t)r);
    }
    char *in = sb_detach(&sin);
    char *t = str_trim(in);
    bool blank = !*t;
    free(t);
    if (blank) {
        free(in);
        return NULL;
    }
    return in;
}

/* --max-time: abort the running turn once the wall-clock deadline passes */
typedef struct {
    app_t *app;
    int64_t deadline;
    volatile bool stop;
} deadline_t;

static void *deadline_main(void *ud) {
    deadline_t *d = ud;
    while (!d->stop) {
        if (now_ms() >= d->deadline) {
            app_abort(d->app);
            break;
        }
        sleep_ms(MIN(200, MAX(1, d->deadline - now_ms())));
    }
    return NULL;
}

/* resolve --models patterns (resolveModelScope): models in order, an explicit `:level` kept */
typedef struct {
    const ai_model_t *m;
    ai_effort_t effort;
    bool has_effort;
    const char *level; /* the explicit `:level` as written */
} scoped_t;

/* ---------- launch ---------- */

static bool g_force_setup; /* runRootCommand(..., { forceSetupWizard: true }) */

static int launch_run(launch_args_t *A, bool force_acp) {
    if (force_acp) A->mode = "acp";
    if (A->version) {
        printf("%s\n", OMC_VERSION);
        return 0;
    }
    if (A->cwd) {
        if (chdir(A->cwd) != 0) {
            int e = errno;
            char *launch = get_cwd();
            const char *code = e == ENOENT ? "ENOENT: no such file or directory" : e == ENOTDIR ? "ENOTDIR: not a directory"
                               : e == EACCES ? "EACCES: permission denied" : strerror(e);
            cli_errf("Error: Cannot change working directory to %s: %s, chdir '%s' -> '%s'.%s", A->cwd, code, launch, A->cwd,
                     e == EACCES || e == EPERM ? " On macOS, grant omc Files & Folders or Full Disk Access permission for "
                                                  "the target directory."
                                               : "");
            free(launch);
            return 1;
        }
    }
    if (A->export_path) {
        if (launch_report_invalid(A)) return 2;
        return export_html_print(A->export_path, A->messages.n ? A->messages.v[0] : NULL);
    }
    bool rpc = str_eq(A->mode, "rpc") || str_eq(A->mode, "rpc-ui");
    if (rpc && A->file_args.n) {
        cli_err_line("Error: @file arguments are not supported in RPC mode");
        return 1;
    }
    bool protocol = rpc || str_eq(A->mode, "acp");
    char *piped = protocol ? NULL : read_piped_input();
    bool stdin_tty = isatty(0);
    bool auto_print = (piped || !stdin_tty) && !A->print && !A->mode;
    bool interactive = !A->print && !auto_print && !A->mode;
    int rc = 0;
    char *cwd = NULL;
    app_t *app = NULL;
    strvec_t add_dirs = {0};
    sb_t file_text = SB_INIT;
    json_t *images = NULL;
    char *template = NULL;
    /* validateGoalLaunch / validateRestoreLaunch */
    if (A->goal) {
        if (!interactive) {
            launch_report_usage_error("--goal requires an interactive terminal (not --print or --mode).");
            rc = 2;
            goto out;
        }
        if (A->continue_recent || A->resume_set || A->fork) {
            launch_report_usage_error("--goal requires a fresh session (no resume, continue, fork, or import).");
            rc = 2;
            goto out;
        }
    }
    if (A->restore) {
        if (!interactive) {
            launch_report_usage_error("--restore requires an interactive terminal (not --print or --mode).");
            rc = 2;
            goto out;
        }
        if (A->continue_recent || A->fork || A->no_session || A->goal) {
            launch_report_usage_error("--restore resumes one existing session (no continue, fork, import, goal, or "
                                      "--no-session).");
            rc = 2;
            goto out;
        }
    }
    bool needs_arg_prompt = auto_print && !piped;
    if (needs_arg_prompt && !A->messages.n && !A->file_args.n && !A->unrecognized.n && !A->invalid_values.n) {
        rc = exit_without_terminal();
        goto out;
    }
    cwd = get_cwd();
    settings_init(cwd, &A->config);
    if (A->approval_mode) settings_set_runtime("tools.approvalMode", json_str(A->approval_mode));
    else if (A->auto_approve) settings_set_runtime("tools.approvalMode", json_str("yolo"));
    if (A->has_warn_cost) settings_set_runtime("session.warnCost", json_num(A->warn_cost));
    if (A->has_max_cost) settings_set_runtime("session.maxCost", json_num(A->max_cost));
    if (A->no_title || protocol) setenv("OMC_NO_TITLE", "1", 1);
    /* model role overrides from CLI args or env vars (ephemeral) */
    const char *smol = A->smol ? A->smol : env_get("PI_SMOL_MODEL");
    const char *slow = A->slow ? A->slow : env_get("PI_SLOW_MODEL");
    const char *plan = A->plan ? A->plan : env_get("PI_PLAN_MODEL");
    if (smol) settings_set_runtime("modelRoles.smol", json_str(smol));
    if (slow) settings_set_runtime("modelRoles.slow", json_str(slow));
    if (plan) settings_set_runtime("modelRoles.plan", json_str(plan));
    if (A->hide_thinking) settings_set_runtime("hideThinkingBlock", json_bool(true));
    if (A->advisor) settings_set_runtime("advisor.enabled", json_bool(true));
    if (A->no_lsp) settings_set_runtime("lsp.enabled", json_bool(false));
    if (g_force_setup) { /* `omc setup`: every wizard scene, whatever the stored setupVersion */
        settings_set_runtime("setupVersion", json_num(0));
        settings_set_runtime("startup.setupWizard", json_bool(true));
    }
    if (A->has_skills && !A->no_skills) {
        json_t *arr = json_arr();
        for (size_t i = 0; i < A->skills.n; i++) json_push_str(arr, A->skills.v[i]);
        settings_set_runtime("skills.includeSkills", arr);
    }
    if (A->system_prompt_template) {
        template = load_template_file(A->system_prompt_template, &rc);
        if (!template) goto out;
    }
    /* --resume without a value opens the session picker */
    bool picker = A->resume_set && !A->resume && !A->fork && !A->no_session;
    if (A->no_session && A->resume_set) {
        rc = session_error("--resume requires session persistence", false);
        goto out;
    }
    if (A->no_session && A->continue_recent) {
        rc = session_error("--continue requires session persistence", false);
        goto out;
    }
    if (A->fork && A->no_session) {
        rc = session_error("--fork requires session persistence", false);
        goto out;
    }
    if (picker) {
        session_list_t l;
        char *dir = A->session_dir ? xstrdup(A->session_dir) : session_dir_for_cwd(cwd);
        session_list(dir, &l);
        free(dir);
        size_t n = l.n;
        session_list_free(&l);
        if (!n) {
            session_list_all(&l);
            n = l.n;
            session_list_free(&l);
            if (!n) {
                char *d = cli_dim("No sessions found");
                printf("%s\n", d);
                free(d);
                goto out;
            }
        }
    }
    const char *resume_path = NULL;
    char *resolved = NULL;
    if (A->resume) {
        resolved = session_resolve(A->resume, cwd);
        if (!resolved) {
            char *m = xasprintf("Session \"%s\" not found.", A->resume);
            rc = session_error(m, true);
            free(m);
            goto out;
        }
        resume_path = resolved;
    }
    /* reportInvalidFlagValues / reportUnrecognizedFlags: exit 2 */
    {
        bool inv = launch_report_invalid(A);
        bool unk = launch_report_unrecognized(A);
        if (inv || unk) {
            rc = 2;
            free(resolved);
            goto out;
        }
    }
    if (A->restore && (A->messages.n || A->file_args.n)) {
        launch_report_usage_error("--restore takes no prompt: the session goes on with its interrupted turn.");
        rc = 2;
        free(resolved);
        goto out;
    }
    if (A->goal) {
        if (A->messages.n || A->file_args.n || piped) {
            launch_report_usage_error("--goal cannot be combined with a positional message, @file, or stdin prompt.");
            rc = 2;
        } else if (A->plan_yolo || A->no_tools) {
            launch_report_usage_error("--goal cannot be combined with --plan-yolo or --no-tools.");
            rc = 2;
        } else if (settings_bool("plan.defaultOnStartup", false) && settings_bool("plan.enabled", true)) {
            launch_report_usage_error("--goal cannot be combined with plan.defaultOnStartup; disable startup plan mode "
                                      "first.");
            rc = 2;
        } else if (!settings_bool("goal.enabled", true)) {
            launch_report_usage_error("--goal requires goal.enabled to be enabled.");
            rc = 2;
        }
        if (rc) {
            free(resolved);
            goto out;
        }
    }
    if (needs_arg_prompt && !A->messages.n && !A->file_args.n) {
        rc = exit_without_terminal();
        free(resolved);
        goto out;
    }
    if (A->file_args.n && process_file_args(&A->file_args, &file_text, &images)) {
        rc = 1;
        free(resolved);
        goto out;
    }
    /* buildInitialMessage */
    char *initial = NULL;
    bool has_ctx = piped || A->file_args.n || images;
    size_t first_rest = 0;
    if (has_ctx) {
        sb_t body = SB_INIT;
        if (A->file_args.n) sb_append(&body, sb_cstr(&file_text));
        if (A->messages.n) {
            sb_append(&body, A->messages.v[0]);
            first_rest = 1;
        }
        if (piped) initial = body.len ? xasprintf("%s\n%s", piped, sb_cstr(&body)) : xstrdup(piped);
        else if (body.len) initial = xstrdup(sb_cstr(&body));
        else if (images) initial = xstrdup("");
        sb_free(&body);
    }
    strvec_t rest = {0};
    for (size_t i = first_rest; i < A->messages.n; i++) strvec_push(&rest, A->messages.v[i]);
    /* --models: the first scoped model starts a fresh session unless --model or a saved default in scope wins */
    VEC(scoped_t) scoped = {0};
    char *scoped_model = NULL;
    if (A->has_models) {
        ai_providers_init();
        catalog_init();
        auth_init();
        for (size_t i = 0; i < A->models.n; i++) {
            if (!*A->models.v[i]) continue;
            scoped_t s = {0};
            s.m = app_resolve_model_pattern(A->models.v[i], &s.effort, &s.has_effort);
            if (s.has_effort && strrchr(A->models.v[i], ':')) s.level = strrchr(A->models.v[i], ':') + 1;
            if (!s.m) {
                char *w = xasprintf("Warning: No models match pattern \"%s\"", A->models.v[i]);
                char *y = cli_yellow(w);
                fprintf(stderr, "%s\n", y);
                free(y);
                free(w);
                continue;
            }
            bool dup = false;
            for (size_t k = 0; k < scoped.n; k++) dup |= scoped.v[k].m == s.m;
            if (!dup) vec_push(&scoped, s);
        }
        bool restoring = A->continue_recent || A->resume_set;
        if (!A->model && scoped.n && !restoring) {
            const scoped_t *pick = &scoped.v[0];
            const char *remembered = settings_str("modelRoles.default", NULL);
            if (remembered) {
                ai_effort_t re;
                bool rh;
                const ai_model_t *rm = app_resolve_model_pattern(remembered, &re, &rh);
                for (size_t k = 0; k < scoped.n && rm; k++)
                    if (scoped.v[k].m == rm) pick = &scoped.v[k];
            }
            scoped_model = xasprintf("%s/%s", pick->m->provider, pick->m->id);
            if (!A->thinking && pick->has_effort && pick->level) A->thinking = pick->level;
        }
    }
    if (A->api_key && !A->model && !scoped_model && !A->has_models) {
        fprintf(stderr, "%s\n", "--api-key requires a model to be specified via --model, --provider/--model, or --models");
        rc = 1;
        goto out2;
    }
    if (A->no_prewalk && (A->prewalk || A->prewalk_into)) {
        fprintf(stderr, "omc: --no-prewalk cannot be combined with --prewalk or --prewalk-into\n");
        rc = 1;
        goto out2;
    }
    app_opts_t o = {0};
    o.cwd = cwd;
    o.model = A->model ? A->model : scoped_model;
    o.provider = A->provider;
    o.api_key = A->api_key;
    o.thinking = A->thinking;
    o.session_dir = A->session_dir;
    o.resume = resume_path;
    o.fork = A->fork;
    o.continue_recent = A->continue_recent;
    o.no_session = A->no_session;
    o.system_prompt = A->system_prompt;
    o.system_prompt_template = template;
    o.append_system_prompt = A->append_system_prompt;
    o.tools = A->has_tools ? &A->tools : NULL;
    o.no_tools = A->no_tools;
    o.no_context_files = A->no_context_files;
    o.no_skills = A->no_skills;
    o.no_rules = A->no_rules;
    o.max_turns = A->max_turns;
    o.auto_approve = A->auto_approve;
    o.service_tier = A->service_tier;
    o.prewalk = A->no_prewalk ? -1 : (A->prewalk || A->prewalk_into) ? 1 : 0;
    o.prewalk_into = A->prewalk_into;
    o.has_ui = interactive;
    /* --add-dir + workspace.additionalDirectories (main.ts: the union, CLI first) */
    if (A->add_dir.n) {
        for (size_t i = 0; i < A->add_dir.n; i++) strvec_push(&add_dirs, A->add_dir.v[i]);
        json_t *x;
        json_arr_foreach(settings_get("workspace.additionalDirectories"), i, x) {
            if (json_is_str(x) && !strvec_contains(&add_dirs, json_sv(x))) strvec_push(&add_dirs, json_sv(x));
        }
        o.add_dirs = &add_dirs;
    }
    o.rpc_ui = str_eq(A->mode, "rpc-ui"); /* the ask tool asks the client through extension_ui_request (WP-15) */
    if (str_eq(A->mode, "acp")) {
        rc = run_acp_mode(&o);
        goto out2;
    }
    bool json = str_eq(A->mode, "json");
    bool print = !interactive && !rpc;
    char *err = NULL;
    o.allow_no_model = interactive && isatty(1); /* omp opens the TUI and lets you /login */
    app = app_create(&o, &err);
    if (!app) {
        fprintf(stderr, "omc: %s\n", err ? err : "failed to start");
        free(err);
        rc = 1;
        goto out2;
    }
    costgate_attach(app);
    if (A->plan_yolo_into && !A->plan_yolo) {
        fprintf(stderr, "omc: --plan-yolo-into requires --plan-yolo\n");
        rc = 1;
        goto out2;
    }
    if (A->plan_yolo) {
        const char *pat = A->plan_yolo_into ? A->plan_yolo_into : "@smol";
        ai_effort_t e = EFFORT_OFF;
        bool has_e = false;
        const ai_model_t *t = app_resolve_model_pattern(pat, &e, &has_e);
        char *key = t ? app_api_key_for(t) : NULL;
        if (!t || !key) {
            if (!t) fprintf(stderr, "omc: Model \"%s\" not found\n", pat);
            else fprintf(stderr, "omc: No API key for %s/%s\n", t->provider, t->id);
            free(key);
            rc = 1;
            goto out2;
        }
        free(key);
        app->plan_yolo_target = t;
        app->plan_yolo_thinking = e;
        app->plan_yolo_has_thinking = has_e;
    }
    if (A->plan_mode) app_set_plan_mode(app, true);
    if (A->goal) {
        char *gerr = NULL;
        char *g = str_trim(A->goal);
        if (app_goal_create(app, g, 0, &gerr) != 0) {
            fprintf(stderr, "omc: %s\n", gerr);
            free(gerr);
            free(g);
            rc = 2;
            goto out2;
        }
        free(initial);
        initial = g;
    }
    deadline_t dl = {.app = app};
    pthread_t dlt;
    bool dl_on = false;
    if (A->has_max_time) {
        dl.deadline = now_ms() + (int64_t)(A->max_time * 1000);
        app_set_deadline(app, dl.deadline);
        dl_on = pthread_create(&dlt, NULL, deadline_main, &dl) == 0;
    }
    if (rpc) {
        rc = run_rpc_mode(app);
    } else if (print) {
        print_opts_t po = {.json = json, .print_thoughts = A->print_thoughts};
        if (initial) strvec_push(&po.messages, initial);
        po.initial_images = images;
        for (size_t i = 0; i < rest.n; i++) strvec_push(&po.messages, rest.v[i]);
        if (!po.messages.n) {
            fprintf(stderr, "omc: no prompt given\n");
            rc = 2;
        } else {
            rc = run_print_mode(app, &po);
        }
        strvec_free(&po.messages);
    } else {
        /* buildModelScopeNotification */
        char *scope_notice = NULL;
        if (scoped.n && !settings_bool("startup.quiet", false)) {
            sb_t b = SB_INIT;
            sb_append(&b, "Model scope: ");
            for (size_t i = 0; i < scoped.n; i++)
                sb_appendf(&b, "%s%s%s%s", i ? ", " : "", scoped.v[i].m->id, scoped.v[i].has_effort && scoped.v[i].level ? ":" : "",
                           scoped.v[i].has_effort && scoped.v[i].level ? scoped.v[i].level : "");
            sb_append(&b, " (Ctrl+P to cycle)");
            scope_notice = sb_detach(&b);
        }
        interactive_start_t st = {.initial = initial, .images = images, .more = &rest, .resume_picker = picker,
                                  .restore = A->restore, .notice = scope_notice};
        images = NULL; /* owned by the interactive start */
        rc = run_interactive_mode_ex(app, &st);
        free(scope_notice);
    }
    if (dl_on) {
        dl.stop = true;
        pthread_join(dlt, NULL);
    }
out2:
    if (app) app_free(app);
    app = NULL;
    strvec_free(&add_dirs);
    {
        char *resume = NULL;
        if (interactive_restart_requested(&resume)) restart_exec(resume); /* returns only when exec failed */
        free(resume);
    }
    free(initial);
    strvec_free(&rest);
    vec_free(&scoped);
    free(scoped_model);
    free(resolved);
out:
    json_free(images);
    sb_free(&file_text);
    free(template);
    free(piped);
    free(cwd);
    http_pool_clear();
    return rc;
}

int cmd_launch_setup(void) {
    g_force_setup = true;
    unsetenv("OMP_SKIP_SETUP");
    unsetenv("OMC_SKIP_SETUP");
    launch_args_t A;
    launch_parse_args(0, NULL, &A);
    int rc = launch_run(&A, false);
    launch_args_free(&A);
    return rc;
}

int cli_launch(int argc, char **argv) {
    launch_args_t A;
    int rc = launch_parse_args(argc, argv, &A);
    if (rc == 0) rc = launch_run(&A, false);
    launch_args_free(&A);
    return rc;
}

/* commands/acp.ts + modes/acp/terminal-auth.ts: ACP mode, unless the client's terminal auth method
 * (--acp-terminal-auth) asks the same command to open the interactive TUI (any --mode is dropped) */
int cli_acp(int argc, char **argv) {
    bool terminal_auth = false;
    for (int i = 0; i < argc; i++) terminal_auth |= !strcmp(argv[i], "--acp-terminal-auth");
    char **av = xcalloc((size_t)argc + 1, sizeof(char *));
    int ac = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--acp-terminal-auth")) continue;
        if (terminal_auth && !strcmp(argv[i], "--mode")) {
            i++;
            continue;
        }
        if (terminal_auth && !strncmp(argv[i], "--mode=", 7)) continue;
        av[ac++] = argv[i];
    }
    launch_args_t A;
    int rc = launch_parse_args(ac, av, &A);
    if (rc == 0) rc = launch_run(&A, !terminal_auth);
    launch_args_free(&A);
    free(av);
    return rc;
}

/* ---------- main ---------- */

static void crash_handler(int sig) {
    void *frames[64];
    int n = backtrace(frames, 64);
    const char msg[] = "omc: fatal signal; backtrace:\n";
    if (write(2, msg, sizeof msg - 1) < 0) {}
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

int omc_main(int argc, char **argv) {
    proc_ignore_sigpipe();
    launch_remember_argv(argc, argv);
    if (env_truthy("OMC_DEBUG")) {
        signal(SIGSEGV, crash_handler);
        signal(SIGABRT, crash_handler);
        signal(SIGBUS, crash_handler);
    }
    const char *lp = env_get("OMC_LOG");
    if (lp) log_init(lp, env_truthy("OMC_DEBUG") ? LOG_DEBUG : LOG_INFO);
    /* main(): no arguments launches */
    char *launch_only[] = {"launch"};
    strvec_t rv = {0};
    char *err = NULL;
    if (argc <= 1) {
        for (int i = 0; i < 1; i++) strvec_push(&rv, launch_only[i]);
    } else if (!resolve_cli_argv(argc - 1, argv + 1, &rv, &err)) {
        fprintf(stderr, "error: %s\n", err);
        free(err);
        return 1;
    }
    int rc;
    const char *id = rv.v[0];
    if (!strcmp(id, "--help") || !strcmp(id, "-h") || !strcmp(id, "help")) {
        cli_render_root_help(stdout);
        rc = 0;
    } else if (!strcmp(id, "--version") || !strcmp(id, "-v")) {
        printf("%s/%s\n", CLI_BIN, OMC_VERSION);
        rc = 0;
    } else {
        const cli_cmd_t *c = cli_find_command(id);
        if (!c) {
            fprintf(stderr, "Error: command %s not found\n", id);
            rc = 1;
        } else {
            rc = cli_run_command(c, (int)rv.n, rv.v);
        }
    }
    strvec_free(&rv);
    fflush(stdout);
    return rc;
}
