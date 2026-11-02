/* interactive mode: TUI chat (port of omp's InteractiveMode, core feature set). */
#include "modes.h"
#include "ui/ui.h"
#include "coding/migrate.h"
#include "coding/goal.h"
#include "coding/memory.h"
#include "mcp/mcp.h"
#include "tools/jobs.h"
#include "prompt/template.h"
#include <unistd.h>

#include "config/auth.h"
#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "tui/ansi.h"
#include "tui/components.h"
#include "tui/editor.h"
#include "tui/markdown.h"
#include "tui/render.h"
#include "tui/theme.h"

#include <pthread.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/wait.h>
#include "ui/modelbrowser.h"
#include "ui/tools/tc.h"
#include "coding/title.h"

static void maybe_plan_approval(ui_t *u, const char *tool, const json_t *result, bool is_error);

/* ---------- agent events -> UI ---------- */

typedef struct {
    ui_t *u;
    agent_event_type_t type;
    json_t *msg;
    char *id, *name;
    json_t *args;
    char *out;
    json_t *result; /* tool result {content, details} */
    bool is_error;
} uiev_t;

static void uiev_free(uiev_t *e) {
    json_free(e->msg);
    free(e->id);
    free(e->name);
    json_free(e->args);
    free(e->out);
    json_free(e->result);
    free(e);
}

static void ui_apply_partial(void *ud) {
    ui_t *u = ud;
    pthread_mutex_lock(&u->mu);
    json_t *p = u->pending_partial;
    u->pending_partial = NULL;
    u->partial_posted = false;
    pthread_mutex_unlock(&u->mu);
    if (!p) return;
    if (!u->cur_assistant) {
        u->cur_assistant = mv_new(MV_ASSISTANT);
        u->cur_assistant->msg = json_obj();
        u->cur_assistant->streaming = true;
        add_view(u, u->cur_assistant);
    }
    json_free(u->cur_assistant->msg);
    u->cur_assistant->msg = p;
    u->cur_assistant->dirty = true;
    ui_assistant_streamed(u, u->cur_assistant);
    toolcard_note_message(u, p, false);
    /* omp draws a tool call's card while its arguments stream */
    json_t *blk;
    json_arr_foreach(json_get_arr(p, "content"), i, blk) {
        if (!str_eq(json_get_str(blk, "type"), "toolCall")) continue;
        toolcard_preview(u, json_get_str(blk, "id"), json_get_str(blk, "name"), json_get(blk, "arguments"));
    }
    tui_request_render(u->tui);
}

static void ui_apply_event(void *ud) {
    uiev_t *e = ud;
    ui_t *u = e->u;
    switch (e->type) {
    case AGENT_EV_MESSAGE_END:
        if (str_eq(json_get_str(e->msg, "role"), "user")) ui_async_result(u, e->msg);
        if (str_eq(json_get_str(e->msg, "role"), "assistant")) {
            ui_apply_partial(u);
            if (!u->cur_assistant) {
                u->cur_assistant = mv_new(MV_ASSISTANT);
                add_view(u, u->cur_assistant);
            }
            json_free(u->cur_assistant->msg);
            u->cur_assistant->msg = e->msg;
            e->msg = NULL;
            ui_assistant_end(u, u->cur_assistant);
            toolcard_note_message(u, u->cur_assistant->msg, true);
            u->cur_assistant = NULL;
        }
        break;
    case AGENT_EV_TOOL_START: {
        msgview_t *v = toolcard_start(u, e->id, e->name, e->args ? e->args : json_obj());
        e->args = NULL;
        hmap_put(&u->tools, e->id, v);
        ui_tool_started(u, e->id);
        break;
    }
    case AGENT_EV_TOOL_UPDATE:
    case AGENT_EV_TOOL_END: {
        msgview_t *v = hmap_get(&u->tools, e->id);
        if (!v) break;
        free(v->output);
        v->output = e->out;
        e->out = NULL;
        json_t *pres = e->type == AGENT_EV_TOOL_END && u->app->plan_review_ui ? json_clone(e->result) : NULL;
        toolcard_result(u, v, e->id, e->result, e->type != AGENT_EV_TOOL_END, e->is_error);
        e->result = NULL;
        if (e->type == AGENT_EV_TOOL_END) hmap_del(&u->tools, e->id);
        if (pres) maybe_plan_approval(u, e->name, pres, e->is_error);
        json_free(pres);
        break;
    }
    case AGENT_EV_AGENT_END:
        ui_working_stop(u);
        ui_chrome_turn(u, false);
        break;
    case AGENT_EV_AGENT_START:
        toolcard_break_group(u);
        ui_chrome_turn(u, true);
        ui_agent_start(u);
        break;
    default: break;
    }
    tui_request_render(u->tui);
    uiev_free(e);
}

static char *result_display(const char *tool, const tool_result_t *r) {
    if (r && r->details && (!strcmp(tool, "edit") || !strcmp(tool, "write"))) {
        const char *d = json_get_str(r->details, "diff");
        if (d && *d) return xstrdup(d);
    }
    return r ? tool_result_joined_text(r) : xstrdup("");
}

static void ui_listener(void *ud, const agent_event_t *ev) {
    ui_t *u = ud;
    if (ev->type == AGENT_EV_MESSAGE_UPDATE) {
        json_t *c = json_clone(ev->message);
        pthread_mutex_lock(&u->mu);
        json_free(u->pending_partial);
        u->pending_partial = c;
        bool post = !u->partial_posted;
        u->partial_posted = true;
        pthread_mutex_unlock(&u->mu);
        if (post) tui_post(u->tui, ui_apply_partial, u);
        return;
    }
    if (ev->type != AGENT_EV_MESSAGE_END && ev->type != AGENT_EV_TOOL_START && ev->type != AGENT_EV_TOOL_UPDATE &&
        ev->type != AGENT_EV_TOOL_END && ev->type != AGENT_EV_AGENT_END && ev->type != AGENT_EV_AGENT_START)
        return;
    uiev_t *e = xcalloc(1, sizeof(*e));
    e->u = u;
    e->type = ev->type;
    if (ev->message) e->msg = json_clone(ev->message);
    if (ev->tool_call_id) e->id = xstrdup(ev->tool_call_id);
    if (ev->tool_name) e->name = xstrdup(ev->tool_name);
    if (ev->args) e->args = json_clone(ev->args);
    if (ev->result) {
        e->out = result_display(ev->tool_name ? ev->tool_name : "", ev->result);
        e->result = json_obj();
        json_set(e->result, "content", ev->result->content ? json_clone(ev->result->content) : json_arr());
        if (ev->result->details) json_set(e->result, "details", json_clone(ev->result->details));
    }
    e->is_error = ev->is_error;
    tui_post(u->tui, ui_apply_event, e);
}

static void post_notice(void *x);

/* interactive-mode.ts showModelCycleTrack: spacer + the cycleOrder segment track, cleared after 4 s */
static void model_cycle_clear(void *ud) {
    ui_t *u = ud;
    u->model_cycle_timer = 0;
    tui_container_clear(u->model_cycle);
    tui_request_render(u->tui);
}

static void show_model_cycle_track(ui_t *u, int active) {
    strvec_t order = app_cycle_order();
    tui_container_clear(u->model_cycle);
    tui_container_add(u->model_cycle, &tui_spacer_new(1)->base);
    char *track = ui_segment_track((const char *const *)order.v, order.n, active);
    tui_container_add(u->model_cycle, &tui_text_new(track, 1, 0)->base);
    free(track);
    strvec_free(&order);
    if (u->model_cycle_timer) tui_remove_timer(u->tui, u->model_cycle_timer);
    u->model_cycle_timer = tui_add_timer(u->tui, 4000, false, model_cycle_clear, u);
}

static void ui_notice(void *ud, const char *level, const char *text) {
    ui_t *u = ud;
    uiev_t *e = xcalloc(1, sizeof(*e));
    e->u = u;
    e->type = (agent_event_type_t)-1;
    e->name = xstrdup(level);
    e->out = xstrdup(text);
    tui_post(u->tui, post_notice, e);
}

static void post_notice(void *x) {
    uiev_t *e = x;
    if (!strcmp(e->name, "retry-recovered")) {
        ui_retry_recovered(e->u, e->out);
        tui_request_render(e->u->tui);
        uiev_free(e);
        return;
    }
    if (!strcmp(e->name, "retry-failed")) {
        char *f1 = strchr(e->out, '\x1f'), *f2 = f1 ? strchr(f1 + 1, '\x1f') : NULL;
        if (f1 && f2) {
            *f1 = *f2 = 0;
            ui_retry_failed(e->u, atoi(e->out), f1 + 1, f2 + 1);
        }
        tui_request_render(e->u->tui);
        uiev_free(e);
        return;
    }
    if (!strcmp(e->name, "retry_fallback_applied")) {
        char *m = app_fallback_notice_text(e->out);
        add_notice(e->u, "warning", m);
        free(m);
        tui_request_render(e->u->tui);
        uiev_free(e);
        return;
    }
    if (!strcmp(e->name, "retry_fallback_succeeded")) {
        /* text: "<selector>\x1f<role>" */
        char *sep = strchr(e->out, '\x1f');
        if (sep) *sep = 0;
        char *m = xasprintf("Fallback succeeded on %s", e->out);
        add_notice(e->u, "info", m);
        free(m);
        tui_request_render(e->u->tui);
        uiev_free(e);
        return;
    }
    const char *lvl = !strcmp(e->name, "error") ? "error" : !strcmp(e->name, "warning") ? "warning" : "info";
    /* a generated session title only shows in the status band / working row, as in omp */
    if (strcmp(e->name, "title")) add_notice(e->u, lvl, e->out);
    tui_request_render(e->u->tui);
    uiev_free(e);
}

/* ---------- worker ---------- */

typedef struct {
    ui_t *u;
    char *text;
    const char *hidden; /* custom type of a hidden prompt (goal continuation, advisor), NULL for user input */
} job_t;

static void start_hidden(ui_t *u, const char *text, const char *ctype);

static void goal_continue_cb(void *ud) {
    ui_t *u = ud;
    char *c = u->goal_cont;
    u->goal_cont = NULL;
    if (!c) return;
    char *draft = tui_editor_get_text(u->editor);
    bool busy = u->worker_running || (draft && *draft) || u->picker_kind;
    free(draft);
    if (!busy) start_hidden(u, c, u->cont_type ? u->cont_type : "goal-continuation");
    free(c);
}

