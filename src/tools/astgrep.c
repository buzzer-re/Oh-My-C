/* astgrep.c - structural search/rewrite over a bracket-aware token tree (ast_grep, ast_edit). */
#define _GNU_SOURCE /* memmem, strcasestr, vasprintf: glibc before 2.38 declares them only with it */
#include "tools.h"
#include "toolcmd.h"

#include "core/fs.h"
#include "diff.h"
#include "text/glob.h"
#include "text/walker.h"

#include <ctype.h>

/* ---------- tokenizer ---------- */

typedef enum { T_IDENT, T_NUM, T_STR, T_PUNCT, T_OPEN, T_CLOSE, T_META, T_META_MULTI } tkind_t;

typedef struct {
    tkind_t kind;
    size_t start, end; /* byte offsets in the source */
    char *text;        /* token text (meta: name without $) */
} tok_t;

typedef VEC(tok_t) toks_t;

typedef struct unit unit_t;
struct unit {
    bool group;
    tok_t tok;            /* token (or opening bracket for a group) */
    tok_t close;          /* closing bracket for a group */
    VEC(unit_t *) kids;   /* group children */
};

typedef VEC(unit_t *) units_t;

typedef struct {
    bool hash_comments, dash_comments, slash_comments, triple_quotes;
} lang_t;

static lang_t lang_for(const char *path, const char *override) {
    const char *e = override ? override : path_ext(path);
    if (*e == '.') e++;
    lang_t l = {.slash_comments = true};
    if (!strcasecmp(e, "py") || !strcasecmp(e, "python") || !strcasecmp(e, "pyi")) {
        l.slash_comments = false;
        l.hash_comments = true;
        l.triple_quotes = true;
    } else if (!strcasecmp(e, "rb") || !strcasecmp(e, "sh") || !strcasecmp(e, "bash") || !strcasecmp(e, "yaml") ||
               !strcasecmp(e, "yml") || !strcasecmp(e, "toml") || !strcasecmp(e, "r") || !strcasecmp(e, "nim")) {
        l.slash_comments = false;
        l.hash_comments = true;
    } else if (!strcasecmp(e, "lua") || !strcasecmp(e, "sql") || !strcasecmp(e, "hs")) {
        l.slash_comments = false;
        l.dash_comments = true;
    } else if (!strcasecmp(e, "php")) {
        l.hash_comments = true;
    }
    return l;
}

static bool lang_supported(const char *path) {
    static const char *const exts[] = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".cu", ".cuh", ".js", ".jsx", ".mjs",
                                       ".cjs", ".ts", ".tsx", ".mts", ".cts", ".java", ".go", ".rs", ".cs", ".swift",
                                       ".kt", ".kts", ".scala", ".php", ".dart", ".py", ".pyi", ".rb", ".lua", ".zig",
                                       ".m", ".mm", ".sh", ".bash", ".sql", ".css", ".scss", ".vue", ".svelte", NULL};
    const char *e = path_ext(path);
    for (size_t i = 0; exts[i]; i++)
        if (!strcasecmp(e, exts[i])) return true;
    return false;
}

static const char *const MULTI_PUNCT[] = {">>>=", "<<=", ">>=", "...", "===", "!==", "**=", "&&=", "||=", "?" "?=", "::",
                                          "->", "=>", "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=",
                                          "/=", "%=", "&=", "|=", "^=", "<<", ">>", "??", "?.", "**", ":=", NULL};

static void tokenize(const char *s, size_t n, lang_t L, bool pattern, toks_t *out) {
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (isspace((unsigned char)c)) {
            i++;
            continue;
        }
        if (!pattern) {
            if (L.slash_comments && c == '/' && i + 1 < n && s[i + 1] == '/') {
                while (i < n && s[i] != '\n') i++;
                continue;
            }
            if (L.slash_comments && c == '/' && i + 1 < n && s[i + 1] == '*') {
                const char *e = memmem(s + i + 2, n - i - 2, "*/", 2);
                i = e ? (size_t)(e - s) + 2 : n;
                continue;
            }
            if (L.hash_comments && c == '#') {
                while (i < n && s[i] != '\n') i++;
                continue;
            }
            if (L.dash_comments && c == '-' && i + 1 < n && s[i + 1] == '-') {
                while (i < n && s[i] != '\n') i++;
                continue;
            }
        }
        tok_t t = {.start = i};
        if (pattern && c == '$') {
            size_t j = i + 1;
            bool multi = j + 1 < n && s[j] == '$' && s[j + 1] == '$';
            if (multi) j += 2;
            size_t k = j;
            while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '_')) k++;
            t.kind = multi ? T_META_MULTI : T_META;
            t.text = xstrndup(s + j, k - j); /* "" for bare $$$ */
            t.end = k;
            vec_push(out, t);
            i = k;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_' || c == '$' || (unsigned char)c >= 0x80) {
            size_t k = i;
            while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '_' || s[k] == '$' || (unsigned char)s[k] >= 0x80)) k++;
            t.kind = T_IDENT;
            t.end = k;
        } else if (isdigit((unsigned char)c)) {
            size_t k = i;
            while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '.' || s[k] == '_')) k++;
            t.kind = T_NUM;
            t.end = k;
        } else if (c == '"' || c == '\'' || c == '`') {
            size_t k = i + 1;
            if (L.triple_quotes && k + 1 < n && s[k] == c && s[k + 1] == c) {
                const char q3[4] = {c, c, c, 0};
                const char *e = memmem(s + k + 2, n - k - 2, q3, 3);
                k = e ? (size_t)(e - s) + 3 : n;
            } else {
                while (k < n && s[k] != c) {
                    if (s[k] == '\\' && k + 1 < n) k++;
                    else if (s[k] == '\n' && c != '`') break;
                    k++;
                }
                if (k < n) k++;
            }
            t.kind = T_STR;
            t.end = k;
        } else if (strchr("([{", c)) {
            t.kind = T_OPEN;
            t.end = i + 1;
        } else if (strchr(")]}", c)) {
            t.kind = T_CLOSE;
            t.end = i + 1;
        } else {
            t.kind = T_PUNCT;
            t.end = i + 1;
            for (size_t m = 0; MULTI_PUNCT[m]; m++) {
                size_t l = strlen(MULTI_PUNCT[m]);
                if (i + l <= n && !memcmp(s + i, MULTI_PUNCT[m], l)) {
                    t.end = i + l;
                    break;
                }
            }
        }
        t.text = xstrndup(s + t.start, t.end - t.start);
        vec_push(out, t);
        i = t.end;
    }
}

