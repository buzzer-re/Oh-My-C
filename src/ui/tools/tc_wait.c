/* tc_wait.c - wait card (port of omp tools/wait.ts jobsRenderResult): pending row, compact settled-job log
 * while collapsed, job tree with output previews when expanded, interrupted/empty forms. Live job widgets
 * (job-widget.ts charts), shimmer, agent roster rows and IRC message cards are not ported. */
#include "ui/tools/tc.h"

#include "tui/ansi.h"

static tc_status_t status_icon(const char *st) {
    return str_eq(st, "completed") ? TC_ST_DONE : str_eq(st, "failed") ? TC_ST_ERROR : str_eq(st, "cancelled") ? TC_ST_ABORTED : TC_ST_RUNNING;
}

static tui_tc_t status_color(const char *st) {
    return str_eq(st, "completed") ? TUI_TC_SUCCESS : str_eq(st, "failed") ? TUI_TC_ERROR : str_eq(st, "cancelled") ? TUI_TC_WARNING
                                                                                                                 : TUI_TC_ACCENT;
}

static int status_order(const char *st) {
    return str_eq(st, "running") ? 0 : str_eq(st, "failed") ? 1 : str_eq(st, "cancelled") ? 2 : 3;
}

static int cmp_job(const void *a, const void *b) {
    const json_t *x = *(const json_t *const *)a, *y = *(const json_t *const *)b;
    int d = status_order(json_get_str(x, "status")) - status_order(json_get_str(y, "status"));
    if (d) return d;
    double dx = json_get_num(x, "durationMs", 0), dy = json_get_num(y, "durationMs", 0);
    return dx < dy ? 1 : dx > dy ? -1 : 0;
}

