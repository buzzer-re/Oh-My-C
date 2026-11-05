/* jobs.c - async background jobs (port of omp's AsyncJobManager subset): `bash` with async:true or a foreground
 * command that outlives the auto-background threshold, subagent task jobs and service watchers. Results are
 * delivered to the agent as a follow-up (or collected by the `wait` tool). */
#define _GNU_SOURCE
#include "jobs.h"
#include "artifacts.h"
#include "outsink.h"

#include "config/settings.h"
#include "core/fs.h"
#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONSUMED_EVICT_MS 30000
#define RETENTION_MS (5 * 60 * 1000)
#define LIVE_OUTPUT_CAP (64 * 1024)
#define STREAM_CAP (1u << 20)

/* ---------- pipe stream taps ---------- */

typedef struct {
    char *producer, *consumer, *fifo;
    sb_t buf;      /* last STREAM_CAP bytes */
    size_t total;  /* bytes seen */
    size_t start;  /* offset of buf[0] in the stream */
    int fd;        /* reader */
    int keep_fd;   /* our own writer: keeps the fifo from hitting EOF between writers */
} pstream_t;

typedef struct {
    char *dir;
    VEC(pstream_t) v;
    pthread_t th;
    bool started;
    volatile bool stop;
    pthread_mutex_t mu;
} streams_t;

static void *streams_reader(void *p) {
    streams_t *s = p;
    char buf[16384];
    for (;;) {
        struct pollfd pf[64];
        size_t n = 0;
        pthread_mutex_lock(&s->mu);
        for (size_t i = 0; i < s->v.n && n < 64; i++)
            if (s->v.v[i].fd >= 0) pf[n++] = (struct pollfd){s->v.v[i].fd, POLLIN, 0};
        pthread_mutex_unlock(&s->mu);
        if (!n) break;
        int pr = poll(pf, n, 100);
        bool any = false;
        if (pr > 0) {
            for (size_t k = 0; k < n; k++) {
                if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                ssize_t r = read(pf[k].fd, buf, sizeof buf);
                pthread_mutex_lock(&s->mu);
                for (size_t i = 0; i < s->v.n; i++) {
                    pstream_t *ps = &s->v.v[i];
                    if (ps->fd != pf[k].fd) continue;
                    if (r > 0) {
                        any = true;
                        sb_appendn(&ps->buf, buf, (size_t)r);
                        ps->total += (size_t)r;
                        if (ps->buf.len > STREAM_CAP) {
                            size_t drop = ps->buf.len - STREAM_CAP;
                            memmove(ps->buf.s, ps->buf.s + drop, STREAM_CAP);
                            sb_truncate(&ps->buf, STREAM_CAP);
                            ps->start += drop;
                        }
                    } else if (r == 0 && s->stop) {
                        close(ps->fd);
                        ps->fd = -1;
                    }
                }
                pthread_mutex_unlock(&s->mu);
            }
        }
        if (s->stop && !any) {
            /* job ended: release our writers so pending data drains to EOF */
            pthread_mutex_lock(&s->mu);
            bool open_writers = false;
            for (size_t i = 0; i < s->v.n; i++) {
                if (s->v.v[i].keep_fd >= 0) {
                    close(s->v.v[i].keep_fd);
                    s->v.v[i].keep_fd = -1;
                    open_writers = true;
                }
            }
            if (!open_writers)
                for (size_t i = 0; i < s->v.n; i++)
                    if (s->v.v[i].fd >= 0) {
                        close(s->v.v[i].fd);
                        s->v.v[i].fd = -1;
                    }
            pthread_mutex_unlock(&s->mu);
        }
    }
    return NULL;
}

static bool word_at(const char *s, size_t i, const char *w) {
    size_t n = strlen(w);
    if (strncmp(s + i, w, n) != 0) return false;
    if (i > 0 && !strchr(" \t\n;&|(", s[i - 1])) return false;
    char c = s[i + n];
    return c == 0 || strchr(" \t\n;&|)", c);
}

static char *stage_text(const char *s, size_t a, size_t b) {
    while (a < b && strchr(" \t\n", s[a])) a++;
    while (b > a && strchr(" \t\n", s[b - 1])) b--;
    static const char *kw[] = {"do", "then", "else", "!", "time"};
    for (size_t k = 0; k < ARRAY_LEN(kw); k++) {
        size_t n = strlen(kw[k]);
        if (b - a > n && !strncmp(s + a, kw[k], n) && strchr(" \t", s[a + n])) {
            a += n;
            while (a < b && strchr(" \t", s[a])) a++;
        }
    }
    return xstrndup(s + a, b - a);
}

