/* eval tool: persistent Python kernel (assets/eval_kernel.py) with a host bridge for tool calls and the
 * session helpers (completion/judge/judge_batch/budget/output; see coding/evalhost.h).
 *
 * Kernel <-> host channel: newline-delimited JSON over the kernel's private duplicates of its original stdin/stdout
 * pipes. At startup the kernel points fd 0 at /dev/null and fd 1 at fd 2 (the stderr pipe), so stray writes from C
 * extensions, os.system or subprocesses can never corrupt the framing; the host reads that pipe as raw cell output.
 *   kernel -> host  {type:"ready"} | {type:"call",id,name,args} | {type:"cancel",id} | {type:"result",id,stdout,error,images}
 *   host -> kernel  {type:"exec",id,code} | {type:"reply",id,ok,value|error}
 * A host reader thread owns the read side; every bridge call runs on its own worker thread so concurrent helper calls
 * (asyncio.gather over judge(), background threads) never block each other. Interrupts (timeout, abort) send SIGINT
 * and cancel the cell's in-flight calls; a kernel that does not answer within 5 s is killed. Waits on helpers
 * (wait(), judge(), drain(), tool.task/wait) pause the cell deadline, which restarts when the call returns. */
#define _GNU_SOURCE
#include "tools.h"
#include "artifacts.h"
#include "outsink.h"

#include "config/settings.h"
#include "core/embed.h"
#include "core/fs.h"
#include "core/proc.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define INTERRUPT_ESCALATION_MS 5000
#define STARTUP_TIMEOUT_MS 15000
#define RAW_CAP (1u << 20)
#define LINE_CAP ((size_t)256 << 20)

typedef struct bcall bcall_t;
typedef struct eval_state eval_state_t;

/* write side of the channel + in-flight bridge calls; shared (refcounted) with worker threads */
typedef struct {
    pthread_mutex_t mu; /* calls, pausing, refs */
    pthread_cond_t cv;
    pthread_mutex_t wmu; /* writes */
    int fd;
    bool closed;
    int refs;
    bcall_t *calls;
    int pausing;
} chan_t;

struct bcall {
    bcall_t *next;
    int64_t id, cell;
    char *name;
    json_t *args;
    volatile sig_atomic_t cancel;
    bool pauses;
    chan_t *ch;
    eval_state_t *st;
    char *tool_call_id;
};

typedef struct {
    child_t child;
    chan_t *ch;
    pthread_t reader;
    bool reader_started;
    volatile sig_atomic_t stop;
    sb_t rbuf; /* reader-private */
    /* guarded by ch->mu */
    int64_t cell_id;
    bool cell_active, ready, eof;
    json_t *result;
    sb_t raw;
    bool raw_truncated;
    eval_state_t *st;
} kernel_t;

struct eval_state {
    tool_env_t *env;
    agent_tool_t *self;
    pthread_mutex_t mu; /* one cell at a time */
    kernel_t *k;
    bool had_kernel;
    char *python;
    /* session context for bridged tool calls (set per cell, read by workers) */
    pthread_mutex_t cmu;
    agent_t *agent;
    void *app;
    char *tool_call_id;
    /* outstanding worker threads */
    int workers;
    pthread_cond_t wcv;
    json_t *tool_names;
};

static int64_t g_cell_serial;

/* ---------- channel ---------- */

static chan_t *chan_new(int fd) {
    chan_t *c = xcalloc(1, sizeof(*c));
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    pthread_mutex_init(&c->wmu, NULL);
    c->fd = fd;
    c->refs = 1;
    return c;
}

static chan_t *chan_ref(chan_t *c) {
    pthread_mutex_lock(&c->mu);
    c->refs++;
    pthread_mutex_unlock(&c->mu);
    return c;
}

static void chan_unref(chan_t *c) {
    pthread_mutex_lock(&c->mu);
    bool last = --c->refs == 0;
    pthread_mutex_unlock(&c->mu);
    if (!last) return;
    if (!c->closed && c->fd >= 0) close(c->fd);
    pthread_mutex_destroy(&c->mu);
    pthread_mutex_destroy(&c->wmu);
    pthread_cond_destroy(&c->cv);
    free(c);
}

