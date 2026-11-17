/* modelhub.c - the Models hub opened by /model and Alt+M (port of omp overlays/model-hub.ts on
 * overlays/hub-frame.ts, with the host callbacks of modes/controllers/selector-controller.ts and
 * config/model-presets.ts): a fullscreen split frame titled "Models" with a sidebar (✦ Roles n/N,
 * ⬢ All models, ● providers with available models and their counts, ○ catalog providers without
 * credentials) and a body that is either the model browser (status row, "Kind:" segment track, search,
 * rows, two detail rows), the Roles view or a provider's "not configured" page.
 *
 * Roles view: every known role (built-ins, then cycleOrder / modelRoles / modelTags roles) with its
 * assignment (configured, or `auto →` the resolver's `pi/<role>` pick), thinking level and ↻ cycle badge,
 * followed by its retry.fallbackChains entries (↳ rows); model-keyed chains (`provider/id`, `provider/…*`)
 * as headed groups; "+ New role…" / "+ New fallback…". Keys: Enter pick, x clear, f fallback, c cycle,
 * [/] reorder, n new role, t thinking, k compaction limit, s save preset, p/P and Ctrl+←/→ presets.
 * Role assignments persist to modelRoles.<role> in config.yml, or to the project config when
 * modelRoleStorage is "project" (a project/global scope strip asks first). F5 re-probes local servers;
 * SGR mouse: sidebar clicks/wheel, row hover, click-to-select / click-again-to-activate, footer chips. */
#include "ui/modelbrowser.h"
#include "ui/overlays.h"
#include "ui/ui.h"

#include "coding/modelpreset.h"
#include "config/auth.h"
#include "config/compaction_point.h"
#include "config/model_resolver.h"
#include "config/settings.h"
#include "tui/fuzzy.h"

#include <ctype.h>
#include <strings.h>

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

/* ---------- roles (coding-agent config/model-roles.ts MODEL_ROLES + getRoleInfo) ---------- */

typedef struct {
    const char *id, *tag, *name;
    tui_tc_t color;
    bool kind_section;
} brole_t;

static const brole_t BUILTIN[] = {
    {"default", "DEFAULT", "Default", TUI_TC_SUCCESS, false},
    {"smol", "SMOL", "Fast", TUI_TC_WARNING, false},
    {"slow", "SLOW", "Thinking", TUI_TC_ACCENT, false},
    {"vision", "VISION", "Vision", TUI_TC_ERROR, false},
    {"plan", "PLAN", "Architect", TUI_TC_MUTED, false},
    {"commit", "COMMIT", "Commit", TUI_TC_DIM, false},
    {"tiny", "TINY", "Tiny", TUI_TC_DIM, false},
    {"memory", "MEMORY", "Memory", TUI_TC_DIM, false},
    {"task", "TASK", "Subtask", TUI_TC_MUTED, false},
    {"advisor", "ADVISOR", "Advisor", TUI_TC_ACCENT, false},
    {"widget", "WIDGET", "Job widgets", TUI_TC_DIM, false},
    {"image", "IMAGE", "Image generation", TUI_TC_ACCENT, true},
    {"web", "WEB", "Web search", TUI_TC_SUCCESS, true},
    {"speech", "SPEECH", "Speech", TUI_TC_WARNING, true},
    {"dictation", "DICTATION", "Dictation", TUI_TC_WARNING, true},
    {"judge", "JUDGE", "Judge", TUI_TC_MUTED, true},
};

typedef struct {
    const char *tag; /* NULL for custom roles */
    char *name;      /* owned */
    tui_tc_t color;
    bool hidden, kind_section;
    const char *label; /* tag ?? name ?? role */
} rinfo_t;

static const brole_t *builtin_role(const char *role) {
    for (size_t i = 0; role && i < ARRAY_LEN(BUILTIN); i++)
        if (!strcmp(BUILTIN[i].id, role)) return &BUILTIN[i];
    return NULL;
}

/* getRoleInfo: modelTags.<role> {name, color, hidden} over the built-in metadata */
static rinfo_t role_info(const char *role) {
    rinfo_t r = {0};
    const brole_t *b = builtin_role(role);
    json_t *tags = settings_get("modelTags");
    json_t *cfg = json_is_obj(tags) ? json_get(tags, role) : NULL;
    r.tag = b ? b->tag : NULL;
    r.color = b ? b->color : TUI_TC_MUTED;
    r.kind_section = b && b->kind_section;
    const char *name = b ? b->name : role;
    if (json_is_obj(cfg)) {
        const char *n = json_get_str(cfg, "name");
        if (n && *n) name = n;
        const char *c = json_get_str(cfg, "color");
        int slot = c ? tui_theme_slot_from_name(c) : -1;
        if (slot >= 0) r.color = (tui_tc_t)slot;
        r.hidden = json_get_bool(cfg, "hidden", false);
    }
    r.name = xstrdup(name);
    r.label = r.tag ? r.tag : r.name;
    return r;
}

static void rinfo_free(rinfo_t *r) { free(r->name); }

static const char *const KINDS[] = {"all", "chat", "tiny", "image", "tts", "stt", "search", "judge", "embedding", "rerank", "video"};

static const char *model_kind(const ai_model_t *m) { return catalog_model_kind(m); }

/* modelRoleValueFromUnknown over one layer (owned or NULL) */
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

static bool project_storage(void) { return str_eq(settings_str("modelRoleStorage", "global"), "project"); }

/* getModelRoleSource */
static const char *role_source(const char *role) {
    char *p = layer_role(settings_project(), role), *g = p ? NULL : layer_role(settings_global(), role);
    const char *r = p ? "project" : g ? "global" : "default";
    free(p);
    free(g);
    return r;
}

/* ---------- state ---------- */

enum { E_ROLES, E_ALL, E_SEP, E_PROVIDER };

typedef struct {
    int kind;
    char *provider;
    char *label;
    char *annotation;
    bool locked;
    bool oauth;
    int catalog_count;
} entry_t;

enum { R_ROLE, R_CHAINKEY, R_FALLBACK, R_SEP, R_NEWROLE, R_NEWFALLBACK };
typedef struct {
    int kind;
    char *role; /* role or chain key */
    int chain_index;
    char *selector;
} rrow_t;

enum { A_NONE, A_ROLE, A_FALLBACK, A_FALLBACKKEY };
enum { SC_NONE, SC_GLOBAL, SC_PROJECT };
enum { CH_ASSIGN, CH_UNASSIGN, CH_FALLBACK, CH_FALLBACK_MODEL, CH_FALLBACK_PROVIDER, CH_SCOPE, CH_THINKING };
enum { S_NONE, S_ROLE, S_SCOPE, S_THINKING, S_NAME };
enum { N_ROLE, N_PRESET, N_COMPACTION };

typedef struct {
    char *label, *styled;
    int action;
    char *role;
    int scope;
    const char *level;
} chip_t;

typedef struct {
    int start, end, index;
} chip_range_t;

typedef struct {
    char *role;
    const ai_model_t *model;
    const char *level;
    bool auto_selected;
} assign_t;

typedef struct {
    tui_component_t base;
    ui_t *u;
    VEC(entry_t) entries;
    size_t active;
    int sidebar_scroll;
    bool follow;
    bool focus_list;
    int sidebar_hover;
    ui_model_browser_t *b;
    VEC(const ai_model_t *) available; /* sorted like sortModelItems */
    strvec_t mru;
    int kind_tab;
    int role_tab; /* 0 all, 1 chat, 2 kinds */
    VEC(rrow_t) rrows;
    int role_index, role_scroll, role_hover, roles_visible, roles_row_start;
    VEC(assign_t) roles; /* every known role with a model */
    strvec_t known;      /* knownRoleIds */
    /* assign mode */
    int assigning;
    char *assign_role;
    int assign_index; /* fallback slot, -1 = append */
    /* strip */
    int strip;
    VEC(chip_t) chips;
    size_t chip_index;
    VEC(chip_range_t) chip_ranges;
    const ai_model_t *strip_model;
    char *strip_role;
    int strip_scope;
    int strip_fallback_index; /* thinking strip on a fallback entry, else -1 */
    bool strip_return;
    const char *strip_initial;
    /* name strip */
    int name_purpose;
    sb_t name_input;
    char *name_error, *name_confirm, *name_preview_value, *name_preview_text;
    /* presets */
    char *preset_cursor;
    int64_t preset_cursor_rev;
    int64_t revision;
    /* layout (mouse hit-testing) */
    int sidebar_w, body_w, content_rows, locked_login_line;
    char *config_error;
} hub_t;

/* ---------- role assignments (model-browser.ts resolveRoleAssignments) ---------- */

static const ai_model_t *assigned(const hub_t *h, const char *role, assign_t **out) {
    for (size_t i = 0; i < h->roles.n; i++)
        if (!strcmp(h->roles.v[i].role, role)) {
            if (out) *out = &h->roles.v[i];
            return h->roles.v[i].model;
        }
    if (out) *out = NULL;
    return NULL;
}

static void roles_free(hub_t *h) {
    for (size_t i = 0; i < h->roles.n; i++) free(h->roles.v[i].role);
    h->roles.n = 0;
}

static const char *default_level_for(const char *role, const mr_result_t *r) {
    if (r->explicit_level && r->level) return r->level;
    if (!strcmp(role, "default")) {
        const char *d = mr_parse_level(settings_str("defaultThinkingLevel", "high"), true);
        return d ? d : "inherit";
    }
    return "inherit";
}

static void reload_roles(hub_t *h) {
    roles_free(h);
    strvec_free(&h->known);
    h->known = mr_known_roles();
    mr_pool_t all = {0};
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (!json_get_bool(m->raw, "gone", false) && !mr_provider_disabled(m->provider)) mr_pool_push(&all, m);
    }
    for (size_t r = 0; r < h->known.n; r++) {
        const char *role = h->known.v[r];
        const char *v = mr_role_get(role);
        mr_pool_t pool = {0};
        if (v) {
            for (size_t i = 0; i < all.n; i++)
                if (mr_role_accepts(role, all.v[i])) mr_pool_push(&pool, all.v[i]);
            mr_result_t res;
            if (mr_resolve_value(v, &pool, &res)) {
                assign_t a = {xstrdup(role), res.model, default_level_for(role, &res), false};
                vec_push(&h->roles, a);
            }
            mr_result_clear(&res);
        } else if (h->available.n) {
            for (size_t i = 0; i < h->available.n; i++)
                if (mr_role_accepts(role, h->available.v[i])) mr_pool_push(&pool, h->available.v[i]);
            char *alias = xasprintf("pi/%s", role);
            mr_result_t res;
            if (mr_resolve_value(alias, &pool, &res)) {
                assign_t a = {xstrdup(role), res.model, default_level_for(role, &res), true};
                vec_push(&h->roles, a);
            }
            mr_result_clear(&res);
            free(alias);
        }
        mr_pool_free(&pool);
    }
    mr_pool_free(&all);
}

static strvec_t visible_roles(const hub_t *h) {
    strvec_t v = {0};
    for (size_t i = 0; i < h->known.n; i++) {
        rinfo_t ri = role_info(h->known.v[i]);
        if (!ri.hidden) strvec_push(&v, h->known.v[i]);
        rinfo_free(&ri);
    }
    return v;
}

static strvec_t cycle_order(void) {
    strvec_t v = {0};
    json_t *c = settings_get("cycleOrder");
    if (json_is_arr(c)) {
        json_t *x;
        json_arr_foreach(c, i, x) if (json_is_str(x)) strvec_push(&v, json_sv(x));
    } else {
        strvec_push(&v, "smol");
        strvec_push(&v, "default");
        strvec_push(&v, "slow");
    }
    return v;
}

static json_t *fallback_chains(void) {
    json_t *c = settings_get("retry.fallbackChains");
    return json_is_obj(c) ? c : NULL;
}

/* one chain with non-string entries dropped */
static strvec_t chain_of(const char *key) {
    strvec_t v = {0};
    json_t *arr = json_get(fallback_chains(), key), *x;
    if (json_is_arr(arr)) json_arr_foreach(arr, i, x) if (json_is_str(x)) strvec_push(&v, json_sv(x));
    return v;
}

/* ---------- presets (config/model-presets.ts) ---------- */

static int cmp_locale(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int c = strcasecmp(x, y);
    return c ? c : -strcmp(x, y);
}

