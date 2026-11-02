/* export_html.c - see export_html.h */
#include "session/export_html.h"

#include "coding/app.h"
#include "config/settings.h"
#include "core/embed.h"
#include "core/fs.h"
#include "session/blobs.h"
#include "session/session.h"
#include "session/trace.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#define EXPORT_APP_NAME "omc"

/* index.ts getTemplate (compiled-binary branch) */
#define MARKED_URL "https://stencil.so/assets/omp/marked.333561b9.js"
#define MARKED_INTEGRITY "sha256-MzVhuUicTTOmBQF03OgI9HBhlGcICMhDRSPIdD9aYmM="
#define HIGHLIGHT_URL "https://stencil.so/assets/omp/highlight.837a6fa5.js"
#define HIGHLIGHT_INTEGRITY "sha256-g3pvpbDHNrUrveKythkPMF2j/J7UFoHbUyFQcFe1yEY="

/* ---------- loading ---------- */

static char *gunzip_file(const char *path, size_t *len) {
    size_t n;
    char *raw = fs_read(path, &n);
    if (!raw) return NULL;
    z_stream z = {0};
    if (inflateInit2(&z, 15 + 16) != Z_OK) {
        free(raw);
        return NULL;
    }
    sb_t out = SB_INIT;
    char buf[65536];
    z.next_in = (Bytef *)raw;
    z.avail_in = (uInt)n;
    int rc;
    do {
        z.next_out = (Bytef *)buf;
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
        sb_appendn(&out, buf, sizeof buf - z.avail_out);
        if (out.len > ((size_t)1 << 31)) break; /* bounded */
    } while (rc != Z_STREAM_END);
    inflateEnd(&z);
    free(raw);
    if (rc != Z_STREAM_END) {
        sb_free(&out);
        return NULL;
    }
    *len = out.len;
    return sb_detach(&out);
}

/* session-title-slot.ts parseTitleSlotObject */
static json_t *title_slot(const json_t *v) {
    if (!json_is_obj(v) || !str_eq(json_get_str(v, "type"), "title")) return NULL;
    json_t *ver = json_get(v, "v");
    if (!json_is_num(ver) || ver->u.num.d != 1) return NULL;
    if (!json_get_str(v, "title") || !json_get_str(v, "updatedAt") || !json_get_str(v, "pad")) return NULL;
    json_t *src = json_get(v, "source");
    if (src && !(str_eq(json_sv(src), "auto") || str_eq(json_sv(src), "user"))) return NULL;
    return (json_t *)v;
}

static void obj_del(json_t *o, const char *key) {
    for (size_t i = 0; json_is_obj(o) && i < o->u.obj.n; i++) {
        if (strcmp(o->u.obj.keys[i], key)) continue;
        free(o->u.obj.keys[i]);
        json_free(o->u.obj.vals[i]);
        memmove(o->u.obj.keys + i, o->u.obj.keys + i + 1, (o->u.obj.n - i - 1) * sizeof *o->u.obj.keys);
        memmove(o->u.obj.vals + i, o->u.obj.vals + i + 1, (o->u.obj.n - i - 1) * sizeof *o->u.obj.vals);
        o->u.obj.n--;
        i--;
    }
}

/* parseSessionContent: entries incl. the header first; *malformed counts unparseable records */
static json_t *parse_session_content(const char *data, size_t len, size_t *malformed, json_t **slot_out) {
    json_t *all = json_arr();
    *malformed = 0;
    *slot_out = NULL;
    const char *p = data, *end = data + len;
    const char *nl = memchr(p, '\n', len);
    if (nl) {
        char *first = xstrndup(p, (size_t)(nl - p));
        json_t *v = json_parse_str(first);
        free(first);
        if (title_slot(v)) {
            *slot_out = v;
            p = nl + 1;
        } else json_free(v);
    }
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        size_t n = e ? (size_t)(e - p) : (size_t)(end - p);
        bool blank = true;
        for (size_t i = 0; i < n && blank; i++) blank = p[i] == ' ' || p[i] == '\t' || p[i] == '\r';
        if (!blank) {
            json_t *v = json_parse(p, n, NULL);
            if (v) json_push(all, v);
            else (*malformed)++;
        }
        if (!e) break;
        p = e + 1;
    }
    /* applyTitleSlot */
    json_t *h = json_at(all, 0);
    if (*slot_out && str_eq(json_get_str(h, "type"), "session") && json_get_str(h, "id")) {
        const char *t = json_get_str(*slot_out, "title");
        if (t && *t) json_set_str(h, "title", t);
        else obj_del(h, "title");
        const char *src = json_get_str(*slot_out, "source");
        if (src && *src) json_set_str(h, "titleSource", src);
        else obj_del(h, "titleSource");
    }
    return all;
}

