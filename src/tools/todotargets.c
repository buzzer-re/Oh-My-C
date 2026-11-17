/*
 * todotargets.c - todoTaskTargets (packages/tui/src/tools/todo.ts): which task contents a todo `task` argument targets.
 * Shared by the todo tool (tools/todo_ask.c) and its card (ui/tools/tc_misc.c).
 */
#include "tools.h"
#include <ctype.h>

/* splitTaskList (tui todo.ts TASK_LIST_DELIMITERS): a `task` string naming no task verbatim splits like a path list;
 * a split wins when every part names a task, a newline / `;` / `,` split also when only some do */
static bool task_known(const strvec_t *known, const char *c) { return strvec_contains(known, c); }

static char *task_part(const char *s, size_t n) {
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n >= 2 && (s[0] == '"' || s[0] == '\'' || s[0] == '`') && s[n - 1] == s[0]) {
        s++, n -= 2;
        while (n && isspace((unsigned char)*s)) s++, n--;
        while (n && isspace((unsigned char)s[n - 1])) n--;
    }
    return xstrndup(s, n);
}

/* split on mode 0 `\r?\n`, 1 `;`, 2 `,`, 3 whitespace runs, 4 `[\s,;]+`; empty parts dropped */
static void task_split(const char *c, int mode, strvec_t *out) {
    size_t st = 0, i = 0, n = strlen(c);
    for (; i <= n; i++) {
        bool d = i == n;
        size_t skip = 1;
        if (!d) switch (mode) {
            case 0: d = c[i] == '\n'; break;
            case 1: d = c[i] == ';'; break;
            case 2: d = c[i] == ','; break;
            case 3: d = isspace((unsigned char)c[i]); break;
            default: d = isspace((unsigned char)c[i]) || c[i] == ',' || c[i] == ';'; break;
            }
        if (!d) continue;
        size_t e = i;
        if (mode == 0 && i < n && e > st && c[e - 1] == '\r') e--;
        char *p = task_part(c + st, e - st);
        if (*p) strvec_push_own(out, p);
        else free(p);
        st = i + skip;
    }
}

static void split_task_list(const char *c, const strvec_t *known, strvec_t *out) {
    if (task_known(known, c)) {
        strvec_push(out, c);
        return;
    }
    strvec_t partial = {0};
    bool have_partial = false;
    for (int mode = 0; mode < 5; mode++) {
        strvec_t parts = {0};
        task_split(c, mode, &parts);
        size_t named = 0;
        for (size_t i = 0; i < parts.n; i++) named += task_known(known, parts.v[i]);
        if (named > 0 && named == parts.n) {
            for (size_t i = 0; i < parts.n; i++) strvec_push(out, parts.v[i]);
            strvec_free(&parts);
            strvec_free(&partial);
            return;
        }
        if (!have_partial && mode < 3 && named > 0 && parts.n > 1) {
            partial = parts;
            have_partial = true;
        } else {
            strvec_free(&parts);
        }
    }
    if (have_partial) {
        for (size_t i = 0; i < partial.n; i++) strvec_push(out, partial.v[i]);
        strvec_free(&partial);
    } else {
        strvec_push(out, c);
    }
}

/* todoTaskTargets: the task contents a `task` arg (string or string array) targets; absent, "" and [] target none */
void todo_task_targets(const json_t *phases, const json_t *task, strvec_t *out) {
    if (!task || (json_is_str(task) && !*json_sv(task))) return;
    strvec_t known = {0};
    const json_t *p, *t;
    json_arr_foreach(phases, i, p) json_arr_foreach(json_get(p, "tasks"), k, t) if (json_get_str(t, "content"))
        strvec_push(&known, json_get_str(t, "content"));
    if (json_is_str(task)) split_task_list(json_sv(task), &known, out);
    else {
        const json_t *e;
        json_arr_foreach(json_is_arr(task) ? task : NULL, i, e) if (json_is_str(e)) split_task_list(json_sv(e), &known, out);
    }
    strvec_free(&known);
}

