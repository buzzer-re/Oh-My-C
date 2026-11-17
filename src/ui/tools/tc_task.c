/* tc_task.c - task (subagent) card (port of omp tools/task.ts renderCall/renderResult and
 * tools/agent-tree.ts): streaming call preview, live progress rows (async spawns update the card after
 * the call returns), finalized result rows with the run summary footer. Review/yield structured
 * output and nested task trees are not ported (omc subagents do not produce them). */
#include "ui/tools/tc.h"

#include "tui/ansi.h"
#include "tui/markdown.h"

static char *flatten(const char *s) {
    sb_t b = SB_INIT;
    bool ws = false;
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') ws = true;
        else {
            if (ws && b.len) sb_putc(&b, ' ');
            ws = false;
            sb_putc(&b, *p);
        }
    }
    return sb_detach(&b);
}

/* previewLine: flattened + truncated */
static char *preview_line(const char *s, int max) {
    char *f = flatten(s);
    char *r = tui_truncate_to_width(f, max, NULL);
    free(f);
    return r;
}

static char *first_line(const char *s) {
    char *t = str_trim(s ? s : "");
    char *nl = strchr(t, '\n');
    if (nl) *nl = 0;
    return t;
}

/* formatTaskId: "Anna.Bob" -> "Anna>Bob" */
static char *task_id(const char *id) {
    char *r = xstrdup(id ? id : "");
    for (char *p = r; *p; p++)
        if (*p == '.') *p = '>';
    return r;
}

static char *badge(const char *label, tui_tc_t slot) {
    char *t = xasprintf("%s%s%s", tc_sym("format.bracketLeft"), label, tc_sym("format.bracketRight"));
    char *r = tc_fg(slot, t);
    free(t);
    return r;
}

/* agentTypeBadge: dim ` ⟨agent⟩` for non-default agents */
static char *agent_badge(const char *agent) {
    char *t = agent ? str_trim(agent) : xstrdup("");
    if (!*t || !strcmp(t, "task")) {
        free(t);
        return xstrdup("");
    }
    char *b = badge(t, TUI_TC_DIM);
    char *r = xasprintf(" %s", b);
    free(b);
    free(t);
    return r;
}

/* Markdown(text, 0, 0, theme, muted) at width - 3 */
static void md_section(const char *text, int width, tc_section_t *sec) {
    char *t = str_trim(text ? text : "");
    if (*t) {
        tui_md_opts_t mo;
        tui_md_opts_default(&mo);
        mo.text_slot = TUI_TC_MUTED;
        tui_md_render(t, MAX(1, width - 3), &mo, &sec->lines);
    }
    free(t);
}

static char *trunc_row(const char *s, int width) { return tui_truncate_to_width(s, MAX(0, width), ""); }

/* formatAgentStatRun */
static void stat_run(sb_t *b, const json_t *st) {
    const char *dot = tc_sym("sep.dot");
    int64_t tools = json_get_int(st, "toolCount", 0), req = json_get_int(st, "requests", 0);
    double ctx = json_get_num(st, "contextTokens", 0), win = json_get_num(st, "contextWindow", 0);
    double cost = json_get_num(st, "cost", 0);
    if (!json_get(st, "cost")) cost = json_get_num(json_get_obj(json_get_obj(st, "usage"), "cost"), "total", 0);
    if (tools) {
        char *n = tc_format_number((double)tools);
        char *t = xasprintf("%s %s", n, tc_sym("icon.extensionTool"));
        char *d = tc_fg(TUI_TC_DIM, t);
        sb_appendf(b, "%s%s", dot, d);
        free(d);
        free(t);
        free(n);
    }
    if (req) {
        char *n = tc_format_number((double)req);
        char *t = xasprintf("%s req", n);
        char *d = tc_fg(TUI_TC_DIM, t);
        sb_appendf(b, "%s%s", dot, d);
        free(d);
        free(t);
        free(n);
    }
    if (ctx > 0) {
        char *t;
        if (win > 0) {
            char *w = tc_format_number(win);
            t = xasprintf("%.1f%%/%s", ctx / win * 100.0, w);
            free(w);
        } else t = tc_format_number(ctx);
        char *d = tc_fg(TUI_TC_DIM, t);
        sb_appendf(b, "%s%s", dot, d);
        free(d);
        free(t);
    }
    if (cost > 0) {
        char *t = xasprintf("$%.2f", cost);
        char *d = tc_fg(TUI_TC_STATUS_LINE_COST, t);
        sb_appendf(b, "%s%s", dot, d);
        free(d);
        free(t);
    }
}

