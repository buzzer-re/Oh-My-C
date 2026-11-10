/* task tool: spawn subagents (parallel batch); agent definitions from bundled + user agents dirs */
#include "tools.h"

#include <ctype.h>
#include "coding/agents.h"

#include "coding/discovery.h"
#include "config/settings.h"
#include "core/embed.h"
#include "core/fs.h"
#include "prompt/template.h"

/* ---------- agent definitions ---------- */

static json_t *parse_agent_def(const char *text, const char *fallback_name) {
    const char *body;
    json_t *fm = frontmatter_parse(text, &body);
    json_t *d = json_obj();
    const char *name = json_get_str(fm, "name");
    json_set_str(d, "name", name ? name : fallback_name);
    json_set_str(d, "description", json_get_str(fm, "description") ? json_get_str(fm, "description") : "");
    json_t *tools = json_get(fm, "tools");
    json_t *tl = json_arr();
    if (json_is_str(tools)) {
        strvec_t parts = str_split(json_sv(tools), ",");
        for (size_t i = 0; i < parts.n; i++) {
            char *t = str_trim(parts.v[i]);
            if (*t) json_push_str(tl, t);
            free(t);
        }
        strvec_free(&parts);
    } else if (json_is_arr(tools)) {
        json_t *x;
        json_arr_foreach(tools, i, x) if (json_is_str(x)) json_push_str(tl, json_sv(x));
    }
    json_set(d, "tools", tl);
    if (json_get_str(fm, "model")) json_set_str(d, "model", json_get_str(fm, "model"));
    else if (json_is_arr(json_get(fm, "model"))) { /* parseModelList: a list joins like a comma pattern list */
        strvec_t v = {0};
        json_t *x;
        json_arr_foreach(json_get(fm, "model"), i, x) if (json_is_str(x) && *json_sv(x)) strvec_push(&v, json_sv(x));
        if (v.n) {
            char *j = strvec_join(&v, ",");
            json_set_str(d, "model", j);
            free(j);
        }
        strvec_free(&v);
    }
    /* prewalk / advisor: true = the default target, a string = a custom model pattern (parseAgentFields) */
    static const char *const FLAGS[] = {"prewalk", "advisor"};
    for (size_t i = 0; i < ARRAY_LEN(FLAGS); i++) {
        json_t *v = json_get(fm, FLAGS[i]);
        if (json_is_bool(v)) json_set_bool(d, FLAGS[i], v->u.b);
        else if (json_is_str(v)) {
            char *t = str_trim(json_sv(v));
            if (str_eq(t, "true") || str_eq(t, "false")) json_set_bool(d, FLAGS[i], str_eq(t, "true"));
            else if (*t) json_set_str(d, FLAGS[i], t);
            free(t);
        }
    }
    if (json_get_str(fm, "thinking-level")) json_set_str(d, "thinking", json_get_str(fm, "thinking-level"));
    char *b = tpl_render(body, NULL);
    json_set_str(d, "body", b);
    free(b);
    json_free(fm);
    return d;
}

static void scan_agents(json_t *out, const char *dir) {
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return;
    for (size_t i = 0; i < dl.n; i++) {
        if (dl.v[i].is_dir || !str_ends(dl.v[i].name, ".md")) continue;
        char *p = path_join(dir, dl.v[i].name);
        char *t = fs_read(p, NULL);
        if (t) {
            char *nm = xstrndup(dl.v[i].name, strlen(dl.v[i].name) - 3);
            json_t *d = parse_agent_def(t, nm);
            json_set_str(d, "source", p);
            json_set(out, json_get_str(d, "name"), d);
            free(nm);
            free(t);
        }
        free(p);
    }
    dirlist_free(&dl);
}


/* Bundled agents in upstream order (task/agents.ts EMBEDDED_AGENT_DEFS): scout, reviewer, security-reviewer, seance
 * from their markdown; `task` and `sonic` share the task.md body with synthesized frontmatter. Then user agents
 * (~/.omc/agent/agents) and project agents (.omc/.omp/.claude agents dirs). Returns owned object name -> def. */
