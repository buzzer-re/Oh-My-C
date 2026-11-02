/* yaml.c - YAML subset parser / emitter. See yaml.h. */
#include "text/yaml.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>

#define YAML_MAX_DEPTH 256
#define YAML_MAX_ALIAS_NODES 1000000

typedef struct {
    const char *s;
    size_t len, pos;
    char *err;
    int depth;
    hmap_t anchors; /* name -> json_t* (owned) */
    size_t alias_nodes;
    bool force_str; /* !!str tag pending for the next scalar */
    bool inline_value; /* next parse_node is a mapping value on the key's line */
} yp_t;

static void yerr_at(yp_t *y, size_t at, const char *fmt, ...) PRINTF_LIKE(3, 4);
static void yerr_at(yp_t *y, size_t at, const char *fmt, ...) {
    if (y->err) return;
    if (at > y->len) at = y->len;
    size_t line = 1, ls = 0;
    for (size_t i = 0; i < at; i++)
        if (y->s[i] == '\n') { line++; ls = i + 1; }
    va_list ap;
    va_start(ap, fmt);
    char *m = xvasprintf(fmt, ap);
    va_end(ap);
    y->err = xasprintf("line %zu, column %zu: %s", line, at - ls + 1, m);
    free(m);
}
#define yerr(y, ...) yerr_at((y), (y)->pos, __VA_ARGS__)

static inline char cur(const yp_t *y) { return y->pos < y->len ? y->s[y->pos] : 0; }
static inline char at(const yp_t *y, size_t off) { return y->pos + off < y->len ? y->s[y->pos + off] : 0; }
static inline bool eof(const yp_t *y) { return y->pos >= y->len; }
static inline bool is_brk(char c) { return c == '\n' || c == '\r'; }
static inline bool is_blank_or_end(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0; }
static inline bool is_flow_ind(char c) { return c == ',' || c == '[' || c == ']' || c == '{' || c == '}'; }

static size_t line_start(const yp_t *y, size_t p) {
    while (p > 0 && y->s[p - 1] != '\n') p--;
    return p;
}
static size_t col(const yp_t *y) { return y->pos - line_start(y, y->pos); }

static void skip_space(yp_t *y) {
    while (!eof(y) && (cur(y) == ' ' || cur(y) == '\t')) y->pos++;
}
static void skip_line(yp_t *y) {
    while (!eof(y) && cur(y) != '\n') y->pos++;
    if (!eof(y)) y->pos++;
}
static bool at_eol(yp_t *y) {
    char c = cur(y);
    return c == 0 || is_brk(c) || c == '#';
}
static void skip_brk(yp_t *y) {
    if (cur(y) == '\r') y->pos++;
    if (cur(y) == '\n') y->pos++;
}

/* skip whitespace, comments and line breaks; error on tab indentation before content */
static void skip_to_content(yp_t *y) {
    for (;;) {
        size_t ls = line_start(y, y->pos);
        bool at_ls = y->pos == ls;
        size_t p = y->pos;
        while (p < y->len && y->s[p] == ' ') p++;
        if (at_ls && p < y->len && y->s[p] == '\t') {
            size_t q = p;
            while (q < y->len && (y->s[q] == ' ' || y->s[q] == '\t')) q++;
            if (q < y->len && !is_brk(y->s[q]) && y->s[q] != '#') {
                y->pos = p;
                yerr(y, "tabs are not allowed for indentation");
                return;
            }
        }
        y->pos = p;
        skip_space(y);
        if (cur(y) == '#') {
            while (!eof(y) && !is_brk(cur(y))) y->pos++;
        }
        if (is_brk(cur(y))) {
            skip_brk(y);
            continue;
        }
        return;
    }
}

static bool is_doc_marker(const yp_t *y) {
    if (y->pos != line_start(y, y->pos) || y->len - y->pos < 3) return false;
    const char *p = y->s + y->pos;
    if (!(memcmp(p, "---", 3) == 0 || memcmp(p, "...", 3) == 0)) return false;
    return y->pos + 3 >= y->len || is_blank_or_end(p[3]);
}

/* ---------------- scalar resolution ---------------- */
static json_t *resolve_plain(const char *t, bool force_str) {
    if (force_str) return json_str(t);
    size_t n = strlen(t);
    if (n == 0 || strcmp(t, "~") == 0 || strcmp(t, "null") == 0 || strcmp(t, "Null") == 0 || strcmp(t, "NULL") == 0)
        return json_null();
    if (strcmp(t, "true") == 0 || strcmp(t, "True") == 0 || strcmp(t, "TRUE") == 0) return json_bool(true);
    if (strcmp(t, "false") == 0 || strcmp(t, "False") == 0 || strcmp(t, "FALSE") == 0) return json_bool(false);
    const char *p = t;
    bool neg = false;
    if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'o') && p[2] && p == t) {
        int base = p[1] == 'x' ? 16 : 8;
        char *end;
        errno = 0;
        unsigned long long v = strtoull(p + 2, &end, base);
        if (*end == 0 && errno == 0 && v <= (unsigned long long)INT64_MAX) return json_int((int64_t)v);
        if (*end == 0) return json_num((double)v);
        return json_str(t);
    }
    if (strcmp(p, ".inf") == 0 || strcmp(p, ".Inf") == 0 || strcmp(p, ".INF") == 0) return json_num(neg ? -INFINITY : INFINITY);
    if (p == t && (strcmp(p, ".nan") == 0 || strcmp(p, ".NaN") == 0 || strcmp(p, ".NAN") == 0)) return json_num(NAN);
    /* int: [-+]?[0-9]+ */
    const char *q = p;
    while (isdigit((unsigned char)*q)) q++;
    if (q > p && *q == 0) {
        char *end;
        errno = 0;
        long long v = strtoll(t, &end, 10);
        if (errno == 0 && *end == 0) return json_int((int64_t)v);
        return json_num(strtod(t, NULL));
    }
    /* float: [-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)? */
    q = p;
    bool digits = false;
    while (isdigit((unsigned char)*q)) { q++; digits = true; }
    if (*q == '.') {
        q++;
        while (isdigit((unsigned char)*q)) { q++; digits = true; }
    }
    if (digits && (*q == 'e' || *q == 'E')) {
        const char *e = q + 1;
        if (*e == '-' || *e == '+') e++;
        if (isdigit((unsigned char)*e)) {
            while (isdigit((unsigned char)*e)) e++;
            q = e;
        }
    }
    if (digits && *q == 0) return json_num(strtod(t, NULL));
    return json_str(t);
}

