/* debugctl.c - `/debug` (port of omp debug/index.ts DebugSelectorComponent, SelectorController.showDebugSelector,
 * CommandController.handleDebugTranscriptCommand and pi-tui apps/debug/terminal-info.ts, v18.14.2).
 *
 * The "Debug Tools" selector replaces the composer. omc offers the tools it implements: open the artifact
 * folder, report bundles (dump / memory), system info, terminal state, the TUI transcript export and the
 * artifact-cache cleanup. omp's JS-runtime tools (CPU profile, work-scheduling flamegraph, JavaScriptCore
 * remote debugger), its log viewer and raw-SSE viewer (omc keeps no process logs or SSE capture) and the
 * terminal-protocol probe are not offered. */
#include "modes/interactive.h"

#include "coding/debugtools.h"
#include "config/settings.h"
#include "tui/ansi.h"
#include "tui/notify.h"
#include "tui/termcaps.h"
#include "tools/outsink.h"

#include <ctype.h>
#include <sys/stat.h>
#include <time.h>

static const struct {
    const char *value, *label, *desc;
} ITEMS[] = {
    {"open-artifacts", "Open: artifact folder", "Open session artifacts in file manager"},
    {"dump", "Report: dump session", "Create report bundle immediately"},
    {"memory", "Report: memory issue", "Memory statistics + bundle"},
    {"system", "View: system info", "Show environment details"},
    {"terminal", "View: terminal state", "Subprotocols, geometry, scrollback strategy"},
    {"transcript", "Export: TUI transcript", "Write visible TUI conversation to a temp txt"},
    {"clear-cache", "Clear: artifact cache", "Remove old session artifacts"},
};

/* ---------- transcript blocks: TranscriptBlock of DynamicBorder / Text(text, 1, 0) rows ---------- */

typedef struct {
    tui_component_t base;
    strvec_t rows; /* "\x01" = DynamicBorder, else styled text */
} block_t;

static void block_render(tui_component_t *c, int width, strvec_t *out) {
    block_t *b = (block_t *)c;
    for (size_t i = 0; i < b->rows.n; i++) {
        if (str_eq(b->rows.v[i], "\x01")) {
            sb_t r = SB_INIT;
            sb_repeat(&r, tui_sym("boxRound.horizontal"), MAX(1, width));
            strvec_push_own(out, fg(TUI_TC_BORDER, sb_cstr(&r)));
            sb_free(&r);
            continue;
        }
        strvec_t w = {0};
        char *clean = tui_replace_tabs(b->rows.v[i]);
        tui_wrap_text_with_ansi(clean, MAX(1, width - 2), &w);
        free(clean);
        for (size_t k = 0; k < w.n; k++) strvec_push_own(out, xasprintf(" %s", w.v[k]));
        strvec_free(&w);
    }
}

static void block_destroy(tui_component_t *c) {
    strvec_free(&((block_t *)c)->rows);
    free(c);
}

static const tui_component_vt BLOCK_VT = {"debug-block", block_render, NULL, NULL, block_destroy};

static block_t *block_new(void) {
    block_t *b = xcalloc(1, sizeof *b);
    tui_component_init(&b->base, &BLOCK_VT);
    return b;
}

static void block_text(block_t *b, tui_tc_t tc, const char *text) { strvec_push_own(&b->rows, fg(tc, text)); }

static void present(ui_t *u, block_t *b) {
    tui_container_add(u->chat, &b->base);
    tui_request_render(u->tui);
}

/* formatFileHyperlink: OSC 8 file:// link around the path */
static char *file_link(const char *path) { return xasprintf("\x1b]8;;file://%s\x07%s\x1b]8;;\x07", path, path); }

/* ---------- handlers ---------- */

