/* extracmds.c - see extracmds.h */
#include "coding/extracmds.h"

#include "coding/changelog.h"
#include "coding/skilldesc.h"
#include "coding/slashcmd.h"
#include "config/settings.h"
#include "prompt/template.h"
#include "session/session.h"

#include <ctype.h>

/* ---------- /force ---------- */

static bool tool_active(app_t *a, const char *name) {
    bool found = false;
    pthread_mutex_lock(&a->agent->mu);
    for (size_t i = 0; i < a->agent->tools.n && !found; i++) found = str_eq(a->agent->tools.v[i]->name, name);
    pthread_mutex_unlock(&a->agent->mu);
    return found;
}

/* buildNamedToolChoice */
json_t *extracmd_named_choice(const ai_model_t *m, const char *tool) {
    if (!m || !m->api) return NULL;
    const char *api = m->api;
    json_t *c = NULL;
    if (str_eq(api, "anthropic-messages") || str_eq(api, "bedrock-converse-stream")) {
        c = json_obj();
        json_set_str(c, "type", "tool");
    } else if (str_eq(api, "openai-codex-responses") || str_eq(api, "openai-responses") ||
               str_eq(api, "openai-completions") || str_eq(api, "azure-openai-responses") || str_eq(api, "openrouter")) {
        /* forcing a choice the transport would drop or turn into "auto" is not reported as forced */
        if (!model_compat_bool(m, "supportsToolChoice", true) || !model_compat_bool(m, "supportsForcedToolChoice", true))
            return NULL;
        c = json_obj();
        json_set_str(c, "type", "function");
    } else if (str_eq(api, "ollama-chat")) {
        c = json_obj();
        json_set_str(c, "type", "function");
    } else if (str_eq(api, "google-generative-ai") || str_eq(api, "google-gemini-cli") || str_eq(api, "google-vertex")) {
        return json_str("required"); /* a string choice: not a named one */
    }
    if (c) json_set_str(c, "name", tool);
    return c;
}

int extracmd_force_tool(app_t *a, const char *tool, char **err) {
    *err = NULL;
    if (!tool_active(a, tool)) {
        *err = xasprintf("Tool \"%s\" is not currently active.", tool);
        return -1;
    }
    json_t *c = extracmd_named_choice(a->model, tool);
    if (!c || !json_is_obj(c)) {
        json_free(c);
        *err = xstrdup("Current model does not support forcing a specific tool.");
        return -1;
    }
    pthread_mutex_lock(&a->mu);
    json_free(a->forced_choice);
    a->forced_choice = c;
    a->forced_step = 1;
    pthread_mutex_unlock(&a->mu);
    return 0;
}

void extracmd_prepare_request(app_t *a, ai_stream_opts_t *o) {
    static json_t *none;
    pthread_mutex_lock(&a->mu);
    if (a->forced_step == 1 && a->forced_choice) {
        /* the named choice stays owned by the app until the "none" step is served */
        o->tool_choice = a->forced_choice;
        a->forced_step = 2;
    } else if (a->forced_step == 2) {
        if (!none) none = json_str("none");
        o->tool_choice = none;
        a->forced_step = 0;
    }
    pthread_mutex_unlock(&a->mu);
}

char *extracmd_force_description(app_t *a) {
    size_t n = a->agent->tools.n;
    return n ? xasprintf("Force: %zu active tools", n) : xstrdup("Force: no active tools");
}

/* ---------- /skillful ---------- */

bool extracmd_skillful(void) { return settings_bool("skillful", true); }

static bool skillful_announced(app_t *a) {
    bool found = false;
    json_t *m;
    pthread_mutex_lock(&a->agent->mu);
    json_arr_foreach(a->agent->messages, i, m) found = found || str_eq(json_get_str(m, "customType"), "skillful-notice");
    pthread_mutex_unlock(&a->agent->mu);
    return found;
}

