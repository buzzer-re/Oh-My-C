/* schema_ts.c - JSON schema rendered as a TypeScript type (port of ai/utils/schema/typescript.ts). */
#include "schema_ts.h"

#include <ctype.h>

/* ---------- JSON schema -> TypeScript ---------- */

typedef struct {
    const json_t *defs;
    int depth;
    bool harmony; /* OpenAI-Harmony style: `//` comments, `,` delimiters, no indentation */
} ts_ctx_t;

static void ts_convert(sb_t *b, const json_t *node, ts_ctx_t *c, const char *pad);

static bool safe_key(const char *k) {
    if (!*k || !(isalpha((unsigned char)*k) || *k == '_' || *k == '$')) return false;
    for (const char *p = k + 1; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '$')) return false;
    return true;
}

static char *ts_str(const json_t *node, ts_ctx_t *c, const char *pad) {
    sb_t b = SB_INIT;
    ts_convert(&b, node, c, pad);
    return sb_detach(&b);
}

static char *ts_literal(const json_t *v) {
    if (json_is_str(v) || json_is_num(v) || json_is_bool(v) || json_is_null(v)) return json_dump(v, 0);
    return json_dump(v, 0);
}

/* join union members, deduping identical renders */
static void ts_join_union(sb_t *b, strvec_t *parts) {
    strvec_t uniq = {0};
    for (size_t i = 0; i < parts->n; i++)
        if (!strvec_contains(&uniq, parts->v[i])) strvec_push(&uniq, parts->v[i]);
    if (!uniq.n) sb_append(b, "never");
    for (size_t i = 0; i < uniq.n; i++) {
        if (i) sb_append(b, " | ");
        sb_append(b, uniq.v[i]);
    }
    strvec_free(&uniq);
}

static void ts_description(sb_t *body, const char *desc, const char *pad, bool harmony) {
    if (harmony) {
        for (const char *p = desc;;) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            sb_t l = SB_INIT;
            sb_appendf(&l, "%s// ", pad);
            sb_appendn(&l, p, n);
            while (l.len && (l.s[l.len - 1] == ' ' || l.s[l.len - 1] == '\t' || l.s[l.len - 1] == '\r')) l.s[--l.len] = 0;
            sb_append(body, sb_cstr(&l));
            sb_putc(body, '\n');
            sb_free(&l);
            if (!nl) break;
            p = nl + 1;
        }
        return;
    }
    char *safe = str_replace(desc, "*/", "* /");
    if (!strchr(safe, '\n')) {
        sb_appendf(body, "%s/** %s */\n", pad, safe);
    } else {
        sb_appendf(body, "%s/**\n", pad);
        const char *p = safe;
        for (;;) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            sb_appendf(body, "%s * ", pad);
            sb_appendn(body, p, n);
            sb_putc(body, '\n');
            if (!nl) break;
            p = nl + 1;
        }
        sb_appendf(body, "%s */\n", pad);
    }
    free(safe);
}

static void ts_array(sb_t *b, const json_t *node, ts_ctx_t *c, const char *pad) {
    json_t *prefix = json_get_arr(node, "prefixItems");
    if (prefix) {
        sb_putc(b, '[');
        json_t *it;
        json_arr_foreach(prefix, i, it) {
            if (i) sb_append(b, ", ");
            ts_convert(b, it, c, pad);
        }
        sb_putc(b, ']');
        return;
    }
    json_t *items = json_get(node, "items");
    if (!items || (json_is_bool(items) && items->u.b)) {
        sb_append(b, "unknown[]");
        return;
    }
    if (json_is_bool(items)) {
        sb_append(b, "never[]");
        return;
    }
    char *inner = ts_str(items, c, pad);
    if (strchr(inner, '\n') || strstr(inner, " | ") || utf8_strlen(inner) > 40) sb_appendf(b, "Array<%s>", inner);
    else sb_appendf(b, "%s[]", inner);
    free(inner);
}

