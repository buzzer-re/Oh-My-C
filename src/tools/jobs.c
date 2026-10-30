/* jobs.c - async background jobs (port of omp's AsyncJobManager subset): `bash` with async:true runs in the
 * background; results are delivered to the agent as a follow-up (or collected by the `wait` tool). */
#include "jobs.h"

#include "core/proc.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>

typedef struct job {
    char *id;
    char *label;
    char *type;
    char *cmd, *cwd;
    int64_t timeout_ms;
    int64_t started, ended;
    char *result;
    int exit_code;
    bool done, delivered, failed;
    volatile sig_atomic_t cancel;
    pthread_t th;
    struct jobs *owner;
} job_t;

struct jobs {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    VEC(job_t *) v;
    uint64_t next;
    void (*notify)(void *ud);
    void *notify_ud;
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

void jobs_free(jobs_t *j) {
    if (!j) return;
    pthread_mutex_lock(&j->mu);
    j->notify = NULL;
    for (size_t i = 0; i < j->v.n; i++) j->v.v[i]->cancel = 1;
    pthread_mutex_unlock(&j->mu);
    for (size_t i = 0; i < j->v.n; i++) {
        job_t *x = j->v.v[i];
        pthread_join(x->th, NULL);
        free(x->id);
        free(x->label);
        free(x->type);
        free(x->cmd);
        free(x->cwd);
        free(x->result);
        free(x);
    }
    vec_free(&j->v);
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv);
    free(j);
}

static void *bash_job(void *p) {
    job_t *x = p;
    static const char *env_extra[] = {"PAGER=cat", "GIT_PAGER=cat", "GIT_TERMINAL_PROMPT=0", "TERM=dumb", "NO_COLOR=1", "OMC=1", NULL};
    proc_opts_t o = {.cwd = x->cwd,
                     .env_extra = env_extra,
                     .timeout_ms = x->timeout_ms,
                     .max_output = 4u << 20,
                     .merge_stderr = true,
                     .new_session = true,
                     .cancel = &x->cancel};
    proc_result_t r = {0};
    int rc = proc_shell(x->cmd, &o, &r);
    sb_t b = SB_INIT;
    const char *outp = r.out ? r.out : "";
    size_t n = r.out_len;
    if (n > 64 * 1024) {
        sb_appendf(&b, "[%zu bytes of earlier output elided]\n", n - 64 * 1024);
        outp += n - 64 * 1024;
    }
    sb_append(&b, outp);
    if (b.len && b.s[b.len - 1] != '\n') sb_putc(&b, '\n');
    if (rc != 0 || r.spawn_errno) sb_appendf(&b, "Failed to start: %s\n", strerror(r.spawn_errno ? r.spawn_errno : errno));
    else if (r.cancelled) sb_append(&b, "Command cancelled\n");
    else if (r.timed_out) sb_appendf(&b, "Command timed out after %llds\n", (long long)(x->timeout_ms / 1000));
    else if (r.exit_code != 0) sb_appendf(&b, "Command exited with code %d\n", r.exit_code);
    if (!r.out_len && !r.exit_code && !r.timed_out && !r.cancelled) sb_append(&b, "(no output)\n");
    jobs_t *j = x->owner;
    pthread_mutex_lock(&j->mu);
    x->result = sb_detach(&b);
    x->exit_code = r.exit_code;
    x->failed = rc != 0 || r.exit_code != 0 || r.timed_out || r.cancelled;
    x->ended = now_ms();
    x->done = true;
    void (*fn)(void *) = j->notify;
    void *ud = j->notify_ud;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
    proc_result_free(&r);
    if (fn) fn(ud);
    return NULL;
}

