#include "template.h"

#include "core/embed.h"
#include "core/fs.h"

#include <ctype.h>
#include <math.h>

/* ---------------- tokenizer ---------------- */

typedef enum { TK_TEXT, TK_VAR, TK_OPEN, TK_INVERT_OPEN, TK_CLOSE, TK_ELSE, TK_COMMENT } tk_kind_t;

typedef struct {
    tk_kind_t kind;
    char *text;      /* text content or tag body (without braces / sigils) */
    bool strip_l, strip_r;
    bool removed;    /* text token fully stripped */
} tok_t;

typedef VEC(tok_t) toks_t;

static void push_text(toks_t *t, const char *s, size_t n) {
    tok_t k = {TK_TEXT, xstrndup(s, n), false, false, false};
    vec_push(t, k);
}

static void tokenize(const char *src, toks_t *out) {
    const char *p = src;
    for (;;) {
        const char *o = strstr(p, "{{");
        if (!o) {
            if (*p) push_text(out, p, strlen(p));
            break;
        }
        if (o > p && o[-1] == '\\') { /* escaped mustache */
            push_text(out, p, (size_t)(o - p - 1));
            push_text(out, "{{", 2);
            p = o + 2;
            continue;
        }
        if (o > p) push_text(out, p, (size_t)(o - p));
        const char *q = o + 2;
        bool triple = *q == '{';
        if (triple) q++;
        /* comments */
        if (*q == '!' || (*q == '~' && q[1] == '!')) {
            bool sl = *q == '~';
            if (sl) q++;
            const char *end;
            bool long_form = strncmp(q, "!--", 3) == 0;
            end = long_form ? strstr(q, "--}}") : strstr(q, "}}");
            if (!end) {
                push_text(out, o, strlen(o));
                break;
            }
            tok_t k = {TK_COMMENT, xstrdup(""), sl, end > q && end[-1] == '~', false};
            vec_push(out, k);
            p = end + (long_form ? 4 : 2);
            continue;
        }
        const char *close = strstr(q, triple ? "}}}" : "}}");
        if (!close) {
            push_text(out, o, strlen(o));
            break;
        }
        const char *bs = q, *be = close;
        bool sl = false, sr = false;
        if (bs < be && *bs == '~') { sl = true; bs++; }
        if (be > bs && be[-1] == '~') { sr = true; be--; }
        while (bs < be && isspace((unsigned char)*bs)) bs++;
        tok_t k = {TK_VAR, NULL, sl, sr, false};
        if (!triple && bs < be && (*bs == '#' || *bs == '^' || *bs == '/')) {
            k.kind = *bs == '#' ? TK_OPEN : *bs == '^' ? TK_INVERT_OPEN : TK_CLOSE;
            bs++;
            if (k.kind == TK_OPEN && bs < be && *bs == '>') bs++; /* partial block: unsupported, treat as open */
        } else if (!triple && be - bs >= 4 && strncmp(bs, "else", 4) == 0 && (be - bs == 4 || isspace((unsigned char)bs[4]))) {
            k.kind = TK_ELSE;
            bs += 4;
        } else if (!triple && be - bs == 1 && *bs == '^') {
            k.kind = TK_ELSE;
            bs++;
        }
        while (bs < be && isspace((unsigned char)*bs)) bs++;
        while (be > bs && isspace((unsigned char)be[-1])) be--;
        k.text = xstrndup(bs, (size_t)(be - bs));
        vec_push(out, k);
        p = close + (triple ? 3 : 2);
    }
}

static bool is_ws_only(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') return false;
    return true;
}

/* standalone line + tilde whitespace control */
static void apply_whitespace(toks_t *t) {
    for (size_t i = 0; i < t->n; i++) {
        tok_t *k = &t->v[i];
        if (k->kind == TK_TEXT) continue;
        /* ~ stripping */
        if (k->strip_l && i > 0 && t->v[i - 1].kind == TK_TEXT) {
            char *s = t->v[i - 1].text;
            size_t n = strlen(s);
            while (n && isspace((unsigned char)s[n - 1])) n--;
            s[n] = 0;
        }
        if (k->strip_r && i + 1 < t->n && t->v[i + 1].kind == TK_TEXT) {
            char *s = t->v[i + 1].text;
            size_t skip = 0;
            while (s[skip] && isspace((unsigned char)s[skip])) skip++;
            memmove(s, s + skip, strlen(s + skip) + 1);
        }
        if (k->kind == TK_VAR) continue;
        /* standalone: previous text ends with "\n<ws>" (or is start) and next begins "<ws>\n" (or end) */
        bool prev_ok, next_ok;
        size_t prev_cut = 0;
        if (i == 0) {
            prev_ok = true;
        } else if (t->v[i - 1].kind != TK_TEXT) {
            prev_ok = false;
        } else {
            const char *s = t->v[i - 1].text;
            const char *nl = strrchr(s, '\n');
            if (nl) {
                prev_ok = is_ws_only(nl + 1, strlen(nl + 1));
                prev_cut = (size_t)(nl + 1 - s);
            } else {
                /* no newline: only standalone if this text is at template start */
                bool at_start = i - 1 == 0;
                prev_ok = at_start && is_ws_only(s, strlen(s));
                prev_cut = 0;
            }
        }
        const char *ns = NULL;
        size_t next_cut = 0;
        if (i + 1 >= t->n) {
            next_ok = true;
        } else if (t->v[i + 1].kind != TK_TEXT) {
            next_ok = false;
        } else {
            ns = t->v[i + 1].text;
            const char *nl = strchr(ns, '\n');
            if (nl) {
                next_ok = is_ws_only(ns, (size_t)(nl - ns));
                next_cut = (size_t)(nl - ns) + 1;
            } else {
                next_ok = i + 2 >= t->n && is_ws_only(ns, strlen(ns));
                next_cut = strlen(ns);
            }
        }
        if (prev_ok && next_ok) {
            if (i > 0 && t->v[i - 1].kind == TK_TEXT) t->v[i - 1].text[prev_cut] = 0;
            if (ns) memmove(t->v[i + 1].text, ns + next_cut, strlen(ns + next_cut) + 1);
        }
    }
}