static char closer_of(char o) { return o == '(' ? ')' : o == '[' ? ']' : '}'; }

static units_t build(toks_t *toks, size_t *pos, int depth) {
    units_t out = {0};
    while (*pos < toks->n) {
        tok_t *t = &toks->v[*pos];
        if (t->kind == T_CLOSE) return out;
        (*pos)++;
        unit_t *u = xcalloc(1, sizeof(*u));
        u->tok = *t;
        if (t->kind == T_OPEN && depth < 200) {
            u->group = true;
            units_t kids = build(toks, pos, depth + 1);
            u->kids.v = kids.v;
            u->kids.n = kids.n;
            u->kids.cap = kids.cap;
            if (*pos < toks->n && toks->v[*pos].kind == T_CLOSE && toks->v[*pos].text[0] == closer_of(t->text[0])) {
                u->close = toks->v[*pos];
                (*pos)++;
            } else {
                u->close = (tok_t){T_CLOSE, u->kids.n ? u->kids.v[u->kids.n - 1]->tok.end : t->end, 0, NULL};
            }
        }
        vec_push(&out, u);
    }
    return out;
}

static void units_free(units_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        units_t k = {v->v[i]->kids.v, v->v[i]->kids.n, v->v[i]->kids.cap};
        units_free(&k);
        free(v->v[i]);
    }
    free(v->v);
}

static size_t u_start(const unit_t *u) { return u->tok.start; }
static size_t u_end(const unit_t *u) { return u->group ? u->close.end : u->tok.end; }

/* ---------- matcher ---------- */

typedef struct {
    char *name;
    size_t start, end; /* byte span in source (start==end for empty multi) */
} capture_t;

typedef struct {
    const char *src;
    VEC(capture_t) caps;
} mctx_t;

static bool is_sep(const unit_t *u) { return !u->group && u->tok.kind == T_PUNCT && (!strcmp(u->tok.text, ",") || !strcmp(u->tok.text, ";")); }

/* normalized text of a source span (whitespace collapsed) for capture equality */
static char *norm_span(const char *src, size_t a, size_t b) {
    sb_t s = SB_INIT;
    bool sp = false;
    for (size_t i = a; i < b; i++) {
        if (isspace((unsigned char)src[i])) sp = true;
        else {
            if (sp && s.len) sb_putc(&s, ' ');
            sp = false;
            sb_putc(&s, src[i]);
        }
    }
    return sb_detach(&s);
}

static bool mv_bind(mctx_t *m, const char *name, size_t a, size_t b) {
    if (!*name || !strcmp(name, "_")) return true;
    for (size_t i = 0; i < m->caps.n; i++) {
        if (!strcmp(m->caps.v[i].name, name)) {
            char *x = norm_span(m->src, m->caps.v[i].start, m->caps.v[i].end), *y = norm_span(m->src, a, b);
            bool eq = !strcmp(x, y);
            free(x);
            free(y);
            return eq;
        }
    }
    capture_t c = {xstrdup(name), a, b};
    vec_push(&m->caps, c);
    return true;
}

static void unbind_to(mctx_t *m, size_t n) {
    while (m->caps.n > n) free(m->caps.v[--m->caps.n].name);
}

static bool match_seq(mctx_t *m, unit_t **pat, size_t np, unit_t **src, size_t ns, size_t *consumed, bool whole, int depth);

