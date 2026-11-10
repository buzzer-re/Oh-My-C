/* iqueue.c - input that arrives while the agent (or a maintenance pass) is busy, ported from omp's
 * InputController (handleFollowUp, handleDequeue, restoreQueuedMessagesToEditor, #queueForYield),
 * CommandController (handleBashCommand, handleCompactCommand / executeCompaction, handleHandoffCommand)
 * and ui-helpers (queueCompactionMessage, flushCompactionQueue, flushPendingBashComponents).
 *
 * Enter while a run streams steers it; the follow-up key queues for when it yields. Both show in the
 * queued-messages band (ui/queue.c) until the agent delivers them, when their user message_start draws
 * them in the transcript. `!cmd`, /compact and /handoff run on their own threads so the UI keeps
 * painting and Esc cancels them; input typed meanwhile queues behind the compaction. */
#include "coding/goal.h"
#include "coding/slashcmd.h"
#include "modes/interactive.h"

#include "config/settings.h"
#include "prompt/template.h"

#include <pthread.h>

static void status(ui_t *u, const char *text) { add_notice(u, "info", text); }

/* ---------- editor restore (InputController#restoreEntriesToEditor) ---------- */

static void restore_to_editor(ui_t *u, const strvec_t *texts) {
    if (!texts->n) {
        ui_queue_refresh(u);
        return;
    }
    sb_t q = SB_INIT;
    for (size_t i = 0; i < texts->n; i++) {
        if (i) sb_append(&q, "\n\n");
        sb_append(&q, texts->v[i]);
    }
    char *cur = tui_editor_get_text(u->editor);
    sb_t b = SB_INIT;
    const char *parts[2] = {sb_cstr(&q), cur ? cur : ""};
    for (int i = 0; i < 2; i++) {
        char *t = str_trim(parts[i]);
        if (*t) {
            if (b.len) sb_append(&b, "\n\n");
            sb_append(&b, parts[i]);
        }
        free(t);
    }
    tui_editor_set_text(u->editor, sb_cstr(&b));
    sb_free(&b);
    sb_free(&q);
    free(cur);
    ui_queue_refresh(u);
}

/* ---------- compaction queue (ui-helpers queueCompactionMessage / flushCompactionQueue) ---------- */

static void cq_push(ui_t *u, const char *text, const char *mode) {
    if (!u->compaction_queue) u->compaction_queue = json_arr();
    json_t *e = json_obj();
    json_set_str(e, "text", text);
    json_set_str(e, "mode", mode);
    json_push(u->compaction_queue, e);
}

static void queue_compaction_message(ui_t *u, const char *text, const char *mode) {
    cq_push(u, text, mode);
    ui_queue_refresh(u);
    status(u, "Queued message for after compaction");
}

/* "." or "c" resume the agent without a visible user message. During a /guided-goal interview "c" is a plausible answer
 * (e.g. option C), so it is sent as a normal reply there. */
static bool iq_is_continue_shortcut(ui_t *u, const char *t) {
    return str_eq(t, ".") || (str_eq(t, "c") && !app_goal_interview_active(u->app));
}

static void flush_compaction_queue(ui_t *u) {
    if (u->yield_compaction || !json_len(u->compaction_queue)) return;
    json_t *q = u->compaction_queue;
    u->compaction_queue = NULL;
    ui_queue_refresh(u);
    json_t *e;
    json_arr_foreach(q, i, e) {
        const char *text = json_get_str(e, "text");
        if (!text || !*text) continue;
        if (text[0] == '/' && ix_handle_command(u, text)) continue;
        if (iq_is_continue_shortcut(u, text) && !u->bg_kind) {
            ix_resume(u);
            continue;
        }
        if (!u->worker_running && !u->bg_kind) ix_start_prompt(u, text);
        else if (str_eq(json_get_str(e, "mode"), "followUp")) agent_follow_up(u->app->agent, msg_user_text(text));
        else agent_steer(u->app->agent, msg_user_text(text));
    }
    json_free(q);
    ui_queue_refresh(u);
}

bool iq_busy(ui_t *u) { return u->bg_kind != 0; }

void iq_hold(ui_t *u, bool on) {
    u->bg_kind = on ? 3 : 0;
    if (!on) flush_compaction_queue(u);
}