static void plan_exec_resume(ui_t *u);
static void worker_done(void *ud) {
    ui_t *u = ud;
    pthread_join(u->worker, NULL);
    u->worker_running = false;
    plan_exec_resume(u);
    if (u->goal_cont) tui_add_timer(u->tui, 800, false, goal_continue_cb, u);
    tui_request_render(u->tui);
}

static void *worker_main(void *p) {
    job_t *j = p;
    app_t *a = j->u->app;
    size_t before = json_len(a->agent->messages);
    if (j->hidden) app_prompt_hidden(a, j->text, j->hidden);
    else app_prompt(a, j->text, NULL);
    json_t *run = json_arr();
    pthread_mutex_lock(&a->agent->mu);
    for (size_t i = before; i < json_len(a->agent->messages); i++) json_push(run, json_clone(json_at(a->agent->messages, i)));
    pthread_mutex_unlock(&a->agent->mu);
    const char *ctype = NULL;
    char *cont = app_after_run(a, run, j->hidden, "interactive", &ctype);
    json_free(run);
    free(j->u->goal_cont);
    j->u->goal_cont = cont;
    j->u->cont_type = ctype;
    tui_post(j->u->tui, worker_done, j->u);
    free(j->text);
    free(j);
    return NULL;
}

static void *retry_main(void *p) {
    ui_t *u = p;
    char *err = NULL;
    if (app_retry(u->app, &err) != 0 && err) ui_notice(u, "warning", err);
    free(err);
    tui_post(u->tui, worker_done, u);
    return NULL;
}

typedef struct {
    ui_t *u;
    char *q;
} btw_t;

static void *btw_main(void *p) {
    btw_t *b = p;
    char *err = NULL;
    char *ans = app_side_question(b->u->app, b->q, &err);
    char *msg = ans ? xasprintf("btw: %s\n%s", b->q, ans) : xasprintf("btw failed: %s", err ? err : "no answer");
    ui_notice(b->u, ans ? "info" : "error", msg);
    free(msg);
    free(ans);
    free(err);
    free(b->q);
    free(b);
    return NULL;
}

/* a background job settled while idle: deliver its result as a hidden turn */
static void job_wake_cb(void *ud) {
    ui_t *u = ud;
    if (u->worker_running || u->picker_kind) return; /* worker_done delivers at the end of the run */
    char *draft = tui_editor_get_text(u->editor);
    bool typing = draft && *draft;
    free(draft);
    if (typing) return;
    char *t = app_job_results_text(u->app);
    if (t) start_hidden(u, t, "async-result");
    free(t);
}

/* background job frame for a returned tool call (job thread -> UI thread) */
typedef struct {
    ui_t *u;
    char *id;
    json_t *details;
} frame_t;

static void frame_apply(void *p) {
    frame_t *f = p;
    toolcard_frame(f->u, f->id, f->details);
    free(f->id);
    free(f);
}

static void ui_tool_frame(void *ud, const char *tool_call_id, json_t *details) {
    ui_t *u = ud;
    frame_t *f = xcalloc(1, sizeof *f);
    f->u = u;
    f->id = xstrdup(tool_call_id ? tool_call_id : "");
    f->details = details;
    tui_post(u->tui, frame_apply, f);
}

static void ui_wake(void *ud) {
    ui_t *u = ud;
    tui_post(u->tui, job_wake_cb, u);
}

static void start_hidden(ui_t *u, const char *text, const char *ctype) {
    job_t *j = xcalloc(1, sizeof(*j));
    j->u = u;
    j->text = xstrdup(text);
    j->hidden = ctype;
    u->worker_running = true;
    if (pthread_create(&u->worker, NULL, worker_main, j) != 0) {
        u->worker_running = false;
        free(j->text);
        free(j);
    }
}

static void start_prompt(ui_t *u, const char *text) {
    if (!u->app->model) { /* omp AgentSession.prompt: "No model selected." */
        char *agent_dir = omc_agent_path("");
        char *m = xasprintf("No model selected.\n\nUse /login, set an API key environment variable, or create %s/models.yml\n\n"
                            "Then use /model to select a model.",
                            agent_dir);
        add_notice(u, "error", m);
        free(m);
        free(agent_dir);
        return;
    }
    msgview_t *v = mv_new(MV_USER);
    v->text = xstrdup(text);
    add_view(u, v);
    job_t *j = xcalloc(1, sizeof(*j));
    j->u = u;
    j->text = xstrdup(text);
    u->worker_running = true;
    if (pthread_create(&u->worker, NULL, worker_main, j) != 0) {
        u->worker_running = false;
        add_notice(u, "error", "failed to start agent thread");
        free(j->text);
        free(j);
    }
}

/* #runNewSessionFlow: reset the transcript and present `✔ New session started` */
static void ui_new_session_banner(ui_t *u) {
    tui_container_clear(u->chat);
    char *l = xasprintf("%s New session started", tui_sym("status.success"));
    char *t = tui_fgs(NULL, TUI_TC_ACCENT, l);
    tui_container_add(u->chat, &tui_text_new(t, 1, 1)->base);
    free(t);
    free(l);
    tui_request_full_render(u->tui);
}

/* #reconcileModeFromSession: re-enter plan mode recorded in the session (#enterPlanMode's status) */
static void ui_plan_restore(ui_t *u) {
    if (app_plan_restore_from_session(u->app) == 1) {
        char *m = xasprintf("Plan mode enabled. Plan file: %s", u->app->plan_file);
        add_notice(u, "info", m);
        free(m);
    }
}

/* ---------- pickers ---------- */

static void picker_close(ui_t *u) {
    if (u->picker) tui_overlay_hide(u->tui, &u->picker->base);
    u->picker_kind = 0;
    json_free(u->picker_data);
    u->picker_data = NULL;
    tui_set_focus(u->tui, &u->editor->base);
}


/* rebuild the transcript view from the session */
static void reload_chat(ui_t *u) { ui_chat_rebuild(u); }

/* move the session leaf; a user message is re-opened in the editor (its parent becomes the leaf) */
static void branch_pick(ui_t *u, const char *entry_id) {
    app_t *a = u->app;
    json_t *e = session_get(a->session, entry_id);
    if (!e) return;
    json_t *msg = json_get(e, "message");
    bool user = str_eq(json_get_str(e, "type"), "message") && str_eq(json_get_str(msg, "role"), "user");
    char *text = user ? ai_message_text(msg) : NULL;
    const char *target = user ? json_get_str(e, "parentId") : entry_id;
    char *tgt = target ? xstrdup(target) : NULL;
    if (app_branch_to(a, tgt) != 0) {
        add_notice(u, "warning", "Cannot switch branches while the agent is running.");
    } else {
        reload_chat(u);
        if (text) tui_editor_set_text(u->editor, text);
        add_notice(u, "info", user ? "Branched: edit and resend the message to continue on a new branch" : "Switched branch");
    }
    free(tgt);
    free(text);
}

/* resume another session file (session selector) */
void ui_resume_session(ui_t *u, const char *path) {
    char *err = NULL;
    if (app_switch_session(u->app, path, &err) != 0) add_notice(u, "error", err);
    else {
        reload_chat(u);
        add_notice(u, "info", "Session resumed");
        ui_plan_restore(u);
    }
    free(err);
    tui_request_render(u->tui);
}

static void on_pick(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    ui_t *u = ud;
    UNUSED(l);
    int kind = u->picker_kind;
    char *val = xstrdup(it->value);
    picker_close(u);
    if (kind == 1) {
        char *err = NULL;
        if (app_set_model(u->app, val, &err) != 0) add_notice(u, "error", err);
        else {
            char *m = xasprintf("Model: %s", val);
            add_notice(u, "info", m);
            free(m);
        }
        free(err);
    } else if (kind == 2) {
        char *err = NULL;
        if (app_switch_session(u->app, val, &err) != 0) add_notice(u, "error", err);
        else {
            reload_chat(u);
            add_notice(u, "info", "Session resumed");
        }
        free(err);
    } else if (kind == 4) {
        ai_effort_t e;
        if (effort_parse(val, &e) == 0) app_set_thinking(u->app, e);
    } else if (kind == 5) {
        branch_pick(u, val);
    }
    free(val);
    tui_request_render(u->tui);
}

static void on_pick_cancel(tui_select_list_t *l, void *ud) {
    ui_t *u = ud;
    UNUSED(l);
    picker_close(u);
}

static void ensure_picker(ui_t *u) {
    if (!u->picker) {
        u->picker = tui_select_list_new(10);
        u->picker->on_select = on_pick;
        u->picker->on_cancel = on_pick_cancel;
        u->picker->ud = u;
    }
}

static void show_picker(ui_t *u, int kind) {
    ensure_picker(u);
    u->picker_kind = kind;
    tui_select_list_set_filter(u->picker, NULL);
    tui_overlay_opts_t o = {TUI_ANCHOR_BOTTOM, -80, 14, 0, 1};
    tui_overlay_show(u->tui, &u->picker->base, &o);
}


/* ---------- ask bridge (tool thread blocks; the UI thread runs the ask dialog) ---------- */

static void ask_done(void *ud, json_t *answers) {
    ui_t *u = ud;
    pthread_mutex_lock(&u->mu);
    json_free(u->ask_answers);
    u->ask_answers = answers;
    u->ask_done = true;
    pthread_cond_signal(&u->ask_cv);
    pthread_mutex_unlock(&u->mu);
    tui_request_render(u->tui);
}

static void ask_start(void *ud) {
    ui_t *u = ud;
    ui_ask_dialog_show(u, u->ask_questions, ask_done, u);
}

static json_t *ui_ask(void *ud, const json_t *questions) {
    ui_t *u = ud;
    pthread_mutex_lock(&u->mu);
    u->ask_questions = json_clone(questions);
    u->ask_answers = NULL;
    u->ask_done = false;
    pthread_mutex_unlock(&u->mu);
    tui_post(u->tui, ask_start, u);
    pthread_mutex_lock(&u->mu);
    while (!u->ask_done) pthread_cond_wait(&u->ask_cv, &u->mu);
    json_t *ans = u->ask_answers;
    u->ask_answers = NULL;
    json_free(u->ask_questions);
    u->ask_questions = NULL;
    pthread_mutex_unlock(&u->mu);
    return ans;
}

/* ---------- plan approval (InteractiveMode.handlePlanApproval / #approvePlan / #savePlanAndQuit) ---------- */

#define PLAN_SAVE_AND_QUIT "Save and quit"
#define PLAN_KEEP_CONTEXT_INDEX 2
#define PLAN_KEEP_DISABLE_PERCENT 95.0

