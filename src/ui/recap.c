/* recap.c - the idle recap (port of omp session/idle-maintenance.ts recap half + event-controller.ts
 * presentIdleRecap + tui chat/recap-notice.ts).
 *
 * A stretch of activity ends when a run settles. If the run produced output (its last message is an assistant
 * message with text, thinking or a tool call) and `recap.enabled` (default on), a timer of `recap.idleSeconds`
 * (default 240, clamped to 1..3600) is armed once the session is quiet. When it fires and the session is still
 * quiet and unblocked (no draft in the editor, no compaction), an ephemeral side turn on the session model asks
 * for the recap (`recap-user.md`, anchored on the goal or session name and the active todo task); a nonblank reply
 * is shown in the transcript as a dim italic `※ recap: …` line (flattened, at most 280 cells). New activity, a
 * session transition or teardown invalidates everything captured before it, including an in-flight request.
 *
 * Not ported: omp journals each recap to history.db (`session_recaps`), which omc does not have.
 */
#include "ui/signals.h"

#include "coding/goal.h"
#include "config/settings.h"
#include "prompt/template.h"
#include "ui/ui.h"

#include <ctype.h>
#include <math.h>

#define RECAP_MAX_CELLS 280 /* TRUNCATE_LENGTHS.RECAP */

typedef struct {
    ui_t *u;
    int gen;
    volatile sig_atomic_t cancel;
    char *prompt;
    char *reply; /* result */
} recap_job_t;

/* threads still running a recap request: teardown waits for them (cancelled) before the UI and app go away */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_threads;
static bool g_closing;

static struct {
    ui_t *u;
    bool ready;
    bool pending;  /* the current stretch still owes a recap */
    int gen;       /* bumped on invalidation */
    int timer;
    recap_job_t *run; /* in flight (owned by its thread until it posts back) */
} R;

bool ui_recap_generated(const json_t *messages) {
    size_t n = json_len(messages);
    if (!n) return false;
    const json_t *last = json_at(messages, n - 1);
    if (!str_eq(json_get_str(last, "role"), "assistant")) return false;
    json_t *b;
    json_arr_foreach(json_get_arr(last, "content"), i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "text") && !str_empty(json_get_str(b, "text"))) return true;
        if (str_eq(t, "thinking") && !str_empty(json_get_str(b, "thinking"))) return true;
        if (str_eq(t, "toolCall")) return true;
    }
    return false;
}

static void clear_timer(void) {
    if (R.timer && R.u) tui_remove_timer(R.u->tui, R.timer);
    R.timer = 0;
}

static void cancel_run(void) {
    if (R.run) R.run->cancel = 1; /* its result is dropped: the generation no longer matches */
    R.run = NULL;
}

void ui_recap_invalidate(void) {
    R.pending = false;
    R.gen++;
    clear_timer();
    cancel_run();
}

static bool recap_enabled(void) { return settings_bool("recap.enabled", true); }

/* isBlocked: startup unfinished, a draft being composed, compaction under way */
static bool blocked(void) {
    ui_t *u = R.u;
    if (!u || !R.ready) return true;
    if (u->app->compacting || u->bg_kind) return true;
    char *draft = tui_editor_get_text(u->editor);
    char *t = str_trim(draft ? draft : "");
    bool composing = *t != 0;
    free(t);
    free(draft);
    return composing;
}

/* isQuiet: nothing live, admitted, queued or scheduled (a goal / advisor continuation) */
static bool quiet(void) {
    ui_t *u = R.u;
    return !u->worker_running && agent_queued_count(u->app->agent) == 0 && !u->goal_cont && !u->bash && !u->python;
}

static void schedule(void);

static char *goal_text(app_t *a) {
    const goal_t *g = app_goal(a);
    const char *o = g ? goal_objective(g) : NULL;
    char *t = o ? str_trim(o) : NULL;
    if (t && *t) return t;
    free(t);
    t = str_trim(a->session && a->session->title ? a->session->title : "");
    if (*t) return t;
    free(t);
    return NULL;
}

/* nextActionableTask: the first in-progress task, else the first pending one */
static const char *next_task(app_t *a) {
    const char *first_pending = NULL;
    json_t *ph, *t;
    json_arr_foreach(a->env ? a->env->todo.phases : NULL, i, ph) {
        json_arr_foreach(json_get_arr(ph, "tasks"), k, t) {
            const char *st = json_get_str(t, "status");
            if (str_eq(st, "in_progress")) return json_get_str(t, "content");
            if (!first_pending && str_eq(st, "pending")) first_pending = json_get_str(t, "content");
        }
    }
    return first_pending;
}