char *pipe_tap_rewrite(const char *cmd, const char *dir, strvec_t *producers, strvec_t *consumers) {
    size_t len = strlen(cmd);
    VEC(size_t) pipes = {0}; /* positions of top-level '|' */
    VEC(size_t) seps = {0};  /* stage boundaries (exclusive end of the previous stage) */
    int depth = 0;
    char q = 0;
    bool bad = false;
    for (size_t i = 0; i < len && !bad; i++) {
        char c = cmd[i];
        if (q == '\'') {
            if (c == '\'') q = 0;
            continue;
        }
        if (c == '\\') {
            i++;
            continue;
        }
        if (q == '"') {
            if (c == '"') q = 0;
            else if (c == '`' || (c == '$' && cmd[i + 1] == '(')) bad = true; /* nested substitution in quotes */
            continue;
        }
        if (c == '\'' || c == '"') {
            q = c;
            continue;
        }
        if (c == '#' && (i == 0 || strchr(" \t\n;&|(", cmd[i - 1]))) {
            while (i < len && cmd[i] != '\n') i++;
            continue;
        }
        if (c == '`') {
            bad = true; /* backtick substitutions: do not rewrite */
            break;
        }
        if (c == '<' && cmd[i + 1] == '<') {
            bad = true; /* heredocs */
            break;
        }
        if (depth == 0 && word_at(cmd, i, "case")) {
            bad = true;
            break;
        }
        if (c == '(' || (c == '{' && i > 0 && cmd[i - 1] == '$') || (c == '[' && cmd[i + 1] == '[')) {
            depth++;
            if (c == '[') i++;
            continue;
        }
        if (c == ')' || (c == '}' && depth > 0) || (c == ']' && cmd[i + 1] == ']' && depth > 0)) {
            if (depth > 0) depth--;
            if (c == ']') i++;
            continue;
        }
        if (depth) continue;
        if (c == '|') {
            if (cmd[i + 1] == '|') {
                vec_push(&seps, i);
                i++;
                continue;
            }
            vec_push(&pipes, i);
            vec_push(&seps, i);
            if (cmd[i + 1] == '&') i++;
            continue;
        }
        if (c == ';' || c == '\n' || c == '&' || c == '{' || c == '}') vec_push(&seps, i);
    }
    if (q || depth) bad = true;
    if (bad || !pipes.n) {
        vec_free(&pipes);
        vec_free(&seps);
        return NULL;
    }
    sb_t out = SB_INIT;
    size_t last = 0;
    for (size_t k = 0; k < pipes.n; k++) {
        size_t p = pipes.v[k];
        /* stage before: from the previous separator to p; after: to the next separator */
        size_t a = 0, b = len;
        for (size_t s = 0; s < seps.n; s++) {
            if (seps.v[s] < p) a = seps.v[s] + 1;
            if (seps.v[s] > p) {
                b = seps.v[s];
                break;
            }
        }
        if (a < p && cmd[a] == '&') a++;
        size_t ca = p + 1 + (cmd[p + 1] == '&');
        strvec_push_own(producers, stage_text(cmd, a, p));
        strvec_push_own(consumers, stage_text(cmd, ca, b));
        bool amp = cmd[p + 1] == '&';
        sb_appendn(&out, cmd + last, p - last);
        sb_appendf(&out, "%s tee '%s/%zu' 2>/dev/null |", amp ? "|&" : "|", dir, k);
        last = p + 1 + amp;
    }
    sb_append(&out, cmd + last);
    vec_free(&pipes);
    vec_free(&seps);
    return sb_detach(&out);
}