/* ---------------- forward decls ---------------- */
static json_t *parse_node(yp_t *y, int n);
static json_t *parse_value_block(yp_t *y, int n, bool seq_ok);
static json_t *parse_flow(yp_t *y);

static size_t json_count(const json_t *j, size_t limit) {
    size_t c = 1;
    if (!j || c > limit) return c;
    if (j->type == JSON_ARRAY)
        for (size_t i = 0; i < j->u.arr.n && c <= limit; i++) c += json_count(j->u.arr.items[i], limit);
    else if (j->type == JSON_OBJECT)
        for (size_t i = 0; i < j->u.obj.n && c <= limit; i++) c += json_count(j->u.obj.vals[i], limit);
    return c;
}

static char *read_name(yp_t *y) {
    size_t st = y->pos;
    while (!eof(y) && !is_blank_or_end(cur(y)) && !is_flow_ind(cur(y))) y->pos++;
    return xstrndup(y->s + st, y->pos - st);
}

static json_t *parse_alias(yp_t *y) {
    size_t st = y->pos;
    y->pos++;
    char *name = read_name(y);
    json_t *a = name[0] ? hmap_get(&y->anchors, name) : NULL;
    if (!a) {
        yerr_at(y, st, "unknown alias '*%s'", name);
        free(name);
        return NULL;
    }
    free(name);
    y->alias_nodes += json_count(a, YAML_MAX_ALIAS_NODES);
    if (y->alias_nodes > YAML_MAX_ALIAS_NODES) {
        yerr_at(y, st, "alias expansion exceeds limit");
        return NULL;
    }
    return json_clone(a);
}

static void set_anchor(yp_t *y, char *name, const json_t *v) {
    json_t *old = hmap_put(&y->anchors, name, json_clone(v));
    json_free(old);
    free(name);
}

/* ---------------- quoted scalars ---------------- */
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Fold a line break inside a quoted scalar; y->pos at the break. */
static void fold_break(yp_t *y, sb_t *out, size_t protect) {
    while (out->len > protect && (out->s[out->len - 1] == ' ' || out->s[out->len - 1] == '\t')) out->len--;
    if (out->s) out->s[out->len] = 0;
    skip_brk(y);
    int blanks = 0;
    for (;;) {
        size_t p = y->pos;
        while (p < y->len && (y->s[p] == ' ' || y->s[p] == '\t')) p++;
        if (p < y->len && is_brk(y->s[p])) {
            y->pos = p;
            skip_brk(y);
            blanks++;
            continue;
        }
        y->pos = p;
        break;
    }
    if (blanks == 0) sb_putc(out, ' ');
    else for (int i = 0; i < blanks; i++) sb_putc(out, '\n');
}

static char *parse_quoted(yp_t *y) {
    char q = cur(y);
    size_t st = y->pos;
    y->pos++;
    sb_t out = SB_INIT;
    size_t protect = 0;
    for (;;) {
        if (eof(y)) {
            yerr_at(y, st, "unterminated quoted scalar");
            sb_free(&out);
            return NULL;
        }
        char c = cur(y);
        if (q == '\'' && c == '\'') {
            if (at(y, 1) == '\'') {
                sb_putc(&out, '\'');
                y->pos += 2;
                protect = out.len;
                continue;
            }
            y->pos++;
            break;
        }
        if (q == '"' && c == '"') {
            y->pos++;
            break;
        }
        if (is_brk(c)) {
            if (is_doc_marker(y)) { yerr(y, "unterminated quoted scalar"); sb_free(&out); return NULL; }
            fold_break(y, &out, protect);
            continue;
        }
        if (q == '"' && c == '\\') {
            y->pos++;
            char e = cur(y);
            if (eof(y)) continue;
            y->pos++;
            int hexn = 0;
            switch (e) {
            case '0': sb_putc(&out, '\0'); break;
            case 'a': sb_putc(&out, '\a'); break;
            case 'b': sb_putc(&out, '\b'); break;
            case 't': case '\t': sb_putc(&out, '\t'); break;
            case 'n': sb_putc(&out, '\n'); break;
            case 'v': sb_putc(&out, '\v'); break;
            case 'f': sb_putc(&out, '\f'); break;
            case 'r': sb_putc(&out, '\r'); break;
            case 'e': sb_putc(&out, 27); break;
            case ' ': sb_putc(&out, ' '); break;
            case '"': sb_putc(&out, '"'); break;
            case '/': sb_putc(&out, '/'); break;
            case '\\': sb_putc(&out, '\\'); break;
            case 'N': sb_append_utf8(&out, 0x85); break;
            case '_': sb_append_utf8(&out, 0xA0); break;
            case 'L': sb_append_utf8(&out, 0x2028); break;
            case 'P': sb_append_utf8(&out, 0x2029); break;
            case 'x': hexn = 2; break;
            case 'u': hexn = 4; break;
            case 'U': hexn = 8; break;
            case '\r': case '\n':
                /* escaped line break: join without space, skip leading whitespace */
                if (e == '\r' && cur(y) == '\n') y->pos++;
                while (!eof(y) && (cur(y) == ' ' || cur(y) == '\t')) y->pos++;
                break;
            default:
                yerr_at(y, y->pos - 2, "invalid escape '\\%c' in double-quoted scalar", e);
                sb_free(&out);
                return NULL;
            }
            if (hexn) {
                uint32_t v = 0;
                for (int i = 0; i < hexn; i++) {
                    int h = hexv(cur(y));
                    if (h < 0) {
                        yerr(y, "invalid hex escape");
                        sb_free(&out);
                        return NULL;
                    }
                    v = v * 16 + (uint32_t)h;
                    y->pos++;
                }
                if (hexn == 2) sb_putc(&out, (char)v);
                else if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) sb_append_utf8(&out, 0xFFFD);
                else sb_append_utf8(&out, v);
            }
            protect = out.len;
            continue;
        }
        sb_putc(&out, c);
        y->pos++;
    }
    if (!out.s) return xstrdup("");
    /* embedded NULs from \0 are kept by length; callers use strlen -> truncate (documented gap) */
    return sb_detach(&out);
}

