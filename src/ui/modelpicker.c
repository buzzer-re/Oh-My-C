/* modelpicker.c - the session-only model picker (Alt+P / `/switch`): port of omp overlays/model-picker.ts
 * (ModelPickerComponent.render) with the host callbacks of modes/controllers/selector-controller.ts
 * (#showModelPicker, switchSessionModel, #applySessionModel).
 *
 * A bottom-anchored full-width overlay over the transcript: rounded box titled "Switch Model", a status
 * row, the model browser over the available chat models (current model marked and preselected, models
 * whose context window is below the session's tokens grayed: picking one compacts with the current model
 * first), and a key-hint footer. A search starting with `@` lists the Ctrl+P quick roles (colored by their
 * cycle-track segment); pressing the picker key again toggles task mode (the pick becomes a session-only
 * task.agentModelOverrides.task override, red chrome). Tab / Shift+Tab step the list through all models
 * and each provider's; in session mode ←/→ move a thinking slider for the highlighted model (shown after
 * the status hint), whose level the pick applies (omp 807c6d37f5 / 94a10c5b22). */
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
    char *scope;    /* NULL: all models; else the provider listed */
    char *thinking; /* the slider's pending level (#thinking); NULL until moved */
} picker_t;

/* ----- the thinking slider (model-picker.ts thinkingSteps / thinkingStep / #slider) ----- */

#define MAX_STEPS 9
typedef struct {
    const char *steps[MAX_STEPS];
    size_t n;
    const char *value;
    bool disabled;
} slider_t;

static const char *step_label(const char *id) {
    static const char *const L[][2] = {{"auto", "Auto"}, {"off", "Off"},   {"minimal", "Min"},  {"low", "Low"},
                                       {"medium", "Med"}, {"high", "High"}, {"xhigh", "XHigh"}, {"max", "Max"}};
    for (size_t i = 0; i < ARRAY_LEN(L); i++)
        if (str_eq(id, L[i][0])) return L[i][1];
    return id;
}

/* `model`'s steps (auto, off, its efforts), frozen at off..high when it has no effort to pick */
static void thinking_steps(const ai_model_t *m, slider_t *s) {
    s->n = 0;
    s->disabled = false;
    if (m->reasoning) {
        s->steps[s->n++] = "auto";
        s->steps[s->n++] = "off";
        for (int e = EFFORT_MINIMAL; e <= EFFORT_MAX; e++)
            if (model_supports_effort(m, (ai_effort_t)e)) s->steps[s->n++] = effort_name((ai_effort_t)e);
        if (s->n > 2) return;
        s->n = 0;
    }
    for (int e = EFFORT_OFF; e <= EFFORT_HIGH; e++) s->steps[s->n++] = effort_name((ai_effort_t)e);
    s->disabled = true;
}

/* `level` as one of the live steps: unset / inherit as the model's default, an effort clamped to its ladder */
static const char *thinking_step(const ai_model_t *m, const slider_t *s, const char *level) {
    const char *dl = m->thinking ? json_get_str(m->thinking, "defaultLevel") : NULL;
    const char *wanted = !level || str_eq(level, "inherit") ? (dl ? dl : "off") : level;
    for (size_t i = 0; i < s->n; i++)
        if (str_eq(s->steps[i], wanted)) return s->steps[i];
    if (str_eq(wanted, "auto") || str_eq(wanted, "off")) return "off";
    ai_effort_t e;
    if (effort_parse(wanted, &e) != 0) return "off";
    const char *c = effort_name(model_clamp_effort(m, e));
    for (size_t i = 0; i < s->n; i++)
        if (str_eq(s->steps[i], c)) return s->steps[i];
    return "off";
}

/* configuredThinkingLevel: the session's selection, `auto` included */
static const char *configured_level(const app_t *a) { return a->auto_thinking ? "auto" : effort_name(a->thinking); }

/* thinkingLevelFor (selector-controller.ts): the level the session runs at, auto's resolved effort once a turn
 * settled it (as the status line shows); the slider clamps it per model (omp 01985becf3, 081b4fbf8b) */
static const char *thinking_level_for(const picker_t *p, const ai_model_t *m) {
    UNUSED(m);
    app_t *a = p->u->app;
    if (a->auto_thinking && a->auto_resolved >= 0) return effort_name((ai_effort_t)a->auto_resolved);
    return configured_level(a);
}

static bool session_mode(const picker_t *p) { return !p->task_mode && !p->role_mode; }