static int cmp_code_unit(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* ---------- sidebar ---------- */

static void entries_free(hub_t *h) {
    for (size_t i = 0; i < h->entries.n; i++) {
        free(h->entries.v[i].provider);
        free(h->entries.v[i].label);
        free(h->entries.v[i].annotation);
    }
    h->entries.n = 0;
}

static void push_entry(hub_t *h, int kind, const char *label, const char *provider, char *ann_owned, bool locked) {
    entry_t e = {kind, provider ? xstrdup(provider) : NULL, xstrdup(label), ann_owned, locked, false, 0};
    vec_push(&h->entries, e);
}

static const char *hub_query(const hub_t *h) { return h->b && h->b->query && *h->b->query ? h->b->query : NULL; }

static int match_count(const hub_t *h, const char *provider) {
    tui_fuzzy_query_t *q = tui_fuzzy_query_new(hub_query(h));
    int n = 0;
    for (size_t i = 0; i < h->available.n; i++) {
        const ai_model_t *m = h->available.v[i];
        if (provider && strcmp(m->provider, provider)) continue;
        bool free_ = ui_model_is_free(m);
        char *t = xasprintf("%s/%s%s", m->provider, m->id, free_ ? " free" : "");
        if (tui_fuzzy_query_match(q, t, NULL)) n++;
        free(t);
    }
    tui_fuzzy_query_free(q);
    return n;
}

static bool hop_skipped(const hub_t *h, const entry_t *e) {
    if (e->kind == E_SEP) return true;
    if (!hub_query(h)) return false;
    if (e->kind == E_ROLES) return true;
    if (e->kind == E_PROVIDER) return e->locked || match_count(h, e->provider) == 0;
    return false;
}

static const char *const LOCAL_PROVIDERS[] = {"ollama", "llama.cpp", "lm-studio"};

static void build_sidebar(hub_t *h) {
    char *keep = h->entries.n ? xstrdup(h->entries.v[h->active].label) : NULL;
    int keep_kind = h->entries.n ? h->entries.v[h->active].kind : E_ALL;
    int keep_offset = (int)h->active - h->sidebar_scroll;
    bool had = h->entries.n > 0;
    entries_free(h);
    strvec_t vis = visible_roles(h);
    int assigned_n = 0;
    for (size_t r = 0; r < vis.n; r++) {
        assign_t *a;
        if (assigned(h, vis.v[r], &a) && !a->auto_selected) assigned_n++;
    }
    push_entry(h, E_ROLES, "Roles", NULL, xasprintf("%d/%zu", assigned_n, vis.n), false);
    strvec_free(&vis);
    push_entry(h, E_ALL, "All models", NULL, xasprintf("%zu", h->available.n), false);
    strvec_t unlocked = {0}, locked = {0};
    for (size_t i = 0; i < h->available.n; i++)
        if (!strvec_contains(&unlocked, h->available.v[i]->provider)) strvec_push(&unlocked, h->available.v[i]->provider);
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (json_get_bool(m->raw, "gone", false) || strvec_contains(&unlocked, m->provider) ||
            strvec_contains(&locked, m->provider) || mr_provider_disabled(m->provider))
            continue;
        strvec_push(&locked, m->provider);
    }
    /* discoverable local endpoints: hidden until discovery reaches a server */
    for (size_t i = 0; i < ARRAY_LEN(LOCAL_PROVIDERS); i++) {
        const char *p = LOCAL_PROVIDERS[i];
        if (strvec_contains(&unlocked, p) || mr_provider_disabled(p)) continue;
        json_t *st = catalog_local_discovery_state(p);
        const char *s = json_get_str(st, "status");
        bool show = st && s && strcmp(s, "unavailable") && !(json_get_bool(st, "optional", true) && !strcmp(s, "idle"));
        json_free(st);
        if (show) strvec_push(&unlocked, p);
    }
    qsort(unlocked.v, unlocked.n, sizeof *unlocked.v, cmp_locale);
    if (hub_query(h)) {
        strvec_t matched = {0}, rest = {0};
        for (size_t i = 0; i < unlocked.n; i++) strvec_push(match_count(h, unlocked.v[i]) > 0 ? &matched : &rest, unlocked.v[i]);
        for (size_t i = 0; i < rest.n; i++) strvec_push(&matched, rest.v[i]);
        strvec_free(&unlocked);
        unlocked = matched;
        strvec_free(&rest);
    }
    qsort(locked.v, locked.n, sizeof *locked.v, cmp_locale);
    json_t *oauth = catalog_auth_providers();
    if (unlocked.n) {
        push_entry(h, E_SEP, "", NULL, NULL, false);
        for (size_t i = 0; i < unlocked.n; i++) {
            int n = 0;
            for (size_t k = 0; k < h->available.n; k++)
                if (!strcmp(h->available.v[k]->provider, unlocked.v[i])) n++;
            push_entry(h, E_PROVIDER, unlocked.v[i], unlocked.v[i], xasprintf("%d", n), false);
        }
    }
    if (locked.n) {
        push_entry(h, E_SEP, "", NULL, NULL, false);
        for (size_t i = 0; i < locked.n; i++) {
            push_entry(h, E_PROVIDER, locked.v[i], locked.v[i], NULL, true);
            entry_t *e = &h->entries.v[h->entries.n - 1];
            json_t *o;
            json_arr_foreach(oauth, k, o) if (str_eq(json_get_str(o, "id"), locked.v[i])) e->oauth = true;
            for (size_t c = 0; c < catalog_count(); c++)
                if (!strcmp(catalog_at(c)->provider, locked.v[i]) && !json_get_bool(catalog_at(c)->raw, "gone", false))
                    e->catalog_count++;
        }
    }
    strvec_free(&unlocked);
    strvec_free(&locked);
    h->active = 1;
    bool found = false;
    for (size_t i = 0; i < h->entries.n; i++)
        if (h->entries.v[i].kind == keep_kind && str_eq(h->entries.v[i].label, keep)) {
            h->active = i;
            found = true;
        }
    if (had && found) h->sidebar_scroll = MAX(0, (int)h->active - keep_offset);
    else h->follow = true;
    free(keep);
}

static int sidebar_width(const hub_t *h) {
    int longest = 0;
    for (size_t i = 0; i < h->entries.n; i++)
        longest = MAX(longest, tui_visible_width(h->entries.v[i].label) +
                                   (h->entries.v[i].annotation ? tui_visible_width(h->entries.v[i].annotation) : 0) + 5);
    return MAX(18, MIN(26, longest));
}

static void render_sidebar(hub_t *h, int width, int rows, strvec_t *out) {
    if (h->follow) {
        int a = (int)h->active;
        if (a < h->sidebar_scroll) h->sidebar_scroll = a;
        else if (a >= h->sidebar_scroll + rows) h->sidebar_scroll = a - rows + 1;
        h->follow = false;
    }
    h->sidebar_scroll = MAX(0, MIN(h->sidebar_scroll, MAX(0, (int)h->entries.n - rows)));
    for (int i = h->sidebar_scroll; i < MIN((int)h->entries.n, h->sidebar_scroll + rows); i++) {
        const entry_t *e = &h->entries.v[i];
        if (e->kind == E_SEP) {
            sb_t r = SB_INIT;
            sb_repeat(&r, "\xe2\x94\x80", width);
            strvec_push_own(out, S(TUI_TC_BORDER, sb_cstr(&r)));
            sb_free(&r);
            continue;
        }
        bool active = (size_t)i == h->active;
        bool searching = hub_query(h) != NULL;
        int mc = -1;
        if (searching) {
            if (e->kind == E_PROVIDER && !e->locked) mc = match_count(h, e->provider);
            else if (e->kind == E_ALL) mc = match_count(h, NULL);
        }
        bool muted = e->locked || mc == 0 || (searching && e->kind == E_ROLES);
        const char *iconsym = e->kind == E_ROLES ? tui_sym("icon.extensionSkill")
                              : e->kind == E_ALL ? tui_sym("icon.model")
                              : muted            ? tui_sym("status.shadowed")
                                                 : tui_sym("status.enabled");
        char *icon = S(muted ? TUI_TC_DIM : e->kind == E_PROVIDER ? TUI_TC_SUCCESS : TUI_TC_ACCENT, iconsym);
        char *cursor = active && !h->focus_list ? S(TUI_TC_ACCENT, tui_sym("nav.cursor")) : xstrdup(" ");
        char *label;
        if (muted) label = S(TUI_TC_DIM, e->label);
        else if (active) {
            char *a = S(TUI_TC_ACCENT, e->label);
            label = tui_bold(a);
            free(a);
        } else label = xstrdup(e->label);
        char *left = xasprintf("%s %s %s", cursor, icon, label);
        char mcs[24];
        snprintf(mcs, sizeof mcs, "%d", mc);
        char *ann = S(TUI_TC_DIM, mc >= 0 ? mcs : e->annotation ? e->annotation : "");
        int lw = tui_visible_width(left), aw = tui_visible_width(ann);
        char *line;
        if (lw + aw + 1 <= width) line = xasprintf("%s%*s%s", left, width - lw - aw, "", ann);
        else {
            char *t = tui_truncate_to_width(left, width, NULL);
            line = tui_pad_to_width(t, width);
            free(t);
        }
        if (i == h->sidebar_hover) {
            char *band = tui_bgs(NULL, TUI_TC_SELECTED_BG, line);
            free(line);
            line = band;
        }
        strvec_push_own(out, line);
        free(icon);
        free(cursor);
        free(label);
        free(left);
        free(ann);
    }
    while ((int)out->n < rows) strvec_push(out, "");
}

/* ---------- body ---------- */

static bool is_browser_view(const hub_t *h) {
    const entry_t *e = &h->entries.v[h->active];
    return h->assigning != A_NONE || e->kind == E_ALL || (e->kind == E_PROVIDER && !e->locked);
}

/* #emptyStateMessage (owned or NULL) */
static char *empty_state(const hub_t *h) {
    if (h->config_error) return xasprintf("  %s", h->config_error);
    const entry_t *e = &h->entries.v[h->active];
    if (e->kind != E_PROVIDER || e->locked || h->assigning != A_NONE) return NULL;
    if (hub_query(h))
        return xasprintf("  No matching models in %s. Switch to All models to search every provider.", e->label);
    json_t *st = catalog_local_discovery_state(e->provider);
    const char *s = json_get_str(st, "status");
    char *r = NULL;
    if (s) {
        int64_t fetched = json_get_int(st, "fetchedAt", 0);
        char *age = NULL;
        if (fetched > 0) {
            int64_t ms = MAX(0, now_ms() - fetched);
            age = ms < 60000 ? xstrdup("less than a minute ago") : xasprintf("%lldm ago", (long long)((ms + 30000) / 60000));
        }
        const char *err = json_get_str(st, "error");
        if (!strcmp(s, "cached"))
            r = age ? xasprintf("  Using cached model list from %s. Live refresh is still pending.", age)
                    : xstrdup("  Using cached model list. Live refresh is still pending.");
        else if (!strcmp(s, "unavailable")) {
            if (err && str_starts(err, "HTTP 404 from "))
                r = xasprintf("  Discovery endpoint %s returned 404. Point baseUrl at the host that serves /models (usually .../v1).",
                              err + 14);
            else if (err) r = xasprintf("  Discovery failed: %s", err);
            else r = age ? xasprintf("  Provider unavailable. Using cached model list from %s.", age) : xstrdup("  Provider unavailable.");
        } else if (!strcmp(s, "unauthenticated"))
            r = xstrdup("  Provider requires authentication before models can be discovered.");
        else if (!strcmp(s, "idle"))
            r = xstrdup("  Provider has not been refreshed yet.");
        else if (!strcmp(s, "empty"))
            r = xstrdup("  Discovery succeeded but returned 0 models. Check that /models returns { data: [{ id }] }.");
        free(age);
    }
    json_free(st);
    return r;
}

static void browser_roles(hub_t *h, ui_model_browser_t *b) {
    ui_role_assign_t *ra = xcalloc(h->roles.n + 1, sizeof *ra);
    for (size_t i = 0; i < h->roles.n; i++) {
        rinfo_t ri = role_info(h->roles.v[i].role);
        ra[i].role = h->roles.v[i].role;
        ra[i].model = h->roles.v[i].model;
        ra[i].level = h->roles.v[i].level;
        ra[i].auto_selected = h->roles.v[i].auto_selected;
        ra[i].label = str_lower(ri.label);
        ra[i].color = (int)ri.color;
        ra[i].hidden = ri.hidden;
        rinfo_free(&ri);
    }
    ui_model_browser_set_roles(b, ra, h->roles.n, &h->mru);
    for (size_t i = 0; i < h->roles.n; i++) free(ra[i].label);
    free(ra);
}

/* #setCandidateItems: assigning a role or a role-keyed fallback re-applies the role's accepts predicate; model-keyed
 * chains (`provider/model-id`, `provider/<any>`) have no role predicate and stay unfiltered */
static bool assign_accepts(const hub_t *h, const ai_model_t *m) {
    bool role = h->assigning == A_ROLE || (h->assigning == A_FALLBACK && h->assign_role && !strchr(h->assign_role, '/'));
    return !role || mr_role_accepts(h->assign_role, m);
}

static void rebuild_browser(hub_t *h) {
    char *query = h->b && h->b->query ? xstrdup(h->b->query) : NULL;
    const ui_model_item_t *cur = h->b ? ui_model_browser_selected(h->b) : NULL;
    char *keep = cur ? xstrdup(cur->selector) : NULL;
    bool focused = h->b ? h->b->focused : false;
    int hovered = h->b ? h->b->hovered : -1;
    ui_model_browser_free(h->b);
    h->b = ui_model_browser_new();
    h->b->focused = focused;
    h->b->hovered = hovered;
    browser_roles(h, h->b);
    const entry_t *e = &h->entries.v[h->active];
    const char *prov = e->kind == E_PROVIDER ? e->provider : NULL;
    bool locked = e->kind == E_PROVIDER && e->locked;
    h->b->show_provider = !prov;
    for (size_t i = 0; i < h->available.n && !locked; i++) {
        const ai_model_t *m = h->available.v[i];
        if (prov && strcmp(m->provider, prov)) continue;
        if (!assign_accepts(h, m)) continue;
        if (h->kind_tab > 0 && strcmp(model_kind(m), KINDS[h->kind_tab])) continue;
        ui_model_browser_add(h->b, m);
    }
    h->b->query = query;
    h->b->empty_text = empty_state(h);
    ui_model_browser_done_adding(h->b);
    if (keep) ui_model_browser_set_current(h->b, keep);
    free(h->b->current); /* the hub marks no current model */
    h->b->current = NULL;
    free(keep);
}

static void rrows_free(hub_t *h) {
    for (size_t i = 0; i < h->rrows.n; i++) {
        free(h->rrows.v[i].role);
        free(h->rrows.v[i].selector);
    }
    h->rrows.n = 0;
}

static void push_rrow(hub_t *h, int kind, const char *role, int idx, const char *sel) {
    rrow_t r = {kind, role ? xstrdup(role) : NULL, idx, sel ? xstrdup(sel) : NULL};
    vec_push(&h->rrows, r);
}

static void append_roles(hub_t *h, const strvec_t *roles) {
    for (size_t i = 0; i < roles->n; i++) {
        push_rrow(h, R_ROLE, roles->v[i], 0, NULL);
        strvec_t ch = chain_of(roles->v[i]);
        for (size_t k = 0; k < ch.n; k++) push_rrow(h, R_FALLBACK, roles->v[i], (int)k, ch.v[k]);
        strvec_free(&ch);
    }
}

/* #buildRolesRows */
static void build_role_rows(hub_t *h) {
    rrows_free(h);
    strvec_t vis = visible_roles(h), chat = {0}, kind = {0};
    for (size_t i = 0; i < vis.n; i++) {
        rinfo_t ri = role_info(vis.v[i]);
        strvec_push(ri.kind_section ? &kind : &chat, vis.v[i]);
        rinfo_free(&ri);
    }
    if (h->role_tab == 0) {
        append_roles(h, &chat);
        if (chat.n && kind.n) push_rrow(h, R_SEP, NULL, 0, NULL);
        append_roles(h, &kind);
    } else append_roles(h, h->role_tab == 1 ? &chat : &kind);
    push_rrow(h, R_NEWROLE, NULL, 0, NULL);
    push_rrow(h, R_SEP, NULL, 0, NULL);
    strvec_t keys = {0};
    json_t *chains = fallback_chains(), *v;
    const char *k;
    json_obj_foreach(chains, i, k, v) if (strchr(k, '/') && json_is_arr(v)) strvec_push(&keys, k);
    /* Object.keys(...).sort(): code-unit order */
    qsort(keys.v, keys.n, sizeof *keys.v, cmp_code_unit);
    for (size_t i = 0; i < keys.n; i++) {
        push_rrow(h, R_CHAINKEY, keys.v[i], 0, NULL);
        strvec_t ch = chain_of(keys.v[i]);
        for (size_t c = 0; c < ch.n; c++) push_rrow(h, R_FALLBACK, keys.v[i], (int)c, ch.v[c]);
        strvec_free(&ch);
    }
    push_rrow(h, R_NEWFALLBACK, NULL, 0, NULL);
    strvec_free(&keys);
    strvec_free(&vis);
    strvec_free(&chat);
    strvec_free(&kind);
    h->role_index = MAX(0, MIN(h->role_index, (int)h->rrows.n - 1));
}