static char *one_line(const char *s) {
    char *t = tc_replace_tabs(s ? s : "");
    for (char *p = t; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') *p = ' ';
    char *f = str_trim(t);
    free(t);
    /* collapse runs */
    sb_t b = SB_INIT;
    bool sp = false;
    for (char *p = f; *p; p++) {
        if (*p == ' ') {
            if (!sp) sb_putc(&b, ' ');
            sp = true;
        } else sb_putc(&b, *p), sp = false;
    }
    free(f);
    return sb_detach(&b);
}

static char *first_line_of(const char *s) {
    const char *p = s ? s : "";
    size_t n = strcspn(p, "\r\n");
    char *l = xstrndup(p, n);
    char *t = tc_replace_tabs(l);
    free(l);
    char *r = str_trim(t);
    free(t);
    return r;
}

/* stripTaskResultEnvelope: the <output>/<preview> body of a <task-result> */
static char *strip_envelope(const char *text) {
    if (!str_starts(text, "<task-result")) return xstrdup(text);
    static const char *const TAGS[] = {"output", "preview"};
    for (size_t k = 0; k < 2; k++) {
        char open[16], close[16];
        snprintf(open, sizeof open, "<%s", TAGS[k]);
        snprintf(close, sizeof close, "</%s>", TAGS[k]);
        const char *o = strstr(text, open);
        if (!o) continue;
        const char *gt = strchr(o, '>');
        const char *c = gt ? strstr(gt, close) : NULL;
        if (!c) continue;
        const char *s = gt + 1;
        if (*s == '\n') s++;
        size_t n = (size_t)(c - s);
        if (n && s[n - 1] == '\n') n--;
        char *body = xstrndup(s, n);
        char *t = str_trim(body);
        free(body);
        if (*t) return t;
        free(t);
    }
    return xstrdup(text);
}

static char *job_label(const json_t *j) {
    const char *st = json_get_str(j, "stage");
    if (str_eq(json_get_str(j, "type"), "task") && str_eq(json_get_str(j, "status"), "running") && st) {
        char *t = str_trim(st);
        if (*t) return t;
        free(t);
    }
    return xstrdup(json_get_str(j, "label") ? json_get_str(j, "label") : "");
}

void tc_wait(const tc_call_t *c, int width, strvec_t *out) {
    if (!c->has_result) {
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "Wait", .title_slot = -1};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        return;
    }
    const json_t *d = c->details;
    if (json_get_bool(d, "interrupted", false) && !c->is_error) {
        const char *m[] = {"interrupted by message"};
        tc_status_line_t sl = {.icon = TC_ST_INFO, .spinner = -1, .title = "Wait", .title_slot = -1, .meta = m, .nmeta = 1};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        return;
    }
    const json_t *jobs = json_get_arr(d, "jobs"), *agents = json_get_arr(d, "agents");
    size_t nj = json_len(jobs), na = json_len(agents);
    if (!nj && !na) {
        char *t = tc_text_of(c->content);
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "Wait", .title_slot = -1};
        char *h = tc_status_line(&sl);
        char *e = tc_empty_message(*t ? t : "No jobs to process");
        char *all = xasprintf("%s\n%s", h, e);
        tc_push_wrapped(out, all, width);
        free(all);
        free(e);
        free(h);
        free(t);
        return;
    }
    size_t done = 0, failed = 0, cancelled = 0, running = 0;
    for (size_t i = 0; i < nj; i++) {
        const char *s = json_get_str(json_at(jobs, i), "status");
        if (str_eq(s, "completed")) done++;
        else if (str_eq(s, "failed")) failed++;
        else if (str_eq(s, "cancelled")) cancelled++;
        else running++;
    }
    strvec_t meta = {0};
    char buf[64];
    if (done) snprintf(buf, sizeof buf, "%zu done", done), strvec_push_own(&meta, tc_fg(TUI_TC_SUCCESS, buf));
    if (failed) snprintf(buf, sizeof buf, "%zu failed", failed), strvec_push_own(&meta, tc_fg(TUI_TC_ERROR, buf));
    if (cancelled) snprintf(buf, sizeof buf, "%zu cancelled", cancelled), strvec_push_own(&meta, tc_fg(TUI_TC_WARNING, buf));
    if (na && nj) snprintf(buf, sizeof buf, "%zu agent%s", na, na == 1 ? "" : "s"), strvec_push_own(&meta, tc_fg(TUI_TC_ACCENT, buf));
    const char *noun = nj == 1 ? "job" : "jobs";
    char *desc = !nj ? xasprintf("%zu running agent%s \xe2\x80\x94 no jobs", na, na == 1 ? "" : "s")
                 : running ? (running == nj ? xasprintf("waiting on %zu %s", nj, noun) : xasprintf("waiting on %zu of %zu %s", running, nj, noun))
                           : xasprintf("%zu %s settled", nj, noun);
    tc_status_line_t sl = {.icon = failed ? TC_ST_WARNING : (running || na) ? TC_ST_INFO : TC_ST_SUCCESS, .spinner = -1,
                           .title = desc, .title_slot = -1, .meta = (const char *const *)meta.v, .nmeta = meta.n};
    char *header = tc_status_line(&sl);
    const json_t **sorted = xcalloc(nj + 1, sizeof *sorted);
    for (size_t i = 0; i < nj; i++) sorted[i] = json_at(jobs, i);
    qsort(sorted, nj, sizeof *sorted, cmp_job);
    strvec_t all = {0};
    const char *dot = tc_sym("sep.dot");
    bool compact = !c->expanded && nj > 0;
    if (compact) {
        size_t nsettled = 0;
        for (size_t i = 0; i < nj; i++)
            if (!str_eq(json_get_str(sorted[i], "status"), "running")) nsettled++;
        if (!nsettled) strvec_push(&all, header);
        char *dic = tc_status_icon(TC_ST_DONE, -1);
        int indent_w = tui_visible_width(dic) + 1;
        free(dic);
        size_t shown = 0;
        for (size_t i = 0; i < nj && shown < 8; i++) {
            const json_t *j = sorted[i];
            const char *st = json_get_str(j, "status");
            if (str_eq(st, "running")) continue;
            shown++;
            char *icon = tc_status_icon(status_icon(st), -1);
            const char *word = str_eq(st, "completed") ? "done" : st;
            sb_t oc = SB_INIT;
            char *w = tc_fg(status_color(st), word);
            sb_appendf(&oc, " %s", w);
            free(w);
            if (json_get(j, "exitCode") && json_get_int(j, "exitCode", 0) != 0) {
                char e[32];
                snprintf(e, sizeof e, "exit %lld", (long long)json_get_int(j, "exitCode", 0));
                char *ec = tc_fg(TUI_TC_ERROR, e);
                sb_appendf(&oc, "%s%s", dot, ec);
                free(ec);
            }
            char *du = tc_format_duration(json_get_int(j, "durationMs", 0));
            char *dd = tc_fg(TUI_TC_DIM, du);
            sb_appendf(&oc, "%s%s", dot, dd);
            char *pre = xasprintf("%s ", icon);
            char *idf = one_line(json_get_str(j, "id"));
            char *id = tui_truncate_to_width(idf, MAX(1, width - tui_visible_width(pre) - tui_visible_width(sb_cstr(&oc))), NULL);
            char *ida = tc_fg(TUI_TC_ACCENT, id);
            sb_t line = SB_INIT;
            sb_appendf(&line, "%s%s%s", pre, ida, sb_cstr(&oc));
            char *lab0 = job_label(j);
            char *lab = first_line_of(lab0);
            int budget = width - tui_visible_width(sb_cstr(&line)) - 2;
            if (*lab && !str_eq(lab, json_get_str(j, "id")) && budget >= 8) {
                char *lt = tui_truncate_to_width(lab, budget, NULL);
                char *lm = tc_fg(TUI_TC_MUTED, lt);
                sb_appendf(&line, "  %s", lm);
                free(lm);
                free(lt);
            }
            strvec_push_own(&all, sb_detach(&line));
            if (str_eq(st, "failed")) {
                const char *et = json_get_str(j, "errorText");
                const char *p = et ? et : "";
                char *reason = NULL;
                while (*p && !reason) {
                    size_t n = strcspn(p, "\r\n");
                    char *l = xstrndup(p, n);
                    char *t = str_trim(l);
                    free(l);
                    if (*t) reason = t;
                    else free(t);
                    p += n;
                    while (*p == '\r' || *p == '\n') p++;
                }
                if (reason) {
                    char *rt = tc_replace_tabs(reason);
                    char *rr = tui_truncate_to_width(rt, MAX(1, width - indent_w), NULL);
                    char *re = tc_fg(TUI_TC_ERROR, rr);
                    strvec_push_own(&all, xasprintf("%*s%s", indent_w, "", re));
                    free(re);
                    free(rr);
                    free(rt);
                    free(reason);
                }
            }
            free(lab);
            free(lab0);
            free(ida);
            free(id);
            free(idf);
            free(pre);
            free(dd);
            free(du);
            sb_free(&oc);
            free(icon);
        }
        if (nsettled > 8) {
            char t[64];
            snprintf(t, sizeof t, "\xe2\x80\xa6 %zu more settled", nsettled - 8);
            char *dm = tc_fg(TUI_TC_DIM, t);
            strvec_push_own(&all, xasprintf("%*s%s", indent_w, "", dm));
            free(dm);
        }
        if (running && nsettled) {
            char t[64];
            snprintf(t, sizeof t, "%zu still running", running);
            char *dm = tc_fg(TUI_TC_DIM, t);
            strvec_push_own(&all, xasprintf("%*s%s", indent_w, "", dm));
            free(dm);
        }
    } else {
        strvec_push(&all, header);
        strvec_t items = {0};
        int row_w = MAX(0, width - 3);
        for (size_t i = 0; i < nj; i++) {
            const json_t *j = sorted[i];
            const char *st = json_get_str(j, "status");
            char *ic = tc_status_icon(status_icon(st), -1);
            sb_t icon = SB_INIT;
            sb_append(&icon, ic);
            free(ic);
            if (json_get(j, "exitCode")) {
                int64_t ex = json_get_int(j, "exitCode", 0);
                char e[32];
                snprintf(e, sizeof e, "exit %lld", (long long)ex);
                char *ec = tc_fg(ex == 0 ? TUI_TC_MUTED : TUI_TC_ERROR, e);
                sb_appendf(&icon, "%s%s", dot, ec);
                free(ec);
            }
            char *tb0 = xasprintf("%s%s%s", tc_sym("format.bracketLeft"), json_get_str(j, "type") ? json_get_str(j, "type") : "job",
                                  tc_sym("format.bracketRight"));
            char *tb = tc_fg(status_color(st), tb0);
            char *du = tc_format_duration(json_get_int(j, "durationMs", 0));
            char *dd = tc_fg(TUI_TC_DIM, du);
            char *suffix = xasprintf("%s%s", dot, dd);
            char *probe = xasprintf("%s %s %s", sb_cstr(&icon), tb, suffix);
            char *idf = one_line(json_get_str(j, "id"));
            char *id = tui_truncate_to_width(idf, MAX(0, row_w - tui_visible_width(probe)), NULL);
            char *lab0 = job_label(j);
            char *lab1 = first_line_of(*lab0 ? lab0 : "(no label)");
            char *lab = tui_truncate_to_width(lab1, 60, NULL);
            char *ho = tc_fg(TUI_TC_TOOL_OUTPUT, id);
            sb_t row = SB_INIT;
            sb_appendf(&row, "%s %s %s", sb_cstr(&icon), tb, ho);
            bool distinct = !str_eq(lab0, json_get_str(j, "id"));
            char *lo = tc_fg(TUI_TC_TOOL_OUTPUT, lab);
            char *probe2 = xasprintf("%s %s%s", sb_cstr(&row), lab, suffix);
            bool inl = distinct && tui_visible_width(probe2) <= row_w;
            if (inl) sb_appendf(&row, " %s", lo);
            sb_append(&row, suffix);
            sb_t item = SB_INIT;
            char *rt = tui_truncate_to_width(sb_cstr(&row), row_w, "");
            sb_append(&item, rt);
            free(rt);
            if (distinct && !inl) sb_appendf(&item, "\n  %s", lo);
            /* output preview: first lines of the result/error */
            const char *src = json_get_str(j, "errorText") && *json_get_str(j, "errorText") ? json_get_str(j, "errorText")
                                                                                              : json_get_str(j, "resultText");
            if (src && *src) {
                char *tr = str_trim(src);
                char *body = strip_envelope(tr);
                strvec_t pl = {0};
                tc_split_lines(body, &pl);
                int cw = MIN(80, MAX(0, row_w - 2));
                size_t lim = MIN(pl.n, (size_t)4);
                tui_tc_t tone = json_get_str(j, "errorText") ? TUI_TC_ERROR : TUI_TC_DIM;
                for (size_t k = 0; k < lim; k++) {
                    char *t1 = tc_replace_tabs(pl.v[k]);
                    char *t2 = tui_truncate_to_width(t1, cw, NULL);
                    char *t3 = tc_fg(tone, t2);
                    sb_appendf(&item, "\n  %s", t3);
                    free(t3);
                    free(t2);
                    free(t1);
                }
                strvec_free(&pl);
                free(body);
                free(tr);
            }
            strvec_push_own(&items, sb_detach(&item));
            sb_free(&row);
            free(probe2);
            free(lo);
            free(ho);
            free(lab);
            free(lab1);
            free(lab0);
            free(id);
            free(idf);
            free(probe);
            free(suffix);
            free(dd);
            free(du);
            free(tb);
            free(tb0);
            sb_free(&icon);
        }
        tc_tree_list(&items, c->expanded, 8, "job", &all);
        strvec_free(&items);
    }
    for (size_t i = 0; i < all.n; i++) strvec_push_own(out, tui_truncate_to_width(all.v[i], width, NULL));
    strvec_free(&all);
    free(sorted);
    free(header);
    free(desc);
    strvec_free(&meta);
}
