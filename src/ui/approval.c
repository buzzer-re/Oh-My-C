/* approval.c - the tool-approval prompt in the composer's place (port of omp packages/tui overlays/approval-dialog.ts
 * and ExtensionUiController.showCollabAwareApproval) plus the bridge that lets a tool thread wait on it.
 *
 * The approval title and its details sit over a list of choices — optional grants (led by a toggle that keeps them
 * across sessions), Approve, an optional "approve for this session", and Deny — and a key-hint footer. The arrows
 * move between the choices and Enter takes the highlighted one; `app.approval.approve` (Ctrl+Enter / Ctrl+Q)
 * approves this call outright and Esc denies. Enter and the approve chord only arm APPROVE_ARM_MS after mount.
 * The composer stays mounted beneath the dialog and takes every other key: while it holds a draft the arrows and
 * Enter edit and send it, and a message sent that way (queued as steering) denies the call and is delivered first.
 * Page Up/Down scroll the details; with a full prompt the expand key (`app.tools.expand`) swaps it in, wrapped. */
#include "ui/ui.h"

#include "tools/approval.h"
#include "ui/signals.h"

#include <pthread.h>
#include <time.h>

#define MAX_DETAIL_ROWS 8     /* PREVIEW_LIMITS.COLLAPSED_ITEMS */
#define APPROVE_ARM_MS 500
#define DENY_AND_SEND_LABEL "Deny and send message"

enum { ANS_ONCE = -1, ANS_SESSION = -2, ANS_DENY = -3, ANS_REMEMBER = -4 }; /* >= 0: grant index */

typedef struct {
    char *label;
    int answer;
} choice_t;

typedef struct approval_req approval_req_t;

typedef struct {
    tui_component_t base;
    ui_t *u;
    approval_req_t *req;
    char *title;
    strvec_t summary, full;
    bool has_full, expanded;
    int scroll, body_rows;
    char *session_label;
    char *grant_label[2], *grant_always[2];
    size_t ngrants;
    VEC(choice_t) choices;
    int choice;
    bool remember_always;
    bool closed;
    int64_t armed_at;
    int hold; /* the run shows `blocked` on this approval (ui_signals_hold_blocked); 0: released */
} approval_view_t;

struct approval_req {
    const char *tool_call_id, *tool_name, *status;
    const approval_prompt_t *prompt;
    bool done;
    int answer; /* approval_answer_kind_t */
    int grant;
    bool always;
    bool cancel_posted;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_serial = PTHREAD_MUTEX_INITIALIZER; /* one dialog at a time */
static approval_req_t *g_req;       /* the request a dialog answers (under g_mu) */
static approval_view_t *g_view;     /* its dialog (UI thread) */
static bool g_closed;               /* the UI loop ended: nothing can answer any more */
static const json_t *g_denying[16]; /* steering messages that denied a call (identity tokens) */
static size_t g_ndenying;

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

/* sanitizeDisplayLines: split on newlines, keep what follows the last \r, sanitize, tabs to spaces */
static void display_lines(const char *text, strvec_t *out) {
    strvec_t lines = str_split(text ? text : "", "\n");
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        const char *cr = strrchr(l, '\r');
        if (cr) l = cr + 1;
        char *plain = tui_strip_ansi(l);
        sb_t b = SB_INIT;
        size_t n = strlen(plain);
        for (size_t k = 0; k < n;) {
            uint32_t cp;
            int m = utf8_decode(plain + k, n - k, &cp);
            bool ctl = (cp <= 0x08) || (cp >= 0x0B && cp <= 0x1F) || (cp >= 0x7F && cp <= 0x9F) || cp == 0xFFFD;
            if (!ctl) sb_appendn(&b, plain + k, (size_t)m);
            k += (size_t)m;
        }
        free(plain);
        char *t = tui_replace_tabs(sb_cstr(&b));
        sb_free(&b);
        strvec_push_own(out, t);
    }
    strvec_free(&lines);
}

/* promptDetails: every line after the title, trailing blanks dropped */
static void prompt_details(const strvec_t *lines, strvec_t *out) {
    size_t end = lines->n;
    while (end > 1) {
        char *t = str_trim(lines->v[end - 1]);
        bool blank = !*t;
        free(t);
        if (!blank) break;
        end--;
    }
    for (size_t i = 1; i < end; i++) strvec_push(out, lines->v[i]);
}

