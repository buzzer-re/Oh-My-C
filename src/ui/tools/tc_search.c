/* tc_search.c - grep and glob cards (port of omp tools/grep.ts, tools/glob.ts, render/file-list.ts,
 * tools/grouped-file-output.ts). */
#include "ui/tools/tc.h"

#include <ctype.h>

#include "tui/ansi.h"
#include "tui/markdown.h"

static char *search_icon(void) { return tc_fg(TUI_TC_TOOL_TITLE, tc_sym("icon.search")); }

static void meta_push(strvec_t *m, char *s) { strvec_push_own(m, s); }

/* path arg(s) joined: "a, b" */
char *tc_paths_of(const json_t *args) {
    const json_t *p = json_get(args, "path");
    if (!p) p = json_get(args, "paths");
    if (json_is_str(p)) return *json_sv(p) ? xstrdup(json_sv(p)) : NULL;
    if (json_is_arr(p) && json_len(p)) {
        strvec_t v = {0};
        json_t *e;
        json_arr_foreach(p, i, e) if (json_is_str(e)) strvec_push(&v, json_sv(e));
        char *r = v.n ? strvec_join(&v, ", ") : NULL;
        strvec_free(&v);
        return r;
    }
    return NULL;
}

/* ---------- grep ---------- */

bool tc_is_search_header(const char *l) {
    const char *p = l;
    while (*p == '#') p++;
    return p > l && *p == ' ';
}

static bool is_match_line(const char *l) {
    const char *p = l;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '*') return false;
    p++;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) p++;
    return str_starts(p, "\xe2\x94\x82") || *p == ':' || *p == '|';
}

/* numbered code-frame row (`*12│src`, ` 3│src`): returns the gutter length, 0 when not one */
static size_t frame_gutter(const char *l) {
    const char *p = l;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '*') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    if (str_starts(p, "\xe2\x94\x82")) return (size_t)(p - l) + 3;
    if (*p == ':' || *p == '|') return (size_t)(p - l) + 1;
    return 0;
}

/* language of a `## name#TAG (suffix)` header's file */
const char *tc_search_header_lang(const char *line) {
    const char *rest = line + strspn(line, "#");
    while (*rest == ' ') rest++;
    char *name = xstrdup(rest);
    char *paren = strstr(name, " (");
    if (paren) *paren = 0;
    char *hash = strrchr(name, '#');
    if (hash && hash != name) *hash = 0;
    const char *l = tc_lang_from_path(name);
    free(name);
    return l;
}

/* createSearchLineStyler + createCodeFrameStyler (omp 18.14): headers accent/dim, code-frame rows with a
 * toolOutput gutter and the source highlighted in its file's language */
char *tc_style_search_line(const char *line, const char *lang) {
    if (tc_is_search_header(line)) {
        size_t depth = strspn(line, "#");
        const char *rest = line + depth + 1;
        size_t rl = strlen(rest);
        while (rl && rest[rl - 1] == ' ') rl--;
        bool dir = rl && rest[rl - 1] == '/';
        if (dir || depth == 1) return tc_fg(TUI_TC_ACCENT, line);
        return tc_fg(TUI_TC_DIM, line);
    }
    size_t g = frame_gutter(line);
    if (g) {
        char *gut = xstrndup(line, g);
        char *src = tc_replace_tabs(line + g);
        char *gs = tc_fg(TUI_TC_TOOL_OUTPUT, gut);
        char *ss;
        if (lang && strcmp(lang, "text")) { /* known grammar: syntect output (plain for grammars omc lacks) */
            strvec_t hl = {0};
            tui_highlight_code(src, lang, NULL, &hl);
            ss = hl.n ? xstrdup(hl.v[0]) : xstrdup("");
            strvec_free(&hl);
        } else ss = tc_fg(TUI_TC_TOOL_OUTPUT, src);
        char *r = xasprintf("%s%s", gs, ss);
        free(ss);
        free(gs);
        free(src);
        free(gut);
        return r;
    }
    return tc_fg(TUI_TC_TOOL_OUTPUT, line);
}