json_t *agent_defs_load(const char *cwd) {
    json_t *out = json_obj();
    static const char *files[] = {"scout", "reviewer", "security-reviewer", "seance"};
    for (size_t i = 0; i < ARRAY_LEN(files); i++) {
        char *rel = xasprintf("coding-agent/prompts/agents/%s.md", files[i]);
        char *t = prompt_load(rel);
        free(rel);
        if (!t) continue;
        json_t *d = parse_agent_def(t, files[i]);
        json_set_str(d, "source", "bundled");
        json_set(out, json_get_str(d, "name"), d);
        free(t);
    }
    char *body = prompt_load("coding-agent/prompts/agents/task.md");
    if (body) {
        static const struct {
            const char *name, *description, *model, *thinking;
        } synth[] = {
            {"task", "General-purpose subagent with full capabilities for delegated multi-step tasks", "@task", NULL},
            {"sonic", "Low-reasoning agent for strictly mechanical updates or data collection only", "@smol", "medium"},
        };
        for (size_t i = 0; i < ARRAY_LEN(synth); i++) {
            json_t *d = json_obj();
            json_set_str(d, "name", synth[i].name);
            json_set_str(d, "description", synth[i].description);
            json_set(d, "tools", json_arr());
            json_set_str(d, "model", synth[i].model);
            if (synth[i].thinking) json_set_str(d, "thinking", synth[i].thinking);
            char *b = tpl_render(body, NULL);
            json_set_str(d, "body", b);
            free(b);
            json_set_str(d, "source", "bundled");
            json_set(out, synth[i].name, d);
        }
        free(body);
    }
    char *ua = omc_agent_path("agents");
    scan_agents(out, ua);
    free(ua);
    const char *pd[] = {".omc/agents", ".omp/agents", ".claude/agents"};
    for (size_t i = 0; i < ARRAY_LEN(pd); i++) {
        char *p = path_join(cwd, pd[i]);
        scan_agents(out, p);
        free(p);
    }
    return out;
}

/* task/read-only-policy.ts: read-only iff the declared tools are a non-empty subset of read-tier tools */
bool agent_def_read_only(const json_t *def) {
    static const char *const RO[] = {"read",     "wait",  "grep", "glob",   "find",        "web_search",
                                     "ast_grep", "yield", "ask",  "todo",   "recall",      "reflect",
                                     "retain",   "memory_edit",   "checkpoint", "rewind"};
    json_t *tl = json_get_arr(def, "tools"), *t;
    if (!json_len(tl)) return false;
    json_arr_foreach(tl, i, t) {
        bool ok = false;
        for (size_t k = 0; k < ARRAY_LEN(RO); k++)
            if (str_eq(json_sv(t), RO[k])) ok = true;
        if (!ok) return false;
    }
    return true;
}

static bool disabled_agent(const char *name) {
    json_t *dis = settings_get("task.disabledAgents"), *d;
    json_arr_foreach(dis, i, d) if (str_eq(json_sv(d), name)) return true;
    return false;
}

/* ---------- tool ---------- */

/* task/types.ts canSpawnAtDepth */
static bool can_spawn_at_depth(int depth) {
    int64_t max = settings_int("task.maxRecursionDepth", 2);
    return max < 0 || depth < max;
}

/* irc/messaging.ts isIrcEnabled: subagents always message; the main session while it can spawn */
static bool irc_enabled(const tool_env_t *env) { return env->subagent_depth > 0 || can_spawn_at_depth(env->subagent_depth); }

/* task/index.ts composeSpawnAdvisory: the specialization nudge (>= 2 generic task/sonic items) and, for spawns that
 * keep running after the call, the coordination suggestion (>= 2 live siblings). NULL when neither applies. */
static char *spawn_advisory(const tool_env_t *env, const json_t *tasks, size_t async_items, bool will_run_async) {
    if (!can_spawn_at_depth(env->subagent_depth)) return NULL;
    sb_t b = SB_INIT;
    size_t generics = 0;
    const char *first = NULL;
    json_t *t;
    json_arr_foreach(tasks, i, t) {
        const char *agent = json_get_str(t, "agent") ? json_get_str(t, "agent") : "task";
        if (str_eq(agent, "task") || str_eq(agent, "sonic")) {
            if (!first) first = agent;
            generics++;
        }
    }
    if (generics >= 2) {
        json_t *c = json_obj();
        json_set_int(c, "count", (int64_t)generics);
        json_set_str(c, "agent", first);
        json_set_bool(c, "scoutAvailable", tools_scout_available());
        char *r = prompt_render("coding-agent/prompts/tools/task-specialization-advisory.md", c);
        json_free(c);
        char *tr = str_trim(r);
        sb_append(&b, tr);
        free(tr);
        free(r);
    }
    if (will_run_async && irc_enabled(env) && async_items >= 2) {
        json_t *c = json_obj();
        json_set_int(c, "count", (int64_t)async_items);
        char *r = prompt_render("coding-agent/prompts/tools/task-coordination-advisory.md", c);
        json_free(c);
        char *tr = str_trim(r);
        if (b.len) sb_append(&b, "\n\n");
        sb_append(&b, tr);
        free(tr);
        free(r);
    }
    if (!b.len) {
        sb_free(&b);
        return NULL;
    }
    return sb_detach(&b);
}

