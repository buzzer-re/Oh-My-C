#include "app.h"

#include "config/auth.h"
#include "config/settings.h"
#include "core/fs.h"
#include "coding/ttsr.h"
#include "lsp/lsp.h"
#include "mcp/mcp.h"
#include "prompt/template.h"

#include <ctype.h>
#include <math.h>

/* ---------- model resolution ---------- */

const ai_model_t *app_resolve_model_pattern(const char *pattern, ai_effort_t *effort_out, bool *has_effort) {
    if (has_effort) *has_effort = false;
    if (!pattern || !*pattern) return NULL;
    char *p = xstrdup(pattern);
    if (p[0] == '@') memmove(p, p + 1, strlen(p));
    /* role reference: modelRoles.<role> */
    char *rk = xasprintf("modelRoles.%s", p);
    const char *role = settings_str(rk, NULL);
    free(rk);
    if (role && strcmp(role, pattern) != 0) {
        const ai_model_t *m = app_resolve_model_pattern(role, effort_out, has_effort);
        free(p);
        return m;
    }
    char *colon = strrchr(p, ':');
    if (colon) {
        ai_effort_t e;
        if (effort_parse(colon + 1, &e) == 0) {
            *colon = 0;
            if (effort_out) *effort_out = e;
            if (has_effort) *has_effort = true;
        }
    }
    const ai_model_t *m = catalog_resolve(p);
    free(p);
    return m;
}

char *app_api_key_for(const ai_model_t *m) { return m ? auth_get_api_key(m->provider) : NULL; }

const ai_model_t *app_default_model(void) {
    const char *def = settings_str("modelRoles.default", NULL);
    if (def) {
        const ai_model_t *m = app_resolve_model_pattern(def, NULL, NULL);
        if (m) return m;
    }
    static const char *order[] = {"anthropic", "openai", "openai-codex", "google", "openrouter", "xai", "groq",
                                  "deepseek", "mistral", "cerebras", "zai", "ollama"};
    for (size_t i = 0; i < ARRAY_LEN(order); i++) {
        if (!auth_has_credentials(order[i])) continue;
        const char *dm = catalog_provider_default_model(order[i]);
        const ai_model_t *m = dm ? catalog_find(order[i], dm) : NULL;
        if (m) return m;
    }
    /* any provider with credentials */
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (!auth_has_credentials(m->provider)) continue;
        const char *dm = catalog_provider_default_model(m->provider);
        const ai_model_t *d = dm ? catalog_find(m->provider, dm) : NULL;
        return d ? d : m;
    }
    return NULL;
}

char *app_model_label(app_t *a) {
    return a->model ? xasprintf("%s/%s", a->model->provider, a->model->id) : xstrdup("(no model)");
}

const char *app_session_file(app_t *a) { return a->session ? a->session->file : NULL; }

/* ---------- persistence listener ---------- */

static void notice(app_t *a, const char *level, const char *fmt, ...) PRINTF_LIKE(3, 4);
static void notice(app_t *a, const char *level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = xvasprintf(fmt, ap);
    va_end(ap);
    LOGI("[%s] %s", level, s);
    if (a->notice) a->notice(a->notice_ud, level, s);
    free(s);
}

static void maybe_title(app_t *a);

static void on_agent_event(void *ud, const agent_event_t *ev) {
    app_t *a = ud;
    if (ev->type == AGENT_EV_TURN_START || ev->type == AGENT_EV_MESSAGE_START) {
        if (ev->type == AGENT_EV_TURN_START || str_eq(json_get_str(ev->message, "role"), "assistant")) ttsr_reset(a->ttsr);
    }
    if (ev->type == AGENT_EV_MESSAGE_END && ev->message) {
        const char *role = json_get_str(ev->message, "role");
        /* TTSR discard mode: the interrupted partial response is dropped, never persisted */
        if (a->ttsr_interrupting && str_eq(role, "assistant") && str_eq(json_get_str(ev->message, "stopReason"), "aborted"))
            return;
        if (str_eq(role, "assistant") && !str_eq(json_get_str(ev->message, "stopReason"), "error")) {
            char *def = ttsr_take_deferred(a->ttsr);
            if (def) {
                json_t *m = msg_user_text(def);
                json_set_bool(m, "synthetic", true);
                agent_follow_up(a->agent, m);
                free(def);
            }
        }
        if (role && a->session) session_append_message(a->session, ev->message);
        if (str_eq(role, "assistant")) {
            json_t *u = json_get(ev->message, "usage");
            a->total_cost += json_get_num(json_get(u, "cost"), "total", 0);
            int64_t ctx = json_get_int(u, "input", 0) + json_get_int(u, "cacheRead", 0) + json_get_int(u, "cacheWrite", 0) +
                          json_get_int(u, "output", 0);
            if (ctx > 0) a->last_context_tokens = ctx;
        }
    }
}

/* ---------- hooks ---------- */

static void hook_prepare(void *ud, agent_t *ag, ai_stream_opts_t *o) {
    app_t *a = ud;
    free(ag->api_key);
    ag->api_key = app_api_key_for(a->model);
    o->api_key = ag->api_key;
    o->cache_retention = settings_str("providers.cacheRetention", "short");
    json_t *tb = settings_get("thinkingBudgets");
    if (json_is_obj(tb)) o->thinking_budgets = tb;
    double temp = settings_num("temperature", NAN);
    if (!isnan(temp)) o->temperature = temp;
    o->text_verbosity = settings_str("textVerbosity", NULL);
    const char *tier = settings_str("tier.openai", NULL);
    if (tier) o->service_tier = tier;
}

