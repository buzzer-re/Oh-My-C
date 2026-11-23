/* toolcli.c - schema-derived command lines for tool commands (port of omp exec/tool-cli.ts).
 *
 * A tool command's `--help` and its argv binding both come from the tool's live JSON schema: required scalar
 * top-level fields are positionals, every leaf is a `--flag` (nested objects as `--a.b`), `-j/--json` merges a raw
 * object, `@FILE` / `-` merge a JSON object or bind text to the next positional. */
#include "toolcmd.h"

#include "core/fs.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>

#define PROTOCOL_INTENT_DESC "Short present-participle intent for this call."

typedef enum { K_STRING, K_INTEGER, K_NUMBER, K_BOOLEAN, K_OBJECT, K_FALLBACK } kind_t;

typedef struct {
    strvec_t path;
    kind_t kind;
    const json_t *values; /* enum array (borrowed) or NULL */
    const char *description;
    bool required, repeatable, positional;
    const json_t *schema; /* borrowed */
    size_t index;
} leaf_t;

typedef VEC(leaf_t *) leafvec_t;

/* ---------- schema helpers ---------- */

static const char *schema_type(const json_t *s) {
    json_t *t = json_get(s, "type");
    if (json_is_str(t)) return json_sv(t);
    json_t *e;
    json_arr_foreach(t, i, e) if (json_is_str(e) && !str_eq(json_sv(e), "null")) return json_sv(e);
    return NULL;
}

static kind_t scalar_kind(const json_t *s) {
    const char *t = schema_type(s);
    if (str_eq(t, "string")) return K_STRING;
    if (str_eq(t, "integer")) return K_INTEGER;
    if (str_eq(t, "number")) return K_NUMBER;
    if (str_eq(t, "boolean")) return K_BOOLEAN;
    if (str_eq(t, "object")) return K_OBJECT;
    if (str_eq(t, "array")) return json_is_obj(json_get(s, "items")) ? scalar_kind(json_get(s, "items")) : K_FALLBACK;
    return K_FALLBACK;
}

static bool has_protocol_intent(const json_t *schema) {
    json_t *i = json_get_obj(json_get_obj(schema, "properties"), "i");
    return i && str_eq(json_get_str(i, "description"), PROTOCOL_INTENT_DESC);
}

static bool all_digits(const char *s) {
    if (!*s) return false;
    if (s[0] == '0') return !s[1];
    for (; *s; s++)
        if (!isdigit((unsigned char)*s)) return false;
    return true;
}

static const json_t *resolve_pointer(const json_t *root, const char *ref) {
    if (str_eq(ref, "#")) return root;
    if (!str_starts(ref, "#/")) return NULL;
    const json_t *v = root;
    const char *p = ref + 2;
    for (;;) {
        const char *sl = strchr(p, '/');
        char *enc = sl ? xstrndup(p, (size_t)(sl - p)) : xstrdup(p);
        char *k1 = str_replace(enc, "~1", "/");
        char *key = str_replace(k1, "~0", "~");
        free(enc);
        free(k1);
        if (json_is_obj(v)) {
            json_t *n = json_get(v, key);
            if (!n) {
                free(key);
                return NULL;
            }
            v = n;
        } else if (json_is_arr(v)) {
            if (!all_digits(key)) {
                free(key);
                return NULL;
            }
            v = json_at(v, (size_t)strtoull(key, NULL, 10));
        } else {
            free(key);
            return NULL;
        }
        free(key);
        if (!sl) break;
        p = sl + 1;
    }
    return v;
}

static void merge_required(json_t *target, const json_t *value, bool unite) {
    if (!json_is_arr(value)) return;
    json_t *out = json_arr(), *e;
    if (unite) json_arr_foreach(json_get_arr(target, "required"), i, e) if (json_is_str(e)) json_push_str(out, json_sv(e));
    json_arr_foreach(value, i, e) {
        if (!json_is_str(e)) continue;
        bool dup = false;
        json_t *x;
        if (unite) json_arr_foreach(out, k, x) dup |= str_eq(json_sv(x), json_sv(e));
        if (!dup) json_push_str(out, json_sv(e));
    }
    json_set(target, "required", out);
}

static void merge_properties(json_t *target, const json_t *value) {
    if (!json_is_obj(value)) return;
    json_t *props = json_get_obj(target, "properties");
    if (!props) {
        json_set(target, "properties", json_obj());
        props = json_get_obj(target, "properties");
    }
    const char *k;
    json_t *v;
    json_obj_foreach(value, i, k, v) json_set(props, k, json_clone(v));
}

static void merge_schema(json_t *target, const json_t *source, bool unite) {
    const char *k;
    json_t *v;
    json_obj_foreach(source, i, k, v) {
        if (str_eq(k, "required")) merge_required(target, v, unite);
        else if (str_eq(k, "properties")) merge_properties(target, v);
        else json_set(target, k, json_clone(v));
    }
}

