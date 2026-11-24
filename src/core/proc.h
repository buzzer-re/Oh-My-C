/* proc.h - subprocess management */
#ifndef OMC_PROC_H
#define OMC_PROC_H

#include "core.h"

#include <signal.h>

typedef void (*proc_output_cb)(void *ud, int stream /*1=stdout,2=stderr*/, const char *data, size_t len);

typedef struct {
    const char *cwd;
    const char *const *env_extra; /* NULL-terminated "K=V" entries added/overriding; a bare "K" unsets K */
    const char *stdin_data;       /* NULL => /dev/null */
    size_t stdin_len;
    int64_t timeout_ms;           /* 0 => none */
    size_t max_output;            /* per stream cap kept in memory (0 => 16MB) */
    bool merge_stderr;            /* stderr into stdout */
    bool new_session;             /* setsid (default true via proc_run) */
    bool use_pty;                 /* run attached to a pseudo-terminal */
    volatile sig_atomic_t *cancel;/* polled; nonzero => kill */
    proc_output_cb on_output;
    void *ud;
    /* shell_run only: text run in the session shell right before the command file is sourced, on the wrapper line
     * (the command's own line numbers stay as written); the bash tool's tool-command functions */
    const char *shell_preamble;
    /* shell_run / shell_exec_once only: the command is a PowerShell script, run in its own PowerShell process (the
     * Windows `powershell` tool); implied when proc_shell_is_powershell() */
    bool powershell;
} proc_opts_t;

typedef struct {
    int exit_code;     /* -1 if killed by signal */
    int term_signal;
    bool timed_out;
    bool cancelled;
    bool truncated;    /* output exceeded max_output */
    char *out;         /* owned */
    size_t out_len;
    char *err;         /* owned (empty if merged) */
    size_t err_len;
    int spawn_errno;   /* non-zero if exec failed */
} proc_result_t;

int proc_run(const char *const *argv, const proc_opts_t *opts, proc_result_t *res);
void proc_result_free(proc_result_t *r);
/* convenience: run and return trimmed stdout or NULL on failure/non-zero */
char *proc_capture(const char *const *argv, const char *cwd, int64_t timeout_ms);
const char *proc_shell_path(void); /* bash if available, else /bin/sh (Windows: PowerShell) */
/* Windows without a bash: proc_shell_path() is PowerShell (pwsh.exe, else Windows PowerShell 5.1) and commands are
 * PowerShell scripts. Always false elsewhere. */
bool proc_shell_is_powershell(void);
/* PowerShell for proc_ps_argv: pwsh.exe when installed, else Windows PowerShell 5.1 (Windows; "pwsh" elsewhere) */
const char *proc_powershell_path(void);
/* The PowerShell script for `cmd`: the command, then an exit status like a shell's (`exit N` passes through; else 0
 * when its last statement succeeded, the last native exit code or 1 when it failed). */
char *proc_ps_script(const char *cmd);
/* NULL-terminated argv running `script` (from proc_ps_script) in PowerShell with UTF-8 output and no progress bars.
 * `file` names a .ps1 to write the script to (long scripts, true line numbers); NULL passes it on the command line. */
strvec_t proc_ps_argv(const char *script, const char *file);
const char *proc_shell_name(void); /* its name in the shell's own messages: `bash` (argv[0] when run through this path) */
int proc_shell(const char *cmd, const proc_opts_t *opts, proc_result_t *res); /* bash -c / sh -c */
/* run a shell command, return trimmed stdout if exit 0 (owned) else NULL */
char *proc_shell_capture(const char *cmd, const char *cwd, int64_t timeout_ms);

/* long-lived child with pipes (LSP, MCP stdio, RPC peers) */
typedef struct {
    pid_t pid;
    int in_fd;   /* write to child's stdin */
    int out_fd;  /* read child's stdout */
    int err_fd;  /* read child's stderr (-1 if inherited) */
} child_t;

int child_spawn(const char *const *argv, const char *cwd, const char *const *env_extra, bool capture_stderr,
                child_t *out);
int child_wait(child_t *c, int64_t timeout_ms); /* returns exit status or -1 */
void child_kill(child_t *c, int sig);           /* signals the process group */
void child_close(child_t *c);                    /* closes fds, kills, reaps */

/* signal utilities */
void proc_ignore_sigpipe(void);

#endif