static void description_lines(const char *desc, const char *prefix, int width, strvec_t *out) {
    char *bp = tui_truncate_to_width(prefix, MAX(0, width - 1), "");
    strvec_t w = {0};
    tui_wrap_text_with_ansi(desc, MAX(1, width - tui_visible_width(bp)), &w);
    for (size_t i = 0; i < w.n; i++) {
        char *d = tc_fg(TUI_TC_DIM, w.v[i]);
        strvec_push_own(out, xasprintf("%s%s", bp, d));
        free(d);
    }
    strvec_free(&w);
    free(bp);
}

/* renderAgentTreeRow (presentation "task") */
static char *agent_row(const json_t *p, int width, int spinner, bool frozen, bool *desc_shown) {
    const char *status = json_get_str(p, "status") ? json_get_str(p, "status") : "pending";
    bool live = str_eq(status, "pending") || str_eq(status, "running");
    bool failed = str_eq(status, "failed") || str_eq(status, "aborted");
    tui_tc_t name_color = live && frozen ? TUI_TC_DIM : str_eq(status, "completed") ? TUI_TC_TEXT : TUI_TC_ACCENT;
    /* task rows draw a static glyph (their bytes hold across spinner frames); a frozen snapshot of a live
     * row dims its glyph with its name, so it stops reading as in flight */
    char *icon;
    if (live && frozen) icon = tc_styled_sym(str_eq(status, "running") ? "status.running" : "status.pending", TUI_TC_DIM);
    else
        icon = tc_status_icon(str_eq(status, "failed")      ? TC_ST_ERROR
                              : str_eq(status, "completed") ? TC_ST_SUCCESS
                              : str_eq(status, "aborted")   ? TC_ST_ABORTED
                              : str_eq(status, "running")   ? TC_ST_RUNNING
                                                            : TC_ST_PENDING,
                              -1);
    UNUSED(spinner);
    char *lead = xasprintf("%s ", icon);
    char *sbadge = NULL;
    if (json_get(p, "retryState") && str_eq(status, "running")) {
        char *b = badge("retrying", TUI_TC_WARNING);
        sbadge = xasprintf(" %s", b);
        free(b);
    } else if (json_get(p, "retryFailure") && failed) {
        char *b = badge("rate-limited", TUI_TC_ERROR);
        sbadge = xasprintf(" %s", b);
        free(b);
    } else if (failed) {
        char *b = badge(status, TUI_TC_ERROR);
        sbadge = xasprintf(" %s", b);
        free(b);
    } else sbadge = xstrdup("");
    char *id0 = task_id(json_get_str(p, "id"));
    char *id = tui_truncate_to_width(id0, MAX(0, width - tui_visible_width(lead) - tui_visible_width(sbadge)), NULL);
    char *rb0 = agent_badge(json_get_str(p, "agent"));
    char *rb = tui_truncate_to_width(rb0, MAX(0, width - tui_visible_width(lead) - tui_visible_width(id) - tui_visible_width(sbadge)), NULL);
    char *badges = xasprintf("%s%s", rb, sbadge);
    char *desc = NULL;
    if (json_get_str(p, "description")) {
        char *t = tc_replace_tabs(json_get_str(p, "description"));
        desc = str_trim(t);
        free(t);
        if (!*desc) {
            free(desc);
            desc = NULL;
        }
    }
    bool shown = false;
    if (desc) {
        char *probe = xasprintf("%s%s: %s%s", lead, id, desc, badges);
        shown = tui_visible_width(probe) <= width;
        free(probe);
    }
    *desc_shown = shown;
    char *title;
    if (live && shown) {
        char *bi = tc_bold(id);
        char *a = tc_fg(name_color, bi), *c = tc_fg(name_color, ":"), *d = tc_fg(name_color, desc);
        title = xasprintf("%s%s %s", a, c, d);
        free(a);
        free(c);
        free(d);
        free(bi);
    } else if (shown) {
        char *bi = tc_bold(id);
        char *t = xasprintf("%s: %s", bi, desc);
        title = tc_fg(name_color, t);
        free(t);
        free(bi);
    } else title = tc_fg(name_color, id);
    sb_t line = SB_INIT;
    sb_appendf(&line, "%s%s%s", lead, title, badges);
    if (str_eq(status, "running") && !desc) {
        const char *src = json_get_str(p, "assignment") ? json_get_str(p, "assignment") : json_get_str(p, "task");
        char *pv = preview_line(src ? src : "", 40);
        char *m = tc_fg(TUI_TC_MUTED, pv);
        sb_appendf(&line, " %s", m);
        free(m);
        free(pv);
    }
    if (live && json_get(p, "completionPercent")) {
        char *t = xasprintf("%lld%%", (long long)json_get_int(p, "completionPercent", 0));
        char *a = tc_fg(TUI_TC_ACCENT, t);
        sb_appendf(&line, "%s%s", tc_sym("sep.dot"), a);
        free(a);
        free(t);
    }
    if (str_eq(status, "running") || str_eq(status, "completed")) stat_run(&line, p);
    char *r = trunc_row(sb_cstr(&line), width);
    sb_free(&line);
    free(title);
    free(desc);
    free(badges);
    free(rb);
    free(rb0);
    free(id);
    free(id0);
    free(sbadge);
    free(lead);
    free(icon);
    return r;
}

