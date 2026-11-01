/*
 * google_schema.c - JSON Schema normalization for the Google wires.
 *   google_normalize_schema      normalizeSchemaForGoogle (Gemini `parametersJsonSchema`)
 *   google_normalize_schema_cca  normalizeSchemaForCCA (Claude-on-Cloud-Code-Assist legacy `parameters`)
 * Port of packages/ai/src/utils/schema/{normalize,fields,equality}.ts.
 */
#include "ai/provider_google.h"

#define SCHEMA_MAX_DEPTH 64
/* ======================================================================
 * schema normalization (normalizeSchemaForGoogle)
 * ====================================================================== */

static const char *const UNSUPPORTED[] = {
    "$schema", "$id", "$anchor", "$ref", "$defs", "$dynamicRef", "$dynamicAnchor", "examples", "prefixItems",
    "unevaluatedProperties", "unevaluatedItems", "patternProperties", "additionalProperties", "propertyNames",
    "minItems", "maxItems", "uniqueItems", "contains", "minContains", "maxContains", "additionalItems", "minLength",
    "maxLength", "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "multipleOf", "pattern", "format",
    "contentEncoding", "contentMediaType", "contentSchema", "dependencies", "dependentSchemas", "dependentRequired",
    "x-mcp-header", "deprecated", "readOnly", "writeOnly", "$comment", "definitions"};
static const char *const LIFTABLE[] = {"pattern", "format", "minLength", "maxLength", "minimum", "maximum",
                                       "exclusiveMinimum", "exclusiveMaximum", "multipleOf", "minItems", "maxItems",
                                       "uniqueItems", "minProperties", "maxProperties", "default", "examples"};
static const char *const MAP_KEYS[] = {"properties", "patternProperties", "dependencies", "dependentSchemas", "$defs",
                                       "definitions"};
static const char *const VALUE_KEYS[] = {"items", "additionalItems", "unevaluatedItems", "not", "if", "then", "else",
                                         "contains", "propertyNames", "contentSchema"};
static const char *const ARRAY_KEYS[] = {"anyOf", "oneOf", "allOf", "prefixItems"};
static const char *const TS_ARRAY[] = {"items", "prefixItems", "contains", "minContains", "maxContains", "minItems",
                                       "maxItems", "uniqueItems", "unevaluatedItems"};
static const char *const TS_OBJECT[] = {"properties", "required", "additionalProperties", "patternProperties",
                                        "propertyNames", "minProperties", "maxProperties", "dependentRequired",
                                        "dependentSchemas", "unevaluatedProperties"};
static const char *const TS_STRING[] = {"minLength", "maxLength", "pattern", "format", "contentEncoding", "contentMediaType"};
static const char *const TS_NUMBER[] = {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "multipleOf"};

static bool in_list(const char *k, const char *const *l, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(k, l[i])) return true;
    return false;
}
#define IN(k, L) in_list((k), (L), ARRAY_LEN(L))

static bool type_specific_for(const char *type, const char *k) {
    if (!type) return false;
    if (!strcmp(type, "array")) return IN(k, TS_ARRAY);
    if (!strcmp(type, "object")) return IN(k, TS_OBJECT);
    if (!strcmp(type, "string")) return IN(k, TS_STRING);
    if (!strcmp(type, "number") || !strcmp(type, "integer")) return IN(k, TS_NUMBER);
    return false;
}

static bool any_type_specific(const char *k) { return IN(k, TS_ARRAY) || IN(k, TS_OBJECT) || IN(k, TS_STRING) || IN(k, TS_NUMBER); }

static const char *snake_rename(const char *k) {
    if (!strcmp(k, "additional_properties")) return "additionalProperties";
    if (!strcmp(k, "any_of")) return "anyOf";
    if (!strcmp(k, "prefix_items")) return "prefixItems";
    if (!strcmp(k, "property_ordering")) return "propertyOrdering";
    return k;
}

/* 0 none, 1 schema, 2 map */
static int child_kind(const char *k, const json_t *v, bool in_map) {
    if (in_map) return 1;
    if (IN(k, MAP_KEYS)) return 2;
    if (IN(k, VALUE_KEYS) || IN(k, ARRAY_KEYS)) return 1;
    if ((!strcmp(k, "additionalProperties") || !strcmp(k, "unevaluatedProperties")) && json_is_obj(v)) return 1;
    return 0;
}

static const char *infer_type(const json_t *v) {
    if (!v) return NULL;
    switch (v->type) {
    case JSON_NULL: return "null";
    case JSON_ARRAY: return "array";
    case JSON_STRING: return "string";
    case JSON_NUMBER: return "number";
    case JSON_BOOL: return "boolean";
    case JSON_OBJECT: return "object";
    }
    return NULL;
}

static void push_enum(json_t *arr, const json_t *v) {
    json_t *it;
    json_arr_foreach(arr, i, it) {
        if (json_equal(it, v)) return;
    }
    json_push(arr, json_clone(v));
}