/* ---------------- plain scalars (block context) ---------------- */
/* Scan a plain scalar's first line from y->pos. Stops before ": ", " #", EOL. */
static size_t scan_plain_line(const yp_t *y, size_t p, bool flow) {
    size_t e = p;
    while (e < y->len) {
        char c = y->s[e];
        if (is_brk(c)) break;
        if (c == ':') {
            char nx = e + 1 < y->len ? y->s[e + 1] : 0;
            if (is_blank_or_end(nx) || (flow && is_flow_ind(nx))) break;
        }
        if (c == '#' && e > p && (y->s[e - 1] == ' ' || y->s[e - 1] == '\t')) break;
        if (flow && is_flow_ind(c)) break;
        e++;
    }
    while (e > p && (y->s[e - 1] == ' ' || y->s[e - 1] == '\t')) e--;
    return e;
}

/* Continue a plain scalar on following lines more indented than n. */
static void plain_continue(yp_t *y, sb_t *out, int n) {
    for (;;) {
        size_t save = y->pos;
        size_t p = y->pos;
        while (p < y->len && (y->s[p] == ' ' || y->s[p] == '\t')) p++;
        if (p < y->len && y->s[p] == '#') break;
        if (p >= y->len || !is_brk(y->s[p])) break;
        int blanks = 0;
        y->pos = p;
        skip_brk(y);
        for (;;) {
            size_t q = y->pos;
            while (q < y->len && y->s[q] == ' ') q++;
            size_t r = q;
            while (r < y->len && y->s[r] == '\t') r++;
            if (r < y->len && is_brk(y->s[r])) {
                y->pos = r;
                skip_brk(y);
                blanks++;
                continue;
            }
            break;
        }
        size_t ls = y->pos;
        size_t q = ls;
        while (q < y->len && y->s[q] == ' ') q++;
        if (q >= y->len || (int)(q - ls) <= n || y->s[q] == '#' || is_doc_marker(y)) {
            y->pos = save;
            return;
        }
        while (q < y->len && y->s[q] == '\t') q++;
        size_t e = scan_plain_line(y, q, false);
        if (e == q) {
            y->pos = save;
            return;
        }
        if (e < y->len && y->s[e] == ':') {
            /* looks like a mapping key: not part of this scalar */
            y->pos = save;
            return;
        }
        if (blanks == 0) sb_putc(out, ' ');
        else for (int i = 0; i < blanks; i++) sb_putc(out, '\n');
        sb_appendn(out, y->s + q, e - q);
        y->pos = e;
    }
}