void tc_grep(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    const char *pattern = json_get_str(c->args, "pattern");
    if (!c->has_result || c->partial) {
        strvec_t meta = {0};
        char *ps = tc_paths_of(c->args);
        if (ps) meta_push(&meta, xasprintf("in %s", ps));
        free(ps);
        if (json_get(c->args, "case") && !json_get_bool(c->args, "case", true)) strvec_push(&meta, "case:insensitive");
        if (json_get(c->args, "gitignore") && !json_get_bool(c->args, "gitignore", true)) strvec_push(&meta, "gitignore:false");
        if (json_get_int(c->args, "skip", 0) > 0) meta_push(&meta, xasprintf("skip:%lld", (long long)json_get_int(c->args, "skip", 0)));
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "Grep", .title_slot = TUI_TC_TOOL_TITLE,
                               .description = pattern && *pattern ? pattern : "?", .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        strvec_free(&meta);
        return;
    }
    if (c->is_error || json_get_str(d, "error")) {
        char *t = json_get_str(d, "error") ? xstrdup(json_get_str(d, "error")) : tc_text_of(c->content);
        char *e = tc_error_message(*t ? t : "Unknown error");
        tc_push_wrapped(out, e, width);
        free(e);
        free(t);
        return;
    }
    bool detailed = json_get(d, "matchCount") || json_get(d, "fileCount");
    const char *dc = json_get_str(d, "displayContent");
    char *text = dc ? xstrdup(dc) : tc_text_of(c->content);
    int64_t matches = json_get_int(d, "matchCount", 0), files = json_get_int(d, "fileCount", 0);
    const char *scope = json_get_str(d, "scopePath");
    strvec_t missing = {0};
    {
        json_t *e;
        json_arr_foreach(json_get_arr(d, "missingPaths"), i, e) if (json_is_str(e)) strvec_push(&missing, json_sv(e));
    }
    char *missing_note = NULL;
    if (missing.n) {
        char *j = strvec_join(&missing, ", ");
        char *t = xasprintf("skipped missing: %s", j);
        missing_note = tc_fg(TUI_TC_WARNING, t);
        free(t);
        free(j);
    }
    if (!detailed) {
        if (!*text || !strcmp(text, "No matches found")) {
            char *e = tc_empty_message("No matches found");
            tc_push_wrapped(out, e, width);
            free(e);
        } else {
            strvec_t items = {0}, all = {0};
            tc_split_lines(text, &all);
            for (size_t i = 0; i < all.n; i++)
                if (strspn(all.v[i], " \t") != strlen(all.v[i])) strvec_push_own(&items, tc_fg(TUI_TC_TOOL_OUTPUT, all.v[i]));
            char *ic = search_icon();
            char *cnt = tc_format_count("item", (int64_t)items.n);
            const char *m[] = {cnt};
            tc_status_line_t sl = {.icon_override = ic, .spinner = -1, .title = "Grep", .title_slot = TUI_TC_TOOL_TITLE,
                                   .description = pattern, .meta = m, .nmeta = 1};
            char *h = tc_status_line(&sl);
            strvec_t rows = {0};
            tc_tree_list(&items, c->expanded, 6, "item", &rows);
            tc_fit_headered(h, &rows, width, out);
            strvec_free(&rows);
            free(h);
            free(cnt);
            free(ic);
            strvec_free(&items);
            strvec_free(&all);
        }
        free(missing_note);
        strvec_free(&missing);
        free(text);
        return;
    }
    if (matches == 0) {
        strvec_t meta = {0};
        strvec_push(&meta, "0 matches");
        if (scope) meta_push(&meta, xasprintf("in %s", scope));
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "Grep", .title_slot = TUI_TC_TOOL_TITLE,
                               .description = pattern, .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        char *e = tc_empty_message("No matches found");
        tc_push_wrapped(out, e, width);
        if (missing_note) tc_push_wrapped(out, missing_note, width);
        free(e);
        free(h);
        strvec_free(&meta);
        free(missing_note);
        strvec_free(&missing);
        free(text);
        return;
    }
    bool truncated = json_get_bool(d, "truncated", false) || json_get_obj(d, "truncation");
    strvec_t meta = {0};
    meta_push(&meta, tc_format_count("match", matches));
    meta_push(&meta, tc_format_count("file", files));
    if (scope) meta_push(&meta, xasprintf("in %s", scope));
    if (truncated) meta_push(&meta, tc_fg(TUI_TC_WARNING, "truncated"));
    char *ic = truncated ? NULL : search_icon();
    tc_status_line_t sl = {.icon = truncated ? TC_ST_WARNING : TC_ST_NONE, .icon_override = ic, .spinner = -1, .title = "Grep",
                           .title_slot = TUI_TC_TOOL_TITLE, .description = pattern, .meta = (const char *const *)meta.v,
                           .nmeta = meta.n};
    char *header = tc_status_line(&sl);

    /* renderBudgetedSearchGroups */
    strvec_t lines = {0};
    tc_split_lines(text, &lines);
    const char **line_lang = xcalloc(lines.n + 1, sizeof(char *));
    {
        const char *cur = NULL;
        const json_t *fl = json_get_arr(d, "files");
        if (fl && json_len(fl) == 1 && json_is_str(json_at(fl, 0))) cur = tc_lang_from_path(json_sv(json_at(fl, 0)));
        for (size_t k = 0; k < lines.n; k++) {
            if (tc_is_search_header(lines.v[k])) {
                size_t rl = strlen(lines.v[k]);
                cur = rl && lines.v[k][rl - 1] == '/' ? NULL : tc_search_header_lang(lines.v[k]);
            } else line_lang[k] = cur;
        }
    }
    VEC(size_t) idx = {0};
    VEC(size_t) gstart = {0}, glen = {0};
    bool has_sep = false;
    for (size_t i = 0; i < lines.n; i++)
        if (strspn(lines.v[i], " \t") == strlen(lines.v[i])) has_sep = true;
    for (size_t i = 0; i < lines.n;) {
        if (strspn(lines.v[i], " \t") == strlen(lines.v[i])) {
            i++;
            continue;
        }
        size_t gs = i;
        while (i < lines.n && !(has_sep && strspn(lines.v[i], " \t") == strlen(lines.v[i]))) i++;
        size_t s = idx.n;
        for (size_t k = gs; k < i; k++) {
            if (strspn(lines.v[k], " \t") == strlen(lines.v[k])) continue;
            if (c->expanded || tc_is_search_header(lines.v[k]) || is_match_line(lines.v[k])) vec_push(&idx, k);
        }
        if (idx.n == s) /* compaction left nothing: keep the whole group */
            for (size_t k = gs; k < i; k++)
                if (strspn(lines.v[k], " \t") != strlen(lines.v[k])) vec_push(&idx, k);
        if (idx.n > s) {
            vec_push(&gstart, s);
            vec_push(&glen, idx.n - s);
        }
    }
    int extra = missing_note ? 1 : 0;
    int budget = MAX((c->expanded ? 24 : 6) - extra, 0);
    size_t total_lines = idx.n, total_marked = 0, total_fallback = 0;
    for (size_t k = 0; k < idx.n; k++) {
        const char *l = lines.v[idx.v[k]];
        if (is_match_line(l)) total_marked++;
        if (!tc_is_search_header(l) && *l) total_fallback++;
    }
    bool marked = total_marked > 0;
    bool needs_summary = (int)total_lines > budget;
    int content_budget = needs_summary ? MAX(budget - 1, 0) : budget;
    strvec_t rows = {0};
    size_t vis_lines = 0, vis_matches = 0;
    VEC(size_t) vg_start = {0}, vg_len = {0};
    for (size_t g = 0; g < gstart.n && budget > 0; g++) {
        if ((int)vis_lines >= content_budget) break;
        size_t take = MIN(glen.v[g], (size_t)(content_budget - (int)vis_lines));
        if (!take) break;
        vec_push(&vg_start, gstart.v[g]);
        vec_push(&vg_len, take);
        vis_lines += take;
        for (size_t k = 0; k < take; k++) {
            const char *l = lines.v[idx.v[gstart.v[g] + k]];
            if (marked ? is_match_line(l) : (!tc_is_search_header(l) && *l)) vis_matches++;
        }
    }
    size_t total_matches = marked ? total_marked : MAX((size_t)matches, total_fallback);
    size_t hidden_matches = total_matches > vis_matches ? total_matches - vis_matches : 0;
    size_t hidden_lines = total_lines > vis_lines ? total_lines - vis_lines : 0;
    bool has_summary = needs_summary && (hidden_matches > 0 || hidden_lines > 0);
    for (size_t g = 0; g < vg_start.n; g++) {
        bool last = !has_summary && g + 1 == vg_start.n;
        char *pre = tc_fg(TUI_TC_DIM, last ? tc_sym("tree.last") : tc_sym("tree.branch"));
        char *cont = tc_fg(TUI_TC_DIM, last ? "   " : "\xe2\x94\x82  ");
        for (size_t k = 0; k < vg_len.v[g]; k++) {
            size_t li = idx.v[vg_start.v[g] + k];
            char *st = tc_style_search_line(lines.v[li], line_lang[li]);
            char *rt = tc_replace_tabs(st);
            if (k == 0) strvec_push_own(&rows, xasprintf("%s %s", pre, rt));
            else strvec_push_own(&rows, xasprintf("%s%s", cont, rt));
            free(rt);
            free(st);
        }
        free(cont);
        free(pre);
    }
    if (has_summary) {
        char *lab = hidden_matches > 0 ? tc_more_items((int64_t)hidden_matches, "match") : tc_more_items((int64_t)hidden_lines, "line");
        char *conn = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
        char *ml = tc_fg(TUI_TC_MUTED, lab);
        strvec_push_own(&rows, xasprintf("%s %s", conn, ml));
        free(ml);
        free(conn);
        free(lab);
    }
    if (missing_note) strvec_push(&rows, missing_note);
    tc_fit_headered(header, &rows, width, out);
    strvec_free(&rows);
    vec_free(&vg_start);
    vec_free(&vg_len);
    vec_free(&gstart);
    vec_free(&glen);
    vec_free(&idx);
    free(line_lang);
    strvec_free(&lines);
    free(header);
    free(ic);
    strvec_free(&meta);
    free(missing_note);
    strvec_free(&missing);
    free(text);
}

