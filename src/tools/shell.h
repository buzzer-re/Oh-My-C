/* shell.h - persistent bash session: state (env, functions, aliases) survives across tool calls. */
#ifndef OMC_SHELL_H
#define OMC_SHELL_H

#include "core/proc.h"

typedef struct shell shell_t;

shell_t *shell_new(void);
void shell_free(shell_t *s);
/* Run `cmd` in the persistent shell with cwd; same result contract as proc_run (merged output). */
int shell_run(shell_t *s, const char *cmd, const char *cwd, const proc_opts_t *o, proc_result_t *res);

#endif