/* ---------------- block scalars ---------------- */
static json_t *parse_block_scalar(yp_t *y, int n) {
    bool literal = cur(y) == '|';
    y->pos++;
    int chomp = 0; /* 0 clip, -1 strip, 1 keep */
    int explicit_ind = 0;
    for (int k = 0; k < 2; k++) {
        char c = cur(y);
        if (c >= '1' && c <= '9' && !explicit_ind) { explicit_ind = c - '0'; y->pos++; }
        else if (c == '-' && !chomp) { chomp = -1; y->pos++; }
        else if (c == '+' && !chomp) { chomp = 1; y->pos++; }
    }
    skip_space(y);
    if (cur(y) == '#') while (!eof(y) && !is_brk(cur(y))) y->pos++;
    if (!eof(y) && !is_brk(cur(y))) {
        yerr(y, "invalid block scalar header");
        return NULL;
    }
    skip_brk(y);
    int base = n < 0 ? 0 : n;
    int indent = explicit_ind ? base + explicit_ind : -1;
    strvec_t lines = {0};
    int trailing_blank = 0;
    for (;;) {
        if (eof(y)) break;
        size_t ls = y->pos;
        size_t p = ls;
        while (p < y->len && y->s[p] == ' ') p++;
        int sp = (int)(p - ls);
        bool blank = p >= y->len || is_brk(y->s[p]);
        if (blank) {
            if (indent >= 0 && sp > indent) {
                /* whitespace-only line with extra spaces: content */
                trailing_blank++;
            } else {
                trailing_blank++;
            }
            y->pos = p;
            skip_brk(y);
            continue;
        }
        if (indent < 0) {
            if (sp <= n) break;
            indent = sp;
        }
        if (sp < indent || is_doc_marker(y)) {
            y->pos = ls;
            break;
        }
        for (int i = 0; i < trailing_blank; i++) strvec_push(&lines, "");
        trailing_blank = 0;
        size_t cs = ls + (size_t)indent;
        size_t e = cs;
        while (e < y->len && !is_brk(y->s[e])) e++;
        strvec_push_own(&lines, xstrndup(y->s + cs, e - cs));
        y->pos = e;
        skip_brk(y);
    }
    /* if we stopped at a non-content line, the blank lines before it were consumed: rewind to its start */
    sb_t out = SB_INIT;
    if (literal) {
        for (size_t i = 0; i < lines.n; i++) {
            if (i) sb_putc(&out, '\n');
            sb_append(&out, lines.v[i]);
        }
    } else {
        int pending = 0;
        bool have_prev = false, prev_more = false;
        for (size_t i = 0; i < lines.n; i++) {
            const char *l = lines.v[i];
            if (!l[0]) { pending++; continue; }
            bool more = l[0] == ' ' || l[0] == '\t';
            if (have_prev) {
                if (!more && !prev_more && pending == 0) sb_putc(&out, ' ');
                else if (!more && !prev_more) for (int k = 0; k < pending; k++) sb_putc(&out, '\n');
                else for (int k = 0; k < pending + 1; k++) sb_putc(&out, '\n');
            } else {
                for (int k = 0; k < pending; k++) sb_putc(&out, '\n');
            }
            sb_append(&out, l);
            pending = 0;
            have_prev = true;
            prev_more = more;
        }
    }
    bool content = lines.n > 0;
    if (chomp == 0 && content) sb_putc(&out, '\n');
    if (chomp == 1) {
        if (content) sb_putc(&out, '\n');
        for (int i = 0; i < trailing_blank; i++) sb_putc(&out, '\n');
    }
    strvec_free(&lines);
    char *s = sb_detach(&out);
    y->force_str = false;
    return json_str_own(s);
}

/* ---------------- block collections ---------------- */
static bool insert_pair(yp_t *y, json_t *obj, char *key, bool merge, json_t *v, size_t at_pos) {
    if (merge) {
        json_t *src[64];
        size_t ns = 0;
        if (json_is_obj(v)) src[ns++] = v;
        else if (json_is_arr(v)) {
            for (size_t i = 0; i < v->u.arr.n && ns < 64; i++) {
                if (!json_is_obj(v->u.arr.items[i])) {
                    yerr_at(y, at_pos, "merge key value must be a mapping or a list of mappings");
                    free(key);
                    json_free(v);
                    return false;
                }
                src[ns++] = v->u.arr.items[i];
            }
        } else {
            yerr_at(y, at_pos, "merge key value must be a mapping or a list of mappings");
            free(key);
            json_free(v);
            return false;
        }
        for (size_t s = 0; s < ns; s++) {
            const char *k;
            json_t *val;
            json_obj_foreach(src[s], i, k, val) {
                if (!json_get(obj, k)) json_set(obj, k, json_clone(val));
            }
        }
        json_free(v);
        free(key);
        return true;
    }
    json_set(obj, key, v);
    free(key);
    return true;
}

/* Parse a mapping key at y->pos (column m). Returns owned key text; *merge set for plain '<<'. */
static char *parse_key(yp_t *y, bool *merge) {
    *merge = false;
    char c = cur(y);
    if (c == '"' || c == '\'') {
        char *k = parse_quoted(y);
        if (!k) return NULL;
        skip_space(y);
        if (cur(y) != ':') {
            yerr(y, "could not find expected ':'");
            free(k);
            return NULL;
        }
        return k;
    }
    if (c == '[' || c == '{') {
        yerr(y, "complex (flow collection) mapping keys are not supported");
        return NULL;
    }
    size_t st = y->pos;
    size_t e = scan_plain_line(y, st, false);
    if (e >= y->len || y->s[e] != ':') {
        y->pos = e;
        yerr_at(y, st, "could not find expected ':'");
        return NULL;
    }
    y->pos = e;
    char *k = xstrndup(y->s + st, e - st);
    if (strcmp(k, "<<") == 0) *merge = true;
    return k;
}

static json_t *parse_block_map(yp_t *y, int m, char *first_key, bool first_merge) {
    json_t *obj = json_obj();
    char *key = first_key;
    bool merge = first_merge;
    for (;;) {
        size_t kpos = y->pos;
        json_t *v = NULL;
        if (key == NULL) {
            /* explicit key '? ' */
            y->pos++;
            skip_space(y);
            json_t *kn = at_eol(y) ? parse_value_block(y, m, false) : parse_node(y, m);
            if (!kn) goto fail;
            if (json_is_str(kn)) key = xstrdup(kn->u.str.s);
            else if (json_is_null(kn)) key = xstrdup("null");
            else key = json_dump(kn, 0);
            json_free(kn);
            skip_to_content(y);
            if (y->err) { free(key); goto fail; }
            if (!eof(y) && (int)col(y) == m && cur(y) == ':' && is_blank_or_end(at(y, 1))) {
                /* fallthrough to value */
            } else {
                json_set(obj, key, json_null());
                free(key);
                key = NULL;
                goto next;
            }
        }
        y->pos++; /* ':' */
        skip_space(y);
        if (at_eol(y)) v = parse_value_block(y, m, true);
        else {
            y->inline_value = true;
            v = parse_node(y, m);
        }
        if (!v) { free(key); goto fail; }
        if (!insert_pair(y, obj, key, merge, v, kpos)) goto fail;
        key = NULL;
    next:
        skip_to_content(y);
        if (y->err) goto fail;
        if (eof(y) || is_doc_marker(y)) break;
        {
            int c = (int)col(y);
            if (c < m) break;
            if (c > m) {
                yerr(y, "bad indentation of a mapping entry");
                goto fail;
            }
        }
        if (cur(y) == '-' && is_blank_or_end(at(y, 1))) {
            yerr(y, "unexpected sequence entry inside a mapping");
            goto fail;
        }
        if (cur(y) == '?' && is_blank_or_end(at(y, 1))) {
            key = NULL;
            merge = false;
            continue;
        }
        key = parse_key(y, &merge);
        if (!key) goto fail;
    }
    return obj;
fail:
    json_free(obj);
    return NULL;
}