static void task_section(const char *task, const char *cont, bool expanded, strvec_t *out) {
    if (!expanded) return;
    char *t = str_trim(task ? task : "");
    if (*t) {
        char *l = tc_fg(TUI_TC_DIM, "Task");
        strvec_push_own(out, xasprintf("%s%s", cont, l));
        free(l);
        strvec_t lines = {0};
        tc_split_lines(t, &lines);
        for (size_t i = 0; i < lines.n && i < 20; i++) {
            char *r = tc_replace_tabs(lines.v[i]);
            char *d = tc_fg(TUI_TC_DIM, r);
            strvec_push_own(out, xasprintf("%s  %s", cont, d));
            free(d);
            free(r);
        }
        if (lines.n > 20) {
            char *m = tc_more_items((int64_t)(lines.n - 20), "line");
            char *d = tc_fg(TUI_TC_DIM, m);
            strvec_push_own(out, xasprintf("%s  %s", cont, d));
            free(d);
            free(m);
        }
        strvec_free(&lines);
    }
    free(t);
}

static void agent_progress(const json_t *p, bool expanded, int width, int spinner, bool frozen, int64_t now_ms, strvec_t *out) {
    const char *cont = "  ";
    bool shown;
    strvec_push_own(out, agent_row(p, width, spinner, frozen, &shown));
    const char *status = json_get_str(p, "status");
    if (json_get_str(p, "description") && !shown) {
        char *t = tc_replace_tabs(json_get_str(p, "description"));
        char *d = str_trim(t);
        if (*d) description_lines(d, cont, width, out);
        free(d);
        free(t);
    }
    if (json_get_str(p, "resolvedModelRoute")) {
        char *t = xasprintf("routed: %s", json_get_str(p, "resolvedModelRoute"));
        char *d = tc_fg(TUI_TC_DIM, t);
        char *l = xasprintf("%s%s", cont, d);
        strvec_push_own(out, trunc_row(l, width));
        free(l);
        free(d);
        free(t);
    }
    task_section(json_get_str(p, "assignment") ? json_get_str(p, "assignment") : json_get_str(p, "task"), cont, expanded, out);
    if (str_eq(status, "running")) {
        if (json_get_str(p, "stage")) {
            char *m = tc_fg(TUI_TC_MUTED, json_get_str(p, "stage"));
            char *l = xasprintf("%s%s", cont, m);
            strvec_push_own(out, trunc_row(l, width));
            free(l);
            free(m);
        }
        const char *cur = json_get_str(p, "currentTool");
        const json_t *recent = json_at(json_get_arr(p, "recentTools"), 0);
        if (cur || recent) {
            const char *name = cur ? cur : json_get_str(recent, "tool");
            const char *intent = cur ? json_get_str(p, "currentToolIntent") : json_get_str(recent, "intent");
            const char *args = cur ? json_get_str(p, "currentToolArgs") : json_get_str(recent, "args");
            char *nm = tc_fg(cur ? TUI_TC_MUTED : TUI_TC_DIM, name ? name : "");
            sb_t l = SB_INIT;
            sb_appendf(&l, "%s%s %s", cont, tc_sym("tree.hook"), nm);
            const char *detail = intent && *intent ? intent : args;
            if (detail && *detail) {
                char *pv = preview_line(detail, 40);
                char *d = tc_fg(TUI_TC_DIM, pv);
                sb_appendf(&l, ": %s", d);
                free(d);
                free(pv);
            }
            int64_t st = json_get_int(p, "currentToolStartMs", 0);
            if (cur && st && now_ms - st > 5000) {
                char *du = tc_format_duration(now_ms - st);
                char *w = tc_fg(TUI_TC_WARNING, du);
                sb_appendf(&l, "%s%s", tc_sym("sep.dot"), w);
                free(w);
                free(du);
            }
            strvec_push_own(out, sb_detach(&l));
            free(nm);
        }
    }
    const json_t *rf = json_get_obj(p, "retryFailure");
    if (rf && !str_eq(status, "running")) {
        int64_t at = json_get_int(rf, "attempt", 0);
        char *pv = preview_line(json_get_str(rf, "errorMessage"), 80);
        char *t = xasprintf("auto-retry gave up after %lld attempt%s: %s", (long long)at, at == 1 ? "" : "s", pv);
        char *e = tc_fg(TUI_TC_ERROR, t);
        strvec_push_own(out, xasprintf("%s%s %s", cont, tc_sym("tree.hook"), e));
        free(e);
        free(t);
        free(pv);
    }
}

