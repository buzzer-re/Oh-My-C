/* cli.c - the omc command line (omc_main): subcommands, flags, mode dispatch. */
#include "ai/ai.h"
#include "coding/app.h"
#include "mcp/mcp.h"
#include "config/auth.h"
#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "modes/modes.h"
#include "coding/goal.h"
#include "tools/tools.h"

#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

static void usage(FILE *f) {
    fprintf(f,
            "omc %s - oh-my-c coding agent (C rewrite of oh-my-pi)\n\n"
            "Usage:\n"
            "  omc [options] [prompt...]          interactive session (prompt optional)\n"
            "  omc -p [options] <prompt...>       print mode: run and print the final answer\n"
            "  omc --mode json -p <prompt>        print mode emitting JSON event lines\n"
            "  omc --mode rpc                     JSONL RPC over stdin/stdout\n"
            "  omc --mode acp                     Agent Client Protocol (Zed etc.) over stdio\n\n"
            "Subcommands:\n"
            "  omc models [filter]                list models (with credentials marked)\n"
            "  omc mcp list|login|logout [name]   MCP servers and their OAuth authorization\n"
            "  omc login [provider]               OAuth login (anthropic, openai-codex, ...)\n"
            "  omc logout <provider>              remove stored credentials\n"
            "  omc auth set <provider> <key>      store an API key; `omc auth list`\n"
            "  omc sessions [--all]               list sessions for this directory\n"
            "  omc export <session> [out.html|md] export a session\n"
            "  omc config get|set|unset|list|path manage settings (config.yml)\n"
            "  omc read <path[:sel]>              run the read tool directly\n"
            "  omc grep <pattern> [path]          run the grep tool directly\n"
            "  omc glob [pattern]                 run the glob tool directly\n\n"
            "Options:\n"
            "  -m, --model <pattern[:effort]>     model (fuzzy, provider/id, or @role)\n"
            "      --provider <name>              provider\n"
            "      --api-key <key>                API key for this run\n"
            "      --thinking <level>             off|minimal|low|medium|high|xhigh|max\n"
            "  -c, --continue                     continue the most recent session\n"
            "  -r, --resume <id|path>             resume a session\n"
            "      --fork <id|path>               fork a session into a new one\n"
            "      --no-session                   don't persist the session\n"
            "      --session-dir <dir>            session storage directory\n"
            "      --cwd <dir>                    working directory\n"
            "      --tools <a,b,c>                enable only these tools\n"
            "      --no-tools                     disable all tools\n"
            "      --system-prompt <text|file>    replace the system prompt\n"
            "      --append-system-prompt <t|f>   append to the system prompt\n"
            "      --no-context-files, --no-skills, --no-rules\n"
            "      --config <file>                extra config overlay (repeatable)\n"
            "      --print-thoughts               print thinking to stderr in print mode\n"
            "      --max-turns <n>                stop after n turns\n"
            "      --plan-mode                    start in read-only plan mode\n"
            "      --goal <objective>             start a fresh interactive session in goal mode\n"
            "  -v, --version                      print version\n"
            "  -h, --help                         this help\n",
            OMC_VERSION);
}

static const char *need(int argc, char **argv, int *i) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "omc: %s requires a value\n", argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

/* ---------- subcommands ---------- */

static int cmd_models(int argc, char **argv) {
    ai_providers_init();
    catalog_init();
    auth_init();
    const char *filter = argc > 0 ? argv[0] : NULL;
    bool all = false;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--all")) all = true;
    if (filter && filter[0] == '-') filter = argc > 1 ? argv[1] : NULL;
    size_t shown = 0;
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        bool cred = auth_has_credentials(m->provider);
        if (!all && !cred && !filter) continue;
        if (filter && !str_istr(m->id, filter) && !str_istr(m->provider, filter) && !str_istr(m->name, filter)) continue;
        printf("%s %-24s %-48s ctx=%-8lld %s%s\n", cred ? "*" : " ", m->provider, m->id, (long long)m->context_window,
               m->reasoning ? "reasoning " : "", m->input_image ? "vision" : "");
        shown++;
    }
    if (!shown) fprintf(stderr, "No models%s. Configure an API key or pass --all.\n", filter ? " match" : " with credentials");
    return 0;
}

