/* loopctl.c - `/loop` mode controller (port of omp InteractiveMode's loop mode: handleLoopCommand,
 * #enableLoopMode, setLoopPrompt, pauseLoop, disableLoopMode, #scheduleLoopAutoSubmit / #runLoopIteration /
 * #passesLoopCondition / #submitLoopPromptWhenReady, as shipped in omp 18.14.2).
 *
 * While loop mode is on, the prompt the user submits while idle (or the inline `/loop … <prompt>`) becomes the
 * loop prompt; 800 ms after every run ends it re-submits, after `loop.mode` compacts the context or starts a
 * new session. A count/duration budget bounds it, and `--while` / `--until` commands (run in the session cwd
 * with `loop.conditionTimeoutMs`) gate each iteration by exit status. Esc pauses the loop (aborting a running
 * turn); the next prompt resumes it; `/loop` again disables it. The mode segment of the status line shows
 * the state (statusline.c). */
#include "modes/interactive.h"

#include "config/settings.h"
#include "core/proc.h"

#include <pthread.h>

#define AUTO_SUBMIT_MS 800

typedef enum { STEP_ITERATION, STEP_SUBMIT } step_t;

/* one condition evaluation (thread); freed on the UI thread once it posted */
typedef struct cond_job {
    ui_t *u;
    pthread_t th;
    char *command, *cwd;
    int64_t timeout_ms;
    volatile sig_atomic_t cancel;
    bool stale;
    /* result */
    int exit_code;
    bool timed_out, cancelled;
    char *spawn_error, *output;
    /* the iteration it gates */
    char *prompt;
    char *action;
} cond_job_t;

static struct {
    loop_state_t st;
    int timer;          /* pending auto-submit */
    step_t step;
    char *step_prompt, *step_action;
    cond_job_t *job;    /* the live evaluation */
    VEC(cond_job_t *) jobs; /* every evaluation not yet joined */
    bool compacting;    /* a loop.mode=compact iteration waits for its compaction */
} g;

static void cancel_auto_submit(ui_t *u) {
    if (g.timer) tui_remove_timer(u->tui, g.timer);
    g.timer = 0;
}

static void abort_condition(void) {
    if (!g.job) return;
    g.job->cancel = 1;
    g.job->stale = true;
    g.job = NULL;
}

/* #syncLoopModeStatus (a duration budget's "left" is recomputed whenever the status line renders; like omp,
 * nothing repaints an idle screen just for it) */
static void sync_status(ui_t *u) {
    u->loop = g.st.enabled ? &g.st : NULL;
    tui_request_render(u->tui);
}

static void clear_state(void) {
    free(g.st.prompt);
    free(g.st.cond.command);
    memset(&g.st, 0, sizeof g.st);
}

/* disableLoopMode */
static void disable(ui_t *u, const char *message) {
    bool was = g.st.enabled;
    clear_state();
    cancel_auto_submit(u);
    abort_condition();
    sync_status(u);
    if (was) add_notice(u, "info", message ? message : "Loop mode disabled.");
}

bool lp_enabled(ui_t *u) {
    (void)u;
    return g.st.enabled;
}

/* setLoopPrompt: a manual submit supersedes any pending gate */
void lp_set_prompt(ui_t *u, const char *prompt) {
    if (!g.st.enabled) return;
    abort_condition();
    free(g.st.prompt);
    g.st.prompt = xstrdup(prompt);
    g.st.paused = false;
    sync_status(u);
}

/* pauseLoop: drop the captured prompt and the pending re-submit; the next prompt resumes */
static void pause_loop(ui_t *u) {
    free(g.st.prompt);
    g.st.prompt = NULL;
    g.st.paused = true;
    cancel_auto_submit(u);
    abort_condition();
    sync_status(u);
}

bool lp_escape(ui_t *u) {
    if (!g.st.enabled) return false;
    /* Esc suspends the loop even mid-iteration: abort the live turn, then drop the captured prompt */
    if (u->worker_running) app_abort_user(u->app);
    pause_loop(u);
    return true;
}

/* the input waiter is armed: nothing runs, nothing is being typed into a picker */
static bool idle(ui_t *u) { return !u->worker_running && !u->bg_kind; }

static void iteration(ui_t *u, const char *action, const char *prompt);
static void submit_when_ready(ui_t *u, const char *prompt);