static bool is_overflow(const char *err) {
    return err && (str_istr(err, "prompt is too long") || str_istr(err, "context length") || str_istr(err, "maximum context") ||
                   str_istr(err, "context_length_exceeded") || str_istr(err, "too many tokens") ||
                   str_istr(err, "exceeds the context window") || str_istr(err, "input is too long"));
}

static bool is_retryable(const char *err) {
    if (!err) return false;
    const char *pats[] = {"overloaded", "rate limit", "rate_limit", "429", "500", "502", "503", "504", "529",
                          "timed out", "timeout", "connection", "network", "reset", "closed before", "mid-body",
                          "internal server error", "service unavailable", "bad gateway", "temporarily", "try again",
                          "stream ended", "terminated"};
    for (size_t i = 0; i < ARRAY_LEN(pats); i++)
        if (str_istr(err, pats[i])) return true;
    return false;
}

static int hook_stream(void *ud, agent_t *ag, const ai_event_t *ev) {
    app_t *a = ud;
    UNUSED(ag);
    if (ttsr_on_event(a->ttsr, ev)) {
        a->ttsr_interrupting = true;
        return 1;
    }
    return 0;
}

static bool cmd_mutates(const char *cmd) {
    static const char *const pats[] = {"rm ", "rm\t", "rmdir", "mv ", "cp ", "mkdir", "touch ", "chmod", "chown", "ln ",
                                       "git commit", "git push", "git checkout", "git reset", "git merge", "git rebase",
                                       "git add", "git rm", "git mv", "git stash", "git apply", "git switch", "git restore",
                                       "npm install", "npm i ", "pnpm add", "pnpm install", "yarn add", "pip install",
                                       "cargo add", "go get", "apt ", "brew install", "sed -i", "tee ", "truncate",
                                       "dd ", "make install", "patch "};
    for (size_t i = 0; i < ARRAY_LEN(pats); i++)
        if (strstr(cmd, pats[i])) return true;
    /* output redirection to a file (allow 2>&1 and >/dev/null) */
    for (const char *p = strchr(cmd, '>'); p; p = strchr(p + 1, '>')) {
        const char *t = p + 1;
        if (*t == '>') t++;
        while (*t == ' ') t++;
        if (*t == '&' || str_starts(t, "/dev/null")) continue;
        if (p > cmd && p[-1] == '2' && str_starts(t, "/dev/")) continue;
        return true;
    }
    return false;
}

static tool_gate_t hook_before_tool(void *ud, agent_t *ag, const json_t *call, char **reason) {
    app_t *a = ud;
    UNUSED(ag);
    if (!a->plan_mode) return TOOL_ALLOW;
    const char *name = json_get_str(call, "name");
    json_t *args = json_get(call, "arguments");
    if (str_eq(name, "write")) {
        const char *p = json_get_str(args, "path");
        if (p && (str_starts(p, "local://") || !strcmp(p, "xd://propose"))) return TOOL_ALLOW;
        *reason = xstrdup("Plan mode is active: the working tree is read-only. Write plans to local://<slug>-plan.md and "
                          "propose with xd://propose.");
        return TOOL_BLOCK;
    }
    if (str_eq(name, "edit")) {
        const char *in = json_get_str(args, "input");
        const char *p = json_get_str(args, "path");
        bool ok = p && str_starts(p, "local://");
        if (in && !p) {
            ok = true;
            bool any = false;
            for (const char *s2 = in; (s2 = strchr(s2, '[')); s2++) {
                if (s2 != in && s2[-1] != '\n') continue;
                const char *h = strchr(s2, '#'), *e = strchr(s2, ']');
                if (!h || !e || h > e) continue;
                char *hp = xstrndup(s2 + 1, (size_t)(h - s2 - 1));
                char *abs = tool_resolve_path(a->env, hp);
                if (!a->env->local_dir || !str_starts(abs, a->env->local_dir)) ok = false;
                any = true;
                free(abs);
                free(hp);
            }
            if (!any) ok = false;
        }
        if (ok) return TOOL_ALLOW;
        *reason = xstrdup("Plan mode is active: only the plan file (local://) may be edited.");
        return TOOL_BLOCK;
    }
    if (str_eq(name, "bash")) {
        const char *c = json_get_str(args, "command");
        if (c && cmd_mutates(c)) {
            *reason = xasprintf("Plan mode is active: refusing a state-changing command (`%s`). Explore read-only.", c);
            return TOOL_BLOCK;
        }
    }
    return TOOL_ALLOW;
}

