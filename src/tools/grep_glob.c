/* grep + glob tools on top of the text engines (regex, gitignore-aware walker, search). */
#include "tools.h"

#include "config/settings.h"
#include "core/fs.h"
#include "text/glob.h"
#include "text/search.h"
#include "text/walker.h"

#include "tools/outsink.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>


/* ---------- display content (omp grouped-file-output: `# dir/` / `## file#TAG` path tree, `*N│text` rows) ---------- */

typedef struct {
    uint32_t line;
    bool match;
    bool cut; /* grep: a column-cut row, not a complete source line */
    char mark; /* gutter marker overriding match (`-`/`+` for ast_edit), 0 = none */
    char *text;
} dline_t;
typedef struct {
    char *path; /* cwd-relative display path */
    char *tag;  /* hashline tag or NULL */
    char *suffix; /* header suffix (` (2 replacements)`) or NULL */
    VEC(dline_t) lines;
    bool no_gaps; /* no `│...` rows between non-adjacent lines */
    size_t model_end; /* model rendering: output index just past this file's rows */
} dfile_t;
typedef VEC(dfile_t) dfiles_t;

/* model rows (omp formatMatchLine): `*N:text` / ` N:text` (hashline) or `*N|text`, `...` between gaps */
static void dfile_model_rows(const dfile_t *f, bool hl, strvec_t *out) {
    uint32_t last = 0;
    for (size_t i = 0; i < f->lines.n; i++) {
        const dline_t *l = &f->lines.v[i];
        if (last && l->line > last + 1) strvec_push(out, "...");
        strvec_push_own(out, xasprintf("%c%u%c%s", l->match ? '*' : ' ', l->line, hl ? ':' : '|', l->text));
        last = l->line;
    }
}

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

/* model < 0: display rows; 0/1: model rows (plain / hashline) */
static void pnode_walk(const pnode_t *n, int depth, dfiles_t *fs, bool *emitted, strvec_t *out, int model) {
    for (size_t i = 0; i < n->files.n; i++) {
        dfile_t *f = &fs->v[n->files.v[i]];
        if (*emitted && depth == 0) strvec_push(out, "");
        *emitted = true;
        const char *slash = strrchr(f->path, '/');
        sb_t h = SB_INIT;
        for (int k = 0; k <= depth; k++) sb_putc(&h, '#');
        sb_appendf(&h, " %s", slash ? slash + 1 : f->path);
        if (f->tag) sb_appendf(&h, "#%s", f->tag);
        if (f->suffix) sb_append(&h, f->suffix);
        strvec_push_own(out, sb_detach(&h));
        if (model >= 0) dfile_model_rows(f, model > 0 && f->tag, out);
        else dfile_rows(f, out);
        f->model_end = out->n;
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
        pnode_walk(d, depth + 1, fs, emitted, out, model);
    }
}

/* omp formatFileMatches: grouped path tree or flat blank-separated files; model >= 0 renders model rows
 * (flat files then carry a `[path#TAG]` hashline header when tagged) and records each file's model_end */
static strvec_t grep_render(dfiles_t *fs, bool grouped, int model) {
    strvec_t out = {0};
    if (!grouped) {
        for (size_t i = 0; i < fs->n; i++) {
            if (out.n) strvec_push(&out, "");
            if (model >= 0) {
                if (fs->v[i].tag) strvec_push_own(&out, xasprintf("[%s#%s]", fs->v[i].path, fs->v[i].tag));
                dfile_model_rows(&fs->v[i], model > 0 && fs->v[i].tag, &out);
            } else {
                dfile_rows(&fs->v[i], &out);
            }
            fs->v[i].model_end = out.n;
        }
    } else {
        pnode_t *root = xcalloc(1, sizeof *root);
        for (size_t i = 0; i < fs->n; i++) {
            strvec_t segs = str_split(fs->v[i].path, "/");
            pnode_t *n = root;
            for (size_t k = 0; k + 1 < segs.n; k++)
                n = pnode_child(n, segs.v[k]);
            vec_push(&n->files, i);
            strvec_free(&segs);
        }
        bool emitted = false;
        pnode_walk(root, 0, fs, &emitted, &out, model);
        pnode_free(root);
    }
    return out;
}