/* chrome/segment-track.ts renderSegmentTrack */
/* resolveSegmentPalette: up to `n` (at most 12) distinct theme colors in candidate order */
size_t ui_segment_palette(size_t n, int *out) {
    static const tui_tc_t CAND[] = {TUI_TC_ACCENT,        TUI_TC_SUCCESS,      TUI_TC_WARNING,       TUI_TC_ERROR,
                                    TUI_TC_MD_CODE,       TUI_TC_MD_LINK,      TUI_TC_SYNTAX_STRING, TUI_TC_SYNTAX_KEYWORD,
                                    TUI_TC_SYNTAX_FUNCTION, TUI_TC_SYNTAX_NUMBER, TUI_TC_SYNTAX_OPERATOR, TUI_TC_SYNTAX_VARIABLE};
    size_t np = 0;
    for (size_t i = 0; i < ARRAY_LEN(CAND) && np < n; i++) {
        bool dup = false;
        for (size_t k = 0; k < np; k++)
            if (!strcmp(tui_fg_seq(NULL, (tui_tc_t)out[k]), tui_fg_seq(NULL, CAND[i]))) dup = true;
        if (!dup) out[np++] = (int)CAND[i];
    }
    return np;
}

static char *segment_track(const char *const *labels, size_t n, int active) {
    int pali[12];
    size_t np = ui_segment_palette(n, pali);
    tui_tc_t pal[12];
    for (size_t i = 0; i < np; i++) pal[i] = (tui_tc_t)pali[i];
    if (!np) pal[np++] = TUI_TC_ACCENT;
    char *thin = S(TUI_TC_STATUS_LINE_SEP, tui_sym("sep.powerlineThin"));
    sb_t t = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            if ((int)i == active || (int)i - 1 == active) sb_append(&t, "  ");
            else sb_appendf(&t, " %s ", thin);
        }
        const char *fg = tui_fg_seq(NULL, pal[i % np]);
        if ((int)i != active) {
            sb_appendf(&t, "%s%s\x1b[39m", fg, labels[i]);
            continue;
        }
        char bg[40];
        snprintf(bg, sizeof bg, "%s", fg);
        char *p = strstr(bg, "\x1b[38;");
        if (p) p[2] = '4';
        int64_t rgb = tui_theme()->rgb[pal[i % np]];
        const char *contrast;
        if (rgb >= 0) {
            double luma = 0.299 * ((rgb >> 16) & 255) + 0.587 * ((rgb >> 8) & 255) + 0.114 * (rgb & 255);
            contrast = luma > 140 ? "\x1b[38;2;0;0;0m" : "\x1b[38;2;255;255;255m";
        } else contrast = tui_fg_seq(NULL, TUI_TC_TEXT);
        sb_appendf(&t, "%s%s%s%s\x1b[1m %s \x1b[22m\x1b[49m%s%s\x1b[39m", fg, tui_sym("sep.powerlineRight"), bg, contrast,
                   labels[i], fg, tui_sym("sep.powerlineLeft"));
    }
    free(thin);
    return sb_detach(&t);
}

static char *hints2(const char *a, const char *b) {
    char *x = ui_key_hint(a), *y = ui_key_hint(b), *r = xasprintf("%s/%s", x, y);
    free(x);
    free(y);
    return r;
}

static void push_trunc(strvec_t *out, char *owned, int width) {
    strvec_push_own(out, tui_truncate_to_width(owned, width, NULL));
    free(owned);
}

static void status_row(hub_t *h, int width, strvec_t *out) {
    const entry_t *e = &h->entries.v[h->active];
    if (h->assigning != A_NONE) {
        char *enter = ui_key_hint("enter"), *cancel = ui_editor_key("tui.select.cancel");
        char *t;
        if (h->assigning == A_FALLBACKKEY) {
            t = xasprintf(" New fallback chain \xe2\x80\x94 %s picks the model it protects, %s cancels", enter, cancel);
        } else {
            rinfo_t ri = role_info(h->assign_role);
            char *lab = tui_bold(ri.label);
            if (h->assigning == A_FALLBACK)
                t = xasprintf(" %s %s \xe2\x80\x94 %s picks the fallback model, %s cancels",
                              h->assign_index < 0 ? "Adding fallback for" : "Replacing fallback of", lab, enter, cancel);
            else
                t = xasprintf(" Assigning %s \xe2\x80\x94 %s assigns, %s cancels", lab, enter, cancel);
            free(lab);
            rinfo_free(&ri);
        }
        push_trunc(out, S(TUI_TC_ACCENT, t), width);
        free(t);
        free(enter);
        free(cancel);
        return;
    }
    char *text;
    if (e->kind == E_ROLES) {
        char *f = ui_key_hint("f");
        text = xasprintf(" Model roles \xe2\x80\x94 %s adds a retry fallback, cleared roles fall back to auto-selection", f);
        free(f);
    } else if (e->kind == E_PROVIDER)
        text = e->locked ? xasprintf(" %s \xc2\xb7 not configured", e->label)
                         : xasprintf(" %s \xc2\xb7 %s models", e->label, e->annotation ? e->annotation : "0");
    else text = xstrdup(" All available models");
    if (h->config_error && e->kind != E_PROVIDER) {
        char *t = xasprintf(" %s", h->config_error);
        push_trunc(out, S(TUI_TC_ERROR, t), width);
        free(t);
    } else push_trunc(out, S(TUI_TC_MUTED, text), width);
    free(text);
}

/* thinkingLevelGlyph + metadata label, plain (owned) */
static char *thinking_text(const char *level, bool *has_glyph, char **glyph_out) {
    const char *glyph = NULL;
    char key[48];
    if (!strcmp(level, "off")) glyph = tui_sym("status.disabled");
    else if (strcmp(level, "inherit")) {
        snprintf(key, sizeof key, "thinking.%s", !strcmp(level, "auto") ? "autoPending" : level);
        glyph = tui_sym(key);
    }
    char *g = glyph ? xstrndup(glyph, strcspn(glyph, " ")) : xstrdup("");
    *has_glyph = *g != 0;
    if (glyph_out) *glyph_out = xstrdup(g);
    char *r = xstrdup(!strcmp(level, "minimal") ? "min" : level);
    free(g);
    return r;
}

static void render_role_tabs(hub_t *h, int width, strvec_t *out) {
    static const char *const TABS[] = {"all", "chat", "kinds"};
    char *track = segment_track(TABS, 3, h->role_tab), *ak = hints2("alt+left", "alt+right");
    char *rl = S(TUI_TC_DIM, "Roles:"), *akd = S(TUI_TC_DIM, ak);
    strvec_t names = mp_names();
    char *preset = xstrdup("");
    if (names.n) {
        char *active = mp_active();
        char *cl = hints2("ctrl+left", "ctrl+right");
        char *keys;
        if (hub_query(h)) keys = xstrdup(cl);
        else {
            char *pp = hints2("p", "shift+p");
            keys = xasprintf("%s \xc2\xb7 %s", cl, pp);
            free(pp);
        }
        char *pl = S(TUI_TC_DIM, "Preset:"), *pv = active ? S(TUI_TC_ACCENT, active) : S(TUI_TC_MUTED, "custom");
        char *kd = S(TUI_TC_DIM, keys);
        free(preset);
        preset = xasprintf("   %s %s  %s", pl, pv, kd);
        free(pl);
        free(pv);
        free(kd);
        free(keys);
        free(cl);
        free(active);
    }
    strvec_free(&names);
    push_trunc(out, xasprintf(" %s %s  %s%s", rl, track, akd, preset), width);
    free(preset);
    free(track);
    free(ak);
    free(rl);
    free(akd);
}

static char *finish_row(char *line, int width, bool hovered) {
    char *t = tui_truncate_to_width(line, width, NULL);
    free(line);
    if (!hovered) return t;
    char *p = tui_pad_to_width(t, width);
    free(t);
    char *b = tui_bgs(NULL, TUI_TC_SELECTED_BG, p);
    free(p);
    return b;
}

static void render_roles(hub_t *h, int width, int rows, strvec_t *out) {
    size_t before = out->n;
    render_role_tabs(h, width, out);
    h->roles_row_start = (int)(out->n - before);
    int tagw = 0;
    for (size_t i = 0; i < h->rrows.n; i++)
        if (h->rrows.v[i].kind == R_ROLE) {
            rinfo_t ri = role_info(h->rrows.v[i].role);
            tagw = MAX(tagw, tui_visible_width(ri.label));
            rinfo_free(&ri);
        }
    strvec_t order = cycle_order();
    int total = (int)h->rrows.n;
    int capacity = MAX(0, rows - 2 - h->roles_row_start);
    bool overflow = total > capacity;
    int vh = overflow ? MAX(0, capacity - 1) : capacity;
    if (vh > 0) {
        if (h->role_index < h->role_scroll) h->role_scroll = h->role_index;
        else if (h->role_index >= h->role_scroll + vh) h->role_scroll = h->role_index - vh + 1;
        h->role_scroll = MAX(0, MIN(h->role_scroll, MAX(0, total - vh)));
    } else h->role_scroll = 0;
    int end = MIN(h->role_scroll + vh, total);
    h->roles_visible = MAX(0, end - h->role_scroll);
    for (int i = h->role_scroll; i < end; i++) {
        const rrow_t *r = &h->rrows.v[i];
        bool sel = i == h->role_index, hov = i == h->role_hover;
        char *cursor = sel && h->focus_list ? S(TUI_TC_ACCENT, tui_sym("nav.cursor")) : xstrdup(" ");
        if (r->kind == R_SEP) {
            sb_t s = SB_INIT;
            sb_repeat(&s, "\xe2\x94\x80", MAX(1, width - 6));
            char *b = S(TUI_TC_BORDER, sb_cstr(&s));
            strvec_push_own(out, xasprintf("   %s", b));
            free(b);
            sb_free(&s);
        } else if (r->kind == R_NEWROLE || r->kind == R_NEWFALLBACK) {
            char *l = S(sel ? TUI_TC_ACCENT : TUI_TC_DIM, r->kind == R_NEWROLE ? "+ New role\xe2\x80\xa6" : "+ New fallback\xe2\x80\xa6");
            strvec_push_own(out, finish_row(xasprintf(" %s %s", cursor, l), width, hov));
            free(l);
        } else if (r->kind == R_CHAINKEY) {
            const char *sl = strrchr(r->role, '/');
            char *head = xstrndup(r->role, (size_t)(sl - r->role) + 1), *hd = S(TUI_TC_DIM, head);
            char *tail = sel ? S(TUI_TC_ACCENT, sl + 1) : xstrdup(sl + 1);
            char *dot = S(TUI_TC_DIM, tui_sym("status.shadowed"));
            strvec_push_own(out, finish_row(xasprintf(" %s %s %s%s", cursor, dot, hd, tail), width, hov));
            free(head);
            free(hd);
            free(tail);
            free(dot);
        } else if (r->kind == R_FALLBACK) {
            char *pad = xasprintf("%*s\xe2\x86\xb3", tagw + 3, ""), *br = S(TUI_TC_DIM, pad);
            char *s = S(sel ? TUI_TC_ACCENT : TUI_TC_MUTED, r->selector);
            strvec_push_own(out, finish_row(xasprintf(" %s %s %s", cursor, br, s), width, hov));
            free(pad);
            free(br);
            free(s);
        } else {
            rinfo_t ri = role_info(r->role);
            assign_t *a;
            assigned(h, r->role, &a);
            char *tag = tui_pad_to_width(ri.label, tagw);
            char *dot, *tags, *value, *level = xstrdup("");
            if (a && !a->auto_selected) {
                dot = S(ri.color, tui_sym("status.enabled"));
                tags = S(ri.color, tag);
                char *pp = xasprintf("%s/", a->model->provider), *pd = S(TUI_TC_DIM, pp);
                char *id = sel ? S(TUI_TC_ACCENT, a->model->id) : xstrdup(a->model->id);
                value = xasprintf("%s%s", pd, id);
                free(pp);
                free(pd);
                free(id);
                if (a->level && strcmp(a->level, "inherit")) {
                    bool hg;
                    char *g;
                    char *lab = thinking_text(a->level, &hg, &g);
                    char *lv = hg ? xasprintf("%s %s", g, lab) : xstrdup(lab);
                    free(level);
                    level = S(TUI_TC_DIM, lv);
                    free(lv);
                    free(lab);
                    free(g);
                }
            } else if (a) {
                dot = S(TUI_TC_DIM, tui_sym("status.shadowed"));
                tags = S(TUI_TC_DIM, tag);
                char *t = xasprintf("auto \xe2\x86\x92 %s/%s", a->model->provider, a->model->id);
                value = S(TUI_TC_DIM, t);
                free(t);
            } else {
                dot = S(TUI_TC_DIM, tui_sym("status.shadowed"));
                tags = S(TUI_TC_DIM, tag);
                value = S(TUI_TC_DIM, "\xe2\x80\x94");
            }
            char *cyc = xstrdup("");
            for (size_t k = 0; k < order.n; k++)
                if (!strcmp(order.v[k], r->role)) {
                    free(cyc);
                    char *c = xasprintf("%s %zu", tui_sym("icon.loop"), k + 1);
                    cyc = S(TUI_TC_ACCENT, c);
                    free(c);
                    break;
                }
            char *l = xasprintf(" %s %s %s  %s", cursor, dot, tags, value);
            char *right = *level && *cyc ? xasprintf("%s  %s", level, cyc) : xasprintf("%s%s", level, cyc);
            int rw = tui_visible_width(right), lw = tui_visible_width(l);
            if (rw > 0 && lw + rw + 2 <= width) {
                char *l2 = xasprintf("%s%*s%s", l, width - lw - rw - 1, "", right);
                free(l);
                l = l2;
            }
            strvec_push_own(out, finish_row(l, width, hov));
            free(right);
            free(cyc);
            free(level);
            free(dot);
            free(tags);
            free(value);
            free(tag);
            rinfo_free(&ri);
        }
        free(cursor);
    }
    if (overflow) {
        int above = h->role_scroll, below = total - end;
        sb_t p = SB_INIT;
        sb_append(&p, "   ");
        if (above > 0) sb_appendf(&p, "\xe2\x86\x91 %d more", above);
        if (below > 0) sb_appendf(&p, "%s\xe2\x86\x93 %d more", above > 0 ? "   " : "", below);
        push_trunc(out, S(TUI_TC_DIM, sb_cstr(&p)), width);
        sb_free(&p);
    }
    while ((int)(out->n - before) < rows - 1) strvec_push(out, "");
    if (rows >= 2) {
        char *ck = ui_editor_key("app.model.cycleForward");
        if (!*ck) {
            free(ck);
            ck = ui_key_hint("ctrl+p");
        }
        char *last;
        if (order.n) {
            int ai = -1;
            const rrow_t *sr = h->role_index < (int)h->rrows.n ? &h->rrows.v[h->role_index] : NULL;
            for (size_t k = 0; sr && (sr->kind == R_ROLE || sr->kind == R_FALLBACK) && k < order.n; k++)
                if (!strcmp(order.v[k], sr->role)) ai = (int)k;
            char *tr = segment_track((const char *const *)order.v, order.n, ai);
            char *c = xasprintf("%s cycle:", ck), *cd = S(TUI_TC_DIM, c);
            last = xasprintf("  %s %s", cd, tr);
            free(c);
            free(cd);
            free(tr);
        } else {
            char *ckk = ui_key_hint("c");
            char *t = xasprintf("  %s cycle is empty \xe2\x80\x94 press %s on a role to add it", ck, ckk);
            last = S(TUI_TC_DIM, t);
            free(t);
            free(ckk);
        }
        push_trunc(out, last, width);
        free(ck);
    }
    strvec_free(&order);
}