typedef struct {
    ui_t *u;
    ui_plan_review_t *ov;
    bool open, committed;
    char *path, *title, *edited, *feedback, *keep_label, *ann_key, *choice;
    app_role_model_t *cycle;
    size_t ncycle;
    int start_tier, sel_tier;
    bool slider;
    bool exec_pending; /* approval waiting for the aborted run to finish */
    ui_plan_save_t *save;
    char *save_content;
} plan_review_t;

static plan_review_t g_pr;
static char *g_pending_review_path, *g_pending_review_title;

typedef struct {
    ui_t *u;
    char *label;
} pr_choice_t;

static json_t *g_plan_ann; /* resolved plan path -> PlanReviewAnnotationState */

static void plan_status(ui_t *u, const char *text) { add_notice(u, "info", text); }

/* $VISUAL / $EDITOR on a temp file with the TUI stopped (utils/external-editor.ts openInEditor); owned
 * result, NULL when unchanged / failed */
static char *ui_external_edit(ui_t *u, const char *text, const char *ext, bool *no_editor) {
    const char *ed = getenv("VISUAL");
    if (!ed || !*ed) ed = getenv("EDITOR");
    *no_editor = !ed || !*ed;
    if (*no_editor) return NULL;
    char tmpl[] = "/tmp/omc-edit-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    close(fd);
    char *path = xasprintf("%s%s", tmpl, ext ? ext : ".md");
    rename(tmpl, path);
    fs_write(path, text, strlen(text));
    tui_term_stop(tui_term(u->tui));
    tui_term_set_loop_active(false);
    char *cmd = xasprintf("%s \"$1\"", ed);
    const char *argv[] = {"/bin/sh", "-c", cmd, "sh", path, NULL};
    pid_t pid = fork();
    int status = -1;
    if (pid == 0) {
        execv("/bin/sh", (char *const *)argv);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, &status, 0);
    tui_term_set_loop_active(true);
    tui_term_start(tui_term(u->tui));
    tui_term_query_size(tui_term(u->tui));
    if (tui_overlay_visible(u->tui) && g_pr.open) tui_term_puts(tui_term(u->tui), "\x1b[?1049h");
    tui_request_full_render(u->tui);
    char *out = NULL;
    if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        size_t n;
        out = fs_read(path, &n);
    }
    unlink(path);
    free(path);
    free(cmd);
    return out;
}

static void plan_review_free_state(void) {
    free(g_pr.path);
    free(g_pr.title);
    free(g_pr.edited);
    free(g_pr.feedback);
    free(g_pr.keep_label);
    free(g_pr.ann_key);
    free(g_pr.choice);
    app_role_model_cycle_free(g_pr.cycle, g_pr.ncycle);
    ui_t *u = g_pr.u;
    memset(&g_pr, 0, sizeof g_pr);
    g_pr.u = u;
}

static void plan_review_hide(ui_t *u) {
    if (!g_pr.open) return;
    g_pr.open = false;
    tui_component_t *c = ui_plan_review_component(g_pr.ov);
    tui_overlay_hide(u->tui, c);
    tui_term_puts(tui_term(u->tui), "\x1b[?1049l");
    tui_set_focus(u->tui, &u->editor->base);
    tui_request_full_render(u->tui);
    tui_component_destroy(c);
    g_pr.ov = NULL;
}

static void pr_cb_copy(void *ud, const char *content) {
    ui_t *u = ud;
    tui_term_copy_osc52(tui_term(u->tui), content, strlen(content));
    plan_status(u, "Copied plan to clipboard");
}

static void pr_cb_external(void *ud) {
    ui_t *u = ud;
    char *abs = app_plan_path(u->app, g_pr.path);
    size_t n;
    char *cur = fs_read(abs, &n);
    if (!cur) {
        char *m = xasprintf("Plan file not found at %s", g_pr.path);
        add_notice(u, "error", m);
        free(m);
        free(abs);
        return;
    }
    bool none = false;
    const char *ext = path_ext(abs);
    char *r = ui_external_edit(u, cur, *ext ? ext : ".md", &none);
    if (none) add_notice(u, "warning", "No editor configured. Set $VISUAL or $EDITOR environment variable.");
    else if (r) {
        fs_write_atomic(abs, r, strlen(r), 0644);
        if (g_pr.ov) ui_plan_review_set_plan(g_pr.ov, r);
        plan_status(u, "Plan updated in external editor.");
    }
    free(r);
    free(cur);
    free(abs);
}

static void pr_cb_annotation_editor(void *ud, const char *draft) {
    ui_t *u = ud;
    bool none = false;
    char *r = ui_external_edit(u, draft, ".md", &none);
    if (none) add_notice(u, "warning", "No editor configured. Set $VISUAL or $EDITOR environment variable.");
    else if (r && g_pr.ov) {
        size_t n = strlen(r);
        if (n && r[n - 1] == '\n') r[n - 1] = 0; /* trimTrailingNewline */
        ui_plan_review_set_annotation_draft(g_pr.ov, r);
    }
    free(r);
}

static void pr_cb_edited(void *ud, const char *content) {
    ui_t *u = ud;
    free(g_pr.edited);
    g_pr.edited = xstrdup(content);
    app_plan_write(u->app, g_pr.path, content);
}

static void pr_cb_feedback(void *ud, const char *fb) {
    (void)ud;
    free(g_pr.feedback);
    g_pr.feedback = xstrdup(fb);
}

static void pr_cb_ann_state(void *ud, const json_t *st) {
    (void)ud;
    if (!g_plan_ann) g_plan_ann = json_obj();
    if (json_len(json_get(st, "annotations")) > 0) json_set(g_plan_ann, g_pr.ann_key, json_clone(st));
    else json_del(g_plan_ann, g_pr.ann_key);
}

static void pr_cb_slider(void *ud, int index) {
    (void)ud;
    g_pr.sel_tier = index;
}

/* ----- approve ----- */

typedef struct {
    ui_t *u;
    char *path, *title, *content;
    bool preserve, compact;
    app_role_model_t *exec; /* borrowed from g_pr.cycle copy below (owned fields) */
    app_role_model_t exec_copy;
    bool has_exec;
} plan_exec_t;

static void post_status(ui_t *u, const char *text) { ui_notice(u, "info", text); }

static void plan_exec_clear_chat(void *ud) { ui_new_session_banner(ud); }

static void plan_exec_hide(void *ud) {
    ui_t *u = ud;
    plan_review_hide(u);
    plan_review_free_state();
    tui_request_render(u->tui);
}

static void copy_dir(const char *from, const char *to) {
    DIR *d = opendir(from);
    if (!d) return;
    fs_mkdir_p(to, 0700);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *s = path_join(from, de->d_name), *t = path_join(to, de->d_name);
        if (fs_is_dir(s)) copy_dir(s, t);
        else if (fs_is_file(s)) {
            size_t n;
            char *c = fs_read(s, &n);
            if (c) fs_write_atomic(t, c, n, 0644);
            free(c);
        }
        free(s);
        free(t);
    }
    closedir(d);
}

static void *plan_exec_main(void *p) {
    plan_exec_t *x = p;
    ui_t *u = x->u;
    app_t *a = u->app;
    /* #exitPlanMode({silent: true}) */
    app_exit_plan_mode(a, false);
    if (!x->preserve) {
        /* handleClearCommand + copyLocalArtifacts + rewrite the plan into the new local root */
        char *old_local = a->env->local_dir ? xstrdup(a->env->local_dir) : NULL;
        app_new_session(a);
        tui_post(u->tui, plan_exec_clear_chat, u);
        if (old_local && a->env->local_dir) copy_dir(old_local, a->env->local_dir);
        free(old_local);
        app_plan_write(a, x->path, x->content);
    } else if (x->compact) {
        json_t *ctx = json_obj();
        json_set_str(ctx, "planFilePath", x->path);
        char *instr = prompt_render("coding-agent/prompts/system/plan-mode-compact-instructions.md", ctx);
        json_free(ctx);
        app_set_plan_reference(a, x->path);
        char *err = NULL;
        if (app_compact(a, instr, &err) != 0) {
            char *m = xasprintf("Compaction failed: %s", err ? err : "compaction failed");
            ui_notice(u, "error", m);
            free(m);
        }
        free(err);
        free(instr);
    }
    app_set_plan_reference(a, x->path);
    char *aerr = NULL;
    char *saved = app_plan_autosave(a, x->title, x->content, &aerr);
    if (saved) {
        char *sp = tc_shorten_path(saved), *m = xasprintf("Saved plan to %s.", sp);
        post_status(u, m);
        free(m);
        free(sp);
    } else if (aerr) {
        char *m = xasprintf("Failed to autosave plan: %s", aerr);
        ui_notice(u, "warning", m);
        free(m);
    }
    free(saved);
    free(aerr);
    if (x->has_exec) { /* #applyPlanExecutionModel */
        app_apply_role_model(a, &x->exec_copy);
        char *m = xasprintf("Continuing with %s: %s", x->exec_copy.role,
                            x->exec_copy.model->name && *x->exec_copy.model->name ? x->exec_copy.model->name : x->exec_copy.model->id);
        post_status(u, m);
        free(m);
    }
    /* seed an auto name from the plan title when the session has none */
    char *seeded = app_plan_humanize_title(x->title);
    if (*seeded && a->session && !a->session->title) session_set_title(a->session, seeded, "auto");
    free(seeded);
    a->plan_reference_sent = true; /* markPlanReferenceSent: the synthetic prompt carries the plan */
    char *prompt = app_plan_approved_prompt(a, x->path, x->content, x->preserve);
    tui_post(u->tui, plan_exec_hide, u);
    size_t before = json_len(a->agent->messages);
    app_prompt_synthetic(a, prompt);
    free(prompt);
    json_t *run = json_arr();
    pthread_mutex_lock(&a->agent->mu);
    for (size_t i = before; i < json_len(a->agent->messages); i++) json_push(run, json_clone(json_at(a->agent->messages, i)));
    pthread_mutex_unlock(&a->agent->mu);
    const char *ctype = NULL;
    char *cont = app_after_run(a, run, "plan-approved", "interactive", &ctype);
    json_free(run);
    free(u->goal_cont);
    u->goal_cont = cont;
    u->cont_type = ctype;
    tui_post(u->tui, worker_done, u);
    free(x->path);
    free(x->title);
    free(x->content);
    free(x->exec_copy.role);
    free(x);
    return NULL;
}

