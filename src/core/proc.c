#define _GNU_SOURCE
#include "proc.h"
#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <util.h> /* openpty, forkpty */
#else
#include <pty.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "core/compat_sys.h"

extern char **environ;

void proc_ignore_sigpipe(void) {
    struct sigaction sa;
    /* libomc shares the process with its host: a handler the host installed stays (writes still get EPIPE) */
    if (sigaction(SIGPIPE, NULL, &sa) == 0 && ((sa.sa_flags & SA_SIGINFO) || sa.sa_handler != SIG_DFL)) return;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

static void close_pair(int p[2]) {
    for (int k = 0; k < 2; k++)
        if (p[k] >= 0) {
            close(p[k]);
            p[k] = -1;
        }
}

static char **build_env(const char *const *extra) {
    size_t n = 0;
    while (environ[n]) n++;
    size_t m = 0;
    if (extra)
        while (extra[m]) m++;
    char **env = xcalloc(n + m + 1, sizeof(char *));
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *eq = strchr(environ[i], '=');
        size_t kl = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
        bool overridden = false;
        for (size_t j = 0; j < m; j++)
            if (strncmp(extra[j], environ[i], kl) == 0 && (extra[j][kl] == '=' || extra[j][kl] == 0)) overridden = true;
        if (!overridden) env[k++] = environ[i];
    }
    for (size_t j = 0; j < m; j++)
        if (strchr(extra[j], '=')) env[k++] = (char *)extra[j]; /* a bare NAME unsets it */
    env[k] = NULL;
    return env;
}

static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static char *g_shell_path; /* proc_shell_path() once known */

#ifndef _WIN32
/* exec in a forked child: the shell runs as `bash`, so its messages read `bash: line 1: ...` like omp's */
static void exec_argv(const char *const *argv, char **env) {
    if (g_shell_path && argv[0] == g_shell_path) {
        size_t n = 0;
        while (argv[n]) n++;
        const char *v[n + 1];
        for (size_t i = 0; i <= n; i++) v[i] = argv[i];
        const char *b = strrchr(g_shell_path, '/');
        if (b) v[0] = b + 1;
        execve(g_shell_path, (char *const *)v, env);
        return;
    }
    execvpe(argv[0], (char *const *)argv, env);
}

static void reset_child_signals(void) {
    sigset_t s;
    sigemptyset(&s);
    sigprocmask(SIG_SETMASK, &s, NULL);
    for (int i = 1; i < 32; i++) {
        if (i == SIGKILL || i == SIGSTOP) continue;
        signal(i, SIG_DFL);
    }
}
#endif

#ifdef _WIN32
/* Windows: no fork. The child starts with the pipe ends as its stdio (only those are inherited); a program that can't
 * start is known right away (spawn_errno), as the exec_p pipe tells it on POSIX. The shell runs as `bash` (argv[0])
 * while the program is the full path. Returns the pid or -1 with errno. */
static pid_t win_start(const char *const *argv, char **env, const char *cwd, int in_fd, int out_fd, int err_fd) {
    size_t n = 0;
    while (argv[n]) n++;
    const char **v = xcalloc(n + 1, sizeof *v);
    for (size_t i = 0; i < n; i++) v[i] = argv[i];
    omc_win_spawn_opts_t so = {.cwd = cwd, .envp = env, .fd_in = in_fd, .fd_out = out_fd, .fd_err = err_fd};
    if (g_shell_path && argv[0] == g_shell_path) {
        so.exe = g_shell_path;
        v[0] = proc_shell_name();
    }
    pid_t pid = omc_win_spawn((char *const *)v, &so);
    free(v);
    return pid;
}
#endif

