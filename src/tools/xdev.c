/* xdev.c - `xd://` virtual tool devices (port of tools/xdev.ts, internal-urls/xd-protocol.ts,
 * tools/report-tool-issue.ts device surface, ai/utils/schema/typescript.ts). */
#include "xdev.h"

#include "config/settings.h"
#include "core/fs.h"
#include "prompt/template.h"
#include "tools.h"

#include <ctype.h>
#include <strings.h>
#include <time.h>

#define XD "xd://"
#define DOCS_TOTAL_BUDGET 48000
#define DOCS_PER_DEVICE_CAP 10000
#define EXTERNAL_DESCRIPTION_CAP 200
#define REPORT_ISSUE "report_issue"

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

/* ---------- device registry views ---------- */

size_t xdev_mounted_count(agent_t *a) {
    size_t n = 0;
    for (size_t i = 0; i < a->all_tools.n; i++)
        if (a->all_tools.v[i]->mounted) n++;
    return n;
}

static bool is_active(agent_t *a, const agent_tool_t *t) {
    for (size_t i = 0; i < a->tools.n; i++)
        if (a->tools.v[i] == t) return true;
    return false;
}

/* a tool's customWireName: `edit` goes on the wire as `apply_patch` in apply_patch mode (edit/index.ts) */
static const char *wire_alias(const agent_tool_t *t) {
    return str_eq(t->name, "edit") && str_eq(tool_edit_mode_setting(), "apply_patch") ? "apply_patch" : NULL;
}

/* resolveXdevTool: a mounted or active top-level tool by canonical name or unambiguous wire alias; an exact registry
 * name shadows aliases even when that tool is disabled (omp 3eed7ca5ae) */
static agent_tool_t *resolve_tool(agent_t *a, const char *name) {
    agent_tool_t *hit = NULL;
    for (size_t i = 0; i < a->all_tools.n && !hit; i++)
        if (strcmp(a->all_tools.v[i]->name, name) == 0) hit = a->all_tools.v[i];
    if (!hit) {
        for (size_t i = 0; i < a->all_tools.n; i++) {
            agent_tool_t *t = a->all_tools.v[i];
            if (!str_eq(wire_alias(t), name)) continue;
            if (hit) return NULL; /* claimed by more than one tool */
            hit = t;
        }
    }
    return hit && (hit->mounted || is_active(a, hit)) ? hit : NULL;
}

agent_tool_t *xdev_resolve_tool(agent_t *a, const char *name) { return resolve_tool(a, name); }

/* MCP/extension tools carry third-party metadata (`dynamic`): their summaries are capped. */
static bool tool_is_builtin(const agent_tool_t *t) {
    return !str_starts(t->name, "mcp__") && !str_eq(t->name, "generate_image") && !str_eq(t->name, "tts");
}

/* summary, else first non-empty description line, else label/name */
static char *tool_summary(const agent_tool_t *t) {
    if (t->summary && *t->summary) return xstrdup(t->summary);
    const char *d = t->description ? t->description : "";
    while (*d) {
        const char *nl = strchr(d, '\n');
        size_t n = nl ? (size_t)(nl - d) : strlen(d);
        char *line = xstrndup(d, n);
        char *tr = str_trim(line);
        free(line);
        if (*tr) return tr;
        free(tr);
        if (!nl) break;
        d = nl + 1;
    }
    return xstrdup(t->label ? t->label : t->name);
}

/* strip control chars, collapse to one line, bound in UTF-8 bytes with an ellipsis */
static char *catalog_summary(const agent_tool_t *t, size_t max_bytes) {
    char *s = tool_summary(t);
    sb_t b = SB_INIT;
    bool in_ctrl = false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        bool ctrl = *p < 0x20 || *p == 0x7f;
        /* U+0080-U+009F (C2 80..C2 9F) and U+2028/U+2029 (E2 80 A8/A9) */
        size_t skip = 0;
        if (p[0] == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) ctrl = true, skip = 1;
        else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)) ctrl = true, skip = 2;
        if (ctrl) {
            if (!in_ctrl) sb_putc(&b, ' ');
            in_ctrl = true;
            p += skip;
            continue;
        }
        in_ctrl = false;
        sb_putc(&b, (char)*p);
    }
    free(s);
    char *raw = sb_detach(&b);
    char *cleaned = str_trim(raw);
    free(raw);
    if (max_bytes && strlen(cleaned) > max_bytes) {
        size_t cut = max_bytes - 3; /* "…" is 3 bytes */
        while (cut > 0 && ((unsigned char)cleaned[cut] & 0xC0) == 0x80) cut--;
        cleaned[cut] = 0;
        size_t n = strlen(cleaned);
        while (n && isspace((unsigned char)cleaned[n - 1])) cleaned[--n] = 0;
        char *r = xasprintf("%s…", cleaned);
        free(cleaned);
        cleaned = r;
    }
    if (!*cleaned) {
        free(cleaned);
        cleaned = xstrdup(t->name);
    }
    return cleaned;
}