/* ---------- /compact and /handoff off the UI thread ---------- */

enum { BG_COMPACT = 1, BG_HANDOFF = 2 };

typedef struct {
    ui_t *u;
    int kind;
    bool resume; /* the compaction interrupted a live turn: resume it afterwards */
    char *instructions;
    int rc;
    char *err;
} bg_t;

static void bg_done(void *p) {
    bg_t *b = p;
    ui_t *u = b->u;
    pthread_join(u->bg_thread, NULL);
    u->bg_kind = 0;
    ui_working_stop(u);
    if (b->kind == BG_COMPACT) {
        if (b->rc != 0) {
            const char *e = b->err ? b->err : "compaction failed";
            if (!strcmp(e, "Compaction cancelled")) add_notice(u, "error", "Compaction cancelled");
            else {
                char *m = xasprintf("Compaction failed: %s", e);
                add_notice(u, "error", m);
                free(m);
            }
        }
    } else if (b->rc != 0) {
        const char *e = b->err ? b->err : "handoff failed";
        if (!strcmp(e, "Compaction cancelled")) add_notice(u, "error", "Handoff cancelled");
        else {
            char *m = xasprintf("Handoff failed: %s", e);
            add_notice(u, "error", m);
            free(m);
        }
    } else {
        /* the transcript was rebuilt around the handoff divider ("Context compacted"); renderInitialMessages
         * reports the compaction count */
        size_t n = 0;
        json_t *e;
        json_arr_foreach(u->app->session->entries, i, e) if (str_eq(json_get_str(e, "type"), "compaction")) n++;
        if (n) {
            char *c = n == 1 ? xstrdup("Session compacted 1 time") : xasprintf("Session compacted %zu times", n);
            status(u, c);
            free(c);
        }
        char *t = xasprintf("%s Context handed off and compacted in place", tui_sym("status.success"));
        char *s = fg(TUI_TC_ACCENT, t);
        tui_container_add(u->chat, &tui_text_new(s, 1, 1)->base);
        free(s);
        free(t);
    }
    /* a committed (or no-op) manual pass resumes the turn it interrupted (compaction.autoContinue) */
    bool noop = b->err && (str_starts(b->err, "Nothing to compact") || !strcmp(b->err, "Already compacted"));
    if (b->kind == BG_COMPACT && u->after_compact) {
        void (*then)(ui_t *, bool, void *) = u->after_compact;
        void *ud = u->after_compact_ud;
        u->after_compact = NULL;
        u->after_compact_ud = NULL;
        then(u, b->rc == 0 || noop, ud);
    }
    if (b->resume && (b->rc == 0 || noop) && settings_bool("compaction.autoContinue", true) && !u->worker_running) {
        char *cont = prompt_load("coding-agent/prompts/system/auto-continue.md");
        if (cont) ix_start_hidden(u, cont, "auto-continue");
        free(cont);
    }
    if (b->resume && !u->worker_running) ui_chrome_turn(u, false);
    free(b->instructions);
    free(b->err);
    free(b);
    tui_request_render(u->tui);
    flush_compaction_queue(u);
    if (!u->worker_running && !u->bg_kind && !u->compaction_queue) iq_run_end(u);
    /* results that settled during the compaction go into the compacted context */
    if (!u->worker_running && !u->bg_kind) ix_job_wake(u);
}

static void *bg_main(void *p) {
    bg_t *b = p;
    app_t *a = b->u->app;
    if (b->kind == BG_COMPACT) {
        /* `/compact [soft|remote|snapcompact] [focus]` (already validated by the slash handler) */
        const char *mode = NULL;
        char *focus = NULL, *perr = NULL;
        if (!slash_parse_compact_args(b->instructions, &mode, &focus, &perr)) {
            b->err = perr;
            b->rc = -1;
        } else b->rc = app_compact_mode(a, mode, focus, &b->err);
        free(focus);
    } else {
        b->rc = app_handoff(a, b->instructions, &b->err); /* focus instructions */
    }
    tui_post(b->u->tui, bg_done, b);
    return NULL;
}