/* renderOutputSection (no warning / structured forms) */
static void output_section(const char *output, const char *cont, bool expanded, int max_c, int max_e, const char *warning,
                           strvec_t *out) {
    char *trim = tc_trim_end(output ? output : "");
    if (!*trim && !warning) {
        free(trim);
        return;
    }
    char *ol = tc_fg(TUI_TC_DIM, "Output");
    strvec_push_own(out, xasprintf("%s%s", cont, ol));
    free(ol);
    const char *ind = "  ";
    if (warning) {
        char *ic = tc_status_icon(TC_ST_WARNING, -1);
        char *w = expanded ? xstrdup(warning) : tui_truncate_to_width(warning, 80, NULL);
        char *wd = tc_fg(TUI_TC_DIM, w);
        strvec_push_own(out, xasprintf("%s  %s %s", cont, ic, wd));
        free(wd);
        free(w);
        free(ic);
    }
    if (*trim) {
        strvec_t lines = {0};
        tc_split_lines(trim, &lines);
        size_t pc = (size_t)(expanded ? max_e : max_c);
        for (size_t i = 0; i < lines.n && i < pc; i++) {
            char *r = tc_replace_tabs(lines.v[i]);
            char *c = expanded ? xstrdup(r) : tui_truncate_to_width(r, 70, NULL);
            char *d = tc_fg(TUI_TC_DIM, c);
            strvec_push_own(out, xasprintf("%s%s%s", cont, ind, d));
            free(d);
            free(c);
            free(r);
        }
        if (lines.n > pc) {
            char *m = tc_more_items((int64_t)(lines.n - pc), "line");
            char *d = tc_fg(TUI_TC_DIM, m);
            strvec_push_own(out, xasprintf("%s%s%s", cont, ind, d));
            free(d);
            free(m);
        }
        strvec_free(&lines);
    }
    free(trim);
}

static bool result_problem(const json_t *r) {
    return json_get_bool(r, "aborted", false) || json_get_int(r, "exitCode", 0) != 0 || json_get_str(r, "error");
}

static void agent_result(const json_t *r, bool expanded, int width, strvec_t *out) {
    const char *cont = "  ";
    const char *output = json_get_str(r, "output") ? json_get_str(r, "output") : "";
    const char *warn_prefix = "SYSTEM WARNING: Subagent exited without calling yield tool";
    char *warning = NULL;
    const char *rest = output;
    {
        char *fl = first_line(output);
        if (str_starts(fl, warn_prefix)) {
            warning = fl;
            const char *nl = strchr(output, '\n');
            rest = nl ? nl + 1 : "";
            while (*rest == '\n' || *rest == ' ' || *rest == '\t') rest++;
        } else free(fl);
    }
    bool aborted = json_get_bool(r, "aborted", false);
    int64_t exit = json_get_int(r, "exitCode", 0);
    const char *err = json_get_str(r, "error");
    bool merge_failed = !aborted && exit == 0 && err;
    bool success = !aborted && exit == 0 && !err;
    bool needs_warning = warning && success;
    tui_tc_t color = needs_warning ? TUI_TC_WARNING : success ? TUI_TC_SUCCESS : merge_failed ? TUI_TC_WARNING : TUI_TC_ERROR;
    /* a merge failure is a warning: the agent's work succeeded, only folding it back did not */
    char *icon = tc_status_icon(aborted ? TC_ST_ABORTED : needs_warning || merge_failed ? TC_ST_WARNING : success ? TC_ST_SUCCESS : TC_ST_ERROR, -1);
    const char *st = aborted ? "aborted" : needs_warning ? "warning" : success ? "done" : merge_failed ? "merge failed" : "failed";
    char *b0 = badge(st, color);
    char *sbadge = xasprintf(" %s", b0);
    free(b0);
    char *probe = xasprintf("%s %s", icon, sbadge);
    char *id0 = task_id(json_get_str(r, "id"));
    char *id = tui_truncate_to_width(id0, MAX(0, width - tui_visible_width(probe)), NULL);
    char *probe2 = xasprintf("%s %s%s", icon, id, sbadge);
    char *rb0 = agent_badge(json_get_str(r, "agent"));
    char *rb = tui_truncate_to_width(rb0, MAX(0, width - tui_visible_width(probe2)), NULL);
    char *badges = xasprintf("%s%s", rb, sbadge);
    char *desc = NULL;
    if (json_get_str(r, "description")) {
        char *t = tc_replace_tabs(json_get_str(r, "description"));
        desc = str_trim(t);
        free(t);
        if (!*desc) {
            free(desc);
            desc = NULL;
        }
    }
    bool shown = false;
    if (desc) {
        char *p = xasprintf("%s %s: %s%s", icon, id, desc, badges);
        shown = tui_visible_width(p) <= width;
        free(p);
    }
    char *tp;
    if (shown) {
        char *bi = tc_bold(id);
        tp = xasprintf("%s: %s", bi, desc);
        free(bi);
    } else tp = xstrdup(id);
    char *ic = tc_fg(color, icon);
    char *tt = tc_fg(success && !needs_warning ? TUI_TC_TEXT : TUI_TC_ACCENT, tp);
    sb_t line = SB_INIT;
    sb_appendf(&line, "%s %s%s", ic, tt, badges);
    stat_run(&line, r);
    char *du = tc_format_duration(json_get_int(r, "durationMs", 0));
    char *dd = tc_fg(TUI_TC_DIM, du);
    sb_appendf(&line, "%s%s", tc_sym("sep.dot"), dd);
    if (json_get_bool(r, "truncated", false)) {
        char *w = tc_fg(TUI_TC_WARNING, "[truncated]");
        sb_appendf(&line, " %s", w);
        free(w);
    }
    strvec_push_own(out, trunc_row(sb_cstr(&line), width));
    if (desc && !shown) description_lines(desc, cont, width, out);
    task_section(json_get_str(r, "assignment") ? json_get_str(r, "assignment") : json_get_str(r, "task"), cont, expanded, out);
    const char *ar = json_get_str(r, "abortReason");
    if (aborted && ar) {
        char *e = tc_status_icon(TC_ST_ABORTED, -1);
        char *f = flatten(ar);
        char *c = expanded ? xstrdup(f) : tui_truncate_to_width(f, 80, NULL);
        char *d = tc_fg(TUI_TC_DIM, c);
        strvec_push_own(out, xasprintf("%s%s %s", cont, e, d));
        free(d);
        free(c);
        free(f);
        free(e);
    }
    output_section(rest, cont, expanded, 3, 12, warning, out);
    if (err && (!success || merge_failed) && (!aborted || !str_eq(err, ar))) {
        char *f = flatten(err);
        char *c = expanded ? xstrdup(f) : tui_truncate_to_width(f, 70, NULL);
        char *e = tc_fg(merge_failed ? TUI_TC_WARNING : TUI_TC_ERROR, c);
        strvec_push_own(out, xasprintf("%s%s", cont, e));
        free(e);
        free(c);
        free(f);
    }
    sb_free(&line);
    free(dd);
    free(du);
    free(tt);
    free(ic);
    free(tp);
    free(desc);
    free(badges);
    free(rb);
    free(rb0);
    free(probe2);
    free(id);
    free(id0);
    free(probe);
    free(sbadge);
    free(icon);
    free(warning);
}