/* ---------------- AST ---------------- */

typedef enum { N_TEXT, N_VAR, N_BLOCK } node_kind_t;
typedef struct node node_t;
typedef VEC(node_t *) nodes_t;

/* parameter expression */
typedef enum { P_PATH, P_STR, P_NUM, P_BOOL, P_NULL, P_SUB } pkind_t;
typedef struct param param_t;
typedef VEC(param_t *) params_t;
typedef struct {
    char *key;
    param_t *val;
} hash_t;
typedef VEC(hash_t) hashes_t;

struct param {
    pkind_t kind;
    char *s;       /* path or string */
    double num;
    bool b;
    /* subexpression */
    char *helper;
    params_t args;
    hashes_t hash;
};

struct node {
    node_kind_t kind;
    char *text;      /* N_TEXT */
    char *name;      /* helper/path name for VAR/BLOCK */
    params_t args;
    hashes_t hash;
    bool inverted;   /* {{^x}} */
    nodes_t body;
    nodes_t inverse;
    char *block_params[2]; /* as |a b| */
};

static void param_free(param_t *p);
static void params_free(params_t *v) {
    for (size_t i = 0; i < v->n; i++) param_free(v->v[i]);
    vec_free(v);
}
static void hashes_free(hashes_t *h) {
    for (size_t i = 0; i < h->n; i++) {
        free(h->v[i].key);
        param_free(h->v[i].val);
    }
    vec_free(h);
}
static void param_free(param_t *p) {
    if (!p) return;
    free(p->s);
    free(p->helper);
    params_free(&p->args);
    hashes_free(&p->hash);
    free(p);
}
static void nodes_free(nodes_t *v);
static void node_free(node_t *n) {
    if (!n) return;
    free(n->text);
    free(n->name);
    params_free(&n->args);
    hashes_free(&n->hash);
    nodes_free(&n->body);
    nodes_free(&n->inverse);
    free(n->block_params[0]);
    free(n->block_params[1]);
    free(n);
}
static void nodes_free(nodes_t *v) {
    for (size_t i = 0; i < v->n; i++) node_free(v->v[i]);
    vec_free(v);
}

/* expression lexer over tag text */
typedef struct {
    const char *p;
} lex_t;

static void lskip(lex_t *l) {
    while (*l->p && isspace((unsigned char)*l->p)) l->p++;
}

static param_t *parse_param(lex_t *l, int depth);

static void parse_args(lex_t *l, params_t *args, hashes_t *hash, int depth, bool in_sub) {
    for (;;) {
        lskip(l);
        if (!*l->p || (in_sub && *l->p == ')') || *l->p == '|') break;
        /* hash pair? ident= */
        const char *s = l->p;
        while (*s && (isalnum((unsigned char)*s) || *s == '_' || *s == '-' || *s == '@')) s++;
        if (s > l->p && *s == '=') {
            hash_t h;
            h.key = xstrndup(l->p, (size_t)(s - l->p));
            l->p = s + 1;
            h.val = parse_param(l, depth);
            if (!h.val) {
                free(h.key);
                break;
            }
            vec_push(hash, h);
            continue;
        }
        param_t *p = parse_param(l, depth);
        if (!p) break;
        vec_push(args, p);
    }
}