/* $ref "#/$defs/X" | "#/definitions/X" inlining with a depth cap */
static json_t *deref(const json_t *node, const json_t *root, int depth) {
    if (depth > SCHEMA_MAX_DEPTH) return json_obj();
    if (json_is_arr(node)) {
        json_t *o = json_arr(), *it;
        json_arr_foreach(node, i, it) { json_push(o, deref(it, root, depth + 1)); }
        return o;
    }
    if (!json_is_obj(node)) return json_clone(node);
    const char *ref = json_get_str(node, "$ref");
    if (ref) {
        const char *name = NULL;
        json_t *defs = NULL;
        if (str_starts(ref, "#/$defs/")) {
            defs = json_get_obj(root, "$defs");
            name = ref + 8;
        } else if (str_starts(ref, "#/definitions/")) {
            defs = json_get_obj(root, "definitions");
            name = ref + 14;
        }
        json_t *target = defs && name ? json_get(defs, name) : NULL;
        if (target) {
            json_t *res = deref(target, root, depth + 1);
            if (json_is_obj(res)) {
                const char *k;
                json_t *v;
                json_obj_foreach(node, i, k, v) {
                    if (strcmp(k, "$ref") != 0 && !json_get(res, k)) json_set(res, k, deref(v, root, depth + 1));
                }
            }
            return res;
        }
    }
    json_t *o = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(node, i, k, v) { json_set(o, k, deref(v, root, depth + 1)); }
    return o;
}

static json_t *gnorm(const json_t *v, bool in_map, bool bool_is_schema, int depth);

static void spill_to_description(json_t *node, json_t *spill) {
    if (json_len(spill) == 0) return;
    sb_t b = SB_INIT;
    const char *existing = json_get_str(node, "description");
    if (existing && *existing) {
        sb_append(&b, existing);
        sb_append(&b, "\n\n");
    }
    sb_putc(&b, '{');
    const char *k;
    json_t *v;
    json_obj_foreach(spill, i, k, v) {
        if (i) sb_append(&b, ", ");
        sb_append(&b, k);
        sb_append(&b, ": ");
        json_dump_sb(&b, v, 0);
    }
    sb_putc(&b, '}');
    json_set(node, "description", json_str_own(sb_detach(&b)));
}

static json_t *norm_child(const char *k, const json_t *v, bool in_map, int depth) {
    int kind = child_kind(k, v, in_map);
    if (!kind) return json_clone(v);
    return gnorm(v, kind == 2, kind == 1, depth + 1);
}

static void post_process(json_t *res, bool bool_is_schema) {
    json_t *en = json_get(res, "enum");
    if (bool_is_schema && json_is_arr(en)) {
        bool all_str = json_len(en) > 0;
        json_t *it;
        json_arr_foreach(en, i, it) {
            if (!json_is_str(it)) all_str = false;
        }
        if (!all_str) json_del(res, "enum");
    }
}

