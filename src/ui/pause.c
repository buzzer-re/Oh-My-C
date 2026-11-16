/* pause.c - the fullscreen `/pause` screen (port of omp packages/tui/src/overlays/pause-screen.ts, ANSI rendering,
 * v18.14.2). `/pause` engages the process-wide agent pause gate (agent.c agent_pause_gate_*): every agent loop
 * (main, subagents, advisor) parks at its next model call or tool start; nothing is aborted. While engaged the
 * screen owns the alternate screen with a large pause glyph and a live hold clock; the interrupt key, Enter,
 * Space or Ctrl+C release the gate. */
#include "ui/ui.h"

#include <ctype.h>

#define PAUSE_TICK_MS 1000
#define BAR_ROWS 7
#define BAR_WIDTH 5
#define BAR_GAP 4
#define MIN_FULL_WIDTH 64
#define MIN_FULL_HEIGHT 23

static const char *const TITLE = "P A U S E D";
static const char *const BODY_LINES[] = {
    "Main agent, subagents, and advisor hold at their next step.",
    "In-flight calls finish; nothing new starts until you resume.",
};

/* formatClock: 0:07, 12:34, 1:02:03 */
static char *format_clock(int64_t ms) {
    int64_t total = ms > 0 ? ms / 1000 : 0;
    int64_t s = total % 60, m = (total / 60) % 60, h = total / 3600;
    if (h > 0) return xasprintf("%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
    return xasprintf("%lld:%02lld", (long long)m, (long long)s);
}

/* chrome/format.ts formatCoarseDuration */
char *ui_coarse_duration(int64_t ms) {
    double r = (double)ms / 1000.0;
    int64_t seconds = r < 0 ? 0 : (int64_t)(r + 0.5);
    if (seconds < 60) return xasprintf("%llds", (long long)seconds);
    int64_t minutes = (int64_t)((double)seconds / 60.0 + 0.5);
    if (minutes < 60) return xasprintf("%lldm", (long long)minutes);
    int64_t hours = (int64_t)((double)minutes / 60.0 + 0.5);
    if (hours < 48) return xasprintf("%lldh", (long long)hours);
    return xasprintf("%lldd", (long long)(int64_t)((double)hours / 24.0 + 0.5));
}

/* centerLine(...).trimEnd() */
static char *center(const char *styled, int width) {
    char *c = ui_center_line(styled, width);
    size_t n = strlen(c);
    while (n && c[n - 1] == ' ') c[--n] = 0;
    return c;
}

static void push_center(strvec_t *out, int tc, bool bold, const char *text, int width) {
    char *f = tc < 0 ? xstrdup(text) : fg((tui_tc_t)tc, text);
    if (bold) {
        char *b = tui_bold(f);
        free(f);
        f = b;
    }
    strvec_push_own(out, center(f, width));
    free(f);
}

/* the session name with whitespace runs collapsed (replace(/\s+/g, " ").trim()) */
static char *collapse_ws(const char *s) {
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = s; *p; p++) {
        if (isspace((unsigned char)*p)) sp = true;
        else {
            if (sp && b.len) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *p);
        }
    }
    return sb_detach(&b);
}

static char *interrupt_key(void) {
    const strvec_t *keys = tui_keybindings_get(tui_keybindings_global(), "app.interrupt");
    return ui_key_hint(keys && keys->n ? keys->v[0] : "escape");
}