static void build_choices(approval_view_t *v) {
    for (size_t i = 0; i < v->choices.n; i++) free(v->choices.v[i].label);
    v->choices.n = 0;
    if (v->ngrants) {
        const char *glyph = tui_sym(v->remember_always ? "checkbox.checked" : "checkbox.unchecked");
        vec_push(&v->choices, ((choice_t){xasprintf("%s Remember across sessions", glyph), ANS_REMEMBER}));
    }
    for (size_t i = 0; i < v->ngrants; i++)
        vec_push(&v->choices, ((choice_t){xstrdup(v->remember_always ? v->grant_always[i] : v->grant_label[i]), (int)i}));
    vec_push(&v->choices, ((choice_t){xstrdup(v->ngrants ? "Approve Once" : "Approve"), ANS_ONCE}));
    if (v->session_label) vec_push(&v->choices, ((choice_t){xstrdup(v->session_label), ANS_SESSION}));
    vec_push(&v->choices, ((choice_t){xstrdup("Deny"), ANS_DENY}));
}

static bool drafting(const approval_view_t *v) { return !tui_editor_is_empty(v->u->editor) || json_len(v->u->att_images) > 0; }

/* the approve keys (appApproveKeys: the bindings, else Ctrl+Enter / Ctrl+Q) formatted, slash-joined */
static char *approve_keys_hint(void) {
    const strvec_t *keys = tui_keybindings_get(tui_keybindings_global(), "app.approval.approve");
    static const char *const DEF[] = {"ctrl+enter", "ctrl+q"};
    strvec_t parts = {0};
    size_t n = keys && keys->n ? keys->n : 2;
    for (size_t i = 0; i < n; i++) strvec_push_own(&parts, ui_key_hint(keys && keys->n ? keys->v[i] : DEF[i]));
    char *r = strvec_join(&parts, "/");
    strvec_free(&parts);
    return r;
}

static char *expand_key_hint(void) {
    const strvec_t *keys = tui_keybindings_get(tui_keybindings_global(), "app.tools.expand");
    return ui_key_hint(keys && keys->n ? keys->v[0] : "ctrl+o");
}

static char *footer_hint(approval_view_t *v, bool overflows, bool draft) {
    char *approve = approve_keys_hint(), *cancel = ui_editor_key("tui.select.cancel");
    char *expand = NULL;
    if (v->has_full) {
        char *ek = expand_key_hint();
        expand = xasprintf(" · %s %s", ek, v->expanded ? "less" : "details");
        free(ek);
    }
    char *keys = xasprintf("%s approve · %s deny%s", approve, cancel, expand ? expand : "");
    char *r;
    if (draft) {
        char *submit = ui_editor_key("tui.input.submit");
        r = xasprintf("%s deny and send · %s", submit, keys);
        free(submit);
    } else {
        char *up = ui_editor_key("tui.select.up"), *down = ui_editor_key("tui.select.down"),
             *confirm = ui_editor_key("tui.select.confirm");
        char *scroll = NULL;
        if (overflows) {
            char *pu = ui_editor_key("tui.select.pageUp"), *pd = ui_editor_key("tui.select.pageDown");
            scroll = xasprintf(" · %s/%s scroll", pu, pd);
            free(pu);
            free(pd);
        }
        r = xasprintf("%s/%s choose · %s select · %s%s · type to deny and steer", up, down, confirm, keys,
                      scroll ? scroll : "");
        free(up);
        free(down);
        free(confirm);
        free(scroll);
    }
    free(approve);
    free(cancel);
    free(expand);
    free(keys);
    return r;
}

static void detail_lines(approval_view_t *v, int width, strvec_t *out) {
    const strvec_t *d = v->expanded && v->has_full ? &v->full : &v->summary;
    for (size_t i = 0; i < d->n; i++) {
        if (!v->expanded) {
            char *c = S(TUI_TC_ACCENT, d->v[i]);
            strvec_push_own(out, tui_truncate_to_width(c, width, NULL));
            free(c);
        } else if (!*d->v[i]) {
            strvec_push(out, "");
        } else {
            char *c = S(TUI_TC_ACCENT, d->v[i]);
            tui_wrap_text_with_ansi(c, width, out);
            free(c);
        }
    }
}

