/* extractl.c - TUI handlers (`handleTui`) of the WP-28 commands: /changelog (CommandController
 * handleChangelogCommand), /setup [providers] (runProviderSetupWizard), /force (builtin-control.ts),
 * /skillful (builtin-modes.ts), /pause (builtin-control.ts, ui/pause.c), /debug (modes/debugctl.c); the startup
 * release notes (startup.changelogMode, the "What's New" header block); /record (toggleRecording over
 * coding/record.c and the renderer's paint observer). The session logic is shared with
 * RPC/ACP in coding/extracmds.c. */
#include "modes/interactive.h"

#include "coding/changelog.h"
#include "coding/extracmds.h"

#include "coding/record.h"
#include "config/settings.h"
#include "core/fs.h"
#include "session/dump.h"

#include <ctype.h>

static char *lower_trim(const char *s) {
    char *t = str_trim(s ? s : "");
    for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
    return t;
}

static void changelog_command(ui_t *u, const char *arg) {
    changelog_view_t v;
    char *err = NULL;
    if (!changelog_parse_view(arg, &v, &err)) {
        add_notice(u, "warning", err);
        free(err);
        return;
    }
    size_t shown = 0;
    char *md = changelog_markdown(&v, &shown);
    sb_t b = SB_INIT;
    sb_append(&b, md ? md : "No changelog entries found.");
    if (v.kind != CHANGELOG_FULL) {
        char *use = tui_fgs(NULL, TUI_TC_DIM, "Use"), *cmd = tui_bold("/changelog full"),
             *tail = tui_fgs(NULL, TUI_TC_DIM, "to view the complete changelog.");
        sb_appendf(&b, "\n\n%s %s %s", use, cmd, tail);
        free(use);
        free(cmd);
        free(tail);
    }
    char buf[64];
    ui_show_markdown_report(u, changelog_title(&v, shown, buf, sizeof buf), sb_cstr(&b));
    sb_free(&b);
    free(md);
}

static void force_command(ui_t *u, const char *args) {
    const char *sp = strchr(args, ' ');
    char *tool = sp ? xstrndup(args, (size_t)(sp - args)) : xstrdup(args);
    char *prompt = sp ? str_trim(sp + 1) : xstrdup("");
    char *err = NULL;
    if (!*tool) add_notice(u, "error", "Usage: /force:<tool-name> [prompt]");
    else if (extracmd_force_tool(u->app, tool, &err) != 0) add_notice(u, "error", err);
    else {
        char *m = xasprintf("Next turn forced to use %s.", tool);
        add_notice(u, "info", m);
        free(m);
        if (*prompt) iq_submit(u, prompt, NULL); /* the prompt passes through as input */
    }
    free(err);
    free(tool);
    free(prompt);
}

static void skillful_command(ui_t *u, const char *args) {
    char *arg = lower_trim(args);
    if (str_eq(arg, "status")) add_notice(u, "info", extracmd_skillful() ? "Skill listing: on." : "Skill listing: off.");
    else if (!*arg || str_eq(arg, "toggle") || str_eq(arg, "on") || str_eq(arg, "off")) {
        bool on = extracmd_set_skillful(u->app, str_eq(arg, "on") ? 1 : str_eq(arg, "off") ? 0 : -1);
        add_notice(u, "info", on ? "Skill listing enabled for this session." : "Skill listing disabled for this session.");
    } else add_notice(u, "info", "Usage: /skillful [on|off|status]");
    free(arg);
}

static void setup_command(ui_t *u, const char *name, const char *args) {
    char *arg = lower_trim(args);
    if (!*arg || str_eq(arg, "providers")) ui_setup_run_providers(u);
    else {
        char *m = xasprintf("Usage: /%s [providers]", name);
        add_notice(u, "warning", m);
        free(m);
    }
    free(arg);
}

/* ---------- /record (InteractiveMode.toggleRecording + SessionRecorder) ---------- */

static record_writer_t *g_rec;
static int g_rec_timer;

static void rec_flush(void *ud) {
    g_rec_timer = 0;
    if (g_rec) record_writer_flush(g_rec);
}

static void rec_paint(void *ud, const tui_paint_t *p) {
    ui_t *u = ud;
    if (!g_rec) return;
    record_paint_t rp = {p->history, p->nhistory, p->viewport, p->nviewport, p->reset, p->alt, p->columns, p->rows};
    record_writer_paint(g_rec, &rp, mono_ms());
    if (!g_rec_timer) g_rec_timer = tui_add_timer(u->tui, RECORD_FLUSH_MS, false, rec_flush, u);
}

void xc_toggle_recording(ui_t *u) {
    if (g_rec) {
        record_writer_t *w = g_rec;
        g_rec = NULL;
        tui_set_paint_listener(u->tui, NULL, NULL);
        if (g_rec_timer) tui_remove_timer(u->tui, g_rec_timer);
        g_rec_timer = 0;
        char *d = dump_format_duration((double)record_writer_elapsed(w, mono_ms()));
        char *m = xasprintf("Saved %s recording to %s \xc2\xb7 replay: omc play", d, record_writer_path(w));
        record_writer_close(w);
        ui_status_set_recording(false);
        add_notice(u, "info", m);
        free(m), free(d);
        return;
    }
    app_t *a = u->app;
    const char *cwd = a->cwd ? a->cwd : ".";
    record_redactor_t *red = record_redactor_load(cwd, settings_get("stream.redactPatterns"));
    const char *title = a->session && a->session->title && *a->session->title ? a->session->title : path_basename(cwd);
    char *path = record_new_path(a->session ? session_id(a->session) : "");
    tui_term_t *t = tui_term(u->tui);
    char *err = NULL;
    g_rec = record_writer_open(path, t->cols, t->rows, title, red, &err);
    free(path);
    if (!g_rec) {
        char *m = xasprintf("Could not start recording: %s", err ? err : "unknown error");
        add_notice(u, "error", m);
        free(m), free(err);
        return;
    }
    tui_set_paint_listener(u->tui, rec_paint, u);
    ui_status_set_recording(true);
    char *m = xasprintf("Recording to %s \xc2\xb7 /record again to stop", record_writer_path(g_rec));
    add_notice(u, "info", m);
    free(m);
}

