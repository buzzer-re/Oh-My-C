/* toolschema.c - see toolschema.h. Ports of omp's packages/ai/src/utils/schema/{draft,dereference,wire,normalize}.ts
 * as of v19.1.1, key order included: every node is built in the order omp's object spreads and assignments produce. */
#include "ai/toolschema.h"

#include <ctype.h>
#include <math.h>

#define TS_MAX_DEPTH 512
#define IN(k, list) ts_in(k, list, ARRAY_LEN(list))

static bool ts_in(const char *k, const char *const *list, size_t n) {
    for (size_t i = 0; k && i < n; i++)
        if (!strcmp(k, list[i])) return true;
    return false;
}

static bool is_empty_obj(const json_t *v) { return json_is_obj(v) && json_len(v) == 0; }

static json_t *obj1(const char *k, json_t *v) {
    json_t *o = json_obj();
    json_set(o, k, v);
    return o;
}

static json_t *null_type(void) { return obj1("type", json_str("null")); }

/* push `v` (cloned) unless an equal value is already there (pushEnumValue / mergeArrayValues) */
static void push_unique(json_t *arr, const json_t *v) {
    const json_t *e;
    json_arr_foreach(arr, i, e) if (json_equal(e, v)) return;
    json_push(arr, json_clone(v));
}

/* ---------- JS whitespace (String.prototype.trim, \s) ---------- */

static size_t js_ws(const char *s, size_t n) {
    if (!n) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c == ' ' || (c >= '\t' && c <= '\r')) return 1;
    if (c < 0x80) return 0;
    uint32_t cp;
    int k = utf8_decode(s, n, &cp);
    bool sp = cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
              cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
    return sp && k > 0 ? (size_t)k : 0;
}

/* trim(s[0..n)): owned */
static char *js_trim(const char *s, size_t n) {
    size_t a = 0, k;
    while (a < n && (k = js_ws(s + a, n - a))) a += k;
    size_t b = n;
    while (b > a) {
        size_t st = b - 1;
        while (st > a && ((unsigned char)s[st] & 0xC0) == 0x80 && b - st < 4) st--;
        k = js_ws(s + st, b - st);
        if (k != b - st || !k) break;
        b = st;
    }
    return xstrndup(s + a, b - a);
}

/* ---------- local `$ref` pointers (omptype local-ref.ts) ---------- */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* decodeURIComponent: owned, NULL for a malformed escape or escapes that are not UTF-8 */
static char *uri_decode(const char *s) {
    sb_t b = SB_INIT;
    for (size_t i = 0; s[i];) {
        if (s[i] != '%') {
            sb_putc(&b, s[i++]);
            continue;
        }
        int hi = hexval(s[i + 1]), lo = hi < 0 ? -1 : hexval(s[i + 2]);
        if (hi < 0 || lo < 0) goto bad;
        unsigned c = (unsigned)(hi * 16 + lo);
        i += 3;
        if (c < 0x80) {
            sb_putc(&b, (char)c);
            continue;
        }
        int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!n) goto bad;
        unsigned char buf[4] = {(unsigned char)c};
        uint32_t cp = c & (n == 2 ? 0x1Fu : n == 3 ? 0x0Fu : 0x07u);
        for (int k = 1; k < n; k++) {
            if (s[i] != '%') goto bad;
            int h2 = hexval(s[i + 1]), l2 = h2 < 0 ? -1 : hexval(s[i + 2]);
            if (h2 < 0 || l2 < 0) goto bad;
            unsigned cc = (unsigned)(h2 * 16 + l2);
            if ((cc & 0xC0) != 0x80) goto bad;
            buf[k] = (unsigned char)cc;
            cp = (cp << 6) | (cc & 0x3F);
            i += 3;
        }
        uint32_t min = n == 2 ? 0x80 : n == 3 ? 0x800 : 0x10000;
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) goto bad;
        sb_appendn(&b, (const char *)buf, (size_t)n);
    }
    return sb_detach(&b);
bad:
    sb_free(&b);
    return NULL;
}

