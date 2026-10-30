/*
 * provider_google.c - Gemini generateContent streaming (apis "google-generative-ai" and "google-vertex").
 * Port of packages/ai/src/providers/google.ts, google-vertex.ts, google-shared.ts and the Google branches of
 * stream.ts option mapping + utils/schema normalizeSchemaForGoogle.
 */
#include "ai/provider_google.h"

#include <math.h>

#define DEFAULT_GEMINI_BASE "https://generativelanguage.googleapis.com/v1beta"
#define SKIP_THOUGHT_SIGNATURE "skip_thought_signature_validator"
#define NON_VISION_IMAGE_PLACEHOLDER "[image omitted: model does not support vision]"
#define MIN_OUTPUT_TOKENS 1024
#define OUTPUT_CAP_WHEN_UNKNOWN 64000
#define MAX_EMPTY_STREAM_RETRIES 2
#define EMPTY_STREAM_BASE_DELAY_MS 500
#define SCHEMA_MAX_DEPTH 64

static const int64_t GOOGLE_THINKING[] = {0, 1024, 4096, 8192, 16384, 24575, 32768};

/* ---------- helpers ---------- */

static bool cflag(const ai_model_t *m, const char *key) {
    json_t *v = model_compat_get(m, key);
    return json_is_bool(v) && v->u.b;
}