json_t *xdev_entries(agent_t *a) {
    json_t *arr = json_arr();
    for (size_t i = 0; i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        if (!t->mounted) continue;
        bool dynamic = !tool_is_builtin(t);
        json_t *e = json_obj();
        json_set_str(e, "name", t->name);
        json_set(e, "summary", json_str_own(catalog_summary(t, dynamic ? EXTERNAL_DESCRIPTION_CAP : 0)));
        json_set_bool(e, "dynamic", dynamic);
        json_push(arr, e);
    }
    return arr;
}

char *xdev_listing(agent_t *a) {
    sb_t b = SB_INIT;
    json_t *ents = xdev_entries(a);
    sb_appendf(&b, XD " %zu mounted tool devices.\n", json_len(ents));
    json_t *e;
    json_arr_foreach(ents, i, e) {
        const char *n = json_get_str(e, "name");
        sb_appendf(&b, XD "%-14s %s\n", n, json_get_str(e, "summary"));
    }
    sb_append(&b, "\nRead " XD "<tool> for docs + JSON schema; write the JSON args object to " XD
                  "<tool> to execute. Active top-level tools accept the same dispatch.");
    json_free(ents);
    return sb_detach(&b);
}

static char *render_docs(const agent_tool_t *t, const char *heading, size_t description_cap) {
    char *schema = json_schema_to_ts(t->parameters);
    char *desc = xstrdup(t->description ? t->description : "");
    if (description_cap && strlen(desc) > description_cap) {
        /* JS slices UTF-16 units; approximate with a code-point boundary */
        size_t cut = description_cap;
        while (cut > 0 && ((unsigned char)desc[cut] & 0xC0) == 0x80) cut--;
        desc[cut] = 0;
        size_t n = strlen(desc);
        while (n && isspace((unsigned char)desc[n - 1])) desc[--n] = 0;
        char *r = xasprintf("%s… (full docs: read " XD "%s)", desc, t->name);
        free(desc);
        desc = r;
    }
    sb_t b = SB_INIT;
    sb_appendf(&b, "%s %s", heading, t->name);
    if (t->label && *t->label) sb_appendf(&b, " — %s", t->label);
    sb_appendf(&b, "\n\n%s\n\n%s# Schema\n```ts\ntype Args = %s;\n```\nExecute by writing JSON to " XD "%s.", desc,
               heading, schema, t->name);
    free(schema);
    free(desc);
    return sb_detach(&b);
}

static bool glob_match_simple(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (const char *p = s;; p++) {
            if (glob_match_simple(pat + 1, p)) return true;
            if (!*p) return false;
        }
    }
    if (*pat == '?') return *s && glob_match_simple(pat + 1, s + 1);
    return *s == *pat && glob_match_simple(pat + 1, s + 1);
}

static bool should_inline(const agent_tool_t *t, const char *mode) {
    if (str_eq(mode, "catalog")) return false;
    if (str_eq(mode, "inline") || tool_is_builtin(t)) return true;
    json_t *globs = settings_get("tools.xdevInlineDevices"), *g;
    json_arr_foreach(globs, i, g) if (json_sv(g) && *json_sv(g) && glob_match_simple(json_sv(g), t->name)) return true;
    return false;
}

/* planXdevPromptDocs: inlined docs sections, then catalog entries ({name: summary}, presentation order) */
static void plan_docs(agent_t *a, sb_t *sections, json_t *catalog) {
    const char *mode = settings_str("tools.xdevDocs", "catalog");
    size_t used = 0;
    for (size_t i = 0; i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        if (!t->mounted) continue;
        size_t cap = tool_is_builtin(t) ? 0 : EXTERNAL_DESCRIPTION_CAP;
        if (should_inline(t, mode)) {
            char *docs = render_docs(t, "##", cap);
            size_t n = strlen(docs); /* JS length is UTF-16 units; bytes are a conservative bound */
            if (n <= DOCS_PER_DEVICE_CAP && used + n <= DOCS_TOTAL_BUDGET) {
                used += n;
                if (sections->len) sb_append(sections, "\n\n");
                sb_append(sections, docs);
                free(docs);
                continue;
            }
            free(docs);
        }
        json_set(catalog, t->name, json_str_own(catalog_summary(t, cap)));
    }
}