static void open_artifacts(ui_t *u) {
    const char *sf = app_session_file(u->app);
    if (!sf) {
        add_notice(u, "warning", "No active session file.");
        return;
    }
    size_t n = strlen(sf);
    char *dir = xstrndup(sf, n > 6 ? n - 6 : n);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) add_notice(u, "warning", "Artifact folder does not exist yet.");
    else {
        ui_open_in_browser(dir);
        char *m = xasprintf("Opened: %s", dir);
        add_notice(u, "info", m);
        free(m);
    }
    free(dir);
}

/* #getResolvedSettings */
static json_t *resolved_settings(ui_t *u) {
    app_t *a = u->app;
    json_t *o = json_obj();
    if (a->model) json_set_str(o, "model", a->model->id);
    /* session.thinkingLevel: the effective level; undefined (left out) for a model without reasoning */
    if (a->model && a->model->reasoning) json_set_str(o, "thinkingLevel", effort_name(a->thinking));
    json_set_bool(o, "planModeEnabled", a->plan_mode);
    json_set_bool(o, "toolOutputExpanded", u->expand_tools);
    json_set_bool(o, "hideThinkingBlock", u->hide_thinking);
    return o;
}

static void report(ui_t *u, bool memory) {
    json_t *settings = resolved_settings(u), *mem = memory ? debug_memory_stats() : NULL;
    strvec_t files = {0};
    char *err = NULL;
    char *path = debug_report_bundle(NULL, app_session_file(u->app), u->app->cwd, settings, mem, &files, &err);
    if (!path) {
        char *m = xasprintf("Failed to create report: %s", err ? err : "unknown error");
        add_notice(u, "error", m);
        free(m);
    } else {
        block_t *b = block_new();
        block_text(b, TUI_TC_SUCCESS, memory ? "+ Memory report saved" : "+ Report bundle saved");
        char *link = file_link(path);
        block_text(b, TUI_TC_DIM, link);
        free(link);
        char *f = xasprintf("Files: %zu", files.n);
        block_text(b, TUI_TC_DIM, f);
        free(f);
        if (memory)
            block_text(b, TUI_TC_WARNING, "Review before sharing: session data, artifacts, logs and settings may contain secrets.");
        present(u, b);
    }
    free(path);
    free(err);
    strvec_free(&files);
    json_free(settings);
    json_free(mem);
}

static void bordered(ui_t *u, const char *text) {
    block_t *b = block_new();
    strvec_push(&b->rows, "\x01");
    strvec_push(&b->rows, text);
    strvec_push(&b->rows, "\x01");
    present(u, b);
}

static void system_info(ui_t *u) {
    json_t *info = debug_system_info(u->app->cwd);
    char *t = debug_format_system_info(info);
    bordered(u, t);
    free(t);
    json_free(info);
}

/* classifyTerminalMultiplexer: session markers, then a tmux/screen TERM prefix, then outer apps */
static const char *multiplexer(void) {
    const char *e;
    if (str_eq(env_get("HERDR_ENV"), "1") || env_get("HERDR_PANE_ID") || env_get("HERDR_TAB_ID") || env_get("HERDR_WORKSPACE_ID"))
        return "herdr";
    if ((e = env_get("TMUX")) && *e) return "tmux";
    if ((e = env_get("STY")) && *e) return "screen";
    if ((e = env_get("ZELLIJ")) && *e) return "zellij";
    if (env_get("CMUX_WORKSPACE_ID") || env_get("CMUX_SURFACE_ID") || env_get("CMUX_REMOTE_TRANSPORT")) return "cmux";
    if (str_eq(env_get("WMUX"), "1") || env_get("WMUX_SURFACE_ID")) return "wmux";
    const char *term = env_get("TERM");
    if (term && !strncasecmp(term, "tmux", 4)) return "tmux";
    if (term && !strncasecmp(term, "screen", 6)) return "screen";
    char *pk = str_trim(env_get("ORCA_PANE_KEY") ? env_get("ORCA_PANE_KEY") : "");
    char *wt = str_trim(env_get("ORCA_WORKTREE_ID") ? env_get("ORCA_WORKTREE_ID") : "");
    bool orca = *pk && *wt;
    free(pk), free(wt);
    return orca ? "orca" : NULL;
}

