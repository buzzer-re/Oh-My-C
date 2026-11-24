/* services.c - named long-lived bash services under detached supervisor processes (see services.h). */
#define _GNU_SOURCE
#include "services.h"
#include "notify.h"
#include "config/settings.h"

#include "core/fs.h"
#include "core/proc.h"
#include "text/regex.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#if defined(__APPLE__)
#include <util.h> /* openpty, forkpty */
#else
#include <pty.h>
#endif
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "core/compat_sys.h"

extern char **environ;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static char *project_dir(const char *project) {
    char *real = path_real(project);
    const char *p = real ? real : project;
    char *h = xasprintf("%016llx", (unsigned long long)hash_fnv1a(p, strlen(p)));
    char *base = omc_agent_path("services");
    char *d = path_join(base, h);
    free(base);
    free(h);
    free(real);
    return d;
}

static char *service_dir(const char *project, const char *name) {
    char *pd = project_dir(project);
    char *d = path_join(pd, name);
    free(pd);
    return d;
}

static bool valid_name(const char *n) {
    size_t len = strlen(n);
    if (!len || len > 48 || !isalnum((unsigned char)n[0])) return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)n[i]) && !strchr("._-", n[i])) return false;
    return true;
}

static json_t *read_spec(const char *dir) {
    char *p = path_join(dir, "spec.json");
    char *t = fs_read(p, NULL);
    free(p);
    json_t *j = t ? json_parse_str(t) : NULL;
    free(t);
    return j;
}

static void write_spec(const char *dir, const json_t *spec) {
    char *p = path_join(dir, "spec.json");
    char *t = json_dump(spec, true);
    fs_write_atomic(p, t, strlen(t), 0600);
    free(t);
    free(p);
}

static bool pid_alive(int64_t pid) { return pid > 0 && (kill((pid_t)pid, 0) == 0 || errno == EPERM); }

/* exit file: "<code> <exitedAtMs>" written by the supervisor */
static bool read_exit(const char *dir, int *code, int64_t *at) {
    char *p = path_join(dir, "exit");
    char *t = fs_read(p, NULL);
    free(p);
    if (!t) return false;
    long long c = 0, a = 0;
    bool ok = sscanf(t, "%lld %lld", &c, &a) >= 1;
    free(t);
    *code = (int)c;
    *at = a;
    return ok;
}

static json_t *snapshot_of(const char *dir, const json_t *spec) {
    json_t *o = json_obj();
    json_set_str(o, "name", json_get_str(spec, "name"));
    int code = 0;
    int64_t at = 0;
    const char *state;
    bool exited = read_exit(dir, &code, &at);
    int64_t pid = json_get_int(spec, "pid", 0);
    if (exited) {
        state = code == 0 || json_get_bool(spec, "stopRequested", false) ? "exited" : "failed";
    } else if (!pid_alive(pid)) {
        state = "failed";
        exited = true;
        code = -1;
        at = json_get_int(spec, "startedAt", 0);
    } else if (json_get(spec, "ready") && !json_get(spec, "readyAt")) {
        state = "starting";
    } else if (json_get(spec, "readyAt")) {
        state = "ready";
    } else {
        state = "running";
    }
    json_set_str(o, "state", state);
    if (!exited) json_set_int(o, "pid", pid);
    json_set_int(o, "startedAt", json_get_int(spec, "startedAt", 0));
    if (json_get(spec, "readyAt")) json_set_int(o, "readyAt", json_get_int(spec, "readyAt", 0));
    if (exited) {
        json_set_int(o, "exitedAt", at);
        json_set_int(o, "exitCode", code);
    }
    json_set_bool(o, "persist", json_get_bool(spec, "persist", false));
    json_set_bool(o, "detached", json_get_bool(spec, "detached", false));
    json_set_str(o, "command", json_get_str(spec, "command"));
    return o;
}

bool services_state_terminal(const char *state) { return str_eq(state, "exited") || str_eq(state, "failed"); }

static char *fmt_duration(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t s = ms / 1000;
    if (s < 60) return xasprintf("%llds", (long long)s);
    if (s < 3600) return xasprintf("%lldm %llds", (long long)(s / 60), (long long)(s % 60));
    return xasprintf("%lldh %lldm", (long long)(s / 3600), (long long)((s % 3600) / 60));
}

char *services_status_line(const json_t *snap) {
    int64_t end = json_get(snap, "exitedAt") ? json_get_int(snap, "exitedAt", 0) : now_ms();
    char *age = fmt_duration(end - json_get_int(snap, "startedAt", 0));
    sb_t b = SB_INIT;
    sb_appendf(&b, "%s [service] — %s — up %s", json_get_str(snap, "name"), json_get_str(snap, "state"), age);
    if (json_get(snap, "pid")) sb_appendf(&b, " — pid %lld", (long long)json_get_int(snap, "pid", 0));
    if (json_get_bool(snap, "persist", false)) sb_append(&b, " — persistent");
    if (json_get_bool(snap, "detached", false)) sb_append(&b, " — detached");
    free(age);
    return sb_detach(&b);
}