/* renderXdevPromptDocs: devices in `listed_elsewhere` get no catalog line (the MCP route guidance lists them) */
char *xdev_prompt_docs_ex(agent_t *a, const strvec_t *listed_elsewhere) {
    sb_t sections = SB_INIT, lines = SB_INIT;
    json_t *catalog = json_obj();
    plan_docs(a, &sections, catalog);
    const char *k;
    json_t *v;
    json_obj_foreach(catalog, i, k, v) {
        if (listed_elsewhere && strvec_contains(listed_elsewhere, k)) continue;
        sb_appendf(&lines, "- %s — %s\n", k, json_sv(v));
    }
    json_free(catalog);
    if (!lines.len) {
        sb_free(&lines);
        return sb_detach(&sections);
    }
    sb_t out = SB_INIT;
    if (sections.len) {
        sb_append(&out, sections.s);
        sb_append(&out, "\n\n");
    }
    sb_append(&out, "## Additional devices (docs on demand)\n");
    sb_append(&out, lines.s);
    sb_append(&out, "\nRead " XD "<tool> for full docs + JSON schema before first use.");
    sb_free(&sections);
    sb_free(&lines);
    return sb_detach(&out);
}

char *xdev_prompt_docs(agent_t *a) { return xdev_prompt_docs_ex(a, NULL); }

/* xdevDocsFor: inlined docs of the named mounted devices under the prompt-doc policy */
char *xdev_docs_for(agent_t *a, const strvec_t *names) {
    const char *mode = settings_str("tools.xdevDocs", "catalog");
    sb_t sections = SB_INIT;
    size_t used = 0;
    for (size_t k = 0; names && k < names->n; k++) {
        agent_tool_t *t = NULL;
        for (size_t i = 0; i < a->all_tools.n && !t; i++)
            if (a->all_tools.v[i]->mounted && str_eq(a->all_tools.v[i]->name, names->v[k])) t = a->all_tools.v[i];
        if (!t || !should_inline(t, mode)) continue;
        char *docs = render_docs(t, "##", tool_is_builtin(t) ? 0 : EXTERNAL_DESCRIPTION_CAP);
        size_t n = strlen(docs);
        if (n <= DOCS_PER_DEVICE_CAP && used + n <= DOCS_TOTAL_BUDGET) {
            used += n;
            if (sections.len) sb_append(&sections, "\n\n");
            sb_append(&sections, docs);
        }
        free(docs);
    }
    return sb_detach(&sections);
}

/* formatMCPXdevGuidanceLabel: JSON.stringify with backtick, U+2028 and U+2029 escaped */
static char *route_label(const char *name) {
    sb_t q = SB_INIT;
    json_js_quote_sb(&q, name, strlen(name));
    char *enc = sb_detach(&q);
    sb_t b = SB_INIT;
    for (const char *p = enc; *p; p++) {
        if (*p == '`') sb_append(&b, "\\u0060");
        else if (!strncmp(p, "\xe2\x80\xa8", 3)) sb_append(&b, "\\u2028"), p += 2;
        else if (!strncmp(p, "\xe2\x80\xa9", 3)) sb_append(&b, "\\u2029"), p += 2;
        else sb_putc(&b, *p);
    }
    free(enc);
    return sb_detach(&b);
}

static size_t js_len(const char *s) { /* UTF-16 length */
    size_t u = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) u += *p >= 0xF0 ? 2 : 1;
    return u;
}

#define MCP_ROUTE_DATA_BUDGET 4000
#define MCP_ROUTE_MAX_MAPPINGS 64

