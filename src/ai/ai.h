/*
 * ai.h - multi-provider LLM streaming layer (port of packages/ai + packages/catalog).
 *
 * Messages, content blocks, tools and usage are represented as json_t trees that follow the
 * upstream omp wire shapes exactly (role/content/toolCall/... ), so sessions are interchangeable
 * and unknown fields round-trip untouched.
 *
 *   UserMessage       {role:"user", content: string | [Text|Image], timestamp}
 *   DeveloperMessage  {role:"developer", content, timestamp}
 *   AssistantMessage  {role:"assistant", content:[Text|Thinking|RedactedThinking|ToolCall],
 *                      api, provider, model, usage, stopReason, errorMessage?, responseId?, timestamp}
 *   ToolResultMessage {role:"toolResult", toolCallId, toolName, content:[Text|Image], isError, details?, timestamp}
 *   Text      {type:"text", text, textSignature?}
 *   Thinking  {type:"thinking", thinking, thinkingSignature?}
 *   Redacted  {type:"redactedThinking", data}
 *   Image     {type:"image", data(base64), mimeType}
 *   ToolCall  {type:"toolCall", id, name, arguments:{}, thoughtSignature?}
 *   Usage     {input, output, cacheRead, cacheWrite, totalTokens, cost:{input,output,cacheRead,cacheWrite,total}}
 */
#ifndef OMC_AI_H
#define OMC_AI_H

#include "core/core.h"
#include "core/json.h"
#include "net/http.h"

/* ---------- effort / thinking ---------- */
typedef enum {
    EFFORT_OFF = 0,
    EFFORT_MINIMAL,
    EFFORT_LOW,
    EFFORT_MEDIUM,
    EFFORT_HIGH,
    EFFORT_XHIGH,
    EFFORT_MAX,
} ai_effort_t;

const char *effort_name(ai_effort_t e);      /* "off","minimal",... */
int effort_parse(const char *s, ai_effort_t *out); /* 0 ok */

/* ---------- models ---------- */
typedef struct ai_model {
    json_t *raw;        /* full model object (compat merged with api defaults); owned by catalog */
    const char *id;
    const char *name;
    const char *api;      /* "anthropic-messages", "openai-completions", "openai-responses", ... */
    const char *provider;
    const char *base_url;
    bool reasoning;
    bool input_image;
    double cost_input, cost_output, cost_cache_read, cost_cache_write; /* $ per 1M tokens */
    int64_t context_window;
    int64_t max_tokens;
    json_t *compat;     /* per-model compat overrides, borrowed from raw (may be NULL); use model_compat_get */
    json_t *compat_defaults; /* per-api defaults (borrowed, may be NULL) */
    json_t *headers;    /* borrowed */
    json_t *thinking;   /* borrowed: {mode, efforts[], defaultLevel?} */
} ai_model_t;

/* catalog: bundled models + ~/.omc/agent/models.json (custom providers/models) */
void catalog_init(void);
size_t catalog_count(void);
const ai_model_t *catalog_at(size_t i);
const ai_model_t *catalog_find(const char *provider, const char *id); /* provider may be NULL */
/* resolve user pattern "provider/id", "id", fuzzy substring; optional ":effort" suffix handled by caller */
const ai_model_t *catalog_resolve(const char *pattern);
const char *catalog_provider_default_model(const char *provider);
json_t *catalog_provider_info(const char *provider);   /* borrowed: {id, envVars, defaultModel, ...} */
json_t *catalog_auth_providers(void);                  /* borrowed array of OAuth login descriptors */
const ai_model_t *catalog_register(json_t *model_obj); /* takes ownership; for custom/discovered models */
/* omp's built-in role selector chains (behavior.kdl role-defaults): borrowed {role: [selector, ...]} */
json_t *catalog_role_defaults(void);
/* provider usable without credentials (omp #keylessProviders: local, web, ollama, llama.cpp, lm-studio, apple,
 * plus discovered local servers); its provider info carries "keyless": true */
bool catalog_provider_keyless(const char *provider);
/* model usable now: its provider has credentials (auth.json / env / models.yml apiKey) or is keyless */
bool catalog_model_available(const ai_model_t *m);
/* omp modelKind(): "chat" (default), "tiny", "image", "tts", "stt", "search", "judge", "embedding", "rerank",
 * "video" (borrowed string) */
