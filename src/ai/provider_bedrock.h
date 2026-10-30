/*
 * provider_bedrock.h - Amazon Bedrock ConverseStream ("bedrock-converse-stream") internals
 * (port of omp packages/ai/src/providers/amazon-bedrock.ts).
 *
 * The streaming entry point is provider_bedrock_stream (ai.h). Request shaping, region/URL resolution and
 * the event-stream state machine are exposed so they can be exercised offline.
 */
#ifndef OMC_PROVIDER_BEDROCK_H
#define OMC_PROVIDER_BEDROCK_H

#include "ai/ai.h"
#include "ai/aws.h"

typedef struct {
    json_t *body;          /* ConverseStreamRequest (owned) */
    bool sentinel_injected; /* `__no_tools__` placeholder tool was added */
    char *error;           /* validation error (owned); body is NULL when set */
} bedrock_request_t;

/* Build the ConverseStream request body (mapOptionsForApi + streamBedrock shaping). */
void bedrock_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                           bedrock_request_t *out);
void bedrock_request_free(bedrock_request_t *r);

/* Region: ARN-embedded > ambient (when it serves the profile's geo) > guardrail ARN > geo default > us-east-1. Owned. */
char *bedrock_resolve_region(const ai_model_t *m, const char *guardrail_identifier);
/* Full converse-stream URL for `region` (AWS's own regional host is re-pointed; other base URLs kept). Owned. */
char *bedrock_request_url(const ai_model_t *m, const char *region);

/* Map a Converse stopReason to an omp stopReason (static string). */
const char *bedrock_map_stop_reason(const char *reason);
/* HTTP status for an in-stream exception shape name (unknown => 400). */
int bedrock_exception_status(const char *code);
/* isThinkingPrefixBindingError */
bool bedrock_is_prefix_binding_error(const char *msg);

/* ---------- event-stream state machine ---------- */
typedef struct {
    int64_t wire;  /* contentBlockIndex */
    int ci;        /* content array index */
    int kind;      /* 1 text, 2 thinking, 3 toolCall */
    bool closed;
    sb_t sig;      /* accumulated reasoning signature */
} bedrock_block_t;

typedef struct {
    ai_builder_t b;
    const ai_model_t *model;
    aws_es_decoder_t dec;
    bool sentinel_injected;
    const char *stop_reason; /* mapped, static */
    char *error;             /* owned */
    json_t *input_transformations; /* owned or NULL */
    VEC(bedrock_block_t) blocks;
} bedrock_stream_t;

void bedrock_stream_init(bedrock_stream_t *s, const ai_model_t *m, bool sentinel_injected, ai_event_cb cb, void *ud);
/* http_data_cb-compatible: feeds raw event-stream bytes. Returns 2 after an exception/error frame or framing error. */
int bedrock_stream_feed(void *ud, const char *data, size_t len);
/* Finish after the transport returned `rc` with optional transport error. Returns the owned final message. */
json_t *bedrock_stream_finish(bedrock_stream_t *s, int rc, const char *err, bool cancelled);
void bedrock_stream_free(bedrock_stream_t *s);

#endif
