/* eval tool: persistent Python kernel (assets/eval_kernel.py) with tool callbacks. */
#define _GNU_SOURCE
#include "tools.h"

#include "config/settings.h"
#include "core/embed.h"
#include "core/fs.h"
#include "core/proc.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

typedef struct {
    child_t child;
    bool alive;
    sb_t rbuf;
    int64_t next_id;
    char *python;
} kernel_t;

typedef struct {
    tool_env_t *env;
    kernel_t k;
    pthread_mutex_t mu;
} eval_state_t;

static void kernel_stop(kernel_t *k) {
    if (!k->alive) return;
    child_kill(&k->child, SIGKILL);
    child_close(&k->child);
    k->alive = false;
    sb_clear(&k->rbuf);
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

/* read one JSON line with timeout; returns owned json or NULL (timeout/eof sets *eof) */
static json_t *read_msg(kernel_t *k, int64_t deadline, volatile sig_atomic_t *cancel, bool *eof) {
    *eof = false;
    for (;;) {
        char *nl = k->rbuf.len ? memchr(k->rbuf.s, '\n', k->rbuf.len) : NULL;
        if (nl) {
            size_t n = (size_t)(nl - k->rbuf.s);
            json_t *j = json_parse(k->rbuf.s, n, NULL);
            memmove(k->rbuf.s, nl + 1, k->rbuf.len - n - 1);
            sb_truncate(&k->rbuf, k->rbuf.len - n - 1);
            if (j) return j;
            continue;
        }
        if (cancel && *cancel) return NULL;
        if (deadline && mono_ms() > deadline) return NULL;
        struct pollfd pf = {k->child.out_fd, POLLIN, 0};
        if (poll(&pf, 1, 100) <= 0) continue;
        char buf[65536];
        ssize_t r = read(k->child.out_fd, buf, sizeof buf);
        if (r <= 0) {
            *eof = true;
            return NULL;
        }
        sb_appendn(&k->rbuf, buf, (size_t)r);
    }
}

static int kernel_start(kernel_t *k, const char *cwd, char **err) {
    if (k->alive) return 0;
    if (!k->python) k->python = find_python();
    if (!k->python) {
        *err = xstrdup("Python 3 is not installed (set python.interpreter)");
        return -1;
    }
    size_t n;
    const char *src = embed_get("assets/eval_kernel.py", &n);
    if (!src) {
        *err = xstrdup("eval kernel asset missing");
        return -1;
    }
    char *code = xstrndup(src, n);
    const char *argv[] = {k->python, "-u", "-c", code, NULL};
    static const char *env_extra[] = {"PYTHONUNBUFFERED=1", "MPLBACKEND=Agg", "PYTHONIOENCODING=utf-8", NULL};
    int rc = child_spawn(argv, cwd, env_extra, false, &k->child);
    free(code);
    if (rc != 0) {
        *err = xasprintf("failed to start %s: %s", k->python, strerror(errno));
        return -1;
    }
    k->alive = true;
    bool eof;
    json_t *ready = read_msg(k, mono_ms() + 15000, NULL, &eof);
    bool ok = ready && str_eq(json_get_str(ready, "type"), "ready");
    json_free(ready);
    if (!ok) {
        kernel_stop(k);
        *err = xstrdup("python kernel failed to start");
        return -1;
    }
    return 0;
}

static int send_line(kernel_t *k, const json_t *j) {
    char *s = json_dump(j, 0);
    sb_t b = SB_INIT;
    sb_append(&b, s);
    sb_putc(&b, '\n');
    free(s);
    size_t off = 0;
    int rc = 0;
    while (off < b.len) {
        ssize_t w = write(k->child.in_fd, b.s + off, b.len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        off += (size_t)w;
    }
    sb_free(&b);
    return rc;
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
    pthread_mutex_lock(&st->mu);
    kernel_t *k = &st->k;
    if (json_get_bool(args, "reset", false)) kernel_stop(k);
    char *err = NULL;
    bool restarted = !k->alive;
    if (kernel_start(k, st->env->cwd, &err) != 0) {
        tool_result_error(out, "%s", err);
        free(err);
        pthread_mutex_unlock(&st->mu);
        return 0;
    }
    json_t *req = json_obj();
    json_set_str(req, "type", "exec");
    json_set_int(req, "id", ++k->next_id);
    json_set_str(req, "code", code);
    send_line(k, req);
    json_free(req);
    int64_t deadline = tsec > 0 ? mono_ms() + (int64_t)(tsec * 1000) : 0;
    json_t *result = NULL;
    bool eof = false;
    while (!result) {
        json_t *m = read_msg(k, deadline, ctx->cancel, &eof);
        if (!m) break;
        const char *type = json_get_str(m, "type");
        if (str_eq(type, "tool_call")) {
            /* bridge into the agent's tools */
            const char *name = json_get_str(m, "name");
            agent_tool_t *t = ctx->agent && name ? agent_find_tool(ctx->agent, name) : NULL;
            json_t *resp = json_obj();
            json_set_str(resp, "type", "tool_result");
            json_set(resp, "id", json_clone(json_get(m, "id")));
            json_t *r = json_obj();
            if (!t || t == self) {
                json_set_bool(r, "isError", true);
                json_set_str(r, "text", t ? "eval cannot call itself" : "unknown tool");
            } else {
                tool_result_t tr = {0};
                json_t *targs = json_clone(json_get(m, "args"));
                if (!json_is_obj(targs)) {
                    json_free(targs);
                    targs = json_obj();
                }
                tool_ctx_t sub = *ctx;
                sub.tool_name = name;
                sub.update = NULL;
                t->execute(t, &sub, targs, &tr);
                char *txt = tool_result_joined_text(&tr);
                json_set_str(r, "text", txt);
                json_set_bool(r, "isError", tr.is_error);
                free(txt);
                tool_result_free(&tr);
                json_free(targs);
            }
            json_set(resp, "result", r);
            send_line(k, resp);
            json_free(resp);
            json_free(m);
        } else if (str_eq(type, "result")) {
            result = m;
        } else {
            json_free(m);
        }
    }
    if (!result) {
        bool timed_out = deadline && mono_ms() > deadline;
        kernel_stop(k);
        tool_result_error(out, "%s", eof ? "Python kernel exited (state lost); re-run setup."
                                     : timed_out ? "Cell timed out; kernel restarted (state lost)."
                                                 : "Cell aborted; kernel restarted (state lost).");
        pthread_mutex_unlock(&st->mu);
        return 0;
    }
    pthread_mutex_unlock(&st->mu);
    sb_t b = SB_INIT;
    if (restarted && k->next_id > 1) sb_append(&b, "[kernel restarted: previous state was lost]\n");
    const char *so = json_get_str(result, "stdout");
    if (so && *so) sb_append(&b, so);
    const char *e = json_get_str(result, "error");
    if (e) {
        if (b.len && b.s[b.len - 1] != '\n') sb_putc(&b, '\n');
        sb_append(&b, e);
        out->is_error = true;
    }
    size_t len = b.len;
    while (len && b.s[len - 1] == '\n') len--;
    sb_truncate(&b, len);
    bool tr;
    char *shown = tool_truncate_tail(sb_cstr(&b), b.len, 50u * 1024, 2000, &tr);
    tool_result_text(out, *shown ? shown : "(no output)");
    free(shown);
    json_t *img;
    json_arr_foreach(json_get_arr(result, "images"), i, img) tool_result_image(out, json_get_str(img, "data"), json_get_str(img, "mimeType"));
    sb_free(&b);
    json_free(result);
    return 0;
}

static void eval_destroy(agent_tool_t *t) {
    eval_state_t *st = t->ud;
    kernel_stop(&st->k);
    sb_free(&st->k.rbuf);
    free(st->k.python);
    pthread_mutex_destroy(&st->mu);
    free(st);
}

/* description context (tools/eval.ts #descriptionOptions): Python only (no JS runtime); subagent `agent`/`workpool`
 * helpers, eval-defined tools and browser/archive preludes are not implemented, so their conditionals stay off */
static char *eval_description(const json_t *tool_names) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "py", true);
    json_set_bool(ctx, "js", false);
    json_set_int(ctx, "timeoutMin", 1);
    json_set_int(ctx, "timeoutMax", 3600);
    if (json_len(tool_names)) json_set(ctx, "toolNames", json_clone(tool_names));
    char *desc = tool_prompt("eval", ctx);
    json_free(ctx);
    return desc;
}

void eval_set_tool_names(agent_tool_t *t, const json_t *names) {
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
                           "\"description\":\"Cell deadline in seconds; nonzero values clamp to 1\u20133600; 0 disables it.\"},"
                           "\"reset\":{\"type\":\"boolean\",\"description\":\"Wipe only this kernel.\"}},\"required\":["
                           "\"language\",\"code\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("eval", "Eval", desc, s, eval_execute);
    free(desc);
    t->intent_mode = INTENT_OMIT; /* eval.ts derives the intent from the cell (function intent) */
    t->examples = schema_lit("[{\"caption\":\"Load a script with spaces without echoing its source\",\"call\":{"
                             "\"language\":\"py\",\"code\":\"%load \\\"scripts/my setup.py\\\"\"}}]");
    eval_state_t *st = xcalloc(1, sizeof(*st));
    st->env = env;
    st->k.child.in_fd = st->k.child.out_fd = st->k.child.err_fd = -1;
    pthread_mutex_init(&st->mu, NULL);
    t->ud = st;
    t->destroy = eval_destroy;
    t->exclusive = true;
    return t;
}