static char *grep_display(dfiles_t *fs, bool grouped) {
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
        pnode_walk(root, 0, fs, &emitted, &out, -1);
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

/* ---------- omp path-utils subset: path lists, selectors, search scope ---------- */

/* normalizePathLikeInput: trim + strip one pair of outer double quotes */
static char *norm_input(const char *s) {
    char *t = str_trim(s ? s : "");
    size_t n = strlen(t);
    if (n > 1 && t[0] == '"' && t[n - 1] == '"') {
        memmove(t, t + 1, n - 2);
        t[n - 2] = 0;
    }
    return t;
}

static bool has_glob_chars(const char *s) { return strpbrk(s, "*?[{") != NULL; }

/* formatPathRelativeToCwd: cwd-relative when inside cwd ("." for cwd itself), else absolute */
static char *rel_cwd(const char *abs, const char *cwd, bool trailing_slash) {
    char *n = path_normalize(abs);
    char *r;
    size_t cl = strlen(cwd);
    if (!strcmp(n, cwd)) r = xstrdup(".");
    else if (!strncmp(n, cwd, cl) && (n[cl] == '/' || (cl == 1 && cwd[0] == '/'))) r = xstrdup(n + (cl == 1 ? 1 : cl + 1));
    else r = xstrdup(n);
    free(n);
    if (trailing_slash && strcmp(r, ".") != 0 && !str_ends(r, "/")) {
        char *t = xasprintf("%s/", r);
        free(r);
        r = t;
    }
    return r;
}

/* probeLiteralPathExists: 1 exists, 0 missing, -1 unknown */
static int probe_literal(tool_env_t *env, const char *p) {
    char *abs = tool_resolve_path(env, p);
    struct stat st;
    int r = lstat(abs, &st) == 0 ? 1 : (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) ? 0 : -1;
    free(abs);
    return r;
}

/* read's splitPathAndSel (tools/readsel.c) */
static void split_path_sel(const char *raw, char **path, char **sel) { read_split_path_sel(raw, path, sel); }

/* splitPathAndSelPreferringLiteral (POSIX: an existing or unprobeable literal keeps the whole path) */
static void split_path_sel_literal(tool_env_t *env, const char *raw, char **path, char **sel) {
    split_path_sel(raw, path, sel);
    if (!*sel) return;
    if (probe_literal(env, raw) != 0) {
        free(*path);
        free(*sel);
        *path = xstrdup(raw);
        *sel = NULL;
    }
}

typedef struct {
    int64_t lo, hi; /* hi 0 = open-ended */
} lrange_t;
typedef VEC(lrange_t) lranges_t;
typedef VEC(size_t) sizevec_t;

static int cmp_lrange(const void *a, const void *b) {
    const lrange_t *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* parseLineRanges(sel, "onward"): false when not a range list; *err set for invalid ranges */
static bool parse_line_ranges(const char *sel, lranges_t *out, char **err) {
    strvec_t ch = str_split(sel, ",");
    bool pin = ch.n > 1, ok = true;
    for (size_t i = 0; i < ch.n && ok; i++) {
        const char *s = ch.v[i];
        size_t k = 0;
        if (s[k] == 'L' || s[k] == 'l') k++;
        size_t d = k;
        while (isdigit((unsigned char)s[k])) k++;
        if (k == d) { ok = false; break; }
        int64_t a = strtoll(s + d, NULL, 10);
        char sep = 0;
        if (s[k] == '.' && s[k + 1] == '.') { sep = '-'; k += 2; }
        else if (s[k] == '-' || s[k] == '+') sep = s[k++];
        bool has_rhs = false;
        int64_t b = 0;
        if (sep) {
            if (s[k] == 'L' || s[k] == 'l') k++;
            size_t d2 = k;
            while (isdigit((unsigned char)s[k])) k++;
            if (k > d2) { has_rhs = true; b = strtoll(s + d2, NULL, 10); }
        }
        if (s[k]) { ok = false; break; }
        if (a < 1) {
            *err = xstrdup("Line selector 0 is invalid; lines are 1-indexed. Use :1.");
            ok = false;
            break;
        }
        lrange_t r = {a, 0};
        if (sep == '+') {
            if (!has_rhs || b < 1) {
                *err = xasprintf("Invalid range %lld+%lld: count must be >= 1.", (long long)a, (long long)(has_rhs ? b : 0));
                ok = false;
                break;
            }
            r.hi = a + b - 1;
        } else if (sep == '-') {
            if (has_rhs) {
                if (b < a) {
                    *err = xasprintf("Invalid range %lld-%lld: end must be >= start.", (long long)a, (long long)b);
                    ok = false;
                    break;
                }
                r.hi = b;
            }
        } else if (pin) {
            r.hi = a;
        }
        vec_push(out, r);
    }
    strvec_free(&ch);
    if (!ok || !out->n) {
        out->n = 0;
        return false;
    }
    /* mergeLineRanges */
    xqsort(out->v, out->n, sizeof(lrange_t), cmp_lrange);
    size_t m = 1;
    for (size_t i = 1; i < out->n; i++) {
        lrange_t *last = &out->v[m - 1];
        if (last->hi == 0) break;
        if (out->v[i].lo <= last->hi + 1) {
            if (out->v[i].hi == 0 || out->v[i].hi > last->hi) last->hi = out->v[i].hi;
            continue;
        }
        out->v[m++] = out->v[i];
    }
    out->n = m;
    return true;
}

static bool in_ranges(int64_t line, const lranges_t *r) {
    for (size_t i = 0; i < r->n; i++)
        if (line >= r->v[i].lo && (r->v[i].hi == 0 || line <= r->v[i].hi)) return true;
    return false;
}

/* parseSearchPath: base dir + glob from the first segment holding glob characters */
static void parse_search_path(const char *p, char **base, char **glob) {
    char *n = str_replace(p, "\\", "/");
    strvec_t segs = str_split(n, "/");
    size_t i = 0;
    while (i < segs.n && !has_glob_chars(segs.v[i])) i++;
    *glob = NULL;
    if (i == segs.n) {
        *base = n;
        n = NULL;
    } else if (i == 0) {
        *base = xstrdup(".");
        *glob = n;
        n = NULL;
    } else {
        sb_t b = SB_INIT, g = SB_INIT;
        for (size_t k = 0; k < segs.n; k++) {
            sb_t *t = k < i ? &b : &g;
            if (k != 0 && k != i) sb_putc(t, '/');
            sb_append(t, segs.v[k]);
        }
        *base = sb_detach(&b);
        *glob = sb_detach(&g);
    }
    free(n);
    strvec_free(&segs);
}

/* parseFindPattern */
static void parse_find_pattern(const char *p, char **base, char **glob, bool *has_glob, bool *dir_only) {
    char *n = str_replace(p, "\\", "/");
    strvec_t segs = str_split(n, "/");
    size_t i = 0;
    while (i < segs.n && !has_glob_chars(segs.v[i])) i++;
    *dir_only = false;
    if (i == segs.n) {
        *base = n;
        *glob = xstrdup("**/*");
        *has_glob = false;
        strvec_free(&segs);
        return;
    }
    *has_glob = true;
    char *g;
    if (i == 0) {
        *base = xstrdup(".");
        g = str_starts(n, "**/") ? xstrdup(n) : xasprintf("**/%s", n);
    } else {
        sb_t b = SB_INIT, gb = SB_INIT;
        for (size_t k = 0; k < segs.n; k++) {
            sb_t *t = k < i ? &b : &gb;
            if (k != 0 && k != i) sb_putc(t, '/');
            sb_append(t, segs.v[k]);
        }
        *base = sb_detach(&b);
        g = sb_detach(&gb);
    }
    size_t gl = strlen(g);
    if (gl && g[gl - 1] == '/') {
        while (gl && g[gl - 1] == '/') g[--gl] = 0;
        *dir_only = true;
    }
    *glob = g;
    free(n);
    strvec_free(&segs);
}

static char *entry_base(const char *entry, bool find_mode) {
    char *b, *g;
    if (find_mode) {
        bool hg, d;
        parse_find_pattern(entry, &b, &g, &hg, &d);
    } else {
        parse_search_path(entry, &b, &g);
    }
    free(g);
    return b;
}

/* delimitedPathPartResolves */
static bool part_resolves(tool_env_t *env, const char *entry, bool find_mode) {
    if (strstr(entry, "://")) return true;
    char *peeled, *sel;
    split_path_sel(entry, &peeled, &sel);
    char *base = entry_base(peeled, find_mode);
    char *abs = !strcmp(base, "/") || !*base ? xstrdup(env->cwd) : tool_resolve_path(env, base);
    struct stat st;
    bool ok = stat(abs, &st) == 0 || !(errno == ENOENT || errno == ENAMETOOLONG || errno == ENOTDIR);
    free(abs);
    free(base);
    free(peeled);
    free(sel);
    return ok;
}

enum { DL_COMMA, DL_SEMI, DL_WS, DL_MIXED };

static bool is_delim(char c, int mode) {
    bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    switch (mode) {
    case DL_COMMA: return c == ',';
    case DL_SEMI: return c == ';';
    case DL_WS: return ws;
    default: return c == ',' || c == ';' || ws;
    }
}

static bool has_top_delim(const char *s) {
    int depth = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\\' && s[i + 1]) { i++; continue; }
        if (s[i] == '{') { depth++; continue; }
        if (s[i] == '}') { if (depth) depth--; continue; }
        if (!depth && is_delim(s[i], DL_MIXED)) return true;
    }
    return false;
}

static strvec_t split_top(const char *s, int mode) {
    strvec_t parts = {0};
    int depth = 0;
    size_t st = 0, i = 0;
    for (; s[i]; i++) {
        if (s[i] == '\\' && s[i + 1]) { i++; continue; }
        if (s[i] == '{') { depth++; continue; }
        if (s[i] == '}') { if (depth) depth--; continue; }
        if (depth || !is_delim(s[i], mode)) continue;
        strvec_push_own(&parts, xstrndup(s + st, i - st));
        st = i + 1;
    }
    strvec_push_own(&parts, xstrndup(s + st, strlen(s + st)));
    return parts;
}

/* tryDelimitedPathSplit; req: 0 none, 1 some, 2 all */
static bool try_delim_split(tool_env_t *env, const char *entry, int mode, int req, bool find_mode, strvec_t *out) {
    strvec_t raw = split_top(entry, mode);
    bool ok = false;
    strvec_t parts = {0};
    if (raw.n >= 2) {
        for (size_t i = 0; i < raw.n; i++) {
            char *p = norm_input(raw.v[i]);
            if (*p) strvec_push_own(&parts, p);
            else free(p);
        }
        ok = parts.n > 0 && !(parts.n < 2 && raw.n == parts.n);
        if (ok && req) {
            size_t good = 0;
            for (size_t i = 0; i < parts.n; i++) good += part_resolves(env, parts.v[i], find_mode);
            ok = req == 2 ? good == parts.n : good > 0;
        }
    }
    if (ok)
        for (size_t i = 0; i < parts.n; i++) strvec_push(out, parts.v[i]);
    strvec_free(&parts);
    strvec_free(&raw);
    return ok;
}

/* an internal URL (`local://`, `skill://`, ...): a scheme the router handles, not a web or file URL */
static bool is_internal_url(const char *s) {
    size_t n = 0;
    while (isalnum((unsigned char)s[n]) || s[n] == '+' || s[n] == '-' || s[n] == '.') n++;
    if (!n || !isalpha((unsigned char)s[0]) || strncmp(s + n, "://", 3)) return false;
    static const char *const ext[] = {"http", "https", "ftp", "ws", "wss", "file"};
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++)
        if (n == strlen(ext[i]) && !strncasecmp(s, ext[i], n)) return false;
    return true;
}

/* splitDelimitedPathEntry with mixedLocalPaths (omp 12a095085d, 1f08eab663, a7003d9901): a `;` list starting with an
 * internal URL splits when every part is a URL (or a `!glob` exclusion for grep); a list that also names local paths
 * splits only when the whole entry does not resolve, so a `;` inside one real URL stays intact */
static bool split_url_list(tool_env_t *env, const char *entry, bool find_mode, strvec_t *out) {
    strvec_t parts = {0};
    if (!try_delim_split(env, entry, DL_SEMI, 0, find_mode, &parts) || parts.n <= 1) {
        strvec_free(&parts);
        return false;
    }
    bool all_urls = true;
    for (size_t i = 0; i < parts.n; i++)
        all_urls &= is_internal_url(parts.v[i]) || (!find_mode && parts.v[i][0] == '!' && parts.v[i][1] && !strstr(parts.v[i], "://"));
    bool split = all_urls;
    if (!split) {
        char *peeled, *sel;
        split_path_sel(entry, &peeled, &sel);
        char *abs = tool_resolve_path(env, peeled);
        struct stat st;
        split = stat(abs, &st) != 0 && (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG || errno == EINVAL);
        free(abs);
        free(peeled);
        free(sel);
    }
    if (split)
        for (size_t i = 0; i < parts.n; i++) strvec_push(out, parts.v[i]);
    strvec_free(&parts);
    return split;
}

/* splitDelimitedPathEntry */
static bool split_delimited(tool_env_t *env, const char *entry, bool find_mode, strvec_t *out) {
    if (!has_top_delim(entry)) return false;
    if (is_internal_url(entry)) return split_url_list(env, entry, find_mode, out);
    if (strstr(entry, "://")) return false;
    if (probe_literal(env, entry) != 0) return false;
    char *peeled, *sel;
    split_path_sel(entry, &peeled, &sel);
    bool keep = sel && probe_literal(env, peeled) != 0;
    if (!keep && !has_glob_chars(peeled) && part_resolves(env, entry, find_mode)) keep = true;
    free(peeled);
    free(sel);
    if (keep) return false;
    return try_delim_split(env, entry, DL_SEMI, 0, find_mode, out) || try_delim_split(env, entry, DL_COMMA, 1, find_mode, out) ||
           try_delim_split(env, entry, DL_WS, 2, find_mode, out) || try_delim_split(env, entry, DL_MIXED, 2, find_mode, out);
}

static void expand_entries(tool_env_t *env, const strvec_t *in, bool find_mode, strvec_t *out) {
    for (size_t i = 0; i < in->n; i++) {
        char *e = norm_input(in->v[i]);
        if (!split_delimited(env, e, find_mode, out)) strvec_push(out, e);
        free(e);
    }
}

/* toPathList: a JSON-encoded string array spelled into the string, else the string itself */
static void path_list(const json_t *v, strvec_t *out) {
    const char *s = json_is_str(v) ? json_sv(v) : NULL;
    if (!s) return;
    char *t = str_trim(s);
    size_t n = strlen(t);
    bool done = false;
    if (n >= 2 && t[0] == '[' && t[n - 1] == ']') {
        json_t *j = json_parse(t, n, NULL);
        if (j && json_is_arr(j)) {
            bool all = true;
            json_t *e;
            json_arr_foreach(j, i, e) if (!json_is_str(e)) all = false;
            if (all) {
                json_arr_foreach(j, i, e) strvec_push(out, json_sv(e));
                done = true;
            }
        }
        json_free(j);
    }
    free(t);
    if (!done) strvec_push(out, s);
}