/* renderPauseScreen: exactly `height` rows, vertically centered */
void ui_pause_render(int width, int height, int64_t elapsed_ms, const char *session_name, int turns, int jobs,
                     strvec_t *out) {
    width = MAX(1, width);
    height = MAX(1, height);
    bool compact = width < MIN_FULL_WIDTH || height < MIN_FULL_HEIGHT;
    bool outstanding = turns > 0 || jobs > 0;
    const char *status = outstanding ? "Pausing \xe2\x80\x94 waiting for work to settle" : "Paused \xe2\x80\x94 ready to suspend";
    tui_tc_t status_tc = outstanding ? TUI_TC_WARNING : TUI_TC_SUCCESS;
    char *wc0 = xasprintf("Unsettled turns: %d", turns), *wc1 = xasprintf("Unfinished jobs: %d", jobs);
    char *esc = interrupt_key();
    char *clock = format_clock(elapsed_ms);
    char *name = session_name && *session_name ? collapse_ws(session_name) : NULL;
    strvec_t content = {0};
    if (compact) {
        char *session_line = NULL, *title_line, *clock_line, *status_line, *resume_line;
        if (name) {
            char *b = tui_bold(name);
            session_line = center(b, width);
            free(b);
        }
        {
            char *t = xasprintf("\xe2\x96\x8c\xe2\x96\x8c %s", TITLE);
            char *f = fg(TUI_TC_ACCENT, t), *b = tui_bold(f);
            title_line = center(b, width);
            free(t), free(f), free(b);
        }
        {
            char *t = xasprintf("paused for %s", clock), *f = fg(TUI_TC_DIM, t);
            clock_line = center(f, width);
            free(t), free(f);
        }
        {
            char *f = fg(status_tc, status);
            status_line = center(f, width);
            free(f);
        }
        {
            char *t = xasprintf("%s to resume", esc), *f = fg(TUI_TC_DIM, t);
            resume_line = center(f, width);
            free(t), free(f);
        }
        size_t full = (session_line ? 1 : 0) + 4 + (outstanding ? 2 : 0) + 1;
        if ((size_t)height >= full) {
            if (session_line) strvec_push(&content, session_line);
            strvec_push(&content, title_line);
            strvec_push(&content, "");
            strvec_push(&content, clock_line);
            strvec_push(&content, status_line);
            if (outstanding) {
                push_center(&content, TUI_TC_MUTED, false, wc0, width);
                push_center(&content, TUI_TC_MUTED, false, wc1, width);
            }
            strvec_push(&content, resume_line);
        } else {
            /* keep the live state and a way out on screen before the decorative card details */
            if (height == 1) {
                char *summary = outstanding ? xasprintf("T:%d J:%d \xc2\xb7 %s to resume", turns, jobs, esc)
                                            : xasprintf("Paused \xc2\xb7 %s to resume", esc);
                char *abbr = outstanding ? xasprintf("T%d J%d %s resume", turns, jobs, esc) : xasprintf("%s resume", esc);
                push_center(&content, status_tc, false, tui_visible_width(summary) > width ? abbr : summary, width);
                free(summary), free(abbr);
            } else if (height == 2 && outstanding) {
                char *counts = xasprintf("Pausing \xc2\xb7 T:%d J:%d", turns, jobs);
                push_center(&content, TUI_TC_WARNING, false, counts, width);
                free(counts);
                strvec_push(&content, resume_line);
            } else {
                strvec_push(&content, status_line);
                if (outstanding && height == 3) {
                    char *t = xasprintf("Turns: %d \xc2\xb7 Jobs: %d", turns, jobs);
                    push_center(&content, TUI_TC_MUTED, false, t, width);
                    free(t);
                } else if (outstanding) {
                    push_center(&content, TUI_TC_MUTED, false, wc0, width);
                    push_center(&content, TUI_TC_MUTED, false, wc1, width);
                }
                strvec_push(&content, resume_line);
            }
            if ((int)content.n < height && session_line) strvec_push(&content, session_line);
            if ((int)content.n < height) strvec_push(&content, title_line);
            if ((int)content.n < height) strvec_push(&content, clock_line);
        }
        free(session_line), free(title_line), free(clock_line), free(status_line), free(resume_line);
    } else {
        if (name) {
            push_center(&content, -1, true, name, width);
            strvec_push(&content, "");
        }
        sb_t g = SB_INIT;
        for (int i = 0; i < BAR_WIDTH; i++) sb_append(&g, "\xe2\x96\x88");
        char *bar = sb_detach(&g);
        char *glyph = xasprintf("%s%*s%s", bar, BAR_GAP, "", bar);
        for (int i = 0; i < BAR_ROWS; i++) push_center(&content, TUI_TC_ACCENT, false, glyph, width);
        free(glyph), free(bar);
        strvec_push(&content, "");
        push_center(&content, TUI_TC_ACCENT, true, TITLE, width);
        strvec_push(&content, "");
        for (size_t i = 0; i < ARRAY_LEN(BODY_LINES); i++) push_center(&content, TUI_TC_MUTED, false, BODY_LINES[i], width);
        push_center(&content, status_tc, false, status, width);
        if (outstanding) {
            push_center(&content, TUI_TC_MUTED, false, wc0, width);
            push_center(&content, TUI_TC_MUTED, false, wc1, width);
        }
        char *pf = xasprintf("paused for %s", clock);
        push_center(&content, TUI_TC_DIM, false, pf, width);
        free(pf);
        strvec_push(&content, "");
        char *en = ui_key_hint("enter"), *sp = ui_key_hint("space");
        char *hint = xasprintf("%s \xc2\xb7 %s \xc2\xb7 %s \xe2\x80\x94 resume", esc, en, sp);
        push_center(&content, TUI_TC_DIM, false, hint, width);
        free(hint), free(en), free(sp);
    }
    int top = MAX(0, (height - (int)content.n) / 2);
    for (int i = 0; i < top && (int)out->n < height; i++) strvec_push(out, "");
    for (size_t i = 0; i < content.n && (int)out->n < height; i++) strvec_push(out, content.v[i]);
    while ((int)out->n < height) strvec_push(out, "");
    strvec_free(&content);
    free(wc0), free(wc1), free(esc), free(clock), free(name);
}