static void chan_close(chan_t *c) {
    pthread_mutex_lock(&c->wmu);
    if (!c->closed) {
        c->closed = true;
        if (c->fd >= 0) close(c->fd);
        c->fd = -1;
    }
    pthread_mutex_unlock(&c->wmu);
}

static int chan_send(chan_t *c, const json_t *j) {
    char *s = json_dump(j, 0);
    size_t n = strlen(s);
    s = xrealloc(s, n + 2);
    s[n++] = '\n';
    int rc = 0;
    pthread_mutex_lock(&c->wmu);
    if (c->closed) rc = -1;
    for (size_t off = 0; !rc && off < n;) {
        ssize_t w = write(c->fd, s + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            rc = -1;
        } else {
            off += (size_t)w;
        }
    }
    pthread_mutex_unlock(&c->wmu);
    free(s);
    return rc;
}

/* ---------- bridge calls ---------- */

static bool call_pauses(const char *name, const json_t *args) {
    if (str_eq(name, "__wait__") || str_eq(name, "__judge__") || str_eq(name, "task") || str_eq(name, "wait")) return true;
    return str_eq(name, "__judge_batch__") && str_eq(json_get_str(args, "op"), "drain");
}

/* run one session tool for the kernel: {text, images?} or *err (tool error text) */
static json_t *run_tool(bcall_t *c, char **err) {
    eval_state_t *st = c->st;
    pthread_mutex_lock(&st->cmu);
    agent_t *ag = st->agent;
    void *app = st->app;
    pthread_mutex_unlock(&st->cmu);
    agent_tool_t *t = ag ? agent_find_tool(ag, c->name) : NULL;
    if (!t || t == st->self) {
        *err = xstrdup(t ? "eval cannot call itself" : "unknown tool");
        return NULL;
    }
    json_t *targs = json_is_obj(c->args) ? json_clone(c->args) : json_obj();
    tool_ctx_t sub = {.agent = ag, .tool_call_id = c->tool_call_id, .tool_name = c->name, .cancel = &c->cancel, .app = app};
    tool_result_t tr = {0};
    t->execute(t, &sub, targs, &tr);
    json_free(targs);
    char *txt = tool_result_joined_text(&tr);
    json_t *v = NULL;
    if (tr.is_error) {
        *err = txt && *txt ? txt : xstrdup("tool error");
        if (*err != txt) free(txt);
    } else {
        v = json_obj();
        json_set(v, "text", json_str_own(txt));
        json_t *imgs = json_arr(), *blk;
        json_arr_foreach(tr.content, i, blk) if (str_eq(json_get_str(blk, "type"), "image")) json_push(imgs, json_clone(blk));
        if (json_len(imgs)) json_set(v, "images", imgs);
        else json_free(imgs);
    }
    tool_result_free(&tr);
    return v;
}

