/*
 * provider_google.h - Gemini generateContent providers ("google-generative-ai", "google-vertex") internals and the
 * google-shared mapping reused by the Cloud Code Assist provider ("google-gemini-cli", provider_gemini_cli.c).
 *
 * provider_google_stream (ai.h) serves both generateContent apis: it dispatches on model->api.
 */
#ifndef OMC_PROVIDER_GOOGLE_H
#define OMC_PROVIDER_GOOGLE_H

#include "ai/ai.h"

/* Wire request body (contents, systemInstruction, tools, toolConfig, generationConfig, ...). Owned. */
json_t *google_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o);

/* Gemini-API streaming URL: `{base}/models/{id}:streamGenerateContent?alt=sse`. Owned. */
char *google_request_url(const ai_model_t *m);

/* normalizeSchemaForGoogle (owned). */
json_t *google_normalize_schema(const json_t *schema);
/* normalizeSchemaForCCA: legacy `parameters` schema for Claude behind Cloud Code Assist (owned). */
json_t *google_normalize_schema_cca(const json_t *schema);

/* ---------- google-shared ---------- */
/* convertMessages over already-transformed messages (ai_transform_messages). Owned `contents` array. */
json_t *google_convert_messages(const ai_model_t *m, const json_t *transformed);
/* convertTools: [{functionDeclarations:[...]}] or NULL when there are no tools. Owned. */
json_t *google_convert_tools(const ai_model_t *m, const json_t *tools);
/* nextToolCallId: `<name>_<ms>_<n>` (owned). */
char *google_next_tool_call_id(const char *name);
/* mapGoogleUsage onto the builder's usage. */
void google_apply_usage(ai_builder_t *b, const json_t *usage_metadata);
/* thinking helpers (stream.ts Google branches) */
ai_effort_t google_clamp_effort(const ai_model_t *m, ai_effort_t e);
const char *google_thinking_level(ai_effort_t e, const ai_model_t *m); /* mapEffortToGoogleThinkingLevel */
int64_t google_thinking_headroom(const ai_model_t *m, const ai_stream_opts_t *o, ai_effort_t e);
int64_t google_max_output(int64_t base, const ai_model_t *m, int64_t headroom); /* googleMaxOutputTokens; -1 unset */
int64_t google_effort_budget(const ai_model_t *m, const ai_stream_opts_t *o, ai_effort_t e); /* custom ?? catalog ?? default */

/* ---------- segmented generations (followGoogleContinuations) ---------- */
/* Gemini 4 Hydrogen ends each 65,536-token output segment with finishReason CONTINUATION + a continuationToken;
 * resending the request with the token streams the next segment. */
typedef struct {
    char *token;    /* owned: the token of the segment that just ended, until taken */
    json_t *earlier; /* owned: summed usageMetadata of the finished segments (NULL until a segment continued) */
    json_t *seg;     /* owned: the current segment's latest usageMetadata */
} google_cont_t;
#define GOOGLE_CONTINUATION_MAX_ATTEMPTS 4 /* attempts per resume request */
/* a chunk's candidate and usageMetadata, before the parser reads them: drops a token-carrying CONTINUATION finish
 * (the next segment follows) and replaces the usage with the sum across segments from the second one on */
void google_cont_observe(google_cont_t *c, json_t *candidate, json_t *usage_holder);
/* the segment ended: its token (owned) when it continues, else NULL; folds its usage into the earlier sum */
char *google_cont_take(google_cont_t *c);
void google_cont_free(google_cont_t *c);

/* ---------- SSE state machine ---------- */
typedef struct {
    ai_builder_t b;
    const ai_model_t *model;
    bool saw_finish;
    const char *stop_reason; /* static */
    char *error;             /* owned */
    google_cont_t cont;
} google_sse_t;

void google_sse_init(google_sse_t *s, const ai_model_t *m, ai_event_cb cb, void *ud);
/* ai_sse_cb-compatible. Returns 2 on an in-band error frame. */
int google_sse_event(void *ud, const char *event, const char *data, size_t len);
/* True when the stream finished with STOP but carries no text/tool call (upstream retries these). */
bool google_sse_empty_stop(const google_sse_t *s, int rc);
json_t *google_sse_finish(google_sse_t *s, int rc, const char *err); /* owned final message */
void google_sse_free(google_sse_t *s);

const char *google_map_finish_reason(const char *reason);

/* ---------- Cloud Code Assist (provider_gemini_cli.c) ---------- */

/* Per-conversation Antigravity envelope identity (providerSessionState). */
typedef struct {
    char *agent_id, *trajectory_id, *session_id;
    int64_t step_index;    /* 0 = unset */
    int64_t request_count;
    char *last_execution_id;
    char *last_good_endpoint;
} cca_session_t;

/* Parsed structured credential (`{token, projectId, refreshToken?, expiresAt?, email?, accountId?}`). */
typedef struct {
    char *token, *project_id, *email, *account_id;
    int64_t expires_at; /* ms, 0 unknown */
} cca_creds_t;
/* 0 ok; -1 with *err (owned) carrying the upstream message. */
int cca_parse_credentials(const char *api_key, cca_creds_t *out, char **err);
void cca_creds_free(cca_creds_t *c);

/* CloudCodeAssistRequest envelope. state may be NULL (ephemeral ids). Owned. */
json_t *gemini_cli_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                                 const char *project_id, bool antigravity, cca_session_t *state);

/* Antigravity endpoint order (auto mode): baseUrl override, else daily/sandbox with the last good one first. */
strvec_t gemini_cli_endpoints(const ai_model_t *m, bool antigravity, const cca_session_t *state);

typedef struct {
    ai_builder_t b;
    const ai_model_t *model;
    bool saw_finish;
    const char *stop_reason; /* static */
    char *error;             /* owned: in-band failure */
    char *response_id;       /* owned */
    bool flash_leak;         /* compat.flashStreamLeakWorkaround */
    strvec_t tool_names;
    bool buffering;
    sb_t tbuf;
    char *tbuf_sig;
    bool stripped_leak;
    void *heal; /* think_heal_t */
    google_cont_t cont;
} cca_sse_t;

void cca_sse_init(cca_sse_t *s, const ai_model_t *m, const json_t *tools, ai_event_cb cb, void *ud);
/* ai_sse_cb-compatible; returns 2 on an in-band error / blocked prompt. */
int cca_sse_event(void *ud, const char *event, const char *data, size_t len);
/* Flush held text (planning-leak buffer, markup healer) and close the open block. */
void cca_sse_end(cca_sse_t *s);
bool cca_sse_meaningful(cca_sse_t *s);
bool cca_sse_thought_only(cca_sse_t *s);
void cca_sse_free(cca_sse_t *s);

json_t *provider_gemini_cli_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb,
                                   void *ud);

#endif
