/*
 * provider_anthropic.h - Anthropic Messages API ("anthropic-messages") internals.
 *
 * The streaming entry point is provider_anthropic_stream (ai.h). The pieces below are exposed so the
 * request shaping and the SSE state machine can be exercised offline (tests) without the network.
 */
#ifndef OMC_PROVIDER_ANTHROPIC_H
#define OMC_PROVIDER_ANTHROPIC_H

#include "ai/ai.h"

/* True for Claude Code OAuth tokens (contain "sk-ant-oat"). */
bool anthropic_is_oauth_token(const char *key);

/* provider_anthropic_stream with the OAuth wire forced on (is_oauth 1) / off (0) or detected from the key (-1),
 * like omp's `isOAuth` stream option (GitLab Duo's proxy takes its direct-access token as an OAuth bearer). */
json_t *provider_anthropic_stream_ex(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, int is_oauth,
                                     ai_event_cb cb, void *ud);

/* Build the request params (wire JSON body as an object). Owned. */
json_t *anthropic_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, bool oauth);

/* Request headers for `params` (as built above). `api_key` may be NULL (keyless). */
void anthropic_build_headers(const ai_model_t *m, const ai_stream_opts_t *o, const ai_context_t *ctx,
                             const json_t *params, const char *api_key, bool oauth, http_headers_t *h);

/* `{base}/v1/messages` (OAuth: `?beta=true`). Owned. */
char *anthropic_request_url(const ai_model_t *m, bool oauth);

/* Serialize params compactly; patches the Claude Code billing-header `cch` attestation when present. Owned. */
char *anthropic_serialize_body(const json_t *params);

/* Hosted web search request (omp web/search/providers/anthropic.ts callSearch): fills `h`, sets *url_out (owned)
 * and returns the body (owned). temperature NAN = unset. */
char *anthropic_search_request(const ai_model_t *m, const char *key, const char *query, const char *system_prompt,
                               int64_t max_tokens, double temperature, const char *session_id, http_headers_t *h,
                               char **url_out);

/* Claude Code CLI version used on the OAuth wire (env PI_AI_CLAUDE_CODE_VERSION > adopted > pinned). */
const char *anthropic_claude_code_version(void);

/* ---------- SSE state machine ---------- */
typedef struct {
    int64_t idx; /* wire content block index */
    int kind;    /* 0 ignored, 1 text, 2 thinking, 3 redacted, 4 tool_use */
    int ci;      /* index into the assistant content array, -1 until materialized */
    bool open;
    sb_t sig;    /* accumulated thinking signature */
} anthropic_block_t;

typedef struct {
    ai_builder_t b;
    const ai_model_t *model;
    bool oauth;
    bool saw_start;
    bool saw_terminal; /* message_delta with stop_reason, or message_stop */
    bool saw_stop;     /* message_stop */
    const char *stop_reason; /* mapped, static string */
    char *error;             /* stream error / refusal message (owned) */
    VEC(anthropic_block_t) blocks;
    json_t *compaction;   /* streamed `compaction` block as an anthropicCompaction payload (owned) */
    bool compaction_stop; /* stop_reason "compaction" */
} anthropic_sse_t;

/* supportsAnthropicCompaction: model policy (compat.supportsServerCompaction) and the model's route */
bool anthropic_compaction_supported(const ai_model_t *m);

void anthropic_sse_init(anthropic_sse_t *s, const ai_model_t *m, bool oauth, ai_event_cb cb, void *ud);
/* ai_sse_cb-compatible. Returns 1 after message_stop (stream complete), 2 on an in-stream error frame. */
int anthropic_sse_event(void *ud, const char *event, const char *data, size_t len);
/* Finish after the transport returned `rc` (ai_sse_post result) with optional transport error `err`.
 * Returns the owned final assistant message (stopReason stop/length/toolUse/error/aborted). */
json_t *anthropic_sse_finish(anthropic_sse_t *s, int rc, const char *err);
void anthropic_sse_free(anthropic_sse_t *s);

/* Map an Anthropic stop_reason to an omp stopReason (static string). */
const char *anthropic_map_stop_reason(const char *reason);

/* Normalize a tool parameter schema into an Anthropic input_schema (owned). */
json_t *anthropic_normalize_tool_schema(const json_t *schema);

#endif
