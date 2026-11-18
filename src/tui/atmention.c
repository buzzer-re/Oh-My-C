/* atmention.c - `@` file mentions and path completion (ports of omp packages/tui/src/autocomplete.ts:
 * extractAtFilePrefix, CombinedAutocompleteProvider #getFileSuggestions / #getFuzzyFileSuggestions /
 * #resolveScopedFuzzyQuery / #isOutsideCwd, buildCompletionValue, and the native fuzzyFind scorer of
 * crates/pi-natives/src/fd.rs).
 *
 * `@query` searches the whole project: a gitignore-aware walk of the cwd (hidden entries included, .git and
 * node_modules skipped, symlinked directories followed) scored by file name (exact 120, prefix 100, substring
 * 80, fuzzy 50+), or by path when the query has a '/'; directories get +10; the best 100 by score, then
 * shallower, then path. `@dir/query` scopes the search to that directory. A bare `@`, a path outside the cwd
 * (`@../`, `@~/`, `@/abs`) and a fuzzy search without hits list the immediate directory instead (directories
 * first). omp walks with a 1.5 s deadline and a stamp-validated listing cache; omc keeps the last walk of a
 * root for 2 s (typing a query re-scores it instead of walking per key) under the same deadline.
 */
#include "tui/editor.h"
#include "core/fs.h"
#include "text/walker.h"

#include <ctype.h>
#include <pthread.h>
#include <sys/stat.h>

#define AT_MAX_RESULTS 100
#define AT_TIMEOUT_MS 1500
#define WALK_CACHE_MS 2000
#define WALK_MAX_ENTRIES 400000
#define LISTING_MAX 2000

static bool is_delim(char c) { return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '='; }

static bool token_start(const char *t, size_t i) { return i == 0 || is_delim(t[i - 1]); }

/* findUnclosedQuoteStart */
static long unclosed_quote(const char *t) {
    bool in = false;
    long start = -1;
    for (size_t i = 0; t[i]; i++)
        if (t[i] == '"') {
            in = !in;
            if (in) start = (long)i;
        }
    return in ? start : -1;
}

/* extractQuotedPrefix: start of the quoted token (its `@` when it has one), or -1 */
static long quoted_prefix_start(const char *t) {
    long q = unclosed_quote(t);
    if (q < 0) return -1;
    if (q > 0 && t[q - 1] == '@') return token_start(t, (size_t)q - 1) ? q - 1 : -1;
    return token_start(t, (size_t)q) ? q : -1;
}

long tui_ac_at_prefix_start(const char *text) {
    long q = quoted_prefix_start(text);
    if (q >= 0 && text[q] == '@' && text[q + 1] == '"') return q;
    size_t n = strlen(text), start = 0;
    for (size_t i = n; i > 0; i--)
        if (is_delim(text[i - 1])) {
            start = i;
            break;
        }
    return text[start] == '@' ? (long)start : -1;
}

typedef struct {
    const char *raw;
    bool at, quoted;
} path_prefix_t;

/* parsePathPrefix */
static path_prefix_t parse_prefix(const char *p) {
    if (p[0] == '@' && p[1] == '"') return (path_prefix_t){p + 2, true, true};
    if (p[0] == '"') return (path_prefix_t){p + 1, false, true};
    if (p[0] == '@') return (path_prefix_t){p + 1, true, false};
    return (path_prefix_t){p, false, false};
}

/* buildCompletionValue (+ the space a file accept appends: applyCompletion's suffix) */
static char *completion_value(const char *path, bool dir, bool at, bool quoted) {
    bool quotes = quoted || strchr(path, ' ');
    const char *pre = at ? "@" : "";
    if (!quotes) return xasprintf("%s%s%s", pre, path, dir ? "" : " ");
    return xasprintf("%s\"%s%s", pre, path, dir ? "" : "\" ");
}

static char *bs_to_slash(const char *s) {
    char *r = xstrdup(s);
    for (char *p = r; *p; p++)
        if (*p == '\\') *p = '/';
    return r;
}

