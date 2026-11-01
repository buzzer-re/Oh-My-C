/* grep + glob tools on top of the text engines (regex, gitignore-aware walker, search). */
#include "tools.h"

#include "config/settings.h"
#include "core/fs.h"
#include "text/glob.h"
#include "text/search.h"
#include "text/walker.h"

#include <sys/stat.h>

#define GREP_FILES_PER_PAGE 20
#define GREP_PER_FILE_MAX 50
#define GREP_LINE_MAX 512
#define GREP_OUTPUT_MAX (50u * 1024)

/* split "a; b; !c" into includes/excludes */
static void split_paths(const char *spec, strvec_t *inc, strvec_t *exc) {
    strvec_t parts = str_split(spec ? spec : ".", ";");
    for (size_t i = 0; i < parts.n; i++) {
        char *t = str_trim(parts.v[i]);
        if (!*t) {
            free(t);
            continue;
        }
        if (t[0] == '!') {
            strvec_push(exc, t + 1);
            free(t);
        } else {
            strvec_push_own(inc, t);
        }
    }
    if (!inc->n) strvec_push(inc, ".");
    strvec_free(&parts);
}

/* split a path into its literal base dir and the glob remainder (from the first magic segment on) */
static void split_glob(const char *p, char **base, char **glob) {
    if (!glob_has_magic(p)) {
        *base = xstrdup(p);
        *glob = NULL;
        return;
    }
    strvec_t segs = str_split(p, "/");
    sb_t b = SB_INIT;
    size_t i = 0;
    for (; i < segs.n; i++) {
        if (glob_has_magic(segs.v[i])) break;
        if (b.len || p[0] == '/') sb_putc(&b, '/');
        sb_append(&b, segs.v[i]);
    }
    sb_t g = SB_INIT;
    for (size_t k = i; k < segs.n; k++) {
        if (g.len) sb_putc(&g, '/');
        sb_append(&g, segs.v[k]);
    }
    *base = b.len ? sb_detach(&b) : (sb_free(&b), xstrdup("."));
    *glob = sb_detach(&g);
    strvec_free(&segs);
}

/* ---------- display content (omp grouped-file-output: `# dir/` / `## file#TAG` path tree, `*N│text` rows) ---------- */

typedef struct {
    uint32_t line;
    bool match;
    char mark; /* gutter marker overriding match (`-`/`+` for ast_edit), 0 = none */
    char *text;
} dline_t;
typedef struct {
    char *path; /* cwd-relative display path */
    char *tag;  /* hashline tag or NULL */
    char *suffix; /* header suffix (` (2 replacements)`) or NULL */
    VEC(dline_t) lines;
    bool no_gaps; /* no `│...` rows between non-adjacent lines */
} dfile_t;
typedef VEC(dfile_t) dfiles_t;

static void dfile_rows(const dfile_t *f, strvec_t *out) {
    int w = 0;
    for (size_t i = 0; i < f->lines.n; i++) {
        char nb[16];
        w = MAX(w, snprintf(nb, sizeof nb, "%u", f->lines.v[i].line));
    }
    uint32_t last = 0;
    for (size_t i = 0; i < f->lines.n; i++) {
        const dline_t *l = &f->lines.v[i];
        if (l->line == 0) { /* verbatim row (ast_grep `  meta: ...`) */
            strvec_push(out, l->text);
            continue;
        }
        if (last && l->line > last + 1 && !f->no_gaps) strvec_push_own(out, xasprintf("%*s\xe2\x94\x82...", w + 1, ""));
        char *g = l->mark ? xasprintf("%c%u", l->mark, l->line) : xasprintf("%s%u", l->match ? "*" : "", l->line);
        strvec_push_own(out, xasprintf("%*s\xe2\x94\x82%s", w + 1, g, l->text));
        free(g);
        last = l->line;
    }
}