static bool is_exclusion(tool_env_t *env, const char *entry) {
    if (entry[0] != '!') return false;
    char *p, *sel;
    split_path_sel_literal(env, entry, &p, &sel);
    bool r = probe_literal(env, p) == 0;
    free(p);
    free(sel);
    return r;
}

/* ---------- grep (port of omp tools/grep.ts) ---------- */

#define GREP_FILE_LIMIT 20
#define GREP_MULTI_PER_FILE 10
#define GREP_SINGLE_FILE 50
#define GREP_TOTAL_CAP 2000
#define GREP_MAX_COLUMN 512
#define GREP_MAX_BYTES (50 * 1024)

typedef struct {
    uint32_t line;
    char *text;
} gctx_t;
typedef struct {
    char *abs; /* absolute file path */
    uint32_t line, column;
    char *text;
    bool truncated;
    VEC(gctx_t) before, after;
    const lranges_t *ranges; /* the file's `path:N-M` ranges, NULL when unfiltered */
} gmatch_t;
typedef VEC(gmatch_t) gmatches_t;

typedef struct {
    char *abs;
    char *glob;
    bool bare;
} gtarget_t;

/* omp's pushLine sanitizeText: ANSI escapes, NUL and other control characters never reach a grep row */
static char *grep_clean(const char *s) { return out_sanitize(s, strlen(s), NULL); }

static void gmatch_free(gmatch_t *m) {
    free(m->abs);
    free(m->text);
    for (size_t i = 0; i < m->before.n; i++) free(m->before.v[i].text);
    for (size_t i = 0; i < m->after.n; i++) free(m->after.v[i].text);
    vec_free(&m->before);
    vec_free(&m->after);
}

/* run one native-equivalent search and append its matches (context grouped per match) */
static int grep_run(const char *pattern, const char *root, const char *glob, bool bare, bool icase, bool multiline,
                    bool gitignore, int before, int after, int max_count, int per_file, const strvec_t *exclude,
                    gmatches_t *out, bool *limit_reached, strvec_t *binary_abs, char **err) {
    search_opts_t so = {0};
    so.pattern = pattern;
    so.path = root;
    so.glob = glob;
    so.glob_anchored = glob && !bare;
    so.exclude = exclude ? (const char *const *)exclude->v : NULL;
    so.nexclude = exclude ? exclude->n : 0;
    so.ignore_case = icase;
    so.hidden = true;
    so.no_ignore = !gitignore;
    so.multiline = multiline;
    so.context_before = before;
    so.context_after = after;
    so.max_count = max_count;
    so.max_count_per_file = per_file;
    so.max_columns = GREP_MAX_COLUMN;
    search_result_t sr;
    if (search_run(&so, &sr) != 0) {
        *err = sr.error ? xstrdup(sr.error) : xstrdup("search failed");
        search_result_free(&sr);
        return -1;
    }
    if (sr.limit_reached) *limit_reached = true;
    struct stat st;
    bool root_is_dir = stat(root, &st) == 0 && S_ISDIR(st.st_mode);
    /* binary files whose skipped content matches (named files are searched as text) */
    for (size_t i = 0; i < sr.binary_matches.n && root_is_dir; i++) strvec_push_own(binary_abs, path_join(root, sr.binary_matches.v[i]));
    const char *cur_path = NULL;
    uint32_t cur_block = UINT32_MAX;
    size_t start = out->n;
    VEC(gctx_t) pending = {0};
    for (size_t i = 0; i < sr.hits.n; i++) {
        search_hit_t *h = &sr.hits.v[i];
        bool same = cur_path && !strcmp(cur_path, h->path) && h->block == cur_block;
        if (!same) {
            for (size_t k = 0; k < pending.n; k++) free(pending.v[k].text);
            pending.n = 0;
            cur_path = h->path;
            cur_block = h->block;
        }
        gmatch_t *m = out->n > start && same && out->v[out->n - 1].text ? &out->v[out->n - 1] : NULL;
        if (h->is_context) {
            gctx_t c = {h->line, xstrdup(h->text ? h->text : "")};
            if (m) vec_push(&m->after, c);
            else vec_push(&pending, c);
            continue;
        }
        gmatch_t nm = {0};
        nm.abs = root_is_dir ? path_join(root, h->path) : xstrdup(root);
        nm.line = h->line;
        nm.text = xstrdup(h->text ? h->text : "");
        nm.truncated = h->truncated;
        nm.column = h->truncated ? h->column : 0;
        for (size_t k = 0; k < pending.n; k++) vec_push(&nm.before, pending.v[k]);
        pending.n = 0;
        vec_push(out, nm);
    }
    for (size_t k = 0; k < pending.n; k++) free(pending.v[k].text);
    vec_free(&pending);
    search_result_free(&sr);
    return 0;
}

typedef struct {
    char *key;     /* absolute path */
    size_t count;  /* matches seen */
    ssize_t list;  /* window list index, -1 outside the page window */
} gfile_t;

static char *format_capped(const strvec_t *paths) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < paths->n && i < 5; i++) {
        if (i) sb_append(&b, ", ");
        sb_append(&b, paths->v[i]);
    }
    if (paths->n > 5) sb_appendf(&b, " and %zu more %s", paths->n - 5, paths->n - 5 == 1 ? "file" : "files");
    return sb_detach(&b);
}

static int grep_fail(tool_result_t *out, char *msg) {
    tool_result_error(out, "%s", msg);
    free(msg);
    return 0;
}

