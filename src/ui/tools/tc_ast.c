/* tc_ast.c - ast_grep and ast_edit cards (port of omp tools/ast-grep.ts, tools/ast-edit.ts). */
#include "ui/tools/tc.h"

#include "tui/ansi.h"

static bool blank(const char *s) { return strspn(s, " \t") == strlen(s); }

static void push_parse_errors(strvec_t *out, const json_t *errs, int64_t total) {
    size_t n = json_len(errs), cap = MIN(n, (size_t)20);
    for (size_t i = 0; i < cap; i++) {
        char *t = xasprintf("  - %s", json_sv(json_at(errs, i)) ? json_sv(json_at(errs, i)) : "");
        strvec_push_own(out, tc_fg(TUI_TC_WARNING, t));
        free(t);
    }
    int64_t full = total > 0 ? total : (int64_t)n;
    if (full > (int64_t)cap) {
        char *t = xasprintf("  \xe2\x80\xa6 %lld more", (long long)(full - (int64_t)cap));
        strvec_push_own(out, tc_fg(TUI_TC_DIM, t));
        free(t);
    }
}

static char *parse_errors_label(const json_t *d) {
    int64_t full = json_get_int(d, "parseErrorsTotal", 0);
    if (full <= 0) full = (int64_t)json_len(json_get_arr(d, "parseErrors"));
    return full > 20 ? xasprintf("20 / %lld parse issues", (long long)full)
                     : xasprintf("%lld parse issue%s", (long long)full, full != 1 ? "s" : "");
}

static bool drop_group(const tc_grouped_t *g, const tc_ggroup_t *gr, const char *a, const char *b) {
    const char *first = g->lines.v[gr->v[0]];
    return str_starts(first, a) || str_starts(first, b);
}

static bool match_unit(const tc_grouped_t *g, size_t i, void *ud) {
    UNUSED(ud);
    return tc_grouped_is_match(g->lines.v[i]);
}

static char *ast_grep_style(tc_grouped_t *g, size_t i, void *ud) {
    UNUSED(ud);
    if (g->rows[i].kind == TC_GROW_FILE) return tc_grouped_file_row(&g->rows[i]);
    char *f = tc_grouped_code_frame(g, i);
    if (f) return f;
    const char *l = g->lines.v[i];
    return tc_fg(str_starts(l, "  meta:") ? TUI_TC_DIM : TUI_TC_TOOL_OUTPUT, l);
}

/* astGrepSubject / astEditSubject: the result's display scope, or the call's requested paths */
static char *scoped(const char *label, const json_t *args, const json_t *d) {
    const char *scope = json_get_str(d, "scopePath");
    if (scope && *scope) return tc_scoped_subject(label, scope);
    char *ps = tc_paths_of(args);
    char *r = tc_scoped_subject(label, ps);
    free(ps);
    return r;
}

