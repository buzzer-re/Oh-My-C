/* extractl.c - TUI handlers (`handleTui`) of the WP-28 commands: /changelog (CommandController
 * handleChangelogCommand), /setup [providers] (runProviderSetupWizard), /force (builtin-control.ts),
 * /skillful (builtin-modes.ts). The session logic is shared with RPC/ACP in coding/extracmds.c. */
#include "modes/interactive.h"

#include "coding/changelog.h"
#include "coding/extracmds.h"

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

bool xc_command(ui_t *u, const char *cmd, const char *name, const char *arg) {
    if (str_eq(cmd, "changelog")) changelog_command(u, arg);
    else if (str_eq(cmd, "force")) force_command(u, arg);
    else if (str_eq(cmd, "skillful")) skillful_command(u, arg);
    else if (str_eq(cmd, "setup")) setup_command(u, name, arg);
    else return false;
    return true;
}

char *xc_description(ui_t *u, const char *name) {
    if (str_eq(name, "force")) return extracmd_force_description(u->app);
    if (str_eq(name, "skillful")) return xasprintf("Skill listing: %s", extracmd_skillful() ? "on" : "off");
    return NULL;
}
