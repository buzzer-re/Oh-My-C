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
    int64_t context_window;     /* effective window (catalog_apply_context_policy) */
    int64_t context_window_std; /* standard window (premium long-context tier capped) */
    int64_t context_window_ext; /* extended window (advertised maximum); == std without tiers */
    bool context_window_pinned; /* a modelOverrides contextWindow fixes the window (no tiers) */
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
/* ModelRegistry.refreshIfStale: re-applies models.yml when it changed on disk since the last load (new models
 * appear, edited ones update in place, removed ones are marked gone); true when the catalog changed */
bool catalog_refresh_if_stale(void);
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
/* re-probe one local provider now (selector-controller.ts: refreshProvider after a login), so its models appear without
 * a restart; other providers untouched. Blocks for the probe: call off the UI thread. No-op for non-local providers. */
int catalog_discover_provider(const char *provider);
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
/* ---------- service tiers (service_tier.c; pi-ai types.ts) ----------
 * Tier names: "auto" "default" "flex" "scale" "priority" "ultrafast" (interned static strings, NULL = omit). */
typedef struct {
    const char *openai, *anthropic, *google; /* per-family selection (ServiceTierByFamily) */
    const char *fireworks; /* request-time only: "priority" with providers.fireworksTier priority */
} ai_service_tiers_t;
const char *ai_service_tier_family(const ai_model_t *m); /* "openai" | "anthropic" | "google" | NULL */
bool ai_service_tier_for_family(const char *family, const char *tier); /* isServiceTierForFamily */
/* shouldSendServiceTier: the tier goes on the wire as the provider's service-tier field (model may be NULL: the
 * bare-provider answer) */
bool ai_should_send_service_tier(const char *tier, const ai_model_t *m);
bool ai_should_send_service_tier_for(const char *tier, const char *provider, const ai_model_t *m);
bool ai_realizes_priority_tier(const char *tier, const ai_model_t *m); /* realizesPriorityServiceTier */
const char *ai_resolve_model_service_tier(const ai_service_tiers_t *tiers, const ai_model_t *m);
const char *ai_service_tier_intern(const char *tier); /* static copy of a known tier name, else NULL */
bool ai_fireworks_fast_model_id(const char *id);

/* ---------- context windows (catalog.c; model-registry.ts #applyContextWindowPolicies) ----------
 * Every catalog model has a standard window (premium long-context tiers capped at their input threshold) and an
 * extended one (the advertised maximum). `extendedContext` picks extended for all; `opt_in` (may be NULL) opts a
 * single model in (a compaction.modelThresholds entry past its standard window). Updates context_window in place. */
void catalog_apply_context_policy(bool extended, bool (*opt_in)(const ai_model_t *m, int64_t standard));
/* the model's standard/extended windows; false when it has a single window */
bool catalog_context_tiers(const ai_model_t *m, int64_t *standard, int64_t *extended);

json_t *model_compat_get(const ai_model_t *m, const char *key); /* override, else api default (borrowed) */
bool model_compat_bool(const ai_model_t *m, const char *key, bool def);
const char *model_compat_str(const ai_model_t *m, const char *key);
bool model_supports_effort(const ai_model_t *m, ai_effort_t e);
ai_effort_t model_clamp_effort(const ai_model_t *m, ai_effort_t e);
/* catalog compat delegationBias for the model: "eager" (default), "gated", "restrained" (static string) */
const char *model_delegation_bias(const ai_model_t *m);
/* catalog `webSearch` grounding of a chat model ("anthropic" | "gemini" | "openai" | "codex" | "xai" | "openrouter")
 * or NULL, and its `webSearchModel` (cheaper same-provider search swap) or NULL; static/borrowed strings */