void tc_ast_grep(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    const char *pat = json_get_str(c->args, "pat");
    if (!c->has_result) {
        strvec_t meta = {0};
        char *subj = scoped(pat && *pat ? pat : "?", c->args, NULL);
        if (json_get_int(c->args, "skip", 0) > 0) strvec_push_own(&meta, xasprintf("skip:%lld", (long long)json_get_int(c->args, "skip", 0)));
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "AST Grep", .title_slot = -1,
                               .description = subj, .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        free(subj);
        strvec_free(&meta);
        return;
    }
    char *subject = scoped(pat && *pat ? pat : NULL, c->args, d);
    if (c->is_error) {
        char *t = tc_text_of(c->content);
        tc_status_line_t sl = {.icon = TC_ST_ERROR, .spinner = -1, .title = "AST Grep", .title_slot = -1, .description = subject};
        char *h = tc_status_line(&sl);
        char *e = tc_error_detail(*t ? t : "Unknown error");
        char *j = xasprintf("%s\n%s", h, e);
        tc_push_wrapped(out, j, width);
        free(j);
        free(e);
        free(h);
        free(t);
        free(subject);
        return;
    }
    int64_t matches = json_get_int(d, "matchCount", 0), files = json_get_int(d, "fileCount", 0);
    int64_t searched = json_get_int(d, "filesSearched", 0);
    bool limit = json_get_bool(d, "limitReached", false);
    const json_t *perrs = json_get_arr(d, "parseErrors");
    if (matches == 0) {
        strvec_t meta = {0};
        strvec_push_own(&meta, tc_fg(TUI_TC_WARNING, "0 matches"));
        if (searched > 0) strvec_push_own(&meta, xasprintf("searched %lld", (long long)searched));
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "AST Grep", .title_slot = -1, .description = subject,
                               .meta = (const char *const *)meta.v, .nmeta = meta.n};
        strvec_t lines = {0};
        strvec_push_own(&lines, tc_status_line(&sl));
        if (json_len(perrs)) {
            strvec_push_own(&lines, tc_fg(TUI_TC_WARNING, "Query may be mis-scoped; narrow `path` before concluding absence"));
            push_parse_errors(&lines, perrs, json_get_int(d, "parseErrorsTotal", 0));
        }
        char *j = strvec_join(&lines, "\n");
        tc_push_wrapped(out, j, width);
        free(j);
        strvec_free(&lines);
        strvec_free(&meta);
        free(subject);
        return;
    }
    strvec_t meta = {0};
    strvec_push_own(&meta, tc_format_count("match", matches));
    strvec_push_own(&meta, tc_format_count("file", files));
    strvec_push_own(&meta, xasprintf("searched %lld", (long long)searched));
    if (limit) strvec_push_own(&meta, tc_fg(TUI_TC_WARNING, "limit reached"));
    char *ic = limit ? NULL : tc_fg(TUI_TC_ACCENT, tc_sym("icon.search"));
    tc_status_line_t sl = {.icon = TC_ST_WARNING, .icon_override = ic, .spinner = -1, .title = "AST Grep", .title_slot = -1,
                           .description = subject, .meta = (const char *const *)meta.v, .nmeta = meta.n};
    char *header = tc_status_line(&sl);
    const char *dc = json_get_str(d, "displayContent");
    char *text = dc ? xstrdup(dc) : tc_text_of(c->content);
    /* classify the whole output so nested directory headers resolve across blank-line groups */
    tc_grouped_t g;
    tc_grouped_parse(&g, text, json_get_str(d, "searchPath"), json_get_str(c->args, "lang"));
    tc_ggroup_t *kept = xcalloc(g.ngroups + 1, sizeof *kept);
    size_t nkept = 0;
    for (size_t i = 0; i < g.ngroups; i++)
        if (!drop_group(&g, &g.groups[i], "Result limit reached", "Parse issues:")) kept[nkept++] = g.groups[i];
    /* collapsed previews drop the per-match `meta:` capture rows; a match keeps all its other rows */
    tc_ggroup_t *compact = xcalloc(nkept + 1, sizeof *compact);
    for (size_t i = 0; i < nkept; i++) {
        compact[i].v = xcalloc(kept[i].n + 1, sizeof(size_t));
        for (size_t k = 0; k < kept[i].n; k++)
            if (!str_starts(g.lines.v[kept[i].v[k]], "  meta:")) compact[i].v[compact[i].n++] = kept[i].v[k];
    }
    tc_ggroup_t *shown;
    size_t nshown;
    char *summary;
    tc_grouped_fit(&g, c->expanded ? kept : compact, nkept, c->expanded ? -1 : 6, match_unit, NULL, "match", matches, 0, &shown, &nshown,
                   &summary);
    strvec_t rows = {0};
    tc_grouped_tree(&g, shown, nshown, summary, ast_grep_style, NULL, &rows);
    if (limit) strvec_push_own(&rows, tc_fg(TUI_TC_WARNING, "limit reached; narrow path or use skip"));
    if (json_len(perrs)) {
        char *lab = parse_errors_label(d);
        strvec_push_own(&rows, tc_fg(TUI_TC_WARNING, lab));
        free(lab);
    }
    tc_fit_headered(header, &rows, width, out);
    strvec_free(&rows);
    free(summary);
    free(shown);
    for (size_t i = 0; i < nkept; i++) free(compact[i].v);
    free(compact);
    free(kept);
    tc_grouped_free(&g);
    free(text);
    free(header);
    free(ic);
    strvec_free(&meta);
    free(subject);
}

/* ---------- ast_edit ---------- */