static char *read_transcript(const char *path, size_t *len) {
    return str_ends(path, ".gz") ? gunzip_file(path, len) : fs_read(path, len);
}

int export_load_session_file(const char *path, json_t **header, json_t **entries, char **err) {
    *header = NULL;
    *entries = NULL;
    size_t len;
    char *data = read_transcript(path, &len);
    if (!data) {
        *err = errno == ENOENT ? xasprintf("File not found: %s", path) : xasprintf("%s: %s", path, strerror(errno));
        return -1;
    }
    size_t bad;
    json_t *slot;
    json_t *all = parse_session_content(data, len, &bad, &slot);
    free(data);
    json_free(slot);
    json_t *out = json_arr(), *e;
    json_arr_foreach(all, i, e) {
        if (str_eq(json_get_str(e, "type"), "session")) {
            if (!*header) *header = json_clone(e);
            continue;
        }
        json_push(out, json_clone(e));
    }
    json_free(all);
    *entries = out;
    return 0;
}

/* sessionHeaderForExport */
static json_t *header_for_export(const json_t *h) {
    if (!json_is_obj(h)) return json_null();
    json_t *c = json_clone(h);
    obj_del(c, "previousSessionFiles");
    obj_del(c, "providerPromptCacheKey");
    obj_del(c, "taskPromptCacheKey");
    return c;
}

/* session-entries.ts resolveSessionLeafId */
static const char *entry_leaf(const json_t *e) {
    json_t *a = json_get(e, "activeLeafId");
    if (a) return json_is_str(a) ? json_sv(a) : NULL; /* null clears */
    return json_get_str(e, "id");
}

static json_t *resolve_leaf_id(const json_t *entries) {
    size_t n = json_len(entries);
    if (!n) return json_null();
    const json_t *last = json_at(entries, n - 1);
    bool explicit_null = json_get(last, "activeLeafId") && !json_is_str(json_get(last, "activeLeafId"));
    const char *leaf = entry_leaf(last);
    if (explicit_null) return json_null();
    if (!leaf) return json_null();
    if (str_eq(leaf, json_get_str(last, "id"))) return json_str(leaf);
    json_t *e;
    json_arr_foreach(entries, i, e) if (str_eq(json_get_str(e, "id"), leaf)) return json_str(leaf);
    for (size_t i = n; i > 0; i--) {
        const json_t *x = json_at(entries, i - 1);
        const char *l = entry_leaf(x);
        if (l && str_eq(l, json_get_str(x, "id"))) return json_str(l);
    }
    return json_null();
}

static bool is_advisor_name(const char *name) {
    return !strcmp(name, "__advisor.jsonl") || (str_starts(name, "__advisor.") && str_ends(name, ".jsonl"));
}

