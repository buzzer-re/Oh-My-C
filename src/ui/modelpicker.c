/* modelpicker.c - the session-only model picker (Alt+P / `/switch`): port of omp overlays/model-picker.ts
 * (ModelPickerComponent.render) with the host callbacks of modes/controllers/selector-controller.ts
 * (#showModelPicker, switchSessionModel, #applySessionModel).
 *
 * A bottom-anchored full-width overlay over the transcript: rounded box titled "Switch Model", a status
 * row, the model browser over the available chat models (current model marked and preselected, models
 * whose context window is below the session's tokens grayed: picking one compacts with the current model
 * first), and a key-hint footer. A search starting with `@` lists the Ctrl+P quick roles (colored by their
 * cycle-track segment); pressing the picker key again toggles task mode (the pick becomes a session-only
 * task.agentModelOverrides.task override, red chrome). */
#include "ui/modelbrowser.h"
#include "ui/overlays.h"
#include "ui/ui.h"

#include "config/settings.h"
#include "modes/interactive.h"

#define CHROME_ROWS 4
#define BROWSER_FRAME_ROWS 5
#define MIN_VISIBLE 5

#define STATUS_HINT "Session-only switch \xe2\x80\x94 role models stay unchanged"
#define QUICK_ROLE_STATUS_HINT "Quick role switch \xe2\x80\x94 applies its model and thinking for this session"
#define TASK_STATUS_HINT "Task subagent switch \xe2\x80\x94 spawned task agents use this model (session-only)"

typedef struct {
    tui_component_t base;
    ui_t *u;
    ui_model_browser_t *b;
    VEC(const ai_model_t *) models; /* session scope, sortModelItems order */
    app_role_entry_t *roles;        /* quick roles (getRoleModelCycle) */
    size_t nroles;
    strvec_t order; /* cycleOrder (segment colors) */
    char *current, *current_role, *task_selector;
    bool role_mode, task_mode;
    char *last_query;
} picker_t;

static picker_t *g_open; /* #focusOpenMenu("model-picker") */

static const char *query_of(const picker_t *p) { return p->b->query ? p->b->query : ""; }

static void add_items(picker_t *p, bool roles) {
    ui_model_browser_clear_items(p->b);
    if (roles) {
        int pal[12];
        size_t np = ui_segment_palette(p->order.n ? p->order.n : p->nroles, pal);
        for (size_t i = 0; i < p->nroles; i++) {
            char *sel = xasprintf("@%s", p->roles[i].role);
            long oi = -1;
            for (size_t k = 0; k < p->order.n; k++)
                if (!strcmp(p->order.v[k], p->roles[i].role)) oi = (long)k;
            int color = np ? pal[(oi >= 0 ? (size_t)oi : i) % np] : -1;
            ui_model_browser_add_labeled(p->b, p->roles[i].model, sel, sel, color);
            free(sel);
        }
    } else
        for (size_t i = 0; i < p->models.n; i++) ui_model_browser_add(p->b, p->models.v[i]);
}

/* #syncItemsForQuery: swap model rows for quick-role rows when a leading `@` toggles the search mode */
static void sync_items(picker_t *p, bool refresh) {
    bool role_mode = query_of(p)[0] == '@' && !p->task_mode;
    bool changed = role_mode != p->role_mode;
    if (!changed && !refresh) return;
    p->role_mode = role_mode;
    p->b->show_provider = !role_mode;
    p->b->mark_over_context = !role_mode && !p->task_mode;
    const char *cur = role_mode ? p->current_role : p->task_mode ? p->task_selector : p->current;
    const ui_model_item_t *sel = ui_model_browser_selected(p->b);
    char *keep = sel ? xstrdup(sel->selector) : NULL;
    add_items(p, role_mode);
    free(p->b->current);
    p->b->current = cur ? xstrdup(cur) : NULL;
    ui_model_browser_done_adding(p->b);
    if (!(changed && cur && ui_model_browser_select(p->b, cur)) && keep) ui_model_browser_select(p->b, keep);
    free(keep);
    free(p->b->empty_text);
    p->b->empty_text = NULL;
    if (role_mode) {
        char *k = ui_editor_key("app.model.cycleForward");
        if (!*k) {
            free(k);
            k = ui_key_hint("ctrl+p");
        }
        p->b->empty_text = xasprintf("  No quick roles in the %s cycle", k);
        free(k);
    }
}

