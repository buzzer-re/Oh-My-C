/* vurl.c - proc://, agent:// and history:// for the read/write tools (see vurl.h). */
#include "vurl.h"

#include "coding/agents.h"
#include "config/settings.h"
#include "jobs.h"
#include "services.h"

#include <ctype.h>

bool vurl_is(const char *path) {
    return path && (str_starts(path, "proc://") || str_starts(path, "agent://") || str_starts(path, "history://"));
}

static char *fmt_dur(int64_t ms) {
    if (ms < 0) ms = 0;
    if (ms < 1000) return xasprintf("%lldms", (long long)ms);
    int64_t s = ms / 1000;
    if (s < 60) return xasprintf("%.1fs", (double)ms / 1000.0);
    if (s < 3600) return xasprintf("%lldm %llds", (long long)(s / 60), (long long)(s % 60));
    return xasprintf("%lldh %lldm", (long long)(s / 3600), (long long)((s % 3600) / 60));
}

/* split a trailing line selector (`:N`, `:N-M`, `:N-`, `:-N`, `:raw`) off a device URL */
static char *split_sel(const char *path, int64_t *lo, int64_t *hi) {
    *lo = *hi = 0;
    const char *scheme_end = strstr(path, "://");
    const char *c = strrchr(path, ':');
    if (!scheme_end || !c || c <= scheme_end + 2) return xstrdup(path);
    const char *s = c + 1;
    if (!strcmp(s, "raw")) return xstrndup(path, (size_t)(c - path));
    char *end;
    if (*s == '-' && isdigit((unsigned char)s[1])) {
        long long n = strtoll(s + 1, &end, 10);
        if (*end) return xstrdup(path);
        *lo = -n;
        return xstrndup(path, (size_t)(c - path));
    }
    if (!isdigit((unsigned char)*s)) return xstrdup(path);
    long long a = strtoll(s, &end, 10), b = a;
    if (*end == '-') {
        b = end[1] ? strtoll(end + 1, &end, 10) : INT64_MAX;
        if (*end) return xstrdup(path);
    } else if (*end == '+') {
        long long n = strtoll(end + 1, &end, 10);
        if (*end) return xstrdup(path);
        b = a + n - 1;
    } else if (*end) {
        return xstrdup(path);
    }
    *lo = a;
    *hi = b;
    return xstrndup(path, (size_t)(c - path));
}

static char *slice_lines(const char *text, int64_t lo, int64_t hi) {
    if (!lo && !hi) return xstrdup(text);
    strvec_t lines = str_split_lines(text, strlen(text));
    int64_t n = (int64_t)lines.n;
    int64_t a = lo, b = hi;
    if (lo < 0) {
        a = MAX(1, n + lo + 1);
        b = n;
    }
    if (b > n) b = n;
    sb_t out = SB_INIT;
    for (int64_t i = a; i >= 1 && i <= b; i++) sb_appendf(&out, "%s%s", out.len ? "\n" : "", lines.v[i - 1]);
    strvec_free(&lines);
    if (!out.len) {
        sb_free(&out);
        return xasprintf("[no lines in range %lld-%lld; %lld lines total]", (long long)a, (long long)b, (long long)n);
    }
    return sb_detach(&out);
}

static void emit_text(tool_result_t *out, const char *text, int64_t lo, int64_t hi) {
    char *t = slice_lines(text, lo, hi);
    tool_result_text(out, t);
    free(t);
}

/* ---------- proc:// ---------- */

typedef struct {
    char *id;
    char *action; /* "", "kill", "mode", "streams" */
    int stream;   /* -1 = list */
} proc_target_t;

static int proc_parse(const char *url, proc_target_t *t, char **err) {
    memset(t, 0, sizeof *t);
    t->stream = -1;
    const char *rest = url + strlen("proc://");
    const char *slash = strchr(rest, '/');
    t->id = slash ? xstrndup(rest, (size_t)(slash - rest)) : xstrdup(rest);
    const char *path = slash ? slash : "";
    if (!*path || !strcmp(path, "/")) t->action = xstrdup("");
    else if (!strcmp(path, "/kill")) t->action = xstrdup("kill");
    else if (!strcmp(path, "/mode")) t->action = xstrdup("mode");
    else if (str_starts(path, "/streams")) {
        const char *p = path + 8;
        if (*p == '/') p++;
        if (*p) {
            char *end;
            long n = strtol(p, &end, 10);
            if (!isdigit((unsigned char)*p) || (*end && strcmp(end, "/"))) goto bad;
            t->stream = (int)n;
        }
        t->action = xstrdup("streams");
    } else {
        goto bad;
    }
    if (strchr(t->id, '?') || strchr(t->id, '#') || !strcmp(t->id, ".") || !strcmp(t->id, "..")) goto bad;
    return 0;
bad:
    *err = xasprintf("Invalid proc:// URL: %s. Use proc://, proc://<id>, proc://<id>/streams[/<n>], proc://<id>/kill, or proc://<id>/mode.", url);
    free(t->id);
    free(t->action);
    t->id = t->action = NULL;
    return -1;
}