/* presentIdleRecap: RecapNotice(previewLine(reply, RECAP)) */
static void present(ui_t *u, const char *reply) {
    sb_t b = SB_INIT;
    bool space = false;
    for (const char *p = reply; *p; p++) { /* flattenLine: \s+ -> " ", trim */
        if (isspace((unsigned char)*p)) {
            space = true;
            continue;
        }
        if (space && b.len) sb_putc(&b, ' ');
        space = false;
        sb_putc(&b, *p);
    }
    char *flat = sb_detach(&b);
    char *line = tui_truncate_to_width(flat, RECAP_MAX_CELLS, "…");
    free(flat);
    if (*line) {
        char *body = xasprintf("\x1b[3m※ recap: %s\x1b[23m", line);
        char *styled = tui_fgs(NULL, TUI_TC_DIM, body);
        tui_container_add(u->chat, &tui_text_new(styled, 1, 0)->base);
        free(styled);
        free(body);
        tui_request_render(u->tui);
    }
    free(line);
}

static void run_done(void *ud) {
    recap_job_t *j = ud;
    bool current = j->gen == R.gen && !j->cancel && R.run == j;
    if (current) R.run = NULL;
    if (current && !blocked()) {
        if (!quiet()) schedule();
        else {
            R.pending = false; /* #complete(pending, "recap") */
            if (j->reply && *j->reply) present(j->u, j->reply);
        }
    }
    free(j->prompt);
    free(j->reply);
    free(j);
}

static void *run_main(void *p) {
    recap_job_t *j = p;
    app_ephemeral_t e = {0};
    e.prompt_text = j->prompt;
    e.cancel = &j->cancel;
    char *reply = NULL, *err = NULL;
    json_t *msg = app_ephemeral_turn(j->u->app, &e, &reply, &err);
    json_free(msg);
    free(err);
    j->reply = reply;
    pthread_mutex_lock(&g_mu);
    if (g_closing) {
        free(j->prompt);
        free(j->reply);
        free(j);
    } else {
        tui_post(j->u->tui, run_done, j);
    }
    g_threads--;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static void start_run(void) {
    ui_t *u = R.u;
    app_t *a = u->app;
    pthread_mutex_lock(&a->agent->mu);
    bool generated = ui_recap_generated(a->agent->messages);
    pthread_mutex_unlock(&a->agent->mu);
    if (!a->model || !generated) {
        R.pending = false;
        return;
    }
    json_t *ctx = json_obj();
    char *goal = goal_text(a);
    const char *task = next_task(a);
    json_set_str(ctx, "goal", goal ? goal : "");
    json_set_str(ctx, "task", task ? task : "");
    free(goal);
    recap_job_t *j = xcalloc(1, sizeof *j);
    j->u = u;
    j->gen = R.gen;
    j->prompt = prompt_render("coding-agent/prompts/system/recap-user.md", ctx);
    json_free(ctx);
    if (!j->prompt) {
        free(j);
        R.pending = false;
        return;
    }
    pthread_t th;
    R.run = j;
    pthread_mutex_lock(&g_mu);
    g_threads++;
    pthread_mutex_unlock(&g_mu);
    if (pthread_create(&th, NULL, run_main, j) != 0) {
        pthread_mutex_lock(&g_mu);
        g_threads--;
        pthread_mutex_unlock(&g_mu);
        R.run = NULL;
        free(j->prompt);
        free(j);
        R.pending = false;
        return;
    }
    pthread_detach(th);
}

static void on_due(void *ud) {
    (void)ud;
    R.timer = 0;
    if (!R.pending || R.run) return;
    if (blocked()) return; /* blocked work stays owed: a refresh re-arms it */
    if (!quiet()) {
        schedule();
        return;
    }
    start_run();
}

/* #schedule / #armRecap */
static void schedule(void) {
    clear_timer();
    if (!R.u || !R.pending || R.run || !recap_enabled()) return;
    if (blocked() || !quiet()) return;
    double secs = settings_num("recap.idleSeconds", 240);
    if (!isfinite(secs)) secs = 240;
    secs = MAX(1, MIN(3600, secs));
    R.timer = tui_add_timer(R.u->tui, (int)(secs * 1000), false, on_due, NULL);
}

void ui_recap_init(ui_t *u) {
    R.u = u;
    R.ready = true;
}

void ui_recap_settled(const json_t *run_messages) {
    ui_recap_invalidate();
    /* nothing was generated: the stretch owes no recap, however its blockers change later */
    R.pending = ui_recap_generated(run_messages);
    schedule();
}

void ui_recap_refresh(void) {
    if (!R.pending) return;
    if (R.run && !(recap_enabled() && !blocked() && quiet())) cancel_run();
    schedule();
}

void ui_recap_dispose(void) {
    recap_job_t *run = R.run;
    ui_recap_invalidate();
    if (run) run->cancel = 1;
    pthread_mutex_lock(&g_mu);
    g_closing = true;
    while (g_threads > 0) pthread_cond_wait(&g_cv, &g_mu);
    g_closing = false;
    pthread_mutex_unlock(&g_mu);
    R.ready = false;
    R.u = NULL;
}
