/* turn-lifecycle signals (WP-11): terminal title composition, OSC 7501 reports, terminal identity, notification
 * formats and routing (OSC 9 / OSC 99 / BEL, tmux passthrough, Zellij, Herdr, cmux, D-Bus argv), power
 * assertion commands, idle-recap eligibility. Expected bytes are omp v18.14.2's (captured under `script`). */
#include "coding/power.h"
#include "tui/notify.h"
#include "ui/signals.h"

#include <stdio.h>
#include <unistd.h>

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static bool eq_free(char *got, const char *want) {
    bool ok = got && want ? !strcmp(got, want) : got == NULL && want == NULL;
    if (!ok) fprintf(stderr, "  got  [%s]\n  want [%s]\n", got ? got : "(null)", want ? want : "(null)");
    free(got);
    return ok;
}

static const char *ENV_KEYS[] = {"KITTY_WINDOW_ID", "GHOSTTY_RESOURCES_DIR", "WEZTERM_PANE", "ITERM_SESSION_ID",
                                 "VSCODE_PID", "ALACRITTY_WINDOW_ID", "TERM_PROGRAM", "LC_TERMINAL", "TERM",
                                 "COLORTERM", "TMUX", "STY", "ZELLIJ", "HERDR_ENV", "HERDR_PANE_ID", "HERDR_TAB_ID",
                                 "HERDR_WORKSPACE_ID", "CMUX_WORKSPACE_ID", "CMUX_SURFACE_ID", "CMUX_REMOTE_TRANSPORT",
                                 "WMUX", "WMUX_SURFACE_ID", "PI_NOTIFICATIONS", "OMC_NOTIFICATIONS",
                                 "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR", "PI_NO_DESKTOP_NOTIFY"};
static void clear_env(void) {
    for (size_t i = 0; i < sizeof ENV_KEYS / sizeof *ENV_KEYS; i++) unsetenv(ENV_KEYS[i]);
}

static strvec_t spawned;
static bool spawn_ok = true;
static bool fake_spawn(const strvec_t *argv) {
    strvec_free(&spawned);
    for (size_t i = 0; i < argv->n; i++) strvec_push(&spawned, argv->v[i]);
    return spawn_ok;
}

/* send `n` through a non-tty term and return what reached the fd */
static char *sent(const tui_notification_t *n) {
    int fds[2];
    if (pipe(fds) != 0) return NULL;
    tui_term_t *t = tui_term_new(-1, fds[1]);
    tui_notify_send(t, n);
    tui_term_free(t);
    close(fds[1]);
    sb_t b = SB_INIT;
    char buf[512];
    ssize_t r;
    while ((r = read(fds[0], buf, sizeof buf)) > 0) sb_appendn(&b, buf, (size_t)r);
    close(fds[0]);
    return sb_detach(&b);
}