/* s with every `from` replaced by `to` (owned) */
static char *replace_all(const char *s, const char *from, const char *to) {
    sb_t b = SB_INIT;
    size_t fl = strlen(from);
    for (const char *p = s; *p;) {
        if (!strncmp(p, from, fl)) {
            sb_append(&b, to);
            p += fl;
        } else sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

bool ts_ref_tokens(const char *ref, strvec_t *out) {
    if (!ref || ref[0] != '#') return false;
    char *p = uri_decode(ref + 1);
    if (!p) return false;
    if (p[0] && p[0] != '/') {
        free(p);
        return false;
    }
    if (p[0]) { /* pointer.split("/").slice(1), each token ~1 -> "/", then ~0 -> "~" */
        for (const char *s = p + 1;;) {
            const char *e = strchr(s, '/');
            char *tok = xstrndup(s, e ? (size_t)(e - s) : strlen(s));
            char *a = replace_all(tok, "~1", "/");
            strvec_push_own(out, replace_all(a, "~0", "~"));
            free(a);
            free(tok);
            if (!e) break;
            s = e + 1;
        }
    }
    free(p);
    return true;
}

/* encodeURIComponent with `$` kept readable (%24 -> $) */
static void uri_encode_sb(sb_t *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()$", c))
            sb_putc(b, (char)c);
        else sb_appendf(b, "%%%02X", c);
    }
}

char *ts_ref_format(const strvec_t *tokens) {
    if (!tokens->n) return xstrdup("#");
    sb_t b = SB_INIT;
    sb_append(&b, "#");
    for (size_t i = 0; i < tokens->n; i++) {
        char *a = replace_all(tokens->v[i], "~", "~0");
        char *t = replace_all(a, "/", "~1");
        sb_putc(&b, '/');
        uri_encode_sb(&b, t);
        free(a);
        free(t);
    }
    return sb_detach(&b);
}

/* ---------- draft-07 -> 2020-12 (draft.ts upgradeJsonSchemaTo202012) ---------- */

static const char *const DRAFT07_URIS[] = {"http://json-schema.org/draft-07/schema#", "https://json-schema.org/draft-07/schema#",
                                           "http://json-schema.org/draft-07/schema", "https://json-schema.org/draft-07/schema"};
#define DRAFT_2020_12_URI "https://json-schema.org/draft/2020-12/schema"
static const char *const UP_MAP_KEYS[] = {"properties", "patternProperties", "dependentSchemas"};
static const char *const UP_VALUE_KEYS[] = {"const", "default", "enum", "example", "examples", "required", "dependentRequired", "type"};

static bool up_needs(const json_t *v, int depth);

static bool up_map_needs(const json_t *v, int depth) {
    const char *k;
    json_t *e;
    json_obj_foreach((json_t *)v, i, k, e) if (up_needs(e, depth + 1)) return true;
    return false;
}

static bool up_needs(const json_t *v, int depth) {
    if (depth > TS_MAX_DEPTH) return false;
    json_t *e;
    if (json_is_arr(v)) {
        json_arr_foreach((json_t *)v, i, e) if (up_needs(e, depth + 1)) return true;
        return false;
    }
    const char *k;
    json_obj_foreach((json_t *)v, i, k, e) {
        if (!strcmp(k, "$schema")) {
            if (json_is_str(e) && IN(json_sv(e), DRAFT07_URIS)) return true;
            continue;
        }
        if (!strcmp(k, "definitions") || !strcmp(k, "dependencies") || !strcmp(k, "additionalItems") || !strcmp(k, "nullable"))
            return true;
        if (!strcmp(k, "$ref")) {
            if (json_is_str(e) && str_starts(json_sv(e), "#/definitions/")) return true;
            continue;
        }
        if (!strcmp(k, "items") && json_is_arr(e)) return true;
        if (!strcmp(k, "$defs") || IN(k, UP_MAP_KEYS)) {
            if (up_map_needs(e, depth)) return true;
            continue;
        }
        if (IN(k, UP_VALUE_KEYS)) continue;
        if (up_needs(e, depth + 1)) return true;
    }
    return false;
}

/* convertRef: `#/definitions/Foo` (any URI-fragment spelling) -> `#/$defs/Foo`; other refs unchanged (owned) */
static char *up_convert_ref(const char *ref) {
    strvec_t t = {0};
    char *out;
    if (!ts_ref_tokens(ref, &t) || !t.n || strcmp(t.v[0], "definitions")) {
        out = xstrdup(ref);
    } else {
        free(t.v[0]);
        t.v[0] = xstrdup("$defs");
        out = ts_ref_format(&t);
    }
    strvec_free(&t);
    return out;
}

/* getObjectMap: target[key] when it is an object, else a fresh {} assigned there */
static json_t *up_object_map(json_t *target, const char *key) {
    json_t *m = json_get(target, key);
    if (json_is_obj(m)) return m;
    m = json_obj();
    json_set(target, key, m);
    return m;
}

static json_t *up_node(const json_t *v, int depth);

static void up_merge_map(json_t *target, const char *key, const json_t *value, int depth) {
    json_t *map = up_object_map(target, key), *e;
    const char *n;
    json_obj_foreach((json_t *)value, i, n, e) json_set(map, n, up_node(e, depth + 1));
}

/* combineSchemas: true / missing is the identity, false absorbs, equal schemas collapse, else allOf (takes both) */
static json_t *up_combine(json_t *left, json_t *right) {
    if (!left || (json_is_bool(left) && left->u.b)) {
        json_free(left);
        return right;
    }
    if (json_is_bool(right) && right->u.b) {
        json_free(right);
        return left;
    }
    if ((json_is_bool(left) && !left->u.b) || (json_is_bool(right) && !right->u.b)) {
        json_free(left);
        json_free(right);
        return json_bool(false);
    }
    if (json_equal(left, right)) {
        json_free(right);
        return left;
    }
    json_t *all = json_arr();
    json_push(all, left);
    json_push(all, right);
    return obj1("allOf", all);
}

static bool up_null_type(const json_t *t) {
    if (str_eq(json_sv(t), "null")) return true;
    const json_t *e;
    json_arr_foreach((json_t *)t, i, e) if (str_eq(json_sv(e), "null")) return true;
    return false;
}

/* makeNullable */
static json_t *up_make_nullable(json_t *s) {
    json_t *t = json_get(s, "type");
    if (json_is_str(t)) {
        if (strcmp(json_sv(t), "null")) {
            json_t *a = json_arr();
            json_push_str(a, json_sv(t));
            json_push_str(a, "null");
            json_set(s, "type", a);
        }
        return s;
    }
    if (json_is_arr(t)) {
        if (!up_null_type(t)) json_push_str(t, "null");
        return s;
    }
    json_t *any = json_get(s, "anyOf");
    if (json_is_arr(any)) {
        bool has = false;
        const json_t *e;
        json_arr_foreach(any, i, e) has = has || (json_is_obj(e) && up_null_type(json_get(e, "type")));
        if (!has) json_push(any, null_type());
        return s;
    }
    json_t *a = json_arr();
    json_push(a, s);
    json_push(a, null_type());
    return obj1("anyOf", a);
}

static json_t *up_node(const json_t *v, int depth) {
    if (depth > TS_MAX_DEPTH) return json_clone(v);
    json_t *e;
    if (json_is_arr(v)) {
        json_t *out = json_arr();
        json_arr_foreach((json_t *)v, i, e) json_push(out, up_node(e, depth + 1));
        return out;
    }
    if (!json_is_obj(v)) return json_clone(v);
    json_t *result = json_obj();
    const char *k;
    json_obj_foreach((json_t *)v, i, k, e) {
        if (!strcmp(k, "definitions") || !strcmp(k, "$defs")) {
            if (json_is_obj(e)) up_merge_map(result, "$defs", e, depth);
            continue;
        }
        if (IN(k, UP_MAP_KEYS)) {
            if (!json_is_obj(e)) json_set(result, k, json_clone(e));
            else up_merge_map(result, k, e, depth);
            continue;
        }
        if (IN(k, UP_VALUE_KEYS)) {
            json_set(result, k, json_clone(e));
            continue;
        }
        if (!strcmp(k, "dependencies") || !strcmp(k, "additionalItems") || !strcmp(k, "nullable")) continue;
        if (!strcmp(k, "$schema")) {
            json_set(result, "$schema", json_is_str(e) && IN(json_sv(e), DRAFT07_URIS) ? json_str(DRAFT_2020_12_URI) : json_clone(e));
            continue;
        }
        if (!strcmp(k, "$ref") && json_is_str(e)) {
            json_set(result, "$ref", json_str_own(up_convert_ref(json_sv(e))));
            continue;
        }
        if (!strcmp(k, "items") && json_is_arr(e)) continue;
        json_set(result, k, up_node(e, depth + 1));
    }
    /* the draft-07 tuple: `items: [a, b]` (+ `additionalItems`) -> `prefixItems: [a, b]` (+ `items` for the rest) */
    const json_t *items = json_get(v, "items");
    if (json_is_arr(items)) {
        json_t *conv = up_node(items, depth + 1), *merged;
        json_t *pi = json_get(result, "prefixItems");
        if (!json_is_arr(pi)) {
            merged = conv;
        } else {
            merged = json_arr();
            size_t n = MAX(json_len(pi), json_len(conv));
            for (size_t x = 0; x < n; x++) {
                bool im = x < json_len(pi), ic = x < json_len(conv);
                if (im && ic) json_push(merged, up_combine(json_clone(json_at(pi, x)), json_clone(json_at(conv, x))));
                else json_push(merged, json_clone(im ? json_at(pi, x) : json_at(conv, x)));
            }
            json_free(conv);
        }
        json_set(result, "prefixItems", merged);
        const json_t *ai = json_get(v, "additionalItems");
        if (ai && !(json_is_bool(ai) && ai->u.b)) json_set(result, "items", up_node(ai, depth + 1));
        else json_del(result, "items");
    }
    /* draft-07 `dependencies`: arrays -> dependentRequired, schemas -> dependentSchemas */
    const json_t *deps = json_get(v, "dependencies");
    json_obj_foreach((json_t *)deps, i, k, e) {
        json_t *conv = up_node(e, depth + 1);
        if (json_is_arr(conv)) {
            json_t *dr = up_object_map(result, "dependentRequired");
            json_t *ex = json_get(dr, k), *x;
            if (!ex) {
                json_set(dr, k, conv);
                continue;
            }
            if (json_is_arr(ex)) json_arr_foreach(conv, j, x) push_unique(ex, x);
            json_free(conv);
        } else {
            json_t *ds = up_object_map(result, "dependentSchemas");
            json_t *cur = json_get(ds, k);
            json_set(ds, k, up_combine(cur ? json_clone(cur) : NULL, conv));
        }
    }
    const json_t *nl = json_get(v, "nullable");
    if (json_is_bool(nl) && nl->u.b) return up_make_nullable(result);
    return result;
}

json_t *ts_upgrade_2020_12(const json_t *schema) { return up_needs(schema, 0) ? up_node(schema, 0) : json_clone(schema); }

/* ---------- local `$ref` inlining (dereference.ts) ---------- */

static const char *const DR_MAP_KEYS[] = {"properties", "patternProperties", "dependencies", "dependentSchemas"};
static const char *const DR_ARRAY_KEYS[] = {"anyOf", "oneOf", "allOf", "prefixItems"};
static const char *const DR_VALUE_KEYS[] = {"items", "additionalItems", "contains", "contentSchema", "propertyNames", "if",
                                            "then", "else", "not", "additionalProperties", "unevaluatedItems", "unevaluatedProperties"};

static json_t *dr_node(const json_t *node, const json_t *root, strvec_t *visiting, int depth);

static json_t *dr_keyword(const char *key, const json_t *value, const json_t *root, strvec_t *visiting, int depth) {
    json_t *e;
    const char *n;
    if (IN(key, DR_MAP_KEYS) && json_is_obj(value)) {
        json_t *out = json_obj();
        json_obj_foreach((json_t *)value, i, n, e) json_set(out, n, dr_node(e, root, visiting, depth + 1));
        return out;
    }
    if ((IN(key, DR_ARRAY_KEYS) || !strcmp(key, "items")) && json_is_arr(value)) {
        json_t *out = json_arr();
        json_arr_foreach((json_t *)value, i, e) json_push(out, dr_node(e, root, visiting, depth + 1));
        return out;
    }
    if (IN(key, DR_VALUE_KEYS)) return dr_node(value, root, visiting, depth + 1);
    return json_clone(value);
}

/* resolveLocalRef: only `#/$defs/Name` and `#/definitions/Name` */
static const json_t *dr_resolve(const char *ref, const json_t *root) {
    strvec_t t = {0};
    const json_t *r = NULL;
    if (ts_ref_tokens(ref, &t) && t.n == 2 && (!strcmp(t.v[0], "$defs") || !strcmp(t.v[0], "definitions"))) {
        const json_t *defs = json_get(root, t.v[0]);
        if (json_is_obj(defs)) r = json_get(defs, t.v[1]);
    }
    strvec_free(&t);
    return json_is_obj(r) ? r : NULL;
}

static json_t *dr_node(const json_t *node, const json_t *root, strvec_t *visiting, int depth) {
    if (!json_is_obj(node) || depth > TS_MAX_DEPTH) return json_clone(node);
    const char *ref = json_get_str(node, "$ref"), *k;
    json_t *v;
    if (ref) {
        if (strvec_contains(visiting, ref)) return json_obj(); /* a cycle */
        const json_t *resolved = dr_resolve(ref, root);
        if (!resolved) return json_clone(node);
        strvec_push(visiting, ref);
        json_t *inlined = dr_node(resolved, root, visiting, depth + 1);
        free(visiting->v[--visiting->n]);
        bool siblings = false;
        json_obj_foreach((json_t *)node, i, k, v) siblings = siblings || (strcmp(k, "$ref") && strcmp(k, "$defs") && strcmp(k, "definitions"));
        if (!siblings || !json_is_obj(inlined)) return inlined;
        json_obj_foreach((json_t *)node, i, k, v) {
            if (!strcmp(k, "$ref") || !strcmp(k, "$defs") || !strcmp(k, "definitions")) continue;
            json_set(inlined, k, dr_keyword(k, v, root, visiting, depth));
        }
        return inlined;
    }
    json_t *result = json_obj();
    json_obj_foreach((json_t *)node, i, k, v) {
        if (!strcmp(k, "$defs") || !strcmp(k, "definitions")) continue;
        json_set(result, k, dr_keyword(k, v, root, visiting, depth));
    }
    return result;
}

json_t *ts_dereference(const json_t *schema) {
    if (!json_is_obj(schema) || (!json_get(schema, "$defs") && !json_get(schema, "definitions"))) return json_clone(schema);
    strvec_t visiting = {0};
    json_t *r = dr_node(schema, schema, &visiting, 0);
    strvec_free(&visiting);
    return r;
}

/* ---------- MCP input schemas (normalize.ts normalizeSchemaForMCP: the generic walk with MCP's options) ---------- */

static const char *const MN_MAP_KEYS[] = {"properties", "patternProperties", "dependencies", "dependentSchemas", "$defs",
                                          "definitions"};
static const char *const MN_VALUE_KEYS[] = {"items", "additionalItems", "unevaluatedItems", "not", "if",
                                            "then", "else", "contains", "propertyNames", "contentSchema"};
static const char *const MN_ARRAY_KEYS[] = {"anyOf", "oneOf", "allOf", "prefixItems"};

/* classifySchemaChild: 0 an instance payload, 1 a schema, 2 a map of schemas */
static int mn_classify(const char *key, const json_t *value, bool in_map) {
    if (in_map) return 1;
    const char *nk = !strcmp(key, "additional_properties") ? "additionalProperties"
                     : !strcmp(key, "any_of")              ? "anyOf"
                     : !strcmp(key, "prefix_items")        ? "prefixItems"
                     : !strcmp(key, "property_ordering")   ? "propertyOrdering"
                                                           : key;
    if (IN(nk, MN_MAP_KEYS)) return 2;
    if (IN(nk, MN_VALUE_KEYS) || IN(nk, MN_ARRAY_KEYS)) return 1;
    if ((!strcmp(nk, "additionalProperties") || !strcmp(nk, "unevaluatedProperties")) && json_is_obj(value)) return 1;
    return 0;
}

/* inferJsonSchemaTypeFromValue */
static const char *json_type_of(const json_t *v) {
    if (!v || v->type == JSON_NULL) return "null";
    switch (v->type) {
    case JSON_ARRAY: return "array";
    case JSON_STRING: return "string";
    case JSON_NUMBER: return "number";
    case JSON_BOOL: return "boolean";
    default: return "object";
    }
}

static json_t *mn_node(const json_t *v, bool in_map, int depth);

static json_t *mn_child(const char *key, const json_t *entry, bool in_map, int depth) {
    int kind = mn_classify(key, entry, in_map);
    return kind ? mn_node(entry, kind == 2, depth + 1) : json_clone(entry);
}

static json_t *mn_node(const json_t *v, bool in_map, int depth) {
    if (depth > TS_MAX_DEPTH) return json_clone(v);
    json_t *e;
    if (json_is_arr(v)) {
        json_t *out = json_arr();
        json_arr_foreach((json_t *)v, i, e) json_push(out, mn_node(e, in_map, depth + 1));
        return out;
    }
    if (!json_is_obj(v)) return json_clone(v);
    json_t *result = json_obj();
    const char *k;
    /* an anyOf / oneOf of bare consts becomes one enum, typed when every value shares a type */
    static const char *const COMBINERS[] = {"anyOf", "oneOf"};
    for (size_t c = 0; c < ARRAY_LEN(COMBINERS); c++) {
        const json_t *vars = json_get(v, COMBINERS[c]);
        if (!json_is_arr(vars) || !json_len(vars)) continue;
        bool all = true;
        json_arr_foreach((json_t *)vars, i, e) all = all && json_is_obj(e) && json_get(e, "const");
        if (!all) continue;
        json_t *en = json_arr();
        json_arr_foreach((json_t *)vars, i, e) push_unique(en, json_get(e, "const"));
        json_set(result, "enum", en);
        size_t explicit_n = 0;
        const char *first = NULL;
        bool same = true;
        json_arr_foreach((json_t *)vars, i, e) {
            const char *t = json_get_str(e, "type");
            if (!t) continue;
            if (!explicit_n) first = t;
            else if (strcmp(t, first)) same = false;
            explicit_n++;
        }
        if (explicit_n == json_len(vars) && same && first && *first) {
            json_set_str(result, "type", first);
        } else {
            strvec_t types = {0}, non_null = {0};
            bool with_null = false;
            json_arr_foreach(en, i, e) {
                const char *t = json_type_of(e);
                if (!strvec_contains(&types, t)) strvec_push(&types, t);
                if (!strcmp(t, "null")) with_null = true;
                else if (!strvec_contains(&non_null, t)) strvec_push(&non_null, t);
            }
            if (types.n == 1) json_set_str(result, "type", types.v[0]);
            else if (with_null && non_null.n == 1) json_set_str(result, "type", non_null.v[0]);
            strvec_free(&types);
            strvec_free(&non_null);
        }
        json_obj_foreach((json_t *)v, i, k, e) {
            if (!strcmp(k, COMBINERS[c]) || json_get(result, k)) continue;
            if ((!in_map && !strcmp(k, "$schema")) || !strcmp(k, "nullable")) continue;
            json_set(result, k, mn_child(k, e, in_map, depth));
        }
        return result;
    }
    const json_t *cv = NULL;
    json_obj_foreach((json_t *)v, i, k, e) {
        if ((!in_map && !strcmp(k, "$schema")) || !strcmp(k, "nullable")) continue;
        if (!strcmp(k, "const")) {
            cv = e;
            continue;
        }
        json_set(result, k, mn_child(k, e, in_map, depth));
    }
    if (cv) {
        json_t *en = json_get(result, "enum");
        json_t *ne = json_is_arr(en) ? json_clone(en) : json_arr();
        push_unique(ne, cv);
        json_set(result, "enum", ne);
        if (!json_truthy(json_get(result, "type"))) json_set_str(result, "type", json_type_of(cv));
    }
    return result;
}

json_t *ts_mcp_normalize(const json_t *input_schema) {
    json_t *up = ts_upgrade_2020_12(input_schema);
    json_t *dr = ts_dereference(up);
    json_t *r = mn_node(dr, false, 0);
    json_free(up);
    json_free(dr);
    return r;
}

/* ---------- the tool wire pass (wire.ts toolWireSchema for raw JSON Schema) ---------- */

static const char *const WIRE_VALUE_KEYS[] = {"additionalProperties", "unevaluatedProperties", "unevaluatedItems", "items",
                                              "contains", "propertyNames", "if", "then", "else", "not"};
static const char *const WIRE_MAP_KEYS[] = {"properties", "patternProperties", "$defs", "definitions"};
static const char *const WIRE_ARRAY_KEYS[] = {"anyOf", "oneOf", "allOf", "prefixItems"};
static const char *const DEFINING_SIBLINGS[] = {"$ref",     "additionalProperties", "allOf", "anyOf",       "const",
                                                "contains", "enum",                 "if",    "items",       "not",
                                                "oneOf",    "patternProperties",    "prefixItems", "properties", "propertyNames",
                                                "then",     "else",                 "unevaluatedItems", "unevaluatedProperties"};
static const char *const NULLABLE_SCALARS[] = {"string", "number", "integer", "boolean"};

static bool has_defining_sibling(const json_t *s) {
    const char *k;
    json_t *v;
    json_obj_foreach((json_t *)s, i, k, v) if (strcmp(k, "anyOf") && IN(k, DEFINING_SIBLINGS)) return true;
    return false;
}

/* rewriteNullableScalarAnyOf: `anyOf: [{type: T, ...}, {type: "null"}]` -> `type: [T, "null"]` (+ the scalar's keywords) */
static void rewrite_nullable_scalar_anyof(json_t *s) {
    if (has_defining_sibling(s)) return;
    json_t *vars = json_get(s, "anyOf"), *e, *scalar = NULL;
    if (!json_is_arr(vars) || json_len(vars) != 2) return;
    bool saw_null = false;
    json_arr_foreach(vars, i, e) {
        if (!json_is_obj(e)) return;
        if (str_eq(json_get_str(e, "type"), "null") && json_len(e) == 1) {
            if (saw_null) return;
            saw_null = true;
            continue;
        }
        if (!IN(json_get_str(e, "type"), NULLABLE_SCALARS) || scalar) return;
        scalar = e;
    }
    if (!saw_null || !scalar) return;
    json_t *sc = json_clone(scalar);
    json_del(s, "anyOf");
    const char *k;
    json_obj_foreach(sc, i, k, e) {
        if (!strcmp(k, "type") || !strcmp(k, "enum") || !strcmp(k, "const") || json_get(s, k)) continue;
        json_set(s, k, json_clone(e));
    }
    if (json_get(sc, "const")) {
        json_t *en = json_arr();
        json_push(en, json_clone(json_get(sc, "const")));
        json_push(en, json_null());
        json_set(s, "enum", en);
    } else if (json_is_arr(json_get(sc, "enum"))) {
        json_t *en = json_clone(json_get(sc, "enum"));
        bool has_null = false;
        json_arr_foreach(en, i, e) has_null = has_null || e->type == JSON_NULL;
        if (!has_null) json_push(en, json_null());
        json_set(s, "enum", en);
    }
    json_t *t = json_arr();
    json_push_str(t, json_get_str(sc, "type"));
    json_push_str(t, "null");
    json_set(s, "type", t);
    json_free(sc);
}

/* homogeneousEnumScalarType */
static const char *homogeneous_scalar(const json_t *values) {
    const char *inferred = NULL;
    const json_t *e;
    json_arr_foreach((json_t *)values, i, e) {
        const char *t = e->type == JSON_STRING ? "string" : e->type == JSON_BOOL ? "boolean" : e->type == JSON_NUMBER ? "number" : NULL;
        if (!t || (inferred && strcmp(inferred, t))) return NULL;
        inferred = t;
    }
    return inferred;
}

/* collapseConstUnionAnyOf: an anyOf of `{const[, description]}` branches of one scalar type -> `{type, enum}` */
static void collapse_const_union(json_t *s) {
    if (has_defining_sibling(s) || json_get(s, "type")) return;
    json_t *vars = json_get(s, "anyOf"), *e;
    if (!json_is_arr(vars) || json_len(vars) < 2) return;
    char *branch_desc = NULL;
    size_t described = 0;
    json_t *values = json_arr();
    json_arr_foreach(vars, i, e) {
        if (!json_is_obj(e) || !json_get(e, "const")) goto keep;
        const char *k;
        json_t *x;
        json_obj_foreach(e, j, k, x) if (strcmp(k, "const") && strcmp(k, "description")) goto keep;
        const char *d = json_get_str(e, "description");
        if (d) {
            if (!described) branch_desc = xstrdup(d);
            else if (strcmp(d, branch_desc)) goto keep;
            described++;
        }
        json_push(values, json_clone(json_get(e, "const")));
    }
    if (described && described != json_len(vars)) goto keep;
    if (described == json_len(vars) && json_is_str(json_get(s, "description")) && strcmp(json_get_str(s, "description"), branch_desc))
        goto keep;
    const char *t = homogeneous_scalar(values);
    if (!t) goto keep;
    json_del(s, "anyOf");
    json_set_str(s, "type", t);
    json_set(s, "enum", values);
    if (!json_is_str(json_get(s, "description")) && branch_desc) json_set_str(s, "description", branch_desc);
    free(branch_desc);
    return;
keep:
    json_free(values);
    free(branch_desc);
}

static void ws_walk(json_t *n, int depth) {
    if (depth > TS_MAX_DEPTH) return;
    json_t *e;
    if (json_is_arr(n)) {
        json_arr_foreach(n, i, e) ws_walk(e, depth + 1);
        return;
    }
    if (!json_is_obj(n)) return;
    rewrite_nullable_scalar_anyof(n);
    /* inferBareEnumScalarType: Gemini rejects an enum without a type */
    if (!json_get(n, "type") && json_is_arr(json_get(n, "enum"))) {
        const char *t = homogeneous_scalar(json_get(n, "enum"));
        if (t) json_set_str(n, "type", t);
    }
    collapse_const_union(n);
    const char *k;
    json_obj_foreach(n, i, k, e) ws_walk(e, depth + 1);
}

/* parseArkObjectKey: an ArkType key "name", whitespace, a doc-comment opener (slash star star), the description, then
 * the closer (star slash) and trailing whitespace; the name may not cross a line terminator (owned parts) */
static bool ark_key(const char *key, char **name, char **desc) {
    size_t n = strlen(key);
    for (const char *p = strstr(key, "/**"); p; p = strstr(p + 1, "/**")) {
        size_t at = (size_t)(p - key), end = at, k;
        /* key[0..at) is the name plus the whitespace before the opener; the name cannot hold a line terminator */
        while (end > 0) {
            size_t st = end - 1;
            while (st > 0 && ((unsigned char)key[st] & 0xC0) == 0x80 && end - st < 4) st--;
            k = js_ws(key + st, end - st);
            if (k != end - st || !k) break;
            end = st;
        }
        bool line_break = false;
        for (size_t q = 0; q < end; q++)
            line_break = line_break || key[q] == '\n' || key[q] == '\r' || !strncmp(key + q, "\xe2\x80\xa8", 3) ||
                         !strncmp(key + q, "\xe2\x80\xa9", 3);
        char *nm = js_trim(key, end);
        char *rest = js_trim(key + at + 3, n - at - 3);
        size_t rl = strlen(rest);
        /* the trailing whitespace trim also ate the space before `*\/`; the description is what precedes it */
        bool ends = rl >= 2 && rest[rl - 2] == '*' && rest[rl - 1] == '/';
        if (!line_break && ends) {
            *name = nm;
            *desc = js_trim(rest, rl - 2);
            free(rest);
            return true;
        }
        free(nm);
        free(rest);
    }
    return false;
}

/* withArkKeyDescription (takes `schema`) */
static json_t *ark_with_desc(json_t *schema, const char *desc) {
    if (json_is_obj(schema)) {
        if (!json_is_str(json_get(schema, "description"))) json_set_str(schema, "description", desc);
        return schema;
    }
    json_t *any = json_arr(), *w = json_obj();
    json_push(any, schema);
    json_set(w, "anyOf", any);
    json_set_str(w, "description", desc);
    return w;
}

static void ws_ark_comments(json_t *n, int depth) {
    if (depth > TS_MAX_DEPTH) return;
    json_t *e;
    if (json_is_arr(n)) {
        json_arr_foreach(n, i, e) ws_ark_comments(e, depth + 1);
        return;
    }
    if (!json_is_obj(n)) return;
    json_t *props = json_get(n, "properties");
    if (json_is_obj(props)) {
        json_t *req = json_get(n, "required");
        if (json_is_arr(req)) {
            json_t *nr = json_arr();
            json_arr_foreach(req, i, e) {
                char *nm = NULL, *d = NULL;
                if (json_is_str(e) && ark_key(json_sv(e), &nm, &d)) json_push(nr, json_str_own(nm));
                else json_push(nr, json_clone(e));
                free(d);
            }
            json_set(n, "required", nr);
        }
        strvec_t keys = {0};
        const char *k;
        json_obj_foreach(props, i, k, e) strvec_push(&keys, k);
        for (size_t i = 0; i < keys.n; i++) {
            char *nm = NULL, *d = NULL;
            json_t *ps = json_get(props, keys.v[i]);
            if (ark_key(keys.v[i], &nm, &d) && *d) {
                json_t *moved = ark_with_desc(json_take(props, keys.v[i]), d);
                json_set(props, nm, moved);
                ws_ark_comments(moved, depth + 1);
            } else if (ps) {
                ws_ark_comments(ps, depth + 1);
            }
            free(nm);
            free(d);
        }
        strvec_free(&keys);
    }
    for (size_t i = 0; i < ARRAY_LEN(WIRE_VALUE_KEYS); i++)
        if (json_get(n, WIRE_VALUE_KEYS[i])) ws_ark_comments(json_get(n, WIRE_VALUE_KEYS[i]), depth + 1);
    for (size_t m = 1; m < ARRAY_LEN(WIRE_MAP_KEYS); m++) { /* `properties` was walked above */
        json_t *map = json_get(n, WIRE_MAP_KEYS[m]);
        const char *k;
        json_obj_foreach(map, i, k, e) ws_ark_comments(e, depth + 1);
    }
    for (size_t a = 0; a < ARRAY_LEN(WIRE_ARRAY_KEYS); a++) {
        json_t *arr = json_get(n, WIRE_ARRAY_KEYS[a]);
        json_arr_foreach(arr, i, e) ws_ark_comments(e, depth + 1);
    }
}

/* normalizeEmptySchemas: `{}` -> `true` in every schema-valued position */
static void ws_empty(json_t *n, int depth) {
    if (depth > TS_MAX_DEPTH) return;
    json_t *e;
    if (json_is_arr(n)) {
        json_arr_foreach(n, i, e) ws_empty(e, depth + 1);
        return;
    }
    if (!json_is_obj(n)) return;
    for (size_t i = 0; i < ARRAY_LEN(WIRE_VALUE_KEYS); i++)
        if (is_empty_obj(json_get(n, WIRE_VALUE_KEYS[i]))) json_set(n, WIRE_VALUE_KEYS[i], json_bool(true));
    for (size_t m = 0; m < ARRAY_LEN(WIRE_MAP_KEYS); m++) {
        json_t *map = json_get(n, WIRE_MAP_KEYS[m]);
        for (size_t i = 0; json_is_obj(map) && i < map->u.obj.n; i++)
            if (is_empty_obj(map->u.obj.vals[i])) {
                json_free(map->u.obj.vals[i]);
                map->u.obj.vals[i] = json_bool(true);
            }
    }
    for (size_t a = 0; a < ARRAY_LEN(WIRE_ARRAY_KEYS); a++) {
        json_t *arr = json_get(n, WIRE_ARRAY_KEYS[a]);
        for (size_t i = 0; json_is_arr(arr) && i < arr->u.arr.n; i++)
            if (is_empty_obj(arr->u.arr.items[i])) {
                json_free(arr->u.arr.items[i]);
                arr->u.arr.items[i] = json_bool(true);
            }
    }
    const char *k;
    json_obj_foreach(n, i, k, e) ws_empty(e, depth + 1);
}

json_t *ts_wire_schema(const json_t *params) {
    json_t *s = ts_upgrade_2020_12(params);
    ws_walk(s, 0);
    ws_ark_comments(s, 0);
    ws_empty(s, 0);
    return s;
}

/* ---------- OpenAI strict mode (normalize.ts tryEnforceStrictSchema) ---------- */

static const char *const NON_STRUCTURAL[] = {
    "format",       "pattern",       "minLength",   "maxLength",        "minimum",          "maximum",
    "exclusiveMinimum", "exclusiveMaximum", "minItems", "maxItems",     "uniqueItems",      "multipleOf",
    "$schema",      "examples",      "default",     "title",            "$comment",         "if",
    "then",         "else",          "not",         "unevaluatedProperties", "unevaluatedItems", "patternProperties",
    "propertyNames", "contains",     "minContains", "maxContains",      "dependentRequired", "dependentSchemas",
    "contentEncoding", "contentMediaType", "contentSchema", "deprecated", "readOnly",       "writeOnly",
    "minProperties", "maxProperties", "$dynamicRef", "$dynamicAnchor"};
static const char *const COMBINATORS[] = {"anyOf", "allOf", "oneOf"};

static bool declares_object(const json_t *type) {
    if (str_eq(json_sv(type), "object")) return true;
    const json_t *e;
    json_arr_foreach((json_t *)type, i, e) if (str_eq(json_sv(e), "object")) return true;
    return false;
}

static bool any_combinator(const json_t *s) {
    for (size_t i = 0; i < ARRAY_LEN(COMBINATORS); i++)
        if (json_is_arr(json_get(s, COMBINATORS[i]))) return true;
    return false;
}

/* isBareOpenObject: an object node with no properties, additionalProperties, $ref or combinator supplying its shape */
static bool bare_open_object(const json_t *s) {
    return declares_object(json_get(s, "type")) && !json_is_obj(json_get(s, "properties")) && !json_get(s, "additionalProperties") &&
           !json_get(s, "$ref") && !any_combinator(s);
}

/* isUnrepresentableStrictBranch: a boolean schema or `{}` */
static bool unrep_branch(const json_t *v) { return json_is_bool(v) || is_empty_obj(v); }

static bool unrep_list(const json_t *arr, int depth);

/* hasUnrepresentableStrictObjectMap: open keysets (patternProperties, additionalProperties true or a schema, bare
 * objects) and unconstrained branches anywhere make the schema non-strict */
static bool unrepresentable(const json_t *s, int depth) {
    if (depth > TS_MAX_DEPTH) return true;
    const json_t *pp = json_get(s, "patternProperties"), *ap = json_get(s, "additionalProperties"), *e;
    if (json_is_obj(pp) && json_len(pp) > 0) return true;
    if ((json_is_bool(ap) && ap->u.b) || json_is_obj(ap)) return true;
    if (bare_open_object(s)) return true;
    const char *k;
    json_obj_foreach(json_get(s, "properties"), i, k, e) if (unrep_branch(e) || (json_is_obj(e) && unrepresentable(e, depth + 1))) return true;
    const json_t *items = json_get(s, "items");
    if (unrep_branch(items)) return true;
    if (json_is_obj(items) ? unrepresentable(items, depth + 1) : unrep_list(items, depth)) return true;
    if (unrep_list(json_get(s, "prefixItems"), depth)) return true;
    for (size_t c = 0; c < ARRAY_LEN(COMBINATORS); c++)
        if (unrep_list(json_get(s, COMBINATORS[c]), depth)) return true;
    static const char *const DEFS[] = {"$defs", "definitions"};
    for (size_t d = 0; d < ARRAY_LEN(DEFS); d++)
        json_obj_foreach(json_get(s, DEFS[d]), i, k, e) if (unrep_branch(e) || (json_is_obj(e) && unrepresentable(e, depth + 1))) return true;
    return false;
}

static bool unrep_list(const json_t *arr, int depth) {
    const json_t *e;
    json_arr_foreach((json_t *)arr, i, e) if (unrep_branch(e) || (json_is_obj(e) && unrepresentable(e, depth + 1))) return true;
    return false;
}

/* inferStrictPrimitiveTypeFromEnumOrConst: the one primitive type of every enum (or const) value, else NULL */
static const char *strict_primitive(const json_t *node) {
    const json_t *en = json_get(node, "enum"), *cv = json_get(node, "const");
    if (!json_is_arr(en) && !cv) return NULL;
    const char *inferred = NULL;
    size_t n = json_is_arr(en) ? json_len(en) : 1;
    if (!n) return NULL;
    for (size_t i = 0; i < n; i++) {
        const json_t *v = json_is_arr(en) ? json_at(en, i) : cv;
        const char *t = v->type == JSON_NULL     ? "null"
                        : v->type == JSON_STRING ? "string"
                        : v->type == JSON_NUMBER ? "number"
                        : v->type == JSON_BOOL   ? "boolean"
                                                 : NULL;
        if (!t || (inferred && strcmp(inferred, t))) return NULL;
        inferred = t;
    }
    return inferred;
}

static bool type_accepts(const char *type, const json_t *v) {
    if (!strcmp(type, "null")) return v->type == JSON_NULL;
    if (!strcmp(type, "string")) return v->type == JSON_STRING;
    if (!strcmp(type, "number")) return v->type == JSON_NUMBER;
    if (!strcmp(type, "integer")) return v->type == JSON_NUMBER && isfinite(v->u.num.d) && floor(v->u.num.d) == v->u.num.d;
    if (!strcmp(type, "boolean")) return v->type == JSON_BOOL;
    if (!strcmp(type, "array")) return v->type == JSON_ARRAY;
    if (!strcmp(type, "object")) return v->type == JSON_OBJECT;
    return true;
}

/* narrowEnumToType: false when no enum value fits the variant's type */
static bool narrow_enum(json_t *s, const char *type) {
    json_t *en = json_get(s, "enum"), *e;
    if (!json_is_arr(en)) return true;
    json_t *nar = json_arr();
    json_arr_foreach(en, i, e) if (type_accepts(type, e)) json_push(nar, json_clone(e));
    if (!json_len(nar)) {
        json_free(nar);
        return false;
    }
    if (json_len(nar) != json_len(en)) json_set(s, "enum", nar);
    else json_free(nar);
    return true;
}

/* an object's entries as borrowed key/value pointers: omp's `{...a, ...b}` spreads, sharing the child objects */
typedef struct {
    size_t n, cap;
    const char **k;
    const json_t **v;
} sview_t;

static void sv_set(sview_t *s, const char *k, const json_t *v) {
    for (size_t i = 0; i < s->n; i++)
        if (!strcmp(s->k[i], k)) {
            s->v[i] = v;
            return;
        }
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->k = xrealloc(s->k, s->cap * sizeof *s->k);
        s->v = xrealloc(s->v, s->cap * sizeof *s->v);
    }
    s->k[s->n] = k;
    s->v[s->n++] = v;
}