static char *app_propose_plan(tool_env_t *env, const char *content, bool *is_error) {
    app_t *a = env->session;
    *is_error = false;
    if (!a->plan_mode) {
        *is_error = true;
        return xstrdup("Not in plan mode.");
    }
    char *slug = str_trim(content);
    char *nl = strchr(slug, '\n');
    if (nl) *nl = 0;
    for (char *p = slug; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) *p = '-';
    char *url = xasprintf("local://%s-plan.md", slug);
    char *abs = tool_resolve_path(env, url);
    size_t n;
    char *plan = fs_read(abs, &n);
    if (!plan) {
        *is_error = true;
        char *r = xasprintf("Plan file %s not found; write it first, then propose the same slug.", url);
        free(url);
        free(abs);
        free(slug);
        return r;
    }
    bool approved = true;
    if (env->ui.ask) {
        json_t *qs = json_parse_str("[{\"id\":\"plan\",\"question\":\"Approve the plan?\",\"options\":["
                                    "{\"label\":\"Approve and execute\"},{\"label\":\"Keep planning\"}]}]");
        char *q = xasprintf("Approve plan %s?", url);
        json_set_str(json_at(qs, 0), "question", q);
        free(q);
        json_t *ans = env->ui.ask(env->ui.ud, qs);
        approved = ans && str_eq(json_get_str(ans, "plan"), "Approve and execute");
        json_free(ans);
        json_free(qs);
    }
    char *r;
    if (!approved) {
        r = xasprintf("User chose to keep planning. Refine %s and propose again when ready.", url);
    } else {
        free(a->plan_file);
        a->plan_file = xstrdup(url);
        app_set_plan_mode(a, false);
        json_t *ctx = json_obj();
        json_set_bool(ctx, "contextPreserved", true);
        json_set_str(ctx, "planFilePath", url);
        json_set_str(ctx, "planContent", plan);
        json_t *tl = json_arr();
        for (size_t i = 0; i < a->agent->tools.n; i++) json_push_str(tl, a->agent->tools.v[i]->name);
        json_set(ctx, "tools", tl);
        char *msg = prompt_render("coding-agent/prompts/system/plan-mode-approved.md", ctx);
        json_free(ctx);
        json_t *m = msg_user_text(msg);
        json_set_bool(m, "synthetic", true);
        agent_follow_up(a->agent, m);
        free(msg);
        notice(a, "info", "Plan approved: %s — executing", url);
        r = xasprintf("Plan %s approved. Plan mode ended; full tool access restored. Execute it now.", url);
    }
    free(plan);
    free(url);
    free(abs);
    free(slug);
    return r;
}

void app_set_plan_mode(app_t *a, bool on) {
    a->plan_mode = on;
    a->env->propose_plan = on ? app_propose_plan : NULL;
    if (a->session) {
        json_t *e = json_obj();
        json_set_str(e, "type", "mode_change");
        json_set_str(e, "mode", on ? "plan" : "none");
        session_append_entry(a->session, e);
    }
    app_rebuild_system_prompt(a);
}

static void hook_after_tool(void *ud, agent_t *ag, const json_t *call, tool_result_t *res) {
    app_t *a = ud;
    UNUSED(ag);
    char *rem = ttsr_take_tool_reminder(a->ttsr, json_get_str(call, "id"));
    if (rem) {
        tool_result_text(res, rem);
        notice(a, "info", "Rule reminder attached to %s result", json_get_str(call, "name"));
        free(rem);
    }
}

static bool hook_should_retry(void *ud, agent_t *ag, const json_t *assistant, int attempt) {
    app_t *a = ud;
    if (attempt < 0) {
        json_t *names = NULL;
        char *inj = ttsr_take_interrupt(a->ttsr, &names);
        a->ttsr_interrupting = false;
        if (!inj) return false;
        sb_t nl = SB_INIT;
        json_t *x;
        json_arr_foreach(names, i, x) sb_appendf(&nl, "%s%s", i ? ", " : "", json_sv(x));
        notice(a, "warning", "Injecting rule: %s", sb_cstr(&nl));
        sb_free(&nl);
        json_t *m = json_obj();
        json_set_str(m, "role", "custom");
        json_set_str(m, "customType", "ttsr-injection");
        json_set_str(m, "content", inj);
        json_set_bool(m, "display", false);
        json_t *det = json_obj();
        json_set(det, "rules", names);
        json_set(m, "details", det);
        json_set_int(m, "timestamp", now_ms());
        /* the agent sends custom messages as user content (see session to_llm) */
        json_t *llm = msg_user_text(inj);
        json_set_bool(llm, "synthetic", true);
        agent_append_message(ag, llm);
        if (a->session) {
            json_t *e = json_obj();
            json_set_str(e, "type", "custom_message");
            json_set_str(e, "customType", "ttsr-injection");
            json_set_str(e, "content", inj);
            json_set_bool(e, "display", false);
            json_set(e, "details", json_clone(det));
            session_append_entry(a->session, e);
        }
        json_free(m);
        free(inj);
        return true;
    }
    const char *stop = json_get_str(assistant, "stopReason");
    if (str_eq(stop, "aborted") || ag->cancel) return false;
    const char *err = json_get_str(assistant, "errorMessage");
    if (is_overflow(err) && attempt == 0 && !a->compacting) {
        notice(a, "warning", "Context overflow; compacting and retrying");
        char *cerr = NULL;
        if (app_compact(a, NULL, &cerr) == 0) return true;
        notice(a, "error", "Compaction failed: %s", cerr ? cerr : "?");
        free(cerr);
        return false;
    }
    if (!settings_bool("retry.enabled", true)) return false;
    int max = (int)settings_int("retry.maxRetries", 3);
    if (attempt >= max || !is_retryable(err)) return false;
    int64_t base = settings_int("retry.baseDelayMs", 2000), maxd = settings_int("retry.maxDelayMs", 60000);
    int64_t delay = base << attempt;
    if (delay > maxd) delay = maxd;
    notice(a, "warning", "Retrying (%d/%d) in %llds: %s", attempt + 1, max, (long long)(delay / 1000), err ? err : "error");
    int64_t until = mono_ms() + delay;
    while (mono_ms() < until) {
        if (ag->cancel) return false;
        sleep_ms(50);
    }
    return true;
}

/* ---------- setup ---------- */

static char *app_after_write(tool_env_t *env, const char *abs) {
    app_t *a = env->session;
    if (!a || !a->lsp || !settings_bool("lsp.diagnosticsOnWrite", true)) return NULL;
    return lsp_after_write(a->lsp, abs, settings_int("lsp.diagnosticsWaitMs", 3000));
}