static json_t *gnorm(const json_t *v, bool in_map, bool bool_is_schema, int depth) {
    if (depth > SCHEMA_MAX_DEPTH) return json_obj();
    if (json_is_arr(v)) {
        json_t *o = json_arr(), *it;
        json_arr_foreach(v, i, it) { json_push(o, gnorm(it, false, bool_is_schema, depth + 1)); }
        return o;
    }
    if (json_is_bool(v)) {
        if (!bool_is_schema) return json_clone(v);
        if (v->u.b) return json_obj();
        json_t *o = json_obj();
        json_set(o, "not", json_obj());
        return o;
    }
    if (!json_is_obj(v)) return json_clone(v);
    if (in_map) {
        json_t *o = json_obj();
        const char *k;
        json_t *x;
        json_obj_foreach(v, i, k, x) { json_set(o, k, gnorm(x, false, true, depth + 1)); }
        return o;
    }
    /* snake_case renames */
    json_t *obj = json_obj();
    {
        const char *k;
        json_t *x;
        json_obj_foreach(v, i, k, x) {
            const char *rk = snake_rename(k);
            if (!json_get(obj, rk) || rk != k) json_set(obj, rk, json_clone(x));
        }
    }
    /* multi-type array -> anyOf branches */
    json_t *ta = json_get(obj, "type");
    if (json_is_arr(ta) && !json_is_arr(json_get(obj, "anyOf"))) {
        strvec_t types = {0};
        bool has_null = false;
        json_t *it;
        json_arr_foreach(ta, i, it) {
            if (!json_is_str(it)) continue;
            if (!strcmp(json_sv(it), "null")) has_null = true;
            else if (!strvec_contains(&types, json_sv(it))) strvec_push(&types, json_sv(it));
        }
        if (types.n >= 2) {
            json_t *out = json_obj(), *branches = json_arr();
            for (size_t t = 0; t < types.n; t++) {
                json_t *br = json_obj();
                json_set_str(br, "type", types.v[t]);
                json_push(branches, br);
            }
            const char *k;
            json_t *x;
            json_obj_foreach(obj, i, k, x) {
                if (!strcmp(k, "type")) continue;
                if (!any_type_specific(k)) {
                    json_set(out, k, json_clone(x));
                    continue;
                }
                for (size_t t = 0; t < types.n; t++)
                    if (type_specific_for(types.v[t], k)) json_set(json_at(branches, t), k, json_clone(x));
            }
            if (has_null) json_set_bool(out, "nullable", true);
            json_set(out, "anyOf", branches);
            json_free(obj);
            obj = out;
        }
        strvec_free(&types);
    }
    /* collapse null fields */
    if (str_eq(json_get_str(obj, "type"), "null")) {
        json_del(obj, "type");
        json_set_bool(obj, "nullable", true);
    } else if (json_is_arr(json_get(obj, "anyOf"))) {
        json_t *kept = json_arr(), *it;
        bool saw_null = false;
        json_arr_foreach(json_get(obj, "anyOf"), i, it) {
            if (json_is_obj(it) && str_eq(json_get_str(it, "type"), "null")) saw_null = true;
            else json_push(kept, json_clone(it));
        }
        if (saw_null) {
            json_set_bool(obj, "nullable", true);
            if (json_len(kept) == 0) {
                json_del(obj, "anyOf");
                json_free(kept);
            } else if (json_len(kept) == 1 && json_is_obj(json_at(kept, 0))) {
                json_del(obj, "anyOf");
                const char *k;
                json_t *x;
                json_obj_foreach(json_at(kept, 0), i, k, x) {
                    if (!json_get(obj, k)) json_set(obj, k, json_clone(x));
                }
                json_free(kept);
            } else {
                json_set(obj, "anyOf", kept);
            }
        } else {
            json_free(kept);
        }
    }
    json_t *res = json_obj(), *spill = json_obj();
    /* all-const anyOf/oneOf -> enum */
    static const char *const combs[] = {"anyOf", "oneOf"};
    const char *const_comb = NULL;
    for (int c = 0; c < 2 && !const_comb; c++) {
        json_t *vars = json_get(obj, combs[c]);
        if (!json_is_arr(vars) || json_len(vars) == 0) continue;
        bool all = true;
        json_t *it;
        json_arr_foreach(vars, i, it) {
            if (!json_is_obj(it) || !json_get(it, "const")) all = false;
        }
        if (!all) continue;
        const_comb = combs[c];
        json_t *en = json_arr();
        json_arr_foreach(vars, i, it) { push_enum(en, json_get(it, "const")); }
        json_set(res, "enum", en);
        const char *t0 = json_get_str(json_at(vars, 0), "type");
        bool same = t0 != NULL;
        json_arr_foreach(vars, i, it) {
            if (!str_eq(json_get_str(it, "type"), t0)) same = false;
        }
        if (same) {
            json_set_str(res, "type", t0);
        } else {
            strvec_t inferred = {0};
            bool has_null = false;
            json_arr_foreach(en, i, it) {
                const char *t = infer_type(it);
                if (!strcmp(t, "null")) has_null = true;
                else if (!strvec_contains(&inferred, t)) strvec_push(&inferred, t);
            }
            if (inferred.n == 1 && !has_null) json_set_str(res, "type", inferred.v[0]);
            else if (inferred.n == 0 && has_null) json_set_str(res, "type", "null");
            else if (inferred.n == 1 && has_null) {
                json_set_str(res, "type", inferred.v[0]);
                json_set_bool(res, "nullable", true);
            }
            strvec_free(&inferred);
        }
    }
    json_t *const_val = NULL;
    const char *k;
    json_t *x;
    json_obj_foreach(obj, i, k, x) {
        if (const_comb && (!strcmp(k, const_comb) || json_get(res, k))) continue;
        if (IN(k, UNSUPPORTED)) {
            if (IN(k, LIFTABLE)) json_set(spill, k, json_clone(x));
            continue;
        }
        if (!const_comb && !strcmp(k, "const")) {
            const_val = x;
            continue;
        }
        json_set(res, k, norm_child(k, x, false, depth));
    }
    if (!const_comb) {
        json_t *tt = json_get(res, "type");
        if (json_is_arr(tt)) {
            const char *first = NULL, *nonnull = NULL;
            bool has_null = false;
            json_t *it;
            json_arr_foreach(tt, i, it) {
                if (!json_is_str(it)) continue;
                if (!first) first = json_sv(it);
                if (!strcmp(json_sv(it), "null")) has_null = true;
                else if (!nonnull) nonnull = json_sv(it);
            }
            char *pick = xstrdup(nonnull ? nonnull : first ? first : "string");
            if (has_null) json_set_bool(res, "nullable", true);
            json_set_str(res, "type", pick);
            /* drop keywords constraining other types */
            for (size_t q = json_len(res); q > 0; q--) {
                const char *rk = res->u.obj.keys[q - 1];
                if (any_type_specific(rk) && !type_specific_for(pick, rk)) json_del(res, rk);
            }
            free(pick);
        }
        if (const_val) {
            json_t *en = json_is_arr(json_get(res, "enum")) ? json_clone(json_get(res, "enum")) : json_arr();
            push_enum(en, const_val);
            json_set(res, "enum", en);
            if (!json_get(res, "type")) json_set_str(res, "type", infer_type(const_val));
        }
        json_t *en = json_get(res, "enum");
        if (!json_get(res, "type") && !json_is_arr(json_get(res, "anyOf")) && !json_is_arr(json_get(res, "oneOf")) &&
            json_is_arr(en) && json_len(en) > 0) {
            const char *t0 = infer_type(json_at(en, 0));
            bool same = true;
            json_t *it;
            json_arr_foreach(en, i, it) {
                if (!str_eq(infer_type(it), t0)) same = false;
            }
            if (same) json_set_str(res, "type", t0);
        }
        if (str_eq(json_get_str(res, "type"), "null")) {
            json_del(res, "type");
            json_set_bool(res, "nullable", true);
        }
        json_t *props = json_get(res, "properties");
        if (str_eq(json_get_str(res, "type"), "object") && !json_get(res, "propertyOrdering") && json_is_obj(props) &&
            json_len(props) > 1) {
            json_t *ord = json_arr();
            for (size_t q = 0; q < props->u.obj.n; q++) json_push_str(ord, props->u.obj.keys[q]);
            json_set(res, "propertyOrdering", ord);
        }
        if (str_eq(json_get_str(res, "type"), "object") && !json_get(res, "properties")) json_set(res, "properties", json_obj());
    }
    spill_to_description(res, spill);
    json_free(spill);
    json_free(obj);
    post_process(res, bool_is_schema);
    return res;
}