static void plan_start_exec(ui_t *u) {
    g_pr.exec_pending = false;
    app_t *a = u->app;
    const char *choice = g_pr.choice;
    char *content = g_pr.edited ? xstrdup(g_pr.edited) : app_plan_read(a, g_pr.path);
    if (g_pr.edited) app_plan_write(a, g_pr.path, g_pr.edited);
    if (!content) {
        char *m = xasprintf("Plan file not found at %s", g_pr.path);
        add_notice(u, "error", m);
        free(m);
        plan_review_hide(u);
        plan_review_free_state();
        return;
    }
    plan_exec_t *x = xcalloc(1, sizeof *x);
    x->u = u;
    x->path = xstrdup(g_pr.path);
    x->title = xstrdup(g_pr.title);
    x->content = content;
    x->preserve = strcmp(choice, "Approve and execute") != 0;
    x->compact = !strcmp(choice, "Approve and compact context");
    /* the tier is applied only when the slider was shown and moved off the model that already runs */
    /* the model #exitPlanMode restores: the pre-plan one when the plan role took over */
    const ai_model_t *rm = a->plan_prev_set ? a->plan_prev_model : a->model;
    ai_effort_t rt = a->plan_prev_set ? a->plan_prev_thinking : a->thinking;
    int restored = -1;
    for (size_t i = 0; i < g_pr.ncycle && restored < 0; i++)
        if (g_pr.cycle[i].model == rm && (!g_pr.cycle[i].explicit_thinking || g_pr.cycle[i].thinking == rt)) restored = (int)i;
    if (g_pr.slider && g_pr.sel_tier != restored && g_pr.sel_tier >= 0 && (size_t)g_pr.sel_tier < g_pr.ncycle) {
        x->has_exec = true;
        x->exec_copy = g_pr.cycle[g_pr.sel_tier];
        x->exec_copy.role = xstrdup(g_pr.cycle[g_pr.sel_tier].role);
    }
    if (g_plan_ann) json_del(g_plan_ann, g_pr.ann_key);
    u->worker_running = true;
    if (pthread_create(&u->worker, NULL, plan_exec_main, x) != 0) {
        u->worker_running = false;
        add_notice(u, "error", "failed to start agent thread");
        free(x->path), free(x->title), free(x->content), free(x->exec_copy.role), free(x);
        plan_review_hide(u);
        plan_review_free_state();
    }
}

/* ----- save and quit ----- */

static void plan_save_done_now(ui_t *u, const char *path);

static void plan_save_done_posted(void *p) {
    pr_choice_t *c = p;
    plan_save_done_now(c->u, c->label);
    free(c->label);
    free(c);
}

/* the prompt is torn down by the answer: finish outside its key handler */
static void plan_save_done(void *ud, const char *path) {
    ui_t *u = ud;
    pr_choice_t *c = xcalloc(1, sizeof *c);
    c->u = u;
    c->label = path ? xstrdup(path) : NULL;
    tui_post(u->tui, plan_save_done_posted, c);
}

/* #promptPlanSavePath: a tiny-model topic (plan-filename.md) for the suggested name */
typedef struct {
    ui_t *u;
    char *excerpt;
    int gen;
    char *result;
} plan_name_job_t;

static int g_plan_save_gen;

static void plan_name_apply(void *p) {
    plan_name_job_t *j = p;
    if (j->result && g_pr.save && j->gen == g_plan_save_gen) {
        char *fn = app_plan_save_file_name(j->result);
        ui_plan_save_set_suggested(g_pr.save, fn);
        free(fn);
        tui_request_render(j->u->tui);
    }
    free(j->result);
    free(j->excerpt);
    free(j);
}

static void *plan_name_main(void *p) {
    plan_name_job_t *j = p;
    char *sys = prompt_render("coding-agent/prompts/system/plan-filename.md", NULL);
    char *raw = app_generate_title_with(j->u->app, j->excerpt, sys);
    j->result = raw ? title_normalize(raw) : NULL;
    free(raw);
    free(sys);
    tui_post(j->u->tui, plan_name_apply, j);
    return NULL;
}

static void plan_save_done_now(ui_t *u, const char *path) {
    tui_component_t *c = ui_plan_save_component(g_pr.save);
    tui_overlay_hide(u->tui, c);
    tui_set_focus(u->tui, &u->editor->base);
    char *sel = path ? xstrdup(path) : NULL;
    char *content = g_pr.save_content;
    g_pr.save_content = NULL;
    tui_component_destroy(c);
    g_pr.save = NULL;
    app_t *a = u->app;
    if (sel && content) {
        char *dest = path_abs(sel, a->cwd);
        char *sp = tc_shorten_path(dest);
        if (fs_write_atomic(dest, content, strlen(content), 0644) != 0) {
            char *m = xasprintf("Failed to save plan to %s: %s", sp, strerror(errno));
            add_notice(u, "error", m);
            free(m);
        } else {
            app_exit_plan_mode(a, false);
            if (g_plan_ann && g_pr.ann_key) json_del(g_plan_ann, g_pr.ann_key);
            app_new_session(a);
            ui_new_session_banner(u);
            char *m = xasprintf("Saved plan to %s.", sp);
            plan_status(u, m);
            free(m);
        }
        free(sp);
        free(dest);
    }
    free(sel);
    free(content);
    plan_review_free_state();
    tui_request_render(u->tui);
}

static void plan_save_and_quit(ui_t *u) {
    char *content = g_pr.edited ? xstrdup(g_pr.edited) : app_plan_read(u->app, g_pr.path);
    if (!content) {
        char *m = xasprintf("Plan file not found at %s", g_pr.path);
        add_notice(u, "error", m);
        free(m);
        plan_review_free_state();
        return;
    }
    char *suggested = app_plan_save_file_name(g_pr.title);
    g_pr.save_content = content;
    g_pr.save = ui_plan_save_new(suggested, plan_save_done, u);
    free(suggested);
    /* excerpt: the first 6 non-empty trimmed lines */
    sb_t ex = SB_INIT;
    int taken = 0;
    strvec_t lines = str_split(content, "\n");
    for (size_t i = 0; i < lines.n && taken < 6; i++) {
        char *t = str_trim(lines.v[i]);
        if (*t) {
            if (ex.len) sb_putc(&ex, '\n');
            sb_append(&ex, t);
            taken++;
        }
        free(t);
    }
    strvec_free(&lines);
    if (ex.len) {
        plan_name_job_t *j = xcalloc(1, sizeof *j);
        j->u = u;
        j->excerpt = sb_detach(&ex);
        j->gen = ++g_plan_save_gen;
        pthread_t th;
        if (pthread_create(&th, NULL, plan_name_main, j) == 0) pthread_detach(th);
        else {
            free(j->excerpt);
            free(j);
        }
    }
    sb_free(&ex);
    tui_overlay_opts_t o = {TUI_ANCHOR_BOTTOM, 0, 0, 0, 0};
    tui_overlay_show(u->tui, ui_plan_save_component(g_pr.save), &o);
    tui_request_render(u->tui);
}

/* ----- the choice ----- */

static void pr_finish(ui_t *u, const char *choice) {
    free(g_pr.choice);
    g_pr.choice = choice ? xstrdup(choice) : NULL;
    if (!choice) {
        plan_review_hide(u);
        plan_review_free_state();
        return;
    }
    if (!strcmp(choice, PLAN_SAVE_AND_QUIT)) {
        plan_review_hide(u);
        plan_save_and_quit(u);
        return;
    }
    if (!strcmp(choice, "Approve and execute") || !strcmp(choice, "Approve and compact context") ||
        str_eq(choice, g_pr.keep_label)) {
        /* the overlay stays up ("— submitting…") until the execution prompt is queued */
        if (u->worker_running) g_pr.exec_pending = true;
        else plan_start_exec(u);
        return;
    }
    if (!strcmp(choice, "Refine plan")) {
        char *fb = g_pr.feedback ? xstrdup(g_pr.feedback) : xstrdup("");
        char *t = str_trim(fb);
        bool has = *t;
        free(t);
        char *key = g_pr.ann_key ? xstrdup(g_pr.ann_key) : NULL;
        plan_review_hide(u);
        plan_review_free_state();
        if (has) {
            if (g_plan_ann && key) json_del(g_plan_ann, key);
            if (u->worker_running) agent_follow_up(u->app->agent, msg_user_text(fb));
            else start_prompt(u, fb);
        } else plan_status(u, "Refine plan: enter a follow-up prompt.");
        free(key);
        free(fb);
        return;
    }
    plan_review_hide(u);
    plan_review_free_state();
}

/* the overlay may be torn down by the choice: finish on the next loop turn, outside its key handler */
static void pr_finish_posted(void *p) {
    pr_choice_t *c = p;
    pr_finish(c->u, c->label);
    free(c->label);
    free(c);
}

static void pr_post_finish(ui_t *u, const char *label) {
    pr_choice_t *c = xcalloc(1, sizeof *c);
    c->u = u;
    c->label = label ? xstrdup(label) : NULL;
    tui_post(u->tui, pr_finish_posted, c);
}

static void pr_cb_pick(void *ud, const char *label) { pr_post_finish(ud, label); }
static void pr_cb_cancel(void *ud) { pr_post_finish(ud, NULL); }

