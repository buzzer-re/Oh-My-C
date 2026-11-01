#include "json.h"

#include <errno.h>
#include <math.h>

#define JSON_MAX_DEPTH 512

/* ---------- construction ---------- */

static json_t *jnew(json_type_t t) {
    json_t *j = xcalloc(1, sizeof(json_t));
    j->type = t;
    return j;
}

json_t *json_null(void) { return jnew(JSON_NULL); }

json_t *json_bool(bool b) {
    json_t *j = jnew(JSON_BOOL);
    j->u.b = b;
    return j;
}

json_t *json_num(double d) {
    json_t *j = jnew(JSON_NUMBER);
    j->u.num.d = d;
    if (isfinite(d) && d == floor(d) && fabs(d) < 9.007199254740992e15) {
        j->is_int = true;
        j->u.num.i = (int64_t)d;
    }
    return j;
}

json_t *json_int(int64_t i) {
    json_t *j = jnew(JSON_NUMBER);
    j->is_int = true;
    j->u.num.i = i;
    j->u.num.d = (double)i;
    return j;
}

json_t *json_strn(const char *s, size_t n) {
    json_t *j = jnew(JSON_STRING);
    j->u.str.s = xstrndup(s ? s : "", s ? n : 0);
    j->u.str.len = s ? n : 0;
    return j;
}

json_t *json_str(const char *s) { return json_strn(s ? s : "", s ? strlen(s) : 0); }

json_t *json_str_own(char *s) {
    json_t *j = jnew(JSON_STRING);
    if (!s) s = xstrdup("");
    j->u.str.s = s;
    j->u.str.len = strlen(s);
    return j;
}

json_t *json_arr(void) { return jnew(JSON_ARRAY); }
json_t *json_obj(void) { return jnew(JSON_OBJECT); }

void json_free(json_t *j) {
    if (!j) return;
    switch (j->type) {
    case JSON_STRING: free(j->u.str.s); break;
    case JSON_ARRAY:
        for (size_t i = 0; i < j->u.arr.n; i++) json_free(j->u.arr.items[i]);
        free(j->u.arr.items);
        break;
    case JSON_OBJECT:
        for (size_t i = 0; i < j->u.obj.n; i++) {
            free(j->u.obj.keys[i]);
            json_free(j->u.obj.vals[i]);
        }
        free(j->u.obj.keys);
        free(j->u.obj.vals);
        break;
    default: break;
    }
    free(j);
}

json_t *json_clone(const json_t *j) {
    if (!j) return NULL;
    json_t *c;
    switch (j->type) {
    case JSON_NULL: return json_null();
    case JSON_BOOL: return json_bool(j->u.b);
    case JSON_NUMBER:
        c = jnew(JSON_NUMBER);
        c->u.num = j->u.num;
        c->is_int = j->is_int;
        return c;
    case JSON_STRING: return json_strn(j->u.str.s, j->u.str.len);
    case JSON_ARRAY:
        c = json_arr();
        for (size_t i = 0; i < j->u.arr.n; i++) json_push(c, json_clone(j->u.arr.items[i]));
        return c;
    case JSON_OBJECT:
        c = json_obj();
        for (size_t i = 0; i < j->u.obj.n; i++) json_set(c, j->u.obj.keys[i], json_clone(j->u.obj.vals[i]));
        return c;
    }
    return NULL;
}

/* ---------- object ---------- */

static ssize_t obj_find(const json_t *o, const char *key) {
    for (size_t i = 0; i < o->u.obj.n; i++)
        if (strcmp(o->u.obj.keys[i], key) == 0) return (ssize_t)i;
    return -1;
}

json_t *json_get(const json_t *o, const char *key) {
    if (!json_is_obj(o) || !key) return NULL;
    ssize_t i = obj_find(o, key);
    return i < 0 ? NULL : o->u.obj.vals[i];
}

const char *json_get_str(const json_t *o, const char *key) { return json_sv(json_get(o, key)); }

double json_get_num(const json_t *o, const char *key, double def) {
    json_t *v = json_get(o, key);
    return json_is_num(v) ? v->u.num.d : def;
}

int64_t json_get_int(const json_t *o, const char *key, int64_t def) {
    json_t *v = json_get(o, key);
    if (!json_is_num(v)) return def;
    return v->is_int ? v->u.num.i : (int64_t)v->u.num.d;
}