json_t *google_normalize_schema(const json_t *schema) {
    json_t *d = deref(schema, schema, 0);
    json_t *out = gnorm(d, false, true, 0);
    json_free(d);
    return out;
}

/* ======================================================================
 * normalizeSchemaForCCA: Claude behind Cloud Code Assist takes the legacy `parameters` field, which the backend
 * translates into Anthropic's `input_schema`. Nullable/combiner/`not` forms it cannot represent are collapsed;
 * anything left over falls back to an empty object schema.
 * (Upstream also meta-validates the result; omc trusts the walk.)
 * ====================================================================== */

static const char *const CCA_SHARED[] = {"title", "description", "default", "examples"};
static const char *const CCA_TYPES[] = {"array", "object", "string", "number", "integer", "boolean", "null"};

static bool cca_known_type(const char *t) { return t && IN(t, CCA_TYPES); }
static bool cca_allowed(const char *type, const char *k) { return type_specific_for(type, k) || IN(k, CCA_SHARED); }

static void drop_foreign_type_keywords(json_t *schema, const char *type) {
    for (size_t q = json_len(schema); q > 0; q--) {
        const char *k = schema->u.obj.keys[q - 1];
        if (any_type_specific(k) && !type_specific_for(type, k)) json_del(schema, k);
    }
}

static json_t *copy_without(const json_t *schema, const char *key) {
    json_t *c = json_clone(schema);
    json_del(c, key);
    return c;
}

static char *merge_descriptions(const json_t *existing, const json_t *incoming) {
    const char *e = json_sv(existing), *in = json_sv(incoming);
    if (!e) return xstrdup(in ? in : "");
    if (!in || !*in || !strcmp(e, in)) return xstrdup(e);
    if (!*e) return xstrdup(in);
    return xasprintf("%s\n\n%s", e, in);
}

/* mergeCompatibleEnumSchemas: owned union or NULL */
static json_t *merge_enum_schemas(const json_t *a, const json_t *b) {
    if (!json_is_obj(a) || !json_is_obj(b)) return NULL;
    json_t *ea = json_get_arr(a, "enum"), *eb = json_get_arr(b, "enum");
    if (!ea || !eb) return NULL;
    json_t *ta = json_get(a, "type"), *tb = json_get(b, "type");
    if ((ta || tb) && !(ta && tb && json_equal(ta, tb))) return NULL;
    if (json_len(a) != json_len(b)) return NULL;
    const char *k;
    json_t *v;
    json_obj_foreach(a, i, k, v) {
        if (!strcmp(k, "enum")) continue;
        json_t *o = json_get(b, k);
        if (!o || !json_equal(v, o)) return NULL;
    }
    json_t *out = json_clone(a), *merged = json_get(out, "enum"), *it;
    json_arr_foreach(eb, i, it) { push_enum(merged, it); }
    return out;
}

/* mergePropertySchemas (owned) */
static json_t *merge_property_schemas(const json_t *a, const json_t *b) {
    if (json_equal(a, b)) return json_clone(a);
    json_t *m = merge_enum_schemas(a, b);
    if (m) return m;
    json_t *variants = json_arr(), *it;
    if (json_is_obj(a) && json_is_arr(json_get(a, "anyOf"))) {
        json_arr_foreach(json_get(a, "anyOf"), i, it) { json_push(variants, json_clone(it)); }
    } else {
        json_push(variants, json_clone(a));
    }
    if (json_is_obj(b) && json_is_arr(json_get(b, "anyOf"))) {
        json_arr_foreach(json_get(b, "anyOf"), i, it) { push_enum(variants, it); }
    } else {
        push_enum(variants, b);
    }
    if (json_len(variants) == 1) {
        json_t *one = json_arr_remove(variants, 0);
        json_free(variants);
        return one;
    }
    json_t *o = json_obj();
    json_set(o, "anyOf", variants);
    return o;
}

static bool str_arr_has(const json_t *arr, const char *s) {
    json_t *it;
    json_arr_foreach(arr, i, it) {
        if (str_eq(json_sv(it), s)) return true;
    }
    return false;
}

