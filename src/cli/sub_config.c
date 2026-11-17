/* sub_config.c - `omc config [list|get|set|reset|path]` (port of omp cli/config-cli.ts) over omp's settings
 * registry (assets/config/settings-registry.json: ids, types, enum values, descriptions, tabs, list order). */
#include "cli/cli_int.h"

#include "config/settings.h"
#include "core/fs.h"
#include "tools/iurl.h"
#include "tui/symbols.h"

#define REDACTED "********"

static char *format_value(const json_t *v) {
    if (!v || json_is_null(v)) return cli_dim("(not set)");
    if (json_is_bool(v)) return v->u.b ? cli_green("true") : cli_red("false");
    if (json_is_num(v)) {
        char *s = json_dump(v, 0), *c = cli_cyan(s);
        free(s);
        return c;
    }
    if (json_is_str(v)) return cli_yellow(json_sv(v));
    char *s = json_dump(v, 0), *c = cli_yellow(s);
    free(s);
    return c;
}

static char *type_display(const json_t *def) {
    const json_t *vals = json_get_arr(def, "values");
    if (json_len(vals)) {
        sb_t b = SB_INIT;
        sb_putc(&b, '(');
        json_t *x;
        json_arr_foreach(vals, i, x) sb_appendf(&b, "%s%s", i ? "|" : "", json_sv(x));
        sb_putc(&b, ')');
        return sb_detach(&b);
    }
    const char *t = json_get_str(def, "type");
    if (str_eq(t, "boolean") || str_eq(t, "number") || str_eq(t, "array") || str_eq(t, "record")) return xasprintf("(%s)", t);
    return xstrdup("(string)");
}

/* chalk.bold.blue */
static char *wrap_blue_bold(const char *s) {
    char *b = cli_blue(s), *r = cli_bold(b);
    free(b);
    return r;
}

static bool is_credential(const json_t *def) { return json_get_bool(def, "credential", false); }

static bool truthy(const json_t *v) {
    if (!v || json_is_null(v)) return false;
    if (json_is_str(v)) return *json_sv(v) != 0;
    if (json_is_bool(v)) return v->u.b;
    return true;
}

static int by_order(const void *x, const void *y) {
    long a = (long)json_d2i(json_get_num(*(const json_t *const *)x, "order", 0)), b = (long)json_d2i(json_get_num(*(const json_t *const *)y, "order", 0));
    return a < b ? -1 : a > b;
}

/* orderedSettings(): the registry in `omp config list` order */
static size_t ordered(const json_t ***out) {
    const json_t *reg = cfg_registry();
    const json_t **v = xcalloc(json_len(reg) + 1, sizeof *v);
    size_t n = 0;
    json_t *e;
    json_arr_foreach(reg, i, e) if (json_get_num(e, "order", -1) >= 0) v[n++] = e;
    xqsort(v, n, sizeof *v, by_order);
    *out = v;
    return n;
}

static void line_entry(const json_t *def, const json_t *value) {
    char *vs = is_credential(def) && truthy(value) ? xstrdup(REDACTED) : format_value(value);
    char *ts = type_display(def), *dt = cli_dim(ts);
    printf("  %s = %s %s\n", json_get_str(def, "id"), vs, dt);
    free(vs);
    free(ts);
    free(dt);
}

static json_t *json_entry(const json_t *def, const json_t *value) {
    json_t *o = json_obj();
    if (is_credential(def) && truthy(value)) json_set_bool(o, "redacted", true);
    else if (value) json_set(o, "value", json_clone(value));
    json_set_str(o, "type", json_get_str(def, "type"));
    json_set_str(o, "description", json_get_str(def, "description") ? json_get_str(def, "description") : "");
    return o;
}

static void print_json(json_t *j) {
    char *s = cli_json_pretty(j);
    printf("%s\n", s);
    free(s);
}

static int tab_cmp(const void *x, const void *y) {
    const char *a = *(const char *const *)x, *b = *(const char *const *)y;
    if (!strcmp(a, "config")) return -1;
    if (!strcmp(b, "config")) return 1;
    return strcmp(a, b);
}