static void ui_plan_approval(ui_t *u, const char *plan_path, const char *title) {
    app_t *a = u->app;
    if (!a->plan_mode) {
        add_notice(u, "warning", "Plan mode is not active.");
        return;
    }
    const char *path = plan_path && *plan_path ? plan_path : a->plan_file ? a->plan_file : "local://PLAN.md";
    char *content = app_plan_read(a, path);
    if (!content) {
        char *m = xasprintf("Plan file not found at %s", path);
        add_notice(u, "error", m);
        free(m);
        return;
    }
    if (g_pr.open || g_pr.save) {
        free(content);
        return;
    }
    plan_review_free_state();
    g_pr.u = u;
    g_pr.path = xstrdup(path);
    g_pr.title = xstrdup(title ? title : "plan");
    /* promote the reviewed path into plan-mode state (the next planning turn targets it) */
    app_set_plan_file(a, path);
    g_pr.ann_key = app_plan_path(a, path);

    /* "Approve and keep context (~tokens / window)", disabled above 95% */
    int64_t window = a->model ? a->model->context_window : 0;
    int64_t used = a->last_context_tokens;
    bool keep_disabled = false;
    if (window > 0) {
        char *t = ui_format_number((double)MAX(0, used)), *w = ui_format_number((double)window);
        for (char *q = t; *q; q++) *q = (char)tolower((unsigned char)*q);
        for (char *q = w; *q; q++) *q = (char)tolower((unsigned char)*q);
        g_pr.keep_label = xasprintf("Approve and keep context (~%s / %s)", t, w);
        free(t);
        free(w);
        keep_disabled = 100.0 * (double)used / (double)window > PLAN_KEEP_DISABLE_PERCENT;
    } else g_pr.keep_label = xstrdup("Approve and keep context");
    const char *options[] = {"Approve and execute", "Approve and compact context", g_pr.keep_label, "Refine plan",
                             PLAN_SAVE_AND_QUIT};
    int disabled[] = {PLAN_KEEP_CONTEXT_INDEX};

    /* model-tier slider over the role models, starting on `default`; hidden with fewer than two */
    int current = 0;
    g_pr.ncycle = app_role_model_cycle(a, &g_pr.cycle, &current);
    int def = -1;
    for (size_t i = 0; i < g_pr.ncycle; i++)
        if (!strcmp(g_pr.cycle[i].role, "default")) def = (int)i;
    g_pr.start_tier = def >= 0 ? def : current;
    g_pr.sel_tier = g_pr.start_tier;
    g_pr.slider = g_pr.ncycle > 1;
    const char **labels = xcalloc(MAX(1, g_pr.ncycle), sizeof *labels), **details = xcalloc(MAX(1, g_pr.ncycle), sizeof *details);
    for (size_t i = 0; i < g_pr.ncycle; i++) {
        labels[i] = g_pr.cycle[i].role;
        details[i] = g_pr.cycle[i].model->name && *g_pr.cycle[i].model->name ? g_pr.cycle[i].model->name : g_pr.cycle[i].model->id;
    }
    char *cancel = ui_editor_key("tui.select.cancel"), *help = xasprintf("%s cancel", cancel);
    char *edkey = ui_editor_key("app.editor.external");
    ui_plan_review_opts_t o = {.prompt_title = "Plan mode - next step",
                               .options = options,
                               .noptions = ARRAY_LEN(options),
                               .disabled = disabled,
                               .ndisabled = keep_disabled ? 1 : 0,
                               .help_text = help,
                               .initial_index = 0,
                               .slider_caption = "continue with",
                               .segment_labels = labels,
                               .segment_details = details,
                               .nsegments = g_pr.slider ? g_pr.ncycle : 0,
                               .slider_index = g_pr.start_tier,
                               .external_editor_label = *edkey ? edkey : NULL,
                               .annotation_state = g_plan_ann ? json_get(g_plan_ann, g_pr.ann_key) : NULL};
    ui_plan_review_cb_t cb = {.on_pick = pr_cb_pick,
                              .on_cancel = pr_cb_cancel,
                              .on_copy = pr_cb_copy,
                              .on_external_editor = pr_cb_external,
                              .on_annotation_editor = pr_cb_annotation_editor,
                              .on_plan_edited = pr_cb_edited,
                              .on_feedback = pr_cb_feedback,
                              .on_annotation_state = pr_cb_ann_state,
                              .on_slider = pr_cb_slider,
                              .ud = u};
    g_pr.ov = ui_plan_review_new(u->tui, content, &o, &cb);
    free(edkey);
    free(help);
    free(cancel);
    free(labels);
    free(details);
    free(content);
    g_pr.open = true;
    /* fullscreen overlay on the alternate screen */
    tui_term_puts(tui_term(u->tui), "\x1b[?1049h");
    tui_overlay_opts_t oo = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, ui_plan_review_component(g_pr.ov), &oo);
    tui_request_full_render(u->tui);
}

/* tool_execution_end of `write xd://propose` in review mode: the XdevDispatch inner carries
 * PlanApprovalDetails (event-controller.ts) */
static void maybe_plan_approval(ui_t *u, const char *tool, const json_t *result, bool is_error) {
    if (is_error || !str_eq(tool, "write")) return;
    const json_t *x = json_get(json_get(result, "details"), "xdev");
    if (!str_eq(json_get_str(x, "tool"), "propose") || !str_eq(json_get_str(x, "mode"), "execute")) return;
    const json_t *in = json_get(x, "inner");
    const char *path = json_get_str(in, "planFilePath"), *title = json_get_str(in, "title");
    if (!path || !title || !json_is_bool(json_get(in, "planExists"))) return;
    if (u->worker_running) { /* the aborted turn settles first (#abortPlanApprovalTurnSilently awaits it) */
        free(g_pending_review_path);
        free(g_pending_review_title);
        g_pending_review_path = xstrdup(path);
        g_pending_review_title = xstrdup(title);
        return;
    }
    ui_plan_approval(u, path, title);
}

static void plan_exec_resume(ui_t *u) {
    if (g_pr.exec_pending && !u->worker_running) plan_start_exec(u);
    if (g_pending_review_path && !u->worker_running) {
        char *p = g_pending_review_path, *t = g_pending_review_title;
        g_pending_review_path = g_pending_review_title = NULL;
        ui_plan_approval(u, p, t);
        free(p);
        free(t);
    }
}

/* ---------- slash commands ---------- */

static const struct {
    const char *name, *desc;
} COMMANDS[] = {
    {"help", "Show commands and keys"},       {"model", "Switch model (picker or /model <pattern>)"},
    {"thinking", "Set thinking level"},       {"new", "Start a new session"},
    {"plan", "Toggle read-only plan mode"},
    {"goal", "Goal mode: /goal <objective> | set | show | pause | resume | drop | budget <N|off>"},
    {"jobs", "Show background jobs status"},
    {"advisor", "Toggle the advisor (a second model that reviews each turn and injects notes)"},
    {"guided-goal", "Have the agent interview you, then set up goal mode [rough objective]"},
    {"tree", "Navigate the session tree (switch branches)"},
    {"branch", "Rewind to a previous message, keeping the old path as a branch"},
    {"retry", "Retry the last failed agent turn"},
    {"btw", "Ask a side question without adding it to the context"},
    {"queue", "Queue a message for after the agent yields"},
    {"handoff", "Summarize the session into a handoff document and compact in place"},
    {"shake", "Drop heavy tool output from the context"},
    {"rename", "Rename the session (omit title to generate)"},
    {"delete", "Delete the current session and start a new one"},
    {"todo", "Show the agent's todo list"},
    {"tools", "Show tools visible to the agent"},
    {"context", "Show context usage"},
    {"skills", "List discovered skills"},
    {"mcp", "Show MCP server status"},
    {"dirs", "List workspace directories"},
    {"effort", "Set thinking level (alias of /thinking)"},
    {"usage", "Show token usage and cost"},
    {"dump", "Copy the session transcript to the clipboard"},
    {"resume", "Resume a previous session"},  {"compact", "Compact the context [instructions]"},
    {"session", "Show session info"},         {"name", "Set session title"},
    {"export", "Export session [path]"},      {"copy", "Copy last answer to clipboard"},
    {"fork", "Fork the session here"},        {"clear", "Clear the conversation context in place, keeping the session"},
    {"hotkeys", "Show keyboard shortcuts"},   {"settings", "Open settings menu"},   {"quit", "Exit omc"},
    {"login", "Login with OAuth provider"},   {"logout", "Logout from OAuth provider"},
    {"migrate", "Copy your omp configuration into omc [--dry-run] [--force] [--no-sessions]"},
    {"exit", "Exit omc"},
};

static void goal_show(ui_t *u) {
    const goal_t *g = app_goal(u->app);
    if (!g) {
        add_notice(u, "info", "No goal. Start one with /goal <objective>.");
        return;
    }
    char *s = goal_budget(g) ? xasprintf("Goal [%s]: %s\nTokens: %lld / %lld", goal_status(g), goal_objective(g), (long long)goal_used(g),
                                         (long long)goal_budget(g))
                             : xasprintf("Goal [%s]: %s\nTokens: %lld used", goal_status(g), goal_objective(g), (long long)goal_used(g));
    add_notice(u, "info", s);
    free(s);
}

static void goal_command(ui_t *u, const char *arg) {
    app_t *a = u->app;
    if (a->plan_mode) {
        add_notice(u, "warning", "Goal mode is blocked while plan mode is active. Exit plan mode first (/plan).");
        return;
    }
    if (!settings_bool("goal.enabled", true)) {
        add_notice(u, "warning", "Goal mode is disabled. Enable it in settings (goal.enabled).");
        return;
    }
    char *sub = xstrndup(arg, strcspn(arg, " \t"));
    const char *rest = arg + strlen(sub);
    while (*rest == ' ' || *rest == '\t') rest++;
    const goal_t *g = app_goal(a);
    char *err = NULL;
    bool known = true;
    if (!strcmp(sub, "show")) {
        goal_show(u);
    } else if (!strcmp(sub, "pause")) {
        if (app_goal_pause(a) != 0) add_notice(u, "warning", "No goal to pause.");
        else add_notice(u, "info", "Goal paused.");
    } else if (!strcmp(sub, "resume")) {
        if (app_goal_resume(a, &err) != 0) add_notice(u, "warning", err);
        else {
            add_notice(u, "info", "Goal resumed.");
            char *c = app_goal_after_run(a, NULL, false, "interactive");
            if (c && !u->worker_running) start_hidden(u, c, "goal-continuation");
            free(c);
        }
    } else if (!strcmp(sub, "drop")) {
        app_goal_drop(a);
        add_notice(u, "info", "Goal dropped; goal mode ended.");
    } else if (!strcmp(sub, "budget")) {
        int64_t b = !strcmp(rest, "off") || !*rest ? 0 : strtoll(rest, NULL, 10);
        if ((strcmp(rest, "off") && *rest && b <= 0) || app_goal_set_budget(a, b, &err) != 0)
            add_notice(u, "warning", err ? err : "Usage: /goal budget <N|off>");
        else goal_show(u);
    } else if (!strcmp(sub, "set")) {
        if (!*rest) add_notice(u, "warning", "Usage: /goal set <objective>");
        else if (app_goal_replace(a, rest, 0, &err) != 0) add_notice(u, "warning", err);
        else if (!u->worker_running) start_prompt(u, rest);
    } else {
        known = false;
    }
    if (!known) {
        if (g && goal_enabled(g)) {
            if (*arg) add_notice(u, "info", "Goal mode is already active. Use /goal to manage it, or /goal drop to start over.");
            else goal_show(u);
        } else if (g) {
            if (*arg) add_notice(u, "warning", "Resume the current goal first, or drop it before setting a new objective.");
            else goal_show(u);
        } else if (!*arg) {
            add_notice(u, "info", "Usage: /goal <objective>");
        } else if (app_goal_create(a, arg, 0, &err) != 0) {
            add_notice(u, "warning", err);
        } else if (!u->worker_running) {
            start_prompt(u, arg);
        }
    }
    free(err);
    free(sub);
}

