/* todo + ask tools */
#include "tools.h"

/* ---------- todo (port of omp tools/todo.ts) ---------- */

static bool is_open(const char *st) { return str_eq(st, "pending") || str_eq(st, "in_progress"); }
static bool is_closed(const char *st) { return str_eq(st, "completed") || str_eq(st, "abandoned"); }

static json_t *new_task(const char *content) {
    json_t *t = json_obj();
    json_set_str(t, "content", content);
    json_set_str(t, "status", "pending");
    return t;
}

static json_t *find_task(json_t *phases, const char *content, json_t **phase_out) {
    json_t *p, *t;
    json_arr_foreach(phases, i, p) {
        json_arr_foreach(json_get(p, "tasks"), k, t) {
            if (str_eq(json_get_str(t, "content"), content)) {
                if (phase_out) *phase_out = p;
                return t;
            }
        }
    }
    return NULL;
}

static json_t *find_phase(json_t *phases, const char *name) {
    json_t *p;
    json_arr_foreach(phases, i, p) if (str_eq(json_get_str(p, "name"), name)) return p;
    return NULL;
}

static bool task_id_like(const char *s) {
    if (strncmp(s, "task-", 5) || !s[5]) return false;
    for (const char *p = s + 5; *p; p++)
        if (*p < '0' || *p > '9') return false;
    return true;
}

static size_t total_tasks(json_t *phases) {
    size_t n = 0;
    json_t *p;
    json_arr_foreach(phases, i, p) n += json_len(json_get(p, "tasks"));
    return n;
}

/* resolveTaskOrError */
static json_t *resolve_task(json_t *phases, const char *content, strvec_t *errs, json_t **phase_out) {
    if (!content || !*content) {
        strvec_push(errs, "Missing task content");
        return NULL;
    }
    json_t *t = find_task(phases, content, phase_out);
    if (!t) {
        if (task_id_like(content))
            strvec_push_own(errs, xasprintf("Task \"%s\" not found. Tasks are referenced by content, not by IDs — pass the "
                                            "task's full text from the previous result.",
                                            content));
        else
            strvec_push_own(errs, xasprintf("Task \"%s\" not found%s", content,
                                            total_tasks(phases) ? "" : " (todo list is empty — was it replaced or not yet created?)"));
    }
    return t;
}

/* resolvePhaseOrError */
static json_t *resolve_phase(json_t *phases, const char *name, strvec_t *errs) {
    if (!name || !*name) {
        strvec_push(errs, "Missing phase name");
        return NULL;
    }
    json_t *p = find_phase(phases, name);
    if (!p) strvec_push_own(errs, xasprintf("Phase \"%s\" not found", name));
    return p;
}

/* getTaskTargets: borrowed task objects */
typedef VEC(json_t *) jvec_t;

static void task_targets(json_t *phases, const char *task, const char *phase, strvec_t *errs, jvec_t *out) {
    if (task && *task) {
        json_t *t = resolve_task(phases, task, errs, NULL);
        if (t) vec_push(out, t);
        return;
    }
    json_t *p, *t;
    if (phase && *phase) {
        p = resolve_phase(phases, phase, errs);
        if (p) json_arr_foreach(json_get(p, "tasks"), k, t) vec_push(out, t);
        return;
    }
    json_arr_foreach(phases, i, p) json_arr_foreach(json_get(p, "tasks"), k, t) vec_push(out, t);
}

/* normalizeInProgressTask: one in_progress task, else the first pending one */
static void normalize(json_t *phases) {
    json_t *p, *t, *first_active = NULL, *first_pending = NULL;
    json_arr_foreach(phases, i, p) {
        json_arr_foreach(json_get(p, "tasks"), k, t) {
            const char *st = json_get_str(t, "status");
            if (str_eq(st, "in_progress")) {
                if (!first_active) first_active = t;
                else json_set_str(t, "status", "pending");
            } else if (str_eq(st, "pending") && !first_pending) {
                first_pending = t;
            }
        }
    }
    if (!first_active && first_pending) json_set_str(first_pending, "status", "in_progress");
}

