/* print mode: run prompts non-interactively, emitting final text or JSON event lines. */
#include "modes.h"
#include "coding/costgate.h"
#include "coding/guards.h"
#include "mcp/mcp.h"
#include "tools/tools.h"

#include <pthread.h>
#include <unistd.h>

static pthread_mutex_t g_out = PTHREAD_MUTEX_INITIALIZER;

void out_json_line(const json_t *j) {
    sb_t b = SB_INIT;
    json_dump_sb(&b, j, 0);
    sb_putc(&b, '\n');
    pthread_mutex_lock(&g_out);
    fwrite(b.s, 1, b.len, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&g_out);
    sb_free(&b);
}

static void json_listener(void *ud, const agent_event_t *ev) {
    UNUSED(ud);
    json_t *j = agent_event_to_json(ev);
    out_json_line(j);
    json_free(j);
}

typedef struct {
    bool print_thoughts;
    bool in_thinking;
} text_ud_t;

static void text_listener(void *ud, const agent_event_t *ev) {
    text_ud_t *t = ud;
    if (ev->type != AGENT_EV_MESSAGE_UPDATE || !ev->ai || !t->print_thoughts) return;
    if (ev->ai->type == AI_EV_THINKING_DELTA) {
        if (!t->in_thinking) fputs("<thinking>\n", stderr);
        t->in_thinking = true;
        fwrite(ev->ai->delta, 1, ev->ai->delta_len, stderr);
    } else if (ev->ai->type == AI_EV_THINKING_END && t->in_thinking) {
        fputs("\n</thinking>\n", stderr);
        t->in_thinking = false;
    }
}

/* singleLine: untrusted server names / errors as one stderr line */
static char *single_line(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            while (p[1] == '\r' || p[1] == '\n' || p[1] == '\t') p++;
            sb_putc(&b, ' ');
        } else if ((unsigned char)*p >= 0x20 && *p != 0x7f) {
            sb_putc(&b, *p);
        }
    }
    return sb_detach(&b);
}

static void notice_stderr(void *ud, const char *level, const char *text) {
    UNUSED(ud);
    if (!strcmp(level, "todo-reminder")) return;
    if (!strcmp(level, "retry-failed")) {
        const char *f1 = strchr(text, '\x1f'), *f2 = f1 ? strchr(f1 + 1, '\x1f') : NULL;
        if (f1 && f2) fprintf(stderr, "[error] Retry failed after %d attempts: %s\n", atoi(text), f2 + 1);
        return;
    }
    if (!strcmp(level, "retry_fallback_applied")) {
        char *m = app_fallback_notice_text(text);
        fprintf(stderr, "[warning] %s\n", m);
        free(m);
        return;
    }
    /* auto_retry_start: a turn stuck retrying (offline, an outage) says why */
    int at, max;
    if (!strcmp(level, "warning") && sscanf(text, "Retrying (%d/%d)", &at, &max) == 2) {
        const char *msg = strstr(text, ": ");
        char *line = single_line(msg ? msg + 2 : "error");
        fprintf(stderr, "Retrying (%d/%d): %s\n", at, max, line);
        free(line);
        return;
    }
    if (strcmp(level, "warning") && strcmp(level, "error")) return;
    fprintf(stderr, "[%s] %s\n", level, text);
}

/* print-mode evaluateReadiness: wait for MCP startup (up to the server timeout), bind the manager's current tools
 * (deferred ones included) and warn about servers that are not usable. Returns their names (owned). */
static json_t *mcp_readiness(app_t *a) {
    json_t *out = json_arr();
    if (!a->mcp) return out;
    int64_t timeout = mcp_timeout_ms();
    mcp_wait_startup(a->mcp, timeout);
    mcp_sync_tools(a->mcp, a->agent);
    tools_finalize(a->agent, a->opts.tools);
    app_rebuild_system_prompt(a);
    json_t *names = mcp_server_names(a->mcp), *nm;
    json_arr_foreach(names, i, nm) {
        if (mcp_server_state(a->mcp, json_sv(nm)) != MCP_CONNECTING) continue;
        char *server = single_line(json_sv(nm));
        if (timeout > 0)
            fprintf(stderr, "Warning: MCP server \"%s\" not ready after %lldms; its tools are unavailable for this run.\n",
                    server, (long long)timeout);
        else fprintf(stderr, "Warning: MCP server \"%s\" not ready; its tools are unavailable for this run.\n", server);
        json_push(out, json_str_own(server));
    }
    json_t *failed = mcp_failed(a->mcp), *f;
    json_arr_foreach(failed, i, f) {
        if (mcp_server_state(a->mcp, json_get_str(f, "name")) == MCP_CONNECTING) continue;
        char *server = single_line(json_get_str(f, "name")), *err = single_line(json_get_str(f, "error"));
        fprintf(stderr, "Warning: MCP server \"%s\" failed to connect: %s; its tools are unavailable for this run.\n", server, err);
        json_push(out, json_str_own(server));
        free(err);
    }
    json_free(failed);
    json_free(names);
    return out;
}