static bool is_null_schema(const json_t *s) {
    if (!json_is_obj(s)) return false;
    if (str_eq(schema_type(s), "null")) return true;
    json_t *en = json_get_arr(s, "enum"), *e;
    if (!en || !json_len(en)) return false;
    json_arr_foreach(en, i, e) if (!json_is_null(e) || !e) return false;
    return true;
}

static void push_unique(json_t *arr, const json_t *v) {
    json_t *e;
    json_arr_foreach(arr, i, e) if (json_equal(e, v)) return;
    json_push(arr, json_clone(v));
}

static json_t *combine_alternatives(json_t **alts, size_t n) {
    json_t *combined = json_obj();
    const char *first = NULL;
    bool same = true, any = false;
    for (size_t i = 0; i < n; i++) {
        const char *t = schema_type(alts[i]);
        if (!t) continue;
        if (!any) first = t, any = true;
        else if (!str_eq(first, t)) same = false;
    }
    if (any && same) json_set_str(combined, "type", first);
    json_t *ev = json_arr();
    for (size_t i = 0; i < n; i++) {
        json_t *en = json_get_arr(alts[i], "enum"), *e;
        if (en) json_arr_foreach(en, k, e) push_unique(ev, e);
        else if (json_get(alts[i], "const")) push_unique(ev, json_get(alts[i], "const"));
    }
    if (json_len(ev)) json_set(combined, "enum", ev);
    else json_free(ev);
    json_t *props = json_obj();
    for (size_t i = 0; i < n; i++) {
        json_t *ap = json_get_obj(alts[i], "properties");
        const char *k;
        json_t *child;
        json_obj_foreach(ap, j, k, child) {
            json_t *existing = json_get(props, k);
            if (json_is_obj(existing) && json_is_obj(child)) {
                json_t *pair[2] = {existing, child};
                json_t *c = combine_alternatives(pair, 2);
                json_set(props, k, c);
            } else {
                json_set(props, k, json_clone(child));
            }
        }
    }
    if (json_len(props)) {
        json_set_str(combined, "type", "object");
        json_set(combined, "properties", props);
        json_t *inter = json_arr(), *r;
        json_arr_foreach(json_get_arr(alts[0], "required"), j, r) {
            if (!json_is_str(r)) continue;
            bool all = true;
            for (size_t i = 1; i < n && all; i++) {
                bool has = false;
                json_t *x;
                json_arr_foreach(json_get_arr(alts[i], "required"), k, x) has |= str_eq(json_sv(x), json_sv(r));
                all = has;
            }
            bool dup = false;
            json_t *x;
            json_arr_foreach(inter, k, x) dup |= str_eq(json_sv(x), json_sv(r));
            if (all && !dup) json_push_str(inter, json_sv(r));
        }
        if (json_len(inter)) json_set(combined, "required", inter);
        else json_free(inter);
    } else {
        json_free(props);
    }
    return combined;
}

#define NORMALIZE_DEPTH_MAX 64

static json_t *normalize_node(const json_t *root, const json_t *schema, strvec_t *resolving, int depth) {
    if (!json_is_obj(schema) || depth > NORMALIZE_DEPTH_MAX) return json_clone(schema);
    json_t *result = json_obj();
    const char *ref = json_get_str(schema, "$ref");
    if (ref && !strvec_contains(resolving, ref)) {
        const json_t *target = resolve_pointer(root, ref);
        if (target) {
            strvec_push(resolving, ref);
            json_t *n = normalize_node(root, target, resolving, depth + 1);
            free(resolving->v[--resolving->n]);
            if (json_is_obj(n)) merge_schema(result, n, true);
            json_free(n);
        }
    }
    json_t *all = json_get_arr(schema, "allOf"), *part;
    json_arr_foreach(all, i, part) {
        json_t *n = normalize_node(root, part, resolving, depth + 1);
        if (json_is_obj(n)) merge_schema(result, n, true);
        json_free(n);
    }
    static const char *const COMB[] = {"anyOf", "oneOf"};
    for (size_t c = 0; c < 2; c++) {
        json_t *alts = json_get_arr(schema, COMB[c]);
        if (!alts) continue;
        VEC(json_t *) keep = {0};
        json_arr_foreach(alts, i, part) {
            json_t *n = normalize_node(root, part, resolving, depth + 1);
            if (!is_null_schema(n) && json_is_obj(n)) vec_push(&keep, n);
            else json_free(n);
        }
        if (keep.n == 1) merge_schema(result, keep.v[0], false);
        else if (keep.n > 1) {
            json_t *comb = combine_alternatives(keep.v, keep.n);
            merge_schema(result, comb, false);
            json_free(comb);
        }
        for (size_t i = 0; i < keep.n; i++) json_free(keep.v[i]);
        free(keep.v);
    }
    const char *k;
    json_t *v;
    json_obj_foreach(schema, i, k, v) {
        if (str_eq(k, "$ref") || str_eq(k, "allOf") || str_eq(k, "anyOf") || str_eq(k, "oneOf") || str_eq(k, "$defs") ||
            str_eq(k, "definitions"))
            continue;
        json_t *n;
        if (str_eq(k, "properties") && json_is_obj(v)) {
            n = json_obj();
            const char *pk;
            json_t *pv;
            json_obj_foreach(v, j, pk, pv) json_set(n, pk, normalize_node(root, pv, resolving, depth + 1));
        } else if (str_eq(k, "items")) {
            n = normalize_node(root, v, resolving, depth + 1);
        } else {
            n = json_clone(v);
        }
        if (str_eq(k, "required")) {
            merge_required(result, n, true);
            json_free(n);
        } else if (str_eq(k, "properties")) {
            merge_properties(result, n);
            json_free(n);
        } else {
            json_set(result, k, n);
        }
    }
    return result;
}