static void sv_from(sview_t *s, const json_t *o) {
    memset(s, 0, sizeof *s);
    const char *k;
    json_t *v;
    json_obj_foreach((json_t *)o, i, k, v) sv_set(s, k, v);
}

static void sv_copy(sview_t *dst, const sview_t *src) {
    memset(dst, 0, sizeof *dst);
    for (size_t i = 0; i < src->n; i++) sv_set(dst, src->k[i], src->v[i]);
}

static const json_t *sv_get(const sview_t *s, const char *k) {
    for (size_t i = 0; i < s->n; i++)
        if (!strcmp(s->k[i], k)) return s->v[i];
    return NULL;
}

static void sv_del(sview_t *s, const char *k) {
    for (size_t i = 0; i < s->n; i++)
        if (!strcmp(s->k[i], k)) {
            memmove(s->k + i, s->k + i + 1, (s->n - i - 1) * sizeof *s->k);
            memmove(s->v + i, s->v + i + 1, (s->n - i - 1) * sizeof *s->v);
            s->n--;
            return;
        }
}

static void sv_free(sview_t *s) {
    free(s->k);
    free(s->v);
}

/* sanitizeSchemaForStrictMode state: omp memoizes by node identity (a WeakMap seeded before a scalar node's children
 * are walked) and cuts revisits with an epoch mark, so both live here keyed by node address. A node revisited while its
 * own scalar walk is in progress would put that node inside itself: omp's enforcement then refuses the circular graph,
 * so `cyclic` makes the whole schema non-strict. Fresh nodes walked by identity are kept alive until the end, so an
 * address is never reused within one run. */