static char *collapse_ws(const char *s) {
    sb_t b = SB_INIT;
    bool sp = false;
    for (; s && *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') sp = true;
        else {
            if (sp && b.len) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *s);
        }
    }
    return sb_detach(&b);
}

static json_t *init_phases(const json_t *args, strvec_t *errs) {
    json_t *items = json_get_arr(args, "items"), *list = json_get_arr(args, "list");
    const char *phase = json_get_str(args, "phase");
    json_t *flat = NULL;
    if (json_len(items)) {
        flat = json_arr();
        json_t *e = json_obj();
        json_set_str(e, "phase", phase ? phase : "Tasks");
        json_set(e, "items", json_clone(items));
        json_push(flat, e);
    }
    const json_t *use = json_len(list) ? list : flat ? flat : list;
    json_t *out = json_arr();
    if (!use) {
        strvec_push(errs, "Missing list for init operation");
        return out;
    }
    strvec_t seen_ph = {0}, seen_t = {0};
    json_t *e, *it;
    json_arr_foreach(use, i, e) {
        const char *nm = json_get_str(e, "phase");
        if (!nm) nm = "";
        bool dup = false;
        for (size_t k = 0; k < seen_ph.n; k++) dup |= !strcmp(seen_ph.v[k], nm);
        if (dup) strvec_push_own(errs, xasprintf("Duplicate phase \"%s\" in init list", nm));
        strvec_push(&seen_ph, nm);
        json_t *p = json_obj(), *tasks = json_arr();
        json_set_str(p, "name", nm);
        json_arr_foreach(json_get_arr(e, "items"), k, it) {
            const char *c = json_sv(it);
            if (!c) continue;
            bool d2 = false;
            for (size_t m = 0; m < seen_t.n; m++) d2 |= !strcmp(seen_t.v[m], c);
            if (d2) strvec_push_own(errs, xasprintf("Duplicate task \"%s\" in init list", c));
            strvec_push(&seen_t, c);
            json_push(tasks, new_task(c));
        }
        json_set(p, "tasks", tasks);
        json_push(out, p);
    }
    strvec_free(&seen_ph);
    strvec_free(&seen_t);
    json_free(flat);
    return out;
}

static void append_items(json_t *phases, const json_t *args, strvec_t *errs) {
    const char *phase = json_get_str(args, "phase");
    json_t *items = json_get_arr(args, "items"), *it;
    if (!phase || !*phase) {
        strvec_push(errs, "Missing phase name for append operation");
        return;
    }
    if (!json_len(items)) {
        strvec_push(errs, "Missing items for append operation");
        return;
    }
    strvec_t seen = {0};
    bool dup = false;
    json_arr_foreach(items, i, it) {
        const char *c = json_sv(it);
        if (!c) continue;
        bool d = find_task(phases, c, NULL) != NULL;
        for (size_t k = 0; k < seen.n; k++) d |= !strcmp(seen.v[k], c);
        if (d) {
            strvec_push_own(errs, xasprintf("Task \"%s\" already exists", c));
            dup = true;
        }
        strvec_push(&seen, c);
    }
    strvec_free(&seen);
    if (dup) return;
    json_t *p = find_phase(phases, phase);
    if (!p) {
        p = json_obj();
        json_set_str(p, "name", phase);
        json_set(p, "tasks", json_arr());
        json_push(phases, p);
    }
    json_arr_foreach(items, i, it) if (json_sv(it)) json_push(json_get(p, "tasks"), new_task(json_sv(it)));
}

static void remove_tasks(json_t *phases, const json_t *args, strvec_t *errs) {
    const char *task = json_get_str(args, "task"), *phase = json_get_str(args, "phase");
    json_t *p;
    if (task && *task) {
        json_t *owner = NULL;
        json_t *t = resolve_task(phases, task, errs, &owner);
        if (!t) return;
        json_t *tasks = json_get(owner, "tasks");
        for (size_t k = 0; k < json_len(tasks); k++)
            if (json_at(tasks, k) == t) {
                json_free(json_arr_remove(tasks, k));
                break;
            }
        return;
    }
    if (phase && *phase) {
        p = resolve_phase(phases, phase, errs);
        if (p) json_set(p, "tasks", json_arr());
        return;
    }
    json_arr_foreach(phases, i, p) json_set(p, "tasks", json_arr());
}

