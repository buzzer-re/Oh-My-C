/* copyctl.c - `/copy [code|cmd|link]` and `/open [link]` (port of omp builtin-collaboration.ts `copy` / `open`
 * and SelectorController.showCopySelector, as shipped in omp 18.14.2).
 *
 * `/copy` alone opens the fullscreen copy picker (ui/copysel.c) on the alternate screen; a pick copies
 * through OSC 52 and reports `Copied <label> to clipboard`, `o` on a link block opens it with the system
 * opener. `/copy code|cmd|link` copy the most recent fenced code block, bash/eval command or link of the
 * conversation; `/open` opens the most recent link. */
#include "modes/interactive.h"
#include "ui/tools/tc.h"
#include "core/fs.h"

#include <ctype.h>

static struct {
    tui_component_t *sel;
    char *content, *label, *href; /* the posted pick / open */
    bool open;
} g;

static void copy_text(ui_t *u, const char *text) { tui_term_copy_osc52(tui_term(u->tui), text, strlen(text)); }

static void sel_close(ui_t *u) {
    if (!g.sel) return;
    tui_component_t *c = g.sel;
    g.sel = NULL;
    tui_overlay_hide(u->tui, c);
    tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    tui_component_destroy(c);
    tui_request_render(u->tui);
}

static bool blank(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

static void picked(void *p) {
    ui_t *u = p;
    char *content = g.content, *label = g.label, *href = g.href;
    bool open = g.open;
    g.content = g.label = g.href = NULL;
    sel_close(u);
    if (open) {
        ui_open_in_browser(href);
        char *m = xasprintf("Opening %s: %s", label, href);
        add_notice(u, "info", m);
        free(m);
    } else if (blank(content)) {
        add_notice(u, "info", "Nothing to copy in that item");
    } else {
        copy_text(u, content);
        char *m = xasprintf("Copied %s to clipboard", label);
        add_notice(u, "info", m);
        free(m);
    }
    free(content), free(label), free(href);
}

static void on_pick(void *ud, const char *content, const char *label) {
    ui_t *u = ud;
    if (g.content || g.href) return;
    g.content = xstrdup(content ? content : "");
    g.label = xstrdup(label ? label : "");
    g.open = false;
    tui_post(u->tui, picked, u);
}

static void on_open(void *ud, const char *href, const char *label) {
    ui_t *u = ud;
    if (g.content || g.href) return;
    g.href = xstrdup(href);
    g.label = xstrdup(label ? label : "link");
    g.open = true;
    tui_post(u->tui, picked, u);
}

static void cancelled(void *p) { sel_close((ui_t *)p); }

static void on_cancel(void *ud) {
    ui_t *u = ud;
    tui_post(u->tui, cancelled, u);
}

static void show_selector(ui_t *u) {
    if (g.sel) return;
    ui_copy_cb_t cb = {on_pick, on_open, on_cancel, u};
    tui_component_t *c = ui_copy_selector_new(u, &cb);
    if (!c) {
        add_notice(u, "info", "Nothing to copy yet.");
        return;
    }
    g.sel = c;
    tui_alt_screen(u->tui, true);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, c, &o);
    tui_request_render(u->tui);
}

/* the agent's messages, cloned under its lock */
static json_t *messages(ui_t *u) {
    pthread_mutex_lock(&u->app->agent->mu);
    json_t *m = json_clone(u->app->agent->messages);
    pthread_mutex_unlock(&u->app->agent->mu);
    return m;
}

static char *lower_arg(const char *arg) {
    char *t = str_trim(arg ? arg : "");
    for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
    return t;
}

void cc_copy_command(ui_t *u, const char *arg) {
    char *a = lower_arg(arg);
    if (!*a) {
        show_selector(u);
        free(a);
        return;
    }
    json_t *msgs = messages(u);
    if (str_eq(a, "code")) {
        char *code = ui_copy_last_code(msgs);
        if (!code) add_notice(u, "info", "No code block to copy.");
        else {
            copy_text(u, code);
            add_notice(u, "info", "Copied code block to clipboard");
        }
        free(code);
    } else if (str_eq(a, "cmd") || str_eq(a, "command")) {
        const char *kind = NULL;
        char *cmd = ui_copy_last_command(msgs, &kind);
        if (!cmd) add_notice(u, "info", "No command to copy.");
        else {
            copy_text(u, cmd);
            add_notice(u, "info", str_eq(kind, "bash") ? "Copied bash command to clipboard" : "Copied eval code to clipboard");
        }
        free(cmd);
    } else if (str_eq(a, "link") || str_eq(a, "url")) {
        char *href = ui_copy_last_link(msgs);
        if (!href) add_notice(u, "info", "No link to copy.");
        else {
            copy_text(u, href);
            add_notice(u, "info", "Copied link to clipboard");
        }
        free(href);
    } else if (str_eq(a, "local") && u->app->env && u->app->env->local_dir) {
        /* the session's local:// directory, created first so the copied path can be entered (omp 7cc6ba48cc) */
        const char *root = u->app->env->local_dir;
        fs_mkdir_p(root, 0755);
        copy_text(u, root);
        char *sp = tc_shorten_path(root), *m = xasprintf("Copied %s to clipboard", sp);
        add_notice(u, "info", m);
        free(m), free(sp);
    } else {
        add_notice(u, "info", "Usage: /copy [code|cmd|link|local]");
    }
    json_free(msgs);
    free(a);
}

void cc_open_command(ui_t *u, const char *arg) {
    char *a = lower_arg(arg);
    if (*a && !str_eq(a, "link") && !str_eq(a, "url")) {
        char *kr = ui_key_hint("right"), *ko = ui_key_hint("o");
        char *m = xasprintf("Usage: /open [link]  (pick a specific link: /copy, %s blocks, %s)", kr, ko);
        add_notice(u, "info", m);
        free(m), free(kr), free(ko);
        free(a);
        return;
    }
    free(a);
    json_t *msgs = messages(u);
    char *href = ui_copy_last_link(msgs);
    json_free(msgs);
    if (!href) {
        add_notice(u, "info", "No link to open.");
        return;
    }
    ui_open_in_browser(href);
    char *m = xasprintf("Opening %s", href);
    add_notice(u, "info", m);
    free(m);
    free(href);
}

void cc_shutdown(ui_t *u) {
    sel_close(u);
    free(g.content), free(g.label), free(g.href);
    memset(&g, 0, sizeof g);
}