static void proc_target_free(proc_target_t *t) {
    free(t->id);
    free(t->action);
}

static bool launch_enabled(void) { return settings_bool("launch.enabled", true); }

/* a running job of agent `id` under a suffixed id (a woken agent's turn) */
static char *running_job_of_agent(jobs_t *j, const char *id) {
    json_t *snap = jobs_snapshot(j), *x;
    char *r = NULL;
    json_arr_foreach(snap, i, x) if (!r && str_eq(json_get_str(x, "status"), "running") && str_eq(json_get_str(x, "agentId"), id) &&
                                    !str_eq(json_get_str(x, "id"), id)) r = xstrdup(json_get_str(x, "id"));
    json_free(snap);
    return r;
}

static bool agent_has_job(jobs_t *j, const char *agent) {
    json_t *snap = jobs_snapshot(j), *x;
    bool r = false;
    json_arr_foreach(snap, i, x) if (str_eq(json_get_str(x, "agentId"), agent) && str_eq(json_get_str(x, "status"), "running")) r = true;
    json_free(snap);
    return r;
}

static void job_line(sb_t *b, const json_t *x) {
    bool running = str_eq(json_get_str(x, "status"), "running");
    char *d = fmt_dur(json_get_int(x, "durationMs", 0));
    char *label = xstrdup(json_get_str(x, "label") ? json_get_str(x, "label") : "");
    for (char *p = label; *p; p++)
        if (isspace((unsigned char)*p)) *p = ' ';
    sb_appendf(b, "%s%s [%s] %s %s %s — %s", b->len ? "\n" : "", json_get_str(x, "id"), json_get_str(x, "type"), json_get_str(x, "status"),
               running ? "up" : "in", d, label);
    free(d);
    free(label);
}