/* applyEntry; returns the (possibly replaced) phases */
static json_t *apply_entry(json_t *phases, const char *op, const json_t *args, strvec_t *errs) {
    const char *task = json_get_str(args, "task"), *phase = json_get_str(args, "phase");
    jvec_t targets = {0};
    if (!strcmp(op, "init")) {
        json_free(phases);
        return init_phases(args, errs);
    } else if (!strcmp(op, "start")) {
        json_t *t = resolve_task(phases, task, errs, NULL);
        if (t) {
            json_t *p, *x;
            json_arr_foreach(phases, i, p) json_arr_foreach(json_get(p, "tasks"), k, x) {
                if (x != t && str_eq(json_get_str(x, "status"), "in_progress")) json_set_str(x, "status", "pending");
            }
            json_set_str(t, "status", "in_progress");
        }
    } else if (!strcmp(op, "done") || !strcmp(op, "drop")) {
        task_targets(phases, task, phase, errs, &targets);
        for (size_t i = 0; i < targets.n; i++) json_set_str(targets.v[i], "status", op[1] == 'o' ? "completed" : "abandoned");
    } else if (!strcmp(op, "block")) {
        if (!(task && *task) && !(phase && *phase)) {
            strvec_push(errs, "block requires a task or phase target");
        } else {
            char *reason = collapse_ws(json_get_str(args, "reason"));
            task_targets(phases, task, phase, errs, &targets);
            for (size_t i = 0; i < targets.n; i++) {
                const char *st = json_get_str(targets.v[i], "status");
                if (!is_open(st) && !str_eq(st, "blocked")) continue;
                json_set_str(targets.v[i], "status", "blocked");
                if (*reason) json_set_str(targets.v[i], "blocker", reason);
                else json_del(targets.v[i], "blocker");
            }
            free(reason);
        }
    } else if (!strcmp(op, "unblock")) {
        if (!(task && *task) && !(phase && *phase)) {
            strvec_push(errs, "unblock requires a task or phase target");
        } else {
            task_targets(phases, task, phase, errs, &targets);
            for (size_t i = 0; i < targets.n; i++)
                if (str_eq(json_get_str(targets.v[i], "status"), "blocked")) {
                    json_set_str(targets.v[i], "status", "pending");
                    json_del(targets.v[i], "blocker");
                }
        }
    } else if (!strcmp(op, "rm")) {
        remove_tasks(phases, args, errs);
    } else if (!strcmp(op, "append")) {
        append_items(phases, args, errs);
    }
    vec_free(&targets);
    return phases;
}

