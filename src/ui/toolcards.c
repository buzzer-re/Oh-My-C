/* toolcards.c - tool execution cards in the transcript (port of omp chat/tool-execution.ts and the
 * read-group wiring of the event controller). The renderers live in ui/tools/ (pure functions over a
 * tc_call_t); this file keeps per-card state and drives live repaints.
 *
 * Card state lives in the view's `msg` json (MV_TOOL views carry no assistant message), so the view's
 * destructor in chat.c frees it:
 *   {"result": {content, details} | absent, "partial": bool, "isError": bool, "startMs": n, "endMs": n,
 *    "group": [ {id, args, result, done, isError}, ... ]   (grouped reads only)} */
#include "tui/motion.h"
#include "ui/ui.h"
#include "ui/tools/tc.h"
#include "tui/termcaps.h"

static msgview_t *g_read_group; /* the read group a run of reads accretes into (omp #lastReadGroup) */
static VEC(msgview_t *) g_live; /* running cards that repaint on a timer */
static int g_timer;
static hmap_t g_bg;       /* tool call id -> card that may receive background job frames (task) */
static bool g_replaying;  /* history replay: persisted async results are not live */
/* latest background-job frame per returned call, keyed "<session id>\x1f<call id>" (omp AgentSession
 * #activeToolExecutionUpdates): a returned call's persisted result is only its "running" snapshot, so a transcript
 * rebuild replays the newest frame instead. Keyed by session, so a /new or switched session never sees them. */
static json_t *g_frames;
#define FRAMES_MAX 256

static char *frame_key(ui_t *u, const char *id) {
    const char *sid = u && u->app && u->app->session ? session_id(u->app->session) : "";
    return xasprintf("%s\x1f%s", sid ? sid : "", id);
}

static void frame_remember(ui_t *u, const char *id, const json_t *details) {
    if (!id || u->view) return; /* main-session cards only */
    if (!g_frames) g_frames = json_obj();
    char *k = frame_key(u, id);
    json_del(g_frames, k);
    while (json_len(g_frames) >= FRAMES_MAX) json_del(g_frames, g_frames->u.obj.keys[0]);
    json_set(g_frames, k, json_clone(details));
    free(k);
}

static const json_t *frame_lookup(ui_t *u, const char *id) {
    if (!g_frames || !id || u->view) return NULL;
    char *k = frame_key(u, id);
    const json_t *f = json_get(g_frames, k);
    free(k);
    return f;
}

static bool is_async_state(const json_t *details, const char *state) {
    return str_eq(json_get_str(json_get_obj(details, "async"), "state"), state);
}

static json_t *state_of(msgview_t *v) {
    if (!v->msg) v->msg = json_obj();
    return v->msg;
}

char *args_summary(const char *tool, const json_t *args) {
    const char *keys[] = {"path", "command", "pattern", "query", "url", "op", "input"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        const char *v = json_get_str(args, keys[i]);
        if (v) {
            char *first = xstrndup(v, strcspn(v, "\n"));
            if (strlen(first) > 120) strcpy(first + 117, "...");
            return first;
        }
    }
    (void)tool;
    char *s = json_dump(args, 0);
    if (strlen(s) > 100) strcpy(s + 97, "...");
    return s;
}

static int term_rows(void) {
    if (!g_ui || !g_ui->tui) return 0;
    tui_term_t *t = tui_term(g_ui->tui);
    return t ? t->rows : 0;
}

static const char *project_dir(void) { return g_ui && g_ui->app ? g_ui->app->cwd : NULL; }

static const char *tool_label(const char *name) {
    if (!g_ui || !g_ui->app || !g_ui->app->agent) return NULL;
    agent_tool_t *t = agent_find_tool(g_ui->app->agent, name);
    return t && t->label && *t->label ? t->label : NULL;
}

static int spinner_now(void) { return tui_spinner_tick(); }

/* ---------- live repaint ticker ---------- */

static bool view_attached(msgview_t *v) {
    if (!g_ui) return false;
    for (size_t i = 0; i < tui_container_count(g_ui->chat); i++)
        if (g_ui->chat->children.v[i] == &v->base) return true;
    return false;
}

/* Tools whose arguments stream a growing file body or diff into the card (omp streamsBody) */
static bool streams_body(const char *name) { return str_eq(name, "edit") || str_eq(name, "apply_patch") || str_eq(name, "write"); }