static void *call_main(void *p) {
    bcall_t *c = p;
    eval_state_t *st = c->st;
    char *err = NULL;
    json_t *value = NULL;
    if (str_starts(c->name, "__") && str_ends(c->name, "__")) {
        if (st->env->eval_bridge) value = st->env->eval_bridge(st->env, c->name, c->args, c->cell, &c->cancel, &err);
        else err = xasprintf("%s is unavailable in this session", c->name);
    } else {
        value = run_tool(c, &err);
    }
    json_t *reply = json_obj();
    json_set_str(reply, "type", "reply");
    json_set_int(reply, "id", c->id);
    json_set_bool(reply, "ok", err == NULL);
    if (err) json_set_str(reply, "error", err);
    else {
        json_set(reply, "value", value ? value : json_null());
        value = NULL;
    }
    chan_send(c->ch, reply);
    json_free(reply);
    json_free(value);
    free(err);
    chan_t *ch = c->ch;
    pthread_mutex_lock(&ch->mu);
    for (bcall_t **pp = &ch->calls; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    if (c->pauses) ch->pausing--;
    pthread_cond_broadcast(&ch->cv);
    pthread_mutex_unlock(&ch->mu);
    chan_unref(ch);
    free(c->name);
    free(c->tool_call_id);
    json_free(c->args);
    free(c);
    pthread_mutex_lock(&st->cmu);
    st->workers--;
    pthread_cond_broadcast(&st->wcv);
    pthread_mutex_unlock(&st->cmu);
    return NULL;
}

static void spawn_call(kernel_t *k, json_t *m) {
    eval_state_t *st = k->st;
    bcall_t *c = xcalloc(1, sizeof(*c));
    c->id = json_get_int(m, "id", 0);
    c->name = xstrdup(json_get_str(m, "name") ? json_get_str(m, "name") : "");
    c->args = json_take(m, "args");
    if (!c->args) c->args = json_obj();
    c->st = st;
    c->pauses = call_pauses(c->name, c->args);
    c->ch = chan_ref(k->ch);
    pthread_mutex_lock(&st->cmu);
    c->tool_call_id = st->tool_call_id ? xstrdup(st->tool_call_id) : NULL;
    st->workers++;
    pthread_mutex_unlock(&st->cmu);
    pthread_mutex_lock(&k->ch->mu);
    c->cell = k->cell_id;
    c->next = k->ch->calls;
    k->ch->calls = c;
    if (c->pauses) k->ch->pausing++;
    pthread_cond_broadcast(&k->ch->cv);
    pthread_mutex_unlock(&k->ch->mu);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, call_main, c) != 0) call_main(c);
    pthread_attr_destroy(&at);
}

/* cancel in-flight calls (cell < 0: all) */
static void cancel_calls(chan_t *ch, int64_t cell) {
    pthread_mutex_lock(&ch->mu);
    for (bcall_t *c = ch->calls; c; c = c->next)
        if (cell < 0 || c->cell == cell) c->cancel = 1;
    pthread_mutex_unlock(&ch->mu);
}

/* ---------- kernel process ---------- */

static void handle_msg(kernel_t *k, json_t *m) {
    const char *type = json_get_str(m, "type");
    if (str_eq(type, "call")) {
        spawn_call(k, m);
    } else if (str_eq(type, "cancel")) {
        int64_t id = json_get_int(m, "id", -1);
        pthread_mutex_lock(&k->ch->mu);
        for (bcall_t *c = k->ch->calls; c; c = c->next)
            if (c->id == id) c->cancel = 1;
        pthread_mutex_unlock(&k->ch->mu);
    } else if (str_eq(type, "result")) {
        pthread_mutex_lock(&k->ch->mu);
        if (k->cell_active && json_get_int(m, "id", -1) == k->cell_id && !k->result) {
            k->result = m;
            m = NULL;
        }
        pthread_cond_broadcast(&k->ch->cv);
        pthread_mutex_unlock(&k->ch->mu);
    } else if (str_eq(type, "ready")) {
        pthread_mutex_lock(&k->ch->mu);
        k->ready = true;
        pthread_cond_broadcast(&k->ch->cv);
        pthread_mutex_unlock(&k->ch->mu);
    }
    json_free(m);
}