static bool handle_command(ui_t *u, const char *line) {
    char *cmd = xstrndup(line + 1, strcspn(line + 1, " \t"));
    const char *arg = line + 1 + strlen(cmd);
    while (*arg == ' ' || *arg == '\t') arg++;
    bool handled = true;
    app_t *a = u->app;
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
        tui_stop(u->tui, 0);
    } else if (!strcmp(cmd, "settings")) {
        ui_overlay_settings(u);
    } else if (!strcmp(cmd, "hotkeys")) {
        ui_overlay_hotkeys(u);
    } else if (!strcmp(cmd, "login") || !strcmp(cmd, "logout")) {
        ui_overlay_login(u, !strcmp(cmd, "logout"));
    } else if (!strcmp(cmd, "help")) {
        sb_t b = SB_INIT;
        sb_append(&b, "Commands:\n");
        for (size_t i = 0; i < ARRAY_LEN(COMMANDS); i++) sb_appendf(&b, "  /%-10s %s\n", COMMANDS[i].name, COMMANDS[i].desc);
        sb_append(&b, "Keys: enter send (steer while running) · alt+enter follow-up/newline · esc interrupt · "
                      "shift+tab thinking · ctrl+o expand tools · ctrl+t toggle thinking · alt+m model · "
                      "ctrl+c clear/exit · ctrl+d exit · !cmd run shell");
        add_notice(u, "info", sb_cstr(&b));
        sb_free(&b);
    } else if (!strcmp(cmd, "model")) {
        if (*arg) {
            char *err = NULL;
            if (app_set_model(a, arg, &err) != 0) add_notice(u, "error", err);
            else {
                char *ml = app_model_label(a), *m = xasprintf("Model: %s", ml);
                add_notice(u, "info", m);
                free(m);
                free(ml);
            }
            free(err);
        } else {
            ui_overlay_model(u);
        }
    } else if (!strcmp(cmd, "thinking")) {
        ai_effort_t e;
        if (*arg && effort_parse(arg, &e) == 0) {
            app_set_thinking(a, e);
        } else {
            ui_overlay_thinking(u);
        }
    } else if (!strcmp(cmd, "migrate")) {
        /* omp -> omc: settings, models, credentials, MCP, skills, rules, agents, themes, sessions, prompt history */
        migrate_opts_t mo = {.sessions = true};
        if (strstr(arg, "--dry-run")) mo.dry_run = true;
        if (strstr(arg, "--force")) mo.force = true;
        if (strstr(arg, "--no-sessions")) mo.sessions = false;
        sb_t rep = SB_INIT;
        char *err = NULL;
        int rc = omc_migrate(migrate_omp_dir(), omc_agent_dir(), &mo, &rep, &err);
        if (rc < 0) {
            add_notice(u, "error", err ? err : "migration failed");
        } else {
            if (!mo.dry_run) {
                auth_reload();
                sb_append(&rep, "Restart omc to load the migrated settings, models and MCP servers.");
            }
            add_notice(u, rc ? "warning" : "info", sb_cstr(&rep));
        }
        free(err);
        sb_free(&rep);
    } else if (!strcmp(cmd, "memory")) {
        memory_cmd_result_t r;
        memory_command(a, arg, &r);
        if (str_eq(r.level, "panel")) {
            char *t = str_trim(r.text);
            tui_markdown_t *m = tui_markdown_new(t, 0, 0);
            free(t);
            ui_show_report(u, r.title, &m->base);
        } else {
            add_notice(u, r.level, r.text);
        }
        memory_cmd_result_free(&r);
    } else if (!strcmp(cmd, "plan")) { /* handlePlanModeCommand */
        if (a->plan_mode) {
            if (u->worker_running) app_abort(a);
            app_exit_plan_mode(a, true);
            add_notice(u, "info", "Plan mode paused.");
        } else if (a->plan_paused && !*arg) {
            a->plan_paused = false;
            a->plan_has_entered = false;
            if (a->session) {
                json_t *e = json_obj();
                json_set_str(e, "type", "mode_change");
                json_set_str(e, "mode", "none");
                session_append_entry(a->session, e);
            }
            add_notice(u, "info", "Plan mode disabled.");
        } else if (!settings_bool("plan.enabled", true)) {
            add_notice(u, "warning", "Plan mode is disabled. Enable it in settings (plan.enabled).");
        } else {
            app_enter_plan_mode(a, NULL);
            app_plan_apply_model(a);
            char *m = xasprintf("Plan mode enabled. Plan file: %s", a->plan_file);
            add_notice(u, "info", m);
            free(m);
            if (*arg) start_prompt(u, arg);
        }
    } else if (!strcmp(cmd, "goal")) {
        goal_command(u, arg);
    } else if (!strcmp(cmd, "jobs")) {
        json_t *snap = jobs_snapshot(a->env->jobs), *x;
        sb_t b = SB_INIT;
        json_arr_foreach(snap, i, x) sb_appendf(&b, "%s%-8s %-10s %5llds  %s", b.len ? "\n" : "", json_get_str(x, "id"), json_get_str(x, "status"),
                                                  (long long)(json_get_int(x, "durationMs", 0) / 1000), json_get_str(x, "label"));
        add_notice(u, "info", b.len ? sb_cstr(&b) : "No background jobs.");
        sb_free(&b);
        json_free(snap);
    } else if (!strcmp(cmd, "advisor")) {
        bool on = !strcmp(arg, "on") ? true : !strcmp(arg, "off") ? false : !a->advisor;
        app_set_advisor(a, on);
        char *ml = NULL;
        const ai_model_t *am = a->advisor ? app_role_model("advisor", NULL, NULL) : NULL;
        ml = am ? mr_model_key(am) : app_model_label(a);
        char *m = on ? xasprintf("Advisor on (%s)", ml) : xstrdup("Advisor off");
        add_notice(u, "info", m);
        free(m);
        free(ml);
    } else if (!strcmp(cmd, "guided-goal")) {
        if (a->plan_mode) add_notice(u, "warning", "Goal mode is blocked while plan mode is active. Exit plan mode first (/plan).");
        else if (app_goal(a)) add_notice(u, "info", "A goal already exists. Use /goal to manage it, or /goal drop to start over.");
        else if (u->worker_running) add_notice(u, "warning", "Wait for the agent to finish first.");
        else {
            app_goal_expose_tool(a);
            json_t *c = json_obj();
            if (*arg) json_set_str(c, "initial", arg);
            char *k = prompt_render("coding-agent/prompts/goals/guided-goal-interview.md", c);
            json_free(c);
            start_hidden(u, k, "guided-goal");
            free(k);
        }
    } else if (!strcmp(cmd, "tree") || !strcmp(cmd, "branch")) {
        if (u->worker_running) {
            add_notice(u, "warning", "Cannot switch branches while the agent is running.");
        } else {
            bool tree = !strcmp(cmd, "tree");
            ensure_picker(u);
            tui_select_list_clear(u->picker);
            json_t *list = tree ? json_clone(a->session->entries) : session_path_to(a->session, a->session->leaf_id);
            json_t *e;
            size_t shown = 0;
            json_arr_foreach(list, i, e) {
                if (!str_eq(json_get_str(e, "type"), "message")) continue;
                json_t *m = json_get(e, "message");
                const char *role = json_get_str(m, "role");
                if (!str_eq(role, "user") && !(tree && str_eq(role, "assistant"))) continue;
                if (json_get_bool(m, "synthetic", false)) continue;
                int depth = 0;
                for (const char *pid = json_get_str(e, "parentId"); pid && depth < 64; depth++) {
                    json_t *pe = session_get(a->session, pid);
                    pid = pe ? json_get_str(pe, "parentId") : NULL;
                }
                char *t = ai_message_text(m);
                if (!*t) {
                    sb_t tb = SB_INIT;
                    json_t *blk;
                    json_arr_foreach(json_get_arr(m, "content"), k, blk) if (str_eq(json_get_str(blk, "type"), "toolCall"))
                        sb_appendf(&tb, "%s[%s]", tb.len ? " " : "", json_get_str(blk, "name"));
                    free(t);
                    t = sb_detach(&tb);
                }
                for (char *q = t; *q; q++)
                    if (*q == '\n') *q = ' ';
                bool on_path = a->session->leaf_id && !strcmp(json_get_str(e, "id"), a->session->leaf_id);
                char *label = xasprintf("%*s%s %.70s", tree ? MIN(depth / 2, 20) : 0, "", str_eq(role, "user") ? ">" : "<", t);
                tui_select_list_add(u->picker, json_get_str(e, "id"), label, on_path ? "(current)" : NULL);
                free(label);
                free(t);
                shown++;
            }
            json_free(list);
            if (!shown) add_notice(u, "info", "No messages yet.");
            else show_picker(u, 5);
        }
    } else if (!strcmp(cmd, "retry")) {
        if (u->worker_running) add_notice(u, "warning", "The agent is already running.");
        else {
            u->worker_running = true;
            if (pthread_create(&u->worker, NULL, retry_main, u) != 0) u->worker_running = false;
        }
    } else if (!strcmp(cmd, "btw")) {
        if (!*arg) add_notice(u, "info", "Usage: /btw <question>");
        else {
            btw_t *b = xcalloc(1, sizeof(*b));
            b->u = u;
            b->q = xstrdup(arg);
            pthread_t th;
            if (pthread_create(&th, NULL, btw_main, b) == 0) pthread_detach(th);
            add_notice(u, "info", "Asking side question…");
        }
    } else if (!strcmp(cmd, "queue")) {
        if (!*arg) add_notice(u, "info", "Usage: /queue <message>");
        else if (u->worker_running) {
            agent_follow_up(a->agent, msg_user_text(arg));
            add_notice(u, "info", "Queued for after the agent yields");
        } else start_prompt(u, arg);
    } else if (!strcmp(cmd, "handoff")) {
        char *err = NULL;
        add_notice(u, "info", "Writing handoff…");
        tui_render_now(u->tui);
        if (app_handoff(a, &err) != 0) add_notice(u, "error", err ? err : "handoff failed");
        else {
            reload_chat(u);
            add_notice(u, "info", "Handoff written; context compacted");
        }
        free(err);
    } else if (!strcmp(cmd, "shake")) {
        size_t n = app_shake(a, *arg ? (size_t)atoi(arg) : 2000);
        char *m = xasprintf("Elided %zu heavy tool result%s from the context", n, n == 1 ? "" : "s");
        add_notice(u, "info", m);
        free(m);
    } else if (!strcmp(cmd, "rename")) {
        char *t = *arg ? xstrdup(arg) : app_generate_title(a);
        if (t) {
            session_set_title(a->session, t, *arg ? "user" : "auto");
            char *m = xasprintf("Session renamed: %s", t);
            add_notice(u, "info", m);
            free(m);
        } else add_notice(u, "warning", "Could not generate a title");
        free(t);
    } else if (!strcmp(cmd, "delete")) {
        char *path = app_session_file(a) ? xstrdup(app_session_file(a)) : NULL;
        app_new_session(a);
        if (path) unlink(path);
        free(path);
        tui_container_clear(u->chat);
        add_notice(u, "info", "Session deleted; new session started");
    } else if (!strcmp(cmd, "todo")) {
        json_t *ph, *t;
        sb_t b = SB_INIT;
        json_arr_foreach(a->env->todo.phases, i, ph) {
            sb_appendf(&b, "%s%s\n", b.len ? "\n" : "", json_get_str(ph, "name") ? json_get_str(ph, "name") : "Todo");
            json_arr_foreach(json_get_arr(ph, "tasks"), k, t) {
                const char *st = json_get_str(t, "status");
                sb_appendf(&b, "  %s %s\n", str_eq(st, "completed") ? "[x]" : str_eq(st, "in_progress") ? "[>]" : str_eq(st, "abandoned") ? "[-]" : "[ ]",
                           json_get_str(t, "content"));
            }
        }
        add_notice(u, "info", b.len ? sb_cstr(&b) : "No todos.");
        sb_free(&b);
    } else if (!strcmp(cmd, "tools")) {
        sb_t b = SB_INIT;
        sb_appendf(&b, "%zu tools visible to the agent:\n", a->agent->tools.n);
        for (size_t i = 0; i < a->agent->tools.n; i++) {
            const char *d = a->agent->tools.v[i]->description ? a->agent->tools.v[i]->description : "";
            sb_appendf(&b, "  %-14s %.*s\n", a->agent->tools.v[i]->name, (int)MIN(strcspn(d, "\n"), (size_t)90), d);
        }
        add_notice(u, "info", sb_cstr(&b));
        sb_free(&b);
    } else if (!strcmp(cmd, "context") || !strcmp(cmd, "usage")) {
        json_t *st = app_stats(a);
        size_t sys = 0;
        json_t *x;
        json_arr_foreach(a->agent->system_prompt, i, x) sys += strlen(json_sv(x));
        size_t tools = 0;
        for (size_t i = 0; i < a->agent->tools.n; i++) {
            char *w = json_dump(a->agent->tools.v[i]->parameters, 0);
            tools += strlen(w) + strlen(a->agent->tools.v[i]->description ? a->agent->tools.v[i]->description : "");
            free(w);
        }
        char *m = !strcmp(cmd, "context")
                      ? xasprintf("Context: %lld / %lld tokens (%.1f%%)\n  system prompt ~%zu tokens · tools ~%zu tokens · %zu messages",
                                  (long long)json_get_int(st, "contextTokens", 0), (long long)json_get_int(st, "contextWindow", 0),
                                  json_get_num(st, "contextPercent", 0), sys / 4, tools / 4, json_len(a->agent->messages))
                      : xasprintf("Usage: %lld in · %lld out · %lld cache read · %lld cache write · $%.4f",
                                  (long long)json_get_int(st, "input", 0), (long long)json_get_int(st, "output", 0),
                                  (long long)json_get_int(st, "cacheRead", 0), (long long)json_get_int(st, "cacheWrite", 0),
                                  json_get_num(st, "cost", 0));
        add_notice(u, "info", m);
        free(m);
        json_free(st);
    } else if (!strcmp(cmd, "skills")) {
        sb_t b = SB_INIT;
        for (size_t i = 0; i < a->disc.skills.n; i++)
            sb_appendf(&b, "  %-20s %s\n", a->disc.skills.v[i].name, a->disc.skills.v[i].description ? a->disc.skills.v[i].description : "");
        add_notice(u, "info", b.len ? sb_cstr(&b) : "No skills discovered.");
        sb_free(&b);
    } else if (!strcmp(cmd, "mcp")) {
        char *st = a->mcp ? mcp_status(a->mcp) : NULL;
        add_notice(u, "info", st && *st ? st : "No MCP servers configured.");
        free(st);
    } else if (!strcmp(cmd, "dirs")) {
        add_notice(u, "info", a->cwd);
    } else if (!strcmp(cmd, "effort")) {
        char *l = xasprintf("/thinking %s", arg);
        handle_command(u, l);
        free(l);
    } else if (!strcmp(cmd, "dump")) {
        char *md = session_export_markdown(a->session);
        tui_term_copy_osc52(tui_term(u->tui), md, strlen(md));
        free(md);
        add_notice(u, "info", "Copied session transcript to clipboard");
    } else if (!strcmp(cmd, "clear")) {
        if (u->worker_running || app_clear_context(a) != 0) add_notice(u, "warning", "Wait for the agent to finish first.");
        else {
            tui_container_clear(u->chat);
            tui_request_full_render(u->tui);
            add_notice(u, "info", "Context cleared (same session)");
        }
    } else if (!strcmp(cmd, "new")) {
        app_new_session(a);
        ui_new_session_banner(u);
    } else if (!strcmp(cmd, "resume")) {
        ui_overlay_session(u);
    } else if (!strcmp(cmd, "compact")) {
        char *err = NULL;
        ui_working_compaction(u, "Compacting context...");
        tui_render_now(u->tui);
        if (app_compact(a, *arg ? arg : NULL, &err) != 0) {
            ui_working_stop(u);
            char *m = xasprintf("Compaction failed: %s", err ? err : "compaction failed");
            add_notice(u, "error", m);
            free(m);
        }
        free(err);
    } else if (!strcmp(cmd, "session")) {
        json_t *st = app_stats(a);
        char *s = xasprintf("Session %s\nFile: %s\nMessages: %lld user, %lld assistant, %lld tool results\n"
                            "Tokens: %lld in, %lld out, %lld cache read, %lld cache write\nCost: $%.4f\nContext: %lld / %lld (%.1f%%)",
                            json_get_str(st, "sessionId"), json_get_str(st, "sessionFile") ? json_get_str(st, "sessionFile") : "(memory)",
                            (long long)json_get_int(st, "userMessages", 0), (long long)json_get_int(st, "assistantMessages", 0),
                            (long long)json_get_int(st, "toolResults", 0), (long long)json_get_int(st, "input", 0),
                            (long long)json_get_int(st, "output", 0), (long long)json_get_int(st, "cacheRead", 0),
                            (long long)json_get_int(st, "cacheWrite", 0), json_get_num(st, "cost", 0),
                            (long long)json_get_int(st, "contextTokens", 0), (long long)json_get_int(st, "contextWindow", 0),
                            json_get_num(st, "contextPercent", 0));
        add_notice(u, "info", s);
        free(s);
        json_free(st);
    } else if (!strcmp(cmd, "name")) {
        if (*arg) session_set_title(a->session, arg, "user");
    } else if (!strcmp(cmd, "export")) {
        char *text = str_ends(arg, ".md") ? session_export_markdown(a->session) : session_export_html(a->session);
        char *dest = *arg ? xstrdup(arg) : xasprintf("omc-session-%s.html", session_short_id(session_id(a->session)));
        int rc = fs_write(dest, text, strlen(text));
        char *m = rc ? xasprintf("Export failed: %s", strerror(-rc)) : xasprintf("Exported to %s", dest);
        add_notice(u, rc ? "error" : "info", m);
        free(m);
        free(dest);
        free(text);
    } else if (!strcmp(cmd, "copy")) {
        const json_t *last = last_assistant(a->agent->messages);
        if (last) {
            char *t = ai_message_text(last);
            tui_term_copy_osc52(tui_term(u->tui), t, strlen(t));
            free(t);
            add_notice(u, "info", "Copied last answer to clipboard");
        }
    } else if (!strcmp(cmd, "fork")) {
        char *err = NULL;
        if (app_fork_from(a, NULL, &err) != 0) add_notice(u, "error", err);
        else add_notice(u, "info", "Forked into a new session");
        free(err);
    } else {
        handled = false;
    }
    free(cmd);
    return handled;
}

