/* proc.h - subprocess management */
#ifndef OMC_PROC_H
#define OMC_PROC_H

#include "core.h"

#include <signal.h>

typedef void (*proc_output_cb)(void *ud, int stream /*1=stdout,2=stderr*/, const char *data, size_t len);

typedef struct {
    const char *cwd;
    const char *const *env_extra; /* NULL-terminated "K=V" entries added/overriding */
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
const char *proc_shell_path(void); /* bash if available, else /bin/sh */
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
