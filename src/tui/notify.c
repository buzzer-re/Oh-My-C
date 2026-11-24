/* notify.c - terminal identity and desktop notifications (see notify.h). */
#include "tui/notify.h"

#include "core/fs.h"
#include "core/proc.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------- multiplexers (terminal-multiplexer.ts registry) ---------- */

static bool herdr_inside(void) {
    return str_eq(env_get("HERDR_ENV"), "1") || env_get("HERDR_PANE_ID") || env_get("HERDR_TAB_ID") ||
           env_get("HERDR_WORKSPACE_ID");
}
static bool cmux_inside(void) {
    return env_get("CMUX_WORKSPACE_ID") || env_get("CMUX_SURFACE_ID") || env_get("CMUX_REMOTE_TRANSPORT");
}
bool tui_inside_tmux(void) { return env_get("TMUX") != NULL; }
static bool screen_inside(void) { return env_get("STY") != NULL; }
static bool zellij_inside(void) { return env_get("ZELLIJ") != NULL; }
static bool wmux_inside(void) { return str_eq(env_get("WMUX"), "1") || env_get("WMUX_SURFACE_ID"); }

/* classifyTerminalMultiplexerModule(...)?.ownsScreenGrid: every session-tier multiplexer owns the grid (Orca, the
 * only outer app, does not), then a tmux/screen TERM prefix */
bool tui_inside_multiplexer(void) {
    if (herdr_inside() || tui_inside_tmux() || screen_inside() || zellij_inside() || cmux_inside() || wmux_inside())
        return true;
    char *term = str_lower(env_get("TERM") ? env_get("TERM") : "");
    bool r = str_starts(term, "tmux") || str_starts(term, "screen");
    free(term);
    return r;
}

char *tui_wrap_tmux_passthrough(const char *payload) {
    sb_t b = SB_INIT;
    sb_append(&b, "\x1bPtmux;");
    for (const char *p = payload; p && *p; p++) {
        if (*p == '\x1b') sb_putc(&b, '\x1b');
        sb_putc(&b, *p);
    }
    sb_append(&b, "\x1b\\");
    return sb_detach(&b);
}

/* ---------- terminal identity (detectTerminalId) ---------- */

static const char *from_program(const char *p) {
    if (!p) return NULL;
    static const char *const MAP[][2] = {
        {"kitty", "kitty"},   {"ghostty", "ghostty"}, {"wezterm", "wezterm"},   {"iterm.app", "iterm2"},
        {"iterm2", "iterm2"}, {"vscode", "vscode"},   {"alacritty", "alacritty"}, {"warpterminal", "warp"},
        {"orca", "orca"},     {"otty", "otty"},       {"rio", "rio"},           {"tern", "tern"},
        {"monstar", "monstar"}, {"rex", "rex"},
    };
    for (size_t i = 0; i < ARRAY_LEN(MAP); i++)
        if (str_ieq(p, MAP[i][0])) return MAP[i][1];
    return NULL;
}

/* tmuxClientProgramsFromReport: "#{client_termtype}\t#{client_termname}" -> emulator names, best first */
static void tmux_client_programs(const char *report, strvec_t *out) {
    const char *tab = strchr(report, '\t');
    char *termtype = tab ? xstrndup(report, (size_t)(tab - report)) : xstrdup(report);
    char *tt = str_trim(termtype);
    /* CLIENT_TERMTYPE_NAME: leading [A-Za-z][A-Za-z0-9._+-]* followed by whitespace, '(' or the end */
    size_t n = 0;
    if (isalpha((unsigned char)tt[0])) {
        n = 1;
        while (isalnum((unsigned char)tt[n]) || tt[n] == '.' || tt[n] == '_' || tt[n] == '+' || tt[n] == '-') n++;
        if (tt[n] && !isspace((unsigned char)tt[n]) && tt[n] != '(') n = 0;
    }
    if (n) strvec_push_own(out, xstrndup(tt, n));
    if (tab) {
        char *tn = str_trim(tab + 1);
        char *low = str_lower(tn);
        if (!strcmp(low, "xterm-kitty")) strvec_push(out, "kitty");
        else if (!strcmp(low, "xterm-ghostty")) strvec_push(out, "ghostty");
        else if (!strcmp(low, "xterm-rex")) strvec_push(out, "rex");
        else if (!strcmp(low, "wezterm")) strvec_push(out, "wezterm");
        free(low);
        free(tn);
    }
    free(tt);
    free(termtype);
}