/* ---------- bash (!cmd) ---------- */

static void run_bang(ui_t *u, const char *line) {
    bool exclude = line[1] == '!';
    const char *cmd = line + (exclude ? 2 : 1);
    msgview_t *v = mv_new(MV_BASH);
    v->tool_name = xstrdup("$");
    v->args = json_obj();
    json_set_str(v->args, "command", cmd);
    if (exclude) json_set_bool(v->args, "exclude", true);
    add_view(u, v);
    tui_render_now(u->tui);
    json_t *m = app_user_bash(u->app, cmd, exclude);
    if (m) {
        v->output = xstrdup(json_get_str(m, "output") ? json_get_str(m, "output") : "");
        int64_t code = json_get_int(m, "exitCode", 0);
        v->is_error = json_get_bool(m, "cancelled", false) || code != 0;
        json_set_int(v->args, "exitCode", code);
        json_free(m);
    }
    v->done = true;
    v->dirty = true;
    tui_request_render(u->tui);
}

/* ---------- prompt history (persisted across sessions) ---------- */

static void history_load(tui_editor_t *ed) {
    char *p = omc_agent_path("prompt-history.jsonl");
    size_t n;
    char *data = fs_read(p, &n);
    free(p);
    if (!data) return;
    strvec_t lines = str_split_lines(data, n);
    size_t start = lines.n > 500 ? lines.n - 500 : 0;
    for (size_t i = start; i < lines.n; i++) {
        json_t *j = json_parse_str(lines.v[i]);
        const char *t = json_get_str(j, "text");
        if (t && *t) tui_editor_add_history(ed, t);
        json_free(j);
    }
    strvec_free(&lines);
    free(data);
}

static void history_append(const char *text, const char *cwd) {
    char *p = omc_agent_path("prompt-history.jsonl");
    json_t *j = json_obj();
    json_set_str(j, "text", text);
    json_set_str(j, "cwd", cwd);
    json_set_int(j, "ts", now_ms());
    char *line = json_dump(j, 0);
    sb_t b = SB_INIT;
    sb_append(&b, line);
    sb_putc(&b, '\n');
    fs_append(p, b.s, b.len);
    sb_free(&b);
    free(line);
    json_free(j);
    free(p);
}

/* ---------- input ---------- */