bool json_get_bool(const json_t *o, const char *key, bool def) {
    json_t *v = json_get(o, key);
    return json_is_bool(v) ? v->u.b : def;
}

json_t *json_get_obj(const json_t *o, const char *key) {
    json_t *v = json_get(o, key);
    return json_is_obj(v) ? v : NULL;
}

json_t *json_get_arr(const json_t *o, const char *key) {
    json_t *v = json_get(o, key);
    return json_is_arr(v) ? v : NULL;
}

void json_set(json_t *o, const char *key, json_t *val) {
    if (!json_is_obj(o)) {
        json_free(val);
        return;
    }
    if (!val) val = json_null();
    ssize_t i = obj_find(o, key);
    if (i >= 0) {
        json_free(o->u.obj.vals[i]);
        o->u.obj.vals[i] = val;
        return;
    }
    if (o->u.obj.n == o->u.obj.cap) {
        o->u.obj.cap = o->u.obj.cap ? o->u.obj.cap * 2 : 8;
        o->u.obj.keys = xrealloc(o->u.obj.keys, o->u.obj.cap * sizeof(char *));
        o->u.obj.vals = xrealloc(o->u.obj.vals, o->u.obj.cap * sizeof(json_t *));
    }
    o->u.obj.keys[o->u.obj.n] = xstrdup(key);
    o->u.obj.vals[o->u.obj.n] = val;
    o->u.obj.n++;
}

/* append without duplicate check (parser fast path); last duplicate wins on lookup via set semantics */
static void obj_append(json_t *o, char *key, json_t *val) {
    ssize_t i = o->u.obj.n > 64 ? -1 : obj_find(o, key);
    if (i >= 0) {
        free(key);
        json_free(o->u.obj.vals[i]);
        o->u.obj.vals[i] = val;
        return;
    }
    if (o->u.obj.n == o->u.obj.cap) {
        o->u.obj.cap = o->u.obj.cap ? o->u.obj.cap * 2 : 8;
        o->u.obj.keys = xrealloc(o->u.obj.keys, o->u.obj.cap * sizeof(char *));
        o->u.obj.vals = xrealloc(o->u.obj.vals, o->u.obj.cap * sizeof(json_t *));
    }
    o->u.obj.keys[o->u.obj.n] = key;
    o->u.obj.vals[o->u.obj.n] = val;
    o->u.obj.n++;
}

void json_set_str(json_t *o, const char *k, const char *s) { json_set(o, k, s ? json_str(s) : json_null()); }
void json_set_int(json_t *o, const char *k, int64_t i) { json_set(o, k, json_int(i)); }
void json_set_num(json_t *o, const char *k, double d) { json_set(o, k, json_num(d)); }
void json_set_bool(json_t *o, const char *k, bool b) { json_set(o, k, json_bool(b)); }

json_t *json_take(json_t *o, const char *key) {
    if (!json_is_obj(o)) return NULL;
    ssize_t i = obj_find(o, key);
    if (i < 0) return NULL;
    json_t *v = o->u.obj.vals[i];
    free(o->u.obj.keys[i]);
    memmove(o->u.obj.keys + i, o->u.obj.keys + i + 1, (o->u.obj.n - (size_t)i - 1) * sizeof(char *));
    memmove(o->u.obj.vals + i, o->u.obj.vals + i + 1, (o->u.obj.n - (size_t)i - 1) * sizeof(json_t *));
    o->u.obj.n--;
    return v;
}

void json_del(json_t *o, const char *key) { json_free(json_take(o, key)); }

/* ---------- array ---------- */

size_t json_len(const json_t *j) {
    if (!j) return 0;
    if (j->type == JSON_ARRAY) return j->u.arr.n;
    if (j->type == JSON_OBJECT) return j->u.obj.n;
    if (j->type == JSON_STRING) return j->u.str.len;
    return 0;
}

json_t *json_at(const json_t *a, size_t i) {
    if (!json_is_arr(a) || i >= a->u.arr.n) return NULL;
    return a->u.arr.items[i];
}

void json_push(json_t *a, json_t *v) {
    if (!json_is_arr(a)) {
        json_free(v);
        return;
    }
    if (!v) v = json_null();
    if (a->u.arr.n == a->u.arr.cap) {
        a->u.arr.cap = a->u.arr.cap ? a->u.arr.cap * 2 : 8;
        a->u.arr.items = xrealloc(a->u.arr.items, a->u.arr.cap * sizeof(json_t *));
    }
    a->u.arr.items[a->u.arr.n++] = v;
}

