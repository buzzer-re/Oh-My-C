/* term.c - terminal control */
#include "tui/term.h"
#include "tui/ansi.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ---------- globals for signal handling / emergency restore ---------- */
static int g_sig_pipe[2] = {-1, -1};
static volatile sig_atomic_t g_loop_active;
static volatile sig_atomic_t g_restore_needed;
static volatile sig_atomic_t g_kitty_pushed;
static volatile sig_atomic_t g_mok;
static int g_restore_fd = -1;
static int g_tty_fd = -1;
static struct termios g_saved_tio;
static tui_term_t *g_term; /* the started terminal, for atexit */
static bool g_atexit_registered;
static bool g_signals_installed;

static const int HANDLED_SIGNALS[] = {SIGWINCH, SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGTSTP, SIGCONT};
static struct sigaction g_old_actions[ARRAY_LEN(HANDLED_SIGNALS)];

static void write_all_raw(int fd, const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

/* async-signal-safe */
static void emergency_restore(void) {
    if (!g_restore_needed) return;
    g_restore_needed = 0;
    if (g_restore_fd >= 0) {
        static const char base[] = "\x1b[?2026l\x1b[?2004l\x1b[?1004l\x1b[0m\x1b[?25h\r\n";
        if (g_kitty_pushed) write_all_raw(g_restore_fd, "\x1b[<u", 4);
        if (g_mok) write_all_raw(g_restore_fd, "\x1b[>4;0m", 7);
        write_all_raw(g_restore_fd, base, sizeof base - 1);
    }
    if (g_tty_fd >= 0) tcsetattr(g_tty_fd, TCSAFLUSH, &g_saved_tio);
}

static void on_signal(int sig) {
    int saved_errno = errno;
    bool terminating = sig == SIGTERM || sig == SIGHUP || sig == SIGINT || sig == SIGQUIT;
    if (terminating && !g_loop_active) {
        emergency_restore();
        signal(sig, SIG_DFL);
        raise(sig);
        errno = saved_errno;
        return;
    }
    if (g_sig_pipe[1] >= 0) {
        unsigned char b = (unsigned char)sig;
        ssize_t r = write(g_sig_pipe[1], &b, 1);
        (void)r;
    }
    errno = saved_errno;
}

static void atexit_restore(void) {
    if (g_term) tui_term_stop(g_term);
    emergency_restore();
}

static void set_cloexec_nonblock(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

int tui_term_install_signals(tui_term_t *t) {
    (void)t;
    if (g_signals_installed) return 0;
    if (g_sig_pipe[0] < 0) {
        if (pipe(g_sig_pipe) != 0) return -errno;
        set_cloexec_nonblock(g_sig_pipe[0]);
        set_cloexec_nonblock(g_sig_pipe[1]);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (size_t i = 0; i < ARRAY_LEN(HANDLED_SIGNALS); i++) sigaction(HANDLED_SIGNALS[i], &sa, &g_old_actions[i]);
    g_signals_installed = true;
    return 0;
}

void tui_term_uninstall_signals(void) {
    if (!g_signals_installed) return;
    for (size_t i = 0; i < ARRAY_LEN(HANDLED_SIGNALS); i++) sigaction(HANDLED_SIGNALS[i], &g_old_actions[i], NULL);
    g_signals_installed = false;
}

int tui_term_signal_fd(void) { return g_sig_pipe[0]; }

int tui_term_read_signal(void) {
    if (g_sig_pipe[0] < 0) return 0;
    unsigned char b;
    ssize_t r = read(g_sig_pipe[0], &b, 1);
    return r == 1 ? (int)b : 0;
}

void tui_term_set_loop_active(bool active) { g_loop_active = active; }

/* ---------- lifecycle ---------- */

tui_term_t *tui_term_new(int in_fd, int out_fd) {
    tui_term_t *t = xcalloc(1, sizeof *t);
    t->in_fd = in_fd;
    t->out_fd = out_fd;
    t->is_tty = isatty(in_fd) && isatty(out_fd);
    t->sync_output = !env_truthy("OMC_TUI_NO_SYNC");
    t->cols = 80;
    t->rows = 24;
    tui_term_query_size(t);
    return t;
}

void tui_term_free(tui_term_t *t) {
    if (!t) return;
    tui_term_stop(t);
    if (g_term == t) g_term = NULL;
    sb_free(&t->out);
    free(t);
}

int tui_term_enter_raw(tui_term_t *t) {
    if (!t->is_tty || t->raw) return 0;
    struct termios tio;
    if (tcgetattr(t->in_fd, &t->saved) != 0) return -errno;
    tio = t->saved;
    tio.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    tio.c_oflag &= ~(tcflag_t)(OPOST);
    tio.c_cflag |= CS8;
    tio.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(t->in_fd, TCSAFLUSH, &tio) != 0) return -errno;
    g_saved_tio = t->saved;
    g_tty_fd = t->in_fd;
    t->raw = true;
    return 0;
}

void tui_term_leave_raw(tui_term_t *t) {
    if (!t->raw) return;
    tcsetattr(t->in_fd, TCSAFLUSH, &t->saved);
    t->raw = false;
    if (g_tty_fd == t->in_fd) g_tty_fd = -1;
}

void tui_term_start(tui_term_t *t) {
    if (t->started) return;
    tui_term_enter_raw(t);
    t->started = true;
    g_term = t;
    g_restore_fd = t->out_fd;
    g_restore_needed = 1;
    if (!g_atexit_registered) {
        atexit(atexit_restore);
        g_atexit_registered = true;
    }
    tui_term_bracketed_paste(t, true);
    if (t->is_tty && !env_truthy("OMC_TUI_NO_KITTY")) {
        /* query kitty keyboard support, then DA1 as a sentinel */
        tui_term_puts(t, "\x1b[?u\x1b[c");
        t->kitty_queried = true;
    }
    tui_term_cursor_show(t, false);
    if (t->mouse_capture && t->is_tty) tui_term_puts(t, TUI_MOUSE_ON);
    tui_term_flush(t);
}

void tui_term_set_mouse_capture(tui_term_t *t, bool on) {
    if (t->mouse_capture == on) return;
    t->mouse_capture = on;
    if (!t->started || !t->is_tty) return;
    tui_term_puts(t, on ? TUI_MOUSE_ON : TUI_MOUSE_OFF);
    tui_term_flush(t);
}

void tui_term_stop(tui_term_t *t) {
    if (!t->started) return;
    tui_term_sync_end(t);
    if (t->kitty_active) tui_term_kitty_pop(t);
    if (t->modify_other_keys) {
        tui_term_puts(t, "\x1b[>4;0m");
        t->modify_other_keys = false;
        g_mok = 0;
    }
    tui_term_bracketed_paste(t, false);
    if (t->focus_events) tui_term_focus_events(t, false);
    if (t->mouse_capture && t->is_tty) tui_term_puts(t, TUI_MOUSE_OFF);
    extern bool g_inband;
    if (g_inband) {
        tui_term_puts(t, "\x1b[?2048l");
        g_inband = false;
    }
    tui_term_puts(t, "\x1b[0m");
    tui_term_cursor_show(t, true);
    tui_term_flush(t);
    tui_term_leave_raw(t);
    t->started = false;
    t->kitty_queried = false;
    if (g_term == t) {
        g_restore_needed = 0;
        g_term = NULL;
    }
}

void tui_term_query_size(tui_term_t *t) {
    if (t->size_fixed) return;
    struct winsize ws;
    if (ioctl(t->out_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        t->cols = ws.ws_col;
        t->rows = ws.ws_row;
        return;
    }
    const char *c = env_get("COLUMNS"), *l = env_get("LINES");
    if (c && atoi(c) > 0) t->cols = atoi(c);
    if (l && atoi(l) > 0) t->rows = atoi(l);
}

void tui_term_set_size(tui_term_t *t, int cols, int rows) {
    t->cols = cols > 0 ? cols : 1;
    t->rows = rows > 0 ? rows : 1;
    t->size_fixed = true;
}

bool tui_term_handle_response(tui_term_t *t, const char *seq, size_t n) {
    if (n < 4 || seq[0] != '\x1b' || seq[1] != '[' || seq[2] != '?') return false;
    char fin = seq[n - 1];
    if (fin == 'u') {
        int reported = atoi(seq + 3);
        if (!t->started || t->kitty_active) return true;
        if (t->modify_other_keys) {
            tui_term_puts(t, "\x1b[>4;0m");
            t->modify_other_keys = false;
            g_mok = 0;
        }
        /* disambiguate (1) + report alternate keys (4); keep event types (2) if a parent enabled them */
        tui_term_kitty_push(t, (reported & 2) ? 7 : 5);
        tui_term_flush(t);
        return true;
    }
    if (fin == 'c') {
        /* DA1 sentinel: no kitty reply arrived first -> modifyOtherKeys fallback */
        if (t->started && t->kitty_queried && !t->kitty_active && !t->modify_other_keys) {
            tui_term_puts(t, "\x1b[>4;2m");
            t->modify_other_keys = true;
            g_mok = 1;
            tui_term_flush(t);
        }
        t->kitty_queried = false;
        return true;
    }
    return false;
}

/* ---------- output ---------- */

void tui_term_write(tui_term_t *t, const char *s, size_t n) { sb_appendn(&t->out, s, n); }
void tui_term_puts(tui_term_t *t, const char *s) { sb_append(&t->out, s); }

void tui_term_printf(tui_term_t *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_vappendf(&t->out, fmt, ap);
    va_end(ap);
}

int tui_term_flush(tui_term_t *t) {
    const char *s = t->out.s;
    size_t n = t->out.len;
    int rc = 0;
    while (n > 0) {
        ssize_t w = write(t->out_fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
                continue;
            }
            rc = -errno;
            break;
        }
        s += w;
        n -= (size_t)w;
    }
    sb_clear(&t->out);
    return rc;
}

void tui_term_cursor_show(tui_term_t *t, bool show) {
    tui_term_puts(t, show ? "\x1b[?25h" : "\x1b[?25l");
    t->cursor_hidden = !show;
}

void tui_term_bracketed_paste(tui_term_t *t, bool on) {
    tui_term_puts(t, on ? "\x1b[?2004h" : "\x1b[?2004l");
    t->bracketed_paste = on;
}

void tui_term_focus_events(tui_term_t *t, bool on) {
    tui_term_puts(t, on ? "\x1b[?1004h" : "\x1b[?1004l");
    t->focus_events = on;
}

void tui_term_kitty_push(tui_term_t *t, int flags) {
    tui_term_printf(t, "\x1b[>%du", flags);
    t->kitty_active = true;
    t->kitty_flags = flags;
    g_kitty_pushed = 1;
}

void tui_term_kitty_pop(tui_term_t *t) {
    tui_term_puts(t, "\x1b[<u");
    t->kitty_active = false;
    t->kitty_flags = 0;
    g_kitty_pushed = 0;
}

void tui_term_sync_begin(tui_term_t *t) {
    if (t->sync_output) tui_term_puts(t, "\x1b[?2026h");
}

void tui_term_sync_end(tui_term_t *t) {
    if (t->sync_output) tui_term_puts(t, "\x1b[?2026l");
}

static void append_sanitized(sb_t *b, const char *s) {
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) continue;
        sb_putc(b, (char)c);
    }
}

void tui_term_set_title(tui_term_t *t, const char *title) {
    tui_term_puts(t, "\x1b]0;");
    append_sanitized(&t->out, title);
    tui_term_puts(t, "\a");
}

void tui_term_copy_osc52(tui_term_t *t, const char *text, size_t n) {
    char *b64 = base64_encode(text, n);
    tui_term_puts(t, "\x1b]52;c;");
    tui_term_puts(t, b64);
    tui_term_puts(t, "\a");
    free(b64);
}

void tui_term_clear_screen(tui_term_t *t, bool scrollback) {
    tui_term_puts(t, scrollback ? "\x1b[3J\x1b[2J\x1b[H" : "\x1b[2J\x1b[H");
}

void tui_hyperlink(sb_t *b, const char *uri, const char *text) {
    sb_append(b, "\x1b]8;;");
    append_sanitized(b, uri);
    sb_append(b, "\a");
    sb_append(b, text ? text : "");
    sb_append(b, TUI_LINK_CLOSE);
}