int main(void) {
    /* ---- terminal title (buildTerminalTitleWithState) ---- */
    CHECK(eq_free(ui_title_compose(NULL, RUN_IDLE, 0, true, "braille", false), "π >"));
    CHECK(eq_free(ui_title_compose("work", RUN_IDLE, 0, true, "braille", false), "π > work"));
    CHECK(eq_free(ui_title_compose("work", RUN_WORKING, 0, true, "braille", false), "π ⠋ work"));
    CHECK(eq_free(ui_title_compose("work", RUN_WORKING, 2, true, "braille", false), "π ⠹ work"));
    CHECK(eq_free(ui_title_compose("work", RUN_WORKING, 10, true, "braille", false), "π ⠋ work"));
    CHECK(eq_free(ui_title_compose("work", RUN_WORKING, 4, true, "pulse", false), "π ● work"));
    CHECK(eq_free(ui_title_compose("work", RUN_WORKING, 1, true, "dots", false), "π ⠂ work"));
    CHECK(eq_free(ui_title_compose("work", RUN_WORKING, 1, true, "line", false), "π \\ work"));
    CHECK(eq_free(ui_title_compose("work", RUN_WORKING, 3, true, "nope", false), "π ⠸ work"));
    CHECK(eq_free(ui_title_compose("work", RUN_WORKING, 3, true, "braille", true), "π : work")); /* WSL */
    CHECK(eq_free(ui_title_compose("work", RUN_BLOCKED, 0, true, "braille", false), "π ! work"));
    CHECK(eq_free(ui_title_compose(NULL, RUN_BLOCKED, 0, true, "braille", false), "π !"));
    CHECK(eq_free(ui_title_compose("work", RUN_WORKING, 0, false, "braille", false), "π: work"));
    CHECK(eq_free(ui_title_compose(NULL, RUN_DONE, 0, false, "braille", false), "π"));
    CHECK(eq_free(ui_title_compose("work", RUN_DONE, 0, true, "braille", false), "π > work"));
    const char *const *frames;
    CHECK(ui_title_spinner_frames("braille", &frames) == 10 && !strcmp(frames[9], "⠏"));
    CHECK(ui_title_spinner_frames("pulse", &frames) == 8);
    CHECK(ui_title_spinner_frames("dots", &frames) == 6);
    CHECK(ui_title_spinner_frames("line", &frames) == 4);
    CHECK(eq_free(ui_title_sanitize("  a\x1b]0;evil\x07 b\xc2\x85 "), "a]0;evil b"));
    CHECK(eq_free(ui_title_sanitize(" \t\n"), NULL));
    CHECK(eq_free(ui_title_sanitize("\xc2\xa0x\xe3\x80\x80"), "x"));

    /* ---- OSC 7501 (formatReport) ---- */
    CHECK(eq_free(ui_program_status_report(RUN_IDLE, NULL, NULL), "\x1b]7501;state=idle:app=omc\x1b\\"));
    CHECK(eq_free(ui_program_status_report(RUN_WORKING, NULL, "ignored"), "\x1b]7501;state=working:app=omc\x1b\\"));
    CHECK(eq_free(ui_program_status_report(RUN_DONE, NULL, NULL), "\x1b]7501;state=done:app=omc\x1b\\"));
    CHECK(eq_free(ui_program_status_report(RUN_BLOCKED, "question", "Pick one?"),
                  "\x1b]7501;state=blocked:kind=question:app=omc:msg=UGljayBvbmU/\x1b\\"));
    CHECK(eq_free(ui_program_status_report(RUN_ERROR, NULL, "400 bad things"),
                  "\x1b]7501;state=error:app=omc:msg=NDAwIGJhZCB0aGluZ3M=\x1b\\"));
    /* control characters and ANSI dropped, whitespace runs collapsed: "a b c" */
    CHECK(eq_free(ui_program_status_report(RUN_ERROR, NULL, "  \x1b[31ma\x1b[0m\n\t b\x01  c "),
                  "\x1b]7501;state=error:app=omc:msg=YSBiIGM=\x1b\\"));
    CHECK(eq_free(ui_program_status_report(RUN_ERROR, NULL, " \n "), "\x1b]7501;state=error:app=omc\x1b\\"));
    CHECK(eq_free(ui_program_status_report(RUN_BLOCKED, "permission", NULL),
                  "\x1b]7501;state=blocked:kind=permission:app=omc\x1b\\"));
    {
        /* decoded msg capped at 2048 bytes: 2045 bytes + "…" */
        char big[3000];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = 0;
        char *r = ui_program_status_report(RUN_ERROR, NULL, big);
        const char *m = strstr(r, ":msg=");
        size_t n = 0;
        unsigned char *dec = m ? base64_decode(m + 5, strlen(m + 5) - 2, &n) : NULL;
        CHECK(dec && n == 2048 && !memcmp(dec + 2045, "…", 3) && dec[2044] == 'x');
        free(dec);
        free(r);
    }

    /* ---- terminal identity (detectTerminalId) ---- */
    clear_env();
    CHECK(str_eq(tui_terminal_id_detect(false), "base"));
    setenv("COLORTERM", "truecolor", 1);
    CHECK(str_eq(tui_terminal_id_detect(false), "trueColor"));
    setenv("TERM_PROGRAM", "WezTerm", 1);
    CHECK(str_eq(tui_terminal_id_detect(false), "wezterm"));
    setenv("TERM_PROGRAM", "iTerm.app", 1);
    CHECK(str_eq(tui_terminal_id_detect(false), "iterm2"));
    setenv("TERM_PROGRAM", "WarpTerminal", 1);
    CHECK(str_eq(tui_terminal_id_detect(false), "warp"));
    setenv("KITTY_WINDOW_ID", "1", 1);
    CHECK(str_eq(tui_terminal_id_detect(false), "kitty"));
    clear_env();
    setenv("LC_TERMINAL", "iTerm2", 1);
    CHECK(str_eq(tui_terminal_id_detect(false), "iterm2"));
    setenv("TMUX", "/tmp/x,1,0", 1);
    CHECK(str_eq(tui_terminal_id_detect(false), "base")); /* iTerm2's LC_TERMINAL only outside a multiplexer */
    clear_env();
    setenv("TERM", "xterm-ghostty", 1);
    CHECK(str_eq(tui_terminal_id_detect(false), "ghostty"));
    setenv("TERM", "monstar", 1);
    CHECK(str_eq(tui_terminal_id_detect(false), "monstar"));
    CHECK(tui_terminal_notify_protocol("kitty") == TUI_NOTIFY_OSC99);
    CHECK(tui_terminal_notify_protocol("tern") == TUI_NOTIFY_OSC99);
    CHECK(tui_terminal_notify_protocol("ghostty") == TUI_NOTIFY_OSC9);
    CHECK(tui_terminal_notify_protocol("iterm2") == TUI_NOTIFY_OSC9);
    CHECK(tui_terminal_notify_protocol("vscode") == TUI_NOTIFY_BELL);
    CHECK(tui_terminal_notify_protocol("rio") == TUI_NOTIFY_BELL);
    CHECK(tui_terminal_notify_protocol("base") == TUI_NOTIFY_BELL);
    clear_env();
    setenv("TERM", "screen-256color", 1);
    CHECK(tui_inside_multiplexer());
    clear_env();
    CHECK(!tui_inside_multiplexer());

    /* ---- notification formats ---- */
    tui_notification_t done = {"🧪 PARK: Fix flaky park tests", "Complete", "completion", NULL};
    tui_notification_t ask = {"omc", "Waiting for input", "ask", "normal"};
    CHECK(eq_free(tui_format_notification(TUI_NOTIFY_BELL, &done), "\x07"));
    CHECK(eq_free(tui_format_notification(TUI_NOTIFY_OSC9, &done), "\x1b]9;🧪 PARK: Fix flaky park tests: Complete\x1b\\"));
    CHECK(eq_free(tui_format_notification(TUI_NOTIFY_OSC99, &ask), "\x1b]99;;omc: Waiting for input\x1b\\"));
    tui_notification_t body_only = {NULL, "Complete", NULL, NULL};
    CHECK(eq_free(tui_format_notification(TUI_NOTIFY_OSC9, &body_only), "\x1b]9;Complete\x1b\\"));
    CHECK(eq_free(tui_wrap_tmux_passthrough("\x1b]9;x\x1b\\"), "\x1bPtmux;\x1b\x1b]9;x\x1b\x1b\\\x1b\\"));
    {
        strvec_t v = tui_desktop_notify_argv("notify-send", "/usr/bin/notify-send", &done);
        char *j = strvec_join(&v, "|");
        CHECK(eq_free(j, "/usr/bin/notify-send|--app-name|omc|--urgency=normal|--expire-time=5000|🧪 PARK: Fix flaky park tests|Complete"));
        strvec_free(&v);
        tui_notification_t crit = {"  ", "b", NULL, "critical"};
        v = tui_desktop_notify_argv("gdbus", "/usr/bin/gdbus", &crit);
        j = strvec_join(&v, "|");
        CHECK(eq_free(j, "/usr/bin/gdbus|call|--session|--dest|org.freedesktop.Notifications|--object-path|"
                         "/org/freedesktop/Notifications|--method|org.freedesktop.Notifications.Notify|omc|0||omc|b|[]|"
                         "{\"urgency\": <byte 2>}|5000"));
        strvec_free(&v);
    }

    /* ---- sendNotification routing ---- */
    tui_notify_set_spawner(fake_spawn);
    clear_env();
    CHECK(eq_free(sent(&done), "\x07")); /* base terminal: BEL, no desktop session */
    CHECK(!spawned.n);
    /* tui_terminal_id is cached once per process: the remaining routing checks run on that BEL terminal */
    setenv("TMUX", "/tmp/t,1,0", 1);
    CHECK(eq_free(sent(&done), "\x07")); /* tmux passes BEL notifications through unchanged */
    clear_env();
    setenv("ZELLIJ", "0", 1);
    CHECK(eq_free(sent(&done), "\x07"));
    clear_env();
    setenv("HERDR_PANE_ID", "w1:p2", 1);
    CHECK(eq_free(sent(&ask), ""));
    CHECK(eq_free(strvec_join(&spawned, "|"), "herdr|notification|show|omc|--body|Waiting for input|--sound|request"));
    spawn_ok = false; /* no herdr binary: the terminal fallback */
    CHECK(eq_free(sent(&done), "\x07"));
    spawn_ok = true;
    clear_env();
    setenv("CMUX_SURFACE_ID", "0123abcd-0123-4567-89ab-0123456789ab", 1);
    CHECK(eq_free(sent(&done), ""));
    CHECK(eq_free(strvec_join(&spawned, "|"),
                  "cmux|notify|--surface|0123abcd-0123-4567-89ab-0123456789ab|--title|🧪 PARK: Fix flaky park tests|--body|Complete"));
    setenv("CMUX_SURFACE_ID", "not-a-uuid", 1);
    strvec_free(&spawned);
    CHECK(eq_free(sent(&done), "\x07"));
    CHECK(!spawned.n);
    clear_env();
    setenv("PI_NOTIFICATIONS", "off", 1);
    CHECK(eq_free(sent(&done), ""));
    clear_env();
    setenv("XDG_RUNTIME_DIR", "/nonexistent-omc-test", 1);
    CHECK(!tui_should_deliver_desktop_notification(true));
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/x", 1);
#ifdef __linux__
    CHECK(tui_should_deliver_desktop_notification(true));
#endif
    CHECK(!tui_should_deliver_desktop_notification(false));
    setenv("PI_NO_DESKTOP_NOTIFY", "1", 1);
    CHECK(!tui_should_deliver_desktop_notification(true));
    clear_env();
    strvec_free(&spawned);

    /* ---- power.sleepPrevention commands ---- */
    {
        strvec_t v = power_command("off");
        CHECK(v.n == 0);
        v = power_command("idle");
        char *j = strvec_join(&v, " ");
#ifdef __APPLE__
        CHECK(str_starts(j, "caffeinate -i -w "));
#else
        CHECK(eq_free(xstrdup(j), "systemd-inhibit --what=idle --who=omc --why=omc agent session --mode=block --no-ask-password cat"));
#endif
        free(j);
        strvec_free(&v);
        v = power_command("system");
        j = strvec_join(&v, " ");
#ifdef __APPLE__
        CHECK(str_starts(j, "caffeinate -i -s -u -d -w "));
#else
        CHECK(str_starts(j, "systemd-inhibit --what=idle:sleep "));
#endif
        free(j);
        strvec_free(&v);
    }

    /* ---- idle recap: priorMessageWasGenerated ---- */
    {
        json_t *m = json_parse_str("[{\"role\":\"user\",\"content\":\"hi\"},{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}]");
        CHECK(ui_recap_generated(m));
        json_free(m);
        m = json_parse_str("[{\"role\":\"assistant\",\"stopReason\":\"error\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"\"},{\"type\":\"toolCall\",\"id\":\"1\"}]}]");
        CHECK(ui_recap_generated(m));
        json_free(m);
        m = json_parse_str("[{\"role\":\"assistant\",\"stopReason\":\"aborted\",\"content\":[{\"type\":\"text\",\"text\":\"\"}]}]");
        CHECK(!ui_recap_generated(m));
        json_free(m);
        m = json_parse_str("[{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"x\"}]},{\"role\":\"toolResult\"}]");
        CHECK(!ui_recap_generated(m));
        json_free(m);
        m = json_parse_str("[]");
        CHECK(!ui_recap_generated(m));
        json_free(m);
    }

    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
