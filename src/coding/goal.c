/* goal.c - goal mode (port of omp goals/runtime): one persistent objective per session with token/time
 * accounting, budget limits, the `goal` tool, hidden per-turn context, persistence and auto-continuation. */
#include "goal.h"

#include "config/settings.h"
#include "prompt/template.h"

struct goal {
    bool present;   /* a goal exists (any status) */
    bool enabled;   /* goal mode on (active/budget-limited) */
    bool exiting;   /* completed; leave goal mode at agent end */
    char id[24];
    char *objective;
    char status[16]; /* active | paused | budget-limited | complete | dropped */
    int64_t budget;  /* 0 = none */
    int64_t used, secs, created, updated;
    int64_t wall_last; /* ms; 0 = wall clock not running */
    bool budget_reported;
    bool tool_installed;
    /* continuation bookkeeping */
    int pending_cont;
    bool suppress;
    char *prev_activity;
};

static goal_t *G(app_t *a) {
    if (!a->goal) a->goal = xcalloc(1, sizeof(goal_t));
    return a->goal;
}

void goal_free(goal_t *g) {
    if (!g) return;
    free(g->objective);
    free(g->prev_activity);
    free(g);
}

static bool accounting(const goal_t *g) {
    return g->present && g->enabled && (!strcmp(g->status, "active") || !strcmp(g->status, "budget-limited"));
}

static char *xml_escape(const char *s) {
    sb_t b = SB_INIT;
    for (; s && *s; s++) {
        if (*s == '&') sb_append(&b, "&amp;");
        else if (*s == '<') sb_append(&b, "&lt;");
        else if (*s == '>') sb_append(&b, "&gt;");
        else sb_putc(&b, *s);
    }
    return sb_detach(&b);
}

json_t *goal_json(const goal_t *g) {
    if (!g || !g->present) return json_null();
    json_t *o = json_obj();
    json_set_str(o, "id", g->id);
    json_set_str(o, "objective", g->objective);
    json_set_str(o, "status", g->status);
    if (g->budget) json_set_int(o, "tokenBudget", g->budget);
    json_set_int(o, "tokensUsed", g->used);
    json_set_int(o, "timeUsedSeconds", g->secs);
    json_set_int(o, "createdAt", g->created);
    json_set_int(o, "updatedAt", g->updated);
    return o;
}

json_t *goal_state_json(const goal_t *g) {
    if (!g || !g->present) return json_null();
    json_t *o = json_obj();
    json_set_bool(o, "enabled", g->enabled);
    json_set_str(o, "mode", g->exiting ? "exiting" : "active");
    if (g->exiting) json_set_str(o, "reason", "completed");
    json_set(o, "goal", goal_json(g));
    return o;
}

static void persist(app_t *a, const char *mode) {
    if (!a->session) return;
    json_t *e = json_obj();
    json_set_str(e, "type", "mode_change");
    json_set_str(e, "mode", mode);
    if (strcmp(mode, "none")) {
        json_t *d = json_obj();
        json_set(d, "goal", goal_json(a->goal));
        json_set(e, "data", d);
    }
    session_append_entry(a->session, e);
}

static void emit(app_t *a) {
    if (a->notice) {
        /* surfaces goal changes to the UI as a structured notice */
        goal_t *g = a->goal;
        char *s = g && g->present ? xasprintf("Goal %s: %.80s", g->status, g->objective) : xstrdup("Goal cleared");
        a->notice(a->notice_ud, "goal", s);
        free(s);
    }
}

/* fold elapsed wall-clock seconds and pending token usage into the goal; returns true when it just hit its budget */
static bool flush(app_t *a, int64_t tokens) {
    goal_t *g = G(a);
    if (!accounting(g)) return false;
    int64_t now = now_ms(), secs = 0;
    if (g->wall_last) {
        secs = (now - g->wall_last) / 1000;
        if (secs < 0) secs = 0;
        g->wall_last += secs * 1000;
    } else {
        g->wall_last = now;
    }
    if (tokens <= 0 && secs <= 0) return false;
    g->used += tokens > 0 ? tokens : 0;
    g->secs += secs;
    g->updated = now;
    bool flipped = g->budget && g->used >= g->budget && !strcmp(g->status, "active");
    if (flipped) snprintf(g->status, sizeof g->status, "budget-limited");
    if (tokens > 0 || flipped) persist(a, "goal");
    if (strcmp(g->status, "budget-limited")) g->budget_reported = false;
    return flipped;
}

