/* todocmd.c - /todo verbs (port of omp modes/controllers/todo-command-controller.ts): show, edit ($VISUAL/$EDITOR
 * Markdown round-trip), copy, expand/collapse, export/import, append, start, done, drop, rm. A change replaces the
 * session todo state, is persisted as a `user_todo_edit` custom entry and tells the model through a developer
 * <system-reminder> message. */
#define _GNU_SOURCE /* memmem, strcasestr, vasprintf: glibc before 2.38 declares them only with it */
#include "ui/ui.h"

#include "tools/tools.h"

#include <ctype.h>

static const char USAGE[] = "Usage: /todo <verb> [args]\n"
                            "  /todo                              Show current todos\n"
                            "  /todo edit                         Open todos in $EDITOR\n"
                            "  /todo copy                         Copy todos as Markdown to clipboard\n"
                            "  /todo expand                       Show every phase and task in the HUD\n"
                            "  /todo collapse                     Restore the bounded HUD preview\n"
                            "  /todo export [<path>]              Write todos to file (default: TODO.md)\n"
                            "  /todo import [<path>]              Replace todos from file (default: TODO.md)\n"
                            "  /todo append [<phase>] <task...>   Append a task; phase fuzzy-matched or auto-created\n"
                            "  /todo start  <task>                Mark task in_progress (fuzzy content match)\n"
                            "  /todo done   [<task|phase>]        Mark task/phase/all completed\n"
                            "  /todo drop   [<task|phase>]        Mark task/phase/all abandoned\n"
                            "  /todo rm     [<task|phase>]        Remove task/phase/all";

/* the HUD expansion flag (omc has no live todo HUD yet; kept for when it lands) */
static bool g_expanded;

/* getLatestTodoPhasesFromEntries over the active branch, else the session's todo state (owned) */
static json_t *current_phases(app_t *a) {
    json_t *path = a->session ? session_path_to(a->session, a->session->leaf_id) : NULL, *found = NULL;
    for (size_t i = json_len(path); i-- > 0 && !found;) {
        const json_t *e = json_at(path, i);
        const char *type = json_get_str(e, "type");
        if (str_eq(type, "custom") && str_eq(json_get_str(e, "customType"), "user_todo_edit")) {
            const json_t *ph = json_get(json_get(e, "data"), "phases");
            if (json_is_arr(ph)) found = json_clone(ph);
        } else if (str_eq(type, "message")) {
            const json_t *m = json_get(e, "message");
            if (!str_eq(json_get_str(m, "role"), "toolResult") || !str_eq(json_get_str(m, "toolName"), "todo") ||
                json_get_bool(m, "isError", false))
                continue;
            const json_t *d = json_get(m, "details");
            if (str_eq(json_get_str(d, "op"), "view")) continue;
            if (json_is_arr(json_get(d, "phases"))) found = json_clone(json_get(d, "phases"));
        }
    }
    json_free(path);
    if (found && json_len(found)) return found;
    json_free(found);
    pthread_mutex_lock(&a->env->mu);
    json_t *r = json_clone(a->env->todo.phases);
    pthread_mutex_unlock(&a->env->mu);
    return r ? r : json_arr();
}

static size_t task_count(const json_t *phases) {
    size_t n = 0;
    json_t *p;
    json_arr_foreach(phases, i, p) n += json_len(json_get(p, "tasks"));
    return n;
}

