#define _GNU_SOURCE
#include "shell.h"

#include "core/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

struct shell {
    child_t child;
    bool started, is_bash;
    char *dir;    /* private directory: the command file and each command's output pipe */
    char *script; /* <dir>/c: the name error messages carry until they are rewritten */
    uint64_t seq;
    pthread_mutex_t mu;
};

shell_t *shell_new(void) {
    shell_t *s = xcalloc(1, sizeof(*s));
    s->child.in_fd = s->child.out_fd = s->child.err_fd = -1;
    pthread_mutex_init(&s->mu, NULL);
    return s;
}

/* output pipes a background job still holds: read and dropped, so the job never blocks or dies writing to one. The job can
 * outlive its shell (a one-shot command's), so this outlives them all. */
static pthread_mutex_t g_drain_mu = PTHREAD_MUTEX_INITIALIZER;
static VEC(int) g_drain;
static bool g_drain_started;

static void *drain_main(void *p) {
    UNUSED(p);
    char buf[16384];
    for (;;) {
        struct pollfd pf[64];
        nfds_t n = 0;
        pthread_mutex_lock(&g_drain_mu);
        for (size_t i = 0; i < g_drain.n && n < 64; i++) pf[n++] = (struct pollfd){g_drain.v[i], POLLIN, 0};
        pthread_mutex_unlock(&g_drain_mu);
        if (poll(pf, n, 200) <= 0) continue;
        for (nfds_t k = 0; k < n; k++) {
            if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
            ssize_t r = read(pf[k].fd, buf, sizeof buf);
            if (r > 0 || (r < 0 && (errno == EAGAIN || errno == EINTR))) continue;
            pthread_mutex_lock(&g_drain_mu);
            for (size_t i = 0; i < g_drain.n; i++)
                if (g_drain.v[i] == pf[k].fd) {
                    close(pf[k].fd);
                    g_drain.v[i] = g_drain.v[--g_drain.n];
                    break;
                }
            pthread_mutex_unlock(&g_drain_mu);
        }
    }
    return NULL;
}

static void drain_add(int fd) {
    pthread_mutex_lock(&g_drain_mu);
    vec_push(&g_drain, fd);
    if (!g_drain_started) {
        pthread_t th;
        if (pthread_create(&th, NULL, drain_main, NULL) == 0) {
            pthread_detach(th);
            g_drain_started = true;
        }
    }
    pthread_mutex_unlock(&g_drain_mu);
}

static void stop(shell_t *s) {
    if (!s->started) return;
    child_kill(&s->child, SIGKILL);
    child_close(&s->child);
    s->started = false;
}