static void bg_start(ui_t *u, int kind, const char *instructions, bool resume) {
    bg_t *b = xcalloc(1, sizeof *b);
    b->u = u;
    b->kind = kind;
    b->resume = resume;
    b->instructions = instructions && *instructions ? xstrdup(instructions) : NULL;
    u->bg_kind = kind;
    ui_working_compaction(u, kind == BG_COMPACT ? "Compacting context..." : "Generating handoff\xe2\x80\xa6");
    if (pthread_create(&u->bg_thread, NULL, bg_main, b) != 0) {
        u->bg_kind = 0;
        ui_working_stop(u);
        add_notice(u, "error", "failed to start compaction thread");
        free(b->instructions);
        free(b);
    }
}

static size_t message_entries(ui_t *u) {
    size_t n = 0;
    json_t *e;
    if (u->app->session) json_arr_foreach(u->app->session->entries, i, e) if (str_eq(json_get_str(e, "type"), "message")) n++;
    return n;
}

static void compact_now(ui_t *u, const char *instructions, bool resume) {
    if (u->bg_kind) {
        add_notice(u, "error", "Compaction failed: Compaction already in progress");
        return;
    }
    if (message_entries(u) < 2) {
        add_notice(u, "warning", "Nothing to compact (no messages yet)");
        return;
    }
    bg_start(u, BG_COMPACT, instructions, resume);
}

void iq_compact(ui_t *u, const char *arg) {
    if (u->worker_running) {
        /* a manual compaction interrupts the live turn and summarizes once it has unwound */
        free(u->compact_pending);
        u->compact_pending = xstrdup(arg ? arg : "");
        app_abort(u->app);
        return;
    }
    compact_now(u, arg, false);
}

void iq_compact_then(ui_t *u, void (*then)(ui_t *u, bool ok, void *ud), void *ud) {
    if (u->bg_kind || u->after_compact) {
        add_notice(u, "error", "Compaction failed: Compaction already in progress");
        then(u, false, ud);
        return;
    }
    u->after_compact = then;
    u->after_compact_ud = ud;
    iq_compact(u, NULL);
    if (!u->bg_kind && !u->compact_pending && u->after_compact) { /* nothing to compact: switch right away */
        u->after_compact = NULL;
        u->after_compact_ud = NULL;
        then(u, true, ud);
    }
}

/* follow-up key while streaming: run the compaction once the agent yields (InteractiveMode.yieldCompaction) */
static void compact_on_yield(ui_t *u, const char *text) {
    free(u->yield_compaction);
    u->yield_compaction = xstrdup(text);
    ui_queue_refresh(u);
    status(u, "Queued compaction for when the agent yields");
}

void iq_handoff(ui_t *u, const char *arg) {
    if (u->worker_running) {
        add_notice(u, "warning", "Wait for the current response to finish or abort it before handing off.");
        return;
    }
    if (u->bg_kind == BG_COMPACT) {
        add_notice(u, "warning", "Wait for context compaction to finish or cancel it before handing off.");
        return;
    }
    if (u->bg_kind) return;
    if (message_entries(u) < 2) {
        add_notice(u, "warning", "Nothing to hand off (no messages yet)");
        return;
    }
    bg_start(u, BG_HANDOFF, arg, false);
}

/* ---------- `!cmd` off the UI thread (CommandController.handleBashCommand) ---------- */

#define BASH_LIVE_CAP (256u * 1024)

struct ui_bash {
    ui_t *u;
    char *cmd;
    bool exclude, python;
    msgview_t *view;
    volatile sig_atomic_t cancel;
    pthread_t th;
    pthread_mutex_t mu;
    sb_t live; /* merged output so far (tail-capped) */
    bool live_dirty;
    int timer;
    json_t *result;
};

static void bash_output(void *ud, int stream, const char *data, size_t len) {
    UNUSED(stream);
    struct ui_bash *b = ud;
    pthread_mutex_lock(&b->mu);
    sb_appendn(&b->live, data, len);
    if (b->live.len > BASH_LIVE_CAP) {
        size_t drop = b->live.len - BASH_LIVE_CAP / 2;
        const char *nl = memchr(b->live.s + drop, '\n', b->live.len - drop);
        if (nl) drop = (size_t)(nl - b->live.s) + 1;
        memmove(b->live.s, b->live.s + drop, b->live.len - drop);
        b->live.len -= drop;
        b->live.s[b->live.len] = 0;
    }
    b->live_dirty = true;
    pthread_mutex_unlock(&b->mu);
}