static const char *yes_no(bool v) { return v ? "yes" : "no"; }
static const char *unset(const char *s) { return s ? s : "(unset)"; }

/* collectTerminalState + formatTerminalState over what omc's renderer actually uses */
char *ui_debug_terminal_state(int cols, int rows, bool sync_output) {
    int cw, ch;
    tui_cell_dims(&cw, &ch);
    const char *img;
    switch (tui_image_protocol()) {
    case TUI_IMG_KITTY: img = "Kitty graphics"; break;
    case TUI_IMG_ITERM2: img = "iTerm2 inline images"; break;
    case TUI_IMG_SIXEL: img = "Sixel"; break;
    default: img = "none";
    }
    const char *id = tui_terminal_id();
    tui_notify_proto_t np = tui_terminal_notify_protocol(id);
    const char *notify = np == TUI_NOTIFY_OSC99 ? "OSC 99 (kitty desktop notifications)"
                         : np == TUI_NOTIFY_OSC9 ? "OSC 9 (iTerm2/WezTerm)"
                                                 : "BEL (\\a)";
    const char *mux = multiplexer();
    sb_t b = SB_INIT;
    sb_append(&b, "Terminal State\n\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
                  "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\n");
    sb_appendf(&b, "Detected:     %s\n", id);
    sb_appendf(&b, "Geometry:     %dx%d cells \xc2\xb7 cell %dx%dpx\n", cols, rows, cw, ch);
    if (mux) sb_appendf(&b, "Multiplexer:  %s\n", mux);
    else sb_append(&b, "Multiplexer:  none\n");
    sb_append(&b, "\nSubprotocols\n");
    sb_appendf(&b, "  Graphics:     %s\n", img);
    sb_appendf(&b, "  Notify:       %s\n", notify);
    sb_appendf(&b, "  Hyperlinks:   %s (OSC 8)\n", yes_no(tui_hyperlinks_enabled()));
    sb_appendf(&b, "  True color:   %s (24-bit SGR)\n", yes_no(tui_detect_color_mode() == TUI_COLOR_TRUECOLOR));
    sb_append(&b, "  DECCARA:      no (rectangular-SGR background fills)\n"); /* omc never emits DECCARA */
    sb_appendf(&b, "  Sync output:  %s (DEC 2026)\n", yes_no(sync_output));
    sb_append(&b, "\nScrollback\n");
    sb_append(&b, "  Screen->history clear: CSI 2 J (redraw)\n"); /* omc never uses CSI 22 J */
    sb_append(&b, "\nDetection signals\n");
    sb_appendf(&b, "  TERM:                 %s\n", unset(env_get("TERM")));
    sb_appendf(&b, "  TERM_PROGRAM:         %s\n", unset(env_get("TERM_PROGRAM")));
    sb_appendf(&b, "  TERM_PROGRAM_VERSION: %s\n", unset(env_get("TERM_PROGRAM_VERSION")));
    sb_appendf(&b, "  COLORTERM:            %s", unset(env_get("COLORTERM")));
    return sb_detach(&b);
}

static void terminal_state(ui_t *u) {
    tui_term_t *t = tui_term(u->tui);
    char *s = ui_debug_terminal_state(t->cols, t->rows, t->sync_output);
    bordered(u, s);
    free(s);
}

