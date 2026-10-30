/* write + bash tools */
#include "tools.h"

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
    if (!content) content = "";
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
    json_t *s = schema_obj();
    schema_prop(s, "path", "string", "Path of the file to write", true);
    schema_prop(s, "content", "string", "Full file content", true);
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

static int bash_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    const char *command = json_get_str(args, "command");
    if (!command || !*command) {
        tool_result_error(out, "command is required");
        return 0;
    }
    char *cmd = xstrdup(command);
    const char *cwd_arg = json_get_str(args, "cwd");
    char *cwd = NULL;
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
        free(cwd);
        free(cmd);
        return 0;
    }
    double tsec = json_get_num(args, "timeout", 300);
    int64_t max_to = settings_int("tools.maxTimeout", 0);
    if (tsec > 0 && max_to > 0 && tsec > (double)max_to) tsec = (double)max_to;
    if (tsec > 0 && tsec < 1) tsec = 1;
    if (tsec > 3600) tsec = 3600;
    const char *prefix = settings_str("shellCommandPrefix", "");
    char *full = (prefix && *prefix) ? xasprintf("%s\n%s", prefix, cmd) : xstrdup(cmd);
    bool pty = json_get_bool(args, "pty", false);

    static const char *env_extra[] = {"PAGER=cat", "GIT_PAGER=cat", "GIT_TERMINAL_PROMPT=0", "TERM=dumb",
                                      "NO_COLOR=1", "OMC=1", NULL};
    bash_stream_t bs = {.ctx = ctx};
    proc_opts_t o = {.cwd = cwd,
                     .env_extra = env_extra,
                     .timeout_ms = tsec > 0 ? (int64_t)(tsec * 1000) : 0,
                     .max_output = 64u << 20,
                     .merge_stderr = true,
                     .new_session = true,
                     .use_pty = pty,
                     .cancel = ctx->cancel,
                     .on_output = bash_on_output,
                     .ud = &bs};
    proc_result_t r;
    int64_t t0 = mono_ms();
    int rc;
    if (!pty && settings_bool("bash.persistent", true)) {
        pthread_mutex_lock(&env->mu);
        if (!env->shell) env->shell = shell_new();
        pthread_mutex_unlock(&env->mu);
        rc = shell_run(env->shell, full, cwd, &o, &r);
    } else {
        rc = proc_shell(full, &o, &r);
    }
    int64_t dur = mono_ms() - t0;
    sb_free(&bs.buf);
    if (rc != 0) {
        tool_result_error(out, "Failed to spawn shell: %s", strerror(errno));
        goto done;
    }
    char *clean = utf8_sanitize(r.out, r.out_len);
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
    if (r.timed_out) sb_appendf(&b, "\n\nCommand timed out after %.0f seconds", tsec);
    else if (r.cancelled) sb_append(&b, "\n\nCommand aborted");
    else if (r.exit_code != 0) {
        if (r.term_signal) sb_appendf(&b, "\n\nCommand terminated by signal %d", r.term_signal);
        else sb_appendf(&b, "\n\nCommand exited with code %d", r.exit_code);
    }
    tool_result_text(out, sb_cstr(&b));
    out->is_error = r.timed_out || r.cancelled || r.exit_code != 0;
    out->details = json_obj();
    json_set_int(out->details, "exitCode", r.exit_code);
    json_set_int(out->details, "durationMs", dur);
    json_set_str(out->details, "cwd", cwd);
    if (truncated) json_set_bool(out->details, "truncated", true);
    sb_free(&b);
    free(shown);
    free(clean);
    proc_result_free(&r);
done:
    free(full);
    free(cwd);
    free(cmd);
    return 0;
}

agent_tool_t *tool_bash_new(tool_env_t *env) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "hasEval", false);
    json_set_bool(ctx, "asyncEnabled", false);
    json_set_int(ctx, "defaultTimeoutSec", 300);
    char *desc = tool_prompt("bash", ctx);
    json_free(ctx);
    json_t *s = schema_obj();
    schema_prop(s, "command", "string", "Shell command to execute", true);
    schema_prop(s, "timeout", "number", "Timeout in seconds (default 300, 0 disables)", false);
    schema_prop(s, "cwd", "string", "Working directory", false);
    schema_prop(s, "pty", "boolean", "Run in a pseudo-terminal", false);
    agent_tool_t *t = agent_tool_new("bash", "Bash", desc, s, bash_execute);
    t->exclusive = true;
    free(desc);
    t->ud = env;
    return t;
}
