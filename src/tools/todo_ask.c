/* todo + ask tools */
#include "tools.h"

/* ---------- todo ---------- */

static bool is_open(const char *st) { return str_eq(st, "pending") || str_eq(st, "in_progress"); }
static bool is_closed(const char *st) { return str_eq(st, "completed") || str_eq(st, "abandoned"); }

static json_t *new_task(const char *content) {
    json_t *t = json_obj();
    json_set_str(t, "content", content);
    json_set_str(t, "status", "pending");
    return t;
}

static json_t *new_phase(const char *name, const json_t *items) {
    json_t *p = json_obj();
    json_set_str(p, "name", name);
    json_t *tasks = json_arr(), *it;
    json_arr_foreach(items, i, it) {
        if (json_is_str(it)) json_push(tasks, new_task(json_sv(it)));
    }
    json_set(p, "tasks", tasks);
    return p;
}

static json_t *find_task(json_t *phases, const char *content) {
    json_t *p, *t;
    json_arr_foreach(phases, i, p) {
        json_arr_foreach(json_get(p, "tasks"), k, t) {
            if (str_eq(json_get_str(t, "content"), content)) return t;
        }
    }
    return NULL;
}

static json_t *find_phase(json_t *phases, const char *name) {
    json_t *p;
    json_arr_foreach(phases, i, p) if (str_eq(json_get_str(p, "name"), name)) return p;
    return NULL;
}

/* apply fn to target task / phase / all; returns number matched */
static int for_targets(json_t *phases, const char *task, const char *phase, void (*fn)(json_t *t, const char *arg),
                       const char *arg, strvec_t *errs) {
    int n = 0;
    if (task) {
        json_t *t = find_task(phases, task);
        if (!t) {
            strvec_push_own(errs, xasprintf("task not found: %s", task));
            return 0;
        }
        fn(t, arg);
        return 1;
    }
    json_t *p, *t;
    json_arr_foreach(phases, i, p) {
        if (phase && !str_eq(json_get_str(p, "name"), phase)) continue;
        json_arr_foreach(json_get(p, "tasks"), k, t) {
            fn(t, arg);
            n++;
        }
    }
    if (phase && !find_phase(phases, phase)) strvec_push_own(errs, xasprintf("phase not found: %s", phase));
    return n;
}

static void set_completed(json_t *t, const char *a) {
    UNUSED(a);
    json_set_str(t, "status", "completed");
    json_del(t, "blocker");
}
static void set_abandoned(json_t *t, const char *a) {
    UNUSED(a);
    if (!str_eq(json_get_str(t, "status"), "completed")) json_set_str(t, "status", "abandoned");
}
static void set_blocked(json_t *t, const char *reason) {
    if (is_closed(json_get_str(t, "status"))) return;
    json_set_str(t, "status", "blocked");
    if (reason && *reason) json_set_str(t, "blocker", reason);
}
static void set_unblocked(json_t *t, const char *a) {
    UNUSED(a);
    if (str_eq(json_get_str(t, "status"), "blocked")) {
        json_set_str(t, "status", "pending");
        json_del(t, "blocker");
    }
}

/* invariant: exactly the earliest open task is in_progress if none is */
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
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') sp = true;
        else {
            if (sp && b.len) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *s);
        }
    }
    return sb_detach(&b);
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
                if (blk) sb_appendf(&b, " (blocked: %s)", blk);
                else sb_append(&b, " (blocked)");
            }
            sb_putc(&b, '\n');
        }
    }
    if (b.len && b.s[b.len - 1] == '\n') sb_truncate(&b, b.len - 1);
    return sb_detach(&b);
}