/* #expandHomePath: ~ / ~/x, keeping a trailing slash */
static char *expand_home(const char *p) {
    const char *home = home_dir();
    if (!strcmp(p, "~")) return xstrdup(home ? home : "~");
    if (strncmp(p, "~/", 2) || !home) return xstrdup(p);
    char *j = path_join(home, p + 2);
    if (str_ends(p, "/") && !str_ends(j, "/")) {
        char *t = xasprintf("%s/", j);
        free(j);
        return t;
    }
    return j;
}

static bool is_abs(const char *p) { return p[0] == '/' || (isalpha((unsigned char)p[0]) && p[1] == ':' && p[2] == '/'); }

/* ICU-ish label order: case-insensitive, lowercase first on ties */
static int label_cmp(const char *a, const char *b) {
    int c = strcasecmp(a, b);
    return c ? c : -strcmp(a, b);
}

typedef struct {
    char *value, *label, *desc;
    bool dir;
} sugg_t;
typedef VEC(sugg_t) suggvec_t;

static int sugg_cmp(const void *x, const void *y) {
    const sugg_t *a = x, *b = y;
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    return label_cmp(a->label, b->label);
}

static void sugg_free(suggvec_t *v) {
    for (size_t i = 0; i < v->n; i++) free(v->v[i].value), free(v->v[i].label), free(v->v[i].desc);
    vec_free(v);
}

/* #getFileSuggestions: entries of the prefix's directory whose name starts with its last segment */
static void listing(const char *base, const char *prefix, suggvec_t *out) {
    path_prefix_t pp = parse_prefix(prefix);
    char *exp = bs_to_slash(pp.raw);
    char *pre = xstrdup(exp);
    if (exp[0] == '~') {
        char *e = expand_home(exp);
        free(exp);
        exp = e;
    }
    bool root = !*pre || !strcmp(pre, "./") || !strcmp(pre, "../") || !strcmp(pre, "~") || !strcmp(pre, "~/") ||
                !strcmp(pre, "/");
    char *dir, *want;
    if (root || str_ends(exp, "/")) {
        dir = exp[0] == '~' || is_abs(exp) ? xstrdup(exp) : path_join(base, exp);
        want = xstrdup("");
    } else {
        char *d = path_dirname(exp);
        dir = exp[0] == '~' || is_abs(exp) ? xstrdup(d) : path_join(base, d);
        want = xstrdup(path_basename(exp));
        free(d);
    }
    dirlist_t dl = {0};
    if (fs_list_dir(dir, &dl) == 0) {
        char *lw = str_lower(want);
        char *disp = bs_to_slash(pp.raw);
        for (size_t i = 0; i < dl.n && out->n < LISTING_MAX; i++) {
            dirent_info_t *e = &dl.v[i];
            if (!strcmp(e->name, ".git")) continue;
            char *ln = str_lower(e->name);
            bool match = str_starts(ln, lw);
            free(ln);
            if (!match) continue;
            if (e->is_link && !e->is_dir) { /* a broken link (or one deleted meanwhile) is dropped */
                char *full = path_join(dir, e->name);
                struct stat st;
                bool ok = stat(full, &st) == 0;
                free(full);
                if (!ok) continue;
            }
            char *rel;
            if (str_ends(disp, "/")) rel = xasprintf("%s%s", disp, e->name);
            else if (strchr(disp, '/')) {
                if (!strncmp(disp, "~/", 2)) {
                    char *d = path_dirname(disp + 2);
                    rel = !strcmp(d, ".") ? xasprintf("~/%s", e->name) : xasprintf("~/%s/%s", d, e->name);
                    free(d);
                } else if (is_abs(disp)) {
                    const char *ls = strrchr(disp, '/');
                    size_t dl2 = (size_t)(ls - disp);
                    rel = dl2 == 0 ? xasprintf("/%s", e->name) : xasprintf("%.*s/%s", (int)dl2, disp, e->name);
                } else {
                    char *d = path_dirname(disp);
                    char *j = !strcmp(d, ".") ? xstrdup(e->name) : xasprintf("%s/%s", d, e->name);
                    char *nj = path_normalize(j);
                    if (!strncmp(disp, "./", 2) && strncmp(nj, "./", 2)) {
                        rel = xasprintf("./%s", nj);
                        free(nj);
                    } else rel = nj;
                    free(j), free(d);
                }
            } else if (disp[0] == '~') rel = xasprintf("~/%s", e->name);
            else rel = xstrdup(e->name);
            char *pv = e->is_dir ? xasprintf("%s/", rel) : xstrdup(rel);
            sugg_t s = {completion_value(pv, e->is_dir, pp.at, pp.quoted), xasprintf("%s%s", e->name, e->is_dir ? "/" : ""),
                        NULL, e->is_dir};
            vec_push(out, s);
            free(pv), free(rel);
        }
        free(lw);
        free(disp);
    }
    dirlist_free(&dl);
    if (out->n) xqsort(out->v, out->n, sizeof(sugg_t), sugg_cmp);
    free(dir), free(want), free(exp), free(pre);
}