void shell_free(shell_t *s) {
    if (!s) return;
    stop(s);
    if (s->script) unlink(s->script);
    if (s->dir) rmdir(s->dir);
    free(s->script);
    free(s->dir);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

static int write_all(int fd, const char *p, size_t n);

static int start(shell_t *s, const proc_opts_t *o) {
    const char *sh = proc_shell_path();
    s->is_bash = str_ends(sh, "bash");
    if (!s->dir) {
        const char *tmp = env_get("TMPDIR");
        char *t = xasprintf("%s/omc-sh-XXXXXX", tmp && *tmp ? tmp : "/tmp");
        if (!mkdtemp(t)) {
            free(t);
            return -1;
        }
        s->dir = t;
        s->script = xasprintf("%s/c", t);
    }
    const char *argv_bash[] = {sh, "--noprofile", "--norc", NULL};
    const char *argv_sh[] = {sh, NULL};
    if (child_spawn(s->is_bash ? argv_bash : argv_sh, NULL, o ? o->env_extra : NULL, false, &s->child) != 0) return -1;
    s->started = true;
    /* aliases defined by one command work in the next (omp's embedded bash keeps them) */
    if (s->is_bash) {
        static const char init[] = "shopt -s expand_aliases\n";
        if (write_all(s->child.in_fd, init, sizeof init - 1) != 0) {
            stop(s);
            return -1;
        }
    }
    return 0;
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static char *sh_quote(const char *s) {
    sb_t b = SB_INIT;
    sb_putc(&b, '\'');
    for (; *s; s++) {
        if (*s == '\'') sb_append(&b, "'\\''");
        else sb_putc(&b, *s);
    }
    sb_putc(&b, '\'');
    return sb_detach(&b);
}

/* ---------- job-status notices ----------
 * Non-interactive bash reports a foreground child killed by a signal on its own stderr, which the command's
 * redirection merges into the captured output: `<sh>: line N: PID Killed                     cmd` (some signals print
 * just `Terminated                 cmd`). omp's embedded shell prints nothing there, so these notices are dropped. */

static const char *sig_desc[NSIG];
static size_t sig_desc_n;
static pthread_once_t sig_desc_once = PTHREAD_ONCE_INIT;

static void sig_desc_init(void) {
    for (int sig = 1; sig < NSIG; sig++) {
        if (sig == SIGINT || sig == SIGPIPE) continue; /* bash never reports these */
        const char *d = strsignal(sig);
        if (!d || !*d || !strncmp(d, "Unknown signal", 14)) continue;
        sig_desc[sig_desc_n++] = xstrdup(d);
    }
}

/* length of the signal description starting at p (followed by a space or the line end), 0 if none */
static size_t match_sig_desc(const char *p, const char *end) {
    pthread_once(&sig_desc_once, sig_desc_init);
    size_t best = 0;
    for (size_t i = 0; i < sig_desc_n; i++) {
        size_t n = strlen(sig_desc[i]);
        if ((size_t)(end - p) >= n && !memcmp(p, sig_desc[i], n) && (p + n == end || p[n] == ' ') && n > best) best = n;
    }
    return best;
}

/* `Desc<pad to 27>[(core dumped) ]cmd` from p to the line end */
static bool match_job_status(const char *p, const char *end) {
    size_t n = match_sig_desc(p, end);
    if (!n) return false;
    const char *q = p + n;
    while (q < end && *q == ' ') q++;
    return q - p >= 27 || (q - p == (ptrdiff_t)n + 1 && n >= 26);
}

static size_t digits(const char *p, const char *end) {
    size_t n = 0;
    while (p + n < end && p[n] >= '0' && p[n] <= '9') n++;
    return n;
}

/* `<sh>: line N: PID <job status>` from p to the line end */
static bool match_job_notice(const char *sh, const char *p, const char *end) {
    size_t sl = strlen(sh);
    if ((size_t)(end - p) < sl + 7 || memcmp(p, sh, sl) || memcmp(p + sl, ": line ", 7)) return false;
    const char *q = p + sl + 7;
    size_t d = digits(q, end);
    if (!d || end - (q + d) < 2 || memcmp(q + d, ": ", 2)) return false;
    q += d + 2;
    while (q < end && *q == ' ') q++;
    d = digits(q, end);
    if (!d || q + d >= end || q[d] != ' ') return false;
    return match_job_status(q + d + 1, end);
}

/* `<script>: line N: ` -> `bash: line N: ` as a one-shot `bash -c` says it, parse errors as `bash: -c: line N: `.
 * Returns the bytes removed from the line at ls..le. */
static size_t rewrite_script_name(const char *script, char *buf, size_t *len, char *ls, char *le, size_t min_keep) {
    size_t sl = strlen(script), removed = 0;
    for (char *p = ls; (p = memmem(p, (size_t)(le - p), script, sl)) != NULL;) {
        const char *q = p + sl;
        size_t d = (size_t)(le - q) > 7 && !memcmp(q, ": line ", 7) ? digits(q + 7, le) : 0;
        if (!d || (size_t)(p - buf) < min_keep) {
            p += sl;
            continue;
        }
        const char *msg = q + 7 + d;
        bool parse = le - msg > 2 && !memcmp(msg, ": ", 2) &&
                     ((size_t)(le - msg) >= 14 && !memcmp(msg + 2, "syntax error", 12) ? true
                      : (size_t)(le - msg) >= 16 && !memcmp(msg + 2, "unexpected EOF", 14) ? true
                                                                                           : msg[2] == '`');
        const char *name = proc_shell_name();
        char *rep = parse ? xasprintf("%s: -c", name) : xstrdup(name);
        size_t rl = strlen(rep);
        if (rl <= sl) { /* the private path is never shorter than the shell's name */
            memcpy(p, rep, rl);
            memmove(p + rl, p + sl, (size_t)(buf + *len - (p + sl)));
            *len -= sl - rl;
            le -= sl - rl;
            removed += sl - rl;
        }
        free(rep);
        p += rl;
    }
    return removed;
}

/* Drop job notices from the complete lines of buf[*from, *len) (never touching bytes before min_keep); *from becomes
 * the start of the trailing incomplete line. Returns how many tail bytes may still grow into a notice (to hold back
 * from streaming). With `script` (sh is then the same string), error messages naming the command file read as `bash -c`'s. */
static size_t strip_job_notices_ex(const char *sh, const char *script, char *buf, size_t *len, size_t *from, size_t min_keep) {
    size_t i = *from;
    while (i < *len) {
        char *nl = memchr(buf + i, '\n', *len - i);
        if (!nl) break;
        char *ls = buf + i, *le = nl;
        char *cut = NULL;
        if (match_job_status(ls, le)) cut = ls;
        for (char *p = ls; !cut && (p = memmem(p, (size_t)(le - p), sh, strlen(sh))) != NULL; p++)
            if (match_job_notice(sh, p, le)) cut = p;
        if (cut && (size_t)(cut - buf) >= min_keep) {
            size_t a = (size_t)(cut - buf), b = (size_t)(le - buf) + 1;
            memmove(buf + a, buf + b, *len - b);
            *len -= b - a;
            /* rescan the line: a notice glued to unterminated output leaves that output joined to the next line */
            i = (size_t)(ls - buf);
            continue;
        }
        if (script) le -= rewrite_script_name(script, buf, len, ls, le, min_keep);
        i = (size_t)(le - buf) + 1;
    }
    *from = i;
    /* incomplete tail: hold from the earliest point a notice could start */
    const char *t = buf + i, *end = buf + *len;
    if (t == end) return 0;
    size_t hold_from = SIZE_MAX;
    pthread_once(&sig_desc_once, sig_desc_init);
    for (size_t k = 0; k < sig_desc_n && hold_from == SIZE_MAX; k++) {
        size_t n = strlen(sig_desc[k]), tl = (size_t)(end - t);
        if (tl <= n ? !memcmp(t, sig_desc[k], tl) : (!memcmp(t, sig_desc[k], n) && t[n] == ' ')) hold_from = i;
    }
    size_t sl = strlen(sh);
    char *full = memmem(t, (size_t)(end - t), sh, sl);
    if (full && (size_t)(full - buf) < hold_from) hold_from = (size_t)(full - buf);
    for (const char *p = end - MIN((size_t)(end - t), sl); p < end; p++)
        if (*p == sh[0] && !memcmp(p, sh, (size_t)(end - p)) && (size_t)(p - buf) < hold_from) {
            hold_from = (size_t)(p - buf);
            break;
        }
    return hold_from == SIZE_MAX ? 0 : *len - hold_from;
}

static size_t strip_job_notices(const char *sh, char *buf, size_t *len, size_t *from, size_t min_keep) {
    return strip_job_notices_ex(sh, NULL, buf, len, from, min_keep);
}

void shell_strip_job_notices(char *buf, size_t *len) {
    size_t from = 0;
    strip_job_notices(proc_shell_name(), buf, len, &from, 0);
}

static int run_locked(shell_t *s, const char *cmd, const char *cwd, const proc_opts_t *o, proc_result_t *res);

int shell_run(shell_t *s, const char *cmd, const char *cwd, const proc_opts_t *o, proc_result_t *res) {
    memset(res, 0, sizeof(*res));
    pthread_mutex_lock(&s->mu);
    return run_locked(s, cmd, cwd, o, res);
}

int shell_run_try(shell_t *s, const char *cmd, const char *cwd, const proc_opts_t *o, proc_result_t *res) {
    memset(res, 0, sizeof(*res));
    if (pthread_mutex_trylock(&s->mu) != 0) return -2;
    return run_locked(s, cmd, cwd, o, res);
}

/* runs with s->mu held; releases it */
static int run_locked(shell_t *s, const char *cmd, const char *cwd, const proc_opts_t *o, proc_result_t *res) {
    if (!s->started && start(s, o) != 0) {
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    /* The command is a file sourced in the shell itself, so its state persists, a syntax error fails it alone and its line
     * numbers start at 1. Its group has its own output pipe: `exec` redirections end with the group and a job left running
     * keeps a pipe nobody shows. The shell's real stdout carries only the status line. */
    char *fifo = xasprintf("%s/f%llu", s->dir, (unsigned long long)++s->seq);
    int ofd = -1;
    if (mkfifo(fifo, 0600) == 0) ofd = open(fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (ofd < 0 || fs_write(s->script, cmd, strlen(cmd)) != 0) {
        if (ofd >= 0) close(ofd);
        unlink(fifo);
        free(fifo);
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    char *qcwd = sh_quote(cwd ? cwd : "/"), *qscript = sh_quote(s->script), *qfifo = sh_quote(fifo);
    /* an EXIT trap belongs to its command: it fires once at the command's end, with its status in $?, not at the session's */
    const char *exit_trap = s->is_bash ? "__omc_t=$(trap -p EXIT); if [ -n \"$__omc_t\" ]; then __omc_t=${__omc_t#trap -- }; "
                                         "eval \"__omc_t=${__omc_t% EXIT}\"; trap - EXIT; (exit $__omc_rc); eval \"$__omc_t\"; fi; "
                                         "\\unset __omc_t; "
                                       : "";
    char *script = xasprintf("\\cd -- %s 2>/dev/null\n{ \\. %s; __omc_rc=$?; %s} </dev/null >%s 2>&1; \\command printf '__OMC_DONE_%%d\\n' \"$__omc_rc\"; \\unset __omc_rc\n",
                             qcwd, qscript, exit_trap, qfifo);
    free(qcwd);
    free(qscript);
    free(qfifo);
    if (write_all(s->child.in_fd, script, strlen(script)) != 0) {
        free(script);
        close(ofd);
        unlink(fifo);
        free(fifo);
        stop(s);
        /* shell died: restart once and retry */
        if (start(s, o) != 0) {
            pthread_mutex_unlock(&s->mu);
            return -1;
        }
        return run_locked(s, cmd, cwd, o, res);
    }
    free(script);
    size_t cap = o && o->max_output ? o->max_output : (16u << 20);
    sb_t out = SB_INIT, ctl = SB_INIT;
    int64_t deadline = o && o->timeout_ms > 0 ? mono_ms() + o->timeout_ms : 0;
    size_t emitted = 0, scan = 0;
    bool done = false, ofd_eof = false;
    while (!done) {
        if (o && o->cancel && *o->cancel) {
            res->cancelled = true;
            break;
        }
        if (deadline && mono_ms() > deadline) {
            res->timed_out = true;
            break;
        }
        struct pollfd pf[2] = {{s->child.out_fd, POLLIN, 0}, {ofd, POLLIN, 0}};
        /* no writer yet, or none left for now: only the status line can still end this */
        poll(pf, ofd_eof ? 1 : 2, ofd_eof ? 20 : 100);
        char buf[16384];
        ssize_t n = read(ofd, buf, sizeof buf);
        ofd_eof = n == 0;
        if (n > 0) sb_appendn(&out, buf, (size_t)n);
        if (pf[0].revents) {
            ssize_t c = read(s->child.out_fd, buf, sizeof buf);
            if (c > 0) {
                sb_appendn(&ctl, buf, (size_t)c);
                char *hit = memmem(ctl.s, ctl.len, "__OMC_DONE_", 11);
                char *end = hit ? memchr(hit, '\n', ctl.len - (size_t)(hit - ctl.s)) : NULL;
                if (end) {
                    res->exit_code = atoi(hit + 11);
                    done = true;
                }
            } else if (c == 0 || (errno != EAGAIN && errno != EINTR)) {
                /* the command exited the shell itself (e.g. `exit 3`): report its status, restart next time */
                int st = child_wait(&s->child, 2000);
                res->exit_code = st >= 0 ? st : -1;
                child_close(&s->child);
                s->started = false;
                done = true;
            }
        }
        if (done) { /* what the command wrote before it ended */
            while ((n = read(ofd, buf, sizeof buf)) > 0) sb_appendn(&out, buf, (size_t)n);
            ofd_eof = n == 0;
        }
        size_t hold = strip_job_notices_ex(s->script, s->script, out.s ? out.s : "", &out.len, &scan, emitted);
        if (out.s) out.s[out.len] = 0;
        /* stream all but what could still become a notice; a finished command has nothing left to wait for */
        size_t safe = done ? out.len : out.len - hold;
        if (o && o->on_output && safe > emitted) {
            o->on_output(o->ud, 1, out.s + emitted, safe - emitted);
            emitted = safe;
        }
        if (out.len > cap) res->truncated = true;
    }
    sb_free(&ctl);
    if (!done && s->started) {
        /* kill the whole group (shell + children); state is lost, a fresh shell starts next time */
        stop(s);
        if (!res->timed_out && !res->cancelled && res->exit_code == 0) res->exit_code = -1;
    }
    /* a job still holding the pipe keeps running; whatever it writes from here on is nobody's output */
    if (done && !ofd_eof) drain_add(ofd);
    else close(ofd);
    unlink(fifo);
    free(fifo);
    if (out.len > cap) {
        memmove(out.s, out.s + out.len - cap, cap);
        sb_truncate(&out, cap);
    }
    res->out_len = out.len;
    res->out = out.s ? sb_detach(&out) : xstrdup("");
    res->err = xstrdup("");
    pthread_mutex_unlock(&s->mu);
    return 0;
}

/* ---------- one-shot `bash -c` with the same notice filtering ---------- */

typedef struct {
    const proc_opts_t *o;
    sb_t pend; /* received, not yet forwarded */
    size_t scan;
} once_filter_t;

static void once_out(void *ud, int stream, const char *data, size_t len) {
    once_filter_t *f = ud;
    sb_appendn(&f->pend, data, len);
    size_t hold = strip_job_notices(proc_shell_name(), f->pend.s, &f->pend.len, &f->scan, 0);
    size_t safe = f->pend.len - hold;
    if (safe && f->o->on_output) f->o->on_output(f->o->ud, stream, f->pend.s, safe);
    memmove(f->pend.s, f->pend.s + safe, f->pend.len - safe);
    f->pend.len -= safe;
    f->scan = f->scan >= safe ? f->scan - safe : 0;
}

int shell_exec_once(const char *cmd, const proc_opts_t *o, proc_result_t *res) {
    if (!o->use_pty) {
        /* a session of its own: the same rules as the persistent shell; a job the command left running outlives it */
        shell_t *s = shell_new();
        int rc = shell_run(s, cmd, o->cwd, o, res);
        if (s->started) {
            child_close(&s->child);
            s->started = false;
        }
        shell_free(s);
        return rc;
    }
    once_filter_t f = {.o = o, .pend = SB_INIT};
    proc_opts_t oo = *o;
    oo.on_output = once_out;
    oo.ud = &f;
    int rc = proc_shell(cmd, &oo, res);
    if (f.pend.len && o->on_output) o->on_output(o->ud, 1, f.pend.s, f.pend.len);
    sb_free(&f.pend);
    if (res->out) {
        shell_strip_job_notices(res->out, &res->out_len);
        res->out[res->out_len] = 0;
    }
    return rc;
}
