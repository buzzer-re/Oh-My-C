#define _GNU_SOURCE
#include "proc.h"
#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#if defined(__APPLE__)
#include <util.h> /* openpty, forkpty */
#else
#include <pty.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

void proc_ignore_sigpipe(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
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

static void reset_child_signals(void) {
    sigset_t s;
    sigemptyset(&s);
    sigprocmask(SIG_SETMASK, &s, NULL);
    for (int i = 1; i < 32; i++) {
        if (i == SIGKILL || i == SIGSTOP) continue;
        signal(i, SIG_DFL);
    }
}

int proc_run(const char *const *argv, const proc_opts_t *o, proc_result_t *res) {
    proc_opts_t def = {0};
    if (!o) o = &def;
    memset(res, 0, sizeof(*res));
    size_t cap = o->max_output ? o->max_output : (16u << 20);

    int in_p[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1}, exec_p[2];
    int master = -1;
    if (pipe2(exec_p, O_CLOEXEC) != 0) return -1;
    if (o->stdin_data && pipe2(in_p, O_CLOEXEC) != 0) return -1;
    if (o->use_pty) {
        struct winsize ws = {.ws_row = 40, .ws_col = 200};
        int slave;
        if (openpty(&master, &slave, NULL, NULL, &ws) != 0) return -1;
        out_p[0] = master;
        out_p[1] = slave;
        fcntl(master, F_SETFD, FD_CLOEXEC);
    } else {
        if (pipe2(out_p, O_CLOEXEC) != 0) return -1;
        if (!o->merge_stderr && pipe2(err_p, O_CLOEXEC) != 0) return -1;
    }
    char **env = build_env(o->env_extra);

    pid_t pid = fork();
    if (pid < 0) {
        free(env);
        return -1;
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
        execvpe(argv[0], (char *const *)argv, env);
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

const char *proc_shell_path(void) {
    static char *sh;
    if (sh) return sh;
    const char *e = env_get("OMC_SHELL");
    if (e && fs_is_exec(e)) sh = xstrdup(e);
    if (!sh) sh = which("bash");
    if (!sh) sh = xstrdup("/bin/sh");
    return sh;
}

int proc_shell(const char *cmd, const proc_opts_t *o, proc_result_t *res) {
    const char *sh = proc_shell_path();
    const char *argv[] = {sh, "-c", cmd, NULL};
    return proc_run(argv, o, res);
}

char *proc_shell_capture(const char *cmd, const char *cwd, int64_t timeout_ms) {
    const char *argv[] = {proc_shell_path(), "-c", cmd, NULL};
    return proc_capture(argv, cwd, timeout_ms);
}

/* ---------- long-lived child ---------- */

int child_spawn(const char *const *argv, const char *cwd, const char *const *env_extra, bool capture_stderr,
                child_t *c) {
    int in_p[2], out_p[2], err_p[2] = {-1, -1}, exec_p[2];
    if (pipe2(in_p, O_CLOEXEC) || pipe2(out_p, O_CLOEXEC) || pipe2(exec_p, O_CLOEXEC)) return -1;
    if (capture_stderr && pipe2(err_p, O_CLOEXEC)) return -1;
    char **env = build_env(env_extra);
    pid_t pid = fork();
    if (pid < 0) {
        free(env);
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
        execvpe(argv[0], (char *const *)argv, env);
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