/* ---------- native fuzzyFind ---------- */

typedef struct {
    char *path; /* relative, '/'-separated, no trailing slash */
    bool dir;
} fentry_t;
typedef VEC(fentry_t) fentries_t;

typedef struct {
    pthread_mutex_t mu;
    fentries_t *list;
    int64_t deadline;
    bool stopped;
} walk_ctx_t;

static bool walk_cb(const walk_entry_t *e, void *ud) {
    walk_ctx_t *w = ud;
    pthread_mutex_lock(&w->mu);
    bool go = !w->stopped;
    if (go && (w->list->n >= WALK_MAX_ENTRIES || mono_ms() > w->deadline)) go = false, w->stopped = true;
    if (go) {
        fentry_t f = {xstrdup(e->path), e->type == WALK_DIR};
        vec_push(w->list, f);
    }
    pthread_mutex_unlock(&w->mu);
    return go;
}

static struct {
    char *root;
    int64_t at;
    fentries_t list;
} g_walk;
static pthread_mutex_t g_walk_mu = PTHREAD_MUTEX_INITIALIZER;

static void fentries_free(fentries_t *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i].path);
    vec_free(l);
}

/* the walk of `root` (shared for WALK_CACHE_MS); caller holds g_walk_mu */
static const fentries_t *walk_root(const char *root) {
    int64_t now = mono_ms();
    if (g_walk.root && !strcmp(g_walk.root, root) && now - g_walk.at < WALK_CACHE_MS) return &g_walk.list;
    fentries_free(&g_walk.list);
    memset(&g_walk.list, 0, sizeof g_walk.list);
    free(g_walk.root);
    g_walk.root = xstrdup(root);
    walk_opts_t o = {0};
    o.include_hidden = true;
    o.follow_symlinks = true;
    o.skip_node_modules = true;
    o.types = WALK_FILE | WALK_DIR;
    walk_ctx_t w = {PTHREAD_MUTEX_INITIALIZER, &g_walk.list, now + AT_TIMEOUT_MS, false};
    walk(root, &o, walk_cb, &w);
    pthread_mutex_destroy(&w.mu);
    g_walk.at = mono_ms();
    return &g_walk.list;
}

/* lowercase without whitespace and path punctuation (normalize_fuzzy_into) */
static char *fuzzy_norm(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c) || c == '/' || c == '\\' || c == '.' || c == '_' || c == '-') continue;
        sb_putc(&b, (char)tolower(c));
    }
    return sb_detach(&b);
}

/* fuzzy_subsequence_score over normalized forms: 40 - 5 per gap, min 1; 0 = no match */
static int fuzzy_subseq(const char *q, const char *t) {
    if (!*q) return 1;
    size_t qi = 0, ql = strlen(q);
    int gaps = 0;
    long last = -1;
    for (size_t ti = 0; t[ti] && qi < ql; ti++)
        if (q[qi] == t[ti]) {
            if (last >= 0 && (long)ti > last + 1) gaps++;
            last = (long)ti;
            qi++;
        }
    if (qi != ql) return 0;
    int s = 40 - gaps * 5;
    return s < 1 ? 1 : s;
}

