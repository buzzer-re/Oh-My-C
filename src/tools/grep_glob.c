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
                if (hashline) {
                    size_t flen;
                    char *content = fs_read_limit(fabs, 4u << 20, &flen, NULL);
                    const char *tag = content ? edit_store_record(&env->store, fabs, content, h->line, h->line) : "????";
                    sb_appendf(&b, "[%s#%s]\n", disp, tag);
                    free(content);
                } else {
                    sb_appendf(&b, "%s\n", disp);
                }
                json_push_str(files_j, disp);
                free(disp);
                free(fabs);
            }
            if (skipping) continue;
            if (!h->is_context) matches++;
            if (h->truncated) any_trunc = true;
            const char *text = h->text ? h->text : "";
            sb_appendf(&b, "%c%u%c%s\n", h->is_context ? ' ' : '*', h->line, hashline ? ':' : '|', text);
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
    out->details = details;
    details = NULL;
done:
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
    json_set_bool(ctx, "hasFind", false);
    char *desc = tool_prompt("grep", ctx);
    json_free(ctx);
    json_t *s = schema_obj();
    schema_prop(s, "pattern", "string", "Regex pattern (Rust regex syntax)", true);
    schema_prop(s, "path", "string", "File, directory or glob; ';'-separated list; '!glob' excludes", false);
    schema_prop(s, "case", "boolean", "Case-sensitive (default true)", false);
    schema_prop(s, "gitignore", "boolean", "Respect .gitignore (default true)", false);
    schema_prop(s, "skip", "number", "File-page offset", false);
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
    sb_free(&b);
    for (size_t i = 0; i < items.n; i++) free(items.v[i].path);
    vec_free(&items);
    strvec_free(&inc);
    strvec_free(&exc);
    strvec_free(&missing);
    return 0;
}

agent_tool_t *tool_glob_new(tool_env_t *env) {
    char *desc = tool_prompt("glob", NULL);
    json_t *s = schema_obj();
    schema_prop(s, "path", "string", "Glob, file or directory; ';'-separated list; '!glob' excludes", false);
    schema_prop(s, "hidden", "boolean", "Include hidden files (default true)", false);
    schema_prop(s, "gitignore", "boolean", "Respect .gitignore (default true)", false);
    schema_prop(s, "limit", "number", "Max results (1-200, default 200)", false);
    agent_tool_t *t = agent_tool_new("glob", "Glob", desc, s, glob_execute);
    free(desc);
    t->ud = env;
    return t;
}