const char *catalog_model_kind(const ai_model_t *m);
/* implicit local servers (ollama, llama.cpp, lm-studio; localdisc.c): probe and register their models (keyless,
 * raw "discovered": true; models a later probe no longer lists get raw "gone": true and stop being available).
 * force=false is the once-per-process startup refresh (24 h cache in <agent>/local-models.json, llama.cpp always
 * probed); force=true (hub F5) probes every server. Blocks for at most the probe timeouts (250 ms + 150 ms per
 * metadata call on loopback). Call from the UI/main thread. Returns the number of models registered. */
int catalog_discover_local(bool force);
/* last discovery outcome for an implicit local provider: owned {status: "ok"|"empty"|"cached"|"unavailable",
 * optional, error?, fetchedAt?} or NULL when never probed (omp ProviderDiscoveryState; the hub hides an
 * optional provider that is unavailable) */
json_t *catalog_local_discovery_state(const char *provider);

/* measured model speed (port of omp agent-storage model_perf; <agent>/model-perf.json):
 * key is "provider/id" (optionally "@tier"); sums decay by half once 256 samples accumulate */
void model_perf_record(const char *key, int64_t output_tokens, int64_t duration_ms, int64_t ttft_ms);
/* fold a finished assistant message (provider, model, usage.output, duration, ttft); errors are skipped */
void model_perf_record_message(const json_t *assistant);
/* recency-weighted averages: false when the model has no samples; *ttft_ms < 0 when no sample reported one */
bool model_perf_get(const char *key, double *tps, double *ttft_ms, double *samples);
/* omp model-browser #perfCell: "1.2s 118t/s" (full), "118t/s" (tps); measured perf first, else the catalog's
 * estimated raw "tps" as "~118t/s"; "" when unknown. owned */
char *model_perf_cell(const ai_model_t *m, bool full);
json_t *model_compat_get(const ai_model_t *m, const char *key); /* override, else api default (borrowed) */
bool model_compat_bool(const ai_model_t *m, const char *key, bool def);
const char *model_compat_str(const ai_model_t *m, const char *key);
bool model_supports_effort(const ai_model_t *m, ai_effort_t e);
ai_effort_t model_clamp_effort(const ai_model_t *m, ai_effort_t e);
/* catalog compat delegationBias for the model: "eager" (default), "gated", "restrained" (static string) */
const char *model_delegation_bias(const ai_model_t *m);

/* per-api compat defaults (borrowed, may be NULL) */
json_t *catalog_api_defaults(const char *api);

/* keyless sentinel: no Authorization header is sent (omp NO_AUTH_SENTINEL) */
#define AI_NO_AUTH_SENTINEL "N/A"
/* credential helpers shared with the login flows */
char *ai_snowflake_account_url(const char *input, char **err); /* owned https://<host> or NULL + *err */
bool ai_cloudflare_credential(const char *value, char **token, char **account, char **gateway);
void ai_copilot_parse_key(const char *raw, char **token, char **enterprise, char **api_endpoint);
char *ai_copilot_cache_get(const char *key);           /* owned or NULL */
void ai_copilot_cache_set(const char *key, const char *id); /* id NULL clears */

/* api key from environment for provider (catalog envVars + legacy) */
char *ai_env_api_key(const char *provider);
const char *ai_env_api_key_name(const char *provider);

/* ---------- cursor-agent exec bridge ----------
 * Cursor's server drives the agent loop and asks the client to run its tools mid-stream (exec frames).
 * The host runs them as local tools (emitting its own tool events) and keeps the results, which belong
 * after the assistant message in the transcript. Every call the provider resolves this way is paired
 * with exactly one `result` before the stream returns. */
typedef struct {
    /* Run local tool `name` with `args` for call `id`; returns an owned ToolResultMessage, or NULL when
     * no such tool is available to the agent. Called on the streaming thread. */
    json_t *(*run)(void *ud, const char *id, const char *name, const json_t *args);
    /* A ToolResultMessage (owned) pairing a resolved toolCall block of the streamed message. */
    void (*result)(void *ud, json_t *tool_result);
    void *ud;
} ai_cursor_exec_t;

