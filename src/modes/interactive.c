/* interactive mode: TUI chat (port of omp's InteractiveMode, core feature set). */
#include "modes.h"
#include "coding/goal.h"

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

/* ---------- message view component ---------- */

typedef enum { MV_USER, MV_ASSISTANT, MV_TOOL, MV_NOTICE, MV_BASH } mv_kind_t;

typedef struct {
    tui_component_t base;
    mv_kind_t kind;
    json_t *msg;        /* assistant message (owned clone) */
    char *text;         /* user/notice text */
    const char *level;  /* notice level */
    /* tool */
    char *tool_name, *tool_id;
    json_t *args;
    char *output;
    bool done, is_error;
    /* cache */
    int cache_w;
    strvec_t cache;
    bool dirty;
} msgview_t;

typedef struct ui ui_t;
static ui_t *g_ui;

struct ui {
    app_t *app;
    tui_t *tui;
    tui_container_t *chat;
    tui_editor_t *editor;
    tui_loader_t *loader;
    tui_component_t *status;
    msgview_t *cur_assistant;
    hmap_t tools; /* toolCallId -> msgview_t* */
    pthread_t worker;
    bool worker_running;
    bool expand_tools;
    bool hide_thinking;
    int64_t last_ctrl_c;
    /* coalesced streaming updates */
    pthread_mutex_t mu;
    json_t *pending_partial;
    bool partial_posted;
    /* ask bridge */
    pthread_cond_t ask_cv;
    json_t *ask_questions;
    json_t *ask_answers;
    size_t ask_index;
    bool ask_done;
    tui_select_list_t *picker;
    char *goal_cont; /* pending goal continuation prompt */
    int picker_kind; /* 1 model, 2 session, 3 ask, 4 thinking */
    json_t *picker_data;
};

static const tui_theme_t *TH(void) { return tui_theme(); }

static char *fg(tui_tc_t slot, const char *s) { return tui_fgs(TH(), slot, s); }

static void push_wrapped(strvec_t *out, const char *text, int width, int indent) {
    strvec_t w = {0};
    tui_wrap_text_with_ansi(text, MAX(10, width - indent), &w);
    char *pad = xmalloc((size_t)indent + 1);
    memset(pad, ' ', (size_t)indent);
    pad[indent] = 0;
    for (size_t i = 0; i < w.n; i++) strvec_push_own(out, xasprintf("%s%s", pad, w.v[i]));
    free(pad);
    strvec_free(&w);
}

static char *args_summary(const char *tool, const json_t *args) {
    const char *keys[] = {"path", "command", "pattern", "query", "url", "op", "input"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        const char *v = json_get_str(args, keys[i]);
        if (v) {
            char *first = xstrndup(v, strcspn(v, "\n"));
            if (strlen(first) > 120) strcpy(first + 117, "...");
            if (!strcmp(tool, "edit") && !strcmp(keys[i], "input")) {
                const char *lb = strchr(v, '['), *rb = lb ? strchr(lb, ']') : NULL;
                if (lb && rb) {
                    free(first);
                    first = xstrndup(lb + 1, (size_t)(rb - lb - 1));
                }
            }
            return first;
        }
    }
    json_t *tasks = json_get_arr(args, "tasks");
    if (tasks) return xasprintf("%zu task(s)", json_len(tasks));
    char *s = json_dump(args, 0);
    if (strlen(s) > 100) strcpy(s + 97, "...");
    return s;
}