static void on_submit(tui_editor_t *ed, const char *text, void *ud) {
    ui_t *u = ud;
    char *t = str_trim(text);
    if (!*t) {
        free(t);
        return;
    }
    tui_editor_add_history(ed, t);
    history_append(t, u->app->cwd);
    tui_editor_clear(ed);
    if (t[0] == '/' && handle_command(u, t)) {
        free(t);
        tui_request_render(u->tui);
        return;
    }
    if (t[0] == '!' && !u->worker_running) {
        run_bang(u, t);
        free(t);
        return;
    }
    if (u->worker_running) {
        agent_steer(u->app->agent, msg_user_text(t));
        char *m = xasprintf("Queued (steer): %s", t);
        add_notice(u, "info", m);
        free(m);
    } else {
        start_prompt(u, t);
    }
    free(t);
    tui_request_render(u->tui);
}

static bool key_hook(tui_t *t, const tui_key_t *k, void *ud) {
    ui_t *u = ud;
    if (tui_overlay_visible(t) || ui_selector_open(u)) return false;
    if (tui_key_matches(k, "escape")) {
        if (u->worker_running) {
            app_abort(u->app);
            return true;
        }
        return false;
    }
    if (tui_key_matches(k, "ctrl+c")) {
        if (!tui_editor_is_empty(u->editor)) {
            tui_editor_clear(u->editor);
            return true;
        }
        if (u->worker_running) {
            app_abort(u->app);
            return true;
        }
        int64_t now = mono_ms();
        if (now - u->last_ctrl_c < 1500) {
            tui_stop(t, 0);
        } else {
            u->last_ctrl_c = now;
            add_notice(u, "info", "Press ctrl+c again to exit");
        }
        return true;
    }
    if (tui_key_matches(k, "ctrl+d") && tui_editor_is_empty(u->editor)) {
        tui_stop(t, 0);
        return true;
    }
    if (tui_key_matches(k, "shift+tab")) {
        app_t *a = u->app;
        ai_effort_t e = a->thinking;
        for (int i = 0; i <= EFFORT_MAX; i++) {
            e = (ai_effort_t)((e + 1) % (EFFORT_MAX + 1));
            if (model_supports_effort(a->model, e)) break;
        }
        app_set_thinking(a, e);
        ui_hint_record("effort");
        tui_request_render(t);
        return true;
    }
    if (tui_key_matches(k, "ctrl+o")) {
        u->expand_tools = !u->expand_tools;
        add_notice(u, "info", u->expand_tools ? "Tool output expansion: enabled" : "Tool output expansion: disabled");
        for (size_t i = 0; i < tui_container_count(u->chat); i++) tui_component_invalidate(u->chat->children.v[i]);
        tui_request_render(t);
        return true;
    }
    if (tui_key_matches(k, "ctrl+t")) {
        u->hide_thinking = !u->hide_thinking;
        for (size_t i = 0; i < tui_container_count(u->chat); i++) tui_component_invalidate(u->chat->children.v[i]);
        tui_request_render(t);
        return true;
    }
    if (tui_key_matches(k, "ctrl+p") || tui_key_matches(k, "shift+ctrl+p")) { /* app.model.cycleForward/Backward */
        int idx = app_cycle_role_model(u->app, tui_key_matches(k, "shift+ctrl+p"));
        if (idx < 0) add_notice(u, "info", "Only one role model available");
        else show_model_cycle_track(u, idx);
        tui_request_render(t);
        return true;
    }
    if (tui_key_matches(k, "alt+m")) { /* app.model.select */
        ui_overlay_model(u);
        return true;
    }
    if (tui_key_matches(k, "alt+enter") && u->worker_running) {
        char *txt = tui_editor_get_expanded_text(u->editor);
        char *tt = str_trim(txt);
        if (*tt) {
            agent_follow_up(u->app->agent, msg_user_text(tt));
            char *m = xasprintf("Queued (follow-up): %s", tt);
            add_notice(u, "info", m);
            free(m);
            tui_editor_clear(u->editor);
        }
        free(tt);
        free(txt);
        return true;
    }
    return false;
}

static void initial_prompt_cb(void *ud) {
    job_t *j = ud;
    start_prompt(j->u, j->text);
    free(j->text);
    free(j);
}

static void setup_done_cb(void *ud) {
    if (g_ui && g_ui->welcome) ui_welcome_play_intro(g_ui->welcome, g_ui->tui); /* playWelcomeIntro */
    if (ud) initial_prompt_cb(ud);
}

static void setup_start_cb(void *ud) {
    job_t *j = ud;
    ui_setup_run(g_ui, setup_done_cb, j);
}

int run_interactive_mode(app_t *a, const char *initial) {
    ui_t u = {0};
    g_ui = &u;
    u.app = a;
    u.hide_thinking = settings_bool("hideThinkingBlock", false);
    pthread_mutex_init(&u.mu, NULL);
    pthread_cond_init(&u.ask_cv, NULL);
    hmap_init(&u.tools);
    ui_apply_appearance(); /* theme.dark/light (titanium/light), symbolPreset, colorBlindMode */
    u.tui = tui_new(NULL);
    if (!u.tui) {
        fprintf(stderr, "omc: cannot initialize terminal (not a TTY?). Use -p for print mode.\n");
        return 1;
    }
    tui_container_t *root = tui_root(u.tui);
    tui_sym_set_preset(settings_str("symbolPreset", "unicode"));
    /* config warnings head the header (interactive-mode.ts #buildConfigWarningComponents): retry.fallbackChains
     * validation (turn-recovery validateRetryFallbackChains) */
    {
        strvec_t warns = {0};
        mr_fallback_validate(&warns);
        for (size_t i = 0; i < warns.n; i++) {
            char *t = xasprintf("Warning: %s", warns.v[i]);
            char *styled = tui_fgs(NULL, TUI_TC_WARNING, t);
            tui_container_add(root, &tui_text_new(styled, 1, 0)->base);
            tui_container_add(root, &tui_spacer_new(1)->base);
            free(styled);
            free(t);
        }
        strvec_free(&warns);
    }
    /* header: welcome banner between one-row spacers (omp Composer#rebuildHeader) */
    if (!settings_bool("startup.quiet", false)) {
        u.welcome = ui_welcome_new();
        tui_container_add(root, &tui_spacer_new(1)->base);
        tui_container_add(root, u.welcome);
        tui_container_add(root, &tui_spacer_new(1)->base);
    }
    u.chat = ui_transcript_new(false); /* the welcome banner brings its own trailing spacer */
    tui_container_add(root, &u.chat->base);
    u.banner = tui_container_new();
    tui_container_add(root, &u.banner->base);
    u.model_cycle = tui_container_new();
    tui_container_add(root, &u.model_cycle->base);
    u.working = ui_working_new(&u);
    tui_container_add(root, u.working);
    u.loader = tui_loader_new(u.tui, "Working…");
    u.loader->base.hidden = true;
    tui_container_add(root, ui_status_row_new(&u, &u.loader->base));
    tui_container_add(root, ui_editor_gap_new(&u));
    u.editor = tui_editor_new();
    tui_editor_set_on_submit(u.editor, on_submit, &u);
    ui_chrome_setup(&u);
    history_load(u.editor);
    tui_ac_default_t *ac = tui_ac_default_new(a->cwd);
    {
        const char *names[ARRAY_LEN(COMMANDS)], *descs[ARRAY_LEN(COMMANDS)];
        for (size_t i = 0; i < ARRAY_LEN(COMMANDS); i++) names[i] = COMMANDS[i].name, descs[i] = COMMANDS[i].desc;
        ui_slash_register(&u, ac, names, descs, ARRAY_LEN(COMMANDS));
    }
    tui_editor_set_autocomplete(u.editor, tui_ac_default_provider, ac);
    tui_container_add(root, &u.editor->base);
    u.status = ui_status_new(&u);
    tui_container_add(root, u.status);
    tui_set_focus(u.tui, &u.editor->base);
    tui_set_key_hook(u.tui, key_hook, &u);
    /* replay resumed history */
    ui_chat_rebuild(&u);
    if (json_len(a->session->entries)) ui_plan_restore(&u);
    if (!a->plan_mode && !a->plan_yolo_target && app_plan_should_start(a)) { /* #applyPlanModeDefault */
        app_enter_plan_mode(a, NULL);
        app_plan_apply_model(a);
        char *m = xasprintf("Plan mode enabled. Plan file: %s", a->plan_file);
        add_notice(&u, "info", m);
        free(m);
    }
    agent_subscribe(a->agent, ui_listener, &u);
    a->notice = ui_notice;
    a->notice_ud = &u;
    a->wake = ui_wake;
    a->wake_ud = &u;
    a->env->ui.ask = ui_ask;
    a->env->ui.tool_frame = ui_tool_frame;
    a->env->ui.ud = &u;
    app_set_plan_review_ui(a, true);
    {
        const strvec_t *ek = tui_keybindings_get(tui_keybindings_global(), "app.editor.external");
        static const char *const CTRL_G[] = {"ctrl+g", NULL};
        if (!ek) tui_keybindings_define(tui_keybindings_global(), "app.editor.external", CTRL_G);
    }
    g_pr.u = &u;
    job_t *init_job = NULL;
    if (initial) {
        init_job = xcalloc(1, sizeof(*init_job));
        init_job->u = &u;
        init_job->text = xstrdup(initial);
    }
    /* first-run setup wizard (omp main.ts: stale setupVersion, not resuming, TTY); the welcome intro
     * plays after it instead of at startup */
    if (ui_setup_pending(a->opts.resume || a->opts.continue_recent)) {
        tui_post(u.tui, setup_start_cb, init_job);
    } else {
        if (u.welcome) ui_welcome_play_intro(u.welcome, u.tui);
        if (init_job) tui_post(u.tui, initial_prompt_cb, init_job);
    }
    int code = tui_run(u.tui);
    if (u.worker_running) {
        app_abort(a);
        pthread_join(u.worker, NULL);
    }
    agent_unsubscribe(a->agent, ui_listener, &u);
    a->notice = NULL;
    a->wake = NULL;
    a->env->ui.ask = NULL;
    a->env->ui.tool_frame = NULL;
    app_set_plan_review_ui(a, false);
    if (g_pr.open) plan_review_hide(&u);
    plan_review_free_state();
    if (u.picker && tui_overlay_visible(u.tui)) tui_overlay_hide(u.tui, &u.picker->base);
    tui_free(u.tui);
    tui_ac_default_free(ac);
    hmap_free(&u.tools, NULL);
    json_free(u.pending_partial);
    if (a->session && a->session->file && a->session->on_disk)
        fprintf(stderr, "Session saved: %s\nResume with: omc -r %s\n", a->session->file, session_short_id(session_id(a->session)));
    return code;
}
