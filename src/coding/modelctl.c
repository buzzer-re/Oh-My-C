/* modelctl.c - service tiers, /fast, /slow and /extended-context (port of omp session/model-controls.ts
 * service-tier half, config/service-tier.ts, agent-session.ts #slowModeTarget / setSlowMode, and
 * slash-commands/builtin-modes.ts runFastCommand / runSlowCommand / applyExtendedContextCommand).
 *
 * The live per-family tier map (ServiceTierByFamily) starts from the `tier.*` settings (or the session's latest
 * `service_tier_change` entry on resume), `--service-tier` overrides its OpenAI entry, and every change is
 * persisted as a `service_tier_change` entry. A request consults only its model's family (Fireworks: the
 * `providers.fireworksTier` setting). */
#include "coding/app.h"
#include "coding/modelctl.h"

#include "ai/anthropic_slow.h"
#include "config/compaction_point.h"
#include "config/settings.h"

#include <ctype.h>

static const char *setting_tier(const char *path) {
    const char *v = settings_str(path, "none");
    return ai_service_tier_intern(v);
}

void modelctl_tiers_from_settings(ai_service_tiers_t *out) {
    out->openai = setting_tier("tier.openai");
    out->anthropic = ai_service_tier_for_family("anthropic", setting_tier("tier.anthropic")) ? setting_tier("tier.anthropic") : NULL;
    out->google = ai_service_tier_for_family("google", setting_tier("tier.google")) ? setting_tier("tier.google") : NULL;
}

/* serviceTierForAllFamilies: one tier broadcast across families, clamped to what each realizes */
void modelctl_tiers_for_all(const char *tier, ai_service_tiers_t *out) {
    memset(out, 0, sizeof *out);
    tier = ai_service_tier_intern(tier);
    if (!tier) return;
    out->openai = tier;
    if (str_eq(tier, "priority")) out->anthropic = tier;
    if (str_eq(tier, "flex") || str_eq(tier, "priority")) out->google = tier;
}

/* resolveSubagentServiceTier: "inherit" takes the parent's live map, a concrete tier is broadcast */
void modelctl_tiers_for_setting(const char *setting, const ai_service_tiers_t *inherited, ai_service_tiers_t *out) {
    if (str_eq(setting, "inherit")) {
        if (inherited) *out = *inherited;
        else memset(out, 0, sizeof *out);
        return;
    }
    modelctl_tiers_for_all(setting, out);
}

bool modelctl_tiers_empty(const ai_service_tiers_t *t) { return !t->openai && !t->anthropic && !t->google; }

json_t *modelctl_tiers_json(const ai_service_tiers_t *t) {
    if (modelctl_tiers_empty(t)) return json_null();
    json_t *o = json_obj();
    if (t->openai) json_set_str(o, "openai", t->openai);
    if (t->anthropic) json_set_str(o, "anthropic", t->anthropic);
    if (t->google) json_set_str(o, "google", t->google);
    return o;
}

/* coerceServiceTierByFamily: known families with a tier valid for them */
void modelctl_tiers_from_json(const json_t *j, ai_service_tiers_t *out) {
    memset(out, 0, sizeof *out);
    if (!json_is_obj(j)) return;
    const char *fam[] = {"openai", "anthropic", "google"};
    const char **slot[] = {&out->openai, &out->anthropic, &out->google};
    for (size_t i = 0; i < 3; i++) {
        const char *v = json_get_str(j, fam[i]);
        if (ai_service_tier_for_family(fam[i], v)) *slot[i] = ai_service_tier_intern(v);
    }
}

static const char **family_slot(ai_service_tiers_t *t, const char *family) {
    if (str_eq(family, "openai")) return &t->openai;
    if (str_eq(family, "anthropic")) return &t->anthropic;
    if (str_eq(family, "google")) return &t->google;
    return NULL;
}

static void with_fireworks(ai_service_tiers_t *t) {
    t->fireworks = str_eq(settings_str("providers.fireworksTier", "standard"), "priority") ? "priority" : NULL;
}

/* #serviceTierFor: Fireworks follows providers.fireworksTier, everything else its family's entry */
static const char *tier_for(const ai_model_t *m, const ai_service_tiers_t *t) {
    ai_service_tiers_t c = *t;
    with_fireworks(&c);
    return ai_resolve_model_service_tier(&c, m);
}

const char *app_effective_service_tier(app_t *a, const ai_model_t *m) {
    pthread_mutex_lock(&a->tier_mu);
    const char *t = tier_for(m, &a->tiers);
    pthread_mutex_unlock(&a->tier_mu);
    return t;
}