/* withAdvisory: appended to the result's first text block (a new block when there is none); takes advisory */
static void append_advisory(tool_result_t *out, char *advisory) {
    if (!advisory) return;
    json_t *blk;
    json_arr_foreach(out->content, i, blk) {
        if (str_eq(json_get_str(blk, "type"), "text")) {
            char *t = xasprintf("%s\n\n%s", json_get_str(blk, "text") ? json_get_str(blk, "text") : "", advisory);
            json_set_str(blk, "text", t);
            free(t);
            free(advisory);
            return;
        }
    }
    tool_result_text(out, advisory);
    free(advisory);
}

static bool async_on(const tool_env_t *env) { return env->jobs && settings_bool("async.enabled", true); }

/* structured-subagent.ts normalizeModelSelector: a present but blank `model` ("", whitespace, []) is "use the
 * agent's routing", not a selector */
static bool blank_model(const json_t *m) {
    if (json_is_str(m)) {
        for (const char *s = json_sv(m); *s; s++)
            if (!isspace((unsigned char)*s)) return false;
        return true;
    }
    return json_is_arr(m) && json_len(m) == 0;
}

static spawn_spec_t spec_of(const json_t *t, const char *context) {
    spawn_spec_t spec = {.agent = json_get_str(t, "agent") ? json_get_str(t, "agent") : "task",
                         .name = json_get_str(t, "name"),
                         .task = json_get_str(t, "task"),
                         .context = context,
                         .source_session = json_get_str(t, "sourceSession"),
                         .model = blank_model(json_get(t, "model")) ? NULL : json_get(t, "model")};
    return spec;
}

static bool blank_str(const json_t *v) {
    const char *s = json_sv(v);
    if (!s) return true;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return !*s;
}

/* omp validateSpawnParams (batch shape, task.file off): the tool takes raw args when the schema rejects them
 * (lenientArgValidation) and reports these model-facing problems itself */
static char *spawn_params_problem(const json_t *args) {
    json_t *tasks = json_get(args, "tasks");
    if (tasks) {
        if (!json_is_arr(tasks) || !json_len(tasks)) return xstrdup("Missing `tasks`. Provide at least one task item with `task` or `taskFile`.");
        if (json_get(args, "task") || json_get(args, "taskFile"))
            return xstrdup("Top-level `task` or `taskFile` is not part of the batch shape. Put the work in `tasks[]` items.");
        json_t *it;
        json_arr_foreach(tasks, i, it) {
            const char *nm = json_get_str(it, "name");
            char *label = nm && *nm ? xasprintf("Task %zu (`%s`)", i + 1, nm) : xasprintf("Task %zu", i + 1);
            char *r = NULL;
            if (blank_str(json_get(it, "task")))
                r = xasprintf("%s is missing `task`. Every task needs complete, self-contained instructions.", label);
            else if (json_get(it, "effort") && !str_eq(json_get_str(it, "effort"), "lo") && !str_eq(json_get_str(it, "effort"), "med") &&
                     !str_eq(json_get_str(it, "effort"), "hi")) {
                char *e = json_dump(json_get(it, "effort"), 0);
                r = xasprintf("%s has an invalid `effort` value %s. Use \"lo\", \"med\", or \"hi\".", label, e);
                free(e);
            }
            free(label);
            if (r) return r;
        }
        strvec_t seen = {0}, shown = {0};
        json_arr_foreach(tasks, i, it) {
            char *nm = json_get_str(it, "name") ? str_trim(json_get_str(it, "name")) : xstrdup("");
            if (!*nm) {
                free(nm);
                continue;
            }
            char *key = xstrdup(nm);
            for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
            for (size_t k = 0; k < seen.n; k++)
                if (!strcmp(seen.v[k], key)) {
                    char *r = !strcmp(shown.v[k], nm)
                                  ? xasprintf("Duplicate task name `%s`. Provided names must be unique within a call (case-insensitive).", nm)
                                  : xasprintf("Duplicate task name `%s` / `%s`. Provided names must be unique within a call "
                                              "(case-insensitive).",
                                              shown.v[k], nm);
                    free(key);
                    free(nm);
                    strvec_free(&seen);
                    strvec_free(&shown);
                    return r;
                }
            strvec_push_own(&seen, key);
            strvec_push_own(&shown, nm);
        }
        strvec_free(&seen);
        strvec_free(&shown);
        if (blank_str(json_get(args, "context")))
            return xstrdup("Missing `context`. Provide the shared background for this batch — goal, constraints, and any contract "
                           "the tasks share.");
        return NULL;
    }
    if (!json_get(args, "task") && !json_get(args, "taskFile"))
        return xstrdup("Missing `tasks`. Provide a `tasks` array (one subagent per item) with a shared `context`.");
    if (blank_str(json_get(args, "task"))) return xstrdup("Missing `task`. Provide complete, self-contained instructions for the agent.");
    return NULL;
}