static int handle_list(bool json) {
    const json_t **defs;
    size_t n = ordered(&defs);
    if (json) {
        json_t *out = json_obj();
        for (size_t i = 0; i < n; i++) json_set(out, json_get_str(defs[i], "id"), json_entry(defs[i], cfg_value(defs[i])));
        print_json(out);
        json_free(out);
        free(defs);
        return 0;
    }
    char *h = cli_bold("Settings:\n");
    printf("%s\n", h);
    free(h);
    strvec_t tabs = {0};
    for (size_t i = 0; i < n; i++) {
        const char *t = json_get_str(defs[i], "tab");
        if (!strvec_contains(&tabs, t ? t : "internal")) strvec_push(&tabs, t ? t : "internal");
    }
    xqsort(tabs.v, tabs.n, sizeof *tabs.v, tab_cmp);
    for (size_t k = 0; k < tabs.n; k++) {
        char *g = xasprintf("[%s]", tabs.v[k]);
        char *bl = wrap_blue_bold(g);
        printf("%s\n", bl);
        free(bl);
        free(g);
        for (size_t i = 0; i < n; i++) {
            const char *t = json_get_str(defs[i], "tab");
            if (str_eq(t ? t : "internal", tabs.v[k])) line_entry(defs[i], cfg_value(defs[i]));
        }
        printf("\n");
    }
    strvec_free(&tabs);
    free(defs);
    return 0;
}

static int handle_list_layer(const char *scope, bool json) {
    const json_t *layer = !strcmp(scope, "project") ? settings_project() : settings_global();
    const json_t **defs;
    size_t n = ordered(&defs);
    json_t *out = json ? json_obj() : NULL;
    if (!json) {
        char *t = xasprintf("Settings in the %s layer:\n", scope), *b = cli_bold(t);
        printf("%s\n", b);
        free(b);
        free(t);
    }
    for (size_t i = 0; i < n; i++) {
        const json_t *v = json_path(layer, json_get_str(defs[i], "id"));
        if (!v) continue;
        if (json) json_set(out, json_get_str(defs[i], "id"), json_entry(defs[i], v));
        else line_entry(defs[i], v);
    }
    if (json) {
        print_json(out);
        json_free(out);
    }
    free(defs);
    return 0;
}

static int unknown_setting(const char *key) {
    char *m = xasprintf("Unknown setting: %s", key);
    cli_err_line(m);
    free(m);
    char *d = cli_dim("\nRun 'omc config list' to see available keys");
    fprintf(stderr, "%s\n", d);
    free(d);
    return 1;
}

static int usage(const char *what) {
    char *m = xasprintf("Usage: %s config %s", CLI_BIN, what);
    cli_err_line(m);
    free(m);
    char *d = cli_dim("\nRun 'omc config list' to see available keys");
    fprintf(stderr, "%s\n", d);
    free(d);
    return 1;
}

/* shadowingSource: where the effective value comes from when not the global config (or the default) */
static const char *shadow(const json_t *def, json_t *jo, const char **msgout) {
    const char *prov = settings_provenance(json_get_str(def, "id"));
    const char *env = json_get_str(def, "env");
    if (!strcmp(prov, "env") && env) {
        bool fb = json_get_bool(def, "envFallback", false);
        static char buf[256];
        snprintf(buf, sizeof buf,
                 fb ? "$%s is used as a fallback while the saved value is blank."
                    : "$%s overrides this value; unset it for the saved value to apply.",
                 env);
        if (jo) json_set_str(jo, fb ? "fallbackEnv" : "overriddenBy", env);
        *msgout = buf;
        return prov;
    }
    if (!strcmp(prov, "project")) {
        *msgout = "Project settings override this value here; edit or remove it there for the saved value to apply.";
    } else if (!strcmp(prov, "overlay")) {
        *msgout = "A --config / PI_CONFIG_FILES overlay overrides this value for this process.";
    } else if (!strcmp(prov, "runtime")) {
        *msgout = "A runtime override supplies the effective value for this process.";
    } else return NULL;
    if (jo) json_set_str(jo, "overriddenBy", prov);
    return prov;
}

