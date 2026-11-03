/*
 * render.h - component model, differential renderer and event loop (pi-tui design).
 *
 * Components render into an array of lines for a given width. The TUI concatenates the
 * root container's lines, composites overlays, and writes only the lines that changed
 * since the previous frame. Rendering is inline (no alternate screen): content taller
 * than the terminal scrolls into native scrollback. If a change lands above the visible
 * viewport (already in scrollback) or the width changes, the TUI clears screen+scrollback
 * and redraws everything.
 *
 * Every rendered line gets "\x1b[0m\x1b]8;;\x07" appended so styles never bleed. Lines
 * wider than the terminal are truncated. A focused component may emit TUI_CURSOR_MARKER
 * (see ansi.h) where the hardware cursor should be parked (for IME); the marker is
 * stripped before output.
 *
 * Threading: everything runs on the UI thread (the one calling tui_run) except
 * tui_request_render(), tui_post() and tui_stop(), which may be called from any thread.
 *
 * Typical use:
 *     tui_t *tui = tui_new(NULL);                    // stdin/stdout
 *     tui_editor_t *ed = tui_editor_new(); ...
 *     tui_container_add(tui_root(tui), &ed->base);
 *     tui_set_focus(tui, &ed->base);
 *     int code = tui_run(tui);                        // until tui_stop()
 *     tui_free(tui);
 */
#ifndef OMC_TUI_RENDER_H
#define OMC_TUI_RENDER_H

#include "core/core.h"
#include "tui/ansi.h"
#include "tui/keys.h"
#include "tui/term.h"
#include "tui/theme.h"

typedef struct tui tui_t;
typedef struct tui_component tui_component_t;

typedef struct {
    const char *name;
    /* Append owned lines (no '\n') for `width` columns to `out`. Required. */
    void (*render)(tui_component_t *c, int width, strvec_t *out);
    /* Handle a key while focused. Return true if consumed. Optional. */
    bool (*handle_key)(tui_component_t *c, const tui_key_t *k);
    /* Drop render caches (theme change, resize). Optional. */
    void (*invalidate)(tui_component_t *c);
    /* Free the component (including the struct itself). Optional (default: free(c)). */
    void (*destroy)(tui_component_t *c);
} tui_component_vt;

struct tui_component {
    const tui_component_vt *vt;
    tui_t *tui;    /* set by tui_set_focus / when attached through containers with a tui */
    bool focused;  /* maintained by tui_set_focus */
    bool hidden;   /* hidden components render nothing */
    void *user;    /* free for application use */
    bool wants_mouse; /* receives SGR mouse reports (TUI_KEY_MOUSE) while it is the top overlay */
};

void tui_component_init(tui_component_t *c, const tui_component_vt *vt);
void tui_component_render(tui_component_t *c, int width, strvec_t *out);
bool tui_component_handle_key(tui_component_t *c, const tui_key_t *k);
void tui_component_invalidate(tui_component_t *c);
void tui_component_destroy(tui_component_t *c);
/* Ask the owning TUI (if any) to re-render. */
void tui_component_request_render(tui_component_t *c);

/* ---------- container (vertical stack; owns its children) ---------- */
typedef struct {
    tui_component_t base;
    VEC(tui_component_t *) children;
} tui_container_t;

tui_container_t *tui_container_new(void);
void tui_container_init(tui_container_t *c); /* for embedding in derived components */
void tui_container_add(tui_container_t *c, tui_component_t *child);
void tui_container_insert(tui_container_t *c, size_t index, tui_component_t *child);
/* Remove without destroying; returns false if not a child. */
bool tui_container_remove(tui_container_t *c, tui_component_t *child);
void tui_container_clear(tui_container_t *c); /* destroys children */
size_t tui_container_count(const tui_container_t *c);
void tui_container_render_children(tui_container_t *c, int width, strvec_t *out);
void tui_container_destroy_children(tui_container_t *c);

/* ---------- overlays ---------- */
typedef enum { TUI_ANCHOR_BOTTOM, TUI_ANCHOR_CENTER, TUI_ANCHOR_TOP } tui_anchor_t;

typedef struct {
    tui_anchor_t anchor;
    int width;          /* columns; 0 = full width; negative = percent of terminal (e.g. -60) */
    int max_height;     /* rows; 0 = terminal height */
    int margin_x;       /* left/right margin when width==0 */
    int offset_y;       /* rows from the anchored edge */
} tui_overlay_opts_t;

/* Show `c` over the viewport and give it focus; the previous focus is restored on hide.
 * The TUI does not take ownership. */
void tui_overlay_show(tui_t *t, tui_component_t *c, const tui_overlay_opts_t *opts);
void tui_overlay_hide(tui_t *t, tui_component_t *c);
/* anchor overlay `c` against the full terminal height (content shorter than the screen is padded first), like
 * omp's overlays over a short transcript */
void tui_overlay_fill_screen(tui_t *t, tui_component_t *c);
bool tui_overlay_visible(const tui_t *t);