/* ---------- glob ---------- */

void tc_glob(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    char *desc = tc_paths_of(c->args);
    if (!c->has_result || c->partial) {
        strvec_t meta = {0};
        if (json_get(c->args, "limit")) meta_push(&meta, xasprintf("limit:%lld", (long long)json_get_int(c->args, "limit", 0)));
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "Glob", .title_slot = TUI_TC_TOOL_TITLE,
                               .description = desc ? desc : "*", .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        strvec_free(&meta);
        free(desc);
        return;
    }
    if (c->is_error || json_get_str(d, "error")) {
        char *t = json_get_str(d, "error") ? xstrdup(json_get_str(d, "error")) : tc_text_of(c->content);
        char *e = tc_error_message(*t ? t : "Unknown error");
        tc_push_wrapped(out, e, width);
        free(e);
        free(t);
        free(desc);
        return;
    }
    char *text = tc_text_of(c->content);
    if (!json_get(d, "fileCount")) {
        char *tr = str_trim(text);
        if (!*tr || strstr(text, "No files matching") || strstr(text, "No files found")) {
            char *e = tc_empty_message("No files found");
            tc_push_wrapped(out, e, width);
            free(e);
        } else {
            strvec_t all = {0}, items = {0};
            tc_split_lines(text, &all);
            for (size_t i = 0; i < all.n; i++)
                if (strspn(all.v[i], " \t") != strlen(all.v[i])) strvec_push_own(&items, tc_fg(TUI_TC_ACCENT, all.v[i]));
            char *ic = search_icon();
            char *cnt = tc_format_count("file", (int64_t)items.n);
            const char *m[] = {cnt};
            tc_status_line_t sl = {.icon_override = ic, .spinner = -1, .title = "Glob", .title_slot = TUI_TC_TOOL_TITLE,
                                   .description = desc, .meta = m, .nmeta = 1};
            char *h = tc_status_line(&sl);
            strvec_t rows = {0};
            tc_tree_list(&items, c->expanded, 8, "file", &rows);
            tc_fit_headered(h, &rows, width, out);
            strvec_free(&rows);
            free(h);
            free(cnt);
            free(ic);
            strvec_free(&items);
            strvec_free(&all);
        }
        free(tr);
        free(text);
        free(desc);
        return;
    }
    int64_t count = json_get_int(d, "fileCount", 0);
    bool truncated = json_get_bool(d, "truncated", false) || json_get_obj(d, "truncation") ||
                     json_get_int(d, "resultLimitReached", 0) > 0 || json_get_bool(d, "resultLimitReached", false);
    bool timed_out = json_get_bool(d, "timedOut", false);
    strvec_t missing = {0};
    {
        json_t *e;
        json_arr_foreach(json_get_arr(d, "missingPaths"), i, e) if (json_is_str(e)) strvec_push(&missing, json_sv(e));
    }
    char *missing_note = NULL;
    if (missing.n) {
        char *j = strvec_join(&missing, ", ");
        char *t = xasprintf("skipped missing: %s", j);
        missing_note = tc_fg(TUI_TC_WARNING, t);
        free(t);
        free(j);
    }
    if (count == 0) {
        bool to = timed_out || truncated;
        strvec_t meta = {0};
        strvec_push(&meta, "0 files");
        if (to) meta_push(&meta, tc_fg(TUI_TC_WARNING, "timed out"));
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "Glob", .title_slot = TUI_TC_TOOL_TITLE,
                               .description = desc, .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        char *e = tc_empty_message(to ? "No matches before timeout (scan incomplete)" : "No files found");
        tc_push_wrapped(out, e, width);
        if (missing_note) tc_push_wrapped(out, missing_note, width);
        free(e);
        free(h);
        strvec_free(&meta);
    } else {
        strvec_t meta = {0};
        meta_push(&meta, tc_format_count("file", count));
        if (json_get_str(d, "scopePath")) meta_push(&meta, xasprintf("in %s", json_get_str(d, "scopePath")));
        if (truncated) meta_push(&meta, tc_fg(TUI_TC_WARNING, timed_out ? "timed out" : "truncated"));
        char *ic = truncated ? NULL : search_icon();
        tc_status_line_t sl = {.icon = truncated ? TC_ST_WARNING : TC_ST_NONE, .icon_override = ic, .spinner = -1,
                               .title = "Glob", .title_slot = TUI_TC_TOOL_TITLE, .description = desc,
                               .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        strvec_t items = {0};
        json_t *e;
        json_arr_foreach(json_get_arr(d, "files"), i, e) {
            if (!json_is_str(e)) continue;
            const char *f = json_sv(e);
            bool dir = str_ends(f, "/");
            char *icon = dir ? tc_fg(TUI_TC_ACCENT, tc_sym("icon.folder")) : tc_fg(TUI_TC_MUTED, tc_lang_icon(tc_lang_from_path(f)));
            char *ps = tc_fg(dir ? TUI_TC_ACCENT : TUI_TC_TOOL_OUTPUT, f);
            strvec_push_own(&items, xasprintf("%s %s", icon, ps));
            free(ps);
            free(icon);
        }
        strvec_t rows = {0};
        tc_tree_list(&items, c->expanded, 8, "file", &rows);
        int64_t lim = json_get_int(d, "resultLimitReached", 0);
        if (lim > 0) {
            char *t = xasprintf("truncated: limit %lld results", (long long)lim);
            strvec_push_own(&rows, tc_fg(TUI_TC_WARNING, t));
            free(t);
        }
        if (missing_note) strvec_push(&rows, missing_note);
        tc_fit_headered(h, &rows, width, out);
        strvec_free(&rows);
        strvec_free(&items);
        free(h);
        free(ic);
        strvec_free(&meta);
    }
    free(missing_note);
    strvec_free(&missing);
    free(text);
    free(desc);
}
