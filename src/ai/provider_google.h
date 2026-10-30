/*
 * provider_google.h - Gemini generateContent providers ("google-generative-ai", "google-vertex") internals.
 *
 * provider_google_stream (ai.h) serves both apis: it dispatches on model->api. Register it for
 * "google-generative-ai" and "google-vertex". ("google-gemini-cli" / Cloud Code Assist is a different
 * request envelope and is not handled here.)
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

/* ---------- SSE state machine ---------- */
typedef struct {
    ai_builder_t b;
    const ai_model_t *model;
    bool saw_finish;
    const char *stop_reason; /* static */
    char *error;             /* owned */
} google_sse_t;

void google_sse_init(google_sse_t *s, const ai_model_t *m, ai_event_cb cb, void *ud);
/* ai_sse_cb-compatible. Returns 2 on an in-band error frame. */
int google_sse_event(void *ud, const char *event, const char *data, size_t len);
/* True when the stream finished with STOP but carries no text/tool call (upstream retries these). */
bool google_sse_empty_stop(const google_sse_t *s, int rc);
json_t *google_sse_finish(google_sse_t *s, int rc, const char *err); /* owned final message */
void google_sse_free(google_sse_t *s);

const char *google_map_finish_reason(const char *reason);

#endif