static bool match_unit(mctx_t *m, const unit_t *p, const unit_t *s, int depth) {
    if (p->group != s->group) return false;
    if (!p->group) {
        if (p->tok.kind != s->tok.kind && !(p->tok.kind == T_IDENT && s->tok.kind == T_IDENT)) return false;
        return !strcmp(p->tok.text, s->tok.text);
    }
    if (p->tok.text[0] != s->tok.text[0]) return false;
    size_t c;
    return match_seq(m, p->kids.v, p->kids.n, s->kids.v, s->kids.n, &c, true, depth + 1);
}

/* match pattern units against source units; whole => must consume all of src */
static bool match_seq(mctx_t *m, unit_t **pat, size_t np, unit_t **src, size_t ns, size_t *consumed, bool whole, int depth) {
    if (depth > 100) return false;
    if (np == 0) {
        *consumed = 0;
        return !whole || ns == 0;
    }
    const unit_t *p = pat[0];
    size_t mark = m->caps.n;
    if (!p->group && (p->tok.kind == T_META || p->tok.kind == T_META_MULTI)) {
        bool multi = p->tok.kind == T_META_MULTI;
        size_t min = multi ? 0 : 1;
        /* single-node metavariable: no top-level separators; try shortest first */
        for (size_t k = min; k <= ns; k++) {
            if (!multi && k > 0 && is_sep(src[k - 1])) break;
            size_t a = k ? u_start(src[0]) : (ns ? u_start(src[0]) : 0);
            size_t b = k ? u_end(src[k - 1]) : a;
            if (!mv_bind(m, p->tok.text, a, b)) {
                unbind_to(m, mark);
                continue;
            }
            size_t c2;
            if (match_seq(m, pat + 1, np - 1, src + k, ns - k, &c2, whole, depth + 1)) {
                *consumed = k + c2;
                return true;
            }
            unbind_to(m, mark);
        }
        return false;
    }
    if (ns == 0) return false;
    if (!match_unit(m, p, src[0], depth)) {
        unbind_to(m, mark);
        return false;
    }
    size_t c2;
    if (match_seq(m, pat + 1, np - 1, src + 1, ns - 1, &c2, whole, depth + 1)) {
        *consumed = 1 + c2;
        return true;
    }
    unbind_to(m, mark);
    return false;
}

typedef struct {
    size_t start, end;
    VEC(capture_t) caps;
} match_t;

typedef VEC(match_t) matches_t;

static void find_all(const char *src, units_t *pat, unit_t **seq, size_t n, matches_t *out, int depth) {
    if (depth > 200) return;
    size_t i = 0;
    while (i < n) {
        mctx_t m = {src, {0}};
        size_t consumed = 0;
        if (match_seq(&m, pat->v, pat->n, seq + i, n - i, &consumed, false, 0) && consumed > 0) {
            match_t mt = {u_start(seq[i]), u_end(seq[i + consumed - 1]), {m.caps.v, m.caps.n, m.caps.cap}};
            vec_push(out, mt);
            i += consumed; /* non-overlapping */
            continue;
        }
        unbind_to(&m, 0);
        vec_free(&m.caps);
        if (seq[i]->group) find_all(src, pat, seq[i]->kids.v, seq[i]->kids.n, out, depth + 1);
        i++;
    }
}

static void matches_free(matches_t *ms) {
    for (size_t i = 0; i < ms->n; i++) {
        for (size_t k = 0; k < ms->v[i].caps.n; k++) free(ms->v[i].caps.v[k].name);
        vec_free(&ms->v[i].caps);
    }
    vec_free(ms);
}

/* compile a pattern into units (owned tokens array kept for text lifetime) */
typedef struct {
    toks_t toks;
    units_t units;
} cpat_t;

static int pat_compile(const char *pat, cpat_t *cp, char **err) {
    memset(cp, 0, sizeof(*cp));
    lang_t L = {.slash_comments = false};
    tokenize(pat, strlen(pat), L, true, &cp->toks);
    size_t pos = 0;
    cp->units = build(&cp->toks, &pos, 0);
    if (pos != cp->toks.n || !cp->units.n) {
        *err = xstrdup(cp->units.n ? "pattern has unbalanced brackets" : "empty pattern");
        return -1;
    }
    for (size_t i = 0; i < cp->toks.n; i++) {
        tok_t *t = &cp->toks.v[i];
        if (t->kind == T_META && *t->text && !strcmp(t->text, "_")) continue;
        if ((t->kind == T_META || t->kind == T_META_MULTI) && *t->text) {
            for (const char *c = t->text; *c; c++)
                if (islower((unsigned char)*c)) {
                    *err = xasprintf("metavariable names must be UPPERCASE: $%s", t->text);
                    return -1;
                }
        }
    }
    return 0;
}

static void pat_free(cpat_t *cp) {
    units_free(&cp->units);
    for (size_t i = 0; i < cp->toks.n; i++) free(cp->toks.v[i].text);
    vec_free(&cp->toks);
}

