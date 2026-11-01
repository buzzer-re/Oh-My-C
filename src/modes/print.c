/* print mode: run prompts non-interactively, emitting final text or JSON event lines. */
#include "modes.h"

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

static void notice_stderr(void *ud, const char *level, const char *text) {
    UNUSED(ud);
    if (strcmp(level, "warning") && strcmp(level, "error")) return;
    fprintf(stderr, "[%s] %s\n", level, text);
}

int run_print_mode(app_t *a, const print_opts_t *o) {
    text_ud_t tu = {.print_thoughts = o->print_thoughts};
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
    }
    int rc = 0;
    for (size_t i = 0; i < o->messages.n; i++) {
        size_t before = json_len(a->agent->messages);
        app_prompt(a, o->messages.v[i], NULL);
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
        const json_t *last = last_assistant(a->agent->messages);
        const char *stop = json_get_str(last, "stopReason");
        if (str_eq(stop, "error") || str_eq(stop, "aborted")) {
            if (!o->json)
                fprintf(stderr, "omc: %s\n", json_get_str(last, "errorMessage") ? json_get_str(last, "errorMessage") : stop);
            rc = 1;
            break;
        }
    }
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