/* ---------- stream options & context ---------- */
typedef struct {
    const char *api_key;
    int64_t max_tokens;          /* 0 => model default policy */
    double temperature;          /* NAN => unset */
    double top_p;                /* NAN => unset */
    ai_effort_t reasoning;       /* EFFORT_OFF => no thinking */
    bool disable_reasoning;
    bool hide_thinking_summary;
    const char *session_id;      /* prompt-cache key / affinity */
    const char *prompt_cache_key; /* prompt_cache_key when it differs from session_id (side requests); NULL = session_id */
    const char *cache_retention; /* "none" | "short" | "long" (NULL => short) */
    json_t *headers;             /* object of extra headers (borrowed) */
    json_t *tool_choice;         /* "auto" | "none" | "required" | {type:"tool",name} (borrowed) */
    const char *service_tier;
    const char *text_verbosity;
    json_t *thinking_budgets;    /* {minimal:..,low:..} (borrowed) */
    json_t *metadata;            /* (borrowed) */
    json_t *stop_sequences;      /* array (borrowed) */
    int64_t max_retry_delay_ms;  /* 0 => 60000 */
    int max_retries;             /* transient retries inside provider (default 3) */
    volatile sig_atomic_t *cancel;
    /* debug: called with the request JSON before sending (may be NULL) */
    void (*on_payload)(void *ud, const json_t *payload);
    void *payload_ud;
    const ai_cursor_exec_t *cursor_exec; /* cursor-agent tool bridge (borrowed, may be NULL) */
    /* GitHub Copilot identity fallback (set by the transport layer): a denied default chat identity (403, or 400
     * model_not_supported) is retried once as the Copilot CLI and the working identity remembered per key */
    bool copilot_identity_retry;
    const char *copilot_cache_key;      /* borrowed */
    const char *copilot_cache_snapshot; /* cached identity the headers were built from (NULL = none) */
} ai_stream_opts_t;

void ai_stream_opts_init(ai_stream_opts_t *o);

typedef struct {
    json_t *system_prompt; /* array of strings (borrowed, may be NULL) */
    json_t *messages;      /* array of messages (borrowed) */
    json_t *tools;         /* array of {name, description, parameters} (borrowed, may be NULL) */
} ai_context_t;

/* ---------- events ---------- */
typedef enum {
    AI_EV_START,
    AI_EV_TEXT_START,
    AI_EV_TEXT_DELTA,
    AI_EV_TEXT_END,
    AI_EV_THINKING_START,
    AI_EV_THINKING_DELTA,
    AI_EV_THINKING_END,
    AI_EV_TOOLCALL_START,
    AI_EV_TOOLCALL_DELTA,
    AI_EV_TOOLCALL_END,
    AI_EV_DONE,
    AI_EV_ERROR,
} ai_event_type_t;

const char *ai_event_name(ai_event_type_t t); /* "start", "text_delta", ... (upstream names) */

typedef struct {
    ai_event_type_t type;
    int content_index;      /* -1 for start/done/error */
    const char *delta;      /* for *_DELTA */
    size_t delta_len;
    const json_t *block;    /* for *_END: the finished content block */
    const json_t *partial;  /* assistant message so far (borrowed; valid during callback) */
    const char *reason;     /* for DONE/ERROR: stopReason */
} ai_event_t;

typedef void (*ai_event_cb)(void *ud, const ai_event_t *ev);

/* Stream a completion. Always returns an owned AssistantMessage (stopReason "error"/"aborted" on failure). */
json_t *ai_stream(const ai_model_t *model, const ai_context_t *ctx, const ai_stream_opts_t *opts, ai_event_cb cb,
                  void *ud);
/* Non-streaming convenience: returns the final message. */
json_t *ai_complete(const ai_model_t *model, const ai_context_t *ctx, const ai_stream_opts_t *opts);
/* Concatenate text blocks of an assistant message (owned). */
char *ai_message_text(const json_t *msg);

/* ---------- provider plumbing (used by provider implementations) ---------- */
typedef json_t *(*ai_stream_fn)(const ai_model_t *model, const ai_context_t *ctx, const ai_stream_opts_t *opts,
                                ai_event_cb cb, void *ud);
void ai_register_api(const char *api, ai_stream_fn fn);
ai_stream_fn ai_lookup_api(const char *api);

/* Assistant message builder: maintains the partial message and emits events consistently. */
typedef struct {
    json_t *msg;          /* the partial/final assistant message */
    ai_event_cb cb;
    void *ud;
    const ai_model_t *model;
    int cur_index;        /* index of the open block or -1 */
    int cur_kind;         /* 0 none, 1 text, 2 thinking, 3 toolcall */
    sb_t buf;             /* accumulates current text/thinking/toolcall-args */
    int64_t t_start;
    int64_t t_first_token;
    bool started;
} ai_builder_t;