/* Loader cadence (80 ms): advance the spinner and pick up streamed output */
static void bash_tick(void *ud) {
    struct ui_bash *b = ud;
    pthread_mutex_lock(&b->mu);
    if (b->live_dirty) {
        free(b->view->output);
        b->view->output = xstrdup(sb_cstr(&b->live));
        b->live_dirty = false;
    }
    pthread_mutex_unlock(&b->mu);
    b->view->dirty = true;
    tui_request_render(b->u->tui);
}

static void bash_done(void *p) {
    struct ui_bash *b = p;
    ui_t *u = b->u;
    pthread_join(b->th, NULL);
    if (b->timer) tui_remove_timer(u->tui, b->timer);
    msgview_t *v = b->view;
    json_t *m = b->result;
    free(v->output);
    if (m) {
        v->output = xstrdup(json_get_str(m, "output") ? json_get_str(m, "output") : "");
        bool cancelled = json_get_bool(m, "cancelled", false);
        int64_t code = json_get_int(m, "exitCode", 0);
        v->is_error = cancelled || code != 0;
        json_set_int(v->args, "exitCode", code);
        if (cancelled) json_set_bool(v->args, "cancelled", true);
        /* a run in flight owns the context: the result joins it once the run ends */
        if (u->worker_running) {
            if (!u->deferred_bash) u->deferred_bash = json_arr();
            json_push(u->deferred_bash, json_clone(m));
        } else if (b->python) {
            app_record_python(u->app, m);
        } else {
            app_record_bash(u->app, m);
        }
    } else {
        v->output = xstrdup("");
        v->is_error = true;
        add_notice(u, "error", b->python ? "Python execution failed: the eval tool is unavailable" : "Bash command failed: Unknown error");
    }
    v->done = true;
    v->dirty = true;
    json_free(m);
    pthread_mutex_destroy(&b->mu);
    sb_free(&b->live);
    if (u->bash == b) u->bash = NULL;
    if (u->python == b) u->python = NULL;
    free(b->cmd);
    free(b);
    tui_request_render(u->tui);
}

static void *bash_main(void *p) {
    struct ui_bash *b = p;
    b->result = b->python ? app_user_python_run(b->u->app, b->cmd, b->exclude, &b->cancel)
                          : app_user_bash_run(b->u->app, b->cmd, b->exclude, bash_output, b, &b->cancel);
    tui_post(b->u->tui, bash_done, b);
    return NULL;
}

static void run_bang(ui_t *u, const char *cmd, bool exclude, bool python) {
    struct ui_bash *b = xcalloc(1, sizeof *b);
    b->u = u;
    b->cmd = xstrdup(cmd);
    b->exclude = exclude;
    b->python = python;
    pthread_mutex_init(&b->mu, NULL);
    msgview_t *v = mv_new(MV_BASH);
    v->tool_name = xstrdup("$");
    v->args = json_obj();
    json_set_str(v->args, "command", cmd);
    json_set_int(v->args, "startMs", mono_ms());
    if (exclude) json_set_bool(v->args, "exclude", true);
    if (python) json_set_bool(v->args, "python", true);
    b->view = v;
    /* while a run streams, the block waits below the transcript until the next prompt (pendingShellContainer) */
    if (u->worker_running) tui_container_add(u->pending_shell, &v->base);
    else add_view(u, v);
    if (python) u->python = b;
    else u->bash = b;
    b->timer = tui_add_clock_timer(u->tui, NULL, bash_tick, b);
    if (pthread_create(&b->th, NULL, bash_main, b) != 0) {
        b->th = pthread_self(); /* never joined below: finish inline */
        b->result = NULL;
        tui_remove_timer(u->tui, b->timer);
        b->timer = 0;
        v->done = true;
        add_notice(u, "error", "Bash command failed: cannot start thread");
        pthread_mutex_destroy(&b->mu);
        free(b->cmd);
        free(b);
        if (python) u->python = NULL;
        else u->bash = NULL;
    }
    tui_request_render(u->tui);
}

/* move `!` blocks that ran during a turn into the transcript (flushPendingBashComponents) */
static void flush_pending_shell(ui_t *u) {
    while (u->pending_shell->children.n) {
        tui_component_t *c = u->pending_shell->children.v[0];
        tui_container_remove(u->pending_shell, c);
        tui_container_add(u->chat, c);
    }
}

