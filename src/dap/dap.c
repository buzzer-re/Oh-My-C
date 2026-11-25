/* dap.c - Debug Adapter Protocol client and the `debug` tool (single active session). */
#define _GNU_SOURCE
#include "dap.h"

#include "config/settings.h"
#include "core/embed.h"
#include "core/fs.h"
#include "core/proc.h"
#include "core/rpcframe.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

typedef struct {
    int64_t seq;
    json_t *resp;
    bool done;
} dpend_t;

typedef struct {
    char *adapter;
    child_t child;
    pthread_t reader;
    bool reader_started;
    volatile bool alive;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int64_t seq;
    VEC(dpend_t *) pending;
    json_t *caps;
    sb_t output;          /* program/adapter output since last read */
    bool initialized_ev;  /* "initialized" event received */
    bool stopped;
    bool terminated;
    char *close_error;    /* why the transport ended ("DAP connection closed: ..."), owned */
    int64_t thread_id;    /* last stopped thread */
    char *stop_reason;
    char *stop_text;
    int64_t stop_seq;     /* increments on every stopped/terminated/continued */
    json_t *bps;          /* file -> [ {line, condition, hitCondition} ] */
    json_t *fn_bps;       /* [ {name, condition} ] */
    json_t *instr_bps;
    json_t *data_bps;
    char *program;
    int id;               /* omp DebugSessionSnapshot id: debug-N */
} dsession_t;

struct dap_manager {
    char *cwd;
    json_t *adapters;
    dsession_t *s;
    pthread_mutex_t mu;
};

/* ---------- transport ---------- */

static int dsend(dsession_t *s, json_t *msg) {
    char *body = json_dump(msg, 0);
    char *hdr = xasprintf("Content-Length: %zu\r\n\r\n", strlen(body));
    pthread_mutex_lock(&s->mu);
    int rc = 0;
    const char *parts[2] = {hdr, body};
    for (int k = 0; k < 2 && !rc; k++) {
        size_t off = 0, len = strlen(parts[k]);
        while (off < len) {
            ssize_t w = write(s->child.in_fd, parts[k] + off, len - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                rc = -1;
                break;
            }
            off += (size_t)w;
        }
    }
    pthread_mutex_unlock(&s->mu);
    free(hdr);
    free(body);
    return rc;
}

