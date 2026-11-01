/* write + bash tools */
#include "tools.h"
#include "jobs.h"
#include "xdev.h"
#include "services.h"
#include "vurl.h"
#include "coding/agents.h"
#include "prompt/template.h"

#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

/* ---------- write ---------- */

static int write_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    const char *path = json_get_str(args, "path");
    const char *content = json_get_str(args, "content");
    if (!path || !*path) {
        tool_result_error(out, "path is required");
        return 0;
    }
    if (vurl_is(path)) return vurl_write(env, path, content, out);
    if (!content) content = "";
    if (!strcmp(path, "xd://resolve") || !strcmp(path, "xd://reject")) {
        bool err = false;
        char *r = astedit_resolve(env, !strcmp(path, "xd://resolve"), content, &err);
        tool_result_text(out, r);
        out->is_error = err;
        free(r);
        return 0;
    }
    if (!strcmp(path, "xd://propose")) {
        if (!env->propose_plan) {
            tool_result_error(out, "xd://propose is only available in plan mode");
            return 0;
        }
        bool err = false;
        char *r = env->propose_plan(env, content, &err);
        tool_result_text(out, r ? r : "");
        out->is_error = err;
        free(r);
        return 0;
    }
    if (xdev_write(ctx, path, content, out)) return 0;
    if (str_starts(path, "ssh://")) {
        char *err = NULL;
        size_t n = strlen(content);
        if (ssh_store(env, path, content, n, &err) != 0) {
            tool_result_error(out, "%s", err);
        } else {
            size_t lines = n ? str_count(content, "\n") + (content[n - 1] != '\n') : 0;
            tool_result_textf(out, "Wrote %s (%zu lines, %zu bytes)", path, lines, n);
            out->details = json_obj();
            json_set_str(out->details, "path", path);
            json_set_int(out->details, "lines", (int64_t)lines);
        }
        free(err);
        return 0;
    }
    char *abs = tool_resolve_path(env, path);
    char *display = str_starts(path, "local://") ? xstrdup(path) : path_display(abs, env->cwd);
    struct stat st;
    bool existed = stat(abs, &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        tool_result_error(out, "Cannot write '%s': it is a directory", path);
    } else if (existed && !S_ISREG(st.st_mode)) {
        tool_result_error(out, "Cannot write '%s': not a regular file", path);
    } else {
        size_t n = strlen(content);
        int rc = fs_write_atomic(abs, content, n, 0644);
        if (rc != 0) {
            tool_result_error(out, "Failed to write %s: %s", display, strerror(-rc));
        } else {
            size_t lines = n ? str_count(content, "\n") + (content[n - 1] != '\n') : 0;
            const char *tag = edit_store_record(&env->store, abs, content, 0, 0);
            sb_t b = SB_INIT;
            sb_appendf(&b, "%s %s (%zu lines, %zu bytes)\n", existed ? "Overwrote" : "Created", display, lines, n);
            if (str_eq(env->edit_mode, "hashline")) format_hashline_header(&b, display, tag);
            if (env->after_write) {
                char *diag = env->after_write(env, abs);
                if (diag) {
                    sb_appendf(&b, "\n%s", diag);
                    free(diag);
                }
            }
            tool_result_text(out, sb_cstr(&b));
            sb_free(&b);
            out->details = json_obj();
            json_set_str(out->details, "path", abs);
            json_set_bool(out->details, "created", !existed);
            json_set_int(out->details, "lines", (int64_t)lines);
        }
    }
    free(abs);
    free(display);
    UNUSED(ctx);
    return 0;
}