static void *reader_main(void *p) {
    kernel_t *k = p;
    int out_fd = k->child.out_fd, err_fd = k->child.err_fd;
    bool err_open = err_fd >= 0;
    char buf[65536];
    while (!k->stop) {
        struct pollfd pf[2] = {{out_fd, POLLIN, 0}, {err_fd, POLLIN, 0}};
        int n = poll(pf, err_open ? 2 : 1, 100);
        if (n <= 0) continue;
        if (err_open && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t r = read(err_fd, buf, sizeof buf);
            if (r <= 0) err_open = false;
            else {
                pthread_mutex_lock(&k->ch->mu);
                if (k->cell_active) {
                    if (k->raw.len + (size_t)r <= RAW_CAP) sb_appendn(&k->raw, buf, (size_t)r);
                    else k->raw_truncated = true;
                }
                pthread_mutex_unlock(&k->ch->mu);
            }
        }
        if (!(pf[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        ssize_t r = read(out_fd, buf, sizeof buf);
        if (r <= 0) break;
        sb_appendn(&k->rbuf, buf, (size_t)r);
        size_t start = 0;
        for (;;) {
            char *nl = memchr(k->rbuf.s + start, '\n', k->rbuf.len - start);
            if (!nl) break;
            size_t len = (size_t)(nl - (k->rbuf.s + start));
            json_t *m = json_parse(k->rbuf.s + start, len, NULL);
            start += len + 1;
            if (m) handle_msg(k, m);
        }
        if (start) {
            memmove(k->rbuf.s, k->rbuf.s + start, k->rbuf.len - start);
            sb_truncate(&k->rbuf, k->rbuf.len - start);
        }
        if (k->rbuf.len > LINE_CAP) break; /* runaway frame: treat as a dead kernel */
    }
    pthread_mutex_lock(&k->ch->mu);
    k->eof = true;
    pthread_cond_broadcast(&k->ch->cv);
    pthread_mutex_unlock(&k->ch->mu);
    return NULL;
}

static void cv_wait_ms(pthread_cond_t *cv, pthread_mutex_t *mu, int64_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(cv, mu, &ts);
}

static void kernel_stop(eval_state_t *st) {
    kernel_t *k = st->k;
    if (!k) return;
    st->k = NULL;
    child_kill(&k->child, SIGKILL);
    k->stop = 1;
    if (k->reader_started) pthread_join(k->reader, NULL);
    chan_close(k->ch);
    cancel_calls(k->ch, -1);
    k->child.in_fd = -1; /* owned (and closed) by the channel */
    child_close(&k->child);
    chan_unref(k->ch);
    sb_free(&k->rbuf);
    sb_free(&k->raw);
    json_free(k->result);
    free(k);
}

static char *find_python(void) {
    const char *cfg = settings_str("python.interpreter", NULL);
    if (cfg && fs_is_exec(cfg)) return xstrdup(cfg);
    const char *names[] = {"python3", "python"};
    for (size_t i = 0; i < ARRAY_LEN(names); i++) {
        char *p = which(names[i]);
        if (p) return p;
    }
    return NULL;
}

static int kernel_start(eval_state_t *st, char **err) {
    if (st->k) return 0;
    if (!st->python) st->python = find_python();
    if (!st->python) {
        *err = xstrdup("Python 3 is not installed (set python.interpreter)");
        return -1;
    }
    size_t n;
    const char *src = embed_get("assets/eval_kernel.py", &n);
    if (!src) {
        *err = xstrdup("eval kernel asset missing");
        return -1;
    }
    tool_env_t *env = st->env;
    char *code = xstrndup(src, n);
    char *local = env->local_dir ? xasprintf("OMC_EVAL_LOCAL_ROOT=%s", env->local_dir)
                                 : xasprintf("OMC_EVAL_LOCAL_ROOT=%s/.omc-local", env->cwd);
    const char *argv[] = {st->python, "-u", "-c", code, NULL};
    const char *env_extra[] = {"PYTHONUNBUFFERED=1", "MPLBACKEND=Agg", "PYTHONIOENCODING=utf-8", local, NULL};
    kernel_t *k = xcalloc(1, sizeof(*k));
    k->st = st;
    k->child.in_fd = k->child.out_fd = k->child.err_fd = -1;
    int rc = child_spawn(argv, env->cwd, env_extra, true, &k->child);
    free(code);
    free(local);
    if (rc != 0) {
        *err = xasprintf("failed to start %s: %s", st->python, strerror(errno));
        free(k);
        return -1;
    }
    k->ch = chan_new(k->child.in_fd);
    st->k = k;
    if (pthread_create(&k->reader, NULL, reader_main, k) != 0) {
        kernel_stop(st);
        *err = xstrdup("python kernel failed to start (no reader thread)");
        return -1;
    }
    k->reader_started = true;
    int64_t deadline = mono_ms() + STARTUP_TIMEOUT_MS;
    pthread_mutex_lock(&k->ch->mu);
    while (!k->ready && !k->eof && mono_ms() < deadline) cv_wait_ms(&k->ch->cv, &k->ch->mu, 100);
    bool ok = k->ready;
    pthread_mutex_unlock(&k->ch->mu);
    if (!ok) {
        kernel_stop(st);
        *err = xstrdup("python kernel failed to start");
        return -1;
    }
    st->had_kernel = true;
    return 0;
}

/* ---------- execute ---------- */

static void interrupt_cell(eval_state_t *st, kernel_t *k, int64_t cell) {
    if (k->child.pid > 0) kill(-k->child.pid, SIGINT);
    cancel_calls(k->ch, cell);
    if (st->env->eval_bridge) {
        json_t *a = json_obj();
        json_set_int(a, "cell", cell);
        char *err = NULL;
        json_free(st->env->eval_bridge(st->env, "__interrupt__", a, cell, NULL, &err));
        free(err);
        json_free(a);
    }
}

static int eval_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    eval_state_t *st = self->ud;
    const char *lang = json_get_str(args, "language");
    const char *code = json_get_str(args, "code");
    if (!code) {
        tool_result_error(out, "code is required");
        return 0;
    }
    if (lang && strcmp(lang, "py") != 0 && strcmp(lang, "python") != 0) {
        tool_result_error(out, "Only the Python runtime (language: \"py\") is available in omc");
        return 0;
    }
    double tsec = json_get_num(args, "timeout", 30);
    if (tsec > 0 && tsec < 1) tsec = 1;
    if (tsec > 3600) tsec = 3600;
    int64_t tms = tsec > 0 ? (int64_t)(tsec * 1000) : 0;
    pthread_mutex_lock(&st->mu);
    if (json_get_bool(args, "reset", false)) { /* an intentional reset is no surprise restart */
        kernel_stop(st);
        st->had_kernel = false;
    }
    if (st->k) { /* a kernel that died between cells (crash, kill, out-of-memory) is replaced on this call */
        pthread_mutex_lock(&st->k->ch->mu);
        bool dead = st->k->eof;
        pthread_mutex_unlock(&st->k->ch->mu);
        if (dead) kernel_stop(st);
    }
    bool restarted = !st->k && st->had_kernel;
    char *err = NULL;
    if (kernel_start(st, &err) != 0) {
        tool_result_error(out, "%s", err);
        free(err);
        pthread_mutex_unlock(&st->mu);
        return 0;
    }
    kernel_t *k = st->k;
    pthread_mutex_lock(&st->cmu);
    st->agent = ctx->agent;
    st->app = ctx->app;
    free(st->tool_call_id);
    st->tool_call_id = ctx->tool_call_id ? xstrdup(ctx->tool_call_id) : NULL;
    pthread_mutex_unlock(&st->cmu);
    int64_t cell = __atomic_add_fetch(&g_cell_serial, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&k->ch->mu);
    k->cell_id = cell;
    k->cell_active = true;
    json_free(k->result);
    k->result = NULL;
    sb_clear(&k->raw);
    k->raw_truncated = false;
    pthread_mutex_unlock(&k->ch->mu);
    json_t *req = json_obj();
    json_set_str(req, "type", "exec");
    json_set_int(req, "id", cell);
    json_set_str(req, "code", code);
    chan_send(k->ch, req);
    json_free(req);

    int64_t t0 = mono_ms();
    int64_t deadline = tms ? mono_ms() + tms : 0, escalate_at = 0;
    bool timed_out = false, aborted = false, paused = false;
    json_t *result = NULL;
    pthread_mutex_lock(&k->ch->mu);
    for (;;) {
        if (k->result || k->eof) break;
        int64_t now = mono_ms();
        if (!escalate_at) {
            /* helper waits pause the deadline; a fresh window starts once they return */
            if (k->ch->pausing > 0) paused = true;
            else if (paused) {
                paused = false;
                if (tms) deadline = now + tms;
            }
            bool cancel = ctx->cancel && *ctx->cancel;
            bool expired = deadline && !paused && now >= deadline;
            if (cancel || expired) {
                timed_out = expired && !cancel;
                aborted = cancel;
                escalate_at = now + INTERRUPT_ESCALATION_MS;
                pthread_mutex_unlock(&k->ch->mu);
                interrupt_cell(st, k, cell);
                pthread_mutex_lock(&k->ch->mu);
                continue;
            }
        } else if (now >= escalate_at) {
            break;
        }
        cv_wait_ms(&k->ch->cv, &k->ch->mu, 100);
    }
    result = k->result;
    k->result = NULL;
    k->cell_active = false;
    bool eof = k->eof;
    char *raw = k->raw.len ? xstrndup(k->raw.s, k->raw.len) : NULL;
    bool raw_trunc = k->raw_truncated;
    sb_clear(&k->raw);
    pthread_mutex_unlock(&k->ch->mu);

    bool killed = false;
    if (!result) {
        killed = true;
        kernel_stop(st);
    }
    pthread_mutex_unlock(&st->mu);

    /* omp: the cell runtime's own OutputSink (head/tail + column cap) shapes the cell output; a second sink
     * mirrors the raw stream into the call's `eval` artifact and supplies the truncation meta (summarizeFinal) */
    out_sink_opts_t so;
    out_sink_opts_from_settings(&so);
    out_sink_t *cellsink = out_sink_new(&so);
    char *art_id = NULL, *art_path = artifacts_alloc(st->env->artifacts, "eval", &art_id);
    so.artifact_path = art_path;
    so.artifact_id = art_id;
    out_sink_t *rawsink = out_sink_new(&so);
    free(art_path);
    free(art_id);
    sb_t stream = SB_INIT;
    const char *so_out = json_get_str(result, "stdout");
    if (so_out && *so_out) sb_append(&stream, so_out);
    if (raw) {
        if (stream.len && stream.s[stream.len - 1] != '\n') sb_putc(&stream, '\n');
        sb_append(&stream, raw);
        if (raw_trunc) sb_append(&stream, "\n[raw output truncated]");
    }
    const char *e = json_get_str(result, "error");
    bool failed = e && !timed_out && !aborted && result;
    if (failed || (e && timed_out)) { /* an interrupted cell's error stays in its output */
        if (stream.len && stream.s[stream.len - 1] != '\n') sb_putc(&stream, '\n');
        sb_append(&stream, e);
    }
    free(raw);
    out_sink_push(cellsink, stream.s ? stream.s : "", stream.len);
    out_sink_push(rawsink, stream.s ? stream.s : "", stream.len);
    sb_free(&stream);
    out_summary_t cs, rs;
    /* the timeout leads the cell's output as its sink notice (kernel-session-registry.ts timeoutAnnotation) */
    char *tnote = !timed_out ? NULL
                  : killed   ? xstrdup("eval cell timed out and the kernel was unresponsive to interrupt; the kernel has been killed and "
                                       "will be recreated on the next call.")
                             : xasprintf("eval cell timed out after %llds; kernel interrupted but remains running. Reset the kernel via "
                                         "{ reset: true } if state appears corrupted.", (long long)MAX(1, (tms + 500) / 1000));
    out_sink_dump(cellsink, tnote, &cs);
    free(tnote);
    out_sink_dump(rawsink, NULL, &rs);
    out_sink_free(cellsink);
    out_sink_free(rawsink);
    char *combined = str_trim(cs.output);
    size_t clen = strlen(combined);
    sb_t b = SB_INIT;
    /* the first cell to run on a replacement opens with this, once per replacement; it mirrors the `eval-state-context`
     * reminder wording so agents recognise it */
    if (restarted) sb_append(&b, "Kernel restarted: the previous Python kernel is dead; state is unavailable. Re-run needed setup: variables, imports and definitions from earlier cells are gone.\n");
    size_t nimg = json_len(json_get_arr(result, "images"));
    if (failed) {
        out->is_error = true;
        if (clen) sb_appendf(&b, "%s\n\nCommand exited with code 1", combined);
        else sb_append(&b, "Command exited with code 1");
    } else if (clen) {
        sb_append(&b, combined);
    } else if (!timed_out && !aborted && result && !restarted) {
        if (nimg) sb_appendf(&b, "(%zu image%s; no text output)", nimg, nimg == 1 ? "" : "s");
        else sb_append(&b, "(no output)");
    }
    if (timed_out) {
        out->is_error = true;
    } else if (aborted) {
        if (b.len && b.s[b.len - 1] != '\n') sb_putc(&b, '\n');
        sb_append(&b, killed ? "Cell aborted; kernel restarted (state lost)." : "Cell aborted (kernel interrupted; state kept).");
        out->is_error = true;
    } else if (!result) {
        if (b.len && b.s[b.len - 1] != '\n') sb_putc(&b, '\n');
        sb_append(&b, eof ? "Python kernel exited (state lost); re-run setup." : "Python kernel stopped responding (state lost).");
        out->is_error = true;
    }
    tool_result_text(out, sb_cstr(&b));
    /* omp EvalToolDetails: one cell for the transcript's code-cell card */
    out->details = json_obj();
    json_t *cells = json_arr(), *cj = json_obj();
    json_set_int(cj, "index", 0);
    if (json_get_str(args, "title")) json_set_str(cj, "title", json_get_str(args, "title"));
    json_set_str(cj, "code", code);
    json_set_str(cj, "language", "python");
    json_set_str(cj, "output", combined);
    json_set_str(cj, "status", out->is_error ? "error" : "complete");
    json_set_int(cj, "durationMs", mono_ms() - t0);
    json_push(cells, cj);
    json_set(out->details, "cells", cells);
    json_set_str(out->details, "language", "python");
    json_t *langs = json_arr();
    json_push_str(langs, "python");
    json_set(out->details, "languages", langs);
    if (out->is_error) json_set_bool(out->details, "isError", true);
    /* summarizeFinal: shown = the trimmed cell output; totals add what the raw capture lost */
    out_summary_t fin = rs;
    fin.output_lines = clen ? (int64_t)out_count_nl(combined, clen) + 1 : 0;
    fin.output_bytes = (int64_t)clen;
    fin.total_lines = fin.output_lines + MAX(0, rs.total_lines - rs.output_lines);
    fin.total_bytes = fin.output_bytes + MAX(0, rs.total_bytes - rs.output_bytes);
    fin.elided_bytes = fin.elided_lines = -1;
    json_t *meta = NULL;
    out_meta_from_summary(&meta, &fin, "tail", 1, 0);
    if (meta) json_set(out->details, "meta", meta);
    free(combined);
    out_summary_free(&cs);
    out_summary_free(&rs);
    json_t *img;
    json_arr_foreach(json_get_arr(result, "images"), i, img) tool_result_image(out, json_get_str(img, "data"), json_get_str(img, "mimeType"));
    sb_free(&b);
    json_free(result);
    return 0;
}

static void eval_destroy(agent_tool_t *t) {
    eval_state_t *st = t->ud;
    pthread_mutex_lock(&st->mu);
    kernel_stop(st);
    pthread_mutex_unlock(&st->mu);
    /* bridge workers reference the session: wait for the (cancelled) calls to return */
    pthread_mutex_lock(&st->cmu);
    int64_t until = mono_ms() + 30000;
    while (st->workers > 0 && mono_ms() < until) cv_wait_ms(&st->wcv, &st->cmu, 100);
    bool drained = st->workers == 0;
    pthread_mutex_unlock(&st->cmu);
    if (!drained) {
        LOGW("eval: %d bridge call(s) still running at shutdown", st->workers);
        return; /* leak the state rather than free it under a running worker */
    }
    free(st->python);
    free(st->tool_call_id);
    json_free(st->tool_names);
    pthread_mutex_destroy(&st->mu);
    pthread_mutex_destroy(&st->cmu);
    pthread_cond_destroy(&st->wcv);
    free(st);
}

/* ---------- description & doc topics ---------- */

static bool names_have(const json_t *names, const char *n) {
    json_t *x;
    json_arr_foreach(names, i, x) if (str_eq(json_sv(x), n)) return true;
    return false;
}

/* template context (tools/eval.ts evalTemplateContext): Python only (no JS runtime); `spawns` (agent()/workpool()),
 * `evalTools` (@tool / tool(fn)), `autoBackgroundEnabled` and the browser/archive preludes stay off because omc's
 * kernel does not offer them, so the description never advertises a helper that is missing. */
static json_t *template_ctx(const json_t *tool_names) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "py", true);
    json_set_bool(ctx, "js", false);
    json_set_bool(ctx, "evalTools", false);
    json_set_bool(ctx, "spawns", false);
    json_set_bool(ctx, "eagerDelegation", true);
    json_set_bool(ctx, "waitTool", tool_names ? names_have(tool_names, "wait") : true);
    json_set_bool(ctx, "autoBackgroundEnabled", false);
    json_set_bool(ctx, "autoProvision", true);
    json_set_int(ctx, "timeoutMin", 1);
    json_set_int(ctx, "timeoutMax", 3600);
    return ctx;
}