const char *model_web_search(const ai_model_t *m);
const char *model_web_search_model(const ai_model_t *m);

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
    /* forceReasoningOff (external-thinking scratchpad): implies disable_reasoning; the Responses APIs send
     * reasoning {effort: "none"} whatever the model's disable mode */
    bool force_reasoning_off;
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
    int64_t max_retry_delay_ms;  /* omp maxRetryDelayMs (retry.maxDelayMs): 0 => 60000, < 0 => no cap */
    int max_retries;             /* transport retries (set per provider from its AI_RETRY_* policy) */
    int retry_policy;            /* AI_RETRY_* transport policy (set by the provider) */
    int max_http_attempts;       /* omp maxHttpAttempts (openai-completions): total transport attempts, 0 = default */
    bool accept_empty_response;  /* omp acceptEmptyResponse: no empty-completion retry */
    volatile sig_atomic_t *cancel;
    /* debug: called with the request JSON before sending (may be NULL) */
    void (*on_payload)(void *ud, const json_t *payload);
    void *payload_ud;
    const ai_cursor_exec_t *cursor_exec; /* cursor-agent tool bridge (borrowed, may be NULL) */
    /* Anthropic on-demand compaction request {instructions, filesDueBefore?} (borrowed, may be NULL): the request
     * carries `compaction` + the compaction beta; the result has stopDetails {type:"compaction"} and providerPayload
     * {type:"anthropicCompaction", provider, content, signature, toolChanges?} */
    json_t *anthropic_compaction;
    /* GitHub Copilot identity fallback (set by the transport layer): a denied default chat identity (403, or 400
     * model_not_supported) is retried once as the Copilot CLI and the working identity remembered per key */
    bool copilot_identity_retry;
    const char *copilot_cache_key;      /* borrowed */
    const char *copilot_cache_snapshot; /* cached identity the headers were built from (NULL = none) */
    /* per-family tiers: when set, ai_stream replaces service_tier with the entry for the request's model */
    bool has_service_tiers;
    ai_service_tiers_t service_tiers;
    /* Anthropic fast mode: an endpoint+model that rejected `speed: "fast"` drops it for the session
     * (anthropic-state.ts fastModeDisabled); both may be NULL */
    bool (*fast_mode_disabled)(void *ud, const ai_model_t *m);
    void (*on_fast_mode_unsupported)(void *ud, const ai_model_t *m);
    void *fast_mode_ud;
    /* Anthropic subscription slow mode (anthropic_slow.h; borrowed, may be NULL) */
    const struct anthropic_slow_hooks *slow_hooks;
    /* stream watchdog budgets (providers.streamFirstEventTimeoutSeconds / streamIdleTimeoutSeconds):
     * -1 = provider/env defaults, 0 = disabled */
    int64_t first_event_timeout_ms;
    int64_t idle_timeout_ms;
    /* providers.maxInFlightRequests: per-provider concurrent request caps (borrowed object, may be NULL) */
    const json_t *max_in_flight;
    /* omp's providerSessionState is present: server-side conversation state (Anthropic message threads) may be used */
    bool provider_session_state;
    int stateful_responses; /* the statefulResponses option: 0 unset, 1 true, -1 false */
    int prefer_websockets;  /* the preferWebsockets option: 0 unset, 1 true, -1 false */
    volatile sig_atomic_t *caller_cancel; /* set by the stream watchdog, which replaces cancel with its own flag */
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
/* ai_stream's provider call through the stream watchdog and providers.maxInFlightRequests slots (stream_guard.c);
 * resolves o's first_event/idle budgets in place */
typedef json_t *(*ai_stream_fn)(const ai_model_t *model, const ai_context_t *ctx, const ai_stream_opts_t *opts,
                                ai_event_cb cb, void *ud);
json_t *ai_stream_guarded(ai_stream_fn fn, const ai_model_t *m, const ai_context_t *ctx, ai_stream_opts_t *o, ai_event_cb cb,
                          void *ud);
/* process default for providers.maxInFlightRequests (borrowed; NULL clears) */
void ai_set_max_in_flight(const json_t *limits);
/* Non-streaming convenience: returns the final message. */
json_t *ai_complete(const ai_model_t *model, const ai_context_t *ctx, const ai_stream_opts_t *opts);
/* Concatenate text blocks of an assistant message (owned). */
char *ai_message_text(const json_t *msg);

/* ---------- provider plumbing (used by provider implementations) ---------- */
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