static void install_agent(app_t *a) {
    agent_t *ag = agent_new();
    a->agent = ag;
    ag->app = a;
    ag->max_turns = a->opts.max_turns;
    ag->hooks.prepare_request = hook_prepare;
    ag->hooks.should_retry = hook_should_retry;
    ag->hooks.on_stream_delta = hook_stream;
    ag->hooks.after_tool = hook_after_tool;
    ag->hooks.before_tool = hook_before_tool;
    ag->hooks.ud = a;
    free(ag->session_id);
    ag->session_id = a->session ? xstrdup(session_id(a->session)) : uuid_v7();
    agent_subscribe(ag, on_agent_event, a);
    agent_set_model(ag, a->model);
    ag->thinking = a->thinking;
    if (!a->opts.no_tools) tools_register_builtin(ag, a->env, a->opts.tools);
    if (a->mcp && !a->opts.no_tools) mcp_register_tools(a->mcp, ag);
    bool lsp_ok = !a->opts.no_tools && settings_bool("lsp.enabled", true) &&
                  (!a->opts.tools || strvec_contains(a->opts.tools, "lsp"));
    if (lsp_ok) {
        if (!a->lsp && a->opts.lsp) a->lsp = a->opts.lsp;
        if (!a->lsp) {
            a->lsp = lsp_manager_new(a->cwd);
            a->owns_lsp = true;
        }
        agent_add_tool(ag, tool_lsp_new(a->env, a->lsp));
        a->env->after_write = app_after_write;
    }
    if (a->opts.subagent && a->opts.subagent_depth >= (int)settings_int("task.maxRecursionDepth", 2))
        agent_remove_tool(ag, "task");
}

void app_rebuild_system_prompt(app_t *a) {
    strvec_t names = {0};
    for (size_t i = 0; i < a->agent->tools.n; i++) strvec_push(&names, a->agent->tools.v[i]->name);
    char *label = app_model_label(a);
    sysprompt_opts_t so = {.cwd = a->cwd,
                           .model_name = label,
                           .tool_names = &names,
                           .disc = &a->disc,
                           .custom_prompt = a->sys_custom,
                           .append_prompt = a->sys_append,
                           .subagent = a->opts.subagent};
    json_t *blocks = build_system_prompt(&so);
    if (a->plan_mode) {
        json_t *pc = json_obj();
        json_set_str(pc, "writeToolName", "write");
        json_set_str(pc, "editToolName", "edit");
        json_set_str(pc, "askToolName", "ask");
        json_set_bool(pc, "askAvailable", agent_find_tool(a->agent, "ask") != NULL);
        json_set_bool(pc, "taskAvailable", agent_find_tool(a->agent, "task") != NULL);
        json_set_bool(pc, "scoutAvailable", agent_find_tool(a->agent, "task") != NULL);
        json_set_bool(pc, "isHashlineEditMode", str_eq(a->env->edit_mode, "hashline"));
        json_set_bool(pc, "planExists", a->plan_file != NULL);
        if (a->plan_file) json_set_str(pc, "planFilePath", a->plan_file);
        char *pm = prompt_render("coding-agent/prompts/system/plan-mode-active.md", pc);
        json_push_str(blocks, pm);
        free(pm);
        json_free(pc);
    }
    char *mi = a->mcp ? mcp_instructions(a->mcp) : NULL;
    if (mi && *mi) json_push_str(blocks, mi);
    free(mi);
    agent_set_system_prompt_parts(a->agent, blocks);
    free(label);
    strvec_free(&names);
}

static void load_session_into_agent(app_t *a) {
    session_context_t sc;
    session_build_context(a->session, &sc);
    agent_replace_messages(a->agent, sc.messages);
    sc.messages = NULL;
    if (sc.model && !a->opts.model) {
        const ai_model_t *m = catalog_resolve(sc.model);
        if (m) {
            a->model = m;
            agent_set_model(a->agent, m);
        }
    }
    if (sc.thinking && !a->opts.thinking) {
        ai_effort_t e;
        if (effort_parse(sc.thinking, &e) == 0) {
            a->thinking = e;
            a->agent->thinking = e;
        }
    }
    session_context_free(&sc);
}

