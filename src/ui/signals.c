/* signals.c - terminal title run state, OSC 7501 program status and turn notifications (see signals.h). */
#include "session/session.h"
#include "ui/overlays.h"
#include "tui/motion.h"
#include "ui/signals.h"
#include "text/htmlform.h"

#include "config/settings.h"
#include "tui/ansi.h"
#include "tui/notify.h"
#include "ui/ui.h"

#include <ctype.h>

/* Plain π, not the nerd-font glyph: window/tab titles render in the OS UI font */
#define DEFAULT_TITLE "\xcf\x80"
#define MSG_MAX_BYTES 2048

static const char *const BRAILLE[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
static const char *const PULSE[] = {"○", "◔", "◑", "◕", "●", "◕", "◑", "◔"};
static const char *const DOTS[] = {"⠁", "⠂", "⠄", "⠠", "⠐", "⠈"};
static const char *const LINE[] = {"-", "\\", "|", "/"};

size_t ui_title_spinner_frames(const char *style, const char *const **frames) {
    if (str_eq(style, "pulse")) return *frames = PULSE, ARRAY_LEN(PULSE);
    if (str_eq(style, "dots")) return *frames = DOTS, ARRAY_LEN(DOTS);
    if (str_eq(style, "line")) return *frames = LINE, ARRAY_LEN(LINE);
    return *frames = BRAILLE, ARRAY_LEN(BRAILLE);
}

static const char *norm_style(const char *s) {
    return str_eq(s, "pulse") || str_eq(s, "dots") || str_eq(s, "line") ? s : "braille";
}

/* JS String.prototype.trim whitespace (ASCII + the common Unicode spaces) */
static size_t ws_len(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c == ' ' || (c >= 0x09 && c <= 0x0d)) return 1;
    if (c == 0xc2 && (unsigned char)p[1] == 0xa0) return 2;
    if (c == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) return 3;
    if (c == 0xe2 && (unsigned char)p[1] == 0x80 &&
        (((unsigned char)p[2] >= 0x80 && (unsigned char)p[2] <= 0x8a) || (unsigned char)p[2] == 0xa8 ||
         (unsigned char)p[2] == 0xa9 || (unsigned char)p[2] == 0xaf))
        return 3;
    if (c == 0xe3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x80) return 3;
    return 0;
}

static char *js_trim(const char *s) {
    while (*s) {
        size_t n = ws_len(s);
        if (!n) break;
        s += n;
    }
    size_t end = 0, i = 0, len = strlen(s);
    while (i < len) {
        size_t n = ws_len(s + i);
        if (n) i += n;
        else {
            int k = utf8_decode(s + i, len - i, &(uint32_t){0});
            i += k;
            end = i;
        }
    }
    return xstrndup(s, end);
}

/* sanitizeTerminalTitlePart: drop U+0000–U+001F and U+007F–U+009F, trim; NULL when nothing remains */
char *ui_title_sanitize(const char *s) {
    if (!s) return NULL;
    sb_t b = SB_INIT;
    size_t len = strlen(s);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int k = utf8_decode(s + i, len - i, &cp);
        if (!(cp < 0x20 || (cp >= 0x7f && cp <= 0x9f))) sb_appendn(&b, s + i, (size_t)k);
        i += (size_t)k;
    }
    char *raw = sb_detach(&b);
    char *t = js_trim(raw);
    free(raw);
    if (!*t) {
        free(t);
        return NULL;
    }
    return t;
}

/* buildTerminalTitleWithState */
char *ui_title_compose(const char *label, run_state_t state, int frame, bool enabled, const char *style, bool static_host) {
    if (!enabled) return label ? xasprintf(DEFAULT_TITLE ": %s", label) : xstrdup(DEFAULT_TITLE);
    const char *const *frames;
    size_t n = ui_title_spinner_frames(style, &frames);
    const char *sep = state == RUN_WORKING ? (static_host ? ":" : frames[(size_t)frame % n])
                      : state == RUN_BLOCKED ? "!"
                                             : ">";
    return label ? xasprintf(DEFAULT_TITLE " %s %s", sep, label) : xasprintf(DEFAULT_TITLE " %s", sep);
}

