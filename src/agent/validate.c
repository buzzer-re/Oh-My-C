/*
 * validate.c - tool-call argument validation, a port of omp's validateToolArguments (packages/ai
 * utils/validation.ts) over the tool's JSON schema with omptype (ArkType-compatible) error messages
 * (packages/omptype errors.ts / compile.ts):
 *   - LLM-quirk normalizations: double-encoded keys, optional null / "null" / rejected "" stripped,
 *     required null -> schema default, numeric strings for number nodes, enum whitespace, trailing
 *     newlines on identifier keys (path, url, ...), JSON-encoded arrays for string|array unions,
 *     a lone string field adopted by a single-required-string schema;
 *   - up to 5 schema-directed coercion passes driven by the JSON-schema type issues (JSON strings,
 *     boolean spellings, scalar / container stringification, 0/1 <-> booleans, singleton arrays,
 *     unrecognized nested keys dropped);
 *   - omptype messages: `pattern must be a string (was missing)`, `op must be "a", "b" or "c" (was "x")`,
 *     `list[0].items must be at least length 1 (was 0)`, a property description as the expectation.
 */
#include "agent/agent.h"

#include <ctype.h>
#include <math.h>

#define MAX_DEPTH 48

/* ---------- small helpers ---------- */

static bool is_int_val(const json_t *v) { return json_is_num(v) && isfinite(v->u.num.d) && v->u.num.d == floor(v->u.num.d); }

static bool type_ok(const char *t, const json_t *v) {
    if (!t) return true;
    if (!strcmp(t, "string")) return json_is_str(v);
    if (!strcmp(t, "number")) return json_is_num(v) && isfinite(v->u.num.d);
    if (!strcmp(t, "integer")) return is_int_val(v);
    if (!strcmp(t, "boolean")) return json_is_bool(v);
    if (!strcmp(t, "object")) return json_is_obj(v);
    if (!strcmp(t, "array")) return json_is_arr(v);
    if (!strcmp(t, "null")) return v && v->type == JSON_NULL;
    return true;
}

/* schema "type" as a list (0 entries = untyped) */
static size_t schema_types(const json_t *s, const char **out, size_t cap) {
    const json_t *t = json_get(s, "type");
    size_t n = 0;
    if (json_is_str(t)) out[n++] = json_sv(t);
    else if (json_is_arr(t)) {
        json_t *x;
        json_arr_foreach(t, i, x) if (json_is_str(x) && n < cap) out[n++] = json_sv(x);
    }
    return n;
}

static bool has_type(const char *const *ts, size_t n, const char *t) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(ts[i], t)) return true;
    return false;
}

static bool is_required(const json_t *schema, const char *key) {
    json_t *r;
    json_arr_foreach(json_get_arr(schema, "required"), i, r) if (str_eq(json_sv(r), key)) return true;
    return false;
}

/* JS String.length of UTF-8 text */
static size_t utf16_len(const char *s, size_t n) {
    size_t u = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        size_t k = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        u += k == 4 ? 2 : 1;
        i += k;
    }
    return u;
}

/* JS String(number) (shortest round trip) */
static char *num_str(double d) {
    json_t *j = json_num(d);
    char *s = json_dump(j, 0);
    json_free(j);
    return s;
}

/* ---------- omptype issues ---------- */

typedef struct {
    json_t *path;      /* array of keys (strings) / indices (numbers) */
    char *expected;    /* display expectation (already configured) */
    bool missing;
    json_t *data;      /* clone of the failing value (NULL when missing) */
    char code;         /* d(omain) u(nit) m(in/max) v(divisor) p(redicate) */
} issue_t;
typedef VEC(issue_t) issues_t;

static void issues_free(issues_t *is) {
    for (size_t i = 0; i < is->n; i++) {
        json_free(is->v[i].path);
        free(is->v[i].expected);
        json_free(is->v[i].data);
    }
    vec_free(is);
}

static bool unit_part(const char *p, size_t n) {
    if (n >= 2 && p[0] == '"' && p[n - 1] == '"') return true;
    char buf[32];
    if (n < sizeof buf) {
        memcpy(buf, p, n);
        buf[n] = 0;
        if (!strcmp(buf, "true") || !strcmp(buf, "false") || !strcmp(buf, "null") || !strcmp(buf, "undefined")) return true;
        char *end;
        if (n && (strtod(buf, &end), *end == 0) && (isdigit((unsigned char)buf[0]) || buf[0] == '-')) return true;
    }
    return false;
}

/* omptype errorCode for a raw expectation */
static char error_code(const char *exp) {
    if (strstr(exp, "divisible by") || strstr(exp, "integer")) return 'v';
    bool all_units = true;
    for (const char *p = exp;;) {
        const char *q = strstr(p, " or ");
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (!unit_part(p, n)) all_units = false;
        if (!q) break;
        p = q + 4;
    }
    if (all_units) return 'u';
    if (strstr(exp, "at least") || strstr(exp, "more than") || !strcmp(exp, "non-negative") || !strcmp(exp, "positive"))
        return 'm';
    if (strstr(exp, "at most") || strstr(exp, "less than") || !strcmp(exp, "non-positive") || !strcmp(exp, "negative"))
        return 'm';
    if (exp[0] == '"') return 'u';
    if (strstr(exp, " or ")) return 'd';
    static const char *const dom[] = {"a string", "a number", "a bigint", "a symbol", "boolean", "an object", "an array",
                                      "a tuple", "undefined", "null", "unknown", "never"};
    for (size_t i = 0; i < sizeof dom / sizeof *dom; i++)
        if (!strcmp(exp, dom[i])) return 'd';
    return 'p';
}

static char *describe_kind(const json_t *v) {
    if (!v || v->type == JSON_NULL) return xstrdup("null");
    switch (v->type) {
    case JSON_STRING: return xstrdup("a string");
    case JSON_NUMBER: return xstrdup(isnan(v->u.num.d) ? "NaN" : "a number");
    case JSON_BOOL: return xstrdup("boolean");
    default: return xstrdup("an object"); /* arrays included (typeof) */
    }
}