static char *lower(const char *s) {
    char *r = xstrdup(s ? s : "");
    for (char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}

/* findPhaseFuzzy: exact (case-insensitive), then a unique prefix, then a unique substring */
static const json_t *find_phase(const json_t *phases, const char *query) {
    char *q = str_trim(query), *ql = lower(q);
    free(q);
    const json_t *p, *hit = NULL;
    if (!*ql) {
        free(ql);
        return NULL;
    }
    json_arr_foreach(phases, i, p) {
        char *n = lower(json_get_str(p, "name"));
        bool eq = !strcmp(n, ql);
        free(n);
        if (eq) {
            free(ql);
            return p;
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        size_t count = 0;
        hit = NULL;
        json_arr_foreach(phases, i, p) {
            char *n = lower(json_get_str(p, "name"));
            if (pass == 0 ? str_starts(n, ql) : strstr(n, ql) != NULL) count++, hit = p;
            free(n);
        }
        if (count == 1) {
            free(ql);
            return hit;
        }
    }
    free(ql);
    return NULL;
}

/* findTaskFuzzy: exact content, a unique substring, else the unique open one among the substring hits */
static const json_t *find_task(const json_t *phases, const char *query) {
    char *q = str_trim(query), *ql = lower(q);
    free(q);
    if (!*ql) {
        free(ql);
        return NULL;
    }
    const json_t *p, *t, *exact = NULL, *one = NULL, *active = NULL;
    size_t matches = 0, nactive = 0;
    json_arr_foreach(phases, i, p) json_arr_foreach(json_get(p, "tasks"), k, t) {
        char *c = lower(json_get_str(t, "content"));
        if (!exact && !strcmp(c, ql)) exact = t;
        if (strstr(c, ql)) {
            matches++, one = t;
            const char *st = json_get_str(t, "status");
            if (str_eq(st, "in_progress") || str_eq(st, "pending")) nactive++, active = t;
        }
        free(c);
    }
    free(ql);
    if (exact) return exact;
    if (matches == 1) return one;
    if (nactive == 1) return active;
    return NULL;
}

static char *reminder(const char *action, const json_t *phases, bool removed) {
    sb_t b = SB_INIT;
    char *md = json_len(phases) ? todo_phases_to_markdown(phases) : xstrdup("(empty)");
    size_t n = strlen(md);
    while (n && (md[n - 1] == '\n' || md[n - 1] == ' ')) md[--n] = 0;
    sb_appendf(&b, "<system-reminder>\nThe user manually modified the todo list (%s).\n", action);
    if (removed)
        sb_append(&b, json_len(phases)
                          ? "The user intentionally removed the entries no longer shown below. Do NOT re-add them unless the user explicitly asks.\n"
                          : "The user intentionally cleared the todo list. Do NOT recreate or re-populate it unless the user explicitly asks; "
                            "continue the current request without a todo list.\n");
    sb_appendf(&b, "Current todo list:\n\n%s\n</system-reminder>", md);
    free(md);
    return sb_detach(&b);
}

/* #commit */
static void commit(ui_t *u, json_t *next, const char *action, bool removed) {
    app_t *a = u->app;
    pthread_mutex_lock(&a->env->mu);
    json_free(a->env->todo.phases);
    a->env->todo.phases = json_clone(next);
    pthread_mutex_unlock(&a->env->mu);
    if (a->session) {
        json_t *data = json_obj();
        json_set(data, "phases", json_clone(next));
        session_append_custom(a->session, "user_todo_edit", data);
    }
    char *text = reminder(action, next, removed);
    json_t *msg = json_obj(), *content = json_arr(), *blk = json_obj();
    json_set_str(msg, "role", "developer");
    json_set_str(blk, "type", "text");
    json_set_str(blk, "text", text);
    json_push(content, blk);
    json_set(msg, "content", content);
    json_set_str(msg, "attribution", "user");
    json_set_int(msg, "timestamp", now_ms());
    if (a->session) session_append_message(a->session, msg);
    pthread_mutex_lock(&a->agent->mu);
    agent_append_message(a->agent, msg);
    pthread_mutex_unlock(&a->agent->mu);
    free(text);
    json_free(next);
}

static void status(ui_t *u, const char *m) { add_notice(u, "info", m); }

static void statusf(ui_t *u, const char *level, const char *fmt, ...) PRINTF_LIKE(3, 4);
static void statusf(ui_t *u, const char *level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *m = NULL;
    if (vasprintf(&m, fmt, ap) < 0) m = NULL;
    va_end(ap);
    if (m) add_notice(u, level, m);
    free(m);
}

/* tokenize: whitespace-separated, double quotes group, backslash escapes */
static void tokenize(const char *in, strvec_t *out) {
    sb_t cur = SB_INIT;
    bool quote = false;
    for (const char *p = in; *p; p++) {
        if (*p == '\\' && p[1]) {
            sb_putc(&cur, *++p);
            continue;
        }
        if (*p == '"') {
            quote = !quote;
            continue;
        }
        if (!quote && isspace((unsigned char)*p)) {
            if (cur.len) strvec_push(out, sb_cstr(&cur)), sb_clear(&cur);
            continue;
        }
        sb_putc(&cur, *p);
    }
    if (cur.len) strvec_push(out, sb_cstr(&cur));
    sb_free(&cur);
}

static char *title_case(const char *s) {
    strvec_t w = {0};
    tokenize(s, &w); /* split on whitespace (quotes already resolved by the caller's tokens) */
    sb_t b = SB_INIT;
    for (size_t i = 0; i < w.n; i++) {
        if (i) sb_putc(&b, ' ');
        w.v[i][0] = (char)toupper((unsigned char)w.v[i][0]);
        sb_append(&b, w.v[i]);
    }
    strvec_free(&w);
    return sb_detach(&b);
}

static void append_cmd(ui_t *u, const char *rest) {
    strvec_t tok = {0};
    tokenize(rest, &tok);
    if (!tok.n) {
        add_notice(u, "error", "Usage: /todo append [<phase>] <task...>");
        return;
    }
    json_t *next = current_phases(u->app), *target = NULL;
    const char *content_src;
    char *joined = NULL;
    if (tok.n == 1) {
        content_src = tok.v[0];
        if (json_len(next)) target = json_at(next, json_len(next) - 1);
        else {
            target = json_obj();
            json_set_str(target, "name", "Todos");
            json_set(target, "tasks", json_arr());
            json_push(next, target);
        }
    } else {
        strvec_t rest_tok = {0};
        for (size_t i = 1; i < tok.n; i++) strvec_push(&rest_tok, tok.v[i]);
        joined = strvec_join(&rest_tok, " ");
        strvec_free(&rest_tok);
        content_src = joined;
        target = (json_t *)find_phase(next, tok.v[0]);
        if (!target) {
            char *name = title_case(tok.v[0]);
            target = json_obj();
            json_set_str(target, "name", name);
            json_set(target, "tasks", json_arr());
            json_push(next, target);
            free(name);
        }
    }
    char *content = str_trim(content_src);
    if (*content) content[0] = (char)toupper((unsigned char)content[0]);
    json_t *t = json_obj();
    json_set_str(t, "content", content);
    json_set_str(t, "status", "pending");
    json_push(json_get(target, "tasks"), t);
    char *pname = xstrdup(json_get_str(target, "name"));
    char *action = xasprintf("/todo append \xe2\x86\x92 %s", pname);
    commit(u, next, action, false);
    statusf(u, "info", "Appended to %s: %s", pname, content);
    free(action), free(pname), free(content), free(joined);
    strvec_free(&tok);
}

static void op_cmd(ui_t *u, const char *op, const char *task, const char *phase, const char *action, bool removed, const char *ok) {
    json_t *cur = current_phases(u->app), *args = json_obj();
    if (task) json_set_str(args, "task", task);
    if (phase) json_set_str(args, "phase", phase);
    strvec_t errs = {0};
    json_t *next = todo_apply_op(cur, op, args, &errs);
    if (errs.n) {
        char *e = strvec_join(&errs, "; ");
        add_notice(u, "error", e);
        free(e);
        json_free(next);
    } else {
        commit(u, next, action, removed);
        status(u, ok);
    }
    strvec_free(&errs);
    json_free(args);
    json_free(cur);
}

static void start_cmd(ui_t *u, const char *rest) {
    if (!*rest) {
        add_notice(u, "error", "Usage: /todo start <task>");
        return;
    }
    json_t *cur = current_phases(u->app);
    const json_t *hit = find_task(cur, rest);
    if (!hit) statusf(u, "error", "No task matched \"%s\". Use /todo to list current tasks.", rest);
    else {
        const char *c = json_get_str(hit, "content");
        char *action = xasprintf("/todo start %s", c), *ok = xasprintf("Started: %s", c);
        op_cmd(u, "start", c, NULL, action, false, ok);
        free(action), free(ok);
    }
    json_free(cur);
}

/* done / drop / rm */
static void mutate_cmd(ui_t *u, const char *verb, const char *rest) {
    bool rm = !strcmp(verb, "rm");
    const char *target = rm ? NULL : !strcmp(verb, "done") ? "completed" : "abandoned";
    json_t *cur = current_phases(u->app);
    char *trimmed = str_trim(rest);
    if (!*trimmed) {
        if (rm) {
            commit(u, json_arr(), "/todo rm (all)", true);
            status(u, "Cleared all todos.");
        } else {
            char *action = xasprintf("/todo %s (all)", verb), *ok = xasprintf("Marked all tasks %s.", target);
            op_cmd(u, verb, NULL, NULL, action, false, ok);
            free(action), free(ok);
        }
    } else {
        const json_t *task = find_task(cur, trimmed), *phase = task ? NULL : find_phase(cur, trimmed);
        if (task || phase) {
            const char *name = task ? json_get_str(task, "content") : json_get_str(phase, "name");
            char *action = xasprintf("/todo %s %s", verb, name);
            char *ok = rm ? (task ? xasprintf("Removed: %s", name) : xasprintf("Removed phase: %s", name))
                          : (task ? xasprintf("Marked %s: %s", target, name) : xasprintf("Marked phase %s %s.", name, target));
            op_cmd(u, verb, task ? name : NULL, task ? NULL : name, action, rm, ok);
            free(action), free(ok);
        } else {
            statusf(u, "error", "No task or phase matched \"%s\".", trimmed);
        }
    }
    free(trimmed);
    json_free(cur);
}

static char *todo_path(ui_t *u, const char *rest) {
    char *raw = str_trim(rest);
    /* normalizePathLikeInput: surrounding quotes and a leading @ */
    size_t n = strlen(raw);
    if (n >= 2 && (raw[0] == '"' || raw[0] == '\'') && raw[n - 1] == raw[0]) {
        memmove(raw, raw + 1, n - 2);
        raw[n - 2] = 0;
    }
    const char *p = raw[0] == '@' ? raw + 1 : raw;
    if (!*p) p = "TODO.md";
    char *expanded = p[0] == '~' && (p[1] == '/' || !p[1]) ? xasprintf("%s%s", home_dir(), p + 1) : xstrdup(p);
    char *abs = path_abs(expanded, u->app->cwd);
    free(expanded), free(raw);
    return abs;
}

static void edit_cmd(ui_t *u) {
    const char *ed = getenv("VISUAL");
    if (!ed || !*ed) ed = getenv("EDITOR");
    if (!ed || !*ed) {
        add_notice(u, "warning", "No editor configured. Set $VISUAL or $EDITOR environment variable.");
        return;
    }
    json_t *cur = current_phases(u->app);
    char *initial = json_len(cur) ? todo_phases_to_markdown(cur) : xstrdup("# Todos\n- [ ] (replace this with your tasks)\n");
    json_free(cur);
    bool no_editor = false;
    char *result = ui_external_edit(u, initial, ".todo.md", &no_editor);
    free(initial);
    if (!result) {
        add_notice(u, "warning", "Editor exited without saving; todos unchanged.");
        return;
    }
    strvec_t errs = {0};
    json_t *parsed = todo_markdown_to_phases(result, &errs);
    free(result);
    if (errs.n) {
        char *e = strvec_join(&errs, "\n  ");
        statusf(u, "error", "Could not parse Markdown:\n  %s", e);
        free(e);
        json_free(parsed);
    } else {
        size_t phases = json_len(parsed), tasks = task_count(parsed);
        commit(u, parsed, "/todo edit", false);
        statusf(u, "info", "Todos updated from editor: %zu phase(s), %zu task(s).", phases, tasks);
    }
    strvec_free(&errs);
}

void ui_todo_command(ui_t *u, const char *arg) {
    char *trimmed = str_trim(arg);
    if (!*trimmed) {
        json_t *cur = current_phases(u->app);
        if (!json_len(cur)) status(u, "No todos. Use /todo append <task> to start one.");
        else {
            char *md = todo_phases_to_markdown(cur);
            size_t n = strlen(md);
            while (n && (md[n - 1] == '\n' || md[n - 1] == ' ')) md[--n] = 0;
            status(u, md);
            free(md);
        }
        json_free(cur);
        free(trimmed);
        return;
    }
    size_t vl = strcspn(trimmed, " \t\n");
    char *v0 = xstrndup(trimmed, vl), *verb = lower(v0);
    free(v0);
    char *rest = str_trim(trimmed + vl);
    if (!strcmp(verb, "expand")) g_expanded = true;
    else if (!strcmp(verb, "collapse")) g_expanded = false;
    else if (!strcmp(verb, "edit")) edit_cmd(u);
    else if (!strcmp(verb, "copy")) {
        json_t *cur = current_phases(u->app);
        if (!json_len(cur)) add_notice(u, "warning", "No todos to copy.");
        else {
            char *md = todo_phases_to_markdown(cur);
            tui_term_copy_osc52(tui_term(u->tui), md, strlen(md));
            free(md);
            status(u, "Copied todos as Markdown to clipboard.");
        }
        json_free(cur);
    } else if (!strcmp(verb, "export")) {
        json_t *cur = current_phases(u->app);
        if (!json_len(cur)) add_notice(u, "warning", "No todos to export.");
        else {
            char *target = todo_path(u, rest), *md = todo_phases_to_markdown(cur);
            int rc = fs_write(target, md, strlen(md));
            if (rc) statusf(u, "error", "Failed to write todos: %s", strerror(-rc));
            else statusf(u, "info", "Wrote todos to %s", target);
            free(target), free(md);
        }
        json_free(cur);
    } else if (!strcmp(verb, "import")) {
        char *source = todo_path(u, rest);
        size_t n = 0;
        char *content = fs_read(source, &n);
        if (!content) {
            statusf(u, "error", "Failed to read todos: ENOENT: no such file or directory, open '%s'", source);
        } else {
            strvec_t errs = {0};
            json_t *phases = todo_markdown_to_phases(content, &errs);
            if (errs.n) {
                char *e = strvec_join(&errs, "\n  ");
                statusf(u, "error", "Could not parse %s:\n  %s", source, e);
                free(e);
                json_free(phases);
            } else {
                size_t np = json_len(phases), nt = task_count(phases);
                char *action = xasprintf("/todo import %s", source);
                commit(u, phases, action, false);
                free(action);
                statusf(u, "info", "Imported %zu phase(s), %zu task(s) from %s.", np, nt, source);
            }
            strvec_free(&errs);
            free(content);
        }
        free(source);
    } else if (!strcmp(verb, "help") || !strcmp(verb, "?")) status(u, USAGE);
    else if (!strcmp(verb, "append")) append_cmd(u, rest);
    else if (!strcmp(verb, "start")) start_cmd(u, rest);
    else if (!strcmp(verb, "done") || !strcmp(verb, "drop") || !strcmp(verb, "rm")) mutate_cmd(u, verb, rest);
    else statusf(u, "error", "Unknown /todo verb \"%s\".\n%s", verb, USAGE);
    free(verb), free(rest), free(trimmed);
}