static char *render(const char *which, const goal_t *g) {
    json_t *c = json_obj();
    char *obj = xml_escape(g->objective);
    json_set_str(c, "objective", obj);
    free(obj);
    char *s;
    s = xasprintf("%lld", (long long)g->used);
    json_set_str(c, "tokensUsed", s);
    free(s);
    if (g->budget) {
        s = xasprintf("%lld", (long long)g->budget);
        json_set_str(c, "tokenBudget", s);
        free(s);
        s = xasprintf("%lld", (long long)(g->budget > g->used ? g->budget - g->used : 0));
        json_set_str(c, "remainingTokens", s);
        free(s);
    } else {
        json_set_str(c, "tokenBudget", "none");
        json_set_str(c, "remainingTokens", "unbounded");
    }
    s = xasprintf("%lld", (long long)g->secs);
    json_set_str(c, "timeUsedSeconds", s);
    free(s);
    char *path = xasprintf("coding-agent/prompts/goals/%s.md", which);
    char *r = prompt_render(path, c);
    free(path);
    json_free(c);
    return r;
}

static void budget_steer(app_t *a) {
    goal_t *g = a->goal;
    if (g->budget_reported) return;
    g->budget_reported = true;
    char *txt = render("goal-budget-limit", g);
    json_t *m = msg_user_text(txt);
    json_set_bool(m, "synthetic", true);
    json_set_str(m, "customType", "goal-budget-limit");
    agent_steer(a->agent, m);
    free(txt);
}

/* ---------- tool ---------- */

static void tool_text(tool_result_t *out, const goal_t *g, bool completion_report, const char *op) {
    if (!g || !g->present) {
        tool_result_text(out, "No active goal.");
    } else {
        sb_t b = SB_INIT;
        sb_appendf(&b, "Goal: %s\nStatus: %s\nTokens: %lld used", g->objective, g->status, (long long)g->used);
        if (g->budget) sb_appendf(&b, " / %lld budget", (long long)g->budget);
        if (g->budget) sb_appendf(&b, "\nRemaining tokens: %lld", (long long)(g->budget > g->used ? g->budget - g->used : 0));
        if (completion_report && !strcmp(g->status, "complete")) {
            sb_t p = SB_INIT;
            if (g->budget) sb_appendf(&p, "tokens used: %lld of %lld", (long long)g->used, (long long)g->budget);
            if (g->secs > 0) sb_appendf(&p, "%stime used: %lld seconds", p.len ? "; " : "", (long long)g->secs);
            if (p.len) sb_appendf(&b, "\n\nGoal achieved. Report final budget usage to the user: %s.", sb_cstr(&p));
            sb_free(&p);
        }
        tool_result_text(out, sb_cstr(&b));
        sb_free(&b);
    }
    out->details = json_obj();
    json_set_str(out->details, "op", op);
    json_set(out->details, "goal", goal_json(g));
    if (g && g->present && g->budget) json_set_int(out->details, "remainingTokens", g->budget > g->used ? g->budget - g->used : 0);
    else json_set(out->details, "remainingTokens", json_null());
}

static int goal_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    app_t *a = self->ud;
    UNUSED(ctx);
    const char *op = json_get_str(args, "op");
    char *err = NULL;
    if (!a->goal || !a->goal->present || (!a->goal->enabled && !str_eq(op, "get") && !str_eq(op, "resume"))) {
        if (!str_eq(op, "create")) {
            tool_result_error(out, "Goal mode is not active.");
            return 0;
        }
    }
    int rc = 0;
    if (str_eq(op, "create")) {
        int64_t b = json_get_int(args, "token_budget", 0);
        if (json_get(args, "token_budget") && b <= 0) err = xstrdup("token_budget must be a positive integer when provided");
        else rc = app_goal_create(a, json_get_str(args, "objective"), b, &err);
    } else if (str_eq(op, "get")) {
        /* nothing to change */
    } else if (str_eq(op, "resume")) {
        rc = app_goal_resume(a, &err);
    } else if (str_eq(op, "drop")) {
        json_t *before = goal_json(a->goal);
        app_goal_drop(a);
        tool_result_text(out, "Goal dropped.");
        out->details = json_obj();
        json_set_str(out->details, "op", "drop");
        json_set_str(before, "status", "dropped");
        json_set(out->details, "goal", before);
        return 0;
    } else if (str_eq(op, "complete")) {
        rc = app_goal_complete(a, &err);
    } else {
        err = xasprintf("unknown op '%s' (expected create, get, complete, resume or drop)", op ? op : "");
    }
    if (err || rc) {
        tool_result_error(out, "%s", err ? err : "goal operation failed");
        free(err);
        return 0;
    }
    tool_text(out, a->goal, str_eq(op, "complete"), op);
    return 0;
}

