/* slash.c - slash-command autocomplete metadata: upstream registry order, type-indicator icons and
 * (live) descriptions from the shared registry (coding/slashcmd.c). Only commands omc implements are
 * offered. */
#include "ui/overlays.h"
#include "ui/ui.h"
#include "tools/jobs.h"

#include <math.h>

#include "coding/slashcmd.h"

#include "coding/advisor.h"
#include "coding/modelctl.h"
#include "mcp/mcp.h"

#include <ctype.h>
#include <math.h>
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
    if (!strcmp(name, "todo")) { /* the session todo state */
        size_t pending = 0, active = 0, done = 0, total = 0;
        json_t *p, *t;
        pthread_mutex_lock(&a->env->mu);
        json_arr_foreach(a->env->todo.phases, i, p) json_arr_foreach(json_get(p, "tasks"), k, t) {
            const char *st = json_get_str(t, "status");
            total++;
            pending += str_eq(st, "pending");
            active += str_eq(st, "in_progress");
            done += str_eq(st, "completed");
        }
        pthread_mutex_unlock(&a->env->mu);
        if (!total) return xstrdup("Todos: none");
        return xasprintf("Todos: %zu open (%zu in progress, %zu done)", pending + active, active, done);
    }
    if (!strcmp(name, "jobs")) {
        json_t *snap = jobs_snapshot(a->env->jobs), *x;
        size_t running = 0, recent = 0;
        json_arr_foreach(snap, i, x) {
            if (str_eq(json_get_str(x, "status"), "running")) running++;
            else recent++;
        }
        json_free(snap);
        if (recent > 5) recent = 5;
        if (!running && !recent) return xstrdup("Jobs: none");
        return xasprintf("Jobs: %zu running, %zu recent", running, recent);
    }
    if (!strcmp(name, "context")) {
        if (!a->model || a->model->context_window <= 0) return xstrdup("Context: unavailable");
        int64_t used = ui_context_used(a), win = a->model->context_window;
        char *t = slash_locale_int(used), *w = slash_locale_int(win);
        char *r = xasprintf("Context: %.0f%% (%s/%s)", round((double)used / (double)win * 100), t, w);
        free(t);
        free(w);
        return r;
    }
    if (!strcmp(name, "goal")) {
        if (a->plan_mode) return xstrdup("Goal: blocked by plan mode");
        return app_goal(a) ? xstrdup(goal_enabled(app_goal(a)) ? "Goal: active" : "Goal: paused") : xstrdup("Goal: off");
    }
    if (!strcmp(name, "advisor")) {
        if (a->advisor) {
            json_t *st = advisor_stats(a->advisor), *m = json_get(st, "model");
            char *s = m ? xasprintf("Advisor: on (%s/%s)", json_get_str(m, "provider"), json_get_str(m, "id")) : xstrdup("Advisor: off");
            json_free(st);
            return s;
        }
        return xstrdup(a->advisor_enabled ? "Advisor: configured, no model" : "Advisor: off");
    }
    if (!strcmp(name, "fast")) return modelctl_fast_description(a);
    if (!strcmp(name, "slow")) return modelctl_slow_description(a);
    if (!strcmp(name, "extended-context")) return modelctl_extended_context_description();
    if (!strcmp(name, "effort")) /* configuredThinkingLevel(): none for a model without a reasoning dial */
        return xasprintf("Thinking: %s", slash_configured_thinking(a));
    if (!strcmp(name, "clear")) return xstrdup(u->worker_running ? "Clear: unavailable while streaming" : "Clear: drop context, keep session");
    if (!strcmp(name, "compact")) {
        json_t *st = app_stats(a);
        int64_t win = json_get_int(st, "contextWindow", 0);
        double pct = json_get_num(st, "contextPercent", 0);
        json_free(st);
        if (win <= 0) return xstrdup("Compact: context unavailable");
        return xasprintf("Compact: context %d%% used", (int)lround(pct));
    }
    if (!strcmp(name, "tools")) {
        size_t n = a->agent->tools.n;
        return n ? xasprintf("Tools: %zu active / %zu available", n, n) : xstrdup("Tools: none available");
    }
    if (!strcmp(name, "login")) return xstrdup("Login: choose provider");
    return NULL;
}