static json_t *parse_block_seq(yp_t *y, int m) {
    json_t *arr = json_arr();
    for (;;) {
        y->pos++; /* '-' */
        skip_space(y);
        json_t *v = at_eol(y) ? parse_value_block(y, m, false) : parse_node(y, m);
        if (!v) { json_free(arr); return NULL; }
        json_push(arr, v);
        skip_to_content(y);
        if (y->err) { json_free(arr); return NULL; }
        if (eof(y) || is_doc_marker(y)) break;
        int c = (int)col(y);
        if (c < m) break;
        if (c > m) {
            yerr(y, "bad indentation of a sequence entry");
            json_free(arr);
            return NULL;
        }
        if (!(cur(y) == '-' && is_blank_or_end(at(y, 1)))) break;
    }
    return arr;
}

static json_t *parse_value_block(yp_t *y, int n, bool seq_ok) {
    skip_to_content(y);
    if (y->err) return NULL;
    if (eof(y) || is_doc_marker(y)) { y->force_str = false; return json_null(); }
    int c = (int)col(y);
    if (c > n) return parse_node(y, n);
    if (c == n && seq_ok && cur(y) == '-' && is_blank_or_end(at(y, 1))) return parse_block_seq(y, c);
    y->force_str = false;
    return json_null();
}

/* Parse a node whose content starts at y->pos (same line); n = parent indent. */
static json_t *parse_node(yp_t *y, int n) {
    if (++y->depth > YAML_MAX_DEPTH) {
        yerr(y, "maximum nesting depth exceeded");
        y->depth--;
        return NULL;
    }
    json_t *v = NULL;
    char *anchor = NULL;
    bool inl = y->inline_value;
    y->inline_value = false;
    for (;;) {
        char c = cur(y);
        if (c == '&') {
            y->pos++;
            free(anchor);
            anchor = read_name(y);
            skip_space(y);
            continue;
        }
        if (c == '!') {
            char *tag = read_name(y);
            if (strcmp(tag, "!!str") == 0 || strcmp(tag, "!str") == 0 || strcmp(tag, "!<tag:yaml.org,2002:str>") == 0)
                y->force_str = true;
            free(tag);
            skip_space(y);
            continue;
        }
        break;
    }
    if (at_eol(y)) {
        v = parse_value_block(y, n, true);
        goto done;
    }
    int c0 = (int)col(y);
    char c = cur(y);
    if (c == '*') {
        v = parse_alias(y);
        if (v) {
            skip_space(y);
            if (cur(y) == ':' && is_blank_or_end(at(y, 1))) {
                yerr(y, "aliases as mapping keys are not supported");
                json_free(v);
                v = NULL;
            }
        }
    } else if (inl && (c == '-' || c == '?') && is_blank_or_end(at(y, 1))) {
        yerr(y, "block collections are not allowed as an inline mapping value");
    } else if (c == '-' && is_blank_or_end(at(y, 1))) {
        v = parse_block_seq(y, c0);
    } else if (c == '?' && is_blank_or_end(at(y, 1))) {
        v = parse_block_map(y, c0, NULL, false);
    } else if (c == '|' || c == '>') {
        v = parse_block_scalar(y, n);
    } else if (c == '[' || c == '{') {
        v = parse_flow(y);
        if (v) {
            skip_space(y);
            if (cur(y) == ':' && is_blank_or_end(at(y, 1))) {
                yerr(y, "complex (flow collection) mapping keys are not supported");
                json_free(v);
                v = NULL;
            } else if (!at_eol(y)) {
                yerr(y, "unexpected content after flow collection");
                json_free(v);
                v = NULL;
            }
        }
    } else if (c == '"' || c == '\'') {
        size_t st = y->pos;
        char *s = parse_quoted(y);
        if (s) {
            skip_space(y);
            if (cur(y) == ':' && is_blank_or_end(at(y, 1)) && inl) {
                yerr(y, "mapping values are not allowed here");
                free(s);
            } else if (cur(y) == ':' && is_blank_or_end(at(y, 1))) {
                y->pos = st;
                bool merge;
                char *k = parse_key(y, &merge);
                free(s);
                v = k ? parse_block_map(y, c0, k, false) : NULL;
            } else if (!at_eol(y)) {
                yerr(y, "unexpected content after quoted scalar");
                free(s);
            } else {
                v = json_str_own(s);
                y->force_str = false;
            }
        }
    } else if (c == '%' || c == '@' || c == '`') {
        yerr(y, "plain scalar cannot start with '%c'", c);
    } else {
        size_t st = y->pos;
        size_t e = scan_plain_line(y, st, false);
        if (e < y->len && y->s[e] == ':' && inl) {
            y->pos = e;
            yerr(y, "mapping values are not allowed here");
        } else if (e < y->len && y->s[e] == ':' && e >= st) {
            bool merge;
            char *k = parse_key(y, &merge);
            v = k ? parse_block_map(y, c0, k, merge) : NULL;
        } else {
            sb_t out = SB_INIT;
            sb_appendn(&out, y->s + st, e - st);
            y->pos = e;
            plain_continue(y, &out, n);
            bool fs = y->force_str;
            y->force_str = false;
            v = resolve_plain(sb_cstr(&out), fs);
            sb_free(&out);
            skip_space(y);
            if (!at_eol(y)) {
                yerr(y, "unexpected content after scalar");
                json_free(v);
                v = NULL;
            }
        }
    }
done:
    y->depth--;
    if (anchor) {
        if (v) set_anchor(y, anchor, v);
        else free(anchor);
    }
    return v;
}

