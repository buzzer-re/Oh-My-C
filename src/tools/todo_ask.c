/* todo + ask tools */
#include "tools.h"
#include <ctype.h>
#include "coding/agents.h"
#include "config/settings.h"
#include "text/htmlform.h"
#include "tui/symbols.h"

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

static void task_targets(json_t *phases, const json_t *task_arg, const char *phase, strvec_t *errs, jvec_t *out) {
    strvec_t contents = {0};
    todo_task_targets(phases, task_arg, &contents);
    if (contents.n) {
        for (size_t i = 0; i < contents.n; i++) {
            json_t *t = resolve_task(phases, contents.v[i], errs, NULL);
            if (t) vec_push(out, t);
        }
        strvec_free(&contents);
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
        if (phase) json_set_str(e, "phase", phase);
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
        /* an empty phase name cannot be explicitly targeted, so it gets the default */
        const char *nm = json_get_str(e, "phase");
        if (!nm || !*nm) nm = "Tasks";
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
    if (phase && !*phase) {
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
    /* an omitted phase: the in-progress task's, else the first pending task's, else the last phase, else `Tasks` */
    if (!phase) {
        json_t *active = NULL, *first_pending = NULL, *q, *t;
        json_arr_foreach(phases, i, q) {
            json_arr_foreach(json_get(q, "tasks"), k, t) {
                const char *st = json_get_str(t, "status");
                if (!active && str_eq(st, "in_progress")) active = q;
                if (!first_pending && str_eq(st, "pending")) first_pending = q;
            }
        }
        json_t *sel = active ? active : first_pending ? first_pending : json_len(phases) ? json_at(phases, json_len(phases) - 1) : NULL;
        phase = sel ? json_get_str(sel, "name") : NULL;
        if (!phase) phase = "Tasks";
    }
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
    const char *phase = json_get_str(args, "phase");
    json_t *p;
    strvec_t contents = {0};
    todo_task_targets(phases, json_get(args, "task"), &contents);
    if (contents.n) {
        for (size_t c = 0; c < contents.n; c++) {
            json_t *owner = NULL;
            json_t *t = resolve_task(phases, contents.v[c], errs, &owner);
            if (!t) continue;
            json_t *tasks = json_get(owner, "tasks");
            for (size_t k = 0; k < json_len(tasks); k++)
                if (json_at(tasks, k) == t) {
                    json_free(json_arr_remove(tasks, k));
                    break;
                }
        }
        strvec_free(&contents);
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
    const json_t *task_arg = json_get(args, "task");
    const char *phase = json_get_str(args, "phase");
    /* a task target: a non-empty string or a non-empty array */
    bool has_task = (json_is_str(task_arg) && *json_sv(task_arg)) || json_len(json_is_arr(task_arg) ? task_arg : NULL);
    jvec_t targets = {0};
    if (!strcmp(op, "init")) {
        json_free(phases);
        return init_phases(args, errs);
    } else if (!strcmp(op, "start")) {
        strvec_t contents = {0};
        todo_task_targets(phases, task_arg, &contents);
        json_t *t = NULL;
        if (contents.n > 1) strvec_push(errs, "start takes a single task: only one task can be in progress");
        else t = resolve_task(phases, contents.n ? contents.v[0] : NULL, errs, NULL);
        strvec_free(&contents);
        if (t) {
            json_t *p, *x;
            json_arr_foreach(phases, i, p) json_arr_foreach(json_get(p, "tasks"), k, x) {
                if (x != t && str_eq(json_get_str(x, "status"), "in_progress")) json_set_str(x, "status", "pending");
            }
            json_set_str(t, "status", "in_progress");
        }
    } else if (!strcmp(op, "done") || !strcmp(op, "drop")) {
        task_targets(phases, task_arg, phase, errs, &targets);
        for (size_t i = 0; i < targets.n; i++) json_set_str(targets.v[i], "status", op[1] == 'o' ? "completed" : "abandoned");
    } else if (!strcmp(op, "block")) {
        if (!has_task && !(phase && *phase)) {
            strvec_push(errs, "block requires a task or phase target");
        } else {
            char *reason = collapse_ws(json_get_str(args, "reason"));
            task_targets(phases, task_arg, phase, errs, &targets);
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
        if (!has_task && !(phase && *phase)) {
            strvec_push(errs, "unblock requires a task or phase target");
        } else {
            task_targets(phases, task_arg, phase, errs, &targets);
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
        "\"description\":\"phases for init\"},\"task\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"array\",\"items\":"
        "{\"type\":\"string\"}}],\"description\":\"verbatim task content; array targets several (not start)\"},"
        "\"phase\":{\"type\":\"string\"},\"items\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
        "\"description\":\"tasks for flat init or append\"},\"reason\":{\"type\":\"string\",\"description\":"
        "\"blocker note for block\"}},\"required\":[\"op\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("todo", "Todo", desc, s, todo_execute);
    t->lenient_args = true; /* omp lenientArgValidation: execute() reports schema errors itself */
    free(desc);
    t->ud = env;
    return t;
}

/* ---------- ask (port of omp tools/ask.ts: one HTML form) ---------- */

#define NO_NAMED_CONTROLS_ERROR                                                                                        \
    "Error: the form has no named controls. Put at least one <input>, <select> or <textarea> with a `name` "          \
    "attribute inside the <form>."
#define OTHER_ROW HF_OTHER_LABEL " (type your own)"

typedef struct {
    tool_env_t *env;
    int64_t timeout;
    volatile sig_atomic_t *cancel;
    char *err; /* the host failed (owned) */
} fb_ui_t;

/* disambiguateDisplayLabels: a label already taken gets ` (2)`, ` (3)`, ... */
static void disambiguate(strvec_t *labels, const strvec_t *reserved) {
    strvec_t taken = {0};
    for (size_t i = 0; reserved && i < reserved->n; i++) strvec_push(&taken, reserved->v[i]);
    for (size_t i = 0; i < labels->n; i++) {
        char *base = xstrdup(labels->v[i]), *cand = xstrdup(base);
        for (int suffix = 2;; suffix++) {
            bool dup = false;
            for (size_t k = 0; k < taken.n && !dup; k++) dup = !strcmp(taken.v[k], cand);
            if (!dup) break;
            free(cand);
            cand = xasprintf("%s (%d)", base, suffix);
        }
        strvec_push(&taken, cand);
        free(labels->v[i]);
        labels->v[i] = cand;
        free(base);
    }
    strvec_free(&taken);
}

static int index_of(const strvec_t *v, const char *s) {
    for (size_t i = 0; s && i < v->n; i++)
        if (!strcmp(v->v[i], s)) return (int)i;
    return -1;
}

/* askChoice: checked option indices (and an Other text) for a choice field; *timed_out at a deadline; false
 * when cancelled */
static bool ask_choice(fb_ui_t *ui, const hf_field_t *f, const char *question, bool *checked, char **other,
                       bool *timed_out) {
    tool_ui_t *u = &ui->env->ui;
    strvec_t labels = {0};
    VEC(size_t) opts = {0};
    for (size_t i = 0; i < f->opts.n; i++) {
        if (f->opts.v[i].disabled) continue;
        char *t = hf_spans_text(&f->opts.v[i].label), *l = hf_one_line(t);
        free(t);
        if (!*l) {
            free(l);
            l = xstrdup(f->opts.v[i].value);
        }
        strvec_push_own(&labels, l);
        vec_push(&opts, i);
    }
    disambiguate(&labels, NULL);
    strvec_t rows = {0};
    for (size_t i = 0; i < labels.n; i++) strvec_push(&rows, labels.v[i]);
    char *other_row = NULL;
    if (f->other) {
        strvec_t one = {0};
        strvec_push(&one, OTHER_ROW);
        disambiguate(&one, &labels);
        other_row = xstrdup(one.v[0]);
        strvec_free(&one);
        strvec_push(&rows, other_row);
    }
    bool ok = true;
    *timed_out = false;
    *other = NULL;
    if (!f->multi) {
        for (;;) {
            bool to = false;
            char *pick = u->select(u->ud, question, &rows, ui->timeout, &to, ui->cancel, &ui->err);
            if (other_row && str_eq(pick, other_row)) {
                free(pick);
                bool to2 = false;
                char *typed = u->input(u->ud, question, NULL, ui->timeout, &to2, ui->cancel, &ui->err);
                if (to2) {
                    *timed_out = true;
                    ok = false;
                    free(typed);
                    break;
                }
                if (ui->err || (ui->cancel && *ui->cancel)) {
                    free(typed);
                    ok = false;
                    break;
                }
                char *t = typed ? str_trim(typed) : NULL;
                bool blank = !t || !*t;
                free(t);
                if (blank) {
                    free(typed);
                    continue;
                }
                memset(checked, 0, f->opts.n * sizeof(bool));
                *other = typed;
                break;
            }
            int row = index_of(&labels, pick);
            free(pick);
            *timed_out = to;
            if (row < 0) {
                ok = false; /* cancelled, or the deadline (field keeps its default) */
                break;
            }
            memset(checked, 0, f->opts.n * sizeof(bool));
            checked[opts.v[row]] = true;
            break;
        }
    } else {
        char *done = xasprintf("%s Done selecting", tui_sym("status.success"));
        strvec_t one = {0};
        strvec_push_own(&one, done);
        disambiguate(&one, &rows);
        done = xstrdup(one.v[0]);
        strvec_free(&one);
        for (;;) {
            size_t answered = (size_t)(*other != NULL);
            for (size_t i = 0; i < f->opts.n; i++) answered += checked[i];
            strvec_t shown = {0};
            for (size_t i = 0; i < rows.n; i++) strvec_push(&shown, rows.v[i]);
            if (!f->required || answered) strvec_push(&shown, done);
            char *title = answered ? xasprintf("(%zu selected) %s", answered, question) : xstrdup(question);
            bool to = false;
            char *pick = u->select(u->ud, title, &shown, ui->timeout, &to, ui->cancel, &ui->err);
            free(title);
            strvec_free(&shown);
            if (to) { /* a deadline keeps whatever was checked so far */
                free(pick);
                *timed_out = true;
                break;
            }
            if (!pick) {
                ok = false;
                break;
            }
            if (!strcmp(pick, done)) {
                free(pick);
                break;
            }
            if (other_row && !strcmp(pick, other_row)) {
                free(pick);
                if (*other) {
                    free(*other);
                    *other = NULL;
                    continue;
                }
                bool to2 = false;
                char *typed = u->input(u->ud, question, NULL, ui->timeout, &to2, ui->cancel, &ui->err);
                if (to2) {
                    free(typed);
                    *timed_out = true;
                    break;
                }
                if (ui->err || (ui->cancel && *ui->cancel)) {
                    free(typed);
                    ok = false;
                    break;
                }
                char *t = typed ? str_trim(typed) : NULL;
                if (t && *t) *other = typed; /* a blank or cancelled answer leaves Other unchecked */
                else free(typed);
                free(t);
                continue;
            }
            int row = index_of(&labels, pick);
            free(pick);
            if (row < 0) continue;
            checked[opts.v[row]] = !checked[opts.v[row]];
        }
        free(done);
    }
    strvec_free(&rows);
    strvec_free(&labels);
    free(opts.v);
    free(other_row);
    return ok;
}

/* askText: re-asked with the validation error until valid; textareas and prefilled inputs open the editor on
 * their current value, empty inputs prompt with their placeholder. NULL when cancelled or timed out */
static char *ask_text(fb_ui_t *ui, const hf_field_t *f, const char *question, bool *timed_out) {
    tool_ui_t *u = &ui->env->ui;
    bool use_editor = f->multiline || *f->value;
    char *error = NULL, *current = xstrdup(f->value);
    *timed_out = false;
    for (;;) {
        char *title = error ? xasprintf("%s (%s)", question, error) : xstrdup(question);
        char *value;
        if (use_editor) value = u->editor(u->ud, title, current, !f->multiline, ui->cancel, &ui->err);
        else value = u->input(u->ud, title, *f->placeholder ? f->placeholder : NULL, ui->timeout, timed_out, ui->cancel, &ui->err);
        free(title);
        if (!value) {
            free(error);
            free(current);
            return NULL;
        }
        json_t *jv = json_str(value);
        free(error);
        error = hf_field_error(f, jv);
        json_free(jv);
        if (!error) {
            free(current);
            return value;
        }
        free(current);
        current = value;
    }
}

/* askFieldByField: undefined (NULL) when cancelled; a deadline stops the walk with the rest at their defaults */
static json_t *ask_field_by_field(fb_ui_t *ui, const hf_form_t *form, bool *timed_out) {
    json_t *data = hf_default_data(form);
    *timed_out = false;
    for (size_t i = 0; i < form->fields.n; i++) {
        const hf_field_t *f = form->fields.v[i];
        if (!*f->name) continue;
        const char *question = f->question ? f->question : f->name;
        bool to = false;
        if (f->kind == HF_CHOICE) {
            bool *checked = xcalloc(f->opts.n + 1, sizeof *checked);
            for (size_t k = 0; k < f->opts.n; k++) checked[k] = f->opts.v[k].checked;
            char *other = NULL;
            bool ok = ask_choice(ui, f, question, checked, &other, &to);
            if (ok || (to && f->multi)) {
                json_t *v = hf_choice_value(f, checked, other);
                if (v) json_set(data, f->name, v);
                else json_del(data, f->name);
            }
            free(other);
            free(checked);
            if (to) {
                *timed_out = true;
                return data;
            }
            if (!ok) {
                json_free(data);
                return NULL;
            }
            continue;
        }
        char *text = ask_text(ui, f, question, &to);
        if (to) {
            *timed_out = true;
            return data;
        }
        if (!text) {
            json_free(data);
            return NULL;
        }
        json_set(data, f->name, hf_text_value(f, text));
        free(text);
    }
    return data;
}

/* ToolAbortError: a cancelled ask ends the run (context.abort()) */
static int ask_cancelled(tool_ctx_t *ctx, tool_result_t *out) {
    tool_result_error(out, "Ask tool was cancelled by the user");
    if (ctx && ctx->agent) agent_abort_user(ctx->agent); /* sdk.ts tool context abort(): USER_INTERRUPT_LABEL */
    return 0;
}

static int ask_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    if (!env->ui.form && !(env->ui.select && env->ui.input && env->ui.editor)) {
        tool_result_error(out, "Ask tool requires interactive mode");
        return 0;
    }
    const char *html = json_get_str(args, "html");
    hf_form_t *form = hf_parse(html ? html : "");
    if (!hf_named_count(form)) {
        hf_form_free(form);
        tool_result_text(out, NO_NAMED_CONTROLS_ERROR);
        out->details = json_obj();
        json_set(out->details, "answers", json_arr());
        return 0;
    }
    bool plan = env->plan_mode_active && env->plan_mode_active(env);
    double secs = settings_num("ask.timeout", 0);
    int64_t timeout = plan || !(secs > 0) ? 0 : (int64_t)(secs * 1000);
    volatile sig_atomic_t *cancel = ctx && ctx->agent ? &ctx->agent->cancel : NULL;
    bool timed_out = false;
    char *err = NULL;
    json_t *data;
    agents_user_wait(env, true);
    if (env->ui.form) data = env->ui.form(env->ui.ud, html ? html : "", timeout, &timed_out, cancel, &err);
    else {
        fb_ui_t fb = {env, timeout, cancel, NULL};
        data = ask_field_by_field(&fb, form, &timed_out);
        err = fb.err;
        if (err) {
            json_free(data);
            data = NULL;
        }
    }
    agents_user_wait(env, false);
    if (err) {
        tool_result_error(out, "%s", err);
        free(err);
        hf_form_free(form);
        return 0;
    }
    if (!data) {
        hf_form_free(form);
        return ask_cancelled(ctx, out);
    }
    char *json = hf_data_json(data);
    out->details = json_obj();
    json_set(out->details, "answers", hf_answers(form, data));
    if (timed_out) {
        json_set_bool(out->details, "timedOut", true);
        tool_result_textf(out, "No answer before the timeout; submitted the form's current values:\n%s", json);
    } else tool_result_textf(out, "User answers:\n%s", json);
    free(json);
    json_free(data);
    hf_form_free(form);
    return 0;
}

static const char ASK_EXAMPLE_FORM[] =
    "<form>\n"
    "  <p>Sessions live in memory and are lost on restart.</p>\n"
    "  <fieldset>\n"
    "    <legend>Session store</legend>\n"
    "    <label><input type=\"radio\" name=\"store\" value=\"sqlite\" checked> SQLite: one file, no new service</label>\n"
    "    <label><input type=\"radio\" name=\"store\" value=\"redis\"> Redis: shared across instances, needs ops</label>\n"
    "  </fieldset>\n"
    "  <label>Session lifetime (days) <input type=\"number\" name=\"ttl_days\" value=\"30\" min=\"1\"></label>\n"
    "</form>";

agent_tool_t *tool_ask_new(tool_env_t *env) {
    char *desc = tool_prompt("ask", NULL);
    json_t *s = schema_obj();
    /* omp's wire schema (ask.ts askSchema through toolWireSchema) */
    json_t *h = json_parse_str("{\"type\":\"string\",\"description\":\"one HTML <form>\"}");
    schema_prop_json(s, "html", h, true);
    json_set_bool(s, "additionalProperties", false);
    agent_tool_t *t = agent_tool_new("ask", "Ask", desc, s, ask_execute);
    t->exclusive = true;
    json_t *ex = json_arr(), *e = json_obj(), *call = json_obj();
    json_set_str(e, "caption", "Choice with context and a number");
    json_set_str(call, "html", ASK_EXAMPLE_FORM);
    json_set(e, "call", call);
    json_push(ex, e);
    t->examples = ex;
    free(desc);
    t->ud = env;
    return t;
}