static char *footer(const picker_t *p) {
    char *up = ui_editor_key("tui.select.up"), *down = ui_editor_key("tui.select.down");
    char *enter = ui_key_hint("enter"), *cancel = ui_editor_key("tui.select.cancel");
    char *r;
    if (p->task_mode)
        r = xasprintf("%s/%s models \xc2\xb7 %s use for Task subagents \xc2\xb7 type to search \xc2\xb7 %s close", up, down, enter, cancel);
    else if (p->role_mode)
        r = xasprintf("%s/%s roles \xc2\xb7 %s apply role model \xc2\xb7 type to search \xc2\xb7 %s close", up, down, enter, cancel);
    else
        r = xasprintf("%s/%s models \xc2\xb7 %s use for this session \xc2\xb7 type to search \xc2\xb7 @ quick roles \xc2\xb7 %s close", up,
                      down, enter, cancel);
    char *tk = ui_editor_key("app.model.selectTemporary");
    if (*tk && !p->role_mode) {
        char *x = xasprintf("%s \xc2\xb7 %s %s", r, tk, p->task_mode ? "session model" : "task model");
        free(r);
        r = x;
    }
    free(tk);
    free(up);
    free(down);
    free(enter);
    free(cancel);
    return r;
}

static void picker_render(tui_component_t *c, int width, strvec_t *out) {
    picker_t *p = (picker_t *)c;
    int term_rows = tui_term(p->u->tui)->rows;
    if (term_rows < 16) term_rows = 16;
    int budget = (int)(term_rows * 0.4) - CHROME_ROWS - BROWSER_FRAME_ROWS;
    ui_model_browser_set_max_visible(p->b, MAX(MIN_VISIBLE, budget));
    int inner = MAX(1, width - 4);
    int tone = p->task_mode ? (int)TUI_TC_ERROR : -1;
    int prev = ui_box_tone(tone);
    char *status = p->task_mode ? tui_fgs(NULL, TUI_TC_ERROR, " " TASK_STATUS_HINT)
                                : tui_fgs(NULL, TUI_TC_MUTED, p->role_mode ? " " QUICK_ROLE_STATUS_HINT : " " STATUS_HINT);
    strvec_push_own(out, ui_box_top(width, p->task_mode ? "Switch Task Model" : "Switch Model"));
    strvec_push_own(out, ui_box_row(status, width));
    free(status);
    strvec_t lines = {0};
    ui_model_browser_render(p->b, inner, &lines);
    for (size_t i = 0; i < lines.n; i++) strvec_push_own(out, ui_box_row(lines.v[i], width));
    strvec_free(&lines);
    char *f = footer(p), *fd = tui_fgs(NULL, TUI_TC_DIM, f);
    strvec_push_own(out, ui_box_row(fd, width));
    free(fd);
    free(f);
    strvec_push_own(out, ui_box_bottom(width));
    ui_box_tone(prev);
}

static void picker_close(picker_t *p) {
    if (g_open == p) g_open = NULL;
    ui_t *u = p->u;
    tui_overlay_hide(u->tui, &p->base);
    tui_component_destroy(&p->base);
    tui_request_render(u->tui);
}

/* #applySessionModel: session-only switch, compacting with the current model first when over context */
typedef struct {
    const ai_model_t *m;
    char *level;
} pending_t;

static void apply_now(ui_t *u, const ai_model_t *m, const char *level) {
    app_t *a = u->app;
    const char *lv = level ? level : app_temporary_thinking(m);
    char *err = NULL;
    if (app_set_model_temporary(a, m, lv, NULL, &err) != 0) {
        add_notice(u, "error", err);
        free(err);
        return;
    }
    char *hint = ui_editor_key("app.model.select");
    if (!*hint) {
        free(hint);
        hint = ui_key_hint("alt+m");
    }
    char *msg = xasprintf("Session-only model: %s/%s. Use %s or /model for roles.", m->provider, m->id, hint);
    add_notice(u, "info", msg);
    free(msg);
    free(hint);
    tui_request_render(u->tui);
}

static void after_compact(ui_t *u, bool ok, void *ud) {
    pending_t *pd = ud;
    if (ok) apply_now(u, pd->m, pd->level);
    free(pd->level);
    free(pd);
}

void ui_switch_session_model(ui_t *u, const ai_model_t *m, const char *level) {
    int64_t used = ui_context_used(u->app);
    if (m->context_window > 0 && used > m->context_window) {
        pending_t *pd = xcalloc(1, sizeof *pd);
        pd->m = m;
        pd->level = level ? xstrdup(level) : NULL;
        iq_compact_then(u, after_compact, pd);
        return;
    }
    apply_now(u, m, level);
}

static void pick_task(picker_t *p, const char *selector) {
    /* cfgTaskAgentModelOverrides.override: a runtime-layer `task` entry, never persisted */
    json_t *cur = settings_get("task.agentModelOverrides");
    json_t *o = json_is_obj(cur) ? json_clone(cur) : json_obj();
    json_set_str(o, "task", selector);
    settings_set_runtime("task.agentModelOverrides", o);
    char *msg = xasprintf("Task subagent model (session-only): %s. Use /agents to persist.", selector);
    add_notice(p->u, "info", msg);
    free(msg);
}