static bool str_blank(const char *s) {
    if (!s) return true;
    for (; *s; s++)
        if (!(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')) return false;
    return true;
}

static const char *thinking_mode(const ai_model_t *m) { return m->thinking ? json_get_str(m->thinking, "mode") : NULL; }
static bool thinking_flag(const ai_model_t *m, const char *k) { return m->thinking && json_get_bool(m->thinking, k, false); }

static bool efforts_contain(const json_t *arr, const char *name) {
    json_t *it;
    json_arr_foreach(arr, i, it) {
        if (str_eq(json_sv(it), name)) return true;
    }
    return false;
}

static ai_effort_t clamp_effort(const ai_model_t *m, ai_effort_t e) {
    json_t *arr = m->thinking ? json_get_arr(m->thinking, "efforts") : NULL;
    if (!arr || json_len(arr) == 0 || efforts_contain(arr, effort_name(e))) return e;
    for (int x = (int)e; x >= EFFORT_MINIMAL; x--)
        if (efforts_contain(arr, effort_name((ai_effort_t)x))) return (ai_effort_t)x;
    for (int x = (int)e; x <= EFFORT_MAX; x++)
        if (efforts_contain(arr, effort_name((ai_effort_t)x))) return (ai_effort_t)x;
    return e;
}

static ai_effort_t minimum_effort(const ai_model_t *m) {
    json_t *arr = m->thinking ? json_get_arr(m->thinking, "efforts") : NULL;
    for (int x = EFFORT_MINIMAL; x <= EFFORT_MAX; x++)
        if (efforts_contain(arr, effort_name((ai_effort_t)x))) return (ai_effort_t)x;
    return EFFORT_OFF;
}

static int64_t effort_budget_field(const ai_model_t *m, ai_effort_t e, bool *found) {
    json_t *eb = m->thinking ? json_get_obj(m->thinking, "effortBudgets") : NULL;
    json_t *v = eb ? json_get(eb, effort_name(e)) : NULL;
    *found = json_is_num(v);
    return *found ? (int64_t)v->u.num.d : 0;
}

static int64_t custom_budget(const ai_stream_opts_t *o, ai_effort_t e, bool *found) {
    json_t *v = o->thinking_budgets ? json_get(o->thinking_budgets, effort_name(e)) : NULL;
    *found = json_is_num(v);
    return *found ? (int64_t)v->u.num.d : 0;
}

static int64_t thinking_headroom(const ai_model_t *m, const ai_stream_opts_t *o, ai_effort_t e) {
    bool f;
    int64_t c = custom_budget(o, e, &f);
    if (f && c > 0) return c;
    int64_t cat = effort_budget_field(m, e, &f);
    return f && cat > 0 ? cat : GOOGLE_THINKING[e];
}

static int64_t max_output(int64_t base, const ai_model_t *m, int64_t headroom) {
    if (base < 0) return -1;
    int64_t un = base + headroom;
    return m->max_tokens > 0 ? MIN(un, m->max_tokens) : un;
}

static const char *thinking_level(ai_effort_t e, const ai_model_t *m) {
    if (e == EFFORT_MINIMAL) {
        json_t *r = m->thinking ? json_get_obj(m->thinking, "effortRouting") : NULL;
        const char *a = r ? json_get_str(r, "minimal") : NULL, *b = r ? json_get_str(r, "low") : NULL;
        if (a && str_eq(a, b)) return "LOW";
        return "MINIMAL";
    }
    if (e == EFFORT_LOW) return "LOW";
    if (e == EFFORT_MEDIUM) return "MEDIUM";
    return "HIGH";
}

static char *render_demoted_thinking(const char *model_id, const char *text) {
    if (!text || !*text) return xstrdup("");
    if (str_istr(model_id, "claude")) return xstrdup(text);
    if (str_istr(model_id, "gemini")) return xasprintf("```thinking\n%s\n```", text);
    if (str_istr(model_id, "minimax")) return xasprintf("<thinking>\n%s\n</thinking>", text);
    return xasprintf("<think>\n%s\n</think>", text);
}

/* base64 (TYPE_BYTES) thought signatures only */
static bool valid_signature(const char *s) {
    if (!s || !*s) return false;
    size_t n = strlen(s);
    if (n % 4) return false;
    size_t i = 0;
    for (; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')) break;
    }
    size_t pad = n - i;
    if (pad > 2 || i == 0) return false;
    for (; i < n; i++)
        if (s[i] != '=') return false;
    return true;
}

static const char *resolve_signature(bool same, const char *sig) { return same && valid_signature(sig) ? sig : NULL; }

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
 * message conversion (google-shared convertMessages)
 * ====================================================================== */

#define ORD_KEY "\x01omc_ord"

static json_t *image_part(const json_t *img) {
    json_t *p = json_obj();
    const char *url = json_get_str(img, "url");
    const char *mime = json_get_str(img, "mimeType");
    if (url && *url) {
        json_t *fd = json_obj();
        json_set_str(fd, "fileUri", url);
        json_set_str(fd, "mimeType", mime ? mime : "");
        json_set(p, "fileData", fd);
    } else {
        json_t *id = json_obj();
        json_set_str(id, "mimeType", mime ? mime : "");
        const char *d = json_get_str(img, "data");
        json_set_str(id, "data", d ? d : "");
        json_set(p, "inlineData", id);
    }
    return p;
}

static json_t *text_part(const char *t) {
    json_t *p = json_obj();
    json_set_str(p, "text", t ? t : "");
    return p;
}

static json_t *content_of(const char *role, json_t *parts) {
    json_t *c = json_obj();
    json_set_str(c, "role", role);
    json_set(c, "parts", parts);
    return c;
}

/* sort parts carrying ORD_KEY into key order within the slots they occupy, then strip the key */
static void sort_known_in_place(json_t *parts) {
    size_t n = json_len(parts);
    size_t *pos = xcalloc(n ? n : 1, sizeof(size_t));
    size_t kn = 0;
    for (size_t i = 0; i < n; i++)
        if (json_is_num(json_get(json_at(parts, i), ORD_KEY))) pos[kn++] = i;
    if (kn >= 2) {
        json_t **items = xmalloc(kn * sizeof(json_t *));
        for (size_t i = 0; i < kn; i++) items[i] = parts->u.arr.items[pos[i]];
        for (size_t i = 1; i < kn; i++) { /* stable insertion sort by ordinal */
            json_t *cur = items[i];
            int64_t ck = json_get_int(cur, ORD_KEY, 0);
            size_t j = i;
            while (j > 0 && json_get_int(items[j - 1], ORD_KEY, 0) > ck) {
                items[j] = items[j - 1];
                j--;
            }
            items[j] = cur;
        }
        for (size_t i = 0; i < kn; i++) parts->u.arr.items[pos[i]] = items[i];
        free(items);
    }
    free(pos);
}

static void strip_ord(json_t *contents) {
    json_t *c, *p;
    json_arr_foreach(contents, i, c) {
        json_arr_foreach(json_get(c, "parts"), j, p) { json_del(p, ORD_KEY); }
    }
}

static long call_ordinal(const strvec_t *ids, const char *id) {
    for (size_t i = 0; i < ids->n; i++)
        if (str_eq(ids->v[i], id)) return (long)i;
    return -1;
}

static void flush_tool_images(json_t *contents, json_t *pending) {
    if (json_len(pending) == 0) return;
    sort_known_in_place(pending);
    json_t *parts = json_arr(), *grp, *p;
    json_arr_foreach(pending, i, grp) {
        json_arr_foreach(json_get(grp, "parts"), j, p) { json_push(parts, json_clone(p)); }
    }
    json_push(contents, content_of("user", parts));
    while (json_len(pending)) json_free(json_arr_remove(pending, 0));
}

static json_t *convert_messages(const ai_model_t *m, const json_t *msgs) {
    json_t *contents = json_arr(), *pending = json_arr();
    strvec_t call_ids = {0}, call_names = {0};
    bool images = m->input_image;
    json_t *msg;
    json_arr_foreach(msgs, mi, msg) {
        const char *role = json_get_str(msg, "role");
        if (!str_eq(role, "toolResult")) flush_tool_images(contents, pending);
        if (str_eq(role, "user") || str_eq(role, "developer")) {
            json_t *c = json_get(msg, "content");
            if (json_is_str(c)) {
                if (str_blank(json_sv(c))) continue;
                json_t *parts = json_arr();
                json_push(parts, text_part(json_sv(c)));
                json_push(contents, content_of("user", parts));
                continue;
            }
            json_t *parts = json_arr(), *blk;
            bool omitted = false;
            json_arr_foreach(c, j, blk) {
                const char *ty = json_get_str(blk, "type");
                if (str_eq(ty, "text")) {
                    if (!str_blank(json_get_str(blk, "text"))) json_push(parts, text_part(json_get_str(blk, "text")));
                } else if (str_eq(ty, "image")) {
                    if (images) json_push(parts, image_part(blk));
                    else omitted = true;
                }
            }
            if (omitted) json_push(parts, text_part(NON_VISION_IMAGE_PLACEHOLDER));
            if (json_len(parts) == 0) {
                json_free(parts);
                continue;
            }
            json_push(contents, content_of("user", parts));
        } else if (str_eq(role, "assistant")) {
            bool same = str_eq(json_get_str(msg, "provider"), m->provider) && str_eq(json_get_str(msg, "model"), m->id);
            bool drop_unsigned = cflag(m, "dropUnsignedThinking");
            bool first_call = true;
            json_t *parts = json_arr(), *blk;
            json_arr_foreach(json_get(msg, "content"), j, blk) {
                const char *ty = json_get_str(blk, "type");
                if (str_eq(ty, "text")) {
                    const char *t = json_get_str(blk, "text");
                    if (str_blank(t)) continue;
                    json_t *p = text_part(t);
                    const char *sig = resolve_signature(same, json_get_str(blk, "textSignature"));
                    if (sig) json_set_str(p, "thoughtSignature", sig);
                    json_push(parts, p);
                } else if (str_eq(ty, "thinking")) {
                    const char *th = json_get_str(blk, "thinking");
                    const char *sig = resolve_signature(same, json_get_str(blk, "thinkingSignature"));
                    if (str_blank(th) && !sig) continue;
                    if (drop_unsigned && !sig) continue;
                    if (same) {
                        json_t *p = json_obj();
                        json_set_bool(p, "thought", true);
                        json_set_str(p, "text", th ? th : "");
                        if (sig) json_set_str(p, "thoughtSignature", sig);
                        json_push(parts, p);
                    } else {
                        char *d = render_demoted_thinking(m->id, th);
                        json_push(parts, text_part(d));
                        free(d);
                    }
                } else if (str_eq(ty, "toolCall")) {
                    const char *id = json_get_str(blk, "id");
                    const char *name = json_get_str(blk, "name");
                    strvec_push(&call_ids, id ? id : "");
                    strvec_push(&call_names, name ? name : "");
                    const char *sig = resolve_signature(same, json_get_str(blk, "thoughtSignature"));
                    bool fallback = cflag(m, "requiresSkipThoughtSignature") ||
                                    (first_call && cflag(m, "requiresSkipThoughtSignatureOnFirstFunctionCall"));
                    const char *eff = sig ? sig : (fallback ? SKIP_THOUGHT_SIGNATURE : NULL);
                    first_call = false;
                    json_t *fc = json_obj();
                    json_set_str(fc, "name", name ? name : "");
                    json_t *args = json_get(blk, "arguments");
                    json_set(fc, "args", json_is_obj(args) ? json_clone(args) : json_obj());
                    if (cflag(m, "supportsFunctionPartId")) json_set_str(fc, "id", id ? id : "");
                    json_t *p = json_obj();
                    json_set(p, "functionCall", fc);
                    if (eff) json_set_str(p, "thoughtSignature", eff);
                    json_push(parts, p);
                }
            }
            if (json_len(parts) == 0) {
                json_free(parts);
                continue;
            }
            json_push(contents, content_of("model", parts));
        } else if (str_eq(role, "toolResult")) {
            json_t *c = json_get(msg, "content"), *blk;
            sb_t text = SB_INIT;
            bool has_text = false, omitted = false;
            json_t *imgs = json_arr();
            json_arr_foreach(c, j, blk) {
                const char *ty = json_get_str(blk, "type");
                if (str_eq(ty, "text")) {
                    if (has_text) sb_putc(&text, '\n');
                    sb_append(&text, json_get_str(blk, "text"));
                    has_text = true;
                } else if (str_eq(ty, "image")) {
                    if (images) json_push(imgs, image_part(blk));
                    else omitted = true;
                }
            }
            has_text = text.len > 0;
            bool has_images = json_len(imgs) > 0;
            char *value;
            if (omitted) value = has_text ? xasprintf("%s\n%s", text.s, NON_VISION_IMAGE_PLACEHOLDER) : xstrdup(NON_VISION_IMAGE_PLACEHOLDER);
            else if (has_text) value = xstrdup(text.s);
            else value = xstrdup(has_images ? "(see attached image)" : "");
            sb_free(&text);
            const char *tcid = json_get_str(msg, "toolCallId");
            long ord = call_ordinal(&call_ids, tcid);
            const char *emitted = ord >= 0 ? call_names.v[ord] : NULL;
            const char *tname = json_get_str(msg, "toolName");
            json_t *fr = json_obj();
            json_set_str(fr, "name", emitted ? emitted : tname ? tname : "");
            json_t *resp = json_obj();
            json_set(resp, json_get_bool(msg, "isError", false) ? "error" : "output", json_str_own(value));
            json_set(fr, "response", resp);
            bool multimodal = cflag(m, "multimodalFunctionResponse");
            if (has_images && multimodal) json_set(fr, "parts", json_clone(imgs));
            if (cflag(m, "supportsFunctionPartId")) json_set_str(fr, "id", tcid ? tcid : "");
            json_t *part = json_obj();
            json_set(part, "functionResponse", fr);
            if (ord >= 0) json_set_int(part, ORD_KEY, ord);
            size_t nc = json_len(contents);
            json_t *last = nc ? json_at(contents, nc - 1) : NULL;
            bool merge = false;
            if (last && str_eq(json_get_str(last, "role"), "user")) {
                json_t *p;
                json_arr_foreach(json_get(last, "parts"), j, p) {
                    if (json_get(p, "functionResponse")) merge = true;
                }
            }
            if (merge) json_push(json_get(last, "parts"), part);
            else {
                json_t *parts = json_arr();
                json_push(parts, part);
                json_push(contents, content_of("user", parts));
            }
            if (has_images && !multimodal) {
                json_t *grp = json_obj(), *gp = json_arr(), *ip;
                json_push(gp, text_part("Tool result image:"));
                json_arr_foreach(imgs, j, ip) { json_push(gp, json_clone(ip)); }
                json_set(grp, "parts", gp);
                if (ord >= 0) json_set_int(grp, ORD_KEY, ord);
                json_push(pending, grp);
            }
            json_free(imgs);
        }
    }
    flush_tool_images(contents, pending);
    json_free(pending);
    json_t *c;
    json_arr_foreach(contents, i, c) { sort_known_in_place(json_get(c, "parts")); }
    strip_ord(contents);
    strvec_free(&call_ids);
    strvec_free(&call_names);
    return contents;
}

/* ======================================================================
 * request
 * ====================================================================== */

typedef struct {
    bool enabled;
    bool has_budget;
    int64_t budget;
    const char *level;
    int64_t max_tokens; /* <0 unset */
} gthink_t;

static void thinking_off(const ai_model_t *m, gthink_t *t) {
    t->enabled = false;
    if (!m->reasoning || !m->thinking) return;
    const char *mode = thinking_mode(m);
    bool suppress = thinking_flag(m, "suppressWhenOff");
    if (str_eq(mode, "budget") && (!thinking_flag(m, "requiresEffort") || suppress)) {
        t->has_budget = true;
        t->budget = 0;
    } else if (str_eq(mode, "google-level") && suppress) {
        t->level = "MINIMAL";
    }
}

static void resolve_thinking(const ai_model_t *m, const ai_stream_opts_t *o, gthink_t *t) {
    memset(t, 0, sizeof(*t));
    int64_t base = o->max_tokens > 0 ? o->max_tokens : (m->max_tokens > 0 ? m->max_tokens : -1);
    t->max_tokens = base;
    ai_effort_t e = o->reasoning;
    bool off = e == EFFORT_OFF || o->disable_reasoning;
    /* normalizeMandatoryReasoningOptions: requiresEffort models clamp "off" to their lowest effort */
    if (m->reasoning && off && thinking_flag(m, "requiresEffort") && !thinking_flag(m, "suppressWhenOff")) {
        ai_effort_t floor = minimum_effort(m);
        if (floor != EFFORT_OFF) {
            e = floor;
            off = false;
        }
    }
    if (off || !m->reasoning) {
        thinking_off(m, t);
        return;
    }
    e = clamp_effort(m, e);
    if ((int)e < EFFORT_MINIMAL || (int)e > EFFORT_MAX) {
        thinking_off(m, t);
        return;
    }
    if (str_eq(thinking_mode(m), "google-level")) {
        t->max_tokens = max_output(base, m, thinking_headroom(m, o, e));
        t->enabled = true;
        t->level = thinking_level(e, m);
        return;
    }
    bool f;
    int64_t requested = custom_budget(o, e, &f);
    if (!f) {
        requested = effort_budget_field(m, e, &f);
        if (!f) requested = -1;
    }
    int64_t maxt = max_output(base, m, requested > 0 ? requested : thinking_headroom(m, o, e));
    int64_t budget = requested;
    if (requested > 0 && maxt >= 0 && maxt <= requested) {
        budget = maxt - MIN_OUTPUT_TOKENS;
        if (budget <= 0) {
            t->max_tokens = base;
            thinking_off(m, t);
            return;
        }
    }
    t->max_tokens = maxt;
    t->enabled = true;
    t->has_budget = true;
    t->budget = budget;
}

json_t *google_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o) {
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    gthink_t th;
    resolve_thinking(m, o, &th);
    json_t *transformed = ai_transform_messages(ctx->messages, m);
    json_t *body = json_obj();
    json_set(body, "contents", convert_messages(m, transformed));
    json_free(transformed);

    json_t *sys = json_arr(), *it;
    json_arr_foreach(ctx->system_prompt, i, it) {
        if (json_is_str(it) && !str_blank(json_sv(it))) json_push(sys, text_part(json_sv(it)));
    }
    if (json_len(sys) > 0) {
        json_t *si = json_obj();
        json_set(si, "parts", sys);
        json_set(body, "systemInstruction", si);
    } else {
        json_free(sys);
    }

    bool has_tools = json_len(ctx->tools) > 0;
    if (has_tools) {
        json_t *decls = json_arr(), *t;
        bool legacy = cflag(m, "ccaLegacyParametersSchema");
        json_arr_foreach(ctx->tools, i, t) {
            json_t *d = json_obj();
            const char *name = json_get_str(t, "name"), *desc = json_get_str(t, "description");
            json_set_str(d, "name", name ? name : "");
            json_set_str(d, "description", desc ? desc : "");
            json_t *params = json_get(t, "parameters");
            json_t *ps = google_normalize_schema(params ? params : json_null());
            if (json_is_null(ps)) {
                json_free(ps);
                ps = json_obj();
                json_set_str(ps, "type", "object");
                json_set(ps, "properties", json_obj());
            }
            json_set(d, legacy ? "parameters" : "parametersJsonSchema", ps);
            json_push(decls, d);
        }
        json_t *tools = json_arr(), *fd = json_obj();
        json_set(fd, "functionDeclarations", decls);
        json_push(tools, fd);
        json_set(body, "tools", tools);
        /* toolConfig */
        json_t *tc = o->tool_choice;
        const char *mode = NULL, *name = NULL;
        if (json_is_str(tc)) {
            const char *s = json_sv(tc);
            if (!strcmp(s, "required") || !strcmp(s, "any")) mode = "ANY";
            else if (!strcmp(s, "none")) mode = "NONE";
        } else if (json_is_obj(tc)) {
            const char *ty = json_get_str(tc, "type");
            if (str_eq(ty, "tool")) name = json_get_str(tc, "name");
            else if (str_eq(ty, "function")) {
                json_t *fn = json_get_obj(tc, "function");
                name = fn ? json_get_str(fn, "name") : json_get_str(tc, "name");
            }
            if (name && *name) mode = "ANY";
            else name = NULL;
        }
        if (mode) {
            json_t *fcc = json_obj();
            json_set_str(fcc, "mode", mode);
            if (name) {
                json_t *names = json_arr();
                json_push_str(names, name);
                json_set(fcc, "allowedFunctionNames", names);
            }
            json_t *tcfg = json_obj();
            json_set(tcfg, "functionCallingConfig", fcc);
            json_set(body, "toolConfig", tcfg);
        }
    }
    if (str_eq(m->api, "google-vertex")) {
        static const char *const cats[] = {"HARM_CATEGORY_HATE_SPEECH", "HARM_CATEGORY_DANGEROUS_CONTENT",
                                           "HARM_CATEGORY_SEXUALLY_EXPLICIT", "HARM_CATEGORY_HARASSMENT"};
        json_t *ss = json_arr();
        for (size_t i = 0; i < ARRAY_LEN(cats); i++) {
            json_t *s = json_obj();
            json_set_str(s, "category", cats[i]);
            json_set_str(s, "threshold", "OFF");
            json_push(ss, s);
        }
        json_set(body, "safetySettings", ss);
    }
    if (str_eq(m->provider, "google") && (str_eq(o->service_tier, "flex") || str_eq(o->service_tier, "priority")))
        json_set_str(body, "serviceTier", o->service_tier);

    json_t *gen = json_obj();
    if (!isnan(o->temperature)) json_set_num(gen, "temperature", o->temperature);
    if (th.max_tokens > 0) json_set_int(gen, "maxOutputTokens", th.max_tokens);
    if (!isnan(o->top_p)) json_set_num(gen, "topP", o->top_p);
    if (json_is_arr(o->stop_sequences) && json_len(o->stop_sequences) > 0)
        json_set(gen, "stopSequences", json_clone(o->stop_sequences));
    if (m->reasoning && (th.enabled || th.level || th.has_budget)) {
        json_t *tc = json_obj();
        json_set_bool(tc, "includeThoughts", th.enabled && !o->hide_thinking_summary);
        if (th.level) json_set_str(tc, "thinkingLevel", th.level);
        else if (th.has_budget) json_set_int(tc, "thinkingBudget", th.budget);
        json_set(gen, "thinkingConfig", tc);
    }
    if (json_len(gen) > 0) json_set(body, "generationConfig", gen);
    else json_free(gen);
    return body;
}

char *google_request_url(const ai_model_t *m) {
    char *base = m->base_url ? str_trim(m->base_url) : xstrdup("");
    if (!*base) {
        free(base);
        base = xstrdup(DEFAULT_GEMINI_BASE);
    }
    char *u = xasprintf("%s/models/%s:streamGenerateContent?alt=sse", base, m->id);
    free(base);
    return u;
}

static char *vertex_host(const char *location) {
    if (!strcmp(location, "global")) return xstrdup("aiplatform.googleapis.com");
    if (!strcmp(location, "eu") || !strcmp(location, "us")) return xasprintf("aiplatform.%s.rep.googleapis.com", location);
    return xasprintf("%s-aiplatform.googleapis.com", location);
}

static const char *env_first(const char *a, const char *b, const char *c) {
    const char *v = env_get(a);
    if (!v && b) v = env_get(b);
    if (!v && c) v = env_get(c);
    return v;
}

/* ======================================================================
 * SSE state machine (consumeGoogleStream / GoogleTextBlocks)
 * ====================================================================== */

const char *google_map_finish_reason(const char *r) {
    if (str_eq(r, "STOP")) return "stop";
    if (str_eq(r, "MAX_TOKENS")) return "length";
    return "error";
}

void google_sse_init(google_sse_t *s, const ai_model_t *m, ai_event_cb cb, void *ud) {
    memset(s, 0, sizeof(*s));
    aib_init(&s->b, m, cb, ud);
    s->model = m;
    s->stop_reason = "stop";
}

void google_sse_free(google_sse_t *s) {
    free(s->error);
    s->error = NULL;
    aib_free(&s->b);
}

static json_t *gcontent(google_sse_t *s) { return json_get(s->b.msg, "content"); }

static void sign_block(google_sse_t *s, int ci, const char *sig) {
    if (!sig || !*sig || ci < 0) return;
    json_t *blk = json_at(gcontent(s), (size_t)ci);
    const char *ty = json_get_str(blk, "type");
    json_set_str(blk, str_eq(ty, "thinking") ? "thinkingSignature" : "textSignature", sig);
}

static bool has_tool_call(google_sse_t *s) {
    json_t *b;
    json_arr_foreach(gcontent(s), i, b) {
        if (str_eq(json_get_str(b, "type"), "toolCall")) return true;
    }
    return false;
}

static void push_text_part(google_sse_t *s, const json_t *part) {
    json_t *tv = json_get(part, "text");
    if (!json_is_str(tv)) return;
    bool thinking = json_get_bool(part, "thought", false);
    const char *sig = json_get_str(part, "thoughtSignature");
    if (tv->u.str.len == 0) {
        if (!sig || !*sig || json_get(part, "functionCall")) return;
        int open_kind = s->b.cur_index >= 0 ? s->b.cur_kind : 0;
        if (thinking && open_kind != 2) {
            /* a signature-only thought part signs thinking of its own */
            aib_start(&s->b);
            aib_end_block(&s->b);
            json_t *tb = json_obj();
            json_set_str(tb, "type", "thinking");
            json_set_str(tb, "thinking", "");
            json_set_str(tb, "thinkingSignature", sig);
            json_push(gcontent(s), tb);
        } else if (open_kind == 1 || open_kind == 2) {
            sign_block(s, s->b.cur_index, sig);
        }
        return;
    }
    if (!s->b.t_first_token) s->b.t_first_token = mono_ms();
    if (thinking) aib_thinking_delta(&s->b, tv->u.str.s, tv->u.str.len);
    else aib_text_delta(&s->b, tv->u.str.s, tv->u.str.len);
    sign_block(s, s->b.cur_index, sig);
}

static void push_function_call(google_sse_t *s, const json_t *part) {
    json_t *fc = json_get_obj(part, "functionCall");
    aib_end_block(&s->b);
    const char *name = json_get_str(fc, "name");
    const char *id = json_get_str(fc, "id");
    bool need_new = !id || !*id;
    if (!need_new) {
        json_t *b;
        json_arr_foreach(gcontent(s), i, b) {
            if (str_eq(json_get_str(b, "type"), "toolCall") && str_eq(json_get_str(b, "id"), id)) need_new = true;
        }
    }
    static unsigned long counter;
    char *gen = need_new ? xasprintf("%s_%lld_%lu", name && *name ? name : "tool", (long long)now_ms(), ++counter) : NULL;
    aib_toolcall_start(&s->b, gen ? gen : id, name ? name : "");
    free(gen);
    json_t *args = json_get_obj(fc, "args");
    aib_toolcall_set_args(&s->b, args ? json_clone(args) : json_obj());
    const char *sig = json_get_str(part, "thoughtSignature");
    if (sig && *sig) aib_toolcall_set_field(&s->b, "thoughtSignature", sig);
    aib_end_block(&s->b);
}

static void apply_usage(google_sse_t *s, const json_t *u) {
    int64_t cand = json_get_int(u, "candidatesTokenCount", 0), thoughts = json_get_int(u, "thoughtsTokenCount", 0);
    int64_t total = json_get_int(u, "totalTokenCount", 0);
    int64_t prompt = json_get_int(u, "promptTokenCount", 0);
    if (!prompt) prompt = MAX(0, total - cand - thoughts);
    int64_t cached = MIN(json_get_int(u, "cachedContentTokenCount", 0), prompt);
    aib_usage(&s->b, prompt - cached, cand + thoughts, cached, 0);
    json_t *usage = json_get(s->b.msg, "usage");
    if (thoughts > 0) aib_usage_add_reasoning(&s->b, thoughts);
    else json_del(usage, "reasoningTokens");
}

int google_sse_event(void *ud, const char *event, const char *data, size_t len) {
    google_sse_t *s = ud;
    json_t *chunk = json_parse(data, len, NULL);
    if (!json_is_obj(chunk)) {
        json_free(chunk);
        return 0;
    }
    aib_start(&s->b);
    json_t *err = json_get_obj(chunk, "error");
    if (err) {
        const char *msg = json_get_str(err, "message"), *status = json_get_str(err, "status");
        char *detail = msg && status ? xasprintf("%s (%s)", msg, status) : xstrdup(msg ? msg : status ? status : "unknown error");
        if (!s->error) s->error = xasprintf("Google API stream error: %s", detail);
        free(detail);
        json_free(chunk);
        return 2;
    }
    json_t *cands = json_get_arr(chunk, "candidates");
    json_t *pf = json_get_obj(chunk, "promptFeedback");
    if (json_len(cands) == 0 && pf && json_get_str(pf, "blockReason")) {
        const char *detail = json_get_str(pf, "blockReasonMessage");
        if (!s->error)
            s->error = xasprintf("Request blocked by Google (%s)%s%s", json_get_str(pf, "blockReason"), detail ? ": " : "",
                                 detail ? detail : "");
        json_free(chunk);
        return 2;
    }
    json_t *cand = json_at(cands, 0);
    json_t *content = cand ? json_get_obj(cand, "content") : NULL;
    json_t *part;
    json_arr_foreach(content ? json_get(content, "parts") : NULL, i, part) {
        push_text_part(s, part);
        if (json_get_obj(part, "functionCall")) push_function_call(s, part);
    }
    const char *fr = cand ? json_get_str(cand, "finishReason") : NULL;
    if (fr) {
        s->saw_finish = true;
        const char *mapped = google_map_finish_reason(fr);
        if ((!strcmp(mapped, "stop") || !strcmp(mapped, "length")) && has_tool_call(s)) {
            s->stop_reason = "toolUse";
        } else {
            s->stop_reason = mapped;
            if (!strcmp(mapped, "error")) {
                free(s->error);
                s->error = xasprintf("Generation failed with finish reason: %s", fr);
            }
        }
    }
    json_t *um = json_get_obj(chunk, "usageMetadata");
    if (um) apply_usage(s, um);
    json_free(chunk);
    return 0;
}

static bool meaningful(google_sse_t *s) {
    json_t *b;
    json_arr_foreach(gcontent(s), i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "toolCall")) return true;
        if (str_eq(t, "text") && !str_blank(json_get_str(b, "text"))) return true;
    }
    return false;
}