/* omp isStreamingArgs: the arguments of a body-streaming card are still arriving */
static bool stream_spinning(msgview_t *v) {
    if (v->done || !streams_body(v->tool_name)) return false;
    json_t *st = state_of(v);
    return !json_get_int(st, "startMs", 0) && !json_get_bool(st, "argsComplete", false) && !json_get_obj(st, "result");
}

static void tick(void *ud) {
    UNUSED(ud);
    bool any = false;
    for (size_t i = 0; i < g_live.n;) {
        msgview_t *v = g_live.v[i];
        /* a body-streaming card only ticks for its header spinner, which stops with the args */
        bool settled = streams_body(v->tool_name) && !tc_wants_tick(v->tool_name) && !stream_spinning(v);
        if (!view_attached(v) || v->done || settled) {
            g_live.v[i] = g_live.v[--g_live.n];
            continue;
        }
        v->dirty = true;
        any = true;
        i++;
    }
    /* a tick under a static spinner paints frame 0: nothing to repaint */
    if (any && tui_spinner_animated()) tui_request_render(g_ui->tui);
    if (!g_live.n && g_timer) {
        tui_remove_timer(g_ui->tui, g_timer);
        g_timer = 0;
    }
}

static void live_add(msgview_t *v) {
    if (!g_ui || !g_ui->tui) return;
    for (size_t i = 0; i < g_live.n; i++)
        if (g_live.v[i] == v) return;
    vec_push(&g_live, v);
    if (!g_timer) g_timer = tui_add_clock_timer(g_ui->tui, NULL, tick, NULL);
}

/* ---------- events ---------- */

msgview_t *toolcard_group_swap(msgview_t *group) {
    msgview_t *old = g_read_group;
    g_read_group = group;
    return old;
}

void toolcard_break_group(ui_t *u) {
    UNUSED(u);
    g_read_group = NULL;
}

bool toolcard_group_open(const msgview_t *v) { return v && v == g_read_group; }

/* tool calls whose arguments are still streaming: id -> pending card (omp draws the call before execution) */
static hmap_t g_preview;
static strvec_t g_stream_ids; /* tool call ids previewed by the assistant message streaming now */
/* read calls of this run whose streamed args named a target: omp's #toolTimelineComponents holds them (in the read
 * group) from the stream on, so their execution never evicts a prior turn's card with the same id */
static strvec_t g_run_streamed_reads;

void toolcard_preview(ui_t *u, const char *id, const char *name, const json_t *args) {
    if (id && *id && str_eq(name, "read")) {
        const char *path = json_get_str(args, "path");
        if (path && *path && !strvec_contains(&g_run_streamed_reads, id)) strvec_push(&g_run_streamed_reads, id);
    }
    if (!id || !*id || !name || !*name || str_eq(name, "read")) return;
    msgview_t *v = hmap_get(&g_preview, id);
    if (v && !view_attached(v)) {
        hmap_del(&g_preview, id);
        v = NULL;
    }
    if (v) {
        if (args && !json_equal(args, v->args)) {
            json_free(v->args);
            v->args = json_clone(args);
            v->dirty = true;
        }
        return;
    }
    g_read_group = NULL;
    v = mv_new(MV_TOOL);
    v->tool_name = xstrdup(name);
    v->tool_id = xstrdup(id);
    v->args = args ? json_clone(args) : json_obj();
    json_set_bool(state_of(v), "partial", true);
    add_view(u, v);
    hmap_put(&g_preview, id, v);
    strvec_push(&g_stream_ids, id);
    if (streams_body(name)) live_add(v); /* the card header's spinner animates while the body streams */
}

void toolcard_stream_begin(void) { strvec_free(&g_stream_ids); }

/* #retractStreamedToolPreviews: a provider replay restarts the message and can abandon previews without ever
 * executing them; no result would settle those cards, so drop the ones the restarted partial no longer holds */
void toolcard_retract_abandoned(ui_t *u, const json_t *partial) {
    size_t w = 0;
    for (size_t i = 0; i < g_stream_ids.n; i++) {
        const char *id = g_stream_ids.v[i];
        bool held = false;
        const json_t *blk;
        json_arr_foreach(json_get_arr(partial, "content"), k, blk) held |= str_eq(json_get_str(blk, "type"), "toolCall") &&
                                                                          str_eq(json_get_str(blk, "id"), id);
        msgview_t *v = held ? NULL : hmap_get(&g_preview, id);
        if (held || !v) {
            g_stream_ids.v[w++] = g_stream_ids.v[i];
            continue;
        }
        hmap_del(&g_preview, id);
        for (size_t j = 0; j < g_live.n; j++)
            if (g_live.v[j] == v) {
                memmove(&g_live.v[j], &g_live.v[j + 1], (g_live.n - j - 1) * sizeof *g_live.v);
                g_live.n--;
                break;
            }
        if (view_attached(v)) {
            tui_container_remove(u->chat, &v->base);
            tui_component_destroy(&v->base);
        }
        free(g_stream_ids.v[i]);
    }
    g_stream_ids.n = w;
}