static void timer_cb(void *ud) {
    ui_t *u = ud;
    g.timer = 0;
    char *prompt = g.step_prompt, *action = g.step_action;
    g.step_prompt = g.step_action = NULL;
    if (g.st.enabled && idle(u) && prompt) {
        if (g.step == STEP_SUBMIT) submit_when_ready(u, prompt);
        else iteration(u, action, prompt);
    }
    free(prompt), free(action);
}

/* #deferLoopAutoSubmit: a brief delay so Esc can land between iterations */
static void defer(ui_t *u, step_t step, const char *action, const char *prompt) {
    cancel_auto_submit(u);
    free(g.step_prompt), free(g.step_action);
    g.step = step;
    g.step_prompt = xstrdup(prompt);
    g.step_action = action ? xstrdup(action) : NULL;
    g.timer = tui_add_timer(u->tui, AUTO_SUBMIT_MS, false, timer_cb, u);
}

static bool current(const char *prompt) { return g.st.enabled && g.st.prompt && str_eq(g.st.prompt, prompt); }

/* #submitLoopPromptWhenReady */
static void submit_when_ready(ui_t *u, const char *prompt) {
    if (!current(prompt)) return;
    if (loop_limit_duration_expired(&g.st.limit, mono_ms())) {
        disable(u, "Loop time limit reached. Loop mode disabled.");
        return;
    }
    if (!idle(u)) {
        defer(u, STEP_SUBMIT, NULL, prompt);
        return;
    }
    if (ix_is_skill_command(u, prompt)) ix_invoke_skill(u, prompt, NULL, false);
    else ix_start_prompt(u, prompt);
}

static void after_compact(ui_t *u, bool ok, void *ud) {
    (void)ok;
    char *prompt = ud;
    g.compacting = false;
    submit_when_ready(u, prompt);
    free(prompt);
}

/* the rest of #loopIteration once the condition passed */
static void iteration_body(ui_t *u, const char *action, const char *prompt) {
    if (!idle(u)) {
        defer(u, STEP_ITERATION, action, prompt);
        return;
    }
    if (!loop_limit_consume(&g.st.limit, mono_ms())) {
        disable(u, "Loop limit reached. Loop mode disabled.");
        return;
    }
    sync_status(u);
    if (str_eq(action, "compact")) {
        g.compacting = true;
        iq_compact_then(u, after_compact, xstrdup(prompt));
        return;
    }
    if (str_eq(action, "reset")) ix_handle_command(u, "/new");
    submit_when_ready(u, prompt);
}

static void cond_done(void *p) {
    cond_job_t *j = p;
    ui_t *u = j->u;
    pthread_join(j->th, NULL);
    for (size_t i = 0; i < g.jobs.n; i++)
        if (g.jobs.v[i] == j) {
            g.jobs.v[i] = g.jobs.v[--g.jobs.n];
            break;
        }
    bool live = !j->stale && g.job == j;
    if (live) g.job = NULL;
    if (live && current(j->prompt) && idle(u)) {
        loop_cond_t c = {j->command, false};
        c.until = g.st.cond.until;
        char *msg = NULL;
        loop_verdict_t v = loop_condition_verdict(&c, j->exit_code, j->timed_out, j->cancelled, j->spawn_error, j->output,
                                                  j->timeout_ms, &msg);
        if (v == LOOP_VERDICT_CONTINUE) iteration_body(u, j->action, j->prompt);
        else if (v != LOOP_VERDICT_ABORTED) disable(u, msg);
        free(msg);
    }
    free(j->command), free(j->cwd), free(j->spawn_error), free(j->output), free(j->prompt), free(j->action);
    free(j);
}

static void *cond_main(void *p) {
    cond_job_t *j = p;
    proc_opts_t o = {0};
    o.cwd = j->cwd;
    o.timeout_ms = j->timeout_ms;
    o.merge_stderr = true;
    o.new_session = true;
    o.max_output = 1 << 20;
    o.cancel = &j->cancel;
    proc_result_t r = {0};
    int rc = proc_shell(j->command, &o, &r);
    if (r.spawn_errno) j->spawn_error = xstrdup(strerror(r.spawn_errno));
    else if (rc < 0 && !r.timed_out && !r.cancelled) j->spawn_error = xstrdup("failed to start");
    j->exit_code = r.term_signal ? -1 : r.exit_code;
    j->timed_out = r.timed_out;
    j->cancelled = r.cancelled || (j->cancel && !r.timed_out);
    j->output = xstrdup(r.out ? r.out : "");
    proc_result_free(&r);
    tui_post(j->u->tui, cond_done, j);
    return NULL;
}