typedef struct {
    const char *lower, *norm;
    bool slash;
} fquery_t;

static int score_path(const char *path, bool dir, const fquery_t *q) {
    if (!*q->lower) return dir ? 11 : 1;
    const char *name = path_basename(path);
    char *ln = str_lower(name);
    int score;
    if (!strcmp(ln, q->lower)) score = 120;
    else if (str_starts(ln, q->lower)) score = 100;
    else if (strstr(ln, q->lower)) score = 80;
    else {
        char *nn = fuzzy_norm(name);
        int f = fuzzy_subseq(q->norm, nn);
        free(nn);
        if (!q->slash) score = f > 0 ? 50 + f : 0;
        else {
            char *lp = str_lower(path);
            bool sub = strstr(lp, q->lower) != NULL;
            free(lp);
            if (sub) score = 60;
            else if (f > 0) score = 50 + f;
            else {
                char *np = fuzzy_norm(path);
                int pf = !strcmp(np, q->norm) ? 40 : fuzzy_subseq(q->norm, np);
                free(np);
                score = pf > 0 ? 30 + pf : 0;
            }
        }
    }
    free(ln);
    if (dir && score > 0) score += 10;
    return score;
}

typedef struct {
    const fentry_t *e;
    int score, depth;
} ranked_t;

static int depth_of(const char *p) {
    int d = 0;
    for (; *p; p++) d += *p == '/';
    return d;
}

/* score desc, then shallower, then path (a directory compares with its trailing '/') */
static int ranked_cmp(const void *x, const void *y) {
    const ranked_t *a = x, *b = y;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    if (a->depth != b->depth) return a->depth < b->depth ? -1 : 1;
    const unsigned char *p = (const unsigned char *)a->e->path, *r = (const unsigned char *)b->e->path;
    while (*p && *p == *r) p++, r++;
    int ca = *p ? *p : a->e->dir ? '/' : -1, cb = *r ? *r : b->e->dir ? '/' : -1;
    if (ca != cb) return ca < cb ? -1 : 1;
    if (!*p && !*r) return 0;
    return *p ? 1 : -1;
}

/* subsequenceMatch */
static bool subseq(const char *q, const char *t) {
    size_t qi = 0, ql = strlen(q);
    if (!ql) return true;
    if (ql > strlen(t)) return false;
    for (size_t ti = 0; t[ti] && qi < ql; ti++)
        if (q[qi] == t[ti]) qi++;
    return qi == ql;
}

static bool has_git_component(const char *p) {
    for (const char *s = p; (s = strstr(s, ".git"));) {
        bool start = s == p || s[-1] == '/';
        bool end = s[4] == 0 || s[4] == '/';
        if (start && end) return true;
        s += 4;
    }
    return false;
}