/* #applySkillful */
static void apply_skillful(app_t *a, bool enabled) {
    if (json_len(a->agent->messages) == 0) {
        app_rebuild_system_prompt(a);
        return;
    }
    if (!enabled) return; /* the provider-visible prompt stays byte-stable until the next session */
    bool reader = agent_find_tool(a->agent, "read") != NULL;
    if (!reader || a->disc.skills.n == 0 || skillful_announced(a)) return;
    json_t *list = json_arr();
    for (size_t i = 0; i < a->disc.skills.n; i++) {
        const skill_t *s = &a->disc.skills.v[i];
        if (s->hide) continue;
        json_t *e = json_obj();
        json_set_str(e, "name", s->name);
        char *d = skilldesc_render(a->skilldesc, s->name, s->description);
        json_set_str(e, "description", d ? d : s->description ? s->description : "");
        free(d);
        json_push(list, e);
    }
    json_t *ctx = json_obj();
    json_set(ctx, "skills", list);
    char *body = prompt_render("coding-agent/prompts/system/skillful-notice.md", ctx);
    json_free(ctx);
    if (!body) return;
    /* sendCustomMessage({customType: "skillful-notice", display: false}, {deliverAs: "nextTurn"}) */
    json_t *m = msg_user_text(body);
    json_set_str(m, "customType", "skillful-notice");
    json_set_bool(m, "synthetic", true);
    if (a->session) session_append_custom_message(a->session, "skillful-notice", body, false);
    agent_append_message(a->agent, m);
    free(body);
}

bool extracmd_set_skillful(app_t *a, int mode) {
    bool cur = extracmd_skillful();
    bool want = mode < 0 ? !cur : mode == 1;
    if (want == cur) return want;
    settings_set_runtime("skillful", json_bool(want));
    apply_skillful(a, want);
    return want;
}

/* ---------- text-mode handlers (builtin `handle`) ---------- */

static void out(slash_rt_t *rt, const char *text) {
    if (rt->output && text && *text) rt->output(rt->ud, text);
}

static char *lower_trim(const char *s) {
    char *t = str_trim(s ? s : "");
    for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
    return t;
}

void slash_changelog_text(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    changelog_view_t v;
    char *err = NULL;
    if (!changelog_parse_view(c->args, &v, &err)) {
        out(rt, err);
        res->failed = true;
        free(err);
        return;
    }
    char *md = changelog_markdown(&v, NULL);
    out(rt, md ? md : "No changelog entries found.");
    free(md);
}

void slash_force_text(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    const char *args = c->args;
    const char *sp = strchr(args, ' ');
    char *tool = sp ? xstrndup(args, (size_t)(sp - args)) : xstrdup(args);
    char *prompt = sp ? str_trim(sp + 1) : xstrdup("");
    char *err = NULL;
    if (!*tool) {
        out(rt, "Usage: /force:<tool-name> [prompt]");
        res->failed = true;
    } else if (extracmd_force_tool(rt->app, tool, &err) != 0) {
        out(rt, err);
        res->failed = true;
    } else {
        char *m = xasprintf("Next turn forced to use %s.", tool);
        out(rt, m);
        free(m);
        if (*prompt) {
            res->kind = SLASH_PROMPT;
            res->prompt = prompt;
            prompt = NULL;
        }
    }
    free(err);
    free(tool);
    free(prompt);
}

void slash_skillful_text(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *arg = lower_trim(c->args);
    if (str_eq(arg, "status")) {
        char *m = xasprintf("Skill listing: %s (session override; default from the skillful setting).",
                            extracmd_skillful() ? "on" : "off");
        out(rt, m);
        free(m);
    } else if (!*arg || str_eq(arg, "toggle") || str_eq(arg, "on") || str_eq(arg, "off")) {
        bool on = extracmd_set_skillful(rt->app, str_eq(arg, "on") ? 1 : str_eq(arg, "off") ? 0 : -1);
        out(rt, on ? "Skill listing enabled for this session." : "Skill listing disabled for this session.");
    } else {
        out(rt, "Usage: /skillful [on|off|status]");
        res->failed = true;
    }
    free(arg);
}