/* collect candidate files from a path spec ("a; b; !c") */
static void collect_files(tool_env_t *env, const char *spec, strvec_t *files) {
    strvec_t parts = str_split(spec && *spec ? spec : ".", ";");
    globset_t exc = {0};
    for (size_t i = 0; i < parts.n; i++) {
        char *t = str_trim(parts.v[i]);
        if (t[0] == '!') globset_add(&exc, t + 1, 0);
        free(t);
    }
    for (size_t i = 0; i < parts.n; i++) {
        char *t = str_trim(parts.v[i]);
        if (!*t || t[0] == '!') {
            free(t);
            continue;
        }
        char *abs = tool_resolve_path(env, t);
        if (fs_is_file(abs)) {
            strvec_push(files, abs);
        } else {
            char *base = abs, *glob = NULL;
            if (!fs_exists(abs) && glob_has_magic(t)) {
                /* split at first magic segment */
                char *slash = NULL;
                for (char *p = t; *p; p++) {
                    if (*p == '/') slash = p;
                    if (strchr("*?[{", *p)) break;
                }
                char *b2 = slash ? xstrndup(t, (size_t)(slash - t)) : xstrdup(".");
                glob = xstrdup(slash ? slash + 1 : t);
                base = tool_resolve_path(env, b2);
                free(b2);
                free(abs);
                abs = NULL;
            }
            globset_t inc = {0};
            if (glob) {
                char *norm = glob_normalize(glob, !strchr(glob, '/'));
                globset_add(&inc, norm, 0);
                free(norm);
            }
            walk_opts_t wo = {0};
            wo.types = WALK_FILE;
            wo.include = glob ? &inc : NULL;
            wo.exclude = exc.pats.n ? &exc : NULL;
            wo.skip_node_modules = true;
            walk_list_t wl;
            if (walk_collect(base, &wo, &wl) == 0) {
                for (size_t k = 0; k < wl.n && files->n < 5000; k++) strvec_push_own(files, path_join(base, wl.v[k].path));
                walk_list_free(&wl);
            }
            globset_free(&inc);
            free(glob);
            free(base);
            abs = NULL;
        }
        free(abs);
        free(t);
    }
    globset_free(&exc);
    strvec_free(&parts);
}

static int64_t line_of(const char *s, size_t off) {
    int64_t l = 1;
    for (size_t i = 0; i < off; i++)
        if (s[i] == '\n') l++;
    return l;
}

/* ---------- ast_grep ---------- */