static int cmp_dur(const void *a, const void *b) {
    const json_t *x = *(const json_t *const *)a, *y = *(const json_t *const *)b;
    double dx = json_get_num(x, "durationMs", 0), dy = json_get_num(y, "durationMs", 0);
    if (dx != dy) return dx < dy ? -1 : 1;
    double ix = json_get_num(x, "index", 0), iy = json_get_num(y, "index", 0);
    return ix < iy ? -1 : ix > iy;
}

static bool is_unfinished(const json_t *p) {
    const char *s = json_get_str(p, "status");
    return !s || str_eq(s, "pending") || str_eq(s, "running");
}

/* orderProgressForDisplay: finished by runtime, then unfinished in dispatch order */
static size_t order_progress(const json_t *arr, const json_t **outv) {
    size_t n = 0, nf = 0;
    json_t *p;
    json_arr_foreach(arr, i, p) if (!is_unfinished(p)) outv[n++] = p;
    nf = n;
    xqsort(outv, nf, sizeof *outv, cmp_dur);
    json_arr_foreach(arr, i, p) if (is_unfinished(p)) outv[n++] = p;
    return n;
}

static char *hidden_progress_line(const json_t **v, size_t n) {
    size_t done = 0, running = 0, pending = 0, failed = 0, aborted = 0;
    for (size_t i = 0; i < n; i++) {
        const char *s = json_get_str(v[i], "status");
        if (str_eq(s, "completed")) done++;
        else if (str_eq(s, "running")) running++;
        else if (str_eq(s, "failed")) failed++;
        else if (str_eq(s, "aborted")) aborted++;
        else pending++;
    }
    strvec_t parts = {0};
    char buf[64];
    if (done) snprintf(buf, sizeof buf, "%zu done", done), strvec_push_own(&parts, tc_fg(TUI_TC_DIM, buf));
    if (running) snprintf(buf, sizeof buf, "%zu running", running), strvec_push_own(&parts, tc_fg(TUI_TC_DIM, buf));
    if (pending) snprintf(buf, sizeof buf, "%zu pending", pending), strvec_push_own(&parts, tc_fg(TUI_TC_DIM, buf));
    if (failed) snprintf(buf, sizeof buf, "%zu failed", failed), strvec_push_own(&parts, tc_fg(TUI_TC_ERROR, buf));
    if (aborted) snprintf(buf, sizeof buf, "%zu aborted", aborted), strvec_push_own(&parts, tc_fg(TUI_TC_ERROR, buf));
    char *sep = tc_fg(TUI_TC_DIM, tc_sym("sep.dot"));
    char *j = strvec_join(&parts, sep);
    char *op = tc_fg(TUI_TC_DIM, " ("), *cp = tc_fg(TUI_TC_DIM, ")");
    char *m = tc_more_items((int64_t)n, "agent");
    char *md = tc_fg(TUI_TC_DIM, m);
    char *hint = tc_expand_hint();
    char *r = parts.n ? xasprintf("%s%s%s%s %s", md, op, j, cp, hint) : xasprintf("%s %s", md, hint);
    free(hint);
    free(md);
    free(m);
    free(op);
    free(cp);
    free(j);
    free(sep);
    strvec_free(&parts);
    return r;
}