static char *describe_value(const json_t *v) {
    if (!v || v->type == JSON_NULL) return xstrdup("null");
    switch (v->type) {
    case JSON_ARRAY: return xstrdup("an array");
    case JSON_STRING:
        return utf16_len(v->u.str.s, v->u.str.len) <= 40 ? json_dump(v, 0)
                                                          : xasprintf("a string (length %zu)", utf16_len(v->u.str.s, v->u.str.len));
    case JSON_NUMBER: return num_str(v->u.num.d);
    case JSON_BOOL: return xstrdup(v->u.b ? "true" : "false");
    default: return json_dump(v, 0);
    }
}

/* `a or b or c` -> `a, b or c` */
static char *expected_display(const char *raw) {
    strvec_t parts = {0};
    for (const char *p = raw;;) {
        const char *q = strstr(p, " or ");
        strvec_push_own(&parts, xstrndup(p, q ? (size_t)(q - p) : strlen(p)));
        if (!q) break;
        p = q + 4;
    }
    char *r;
    if (parts.n < 3) r = xstrdup(raw);
    else {
        sb_t b = SB_INIT;
        for (size_t i = 0; i + 1 < parts.n; i++) sb_appendf(&b, "%s%s", i ? ", " : "", parts.v[i]);
        sb_appendf(&b, " or %s", parts.v[parts.n - 1]);
        r = sb_detach(&b);
    }
    strvec_free(&parts);
    return r;
}

static char *format_path(const json_t *path) {
    sb_t b = SB_INIT;
    json_t *k;
    json_arr_foreach(path, i, k) {
        if (json_is_num(k)) sb_appendf(&b, "[%lld]", (long long)k->u.num.d);
        else sb_appendf(&b, "%s%s", b.len ? "." : "", json_sv(k));
    }
    return sb_detach(&b);
}

static char *issue_actual(const issue_t *is) {
    if (is->missing) return xstrdup("missing");
    return is->code == 'd' ? describe_kind(is->data) : describe_value(is->data);
}

static char *issue_problem(const issue_t *is) {
    char *act = issue_actual(is);
    char *r = is->missing ? xasprintf("must be %s (was missing)", is->expected) : xasprintf("must be %s (was %s)", is->expected, act);
    free(act);
    return r;
}

static char *issue_message(const issue_t *is) {
    char *at = format_path(is->path), *pr = issue_problem(is);
    char *r = *at ? xasprintf("%s %s", at, pr) : xstrdup(pr);
    free(at);
    free(pr);
    return r;
}

/* raw: the schema-derived expectation (display-split); desc: a description configured on the node (verbatim) */
static void add_issue(issues_t *is, const json_t *path, const char *raw, const char *desc, const json_t *data, bool missing) {
    issue_t x = {.path = json_clone(path), .missing = missing, .data = missing ? NULL : json_clone(data ? data : NULL)};
    if (!missing && !data) x.data = json_null();
    x.code = error_code(raw);
    x.expected = desc ? xstrdup(desc) : expected_display(raw);
    vec_push(is, x);
}

static json_t *path_with(const json_t *path, const char *key, long idx) {
    json_t *p = json_clone(path);
    if (key) json_push_str(p, key);
    else json_push(p, json_int(idx));
    return p;
}

static char *literal_expected(const json_t *en) {
    strvec_t parts = {0};
    json_t *e;
    json_arr_foreach(en, i, e) {
        char *s = json_dump(e, 0);
        bool dup = false;
        for (size_t k = 0; k < parts.n; k++)
            if (!strcmp(parts.v[k], s)) dup = true;
        if (dup) free(s);
        else strvec_push_own(&parts, s);
    }
    char *r = strvec_join(&parts, " or ");
    strvec_free(&parts);
    return r;
}

static const char *type_phrase(const char *t) {
    if (!strcmp(t, "string")) return "a string";
    if (!strcmp(t, "number")) return "a number";
    if (!strcmp(t, "integer")) return "an integer";
    if (!strcmp(t, "boolean")) return "boolean";
    if (!strcmp(t, "object")) return "an object";
    if (!strcmp(t, "array")) return "an array";
    if (!strcmp(t, "null")) return "null";
    return "unknown";
}

static char *union_expected(char **members, size_t n) {
    strvec_t ex = {0};
    for (size_t i = 0; i < n; i++) {
        bool dup = false;
        for (size_t k = 0; k < ex.n; k++)
            if (!strcmp(ex.v[k], members[i])) dup = true;
        if (!dup) strvec_push(&ex, members[i]);
    }
    char *r = strvec_join(&ex, " or ");
    strvec_free(&ex);
    return r;
}

