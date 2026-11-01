/* interactive mode: TUI chat (port of omp's InteractiveMode, core feature set). */
#include "modes.h"
#include "ui/ui.h"
#include "coding/goal.h"
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

/* ---------- agent events -> UI ---------- */

typedef struct {
    ui_t *u;
    agent_event_type_t type;
    json_t *msg;
    char *id, *name;
    json_t *args;
    char *out;
    bool is_error;
} uiev_t;

static void uiev_free(uiev_t *e) {
    json_free(e->msg);
    free(e->id);
    free(e->name);
    json_free(e->args);
    free(e->out);
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
    tui_request_render(u->tui);
}

static void ui_apply_event(void *ud) {
    uiev_t *e = ud;
    ui_t *u = e->u;
    switch (e->type) {
    case AGENT_EV_MESSAGE_END:
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
            u->cur_assistant = NULL;
        }
        break;
    case AGENT_EV_TOOL_START: {
        msgview_t *v = mv_new(MV_TOOL);
        v->tool_name = xstrdup(e->name);
        v->tool_id = xstrdup(e->id);
        v->args = e->args;
        e->args = NULL;
        hmap_put(&u->tools, e->id, v);
        add_view(u, v);
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
        if (e->type == AGENT_EV_TOOL_END) {
            v->done = true;
            v->is_error = e->is_error;
            hmap_del(&u->tools, e->id);
        }
        v->dirty = true;
        break;
    }
    case AGENT_EV_AGENT_END:
        ui_working_stop(u);
        ui_chrome_turn(u, false);
        break;
    case AGENT_EV_AGENT_START:
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
    if (ev->result) e->out = result_display(ev->tool_name ? ev->tool_name : "", ev->result);
    e->is_error = ev->is_error;
    tui_post(u->tui, ui_apply_event, e);
}

static void post_notice(void *x);

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