static int grep_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(ctx);
    const char *pattern = json_get_str(args, "pattern");
    if (!pattern) pattern = "";
    if (strspn(pattern, " \t\r\n\v\f") == strlen(pattern)) return grep_fail(out, xstrdup("Pattern must not be empty"));
    int64_t skip = 0;
    const json_t *sj = json_get(args, "skip");
    if (json_is_num(sj)) {
        double d = json_get_num(args, "skip", 0);
        if (!isfinite(d) || floor(d) < 0) return grep_fail(out, xstrdup("Skip must be a non-negative number"));
        skip = (int64_t)floor(d);
    }
    bool case_sensitive = json_get_bool(args, "case", true);
    bool gitignore = json_get_bool(args, "gitignore", true);
    bool hashline = str_eq(env->edit_mode, "hashline");

    char *errmsg = NULL;
    strvec_t scoped = {0}, expanded = {0}, exclusions = {0}, entries = {0}, resolved = {0}, raw_paths = {0},
             missing = {0}, effective = {0}, exact = {0}, binary_abs = {0}, binary_disp = {0};
    VEC(lranges_t) spec_ranges = {0};
    VEC(gtarget_t) targets = {0};
    gmatches_t matches = {0};
    VEC(gfile_t) files = {0};
    dfiles_t dfiles = {0};
    strvec_t model = {0};
    char *search_path = NULL, *scope_path = NULL, *glob_filter = NULL;
    bool bare_glob = false, is_dir = false, multi_targets = false;
    VEC(char *) range_keys = {0};
    VEC(size_t) range_spec = {0};

    path_list(json_get(args, "path"), &scoped);
    if (!scoped.n) strvec_push(&scoped, ".");
    expand_entries(env, &scoped, false, &expanded);
    for (size_t i = 0; i < expanded.n; i++) {
        if (is_exclusion(env, expanded.v[i])) strvec_push(&exclusions, expanded.v[i] + 1);
        else strvec_push(&entries, expanded.v[i]);
    }
    if (!entries.n) strvec_push(&entries, ".");
    /* parsePathSpecs */
    for (size_t i = 0; i < entries.n; i++) {
        const char *e = entries.v[i];
        char *sp, *ss, *lp, *ls;
        split_path_sel(e, &sp, &ss);
        split_path_sel_literal(env, e, &lp, &ls);
        bool literal = ss && !ls;
        lranges_t rg = {0};
        if (!literal && ls) {
            char *rerr = NULL;
            if (!parse_line_ranges(ls, &rg, &rerr)) {
                errmsg = rerr ? rerr
                              : xasprintf("path entry \"%s\" \xe2\x80\x94 only line-range selectors like \":50-100\" are "
                                          "supported (no \":raw\"/\":conflicts\")",
                                          e);
            } else if (has_glob_chars(lp) && probe_literal(env, lp) == 0) {
                errmsg = xasprintf("Line-range selector requires a single file, not a glob: %s", e);
            }
            strvec_push(&resolved, lp);
        } else {
            strvec_push(&resolved, literal ? e : e);
        }
        vec_push(&spec_ranges, rg);
        free(sp);
        free(ss);
        free(lp);
        free(ls);
        if (errmsg) goto fail;
    }
    /* resolveToolSearchScope */
    for (size_t i = 0; i < resolved.n; i++) {
        char *n = norm_input(resolved.v[i]);
        bool empty = !*n;
        free(n);
        if (empty) {
            errmsg = xstrdup("Search scope entries must be non-empty paths or globs");
            goto fail;
        }
    }
    expand_entries(env, &resolved, false, &raw_paths);
    for (size_t i = 0; i < raw_paths.n; i++) {
        if (!*raw_paths.v[i]) {
            errmsg = xstrdup("Search scope entries must be non-empty paths or globs");
            goto fail;
        }
        const char *p = raw_paths.v[i];
        if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8) || !strncasecmp(p, "ftp://", 6) ||
            !strncasecmp(p, "ws://", 5) || !strncasecmp(p, "wss://", 6)) {
            errmsg = xasprintf("Cannot search external URL: %s. Use `read` to fetch web content, then search the returned text.", p);
            goto fail;
        }
    }
    if (raw_paths.n > 1) {
        for (size_t i = 0; i < raw_paths.n; i++) {
            char *b = entry_base(raw_paths.v[i], false);
            char *abs = tool_resolve_path(env, b);
            struct stat st;
            bool ok = stat(abs, &st) == 0 || errno != ENOENT;
            strvec_push(ok ? &effective : &missing, raw_paths.v[i]);
            free(abs);
            free(b);
        }
        if (!effective.n) {
            char *j = strvec_join(&missing, ", ");
            errmsg = xasprintf("Path not found: %s", j);
            free(j);
            goto fail;
        }
    } else {
        for (size_t i = 0; i < raw_paths.n; i++) strvec_push(&effective, raw_paths.v[i]);
    }
    /* dedupe (resolveExplicitSearchPaths) */
    {
        strvec_t u = {0};
        for (size_t i = 0; i < effective.n; i++) {
            bool dup = false;
            for (size_t k = 0; k < u.n && !dup; k++) dup = !strcmp(u.v[k], effective.v[i]);
            if (!dup) strvec_push(&u, effective.v[i]);
        }
        if (effective.n == 1 || u.n > 0) {
            strvec_free(&effective);
            effective = u;
        }
    }
    if (effective.n == 1) {
        char *b, *g;
        const char *raw = effective.v[0];
        if (has_glob_chars(raw) && probe_literal(env, raw) == 1) {
            b = str_replace(raw, "\\", "/");
            g = NULL;
        } else {
            parse_search_path(raw, &b, &g);
        }
        search_path = tool_resolve_path(env, b);
        glob_filter = g;
        bare_glob = g && !strchr(raw, '/') && !strchr(raw, '\\');
        scope_path = rel_cwd(search_path, env->cwd, false);
        free(b);
    } else {
        /* resolveSearchPathItems (fanOutFileItems) */
        size_t n = effective.n;
        char **abs = xcalloc(n, sizeof(char *)), **globs = xcalloc(n, sizeof(char *));
        bool *isfile = xcalloc(n, sizeof(bool));
        bool all_exact = true, any_glob = false, any_file = false;
        for (size_t i = 0; i < n; i++) {
            char *b;
            const char *raw = effective.v[i];
            if (has_glob_chars(raw) && probe_literal(env, raw) == 1) {
                b = str_replace(raw, "\\", "/");
                globs[i] = NULL;
            } else {
                parse_search_path(raw, &b, &globs[i]);
            }
            abs[i] = tool_resolve_path(env, b);
            free(b);
            struct stat st;
            isfile[i] = stat(abs[i], &st) == 0 && S_ISREG(st.st_mode);
            if (globs[i] || !isfile[i]) all_exact = false;
            if (globs[i]) any_glob = true;
            if (!globs[i] && isfile[i]) any_file = true;
        }
        /* findCommonBasePath */
        char *common = xstrdup(abs[0]);
        for (size_t i = 1; i < n; i++) {
            size_t k = 0, lastsep = 0;
            const char *a = common, *c = abs[i];
            while (a[k] && c[k] && a[k] == c[k]) {
                if (a[k] == '/') lastsep = k;
                k++;
            }
            if ((!a[k] && (c[k] == '/' || !c[k])) || (!c[k] && a[k] == '/')) lastsep = k;
            common[lastsep] = 0;
        }
        if (!*common) {
            free(common);
            common = xstrdup("/");
        }
        bool common_requested = false;
        for (size_t i = 0; i < n; i++) common_requested |= !strcmp(abs[i], common);
        bool demotes = !all_exact && any_file;
        multi_targets = n > 1 && (!common_requested || demotes || any_glob);
        if (multi_targets) {
            for (size_t i = 0; i < n; i++) {
                gtarget_t t = {xstrdup(abs[i]), globs[i] ? xstrdup(globs[i]) : NULL,
                               globs[i] && !strchr(effective.v[i], '/') && !strchr(effective.v[i], '\\')};
                vec_push(&targets, t);
            }
        } else if (!all_exact) {
            /* one walk at the common base with a brace union of per-item globs */
            strvec_t pats = {0};
            for (size_t i = 0; i < n; i++) {
                char *rel = path_relative(common, abs[i]);
                char *p;
                if (globs[i]) p = strcmp(rel, ".") ? xasprintf("%s/%s", rel, globs[i]) : xstrdup(globs[i]);
                else if (!isfile[i]) p = strcmp(rel, ".") ? xasprintf("%s/**/*", rel) : xstrdup("**/*");
                else p = strcmp(rel, ".") ? xstrdup(rel) : xstrdup(path_basename(abs[i]));
                bool dup = false;
                for (size_t k = 0; k < pats.n && !dup; k++) dup = !strcmp(pats.v[k], p);
                if (!dup) strvec_push(&pats, p);
                free(p);
                free(rel);
            }
            if (pats.n == 1) glob_filter = xstrdup(pats.v[0]);
            else if (pats.n > 1) {
                char *j = strvec_join(&pats, ",");
                glob_filter = xasprintf("{%s}", j);
                free(j);
            }
            strvec_free(&pats);
        }
        if (all_exact)
            for (size_t i = 0; i < n; i++) strvec_push(&exact, abs[i]);
        search_path = common;
        strvec_t sc = {0};
        for (size_t i = 0; i < n; i++) {
            char *a = tool_resolve_path(env, effective.v[i]);
            const char *e = effective.v[i];
            strvec_push_own(&sc, rel_cwd(a, env->cwd, str_ends(e, "/")));
            free(a);
        }
        scope_path = strvec_join(&sc, ", ");
        strvec_free(&sc);
        for (size_t i = 0; i < n; i++) {
            free(abs[i]);
            free(globs[i]);
        }
        free(abs);
        free(globs);
        free(isfile);
    }
    {
        struct stat st;
        if (stat(search_path, &st) != 0) {
            errmsg = xasprintf("Path not found: %s%s", scope_path,
                               raw_paths.n > 1 ? " (`path` list entries must each exist relative to cwd)" : "");
            goto fail;
        }
        is_dir = S_ISDIR(st.st_mode);
    }
    /* line-range filters keyed by absolute file */
    bool has_ranges = false;
    int64_t range_fetch = 0;
    for (size_t i = 0; i < spec_ranges.n; i++) {
        if (!spec_ranges.v[i].n) continue;
        has_ranges = true;
        char *a = tool_resolve_path(env, resolved.v[i]);
        struct stat st;
        if (stat(a, &st) != 0) {
            errmsg = xasprintf("Path not found for line-range selector: %s", entries.v[i]);
            free(a);
            goto fail;
        }
        if (!S_ISREG(st.st_mode)) {
            errmsg = xasprintf("Line-range selector requires a single file: %s is a directory", entries.v[i]);
            free(a);
            goto fail;
        }
        vec_push(&range_keys, a);
        vec_push(&range_spec, i);
    }
    bool multi_scope = is_dir || exact.n || multi_targets;
    int64_t per_file_cap = multi_scope ? GREP_MULTI_PER_FILE : GREP_SINGLE_FILE;
    for (size_t i = 0; i < spec_ranges.n; i++)
        for (size_t k = 0; k < spec_ranges.v[i].n; k++) {
            lrange_t r = spec_ranges.v[i].v[k];
            range_fetch = MAX(range_fetch, r.hi ? r.hi : r.lo - 1 + per_file_cap + 1);
        }
    range_fetch = MIN(range_fetch, (int64_t)4 * 1024 * 1024);
    int64_t native_per_file = has_ranges ? MAX(per_file_cap + 1, range_fetch) : per_file_cap + 1;
    int64_t native_max = has_ranges ? ((GREP_TOTAL_CAP + per_file_cap) / (per_file_cap + 1)) * native_per_file : GREP_TOTAL_CAP;
    if (native_max > INT32_MAX) native_max = INT32_MAX;
    json_t *cb = settings_get("grep.contextBefore"), *ca = settings_get("grep.contextAfter");
    int before = json_is_num(cb) ? (int)MAX(0, cb->u.num.d) : 0;
    int after = json_is_num(ca) ? (int)MAX(0, ca->u.num.d) : 0;
    bool multiline = strchr(pattern, '\n') || strstr(pattern, "\\n");
    bool limit_reached = false, total_cap = false;
    char *serr = NULL;
    if (exact.n || multi_targets) {
        size_t nt = exact.n ? exact.n : targets.n;
        for (size_t t = 0; t < nt; t++) {
            const char *root = exact.n ? exact.v[t] : targets.v[t].abs;
            const char *g = exact.n ? NULL : targets.v[t].glob;
            bool bare = exact.n ? false : targets.v[t].bare;
            gmatches_t tm = {0};
            if (grep_run(pattern, root, g, bare, !case_sensitive, multiline, gitignore, before, after, (int)native_max,
                         (int)native_per_file, &exclusions, &tm, &limit_reached, &binary_abs, &serr) != 0) {
                vec_free(&tm);
                goto search_err;
            }
            if ((int64_t)tm.n >= native_max) total_cap = true;
            for (size_t k = 0; k < tm.n; k++) {
                bool dup = false;
                for (size_t q = 0; q < matches.n && !dup; q++)
                    dup = matches.v[q].line == tm.v[k].line && !strcmp(matches.v[q].abs, tm.v[k].abs);
                if (dup) gmatch_free(&tm.v[k]);
                else vec_push(&matches, tm.v[k]);
            }
            vec_free(&tm);
        }
    } else {
        if (grep_run(pattern, search_path, glob_filter, bare_glob, !case_sensitive, multiline, gitignore, before, after,
                     (int)native_max, (int)native_per_file, &exclusions, &matches, &limit_reached, &binary_abs, &serr) != 0)
            goto search_err;
        total_cap = (int64_t)matches.n >= native_max;
    }
    goto searched;
