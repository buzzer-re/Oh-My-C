/* slash.c - slash-command autocomplete metadata: upstream registry order, type-indicator icons and
 * (live) descriptions from the shared registry (coding/slashcmd.c). Only commands omc implements are
 * offered. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include "coding/slashcmd.h"

#include "coding/advisor.h"

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
    if (!strcmp(name, "effort")) /* configuredThinkingLevel(): none for a model without a reasoning dial */
        return xasprintf("Thinking: %s", a->model && a->model->reasoning ? effort_name(a->thinking) : "model default");
    if (!strcmp(name, "clear")) return xstrdup(u->worker_running ? "Clear: unavailable while streaming" : "Clear: drop context, keep session");
    if (!strcmp(name, "compact") || !strcmp(name, "context")) {
        json_t *st = app_stats(a);
        int64_t win = json_get_int(st, "contextWindow", 0), tok = json_get_int(st, "contextTokens", 0);
        double pct = json_get_num(st, "contextPercent", 0);
        json_free(st);
        if (win <= 0) return xstrdup(!strcmp(name, "compact") ? "Compact: context unavailable" : "Context: unavailable");
        if (!strcmp(name, "compact")) return xasprintf("Compact: context %d%% used", (int)lround(pct));
        char *t = slash_locale_int(tok), *w = slash_locale_int(win);
        char *s = xasprintf("Context: %d%% (%s/%s)", (int)lround(pct), t, w);
        free(t);
        free(w);
        return s;
    }
    if (!strcmp(name, "todo")) {
        int pending = 0, progress = 0, done = 0, total = 0;
        json_t *ph, *t;
        json_arr_foreach(a->env->todo.phases, i, ph) json_arr_foreach(json_get_arr(ph, "tasks"), k, t) {
            const char *st = json_get_str(t, "status");
            total++;
            if (str_eq(st, "pending")) pending++;
            else if (str_eq(st, "in_progress")) progress++;
            else if (str_eq(st, "completed")) done++;
        }
        if (!total) return xstrdup("Todos: none");
        return xasprintf("Todos: %d open (%d in progress, %d done)", pending + progress, progress, done);
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
            char *d = !strcmp(lv.v[i], effort_name(u->app->thinking)) ? xasprintf("%s (current)", desc) : xstrdup(desc);
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

void ui_slash_register(ui_t *u, tui_ac_default_t *ac) {
    for (size_t i = 0; i < slash_count(); i++) {
        const slash_cmd_t *c = slash_at(i);
        char *d = slash_format_description(c->description, ui_key_hint);
        char *icon = command_icon(c->icon);
        tui_ac_default_add_command_icon(ac, c->name, d, icon);
        for (size_t k = 0; k < c->aliases.n; k++) tui_ac_default_add_alias(ac, c->name, c->aliases.v[k]);
        free(icon);
        free(d);
    }
    ac->describe = live_description;
    ac->describe_ud = u;
    ac->args = slash_args;
    ac->inline_hint = slash_inline_hint;
    ac->args_ud = u;
}