static int task_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    char *problem = spawn_params_problem(args);
    if (problem) {
        /* createTaskModeError */
        tool_result_error(out, "%s", problem);
        out->details = json_obj();
        json_set(out->details, "projectAgentsDir", json_null());
        json_set(out->details, "results", json_arr());
        json_set_int(out->details, "totalDurationMs", 0);
        free(problem);
        return 0;
    }
    if (!env->session) {
        tool_result_error(out, "Subagents are unavailable in this context");
        return 0;
    }
    json_t *tasks = json_get_arr(args, "tasks");
    json_t *single = NULL;
    if (!tasks) {
        if (!json_get_str(args, "task")) {
            tool_result_error(out, "tasks[] (or task) is required");
            return 0;
        }
        single = json_arr();
        json_push(single, json_clone(args));
        tasks = single;
    }
    const char *context = json_get_str(args, "context");
    size_t n = json_len(tasks);
    if (!n) {
        tool_result_error(out, "tasks must not be empty");
        json_free(single);
        return 0;
    }
    /* validate every item before spawning anything */
    json_t *defs = agent_defs_load(env->cwd);
    sb_t errs = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        json_t *t = json_at(tasks, i);
        const char *agent = json_get_str(t, "agent") ? json_get_str(t, "agent") : "task";
        const char *task = json_get_str(t, "task");
        if (!task || !*task) sb_appendf(&errs, "tasks[%zu]: `task` must not be empty\n", i);
        if (!json_get(defs, agent) || disabled_agent(agent))
            sb_appendf(&errs, "tasks[%zu]: unknown agent \"%s\"\n", i, agent);
        if (json_len(json_get_arr(t, "tools")))
            sb_appendf(&errs, "tasks[%zu]: `tools` (eval-defined tools) are not supported\n", i);
        bool seance = str_eq(agent, "seance");
        if (json_get(t, "sourceSession") && !seance)
            sb_appendf(&errs, "tasks[%zu]: `sourceSession` is only valid with agent \"seance\"\n", i);
        if (seance && !json_get_str(t, "sourceSession"))
            sb_appendf(&errs, "tasks[%zu]: agent \"seance\" requires `sourceSession`\n", i);
        if (json_get(t, "model") && !blank_model(json_get(t, "model")) && !seance)
            sb_appendf(&errs, "tasks[%zu]: per-call `model` is disabled (task.perCallModel: off)\n", i);
    }
    if (errs.len) {
        json_free(defs);
        sb_truncate(&errs, errs.len - 1);
        tool_result_error(out, "%s", errs.s);
        sb_free(&errs);
        json_free(single);
        return 0;
    }
    sb_free(&errs);

    if (async_on(env)) {
        /* omp TaskToolDetails for the card: one pending progress row per spawn, async state running */
        json_t *details = json_obj(), *progress = json_arr();
        json_set(details, "projectAgentsDir", json_null());
        json_set(details, "results", json_arr());
        json_set_int(details, "totalDurationMs", 0);
        for (size_t i = 0; i < n; i++) {
            json_t *t = json_at(tasks, i), *p = json_obj();
            const char *agent = json_get_str(t, "agent") ? json_get_str(t, "agent") : "task";
            const char *src = json_get_str(json_get(defs, agent), "source");
            char *assign = str_trim(json_get_str(t, "task") ? json_get_str(t, "task") : "");
            char *up = tool_subagent_user_prompt(assign);
            json_set_int(p, "index", (int64_t)i);
            json_set_str(p, "id", json_get_str(t, "name") ? json_get_str(t, "name") : agent);
            json_set_str(p, "agent", agent);
            json_set_str(p, "agentSource", str_eq(src, "bundled") || !src ? "bundled" : strstr(src, env->cwd) ? "project" : "user");
            json_set_str(p, "status", "pending");
            json_set_str(p, "task", up);
            json_set_str(p, "assignment", assign);
            json_set(p, "recentTools", json_arr());
            json_set(p, "recentOutput", json_arr());
            json_set_int(p, "toolCount", 0);
            json_set_int(p, "requests", 0);
            json_set_int(p, "tokens", 0);
            json_set_int(p, "cost", 0);
            json_set_int(p, "durationMs", 0);
            json_push(progress, p);
            free(up);
            free(assign);
        }
        json_set(details, "progress", progress);
        json_t *as = json_obj();
        json_set_str(as, "state", "running");
        json_set_str(as, "jobId", "");
        json_set_str(as, "type", "task");
        json_set(details, "async", as);
        task_card_t *card = task_card_new(env, ctx->tool_call_id, args, details);
        json_free(defs);
        json_t *started = json_arr();
        strvec_t agents = {0};
        sb_t fails = SB_INIT;
        size_t nfail = 0;
        for (size_t i = 0; i < n; i++) {
            spawn_spec_t spec = spec_of(json_at(tasks, i), context);
            spec.card = card;
            spec.card_index = (int)i;
            char *aid = NULL, *jid = NULL, *err = NULL;
            if (agents_spawn(env, &spec, &aid, &jid, &err) != 0) {
                sb_appendf(&fails, "%s%s: %s", nfail ? "; " : "", spec.name ? spec.name : spec.agent, err ? err : "failed");
                nfail++;
                task_card_settle(card, (int)i, "failed");
            } else {
                task_card_set_id(card, (int)i, aid, json_len(started) == 0);
                json_t *e = json_obj();
                json_set_str(e, "agentId", aid);
                json_set_str(e, "jobId", jid);
                json_push(started, e);
                if (!strvec_contains(&agents, spec.agent)) strvec_push(&agents, spec.agent);
            }
            free(aid);
            free(jid);
            free(err);
        }
        size_t ns = json_len(started);
        if (!ns) {
            tool_result_error(out, "Failed to start background task job%s: %s", nfail == 1 ? "" : "s",
                              fails.s ? fails.s : "");
        } else {
            if (ctx->update) { /* onUpdate before the call returns */
                tool_result_t pr = {0};
                if (n == 1) tool_result_textf(&pr, "Spawned agent `%s`...", json_get_str(json_at(started, 0), "agentId"));
                else tool_result_textf(&pr, "Spawned %zu agents...", ns);
                pr.details = task_card_snapshot(card);
                ctx->update(ctx, &pr);
                tool_result_free(&pr);
            }
            sb_t label = SB_INIT;
            for (size_t i = 0; i < agents.n; i++) sb_appendf(&label, "%s%s", i ? ", " : "", agents.v[i]);
            json_t *c = json_obj();
            json_set_bool(c, "mixed", false);
            json_set_bool(c, "singular", ns == 1);
            json_set_bool(c, "singleCall", n == 1);
            json_set_bool(c, "showListing", n > 1);
            json_set_int(c, "count", (int64_t)ns);
            json_set_str(c, "agentId", json_get_str(json_at(started, 0), "agentId"));
            json_set_str(c, "jobId", json_get_str(json_at(started, 0), "jobId"));
            json_set_str(c, "agentLabel", label.s ? label.s : "");
            json_set(c, "started", json_clone(started));
            char *sfs = nfail ? xasprintf(" Failed to schedule %zu spawn%s: %s.", nfail, nfail == 1 ? "" : "s", fails.s)
                              : xstrdup("");
            json_set_str(c, "scheduleFailureSummary", sfs);
            free(sfs);
            char *fb = prompt_render("coding-agent/prompts/tools/task-spawn-feedback.md", c);
            char *tr = str_trim(fb);
            tool_result_text(out, tr);
            free(tr);
            free(fb);
            json_free(c);
            sb_free(&label);
            append_advisory(out, spawn_advisory(env, tasks, n, true));
            out->details = task_card_snapshot(card);
        }
        task_card_release(card);
        json_free(started);
        strvec_free(&agents);
        sb_free(&fails);
        json_free(single);
        return 0;
    }

    /* async disabled: each spawn runs to completion inline */
    json_free(defs);
    sb_t b = SB_INIT;
    bool any_err = false;
    for (size_t i = 0; i < n; i++) {
        spawn_spec_t spec = spec_of(json_at(tasks, i), context);
        bool failed = false;
        char *r = agents_run_blocking(env, &spec, ctx->cancel, &failed);
        if (failed) any_err = true;
        if (b.len) sb_append(&b, "\n\n");
        sb_append(&b, r ? r : "(no output)");
        free(r);
    }
    tool_result_text(out, b.s ? b.s : "");
    out->is_error = any_err && n == 1;
    sb_free(&b);
    append_advisory(out, spawn_advisory(env, tasks, 0, false));
    json_free(single);
    return 0;
}