static void on_event(dsession_t *s, json_t *msg) {
    const char *ev = json_get_str(msg, "event");
    json_t *body = json_get(msg, "body");
    pthread_mutex_lock(&s->mu);
    if (str_eq(ev, "output")) {
        const char *o = json_get_str(body, "output");
        const char *cat = json_get_str(body, "category");
        if (o && !str_eq(cat, "telemetry") && s->output.len < (4u << 20)) sb_append(&s->output, o);
    } else if (str_eq(ev, "initialized")) {
        s->initialized_ev = true;
    } else if (str_eq(ev, "stopped")) {
        s->stopped = true;
        s->thread_id = json_get_int(body, "threadId", s->thread_id);
        free(s->stop_reason);
        s->stop_reason = xstrdup(json_get_str(body, "reason") ? json_get_str(body, "reason") : "stopped");
        free(s->stop_text);
        s->stop_text = json_get_str(body, "text") ? xstrdup(json_get_str(body, "text")) : json_get_str(body, "description") ? xstrdup(json_get_str(body, "description")) : NULL;
        s->stop_seq++;
    } else if (str_eq(ev, "continued")) {
        s->stopped = false;
    } else if (str_eq(ev, "terminated") || str_eq(ev, "exited")) {
        if (str_eq(ev, "exited")) sb_appendf(&s->output, "[process exited with code %lld]\n", (long long)json_get_int(body, "exitCode", 0));
        s->terminated = true;
        s->stopped = false;
        s->stop_seq++;
    }
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

static void handle(dsession_t *s, json_t *msg) {
    const char *type = json_get_str(msg, "type");
    if (str_eq(type, "response")) {
        int64_t rs = json_get_int(msg, "request_seq", -1);
        pthread_mutex_lock(&s->mu);
        for (size_t i = 0; i < s->pending.n; i++)
            if (s->pending.v[i]->seq == rs) {
                s->pending.v[i]->resp = msg;
                s->pending.v[i]->done = true;
                msg = NULL;
                pthread_cond_broadcast(&s->cv);
                break;
            }
        pthread_mutex_unlock(&s->mu);
    } else if (str_eq(type, "event")) {
        on_event(s, msg);
    } else if (str_eq(type, "request")) {
        /* reverse requests (runInTerminal, startDebugging): decline politely */
        json_t *r = json_obj();
        json_set_str(r, "type", "response");
        json_set_int(r, "request_seq", json_get_int(msg, "seq", 0));
        json_set_str(r, "command", json_get_str(msg, "command"));
        json_set_bool(r, "success", false);
        json_set_str(r, "message", "not supported by omc");
        pthread_mutex_lock(&s->mu);
        json_set_int(r, "seq", ++s->seq);
        pthread_mutex_unlock(&s->mu);
        dsend(s, r);
        json_free(r);
    }
    json_free(msg);
}

static void *dreader(void *p) {
    dsession_t *s = p;
    sb_t buf = SB_INIT;
    char tmp[65536];
    while (s->alive) {
        struct pollfd pf = {s->child.out_fd, POLLIN, 0};
        if (poll(&pf, 1, 200) <= 0) continue;
        ssize_t n = read(s->child.out_fd, tmp, sizeof tmp);
        if (n <= 0) break;
        sb_appendn(&buf, tmp, (size_t)n);
        bool framing_failed = false;
        for (;;) {
            const char *body;
            size_t bl, used;
            char *ferr = NULL;
            rpcframe_rc_t fr = rpcframe_next(&buf, &body, &bl, &used, &ferr);
            if (fr == RPCFRAME_RESYNC) {
                LOGW("DAP framing resync: header block without Content-Length");
                continue;
            }
            if (fr == RPCFRAME_ERROR) {
                /* omp: the reader fails ("DAP connection closed: ..."), pending requests fail, the adapter is disposed */
                LOGW("DAP connection closed: %s", ferr);
                pthread_mutex_lock(&s->mu);
                free(s->close_error);
                s->close_error = xasprintf("DAP connection closed: %s", ferr);
                pthread_mutex_unlock(&s->mu);
                free(ferr);
                framing_failed = true;
                break;
            }
            if (fr != RPCFRAME_MSG) break;
            json_t *m = json_parse(body, bl, NULL);
            rpcframe_consume(&buf, used);
            if (m) handle(s, m);
        }
        if (framing_failed) break;
    }
    sb_free(&buf);
    pthread_mutex_lock(&s->mu);
    if (s->alive && !s->close_error) s->close_error = xasprintf("DAP connection closed: %s transport ended", s->adapter ? s->adapter : "");
    s->alive = false;
    s->terminated = true;
    s->stop_seq++;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

static json_t *drequest(dsession_t *s, const char *command, json_t *args, int64_t timeout_ms, char **err) {
    *err = NULL;
    if (!s->alive) {
        json_free(args);
        *err = xstrdup("debug adapter is not running");
        return NULL;
    }
    dpend_t *p = xcalloc(1, sizeof(*p));
    json_t *msg = json_obj();
    pthread_mutex_lock(&s->mu);
    p->seq = ++s->seq;
    vec_push(&s->pending, p);
    pthread_mutex_unlock(&s->mu);
    json_set_int(msg, "seq", p->seq);
    json_set_str(msg, "type", "request");
    json_set_str(msg, "command", command);
    if (args) json_set(msg, "arguments", args);
    dsend(s, msg);
    json_free(msg);
    json_t *body = NULL;
    pthread_mutex_lock(&s->mu);
    int64_t deadline = mono_ms() + timeout_ms;
    while (!p->done && s->alive && mono_ms() < deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&s->cv, &s->mu, &ts);
    }
    if (p->done) {
        if (json_get_bool(p->resp, "success", false)) {
            body = json_take(p->resp, "body");
            if (!body) body = json_obj();
        } else {
            const char *m = json_get_str(p->resp, "message");
            const char *fm = json_get_str(json_get(json_get(p->resp, "body"), "error"), "format");
            *err = xasprintf("%s failed: %s", command, fm ? fm : m ? m : "error");
        }
    } else if (!s->alive && s->close_error) {
        *err = xstrdup(s->close_error); /* failConnection: in-flight requests fail at once with the close reason */
    } else {
        *err = xasprintf("%s timed out", command);
    }
    for (size_t i = 0; i < s->pending.n; i++)
        if (s->pending.v[i] == p) {
            memmove(s->pending.v + i, s->pending.v + i + 1, (s->pending.n - i - 1) * sizeof(void *));
            s->pending.n--;
            break;
        }
    pthread_mutex_unlock(&s->mu);
    json_free(p->resp);
    free(p);
    return body;
}

static void session_end(dsession_t *s) {
    if (!s) return;
    if (s->alive) {
        char *err = NULL;
        json_t *a = json_obj();
        json_set_bool(a, "terminateDebuggee", true);
        json_free(drequest(s, "disconnect", a, 3000, &err));
        free(err);
    }
    s->alive = false;
    child_kill(&s->child, SIGTERM);
    child_close(&s->child);
    if (s->reader_started) pthread_join(s->reader, NULL);
    vec_free(&s->pending);
    json_free(s->caps);
    json_free(s->bps);
    json_free(s->fn_bps);
    json_free(s->instr_bps);
    json_free(s->data_bps);
    sb_free(&s->output);
    free(s->adapter);
    free(s->close_error);
    free(s->stop_reason);
    free(s->stop_text);
    free(s->program);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    free(s);
}

/* ---------- manager ---------- */

dap_manager_t *dap_manager_new(const char *cwd) {
    dap_manager_t *m = xcalloc(1, sizeof(*m));
    m->cwd = xstrdup(cwd);
    size_t n;
    const char *d = embed_get("assets/dap-adapters.json", &n);
    m->adapters = d ? json_parse(d, n, NULL) : json_obj();
    json_t *user = settings_get("debug.adapters");
    if (json_is_obj(user)) json_merge(m->adapters, user);
    pthread_mutex_init(&m->mu, NULL);
    return m;
}

void dap_manager_free(dap_manager_t *m) {
    if (!m) return;
    session_end(m->s);
    json_free(m->adapters);
    free(m->cwd);
    pthread_mutex_destroy(&m->mu);
    free(m);
}

static bool is_elf_or_macho(const char *path) {
    unsigned char h[4] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(h, 1, 4, f);
    fclose(f);
    if (n < 4) return false;
    return (h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') || (h[0] == 0xcf && h[1] == 0xfa) || (h[0] == 0xfe && h[1] == 0xed);
}

static const char *pick_adapter(dap_manager_t *m, const char *program, const char *requested, char **cmd_out) {
    const char *k;
    json_t *v;
    if (requested) {
        json_t *a = json_get(m->adapters, requested);
        if (!a) return NULL;
        char *bin = which(json_get_str(a, "command"));
        if (!bin) return NULL;
        *cmd_out = bin;
        return requested;
    }
    bool native = program && is_elf_or_macho(program);
    const char *ext = program ? path_ext(program) : "";
    static const char *native_order[] = {"lldb-dap", "gdb", "codelldb"};
    if (native) {
        for (size_t i = 0; i < ARRAY_LEN(native_order); i++) {
            json_t *a = json_get(m->adapters, native_order[i]);
            char *bin = a ? which(json_get_str(a, "command")) : NULL;
            if (bin) {
                *cmd_out = bin;
                return native_order[i];
            }
        }
    }
    json_obj_foreach(m->adapters, i, k, v) {
        json_t *ft = json_get_arr(v, "fileTypes"), *x;
        bool match = false;
        json_arr_foreach(ft, j, x) if (*ext && str_ieq(json_sv(x), ext)) match = true;
        if (!match) continue;
        char *bin = which(json_get_str(v, "command"));
        if (bin) {
            *cmd_out = bin;
            return k;
        }
    }
    return NULL;
}

/* wait for a stop/terminate after a resume-type request */
static void wait_stop(dsession_t *s, int64_t since, int64_t timeout_ms) {
    pthread_mutex_lock(&s->mu);
    int64_t deadline = mono_ms() + timeout_ms;
    while (s->stop_seq == since && s->alive && mono_ms() < deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&s->cv, &s->mu, &ts);
    }
    pthread_mutex_unlock(&s->mu);
}

static char *src_line(const char *path, int64_t line1) {
    size_t n;
    char *t = fs_read_limit(path, 8u << 20, &n, NULL);
    if (!t) return NULL;
    char *p = t;
    for (int64_t i = 1; i < line1 && p; i++) {
        p = strchr(p, '\n');
        if (p) p++;
    }
    char *r = p ? xstrndup(p, strcspn(p, "\n")) : NULL;
    free(t);
    return r;
}

/* report current state: stop reason + top frame + snippet + drained output */
static void report_state(dsession_t *s, const char *cwd, sb_t *b) {
    pthread_mutex_lock(&s->mu);
    bool term = s->terminated, stopped = s->stopped;
    char *reason = s->stop_reason ? xstrdup(s->stop_reason) : NULL;
    char *text = s->stop_text ? xstrdup(s->stop_text) : NULL;
    int64_t tid = s->thread_id;
    char *out = s->output.len ? xstrdup(s->output.s) : NULL;
    sb_clear(&s->output);
    pthread_mutex_unlock(&s->mu);
    if (term) sb_append(b, "Status: terminated\n");
    else if (stopped) {
        sb_appendf(b, "Status: stopped (%s%s%s), thread %lld\n", reason ? reason : "?", text ? ": " : "", text ? text : "", (long long)tid);
        char *err = NULL;
        json_t *a = json_obj();
        json_set_int(a, "threadId", tid);
        json_set_int(a, "levels", 1);
        json_t *st = drequest(s, "stackTrace", a, 10000, &err);
        json_t *fr = json_at(json_get_arr(st, "stackFrames"), 0);
        if (fr) {
            const char *path = json_get_str(json_get(fr, "source"), "path");
            int64_t line = json_get_int(fr, "line", 0);
            char *d = path ? path_display(path, cwd) : NULL;
            sb_appendf(b, "Frame: %s (id %lld) at %s:%lld\n", json_get_str(fr, "name"), (long long)json_get_int(fr, "id", 0),
                       d ? d : "?", (long long)line);
            if (json_get_str(fr, "instructionPointerReference"))
                sb_appendf(b, "Instruction pointer: %s\n", json_get_str(fr, "instructionPointerReference"));
            char *code = path && line ? src_line(path, line) : NULL;
            if (code) sb_appendf(b, "%lld:%s\n", (long long)line, code);
            free(code);
            free(d);
        }
        json_free(st);
        free(err);
    } else {
        sb_append(b, "Status: running\n");
    }
    if (out) sb_appendf(b, "Output:\n%s%s", out, out[strlen(out) - 1] == '\n' ? "" : "\n");
    free(out);
    free(reason);
    free(text);
}

static json_t *bp_source_args(const char *file, json_t *list) {
    json_t *a = json_obj(), *src = json_obj();
    json_set_str(src, "path", file);
    json_set(a, "source", src);
    json_t *arr = json_arr(), *x;
    json_arr_foreach(list, i, x) json_push(arr, json_clone(x));
    json_set(a, "breakpoints", arr);
    return a;
}

static void fmt_bps_result(sb_t *b, json_t *res) {
    json_t *bp;
    json_arr_foreach(json_get_arr(res, "breakpoints"), i, bp) {
        sb_appendf(b, "breakpoint %lld: %s", (long long)json_get_int(bp, "id", (int64_t)i),
                   json_get_bool(bp, "verified", false) ? "verified" : "pending");
        if (json_get(bp, "line")) sb_appendf(b, " line %lld", (long long)json_get_int(bp, "line", 0));
        if (json_get_str(bp, "message")) sb_appendf(b, " (%s)", json_get_str(bp, "message"));
        sb_putc(b, '\n');
    }
}

typedef struct {
    tool_env_t *env;
    dap_manager_t *m;
} dtool_t;

static int start_session(dap_manager_t *m, tool_env_t *env, const json_t *args, bool attach, sb_t *b, char **err) {
    const char *program = json_get_str(args, "program");
    char *prog_abs = program ? tool_resolve_path(env, program) : NULL;
    char *bin = NULL;
    const char *name = pick_adapter(m, prog_abs, json_get_str(args, "adapter"), &bin);
    if (!name) {
        *err = xasprintf("No installed debug adapter for %s", program ? program : "this target");
        free(prog_abs);
        return -1;
    }
    json_t *acfg = json_get(m->adapters, name);
    strvec_t argv = {0};
    strvec_push_own(&argv, bin);
    json_t *x;
    json_arr_foreach(json_get_arr(acfg, "args"), i, x) if (json_is_str(x)) strvec_push(&argv, json_sv(x));
    vec_push(&argv, NULL);
    dsession_t *s = xcalloc(1, sizeof(*s));
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->adapter = xstrdup(name);
    static int next_id;
    s->id = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
    s->bps = json_obj();
    s->fn_bps = json_arr();
    s->instr_bps = json_arr();
    s->data_bps = json_arr();
    s->program = prog_abs;
    const char *cwd_arg = json_get_str(args, "cwd");
    char *cwd = cwd_arg ? tool_resolve_path(env, cwd_arg) : xstrdup(env->cwd);
    int rc = child_spawn((const char *const *)argv.v, cwd, NULL, false, &s->child);
    argv.n--;
    strvec_free(&argv);
    if (rc != 0) {
        *err = xasprintf("failed to start %s: %s", name, strerror(errno));
        free(cwd);
        s->alive = false;
        session_end(s);
        return -1;
    }
    s->alive = true;
    if (pthread_create(&s->reader, NULL, dreader, s) == 0) s->reader_started = true;
    m->s = s;
    json_t *ia = json_parse_str("{\"clientID\":\"omc\",\"clientName\":\"omc\",\"linesStartAt1\":true,\"columnsStartAt1\":true,"
                                "\"pathFormat\":\"path\",\"supportsVariableType\":true,\"supportsMemoryReferences\":true,"
                                "\"supportsRunInTerminalRequest\":false}");
    json_set_str(ia, "adapterID", name);
    s->caps = drequest(s, "initialize", ia, 15000, err);
    if (!s->caps) {
        free(cwd);
        session_end(s);
        m->s = NULL;
        return -1;
    }
    json_t *req = json_clone(json_get(acfg, attach ? "attachDefaults" : "launchDefaults"));
    if (!json_is_obj(req)) {
        json_free(req);
        req = json_obj();
    }
    if (attach) {
        if (json_get(args, "pid")) json_set_int(req, "pid", json_get_int(args, "pid", 0));
        if (json_get(args, "port")) json_set_int(req, "port", json_get_int(args, "port", 0));
        if (json_get_str(args, "host")) json_set_str(req, "host", json_get_str(args, "host"));
        if (prog_abs) json_set_str(req, "program", prog_abs);
    } else {
        if (prog_abs) json_set_str(req, "program", prog_abs);
        json_set(req, "args", json_clone(json_get_arr(args, "args") ? json_get_arr(args, "args") : json_arr()));
        json_set_str(req, "cwd", cwd);
    }
    json_t *extra = json_get_obj(args, "arguments");
    if (extra) json_merge(req, extra);
    const char *cmd = attach ? "attach" : "launch";
    /* send launch; many adapters reply only after configurationDone, so don't block on the response first */
    dpend_t *p = xcalloc(1, sizeof(*p));
    pthread_mutex_lock(&s->mu);
    p->seq = ++s->seq;
    vec_push(&s->pending, p);
    int64_t seq0 = s->stop_seq;
    pthread_mutex_unlock(&s->mu);
    json_t *msg = json_obj();
    json_set_int(msg, "seq", p->seq);
    json_set_str(msg, "type", "request");
    json_set_str(msg, "command", cmd);
    json_set(msg, "arguments", req);
    dsend(s, msg);
    json_free(msg);
    /* wait for initialized event (or the launch response) */
    int64_t deadline = mono_ms() + 15000;
    pthread_mutex_lock(&s->mu);
    while (!s->initialized_ev && !p->done && s->alive && mono_ms() < deadline) {
        pthread_mutex_unlock(&s->mu);
        sleep_ms(20);
        pthread_mutex_lock(&s->mu);
    }
    pthread_mutex_unlock(&s->mu);
    char *e2 = NULL;
    if (json_get_bool(s->caps, "supportsConfigurationDoneRequest", true)) json_free(drequest(s, "configurationDone", json_obj(), 10000, &e2));
    free(e2);
    /* now the launch response */
    pthread_mutex_lock(&s->mu);
    deadline = mono_ms() + 15000;
    while (!p->done && s->alive && mono_ms() < deadline) {
        pthread_mutex_unlock(&s->mu);
        sleep_ms(20);
        pthread_mutex_lock(&s->mu);
    }
    bool ok = p->done && json_get_bool(p->resp, "success", false);
    char *lerr = !ok ? xstrdup(p->done ? (json_get_str(p->resp, "message") ? json_get_str(p->resp, "message") : "failed") : "no response") : NULL;
    for (size_t i = 0; i < s->pending.n; i++)
        if (s->pending.v[i] == p) {
            memmove(s->pending.v + i, s->pending.v + i + 1, (s->pending.n - i - 1) * sizeof(void *));
            s->pending.n--;
            break;
        }
    pthread_mutex_unlock(&s->mu);
    json_free(p->resp);
    free(p);
    free(cwd);
    if (!ok) {
        *err = xasprintf("%s via %s failed: %s", cmd, name, lerr);
        free(lerr);
        session_end(s);
        m->s = NULL;
        return -1;
    }
    wait_stop(s, seq0, 5000);
    sb_appendf(b, "Adapter: %s\n", name);
    return 0;
}

/* omp DebugSessionSnapshot (transcript card's Session section) */
static json_t *snapshot_json(dsession_t *s, const char *cwd) {
    json_t *o = json_obj();
    char id[32];
    snprintf(id, sizeof id, "debug-%d", s->id);
    json_set_str(o, "id", id);
    json_set_str(o, "adapter", s->adapter ? s->adapter : "");
    pthread_mutex_lock(&s->mu);
    json_set_str(o, "status", s->terminated ? "terminated" : s->stopped ? "stopped" : "running");
    if (s->stopped && !s->terminated && s->stop_reason) json_set_str(o, "stopReason", s->stop_reason);
    pthread_mutex_unlock(&s->mu);
    json_set_str(o, "cwd", cwd ? cwd : "");
    if (s->program) json_set_str(o, "program", s->program);
    json_set_bool(o, "needsConfigurationDone", false);
    return o;
}

static int debug_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    dtool_t *dt = self->ud;
    dap_manager_t *m = dt->m;
    UNUSED(ctx);
    const char *action = json_get_str(args, "action");
    if (!action) {
        tool_result_error(out, "action is required");
        return 0;
    }
    int64_t to = (int64_t)(json_get_num(args, "timeout", 30) * 1000);
    pthread_mutex_lock(&m->mu);
    sb_t b = SB_INIT;
    char *err = NULL;
    dsession_t *s = m->s;
    if (!strcmp(action, "launch") || !strcmp(action, "attach")) {
        if (s) {
            session_end(s);
            m->s = NULL;
        }
        if (start_session(m, dt->env, args, !strcmp(action, "attach"), &b, &err) == 0) report_state(m->s, dt->env->cwd, &b);
        goto finish;
    }
    if (!strcmp(action, "sessions")) {
        if (s) sb_appendf(&b, "Active: %s (%s)%s\n", s->program ? s->program : "?", s->adapter, s->terminated ? " [terminated]" : "");
        else sb_append(&b, "No active debug session.\n");
        goto finish;
    }
    if (!s) {
        err = xstrdup("No active debug session; use action launch or attach first.");
        goto finish;
    }
    if (!strcmp(action, "terminate")) {
        report_state(s, dt->env->cwd, &b);
        session_end(s);
        m->s = NULL;
        sb_append(&b, "Session terminated.\n");
        goto finish;
    }
    if (!strcmp(action, "output")) {
        pthread_mutex_lock(&s->mu);
        sb_append(&b, s->output.len ? s->output.s : "(no new output)\n");
        sb_clear(&s->output);
        pthread_mutex_unlock(&s->mu);
        goto finish;
    }
    if (!strcmp(action, "set_breakpoint") || !strcmp(action, "remove_breakpoint")) {
        bool add = !strcmp(action, "set_breakpoint");
        const char *fn = json_get_str(args, "function");
        if (fn) {
            json_t *list = s->fn_bps;
            for (size_t i = json_len(list); i > 0; i--)
                if (str_eq(json_get_str(json_at(list, i - 1), "name"), fn)) json_free(json_arr_remove(list, i - 1));
            if (add) {
                json_t *bp = json_obj();
                json_set_str(bp, "name", fn);
                if (json_get_str(args, "condition")) json_set_str(bp, "condition", json_get_str(args, "condition"));
                json_push(list, bp);
            }
            json_t *a = json_obj();
            json_set(a, "breakpoints", json_clone(list));
            json_t *res = drequest(s, "setFunctionBreakpoints", a, to, &err);
            if (res) fmt_bps_result(&b, res);
            json_free(res);
        } else {
            const char *file = json_get_str(args, "file");
            int64_t line = json_get_int(args, "line", 0);
            if (!file || !line) {
                err = xstrdup("set_breakpoint needs file+line or function");
                goto finish;
            }
            char *abs = tool_resolve_path(dt->env, file);
            json_t *list = json_get_arr(s->bps, abs);
            if (!list) {
                list = json_arr();
                json_set(s->bps, abs, list);
            }
            for (size_t i = json_len(list); i > 0; i--)
                if (json_get_int(json_at(list, i - 1), "line", 0) == line) json_free(json_arr_remove(list, i - 1));
            if (add) {
                json_t *bp = json_obj();
                json_set_int(bp, "line", line);
                if (json_get_str(args, "condition")) json_set_str(bp, "condition", json_get_str(args, "condition"));
                if (json_get_str(args, "hit_condition")) json_set_str(bp, "hitCondition", json_get_str(args, "hit_condition"));
                json_push(list, bp);
            }
            json_t *res = drequest(s, "setBreakpoints", bp_source_args(abs, list), to, &err);
            if (res) fmt_bps_result(&b, res);
            if (res && !add) sb_appendf(&b, "Removed breakpoint at %s:%lld\n", file, (long long)line);
            json_free(res);
            free(abs);
        }
        goto finish;
    }
    if (!strcmp(action, "continue") || !strcmp(action, "step_over") || !strcmp(action, "step_in") || !strcmp(action, "step_out")) {
        const char *cmd = !strcmp(action, "continue") ? "continue" : !strcmp(action, "step_over") ? "next"
                          : !strcmp(action, "step_in") ? "stepIn" : "stepOut";
        pthread_mutex_lock(&s->mu);
        int64_t seq0 = s->stop_seq;
        s->stopped = false;
        pthread_mutex_unlock(&s->mu);
        json_t *a = json_obj();
        json_set_int(a, "threadId", s->thread_id);
        json_free(drequest(s, cmd, a, to, &err));
        if (!err) wait_stop(s, seq0, to);
        report_state(s, dt->env->cwd, &b);
        goto finish;
    }
    if (!strcmp(action, "pause")) {
        pthread_mutex_lock(&s->mu);
        int64_t seq0 = s->stop_seq;
        pthread_mutex_unlock(&s->mu);
        json_t *a = json_obj();
        json_set_int(a, "threadId", s->thread_id);
        json_free(drequest(s, "pause", a, to, &err));
        if (!err) wait_stop(s, seq0, 5000);
        report_state(s, dt->env->cwd, &b);
        goto finish;
    }
    /* request/response actions */
    const char *cmd = NULL;
    json_t *a = json_obj();
    if (!strcmp(action, "evaluate")) {
        cmd = "evaluate";
        json_set_str(a, "expression", json_get_str(args, "expression") ? json_get_str(args, "expression") : "");
        if (json_get(args, "frame_id")) json_set_int(a, "frameId", json_get_int(args, "frame_id", 0));
        else {
            /* default to the top frame of the stopped thread */
            char *e2 = NULL;
            json_t *sa = json_obj();
            json_set_int(sa, "threadId", s->thread_id);
            json_set_int(sa, "levels", 1);
            json_t *st = drequest(s, "stackTrace", sa, 5000, &e2);
            json_t *fr = json_at(json_get_arr(st, "stackFrames"), 0);
            if (fr) json_set_int(a, "frameId", json_get_int(fr, "id", 0));
            json_free(st);
            free(e2);
        }
        json_set_str(a, "context", json_get_str(args, "context") ? json_get_str(args, "context") : "repl");
    } else if (!strcmp(action, "stack_trace")) {
        cmd = "stackTrace";
        json_set_int(a, "threadId", s->thread_id);
        json_set_int(a, "levels", json_get_int(args, "levels", 20));
    } else if (!strcmp(action, "threads")) {
        cmd = "threads";
    } else if (!strcmp(action, "scopes")) {
        cmd = "scopes";
        json_set_int(a, "frameId", json_get_int(args, "frame_id", 0));
    } else if (!strcmp(action, "variables")) {
        cmd = "variables";
        json_set_int(a, "variablesReference", json_get_int(args, "variable_ref", json_get_int(args, "scope_id", 0)));
    } else if (!strcmp(action, "disassemble")) {
        cmd = "disassemble";
        json_set_str(a, "memoryReference", json_get_str(args, "memory_reference") ? json_get_str(args, "memory_reference") : "");
        json_set_int(a, "instructionCount", json_get_int(args, "instruction_count", 20));
        if (json_get(args, "instruction_offset")) json_set_int(a, "instructionOffset", json_get_int(args, "instruction_offset", 0));
        if (json_get(args, "offset")) json_set_int(a, "offset", json_get_int(args, "offset", 0));
        json_set_bool(a, "resolveSymbols", json_get_bool(args, "resolve_symbols", true));
    } else if (!strcmp(action, "read_memory")) {
        cmd = "readMemory";
        json_set_str(a, "memoryReference", json_get_str(args, "memory_reference") ? json_get_str(args, "memory_reference") : "");
        json_set_int(a, "count", json_get_int(args, "count", 64));
        if (json_get(args, "offset")) json_set_int(a, "offset", json_get_int(args, "offset", 0));
    } else if (!strcmp(action, "write_memory")) {
        cmd = "writeMemory";
        json_set_str(a, "memoryReference", json_get_str(args, "memory_reference") ? json_get_str(args, "memory_reference") : "");
        json_set_str(a, "data", json_get_str(args, "data") ? json_get_str(args, "data") : "");
        json_set_bool(a, "allowPartial", json_get_bool(args, "allow_partial", false));
        if (json_get(args, "offset")) json_set_int(a, "offset", json_get_int(args, "offset", 0));
    } else if (!strcmp(action, "modules")) {
        cmd = "modules";
        json_set_int(a, "startModule", json_get_int(args, "start_module", 0));
        json_set_int(a, "moduleCount", json_get_int(args, "module_count", 50));
    } else if (!strcmp(action, "loaded_sources")) {
        cmd = "loadedSources";
    } else if (!strcmp(action, "custom_request")) {
        cmd = json_get_str(args, "command");
        json_free(a);
        a = json_get_obj(args, "arguments") ? json_clone(json_get_obj(args, "arguments")) : json_obj();
    } else if (!strcmp(action, "set_instruction_breakpoint") || !strcmp(action, "remove_instruction_breakpoint")) {
        const char *ref = json_get_str(args, "instruction_reference");
        for (size_t i = json_len(s->instr_bps); i > 0; i--)
            if (str_eq(json_get_str(json_at(s->instr_bps, i - 1), "instructionReference"), ref)) json_free(json_arr_remove(s->instr_bps, i - 1));
        if (!strcmp(action, "set_instruction_breakpoint") && ref) {
            json_t *bp = json_obj();
            json_set_str(bp, "instructionReference", ref);
            if (json_get(args, "offset")) json_set_int(bp, "offset", json_get_int(args, "offset", 0));
            json_push(s->instr_bps, bp);
        }
        cmd = "setInstructionBreakpoints";
        json_set(a, "breakpoints", json_clone(s->instr_bps));
    } else if (!strcmp(action, "data_breakpoint_info")) {
        cmd = "dataBreakpointInfo";
        json_set_str(a, "name", json_get_str(args, "name") ? json_get_str(args, "name") : "");
        if (json_get(args, "variable_ref")) json_set_int(a, "variablesReference", json_get_int(args, "variable_ref", 0));
        if (json_get(args, "frame_id")) json_set_int(a, "frameId", json_get_int(args, "frame_id", 0));
    } else if (!strcmp(action, "set_data_breakpoint") || !strcmp(action, "remove_data_breakpoint")) {
        const char *id = json_get_str(args, "data_id");
        for (size_t i = json_len(s->data_bps); i > 0; i--)
            if (str_eq(json_get_str(json_at(s->data_bps, i - 1), "dataId"), id)) json_free(json_arr_remove(s->data_bps, i - 1));
        if (!strcmp(action, "set_data_breakpoint") && id) {
            json_t *bp = json_obj();
            json_set_str(bp, "dataId", id);
            json_set_str(bp, "accessType", json_get_str(args, "access_type") ? json_get_str(args, "access_type") : "write");
            if (json_get_str(args, "condition")) json_set_str(bp, "condition", json_get_str(args, "condition"));
            json_push(s->data_bps, bp);
        }
        cmd = "setDataBreakpoints";
        json_set(a, "breakpoints", json_clone(s->data_bps));
    }
    if (!cmd) {
        json_free(a);
        err = xasprintf("Unknown action: %s", action);
        goto finish;
    }
    json_t *res = drequest(s, cmd, a, to, &err);
    if (res) {
        if (!strcmp(action, "evaluate")) {
            sb_appendf(&b, "%s", json_get_str(res, "result") ? json_get_str(res, "result") : "");
            if (json_get_str(res, "type")) sb_appendf(&b, "  (%s)", json_get_str(res, "type"));
            if (json_get_int(res, "variablesReference", 0)) sb_appendf(&b, "  [variable_ref %lld]", (long long)json_get_int(res, "variablesReference", 0));
            sb_putc(&b, '\n');
        } else if (!strcmp(action, "stack_trace")) {
            json_t *fr;
            json_arr_foreach(json_get_arr(res, "stackFrames"), i, fr) {
                const char *path = json_get_str(json_get(fr, "source"), "path");
                char *d = path ? path_display(path, dt->env->cwd) : NULL;
                sb_appendf(&b, "#%zu %s (frame_id %lld) %s:%lld\n", i, json_get_str(fr, "name"), (long long)json_get_int(fr, "id", 0),
                           d ? d : "?", (long long)json_get_int(fr, "line", 0));
                free(d);
            }
        } else if (!strcmp(action, "threads")) {
            json_t *t;
            json_arr_foreach(json_get_arr(res, "threads"), i, t)
                sb_appendf(&b, "thread %lld: %s\n", (long long)json_get_int(t, "id", 0), json_get_str(t, "name"));
        } else if (!strcmp(action, "scopes")) {
            json_t *sc;
            json_arr_foreach(json_get_arr(res, "scopes"), i, sc)
                sb_appendf(&b, "%s (variable_ref %lld)\n", json_get_str(sc, "name"), (long long)json_get_int(sc, "variablesReference", 0));
        } else if (!strcmp(action, "variables")) {
            json_t *v;
            json_arr_foreach(json_get_arr(res, "variables"), i, v) {
                sb_appendf(&b, "%s = %s", json_get_str(v, "name"), json_get_str(v, "value") ? json_get_str(v, "value") : "");
                if (json_get_str(v, "type")) sb_appendf(&b, "  (%s)", json_get_str(v, "type"));
                if (json_get_int(v, "variablesReference", 0)) sb_appendf(&b, "  [variable_ref %lld]", (long long)json_get_int(v, "variablesReference", 0));
                sb_putc(&b, '\n');
            }
        } else if (!strcmp(action, "disassemble")) {
            json_t *ins;
            json_arr_foreach(json_get_arr(res, "instructions"), i, ins)
                sb_appendf(&b, "%s  %s\n", json_get_str(ins, "address"), json_get_str(ins, "instruction"));
        } else if (!strcmp(action, "set_instruction_breakpoint") || !strcmp(action, "set_data_breakpoint") ||
                   !strcmp(action, "remove_instruction_breakpoint") || !strcmp(action, "remove_data_breakpoint")) {
            fmt_bps_result(&b, res);
            if (!b.len) sb_append(&b, "ok\n");
        } else {
            char *d = json_dump(res, 2);
            sb_append(&b, d);
            free(d);
        }
        json_free(res);
    }
finish:
    out->details = json_obj();
    json_set_str(out->details, "action", action);
    json_set_bool(out->details, "success", !err);
    if (m->s) json_set(out->details, "snapshot", snapshot_json(m->s, dt->env->cwd));
    pthread_mutex_unlock(&m->mu);
    if (err) {
        if (b.len) tool_result_textf(out, "%s%s", sb_cstr(&b), err);
        else tool_result_text(out, err);
        out->is_error = true;
    } else {
        size_t len = b.len;
        while (len && b.s[len - 1] == '\n') len--;
        sb_truncate(&b, len);
        tool_result_text(out, b.len ? sb_cstr(&b) : "ok");
    }
    free(err);
    sb_free(&b);
    return 0;
}

