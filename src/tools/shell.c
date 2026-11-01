#define _GNU_SOURCE
#include "shell.h"

#include "core/fs.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

struct shell {
    child_t child;
    bool started;
    char *nonce;
    pthread_mutex_t mu;
};

shell_t *shell_new(void) {
    shell_t *s = xcalloc(1, sizeof(*s));
    s->child.in_fd = s->child.out_fd = s->child.err_fd = -1;
    pthread_mutex_init(&s->mu, NULL);
    return s;
}

static void stop(shell_t *s) {
    if (!s->started) return;
    child_kill(&s->child, SIGKILL);
    child_close(&s->child);
    s->started = false;
    free(s->nonce);
    s->nonce = NULL;
}

void shell_free(shell_t *s) {
    if (!s) return;
    stop(s);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

static int start(shell_t *s, const proc_opts_t *o) {
    const char *sh = proc_shell_path();
    bool is_bash = str_ends(sh, "bash");
    const char *argv_bash[] = {sh, "--noprofile", "--norc", NULL};
    const char *argv_sh[] = {sh, NULL};
    if (child_spawn(is_bash ? argv_bash : argv_sh, NULL, o ? o->env_extra : NULL, false, &s->child) != 0) return -1;
    s->started = true;
    s->nonce = rand_hex(8);
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

/* Drop job notices from the complete lines of buf[*from, *len) (never touching bytes before min_keep); *from becomes
 * the start of the trailing incomplete line. Returns how many tail bytes may still grow into a notice (to hold back
 * from streaming). */
static size_t strip_job_notices(const char *sh, char *buf, size_t *len, size_t *from, size_t min_keep) {
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

void shell_strip_job_notices(char *buf, size_t *len) {
    size_t from = 0;
    strip_job_notices(proc_shell_path(), buf, len, &from, 0);
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
    char *qcwd = sh_quote(cwd ? cwd : "/");
    /* The command runs in the shell itself (state persists); output merged; stdin detached. */
    char *script = xasprintf("cd -- %s 2>/dev/null\n{\n%s\n} </dev/null 2>&1\n__omc_rc=$?\nprintf '\\n__OMC_DONE_%s_%%d__\\n' \"$__omc_rc\"\n",
                             qcwd, cmd, s->nonce);
    free(qcwd);
    if (write_all(s->child.in_fd, script, strlen(script)) != 0) {
        free(script);
        stop(s);
        /* shell died: restart once and retry */
        if (start(s, o) != 0) {
            pthread_mutex_unlock(&s->mu);
            return -1;
        }
        return run_locked(s, cmd, cwd, o, res);
    }
    free(script);
    char *marker = xasprintf("\n__OMC_DONE_%s_", s->nonce);
    size_t mlen = strlen(marker);
    size_t cap = o && o->max_output ? o->max_output : (16u << 20);
    sb_t out = SB_INIT;
    int64_t deadline = o && o->timeout_ms > 0 ? mono_ms() + o->timeout_ms : 0;
    size_t emitted = 0, scan = 0;
    bool done = false;
    while (!done) {
        if (o && o->cancel && *o->cancel) {
            res->cancelled = true;
            break;
        }
        if (deadline && mono_ms() > deadline) {
            res->timed_out = true;
            break;
        }
        struct pollfd pf = {s->child.out_fd, POLLIN, 0};
        int pr = poll(&pf, 1, 100);
        if (pr <= 0) continue;
        char buf[16384];
        ssize_t n = read(s->child.out_fd, buf, sizeof buf);
        if (n <= 0) {
            /* the command exited the shell itself (e.g. `exit 3`): report its status, restart next time */
            int st = child_wait(&s->child, 2000);
            res->exit_code = st >= 0 ? st : -1;
            child_close(&s->child);
            s->started = false;
            free(s->nonce);
            s->nonce = NULL;
            done = true;
            break;
        }
        sb_appendn(&out, buf, (size_t)n);
        size_t note_hold = strip_job_notices(proc_shell_path(), out.s, &out.len, &scan, emitted);
        /* search for the marker (allow the leading newline to be the start of output) */
        char *hit = memmem(out.s, out.len, marker, mlen);
        if (!hit && out.len >= mlen - 1 && memcmp(out.s, marker + 1, mlen - 1) == 0) hit = out.s - 1;
        if (hit) {
            char *num = hit + mlen;
            char *end = memmem(num, out.len - (size_t)(num - out.s), "__\n", 3);
            if (end) {
                res->exit_code = atoi(num);
                size_t keep = hit < out.s ? 0 : (size_t)(hit - out.s);
                if (o && o->on_output && keep > emitted) o->on_output(o->ud, 1, out.s + emitted, keep - emitted);
                sb_truncate(&out, keep);
                done = true;
                break;
            }
        }
        /* stream everything except a possible partial marker at the tail */
        /* hold back only a tail that could still grow into the marker, so short output streams live */
        size_t hold = 0;
        for (size_t k = MIN(out.len, mlen + 32); k > 0; k--)
            if (memcmp(out.s + out.len - k, marker, MIN(k, mlen)) == 0 || (k <= mlen - 1 && memcmp(out.s + out.len - k, marker + 1, k) == 0)) {
                hold = k;
                break;
            }
        if (note_hold > hold) hold = note_hold;
        size_t safe = out.len - hold;
        if (o && o->on_output && safe > emitted) {
            o->on_output(o->ud, 1, out.s + emitted, safe - emitted);
            emitted = safe;
        }
        if (out.len > cap) res->truncated = true;
    }
    if (!done && s->started) {
        /* kill the whole group (shell + children); state is lost, a fresh shell starts next time */
        stop(s);
        if (!res->timed_out && !res->cancelled && res->exit_code == 0) res->exit_code = -1;
    }
    free(marker);
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
    size_t hold = strip_job_notices(proc_shell_path(), f->pend.s, &f->pend.len, &f->scan, 0);
    size_t safe = f->pend.len - hold;
    if (safe && f->o->on_output) f->o->on_output(f->o->ud, stream, f->pend.s, safe);
    memmove(f->pend.s, f->pend.s + safe, f->pend.len - safe);
    f->pend.len -= safe;
    f->scan = f->scan >= safe ? f->scan - safe : 0;
}

int shell_exec_once(const char *cmd, const proc_opts_t *o, proc_result_t *res) {
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