static void sync_tool(app_t *a) {
    goal_t *g = G(a);
    bool want = settings_bool("goal.enabled", true) && g->present && (g->enabled || g->exiting);
    if (want && !agent_find_tool(a->agent, "goal")) {
        char *desc = tool_prompt("goal", NULL);
        json_t *s = schema_obj();
        json_t *op = json_obj();
        json_set_str(op, "type", "string");
        json_set(op, "enum", json_parse_str("[\"create\",\"get\",\"complete\",\"resume\",\"drop\"]"));
        json_set_str(op, "description", "goal operation");
        schema_prop_json(s, "op", op, true);
        schema_prop(s, "objective", "string", "goal objective", false);
        schema_prop(s, "token_budget", "integer", "token budget", false);
        agent_tool_t *t = agent_tool_new("goal", "Goal", desc, s, goal_execute);
        t->ud = a;
        t->exclusive = true;
        agent_add_tool(a->agent, t);
        free(desc);
        app_rebuild_system_prompt(a);
    } else if (!want && agent_find_tool(a->agent, "goal") && !a->agent->running) {
        agent_remove_tool(a->agent, "goal");
        app_rebuild_system_prompt(a);
    }
}

/* ---------- operations ---------- */

static void set_objective(goal_t *g, const char *objective, int64_t budget) {
    free(g->objective);
    char *t = str_trim(objective);
    g->objective = t;
    int64_t now = now_ms();
    snprintf(g->id, sizeof g->id, "%lld%03d", (long long)now, (int)(hash_xx64(objective, strlen(objective), (uint64_t)now) % 1000));
    snprintf(g->status, sizeof g->status, "active");
    g->present = g->enabled = true;
    g->exiting = false;
    g->budget = budget > 0 ? budget : 0;
    g->used = g->secs = 0;
    g->created = g->updated = now;
    g->wall_last = now;
    g->budget_reported = false;
    g->suppress = false;
    free(g->prev_activity);
    g->prev_activity = NULL;
}

int app_goal_create(app_t *a, const char *objective, int64_t budget, char **err) {
    goal_t *g = G(a);
    char *t = str_trim(objective ? objective : "");
    bool empty = !*t;
    free(t);
    if (empty) {
        *err = xstrdup("objective is required when op=create");
        return -1;
    }
    if (g->present && strcmp(g->status, "dropped") && strcmp(g->status, "complete")) {
        *err = xstrdup("cannot create a new goal because this session already has a goal");
        return -1;
    }
    if (a->plan_mode) {
        *err = xstrdup("cannot start goal mode while plan mode is active");
        return -1;
    }
    set_objective(g, objective, budget);
    persist(a, "goal");
    sync_tool(a);
    emit(a);
    return 0;
}

int app_goal_replace(app_t *a, const char *objective, int64_t budget, char **err) {
    goal_t *g = G(a);
    if (!g->present || !strcmp(g->status, "dropped") || !strcmp(g->status, "complete")) return app_goal_create(a, objective, budget, err);
    char *t = str_trim(objective ? objective : "");
    bool empty = !*t;
    free(t);
    if (empty) {
        *err = xstrdup("objective is required when op=replace");
        return -1;
    }
    flush(a, 0);
    set_objective(g, objective, budget);
    persist(a, "goal");
    sync_tool(a);
    emit(a);
    return 0;
}