/* ---------------- flow collections ---------------- */
static void skip_flow_ws(yp_t *y) {
    for (;;) {
        while (!eof(y) && (cur(y) == ' ' || cur(y) == '\t' || is_brk(cur(y)))) y->pos++;
        if (cur(y) == '#') {
            while (!eof(y) && !is_brk(cur(y))) y->pos++;
            continue;
        }
        break;
    }
}

/* Plain flow scalar (may span lines). Returns owned raw text. */
static char *flow_plain(yp_t *y) {
    sb_t out = SB_INIT;
    for (;;) {
        size_t st = y->pos;
        size_t e = scan_plain_line(y, st, true);
        sb_appendn(&out, y->s + st, e - st);
        y->pos = e;
        size_t save = y->pos;
        skip_space(y);
        if (is_brk(cur(y))) {
            skip_flow_ws(y);
            char c = cur(y);
            if (c && !is_flow_ind(c) && c != ':' && c != '#') {
                sb_putc(&out, ' ');
                continue;
            }
            y->pos = save;
        } else {
            y->pos = save;
        }
        break;
    }
    if (!out.s) return xstrdup("");
    return sb_detach(&out);
}

static json_t *parse_flow_item(yp_t *y, char **raw_key) {
    if (raw_key) *raw_key = NULL;
    char *anchor = NULL;
    bool fs = false;
    for (;;) {
        if (cur(y) == '&') {
            y->pos++;
            free(anchor);
            anchor = read_name(y);
            skip_flow_ws(y);
            continue;
        }
        if (cur(y) == '!') {
            char *tag = read_name(y);
            if (strcmp(tag, "!!str") == 0) fs = true;
            free(tag);
            skip_flow_ws(y);
            continue;
        }
        break;
    }
    json_t *v = NULL;
    char c = cur(y);
    if (c == '[' || c == '{') {
        v = parse_flow(y);
    } else if (c == '*') {
        v = parse_alias(y);
    } else if (c == '"' || c == '\'') {
        char *s = parse_quoted(y);
        if (s) {
            if (raw_key) *raw_key = xstrdup(s);
            v = json_str_own(s);
        }
    } else if (c == 0) {
        yerr(y, "unterminated flow collection");
    } else {
        char *raw = flow_plain(y);
        v = resolve_plain(raw, fs);
        if (raw_key) *raw_key = raw;
        else free(raw);
    }
    if (anchor) {
        if (v) set_anchor(y, anchor, v);
        else free(anchor);
    }
    return v;
}

static char *key_text(json_t *k, char *raw) {
    if (raw) return raw;
    if (json_is_str(k)) return xstrdup(k->u.str.s);
    return json_dump(k, 0);
}

static json_t *parse_flow(yp_t *y) {
    if (++y->depth > YAML_MAX_DEPTH) {
        yerr(y, "maximum nesting depth exceeded");
        y->depth--;
        return NULL;
    }
    char open = cur(y);
    size_t st = y->pos;
    y->pos++;
    json_t *res = open == '[' ? json_arr() : json_obj();
    char close = open == '[' ? ']' : '}';
    for (;;) {
        skip_flow_ws(y);
        if (eof(y)) {
            yerr_at(y, st, "unterminated flow collection");
            goto fail;
        }
        if (cur(y) == close) {
            y->pos++;
            break;
        }
        if (cur(y) == ',') {
            yerr(y, "unexpected ',' in flow collection");
            goto fail;
        }
        bool explicit_key = false;
        if (cur(y) == '?' && is_blank_or_end(at(y, 1))) {
            y->pos++;
            skip_flow_ws(y);
            explicit_key = true;
        }
        char *raw = NULL;
        json_t *item = NULL;
        if (cur(y) == ':' && open == '{') {
            item = json_null();
            raw = xstrdup("");
        } else {
            item = parse_flow_item(y, &raw);
            if (!item) { free(raw); goto fail; }
        }
        skip_flow_ws(y);
        bool has_colon = cur(y) == ':' && (is_blank_or_end(at(y, 1)) || is_flow_ind(at(y, 1)) || raw);
        if (open == '{' || has_colon || explicit_key) {
            json_t *val;
            if (has_colon) {
                y->pos++;
                skip_flow_ws(y);
                if (cur(y) == ',' || cur(y) == close) val = json_null();
                else {
                    val = parse_flow_item(y, NULL);
                    if (!val) { json_free(item); free(raw); goto fail; }
                }
            } else {
                val = json_null();
            }
            char *k = key_text(item, raw);
            json_free(item);
            if (open == '{') {
                json_set(res, k, val);
            } else {
                json_t *pair = json_obj();
                json_set(pair, k, val);
                json_push(res, pair);
            }
            free(k);
        } else {
            free(raw);
            json_push(res, item);
        }
        skip_flow_ws(y);
        if (cur(y) == ',') {
            y->pos++;
            continue;
        }
        if (cur(y) == close) continue;
        yerr(y, "expected ',' or '%c' in flow collection", close);
        goto fail;
    }
    y->depth--;
    return res;
fail:
    y->depth--;
    json_free(res);
    return NULL;
}

