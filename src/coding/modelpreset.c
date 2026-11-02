/* modelpreset.c - model presets (config/model-presets.ts) and `/modelpreset` (builtin-modes.ts
 * runPresetsCommand). See modelpreset.h. */
#include "coding/modelpreset.h"

#include "config/model_resolver.h"
#include "config/settings.h"

#include <ctype.h>
#include <strings.h>

bool mp_valid_name(const char *n) {
    if (!n || !isalpha((unsigned char)n[0])) return false;
    for (const char *p = n; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return false;
    return true;
}

/* getOwnedModelPreset: the whole entry from the highest layer defining it (project, then global) */
static json_t *owned_preset(const char *name) {
    json_t *p = json_get(json_get(settings_project(), "modelPresets"), name);
    if (p && !json_is_null(p)) return p;
    p = json_get(json_get(settings_global(), "modelPresets"), name);
    if (p && !json_is_null(p)) return p;
    return NULL;
}

static bool project_owns(const char *name) {
    json_t *p = json_get(json_get(settings_project(), "modelPresets"), name);
    return p && !json_is_null(p);
}

/* localeCompare: case-insensitive first, lowercase before uppercase on ties */
static int cmp_locale(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int c = strcasecmp(x, y);
    return c ? c : -strcmp(x, y);
}

strvec_t mp_names(void) {
    strvec_t v = {0};
    json_t *all = settings_get("modelPresets"), *x;
    const char *k;
    json_obj_foreach(all, i, k, x) if (owned_preset(k)) strvec_push(&v, k);
    (void)x;
    qsort(v.v, v.n, sizeof *v.v, cmp_locale);
    return v;
}

/* parseModelPreset: roles (borrowed object) + level (static or NULL); false + *why (owned) when malformed */
static bool parse_preset(json_t *raw, json_t **roles_out, const char **level_out, char **why) {
    *roles_out = NULL;
    *level_out = NULL;
    if (!json_is_obj(raw)) {
        *why = xstrdup("it is not a mapping");
        return false;
    }
    json_t *roles = json_get(raw, "modelRoles");
    if (!json_is_obj(roles)) {
        *why = xstrdup("`modelRoles` is missing or not a mapping");
        return false;
    }
    const char *k;
    json_t *v;
    json_obj_foreach(roles, i, k, v) {
        char *t = json_is_str(v) ? str_trim(json_sv(v)) : NULL;
        bool ok = t && *t;
        free(t);
        if (!ok) {
            *why = xasprintf("role `%s` is not a model selector", k);
            return false;
        }
    }
    json_t *lv = json_get(raw, "defaultThinkingLevel");
    if (lv) {
        const char *l = json_is_str(lv) ? mr_parse_level(json_sv(lv), true) : NULL;
        if (!l || !strcmp(l, "inherit") || !strcmp(l, "off")) {
            *why = xstrdup("`defaultThinkingLevel` is not a thinking level");
            return false;
        }
        *level_out = l;
    }
    *roles_out = roles;
    return true;
}

json_t *mp_effective_roles(void) {
    json_t *out = json_obj();
    json_t *roles = settings_get("modelRoles"), *v;
    const char *k;
    json_obj_foreach(roles, i, k, v) {
        const char *s = mr_role_get(k);
        if (s && *s) json_set_str(out, k, s);
    }
    (void)v;
    return out;
}

char *mp_active(void) {
    strvec_t names = mp_names();
    json_t *roles = mp_effective_roles();
    const char *thinking = settings_str("defaultThinkingLevel", "high");
    char *found = NULL;
    for (size_t i = 0; i < names.n && !found; i++) {
        json_t *pr;
        const char *lv;
        char *why = NULL;
        bool ok = parse_preset(owned_preset(names.v[i]), &pr, &lv, &why);
        free(why);
        if (!ok) continue;
        if (lv && strcmp(lv, thinking)) continue;
        if (json_len(pr) != json_len(roles)) continue;
        bool eq = true;
        const char *k;
        json_t *v;
        json_obj_foreach(pr, j, k, v) {
            const char *e = json_get_str(roles, k);
            if (!e || strcmp(e, json_sv(v))) eq = false;
        }
        if (eq) found = xstrdup(names.v[i]);
    }
    json_free(roles);
    strvec_free(&names);
    return found;
}

/* SOURCE_LABELS over settings provenance */
static const char *source_label(const char *path) {
    const char *p = settings_provenance(path);
    if (str_eq(p, "runtime")) return "command line";
    if (str_eq(p, "overlay")) return "--config file";
    if (str_eq(p, "project")) return "project config";
    if (str_eq(p, "global")) return "global config";
    return "default";
}

char *mp_save(const char *name) {
    json_t *preset = json_obj();
    json_set(preset, "modelRoles", mp_effective_roles());
    json_set_str(preset, "defaultThinkingLevel", settings_str("defaultThinkingLevel", "high"));
    settings_set_global_entry("modelPresets", name, preset);
    if (project_owns(name))
        return xasprintf("Saved model preset \"%s\" to the global config, but the project config still defines a preset of "
                         "the same name, which takes precedence",
                         name);
    return xasprintf("Saved model preset \"%s\"", name);
}

/* deleteModelPreset: 0 deleted, 1 missing, 2 still defined by a project file */
static int mp_delete(const char *name) {
    json_t *g = json_get(settings_global(), "modelPresets");
    if (json_is_obj(g) && json_get(g, name)) {
        settings_set_global_entry("modelPresets", name, NULL);
        json_t *left = json_get(settings_global(), "modelPresets");
        if (json_is_obj(left) && !json_len(left)) settings_unset_global("modelPresets"); /* omp drops the empty map */
        return owned_preset(name) ? 2 : 0;
    }
    return owned_preset(name) ? 2 : 1;
}

static char *layer_role(json_t *layer, const char *role) {
    json_t *v = json_get(json_get(layer, "modelRoles"), role);
    if (json_is_str(v)) return *json_sv(v) ? xstrdup(json_sv(v)) : NULL;
    if (!json_is_arr(v) || !json_len(v)) return NULL;
    sb_t b = SB_INIT;
    json_t *x;
    json_arr_foreach(v, i, x) {
        if (!json_is_str(x)) {
            sb_free(&b);
            return NULL;
        }
        if (i) sb_putc(&b, ',');
        sb_append(&b, json_sv(x));
    }
    return sb_detach(&b);
}

static strvec_t role_union(json_t *a, json_t *b) {
    strvec_t v = {0};
    const char *k;
    json_t *x;
    json_obj_foreach(a, i, k, x) if (!strvec_contains(&v, k)) strvec_push(&v, k);
    json_obj_foreach(b, j, k, x) if (!strvec_contains(&v, k)) strvec_push(&v, k);
    (void)x;
    return v;
}

char *mp_apply(app_t *a, const char *name, bool *clean_out, bool *switched_out, bool *wrote_out) {
    bool clean = false, switched = false, wrote = false;
    json_t *roles;
    const char *plevel;
    char *why = NULL, *msg = NULL;
    json_t *raw = owned_preset(name);
    if (!raw) {
        msg = xasprintf("Preset not found: %s", name);
        goto done;
    }
    if (!parse_preset(raw, &roles, &plevel, &why)) {
        msg = xasprintf("Preset \"%s\" is malformed: %s", name, why);
        free(why);
        goto done;
    }
    roles = json_clone(roles);
    mr_pool_t cands = {0};
    mr_pool_available(&cands, "chat");
    const char *pdef = json_get_str(roles, "default");
    if (pdef) {
        mr_result_t r;
        bool ok = mr_resolve_value(pdef, &cands, &r);
        mr_result_clear(&r);
        if (!ok) msg = xasprintf("Preset \"%s\" not applied: default model `%s` is not available", name, pdef);
    } else if (!mr_pick_default_available(&cands))
        msg = xasprintf("Preset \"%s\" not applied: no model with configured credentials is available", name);
    if (msg) {
        json_free(roles);
        mr_pool_free(&cands);
        goto done;
    }
    /* writePresetRoles */
    json_t *eff = mp_effective_roles();
    strvec_t all = role_union(eff, roles);
    bool proj = str_eq(settings_str("modelRoleStorage", "global"), "project");
    for (size_t i = 0; i < all.n; i++) {
        const char *v = json_get_str(roles, all.v[i]);
        if (proj) {
            char *pv = layer_role(settings_project(), all.v[i]);
            if (v) settings_set_project_entry("modelRoles", all.v[i], json_str(v));
            else if (pv) settings_set_project_entry("modelRoles", all.v[i], json_null());
            free(pv);
        } else {
            char *gv = layer_role(settings_global(), all.v[i]);
            if (v) {
                if (!gv || strcmp(gv, v)) settings_set_global_entry("modelRoles", all.v[i], json_str(v));
            } else if (gv)
                settings_set_global_entry("modelRoles", all.v[i], NULL);
            free(gv);
        }
    }
    if (plevel && !str_eq(json_get_str(settings_global(), "defaultThinkingLevel"), plevel))
        settings_set_global("defaultThinkingLevel", json_str(plevel));
    wrote = true;
    /* shadowedRoles + shadowedThinking */
    json_t *after = mp_effective_roles();
    strvec_t un = role_union(after, roles);
    qsort(un.v, un.n, sizeof *un.v, cmp_locale);
    sb_t shadow = SB_INIT;
    for (size_t i = 0; i < un.n; i++) {
        const char *ex = json_get_str(roles, un.v[i]), *ac = json_get_str(after, un.v[i]);
        if ((!ex && !ac) || (ex && ac && !strcmp(ex, ac))) continue;
        char *path = xasprintf("modelRoles.%s", un.v[i]);
        sb_appendf(&shadow, "%s%s: %s from %s (preset: %s)", shadow.len ? "; " : "", un.v[i], ac ? ac : "(unset)",
                   source_label(path), ex ? ex : "(unset)");
        free(path);
    }
    if (plevel && !str_eq(settings_str("defaultThinkingLevel", "high"), plevel))
        sb_appendf(&shadow, "%sdefaultThinkingLevel: %s from %s (preset: %s)", shadow.len ? "; " : "",
                   settings_str("defaultThinkingLevel", "high"), source_label("defaultThinkingLevel"), plevel);
    /* resolveLiveDefault */
    const char *dv = mr_role_get("default");
    const char *tl = plevel ? plevel : mr_parse_level(settings_str("defaultThinkingLevel", "high"), true);
    const ai_model_t *model = NULL;
    char *fail = NULL;
    if (!dv) {
        model = mr_pick_default_available(&cands);
        if (!model) fail = xstrdup("no model with configured credentials is available");
    } else {
        mr_result_t r;
        if (mr_resolve_value(dv, &cands, &r)) {
            model = r.model;
            if (r.explicit_level) tl = r.level;
        } else
            fail = xasprintf("default model `%s` is not available", dv);
        mr_result_clear(&r);
    }
    if (!fail) {
        char *k = mr_model_key(model), *err = NULL;
        if (app_set_model(a, k, &err) != 0) fail = err;
        else {
            switched = true;
            mr_usage_record(k);
            ai_effort_t e;
            if (tl && strcmp(tl, "inherit")) {
                if (!strcmp(tl, "auto")) app_set_thinking_auto(a);
                else if (effort_parse(tl, &e) == 0) app_set_thinking(a, e);
            }
            char *th = tl ? xasprintf(" \xc2\xb7 %s", tl) : xstrdup("");
            if (shadow.len) msg = xasprintf("Switched to preset \"%s\" (%s%s); still set elsewhere: %s", name, k, th, sb_cstr(&shadow));
            else {
                msg = xasprintf("Switched to preset \"%s\" (%s%s)", name, k, th);
                clean = true;
            }
            free(th);
        }
        free(k);
    }
    if (fail) {
        msg = xasprintf("Preset \"%s\" roles saved, but the model was not switched: %s", name, fail);
        free(fail);
    }
    sb_free(&shadow);
    strvec_free(&un);
    strvec_free(&all);
    json_free(after);
    json_free(eff);
    json_free(roles);
    mr_pool_free(&cands);
done:
    if (clean_out) *clean_out = clean;
    if (switched_out) *switched_out = switched;
    if (wrote_out) *wrote_out = wrote;
    return msg;
}

#define PRESETS_USAGE "Usage: /modelpreset [list | save <name> | switch <name> | delete <name>]"

/* args.trim().split(/\s+/).filter(Boolean) */
static strvec_t str_split_ws(const char *s) {
    strvec_t v = {0};
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        const char *b = s;
        while (*s && !isspace((unsigned char)*s)) s++;
        if (s > b) {
            char *t = xstrndup(b, (size_t)(s - b));
            strvec_push(&v, t);
            free(t);
        }
    }
    return v;
}