/* omptype expectedOf for a schema node (description wins) */
static char *expected_of(const json_t *s, int depth) {
    const char *desc = json_get_str(s, "description");
    if (desc) return xstrdup(desc);
    json_t *en = json_get_arr(s, "enum");
    if (en) return literal_expected(en);
    if (json_get(s, "const")) {
        json_t *one = json_arr();
        json_push(one, json_clone(json_get(s, "const")));
        char *r = literal_expected(one);
        json_free(one);
        return r;
    }
    json_t *any = json_get_arr(s, "anyOf");
    if (!any) any = json_get_arr(s, "oneOf");
    if (any && depth < MAX_DEPTH) {
        char *m[64];
        size_t n = 0;
        json_t *b;
        json_arr_foreach(any, i, b) if (n < 64) m[n++] = expected_of(b, depth + 1);
        char *r = union_expected(m, n);
        for (size_t i = 0; i < n; i++) free(m[i]);
        return r;
    }
    const char *ts[8];
    size_t nt = schema_types(s, ts, 8);
    if (nt == 0) return xstrdup("unknown");
    if (nt > 1) {
        char *m[8];
        for (size_t i = 0; i < nt; i++) m[i] = xstrdup(type_phrase(ts[i]));
        char *r = union_expected(m, nt);
        for (size_t i = 0; i < nt; i++) free(m[i]);
        return r;
    }
    const char *t = ts[0];
    if (!strcmp(t, "string")) {
        json_t *mn = json_get(s, "minLength"), *mx = json_get(s, "maxLength");
        if (mn && mx) return xasprintf("a string (length %lld to %lld)", (long long)mn->u.num.d, (long long)mx->u.num.d);
        if (mn) return xasprintf("a string (length at least %lld)", (long long)mn->u.num.d);
        if (mx) return xasprintf("a string (length at most %lld)", (long long)mx->u.num.d);
        return xstrdup("a string");
    }
    if (!strcmp(t, "number") || !strcmp(t, "integer")) {
        sb_t b = SB_INIT;
        sb_append(&b, !strcmp(t, "integer") ? "an integer" : "a number");
        json_t *mn = json_get(s, "minimum"), *xmn = json_get(s, "exclusiveMinimum");
        json_t *mx = json_get(s, "maximum"), *xmx = json_get(s, "exclusiveMaximum");
        const json_t *lo = json_is_num(xmn) ? xmn : mn, *hi = json_is_num(xmx) ? xmx : mx;
        if (json_is_num(lo)) {
            char *n = num_str(lo->u.num.d);
            sb_appendf(&b, " %s %s", lo == xmn ? "more than" : "at least", n);
            free(n);
        }
        if (json_is_num(hi)) {
            char *n = num_str(hi->u.num.d);
            sb_appendf(&b, "%s %s %s", json_is_num(lo) ? " and" : "", hi == xmx ? "less than" : "at most", n);
            free(n);
        }
        return sb_detach(&b);
    }
    return xstrdup(type_phrase(t));
}

static void check_node(const json_t *s, const json_t *v, const json_t *path, issues_t *is, int depth);

static bool node_valid(const json_t *s, const json_t *v, int depth) {
    issues_t tmp = {0};
    json_t *p = json_arr();
    check_node(s, v, p, &tmp, depth);
    bool ok = tmp.n == 0;
    issues_free(&tmp);
    json_free(p);
    return ok;
}

static void range_issue(const json_t *s, const json_t *v, const json_t *path, issues_t *is, const char *desc) {
    double d = v->u.num.d;
    json_t *mn = json_get(s, "minimum"), *xmn = json_get(s, "exclusiveMinimum");
    json_t *mx = json_get(s, "maximum"), *xmx = json_get(s, "exclusiveMaximum");
    const json_t *lo = json_is_num(xmn) ? xmn : json_is_num(mn) ? mn : NULL, *hi = json_is_num(xmx) ? xmx : json_is_num(mx) ? mx : NULL;
    if (lo && (lo == xmn ? d <= lo->u.num.d : d < lo->u.num.d)) {
        char *n = num_str(lo->u.num.d);
        char *e = lo->u.num.d == 0 ? xstrdup(lo == xmn ? "positive" : "non-negative")
                                   : xasprintf("a number %s %s", lo == xmn ? "more than" : "at least", n);
        add_issue(is, path, e, desc, v, false);
        free(e);
        free(n);
    }
    if (hi && (hi == xmx ? d >= hi->u.num.d : d > hi->u.num.d)) {
        char *n = num_str(hi->u.num.d);
        char *e = hi->u.num.d == 0 ? xstrdup(hi == xmx ? "negative" : "non-positive")
                                   : xasprintf("a number %s %s", hi == xmx ? "less than" : "at most", n);
        add_issue(is, path, e, desc, v, false);
        free(e);
        free(n);
    }
}

static void length_issue(const json_t *s, size_t len, const json_t *path, issues_t *is, const char *desc, const char *kmin,
                         const char *kmax) {
    json_t *mn = json_get(s, kmin), *mx = json_get(s, kmax);
    json_t *lj = json_int((int64_t)len);
    if (json_is_num(mn) && (double)len < mn->u.num.d) {
        char *e = xasprintf("at least length %lld", (long long)mn->u.num.d);
        add_issue(is, path, e, desc, lj, false);
        free(e);
    } else if (json_is_num(mx) && (double)len > mx->u.num.d) {
        char *e = xasprintf("at most length %lld", (long long)mx->u.num.d);
        add_issue(is, path, e, desc, lj, false);
        free(e);
    }
    json_free(lj);
}