static void collect_dir(const char *dir, const char *parent_key, json_t *out, int depth) {
    if (depth > 32) return;
    dirlist_t l = {0};
    if (fs_list_dir(dir, &l) != 0) return;
    strvec_t files = {0}, dirs = {0};
    for (size_t i = 0; i < l.n; i++) {
        if (l.v[i].is_link) continue;
        if (l.v[i].is_dir) strvec_push(&dirs, l.v[i].name);
        else strvec_push(&files, l.v[i].name);
    }
    dirlist_free(&l);
    strvec_sort(&files);
    for (size_t i = 0; i < files.n; i++) {
        const char *name = files.v[i];
        bool compressed = str_ends(name, ".jsonl.gz");
        char *tname = compressed ? xstrndup(name, strlen(name) - 3) : xstrdup(name);
        bool skip = !str_ends(tname, ".jsonl") || strstr(name, ".bak");
        (void)is_advisor_name; /* advisors are included for exports */
        if (!skip && compressed) {
            for (size_t k = 0; k < files.n && !skip; k++) skip = !strcmp(files.v[k], tname);
        }
        if (skip) {
            free(tname);
            continue;
        }
        char *agent_id = xstrndup(tname, strlen(tname) - 6);
        char *key = parent_key ? xasprintf("%s/%s", parent_key, agent_id) : xstrdup(agent_id);
        char *path = path_join(dir, name);
        size_t len;
        char *data = read_transcript(path, &len);
        free(path);
        if (data) {
            size_t bad;
            json_t *slot;
            json_t *all = parse_session_content(data, len, &bad, &slot);
            free(data);
            json_free(slot);
            bool invalid = json_len(all) > 0 ? !(str_eq(json_get_str(json_at(all, 0), "type"), "session") &&
                                                 json_get_str(json_at(all, 0), "id"))
                                             : bad > 0;
            if (!invalid && json_len(all) > 0) {
                json_t *header = NULL, *entries = json_arr(), *e;
                json_arr_foreach(all, k, e) {
                    if (str_eq(json_get_str(e, "type"), "session")) {
                        if (!header) header = e;
                    } else json_push(entries, json_clone(e));
                }
                json_t *sub = json_obj();
                json_set_str(sub, "agentId", agent_id);
                json_set(sub, "parent", parent_key ? json_str(parent_key) : json_null());
                json_set(sub, "header", header ? header_for_export(header) : json_null());
                json_t *leaf = resolve_leaf_id(entries);
                json_set(sub, "entries", entries);
                json_set(sub, "leafId", leaf);
                char *tomb = xasprintf("%s.tombstone", tname);
                char *tomb_gz = xasprintf("%s.tombstone", name);
                bool aborted = false;
                for (size_t k = 0; k < files.n; k++)
                    if (!strcmp(files.v[k], tomb) || (compressed && !strcmp(files.v[k], tomb_gz))) aborted = true;
                free(tomb);
                free(tomb_gz);
                json_set_bool(sub, "aborted", aborted);
                json_set(sub, "malformedRecords", json_int((int64_t)bad));
                json_set(out, key, sub);
            }
            json_free(all);
        }
        bool has_dir = false;
        for (size_t k = 0; k < dirs.n && !has_dir; k++) has_dir = !strcmp(dirs.v[k], agent_id);
        if (has_dir && strcmp(agent_id, ".") && strcmp(agent_id, "..")) {
            char *sub = path_join(dir, agent_id);
            collect_dir(sub, key, out, depth + 1);
            free(sub);
        }
        free(key);
        free(agent_id);
        free(tname);
    }
    strvec_free(&files);
    strvec_free(&dirs);
}

json_t *export_collect_sub_sessions(const char *session_file) {
    json_t *out = json_obj();
    char *t = str_ends(session_file, ".jsonl.gz") ? xstrndup(session_file, strlen(session_file) - 3) : xstrdup(session_file);
    if (str_ends(t, ".jsonl")) {
        t[strlen(t) - 6] = 0;
        collect_dir(t, NULL, out, 0);
    }
    free(t);
    return out;
}

/* ---------- rendering ---------- */

static void replace_once(sb_t *b, const char *needle, const char *with) {
    char *at = strstr(b->s, needle);
    if (!at) return;
    size_t pos = (size_t)(at - b->s), nl = strlen(needle);
    sb_t r = SB_INIT;
    sb_appendn(&r, b->s, pos);
    sb_append(&r, with);
    sb_append(&r, b->s + pos + nl);
    sb_free(b);
    *b = r;
}

/* encodeURIComponent */
static char *uri_component(const char *s) {
    sb_t b = SB_INIT;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || strchr("-_.!~*'()", *p)) sb_putc(&b, (char)*p);
        else sb_appendf(&b, "%%%02X", *p);
    }
    return sb_detach(&b);
}