typedef struct pnode pnode_t;
struct pnode {
    strvec_t dir_names;
    VEC(pnode_t *) dirs;
    VEC(size_t) files; /* indices into dfiles */
};

static pnode_t *pnode_child(pnode_t *n, const char *name) {
    for (size_t i = 0; i < n->dirs.n; i++)
        if (!strcmp(n->dir_names.v[i], name)) return n->dirs.v[i];
    pnode_t *c = xcalloc(1, sizeof *c);
    strvec_push(&n->dir_names, name);
    vec_push(&n->dirs, c);
    return c;
}

static void pnode_free(pnode_t *n) {
    for (size_t i = 0; i < n->dirs.n; i++) pnode_free(n->dirs.v[i]);
    strvec_free(&n->dir_names);
    vec_free(&n->dirs);
    vec_free(&n->files);
    free(n);
}

static void pnode_walk(const pnode_t *n, int depth, const dfiles_t *fs, bool *emitted, strvec_t *out) {
    for (size_t i = 0; i < n->files.n; i++) {
        const dfile_t *f = &fs->v[n->files.v[i]];
        if (*emitted && depth == 0) strvec_push(out, "");
        *emitted = true;
        const char *slash = strrchr(f->path, '/');
        sb_t h = SB_INIT;
        for (int k = 0; k <= depth; k++) sb_putc(&h, '#');
        sb_appendf(&h, " %s", slash ? slash + 1 : f->path);
        if (f->tag) sb_appendf(&h, "#%s", f->tag);
        if (f->suffix) sb_append(&h, f->suffix);
        strvec_push_own(out, sb_detach(&h));
        dfile_rows(f, out);
    }
    for (size_t i = 0; i < n->dirs.n; i++) {
        const pnode_t *d = n->dirs.v[i];
        sb_t name = SB_INIT;
        sb_append(&name, n->dir_names.v[i]);
        while (d->files.n == 0 && d->dirs.n == 1) {
            sb_appendf(&name, "/%s", d->dir_names.v[0]);
            d = d->dirs.v[0];
        }
        if (*emitted) strvec_push(out, "");
        *emitted = true;
        sb_t h = SB_INIT;
        for (int k = 0; k <= depth; k++) sb_putc(&h, '#');
        sb_appendf(&h, " %s/", sb_cstr(&name));
        strvec_push_own(out, sb_detach(&h));
        sb_free(&name);
        pnode_walk(d, depth + 1, fs, emitted, out);
    }
}

static char *grep_display(const dfiles_t *fs, bool grouped) {
    strvec_t out = {0};
    if (!grouped) {
        for (size_t i = 0; i < fs->n; i++) {
            if (i) strvec_push(&out, "");
            dfile_rows(&fs->v[i], &out);
        }
    } else {
        pnode_t *root = xcalloc(1, sizeof *root);
        for (size_t i = 0; i < fs->n; i++) {
            strvec_t segs = str_split(fs->v[i].path, "/");
            pnode_t *n = root;
            for (size_t k = 0; k + 1 < segs.n; k++)
                if (*segs.v[k]) n = pnode_child(n, segs.v[k]);
            vec_push(&n->files, i);
            strvec_free(&segs);
        }
        bool emitted = false;
        pnode_walk(root, 0, fs, &emitted, &out);
        pnode_free(root);
    }
    char *r = strvec_join(&out, "\n");
    strvec_free(&out);
    return r;
}