static streams_t *streams_setup(const char *cmd, char **rewritten) {
    *rewritten = NULL;
    char *base = omc_agent_path("tmp");
    fs_mkdir_p(base, 0700);
    char *rid = short_id();
    char *dir = xasprintf("%s/pipes-%d-%s", base, (int)getpid(), rid);
    free(rid);
    free(base);
    strvec_t prod = {0}, cons = {0};
    char *rw = pipe_tap_rewrite(cmd, dir, &prod, &cons);
    if (!rw || mkdir(dir, 0700) != 0) {
        free(rw);
        free(dir);
        strvec_free(&prod);
        strvec_free(&cons);
        return NULL;
    }
    streams_t *s = xcalloc(1, sizeof *s);
    s->dir = dir;
    pthread_mutex_init(&s->mu, NULL);
    for (size_t i = 0; i < prod.n; i++) {
        pstream_t ps = {0};
        ps.fifo = xasprintf("%s/%zu", dir, i);
        ps.producer = xstrdup(prod.v[i]);
        ps.consumer = xstrdup(cons.v[i]);
        ps.fd = ps.keep_fd = -1;
        if (mkfifo(ps.fifo, 0600) == 0) {
            ps.fd = open(ps.fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (ps.fd >= 0) ps.keep_fd = open(ps.fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        }
        vec_push(&s->v, ps);
    }
    strvec_free(&prod);
    strvec_free(&cons);
    s->started = pthread_create(&s->th, NULL, streams_reader, s) == 0;
    *rewritten = rw;
    return s;
}

static void streams_finish(streams_t *s) {
    if (!s) return;
    s->stop = true;
    if (s->started) {
        pthread_join(s->th, NULL);
        s->started = false;
    }
    for (size_t i = 0; i < s->v.n; i++) {
        pstream_t *ps = &s->v.v[i];
        if (ps->fd >= 0) close(ps->fd);
        if (ps->keep_fd >= 0) close(ps->keep_fd);
        ps->fd = ps->keep_fd = -1;
        if (ps->fifo) unlink(ps->fifo);
    }
    if (s->dir) rmdir(s->dir);
}

static void streams_free(streams_t *s) {
    if (!s) return;
    streams_finish(s);
    for (size_t i = 0; i < s->v.n; i++) {
        free(s->v.v[i].producer);
        free(s->v.v[i].consumer);
        free(s->v.v[i].fifo);
        sb_free(&s->v.v[i].buf);
    }
    vec_free(&s->v);
    pthread_mutex_destroy(&s->mu);
    free(s->dir);
    free(s);
}

/* ---------- jobs ---------- */

typedef struct {
    char *cmd, *cwd;
    int64_t timeout_ms;
    struct shell *shell;
    proc_result_t res;
    int rc;
    bool temp_shell;
    pthread_mutex_t fwd_mu;
    proc_output_cb fwd;
    void *fwd_ud;
    streams_t *streams;
} bashx_t;

struct job {
    char *id, *type, *label, *agent_id;
    int64_t started, ended, delivered_at;
    char *result;
    sb_t output;
    bool done, delivered, failed, hidden, attached, cancelled, quiet;
    bool killed; /* jobs_cancel: settles as cancelled and is never delivered (AsyncJobManager.cancel) */
    volatile sig_atomic_t cancel;
    pthread_t th;
    bool joinable;
    jobs_t *owner;
    job_fn fn;
    void *ud;
    void (*ud_free)(void *);
    bashx_t *bash;
};

struct jobs {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    VEC(bgjob_t *) v;
    uint64_t next, next_fg;
    void (*notify)(void *ud);
    void *notify_ud;
    strvec_t services;
};

jobs_t *jobs_new(void) {
    jobs_t *j = xcalloc(1, sizeof(*j));
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    return j;
}

void jobs_set_notify(jobs_t *j, void (*fn)(void *ud), void *ud) {
    pthread_mutex_lock(&j->mu);
    j->notify = fn;
    j->notify_ud = ud;
    pthread_mutex_unlock(&j->mu);
}

static void bashx_free(bashx_t *b) {
    if (!b) return;
    free(b->cmd);
    free(b->cwd);
    proc_result_free(&b->res);
    pthread_mutex_destroy(&b->fwd_mu);
    streams_free(b->streams);
    free(b);
}

static void job_free(bgjob_t *x) {
    if (x->joinable) pthread_join(x->th, NULL);
    if (x->ud_free) x->ud_free(x->ud);
    bashx_free(x->bash);
    free(x->id);
    free(x->label);
    free(x->type);
    free(x->agent_id);
    free(x->result);
    sb_free(&x->output);
    free(x);
}

/* remove evicted rows (caller holds mu); evicted jobs are returned for freeing outside the lock */
typedef VEC(bgjob_t *) jobvec_t;
static void prune_locked(jobs_t *j, jobvec_t *dead) {
    int64_t now = now_ms();
    for (size_t i = 0; i < j->v.n;) {
        bgjob_t *x = j->v.v[i];
        bool evict = x->done && x->delivered && !x->attached &&
                     ((x->delivered_at && now - x->delivered_at > CONSUMED_EVICT_MS) || now - x->ended > RETENTION_MS);
        if (evict) {
            vec_push(dead, x);
            memmove(j->v.v + i, j->v.v + i + 1, (j->v.n - i - 1) * sizeof(bgjob_t *));
            j->v.n--;
        } else {
            i++;
        }
    }
}

static void prune(jobs_t *j) {
    jobvec_t dead = {0};
    pthread_mutex_lock(&j->mu);
    prune_locked(j, &dead);
    pthread_mutex_unlock(&j->mu);
    for (size_t i = 0; i < dead.n; i++) job_free(dead.v[i]);
    vec_free(&dead);
}

void services_stop_owned(jobs_t *j); /* services.c */

void jobs_free(jobs_t *j) {
    if (!j) return;
    services_stop_owned(j);
    pthread_mutex_lock(&j->mu);
    j->notify = NULL;
    for (size_t i = 0; i < j->v.n; i++) j->v.v[i]->cancel = 1;
    pthread_mutex_unlock(&j->mu);
    for (size_t i = 0; i < j->v.n; i++) job_free(j->v.v[i]);
    vec_free(&j->v);
    strvec_free(&j->services);
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv);
    free(j);
}

const char *job_id(const bgjob_t *job) { return job->id; }
volatile sig_atomic_t *job_cancel_flag(bgjob_t *job) { return &job->cancel; }

void job_set_output(bgjob_t *job, const char *text) {
    pthread_mutex_lock(&job->owner->mu);
    sb_clear(&job->output);
    sb_append(&job->output, text ? text : "");
    pthread_mutex_unlock(&job->owner->mu);
}

static void *job_thread(void *p) {
    bgjob_t *x = p;
    bool failed = false;
    char *res = x->fn(x, x->ud, &failed);
    jobs_t *j = x->owner;
    pthread_mutex_lock(&j->mu);
    x->result = res ? res : xstrdup("");
    x->failed = failed;
    x->cancelled = x->cancel != 0;
    x->ended = now_ms();
    x->done = true;
    if (x->quiet || x->killed) {
        x->delivered = true;
        x->delivered_at = x->ended;
    }
    bool visible = !x->attached && !x->quiet;
    void (*fn)(void *) = visible ? j->notify : NULL;
    void *ud = j->notify_ud;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
    if (fn) fn(ud);
    return NULL;
}

static bool id_taken(jobs_t *j, const char *id) {
    for (size_t i = 0; i < j->v.n; i++)
        if (str_eq(j->v.v[i]->id, id)) return true;
    return false;
}

static bgjob_t *job_new(jobs_t *j, const char *type, const char *label) {
    bgjob_t *x = xcalloc(1, sizeof(*x));
    x->owner = j;
    x->type = xstrdup(type);
    char *l = label ? xstrdup(label) : xstrdup(type);
    char *nl = strchr(l, '\n');
    if (nl) *nl = 0;
    if (strlen(l) > 60) strcpy(l + 57, "...");
    x->label = l;
    x->started = now_ms();
    return x;
}

/* caller holds mu */
static char *assign_id(jobs_t *j, const char *hint) {
    if (!hint) {
        char *id = NULL;
        do {
            free(id);
            id = xasprintf("bg_%llu", (unsigned long long)++j->next);
        } while (id_taken(j, id));
        return id;
    }
    if (!id_taken(j, hint)) return xstrdup(hint);
    for (int n = 2;; n++) {
        char *id = xasprintf("%s-%d", hint, n);
        if (!id_taken(j, id)) return id;
        free(id);
    }
}

static void spawn(jobs_t *j, bgjob_t *x) {
    vec_push(&j->v, x);
    if (pthread_create(&x->th, NULL, job_thread, x) == 0) {
        x->joinable = true;
    } else {
        x->done = true;
        x->failed = true;
        x->ended = now_ms();
        x->result = xstrdup("Failed to start background job\n");
    }
}

char *jobs_start(jobs_t *j, const char *type, const char *id_hint, const char *label, const char *agent_id, bool hidden,
                 job_fn fn, void *ud, void (*ud_free)(void *)) {
    prune(j);
    bgjob_t *x = job_new(j, type, label);
    x->fn = fn;
    x->ud = ud;
    x->ud_free = ud_free;
    x->hidden = hidden;
    if (agent_id) x->agent_id = xstrdup(agent_id);
    pthread_mutex_lock(&j->mu);
    x->id = assign_id(j, id_hint);
    char *id = xstrdup(x->id);
    spawn(j, x);
    pthread_mutex_unlock(&j->mu);
    return id;
}

/* ---------- bash jobs ---------- */

static const char *BASH_ENV[] = {"PAGER=cat", "GIT_PAGER=cat", "GIT_TERMINAL_PROMPT=0", "TERM=dumb", "NO_COLOR=1", "OMC=1", NULL};

static void bash_out(void *ud, int stream, const char *data, size_t len) {
    bgjob_t *x = ud;
    pthread_mutex_lock(&x->owner->mu);
    sb_appendn(&x->output, data, len);
    if (x->output.len > 2 * LIVE_OUTPUT_CAP) {
        size_t keep = LIVE_OUTPUT_CAP;
        memmove(x->output.s, x->output.s + x->output.len - keep, keep);
        sb_truncate(&x->output, keep);
    }
    pthread_mutex_unlock(&x->owner->mu);
    bashx_t *b = x->bash;
    pthread_mutex_lock(&b->fwd_mu);
    if (b->fwd) b->fwd(b->fwd_ud, stream, data, len);
    pthread_mutex_unlock(&b->fwd_mu);
}

/* settled job text: omp's completed-result text plus its meta notice (#extractTextResult), captured
 * through an OutputSink (sanitized, column-capped, head/tail elided; no artifact) */
static char *format_bash_result(const bgjob_t *x) {
    const bashx_t *b = x->bash;
    const proc_result_t *r = &b->res;
    if (b->rc != 0 || r->spawn_errno) return xasprintf("Failed to start: %s", strerror(r->spawn_errno ? r->spawn_errno : EIO));
    out_sink_opts_t so;
    out_sink_opts_from_settings(&so);
    out_sink_t *sink = out_sink_new(&so);
    out_sink_push(sink, r->out ? r->out : "", r->out_len);
    char *tn = r->timed_out ? xasprintf("Command timed out after %lld seconds", (long long)((b->timeout_ms + 500) / 1000)) : NULL;
    out_summary_t sum;
    out_sink_dump(sink, tn ? tn : r->cancelled ? "Command cancelled" : NULL, &sum);
    free(tn);
    out_sink_free(sink);
    sb_t s = SB_INIT;
    int code = r->term_signal ? 128 + r->term_signal : r->exit_code;
    if (r->cancelled && !r->timed_out) {
        sb_append(&s, *sum.output ? sum.output : "Command aborted");
    } else {
        sb_append(&s, *sum.output ? sum.output : "(no output)");
        if (!r->timed_out && code != 0) sb_appendf(&s, "\n\nCommand exited with code %d", code);
    }
    json_t *meta = NULL;
    out_meta_from_summary(&meta, &sum, "tail", 1, 0);
    char *n = out_format_notice(meta);
    sb_append(&s, n);
    free(n);
    json_free(meta);
    out_summary_free(&sum);
    return sb_detach(&s);
}

static char *bash_body(bgjob_t *x, void *ud, bool *failed) {
    UNUSED(ud);
    bashx_t *b = x->bash;
    char *rewritten = NULL;
    if (settings_bool("bash.pipeStreams", true)) b->streams = streams_setup(b->cmd, &rewritten);
    const char *cmd = rewritten ? rewritten : b->cmd;
    proc_opts_t o = {.cwd = b->cwd,
                     .env_extra = BASH_ENV,
                     .timeout_ms = b->timeout_ms,
                     .max_output = 64u << 20,
                     .merge_stderr = true,
                     .new_session = true,
                     .cancel = &x->cancel,
                     .on_output = bash_out,
                     .ud = x};
    int rc = -2;
    if (b->shell) rc = shell_run_try(b->shell, cmd, b->cwd, &o, &b->res);
    if (rc == -2) {
        b->temp_shell = b->shell != NULL;
        rc = shell_exec_once(cmd, &o, &b->res);
    }
    b->rc = rc;
    free(rewritten);
    if (b->streams) streams_finish(b->streams);
    *failed = rc != 0 || b->res.exit_code != 0 || b->res.timed_out || b->res.cancelled;
    return format_bash_result(x);
}

static bgjob_t *bash_job_new(jobs_t *j, const char *cmd, const char *cwd, int64_t timeout_ms, const char *label) {
    bgjob_t *x = job_new(j, "bash", label ? label : cmd);
    x->bash = xcalloc(1, sizeof(bashx_t));
    x->bash->cmd = xstrdup(cmd);
    x->bash->cwd = xstrdup(cwd);
    x->bash->timeout_ms = timeout_ms;
    pthread_mutex_init(&x->bash->fwd_mu, NULL);
    x->fn = bash_body;
    return x;
}

char *jobs_start_bash(jobs_t *j, const char *cmd, const char *cwd, int64_t timeout_ms, const char *label) {
    prune(j);
    bgjob_t *x = bash_job_new(j, cmd, cwd, timeout_ms, label);
    pthread_mutex_lock(&j->mu);
    x->id = assign_id(j, NULL);
    char *id = xstrdup(x->id);
    spawn(j, x);
    pthread_mutex_unlock(&j->mu);
    return id;
}

char *jobs_start_bash_fg(jobs_t *j, const char *cmd, const char *cwd, int64_t timeout_ms, struct shell *shell,
                         proc_output_cb cb, void *cb_ud) {
    prune(j);
    bgjob_t *x = bash_job_new(j, cmd, cwd, timeout_ms, NULL);
    x->bash->shell = shell;
    x->bash->fwd = cb;
    x->bash->fwd_ud = cb_ud;
    x->attached = true;
    pthread_mutex_lock(&j->mu);
    x->id = xasprintf("fg_%llu", (unsigned long long)++j->next_fg);
    char *id = xstrdup(x->id);
    spawn(j, x);
    pthread_mutex_unlock(&j->mu);
    return id;
}

static bgjob_t *find_locked(jobs_t *j, const char *id) {
    for (size_t i = 0; i < j->v.n; i++)
        if (str_eq(j->v.v[i]->id, id)) return j->v.v[i];
    return NULL;
}

static void cond_wait_ms(jobs_t *j, int64_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += ms * 1000000L;
    while (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&j->cv, &j->mu, &ts);
}

bool jobs_fg_wait(jobs_t *j, const char *id, int64_t ms, volatile sig_atomic_t *cancel) {
    int64_t deadline = mono_ms() + ms;
    pthread_mutex_lock(&j->mu);
    for (;;) {
        bgjob_t *x = find_locked(j, id);
        if (!x || x->done) {
            pthread_mutex_unlock(&j->mu);
            return true;
        }
        if (cancel && *cancel) x->cancel = 1;
        int64_t left = deadline - mono_ms();
        if (left <= 0) {
            pthread_mutex_unlock(&j->mu);
            return false;
        }
        cond_wait_ms(j, MIN(left, 100));
    }
}

bool jobs_fg_take(jobs_t *j, const char *id, proc_result_t *out, char **notice) {
    memset(out, 0, sizeof *out);
    if (notice) *notice = NULL;
    pthread_mutex_lock(&j->mu);
    bgjob_t *x = find_locked(j, id);
    if (!x || !x->done || !x->bash) {
        pthread_mutex_unlock(&j->mu);
        return false;
    }
    for (size_t i = 0; i < j->v.n; i++)
        if (j->v.v[i] == x) {
            memmove(j->v.v + i, j->v.v + i + 1, (j->v.n - i - 1) * sizeof(bgjob_t *));
            j->v.n--;
            break;
        }
    pthread_mutex_unlock(&j->mu);
    if (x->joinable) {
        pthread_join(x->th, NULL);
        x->joinable = false;
    }
    *out = x->bash->res;
    if (x->bash->rc != 0 && !out->spawn_errno) out->spawn_errno = EIO;
    memset(&x->bash->res, 0, sizeof x->bash->res);
    if (notice && x->bash->temp_shell)
        *notice = xstrdup("Ran in a temporary shell while the session shell is busy with another command: session variables and "
                          "functions were unavailable, and nothing set here persists.");
    job_free(x);
    return true;
}

char *jobs_fg_detach(jobs_t *j, const char *id) {
    pthread_mutex_lock(&j->mu);
    bgjob_t *x = find_locked(j, id);
    if (!x || x->done) {
        pthread_mutex_unlock(&j->mu);
        return NULL;
    }
    /* before the free: assign_id compares against every job's id, and a freed x->id could be handed straight back as
     * the candidate, which then always looks taken (an endless loop holding j->mu, freezing the UI) */
    char *new_id = assign_id(j, NULL);
    free(x->id);
    x->id = new_id;
    x->attached = false;
    char *nid = xstrdup(x->id);
    bashx_t *b = x->bash;
    pthread_mutex_unlock(&j->mu);
    if (b) {
        pthread_mutex_lock(&b->fwd_mu);
        b->fwd = NULL;
        b->fwd_ud = NULL;
        pthread_mutex_unlock(&b->fwd_mu);
    }
    /* it may have finished between the checks: then the notify was skipped (it was attached) */
    pthread_mutex_lock(&j->mu);
    bool fin = x->done;
    void (*fn)(void *) = j->notify;
    void *ud = j->notify_ud;
    pthread_mutex_unlock(&j->mu);
    if (fin && fn) fn(ud);
    return nid;
}

/* ---------- queries ---------- */

static size_t count_running(jobs_t *j, bool include_services) {
    size_t n = 0;
    for (size_t i = 0; i < j->v.n; i++) {
        bgjob_t *x = j->v.v[i];
        if (x->done || x->attached) continue;
        if (!include_services && str_eq(x->type, "service")) continue;
        n++;
    }
    return n;
}

size_t jobs_running(jobs_t *j) {
    if (!j) return 0;
    pthread_mutex_lock(&j->mu);
    size_t n = count_running(j, false);
    pthread_mutex_unlock(&j->mu);
    return n;
}

size_t jobs_running_all(jobs_t *j) {
    if (!j) return 0;
    pthread_mutex_lock(&j->mu);
    size_t n = count_running(j, true);
    pthread_mutex_unlock(&j->mu);
    return n;
}

static size_t count_ready(jobs_t *j) {
    size_t n = 0;
    for (size_t i = 0; i < j->v.n; i++) n += j->v.v[i]->done && !j->v.v[i]->delivered && !j->v.v[i]->attached;
    return n;
}

size_t jobs_undelivered(jobs_t *j) {
    if (!j) return 0;
    pthread_mutex_lock(&j->mu);
    size_t n = count_ready(j);
    pthread_mutex_unlock(&j->mu);
    return n;
}

static const char *job_status(const bgjob_t *x);

json_t *jobs_take_finished(jobs_t *j) {
    json_t *arr = json_arr();
    if (!j) return arr;
    pthread_mutex_lock(&j->mu);
    int64_t now = now_ms();
    for (size_t i = 0; i < j->v.n; i++) {
        bgjob_t *x = j->v.v[i];
        if (!x->done || x->delivered || x->attached) continue;
        x->delivered = true;
        x->delivered_at = now;
        json_t *o = json_obj();
        json_set_str(o, "jobId", x->id);
        json_set_str(o, "label", x->label);
        json_set_str(o, "type", x->type);
        json_set_str(o, "result", x->result ? x->result : "");
        json_set_bool(o, "failed", x->failed);
        json_set_str(o, "status", job_status(x));
        json_set_int(o, "durationMs", x->ended - x->started);
        if (x->agent_id) json_set_str(o, "agentId", x->agent_id);
        json_push(arr, o);
    }
    pthread_mutex_unlock(&j->mu);
    return arr;
}

int jobs_wait_any_ex(jobs_t *j, volatile sig_atomic_t *cancel, int64_t max_ms, bool include_services) {
    if (!j) return 0;
    int64_t deadline = mono_ms() + max_ms;
    pthread_mutex_lock(&j->mu);
    for (;;) {
        size_t ready = count_ready(j), running = count_running(j, include_services);
        if (ready || !running || (cancel && *cancel) || mono_ms() >= deadline) {
            pthread_mutex_unlock(&j->mu);
            return (int)ready;
        }
        cond_wait_ms(j, MIN(200, MAX(1, deadline - mono_ms())));
    }
}

int jobs_wait_any(jobs_t *j, volatile sig_atomic_t *cancel, int64_t max_ms) { return jobs_wait_any_ex(j, cancel, max_ms, false); }

bool jobs_cancel(jobs_t *j, const char *id) {
    bool found = false;
    pthread_mutex_lock(&j->mu);
    for (size_t i = 0; i < j->v.n; i++) {
        bgjob_t *x = j->v.v[i];
        if (x->done || x->attached || (x->hidden && !id)) continue;
        if (!id || str_eq(x->id, id)) {
            x->cancel = 1;
            x->killed = true;
            found = true;
        }
    }
    pthread_mutex_unlock(&j->mu);
    return found;
}

static const char *job_status(const bgjob_t *x) {
    return !x->done ? "running" : x->cancelled ? "cancelled" : x->failed ? "failed" : "completed";
}

static json_t *job_json(const bgjob_t *x, bool full) {
    json_t *o = json_obj();
    json_set_str(o, "id", x->id);
    json_set_str(o, "type", x->type);
    json_set_str(o, "label", x->label);
    json_set_str(o, "status", job_status(x));
    json_set_int(o, "durationMs", (x->done ? x->ended : now_ms()) - x->started);
    json_set_int(o, "startedAt", x->started);
    if (x->done) json_set_int(o, "endedAt", x->ended);
    if (x->agent_id) json_set_str(o, "agentId", x->agent_id);
    if (x->bash && x->bash->cmd) json_set_str(o, "command", x->bash->cmd);
    if (full) {
        if (x->done) json_set_str(o, "result", x->result ? x->result : "");
        if (!x->done && x->output.len) json_set_str(o, "output", sb_cstr((sb_t *)&x->output));
        json_set_bool(o, "delivered", x->delivered);
        if (x->bash && x->bash->streams) json_set_bool(o, "streams", true);
    }
    return o;
}

json_t *jobs_snapshot(jobs_t *j) {
    json_t *arr = json_arr();
    if (!j) return arr;
    prune(j);
    pthread_mutex_lock(&j->mu);
    for (size_t i = 0; i < j->v.n; i++) {
        bgjob_t *x = j->v.v[i];
        if (x->hidden || x->attached) continue;
        json_push(arr, job_json(x, false));
    }
    pthread_mutex_unlock(&j->mu);
    return arr;
}

json_t *jobs_get(jobs_t *j, const char *id) {
    if (!j || !id) return NULL;
    prune(j);
    json_t *o = NULL;
    pthread_mutex_lock(&j->mu);
    bgjob_t *x = find_locked(j, id);
    if (x && !x->hidden && !x->attached) o = job_json(x, true);
    pthread_mutex_unlock(&j->mu);
    return o;
}

static char *fmt_size(size_t n) {
    if (n < 1024) return xasprintf("%zuB", n);
    if (n < 1024 * 1024) return xasprintf("%.1fKB", (double)n / 1024);
    return xasprintf("%.1fMB", (double)n / (1024.0 * 1024));
}

static char *one_line(const char *s, size_t max) {
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = s; *p; p++) {
        if (isspace((unsigned char)*p)) {
            if (b.len && !sp) sb_putc(&b, ' ');
            sp = true;
        } else {
            sb_putc(&b, *p);
            sp = false;
        }
    }
    while (b.len && b.s[b.len - 1] == ' ') sb_truncate(&b, b.len - 1);
    if (utf8_strlen(sb_cstr(&b)) > max) {
        /* cut on a code point boundary */
        size_t cps = 0, i = 0;
        while (i < b.len && cps < max - 1) {
            uint32_t cp;
            i += (size_t)utf8_decode(b.s + i, b.len - i, &cp);
            cps++;
        }
        sb_truncate(&b, i);
        sb_append(&b, "…");
    }
    return sb_detach(&b);
}