/* mergeObjectCombinerVariants; replaces *sp and returns true when it applied */
static bool merge_object_combiner(json_t **sp, const char *comb) {
    json_t *s = *sp, *vars = json_get(s, comb), *v;
    if (!json_is_arr(vars) || json_len(vars) == 0) return false;
    json_arr_foreach(vars, i, v) {
        if (!json_is_obj(v)) return false;
        json_t *ty = json_get(v, "type");
        bool shape = json_is_obj(json_get(v, "properties")) || json_is_arr(json_get(v, "required")) ||
                     json_get(v, "additionalProperties");
        if (!ty && !shape) return false;
        if (ty && !str_eq(json_sv(ty), "object")) return false;
        if (json_get(v, "properties") && !json_is_obj(json_get(v, "properties"))) return false;
        if (json_get(v, "required") && !json_is_arr(json_get(v, "required"))) return false;
    }
    json_t *own = json_get_obj(s, "properties");
    json_t *merged = own ? json_clone(own) : json_obj();
    json_arr_foreach(vars, i, v) {
        const char *name;
        json_t *ps;
        json_obj_foreach(json_get_obj(v, "properties"), j, name, ps) {
            json_t *ex = json_get(merged, name);
            json_set(merged, name, ex ? merge_property_schemas(ex, ps) : json_clone(ps));
        }
    }
    json_t *next = copy_without(s, comb);
    json_set_str(next, "type", "object");
    /* anyOf/oneOf accept any single branch: only fields every branch requires stay required */
    json_t *inter = NULL;
    json_arr_foreach(vars, i, v) {
        json_t *req = json_get_arr(v, "required"), *r;
        json_t *strs = json_arr();
        json_arr_foreach(req, j, r) {
            if (json_is_str(r)) json_push(strs, json_clone(r));
        }
        if (!inter) {
            inter = strs;
        } else {
            json_t *keep = json_arr();
            json_arr_foreach(inter, j, r) {
                if (str_arr_has(strs, json_sv(r))) json_push(keep, json_clone(r));
            }
            json_free(inter);
            json_free(strs);
            inter = keep;
        }
    }
    json_t *parent_req = json_get_arr(s, "required");
    json_t *required = json_arr();
    for (size_t q = 0; q < json_len(merged); q++) {
        const char *name = merged->u.obj.keys[q];
        bool keep = str_arr_has(inter, name) || (own && json_get(own, name) && str_arr_has(parent_req, name));
        if (keep) json_push_str(required, name);
    }
    json_free(inter);
    json_set(next, "properties", merged);
    if (json_len(required) > 0) {
        json_set(next, "required", required);
    } else {
        json_free(required);
        json_del(next, "required");
    }
    json_free(s);
    *sp = next;
    return true;
}

/* collapseMixedTypeCombinerVariants */
static bool collapse_mixed_type(json_t **sp, const char *comb) {
    json_t *s = *sp, *vars = json_get(s, comb), *v;
    if (!json_is_arr(vars) || json_len(vars) == 0) return false;
    strvec_t types = {0};
    json_t *fields = json_obj();
    bool ok = true;
    json_arr_foreach(vars, i, v) {
        const char *ty = json_is_obj(v) ? json_get_str(v, "type") : NULL;
        if (!ty || strvec_contains(&types, ty) || !cca_known_type(ty)) {
            ok = false;
            break;
        }
        const char *k;
        json_t *x;
        json_obj_foreach(v, j, k, x) {
            if (!strcmp(k, "type")) continue;
            if (!cca_allowed(ty, k)) {
                ok = false;
                break;
            }
            json_t *ex = json_get(fields, k);
            if (ex && !json_equal(ex, x)) {
                if (strcmp(k, "description") != 0) {
                    ok = false;
                    break;
                }
                json_set(fields, k, json_str_own(merge_descriptions(ex, x)));
                continue;
            }
            json_set(fields, k, json_clone(x));
        }
        if (!ok) break;
        strvec_push(&types, ty);
    }
    bool all_object = true;
    for (size_t i = 0; i < types.n; i++)
        if (strcmp(types.v[i], "object") != 0) all_object = false;
    if (!ok || types.n < 2 || all_object) {
        strvec_free(&types);
        json_free(fields);
        return false;
    }
    const char *chosen = types.v[0];
    for (size_t i = 0; i < types.n; i++) {
        if (strcmp(types.v[i], "null") != 0) {
            chosen = types.v[i];
            break;
        }
    }
    json_t *next = copy_without(s, comb);
    json_set_str(next, "type", chosen);
    drop_foreign_type_keywords(next, chosen);
    const char *k;
    json_t *x;
    json_obj_foreach(fields, i, k, x) {
        if (!cca_allowed(chosen, k)) continue;
        json_t *ex = json_get(next, k);
        if (ex && !json_equal(ex, x)) {
            if (strcmp(k, "description") != 0) {
                ok = false;
                break;
            }
            json_set(next, k, json_str_own(merge_descriptions(ex, x)));
            continue;
        }
        if (!ex) json_set(next, k, json_clone(x));
    }
    strvec_free(&types);
    json_free(fields);
    if (!ok) {
        json_free(next);
        return false;
    }
    json_free(s);
    *sp = next;
    return true;
}