/* sanitizeText (control characters but \t \n, ANSI sequences) + whitespace runs collapsed + trim, capped at the
 * protocol's decoded `msg` limit (truncateLineBytes: head + "…"), base64; NULL when nothing printable remains */
static char *encode_msg(const char *text) {
    if (!text || !*text) return NULL;
    char *plain = strchr(text, '\x1b') ? tui_strip_ansi(text) : xstrdup(text);
    sb_t b = SB_INIT;
    bool space = false;
    size_t len = strlen(plain);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int k = utf8_decode(plain + i, len - i, &cp);
        bool ctrl = (cp <= 0x08) || (cp >= 0x0b && cp <= 0x1f) || (cp >= 0x7f && cp <= 0x9f);
        size_t w = ws_len(plain + i);
        if (ctrl) {
            i += (size_t)k;
            continue;
        }
        if (w) {
            space = true;
            i += w;
            continue;
        }
        if (space && b.len) sb_putc(&b, ' ');
        space = false;
        sb_appendn(&b, plain + i, (size_t)k);
        i += (size_t)k;
    }
    free(plain);
    char *line = sb_detach(&b);
    if (!*line) {
        free(line);
        return NULL;
    }
    if (strlen(line) > MSG_MAX_BYTES) {
        size_t cut = MSG_MAX_BYTES - 3;
        while (cut > 0 && ((unsigned char)line[cut] & 0xc0) == 0x80) cut--;
        char *t = xasprintf("%.*s…", (int)cut, line);
        free(line);
        line = t;
    }
    char *r = base64_encode(line, strlen(line));
    free(line);
    return r;
}

static const char *state_name(run_state_t s) {
    switch (s) {
    case RUN_WORKING: return "working";
    case RUN_BLOCKED: return "blocked";
    case RUN_DONE: return "done";
    case RUN_ERROR: return "error";
    default: return "idle";
    }
}

/* run-status.ts formatReport */
char *ui_program_status_report(run_state_t state, const char *kind, const char *msg) {
    sb_t b = SB_INIT;
    sb_appendf(&b, "\x1b]7501;state=%s", state_name(state));
    if (state == RUN_BLOCKED) sb_appendf(&b, ":kind=%s", kind ? kind : "question");
    sb_append(&b, ":app=" TUI_NOTIFY_APP_NAME);
    char *m = state == RUN_BLOCKED || state == RUN_ERROR ? encode_msg(msg) : NULL;
    if (m) sb_appendf(&b, ":msg=%s", m);
    free(m);
    sb_append(&b, "\x1b\\");
    return sb_detach(&b);
}

/* ---------- runtime ---------- */

static char *g_warp_prompt; /* Warp bridge activePrompt */

/* holdBlockedStatus: a dialog waiting on the user */
typedef struct {
    int id;
    char *msg;
    const char *kind; /* "question" | "permission" */
} hold_t;

static struct {
    ui_t *u;
    bool claimed, disposed;
    /* title */
    char *label;
    run_state_t title_state;
    int frame;
    int timer;
    int interval; /* the title spinner's period (setTerminalTitleSpinnerInterval) */
    char *last_title;
    const char *style;
    /* program status: the event flow's status (setRunStatus), shown unless a dialog holds `blocked` */
    run_state_t status;
    char *kind, *msg;
    char *reported;
    VEC(hold_t) holds; /* open dialogs, newest last */
    int next_hold;
    json_t *ended; /* the last agent_end's messages, not yet settled */
    bool startup_run;  /* set before init: a command-line prompt runs once startup ends */
    bool startup_hold; /* the boot `working` that prompt holds until its run reports (run-status.ts startup) */
} S = {.style = "braille"};