char *xdev_mcp_route_guidance(agent_t *a, strvec_t *routed_catalog) {
    sb_t sec = SB_INIT;
    json_t *catalog = json_obj();
    plan_docs(a, &sec, catalog);
    sb_free(&sec);
    json_t *tools = json_arr();
    size_t remaining = MCP_ROUTE_DATA_BUDGET, n = 0;
    bool omitted = false, catalog_only = false;
    for (size_t i = 0; i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        if (!t->mounted || !t->mcp_server || !t->mcp_tool) continue;
        size_t raw = js_len(t->mcp_tool) + strlen(XD) + js_len(t->name);
        if (n >= MCP_ROUTE_MAX_MAPPINGS || raw > remaining) {
            omitted = true;
            continue;
        }
        char *label = route_label(t->mcp_tool);
        char *path = xasprintf(XD "%s", t->name);
        size_t len = js_len(label) + js_len(path);
        if (len > remaining) {
            omitted = true;
            free(label);
            free(path);
            continue;
        }
        remaining -= len;
        n++;
        json_t *e = json_obj();
        json_set(e, "mcpToolName", json_str_own(label));
        json_set(e, "path", json_str_own(path));
        const char *summary = json_get_str(catalog, t->name);
        if (summary) {
            json_set_str(e, "summary", summary);
            catalog_only = true;
            if (routed_catalog) strvec_push(routed_catalog, t->name);
        }
        json_push(tools, e);
    }
    json_free(catalog);
    if (!n && !omitted) {
        json_free(tools);
        return NULL;
    }
    json_t *ctx = json_obj();
    json_set(ctx, "tools", tools);
    json_set_bool(ctx, "hasCatalogOnlyTools", catalog_only);
    json_set_bool(ctx, "hasOmittedTools", omitted);
    char *r = prompt_render("coding-agent/prompts/system/mcp-xdev-guidance.md", ctx);
    json_free(ctx);
    char *t = str_trim(r);
    free(r);
    return t;
}

/* ---------- report_issue (automated QA) ---------- */

bool xdev_autoqa_enabled(void) {
    const char *env = getenv("PI_AUTO_QA");
    if (env && *env) return !(str_eq(env, "0") || str_ieq(env, "false") || str_ieq(env, "off"));
    json_t *configured = settings_get("dev.autoqa");
    if (configured) return json_truthy(configured);
    return !str_eq(settings_str("dev.autoqaConsent", "unset"), "denied");
}

static const char *REPORT_USAGE =
    "Write `<tool>: <concise description>` as plain text to " XD REPORT_ISSUE
    ". A two-line fallback also works: tool name on line 1, report body below.";

/* parse `<tool>: <desc>` or `tool\nbody`; returns false on bad format */
static bool parse_report(const char *text, char **tool, char **report) {
    char *body = str_trim(text);
    *tool = *report = NULL;
    char *nl = strchr(body, '\n');
    if (nl) {
        char *t0 = xstrndup(body, (size_t)(nl - body));
        char *t = str_trim(t0), *r = str_trim(nl + 1);
        free(t0);
        if (*t && *r) {
            *tool = t;
            *report = r;
            free(body);
            return true;
        }
        free(t);
        free(r);
    }
    for (char *p = body; *p; p++) {
        if (*p == ':' && p > body && p[1] && isspace((unsigned char)p[1])) {
            char *t0 = xstrndup(body, (size_t)(p - body));
            char *t = str_trim(t0), *r = str_trim(p + 1);
            free(t0);
            if (*t && *r) {
                *tool = t;
                *report = r;
                free(body);
                return true;
            }
            free(t);
            free(r);
            break;
        }
    }
    free(body);
    return false;
}

/* Records only after explicit consent (`dev.autoqaConsent: granted`, or PI_AUTO_QA_PUSH=1), to a local JSONL
 * file. omc has no collector endpoint, so nothing ever leaves the machine. */
static void record_issue(tool_ctx_t *ctx, const char *tool, const char *report) {
    const char *force = getenv("PI_AUTO_QA_PUSH");
    bool consent = (force && str_eq(force, "1")) || str_eq(settings_str("dev.autoqaConsent", "unset"), "granted");
    if (!consent) return;
    char *path = xasprintf("%s/autoqa.jsonl", omc_agent_dir());
    json_t *row = json_obj();
    json_set_str(row, "model", ctx && ctx->agent && ctx->agent->model ? ctx->agent->model->id : "unknown");
    json_set_str(row, "tool", str_starts(tool, "proxy_") ? tool + 6 : tool);
    json_set_str(row, "report", report);
    json_set_int(row, "created_at", (int64_t)time(NULL));
    char *line = json_dump(row, 0);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    free(line);
    json_free(row);
    free(path);
}

/* ---------- read / write handlers ---------- */