int run_print_mode(app_t *a, const print_opts_t *o) {
    text_ud_t tu = {.print_thoughts = o->print_thoughts};
    /* plan.defaultOnStartup needs a review surface; the headless plan flow is --plan-yolo */
    if (!a->plan_mode && !a->plan_yolo_target && app_plan_should_start(a))
        fputs("Note: plan.defaultOnStartup is ignored in print mode (no interactive surface to review the plan). Use "
              "--plan-yolo for a headless plan flow.\n",
              stderr);
    if (o->json) {
        agent_subscribe(a->agent, json_listener, NULL);
        json_t *hdr = json_obj();
        json_set_str(hdr, "type", "session");
        json_set_str(hdr, "id", session_id(a->session));
        json_set_str(hdr, "cwd", a->cwd);
        char *ml = app_model_label(a);
        json_set_str(hdr, "model", ml);
        free(ml);
        out_json_line(hdr);
        json_free(hdr);
    } else {
        agent_subscribe(a->agent, text_listener, &tu);
        a->notice = notice_stderr;
        a->text_buffered = true; /* setTextOutputCommitted(false): text prints after the run settles */
    }
    int rc = 0;
    guards_disable_stop_reminders(a); /* omp print mode: todo reminders never start extra turns */
    json_t *unavailable = NULL;
    bool strict = env_truthy("OMP_MCP_REQUIRE_READY") || env_truthy("OMC_MCP_REQUIRE_READY");
    for (size_t i = 0; i < o->messages.n; i++) {
        size_t before = json_len(a->agent->messages);
        /* prepareAgentTurn: MCP readiness before the first model turn (and before MCP prompt commands expand);
         * OMP_MCP_REQUIRE_READY refuses the run */
        if (!unavailable) unavailable = mcp_readiness(a);
        if (strict && json_len(unavailable)) break;
        /* session.prompt: MCP prompt / file / template commands expand (skill commands do not run here) */
        char *x = app_expand_input(a, o->messages.v[i], true);
        if (!x) continue;
        int prc = app_prompt(a, x, i == 0 && json_len(o->initial_images) ? json_clone(o->initial_images) : NULL);
        free(x);
        if (prc == APP_PROMPT_REFUSED) { /* session.prompt rejected: the run fails with no turn to report */
            rc = 1;
            break;
        }
        /* advisor notes (and goal continuations when "print" is enabled) extend the run before printing */
        const char *prev = NULL;
        for (int guard = 0; guard < 50; guard++) {
            json_t *run = json_arr();
            for (size_t k = before; k < json_len(a->agent->messages); k++) json_push(run, json_clone(json_at(a->agent->messages, k)));
            const char *ctype = NULL;
            char *cont = app_after_run(a, run, prev, "print", &ctype);
            json_free(run);
            if (!cont) break;
            before = json_len(a->agent->messages);
            prev = ctype;
            app_prompt_hidden(a, cont, ctype);
            free(cont);
        }
        /* a request refused by the cost cap ends the run short of an answer: the refusal is the outcome (exit 3) */
        if (costgate_take_refusal(a)) {
            rc = 3;
            break;
        }
        const json_t *last = last_assistant(a->agent->messages);
        const char *stop = json_get_str(last, "stopReason");
        if (str_eq(stop, "error") || str_eq(stop, "aborted")) {
            if (!o->json)
                fprintf(stderr, "omc: %s\n", json_get_str(last, "errorMessage") ? json_get_str(last, "errorMessage") : stop);
            rc = 1;
            break;
        }
    }
    /* a run with no model turn still reports strict unreadiness */
    if (strict && !unavailable) unavailable = mcp_readiness(a);
    if (strict && json_len(unavailable)) {
        sb_t b = SB_INIT;
        json_t *n;
        json_arr_foreach(unavailable, i, n) sb_appendf(&b, "%s%s", i ? ", " : "", json_sv(n));
        fprintf(stderr, "Error: MCP servers not ready: %s\n", sb_cstr(&b));
        sb_free(&b);
        rc = 1;
    }
    json_free(unavailable);
    if (!o->json) {
        const json_t *last = last_assistant(a->agent->messages);
        if (last && rc == 0) {
            char *text = ai_message_text(last);
            fputs(text, stdout);
            if (*text && text[strlen(text) - 1] != '\n') fputc('\n', stdout);
            free(text);
        }
        fflush(stdout);
    }
    return rc;
}