static tui_term_t *term(void) {
    if (!S.u || !S.u->tui) return NULL;
    tui_term_t *t = tui_term(S.u->tui);
    return t && t->is_tty ? t : NULL;
}

static void write_seq(const char *seq) {
    tui_term_t *t = term();
    if (!t) return;
    tui_term_puts(t, seq);
    tui_term_flush(t);
}

static bool title_state_enabled(void) { return settings_bool("tui.titleState", true); }
static bool program_status_enabled(void) { return settings_bool("terminal.programStatus", true); }
static bool static_host(void) {
#ifdef __linux__
    return env_get("WSL_DISTRO_NAME") || env_get("WSL_INTEROP");
#else
    return false;
#endif
}

/* writeTerminalTitle: the sink every title write funnels through (teardown latch, TTY gate, dedupe) */
static void write_title(const char *title) {
    if (S.disposed || !S.claimed || !term()) return;
    char *next = ui_title_sanitize(title);
    if (!next) next = xstrdup(DEFAULT_TITLE);
    if (S.last_title && !strcmp(next, S.last_title)) {
        free(next);
        return;
    }
    char *seq = xasprintf("\x1b]0;%s\x07", next);
    write_seq(seq);
    free(seq);
    free(S.last_title);
    S.last_title = next;
}

static void emit_title(void) {
    /* tui.spinnerInterval 0 pins the working separator too */
    char *t = ui_title_compose(S.label, S.title_state, S.frame, title_state_enabled(), S.style, static_host() || !tui_spinner_animated());
    write_title(t);
    free(t);
}

static void spinner_tick(void *ud) {
    (void)ud;
    const char *const *frames;
    size_t n = ui_title_spinner_frames(S.style, &frames);
    S.frame = (int)((size_t)(S.frame + 1) % n);
    emit_title();
}

static void stop_spinner(void) {
    if (S.timer && S.u) tui_remove_timer(S.u->tui, S.timer);
    S.timer = 0;
}

static void start_spinner(void) {
    if (static_host() || S.disposed || S.timer || !term() || !tui_spinner_animated()) return;
    S.interval = tui_spinner_interval();
    S.timer = tui_add_timer(S.u->tui, S.interval, true, spinner_tick, NULL);
}

/* setTerminalTitleState */
static void set_title_state(run_state_t st) {
    S.title_state = st;
    if (st == RUN_WORKING && title_state_enabled()) start_spinner();
    else stop_spinner();
    emit_title();
}

/* shownStatus: the newest open dialog, else the event flow's status */
static run_state_t shown_state(const char **kind, const char **msg) {
    if (S.holds.n) {
        *kind = S.holds.v[S.holds.n - 1].kind;
        *msg = S.holds.v[S.holds.n - 1].msg;
        return RUN_BLOCKED;
    }
    *kind = S.kind;
    *msg = S.msg;
    return S.status;
}

/* run-status.ts report / clear */
static void report(void) {
    if (S.disposed || !program_status_enabled() || !term()) return;
    const char *kind, *msg;
    run_state_t st = shown_state(&kind, &msg);
    char *next = ui_program_status_report(st, kind, msg);
    if (S.reported && !strcmp(next, S.reported)) {
        free(next);
        return;
    }
    write_seq(next);
    free(S.reported);
    S.reported = next;
}

static void clear_report(void) {
    if (!S.reported) return;
    free(S.reported);
    S.reported = NULL;
    write_seq("\x1b]7501;state=clear\x1b\\");
}

/* publish: the title separator and the OSC 7501 record follow the shown status */
static void publish(void) {
    const char *kind, *msg;
    run_state_t st = shown_state(&kind, &msg);
    set_title_state(st == RUN_WORKING ? RUN_WORKING : st == RUN_BLOCKED ? RUN_BLOCKED : RUN_IDLE);
    report();
}

