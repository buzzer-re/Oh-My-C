/* test_settings_audit.c - WP-26 settings honesty (docs/gaps.md §3), the rules of build-tools/settings_audit.py:
 * - a setting key omc reads through a settings call must be in omp's registry
 *   (assets/config/settings-registry.json) or be marked omc-only in build-tools/settings_audit.tsv;
 * - a registry key nothing reads must be annotated inert or oos (⛔) there;
 * - a key annotated inert must not be read (reclassify it as wired/partial when it gets wired);
 * - every exact key in the annotations must exist in the registry.
 * A "read" is the key as a string literal first argument of a call, anywhere in src/ outside the generic
 * settings plumbing. Run from the repository root (make test does). */
#include "core/core.h"
#include "core/fs.h"
#include "core/json.h"

#include <ctype.h>
#include <stdio.h>

static int fails;

static const char *const PLUMBING[] = {"src/config/settings.c", "src/ui/settings.c", "src/tools/cfg_url.c",
                                       "src/cli/sub_config.c", NULL};

typedef struct {
    char *pat, *status;
} ann_t;
static VEC(ann_t) g_ann;

typedef struct {
    char *callee, *lit, *where;
} call_t;
static VEC(call_t) g_calls;

static bool is_plumbing(const char *rel) {
    for (size_t i = 0; PLUMBING[i]; i++)
        if (!strcmp(rel, PLUMBING[i])) return true;
    return false;
}

static void scan_file(const char *rel) {
    size_t len;
    char *s = fs_read(rel, &len);
    if (!s) return;
    int line = 1;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') line++;
        if (s[i] != '"') continue;
        /* `ident ( "` with optional whitespace (newlines included) */
        size_t j = i;
        while (j > 0 && isspace((unsigned char)s[j - 1])) j--;
        if (j == 0 || s[j - 1] != '(') continue;
        j--;
        while (j > 0 && isspace((unsigned char)s[j - 1])) j--;
        size_t end = j;
        while (j > 0 && (isalnum((unsigned char)s[j - 1]) || s[j - 1] == '_')) j--;
        if (j == end || (j > 0 && (isalnum((unsigned char)s[j - 1]) || s[j - 1] == '_'))) continue;
        size_t k = i + 1;
        while (k < len && s[k] != '"' && s[k] != '\\' && s[k] != '\n') k++;
        if (k >= len || s[k] != '"' || k == i + 1) continue;
        call_t c = {xstrndup(s + j, end - j), xstrndup(s + i + 1, k - i - 1), xasprintf("%s:%d", rel, line)};
        vec_push(&g_calls, c);
        i = k; /* the closing quote is not a new literal */
    }
    free(s);
}

static void scan_dir(const char *dir) {
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return;
    for (size_t i = 0; i < dl.n; i++) {
        char *p = xasprintf("%s/%s", dir, dl.v[i].name);
        if (dl.v[i].is_dir) scan_dir(p);
        else if ((str_ends(p, ".c") || str_ends(p, ".h")) && !is_plumbing(p)) scan_file(p);
        free(p);
    }
    dirlist_free(&dl);
}

static bool pat_matches(const char *pat, const char *key) {
    size_t n = strlen(pat);
    if (n > 2 && !strcmp(pat + n - 2, ".*")) return !strncmp(key, pat, n - 1) && key[n - 1];
    return !strcmp(pat, key);
}

/* first matching annotation (omc-only entries excluded unless `omc_only`) */
static const ann_t *annotation(const char *key, bool omc_only) {
    for (size_t i = 0; i < g_ann.n; i++) {
        if (str_eq(g_ann.v[i].status, "omc-only") != omc_only) continue;
        if (pat_matches(g_ann.v[i].pat, key)) return &g_ann.v[i];
    }
    return NULL;
}

static bool keylike(const char *s) {
    if (!isalpha((unsigned char)*s)) return false;
    bool dot = true;
    for (; *s; s++) {
        if (*s == '.') {
            if (dot) return false;
            dot = true;
        } else if (isalnum((unsigned char)*s) || *s == '_' || *s == '-') dot = false;
        else return false;
    }
    return !dot;
}

static void load_annotations(void) {
    size_t len;
    char *t = fs_read("build-tools/settings_audit.tsv", &len);
    if (!t) {
        fprintf(stderr, "FAIL: cannot read build-tools/settings_audit.tsv (run from the repo root)\n");
        fails++;
        return;
    }
    strvec_t lines = str_split(t, "\n");
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        if (!*l || *l == '#') continue;
        strvec_t f = str_split(l, "\t");
        bool ok = f.n >= 2 && (str_eq(f.v[1], "wired") || str_eq(f.v[1], "partial") || str_eq(f.v[1], "inert") ||
                               str_eq(f.v[1], "oos") || str_eq(f.v[1], "omc-only"));
        if (!ok) {
            fprintf(stderr, "FAIL: settings_audit.tsv:%zu: bad line: %s\n", i + 1, l);
            fails++;
        } else {
            ann_t a = {xstrdup(f.v[0]), xstrdup(f.v[1])};
            vec_push(&g_ann, a);
        }
        strvec_free(&f);
    }
    strvec_free(&lines);
    free(t);
}