/* ---------- TUI ---------- */
/* `term` NULL => stdin/stdout. The TUI owns the terminal. */
tui_t *tui_new(tui_term_t *term);
void tui_free(tui_t *t); /* restores terminal, destroys root tree */
tui_term_t *tui_term(tui_t *t);
/* frame row shown on 1-based screen row `screen_row` (mouse reports), or -1 outside the frame */
int tui_frame_row_at(tui_t *t, int screen_row);
tui_container_t *tui_root(tui_t *t);

void tui_set_focus(tui_t *t, tui_component_t *c);
tui_component_t *tui_focus(tui_t *t);
void tui_attach(tui_t *t, tui_component_t *c); /* set c->tui without focusing */

/* Global key hook, called before the focused component. Return true to consume. */
typedef bool (*tui_key_hook_fn)(tui_t *t, const tui_key_t *k, void *ud);
void tui_set_key_hook(tui_t *t, tui_key_hook_fn fn, void *ud);
/* Signal hook for SIGINT/SIGTERM/SIGHUP/SIGQUIT received while running. Return true if
 * handled; otherwise the loop stops with exit code 128+sig. */
typedef bool (*tui_signal_hook_fn)(tui_t *t, int sig, void *ud);
void tui_set_signal_hook(tui_t *t, tui_signal_hook_fn fn, void *ud);

void tui_set_show_hardware_cursor(tui_t *t, bool show);
void tui_set_min_render_interval(tui_t *t, int ms); /* throttle, default 33 (omp: 30 fps) */

/* Thread-safe. Coalesced and throttled (min interval). The _full variant clears screen and
 * scrollback and repaints everything. */
void tui_request_render(tui_t *t);
void tui_request_full_render(tui_t *t);
/* Thread-safe: run fn(ud) on the UI thread at the next loop iteration. */
void tui_post(tui_t *t, void (*fn)(void *ud), void *ud);
/* Thread-safe: make tui_run return `code`. */
void tui_stop(tui_t *t, int code);

/* Timers (UI thread only). Returns id > 0. Repeating timers re-arm every interval_ms. */
int tui_add_timer(tui_t *t, int interval_ms, bool repeat, void (*fn)(void *ud), void *ud);
void tui_remove_timer(tui_t *t, int id);

/* Start terminal (raw mode, protocols) without running the loop. tui_run calls it. */
void tui_start(tui_t *t);
/* Stop terminal: move cursor below content, restore modes. tui_free calls it. */
void tui_shutdown(tui_t *t);
/* Run the event loop until tui_stop. Returns the stop code. */
int tui_run(tui_t *t);
/* One loop iteration with timeout_ms (-1 = block). Returns false once stopped. */
bool tui_run_once(tui_t *t, int timeout_ms);
/* Render immediately (UI thread). */
void tui_render_now(tui_t *t);
/* Called by a component while it renders: frame rows from `row` on (absolute, i.e. the output's
 * length when the component starts) may still change, so they are kept out of native scrollback.
 * When they would scroll off the screen they are clipped instead (the screen shows the frame's tail)
 * and written to scrollback once no hold covers them. omp: unfinalized transcript blocks never retire
 * into history. */
void tui_hold_history(tui_t *t, size_t row);
/* Enter/leave the alternate screen for a fullscreen page (omp: fullscreen overlays borrow it). The
 * normal screen's frame is parked and resumes untouched on exit (the terminal restores that screen);
 * a full render requested meanwhile is applied to the normal screen when the page closes. */
void tui_alt_screen(tui_t *t, bool on);
bool tui_alt_screen_active(const tui_t *t);
/* Feed raw input bytes as if read from the terminal (tests / embedding). */
void tui_feed_input(tui_t *t, const char *data, size_t n);
/* Dispatch one decoded key (global hook -> focused component). */
void tui_dispatch_key(tui_t *t, const tui_key_t *k);
/* Suspend (ctrl+z): restore the terminal, SIGSTOP the foreground process group, then resume and
 * redraw. Returns 0, or -errno when the stop signal was refused (the TUI is back either way). */
int tui_suspend(tui_t *t);

/* Paint observer (omp TUI.addPaintListener / TuiPaint), called on the UI thread after each written frame:
 * `history` the rows this paint committed above the viewport (native scrollback; empty on the alternate
 * screen), `viewport` the rows on screen, `reset` when the paint erased scrollback and repainted from row
 * zero, `alt` for an alternate-screen page. Pointers are valid for the call only. NULL fn removes it. */
typedef struct {
    const char *const *history;
    size_t nhistory;
    const char *const *viewport;
    size_t nviewport;
    bool reset, alt;
    int columns, rows;
} tui_paint_t;
typedef void (*tui_paint_fn)(void *ud, const tui_paint_t *p);
void tui_set_paint_listener(tui_t *t, tui_paint_fn fn, void *ud);

/* Introspection (tests): previous frame lines and the hardware cursor row. */
const strvec_t *tui_prev_lines(const tui_t *t);
int tui_render_count(const tui_t *t);
int tui_full_redraw_count(const tui_t *t);

#endif