static void render_tool(msgview_t *v, int width, strvec_t *out) {
    tui_tc_t bg = !v->done ? TUI_TC_TOOL_PENDING_BG : v->is_error ? TUI_TC_TOOL_ERROR_BG : TUI_TC_TOOL_SUCCESS_BG;
    char *sum = args_summary(v->tool_name, v->args);
    const char *icon = !v->done ? "…" : v->is_error ? "✗" : "✓";
    char *t1 = fg(TUI_TC_TOOL_TITLE, v->tool_name);
    char *b1 = tui_bold(t1);
    char *t2 = fg(TUI_TC_MUTED, sum);
    char *title = xasprintf(" %s %s %s", icon, b1, t2);
    strvec_t lines = {0};
    push_wrapped(&lines, title, width - 1, 0);
    if (v->output && *v->output) {
        strvec_t ol = str_split_lines(v->output, strlen(v->output));
        size_t max = g_ui && g_ui->expand_tools ? ol.n : 10;
        size_t start = ol.n > max ? ol.n - max : 0;
        if (start) {
            char *m = xasprintf("   … %zu earlier lines (ctrl+o to expand)", start);
            char *mm = fg(TUI_TC_DIM, m);
            strvec_push_own(&lines, mm);
            free(m);
        }
        for (size_t i = start; i < ol.n; i++) {
            char *clean = tui_replace_tabs(ol.v[i]);
            char *stripped = tui_strip_ansi(clean);
            tui_tc_t slot = TUI_TC_TOOL_OUTPUT;
            if (!strcmp(v->tool_name, "edit") || !strcmp(v->tool_name, "write")) {
                if (stripped[0] == '+') slot = TUI_TC_TOOL_DIFF_ADDED;
                else if (stripped[0] == '-') slot = TUI_TC_TOOL_DIFF_REMOVED;
            }
            char *c = fg(slot, stripped);
            char *line = xasprintf("   %s", c);
            char *tr = tui_truncate_to_width(line, width - 1, "…");
            strvec_push_own(&lines, tr);
            free(line);
            free(c);
            free(stripped);
            free(clean);
        }
        strvec_free(&ol);
    }
    for (size_t i = 0; i < lines.n; i++) {
        char *padded = tui_pad_to_width(lines.v[i], width);
        char *b = tui_bgs(TH(), bg, padded);
        strvec_push_own(out, b);
        free(padded);
    }
    strvec_free(&lines);
    free(title);
    free(t2);
    free(b1);
    free(t1);
    free(sum);
}

static void mv_render(tui_component_t *c, int width, strvec_t *out) {
    msgview_t *v = (msgview_t *)c;
    if (!v->dirty && v->cache_w == width) {
        for (size_t i = 0; i < v->cache.n; i++) strvec_push(out, v->cache.v[i]);
        return;
    }
    strvec_free(&v->cache);
    strvec_t *o = &v->cache;
    switch (v->kind) {
    case MV_USER: {
        strvec_t lines = {0};
        tui_md_opts_t mo;
        tui_md_opts_default(&mo);
        tui_md_render(v->text, width - 2, &mo, &lines);
        strvec_push(o, "");
        for (size_t i = 0; i < lines.n; i++) {
            char *l = xasprintf(" %s", lines.v[i]);
            char *p = tui_pad_to_width(l, width);
            strvec_push_own(o, tui_bgs(TH(), TUI_TC_USER_MESSAGE_BG, p));
            free(p);
            free(l);
        }
        strvec_free(&lines);
        break;
    }
    case MV_ASSISTANT: {
        json_t *content = json_get(v->msg, "content"), *blk;
        bool first = true;
        json_arr_foreach(content, i, blk) {
            const char *t = json_get_str(blk, "type");
            if (str_eq(t, "thinking")) {
                if (g_ui->hide_thinking) continue;
                const char *th = json_get_str(blk, "thinking");
                if (str_empty(th)) continue;
                if (first) strvec_push(o, "");
                first = false;
                strvec_t tl = {0};
                tui_wrap_text_with_ansi(th, width - 2, &tl);
                for (size_t k = 0; k < tl.n; k++) {
                    char *it = tui_italic(tl.v[k]);
                    char *c2 = fg(TUI_TC_THINKING_TEXT, it);
                    strvec_push_own(o, xasprintf(" %s", c2));
                    free(c2);
                    free(it);
                }
                strvec_free(&tl);
            } else if (str_eq(t, "text")) {
                const char *tx = json_get_str(blk, "text");
                if (str_empty(tx)) continue;
                if (first) strvec_push(o, "");
                first = false;
                tui_md_opts_t mo;
                tui_md_opts_default(&mo);
                strvec_t ml = {0};
                tui_md_render(tx, width - 2, &mo, &ml);
                for (size_t k = 0; k < ml.n; k++) strvec_push_own(o, xasprintf(" %s", ml.v[k]));
                strvec_free(&ml);
            }
        }
        const char *stop = json_get_str(v->msg, "stopReason");
        const char *err = json_get_str(v->msg, "errorMessage");
        if ((str_eq(stop, "error") || str_eq(stop, "aborted")) ) {
            char *m = xasprintf(" %s: %s", str_eq(stop, "aborted") ? "Aborted" : "Error", err ? err : stop);
            char *c2 = fg(str_eq(stop, "aborted") ? TUI_TC_WARNING : TUI_TC_ERROR, m);
            push_wrapped(o, c2, width, 0);
            free(c2);
            free(m);
        }
        break;
    }
    case MV_TOOL:
        strvec_push(o, "");
        render_tool(v, width, o);
        break;
    case MV_NOTICE: {
        tui_tc_t slot = str_eq(v->level, "error") ? TUI_TC_ERROR : str_eq(v->level, "warning") ? TUI_TC_WARNING : TUI_TC_MUTED;
        char *c2 = fg(slot, v->text);
        strvec_push(o, "");
        push_wrapped(o, c2, width, 1);
        free(c2);
        break;
    }
    case MV_BASH:
        strvec_push(o, "");
        render_tool(v, width, o);
        break;
    }
    v->cache_w = width;
    v->dirty = false;
    for (size_t i = 0; i < v->cache.n; i++) strvec_push(out, v->cache.v[i]);
}

