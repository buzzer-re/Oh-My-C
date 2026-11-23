/*
 * provider_openai.h - internals of the OpenAI-family providers (Chat Completions, Responses incl.
 * Azure/Codex variants, Ollama native chat). Exposed so request building and stream decoding can be
 * tested offline; normal callers only use the provider_*_stream entry points in ai.h.
 */
#ifndef OMC_PROVIDER_OPENAI_H
#define OMC_PROVIDER_OPENAI_H

#include "ai/ai.h"

/* return code used by stream callbacks to stop reading once the response is complete */
#define OAI_STREAM_STOP 7001

/* ---------- shared helpers (provider_openai_completions.c) ---------- */

/* compat lookups with an optional override object (compat.whenThinking) consulted first */
bool oai_cbool(const ai_model_t *m, const json_t *over, const char *key, bool def);
const char *oai_cstr(const ai_model_t *m, const json_t *over, const char *key, const char *def);
json_t *oai_cget(const ai_model_t *m, const json_t *over, const char *key);

/* Resolved reasoning policy (port of resolveOpenAICompatPolicy). */
typedef struct {
    bool model_supported;
    bool supports_params;
    bool enabled;
    bool disabled;
    bool omit_effort;
    const char *requested;    /* requested effort name (static) or NULL */
    char wire[32];            /* wire effort ("" when unset) */
    const char *disable_reason; /* "caller","forced-tool-choice","tool-choice","tools","not-requested" or NULL */
    const char *disable_mode;
    const char *dialect;      /* thinkingFormat */
    const json_t *over;       /* compat.whenThinking when active (borrowed) */
} oai_policy_t;

void oai_policy_resolve(oai_policy_t *p, const ai_model_t *m, const ai_stream_opts_t *o, const json_t *wire_tool_choice,
                        bool has_tools);
bool oai_tool_choice_forced(const json_t *tc);       /* "required" or named */
const char *oai_tool_choice_name(const json_t *tc);  /* named tool or NULL */
const char *oai_tool_choice_mode(const json_t *tc);  /* "auto"/"none"/"required" or NULL */
/* Adapt a JSON schema for OpenAI strict mode. Returns an owned clone, or NULL when not representable. */
json_t *oai_strict_schema(const json_t *schema);
char *oai_hash36(const char *s);                     /* owned base36 hash */
char *oai_prompt_cache_key(const ai_stream_opts_t *o, size_t max_len, const char *prefix); /* owned or NULL */
bool oai_send_service_tier(const ai_model_t *m, const char *tier);
char *oai_image_url(const json_t *img);              /* owned data: URI (or url field) */
json_t *oai_system_prompts(const ai_context_t *ctx); /* owned array of non-empty strings */
/* output-token cap: 0 => omit */
int64_t oai_output_tokens(const ai_model_t *m, const ai_stream_opts_t *o, bool clamp_to_model);
const char *oai_effort_wire(const ai_model_t *m, const json_t *over, const char *effort);
char *oai_bearer_key(const ai_model_t *m, const ai_stream_opts_t *o); /* owned or NULL */
char *oai_base_url(const ai_model_t *m, const char *def);           /* owned, without trailing '/' */
void oai_add_openrouter_headers(http_headers_t *h);
void oai_apply_inference_headers(http_headers_t *h, const ai_model_t *m, const ai_stream_opts_t *o);
void oai_apply_prompt_cache_session_header(http_headers_t *h, const ai_model_t *m, const ai_stream_opts_t *o);
/* emit an event for a block the builder does not currently hold open */
void oai_emit(ai_builder_t *b, ai_event_type_t t, int idx, const char *delta, size_t n, const json_t *blk);
/* OpenAI-family usage accounting (calculateOpenAIUsageAccounting); cw_* < 0 => not reported */
void oai_usage_accounting(ai_builder_t *b, int64_t prompt, int64_t output, int64_t cached, int64_t reasoning,
                          int64_t cw_openrouter, int64_t cw_deepseek, bool has_deepseek_hit_miss);
/* parse tool-call argument JSON (complete, then partial, else {}); owned object */
json_t *oai_parse_args(const char *s, size_t n);
bool oai_is_reasoning_field(const char *s); /* reasoning_content / reasoning / reasoning_text */

/* ---------- Chat Completions ---------- */

json_t *oaic_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o);
json_t *oaic_convert_messages(const ai_model_t *m, const ai_context_t *ctx, const json_t *compat_over);

typedef struct oaic_stream oaic_stream_t;
oaic_stream_t *oaic_stream_new(const ai_model_t *m, ai_event_cb cb, void *ud);
/* ai_sse_cb-compatible: feed one SSE event. Returns OAI_STREAM_STOP when the response is complete. */
int oaic_stream_on_sse(void *ud, const char *event, const char *data, size_t len);
/* Finalize: transport_rc/err are ai_sse_post's result (0/NULL when the stream was consumed). Frees s. */
json_t *oaic_stream_finish(oaic_stream_t *s, int transport_rc, const char *transport_err);

/* ---------- Responses (openai-responses / azure-openai-responses / openai-codex-responses) ---------- */

json_t *oair_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o);
json_t *oair_convert_input(const ai_model_t *m, const ai_context_t *ctx, int variant);
/* moveCodexInstructionsToInput: `instructions` -> a leading model.base_instructions developer item (id from
 * prompt_cache_key), in place */
void oair_codex_instructions_to_input(json_t *body);
/* isOpenAIResponsesRefusedEncryptedReasoningError over an error message */
bool oair_refused_encrypted_reasoning(const char *msg);
/* an assistant turn retried without replayed encrypted reasoning (omp providerPayload.encryptedReasoningFloor) */
bool oair_encrypted_reasoning_floor(const json_t *msg);
char *oair_codex_account_id(const char *access_token); /* owned or NULL */
char *oair_url(const ai_model_t *m);                   /* owned request URL */

typedef struct oair_stream oair_stream_t;
oair_stream_t *oair_stream_new(const ai_model_t *m, ai_event_cb cb, void *ud);
/* toolNamesByWire: wire name -> declared name when compat.toolNamePrefix renames tools, NULL otherwise; a wire-name
 * collision fails the request (*err owned). Owned. */
json_t *oair_tool_names_by_wire(const ai_model_t *m, const json_t *tools, char **err);
void oair_stream_set_wire_names(oair_stream_t *s, const json_t *names); /* borrowed, outlives the stream */
int oair_stream_on_sse(void *ud, const char *event, const char *data, size_t len);
json_t *oair_stream_finish(oair_stream_t *s, int transport_rc, const char *transport_err);

/* ---------- Ollama native chat ---------- */

json_t *ollama_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o);
typedef struct ollama_stream ollama_stream_t;
ollama_stream_t *ollama_stream_new(const ai_model_t *m, ai_event_cb cb, void *ud);
/* http_data_cb-compatible: raw NDJSON bytes (any chunking). */
int ollama_stream_on_data(void *ud, const char *data, size_t len);
json_t *ollama_stream_finish(ollama_stream_t *s, int transport_rc, const char *transport_err);

#endif