void ui_signals_run_status(run_state_t state, const char *kind, const char *msg) {
    S.startup_hold = false;
    S.status = state;
    free(S.kind);
    free(S.msg);
    S.kind = kind ? xstrdup(kind) : NULL;
    S.msg = msg ? xstrdup(msg) : NULL;
    publish();
}

int ui_signals_hold_blocked(const char *kind, const char *msg) {
    hold_t h = {++S.next_hold, msg ? xstrdup(msg) : NULL, str_eq(kind, "permission") ? "permission" : "question"};
    vec_push(&S.holds, h);
    publish();
    return h.id;
}

void ui_signals_release_blocked(int id) {
    for (size_t i = 0; i < S.holds.n; i++) {
        if (S.holds.v[i].id != id) continue;
        free(S.holds.v[i].msg);
        memmove(&S.holds.v[i], &S.holds.v[i + 1], (S.holds.n - i - 1) * sizeof S.holds.v[0]);
        S.holds.n--;
        publish();
        return;
    }
}

static void drop_holds(void) {
    for (size_t i = 0; i < S.holds.n; i++) free(S.holds.v[i].msg);
    S.holds.n = 0;
}

/* getFallbackTerminalTitle: the resolved cwd's basename (none for the root) */
static char *cwd_label(const char *cwd) {
    if (!cwd || !*cwd) return NULL;
    char *r = realpath(cwd, NULL);
    char *p = r ? r : xstrdup(cwd);
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    char *out = *base ? ui_title_sanitize(base) : NULL;
    free(p);
    return out;
}

void ui_signals_session_title(void) {
    if (!S.u) return;
    app_t *a = S.u->app;
    char *name = ui_title_sanitize(ui_session_title(S.u));
    free(S.label);
    S.label = name ? name : cwd_label(a ? a->cwd : NULL);
    emit_title();
}

/* resendProgramStatus: `done` and `error` survive a shell prompt, so only the others are reported again */
void ui_signals_resumed(void) {
    const char *kind, *msg;
    run_state_t st = shown_state(&kind, &msg);
    if (st == RUN_DONE || st == RUN_ERROR) return;
    free(S.reported);
    S.reported = NULL;
    report();
}

void ui_signals_settings_changed(void) {
    if (!S.u || S.disposed) return;
    const char *style = norm_style(settings_str("tui.titleSpinner", "braille"));
    bool restyled = strcmp(style, S.style) != 0;
    if (restyled) {
        S.style = style;
        S.frame = 0;
        stop_spinner();
    }
    if (S.title_state == RUN_WORKING && title_state_enabled()) start_spinner();
    else stop_spinner();
    emit_title();
    if (program_status_enabled()) report();
    else clear_report();
}

bool ui_signals_blocked(void) { return S.holds.n > 0; }

static void settings_changed_cb(void *ud) {
    (void)ud;
    ui_apply_motion();
    /* setTerminalTitleSpinnerInterval: re-arm a live spinner so the new cadence applies immediately */
    if (tui_spinner_interval() != S.interval) {
        S.interval = tui_spinner_interval();
        S.frame = 0;
        if (S.timer && !tui_spinner_animated()) stop_spinner();
        else if (S.timer) {
            stop_spinner();
            start_spinner();
        } else if (S.title_state == RUN_WORKING && title_state_enabled()) start_spinner();
        emit_title();
    }
    ui_signals_settings_changed();
    ui_recap_refresh();
}

/* settings listener: may run on any thread, hop to the UI thread */
static void on_settings_change(void *ud) {
    ui_t *u = ud;
    if (u && u->tui) tui_post(u->tui, settings_changed_cb, NULL);
}

