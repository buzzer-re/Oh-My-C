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

/* displayContent rows styled (headers tinted, code frames highlighted, `  meta:` dim) and grouped by blank
 * lines; groups starting with one of `drop` are left out. Rows of groups starting past `budget` render
 * blank (omp renders them empty: they never show collapsed). */
static void styled_groups(const char *text, const json_t *d, const char *force_lang, bool expanded, int budget,
                          const char *const *drop, strvec_t *items) {
    strvec_t lines = {0};
    tc_split_lines(text, &lines);
    const char *cur = NULL;
    const json_t *fl = json_get_arr(d, "files");
    if (fl && json_len(fl) == 1 && json_is_str(json_at(fl, 0))) cur = tc_lang_from_path(json_sv(json_at(fl, 0)));
    bool has_sep = false;
    for (size_t i = 0; i < lines.n; i++) has_sep |= blank(lines.v[i]);
    size_t start_line = 0;
    for (size_t i = 0; i < lines.n;) {
        if (blank(lines.v[i])) {
            i++;
            continue;
        }
        size_t gs = i;
        sb_t g = SB_INIT;
        size_t cnt = 0;
        bool show = expanded || (int)start_line < budget;
        for (; i < lines.n; i++) {
            const char *l = lines.v[i];
            if (blank(l)) {
                if (has_sep) break;
                continue;
            }
            if (tc_is_search_header(l)) {
                size_t rl = strlen(l);
                cur = rl && l[rl - 1] == '/' ? NULL : tc_search_header_lang(l);
            }
            if (cnt++) sb_putc(&g, '\n');
            if (!show) continue;
            char *st = !tc_is_search_header(l) && str_starts(l, "  meta:") ? tc_fg(TUI_TC_DIM, l)
                                                                           : tc_style_search_line(l, force_lang ? force_lang : cur);
            sb_append(&g, st);
            free(st);
        }
        bool dropped = false;
        for (size_t k = 0; drop && drop[k]; k++) dropped |= str_starts(lines.v[gs], drop[k]);
        if (dropped) {
            sb_free(&g);
            continue;
        }
        start_line += cnt;
        strvec_push_own(items, sb_detach(&g));
    }
    strvec_free(&lines);
}

void tc_ast_grep(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    const char *pat = json_get_str(c->args, "pat");
    if (!c->has_result) {
        strvec_t meta = {0};
        char *ps = tc_paths_of(c->args);
        if (ps) strvec_push_own(&meta, xasprintf("in %s", ps));
        free(ps);
        if (json_get_int(c->args, "skip", 0) > 0) strvec_push_own(&meta, xasprintf("skip:%lld", (long long)json_get_int(c->args, "skip", 0)));
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "AST Grep", .title_slot = -1,
                               .description = pat ? pat : "?", .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        strvec_free(&meta);
        return;
    }
    if (c->is_error) {
        char *t = tc_text_of(c->content);
        char *e = tc_error_message(*t ? t : "Unknown error");
        tc_push_wrapped(out, e, width);
        free(e);
        free(t);
        return;
    }
    int64_t matches = json_get_int(d, "matchCount", 0), files = json_get_int(d, "fileCount", 0);
    int64_t searched = json_get_int(d, "filesSearched", 0);
    bool limit = json_get_bool(d, "limitReached", false);
    const char *scope = json_get_str(d, "scopePath");
    const json_t *perrs = json_get_arr(d, "parseErrors");
    if (matches == 0) {
        strvec_t meta = {0};
        strvec_push(&meta, "0 matches");
        if (scope) strvec_push_own(&meta, xasprintf("in %s", scope));
        if (searched > 0) strvec_push_own(&meta, xasprintf("searched %lld", (long long)searched));
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "AST Grep", .title_slot = -1, .description = pat,
                               .meta = (const char *const *)meta.v, .nmeta = meta.n};
        strvec_t lines = {0};
        strvec_push_own(&lines, tc_status_line(&sl));
        strvec_push_own(&lines, tc_empty_message("No matches found"));
        if (json_len(perrs)) {
            strvec_push_own(&lines, tc_fg(TUI_TC_WARNING, "Query may be mis-scoped; narrow `path` before concluding absence"));
            push_parse_errors(&lines, perrs, json_get_int(d, "parseErrorsTotal", 0));
        }
        char *j = strvec_join(&lines, "\n");
        tc_push_wrapped(out, j, width);
        free(j);
        strvec_free(&lines);
        strvec_free(&meta);
        return;
    }
    strvec_t meta = {0};
    strvec_push_own(&meta, tc_format_count("match", matches));
    strvec_push_own(&meta, tc_format_count("file", files));
    if (scope) strvec_push_own(&meta, xasprintf("in %s", scope));
    strvec_push_own(&meta, xasprintf("searched %lld", (long long)searched));
    if (limit) strvec_push_own(&meta, tc_fg(TUI_TC_WARNING, "limit reached"));
    char *ic = limit ? NULL : tc_fg(TUI_TC_ACCENT, tc_sym("icon.search"));
    tc_status_line_t sl = {.icon = TC_ST_WARNING, .icon_override = ic, .spinner = -1, .title = "AST Grep", .title_slot = -1,
                           .description = pat, .meta = (const char *const *)meta.v, .nmeta = meta.n};
    char *header = tc_status_line(&sl);
    const char *dc = json_get_str(d, "displayContent");
    char *text = dc ? xstrdup(dc) : tc_text_of(c->content);
    static const char *const DROP[] = {"Result limit reached", "Parse issues:", NULL};
    strvec_t items = {0}, rows = {0};
    styled_groups(text, d, json_get_str(c->args, "lang"), c->expanded, 6, DROP, &items);
    tc_tree_list_ex(&items, c->expanded, (int)items.n, c->expanded ? -1 : 6, "match", &rows);
    if (limit) strvec_push_own(&rows, tc_fg(TUI_TC_WARNING, "limit reached; narrow path or use skip"));
    if (json_len(perrs)) {
        char *lab = parse_errors_label(d);
        strvec_push_own(&rows, tc_fg(TUI_TC_WARNING, lab));
        free(lab);
    }
    tc_fit_headered(header, &rows, width, out);
    strvec_free(&rows);
    strvec_free(&items);
    free(text);
    free(header);
    free(ic);
    strvec_free(&meta);
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