static int astgrep_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(ctx);
    const char *pat = json_get_str(args, "pat");
    char *pt = pat ? str_trim(pat) : NULL;
    if (!pt || !*pt) {
        free(pt);
        tool_result_error(out, "pat must not be empty");
        return 0;
    }
    cpat_t cp;
    char *err = NULL;
    if (pat_compile(pt, &cp, &err) != 0) {
        tool_result_error(out, "Invalid pattern: %s", err);
        free(err);
        pat_free(&cp);
        free(pt);
        return 0;
    }
    const char *lang = json_get_str(args, "lang");
    int64_t skip = json_get_int(args, "skip", 0);
    strvec_t files = {0};
    collect_files(env, json_get_str(args, "path"), &files);
    sb_t b = SB_INIT;
    size_t total = 0, shown = 0, nfiles = 0;
    const size_t LIMIT = 100;
    json_t *dfiles = json_arr(); /* omp displayContent source: [{path, lines}] */
    for (size_t f = 0; f < files.n; f++) {
        if (!lang && !lang_supported(files.v[f])) continue;
        size_t n;
        char *src = fs_read_limit(files.v[f], 4u << 20, &n, NULL);
        if (!src || fs_is_binary(src, n)) {
            free(src);
            continue;
        }
        toks_t toks = {0};
        tokenize(src, n, lang_for(files.v[f], lang), false, &toks);
        size_t pos = 0;
        units_t root = {0};
        while (pos < toks.n) {
            units_t part = build(&toks, &pos, 0);
            for (size_t i = 0; i < part.n; i++) vec_push(&root, part.v[i]);
            free(part.v);
            if (pos < toks.n) pos++; /* stray closer */
        }
        matches_t ms = {0};
        find_all(src, &cp.units, root.v, root.n, &ms, 0);
        if (ms.n) {
            nfiles++;
            char *disp = path_display(files.v[f], env->cwd);
            bool header = false;
            json_t *dlines = NULL;
            for (size_t i = 0; i < ms.n; i++) {
                total++;
                if ((int64_t)total <= skip || shown >= LIMIT) continue;
                if (!header) {
                    sb_appendf(&b, "%s%s\n", b.len ? "\n" : "", disp);
                    header = true;
                    json_t *df = json_obj();
                    json_set_str(df, "path", disp);
                    dlines = json_arr();
                    json_set(df, "lines", dlines);
                    json_push(dfiles, df);
                }
                shown++;
                int64_t l1 = line_of(src, ms.v[i].start), l2 = line_of(src, ms.v[i].end);
                {
                    /* display rows: the whole matched text from its first line's start */
                    size_t ls = ms.v[i].start;
                    while (ls > 0 && src[ls - 1] != '\n') ls--;
                    size_t le = ms.v[i].end;
                    while (le < n && src[le] != '\n') le++;
                    strvec_t ml = str_split_lines(src + ls, le - ls);
                    for (size_t k = 0; k < ml.n && k < 50; k++) {
                        json_t *dl = json_obj();
                        json_set_int(dl, "line", l1 + (int64_t)k);
                        json_set_bool(dl, "match", k == 0);
                        json_set_str(dl, "text", ml.v[k]);
                        json_push(dlines, dl);
                    }
                    strvec_free(&ml);
                    if (ms.v[i].caps.n) {
                        sb_t mb = SB_INIT;
                        sb_append(&mb, "  meta: ");
                        for (size_t k = 0; k < ms.v[i].caps.n; k++) {
                            capture_t *c = &ms.v[i].caps.v[k];
                            sb_appendf(&mb, "%s%s=%.*s", k ? ", " : "", c->name, (int)MIN(c->end - c->start, (size_t)120), src + c->start);
                        }
                        /* singleLine: a capture can span lines; its display row stays one row */
                        {
                            sb_t one = SB_INIT;
                            for (const char *p = sb_cstr(&mb); *p; p++) {
                                if (*p == '\r' || *p == '\n') {
                                    while (p[1] == '\r' || p[1] == '\n') p++;
                                    sb_putc(&one, ' ');
                                } else if (*p == '\t') sb_append(&one, "   "); /* replaceTabs */
                                else sb_putc(&one, *p);
                            }
                            sb_clear(&mb);
                            sb_append(&mb, sb_cstr(&one));
                            sb_free(&one);
                        }
                        json_t *dl = json_obj();
                        json_set_int(dl, "line", 0);
                        json_set_str(dl, "text", sb_cstr(&mb));
                        json_push(dlines, dl);
                        sb_free(&mb);
                    }
                }
                char *text = xstrndup(src + ms.v[i].start, MIN(ms.v[i].end - ms.v[i].start, (size_t)300));
                char *nl = strchr(text, '\n');
                if (nl) { /* " …" is longer than the "\n" it replaces: a match ending in a newline overflowed in place */
                    char *t2 = xasprintf("%.*s \xe2\x80\xa6", (int)(nl - text), text);
                    free(text);
                    text = t2;
                }
                if (l2 > l1) sb_appendf(&b, "  %lld-%lld: %s\n", (long long)l1, (long long)l2, text);
                else sb_appendf(&b, "  %lld: %s\n", (long long)l1, text);
                for (size_t k = 0; k < ms.v[i].caps.n; k++) {
                    capture_t *c = &ms.v[i].caps.v[k];
                    char *cv = xstrndup(src + c->start, MIN(c->end - c->start, (size_t)120));
                    for (char *p = cv; *p; p++)
                        if (*p == '\n') *p = ' ';
                    sb_appendf(&b, "    $%s = %s\n", c->name, cv);
                    free(cv);
                }
                free(text);
            }
            free(disp);
        }
        matches_free(&ms);
        units_free(&root);
        for (size_t i = 0; i < toks.n; i++) free(toks.v[i].text);
        vec_free(&toks);
        free(src);
    }
    if (!total) {
        sb_appendf(&b, "No matches found (%zu files scanned)", files.n);
        out->useless = true;
    } else {
        sb_appendf(&b, "\n%zu match%s in %zu file%s", total, total == 1 ? "" : "es", nfiles, nfiles == 1 ? "" : "s");
        if (total > (size_t)skip + shown) sb_appendf(&b, "; showing %zu, use skip: %lld for more", shown, (long long)(skip + (int64_t)shown));
    }
    tool_result_text(out, sb_cstr(&b));
    out->details = json_obj();
    json_set_int(out->details, "matchCount", (int64_t)total);
    json_set_int(out->details, "fileCount", (int64_t)nfiles);
    json_set_int(out->details, "filesSearched", (int64_t)files.n);
    json_set_bool(out->details, "limitReached", total > (size_t)skip + shown);
    if (json_get_str(args, "path")) json_set_str(out->details, "scopePath", json_get_str(args, "path"));
    json_set_str(out->details, "cwd", env->cwd);
    if (json_len(dfiles)) {
        /* a directory scope groups by path (omp formatFileMatches isDirectory) */
        bool single = json_get_str(args, "path") && files.n == 1 && str_ends(files.v[0], json_get_str(args, "path"));
        char *dc = grep_display_content(dfiles, !single);
        json_set_str(out->details, "displayContent", dc);
        free(dc);
    }
    json_free(dfiles);
    sb_free(&b);
    strvec_free(&files);
    pat_free(&cp);
    free(pt);
    return 0;
}