/* handleDebugTranscriptCommand: the transcript as rendered at the terminal width, ANSI stripped */
static void transcript(ui_t *u) {
    int width = MAX(1, tui_term(u->tui)->cols);
    strvec_t lines = {0};
    tui_component_render(&u->chat->base, width, &lines);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < lines.n; i++) {
        char *plain = tui_strip_ansi(lines.v[i]), *t = tui_replace_tabs(plain);
        sb_appendf(&b, "%s%s", i ? "\n" : "", t);
        free(plain), free(t);
    }
    strvec_free(&lines);
    while (b.len && isspace((unsigned char)sb_cstr(&b)[b.len - 1])) b.len--;
    if (!b.len) add_notice(u, "error", "No messages to dump yet.");
    else {
        sb_putc(&b, '\n');
        const char *tmp = getenv("TMPDIR");
        char *id = snowflake_next(), *name = xasprintf("%s-tmp.txt", id);
        char *dir = xstrdup(tmp && *tmp ? tmp : "/tmp");
        size_t dl = strlen(dir);
        if (dl > 1 && dir[dl - 1] == '/') dir[dl - 1] = 0;
        char *path = path_join(dir, name);
        if (fs_write(path, sb_cstr(&b), b.len) != 0) add_notice(u, "error", "Failed to write debug transcript: Unknown error");
        else {
            char *m = xasprintf("Debug transcript written to:\n%s", path);
            add_notice(u, "info", m);
            free(m);
        }
        free(id), free(name), free(dir), free(path);
    }
    sb_free(&b);
}

static char *sessions_dir(void) { return omc_agent_path("sessions"); }

static void clear_cache_cb(void *ud, bool yes) {
    ui_t *u = ud;
    if (!yes) {
        add_notice(u, "info", "Cache clear cancelled.");
        return;
    }
    char *dir = sessions_dir();
    int removed = debug_clear_artifact_cache(dir, 30);
    free(dir);
    block_t *b = block_new();
    char *m = xasprintf("- Cleared %d artifact directories", removed);
    block_text(b, TUI_TC_SUCCESS, m);
    free(m);
    present(u, b);
}

static void clear_cache(ui_t *u) {
    char *dir = sessions_dir();
    debug_cache_stats_t st;
    debug_artifact_cache_stats(dir, &st);
    free(dir);
    if (st.count == 0) {
        add_notice(u, "info", "Artifact cache is empty.");
        return;
    }
    char *size = out_format_bytes(st.total_size);
    char oldest[32] = "unknown";
    if (st.oldest_ms) { /* toLocaleDateString (en-US): M/D/YYYY */
        time_t t = (time_t)(st.oldest_ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(oldest, sizeof oldest, "%d/%d/%d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
    }
    char *msg = xasprintf("Found %d artifact files (%s)\nOldest: %s\n\nRemove artifacts older than 30 days?", st.count, size, oldest);
    ui_hook_confirm(u, "Clear Artifact Cache", msg, clear_cache_cb, u);
    free(msg);
    free(size);
}

static void dispatch(ui_t *u, const char *value) {
    if (str_eq(value, "open-artifacts")) open_artifacts(u);
    else if (str_eq(value, "dump")) report(u, false);
    else if (str_eq(value, "memory")) report(u, true);
    else if (str_eq(value, "system")) system_info(u);
    else if (str_eq(value, "terminal")) terminal_state(u);
    else if (str_eq(value, "transcript")) transcript(u);
    else if (str_eq(value, "clear-cache")) clear_cache(u);
}

static void on_pick(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    ui_t *u = ud;
    char *v = xstrdup(it->value);
    ui_close_selector(u); /* frees `it` */
    dispatch(u, v);
    free(v);
    tui_request_render(u->tui);
}

static void on_cancel(tui_select_list_t *l, void *ud) { ui_close_selector((ui_t *)ud); }

void dc_debug_open(ui_t *u) {
    ui_panel_t *p = ui_panel_new("Debug Tools");
    tui_container_add(&p->c, &tui_spacer_new(1)->base);
    tui_select_list_t *l = tui_select_list_new(7);
    for (size_t i = 0; i < ARRAY_LEN(ITEMS); i++) tui_select_list_add(l, ITEMS[i].value, ITEMS[i].label, ITEMS[i].desc);
    l->on_select = on_pick;
    l->on_cancel = on_cancel;
    l->ud = u;
    tui_container_add(&p->c, &l->base);
    ui_show_selector(u, &p->c.base, &l->base);
}