search_err:
    if (!strncasecmp(serr, "regex parse error", 17) || !strncasecmp(serr, "regex error", 11)) {
        const char *rest = serr + (!strncasecmp(serr, "regex parse error", 17) ? 17 : 11);
        if (*rest == ':') rest++;
        while (*rest == ' ' || *rest == '\t' || *rest == '\n') rest++;
        errmsg = xasprintf("Invalid regex: %s", rest);
        free(serr);
    } else {
        errmsg = serr;
    }
    goto fail;
searched:
    /* line-range filtering */
    if (range_keys.n) {
        size_t w = 0;
        for (size_t i = 0; i < matches.n; i++) {
            gmatch_t *m = &matches.v[i];
            const lranges_t *rg = NULL;
            for (size_t k = 0; k < range_keys.n && !rg; k++)
                if (!strcmp(range_keys.v[k], m->abs)) rg = &spec_ranges.v[range_spec.v[k]];
            if (rg && !in_ranges(m->line, rg)) {
                gmatch_free(m);
                continue;
            }
            if (rg) {
                size_t b2 = 0, a2 = 0;
                for (size_t k = 0; k < m->before.n; k++) {
                    if (in_ranges(m->before.v[k].line, rg)) m->before.v[b2++] = m->before.v[k];
                    else free(m->before.v[k].text);
                }
                m->before.n = b2;
                for (size_t k = 0; k < m->after.n; k++) {
                    if (in_ranges(m->after.v[k].line, rg)) m->after.v[a2++] = m->after.v[k];
                    else free(m->after.v[k].text);
                }
                m->after.n = a2;
                m->ranges = rg; /* a cross-line match is kept by its first line: its other rows are filtered at render */
            }
            matches.v[w++] = *m;
        }
        matches.n = w;
    }
    /* group by file in encounter order; per-file caps; page window */
    bool can_page = multi_scope;
    int64_t win_start = can_page ? skip : 0, win_end = can_page ? skip + GREP_FILE_LIMIT : INT64_MAX;
    bool per_file_limit = false;
    strvec_t capped = {0};
    VEC(sizevec_t) lists = {0};
    VEC(size_t) mfile = {0}; /* match index -> files index */
    for (size_t i = 0; i < matches.n; i++) {
        size_t f = 0;
        while (f < files.n && strcmp(files.v[f].key, matches.v[i].abs)) f++;
        if (f == files.n) {
            gfile_t nf = {matches.v[i].abs, 0, -1};
            int64_t order = (int64_t)files.n;
            if (order >= win_start && order < win_end) {
                nf.list = (ssize_t)lists.n;
                sizevec_t l = {0};
                vec_push(&lists, l);
            }
            vec_push(&files, nf);
        }
        gfile_t *gf = &files.v[f];
        vec_push(&mfile, f);
        gf->count++;
        if ((int64_t)gf->count > per_file_cap) {
            per_file_limit = true;
            if ((int64_t)gf->count == per_file_cap + 1 && gf->list >= 0) strvec_push(&capped, gf->key);
            continue;
        }
        if (gf->list >= 0) vec_push(&lists.v[gf->list], i);
    }
    size_t total_files = files.n;
    char *total_label = total_cap ? xasprintf("%zu+", total_files) : xasprintf("%zu", total_files);
    int64_t skip_files = can_page ? MIN(skip, (int64_t)total_files) : 0;
    bool file_limit = can_page && (int64_t)total_files > skip_files + GREP_FILE_LIMIT;
    size_t selected = 0;
    for (size_t i = 0; i < lists.n; i++) selected += lists.v[i].n;
    int64_t next_skip = skip_files + (int64_t)lists.n;
    char *limit_msg = file_limit ? xasprintf("Showing files %lld-%lld of %s. Use skip=%lld for the next page, or narrow "
                                             "paths/pattern.",
                                             (long long)skip_files + 1, (long long)next_skip, total_label, (long long)next_skip)
                                 : NULL;
    /* the page's warning notes, one per line (omp warningNote: missing paths, then binary matches) */
    char *missing_note = NULL;
    for (size_t k = 0; k < binary_abs.n; k++) strvec_push_own(&binary_disp, rel_cwd(binary_abs.v[k], env->cwd, false));
    {
        sb_t w = SB_INIT;
        if (missing.n) {
            char *j = strvec_join(&missing, ", ");
            sb_appendf(&w, "Skipped missing paths: %s", j);
            free(j);
        }
        if (binary_disp.n) {
            char *names = format_capped(&binary_disp);
            sb_appendf(&w, "%sBinary files also match (name one as `path` to search it as text): %s", w.len ? "\n" : "", names);
            free(names);
        }
        if (w.len) missing_note = sb_detach(&w);
        else sb_free(&w);
    }
    json_t *details = json_obj();
    json_set_str(details, "scopePath", scope_path);
    json_set_str(details, "searchPath", search_path);
    json_set_str(details, "cwd", env->cwd);
    if (!selected) {
        json_set_int(details, "matchCount", 0);
        json_set_int(details, "fileCount", 0);
        json_set(details, "files", json_arr());
        json_set_bool(details, "truncated", false);
        if (binary_disp.n) {
            json_t *bm = json_arr();
            for (size_t k = 0; k < binary_disp.n; k++) json_push_str(bm, binary_disp.v[k]);
            json_set(details, "binaryMatches", bm);
        }
        if (missing.n) {
            json_t *mp = json_arr();
            for (size_t k = 0; k < missing.n; k++) json_push_str(mp, missing.v[k]);
            json_set(details, "missingPaths", mp);
        }
        bool past_end = can_page && skip > 0 && total_files > 0 && skip_files >= (int64_t)total_files;
        sb_t t = SB_INIT;
        if (past_end) sb_appendf(&t, "No more results (%s files total; skip=%lld is past the end)", total_label, (long long)skip);
        else if (binary_disp.n) sb_append(&t, "No matches in text files");
        else sb_append(&t, "No matches found");
        if (missing_note) sb_appendf(&t, "\n%s", missing_note);
        tool_result_text(out, sb_cstr(&t));
        sb_free(&t);
        out->useless = true;
        out->details = details;
        free(total_label);
        free(limit_msg);
        free(missing_note);
        strvec_free(&capped);
        for (size_t i = 0; i < lists.n; i++) vec_free(&lists.v[i]);
        vec_free(&lists);
        vec_free(&mfile);
        goto cleanup;
    }
    /* files in window order, each with its kept matches in order */
    bool lines_truncated = false;
    for (size_t li = 0; li < lists.n; li++) {
        size_t f0 = mfile.v[lists.v[li].v[0]];
        char *disp = is_dir ? rel_cwd(files.v[f0].key, env->cwd, false) : rel_cwd(search_path, env->cwd, false);
        dfile_t df = {.path = disp};
        if (hashline) {
            size_t flen;
            bool over = false;
            char *content = fs_read_limit(files.v[f0].key, 4u << 20, &flen, &over);
            if (content && !over && utf8_valid(content, flen)) df.tag = xstrdup(edit_store_record(&env->store, files.v[f0].key, content, -1, -1));
            free(content);
        }
        for (size_t k = 0; k < lists.v[li].n; k++) {
            gmatch_t *m = &matches.v[lists.v[li].v[k]];
            for (size_t c = 0; c < m->before.n; c++) {
                dline_t dl = {.line = m->before.v[c].line, .text = grep_clean(m->before.v[c].text)};
                vec_push(&df.lines, dl);
            }
            /* a cross-line match arrives as one block: each line it spans gets its own numbered row; a cut line shows a
             * window around its first match, so the first row names where that match sits */
            strvec_t rows = {0};
            for (const char *p = m->text;;) {
                const char *nl = strchr(p, '\n');
                size_t n = nl ? (size_t)(nl - p) : strlen(p);
                if (nl && n && p[n - 1] == '\r') n--;
                char *raw_row = xstrndup(p, n);
                strvec_push_own(&rows, grep_clean(raw_row));
                free(raw_row);
                if (!nl) break;
                p = nl + 1;
            }
            if (m->column) {
                char *c0 = xasprintf("%s [col %u]", rows.v[0], m->column);
                free(rows.v[0]);
                rows.v[0] = c0;
            }
            if (m->truncated) lines_truncated = true;
            for (size_t r = 0; r < rows.n; r++) {
                uint32_t ln = m->line + (uint32_t)r;
                /* like context lines, rows outside the selected ranges are neither shown nor recorded as seen */
                if (m->ranges && !in_ranges(ln, m->ranges)) continue;
                /* the cut window is contiguous, so only the block's first and last rows may be partial */
                dline_t ml = {.line = ln, .match = r == 0, .cut = m->truncated && (r == 0 || r + 1 == rows.n),
                              .text = rows.v[r]};
                rows.v[r] = NULL;
                vec_push(&df.lines, ml);
            }
            for (size_t r = 0; r < rows.n; r++) free(rows.v[r]);
            vec_free(&rows);
            for (size_t c = 0; c < m->after.n; c++) {
                dline_t dl = {.line = m->after.v[c].line, .text = grep_clean(m->after.v[c].text)};
                vec_push(&df.lines, dl);
            }
        }
        vec_push(&dfiles, df);
    }
    bool grouped = is_dir || multi_scope;
    model = grep_render(&dfiles, grouped, hashline ? 1 : 0);
    /* capped files the kept text shows with a full cap of matches */
    strvec_t capped_disp = {0};
    VEC(size_t) capped_end = {0};
    for (size_t i = 0; i < capped.n; i++) {
        for (size_t d = 0; d < dfiles.n; d++) {
            size_t f0 = mfile.v[lists.v[d].v[0]];
            if (!strcmp(files.v[f0].key, capped.v[i]) && (int64_t)lists.v[d].n == per_file_cap) {
                strvec_push(&capped_disp, dfiles.v[d].path);
                vec_push(&capped_end, dfiles.v[d].model_end);
            }
        }
    }
    char *raw = strvec_join(&model, "\n");
    size_t raw_len = strlen(raw);
    out_trunc_t tr = {0};
    char *suffix = NULL;
    size_t reserved = 0;
    for (int guard = 0;; guard++) {
        strvec_t shown = {0};
        if (suffix) {
            int64_t kept = tr.truncated ? tr.output_lines : (int64_t)model.n;
            for (size_t i = 0; i < capped_disp.n; i++)
                if (!multi_scope || (int64_t)capped_end.v[i] <= kept) strvec_push(&shown, capped_disp.v[i]);
        } else {
            for (size_t i = 0; i < capped_disp.n; i++) strvec_push(&shown, capped_disp.v[i]);
        }
        char *cap_notice = NULL;
        if (shown.n) {
            char *names = format_capped(&shown);
            cap_notice = multi_scope ? xasprintf("More than %lld matches in %s; grep keeps the first %lld per file when "
                                                 "searching several. Search one file alone for up to %d.",
                                                 (long long)per_file_cap, names, (long long)per_file_cap, GREP_SINGLE_FILE)
                                     : xasprintf("More than %lld matches; grep keeps the first %lld in a file. Add a line "
                                                 "range (`path:N-M`) or narrow the pattern for the rest.",
                                                 (long long)per_file_cap, (long long)per_file_cap);
            free(names);
        }
        strvec_free(&shown);
        sb_t s = SB_INIT;
        if (limit_msg) sb_appendf(&s, "\n\n%s", limit_msg);
        if (missing_note) sb_appendf(&s, "\n\n%s", missing_note);
        if (cap_notice) sb_appendf(&s, "\n\n%s", cap_notice);
        free(cap_notice);
        char *ns = sb_detach(&s);
        if (suffix && strlen(ns) <= reserved) {
            free(suffix);
            suffix = ns;
            break;
        }
        free(suffix);
        suffix = ns;
        reserved = strlen(suffix);
        out_trunc_free(&tr);
        memset(&tr, 0, sizeof tr);
        out_truncate_head(raw, raw_len, (int64_t)GREP_MAX_BYTES - (int64_t)reserved, INT64_MAX, &tr);
        if (guard > 8) break;
    }
    /* hashline: the rows the byte cap kept become editable anchors (cut rows excluded) */
    if (hashline) {
        int64_t kept = tr.truncated ? tr.output_lines : (int64_t)model.n;
        for (size_t i = 0; i < dfiles.n; i++) {
            const dfile_t *df = &dfiles.v[i];
            if (!df->tag) continue;
            /* the file's model rows (with `...` gaps) end at model_end; its header sits just above them */
            int64_t nrows = 0;
            uint32_t last = 0;
            for (size_t k = 0; k < df->lines.n; k++) {
                if (last && df->lines.v[k].line > last + 1) nrows++;
                nrows++;
                last = df->lines.v[k].line;
            }
            int64_t first_row = (int64_t)df->model_end - nrows;
            if (kept < first_row) continue; /* dropped with its header: this output never showed the tag */
            int64_t keep_rows = kept - first_row;
            char *abs = tool_resolve_path(env, df->path);
            snapshot_t *sn = edit_store_find(&env->store, abs, df->tag);
            free(abs);
            if (!sn) continue;
            pthread_mutex_lock(&env->store.mu);
            bool whole = sn->seen.n == 1 && sn->seen.v[0].hi == INT64_MAX;
            bool any_cut = false; /* omp recordFullySeenRows: any cut row in the section */
            for (size_t k = 0; k < df->lines.n; k++) any_cut |= df->lines.v[k].cut;
            int64_t row = 0;
            last = 0;
            size_t recorded = 0;
            for (size_t k = 0; k < df->lines.n && !whole; k++) {
                const dline_t *l = &df->lines.v[k];
                if (last && l->line > last + 1) row++;
                last = l->line;
                if (row++ >= keep_rows) break;
                if (l->cut) continue;
                line_span_t r = {l->line, l->line};
                vec_push(&sn->seen, r);
                recorded++;
            }
            /* only the header survived, or a cut row: provenance that anchors no line (omp NO_FULL_LINE) */
            if (!whole && (any_cut || (keep_rows <= 0 && !recorded))) {
                line_span_t r = {0, 0};
                vec_push(&sn->seen, r);
            }
            pthread_mutex_unlock(&env->store.mu);
        }
    }
    sb_t text = SB_INIT;
    sb_appendn(&text, tr.content ? tr.content : "", tr.content ? tr.content_len : 0);
    sb_append(&text, suffix);
    tool_result_text(out, sb_cstr(&text));
    sb_free(&text);
    /* details (omp GrepToolDetails) */
    json_set_int(details, "matchCount", (int64_t)selected);
    json_set_int(details, "fileCount", (int64_t)dfiles.n);
    json_t *fl = json_arr(), *fm = json_arr();
    for (size_t i = 0; i < dfiles.n; i++) {
        json_push_str(fl, dfiles.v[i].path);
        json_t *o = json_obj();
        json_set_str(o, "path", dfiles.v[i].path);
        size_t cnt = 0;
        for (size_t k = 0; k < dfiles.v[i].lines.n; k++) cnt += dfiles.v[i].lines.v[k].match;
        json_set_int(o, "count", (int64_t)cnt);
        json_push(fm, o);
    }
    json_set(details, "files", fl);
    json_set(details, "fileMatches", fm);
    json_set_bool(details, "truncated", file_limit || per_file_limit || limit_reached || tr.truncated || lines_truncated);
    if (file_limit) json_set_int(details, "fileLimitReached", GREP_FILE_LIMIT);
    if (per_file_limit) json_set_int(details, "perFileLimitReached", per_file_cap);
    {
        char *dc = grep_display(&dfiles, grouped);
        json_set_str(details, "displayContent", dc);
        free(dc);
    }
    if (binary_disp.n) {
        json_t *bm = json_arr();
        for (size_t k = 0; k < binary_disp.n; k++) json_push_str(bm, binary_disp.v[k]);
        json_set(details, "binaryMatches", bm);
    }
    if (missing.n) {
        json_t *mp = json_arr();
        for (size_t k = 0; k < missing.n; k++) json_push_str(mp, missing.v[k]);
        json_set(details, "missingPaths", mp);
    }
    if (lines_truncated) json_set_bool(details, "linesTruncated", true);
    {
        json_t *meta = NULL;
        if (tr.truncated) out_meta_from_trunc(&meta, &tr, "head", 0, 0, NULL, 0, 0);
        if (lines_truncated) out_meta_column_truncated(&meta, GREP_MAX_COLUMN, "bytes", NULL, 0, 0, 0, NULL);
        if (meta) json_set(details, "meta", meta);
    }
    out->details = details;
    out_trunc_free(&tr);
    free(suffix);
    free(raw);
    strvec_free(&capped_disp);
    vec_free(&capped_end);
    free(total_label);
    free(limit_msg);
    free(missing_note);
    strvec_free(&capped);
    for (size_t i = 0; i < lists.n; i++) vec_free(&lists.v[i]);
    vec_free(&lists);
    vec_free(&mfile);
    goto cleanup;