static void activate(picker_t *p, const ui_model_item_t *it) {
    ui_t *u = p->u;
    for (size_t i = 0; i < p->nroles; i++) {
        char *sel = xasprintf("@%s", p->roles[i].role);
        bool hit = !strcmp(sel, it->selector);
        free(sel);
        if (!hit) continue;
        /* onPickRole: applyRoleModel + the cycle track */
        app_role_entry_t e = p->roles[i];
        e.role = xstrdup(e.role);
        int idx = e.order_index;
        picker_close(p);
        app_apply_role_entry(u->app, &e);
        free(e.role);
        ui_show_model_cycle_track(u, idx);
        tui_request_render(u->tui);
        return;
    }
    if (p->task_mode) {
        char *sel = xstrdup(it->selector);
        pick_task(p, sel);
        free(sel);
        picker_close(p);
        return;
    }
    const ai_model_t *m = it->model;
    bool over = ui_model_browser_over_context(p->b, it);
    picker_close(p); /* an over-context pick closes first so the compaction loader shows */
    (void)over;
    ui_switch_session_model(u, m, NULL);
}

static bool picker_key(tui_component_t *c, const tui_key_t *k) {
    picker_t *p = (picker_t *)c;
    if (k->kind == TUI_KEY_MOUSE) return true; /* keyboard-only outside fullscreen overlays */
    if (tui_kb(k, "app.model.selectTemporary")) {
        p->task_mode = !p->task_mode;
        sync_items(p, true);
        const char *target = p->task_mode ? p->task_selector : p->current;
        if (target) ui_model_browser_select(p->b, target);
        tui_request_render(p->u->tui);
        return true;
    }
    int r = ui_model_browser_key(p->b, k);
    if (strcmp(p->last_query, query_of(p))) {
        free(p->last_query);
        p->last_query = xstrdup(query_of(p));
        sync_items(p, false);
    }
    if (r == UI_MB_CANCEL) {
        picker_close(p);
        return true;
    }
    if (r == UI_MB_ACTIVATE) {
        const ui_model_item_t *it = ui_model_browser_selected(p->b);
        if (it) activate(p, it);
        return true;
    }
    tui_request_render(p->u->tui);
    return true;
}

static void picker_destroy(tui_component_t *c) {
    picker_t *p = (picker_t *)c;
    ui_model_browser_free(p->b);
    vec_free(&p->models);
    app_role_cycle_free(p->roles, p->nroles);
    strvec_free(&p->order);
    free(p->current);
    free(p->current_role);
    free(p->task_selector);
    free(p->last_query);
    free(p);
}

static const tui_component_vt PICKER_VT = {"model-picker", picker_render, picker_key, NULL, picker_destroy};

void ui_overlay_model_picker(ui_t *u) {
    if (g_open) {
        tui_set_focus(u->tui, &g_open->base);
        return;
    }
    app_t *a = u->app;
    picker_t *p = xcalloc(1, sizeof *p);
    tui_component_init(&p->base, &PICKER_VT);
    p->u = u;
    p->b = ui_session_model_browser();
    for (size_t i = 0; i < p->b->items.n; i++) vec_push(&p->models, p->b->items.v[i].model);
    p->b->ctx_tokens = ui_context_used(a);
    p->b->mark_over_context = true;
    p->order = app_cycle_order();
    int cur = 0;
    p->nroles = app_role_cycle(a, &p->roles, &cur);
    if (p->nroles) p->current_role = xasprintf("@%s", p->roles[cur].role);
    if (a->model) p->current = xasprintf("%s/%s", a->model->provider, a->model->id);
    /* the effective Task model: the configured override, else the session model */
    json_t *ov = json_get(settings_get("task.agentModelOverrides"), "task");
    const char *ts = json_is_str(ov) ? json_sv(ov) : json_is_arr(ov) && json_len(ov) ? json_sv(json_at(ov, 0)) : NULL;
    p->task_selector = ts ? xstrdup(ts) : p->current ? xstrdup(p->current) : NULL;
    p->last_query = xstrdup("");
    free(p->b->current);
    p->b->current = p->current ? xstrdup(p->current) : NULL;
    ui_model_browser_done_adding(p->b);
    if (p->current) ui_model_browser_select(p->b, p->current);
    g_open = p;
    tui_overlay_opts_t o = {TUI_ANCHOR_BOTTOM, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &p->base, &o);
    tui_overlay_fill_screen(u->tui, &p->base); /* bottom-center against the full terminal, over a short transcript */
    tui_set_focus(u->tui, &p->base);
    tui_request_render(u->tui);
}