static void approval_render(tui_component_t *c, int width, strvec_t *out) {
    approval_view_t *v = (approval_view_t *)c;
    int inner = MAX(1, width - 4);
    strvec_t lines = {0};
    detail_lines(v, inner, &lines);
    int rows = MAX(1, MIN((int)lines.n, MAX_DETAIL_ROWS));
    bool overflows = (int)lines.n > rows;
    if (overflows && inner > 1) { /* leave a column for the scrollbar */
        strvec_free(&lines);
        detail_lines(v, inner - 1, &lines);
    }
    v->body_rows = rows;
    strvec_t body = {0};
    if (overflows) ui_scroll_view(&lines, rows, &v->scroll, inner, &body);
    else {
        v->scroll = 0;
        for (size_t i = 0; i < lines.n; i++) strvec_push(&body, lines.v[i]);
        while ((int)body.n < rows) strvec_push(&body, "");
    }
    bool draft = drafting(v);
    int shown = draft ? (int)v->choices.n - 1 : v->choice;
    strvec_push_own(out, ui_box_top(width, v->title));
    for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
    for (size_t i = 0; i < v->choices.n; i++) {
        const choice_t *ch = &v->choices.v[i];
        const char *label = draft && ch->answer == ANS_DENY ? DENY_AND_SEND_LABEL : ch->label;
        char *row;
        if ((int)i != shown) {
            char *t = xasprintf("  %s", label);
            row = tui_truncate_to_width(t, inner, NULL);
            free(t);
        } else {
            char *t = xasprintf("%s %s", tui_sym("nav.cursor"), label);
            char *tr = tui_truncate_to_width(t, inner, NULL);
            char *col = S(ch->answer == ANS_DENY ? TUI_TC_ERROR : TUI_TC_ACCENT, tr);
            row = tui_bold(col);
            free(t);
            free(tr);
            free(col);
        }
        strvec_push_own(out, ui_box_row(row, width));
        free(row);
    }
    strvec_push_own(out, ui_box_divider(width));
    char *hint = footer_hint(v, overflows, draft), *dim = S(TUI_TC_DIM, hint);
    strvec_push_own(out, ui_box_row(dim, width));
    free(hint);
    free(dim);
    strvec_push_own(out, ui_box_bottom(width));
    /* the composer beneath the dialog takes the typing: its cursor stays visible */
    tui_component_t *ed = &v->u->editor->base;
    ed->focused = true;
    ed->vt->render(ed, width, out);
    strvec_free(&lines);
    strvec_free(&body);
}

static void request(approval_view_t *v) { tui_request_render(v->u->tui); }