int app_goal_resume(app_t *a, char **err) {
    goal_t *g = G(a);
    if (!g->present) {
        *err = xstrdup("No paused goal.");
        return -1;
    }
    if (!strcmp(g->status, "complete")) {
        *err = xstrdup("Goal is already complete.");
        return -1;
    }
    g->enabled = true;
    g->exiting = false;
    snprintf(g->status, sizeof g->status, "active");
    g->updated = now_ms();
    g->wall_last = g->updated;
    g->budget_reported = false;
    g->suppress = false;
    persist(a, "goal");
    sync_tool(a);
    emit(a);
    return 0;
}

int app_goal_pause(app_t *a) {
    goal_t *g = G(a);
    if (!g->present) return -1;
    flush(a, 0);
    g->enabled = false;
    if (!strcmp(g->status, "active") || !strcmp(g->status, "budget-limited")) snprintf(g->status, sizeof g->status, "paused");
    g->updated = now_ms();
    g->wall_last = 0;
    persist(a, "goal_paused");
    sync_tool(a);
    emit(a);
    return 0;
}

void app_goal_drop(app_t *a) {
    goal_t *g = G(a);
    if (!g->present) return;
    flush(a, 0);
    g->present = g->enabled = g->exiting = false;
    snprintf(g->status, sizeof g->status, "dropped");
    g->wall_last = 0;
    persist(a, "none");
    sync_tool(a);
    emit(a);
}

int app_goal_complete(app_t *a, char **err) {
    goal_t *g = G(a);
    if (!g->present) {
        *err = xstrdup("cannot complete goal because no goal is active");
        return -1;
    }
    if (!strcmp(g->status, "complete")) {
        *err = xstrdup("goal is already complete");
        return -1;
    }
    flush(a, 0);
    g->enabled = false;
    g->exiting = true;
    snprintf(g->status, sizeof g->status, "complete");
    g->updated = now_ms();
    g->wall_last = 0;
    persist(a, "goal");
    emit(a);
    return 0;
}

int app_goal_set_budget(app_t *a, int64_t budget, char **err) {
    goal_t *g = G(a);
    if (budget < 0) {
        *err = xstrdup("goal token_budget must be a positive integer when provided");
        return -1;
    }
    if (!g->present) {
        *err = xstrdup("No goal.");
        return -1;
    }
    flush(a, 0);
    g->budget_reported = false;
    g->budget = budget;
    g->updated = now_ms();
    bool steer = false;
    if (budget && g->used >= budget) {
        if (!strcmp(g->status, "active")) {
            snprintf(g->status, sizeof g->status, "budget-limited");
            steer = true;
        }
    } else if (!strcmp(g->status, "budget-limited")) {
        snprintf(g->status, sizeof g->status, "active");
        g->enabled = true;
        g->wall_last = now_ms();
    }
    persist(a, g->enabled ? "goal" : "goal_paused");
    if (steer) budget_steer(a);
    emit(a);
    return 0;
}

const goal_t *app_goal(app_t *a) { return a->goal && a->goal->present ? a->goal : NULL; }
const char *goal_status(const goal_t *g) { return g ? g->status : NULL; }
const char *goal_objective(const goal_t *g) { return g ? g->objective : NULL; }
bool goal_enabled(const goal_t *g) { return g && g->enabled; }
int64_t goal_used(const goal_t *g) { return g ? g->used : 0; }
int64_t goal_budget(const goal_t *g) { return g ? g->budget : 0; }

/* ---------- agent hooks ---------- */

void app_goal_on_event(app_t *a, const agent_event_t *ev) {
    goal_t *g = a->goal;
    if (!g || !accounting(g)) return;
    if (ev->type == AGENT_EV_MESSAGE_END && str_eq(json_get_str(ev->message, "role"), "assistant")) {
        json_t *u = json_get(ev->message, "usage");
        int64_t t = json_get_int(u, "input", 0) + json_get_int(u, "output", 0) + json_get_int(u, "cacheWrite", 0);
        g->used += t > 0 ? t : 0; /* folded into the next flush's persistence */
        if (t > 0) {
            bool flipped = g->budget && g->used >= g->budget && !strcmp(g->status, "active");
            if (flipped) snprintf(g->status, sizeof g->status, "budget-limited");
            persist(a, "goal");
        }
    } else if (ev->type == AGENT_EV_TOOL_END && !str_eq(ev->tool_name, "goal")) {
        bool was_limited = !strcmp(g->status, "budget-limited");
        flush(a, 0);
        if (!strcmp(g->status, "budget-limited") && (!was_limited || !g->budget_reported)) budget_steer(a);
    }
}