/* omptype collect-check of value v against schema node s */
static void check_node(const json_t *s, const json_t *v, const json_t *path, issues_t *is, int depth) {
    if (!json_is_obj(s) || depth > MAX_DEPTH) return;
    const char *desc = json_get_str(s, "description");
    json_t *any = json_get_arr(s, "anyOf");
    if (!any) any = json_get_arr(s, "oneOf");
    if (any) {
        json_t *b;
        json_arr_foreach(any, i, b) if (node_valid(b, v, depth + 1)) return;
        json_t *d2 = json_clone(s);
        json_del(d2, "description");
        char *raw = expected_of(d2, depth);
        add_issue(is, path, raw, desc, v, false);
        free(raw);
        json_free(d2);
        return;
    }
    json_t *en = json_get_arr(s, "enum");
    if (en || json_get(s, "const")) {
        bool found = false;
        json_t *e;
        if (en) json_arr_foreach(en, i, e) if (json_equal(e, v)) found = true;
        if (!en && json_equal(json_get(s, "const"), v)) found = true;
        if (!found) {
            json_t *d2 = json_clone(s);
            json_del(d2, "description");
            char *raw = expected_of(d2, depth);
            add_issue(is, path, raw, desc, v, false);
            free(raw);
            json_free(d2);
        }
        return;
    }
    const char *ts[8];
    size_t nt = schema_types(s, ts, 8);
    if (nt > 1) {
        bool ok = false;
        for (size_t i = 0; i < nt; i++) ok |= type_ok(ts[i], v);
        if (!ok) {
            json_t *d2 = json_clone(s);
            json_del(d2, "description");
            char *raw = expected_of(d2, depth);
            add_issue(is, path, raw, desc, v, false);
            free(raw);
            json_free(d2);
            return;
        }
        /* narrow to the matching member for its own checks */
        for (size_t i = 0; i < nt; i++)
            if (type_ok(ts[i], v)) {
                ts[0] = ts[i];
                break;
            }
        nt = 1;
    }
    const char *t = nt ? ts[0] : NULL;
    if (!t) {
        /* untyped node with properties/items: treat as object/array */
        if (json_get(s, "properties") || json_get(s, "required")) t = "object";
        else if (json_get(s, "items")) t = "array";
        else return;
    }
    if (!strcmp(t, "string")) {
        if (!json_is_str(v)) add_issue(is, path, "a string", desc, v, false);
        else length_issue(s, utf16_len(v->u.str.s, v->u.str.len), path, is, desc, "minLength", "maxLength");
    } else if (!strcmp(t, "number") || !strcmp(t, "integer")) {
        bool integer = !strcmp(t, "integer");
        if (!json_is_num(v) || !isfinite(v->u.num.d)) add_issue(is, path, integer ? "an integer" : "a number", desc, v, false);
        else {
            if (integer && !is_int_val(v)) add_issue(is, path, "an integer", desc, v, false);
            range_issue(s, v, path, is, desc);
        }
    } else if (!strcmp(t, "boolean")) {
        if (!json_is_bool(v)) add_issue(is, path, "boolean", desc, v, false);
    } else if (!strcmp(t, "null")) {
        if (!json_is_null(v)) add_issue(is, path, "null", desc, v, false);
    } else if (!strcmp(t, "array")) {
        if (!json_is_arr(v)) {
            add_issue(is, path, "an array", desc, v, false);
            return;
        }
        size_t before = is->n;
        length_issue(s, json_len(v), path, is, desc, "minItems", "maxItems");
        if (is->n != before) return;
        json_t *items = json_get_obj(s, "items"), *x;
        if (items)
            json_arr_foreach(v, i, x) {
                json_t *p = path_with(path, NULL, (long)i);
                check_node(items, x, p, is, depth + 1);
                json_free(p);
            }
    } else if (!strcmp(t, "object")) {
        if (!json_is_obj(v) && !json_is_arr(v)) { /* typeof [] === "object" */
            add_issue(is, path, "an object", desc, v, false);
            return;
        }
        const char *k;
        json_t *ps;
        json_obj_foreach(json_get_obj(s, "properties"), i, k, ps) {
            json_t *p = path_with(path, k, 0);
            json_t *child = json_is_obj(v) ? json_get(v, k) : NULL;
            if (!child) {
                if (is_required(s, k)) {
                    char *e = expected_of(ps, depth + 1);
                    char *raw = json_get_str(ps, "description") ? NULL : e;
                    add_issue(is, p, raw ? raw : "unknown", raw ? NULL : e, NULL, true);
                    free(e);
                }
            } else {
                check_node(ps, child, p, is, depth + 1);
            }
            json_free(p);
        }
        /* required keys the properties do not declare */
        json_t *r;
        json_arr_foreach(json_get_arr(s, "required"), i, r) {
            if (!json_is_str(r) || json_get(json_get_obj(s, "properties"), json_sv(r))) continue;
            if (json_is_obj(v) && json_get(v, json_sv(r))) continue;
            json_t *p = path_with(path, json_sv(r), 0);
            add_issue(is, p, "unknown", NULL, NULL, true);
            json_free(p);
        }
    }
}

/* ---------- JSON-schema type issues (drive the coercion passes) ---------- */

typedef struct {
    json_t *path;
    bool unrecognized;
    const char *types[8];
    size_t ntypes;
} jissue_t;
typedef VEC(jissue_t) jissues_t;

static void jissues_free(jissues_t *js) {
    for (size_t i = 0; i < js->n; i++) json_free(js->v[i].path);
    vec_free(js);
}

static void json_issues(const json_t *s, const json_t *v, const json_t *path, jissues_t *out, int depth) {
    if (!json_is_obj(s) || depth > MAX_DEPTH || !v) return;
    if (json_get(s, "anyOf") || json_get(s, "oneOf")) return; /* union branches: no authoritative repair */
    const char *ts[8];
    size_t nt = schema_types(s, ts, 8);
    if (nt) {
        bool ok = false;
        for (size_t i = 0; i < nt; i++) ok |= type_ok(ts[i], v);
        if (!ok) {
            jissue_t j = {.path = json_clone(path), .ntypes = nt};
            memcpy(j.types, ts, nt * sizeof *ts);
            vec_push(out, j);
            return;
        }
    }
    if (json_is_obj(v)) {
        json_t *props = json_get_obj(s, "properties");
        const char *k;
        json_t *x;
        json_obj_foreach(v, i, k, x) {
            json_t *ps = json_get(props, k);
            json_t *p = path_with(path, k, 0);
            if (ps) json_issues(ps, x, p, out, depth + 1);
            else if (json_is_bool(json_get(s, "additionalProperties")) && !json_get_bool(s, "additionalProperties", true)) {
                jissue_t j = {.path = json_clone(p), .unrecognized = true};
                vec_push(out, j);
            }
            json_free(p);
        }
    } else if (json_is_arr(v)) {
        json_t *items = json_get_obj(s, "items"), *x;
        if (items)
            json_arr_foreach(v, i, x) {
                json_t *p = path_with(path, NULL, (long)i);
                json_issues(items, x, p, out, depth + 1);
                json_free(p);
            }
    }
}

/* ---------- coercions (validation.ts tryCoerceForExpectedTypes) ---------- */

static bool numeric_string(const char *s) {
    /* ^[+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?$ */
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) p++;
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    return *p == 0;
}

static bool json_number_literal(const char *s) {
    /* ^[+-]?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?$ */
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') while (isdigit((unsigned char)*p)) p++;
    else return false;
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    return *p == 0;
}