static strvec_t query_tmux_client_names(void) {
    strvec_t out = {0};
    char *tmux = which("tmux");
    if (!tmux) return out;
    const char *argv[] = {tmux, "display-message", "-p", "#{client_termtype}\t#{client_termname}", NULL};
    proc_opts_t o = {0};
    o.timeout_ms = 2000;
    o.max_output = 4096;
    proc_result_t r = {0};
    if (proc_run(argv, &o, &r) == 0 && r.exit_code == 0 && r.out) tmux_client_programs(r.out, &out);
    proc_result_free(&r);
    free(tmux);
    return out;
}

static const char *detect(bool query_tmux) {
    if (env_get("KITTY_WINDOW_ID")) return "kitty";
    if (env_get("GHOSTTY_RESOURCES_DIR")) return "ghostty";
    if (env_get("WEZTERM_PANE")) return "wezterm";
    if (env_get("ITERM_SESSION_ID")) return "iterm2";
    if (env_get("VSCODE_PID")) return "vscode";
    if (env_get("ALACRITTY_WINDOW_ID")) return "alacritty";
    const char *id = from_program(env_get("TERM_PROGRAM"));
    if (id) return id;
    if (str_ieq(env_get("LC_TERMINAL"), "iTerm2") && !tui_inside_multiplexer()) return "iterm2";
    if (query_tmux && tui_inside_tmux()) {
        strvec_t names = query_tmux_client_names();
        const char *found = NULL;
        for (size_t i = 0; i < names.n && !found; i++) found = from_program(names.v[i]);
        strvec_free(&names);
        if (found) return found;
    }
    const char *term = env_get("TERM");
    if (term && str_istr(term, "ghostty")) return "ghostty";
    if (str_ieq(term, "monstar")) return "monstar";
    if (str_ieq(term, "xterm-rex")) return "rex";
    const char *ct = env_get("COLORTERM");
    if (ct && (str_ieq(ct, "truecolor") || str_ieq(ct, "24bit"))) return "trueColor";
    return "base";
}

const char *tui_terminal_id_detect(bool query_tmux) { return detect(query_tmux); }

const char *tui_terminal_id(void) {
    static const char *cached;
    if (!cached) cached = detect(true);
    return cached;
}

/* KNOWN_TERMINALS notifyProtocol */
tui_notify_proto_t tui_terminal_notify_protocol(const char *id) {
    if (str_eq(id, "kitty") || str_eq(id, "otty") || str_eq(id, "tern")) return TUI_NOTIFY_OSC99;
    if (str_eq(id, "ghostty") || str_eq(id, "wezterm") || str_eq(id, "iterm2") || str_eq(id, "warp") ||
        str_eq(id, "monstar"))
        return TUI_NOTIFY_OSC9;
    return TUI_NOTIFY_BELL;
}

/* ---------- formatting ---------- */

static const char *title_of(const tui_notification_t *n, const char *def) {
    return n->title && *n->title ? n->title : def;
}

/* notificationToLine */
static char *notification_line(const tui_notification_t *n) {
    if (n->title && *n->title && n->body && *n->body) return xasprintf("%s: %s", n->title, n->body);
    return xstrdup(n->title && *n->title ? n->title : n->body ? n->body : "");
}

char *tui_format_notification(tui_notify_proto_t proto, const tui_notification_t *n) {
    if (proto == TUI_NOTIFY_BELL) return xstrdup("\x07");
    /* OSC 99's structured form needs the `p=?` capability probe, which omc does not send: like omp before
     * the probe answers, both protocols carry the single `title: body` line */
    char *line = notification_line(n);
    char *r = xasprintf("%s%s\x1b\\", proto == TUI_NOTIFY_OSC99 ? "\x1b]99;;" : "\x1b]9;", line);
    free(line);
    return r;
}

/* ---------- D-Bus desktop fallback (desktop-notify.ts) ---------- */