static void ts_object(sb_t *b, const json_t *node, ts_ctx_t *c, const char *pad) {
    json_t *props = json_get_obj(node, "properties");
    json_t *additional = json_get(node, "additionalProperties");
    char *child_pad = xasprintf("%s%s", pad, c->harmony ? "" : "  ");
    const char *delim = c->harmony ? "," : ";";
    sb_t body = SB_INIT;
    size_t count = 0;
    if (props) {
        json_t *req = json_get_arr(node, "required");
        const char *k;
        json_t *v;
        json_obj_foreach(props, i, k, v) {
            const char *d = json_get_str(v, "description");
            if (json_is_obj(v) && d && *d) ts_description(&body, d, child_pad, c->harmony);
            bool required = false;
            json_t *r;
            json_arr_foreach(req, j, r) if (str_eq(json_sv(r), k)) required = true;
            char *val = ts_str(v, c, child_pad);
            if (safe_key(k)) sb_appendf(&body, "%s%s%s: %s%s\n", child_pad, k, required ? "" : "?", val, delim);
            else {
                char *q = json_quote(k);
                sb_appendf(&body, "%s%s%s: %s%s\n", child_pad, q, required ? "" : "?", val, delim);
                free(q);
            }
            free(val);
            count++;
        }
    }
    if (!count) {
        sb_free(&body);
        free(child_pad);
        if (json_is_obj(additional)) {
            char *inner = ts_str(additional, c, pad);
            sb_appendf(b, "Record<string, %s>", inner);
            free(inner);
        } else if (json_is_bool(additional) && additional->u.b) {
            sb_append(b, "Record<string, unknown>");
        } else {
            sb_append(b, "{}");
        }
        return;
    }
    if (json_is_obj(additional)) {
        char *inner = ts_str(additional, c, child_pad);
        sb_appendf(&body, "%s[key: string]: %s%s\n", child_pad, inner, delim);
        free(inner);
    }
    sb_append(b, "{\n");
    sb_append(b, body.s);
    sb_appendf(b, "%s}", pad);
    sb_free(&body);
    free(child_pad);
}

static void ts_type(sb_t *b, const char *type, const json_t *node, ts_ctx_t *c, const char *pad) {
    if (str_eq(type, "string")) sb_append(b, "string");
    else if (str_eq(type, "integer") || str_eq(type, "number")) sb_append(b, "number");
    else if (str_eq(type, "boolean")) sb_append(b, "boolean");
    else if (str_eq(type, "null")) sb_append(b, "null");
    else if (str_eq(type, "array")) ts_array(b, node, c, pad);
    else if (str_eq(type, "object")) ts_object(b, node, c, pad);
    else sb_append(b, "unknown");
}

static void ts_convert(sb_t *b, const json_t *node, ts_ctx_t *c, const char *pad) {
    if (json_is_bool(node)) {
        sb_append(b, node->u.b ? "unknown" : "never");
        return;
    }
    if (!json_is_obj(node) || c->depth > 32) {
        sb_append(b, "unknown");
        return;
    }
    c->depth++;
    const char *ref = json_get_str(node, "$ref");
    if (ref) {
        const char *name = strrchr(ref, '/');
        name = name ? name + 1 : ref;
        const json_t *resolved = NULL;
        if ((str_starts(ref, "#/$defs/") || str_starts(ref, "#/definitions/")) && c->defs)
            resolved = json_get_obj(c->defs, name);
        if (resolved && c->depth < 16) ts_convert(b, resolved, c, pad);
        else sb_append(b, name);
        c->depth--;
        return;
    }
    json_t *cv = json_get(node, "const");
    if (cv) {
        char *l = ts_literal(cv);
        sb_append(b, l);
        free(l);
        c->depth--;
        return;
    }
    json_t *en = json_get_arr(node, "enum");
    if (en) {
        strvec_t parts = {0};
        json_t *e;
        json_arr_foreach(en, i, e) strvec_push_own(&parts, ts_literal(e));
        ts_join_union(b, &parts);
        strvec_free(&parts);
        c->depth--;
        return;
    }
    json_t *un = json_get_arr(node, "anyOf");
    if (!un) un = json_get_arr(node, "oneOf");
    if (un) {
        strvec_t parts = {0};
        json_t *e;
        json_arr_foreach(un, i, e) strvec_push_own(&parts, ts_str(e, c, pad));
        ts_join_union(b, &parts);
        strvec_free(&parts);
        c->depth--;
        return;
    }
    json_t *all = json_get_arr(node, "allOf");
    if (all) {
        json_t *e;
        json_arr_foreach(all, i, e) {
            if (i) sb_append(b, " & ");
            ts_convert(b, e, c, pad);
        }
        c->depth--;
        return;
    }
    json_t *type = json_get(node, "type");
    if (json_is_arr(type)) {
        strvec_t parts = {0};
        json_t *e;
        json_arr_foreach(type, i, e) {
            sb_t t = SB_INIT;
            ts_type(&t, json_sv(e) ? json_sv(e) : "", node, c, pad);
            strvec_push_own(&parts, sb_detach(&t));
        }
        ts_join_union(b, &parts);
        strvec_free(&parts);
    } else if (json_is_str(type)) {
        ts_type(b, json_sv(type), node, c, pad);
    } else {
        sb_append(b, "unknown");
    }
    c->depth--;
}

char *json_schema_to_ts(const json_t *schema) { return json_schema_to_ts_style(schema, false); }

char *json_schema_to_ts_style(const json_t *schema, bool harmony) {
    ts_ctx_t c = {0};
    c.harmony = harmony;
    if (json_is_obj(schema)) {
        c.defs = json_get_obj(schema, "$defs");
        if (!c.defs) c.defs = json_get_obj(schema, "definitions");
    }
    return ts_str(schema, &c, "");
}