app_t *app_create(const app_opts_t *o, char **err) {
    *err = NULL;
    ai_providers_init();
    catalog_init();
    auth_init();
    app_t *a = xcalloc(1, sizeof(*a));
    pthread_mutex_init(&a->mu, NULL);
    a->opts = *o;
    a->cwd = o->cwd ? path_abs(o->cwd, NULL) : get_cwd();
    a->sys_custom = o->system_prompt ? xstrdup(o->system_prompt) : NULL;
    a->sys_append = o->append_system_prompt ? xstrdup(o->append_system_prompt) : NULL;
    /* model */
    ai_effort_t eff = EFFORT_OFF;
    bool has_eff = false;
    if (o->model) {
        char *pat = o->provider && !strchr(o->model, '/') ? xasprintf("%s/%s", o->provider, o->model) : xstrdup(o->model);
        a->model = app_resolve_model_pattern(pat, &eff, &has_eff);
        free(pat);
        if (!a->model) {
            *err = xasprintf("Unknown model: %s (see `omc models`)", o->model);
            app_free(a);
            return NULL;
        }
    } else if (o->provider) {
        const char *dm = catalog_provider_default_model(o->provider);
        a->model = dm ? catalog_find(o->provider, dm) : NULL;
        if (!a->model) {
            for (size_t i = 0; i < catalog_count() && !a->model; i++)
                if (!strcmp(catalog_at(i)->provider, o->provider)) a->model = catalog_at(i);
        }
    }
    if (o->api_key && (a->model || o->provider)) auth_set_runtime_key(a->model ? a->model->provider : o->provider, o->api_key);
    if (o->thinking) {
        if (effort_parse(o->thinking, &eff) != 0 && strcmp(o->thinking, "auto") != 0) {
            *err = xasprintf("Invalid thinking level: %s", o->thinking);
            app_free(a);
            return NULL;
        }
        has_eff = true;
    }
    if (!has_eff) effort_parse(settings_str("defaultThinkingLevel", "medium"), &eff);
    a->thinking = eff;
    /* environment & discovery */
    a->env = tool_env_new(a->cwd);
    a->env->settings = settings_effective();
    a->env->subagent_depth = o->subagent_depth;
    a->env->session = a;
    a->env->run_subagent = app_run_subagent;
    discover_all(a->cwd, &a->disc, o->no_context_files, o->no_skills, o->no_rules);
    a->env->disc = &a->disc;
    a->ttsr = ttsr_new(&a->disc);
    if (o->mcp) {
        a->mcp = o->mcp;
    } else if (!o->no_mcp && !o->no_tools && settings_bool("mcp.enabled", true)) {
        json_t *cfg = mcp_discover_config(a->cwd);
        if (json_len(cfg)) {
            a->mcp = mcp_manager_new();
            a->owns_mcp = true;
            mcp_connect_all(a->mcp, cfg, settings_int("mcp.startupTimeoutMs", 15000));
        }
        json_free(cfg);
    }
    /* session */
    if (o->no_session) {
        a->session = session_in_memory(a->cwd);
    } else if (o->resume) {
        char *path = session_resolve(o->resume, a->cwd);
        if (!path) {
            *err = xasprintf("Session not found: %s", o->resume);
            app_free(a);
            return NULL;
        }
        a->session = session_open(path, err);
        free(path);
        if (!a->session) {
            app_free(a);
            return NULL;
        }
    } else if (o->fork) {
        char *path = session_resolve(o->fork, a->cwd);
        session_t *src = path ? session_open(path, err) : NULL;
        free(path);
        if (!src) {
            if (!*err) *err = xasprintf("Session not found: %s", o->fork);
            app_free(a);
            return NULL;
        }
        a->session = session_fork(src, NULL, o->session_dir);
        session_free(src);
    } else if (o->continue_recent) {
        a->session = session_continue_recent(a->cwd, o->session_dir);
        if (!a->session) a->session = session_create(a->cwd, o->session_dir);
    } else {
        a->session = session_create(a->cwd, o->session_dir);
    }
    if (!a->model) {
        session_context_t sc;
        session_build_context(a->session, &sc);
        if (sc.model) a->model = catalog_resolve(sc.model);
        session_context_free(&sc);
    }
    if (!a->model) a->model = app_default_model();
    if (!a->model) {
        *err = xstrdup("No model available. Set an API key (e.g. ANTHROPIC_API_KEY or OPENAI_API_KEY), run `omc login`, "
                       "or pass --model provider/id with --api-key.");
        app_free(a);
        return NULL;
    }
    {
        const char *sid = session_id(a->session);
        if (a->session->dir) a->env->local_dir = xasprintf("%s/%s/local", a->session->dir, sid);
        else {
            char *base = omc_agent_path("local");
            a->env->local_dir = xasprintf("%s/%s", base, sid);
            free(base);
        }
    }
    install_agent(a);
    if (json_len(a->session->entries)) load_session_into_agent(a);
    app_rebuild_system_prompt(a);
    if (!json_len(a->session->entries) && a->session->persist) {
        char *label = app_model_label(a);
        session_append_model_change(a->session, label, NULL);
        session_append_thinking_change(a->session, effort_name(a->thinking));
        free(label);
    }
    a->title_done = a->session->title != NULL;
    if (settings_bool("plan.defaultOnStartup", false) && !o->subagent) app_set_plan_mode(a, true);
    return a;
}

void app_free(app_t *a) {
    if (!a) return;
    agent_free(a->agent);
    session_free(a->session);
    tool_env_free(a->env);
    ttsr_free(a->ttsr);
    if (a->owns_mcp) mcp_manager_free(a->mcp);
    if (a->owns_lsp) lsp_manager_free(a->lsp);
    discovery_free(&a->disc);
    free(a->cwd);
    free(a->sys_custom);
    free(a->sys_append);
    free(a->plan_file);
    pthread_mutex_destroy(&a->mu);
    free(a);
}

int app_set_model(app_t *a, const char *pattern, char **err) {
    ai_effort_t e;
    bool has_e;
    const ai_model_t *m = app_resolve_model_pattern(pattern, &e, &has_e);
    if (!m) {
        *err = xasprintf("Unknown model: %s", pattern);
        return -1;
    }
    a->model = m;
    agent_set_model(a->agent, m);
    if (has_e) app_set_thinking(a, e);
    char *label = app_model_label(a);
    session_append_model_change(a->session, label, NULL);
    free(label);
    app_rebuild_system_prompt(a);
    return 0;
}

void app_set_thinking(app_t *a, ai_effort_t e) {
    a->thinking = e;
    a->agent->thinking = e;
    session_append_thinking_change(a->session, effort_name(e));
}

void app_abort(app_t *a) { agent_abort(a->agent); }

/* ---------- one-shot completion ---------- */

char *app_complete_text(app_t *a, const ai_model_t *m, const char *system, const char *user, ai_effort_t effort) {
    if (!m) m = a->model;
    json_t *sys = json_arr();
    if (system) json_push_str(sys, system);
    json_t *msgs = json_arr();
    json_push(msgs, msg_user_text(user));
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    char *key = app_api_key_for(m);
    o.api_key = key;
    o.reasoning = model_clamp_effort(m, effort);
    o.disable_reasoning = effort == EFFORT_OFF;
    o.cancel = &a->agent->cancel;
    json_t *r = ai_complete(m, &ctx, &o);
    char *text = NULL;
    const char *stop = json_get_str(r, "stopReason");
    if (!str_eq(stop, "error") && !str_eq(stop, "aborted")) text = ai_message_text(r);
    else LOGW("one-shot completion failed: %s", json_get_str(r, "errorMessage"));
    json_free(r);
    json_free(sys);
    json_free(msgs);
    free(key);
    return text;
}