static void bang(ui_t *u, const char *text) {
    bool exclude = text[1] == '!';
    char *cmd = str_trim(text + (exclude ? 2 : 1));
    if (*cmd) {
        if (u->bash) {
            char *k = ui_editor_key("app.interrupt");
            char *m = xasprintf("A bash command is already running. Press %s to cancel it first.", k);
            add_notice(u, "warning", m);
            free(m);
            free(k);
            tui_editor_set_text(u->editor, text);
        } else {
            run_bang(u, cmd, exclude, false);
        }
    }
    free(cmd);
}

static bool python_mode(const char *t) {
    size_t n = t[0] == '$' ? (t[1] == '$' ? 2 : 1) : 0;
    return n && (t[n] == ' ' || t[n] == '\t' || t[n] == '\n' || t[n] == '\r');
}

/* `$ code` / `$$ code` (CommandController.handlePythonCommand); true when the text was Python input */
static bool python(ui_t *u, const char *text) {
    if (!python_mode(text)) return false;
    bool exclude = text[1] == '$';
    char *code = str_trim(text + (exclude ? 2 : 1));
    if (!*code) {
        free(code);
        return false;
    }
    if (u->python) {
        char *k = ui_editor_key("app.interrupt");
        char *m = xasprintf("A Python execution is already running. Press %s to cancel it first.", k);
        add_notice(u, "warning", m);
        free(m);
        free(k);
        tui_editor_set_text(u->editor, text);
    } else {
        run_bang(u, code, exclude, true);
    }
    free(code);
    return true;
}

/* ---------- /queue and `->` / `=>` (InputController#queueForYield) ---------- */

void iq_queue_command(ui_t *u, const char *text) {
    strvec_t msgs = {0};
    ui_split_queued_messages(text ? text : "", &msgs);
    if (!msgs.n) {
        add_notice(u, "warning", "Usage: /queue <message> (or start a prompt with -> / =>)");
        return;
    }
    char *m;
    if (u->bg_kind == BG_COMPACT || u->yield_compaction) {
        for (size_t i = 0; i < msgs.n; i++) cq_push(u, msgs.v[i], "followUp");
        ui_queue_refresh(u);
        m = msgs.n == 1 ? xstrdup("Queued message for after compaction") : xasprintf("Queued %zu messages for after compaction", msgs.n);
        status(u, m);
        free(m);
        strvec_free(&msgs);
        return;
    }
    bool now = !u->worker_running && !agent_queued_count(u->app->agent);
    size_t i = 0;
    if (now) {
        ix_start_prompt(u, msgs.v[0]);
        i = 1;
        if (!u->worker_running && msgs.n > 1) {
            /* the first never reached the agent (refused at preflight): the whole list goes back as one `=>` list,
             * nothing runs ahead of it (omp e0eba274da) */
            char *cur = tui_editor_get_expanded_text(u->editor);
            if (!*cur || str_eq(cur, msgs.v[0])) {
                char *list = ui_format_queue_shorthand(&msgs);
                tui_editor_set_text(u->editor, list);
                free(list);
            }
            free(cur);
            i = msgs.n;
        }
    }
    for (; i < msgs.n; i++) agent_follow_up(u->app->agent, msg_user_text(msgs.v[i]));
    ui_queue_refresh(u);
    if (now) m = msgs.n == 1 ? xstrdup("Sent queued message") : xasprintf("Sent first message; queued %zu for later yields", msgs.n - 1);
    else m = msgs.n == 1 ? xstrdup("Queued message for when the agent yields") : xasprintf("Queued %zu messages for when the agent yields", msgs.n);
    status(u, m);
    free(m);
    strvec_free(&msgs);
}

/* ---------- Enter ---------- */

void iq_submit(ui_t *u, const char *t, json_t *attachments) { iq_submit_mode(u, t, attachments, false); }

