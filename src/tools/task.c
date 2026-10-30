/* task tool: spawn subagents (parallel batch); agent definitions from bundled + user agents dirs */
#include "tools.h"

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

/* returns owned object name -> def */
json_t *agent_defs_load(const char *cwd) {
    json_t *out = json_obj();
    static const char *bundled[] = {"task", "scout", "reviewer", "security-reviewer", "init"};
    for (size_t i = 0; i < ARRAY_LEN(bundled); i++) {
        char *rel = xasprintf("coding-agent/prompts/agents/%s.md", bundled[i]);
        char *t = prompt_load(rel);
        free(rel);
        if (!t) continue;
        json_t *d = parse_agent_def(t, bundled[i]);
        if (!json_get_str(d, "description") || !*json_get_str(d, "description")) {
            json_set_str(d, "description", !strcmp(bundled[i], "task") ? "General-purpose worker with full tool access"
                                                                         : bundled[i]);
        }
        json_set_str(d, "name", bundled[i]);
        json_set(out, bundled[i], d);
        free(t);
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

/* ---------- tool ---------- */

typedef struct {
    tool_env_t *env;
    const char *agent;
    char *name;
    const char *task;
    const char *context;
    volatile sig_atomic_t *cancel;
    char *result;
    json_t *details;
    pthread_t th;
    bool started;
} task_job_t;

static void *task_thread(void *p) {
    task_job_t *j = p;
    j->result = j->env->run_subagent(j->env, j->agent, j->task, j->context, &j->details, j->cancel);
    return NULL;
}

static int task_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    if (!env->run_subagent) {
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
    json_t *defs = agent_defs_load(env->cwd);
    task_job_t *jobs = xcalloc(n, sizeof(task_job_t));
    for (size_t i = 0; i < n; i++) {
        json_t *t = json_at(tasks, i);
        jobs[i].env = env;
        jobs[i].agent = json_get_str(t, "agent") ? json_get_str(t, "agent") : "task";
        const char *nm = json_get_str(t, "name");
        jobs[i].name = nm ? xstrdup(nm) : xasprintf("%s%zu", jobs[i].agent, i + 1);
        jobs[i].task = json_get_str(t, "task");
        jobs[i].context = context;
        jobs[i].cancel = ctx->cancel;
        if (!jobs[i].task || !*jobs[i].task) {
            jobs[i].result = xstrdup("Error: empty task");
            continue;
        }
        if (!json_get(defs, jobs[i].agent)) {
            jobs[i].result = xasprintf("Error: unknown agent type \"%s\"", jobs[i].agent);
            continue;
        }
    }
    size_t maxc = (size_t)MAX(1, settings_int("task.maxConcurrency", 8));
    size_t next = 0;
    while (next < n) {
        size_t batch_end = MIN(n, next + maxc);
        for (size_t i = next; i < batch_end; i++) {
            if (jobs[i].result) continue;
            if (ctx->update) {
                tool_result_t pr = {0};
                tool_result_textf(&pr, "Running %s (%s)…", jobs[i].name, jobs[i].agent);
                ctx->update(ctx, &pr);
                tool_result_free(&pr);
            }
            jobs[i].started = pthread_create(&jobs[i].th, NULL, task_thread, &jobs[i]) == 0;
            if (!jobs[i].started) task_thread(&jobs[i]);
        }
        for (size_t i = next; i < batch_end; i++)
            if (jobs[i].started) pthread_join(jobs[i].th, NULL);
        next = batch_end;
    }
    sb_t b = SB_INIT;
    json_t *results = json_arr();
    bool any_err = false;
    for (size_t i = 0; i < n; i++) {
        sb_appendf(&b, "%s<agent id=\"%s\" type=\"%s\">\n%s\n</agent>\n", i ? "\n" : "", jobs[i].name, jobs[i].agent,
                   jobs[i].result ? jobs[i].result : "(no output)");
        json_t *r = jobs[i].details ? jobs[i].details : json_obj();
        json_set_str(r, "id", jobs[i].name);
        json_set_str(r, "output", jobs[i].result ? jobs[i].result : "");
        if (json_get_str(r, "error")) any_err = true;
        json_push(results, r);
        free(jobs[i].result);
        free(jobs[i].name);
    }
    free(jobs);
    json_free(defs);
    json_free(single);
    tool_result_text(out, sb_cstr(&b));
    out->details = json_obj();
    json_set(out->details, "results", results);
    out->is_error = any_err && n == 1;
    sb_free(&b);
    return 0;
}

agent_tool_t *tool_task_new(tool_env_t *env) {
    json_t *defs = agent_defs_load(env->cwd);
    json_t *ctx = json_obj();
    json_set_bool(ctx, "batchEnabled", true);
    json_set_bool(ctx, "asyncEnabled", false);
    json_set_str(ctx, "defaultAgent", "task");
    json_set_bool(ctx, "scoutAvailable", json_get(defs, "scout") != NULL);
    char *desc_base = tool_prompt("task", ctx);
    json_free(ctx);
    sb_t d = SB_INIT;
    sb_append(&d, desc_base);
    sb_append(&d, "\n\n# Agents\n");
    const char *k;
    json_t *v;
    json_obj_foreach(defs, i, k, v) sb_appendf(&d, "- `%s`: %s\n", k, json_get_str(v, "description"));
    free(desc_base);
    json_free(defs);
    json_t *s = schema_obj();
    schema_prop(s, "context", "string", "Shared background for every task", true);
    json_t *tasks = json_parse_str(
        "{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"CamelCase agent name\"},"
        "\"agent\":{\"type\":\"string\",\"description\":\"Agent type\"},"
        "\"task\":{\"type\":\"string\",\"description\":\"Self-contained instructions\"},"
        "\"solutionSpace\":{\"type\":\"string\",\"description\":\"How open-ended the problem is\"}},"
        "\"required\":[\"task\",\"solutionSpace\"]}}");
    schema_prop_json(s, "tasks", tasks, true);
    agent_tool_t *t = agent_tool_new("task", "Task", sb_cstr(&d), s, task_execute);
    t->lenient_args = true;
    sb_free(&d);
    t->ud = env;
    return t;
}