/* the TUI is closing: stop and save a running recording */
void xc_recording_shutdown(ui_t *u) {
    if (!g_rec) return;
    tui_set_paint_listener(u->tui, NULL, NULL);
    if (g_rec_timer) tui_remove_timer(u->tui, g_rec_timer);
    g_rec_timer = 0;
    record_writer_close(g_rec);
    g_rec = NULL;
    ui_status_set_recording(false);
}

bool xc_command(ui_t *u, const char *cmd, const char *name, const char *arg) {
    if (str_eq(cmd, "changelog")) changelog_command(u, arg);
    else if (str_eq(cmd, "force")) force_command(u, arg);
    else if (str_eq(cmd, "skillful")) skillful_command(u, arg);
    else if (str_eq(cmd, "setup")) setup_command(u, name, arg);
    else if (str_eq(cmd, "pause")) ui_pause_run(u); /* builtin-control.ts pause: runPauseScreen */
    else if (str_eq(cmd, "debug")) dc_debug_open(u); /* builtin-lifecycle.ts debug: showDebugSelector */
    else if (str_eq(cmd, "record")) xc_toggle_recording(u); /* builtin-control.ts record: toggleRecording */
    else return false;
    return true;
}

char *xc_description(ui_t *u, const char *name) {
    if (str_eq(name, "force")) return extracmd_force_description(u->app);
    if (str_eq(name, "skillful")) return xasprintf("Skill listing: %s", extracmd_skillful() ? "on" : "off");
    return NULL;
}

/* ---------- startup release notes (interactive-mode.ts headerAfter "What's New") ---------- */

static changelog_startup_t g_startup;
static bool g_startup_set;

/* cfgStartupChangelogMode with the legacy `collapseChangelog` boolean (true -> summary, false -> expanded) */
static const char *changelog_mode(void) {
    const char *m = settings_str("startup.changelogMode", NULL);
    if (m && (str_eq(m, "summary") || str_eq(m, "expanded") || str_eq(m, "hidden"))) return m;
    const json_t *legacy = settings_get("collapseChangelog");
    if (legacy && json_is_bool(legacy)) return json_bool(legacy) ? "summary" : "expanded";
    return "summary";
}

void interactive_startup_changelog(void) {
    char *marker = omc_agent_path("last-changelog-version");
    changelog_startup_free(&g_startup);
    g_startup_set = changelog_startup_resolve(changelog_mode(), marker, &g_startup);
    free(marker);
}

/* DynamicBorder */
static void border_render(tui_component_t *c, int width, strvec_t *out) {
    sb_t r = SB_INIT;
    sb_repeat(&r, tui_sym("boxRound.horizontal"), MAX(1, width));
    strvec_push_own(out, fg(TUI_TC_BORDER, sb_cstr(&r)));
    sb_free(&r);
}

static void border_destroy(tui_component_t *c) { free(c); }

static const tui_component_vt BORDER_VT = {"dynamic-border", border_render, NULL, NULL, border_destroy};

static tui_component_t *border_new(void) {
    tui_component_t *c = xcalloc(1, sizeof *c);
    tui_component_init(c, &BORDER_VT);
    return c;
}

/* `/changelog` and `/changelog full` in the summary turn bold */
static char *bold_commands(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s; *p;) {
        const char *k = strstr(p, "/changelog");
        if (!k) {
            sb_append(&b, p);
            break;
        }
        sb_appendn(&b, p, (size_t)(k - p));
        size_t n = strlen("/changelog");
        if (!strncmp(k + n, " full", 5)) n += 5;
        char *cmd = xstrndup(k, n), *bd = tui_bold(cmd);
        sb_append(&b, bd);
        free(cmd), free(bd);
        p = k + n;
    }
    return sb_detach(&b);
}

void xc_whats_new(ui_t *u, tui_container_t *root) {
    if (!g_startup_set || settings_bool("startup.quiet", false)) return;
    const char *mode = changelog_mode();
    if (str_eq(mode, "hidden")) return;
    tui_container_add(root, border_new());
    char *f = fg(TUI_TC_ACCENT, "What's New"), *t = tui_bold(f);
    tui_container_add(root, &tui_text_new(t, 1, 0)->base);
    free(f), free(t);
    tui_container_add(root, &tui_spacer_new(1)->base);
    if (str_eq(mode, "summary")) {
        char *sum = changelog_startup_summary(&g_startup), *b = bold_commands(sum);
        tui_container_add(root, &tui_text_new(b, 1, 0)->base);
        free(sum), free(b);
    } else {
        char *md = str_trim(g_startup.markdown ? g_startup.markdown : "");
        tui_container_add(root, &tui_markdown_new(md, 1, 0)->base);
        free(md);
    }
    tui_container_add(root, &tui_spacer_new(1)->base);
    tui_container_add(root, border_new());
}