/* ---------- title ---------- */

static void maybe_title(app_t *a) {
    if (a->title_done || a->opts.subagent || env_truthy("PI_NO_TITLE") || env_truthy("OMC_NO_TITLE")) return;
    a->title_done = true;
    /* first user message */
    const char *first = NULL;
    json_t *m;
    json_arr_foreach(a->agent->messages, i, m) {
        if (!first && str_eq(json_get_str(m, "role"), "user")) {
            json_t *c = json_get(m, "content");
            if (json_is_str(c)) first = json_sv(c);
            else first = json_get_str(json_at(c, 0), "text");
        }
    }
    if (!first) return;
    char *sys = prompt_load_or("coding-agent/prompts/system/title-system.md",
                               "Generate a concise 3-6 word title for this conversation. Output only the title.");
    const ai_model_t *tm = NULL;
    const char *smol = settings_str("modelRoles.smol", NULL);
    if (smol) tm = app_resolve_model_pattern(smol, NULL, NULL);
    if (!tm) tm = a->model;
    char *snippet = xasprintf("%.2000s", first);
    char *title = app_complete_text(a, tm, sys, snippet, EFFORT_OFF);
    if (title) {
        char *t = str_trim(title);
        /* strip quotes / trailing punctuation, single line */
        char *nl = strchr(t, '\n');
        if (nl) *nl = 0;
        size_t n = strlen(t);
        while (n && (t[n - 1] == '.' || t[n - 1] == '"')) t[--n] = 0;
        char *s = t;
        while (*s == '"' || *s == '#' || *s == ' ') s++;
        if (*s) session_set_title(a->session, s, "auto");
        free(t);
    }
    free(title);
    free(snippet);
    free(sys);
}

/* ---------- compaction ---------- */

static int64_t estimate_tokens(const json_t *msg) {
    char *s = json_dump(json_get(msg, "content"), 0);
    int64_t n = (int64_t)strlen(s) / 4 + 4;
    free(s);
    return n;
}

static void serialize_for_summary(sb_t *b, const json_t *msg) {
    const char *role = json_get_str(msg, "role");
    json_t *c = json_get(msg, "content");
    if (str_eq(role, "user")) sb_append(b, "[User]: ");
    else if (str_eq(role, "assistant")) sb_append(b, "[Assistant]: ");
    else if (str_eq(role, "toolResult")) sb_appendf(b, "[Tool result %s]: ", json_get_str(msg, "toolName"));
    else sb_appendf(b, "[%s]: ", role ? role : "message");
    if (json_is_str(c)) {
        sb_append(b, json_sv(c));
    } else {
        json_t *blk;
        json_arr_foreach(c, i, blk) {
            const char *t = json_get_str(blk, "type");
            if (str_eq(t, "text")) {
                const char *tx = json_get_str(blk, "text");
                size_t n = strlen(tx);
                if (str_eq(role, "toolResult") && n > 4000) sb_appendf(b, "%.2000s\n…[%zu chars elided]…\n%s", tx, n - 4000, tx + n - 2000);
                else sb_append(b, tx);
            } else if (str_eq(t, "toolCall")) {
                char *args = json_dump(json_get(blk, "arguments"), 0);
                if (strlen(args) > 2000) args[2000] = 0;
                sb_appendf(b, "\n<tool_call name=\"%s\">%s</tool_call>", json_get_str(blk, "name"), args);
                free(args);
            } else if (str_eq(t, "image")) {
                sb_append(b, "[image]");
            }
        }
    }
    sb_append(b, "\n\n");
}