static void io_print(void *ud, const char *msg) {
    UNUSED(ud);
    fprintf(stderr, "%s\n", msg);
}

static int cmd_login(int argc, char **argv) {
    ai_providers_init();
    catalog_init();
    auth_init();
    const char *provider = argc > 0 ? argv[0] : NULL;
    if (!provider) {
        fprintf(stderr, "Usage: omc login <provider>\nOAuth providers:\n");
        json_t *arr = auth_oauth_providers(), *d;
        json_arr_foreach(arr, i, d) {
            if (str_eq(json_get_str(json_get_obj(d, "login"), "kind"), "oauth-code"))
                fprintf(stderr, "  %-22s %s\n", json_get_str(d, "id"), json_get_str(d, "name") ? json_get_str(d, "name") : "");
        }
        return 2;
    }
    auth_io_t io = {.print = io_print};
    char *err = NULL;
    if (auth_login(provider, &io, &err) != 0) {
        fprintf(stderr, "omc: login failed: %s\n", err ? err : "?");
        free(err);
        return 1;
    }
    fprintf(stderr, "Logged in to %s.\n", provider);
    return 0;
}

/* omc mcp list | login <server> | logout <server> */
static int cmd_mcp(int argc, char **argv) {
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

static int cmd_auth(int argc, char **argv) {
    auth_init();
    if (argc >= 3 && !strcmp(argv[0], "set")) {
        int rc = auth_set_api_key(argv[1], argv[2]);
        if (rc) fprintf(stderr, "omc: failed to save: %s\n", strerror(-rc));
        return rc ? 1 : 0;
    }
    if (argc >= 1 && !strcmp(argv[0], "list")) {
        json_t *l = auth_list(), *e;
        json_arr_foreach(l, i, e) printf("%-24s %s%s%s\n", json_get_str(e, "provider"), json_get_str(e, "type"),
                                         json_get_str(e, "email") ? " " : "", json_get_str(e, "email") ? json_get_str(e, "email") : "");
        json_free(l);
        return 0;
    }
    if (argc >= 2 && (!strcmp(argv[0], "remove") || !strcmp(argv[0], "logout"))) return auth_logout(argv[1]) ? 1 : 0;
    fprintf(stderr, "Usage: omc auth set <provider> <key> | list | remove <provider>\n");
    return 2;
}

static int cmd_sessions(int argc, char **argv) {
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
        printf("%s  %.19s  %3d msgs  %s%s%s\n", session_short_id(s->id), when, s->message_count, t1,
               all ? "  " : "", all && s->cwd ? s->cwd : "");
        free(t1);
        free(when);
    }
    if (!l.n) fprintf(stderr, "No sessions.\n");
    session_list_free(&l);
    return 0;
}

static int cmd_export(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: omc export <session> [out.html|out.md]\n");
        return 2;
    }
    char *cwd = get_cwd();
    char *path = session_resolve(argv[0], cwd);
    free(cwd);
    if (!path) {
        fprintf(stderr, "omc: session not found: %s\n", argv[0]);
        return 1;
    }
    char *err = NULL;
    session_t *s = session_open(path, &err);
    free(path);
    if (!s) {
        fprintf(stderr, "omc: %s\n", err);
        free(err);
        return 1;
    }
    const char *out = argc > 1 ? argv[1] : NULL;
    bool md = out && str_ends(out, ".md");
    char *text = md ? session_export_markdown(s) : session_export_html(s);
    char *dest = out ? xstrdup(out) : xasprintf("omc-session-%s.html", session_short_id(session_id(s)));
    int rc = fs_write(dest, text, strlen(text));
    if (rc == 0) printf("%s\n", dest);
    else fprintf(stderr, "omc: write failed: %s\n", strerror(-rc));
    free(dest);
    free(text);
    session_free(s);
    return rc ? 1 : 0;
}