static char *trim_dup(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

static bool matches_types(const json_t *v, const char *const *ts, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (type_ok(ts[i], v)) return true;
    return false;
}

static json_t *parse_number_string(const char *s, const char *const *ts, size_t n) {
    if (!has_type(ts, n, "number") && !has_type(ts, n, "integer")) return NULL;
    char *t = trim_dup(s);
    json_t *r = NULL;
    if (*t && numeric_string(t)) {
        double d = strtod(t, NULL);
        if (isfinite(d)) {
            json_t *j = json_num(d);
            if (matches_types(j, ts, n)) r = j;
            else json_free(j);
        }
    }
    free(t);
    return r;
}

static bool looks_json_container(const char *t) {
    while (*t && isspace((unsigned char)*t)) t++;
    if (*t == '{') {
        const char *b = t + 1;
        const char *bs = b;
        while (*bs && isspace((unsigned char)*bs)) bs++;
        return *bs == '"' || strchr(b, ':') || *bs == '}';
    }
    if (*t != '[') return false;
    const char *f = t + 1;
    while (*f && isspace((unsigned char)*f)) f++;
    if (*f == '{' || *f == '[' || *f == '"' || *f == ']' || !strncmp(f, "true", 4) || !strncmp(f, "false", 5) || !strncmp(f, "null", 4))
        return true;
    /* -?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?\s*(,|\]|$) */
    const char *p = f;
    if (*p == '-') p++;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') while (isdigit((unsigned char)*p)) p++;
    else return false;
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    return *p == ',' || *p == ']' || *p == 0;
}

/* tryParseJsonForTypes; returns an owned replacement or NULL */
static json_t *parse_json_for_types(const char *value, const char *const *ts, size_t n, int depth) {
    char *t = trim_dup(value);
    if (!*t) {
        free(t);
        return NULL;
    }
    json_t *num = parse_number_string(t, ts, n);
    if (num) {
        free(t);
        return num;
    }
    bool obj = t[0] == '{' && looks_json_container(t), arr = t[0] == '[' && looks_json_container(t);
    bool str = t[0] == '"' && !has_type(ts, n, "string");
    bool lit = !strcmp(t, "true") || !strcmp(t, "false") || !strcmp(t, "null") || json_number_literal(t);
    json_t *r = NULL;
    if (obj || arr || str || lit) {
        json_t *p = json_parse_str(t);
        if (p) {
            if (p->type == JSON_NULL && !strcmp(t, "null")) r = p;
            else if (matches_types(p, ts, n)) r = p;
            else if (json_is_str(p) && !has_type(ts, n, "string") && depth < 3) {
                r = parse_json_for_types(json_sv(p), ts, n, depth + 1);
                json_free(p);
            } else json_free(p);
        }
    }
    free(t);
    return r;
}

static json_t *coerce_bool(const json_t *v, const char *const *ts, size_t n) {
    if (!has_type(ts, n, "boolean")) return NULL;
    if (json_is_num(v)) {
        if (v->u.num.d == 0) return json_bool(false);
        if (v->u.num.d == 1) return json_bool(true);
        return NULL;
    }
    if (!json_is_str(v)) return NULL;
    char *t = trim_dup(json_sv(v));
    for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
    json_t *r = NULL;
    if (!strcmp(t, "true") || !strcmp(t, "1") || !strcmp(t, "yes") || !strcmp(t, "on")) r = json_bool(true);
    else if (!strcmp(t, "false") || !strcmp(t, "0") || !strcmp(t, "no") || !strcmp(t, "off")) r = json_bool(false);
    free(t);
    return r;
}

static json_t *coerce_for_types(const json_t *v, const char *const *ts, size_t n) {
    if (json_is_str(v)) {
        json_t *p = parse_json_for_types(json_sv(v), ts, n, 0);
        return p ? p : coerce_bool(v, ts, n);
    }
    json_t *b = coerce_bool(v, ts, n);
    if (b) return b;
    if (json_is_bool(v) && (has_type(ts, n, "number") || has_type(ts, n, "integer"))) return json_int(v->u.b ? 1 : 0);
    if (!has_type(ts, n, "string") || !v || v->type == JSON_NULL) return NULL;
    if (json_is_arr(v) || json_is_obj(v)) return json_str_own(json_dump(v, 0));
    if (json_is_num(v)) return json_str_own(num_str(v->u.num.d));
    if (json_is_bool(v)) return json_str(v->u.b ? "true" : "false");
    return NULL;
}

/* ---------- path access ---------- */

static json_t **slot_at(json_t **root, const json_t *path) {
    json_t **cur = root;
    json_t *k;
    json_arr_foreach(path, i, k) {
        json_t *c = *cur;
        if (json_is_num(k)) {
            size_t idx = (size_t)k->u.num.d;
            if (!json_is_arr(c) || idx >= c->u.arr.n) return NULL;
            cur = &c->u.arr.items[idx];
        } else {
            if (!json_is_obj(c)) return NULL;
            size_t m = c->u.obj.n, j;
            for (j = 0; j < m; j++)
                if (!strcmp(c->u.obj.keys[j], json_sv(k))) break;
            if (j == m) return NULL;
            cur = &c->u.obj.vals[j];
        }
    }
    return cur;
}

static bool delete_at(json_t *root, const json_t *path) {
    size_t n = json_len(path);
    if (!n) return false;
    json_t *parent_path = json_arr();
    for (size_t i = 0; i + 1 < n; i++) json_push(parent_path, json_clone(json_at(path, i)));
    json_t **ps = slot_at(&root, parent_path);
    json_free(parent_path);
    json_t *last = json_at(path, n - 1);
    if (!ps || !json_is_obj(*ps) || !json_is_str(last) || !json_get(*ps, json_sv(last))) return false;
    json_del(*ps, json_sv(last));
    return true;
}

/* coerceArgsFromIssues */
static bool coerce_from_issues(json_t **args, const jissues_t *js) {
    bool changed = false;
    for (size_t i = 0; i < js->n; i++) {
        const jissue_t *j = &js->v[i];
        if (j->unrecognized) {
            if (delete_at(*args, j->path)) changed = true;
            continue;
        }
        json_t **slot = slot_at(args, j->path);
        if (!slot) continue;
        json_t *cur = *slot;
        json_t *rep = coerce_for_types(cur, j->types, j->ntypes);
        if (!rep && has_type(j->types, j->ntypes, "array") && !json_is_arr(cur)) {
            json_t *obj = NULL;
            if (json_is_str(cur)) {
                const char *ot[1] = {"object"};
                obj = parse_json_for_types(json_sv(cur), ot, 1, 0);
            }
            if (obj || !(json_is_str(cur) && looks_json_container(json_sv(cur)))) {
                rep = json_arr();
                json_push(rep, obj ? obj : json_clone(cur));
            }
        }
        if (!rep) continue;
        json_free(*slot);
        *slot = rep;
        changed = true;
    }
    return changed;
}

/* ---------- normalizations ---------- */

static const char *const IDENT_KEYS[] = {"path", "paths", "file", "file_path", "filePath", "filepath", "url", "uri", "title", "label"};
static const char *const CONTENT_KEYS[] = {"content", "input", "body", "text", "command", "code"};

static bool in_list(const char *k, const char *const *l, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(k, l[i])) return true;
    return false;
}