fail:
    tool_result_error(out, "%s", errmsg);
    free(errmsg);
cleanup:
    strvec_free(&model);
    for (size_t k = 0; k < dfiles.n; k++) {
        for (size_t m = 0; m < dfiles.v[k].lines.n; m++) free(dfiles.v[k].lines.v[m].text);
        vec_free(&dfiles.v[k].lines);
        free(dfiles.v[k].path);
        free(dfiles.v[k].tag);
    }
    vec_free(&dfiles);
    for (size_t i = 0; i < matches.n; i++) gmatch_free(&matches.v[i]);
    vec_free(&matches);
    vec_free(&files);
    for (size_t i = 0; i < targets.n; i++) {
        free(targets.v[i].abs);
        free(targets.v[i].glob);
    }
    vec_free(&targets);
    for (size_t i = 0; i < spec_ranges.n; i++) vec_free(&spec_ranges.v[i]);
    vec_free(&spec_ranges);
    for (size_t i = 0; i < range_keys.n; i++) free(range_keys.v[i]);
    vec_free(&range_keys);
    vec_free(&range_spec);
    free(search_path);
    free(scope_path);
    free(glob_filter);
    strvec_free(&scoped);
    strvec_free(&expanded);
    strvec_free(&exclusions);
    strvec_free(&entries);
    strvec_free(&resolved);
    strvec_free(&raw_paths);
    strvec_free(&missing);
    strvec_free(&effective);
    strvec_free(&exact);
    strvec_free(&binary_abs);
    strvec_free(&binary_disp);
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

/* ---------- formatGroupedPaths (pi-utils path-tree): prefix-folded `# dir/` tree of bare names ---------- */

typedef struct gpnode gpnode_t;
struct gpnode {
    strvec_t files;
    strvec_t dir_names;
    VEC(gpnode_t *) dirs;
};