/* parse xd://name (no / ? #). Returns 1 with *name (owned, NULL for bare xd://), 2 for xd://name/topic, 0 invalid */
static int parse_xd(const char *url, char **name, char **topic) {
    *name = *topic = NULL;
    char *t = str_trim(url);
    if (strncasecmp(t, XD, 5) != 0) {
        free(t);
        return 0;
    }
    const char *rest = t + 5;
    int rc = 0;
    if (!*rest) rc = 1;
    else if (!strpbrk(rest, "/?#")) {
        *name = xstrdup(rest);
        rc = 1;
    } else {
        const char *sl = strchr(rest, '/');
        if (sl && sl > rest && sl[1] && !strpbrk(sl + 1, "/?#") && !memchr(rest, '?', (size_t)(sl - rest)) &&
            !memchr(rest, '#', (size_t)(sl - rest))) {
            *name = xstrndup(rest, (size_t)(sl - rest));
            *topic = xstrdup(sl + 1);
            rc = 2;
        }
    }
    free(t);
    return rc;
}

static char *no_such_tool(agent_t *a, const char *name) {
    sb_t b = SB_INIT;
    sb_appendf(&b, "No such tool: " XD "%s. Mounted devices: ", name);
    bool first = true;
    for (size_t i = 0; i < a->all_tools.n; i++) {
        if (!a->all_tools.v[i]->mounted) continue;
        if (!first) sb_append(&b, ", ");
        sb_append(&b, a->all_tools.v[i]->name);
        first = false;
    }
    sb_append(&b, ". Active top-level tools are also dispatchable via " XD "<tool>.");
    return sb_detach(&b);
}

static const char *NOT_MOUNTED = XD " is not mounted in this session.";

bool xdev_read(agent_t *a, const char *path, tool_result_t *out) {
    if (!path || strncasecmp(path, XD, 5) != 0) return false;
    char *name = NULL, *topic = NULL;
    int kind = parse_xd(path, &name, &topic);
    if (kind == 0) {
        tool_result_error(out, "Invalid xd:// URL: %s. Use xd://, xd://<tool>, or xd://<tool>/<topic>.", path);
    } else if (kind == 2) {
        /* xd://<tool>/<topic>: one named doc topic of a session tool (e.g. xd://eval/judge) */
        agent_tool_t *t = a ? agent_find_tool(a, name) : NULL;
        json_t *topics = t && t->doc_topics && (t->mounted || is_active(a, t)) ? t->doc_topics(t) : NULL;
        const char *doc = json_get_str(topics, topic);
        if (!topics) {
            tool_result_error(out, "Tool '%s' has no doc topics.", name);
        } else if (!doc) {
            sb_t b = SB_INIT;
            const char *k;
            json_t *v;
            json_obj_foreach(topics, i, k, v) sb_appendf(&b, "%s%s", i ? ", " : "", k);
            tool_result_error(out, "Unknown topic '%s' for %s. Available: %s.", topic, name, sb_cstr(&b));
            sb_free(&b);
        } else {
            tool_result_text(out, doc);
        }
        json_free(topics);
    } else if (name && str_eq(name, REPORT_ISSUE)) {
        tool_result_text(out, REPORT_USAGE);
    } else if (!a || !xdev_mounted_count(a)) {
        tool_result_error(out, "%s", NOT_MOUNTED);
    } else if (!name) {
        char *l = xdev_listing(a);
        tool_result_text(out, l);
        free(l);
    } else {
        agent_tool_t *t = resolve_tool(a, name);
        if (!t) {
            char *e = no_such_tool(a, name);
            tool_result_error(out, "%s", e);
            free(e);
        } else {
            char *d = render_docs(t, "#", 0);
            tool_result_text(out, d);
            free(d);
        }
    }
    free(name);
    free(topic);
    return true;
}

static bool help_content(const char *content) {
    char *t = str_trim(content ? content : "");
    bool r = !*t || str_eq(t, "?") || str_ieq(t, "help");
    free(t);
    return r;
}

static json_t *xdev_details(const char *tool, const char *mode, const json_t *args, const json_t *inner) {
    json_t *x = json_obj();
    json_set_str(x, "tool", tool);
    json_set_str(x, "mode", mode);
    if (args) json_set(x, "args", json_clone(args));
    if (inner) json_set(x, "inner", json_clone(inner));
    json_t *d = json_obj();
    json_set(d, "xdev", x);
    return d;
}

/* dispatchXdevTool innerOnUpdate: a device's partial result renders through the write card as the device's own */
static void xdev_update(tool_ctx_t *ctx, const tool_result_t *partial) {
    tool_result_t q = *partial;
    q.details = xdev_details(ctx->xdev_tool, "execute", ctx->xdev_args, partial->details);
    ctx->xdev_outer_update(ctx, &q);
    json_free(q.details);
}

