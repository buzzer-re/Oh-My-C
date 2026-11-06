/* cfg_url.c - `cfg://`, the agent's view of the settings (port of omp internal-urls/cfg-protocol.ts,
 * packages/tui/src/tools/cfg-url.ts and the cfg approval prompt of modes/interactive-mode.ts).
 *
 * Read forms (`/` and `.` both separate segments):
 *   cfg://                every setting as a YAML-ish tree
 *   cfg://<namespace>     one namespace's subtree
 *   cfg://<setting>       value, type, default, source, values, description
 * Write forms (content is the new value), only from the top-level interactive session, each approved by the user:
 *   cfg://<setting>       session-only runtime override
 *   cfg://<setting>/save  persisted to the global config.yml
 * The registry (ids, types, defaults, enum values, descriptions, credential flags) is omp's own, dumped by
 * build-tools/extract_settings.sh --registry into assets/config/settings-registry.json. omc has no environment
 * layer for settings, so provenance stops at session override / --config overlay / project / global / default.
 * Credential values are always redacted. */
#include "iurl.h"

#include "coding/agents.h"
#include "config/settings.h"
#include "core/embed.h"
#include "prompt/template.h"
#include "tui/fuzzy.h"

#include <ctype.h>
#include <math.h>

#define CFG_PREFIX "cfg://"
#define TREE_COMMENT_MAX_CHARS 120
static const char *const REDACTED = "<redacted>";
static const char *const OPT_SESSION = "Always for this session";
static const char *const OPT_ONCE = "Allow once";
static const char *const OPT_DENY = "Deny";

typedef struct {
    const char *id;
    const json_t *def; /* registry entry {id, type, default?, values?, description?, credential?, env?} */
} setting_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *g_registry;
static setting_t *g_sorted;
static size_t g_count;
/* "Always for this session" grant: which session (tool env) and whether it extends to /save */
static const void *g_grant_session;
static bool g_grant_save;

/* ICU root collation at base strength, enough for setting ids: case-insensitive, `_` < `-` < other punctuation <
 * digits < letters */
static int coll_weight(unsigned char c) {
    if (isalpha(c)) return 300 + tolower(c);
    if (isdigit(c)) return 200 + c;
    if (c == '_') return 1;
    if (c == '-') return 2;
    return 10 + c;
}

static int coll_cmp(const char *a, size_t al, const char *b, size_t bl) {
    for (size_t i = 0; i < al && i < bl; i++) {
        int d = coll_weight((unsigned char)a[i]) - coll_weight((unsigned char)b[i]);
        if (d) return d;
    }
    return al < bl ? -1 : al > bl ? 1 : 0;
}

/* allSettings order: segment by segment (localeCompare base), then fewer segments first */
static int setting_cmp(const void *x, const void *y) {
    const char *a = ((const setting_t *)x)->id, *b = ((const setting_t *)y)->id;
    for (;;) {
        size_t al = strcspn(a, "."), bl = strcspn(b, ".");
        int d = coll_cmp(a, al, b, bl);
        if (d) return d;
        bool ae = !a[al], be = !b[bl];
        if (ae || be) return ae && be ? 0 : ae ? -1 : 1;
        a += al + 1;
        b += bl + 1;
    }
}

static void load_registry(void) {
    pthread_mutex_lock(&g_mu);
    if (!g_registry) {
        size_t len = 0;
        const char *data = embed_get("assets/config/settings-registry.json", &len);
        g_registry = data ? json_parse(data, len, NULL) : NULL;
        if (!json_is_arr(g_registry)) {
            json_free(g_registry);
            g_registry = json_arr();
        }
        g_count = json_len(g_registry);
        g_sorted = xcalloc(g_count + 1, sizeof *g_sorted);
        json_t *e;
        json_arr_foreach(g_registry, i, e) g_sorted[i] = (setting_t){json_get_str(e, "id"), e};
        qsort(g_sorted, g_count, sizeof *g_sorted, setting_cmp);
    }
    pthread_mutex_unlock(&g_mu);
}