static gpnode_t *gpnode_child(gpnode_t *n, const char *name) {
    for (size_t i = 0; i < n->dirs.n; i++)
        if (!strcmp(n->dir_names.v[i], name)) return n->dirs.v[i];
    gpnode_t *c = xcalloc(1, sizeof *c);
    strvec_push(&n->dir_names, name);
    vec_push(&n->dirs, c);
    return c;
}

static void gpnode_free(gpnode_t *n) {
    for (size_t i = 0; i < n->dirs.n; i++) gpnode_free(n->dirs.v[i]);
    strvec_free(&n->files);
    strvec_free(&n->dir_names);
    vec_free(&n->dirs);
    free(n);
}

static void gpnode_walk(const gpnode_t *n, int depth, strvec_t *out) {
    for (size_t i = 0; i < n->files.n; i++) strvec_push(out, n->files.v[i]);
    for (size_t i = 0; i < n->dirs.n; i++) {
        const gpnode_t *d = n->dirs.v[i];
        sb_t h = SB_INIT;
        for (int k = 0; k <= depth; k++) sb_putc(&h, '#');
        sb_appendf(&h, " %s", n->dir_names.v[i]);
        while (d->files.n == 0 && d->dirs.n == 1) {
            sb_appendf(&h, "/%s", d->dir_names.v[0]);
            d = d->dirs.v[0];
        }
        sb_putc(&h, '/');
        strvec_push_own(out, sb_detach(&h));
        gpnode_walk(d, depth + 1, out);
    }
}

char *format_grouped_paths(const strvec_t *paths) {
    if (!paths->n) return xstrdup("");
    gpnode_t *root = xcalloc(1, sizeof *root);
    for (size_t i = 0; i < paths->n; i++) {
        char *p = str_replace(paths->v[i], "\\", "/");
        size_t len = strlen(p);
        bool is_dir = len && p[len - 1] == '/';
        if (strstr(p, "://")) {
            bool dup = false;
            for (size_t k = 0; k < root->files.n && !dup; k++) dup = !strcmp(root->files.v[k], p);
            if (!dup) strvec_push(&root->files, p);
            free(p);
            continue;
        }
        if (is_dir) p[--len] = 0;
        if (!len) {
            free(p);
            continue;
        }
        strvec_t segs = str_split(p, "/");
        size_t ndir = is_dir ? segs.n : segs.n - 1;
        gpnode_t *n = root;
        for (size_t k = 0; k < ndir; k++) n = gpnode_child(n, segs.v[k]);
        if (!is_dir) {
            bool dup = false;
            for (size_t k = 0; k < n->files.n && !dup; k++) dup = !strcmp(n->files.v[k], segs.v[segs.n - 1]);
            if (!dup) strvec_push(&n->files, segs.v[segs.n - 1]);
        }
        strvec_free(&segs);
        free(p);
    }
    strvec_t out = {0};
    gpnode_walk(root, 0, &out);
    gpnode_free(root);
    char *r = strvec_join(&out, "\n");
    strvec_free(&out);
    return r;
}

/* ---------- glob (port of omp tools/glob.ts) ---------- */

#define GLOB_MAX_LIMIT 200

typedef struct {
    char *path; /* display path (dirs end with '/') */
    int64_t mtime;
    char *key;  /* walk-relative path for tie ordering */
} gitem_t;

static int cmp_gitem_mtime(const void *a, const void *b) {
    const gitem_t *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcmp(x->key, y->key);
}

/* JS Number#toString for the clamp notice */
static char *js_num(double d) {
    if (d == floor(d) && fabs(d) < 1e21) return xasprintf("%.0f", d);
    for (int p = 1; p <= 17; p++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.*g", p, d);
        if (strtod(buf, NULL) == d) return xstrdup(buf);
    }
    return xasprintf("%.17g", d);
}

typedef struct {
    pthread_mutex_t mu;
    VEC(gitem_t) items;
    const char *root;
    const char *cwd;
    globset_t *sel;
    bool dir_only;
} gwalk_t;

static bool glob_walk_cb(const walk_entry_t *e, void *ud) {
    gwalk_t *g = ud;
    if (!globset_match(g->sel, e->path)) return true;
    int type = e->type;
    if (g->dir_only && type != WALK_DIR) return true;
    gitem_t it;
    it.key = xstrdup(e->path);
    it.mtime = e->mtime_ms;
    char *full = path_join(g->root, e->path);
    it.path = rel_cwd(full, g->cwd, type == WALK_DIR);
    free(full);
    pthread_mutex_lock(&g->mu);
    vec_push(&g->items, it);
    pthread_mutex_unlock(&g->mu);
    return true;
}

/* globSearchPath (omp 1d9d5d1d46): the `path` a call searches. Models often pass the glob as `pattern` beside a
 * directory `path` or instead of it; each `;` entry of `pattern` is scoped under each `path` entry unless it is
 * absolute, `~` or a URL. Owned, or NULL without either. */
char *glob_search_path(const json_t *args) {
    const char *base = json_get_str(args, "path");
    const char *pattern = json_get_str(args, "pattern");
    if (!pattern) return base ? xstrdup(base) : NULL;
    strvec_t pats = {0}, bases = {0}, scoped = {0};
    strvec_t ps = str_split(pattern, ";");
    for (size_t i = 0; i < ps.n; i++) {
        char *t = str_trim(ps.v[i]);
        if (*t) strvec_push_own(&pats, t);
        else free(t);
    }
    strvec_free(&ps);
    if (base) {
        strvec_t bs = str_split(base, ";");
        for (size_t i = 0; i < bs.n; i++) {
            char *t = str_trim(bs.v[i]);
            if (*t) strvec_push_own(&bases, t);
            else free(t);
        }
        strvec_free(&bs);
    }
    char *r;
    if (!bases.n) {
        r = strvec_join(&pats, ";");
    } else {
        for (size_t b = 0; b < bases.n; b++) {
            size_t bl = strlen(bases.v[b]);
            while (bl && (bases.v[b][bl - 1] == '/' || bases.v[b][bl - 1] == '\\')) bl--;
            for (size_t k = 0; k < pats.n; k++) {
                const char *pt = pats.v[k];
                const char *sep = strstr(pt, "://");
                bool url = sep && sep > pt && isalpha((unsigned char)pt[0]);
                for (const char *q = pt; url && q < sep; q++) url = isalnum((unsigned char)*q) || strchr("+.-", *q);
                bool standalone = pt[0] == '/' || pt[0] == '~' || url;
                char *e = standalone ? xstrdup(pt) : xasprintf("%.*s/%s", (int)bl, bases.v[b], pt);
                if (!strvec_contains(&scoped, e)) strvec_push_own(&scoped, e);
                else free(e);
            }
        }
        r = strvec_join(&scoped, ";");
    }
    strvec_free(&pats);
    strvec_free(&bases);
    strvec_free(&scoped);
    return r;
}