json_t *services_find(const char *project, const char *name) {
    if (!valid_name(name)) return NULL;
    char *dir = service_dir(project, name);
    json_t *spec = read_spec(dir);
    json_t *r = spec ? snapshot_of(dir, spec) : NULL;
    json_free(spec);
    free(dir);
    return r;
}

json_t *services_list(const char *project) {
    json_t *arr = json_arr();
    char *pd = project_dir(project);
    dirlist_t dl;
    if (fs_list_dir(pd, &dl) == 0) {
        for (size_t i = 0; i < dl.n; i++) {
            if (!dl.v[i].is_dir) continue;
            json_t *s = services_find(project, dl.v[i].name);
            if (s) json_push(arr, s);
        }
        dirlist_free(&dl);
    }
    free(pd);
    return arr;
}

char *services_log_path(const char *project, const char *name) {
    char *dir = service_dir(project, name);
    char *p = path_join(dir, "output.log");
    free(dir);
    return p;
}

/* terminal bytes -> plain text rows: CSI/OSC stripped, CRLF -> LF, a bare CR restarts the line */
static char *render_terminal(const char *s, size_t len) {
    sb_t out = SB_INIT;
    size_t line_start = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            if (i + 1 < len && s[i + 1] == '[') {
                i += 2;
                while (i < len && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7e)) i++;
            } else if (i + 1 < len && s[i + 1] == ']') {
                i += 2;
                while (i < len && s[i] != 0x07 && !(s[i] == 0x1b && i + 1 < len && s[i + 1] == '\\')) i++;
                if (i < len && s[i] == 0x1b) i++;
            } else {
                i++;
            }
            continue;
        }
        if (c == '\r') {
            if (i + 1 < len && s[i + 1] == '\n') continue;
            sb_truncate(&out, line_start);
            continue;
        }
        if (c == '\n') {
            sb_putc(&out, '\n');
            line_start = out.len;
            continue;
        }
        if (c == '\t') {
            sb_append(&out, "    ");
            continue;
        }
        if (c < 0x20 || c == 0x7f) continue;
        sb_putc(&out, (char)c);
    }
    char *r = utf8_sanitize(out.s ? out.s : "", out.len);
    sb_free(&out);
    return r;
}

char *services_logs(const char *project, const char *name) {
    char *p = services_log_path(project, name);
    size_t n = 0;
    char *raw = fs_read(p, &n);
    free(p);
    if (!raw) return xstrdup("");
    const char *start = raw;
    if (n > (4u << 20)) {
        start = raw + n - (4u << 20);
        n = 4u << 20;
    }
    char *txt = render_terminal(start, n);
    free(raw);
    /* last 1000 lines, trailing newlines trimmed */
    size_t len = strlen(txt);
    while (len && txt[len - 1] == '\n') txt[--len] = 0;
    size_t lines = 0, cut = 0;
    for (size_t i = len; i > 0; i--)
        if (txt[i - 1] == '\n' && ++lines >= 1000) {
            cut = i;
            break;
        }
    char *r = xstrdup(txt + cut);
    free(txt);
    return r;
}

/* ---------- supervisor ---------- */