agent_tool_t *tool_write_new(tool_env_t *env) {
    char *desc = tool_prompt("write", NULL);
    json_t *s = schema_lit("{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":"
                           "\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("write", "Write", desc, s, write_execute);
    t->exclusive = true;
    free(desc);
    t->ud = env;
    return t;
}

/* ---------- bash ---------- */

#define BASH_MAX_BYTES (50u * 1024)
#define BASH_MAX_LINES 2000

typedef struct {
    tool_ctx_t *ctx;
    sb_t buf;
    int64_t last_emit;
} bash_stream_t;

static void bash_on_output(void *ud, int stream, const char *data, size_t len) {
    bash_stream_t *s = ud;
    UNUSED(stream);
    sb_appendn(&s->buf, data, len);
    if (s->buf.len > 4 * BASH_MAX_BYTES) {
        /* keep only the tail for live preview */
        size_t keep = 2 * BASH_MAX_BYTES;
        memmove(s->buf.s, s->buf.s + s->buf.len - keep, keep);
        sb_truncate(&s->buf, keep);
    }
    int64_t now = mono_ms();
    if (s->ctx->update && now - s->last_emit > 100) {
        s->last_emit = now;
        bool tr;
        char *tail = tool_truncate_tail(s->buf.s, s->buf.len, BASH_MAX_BYTES, BASH_MAX_LINES, &tr);
        tool_result_t partial = {0};
        tool_result_text(&partial, tail);
        s->ctx->update(s->ctx, &partial);
        tool_result_free(&partial);
        free(tail);
    }
}

static char *spill_artifact(tool_env_t *env, const char *data, size_t len) {
    if (!env->artifacts_dir) {
        char *base = omc_agent_path("artifacts");
        env->artifacts_dir = base;
    }
    fs_mkdir_p(env->artifacts_dir, 0700);
    char *id = short_id();
    char *p = xasprintf("%s/%s.log", env->artifacts_dir, id);
    fs_write(p, data, len);
    free(p);
    return id;
}

/* leading `cd <dir> && rest` → (dir, rest) when cwd omitted */
static bool split_leading_cd(const char *cmd, char **dir, char **rest) {
    const char *p = cmd;
    while (isspace((unsigned char)*p)) p++;
    if (strncmp(p, "cd ", 3) != 0) return false;
    p += 3;
    while (*p == ' ') p++;
    const char *d = p;
    char q = 0;
    if (*p == '"' || *p == '\'') {
        q = *p++;
        d = p;
        while (*p && *p != q) p++;
        if (!*p) return false;
    } else {
        while (*p && !isspace((unsigned char)*p) && *p != '&' && *p != ';') p++;
    }
    size_t dl = (size_t)(p - d);
    if (q) p++;
    while (*p == ' ') p++;
    if (strncmp(p, "&&", 2) != 0) return false;
    p += 2;
    while (*p == ' ') p++;
    if (!*p || dl == 0) return false;
    *dir = xstrndup(d, dl);
    *rest = xstrdup(p);
    return true;
}

#define BASH_TIMEOUT_DESC "timeout in seconds; 0 disables the command deadline; nonzero values are clamped to 1-3600"

static char *background_notice(const char *jid, double tsec) {
    if (tsec > 0)
        return xasprintf("Backgrounded as job %s (killed once it has run %.0fs in total; `timeout: 0` disables the deadline); its "
                         "output is injected into the conversation as a follow-up the moment it finishes. Do NOT poll for it (no "
                         "`sleep`, `ps`, `pgrep`, `top`, `pidwait`, log tailing): every poll is a wasted turn. Do other work, or end "
                         "your reply and wait to be woken.",
                         jid, tsec);
    return xasprintf("Backgrounded as job %s (no deadline); its output is injected into the conversation as a follow-up the "
                     "moment it finishes. Do NOT poll for it (no `sleep`, `ps`, `pgrep`, `top`, `pidwait`, log tailing): every "
                     "poll is a wasted turn. Do other work, or end your reply and wait to be woken.",
                     jid);
}

static void background_result(tool_result_t *out, const char *jid, const char *preview, const strvec_t *notices, double tsec) {
    sb_t b = SB_INIT;
    char *pv = str_trim(preview ? preview : "");
    if (*pv) sb_appendf(&b, "%s\n\n", pv);
    free(pv);
    for (size_t i = 0; notices && i < notices->n; i++) sb_appendf(&b, "%s\n", notices->v[i]);
    if (notices && notices->n) sb_putc(&b, '\n');
    char *n = background_notice(jid, tsec);
    sb_append(&b, n);
    free(n);
    tool_result_text(out, sb_cstr(&b));
    sb_free(&b);
    out->details = json_obj();
    json_t *as = json_obj();
    json_set_str(as, "state", "running");
    json_set_str(as, "jobId", jid);
    json_set_str(as, "type", "bash");
    json_set(out->details, "async", as);
    if (tsec > 0) json_set_int(out->details, "timeoutSeconds", (int64_t)tsec);
    else json_set_bool(out->details, "timeoutDisabled", true);
}

/* completed foreground command -> tool result */
static void bash_finish(tool_env_t *env, tool_result_t *out, proc_result_t *r, double tsec, const char *cwd, int64_t dur,
                        const strvec_t *notices) {
    if (r->spawn_errno && !r->out_len) {
        tool_result_error(out, "Failed to spawn shell: %s", strerror(r->spawn_errno));
        return;
    }
    char *clean = utf8_sanitize(r->out ? r->out : "", r->out_len);
    size_t clen = strlen(clean);
    while (clen && (clean[clen - 1] == '\n')) clean[--clen] = 0;
    bool truncated;
    char *shown = tool_truncate_tail(clean, clen, BASH_MAX_BYTES, BASH_MAX_LINES, &truncated);
    sb_t b = SB_INIT;
    if (truncated) {
        char *id = spill_artifact(env, clean, clen);
        size_t total_lines = str_count(clean, "\n") + 1, shown_lines = str_count(shown, "\n") + 1;
        sb_appendf(&b, "[Showing last %zu of %zu lines. Full output: artifact://%s]\n", shown_lines, total_lines, id);
        free(id);
    }
    sb_append(&b, *shown ? shown : "(no output)");
    if (r->timed_out) sb_appendf(&b, "\n\nCommand timed out after %.0f seconds", tsec);
    else if (r->cancelled) sb_append(&b, "\n\nCommand aborted");
    else if (r->exit_code != 0) {
        if (r->term_signal) sb_appendf(&b, "\n\nCommand terminated by signal %d", r->term_signal);
        else sb_appendf(&b, "\n\nCommand exited with code %d", r->exit_code);
    }
    for (size_t i = 0; notices && i < notices->n; i++) sb_appendf(&b, "\n\n%s", notices->v[i]);
    tool_result_text(out, sb_cstr(&b));
    out->is_error = r->timed_out || r->cancelled || r->exit_code != 0;
    out->details = json_obj();
    json_set_int(out->details, "exitCode", r->exit_code);
    json_set_int(out->details, "durationMs", dur);
    /* omp BashToolDetails: wall time + effective timeout for the card footer */
    json_set_int(out->details, "wallTimeMs", dur);
    if (tsec > 0) json_set_int(out->details, "timeoutSeconds", (int64_t)tsec);
    else json_set_bool(out->details, "timeoutDisabled", true);
    if (r->timed_out) json_set_bool(out->details, "timedOut", true);
    json_set_str(out->details, "cwd", cwd);
    if (truncated) json_set_bool(out->details, "truncated", true);
    sb_free(&b);
    free(shown);
    free(clean);
}

static const char *blank_null(const char *s) {
    if (!s) return NULL;
    while (isspace((unsigned char)*s)) s++;
    return *s ? s : NULL;
}

static void bash_service(tool_env_t *env, tool_ctx_t *ctx, const char *name, const char *cmd, const char *cwd, const json_t *args,
                         strvec_t *notices, tool_result_t *out) {
    const json_t *rj = json_get(args, "ready");
    service_ready_t ready = {0};
    bool has_ready = false;
    if (json_is_obj(rj)) {
        ready.log = blank_null(json_get_str(rj, "log"));
        ready.host = blank_null(json_get_str(rj, "host"));
        ready.port = (int)json_get_num(rj, "port", 0);
        ready.timeout = json_get_num(rj, "timeout", 0);
        has_ready = ready.log || ready.host || json_get(rj, "port") || json_get(rj, "timeout");
    }
    bool pty = json_get_bool(args, "pty", true);
    json_t *snap = NULL;
    bool timed_out = false;
    char *err = NULL;
    if (services_start(env->jobs, env->cwd, name, cmd, cwd, pty, has_ready ? &ready : NULL, ctx ? ctx->cancel : NULL, &snap,
                       &timed_out, &err) != 0) {
        tool_result_error(out, "%s", err ? err : "service start failed");
        free(err);
        return;
    }
    sb_t b = SB_INIT;
    sb_appendf(&b, "%s: %s", json_get_str(snap, "name"), json_get_str(snap, "state"));
    if (json_get(snap, "pid")) sb_appendf(&b, " pid=%lld", (long long)json_get_int(snap, "pid", 0));
    if (json_get(snap, "readyAt")) sb_append(&b, " ready");
    if (timed_out)
        sb_appendf(&b, "\nNOT ready — readiness timed out after %gs; process state: %s.", ready.timeout > 0 ? ready.timeout : 30,
                   json_get_str(snap, "state"));
    for (size_t i = 0; i < notices->n; i++) sb_appendf(&b, "\n%s", notices->v[i]);
    char *log = services_logs(env->cwd, name);
    if (*log) sb_appendf(&b, "\n%s", log);
    free(log);
    tool_result_text(out, sb_cstr(&b));
    sb_free(&b);
    const char *st = json_get_str(snap, "state");
    out->details = json_obj();
    json_t *sv = json_obj();
    json_set_str(sv, "name", name);
    json_set_str(sv, "state", st);
    json_set_bool(sv, "ready", json_get(snap, "readyAt") || str_eq(st, "running"));
    json_set_bool(sv, "timedOut", timed_out);
    if (json_get(snap, "pid")) json_set_int(sv, "pid", json_get_int(snap, "pid", 0));
    json_set(out->details, "service", sv);
    out->is_error = str_eq(st, "failed");
    json_free(snap);
}

static bool steer_queued(tool_ctx_t *ctx) {
    return ctx && ctx->agent && ctx->agent->steering && json_len(ctx->agent->steering) > 0;
}

static int bash_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    const char *command = json_get_str(args, "command");
    if (!command || !*command) {
        tool_result_error(out, "command is required");
        return 0;
    }
    bool launch = settings_bool("launch.enabled", true);
    bool async_on = settings_bool("async.enabled", true);
    const char *name = blank_null(json_get_str(args, "name"));
    bool async_req = json_get_bool(args, "async", false);
    strvec_t notices = {0};
    if (name) {
        if (!launch) {
            tool_result_error(out, "Service launch is disabled in this session.");
            return 0;
        }
        if (json_get(args, "timeout")) strvec_push(&notices, "Ignored timeout: service-only; readiness follows ready.timeout.");
        if (async_req) strvec_push(&notices, "Ignored async: service-only; a service start is asynchronous already.");
    } else {
        const json_t *rj = json_get(args, "ready");
        if (json_is_obj(rj) && (blank_null(json_get_str(rj, "log")) || json_get_num(rj, "port", 0) != 0))
            strvec_push(&notices, "Ignored ready: service-only, and no service name was given.");
        if (async_req && !async_on) {
            tool_result_error(out, "Async bash execution is disabled. Enable async.enabled to use async mode.");
            strvec_free(&notices);
            return 0;
        }
    }
    char *cmd = xstrdup(command);
    const char *cwd_arg = json_get_str(args, "cwd");
    char *cwd = NULL;
    char *full = NULL;
    if (cwd_arg && *cwd_arg) {
        cwd = tool_resolve_path(env, cwd_arg);
    } else {
        char *d, *rest;
        if (split_leading_cd(command, &d, &rest)) {
            char *cand = tool_resolve_path(env, d);
            if (fs_is_dir(cand)) {
                cwd = cand;
                free(cmd);
                cmd = rest;
            } else {
                free(cand);
                free(rest);
            }
            free(d);
        }
    }
    if (!cwd) cwd = xstrdup(env->cwd);
    if (!fs_is_dir(cwd)) {
        tool_result_error(out, "Working directory does not exist: %s", cwd);
        goto done;
    }
    const char *prefix = settings_str("shellCommandPrefix", "");
    full = (prefix && *prefix) ? xasprintf("%s\n%s", prefix, cmd) : xstrdup(cmd);
    if (name) {
        bash_service(env, ctx, name, full, cwd, args, &notices, out);
        goto done;
    }
    double tsec = json_get_num(args, "timeout", 300);
    int64_t max_to = settings_int("tools.maxTimeout", 0);
    if (tsec > 0 && max_to > 0 && tsec > (double)max_to) tsec = (double)max_to;
    if (tsec > 0 && tsec < 1) tsec = 1;
    if (tsec > 3600) tsec = 3600;
    int64_t timeout_ms = tsec > 0 ? (int64_t)(tsec * 1000) : 0;
    bool pty = json_get_bool(args, "pty", false);
    if (async_req && env->jobs) {
        char *jid = jobs_start_bash(env->jobs, full, cwd, timeout_ms, NULL);
        background_result(out, jid, NULL, &notices, tsec);
        free(jid);
        goto done;
    }

    bash_stream_t bs = {.ctx = ctx};
    bool persistent = settings_bool("bash.persistent", true);
    if (!pty && persistent) {
        pthread_mutex_lock(&env->mu);
        if (!env->shell) env->shell = shell_new();
        pthread_mutex_unlock(&env->mu);
    }
    int64_t cap = MIN(100, settings_int("async.maxJobs", 100));
    bool auto_bg = !pty && env->jobs && async_on && settings_bool("bash.autoBackground.enabled", true) &&
                   (int64_t)jobs_running(env->jobs) < cap;
    int64_t t0 = mono_ms();
    proc_result_t r;
    if (auto_bg) {
        int64_t thr = settings_int("bash.autoBackground.thresholdMs", 60000);
        int64_t wait_ms = thr <= 0 ? 0 : timeout_ms ? MAX(0, MIN(thr, timeout_ms - 1000)) : thr;
        char *fid = jobs_start_bash_fg(env->jobs, full, cwd, timeout_ms, persistent ? env->shell : NULL, bash_on_output, &bs);
        bool fin = false, steered = false;
        int64_t deadline = mono_ms() + wait_ms;
        for (;;) {
            fin = jobs_fg_wait(env->jobs, fid, MIN(100, MAX(0, deadline - mono_ms())), ctx ? ctx->cancel : NULL);
            if (fin) break;
            if (ctx && ctx->cancel && *ctx->cancel) continue; /* the wait propagated the cancel; let it finish */
            if (mono_ms() >= deadline) break;
            if (steer_queued(ctx)) {
                steered = true;
                break;
            }
        }
        if (!fin) {
            char *jid = jobs_fg_detach(env->jobs, fid);
            if (jid) {
                if (steered) strvec_push(&notices, "Backgrounded early to handle an incoming message; the command keeps running.");
                bool tr;
                char *pv = bs.buf.len ? tool_truncate_tail(bs.buf.s, bs.buf.len, 4096, 20, &tr) : NULL;
                background_result(out, jid, pv, &notices, tsec);
                free(pv);
                free(jid);
                free(fid);
                sb_free(&bs.buf);
                goto done;
            }
            jobs_fg_wait(env->jobs, fid, 60000, NULL); /* finished right at the threshold */
        }
        char *notice = NULL;
        if (!jobs_fg_take(env->jobs, fid, &r, &notice)) {
            tool_result_error(out, "Failed to run command");
            free(fid);
            sb_free(&bs.buf);
            goto done;
        }
        if (notice) strvec_push_own(&notices, notice);
        free(fid);
    } else {
        static const char *env_extra[] = {"PAGER=cat", "GIT_PAGER=cat", "GIT_TERMINAL_PROMPT=0", "TERM=dumb",
                                          "NO_COLOR=1", "OMC=1", NULL};
        proc_opts_t o = {.cwd = cwd,
                         .env_extra = env_extra,
                         .timeout_ms = timeout_ms,
                         .max_output = 64u << 20,
                         .merge_stderr = true,
                         .new_session = true,
                         .use_pty = pty,
                         .cancel = ctx->cancel,
                         .on_output = bash_on_output,
                         .ud = &bs};
        int rc = (!pty && persistent) ? shell_run(env->shell, full, cwd, &o, &r) : proc_shell(full, &o, &r);
        if (rc != 0 && !r.spawn_errno) r.spawn_errno = errno ? errno : EIO;
    }
    sb_free(&bs.buf);
    bash_finish(env, out, &r, tsec, cwd, mono_ms() - t0, &notices);
    proc_result_free(&r);
done:
    strvec_free(&notices);
    free(full);
    free(cwd);
    free(cmd);
    return 0;
}