static bool slider_of(const picker_t *p, slider_t *s) {
    if (!session_mode(p)) return false;
    const ui_model_item_t *it = ui_model_browser_selected(p->b);
    if (!it || !it->model) return false;
    thinking_steps(it->model, s);
    if (s->disabled) {
        s->value = "off";
        return true;
    }
    s->value = thinking_step(it->model, s, p->thinking ? p->thinking : thinking_level_for(p, it->model));
    return true;
}

/* #pendingThinking: the slider level picking `m` applies; NULL when untouched or the model has no effort */
static const char *pending_thinking(const picker_t *p, const ai_model_t *m) {
    if (!p->thinking || !session_mode(p)) return NULL;
    slider_t s;
    thinking_steps(m, &s);
    return s.disabled ? NULL : thinking_step(m, &s, p->thinking);
}

static void step_thinking(picker_t *p, int delta) {
    slider_t s;
    if (!slider_of(p, &s) || s.disabled) return;
    size_t i = 0;
    while (i < s.n && !str_eq(s.steps[i], s.value)) i++;
    long j = (long)i + delta;
    if (j < 0) j = 0;
    if (j >= (long)s.n) j = (long)s.n - 1;
    if (str_eq(s.steps[j], s.value)) return;
    free(p->thinking);
    p->thinking = xstrdup(s.steps[j]);
    tui_request_render(p->u->tui);
}

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
    } else {
        /* #scopedModelItems: a provider scope that lost its models falls back to all */
        bool any = false;
        for (size_t i = 0; p->scope && i < p->models.n; i++)
            if (str_eq(p->models.v[i]->provider, p->scope)) any = true;
        if (!any) {
            free(p->scope);
            p->scope = NULL;
        }
        for (size_t i = 0; i < p->models.n; i++)
            if (!p->scope || str_eq(p->models.v[i]->provider, p->scope)) ui_model_browser_add(p->b, p->models.v[i]);
    }
}