bool google_sse_empty_stop(const google_sse_t *s, int rc) {
    google_sse_t *ms = (google_sse_t *)s;
    aib_end_block(&ms->b);
    return (rc == 0 || rc == 1) && !s->error && s->saw_finish && str_eq(s->stop_reason, "stop") && !meaningful(ms);
}

json_t *google_sse_finish(google_sse_t *s, int rc, const char *err) {
    if (rc == -2) return aib_finish(&s->b, "aborted", err ? err : "Request was aborted");
    if (s->error) return aib_finish(&s->b, "error", s->error);
    if (rc < 0 || rc > 2) return aib_finish(&s->b, "error", err ? err : "request failed");
    if (!s->saw_finish)
        return aib_finish(&s->b, "error",
                          "Google API stream ended without a finish reason (connection dropped or response truncated)");
    return aib_finish(&s->b, s->stop_reason, NULL);
}

/* ======================================================================
 * entry point
 * ====================================================================== */

/* Google error bodies: keep `error.message` (plus RPC status/details on usage-limit statuses). Owned. */
static char *google_error_text(int status, const char *err) {
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%d ", status);
    if (err && str_starts(err, prefix)) err += strlen(prefix); /* ai_extract_error prefixes the status */
    return xasprintf("Google API error (%d): %s", status, err && *err ? err : "Unknown error");
}