/* normalizeDoubleEncodedKeys */
static bool norm_keys(json_t *v, int depth) {
    if (depth > MAX_DEPTH) return false;
    bool changed = false;
    if (json_is_arr(v)) {
        json_t *x;
        json_arr_foreach(v, i, x) changed |= norm_keys(x, depth + 1);
        return changed;
    }
    if (!json_is_obj(v)) return false;
    for (size_t i = 0; i < v->u.obj.n; i++) {
        changed |= norm_keys(v->u.obj.vals[i], depth + 1);
        char *key = v->u.obj.keys[i];
        char *cur = xstrdup(key), *dec = NULL;
        for (int d = 0; d < 3; d++) {
            size_t n = strlen(cur);
            if (n < 2 || cur[0] != '"' || cur[n - 1] != '"') break;
            json_t *p = json_parse_str(cur);
            if (!json_is_str(p)) {
                json_free(p);
                break;
            }
            free(cur);
            cur = xstrdup(json_sv(p));
            json_free(p);
            free(dec);
            dec = xstrdup(cur);
        }
        free(cur);
        if (dec && strcmp(dec, key) && !json_get(v, dec)) {
            free(v->u.obj.keys[i]);
            v->u.obj.keys[i] = dec;
            changed = true;
        } else {
            free(dec);
        }
    }
    return changed;
}

/* normalizeOptionalNullsForSchema (anyOf branches are left alone) */
static json_t *norm_nulls(const json_t *s, json_t *v, bool is_root, bool *changed, int depth) {
    if (!v || v->type == JSON_NULL || !json_is_obj(s) || depth > MAX_DEPTH) return v;
    if (json_get(s, "anyOf") || json_get(s, "oneOf")) return v;
    if (json_is_arr(v)) {
        json_t *items = json_get_obj(s, "items");
        if (!items) return v;
        for (size_t i = 0; i < v->u.arr.n; i++) v->u.arr.items[i] = norm_nulls(items, v->u.arr.items[i], false, changed, depth + 1);
        return v;
    }
    const char *t = json_get_str(s, "type");
    if ((str_eq(t, "number") || str_eq(t, "integer")) && json_is_str(v)) {
        const char *ts[1] = {t};
        json_t *n = parse_number_string(json_sv(v), ts, 1);
        if (n) {
            json_free(v);
            *changed = true;
            return n;
        }
        return v;
    }
    if (!str_eq(t, "object") || !json_is_obj(v)) return v;
    json_t *props = json_get_obj(s, "properties");
    if (!props) return v;
    const char *k;
    json_t *ps;
    json_obj_foreach(props, i, k, ps) {
        json_t *cur = json_get(v, k);
        if (!cur) continue;
        bool nullish = cur->type == JSON_NULL || str_eq(json_sv(cur), "null");
        bool req = is_required(s, k);
        bool bad_empty = json_is_str(cur) && cur->u.str.len == 0 && !req && !node_valid(ps, cur, depth + 1);
        if ((nullish || bad_empty) && !req) {
            json_del(v, k);
            *changed = true;
            continue;
        }
        if (nullish && json_get(ps, "default")) {
            json_set(v, k, json_clone(json_get(ps, "default")));
            *changed = true;
            continue;
        }
        for (size_t j = 0; j < v->u.obj.n; j++)
            if (!strcmp(v->u.obj.keys[j], k)) v->u.obj.vals[j] = norm_nulls(ps, v->u.obj.vals[j], false, changed, depth + 1);
    }
    if (!is_root && json_is_bool(json_get(s, "additionalProperties")) && !json_get_bool(s, "additionalProperties", true)) {
        for (size_t j = 0; j < v->u.obj.n;) {
            json_t *x = v->u.obj.vals[j];
            if (!json_get(props, v->u.obj.keys[j]) && (x->type == JSON_NULL || str_eq(json_sv(x), "null"))) {
                json_del(v, v->u.obj.keys[j]);
                *changed = true;
            } else j++;
        }
    }
    return v;
}

/* normalizeEnumStringWhitespace */
static json_t *norm_enum_ws(const json_t *s, json_t *v, bool *changed, int depth) {
    if (!v || !json_is_obj(s) || depth > MAX_DEPTH) return v;
    if (json_is_str(v)) {
        char *t = trim_dup(json_sv(v));
        if (strcmp(t, json_sv(v))) {
            json_t *en = json_get_arr(s, "enum");
            json_t *tj = json_str(t);
            bool has_raw = false, has_trim = false;
            json_t *e;
            json_arr_foreach(en, i, e) {
                if (json_equal(e, v)) has_raw = true;
                if (json_equal(e, tj)) has_trim = true;
            }
            if ((en && !has_raw && has_trim) || json_equal(json_get(s, "const"), tj)) {
                json_free(v);
                free(t);
                *changed = true;
                return tj;
            }
            json_free(tj);
        }
        free(t);
        return v;
    }
    if (json_is_arr(v)) {
        json_t *items = json_get_obj(s, "items");
        if (items)
            for (size_t i = 0; i < v->u.arr.n; i++) v->u.arr.items[i] = norm_enum_ws(items, v->u.arr.items[i], changed, depth + 1);
        return v;
    }
    if (!json_is_obj(v)) return v;
    json_t *props = json_get_obj(s, "properties");
    for (size_t j = 0; props && j < v->u.obj.n; j++) {
        json_t *ps = json_get(props, v->u.obj.keys[j]);
        if (ps) v->u.obj.vals[j] = norm_enum_ws(ps, v->u.obj.vals[j], changed, depth + 1);
    }
    return v;
}