/* ---------- the component ---------- */

typedef struct {
    tui_component_t base;
    ui_t *u;
    int64_t started;
    int timer;
    int turns, jobs;
} pause_t;

static pause_t *g_pause;

/* pauseWorkStatus: loops still running toward their next park; running jobs outside live subagents */
static void work_status(pause_t *p, int *turns, int *jobs) {
    *turns = agent_unsettled_loops();
    *jobs = (int)ui_background_job_count(p->u);
}

static void pause_render(tui_component_t *c, int width, strvec_t *out) {
    pause_t *p = (pause_t *)c;
    int rows = p->u->tui ? MAX(1, tui_term(p->u->tui)->rows) : 24;
    const char *name = ui_session_title(p->u);
    ui_pause_render(width, rows, mono_ms() - p->started, name, p->turns, p->jobs, out);
}

static void pause_close(pause_t *p) {
    ui_t *u = p->u;
    g_pause = NULL;
    tui_remove_timer(u->tui, p->timer);
    tui_overlay_hide(u->tui, &p->base);
    tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    int64_t held = agent_pause_gate_resume();
    tui_component_destroy(&p->base);
    if (held >= 0) {
        char *d = ui_coarse_duration(held);
        char *m = xasprintf("Resumed after %s \xe2\x80\x94 agents are running again.", d);
        add_notice(u, "info", m);
        free(m);
        free(d);
    }
    tui_request_render(u->tui);
}

static bool pause_key(tui_component_t *c, const tui_key_t *k) {
    /* every dismissal path resumes, ctrl+c included: it must never double as "abort agents" here */
    if (tui_kb(k, "app.interrupt") || tui_key_matches(k, "escape") || tui_key_matches(k, "enter") ||
        tui_key_matches(k, "return") || tui_key_matches(k, "space") || tui_key_matches(k, "ctrl+c"))
        pause_close((pause_t *)c);
    return true;
}

static void pause_destroy(tui_component_t *c) { free(c); }

static const tui_component_vt PAUSE_VT = {"pause-screen", pause_render, pause_key, NULL, pause_destroy};

static void pause_tick(void *ud) {
    pause_t *p = ud;
    work_status(p, &p->turns, &p->jobs);
    tui_component_request_render(&p->base);
}

bool ui_pause_active(void) { return g_pause != NULL; }

void ui_pause_run(ui_t *u) {
    if (g_pause || !agent_pause_gate_pause()) return;
    pause_t *p = xcalloc(1, sizeof *p);
    tui_component_init(&p->base, &PAUSE_VT);
    p->u = u;
    p->started = mono_ms();
    work_status(p, &p->turns, &p->jobs);
    g_pause = p;
    tui_alt_screen(u->tui, true);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &p->base, &o);
    p->timer = tui_add_timer(u->tui, PAUSE_TICK_MS, true, pause_tick, p);
    tui_request_render(u->tui);
}
