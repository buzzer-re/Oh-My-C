/*
 * kiro.h - the native Kiro provider (api "kiro-api"; omp 2edca483d9 and its follow-ups, final state at v19.1.0).
 * Ports catalog/src/wire/{kiro,kiro-cli}.ts (routing, credential codec, the Kiro CLI's wire identity),
 * catalog/src/discovery/kiro.ts (AWS JSON 1.0 management client, ListAvailableModels mapping),
 * catalog/src/provider-models/special.ts kiroModelManagerOptions (per-credential discovery) and
 * ai/src/providers/kiro/{transform,stream}.ts (request transform, event-stream decoding). The login and refresh hooks
 * live in config/auth_custom.c, the usage fetcher (usage/kiro.ts) in config/usage_providers.c.
 *
 * Test hook: management and runtime requests honor OMC_PROVIDER_URL_REWRITE, then OMC_AUTH_URL_REWRITE.
 */
#ifndef OMC_KIRO_H
#define OMC_KIRO_H

#include "ai/ai.h"
#include "net/http.h"

#define KIRO_API_KEY_PREFIX "ksk_"
#define KIRO_SOCIAL_CLIENT_ID "Kiro-CLI"

/* ---------- routing + credential codec (wire/kiro.ts) ---------- */

/* KIRO_BOOTSTRAP_REGIONS: us-east-1, eu-central-1 */
extern const char *const KIRO_BOOTSTRAP_REGIONS[2];
/* kiroProfileRegions: the GovCloud pair for a us-gov-* OIDC region, else the bootstrap regions (2 entries) */
const char *const *kiro_profile_regions(const char *oidc_region);
bool kiro_is_region(const char *s);
/* the region a profile ARN routes to (owned), or NULL when the value is not a profile ARN */
char *kiro_profile_region(const char *arn);
char *kiro_runtime_base_url(const char *region);    /* https://runtime.<region>.kiro.dev/ (owned) */
char *kiro_management_base_url(const char *region); /* https://management.<region>.kiro.dev/ (owned) */

typedef struct {
    bool oauth;        /* else a ksk_ API key */
    char *token;
    char *region;      /* NULL: an API key with no region */
    char *login_id;    /* oauth only */
    char *profile_arn; /* oauth, optional */
} kiro_cred_t;

void kiro_cred_free(kiro_cred_t *c);
/* encodeKiroCredential (JSON, upstream key order). Owned. */
char *kiro_encode_credential(const kiro_cred_t *c);
/* decodeKiroCredential: the encoded form or a raw ksk_ key; an unpinned key takes KIRO_API_REGION. 0, or -1 + *err */
int kiro_decode_credential(const char *value, kiro_cred_t *out, char **err);
/* encodeKiroOAuthRow: the request key of a stored OAuth row {access, region, accountId, kiroProfileArn?}; NULL + *err
 * "Kiro login is incomplete; run /login kiro again" when the row lacks its region or login id */
char *kiro_encode_oauth_row(const json_t *row, char **err);
/* model-cache key of one credential (kiroCredentialCacheScope hashed, like the kiro:models-v2 cache id). Owned. */
char *kiro_credential_cache_key(const kiro_cred_t *c);

/* ---------- the Kiro CLI's headers (wire/kiro-cli.ts) ---------- */

/* operation: GenerateAssistantResponse | ListAvailableModels | ListAvailableProfiles | GetUsageLimits */
void kiro_service_headers(http_headers_t *h, const char *operation, const char *token, int attempt, int max_attempts);
void kiro_oidc_headers(http_headers_t *h, int attempt, int max_attempts);
void kiro_social_headers(http_headers_t *h);

/* ---------- management client (discovery/kiro.ts) ---------- */

/* One AWS JSON 1.0 POST to the region's management host (10 s, 1 MiB). Owned JSON, or NULL with *status (0 on a
 * transport failure) and *err "<target>: HTTP <status>" / the transport error. */
json_t *kiro_management_request(const char *region, const char *token, const char *target, const json_t *body,
                                volatile sig_atomic_t *cancel, int *status, char **err);
/* listKiroProfiles: [{arn, name?}] over every nextToken page; NULL + *err on failure */
json_t *kiro_list_profiles(const char *region, const char *token, volatile sig_atomic_t *cancel, char **err);
/* resolveKiroApiKeyRegion: the single bootstrap region whose ListAvailableModels accepts the key (owned), else NULL */
char *kiro_resolve_api_key_region(const char *token, volatile sig_atomic_t *cancel);
/* fetchKiroModels: model specs over every page (an unpinned key probes the bootstrap regions first); NULL on failure
 * or an empty roster */
json_t *kiro_fetch_models(const kiro_cred_t *c, volatile sig_atomic_t *cancel);
/* mapModel for one ListAvailableModels entry (NULL when skipped). Owned spec. */
json_t *kiro_map_model(const json_t *raw, const char *base_url);

/* Per-credential discovery (kiroModelManagerOptions): registers the roster of the stored / env Kiro credential,
 * cached 2 h per credential in <agent dir>/kiro-models.json (a failed refresh keeps the last cache); force refetches.
 * Returns the models registered. */
int kiro_discover(bool force);

/* ---------- request + stream (providers/kiro) ---------- */

/* transformKiroRequest. Owned request, or NULL + *err (owned); *overflow when it exceeds the local byte budget. */
json_t *kiro_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, char **err,
                           bool *overflow);
json_t *provider_kiro_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb,
                             void *ud);

#endif