/* ---------- Anthropic fast-mode fallback (anthropic-state.ts) ---------- */

static char *fast_key(const ai_model_t *m) { return xasprintf("%s\x1f%s", m->base_url ? m->base_url : "", m->id); }

bool app_anthropic_fast_disabled(app_t *a, const ai_model_t *m) {
    if (!m || !str_eq(m->provider, "anthropic") || !str_eq(m->api, "anthropic-messages")) return false;
    char *k = fast_key(m);
    pthread_mutex_lock(&a->tier_mu);
    bool r = a->fast_disabled && json_get_bool(a->fast_disabled, k, false);
    pthread_mutex_unlock(&a->tier_mu);
    free(k);
    return r;
}

static void fast_disabled_cb(void *ud, const ai_model_t *m) {
    app_t *a = ud;
    char *k = fast_key(m);
    pthread_mutex_lock(&a->tier_mu);
    if (!a->fast_disabled) a->fast_disabled = json_obj();
    json_set_bool(a->fast_disabled, k, true);
    pthread_mutex_unlock(&a->tier_mu);
    free(k);
}

static bool fast_query_cb(void *ud, const ai_model_t *m) { return app_anthropic_fast_disabled(ud, m); }

void app_tier_request_opts(app_t *a, ai_stream_opts_t *o) {
    pthread_mutex_lock(&a->tier_mu);
    o->service_tiers = a->tiers;
    pthread_mutex_unlock(&a->tier_mu);
    with_fireworks(&o->service_tiers);
    o->has_service_tiers = true;
    o->fast_mode_disabled = fast_query_cb;
    o->on_fast_mode_unsupported = fast_disabled_cb;
    o->fast_mode_ud = a;
}

/* clearAnthropicFastModeFallback */
static void clear_fast_fallback(app_t *a) {
    pthread_mutex_lock(&a->tier_mu);
    json_free(a->fast_disabled);
    a->fast_disabled = NULL;
    pthread_mutex_unlock(&a->tier_mu);
}

/* ---------- live map ---------- */

static void persist_tiers(app_t *a) {
    if (!a->session) return;
    json_t *e = json_obj();
    json_set_str(e, "type", "service_tier_change");
    pthread_mutex_lock(&a->tier_mu);
    json_set(e, "serviceTier", modelctl_tiers_json(&a->tiers));
    pthread_mutex_unlock(&a->tier_mu);
    session_append_entry(a->session, e);
}

void app_append_service_tier_entry(app_t *a) { persist_tiers(a); }

static void apply_tiers(app_t *a, const ai_service_tiers_t *next) {
    /* re-arming Anthropic priority clears the per-session fast-mode auto-disable */
    if (str_eq(next->anthropic, "priority") && !str_eq(a->tiers.anthropic, "priority")) clear_fast_fallback(a);
    pthread_mutex_lock(&a->tier_mu);
    a->tiers = *next;
    pthread_mutex_unlock(&a->tier_mu);
    persist_tiers(a);
}

void app_set_service_tier_family(app_t *a, const char *family, const char *tier) {
    ai_service_tiers_t next = a->tiers;
    const char **slot = family_slot(&next, family);
    if (!slot) return;
    tier = ai_service_tier_intern(tier);
    if (*slot == tier) return;
    *slot = tier;
    apply_tiers(a, &next);
}

void app_restore_service_tiers(app_t *a, const ai_service_tiers_t *t) {
    pthread_mutex_lock(&a->tier_mu);
    a->tiers = *t;
    pthread_mutex_unlock(&a->tier_mu);
}

bool app_fast_mode_enabled(app_t *a) {
    const char *f = ai_service_tier_family(a->model);
    const char **slot = f ? family_slot(&a->tiers, f) : NULL;
    return slot && (str_eq(*slot, "priority") || str_eq(*slot, "ultrafast"));
}

bool app_ultrafast_mode_enabled(app_t *a) {
    return a->model && str_eq(ai_service_tier_family(a->model), "openai") && str_eq(a->tiers.openai, "ultrafast");
}

static bool realizes_fast(const ai_model_t *m, const ai_service_tiers_t *t) {
    const char *tier = tier_for(m, t);
    return str_eq(tier, "ultrafast") ? ai_should_send_service_tier(tier, m) : ai_realizes_priority_tier(tier, m);
}

bool app_fast_mode_active(app_t *a) {
    const ai_model_t *m = a->model;
    if (!m) return false;
    pthread_mutex_lock(&a->tier_mu);
    bool r = realizes_fast(m, &a->tiers);
    pthread_mutex_unlock(&a->tier_mu);
    return r && (!str_eq(m->provider, "anthropic") || !app_anthropic_fast_disabled(a, m));
}