void ui_signals_init(ui_t *u) {
    S.u = u;
    settings_on_change(on_settings_change, u);
    if (term()) write_seq("\x1b[22;2t"); /* pushTerminalTitle */
    S.claimed = true;
    S.disposed = false;
    S.style = norm_style(settings_str("tui.titleSpinner", "braille"));
    S.interval = tui_spinner_interval();
    /* setTerminalTitleStateEnabled: emits before the session label is known (`π >`) */
    if (S.title_state == RUN_WORKING && title_state_enabled()) start_spinner();
    emit_title();
    /* initProgramStatus({startupRun}): the first report is `working`, not the `idle` a supervisor waiting on
     * this terminal would take as finished before the prompt even starts; the title turns to the spinner right
     * after its idle first write (omp: `π >`, `π *`, then the report) */
    drop_holds(); /* a UI starts with no dialog open */
    S.startup_hold = S.startup_run;
    S.startup_run = false;
    if (S.startup_hold) {
        S.status = S.title_state = RUN_WORKING;
        if (title_state_enabled()) start_spinner();
        emit_title();
    }
    report(); /* setProgramStatusEnabled */
    ui_signals_session_title();
    ui_recap_init(u);
    ui_signals_session_start();
}

void ui_signals_dispose(void) {
    if (!S.u || S.disposed) return;
    settings_off_change(on_settings_change, S.u);
    ui_recap_dispose();
    stop_spinner();
    S.disposed = true;
    free(S.last_title);
    S.last_title = NULL;
    clear_report();
    if (term()) write_seq("\x1b]0;\x07\x1b[23;2t"); /* popTerminalTitle */
    free(S.label);
    free(S.kind);
    free(S.msg);
    S.label = S.kind = S.msg = NULL;
    drop_holds();
    free(S.holds.v);
    memset(&S.holds, 0, sizeof S.holds);
    json_free(S.ended);
    S.ended = NULL;
    S.u = NULL; /* its ui lives on run_interactive_mode's stack; omc_main can run again in this process */
    free(g_warp_prompt);
    g_warp_prompt = NULL;
}

/* ---------- Warp CLI-agent events (modes/warp-events.ts) ---------- */

/* Warp negotiated its structured CLI-agent OSC protocol; it then drives native completion UX itself */
static bool warp_protocol_active(void) {
    const char *v = env_get("WARP_CLI_AGENT_PROTOCOL_VERSION");
    return v && atof(v) >= 1;
}


/* truncateEventText: the first 200 code points */
static char *event_text(const char *s) {
    size_t i = 0, n = 0, len = strlen(s ? s : "");
    while (i < len && n < 200) {
        uint32_t cp;
        i += (size_t)utf8_decode(s + i, len - i, &cp);
        n++;
    }
    return xstrndup(s ? s : "", i);
}

/* `\x1b]777;notify;warp://cli-agent;<json>\x07`, the envelope after the event's own fields; DCS-wrapped under
 * tmux, with an outer BEL for the attention-worthy events */
static void warp_emit(json_t *ev) {
    if (!warp_protocol_active() || !S.u || S.disposed) {
        json_free(ev);
        return;
    }
    app_t *a = S.u->app;
    const char *cwd = a && a->cwd ? a->cwd : "";
    json_set_int(ev, "v", 1);
    json_set_str(ev, "agent", "omp"); /* Warp resolves the agent through CLIAgent.command_prefix() */
    json_set_str(ev, "session_id", a && a->session ? session_id(a->session) : "");
    json_set_str(ev, "cwd", cwd);
    const char *base = strrchr(cwd, '/');
    json_set_str(ev, "project", base ? base + 1 : cwd);
    json_set_str(ev, "plugin_version", OMC_VERSION);
    char *body = json_dump(ev, 0);
    char *osc = xasprintf("\x1b]777;notify;warp://cli-agent;%s\x07", body);
    if (!tui_inside_tmux()) write_seq(osc);
    else {
        const char *name = json_get_str(ev, "event");
        bool ring = str_eq(name, "stop") || str_eq(name, "stop_failure") || str_eq(name, "permission_request") ||
                    str_eq(name, "question_asked");
        char *w = tui_wrap_tmux_passthrough(osc);
        if (ring) {
            char *r = xasprintf("%s\x07", w);
            free(w);
            w = r;
        }
        write_seq(w);
        free(w);
    }
    free(osc);
    free(body);
    json_free(ev);
}