/* #loopIteration */
static void iteration(ui_t *u, const char *action, const char *prompt) {
    if (!current(prompt)) return;
    if (!idle(u)) {
        defer(u, STEP_ITERATION, action, prompt);
        return;
    }
    /* an exhausted budget ends the loop before the condition runs one last time for nothing */
    if (loop_limit_exhausted(&g.st.limit, mono_ms())) {
        disable(u, "Loop limit reached. Loop mode disabled.");
        return;
    }
    if (!g.st.has_cond) {
        iteration_body(u, action, prompt);
        return;
    }
    abort_condition();
    cond_job_t *j = xcalloc(1, sizeof *j);
    j->u = u;
    j->command = xstrdup(g.st.cond.command);
    j->cwd = xstrdup(u->app->cwd);
    j->timeout_ms = MAX(0, settings_int("loop.conditionTimeoutMs", 30000));
    j->prompt = xstrdup(prompt);
    j->action = xstrdup(action);
    if (pthread_create(&j->th, NULL, cond_main, j) != 0) {
        free(j->command), free(j->cwd), free(j->prompt), free(j->action);
        free(j);
        disable(u, "Loop condition could not run. Loop mode disabled.");
        return;
    }
    vec_push(&g.jobs, j);
    g.job = j;
}

/* #scheduleLoopAutoSubmit: a run ended with nothing else queued; true when loop mode owns the idle turn */
bool lp_run_end(ui_t *u) {
    if (!g.st.enabled) return false;
    cancel_auto_submit(u);
    /* a terminal provider failure is not a loop yield: resubmitting the captured prompt would append another user
     * turn (and every @file attachment) without making progress */
    const json_t *last = NULL, *m;
    pthread_mutex_lock(&u->app->agent->mu);
    json_arr_foreach(u->app->agent->messages, i, m) if (str_eq(json_get_str(m, "role"), "assistant")) last = m;
    bool failed = last && str_eq(json_get_str(last, "stopReason"), "error");
    pthread_mutex_unlock(&u->app->agent->mu);
    if (failed && g.st.prompt) {
        pause_loop(u);
        return true;
    }
    if (g.st.prompt && !g.compacting) {
        const char *action = settings_str("loop.mode", "prompt");
        if (!str_eq(action, "compact") && !str_eq(action, "reset")) action = "prompt";
        defer(u, STEP_ITERATION, action, g.st.prompt);
    }
    return true;
}

/* handleLoopCommand: toggle; an inline prompt comes back (owned) for the normal submit flow */
char *lp_command(ui_t *u, const char *args) {
    if (g.st.enabled) {
        disable(u, NULL);
        return NULL;
    }
    loop_args_t a;
    char *err = loop_parse_args(args, &a);
    if (err) {
        add_notice(u, "error", err);
        free(err);
        return NULL;
    }
    /* #enableLoopMode */
    clear_state();
    g.st.enabled = true;
    loop_limit_init(&g.st.limit, a.limit_kind, a.amount, mono_ms());
    if (a.has_cond) {
        g.st.has_cond = true;
        g.st.cond.command = xstrdup(a.cond.command);
        g.st.cond.until = a.cond.until;
    }
    sync_status(u);
    char *key = ui_editor_key("app.interrupt");
    char *m = loop_enabled_message(&a, &g.st.limit, key);
    add_notice(u, "info", m);
    free(m), free(key);
    char *prompt = a.prompt ? xstrdup(a.prompt) : NULL;
    loop_args_free(&a);
    return prompt;
}

void lp_shutdown(ui_t *u) {
    cancel_auto_submit(u);
    abort_condition();
    for (size_t i = 0; i < g.jobs.n; i++) {
        g.jobs.v[i]->cancel = 1;
        pthread_join(g.jobs.v[i]->th, NULL); /* its posted cond_done never runs: the loop has stopped */
    }
    vec_free(&g.jobs);
    free(g.step_prompt), free(g.step_action);
    clear_state();
    u->loop = NULL;
    memset(&g, 0, sizeof g);
}