static void render_locked(hub_t *h, const entry_t *e, int width, int rows, strvec_t *out) {
    size_t before = out->n;
    h->locked_login_line = -1;
    strvec_push(out, "");
    char *t = xasprintf("  %s has no credentials configured", e->label);
    push_trunc(out, S(TUI_TC_WARNING, t), width);
    free(t);
    strvec_push(out, "");
    json_t *info = catalog_provider_info(e->provider);
    json_t *envs = json_get_arr(info, "envVars");
    if (envs && json_len(envs)) {
        sb_t b = SB_INIT;
        json_t *x;
        json_arr_foreach(envs, i, x) sb_appendf(&b, "%s%s", i ? " or " : "", json_sv(x) ? json_sv(x) : "");
        char *m = xasprintf("  Set %s in your environment, or add a key in config.", sb_cstr(&b));
        push_trunc(out, S(TUI_TC_MUTED, m), width);
        free(m);
        sb_free(&b);
    } else {
        push_trunc(out, S(TUI_TC_MUTED, xstrdup("  Add an API key for this provider in config.")), width);
    }
    if (e->oauth) {
        h->locked_login_line = (int)(out->n - before) + 1;
        char *en = ui_key_hint("enter"), *l = xasprintf("  %s Log in with OAuth (%s)", tui_sym("nav.cursor"), en);
        push_trunc(out, S(TUI_TC_ACCENT, l), width);
        free(en);
        free(l);
    }
    strvec_push(out, "");
    if (e->catalog_count > 0) {
        char *c = xasprintf("  %d models in catalog:", e->catalog_count);
        push_trunc(out, S(TUI_TC_DIM, c), width);
        free(c);
        for (size_t i = 0; i < catalog_count() && (int)(out->n - before) < rows; i++) {
            const ai_model_t *m = catalog_at(i);
            if (strcmp(m->provider, e->provider) || json_get_bool(m->raw, "gone", false)) continue;
            char *l = xasprintf("    %s", m->id);
            push_trunc(out, S(TUI_TC_DIM, l), width);
            free(l);
        }
    }
    while ((int)(out->n - before) < rows) strvec_push(out, "");
    while ((int)(out->n - before) > rows) free(out->v[--out->n]);
}

static void render_body(hub_t *h, int width, int rows, strvec_t *out) {
    size_t before = out->n;
    status_row(h, width, out);
    const entry_t *e = &h->entries.v[h->active];
    if (e->kind == E_ROLES && h->assigning == A_NONE) {
        render_roles(h, width, rows - 1, out);
    } else if (e->kind == E_PROVIDER && e->locked && h->assigning == A_NONE) {
        render_locked(h, e, width, rows - 1, out);
    } else {
        char *track = segment_track(KINDS, ARRAY_LEN(KINDS), h->kind_tab), *ak = hints2("alt+left", "alt+right");
        char *kl = S(TUI_TC_DIM, "Kind:"), *akd = S(TUI_TC_DIM, ak);
        push_trunc(out, xasprintf(" %s %s  %s", kl, track, akd), width);
        free(track);
        free(ak);
        free(kl);
        free(akd);
        ui_model_browser_set_max_visible(h->b, rows - 2 - 5);
        h->b->focused = h->focus_list;
        ui_model_browser_render(h->b, width, out);
    }
    while ((int)(out->n - before) < rows) strvec_push(out, "");
    while ((int)(out->n - before) > rows) free(out->v[--out->n]);
}

/* ---------- row helpers ---------- */

static const rrow_t *cur_row(const hub_t *h) {
    return h->role_index >= 0 && h->role_index < (int)h->rrows.n ? &h->rrows.v[h->role_index] : NULL;
}

/* #parseFallbackEntry + #resolveFallbackEntry: the model a ↳ row names (wildcards, unknown ids: NULL) */
static const ai_model_t *fallback_entry_model(const char *raw, const char **level, char **upstream) {
    if (level) *level = NULL;
    if (upstream) *upstream = NULL;
    if (!raw || str_ends(raw, "/*")) return NULL;
    mr_selector_t s;
    const ai_model_t *m = NULL;
    if (mr_parse_selector(raw, &s)) {
        m = catalog_find(s.provider, s.id);
        for (size_t i = 0; i < catalog_count() && !m; i++)
            if (!strcasecmp(catalog_at(i)->provider, s.provider) && !strcasecmp(catalog_at(i)->id, s.id)) m = catalog_at(i);
        if (m && level) *level = s.level;
        mr_selector_free(&s);
    }
    if (!m) { /* `@upstream` routing kept verbatim */
        const char *at = strrchr(raw, '@');
        if (at && at > raw) {
            const char *colon = strchr(at, ':');
            char *base = colon ? xasprintf("%.*s%s", (int)(at - raw), raw, colon) : xstrndup(raw, (size_t)(at - raw));
            if (mr_parse_selector(base, &s)) {
                m = catalog_find(s.provider, s.id);
                if (m) {
                    if (level) *level = s.level;
                    if (upstream) *upstream = colon ? xstrndup(at + 1, (size_t)(colon - at - 1)) : xstrdup(at + 1);
                }
                mr_selector_free(&s);
            }
            free(base);
        }
    }
    return m;
}

/* #roleRowModel */
static const ai_model_t *role_row_model(const hub_t *h) {
    const rrow_t *r = cur_row(h);
    if (r && r->kind == R_ROLE) return assigned(h, r->role, NULL);
    if (r && r->kind == R_FALLBACK) return fallback_entry_model(r->selector, NULL, NULL);
    return NULL;
}

/* #thinkingOptionsFor */
static void thinking_options(const ai_model_t *m, strvec_t *out, bool with_auto) {
    if (!m->reasoning) return;
    strvec_push(out, "inherit");
    strvec_push(out, "off");
    if (with_auto && !json_get_bool(m->thinking, "unverified", false)) strvec_push(out, "auto");
    for (int i = EFFORT_MINIMAL; i <= EFFORT_MAX; i++)
        if (model_supports_effort(m, (ai_effort_t)i)) strvec_push(out, effort_name((ai_effort_t)i));
}

static const char *role_scope_name(int sc) { return sc == SC_PROJECT ? "project" : "global"; }

/* #roleForScope: the role's value in one persisted layer, resolved over the catalog */
static const ai_model_t *role_for_scope(const char *role, int scope, const char **level, bool *explicit_level) {
    char *v = layer_role(scope == SC_PROJECT ? settings_project() : settings_global(), role);
    if (!v && scope == SC_PROJECT) v = NULL;
    if (level) *level = NULL;
    if (explicit_level) *explicit_level = false;
    if (!v) return NULL;
    mr_pool_t all = {0};
    mr_pool_all(&all, "all");
    mr_result_t r;
    const ai_model_t *m = mr_resolve_value(v, &all, &r) ? r.model : NULL;
    if (m && level) *level = r.level;
    if (m && explicit_level) *explicit_level = r.explicit_level;
    mr_result_clear(&r);
    mr_pool_free(&all);
    free(v);
    return m;
}

/* #roleThinkingTarget */
static const ai_model_t *role_thinking_target(const hub_t *h, const char *role, int *scope) {
    *scope = SC_NONE;
    assign_t *a;
    if (!assigned(h, role, &a)) return NULL;
    const char *src = project_storage() ? role_source(role) : "default";
    const ai_model_t *m = a->model;
    if (!strcmp(src, "project") || !strcmp(src, "global")) {
        *scope = !strcmp(src, "project") ? SC_PROJECT : SC_GLOBAL;
        m = role_for_scope(role, *scope, NULL, NULL);
    }
    if (!m) return NULL;
    strvec_t opts = {0};
    thinking_options(m, &opts, true);
    bool ok = opts.n > 0;
    strvec_free(&opts);
    return ok ? m : NULL;
}

/* ---------- footer ---------- */

static char *footer_hint(hub_t *h) {
    char *enter = ui_key_hint("enter"), *cancel = ui_editor_key("tui.select.cancel"), *ud = hints2("up", "down");
    char *left = ui_key_hint("left"), *lr = hints2("left", "right"), *er = hints2("enter", "right");
    char *alr = hints2("alt+left", "alt+right");
    char *r;
    const entry_t *e = &h->entries.v[h->active];
    if (h->strip == S_NAME) {
        if (h->name_purpose == N_COMPACTION)
            r = xasprintf("%s %s compaction point \xc2\xb7 %s cancel", enter,
                          h->name_confirm && !strcmp(h->name_confirm, sb_cstr(&h->name_input)) ? "accept" : "set", cancel);
        else if (h->name_purpose == N_PRESET) r = xasprintf("%s save preset \xc2\xb7 %s cancel", enter, cancel);
        else r = xasprintf("%s create + pick model \xc2\xb7 %s cancel", enter, cancel);
    } else if (h->strip == S_ROLE) r = xasprintf("%s choose \xc2\xb7 %s assign/clear \xc2\xb7 %s cancel", lr, enter, cancel);
    else if (h->strip == S_SCOPE) r = xasprintf("%s save scope \xc2\xb7 %s choose \xc2\xb7 %s cancel", lr, enter, cancel);
    else if (h->strip == S_THINKING) r = xasprintf("%s thinking level \xc2\xb7 %s apply \xc2\xb7 %s keep", lr, enter, cancel);
    else if (h->assigning != A_NONE) {
        if (!h->focus_list)
            r = xasprintf("%s models \xc2\xb7 %s providers \xc2\xb7 type to search \xc2\xb7 %s kind \xc2\xb7 %s cancel", er, ud, alr, cancel);
        else {
            const char *verb = h->assigning == A_FALLBACK      ? "pick fallback"
                               : h->assigning == A_FALLBACKKEY ? "pick the protected model"
                                                               : "assign";
            r = xasprintf("%s %s \xc2\xb7 %s models \xc2\xb7 %s providers \xc2\xb7 type to search \xc2\xb7 %s kind \xc2\xb7 %s cancel", enter, verb,
                          ud, left, alr, cancel);
        }
    } else if (e->kind == E_ROLES) {
        strvec_t presets = mp_names();
        if (!h->focus_list) {
            char *cl = hints2("ctrl+left", "ctrl+right");
            char *pr = presets.n ? xasprintf(" \xc2\xb7 %s preset", cl) : xstrdup("");
            r = xasprintf("%s providers \xc2\xb7 %s roles \xc2\xb7 %s tabs%s \xc2\xb7 %s close", ud, er, alr, pr, cancel);
            free(pr);
            free(cl);
        } else if (hub_query(h)) {
            r = xasprintf("%s rows \xc2\xb7 %s pick \xc2\xb7 type to search \xc2\xb7 %s providers \xc2\xb7 %s close", ud, enter, left, cancel);
        } else {
            char *f = ui_key_hint("f"), *x = ui_key_hint("x"), *k = ui_key_hint("k"), *c = ui_key_hint("c"), *n = ui_key_hint("n"),
                 *s = ui_key_hint("s"), *t = ui_key_hint("t"), *ef = hints2("enter", "f"), *pp = hints2("p", "shift+p");
            const rrow_t *row = cur_row(h);
            char *compaction = role_row_model(h) ? xasprintf(" \xc2\xb7 %s compaction limit", k) : xstrdup("");
            if (row && row->kind == R_FALLBACK) {
                bool editable = fallback_entry_model(row->selector, NULL, NULL) != NULL;
                char *th = editable ? xasprintf(" \xc2\xb7 %s thinking", t) : xstrdup("");
                r = xasprintf("%s rows \xc2\xb7 %s replace \xc2\xb7 %s add another \xc2\xb7 %s remove%s%s \xc2\xb7 [/] reorder \xc2\xb7 %s providers", ud, enter, f,
                              x, th, compaction, left);
                free(th);
            } else if (row && row->kind == R_CHAINKEY) {
                r = xasprintf("%s rows \xc2\xb7 %s add fallback \xc2\xb7 %s clear chain \xc2\xb7 %s providers", ud, ef, x, left);
            } else if (row && row->kind == R_NEWFALLBACK) {
                r = xasprintf("%s rows \xc2\xb7 %s new model/provider fallback chain \xc2\xb7 %s providers", ud, enter, left);
            } else {
                int sc;
                bool editable = row && row->kind == R_ROLE && role_thinking_target(h, row->role, &sc);
                char *th = editable ? xasprintf(" \xc2\xb7 %s thinking", t) : xstrdup("");
                char *sp = presets.n ? xasprintf(" \xc2\xb7 %s preset", pp) : xstrdup("");
                r = xasprintf("%s rows \xc2\xb7 %s pick \xc2\xb7 %s fallback \xc2\xb7 %s clear%s%s \xc2\xb7 %s cycle \xc2\xb7 [/] reorder \xc2\xb7 %s new \xc2\xb7 %s save preset%s",
                              ud, enter, f, x, th, compaction, c, n, s, sp);
                free(th);
                free(sp);
            }
            free(compaction);
            free(f);
            free(x);
            free(k);
            free(c);
            free(n);
            free(s);
            free(t);
            free(ef);
            free(pp);
        }
        strvec_free(&presets);
    } else if (e->kind == E_PROVIDER && e->locked) {
        r = e->oauth ? xasprintf("%s log in \xc2\xb7 %s providers \xc2\xb7 %s close", enter, ud, cancel) : xasprintf("%s providers \xc2\xb7 %s close", ud, cancel);
    } else {
        char *f5 = ui_key_hint("f5");
        char *refresh = e->kind == E_PROVIDER ? xasprintf(" \xc2\xb7 %s refresh", f5) : xstrdup("");
        free(f5);
        if (!h->focus_list)
            r = xasprintf("%s models \xc2\xb7 %s providers \xc2\xb7 type to search \xc2\xb7 %s kind%s \xc2\xb7 %s close", er, ud, alr, refresh, cancel);
        else
            r = xasprintf("%s assign roles \xc2\xb7 %s models \xc2\xb7 %s providers \xc2\xb7 type to search \xc2\xb7 %s kind%s \xc2\xb7 %s close", enter, ud, left,
                          alr, refresh, cancel);
        free(refresh);
    }
    free(enter);
    free(cancel);
    free(ud);
    free(left);
    free(lr);
    free(er);
    free(alr);
    return r;
}