typedef struct {
    const json_t *root;
    hmap_t seen, cache;
    VEC(json_t *) keep;
    bool cyclic, failed;
    int depth;
} sctx_t;

static char g_busy_mark;
#define SC_BUSY ((void *)&g_busy_mark)

static void pkey(char buf[32], const json_t *p) { snprintf(buf, 32, "%p", (const void *)p); }

static void free_cached(void *v) {
    if (v && v != SC_BUSY) json_free(v);
}

static void st_cache_done(sctx_t *c, const json_t *id, const json_t *val) {
    if (!id) return;
    char key[32];
    pkey(key, id);
    free_cached(hmap_put(&c->cache, key, json_clone(val)));
}

/* resolveStrictRef: a URI-fragment JSON Pointer walked through objects from the root */
static const json_t *st_resolve_ref(const json_t *root, const char *ref) {
    strvec_t t = {0};
    const json_t *cur = NULL;
    if (ts_ref_tokens(ref, &t) && t.n) {
        cur = root;
        for (size_t i = 0; i < t.n && cur; i++) cur = json_is_obj(cur) ? json_get(cur, t.v[i]) : NULL;
    }
    strvec_free(&t);
    return json_is_obj(cur) ? cur : NULL;
}

static json_t *st_view(sctx_t *c, const json_t *id, sview_t *v);

