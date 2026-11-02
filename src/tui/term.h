/*
 * term.h - terminal control: raw mode, size, signals, protocol toggles, buffered output.
 *
 * Output is accumulated in an in-memory buffer (`tui_term_write*`) and sent with one
 * write burst by `tui_term_flush()`, so a frame reaches the terminal at once.
 *
 * Signals: `tui_term_install_signals()` routes SIGWINCH, SIGTERM, SIGHUP, SIGINT, SIGQUIT,
 * SIGTSTP and SIGCONT into a self-pipe (`tui_term_signal_fd`, drained with
 * `tui_term_read_signal`). If a terminating signal arrives while no event loop is
 * dispatching (`tui_term_set_loop_active(false)`), the handler restores the terminal
 * itself (async-signal-safe) and re-raises with the default action. An atexit hook
 * also restores the terminal.
 *
 * Non-tty fds (tests) are supported: raw mode and probes are skipped, size defaults to
 * 80x24 or whatever `tui_term_set_size` sets.
 */
#ifndef OMC_TUI_TERM_H
#define OMC_TUI_TERM_H

#include "core/core.h"

#include <termios.h>

typedef struct tui_term {
    int in_fd, out_fd;
    bool is_tty;      /* in_fd and out_fd are terminals */
    bool raw;         /* raw mode currently active */
    bool started;     /* protocols enabled by tui_term_start */
    struct termios saved;
    int cols, rows;
    bool size_fixed;  /* set by tui_term_set_size; disables ioctl queries */
    bool kitty_active;
    int kitty_flags;
    bool kitty_queried;
    bool modify_other_keys;
    bool bracketed_paste;
    bool focus_events;
    bool cursor_hidden;
    bool sync_output; /* use CSI ?2026 synchronized output (default true) */
    bool mouse_capture; /* tui.mouse: SGR any-motion reporting on the normal screen */
    sb_t out;
} tui_term_t;

tui_term_t *tui_term_new(int in_fd, int out_fd);
void tui_term_free(tui_term_t *t); /* restores the terminal first */

/* Raw mode (termios); no-ops for non-ttys. */
int tui_term_enter_raw(tui_term_t *t);
void tui_term_leave_raw(tui_term_t *t);
/* Enter raw mode, enable bracketed paste, query kitty keyboard protocol, hide cursor. */
void tui_term_start(tui_term_t *t);
/* Undo everything tui_term_start did (pop kitty, disable modes, show cursor, restore termios). */
void tui_term_stop(tui_term_t *t);

/* Size */
void tui_term_query_size(tui_term_t *t); /* ioctl TIOCGWINSZ, fallback COLUMNS/LINES, 80x24 */
void tui_term_set_size(tui_term_t *t, int cols, int rows);

/* Signals */
int tui_term_install_signals(tui_term_t *t);
void tui_term_uninstall_signals(void);
int tui_term_signal_fd(void);
int tui_term_read_signal(void); /* next pending signal number, 0 if none */
void tui_term_set_loop_active(bool active);

/* Feed a terminal reply (TUI_KEY_RESPONSE text). Handles the kitty flags reply (pushes
 * the progressive enhancement flags) and DA1 (modifyOtherKeys fallback). Returns true
 * if consumed. */
bool tui_term_handle_response(tui_term_t *t, const char *seq, size_t n);

/* Buffered output */
void tui_term_write(tui_term_t *t, const char *s, size_t n);
void tui_term_puts(tui_term_t *t, const char *s);
void tui_term_printf(tui_term_t *t, const char *fmt, ...) PRINTF_LIKE(2, 3);
int tui_term_flush(tui_term_t *t); /* 0 or -errno */

/* Mode toggles (buffered) */
void tui_term_cursor_show(tui_term_t *t, bool show);
void tui_term_bracketed_paste(tui_term_t *t, bool on);
void tui_term_focus_events(tui_term_t *t, bool on);
void tui_term_kitty_push(tui_term_t *t, int flags); /* CSI > flags u */
void tui_term_kitty_pop(tui_term_t *t);             /* CSI < u */
void tui_term_sync_begin(tui_term_t *t);
void tui_term_sync_end(tui_term_t *t);
void tui_term_set_title(tui_term_t *t, const char *title);
/* tui.mouse inline click capture: MOUSE_TRACKING_ON / OFF now (when started) and across stop/start */
void tui_term_set_mouse_capture(tui_term_t *t, bool on);
#define TUI_MOUSE_ON "\x1b[?1000h\x1b[?1003h\x1b[?1006h"
#define TUI_MOUSE_OFF "\x1b[?1006l\x1b[?1003l\x1b[?1000l"
/* OSC 52 clipboard copy (base64 payload). */
void tui_term_copy_osc52(tui_term_t *t, const char *text, size_t n);
void tui_term_clear_screen(tui_term_t *t, bool scrollback);

/* OSC 8 hyperlink: appends ESC]8;;uri BEL text ESC]8;; BEL to `b`. Control characters in
 * `uri` are dropped. */
void tui_hyperlink(sb_t *b, const char *uri, const char *text);

#endif