/* patternPreview: whitespace runs collapsed */
static char *pattern_preview(const char *pat) {
    if (!pat) return NULL;
    sb_t b = SB_INIT;
    bool ws = false;
    for (const char *p = pat; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') ws = true;
        else {
            if (ws && b.len) sb_putc(&b, ' ');
            ws = false;
            sb_putc(&b, *p);
        }
    }
    if (!b.len) {
        sb_free(&b);
        return NULL;
    }
    return sb_detach(&b);
}

static bool file_unit(const tc_grouped_t *g, size_t i, void *ud) {
    UNUSED(ud);
    return g->rows[i].kind == TC_GROW_FILE;
}

static char *edit_style(tc_grouped_t *g, size_t i) {
    if (g->rows[i].kind == TC_GROW_FILE) return tc_grouped_file_row(&g->rows[i]);
    /* swap the code-frame gutter `│` for a space so it does not nest a second vertical bar inside the frame border */
    const char *line = g->lines.v[i];
    sb_t d = SB_INIT;
    const char *bar = strstr(line, "\xe2\x94\x82");
    if (bar) {
        sb_appendn(&d, line, (size_t)(bar - line));
        sb_putc(&d, ' ');
        sb_append(&d, bar + 3);
    } else sb_append(&d, line);
    char *disp = tc_replace_tabs(sb_cstr(&d));
    sb_free(&d);
    char *r = tc_fg(disp[0] == '+' ? TUI_TC_TOOL_DIFF_ADDED : disp[0] == '-' ? TUI_TC_TOOL_DIFF_REMOVED : TUI_TC_TOOL_OUTPUT, disp);
    free(disp);
    return r;
}