static json_t *parse_cli_value(const char *s) {
    json_t *j = json_parse_str(s);
    return j ? j : json_str(s);
}

static int cmd_config(int argc, char **argv) {
    settings_init(NULL, NULL);
    if (argc < 1 || !strcmp(argv[0], "list")) {
        bool global = argc > 1 && !strcmp(argv[1], "--scope") && argc > 2 && !strcmp(argv[2], "global");
        char *s = json_dump(global ? settings_global() : settings_effective(), 2);
        printf("%s\n", s);
        free(s);
        return 0;
    }
    if (!strcmp(argv[0], "path")) {
        printf("%s\n", omc_agent_dir());
        return 0;
    }
    if (!strcmp(argv[0], "get") && argc > 1) {
        json_t *v = settings_get(argv[1]);
        if (!v) return 1;
        if (json_is_str(v)) printf("%s\n", json_sv(v));
        else {
            char *s = json_dump(v, 2);
            printf("%s\n", s);
            free(s);
        }
        return 0;
    }
    if (!strcmp(argv[0], "set") && argc > 2) return settings_set_global(argv[1], parse_cli_value(argv[2])) ? 1 : 0;
    if ((!strcmp(argv[0], "unset") || !strcmp(argv[0], "reset")) && argc > 1) return settings_unset_global(argv[1]) ? 1 : 0;
    fprintf(stderr, "Usage: omc config list|path|get <key>|set <key> <value>|unset <key>\n");
    return 2;
}