agent_tool_t *tool_ast_grep_new(tool_env_t *env) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "eagerDelegation", tools_eager_delegation(env));
    json_set_bool(ctx, "scoutAvailable", tools_scout_available());
    char *desc = tool_prompt("ast-grep", ctx);
    json_free(ctx);
    json_t *s = schema_lit(
        "{\"type\":\"object\",\"properties\":{\"pat\":{\"type\":\"string\",\"description\":\"ast pattern\"},\"path\":{\"type\":\"string\",\"description\":\"file, directory, glob, or internal URL to sea"
        "rch; pass several as a semicolon-delimited list (\\\"src; tests\\\"). Omitted -> searches the wo"
        "rkspace root (\\\".\\\")\"},\"lang\":{\"type\":\"string\",\"description\":\"language override, e"
        ".g. cpp for ambiguous .h files\"},\"skip\":{\"type\":\"number\",\"description\":\"matches to ski"
        "p\"}},\"required\":[\"pat\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("ast_grep", "AST Grep", desc, s, astgrep_execute);
    t->strict = TOOL_STRICT;
    t->examples = schema_lit(
        "[{\"caption\":\"Search TypeScript files under src\",\"call\":{\"pat\":\"console.log($$$)\",\"pat"
        "h\":\"src/**/*.ts\"}},{\"caption\":\"Named imports from a specific package\",\"call\":{\"pat\":\"import { $$$IMPORTS } from \\\"react\\\"\",\"path\":\"src/**/*.ts\"}},{\"caption\":\"Arrow func"
        "tions assigned to a const\",\"call\":{\"pat\":\"const $NAME = ($$$ARGS) => $BODY\",\"path\":\"sr"
        "c/utils/**/*.ts\"}},{\"caption\":\"Method call on any object, ignoring method name with `$_`\",\"call\":{\"pat\":\"logger.$_($$$ARGS)\",\"path\":\"src/**/*.ts\"}},{\"caption\":\"Loosest existe"
        "nce check for a symbol in one file\",\"call\":{\"pat\":\"processItems\",\"path\":\"src/worker.ts"
        "\"}}]");
    free(desc);
    t->ud = env;
    return t;
}

/* ---------- ast_edit (staged) ---------- */