json_t *toolcli_normalize_schema(const json_t *schema) {
    strvec_t resolving = {0};
    json_t *n = normalize_node(schema, schema, &resolving, 0);
    strvec_free(&resolving);
    if (!json_is_obj(n)) {
        json_free(n);
        n = json_obj();
    }
    if (has_protocol_intent(schema)) {
        json_t *props = json_get_obj(n, "properties");
        if (props) {
            json_del(props, "i");
            json_del(props, "notrunc");
        }
        json_t *req = json_get_arr(n, "required");
        if (req) {
            json_t *f = json_arr(), *e;
            json_arr_foreach(req, i, e) if (!str_eq(json_sv(e), "i") && !str_eq(json_sv(e), "notrunc")) json_push(f, json_clone(e));
            json_set(n, "required", f);
        }
    }
    return n;
}

/* ---------- leaves ---------- */

static bool req_has(const json_t *schema, const char *name) {
    json_t *e;
    json_arr_foreach(json_get_arr(schema, "required"), i, e) if (str_eq(json_sv(e), name)) return true;
    return false;
}

static void collect_leaves(const json_t *schema, bool parent_required, strvec_t *path, leafvec_t *out, int depth) {
    json_t *props = json_get_obj(schema, "properties");
    if (!props || depth > NORMALIZE_DEPTH_MAX) return;
    const char *name;
    json_t *child;
    json_obj_foreach(props, i, name, child) {
        if (!path->n && (str_eq(name, "help") || str_eq(name, "json"))) continue;
        if (!json_is_obj(child)) continue;
        bool required_here = parent_required && req_has(schema, name);
        strvec_push(path, name);
        if (json_get_obj(child, "properties")) {
            collect_leaves(child, required_here, path, out, depth + 1);
        } else {
            leaf_t *l = xcalloc(1, sizeof *l);
            for (size_t k = 0; k < path->n; k++) strvec_push(&l->path, path->v[k]);
            l->repeatable = str_eq(schema_type(child), "array");
            const json_t *item = l->repeatable && json_is_obj(json_get(child, "items")) ? json_get(child, "items") : child;
            l->values = json_get_arr(item, "enum");
            l->kind = scalar_kind(child);
            l->description = json_get_str(child, "description");
            l->required = required_here;
            l->positional = path->n == 1 && required_here && !l->repeatable &&
                            (l->values || l->kind == K_STRING || l->kind == K_INTEGER || l->kind == K_NUMBER);
            l->schema = child;
            l->index = out->n;
            vec_push(out, l);
        }
        free(path->v[--path->n]);
    }
}

static void leaf_free(leaf_t *l) {
    strvec_free(&l->path);
    free(l);
}

static void leaves_free(leafvec_t *v) {
    for (size_t i = 0; i < v->n; i++) leaf_free(v->v[i]);
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static size_t req_index(const json_t *schema, const char *name) {
    json_t *e;
    json_arr_foreach(json_get_arr(schema, "required"), i, e) if (str_eq(json_sv(e), name)) return i;
    return (size_t)-1;
}

static char *flag_name(const strvec_t *path) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < path->n; i++) {
        if (i) sb_putc(&b, '.');
        for (const char *p = path->v[i]; *p; p++) sb_putc(&b, *p == '_' ? '-' : *p);
    }
    return sb_detach(&b);
}

static bool is_flag_bool(const leaf_t *l) { return !l->repeatable && l->kind == K_BOOLEAN && !l->values; }