json_t *provider_google_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb,
                               void *ud) {
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    bool vertex = str_eq(m->api, "google-vertex");
    char *env_key = NULL;
    const char *key = o->api_key;
    if (key && (key[0] == '<' || !strcmp(key, "N/A"))) key = NULL;
    if (!key || !*key) key = env_key = vertex ? (env_get("GOOGLE_CLOUD_API_KEY") ? xstrdup(env_get("GOOGLE_CLOUD_API_KEY")) : NULL)
                                              : ai_env_api_key(m->provider);
    char *url = NULL, *fallback_url = NULL, *cfg_err = NULL;
    http_headers_t h = {0};
    ai_apply_headers(&h, m, o);
    if (!vertex) {
        if (!key || !*key) cfg_err = xstrdup("Google Generative AI requires an API key (GEMINI_API_KEY or options.apiKey).");
        else {
            url = google_request_url(m);
            http_headers_set(&h, "x-goog-api-key", key);
        }
    } else {
        if (str_eq(o->service_tier, "priority")) http_headers_set(&h, "X-Vertex-AI-LLM-Shared-Request-Type", "priority");
        const char *loc = env_first("GOOGLE_VERTEX_LOCATION", "GOOGLE_CLOUD_LOCATION", "VERTEX_LOCATION");
        bool access_token = key && str_starts(key, "ya29.");
        if (key && *key && !access_token) {
            const char *location = loc ? loc : "global";
            char *host = vertex_host(location);
            char *path = xasprintf("v1/publishers/google/models/%s:streamGenerateContent?alt=sse", m->id);
            url = xasprintf("https://%s/%s", host, path);
            if (loc && strcmp(host, "aiplatform.googleapis.com") != 0)
                fallback_url = xasprintf("https://aiplatform.googleapis.com/%s", path);
            http_headers_set(&h, "x-goog-api-key", key);
            free(host);
            free(path);
        } else if (access_token) {
            const char *project = env_first("GOOGLE_CLOUD_PROJECT", "GCP_PROJECT", "GCLOUD_PROJECT");
            if (!project)
                cfg_err = xstrdup("Vertex AI requires a project ID. Set GOOGLE_CLOUD_PROJECT/GCP_PROJECT/GCLOUD_PROJECT or pass project in options.");
            else if (!loc)
                cfg_err = xstrdup("Vertex AI requires a location. Set GOOGLE_VERTEX_LOCATION/GOOGLE_CLOUD_LOCATION/VERTEX_LOCATION or pass location in options.");
            else {
                char *host = vertex_host(loc);
                url = xasprintf("https://%s/v1/projects/%s/locations/%s/publishers/google/models/%s:streamGenerateContent?alt=sse",
                                host, project, loc, m->id);
                free(host);
                char *bearer = xasprintf("Bearer %s", key);
                http_headers_set(&h, "Authorization", bearer);
                free(bearer);
            }
        } else {
            cfg_err = xstrdup("Vertex AI requires GOOGLE_CLOUD_API_KEY or an OAuth access token (ADC is not supported yet).");
        }
    }
    if (cfg_err) {
        ai_builder_t b;
        aib_init(&b, m, cb, ud);
        json_t *msg = aib_finish(&b, "error", cfg_err);
        aib_free(&b);
        free(cfg_err);
        free(env_key);
        http_headers_free(&h);
        return msg;
    }
    json_t *params = google_build_request(m, ctx, o);
    if (o->on_payload) o->on_payload(o->payload_ud, params);
    char *body = json_dump(params, 0);
    json_free(params);

    google_sse_t s;
    google_sse_init(&s, m, cb, ud);
    json_t *result = NULL;
    for (int attempt = 0;; attempt++) {
        int status = 0;
        char *err = NULL;
        int rc = ai_sse_post(url, &h, body, o, google_sse_event, &s, &status, &err);
        if (rc == -1 && status == 404 && fallback_url && !s.b.started) {
            free(err);
            err = NULL;
            rc = ai_sse_post(fallback_url, &h, body, o, google_sse_event, &s, &status, &err);
        }
        if (rc == -1 && status >= 400) {
            char *e2 = google_error_text(status, err);
            free(err);
            err = e2;
        }
        if (google_sse_empty_stop(&s, rc)) {
            if (attempt >= MAX_EMPTY_STREAM_RETRIES) {
                free(s.error);
                s.error = xasprintf("Google API returned an empty response (finishReason STOP with no content) after %d attempts",
                                    MAX_EMPTY_STREAM_RETRIES + 1);
                result = google_sse_finish(&s, rc, err);
                free(err);
                break;
            }
            free(err);
            int64_t until = mono_ms() + ((int64_t)EMPTY_STREAM_BASE_DELAY_MS << attempt);
            bool cancelled = false;
            while (mono_ms() < until) {
                if (o->cancel && *o->cancel) {
                    cancelled = true;
                    break;
                }
                sleep_ms(20);
            }
            if (cancelled) {
                result = google_sse_finish(&s, -2, NULL);
                break;
            }
            /* restart with a clean message; "start" was already emitted */
            google_sse_free(&s);
            google_sse_init(&s, m, cb, ud);
            s.b.started = true;
            continue;
        }
        result = google_sse_finish(&s, rc, err);
        free(err);
        break;
    }
    google_sse_free(&s);
    free(body);
    free(url);
    free(fallback_url);
    free(env_key);
    http_headers_free(&h);
    return result;
}
