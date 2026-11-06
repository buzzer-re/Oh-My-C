/* tc_search.c - grep and glob cards (port of omp tools/grep.ts, tools/glob.ts, render/file-list.ts,
 * tools/grouped-file-output.ts). */
#include "ui/tools/tc.h"

#include <ctype.h>
#include <unistd.h>

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

/* formatScopedSubject: `pattern in scope`, with `in` dim so the pattern and the scope read as one subject */
char *tc_scoped_subject(const char *subject, const char *scope) {
    if (!scope || !*scope) return subject ? xstrdup(subject) : NULL;
    char *kw = tc_fg(TUI_TC_DIM, "in");
    char *r = subject ? xasprintf("%s %s %s", subject, kw, scope) : xasprintf("%s %s", kw, scope);
    free(kw);
    return r;
}

/* fileHyperlink: `text` as an OSC 8 link to a local path (resolved against the cwd, percent-encoded) */
char *tc_file_hyperlink(const char *path, const char *text) {
    char cwd[4096];
    sb_t uri = SB_INIT;
    sb_append(&uri, "file://");
    if (path[0] != '/' && getcwd(cwd, sizeof cwd)) {
        sb_append(&uri, cwd);
        sb_putc(&uri, '/');
    }
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (isalnum(*p) || strchr("/-_.~", *p)) sb_putc(&uri, (char)*p);
        else sb_appendf(&uri, "%%%02X", *p);
    }
    char *r = tc_hyperlink(sb_cstr(&uri), text);
    sb_free(&uri);
    return r;
}

/* ---------- grouped output (tools/grouped-file-output.ts) ---------- */

bool tc_is_url_like(const char *path) {
    /* URL_LIKE_PATH_RE: /^[a-z][a-z0-9+.-]*:\/\//i */
    const char *p = path ? path : "";
    if (!isalpha((unsigned char)*p)) return false;
    p++;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '.' || *p == '-') p++;
    return str_starts(p, "://");
}

bool tc_grouped_is_match(const char *l) {
    const char *p = l;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '*') return false;
    p++;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) p++;
    return str_starts(p, "\xe2\x94\x82") || *p == ':' || *p == '|';
}

/* GROUPED_NUMBERED_LINE_RE: `*12│src`, ` 3:src`. Returns the gutter length (0: not a frame), `num` the line number. */
static size_t frame_gutter(const char *l, long *num) {
    const char *p = l;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '*') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    long n = strtol(p, NULL, 10);
    while (isdigit((unsigned char)*p)) p++;
    size_t g = 0;
    if (str_starts(p, "\xe2\x94\x82")) g = (size_t)(p - l) + 3;
    else if (*p == ':' || *p == '|') g = (size_t)(p - l) + 1;
    if (g && num) *num = n;
    return g;
}

/* HEADER_SUFFIX_RE: the start of a trailing `  (note)`, NULL when none */
static const char *header_suffix(const char *rest) {
    size_t n = strlen(rest);
    while (n && isspace((unsigned char)rest[n - 1])) n--;
    if (!n || rest[n - 1] != ')') return NULL;
    size_t open = n - 1;
    while (open && rest[open - 1] != '(' && rest[open - 1] != ')') open--;
    if (!open || rest[open - 1] != '(') return NULL;
    open--;
    if (!open || !isspace((unsigned char)rest[open - 1])) return NULL;
    while (open && isspace((unsigned char)rest[open - 1])) open--;
    return rest + open;
}

/* HASHLINE_TAG_SUFFIX_RE: a trailing `#TAG` exactly as the hashline store writes it (4 uppercase hex) */
static void strip_hashline_tag(char *name) {
    size_t n = strlen(name);
    if (n < 5 || name[n - 5] != '#') return;
    for (size_t i = n - 4; i < n; i++)
        if (!(isdigit((unsigned char)name[i]) || (name[i] >= 'A' && name[i] <= 'F'))) return;
    name[n - 5] = 0;
}