static bool python_available(void) {
    const char *cfg = settings_str("python.interpreter", NULL);
    if (cfg && *cfg) return true;
    char *p = which("python3");
    if (!p) p = which("python");
    bool ok = p != NULL;
    free(p);
    return ok;
}

char *bash_tool_description(bool has_eval) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "hasEval", has_eval);
    json_set_bool(ctx, "asyncEnabled", settings_bool("async.enabled", true));
    int64_t max_to = settings_int("tools.maxTimeout", 0);
    json_set_int(ctx, "defaultTimeoutSec", max_to > 0 && max_to < 300 ? max_to : 300);
    json_set_bool(ctx, "autoBackgroundEnabled", settings_bool("bash.autoBackground.enabled", true));
    int64_t thr = settings_int("bash.autoBackground.thresholdMs", 60000);
    char *secs = thr % 1000 ? xasprintf("%g", (double)thr / 1000.0) : xasprintf("%lld", (long long)(thr / 1000));
    json_set_str(ctx, "autoBackgroundSeconds", secs);
    free(secs);
    json_set_bool(ctx, "hasLaunch", settings_bool("launch.enabled", true));
    char *desc = tool_prompt("bash", ctx);
    json_free(ctx);
    return desc;
}

json_t *bash_tool_schema(void) {
    bool async_on = settings_bool("async.enabled", true), launch = settings_bool("launch.enabled", true);
    sb_t b = SB_INIT;
    sb_append(&b, "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
                  "\"timeout\":{\"type\":\"number\",\"description\":\"" BASH_TIMEOUT_DESC "\"},"
                  "\"cwd\":{\"type\":\"string\"},\"pty\":{\"type\":\"boolean\"}");
    if (async_on) sb_append(&b, ",\"async\":{\"type\":\"boolean\"}");
    if (launch)
        sb_append(&b, ",\"name\":{\"type\":\"string\",\"maxLength\":48},\"ready\":{\"type\":\"object\",\"properties\":{"
                      "\"log\":{\"type\":\"string\"},\"port\":{\"type\":\"number\"},\"host\":{\"type\":\"string\"},"
                      "\"timeout\":{\"type\":\"number\"}},\"additionalProperties\":false}");
    sb_append(&b, "},\"required\":[\"command\"],\"additionalProperties\":false}");
    json_t *s = json_parse_str(sb_cstr(&b));
    sb_free(&b);
    return s;
}