/* schemaLeaves (positionals first, in `required` order) then unambiguousCliLeaves */
static void schema_leaves(const json_t *normalized, leafvec_t *out, strvec_t *ambiguous) {
    leafvec_t all = {0};
    strvec_t path = {0};
    collect_leaves(normalized, true, &path, &all, 0);
    strvec_free(&path);
    /* stable insertion sort by (positional ? required index : max, index) */
    for (size_t i = 1; i < all.n; i++) {
        leaf_t *x = all.v[i];
        size_t kx = x->positional ? req_index(normalized, x->path.v[0]) : (size_t)-1;
        size_t j = i;
        while (j > 0) {
            leaf_t *y = all.v[j - 1];
            size_t ky = y->positional ? req_index(normalized, y->path.v[0]) : (size_t)-1;
            if (ky < kx || (ky == kx && y->index < x->index)) break;
            all.v[j] = y;
            j--;
        }
        all.v[j] = x;
    }
    /* spellings -> owners */
    strvec_t spell = {0};
    VEC(size_t) owner = {0};
    for (size_t i = 0; i < all.n; i++) {
        char *f = flag_name(&all.v[i]->path);
        strvec_push(&spell, f);
        vec_push(&owner, i);
        if (is_flag_bool(all.v[i])) {
            strvec_push_own(&spell, xasprintf("no-%s", f));
            vec_push(&owner, i);
        }
        free(f);
    }
    bool *amb = xcalloc(all.n ? all.n : 1, sizeof *amb);
    for (size_t a = 0; a < spell.n; a++)
        for (size_t b = a + 1; b < spell.n; b++)
            if (owner.v[a] != owner.v[b] && str_eq(spell.v[a], spell.v[b])) amb[owner.v[a]] = amb[owner.v[b]] = true;
    for (size_t i = 0; i < all.n; i++) {
        if (!amb[i]) {
            vec_push(out, all.v[i]);
            continue;
        }
        if (ambiguous) {
            sb_t b = SB_INIT;
            for (size_t k = 0; k < all.v[i]->path.n; k++) {
                if (k) sb_putc(&b, '.');
                json_js_quote_sb(&b, all.v[i]->path.v[k], strlen(all.v[i]->path.v[k]));
            }
            strvec_push_own(ambiguous, sb_detach(&b));
        }
        leaf_free(all.v[i]);
    }
    free(amb);
    strvec_free(&spell);
    free(owner.v);
    free(all.v);
}

/* ---------- help ---------- */

static void format_enum(sb_t *b, const json_t *values) {
    sb_putc(b, '{');
    json_t *e;
    json_arr_foreach(values, i, e) {
        if (i) sb_putc(b, '|');
        if (json_is_str(e)) sb_append(b, json_sv(e));
        else json_dump_js_sb(b, e);
    }
    sb_putc(b, '}');
}

static void value_usage(sb_t *b, const leaf_t *l) {
    if (l->values) format_enum(b, l->values);
    else {
        static const char *const U[] = {"<STRING>", "<INTEGER>", "<NUMBER>", "<BOOLEAN>", "<JSON_OBJECT>", "<JSON>"};
        sb_append(b, U[l->kind]);
    }
    if (l->repeatable) sb_append(b, "...");
}

