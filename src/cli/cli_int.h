/* cli_int.h - internals shared by the src/cli files: the command entry points and small helpers. */
#ifndef OMC_CLI_INT_H
#define OMC_CLI_INT_H

#include "cli/cmd.h"

#include <stdarg.h>

/* omp commands ported so far (the others are refused by resolve_cli_argv) */
#define OMC_CLI_WORKTREE 1

/* launch (cli.c) */
int cli_launch(int argc, char **argv);
int cli_acp(int argc, char **argv);
extern const cflag_t LAUNCH_FLAGS[];

/* subcommands (cparse_t-based) */
int cmd_agents(cparse_t *p);
int cmd_auth(cparse_t *p);
int cmd_completions_raw(int argc, char **argv);
int cmd_launch_setup(void); /* `omc setup`: the launch with the first-run wizard forced */
int cmd_complete(int argc, char **argv);
int cmd_config(cparse_t *p);
int cmd_grep(cparse_t *p);
int cmd_login(cparse_t *p);
int cmd_models(cparse_t *p);
int cmd_read(cparse_t *p);
int cmd_play(cparse_t *p); /* sub_play.c */
int cmd_search(cparse_t *p);
int cmd_session(cparse_t *p);
int cmd_setup(cparse_t *p);
int cmd_ssh(cparse_t *p);
int cmd_token(cparse_t *p);
int cmd_ttsr(cparse_t *p);
int cmd_usage(cparse_t *p);
int cmd_worktree(cparse_t *p);

/* omc extras (raw argv) */
int cmd_migrate_raw(int argc, char **argv);
int cmd_mcp_raw(int argc, char **argv);
int cmd_export_raw(int argc, char **argv);
int cmd_sessions_raw(int argc, char **argv);
int cmd_logout_raw(int argc, char **argv);
int cmd_glob_raw(int argc, char **argv);
int cmd_bash_raw(int argc, char **argv);
int cmd_version_raw(int argc, char **argv);

/* helpers */
void cli_err_line(const char *msg);       /* chalk.red(msg) + "\n" on stderr */
void cli_errf(const char *fmt, ...) PRINTF_LIKE(1, 2); /* same, formatted */
void cli_init_runtime(const strvec_t *configs); /* providers, catalog, auth, settings for the cwd */
char *cli_json_pretty(const json_t *j);   /* JSON.stringify(v, null, 2) (owned) */

#endif