void iq_submit_mode(ui_t *u, const char *t, json_t *attachments, bool loop_body) {
    app_sync_queue_modes(u->app);
    if (!*t && !json_len(attachments)) {
        json_free(attachments);
        /* empty submit while streaming with queued input: interrupt the turn; the queue is delivered next */
        if (u->worker_running && agent_has_interruptible_input(u->app->agent)) {
            agent_abort_deliver_queued(u->app->agent);
            app_abort(u->app);
        }
        return;
    }
    if (!loop_body && !json_len(attachments) && iq_is_continue_shortcut(u, t)) {
        json_free(attachments);
        if (u->bg_kind) queue_compaction_message(u, t, "steer");
        else ix_resume(u);
        return;
    }
    /* `->` / `=>`, slash commands and `!` / `$` runs are handled locally (staged images are dropped) */
    const char *body = ui_queue_shorthand(t);
    bool local = true;
    if (body) iq_queue_command(u, body);
    else if (t[0] == '/' && ix_handle_command(u, t)) {
    } else if (ix_side_file_command(u, t)) {
    } else if (ix_is_skill_command(u, t)) {
        /* /skill:<name>: Enter steers like free text; behind a compaction it queues as typed */
        lp_set_prompt(u, t);
        if (u->bg_kind) {
            queue_compaction_message(u, t, "steer");
            json_free(attachments);
        } else ix_invoke_skill(u, t, attachments, false);
        return;
    } else if (t[0] == '!') bang(u, t);
    else if (!python(u, t)) local = false;
    if (!local && u->bg_kind) {
        queue_compaction_message(u, t, "steer"); /* text-only behind a compaction */
        if (loop_body) lp_set_prompt(u, t);
    }
    if (local || u->bg_kind) {
        json_free(attachments);
        return;
    }
    if (u->worker_running) {
        /* session.prompt(..., steer): custom / file / template commands expand before queueing */
        char *x = app_expand_input(u->app, t, true);
        if (!x) json_free(attachments);
        else if (json_len(attachments)) app_steer_attachments(u->app, x, attachments);
        else {
            json_free(attachments);
            agent_steer(u->app->agent, msg_user_text(x));
        }
        if (x && loop_body) lp_set_prompt(u, t);
        free(x);
        ui_queue_refresh(u);
        return;
    }
    /* while loop mode is on, an idle prompt becomes the loop prompt (armed before the run starts) */
    lp_set_prompt(u, t);
    flush_pending_shell(u);
    ix_start_prompt_with(u, t, attachments);
}

/* /btw inject (session.prompt with expandPromptTemplates off, streamingBehavior steer): a steer while the agent
 * runs, else a new turn, the text never run as a command or template. Returns whether it was admitted; *reason
 * (owned) says why not when it was refused. */
bool iq_inject(ui_t *u, const char *text, char **reason) {
    *reason = NULL;
    if (u->bg_kind) return false; /* a compaction holds the session: the inject key retries */
    app_sync_queue_modes(u->app);
    if (u->worker_running) {
        agent_steer(u->app->agent, msg_user_text(text));
        ui_queue_refresh(u);
        return true;
    }
    if (!u->app->model) {
        *reason = xstrdup("No model selected.");
        return false;
    }
    flush_pending_shell(u);
    ix_start_prompt_literal(u, text);
    return true;
}

/* ---------- follow-up key (InputController.handleFollowUp) ---------- */

void iq_follow_up(ui_t *u) {
    app_sync_queue_modes(u->app);
    char *raw = tui_editor_get_expanded_text(u->editor);
    char *t = str_trim(raw);
    free(raw);
    if (!*t) {
        free(t);
        return;
    }
    tui_editor_clear(u->editor);
    ix_history_add(u, t);
    if (u->bg_kind == BG_COMPACT || u->yield_compaction) {
        queue_compaction_message(u, t, "followUp");
    } else if (!strncmp(t, "/compact", 8) && (!t[8] || t[8] == ' ') && u->worker_running) {
        compact_on_yield(u, t);
    } else if (t[0] == '/' && ix_handle_command(u, t)) {
        /* handled */
    } else if (ix_is_skill_command(u, t)) {
        ix_invoke_skill(u, t, NULL, true);
    } else if (u->worker_running) {
        char *x = app_expand_input(u->app, t, true);
        if (x) agent_follow_up(u->app->agent, msg_user_text(x));
        free(x);
        ui_queue_refresh(u);
    } else {
        ix_start_prompt(u, t);
    }
    free(t);
    tui_request_render(u->tui);
}

