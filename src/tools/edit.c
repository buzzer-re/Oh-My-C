/* edit tool: hashline patch language (default) and replace mode. */
#include "tools.h"

#include "config/settings.h"
#include "core/fs.h"
#include "diff.h"
#include "prompt/template.h"

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- text helpers ---------- */

typedef struct {
    strvec_t lines;
    bool trailing_nl;
    bool crlf;
} doc_t;

static void doc_load(doc_t *d, const char *text) {
    memset(d, 0, sizeof(*d));
    size_t len = strlen(text);
    d->crlf = strstr(text, "\r\n") != NULL;
    d->lines = str_split_lines(text, len);
    d->trailing_nl = len > 0 && text[len - 1] == '\n';
}

static char *doc_join(const strvec_t *lines, bool trailing_nl, bool crlf) {
    sb_t b = SB_INIT;
    const char *nl = crlf ? "\r\n" : "\n";
    for (size_t i = 0; i < lines->n; i++) {
        sb_append(&b, lines->v[i]);
        if (i + 1 < lines->n || trailing_nl) sb_append(&b, nl);
    }
    return sb_detach(&b);
}

/* ---------- block resolution ---------- */

static size_t indent_of(const char *s) {
    size_t n = 0;
    for (; *s == ' ' || *s == '\t'; s++) n += *s == '\t' ? 4 : 1;
    return n;
}