static char *template_doc(void) {
    size_t len = 0;
    const char *tpl = embed_get("assets/export/template.html.txt", &len);
    const char *vj = embed_get("assets/export/viewer.generated.json", NULL);
    json_t *viewer = vj ? json_parse_str(vj) : NULL;
    sb_t b = SB_INIT;
    sb_appendn(&b, tpl ? tpl : "", tpl ? len : 0);
    replace_once(&b, "{{MARKED_URL}}", MARKED_URL);
    replace_once(&b, "{{MARKED_INTEGRITY}}", MARKED_INTEGRITY);
    replace_once(&b, "{{HIGHLIGHT_URL}}", HIGHLIGHT_URL);
    replace_once(&b, "{{HIGHLIGHT_INTEGRITY}}", HIGHLIGHT_INTEGRITY);
    char *ver = uri_component(EXPORT_VIEWER_VERSION);
    char *script = xasprintf("<script defer src=\"https://build.stencil.so/web/omp/%s/%s\" integrity=\"%s\" crossorigin=\"anonymous\" "
                             "onerror=\"document.getElementById('viewer-status').textContent='Could not load the session viewer. "
                             "Connect to the internet and reload this file.'\"></script>",
                             ver, json_get_str(viewer, "file") ? json_get_str(viewer, "file") : "",
                             json_get_str(viewer, "integrity") ? json_get_str(viewer, "integrity") : "");
    replace_once(&b, "<viewer-script/>", script);
    free(script);
    free(ver);
    json_free(viewer);
    return sb_detach(&b);
}

char *export_render_html(json_t *session_data, const char *theme_styles) {
    json_t *trace = session_trace_build(json_get(session_data, "header"), json_get(session_data, "entries"),
                                        json_get(session_data, "subSessions"));
    json_set(session_data, "trace", trace);
    char *js = json_dump_js(session_data);
    char *b64 = base64_encode(js, strlen(js));
    free(js);
    sb_t b = SB_INIT;
    char *tpl = template_doc();
    sb_append(&b, tpl);
    free(tpl);
    char *style = theme_styles ? xasprintf("<style>%s</style>", theme_styles) : xstrdup("");
    replace_once(&b, "<theme-vars/>", style);
    free(style);
    replace_once(&b, "{{SESSION_DATA}}", b64);
    free(b64);
    return sb_detach(&b);
}

/* ---------- --themes palette (index.ts generateThemeVars("theme") + web-palette.ts) ---------- */