char *toolcli_render_help(const char *name, const char *description, const json_t *schema) {
    json_t *normalized = toolcli_normalize_schema(schema);
    leafvec_t leaves = {0};
    strvec_t ambiguous = {0};
    schema_leaves(normalized, &leaves, &ambiguous);
    strvec_t flags = {0};
    for (size_t i = 0; i < leaves.n; i++) strvec_push_own(&flags, flag_name(&leaves.v[i]->path));
    sb_t b = SB_INIT;
    sb_appendf(&b, "%s — %s\n\nUsage:\n  %s", name, description ? description : "session tool", name);
    size_t npos = 0;
    for (size_t i = 0; i < leaves.n; i++)
        if (leaves.v[i]->positional) sb_appendf(&b, " <%s>", leaves.v[i]->path.v[0]), npos++;
    sb_append(&b, " [OPTIONS] [@FILE] [-]\n");
    if (npos) {
        sb_append(&b, "\nArguments:\n");
        for (size_t i = 0; i < leaves.n; i++) {
            leaf_t *l = leaves.v[i];
            if (!l->positional) continue;
            sb_appendf(&b, "  <%s> ", l->path.v[0]);
            value_usage(&b, l);
            if (l->description) sb_appendf(&b, "  %s", l->description);
            sb_putc(&b, '\n');
        }
    }
    if (leaves.n) {
        sb_append(&b, "\nOptions:\n");
        for (size_t i = 0; i < leaves.n; i++) {
            leaf_t *l = leaves.v[i];
            const char *flag = flags.v[i];
            sb_appendf(&b, "  --%s", flag);
            if (is_flag_bool(l)) {
                char *neg = xasprintf("no-%s", flag);
                if (strvec_contains(&flags, neg)) sb_appendf(&b, " / --%s=<BOOLEAN>", flag);
                else sb_appendf(&b, " / --no-%s", flag);
                free(neg);
            } else {
                sb_putc(&b, ' ');
                value_usage(&b, l);
            }
            if (l->description) sb_appendf(&b, "  %s", l->description);
            if (l->required) sb_append(&b, "  (required)");
            if (l->repeatable) sb_append(&b, "  (repeatable)");
            sb_putc(&b, '\n');
        }
    }
    json_t *props = json_get_obj(normalized, "properties");
    bool rh = props && json_get(props, "help"), rj = props && json_get(props, "json");
    if (rh || rj)
        sb_appendf(&b, "\nReserved schema fields:\n  %s%s%s  Set through -j/--json; their flag names belong to the command line.\n",
                   rh ? "help" : "", rh && rj ? ", " : "", rj ? "json" : "");
    if (ambiguous.n) {
        sb_append(&b, "\nAmbiguous schema fields:\n  ");
        for (size_t i = 0; i < ambiguous.n; i++) sb_appendf(&b, "%s%s", i ? ", " : "", ambiguous.v[i]);
        sb_append(&b, "  Set through -j/--json; their derived flag names collide.\n");
    }
    sb_append(&b, "  -j, --json <JSON>  Merge one raw JSON object.\n"
                  "  @FILE             Merge a JSON object from FILE, or bind its text to the next argument.\n"
                  "  -                 Same as @FILE, read from stdin.\n"
                  "  -h, --help        Show this help.\n");
    strvec_free(&flags);
    strvec_free(&ambiguous);
    leaves_free(&leaves);
    json_free(normalized);
    return sb_detach(&b);
}

/* ---------- parse ---------- */

typedef struct {
    char *err; /* usage error (owned) */
} perr_t;

static void usage_err(perr_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void usage_err(perr_t *e, const char *fmt, ...) {
    if (e->err) return;
    va_list ap;
    va_start(ap, fmt);
    e->err = xvasprintf(fmt, ap);
    va_end(ap);
}

static const json_t *value_at(const json_t *root, const strvec_t *path) {
    const json_t *v = root;
    for (size_t i = 0; i < path->n; i++) {
        if (!json_is_obj(v)) return NULL;
        v = json_get(v, path->v[i]);
        if (!v) return NULL;
    }
    return v;
}

static void insert_value(json_t *root, const strvec_t *path, json_t *value, bool repeatable) {
    json_t *t = root;
    for (size_t i = 0; i + 1 < path->n; i++) {
        json_t *c = json_get(t, path->v[i]);
        if (!json_is_obj(c)) {
            json_set(t, path->v[i], json_obj());
            c = json_get(t, path->v[i]);
        }
        t = c;
    }
    if (!path->n) {
        json_free(value);
        return;
    }
    const char *last = path->v[path->n - 1];
    if (repeatable) {
        json_t *arr = json_get(t, last);
        if (json_is_arr(arr)) json_push(arr, value);
        else {
            json_t *na = json_arr();
            json_push(na, value);
            json_set(t, last, na);
        }
    } else {
        json_set(t, last, value);
    }
}

static void deep_merge(json_t *target, const json_t *source) {
    const char *k;
    json_t *v;
    json_obj_foreach(source, i, k, v) {
        json_t *ex = json_get(target, k);
        if (json_is_obj(ex) && json_is_obj(v)) deep_merge(ex, v);
        else json_set(target, k, json_clone(v));
    }
}

static char *strip_one_newline(const char *t) {
    size_t n = strlen(t);
    if (n >= 2 && t[n - 2] == '\r' && t[n - 1] == '\n') n -= 2;
    else if (n >= 1 && t[n - 1] == '\n') n -= 1;
    return xstrndup(t, n);
}

/* readArgumentSource: `-` (stdin) or `@path`; returns owned text, NULL when not a source (err set on failure) */
static char *read_source(const char *arg, const toolcli_opts_t *o, char **origin, perr_t *e) {
    *origin = NULL;
    if (str_eq(arg, "-")) {
        if (o->stdin_len > TOOLCLI_INPUT_LIMIT) {
            usage_err(e, "argument source exceeds %d-byte limit", TOOLCLI_INPUT_LIMIT);
            return NULL;
        }
        *origin = xstrdup("<stdin>");
        return xstrndup(o->stdin_text ? o->stdin_text : "", o->stdin_len);
    }
    if (arg[0] != '@' || !arg[1]) return NULL;
    const char *raw = arg + 1;
    char *abs = path_abs(raw, o->cwd ? o->cwd : ".");
    struct stat st;
    char *text = NULL;
    int err = 0;
    if (stat(abs, &st) != 0) err = errno;
    else if (S_ISDIR(st.st_mode)) err = EISDIR;
    else if (st.st_size > TOOLCLI_INPUT_LIMIT) {
        free(abs);
        usage_err(e, "argument source exceeds %d-byte limit", TOOLCLI_INPUT_LIMIT);
        return NULL;
    } else {
        size_t len = 0;
        text = fs_read(abs, &len);
        if (!text) err = errno ? errno : EIO;
        else if (len > TOOLCLI_INPUT_LIMIT) {
            free(text);
            free(abs);
            usage_err(e, "argument source exceeds %d-byte limit", TOOLCLI_INPUT_LIMIT);
            return NULL;
        }
    }
    if (!text) {
        /* Node's fs error message: `ENOENT: no such file or directory, open '<abs>'` */
        char *m = fs_node_error(err, "open", abs);
        usage_err(e, "failed to read argument source `%s`: %s", raw, m);
        free(m);
        free(abs);
        return NULL;
    }
    free(abs);
    *origin = xstrdup(raw);
    return text;
}

/* JS Number(raw) for a decimal or exponent literal (trimmed); false when NaN */
static bool js_number(const char *raw, double *out) {
    char *t = str_trim(raw);
    if (!*t) {
        free(t);
        *out = 0;
        return true; /* Number("") === 0 */
    }
    char *end = NULL;
    errno = 0;
    double d;
    if (str_eq(t, "Infinity") || str_eq(t, "+Infinity")) d = INFINITY, end = t + strlen(t);
    else if (str_eq(t, "-Infinity")) d = -INFINITY, end = t + strlen(t);
    else if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X' || t[1] == 'o' || t[1] == 'O' || t[1] == 'b' || t[1] == 'B')) {
        int base = (t[1] == 'x' || t[1] == 'X') ? 16 : (t[1] == 'o' || t[1] == 'O') ? 8 : 2;
        d = (double)strtoull(t + 2, &end, base);
        if (end == t + 2) end = t;
    } else if (strpbrk(t, "xXpP") || strstr(t, "nan") || strstr(t, "NAN") || strstr(t, "inf") || strstr(t, "INF")) {
        end = t; /* strtod accepts hex floats, nan and inf; JS does not */
        d = 0;
    } else {
        d = strtod(t, &end);
    }
    bool ok = end && end != t && !*end;
    free(t);
    *out = d;
    return ok;
}