/* #getFuzzyFileSuggestions */
static void fuzzy(const char *base, const char *raw, bool quoted, suggvec_t *out) {
    /* #resolveScopedFuzzyQuery: `dir/query` searches inside dir when it is a directory */
    char *display_base = NULL, *search = NULL;
    const char *query = raw;
    const char *slash = strrchr(raw, '/');
    if (slash) {
        display_base = xstrndup(raw, (size_t)(slash - raw) + 1);
        char *b = display_base[0] == '~' ? expand_home(display_base)
                  : display_base[0] == '/' ? xstrdup(display_base)
                                           : path_join(base, display_base);
        struct stat st;
        if (stat(b, &st) == 0 && S_ISDIR(st.st_mode)) {
            search = b;
            query = slash + 1;
        } else {
            free(b);
            free(display_base);
            display_base = NULL;
        }
    }
    if (!search) search = xstrdup(base);
    char *trimmed = str_trim(query), *lower = str_lower(trimmed), *norm = fuzzy_norm(lower);
    fquery_t q = {lower, norm, strchr(lower, '/') != NULL};
    if (*lower && !*norm) goto done; /* nothing left to match */
    pthread_mutex_lock(&g_walk_mu);
    const fentries_t *all = walk_root(search);
    VEC(ranked_t) top = {0};
    for (size_t i = 0; i < all->n; i++) {
        int s = score_path(all->v[i].path, all->v[i].dir, &q);
        if (s <= 0) continue;
        ranked_t r = {&all->v[i], s, depth_of(all->v[i].path)};
        vec_push(&top, r);
    }
    if (top.n) xqsort(top.v, top.n, sizeof(ranked_t), ranked_cmp);
    char *lq = str_lower(query);
    for (size_t i = 0; i < top.n && i < AT_MAX_RESULTS; i++) {
        const fentry_t *e = top.v[i].e;
        if (has_git_component(e->path)) continue;
        char *lp = str_lower(e->path);
        bool keep = !*lq || subseq(lq, lp);
        free(lp);
        if (!keep) continue;
        char *disp = display_base ? (!strcmp(display_base, "/") ? xasprintf("/%s", e->path)
                                                                : xasprintf("%s%s", display_base, e->path))
                                  : xstrdup(e->path);
        char *cp = e->dir ? xasprintf("%s/", disp) : xstrdup(disp);
        sugg_t s = {completion_value(cp, e->dir, true, quoted),
                    xasprintf("%s%s", path_basename(disp), e->dir ? "/" : ""), xstrdup(disp), e->dir};
        vec_push(out, s);
        free(cp), free(disp);
    }
    free(lq);
    vec_free(&top);
    pthread_mutex_unlock(&g_walk_mu);
done:
    free(trimmed), free(lower), free(norm), free(search), free(display_base);
}

/* #isOutsideCwd: a lexical resolve of the prefix leaves the cwd */
static bool outside_cwd(const char *base, const char *raw) {
    if (!*raw) return false;
    char *target;
    if (raw[0] == '~') target = expand_home(raw);
    else if (is_abs(raw)) target = xstrdup(raw);
    else target = path_join(base, raw);
    char *nt = path_normalize(target), *nb = path_normalize(base);
    size_t bl = strlen(nb);
    bool inside = !strcmp(nt, nb) || (!strncmp(nt, nb, bl) && (nt[bl] == '/' || (bl && nb[bl - 1] == '/')));
    free(target), free(nt), free(nb);
    return !inside;
}

static bool emit(suggvec_t *v, size_t prefix_len, tui_ac_result_t *out) {
    for (size_t i = 0; i < v->n; i++) tui_ac_result_add(out, v->v[i].value, v->v[i].label, v->v[i].desc);
    bool any = v->n > 0;
    if (any) out->prefix_len = prefix_len;
    sugg_free(v);
    return any;
}

bool tui_ac_at_suggestions(const char *cwd, const char *at_token, tui_ac_result_t *out) {
    char *base = cwd ? xstrdup(cwd) : get_cwd();
    if (!base) base = xstrdup(".");
    path_prefix_t pp = parse_prefix(at_token);
    suggvec_t v = {0};
    if (*pp.raw && outside_cwd(base, pp.raw)) listing(base, at_token, &v);
    else if (!*pp.raw) listing(base, "@", &v);
    else {
        fuzzy(base, pp.raw, pp.quoted, &v);
        if (!v.n) listing(base, at_token, &v);
    }
    free(base);
    return emit(&v, strlen(at_token), out);
}

/* getForceFileSuggestions (Tab on a plain path token) */
bool tui_ac_path_suggestions(const char *cwd, const char *token, tui_ac_result_t *out) {
    char *base = cwd ? xstrdup(cwd) : get_cwd();
    if (!base) base = xstrdup(".");
    suggvec_t v = {0};
    listing(base, token, &v);
    /* plain path tokens complete without the accept-time space (applyCompletion's path tail) */
    for (size_t i = 0; i < v.n; i++) {
        size_t n = strlen(v.v[i].value);
        if (n && v.v[i].value[n - 1] == ' ') v.v[i].value[n - 1] = 0;
    }
    free(base);
    return emit(&v, strlen(token), out);
}