/* Input.render: "> " + text with an inverse cursor cell, padded to width */
static char *render_input(const char *text, int width) {
    int avail = MAX(1, width - 2);
    int tw = tui_visible_width(text);
    char *vis = tw + 1 > avail ? tui_slice_cols(text, tw + 1 - avail, avail - 1) : xstrdup(text);
    int vw = tui_visible_width(vis) + 1;
    char *r = xasprintf("> %s\x1b[7m \x1b[27m%*s", vis, MAX(0, avail - vw), "");
    free(vis);
    return r;
}

static const char *const COMPACTION_INPUT_HINT = "400k base \xc2\xb7 f400k fixed \xc2\xb7 80% \xc2\xb7 empty resets";

static char *render_name_strip(hub_t *h, int width) {
    char *label_text = h->name_purpose == N_COMPACTION ? xasprintf("%s limit:", h->strip_model->id)
                       : h->name_purpose == N_PRESET   ? xstrdup("Preset name:")
                                                       : xstrdup("New role name:");
    char *label = S(TUI_TC_ACCENT, label_text);
    int maxw = h->name_purpose == N_COMPACTION ? 10 : 32;
    int iw = MAX(8, MIN(maxw, width - tui_visible_width(label_text) - 24));
    char *input = render_input(sb_cstr(&h->name_input), iw);
    char *hint;
    if (h->name_purpose == N_COMPACTION) {
        const char *value = sb_cstr(&h->name_input);
        if (h->name_error) hint = S(TUI_TC_ERROR, h->name_error);
        else if (h->name_confirm && !strcmp(h->name_confirm, value)) hint = S(TUI_TC_WARNING, h->name_confirm);
        else {
            char *t = str_trim(value);
            hint = NULL;
            if (*t) {
                if (!h->name_preview_value || strcmp(h->name_preview_value, value)) {
                    free(h->name_preview_value);
                    free(h->name_preview_text);
                    h->name_preview_value = xstrdup(value);
                    h->name_preview_text = cp_preview(h->strip_model, value);
                }
                if (h->name_preview_text) hint = S(TUI_TC_DIM, h->name_preview_text);
            }
            free(t);
            if (!hint) hint = S(TUI_TC_DIM, COMPACTION_INPUT_HINT);
        }
    } else hint = S(TUI_TC_DIM, "(letters, digits, - and _)");
    char *line = xasprintf("%s %s %s", label, input, hint);
    char *r = tui_truncate_to_width(line, width, NULL);
    free(line);
    free(label_text);
    free(label);
    free(input);
    free(hint);
    return r;
}

static char *render_strip(hub_t *h, int width) {
    h->chip_ranges.n = 0;
    if (h->strip == S_NAME) return render_name_strip(h, width);
    char *prefix;
    if (h->strip == S_ROLE) {
        char *a = S(TUI_TC_ACCENT, h->strip_model->id), *d = S(TUI_TC_DIM, " \xe2\x86\x92");
        prefix = xasprintf("%s%s ", a, d);
        free(a);
        free(d);
    } else {
        rinfo_t ri = role_info(h->strip_role ? h->strip_role : "");
        char *low = str_lower(ri.label);
        char *a = S(ri.color, low), *t = xasprintf(" \xc2\xb7 %s \xe2\x86\x92", h->strip_model->id), *d = S(TUI_TC_DIM, t);
        prefix = xasprintf("%s%s ", a, d);
        free(low);
        free(a);
        free(t);
        free(d);
        rinfo_free(&ri);
    }
    int avail = MAX(1, width - tui_visible_width(prefix));
    size_t n = h->chips.n;
    int *cw = xcalloc(n + 1, sizeof *cw);
    for (size_t i = 0; i < n; i++) {
        char *b = xasprintf(" %s ", h->chips.v[i].styled);
        cw[i] = tui_visible_width(b) + (i == h->chip_index ? 2 : 0) + 1;
        free(b);
    }
#define START_FOR(target, out)                                                                                         \
    do {                                                                                                               \
        size_t s_ = 0;                                                                                                 \
        while (s_ < (target)) {                                                                                        \
            int sum_ = s_ > 0 ? 2 : 0;                                                                                 \
            for (size_t i_ = s_; i_ <= (target); i_++) sum_ += cw[i_];                                                 \
            if (sum_ <= avail) break;                                                                                  \
            s_++;                                                                                                      \
        }                                                                                                              \
        (out) = s_;                                                                                                    \
    } while (0)
    size_t start;
    START_FOR(MIN(h->chip_index + 1, n ? n - 1 : 0), start);
    if (start > h->chip_index) START_FOR(h->chip_index, start);
#undef START_FOR
    free(cw);
    sb_t line = SB_INIT;
    sb_append(&line, prefix);
    int col = 2 + tui_visible_width(prefix);
    if (start > 0) {
        char *d = S(TUI_TC_DIM, "\xe2\x80\xa6 ");
        sb_append(&line, d);
        free(d);
        col += 2;
    }
    for (size_t i = start; i < n; i++) {
        char *body = xasprintf(" %s ", h->chips.v[i].styled);
        int w = tui_visible_width(body) + (i == h->chip_index ? 2 : 0);
        chip_range_t cr = {col, col + w, (int)i};
        vec_push(&h->chip_ranges, cr);
        if (i == h->chip_index) {
            char *lb = S(TUI_TC_ACCENT, "["), *rb = S(TUI_TC_ACCENT, "]");
            char *in = xasprintf("%s%s%s", lb, body, rb), *bg = tui_bgs(NULL, TUI_TC_SELECTED_BG, in);
            sb_append(&line, bg);
            free(lb);
            free(rb);
            free(in);
            free(bg);
        } else sb_append(&line, body);
        sb_putc(&line, ' ');
        col += w + 1;
        free(body);
    }
    char *r = tui_truncate_to_width(sb_cstr(&line), width, NULL);
    sb_free(&line);
    free(prefix);
    return r;
}

/* ---------- frame (hub-frame.ts) ---------- */

static void paint(sb_t *b, const char *s) { tui_fg(b, NULL, TUI_TC_BORDER, s); }

static void hub_render(tui_component_t *c, int width, strvec_t *out) {
    hub_t *h = (hub_t *)c;
    int height = MAX(16, tui_term(h->u->tui)->rows > 0 ? tui_term(h->u->tui)->rows : 40);
    int content = MAX(10, height - 4);
    int L = sidebar_width(h);
    int R = width - 7 - L;
    if (R < 28) {
        L = MAX(18, width - 7 - 28);
        R = width - 7 - L;
    }
    if (R < 0) R = 0;
    h->sidebar_w = L;
    h->body_w = R;
    h->content_rows = content;
    const char *H = tui_sym("boxRound.horizontal");
    int dcol = L + 3, left_len = MAX(0, dcol - 1), right_len = MAX(0, width - 2 - dcol);
    sb_t t = SB_INIT, r = SB_INIT;
    char *shown = tui_truncate_to_width(" Models ", MAX(0, left_len - 1), NULL);
    int fill = MAX(0, left_len - 1 - tui_visible_width(shown));
    sb_appendf(&r, "%s%s", tui_sym("boxRound.topLeft"), H);
    paint(&t, sb_cstr(&r));
    sb_clear(&r);
    char *acc = S(TUI_TC_ACCENT, shown), *bold = tui_bold(acc);
    sb_append(&t, bold);
    free(acc);
    free(bold);
    free(shown);
    sb_repeat(&r, H, fill);
    sb_append(&r, tui_sym("boxSharp.teeDown"));
    sb_repeat(&r, H, right_len);
    sb_append(&r, tui_sym("boxRound.topRight"));
    paint(&t, sb_cstr(&r));
    strvec_push_own(out, sb_detach(&t));
    sb_free(&r);
    strvec_t side = {0}, body = {0};
    render_sidebar(h, L, content, &side);
    render_body(h, R, content, &body);
    char *bar = S(TUI_TC_BORDER, tui_sym("boxRound.vertical"));
    for (int i = 0; i < content; i++) {
        char *sl = ui_fit_line(i < (int)side.n ? side.v[i] : "", L);
        char *bl = ui_fit_line(i < (int)body.n ? body.v[i] : "", R);
        strvec_push_own(out, xasprintf("%s %s %s %s %s", bar, sl, bar, bl, bar));
        free(sl);
        free(bl);
    }
    strvec_free(&side);
    strvec_free(&body);
    sb_t d = SB_INIT, dd = SB_INIT;
    sb_append(&d, tui_sym("boxSharp.teeRight"));
    sb_repeat(&d, H, left_len);
    sb_append(&d, tui_sym("boxSharp.teeUp"));
    sb_repeat(&d, H, right_len);
    sb_append(&d, tui_sym("boxSharp.teeLeft"));
    paint(&dd, sb_cstr(&d));
    strvec_push_own(out, sb_detach(&dd));
    sb_free(&d);
    char *foot;
    if (h->strip) foot = render_strip(h, width - 4);
    else {
        h->chip_ranges.n = 0;
        char *f = footer_hint(h), *fd = S(TUI_TC_DIM, f);
        foot = tui_truncate_to_width(fd, width - 4, NULL);
        free(f);
        free(fd);
    }
    strvec_push_own(out, ui_box_row(foot, width));
    free(foot);
    strvec_push_own(out, ui_box_bottom(width));
    free(bar);
}

/* ---------- mutations (selector-controller.ts host callbacks) ---------- */

static const char *const MOUSE_ON = "\x1b[?1000h\x1b[?1003h\x1b[?1006h";
static const char *const MOUSE_OFF = "\x1b[?1006l\x1b[?1003l\x1b[?1000l";

static void close_hub(hub_t *h) {
    ui_t *u = h->u;
    tui_overlay_hide(u->tui, &h->base);
    if (!tui_term(u->tui)->mouse_capture) tui_term_puts(tui_term(u->tui), MOUSE_OFF); /* tui.mouse keeps it */
    tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    tui_component_destroy(&h->base);
}

static void chips_free(hub_t *h) {
    for (size_t i = 0; i < h->chips.n; i++) {
        free(h->chips.v[i].label);
        free(h->chips.v[i].styled);
        free(h->chips.v[i].role);
    }
    h->chips.n = 0;
}

static void name_reset(hub_t *h) {
    sb_clear(&h->name_input);
    free(h->name_error);
    free(h->name_confirm);
    free(h->name_preview_value);
    free(h->name_preview_text);
    h->name_error = h->name_confirm = h->name_preview_value = h->name_preview_text = NULL;
}

static void select_roles_entry(hub_t *h) {
    for (size_t i = 0; i < h->entries.n; i++)
        if (h->entries.v[i].kind == E_ROLES) h->active = i;
    h->follow = true;
}

static void close_strip(hub_t *h) {
    bool back = (h->strip == S_SCOPE || h->strip == S_THINKING) && h->strip_return;
    h->strip = S_NONE;
    chips_free(h);
    free(h->strip_role);
    h->strip_role = NULL;
    h->strip_fallback_index = -1;
    h->chip_ranges.n = 0;
    name_reset(h);
    if (back) {
        select_roles_entry(h);
        h->focus_list = true;
    }
}

static void push_chip(hub_t *h, const char *label, char *styled, int action, const char *role, int scope, const char *level) {
    chip_t c = {xstrdup(label), styled, action, role ? xstrdup(role) : NULL, scope, level};
    vec_push(&h->chips, c);
}

static void load_available(hub_t *h) {
    h->available.n = 0;
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (catalog_model_available(m) && !mr_provider_disabled(m->provider)) vec_push(&h->available, m);
    }
}

/* #syncFromRegistryState */
static void sync_state(hub_t *h) {
    h->revision++;
    load_available(h);
    strvec_free(&h->mru);
    h->mru = mr_usage_order();
    reload_roles(h);
    ui_role_assign_t *ra = xcalloc(h->roles.n + 1, sizeof *ra);
    for (size_t i = 0; i < h->roles.n; i++) {
        ra[i].role = h->roles.v[i].role;
        ra[i].model = h->roles.v[i].model;
    }
    ui_model_sort_items(h->available.v, h->available.n, ra, h->roles.n, &h->mru);
    free(ra);
    build_role_rows(h);
    build_sidebar(h);
    rebuild_browser(h);
}

static void refresh_after_mutation(hub_t *h) { sync_state(h); }

static char *role_label_of(const char *role) {
    rinfo_t ri = role_info(role);
    char *r = xstrdup(ri.label);
    rinfo_free(&ri);
    return r;
}

/* onAssign: persist modelRoles.<role> ("provider/id[:level]") in the storage scope; default also
 * switches the session model */
static void assign_role(hub_t *h, const ai_model_t *m, const char *role, const char *level, int scope) {
    ui_t *u = h->u;
    char *sel = mr_model_key(m);
    bool proj_storage = project_storage();
    int target = proj_storage ? (scope == SC_NONE ? SC_PROJECT : scope) : SC_GLOBAL;
    const char *scope_label = !proj_storage ? "" : target == SC_PROJECT ? "Project " : "Global ";
    bool is_default = !strcmp(role, "default");
    bool concrete = level && strcmp(level, "inherit") && strcmp(level, "auto");
    char *val = (level && strcmp(level, "inherit") && !(is_default && !strcmp(level, "auto"))) ? xasprintf("%s:%s", sel, level)
                                                                                                : xstrdup(sel);
    if (target == SC_PROJECT) settings_set_project_entry("modelRoles", role, json_str(val));
    else settings_set_global_entry("modelRoles", role, json_str(val));
    char *status;
    if (is_default) {
        if (level && !strcmp(level, "auto")) settings_set_global("defaultThinkingLevel", json_str("auto"));
        char *err = NULL;
        if (app_set_model(u->app, sel, &err) != 0) {
            add_notice(u, "error", err);
            free(err);
        } else {
            mr_usage_record(sel);
            if (concrete) {
                ai_effort_t e;
                if (effort_parse(level, &e) == 0) app_set_thinking(u->app, e);
            }
        }
        status = proj_storage ? xasprintf("%sdefault model: %s", scope_label, sel) : xasprintf("Default model: %s", sel);
    } else {
        char *lab = role_label_of(role);
        status = xasprintf("%s%s model: %s", scope_label, lab, sel);
        free(lab);
    }
    add_notice(u, "info", status);
    free(status);
    free(val);
    free(sel);
}