static void mv_invalidate(tui_component_t *c) { ((msgview_t *)c)->dirty = true; }

static void mv_destroy(tui_component_t *c) {
    msgview_t *v = (msgview_t *)c;
    json_free(v->msg);
    free(v->text);
    free(v->tool_name);
    free(v->tool_id);
    json_free(v->args);
    free(v->output);
    strvec_free(&v->cache);
    free(v);
}

static const tui_component_vt MV_VT = {"message", mv_render, NULL, mv_invalidate, mv_destroy};

static msgview_t *mv_new(mv_kind_t k) {
    msgview_t *v = xcalloc(1, sizeof(*v));
    tui_component_init(&v->base, &MV_VT);
    v->kind = k;
    v->dirty = true;
    return v;
}

static void add_view(ui_t *u, msgview_t *v) {
    tui_container_add(u->chat, &v->base);
    tui_request_render(u->tui);
}

static void add_notice(ui_t *u, const char *level, const char *text) {
    msgview_t *v = mv_new(MV_NOTICE);
    v->level = level;
    v->text = xstrdup(text);
    add_view(u, v);
}

/* ---------- status line ---------- */

static void status_render(tui_component_t *c, int width, strvec_t *out) {
    ui_t *u = c->user;
    app_t *a = u->app;
    char *model = app_model_label(a);
    char *cwd = path_display(a->cwd, NULL);
    json_t *st = app_stats(a);
    double pct = json_get_num(st, "contextPercent", 0);
    double cost = json_get_num(st, "cost", 0);
    json_free(st);
    tui_tc_t tslot = (tui_tc_t)(TUI_TC_THINKING_OFF + (int)a->thinking);
    char *m = fg(TUI_TC_STATUS_LINE_MODEL, model);
    char *t = fg(tslot, effort_name(a->thinking));
    char *p = fg(TUI_TC_STATUS_LINE_PATH, cwd);
    char ctx[64], cs[32];
    snprintf(ctx, sizeof ctx, "%.1f%%", pct);
    snprintf(cs, sizeof cs, "$%.3f", cost);
    char *cx = fg(TUI_TC_STATUS_LINE_CONTEXT, ctx);
    char *co = fg(TUI_TC_STATUS_LINE_COST, cs);
    char *sep = fg(TUI_TC_STATUS_LINE_SEP, " · ");
    char *line = xasprintf(" %s%s%s%s%s%s%s%s%s%s", a->plan_mode ? "[plan] " : app_goal(a) && goal_enabled(app_goal(a)) ? "[goal] " : app_goal(a) ? "[goal paused] " : "", m, sep, t, sep, p, sep, cx, sep, co);
    if (a->session && a->session->title) {
        char *tt = fg(TUI_TC_MUTED, a->session->title);
        char *l2 = xasprintf("%s%s%s", line, sep, tt);
        free(line);
        line = l2;
        free(tt);
    }
    strvec_push_own(out, tui_truncate_to_width(line, width, "…"));
    free(line);
    free(sep);
    free(co);
    free(cx);
    free(p);
    free(t);
    free(m);
    free(cwd);
    free(model);
}