static void task_call(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *a = c->args;
    char *ov = tc_styled_sym("tool.task", TUI_TC_ACCENT);
    char *agent = json_get_str(a, "agent") ? str_trim(json_get_str(a, "agent")) : NULL;
    tc_status_line_t sl = {.icon_override = ov, .spinner = -1, .title = "Task", .title_slot = -1,
                           .description = agent && *agent ? agent : NULL};
    char *h = tc_status_line(&sl);
    tc_section_t secs[3] = {0};
    size_t ns = 0;
    md_section(json_get_str(a, "context"), width, &secs[ns]);
    if (secs[ns].lines.n) ns++;
    md_section(json_get_str(a, "task"), width, &secs[ns]);
    if (secs[ns].lines.n) ns++;
    /* renderTaskCallLines */
    strvec_t *cl = &secs[ns].lines;
    char *bullet = tc_fg(TUI_TC_DIM, "\xe2\x80\xa2");
    {
        char *nm = json_get_str(a, "name") ? str_trim(json_get_str(a, "name")) : xstrdup("");
        char *id = *nm ? task_id(nm) : xstrdup("");
        char *brief = first_line(json_get_str(a, "task"));
        if (*id || *brief) {
            char *bi = tc_bold(*id ? id : "agent");
            char *ia = tc_fg(TUI_TC_ACCENT, bi);
            sb_t l = SB_INIT;
            sb_appendf(&l, "%s %s", bullet, ia);
            if (*brief) {
                char *pv = preview_line(brief, 64);
                char *m = tc_fg(TUI_TC_MUTED, pv);
                sb_appendf(&l, ": %s", m);
                free(m);
                free(pv);
            }
            char *ab = agent_badge(json_get_str(a, "agent"));
            sb_append(&l, ab);
            free(ab);
            strvec_push_own(cl, sb_detach(&l));
            free(ia);
            free(bi);
        }
        free(brief);
        free(id);
        free(nm);
    }
    const json_t *tasks = json_get_arr(a, "tasks");
    size_t nt = json_len(tasks), cap = MIN(nt, (size_t)4);
    for (size_t i = 0; i < cap; i++) {
        const json_t *it = json_at(tasks, i);
        char *nm = json_get_str(it, "name") ? str_trim(json_get_str(it, "name")) : xstrdup("");
        char *id = *nm ? task_id(nm) : xasprintf("#%zu", i + 1);
        char *bi = tc_bold(id);
        char *ia = tc_fg(TUI_TC_ACCENT, bi);
        sb_t l = SB_INIT;
        sb_appendf(&l, "%s %s", bullet, ia);
        char *brief = first_line(json_get_str(it, "task"));
        if (*brief) {
            char *pv = preview_line(brief, 64);
            char *m = tc_fg(TUI_TC_MUTED, pv);
            sb_appendf(&l, ": %s", m);
            free(m);
            free(pv);
        }
        char *ab = agent_badge(json_get_str(it, "agent"));
        sb_append(&l, ab);
        if (json_get_bool(it, "isolated", false)) {
            char *d = tc_fg(TUI_TC_DIM, " [isolated]");
            sb_append(&l, d);
            free(d);
        }
        strvec_push_own(cl, sb_detach(&l));
        free(ab);
        free(brief);
        free(ia);
        free(bi);
        free(id);
        free(nm);
    }
    if (cap < nt) {
        char *m = tc_more_items((int64_t)(nt - cap), "agent");
        char *d = tc_fg(TUI_TC_DIM, m);
        strvec_push_own(cl, xasprintf("%s %s", bullet, d));
        free(d);
        free(m);
    }
    free(bullet);
    if (cl->n) secs[ns++].separator = true;
    tc_block_t blk = {.header = h, .header_meta = json_get_bool(a, "isolated", false) ? "isolated" : NULL, .state = TC_STATE_PENDING,
                      .sections = secs, .nsections = ns, .width = width, .pad_left = -1, .pad_right = -1,
                      .border_slot = TUI_TC_BORDER_MUTED};
    tc_output_block(&blk, out);
    for (size_t i = 0; i < 3; i++) strvec_free(&secs[i].lines);
    free(h);
    free(agent);
    free(ov);
}