static param_t *parse_param(lex_t *l, int depth) {
    lskip(l);
    if (!*l->p || depth > 16) return NULL;
    param_t *p = xcalloc(1, sizeof(*p));
    char c = *l->p;
    if (c == '"' || c == '\'') {
        char q = c;
        l->p++;
        sb_t b = SB_INIT;
        while (*l->p && *l->p != q) {
            if (*l->p == '\\' && l->p[1]) l->p++;
            sb_putc(&b, *l->p++);
        }
        if (*l->p == q) l->p++;
        p->kind = P_STR;
        p->s = sb_detach(&b);
        return p;
    }
    if (c == '(') {
        l->p++;
        lskip(l);
        const char *s = l->p;
        while (*l->p && !isspace((unsigned char)*l->p) && *l->p != ')') l->p++;
        p->kind = P_SUB;
        p->helper = xstrndup(s, (size_t)(l->p - s));
        parse_args(l, &p->args, &p->hash, depth + 1, true);
        if (*l->p == ')') l->p++;
        return p;
    }
    const char *s = l->p;
    while (*l->p && !isspace((unsigned char)*l->p) && *l->p != ')' && *l->p != '|') l->p++;
    char *tok = xstrndup(s, (size_t)(l->p - s));
    char *end;
    double d = strtod(tok, &end);
    if (*tok && *end == 0 && (isdigit((unsigned char)tok[0]) || tok[0] == '-' || tok[0] == '.')) {
        p->kind = P_NUM;
        p->num = d;
        free(tok);
    } else if (strcmp(tok, "true") == 0 || strcmp(tok, "false") == 0) {
        p->kind = P_BOOL;
        p->b = tok[0] == 't';
        free(tok);
    } else if (strcmp(tok, "null") == 0 || strcmp(tok, "undefined") == 0) {
        p->kind = P_NULL;
        free(tok);
    } else {
        p->kind = P_PATH;
        p->s = tok;
    }
    return p;
}

static void parse_tag(const char *text, char **name, params_t *args, hashes_t *hash, char *bp[2]) {
    lex_t l = {text};
    lskip(&l);
    const char *s = l.p;
    while (*l.p && !isspace((unsigned char)*l.p)) l.p++;
    *name = xstrndup(s, (size_t)(l.p - s));
    parse_args(&l, args, hash, 0, false);
    if (*l.p == '|' && bp) {
        /* "as |a b|" handled loosely: the preceding "as" was parsed as a path param; drop it */
        if (args->n && args->v[args->n - 1]->kind == P_PATH && strcmp(args->v[args->n - 1]->s, "as") == 0) {
            param_free(vec_pop(args));
        }
        l.p++;
        for (int i = 0; i < 2; i++) {
            lskip(&l);
            const char *b = l.p;
            while (*l.p && !isspace((unsigned char)*l.p) && *l.p != '|') l.p++;
            if (l.p > b) bp[i] = xstrndup(b, (size_t)(l.p - b));
        }
    }
}

typedef struct {
    toks_t *t;
    size_t i;
} parser_t;

/* parses until a CLOSE or ELSE at this level; returns the terminating token index or t->n */
static void parse_nodes(parser_t *ps, nodes_t *out, int depth) {
    while (ps->i < ps->t->n) {
        tok_t *k = &ps->t->v[ps->i];
        if (k->kind == TK_CLOSE || k->kind == TK_ELSE) return;
        ps->i++;
        if (k->kind == TK_COMMENT) continue;
        node_t *n = xcalloc(1, sizeof(*n));
        if (k->kind == TK_TEXT) {
            if (!*k->text) {
                free(n);
                continue;
            }
            n->kind = N_TEXT;
            n->text = xstrdup(k->text);
        } else if (k->kind == TK_VAR) {
            n->kind = N_VAR;
            parse_tag(k->text, &n->name, &n->args, &n->hash, NULL);
        } else {
            n->kind = N_BLOCK;
            n->inverted = k->kind == TK_INVERT_OPEN;
            parse_tag(k->text, &n->name, &n->args, &n->hash, n->block_params);
            if (depth < 64) parse_nodes(ps, &n->body, depth + 1);
            /* else / else-if chain */
            node_t *cur = n;
            while (ps->i < ps->t->n && ps->t->v[ps->i].kind == TK_ELSE) {
                tok_t *e = &ps->t->v[ps->i++];
                if (*e->text) {
                    /* {{else if x}} => inverse contains a single chained block */
                    node_t *chain = xcalloc(1, sizeof(*chain));
                    chain->kind = N_BLOCK;
                    parse_tag(e->text, &chain->name, &chain->args, &chain->hash, chain->block_params);
                    parse_nodes(ps, &chain->body, depth + 1);
                    vec_push(&cur->inverse, chain);
                    cur = chain;
                } else {
                    parse_nodes(ps, &cur->inverse, depth + 1);
                }
            }
            if (ps->i < ps->t->n && ps->t->v[ps->i].kind == TK_CLOSE) ps->i++;
        }
        vec_push(out, n);
    }
}

/* ---------------- evaluation ---------------- */

typedef struct frame {
    const json_t *ctx;
    const json_t *data; /* @index etc. object */
    struct frame *parent;
    const char *bp_names[2];
    const json_t *bp_vals[2];
} frame_t;

typedef struct {
    VEC(json_t *) temps; /* values created during rendering */
    const json_t *root;
    int depth;
} rctx_t;

static json_t *keep(rctx_t *r, json_t *j) {
    vec_push(&r->temps, j);
    return j;
}