void json_push_str(json_t *a, const char *s) { json_push(a, json_str(s)); }

json_t *json_arr_remove(json_t *a, size_t i) {
    if (!json_is_arr(a) || i >= a->u.arr.n) return NULL;
    json_t *v = a->u.arr.items[i];
    memmove(a->u.arr.items + i, a->u.arr.items + i + 1, (a->u.arr.n - i - 1) * sizeof(json_t *));
    a->u.arr.n--;
    return v;
}

void json_arr_insert(json_t *a, size_t i, json_t *v) {
    if (!json_is_arr(a)) {
        json_free(v);
        return;
    }
    if (i >= a->u.arr.n) {
        json_push(a, v);
        return;
    }
    json_push(a, v); /* grow */
    memmove(a->u.arr.items + i + 1, a->u.arr.items + i, (a->u.arr.n - 1 - i) * sizeof(json_t *));
    a->u.arr.items[i] = v;
}

bool json_truthy(const json_t *j) {
    if (!j) return false;
    switch (j->type) {
    case JSON_NULL: return false;
    case JSON_BOOL: return j->u.b;
    case JSON_NUMBER: return j->u.num.d != 0;
    case JSON_STRING: return j->u.str.len > 0;
    default: return true;
    }
}

bool json_equal(const json_t *a, const json_t *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return false;
    switch (a->type) {
    case JSON_NULL: return true;
    case JSON_BOOL: return a->u.b == b->u.b;
    case JSON_NUMBER: return a->u.num.d == b->u.num.d;
    case JSON_STRING: return a->u.str.len == b->u.str.len && memcmp(a->u.str.s, b->u.str.s, a->u.str.len) == 0;
    case JSON_ARRAY:
        if (a->u.arr.n != b->u.arr.n) return false;
        for (size_t i = 0; i < a->u.arr.n; i++)
            if (!json_equal(a->u.arr.items[i], b->u.arr.items[i])) return false;
        return true;
    case JSON_OBJECT:
        if (a->u.obj.n != b->u.obj.n) return false;
        for (size_t i = 0; i < a->u.obj.n; i++)
            if (!json_equal(a->u.obj.vals[i], json_get(b, a->u.obj.keys[i]))) return false;
        return true;
    }
    return false;
}

json_t *json_path(const json_t *j, const char *path) {
    const json_t *cur = j;
    const char *p = path;
    char seg[256];
    while (cur && *p) {
        size_t n = strcspn(p, ".");
        if (n >= sizeof seg) return NULL;
        memcpy(seg, p, n);
        seg[n] = 0;
        p += n;
        if (*p == '.') p++;
        if (cur->type == JSON_ARRAY) {
            char *end;
            long idx = strtol(seg, &end, 10);
            if (*end || idx < 0) return NULL;
            cur = json_at(cur, (size_t)idx);
        } else if (cur->type == JSON_OBJECT) {
            cur = json_get(cur, seg);
        } else {
            return NULL;
        }
    }
    return (json_t *)cur;
}

void json_merge(json_t *dst, const json_t *src) {
    if (!json_is_obj(dst) || !json_is_obj(src)) return;
    for (size_t i = 0; i < src->u.obj.n; i++) {
        const char *k = src->u.obj.keys[i];
        json_t *sv = src->u.obj.vals[i];
        json_t *dv = json_get(dst, k);
        if (json_is_obj(dv) && json_is_obj(sv)) json_merge(dv, sv);
        else json_set(dst, k, json_clone(sv));
    }
}

/* ---------- parser ---------- */

typedef struct {
    const char *s;
    size_t len, pos;
    int depth;
    bool partial;
    bool eof_hit;
    char err[160];
} jp_t;

static void jp_err(jp_t *p, const char *msg) {
    if (!p->err[0]) snprintf(p->err, sizeof p->err, "%s at offset %zu", msg, p->pos);
}