/* ---------- transport retries (pi-utils fetch-retry.ts, anthropic-client.ts, empty-completion-retry.ts) ----------
 * ai_sse_post / ai_stream_post retry a request that failed before any response data arrived, following
 * opts->retry_policy with opts->max_retries retries:
 *   AI_RETRY_FETCH       fetchWithRetry: network errors and 408/429/5xx, 500 ms·2^n (bedrock; 4 retries)
 *   AI_RETRY_OPENAI      fetchWithRetry + LiteLLM concurrency-admission 429s fail at once (postOpenAIStream; 5)
 *   AI_RETRY_OLLAMA      fetchWithRetry with 2 s / 5 s / 10 s, llama.cpp tool-call parse 500s fail at once (4)
 *   AI_RETRY_CODEX       fetchWithRetry with 500 ms·(n+1) (5)
 *   AI_RETRY_GEMINI_CLI  fetchWithRetry with 1 s·2^n
 *   AI_RETRY_ANTHROPIC   the anthropic.ts provider loop: provider-retryable errors, min(0.5 s·2^n, 8 s) with up
 *                        to 25% jitter, a header retry-after wins when longer (10)
 * fetchWithRetry delays use the server hint (headers, then body text) when present; a hint above the cap
 * (opts->max_retry_delay_ms for anthropic / gemini-cli, else 60 s) fails at once. A final HTTP error whose
 * headers carry retry timing gets omp's " retry-after-ms=N" suffix. */
/* networkFailureMessage: omp's sentence for a transport failure (net/http.c wording), the error itself otherwise. Owned. */
char *ai_network_failure_message(const char *http_error, const char *url);
enum { AI_RETRY_FETCH, AI_RETRY_OPENAI, AI_RETRY_OLLAMA, AI_RETRY_CODEX, AI_RETRY_GEMINI_CLI, AI_RETRY_ANTHROPIC };
/* extractRetryHint: server-suggested delay from headers (may be NULL) and/or an error body (may be NULL); -1 none */
int64_t ai_extract_retry_hint(const http_headers_t *h, const char *body);
/* getRetryAfterMsFromHeaders: the longest of retry-after-ms / retry-after / x-ratelimit-reset(-ms); -1 none */
int64_t ai_retry_after_from_headers(const http_headers_t *h);
/* isProviderRetryableError over a finished error message (+ HTTP status, 0 = unknown) */
bool ai_provider_retryable_error(const char *message, int status);
/* getShouldRetryFromHeaders: the server's `x-should-retry` verdict, 1 true, 0 false, -1 absent or unrecognised */
int ai_should_retry_verdict(const http_headers_t *h);
/* account-level usage/quota wording (rate-limit.ts matchesUsageLimitText, approximated); status 0 = unknown */
bool ai_usage_limit_text(const char *msg, int status);
/* withReplaySafeStreamRetry: runs `once` again while no meaningful output was emitted, after a benign empty
 * completion (retry_empty, 2 retries at 500 ms·2^n) or a provider-retryable error (max_error_retries at
 * 500 ms·2^n). Pre-output events of a discarded attempt never reach cb. */
typedef json_t *(*ai_attempt_fn)(void *self, ai_event_cb cb, void *ud); /* one provider attempt */
json_t *ai_replay_safe_stream(ai_attempt_fn once, void *self, const ai_stream_opts_t *o, ai_event_cb cb, void *ud,
                              bool retry_empty, int max_error_retries);
/* retry_empty read after each attempt (a function-valued retryEmptyCompletion) */
json_t *ai_replay_safe_stream_ex(ai_attempt_fn once, void *self, const ai_stream_opts_t *o, ai_event_cb cb, void *ud,
                                 const bool *retry_empty, int max_error_retries);

/* Streaming POST with SSE parsing + retry on transient errors (see the transport retry policies above).
 * on_event returns non-zero to abort. Returns 0 on success (2xx stream consumed);
 * otherwise sets *err (owned) and *status. */