static bool truthy(const json_t *j) {
    if (!j) return false;
    switch (j->type) {
    case JSON_NULL: return false;
    case JSON_BOOL: return j->u.b;
    case JSON_NUMBER: return j->u.num.d != 0;
    case JSON_STRING: return j->u.str.len > 0;
    case JSON_ARRAY: return j->u.arr.n > 0; /* handlebars: empty array is falsy for #if */
    case JSON_OBJECT: return true;
    }
    return false;
}

static json_t *keep(rctx_t *r, json_t *j);

/* json_path plus JS-ish ".length" on arrays/strings/objects */
static const json_t *tpl_path(rctx_t *r, const json_t *v, const char *path) {
    const char *dot = strrchr(path, '.');
    const char *last = dot ? dot + 1 : path;
    if (strcmp(last, "length") == 0) {
        const json_t *base = v;
        if (dot) {
            char *head = xstrndup(path, (size_t)(dot - path));
            base = json_path(v, head);
            free(head);
        }
        if (json_is_arr(base) || json_is_obj(base)) return keep(r, json_int((int64_t)json_len(base)));
        if (json_is_str(base)) return keep(r, json_int((int64_t)utf8_strlen(json_sv(base))));
        if (!dot) return json_path(v, path);
        return NULL;
    }
    return json_path(v, path);
}

static const json_t *lookup_path(rctx_t *r, frame_t *f, const char *path) {
    if (!path || !*path) return NULL;
    if (strcmp(path, "this") == 0 || strcmp(path, ".") == 0) return f->ctx;
    while (strncmp(path, "../", 3) == 0) {
        if (f->parent) f = f->parent;
        path += 3;
    }
    if (path[0] == '@') {
        if (strcmp(path, "@root") == 0) return r->root;
        if (str_starts(path, "@root.")) {
            return tpl_path(r, r->root, path + 6);
        }
        for (frame_t *x = f; x; x = x->parent)
            if (x->data) return json_get(x->data, path + 1);
        return NULL;
    }
    if (str_starts(path, "this.")) path += 5;
    /* block params */
    for (frame_t *x = f; x; x = x->parent) {
        for (int i = 0; i < 2; i++) {
            if (!x->bp_names[i]) continue;
            size_t n = strlen(x->bp_names[i]);
            if (strncmp(path, x->bp_names[i], n) == 0 && (path[n] == 0 || path[n] == '.')) {
                const json_t *v = x->bp_vals[i];
                if (path[n] == 0) return v;
                return tpl_path(r, v, path + n + 1);
            }
        }
    }
    return tpl_path(r, f->ctx, path);
}

static const json_t *eval_param(rctx_t *r, frame_t *f, param_t *p);
static json_t *call_helper(rctx_t *r, frame_t *f, const char *name, params_t *args, hashes_t *hash, bool *found);

static const json_t *eval_param(rctx_t *r, frame_t *f, param_t *p) {
    switch (p->kind) {
    case P_STR: return keep(r, json_str(p->s));
    case P_NUM: return keep(r, json_num(p->num));
    case P_BOOL: return keep(r, json_bool(p->b));
    case P_NULL: return NULL;
    case P_PATH: return lookup_path(r, f, p->s);
    case P_SUB: {
        bool found;
        json_t *v = call_helper(r, f, p->helper, &p->args, &p->hash, &found);
        if (!found) return lookup_path(r, f, p->helper);
        return v ? keep(r, v) : NULL;
    }
    }
    return NULL;
}

static void stringify(sb_t *b, const json_t *v) {
    if (!v) return;
    switch (v->type) {
    case JSON_NULL: break;
    case JSON_STRING: sb_appendn(b, v->u.str.s, v->u.str.len); break;
    case JSON_BOOL: sb_append(b, v->u.b ? "true" : "false"); break;
    case JSON_NUMBER: {
        char *s = json_dump(v, 0);
        sb_append(b, s);
        free(s);
        break;
    }
    case JSON_ARRAY:
        /* JS Array.toString: comma-joined */
        for (size_t i = 0; i < v->u.arr.n; i++) {
            if (i) sb_putc(b, ',');
            stringify(b, v->u.arr.items[i]);
        }
        break;
    case JSON_OBJECT: sb_append(b, "[object Object]"); break;
    }
}

static const char *hash_str(rctx_t *r, frame_t *f, hashes_t *h, const char *key) {
    for (size_t i = 0; i < h->n; i++)
        if (strcmp(h->v[i].key, key) == 0) return json_sv(eval_param(r, f, h->v[i].val));
    return NULL;
}

static char *unescape_nt(const char *s) {
    char *a = str_replace(s, "\\n", "\n");
    char *b = str_replace(a, "\\t", "\t");
    free(a);
    return b;
}

static double num_of(const json_t *j) {
    if (json_is_num(j)) return j->u.num.d;
    if (json_is_str(j)) return strtod(json_sv(j), NULL);
    if (json_is_bool(j)) return j->u.b;
    return 0;
}

