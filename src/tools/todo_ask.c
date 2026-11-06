/* todo + ask tools */
#include "tools.h"
#include "coding/agents.h"

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

/* ---------- /todo helpers (applyOpsToPhases, Markdown round-trip) ---------- */

json_t *todo_apply_op(const json_t *phases, const char *op, const json_t *args, strvec_t *errors) {
    json_t *next = json_is_arr(phases) ? json_clone(phases) : json_arr();
    next = apply_entry(next, op, args, errors);
    normalize(next);
    return next;
}

static const char *status_marker(const char *st) {
    if (str_eq(st, "in_progress")) return "/";
    if (str_eq(st, "completed")) return "x";
    if (str_eq(st, "abandoned")) return "-";
    if (str_eq(st, "blocked")) return "!";
    return " ";
}

char *todo_phases_to_markdown(const json_t *phases) {
    if (!json_len(phases)) return xstrdup("# Todos\n");
    sb_t b = SB_INIT;
    json_t *p, *t;
    json_arr_foreach(phases, i, p) {
        if (i) sb_putc(&b, '\n');
        sb_appendf(&b, "# %s\n", json_get_str(p, "name") ? json_get_str(p, "name") : "");
        json_arr_foreach(json_get(p, "tasks"), k, t) {
            const char *st = json_get_str(t, "status"), *blocker = json_get_str(t, "blocker");
            sb_appendf(&b, "- [%s] %s", status_marker(st), json_get_str(t, "content") ? json_get_str(t, "content") : "");
            if (str_eq(st, "blocked") && blocker && *blocker) sb_appendf(&b, " <!-- blocker: %s -->", blocker);
            sb_putc(&b, '\n');
        }
    }
    return sb_detach(&b);
}