static char *decode_stream(const char *s, size_t len) {
    /* CR progress becomes lines; control sequences are stripped */
    sb_t b = SB_INIT;
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
            sb_putc(&b, '\n');
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\t') continue;
        sb_putc(&b, (char)c);
    }
    char *r = utf8_sanitize(b.s ? b.s : "", b.len);
    sb_free(&b);
    return r;
}

char *jobs_streams(jobs_t *j, const char *id, int n, char **err) {
    *err = NULL;
    pthread_mutex_lock(&j->mu);
    bgjob_t *x = find_locked(j, id);
    if (!x || x->hidden || x->attached) {
        pthread_mutex_unlock(&j->mu);
        *err = xasprintf("Background job not found: %s", id);
        return NULL;
    }
    streams_t *s = x->bash ? x->bash->streams : NULL;
    if (!x->bash) {
        pthread_mutex_unlock(&j->mu);
        *err = xasprintf("proc://%s has no pipe streams: only bash jobs record them.", id);
        return NULL;
    }
    bool running = !x->done;
    char *r = NULL;
    if (n < 0) {
        sb_t b = SB_INIT;
        if (s) {
            pthread_mutex_lock(&s->mu);
            for (size_t i = 0; i < s->v.n; i++) {
                pstream_t *ps = &s->v.v[i];
                char *pr = one_line(ps->producer, 80), *co = one_line(ps->consumer, 80), *sz = fmt_size(ps->total);
                sb_appendf(&b, "%sproc://%s/streams/%zu %s %s — %s → %s", b.len ? "\n" : "", id, i, running ? "open" : "closed", sz, pr, co);
                free(pr);
                free(co);
                free(sz);
            }
            pthread_mutex_unlock(&s->mu);
        }
        if (!b.len) sb_appendf(&b, "%s has not piped anything yet.", id);
        r = sb_detach(&b);
    } else if (!s || (size_t)n >= s->v.n) {
        *err = xasprintf("Stream not found: proc://%s/streams/%d. List them with proc://%s/streams.", id, n, id);
    } else {
        pthread_mutex_lock(&s->mu);
        pstream_t *ps = &s->v.v[n];
        char *txt = decode_stream(ps->buf.s ? ps->buf.s : "", ps->buf.len);
        if (ps->start > 0) {
            char *sz = fmt_size(ps->start);
            r = xasprintf("[first %s no longer kept]\n%s", sz, txt);
            free(sz);
            free(txt);
        } else {
            r = txt;
        }
        pthread_mutex_unlock(&s->mu);
    }
    pthread_mutex_unlock(&j->mu);
    return r;
}

