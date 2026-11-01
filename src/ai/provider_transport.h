/* provider_transport.h - provider-owned request shaping before API dispatch (omp registry prepareModel /
 * prepareRequest + the provider branches of the OpenAI/Anthropic request setup). ai_stream runs it for every
 * request: structured credentials stored by login flows (Cloudflare JSON keys, Muse key bundles, Snowflake
 * account URLs, QwenCloud Token Plan keys, Copilot enterprise/endpoint keys) become the bearer, base URL and
 * headers the wire needs. */
#ifndef OMC_PROVIDER_TRANSPORT_H
#define OMC_PROVIDER_TRANSPORT_H

#include "ai.h"

typedef struct {
    ai_model_t model;       /* shallow copy of the catalog model with the overrides below */
    ai_stream_opts_t opts;  /* copy of the caller options (api_key/headers replaced) */
    char *api_key, *base_url, *api;
    json_t *raw;            /* cloned model row when a field (requestModelId) changed */
    json_t *model_headers;  /* replacement model headers */
    json_t *opt_headers;    /* replacement option headers */
    char *copilot_cache_key, *copilot_snapshot;
    double premium_requests;
    bool has_premium;
} ai_transport_t;

/* 0 ok; -1 with *err (owned): the credential cannot address the endpoint */
int ai_transport_prepare(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_transport_t *t,
                         char **err);
void ai_transport_own_raw(ai_transport_t *t);
void ai_transport_finish(const ai_transport_t *t, json_t *msg); /* usage.premiumRequests for Copilot */
void ai_transport_free(ai_transport_t *t);

#endif