json_t *app_goal_context_message(app_t *a) {
    goal_t *g = a->goal;
    if (!g || !g->present || !g->enabled || strcmp(g->status, "active")) return NULL;
    flush(a, 0);
    char *active = render("goal-mode-active", g);
    json_t *c = json_obj();
    json_set_str(c, "goalContext", active);
    free(active);
    /* live todo state */
    json_t *phases = a->env ? a->env->todo.phases : NULL;
    if (json_len(phases) && agent_find_tool(a->agent, "todo")) {
        json_t *pp = json_arr(), *ph, *t;
        int total = 0, closed = 0;
        json_arr_foreach(phases, i, ph) {
            if (!json_len(json_get_arr(ph, "tasks"))) continue;
            json_t *p = json_obj(), *tasks = json_arr();
            char *nm = xml_escape(json_get_str(ph, "name") ? json_get_str(ph, "name") : "");
            json_set_str(p, "name", nm);
            free(nm);
            json_arr_foreach(json_get_arr(ph, "tasks"), k, t) {
                const char *st = json_get_str(t, "status");
                total++;
                if (str_eq(st, "completed") || str_eq(st, "abandoned")) closed++;
                json_t *x = json_obj();
                char *ct = xml_escape(json_get_str(t, "content") ? json_get_str(t, "content") : "");
                for (char *q = ct; *q; q++)
                    if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
                json_set_str(x, "content", ct);
                free(ct);
                json_set_str(x, "status", st ? st : "pending");
                json_push(tasks, x);
            }
            json_set(p, "tasks", tasks);
            json_push(pp, p);
        }
        if (total) {
            json_t *tc = json_obj();
            json_set_bool(tc, "canCallTodoTool", true);
            char buf[32];
            snprintf(buf, sizeof buf, "%d", closed);
            json_set_str(tc, "closed", buf);
            snprintf(buf, sizeof buf, "%d", total - closed);
            json_set_str(tc, "open", buf);
            snprintf(buf, sizeof buf, "%d", total);
            json_set_str(tc, "total", buf);
            json_set(tc, "phases", pp);
            pp = NULL;
            char *todo = prompt_render("coding-agent/prompts/goals/goal-todo-context.md", tc);
            json_set_str(c, "todoContext", todo);
            free(todo);
            json_free(tc);
        }
        json_free(pp);
    }
    char *txt = prompt_render("coding-agent/prompts/goals/goal-mode-context.md", c);
    json_free(c);
    json_t *m = msg_user_text(txt);
    json_set_bool(m, "synthetic", true);
    json_set_str(m, "customType", "goal-mode-context");
    free(txt);
    return m;
}

/* digest of a run's tool calls and results; identical digests across continuation turns = no progress */
static char *activity(const json_t *msgs) {
    sb_t b = SB_INIT;
    json_t *m, *blk;
    json_arr_foreach(msgs, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            json_arr_foreach(json_get_arr(m, "content"), k, blk) {
                if (!str_eq(json_get_str(blk, "type"), "toolCall")) continue;
                char *args = json_dump(json_get(blk, "arguments"), 0);
                char *s = xasprintf("call\x1f%s\x1f%s", json_get_str(blk, "name"), args);
                sb_appendf(&b, "%zu:%016llx:", strlen(s), (unsigned long long)hash_xx64(s, strlen(s), 0));
                free(s);
                free(args);
            }
        } else if (str_eq(role, "toolResult")) {
            char *c = json_dump(json_get(m, "content"), 0);
            char *s = xasprintf("result\x1f%s\x1f%s\x1f%d", json_get_str(m, "toolName"), c, json_get_bool(m, "isError", false));
            sb_appendf(&b, "%zu:%016llx:", strlen(s), (unsigned long long)hash_xx64(s, strlen(s), 0));
            free(s);
            free(c);
        }
    }
    return sb_detach(&b);
}