static bool blank(const char *s) {
    for (; *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

static bool is_markdown(const char *path) {
    const char *e = path_ext(path);
    return !strcasecmp(e, ".md") || !strcasecmp(e, ".markdown") || !strcasecmp(e, ".mdx");
}

static bool is_indent_lang(const char *path) {
    const char *e = path_ext(path);
    return !strcasecmp(e, ".py") || !strcasecmp(e, ".pyi") || !strcasecmp(e, ".yaml") || !strcasecmp(e, ".yml") ||
           !strcasecmp(e, ".nim") || !strcasecmp(e, ".coffee");
}

static int heading_level(const char *s) {
    int n = 0;
    while (s[n] == '#') n++;
    return (n > 0 && n <= 6 && (s[n] == ' ' || s[n] == 0)) ? n : 0;
}

/* net bracket depth change of a line, ignoring string literals and // # comments roughly */
static int bracket_delta(const char *s, int *opened_any) {
    int d = 0;
    char q = 0;
    for (const char *p = s; *p; p++) {
        if (q) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == q) q = 0;
            continue;
        }
        if (*p == '"' || *p == '\'' || *p == '`') {
            q = *p;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') break;
        if (*p == '{' || *p == '(' || *p == '[') {
            d++;
            if (opened_any) *opened_any = 1;
        } else if (*p == '}' || *p == ')' || *p == ']') {
            d--;
        }
    }
    return d;
}

/* resolve block starting at 1-based line n; returns end line (inclusive) or 0 on failure with *err */
static int64_t resolve_block(const doc_t *d, const char *path, int64_t n, char **err) {
    int64_t total = (int64_t)d->lines.n;
    if (n < 1 || n > total) {
        *err = xasprintf("block anchor %lld out of range (1-%lld)", (long long)n, (long long)total);
        return 0;
    }
    const char *first = d->lines.v[n - 1];
    if (blank(first)) {
        *err = xasprintf("block anchor %lld is a blank line; anchor the construct opener", (long long)n);
        return 0;
    }
    if (is_markdown(path)) {
        int lv = heading_level(first);
        if (!lv) {
            /* paragraph/list block: until blank line */
            int64_t e = n;
            while (e < total && !blank(d->lines.v[e])) e++;
            if (e == n) {
                *err = xasprintf("line %lld is a single-line node; use PUT %lld.=%lld", (long long)n, (long long)n, (long long)n);
                return 0;
            }
            return e;
        }
        int64_t e = n;
        bool in_fence = false;
        for (int64_t i = n; i < total; i++) {
            const char *l = d->lines.v[i];
            if (str_starts(l, "```") || str_starts(l, "~~~")) in_fence = !in_fence;
            if (!in_fence) {
                int hl = heading_level(l);
                if (hl && hl <= lv) break;
            }
            e = i + 1;
        }
        while (e > n && blank(d->lines.v[e - 1])) e--;
        return e;
    }
    /* skip decorators / attributes / doc comments to the declaration line */
    int64_t decl = n;
    while (decl <= total) {
        const char *l = d->lines.v[decl - 1];
        while (*l == ' ' || *l == '\t') l++;
        bool deco = (*l == '@' && isalpha((unsigned char)l[1])) || str_starts(l, "#[") || str_starts(l, "///") ||
                    str_starts(l, "/**") || str_starts(l, "* ") || str_starts(l, "*/") || str_starts(l, "//!");
        if (str_starts(l, "/**")) {
            /* skip whole doc comment */
            while (decl <= total && !strstr(d->lines.v[decl - 1], "*/")) decl++;
            decl++;
            continue;
        }
        if (!deco) break;
        if (*l == '@' && bracket_delta(l, NULL) > 0) {
            /* multi-line decorator call */
            int depth = 0;
            while (decl <= total) {
                depth += bracket_delta(d->lines.v[decl - 1], NULL);
                if (depth <= 0) break;
                decl++;
            }
        }
        decl++;
    }
    if (decl > total) {
        *err = xasprintf("no declaration follows line %lld", (long long)n);
        return 0;
    }
    if (!is_indent_lang(path)) {
        int depth = 0, opened = 0;
        for (int64_t i = decl; i <= total; i++) {
            depth += bracket_delta(d->lines.v[i - 1], &opened);
            if (opened && depth <= 0) {
                if (i == n) {
                    *err = xasprintf("line %lld is a single-line node; use PUT %lld.=%lld", (long long)n, (long long)n,
                                     (long long)n);
                    return 0;
                }
                return i;
            }
            if (!opened && i > decl + 3) break; /* signature spans too many lines without opening */
        }
        if (opened) {
            *err = xasprintf("unbalanced block starting at line %lld", (long long)n);
            return 0;
        }
    }
    /* indentation-based */
    size_t base = indent_of(d->lines.v[decl - 1]);
    int64_t e = decl;
    for (int64_t i = decl + 1; i <= total; i++) {
        const char *l = d->lines.v[i - 1];
        if (blank(l)) continue;
        if (indent_of(l) <= base) {
            /* include closing keywords at same indent like 'end' / '}' / ')' */
            const char *t = l + strspn(l, " \t");
            if (*t == '}' || *t == ')' || *t == ']' || str_starts(t, "end")) e = i;
            break;
        }
        e = i;
    }
    if (e == n) {
        *err = xasprintf("line %lld is a single-line node; use PUT %lld.=%lld", (long long)n, (long long)n, (long long)n);
        return 0;
    }
    return e;
}

/* ---------- parsing ---------- */

typedef enum { OP_REPLACE, OP_INSERT_BEFORE, OP_INSERT_AFTER, OP_CUT, OP_REM, OP_MV } op_kind_t;

typedef struct {
    op_kind_t kind;
    int64_t a, b;      /* line range (1-based inclusive) / anchor; b==-1 => '$' tail */
    bool block;        /* N* */
    bool has_body;     /* ':' form */
    strvec_t body;
    char *reg;         /* @name for paste/cut (NULL => anonymous) */
    bool paste;        /* register-backed */
    char *dest;        /* MV */
    int line_no;       /* input line for errors */
} hop_t;

typedef struct {
    char *path;
    char *tag;
    VEC(hop_t) ops;
} hsection_t;

typedef VEC(hsection_t) hsections_t;

static void sections_free(hsections_t *s) {
    for (size_t i = 0; i < s->n; i++) {
        for (size_t k = 0; k < s->v[i].ops.n; k++) {
            strvec_free(&s->v[i].ops.v[k].body);
            free(s->v[i].ops.v[k].reg);
            free(s->v[i].ops.v[k].dest);
        }
        vec_free(&s->v[i].ops);
        free(s->v[i].path);
        free(s->v[i].tag);
    }
    vec_free(s);
}

static bool parse_num(const char **p, int64_t *out) {
    const char *s = *p;
    if (*s == 'L') s++;
    if (!isdigit((unsigned char)*s)) return false;
    char *e;
    long long v = strtoll(s, &e, 10);
    *out = v;
    *p = e;
    return true;
}

static void skip_sp(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

static char *parse_reg(const char **p) {
    skip_sp(p);
    if (**p != '@') return NULL;
    (*p)++;
    const char *s = *p;
    while (isalnum((unsigned char)**p) || **p == '_' || **p == '-') (*p)++;
    return xstrndup(s, (size_t)(*p - s));
}

/* parse header op line; returns error string (owned) or NULL */
static char *parse_op(const char *line, hop_t *op) {
    memset(op, 0, sizeof(*op));
    const char *p = line;
    if (str_starts(p, "REM")) {
        op->kind = OP_REM;
        return NULL;
    }
    if (str_starts(p, "MV ")) {
        op->kind = OP_MV;
        p += 3;
        skip_sp(&p);
        char *d;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            const char *e = strchr(p, q);
            d = e ? xstrndup(p, (size_t)(e - p)) : xstrdup(p);
        } else {
            d = str_trim(p);
        }
        op->dest = d;
        return NULL;
    }
    bool cut = str_starts(p, "CUT ");
    bool put = str_starts(p, "PUT ");
    if (!cut && !put) return xasprintf("unknown operation: %s", line);
    p += 4;
    skip_sp(&p);
    if (put && (*p == '<' || *p == '>')) {
        op->kind = *p == '<' ? OP_INSERT_BEFORE : OP_INSERT_AFTER;
        p++;
        if (*p == '$') {
            op->a = -1;
            p++;
        } else if (!parse_num(&p, &op->a)) {
            return xasprintf("bad anchor in: %s", line);
        }
        if (*p == '*') {
            op->block = true;
            p++;
        }
    } else {
        op->kind = cut ? OP_CUT : OP_REPLACE;
        if (!parse_num(&p, &op->a)) return xasprintf("bad line number in: %s", line);
        if (*p == '*') {
            op->block = true;
            p++;
        } else if (p[0] == '.' && p[1] == '=') {
            p += 2;
            if (!parse_num(&p, &op->b)) return xasprintf("bad range end in: %s", line);
        } else if (*p == '-' || (p[0] == '.' && p[1] == '.')) {
            /* recovered spelling N-M / N..M */
            p += *p == '-' ? 1 : 2;
            if (!parse_num(&p, &op->b)) return xasprintf("bad range end in: %s", line);
        } else {
            op->b = op->a; /* bare N treated as single line */
        }
        if (!op->block && op->b < op->a) return xasprintf("reversed range in: %s", line);
    }
    skip_sp(&p);
    if (*p == ':') {
        op->has_body = true;
        p++;
        if (cut) return xasprintf("CUT takes no body: %s", line);
    } else {
        op->reg = parse_reg(&p);
        if (put) op->paste = true;
        skip_sp(&p);
        if (*p == ':') {
            /* "PUT 3.=4 @x:" is ambiguous; treat as body-bearing */
            op->has_body = true;
            op->paste = false;
            p++;
        }
    }
    if (cut) op->reg = op->reg ? op->reg : parse_reg(&p);
    if (put && op->kind == OP_REPLACE && op->paste && !op->reg)
        return xasprintf("span/block paste requires a named register: %s", line);
    return NULL;
}

static char *parse_input(const char *input, hsections_t *out) {
    memset(out, 0, sizeof(*out));
    strvec_t lines = str_split_lines(input, strlen(input));
    hsection_t *cur = NULL;
    hop_t *last = NULL;
    char *err = NULL;
    for (size_t i = 0; i < lines.n && !err; i++) {
        char *l = lines.v[i];
        if (str_starts(l, "*** Begin Patch") || str_starts(l, "*** End Patch")) continue;
        if (l[0] == '[' && strchr(l, ']')) {
            char *close = strrchr(l, ']');
            char *inner = xstrndup(l + 1, (size_t)(close - l - 1));
            char *hash = strrchr(inner, '#');
            hsection_t s = {0};
            if (hash) {
                *hash = 0;
                s.tag = xstrdup(hash + 1);
            }
            s.path = xstrdup(inner);
            free(inner);
            vec_push(out, s);
            cur = &out->v[out->n - 1];
            last = NULL;
            continue;
        }
        if (!cur) {
            if (blank(l)) continue;
            err = xasprintf("line %zu: expected [PATH#TAG] section header before: %s", i + 1, l);
            break;
        }
        if (l[0] == '+') {
            if (!last || !last->has_body) {
                err = xasprintf("line %zu: body row without a body-bearing PUT ...: header", i + 1);
                break;
            }
            strvec_push(&last->body, l + 1);
            continue;
        }
        if (blank(l)) continue;
        if (str_starts(l, "@@") || (l[0] == '-' && l[1] != '-')) {
            err = xasprintf("line %zu: unified-diff rows are not hashline syntax; use PUT/CUT ops with +TEXT body rows", i + 1);
            break;
        }
        hop_t op;
        char *e = parse_op(l, &op);
        if (e) {
            err = xasprintf("line %zu: %s", i + 1, e);
            free(e);
            break;
        }
        op.line_no = (int)i + 1;
        vec_push(&cur->ops, op);
        last = &cur->ops.v[cur->ops.n - 1];
    }
    strvec_free(&lines);
    if (!err) {
        for (size_t i = 0; i < out->n && !err; i++) {
            hsection_t *s = &out->v[i];
            if (!s->ops.n) err = xasprintf("section [%s] has no operations", s->path);
            for (size_t k = 0; k < s->ops.n && !err; k++) {
                hop_t *o = &s->ops.v[k];
                if (o->has_body && !o->body.n)
                    err = xasprintf("line %d: PUT ...: requires +TEXT body rows (delete with CUT)", o->line_no);
            }
        }
        if (!out->n && !err) err = xstrdup("empty edit input: expected one or more [PATH#TAG] sections");
    }
    return err;
}

/* ---------- application ---------- */

typedef struct {
    int64_t start; /* 0-based index of first replaced line */
    int64_t count; /* lines removed */
    strvec_t ins;  /* lines inserted (borrowed copies owned here) */
    int order;
    bool is_insert;
} splice_t;

static int cmp_splice(const void *x, const void *y) {
    const splice_t *a = x, *b = y;
    if (a->start != b->start) return a->start < b->start ? 1 : -1; /* descending */
    return a->order < b->order ? 1 : -1;
}

static bool seen_ok(snapshot_t *sn, int64_t a, int64_t b) {
    if (!sn || !sn->seen.n) return true;
    for (int64_t ln = a; ln <= b; ln++) {
        bool ok = false;
        for (size_t i = 0; i < sn->seen.n; i++)
            if (ln >= sn->seen.v[i].lo && ln <= sn->seen.v[i].hi) ok = true;
        if (!ok) return false;
    }
    return true;
}

static void preview(sb_t *b, const strvec_t *lines, int64_t lo, int64_t hi) {
    lo = MAX(1, lo);
    hi = MIN((int64_t)lines->n, hi);
    for (int64_t i = lo; i <= hi; i++) sb_appendf(b, "%lld:%s\n", (long long)i, lines->v[i - 1]);
}

typedef struct {
    char *abs;
    char *display;
    char *old_text;
    char *new_text;
    char *move_to;
    bool remove;
    int64_t first_changed;
    sb_t msg;
} sres_t;

/* prepares one section: computes new text without writing */
static char *prepare_section(tool_env_t *env, hsection_t *s, hmap_t *anon_regs, sres_t *r) {
    memset(r, 0, sizeof(*r));
    r->abs = tool_resolve_path(env, s->path);
    r->display = path_display(r->abs, env->cwd);
    struct stat st;
    if (stat(r->abs, &st) != 0) return xasprintf("[%s] file not found (use write to create files)", s->path);
    if (!S_ISREG(st.st_mode)) return xasprintf("Cannot edit '%s': it is not a regular file", s->path);
    size_t len;
    char *cur = fs_read(r->abs, &len);
    if (!cur) return xasprintf("[%s] cannot read: %s", s->path, strerror(errno));
    if (memchr(cur, 0, len)) {
        free(cur);
        return xasprintf("[%s] is a binary file", s->path);
    }
    char *cur_tag = file_tag(cur, len);
    if (!s->tag || !*s->tag || strspn(s->tag, "0123456789abcdefABCDEF") != strlen(s->tag) || strlen(s->tag) != 4) {
        char *e = xasprintf("[%s] missing or invalid snapshot tag; current tag is #%s. Copy [%s#%s] from a fresh read.",
                            s->path, cur_tag, r->display, cur_tag);
        free(cur);
        free(cur_tag);
        return e;
    }
    snapshot_t *sn = edit_store_find(&env->store, r->abs, s->tag);
    const char *base_text = cur;
    if (strcasecmp(cur_tag, s->tag) != 0) {
        char *e;
        if (sn) e = xasprintf("[%s#%s] is stale: the file changed since that read (current tag #%s). Re-read the lines "
                              "you need, then edit with the new tag.", r->display, s->tag, cur_tag);
        else e = xasprintf("[%s#%s] unknown snapshot tag (current tag #%s). Re-read the file and copy its header.",
                           r->display, s->tag, cur_tag);
        free(cur);
        free(cur_tag);
        return e;
    }
    free(cur_tag);
    doc_t d;
    doc_load(&d, base_text);
    int64_t total = (int64_t)d.lines.n;
    bool enforce = settings_bool("edit.enforceSeenLines", true);
    VEC(splice_t) sp = {0};
    char *err = NULL;
    int order = 0;
    for (size_t k = 0; k < s->ops.n && !err; k++) {
        hop_t *o = &s->ops.v[k];
        if (o->kind == OP_REM) {
            r->remove = true;
            continue;
        }
        if (o->kind == OP_MV) {
            free(r->move_to);
            r->move_to = tool_resolve_path(env, o->dest);
            continue;
        }
        int64_t a = o->a, b = o->b;
        if (o->a == -1) a = total; /* '$' */
        if (o->block) {
            char *be = NULL;
            int64_t end = resolve_block(&d, r->abs, a, &be);
            if (!end) {
                if (o->kind == OP_INSERT_AFTER) {
                    sb_appendf(&r->msg, "Warning: no block at line %lld; inserted after the line instead.\n", (long long)a);
                    end = a;
                    free(be);
                } else {
                    err = xasprintf("line %d: %s", o->line_no, be);
                    free(be);
                    break;
                }
            }
            b = end;
            if (o->kind != OP_INSERT_AFTER) sb_appendf(&r->msg, "Block %lld* resolved to %lld-%lld.\n", (long long)a,
                                                       (long long)a, (long long)b);
        }
        if (o->kind == OP_INSERT_BEFORE || o->kind == OP_INSERT_AFTER) {
            int64_t anchor = o->block ? b : a;
            if (o->a != -1 && (a < 1 || a > MAX(total, 1))) {
                err = xasprintf("line %d: anchor %lld out of range (file has %lld lines)", o->line_no, (long long)a, (long long)total);
                break;
            }
            if (enforce && o->a != -1 && total && !seen_ok(sn, a, a)) {
                err = xasprintf("line %d: line %lld was not shown in your read; re-read it before editing", o->line_no, (long long)a);
                break;
            }
            splice_t s2 = {.count = 0, .order = order++, .is_insert = true};
            s2.start = o->kind == OP_INSERT_BEFORE ? a - 1 : (o->a == -1 ? total : anchor);
            if (s2.start < 0) s2.start = 0;
            if (o->paste) {
                const char *reg = o->reg ? hmap_get(&env->store.registers, o->reg) : hmap_get(anon_regs, "");
                if (!reg) {
                    err = xasprintf("line %d: register %s%s is empty", o->line_no, o->reg ? "@" : "", o->reg ? o->reg : "(anonymous)");
                    break;
                }
                s2.ins = str_split_lines(reg, strlen(reg));
            } else {
                for (size_t q = 0; q < o->body.n; q++) strvec_push(&s2.ins, o->body.v[q]);
            }
            vec_push(&sp, s2);
            continue;
        }
        /* replace / cut */
        if (a < 1 || b > total) {
            err = xasprintf("line %d: range %lld-%lld out of range (file has %lld lines)", o->line_no, (long long)a,
                            (long long)b, (long long)total);
            break;
        }
        if (enforce && !seen_ok(sn, a, b)) {
            err = xasprintf("line %d: lines %lld-%lld were not all shown in your read; re-read %s:%lld-%lld first",
                            o->line_no, (long long)a, (long long)b, r->display, (long long)a, (long long)b);
            break;
        }
        splice_t s2 = {.start = a - 1, .count = b - a + 1, .order = order++};
        if (o->kind == OP_CUT) {
            strvec_t cut = {0};
            for (int64_t i = a; i <= b; i++) strvec_push(&cut, d.lines.v[i - 1]);
            char *txt = strvec_join(&cut, "\n");
            strvec_free(&cut);
            free(hmap_put(o->reg ? &env->store.registers : anon_regs, o->reg ? o->reg : "", txt));
        } else if (o->paste) {
            const char *reg = hmap_get(&env->store.registers, o->reg);
            if (!reg) {
                err = xasprintf("line %d: register @%s is empty", o->line_no, o->reg);
                break;
            }
            s2.ins = str_split_lines(reg, strlen(reg));
        } else {
            for (size_t q = 0; q < o->body.n; q++) strvec_push(&s2.ins, o->body.v[q]);
        }
        vec_push(&sp, s2);
    }
    /* overlap detection among range ops */
    for (size_t i = 0; i < sp.n && !err; i++)
        for (size_t j = i + 1; j < sp.n && !err; j++) {
            splice_t *x = &sp.v[i], *y = &sp.v[j];
            if (!x->count || !y->count) continue;
            if (x->start < y->start + y->count && y->start < x->start + x->count)
                err = xasprintf("overlapping ranges %lld-%lld and %lld-%lld", (long long)x->start + 1,
                                (long long)(x->start + x->count), (long long)y->start + 1, (long long)(y->start + y->count));
        }
    if (!err) {
        if (sp.n > 1) qsort(sp.v, sp.n, sizeof(splice_t), cmp_splice);
        strvec_t lines = d.lines;
        int64_t first = INT64_MAX;
        for (size_t i = 0; i < sp.n; i++) {
            splice_t *x = &sp.v[i];
            first = MIN(first, x->start + 1);
            for (int64_t k = 0; k < x->count; k++) free(lines.v[x->start + k]);
            size_t tail = lines.n - (size_t)(x->start + x->count);
            size_t newn = lines.n - (size_t)x->count + x->ins.n;
            if (newn > lines.cap) {
                lines.cap = newn + 8;
                lines.v = xrealloc(lines.v, lines.cap * sizeof(char *));
            }
            memmove(lines.v + x->start + x->ins.n, lines.v + x->start + x->count, tail * sizeof(char *));
            for (size_t k = 0; k < x->ins.n; k++) lines.v[x->start + k] = xstrdup(x->ins.v[k]);
            lines.n = newn;
        }
        d.lines = lines;
        bool tnl = d.trailing_nl || (total == 0 && d.lines.n);
        r->new_text = doc_join(&d.lines, tnl, d.crlf);
        r->old_text = cur;
        cur = NULL;
        r->first_changed = first == INT64_MAX ? 0 : first;
        if (!r->remove && !r->move_to && strcmp(r->new_text, r->old_text) == 0)
            err = xasprintf("[%s] edit produced no change", r->display);
    }
    for (size_t i = 0; i < sp.n; i++) strvec_free(&sp.v[i].ins);
    vec_free(&sp);
    strvec_free(&d.lines);
    free(cur);
    return err;
}

static void sres_free(sres_t *r) {
    free(r->abs);
    free(r->display);
    free(r->old_text);
    free(r->new_text);
    free(r->move_to);
    sb_free(&r->msg);
}

int hashline_apply(tool_env_t *env, const char *input, tool_result_t *out) {
    hsections_t secs;
    char *err = parse_input(input, &secs);
    if (err) {
        tool_result_error(out, "%s", err);
        out->details = json_obj();
        json_set_str(out->details, "errorKind", "parse");
        free(err);
        sections_free(&secs);
        return 0;
    }
    hmap_t anon;
    hmap_init(&anon);
    VEC(sres_t) res = {0};
    for (size_t i = 0; i < secs.n && !err; i++) {
        sres_t r;
        err = prepare_section(env, &secs.v[i], &anon, &r);
        vec_push(&res, r);
    }
    if (err) {
        tool_result_error(out, "%s", err);
        out->details = json_obj();
        json_set_str(out->details, "errorKind", str_istr(err, "stale") ? "stale_tag" : str_istr(err, "unknown snapshot") ? "unknown_tag"
                                                 : str_istr(err, "not shown") ? "unseen_lines" : str_istr(err, "no change") ? "no_change"
                                                 : str_istr(err, "out of range") ? "out_of_range" : str_istr(err, "not found") ? "file_not_found" : "apply");
        free(err);
        goto cleanup;
    }
    /* write phase */
    sb_t b = SB_INIT;
    json_t *details = json_obj();
    json_t *files = json_arr();
    sb_t all_diff = SB_INIT;
    for (size_t i = 0; i < res.n; i++) {
        sres_t *r = &res.v[i];
        if (r->remove) {
            if (unlink(r->abs) != 0) {
                sb_appendf(&b, "Failed to delete %s: %s\n", r->display, strerror(errno));
                out->is_error = true;
                continue;
            }
            sb_appendf(&b, "Deleted %s\n", r->display);
            continue;
        }
        const char *target = r->move_to ? r->move_to : r->abs;
        int rc = fs_write_atomic(target, r->new_text, strlen(r->new_text), 0644);
        if (rc != 0) {
            sb_appendf(&b, "Failed to write %s: %s\n", r->display, strerror(-rc));
            out->is_error = true;
            break;
        }
        if (r->move_to && strcmp(r->move_to, r->abs) != 0) unlink(r->abs);
        char *tdisp = path_display(target, env->cwd);
        const char *tag = edit_store_record(&env->store, target, r->new_text, 0, 0);
        format_hashline_header(&b, tdisp, tag);
        if (r->move_to) sb_appendf(&b, "Moved %s -> %s\n", r->display, tdisp);
        sb_append(&b, sb_cstr(&r->msg));
        /* compact preview around first change */
        if (r->first_changed) {
            strvec_t nl = str_split_lines(r->new_text, strlen(r->new_text));
            int64_t add, rem;
            diff_stats(r->old_text, r->new_text, &add, &rem);
            int64_t lo = r->first_changed - 2, hi = r->first_changed + MAX(add, (int64_t)1) + 1;
            if (hi - lo > 40) hi = lo + 40;
            preview(&b, &nl, lo, hi);
            strvec_free(&nl);
        }
        if (env->after_write) {
            char *diag = env->after_write(env, target);
            if (diag) {
                sb_appendf(&b, "%s\n", diag);
                free(diag);
            }
        }
        char *dd = diff_unified(r->old_text, r->new_text, r->display, tdisp, 3);
        sb_append(&all_diff, dd);
        json_t *f = json_obj();
        json_set_str(f, "path", target);
        json_set_str(f, "diff", dd);
        json_set_int(f, "firstChangedLine", r->first_changed);
        json_push(files, f);
        free(dd);
        free(tdisp);
    }
    json_set_str(details, "diff", sb_cstr(&all_diff));
    if (res.n) json_set_int(details, "firstChangedLine", res.v[0].first_changed);
    json_set(details, "files", files);
    json_set_str(details, "operation", res.n && res.v[0].remove ? "delete" : "update");
    if (b.len && b.s[b.len - 1] == '\n') sb_truncate(&b, b.len - 1);
    tool_result_text(out, sb_cstr(&b));
    out->details = details;
    sb_free(&b);
    sb_free(&all_diff);
cleanup:
    for (size_t i = 0; i < res.n; i++) sres_free(&res.v[i]);
    vec_free(&res);
    hmap_free(&anon, free);
    sections_free(&secs);
    return 0;
}

/* ---------- replace mode ---------- */

static char *normalize_ws_line(const char *s) {
    char *t = str_trim(s);
    return t;
}

int replace_apply(tool_env_t *env, const char *path, const char *old_s, const char *new_s, bool all, tool_result_t *out) {
    char *abs = tool_resolve_path(env, path);
    char *display = path_display(abs, env->cwd);
    size_t len;
    char *cur = fs_read(abs, &len);
    if (!cur) {
        tool_result_error(out, "File not found: %s", display);
        goto done;
    }
    if (!old_s || !*old_s) {
        tool_result_error(out, "old_string must not be empty (use write to create files)");
        goto done;
    }
    size_t cnt = str_count(cur, old_s);
    char *next = NULL;
    if (cnt == 0) {
        /* whitespace-tolerant line-block match */
        strvec_t fl = str_split_lines(cur, len), ol = str_split_lines(old_s, strlen(old_s));
        ssize_t found = -1;
        size_t matches = 0;
        for (size_t i = 0; ol.n && i + ol.n <= fl.n; i++) {
            bool ok = true;
            for (size_t k = 0; k < ol.n && ok; k++) {
                char *x = normalize_ws_line(fl.v[i + k]), *y = normalize_ws_line(ol.v[k]);
                ok = strcmp(x, y) == 0;
                free(x);
                free(y);
            }
            if (ok) {
                if (found < 0) found = (ssize_t)i;
                matches++;
            }
        }
        if (matches == 1) {
            strvec_t nl = {0};
            for (size_t i = 0; i < (size_t)found; i++) strvec_push(&nl, fl.v[i]);
            strvec_t ins = str_split_lines(new_s ? new_s : "", strlen(new_s ? new_s : ""));
            for (size_t i = 0; i < ins.n; i++) strvec_push(&nl, ins.v[i]);
            for (size_t i = (size_t)found + ol.n; i < fl.n; i++) strvec_push(&nl, fl.v[i]);
            next = doc_join(&nl, len && cur[len - 1] == '\n', strstr(cur, "\r\n") != NULL);
            strvec_free(&nl);
            strvec_free(&ins);
        }
        strvec_free(&fl);
        strvec_free(&ol);
        if (!next) {
            if (matches > 1)
                tool_result_error(out, "old_string matches %zu locations in %s (whitespace-insensitive); add more context",
                                  matches, display);
            else
                tool_result_error(out, "Could not find old_string in %s. It must match exactly, including whitespace.",
                                  display);
            goto done;
        }
    } else if (cnt > 1 && !all) {
        tool_result_error(out, "old_string matches %zu locations in %s; provide more context or set replace_all", cnt, display);
        goto done;
    } else if (all) {
        next = str_replace(cur, old_s, new_s ? new_s : "");
    } else {
        const char *p = strstr(cur, old_s);
        sb_t b = SB_INIT;
        sb_appendn(&b, cur, (size_t)(p - cur));
        sb_append(&b, new_s ? new_s : "");
        sb_append(&b, p + strlen(old_s));
        next = sb_detach(&b);
    }
    if (strcmp(next, cur) == 0) {
        tool_result_error(out, "No changes made to %s: replacement is identical", display);
        free(next);
        goto done;
    }
    int rc = fs_write_atomic(abs, next, strlen(next), 0644);
    if (rc != 0) {
        tool_result_error(out, "Failed to write %s: %s", display, strerror(-rc));
    } else {
        edit_store_record(&env->store, abs, next, 0, 0);
        char *dd = diff_unified(cur, next, display, display, 3);
        tool_result_textf(out, "Successfully replaced %zu occurrence(s) in %s.", all ? cnt : (size_t)1, display);
        out->details = json_obj();
        json_set_str(out->details, "diff", dd);
        json_set_int(out->details, "firstChangedLine", diff_first_changed_line(cur, next));
        free(dd);
    }
    free(next);
done:
    free(cur);
    free(abs);
    free(display);
    return 0;
}

/* ---------- tool ---------- */

static int edit_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(ctx);
    if (str_eq(env->edit_mode, "replace")) {
        return replace_apply(env, json_get_str(args, "path"), json_get_str(args, "old_string"),
                             json_get_str(args, "new_string"), json_get_bool(args, "replace_all", false), out);
    }
    const char *input = json_get_str(args, "input");
    if (!input) {
        tool_result_error(out, "input is required");
        return 0;
    }
    pthread_mutex_lock(&env->mu);
    int rc = hashline_apply(env, input, out);
    pthread_mutex_unlock(&env->mu);
    return rc;
}