static void unassign_role(hub_t *h, const char *role, int scope) {
    ui_t *u = h->u;
    bool proj_storage = project_storage();
    int target = proj_storage ? (scope == SC_NONE ? SC_PROJECT : scope) : SC_GLOBAL;
    const char *scope_label = !proj_storage ? "" : target == SC_PROJECT ? "Project " : "Global ";
    char *prev = mr_role_get("default") ? xstrdup(mr_role_get("default")) : NULL;
    if (target == SC_PROJECT) settings_set_project_entry("modelRoles", role, json_null());
    else settings_set_global_entry("modelRoles", role, NULL);
    char *lab = role_label_of(role);
    char *st = xasprintf("%s%s role cleared \xe2\x80\x94 auto-selection applies", scope_label, lab);
    add_notice(u, "info", st);
    free(st);
    free(lab);
    /* a newly exposed persisted default switches the live session (without writing it back) */
    const char *fallback = !strcmp(role, "default") ? mr_role_get("default") : NULL;
    if (fallback) {
        mr_result_t r;
        if (mr_resolve_value(fallback, NULL, &r)) {
            const ai_model_t *live = u->app->model;
            if (!prev || strcmp(prev, fallback) || live != r.model) {
                char *k = mr_model_key(r.model);
                char *err = NULL;
                if (app_set_model(u->app, k, &err) == 0) {
                    char *m = xasprintf("Default model: %s", k);
                    add_notice(u, "info", m);
                    free(m);
                } else {
                    add_notice(u, "error", err);
                    free(err);
                }
                free(k);
            }
        }
        mr_result_clear(&r);
    }
    free(prev);
}

/* onFallbackChainChange: retry.fallbackChains.<key> (an empty chain clears the key) */
static void set_fallback_chain(hub_t *h, const char *key, const strvec_t *chain) {
    json_t *arr = NULL;
    if (chain->n) {
        arr = json_arr();
        for (size_t i = 0; i < chain->n; i++) json_push_str(arr, chain->v[i]);
    }
    settings_set_global_entry("retry.fallbackChains", key, arr);
    char *lab = role_label_of(key);
    char *joined = strvec_join(chain, " \xe2\x86\x92 ");
    char *st = chain->n ? xasprintf("%s fallbacks: %s", lab, joined) : xasprintf("%s fallbacks cleared", lab);
    add_notice(h->u, "info", st);
    free(st);
    free(joined);
    free(lab);
    refresh_after_mutation(h);
}

static void set_cycle_order(hub_t *h, const strvec_t *order) {
    json_t *arr = json_arr();
    for (size_t i = 0; i < order->n; i++) json_push_str(arr, order->v[i]);
    settings_set_global("cycleOrder", arr);
    char *joined = strvec_join(order, " \xe2\x86\x92 ");
    char *st = order->n ? xasprintf("Quick-switch cycle: %s", joined) : xstrdup("Quick-switch cycle cleared");
    add_notice(h->u, "info", st);
    free(st);
    free(joined);
    refresh_after_mutation(h);
}

/* #thinkingChips */
static void push_thinking_chips(hub_t *h, const strvec_t *opts, const char *role, int scope, const char *current, size_t *pre) {
    *pre = 0;
    for (size_t i = 0; i < opts->n; i++) {
        const char *lv = mr_parse_level(opts->v[i], true);
        bool hg;
        char *g;
        char *label = thinking_text(lv, &hg, &g);
        char *styled;
        if (hg) {
            char *ga = S(TUI_TC_ACCENT, g);
            styled = xasprintf("%s %s", ga, label);
            free(ga);
        } else styled = xstrdup(label);
        push_chip(h, label, styled, CH_THINKING, role, scope, lv);
        if (current && !strcmp(current, lv)) *pre = i;
        free(label);
        free(g);
    }
}

static void open_thinking_strip(hub_t *h, const ai_model_t *m, const char *role, bool back, int scope, const char *committed) {
    strvec_t opts = {0};
    thinking_options(m, &opts, true);
    chips_free(h);
    size_t pre;
    push_thinking_chips(h, &opts, role, scope, committed, &pre);
    strvec_free(&opts);
    char *r = xstrdup(role);
    free(h->strip_role);
    h->strip_role = r;
    h->strip = S_THINKING;
    h->chip_index = pre;
    h->strip_model = m;
    h->strip_scope = scope;
    h->strip_return = back;
    h->strip_fallback_index = -1;
    h->strip_initial = committed ? mr_parse_level(committed, true) : "inherit";
}

static const char *thinking_for_scope(const char *role, int scope) {
    const char *lv;
    bool ex;
    role_for_scope(role, scope, &lv, &ex);
    return ex && lv ? lv : "inherit";
}

static void open_scope_strip(hub_t *h, const ai_model_t *m, const char *role, bool back) {
    chips_free(h);
    push_chip(h, "project", S(TUI_TC_ACCENT, "project"), CH_SCOPE, role, SC_PROJECT, NULL);
    push_chip(h, "global", S(TUI_TC_MUTED, "global"), CH_SCOPE, role, SC_GLOBAL, NULL);
    char *r = xstrdup(role);
    free(h->strip_role);
    h->strip_role = r;
    h->strip = S_SCOPE;
    h->chip_index = 0;
    h->strip_model = m;
    h->strip_return = back;
}

/* #assignRole */
static void do_assign(hub_t *h, const ai_model_t *m, const char *role_in, bool back, int scope) {
    char *role = xstrdup(role_in);
    if (project_storage() && scope == SC_NONE) {
        open_scope_strip(h, m, role, back);
        free(role);
        return;
    }
    assign_t *cur;
    assigned(h, role, &cur);
    const char *level = "inherit";
    if (project_storage() && scope != SC_NONE) level = thinking_for_scope(role, scope);
    else if (cur && !cur->auto_selected && cur->level) level = cur->level;
    strvec_t opts = {0};
    thinking_options(m, &opts, true);
    if (!strvec_contains(&opts, level)) level = "inherit";
    assign_role(h, m, role, level, scope);
    refresh_after_mutation(h);
    if (!opts.n) {
        if (back) {
            select_roles_entry(h);
            h->focus_list = true;
            rebuild_browser(h);
        }
    } else open_thinking_strip(h, m, role, back, scope, level);
    free(role);
    strvec_free(&opts);
}

/* #openRoleStrip: only the roles this model can fill */
static void open_role_strip(hub_t *h, const ai_model_t *m) {
    chips_free(h);
    bool scoped = project_storage();
    strvec_t vis = visible_roles(h);
    for (size_t r = 0; r < vis.n; r++) {
        const char *role = vis.v[r];
        if (!mr_role_accepts(role, m)) continue;
        rinfo_t ri = role_info(role);
        assign_t *a;
        assigned(h, role, &a);
        int scopes[2] = {scoped ? SC_PROJECT : SC_GLOBAL, SC_GLOBAL};
        for (int si = 0; si < (scoped ? 2 : 1); si++) {
            int sc = scopes[si];
            const ai_model_t *sm = scoped ? role_for_scope(role, sc, NULL, NULL) : (a && !a->auto_selected ? a->model : NULL);
            bool here = sm && sm == m;
            char *rl = str_lower(ri.label);
            char *label = scoped ? xasprintf("%s %s", role_scope_name(sc), rl) : xstrdup(rl);
            free(rl);
            char *styled;
            if (here) {
                char *t = xasprintf("%s %s", tui_sym("status.enabled"), label), *c = S(ri.color, t);
                char *s2 = xasprintf(" %s", tui_sym("status.success")), *d = S(TUI_TC_DIM, s2);
                styled = xasprintf("%s%s", c, d);
                free(t);
                free(c);
                free(s2);
                free(d);
            } else styled = S(ri.color, label);
            push_chip(h, label, styled, here ? CH_UNASSIGN : CH_ASSIGN, role, scoped ? sc : SC_NONE, NULL);
            free(label);
        }
        rinfo_free(&ri);
    }
    strvec_free(&vis);
    char *fm = xasprintf("fallbacks:%s", m->id), *fp = xasprintf("fallbacks:%s/*", m->provider);
    push_chip(h, fm, S(TUI_TC_MUTED, fm), CH_FALLBACK_MODEL, NULL, SC_NONE, NULL);
    push_chip(h, fp, S(TUI_TC_MUTED, fp), CH_FALLBACK_PROVIDER, NULL, SC_NONE, NULL);
    free(fm);
    free(fp);
    if (mr_role_accepts("default", m)) push_chip(h, "fallback", S(TUI_TC_MUTED, "retry-fallback"), CH_FALLBACK, NULL, SC_NONE, NULL);
    h->strip = S_ROLE;
    h->chip_index = 0;
    h->strip_model = m;
    h->strip_return = false;
}

/* #openFallbackKeyStrip */
static void open_fallback_key_strip(hub_t *h, const ai_model_t *m) {
    chips_free(h);
    char *k = mr_model_key(m);
    char *a = xasprintf("for %s", k), *b = xasprintf("for %s/*", m->provider);
    push_chip(h, a, S(TUI_TC_MUTED, a), CH_FALLBACK_MODEL, NULL, SC_NONE, NULL);
    push_chip(h, b, S(TUI_TC_MUTED, b), CH_FALLBACK_PROVIDER, NULL, SC_NONE, NULL);
    free(a);
    free(b);
    free(k);
    h->strip = S_ROLE;
    h->chip_index = 0;
    h->strip_model = m;
    h->strip_return = false;
}

static void start_assign_kind(hub_t *h, int kind, const char *role, int index) {
    char *r = role ? xstrdup(role) : NULL;
    h->assigning = kind;
    free(h->assign_role);
    h->assign_role = r;
    h->assign_index = index;
    h->focus_list = true;
    if (h->b) {
        free(h->b->query);
        h->b->query = NULL;
    }
    rebuild_browser(h);
    build_sidebar(h);
}

static void start_assign(hub_t *h, const char *role) {
    start_assign_kind(h, A_ROLE, role, -1);
    const ai_model_t *m = assigned(h, h->assign_role, NULL);
    if (m) {
        char *s = mr_model_key(m);
        ui_model_browser_select(h->b, s);
        free(s);
    }
}

static void start_assign_fallback(hub_t *h, const char *key, int index) {
    start_assign_kind(h, A_FALLBACK, key, index);
    if (index >= 0) {
        strvec_t ch = chain_of(h->assign_role);
        if (index < (int)ch.n) {
            const ai_model_t *m = fallback_entry_model(ch.v[index], NULL, NULL);
            if (m) {
                char *s = mr_model_key(m);
                ui_model_browser_select(h->b, s);
                free(s);
            } else ui_model_browser_select(h->b, ch.v[index]);
        }
        strvec_free(&ch);
    }
}

static int find_fallback_row(hub_t *h, const char *key, const char *sel) {
    for (size_t i = 0; i < h->rrows.n; i++)
        if (h->rrows.v[i].kind == R_FALLBACK && !strcmp(h->rrows.v[i].role, key) && !strcmp(h->rrows.v[i].selector, sel))
            return (int)i;
    return -1;
}

static void strvec_remove_at(strvec_t *v, size_t i) {
    free(v->v[i]);
    memmove(&v->v[i], &v->v[i + 1], (v->n - i - 1) * sizeof *v->v);
    v->n--;
}

/* #commitFallback: write the pick into the slot, dedupe, land back on its Roles row */
static void commit_fallback(hub_t *h, const ai_model_t *m, const char *key_in, int index) {
    char *key = xstrdup(key_in);
    strvec_t ch = chain_of(key);
    char *sel = mr_model_key(m);
    if (index >= 0 && index < (int)ch.n) {
        free(ch.v[index]);
        ch.v[index] = xstrdup(sel);
        for (long i = (long)ch.n - 1; i >= 0; i--)
            if (i != index && !strcmp(ch.v[i], sel)) strvec_remove_at(&ch, (size_t)i);
    } else if (!strvec_contains(&ch, sel))
        strvec_push(&ch, sel);
    h->assigning = A_NONE;
    if (h->b) ui_model_browser_set_query(h->b, NULL);
    set_fallback_chain(h, key, &ch);
    select_roles_entry(h);
    h->focus_list = true;
    rebuild_browser(h);
    int ri = find_fallback_row(h, key, sel);
    if (ri >= 0) h->role_index = ri;
    free(key);
    free(sel);
    strvec_free(&ch);
}

static void append_fallback(hub_t *h, const ai_model_t *m, const char *key) {
    strvec_t ch = chain_of(key);
    char *sel = mr_model_key(m);
    if (!strvec_contains(&ch, sel)) {
        strvec_push(&ch, sel);
        set_fallback_chain(h, key, &ch);
    }
    free(sel);
    strvec_free(&ch);
}

/* #setFallbackThinking: an explicit effort is suffixed, inherit is stored bare */
static void set_fallback_thinking(hub_t *h, const char *key_in, int index, const char *level) {
    char *key = xstrdup(key_in);
    strvec_t ch = chain_of(key);
    if (index < (int)ch.n) {
        const char *lv;
        char *up = NULL;
        const ai_model_t *m = fallback_entry_model(ch.v[index], &lv, &up);
        if (m) {
            char *base = mr_model_key(m);
            char *routed = up ? xasprintf("%s@%s", base, up) : xstrdup(base);
            char *next = mr_selector_with_level(routed, level);
            free(ch.v[index]);
            ch.v[index] = xstrdup(next);
            for (long i = (long)ch.n - 1; i >= 0; i--)
                if (i != index && !strcmp(ch.v[i], next)) strvec_remove_at(&ch, (size_t)i);
            set_fallback_chain(h, key, &ch);
            int ri = find_fallback_row(h, key, next);
            if (ri >= 0) h->role_index = ri;
            free(next);
            free(base);
            free(routed);
        }
        free(up);
    }
    strvec_free(&ch);
    free(key);
}