static json_t *st_node(sctx_t *c, const json_t *s) {
    char key[32];
    pkey(key, s);
    void *hit = hmap_get(&c->cache, key);
    if (hit == SC_BUSY) {
        c->cyclic = true;
        return json_obj();
    }
    if (hit) return json_clone(hit);
    if (hmap_has(&c->seen, key)) return json_obj(); /* once(): revisited while it is still being walked */
    hmap_put(&c->seen, key, (void *)1);
    sview_t v;
    sv_from(&v, s);
    json_t *r = st_view(c, s, &v);
    sv_free(&v);
    return r;
}

static json_t *st_child(sctx_t *c, const json_t *x) { return json_is_obj(x) ? st_node(c, x) : json_clone(x); }

static json_t *st_children(sctx_t *c, const json_t *arr) {
    json_t *out = json_arr(), *e;
    json_arr_foreach((json_t *)arr, i, e) json_push(out, st_child(c, e));
    return out;
}

static json_t *st_view_body(sctx_t *c, const json_t *id, sview_t *v) {
    /* `$ref` with sibling keys: the resolved def inlined, the siblings winning (openai-python _ensure_strict_json_schema) */
    const json_t *ref = sv_get(v, "$ref");
    if (json_is_str(ref) && v->n > 1) {
        const json_t *resolved = st_resolve_ref(c->root, json_sv(ref));
        if (resolved) {
            sview_t m;
            sv_from(&m, resolved);
            for (size_t i = 0; i < v->n; i++)
                if (strcmp(v->k[i], "$ref")) sv_set(&m, v->k[i], v->v[i]);
            json_t *r = st_view(c, NULL, &m);
            sv_free(&m);
            st_cache_done(c, id, r);
            return r;
        }
    }
    /* a single-entry `allOf` is inlined, its keys winning over the siblings */
    const json_t *all_of = sv_get(v, "allOf");
    if (json_is_arr(all_of) && json_len(all_of) == 1 && json_is_obj(json_at(all_of, 0))) {
        sview_t m;
        sv_copy(&m, v);
        sv_del(&m, "allOf");
        const char *k;
        json_t *x;
        json_obj_foreach(json_at(all_of, 0), i, k, x) sv_set(&m, k, x);
        json_t *r = st_view(c, NULL, &m);
        sv_free(&m);
        st_cache_done(c, id, r);
        return r;
    }
    const json_t *type = sv_get(v, "type");
    if (json_is_arr(type)) {
        /* `type: [a, b]` -> one variant per type, each keeping only the keywords its type can use */
        sview_t wt;
        sv_copy(&wt, v);
        sv_del(&wt, "type");
        json_t *sw = st_view(c, NULL, &wt);
        sv_free(&wt);
        json_t *variants = json_arr(), *e;
        size_t typed = 0;
        json_arr_foreach((json_t *)type, i, e) {
            if (!json_is_str(e)) continue;
            typed++;
            const char *vt = json_sv(e);
            json_t *vs = json_clone(sw);
            json_del(vs, "description");
            json_set_str(vs, "type", vt);
            if (strcmp(vt, "object")) {
                json_del(vs, "properties");
                json_del(vs, "required");
                json_del(vs, "additionalProperties");
            }
            if (strcmp(vt, "array")) json_del(vs, "items");
            if (!narrow_enum(vs, vt)) {
                json_free(vs);
                continue;
            }
            vec_push(&c->keep, vs);
            json_push(variants, st_node(c, vs));
        }
        if (!typed || !json_len(variants)) {
            json_free(variants);
            st_cache_done(c, id, sw);
            return sw;
        }
        const json_t *desc = json_get(sw, "description");
        json_t *res;
        if (json_len(variants) == 1) {
            res = json_arr_remove(variants, 0);
            json_free(variants);
            if (desc && !json_get(res, "description")) json_set(res, "description", json_clone(desc));
        } else {
            res = obj1("anyOf", variants);
            if (desc) json_set(res, "description", json_clone(desc));
        }
        json_free(sw);
        st_cache_done(c, id, res);
        return res;
    }
    if (id) {
        char key[32];
        pkey(key, id);
        free_cached(hmap_put(&c->cache, key, SC_BUSY));
    }
    json_t *out = json_obj();
    const json_t *dflt = sv_get(v, "default");
    for (size_t i = 0; i < v->n; i++) {
        const char *k = v->k[i];
        const json_t *x = v->v[i];
        if (IN(k, NON_STRUCTURAL) || !strcmp(k, "type") || !strcmp(k, "const") || !strcmp(k, "nullable")) continue;
        if (!strcmp(k, "properties") && json_is_obj(x)) {
            json_t *p = json_obj(), *ps;
            const char *name;
            json_obj_foreach((json_t *)x, j, name, ps) json_set(p, name, st_child(c, ps));
            json_set(out, "properties", p);
            continue;
        }
        if (!strcmp(k, "items")) {
            json_set(out, "items", json_is_obj(x) ? st_node(c, x) : json_is_arr(x) ? st_children(c, x) : json_clone(x));
            continue;
        }
        if ((!strcmp(k, "prefixItems") || IN(k, COMBINATORS)) && json_is_arr(x)) {
            json_set(out, k, st_children(c, x));
            continue;
        }
        if ((!strcmp(k, "$defs") || !strcmp(k, "definitions")) && json_is_obj(x)) {
            json_t *d = json_obj(), *ds;
            const char *name;
            json_obj_foreach((json_t *)x, j, name, ds) json_set(d, name, st_child(c, ds));
            json_set(out, k, d);
            continue;
        }
        if (!strcmp(k, "additionalProperties")) continue; /* enforcement closes every object */
        if (!strcmp(k, "description") && json_is_str(x) && dflt) {
            /* the stripped `default` survives as "(default: X)" in the description */
            char *fmt = json_is_str(dflt) ? xstrdup(json_sv(dflt)) : json_dump_js(dflt);
            json_set(out, "description",
                     json_str_own(strstr(json_sv(x), "(default:") ? xstrdup(json_sv(x)) : xasprintf("%s (default: %s)", json_sv(x), fmt)));
            free(fmt);
            continue;
        }
        json_set(out, k, json_clone(x));
    }
    const json_t *cv = sv_get(v, "const");
    if (cv) { /* const -> a single-entry enum */
        json_t *en = json_get(out, "enum");
        json_t *ne = json_is_arr(en) ? json_clone(en) : json_arr();
        push_unique(ne, cv);
        json_set(out, "enum", ne);
    }
    if (json_is_str(type)) json_set_str(out, "type", json_sv(type));
    if (!json_get(out, "type") && json_is_obj(json_get(out, "properties"))) json_set_str(out, "type", "object");
    if (!json_get(out, "type") && (json_get(out, "items") || json_get(out, "prefixItems"))) json_set_str(out, "type", "array");
    if (!json_get(out, "type")) {
        const char *t = strict_primitive(out);
        if (t) json_set_str(out, "type", t);
    }
    st_cache_done(c, id, out); /* the memo holds the node itself, not the nullable wrapper */
    const json_t *nl = sv_get(v, "nullable");
    if (json_is_bool(nl) && nl->u.b) {
        json_t *d = json_take(out, "description");
        json_t *any = json_arr();
        json_push(any, out);
        json_push(any, null_type());
        json_t *w = obj1("anyOf", any);
        if (d) json_set(w, "description", d);
        return w;
    }
    return out;
}