void aib_init(ai_builder_t *b, const ai_model_t *m, ai_event_cb cb, void *ud);
void aib_start(ai_builder_t *b);                           /* emits "start" once */
void aib_text_delta(ai_builder_t *b, const char *s, size_t n);   /* opens a text block if needed */
void aib_thinking_delta(ai_builder_t *b, const char *s, size_t n);
void aib_set_thinking_signature(ai_builder_t *b, const char *sig); /* on current or last thinking block */
void aib_set_text_signature(ai_builder_t *b, const char *sig);
void aib_redacted_thinking(ai_builder_t *b, const char *data);
/* Starts a tool call block; closes any open block first. */
void aib_toolcall_start(ai_builder_t *b, const char *id, const char *name);
void aib_toolcall_delta(ai_builder_t *b, const char *s, size_t n); /* argument JSON fragment */
void aib_toolcall_set_args(ai_builder_t *b, json_t *args);           /* full args (takes ownership) */
void aib_toolcall_set_field(ai_builder_t *b, const char *key, const char *val); /* e.g. thoughtSignature */
void aib_end_block(ai_builder_t *b);                       /* closes the open block, emits *_end */
void aib_usage(ai_builder_t *b, int64_t input, int64_t output, int64_t cache_read, int64_t cache_write);
void aib_usage_add_reasoning(ai_builder_t *b, int64_t reasoning_tokens);
void aib_set(ai_builder_t *b, const char *key, json_t *val); /* top-level field (responseId, ...) */
/* Finishes the message: closes blocks, computes cost, sets stopReason, emits done/error. Returns owned msg. */
json_t *aib_finish(ai_builder_t *b, const char *stop_reason, const char *error_message);
void aib_free(ai_builder_t *b);
int aib_block_count(const ai_builder_t *b);

json_t *ai_usage_new(void);
void ai_usage_compute_cost(const ai_model_t *m, json_t *usage);
json_t *ai_error_message(const ai_model_t *m, const char *stop_reason, const char *err); /* owned */

/* Streaming POST with SSE parsing + retry on transient errors (429/5xx/overloaded/network).
 * on_event returns non-zero to abort. Returns 0 on success (2xx stream consumed);
 * otherwise sets *err (owned) and *status. */
typedef int (*ai_sse_cb)(void *ud, const char *event, const char *data, size_t len);
int ai_sse_post(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                ai_sse_cb on_event, void *ud, int *status, char **err);
/* ai_sse_post with extras. */
typedef struct {
    int64_t idle_timeout_ms; /* max silence between received bytes (0 => 300s) */
    char *err_body;          /* out: raw body of the last failed (non-2xx) response, owned (caller frees) */
} ai_post_ex_t;
int ai_sse_post_ex(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                   ai_sse_cb on_event, void *ud, int *status, char **err, ai_post_ex_t *ex);
/* Raw streaming POST (for newline-delimited JSON etc.). */
int ai_stream_post(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                   http_data_cb on_data, void *ud, int *status, char **err);
/* Test-only: OMC_PROVIDER_URL_REWRITE ("from=to,from=to", prefix match) redirects provider requests (applied by
 * ai_sse_post / ai_stream_post; providers making their own HTTP calls apply it too). Owned. */
char *ai_rewrite_url(const char *url);
/* Extract a human-readable error from a provider error body. Owned. */
char *ai_extract_error(int status, const char *body);
/* Copy model headers + option headers into h. */
void ai_apply_headers(http_headers_t *h, const ai_model_t *m, const ai_stream_opts_t *o);

/* Normalize history for a target model: drops/converts foreign thinking, synthesizes missing tool results,
 * strips images if model lacks image input, drops errored/aborted assistant turns. Returns owned array. */
json_t *ai_transform_messages(const json_t *messages, const ai_model_t *target);

/* Default reasoning token budget for effort levels (token-budget providers). */
int64_t ai_thinking_budget(const ai_model_t *m, ai_effort_t e, const json_t *custom_budgets);

/* register all built-in providers */
void ai_providers_init(void);

/* individual provider entry points */
json_t *provider_anthropic_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);
json_t *provider_openai_completions_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);
json_t *provider_openai_responses_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);
json_t *provider_google_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);
json_t *provider_ollama_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);
json_t *provider_bedrock_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);
json_t *provider_gemini_cli_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);
json_t *provider_devin_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);
json_t *provider_cursor_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);
/* provider "gitlab-duo" (any of its chat APIs): direct-access token + GitLab Cloud Connector proxy */
json_t *provider_gitlab_duo_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);
/* api "gitlab-duo-agent": Duo Workflow Service over WebSocket */
json_t *provider_gitlab_duo_workflow_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);

#endif