bool tui_should_deliver_desktop_notification(bool proto_is_bell) {
    if (!proto_is_bell) return false;
#ifndef __linux__
    return false;
#else
    bool session = env_get("DBUS_SESSION_BUS_ADDRESS") != NULL;
    if (!session && env_get("XDG_RUNTIME_DIR")) {
        char *p = xasprintf("%s/bus", env_get("XDG_RUNTIME_DIR"));
        session = access(p, F_OK) == 0;
        free(p);
    }
    if (!session) return false;
    if (str_eq(env_get("PI_NO_DESKTOP_NOTIFY"), "1") || str_eq(env_get("OMC_NO_DESKTOP_NOTIFY"), "1")) return false;
    return true;
#endif
}

strvec_t tui_desktop_notify_argv(const char *notifier, const char *path, const tui_notification_t *n) {
    strvec_t v = {0};
    char *title = str_trim(title_of(n, TUI_NOTIFY_APP_NAME));
    if (!*title) {
        free(title);
        title = xstrdup(TUI_NOTIFY_APP_NAME);
    }
    const char *body = n->body ? n->body : "";
    const char *urg = str_eq(n->urgency, "critical") || str_eq(n->urgency, "low") ? n->urgency : "normal";
    strvec_push(&v, path);
    if (str_eq(notifier, "notify-send")) {
        strvec_push(&v, "--app-name");
        strvec_push(&v, TUI_NOTIFY_APP_NAME);
        strvec_push_own(&v, xasprintf("--urgency=%s", urg));
        strvec_push(&v, "--expire-time=5000");
        strvec_push(&v, title);
        strvec_push(&v, body);
    } else {
        static const char *const HEAD[] = {"call", "--session", "--dest", "org.freedesktop.Notifications", "--object-path",
                                           "/org/freedesktop/Notifications", "--method",
                                           "org.freedesktop.Notifications.Notify"};
        for (size_t i = 0; i < ARRAY_LEN(HEAD); i++) strvec_push(&v, HEAD[i]);
        strvec_push(&v, TUI_NOTIFY_APP_NAME);
        strvec_push(&v, "0");
        strvec_push(&v, "");
        strvec_push(&v, title);
        strvec_push(&v, body);
        strvec_push(&v, "[]");
        strvec_push_own(&v, xasprintf("{\"urgency\": <byte %d>}", str_eq(urg, "low") ? 0 : str_eq(urg, "critical") ? 2 : 1));
        strvec_push(&v, "5000");
    }
    free(title);
    return v;
}

/* Bun.spawn(...).unref() with stdio ignored: a double fork so the notifier never becomes our zombie and never
 * holds the terminal; false when the binary is not on PATH */
static bool spawn_detached(const strvec_t *argv) {
    if (!argv->n) return false;
    char *bin = strchr(argv->v[0], '/') ? xstrdup(argv->v[0]) : which(argv->v[0]);
    if (!bin) return false;
#ifdef _WIN32
    /* detached, stdio ignored, never our child to reap (a detached spawn is not tracked) */
    char **wav = xcalloc(argv->n + 1, sizeof *wav);
    for (size_t i = 0; i < argv->n; i++) wav[i] = argv->v[i];
    omc_win_spawn_opts_t so = {.exe = bin, .fd_in = -1, .fd_out = -1, .fd_err = -1, .detached = true};
    int wpid = omc_win_spawn(wav, &so);
    free(wav);
    free(bin);
    return wpid > 0;
#else
    pid_t pid = fork();
    if (pid < 0) {
        free(bin);
        return false;
    }
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        char **av = xcalloc(argv->n + 1, sizeof *av);
        for (size_t i = 0; i < argv->n; i++) av[i] = argv->v[i];
        execv(bin, av);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    free(bin);
    return true;
#endif
}

static bool (*g_spawner)(const strvec_t *argv) = spawn_detached;
void tui_notify_set_spawner(bool (*fn)(const strvec_t *argv)) { g_spawner = fn ? fn : spawn_detached; }

static void send_desktop(const tui_notification_t *n) {
    static int resolved;
    static char *path;
    static const char *kind;
    if (!resolved) {
        resolved = 1;
        if ((path = which("notify-send"))) kind = "notify-send";
        else if ((path = which("gdbus"))) kind = "gdbus";
    }
    if (!path) return;
    strvec_t argv = tui_desktop_notify_argv(kind, path, n);
    g_spawner(&argv);
    strvec_free(&argv);
}