static char *format_summary(json_t *phases, strvec_t *errors, bool readonly) {
    size_t total = 0, closed = 0, blocked = 0, open = 0;
    json_t *p, *t;
    json_arr_foreach(phases, i, p) {
        json_arr_foreach(json_get(p, "tasks"), k, t) {
            const char *st = json_get_str(t, "status");
            total++;
            if (is_closed(st)) closed++;
            if (str_eq(st, "blocked")) blocked++;
            if (is_open(st)) open++;
        }
    }
    char *errs = errors->n ? strvec_join(errors, "; ") : NULL;
    sb_t b = SB_INIT;
    if (!total) {
        if (errs) sb_appendf(&b, "Errors: %s", errs);
        else sb_append(&b, readonly ? "Todo list is empty." : "Todo list cleared.");
        free(errs);
        return sb_detach(&b);
    }
    if (errs) sb_appendf(&b, "Errors: %s\n", errs);
    free(errs);
    if (!open) sb_append(&b, "Remaining items: none.\n");
    else {
        sb_appendf(&b, "Remaining items (%zu):\n", open);
        json_arr_foreach(phases, i, p) {
            json_arr_foreach(json_get(p, "tasks"), k, t) {
                if (is_open(json_get_str(t, "status")))
                    sb_appendf(&b, "  - %s [%s] (%s)\n", json_get_str(t, "content"), json_get_str(t, "status"),
                               json_get_str(p, "name"));
            }
        }
    }
    size_t nph = json_len(phases), cur = SIZE_MAX;
    json_arr_foreach(phases, i, p) {
        if (cur != SIZE_MAX) break;
        json_arr_foreach(json_get(p, "tasks"), k, t) if (is_open(json_get_str(t, "status"))) cur = i;
    }
    if (cur == SIZE_MAX) cur = nph - 1;
    json_t *cp = json_at(phases, cur);
    size_t cdone = 0;
    json_arr_foreach(json_get(cp, "tasks"), k, t) if (is_closed(json_get_str(t, "status"))) cdone++;
    bool ahead = false;
    for (size_t i = cur + 1; i < nph; i++)
        json_arr_foreach(json_get(json_at(phases, i), "tasks"), k, t) if (is_closed(json_get_str(t, "status"))) ahead = true;
    sb_appendf(&b, "Overall: %zu/%zu done, %zu open", closed, total, open);
    if (blocked) sb_appendf(&b, ", %zu blocked", blocked);
    sb_append(&b, ".\n");
    sb_appendf(&b, "Active phase %zu/%zu \"%s\" (%zu/%zu)%s\n", cur + 1, nph, json_get_str(cp, "name"), cdone,
               json_len(json_get(cp, "tasks")),
               ahead ? " — earliest phase with open tasks; the in-progress pointer auto-advances to the earliest open task "
                       "on each completion, so it can sit behind out-of-order work (nothing was un-completed)."
                     : ".");
    json_arr_foreach(phases, i, p) {
        sb_appendf(&b, "  %s:\n", json_get_str(p, "name"));
        json_arr_foreach(json_get(p, "tasks"), k, t) {
            const char *st = json_get_str(t, "status");
            const char *blk = json_get_str(t, "blocker");
            sb_appendf(&b, "    - %s %s", str_eq(st, "completed") ? "[X]" : "[ ]", json_get_str(t, "content"));
            if (str_eq(st, "in_progress")) sb_append(&b, " (in progress)");
            else if (str_eq(st, "abandoned")) sb_append(&b, " (dropped)");
            else if (str_eq(st, "blocked")) {
                if (blk && *blk) sb_appendf(&b, " (blocked: %s)", blk);
                else sb_append(&b, " (blocked)");
            }
            sb_putc(&b, '\n');
        }
    }
    if (b.len && b.s[b.len - 1] == '\n') sb_truncate(&b, b.len - 1);
    return sb_detach(&b);
}

/* getCompletionTransitions: tasks completed by this call */
static json_t *completion_transitions(const json_t *prev, const json_t *next) {
    json_t *out = json_arr(), *p, *t, *pp, *pt;
    json_arr_foreach(next, i, p) {
        json_arr_foreach(json_get(p, "tasks"), k, t) {
            if (!str_eq(json_get_str(t, "status"), "completed")) continue;
            const char *was = NULL;
            json_arr_foreach(prev, a, pp) {
                if (!str_eq(json_get_str(pp, "name"), json_get_str(p, "name"))) continue;
                json_arr_foreach(json_get(pp, "tasks"), b, pt) if (str_eq(json_get_str(pt, "content"), json_get_str(t, "content")))
                    was = json_get_str(pt, "status");
            }
            if (was && !str_eq(was, "completed")) {
                json_t *x = json_obj();
                json_set_str(x, "phase", json_get_str(p, "name"));
                json_set_str(x, "content", json_get_str(t, "content"));
                json_push(out, x);
            }
        }
    }
    return out;
}

/* inferTodoOp: a missing op recovered from an unambiguous payload */
static const char *infer_op(const json_t *args, bool has_phases) {
    if (json_len(json_get_arr(args, "list"))) return "init";
    if (json_len(json_get_arr(args, "items"))) {
        const char *ph = json_get_str(args, "phase");
        if (ph && *ph) return "append";
        if (!has_phases) return "init";
    }
    return NULL;
}

