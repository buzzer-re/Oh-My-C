/* shell.h - persistent bash session: state (env, functions, aliases) survives across tool calls, while everything else
 * stays scoped to one command. Each command is a sourced file with its own output pipe: `exec >file` ends with it, an `EXIT`
 * trap fires once at its end, and a background job's late output reaches nothing (the job keeps running; a thread reads and
 * drops what it writes). A syntax error fails that command alone. */
#ifndef OMC_SHELL_H
#define OMC_SHELL_H

#include "core/proc.h"

typedef struct shell shell_t;

shell_t *shell_new(void);
void shell_free(shell_t *s);
/* Run `cmd` in the persistent shell with cwd; same result contract as proc_run (merged output). */
int shell_run(shell_t *s, const char *cmd, const char *cwd, const proc_opts_t *o, proc_result_t *res);
/* Drop bash job-status notices (`<sh>: line N: PID Killed  cmd`) from complete lines of a `bash -c` output. */
void shell_strip_job_notices(char *buf, size_t *len);
/* a command in a temporary session (`pty`: proc_shell under a pty, with those notices dropped from the streamed and the
 * collected output) */
int shell_exec_once(const char *cmd, const proc_opts_t *o, proc_result_t *res);
/* a command in a copy of `parent`, which another command holds: a temporary session seeded with the state that command
 * started from (variables, functions, aliases, options); what it sets stays in the copy. Without a published state (or
 * under a pty) it is shell_exec_once. */
int shell_exec_fork(shell_t *parent, const char *cmd, const proc_opts_t *o, proc_result_t *res);
/* like shell_run, but returns -2 at once when another command holds the shell */
int shell_run_try(shell_t *s, const char *cmd, const char *cwd, const proc_opts_t *o, proc_result_t *res);

#endif