/* collapseSameTypeCombinerVariants */
static bool collapse_same_type(json_t **sp, const char *comb) {
    json_t *s = *sp, *vars = json_get(s, comb), *v;
    if (!json_is_arr(vars) || json_len(vars) == 0) return false;
    const char *common = NULL;
    size_t enums = 0;
    json_arr_foreach(vars, i, v) {
        const char *ty = json_is_obj(v) ? json_get_str(v, "type") : NULL;
        if (!ty) return false;
        if (!common) common = ty;
        else if (strcmp(ty, common) != 0) return false;
        if (json_is_arr(json_get(v, "enum"))) enums++;
    }
    json_t *collapsed;
    if (enums == json_len(vars)) {
        collapsed = json_clone(json_at(vars, 0));
        for (size_t i = 1; i < json_len(vars) && collapsed; i++) {
            json_t *m = merge_enum_schemas(collapsed, json_at(vars, i));
            json_free(collapsed);
            collapsed = m;
        }
        if (!collapsed) return false;
    } else if (enums > 0) {
        json_t *pick = json_at(vars, 0);
        json_arr_foreach(vars, i, v) {
            if (!json_is_arr(json_get(v, "enum"))) {
                pick = v;
                break;
            }
        }
        collapsed = json_clone(pick);
    } else {
        collapsed = json_clone(json_at(vars, 0));
    }
    json_t *next = copy_without(s, comb);
    const char *k;
    json_t *x;
    json_obj_foreach(collapsed, i, k, x) {
        if (!json_get(next, k)) json_set(next, k, json_clone(x));
    }
    json_free(collapsed);
    json_free(s);
    *sp = next;
    return true;
}

static json_t *cca_post(json_t *s) {
    static const char *const combs[] = {"anyOf", "oneOf"};
    for (int c = 0; c < 2; c++) {
        merge_object_combiner(&s, combs[c]);
        collapse_mixed_type(&s, combs[c]);
        collapse_same_type(&s, combs[c]);
    }
    return s;
}

static json_t *cnorm(const json_t *v, bool in_map, bool bool_is_schema, int depth);

static json_t *cnorm_child(const char *k, const json_t *v, int depth) {
    int kind = child_kind(k, v, false);
    if (!kind) return json_clone(v);
    return cnorm(v, kind == 2, kind == 1, depth + 1);
}

