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

int shell_run(shell_t *s, const char *cmd, const char *cwd, const proc_opts_t *o, proc_result_t *res) {
    memset(res, 0, sizeof(*res));
    pthread_mutex_lock(&s->mu);
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
        pthread_mutex_unlock(&s->mu);
        return shell_run(s, cmd, cwd, o, res);
    }
    free(script);
    char *marker = xasprintf("\n__OMC_DONE_%s_", s->nonce);
    size_t mlen = strlen(marker);
    size_t cap = o && o->max_output ? o->max_output : (16u << 20);
    sb_t out = SB_INIT;
    int64_t deadline = o && o->timeout_ms > 0 ? mono_ms() + o->timeout_ms : 0;
    size_t emitted = 0;
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
        size_t safe = out.len > mlen + 32 ? out.len - mlen - 32 : 0;
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