bool xdev_write(tool_ctx_t *ctx, const char *path, const char *content, tool_result_t *out) {
    if (!path || strncasecmp(path, XD, 5) != 0) return false;
    agent_t *a = ctx ? ctx->agent : NULL;
    char *name = NULL, *topic = NULL;
    int kind = parse_xd(path, &name, &topic);
    if (kind != 1) {
        tool_result_error(out, "Invalid xd:// URL: %s. Use xd://<tool>.", path);
        goto done;
    }
    if (name && str_eq(name, REPORT_ISSUE)) {
        if (xdev_autoqa_enabled()) {
            char *tool = NULL, *report = NULL;
            char *body = str_trim(content ? content : "");
            if (!*body) {
                tool_result_error(out, "Empty report. %s", REPORT_USAGE);
                free(body);
                goto done;
            }
            free(body);
            if (!parse_report(content, &tool, &report)) {
                tool_result_error(out, "Invalid report format. %s", REPORT_USAGE);
                goto done;
            }
            record_issue(ctx, tool, report);
            free(tool);
            free(report);
        }
        tool_result_text(out, "Noted, thanks!");
        json_t *args = json_obj();
        char *tr = str_trim(content ? content : "");
        json_set(args, "report", json_str_own(tr));
        out->details = xdev_details(REPORT_ISSUE, "execute", args, NULL);
        json_free(args);
        goto done;
    }
    if (!a || !xdev_mounted_count(a)) {
        tool_result_error(out, "%s", NOT_MOUNTED);
        goto done;
    }
    if (!name) {
        char *l = xdev_listing(a);
        tool_result_error(out, "Cannot write to xd:// itself — pick a device:\n%s", l);
        free(l);
        goto done;
    }
    agent_tool_t *t = resolve_tool(a, name);
    if (!t) {
        char *e = no_such_tool(a, name);
        tool_result_error(out, "%s", e);
        free(e);
        out->details = xdev_details(name, "execute", NULL, NULL);
        goto done;
    }
    if (help_content(content)) {
        char *d = render_docs(t, "#", 0);
        tool_result_text(out, d);
        free(d);
        out->details = xdev_details(name, "help", NULL, NULL);
        goto done;
    }
    char *perr = NULL;
    json_t *args = json_parse(content, strlen(content), &perr);
    if (!args) {
        tool_result_error(out, XD "%s expects a JSON args object as content (%s). Write `?` for docs.", name,
                          perr ? perr : "parse error");
        free(perr);
        out->details = xdev_details(name, "execute", NULL, NULL);
        goto done;
    }
    free(perr);
    if (!json_is_obj(args)) {
        tool_result_error(out, XD "%s content must be a JSON object, got %s.", name,
                          json_is_arr(args) ? "array" : json_is_str(args) ? "string" : json_is_num(args) ? "number"
                          : json_is_bool(args) ? "boolean" : "object");
        json_free(args);
        out->details = xdev_details(name, "execute", NULL, NULL);
        goto done;
    }
    if (json_get(args, INTENT_FIELD) && !schema_declares_property(t->parameters, INTENT_FIELD))
        json_del(args, INTENT_FIELD);
    char *argstr = NULL;
    char *verr = t->lenient_args ? NULL : schema_validate_args(t->parameters, args, &argstr);
    if (verr) {
        char *d = render_docs(t, "#", 0);
        tool_result_error(out, "Invalid args for " XD "%s: Validation failed for tool \"%s\":\n%s\n\nReceived arguments:\n%s\n\n%s",
                          name, name, verr, argstr, d);
        free(argstr);
        free(d);
        free(verr);
        json_free(args);
        out->details = xdev_details(name, "execute", NULL, NULL);
        goto done;
    }
    /* the ctx is embedded in the agent's job record (update callbacks locate it by address): reuse it */
    const char *outer_name = ctx->tool_name;
    ctx->tool_name = t->name;
    void (*outer_update)(tool_ctx_t *, const tool_result_t *) = ctx->update;
    if (outer_update) {
        ctx->xdev_outer_update = outer_update;
        ctx->xdev_tool = name;
        ctx->xdev_args = args;
        ctx->update = xdev_update;
    }
    t->execute(t, ctx, args, out);
    ctx->update = outer_update;
    ctx->xdev_outer_update = NULL;
    ctx->xdev_tool = NULL;
    ctx->xdev_args = NULL;
    ctx->tool_name = outer_name;
    json_t *inner = out->details;
    out->details = xdev_details(name, "execute", args, inner);
    json_free(inner);
    json_free(args);
done:
    free(name);
    free(topic);
    return true;
}
