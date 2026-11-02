/* btw.c - `/btw` side questions in the TUI: the controller (port of omp
 * coding-agent/src/modes/controllers/btw-controller.ts), the inline answer panel above the editor
 * (tui/src/overlays/btw-panel.ts on chrome/streaming-panel.ts + chrome/overlay-box.ts) and the fullscreen
 * BTW history (tui/src/overlays/btw-history-panel.ts) with its follow-up composer.
 *
 * The side turn runs on a worker thread (coding/btw.c); text deltas are coalesced and applied on the UI
 * thread, which owns every record, the panels and the history store. Checkpoints are synchronous writes on
 * the UI thread (small files), so omp's write queue reduces to "the last failed write is retried before any
 * session move". */
#include "ui/ui.h"

#include "coding/btw.h"
#include "core/fs.h"
#include "tools/artifacts.h"

#include <ctype.h>
#include <math.h>
#include <time.h>

typedef enum { PS_RUNNING, PS_COMPLETE, PS_BRANCHING, PS_ABORTED, PS_ERROR } pstate_t;

typedef struct btw_ctl btw_ctl_t;
typedef struct btw_req btw_req_t;
typedef struct hist hist_t;

/* BtwPanelComponent: rounded box titled `/btw <question>` (or the side command's title) */
typedef struct {
    pstate_t state;
    char *base_title;
    char *answer;  /* raw */
    char *visible; /* replaceTabs(answer).trim() */
    char *error;
    bool copied, closed;
    /* markdown cache */
    char *md_text;
    int md_width;
    strvec_t md_lines;
} panel_t;

struct btw_req {
    btw_ctl_t *c;
    panel_t panel;
    char *question;
    json_t *images;
    char *leaf_id, *session_id;
    btw_store_t *store;
    json_t *record;
    json_t *history; /* earlier turns of a follow-up's topic, NULL for a new topic */
    char *conversation_key;
    json_t *streaming; /* in-flight assistant reply when asked */
    bool persisted;
    bool failed_write; /* a checkpoint after the first failed; retried by flush */
    /* worker */
    volatile sig_atomic_t cancel;
    bool worker_live; /* thread started and its done callback has not run yet */
    bool dropped;     /* no longer the controller's: freed by the done callback */
    pthread_t th;
    pthread_mutex_t mu;
    sb_t pending; /* deltas not yet applied on the UI thread */
    bool posted;
    int rc;
    char *reply, *err;
    json_t *assistant;
};

struct btw_ctl {
    ui_t *u;
    tui_component_t *box; /* btwContainer: shows the active request's panel while visible */
    btw_req_t *active;
    bool visible, starting, branch_in_flight, copy_in_flight;
    int transition;
    int generation;
    btw_store_t *store;
    char *store_sid, *store_dir;
    hist_t *hist;
    /* the last completed answer (copy / branch) */
    char *last_question, *last_reply, *last_copy, *last_leaf, *last_sid;
    json_t *last_images, *last_assistant;
};

static void hist_close(btw_ctl_t *c);
static void hist_refresh(btw_ctl_t *c);
static hist_t *hist_show(btw_ctl_t *c);
static bool hist_open_follow_up(hist_t *h, const char *record_id);
static void hist_mark_copied(hist_t *h, const char *record_id, const char *text);
static bool can_branch(btw_ctl_t *c);
static bool can_follow_up(btw_ctl_t *c);
static bool start(btw_ctl_t *c, const char *question, const char *record_id, const json_t *images, const char *title);

/* ---------- small helpers ---------- */