/* ---------- dequeue key (InputController.handleDequeue / #popLastQueuedMessage) ---------- */

void iq_dequeue(ui_t *u) {
    char *text = NULL;
    strvec_t st = {0};
    if (u->yield_compaction) agent_queue_texts(u->app->agent, &st, NULL);
    /* steering pops first; behind a yield compaction the follow-ups it holds go next, then the request */
    if (!u->yield_compaction || st.n) {
        json_t *m = agent_pop_last_queued(u->app->agent);
        if (m) text = agent_queued_text(m);
        json_free(m);
    }
    strvec_free(&st);
    if (!text && json_len(u->compaction_queue)) {
        json_t *last = json_arr_remove(u->compaction_queue, json_len(u->compaction_queue) - 1);
        text = xstrdup(json_get_str(last, "text") ? json_get_str(last, "text") : "");
        json_free(last);
    }
    if (!text && u->yield_compaction) {
        text = u->yield_compaction;
        u->yield_compaction = NULL;
    }
    if (!text) {
        status(u, "No queued messages to restore");
        return;
    }
    strvec_t one = {0};
    strvec_push_own(&one, text);
    restore_to_editor(u, &one);
    strvec_free(&one);
    status(u, "Restored last queued message to editor");
}

/* every queued message back to the editor (Esc while working): steering, compaction steering, follow-ups,
 * the queued /compact, compaction follow-ups */
static void restore_all(ui_t *u) {
    json_t *taken = agent_take_user_queued(u->app->agent);
    strvec_t texts = {0};
    json_t *m;
    json_arr_foreach(json_get_arr(taken, "steering"), i, m) strvec_push_own(&texts, agent_queued_text(m));
    json_arr_foreach(u->compaction_queue, i, m) if (str_eq(json_get_str(m, "mode"), "steer")) strvec_push(&texts, json_get_str(m, "text"));
    json_arr_foreach(json_get_arr(taken, "followUp"), i, m) strvec_push_own(&texts, agent_queued_text(m));
    if (u->yield_compaction) strvec_push(&texts, u->yield_compaction);
    json_arr_foreach(u->compaction_queue, i, m) if (!str_eq(json_get_str(m, "mode"), "steer")) strvec_push(&texts, json_get_str(m, "text"));
    json_free(taken);
    json_free(u->compaction_queue);
    u->compaction_queue = NULL;
    free(u->yield_compaction);
    u->yield_compaction = NULL;
    restore_to_editor(u, &texts);
    strvec_free(&texts);
}

/* ---------- Esc (app.interrupt) ---------- */

bool iq_escape(ui_t *u) {
    if (u->bg_kind) { /* abortCompaction / abortHandoff */
        agent_abort(u->app->agent);
        return true;
    }
    if (lp_escape(u)) return true; /* loop mode: Esc suspends the loop (and aborts the live turn) */
    if (u->worker_running) {
        restore_all(u);
        app_abort(u->app);
        return true;
    }
    if (u->bash) {
        u->bash->cancel = 1;
        return true;
    }
    char *txt = tui_editor_get_text(u->editor);
    const char *p = txt ? txt : "";
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    bool bash_mode = *p == '!', py_mode = python_mode(p);
    free(txt);
    if (!bash_mode && u->python) {
        u->python->cancel = 1;
        return true;
    }
    if (bash_mode || py_mode) { /* leave bash / python mode */
        tui_editor_set_text(u->editor, "");
        tui_request_render(u->tui);
        return true;
    }
    return false;
}

/* ---------- run boundaries ---------- */