static void skip_ws(jp_t *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(jp_t *p, uint32_t *out) {
    if (p->pos + 4 > p->len) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexv(p->s[p->pos + (size_t)i]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    p->pos += 4;
    *out = v;
    return true;
}

/* parses a string starting at the opening quote; returns owned string */
static char *parse_string(jp_t *p, size_t *out_len) {
    p->pos++; /* opening quote */
    sb_t b = SB_INIT;
    for (;;) {
        if (p->pos >= p->len) {
            if (p->partial) {
                p->eof_hit = true;
                if (out_len) *out_len = b.len;
                return sb_detach(&b);
            }
            jp_err(p, "unterminated string");
            sb_free(&b);
            return NULL;
        }
        /* fast scan for plain bytes */
        size_t start = p->pos;
        while (p->pos < p->len) {
            unsigned char c = (unsigned char)p->s[p->pos];
            if (c == '"' || c == '\\' || c < 0x20) break;
            p->pos++;
        }
        if (p->pos > start) sb_appendn(&b, p->s + start, p->pos - start);
        if (p->pos >= p->len) continue;
        char c = p->s[p->pos];
        if (c == '"') {
            p->pos++;
            break;
        }
        if ((unsigned char)c < 0x20) {
            /* be lenient: accept raw control chars (some providers emit raw newlines) */
            sb_putc(&b, c);
            p->pos++;
            continue;
        }
        /* backslash */
        p->pos++;
        if (p->pos >= p->len) {
            if (p->partial) { p->eof_hit = true; continue; }
            jp_err(p, "bad escape");
            sb_free(&b);
            return NULL;
        }
        char e = p->s[p->pos++];
        switch (e) {
        case '"': sb_putc(&b, '"'); break;
        case '\\': sb_putc(&b, '\\'); break;
        case '/': sb_putc(&b, '/'); break;
        case 'b': sb_putc(&b, '\b'); break;
        case 'f': sb_putc(&b, '\f'); break;
        case 'n': sb_putc(&b, '\n'); break;
        case 'r': sb_putc(&b, '\r'); break;
        case 't': sb_putc(&b, '\t'); break;
        case 'u': {
            uint32_t cp;
            if (!read_hex4(p, &cp)) {
                if (p->partial && p->pos + 4 > p->len) { p->pos = p->len; p->eof_hit = true; continue; }
                jp_err(p, "bad \\u escape");
                sb_free(&b);
                return NULL;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo;
                size_t save = p->pos;
                if (p->pos + 1 < p->len && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                    p->pos += 2;
                    if (read_hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p->pos = save;
                        cp = 0xFFFD;
                    }
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            sb_append_utf8(&b, cp);
            break;
        }
        default:
            if (p->partial) { sb_putc(&b, e); break; }
            jp_err(p, "bad escape");
            sb_free(&b);
            return NULL;
        }
    }
    if (out_len) *out_len = b.len;
    if (!b.s) return xstrdup("");
    return sb_detach(&b);
}

static json_t *parse_value(jp_t *p);

static json_t *parse_number(jp_t *p) {
    size_t start = p->pos;
    if (p->pos < p->len && p->s[p->pos] == '-') p->pos++;
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') p->pos++;
        else break;
    }
    size_t n = p->pos - start;
    if (n == 0 || n > 64) {
        jp_err(p, "bad number");
        return NULL;
    }
    char buf[72];
    memcpy(buf, p->s + start, n);
    buf[n] = 0;
    if (p->partial && p->pos >= p->len) {
        /* trim incomplete exponent / fraction */
        while (n && (buf[n - 1] == 'e' || buf[n - 1] == 'E' || buf[n - 1] == '+' || buf[n - 1] == '-' ||
                     buf[n - 1] == '.'))
            buf[--n] = 0;
        if (!n) return json_int(0);
    }
    bool integral = strpbrk(buf, ".eE") == NULL;
    char *end;
    if (integral) {
        errno = 0;
        long long v = strtoll(buf, &end, 10);
        if (*end == 0 && errno == 0) return json_int((int64_t)v);
    }
    double d = strtod(buf, &end);
    if (*end) {
        jp_err(p, "bad number");
        return NULL;
    }
    json_t *j = jnew(JSON_NUMBER);
    j->u.num.d = d;
    return j;
}

static bool match_lit(jp_t *p, const char *lit) {
    size_t n = strlen(lit);
    size_t avail = p->len - p->pos;
    if (avail >= n && memcmp(p->s + p->pos, lit, n) == 0) {
        p->pos += n;
        return true;
    }
    if (p->partial && avail < n && memcmp(p->s + p->pos, lit, avail) == 0) {
        p->pos = p->len;
        p->eof_hit = true;
        return true;
    }
    return false;
}

static json_t *parse_array(jp_t *p) {
    p->pos++;
    json_t *a = json_arr();
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        return a;
    }
    for (;;) {
        skip_ws(p);
        if (p->pos >= p->len) {
            if (p->partial) { p->eof_hit = true; return a; }
            jp_err(p, "unterminated array");
            json_free(a);
            return NULL;
        }
        json_t *v = parse_value(p);
        if (!v) {
            if (p->partial && p->eof_hit) return a;
            json_free(a);
            return NULL;
        }
        json_push(a, v);
        skip_ws(p);
        if (p->pos >= p->len) {
            if (p->partial) { p->eof_hit = true; return a; }
            jp_err(p, "unterminated array");
            json_free(a);
            return NULL;
        }
        char c = p->s[p->pos++];
        if (c == ']') return a;
        if (c != ',') {
            jp_err(p, "expected , or ]");
            json_free(a);
            return NULL;
        }
    }
}

static json_t *parse_object(jp_t *p) {
    p->pos++;
    json_t *o = json_obj();
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        return o;
    }
    for (;;) {
        skip_ws(p);
        if (p->pos >= p->len) {
            if (p->partial) { p->eof_hit = true; return o; }
            jp_err(p, "unterminated object");
            json_free(o);
            return NULL;
        }
        if (p->s[p->pos] != '"') {
            jp_err(p, "expected string key");
            json_free(o);
            return NULL;
        }
        char *key = parse_string(p, NULL);
        if (!key) {
            json_free(o);
            return NULL;
        }
        if (p->eof_hit) {
            free(key);
            return o;
        }
        skip_ws(p);
        if (p->pos >= p->len) {
            free(key);
            if (p->partial) { p->eof_hit = true; return o; }
            jp_err(p, "unterminated object");
            json_free(o);
            return NULL;
        }
        if (p->s[p->pos] != ':') {
            free(key);
            jp_err(p, "expected :");
            json_free(o);
            return NULL;
        }
        p->pos++;
        skip_ws(p);
        if (p->pos >= p->len && p->partial) {
            free(key);
            p->eof_hit = true;
            return o;
        }
        json_t *v = parse_value(p);
        if (!v) {
            free(key);
            if (p->partial && p->eof_hit) return o;
            json_free(o);
            return NULL;
        }
        obj_append(o, key, v);
        skip_ws(p);
        if (p->pos >= p->len) {
            if (p->partial) { p->eof_hit = true; return o; }
            jp_err(p, "unterminated object");
            json_free(o);
            return NULL;
        }
        char c = p->s[p->pos++];
        if (c == '}') return o;
        if (c != ',') {
            jp_err(p, "expected , or }");
            json_free(o);
            return NULL;
        }
    }
}

static json_t *parse_value(jp_t *p) {
    skip_ws(p);
    if (p->pos >= p->len) {
        if (p->partial) p->eof_hit = true;
        jp_err(p, "unexpected end of input");
        return NULL;
    }
    if (++p->depth > JSON_MAX_DEPTH) {
        jp_err(p, "nesting too deep");
        return NULL;
    }
    json_t *r = NULL;
    char c = p->s[p->pos];
    switch (c) {
    case '{': r = parse_object(p); break;
    case '[': r = parse_array(p); break;
    case '"': {
        size_t n;
        char *s = parse_string(p, &n);
        if (s) {
            r = jnew(JSON_STRING);
            r->u.str.s = s;
            r->u.str.len = n;
        }
        break;
    }
    case 't':
        if (match_lit(p, "true")) r = json_bool(true);
        else jp_err(p, "bad literal");
        break;
    case 'f':
        if (match_lit(p, "false")) r = json_bool(false);
        else jp_err(p, "bad literal");
        break;
    case 'n':
        if (match_lit(p, "null")) r = json_null();
        else jp_err(p, "bad literal");
        break;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) r = parse_number(p);
        else jp_err(p, "unexpected character");
    }
    p->depth--;
    return r;
}