static bool trim_trailing_nl(json_t *v) {
    if (!json_is_str(v)) return false;
    size_t n = v->u.str.len;
    while (n && (v->u.str.s[n - 1] == '\n' || v->u.str.s[n - 1] == '\r')) n--;
    if (n == v->u.str.len) return false;
    v->u.str.s[n] = 0;
    v->u.str.len = n;
    return true;
}

/* normalizeIdentifierStringWhitespace */
static bool norm_ident(json_t *v, int depth) {
    if (depth > MAX_DEPTH) return false;
    bool changed = false;
    if (json_is_arr(v)) {
        json_t *x;
        json_arr_foreach(v, i, x) changed |= norm_ident(x, depth + 1);
        return changed;
    }
    if (!json_is_obj(v)) return false;
    for (size_t i = 0; i < v->u.obj.n; i++) {
        const char *k = v->u.obj.keys[i];
        json_t *x = v->u.obj.vals[i];
        if (in_list(k, CONTENT_KEYS, sizeof CONTENT_KEYS / sizeof *CONTENT_KEYS)) continue;
        if (in_list(k, IDENT_KEYS, sizeof IDENT_KEYS / sizeof *IDENT_KEYS)) {
            if (json_is_str(x)) changed |= trim_trailing_nl(x);
            else if (json_is_arr(x)) {
                json_t *y;
                json_arr_foreach(x, j, y) changed |= trim_trailing_nl(y);
            }
        }
        changed |= norm_ident(x, depth + 1);
    }
    return changed;
}

static bool accepts_string_and_array(const json_t *s) {
    const char *ts[8];
    size_t n = schema_types(s, ts, 8);
    if (has_type(ts, n, "string") && has_type(ts, n, "array")) return true;
    const char *kw[2] = {"anyOf", "oneOf"};
    for (int k = 0; k < 2; k++) {
        bool hs = false, ha = false;
        json_t *b;
        json_arr_foreach(json_get_arr(s, kw[k]), i, b) {
            const char *bt[8];
            size_t bn = schema_types(b, bt, 8);
            hs |= has_type(bt, bn, "string");
            ha |= has_type(bt, bn, "array");
        }
        if (hs && ha) return true;
    }
    return false;
}

/* normalizeStringEncodedArrayUnions */
static json_t *norm_str_arrays(const json_t *s, json_t *v, bool *changed, int depth) {
    if (!v || v->type == JSON_NULL || !json_is_obj(s) || depth > MAX_DEPTH) return v;
    if (json_is_str(v) && accepts_string_and_array(s)) {
        char *t = trim_dup(json_sv(v));
        json_t *p = t[0] == '[' ? json_parse_str(t) : NULL;
        free(t);
        if (json_is_arr(p)) {
            norm_keys(p, 0);
            bool ok = false;
            const char *ts[8];
            size_t n = schema_types(s, ts, 8);
            if (has_type(ts, n, "array")) ok = node_valid(s, p, depth);
            const char *kw[2] = {"anyOf", "oneOf"};
            for (int k = 0; k < 2 && !ok; k++) {
                json_t *b;
                json_arr_foreach(json_get_arr(s, kw[k]), i, b) {
                    const char *bt[8];
                    size_t bn = schema_types(b, bt, 8);
                    if (has_type(bt, bn, "array") && node_valid(b, p, depth + 1)) ok = true;
                }
            }
            if (ok) {
                json_free(v);
                *changed = true;
                return p;
            }
        }
        json_free(p);
        return v;
    }
    if (json_is_arr(v)) {
        json_t *items = json_get_obj(s, "items");
        if (items)
            for (size_t i = 0; i < v->u.arr.n; i++) v->u.arr.items[i] = norm_str_arrays(items, v->u.arr.items[i], changed, depth + 1);
        return v;
    }
    if (!str_eq(json_get_str(s, "type"), "object") || !json_is_obj(v)) return v;
    json_t *props = json_get_obj(s, "properties");
    for (size_t j = 0; props && j < v->u.obj.n; j++) {
        json_t *ps = json_get(props, v->u.obj.keys[j]);
        if (ps) v->u.obj.vals[j] = norm_str_arrays(ps, v->u.obj.vals[j], changed, depth + 1);
    }
    return v;
}

/* normalizeSingleStringField */
static bool norm_single_string(const json_t *s, json_t *v) {
    if (!str_eq(json_get_str(s, "type"), "object") || !json_is_obj(v)) return false;
    json_t *props = json_get_obj(s, "properties");
    json_t *req = json_get_arr(s, "required");
    if (json_len(props) != 1 || json_len(req) != 1) return false;
    const char *key = props->u.obj.keys[0];
    if (!str_eq(json_sv(json_at(req, 0)), key) || !str_eq(json_get_str(props->u.obj.vals[0], "type"), "string")) return false;
    if (json_get(v, key)) return false;
    const char *adopted = NULL;
    for (size_t i = 0; i < v->u.obj.n; i++) {
        if (!json_is_str(v->u.obj.vals[i])) continue;
        if (adopted) return false;
        adopted = v->u.obj.keys[i];
    }
    if (!adopted) return false;
    json_t *val = json_take(v, adopted);
    json_set(v, key, val);
    return true;
}