void iq_user_message_start(ui_t *u, const json_t *msg) {
    msgview_t *irc = str_eq(json_get_str(msg, "role"), "user") ? ui_parent_irc_view(msg) : NULL;
    if (irc) { /* a parent steer: its sender and body, not the model-facing envelope (omp db90c0d96e) */
        toolcard_break_group(u);
        add_view(u, irc);
        ui_queue_refresh(u);
        return;
    }
    if (!agent_msg_user_authored(msg)) return;
    if (str_eq(json_get_str(msg, "customType"), "skill-prompt")) { /* SkillMessageComponent */
        toolcard_break_group(u);
        msgview_t *v = mv_new(MV_SKILL);
        v->msg = json_clone(msg);
        add_view(u, v);
        ui_queue_refresh(u);
        return;
    }
    char *text = agent_queued_text(msg);
    bool drawn = false;
    for (size_t i = 0; i < u->optimistic.n; i++) {
        if (!strcmp(u->optimistic.v[i], text)) {
            free(u->optimistic.v[i]);
            memmove(&u->optimistic.v[i], &u->optimistic.v[i + 1], (u->optimistic.n - i - 1) * sizeof(char *));
            u->optimistic.n--;
            drawn = true;
            break;
        }
    }
    if (!drawn) {
        toolcard_break_group(u);
        msgview_t *v = mv_new(MV_USER);
        v->text = text;
        text = NULL;
        add_view(u, v);
    }
    free(text);
    ui_queue_refresh(u);
}

void iq_flush_deferred(ui_t *u) {
    json_t *m;
    json_arr_foreach(u->deferred_bash, i, m) {
        if (str_eq(json_get_str(m, "role"), "pythonExecution")) app_record_python(u->app, m);
        else app_record_bash(u->app, m);
    }
    json_free(u->deferred_bash);
    u->deferred_bash = NULL;
}

bool iq_run_end(ui_t *u) {
    strvec_free(&u->optimistic);
    iq_flush_deferred(u);
    if (u->bg_kind || u->worker_running) return true; /* e.g. an approved plan's execution turn already started */
    if (u->compact_pending) {
        char *instr = u->compact_pending;
        u->compact_pending = NULL;
        compact_now(u, instr, true);
        free(instr);
        if (u->bg_kind) return true;
        ui_chrome_turn(u, false);
    }
    if (u->resume_pending) { /* `.`/`c` interrupted the turn: continue from where it stopped */
        u->resume_pending = false;
        ix_resume(u);
        if (u->worker_running) return true;
    }
    bool forced = agent_take_forced_delivery(u->app->agent);
    strvec_t st = {0}, fu = {0};
    agent_queue_texts(u->app->agent, &st, &fu);
    bool queued = st.n || fu.n;
    strvec_free(&st);
    strvec_free(&fu);
    if (queued) {
        ix_start_queued_run(u);
        ui_queue_refresh(u);
        return true;
    }
    /* the forcing Enter raced the turn boundary: the loop took the message into the turn it aborted before any output */
    if (forced && app_has_unanswered_prompt(u->app)) {
        ix_resume(u);
        ui_queue_refresh(u);
        if (u->worker_running) return true;
    }
    if (u->yield_compaction) {
        const char *arg = u->yield_compaction + strlen("/compact");
        while (*arg == ' ') arg++;
        char *instr = xstrdup(arg);
        free(u->yield_compaction);
        u->yield_compaction = NULL;
        ui_queue_refresh(u);
        compact_now(u, instr, false);
        free(instr);
        if (u->bg_kind) return true;
        flush_compaction_queue(u);
        return u->worker_running;
    }
    ui_queue_refresh(u);
    return false;
}

void iq_shutdown(ui_t *u) {
    struct ui_bash *runs[2] = {u->bash, u->python};
    for (int i = 0; i < 2; i++) {
        if (!runs[i]) continue;
        runs[i]->cancel = 1;
        pthread_join(runs[i]->th, NULL);
        json_free(runs[i]->result);
        runs[i]->result = NULL; /* bash_done is still queued on the (stopped) loop; the view is owned by the transcript */
    }
    u->bash = u->python = NULL;
    if (u->bg_kind) {
        agent_abort(u->app->agent);
        pthread_join(u->bg_thread, NULL);
        u->bg_kind = 0;
    }
    json_free(u->compaction_queue);
    u->compaction_queue = NULL;
    json_free(u->deferred_bash);
    u->deferred_bash = NULL;
    free(u->yield_compaction);
    u->yield_compaction = NULL;
    free(u->compact_pending);
    u->compact_pending = NULL;
    if (u->after_compact) {
        void (*then)(ui_t *, bool, void *) = u->after_compact;
        u->after_compact = NULL;
        then(u, false, u->after_compact_ud);
        u->after_compact_ud = NULL;
    }
    strvec_free(&u->optimistic);
    strvec_free(&u->q_steer);
    strvec_free(&u->q_follow);
}