static json_t *cnorm(const json_t *v, bool in_map, bool bool_is_schema, int depth) {
    if (depth > SCHEMA_MAX_DEPTH) return json_obj();
    if (json_is_arr(v)) {
        json_t *o = json_arr(), *it;
        json_arr_foreach(v, i, it) { json_push(o, cnorm(it, false, bool_is_schema, depth + 1)); }
        return o;
    }
    if (json_is_bool(v)) {
        if (!bool_is_schema) return json_clone(v);
        if (v->u.b) return json_obj();
        json_t *o = json_obj();
        json_set(o, "not", json_obj());
        return o;
    }
    if (!json_is_obj(v)) return json_clone(v);
    if (in_map) {
        json_t *o = json_obj();
        const char *k;
        json_t *x;
        json_obj_foreach(v, i, k, x) { json_set(o, k, cnorm(x, false, true, depth + 1)); }
        return o;
    }
    json_t *obj = json_obj();
    {
        const char *k;
        json_t *x;
        json_obj_foreach(v, i, k, x) {
            const char *rk = snake_rename(k);
            if (!json_get(obj, rk) || rk != k) json_set(obj, rk, json_clone(x));
        }
    }
    /* multi-type array -> one anyOf branch per non-null type (CCA strips the nullable marker) */
    json_t *ta = json_get(obj, "type");
    if (json_is_arr(ta) && !json_is_arr(json_get(obj, "anyOf"))) {
        strvec_t types = {0};
        json_t *it;
        json_arr_foreach(ta, i, it) {
            if (json_is_str(it) && strcmp(json_sv(it), "null") != 0 && !strvec_contains(&types, json_sv(it)))
                strvec_push(&types, json_sv(it));
        }
        if (types.n >= 2) {
            json_t *out = json_obj(), *branches = json_arr();
            for (size_t t = 0; t < types.n; t++) {
                json_t *br = json_obj();
                json_set_str(br, "type", types.v[t]);
                json_push(branches, br);
            }
            const char *k;
            json_t *x;
            json_obj_foreach(obj, i, k, x) {
                if (!strcmp(k, "type")) continue;
                if (!any_type_specific(k)) {
                    json_set(out, k, json_clone(x));
                    continue;
                }
                for (size_t t = 0; t < types.n; t++)
                    if (type_specific_for(types.v[t], k)) json_set(json_at(branches, t), k, json_clone(x));
            }
            json_set(out, "anyOf", branches);
            json_free(obj);
            obj = out;
        }
        strvec_free(&types);
    }
    json_t *res = json_obj(), *spill = json_obj();
    static const char *const combs[] = {"anyOf", "oneOf"};
    const char *const_comb = NULL;
    for (int c = 0; c < 2 && !const_comb; c++) {
        json_t *vars = json_get(obj, combs[c]), *it;
        if (!json_is_arr(vars) || json_len(vars) == 0) continue;
        bool all = true;
        json_arr_foreach(vars, i, it) {
            if (!json_is_obj(it) || !json_get(it, "const")) all = false;
        }
        if (!all) continue;
        const_comb = combs[c];
        json_t *en = json_arr();
        json_arr_foreach(vars, i, it) { push_enum(en, json_get(it, "const")); }
        json_set(res, "enum", en);
        const char *t0 = json_get_str(json_at(vars, 0), "type");
        bool same = t0 != NULL;
        json_arr_foreach(vars, i, it) {
            if (!str_eq(json_get_str(it, "type"), t0)) same = false;
        }
        if (same) {
            json_set_str(res, "type", t0);
        } else {
            strvec_t inferred = {0}, nonnull = {0};
            json_arr_foreach(en, i, it) {
                const char *t = infer_type(it);
                if (!strvec_contains(&inferred, t)) strvec_push(&inferred, t);
                if (strcmp(t, "null") != 0 && !strvec_contains(&nonnull, t)) strvec_push(&nonnull, t);
            }
            if (inferred.n == 1) json_set_str(res, "type", inferred.v[0]);
            else if (strvec_contains(&inferred, "null") && nonnull.n == 1) json_set_str(res, "type", nonnull.v[0]);
            strvec_free(&inferred);
            strvec_free(&nonnull);
        }
    }
    json_t *const_val = NULL;
    const char *k;
    json_t *x;
    json_obj_foreach(obj, i, k, x) {
        if (const_comb && (!strcmp(k, const_comb) || json_get(res, k))) continue;
        if (IN(k, UNSUPPORTED)) {
            if (IN(k, LIFTABLE)) json_set(spill, k, json_clone(x));
            continue;
        }
        if (!strcmp(k, "nullable")) continue;
        if (!const_comb && !strcmp(k, "const")) {
            const_val = x;
            continue;
        }
        json_set(res, k, cnorm_child(k, x, depth));
    }
    if (!const_comb) {
        json_t *tt = json_get(res, "type");
        if (json_is_arr(tt)) {
            const char *first = NULL, *nonnull = NULL;
            json_t *it;
            json_arr_foreach(tt, i, it) {
                if (!json_is_str(it)) continue;
                if (!first) first = json_sv(it);
                if (strcmp(json_sv(it), "null") != 0 && !nonnull) nonnull = json_sv(it);
            }
            if (nonnull || first) {
                char *pick = xstrdup(nonnull ? nonnull : first);
                json_set_str(res, "type", pick);
                drop_foreign_type_keywords(res, pick);
                free(pick);
            } else {
                json_del(res, "type");
            }
        }
        if (const_val) {
            json_t *en = json_is_arr(json_get(res, "enum")) ? json_clone(json_get(res, "enum")) : json_arr();
            push_enum(en, const_val);
            json_set(res, "enum", en);
            if (!json_get(res, "type")) json_set_str(res, "type", infer_type(const_val));
        }
        json_t *en = json_get(res, "enum");
        if (!json_get(res, "type") && !json_is_arr(json_get(res, "anyOf")) && !json_is_arr(json_get(res, "oneOf")) &&
            json_is_arr(en) && json_len(en) > 0) {
            const char *t0 = infer_type(json_at(en, 0));
            bool same = true;
            json_t *it;
            json_arr_foreach(en, i, it) {
                if (!str_eq(infer_type(it), t0)) same = false;
            }
            if (same) json_set_str(res, "type", t0);
        }
        if (str_eq(json_get_str(res, "type"), "object") && !json_get(res, "properties")) json_set(res, "properties", json_obj());
    }
    spill_to_description(res, spill);
    json_free(spill);
    json_free(obj);
    return cca_post(res);
}

/* stripResidualCombiners: collapse anyOf/oneOf that object merging re-created in merged subtrees */
static json_t *strip_residual(const json_t *v, bool in_map, int depth) {
    if (depth > SCHEMA_MAX_DEPTH) return json_obj();
    if (json_is_arr(v)) {
        json_t *o = json_arr(), *it;
        json_arr_foreach(v, i, it) { json_push(o, strip_residual(it, false, depth + 1)); }
        return o;
    }
    if (!json_is_obj(v)) return json_clone(v);
    json_t *res = json_obj();
    const char *k;
    json_t *x;
    json_obj_foreach(v, i, k, x) {
        int kind = child_kind(k, x, in_map);
        json_set(res, k, kind ? strip_residual(x, kind == 2, depth + 1) : json_clone(x));
    }
    if (in_map) return res;
    static const char *const combs[] = {"anyOf", "oneOf"};
    for (int guard = 0; guard < 64; guard++) {
        bool changed = false;
        for (int c = 0; c < 2; c++) {
            if (collapse_same_type(&res, combs[c])) changed = true;
            if (collapse_mixed_type(&res, combs[c])) changed = true;
        }
        if (!changed) break;
    }
    return res;
}