static json_t *warp_event(const char *name) {
    json_t *e = json_obj();
    json_set_str(e, "event", name);
    return e;
}

void ui_signals_session_start(void) {
    session_pins_sync(); /* #syncSessionPinned: at startup and on session switch, picking up other instances' pins */
    free(g_warp_prompt);
    g_warp_prompt = NULL;
    warp_emit(warp_event("session_start"));
}

void ui_signals_user_message(const json_t *msg) {
    if (!str_eq(json_get_str(msg, "role"), "user") || json_get_bool(msg, "synthetic", false) ||
        str_eq(json_get_str(msg, "attribution"), "agent"))
        return;
    const json_t *c = json_get(msg, "content");
    sb_t b = SB_INIT;
    if (json_is_str(c)) sb_append(&b, json_sv(c));
    json_t *blk;
    json_arr_foreach(json_is_arr(c) ? c : NULL, i, blk) if (str_eq(json_get_str(blk, "type"), "text")) sb_append(&b, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
    free(g_warp_prompt);
    g_warp_prompt = event_text(sb_cstr(&b));
    sb_free(&b);
    json_t *e = warp_event("prompt_submit");
    json_set_str(e, "query", g_warp_prompt);
    warp_emit(e);
}

static bool silent_abort(const json_t *m) { return str_eq(json_get_str(m, "errorMessage"), "__omp.silent_abort__"); }
static bool user_interrupt(const json_t *m) { /* isInterruptReason */
    return str_eq(json_get_str(m, "errorMessage"), "Interrupted by user") || str_eq(json_get_str(m, "errorMessage"), "Stopped by parent agent");
}

/* lastAssistantStop */
static void warp_stop(const json_t *msgs) {
    json_t *e = NULL;
    for (size_t i = json_len(msgs); i-- > 0 && !e;) {
        const json_t *m = json_at(msgs, i);
        if (!str_eq(json_get_str(m, "role"), "assistant")) continue;
        sb_t t = SB_INIT;
        json_t *blk;
        json_arr_foreach(json_get_arr(m, "content"), k, blk) if (str_eq(json_get_str(blk, "type"), "text")) sb_append(&t, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
        const char *stop = json_get_str(m, "stopReason"), *err = json_get_str(m, "errorMessage");
        char *response = t.len ? xstrdup(sb_cstr(&t)) : NULL;
        sb_free(&t);
        if (!response && err && *err && !silent_abort(m) &&
            (str_eq(stop, "error") || (str_eq(stop, "aborted") && !user_interrupt(m))))
            response = xstrdup(err);
        const char *ev = "stop", *etype = NULL;
        if (str_eq(stop, "error")) ev = "stop_failure", etype = "error";
        else if (str_eq(stop, "aborted") && !silent_abort(m) && !user_interrupt(m) && err && *err)
            ev = "stop_failure", etype = "aborted";
        e = warp_event(ev);
        if (g_warp_prompt) json_set_str(e, "query", g_warp_prompt);
        char *r = event_text(response ? response : "");
        json_set_str(e, "response", r);
        free(r);
        free(response);
        if (etype) json_set_str(e, "error_type", etype);
    }
    if (!e) {
        e = warp_event("stop");
        if (g_warp_prompt) json_set_str(e, "query", g_warp_prompt);
        json_set_str(e, "response", "");
    }
    warp_emit(e);
}

/* ---------- agent events ---------- */

void ui_signals_agent_start(void) {
    ui_recap_invalidate();
    json_free(S.ended);
    S.ended = NULL;
    ui_signals_run_status(RUN_WORKING, NULL, NULL);
}

void ui_signals_tool_start(const char *tool_call_id, const char *tool_name, const json_t *args) {
    if (!str_eq(tool_name, "ask") || !tool_call_id) return;
    /* AskTool.execute sends its notification before the UI handles the call's tool_execution_start */
    ui_signals_ask_waiting();
    json_t *qa = warp_event("question_asked");
    json_set_str(qa, "summary", "Waiting for your answer");
    warp_emit(qa);
}

void ui_signals_approval_requested(const char *tool_call_id, const char *tool_name, const char *msg) {
    json_t *e = warp_event("permission_request");
    json_set_str(e, "tool_name", tool_name ? tool_name : "");
    char *summary = xasprintf("omp wants to run %s", tool_name ? tool_name : "");
    json_set_str(e, "summary", summary);
    free(summary);
    warp_emit(e);
}

void ui_signals_approval_resolved(void) { warp_emit(warp_event("permission_replied")); }

void ui_signals_tool_end(const char *tool_call_id, const char *tool_name) {
    json_t *tc = warp_event("tool_complete");
    json_set_str(tc, "tool_name", tool_name ? tool_name : "");
    warp_emit(tc);
}

static const json_t *sig_last_assistant(const json_t *msgs) {
    for (size_t i = json_len(msgs); i-- > 0;) {
        const json_t *m = json_at(msgs, i);
        if (str_eq(json_get_str(m, "role"), "assistant")) return m;
    }
    return NULL;
}


static void notify(const char *title, const char *body, const char *type, const char *urgency) {
    tui_term_t *t = S.u && S.u->tui ? tui_term(S.u->tui) : NULL;
    tui_notification_t n = {title, body, type, urgency};
    tui_notify_send(t, &n);
}

static const char *session_title(void) {
    const char *t = ui_session_title(S.u);
    return t && *t ? t : TUI_NOTIFY_APP_NAME;
}

void ui_signals_run_settled(const json_t *run_messages, bool interrupted) {
    /* #settledRunStatus (a dialog still open shows over it) */
    const json_t *last = sig_last_assistant(run_messages);
    const char *stop = last ? json_get_str(last, "stopReason") : NULL;
    if (interrupted) ui_signals_run_status(RUN_IDLE, NULL, NULL);
    else if (str_eq(stop, "error")) ui_signals_run_status(RUN_ERROR, NULL, json_get_str(last, "errorMessage"));
    else if (!last || str_eq(stop, "aborted")) ui_signals_run_status(RUN_IDLE, NULL, NULL);
    else ui_signals_run_status(RUN_DONE, NULL, NULL);
    if (interrupted) return;
    /* sendErrorNotification */
    if (!warp_protocol_active() && str_eq(settings_str("error.notify", "off"), "on") && str_eq(stop, "error"))
        notify(session_title(), "Stopped with error", "error", NULL);
    /* sendCompletionNotification */
    if (!str_eq(settings_str("completion.notify", "on"), "off") && !warp_protocol_active() && !str_eq(stop, "aborted") &&
        !str_eq(stop, "error"))
        notify(session_title(), "Complete", "completion", NULL);
}

void ui_signals_agent_end(const json_t *run_messages) {
    json_free(S.ended);
    S.ended = run_messages ? json_clone(run_messages) : json_arr();
}

void ui_signals_set_startup_run(bool on) { S.startup_run = on; }

void ui_signals_settle_startup(void) {
    if (!S.startup_hold) return;
    S.startup_hold = false;
    ui_signals_run_status(RUN_IDLE, NULL, NULL);
}

bool ui_signals_settle(void) {
    if (!S.ended) return false;
    json_t *m = S.ended;
    S.ended = NULL;
    const json_t *last = sig_last_assistant(m);
    const char *stop = last ? json_get_str(last, "stopReason") : NULL;
    bool done = last && !str_eq(stop, "error") && !str_eq(stop, "aborted");
    ui_signals_run_settled(m, false);
    warp_stop(m);
    ui_recap_settled(m);
    json_free(m);
    return done;
}

void ui_signals_ask_waiting(void) {
    if (str_eq(settings_str("ask.notify", "on"), "off")) return;
    notify(TUI_NOTIFY_APP_NAME, "Waiting for input", "ask", "normal");
}