char *grep_display_content(const json_t *files, bool grouped) {
    dfiles_t fs = {0};
    json_t *f;
    json_arr_foreach(files, i, f) {
        dfile_t df = {.path = xstrdup(json_get_str(f, "path") ? json_get_str(f, "path") : ""), .no_gaps = true};
        if (json_get_str(f, "tag")) df.tag = xstrdup(json_get_str(f, "tag"));
        if (json_get_str(f, "suffix")) df.suffix = xstrdup(json_get_str(f, "suffix"));
        json_t *l;
        json_arr_foreach(json_get_arr(f, "lines"), k, l) {
            const char *mk = json_get_str(l, "mark");
            dline_t dl = {.line = (uint32_t)json_get_int(l, "line", 0), .match = json_get_bool(l, "match", false),
                          .mark = mk && *mk ? mk[0] : 0, .text = xstrdup(json_get_str(l, "text") ? json_get_str(l, "text") : "")};
            vec_push(&df.lines, dl);
        }
        vec_push(&fs, df);
    }
    char *r = grep_display(&fs, grouped);
    for (size_t k = 0; k < fs.n; k++) {
        for (size_t m = 0; m < fs.v[k].lines.n; m++) free(fs.v[k].lines.v[m].text);
        vec_free(&fs.v[k].lines);
        free(fs.v[k].path);
        free(fs.v[k].tag);
        free(fs.v[k].suffix);
    }
    vec_free(&fs);
    return r;
}

static char *scope_display(const char *spec, const char *cwd, tool_env_t *env) {
    char *abs = tool_resolve_path(env, spec);
    char *r = !strcmp(abs, cwd) ? xstrdup(".") : path_display(abs, cwd);
    free(abs);
    return r;
}

/* ---------- grep ---------- */