static bool open_work_all_blocked(app_t *a) {
    json_t *phases = a->env ? a->env->todo.phases : NULL, *ph, *t;
    bool blocked = false, actionable = false;
    json_arr_foreach(phases, i, ph) {
        json_arr_foreach(json_get_arr(ph, "tasks"), k, t) {
            const char *st = json_get_str(t, "status");
            if (str_eq(st, "blocked")) blocked = true;
            else if (!str_eq(st, "completed") && !str_eq(st, "abandoned")) actionable = true;
        }
    }
    return blocked && !actionable;
}

char *app_goal_after_run(app_t *a, const json_t *run_messages, bool was_continuation, const char *mode) {
    goal_t *g = a->goal;
    if (!g) return NULL;
    if (accounting(g)) flush(a, 0);
    if (was_continuation) {
        char *act = activity(run_messages);
        g->suppress = !*act || (g->prev_activity && !strcmp(act, g->prev_activity));
        free(g->prev_activity);
        g->prev_activity = act;
    } else {
        g->suppress = false;
        free(g->prev_activity);
        g->prev_activity = NULL;
    }
    if (g->exiting) {
        g->exiting = false;
        sync_tool(a);
        if (a->notice) a->notice(a->notice_ud, "info", "Goal complete; goal mode ended.");
        return NULL;
    }
    sync_tool(a);
    json_t *modes = settings_get("goal.continuationModes");
    bool allowed = false;
    if (json_is_arr(modes)) {
        json_t *x;
        json_arr_foreach(modes, i, x) if (str_eq(json_sv(x), mode)) allowed = true;
    } else {
        allowed = !strcmp(mode, "interactive");
    }
    if (!allowed || g->suppress || a->plan_mode || !g->present || !g->enabled || strcmp(g->status, "active")) return NULL;
    const char *stop = json_get_str(last_assistant(a->agent->messages), "stopReason");
    if (str_eq(stop, "aborted") || str_eq(stop, "error")) return NULL;
    if (open_work_all_blocked(a)) return NULL;
    return render("goal-continuation", g);
}

void app_goal_on_abort(app_t *a) {
    goal_t *g = a->goal;
    if (!g || !g->present || !g->enabled || strcmp(g->status, "active")) return;
    flush(a, 0);
    g->enabled = false;
    snprintf(g->status, sizeof g->status, "paused");
    g->updated = now_ms();
    g->wall_last = 0;
    persist(a, "goal_paused");
    emit(a);
}

void app_goal_restore(app_t *a) {
    goal_t *g = G(a);
    free(g->objective);
    free(g->prev_activity);
    memset(g, 0, sizeof(*g));
    if (!a->session || !a->session->leaf_id) {
        sync_tool(a);
        return;
    }
    json_t *path = session_path_to(a->session, a->session->leaf_id), *e;
    const json_t *last = NULL;
    json_arr_foreach(path, i, e) if (str_eq(json_get_str(e, "type"), "mode_change")) last = e;
    const char *mode = json_get_str(last, "mode");
    json_t *gd = json_get(json_get(last, "data"), "goal");
    if ((str_eq(mode, "goal") || str_eq(mode, "goal_paused")) && json_get_str(gd, "id") && json_get_str(gd, "objective") &&
        json_get_str(gd, "status")) {
        g->present = true;
        snprintf(g->id, sizeof g->id, "%s", json_get_str(gd, "id"));
        g->objective = xstrdup(json_get_str(gd, "objective"));
        snprintf(g->status, sizeof g->status, "%s", json_get_str(gd, "status"));
        g->budget = json_get_int(gd, "tokenBudget", 0);
        g->used = json_get_int(gd, "tokensUsed", 0);
        g->secs = json_get_int(gd, "timeUsedSeconds", 0);
        g->created = json_get_int(gd, "createdAt", 0);
        g->updated = json_get_int(gd, "updatedAt", 0);
        g->enabled = str_eq(mode, "goal") && (!strcmp(g->status, "active") || !strcmp(g->status, "budget-limited"));
        if (!strcmp(g->status, "complete") || !strcmp(g->status, "dropped")) g->present = false;
        else if (!strcmp(g->status, "active")) {
            /* a resumed thread never silently keeps spending: active goals come back paused */
            g->enabled = false;
            snprintf(g->status, sizeof g->status, "paused");
            g->updated = now_ms();
            persist(a, "goal_paused");
        }
    }
    json_free(path);
    sync_tool(a);
}