agent_tool_t *tool_edit_new(tool_env_t *env) {
    const char *mode = settings_str("edit.mode", "hashline");
    const char *ev = env_get("PI_EDIT_VARIANT");
    if (!ev) ev = env_get("OMC_EDIT_VARIANT");
    if (ev) mode = ev;
    if (strcmp(mode, "replace") != 0) mode = "hashline";
    free(env->edit_mode);
    env->edit_mode = xstrdup(mode);
    json_t *s = schema_obj();
    char *desc;
    if (!strcmp(mode, "replace")) {
        desc = prompt_load_or("crates/pi-edit/replace.md", "Replace exact text in a file.");
        schema_prop(s, "path", "string", "File to edit", true);
        schema_prop(s, "old_string", "string", "Exact text to replace", true);
        schema_prop(s, "new_string", "string", "Replacement text", true);
        schema_prop(s, "replace_all", "boolean", "Replace every occurrence", false);
    } else {
        desc = prompt_load_or("crates/pi-edit/hashline.md", "Hashline patch.");
        schema_prop(s, "input", "string", "One or more [PATH#TAG] sections of hashline operations", true);
    }
    agent_tool_t *t = agent_tool_new("edit", "Edit", desc, s, edit_execute);
    t->exclusive = true;
    free(desc);
    t->ud = env;
    return t;
}