static json_t *coerce(const leaf_t *l, const char *raw, const char *source, perr_t *e) {
    if (l->values) {
        json_t *v;
        json_arr_foreach(l->values, i, v) {
            char *s = json_is_str(v) ? xstrdup(json_sv(v)) : json_dump_js(v);
            bool m = str_eq(s, raw);
            free(s);
            if (m) return json_clone(v);
        }
        usage_err(e, "invalid value for `%s`; expected an enum member", source);
        return NULL;
    }
    const json_t *ks = l->repeatable && json_is_obj(json_get(l->schema, "items")) ? json_get(l->schema, "items") : l->schema;
    switch (scalar_kind(ks)) {
    case K_STRING: return json_str(raw);
    case K_INTEGER: {
        const char *p = raw[0] == '-' ? raw + 1 : raw;
        bool shape = *p && (str_eq(p, "0") || (p[0] >= '1' && p[0] <= '9' && strspn(p, "0123456789") == strlen(p)));
        if (shape) {
            double d = strtod(raw, NULL);
            if (fabs(d) <= 9007199254740991.0) return json_int((int64_t)d);
        }
        usage_err(e, "invalid value for `%s`; expected an integer", source);
        return NULL;
    }
    case K_NUMBER: {
        double d;
        char *t = str_trim(raw);
        bool blank = !*t;
        free(t);
        if (!blank && js_number(raw, &d) && isfinite(d)) {
            if (d == floor(d) && fabs(d) < 9007199254740992.0) return json_int((int64_t)d);
            return json_num(d);
        }
        usage_err(e, "invalid value for `%s`; expected a number", source);
        return NULL;
    }
    case K_BOOLEAN:
        if (str_eq(raw, "true")) return json_bool(true);
        if (str_eq(raw, "false")) return json_bool(false);
        usage_err(e, "invalid value for `%s`; expected true or false", source);
        return NULL;
    case K_OBJECT: {
        json_t *v = json_parse_str(raw);
        if (json_is_obj(v)) return v;
        json_free(v);
        usage_err(e, "invalid value for `%s`; expected a JSON object", source);
        return NULL;
    }
    case K_FALLBACK: {
        json_t *v = json_parse_str(raw);
        return v ? v : json_str(raw);
    }
    }
    return json_str(raw);
}