static const char *s_type(const setting_t *s) { return json_get_str(s->def, "type"); }
static const json_t *s_default(const setting_t *s) { return json_get(s->def, "default"); }
static bool s_credential(const setting_t *s) { return json_get_bool(s->def, "credential", false); }
static const json_t *s_values(const setting_t *s) { return json_get_arr(s->def, "values"); }
static const char *s_desc(const setting_t *s) { return json_get_str(s->def, "description"); }

/* current value (borrowed): the configured layers, else the registry default; NULL = undefined */
static const json_t *s_value(const setting_t *s) {
    /* omc's built-in defaults table is not an omp layer: unconfigured settings read as omp's registry default */
    if (!strcmp(settings_provenance(s->id), "default")) return s_default(s);
    const json_t *v = settings_get(s->id);
    return v && !json_is_null(v) ? v : s_default(s);
}

static bool values_equal(const json_t *a, const json_t *b) {
    if (!a || !b) return !a && !b;
    return json_equal(a, b);
}

static char *format_value(const setting_t *s, const json_t *v) {
    if (!v || json_is_null(v)) return xstrdup("null");
    if (s_credential(s) && !(json_is_str(v) && !*json_sv(v))) return xstrdup(REDACTED);
    if (json_is_bool(v)) return xstrdup(v->u.b ? "true" : "false");
    return json_dump(v, 0);
}

static const char *provenance_label(const char *p) {
    if (!strcmp(p, "runtime")) return "session override";
    if (!strcmp(p, "overlay")) return "--config overlay";
    if (!strcmp(p, "project")) return "project config";
    if (!strcmp(p, "global")) return "global config";
    if (!strcmp(p, "env")) return "environment variable";
    return "default";
}