int app_compact(app_t *a, const char *instructions, char **err) {
    if (a->compacting) {
        *err = xstrdup("compaction already running");
        return -1;
    }
    a->compacting = true;
    int rc = -1;
    json_t *path = session_path_to(a->session, a->session->leaf_id);
    /* find message entries, pick cut point keeping ~keepRecentTokens at a user-message boundary */
    int64_t keep = settings_int("compaction.keepRecentTokens", 20000);
    ssize_t cut = -1;
    int64_t acc = 0;
    size_t n = json_len(path);
    for (size_t i = n; i > 0; i--) {
        json_t *e = json_at(path, i - 1);
        if (!str_eq(json_get_str(e, "type"), "message")) continue;
        json_t *m = json_get(e, "message");
        acc += estimate_tokens(m);
        if (acc >= keep && str_eq(json_get_str(m, "role"), "user")) {
            cut = (ssize_t)(i - 1);
            break;
        }
    }
    /* previous compaction summary on path */
    const char *prev_summary = NULL;
    ssize_t prev_idx = -1;
    for (size_t i = 0; i < n; i++)
        if (str_eq(json_get_str(json_at(path, i), "type"), "compaction")) {
            prev_summary = json_get_str(json_at(path, i), "summary");
            prev_idx = (ssize_t)i;
        }
    if (cut < 0) {
        /* keep only the last user turn */
        for (size_t i = n; i > 0 && cut < 0; i--) {
            json_t *e = json_at(path, i - 1);
            if (str_eq(json_get_str(e, "type"), "message") && str_eq(json_get_str(json_get(e, "message"), "role"), "user") &&
                (ssize_t)(i - 1) > prev_idx)
                cut = (ssize_t)(i - 1);
        }
    }
    if (cut <= prev_idx + 1) {
        *err = xstrdup("Nothing to compact");
        goto out;
    }
    sb_t conv = SB_INIT;
    sb_append(&conv, "<conversation>\n");
    size_t from = prev_idx >= 0 ? (size_t)prev_idx + 1 : 0;
    for (size_t i = from; i < (size_t)cut; i++) {
        json_t *e = json_at(path, i);
        if (str_eq(json_get_str(e, "type"), "message")) serialize_for_summary(&conv, json_get(e, "message"));
    }
    sb_append(&conv, "</conversation>\n\n");
    if (prev_summary) {
        sb_appendf(&conv, "<previous-summary>\n%s\n</previous-summary>\n\n", prev_summary);
        char *p = prompt_load_or("agent/compaction/prompts/compaction-update-summary.md", "Update the summary.");
        sb_append(&conv, p);
        free(p);
    } else {
        char *p = prompt_load_or("agent/compaction/prompts/compaction-summary.md", "Summarize the conversation.");
        sb_append(&conv, p);
        free(p);
    }
    if (instructions && *instructions) sb_appendf(&conv, "\n\nAdditional focus: %s", instructions);
    char *sys = prompt_load_or("agent/compaction/prompts/summarization-system.md", "Summarize.");
    const ai_model_t *cm = a->model;
    const char *cmodel = settings_str("modelRoles.compaction", NULL);
    if (cmodel) {
        const ai_model_t *x = app_resolve_model_pattern(cmodel, NULL, NULL);
        if (x) cm = x;
    }
    notice(a, "info", "Compacting context…");
    char *summary = app_complete_text(a, cm, sys, sb_cstr(&conv), EFFORT_LOW);
    sb_free(&conv);
    free(sys);
    if (!summary || !*summary) {
        *err = xstrdup("summarization request failed");
        free(summary);
        goto out;
    }
    const char *first_kept = json_get_str(json_at(path, (size_t)cut), "id");
    session_append_compaction(a->session, summary, first_kept, a->last_context_tokens, NULL);
    free(summary);
    load_session_into_agent(a);
    a->last_context_tokens = 0;
    notice(a, "info", "Context compacted");
    rc = 0;
out:
    json_free(path);
    a->compacting = false;
    return rc;
}

static void maybe_auto_compact(app_t *a) {
    if (!settings_bool("compaction.enabled", true) || !a->model) return;
    int64_t window = a->model->context_window;
    if (window <= 0) return;
    int64_t reserve = settings_int("compaction.reserveTokens", 16384);
    double pct = settings_num("compaction.thresholdPercent", 85);
    int64_t thresh = (int64_t)((double)window * pct / 100.0);
    int64_t abs_t = settings_int("compaction.thresholdTokens", 0);
    if (abs_t > 0) thresh = abs_t;
    if (window - reserve < thresh) thresh = window - reserve;
    if (a->last_context_tokens < thresh) return;
    char *err = NULL;
    if (app_compact(a, NULL, &err) != 0) {
        notice(a, "error", "Auto-compaction failed: %s", err ? err : "?");
        free(err);
    }
}

/* ---------- prompting ---------- */

/* omp `@path` mentions: inline existing files (bounded) after the prompt text */
static void add_mentions(app_t *a, const char *text, json_t *content) {
    size_t budget = 256u * 1024;
    strvec_t seen = {0};
    for (const char *p = text; (p = strchr(p, '@')); p++) {
        if (p > text && !isspace((unsigned char)p[-1]) && p[-1] != '(' && p[-1] != '"') continue;
        size_t n = 0;
        while (p[1 + n] && !isspace((unsigned char)p[1 + n]) && !strchr("\"'`),;", p[1 + n])) n++;
        while (n && strchr(".:!?", p[n])) n--; /* trailing punctuation */
        if (!n) continue;
        char *rel = xstrndup(p + 1, n);
        char *abs = path_abs(rel, a->cwd);
        if (fs_is_file(abs) && !strvec_contains(&seen, abs)) {
            size_t len;
            bool trunc = false;
            char *data = fs_read_limit(abs, MIN(budget, (size_t)(64u * 1024)), &len, &trunc);
            if (data && !fs_is_binary(data, len)) {
                char *t = xasprintf("<file path=\"%s\">\n%s%s\n</file>", rel, data, trunc ? "\n[…truncated]" : "");
                json_push(content, text_block(t));
                free(t);
                budget = budget > len ? budget - len : 0;
                strvec_push(&seen, abs);
            }
            free(data);
        }
        free(abs);
        free(rel);
        if (!budget) break;
    }
    strvec_free(&seen);
}

int app_prompt(app_t *a, const char *text, json_t *images) {
    json_t *content = json_arr();
    json_push(content, text_block(text));
    add_mentions(a, text, content);
    json_t *img;
    json_arr_foreach(images, i, img) json_push(content, json_clone(img));
    json_free(images);
    json_t *msgs = json_arr();
    json_push(msgs, msg_user_content(content));
    int rc = agent_prompt(a->agent, msgs);
    if (rc == 0) {
        maybe_title(a);
        maybe_auto_compact(a);
    }
    return rc;
}

int app_new_session(app_t *a) {
    session_t *ns = a->opts.no_session ? session_in_memory(a->cwd) : session_create(a->cwd, a->opts.session_dir);
    session_free(a->session);
    a->session = ns;
    agent_replace_messages(a->agent, json_arr());
    free(a->agent->session_id);
    a->agent->session_id = xstrdup(session_id(ns));
    char *label = app_model_label(a);
    session_append_model_change(ns, label, NULL);
    session_append_thinking_change(ns, effort_name(a->thinking));
    free(label);
    a->title_done = false;
    a->total_cost = 0;
    a->last_context_tokens = 0;
    json_free(a->env->todo.phases);
    a->env->todo.phases = json_arr();
    return 0;
}