static int todo_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(ctx);
    const char *op = json_get_str(args, "op");
    const char *task = json_get_str(args, "task");
    const char *phase = json_get_str(args, "phase");
    json_t *items = json_get_arr(args, "items");
    json_t *list = json_get_arr(args, "list");
    if (!op) op = list ? "init" : items && !phase ? "init" : "view";
    pthread_mutex_lock(&env->mu);
    json_t *ph = json_clone(env->todo.phases);
    strvec_t errs = {0};
    bool readonly = false;
    if (!strcmp(op, "init")) {
        json_free(ph);
        ph = json_arr();
        if (list) {
            json_t *e;
            json_arr_foreach(list, i, e) {
                const char *nm = json_get_str(e, "phase");
                json_push(ph, new_phase(nm ? nm : "Tasks", json_get_arr(e, "items")));
            }
        } else if (items) {
            json_push(ph, new_phase(phase ? phase : "Tasks", items));
        } else {
            strvec_push(&errs, "init requires list or items");
        }
    } else if (!strcmp(op, "start")) {
        json_t *t = task ? find_task(ph, task) : NULL;
        if (!t) strvec_push_own(&errs, xasprintf("task not found: %s", task ? task : "(none)"));
        else {
            json_t *p, *x;
            json_arr_foreach(ph, i, p) json_arr_foreach(json_get(p, "tasks"), k, x) {
                if (str_eq(json_get_str(x, "status"), "in_progress")) json_set_str(x, "status", "pending");
            }
            json_set_str(t, "status", "in_progress");
        }
    } else if (!strcmp(op, "done")) {
        for_targets(ph, task, phase, set_completed, NULL, &errs);
    } else if (!strcmp(op, "drop")) {
        for_targets(ph, task, phase, set_abandoned, NULL, &errs);
    } else if (!strcmp(op, "block")) {
        char *reason = collapse_ws(json_get_str(args, "reason"));
        if (!task && !phase) strvec_push(&errs, "block requires task or phase");
        else for_targets(ph, task, phase, set_blocked, reason, &errs);
        free(reason);
    } else if (!strcmp(op, "unblock")) {
        if (!task && !phase) strvec_push(&errs, "unblock requires task or phase");
        else for_targets(ph, task, phase, set_unblocked, NULL, &errs);
    } else if (!strcmp(op, "rm")) {
        if (task) {
            bool found = false;
            json_t *p;
            json_arr_foreach(ph, i, p) {
                json_t *tasks = json_get(p, "tasks");
                for (size_t k = 0; k < json_len(tasks); k++) {
                    if (str_eq(json_get_str(json_at(tasks, k), "content"), task)) {
                        json_free(json_arr_remove(tasks, k));
                        found = true;
                        break;
                    }
                }
            }
            if (!found) strvec_push_own(&errs, xasprintf("task not found: %s", task));
        } else if (phase) {
            json_t *p = find_phase(ph, phase);
            if (!p) strvec_push_own(&errs, xasprintf("phase not found: %s", phase));
            else json_set(p, "tasks", json_arr());
        } else {
            json_free(ph);
            ph = json_arr();
        }
    } else if (!strcmp(op, "append")) {
        if (!phase || !items) strvec_push(&errs, "append requires phase and items");
        else {
            json_t *p = find_phase(ph, phase);
            if (!p) {
                p = new_phase(phase, NULL);
                json_push(ph, p);
            }
            json_t *it;
            json_arr_foreach(items, i, it) if (json_is_str(it)) json_push(json_get(p, "tasks"), new_task(json_sv(it)));
        }
    } else if (!strcmp(op, "view")) {
        readonly = true;
    } else {
        strvec_push_own(&errs, xasprintf("unknown op: %s", op));
    }
    if (errs.n) {
        json_free(ph);
        ph = json_clone(env->todo.phases);
        out->is_error = true;
    } else if (!readonly) {
        normalize(ph);
        json_free(env->todo.phases);
        env->todo.phases = json_clone(ph);
    }
    char *summary = format_summary(ph, &errs, readonly);
    tool_result_text(out, summary);
    free(summary);
    out->details = json_obj();
    json_set(out->details, "phases", ph);
    json_set_str(out->details, "storage", "session");
    json_set_str(out->details, "op", op);
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