#ifndef _WIN32
static void put_num(char *buf, size_t *n, long long v) {
    char tmp[24];
    int k = 0;
    bool neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    do {
        tmp[k++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg) buf[(*n)++] = '-';
    while (k) buf[(*n)++] = tmp[--k];
}
#endif

static void write_full(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct pollfd pf = {fd, POLLOUT, 0};
                poll(&pf, 1, 100);
                continue;
            }
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

/* runs in the forked supervisor: async-signal-safe calls only */
#ifndef _WIN32
static void supervise(pid_t child, int master, int fifofd, int logfd, const char *exit_path) {
    int status = 0;
    bool reaped = false;
    if (master >= 0) {
        fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
        bool tty_open = true;
        char buf[16384];
        while (!reaped || tty_open) {
            struct pollfd pf[2] = {{master, POLLIN, 0}, {fifofd, POLLIN, 0}};
            int pr = poll(pf, tty_open ? 2 : 0, 200);
            if (pr > 0 && (pf[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t r = read(master, buf, sizeof buf);
                if (r > 0) write_full(logfd, buf, (size_t)r);
                else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) tty_open = false;
            }
            if (pr > 0 && (pf[1].revents & POLLIN)) {
                ssize_t r = read(fifofd, buf, sizeof buf);
                if (r > 0) write_full(master, buf, (size_t)r);
            }
            if (!reaped && waitpid(child, &status, WNOHANG) == child) reaped = true;
            if (reaped && tty_open && pr == 0) tty_open = false; /* drained */
        }
    } else {
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    char line[64];
    size_t n = 0;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    put_num(line, &n, code);
    line[n++] = ' ';
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    put_num(line, &n, (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    line[n++] = '\n';
    int fd = open(exit_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        write_full(fd, line, n);
        close(fd);
    }
}
#endif

static char **service_env(bool pty) {
    static const char *extra_pty[] = {"TERM=xterm-256color", "OMC=1", "PAGER=cat", "GIT_PAGER=cat", NULL};
    static const char *extra_pipe[] = {"TERM=dumb", "NO_COLOR=1", "OMC=1", "PAGER=cat", "GIT_PAGER=cat", NULL};
    const char *const *extra = pty ? extra_pty : extra_pipe;
    size_t n = 0, m = 0;
    while (environ[n]) n++;
    while (extra[m]) m++;
    char **env = xcalloc(n + m + 1, sizeof(char *));
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *eq = strchr(environ[i], '=');
        size_t kl = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
        bool over = false;
        for (size_t j = 0; j < m; j++)
            if (!strncmp(extra[j], environ[i], kl) && extra[j][kl] == '=') over = true;
        if (!over) env[k++] = environ[i];
    }
    for (size_t j = 0; j < m; j++) env[k++] = (char *)extra[j];
    return env;
}

#ifdef _WIN32
/* Windows: no fork, no fifo. The supervisor is omc itself (`omc __omc_service_supervisor <dir>`), started detached so it
 * outlives this session. It runs the command (bash -c) in a job that ends with the supervisor, appends its output to
 * <dir>/output.log (through a pseudo console when pty), feeds it what is appended to <dir>/stdin (a plain file here),
 * and records "<child> <supervisor>" in <dir>/pids and "<code> <exitedAtMs>" in <dir>/exit. */
char *su_self_exe(void);

static int launch(const char *dir, const char *command, const char *cwd, bool pty, int64_t *child_pid, int64_t *sup_pid,
                  char **err) {
    char *logp = path_join(dir, "output.log"), *inp = path_join(dir, "stdin"), *exitp = path_join(dir, "exit"),
         *pidp = path_join(dir, "pids"), *lp = path_join(dir, "launch.json");
    unlink(exitp);
    unlink(pidp);
    int rc = -1;
    json_t *l = json_obj();
    json_set_str(l, "command", command);
    if (cwd) json_set_str(l, "cwd", cwd);
    json_set_bool(l, "pty", pty);
    char *lt = json_dump(l, 0);
    json_free(l);
    char *self = su_self_exe();
    if (fs_write(lp, lt, strlen(lt)) != 0 || fs_write(logp, "", 0) != 0 || fs_write(inp, "", 0) != 0) {
        *err = xasprintf("cannot open service files: %s", strerror(errno));
    } else if (!self) {
        *err = xstrdup("failed to start the service supervisor");
    } else {
        char *argv[] = {self, "__omc_service_supervisor", (char *)dir, NULL};
        omc_win_spawn_opts_t so = {.fd_in = -1, .fd_out = -1, .fd_err = -1, .detached = true};
        if (omc_win_spawn(argv, &so) < 0) {
            *err = xasprintf("spawn: %s", strerror(errno));
        } else {
            int64_t deadline = mono_ms() + 10000;
            while (mono_ms() < deadline) {
                char *t = fs_read(pidp, NULL);
                long long c = 0, s = 0;
                if (t && sscanf(t, "%lld %lld", &c, &s) == 2 && c > 0) {
                    *child_pid = c;
                    *sup_pid = s;
                    rc = 0;
                }
                free(t);
                if (rc == 0) break;
                sleep_ms(20);
            }
            if (rc != 0) *err = xstrdup("failed to start the service supervisor");
        }
    }
    free(self);
    free(lt);
    free(logp);
    free(inp);
    free(exitp);
    free(pidp);
    free(lp);
    return rc;
}

int services_supervisor_main(const char *dir) {
    char *lp = path_join(dir, "launch.json");
    char *lt = fs_read(lp, NULL);
    free(lp);
    json_t *l = lt ? json_parse_str(lt) : NULL;
    free(lt);
    if (!l) return 1;
    const char *command = json_get_str(l, "command");
    if (!command) command = "";
    const char *cwd = json_get_str(l, "cwd");
    bool pty = json_get_bool(l, "pty", false);
    char *logp = path_join(dir, "output.log"), *inp = path_join(dir, "stdin"), *exitp = path_join(dir, "exit"),
         *pidp = path_join(dir, "pids");
    int logfd = open(logp, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    const char *sh = proc_shell_path();
    const char *name = proc_shell_name();
    char *bargv[] = {(char *)name, "-c", (char *)command, NULL};
    char *pbargv[] = {(char *)sh, "-c", (char *)command, NULL};
    char **argv = bargv, **pargv = pbargv;
    /* no bash: the service is a PowerShell command line */
    strvec_t psv = {0};
    char *script = NULL;
    if (proc_shell_is_powershell()) {
        script = proc_ps_script(command);
        psv = proc_ps_argv(script, NULL);
        argv = pargv = psv.v;
    }
    char **envp = service_env(pty);
    int child = -1, in_w = -1, out_r = -1;
    if (pty) {
        child = omc_win_pty_spawn(pargv, cwd, envp, 160, 40, &in_w, &out_r);
    } else {
        int p[2];
        if (pipe2(p, O_CLOEXEC) == 0) {
            omc_win_spawn_opts_t so = {.exe = sh, .cwd = cwd, .envp = envp, .fd_in = p[0], .fd_out = logfd, .fd_err = logfd};
            child = omc_win_spawn(argv, &so);
            close(p[0]);
            in_w = p[1];
        }
    }
    free(envp);
    strvec_free(&psv);
    free(script);
    if (child > 0) omc_win_job_kill_on_close(child);
    char ids[64];
    snprintf(ids, sizeof ids, "%d %d\n", child, (int)getpid());
    fs_write_atomic(pidp, ids, strlen(ids), 0600);
    int status = 127 << 8;
    if (child > 0) {
        if (out_r >= 0) fcntl(out_r, F_SETFL, O_NONBLOCK);
        long long in_off = 0;
        char buf[16384];
        bool reaped = false;
        while (!reaped) {
            bool busy = false;
            /* input appended to <dir>/stdin since the last look */
            int fd = open(inp, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                if (lseek(fd, in_off, SEEK_SET) == in_off) {
                    ssize_t r;
                    while ((r = read(fd, buf, sizeof buf)) > 0) {
                        if (in_w >= 0) write_full(in_w, buf, (size_t)r);
                        in_off += r;
                        busy = true;
                    }
                }
                close(fd);
            }
            if (out_r >= 0) {
                ssize_t r;
                while ((r = read(out_r, buf, sizeof buf)) > 0) {
                    if (logfd >= 0) write_full(logfd, buf, (size_t)r);
                    busy = true;
                }
            }
            if (waitpid(child, &status, WNOHANG) == child) reaped = true;
            else if (!busy) sleep_ms(50);
        }
        if (out_r >= 0) {
            /* the pseudo console flushes what it still holds once it closes */
            fcntl(out_r, F_SETFL, 0);
            int64_t until = mono_ms() + 2000;
            struct pollfd pf = {out_r, POLLIN, 0};
            while (mono_ms() < until && poll(&pf, 1, 100) > 0) {
                ssize_t r = read(out_r, buf, sizeof buf);
                if (r <= 0) break;
                if (logfd >= 0) write_full(logfd, buf, (size_t)r);
            }
        }
    }
    if (in_w >= 0) close(in_w);
    if (out_r >= 0) close(out_r);
    if (logfd >= 0) close(logfd);
    char line[64];
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    snprintf(line, sizeof line, "%d %lld\n", code, (long long)now_ms());
    fs_write(exitp, line, strlen(line));
    free(logp);
    free(inp);
    free(exitp);
    free(pidp);
    json_free(l);
    return 0;
}
#else
static int launch(const char *dir, const char *command, const char *cwd, bool pty, int64_t *child_pid, int64_t *sup_pid,
                  char **err) {
    char *logp = path_join(dir, "output.log"), *fifop = path_join(dir, "stdin"), *exitp = path_join(dir, "exit");
    unlink(exitp);
    unlink(fifop);
    int logfd = -1, fifofd = -1, master = -1, slave = -1, rep[2] = {-1, -1};
    int rc = -1;
    if (mkfifo(fifop, 0600) != 0) {
        *err = xasprintf("mkfifo %s: %s", fifop, strerror(errno));
        goto out;
    }
    logfd = open(logp, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
    fifofd = open(fifop, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (logfd < 0 || fifofd < 0) {
        *err = xasprintf("cannot open service files: %s", strerror(errno));
        goto out;
    }
    if (pty) {
        struct winsize ws = {.ws_row = 40, .ws_col = 160};
        if (openpty(&master, &slave, NULL, NULL, &ws) != 0) {
            *err = xasprintf("openpty: %s", strerror(errno));
            goto out;
        }
        fcntl(master, F_SETFD, FD_CLOEXEC);
        fcntl(slave, F_SETFD, FD_CLOEXEC);
    }
    if (pipe2(rep, O_CLOEXEC) != 0) {
        *err = xasprintf("pipe: %s", strerror(errno));
        goto out;
    }
    const char *sh = proc_shell_path();
    const char *argv[] = {sh, "-c", command, NULL};
    char **envp = service_env(pty);
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
    pid_t a = fork();
    if (a < 0) {
        free(envp);
        *err = xasprintf("fork: %s", strerror(errno));
        goto out;
    }
    if (a == 0) {
        sigset_t ss;
        sigemptyset(&ss);
        sigprocmask(SIG_SETMASK, &ss, NULL);
        for (int i = 1; i < 32; i++)
            if (i != SIGKILL && i != SIGSTOP) signal(i, SIG_DFL);
        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        setsid();
        /* drop every descriptor the supervisor does not need */
        for (int fd = 3; fd < maxfd; fd++)
            if (fd != logfd && fd != fifofd && fd != master && fd != slave && fd != rep[1]) close(fd);
        pid_t b = fork();
        if (b != 0) _exit(0);
        int in_fd = -1;
        if (!pty) in_fd = open(fifop, O_RDONLY | O_CLOEXEC);
        pid_t c = fork();
        if (c == 0) {
            signal(SIGHUP, SIG_DFL);
            signal(SIGPIPE, SIG_DFL);
            setsid();
            if (pty) {
                ioctl(slave, TIOCSCTTY, 0);
                dup2(slave, 0);
                dup2(slave, 1);
                dup2(slave, 2);
            } else {
                if (in_fd >= 0) dup2(in_fd, 0);
                dup2(logfd, 1);
                dup2(logfd, 2);
            }
            if (cwd && chdir(cwd) != 0) _exit(127);
            execve(sh, (char *const *)argv, envp);
            _exit(127);
        }
        int64_t ids[2] = {c, getpid()};
        write_full(rep[1], (const char *)ids, sizeof ids);
        close(rep[1]);
        if (in_fd >= 0) close(in_fd);
        if (slave >= 0) close(slave);
        if (c > 0) supervise(c, master, fifofd, logfd, exitp);
        _exit(0);
    }
    free(envp);
    close(rep[1]);
    rep[1] = -1;
    int st;
    while (waitpid(a, &st, 0) < 0 && errno == EINTR) {}
    int64_t ids[2] = {0, 0};
    ssize_t r;
    do r = read(rep[0], ids, sizeof ids);
    while (r < 0 && errno == EINTR);
    if (r != (ssize_t)sizeof ids || ids[0] <= 0) {
        *err = xstrdup("failed to start the service supervisor");
        goto out;
    }
    *child_pid = ids[0];
    *sup_pid = ids[1];
    rc = 0;
out:
    if (logfd >= 0) close(logfd);
    if (fifofd >= 0) close(fifofd);
    if (master >= 0) close(master);
    if (slave >= 0) close(slave);
    if (rep[0] >= 0) close(rep[0]);
    if (rep[1] >= 0) close(rep[1]);
    free(logp);
    free(fifop);
    free(exitp);
    return rc;
}
#endif

static bool port_open(const char *host, int port) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return false;
    bool ok = false;
    for (struct addrinfo *ai = res; ai && !ok; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        int c = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (c == 0) ok = true;
        else if (errno == EINPROGRESS) {
            struct pollfd pf = {fd, POLLOUT, 0};
            if (poll(&pf, 1, 200) > 0) {
                int e = 0;
                socklen_t l = sizeof e;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
                ok = e == 0;
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
    return ok;
}

/* readinessHosts / listeningHost (omp 04833e5623): the configured host, else both loopbacks (`localhost` servers such as
 * Vite or Node >= 17 may bind only ::1); the first that accepts a connection, or NULL */
static const char *listening_host(const char *host, int port) {
    if (host) return port_open(host, port) ? host : NULL;
    if (port_open("127.0.0.1", port)) return "127.0.0.1";
    if (port_open("::1", port)) return "::1";
    return NULL;
}

/* the service exited: deliver a completion notice to the owning session (unless it was stopped on purpose) */
typedef struct {
    char *project, *name;
    int64_t started;
    jobs_t *owner;
    char *notify;          /* `notify` regex (NULL: none) */
    notifier_t *notifier;  /* matches of the service log, reported to the owning session */
    int64_t log_off;
} watch_t;

/* a service's batch: queued like a job's (queueServiceNotify) */
static void watch_notify_emit(void *ud, json_t *lines, int64_t omitted, bool stopped) {
    watch_t *w = ud;
    json_t *s = services_find(w->project, w->name);
    bool running = s && !services_state_terminal(json_get_str(s, "state"));
    json_free(s);
    json_t *e = json_obj();
    json_set_str(e, "id", w->name);
    json_set_str(e, "type", "service");
    json_set_str(e, "pattern", w->notify ? w->notify : "");
    json_set(e, "lines", lines);
    json_set_int(e, "omitted", omitted);
    json_set_bool(e, "stopped", stopped);
    json_set_bool(e, "running", running);
    jobs_queue_notice(w->owner, e);
}

/* feed the log bytes written since the last look */
static void watch_feed_log(watch_t *w) {
    if (!w->notifier) return;
    char *path = services_log_path(w->project, w->name);
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    free(path);
    if (fd < 0) return;
    char buf[65536];
    for (;;) {
        ssize_t n = pread(fd, buf, sizeof buf, w->log_off);
        if (n <= 0) break;
        w->log_off += n;
        notifier_feed(w->notifier, buf, (size_t)n);
    }
    close(fd);
}

static void watch_free(void *p) {
    watch_t *w = p;
    notifier_free(w->notifier);
    free(w->notify);
    free(w->project);
    free(w->name);
    free(w);
}

static char *watch_body(bgjob_t *job, void *ud, bool *failed) {
    watch_t *w = ud;
    volatile sig_atomic_t *cancel = job_cancel_flag(job);
    for (;;) {
        json_t *s = services_find(w->project, w->name);
        if (!s || json_get_int(s, "startedAt", 0) != w->started) {
            json_free(s);
            *failed = false;
            return xasprintf("Service %s was replaced or removed.", w->name);
        }
        watch_feed_log(w);
        if (services_state_terminal(json_get_str(s, "state"))) {
            notifier_finish(w->notifier); /* the exit notice does not carry the output: the last matches go out */
            *failed = str_eq(json_get_str(s, "state"), "failed");
            char *st = services_status_line(s);
            char *r = xasprintf("%s (exit %lld). Read proc://%s for its output.", st, (long long)json_get_int(s, "exitCode", 0), w->name);
            free(st);
            json_free(s);
            return r;
        }
        json_free(s);
        for (int i = 0; i < 5 && !*cancel; i++) sleep_ms(100);
        if (*cancel) {
            *failed = false;
            return xstrdup("");
        }
    }
}

static void start_watch(jobs_t *owner, const char *project, const char *name, int64_t started, const char *notify) {
    if (!owner) return;
    watch_t *w = xcalloc(1, sizeof *w);
    w->project = xstrdup(project);
    w->name = xstrdup(name);
    w->started = started;
    w->owner = owner;
    if (notify) { /* no deadline: the burst once per start */
        w->notify = xstrdup(notify);
        w->notifier = notifier_new(notify, false, watch_notify_emit, w);
    }
    char *label = xasprintf("service %s", name);
    char *hint = xasprintf("svc_%s", name);
    char *id = jobs_start(owner, "service", hint, label, name, true, watch_body, w, watch_free);
    free(id);
    free(hint);
    free(label);
}

static void kill_service(const char *dir, json_t *spec, int64_t wait_ms) {
    int64_t pid = json_get_int(spec, "pid", 0);
    int code;
    int64_t at;
    if (read_exit(dir, &code, &at) || !pid_alive(pid)) return;
    kill(-(pid_t)pid, SIGTERM);
    kill((pid_t)pid, SIGTERM);
    int64_t deadline = mono_ms() + wait_ms;
    while (mono_ms() < deadline) {
        if (read_exit(dir, &code, &at) || !pid_alive(pid)) return;
        sleep_ms(50);
    }
    kill(-(pid_t)pid, SIGKILL);
    kill((pid_t)pid, SIGKILL);
    deadline = mono_ms() + 2000;
    while (mono_ms() < deadline && !read_exit(dir, &code, &at)) sleep_ms(50);
}

static int start_locked(jobs_t *owner, const char *project, const char *name, const char *command, const char *cwd, bool pty,
                        bool persist, bool detached, const json_t *ready_spec, const char *notify, json_t **spec_out, char **err) {
    char *dir = service_dir(project, name);
    fs_mkdir_p(dir, 0700);
    json_t *old = read_spec(dir);
    if (old) {
        if (owner) jobs_service_stopping(owner, name);
        kill_service(dir, old, 5000);
        json_free(old);
    }
    int64_t cpid = 0, spid = 0;
    if (launch(dir, command, cwd, pty, &cpid, &spid, err) != 0) {
        free(dir);
        return -1;
    }
    json_t *spec = json_obj();
    json_set_str(spec, "name", name);
    json_set_str(spec, "command", command);
    json_set_str(spec, "cwd", cwd);
    json_set_bool(spec, "pty", pty);
    json_set_bool(spec, "persist", persist);
    json_set_bool(spec, "detached", detached);
    json_set_int(spec, "pid", cpid);
    json_set_int(spec, "supervisor", spid);
    json_set_int(spec, "startedAt", now_ms());
    json_set_int(spec, "ownerPid", (int64_t)getpid());
    if (ready_spec) json_set(spec, "ready", json_clone(ready_spec));
    write_spec(dir, spec);
    if (owner) {
        jobs_own_service(owner, name);
        start_watch(owner, project, name, json_get_int(spec, "startedAt", 0), notify);
    }
    *spec_out = spec;
    free(dir);
    return 0;
}

int services_start(jobs_t *owner, const char *project, const char *name, const char *command, const char *cwd, bool pty,
                   const service_ready_t *ready, const char *notify, volatile sig_atomic_t *cancel, json_t **snapshot,
                   bool *ready_timed_out, char **err) {
    *snapshot = NULL;
    *ready_timed_out = false;
    *err = NULL;
    if (!valid_name(name)) {
        *err = xstrdup("Service name must be 1-48 letters, numbers, dots, underscores, or hyphens");
        return -1;
    }
    if (notify && !notify_pattern_ok(notify, err)) return -1;
    rx_t *rx = NULL;
    json_t *rspec = NULL;
    if (ready) {
        if (ready->port && (ready->port < 1 || ready->port > 65535)) {
            *err = xstrdup("ready.port must be an integer from 1 to 65535");
            return -1;
        }
        if (!ready->log && !ready->port) {
            *err = xstrdup("ready requires log or port");
            return -1;
        }
        if (ready->log) {
            char *rerr = NULL;
            rx = rx_compile(ready->log, RX_MULTILINE, &rerr); /* DAEMON_OUTPUT_PATTERN_FLAGS "mu": ^ and $ at line boundaries (omp cf93771fa1) */
            if (!rx) {
                *err = xasprintf("Invalid readiness regex: %s", rerr ? rerr : "?");
                free(rerr);
                return -1;
            }
        }
        rspec = json_obj();
        if (ready->log) json_set_str(rspec, "log", ready->log);
        if (ready->port) json_set_int(rspec, "port", ready->port);
        if (ready->host) json_set_str(rspec, "host", ready->host);
    }
    /* a listener already on the port would satisfy the readiness probe before this service binds it (and the service
     * itself then fails with "address in use") */
    if (ready && ready->port) {
        char *sdir = service_dir(project, name);
        json_t *cur = read_spec(sdir);
        json_t *snap = cur ? snapshot_of(sdir, cur) : NULL;
        bool running = snap && !services_state_terminal(json_get_str(snap, "state"));
        json_free(snap);
        json_free(cur);
        free(sdir);
        const char *busy = running ? NULL : listening_host(ready->host, ready->port);
        if (busy) {
            *err = xasprintf("Port %d already accepts connections on %s before %s started; another process holds it, so "
                             "ready.port cannot tell when %s is ready. Stop that process or use another port.",
                             ready->port, busy, name, name);
            json_free(rspec);
            if (rx) rx_free(rx);
            return -1;
        }
    }
    pthread_mutex_lock(&g_mu);
    json_t *spec = NULL;
    /* startService: persist follows launch.persist (on for print, RPC and ACP runs; omp 5bc2903877) */
    int rc = start_locked(owner, project, name, command, cwd, pty, settings_bool("launch.persist", false), false, rspec, notify, &spec, err);
    pthread_mutex_unlock(&g_mu);
    json_free(rspec);
    if (rc != 0) {
        if (rx) rx_free(rx);
        return -1;
    }
    char *dir = service_dir(project, name);
    if (ready) {
        double tsec = ready->timeout > 0 ? ready->timeout : 30;
        if (tsec < 0.05) tsec = 0.05;
        if (tsec > 3600) tsec = 3600;
        int64_t deadline = mono_ms() + (int64_t)(tsec * 1000);
        bool ok = false;
        char *logp = path_join(dir, "output.log");
        for (;;) {
            bool log_ok = !rx, port_ok = !ready->port;
            if (rx) {
                size_t n = 0;
                char *raw = fs_read(logp, &n);
                if (raw) {
                    char *txt = render_terminal(raw, n);
                    log_ok = rx_is_match(rx, txt, strlen(txt), 0);
                    free(txt);
                    free(raw);
                }
            }
            if (!port_ok) port_ok = listening_host(ready->host, ready->port) != NULL;
            if (log_ok && port_ok) {
                ok = true;
                break;
            }
            json_t *s = snapshot_of(dir, spec);
            bool term = services_state_terminal(json_get_str(s, "state"));
            json_free(s);
            if (term || (cancel && *cancel)) break;
            if (mono_ms() >= deadline) {
                *ready_timed_out = true;
                break;
            }
            sleep_ms(100);
        }
        free(logp);
        if (ok) {
            pthread_mutex_lock(&g_mu);
            json_t *cur = read_spec(dir);
            if (cur && json_get_int(cur, "startedAt", 0) == json_get_int(spec, "startedAt", -1)) {
                json_set_int(cur, "readyAt", now_ms());
                write_spec(dir, cur);
                json_free(spec);
                spec = cur;
                cur = NULL;
            }
            json_free(cur);
            pthread_mutex_unlock(&g_mu);
        }
    } else {
        sleep_ms(50); /* let an immediately-failing command report its exit */
    }
    *snapshot = snapshot_of(dir, spec);
    json_free(spec);
    free(dir);
    if (rx) rx_free(rx);
    return 0;
}

static json_t *require(const char *project, const char *name, char **dir_out, char **err) {
    if (!valid_name(name)) {
        *err = xasprintf("Service not found: %s", name);
        return NULL;
    }
    char *dir = service_dir(project, name);
    json_t *spec = read_spec(dir);
    if (!spec) {
        *err = xasprintf("Service not found: %s", name);
        free(dir);
        return NULL;
    }
    *dir_out = dir;
    return spec;
}

int services_send(const char *project, const char *name, const char *content, json_t **snap, char **err) {
    *err = NULL;
    *snap = NULL;
    char *dir = NULL;
    json_t *spec = require(project, name, &dir, err);
    if (!spec) return -1;
    json_t *s = snapshot_of(dir, spec);
    if (services_state_terminal(json_get_str(s, "state"))) {
        *err = xasprintf("Service %s is not running (%s)", name, json_get_str(s, "state"));
        json_free(s);
        json_free(spec);
        free(dir);
        return -1;
    }
    size_t n = strlen(content);
    bool nl = n && (content[n - 1] == '\n' || content[n - 1] == '\r');
    char *data = nl ? xstrdup(content) : xasprintf("%s%s", content, json_get_bool(spec, "pty", true) ? "\r" : "\n");
    char *fifo = path_join(dir, "stdin");
#ifdef _WIN32
    int fd = open(fifo, O_WRONLY | O_APPEND | O_CLOEXEC); /* a plain file the supervisor reads on (see launch) */
#else
    int fd = open(fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
#endif
    int rc = 0;
    if (fd < 0) {
        *err = xasprintf("Cannot reach %s's stdin: %s", name, strerror(errno));
        rc = -1;
    } else {
        write_full(fd, data, strlen(data));
        close(fd);
    }
    free(fifo);
    free(data);
    *snap = s;
    json_free(spec);
    free(dir);
    return rc;
}

int services_stop(jobs_t *owner, const char *project, const char *name, json_t **snap, char **err) {
    *err = NULL;
    *snap = NULL;
    char *dir = NULL;
    pthread_mutex_lock(&g_mu);
    json_t *spec = require(project, name, &dir, err);
    if (!spec) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    json_set_bool(spec, "stopRequested", true);
    write_spec(dir, spec);
    if (owner) jobs_service_stopping(owner, name);
    pthread_mutex_unlock(&g_mu);
    kill_service(dir, spec, 5000);
    *snap = snapshot_of(dir, spec);
    json_free(spec);
    free(dir);
    return 0;
}

int services_mode(jobs_t *owner, const char *project, const char *name, const char *mode, json_t **snap, char **err) {
    *err = NULL;
    *snap = NULL;
    if (!str_eq(mode, "persist") && !str_eq(mode, "session") && !str_eq(mode, "detached")) {
        *err = xstrdup("Service mode must be persist, session, or detached");
        return -1;
    }
    char *dir = NULL;
    pthread_mutex_lock(&g_mu);
    json_t *spec = require(project, name, &dir, err);
    if (!spec) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    int rc = 0;
    if (str_eq(mode, "detached")) {
        if (!json_get_bool(spec, "detached", false)) {
            /* a detached service runs without a terminal: relaunch it on pipes */
            json_t *s = snapshot_of(dir, spec);
            bool term = services_state_terminal(json_get_str(s, "state"));
            json_free(s);
            json_t *nspec = NULL;
            if (term) {
                json_set_bool(spec, "detached", true);
                json_set_bool(spec, "persist", true);
                json_set_bool(spec, "pty", false);
                write_spec(dir, spec);
            } else {
                char *command = xstrdup(json_get_str(spec, "command"));
                char *cwd = xstrdup(json_get_str(spec, "cwd") ? json_get_str(spec, "cwd") : project);
                json_t *ready = json_clone(json_get(spec, "ready"));
                rc = start_locked(owner, project, name, command, cwd, false, true, true, ready, NULL, &nspec, err);
                json_free(ready);
                free(command);
                free(cwd);
                if (rc == 0) {
                    json_free(spec);
                    spec = nspec;
                }
            }
        }
    } else {
        if (json_get_bool(spec, "detached", false) && str_eq(mode, "session")) {
            *err = xasprintf("Detached service %s must remain persistent", name);
            rc = -1;
        } else {
            json_set_bool(spec, "persist", str_eq(mode, "persist"));
            write_spec(dir, spec);
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (rc == 0) *snap = snapshot_of(dir, spec);
    json_free(spec);
    free(dir);
    return rc;
}

strvec_t *jobs_services_locked(jobs_t *j, pthread_mutex_t **mu); /* jobs.c */

void services_stop_owned(jobs_t *j) {
    if (!j) return;
    pthread_mutex_t *mu;
    strvec_t names = {0};
    strvec_t *v = jobs_services_locked(j, &mu);
    pthread_mutex_lock(mu);
    for (size_t i = 0; i < v->n; i++) strvec_push(&names, v->v[i]);
    pthread_mutex_unlock(mu);
    /* the project is recorded per service; scan every project dir for owned names started by this process */
    char *base = omc_agent_path("services");
    dirlist_t projects;
    if (names.n && fs_list_dir(base, &projects) == 0) {
        for (size_t p = 0; p < projects.n; p++) {
            if (!projects.v[p].is_dir) continue;
            for (size_t i = 0; i < names.n; i++) {
                char *dir = path_join3(base, projects.v[p].name, names.v[i]);
                json_t *spec = read_spec(dir);
                if (spec && json_get_int(spec, "ownerPid", 0) == (int64_t)getpid() && !json_get_bool(spec, "persist", false) &&
                    !json_get_bool(spec, "detached", false)) {
                    json_set_bool(spec, "stopRequested", true);
                    write_spec(dir, spec);
                    jobs_service_stopping(j, names.v[i]);
                    kill_service(dir, spec, 2000);
                }
                json_free(spec);
                free(dir);
            }
        }
        dirlist_free(&projects);
    }
    free(base);
    strvec_free(&names);
}