static const leaf_t *next_positional(const leafvec_t *leaves, const json_t *output, const json_t *overrides) {
    for (size_t i = 0; i < leaves->n; i++) {
        const leaf_t *l = leaves->v[i];
        if (l->positional && !json_get(output, l->path.v[0]) && !json_get(overrides, l->path.v[0])) return l;
    }
    return NULL;
}

static const leaf_t *sole_object_target(const leafvec_t *leaves) {
    const leaf_t *only = NULL;
    size_t n = 0;
    for (size_t i = 0; i < leaves->n; i++)
        if (leaves->v[i]->required) only = leaves->v[i], n++;
    return n == 1 && only->path.n == 1 && only->kind == K_OBJECT ? only : NULL;
}

static void bind_literal(const leafvec_t *leaves, json_t *output, json_t *overrides, const char *raw, const char *origin, perr_t *e) {
    const leaf_t *l = next_positional(leaves, output, overrides);
    if (!l) {
        usage_err(e, "unexpected argument `%s`; every positional is already set", origin);
        return;
    }
    json_t *v = coerce(l, raw, origin, e);
    if (v) insert_value(output, &l->path, v, l->repeatable);
}

static void bind_source(const leafvec_t *leaves, json_t *output, json_t *overrides, const char *text, const char *origin, perr_t *e) {
    json_t *parsed = json_parse_str(text);
    if (json_is_obj(parsed)) {
        const leaf_t *t = sole_object_target(leaves);
        if (t && !json_get(output, t->path.v[0]) && !json_get(overrides, t->path.v[0]) && !json_get(parsed, t->path.v[0])) {
            insert_value(output, &t->path, parsed, false);
            return;
        }
        deep_merge(output, parsed);
        json_free(parsed);
        return;
    }
    json_free(parsed);
    if (!next_positional(leaves, output, overrides)) {
        usage_err(e, "`%s` is not a JSON object and no positional remains for its text", origin);
        return;
    }
    char *s = strip_one_newline(text);
    bind_literal(leaves, output, overrides, s, origin, e);
    free(s);
}

static void validate_required(const json_t *schema, const json_t *value, strvec_t *path, perr_t *e, int depth) {
    if (!json_is_obj(value) || e->err || depth > NORMALIZE_DEPTH_MAX) return;
    json_t *r;
    json_arr_foreach(json_get_arr(schema, "required"), i, r) {
        if (!json_is_str(r) || json_get(value, json_sv(r))) continue;
        sb_t b = SB_INIT;
        for (size_t k = 0; k < path->n; k++) sb_appendf(&b, "%s.", path->v[k]);
        sb_append(&b, json_sv(r));
        usage_err(e, "missing required argument `%s`", sb_cstr(&b));
        sb_free(&b);
        return;
    }
    const char *k;
    json_t *cs;
    json_obj_foreach(json_get_obj(schema, "properties"), i, k, cs) {
        json_t *cv = json_get(value, k);
        if (!cv || !json_is_obj(cs)) continue;
        strvec_push(path, k);
        validate_required(cs, cv, path, e, depth + 1);
        free(path->v[--path->n]);
        if (e->err) return;
    }
}

static const leaf_t *find_leaf(const leafvec_t *leaves, const char *flag) {
    for (size_t i = 0; i < leaves->n; i++) {
        char *f = flag_name(&leaves->v[i]->path);
        bool m = str_eq(f, flag);
        free(f);
        if (m) return leaves->v[i];
    }
    return NULL;
}