static void dtool_destroy(agent_tool_t *t) { free(t->ud); }

agent_tool_t *tool_debug_new(tool_env_t *env, dap_manager_t *m) {
    char *desc = tool_prompt("debug", NULL);
    json_t *s = schema_lit(
        "{\"type\":\"object\",\"properties\":{\"action\":{\"enum\":[\"launch\",\"attach\",\"set_breakpoin"
        "t\",\"remove_breakpoint\",\"set_instruction_breakpoint\",\"remove_instruction_breakpoint\",\"dat"
        "a_breakpoint_info\",\"set_data_breakpoint\",\"remove_data_breakpoint\",\"continue\",\"step_over\",\"step_in\",\"step_out\",\"pause\",\"evaluate\",\"stack_trace\",\"threads\",\"scopes\",\"varia"
        "bles\",\"disassemble\",\"read_memory\",\"write_memory\",\"modules\",\"loaded_sources\",\"custom_"
        "request\",\"output\",\"terminate\",\"sessions\"],\"type\":\"string\"},\"program\":{\"type\":\"st"
        "ring\",\"description\":\"debug target path; Delve accepts Go package directories\"},\"args\":{\""
        "type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"program arguments\"},\"adapte"
        "r\":{\"type\":\"string\",\"description\":\"configured adapter id (gdb, lldb-dap, debugpy, dlv, r"
        "dbg, or dap.json entry)\"},\"cwd\":{\"type\":\"string\"},\"file\":{\"type\":\"string\",\"descrip"
        "tion\":\"source file\"},\"line\":{\"type\":\"number\",\"description\":\"source line\"},\"functio"
        "n\":{\"type\":\"string\",\"description\":\"function name\"},\"name\":{\"type\":\"string\",\"desc"
        "ription\":\"variable or data name\"},\"condition\":{\"type\":\"string\",\"description\":\"breakp"
        "oint condition\"},\"hit_condition\":{\"type\":\"string\"},\"expression\":{\"type\":\"string\",\""
        "description\":\"expression to evaluate\"},\"context\":{\"type\":\"string\",\"description\":\"eva"
        "luate context: watch | repl | hover | variables | clipboard\"},\"frame_id\":{\"type\":\"number\""
        "},\"scope_id\":{\"type\":\"number\",\"description\":\"scope variables reference\"},\"variable_re"
        "f\":{\"type\":\"number\",\"description\":\"variable reference\"},\"pid\":{\"type\":\"number\",\""
        "description\":\"process id for attach\"},\"port\":{\"type\":\"number\",\"description\":\"remote "
        "attach port\"},\"host\":{\"type\":\"string\",\"description\":\"remote attach host\"},\"levels\":"
        "{\"type\":\"number\",\"description\":\"max stack frames\"},\"memory_reference\":{\"type\":\"stri"
        "ng\",\"description\":\"memory reference or address\"},\"instruction_reference\":{\"type\":\"stri"
        "ng\"},\"instruction_count\":{\"type\":\"number\"},\"instruction_offset\":{\"type\":\"number\"},\"count\":{\"type\":\"number\",\"description\":\"bytes to read\"},\"data\":{\"type\":\"string\",\"description\":\"base64 memory payload\"},\"data_id\":{\"type\":\"string\",\"description\":\"dat"
        "a breakpoint id\"},\"access_type\":{\"enum\":[\"read\",\"write\",\"readWrite\"],\"type\":\"strin"
        "g\"},\"command\":{\"type\":\"string\",\"description\":\"custom dap request command\"},\"argument"
        "s\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":true,\"description\":\"custo"
        "m request arguments\"},\"offset\":{\"type\":\"number\"},\"resolve_symbols\":{\"type\":\"boolean\"},\"allow_partial\":{\"type\":\"boolean\"},\"start_module\":{\"type\":\"number\"},\"module_coun"
        "t\":{\"type\":\"number\"},\"timeout\":{\"type\":\"number\",\"description\":\"per-request timeout"
        " seconds\"}},\"required\":[\"action\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("debug", "Debug", desc, s, debug_execute);
    t->strict = TOOL_STRICT;
    t->examples = schema_lit(
        "[{\"caption\":\"Launch and inspect hang\",\"note\":\"1. debug(action: \\\"launch\\\", program: \\\"./my_app\\\")\\n2. debug(action: \\\"set_breakpoint\\\", file: \\\"src/main.c\\\", line: 42)\\n3. debug(action: \\\"continue\\\")\\n4. If the program appears hung: debug(action: \\\"pause\\"
        "\")\\n5. Inspect state with `threads`, `stack_trace`, `scopes`, and `variables`\"},{\"caption\":"
        "\"Launch a Python script with debugpy\",\"call\":{\"action\":\"launch\",\"adapter\":\"debugpy\","
        "\"program\":\"scripts/job.py\",\"args\":[\"--flag\"]}},{\"caption\":\"Raw debugger command throu"
        "gh repl\",\"call\":{\"action\":\"evaluate\",\"expression\":\"info registers\",\"context\":\"repl"
        "\"}}]");
    free(desc);
    dtool_t *dt = xcalloc(1, sizeof(*dt));
    dt->env = env;
    dt->m = m;
    t->ud = dt;
    t->destroy = dtool_destroy;
    t->exclusive = true;
    t->lenient_args = true;
    return t;
}