/* ---------------- documents ---------------- */
static void anchors_free(yp_t *y) { hmap_free(&y->anchors, (void (*)(void *))json_free); }

static json_t *parse_stream(const char *s, size_t len, char **err, bool all) {
    if (err) *err = NULL;
    yp_t y;
    memset(&y, 0, sizeof(y));
    y.s = s ? s : "";
    y.len = s ? len : 0;
    hmap_init(&y.anchors);
    if (y.len >= 3 && (unsigned char)y.s[0] == 0xEF && (unsigned char)y.s[1] == 0xBB && (unsigned char)y.s[2] == 0xBF) {
        y.s += 3;
        y.len -= 3;
    }
    json_t *docs = json_arr();
    for (;;) {
        skip_to_content(&y);
        if (y.err) break;
        while (!eof(&y) && cur(&y) == '%' && col(&y) == 0) {
            skip_line(&y);
            skip_to_content(&y);
        }
        if (y.err || eof(&y)) break;
        if (is_doc_marker(&y) && y.s[y.pos] == '.') {
            y.pos += 3;
            continue;
        }
        if (is_doc_marker(&y)) {
            y.pos += 3;
            skip_space(&y);
        }
        json_t *v;
        if (at_eol(&y)) v = parse_value_block(&y, -1, false);
        else v = parse_node(&y, -1);
        if (!v) break;
        json_push(docs, v);
        skip_to_content(&y);
        if (y.err) break;
        if (!eof(&y) && !is_doc_marker(&y)) {
            yerr(&y, "unexpected content (bad indentation?)");
            break;
        }
        if (!all) break;
    }
    anchors_free(&y);
    if (y.err) {
        json_free(docs);
        if (err) *err = y.err;
        else free(y.err);
        return NULL;
    }
    if (all) return docs;
    json_t *first = json_len(docs) ? json_arr_remove(docs, 0) : json_null();
    json_free(docs);
    return first;
}

json_t *yaml_parse(const char *s, size_t len, char **err) { return parse_stream(s, len, err, false); }
json_t *yaml_parse_all(const char *s, size_t len, char **err) { return parse_stream(s, len, err, true); }

json_t *yaml_frontmatter(const char *text, size_t len, const char **body_out, char **err) {
    if (err) *err = NULL;
    if (body_out) *body_out = text;
    if (!text) return NULL;
    size_t p = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) p = 3;
    if (len - p < 3 || memcmp(text + p, "---", 3) != 0) return NULL;
    size_t q = p + 3;
    while (q < len && (text[q] == ' ' || text[q] == '\t')) q++;
    if (q < len && text[q] == '\r') q++;
    if (q < len && text[q] != '\n') return NULL;
    size_t start = q < len ? q + 1 : len;
    size_t i = start;
    while (i <= len) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        size_t le = e;
        if (le > i && text[le - 1] == '\r') le--;
        while (le > i && (text[le - 1] == ' ' || text[le - 1] == '\t')) le--;
        if (le - i == 3 && (memcmp(text + i, "---", 3) == 0 || memcmp(text + i, "...", 3) == 0)) {
            json_t *v = yaml_parse(text + start, i - start, err);
            if (v && json_is_null(v)) {
                json_free(v);
                v = json_obj();
            }
            if (v && body_out) *body_out = text + (e < len ? e + 1 : len);
            return v;
        }
        if (e >= len) break;
        i = e + 1;
    }
    return NULL;
}

/* ---------------- emitter ---------------- */
static bool needs_quote(const char *s, size_t n) {
    if (n == 0) return true;
    json_t *r = resolve_plain(s, false);
    bool str = json_is_str(r);
    json_free(r);
    if (!str) return true;
    unsigned char c0 = (unsigned char)s[0];
    if (strchr("-?:,[]{}#&*!|>'\"%@`", c0)) return true;
    if (s[0] == ' ' || s[0] == '\t' || s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == ':') return true;
    if (n >= 3 && (strncmp(s, "---", 3) == 0 || strncmp(s, "...", 3) == 0)) return true;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return true;
        /* Bun's YAML.stringify quotes flow indicators and quote characters anywhere in a scalar */
        if (strchr(",[]{}'`\"", c)) return true;
        if (c == ':' && i + 1 < n && (s[i + 1] == ' ' || s[i + 1] == '\t')) return true;
        if (c == '#' && i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) return true;
        if (c == 0xC2 && i + 1 < n && ((unsigned char)s[i + 1] == 0x85 || (unsigned char)s[i + 1] == 0xA0)) return true;
        if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 &&
            ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9))
            return true;
    }
    if (!utf8_valid(s, n)) return true;
    return false;
}

static void emit_dq(sb_t *b, const char *s, size_t n) {
    sb_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_append(b, "\\\""); break;
        case '\\': sb_append(b, "\\\\"); break;
        case '\n': sb_append(b, "\\n"); break;
        case '\t': sb_append(b, "\\t"); break;
        case '\r': sb_append(b, "\\r"); break;
        case 0: sb_append(b, "\\0"); break;
        default:
            if (c < 0x20 || c == 0x7F) sb_appendf(b, "\\x%02X", c);
            else if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0x85) { sb_append(b, "\\N"); i++; }
            else if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0xA8) { sb_append(b, "\\L"); i += 2; }
            else if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0xA9) { sb_append(b, "\\P"); i += 2; }
            else sb_putc(b, (char)c);
        }
    }
    sb_putc(b, '"');
}

static void emit_scalar_str(sb_t *b, const char *s, size_t n) {
    if (needs_quote(s, n)) emit_dq(b, s, n);
    else sb_appendn(b, s, n);
}