json_t *toolcli_parse_args(const json_t *schema, const strvec_t *argv, const toolcli_opts_t *o, char **err) {
    *err = NULL;
    json_t *normalized = toolcli_normalize_schema(schema);
    leafvec_t leaves = {0};
    schema_leaves(normalized, &leaves, NULL);
    json_t *output = json_obj(), *overrides = json_obj();
    perr_t e = {0};
    bool ended = false;
    for (size_t i = 0; i < argv->n && !e.err; i++) {
        const char *a = argv->v[i];
        if (ended) {
            bind_literal(&leaves, output, overrides, a, a, &e);
            continue;
        }
        if (str_eq(a, "--")) {
            ended = true;
            continue;
        }
        if (str_eq(a, "-") || (a[0] == '@' && a[1])) {
            char *origin = NULL;
            char *text = read_source(a, o, &origin, &e);
            if (text) bind_source(&leaves, output, overrides, text, origin, &e);
            free(text);
            free(origin);
            continue;
        }
        if (str_eq(a, "-j") || str_eq(a, "--json") || str_starts(a, "--json=")) {
            const char *raw = str_starts(a, "--json=") ? a + 7 : (i + 1 < argv->n ? argv->v[++i] : NULL);
            if (!raw) {
                usage_err(&e, "flag `%s` requires a value", a);
                break;
            }
            char *origin = NULL;
            char *text = read_source(raw, o, &origin, &e);
            free(origin);
            if (e.err) {
                free(text);
                break;
            }
            char *perr = NULL;
            const char *src = text ? text : raw;
            json_t *parsed = json_parse(src, strlen(src), &perr);
            free(text);
            if (!parsed) {
                usage_err(&e, "invalid JSON for `--json`: %s", perr ? perr : "parse error");
                free(perr);
                break;
            }
            free(perr);
            if (!json_is_obj(parsed)) {
                usage_err(&e, "`--json` must be a JSON object");
                json_free(parsed);
                break;
            }
            deep_merge(output, parsed);
            json_free(parsed);
            continue;
        }
        if (!str_starts(a, "--")) {
            if (a[0] == '-') {
                usage_err(&e, "unknown flag `%s`", a);
                break;
            }
            bind_literal(&leaves, output, overrides, a, a, &e);
            continue;
        }
        const char *eq = strchr(a, '=');
        char *flag = eq ? xstrndup(a + 2, (size_t)(eq - a - 2)) : xstrdup(a + 2);
        const char *inl = eq ? eq + 1 : NULL;
        const leaf_t *l = find_leaf(&leaves, flag);
        bool negative = false;
        if (!l && str_starts(flag, "no-")) {
            negative = true;
            l = find_leaf(&leaves, flag + 3);
        }
        free(flag);
        if (!l) {
            usage_err(&e, "unknown flag `%s`", a);
            break;
        }
        if (is_flag_bool(l)) {
            if (negative && inl) {
                usage_err(&e, "invalid value for `%s`; expected a flag without a value", a);
                break;
            }
            json_t *v = inl ? coerce(l, inl, a, &e) : json_bool(!negative);
            if (v) insert_value(overrides, &l->path, v, false);
            continue;
        }
        if (negative) {
            usage_err(&e, "unknown flag `%s`", a);
            break;
        }
        const char *raw = inl ? inl : (i + 1 < argv->n ? argv->v[++i] : NULL);
        if (!raw) {
            usage_err(&e, "flag `%s` requires a value", a);
            break;
        }
        char *origin = NULL;
        char *text = read_source(raw, o, &origin, &e);
        free(origin);
        if (e.err) {
            free(text);
            break;
        }
        char *val = text ? strip_one_newline(text) : xstrdup(raw);
        free(text);
        json_t *v = coerce(l, val, a, &e);
        free(val);
        if (v) insert_value(overrides, &l->path, v, l->repeatable);
    }
    if (!e.err) {
        for (size_t i = 0; i < leaves.n; i++) {
            const json_t *v = value_at(overrides, &leaves.v[i]->path);
            if (v) insert_value(output, &leaves.v[i]->path, json_clone(v), false);
        }
        strvec_t path = {0};
        validate_required(normalized, output, &path, &e, 0);
        strvec_free(&path);
    }
    json_free(overrides);
    leaves_free(&leaves);
    json_free(normalized);
    if (e.err) {
        json_free(output);
        *err = e.err;
        return NULL;
    }
    return output;
}

/* ---------- run ---------- */

void toolcli_result_free(toolcli_result_t *r) {
    free(r->out);
    free(r->err);
    r->out = r->err = NULL;
}

static void err_result(toolcli_result_t *r, const char *name, const char *msg, int code) {
    r->out = xstrdup("");
    r->err = xasprintf("%s: %s\n", name, msg);
    r->exit_code = code;
}

void toolcli_run(const char *name, const strvec_t *argv, const toolcli_opts_t *o, const toolcli_host_t *host,
                 toolcli_result_t *res) {
    memset(res, 0, sizeof *res);
    bool help = false;
    for (size_t i = 0; i < argv->n; i++) help |= str_eq(argv->v[i], "-h") || str_eq(argv->v[i], "--help");
    if (help) {
        if (argv->n != 1) {
            err_result(res, name, "--help cannot be combined with invocation arguments", 2);
            return;
        }
        res->out = toolcli_render_help(name, host->description, host->schema);
        res->err = xstrdup("");
        res->exit_code = 0;
        res->help = true;
        return;
    }
    char *perr = NULL;
    json_t *args = toolcli_parse_args(host->schema, argv, o, &perr);
    if (!args) {
        char *m = xasprintf("%s\n%s: run `%s --help`", perr, name, name);
        err_result(res, name, m, 2);
        free(m);
        free(perr);
        return;
    }
    if (has_protocol_intent(host->schema)) {
        char *iv = xasprintf("Invoking %s", name);
        json_set_str(args, "i", iv);
        free(iv);
    }
    host->call(host->ud, args, res);
    json_free(args);
}
