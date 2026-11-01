/* model_resolver.h - model selection shared by every role consumer (port of omp
 * config/model-resolver.ts, config/model-roles.ts, tiny/online-candidates.ts and
 * session/retry-fallback-chains.ts).
 *
 * Layering like omp: mr_match() is the single matching engine (exact provider/id reference with
 * spelling / wire-route / OpenRouter fallbacks -> exact bare id -> substring with the alias-vs-dated
 * pick, preference-ranked by MRU usage then provider priority); mr_parse_pattern() layers the
 * `:level` thinking suffix and `@upstream` grammar on top; role aliases (`@role`, `pi/role`, `*`)
 * expand through configured modelRoles, ROLE_CONFIGURED_FALLBACK and the catalog's role-default
 * chains (mr_configured_patterns); mr_resolve_value() resolves a role value against a pool;
 * mr_role_chain() is resolveRoleChain (primary + retry.fallbackChains / role defaults).
 *
 * Thinking levels are the omp selector strings ("inherit", "off", "minimal", "low", "medium",
 * "high", "xhigh", "max", "auto"); all returned level pointers are static strings. */
#ifndef OMC_MODEL_RESOLVER_H
#define OMC_MODEL_RESOLVER_H

#include "ai/ai.h"
#include "core/core.h"
#include "core/json.h"

/* ---------- pools ---------- */
typedef struct {
    const ai_model_t **v;
    size_t n, cap;
} mr_pool_t;

void mr_pool_free(mr_pool_t *p);
void mr_pool_push(mr_pool_t *p, const ai_model_t *m);
/* registry.getAvailable(kind): models of an available provider (credentials or keyless), minus
 * disabledProviders; kind NULL or "all" = every kind, else modelKind == kind (omp default "chat") */
void mr_pool_available(mr_pool_t *p, const char *kind);
/* registry.getAll(kind) (catalog order, disabled providers removed) */
void mr_pool_all(mr_pool_t *p, const char *kind);
/* roleCandidatePool(role): available "all" models the role accepts */
void mr_pool_role(mr_pool_t *p, const char *role);
bool mr_pool_contains(const mr_pool_t *p, const ai_model_t *m);

bool mr_provider_available(const char *provider); /* credentials, keyless provider, not disabled */
bool mr_provider_disabled(const char *provider);
bool mr_provider_known(const char *provider);     /* in the catalog or providers.json */

/* ---------- roles ---------- */
const char *mr_model_kind(const ai_model_t *m); /* raw "kind" or "chat" */
bool mr_is_builtin_role(const char *role);
bool mr_is_kind_role(const char *role);   /* image web speech dictation judge */
bool mr_role_accepts(const char *role, const ai_model_t *m); /* getRoleInfo(role).accepts */
extern const char *const MR_ROLE_IDS[];   /* MODEL_ROLE_IDS (16 built-ins) */
extern const size_t MR_ROLE_COUNT;
extern const size_t MR_CHAT_ROLE_COUNT;   /* the first 11 are CHAT_MODEL_ROLE_IDS */
/* getKnownRoleIds: visible built-ins, then cycleOrder, configured modelRoles, modelTags roles */
strvec_t mr_known_roles(void);
/* settings.getModelRole(role): effective modelRoles.<role> (string or {model:..}) or NULL; borrowed */
const char *mr_role_get(const char *role);

/* ---------- selectors ---------- */
/* parseThinkingSuffix: static level string or NULL; allow_guarded admits "max" / "auto" */
const char *mr_parse_level(const char *s, bool allow_guarded);
/* splitThinkingSuffix: *base owned; returns the level (static) or NULL */
const char *mr_split_level(const char *pattern, int min_colon, bool allow_guarded, char **base);
/* "provider/id" (owned) */
char *mr_model_key(const ai_model_t *m);
/* formatModelSelectorValue */
char *mr_selector_with_level(const char *selector, const char *level);
/* normalizeModelPatternList over a comma-separated string or JSON array/string */
strvec_t mr_pattern_list(const char *value);

/* built-in priority patterns for a role (rolePriorityDefaults: catalog role-defaults, aliases) */
strvec_t mr_role_priority_defaults(const char *role);
/* resolveConfiguredModelPatterns: role aliases expanded through configuration and defaults */
strvec_t mr_configured_patterns(const char *value);
/* expandRoleAlias (owned) */
char *mr_expand_role_alias(const char *value);
/* resolveExplicitModelRole: the first role alias named by a value (owned) or NULL */
char *mr_explicit_role(const char *value);