/* ---------- argument completion + inline hints (builtin-completions.ts) ---------- */

static char *lower_dup(const char *s) {
    char *l = xstrdup(s);
    for (char *p = l; *p; p++) *p = (char)tolower((unsigned char)*p);
    return l;
}

/* subcommandRemainderHint: the rest of the name, then its usage */
static char *sub_remainder(const slash_sub_t *sub, size_t typed) {
    char *h = xasprintf("%s%s%s", sub->name + MIN(typed, strlen(sub->name)), sub->usage ? " " : "", sub->usage ? sub->usage : "");
    if (!*h) {
        free(h);
        return NULL;
    }
    return h;
}

static int slash_args(void *ud, const char *name, const char *arg, tui_ac_result_t *out) {
    ui_t *u = ud;
    const slash_cmd_t *c = slash_lookup(name);
    if (!c) return 0;
    bool blank = true;
    for (const char *p = arg; *p; p++)
        if (!isspace((unsigned char)*p)) blank = false;
    if (!c->allow_args) return blank ? -1 : 0;
    if (strchr(arg, ' ')) return 0; /* past the subcommand */
    char *low = lower_dup(arg);
    if (!strcmp(c->name, "effort")) { /* buildEffortArgumentCompletions: the live selector list */
        strvec_t lv = {0};
        slash_effort_selectors(u->app, &lv);
        for (size_t i = 0; i < lv.n; i++) {
            if (strncmp(lv.v[i], low, strlen(low))) continue;
            const char *desc = "";
            for (size_t k = 0; k < c->nsubs; k++)
                if (!strcmp(c->subs[k].name, lv.v[i])) desc = c->subs[k].description;
            char *val = xasprintf("%s ", lv.v[i]);
            char *d = !strcmp(lv.v[i], slash_configured_thinking(u->app)) ? xasprintf("%s (current)", desc) : xstrdup(desc);
            tui_ac_result_add(out, val, lv.v[i], d);
            strvec_push(&out->ghosts, "");
            free(d);
            free(val);
        }
        strvec_free(&lv);
        free(low);
        return out->items.n ? 1 : 0;
    }
    if (!c->nsubs || (c->sub_optional && !*arg)) {
        free(low);
        return 0;
    }
    for (size_t i = 0; i < c->nsubs; i++) {
        if (strncmp(c->subs[i].name, low, strlen(low))) continue;
        char *val = xasprintf("%s ", c->subs[i].name);
        tui_ac_result_add(out, val, c->subs[i].name, c->subs[i].description);
        char *h = sub_remainder(&c->subs[i], strlen(low));
        strvec_push(&out->ghosts, h ? h : "");
        free(h);
        free(val);
    }
    free(low);
    return out->items.n ? 1 : 0;
}

static char *slash_inline_hint(void *ud, const char *name, const char *arg) {
    ui_t *u = ud;
    const slash_cmd_t *c = slash_lookup(name);
    if (!c) return NULL;
    while (*arg == ' ' || *arg == '\t') arg++;
    if (!strcmp(c->name, "effort")) { /* buildEffortInlineHint */
        if (!*arg || strchr(arg, ' ')) return NULL;
        char *low = lower_dup(arg), *h = NULL;
        strvec_t lv = {0};
        slash_effort_selectors(u->app, &lv);
        for (size_t i = 0; i < lv.n && !h; i++)
            if (!strncmp(lv.v[i], low, strlen(low)) && strcmp(lv.v[i], low)) h = xstrdup(lv.v[i] + strlen(low));
        strvec_free(&lv);
        free(low);
        return h;
    }
    if (c->nsubs) { /* buildSubcommandInlineHint */
        const char *sp = strchr(arg, ' ');
        if (!sp) {
            if (!*arg) {
                if (!c->sub_optional) return NULL;
                sb_t b = SB_INIT;
                sb_putc(&b, '[');
                for (size_t i = 0; i < c->nsubs; i++) sb_appendf(&b, "%s%s", i ? "|" : "", c->subs[i].name);
                sb_putc(&b, ']');
                return sb_detach(&b);
            }
            char *low = lower_dup(arg), *h = NULL;
            for (size_t i = 0; i < c->nsubs && !h; i++)
                if (!strncmp(c->subs[i].name, low, strlen(low))) h = sub_remainder(&c->subs[i], strlen(low));
            free(low);
            return h;
        }
        char *sub = xstrndup(arg, (size_t)(sp - arg));
        char *low = lower_dup(sub);
        free(sub);
        const slash_sub_t *found = NULL;
        for (size_t i = 0; i < c->nsubs; i++)
            if (!strcmp(c->subs[i].name, low)) found = &c->subs[i];
        free(low);
        if (!found || !found->usage) return NULL;
        const char *after = sp + 1;
        if (!*after) return xstrdup(found->usage);
        /* the usage words not covered by the typed ones */
        size_t typed = 0;
        bool in = false;
        for (const char *p = after; *p; p++) {
            bool ws = isspace((unsigned char)*p);
            if (!ws && !in) typed++;
            in = !ws;
        }
        const char *rest = found->usage;
        for (size_t k = 0; k < typed && rest; k++) {
            rest = strchr(rest, ' ');
            if (rest) rest++;
        }
        return rest && *rest ? xstrdup(rest) : NULL;
    }
    if (c->inline_hint && !*arg) return xstrdup(c->inline_hint); /* buildStaticInlineHint */
    return NULL;
}