static int grep_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(ctx);
    const char *pattern = json_get_str(args, "pattern");
    if (!pattern || strspn(pattern, " \t\r\n") == strlen(pattern)) {
        tool_result_error(out, "pattern must not be empty");
        return 0;
    }
    bool case_sensitive = json_get_bool(args, "case", true);
    bool gitignore = json_get_bool(args, "gitignore", true);
    int64_t skip = json_get_int(args, "skip", 0);
    if (skip < 0) {
        tool_result_error(out, "skip must be >= 0");
        return 0;
    }
    strvec_t inc = {0}, exc = {0};
    split_paths(json_get_str(args, "path"), &inc, &exc);
    bool hashline = str_eq(env->edit_mode, "hashline");
    sb_t b = SB_INIT;
    json_t *details = json_obj();
    json_t *files_j = json_arr();
    size_t files_seen = 0, files_shown = 0, matches = 0;
    bool more = false, any_trunc = false;
    strvec_t missing = {0};
    dfiles_t dfiles = {0};
    bool grouped = inc.n > 1;
    for (size_t pi = 0; pi < inc.n && !more; pi++) {
        char *spec = xstrdup(inc.v[pi]);
        /* file:line-range selector */
        int64_t lo = 0, hi = 0;
        char *colon = strrchr(spec, ':');
        if (colon && colon[1] && strspn(colon + 1, "0123456789-") == strlen(colon + 1)) {
            char *probe = xstrndup(spec, (size_t)(colon - spec));
            char *pabs = tool_resolve_path(env, probe);
            if (fs_is_file(pabs)) {
                lo = strtoll(colon + 1, NULL, 10);
                const char *dash = strchr(colon + 1, '-');
                hi = dash ? strtoll(dash + 1, NULL, 10) : lo;
                *colon = 0;
            }
            free(pabs);
            free(probe);
        }
        char *base, *glob;
        split_glob(spec, &base, &glob);
        char *abs = tool_resolve_path(env, base);
        if (!fs_exists(abs)) {
            strvec_push(&missing, spec);
            free(abs);
            free(base);
            free(glob);
            free(spec);
            continue;
        }
        search_opts_t so = {0};
        so.pattern = pattern;
        so.path = abs;
        so.glob = glob;
        so.exclude = (const char *const *)exc.v;
        so.nexclude = exc.n;
        so.ignore_case = !case_sensitive;
        so.hidden = true;
        so.no_ignore = !gitignore;
        so.max_count_per_file = GREP_PER_FILE_MAX;
        so.max_columns = GREP_LINE_MAX;
        so.multiline = strstr(pattern, "\\n") != NULL;
        search_result_t sr;
        if (search_run(&so, &sr) != 0) {
            tool_result_error(out, "%s", sr.error ? sr.error : "search failed");
            search_result_free(&sr);
            free(abs);
            free(base);
            free(glob);
            free(spec);
            goto done;
        }
        bool is_file = fs_is_file(abs);
        if (!is_file) grouped = true;
        const char *cur = NULL;
        bool skipping = false;
        for (size_t i = 0; i < sr.hits.n; i++) {
            search_hit_t *h = &sr.hits.v[i];
            if (lo && (h->line < (uint32_t)lo || h->line > (uint32_t)hi)) continue;
            if (!cur || strcmp(cur, h->path) != 0) {
                cur = h->path;
                files_seen++;
                skipping = (int64_t)files_seen <= skip;
                if (!skipping && files_shown >= GREP_FILES_PER_PAGE) {
                    more = true;
                    break;
                }
                if (skipping) continue;
                files_shown++;
                char *fabs = is_file ? xstrdup(abs) : path_join(abs, h->path);
                char *disp = path_display(fabs, env->cwd);
                if (b.len) sb_putc(&b, '\n');
                dfile_t df = {.path = xstrdup(disp)};
                if (hashline) {
                    size_t flen;
                    char *content = fs_read_limit(fabs, 4u << 20, &flen, NULL);
                    const char *tag = content ? edit_store_record(&env->store, fabs, content, h->line, h->line) : "????";
                    sb_appendf(&b, "[%s#%s]\n", disp, tag);
                    if (content) df.tag = xstrdup(tag);
                    free(content);
                } else {
                    sb_appendf(&b, "%s\n", disp);
                }
                json_push_str(files_j, disp);
                vec_push(&dfiles, df);
                free(disp);
                free(fabs);
            }
            if (skipping) continue;
            if (!h->is_context) matches++;
            if (h->truncated) any_trunc = true;
            const char *text = h->text ? h->text : "";
            sb_appendf(&b, "%c%u%c%s\n", h->is_context ? ' ' : '*', h->line, hashline ? ':' : '|', text);
            if (dfiles.n) {
                dline_t dl = {.line = h->line, .match = !h->is_context, .text = xstrdup(text)};
                vec_push(&dfiles.v[dfiles.n - 1].lines, dl);
            }
            if (hashline) {
                /* widen seen range for this snapshot */
                char *fabs = is_file ? xstrdup(abs) : path_join(abs, h->path);
                snapshot_t *sn = edit_store_find(&env->store, fabs, NULL);
                if (sn) {
                    line_span_t r = {h->line, h->line};
                    pthread_mutex_lock(&env->store.mu);
                    if (!(sn->seen.n == 1 && sn->seen.v[0].hi == INT64_MAX)) vec_push(&sn->seen, r);
                    pthread_mutex_unlock(&env->store.mu);
                }
                free(fabs);
            }
            if (b.len > GREP_OUTPUT_MAX) {
                more = true;
                break;
            }
        }
        if (sr.limit_reached) any_trunc = true;
        search_result_free(&sr);
        free(abs);
        free(base);
        free(glob);
        free(spec);
    }
    if (!files_shown) {
        if (skip && files_seen) sb_appendf(&b, "No more results (skip %lld past %zu matching files)", (long long)skip, files_seen);
        else sb_append(&b, "No matches found");
        out->useless = true;
    } else if (more) {
        sb_appendf(&b, "\n[More matching files. Use skip: %lld to see the next page]", (long long)(skip + (int64_t)files_shown));
    }
    if (missing.n) {
        char *m = strvec_join(&missing, ", ");
        sb_appendf(&b, "\nSkipped missing paths: %s", m);
        free(m);
    }
    if (b.len && b.s[b.len - 1] == '\n') sb_truncate(&b, b.len - 1);
    tool_result_text(out, sb_cstr(&b));
    json_set_int(details, "matchCount", (int64_t)matches);
    json_set_int(details, "fileCount", (int64_t)files_shown);
    json_set(details, "files", files_j);
    files_j = NULL;
    json_set_bool(details, "fileLimitReached", more);
    json_set_bool(details, "truncated", more || any_trunc);
    if (files_shown) {
        char *dc = grep_display(&dfiles, grouped);
        json_set_str(details, "displayContent", dc);
        free(dc);
    }
    {
        strvec_t sc = {0};
        for (size_t k = 0; k < inc.n; k++) strvec_push_own(&sc, scope_display(inc.v[k], env->cwd, env));
        char *j = strvec_join(&sc, ", ");
        json_set_str(details, "scopePath", j);
        free(j);
        strvec_free(&sc);
    }
    if (missing.n) {
        json_t *mp = json_arr();
        for (size_t k = 0; k < missing.n; k++) json_push_str(mp, missing.v[k]);
        json_set(details, "missingPaths", mp);
    }
    out->details = details;
    details = NULL;