json_t *json_parse(const char *s, size_t len, char **err) {
    jp_t p = {.s = s, .len = len};
    /* skip UTF-8 BOM */
    if (len >= 3 && memcmp(s, "\xEF\xBB\xBF", 3) == 0) p.pos = 3;
    json_t *j = parse_value(&p);
    if (j) {
        skip_ws(&p);
        if (p.pos != p.len) {
            jp_err(&p, "trailing data");
            json_free(j);
            j = NULL;
        }
    }
    if (!j && err) *err = xstrdup(p.err[0] ? p.err : "parse error");
    return j;
}

json_t *json_parse_str(const char *s) { return s ? json_parse(s, strlen(s), NULL) : NULL; }

json_t *json_parse_partial(const char *s, size_t len) {
    jp_t p = {.s = s, .len = len, .partial = true};
    return parse_value(&p);
}

/* ---------- serialization ---------- */

void json_escape_sb(sb_t *b, const char *s, size_t len) {
    static const char hx[] = "0123456789abcdef";
    sb_reserve(b, len + 2);
    sb_putc(b, '"');
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x20 || c == '"' || c == '\\' || c >= 0x80) break;
            i++;
        }
        if (i > start) sb_appendn(b, s + start, i - start);
        if (i >= len) break;
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            uint32_t cp;
            int n = utf8_decode(s + i, len - i, &cp);
            if (n == 1) {
                sb_append(b, "\\ufffd");
            } else if (cp == 0x2028 || cp == 0x2029) {
                sb_appendf(b, "\\u%04x", cp);
            } else {
                sb_appendn(b, s + i, (size_t)n);
            }
            i += (size_t)n;
            continue;
        }
        i++;
        switch (c) {
        case '"': sb_append(b, "\\\""); break;
        case '\\': sb_append(b, "\\\\"); break;
        case '\n': sb_append(b, "\\n"); break;
        case '\r': sb_append(b, "\\r"); break;
        case '\t': sb_append(b, "\\t"); break;
        case '\b': sb_append(b, "\\b"); break;
        case '\f': sb_append(b, "\\f"); break;
        default: {
            char u[7] = {'\\', 'u', '0', '0', hx[c >> 4], hx[c & 15], 0};
            sb_append(b, u);
        }
        }
    }
    sb_putc(b, '"');
}