static bool js_eq(const json_t *a, const json_t *b) {
    if (!a || a->type == JSON_NULL) return !b || b->type == JSON_NULL;
    if (!b) return false;
    if (a->type != b->type) return false;
    if (a->type == JSON_ARRAY || a->type == JSON_OBJECT) return a == b;
    return json_equal(a, b);
}

/* simple (non-block) helpers; returns owned value, *found=false if no such helper */
static json_t *call_helper(rctx_t *r, frame_t *f, const char *name, params_t *args, hashes_t *hash, bool *found) {
    *found = true;
    const json_t *a0 = args->n > 0 ? eval_param(r, f, args->v[0]) : NULL;
    const json_t *a1 = args->n > 1 ? eval_param(r, f, args->v[1]) : NULL;
    if (strcmp(name, "arg") == 0) {
        json_t *argv = json_get(f->ctx, "args");
        int idx = (int)num_of(a0) - 1;
        json_t *v = idx >= 0 ? json_at(argv, (size_t)idx) : NULL;
        return json_str(json_is_str(v) ? json_sv(v) : "");
    }
    if (strcmp(name, "join") == 0) {
        if (!json_is_arr(a0)) return json_str("");
        char *sep = json_is_str(a1) ? unescape_nt(json_sv(a1)) : xstrdup(", ");
        sb_t b = SB_INIT;
        for (size_t i = 0; i < json_len(a0); i++) {
            if (i) sb_append(&b, sep);
            stringify(&b, json_at(a0, i));
        }
        free(sep);
        return json_str_own(sb_detach(&b));
    }
    if (strcmp(name, "default") == 0) return json_clone(truthy(a0) ? a0 : a1);
    if (strcmp(name, "pluralize") == 0) {
        const json_t *a2 = args->n > 2 ? eval_param(r, f, args->v[2]) : NULL;
        double c = num_of(a0);
        sb_t b = SB_INIT;
        stringify(&b, a0);
        sb_putc(&b, ' ');
        stringify(&b, c == 1 ? a1 : a2);
        return json_str_own(sb_detach(&b));
    }
    if (strcmp(name, "escapeXml") == 0) {
        sb_t b = SB_INIT;
        sb_t raw = SB_INIT;
        stringify(&raw, a0);
        for (const char *p = sb_cstr(&raw); *p; p++) {
            switch (*p) {
            case '&': sb_append(&b, "&amp;"); break;
            case '<': sb_append(&b, "&lt;"); break;
            case '>': sb_append(&b, "&gt;"); break;
            case '"': sb_append(&b, "&quot;"); break;
            default: sb_putc(&b, *p);
            }
        }
        sb_free(&raw);
        return json_str_own(sb_detach(&b));
    }
    if (strcmp(name, "len") == 0) {
        if (json_is_arr(a0)) return json_int((int64_t)json_len(a0));
        if (json_is_str(a0)) return json_int((int64_t)utf8_strlen(json_sv(a0)));
        return json_int(0);
    }
    if (strcmp(name, "add") == 0) return json_num(num_of(a0) + num_of(a1));
    if (strcmp(name, "sub") == 0) return json_num(num_of(a0) - num_of(a1));
    if (strcmp(name, "includes") == 0) {
        bool inc = false;
        json_t *it;
        json_arr_foreach(a0, i, it) if (js_eq(it, a1)) inc = true;
        return json_bool(inc);
    }
    if (strcmp(name, "not") == 0) return json_bool(!truthy(a0));
    if (strcmp(name, "eq") == 0) return json_bool(js_eq(a0, a1));
    if (strcmp(name, "ne") == 0) return json_bool(!js_eq(a0, a1));
    if (strcmp(name, "gt") == 0) return json_bool(num_of(a0) > num_of(a1));
    if (strcmp(name, "lt") == 0) return json_bool(num_of(a0) < num_of(a1));
    if (strcmp(name, "and") == 0) {
        bool all = true;
        for (size_t i = 0; i < args->n; i++) all = all && truthy(eval_param(r, f, args->v[i]));
        return json_bool(all);
    }
    if (strcmp(name, "or") == 0) {
        for (size_t i = 0; i < args->n; i++) {
            const json_t *v = eval_param(r, f, args->v[i]);
            if (truthy(v)) return json_clone(v);
        }
        return json_bool(false);
    }
    if (strcmp(name, "jsonStringify") == 0) {
        if (!a0) return json_str("undefined");
        return json_str_own(json_dump(a0, 0));
    }
    if (strcmp(name, "lookup") == 0) {
        if (json_is_obj(a0) && json_is_str(a1)) return json_clone(json_get(a0, json_sv(a1)));
        if (json_is_arr(a0)) return json_clone(json_at(a0, (size_t)num_of(a1)));
        return NULL;
    }
    UNUSED(hash);
    *found = false;
    return NULL;
}

static void render_nodes(rctx_t *r, frame_t *f, nodes_t *nodes, sb_t *out);

static char *render_to_str(rctx_t *r, frame_t *f, nodes_t *nodes) {
    sb_t b = SB_INIT;
    render_nodes(r, f, nodes, &b);
    return sb_detach(&b);
}