int proc_run(const char *const *argv, const proc_opts_t *o, proc_result_t *res) {
    proc_opts_t def = {0};
    if (!o) o = &def;
    memset(res, 0, sizeof(*res));
    size_t cap = o->max_output ? o->max_output : (16u << 20);

    int in_p[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1}, exec_p[2] = {-1, -1};
    int master = -1;
#ifdef _WIN32
    pid_t pid = -1;
    int pty_in = -1;
    {
        (void)master;
        char **env = build_env(o->env_extra);
        int spawn_err = 0;
        if (o->use_pty) {
            /* a pseudo console stands in for the pty: 200x40 like the POSIX one */
            size_t n = 0;
            while (argv[n]) n++;
            const char **v = xcalloc(n + 1, sizeof *v);
            for (size_t i = 0; i < n; i++) v[i] = argv[i];
            pid = omc_win_pty_spawn((char *const *)v, o->cwd, env, 200, 40, &pty_in, &out_p[0]);
            spawn_err = errno;
            free(v);
            if (pid >= 0 && o->stdin_data) in_p[1] = pty_in, pty_in = -1;
        } else {
            if (o->stdin_data && pipe2(in_p, O_CLOEXEC) != 0) goto fail;
            if (pipe2(out_p, O_CLOEXEC) != 0) goto fail;
            if (!o->merge_stderr && pipe2(err_p, O_CLOEXEC) != 0) goto fail;
            pid = win_start(argv, env, o->cwd, o->stdin_data ? in_p[0] : -1, out_p[1], o->merge_stderr ? out_p[1] : err_p[1]);
            spawn_err = errno;
            if (in_p[0] >= 0) close(in_p[0]), in_p[0] = -1;
            close(out_p[1]);
            out_p[1] = -1;
            if (err_p[1] >= 0) close(err_p[1]), err_p[1] = -1;
        }
        free(env);
        if (pid < 0) {
            res->spawn_errno = spawn_err ? spawn_err : ENOENT;
            close_pair(in_p);
            close_pair(out_p);
            close_pair(err_p);
            res->exit_code = 127;
            res->out = xstrdup("");
            res->err = xasprintf("%s: %s", argv[0], strerror(res->spawn_errno));
            res->err_len = strlen(res->err);
            return 0;
        }
    }
#else
    /* a failed pipe/openpty/fork (EMFILE, EAGAIN) must not leak the fds opened before it */
    if (pipe2(exec_p, O_CLOEXEC) != 0) goto fail;
    if (o->stdin_data && pipe2(in_p, O_CLOEXEC) != 0) goto fail;
    if (o->use_pty) {
        struct winsize ws = {.ws_row = 40, .ws_col = 200};
        int slave;
        if (openpty(&master, &slave, NULL, NULL, &ws) != 0) goto fail;
        out_p[0] = master;
        out_p[1] = slave;
        fcntl(master, F_SETFD, FD_CLOEXEC);
    } else {
        if (pipe2(out_p, O_CLOEXEC) != 0) goto fail;
        if (!o->merge_stderr && pipe2(err_p, O_CLOEXEC) != 0) goto fail;
    }
    char **env = build_env(o->env_extra);

    pid_t pid = fork();
    if (pid < 0) {
        free(env);
        goto fail;
    }
    if (pid == 0) {
        reset_child_signals();
        if (o->new_session || o->use_pty) setsid();
        if (o->use_pty) ioctl(out_p[1], TIOCSCTTY, 0);
        int devnull = open("/dev/null", O_RDWR);
        dup2(o->stdin_data ? in_p[0] : (o->use_pty ? out_p[1] : devnull), 0);
        dup2(out_p[1], 1);
        dup2(o->merge_stderr || o->use_pty ? out_p[1] : err_p[1], 2);
        if (o->cwd && chdir(o->cwd) != 0) {
            int e = errno;
            if (write(exec_p[1], &e, sizeof e) < 0) {}
            _exit(127);
        }
        exec_argv(argv, env);
        int e = errno;
        if (write(exec_p[1], &e, sizeof e) < 0) {}
        _exit(127);
    }
    free(env);
    close(exec_p[1]);
    if (o->stdin_data) close(in_p[0]);
    close(out_p[1]);
    if (err_p[1] >= 0) close(err_p[1]);

    int child_errno = 0;
    ssize_t er;
    do er = read(exec_p[0], &child_errno, sizeof child_errno);
    while (er < 0 && errno == EINTR);
    close(exec_p[0]);
    if (er == sizeof child_errno) {
        res->spawn_errno = child_errno;
        if (in_p[1] >= 0) close(in_p[1]);
        close(out_p[0]);
        if (err_p[0] >= 0) close(err_p[0]);
        waitpid(pid, NULL, 0);
        res->exit_code = 127;
        res->out = xstrdup("");
        res->err = xasprintf("%s: %s", argv[0], strerror(child_errno));
        res->err_len = strlen(res->err);
        return 0;
    }
#endif

    sb_t out = SB_INIT, err = SB_INIT;
    size_t in_off = 0;
    int in_fd = in_p[1];
    if (in_fd >= 0) set_nonblock(in_fd);
    int out_fd = out_p[0], err_fd = err_p[0];
    set_nonblock(out_fd);
    if (err_fd >= 0) set_nonblock(err_fd);
    int64_t deadline = o->timeout_ms > 0 ? mono_ms() + o->timeout_ms : 0;
    bool killed = false;
    int status = 0;
    bool reaped = false;

    while (out_fd >= 0 || err_fd >= 0 || in_fd >= 0) {
        struct pollfd pf[3];
        int np = 0, io = -1, oo = -1, eo = -1;
        if (in_fd >= 0) { io = np; pf[np++] = (struct pollfd){in_fd, POLLOUT, 0}; }
        if (out_fd >= 0) { oo = np; pf[np++] = (struct pollfd){out_fd, POLLIN, 0}; }
        if (err_fd >= 0) { eo = np; pf[np++] = (struct pollfd){err_fd, POLLIN, 0}; }
        int wait = 100;
        int pr = poll(pf, (nfds_t)np, wait);
        if (pr < 0 && errno != EINTR) break;
        if (!killed && ((o->cancel && *o->cancel) || (deadline && mono_ms() > deadline))) {
            if (o->cancel && *o->cancel) res->cancelled = true;
            else res->timed_out = true;
            kill(-pid, SIGTERM);
            kill(pid, SIGTERM);
            killed = true;
            /* give it a moment, then SIGKILL */
            int64_t grace = mono_ms() + 1500;
            while (mono_ms() < grace) {
                if (waitpid(pid, &status, WNOHANG) == pid) {
                    reaped = true;
                    break;
                }
                sleep_ms(20);
            }
            if (!reaped) {
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
            }
        }
        if (pr <= 0) {
            /* PTY: child may have exited while grandchildren keep slave open; check */
            if (o->use_pty && !reaped && waitpid(pid, &status, WNOHANG) == pid) {
                reaped = true;
            }
            if (killed && out_fd >= 0) {
                /* drain whatever is left without blocking forever */
                char tmp[4096];
                while (read(out_fd, tmp, sizeof tmp) > 0) {}
                close(out_fd);
                out_fd = -1;
                if (err_fd >= 0) { close(err_fd); err_fd = -1; }
                if (in_fd >= 0) { close(in_fd); in_fd = -1; }
            }
            if (reaped && o->use_pty && out_fd >= 0) {
                char tmp[4096];
                ssize_t r;
                while ((r = read(out_fd, tmp, sizeof tmp)) > 0) {
                    if (out.len < cap) sb_appendn(&out, tmp, MIN((size_t)r, cap - out.len));
                    if (o->on_output) o->on_output(o->ud, 1, tmp, (size_t)r);
                }
                close(out_fd);
                out_fd = -1;
            }
            continue;
        }
        if (io >= 0 && (pf[io].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (pf[io].revents & POLLOUT) {
                ssize_t w = write(in_fd, o->stdin_data + in_off, o->stdin_len - in_off);
                if (w > 0) in_off += (size_t)w;
                if (w < 0 && errno != EAGAIN && errno != EINTR) in_off = o->stdin_len;
            } else {
                in_off = o->stdin_len;
            }
            if (in_off >= o->stdin_len) {
                close(in_fd);
                in_fd = -1;
            }
        }
        int fds[2] = {oo, eo};
        for (int k = 0; k < 2; k++) {
            int idx = fds[k];
            if (idx < 0) continue;
            if (!(pf[idx].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int *fdp = k == 0 ? &out_fd : &err_fd;
            sb_t *b = k == 0 ? &out : &err;
            char buf[16384];
            ssize_t r = read(*fdp, buf, sizeof buf);
            if (r > 0) {
                if (b->len < cap) sb_appendn(b, buf, MIN((size_t)r, cap - b->len));
                else res->truncated = true;
                if (o->on_output) o->on_output(o->ud, k + 1, buf, (size_t)r);
            } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                close(*fdp);
                *fdp = -1;
            }
        }
    }
    if (!reaped) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
#ifdef _WIN32
    if (pty_in >= 0) close(pty_in);
#endif
    if (WIFEXITED(status)) {
        res->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res->exit_code = -1;
        res->term_signal = WTERMSIG(status);
    }
    res->out_len = out.len;
    res->out = sb_detach(&out);
    res->err_len = err.len;
    res->err = sb_detach(&err);
    return 0;
fail:
    close_pair(exec_p);
    close_pair(in_p);
    close_pair(out_p);
    close_pair(err_p);
    return -1;
}

void proc_result_free(proc_result_t *r) {
    free(r->out);
    free(r->err);
    r->out = r->err = NULL;
}

char *proc_capture(const char *const *argv, const char *cwd, int64_t timeout_ms) {
    proc_opts_t o = {.cwd = cwd, .timeout_ms = timeout_ms ? timeout_ms : 10000, .new_session = true};
    proc_result_t r;
    if (proc_run(argv, &o, &r) != 0) return NULL;
    char *ret = NULL;
    if (r.exit_code == 0 && !r.spawn_errno) {
        size_t n = r.out_len;
        while (n && (r.out[n - 1] == '\n' || r.out[n - 1] == '\r')) n--;
        ret = xstrndup(r.out, n);
    }
    proc_result_free(&r);
    return ret;
}

#ifdef _WIN32
/* procmgr.ts resolveWindowsShell: Git for Windows' bash, then a bash on PATH that is not WSL's launcher
 * (System32\bash.exe, the WindowsApps alias), then a sh.exe (its sibling bash.exe if any). NULL when there is none:
 * omp then runs its embedded shell; omc runs PowerShell (win_powershell). */
static char *win_resolve_shell(void) {
    const char *roots[6][2] = {{env_get("ProgramFiles"), "Git"},
                               {env_get("ProgramFiles(x86)"), "Git"},
                               {env_get("LOCALAPPDATA"), "Programs/Git"},
                               {env_get("GIT_INSTALL_ROOT"), ""},
                               {env_get("SCOOP"), "apps/git/current"},
                               {env_get("USERPROFILE"), "scoop/apps/git/current"}};
    for (size_t i = 0; i < ARRAY_LEN(roots); i++) {
        if (!roots[i][0]) continue;
        char *c = *roots[i][1] ? xasprintf("%s/%s/bin/bash.exe", roots[i][0], roots[i][1]) : xasprintf("%s/bin/bash.exe", roots[i][0]);
        if (fs_is_file(c)) {
            for (char *p = c; *p; p++)
                if (*p == '\\') *p = '/';
            return c;
        }
        free(c);
    }
    const char *sr = env_get("SystemRoot");
    if (!sr) sr = env_get("windir");
    char *sys = xasprintf("%s/", sr ? sr : "C:/Windows");
    for (char *p = sys; *p; p++) *p = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
    const char *path = getenv("PATH");
    char *found = NULL;
    if (path) {
        strvec_t dirs = str_split(path, ";");
        for (size_t i = 0; i < dirs.n && !found; i++) {
            if (!*dirs.v[i]) continue;
            char *c = xasprintf("%s/bash.exe", dirs.v[i]);
            char *low = xstrdup(c);
            for (char *p = low; *p; p++) *p = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
            if (fs_is_file(c) && !str_starts(low, sys) && !strstr(low, "/microsoft/windowsapps/")) found = c;
            else free(c);
            free(low);
        }
        strvec_free(&dirs);
    }
    free(sys);
    if (!found) {
        char *sh = omc_win_find_exe("sh.exe");
        if (sh) {
            char *sib = path_dirname(sh);
            char *b = xasprintf("%s/bash.exe", sib);
            free(sib);
            if (fs_is_file(b)) {
                free(sh);
                found = b;
            } else {
                free(b);
                found = sh;
            }
        }
    }
    if (found)
        for (char *p = found; *p; p++)
            if (*p == '\\') *p = '/';
    return found;
}
#endif

#ifdef _WIN32
/* no bash: PowerShell 7 when installed, else the Windows PowerShell every Windows 10/11 has */
static char *win_powershell(void) {
    char *p = omc_win_find_exe("pwsh.exe");
    if (!p) {
        const char *sr = env_get("SystemRoot");
        if (!sr) sr = env_get("windir");
        p = xasprintf("%s/System32/WindowsPowerShell/v1.0/powershell.exe", sr ? sr : "C:/Windows");
        if (!fs_is_file(p)) {
            free(p);
            p = xstrdup("powershell.exe");
        }
    }
    for (char *q = p; *q; q++)
        if (*q == '\\') *q = '/';
    return p;
}
#endif

static bool g_shell_ps; /* proc_shell_path() is PowerShell */

const char *proc_shell_path(void) {
    if (g_shell_path) return g_shell_path;
    char *sh = NULL;
    const char *e = env_get("OMC_SHELL");
    if (e && fs_is_exec(e)) sh = xstrdup(e);
#ifdef _WIN32
    if (!sh) sh = win_resolve_shell();
    if (!sh) sh = win_powershell();
    const char *base = strrchr(sh, '/');
    base = base ? base + 1 : sh;
    g_shell_ps = !strcasecmp(base, "powershell.exe") || !strcasecmp(base, "pwsh.exe") || !strcasecmp(base, "powershell") ||
                 !strcasecmp(base, "pwsh");
#else
    if (!sh) sh = which("bash");
    if (!sh) sh = xstrdup("/bin/sh");
#endif
    g_shell_path = sh;
    return sh;
}

bool proc_shell_is_powershell(void) {
    (void)proc_shell_path();
    return g_shell_ps;
}

static pthread_once_t g_ps_once = PTHREAD_ONCE_INIT;
static char *g_ps_path;

static void find_powershell(void) {
#ifdef _WIN32
    g_ps_path = win_powershell();
#else
    g_ps_path = xstrdup("pwsh");
#endif
}

const char *proc_powershell_path(void) {
    if (proc_shell_is_powershell()) return proc_shell_path();
    pthread_once(&g_ps_once, find_powershell);
    return g_ps_path;
}

/* `$?` right after the command's last statement is its status, as `$?` is in a shell; an `exit N` inside never
 * reaches the check. The blank line ends a trailing backtick continuation. */
char *proc_ps_script(const char *cmd) {
    return xasprintf("%s\n\nif ($?) { exit 0 } elseif ($LASTEXITCODE) { exit $LASTEXITCODE } else { exit 1 }\n", cmd);
}

/* UTF-8 both ways without a BOM (Windows PowerShell 5.1 otherwise writes the OEM code page into a pipe), no progress
 * records, and a fresh $LASTEXITCODE so a failed cmdlet is not mistaken for an earlier native failure */
#define PS_PRELUDE                                                                                                   \
    "$ProgressPreference = 'SilentlyContinue'; $OutputEncoding = New-Object System.Text.UTF8Encoding($false); "        \
    "try { [Console]::OutputEncoding = $OutputEncoding } catch { }; $global:LASTEXITCODE = $null"

strvec_t proc_ps_argv(const char *script, const char *file) {
    strvec_t v = {0};
    strvec_push(&v, proc_powershell_path());
    static const char *const flags[] = {"-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass"};
    for (size_t i = 0; i < ARRAY_LEN(flags); i++) strvec_push(&v, flags[i]);
    sb_t body = SB_INIT;
    if (file) {
        sb_append(&body, "\xEF\xBB\xBF"); /* 5.1 reads a .ps1 without a BOM in the ANSI code page */
        sb_append(&body, script);
    }
    if (file && fs_write(file, body.s, body.len) == 0) {
        char *q = str_replace(file, "'", "''");
        strvec_push(&v, "-Command");
        strvec_push_own(&v, xasprintf(PS_PRELUDE "; & '%s'; exit $LASTEXITCODE", q));
        free(q);
    } else {
        /* -EncodedCommand: UTF-16LE in base64, so no quoting of the script survives or breaks the command line */
        char *text = xasprintf(PS_PRELUDE "\n%s", script);
        size_t n = strlen(text);
        uint16_t *w = xmalloc((n + 1) * 2 * sizeof *w);
        size_t wn = 0;
        for (size_t i = 0; i < n;) {
            uint32_t cp;
            int len = utf8_decode(text + i, n - i, &cp);
            if (len <= 0) {
                cp = 0xFFFD;
                len = 1;
            }
            i += (size_t)len;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                w[wn++] = (uint16_t)(0xD800 + (cp >> 10));
                w[wn++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            } else w[wn++] = (uint16_t)cp;
        }
        unsigned char *le = xmalloc(wn * 2 + 1);
        for (size_t i = 0; i < wn; i++) le[2 * i] = (unsigned char)(w[i] & 0xFF), le[2 * i + 1] = (unsigned char)(w[i] >> 8);
        strvec_push(&v, "-EncodedCommand");
        strvec_push_own(&v, base64_encode(le, wn * 2));
        free(le);
        free(w);
        free(text);
    }
    sb_free(&body);
    strvec_push_own(&v, NULL);
    return v;
}

int proc_shell(const char *cmd, const proc_opts_t *o, proc_result_t *res) {
    if (proc_shell_is_powershell()) {
        char *script = proc_ps_script(cmd);
        strvec_t argv = proc_ps_argv(script, NULL);
        int rc = proc_run((const char **)argv.v, o, res);
        strvec_free(&argv);
        free(script);
        return rc;
    }
    const char *sh = proc_shell_path();
    const char *argv[] = {sh, "-c", cmd, NULL};
    return proc_run(argv, o, res);
}

char *proc_shell_capture(const char *cmd, const char *cwd, int64_t timeout_ms) {
    if (proc_shell_is_powershell()) {
        char *script = proc_ps_script(cmd);
        strvec_t argv = proc_ps_argv(script, NULL);
        char *r = proc_capture((const char **)argv.v, cwd, timeout_ms);
        strvec_free(&argv);
        free(script);
        return r;
    }
    const char *argv[] = {proc_shell_path(), "-c", cmd, NULL};
    return proc_capture(argv, cwd, timeout_ms);
}

const char *proc_shell_name(void) {
    const char *sh = proc_shell_path(), *b = strrchr(sh, '/');
#ifdef _WIN32
    /* `bash`, as the shell names itself in its messages: no directory, no .exe */
    static char *name;
    if (!name) {
        const char *bs = strrchr(sh, '\\');
        if (bs && (!b || bs > b)) b = bs;
        name = xstrdup(b ? b + 1 : sh);
        size_t n = strlen(name);
        if (n > 4 && !strcasecmp(name + n - 4, ".exe")) name[n - 4] = 0;
    }
    return name;
#else
    return b ? b + 1 : sh;
#endif
}

/* ---------- long-lived child ---------- */

int child_spawn(const char *const *argv, const char *cwd, const char *const *env_extra, bool capture_stderr,
                child_t *c) {
#ifdef _WIN32
    int in_p[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1};
    if (pipe2(in_p, O_CLOEXEC) || pipe2(out_p, O_CLOEXEC) || (capture_stderr && pipe2(err_p, O_CLOEXEC))) {
        close_pair(in_p);
        close_pair(out_p);
        close_pair(err_p);
        return -1;
    }
    char **wenv = build_env(env_extra);
    pid_t wpid = win_start(argv, wenv, cwd, in_p[0], out_p[1], capture_stderr ? err_p[1] : -1);
    int e = errno;
    free(wenv);
    close(in_p[0]);
    close(out_p[1]);
    if (err_p[1] >= 0) close(err_p[1]);
    if (wpid < 0) {
        close(in_p[1]);
        close(out_p[0]);
        if (err_p[0] >= 0) close(err_p[0]);
        errno = e;
        return -1;
    }
    c->pid = wpid;
    c->in_fd = in_p[1];
    c->out_fd = out_p[0];
    c->err_fd = err_p[0];
    return 0;
#else
    int in_p[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1}, exec_p[2] = {-1, -1};
    pid_t pid = -1;
    char **env = NULL;
    if (pipe2(in_p, O_CLOEXEC) || pipe2(out_p, O_CLOEXEC) || pipe2(exec_p, O_CLOEXEC) || (capture_stderr && pipe2(err_p, O_CLOEXEC)))
        goto fail;
    env = build_env(env_extra);
    pid = fork();
    if (pid < 0) {
    fail:
        free(env);
        close_pair(in_p);
        close_pair(out_p);
        close_pair(exec_p);
        close_pair(err_p);
        return -1;
    }
    if (pid == 0) {
        reset_child_signals();
        setsid();
        dup2(in_p[0], 0);
        dup2(out_p[1], 1);
        if (capture_stderr) dup2(err_p[1], 2);
        else {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) dup2(dn, 2);
        }
        if (cwd && chdir(cwd) != 0) {
            int e = errno;
            if (write(exec_p[1], &e, sizeof e) < 0) {}
            _exit(127);
        }
        exec_argv(argv, env);
        int e = errno;
        if (write(exec_p[1], &e, sizeof e) < 0) {}
        _exit(127);
    }
    free(env);
    close(in_p[0]);
    close(out_p[1]);
    if (err_p[1] >= 0) close(err_p[1]);
    close(exec_p[1]);
    int child_errno = 0;
    ssize_t er;
    do er = read(exec_p[0], &child_errno, sizeof child_errno);
    while (er < 0 && errno == EINTR);
    close(exec_p[0]);
    if (er == sizeof child_errno) {
        close(in_p[1]);
        close(out_p[0]);
        if (err_p[0] >= 0) close(err_p[0]);
        waitpid(pid, NULL, 0);
        errno = child_errno;
        return -1;
    }
    c->pid = pid;
    c->in_fd = in_p[1];
    c->out_fd = out_p[0];
    c->err_fd = err_p[0];
    return 0;
#endif
}

int child_wait(child_t *c, int64_t timeout_ms) {
    int64_t deadline = mono_ms() + timeout_ms;
    int status;
    for (;;) {
        pid_t r = waitpid(c->pid, &status, WNOHANG);
        if (r == c->pid) {
            c->pid = 0;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (r < 0) return -1;
        if (timeout_ms >= 0 && mono_ms() > deadline) return -2;
        sleep_ms(10);
    }
}

void child_kill(child_t *c, int sig) {
    if (c->pid > 0) {
        kill(-c->pid, sig);
        kill(c->pid, sig);
    }
}

void child_close(child_t *c) {
    if (c->in_fd >= 0) close(c->in_fd);
    if (c->out_fd >= 0) close(c->out_fd);
    if (c->err_fd >= 0) close(c->err_fd);
    c->in_fd = c->out_fd = c->err_fd = -1;
    if (c->pid > 0) {
        if (child_wait(c, 500) == -2) {
            child_kill(c, SIGTERM);
            if (child_wait(c, 1500) == -2) {
                child_kill(c, SIGKILL);
                child_wait(c, -1);
            }
        }
    }
    c->pid = 0;
}