void mp_command(app_t *a, const char *args, mp_outcome_t *o) {
    memset(o, 0, sizeof *o);
    strvec_t w = str_split_ws(args ? args : "");
    const char *sub = w.n ? w.v[0] : "list";
    sb_t nb = SB_INIT;
    for (size_t i = 1; i < w.n; i++) sb_appendf(&nb, "%s%s", i > 1 ? " " : "", w.v[i]);
    const char *name = sb_cstr(&nb);
    if (!strcmp(sub, "list")) {
        strvec_t names = mp_names();
        if (names.n) {
            char *j = strvec_join(&names, ", ");
            o->message = xasprintf("Model presets: %s", j);
            free(j);
        } else
            o->message = xstrdup(MP_NO_PRESETS);
        strvec_free(&names);
    } else if (!strcmp(sub, "save")) {
        if (!*name) {
            o->message = xstrdup("Usage: /modelpreset save <name>");
            o->usage = true;
        } else if (!mp_valid_name(name)) {
            o->message = xasprintf("Invalid preset name \"%s\": use a letter, then letters, digits, - or _", name);
            o->usage = true;
        } else {
            o->message = mp_save(name);
            o->changed_config = true;
        }
    } else if (!strcmp(sub, "switch")) {
        if (!*name) {
            o->message = xstrdup("Usage: /modelpreset switch <name>");
            o->usage = true;
        } else {
            bool clean, switched, wrote;
            o->message = mp_apply(a, name, &clean, &switched, &wrote);
            o->failed = !clean;
            o->switched = switched;
            o->changed_config = wrote;
        }
    } else if (!strcmp(sub, "delete")) {
        if (!*name) {
            o->message = xstrdup("Usage: /modelpreset delete <name>");
            o->usage = true;
        } else {
            int r = mp_delete(name);
            if (r == 0) {
                o->message = xasprintf("Deleted model preset \"%s\"", name);
                o->changed_config = true;
            } else if (r == 2) {
                o->message = xasprintf("Preset \"%s\" is defined by a project or --config file; remove it there", name);
                o->failed = true;
            } else {
                o->message = xasprintf("Preset not found: %s", name);
                o->failed = true;
            }
        }
    } else {
        o->message = xstrdup(PRESETS_USAGE);
        o->usage = true;
    }
    sb_free(&nb);
    strvec_free(&w);
}

void mp_outcome_free(mp_outcome_t *o) {
    free(o->message);
    o->message = NULL;
}