static json_t *st_view(sctx_t *c, const json_t *id, sview_t *v) {
    if (++c->depth > TS_MAX_DEPTH) { /* a self-referential `$ref` chain: omp overflows its stack and falls back */
        c->failed = true;
        c->depth--;
        return json_obj();
    }
    json_t *r = st_view_body(c, id, v);
    c->depth--;
    return r;
}

/* isPureAnyOfNode: only `anyOf` (and a description) constrain the node */
static bool pure_anyof(const json_t *v) {
    if (!json_is_obj(v) || !json_is_arr(json_get(v, "anyOf"))) return false;
    const char *k;
    json_t *x;
    json_obj_foreach((json_t *)v, i, k, x) if (strcmp(k, "anyOf") && strcmp(k, "description")) return false;
    return true;
}

static bool has_null_branch(const json_t *any) {
    const json_t *e;
    json_arr_foreach((json_t *)any, i, e) if (json_is_obj(e) && str_eq(json_get_str(e, "type"), "null")) return true;
    return false;
}

static json_t *st_enforce(sctx_t *c, const json_t *s, int depth);

static void st_enforce_list(sctx_t *c, json_t *arr, int depth) {
    for (size_t i = 0; json_is_arr(arr) && i < arr->u.arr.n; i++)
        if (json_is_obj(arr->u.arr.items[i])) {
            json_t *r = st_enforce(c, arr->u.arr.items[i], depth + 1);
            json_free(arr->u.arr.items[i]);
            arr->u.arr.items[i] = r;
        }
}