agent_tool_t *tool_bash_new(tool_env_t *env) {
    char *desc = bash_tool_description(python_available());
    agent_tool_t *t = agent_tool_new("bash", "Bash", desc, bash_tool_schema(), bash_execute);
    t->exclusive = true;
    free(desc);
    t->ud = env;
    return t;
}

/* ---------- wait (background jobs, services, peer replies) ---------- */

char *jobs_render_results(const json_t *jobs) {
    json_t *c = json_obj();
    json_set(c, "jobs", json_clone(jobs));
    json_set_bool(c, "multiple", json_len(jobs) > 1);
    char *s = prompt_render("coding-agent/prompts/tools/async-result.md", c);
    json_free(c);
    return s;
}

#define WAIT_MAX_MS (30 * 60 * 1000)

static const char *NOTHING_TO_WAIT =
    "Nothing to wait for: no background job or service you started is running, and no agent you messaged this run owes a "
    "reply. Other agents' results and messages arrive on their own.";

static int wait_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(args);
    out->details = json_obj();
    json_set_str(out->details, "op", "wait");
    char *msg = agents_take_message(env);
    if (msg) {
        tool_result_text(out, msg);
        free(msg);
        return 0;
    }
    jobs_t *jobs = env->jobs;
    size_t running = jobs ? jobs_running_all(jobs) : 0, ready = jobs ? jobs_undelivered(jobs) : 0;
    char *who = NULL;
    int replies = agents_reply_state(env, &who);
    if (!ready && !running && replies != 1) {
        if (replies == 2)
            tool_result_textf(out, "Nothing to wait for: %s finished its turn without replying. Ask again, or finish without the answer.", who);
        else {
            tool_result_text(out, NOTHING_TO_WAIT);
            out->useless = true;
        }
        free(who);
        json_set(out->details, "jobs", json_arr());
        return 0;
    }
    free(who);
    who = NULL;
    agents_wait_enter(env, true);
    int64_t deadline = mono_ms() + WAIT_MAX_MS;
    const char *wake = "timeout";
    for (;;) {
        msg = agents_take_message(env);
        if (msg) break;
        if (jobs && jobs_undelivered(jobs)) {
            wake = "job";
            break;
        }
        if (ctx && ctx->cancel && *ctx->cancel) {
            wake = "abort";
            break;
        }
        if (steer_queued(ctx)) {
            wake = "steer";
            break;
        }
        bool any = jobs && jobs_running_all(jobs);
        if (!any) {
            free(who);
            who = NULL;
            if (agents_reply_state(env, &who) != 1) {
                wake = "unanswerable";
                break;
            }
        }
        if (mono_ms() >= deadline) break;
        if (any) jobs_wait_any_ex(jobs, ctx ? ctx->cancel : NULL, 200, true);
        else sleep_ms(100);
    }
    agents_wait_enter(env, false);
    if (!msg) msg = agents_take_message(env);
    if (msg) {
        tool_result_text(out, msg);
        free(msg);
        free(who);
        return 0;
    }
    json_t *done = jobs ? jobs_take_finished(jobs) : json_arr();
    if (json_len(done)) {
        char *txt = jobs_render_results(done);
        tool_result_text(out, txt);
        free(txt);
    } else if (str_eq(wake, "abort")) {
        tool_result_text(out, "Wait interrupted.");
    } else if (str_eq(wake, "steer")) {
        tool_result_text(out, "Wait interrupted by message.");
        json_set_bool(out->details, "interrupted", true);
    } else if (str_eq(wake, "unanswerable")) {
        if (who) tool_result_textf(out, "%s finished its turn without replying. Ask again, or finish without the answer.", who);
        else tool_result_text(out, NOTHING_TO_WAIT);
    } else {
        json_t *snap = jobs ? jobs_snapshot(jobs) : json_arr(), *x;
        sb_t b = SB_INIT;
        sb_append(&b, "Wait limit reached; background work may still be running. Read proc:// for status.");
        json_arr_foreach(snap, i, x) if (str_eq(json_get_str(x, "status"), "running"))
            sb_appendf(&b, "\n- %s (%s, %llds)", json_get_str(x, "id"), json_get_str(x, "label"), (long long)(json_get_int(x, "durationMs", 0) / 1000));
        tool_result_text(out, sb_cstr(&b));
        sb_free(&b);
        json_free(snap);
        out->useless = true;
    }
    free(who);
    /* omp JobSnapshot rows: {id, type, status, label, durationMs, resultText | errorText} */
    json_t *snaps = json_arr(), *x;
    json_arr_foreach(done, i, x) {
        json_t *s = json_obj();
        bool failed = json_get_bool(x, "failed", false);
        json_set_str(s, "id", json_get_str(x, "jobId") ? json_get_str(x, "jobId") : "");
        json_set_str(s, "type", json_get_str(x, "type") ? json_get_str(x, "type") : "bash");
        json_set_str(s, "status", failed ? "failed" : "completed");
        json_set_str(s, "label", json_get_str(x, "label") ? json_get_str(x, "label") : "");
        json_set_int(s, "durationMs", json_get_int(x, "durationMs", 0));
        json_set_str(s, failed ? "errorText" : "resultText", json_get_str(x, "result") ? json_get_str(x, "result") : "");
        json_push(snaps, s);
    }
    json_free(done);
    json_set(out->details, "jobs", snaps);
    return 0;
}

agent_tool_t *tool_wait_new(tool_env_t *env) {
    char *desc = tool_prompt("wait", NULL);
    json_t *s = json_parse_str("{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("wait", "Wait", desc, s, wait_execute);
    free(desc);
    t->intent_mode = INTENT_OPTIONAL; /* wait.ts: intent = "optional" */
    t->ud = env;
    return t;
}