static char *sfg(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static bool blank(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

static char *collapse_ws(const char *s) {
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = s ? s : ""; *p; p++) {
        if (isspace((unsigned char)*p)) {
            sp = b.len > 0;
            continue;
        }
        if (sp) sb_putc(&b, ' ');
        sp = false;
        sb_putc(&b, *p);
    }
    return sb_detach(&b);
}

/* sanitizeErrorLine: plain one-line error, home paths as ~, at most 110 cells */
static char *sanitize_error_line(const char *msg) {
    char *t = tui_replace_tabs(msg ? msg : "");
    const char *home = home_dir();
    if (home && *home && strcmp(home, "/")) {
        sb_t b = SB_INIT;
        size_t hl = strlen(home);
        for (const char *p = t; *p;) {
            if (!strncmp(p, home, hl) && (p[hl] == '/' || !p[hl] || isspace((unsigned char)p[hl]))) {
                sb_putc(&b, '~');
                p += hl;
            } else sb_putc(&b, *p++);
        }
        free(t);
        t = sb_detach(&b);
    }
    char *c = collapse_ws(t);
    free(t);
    if (!*c) {
        free(c);
        c = xstrdup("Unknown error");
    }
    char *r = tui_truncate_to_width(c, 110, "\xe2\x80\xa6");
    free(c);
    return r;
}

/* sanitizeDisplayLine: one line, no control characters */
static char *display_line(const char *s) {
    char *c = collapse_ws(s);
    for (char *p = c; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f) *p = ' ';
    return c;
}

/* interruptKey(): the first key bound to app.interrupt, Escape by default */
static char *interrupt_key(void) {
    const strvec_t *keys = tui_keybindings_get(tui_keybindings_global(), "app.interrupt");
    return ui_key_hint(keys && keys->n ? keys->v[0] : "escape");
}

/* keyHint / rawKeyHint: dim keys, muted " description" */
static char *key_hint(const char *keys, const char *desc) {
    char *k = sfg(TUI_TC_DIM, keys);
    char *d0 = xasprintf(" %s", desc), *d = sfg(TUI_TC_MUTED, d0);
    char *r = xasprintf("%s%s", k, d);
    free(k), free(d0), free(d);
    return r;
}

static char *editor_key_hint(const char *action, const char *desc) {
    char *k = ui_editor_key(action);
    char *r = key_hint(k, desc);
    free(k);
    return r;
}

static void fmt_time(int64_t ms, bool date, char *buf, size_t n) {
    static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    if (date) snprintf(buf, n, "%s %d, %d, %02d:%02d", MON[tm.tm_mon % 12], tm.tm_mday, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    else snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static void md_lines(const char *text, int width, strvec_t *out) {
    tui_md_opts_t o;
    tui_md_opts_default(&o);
    tui_md_render(text, MAX(1, width), &o, out);
}

static const char *session_sid(ui_t *u) { return u->app->session ? session_id(u->app->session) : ""; }

/* ---------- inline panel (btw-panel.ts) ---------- */

static void panel_init(panel_t *p, const char *question, const char *title) {
    memset(p, 0, sizeof *p);
    char *base = title && *title ? xstrdup(title) : xasprintf("/btw %s", question);
    char *t = tui_replace_tabs(base);
    p->base_title = collapse_ws(t);
    free(t);
    free(base);
    p->answer = xstrdup("");
    p->visible = xstrdup("");
}

static void panel_free(panel_t *p) {
    free(p->base_title);
    free(p->answer);
    free(p->visible);
    free(p->error);
    free(p->md_text);
    strvec_free(&p->md_lines);
}

static void panel_set_visible(panel_t *p) {
    char *t = tui_replace_tabs(p->answer);
    free(p->visible);
    p->visible = str_trim(t);
    free(t);
}

static void panel_append(panel_t *p, const char *delta) {
    if (!delta || !*delta || p->closed) return;
    char *n = xasprintf("%s%s", p->answer, delta);
    free(p->answer);
    p->answer = n;
    panel_set_visible(p);
    p->copied = false;
}

static void panel_set_answer(panel_t *p, const char *text) {
    if (p->closed) return;
    free(p->answer);
    p->answer = xstrdup(text ? text : "");
    panel_set_visible(p);
    p->copied = false;
}

static void panel_mark(panel_t *p, pstate_t st, const char *error) {
    if (p->closed) return;
    p->state = st;
    free(p->error);
    p->error = error ? xstrdup(error) : NULL;
    p->copied = false;
}

static bool panel_copyable(const panel_t *p) { return p->state == PS_COMPLETE && *p->visible; }

static char *panel_footer(btw_ctl_t *c, panel_t *p) {
    char *esc = interrupt_key();
    char *r = NULL;
    switch (p->state) {
    case PS_RUNNING: {
        char *t = xasprintf("%s to cancel", esc);
        r = sfg(TUI_TC_MUTED, t);
        free(t);
        break;
    }
    case PS_COMPLETE: {
        strvec_t acts = {0};
        if (panel_copyable(p)) strvec_push(&acts, p->copied ? "c to copy again" : "c to copy");
        if (can_follow_up(c)) strvec_push(&acts, "f to follow up");
        if (can_branch(c)) strvec_push(&acts, "b to branch");
        strvec_push_own(&acts, xasprintf("%s to close", esc));
        char *joined = strvec_join(&acts, " \xc2\xb7 ");
        if (p->copied) {
            char *ok = sfg(TUI_TC_SUCCESS, "\xe2\x9c\x93 Copied to clipboard");
            char *rest0 = xasprintf(" \xc2\xb7 %s", joined), *rest = sfg(TUI_TC_MUTED, rest0);
            r = xasprintf("%s%s", ok, rest);
            free(ok), free(rest0), free(rest);
        } else r = sfg(TUI_TC_MUTED, joined);
        free(joined);
        strvec_free(&acts);
        break;
    }
    case PS_BRANCHING: {
        char *t = xasprintf("%s Branching to chat\xe2\x80\xa6", tui_sym("status.pending"));
        r = sfg(TUI_TC_MUTED, t);
        free(t);
        break;
    }
    case PS_ABORTED: {
        char *t = xasprintf("%s Cancelled \xc2\xb7 %s to close", tui_sym("status.warning"), esc);
        r = sfg(TUI_TC_WARNING, t);
        free(t);
        break;
    }
    case PS_ERROR: {
        char *t = xasprintf("%s Error \xc2\xb7 %s to close", tui_sym("status.error"), esc);
        r = sfg(TUI_TC_ERROR, t);
        free(t);
        break;
    }
    }
    free(esc);
    return r;
}

static void panel_render(btw_ctl_t *c, panel_t *p, int width, strvec_t *out) {
    int inner = MAX(1, width - 4);
    char *title = p->copied ? xasprintf("%s \xe2\x9c\x93 Copied", p->base_title) : xstrdup(p->base_title);
    strvec_t body = {0};
    strvec_push(&body, ""); /* StreamingPanelContent: Spacer(1), each section + Spacer(1), the footer */
    if (p->state == PS_ERROR) {
        char *e = sanitize_error_line(p->error ? p->error : "Unknown error"), *s = sfg(TUI_TC_ERROR, e);
        ui_push_text(&body, s, inner);
        free(e), free(s);
    } else if (*p->visible) {
        if (!p->md_text || strcmp(p->md_text, p->visible) || p->md_width != inner) {
            free(p->md_text);
            p->md_text = xstrdup(p->visible);
            p->md_width = inner;
            strvec_free(&p->md_lines);
            md_lines(p->visible, inner, &p->md_lines);
        }
        for (size_t i = 0; i < p->md_lines.n; i++) strvec_push(&body, p->md_lines.v[i]);
    } else {
        char *w = p->state == PS_RUNNING ? xasprintf("%s Waiting for response\xe2\x80\xa6", tui_sym("status.pending"))
                                         : xstrdup("No text returned.");
        char *s = sfg(TUI_TC_DIM, w);
        ui_push_text(&body, s, inner);
        free(w), free(s);
    }
    strvec_push(&body, "");
    char *f = panel_footer(c, p);
    ui_push_text(&body, f, inner);
    free(f);
    strvec_t lines = {0};
    strvec_push_own(&lines, ui_box_top(width, title));
    for (size_t i = 0; i < body.n; i++) strvec_push_own(&lines, ui_box_row(body.v[i], width));
    strvec_push_own(&lines, ui_box_bottom(width));
    for (size_t i = 0; i < lines.n; i++) {
        if (width < 4) strvec_push_own(out, tui_truncate_to_width(lines.v[i], width, ""));
        else strvec_push(out, lines.v[i]);
    }
    strvec_free(&lines);
    strvec_free(&body);
    free(title);
}

/* ---------- requests ---------- */

static void req_free(btw_req_t *r) {
    panel_free(&r->panel);
    free(r->question);
    json_free(r->images);
    free(r->leaf_id);
    free(r->session_id);
    json_free(r->record);
    json_free(r->history);
    free(r->conversation_key);
    json_free(r->streaming);
    pthread_mutex_destroy(&r->mu);
    sb_free(&r->pending);
    free(r->reply);
    free(r->err);
    json_free(r->assistant);
    free(r);
}

/* the controller lets go of a request: a live worker frees it from its done callback */
static void req_drop(btw_req_t *r) {
    if (!r) return;
    r->panel.closed = true;
    if (r->worker_live) {
        r->dropped = true;
        r->cancel = 1;
    } else req_free(r);
}

/* #isActiveRequest: current, and its own session is still loaded */
static bool is_active(btw_ctl_t *c, btw_req_t *r) { return c->active == r && str_eq(r->session_id, session_sid(c->u)); }

static const char *latest_status(btw_req_t *r) { return btw_latest_status(r->record); }

static void clear_completed(btw_ctl_t *c) {
    free(c->last_question), free(c->last_reply), free(c->last_copy), free(c->last_leaf), free(c->last_sid);
    json_free(c->last_images);
    json_free(c->last_assistant);
    c->last_question = c->last_reply = c->last_copy = c->last_leaf = c->last_sid = NULL;
    c->last_images = c->last_assistant = NULL;
}

static void hide_inline(btw_ctl_t *c) {
    c->visible = false;
    tui_request_render(c->u->tui);
}

/* #persist: checkpoint the request's record; failures are reported and remembered for flush */
static bool persist(btw_ctl_t *c, btw_req_t *r, bool retry) {
    char *err = NULL;
    int rc = retry ? btw_store_retry(r->store, r->record, &err) : btw_store_upsert(r->store, r->record, &err);
    if (rc == 0) {
        r->persisted = true;
        r->failed_write = false;
        return true;
    }
    if (r->persisted) r->failed_write = true;
    log_msg(LOG_ERROR, "BTW history save failed: %s", err ? err : "");
    if (str_eq(r->session_id, session_sid(c->u))) {
        char *m = xasprintf("Could not save /btw history: %s", err ? err : "write failed");
        char *s = sanitize_error_line(m);
        add_notice(c->u, "error", s);
        free(s), free(m);
    }
    free(err);
    return false;
}

/* flush(): retry the active request's failed checkpoint; *err (owned) when it still cannot be saved */
static bool flush(btw_ctl_t *c, char **err) {
    btw_req_t *r = c->active;
    if (!r || !r->failed_write) return true;
    char *e = NULL;
    if (btw_store_retry(r->store, r->record, &e) == 0) {
        r->failed_write = false;
        return true;
    }
    char *m = xasprintf("BTW history could not be saved: %s. The session operation was stopped; retry after fixing "
                        "storage. Unsaved answers remain in /btw.",
                        e ? e : "write failed");
    free(e);
    *err = tui_truncate_to_width(m, 280, "\xe2\x80\xa6");
    free(m);
    return false;
}

/* coalesced deltas, applied on the UI thread */
static void apply_deltas(void *ud) {
    btw_req_t *r = ud;
    pthread_mutex_lock(&r->mu);
    char *d = sb_detach(&r->pending);
    r->posted = false;
    pthread_mutex_unlock(&r->mu);
    btw_ctl_t *c = r->c;
    if (!r->dropped && d && *d && str_eq(latest_status(r), "running")) {
        json_t *t = btw_latest_turn(r->record);
        char *ans = xasprintf("%s%s", json_get_str(t, "answer") ? json_get_str(t, "answer") : "", d);
        json_set_str(t, "answer", ans);
        json_set_int(t, "updatedAt", now_ms());
        free(ans);
        if (is_active(c, r)) {
            if (c->visible) panel_append(&r->panel, d);
            hist_refresh(c);
        }
        tui_request_render(c->u->tui);
    }
    free(d);
}

static void worker_delta(void *ud, const char *delta) {
    btw_req_t *r = ud;
    pthread_mutex_lock(&r->mu);
    sb_append(&r->pending, delta);
    bool post = !r->posted;
    r->posted = true;
    pthread_mutex_unlock(&r->mu);
    if (post) tui_post(r->c->u->tui, apply_deltas, r);
}

static void run_done(void *ud);

static void *worker_main(void *p) {
    btw_req_t *r = p;
    r->rc = btw_run_turn(r->c->u->app, r->question, r->images, r->history, r->conversation_key, r->streaming,
                         worker_delta, r, &r->cancel, &r->reply, &r->assistant, &r->err);
    tui_post(r->c->u->tui, run_done, r);
    return NULL;
}

/* AssistantMessage with its text replaced by the shown reply (assistantMessageWithReplyText) */
static json_t *assistant_with_reply(const json_t *msg, const char *reply) {
    json_t *m = json_clone(msg);
    json_t *content = json_arr(), *blk;
    bool replaced = false;
    json_arr_foreach(json_get(msg, "content"), i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "thinking")) {
            json_t *b = json_obj();
            json_set_str(b, "type", "thinking");
            json_set_str(b, "thinking", json_get_str(blk, "thinking") ? json_get_str(blk, "thinking") : "");
            if (json_get(blk, "summary")) json_set(b, "summary", json_clone(json_get(blk, "summary")));
            if (json_get_bool(blk, "narration", false)) json_set_bool(b, "narration", true);
            json_push(content, b);
            continue;
        }
        if (str_eq(t, "redactedThinking")) continue;
        if (!str_eq(t, "text")) {
            json_push(content, json_clone(blk));
            continue;
        }
        if (replaced) continue;
        json_push(content, text_block(reply));
        replaced = true;
    }
    if (!replaced) json_push(content, text_block(reply));
    json_set(m, "content", content);
    json_del(m, "providerPayload");
    return m;
}

/* #runRequest's settlement, on the UI thread */
static void run_done(void *ud) {
    btw_req_t *r = ud;
    btw_ctl_t *c = r->c;
    apply_deltas(r);
    pthread_join(r->th, NULL);
    r->worker_live = false;
    if (r->dropped) {
        /* a dropped turn still checkpoints its terminal state when it was running */
        if (str_eq(latest_status(r), "running")) {
            btw_set_status(r->record, r->rc == 0 ? "complete" : "cancelled", r->rc == 0 ? r->reply : NULL, NULL);
            char *e = NULL;
            if (btw_store_upsert(r->store, r->record, &e) != 0) log_msg(LOG_ERROR, "BTW history save failed: %s", e ? e : "");
            free(e);
        }
        if (c->store != r->store) btw_store_free(r->store);
        req_free(r);
        return;
    }
    if (str_eq(latest_status(r), "running")) {
        if (r->rc == 0) {
            btw_set_status(r->record, "complete", r->reply, NULL);
            if (is_active(c, r)) {
                panel_set_answer(&r->panel, r->reply);
                panel_mark(&r->panel, PS_COMPLETE, NULL);
                if (panel_copyable(&r->panel)) {
                    clear_completed(c);
                    c->last_question = xstrdup(r->question);
                    c->last_images = r->images ? json_clone(r->images) : NULL;
                    c->last_reply = xstrdup(r->reply);
                    c->last_copy = xstrdup(r->panel.visible);
                    c->last_assistant = assistant_with_reply(r->assistant, r->reply);
                    c->last_leaf = r->leaf_id ? xstrdup(r->leaf_id) : NULL;
                    c->last_sid = xstrdup(r->session_id);
                } else clear_completed(c);
            }
        } else {
            bool cancelled = r->cancel != 0;
            btw_set_status(r->record, cancelled ? "cancelled" : "error", NULL, cancelled ? NULL : r->err);
            if (is_active(c, r)) panel_mark(&r->panel, cancelled ? PS_ABORTED : PS_ERROR, r->err);
        }
    }
    persist(c, r, false);
    if (is_active(c, r)) hist_refresh(c);
    tui_request_render(c->u->tui);
}

static void run_request(btw_ctl_t *c, btw_req_t *r) {
    pthread_mutex_init(&r->mu, NULL);
    r->worker_live = true;
    if (pthread_create(&r->th, NULL, worker_main, r) != 0) {
        r->worker_live = false;
        r->rc = -1;
        r->err = xstrdup("failed to start the /btw thread");
        btw_set_status(r->record, "error", NULL, r->err);
        panel_mark(&r->panel, PS_ERROR, r->err);
        persist(c, r, false);
    }
}

/* ---------- controller ---------- */

static btw_ctl_t *ctl(ui_t *u) {
    if (!u->btw) {
        btw_ctl_t *c = xcalloc(1, sizeof *c);
        c->u = u;
        u->btw = c;
    }
    return u->btw;
}

/* hasActiveRequest: the inline panel owns Escape */
static bool has_active(btw_ctl_t *c) { return c->visible; }

static const char *branch_unavailable(btw_ctl_t *c) {
    if (c->branch_in_flight) return "a branch is already in progress";
    if (c->transition > 0) return "a session operation is in progress";
    if (!c->visible || !c->active || !panel_copyable(&c->active->panel)) return "the answer is not ready";
    /* inline branch promotion carries one pair; earlier side turns stay in BTW history */
    if (json_len(c->active->history)) return "multi-turn side conversations remain in BTW history";
    if (!c->last_question || !c->last_reply || !c->last_assistant) return "the answer is unavailable";
    if (!c->last_leaf) return "the session has no branch point";
    const char *leaf = c->u->app->session ? c->u->app->session->leaf_id : NULL;
    if (!str_eq(c->last_sid, session_sid(c->u)) || !str_eq(c->last_leaf, leaf)) return "the session changed since /btw started";
    if (c->u->worker_running) return "a turn is still running";
    return NULL;
}

static bool can_branch(btw_ctl_t *c) { return branch_unavailable(c) == NULL; }

/* handlesBranchKey: plain `b` is reserved for a completed or pending branch action */
static bool handles_branch_key(btw_ctl_t *c) {
    if (c->branch_in_flight) return true;
    if (!c->visible || !c->active || !panel_copyable(&c->active->panel)) return false;
    return c->last_question && c->last_reply && c->last_assistant && c->last_sid;
}

static bool can_copy(btw_ctl_t *c) {
    return c->visible && !c->copy_in_flight && c->active && panel_copyable(&c->active->panel) && c->last_copy;
}

static bool can_follow_up(btw_ctl_t *c) {
    btw_req_t *r = c->active;
    return c->visible && !c->starting && !c->branch_in_flight && c->transition == 0 && r && is_active(c, r) &&
           str_eq(latest_status(r), "complete");
}

static void copy_answer(btw_ctl_t *c, const char *answer, const char *history_record_id) {
    if (c->copy_in_flight || !answer) return;
    char *t = tui_replace_tabs(answer), *tr = str_trim(t);
    free(t);
    if (!*tr) {
        free(tr);
        return;
    }
    c->copy_in_flight = true;
    btw_req_t *inline_req = history_record_id ? NULL : c->active;
    tui_term_copy_osc52(tui_term(c->u->tui), tr, strlen(tr));
    add_notice(c->u, "info", "Copied /btw answer to clipboard");
    if (history_record_id) {
        if (c->hist) hist_mark_copied(c->hist, history_record_id, answer);
    } else if (inline_req && c->visible && c->active == inline_req && panel_copyable(&inline_req->panel)) {
        inline_req->panel.copied = true;
    }
    c->copy_in_flight = false;
    free(tr);
    tui_request_render(c->u->tui);
}

static bool handle_cancel(btw_ctl_t *c) {
    btw_req_t *r = c->active;
    if (!r || !str_eq(latest_status(r), "running")) return false;
    btw_set_status(r->record, "cancelled", NULL, NULL);
    r->cancel = 1;
    panel_mark(&r->panel, PS_ABORTED, NULL);
    persist(c, r, false);
    hist_refresh(c);
    tui_request_render(c->u->tui);
    return true;
}

static bool handle_escape(btw_ctl_t *c) {
    if (c->branch_in_flight) {
        add_notice(c->u, "info", "/btw branch is in progress");
        return true;
    }
    if (!c->visible) return false;
    if (handle_cancel(c)) return true;
    hide_inline(c);
    return true;
}

static void dispose(btw_ctl_t *c) {
    c->generation++;
    c->transition++;
    handle_cancel(c);
    char *err = NULL;
    flush(c, &err); /* a session move already happened: report, do not block */
    if (err) {
        add_notice(c->u, "error", err);
        free(err);
    }
    hist_close(c);
    hide_inline(c);
    btw_req_t *r = c->active;
    c->active = NULL;
    if (r) {
        if (r->worker_live) {
            /* its store goes with it: the turn frees the store it holds once it lands */
            if (r->store == c->store) c->store = NULL;
        }
        req_drop(r);
    }
    clear_completed(c);
    btw_store_free(c->store);
    c->store = NULL;
    free(c->store_sid);
    free(c->store_dir);
    c->store_sid = c->store_dir = NULL;
    c->transition--;
}

/* #loadHistory: the current session's store, reopened when the session changed */
static btw_store_t *load_history(btw_ctl_t *c, char **err) {
    app_t *a = c->u->app;
    const char *sid = session_sid(c->u);
    char *dir = a->session && a->session->file && a->session->persist ? artifacts_dir_for_session(a->session->file) : NULL;
    if (!c->store || !str_eq(c->store_sid, sid) || !str_eq(c->store_dir ? c->store_dir : "", dir ? dir : "")) {
        if (c->store || c->active) dispose(c);
        c->store = btw_store_open(dir, NULL, err);
        if (!c->store) {
            free(dir);
            return NULL;
        }
        free(c->store_sid);
        free(c->store_dir);
        c->store_sid = xstrdup(sid);
        c->store_dir = dir;
        return c->store;
    }
    free(dir);
    return c->store;
}

static json_t *history_records(btw_ctl_t *c) {
    json_t *recs = c->store ? btw_store_records(c->store) : json_arr();
    btw_req_t *r = c->active;
    if (!r || r->store != c->store) return recs;
    for (size_t i = 0; i < json_len(recs); i++) {
        if (str_eq(json_get_str(json_at(recs, i), "id"), json_get_str(r->record, "id"))) {
            json_free(recs->u.arr.items[i]);
            recs->u.arr.items[i] = json_clone(r->record);
        }
    }
    return recs;
}

/* #start: ask a side question, a follow-up in topic `record_id`, or (empty question) open the history */
static bool start(btw_ctl_t *c, const char *question, const char *record_id, const json_t *images, const char *title) {
    ui_t *u = c->u;
    app_t *a = u->app;
    char *q = str_trim(question ? question : "");
    bool ok = false;
    if (c->starting || c->branch_in_flight || c->transition > 0) {
        add_notice(u, "info", "A /btw action is in progress. Please wait.");
        goto out;
    }
    btw_req_t *act = c->active;
    if (act && str_eq(latest_status(act), "running") && *q && str_eq(act->session_id, session_sid(u))) {
        add_notice(u, "info", "A /btw question is still running. Open /btw to view it or cancel it first.");
        goto out;
    }
    c->starting = true;
    char *err = NULL;
    btw_store_t *store = load_history(c, &err);
    if (!store) {
        char *m = xasprintf("Cannot open /btw history: %s", err ? err : "unavailable"), *s = sanitize_error_line(m);
        add_notice(u, "error", s);
        free(s), free(m), free(err);
        goto starting;
    }
    if (!*q) {
        hist_show(c);
        ok = true;
        goto starting;
    }
    /* a just-cancelled/completed turn may still hold an unsaved checkpoint */
    if (!flush(c, &err)) {
        char *m = xasprintf("Cannot open /btw history: %s", err), *s = sanitize_error_line(m);
        add_notice(u, "error", s);
        free(s), free(m), free(err);
        goto starting;
    }
    json_t *previous = record_id ? btw_store_find(store, record_id) : NULL;
    if (previous && c->active && str_eq(json_get_str(c->active->record, "id"), record_id)) {
        json_free(previous);
        previous = json_clone(c->active->record);
    }
    if (record_id && (!previous || str_eq(btw_latest_status(previous), "running"))) {
        json_free(previous);
        add_notice(u, "info", "This side conversation is unavailable or still running.");
        goto starting;
    }
    if (!a->model) {
        json_free(previous);
        add_notice(u, "error", "No active model available for /btw.");
        goto starting;
    }
    if (a->session) session_ensure_on_disk(a->session);
    if (!previous) hist_close(c);
    req_drop(c->active);
    c->active = NULL;
    clear_completed(c);
    btw_req_t *r = xcalloc(1, sizeof *r);
    r->c = c;
    panel_init(&r->panel, q, title);
    r->question = xstrdup(q);
    r->images = json_len(images) ? json_clone(images) : NULL;
    r->leaf_id = a->session && a->session->leaf_id ? xstrdup(a->session->leaf_id) : NULL;
    r->session_id = xstrdup(session_sid(u));
    r->store = store;
    btw_begin_t b;
    btw_begin_turn(q, r->leaf_id, previous, images, title, &b);
    json_free(previous);
    r->record = b.record;
    r->history = b.history;
    r->conversation_key = b.conversation_key;
    /* the in-flight streaming reply is part of the context the side question sees */
    if (u->cur_assistant && u->cur_assistant->msg) r->streaming = json_clone(u->cur_assistant->msg);
    c->active = r;
    c->visible = !previous || !c->hist;
    tui_request_render(u->tui);
    if (!persist(c, r, false)) {
        if (c->active == r) {
            c->active = NULL;
            hide_inline(c);
            btw_store_free(c->store);
            c->store = NULL;
            free(c->store_sid);
            c->store_sid = NULL;
            req_free(r);
            hist_close(c);
        }
        goto starting;
    }
    hist_refresh(c);
    run_request(c, r);
    ok = true;
starting:
    c->starting = false;
out:
    free(q);
    tui_request_render(u->tui);
    return ok;
}

static void handle_follow_up(btw_ctl_t *c) {
    btw_req_t *r = c->active;
    if (!r || !can_follow_up(c)) return;
    hist_t *h = hist_show(c);
    if (h) hist_open_follow_up(h, json_get_str(r->record, "id"));
}

static void handle_branch(btw_ctl_t *c) {
    ui_t *u = c->u;
    const char *why = branch_unavailable(c);
    if (why) {
        char *m = xasprintf("/btw branch unavailable: %s", why);
        add_notice(u, "info", m);
        free(m);
        return;
    }
    btw_req_t *r = c->active;
    c->branch_in_flight = true;
    panel_mark(&r->panel, PS_BRANCHING, NULL);
    tui_render_now(u->tui);
    char *err = NULL;
    char *q = xstrdup(c->last_question), *leaf = xstrdup(c->last_leaf), *sid = xstrdup(c->last_sid);
    json_t *msg = json_clone(c->last_assistant), *imgs = c->last_images ? json_clone(c->last_images) : NULL;
    if (!flush(c, &err) || app_branch_from_btw(u->app, q, imgs, msg, leaf, sid, &err) != 0) {
        char *m = xasprintf("Cannot branch /btw: %s", err ? err : "failed"), *s = sanitize_error_line(m);
        add_notice(u, "error", s);
        free(s), free(m);
        c->branch_in_flight = false;
        if (c->active == r) panel_mark(&r->panel, PS_COMPLETE, NULL);
    } else {
        c->branch_in_flight = false;
        dispose(c);
        ui_chat_rebuild(u);
        tui_request_full_render(u->tui);
        const char *f = app_session_file(u->app);
        char *m = f ? xasprintf("Branched /btw to %s", path_basename(f)) : xstrdup("Branched /btw");
        add_notice(u, "info", m);
        free(m);
    }
    free(err), free(q), free(leaf), free(sid);
    json_free(msg);
    json_free(imgs);
    tui_request_render(u->tui);
}

/* ---------- inline container ---------- */

static void box_render(tui_component_t *cp, int width, strvec_t *out) {
    btw_ctl_t *c = ((ui_t *)cp->user)->btw;
    if (!c || !c->visible || !c->active) return;
    panel_render(c, &c->active->panel, width, out);
}

static void box_invalidate(tui_component_t *cp) {
    btw_ctl_t *c = ((ui_t *)cp->user)->btw;
    if (c && c->active) {
        free(c->active->panel.md_text);
        c->active->panel.md_text = NULL;
    }
}

static const tui_component_vt BOX_VT = {"btw-container", box_render, NULL, box_invalidate, NULL};

tui_component_t *ui_btw_container_new(ui_t *u) {
    btw_ctl_t *c = ctl(u);
    c->box = xcalloc(1, sizeof *c->box);
    tui_component_init(c->box, &BOX_VT);
    c->box->user = u;
    return c->box;
}

bool ui_btw_command(ui_t *u, const char *question, const json_t *images, const char *title) {
    return start(ctl(u), question, NULL, images, title);
}

bool ui_btw_key(ui_t *u, const tui_key_t *k) {
    if (!u->btw) return false;
    btw_ctl_t *c = u->btw;
    if (tui_key_matches(k, "escape")) return (has_active(c) || c->branch_in_flight) && handle_escape(c);
    if (tui_focus(u->tui) != &u->editor->base || !tui_editor_is_empty(u->editor)) return false;
    if (tui_key_matches(k, "b") && handles_branch_key(c)) {
        handle_branch(c);
        return true;
    }
    if (tui_key_matches(k, "f") && can_follow_up(c)) {
        handle_follow_up(c);
        return true;
    }
    if (tui_key_matches(k, "c") && can_copy(c)) {
        copy_answer(c, c->last_copy, NULL);
        return true;
    }
    return false;
}

void ui_btw_dispose(ui_t *u) {
    if (u->btw) dispose(u->btw);
}

void ui_btw_free(ui_t *u) {
    btw_ctl_t *c = u->btw;
    if (!c) return;
    dispose(c);
    u->btw = NULL;
    /* the box component is owned by the root container; its render reads u->btw lazily */
    free(c);
}

/* ---------- BTW history (btw-history-panel.ts) ---------- */

struct hist {
    tui_component_t base;
    btw_ctl_t *c;
    json_t *records; /* newest first */
    char *selected_id;
    int focus; /* 0 list, 1 answer */
    int list_scroll, list_height;
    /* follow-up composer */
    bool composing;
    char *compose_id;
    ui_input_t input;
    char *notice;
    bool follow_pending, follow_latest;
    char *copied_id, *copied_text;
    /* answer pane */
    int detail_scroll;
    strvec_t detail;
    int detail_width;
    char *detail_id;
    bool detail_dirty;
};

static const struct {
    const char *status, *label;
    tui_tc_t color;
} STATUS[] = {{"running", "Running", TUI_TC_ACCENT},          {"complete", "Complete", TUI_TC_SUCCESS},
              {"cancelled", "Cancelled", TUI_TC_WARNING},     {"error", "Error", TUI_TC_ERROR},
              {"interrupted", "Interrupted", TUI_TC_WARNING}};

static size_t status_ix(const char *s) {
    for (size_t i = 0; i < ARRAY_LEN(STATUS); i++)
        if (str_eq(STATUS[i].status, s)) return i;
    return 0;
}

static size_t h_count(hist_t *h) { return json_len(h->records); }

static int h_selected_index(hist_t *h) {
    for (size_t i = 0; i < h_count(h); i++)
        if (str_eq(json_get_str(json_at(h->records, i), "id"), h->selected_id)) return (int)i;
    return 0;
}

static json_t *h_selected(hist_t *h) { return h_count(h) ? json_at(h->records, (size_t)h_selected_index(h)) : NULL; }

static bool h_is_copied(hist_t *h, const json_t *rec) {
    if (!rec || !h->copied_id || !str_eq(json_get_str(rec, "id"), h->copied_id)) return false;
    char *t = btw_copy_text(rec);
    bool same = t && str_eq(t, h->copied_text);
    free(t);
    return same;
}

static void h_set_selected(hist_t *h, const char *id) {
    free(h->selected_id);
    h->selected_id = id ? xstrdup(id) : NULL;
}

static void h_select(hist_t *h, int index) {
    if (!h_count(h)) return;
    index = MAX(0, MIN(index, (int)h_count(h) - 1));
    const char *id = json_get_str(json_at(h->records, (size_t)index), "id");
    if (str_eq(id, h->selected_id)) return;
    h_set_selected(h, id);
    h->follow_latest = false;
    h->detail_scroll = 0;
    h->detail_dirty = true;
}

static bool h_can_follow_up(hist_t *h, const json_t *rec) {
    btw_ctl_t *c = h->c;
    return !h->follow_pending && rec && !c->starting && !c->branch_in_flight && c->transition == 0 &&
           !str_eq(btw_latest_status(rec), "running") && (!c->active || !str_eq(latest_status(c->active), "running"));
}

static void h_update(hist_t *h, json_t *records) {
    json_free(h->records);
    h->records = records;
    h->detail_dirty = true;
    json_t *copied = NULL;
    for (size_t i = 0; h->copied_id && i < h_count(h); i++)
        if (str_eq(json_get_str(json_at(records, i), "id"), h->copied_id)) copied = json_at(records, i);
    if (!copied || !h_is_copied(h, copied)) {
        free(h->copied_id), free(h->copied_text);
        h->copied_id = h->copied_text = NULL;
    }
    bool has_compose = false, has_sel = false;
    for (size_t i = 0; i < h_count(h); i++) {
        const char *id = json_get_str(json_at(records, i), "id");
        if (h->composing && str_eq(id, h->compose_id)) has_compose = true;
        if (str_eq(id, h->selected_id)) has_sel = true;
    }
    if (h->composing && !has_compose) h->composing = false;
    if (!has_sel) {
        h_set_selected(h, h_count(h) ? json_get_str(json_at(records, 0), "id") : NULL);
        h->list_scroll = 0;
        h->follow_latest = false;
        h->detail_scroll = 0;
    }
    /* one side question has nothing to pick: its answer keeps the arrow keys */
    if (h_count(h) == 1) h->focus = 1;
    tui_request_render(h->c->u->tui);
}

static void hist_mark_copied(hist_t *h, const char *record_id, const char *text) {
    free(h->copied_id), free(h->copied_text);
    h->copied_id = xstrdup(record_id);
    h->copied_text = xstrdup(text);
    h->detail_dirty = true;
    tui_request_render(h->c->u->tui);
}

static void h_jump_to_end(hist_t *h) { h->follow_latest = true; }

static void h_open_composer(hist_t *h, const json_t *rec) {
    sb_free(&h->input.v);
    h->input.cur = 0;
    free(h->compose_id);
    h->compose_id = xstrdup(json_get_str(rec, "id"));
    free(h->notice);
    h->notice = NULL;
    h->composing = true;
    h->focus = 1;
    h_jump_to_end(h); /* following up reads from the newest turn, right above the composer */
    tui_request_render(h->c->u->tui);
}

static bool hist_open_follow_up(hist_t *h, const char *record_id) {
    if (h->composing) return false;
    for (size_t i = 0; i < h_count(h); i++) {
        json_t *rec = json_at(h->records, i);
        if (!str_eq(json_get_str(rec, "id"), record_id)) continue;
        if (!h_can_follow_up(h, rec)) return false;
        h_select(h, (int)i);
        h_open_composer(h, rec);
        return true;
    }
    return false;
}

static void h_submit(hist_t *h) {
    if (h->follow_pending || !h->composing) return;
    char *q = str_trim(h->input.v.s ? h->input.v.s : "");
    free(h->notice);
    h->notice = NULL;
    if (!*q) {
        h->notice = xstrdup("Enter a follow-up question.");
        free(q);
        return;
    }
    json_t *rec = NULL;
    for (size_t i = 0; i < h_count(h); i++)
        if (str_eq(json_get_str(json_at(h->records, i), "id"), h->compose_id)) rec = json_at(h->records, i);
    if (!rec || !h_can_follow_up(h, rec)) {
        h->notice = xstrdup("A BTW request is busy. Try again when it finishes.");
        free(q);
        return;
    }
    h->follow_pending = true;
    h->notice = xstrdup("Starting follow-up\xe2\x80\xa6");
    char *id = xstrdup(h->compose_id);
    btw_ctl_t *c = h->c;
    bool accepted = start(c, q, id, NULL, NULL);
    /* the panel may have been closed meanwhile */
    if (c->hist == h) {
        h->follow_pending = false;
        if (accepted) {
            h->composing = false;
            h_set_selected(h, id);
            h->focus = 1;
            h_jump_to_end(h);
            free(h->notice);
            h->notice = NULL;
        } else if (h->composing) {
            char *k = ui_editor_key("tui.input.submit");
            free(h->notice);
            h->notice = xasprintf("Follow-up was not started. Your draft is kept; %s to retry.", k);
            free(k);
        }
        tui_request_render(c->u->tui);
    }
    free(id);
    free(q);
}

/* the selected answer pane's scroll keys; a scroll stops following the newest row unless it is End */
static bool h_scroll_detail(hist_t *h, const tui_key_t *k, int page) {
    if (tui_kb(k, "tui.select.up")) h->detail_scroll--;
    else if (tui_kb(k, "tui.select.down")) h->detail_scroll++;
    else if (tui_kb(k, "tui.select.pageUp")) h->detail_scroll -= MAX(1, page - 1);
    else if (tui_kb(k, "tui.select.pageDown")) h->detail_scroll += MAX(1, page - 1);
    else if (tui_key_matches(k, "shift+up")) h->detail_scroll -= 5;
    else if (tui_key_matches(k, "shift+down")) h->detail_scroll += 5;
    else if (tui_key_matches(k, "home")) h->detail_scroll = 0;
    else if (tui_key_matches(k, "end")) h->detail_scroll = INT32_MAX / 2;
    else return false;
    h->follow_latest = tui_key_matches(k, "end");
    if (h->detail_scroll < 0) h->detail_scroll = 0;
    return true;
}

static int g_page_rows = 10;

static bool hist_key(tui_component_t *cp, const tui_key_t *k) {
    hist_t *h = (hist_t *)cp;
    btw_ctl_t *c = h->c;
    if (h->composing) {
        if (tui_kb(k, "tui.input.submit") || tui_key_matches(k, "enter")) h_submit(h);
        else if (tui_key_matches(k, "escape")) {
            h->composing = false;
            free(h->notice);
            h->notice = NULL;
        } else if (!ui_input_key(&h->input, k)) h_scroll_detail(h, k, g_page_rows);
        tui_request_render(c->u->tui);
        return true;
    }
    json_t *rec = h_selected(h);
    bool running = rec && str_eq(btw_latest_status(rec), "running");
    if (tui_kb(k, "tui.select.cancel") || tui_key_matches(k, "escape")) {
        if (running) {
            if (c->active && str_eq(json_get_str(c->active->record, "id"), json_get_str(rec, "id"))) handle_cancel(c);
        } else hist_close(c);
        return true;
    }
    if (tui_key_matches(k, "f") || tui_key_matches(k, "enter")) {
        if (rec) hist_open_follow_up(h, json_get_str(rec, "id"));
        return true;
    }
    if (tui_key_matches(k, "c")) {
        char *t = rec ? btw_copy_text(rec) : NULL;
        if (t) {
            char *id = xstrdup(json_get_str(rec, "id"));
            copy_answer(c, t, id);
            free(id);
        }
        free(t);
        return true;
    }
    if (tui_key_matches(k, "tab") || tui_key_matches(k, "shift+tab") || tui_key_matches(k, "ctrl+/") ||
        tui_key_matches(k, "ctrl+_")) {
        if (h_count(h) > 1) h->focus = !h->focus;
    } else if (tui_key_matches(k, "right")) {
        h->focus = 1;
    } else if (tui_key_matches(k, "left")) {
        if (h_count(h) > 1) h->focus = 0;
    } else if (h->focus == 0) {
        int i = h_selected_index(h);
        if (tui_kb(k, "tui.select.up")) h_select(h, i - 1);
        else if (tui_kb(k, "tui.select.down")) h_select(h, i + 1);
        else if (tui_kb(k, "tui.select.pageUp")) h_select(h, i - h->list_height);
        else if (tui_kb(k, "tui.select.pageDown")) h_select(h, i + h->list_height);
        else if (tui_key_matches(k, "home")) h_select(h, 0);
        else if (tui_key_matches(k, "end")) h_select(h, (int)h_count(h) - 1);
        else return true;
    } else if (!h_scroll_detail(h, k, g_page_rows)) {
        return true;
    }
    tui_request_render(c->u->tui);
    return true;
}

/* one turn: status line, Question, Answer, error / interrupted notes */
static void h_render_turn(const json_t *turn, const char *question, int width, strvec_t *out) {
    const char *st = json_get_str(turn, "status");
    size_t si = status_ix(st);
    char date[64];
    fmt_time(json_get_int(turn, "createdAt", 0), true, date, sizeof date);
    char *head0 = xasprintf("%s \xc2\xb7 %s", STATUS[si].label, date), *head = sfg(STATUS[si].color, head0);
    tui_wrap_text_with_ansi(head, width, out);
    free(head0), free(head);
    char *lab = sfg(TUI_TC_ACCENT, "Question"), *bl = tui_bold(lab);
    strvec_push(out, "");
    strvec_push(out, bl);
    free(lab), free(bl);
    md_lines(question, width, out);
    strvec_push(out, "");
    lab = sfg(TUI_TC_ACCENT, "Answer");
    bl = tui_bold(lab);
    strvec_push(out, bl);
    free(lab), free(bl);
    const char *ans = json_get_str(turn, "answer");
    if (!blank(ans)) md_lines(ans, width, out);
    else {
        char *w = sfg(TUI_TC_DIM, str_eq(st, "running") ? "Waiting for response\xe2\x80\xa6" : "No answer text.");
        tui_wrap_text_with_ansi(w, width, out);
        free(w);
    }
    const char *err = json_get_str(turn, "error");
    if (err) {
        char *e = sanitize_error_line(err), *s = sfg(TUI_TC_ERROR, e);
        strvec_push(out, "");
        tui_wrap_text_with_ansi(s, width, out);
        free(e), free(s);
    }
    if (str_eq(st, "interrupted")) {
        char *s = sfg(TUI_TC_MUTED, "Not resumed in this view.");
        strvec_push(out, "");
        tui_wrap_text_with_ansi(s, width, out);
        free(s);
    }
}

static void h_render_detail(hist_t *h, int width) {
    json_t *rec = h_selected(h);
    const char *rid = rec ? json_get_str(rec, "id") : NULL;
    if (!h->detail_dirty && h->detail_width == width && str_eq(h->detail_id ? h->detail_id : "", rid ? rid : "")) return;
    strvec_free(&h->detail);
    int inner = MAX(1, width - 1);
    if (rec && h_is_copied(h, rec)) {
        char *s = sfg(TUI_TC_SUCCESS, "\xe2\x9c\x93 Copied to clipboard");
        tui_wrap_text_with_ansi(s, inner, &h->detail);
        strvec_push(&h->detail, "");
        free(s);
    }
    if (rec) {
        const json_t *turns[512];
        size_t n = btw_turns(rec, turns, ARRAY_LEN(turns));
        for (size_t i = 0; i < n; i++) {
            if (i > 0) {
                sb_t rule = SB_INIT;
                sb_repeat(&rule, tui_sym("boxRound.horizontal"), inner);
                char *r = sfg(TUI_TC_DIM, sb_cstr(&rule));
                strvec_push(&h->detail, "");
                strvec_push_own(&h->detail, r);
                strvec_push(&h->detail, "");
                sb_free(&rule);
            }
            /* a titled topic's opening question displays as its title */
            const char *q = i == 0 && json_get_str(rec, "title") ? json_get_str(rec, "title") : json_get_str(turns[i], "question");
            h_render_turn(turns[i], q ? q : "", inner, &h->detail);
        }
    } else {
        char *s = sfg(TUI_TC_MUTED, "No side questions yet. Use /btw QUESTION to start one.");
        tui_wrap_text_with_ansi(s, inner, &h->detail);
        free(s);
    }
    free(h->detail_id);
    h->detail_id = rid ? xstrdup(rid) : NULL;
    h->detail_width = width;
    h->detail_dirty = false;
}

/* renderScrollableList: rows already windowed; a track/thumb column when the list overflows */
static void h_render_list(hist_t *h, int width, int height, strvec_t *out) {
    int n = (int)h_count(h);
    int span = width < 36 && height >= 2 ? 2 : 1;
    h->list_height = MAX(1, height / span);
    if (!n) {
        strvec_t t = {0};
        char *s = sfg(TUI_TC_MUTED, "No side questions yet.\n\nUse /btw QUESTION to start one.");
        ui_push_text(&t, s, width);
        free(s);
        for (size_t i = 0; i < t.n && (int)i < height; i++) strvec_push(out, t.v[i]);
        strvec_free(&t);
        return;
    }
    int sel = h_selected_index(h);
    /* clampSelection: keep the selection in the window (nearest) */
    if (sel < h->list_scroll) h->list_scroll = sel;
    else if (sel >= h->list_scroll + h->list_height) h->list_scroll = sel - h->list_height + 1;
    h->list_scroll = MIN(h->list_scroll, MAX(0, n - h->list_height));
    h->list_scroll = MAX(0, h->list_scroll);
    int total = n * span;
    bool bar = total > height && width > 1;
    int cw = MAX(0, width - (bar ? 1 : 0));
    strvec_t rows = {0};
    for (int i = h->list_scroll; i < MIN(n, h->list_scroll + h->list_height); i++) {
        json_t *rec = json_at(h->records, (size_t)i);
        bool selected = i == sel;
        size_t si = status_ix(btw_latest_status(rec));
        char *cursor = selected ? sfg(h->focus == 0 ? TUI_TC_ACCENT : TUI_TC_MUTED, tui_sym("nav.cursor")) : xstrdup(" ");
        char tm[16];
        fmt_time(json_get_int(rec, "createdAt", 0), false, tm, sizeof tm);
        char *time = sfg(TUI_TC_DIM, tm), *badge = sfg(STATUS[si].color, STATUS[si].label);
        char *prefix = xasprintf("%s %s %s ", cursor, time, badge);
        char *question = display_line(btw_topic_label(rec));
        char *snippet = tui_truncate_to_width(question, MAX(0, cw - (span == 2 ? 2 : tui_visible_width(prefix))), "\xe2\x80\xa6");
        char *bsn = selected ? tui_bold(snippet) : xstrdup(snippet);
        char *text = span == 2 ? xstrdup(prefix) : xasprintf("%s%s", prefix, bsn);
        if (selected) {
            char *pad = cw > 0 ? tui_pad_to_width(text, cw) : xstrdup("");
            strvec_push_own(&rows, tui_bgs(NULL, TUI_TC_SELECTED_BG, pad));
            free(pad);
        } else strvec_push_own(&rows, tui_truncate_to_width(text, cw, "\xe2\x80\xa6"));
        if (span == 2) {
            char *ql = xasprintf("  %s", bsn);
            if (selected) {
                char *pad = cw > 0 ? tui_pad_to_width(ql, cw) : xstrdup("");
                strvec_push_own(&rows, tui_bgs(NULL, TUI_TC_SELECTED_BG, pad));
                free(pad);
                free(ql);
            } else strvec_push_own(&rows, ql);
        }
        free(cursor), free(time), free(badge), free(prefix), free(question), free(snippet), free(bsn), free(text);
    }
    while ((int)rows.n < height) strvec_push(&rows, "");
    int ts = 0, te = height;
    if (bar) {
        int size = MAX(1, MIN((height * height) / total, height));
        int maxoff = MAX(1, total - height);
        ts = (int)lround((double)MIN(h->list_scroll * span, maxoff) / maxoff * (height - size));
        te = ts + size;
    }
    for (int i = 0; i < height && i < (int)rows.n; i++) {
        if (!bar) {
            strvec_push_own(out, tui_truncate_to_width(rows.v[i], width, ""));
            continue;
        }
        char *pad = tui_pad_to_width(rows.v[i], cw);
        bool thumb = i >= ts && i < te;
        char *g = sfg(thumb ? TUI_TC_ACCENT : TUI_TC_MUTED, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
        strvec_push_own(out, xasprintf("%s%s", pad, g));
        free(pad), free(g);
    }
    strvec_free(&rows);
}

static char *h_focus_label(hist_t *h, int which) {
    char *label = which == 0 ? xasprintf("History (%zu)", h_count(h)) : xstrdup("Details");
    char *r;
    if (h->focus == which) {
        char *t = xasprintf("%s %s", tui_sym("nav.cursor"), label), *f = sfg(TUI_TC_ACCENT, t);
        r = tui_bold(f);
        free(t), free(f);
    } else {
        char *t = xasprintf("  %s", label);
        r = sfg(TUI_TC_MUTED, t);
        free(t);
    }
    free(label);
    return r;
}

static char *fit(const char *s, int w) {
    if (w <= 0) return xstrdup("");
    if (tui_visible_width(s) > w) return tui_truncate_to_width(s, w, "");
    return tui_pad_to_width(s, w);
}

static void hist_render(tui_component_t *cp, int width, strvec_t *out) {
    hist_t *h = (hist_t *)cp;
    btw_ctl_t *c = h->c;
    int height = MAX(0, tui_term(c->u->tui)->rows);
    if (width <= 0 || height <= 0) return;
    bool framed = width >= 12 && height >= 8;
    int inner = MAX(1, width - (framed ? 4 : 0));
    /* SplitPane: "│ " + left + " │ " + right + " │", split at >= 96 columns, else the focused pane alone */
    bool wide = framed && width >= 96;
    int avail = MAX(0, width - 7);
    int lw = wide ? MIN(46, MIN(avail, (int)floor(width * 0.42))) : inner;
    int rw = wide ? MAX(0, avail - lw) : inner;
    json_t *rec = h_selected(h);
    bool running = rec && str_eq(btw_latest_status(rec), "running");
    strvec_t acts = {0};
    if (h->composing) {
        strvec_push_own(&acts, editor_key_hint("tui.input.submit", h->follow_pending ? "starting\xe2\x80\xa6" : "send"));
        strvec_push_own(&acts, editor_key_hint("tui.select.cancel", "cancel"));
    } else {
        strvec_push_own(&acts, editor_key_hint("tui.select.cancel", running ? "cancel" : "close"));
        if (h_count(h) > 1) {
            char *tk = ui_key_hint("tab"), *sk = ui_key_hint("ctrl+/"), *ks = xasprintf("%s/%s", tk, sk);
            strvec_push_own(&acts, key_hint(ks, "switch pane"));
            free(tk), free(sk), free(ks);
        }
        if (rec && h_can_follow_up(h, rec)) {
            char *ek = ui_key_hint("enter"), *ks = xasprintf("f/%s", ek);
            strvec_push_own(&acts, key_hint(ks, "follow up"));
            free(ek), free(ks);
        }
        char *ct = rec ? btw_copy_text(rec) : NULL;
        if (ct) {
            if (h_is_copied(h, rec)) strvec_push_own(&acts, sfg(TUI_TC_SUCCESS, "\xe2\x9c\x93 copied \xc2\xb7 c to copy again"));
            else strvec_push_own(&acts, key_hint("c", inner < 40 ? "copy" : "copy answer"));
        }
        free(ct);
    }
    char *aj = strvec_join(&acts, " \xc2\xb7 ");
    strvec_free(&acts);
    strvec_t action_lines = {0}, wrapped = {0};
    tui_wrap_text_with_ansi(aj, inner, &wrapped);
    free(aj);
    for (size_t i = 0; i < wrapped.n && (int)i < (framed ? 2 : 1); i++) strvec_push(&action_lines, wrapped.v[i]);
    strvec_free(&wrapped);
    int chrome = framed ? 3 + (int)action_lines.n : height >= 3 ? 2 : 0;
    strvec_t composer = {0};
    if (h->composing) {
        int rows = MAX(1, height - chrome - 1);
        if (rows >= 2) {
            char *lab = display_line(rec ? btw_topic_label(rec) : ""), *t = xasprintf("Topic: %s", lab);
            char *tr = tui_truncate_to_width(t, inner, "\xe2\x80\xa6");
            strvec_push_own(&composer, sfg(TUI_TC_DIM, tr));
            free(lab), free(t), free(tr);
        }
        if (h->notice && rows >= 3) {
            char *tr = tui_truncate_to_width(h->notice, inner, "\xe2\x80\xa6");
            strvec_push_own(&composer, sfg(h->follow_pending ? TUI_TC_DIM : TUI_TC_WARNING, tr));
            free(tr);
        }
        char *prompt = sfg(TUI_TC_ACCENT, "Follow up: ");
        strvec_push_own(&composer, ui_input_render(&h->input, prompt, inner, true, c->u->editor->use_terminal_cursor));
        free(prompt);
    }
    int body_h = MAX(h->composing ? 0 : 1, height - chrome - (int)composer.n);
    int dw = wide ? rw : inner;
    bool show_detail = wide || h->focus == 1;
    if (show_detail) h_render_detail(h, dw);
    int max_off = show_detail ? MAX(0, (int)h->detail.n - body_h) : 0;
    bool nav = framed && !h->composing && (h->focus == 0 || max_off > 0);
    if (nav) body_h = MAX(1, body_h - 1);
    g_page_rows = body_h;
    strvec_t detail = {0};
    if (show_detail) {
        if (h->follow_latest) h->detail_scroll = MAX(0, (int)h->detail.n - body_h);
        ui_scroll_view(&h->detail, body_h, &h->detail_scroll, dw, &detail);
    }
    strvec_t lines = {0};
    if (framed) {
        strvec_push_own(&lines, ui_box_top(width, "BTW history"));
        char *bar = sfg(TUI_TC_BORDER, tui_sym("boxRound.vertical"));
        char *pre = xasprintf("%s ", bar), *div = xasprintf(" %s ", bar), *suf = xasprintf(" %s", bar);
        if (wide) {
            char *l = h_focus_label(h, 0), *r = h_focus_label(h, 1), *lf = fit(l, lw), *rf = fit(r, rw);
            strvec_push_own(&lines, xasprintf("%s%s%s%s%s", pre, lf, div, rf, suf));
            free(l), free(r), free(lf), free(rf);
        } else {
            char *l = h_focus_label(h, h->focus), *lf = fit(l, inner);
            strvec_push_own(&lines, xasprintf("%s%s%s", pre, lf, suf));
            free(l), free(lf);
        }
        strvec_t list = {0};
        if (wide || h->focus == 0) h_render_list(h, wide ? lw : inner, body_h, &list);
        for (int i = 0; i < body_h; i++) {
            if (wide) {
                char *lf = fit(i < (int)list.n ? list.v[i] : "", lw), *rf = fit(i < (int)detail.n ? detail.v[i] : "", rw);
                strvec_push_own(&lines, xasprintf("%s%s%s%s%s", pre, lf, div, rf, suf));
                free(lf), free(rf);
            } else {
                const strvec_t *pane = h->focus == 0 ? &list : &detail;
                char *f = fit(i < (int)pane->n ? pane->v[i] : "", inner);
                strvec_push_own(&lines, xasprintf("%s%s%s", pre, f, suf));
                free(f);
            }
        }
        strvec_free(&list);
        free(bar), free(pre), free(div), free(suf);
    } else {
        if (height >= 3) {
            char *l = h_focus_label(h, h->focus);
            strvec_push_own(&lines, tui_truncate_to_width(l, width, ""));
            free(l);
        }
        strvec_t body = {0};
        if (h->focus == 0) h_render_list(h, inner, body_h, &body);
        const strvec_t *src = h->focus == 0 ? &body : &detail;
        for (int i = 0; i < body_h; i++) strvec_push(&lines, i < (int)src->n ? src->v[i] : "");
        strvec_free(&body);
    }
    for (size_t i = 0; i < composer.n; i++) strvec_push_own(&lines, framed ? ui_box_row(composer.v[i], width) : xstrdup(composer.v[i]));
    if (framed) {
        if (nav) {
            char *up = ui_editor_key("tui.select.up"), *dn = ui_editor_key("tui.select.down");
            char *ks = xasprintf("%s/%s", up, dn), *kd = sfg(TUI_TC_DIM, ks);
            char *lab = sfg(TUI_TC_MUTED, h->focus == 0 ? " select" : " scroll"), *row = xasprintf("%s%s", kd, lab);
            strvec_push_own(&lines, ui_box_row(row, width));
            free(up), free(dn), free(ks), free(kd), free(lab), free(row);
        }
        for (size_t i = 0; i < action_lines.n; i++) strvec_push_own(&lines, ui_box_row(action_lines.v[i], width));
        strvec_push_own(&lines, ui_box_bottom(width));
    } else if (height >= 3) {
        strvec_push(&lines, action_lines.n ? action_lines.v[0] : "");
    }
    for (size_t i = 0; i < lines.n && (int)i < height; i++) strvec_push_own(out, tui_truncate_to_width(lines.v[i], width, ""));
    for (int i = (int)MIN(lines.n, (size_t)height); i < height; i++) strvec_push(out, "");
    strvec_free(&lines);
    strvec_free(&detail);
    strvec_free(&composer);
    strvec_free(&action_lines);
}

static void hist_invalidate(tui_component_t *cp) { ((hist_t *)cp)->detail_dirty = true; }

static void hist_destroy(tui_component_t *cp) {
    hist_t *h = (hist_t *)cp;
    json_free(h->records);
    free(h->selected_id);
    free(h->compose_id);
    sb_free(&h->input.v);
    free(h->notice);
    free(h->copied_id);
    free(h->copied_text);
    strvec_free(&h->detail);
    free(h->detail_id);
    free(h);
}

static const tui_component_vt HIST_VT = {"btw-history", hist_render, hist_key, hist_invalidate, hist_destroy};

/* #showHistory: the fullscreen BTW history (alternate screen); hides the inline panel */
static hist_t *hist_show(btw_ctl_t *c) {
    if (c->hist) {
        hist_refresh(c);
        return c->hist;
    }
    hide_inline(c);
    hist_t *h = xcalloc(1, sizeof *h);
    tui_component_init(&h->base, &HIST_VT);
    h->c = c;
    h->records = history_records(c);
    h->detail_dirty = true;
    if (h_count(h)) h_set_selected(h, json_get_str(json_at(h->records, 0), "id"));
    if (h_count(h) == 1) h->focus = 1;
    c->hist = h;
    tui_alt_screen(c->u->tui, true);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(c->u->tui, &h->base, &o);
    return h;
}

static void hist_refresh(btw_ctl_t *c) {
    if (c->hist && c->store) h_update(c->hist, history_records(c));
}

/* #closeHistory: closing while a BTW still answers keeps it running in the inline panel */
static void hist_close(btw_ctl_t *c) {
    hist_t *h = c->hist;
    if (!h) return;
    c->hist = NULL;
    ui_t *u = c->u;
    tui_overlay_hide(u->tui, &h->base);
    tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    tui_component_destroy(&h->base);
    btw_req_t *r = c->active;
    if (r && is_active(c, r) && str_eq(latest_status(r), "running")) {
        panel_set_answer(&r->panel, json_get_str(btw_latest_turn(r->record), "answer"));
        c->visible = true;
    }
    tui_request_render(u->tui);
}