/* run a builtin tool directly: omc read <path> | grep <pattern> [path] | glob <pattern> */
static int cmd_tool(const char *name, int argc, char **argv) {
    settings_init(NULL, NULL);
    char *cwd = get_cwd();
    tool_env_t *env = tool_env_new(cwd);
    free(cwd);
    agent_tool_t *t = !strcmp(name, "read") ? tool_read_new(env) : !strcmp(name, "grep") ? tool_grep_new(env)
                      : !strcmp(name, "bash") ? tool_bash_new(env) : tool_glob_new(env);
    json_t *args = json_obj();
    if (!strcmp(name, "read")) {
        if (argc < 1) { fprintf(stderr, "Usage: omc read <path[:selector]>\n"); return 2; }
        json_set_str(args, "path", argv[0]);
    } else if (!strcmp(name, "grep")) {
        if (argc < 1) { fprintf(stderr, "Usage: omc grep <pattern> [path]\n"); return 2; }
        json_set_str(args, "pattern", argv[0]);
        if (argc > 1) json_set_str(args, "path", argv[1]);
    } else if (!strcmp(name, "bash")) {
        if (argc < 1) { fprintf(stderr, "Usage: omc bash <command> [timeout-seconds]\n"); return 2; }
        json_set_str(args, "command", argv[0]);
        if (argc > 1) json_set_num(args, "timeout", atof(argv[1]));
    } else {
        if (argc > 0) json_set_str(args, "path", argv[0]);
    }
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
    if (env_truthy("OMC_DEBUG")) {
        signal(SIGSEGV, crash_handler);
        signal(SIGABRT, crash_handler);
        signal(SIGBUS, crash_handler);
    }
    const char *lp = env_get("OMC_LOG");
    if (lp) log_init(lp, env_truthy("OMC_DEBUG") ? LOG_DEBUG : LOG_INFO);
    /* subcommands */
    if (argc > 1) {
        const char *c = argv[1];
        if (!strcmp(c, "models")) return cmd_models(argc - 2, argv + 2);
        if (!strcmp(c, "login")) return cmd_login(argc - 2, argv + 2);
        if (!strcmp(c, "mcp")) return cmd_mcp(argc - 2, argv + 2);
        if (!strcmp(c, "logout")) {
            auth_init();
            return argc > 2 ? (auth_logout(argv[2]) ? 1 : 0) : 2;
        }
        if (!strcmp(c, "auth")) return cmd_auth(argc - 2, argv + 2);
        if (!strcmp(c, "sessions")) return cmd_sessions(argc - 2, argv + 2);
        if (!strcmp(c, "export")) return cmd_export(argc - 2, argv + 2);
        if (!strcmp(c, "config")) return cmd_config(argc - 2, argv + 2);
        if (!strcmp(c, "read") || !strcmp(c, "grep") || !strcmp(c, "glob") || !strcmp(c, "bash")) return cmd_tool(c, argc - 2, argv + 2);
        if (!strcmp(c, "version")) {
            printf("omc %s\n", OMC_VERSION);
            return 0;
        }
        if (!strcmp(c, "help")) {
            usage(stdout);
            return 0;
        }
    }
    app_opts_t o = {0};
    strvec_t tools = {0}, configs = {0}, prompt_parts = {0};
    bool print = false, json = false, rpc = false, acp = false, print_thoughts = false, plan_mode = false;
    const char *mode = NULL;
    const char *goal = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("omc %s\n", OMC_VERSION);
            return 0;
        } else if (!strcmp(a, "-p") || !strcmp(a, "--print")) print = true;
        else if (!strcmp(a, "-m") || !strcmp(a, "--model")) o.model = need(argc, argv, &i);
        else if (!strcmp(a, "--provider")) o.provider = need(argc, argv, &i);
        else if (!strcmp(a, "--api-key")) o.api_key = need(argc, argv, &i);
        else if (!strcmp(a, "--thinking")) o.thinking = need(argc, argv, &i);
        else if (!strcmp(a, "-c") || !strcmp(a, "--continue")) o.continue_recent = true;
        else if (!strcmp(a, "-r") || !strcmp(a, "--resume") || !strcmp(a, "--session")) o.resume = need(argc, argv, &i);
        else if (!strcmp(a, "--fork")) o.fork = need(argc, argv, &i);
        else if (!strcmp(a, "--no-session")) o.no_session = true;
        else if (!strcmp(a, "--session-dir")) o.session_dir = need(argc, argv, &i);
        else if (!strcmp(a, "--cwd")) o.cwd = need(argc, argv, &i);
        else if (!strcmp(a, "--tools")) {
            strvec_t parts = str_split(need(argc, argv, &i), ",");
            for (size_t k = 0; k < parts.n; k++) {
                char *t = str_trim(parts.v[k]);
                if (*t) strvec_push_own(&tools, t);
                else free(t);
            }
            strvec_free(&parts);
            o.tools = &tools;
        } else if (!strcmp(a, "--no-tools")) o.no_tools = true;
        else if (!strcmp(a, "--system-prompt")) o.system_prompt = need(argc, argv, &i);
        else if (!strcmp(a, "--append-system-prompt")) o.append_system_prompt = need(argc, argv, &i);
        else if (!strcmp(a, "--no-context-files")) o.no_context_files = true;
        else if (!strcmp(a, "--no-skills")) o.no_skills = true;
        else if (!strcmp(a, "--no-rules")) o.no_rules = true;
        else if (!strcmp(a, "--config")) strvec_push(&configs, need(argc, argv, &i));
        else if (!strcmp(a, "--mode")) mode = need(argc, argv, &i);
        else if (!strcmp(a, "--print-thoughts")) print_thoughts = true;
        else if (!strcmp(a, "--max-turns")) o.max_turns = atoi(need(argc, argv, &i));
        else if (!strcmp(a, "--no-title")) setenv("OMC_NO_TITLE", "1", 1);
        else if (!strcmp(a, "--plan-mode")) plan_mode = true;
        else if (!strcmp(a, "--goal")) goal = need(argc, argv, &i);
        else if (!strcmp(a, "--")) {
            for (i++; i < argc; i++) strvec_push(&prompt_parts, argv[i]);
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "omc: unknown option %s (see --help)\n", a);
            return 2;
        } else {
            strvec_push(&prompt_parts, a);
        }
    }
    if (mode) {
        if (!strcmp(mode, "json")) json = true;
        else if (!strcmp(mode, "rpc")) rpc = true;
        else if (!strcmp(mode, "acp")) acp = true;
        else if (strcmp(mode, "text") != 0) {
            fprintf(stderr, "omc: unsupported mode %s\n", mode);
            return 2;
        }
    }
    if (o.cwd && chdir(o.cwd) != 0) {
        fprintf(stderr, "omc: cannot chdir to %s: %s\n", o.cwd, strerror(errno));
        return 1;
    }
    char *cwd = get_cwd();
    o.cwd = cwd;
    settings_init(cwd, &configs);
    /* @file arguments inline file contents; piped stdin is appended */
    sb_t prompt = SB_INIT;
    for (size_t i = 0; i < prompt_parts.n; i++) {
        const char *p = prompt_parts.v[i];
        if (p[0] == '@' && fs_is_file(p + 1)) {
            char *c = fs_read(p + 1, NULL);
            sb_appendf(&prompt, "%s<file path=\"%s\">\n%s\n</file>", prompt.len ? "\n" : "", p + 1, c ? c : "");
            free(c);
        } else {
            sb_appendf(&prompt, "%s%s", prompt.len ? " " : "", p);
        }
    }
    bool read_stdin = false;
    if (acp) {
        int arc = run_acp_mode(&o);
        free(cwd);
        return arc;
    }
    if (!rpc && !isatty(0)) {
        struct stat st;
        if (fstat(0, &st) == 0) {
            if (!prompt.len) read_stdin = S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode);
            else if (S_ISREG(st.st_mode)) read_stdin = st.st_size > 0;
            else if (S_ISFIFO(st.st_mode)) {
                struct pollfd pf = {0, POLLIN, 0};
                read_stdin = poll(&pf, 1, 150) > 0;
            }
        }
    }
    if (read_stdin) {
        size_t n;
        char *in = NULL;
        sb_t sin = SB_INIT;
        char buf[65536];
        ssize_t r;
        while ((r = read(0, buf, sizeof buf)) > 0) sb_appendn(&sin, buf, (size_t)r);
        in = sb_detach(&sin);
        n = strlen(in);
        if (n) {
            sb_appendf(&prompt, "%s%s", prompt.len ? "\n\n" : "", in);
            print = true;
        }
        free(in);
    }
    if (json) print = true;
    if (rpc) setenv("OMC_NO_TITLE", "1", 1);
    char *err = NULL;
    o.allow_no_model = !print && !rpc && isatty(0) && isatty(1); /* omp opens the TUI and lets you /login */
    app_t *app = app_create(&o, &err);
    if (!app) {
        fprintf(stderr, "omc: %s\n", err ? err : "failed to start");
        free(err);
        return 1;
    }
    if (plan_mode) app_set_plan_mode(app, true);
    if (goal) {
        if (print || json || rpc || !isatty(0)) {
            fprintf(stderr, "omc: --goal requires an interactive terminal (not --print or --mode).\n");
            app_free(app);
            return 2;
        }
        if (o.resume || o.fork || o.continue_recent || o.no_session) {
            fprintf(stderr, "omc: --goal requires a fresh session (no resume, continue, fork, or no-session).\n");
            app_free(app);
            return 2;
        }
        char *gerr = NULL;
        if (app_goal_create(app, goal, 0, &gerr) != 0) {
            fprintf(stderr, "omc: %s\n", gerr);
            free(gerr);
            app_free(app);
            return 2;
        }
        if (!prompt.len) sb_append(&prompt, goal);
    }
    int rc;
    if (rpc) {
        rc = run_rpc_mode(app);
    } else if (print) {
        if (!prompt.len) {
            fprintf(stderr, "omc: no prompt given\n");
            rc = 2;
        } else {
            print_opts_t po = {.json = json, .print_thoughts = print_thoughts};
            strvec_push(&po.messages, sb_cstr(&prompt));
            rc = run_print_mode(app, &po);
            strvec_free(&po.messages);
        }
    } else {
        rc = run_interactive_mode(app, prompt.len ? sb_cstr(&prompt) : NULL);
    }
    app_free(app);
    sb_free(&prompt);
    strvec_free(&tools);
    strvec_free(&configs);
    strvec_free(&prompt_parts);
    free(cwd);
    http_pool_clear();
    return rc;
}