void jobs_set_quiet(jobs_t *j, const char *id) {
    pthread_mutex_lock(&j->mu);
    bgjob_t *x = find_locked(j, id);
    if (x) {
        x->quiet = true;
        if (x->done && !x->delivered) {
            x->delivered = true;
            x->delivered_at = now_ms();
        }
    }
    pthread_mutex_unlock(&j->mu);
}

/* ---------- services ownership ---------- */

void jobs_own_service(jobs_t *j, const char *name) {
    pthread_mutex_lock(&j->mu);
    if (!strvec_contains(&j->services, name)) strvec_push(&j->services, name);
    pthread_mutex_unlock(&j->mu);
}

bool jobs_owns_service(jobs_t *j, const char *name) {
    pthread_mutex_lock(&j->mu);
    bool r = strvec_contains(&j->services, name);
    pthread_mutex_unlock(&j->mu);
    return r;
}

void jobs_service_stopping(jobs_t *j, const char *name) {
    pthread_mutex_lock(&j->mu);
    for (size_t i = 0; i < j->v.n; i++) {
        bgjob_t *x = j->v.v[i];
        if (str_eq(x->type, "service") && !x->done && str_eq(x->agent_id, name)) x->quiet = true;
    }
    pthread_mutex_unlock(&j->mu);
}

/* services.c reads the owned names */
strvec_t *jobs_services_locked(jobs_t *j, pthread_mutex_t **mu) {
    *mu = &j->mu;
    return &j->services;
}