/* xd://eval/<topic> docs: judge (incl. completion) and helpers */
static json_t *topics_for(const json_t *tool_names) {
    json_t *ctx = template_ctx(tool_names);
    json_t *t = json_obj();
    json_set(t, "judge", json_str_own(tool_prompt("eval-judge", ctx)));
    json_set(t, "helpers", json_str_own(tool_prompt("eval-helpers", ctx)));
    json_free(ctx);
    return t;
}

static json_t *eval_doc_topics(agent_tool_t *self) {
    eval_state_t *st = self->ud;
    return topics_for(st->tool_names);
}

static char *eval_description(const json_t *tool_names) {
    json_t *ctx = template_ctx(tool_names);
    if (json_len(tool_names)) json_set(ctx, "toolNames", json_clone(tool_names));
    /* a session without `read` cannot open xd://eval/<topic>: inline every topic */
    if (tool_names && !names_have(tool_names, "read")) {
        json_t *topics = topics_for(tool_names);
        sb_t b = SB_INIT;
        const char *k;
        json_t *v;
        json_obj_foreach(topics, i, k, v) {
            if (i) sb_append(&b, "\n\n");
            sb_append(&b, json_sv(v));
        }
        json_set(ctx, "inlineTopics", json_str_own(sb_detach(&b)));
        json_free(topics);
    }
    char *desc = tool_prompt("eval", ctx);
    json_free(ctx);
    return desc;
}