static bool blank_line(const char *s) {
    for (; *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

/* classifyGroupedLines: a directory stack keyed by header depth, so a file header knows its full display path */
static void classify_rows(tc_grouped_t *g, const char *scope) {
    strvec_t dirs = {0}; /* per depth: the directory's display name with its trailing `/` */
    const char *header_lang = scope && !tc_is_url_like(scope) ? tc_lang_from_path(scope) : NULL;
    for (size_t i = 0; i < g->lines.n; i++) {
        const char *line = g->lines.v[i];
        tc_grow_t *r = &g->rows[i];
        size_t depth = strspn(line, "#");
        if (!depth || !isspace((unsigned char)line[depth])) {
            r->kind = TC_GROW_CONTENT;
            r->lang = g->lang ? g->lang : header_lang;
            continue;
        }
        const char *s = line + depth;
        while (isspace((unsigned char)*s)) s++;
        char *rest = tc_trim_end(s);
        bool url = tc_is_url_like(rest);
        while (dirs.n > depth - 1) free(dirs.v[--dirs.n]);
        while (dirs.n < depth - 1) strvec_push(&dirs, ""); /* a skipped level */
        size_t rl = strlen(rest);
        if (!url && rl && rest[rl - 1] == '/') {
            /* directory headers are always `name/`: a trailing `(…)` is part of the name, not a suffix */
            r->kind = TC_GROW_DIR;
            strvec_push(&dirs, rest);
            header_lang = NULL;
            free(rest);
            continue;
        }
        const char *suf = header_suffix(rest);
        char *target = suf ? xstrndup(rest, (size_t)(suf - rest)) : xstrdup(rest);
        char *name = xstrdup(target);
        strip_hashline_tag(name);
        r->kind = TC_GROW_FILE;
        r->url = url;
        if (suf) r->suffix = str_trim(suf);
        if (url) r->display_path = xstrdup(name);
        else {
            char *chain = strvec_join(&dirs, "");
            r->display_path = xasprintf("%s%s", chain, name);
            free(chain);
        }
        header_lang = tc_lang_from_path(r->display_path);
        free(name);
        free(target);
        free(rest);
    }
    strvec_free(&dirs);
}

/* groupLineIndicesByBlank + groupsByFile: `# dir/` rows dropped, every file row starts a group of its own;
 * rows before a group's first file row (notes, a single-file scope's matches) keep their group */
static void group_rows(tc_grouped_t *g) {
    bool has_sep = false;
    for (size_t i = 0; i < g->lines.n; i++) has_sep |= blank_line(g->lines.v[i]);
    VEC(tc_ggroup_t) out = {0};
    VEC(size_t) cur = {0};
#define FLUSH_GROUP()                                                                  \
    do {                                                                               \
        if (cur.n) {                                                                   \
            tc_ggroup_t ng = {xmalloc(cur.n * sizeof(size_t)), cur.n};                 \
            memcpy(ng.v, cur.v, cur.n * sizeof(size_t));                               \
            vec_push(&out, ng);                                                        \
            cur.n = 0;                                                                 \
        }                                                                              \
    } while (0)
    for (size_t i = 0; i < g->lines.n; i++) {
        if (blank_line(g->lines.v[i])) {
            if (has_sep) FLUSH_GROUP();
            continue;
        }
        if (g->rows[i].kind == TC_GROW_DIR) continue;
        if (g->rows[i].kind == TC_GROW_FILE) FLUSH_GROUP();
        vec_push(&cur, i);
    }
    FLUSH_GROUP();
#undef FLUSH_GROUP
    vec_free(&cur);
    g->groups = out.v;
    g->ngroups = out.n;
}

void tc_grouped_parse(tc_grouped_t *g, const char *text, const char *scope, const char *lang) {
    memset(g, 0, sizeof *g);
    g->lines = str_split(text ? text : "", "\n");
    g->rows = xcalloc(g->lines.n + 1, sizeof *g->rows);
    g->frames = xcalloc(g->lines.n + 1, sizeof *g->frames);
    /* the override only counts when the highlighter knows it */
    g->lang = lang && tui_highlight_supported(lang) ? lang : NULL;
    classify_rows(g, scope);
    group_rows(g);
}

void tc_grouped_free(tc_grouped_t *g) {
    for (size_t i = 0; i < g->lines.n; i++) {
        free(g->rows[i].display_path);
        free(g->rows[i].suffix);
        free(g->frames[i]);
    }
    for (size_t i = 0; i < g->ngroups; i++) free(g->groups[i].v);
    free(g->groups);
    free(g->rows);
    free(g->frames);
    strvec_free(&g->lines);
}

/* Source rows highlighted together: a longer run highlights chunk by chunk as rows are requested */
#define CODE_FRAME_CHUNK_ROWS 64

static bool frame_continues(const tc_grouped_t *g, size_t i) {
    if (!i || g->rows[i].kind != TC_GROW_CONTENT || g->rows[i - 1].kind != TC_GROW_CONTENT) return false;
    const char *a = g->rows[i].lang, *b = g->rows[i - 1].lang;
    if (a != b && !(a && b && !strcmp(a, b))) return false;
    long n = 0, m = 0;
    if (!frame_gutter(g->lines.v[i], &n) || !frame_gutter(g->lines.v[i - 1], &m)) return false;
    return n == m + 1;
}

char *tc_grouped_code_frame(tc_grouped_t *g, size_t index) {
    if (g->rows[index].kind != TC_GROW_CONTENT || !frame_gutter(g->lines.v[index], NULL)) return NULL;
    if (g->frames[index]) return xstrdup(g->frames[index]);
    size_t run = index;
    while (frame_continues(g, run)) run--;
    size_t start = run + (index - run) / CODE_FRAME_CHUNK_ROWS * CODE_FRAME_CHUNK_ROWS;
    size_t end = index + 1;
    while (end < start + CODE_FRAME_CHUNK_ROWS && end < g->lines.n && frame_continues(g, end)) end++;
    /* each gutter in toolOutput, the sources highlighted as one run: a multi-line string or comment keeps its colors */
    const char *lang = g->rows[index].lang;
    /* the native highlighter knows Markdown: its plain rows stay uncolored rather than taking the frame's color */
    bool plain = lang && !strcmp(lang, "markdown");
    bool known = lang && tui_highlight_supported(lang);
    strvec_t src = {0}, hl = {0};
    for (size_t r = start; r < end; r++) {
        char *t = tc_replace_tabs(g->lines.v[r] + frame_gutter(g->lines.v[r], NULL));
        strvec_push_own(&src, t);
    }
    if (known) {
        char *joined = strvec_join(&src, "\n");
        tui_highlight_code(joined, lang, NULL, &hl);
        free(joined);
    }
    for (size_t r = start; r < end; r++) {
        size_t k = r - start;
        char *gut = xstrndup(g->lines.v[r], frame_gutter(g->lines.v[r], NULL));
        char *gs = tc_fg(TUI_TC_TOOL_OUTPUT, gut);
        char *ss = plain ? xstrdup(src.v[k]) : known && k < hl.n ? xstrdup(hl.v[k]) : tc_fg(TUI_TC_TOOL_OUTPUT, src.v[k]);
        free(g->frames[r]);
        g->frames[r] = xasprintf("%s%s", gs, ss);
        free(ss);
        free(gs);
        free(gut);
    }
    strvec_free(&hl);
    strvec_free(&src);
    return xstrdup(g->frames[index]);
}

char *tc_grouped_file_row(const tc_grow_t *row) {
    char *name = tc_replace_tabs(row->display_path ? row->display_path : "");
    char *label = row->url ? tc_fg(TC_FILE_NAME_COLOR, name) : tc_styled_path(name);
    char *r;
    if (row->suffix) {
        char *st = tc_replace_tabs(row->suffix);
        char *sd = tc_fg(TUI_TC_DIM, st);
        r = xasprintf("%s %s", label, sd);
        free(sd);
        free(st);
    } else r = xstrdup(label);
    free(label);
    free(name);
    return r;
}

void tc_grouped_fit(const tc_grouped_t *g, const tc_ggroup_t *groups, size_t ngroups, int budget, tc_gunit_fn is_unit, void *ud,
                    const char *unit, int64_t total, int gap, tc_ggroup_t **shown, size_t *nshown, char **summary) {
    tc_ggroup_t *sv = xcalloc(ngroups + 1, sizeof *sv);
    *shown = sv;
    *nshown = 0;
    *summary = NULL;
    int64_t total_lines = (int64_t)(ngroups > 1 ? ngroups - 1 : 0) * gap;
    for (size_t i = 0; i < ngroups; i++) total_lines += (int64_t)groups[i].n;
    if (budget < 0 || total_lines <= budget) {
        for (size_t i = 0; i < ngroups; i++) sv[i] = groups[i];
        *nshown = ngroups;
        return;
    }
    if (budget == 0) return;
    int64_t room = budget - 1, used = 0, shown_units = 0;
    for (size_t i = 0; i < ngroups; i++) {
        int64_t lead = *nshown > 0 ? gap : 0;
        int64_t take = MIN((int64_t)groups[i].n, room - used - lead);
        if (take <= 0 || (take < MIN((int64_t)groups[i].n, 2) && *nshown > 0)) break;
        sv[*nshown].v = groups[i].v;
        sv[*nshown].n = (size_t)take;
        (*nshown)++;
        used += lead + take;
        for (int64_t k = 0; k < take; k++)
            if (is_unit(g, groups[i].v[k], ud)) shown_units++;
        if (take != (int64_t)groups[i].n) break;
    }
    int64_t all_units = total;
    if (!all_units)
        for (size_t i = 0; i < ngroups; i++)
            for (size_t k = 0; k < groups[i].n; k++)
                if (is_unit(g, groups[i].v[k], ud)) all_units++;
    int64_t hidden_units = all_units - shown_units;
    if (hidden_units > 0) {
        *summary = tc_more_items(hidden_units, unit);
        return;
    }
    int64_t hidden_lines = total_lines - used;
    if (hidden_lines > 0) *summary = tc_more_items(hidden_lines, "line");
}

void tc_grouped_tree(tc_grouped_t *g, const tc_ggroup_t *shown, size_t nshown, const char *summary, tc_gstyle_fn style, void *ud, strvec_t *out) {
    for (size_t i = 0; i < nshown; i++) {
        bool last = !summary && i + 1 == nshown;
        char *conn = tc_fg(TUI_TC_DIM, tc_sym(last ? "tree.last" : "tree.branch"));
        char *cp = last ? xstrdup("   ") : xasprintf("%s  ", tc_sym("tree.vertical"));
        char *cont = tc_fg(TUI_TC_DIM, cp);
        for (size_t k = 0; k < shown[i].n; k++) {
            char *st = style(g, shown[i].v[k], ud);
            char *rt = tc_replace_tabs(st);
            if (k == 0) strvec_push_own(out, xasprintf("%s %s", conn, rt));
            else strvec_push_own(out, xasprintf("%s%s", cont, rt));
            free(rt);
            free(st);
        }
        free(cont);
        free(cp);
        free(conn);
    }
    if (summary) {
        char *conn = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
        char *m = tc_fg(TUI_TC_MUTED, summary);
        strvec_push_own(out, xasprintf("%s %s", conn, m));
        free(m);
        free(conn);
    }
}

/* ---------- grep ---------- */

typedef struct {
    bool marked;
} grep_ctx_t;

static bool grep_is_match(const tc_grouped_t *g, size_t i, void *ud) {
    const grep_ctx_t *x = ud;
    if (x->marked) return tc_grouped_is_match(g->lines.v[i]);
    return g->rows[i].kind == TC_GROW_CONTENT && *g->lines.v[i];
}

static char *grep_style(tc_grouped_t *g, size_t i, void *ud) {
    UNUSED(ud);
    if (g->rows[i].kind == TC_GROW_FILE) return tc_grouped_file_row(&g->rows[i]);
    char *f = tc_grouped_code_frame(g, i);
    return f ? f : tc_fg(TUI_TC_TOOL_OUTPUT, g->lines.v[i]);
}

/* grepSubject: a result names the tool's display scope (linked to the original search path); a call falls back
 * to the requested paths */
static char *grep_subject(const char *pattern, const json_t *args, const json_t *d) {
    const char *scope = json_get_str(d, "scopePath");
    const char *sp = json_get_str(d, "searchPath");
    char *sc = NULL;
    if (scope && *scope) sc = sp ? tc_file_hyperlink(sp, scope) : xstrdup(scope);
    else sc = tc_paths_of(args);
    char *r = tc_scoped_subject(pattern && *pattern ? pattern : NULL, sc);
    free(sc);
    return r;
}

void tc_grep(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    const char *pattern = json_get_str(c->args, "pattern");
    if (!c->has_result || c->partial) {
        strvec_t meta = {0};
        char *subj = grep_subject(pattern && *pattern ? pattern : "?", c->args, NULL);
        if (json_get(c->args, "case") && !json_get_bool(c->args, "case", true)) strvec_push(&meta, "case:insensitive");
        if (json_get(c->args, "gitignore") && !json_get_bool(c->args, "gitignore", true)) strvec_push(&meta, "gitignore:false");
        if (json_get_int(c->args, "skip", 0) > 0) meta_push(&meta, xasprintf("skip:%lld", (long long)json_get_int(c->args, "skip", 0)));
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "Grep", .title_slot = TUI_TC_TOOL_TITLE,
                               .description = subj, .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        free(subj);
        strvec_free(&meta);
        return;
    }
    char *subject = grep_subject(pattern, c->args, d);
    if (c->is_error || json_get_str(d, "error")) {
        char *t = json_get_str(d, "error") ? xstrdup(json_get_str(d, "error")) : tc_text_of(c->content);
        tc_status_line_t sl = {.icon = TC_ST_ERROR, .spinner = -1, .title = "Grep", .title_slot = TUI_TC_TOOL_TITLE, .description = subject};
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
    bool detailed = json_get(d, "matchCount") || json_get(d, "fileCount");
    const char *dc = json_get_str(d, "displayContent");
    char *text = dc ? xstrdup(dc) : tc_text_of(c->content);
    int64_t matches = json_get_int(d, "matchCount", 0), files = json_get_int(d, "fileCount", 0);
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
                                   .description = subject, .meta = m, .nmeta = 1};
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
        free(subject);
        return;
    }
    if (matches == 0) {
        strvec_t meta = {0};
        strvec_push(&meta, "0 matches");
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "Grep", .title_slot = TUI_TC_TOOL_TITLE,
                               .description = subject, .meta = (const char *const *)meta.v, .nmeta = meta.n};
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
        free(subject);
        return;
    }
    const json_t *mt = json_get_obj(d, "meta");
    bool truncated = json_get_bool(d, "truncated", false) || json_get_obj(d, "truncation") || json_get_obj(mt, "truncation") ||
                     json_get_bool(json_get_obj(mt, "limits"), "columnTruncated", false);
    strvec_t meta = {0};
    meta_push(&meta, tc_format_count("match", matches));
    meta_push(&meta, tc_format_count("file", files));
    if (truncated) meta_push(&meta, tc_fg(TUI_TC_WARNING, "truncated"));
    char *ic = truncated ? NULL : search_icon();
    tc_status_line_t sl = {.icon = truncated ? TC_ST_WARNING : TC_ST_NONE, .icon_override = ic, .spinner = -1, .title = "Grep",
                           .title_slot = TUI_TC_TOOL_TITLE, .description = subject, .meta = (const char *const *)meta.v,
                           .nmeta = meta.n};
    char *header = tc_status_line(&sl);

    /* classify the whole output so nested directory headers resolve across blank-line groups; a single-file
     * search's headerless lines belong to the searched file */
    tc_grouped_t g;
    tc_grouped_parse(&g, text, json_get_str(d, "searchPath"), NULL);
    grep_ctx_t x = {false};
    for (size_t i = 0; i < g.lines.n; i++) x.marked |= tc_grouped_is_match(g.lines.v[i]);
    /* collapsed previews drop context rows, keeping each file row and its matches */
    tc_ggroup_t *compact = xcalloc(g.ngroups + 1, sizeof *compact);
    for (size_t i = 0; i < g.ngroups; i++) {
        compact[i].v = xcalloc(g.groups[i].n + 1, sizeof(size_t));
        for (size_t k = 0; k < g.groups[i].n; k++) {
            size_t li = g.groups[i].v[k];
            if (!x.marked || g.rows[li].kind == TC_GROW_FILE || grep_is_match(&g, li, &x)) compact[i].v[compact[i].n++] = li;
        }
    }
    int extra = missing_note ? 1 : 0;
    tc_ggroup_t *shown;
    size_t nshown;
    char *summary;
    tc_grouped_fit(&g, c->expanded ? g.groups : compact, g.ngroups, MAX((c->expanded ? 24 : 6) - extra, 0), grep_is_match, &x, "match",
                   matches, 0, &shown, &nshown, &summary);
    strvec_t rows = {0};
    tc_grouped_tree(&g, shown, nshown, summary, grep_style, NULL, &rows);
    if (missing_note) strvec_push(&rows, missing_note);
    tc_fit_headered(header, &rows, width, out);
    strvec_free(&rows);
    free(summary);
    free(shown);
    for (size_t i = 0; i < g.ngroups; i++) free(compact[i].v);
    free(compact);
    tc_grouped_free(&g);
    free(header);
    free(ic);
    strvec_free(&meta);
    free(missing_note);
    strvec_free(&missing);
    free(text);
    free(subject);
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
                if (strspn(all.v[i], " \t") != strlen(all.v[i])) strvec_push_own(&items, tc_styled_path(all.v[i]));
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
            /* type icons stay muted beside the dim directory and colored file name */
            char *icon = tc_fg(TUI_TC_MUTED, dir ? tc_sym("icon.folder") : tc_lang_icon(tc_lang_from_path(f)));
            char *ps = tc_styled_path(f);
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