/* ---------- multiplexer notifiers (innermost tier first: pane, surface, in-band) ---------- */

static bool herdr_pane_ok(const char *id) {
    size_t n = strlen(id);
    if (n < 1 || n > 64) return false;
    for (const char *p = id; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != ':' && *p != '_' && *p != '-') return false;
    return true;
}

static bool cmux_surface_ok(const char *id) {
    static const int groups[] = {8, 4, 4, 4, 12};
    const char *p = id;
    for (size_t g = 0; g < 5; g++) {
        for (int i = 0; i < groups[g]; i++, p++)
            if (!isxdigit((unsigned char)*p)) return false;
        if (g < 4 && *p++ != '-') return false;
    }
    return *p == 0;
}

static bool route_multiplexer(tui_term_t *t, const tui_notification_t *n, const char *sequence) {
    if (herdr_inside()) {
        char *pane = str_trim(env_get("HERDR_PANE_ID") ? env_get("HERDR_PANE_ID") : "");
        bool ok = false;
        if (*pane && herdr_pane_ok(pane)) {
            char *title = str_trim(title_of(n, TUI_NOTIFY_APP_NAME));
            if (!*title || !strcmp(title, "help") || !strcmp(title, "--help") || !strcmp(title, "-h")) {
                free(title);
                title = xstrdup(TUI_NOTIFY_APP_NAME);
            }
            const char *sound = str_eq(n->type, "ask") || str_eq(n->type, "error") ? "request"
                                : str_eq(n->type, "completion")                    ? "done"
                                                                                   : "none";
            strvec_t argv = {0};
            strvec_push(&argv, "herdr");
            strvec_push(&argv, "notification");
            strvec_push(&argv, "show");
            strvec_push(&argv, title);
            strvec_push(&argv, "--body");
            strvec_push(&argv, n->body ? n->body : "");
            strvec_push(&argv, "--sound");
            strvec_push(&argv, sound);
            ok = g_spawner(&argv);
            strvec_free(&argv);
            free(title);
        }
        free(pane);
        if (ok) return true;
    }
    if (cmux_inside()) {
        char *surface = str_trim(env_get("CMUX_SURFACE_ID") ? env_get("CMUX_SURFACE_ID") : "");
        bool ok = false;
        if (*surface && cmux_surface_ok(surface)) {
            char *title = str_trim(title_of(n, TUI_NOTIFY_APP_NAME));
            if (!*title) {
                free(title);
                title = xstrdup(TUI_NOTIFY_APP_NAME);
            }
            strvec_t argv = {0};
            const char *a[] = {"cmux", "notify", "--surface", surface, "--title", title, "--body", n->body ? n->body : ""};
            for (size_t i = 0; i < ARRAY_LEN(a); i++) strvec_push(&argv, a[i]);
            ok = g_spawner(&argv);
            strvec_free(&argv);
            free(title);
        }
        free(surface);
        if (ok) return true;
    }
    if (tui_inside_tmux() && sequence) {
        char *w = tui_wrap_tmux_passthrough(sequence);
        tui_term_puts(t, w);
        tui_term_puts(t, "\x07");
        free(w);
        return true;
    }
    if (zellij_inside() && sequence) {
        tui_term_puts(t, sequence);
        tui_term_puts(t, "\x07");
        return true;
    }
    return false;
}

bool tui_notifications_suppressed(void) {
    const char *names[] = {"PI_NOTIFICATIONS", "OMC_NOTIFICATIONS"};
    for (size_t i = 0; i < ARRAY_LEN(names); i++) {
        const char *v = env_get(names[i]);
        if (v && (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false"))) return true;
    }
    return false;
}

void tui_notify_send(tui_term_t *t, const tui_notification_t *n) {
    if (!t || tui_notifications_suppressed()) return;
    tui_notify_proto_t proto = tui_terminal_notify_protocol(tui_terminal_id());
    char *formatted = tui_format_notification(proto, n);
    if (!route_multiplexer(t, n, proto == TUI_NOTIFY_BELL ? NULL : formatted)) {
        tui_term_puts(t, formatted);
        if (proto == TUI_NOTIFY_BELL && tui_should_deliver_desktop_notification(true)) send_desktop(n);
    }
    tui_term_flush(t);
    free(formatted);
}