void eval_set_tool_names(agent_tool_t *t, const json_t *names) {
    eval_state_t *st = t->ud;
    json_free(st->tool_names);
    st->tool_names = json_clone(names);
    char *d = eval_description(names);
    free(t->description);
    t->description = d;
}

agent_tool_t *tool_eval_new(tool_env_t *env) {
    char *py = find_python();
    if (!py) return NULL; /* runtime unavailable: tool not offered */
    free(py);
    char *desc = eval_description(NULL);
    json_t *s = schema_lit("{\"type\":\"object\",\"properties\":{\"language\":{\"const\":\"py\",\"description\":"
                           "\"\\\"py\\\": Python with IPython-style magics (not IPython)\"},\"code\":{\"type\":\"string\","
                           "\"description\":\"Code or standalone % command; top-level await works.\"},\"title\":{\"type\":"
                           "\"string\",\"description\":\"Short transcript label.\"},\"timeout\":{\"type\":\"number\","
                           "\"description\":\"Cell deadline in seconds; nonzero values clamp to 1–3600; 0 disables it.\"},"
                           "\"reset\":{\"type\":\"boolean\",\"description\":\"Wipe only this kernel.\"}},\"required\":["
                           "\"language\",\"code\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("eval", "Eval", desc, s, eval_execute);
    free(desc);
    t->intent_mode = INTENT_OMIT; /* eval.ts derives the intent from the cell (function intent) */
    t->examples = schema_lit("[{\"caption\":\"Load a script with spaces without echoing its source\",\"call\":{"
                             "\"language\":\"py\",\"code\":\"%load \\\"scripts/my setup.py\\\"\"}}]");
    eval_state_t *st = xcalloc(1, sizeof(*st));
    st->env = env;
    st->self = t;
    pthread_mutex_init(&st->mu, NULL);
    pthread_mutex_init(&st->cmu, NULL);
    pthread_cond_init(&st->wcv, NULL);
    t->ud = st;
    t->destroy = eval_destroy;
    t->doc_topics = eval_doc_topics;
    t->exclusive = true;
    return t;
}