void toolcard_args_complete(ui_t *u, const json_t *msg) {
    const char *stop = json_get_str(msg, "stopReason");
    if (str_eq(stop, "aborted") || str_eq(stop, "error")) return;
    json_t *blk;
    json_arr_foreach(json_get_arr(msg, "content"), i, blk) {
        const char *id = str_eq(json_get_str(blk, "type"), "toolCall") ? json_get_str(blk, "id") : NULL;
        msgview_t *v = id ? hmap_get(&g_preview, id) : NULL;
        if (!v || !view_attached(v)) continue;
        json_set_bool(state_of(v), "argsComplete", true);
        v->dirty = true;
    }
    if (u && u->tui) tui_request_render(u->tui);
}

msgview_t *toolcard_start(ui_t *u, const char *id, const char *name, json_t *args) {
    msgview_t *pv = id ? hmap_get(&g_preview, id) : NULL;
    if (pv) {
        hmap_del(&g_preview, id);
        if (view_attached(pv) && str_eq(pv->tool_name, name)) {
            json_free(pv->args);
            pv->args = args;
            json_set_int(state_of(pv), "startMs", mono_ms());
            pv->dirty = true;
            if (tc_wants_tick(name)) live_add(pv);
            if (str_eq(name, "task") && !g_replaying) hmap_put(&g_bg, id, pv);
            return pv;
        }
    }
    if (str_eq(name, "read") && tc_read_groupable(args)) {
        if (g_read_group && view_attached(g_read_group)) {
            json_t *grp = json_get_arr(state_of(g_read_group), "group");
            json_t *e = json_obj();
            json_set_str(e, "id", id);
            json_set(e, "args", args);
            json_push(grp, e);
            g_read_group->done = false;
            g_read_group->dirty = true;
            return g_read_group;
        }
        msgview_t *v = mv_new(MV_TOOL);
        v->tool_name = xstrdup(name);
        v->tool_id = xstrdup(id);
        v->args = json_clone(args);
        json_t *st = state_of(v);
        json_t *grp = json_arr();
        json_t *e = json_obj();
        json_set_str(e, "id", id);
        json_set(e, "args", args);
        json_push(grp, e);
        json_set(st, "group", grp);
        add_view(u, v);
        g_read_group = v;
        return v;
    }
    g_read_group = NULL;
    msgview_t *v = mv_new(MV_TOOL);
    v->tool_name = xstrdup(name);
    v->tool_id = xstrdup(id);
    v->args = args;
    json_set_int(state_of(v), "startMs", mono_ms());
    json_set_bool(state_of(v), "partial", true);
    add_view(u, v);
    if (tc_wants_tick(name)) live_add(v);
    if (str_eq(name, "task") && id && !g_replaying) hmap_put(&g_bg, id, v);
    return v;
}