static int proc_read(tool_env_t *env, const char *url, tool_result_t *out) {
    int64_t lo, hi;
    char *u = split_sel(url, &lo, &hi);
    proc_target_t t;
    char *err = NULL;
    if (proc_parse(u, &t, &err) != 0) {
        tool_result_error(out, "%s", err);
        free(err);
        free(u);
        return 0;
    }
    jobs_t *jobs = env->jobs;
    const char *id = t.id;
    if (str_eq(t.action, "streams")) {
        if (!*id) tool_result_error(out, "proc://<id>/streams needs a job id, e.g. proc://bg_3/streams");
        else if (!jobs) tool_result_error(out, "Background job not found: %s", id);
        else {
            char *txt = jobs_streams(jobs, id, t.stream, &err);
            if (txt) emit_text(out, txt, lo, hi);
            else tool_result_error(out, "%s", err);
            free(txt);
            free(err);
        }
        goto done;
    }
    if (str_eq(t.action, "kill")) {
        tool_result_error(out,
                          "proc://%s/kill is write-only; reading it did not stop anything. To cancel %s, call write with "
                          "{\"path\":\"proc://%s/kill\"} (no content needed); read proc://%s for its status.",
                          *id ? id : "<id>", *id ? id : "<id>", *id ? id : "<id>", *id ? id : "<id>");
        goto done;
    }
    if (str_eq(t.action, "mode")) {
        tool_result_error(out,
                          "proc://%s/mode is write-only; modes and stdin are services-only, not available for in-process jobs or "
                          "agents. To set a service's mode, call write with {\"path\":\"proc://%s/mode\",\"content\":\"persist\"} "
                          "(persist, session, or detached); read proc://%s for its current mode.",
                          *id ? id : "<id>", *id ? id : "<id>", *id ? id : "<id>");
        goto done;
    }
    json_t *services = launch_enabled() ? services_list(env->cwd) : json_arr();
    if (!*id) {
        sb_t b = SB_INIT;
        json_t *snap = jobs ? jobs_snapshot(jobs) : json_arr(), *x;
        json_arr_foreach(snap, i, x) job_line(&b, x);
        json_free(snap);
        json_t *ag = agents_running();
        json_arr_foreach(ag, i, x) {
            if (jobs && agent_has_job(jobs, json_get_str(x, "id"))) continue;
            char *d = fmt_dur(json_get_int(x, "ageMs", 0));
            sb_appendf(&b, "%s%s [task] running up %s — %s", b.len ? "\n" : "", json_get_str(x, "id"), d,
                       json_get_str(x, "activity") ? json_get_str(x, "activity") : "agent");
            free(d);
        }
        json_free(ag);
        json_arr_foreach(services, i, x) {
            char *l = services_status_line(x);
            sb_appendf(&b, "%s%s", b.len ? "\n" : "", l);
            free(l);
        }
        if (!b.len) sb_append(&b, "No background jobs or services.");
        emit_text(out, sb_cstr(&b), lo, hi);
        sb_free(&b);
        out->details = json_obj();
        json_set_bool(out->details, "isDirectory", true);
        json_free(services);
        goto done;
    }
    json_t *svc = NULL, *x;
    json_arr_foreach(services, i, x) if (str_eq(json_get_str(x, "name"), id)) svc = x;
    json_t *job = jobs ? jobs_get(jobs, id) : NULL;
    if (!job && !svc && jobs) {
        char *rj = running_job_of_agent(jobs, id);
        if (rj) job = jobs_get(jobs, rj);
        free(rj);
    }
    if (job && svc) {
        tool_result_error(out, "proc://%s is ambiguous: both job %s and service %s exist.", id, id, id);
    } else if (svc) {
        sb_t b = SB_INIT;
        char *l = services_status_line(svc);
        const char *st = json_get_str(svc, "state");
        sb_appendf(&b, "%s\nready=%s persist=%s detached=%s", l,
                   json_get(svc, "readyAt") || str_eq(st, "ready") || str_eq(st, "running") ? "true" : "false",
                   json_get_bool(svc, "persist", false) ? "true" : "false", json_get_bool(svc, "detached", false) ? "true" : "false");
        if (json_get(svc, "exitCode")) sb_appendf(&b, " exit=%lld", (long long)json_get_int(svc, "exitCode", 0));
        free(l);
        char *logs = services_logs(env->cwd, id);
        if (*logs) sb_appendf(&b, "\n%s", logs);
        free(logs);
        emit_text(out, sb_cstr(&b), lo, hi);
        sb_free(&b);
    } else if (job) {
        sb_t b = SB_INIT;
        sb_appendf(&b, "%s [%s] — %s — %s", json_get_str(job, "id"), json_get_str(job, "type"), json_get_str(job, "status"),
                   json_get_str(job, "label"));
        bool running = str_eq(json_get_str(job, "status"), "running");
        const char *res = json_get_str(job, "result");
        if (res && *res) sb_appendf(&b, "\n%s", res);
        if (running) {
            const char *o = json_get_str(job, "output");
            if (o && *o) sb_appendf(&b, "\n%s", o);
            if (str_eq(json_get_str(job, "type"), "task"))
                sb_appendf(&b, "\nagent://%s", json_get_str(job, "agentId") ? json_get_str(job, "agentId") : json_get_str(job, "id"));
        } else if (str_eq(json_get_str(job, "type"), "task") && jobs) {
            const char *aid = json_get_str(job, "agentId") ? json_get_str(job, "agentId") : json_get_str(job, "id");
            char *succ = running_job_of_agent(jobs, aid);
            if (succ) sb_appendf(&b, "\n%s is running again as job %s (proc://%s)", aid, succ, succ);
            free(succ);
        }
        emit_text(out, sb_cstr(&b), lo, hi);
        sb_free(&b);
    } else {
        json_t *ag = agents_running(), *a = NULL;
        json_arr_foreach(ag, i, x) if (str_eq(json_get_str(x, "id"), id)) a = x;
        if (a) {
            char *txt = xasprintf("%s [task] — running — %s\nhistory://%s", id,
                                  json_get_str(a, "activity") ? json_get_str(a, "activity") : "agent", id);
            emit_text(out, txt, lo, hi);
            free(txt);
        } else {
            tool_result_error(out, "Background job or service not found: %s", id);
        }
        json_free(ag);
    }
    json_free(job);
    json_free(services);
done:
    proc_target_free(&t);
    free(u);
    return 0;
}