agent_tool_t *tool_task_new(tool_env_t *env) {
    json_t *defs = agent_defs_load(env->cwd);
    bool async = async_on(env);
    bool irc = irc_enabled(env); /* `write agent://<id>` peer messaging (agents.c) */
    bool batch = settings_bool("task.batch", true);
    json_t *ctx = json_obj();
    json_t *agents = json_arr();
    bool seance = false;
    const char *k;
    json_t *v;
    json_obj_foreach(defs, i, k, v) {
        if (disabled_agent(k)) continue;
        json_t *a = json_obj();
        json_set_str(a, "name", k);
        json_set_str(a, "description", json_get_str(v, "description") ? json_get_str(v, "description") : "");
        json_set_bool(a, "readOnly", agent_def_read_only(v));
        json_set_bool(a, "blocking", false);
        json_set_bool(a, "structured", false);
        json_push(agents, a);
        if (str_eq(k, "seance") && str_eq(json_get_str(v, "source"), "bundled")) seance = true;
    }
    json_set(ctx, "agents", agents);
    json_set_bool(ctx, "scoutAvailable", tools_scout_available());
    json_set_bool(ctx, "spawningDisabled", false);
    json_set_str(ctx, "defaultAgent", "task");
    json_set_bool(ctx, "batchEnabled", batch);
    json_set_bool(ctx, "asyncEnabled", async);
    json_set_bool(ctx, "ircEnabled", irc);
    /* eval-defined `tools` are not implemented: the conditional stays off (never advertised) */
    json_set_bool(ctx, "evalToolsEnabled", false);
    if (seance) {
        char *sg = prompt_load("coding-agent/prompts/tools/seance-task-guidance.md");
        json_set_str(ctx, "seanceGuidance", sg ? sg : "");
        free(sg);
    }
    if (async) {
        json_t *ac = json_obj();
        json_set_bool(ac, "ircEnabled", irc);
        json_set_bool(ac, "waitTool", true);
        char *g = prompt_render("coding-agent/prompts/tools/task-async-contract.md", ac);
        json_free(ac);
        json_set_str(ctx, "asyncGuidance", g);
        free(g);
    }
    char *desc = tool_prompt("task", ctx);
    json_free(ctx);
    json_free(defs);
    /* upstream task/types.ts createTaskSchema (no isolation, no eval tools): `model` is declared only while the
     * caller may send it, i.e. seance's selector list (task.perCallModel has no per-call selector here); an
     * undeclared `model` still reaches the preflight refusal through lenient validation */
    const char *seance_fields = seance ? ",\"model\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"array\",\"items\":{"
                                         "\"type\":\"string\"}}]},\"sourceSession\":{\"type\":\"string\"}"
                                       : "";
    char *it = xasprintf("{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\"},\"solutionSpace\":{\"type\":"
                         "\"string\"},\"name\":{\"type\":\"string\"},\"agent\":{\"type\":\"string\"}%s},\"required\":["
                         "\"task\",\"solutionSpace\"],\"additionalProperties\":false}",
                         seance_fields);
    char *sch = batch ? xasprintf("{\"type\":\"object\",\"properties\":{\"context\":{\"type\":\"string\"},\"tasks\":{"
                                  "\"type\":\"array\",\"items\":%s}},\"required\":[\"context\",\"tasks\"],"
                                  "\"additionalProperties\":false}",
                                  it)
                      : xstrdup(it);
    agent_tool_t *t = agent_tool_new("task", "Task", desc, schema_lit(sch), task_execute);
    t->lenient_args = true; /* omp lenientArgValidation: execute() reports spawn-shape problems */
    free(it);
    free(sch);
    free(desc);
    t->ud = env;
    return t;
}