void toolcard_result(ui_t *u, msgview_t *v, const char *id, json_t *result, bool partial, bool is_error) {
    json_t *st = state_of(v);
    json_t *grp = json_get_arr(st, "group");
    if (grp) {
        json_t *e;
        bool all_done = true;
        json_arr_foreach(grp, i, e) {
            if (str_eq(json_get_str(e, "id"), id) && !partial) {
                ui_read_images(u, id, json_get_arr(result, "content")); /* grouped read images draw in the assistant */
                json_set(e, "result", result);
                result = NULL;
                json_set_bool(e, "done", true);
                json_set_bool(e, "isError", is_error);
            }
            if (!json_get_bool(e, "done", false)) all_done = false;
        }
        json_free(result);
        v->done = all_done;
        v->dirty = true;
        return;
    }
    if (!result) result = json_obj();
    bool parked = false;
    /* parksAsBackground: detached `task` spawns only; backgrounded bash/eval park only under native rendering, the terminal
     * transcript keeps their card frozen as a settled result */
    bool job_card = str_eq(v->tool_name, "task") && is_async_state(json_get_obj(result, "details"), "running");
    const json_t *latest = !partial && g_replaying && job_card
                               ? frame_lookup(u, id)
                               : NULL;
    if (latest) { /* a rebuild: the job's newest frame supersedes the persisted "running" snapshot */
        json_set(result, "details", json_clone(latest));
        parked = is_async_state(latest, "running");
        if (is_async_state(latest, "failed")) is_error = true;
        if (parked && id) hmap_put(&g_bg, id, v); /* later frames keep landing on the rebuilt card */
    } else if (!partial && !g_replaying && job_card) {
        /* a background call returned: the card stays live for its job frames (omp parkAsBackground) */
        parked = true;
        json_t *frame = json_get_obj(st, "frame");
        if (frame) {
            json_set(result, "details", json_clone(frame));
            parked = is_async_state(frame, "running");
            if (is_async_state(frame, "failed")) is_error = true;
            json_del(st, "frame");
        }
    }
    json_set(st, "result", result);
    json_set_bool(st, "partial", partial || parked);
    json_set_bool(st, "parked", parked);
    json_set_bool(st, "isError", is_error);
    if (!partial) {
        json_set_int(st, "endMs", mono_ms());
        v->done = true;
        v->is_error = is_error;
        if (!parked && v->tool_id) hmap_del(&g_bg, v->tool_id);
    }
    v->dirty = true;
}

void toolcard_frame(ui_t *u, const char *id, json_t *details) {
    frame_remember(u, id, details);
    msgview_t *v = id ? hmap_get(&g_bg, id) : NULL;
    if (!v || !view_attached(v)) {
        if (v) hmap_del(&g_bg, id);
        json_free(details);
        return;
    }
    json_t *st = state_of(v);
    if (!v->done) { /* the call has not returned yet: its final result picks this frame up */
        json_set(st, "frame", details);
        return;
    }
    if (!json_get_bool(st, "parked", false)) {
        json_free(details);
        return;
    }
    json_t *result = json_get_obj(st, "result");
    json_set(result, "details", details);
    bool running = is_async_state(details, "running");
    bool failed = is_async_state(details, "failed");
    json_set_bool(st, "partial", running);
    json_set_bool(st, "parked", running);
    json_set_bool(st, "isError", failed);
    v->is_error = failed;
    if (!running) hmap_del(&g_bg, id);
    v->dirty = true;
    if (u && u->tui) tui_request_render(u->tui);
}

/* ---------- rendering ---------- */

static void fill_call(tc_call_t *c, const char *name, const json_t *args, const json_t *result, bool done, bool partial,
                      bool is_error) {
    memset(c, 0, sizeof *c);
    c->name = name;
    c->label = tool_label(name);
    /* tool-call-block.ts: the name-keyed renderers only draw a tool the session holds */
    c->unregistered = name && g_ui && g_ui->app && g_ui->app->agent && !agent_find_tool(g_ui->app->agent, name);
    c->label_of = tool_label;
    c->args = args;
    c->content = result ? json_get_arr(result, "content") : NULL;
    c->details = result ? json_get_obj(result, "details") : NULL;
    c->has_result = result != NULL;
    c->partial = !done || partial;
    c->is_error = is_error;
    c->expanded = g_ui && g_ui->expand_tools;
    c->spinner = -1;
    c->elapsed_ms = -1;
    c->cwd = project_dir();
    c->term_rows = term_rows();
    c->show_images = ui_inline_images();
}