/* #openFallbackThinkingStrip: no `auto` chip */
static void open_fallback_thinking_strip(hub_t *h, const char *key, int idx, const char *selector) {
    const char *lv;
    const ai_model_t *m = fallback_entry_model(selector, &lv, NULL);
    if (!m) return;
    strvec_t opts = {0};
    strvec_push(&opts, "inherit");
    strvec_push(&opts, "off");
    if (m->reasoning)
        for (int i = EFFORT_MINIMAL; i <= EFFORT_MAX; i++)
            if (model_supports_effort(m, (ai_effort_t)i)) strvec_push(&opts, effort_name((ai_effort_t)i));
    const char *current = !lv || !strcmp(lv, "auto") ? "inherit" : lv;
    char *k = xstrdup(key);
    chips_free(h);
    size_t pre;
    push_thinking_chips(h, &opts, k, SC_NONE, current, &pre);
    strvec_free(&opts);
    free(h->strip_role);
    h->strip_role = k;
    h->strip = S_THINKING;
    h->chip_index = pre;
    h->strip_model = m;
    h->strip_fallback_index = idx;
    h->strip_return = true;
}

/* ---------- presets ---------- */

/* saveModelPreset + modelPresetSavedMessage (coding/modelpreset.c) */
static void save_preset(hub_t *h, const char *name) {
    char *msg = mp_save(name);
    add_notice(h->u, "info", msg);
    free(msg);
}

/* applyModelPreset + formatModelPresetSwitch (coding/modelpreset.c) */
static void apply_preset(hub_t *h, const char *name) {
    bool clean = false;
    char *msg = mp_apply(h->u->app, name, &clean, NULL, NULL);
    add_notice(h->u, clean ? "info" : "warning", msg);
    free(msg);
}

/* #switchPreset: previous/next saved preset, wrapping */
static void switch_preset(hub_t *h, int delta) {
    strvec_t names = mp_names();
    if (!names.n) {
        strvec_free(&names);
        return;
    }
    char *active = mp_active();
    const char *base = h->preset_cursor && (!active || h->preset_cursor_rev == h->revision) ? h->preset_cursor : active;
    long cur = -1;
    for (size_t i = 0; base && i < names.n; i++)
        if (!strcmp(names.v[i], base)) cur = (long)i;
    long n = (long)names.n;
    long next = cur < 0 ? (delta > 0 ? 0 : n - 1) : ((cur + delta) % n + n) % n;
    char *name = xstrdup(names.v[next]);
    if (!active || strcmp(name, active)) apply_preset(h, name);
    refresh_after_mutation(h);
    free(h->preset_cursor);
    h->preset_cursor = name;
    h->preset_cursor_rev = h->revision;
    free(active);
    strvec_free(&names);
}

/* ---------- strip activation ---------- */

static void activate_chip(hub_t *h) {
    if (h->chip_index >= h->chips.n) return;
    chip_t *c = &h->chips.v[h->chip_index];
    const ai_model_t *m = h->strip_model;
    int action = c->action, scope = c->scope;
    char *role = c->role ? xstrdup(c->role) : NULL;
    const char *level = c->level;
    switch (action) {
    case CH_ASSIGN:
        h->strip = S_NONE;
        chips_free(h);
        do_assign(h, m, role, false, scope);
        break;
    case CH_UNASSIGN:
        unassign_role(h, role, project_storage() ? scope : SC_NONE);
        refresh_after_mutation(h);
        close_strip(h);
        break;
    case CH_FALLBACK:
        append_fallback(h, m, "default");
        close_strip(h);
        break;
    case CH_FALLBACK_MODEL: {
        char *k = mr_model_key(m);
        close_strip(h);
        start_assign_fallback(h, k, -1);
        free(k);
        break;
    }
    case CH_FALLBACK_PROVIDER: {
        char *k = xasprintf("%s/*", m->provider);
        close_strip(h);
        start_assign_fallback(h, k, -1);
        free(k);
        break;
    }
    case CH_SCOPE: {
        bool back = h->strip_return;
        char *r = xstrdup(h->strip_role);
        h->strip = S_NONE;
        chips_free(h);
        do_assign(h, m, r, back, scope);
        free(r);
        break;
    }
    case CH_THINKING:
        if (h->strip_fallback_index >= 0) {
            int idx = h->strip_fallback_index;
            h->strip = S_NONE;
            chips_free(h);
            h->strip_fallback_index = -1;
            h->chip_ranges.n = 0;
            set_fallback_thinking(h, role, idx, level);
        } else {
            bool changed = strcmp(level, h->strip_initial ? h->strip_initial : "inherit") != 0;
            int sc = h->strip_scope;
            close_strip(h);
            if (changed) {
                assign_role(h, m, role, level, sc);
                refresh_after_mutation(h);
            }
        }
        break;
    }
    free(role);
}

static void submit_name(hub_t *h) {
    char *v = str_trim(sb_cstr(&h->name_input));
    if (h->name_purpose == N_COMPACTION) {
        char *status = NULL, *err = NULL;
        if (cp_set(h->strip_model, sb_cstr(&h->name_input), &status, &err) != 0) {
            free(h->name_error);
            h->name_error = err;
            free(h->name_confirm);
            h->name_confirm = NULL;
        } else {
            add_notice(h->u, "info", status);
            free(status);
            close_strip(h);
            refresh_after_mutation(h);
        }
    } else if (mp_valid_name(v)) {
        if (h->name_purpose == N_PRESET) {
            close_strip(h);
            save_preset(h, v);
            refresh_after_mutation(h);
        } else {
            strvec_t vis = visible_roles(h);
            bool exists = strvec_contains(&vis, v);
            strvec_free(&vis);
            if (!exists) {
                close_strip(h);
                start_assign(h, v);
            }
        }
    }
    free(v);
}

static void open_name_strip(hub_t *h, int purpose) {
    close_strip(h);
    h->strip = S_NAME;
    h->name_purpose = purpose;
}

/* #openCompactionStrip: prefilled with the model's own entry */
static void open_compaction_strip(hub_t *h) {
    const ai_model_t *m = role_row_model(h);
    if (!m) return;
    open_name_strip(h, N_COMPACTION);
    h->strip_model = m;
    char *d = cp_draft(m);
    if (d) sb_append(&h->name_input, d);
    free(d);
}

static void name_key(hub_t *h, const tui_key_t *k) {
    const char *txt = tui_key_text(k);
    if (k->kind == TUI_KEY_CHAR && txt && *txt) sb_append(&h->name_input, txt);
    else if (k->kind == TUI_KEY_PASTE && txt) {
        for (const char *p = txt; *p; p++)
            if (*p != '\n' && *p != '\r') sb_putc(&h->name_input, *p);
    } else if (tui_kb(k, "tui.editor.deleteCharBackward") && h->name_input.len) {
        size_t i = h->name_input.len - 1;
        while (i > 0 && ((unsigned char)h->name_input.s[i] & 0xC0) == 0x80) i--;
        h->name_input.len = i;
        h->name_input.s[i] = 0;
    } else if (tui_kb(k, "tui.editor.deleteToLineStart")) sb_clear(&h->name_input);
    else if (tui_kb(k, "tui.editor.deleteWordBackward")) {
        size_t i = h->name_input.len;
        while (i > 0 && h->name_input.s[i - 1] == ' ') i--;
        while (i > 0 && h->name_input.s[i - 1] != ' ') i--;
        h->name_input.len = i;
        h->name_input.s[i] = 0;
    } else
        return;
    if (h->name_purpose == N_COMPACTION) {
        free(h->name_error);
        h->name_error = NULL;
    }
}

/* ---------- navigation ---------- */

static void move_sidebar(hub_t *h, int delta) {
    size_t n = h->entries.n;
    size_t i = h->active;
    for (size_t step = 0; step < n; step++) {
        i = (i + n + (size_t)(delta < 0 ? n - 1 : 1)) % n;
        if (hop_skipped(h, &h->entries.v[i])) continue;
        if (h->entries.v[i].kind == E_ROLES) h->assigning = A_NONE;
        h->active = i;
        h->follow = true;
        h->focus_list = false;
        rebuild_browser(h);
        return;
    }
}

static void select_all_entry(hub_t *h) {
    for (size_t i = 0; i < h->entries.n; i++)
        if (h->entries.v[i].kind == E_ALL) h->active = i;
    h->follow = true;
    rebuild_browser(h);
}

static bool printable(const tui_key_t *k) {
    const char *t = tui_key_text(k);
    return k->kind == TUI_KEY_CHAR && t && *t && !(k->mods & (TUI_MOD_CTRL | TUI_MOD_ALT | TUI_MOD_SUPER)) && !isspace((unsigned char)*t);
}

/* #stepRoleIndex: skip separators; wraps unless `wrap` is false */
static int step_role(const hub_t *h, int from, int delta, bool wrap) {
    int n = (int)h->rrows.n;
    if (!n) return 0;
    int idx = from;
    for (int i = 0; i < n; i++) {
        int next = idx + delta;
        if (next < 0 || next >= n) {
            if (!wrap) return from;
            idx = (next + n) % n;
        } else
            idx = next;
        if (h->rrows.v[idx].kind != R_SEP) return idx;
    }
    return from;
}

static void cancel_assign(hub_t *h) {
    h->assigning = A_NONE;
    if (h->b) ui_model_browser_set_query(h->b, NULL);
    select_roles_entry(h);
    h->focus_list = true;
    rebuild_browser(h);
    build_sidebar(h);
}

static void activate_role_row(hub_t *h, int index) {
    if (index < 0 || index >= (int)h->rrows.n) return;
    int kind = h->rrows.v[index].kind, ci = h->rrows.v[index].chain_index;
    char *role = h->rrows.v[index].role ? xstrdup(h->rrows.v[index].role) : NULL;
    switch (kind) {
    case R_ROLE: start_assign(h, role); break;
    case R_CHAINKEY: start_assign_fallback(h, role, -1); break;
    case R_FALLBACK: start_assign_fallback(h, role, ci); break;
    case R_NEWFALLBACK: start_assign_kind(h, A_FALLBACKKEY, NULL, -1); break;
    case R_NEWROLE: open_name_strip(h, N_ROLE); break;
    default: break;
    }
    free(role);
}

/* #requestLogin: that provider's login dialog; a successful login reopens the hub on it */
static void login_request(hub_t *h, const entry_t *e) {
    if (!e->provider || !e->oauth) return;
    char *prov = xstrdup(e->provider);
    ui_t *u = h->u;
    close_hub(h);
    ui_overlay_login_provider(u, prov, true);
    free(prov);
}

/* #runRolesAction */
static void roles_action(hub_t *h, char action) {
    const rrow_t *row = cur_row(h);
    int kind = row ? row->kind : -1;
    char *key = row && row->role ? xstrdup(row->role) : NULL;
    int ci = row ? row->chain_index : 0;
    char *sel = row && row->selector ? xstrdup(row->selector) : NULL;
    const char *role = kind == R_ROLE ? key : NULL;
    switch (action) {
    case 'e':
        activate_role_row(h, h->role_index);
        break;
    case 'x':
        if (role) {
            assign_t *a;
            if (assigned(h, role, &a) && !a->auto_selected) {
                int sc = SC_NONE;
                if (project_storage()) {
                    const char *src = role_source(role);
                    sc = !strcmp(src, "project") ? SC_PROJECT : !strcmp(src, "global") ? SC_GLOBAL : SC_NONE;
                }
                unassign_role(h, role, sc);
                refresh_after_mutation(h);
            }
        } else if (kind == R_FALLBACK) {
            strvec_t ch = chain_of(key);
            if (ci < (int)ch.n) {
                strvec_remove_at(&ch, (size_t)ci);
                set_fallback_chain(h, key, &ch);
                h->role_index = MIN(h->role_index, MAX(0, (int)h->rrows.n - 1));
            }
            strvec_free(&ch);
        } else if (kind == R_CHAINKEY) {
            strvec_t empty = {0};
            set_fallback_chain(h, key, &empty);
        }
        break;
    case 'f':
        if (kind == R_NEWFALLBACK) start_assign_kind(h, A_FALLBACKKEY, NULL, -1);
        else if (key && kind != R_NEWROLE && kind != R_SEP) start_assign_fallback(h, key, -1);
        break;
    case 'c':
        if (role) {
            strvec_t order = cycle_order();
            long idx = -1;
            for (size_t i = 0; i < order.n; i++)
                if (!strcmp(order.v[i], role)) idx = (long)i;
            if (idx >= 0) strvec_remove_at(&order, (size_t)idx);
            else strvec_push(&order, role);
            set_cycle_order(h, &order);
            strvec_free(&order);
        }
        break;
    case '[':
    case ']': {
        int delta = action == '[' ? -1 : 1;
        if (role) {
            strvec_t order = cycle_order();
            long idx = -1;
            for (size_t i = 0; i < order.n; i++)
                if (!strcmp(order.v[i], role)) idx = (long)i;
            long to = idx + delta;
            if (idx >= 0 && to >= 0 && to < (long)order.n) {
                char *tmp = order.v[idx];
                order.v[idx] = order.v[to];
                order.v[to] = tmp;
                set_cycle_order(h, &order);
            }
            strvec_free(&order);
        } else if (kind == R_FALLBACK) {
            strvec_t ch = chain_of(key);
            int t = ci + delta;
            if (ci < (int)ch.n && t >= 0 && t < (int)ch.n) {
                char *tmp = ch.v[ci];
                ch.v[ci] = ch.v[t];
                ch.v[t] = tmp;
                set_fallback_chain(h, key, &ch);
                h->role_index += delta;
            }
            strvec_free(&ch);
        }
        break;
    }
    case 'n': open_name_strip(h, N_ROLE); break;
    case 's': open_name_strip(h, N_PRESET); break;
    case 'p': switch_preset(h, 1); break;
    case 'P': switch_preset(h, -1); break;
    case 'k': open_compaction_strip(h); break;
    case 't':
        if (role) {
            int sc;
            const ai_model_t *m = role_thinking_target(h, role, &sc);
            if (m) {
                const char *committed;
                if (project_storage() && sc != SC_NONE) committed = thinking_for_scope(role, sc);
                else {
                    assign_t *a;
                    assigned(h, role, &a);
                    committed = a && a->level ? a->level : "inherit";
                }
                open_thinking_strip(h, m, role, true, sc, committed);
            }
        } else if (kind == R_FALLBACK)
            open_fallback_thinking_strip(h, key, ci, sel);
        break;
    }
    free(key);
    free(sel);
}