static int glob_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(ctx);
    /* unknown keys reach execute (lenient validation) so a misplaced `pattern` is refused, not dropped unread */
    {
        static const char *const known[] = {"path", "hidden", "gitignore", "limit", "pattern", NULL};
        sb_t bad = SB_INIT;
        const char *k;
        json_t *v;
        json_obj_foreach(args, i, k, v) {
            bool ok = false;
            for (size_t j = 0; known[j] && !ok; j++) ok = !strcmp(known[j], k);
            if (!ok) sb_appendf(&bad, "%s`%s`", bad.len ? ", " : "", k);
        }
        if (bad.len) {
            tool_result_error(out, "glob has no %s argument; put the glob in `path`", bad.s);
            sb_free(&bad);
            return 0;
        }
        if (json_get(args, "pattern") && !json_is_str(json_get(args, "pattern"))) {
            tool_result_error(out, "`pattern` must be a string glob");
            return 0;
        }
        json_t *declared = json_clone(args);
        json_del(declared, "pattern");
        char *summary = schema_summary(self->parameters, declared);
        json_free(declared);
        if (summary) {
            tool_result_error(out, "%s", summary);
            free(summary);
            return 0;
        }
    }
    char *search_path = glob_search_path(args);
    char *errmsg = NULL;
    strvec_t scoped = {0}, expanded = {0}, patterns = {0}, missing = {0}, effective = {0};
    VEC(gitem_t) items = {0};
    char *scope_path = NULL;
    typedef struct {
        char *abs, *glob;
        bool has_glob, dir_only;
    } ttarget_t;
    VEC(ttarget_t) targets = {0};

    {
        json_t *sp = search_path ? json_str(search_path) : NULL;
        path_list(sp, &scoped);
        json_free(sp);
        free(search_path);
    }
    if (!scoped.n) strvec_push(&scoped, ".");
    expand_entries(env, &scoped, true, &expanded);
    for (size_t i = 0; i < expanded.n; i++) {
        char *n = norm_input(expanded.v[i]);
        char *r = str_replace(n, "\\", "/");
        free(n);
        if (*r && strspn(r, "/") == strlen(r)) { /* the registered glob tool aliases a bare `/` to cwd */
            free(r);
            r = xstrdup(".");
        }
        strvec_push_own(&patterns, r);
    }
    for (size_t i = 0; i < patterns.n; i++)
        if (!*patterns.v[i]) {
            errmsg = xstrdup("`path` must contain non-empty globs or paths");
            goto fail;
        }
    if (patterns.n > 1) {
        for (size_t i = 0; i < patterns.n; i++) {
            char *b = entry_base(patterns.v[i], true);
            char *abs = tool_resolve_path(env, b);
            struct stat st;
            bool ok = stat(abs, &st) == 0 || errno != ENOENT;
            strvec_push(ok ? &effective : &missing, patterns.v[i]);
            free(abs);
            free(b);
        }
        if (!effective.n) {
            sb_t m = SB_INIT;
            for (size_t i = 0; i < missing.n; i++) sb_appendf(&m, "\n- %s", missing.v[i]);
            errmsg = xasprintf("None of the %zu paths exist:%s", missing.n, m.s);
            sb_free(&m);
            goto fail;
        }
    } else {
        strvec_push(&effective, patterns.v[0]);
    }
    {
        strvec_t u = {0};
        for (size_t i = 0; i < effective.n; i++) {
            bool dup = false;
            for (size_t k = 0; k < u.n && !dup; k++) dup = !strcmp(u.v[k], effective.v[i]);
            if (!dup) strvec_push(&u, effective.v[i]);
        }
        strvec_free(&effective);
        effective = u;
    }
    bool single = effective.n <= 1;
    for (size_t i = 0; i < effective.n; i++) {
        ttarget_t t;
        char *b;
        parse_find_pattern(effective.v[i], &b, &t.glob, &t.has_glob, &t.dir_only);
        t.abs = tool_resolve_path(env, b);
        free(b);
        vec_push(&targets, t);
    }
    if (single) {
        scope_path = rel_cwd(targets.v[0].abs, env->cwd, false);
    } else {
        strvec_t sc = {0};
        for (size_t i = 0; i < effective.n; i++) {
            char *a = tool_resolve_path(env, effective.v[i]);
            strvec_push_own(&sc, rel_cwd(a, env->cwd, str_ends(effective.v[i], "/")));
            free(a);
        }
        scope_path = strvec_join(&sc, ", ");
        strvec_free(&sc);
    }
    for (size_t i = 0; i < targets.n; i++)
        if (!strcmp(targets.v[i].abs, "/")) {
            errmsg = xstrdup("Searching from root directory '/' is not allowed");
            goto fail;
        }
    double req_limit = 200;
    if (json_is_num(json_get(args, "limit"))) req_limit = json_get_num(args, "limit", 200);
    if (!isfinite(req_limit) || req_limit <= 0) {
        errmsg = xstrdup("Limit must be a positive number");
        goto fail;
    }
    int64_t limit = (int64_t)MIN((double)GLOB_MAX_LIMIT, MAX(1.0, floor(req_limit)));
    bool hidden = json_get_bool(args, "hidden", true);
    bool gitignore = json_get_bool(args, "gitignore", true);
    for (size_t i = 0; i < targets.n; i++) {
        ttarget_t *t = &targets.v[i];
        struct stat st;
        if (stat(t->abs, &st) != 0) {
            if (errno == ENOENT || errno == ENAMETOOLONG || errno == ENOTDIR) {
                if (single) {
                    errmsg = xasprintf("Path not found: %s", scope_path);
                    goto fail;
                }
                continue;
            }
            errmsg = xasprintf("%s: %s", t->abs, strerror(errno));
            goto fail;
        }
        if (!t->has_glob && S_ISREG(st.st_mode)) {
            gitem_t it = {rel_cwd(t->abs, env->cwd, false),
                          (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000, xstrdup("")};
            vec_push(&items, it);
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (single) {
                errmsg = xasprintf("Path is not a directory: %s", t->abs);
                goto fail;
            }
            continue;
        }
        globset_t sel = {0};
        char *pat = glob_normalize(t->glob, false);
        globset_add(&sel, pat, GLOB_PATHNAME);
        gwalk_t g;
        memset(&g, 0, sizeof g);
        pthread_mutex_init(&g.mu, NULL);
        g.root = t->abs;
        g.cwd = env->cwd;
        g.sel = &sel;
        g.dir_only = t->dir_only;
        walk_opts_t wo = {0};
        wo.include_hidden = hidden;
        wo.no_ignore = !gitignore;
        wo.skip_node_modules = !strstr(pat, "node_modules");
        wo.types = t->dir_only ? WALK_DIR : (WALK_FILE | WALK_DIR | WALK_SYMLINK);
        wo.want_stat = true;
        int db = glob_depth_bound(pat);
        if (db > 0) wo.max_depth = db;
        walk(t->abs, &wo, glob_walk_cb, &g);
        pthread_mutex_destroy(&g.mu);
        free(pat);
        globset_free(&sel);
        /* newest first by millisecond, ties by path; keep the limit */
        if (g.items.n > 1) xqsort(g.items.v, g.items.n, sizeof(gitem_t), cmp_gitem_mtime);
        for (size_t k = 0; k < g.items.n; k++) {
            if ((int64_t)k < limit) vec_push(&items, g.items.v[k]);
            else {
                free(g.items.v[k].path);
                free(g.items.v[k].key);
            }
        }
        vec_free(&g.items);
    }
    /* merge: dedupe, stable re-sort by mtime */
    {
        size_t w = 0;
        for (size_t i = 0; i < items.n; i++) {
            bool dup = false;
            for (size_t k = 0; k < w && !dup; k++) dup = !strcmp(items.v[k].path, items.v[i].path);
            if (dup) {
                free(items.v[i].path);
                free(items.v[i].key);
                continue;
            }
            items.v[w++] = items.v[i];
        }
        items.n = w;
        /* insertion sort keeps equal mtimes in order (JS sort is stable) */
        for (size_t i = 1; i < items.n; i++) {
            gitem_t x = items.v[i];
            size_t k = i;
            while (k > 0 && items.v[k - 1].mtime < x.mtime) {
                items.v[k] = items.v[k - 1];
                k--;
            }
            items.v[k] = x;
        }
    }
    char *clamp = NULL;
    if (req_limit > GLOB_MAX_LIMIT) {
        char *n = js_num(req_limit);
        clamp = xasprintf("Requested limit %s clamped to the max of %d", n, GLOB_MAX_LIMIT);
        free(n);
    }
    char *missing_note = NULL;
    if (missing.n) {
        char *j = strvec_join(&missing, ", ");
        missing_note = xasprintf("Skipped missing paths: %s", j);
        free(j);
    }
    out->details = json_obj();
    json_set_str(out->details, "scopePath", scope_path);
    if (!items.n) {
        json_set_int(out->details, "fileCount", 0);
        json_set(out->details, "files", json_arr());
        json_set_bool(out->details, "truncated", false);
        json_set_str(out->details, "cwd", env->cwd);
        sb_t t = SB_INIT;
        sb_append(&t, "No files found matching pattern");
        if (missing_note) sb_appendf(&t, "\n%s", missing_note);
        tool_result_text(out, sb_cstr(&t));
        sb_free(&t);
        out->useless = true;
    } else {
        bool reached = (int64_t)items.n >= limit;
        size_t shown = reached ? (size_t)limit : items.n;
        strvec_t paths = {0};
        json_t *files = json_arr();
        for (size_t i = 0; i < shown; i++) {
            strvec_push(&paths, items.v[i].path);
            json_push_str(files, items.v[i].path);
        }
        char *base = format_grouped_paths(&paths);
        sb_t raw = SB_INIT;
        sb_append(&raw, base);
        free(base);
        if (clamp || missing_note) {
            sb_append(&raw, "\n\n");
            if (clamp) sb_append(&raw, clamp);
            if (clamp && missing_note) sb_putc(&raw, '\n');
            if (missing_note) sb_append(&raw, missing_note);
        }
        out_trunc_t tr = {0};
        out_truncate_head(raw.s ? raw.s : "", raw.len, 50 * 1024, INT64_MAX, &tr);
        tool_result_text(out, tr.content ? tr.content : "");
        json_set_int(out->details, "fileCount", (int64_t)shown);
        json_set(out->details, "files", files);
        json_set_bool(out->details, "truncated", reached || tr.truncated);
        if (reached) json_set_int(out->details, "resultLimitReached", limit);
        json_set_str(out->details, "cwd", env->cwd);
        json_t *meta = NULL;
        if (reached) {
            int64_t sug = MIN(limit * 2, (int64_t)GLOB_MAX_LIMIT);
            out_meta_result_limit(&meta, limit, sug > limit ? sug : -1);
        }
        if (tr.truncated) out_meta_from_trunc(&meta, &tr, "head", 0, 0, NULL, 0, 0);
        if (meta) json_set(out->details, "meta", meta);
        out_trunc_free(&tr);
        sb_free(&raw);
        strvec_free(&paths);
    }
    if (missing.n) {
        json_t *mp = json_arr();
        for (size_t k = 0; k < missing.n; k++) json_push_str(mp, missing.v[k]);
        json_set(out->details, "missingPaths", mp);
    }
    free(clamp);
    free(missing_note);
    goto cleanup;
fail:
    tool_result_error(out, "%s", errmsg);
    free(errmsg);
cleanup:
    for (size_t i = 0; i < items.n; i++) {
        free(items.v[i].path);
        free(items.v[i].key);
    }
    vec_free(&items);
    for (size_t i = 0; i < targets.n; i++) {
        free(targets.v[i].abs);
        free(targets.v[i].glob);
    }
    vec_free(&targets);
    free(scope_path);
    strvec_free(&scoped);
    strvec_free(&expanded);
    strvec_free(&patterns);
    strvec_free(&missing);
    strvec_free(&effective);
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
                           "\"boolean\"},\"gitignore\":{\"type\":\"boolean\"},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200}},"
                           "\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("glob", "Glob", desc, s, glob_execute);
    free(desc);
    t->ud = env;
    t->lenient_args = true; /* lenientArgValidation: glob_execute refuses unknown keys and validates the rest */
    return t;
}