static int proc_write(tool_env_t *env, const char *url, const char *content, tool_result_t *out) {
    proc_target_t t;
    char *err = NULL;
    if (proc_parse(url, &t, &err) != 0) {
        tool_result_error(out, "%s", err);
        free(err);
        return 0;
    }
    const char *id = t.id;
    jobs_t *jobs = env->jobs;
    if (str_eq(t.action, "streams")) {
        tool_result_error(out, "proc://%s/streams is read-only", id);
        goto done;
    }
    if (!*id) {
        tool_result_error(out, "Write requires proc://<service> for stdin, proc://<id>/kill, or proc://<service>/mode; stdin and modes are "
                               "services-only.");
        goto done;
    }
    json_t *job = jobs ? jobs_get(jobs, id) : NULL;
    char *agent_job = NULL;
    if (!job && jobs) agent_job = running_job_of_agent(jobs, id);
    json_t *ag = agents_running(), *x;
    bool agent = false;
    json_arr_foreach(ag, i, x) if (str_eq(json_get_str(x, "id"), id)) agent = true;
    json_free(ag);
    json_t *svc = launch_enabled() ? services_find(env->cwd, id) : NULL;
    bool injob = job || agent_job;
    if ((injob || agent) && svc) {
        tool_result_error(out, "proc://%s is ambiguous: both job %s and service %s exist.", id, id, id);
    } else if ((injob || agent) && !str_eq(t.action, "kill")) {
        tool_result_error(out,
                          "%s is an in-process %s; stdin and persist/session/detached modes are services-only. Start long-lived work as a "
                          "named service when it needs stdin or lifetime controls. To cancel, call write with {\"path\":\"proc://%s/kill\"} "
                          "(no content needed).",
                          id, injob ? "job" : "agent", id);
    } else if (str_eq(t.action, "mode")) {
        json_t *snap = NULL;
        if (!svc) tool_result_error(out, "Service not found: %s", id);
        else if (services_mode(jobs, env->cwd, id, content ? content : "", &snap, &err) != 0) tool_result_error(out, "%s", err);
        else {
            char *l = services_status_line(snap);
            tool_result_textf(out, "%s; mode=%s", l, content);
            free(l);
        }
        json_free(snap);
    } else if (str_eq(t.action, "")) {
        json_t *snap = NULL;
        if (!svc) tool_result_error(out, "Service not found: %s", id);
        else if (services_send(env->cwd, id, content ? content : "", &snap, &err) != 0) tool_result_error(out, "%s", err);
        else {
            char *l = services_status_line(snap);
            tool_result_textf(out, "Sent input to %s", l);
            free(l);
        }
        json_free(snap);
    } else if (svc) {
        json_t *snap = NULL;
        if (services_stop(jobs, env->cwd, id, &snap, &err) != 0) tool_result_error(out, "%s", err);
        else {
            char *l = services_status_line(snap);
            tool_result_textf(out, "Stopped %s", l);
            free(l);
        }
        json_free(snap);
    } else if (injob) {
        const char *jid = job ? id : agent_job;
        json_t *cur = jobs_get(jobs, jid);
        if (cur && !str_eq(json_get_str(cur, "status"), "running")) {
            tool_result_textf(out, "%s already %s; nothing to cancel.", jid, json_get_str(cur, "status"));
        } else {
            jobs_cancel(jobs, jid);
            tool_result_textf(out, "Cancelled %s.", jid);
        }
        json_free(cur);
    } else if (agent) {
        if (agents_cancel(id)) tool_result_textf(out, "Cancelled %s.", id);
        else tool_result_error(out, "%s is not running.", id);
    } else {
        tool_result_error(out, "Background job or service not found: %s", id);
    }
    json_free(svc);
    json_free(job);
    free(agent_job);
done:
    proc_target_free(&t);
    return 0;
}

/* ---------- entry points ---------- */

int vurl_read(tool_env_t *env, const char *path, tool_result_t *out) {
    if (str_starts(path, "proc://")) return proc_read(env, path, out);
    int64_t lo, hi;
    char *u = split_sel(path, &lo, &hi);
    char *err = NULL, *txt = NULL;
    if (str_starts(u, "agent://")) txt = agents_read_output(u + strlen("agent://"), &err);
    else txt = agents_history(u + strlen("history://"), &err);
    if (txt) emit_text(out, txt, lo, hi);
    else tool_result_error(out, "%s", err ? err : "not found");
    free(txt);
    free(err);
    free(u);
    return 0;
}

int vurl_write(tool_env_t *env, const char *path, const char *content, tool_result_t *out) {
    if (str_starts(path, "proc://")) return proc_write(env, path, content, out);
    if (str_starts(path, "history://")) {
        tool_result_error(out, "history:// is read-only");
        return 0;
    }
    const char *rest = path + strlen("agent://");
    if (strchr(rest, '/') && strchr(rest, '/')[1]) {
        tool_result_error(out, "agent:// message target cannot have a JSON-path suffix.");
        return 0;
    }
    char *to = xstrdup(rest);
    char *sl = strchr(to, '/');
    if (sl) *sl = 0;
    bool is_err = false;
    char *r = agents_send(env, to, content ? content : "", &is_err);
    tool_result_text(out, r);
    out->is_error = is_err;
    free(r);
    free(to);
    return 0;
}