static int todo_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(ctx);
    pthread_mutex_lock(&env->mu);
    json_t *prev = json_clone(env->todo.phases);
    /* resolveTodoParams: the tool is lenient, so raw args arrive when the schema rejected them */
    json_t *params = json_clone(args);
    char *bad = schema_summary(self->parameters, params);
    if (bad && json_is_obj(params) && !json_get(params, "op")) {
        const char *op = infer_op(params, json_len(prev) > 0);
        if (op) {
            json_t *fixed = json_clone(params);
            json_set_str(fixed, "op", op);
            char *bad2 = schema_summary(self->parameters, fixed);
            if (!bad2) {
                free(bad);
                bad = NULL;
                json_free(params);
                params = fixed;
            } else {
                free(bad2);
                json_free(fixed);
            }
        }
    }
    if (bad) {
        tool_result_error(out, "Invalid todo arguments: %s", bad);
        out->details = json_obj();
        json_set(out->details, "phases", prev);
        json_set_str(out->details, "storage", "session");
        free(bad);
        json_free(params);
        pthread_mutex_unlock(&env->mu);
        return 0;
    }
    const char *op = json_get_str(params, "op");
    bool readonly = !strcmp(op, "view");
    strvec_t errs = {0};
    json_t *next = json_clone(prev);
    if (!readonly) {
        next = apply_entry(next, op, params, &errs);
        normalize(next);
    }
    bool failed = errs.n > 0;
    json_t *effective = failed ? json_clone(prev) : json_clone(next);
    json_t *completed = readonly || failed ? NULL : completion_transitions(prev, next);
    if (!readonly && !failed) {
        json_free(env->todo.phases);
        env->todo.phases = json_clone(next);
    }
    char *summary = format_summary(effective, &errs, readonly);
    tool_result_text(out, summary);
    free(summary);
    out->is_error = failed;
    out->details = json_obj();
    json_set_str(out->details, "op", op);
    json_set(out->details, "phases", effective);
    json_set_str(out->details, "storage", "session");
    if (json_len(completed)) json_set(out->details, "completedTasks", completed);
    else json_free(completed);
    json_free(next);
    json_free(prev);
    json_free(params);
    pthread_mutex_unlock(&env->mu);
    strvec_free(&errs);
    return 0;
}