static bool normalize_all(const json_t *schema, json_t **args, bool keys) {
    bool ch = false;
    if (keys) ch |= norm_keys(*args, 0);
    *args = norm_nulls(schema, *args, true, &ch, 0);
    *args = norm_enum_ws(schema, *args, &ch, 0);
    ch |= norm_ident(*args, 0);
    *args = norm_str_arrays(schema, *args, &ch, 0);
    ch |= norm_ident(*args, 0);
    ch |= norm_single_string(schema, *args);
    return ch;
}

/* ---------- public ---------- */

static void collect(const json_t *schema, const json_t *v, issues_t *is) {
    json_t *p = json_arr();
    check_node(schema, v, p, is, 0);
    json_free(p);
}

char *schema_summary(const json_t *schema, const json_t *value) {
    issues_t is = {0};
    collect(schema, value, &is);
    if (!is.n) {
        issues_free(&is);
        return NULL;
    }
    sb_t b = SB_INIT;
    bool same = is.n > 1;
    for (size_t i = 1; i < is.n && same; i++)
        if (!json_equal(is.v[i].path, is.v[0].path) || is.v[i].missing != is.v[0].missing ||
            !json_equal(is.v[i].data, is.v[0].data))
            same = false;
    if (same) {
        char *at = format_path(is.v[0].path);
        const json_t *d = is.v[0].data;
        char *act = is.v[0].missing ? xstrdup("undefined") : json_is_str(d) ? json_dump(d, 0) : describe_value(d);
        if (json_is_arr(d) || json_is_obj(d)) {
            free(act);
            act = xstrdup(json_is_arr(d) ? "" : "[object Object]");
        }
        sb_appendf(&b, "%s%s(%s) must be...", at, *at ? " " : "", act);
        for (size_t i = 0; i < is.n; i++) sb_appendf(&b, "\n  \xe2\x97\xa6 %s", is.v[i].expected);
        free(at);
        free(act);
    } else {
        for (size_t i = 0; i < is.n; i++) {
            char *m = issue_message(&is.v[i]);
            sb_appendf(&b, "%s%s", i ? "\n" : "", m);
            free(m);
        }
    }
    issues_free(&is);
    return sb_detach(&b);
}

/* truncateArgsForError: strings over 256 UTF-16 units are cut with `… [truncated N chars]` */
static void truncate_strings(json_t *v, int depth) {
    if (!v || depth > MAX_DEPTH) return;
    if (json_is_str(v)) {
        size_t u = utf16_len(v->u.str.s, v->u.str.len);
        if (u <= 256) return;
        size_t i = 0, units = 0;
        while (i < v->u.str.len && units < 256) {
            unsigned char c = (unsigned char)v->u.str.s[i];
            size_t k = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            if (k == 4 && units == 255) break;
            units += k == 4 ? 2 : 1;
            i += k;
        }
        char *s = xasprintf("%.*s\xe2\x80\xa6 [truncated %zu chars]", (int)i, v->u.str.s, u - 256);
        free(v->u.str.s);
        v->u.str.s = s;
        v->u.str.len = strlen(s);
        return;
    }
    json_t *x;
    if (json_is_arr(v)) json_arr_foreach(v, i, x) truncate_strings(x, depth + 1);
    if (json_is_obj(v))
        for (size_t i = 0; i < v->u.obj.n; i++) truncate_strings(v->u.obj.vals[i], depth + 1);
}

char *tool_args_validate(const json_t *schema, json_t **args, char **received) {
    if (received) *received = NULL;
    if (!json_is_obj(schema)) return NULL;
    json_t *orig = json_clone(*args);
    json_t *norm = json_clone(*args);
    bool changed = normalize_all(schema, &norm, true);
    issues_t is = {0};
    collect(schema, norm, &is);
    for (int pass = 0; pass < 5 && is.n; pass++) {
        jissues_t js = {0};
        json_t *p = json_arr();
        json_issues(schema, norm, p, &js, 0);
        json_free(p);
        bool pc = coerce_from_issues(&norm, &js);
        jissues_free(&js);
        pc |= normalize_all(schema, &norm, true);
        if (!pc) break;
        changed = true;
        issues_free(&is);
        is = (issues_t){0};
        collect(schema, norm, &is);
    }
    if (!is.n) {
        issues_free(&is);
        json_free(orig);
        json_free(*args);
        *args = norm;
        return NULL;
    }
    sb_t b = SB_INIT;
    for (size_t i = 0; i < is.n; i++) {
        sb_t ip = SB_INIT;
        json_t *k;
        json_arr_foreach(is.v[i].path, j, k) {
            if (j) sb_putc(&ip, '/');
            if (json_is_num(k)) sb_appendf(&ip, "%lld", (long long)k->u.num.d);
            else sb_append(&ip, json_sv(k));
        }
        char *m = issue_message(&is.v[i]);
        sb_appendf(&b, "%s  - %s: %s", i ? "\n" : "", ip.len ? sb_cstr(&ip) : "root", m);
        free(m);
        sb_free(&ip);
    }
    issues_free(&is);
    if (received) {
        json_t *shown;
        if (changed) {
            shown = json_obj();
            truncate_strings(orig, 0);
            truncate_strings(norm, 0);
            json_set(shown, "original", orig);
            json_set(shown, "normalized", norm);
            orig = norm = NULL;
        } else {
            shown = orig;
            truncate_strings(shown, 0);
            orig = NULL;
        }
        *received = json_dump(shown, 2);
        json_free(shown);
    }
    json_free(orig);
    json_free(norm);
    return sb_detach(&b);
}

char *schema_validate_args(const json_t *schema, json_t *args, char **received) {
    json_t *a = json_clone(args);
    char *err = tool_args_validate(schema, &a, received);
    if (!err && json_is_obj(a) && json_is_obj(args)) {
        while (args->u.obj.n) json_del(args, args->u.obj.keys[0]);
        for (size_t i = 0; i < a->u.obj.n; i++) json_set(args, a->u.obj.keys[i], json_clone(a->u.obj.vals[i]));
    }
    json_free(a);
    return err;
}

char *schema_validate(const json_t *schema, json_t *args) { return schema_validate_args(schema, args, NULL); }
