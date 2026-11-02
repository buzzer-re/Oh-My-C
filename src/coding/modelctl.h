/* modelctl.h - service tiers (/fast, /slow, tier.*, --service-tier), Anthropic slow mode wiring and
 * /extended-context (port of omp session/model-controls.ts service tiers, config/service-tier.ts,
 * agent-session.ts slow mode, slash-commands/builtin-modes.ts). */
#ifndef OMC_MODELCTL_H
#define OMC_MODELCTL_H

#include "ai/ai.h"

typedef struct app app_t;

/* buildServiceTierByFamily from the tier.openai / tier.anthropic / tier.google settings */
void modelctl_tiers_from_settings(ai_service_tiers_t *out);
/* serviceTierForAllFamilies / resolveSubagentServiceTier ("inherit" takes `inherited`) */
void modelctl_tiers_for_all(const char *tier, ai_service_tiers_t *out);
void modelctl_tiers_for_setting(const char *setting, const ai_service_tiers_t *inherited, ai_service_tiers_t *out);
bool modelctl_tiers_empty(const ai_service_tiers_t *t);
json_t *modelctl_tiers_json(const ai_service_tiers_t *t); /* {family: tier} or null (owned) */
void modelctl_tiers_from_json(const json_t *j, ai_service_tiers_t *out); /* coerceServiceTierByFamily */

/* live session tiers */
const char *app_effective_service_tier(app_t *a, const ai_model_t *m); /* the wire tier for a request to m */
void app_tier_request_opts(app_t *a, ai_stream_opts_t *o); /* live tiers + fast-mode fallback state */
void app_set_service_tier_family(app_t *a, const char *family, const char *tier); /* persists service_tier_change */
void app_restore_service_tiers(app_t *a, const ai_service_tiers_t *t);            /* no entry written */
void app_append_service_tier_entry(app_t *a);
bool app_fast_mode_enabled(app_t *a);   /* the active model's family is priority/ultrafast (/fast on) */
bool app_ultrafast_mode_enabled(app_t *a);
bool app_fast_mode_active(app_t *a);    /* a fast tier is realized on the wire for the active model (status badge) */
bool app_fast_mode_configured_for(app_t *a, const ai_model_t *m);
bool app_set_fast_mode(app_t *a, bool enabled);
bool app_set_ultrafast_mode(app_t *a, bool enabled);
bool app_toggle_fast_mode(app_t *a);
bool app_anthropic_fast_disabled(app_t *a, const ai_model_t *m); /* fast mode auto-disabled after a rejection */
bool app_slow_mode_supported(app_t *a);
bool app_slow_mode_enabled(app_t *a);
bool app_set_slow_mode(app_t *a, bool enabled);
const char *app_slow_mode_scope(app_t *a); /* "session" | "global" | NULL */
char *app_anthropic_slow_label(app_t *a);  /* status-line usage-limit label (owned) or NULL */

void app_slow_hooks_init(app_t *a);
int64_t app_anthropic_wrap_up_key(app_t *a); /* wrap-up window to steer the model about once, -1 none */

/* slash commands: reply text (owned), or NULL for an unknown argument (the caller prints the usage) */
char *modelctl_fast_command(app_t *a, const char *arg);
char *modelctl_slow_command(app_t *a, const char *arg);
char *modelctl_extended_context_command(const char *args);
char *modelctl_fast_description(app_t *a); /* getTuiAutocompleteDescription */
char *modelctl_slow_description(app_t *a);
char *modelctl_extended_context_description(void);

/* re-apply the context-window policy (extendedContext + compaction.modelThresholds opt-ins) to the catalog */
void modelctl_apply_context_policy(void);
void modelctl_watch_settings(void); /* re-apply it whenever settings change (once per process) */

#endif
