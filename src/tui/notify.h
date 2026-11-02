/*
 * notify.h - terminal identity and desktop notifications (port of omp packages/tui
 * terminal-capabilities.ts detectTerminalId / formatNotification / sendNotification, the
 * multiplexer notifiers (multiplexers/{herdr,cmux,tmux,zellij}.ts) and desktop-notify.ts).
 *
 * A notification goes, in omp's order: to a multiplexer that swallows or rewrites the terminal's
 * own sequence (Herdr / cmux CLIs, tmux DCS passthrough + BEL, Zellij sequence + BEL), else
 * in-band as the terminal's protocol (OSC 9, OSC 99 single-line form, or a bare BEL). On a BEL
 * terminal inside a Linux desktop session the toast is also handed to `notify-send` (or
 * `gdbus call … Notify`), detached and fire-and-forget. `PI_NOTIFICATIONS=off|0|false`
 * suppresses everything; `PI_NO_DESKTOP_NOTIFY=1` only the D-Bus fallback (OMC_* spellings too).
 */
#ifndef OMC_TUI_NOTIFY_H
#define OMC_TUI_NOTIFY_H

#include "core/core.h"
#include "tui/term.h"

/* omp's notifications name their source "omp"; omc names itself */
#define TUI_NOTIFY_APP_NAME "omc"

typedef enum { TUI_NOTIFY_BELL, TUI_NOTIFY_OSC99, TUI_NOTIFY_OSC9 } tui_notify_proto_t;

typedef struct {
    const char *title;   /* NULL: none */
    const char *body;    /* NULL: none */
    const char *type;    /* "completion" | "error" | "ask" (Herdr sound) or NULL */
    const char *urgency; /* "low" | "normal" | "critical" or NULL (normal) */
} tui_notification_t;

/* detectTerminalId over the process environment ("kitty", "ghostty", …, "trueColor", "base");
 * inside tmux the attached client's terminal type is asked once (`tmux display-message`). Cached. */
const char *tui_terminal_id(void);
/* the same detection, uncached and without the tmux client query (tests) */
const char *tui_terminal_id_detect(bool query_tmux);
tui_notify_proto_t tui_terminal_notify_protocol(const char *terminal_id);
/* a multiplexer owns the screen grid (isInsideTerminalMultiplexer) */
bool tui_inside_multiplexer(void);
bool tui_inside_tmux(void);

/* \x1bPtmux;<payload, ESC doubled>\x1b\\ (owned) */
char *tui_wrap_tmux_passthrough(const char *payload);
/* formatNotification: the in-band sequence for `proto` (owned) */
char *tui_format_notification(tui_notify_proto_t proto, const tui_notification_t *n);
/* argv of the D-Bus delivery through `notifier` ("notify-send" or "gdbus", at `path`); owned */
strvec_t tui_desktop_notify_argv(const char *notifier, const char *path, const tui_notification_t *n);
/* hasLinuxDesktopSession && !PI_NO_DESKTOP_NOTIFY, for a BEL terminal */
bool tui_should_deliver_desktop_notification(bool proto_is_bell);

/* sendNotification: deliver `n` (writes go to `t` and are flushed at once) */
void tui_notify_send(tui_term_t *t, const tui_notification_t *n);
/* PI_NOTIFICATIONS=off|0|false */
bool tui_notifications_suppressed(void);

/* test hook: the spawner used for notifier CLIs (default: detached double fork); returns false when the
 * binary cannot be spawned */
void tui_notify_set_spawner(bool (*fn)(const strvec_t *argv));

#endif