static void render_with(rctx_t *r, frame_t *parent, const json_t *ctx, const json_t *data, nodes_t *nodes, sb_t *out,
                        node_t *n, const json_t *bp0, const json_t *bp1) {
    frame_t fr = {.ctx = ctx, .data = data, .parent = parent};
    if (n) {
        fr.bp_names[0] = n->block_params[0];
        fr.bp_names[1] = n->block_params[1];
        fr.bp_vals[0] = bp0;
        fr.bp_vals[1] = bp1;
    }
    render_nodes(r, &fr, nodes, out);
}

static void render_block(rctx_t *r, frame_t *f, node_t *n, sb_t *out) {
    const char *name = n->name;
    const json_t *a0 = n->args.n > 0 ? eval_param(r, f, n->args.v[0]) : NULL;
    if (n->inverted) {
        const json_t *v = lookup_path(r, f, name);
        if (!truthy(v)) render_nodes(r, f, &n->body, out);
        else render_nodes(r, f, &n->inverse, out);
        return;
    }
    if (strcmp(name, "if") == 0) {
        render_nodes(r, f, truthy(a0) ? &n->body : &n->inverse, out);
        return;
    }
    if (strcmp(name, "unless") == 0) {
        render_nodes(r, f, !truthy(a0) ? &n->body : &n->inverse, out);
        return;
    }
    if (strcmp(name, "with") == 0) {
        if (truthy(a0)) render_with(r, f, a0, NULL, &n->body, out, n, a0, NULL);
        else render_nodes(r, f, &n->inverse, out);
        return;
    }
    if (strcmp(name, "each") == 0) {
        size_t cnt = json_len(a0);
        if (!(json_is_arr(a0) || json_is_obj(a0)) || cnt == 0) {
            render_nodes(r, f, &n->inverse, out);
            return;
        }
        for (size_t i = 0; i < cnt; i++) {
            json_t *data = json_obj();
            json_set_int(data, "index", (int64_t)i);
            json_set_bool(data, "first", i == 0);
            json_set_bool(data, "last", i + 1 == cnt);
            const json_t *item;
            json_t *keyj = NULL;
            if (json_is_arr(a0)) {
                item = json_at(a0, i);
            } else {
                item = a0->u.obj.vals[i];
                json_set_str(data, "key", a0->u.obj.keys[i]);
                keyj = json_get(data, "key");
            }
            json_t *idx = json_get(data, "index");
            render_with(r, f, item, data, &n->body, out, n, item, keyj ? keyj : idx);
            json_free(data);
        }
        return;
    }
    if (strcmp(name, "list") == 0) {
        if (!json_is_arr(a0) || !json_len(a0)) return;
        const char *prefix = hash_str(r, f, &n->hash, "prefix");
        const char *suffix = hash_str(r, f, &n->hash, "suffix");
        const char *join = hash_str(r, f, &n->hash, "join");
        char *sep = unescape_nt(join ? join : "\n");
        for (size_t i = 0; i < json_len(a0); i++) {
            if (i) sb_append(out, sep);
            if (prefix) sb_append(out, prefix);
            sb_t tmp = SB_INIT;
            render_with(r, f, json_at(a0, i), NULL, &n->body, &tmp, n, json_at(a0, i), NULL);
            sb_append(out, sb_cstr(&tmp));
            sb_free(&tmp);
            if (suffix) sb_append(out, suffix);
        }
        free(sep);
        return;
    }
    if (strcmp(name, "when") == 0) {
        const json_t *op = n->args.n > 1 ? eval_param(r, f, n->args.v[1]) : NULL;
        const json_t *rhs = n->args.n > 2 ? eval_param(r, f, n->args.v[2]) : NULL;
        const char *o = json_sv(op);
        bool res = false, known = true;
        if (!o) known = false;
        else if (!strcmp(o, "==") || !strcmp(o, "===")) res = js_eq(a0, rhs);
        else if (!strcmp(o, "!=") || !strcmp(o, "!==")) res = !js_eq(a0, rhs);
        else if (!strcmp(o, ">")) res = num_of(a0) > num_of(rhs);
        else if (!strcmp(o, "<")) res = num_of(a0) < num_of(rhs);
        else if (!strcmp(o, ">=")) res = num_of(a0) >= num_of(rhs);
        else if (!strcmp(o, "<=")) res = num_of(a0) <= num_of(rhs);
        else known = false;
        render_nodes(r, f, known && res ? &n->body : &n->inverse, out);
        return;
    }
    if (strcmp(name, "ifAny") == 0 || strcmp(name, "ifAll") == 0) {
        bool any = false, all = true;
        for (size_t i = 0; i < n->args.n; i++) {
            bool t = truthy(eval_param(r, f, n->args.v[i]));
            any = any || t;
            all = all && t;
        }
        bool res = name[2] == 'A' && name[3] == 'n' ? any : all;
        render_nodes(r, f, res ? &n->body : &n->inverse, out);
        return;
    }
    if (strcmp(name, "has") == 0) {
        const json_t *item = n->args.n > 1 ? eval_param(r, f, n->args.v[1]) : NULL;
        bool found = false;
        if (json_is_arr(a0)) {
            json_t *it;
            json_arr_foreach(a0, i, it) if (js_eq(it, item)) found = true;
        } else if (json_is_obj(a0) && json_is_str(item)) {
            found = json_get(a0, json_sv(item)) != NULL;
        }
        render_nodes(r, f, found ? &n->body : &n->inverse, out);
        return;
    }
    if (strcmp(name, "table") == 0) {
        if (!json_is_arr(a0) || !json_len(a0)) return;
        const char *hs = hash_str(r, f, &n->hash, "headers");
        if (hs) {
            strvec_t hv = str_split(hs, "|");
            sb_append(out, "| ");
            for (size_t i = 0; i < hv.n; i++) sb_appendf(out, "%s%s", i ? " | " : "", hv.v[i]);
            sb_append(out, " |\n| ");
            for (size_t i = 0; i < hv.n; i++) sb_appendf(out, "%s---", i ? " | " : "");
            sb_append(out, " |\n");
            strvec_free(&hv);
        }
        for (size_t i = 0; i < json_len(a0); i++) {
            if (i) sb_putc(out, '\n');
            sb_t tmp = SB_INIT;
            render_with(r, f, json_at(a0, i), NULL, &n->body, &tmp, n, json_at(a0, i), NULL);
            char *t = str_trim(sb_cstr(&tmp));
            sb_appendf(out, "| %s |", t);
            free(t);
            sb_free(&tmp);
        }
        return;
    }
    if (strcmp(name, "codeblock") == 0) {
        const char *lang = hash_str(r, f, &n->hash, "lang");
        char *body = render_to_str(r, f, &n->body);
        char *t = str_trim(body);
        sb_appendf(out, "```%s\n%s\n```", lang ? lang : "", t);
        free(t);
        free(body);
        return;
    }
    if (strcmp(name, "xml") == 0) {
        char *body = render_to_str(r, f, &n->body);
        char *t = str_trim(body);
        if (*t) sb_appendf(out, "<%s>\n%s\n</%s>", json_sv(a0) ? json_sv(a0) : "", t, json_sv(a0) ? json_sv(a0) : "");
        free(t);
        free(body);
        return;
    }
    /* default: mustache-style section on a path */
    const json_t *v = n->args.n ? a0 : lookup_path(r, f, name);
    if (json_is_arr(v)) {
        if (!json_len(v)) render_nodes(r, f, &n->inverse, out);
        for (size_t i = 0; i < json_len(v); i++) render_with(r, f, json_at(v, i), NULL, &n->body, out, n, json_at(v, i), NULL);
    } else if (truthy(v)) {
        render_with(r, f, json_is_obj(v) ? v : f->ctx, NULL, &n->body, out, n, v, NULL);
    } else {
        render_nodes(r, f, &n->inverse, out);
    }
}