bool app_fast_mode_configured_for(app_t *a, const ai_model_t *m) {
    ai_service_tiers_t t;
    modelctl_tiers_from_settings(&t);
    return m && realizes_fast(m, &t);
}

bool app_set_fast_mode(app_t *a, bool enabled) {
    const ai_model_t *m = a->model;
    const char *family = m ? ai_service_tier_family(m) : NULL;
    if (!m || !family) {
        app_notice(a, "info", "The current model has no service-tier control for /fast to toggle.");
        return false;
    }
    if (!enabled) {
        const char *t = *family_slot(&a->tiers, family);
        if (str_eq(t, "priority") || str_eq(t, "ultrafast")) app_set_service_tier_family(a, family, NULL);
        return true;
    }
    if (str_eq(family, "openai") && !ai_should_send_service_tier("priority", m)) {
        app_notice(a, "info", "The current model does not offer the priority (Fast) service tier.");
        return false;
    }
    if (str_eq(family, "anthropic") && str_eq(a->tiers.anthropic, "priority")) clear_fast_fallback(a);
    app_set_service_tier_family(a, family, "priority");
    return true;
}

bool app_set_ultrafast_mode(app_t *a, bool enabled) {
    const ai_model_t *m = a->model;
    if (!enabled) {
        if (str_eq(a->tiers.openai, "ultrafast")) app_set_service_tier_family(a, "openai", NULL);
        return true;
    }
    if (!m || !str_eq(ai_service_tier_family(m), "openai") || !ai_should_send_service_tier("ultrafast", m)) {
        app_notice(a, "info", "The current model does not offer the Ultrafast service tier.");
        return false;
    }
    app_set_service_tier_family(a, "openai", "ultrafast");
    return true;
}

bool app_toggle_fast_mode(app_t *a) {
    if (!app_set_fast_mode(a, !app_fast_mode_enabled(a))) return false;
    return app_fast_mode_enabled(a);
}

/* ---------- /slow (#slowModeTarget) ---------- */

static char *lane_copy(app_t *a) {
    pthread_mutex_lock(&a->tier_mu);
    char *l = a->slow_lane ? xstrdup(a->slow_lane) : NULL;
    pthread_mutex_unlock(&a->tier_mu);
    return l;
}

/* 0 none, 1 flex tier of *family, 2 Anthropic subscription low priority */
static int slow_target(app_t *a, const char **family) {
    const ai_model_t *m = a->model;
    if (!m) return 0;
    if (str_eq(m->provider, "anthropic")) return 2;
    const char *f = ai_service_tier_family(m);
    if (f && ai_service_tier_for_family(f, "flex")) {
        if (family) *family = f;
        return 1;
    }
    return 0;
}

bool app_slow_mode_supported(app_t *a) { return slow_target(a, NULL) != 0; }

const char *app_slow_mode_scope(app_t *a) {
    int k = slow_target(a, NULL);
    return k == 2 ? "global" : k == 1 ? "session" : NULL;
}

bool app_slow_mode_enabled(app_t *a) {
    const char *f = NULL;
    int k = slow_target(a, &f);
    if (k == 2) return str_eq(settings_str("providers.anthropic.slowMode", "off"), "auto");
    if (k == 1) return str_eq(*family_slot(&a->tiers, f), "flex");
    return false;
}

bool app_set_slow_mode(app_t *a, bool enabled) {
    const char *f = NULL;
    int k = slow_target(a, &f);
    if (!k) return false;
    if (k == 1) {
        if (enabled) app_set_service_tier_family(a, f, "flex");
        else if (str_eq(*family_slot(&a->tiers, f), "flex")) app_set_service_tier_family(a, f, NULL);
        return true;
    }
    settings_set_global("providers.anthropic.slowMode", json_str(enabled ? "auto" : "off"));
    char *lane = lane_copy(a);
    if (lane) {
        if (enabled) anthropic_slow_accept(lane);
        else anthropic_slow_stop(lane, "user");
    }
    free(lane);
    return true;
}

char *app_anthropic_slow_label(app_t *a) {
    if (!a->model || !str_eq(a->model->provider, "anthropic")) return NULL;
    char *lane = lane_copy(a);
    char *r = lane ? anthropic_slow_label(lane, str_eq(settings_str("providers.anthropic.slowMode", "off"), "auto")) : NULL;
    free(lane);
    return r;
}