static char *substitute(const char *outtpl, const char *src, const match_t *m) {
    sb_t b = SB_INIT;
    for (const char *p = outtpl; *p;) {
        if (*p == '$') {
            const char *q = p + 1;
            if (q[0] == '$' && q[1] == '$') q += 2;
            const char *e = q;
            while (isalnum((unsigned char)*e) || *e == '_') e++;
            if (e > q) {
                char *name = xstrndup(q, (size_t)(e - q));
                const capture_t *c = NULL;
                for (size_t k = 0; k < m->caps.n; k++)
                    if (!strcmp(m->caps.v[k].name, name)) c = &m->caps.v[k];
                free(name);
                if (c) {
                    sb_appendn(&b, src + c->start, c->end - c->start);
                    p = e;
                    continue;
                }
            }
        }
        sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

typedef struct {
    char *path;
    char *old_text;
    char *new_text;
} staged_file_t;

static VEC(staged_file_t) g_staged; /* one pending proposal per process (tools run serially; edit is exclusive) */
static size_t g_staged_total;        /* replacements the pending proposal previewed */

static void staged_clear(void) {
    for (size_t i = 0; i < g_staged.n; i++) {
        free(g_staged.v[i].path);
        free(g_staged.v[i].old_text);
        free(g_staged.v[i].new_text);
    }
    g_staged.n = 0;
}

static int astedit_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(ctx);
    json_t *ops = json_get_arr(args, "ops");
    json_t *paths = json_get_arr(args, "paths");
    if (!json_len(ops) || !json_len(paths)) {
        tool_result_error(out, "ops and paths are required");
        return 0;
    }
    VEC(cpat_t) pats = {0};
    char *err = NULL;
    json_t *op;
    json_arr_foreach(ops, i, op) {
        cpat_t cp;
        const char *p = json_get_str(op, "pat");
        if (!p || !*p || pat_compile(p, &cp, &err) != 0) {
            tool_result_error(out, "Invalid pattern %s: %s", p ? p : "", err ? err : "empty");
            free(err);
            if (p && *p) pat_free(&cp);
            for (size_t k = 0; k < pats.n; k++) pat_free(&pats.v[k]);
            vec_free(&pats);
            return 0;
        }
        vec_push(&pats, cp);
    }
    strvec_t files = {0};
    json_t *pj;
    json_arr_foreach(paths, i, pj) if (json_is_str(pj)) collect_files(env, json_sv(pj), &files);
    staged_clear();
    size_t total = 0;
    sb_t preview = SB_INIT;
    json_t *dfiles = json_arr(); /* omp displayContent source: per file `-N│before` / `+N│after` rows */
    size_t nsearched = 0;
    for (size_t f = 0; f < files.n; f++) {
        if (!lang_supported(files.v[f])) continue;
        size_t n;
        char *src = fs_read_limit(files.v[f], 4u << 20, &n, NULL);
        if (!src || fs_is_binary(src, n)) {
            free(src);
            continue;
        }
        nsearched++;
        char *cur = src;
        size_t file_hits = 0;
        json_t *dlines = json_arr();
        for (size_t pi = 0; pi < pats.n; pi++) {
            toks_t toks = {0};
            size_t cl = strlen(cur);
            tokenize(cur, cl, lang_for(files.v[f], NULL), false, &toks);
            size_t pos = 0;
            units_t root = {0};
            while (pos < toks.n) {
                units_t part = build(&toks, &pos, 0);
                for (size_t i = 0; i < part.n; i++) vec_push(&root, part.v[i]);
                free(part.v);
                if (pos < toks.n) pos++;
            }
            matches_t ms = {0};
            find_all(cur, &pats.v[pi].units, root.v, root.n, &ms, 0);
            if (ms.n) {
                const char *tpl = json_get_str(json_at(ops, pi), "out");
                sb_t nb = SB_INIT;
                size_t last = 0;
                /* matches are found in document order except nested ones; sort by start */
                for (size_t a = 1; a < ms.n; a++)
                    for (size_t b2 = a; b2 > 0 && ms.v[b2].start < ms.v[b2 - 1].start; b2--) {
                        match_t t2 = ms.v[b2];
                        ms.v[b2] = ms.v[b2 - 1];
                        ms.v[b2 - 1] = t2;
                    }
                for (size_t k = 0; k < ms.n; k++) {
                    if (ms.v[k].start < last) continue; /* overlap */
                    sb_appendn(&nb, cur + last, ms.v[k].start - last);
                    char *rep = substitute(tpl ? tpl : "", cur, &ms.v[k]);
                    {
                        int64_t ln = line_of(cur, ms.v[k].start);
                        size_t bl = strcspn(cur + ms.v[k].start, "\n");
                        bl = MIN(bl, ms.v[k].end - ms.v[k].start);
                        char *before = xstrndup(cur + ms.v[k].start, MIN(bl, (size_t)120));
                        char *after = xstrndup(rep, MIN(strcspn(rep, "\n"), (size_t)120));
                        for (int side = 0; side < 2; side++) {
                            json_t *dl = json_obj();
                            json_set_int(dl, "line", ln);
                            json_set_str(dl, "mark", side ? "+" : "-");
                            json_set_str(dl, "text", side ? after : before);
                            json_push(dlines, dl);
                        }
                        free(after);
                        free(before);
                    }
                    sb_append(&nb, rep);
                    free(rep);
                    last = ms.v[k].end;
                    file_hits++;
                }
                sb_append(&nb, cur + last);
                if (cur != src) free(cur);
                cur = sb_detach(&nb);
            }
            matches_free(&ms);
            units_free(&root);
            for (size_t i = 0; i < toks.n; i++) free(toks.v[i].text);
            vec_free(&toks);
        }
        if (file_hits) {
            total += file_hits;
            char *disp = path_display(files.v[f], env->cwd);
            json_t *df = json_obj();
            json_set_str(df, "path", disp);
            char *sfx = xasprintf(" (%zu replacement%s)", file_hits, file_hits == 1 ? "" : "s");
            json_set_str(df, "suffix", sfx);
            free(sfx);
            json_set(df, "lines", dlines);
            dlines = NULL;
            json_push(dfiles, df);
            char *d = diff_unified(src, cur, disp, disp, 2);
            if (preview.len < 40000) sb_append(&preview, d);
            free(d);
            free(disp);
            staged_file_t sf = {xstrdup(files.v[f]), src, cur};
            vec_push(&g_staged, sf);
        } else {
            if (cur != src) free(cur);
            free(src);
        }
        json_free(dlines);
    }
    for (size_t k = 0; k < pats.n; k++) pat_free(&pats.v[k]);
    vec_free(&pats);
    strvec_free(&files);
    if (!total) {
        tool_result_text(out, "No matches; nothing staged.");
        out->useless = true;
    } else {
        g_staged_total = total;
        /* the renderer's proposed badge is TUI-only; this line is the model's signal that the diff is staged */
        tool_result_textf(out, "%s\n\n(proposed) %zu replacement%s · %zu file%s\n%s", TOOLCMD_PREVIEW_PENDING_NOTICE, total,
                          total == 1 ? "" : "s", g_staged.n, g_staged.n == 1 ? "" : "s", sb_cstr(&preview));
        out->details = json_obj();
        json_set_int(out->details, "replacements", (int64_t)total);
        json_set_int(out->details, "files", (int64_t)g_staged.n);
        json_set_bool(out->details, "proposed", true);
        /* omp AstEditToolDetails */
        json_set_int(out->details, "totalReplacements", (int64_t)total);
        json_set_int(out->details, "filesTouched", (int64_t)g_staged.n);
        json_set_int(out->details, "filesSearched", (int64_t)nsearched);
        json_set_bool(out->details, "applied", false);
        json_set_bool(out->details, "limitReached", false);
        json_set_str(out->details, "cwd", env->cwd);
        strvec_t sp = {0};
        json_t *pe;
        json_arr_foreach(paths, i, pe) if (json_is_str(pe)) strvec_push(&sp, json_sv(pe));
        char *spj = strvec_join(&sp, ", ");
        json_set_str(out->details, "scopePath", spj);
        free(spj);
        strvec_free(&sp);
        bool single = json_len(paths) == 1 && nsearched == 1 && json_len(dfiles) == 1;
        char *dc = grep_display_content(dfiles, !single);
        json_set_str(out->details, "displayContent", dc);
        free(dc);
    }
    json_free(dfiles);
    sb_free(&preview);
    return 0;
}

/* `resolve` / `reject` of the pending ast_edit preview (resolve.ts runResolveInvocation); NULL when none is pending */
char *astedit_resolve(tool_env_t *env, bool apply, const char *reason, bool *is_error, json_t **details) {
    *is_error = false;
    *details = NULL;
    if (!g_staged.n) return NULL;
    size_t files = g_staged.n, total = g_staged_total;
    char *label = xasprintf("AST Edit: %zu replacement%s in %zu file%s", total, total == 1 ? "" : "s", files,
                            files == 1 ? "" : "s");
    json_t *d = json_obj();
    json_set_str(d, "action", apply ? "apply" : "discard");
    json_set_str(d, "reason", reason ? reason : "");
    json_set_str(d, "sourceToolName", "ast_edit");
    json_set_str(d, "label", label);
    *details = d;
    if (!apply) {
        char *r = xasprintf("Discarded: %s. Reason: %s", label, reason ? reason : "");
        free(label);
        staged_clear();
        return r;
    }
    free(label);
    /* verify nothing changed since staging, then write all */
    for (size_t i = 0; i < g_staged.n; i++) {
        size_t n;
        char *cur = fs_read(g_staged.v[i].path, &n);
        bool same = cur && !strcmp(cur, g_staged.v[i].old_text);
        free(cur);
        if (!same) {
            *is_error = true;
            char *r = xasprintf("Preview is stale / no longer matches; no replacements were applied. Preview expected %zu "
                                "replacement%s in %zu file%s.",
                                total, total == 1 ? "" : "s", files, files == 1 ? "" : "s");
            staged_clear();
            return r;
        }
    }
    size_t ok = 0;
    for (size_t i = 0; i < g_staged.n; i++) {
        if (fs_write_atomic(g_staged.v[i].path, g_staged.v[i].new_text, strlen(g_staged.v[i].new_text), 0644) == 0) {
            ok++;
            edit_store_record(&env->store, g_staged.v[i].path, g_staged.v[i].new_text, 0, 0);
        }
    }
    char *r = xasprintf("Applied %zu replacement%s in %zu file%s.", ok == files ? total : ok, (ok == files ? total : ok) == 1 ? "" : "s",
                        ok, ok == 1 ? "" : "s");
    staged_clear();
    return r;
}

agent_tool_t *tool_ast_edit_new(tool_env_t *env) {
    char *desc = tool_prompt("ast-edit", NULL);
    json_t *s = schema_lit(
        "{\"type\":\"object\",\"properties\":{\"ops\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
        "\"properties\":{\"pat\":{\"type\":\"string\",\"description\":\"ast pattern\"},\"out\":{\"type\":"
        "\"string\",\"description\":\"replacement template\"}},\"required\":[\"pat\",\"out\"],\"additiona"
        "lProperties\":false},\"minItems\":1,\"description\":\"rewrite ops\"},\"paths\":{\"type\":\"array"
        "\",\"items\":{\"type\":\"string\",\"description\":\"file, directory, glob, or internal URL to re"
        "write\"},\"minItems\":1,\"description\":\"files, directories, globs, or internal URLs to rewrite"
        "\"}},\"required\":[\"ops\",\"paths\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("ast_edit", "AST Edit", desc, s, astedit_execute);
    t->strict = TOOL_STRICT;
    t->examples = schema_lit(
        "[{\"caption\":\"Rename a call site across TypeScript files\",\"call\":{\"ops\":[{\"pat\":\"oldAp"
        "i($$$ARGS)\",\"out\":\"newApi($$$ARGS)\"}],\"paths\":[\"src/**/*.ts\"]}},{\"caption\":\"Delete m"
        "atching calls\",\"call\":{\"ops\":[{\"pat\":\"console.log($$$ARGS)\",\"out\":\"\"}],\"paths\":[\"src/**/*.ts\"]}},{\"caption\":\"Rewrite import source path\",\"call\":{\"ops\":[{\"pat\":\"impo"
        "rt { $$$IMPORTS } from \\\"old-package\\\"\",\"out\":\"import { $$$IMPORTS } from \\\"new-packag"
        "e\\\"\"}],\"paths\":[\"src/**/*.ts\"]}},{\"caption\":\"Modernize to optional chaining (same meta"
        "variable enforces identity)\",\"call\":{\"ops\":[{\"pat\":\"$A && $A()\",\"out\":\"$A?.()\"}],\""
        "paths\":[\"src/**/*.ts\"]}},{\"caption\":\"Swap two arguments using captures\",\"call\":{\"ops\""
        ":[{\"pat\":\"assertEqual($A, $B)\",\"out\":\"assertEqual($B, $A)\"}],\"paths\":[\"tests/**/*.ts\"]}},{\"caption\":\"Python — convert print calls to logging\",\"call\":{\"ops\":[{\"pat\":\"prin"
        "t($$$ARGS)\",\"out\":\"logger.info($$$ARGS)\"}],\"paths\":[\"src/**/*.py\"]}}]");
    free(desc);
    t->ud = env;
    t->exclusive = true;
    return t;
}