static char *edit_style_line(const char *line) {
    sb_t d = SB_INIT;
    const char *bar = strstr(line, "\xe2\x94\x82");
    if (bar) {
        sb_appendn(&d, line, (size_t)(bar - line));
        sb_putc(&d, ' ');
        sb_append(&d, bar + 3);
    } else sb_append(&d, line);
    char *disp = tc_replace_tabs(sb_cstr(&d));
    sb_free(&d);
    char *r;
    if (tc_is_search_header(line)) {
        size_t depth = strspn(line, "#");
        char *rest = str_trim(line + depth);
        size_t rl = strlen(rest);
        bool dir = rl && rest[rl - 1] == '/';
        r = tc_fg(dir || depth == 1 ? TUI_TC_ACCENT : TUI_TC_DIM, disp);
        free(rest);
    } else if (disp[0] == '+') r = tc_fg(TUI_TC_TOOL_DIFF_ADDED, disp);
    else if (disp[0] == '-') r = tc_fg(TUI_TC_TOOL_DIFF_REMOVED, disp);
    else r = tc_fg(TUI_TC_TOOL_OUTPUT, disp);
    free(disp);
    return r;
}

void tc_ast_edit(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    const json_t *ops = json_get_arr(c->args, "ops");
    size_t nops = json_len(ops);
    char *pv = nops == 1 ? pattern_preview(json_get_str(json_at(ops, 0), "pat")) : NULL;
    if (!c->has_result) {
        strvec_t meta = {0};
        const json_t *paths = json_get_arr(c->args, "paths");
        if (json_len(paths)) {
            strvec_t p = {0};
            json_t *e;
            json_arr_foreach(paths, i, e) if (json_is_str(e)) strvec_push(&p, json_sv(e));
            char *j = strvec_join(&p, ", ");
            strvec_push_own(&meta, xasprintf("in %s", j));
            free(j);
            strvec_free(&p);
        }
        char *rw = nops ? xasprintf("%zu rewrites", nops) : NULL;
        if (nops > 1) strvec_push(&meta, rw);
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "AST Edit", .title_slot = -1,
                               .description = nops == 1 ? pv : nops ? rw : "?", .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        free(rw);
        strvec_free(&meta);
        free(pv);
        return;
    }
    if (c->is_error) {
        tc_status_line_t sl = {.icon = TC_ST_ERROR, .spinner = -1, .title = "AST Edit", .title_slot = -1};
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
        free(pv);
        return;
    }
    int64_t total = json_get_int(d, "totalReplacements", 0), touched = json_get_int(d, "filesTouched", 0);
    int64_t searched = json_get_int(d, "filesSearched", 0);
    bool limit = json_get_bool(d, "limitReached", false);
    const char *scope = json_get_str(d, "scopePath");
    const json_t *perrs = json_get_arr(d, "parseErrors");
    if (total == 0) {
        strvec_t meta = {0};
        strvec_push(&meta, "0 replacements");
        if (scope) strvec_push_own(&meta, xasprintf("in %s", scope));
        if (searched > 0) strvec_push_own(&meta, xasprintf("searched %lld", (long long)searched));
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "AST Edit", .title_slot = -1, .description = pv,
                               .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_section_t sec = {0};
        push_parse_errors(&sec.lines, perrs, json_get_int(d, "parseErrorsTotal", 0));
        if (!sec.lines.n) tc_push_wrapped(out, h, width);
        else {
            tc_block_t blk = {.header = h, .state = TC_STATE_WARNING, .sections = &sec, .nsections = 1, .width = width,
                              .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
            tc_output_block(&blk, out);
        }
        strvec_free(&sec.lines);
        free(h);
        strvec_free(&meta);
        free(pv);
        return;
    }
    strvec_t meta = {0};
    strvec_push_own(&meta, tc_format_count("replacement", total));
    strvec_push_own(&meta, tc_format_count("file", touched));
    if (scope) strvec_push_own(&meta, xasprintf("in %s", scope));
    strvec_push_own(&meta, xasprintf("searched %lld", (long long)searched));
    if (limit) strvec_push_own(&meta, tc_fg(TUI_TC_WARNING, "limit reached"));
    tc_status_line_t sl = {.icon = limit ? TC_ST_WARNING : TC_ST_SUCCESS, .spinner = -1, .title = "AST Edit", .title_slot = -1,
                           .description = pv, .badge = "proposed", .badge_slot = TUI_TC_WARNING,
                           .meta = (const char *const *)meta.v, .nmeta = meta.n};
    char *header = tc_status_line(&sl);
    const char *dc = json_get_str(d, "displayContent");
    char *text = dc ? xstrdup(dc) : tc_text_of(c->content);
    strvec_t lines = {0};
    tc_split_lines(text, &lines);
    /* change groups (blank-separated), minus the safety-cap / parse trailers */
    bool has_sep = false;
    for (size_t i = 0; i < lines.n; i++) has_sep |= blank(lines.v[i]);
    VEC(strvec_t) groups = {0};
    for (size_t i = 0; i < lines.n;) {
        if (blank(lines.v[i])) {
            i++;
            continue;
        }
        size_t gs = i;
        strvec_t g = {0};
        for (; i < lines.n; i++) {
            if (blank(lines.v[i])) {
                if (has_sep) break;
                continue;
            }
            strvec_push(&g, lines.v[i]);
        }
        if (str_starts(lines.v[gs], "Safety cap reached") || str_starts(lines.v[gs], "Parse issues:")) strvec_free(&g);
        else vec_push(&groups, g);
    }
    /* buildChangeBody */
    strvec_t body = {0};
    size_t shown = 0;
    for (size_t gi = 0; gi < groups.n; gi++) {
        strvec_t *g = &groups.v[gi];
        size_t sep = shown > 0 ? 1 : 0;
        size_t reserved = !c->expanded && groups.n - (gi + 1) > 0 ? 1 : 0;
        if (!c->expanded && shown > 0 && body.n + sep + g->n + reserved > 6) break;
        if (sep) strvec_push(&body, "");
        for (size_t k = 0; k < g->n; k++) strvec_push_own(&body, edit_style_line(g->v[k]));
        shown++;
    }
    if (!c->expanded && groups.n > shown) {
        char *m = tc_more_items((int64_t)(groups.n - shown), "change");
        strvec_push_own(&body, tc_fg(TUI_TC_MUTED, m));
        free(m);
    }
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
    tc_block_t blk = {.header = header, .state = c->partial ? TC_STATE_PENDING : TC_STATE_SUCCESS, .sections = &sec,
                      .nsections = sec.lines.n ? 1 : 0, .width = width, .pad_left = -1, .pad_right = -1,
                      .border_slot = TUI_TC_BORDER_MUTED};
    tc_output_block(&blk, out);
    strvec_free(&sec.lines);
    strvec_free(&body);
    for (size_t i = 0; i < groups.n; i++) strvec_free(&groups.v[i]);
    vec_free(&groups);
    strvec_free(&lines);
    free(text);
    free(header);
    strvec_free(&meta);
    free(pv);
}