/* answer the request (UI thread) and close the dialog; `v` is freed */
static void finish(approval_view_t *v, int answer) {
    if (v->closed) return;
    v->closed = true;
    pthread_mutex_lock(&g_mu);
    approval_req_t *r = v->req;
    if (r && g_req == r) {
        if (answer == ANS_ONCE) r->answer = APPROVE_ONCE;
        else if (answer == ANS_SESSION) r->answer = APPROVE_SESSION;
        else if (answer >= 0) {
            r->answer = APPROVE_GRANT;
            r->grant = answer;
            r->always = v->remember_always;
        } else r->answer = APPROVE_DENY;
        r->done = true;
        g_req = NULL;
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
    ui_t *u = v->u;
    if (g_view == v) g_view = NULL;
    ui_signals_approval_resolved();
    ui_close_selector(u); /* destroys v */
    tui_set_focus(u->tui, &u->editor->base);
    tui_request_render(u->tui);
}

/* the remember toggle flips the grants' lifetime in place; every other choice answers */
static void take(approval_view_t *v, int answer) {
    if (answer != ANS_REMEMBER) {
        finish(v, answer);
        return;
    }
    v->remember_always = !v->remember_always;
    build_choices(v);
    request(v);
}

static bool armed(const approval_view_t *v) { return mono_ms() >= v->armed_at; }

/* a key for the composer: a message it queues as steering denies the call and is delivered first */
static bool edit_draft(approval_view_t *v, const tui_key_t *k) {
    agent_t *ag = v->u->app->agent;
    size_t before = agent_steering_count(ag);
    tui_editor_handle_key(v->u->editor, k);
    size_t after = agent_steering_count(ag);
    request(v);
    if (after <= before) return false;
    agent_prioritize_steering(ag, before);
    pthread_mutex_lock(&g_mu);
    g_ndenying = 0;
    for (size_t i = 0; i < after - before && g_ndenying < ARRAY_LEN(g_denying); i++)
        g_denying[g_ndenying++] = agent_steering_at(ag, i);
    pthread_mutex_unlock(&g_mu);
    finish(v, ANS_DENY);
    return true;
}

static void move_choice(approval_view_t *v, int delta) {
    int n = (int)v->choices.n;
    v->choice = ((v->choice + delta) % n + n) % n;
    request(v);
}

static bool approval_key(tui_component_t *c, const tui_key_t *k) {
    approval_view_t *v = (approval_view_t *)c;
    if (v->closed) return true;
    if (tui_kb(k, "app.approval.approve")) {
        if (armed(v)) finish(v, ANS_ONCE);
        return true;
    }
    if (v->has_full && tui_kb(k, "app.tools.expand")) {
        v->expanded = !v->expanded;
        v->scroll = 0;
        request(v);
        return true;
    }
    bool draft = drafting(v);
    /* the composer's clear key (Ctrl+C, also a cancel key) clears a draft before it denies */
    if (draft && tui_kb(k, "app.clear")) {
        tui_editor_set_text(v->u->editor, "");
        ui_attach_clear(v->u);
        request(v);
        return true;
    }
    if (tui_kb(k, "tui.select.cancel")) {
        finish(v, ANS_DENY);
        return true;
    }
    if (draft) {
        edit_draft(v, k);
        return true;
    }
    int page = MAX(1, v->body_rows - 1);
    if (tui_kb(k, "tui.select.up")) move_choice(v, -1);
    else if (tui_kb(k, "tui.select.down")) move_choice(v, 1);
    else if (tui_kb(k, "tui.select.pageUp")) {
        v->scroll = MAX(0, v->scroll - page);
        request(v);
    } else if (tui_kb(k, "tui.select.pageDown")) {
        v->scroll += page;
        request(v);
    } else if (tui_kb(k, "tui.select.confirm")) {
        if (armed(v)) take(v, v->choices.v[v->choice].answer);
    } else if (!tui_kb(k, "tui.input.submit")) { /* Enter never reaches an empty composer */
        edit_draft(v, k);
    }
    return true;
}

static void approval_destroy(tui_component_t *c) {
    approval_view_t *v = (approval_view_t *)c;
    if (v->hold) ui_signals_release_blocked(v->hold);
    v->hold = 0;
    if (g_view == v) g_view = NULL;
    if (!v->closed) { /* closed from outside (another selector replaced it): the call is denied */
        pthread_mutex_lock(&g_mu);
        if (v->req && g_req == v->req) {
            v->req->answer = APPROVE_DENY;
            v->req->done = true;
            g_req = NULL;
            pthread_cond_broadcast(&g_cv);
        }
        pthread_mutex_unlock(&g_mu);
    }
    v->u->editor->base.focused = false;
    free(v->title);
    strvec_free(&v->summary);
    strvec_free(&v->full);
    free(v->session_label);
    for (size_t i = 0; i < v->ngrants; i++) {
        free(v->grant_label[i]);
        free(v->grant_always[i]);
    }
    for (size_t i = 0; i < v->choices.n; i++) free(v->choices.v[i].label);
    vec_free(&v->choices);
    free(v);
}

static const tui_component_vt APPROVAL_VT = {"approval-dialog", approval_render, approval_key, NULL, approval_destroy};

/* UI thread: mount the dialog for the current request */
static void show_request(void *ud) {
    ui_t *u = ud;
    pthread_mutex_lock(&g_mu);
    approval_req_t *r = g_req;
    if (!r || g_closed) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    approval_view_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &APPROVAL_VT);
    v->u = u;
    v->req = r;
    const approval_prompt_t *p = r->prompt;
    strvec_t lines = {0};
    display_lines(p->compact, &lines);
    v->title = xstrdup(lines.n ? lines.v[0] : "");
    prompt_details(&lines, &v->summary);
    strvec_free(&lines);
    if (p->full) {
        strvec_t fl = {0};
        display_lines(p->full, &fl);
        prompt_details(&fl, &v->full);
        strvec_free(&fl);
        v->has_full = true;
    }
    if (p->session) v->session_label = xstrdup(p->session);
    for (size_t i = 0; i < p->ngrants && i < 2; i++) {
        v->grant_label[i] = xstrdup(p->grants[i].label);
        v->grant_always[i] = xstrdup(p->grants[i].always_label);
        v->ngrants++;
    }
    char *id = r->tool_call_id ? xstrdup(r->tool_call_id) : NULL, *tool = xstrdup(r->tool_name ? r->tool_name : ""),
         *status = r->status ? xstrdup(r->status) : NULL, *compact = xstrdup(p->compact ? p->compact : "");
    pthread_mutex_unlock(&g_mu);
    build_choices(v);
    for (size_t i = 0; i < v->choices.n; i++)
        if (v->choices.v[i].answer == ANS_ONCE) v->choice = (int)i;
    v->armed_at = mono_ms() + APPROVE_ARM_MS;
    g_view = v;
    ui_show_selector(u, &v->base, &v->base);
    v->hold = ui_signals_hold_blocked("permission", compact);
    ui_signals_approval_requested(id, tool, status);
    free(compact);
    free(id);
    free(tool);
    free(status);
}