static bool is_sp(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

json_t *todo_markdown_to_phases(const char *md, strvec_t *errors) {
    json_t *phases = json_arr(), *cur = NULL;
    strvec_t lines = str_split_lines(md, strlen(md));
    for (size_t ln = 0; ln < lines.n; ln++) {
        char *tr = str_trim(lines.v[ln]);
        if (!*tr) {
            free(tr);
            continue;
        }
        const char *p = tr;
        size_t h = 0;
        while (p[h] == '#') h++;
        if (h >= 1 && h <= 6 && is_sp(p[h])) { /* ^#{1,6}\s+(.+?)\s*$ */
            char *name = str_trim(p + h);
            if (*name) {
                cur = json_obj();
                json_set_str(cur, "name", name);
                json_set(cur, "tasks", json_arr());
                json_push(phases, cur);
                free(name);
                free(tr);
                continue;
            }
            free(name);
        }
        /* ^[-*+]\s*\\?\[(.?)\\?\]\s+(.+?)\s*$ */
        const char *q = p;
        bool task = false;
        char marker[8] = "";
        const char *content = NULL;
        if (*q == '-' || *q == '*' || *q == '+') {
            q++;
            while (is_sp(*q)) q++;
            if (*q == '\\') q++;
            if (*q == '[') {
                q++;
                const char *m = q;
                size_t ml = 0;
                if (*m && *m != ']' && !(*m == '\\' && m[1] == ']')) {
                    uint32_t cp;
                    ml = (size_t)utf8_decode(m, strlen(m), &cp);
                }
                q = m + ml;
                if (*q == '\\') q++;
                if (*q == ']' && is_sp(q[1])) {
                    q++;
                    while (is_sp(*q)) q++;
                    if (*q) {
                        task = true;
                        snprintf(marker, sizeof marker, "%.*s", (int)ml, m);
                        content = q;
                    }
                }
            }
        }
        if (!task) {
            strvec_push_own(errors, xasprintf("Line %zu: unrecognized syntax \"%s\"", ln + 1, tr));
            free(tr);
            continue;
        }
        const char *st = !strcmp(marker, " ") || !*marker ? "pending"
                         : !strcmp(marker, "x") || !strcmp(marker, "X") ? "completed"
                         : !strcmp(marker, "/") || !strcmp(marker, ">") ? "in_progress"
                         : !strcmp(marker, "-") || !strcmp(marker, "~") ? "abandoned"
                         : !strcmp(marker, "!") ? "blocked" : NULL;
        if (!st) {
            strvec_push_own(errors, xasprintf("Line %zu: unknown status marker \"[%s]\" (use [ ], [x], [/], [-], [!])", ln + 1, marker));
            free(tr);
            continue;
        }
        if (!cur) {
            cur = json_obj();
            json_set_str(cur, "name", "Todos");
            json_set(cur, "tasks", json_arr());
            json_push(phases, cur);
        }
        char *raw = str_trim(content);
        json_t *t = json_obj();
        const char *cm = strstr(raw, "<!--");
        size_t rl = strlen(raw);
        if (!strcmp(st, "blocked") && cm && rl >= 3 && !strcmp(raw + rl - 3, "-->")) {
            const char *in = cm + 4;
            while (is_sp(*in)) in++;
            if (!strncmp(in, "blocker:", 8)) {
                char *before = xstrndup(raw, (size_t)(cm - raw)), *bt = str_trim(before);
                char *note = xstrndup(in + 8, (size_t)(raw + rl - 3 - (in + 8))), *nt = str_trim(note);
                json_set_str(t, "content", bt);
                json_set_str(t, "status", st);
                json_set_str(t, "blocker", nt);
                free(before), free(bt), free(note), free(nt);
            }
        }
        if (!json_get(t, "content")) {
            json_set_str(t, "content", raw);
            json_set_str(t, "status", st);
        }
        json_push(json_get(cur, "tasks"), t);
        free(raw);
        free(tr);
    }
    strvec_free(&lines);
    normalize(phases);
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

/* one question's result (omp QuestionResult) from the dialog's rich result, or from the legacy
 * {id: label | [labels] | custom} answer object */
static json_t *ask_result(const json_t *q, const json_t *ans) {
    const char *id = json_get_str(q, "id");
    json_t *r = json_obj(), *opts = json_arr(), *o;
    json_set_str(r, "id", id ? id : "");
    json_set_str(r, "question", json_get_str(q, "question") ? json_get_str(q, "question") : "");
    json_arr_foreach(json_get_arr(q, "options"), k, o) {
        const char *l = json_is_str(o) ? json_sv(o) : json_get_str(o, "label");
        if (l) json_push_str(opts, l);
    }
    json_set(r, "options", opts);
    json_set_bool(r, "multi", json_get_bool(q, "multi", false));
    const json_t *rich = NULL, *x;
    json_arr_foreach(json_get_arr(ans, "__results"), k, x) if (str_eq(json_get_str(x, "id"), id)) rich = x;
    if (rich) {
        json_set(r, "selectedOptions", json_clone(json_get(rich, "selectedOptions")));
        static const char *const keys[] = {"customInput", "note", "noteOption", "timedOut"};
        for (size_t k = 0; k < 4; k++)
            if (json_get(rich, keys[k])) json_set(r, keys[k], json_clone(json_get(rich, keys[k])));
        return r;
    }
    json_t *sel = json_arr();
    const json_t *a = json_get(ans, id ? id : "");
    const char *custom = NULL;
    json_t *y;
    if (json_is_arr(a)) {
        json_arr_foreach(a, k, y) {
            if (!json_is_str(y)) continue;
            bool known = false;
            json_arr_foreach(opts, m, o) known |= str_eq(json_sv(o), json_sv(y));
            if (known) json_push_str(sel, json_sv(y));
            else if (!custom) custom = json_sv(y);
        }
    } else if (json_is_str(a)) {
        bool known = false;
        json_arr_foreach(opts, m, o) known |= str_eq(json_sv(o), json_sv(a));
        if (known) json_push_str(sel, json_sv(a));
        else custom = json_sv(a);
    }
    json_set(r, "selectedOptions", sel);
    if (custom) json_set_str(r, "customInput", custom);
    return r;
}

static void indented(sb_t *b, const char *text) {
    for (const char *p = text;;) {
        const char *nl = strchr(p, '\n');
        sb_appendf(b, "\n  %.*s", (int)(nl ? nl - p : (ptrdiff_t)strlen(p)), p);
        if (!nl) break;
        p = nl + 1;
    }
}

/* formatSingleQuestionResponse */
static char *ask_single_text(const json_t *r) {
    sb_t b = SB_INIT;
    json_t *sel = json_get_arr(r, "selectedOptions"), *x;
    bool multi = json_get_bool(r, "multi", false);
    const char *custom = json_get_str(r, "customInput"), *note = json_get_str(r, "note"), *on = json_get_str(r, "noteOption");
    if (json_len(sel)) {
        sb_append(&b, "User selected: ");
        if (multi) json_arr_foreach(sel, i, x) sb_appendf(&b, "%s%s", i ? ", " : "", json_sv(x));
        else sb_append(&b, json_sv(json_at(sel, 0)));
        if (json_get_bool(r, "timedOut", false)) sb_append(&b, " (auto-selected after timeout)");
    }
    if (custom) {
        if (b.len) sb_putc(&b, '\n');
        if (strchr(custom, '\n')) {
            sb_append(&b, "User provided custom input:");
            indented(&b, custom);
        } else sb_appendf(&b, "User provided custom input: %s", custom);
    }
    if (note && *note) {
        if (b.len) sb_putc(&b, '\n');
        char *label = on ? xasprintf("User added note on %s:", on) : xstrdup("User added note:");
        if (strchr(note, '\n')) {
            sb_append(&b, label);
            indented(&b, note);
        } else sb_appendf(&b, "%s %s", label, note);
        free(label);
    }
    if (!b.len) sb_append(&b, multi ? "User did not select any options" : "User cancelled the selection");
    return sb_detach(&b);
}

/* formatQuestionResult */
static void ask_result_line(sb_t *b, const json_t *r) {
    const char *id = json_get_str(r, "id"), *custom = json_get_str(r, "customInput"), *note = json_get_str(r, "note"),
               *on = json_get_str(r, "noteOption");
    bool multi = json_get_bool(r, "multi", false);
    char *suffix = note && *note ? (on ? xasprintf(" (note on %s: %s)", on, note) : xasprintf(" (note: %s)", note)) : xstrdup("");
    json_t *sel = json_get_arr(r, "selectedOptions"), *x;
    if (!json_len(sel)) {
        if (custom) sb_appendf(b, "%s: \"%s\"%s", id, custom, suffix);
        else sb_appendf(b, "%s: %s%s", id, multi ? "[]" : "(cancelled)", suffix);
        free(suffix);
        return;
    }
    sb_appendf(b, "%s: ", id);
    if (multi) {
        sb_putc(b, '[');
        json_arr_foreach(sel, i, x) sb_appendf(b, "%s%s", i ? ", " : "", json_sv(x));
        sb_putc(b, ']');
    } else sb_append(b, json_sv(json_at(sel, 0)));
    if (custom) sb_appendf(b, " + \"%s\"", custom);
    if (json_get_bool(r, "timedOut", false)) sb_append(b, " (auto-selected after timeout)");
    sb_append(b, suffix);
    free(suffix);
}

static char *strip_cr(const char *s) {
    /* sanitizeCarriageReturns: \r\n -> \n, other \r runs -> one space */
    sb_t b = SB_INIT;
    for (; s && *s; s++) {
        if (*s != '\r') {
            sb_putc(&b, *s);
            continue;
        }
        if (s[1] == '\n') continue;
        while (s[1] == '\r' && s[2] != '\n') s++;
        sb_putc(&b, ' ');
    }
    return sb_detach(&b);
}

/* ToolAbortError: a cancelled ask ends the run (context.abort()) */
static int ask_cancelled(tool_ctx_t *ctx, tool_result_t *out) {
    tool_result_error(out, "Ask tool was cancelled by the user");
    if (ctx && ctx->agent) agent_abort(ctx->agent);
    return 0;
}

static int ask_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    json_t *qs = json_get_arr(args, "questions"), *q, *o;
    if (!env->ui.ask) {
        tool_result_error(out, "Ask tool requires interactive mode");
        return 0;
    }
    /* reserved runtime labels, duplicate ids / labels (omp fails closed with a plain text result) */
    static const char *const reserved[] = {"Other (type your own)", "Chat about this", "Next \xe2\x86\x92"};
    json_arr_foreach(qs, i, q) json_arr_foreach(json_get_arr(q, "options"), k, o) {
        char *l = strip_cr(json_get_str(o, "label"));
        for (size_t r = 0; r < 3; r++)
            if (!strcmp(l, reserved[r])) {
                tool_result_textf(out, "Error: option labels must not collide with reserved runtime labels: %s", l);
                out->details = json_obj();
                free(l);
                return 0;
            }
        free(l);
    }
    strvec_t ids = {0};
    json_arr_foreach(qs, i, q) {
        char *id = strip_cr(json_get_str(q, "id"));
        for (size_t k = 0; k < ids.n; k++)
            if (!strcmp(ids.v[k], id)) {
                tool_result_textf(out, "Error: question ids must be unique: %s", id);
                out->details = json_obj();
                free(id);
                strvec_free(&ids);
                return 0;
            }
        strvec_push_own(&ids, id);
        strvec_t labels = {0};
        json_arr_foreach(json_get_arr(q, "options"), k, o) {
            char *l = strip_cr(json_get_str(o, "label"));
            for (size_t m = 0; m < labels.n; m++)
                if (!strcmp(labels.v[m], l)) {
                    tool_result_textf(out, "Error: option labels must be unique within a question: %s", l);
                    out->details = json_obj();
                    free(l);
                    strvec_free(&labels);
                    strvec_free(&ids);
                    return 0;
                }
            strvec_push_own(&labels, l);
        }
        strvec_free(&labels);
    }
    strvec_free(&ids);
    if (!json_len(qs)) {
        tool_result_text(out, "Error: questions must not be empty");
        out->details = json_obj();
        return 0;
    }
    agents_user_wait(env, true);
    json_t *ans = env->ui.ask(env->ui.ud, qs);
    agents_user_wait(env, false);
    if (!ans) return ask_cancelled(ctx, out);
    if (json_get_str(ans, "__error")) { /* the host's answer failed validation, or the host went away */
        tool_result_error(out, "%s", json_get_str(ans, "__error"));
        json_free(ans);
        return 0;
    }
    json_t *results = json_arr();
    json_arr_foreach(qs, i, q) json_push(results, ask_result(q, ans));
    json_free(ans);
    if (json_len(results) == 1) {
        json_t *r = json_at(results, 0);
        if (!json_get_bool(r, "timedOut", false) && !json_get_bool(r, "multi", false) &&
            !json_len(json_get_arr(r, "selectedOptions")) && !json_get(r, "customInput")) {
            json_free(results);
            return ask_cancelled(ctx, out);
        }
        char *text = ask_single_text(r);
        tool_result_text(out, text);
        free(text);
        json_t *d = json_clone(r);
        json_del(d, "id");
        out->details = d;
        json_free(results);
        return 0;
    }
    sb_t b = SB_INIT;
    sb_append(&b, "User answers:");
    json_t *r;
    json_arr_foreach(results, i, r) {
        sb_putc(&b, '\n');
        ask_result_line(&b, r);
    }
    tool_result_text(out, sb_cstr(&b));
    sb_free(&b);
    out->details = json_obj();
    json_set(out->details, "results", results);
    return 0;
}

agent_tool_t *tool_ask_new(tool_env_t *env) {
    char *desc = tool_prompt("ask", NULL);
    json_t *s = schema_obj();
    /* omp's wire schema (ask.ts askSchema through toolWireSchema): described optional fields, closed objects */
    json_t *qs = json_parse_str(
        "{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\"},\"question\":{\"type\":\"string\"},"
        "\"options\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},"
        "\"description\":{\"type\":\"string\"},\"preview\":{\"type\":\"string\",\"description\":\"rich preview\"}},"
        "\"required\":[\"label\"],\"additionalProperties\":false}},"
        "\"header\":{\"type\":\"string\",\"description\":\"display chip\"},\"multi\":{\"type\":\"boolean\"},"
        "\"recommended\":{\"type\":\"number\",\"description\":\"0-based default index\"}},"
        "\"required\":[\"id\",\"question\",\"options\"],\"additionalProperties\":false},\"minItems\":1}");
    schema_prop_json(s, "questions", qs, true);
    json_set_bool(s, "additionalProperties", false);
    agent_tool_t *t = agent_tool_new("ask", "Ask", desc, s, ask_execute);
    t->exclusive = true;
    t->examples = json_parse_str("[{\"caption\":\"Choice\",\"call\":{\"questions\":[{\"id\":\"storage\",\"question\":"
                                 "\"Database?\",\"options\":[{\"label\":\"SQLite\"},{\"label\":\"Postgres\"}],"
                                 "\"recommended\":0}]}}]");
    free(desc);
    t->ud = env;
    return t;
}