char *jobs_start_bash(jobs_t *j, const char *cmd, const char *cwd, int64_t timeout_ms, const char *label) {
    job_t *x = xcalloc(1, sizeof(*x));
    x->owner = j;
    x->type = xstrdup("bash");
    x->cmd = xstrdup(cmd);
    x->cwd = xstrdup(cwd);
    x->timeout_ms = timeout_ms;
    x->started = now_ms();
    char *l = label ? xstrdup(label) : xstrndup(cmd, strcspn(cmd, "\n"));
    if (strlen(l) > 60) strcpy(l + 57, "...");
    x->label = l;
    pthread_mutex_lock(&j->mu);
    x->id = xasprintf("bg_%llu", (unsigned long long)++j->next);
    vec_push(&j->v, x);
    char *id = xstrdup(x->id);
    if (pthread_create(&x->th, NULL, bash_job, x) != 0) {
        x->done = true;
        x->failed = true;
        x->result = xstrdup("Failed to start background job\n");
    }
    pthread_mutex_unlock(&j->mu);
    return id;
}

size_t jobs_running(jobs_t *j) {
    if (!j) return 0;
    size_t n = 0;
    pthread_mutex_lock(&j->mu);
    for (size_t i = 0; i < j->v.n; i++) n += !j->v.v[i]->done;
    pthread_mutex_unlock(&j->mu);
    return n;
}

size_t jobs_undelivered(jobs_t *j) {
    if (!j) return 0;
    size_t n = 0;
    pthread_mutex_lock(&j->mu);
    for (size_t i = 0; i < j->v.n; i++) n += j->v.v[i]->done && !j->v.v[i]->delivered;
    pthread_mutex_unlock(&j->mu);
    return n;
}

json_t *jobs_take_finished(jobs_t *j) {
    json_t *arr = json_arr();
    if (!j) return arr;
    pthread_mutex_lock(&j->mu);
    for (size_t i = 0; i < j->v.n; i++) {
        job_t *x = j->v.v[i];
        if (!x->done || x->delivered) continue;
        x->delivered = true;
        json_t *o = json_obj();
        json_set_str(o, "jobId", x->id);
        json_set_str(o, "label", x->label);
        json_set_str(o, "type", x->type);
        json_set_str(o, "result", x->result ? x->result : "");
        json_set_bool(o, "failed", x->failed);
        json_set_int(o, "durationMs", x->ended - x->started);
        json_push(arr, o);
    }
    pthread_mutex_unlock(&j->mu);
    return arr;
}

int jobs_wait_any(jobs_t *j, volatile sig_atomic_t *cancel, int64_t max_ms) {
    if (!j) return 0;
    int64_t deadline = mono_ms() + max_ms;
    pthread_mutex_lock(&j->mu);
    for (;;) {
        size_t running = 0, ready = 0;
        for (size_t i = 0; i < j->v.n; i++) {
            running += !j->v.v[i]->done;
            ready += j->v.v[i]->done && !j->v.v[i]->delivered;
        }
        if (ready || !running || (cancel && *cancel) || mono_ms() >= deadline) {
            pthread_mutex_unlock(&j->mu);
            return (int)ready;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&j->cv, &j->mu, &ts);
    }
}

bool jobs_cancel(jobs_t *j, const char *id) {
    bool found = false;
    pthread_mutex_lock(&j->mu);
    for (size_t i = 0; i < j->v.n; i++)
        if (!j->v.v[i]->done && (!id || !strcmp(j->v.v[i]->id, id))) {
            j->v.v[i]->cancel = 1;
            found = true;
        }
    pthread_mutex_unlock(&j->mu);
    return found;
}

json_t *jobs_snapshot(jobs_t *j) {
    json_t *arr = json_arr();
    if (!j) return arr;
    pthread_mutex_lock(&j->mu);
    int64_t now = now_ms();
    for (size_t i = 0; i < j->v.n; i++) {
        job_t *x = j->v.v[i];
        json_t *o = json_obj();
        json_set_str(o, "id", x->id);
        json_set_str(o, "type", x->type);
        json_set_str(o, "label", x->label);
        json_set_str(o, "status", !x->done ? "running" : x->failed ? "failed" : "completed");
        json_set_int(o, "durationMs", (x->done ? x->ended : now) - x->started);
        json_push(arr, o);
    }
    pthread_mutex_unlock(&j->mu);
    return arr;
}