/* UI thread: the run was aborted while the dialog waited */
static void cancel_request(void *ud) {
    approval_req_t *r = ud;
    pthread_mutex_lock(&g_mu);
    bool current = g_req == r;
    pthread_mutex_unlock(&g_mu);
    if (!current) return;
    if (g_view && g_view->req == r) finish(g_view, ANS_DENY);
    else {
        pthread_mutex_lock(&g_mu);
        if (g_req == r) {
            r->answer = APPROVE_DENY;
            r->done = true;
            g_req = NULL;
            pthread_cond_broadcast(&g_cv);
        }
        pthread_mutex_unlock(&g_mu);
    }
}

static bool denied_by_steer(agent_t *ag) {
    for (size_t i = 0; i < g_ndenying; i++)
        if (g_denying[i] && agent_steering_has(ag, g_denying[i])) return true;
    g_ndenying = 0;
    return false;
}

/* tool thread: requestApproval through the dialog */
static int ui_approve(void *ud, const char *tool_call_id, const char *tool_name, const char *status,
                      const approval_prompt_t *prompt, int *grant, bool *always, volatile sig_atomic_t *cancel) {
    ui_t *u = ud;
    pthread_mutex_lock(&g_serial);
    pthread_mutex_lock(&g_mu);
    /* a message that denied an earlier call in this batch also denies the approvals queued behind it */
    if (g_closed || denied_by_steer(u->app->agent)) {
        pthread_mutex_unlock(&g_mu);
        pthread_mutex_unlock(&g_serial);
        return APPROVE_DENY;
    }
    approval_req_t r = {tool_call_id, tool_name, status, prompt, false, APPROVE_DENY, 0, false, false};
    g_req = &r;
    pthread_mutex_unlock(&g_mu);
    tui_post(u->tui, show_request, u);
    pthread_mutex_lock(&g_mu);
    while (!r.done) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
        if (!r.done && cancel && *cancel && !r.cancel_posted) {
            r.cancel_posted = true;
            pthread_mutex_unlock(&g_mu);
            tui_post(u->tui, cancel_request, &r);
            pthread_mutex_lock(&g_mu);
        }
    }
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_unlock(&g_serial);
    *grant = r.grant;
    *always = r.always;
    return r.answer;
}

void ui_approval_install(ui_t *u) {
    pthread_mutex_lock(&g_mu);
    g_closed = false;
    pthread_mutex_unlock(&g_mu);
    u->app->env->ui.approve = ui_approve;
}

void ui_approval_shutdown(ui_t *u) {
    pthread_mutex_lock(&g_mu);
    g_closed = true;
    if (g_req) {
        g_req->answer = APPROVE_DENY;
        g_req->done = true;
        g_req = NULL;
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
    if (u && u->app && u->app->env) u->app->env->ui.approve = NULL;
}