int app_switch_session(app_t *a, const char *path, char **err) {
    session_t *s = session_open(path, err);
    if (!s) return -1;
    session_free(a->session);
    a->session = s;
    free(a->agent->session_id);
    a->agent->session_id = xstrdup(session_id(s));
    load_session_into_agent(a);
    a->title_done = s->title != NULL;
    return 0;
}

int app_fork_from(app_t *a, const char *entry_id, char **err) {
    session_t *ns = session_fork(a->session, entry_id, a->opts.session_dir);
    if (!ns) {
        *err = xstrdup("fork failed");
        return -1;
    }
    session_free(a->session);
    a->session = ns;
    load_session_into_agent(a);
    return 0;
}

json_t *app_stats(app_t *a) {
    json_t *o = json_obj();
    int64_t in = 0, out = 0, cr = 0, cw = 0;
    int user = 0, asst = 0, tools = 0;
    json_t *m;
    json_arr_foreach(a->agent->messages, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "user")) user++;
        else if (str_eq(role, "toolResult")) tools++;
        else if (str_eq(role, "assistant")) {
            asst++;
            json_t *u = json_get(m, "usage");
            in += json_get_int(u, "input", 0);
            out += json_get_int(u, "output", 0);
            cr += json_get_int(u, "cacheRead", 0);
            cw += json_get_int(u, "cacheWrite", 0);
        }
    }
    json_set_int(o, "userMessages", user);
    json_set_int(o, "assistantMessages", asst);
    json_set_int(o, "toolResults", tools);
    json_set_int(o, "input", in);
    json_set_int(o, "output", out);
    json_set_int(o, "cacheRead", cr);
    json_set_int(o, "cacheWrite", cw);
    json_set_num(o, "cost", a->total_cost);
    json_set_int(o, "contextTokens", a->last_context_tokens);
    json_set_int(o, "contextWindow", a->model ? a->model->context_window : 0);
    json_set_num(o, "contextPercent",
                 a->model && a->model->context_window ? 100.0 * (double)a->last_context_tokens / (double)a->model->context_window : 0);
    if (a->session->file) json_set_str(o, "sessionFile", a->session->file);
    json_set_str(o, "sessionId", session_id(a->session));
    return o;
}

/* ---------- subagents ---------- */

typedef struct {
    volatile sig_atomic_t *pc;
    agent_t *ag;
    volatile int done;
} watch_ctx_t;

static void *cancel_watch(void *x) {
    watch_ctx_t *w = x;
    while (!w->done) {
        if (*w->pc) {
            agent_abort(w->ag);
            break;
        }
        sleep_ms(50);
    }
    return NULL;
}

char *app_run_subagent(tool_env_t *env, const char *agent_name, const char *assignment, const char *context,
                       json_t **details, volatile sig_atomic_t *cancel) {
    app_t *parent = env->session;
    app_opts_t o = {0};
    o.cwd = env->cwd;
    char *label = app_model_label(parent);
    json_t *defs = agent_defs_load(env->cwd);
    json_t *def = json_get(defs, agent_name ? agent_name : "task");
    const char *role_model = settings_str("modelRoles.task", NULL);
    o.model = role_model ? role_model : label;
    const char *dm = json_get_str(def, "model");
    if (dm && dm[0] == '@') {
        char *rk = xasprintf("modelRoles.%s", dm + 1);
        if (settings_str(rk, NULL)) o.model = dm;
        free(rk);
    } else if (dm && catalog_resolve(dm)) {
        o.model = dm;
    }
    o.append_system_prompt = json_get_str(def, "body");
    o.mcp = parent->mcp;
    o.lsp = parent->lsp;
    strvec_t tools = {0};
    json_t *tl = json_get_arr(def, "tools"), *tn;
    json_arr_foreach(tl, i, tn) if (json_is_str(tn)) strvec_push(&tools, json_sv(tn));
    if (tools.n) o.tools = &tools;
    o.no_session = true;
    o.subagent = true;
    o.subagent_depth = env->subagent_depth + 1;
    o.no_context_files = false;
    char *err = NULL;
    app_t *child = app_create(&o, &err);
    free(label);
    strvec_free(&tools);
    json_free(defs);
    if (!child) {
        char *r = xasprintf("Failed to start subagent: %s", err ? err : "?");
        free(err);
        return r;
    }
    child->thinking = parent->thinking;
    child->agent->thinking = parent->thinking;
    sb_t p = SB_INIT;
    if (agent_name && *agent_name) sb_appendf(&p, "You are the `%s` subagent.\n\n", agent_name);
    if (context && *context) sb_appendf(&p, "<context>\n%s\n</context>\n\n", context);
    sb_append(&p, assignment ? assignment : "");
    /* propagate cancellation */
    volatile sig_atomic_t *pc = cancel;
    pthread_t watcher;
    watch_ctx_t wc = {pc, child->agent, 0};
    bool watching = pc && pthread_create(&watcher, NULL, cancel_watch, &wc) == 0;
    app_prompt(child, sb_cstr(&p), NULL);
    wc.done = 1;
    if (watching) pthread_join(watcher, NULL);
    sb_free(&p);
    const json_t *last = last_assistant(child->agent->messages);
    char *text = last ? ai_message_text(last) : xstrdup("");
    if (details) {
        *details = app_stats(child);
        json_set_str(*details, "agent", agent_name ? agent_name : "task");
        if (last && json_get_str(last, "errorMessage")) json_set_str(*details, "error", json_get_str(last, "errorMessage"));
    }
    parent->total_cost += child->total_cost;
    app_free(child);
    return text;
}
