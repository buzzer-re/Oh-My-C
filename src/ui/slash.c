/* slash.c - slash-command autocomplete metadata: upstream registry order, type-indicator icons and
 * (live) descriptions from assets/tui/slash-commands.json (extracted from omp's builtin-*.ts by
 * build-tools/extract_slash_commands.py). Only commands omc implements are offered. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include "core/embed.h"
#include "config/settings.h"

/* SlashCommandIconName -> symbol key (theme-class.ts `get cmd()`) */
static const char *icon_key(const char *name) {
    static const char *const MAP[][2] = {
        {"model", "icon.model"},     {"plan", "icon.plan"},           {"prewalk", "icon.prewalk"},
        {"goal", "icon.goal"},       {"pause", "icon.pause"},         {"loop", "icon.loop"},
        {"session", "icon.session"}, {"jobs", "icon.job"},            {"gauge", "icon.throughput"},
        {"context", "icon.context"}, {"ghost", "icon.ghost"},         {"agents", "icon.agents"},
        {"branch", "icon.branch"},   {"tree", "icon.worktree"},       {"signIn", "icon.input"},
        {"signOut", "icon.output"},  {"advisor", "icon.advisor"},     {"host", "icon.host"},
        {"package", "icon.package"}, {"fast", "icon.fast"},           {"voice", "icon.mic"},
        {"tools", "icon.extensionTool"}, {"rule", "icon.extensionRule"}, {"skill", "icon.extensionSkill"},
        {"mcp", "icon.extensionMcp"}, {"pin", "icon.pin"},
    };
    for (size_t i = 0; i < ARRAY_LEN(MAP); i++)
        if (!strcmp(MAP[i][0], name)) return MAP[i][1];
    return NULL;
}

/* getSlashCommandTypeIcon: no icon column in the ascii preset */
static char *command_icon(const char *name) {
    if (!name || str_eq(tui_sym_preset(), "ascii")) return NULL;
    const char *k = icon_key(name);
    char *key = k ? xstrdup(k) : xasprintf("cmd.%s", name);
    const char *g = tui_sym(key);
    free(key);
    return g && *g ? xstrdup(g) : NULL;
}

static json_t *registry(void) {
    static json_t *reg;
    if (!reg) {
        size_t len = 0;
        const char *data = embed_get("assets/tui/slash-commands.json", &len);
        reg = data ? json_parse(data, len, NULL) : NULL;
        if (!reg) reg = json_arr();
    }
    return reg;
}

/* live description (getTuiAutocompleteDescription) for the commands omc implements */
static char *live_description(void *ud, const char *name) {
    ui_t *u = ud;
    if (!u || !u->app) return NULL;
    app_t *a = u->app;
    if (!strcmp(name, "model")) {
        char *m = a->model ? xasprintf("Model: %s/%s", a->model->provider, a->model->id) : xstrdup("Model: none selected");
        return m;
    }
    if (!strcmp(name, "plan")) {
        if (!settings_bool("plan.enabled", true)) return xstrdup("Plan: disabled in settings");
        if (a->plan_mode) {
            const char *pf = a->plan_file ? strrchr(a->plan_file, '/') : NULL;
            return pf ? xasprintf("Plan: on (%s)", pf + 1) : xstrdup("Plan: on");
        }
        if (app_goal(a) && goal_enabled(app_goal(a))) return xstrdup("Plan: blocked by goal mode");
        return xstrdup("Plan: off");
    }
    if (!strcmp(name, "plan-review")) return xstrdup(a->plan_mode ? "Plan review: available" : "Plan review: plan mode inactive");
    if (!strcmp(name, "goal")) {
        if (a->plan_mode) return xstrdup("Goal: blocked by plan mode");
        return app_goal(a) ? xstrdup(goal_enabled(app_goal(a)) ? "Goal: active" : "Goal: paused") : xstrdup("Goal: off");
    }
    return NULL;
}

void ui_slash_register(ui_t *u, tui_ac_default_t *ac, const char *const *names, const char *const *descs, size_t n) {
    json_t *reg = registry(), *e;
    bool *used = xcalloc(n ? n : 1, sizeof *used);
    /* upstream registry order first */
    json_arr_foreach(reg, i, e) {
        const char *name = json_get_str(e, "name");
        for (size_t k = 0; k < n; k++) {
            if (used[k] || !str_eq(names[k], name)) continue;
            used[k] = true;
            const char *d = json_get_str(e, "description");
            char *icon = command_icon(json_get_str(e, "icon"));
            tui_ac_default_add_command_icon(ac, names[k], d && *d ? d : descs[k], icon);
            free(icon);
        }
    }
    /* omc-only commands keep their order after the upstream ones */
    for (size_t k = 0; k < n; k++)
        if (!used[k]) tui_ac_default_add_command(ac, names[k], descs[k]);
    free(used);
    ac->describe = live_description;
    ac->describe_ud = u;
}