typedef int (*ai_sse_cb)(void *ud, const char *event, const char *data, size_t len);
int ai_sse_post(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                ai_sse_cb on_event, void *ud, int *status, char **err);
/* ai_sse_post with extras. */
typedef struct {
    int64_t idle_timeout_ms; /* max silence between received bytes (0 => 300s) */
    char *err_body;          /* out: raw body of the last failed (non-2xx) response, owned (caller frees) */
    /* response headers of every attempt (success or failure); may be NULL */
    void (*on_headers)(void *ud, int status, const http_headers_t *h);
    /* a failed attempt before any content (status 0: network error; body: error body): >= 0 resends after that
     * many ms (headers may be edited first) without spending a retry, -1 runs the normal retry/error handling,
     * -2 fails now without retrying; may be NULL */
    int64_t (*on_failure)(void *ud, int status, const char *body, http_headers_t *headers);
    void *hook_ud;
} ai_post_ex_t;
int ai_sse_post_ex(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                   ai_sse_cb on_event, void *ud, int *status, char **err, ai_post_ex_t *ex);
/* Raw streaming POST (for newline-delimited JSON etc.). */
int ai_stream_post(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                   http_data_cb on_data, void *ud, int *status, char **err);
int ai_stream_post_ex(const char *url, http_headers_t *headers, const char *body, const ai_stream_opts_t *opts,
                      http_data_cb on_data, void *ud, int *status, char **err, ai_post_ex_t *ex);
/* Test-only: OMC_PROVIDER_URL_REWRITE ("from=to,from=to", prefix match) redirects provider requests (applied by
 * ai_sse_post / ai_stream_post; providers making their own HTTP calls apply it too). Owned. */
char *ai_rewrite_url(const char *url);
/* Extract a human-readable error from a provider error body. Owned. */
char *ai_extract_error(int status, const char *body);
/* inference-headers.ts applyInferenceHeaders: gateway attribution and the conversation id under the protocol's and
 * host's own header (protocol anthropic or openai) */
void ai_apply_inference_headers(http_headers_t *h, const ai_model_t *m, const char *session_id, bool anthropic);
/* text plus the retry-after hint and dump line the transport appended to its error err: for a provider whose own wording
 * wraps the failed response. Owned. */
char *ai_rewrap_http_error(const char *text, const char *err);
/* the reason phrase of an HTTP status, "" when unknown */
const char *ai_http_status_text(int status);
/* stripRawHttpRequestDiagnostics: the error without its trailing local dump lines (hosts relaying it must not leak paths). Owned. */
char *ai_strip_request_dump_lines(const char *msg);
/* Copy model headers + option headers into h. */
void ai_apply_headers(http_headers_t *h, const ai_model_t *m, const ai_stream_opts_t *o);

/* Normalize history for a target model: drops/converts foreign thinking, synthesizes missing tool results,
 * strips images if model lacks image input, drops errored/aborted assistant turns. Returns owned array. */
json_t *ai_transform_messages(const json_t *messages, const ai_model_t *target);
/* OpenAI Responses remote compaction (agent compaction/openai.ts requestOpenAiRemoteCompaction): POST
 * <base>/responses/compact {model, input, instructions, prompt_cache_key?} with `messages` (LLM messages) encoded as
 * Responses input after `previous` (a prior replacementHistory, borrowed, may be NULL). Owned
 * {replacementHistory, compactionItem}, or NULL + *err (owned) and *status (HTTP status, 0 when none). */
json_t *ai_openai_remote_compact(const ai_model_t *m, const char *api_key, const json_t *messages, const json_t *previous,
                                 const char *instructions, const char *session_id, volatile sig_atomic_t *cancel, char **err,
                                 int *status);

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
/* getFactoryDroidCreditRate: the Standard Credits rate at `now` (ms): the `input` of the dated `credit-rates` card due
 * then, else the base rate, and the next dated change to it. False for models without a credit rate. */
typedef struct {
    double rate;
    bool has_next;
    double next_rate;
    int64_t next_at; /* Unix ms */
} ai_credit_rate_t;
bool ai_factory_droid_credit_rate(const ai_model_t *m, int64_t now, ai_credit_rate_t *out);

/* api "gitlab-duo-agent": Duo Workflow Service over WebSocket */
json_t *provider_gitlab_duo_workflow_stream(const ai_model_t *, const ai_context_t *, const ai_stream_opts_t *, ai_event_cb, void *);

#endif