/* #syncItemsForQuery: swap model rows for quick-role rows when a leading `@` toggles the search mode */
static void sync_items(picker_t *p, bool refresh) {
    bool role_mode = query_of(p)[0] == '@' && !p->task_mode;
    bool changed = role_mode != p->role_mode;
    if (!changed && !refresh) return;
    p->role_mode = role_mode;
    if (role_mode) {
        free(p->scope);
        p->scope = NULL;
    }
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

/* the providers with models, alphabetical (#pickerScopes) */
static int cmp_provider(const void *a, const void *b) {
    int c = strcasecmp(*(char *const *)a, *(char *const *)b);
    return c ? c : strcmp(*(char *const *)a, *(char *const *)b);
}

static strvec_t picker_scopes(const picker_t *p) {
    strvec_t v = {0};
    for (size_t i = 0; i < p->models.n; i++) {
        bool seen = false;
        for (size_t k = 0; k < v.n && !seen; k++) seen = str_eq(v.v[k], p->models.v[i]->provider);
        if (!seen) strvec_push(&v, p->models.v[i]->provider);
    }
    if (v.n > 1) qsort(v.v, v.n, sizeof *v.v, cmp_provider);
    return v;
}

/* #setScope: a provider (picking the active one again goes back to all); the highlight stays on its model when
 * the scope lists it, else falls back to the current model */
static void set_scope(picker_t *p, const char *picked) {
    const char *id = picked && p->scope && str_eq(picked, p->scope) ? NULL : picked;
    if (p->role_mode || (!id && !p->scope) || (id && p->scope && str_eq(id, p->scope))) return;
    const ui_model_item_t *sel = ui_model_browser_selected(p->b);
    char *before = sel ? xstrdup(sel->selector) : NULL;
    free(p->scope);
    p->scope = id ? xstrdup(id) : NULL;
    sync_items(p, true);
    sel = ui_model_browser_selected(p->b);
    const char *target = p->task_mode ? p->task_selector : p->current;
    if (!(sel && before && str_eq(sel->selector, before)) && target) ui_model_browser_select(p->b, target);
    free(before);
    tui_request_render(p->u->tui);
}

/* #cycleScope: all models, then each provider, wrapping */
static void cycle_scope(picker_t *p, int delta) {
    strvec_t sc = picker_scopes(p);
    long n = (long)sc.n + 1, idx = 0;
    for (size_t i = 0; p->scope && i < sc.n; i++)
        if (str_eq(sc.v[i], p->scope)) idx = (long)i + 1;
    long next = (idx + delta + n) % n;
    set_scope(p, next == 0 ? NULL : sc.v[next - 1]);
    strvec_free(&sc);
}

static char *footer(const picker_t *p) {
    char *up = ui_editor_key("tui.select.up"), *down = ui_editor_key("tui.select.down");
    char *enter = ui_key_hint("enter"), *cancel = ui_editor_key("tui.select.cancel");
    char *tab = ui_key_hint("tab"), *left = ui_key_hint("left"), *right = ui_key_hint("right");
    char *r;
    if (p->task_mode)
        r = xasprintf("%s/%s models \xc2\xb7 %s use for Task subagents \xc2\xb7 %s provider \xc2\xb7 type to search \xc2\xb7 %s close", up,
                      down, enter, tab, cancel);
    else if (p->role_mode)
        r = xasprintf("%s/%s roles \xc2\xb7 %s apply role model \xc2\xb7 type to search \xc2\xb7 %s close", up, down, enter, cancel);
    else
        r = xasprintf("%s/%s models \xc2\xb7 %s use for this session \xc2\xb7 %s provider \xc2\xb7 %s/%s thinking \xc2\xb7 type to search "
                      "\xc2\xb7 @ quick roles \xc2\xb7 %s close",
                      up, down, enter, tab, left, right, cancel);
    free(tab);
    free(left);
    free(right);
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
    slider_t sl;
    if (slider_of(p, &sl)) {
        char *add;
        if (sl.disabled) add = tui_fgs(NULL, TUI_TC_DIM, " \xc2\xb7 no thinking");
        else {
            ai_effort_t e = EFFORT_OFF;
            if (effort_parse(sl.value, &e) != 0) e = EFFORT_OFF; /* auto: thinkingOff */
            char *m = tui_fgs(NULL, TUI_TC_MUTED, " \xc2\xb7 thinking ");
            char *v = tui_fgs(NULL, (tui_tc_t)(TUI_TC_THINKING_OFF + (int)e), step_label(sl.value));
            add = xasprintf("%s%s", m, v);
            free(m);
            free(v);
        }
        char *x = xasprintf("%s%s", status, add);
        free(status);
        free(add);
        status = x;
    }
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
    /* without a slider level, the session's level carries over (clamped to the model) */
    char *err = NULL;
    if (app_set_model_temporary(a, m, level, NULL, &err) != 0) {
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
    const char *pend = pending_thinking(p, m);
    char *level = pend ? xstrdup(pend) : NULL;
    picker_close(p); /* an over-context pick closes first so the compaction loader shows */
    (void)over;
    ui_switch_session_model(u, m, level);
    free(level);
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
    if (!p->role_mode) {
        if (tui_key_matches(k, "tab")) {
            cycle_scope(p, 1);
            return true;
        }
        if (tui_key_matches(k, "shift+tab")) {
            cycle_scope(p, -1);
            return true;
        }
    }
    /* ←/→ step the thinking slider; the search keeps ⌥←/→ and ⌃←/→ to move its caret by words */
    if (session_mode(p)) {
        if (tui_key_matches(k, "left")) {
            step_thinking(p, -1);
            return true;
        }
        if (tui_key_matches(k, "right")) {
            step_thinking(p, 1);
            return true;
        }
    }
    int r = ui_model_browser_key(p->b, k);
    if (strcmp(p->last_query, query_of(p))) {
        free(p->last_query);
        p->last_query = xstrdup(query_of(p));
        sync_items(p, false);
    }
    if (r == UI_MB_CANCEL) {
        /* closing on the current model's row with the slider moved keeps that level (onKeepThinking) */
        const ui_model_item_t *sel = ui_model_browser_selected(p->b);
        const char *kept = sel && sel->model && p->current && str_eq(sel->selector, p->current) ? pending_thinking(p, sel->model) : NULL;
        app_t *a = p->u->app;
        if (kept && !str_eq(kept, configured_level(a))) {
            ai_effort_t e;
            if (str_eq(kept, "auto")) app_set_thinking_auto(a);
            else if (effort_parse(kept, &e) == 0) app_set_thinking(a, e);
        }
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
    free(p->scope);
    free(p->thinking);
    free(p);
}

static const tui_component_vt PICKER_VT = {"model-picker", picker_render, picker_key, NULL, picker_destroy};

void ui_overlay_model_picker(ui_t *u) {
    if (g_open) {
        tui_set_focus(u->tui, &g_open->base);
        return;
    }
    catalog_refresh_if_stale(); /* model-picker.ts refreshIfStale: pick up a mid-session models.yml edit */
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