static void roles_input(hub_t *h, const tui_key_t *k) {
    if (!h->focus_list) {
        if (tui_key_matches(k, "enter") || tui_key_matches(k, "space")) h->focus_list = true;
        return;
    }
    if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) {
        h->role_index = step_role(h, h->role_index, tui_kb(k, "tui.select.up") ? -1 : 1, true);
        return;
    }
    if (tui_key_matches(k, "enter")) roles_action(h, 'e');
    else if (tui_key_matches(k, "backspace") || tui_key_matches(k, "delete")) roles_action(h, 'x');
    else if (tui_key_matches(k, "shift+up")) roles_action(h, '[');
    else if (tui_key_matches(k, "shift+down")) roles_action(h, ']');
    else {
        const char *txt = tui_key_text(k);
        if (k->kind == TUI_KEY_CHAR && txt && strlen(txt) == 1 && strchr("xfc[]ntksPp", txt[0])) roles_action(h, txt[0]);
    }
}

/* ---------- mouse (hub-frame.ts locate + model-hub.ts #routeMouseEvent) ---------- */

static bool parse_sgr(const char *s, int *button, int *col, int *row, bool *release) {
    if (!s || strncmp(s, "\x1b[<", 3)) return false;
    char fin = 0;
    if (sscanf(s + 3, "%d;%d;%d%c", button, col, row, &fin) != 4 || (fin != 'M' && fin != 'm')) return false;
    *col -= 1;
    *row -= 1;
    *release = fin == 'm';
    return true;
}

static void activate_item(hub_t *h, const ai_model_t *m) {
    if (h->assigning == A_ROLE) {
        char *r = xstrdup(h->assign_role);
        h->assigning = A_NONE;
        do_assign(h, m, r, true, SC_NONE);
        free(r);
    } else if (h->assigning == A_FALLBACKKEY) {
        h->assigning = A_NONE;
        open_fallback_key_strip(h, m);
    } else if (h->assigning == A_FALLBACK) {
        char *r = xstrdup(h->assign_role);
        commit_fallback(h, m, r, h->assign_index);
        free(r);
    } else
        open_role_strip(h, m);
}

static void hub_mouse(hub_t *h, const tui_key_t *k) {
    int button, col, row;
    bool release;
    if (!parse_sgr(k->text, &button, &col, &row, &release)) return;
    int wheel = (button & 64) && !(button & 2) ? ((button & 1) ? 1 : -1) : 0;
    bool motion = (button & 32) && !wheel;
    bool click = !release && !wheel && !motion && (button & 3) == 0;
    int footer_row = 1 + h->content_rows + 1;
    if (row == footer_row && h->strip) {
        if (click && h->strip != S_NAME)
            for (size_t i = 0; i < h->chip_ranges.n; i++)
                if (col >= h->chip_ranges.v[i].start && col < h->chip_ranges.v[i].end) {
                    h->chip_index = (size_t)h->chip_ranges.v[i].index;
                    activate_chip(h);
                    return;
                }
        return;
    }
    int line = row - 1;
    bool over_content = line >= 0 && line < h->content_rows;
    bool over_side = over_content && col >= 2 && col < 2 + h->sidebar_w;
    int body_x = 2 + h->sidebar_w + 3;
    bool over_body = over_content && col >= body_x && col < body_x + h->body_w;
    int body_line = line - 1;
    const entry_t *e = &h->entries.v[h->active];
    bool roles_view = e->kind == E_ROLES && h->assigning == A_NONE;
    int side_idx = over_side && h->sidebar_scroll + line < (int)h->entries.n ? h->sidebar_scroll + line : -1;
    if (wheel) {
        if (over_side) {
            int maxs = MAX(0, (int)h->entries.n - h->content_rows);
            h->sidebar_scroll = MAX(0, MIN(maxs, h->sidebar_scroll + wheel));
            h->sidebar_hover = h->sidebar_scroll + line < (int)h->entries.n ? h->sidebar_scroll + line : -1;
        } else if (over_body) {
            if (roles_view) h->role_index = step_role(h, h->role_index, wheel > 0 ? 1 : -1, false);
            else if (is_browser_view(h) && body_line > 0) ui_model_browser_mouse(h->b, body_line - 1, wheel, false, false);
        }
        return;
    }
    if (motion) {
        h->sidebar_hover = side_idx;
        if (over_body && roles_view) {
            int rl = body_line - h->roles_row_start;
            h->role_hover = rl >= 0 && rl < h->roles_visible ? rl + h->role_scroll : -1;
        } else {
            h->role_hover = -1;
            if (over_body && is_browser_view(h) && body_line > 0) ui_model_browser_mouse(h->b, body_line - 1, 0, true, false);
            else h->b->hovered = -1;
        }
        return;
    }
    if (!click) return;
    if (over_side) {
        if (side_idx < 0 || h->entries.v[side_idx].kind == E_SEP) return;
        bool already = (size_t)side_idx == h->active;
        int kind = h->entries.v[side_idx].kind;
        if (kind == E_ROLES) h->assigning = A_NONE;
        h->active = (size_t)side_idx;
        h->follow = true;
        h->focus_list = kind == E_ROLES;
        rebuild_browser(h);
        if (already && kind == E_PROVIDER && h->entries.v[side_idx].locked) login_request(h, &h->entries.v[side_idx]);
        return;
    }
    if (!over_body) return;
    if (roles_view) {
        h->focus_list = true;
        int rl = body_line - h->roles_row_start;
        if (rl >= 0 && rl < h->roles_visible) {
            int ri = rl + h->role_scroll;
            if (ri < (int)h->rrows.n && h->rrows.v[ri].kind != R_SEP) {
                if (ri == h->role_index) activate_role_row(h, ri);
                else h->role_index = ri;
            }
        }
    } else if (e->kind == E_PROVIDER && e->locked && h->assigning == A_NONE) {
        if (h->locked_login_line >= 0 && body_line == h->locked_login_line) login_request(h, e);
    } else if (is_browser_view(h) && body_line > 0) {
        if (ui_model_browser_mouse(h->b, body_line - 1, 0, false, true) == UI_MB_ACTIVATE) {
            const ui_model_item_t *it = ui_model_browser_selected(h->b);
            if (it) activate_item(h, it->model);
        }
    }
}

/* ---------- keys ---------- */

/* F5: re-probe the provider's model list (local servers; remote catalogs are bundled) */
static void f5_refresh(hub_t *h) {
    const entry_t *e = &h->entries.v[h->active];
    if (e->kind != E_PROVIDER || e->locked) return;
    bool local = false;
    for (size_t i = 0; i < ARRAY_LEN(LOCAL_PROVIDERS); i++)
        if (!strcmp(LOCAL_PROVIDERS[i], e->provider)) local = true;
    if (local || catalog_provider_keyless(e->provider)) catalog_discover_local(true);
    refresh_after_mutation(h);
}

static bool hub_key(tui_component_t *c, const tui_key_t *k) {
    hub_t *h = (hub_t *)c;
    tui_component_request_render(c);
    if (k->kind == TUI_KEY_MOUSE) {
        hub_mouse(h, k);
        return true;
    }
    if (h->strip) {
        if (tui_kb(k, "tui.select.cancel")) close_strip(h);
        else if (h->strip == S_NAME) {
            if (tui_key_matches(k, "enter")) submit_name(h);
            else name_key(h, k);
        } else if (tui_key_matches(k, "left") || tui_key_matches(k, "shift+tab"))
            h->chip_index = (h->chip_index + h->chips.n - 1) % MAX(1, h->chips.n);
        else if (tui_key_matches(k, "right") || tui_key_matches(k, "tab")) h->chip_index = (h->chip_index + 1) % MAX(1, h->chips.n);
        else if (tui_key_matches(k, "enter")) activate_chip(h);
        return true;
    }
    entry_t *e = &h->entries.v[h->active];
    bool roles_view = e->kind == E_ROLES && h->assigning == A_NONE;
    bool locked_view = e->kind == E_PROVIDER && e->locked && h->assigning == A_NONE;
    if (tui_kb(k, "tui.select.cancel")) {
        if (h->assigning != A_NONE) cancel_assign(h);
        else if (is_browser_view(h) && h->b->query && *h->b->query) {
            ui_model_browser_key(h->b, k);
            build_sidebar(h);
            rebuild_browser(h);
        } else close_hub(h);
        return true;
    }
    if (tui_key_matches(k, "tab") || tui_key_matches(k, "shift+tab")) {
        h->focus_list = !h->focus_list;
        return true;
    }
    if (tui_key_matches(k, "f5")) {
        f5_refresh(h);
        return true;
    }
    if (roles_view && (tui_key_matches(k, "ctrl+left") || tui_key_matches(k, "ctrl+right"))) {
        switch_preset(h, tui_key_matches(k, "ctrl+left") ? -1 : 1);
        return true;
    }
    if (tui_key_matches(k, "alt+left") || tui_key_matches(k, "alt+b") || tui_key_matches(k, "alt+right") || tui_key_matches(k, "alt+f")) {
        int d = tui_key_matches(k, "alt+left") || tui_key_matches(k, "alt+b") ? -1 : 1;
        if (roles_view) {
            h->role_tab = (h->role_tab + 3 + d) % 3;
            h->role_index = 0;
            h->role_scroll = 0;
            build_role_rows(h);
        } else {
            int n = (int)ARRAY_LEN(KINDS);
            h->kind_tab = (h->kind_tab + n + d) % n;
            rebuild_browser(h);
        }
        return true;
    }
    if (tui_key_matches(k, "left")) {
        h->focus_list = false;
        return true;
    }
    if (tui_key_matches(k, "right")) {
        if (roles_view || is_browser_view(h)) h->focus_list = true;
        return true;
    }
    if (!h->focus_list) {
        if (tui_kb(k, "tui.select.up")) {
            move_sidebar(h, -1);
            return true;
        }
        if (tui_kb(k, "tui.select.down")) {
            move_sidebar(h, 1);
            return true;
        }
    }
    if (roles_view) {
        bool edits = h->b->query && *h->b->query && (printable(k) || tui_key_matches(k, "backspace"));
        if ((!h->focus_list && printable(k)) || edits) {
            select_all_entry(h);
            h->focus_list = true;
            ui_model_browser_key(h->b, k);
            build_sidebar(h);
            rebuild_browser(h);
            return true;
        }
        roles_input(h, k);
        return true;
    }
    if (locked_view) {
        if (printable(k)) {
            select_all_entry(h);
            h->focus_list = true;
            ui_model_browser_key(h->b, k);
            build_sidebar(h);
            rebuild_browser(h);
        } else if (tui_key_matches(k, "enter"))
            login_request(h, e);
        return true;
    }
    if (!h->focus_list && tui_key_matches(k, "enter")) {
        h->focus_list = true;
        return true;
    }
    char *before = xstrdup(h->b->query ? h->b->query : "");
    int r = ui_model_browser_key(h->b, k);
    bool changed = strcmp(before, h->b->query ? h->b->query : "") != 0;
    if (printable(k) || changed) h->focus_list = true;
    if (changed) {
        build_sidebar(h);
        /* #onQueryChanged: a provider scope that just lost its last match falls back to All models */
        const entry_t *ce = &h->entries.v[h->active];
        if (hub_query(h) && h->assigning == A_NONE && ce->kind == E_PROVIDER && (ce->locked || match_count(h, ce->provider) == 0))
            select_all_entry(h);
        free(h->b->empty_text);
        h->b->empty_text = empty_state(h);
    }
    free(before);
    if (r == UI_MB_ACTIVATE) {
        const ui_model_item_t *it = ui_model_browser_selected(h->b);
        if (it) activate_item(h, it->model);
    }
    return true;
}

static void hub_destroy(tui_component_t *c) {
    hub_t *h = (hub_t *)c;
    entries_free(h);
    vec_free(&h->entries);
    ui_model_browser_free(h->b);
    vec_free(&h->available);
    rrows_free(h);
    vec_free(&h->rrows);
    roles_free(h);
    vec_free(&h->roles);
    strvec_free(&h->known);
    strvec_free(&h->mru);
    chips_free(h);
    vec_free(&h->chips);
    vec_free(&h->chip_ranges);
    free(h->strip_role);
    free(h->assign_role);
    name_reset(h);
    sb_free(&h->name_input);
    free(h->preset_cursor);
    free(h->config_error);
    free(h);
}

static const tui_component_vt HUB_VT = {"model-hub", hub_render, hub_key, NULL, hub_destroy};

void ui_overlay_model_provider(ui_t *u, const char *provider) {
    /* model-picker.ts: re-read only if models.yml changed since the last load (a mid-session edit) */
    catalog_refresh_if_stale();
    hub_t *h = xcalloc(1, sizeof *h);
    tui_component_init(&h->base, &HUB_VT);
    h->base.wants_mouse = true;
    h->u = u;
    h->assigning = A_NONE;
    h->assign_index = -1;
    h->strip_fallback_index = -1;
    h->follow = true;
    h->sidebar_hover = h->role_hover = h->locked_login_line = -1;
    h->content_rows = 10;
    sync_state(h);
    h->active = 1;
    for (size_t i = 0; provider && i < h->entries.n; i++)
        if (h->entries.v[i].kind == E_PROVIDER && str_eq(h->entries.v[i].provider, provider)) h->active = i;
    rebuild_browser(h);
    h->b->focused = false;
    tui_alt_screen(u->tui, true);
    tui_term_puts(tui_term(u->tui), MOUSE_ON);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &h->base, &o);
    tui_set_focus(u->tui, &h->base);
}

void ui_overlay_model(ui_t *u) { ui_overlay_model_provider(u, NULL); }

/* buildSessionModelScope (model-browser.ts): available chat models sorted like sortModelItems, with the role
 * assignments (chips, search affinity) and the MRU order of the hub */
ui_model_browser_t *ui_session_model_browser(void) {
    hub_t h = {0};
    load_available(&h);
    h.mru = mr_usage_order();
    reload_roles(&h);
    ui_role_assign_t *ra = xcalloc(h.roles.n + 1, sizeof *ra);
    for (size_t i = 0; i < h.roles.n; i++) {
        ra[i].role = h.roles.v[i].role;
        ra[i].model = h.roles.v[i].model;
    }
    ui_model_sort_items(h.available.v, h.available.n, ra, h.roles.n, &h.mru);
    free(ra);
    ui_model_browser_t *b = ui_model_browser_new();
    browser_roles(&h, b);
    for (size_t i = 0; i < h.available.n; i++)
        if (!strcmp(model_kind(h.available.v[i]), "chat")) ui_model_browser_add(b, h.available.v[i]);
    roles_free(&h);
    vec_free(&h.roles);
    vec_free(&h.available);
    strvec_free(&h.known);
    strvec_free(&h.mru);
    return b;
}

/* the chrome segment track for other surfaces (Ctrl+P model cycle track in interactive mode) */
char *ui_segment_track(const char *const *labels, size_t n, int active) { return segment_track(labels, n, active); }