static const tui_component_vt STATUS_VT = {"status", status_render, NULL, NULL, NULL};

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
        add_view(u, u->cur_assistant);
    }
    json_free(u->cur_assistant->msg);
    u->cur_assistant->msg = p;
    u->cur_assistant->dirty = true;
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
            u->cur_assistant->dirty = true;
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
        tui_loader_stop(u->loader);
        u->loader->base.hidden = true;
        break;
    case AGENT_EV_AGENT_START:
        u->loader->base.hidden = false;
        tui_loader_set_message(u->loader, "Working… (esc to interrupt)");
        tui_loader_start(u->loader);
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
    add_notice(e->u, lvl, e->out);
    tui_request_render(e->u->tui);
    uiev_free(e);
}

/* ---------- worker ---------- */

typedef struct {
    ui_t *u;
    char *text;
    bool hidden; /* goal continuation */
} job_t;

static void start_hidden(ui_t *u, const char *text);

static void goal_continue_cb(void *ud) {
    ui_t *u = ud;
    char *c = u->goal_cont;
    u->goal_cont = NULL;
    if (!c) return;
    char *draft = tui_editor_get_text(u->editor);
    bool busy = u->worker_running || (draft && *draft) || u->picker_kind;
    free(draft);
    if (!busy) start_hidden(u, c);
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
    if (j->hidden) app_prompt_hidden(a, j->text, "goal-continuation");
    else app_prompt(a, j->text, NULL);
    json_t *run = json_arr();
    pthread_mutex_lock(&a->agent->mu);
    for (size_t i = before; i < json_len(a->agent->messages); i++) json_push(run, json_clone(json_at(a->agent->messages, i)));
    pthread_mutex_unlock(&a->agent->mu);
    char *cont = app_goal_after_run(a, run, j->hidden, "interactive");
    json_free(run);
    free(j->u->goal_cont);
    j->u->goal_cont = cont;
    tui_post(j->u->tui, worker_done, j->u);
    free(j->text);
    free(j);
    return NULL;
}

static void start_hidden(ui_t *u, const char *text) {
    job_t *j = xcalloc(1, sizeof(*j));
    j->u = u;
    j->text = xstrdup(text);
    j->hidden = true;
    u->worker_running = true;
    if (pthread_create(&u->worker, NULL, worker_main, j) != 0) {
        u->worker_running = false;
        free(j->text);
        free(j);
    }
}