/* enforceStrictSchema: every object closed with all its keys required (optionals nullable); a node strict mode cannot
 * type sets `failed` (omp throws, and tryEnforceStrictSchema falls back) */
static json_t *st_enforce(sctx_t *c, const json_t *s, int depth) {
    json_t *r = json_clone(s), *e;
    if (depth > TS_MAX_DEPTH) {
        c->failed = true;
        return r;
    }
    if (str_eq(json_get_str(r, "type"), "object")) {
        json_set_bool(r, "additionalProperties", false);
        json_t *props = json_get_obj(r, "properties");
        strvec_t req = {0};
        json_arr_foreach(json_get(r, "required"), i, e) if (json_is_str(e)) strvec_push(&req, json_sv(e));
        json_t *sp = json_obj();
        const char *k;
        json_obj_foreach(props, i, k, e) {
            json_t *p = json_is_obj(e) ? st_enforce(c, e, depth + 1) : json_clone(e);
            if (strvec_contains(&req, k)) {
                json_set(sp, k, p);
                continue;
            }
            /* optional: nullable, so the model sends null instead of omitting the key */
            if (json_is_obj(p) && json_is_arr(json_get(p, "anyOf")) && has_null_branch(json_get(p, "anyOf"))) {
                json_set(sp, k, p);
            } else if (pure_anyof(p)) {
                json_push(json_get(p, "anyOf"), null_type());
                json_set(sp, k, p);
            } else {
                json_t *d = json_is_obj(p) && json_is_str(json_get(p, "description")) ? json_take(p, "description") : NULL;
                json_t *any = json_arr();
                json_push(any, p);
                json_push(any, null_type());
                json_t *w = obj1("anyOf", any);
                if (d) json_set(w, "description", d);
                json_set(sp, k, w);
            }
        }
        strvec_free(&req);
        json_t *keys = json_arr();
        json_obj_foreach(sp, i, k, e) json_push_str(keys, k);
        json_set(r, "properties", sp);
        json_set(r, "required", keys);
    }
    json_t *items = json_get(r, "items");
    if (json_is_arr(items)) st_enforce_list(c, items, depth);
    else if (json_is_obj(items)) json_set(r, "items", st_enforce(c, items, depth + 1));
    st_enforce_list(c, json_get(r, "prefixItems"), depth);
    for (size_t i = 0; i < ARRAY_LEN(COMBINATORS); i++) st_enforce_list(c, json_get(r, COMBINATORS[i]), depth);
    /* nested pure unions splice into the parent anyOf: (A | B) | C = A | B | C */
    json_t *any = json_get(r, "anyOf");
    bool nested = false;
    json_arr_foreach(any, i, e) nested = nested || pure_anyof(e);
    if (nested) {
        json_t *flat = json_arr(), *x;
        json_arr_foreach(any, i, e) {
            if (!pure_anyof(e)) {
                json_push(flat, json_clone(e));
                continue;
            }
            json_arr_foreach(json_get(e, "anyOf"), j, x) json_push(flat, json_clone(x));
            if (json_is_str(json_get(e, "description")) && !json_get(r, "description"))
                json_set_str(r, "description", json_get_str(e, "description"));
        }
        json_set(r, "anyOf", flat);
    }
    static const char *const DEFS[] = {"$defs", "definitions"};
    for (size_t d = 0; d < ARRAY_LEN(DEFS); d++) {
        json_t *defs = json_get_obj(r, DEFS[d]);
        for (size_t i = 0; defs && i < defs->u.obj.n; i++)
            if (json_is_obj(defs->u.obj.vals[i])) {
                json_t *x = st_enforce(c, defs->u.obj.vals[i], depth + 1);
                json_free(defs->u.obj.vals[i]);
                defs->u.obj.vals[i] = x;
            }
    }
    if (!json_get(r, "type")) {
        const char *t = strict_primitive(r);
        if (t) json_set_str(r, "type", t);
    }
    if (!json_get(r, "type") && !json_get(r, "$ref") && !any_combinator(r) && !json_is_obj(json_get(r, "not"))) c->failed = true;
    return r;
}

json_t *ts_try_enforce_strict(const json_t *schema, bool *strict) {
    json_t *up = ts_upgrade_2020_12(schema);
    *strict = false;
    if (!json_is_obj(up) || unrepresentable(up, 0)) return up;
    sctx_t c = {.root = up};
    hmap_init(&c.seen);
    hmap_init(&c.cache);
    json_t *san = st_node(&c, up), *res = NULL;
    if (!c.failed && !c.cyclic) {
        res = st_enforce(&c, san, 0);
        if (c.failed) {
            json_free(res);
            res = NULL;
        }
    }
    json_free(san);
    hmap_free(&c.seen, NULL);
    hmap_free(&c.cache, free_cached);
    for (size_t i = 0; i < c.keep.n; i++) json_free(c.keep.v[i]);
    vec_free(&c.keep);
    if (!res) return up;
    json_free(up);
    *strict = true;
    return res;
}

json_t *ts_adapt_for_strict(const json_t *schema, bool strict, bool *effective) {
    if (!strict) {
        *effective = false;
        return ts_upgrade_2020_12(schema);
    }
    return ts_try_enforce_strict(schema, effective);
}