/* ---------- matching ---------- */
typedef struct {
    const ai_model_t *model;
    const char *level;       /* thinking level (static) or NULL */
    bool explicit_level;
    int matched_index;       /* index of the configured pattern that matched (resolve_value) */
    char *warning;           /* owned or NULL */
} mr_result_t;
void mr_result_clear(mr_result_t *r);

/* matchModel; exact_only stops before the substring phase */
const ai_model_t *mr_match(const char *pattern, const mr_pool_t *pool, bool exact_only);
/* parseModelPattern (+ `@upstream` fallback) */
bool mr_parse_pattern(const char *pattern, const mr_pool_t *pool, mr_result_t *out);
/* resolveModelRoleValue: pool NULL = available chat models */
bool mr_resolve_value(const char *value, const mr_pool_t *pool, mr_result_t *out);
/* resolveModelFromString */
const ai_model_t *mr_resolve_string(const char *value, const mr_pool_t *pool);
/* resolveModelOverride(patterns): first pattern that resolves over the available pool */
bool mr_resolve_override(const strvec_t *patterns, mr_result_t *out);
/* resolveRoleSelection([role]) over the configured value only (no defaults); pool NULL = available */
const ai_model_t *mr_role_selection(const char *role, const mr_pool_t *pool, const char **level);
/* resolveModelRoleValue("@role") with the role's candidate pool (auto-selection incl. defaults) */
const ai_model_t *mr_resolve_role(const char *role, const char **level);
/* pickDefaultAvailableModel over the available chat pool */
const ai_model_t *mr_pick_default_available(const mr_pool_t *pool);
/* the session default: modelRoles.default resolved over the available pool, else
 * pickDefaultAvailableModel (sdk.ts + findInitialModel) */
const ai_model_t *mr_default_model(const char **level);

/* ---------- role chains ---------- */
typedef struct {
    const ai_model_t *model;
    bool explicit_;
    const char *level;
    char *replaces; /* owned or NULL */
} mr_candidate_t;
typedef struct {
    mr_candidate_t *v;
    size_t n, cap;
} mr_chain_t;
void mr_chain_free(mr_chain_t *c);
/* resolveRoleChain(role, pool): pool NULL = mr_pool_role(role) */
void mr_role_chain(const char *role, const mr_pool_t *pool, mr_chain_t *out);
/* collectOnlineTinyCandidates(roles) (+ current model and its own chain like the title generator
 * when `current` is set); pool NULL = role pool of "tiny" */
void mr_online_tiny_candidates(const char *const *roles, size_t nroles, const ai_model_t *current, mr_pool_t *out);

/* ---------- retry fallback chains (session/retry-fallback-chains.ts) ---------- */
typedef struct {
    char *raw;
    char *provider;
    char *id;
    const char *level; /* static or NULL */
} mr_selector_t;
void mr_selector_free(mr_selector_t *s);
bool mr_parse_selector(const char *selector, mr_selector_t *out); /* parseRetryFallbackSelector */
/* chain key for the live selector (resolveRetryFallbackChainKey) over the effective chains
 * (configured + default chain applied to configured roles); owned or NULL */
char *mr_fallback_chain_key(const char *current_selector, const ai_model_t *current, const char *role_hint);
/* candidates after the current selector in the chain of `key` (findRetryFallbackCandidates) */
size_t mr_fallback_candidates(const char *key, const char *current_selector, const ai_model_t *current, bool wrap,
                              mr_selector_t **out);
/* retry.fallbackChains entry -> warnings (validateRetryFallbackChains); returns count, appends to out */
size_t mr_fallback_validate(strvec_t *out);
/* selector cooldowns (modelRegistry.suppressSelector / isSelectorSuppressed) */
void mr_suppress_selector(const char *selector, int64_t until_ms);
bool mr_selector_suppressed(const char *selector);
/* retry.fallbackOn admission */
bool mr_fallback_admits(bool usage_limit);
/* calculateRetryBackoffDelayMs (capped 8 s, 25% downward jitter) */
int64_t mr_retry_backoff_ms(int64_t base_ms, int attempt);
/* rate-limit reason heuristics (parseRateLimitReason + calculateRateLimitBackoffMs) */
int64_t mr_rate_limit_backoff_ms(const char *error_message);

/* ---------- model usage (MRU) ---------- */
void mr_usage_record(const char *model_key);
strvec_t mr_usage_order(void); /* most recent first */

#endif