int main(void) {
    size_t len;
    char *raw = fs_read("assets/config/settings-registry.json", &len);
    json_t *reg = raw ? json_parse(raw, len, NULL) : NULL;
    free(raw);
    if (!json_is_arr(reg) || !json_len(reg)) {
        fprintf(stderr, "FAIL: cannot load assets/config/settings-registry.json (run from the repo root)\n");
        return 1;
    }
    load_annotations();
    scan_dir("src");
    if (g_calls.n < 1000) {
        fprintf(stderr, "FAIL: only %zu call literals scanned under src/\n", g_calls.n);
        fails++;
    }
    hmap_t keys, parents, records;
    hmap_init(&keys), hmap_init(&parents), hmap_init(&records);
    json_t *e;
    json_arr_foreach(reg, i, e) {
        const char *id = json_get_str(e, "id");
        if (!id) continue;
        hmap_put(&keys, id, (void *)1);
        if (str_eq(json_get_str(e, "type"), "record")) hmap_put(&records, id, (void *)1);
        char *p = xstrdup(id), *dot;
        while ((dot = strrchr(p, '.'))) {
            *dot = 0;
            hmap_put(&parents, p, (void *)1);
        }
        free(p);
    }
    /* registry keys: read, or annotated */
    size_t n_read = 0, n_ann = 0;
    json_arr_foreach(reg, i, e) {
        const char *id = json_get_str(e, "id");
        const char *where = NULL;
        for (size_t k = 0; k < g_calls.n && !where; k++)
            if (!strcmp(g_calls.v[k].lit, id)) where = g_calls.v[k].where;
        const ann_t *a = annotation(id, false);
        n_read += where != NULL;
        n_ann += a != NULL;
        if (!a && !where) {
            fprintf(stderr, "FAIL: %s is read nowhere; wire it or annotate it inert/oos in build-tools/settings_audit.tsv\n", id);
            fails++;
        } else if (a && str_eq(a->status, "inert") && where) {
            fprintf(stderr, "FAIL: %s is annotated inert but read at %s; reclassify it in build-tools/settings_audit.tsv\n", id,
                    where);
            fails++;
        }
    }
    /* keys read through a settings call that the registry does not know */
    for (size_t k = 0; k < g_calls.n; k++) {
        const call_t *c = &g_calls.v[k];
        char *lc = str_lower(c->callee);
        bool settings_call = str_starts(c->callee, "settings_") || strstr(lc, "setting");
        free(lc);
        if (!settings_call || !keylike(c->lit) || hmap_has(&keys, c->lit) || hmap_has(&parents, c->lit)) continue;
        const char *dot = strrchr(c->lit, '.');
        if (dot) {
            char *parent = xstrndup(c->lit, (size_t)(dot - c->lit));
            bool rec = hmap_has(&records, parent);
            free(parent);
            if (rec) continue;
        }
        if (annotation(c->lit, true)) continue;
        fprintf(stderr, "FAIL: %s read at %s is not in omp's settings registry; add it there or mark it omc-only in "
                        "build-tools/settings_audit.tsv\n", c->lit, c->where);
        fails++;
    }
    /* stale annotations */
    for (size_t i = 0; i < g_ann.n; i++) {
        const char *p = g_ann.v[i].pat;
        if (str_eq(g_ann.v[i].status, "omc-only") || str_ends(p, ".*") || hmap_has(&keys, p)) continue;
        fprintf(stderr, "FAIL: settings_audit.tsv names %s, which is not a registry key\n", p);
        fails++;
    }
    printf("settings audit: %zu registry keys, %zu read in src/, %zu annotated, %zu call literals scanned\n", json_len(reg),
           n_read, n_ann, g_calls.n);
    hmap_free(&keys, NULL), hmap_free(&parents, NULL), hmap_free(&records, NULL);
    for (size_t i = 0; i < g_calls.n; i++) free(g_calls.v[i].callee), free(g_calls.v[i].lit), free(g_calls.v[i].where);
    vec_free(&g_calls);
    for (size_t i = 0; i < g_ann.n; i++) free(g_ann.v[i].pat), free(g_ann.v[i].status);
    vec_free(&g_ann);
    json_free(reg);
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