static void render_nodes(rctx_t *r, frame_t *f, nodes_t *nodes, sb_t *out) {
    if (++r->depth > 200) {
        r->depth--;
        return;
    }
    for (size_t i = 0; i < nodes->n; i++) {
        node_t *n = nodes->v[i];
        if (n->kind == N_TEXT) {
            sb_append(out, n->text);
        } else if (n->kind == N_VAR) {
            bool found = false;
            json_t *hv = NULL;
            if (n->args.n || n->hash.n) hv = call_helper(r, f, n->name, &n->args, &n->hash, &found);
            if (!found && !n->args.n) hv = call_helper(r, f, n->name, &n->args, &n->hash, &found);
            if (found) {
                stringify(out, hv);
                json_free(hv);
            } else {
                stringify(out, lookup_path(r, f, n->name));
            }
        } else {
            render_block(r, f, n, out);
        }
    }
    r->depth--;
}

char *tpl_render_raw(const char *tpl, const json_t *ctx) {
    toks_t toks = {0};
    tokenize(tpl, &toks);
    apply_whitespace(&toks);
    parser_t ps = {&toks, 0};
    nodes_t root = {0};
    parse_nodes(&ps, &root, 0);
    /* stray closers/else at top level: skip and continue */
    while (ps.i < toks.n) {
        ps.i++;
        parse_nodes(&ps, &root, 0);
    }
    json_t *empty = NULL;
    if (!ctx) ctx = empty = json_obj();
    rctx_t r = {.root = ctx};
    frame_t f = {.ctx = ctx};
    sb_t out = SB_INIT;
    render_nodes(&r, &f, &root, &out);
    for (size_t i = 0; i < r.temps.n; i++) json_free(r.temps.v[i]);
    vec_free(&r.temps);
    nodes_free(&root);
    for (size_t i = 0; i < toks.n; i++) free(toks.v[i].text);
    vec_free(&toks);
    json_free(empty);
    return sb_detach(&out);
}

/* ---------------- format pass ---------------- */