char *json_quote(const char *s) {
    sb_t b = SB_INIT;
    json_escape_sb(&b, s, strlen(s));
    return sb_detach(&b);
}

static void indent_nl(sb_t *b, int indent, int level) {
    sb_putc(b, '\n');
    for (int i = 0; i < indent * level; i++) sb_putc(b, ' ');
}

static void dump(sb_t *b, const json_t *j, int indent, int level) {
    if (!j) {
        sb_append(b, "null");
        return;
    }
    switch (j->type) {
    case JSON_NULL: sb_append(b, "null"); break;
    case JSON_BOOL: sb_append(b, j->u.b ? "true" : "false"); break;
    case JSON_NUMBER:
        if (j->is_int) sb_appendf(b, "%lld", (long long)j->u.num.i);
        else if (!isfinite(j->u.num.d)) sb_append(b, "null");
        else {
            char buf[40];
            snprintf(buf, sizeof buf, "%.17g", j->u.num.d);
            /* prefer shortest round-trip representation */
            for (int prec = 1; prec <= 17; prec++) {
                char t[40];
                snprintf(t, sizeof t, "%.*g", prec, j->u.num.d);
                if (strtod(t, NULL) == j->u.num.d) {
                    memcpy(buf, t, sizeof t);
                    break;
                }
            }
            sb_append(b, buf);
        }
        break;
    case JSON_STRING: json_escape_sb(b, j->u.str.s, j->u.str.len); break;
    case JSON_ARRAY:
        sb_putc(b, '[');
        for (size_t i = 0; i < j->u.arr.n; i++) {
            if (i) sb_putc(b, ',');
            if (indent) indent_nl(b, indent, level + 1);
            dump(b, j->u.arr.items[i], indent, level + 1);
        }
        if (indent && j->u.arr.n) indent_nl(b, indent, level);
        sb_putc(b, ']');
        break;
    case JSON_OBJECT:
        sb_putc(b, '{');
        for (size_t i = 0; i < j->u.obj.n; i++) {
            if (i) sb_putc(b, ',');
            if (indent) indent_nl(b, indent, level + 1);
            json_escape_sb(b, j->u.obj.keys[i], strlen(j->u.obj.keys[i]));
            sb_putc(b, ':');
            if (indent) sb_putc(b, ' ');
            dump(b, j->u.obj.vals[i], indent, level + 1);
        }
        if (indent && j->u.obj.n) indent_nl(b, indent, level);
        sb_putc(b, '}');
        break;
    }
}

void json_dump_sb(sb_t *b, const json_t *j, int indent) { dump(b, j, indent, 0); }

char *json_dump(const json_t *j, int indent) {
    sb_t b = SB_INIT;
    dump(&b, j, indent, 0);
    return sb_detach(&b);
}
