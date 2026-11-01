/* tc_misc.c - todo card, the generic fallback card and the renderer dispatch (port of omp
 * tools/todo.ts, tools/default-renderer.ts and the registry in tools/index.ts). */
#include "ui/tools/tc.h"

#include <ctype.h>

#include "tui/ansi.h"

/* ---------- todo ---------- */

static bool todo_closed(const char *st) { return str_eq(st, "completed") || str_eq(st, "abandoned"); }

static const char *ROMAN_SYM[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
static const int ROMAN_VAL[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};

static char *roman(int n) {
    sb_t b = SB_INIT;
    for (int i = 0; i < 13; i++)
        while (n >= ROMAN_VAL[i]) {
            sb_append(&b, ROMAN_SYM[i]);
            n -= ROMAN_VAL[i];
        }
    return sb_detach(&b);
}

static char *todo_line(const json_t *task) {
    const char *st = json_get_str(task, "status");
    char *text = tc_replace_tabs(json_get_str(task, "content") ? json_get_str(task, "content") : "");
    const char *checked = tc_sym("checkbox.checked"), *unchecked = tc_sym("checkbox.unchecked");
    char *line;
    char *r;
    if (str_eq(st, "completed")) {
        line = xasprintf("%s \x1b[9m%s\x1b[29m", checked, text);
        r = tc_fg(TUI_TC_SUCCESS, line);
    } else if (str_eq(st, "in_progress")) {
        line = xasprintf("%s %s", unchecked, text);
        r = tc_fg(TUI_TC_ACCENT, line);
    } else if (str_eq(st, "abandoned")) {
        line = xasprintf("%s \x1b[9m%s\x1b[29m", unchecked, text);
        r = tc_fg(TUI_TC_ERROR, line);
    } else if (str_eq(st, "blocked")) {
        const char *bl = json_get_str(task, "blocker");
        char *note = bl ? xasprintf("blocked: %s", bl) : xstrdup("blocked");
        line = xasprintf("%s %s (%s)", unchecked, text, note);
        r = tc_fg(TUI_TC_WARNING, line);
        free(note);
    } else {
        line = xasprintf("%s %s", unchecked, text);
        r = tc_fg(TUI_TC_DIM, line);
    }
    free(line);
    free(text);
    return r;
}

/* selectCollapsedTodos with cap 8 (no active-description matching) */
typedef VEC(const json_t *) jvec_t;

static void collapsed_select(const json_t *tasks, jvec_t *items, char **summary) {
    size_t n = json_len(tasks);
    *summary = NULL;
    jvec_t open = {0};
    const json_t *last_closed = NULL;
    for (size_t i = 0; i < n; i++) {
        const json_t *t = json_at(tasks, i);
        if (todo_closed(json_get_str(t, "status"))) last_closed = t;
        else vec_push(&open, t);
    }
    bool all_closed = open.n == 0;
    jvec_t base = {0};
    if (all_closed)
        for (size_t i = 0; i < n; i++) vec_push(&base, json_at(tasks, i));
    else {
        if (last_closed) vec_push(items, last_closed);
        for (size_t i = 0; i < open.n; i++) vec_push(&base, open.v[i]);
    }
    const size_t cap = 8;
    if (base.n <= cap + 1) {
        for (size_t i = 0; i < base.n; i++) vec_push(items, base.v[i]);
    } else {
        jvec_t active = {0};
        for (size_t i = 0; i < base.n; i++)
            if (str_eq(json_get_str(base.v[i], "status"), "in_progress")) vec_push(&active, base.v[i]);
        if (active.n > cap) {
            for (size_t i = 0; i < cap; i++) vec_push(items, active.v[i]);
            size_t h = active.n - cap;
            *summary = xasprintf("\xe2\x80\xa6 %zu more active todo%s", h, h == 1 ? "" : "s");
        } else {
            size_t first = 0;
            if (active.n)
                for (size_t i = 0; i < base.n; i++)
                    if (base.v[i] == active.v[0]) {
                        first = i;
                        break;
                    }
            size_t added = 0;
            for (size_t i = 0; i < active.n; i++) vec_push(items, active.v[i]);
            for (size_t i = first; i < base.n && active.n + added < cap; i++) {
                if (str_eq(json_get_str(base.v[i], "status"), "in_progress")) continue;
                vec_push(items, base.v[i]);
                added++;
            }
            size_t hidden = base.n - active.n - added;
            if (hidden > 0) *summary = tc_more_items((int64_t)hidden, "todo");
        }
        vec_free(&active);
    }
    vec_free(&base);
    vec_free(&open);
}

void tc_todo(const tc_call_t *c, int width, strvec_t *out) {
    if (!c->has_result) {
        /* pending: `⏳ Todo op task phase N items` */
        strvec_t meta = {0};
        const json_t *a = c->args;
        const json_t *ops = json_get_arr(a, "ops");
        size_t nops = ops ? json_len(ops) : (json_get_str(a, "op") ? 1 : 0);
        for (size_t i = 0; i < nops; i++) {
            const json_t *e = ops ? json_at(ops, i) : a;
            sb_t b = SB_INIT;
            sb_append(&b, json_get_str(e, "op") ? json_get_str(e, "op") : "update");
            if (json_get_str(e, "task")) sb_appendf(&b, " %s", json_get_str(e, "task"));
            if (json_get_str(e, "phase")) sb_appendf(&b, " %s", json_get_str(e, "phase"));
            const json_t *items = json_get_arr(e, "items");
            if (items && json_len(items)) sb_appendf(&b, " %zu item%s", json_len(items), json_len(items) == 1 ? "" : "s");
            strvec_push_own(&meta, sb_detach(&b));
        }
        if (!meta.n) strvec_push(&meta, "update");
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = c->spinner, .title = "Todo", .title_slot = -1,
                               .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        strvec_free(&meta);
        return;
    }
    if (c->is_error) {
        tc_status_line_t sl = {.icon = TC_ST_ERROR, .spinner = -1, .title = "Todo", .title_slot = -1};
        char *h = tc_status_line(&sl);
        char *et = tc_text_of(c->content);
        char *ed = tc_error_detail(*et ? et : "Todo operation failed");
        tc_section_t sec = {0};
        tc_split_lines(ed, &sec.lines);
        tc_block_t blk = {.header = h, .state = TC_STATE_ERROR, .sections = &sec, .nsections = 1, .width = width,
                          .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_ERROR};
        tc_output_block(&blk, out);
        strvec_free(&sec.lines);
        free(ed);
        free(et);
        free(h);
        return;
    }
    const json_t *phases = json_get_arr(c->details, "phases");
    jvec_t ph = {0};
    size_t total = 0;
    json_t *p;
    json_arr_foreach(phases, i, p) {
        size_t n = json_len(json_get_arr(p, "tasks"));
        if (n) {
            vec_push(&ph, p);
            total += n;
        }
    }
    char *ov = tc_styled_sym("tool.todo", TUI_TC_ACCENT);
    char *cnt = xasprintf("%zu tasks", total);
    const char *m[] = {cnt};
    tc_status_line_t sl = {.icon_override = ov, .spinner = -1, .title = "Todo", .title_slot = -1, .meta = m, .nmeta = 1};
    char *header = tc_status_line(&sl);
    if (total == 0) {
        char *t = tc_text_of(c->content);
        char *fb = tc_fg(TUI_TC_DIM, *t ? t : "No todos");
        tc_push_wrapped(out, header, width);
        char *l = xasprintf("  %s", fb);
        tc_push_wrapped(out, l, width);
        free(l);
        free(fb);
        free(t);
    } else {
        int cw = tc_block_content_width(width, -1, -1);
        bool multi = ph.n > 1;
        strvec_t body = {0};
        /* touched phases (collapsed multi-phase view): active, just completed, or named by the op */
        for (size_t pi = 0; pi < ph.n; pi++) {
            const json_t *phase = ph.v[pi];
            const json_t *tasks = json_get_arr(phase, "tasks");
            size_t done = 0;
            bool active = false;
            for (size_t k = 0; k < json_len(tasks); k++) {
                const char *st = json_get_str(json_at(tasks, k), "status");
                if (todo_closed(st)) done++;
                if (str_eq(st, "in_progress")) active = true;
            }
            const char *op = json_get_str(c->args, "op");
            bool touched = !multi || c->expanded || active || str_eq(op, "init") ||
                           str_eq(json_get_str(c->args, "phase"), json_get_str(phase, "name"));
            char *rn = roman((int)pi + 1);
            char *pname = xasprintf("%s. %s", rn, json_get_str(phase, "name") ? json_get_str(phase, "name") : "");
            char *prog = xasprintf("  %zu/%zu", done, json_len(tasks));
            char *progd = tc_fg(TUI_TC_DIM, prog);
            char *nb = tc_bold(pname);
            if (!touched) {
                char *nd = tc_fg(TUI_TC_DIM, nb);
                strvec_push_own(&body, xasprintf("%s%s", nd, progd));
                free(nd);
            } else {
                if (multi) {
                    char *na = tc_fg(TUI_TC_ACCENT, nb);
                    strvec_push_own(&body, xasprintf("%s%s", na, progd));
                    free(na);
                }
                strvec_t items = {0};
                char *summary = NULL;
                if (c->expanded) {
                    for (size_t k = 0; k < json_len(tasks); k++) strvec_push_own(&items, todo_line(json_at(tasks, k)));
                } else {
                    jvec_t sel = {0};
                    collapsed_select(tasks, &sel, &summary);
                    for (size_t k = 0; k < sel.n; k++) strvec_push_own(&items, todo_line(sel.v[k]));
                    vec_free(&sel);
                }
                strvec_t tl = {0};
                if (summary) {
                    /* caller-driven collapse: items + muted summary row */
                    for (size_t k = 0; k < items.n; k++) {
                        char *conn = tc_fg(TUI_TC_DIM, tc_sym("tree.branch"));
                        strvec_push_own(&tl, xasprintf("%s %s", conn, items.v[k]));
                        free(conn);
                    }
                    char *conn = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
                    char *sm = tc_fg(TUI_TC_MUTED, summary);
                    strvec_push_own(&tl, xasprintf("%s %s", conn, sm));
                    free(sm);
                    free(conn);
                } else {
                    tc_tree_list(&items, true, 0, "todo", &tl);
                }
                for (size_t k = 0; k < tl.n; k++) {
                    char *l = xasprintf("%s%s", multi ? "  " : "", tl.v[k]);
                    tc_push_wrapped(&body, l, cw);
                    free(l);
                }
                strvec_free(&tl);
                strvec_free(&items);
                free(summary);
            }
            free(nb);
            free(progd);
            free(prog);
            free(pname);
            free(rn);
        }
        tc_section_t sec = {.lines = body};
        tc_block_t blk = {.header = header, .state = c->partial ? TC_STATE_PENDING : TC_STATE_SUCCESS, .sections = &sec,
                          .nsections = body.n ? 1 : 0, .width = width, .no_bg = true, .pad_left = -1, .pad_right = -1,
                          .border_slot = TUI_TC_BORDER_MUTED};
        tc_output_block(&blk, out);
        strvec_free(&body);
    }
    free(header);
    free(cnt);
    free(ov);
    vec_free(&ph);
}

/* ---------- generic fallback (default-renderer.ts formatDefaultToolExecution) ---------- */

void tc_generic(const tc_call_t *c, int width, strvec_t *out) {
    int cw = MAX(1, width - 2);
    tc_status_t icon = c->partial ? (c->spinner >= 0 ? TC_ST_RUNNING : TC_ST_PENDING) : c->is_error ? TC_ST_ERROR : TC_ST_DONE;
    tc_status_line_t sl = {.icon = icon, .spinner = c->spinner, .title = c->label ? c->label : c->name, .title_slot = -1};
    strvec_t body = {0};
    strvec_push_own(&body, tc_status_line(&sl));
    bool has_args = json_is_obj(c->args) && json_len(c->args) > 0;
    if (!c->expanded && has_args) {
        int budget = MAX(20, cw - tui_visible_width(tc_sym("tree.last")) - 2);
        char *pv = tc_json_inline(c->args, budget);
        if (*pv) {
            char *a = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
            char *b = tc_fg(TUI_TC_DIM, pv);
            strvec_push_own(&body, xasprintf(" %s %s", a, b));
            free(b);
            free(a);
        }
        free(pv);
    }
    if (c->expanded && c->args) {
        strvec_push(&body, "");
        strvec_push_own(&body, tc_fg(TUI_TC_DIM, "Args"));
        bool tr = false;
        tc_json_tree(c->args, 6, 200, 2000, true, &body, &tr);
        if (tr) strvec_push_own(&body, tc_fg(TUI_TC_DIM, "\xe2\x80\xa6"));
        strvec_push(&body, "");
    }
    if (c->has_result) {
        char *raw = tc_text_of(c->content);
        char *txt = tc_trim_end(raw);
        if (!*txt) {
            strvec_push_own(&body, tc_fg(TUI_TC_DIM, "(no output)"));
        } else {
            bool json_done = false;
            if ((txt[0] == '{' || txt[0] == '[') && strlen(txt) <= 64 * 1024) {
                json_t *j = json_parse_str(txt);
                if (j) {
                    strvec_t tl = {0};
                    bool tr = false;
                    if (c->expanded) tc_json_tree(j, 6, 200, 2000, false, &tl, &tr);
                    else tc_json_tree(j, 2, 6, 60, false, &tl, &tr);
                    if (tl.n && !(c->expanded && tr)) {
                        for (size_t i = 0; i < tl.n; i++) strvec_push(&body, tl.v[i]);
                        if (!c->expanded) strvec_push_own(&body, tc_expand_hint());
                        json_done = true;
                    }
                    strvec_free(&tl);
                    json_free(j);
                }
            }
            if (!json_done) {
                strvec_t lines = {0}, styled = {0};
                tc_split_lines(txt, &lines);
                for (size_t i = 0; i < lines.n; i++) {
                    char *n = tc_replace_tabs(lines.v[i]);
                    char *s = strstr(n, "\x1b[") ? xstrdup(n) : tc_fg(TUI_TC_TOOL_OUTPUT, n);
                    strvec_push_own(&styled, c->expanded ? s : tui_truncate_to_width(s, cw, NULL));
                    if (!c->expanded) free(s);
                    free(n);
                }
                tc_pane_t pane = {.lines = &styled, .expanded = c->expanded, .collapsed_max = 4, .style_plain = true,
                                  .hint_when_uncapped = true};
                tc_output_pane(&pane, &body);
                strvec_free(&styled);
                strvec_free(&lines);
            }
        }
        free(txt);
        free(raw);
    }
    /* WidthAwareText(…, paddingX 1, paddingY 1) with the state tint */
    tui_tc_t bgs = c->partial ? TUI_TC_TOOL_PENDING_BG : c->is_error ? TUI_TC_TOOL_ERROR_BG : TUI_TC_TOOL_SUCCESS_BG;
    const char *bg = tui_bg_seq(NULL, bgs);
    strvec_t wrapped = {0};
    for (size_t i = 0; i < body.n; i++) tui_wrap_text_with_ansi(body.v[i], cw, &wrapped);
    sb_t blank = SB_INIT;
    tui_sb_spaces(&blank, width);
    strvec_t rows = {0};
    strvec_push(&rows, sb_cstr(&blank));
    for (size_t i = 0; i < wrapped.n; i++) {
        sb_t r = SB_INIT;
        sb_putc(&r, ' ');
        sb_append(&r, wrapped.v[i]);
        sb_putc(&r, ' ');
        tui_sb_spaces(&r, MAX(0, width - tui_visible_width(sb_cstr(&r))));
        strvec_push_own(&rows, sb_detach(&r));
    }
    strvec_push(&rows, sb_cstr(&blank));
    for (size_t i = 0; i < rows.n; i++) {
        sb_t r = SB_INIT;
        sb_append(&r, bg);
        for (const char *q = rows.v[i]; *q;) {
            if (str_starts(q, "\x1b[0m") || str_starts(q, "\x1b[49m")) {
                size_t n = q[2] == '0' ? 4 : 5;
                sb_appendn(&r, q, n);
                sb_append(&r, bg);
                q += n;
            } else sb_putc(&r, *q++);
        }
        sb_append(&r, "\x1b[49m");
        strvec_push_own(out, sb_detach(&r));
    }
    strvec_free(&rows);
    sb_free(&blank);
    strvec_free(&wrapped);
    strvec_free(&body);
}

/* ---------- dispatch ---------- */

bool tc_wants_tick(const char *name) {
    /* bash: live elapsed clock; generic cards animate a spinner while running */
    static const char *quiet[] = {"read", "write", "edit", "grep", "glob", "todo", "web_search", "ask", "goal", "find", "ast_grep", "ast_edit"};
    for (size_t i = 0; i < ARRAY_LEN(quiet); i++)
        if (str_eq(name, quiet[i])) return false;
    return true;
}

void tc_render(const tc_call_t *c, int width, strvec_t *out) {
    const char *n = c->name ? c->name : "";
    if (!strcmp(n, "bash")) tc_bash(c, width, out);
    else if (!strcmp(n, "read")) tc_read(c, width, out);
    else if (!strcmp(n, "write")) tc_write(c, width, out);
    else if (!strcmp(n, "edit")) tc_edit(c, width, out);
    else if (!strcmp(n, "grep")) tc_grep(c, width, out);
    else if (!strcmp(n, "glob")) tc_glob(c, width, out);
    else if (!strcmp(n, "todo")) tc_todo(c, width, out);
    else if (!strcmp(n, "web_search")) tc_web_search(c, width, out);
    else if (!strcmp(n, "ask")) tc_ask(c, width, out);
    else if (!strcmp(n, "goal")) tc_goal(c, width, out);
    else if (!strcmp(n, "find")) tc_find(c, width, out);
    else if (!strcmp(n, "ast_grep")) tc_ast_grep(c, width, out);
    else if (!strcmp(n, "ast_edit")) tc_ast_edit(c, width, out);
    else tc_generic(c, width, out);
}