int cmd_config(cparse_t *p) {
    const char *action = cp_arg(p, "action") ? cp_arg(p, "action") : "list";
    const char *key = cp_arg(p, "key");
    const strvec_t *vals = cp_arg_multi(p, "value");
    char *value = vals ? strvec_join(vals, " ") : NULL;
    bool json = cp_bool(p, "json");
    const char *scope = cp_str(p, "scope");
    int rc = 0;
    if (scope && strcmp(action, "list")) {
        cli_err_line("--scope applies to `config list` only");
        free(value);
        return 1;
    }
    cli_init_runtime(NULL);
    if (!strcmp(action, "list")) rc = scope ? handle_list_layer(scope, json) : handle_list(json);
    else if (!strcmp(action, "path")) printf("%s\n", omc_agent_dir());
    else if (!strcmp(action, "get")) {
        const json_t *def = key ? cfg_setting(key) : NULL;
        if (!key) rc = usage("get <key>");
        else if (!def) rc = unknown_setting(key);
        else {
            const json_t *v = cfg_value(def);
            if (json) {
                json_t *o = json_obj();
                json_set_str(o, "key", key);
                if (v) json_set(o, "value", json_clone(v));
                json_set_str(o, "type", json_get_str(def, "type"));
                json_set_str(o, "description", json_get_str(def, "description") ? json_get_str(def, "description") : "");
                print_json(o);
                json_free(o);
            } else {
                char *s = format_value(v);
                printf("%s\n", s);
                free(s);
            }
        }
    } else if (!strcmp(action, "set")) {
        const json_t *def = key ? cfg_setting(key) : NULL;
        if (!key || !value) rc = usage("set <key> <value>");
        else if (!def) rc = unknown_setting(key);
        else {
            char *err = NULL;
            json_t *v = cfg_parse(def, value, &err);
            if (!v || settings_set_global(key, v)) {
                char *m = xasprintf("Error: %s", err ? err : "could not write config.yml");
                cli_err_line(m);
                free(m);
                rc = 1;
            } else {
                const json_t *saved = json_path(settings_global(), key);
                json_t *jo = json ? json_obj() : NULL;
                if (jo) {
                    json_set_str(jo, "key", key);
                    if (saved) json_set(jo, "value", json_clone(saved));
                }
                const char *msg = NULL;
                const char *sh = shadow(def, jo, &msg);
                if (jo) {
                    char *s = json_dump(jo, 0);
                    printf("%s\n", s);
                    free(s);
                    json_free(jo);
                } else {
                    char *fv = format_value(saved);
                    char *line = xasprintf("%s Set %s = %s", tui_sym("status.success"), key, fv), *g = cli_green(line);
                    printf("%s\n", g);
                    free(g);
                    free(line);
                    free(fv);
                    if (sh) {
                        char *w = xasprintf("%s %s", tui_sym("status.warning"), msg), *y = cli_yellow(w);
                        printf("%s\n", y);
                        free(y);
                        free(w);
                    }
                }
            }
            free(err);
        }
    } else if (!strcmp(action, "reset")) {
        const json_t *def = key ? cfg_setting(key) : NULL;
        if (!key) rc = usage("reset <key>");
        else if (!def) rc = unknown_setting(key);
        else if (settings_unset_global(key)) {
            cli_err_line("Error: could not write config.yml");
            rc = 1;
        } else {
            const json_t *v = cfg_value(def);
            bool red = is_credential(def) && truthy(v);
            if (json) {
                json_t *o = json_obj();
                json_set_str(o, "key", key);
                if (red) json_set_bool(o, "redacted", true);
                else if (v) json_set(o, "value", json_clone(v));
                char *s = json_dump(o, 0);
                printf("%s\n", s);
                free(s);
                json_free(o);
            } else {
                char *fv = red ? xstrdup(REDACTED) : format_value(v);
                char *line = xasprintf("%s Reset %s to %s", tui_sym("status.success"), key, fv), *g = cli_green(line);
                printf("%s\n", g);
                free(g);
                free(line);
                free(fv);
            }
        }
    }
    free(value);
    return rc;
}