agent_tool_t *tool_todo_new(tool_env_t *env) {
    char *desc = tool_prompt("todo", NULL);
    json_t *s = schema_lit(
        "{\"type\":\"object\",\"properties\":{\"op\":{\"enum\":[\"init\",\"start\",\"done\",\"rm\",\"drop\",\"block\","
        "\"unblock\",\"append\",\"view\"],\"type\":\"string\"},\"list\":{\"type\":\"array\",\"items\":{\"type\":"
        "\"object\",\"properties\":{\"phase\":{\"type\":\"string\"},\"items\":{\"type\":\"array\",\"items\":{\"type\":"
        "\"string\"},\"minItems\":1}},\"required\":[\"phase\",\"items\"],\"additionalProperties\":false},"
        "\"description\":\"phases for init\"},\"task\":{\"type\":\"string\",\"description\":\"verbatim task content\"},"
        "\"phase\":{\"type\":\"string\"},\"items\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
        "\"description\":\"tasks for flat init or append\"},\"reason\":{\"type\":\"string\",\"description\":"
        "\"blocker note for block\"}},\"required\":[\"op\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("todo", "Todo", desc, s, todo_execute);
    t->lenient_args = true; /* omp lenientArgValidation: execute() reports schema errors itself */
    free(desc);
    t->ud = env;
    return t;
}

/* ---------- ask ---------- */

static bool label_known(const json_t *opts, const char *s) {
    json_t *o;
    json_arr_foreach(opts, i, o) if (str_eq(json_sv(o), s)) return true;
    return false;
}

/* omp AskToolDetails from the dialog answers ({id: label | [labels] | custom text}): one question's
 * fields inline, several questions as `results` */
static json_t *ask_details(const json_t *qs, const json_t *ans) {
    json_t *results = json_arr(), *q;
    json_arr_foreach(qs, i, q) {
        const char *id = json_get_str(q, "id");
        json_t *r = json_obj(), *opts = json_arr(), *sel = json_arr(), *o;
        json_set_str(r, "id", id ? id : "");
        json_set_str(r, "question", json_get_str(q, "question") ? json_get_str(q, "question") : "");
        json_arr_foreach(json_get_arr(q, "options"), k, o) {
            const char *l = json_is_str(o) ? json_sv(o) : json_get_str(o, "label");
            if (l) json_push_str(opts, l);
        }
        json_set(r, "options", opts);
        json_set_bool(r, "multi", json_get_bool(q, "multi", false));
        const json_t *a = json_get(ans, id ? id : "");
        const char *custom = NULL;
        if (json_is_arr(a)) {
            json_t *x;
            json_arr_foreach(a, k, x) {
                if (!json_is_str(x)) continue;
                if (label_known(opts, json_sv(x))) json_push_str(sel, json_sv(x));
                else if (!custom) custom = json_sv(x);
            }
        } else if (json_is_str(a)) {
            if (label_known(opts, json_sv(a))) json_push_str(sel, json_sv(a));
            else custom = json_sv(a);
        }
        json_set(r, "selectedOptions", sel);
        if (custom) json_set_str(r, "customInput", custom);
        json_push(results, r);
    }
    if (json_len(results) == 1) {
        json_t *d = json_clone(json_at(results, 0));
        json_del(d, "id");
        json_free(results);
        return d;
    }
    json_t *d = json_obj();
    json_set(d, "results", results);
    return d;
}

static int ask_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(ctx);
    json_t *qs = json_get_arr(args, "questions");
    if (!qs || !json_len(qs)) {
        tool_result_error(out, "questions must be a non-empty array");
        return 0;
    }
    if (!env->ui.ask) {
        tool_result_error(out, "ask is unavailable: no interactive user is attached to this session. Proceed with your "
                               "best judgment and state your assumptions.");
        return 0;
    }
    json_t *ans = env->ui.ask(env->ui.ud, qs);
    if (!ans) {
        tool_result_error(out, "User cancelled the question.");
        return 0;
    }
    sb_t b = SB_INIT;
    json_t *q;
    json_arr_foreach(qs, i, q) {
        const char *id = json_get_str(q, "id");
        json_t *a = json_get(ans, id ? id : "");
        sb_appendf(&b, "%s: ", id ? id : "answer");
        if (json_is_arr(a)) {
            json_t *x;
            json_arr_foreach(a, k, x) sb_appendf(&b, "%s%s", k ? ", " : "", json_sv(x) ? json_sv(x) : "");
        } else if (json_is_str(a)) {
            sb_append(&b, json_sv(a));
        } else {
            sb_append(&b, "(no answer)");
        }
        sb_putc(&b, '\n');
    }
    tool_result_text(out, sb_cstr(&b));
    out->details = ask_details(qs, ans);
    json_free(ans);
    sb_free(&b);
    return 0;
}

agent_tool_t *tool_ask_new(tool_env_t *env) {
    char *desc = tool_prompt("ask", NULL);
    json_t *s = schema_obj();
    json_t *qs = json_parse_str(
        "{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\"},\"question\":{\"type\":\"string\"},\"header\":{\"type\":\"string\"},"
        "\"options\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},"
        "\"description\":{\"type\":\"string\"},\"preview\":{\"type\":\"string\"}},\"required\":[\"label\"]}},"
        "\"multi\":{\"type\":\"boolean\"},\"recommended\":{\"type\":\"number\"}},"
        "\"required\":[\"id\",\"question\",\"options\"]}}");
    schema_prop_json(s, "questions", qs, true);
    agent_tool_t *t = agent_tool_new("ask", "Ask", desc, s, ask_execute);
    t->exclusive = true;
    free(desc);
    t->ud = env;
    return t;
}