void render_tool(msgview_t *v, int width, strvec_t *out) {
    json_t *st = state_of(v);
    json_t *grp = json_get_arr(st, "group");
    if (grp) {
        size_t n = json_len(grp);
        tc_call_t *calls = xcalloc(n ? n : 1, sizeof(tc_call_t));
        for (size_t i = 0; i < n; i++) {
            json_t *e = json_at(grp, i);
            fill_call(&calls[i], "read", json_get(e, "args"), json_get_obj(e, "result"), json_get_bool(e, "done", false), false,
                      json_get_bool(e, "isError", false));
        }
        tc_render_read_group(calls, n, width, out);
        free(calls);
        return;
    }
    if (v->kind == MV_BASH) {
        tc_bang_t b = {.command = json_get_str(v->args, "command"), .output = v->output, .running = !v->done,
                       .exclude = json_get_bool(v->args, "exclude", false), .exit_code = v->is_error ? 1 : 0,
                       .expanded = g_ui && g_ui->expand_tools, .spinner = spinner_now(), .term_rows = term_rows()};
        if (json_get(v->args, "exitCode")) b.exit_code = (int)json_get_int(v->args, "exitCode", 0);
        b.cancelled = json_get_bool(v->args, "cancelled", false);
        b.python = json_get_bool(v->args, "python", false);
        if (b.python) b.command = json_get_str(v->args, "code") ? json_get_str(v->args, "code") : b.command;
        tc_render_bang(&b, width, out);
        return;
    }
    json_t *result = json_get_obj(st, "result");
    if (!result && v->done) {
        /* a view finished without toolcard_result (history replay): its text output stands in for the result */
        result = json_obj();
        json_t *content = json_arr(), *blk = json_obj();
        json_set_str(blk, "type", "text");
        json_set_str(blk, "text", v->output ? v->output : "");
        json_push(content, blk);
        json_set(result, "content", content);
        json_set(st, "result", result);
        json_set_bool(st, "partial", false);
    }
    tc_call_t c;
    fill_call(&c, v->tool_name, v->args, result, v->done, json_get_bool(st, "partial", !v->done), v->done && v->is_error);
    if (!v->done) {
        int64_t start = json_get_int(st, "startMs", 0);
        if (start) {
            c.exec_started = true;
            c.elapsed_ms = mono_ms() - start;
        }
        /* only the generic fallback consumes the spinner frame while running (omp animatedPartialResult) */
        if (start && tc_wants_tick(v->tool_name) && strcmp(v->tool_name, "bash")) c.spinner = spinner_now();
        if (stream_spinning(v)) c.spinner = spinner_now();
        c.args_complete = !stream_spinning(v);
    } else c.args_complete = true;
    tc_render(&c, width, out);
    if (c.show_images && result) { /* result images below the card: a spacer and the image per block */
        const json_t *blk;
        size_t k = 0;
        json_arr_foreach(json_get_arr(result, "content"), i, blk) {
            if (!str_eq(json_get_str(blk, "type"), "image") || !json_get_str(blk, "data") || !json_get_str(blk, "mimeType"))
                continue;
            char *key = xasprintf("te%s:%zu", v->tool_id ? v->tool_id : "", k++);
            strvec_push(out, "");
            ui_image_rows(json_get_str(blk, "data"), json_get_str(blk, "mimeType"), key, width, out);
            free(key);
        }
    }
}

static size_t g_visible_blocks;

void toolcard_note_message(ui_t *u, const json_t *msg, bool final) {
    size_t n = 0;
    json_t *blk;
    json_arr_foreach(json_get_arr(msg, "content"), i, blk) {
        const char *t = json_get_str(blk, "type");
        const char *s = str_eq(t, "text") ? json_get_str(blk, "text") : str_eq(t, "thinking") ? json_get_str(blk, "thinking") : NULL;
        if (s && strspn(s, " \t\r\n") != strlen(s)) n++;
    }
    if (n > g_visible_blocks) toolcard_break_group(u);
    g_visible_blocks = final ? 0 : n;
}

static void replay_msg(ui_t *u, const json_t *msg) {
    const char *role = json_get_str(msg, "role");
    if (str_eq(role, "user")) {
        toolcard_break_group(u);
        return;
    }
    if (str_eq(role, "assistant")) {
        toolcard_note_message(u, msg, true);
        json_t *blk;
        json_arr_foreach(json_get_arr(msg, "content"), i, blk) {
            if (!str_eq(json_get_str(blk, "type"), "toolCall")) continue;
            const char *id = json_get_str(blk, "id"), *name = json_get_str(blk, "name");
            if (!id || !name) continue;
            json_t *args = json_get(blk, "arguments");
            msgview_t *v = toolcard_start(u, id, name, args ? json_clone(args) : json_obj());
            hmap_put(&u->tools, id, v);
        }
        return;
    }
    if (str_eq(role, "toolResult")) {
        const char *id = json_get_str(msg, "toolCallId");
        msgview_t *v = id ? hmap_get(&u->tools, id) : NULL;
        if (!v) return;
        json_t *r = json_obj();
        json_t *content = json_get(msg, "content");
        json_set(r, "content", content ? json_clone(content) : json_arr());
        json_t *details = json_get(msg, "details");
        if (details) json_set(r, "details", json_clone(details));
        toolcard_result(u, v, id, r, false, json_get_bool(msg, "isError", false));
        hmap_del(&u->tools, id);
    }
}