/* ---------- slow-mode hooks (SettingsStreamSlowModeContext) ---------- */

static bool hook_low_priority(void *ud) {
    UNUSED(ud);
    return str_eq(settings_str("providers.anthropic.slowMode", "off"), "auto");
}

static void hook_notify(void *ud, const char *level, const char *message) { app_notice(ud, level, "%s", message); }

static void hook_on_lane(void *ud, const char *lane) {
    app_t *a = ud;
    pthread_mutex_lock(&a->tier_mu);
    if (!str_eq(a->slow_lane, lane)) {
        free(a->slow_lane);
        a->slow_lane = xstrdup(lane);
    }
    pthread_mutex_unlock(&a->tier_mu);
}

void app_slow_hooks_init(app_t *a) {
    a->slow_hooks = (anthropic_slow_hooks_t){hook_low_priority, hook_notify, hook_on_lane, a};
    a->wrap_up_hinted = -1;
}

/* #steerAnthropicWrapUp: the wrap-up window key to steer once, or -1 */
int64_t app_anthropic_wrap_up_key(app_t *a) {
    if (!a->model || !str_eq(a->model->provider, "anthropic")) return -1;
    char *lane = lane_copy(a);
    int64_t k = lane ? anthropic_slow_wrap_up_hint_key(lane, hook_low_priority(NULL)) : -1;
    free(lane);
    if (k < 0 || k == a->wrap_up_hinted) return -1;
    a->wrap_up_hinted = k;
    return k;
}

/* ---------- slash commands ---------- */

static const char *fast_status(app_t *a) {
    if (app_ultrafast_mode_enabled(a)) return "ultra";
    return app_fast_mode_enabled(a) ? "on" : "off";
}

char *modelctl_fast_description(app_t *a) { return xasprintf("Fast: %s", fast_status(a)); }

char *modelctl_fast_command(app_t *a, const char *arg) {
    if (!*arg || str_eq(arg, "toggle")) return xasprintf("Fast mode %s.", app_toggle_fast_mode(a) ? "enabled" : "disabled");
    if (str_eq(arg, "on")) return xstrdup(app_set_fast_mode(a, true) ? "Fast mode enabled." : "Fast mode is unavailable for the current model.");
    if (str_eq(arg, "ultra") || str_eq(arg, "ultrafast"))
        return xstrdup(app_set_ultrafast_mode(a, true) ? "Ultrafast mode enabled." : "Ultrafast is unavailable for the current model.");
    if (str_eq(arg, "off")) {
        app_set_fast_mode(a, false);
        return xstrdup("Fast mode disabled.");
    }
    if (str_eq(arg, "status")) return xasprintf("Fast mode is %s.", fast_status(a));
    return NULL;
}

#define SLOW_UNSUPPORTED                                                                                               \
    "The current model has no slow mode: /slow uses the flex tier on OpenAI/Google models and low priority on "        \
    "Anthropic subscriptions."

char *modelctl_slow_command(app_t *a, const char *arg) {
    if (*arg && !str_eq(arg, "toggle") && !str_eq(arg, "on") && !str_eq(arg, "off") && !str_eq(arg, "status")) return NULL;
    bool anthropic = a->model && str_eq(a->model->provider, "anthropic");
    if (str_eq(arg, "status")) {
        char *label = anthropic ? app_anthropic_slow_label(a) : NULL;
        char *r;
        if (!app_slow_mode_enabled(a)) r = label ? xasprintf("Slow mode is off (%s).", label) : xstrdup("Slow mode is off.");
        else if (!anthropic) r = xstrdup("Slow mode is on (flex tier).");
        else r = label ? xasprintf("Slow mode is on (%s).", label) : xstrdup("Slow mode is on (low priority at the Claude session limit).");
        free(label);
        return r;
    }
    bool enabled = str_eq(arg, "on") || (!str_eq(arg, "off") && !app_slow_mode_enabled(a));
    if (!app_set_slow_mode(a, enabled)) return xstrdup(SLOW_UNSUPPORTED);
    if (!app_slow_mode_enabled(a))
        return xstrdup(anthropic ? "Slow mode off: at your Claude usage limit, requests may get a short wrap-up allowance, then "
                                   "wait for the limit to reset."
                                 : "Slow mode off.");
    if (!anthropic) return xstrdup("Slow mode on: requests use the flex tier (lower cost, higher latency).");
    char *lane = lane_copy(a);
    int64_t resets = lane ? anthropic_slow_active_resets_at(lane) : -1;
    free(lane);
    if (resets >= 0) {
        char *clock = anthropic_slow_reset_clock(resets);
        char *r = xasprintf("Slow mode on: continuing at low priority until your limit resets at %s. Your weekly limit still "
                            "applies, and responses may pause while waiting for spare capacity.",
                            clock);
        free(clock);
        return r;
    }
    return xstrdup("Slow mode on: when your Claude subscription hits its session limit and Anthropic offers low priority, "
                   "requests switch to it after any wrap-up allowance.");
}