/* extractNullableUnionSchema: *nullable set when a property schema is "T or null" (takes ownership of s) */
static json_t *extract_nullable(json_t *s, bool *nullable) {
    *nullable = false;
    if (!json_is_obj(s)) return s;
    json_t *nv = json_get(s, "nullable");
    if (json_is_bool(nv) && nv->u.b) {
        json_del(s, "nullable");
        *nullable = true;
        return s;
    }
    json_t *ta = json_get(s, "type");
    if (json_is_arr(ta)) {
        bool has_null = false;
        const char *only = NULL;
        size_t nonnull = 0;
        json_t *it;
        json_arr_foreach(ta, i, it) {
            if (!json_is_str(it)) continue;
            if (!strcmp(json_sv(it), "null")) {
                has_null = true;
            } else {
                nonnull++;
                only = json_sv(it);
            }
        }
        if (has_null && nonnull == 1) {
            char *t = xstrdup(only);
            json_set_str(s, "type", t);
            free(t);
            *nullable = true;
            return s;
        }
    }
    static const char *const combs[] = {"anyOf", "oneOf"};
    for (int c = 0; c < 2; c++) {
        json_t *vars = json_get(s, combs[c]), *it;
        if (!json_is_arr(vars)) continue;
        bool has_null = false;
        json_t *keep = NULL;
        size_t nkeep = 0;
        json_arr_foreach(vars, i, it) {
            if (json_is_obj(it) && str_eq(json_get_str(it, "type"), "null") && json_len(it) == 1) {
                has_null = true;
                continue;
            }
            nkeep++;
            keep = it;
        }
        if (!has_null || nkeep != 1 || !json_is_obj(keep)) continue;
        json_t *next = copy_without(s, combs[c]);
        const char *k;
        json_t *x;
        json_obj_foreach(keep, i, k, x) {
            json_t *ex = json_get(next, k);
            if (ex && !json_equal(ex, x)) {
                json_free(next);
                return s;
            }
            if (!ex) json_set(next, k, json_clone(x));
        }
        json_free(s);
        *nullable = true;
        return next;
    }
    return s;
}

/* normalizeNullablePropertiesForCloudCodeAssist: a nullable property becomes optional instead */
static json_t *nullable_props(const json_t *v, bool is_prop, bool in_map, bool *nullable, int depth) {
    *nullable = false;
    if (depth > SCHEMA_MAX_DEPTH) return json_obj();
    bool dummy;
    if (json_is_arr(v)) {
        json_t *o = json_arr(), *it;
        json_arr_foreach(v, i, it) { json_push(o, nullable_props(it, false, false, &dummy, depth + 1)); }
        return o;
    }
    if (!json_is_obj(v)) return json_clone(v);
    json_t *n = json_obj();
    const char *k;
    json_t *x;
    json_obj_foreach(v, i, k, x) {
        int kind = child_kind(k, x, in_map);
        json_set(n, k, kind ? nullable_props(x, false, kind == 2, &dummy, depth + 1) : json_clone(x));
    }
    if (in_map) return n;
    json_t *props = json_get_obj(n, "properties");
    if (props) {
        json_t *req = json_get_arr(n, "required");
        json_t *dropped = json_arr();
        for (size_t q = 0; q < json_len(props); q++) {
            bool nul = false;
            json_t *ps = nullable_props(props->u.obj.vals[q], true, false, &nul, depth + 1);
            json_free(props->u.obj.vals[q]);
            props->u.obj.vals[q] = ps;
            if (nul) json_push_str(dropped, props->u.obj.keys[q]);
        }
        if (req) {
            json_t *nr = json_arr(), *it;
            json_arr_foreach(req, i, it) {
                if (!json_is_str(it) || str_arr_has(dropped, json_sv(it)) || str_arr_has(nr, json_sv(it))) continue;
                json_push(nr, json_clone(it));
            }
            json_set(n, "required", nr);
        }
        json_free(dropped);
    }
    if (!is_prop) return n;
    return extract_nullable(n, nullable);
}

static bool residual_incompatible(const json_t *v, bool in_map, int depth) {
    if (depth > SCHEMA_MAX_DEPTH) return false;
    if (json_is_arr(v)) {
        json_t *it;
        json_arr_foreach(v, i, it) {
            if (residual_incompatible(it, false, depth + 1)) return true;
        }
        return false;
    }
    if (!json_is_obj(v)) return false;
    if (!in_map) {
        if (json_is_arr(json_get(v, "type"))) return true;
        if (str_eq(json_get_str(v, "type"), "null")) return true;
        if (json_get(v, "nullable") || json_get(v, "not")) return true;
        if (json_is_arr(json_get(v, "allOf")) || json_is_arr(json_get(v, "anyOf")) || json_is_arr(json_get(v, "oneOf")))
            return true;
    }
    const char *k;
    json_t *x;
    json_obj_foreach(v, i, k, x) {
        int kind = child_kind(k, x, in_map);
        if (kind && residual_incompatible(x, kind == 2, depth + 1)) return true;
    }
    return false;
}

json_t *google_normalize_schema_cca(const json_t *schema) {
    json_t *d = deref(schema, schema, 0);
    json_t *a = cnorm(d, false, true, 0);
    json_free(d);
    json_t *b = strip_residual(a, false, 0);
    json_free(a);
    bool nul;
    json_t *c = nullable_props(b, false, false, &nul, 0);
    json_free(b);
    if (residual_incompatible(c, false, 0)) {
        json_free(c);
        c = json_obj();
        json_set_str(c, "type", "object");
        json_set(c, "properties", json_obj());
    }
    return c;
}