done:
    for (size_t k = 0; k < dfiles.n; k++) {
        for (size_t m = 0; m < dfiles.v[k].lines.n; m++) free(dfiles.v[k].lines.v[m].text);
        vec_free(&dfiles.v[k].lines);
        free(dfiles.v[k].path);
        free(dfiles.v[k].tag);
    }
    vec_free(&dfiles);
    json_free(details);
    json_free(files_j);
    sb_free(&b);
    strvec_free(&inc);
    strvec_free(&exc);
    strvec_free(&missing);
    return 0;
}

agent_tool_t *tool_grep_new(tool_env_t *env) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "IS_HL_MODE", str_eq(env->edit_mode, "hashline"));
    json_set_bool(ctx, "hasFind", env->find_enabled);
    json_set_bool(ctx, "eagerDelegation", tools_eager_delegation(env));
    json_set_bool(ctx, "scoutAvailable", tools_scout_available());
    char *desc = tool_prompt("grep", ctx);
    json_free(ctx);
    json_t *s = schema_lit("{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":"
                           "\"string\"},\"case\":{\"type\":\"boolean\"},\"gitignore\":{\"type\":\"boolean\"},\"skip\":{"
                           "\"type\":[\"number\",\"null\"]}},\"required\":[\"pattern\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("grep", "Grep", desc, s, grep_execute);
    free(desc);
    t->ud = env;
    return t;
}

/* ---------- glob ---------- */

typedef struct {
    char *path;
    int64_t mtime;
    bool dir;
} gitem_t;

static int cmp_gitem(const void *a, const void *b) {
    const gitem_t *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcmp(x->path, y->path);
}