static char *lower_dup(const char *s) {
    char *r = xstrdup(s);
    for (char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}

/* parseCfgUrl: segments (split on `/` and `.`, empties dropped; query/fragment ignored) and the /save suffix */
static strvec_t parse_url(const char *url, bool *save) {
    strvec_t segs = {0};
    *save = false;
    const char *p = url;
    while (isspace((unsigned char)*p)) p++;
    p += strlen(CFG_PREFIX);
    size_t n = strcspn(p, "?#");
    char *body = xstrndup(p, n);
    while (n && isspace((unsigned char)body[n - 1])) body[--n] = 0;
    char *tok = body;
    for (char *q = body;; q++) {
        if (*q == '/' || *q == '.' || !*q) {
            bool end = !*q;
            *q = 0;
            if (*tok) strvec_push(&segs, tok);
            if (end) break;
            tok = q + 1;
        }
    }
    free(body);
    if (segs.n > 1 && !strcmp(segs.v[segs.n - 1], "save")) {
        *save = true;
        free(segs.v[--segs.n]);
    }
    return segs;
}

typedef struct {
    char *path;           /* canonical path ("" for the root) */
    const setting_t *leaf; /* NULL: namespace */
    size_t first, count;  /* members [first, first+count) of g_sorted */
} target_t;

/* resolveSegments; false with *err (owned) for a path that names neither a setting nor a namespace */
static bool resolve_segments(const strvec_t *segs, target_t *t, char **err) {
    *err = NULL;
    memset(t, 0, sizeof *t);
    if (!segs->n) {
        t->path = xstrdup("");
        t->count = g_count;
        return true;
    }
    sb_t j = SB_INIT;
    for (size_t i = 0; i < segs->n; i++) sb_appendf(&j, "%s%s", i ? "." : "", segs->v[i]);
    char *lower = lower_dup(sb_cstr(&j));
    size_t ll = strlen(lower);
    bool have_first = false;
    for (size_t i = 0; i < g_count; i++) {
        char *id = lower_dup(g_sorted[i].id);
        if (!strcmp(id, lower)) t->leaf = &g_sorted[i];
        else if (!strncmp(id, lower, ll) && id[ll] == '.') {
            if (!have_first) t->first = i;
            have_first = true;
            t->count++;
        }
        free(id);
    }
    if (t->leaf) t->path = xstrdup(t->leaf->id);
    else if (t->count) t->path = xstrndup(g_sorted[t->first].id, ll);
    else {
        const char **ids = xcalloc(g_count + 1, sizeof *ids);
        size_t *idx = xcalloc(g_count + 1, sizeof *idx);
        for (size_t i = 0; i < g_count; i++) ids[i] = g_sorted[i].id;
        size_t hits = tui_fuzzy_rank(sb_cstr(&j), ids, g_count, idx);
        sb_t sim = SB_INIT;
        for (size_t i = 0; i < hits && i < 8; i++) sb_appendf(&sim, "%s%s", i ? ", " : "", ids[idx[i]]);
        *err = xasprintf("Unknown setting: %s%s%s\nRead " CFG_PREFIX " for the full tree.", sb_cstr(&j), sim.len ? "\nSimilar: " : "",
                         sb_cstr(&sim));
        sb_free(&sim);
        free(ids);
        free(idx);
    }
    free(lower);
    sb_free(&j);
    return *err == NULL;
}

/* first sentence of a description, capped (treeComment) */
static char *first_sentence(const char *d) {
    size_t n = strlen(d), end = n;
    for (size_t i = 0; i < n; i++)
        if ((d[i] == '.' || d[i] == '!' || d[i] == '?') && (!d[i + 1] || isspace((unsigned char)d[i + 1]))) {
            end = i + 1;
            break;
        }
    char *s = xstrndup(d, end);
    size_t cps = 0, cut = 0;
    for (size_t i = 0; s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (cps == TREE_COMMENT_MAX_CHARS - 1) cut = i;
            cps++;
        }
    if (cps > TREE_COMMENT_MAX_CHARS) {
        s[cut] = 0;
        char *r = xasprintf("%s…", s);
        free(s);
        return r;
    }
    return s;
}

static void tree_comment(sb_t *b, const setting_t *s, const json_t *value) {
    sb_t parts = SB_INIT;
    const json_t *vals = s_values(s), *v;
    if (vals) {
        json_arr_foreach(vals, i, v) sb_appendf(&parts, "%s%s", i ? "|" : "", json_sv(v) ? json_sv(v) : "");
    }
    if (!values_equal(value, s_default(s))) {
        char *f = format_value(s, s_default(s));
        sb_appendf(&parts, "%sdefault %s", parts.len ? " · " : "", f);
        free(f);
    }
    const char *d = s_desc(s);
    if (d && *d) {
        char *fs = first_sentence(d);
        sb_appendf(&parts, "%s%s", parts.len ? " · " : "", fs);
        free(fs);
    }
    if (parts.len) sb_appendf(b, "  # %s", sb_cstr(&parts));
    sb_free(&parts);
}

static size_t render_tree(sb_t *b, size_t first, size_t count, const char *prefix) {
    size_t modified = 0;
    strvec_t opened = {0};
    size_t strip = *prefix ? strlen(prefix) + 1 : 0;
    for (size_t k = first; k < first + count; k++) {
        const setting_t *s = &g_sorted[k];
        strvec_t segs = str_split(s->id + strip, ".");
        size_t shared = 0;
        while (shared < opened.n && shared + 1 < segs.n && !strcmp(opened.v[shared], segs.v[shared])) shared++;
        while (opened.n > shared) free(opened.v[--opened.n]);
        for (size_t d = shared; d + 1 < segs.n; d++) {
            if (b->len) sb_putc(b, '\n');
            for (size_t i = 0; i < d; i++) sb_append(b, "  ");
            sb_appendf(b, "%s:", segs.v[d]);
            strvec_push(&opened, segs.v[d]);
        }
        const json_t *value = s_value(s);
        if (!values_equal(value, s_default(s))) modified++;
        if (b->len) sb_putc(b, '\n');
        for (size_t i = 0; i + 1 < segs.n; i++) sb_append(b, "  ");
        char *f = format_value(s, value);
        sb_appendf(b, "%s: %s", segs.v[segs.n - 1], f);
        free(f);
        tree_comment(b, s, value);
        strvec_free(&segs);
    }
    strvec_free(&opened);
    return modified;
}

static void render_leaf(sb_t *b, const setting_t *s) {
    const json_t *value = s_value(s);
    char *f = format_value(s, value), *d = format_value(s, s_default(s));
    sb_appendf(b, "%s: %s\ntype: %s\ndefault: %s\nsource: %s", s->id, f, s_type(s), d,
               provenance_label(settings_provenance(s->id)));
    free(f);
    free(d);
    const json_t *vals = s_values(s), *v;
    if (vals) {
        sb_append(b, "\nvalues: [");
        json_arr_foreach(vals, i, v) sb_appendf(b, "%s%s", i ? ", " : "", json_sv(v) ? json_sv(v) : "");
        sb_append(b, "]");
    }
    if (s_desc(s) && *s_desc(s)) sb_appendf(b, "\ndescription: %s", s_desc(s));
}

char *cfg_resolve(const char *url, json_t **details, char **err) {
    *details = NULL;
    load_registry();
    bool save;
    strvec_t segs = parse_url(url, &save);
    target_t t;
    if (!resolve_segments(&segs, &t, err)) {
        strvec_free(&segs);
        return NULL;
    }
    strvec_free(&segs);
    sb_t b = SB_INIT;
    size_t modified = 0;
    if (t.leaf) {
        render_leaf(&b, t.leaf);
        if (!values_equal(s_value(t.leaf), s_default(t.leaf))) modified++;
    }
    if (t.count) {
        sb_t tree = SB_INIT;
        modified += render_tree(&tree, t.first, t.count, t.path);
        if (b.len) sb_append(&b, "\n\n");
        sb_append(&b, sb_cstr(&tree));
        sb_free(&tree);
    }
    *details = json_obj();
    json_t *c = json_obj();
    json_set_str(c, "path", t.path);
    json_set_int(c, "count", (int64_t)(t.count + (t.leaf ? 1 : 0)));
    json_set_int(c, "modified", (int64_t)modified);
    json_set(*details, "cfg", c);
    free(t.path);
    return sb_detach(&b);
}

/* ---------- writes ---------- */

bool cfg_settings_approval(const tool_env_t *env) { return env && env->can_prompt_user && env->subagent_depth == 0; }

static char *trim(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

/* Setting.parse: owned value or NULL with *err */
static json_t *parse_value(const setting_t *s, const char *text, char **err) {
    char *t = trim(text);
    const char *type = s_type(s);
    json_t *v = NULL;
    if (str_eq(type, "boolean")) {
        char *l = lower_dup(t);
        if (!strcmp(l, "true") || !strcmp(l, "1") || !strcmp(l, "yes") || !strcmp(l, "on")) v = json_bool(true);
        else if (!strcmp(l, "false") || !strcmp(l, "0") || !strcmp(l, "no") || !strcmp(l, "off")) v = json_bool(false);
        else *err = xasprintf("Invalid boolean value: %s. Use true/false, yes/no, on/off, or 1/0", text);
        free(l);
    } else if (str_eq(type, "number")) {
        char *end = NULL;
        double d = *t ? strtod(t, &end) : NAN;
        if (!*t || !end || *end || !isfinite(d)) *err = xasprintf("Invalid number: %s", text);
        else v = json_num(d);
    } else if (str_eq(type, "array") || str_eq(type, "record")) {
        json_t *p = json_parse(t, strlen(t), NULL);
        bool ok = str_eq(type, "array") ? json_is_arr(p) : json_is_obj(p);
        if (!ok) {
            json_free(p);
            *err = xasprintf("Invalid %s JSON: %s", type, text);
        } else v = p;
    } else { /* string, enum */
        char *val = xstrdup(t);
        if (*val == '"') {
            json_t *p = json_parse(val, strlen(val), NULL);
            if (json_is_str(p)) {
                free(val);
                val = xstrdup(json_sv(p));
            }
            json_free(p);
        }
        const json_t *vals = s_values(s), *x;
        bool valid = !vals;
        json_arr_foreach(vals, i, x) valid |= str_eq(json_sv(x), val);
        if (!valid) {
            sb_t l = SB_INIT;
            json_arr_foreach(vals, i, x) sb_appendf(&l, "%s%s", i ? ", " : "", json_sv(x));
            *err = xasprintf("Invalid value: %s. Valid values: %s", text, sb_cstr(&l));
            sb_free(&l);
        } else v = json_str(val);
        free(val);
    }
    free(t);
    return v;
}

static bool layer_in(const char *owner, const char *const *above, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(owner, above[i])) return true;
    return false;
}

/* shadowingLayer against the effective settings: the owning layer when it keeps `value` from taking effect */
static const char *shadowing(const setting_t *s, const json_t *value, const char *const *above, size_t n) {
    const char *owner = settings_provenance(s->id);
    if (!layer_in(owner, above, n)) return NULL;
    const json_t *eff = s_value(s);
    if (!json_is_obj(value) || !json_is_obj(eff)) return values_equal(eff, value) ? NULL : owner;
    const char *k;
    json_t *v;
    json_obj_foreach(value, i, k, v) if (!values_equal(json_get(eff, k), v)) return owner;
    return NULL;
}

/* saveShadowingLayer: a written record key is shadowed only when its current value comes from above the global */
static const char *save_shadowing(const setting_t *s, const json_t *value, const char *const *above, size_t n) {
    const char *owner = settings_provenance(s->id);
    const json_t *eff = s_value(s);
    if (!layer_in(owner, above, n) || !json_is_obj(value) || !json_is_obj(eff)) return shadowing(s, value, above, n);
    const json_t *global = json_path(settings_global(), s->id);
    const char *k;
    json_t *v;
    json_obj_foreach(value, i, k, v) {
        const json_t *cur = json_get(eff, k);
        if (!cur || values_equal(cur, v)) continue;
        if (json_is_obj(global) && values_equal(json_get(global, k), cur)) continue;
        return owner;
    }
    return NULL;
}

typedef enum { ANS_ONCE, ANS_SESSION, ANS_DENY, ANS_TIMEOUT } answer_t;

/* #promptCfgChange through the session's ask UI */
static answer_t ask_user(tool_env_t *env, const char *path, const char *previous, const char *value, bool save,
                         const char *shadowed_by) {
    char *headline = save ? xasprintf("\xf0\x9f\x92\xbe Your agent wants to save `%s` to your config.", path)
                          : xasprintf("\xe2\x9a\x99\xef\xb8\x8f Your agent wants to change `%s` for this session.", path);
    char *warning = shadowed_by ? xasprintf("\n\xe2\x9a\xa0\xef\xb8\x8f Overridden by your %s: the saved value won't take effect here.",
                                            shadowed_by)
                                : xstrdup("");
    char *question = xasprintf("%s\n%s \xe2\x86\x92 %s%s", headline, previous, value, warning);
    json_t *qs = json_arr(), *q = json_obj(), *opts = json_arr();
    json_set_str(q, "id", "cfg");
    json_set_str(q, "question", question);
    const char *labels[] = {OPT_SESSION, OPT_ONCE, OPT_DENY};
    for (size_t i = 0; i < 3; i++) {
        json_t *o = json_obj();
        json_set_str(o, "label", labels[i]);
        json_push(opts, o);
    }
    json_set(q, "options", opts);
    json_set_int(q, "recommended", 1); /* a reflexive Enter approves this change only */
    json_push(qs, q);
    agents_user_wait(env, true);
    json_t *ans = env->ui.ask(env->ui.ud, qs);
    agents_user_wait(env, false);
    answer_t r = ANS_DENY;
    const char *choice = NULL;
    bool timed_out = false;
    const json_t *x;
    json_arr_foreach(json_get_arr(ans, "__results"), i, x) {
        if (!str_eq(json_get_str(x, "id"), "cfg")) continue;
        timed_out = json_get_bool(x, "timedOut", false);
        choice = json_sv(json_at(json_get_arr(x, "selectedOptions"), 0));
    }
    if (!choice && json_is_str(json_get(ans, "cfg"))) choice = json_get_str(ans, "cfg");
    if (!choice && json_is_arr(json_get(ans, "cfg"))) choice = json_sv(json_at(json_get(ans, "cfg"), 0));
    if (timed_out) r = ANS_TIMEOUT;
    else if (str_eq(choice, OPT_SESSION)) r = ANS_SESSION;
    else if (str_eq(choice, OPT_ONCE)) r = ANS_ONCE;
    json_free(ans);
    json_free(qs);
    free(question);
    free(headline);
    free(warning);
    return r;
}

static void write_result(tool_result_t *out, const char *path, const char *previous, const char *value, bool save,
                         const char *outcome, const char *effective, const char *provenance) {
    json_t *c = json_obj();
    json_set_str(c, "path", path);
    json_set_str(c, "previous", previous);
    json_set_str(c, "value", value);
    json_set_bool(c, "save", save);
    char *url = xasprintf(CFG_PREFIX "%s", path);
    for (char *p = url + strlen(CFG_PREFIX); *p; p++)
        if (*p == '.') *p = '/';
    char *save_url = xasprintf("%s/save", url);
    json_set_str(c, "url", url);
    json_set_str(c, "saveUrl", save_url);
    json_set_bool(c, "declined", !strcmp(outcome, "declined"));
    json_set_bool(c, "unchanged", !strcmp(outcome, "unchanged"));
    json_set_bool(c, "saved", !strcmp(outcome, "applied") && save);
    json_set_bool(c, "applied", !strcmp(outcome, "applied") && !save);
    if (effective) json_set_str(c, "effective", effective);
    if (provenance) json_set_str(c, "provenance", provenance);
    char *text = prompt_render("coding-agent/prompts/tools/cfg-write-result.md", c);
    char *tt = trim(text ? text : "");
    tool_result_text(out, tt);
    free(tt);
    free(text);
    json_t *d = json_obj();
    json_set_str(d, "path", path);
    json_set_str(d, "previous", previous);
    json_set_str(d, "value", value);
    json_set_bool(d, "save", save);
    json_set_str(d, "outcome", outcome);
    if (effective) json_set_str(d, "effective", effective);
    out->details = json_obj();
    json_set(out->details, "cfg", d);
    json_free(c);
    free(url);
    free(save_url);
}

int cfg_write(tool_env_t *env, const char *path, const char *content, tool_result_t *out);
int cfg_write(tool_env_t *env, const char *path, const char *content, tool_result_t *out) {
    load_registry();
    if (env->subagent_depth > 0) {
        tool_result_error(out, "Subagents cannot change settings. Report the setting you need changed to the parent agent "
                               "instead of writing " CFG_PREFIX ".");
        return 0;
    }
    if (!cfg_settings_approval(env)) {
        tool_result_error(out, "Changing settings requires user approval, but this session has no interactive UI. Ask the user "
                               "to change the setting themselves.");
        return 0;
    }
    bool save;
    strvec_t segs = parse_url(path, &save);
    target_t t;
    char *err;
    if (!resolve_segments(&segs, &t, &err)) {
        tool_result_error(out, "%s", err);
        free(err);
        strvec_free(&segs);
        return 0;
    }
    strvec_free(&segs);
    if (!t.leaf) {
        char *slashed = xstrdup(t.path);
        for (char *p = slashed; *p; p++)
            if (*p == '.') *p = '/';
        tool_result_error(out, "%s is a namespace; write a single setting, e.g. " CFG_PREFIX "%s%s<key>.",
                          *t.path ? t.path : CFG_PREFIX, slashed, *t.path ? "/" : "");
        free(slashed);
        free(t.path);
        return 0;
    }
    const setting_t *s = t.leaf;
    free(t.path);
    json_t *value = parse_value(s, content ? content : "", &err);
    if (!value) {
        tool_result_error(out, "%s", err);
        free(err);
        return 0;
    }
    json_t *previous = s_value(s) ? json_clone(s_value(s)) : NULL;
    char *prev_s = format_value(s, previous), *val_s = format_value(s, value);
    if (!save && values_equal(previous, value)) {
        write_result(out, s->id, prev_s, val_s, save, "unchanged", NULL, NULL);
        goto done;
    }
    if (!env->ui.ask) {
        tool_result_error(out, "Changing settings requires user approval, but no interactive UI is attached. Ask the user to "
                               "change `%s` themselves.",
                          s->id);
        goto done;
    }
    /* the session shares the persistent settings: its own override never shadows a save */
    static const char *const above_global[] = {"overlay", "project"};
    const char *shadow_owner = save ? save_shadowing(s, value, above_global, 2) : NULL;
    answer_t ans;
    pthread_mutex_lock(&g_mu);
    bool granted = g_grant_session == env && (g_grant_save || !save);
    pthread_mutex_unlock(&g_mu);
    if (granted) {
        ans = ANS_ONCE;
    } else {
        ans = ask_user(env, s->id, prev_s, val_s, save, shadow_owner ? provenance_label(shadow_owner) : NULL);
        if (ans == ANS_SESSION) {
            pthread_mutex_lock(&g_mu);
            g_grant_save = save || (g_grant_session == env && g_grant_save);
            g_grant_session = env;
            pthread_mutex_unlock(&g_mu);
        }
    }
    if (ans == ANS_TIMEOUT) {
        json_t *c = json_obj();
        json_set_str(c, "path", s->id);
        json_set_str(c, "previous", prev_s);
        char *text = prompt_render("coding-agent/prompts/tools/cfg-approval-timeout.md", c);
        char *tt = trim(text ? text : "");
        tool_result_error(out, "%s", tt);
        free(tt);
        free(text);
        json_free(c);
        goto done;
    }
    if (ans == ANS_DENY) {
        write_result(out, s->id, prev_s, val_s, save, "declined", NULL, NULL);
        goto done;
    }
    if (!save) {
        settings_set_runtime(s->id, json_clone(value));
        write_result(out, s->id, prev_s, val_s, save, "applied", NULL, NULL); /* no env layer can outrank it */
        goto done;
    }
    if (settings_set_global(s->id, json_clone(value)) != 0) {
        tool_result_error(out, "Failed to save `%s` to %s", s->id, settings_global_path());
        goto done;
    }
    settings_unset_runtime(s->id);
    const char *layer = shadowing(s, value, above_global, 2);
    char *eff = layer ? format_value(s, s_value(s)) : NULL;
    write_result(out, s->id, prev_s, val_s, save, "applied", eff, layer ? provenance_label(layer) : NULL);
    free(eff);
done:
    json_free(value);
    json_free(previous);
    free(prev_s);
    free(val_s);
    return 0;
}

/* ---------- registry accessors (omc config) ---------- */

const json_t *cfg_registry(void) {
    load_registry();
    return g_registry;
}

const json_t *cfg_setting(const char *id) {
    load_registry();
    json_t *e;
    json_arr_foreach(g_registry, i, e) if (str_eq(json_get_str(e, "id"), id)) return e;
    return NULL;
}

json_t *cfg_parse(const json_t *setting, const char *text, char **err) {
    setting_t s = {json_get_str(setting, "id"), setting};
    return parse_value(&s, text, err);
}

const json_t *cfg_value(const json_t *setting) {
    setting_t s = {json_get_str(setting, "id"), setting};
    return s_value(&s);
}