/* argument hints show on one line (replaceTabs, CR/LF -> space) */
static char *one_line_hint(const char *h) {
    if (!h || !*h) return NULL;
    sb_t b = SB_INIT;
    for (const char *p = h; *p; p++) {
        if (*p == '\t') sb_append(&b, "   ");
        else if (*p == '\r' || *p == '\n') {
            while (p[1] == '\r' || p[1] == '\n') p++;
            sb_putc(&b, ' ');
        } else sb_putc(&b, *p);
    }
    return sb_detach(&b);
}

void ui_slash_register(ui_t *u, tui_ac_default_t *ac) {
    /* #buildPendingSlashCommands (builtins, MCP prompt commands, `skill:<name>`), then the file commands and
     * the prompt templates no earlier command (or alias) names (#rebuildSlashCommandAutocomplete) */
    for (size_t i = 0; i < slash_count(); i++) {
        const slash_cmd_t *c = slash_at(i);
        char *d = slash_format_description(c->description, ui_key_hint);
        char *icon = command_icon(c->icon);
        tui_ac_default_add_command_icon(ac, c->name, d, icon);
        for (size_t k = 0; k < c->aliases.n; k++) tui_ac_default_add_alias(ac, c->name, c->aliases.v[k]);
        free(icon);
        free(d);
    }
    app_t *a = u->app;
    if (a && a->mcp) {
        char *icon = command_icon("mcp");
        json_t *mc = mcp_prompt_commands(a->mcp), *c;
        json_arr_foreach(mc, i, c) {
            char *d = xasprintf("%s (bundled)", json_get_str(c, "description"));
            tui_ac_default_add_command_icon(ac, json_get_str(c, "name"), d, icon);
            free(d);
        }
        json_free(mc);
        free(icon);
    }
    if (a && settings_bool("skills.enableSkillCommands", true)) {
        char *icon = command_icon("skill");
        for (size_t i = 0; i < a->disc.skills.n; i++) {
            char *n = xasprintf("skill:%s", a->disc.skills.v[i].name);
            tui_ac_default_add_command_icon(ac, n, a->disc.skills.v[i].description, icon);
            free(n);
        }
        free(icon);
    }
    if (a) {
        char *icon = command_icon("prompt");
        for (size_t i = 0; i < a->file_cmds.n; i++) {
            const file_cmd_t *f = &a->file_cmds.v[i];
            char *h = one_line_hint(f->argument_hint);
            tui_ac_default_add_command_hint(ac, f->name, f->description, icon, h);
            free(h);
        }
        for (size_t i = 0; i < a->prompt_tpls.n; i++) {
            const prompt_tpl_t *t = &a->prompt_tpls.v[i];
            if (strvec_contains(&ac->commands, t->name) || strvec_contains(&ac->alias_names, t->name)) continue;
            tui_ac_default_add_command_icon(ac, t->name, t->description, icon);
        }
        free(icon);
    }
    ac->describe = live_description;
    ac->describe_ud = u;
    ac->args = slash_args;
    ac->inline_hint = slash_inline_hint;
    ac->args_ud = u;
}