static const unsigned char ANSI16[16][3] = {{0, 0, 0},     {128, 0, 0},   {0, 128, 0},   {128, 128, 0}, {0, 0, 128},   {128, 0, 128},
                                            {0, 128, 128}, {192, 192, 192}, {128, 128, 128}, {255, 0, 0}, {0, 255, 0},   {255, 255, 0},
                                            {0, 0, 255},   {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
static const int CUBE[6] = {0, 95, 135, 175, 215, 255};

static bool palette_rgb(double idx, int rgb[3]) {
    if (idx != floor(idx) || idx < 0 || idx > 255) return false;
    int i = (int)idx;
    if (i < 16) {
        for (int k = 0; k < 3; k++) rgb[k] = ANSI16[i][k];
    } else if (i < 232) {
        int n = i - 16;
        rgb[0] = CUBE[(n / 36) % 6];
        rgb[1] = CUBE[(n / 6) % 6];
        rgb[2] = CUBE[n % 6];
    } else rgb[0] = rgb[1] = rgb[2] = 8 + (i - 232) * 10;
    return true;
}

static char *ansi_hex(double idx) {
    int rgb[3];
    if (!palette_rgb(idx, rgb)) return NULL;
    return xasprintf("#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
}

/* colorLuma; -1 when unparseable */
static double color_luma(const json_t *v) {
    int rgb[3];
    if (json_is_num(v)) {
        if (!palette_rgb(v->u.num.d, rgb)) return -1;
    } else if (json_is_str(v) && json_sv(v)[0] == '#' && (strlen(json_sv(v)) == 4 || strlen(json_sv(v)) == 7)) {
        const char *h = json_sv(v) + 1;
        char t[3] = {0};
        for (int k = 0; k < 3; k++) {
            if (strlen(h) == 3) t[0] = t[1] = h[k];
            else t[0] = h[2 * k], t[1] = h[2 * k + 1];
            char *endp;
            long x = strtol(t, &endp, 16);
            if (*endp) return -1;
            rgb[k] = (int)x;
        }
    } else return -1;
    return (0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]) / 255.0;
}

static json_t *load_theme_json(const char *name, char **err) {
    if (!name || !*name || strchr(name, '/') || strstr(name, "..")) {
        *err = xasprintf("Theme not found: %s", name ? name : "");
        return NULL;
    }
    char *rel = xasprintf("assets/tui/themes/%s.json", name);
    size_t len = 0;
    const char *data = embed_get(rel, &len);
    free(rel);
    if (data) return json_parse(data, len, NULL);
    rel = xasprintf("themes/%s.json", name);
    char *path = omc_agent_path(rel);
    free(rel);
    char *raw = fs_read(path, &len);
    free(path);
    json_t *j = raw ? json_parse(raw, len, NULL) : NULL;
    free(raw);
    if (!j) *err = xasprintf("Theme not found: %s", name);
    return j;
}

/* resolveVarRefs; NULL on a missing/circular reference */
static const json_t *resolve_var(const json_t *value, const json_t *vars, int depth) {
    if (!value || depth > 64) return NULL;
    if (json_is_num(value) || !json_is_str(value) || !*json_sv(value) || json_sv(value)[0] == '#') return value;
    const json_t *v = json_get(vars, json_sv(value));
    return v ? resolve_var(v, vars, depth + 1) : NULL;
}

static char *resolve_export(const json_t *v, const json_t *vars) {
    if (!v) return NULL;
    if (json_is_num(v)) return ansi_hex(v->u.num.d);
    const char *s = json_sv(v);
    if (!s) return NULL;
    if (!*s || s[0] == '#') return xstrdup(s);
    const char *name = s[0] == '$' ? s + 1 : s;
    if (json_get(vars, name)) {
        json_t tmp = {.type = JSON_STRING};
        tmp.u.str.s = (char *)name;
        tmp.u.str.len = strlen(name);
        const json_t *r = resolve_var(&tmp, vars, 0);
        if (!r) return xstrdup(s);
        return json_is_num(r) ? ansi_hex(r->u.num.d) : xstrdup(json_sv(r));
    }
    return xstrdup(s);
}

/* parse "#rrggbb"/"rgb(r, g, b)" */
static bool parse_color(const char *c, int rgb[3]) {
    if (c[0] == '#' && strlen(c) == 7) {
        for (int k = 0; k < 3; k++) {
            char t[3] = {c[1 + 2 * k], c[2 + 2 * k], 0}, *e;
            rgb[k] = (int)strtol(t, &e, 16);
            if (*e) return false;
        }
        return true;
    }
    return sscanf(c, "rgb(%d, %d, %d)", &rgb[0], &rgb[1], &rgb[2]) == 3 || sscanf(c, "rgb(%d,%d,%d)", &rgb[0], &rgb[1], &rgb[2]) == 3;
}

static double lin(int c) {
    double s = c / 255.0;
    return s <= 0.03928 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static char *adjust(const char *color, double f) {
    int rgb[3];
    if (!parse_color(color, rgb)) return xstrdup(color);
    int a[3];
    for (int k = 0; k < 3; k++) a[k] = (int)fmin(255, fmax(0, floor(rgb[k] * f + 0.5)));
    return xasprintf("rgb(%d, %d, %d)", a[0], a[1], a[2]);
}

static char *theme_vars(const char *name, char **err) {
    json_t *t = load_theme_json(name, err);
    if (!t) return NULL;
    json_t *vars = json_get(t, "vars"), *colors = json_get(t, "colors"), *exp = json_get(t, "export");
    char *page = resolve_export(json_get(exp, "pageBg"), vars), *card = resolve_export(json_get(exp, "cardBg"), vars),
         *info = resolve_export(json_get(exp, "infoBg"), vars);
    /* getHtmlDefaultTextForSurface(cardBg ?? pageBg ?? userMessageBg) */
    json_t *surface = NULL;
    const json_t *umb = resolve_var(json_get(colors, "userMessageBg"), vars, 0);
    if (card) surface = json_str(card);
    else if (page) surface = json_str(page);
    else if (umb) surface = json_clone(umb);
    double lum = surface ? color_luma(surface) : -1;
    json_free(surface);
    const char *default_text = lum > 0.5 ? "#000000" : "#e5e5e7";
    sb_t b = SB_INIT;
    const char *k;
    json_t *v;
    char *umb_css = NULL;
    json_obj_foreach(colors, i, k, v) {
        const json_t *r = resolve_var(v, vars, 0);
        if (!r) {
            *err = xasprintf("Variable reference not found: %s", json_sv(v) ? json_sv(v) : "");
            sb_free(&b);
            free(page), free(card), free(info), free(umb_css);
            json_free(t);
            return NULL;
        }
        char *css = json_is_num(r) ? ansi_hex(r->u.num.d) : !*json_sv(r) ? xstrdup(default_text) : xstrdup(json_sv(r));
        if (!css) css = xstrdup("");
        sb_appendf(&b, "%s--%s: %s;", b.len ? " " : "", k, css);
        if (!strcmp(k, "userMessageBg")) {
            free(umb_css);
            umb_css = xstrdup(css);
        }
        free(css);
    }
    /* deriveExportColors(colors.userMessageBg || "#343541") */
    const char *base = umb_css && *umb_css ? umb_css : "#343541";
    int rgb[3];
    char *dpage, *dcard, *dinfo;
    if (!parse_color(base, rgb)) {
        dpage = xstrdup("rgb(24, 24, 30)");
        dcard = xstrdup("rgb(30, 30, 36)");
        dinfo = xstrdup("rgb(60, 55, 40)");
    } else if (0.2126 * lin(rgb[0]) + 0.7152 * lin(rgb[1]) + 0.0722 * lin(rgb[2]) > 0.5) {
        dpage = adjust(base, 0.96);
        dcard = xstrdup(base);
        dinfo = xasprintf("rgb(%d, %d, %d)", MIN(255, rgb[0] + 10), MIN(255, rgb[1] + 5), MAX(0, rgb[2] - 20));
    } else {
        dpage = adjust(base, 0.7);
        dcard = adjust(base, 0.85);
        dinfo = xasprintf("rgb(%d, %d, %d)", MIN(255, rgb[0] + 20), MIN(255, rgb[1] + 15), rgb[2]);
    }
    sb_appendf(&b, " --body-bg: %s; --container-bg: %s; --info-bg: %s;", page ? page : dpage, card ? card : dcard, info ? info : dinfo);
    free(dpage), free(dcard), free(dinfo), free(page), free(card), free(info), free(umb_css);
    json_free(t);
    return sb_detach(&b);
}

char *export_theme_styles(const char *dark_theme, const char *light_theme, char **err) {
    char *dark = theme_vars(dark_theme, err);
    if (!dark) return NULL;
    char *light = theme_vars(light_theme, err);
    if (!light) {
        free(dark);
        return NULL;
    }
    char *s = xasprintf(":root, :root[data-theme=\"dark\"] { color-scheme: dark; %s }\n"
                        ":root[data-theme=\"light\"] { color-scheme: light; %s }\n"
                        "@media (prefers-color-scheme: light) { :root:not([data-theme]) { color-scheme: light; %s } }",
                        dark, light, light);
    free(dark);
    free(light);
    return s;
}

/* ---------- entry points ---------- */

char *export_default_name(const char *session_file) {
    const char *base = path_basename(session_file);
    size_t n = strlen(base);
    if (n >= 6 && !strcmp(base + n - 6, ".jsonl")) n -= 6;
    return xasprintf(EXPORT_APP_NAME "-session-%.*s.html", (int)n, base);
}

/* Bun.write: created 0666 under the umask (fs_write uses 0644) */
static int write_out(const char *path, const char *html, char **err) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    int rc = fd < 0 ? -errno : 0;
    if (fd >= 0) {
        rc = fs_write(path, html, strlen(html));
        close(fd);
    }
    if (rc) *err = xasprintf("%s: %s", path, strerror(-rc));
    return rc ? -1 : 0;
}

int export_html_from_file(const char *session_file, const char *output_path, char **written_path, char **err) {
    *written_path = NULL;
    json_t *header, *entries;
    if (export_load_session_file(session_file, &header, &entries, err) != 0) return -1;
    json_t *e;
    json_arr_foreach(entries, i, e) session_blobs_resolve(e);
    json_t *d = json_obj();
    json_set(d, "header", header_for_export(header));
    json_t *leaf = resolve_leaf_id(entries);
    json_set(d, "entries", entries);
    json_set(d, "leafId", leaf);
    json_t *subs = export_collect_sub_sessions(session_file);
    if (json_len(subs) || (json_is_obj(subs) && subs->u.obj.n)) json_set(d, "subSessions", subs);
    else json_free(subs);
    json_free(header);
    char *html = export_render_html(d, NULL);
    json_free(d);
    char *out = output_path && *output_path ? xstrdup(output_path) : export_default_name(session_file);
    int rc = write_out(out, html, err);
    free(html);
    if (rc) free(out);
    else *written_path = out;
    return rc;
}

/* buildSessionData(sm): the live header (title slot folded in), entries and leaf */
static json_t *session_data(const session_t *s) {
    json_t *header = json_clone(s->header);
    if (s->title && *s->title) json_set_str(header, "title", s->title);
    if (s->title_source && *s->title_source) json_set_str(header, "titleSource", s->title_source);
    json_t *d = json_obj();
    json_set(d, "header", header_for_export(header));
    json_free(header);
    json_set(d, "entries", json_clone(s->entries));
    json_set(d, "leafId", s->leaf_id ? json_str(s->leaf_id) : json_null());
    return d;
}

char *session_export_html(const session_t *s) {
    json_t *d = session_data(s);
    if (s->file) {
        json_t *subs = export_collect_sub_sessions(s->file);
        if (json_is_obj(subs) && subs->u.obj.n) json_set(d, "subSessions", subs);
        else json_free(subs);
    }
    char *html = export_render_html(d, NULL);
    json_free(d);
    return html;
}

int app_export_html(app_t *a, const char *output_path, bool use_user_themes, char **written_path, char **err) {
    *written_path = NULL;
    const char *file = app_session_file(a);
    if (!file) {
        *err = xstrdup("Cannot export in-memory session to HTML");
        return -1;
    }
    char *styles = NULL;
    if (use_user_themes) {
        styles = export_theme_styles(settings_str("theme.dark", "titanium"), settings_str("theme.light", "light"), err);
        if (!styles) return -1;
    }
    json_t *d = session_data(a->session);
    sb_t sp = SB_INIT;
    json_t *x;
    json_arr_foreach(a->agent->system_prompt, i, x) {
        if (i) sb_append(&sp, "\n\n");
        sb_append(&sp, json_sv(x) ? json_sv(x) : "");
    }
    json_set_str(d, "systemPrompt", sb_cstr(&sp));
    sb_free(&sp);
    json_t *tools = json_arr();
    for (size_t i = 0; i < a->agent->tools.n; i++) {
        json_t *t = json_obj();
        json_set_str(t, "name", a->agent->tools.v[i]->name);
        json_set_str(t, "description", a->agent->tools.v[i]->description ? a->agent->tools.v[i]->description : "");
        json_push(tools, t);
    }
    json_set(d, "tools", tools);
    json_t *subs = export_collect_sub_sessions(file);
    if (json_is_obj(subs) && subs->u.obj.n) json_set(d, "subSessions", subs);
    else json_free(subs);
    char *html = export_render_html(d, styles);
    json_free(d);
    free(styles);
    char *out = output_path && *output_path ? xstrdup(output_path) : export_default_name(file);
    int rc = write_out(out, html, err);
    free(html);
    if (rc) free(out);
    else *written_path = out;
    return rc;
}