/* Can s be written as a literal block scalar? */
static bool block_ok(const char *s, size_t n) {
    if (n == 0 || !memchr(s, '\n', n) || !utf8_valid(s, n)) return false;
    bool any = false;
    size_t i = 0;
    /* first non-empty line must not start with whitespace */
    while (i < n && s[i] == '\n') i++;
    if (i >= n) return false;
    if (s[i] == ' ' || s[i] == '\t') return false;
    for (size_t k = 0; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c == '\r' || (c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) return false;
        if (c != '\n') any = true;
        if (c == 0xC2 && k + 1 < n && (unsigned char)s[k + 1] == 0x85) return false;
        if (c == 0xE2 && k + 2 < n && (unsigned char)s[k + 1] == 0x80 &&
            ((unsigned char)s[k + 2] == 0xA8 || (unsigned char)s[k + 2] == 0xA9))
            return false;
    }
    return any;
}

static void indent_to(sb_t *b, int n) {
    for (int i = 0; i < n; i++) sb_putc(b, ' ');
}

static void emit_block_str(sb_t *b, const char *s, size_t n, int indent) {
    size_t trail = 0;
    while (trail < n && s[n - 1 - trail] == '\n') trail++;
    sb_putc(b, '|');
    if (trail == 0) sb_putc(b, '-');
    else if (trail > 1) sb_putc(b, '+');
    sb_putc(b, '\n');
    size_t body = n - trail;
    size_t i = 0;
    while (i <= body) {
        const char *e = memchr(s + i, '\n', body - i);
        size_t le = e ? (size_t)(e - s) : body;
        if (le > i) {
            indent_to(b, indent);
            sb_appendn(b, s + i, le - i);
        }
        sb_putc(b, '\n');
        if (!e) break;
        i = le + 1;
    }
    for (size_t k = 1; k < trail; k++) sb_putc(b, '\n');
}

static void emit_num(sb_t *b, const json_t *j) {
    if (j->is_int) {
        sb_appendf(b, "%lld", (long long)j->u.num.i);
        return;
    }
    double d = j->u.num.d;
    if (isnan(d)) { sb_append(b, ".nan"); return; }
    if (isinf(d)) { sb_append(b, d < 0 ? "-.inf" : ".inf"); return; }
    char buf[64];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    if (!strpbrk(buf, ".eEn")) strcat(buf, ".0");
    sb_append(b, buf);
}

static void emit(sb_t *b, const json_t *j, int indent, int depth);

/* Emit a value that follows "key:" or "- " (inline position). */
static void emit_inline(sb_t *b, const json_t *v, int indent, int depth, bool after_dash) {
    if (json_is_obj(v) && v->u.obj.n) {
        if (after_dash) {
            sb_t tmp = SB_INIT;
            emit(&tmp, v, indent, depth + 1);
            sb_append(b, sb_cstr(&tmp) + indent); /* first line continues after "- " */
            sb_free(&tmp);
        } else {
            sb_putc(b, '\n');
            emit(b, v, indent, depth + 1);
        }
    } else if (json_is_arr(v) && v->u.arr.n) {
        if (after_dash) {
            sb_t tmp = SB_INIT;
            emit(&tmp, v, indent, depth + 1);
            sb_append(b, sb_cstr(&tmp) + indent);
            sb_free(&tmp);
        } else {
            sb_putc(b, '\n');
            emit(b, v, indent, depth + 1);
        }
    } else if (json_is_str(v) && block_ok(v->u.str.s, v->u.str.len)) {
        if (!after_dash) sb_putc(b, ' ');
        emit_block_str(b, v->u.str.s, v->u.str.len, indent);
    } else {
        if (!after_dash) sb_putc(b, ' ');
        emit(b, v, indent, depth + 1);
        sb_putc(b, '\n');
    }
}

static void emit(sb_t *b, const json_t *j, int indent, int depth) {
    if (!j || depth > 1000) {
        sb_append(b, "null");
        return;
    }
    switch (j->type) {
    case JSON_NULL: sb_append(b, "null"); break;
    case JSON_BOOL: sb_append(b, j->u.b ? "true" : "false"); break;
    case JSON_NUMBER: emit_num(b, j); break;
    case JSON_STRING: emit_scalar_str(b, j->u.str.s, j->u.str.len); break;
    case JSON_ARRAY:
        if (!j->u.arr.n) { sb_append(b, "[]"); break; }
        for (size_t i = 0; i < j->u.arr.n; i++) {
            indent_to(b, indent);
            sb_append(b, "- ");
            emit_inline(b, j->u.arr.items[i], indent + 2, depth, true);
        }
        break;
    case JSON_OBJECT:
        if (!j->u.obj.n) { sb_append(b, "{}"); break; }
        for (size_t i = 0; i < j->u.obj.n; i++) {
            indent_to(b, indent);
            const char *k = j->u.obj.keys[i];
            emit_scalar_str(b, k, strlen(k));
            sb_putc(b, ':');
            const json_t *v = j->u.obj.vals[i];
            emit_inline(b, v, indent + 2, depth, false);
        }
        break;
    }
}

char *yaml_dump(const json_t *j) {
    sb_t b = SB_INIT;
    if (json_is_obj(j) && j->u.obj.n) emit(&b, j, 0, 0);
    else if (json_is_arr(j) && j->u.arr.n) emit(&b, j, 0, 0);
    else if (json_is_str(j) && block_ok(j->u.str.s, j->u.str.len)) emit_block_str(&b, j->u.str.s, j->u.str.len, 2);
    else {
        emit(&b, j, 0, 0);
        sb_putc(&b, '\n');
    }
    return sb_detach(&b);
}