void toolcard_replay(ui_t *u, const json_t *msg) {
    bool was = g_replaying;
    g_replaying = true;
    replay_msg(u, msg);
    g_replaying = was;
}

void toolcard_set_replaying(bool on) { g_replaying = on; }

static hmap_t g_run_cards, g_prior_cards; /* call id -> card (borrowed; checked against the transcript before use) */

void toolcard_run_track(const char *id, msgview_t *v) {
    if (id && *id && v) hmap_put(&g_run_cards, id, v);
}

void toolcard_run_end(void) {
    strvec_free(&g_run_streamed_reads);
    hmap_free(&g_prior_cards, NULL);
    g_prior_cards = g_run_cards;
    hmap_init(&g_run_cards);
}

void toolcard_prior_reset(ui_t *u) {
    UNUSED(u);
    strvec_free(&g_run_streamed_reads);
    hmap_free(&g_prior_cards, NULL);
    hmap_init(&g_prior_cards);
    hmap_free(&g_run_cards, NULL);
    hmap_init(&g_run_cards);
}

void toolcard_prior_evict(ui_t *u, const char *id) {
    if (!id || !*id || hmap_get(&g_preview, id) || hmap_get(&g_run_cards, id) || strvec_contains(&g_run_streamed_reads, id))
        return;
    msgview_t *v = hmap_del(&g_prior_cards, id);
    /* never evict a shared read-group card: it may still carry sibling reads */
    if (!v || !view_attached(v) || json_get_arr(state_of(v), "group") || v == g_read_group) return;
    for (size_t j = 0; j < g_live.n; j++)
        if (g_live.v[j] == v) {
            g_live.v[j] = g_live.v[--g_live.n];
            break;
        }
    if (hmap_get(&g_bg, id) == v) hmap_del(&g_bg, id);
    tui_container_remove(u->chat, &v->base);
    tui_component_destroy(&v->base);
}

/* drop every key of `m` that maps to `v` (keys collected first: deleting while probing would skip entries) */
static bool forget_in(hmap_t *m, const msgview_t *v) {
    strvec_t keys = {0};
    hmap_foreach(m, it) if (it->val == v) strvec_push(&keys, it->key);
    for (size_t i = 0; i < keys.n; i++) hmap_del(m, keys.v[i]);
    bool found = keys.n > 0;
    strvec_free(&keys);
    return found;
}

/* The registries above, the transcript's call-id map and the run's synthetic/current/pinned views borrow their
 * cards. A card that leaves the transcript any way (a rebuild after compaction, a focus switch, a superseded retry,
 * an evicted prior card) is dropped from all of them before its memory goes, so the repaint ticker, previews,
 * background frames and later tool events never reach a freed card. */
void toolcard_forget(msgview_t *v) {
    for (size_t i = 0; i < g_live.n; i++)
        if (g_live.v[i] == v) {
            g_live.v[i] = g_live.v[--g_live.n];
            break;
        }
    if (g_read_group == v) g_read_group = NULL;
    forget_in(&g_preview, v);
    forget_in(&g_bg, v);
    forget_in(&g_run_cards, v);
    forget_in(&g_prior_cards, v);
    ui_t *u = g_ui;
    if (!u) return;
    forget_in(&u->tools, v);
    for (size_t i = 0; i < u->synthetic_cards.n; i++)
        if (u->synthetic_cards.v[i] == v) {
            u->synthetic_cards.v[i] = u->synthetic_cards.v[--u->synthetic_cards.n];
            i--;
        }
    if (u->cur_assistant == v) u->cur_assistant = NULL;
    if (u->pinned_error == v) u->pinned_error = NULL;
}

bool toolcard_tracked(const msgview_t *v) {
    for (size_t i = 0; i < g_live.n; i++)
        if (g_live.v[i] == v) return true;
    const hmap_t *maps[] = {&g_preview, &g_bg, &g_run_cards, &g_prior_cards, g_ui ? &g_ui->tools : NULL};
    for (size_t k = 0; k < ARRAY_LEN(maps); k++)
        if (maps[k]) hmap_foreach(maps[k], it) if (it->val == v) return true;
    if (g_ui)
        for (size_t i = 0; i < g_ui->synthetic_cards.n; i++)
            if (g_ui->synthetic_cards.v[i] == v) return true;
    return v == g_read_group || (g_ui && (v == g_ui->cur_assistant || v == g_ui->pinned_error));
}

void toolcard_frames_reset(void) {
    json_free(g_frames);
    g_frames = NULL;
}