static bool is_table_sep(const char *s) {
    size_t n = strlen(s);
    if (n < 3 || s[0] != '|' || s[n - 1] != '|') return false;
    for (size_t i = 1; i < n - 1; i++)
        if (!strchr("-:| \t", s[i])) return false;
    return true;
}

static bool is_closing_tag(const char *s) {
    size_t n = strlen(s);
    if (n < 4 || s[0] != '<' || s[1] != '/' || s[n - 1] != '>') return false;
    for (size_t j = 2; j < n - 1; j++)
        if (!(islower((unsigned char)s[j]) || s[j] == '-' || s[j] == '_')) return false;
    return true;
}

char *prompt_format(const char *text) {
    strvec_t lines = str_split(text, "\n");
    strvec_t res = {0};
    bool in_code = false;
    for (size_t i = 0; i < lines.n; i++) {
        char *line = lines.v[i];
        size_t len = strlen(line);
        while (len && isspace((unsigned char)line[len - 1])) line[--len] = 0;
        size_t s = 0;
        while (line[s] == ' ' || line[s] == '\t') s++;
        if (strncmp(line + s, "```", 3) == 0 || strncmp(line + s, "~~~", 3) == 0) {
            in_code = !in_code;
            strvec_push(&res, line);
            continue;
        }
        if (in_code) {
            strvec_push(&res, line);
            continue;
        }
        char *owned = NULL;
        bool closing = false;
        if (line[s] == '<' && line[s + 1] == '/') {
            closing = is_closing_tag(line + s);
        } else if (line[s] == '|') {
            if (is_table_sep(line + s)) {
                strvec_t cells = str_split(line + s, "|");
                sb_t b = SB_INIT;
                sb_appendn(&b, line, s);
                sb_putc(&b, '|');
                for (size_t c = 0; c < cells.n; c++) {
                    char *t = str_trim(cells.v[c]);
                    if (*t) {
                        bool l = t[0] == ':', r2 = t[strlen(t) - 1] == ':';
                        sb_append(&b, l && r2 ? ":---:" : l ? ":---" : r2 ? "---:" : "---");
                        sb_putc(&b, '|');
                    }
                    free(t);
                }
                strvec_free(&cells);
                owned = sb_detach(&b);
            } else if (len - s >= 2 && line[len - 1] == '|') {
                strvec_t cells = str_split(line + s, "|");
                sb_t b = SB_INIT;
                sb_appendn(&b, line, s);
                for (size_t c = 0; c < cells.n; c++) {
                    char *t = str_trim(cells.v[c]);
                    if (c) sb_putc(&b, '|');
                    sb_append(&b, t);
                    free(t);
                }
                strvec_free(&cells);
                owned = sb_detach(&b);
            }
        }
        const char *L = owned ? owned : line;
        if (!*L) {
            /* blank line: drop runs, never leading, at most one */
            bool next_blank = i + 1 >= lines.n;
            if (!next_blank) {
                const char *nx = lines.v[i + 1];
                next_blank = true;
                for (const char *p = nx; *p; p++)
                    if (!isspace((unsigned char)*p)) next_blank = false;
            }
            if (next_blank) {
                while (res.n && !*vec_last(&res)) free(vec_pop(&res));
                size_t j = i + 1;
                while (j < lines.n) {
                    bool blank = true;
                    for (const char *p = lines.v[j]; *p; p++)
                        if (!isspace((unsigned char)*p)) blank = false;
                    if (!blank) break;
                    j++;
                }
                i = j - 1;
                free(owned);
                continue;
            }
            if (!res.n || !*vec_last(&res)) {
                free(owned);
                continue;
            }
        }
        if (closing) {
            while (res.n && !*vec_last(&res)) free(vec_pop(&res));
        }
        strvec_push(&res, L);
        free(owned);
    }
    while (res.n && !*vec_last(&res)) free(vec_pop(&res));
    char *out = strvec_join(&res, "\n");
    strvec_free(&lines);
    strvec_free(&res);
    return out;
}

char *tpl_render(const char *tpl, const json_t *ctx) {
    char *raw = tpl_render_raw(tpl, ctx);
    char *f = prompt_format(raw);
    free(raw);
    return f;
}

char *prompt_load(const char *rel) {
    char *override = xasprintf("%s/prompts/%s", omc_agent_dir(), rel);
    if (fs_is_file(override)) {
        char *s = fs_read(override, NULL);
        free(override);
        if (s) return s;
    } else {
        free(override);
    }
    char *name = xasprintf("prompts/%s", rel);
    size_t len;
    const char *d = embed_get(name, &len);
    free(name);
    return d ? xstrndup(d, len) : NULL;
}

char *prompt_render(const char *rel, const json_t *ctx) {
    char *t = prompt_load(rel);
    if (!t) return xstrdup("");
    char *r = tpl_render(t, ctx);
    free(t);
    return r;
}

char *prompt_load_or(const char *rel, const char *fallback) {
    char *t = prompt_load(rel);
    if (!t) return xstrdup(fallback ? fallback : "");
    char *r = tpl_render(t, NULL);
    free(t);
    return r;
}