void tc_task(const tc_call_t *c, int width, strvec_t *out) {
    if (!c->has_result) {
        task_call(c, width, out);
        return;
    }
    const json_t *d = c->details, *a = c->args;
    int cw = tc_block_content_width(width, -1, -1);
    char *agent = json_get_str(a, "agent") ? str_trim(json_get_str(a, "agent")) : NULL;
    if (agent && !*agent) {
        free(agent);
        agent = NULL;
    }
    char *text = tc_text_of(c->content);
    tc_section_t secs[3] = {0};
    size_t ns = 0;
    md_section(json_get_str(a, "context"), width, &secs[ns]);
    if (secs[ns].lines.n) ns++;
    md_section(json_get_str(a, "task"), width, &secs[ns]);
    if (secs[ns].lines.n) ns++;
    if (!d) {
        tc_status_line_t sl = {.icon = c->is_error ? TC_ST_ERROR : TC_ST_SUCCESS, .spinner = -1, .title = "Task", .title_slot = -1,
                               .description = agent};
        char *h = tc_status_line(&sl);
        if (*text) {
            char *t = tui_truncate_to_width(text, width, NULL);
            strvec_push_own(&secs[ns].lines, tc_fg(TUI_TC_DIM, t));
            free(t);
            secs[ns++].separator = true;
        }
        tc_block_t blk = {.header = h, .state = c->is_error ? TC_STATE_ERROR : TC_STATE_SUCCESS, .sections = secs, .nsections = ns,
                          .width = width, .pad_left = -1, .pad_right = -1,
                          .border_slot = c->is_error ? TUI_TC_ERROR : TUI_TC_BORDER_MUTED};
        tc_output_block(&blk, out);
        for (size_t i = 0; i < 3; i++) strvec_free(&secs[i].lines);
        free(h);
        free(text);
        free(agent);
        return;
    }
    const json_t *results = json_get_arr(d, "results"), *progress = json_get_arr(d, "progress");
    size_t nres = json_len(results), nprog = json_len(progress);
    int64_t aborted = 0, fail = 0, merge = 0, ok = 0, req = 0;
    json_t *r;
    json_arr_foreach(results, i, r) {
        req += json_get_int(r, "requests", 0);
        if (json_get_bool(r, "aborted", false)) aborted++;
        else if (json_get_int(r, "exitCode", 0) != 0) fail++;
        else if (json_get_str(r, "error")) merge++;
        else ok++;
    }
    bool is_error = aborted || fail;
    size_t count = nres ? nres : nprog;
    char *cnt = count ? xasprintf("%zu %s", count, count == 1 ? "agent" : "agents") : NULL;
    const char *meta_label = cnt ? cnt : agent;
    tc_status_t st = c->partial ? TC_ST_RUNNING : is_error ? TC_ST_ERROR : merge ? TC_ST_WARNING : TC_ST_SUCCESS;
    /* while agents are in flight the header shows the dispatch glyph, not a spinner */
    char *ov = st == TC_ST_RUNNING ? tc_styled_sym("tool.task", TUI_TC_ACCENT) : NULL;
    const char *m[] = {meta_label};
    tc_status_line_t sl = {.icon = st, .icon_override = ov, .spinner = -1, .title = "Task", .title_slot = -1,
                           .meta = m, .nmeta = meta_label ? 1 : 0};
    char *header = tc_status_line(&sl);
    strvec_t lines = {0};
    int64_t now = c->now_ms > 0 ? c->now_ms : now_ms();
    const json_t **ord = xcalloc(MAX(nprog, nres) + 1, sizeof *ord);
    if (nprog && !nres) {
        size_t n = order_progress(progress, ord);
        size_t start = c->expanded || n <= 4 ? 0 : n - 4;
        if (start) strvec_push_own(&lines, hidden_progress_line(ord, start));
        for (size_t i = start; i < n; i++) agent_progress(ord[i], c->expanded, cw, c->spinner, c->frozen, now, &lines);
    } else if (nres) {
        for (size_t i = 0; i < nres; i++) ord[i] = json_at(results, i);
        xqsort(ord, nres, sizeof *ord, cmp_dur);
        bool *pick = xcalloc(nres, sizeof(bool));
        size_t picked = 0;
        if (c->expanded || nres <= 4) {
            for (size_t i = 0; i < nres; i++) pick[i] = true;
            picked = nres;
        } else {
            for (size_t i = 0; i < nres && picked < 4; i++)
                if (result_problem(ord[i])) pick[i] = true, picked++;
            for (size_t i = 0; i < nres && picked < 4; i++)
                if (!pick[i]) pick[i] = true, picked++;
        }
        for (size_t i = 0; i < nres; i++)
            if (pick[i]) agent_result(ord[i], c->expanded, cw, &lines);
        if (picked < nres) {
            char *mi = tc_more_items((int64_t)(nres - picked), "agent");
            char *md = tc_fg(TUI_TC_DIM, mi);
            char *hint = tc_expand_hint();
            strvec_push_own(&lines, xasprintf("%s %s", md, hint));
            free(hint);
            free(md);
            free(mi);
        }
        free(pick);
        /* async spawns of a mixed call keep their progress rows */
        json_t *p;
        json_arr_foreach(progress, i, p) {
            bool in = false;
            json_arr_foreach(results, k, r) in |= str_eq(json_get_str(r, "id"), json_get_str(p, "id"));
            if (!in) agent_progress(p, c->expanded, cw, c->spinner, c->frozen, now, &lines);
        }
        strvec_t parts = {0};
        char buf[64];
        if (aborted) snprintf(buf, sizeof buf, "%lld aborted", (long long)aborted), strvec_push_own(&parts, tc_fg(TUI_TC_ERROR, buf));
        if (ok) snprintf(buf, sizeof buf, "%lld succeeded", (long long)ok), strvec_push_own(&parts, tc_fg(TUI_TC_SUCCESS, buf));
        if (merge) snprintf(buf, sizeof buf, "%lld merge failed", (long long)merge), strvec_push_own(&parts, tc_fg(TUI_TC_WARNING, buf));
        if (fail) snprintf(buf, sizeof buf, "%lld failed", (long long)fail), strvec_push_own(&parts, tc_fg(TUI_TC_ERROR, buf));
        if (req > 0) {
            char *n = tc_format_number((double)req);
            char *t = xasprintf("%s req", n);
            strvec_push_own(&parts, tc_fg(TUI_TC_DIM, t));
            free(t);
            free(n);
        }
        char *du = tc_format_duration(json_get_int(d, "totalDurationMs", 0));
        strvec_push_own(&parts, tc_fg(TUI_TC_DIM, du));
        free(du);
        char *sep = tc_fg(TUI_TC_DIM, tc_sym("sep.dot"));
        char *j = strvec_join(&parts, sep);
        char *bl = tc_fg(TUI_TC_DIM, tc_sym("format.bracketLeft")), *br = tc_fg(TUI_TC_DIM, tc_sym("format.bracketRight"));
        strvec_push_own(&lines, xasprintf("%s%s%s", bl, j, br));
        free(bl);
        free(br);
        free(j);
        free(sep);
        strvec_free(&parts);
    }
    free(ord);
    tc_state_t state = c->partial ? TC_STATE_PENDING : is_error ? TC_STATE_ERROR : merge ? TC_STATE_WARNING : TC_STATE_SUCCESS;
    int border = is_error ? TUI_TC_ERROR : TUI_TC_BORDER_MUTED;
    if (!lines.n) {
        const char *t = strspn(text, " \t\r\n") != strlen(text) ? text : "No results";
        char *tt = tui_truncate_to_width(t, width, NULL);
        strvec_push_own(&secs[ns].lines, tc_fg(TUI_TC_DIM, tt));
        free(tt);
        secs[ns++].separator = true;
    } else {
        /* trailing notices of the fallback text (patches, system notifications) */
        strvec_t tl = {0};
        tc_split_lines(text, &tl);
        size_t mark = tl.n;
        for (size_t i = 0; i < tl.n && mark == tl.n; i++)
            if (strstr(tl.v[i], "<system-notification>") || str_starts(tl.v[i], "Applied patches:") ||
                str_starts(tl.v[i], "No changes to apply."))
                mark = i;
        for (size_t i = mark; i < tl.n; i++)
            if (strspn(tl.v[i], " \t") != strlen(tl.v[i])) strvec_push_own(&lines, tc_fg(TUI_TC_DIM, tl.v[i]));
        strvec_free(&tl);
        size_t skip = 0;
        while (skip < lines.n) {
            char *p = tui_strip_ansi(lines.v[skip]);
            bool blank = strspn(p, " \t") == strlen(p);
            free(p);
            if (!blank) break;
            skip++;
        }
        for (size_t i = skip; i < lines.n; i++) strvec_push(&secs[ns].lines, lines.v[i]);
        if (secs[ns].lines.n) secs[ns++].separator = true;
    }
    tc_block_t blk = {.header = header, .state = state, .sections = secs, .nsections = ns, .width = width, .pad_left = -1,
                      .pad_right = -1, .border_slot = border};
    tc_output_block(&blk, out);
    for (size_t i = 0; i < 3; i++) strvec_free(&secs[i].lines);
    strvec_free(&lines);
    free(header);
    free(ov);
    free(cnt);
    free(text);
    free(agent);
}