char *modelctl_slow_description(app_t *a) { return xstrdup(app_slow_mode_enabled(a) ? "Slow mode: on" : "Slow mode: off"); }

/* ---------- extended context ---------- */

static bool opt_in(const ai_model_t *m, int64_t standard) { return cp_reaches(m, standard); }

void modelctl_apply_context_policy(void) { catalog_apply_context_policy(settings_bool("extendedContext", false), opt_in); }

/* cfgExtendedContext.listen -> #reapplyContextWindowPolicy (also compaction.modelThresholds opt-ins) */
static void on_settings_change(void *ud) {
    UNUSED(ud);
    modelctl_apply_context_policy();
}

void modelctl_watch_settings(void) {
    static bool done;
    if (done) return;
    done = true;
    settings_on_change(on_settings_change, NULL);
}

char *modelctl_extended_context_description(void) {
    return xasprintf("Extended context: %s", settings_bool("extendedContext", false) ? "on" : "off");
}

char *modelctl_extended_context_command(const char *args) {
    char *arg = str_trim(args ? args : "");
    for (char *p = arg; *p; p++) *p = (char)tolower((unsigned char)*p);
    bool cur = settings_bool("extendedContext", false);
    char *r = NULL;
    int set = -1;
    if (!*arg || str_eq(arg, "toggle")) {
        set = !cur;
        r = xasprintf("Extended context %s.", set ? "enabled" : "disabled");
    } else if (str_eq(arg, "on")) {
        set = 1;
        r = xstrdup("Extended context enabled.");
    } else if (str_eq(arg, "off")) {
        set = 0;
        r = xstrdup("Extended context disabled.");
    } else if (str_eq(arg, "status")) {
        r = xasprintf("Extended context is %s.", cur ? "on" : "off");
    }
    if (set >= 0) {
        settings_set_global("extendedContext", json_bool(set));
        modelctl_apply_context_policy();
    }
    free(arg);
    return r;
}

/* ---------- RPC (rpc-mode.ts set_fast_mode / set_slow_mode / set_cache_warming) ---------- */

json_t *modelctl_rpc(app_t *a, const char *type, const json_t *cmd, bool *handled, char **err) {
    *handled = true;
    if (!strcmp(type, "set_fast_mode")) {
        bool enabled = json_truthy(json_get(cmd, "enabled"));
        bool supported = app_set_fast_mode(a, enabled);
        if (enabled && !supported) {
            *err = xstrdup("Fast mode is unavailable for the current model.");
            return NULL;
        }
        json_t *d = json_obj();
        json_set_bool(d, "enabled", app_fast_mode_enabled(a));
        json_set_bool(d, "active", app_fast_mode_active(a));
        return d;
    }
    if (!strcmp(type, "set_slow_mode")) {
        const json_t *en = json_get(cmd, "enabled");
        if (!json_is_bool(en)) { /* a truthy non-boolean must not flip a persisted global setting */
            *err = xstrdup("set_slow_mode requires boolean enabled");
            return NULL;
        }
        bool supported = app_set_slow_mode(a, en->u.b);
        if (en->u.b && !supported) {
            *err = xstrdup("Slow mode is unavailable for the current model.");
            return NULL;
        }
        json_t *d = json_obj();
        json_set_bool(d, "enabled", app_slow_mode_enabled(a));
        return d;
    }
    if (!strcmp(type, "set_cache_warming")) {
        const char *mode = json_get_str(cmd, "mode");
        if (!str_eq(mode, "off") && !str_eq(mode, "streaming") && !str_eq(mode, "idle")) {
            char *v = json_dump_js(json_get(cmd, "mode"));
            *err = xasprintf("Invalid cache warming mode: %s", json_is_str(json_get(cmd, "mode")) ? mode : json_get(cmd, "mode") ? v : "undefined");
            free(v);
            return NULL;
        }
        /* setCacheWarmingMode: a session override, never written to config.yml */
        settings_set_runtime("providers.cacheWarming", json_str(mode));
        json_t *d = json_obj();
        json_set_str(d, "mode", settings_str("providers.cacheWarming", "idle"));
        return d;
    }
    *handled = false;
    return NULL;
}