static int glob_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(ctx);
    const char *pspec = json_get_str(args, "path");
    if (pspec && !*pspec) {
        tool_result_error(out, "path must not be empty");
        return 0;
    }
    bool hidden = json_get_bool(args, "hidden", true);
    bool gitignore = json_get_bool(args, "gitignore", true);
    int64_t req_limit = json_get_int(args, "limit", 200);
    int64_t limit = req_limit < 1 ? 1 : req_limit > 200 ? 200 : req_limit;
    strvec_t inc = {0}, exc = {0};
    split_paths(pspec, &inc, &exc);
    VEC(gitem_t) items = {0};
    strvec_t missing = {0};
    for (size_t pi = 0; pi < inc.n; pi++) {
        char *base, *glob;
        split_glob(inc.v[pi], &base, &glob);
        char *abs = tool_resolve_path(env, base);
        struct stat st;
        if (stat(abs, &st) != 0) {
            strvec_push(&missing, inc.v[pi]);
        } else if (S_ISREG(st.st_mode)) {
            gitem_t g = {path_display(abs, env->cwd), (int64_t)st.st_mtime * 1000, false};
            vec_push(&items, g);
        } else {
            globset_t gs = {0}, xs = {0};
            if (glob) {
                char *norm = glob_normalize(glob, !strchr(glob, '/'));
                globset_add(&gs, norm, 0);
                free(norm);
            }
            for (size_t k = 0; k < exc.n; k++) globset_add(&xs, exc.v[k], 0);
            walk_opts_t wo = {0};
            wo.include_hidden = hidden;
            wo.no_ignore = !gitignore;
            wo.skip_node_modules = gitignore;
            wo.types = WALK_FILE | (glob ? 0 : WALK_DIR);
            wo.include = glob ? &gs : NULL;
            wo.exclude = exc.n ? &xs : NULL;
            wo.want_stat = true;
            if (!glob) wo.max_depth = 0;
            walk_list_t wl;
            if (walk_collect(abs, &wo, &wl) == 0) {
                for (size_t k = 0; k < wl.n; k++) {
                    char *full = path_join(abs, wl.v[k].path);
                    char *disp = path_display(full, env->cwd);
                    gitem_t g = {wl.v[k].type == WALK_DIR ? xasprintf("%s/", disp) : xstrdup(disp), wl.v[k].mtime_ms,
                                 wl.v[k].type == WALK_DIR};
                    vec_push(&items, g);
                    free(disp);
                    free(full);
                }
                walk_list_free(&wl);
            }
            globset_free(&gs);
            globset_free(&xs);
        }
        free(abs);
        free(base);
        free(glob);
    }
    if (items.n > 1) qsort(items.v, items.n, sizeof(gitem_t), cmp_gitem);
    sb_t b = SB_INIT;
    json_t *files = json_arr();
    size_t shown = MIN(items.n, (size_t)limit);
    for (size_t i = 0; i < shown; i++) {
        sb_appendf(&b, "%s\n", items.v[i].path);
        json_push_str(files, items.v[i].path);
    }
    if (!items.n) {
        sb_append(&b, "No files found matching pattern");
        out->useless = true;
    } else {
        if (items.n > shown) sb_appendf(&b, "[%zu more results; narrow the pattern]\n", items.n - shown);
        if (req_limit > 200) sb_appendf(&b, "Requested limit %lld clamped to the max of 200\n", (long long)req_limit);
    }
    if (missing.n) {
        char *m = strvec_join(&missing, ", ");
        sb_appendf(&b, "%sSkipped missing paths: %s", b.len && b.s[b.len - 1] != '\n' ? "\n" : "", m);
        free(m);
    }
    while (b.len && b.s[b.len - 1] == '\n') sb_truncate(&b, b.len - 1);
    tool_result_text(out, sb_cstr(&b));
    out->details = json_obj();
    json_set_int(out->details, "fileCount", (int64_t)shown);
    json_set(out->details, "files", files);
    json_set_bool(out->details, "resultLimitReached", items.n > shown);
    {
        char *base, *glob;
        split_glob(inc.v[0], &base, &glob);
        char *sc = scope_display(base, env->cwd, env);
        json_set_str(out->details, "scopePath", sc);
        free(sc);
        free(base);
        free(glob);
    }
    sb_free(&b);
    for (size_t i = 0; i < items.n; i++) free(items.v[i].path);
    vec_free(&items);
    strvec_free(&inc);
    strvec_free(&exc);
    strvec_free(&missing);
    return 0;
}

agent_tool_t *tool_glob_new(tool_env_t *env) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "hasFind", env->find_enabled);
    json_set_bool(ctx, "eagerDelegation", tools_eager_delegation(env));
    json_set_bool(ctx, "scoutAvailable", tools_scout_available());
    char *desc = tool_prompt("glob", ctx);
    json_free(ctx);
    json_t *s = schema_lit("{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"hidden\":{\"type\":"
                           "\"boolean\"},\"gitignore\":{\"type\":\"boolean\"},\"limit\":{\"type\":\"number\"}},"
                           "\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("glob", "Glob", desc, s, glob_execute);
    free(desc);
    t->ud = env;
    return t;
}