static void worker_done(void *ud) {
    ui_t *u = ud;
    pthread_join(u->worker, NULL);
    u->worker_running = false;
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

/* ---------- pickers ---------- */

static void picker_close(ui_t *u) {
    if (u->picker) tui_overlay_hide(u->tui, &u->picker->base);
    u->picker_kind = 0;
    json_free(u->picker_data);
    u->picker_data = NULL;
    tui_set_focus(u->tui, &u->editor->base);
}

static void ask_next(ui_t *u);

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

static void on_pick(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    ui_t *u = ud;
    UNUSED(l);
    int kind = u->picker_kind;
    char *val = xstrdup(it->value);
    if (kind == 3) {
        json_t *q = json_at(u->ask_questions, u->ask_index);
        json_set_str(u->ask_answers, json_get_str(q, "id") ? json_get_str(q, "id") : "answer", val);
        u->ask_index++;
        free(val);
        ask_next(u);
        return;
    }
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
    if (u->picker_kind == 3) {
        pthread_mutex_lock(&u->mu);
        json_free(u->ask_answers);
        u->ask_answers = NULL;
        u->ask_done = true;
        pthread_cond_signal(&u->ask_cv);
        pthread_mutex_unlock(&u->mu);
    }
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

static void model_picker(ui_t *u) {
    ensure_picker(u);
    tui_select_list_clear(u->picker);
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (!auth_has_credentials(m->provider)) continue;
        char *v = xasprintf("%s/%s", m->provider, m->id);
        tui_select_list_add(u->picker, v, v, m->name);
        free(v);
    }
    show_picker(u, 1);
}

static void session_picker(ui_t *u) {
    ensure_picker(u);
    tui_select_list_clear(u->picker);
    char *dir = session_dir_for_cwd(u->app->cwd);
    session_list_t l;
    session_list(dir, &l);
    for (size_t i = 0; i < l.n; i++) {
        const char *t = l.v[i].title ? l.v[i].title : l.v[i].first_message ? l.v[i].first_message : "(empty)";
        char *label = xstrndup(t, strcspn(t, "\n"));
        char *when = iso8601_from_ms(l.v[i].mtime_ms);
        char *desc = xasprintf("%.16s · %d msgs", when, l.v[i].message_count);
        tui_select_list_add(u->picker, l.v[i].path, label, desc);
        free(desc);
        free(when);
        free(label);
    }
    session_list_free(&l);
    free(dir);
    show_picker(u, 2);
}

/* ---------- ask bridge (tool thread blocks; UI thread drives pickers) ---------- */

static void ask_next(ui_t *u) {
    if (u->ask_index >= json_len(u->ask_questions)) {
        picker_close(u);
        pthread_mutex_lock(&u->mu);
        u->ask_done = true;
        pthread_cond_signal(&u->ask_cv);
        pthread_mutex_unlock(&u->mu);
        return;
    }
    json_t *q = json_at(u->ask_questions, u->ask_index);
    ensure_picker(u);
    tui_select_list_clear(u->picker);
    json_t *opt;
    json_arr_foreach(json_get_arr(q, "options"), i, opt) {
        const char *label = json_get_str(opt, "label");
        tui_select_list_add(u->picker, label, label, json_get_str(opt, "description"));
    }
    add_notice(u, "info", json_get_str(q, "question") ? json_get_str(q, "question") : "Question");
    show_picker(u, 3);
    tui_request_render(u->tui);
}

static void ask_start(void *ud) {
    ui_t *u = ud;
    u->ask_index = 0;
    ask_next(u);
}

static json_t *ui_ask(void *ud, const json_t *questions) {
    ui_t *u = ud;
    pthread_mutex_lock(&u->mu);
    u->ask_questions = json_clone(questions);
    u->ask_answers = json_obj();
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
    {"hotkeys", "Show keyboard shortcuts"},   {"quit", "Exit omc"},
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
    } else if (!strcmp(cmd, "help") || !strcmp(cmd, "hotkeys")) {
        sb_t b = SB_INIT;
        sb_append(&b, "Commands:\n");
        for (size_t i = 0; i < ARRAY_LEN(COMMANDS); i++) sb_appendf(&b, "  /%-10s %s\n", COMMANDS[i].name, COMMANDS[i].desc);
        sb_append(&b, "Keys: enter send (steer while running) · alt+enter follow-up/newline · esc interrupt · "
                      "shift+tab thinking · ctrl+o expand tools · ctrl+t toggle thinking · ctrl+l model · "
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
            model_picker(u);
        }
    } else if (!strcmp(cmd, "thinking")) {
        ai_effort_t e;
        if (*arg && effort_parse(arg, &e) == 0) {
            app_set_thinking(a, e);
        } else {
            ensure_picker(u);
            tui_select_list_clear(u->picker);
            for (int i = EFFORT_OFF; i <= EFFORT_MAX; i++)
                if (model_supports_effort(a->model, (ai_effort_t)i))
                    tui_select_list_add(u->picker, effort_name((ai_effort_t)i), effort_name((ai_effort_t)i), NULL);
            show_picker(u, 4);
        }
    } else if (!strcmp(cmd, "plan")) {
        app_set_plan_mode(a, !a->plan_mode);
        add_notice(u, "info", a->plan_mode ? "Plan mode on: read-only exploration; the agent proposes a plan for approval."
                                           : "Plan mode off.");
        if (*arg && a->plan_mode) start_prompt(u, arg);
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
        const char *role = settings_str("modelRoles.advisor", NULL);
        ml = role ? xstrdup(role) : app_model_label(a);
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
        tui_container_clear(u->chat);
        add_notice(u, "info", "New session started");
    } else if (!strcmp(cmd, "resume")) {
        session_picker(u);
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
    add_view(u, v);
    tui_render_now(u->tui);
    proc_opts_t o = {.cwd = u->app->cwd, .timeout_ms = 600000, .merge_stderr = true, .new_session = true};
    proc_result_t r;
    if (proc_shell(cmd, &o, &r) == 0) {
        v->output = utf8_sanitize(r.out, r.out_len);
        v->is_error = r.exit_code != 0;
        json_t *m = json_obj();
        json_set_str(m, "role", "bashExecution");
        json_set_str(m, "command", cmd);
        json_set_str(m, "output", v->output);
        json_set_int(m, "exitCode", r.exit_code);
        json_set_bool(m, "cancelled", false);
        json_set_bool(m, "truncated", false);
        if (exclude) json_set_bool(m, "excludeFromContext", true);
        json_set_int(m, "timestamp", now_ms());
        session_append_message(u->app->session, m);
        if (!exclude) {
            char *t = r.out_len ? xasprintf("Ran `%s`\n```\n%s\n```", cmd, v->output) : xasprintf("Ran `%s`\n(no output)", cmd);
            agent_append_message(u->app->agent, msg_user_text(t));
            free(t);
        }
        json_free(m);
        proc_result_free(&r);
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
    if (tui_overlay_visible(t)) return false;
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
    if (tui_key_matches(k, "ctrl+l")) {
        model_picker(u);
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

int run_interactive_mode(app_t *a, const char *initial) {
    ui_t u = {0};
    g_ui = &u;
    u.app = a;
    u.hide_thinking = settings_bool("hideThinkingBlock", false);
    pthread_mutex_init(&u.mu, NULL);
    pthread_cond_init(&u.ask_cv, NULL);
    hmap_init(&u.tools);
    char *terr = NULL;
    const char *tname = settings_str(tui_detect_theme_name() && !strcmp(tui_detect_theme_name(), "light") ? "theme.light" : "theme.dark",
                                     tui_detect_theme_name());
    tui_theme_t *th = tui_theme_load(tname ? tname : "dark", tui_detect_color_mode(), &terr);
    if (th) tui_theme_set(th);
    free(terr);
    u.tui = tui_new(NULL);
    if (!u.tui) {
        fprintf(stderr, "omc: cannot initialize terminal (not a TTY?). Use -p for print mode.\n");
        return 1;
    }
    tui_container_t *root = tui_root(u.tui);
    tui_sym_set_preset(settings_str("symbolPreset", "unicode"));
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
    for (size_t i = 0; i < ARRAY_LEN(COMMANDS); i++) tui_ac_default_add_command(ac, COMMANDS[i].name, COMMANDS[i].desc);
    tui_editor_set_autocomplete(u.editor, tui_ac_default_provider, ac);
    tui_container_add(root, &u.editor->base);
    u.status = ui_status_new(&u);
    tui_container_add(root, u.status);
    tui_set_focus(u.tui, &u.editor->base);
    tui_set_key_hook(u.tui, key_hook, &u);
    /* replay resumed history */
    ui_chat_rebuild(&u);
    agent_subscribe(a->agent, ui_listener, &u);
    a->notice = ui_notice;
    a->notice_ud = &u;
    a->wake = ui_wake;
    a->wake_ud = &u;
    a->env->ui.ask = ui_ask;
    a->env->ui.ud = &u;
    if (initial) {
        job_t *j = xcalloc(1, sizeof(*j));
        j->u = &u;
        j->text = xstrdup(initial);
        tui_post(u.tui, initial_prompt_cb, j);
    }
    if (u.welcome) ui_welcome_play_intro(u.welcome, u.tui);
    int code = tui_run(u.tui);
    if (u.worker_running) {
        app_abort(a);
        pthread_join(u.worker, NULL);
    }
    agent_unsubscribe(a->agent, ui_listener, &u);
    a->notice = NULL;
    a->wake = NULL;
    a->env->ui.ask = NULL;
    if (u.picker && tui_overlay_visible(u.tui)) tui_overlay_hide(u.tui, &u.picker->base);
    tui_free(u.tui);
    tui_ac_default_free(ac);
    hmap_free(&u.tools, NULL);
    json_free(u.pending_partial);
    if (a->session && a->session->file && a->session->on_disk)
        fprintf(stderr, "Session saved: %s\nResume with: omc -r %s\n", a->session->file, session_short_id(session_id(a->session)));
    return code;
}