static void start_prompt(ui_t *u, const char *text) {
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
            tui_container_clear(u->chat);
            json_t *m;
            json_arr_foreach(u->app->agent->messages, i, m) {
                const char *role = json_get_str(m, "role");
                if (str_eq(role, "user")) {
                    msgview_t *v = mv_new(MV_USER);
                    v->text = ai_message_text(m);
                    add_view(u, v);
                } else if (str_eq(role, "assistant")) {
                    msgview_t *v = mv_new(MV_ASSISTANT);
                    v->msg = json_clone(m);
                    add_view(u, v);
                }
            }
            add_notice(u, "info", "Session resumed");
        }
        free(err);
    } else if (kind == 4) {
        ai_effort_t e;
        if (effort_parse(val, &e) == 0) app_set_thinking(u->app, e);
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
    {"resume", "Resume a previous session"},  {"compact", "Compact the context [instructions]"},
    {"session", "Show session info"},         {"name", "Set session title"},
    {"export", "Export session [path]"},      {"copy", "Copy last answer to clipboard"},
    {"fork", "Fork the session here"},        {"clear", "Clear the screen and start a new session"},
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
            if (c && !u->worker_running) start_hidden(u, c);
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
    } else if (!strcmp(cmd, "new") || !strcmp(cmd, "clear")) {
        app_new_session(a);
        tui_container_clear(u->chat);
        if (!strcmp(cmd, "clear")) tui_request_full_render(u->tui);
        add_notice(u, "info", "New session started");
    } else if (!strcmp(cmd, "resume")) {
        session_picker(u);
    } else if (!strcmp(cmd, "compact")) {
        char *err = NULL;
        add_notice(u, "info", "Compacting…");
        tui_render_now(u->tui);
        if (app_compact(a, *arg ? arg : NULL, &err) != 0) add_notice(u, "error", err ? err : "compaction failed");
        else add_notice(u, "info", "Context compacted");
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
    char *ml = app_model_label(a);
    char *hdr = xasprintf("**omc** %s · %s · /help for commands", OMC_VERSION, ml);
    free(ml);
    tui_container_add(root, &tui_markdown_new(hdr, 1, 0)->base);
    free(hdr);
    if (a->disc.files.n) {
        sb_t b = SB_INIT;
        sb_append(&b, "Loaded context:");
        for (size_t i = 0; i < a->disc.files.n; i++) {
            char *d = path_display(a->disc.files.v[i].path, a->cwd);
            sb_appendf(&b, " %s", d);
            free(d);
        }
        char *c = fg(TUI_TC_DIM, sb_cstr(&b));
        tui_container_add(root, &tui_text_new(c, 1, 0)->base);
        free(c);
        sb_free(&b);
    }
    u.chat = tui_container_new();
    tui_container_add(root, &u.chat->base);
    u.loader = tui_loader_new(u.tui, "Working…");
    u.loader->base.hidden = true;
    tui_container_add(root, &u.loader->base);
    tui_container_add(root, &tui_spacer_new(1)->base);
    u.editor = tui_editor_new();
    tui_editor_set_on_submit(u.editor, on_submit, &u);
    tui_editor_set_placeholder(u.editor, "Ask omc anything… (/help)");
    history_load(u.editor);
    tui_ac_default_t *ac = tui_ac_default_new(a->cwd);
    for (size_t i = 0; i < ARRAY_LEN(COMMANDS); i++) tui_ac_default_add_command(ac, COMMANDS[i].name, COMMANDS[i].desc);
    tui_editor_set_autocomplete(u.editor, tui_ac_default_provider, ac);
    tui_container_add(root, &u.editor->base);
    u.status = xcalloc(1, sizeof(tui_component_t));
    tui_component_init(u.status, &STATUS_VT);
    u.status->user = &u;
    tui_container_add(root, u.status);
    tui_set_focus(u.tui, &u.editor->base);
    tui_set_key_hook(u.tui, key_hook, &u);
    /* replay resumed history */
    json_t *m;
    json_arr_foreach(a->agent->messages, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "user") && json_get_bool(m, "synthetic", false)) {
            continue;
        } else if (str_eq(role, "user")) {
            msgview_t *v = mv_new(MV_USER);
            v->text = ai_message_text(m);
            tui_container_add(u.chat, &v->base);
        } else if (str_eq(role, "assistant")) {
            msgview_t *v = mv_new(MV_ASSISTANT);
            v->msg = json_clone(m);
            tui_container_add(u.chat, &v->base);
        }
    }
    agent_subscribe(a->agent, ui_listener, &u);
    a->notice = ui_notice;
    a->notice_ud = &u;
    a->env->ui.ask = ui_ask;
    a->env->ui.ud = &u;
    if (initial) {
        job_t *j = xcalloc(1, sizeof(*j));
        j->u = &u;
        j->text = xstrdup(initial);
        tui_post(u.tui, initial_prompt_cb, j);
    }
    int code = tui_run(u.tui);
    if (u.worker_running) {
        app_abort(a);
        pthread_join(u.worker, NULL);
    }
    agent_unsubscribe(a->agent, ui_listener, &u);
    a->notice = NULL;
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