void tc_ast_edit(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    const json_t *ops = json_get_arr(c->args, "ops");
    size_t nops = json_len(ops);
    /* rewriteLabel: the single rewrite's pattern preview, or `N rewrites` for several */
    char *pv = nops == 1 ? pattern_preview(json_get_str(json_at(ops, 0), "pat")) : nops > 1 ? xasprintf("%zu rewrites", nops) : NULL;
    if (!c->has_result) {
        char *subj = scoped(pv ? pv : "?", c->args, NULL);
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "AST Edit", .title_slot = -1, .description = subj};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        free(subj);
        free(pv);
        return;
    }
    char *subject = scoped(pv, c->args, d);
    free(pv);
    if (c->is_error) {
        tc_status_line_t sl = {.icon = TC_ST_ERROR, .spinner = -1, .title = "AST Edit", .title_slot = -1, .description = subject};
        char *h = tc_status_line(&sl);
        char *t = tc_text_of(c->content);
        char *e = tc_error_detail(*t ? t : "Unknown error");
        tc_section_t sec = {0};
        tc_split_lines(e, &sec.lines);
        tc_block_t blk = {.header = h, .state = TC_STATE_ERROR, .sections = &sec, .nsections = 1, .width = width,
                          .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_ERROR};
        tc_output_block(&blk, out);
        strvec_free(&sec.lines);
        free(e);
        free(t);
        free(h);
        free(subject);
        return;
    }
    int64_t total = json_get_int(d, "totalReplacements", 0), touched = json_get_int(d, "filesTouched", 0);
    int64_t searched = json_get_int(d, "filesSearched", 0);
    bool limit = json_get_bool(d, "limitReached", false);
    const json_t *perrs = json_get_arr(d, "parseErrors");
    if (total == 0) {
        strvec_t meta = {0};
        strvec_push(&meta, "0 replacements");
        if (searched > 0) strvec_push_own(&meta, xasprintf("searched %lld", (long long)searched));
        /* only parse errors are worth a body; a framed card carries the counts in its bottom border */
        tc_section_t sec = {0};
        push_parse_errors(&sec.lines, perrs, json_get_int(d, "parseErrorsTotal", 0));
        if (!sec.lines.n) {
            tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "AST Edit", .title_slot = -1, .description = subject,
                                   .meta = (const char *const *)meta.v, .nmeta = meta.n};
            char *h = tc_status_line(&sl);
            tc_push_wrapped(out, h, width);
            free(h);
        } else {
            tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "AST Edit", .title_slot = -1, .description = subject};
            char *h = tc_status_line(&sl);
            char *joined = strvec_join(&meta, tc_sym("sep.dot"));
            char *fm = tc_fg(TUI_TC_DIM, joined);
            tc_block_t blk = {.header = h, .footer_meta = fm, .state = TC_STATE_WARNING, .sections = &sec, .nsections = 1, .width = width,
                              .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
            tc_output_block(&blk, out);
            free(fm);
            free(joined);
            free(h);
        }
        strvec_free(&sec.lines);
        strvec_free(&meta);
        free(subject);
        return;
    }
    tc_status_line_t sl = {.icon = limit ? TC_ST_WARNING : TC_ST_SUCCESS, .spinner = -1, .title = "AST Edit", .title_slot = -1,
                           .description = subject};
    char *header = tc_status_line(&sl);
    /* an unapplied result is a staged proposal; the bottom rail leads with that state */
    char *f_rep = tc_format_count("replacement", total), *f_file = tc_format_count("file", touched);
    char *f_searched = xasprintf("searched %lld", (long long)searched);
    char *dim_rep = tc_fg(TUI_TC_DIM, f_rep), *dim_file = tc_fg(TUI_TC_DIM, f_file), *dim_searched = tc_fg(TUI_TC_DIM, f_searched);
    char *dim_sep = tc_fg(TUI_TC_DIM, tc_sym("sep.dot"));
    sb_t foot = SB_INIT;
    if (!json_get_bool(d, "applied", false)) {
        tc_fg_sb(&foot, TUI_TC_WARNING, "proposed");
        sb_append(&foot, dim_sep);
    }
    sb_appendf(&foot, "%s%s%s%s%s", dim_rep, dim_sep, dim_file, dim_sep, dim_searched);
    free(f_rep);
    free(f_file);
    free(f_searched);
    free(dim_rep);
    free(dim_file);
    free(dim_searched);
    free(dim_sep);
    const char *dc = json_get_str(d, "displayContent");
    char *text = dc ? xstrdup(dc) : tc_text_of(c->content);
    /* per-file change groups, minus the safety-cap / parse trailers */
    tc_grouped_t g;
    tc_grouped_parse(&g, text, json_get_str(d, "searchPath"), NULL);
    tc_ggroup_t *kept = xcalloc(g.ngroups + 1, sizeof *kept);
    size_t nkept = 0;
    for (size_t i = 0; i < g.ngroups; i++)
        if (!drop_group(&g, &g.groups[i], "Safety cap reached", "Parse issues:")) kept[nkept++] = g.groups[i];
    /* buildChangeBody: files apart by a blank line and without tree guides (the frame border is the container),
     * fit to the budget with the rest summarized as `… N more files` */
    tc_ggroup_t *shown;
    size_t nshown;
    char *summary;
    tc_grouped_fit(&g, kept, nkept, c->expanded ? -1 : 6, file_unit, NULL, "file", 0, 1, &shown, &nshown, &summary);
    strvec_t body = {0};
    for (size_t gi = 0; gi < nshown; gi++) {
        if (body.n) strvec_push(&body, "");
        for (size_t k = 0; k < shown[gi].n; k++) strvec_push_own(&body, edit_style(&g, shown[gi].v[k]));
    }
    if (summary) strvec_push_own(&body, tc_fg(TUI_TC_MUTED, summary));
    if (limit) strvec_push_own(&body, tc_fg(TUI_TC_WARNING, "limit reached; narrow path"));
    if (json_len(perrs)) {
        char *lab = parse_errors_label(d);
        strvec_push_own(&body, tc_fg(TUI_TC_WARNING, lab));
        free(lab);
    }
    int cw = tc_block_content_width(width, -1, -1);
    tc_section_t sec = {0};
    for (size_t i = 0; i < body.n; i++) {
        if (!sec.lines.n && blank(body.v[i])) continue;
        strvec_push_own(&sec.lines, tui_truncate_to_width(body.v[i], cw, ""));
    }
    tc_block_t blk = {.header = header, .footer_meta = sb_cstr(&foot), .state = c->partial ? TC_STATE_PENDING : TC_STATE_SUCCESS, .sections = &sec,
                      .nsections = sec.lines.n ? 1 : 0, .width = width, .pad_left = -1, .pad_right = -1,
                      .border_slot = TUI_TC_BORDER_MUTED};
    tc_output_block(&blk, out);
    strvec_free(&sec.lines);
    strvec_free(&body);
    free(summary);
    free(shown);
    free(kept);
    tc_grouped_free(&g);
    free(text);
    free(header);
    sb_free(&foot);
    free(subject);
}
