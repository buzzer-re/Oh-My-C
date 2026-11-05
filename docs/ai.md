# AI provider layer

`src/ai/` ports omp's `packages/ai` and `packages/catalog`. It turns a model plus a context
(system prompt parts, messages, tools) into a streamed assistant message, whatever the wire
protocol. The public surface is `src/ai/ai.h`. For which providers and APIs are complete, see
[status.md](status.md) (Providers).

## Data model

Messages, content blocks, tools and usage are `json_t` trees in omp's exact shapes (listed at the
top of `ai.h`): `user`, `developer`, `assistant` (`text`, `thinking`, `redactedThinking`, `toolCall`
blocks plus `api/provider/model/usage/stopReason/errorMessage`), and `toolResult` (`toolCallId`,
`content`, `isError`, `details`). Because the shapes match, omp and omc session files are
interchangeable and unknown fields survive a round trip.

`ai_transform_messages()` normalizes a history for the target model before every request. It
drops or converts foreign thinking blocks, synthesizes missing tool results, strips images for
models without image input, and drops errored or aborted assistant turns
(`providers/transform-messages.ts`).

## Model catalog (`catalog.c`)

- The bundled catalog is `assets/models.json.z` (5,948 models, 79 providers, from omp's
  `catalog/src/models.json`). Each model row carries `api`, `baseUrl`, costs per 1M tokens,
  `contextWindow`, `maxTokens`, `reasoning`, `input` modalities, `thinking` (mode, efforts,
  default level), headers and `compat` flags. Compat values shared by most models of an API are
  stored once per API (`defaults`), and `model_compat_get()` returns the row override or else the
  API default.
- Provider metadata and login descriptors come from `assets/providers.json.z`: env var names,
  default models, base URLs, and the auth registry used by `omc login`.
- Custom providers and models come from `<agent dir>/models.yml` (or `models.yaml` / `models.json`)
  in omp's format. `apiKey` is an env var name or a literal. `catalog_register()` adds discovered
  models at runtime.
- Custom and discovered models have no bundled policy row, so `compat_detect.c` builds one the way omp does.
  `compat_finalize_custom()` (finalizeCustomModel, models.yml) first fills what the entry leaves out from its bundled
  reference (`compat_ref_lookup`: identity/reference.ts, tried through the bracket, namespace, `:cloud`, marker and
  case variants of the id; the largest limits win; `charm-hyper` / `factory-droid` rows are excluded because omp only
  discovers them online), else omp's defaults (free, text, not reasoning, 128000 / 16384). `compat_resolve_model()`
  (buildModel, also used for discovered models) then computes the identity (taxonomy with overrides and variant
  collapse), resolves the rule cascade (`assets/compat_rules.json.z`: per axis, the matching rule with the greatest
  exactness, dimensions and priority wins), builds the API's compat record (loopback and private-network URLs count as a
  local backend: `replayReasoningContent`, `clampOutputToModelMax`, `grammar` tool schemas, a 300 s idle timeout, no
  first-event timeout; Qwen ids use the `qwen` dialect, so requests carry `enable_thinking`, on local backends
  `preserve_thinking`, and never `store`), applies wire axes and then `compat` (provider under model; known keys only),
  resolves the thinking ladder, and applies catalog corrections. Explicit prices, limits, kind and `promptCache` win
  afterwards. A resolved record is complete, so the per-API catalog defaults do not fill its unset keys; a custom
  model's context window has no inferred tiers. Legacy `thinking.minLevel`/`maxLevel`/`levels` become `efforts`. An
  `anthropic-messages` custom model without a provider `auth` is `isOAuth`: the cloaked Claude Code request shape.
- `catalog_resolve(pattern)` accepts `provider/id`, a bare id or a fuzzy substring.
- Context-window tiers (omp `model-registry.ts #applyContextWindowPolicies`): every row has a standard
  window (a premium long-context price tier, `cost.longContext.inputThreshold`, caps it unless
  `longContextWindowUncapped`) and an extended one (the advertised `maxContextWindow`).
  `catalog_apply_context_policy()` sets `context_window` to the extended window when `extendedContext`
  is on, or for one model whose `compaction.modelThresholds` entry reaches past its standard window
  (`cp_reaches`); `modelctl.c` re-applies it on every settings change. Authoritative windows and
  models.yml `modelOverrides` with a `contextWindow` keep one window. Matches omp for all 688 rows of
  openai / anthropic / amazon-bedrock / google with both settings.
- Service tiers (`service_tier.c`, omp `pi-ai/types.ts`): `ai_service_tier_family()` (openai /
  anthropic / google, OpenRouter by identity class, Claude on any `anthropic-messages` host, OpenAI-id
  relays), `ai_should_send_service_tier()` (the wire gate per provider: OpenAI and Codex (advertised
  `serviceTiers`; `ultrafast` also reaches an OpenAI-family relay row that lists it), OpenRouter flex/scale/priority, Google flex/priority, Vertex/Fireworks priority,
  never `auto`) and `ai_realizes_priority_tier()` (Anthropic direct → `speed: "fast"` + the fast-mode
  beta). A request carries the session's per-family map (`opts.service_tiers`); `ai_stream()` picks
  the entry for the request's model (Fireworks: `providers.fireworksTier`). The google-generative-ai
  budget-thinking path does not forward `serviceTier`, like omp.

### Model selection and roles

`app_resolve_model_pattern()` (`src/coding/app.c`) resolves the user-facing pattern:

1. An optional leading `@`, then a role name looks up `modelRoles.<role>` in settings (`@smol`,
   `@default`, ...). This is recursive.
2. A trailing `:effort` (`opus:high`) sets the thinking level.
3. The rest goes to `catalog_resolve()`.

Roles read by omc: `default` (session model), `smol`, `tiny`, `commit`, `judge`, `advisor`,
`task`, `compaction`. Helpers walk role chains, for example the eval judge uses
`judge → tiny → smol → default → session model`, skill-description compression uses
`smol → default → tiny`, and title fallback uses `tiny/commit/smol`. Without `modelRoles.default`,
`app_default_model()` picks the first credentialed provider in a fixed order (anthropic, openai,
openai-codex, google, openrouter, ...) and uses its default model.

Thinking levels are `off, minimal, low, medium, high, xhigh, max` (`ai_effort_t`).
`model_clamp_effort()` maps a request onto what the model supports, and `ai_thinking_budget()` gives
token budgets for budget-style APIs (overridable with the `thinkingBudgets` setting).

## API registry and dispatch

`ai_providers_init()` (`providers.c`) registers one stream function per wire API
(`ai_register_api`). `ai_stream()` then:

1. runs `ai_transport_prepare()` (`provider_transport.c`), the provider-owned shaping step that
   turns structured credentials into the bearer, base URL and headers (see below);
2. looks up the function by `model->api` and calls it through `ai_stream_guarded()`
   (`stream_guard.c`, below);
3. lets the transport post-process the result (`ai_transport_finish`).

`ai_stream_guarded()` wraps every provider call:

- **Stream watchdog** (omp `register-builtins.ts forwardStream` + `idle-iterator.ts`): no semantic
  event (anything but `start`) within the first-event budget, or silence longer than the idle budget
  afterwards, cancels the request locally and ends it as an error with the provider's message
  (`Anthropic stream timed out while waiting for the first event`, `OpenAI completions stream stalled
  while waiting for the next event`, `Provider stream …`). Budgets: `opts.first_event_timeout_ms` /
  `idle_timeout_ms` (settings `providers.streamFirstEventTimeoutSeconds` / `streamIdleTimeoutSeconds`,
  -1 auto, 0 off), else `PI_STREAM_FIRST_EVENT_TIMEOUT_MS` / `PI_STREAM_IDLE_TIMEOUT_MS` (OpenAI-family
  APIs and Ollama prefer `PI_OPENAI_STREAM_*`), else the model's compat `streamIdleTimeoutMs` /
  `streamFirstEventTimeoutMs`, else 300 s; the first-event budget never undercuts the idle one.
  Cloud Code Assist keeps its own first-event watchdog (it fails over between endpoints); Cursor and
  GitLab Duo have none. The byte-level HTTP read timeout is widened so it never fires first.
- **In-flight caps** (`providers.maxInFlightRequests`, `{provider: n}`): a request holds one of `n`
  slot files under `<config root>/run/provider-inflight/<sha256(provider)>/` with `flock`, so the cap
  is shared by every local omc process with that config root; a lock dies with its process (omp uses
  heartbeat lease directories for the same contract).

`ai_complete()` is the non-streaming wrapper used for one-shot side requests: titles, compaction
summaries, judges, the advisor and memory extraction.

### Stream events

Providers never emit events directly. They drive an `ai_builder_t` (`aib_text_delta`,
`aib_thinking_delta`, `aib_toolcall_start/delta`, `aib_usage`, `aib_finish`), which keeps the
partial message and emits omp's event sequence:

`start` → (`text_start` `text_delta`* `text_end` | `thinking_*` | `toolcall_start` `toolcall_delta`*
`toolcall_end`)* → `done` | `error`

Each event carries the content index, the delta, the finished block (on `*_end`) and the partial
message. `aib_finish()` computes cost from usage and the catalog prices and sets `stopReason`
(`stop`, `length`, `toolUse`, `error`, `aborted`). `ai_stream()` never returns NULL: failures come
back as an assistant message with `stopReason: "error"` and `errorMessage`.

## Providers

| API (`model.api`) | omc file | omp file(s) | Wire |
|---|---|---|---|
| `anthropic-messages` | `provider_anthropic.c` | `providers/anthropic.ts`, `anthropic-identity.ts`, `claude-code-fingerprint.ts` | SSE; API key or Claude OAuth identity (Claude Code headers/fingerprint) |
| `openai-completions`, `openrouter` | `provider_openai_completions.c` | `openai-completions.ts`, `openai-shared.ts` | SSE; per-model compat flags, reasoning formats |
| `openai-responses`, `azure-openai-responses`, `openai-codex-responses` | `provider_openai_responses.c` | `openai-responses.ts`, `azure-openai-responses.ts`, `openai-codex-responses.ts` | SSE; Azure api-version and deployment map, Codex ChatGPT backend (JWT account header, `store:false`) |
| `google-generative-ai`, `google-vertex` | `provider_google.c` (+ `google_schema.c`, `google_auth.c`) | `google.ts`, `google-vertex.ts`, `google-shared.ts`, `google-auth.ts` | SSE `streamGenerateContent`; Vertex via API key or Application Default Credentials |
| `google-gemini-cli` (Gemini CLI, Antigravity) | `provider_gemini_cli.c` (+ `google_cca.c`, `think_heal.c`) | `google-gemini-cli.ts`, `catalog/src/wire/gemini-headers.ts` | Cloud Code Assist `v1internal:streamGenerateContent`, endpoint failover |
| `ollama-chat` | `provider_ollama.c` | `ollama.ts` | NDJSON `POST /api/chat` |
| `bedrock-converse-stream` | `provider_bedrock.c` (+ `aws.c`) | `amazon-bedrock.ts`, `aws-*.ts` | AWS event-stream; SigV4 or bearer key |
| `cursor-agent` | `provider_cursor.c` (+ `cursor_proto.c`, `pb.c`) | `cursor.ts`, `connect-frame.ts`, `cursor-proto.ts` | Connect + protobuf over HTTP/1.1 RunSSE + BidiAppend |
| `devin-agent` | `provider_devin.c` | `devin.ts`, `catalog/src/wire/devin*.ts` | Connect + protobuf server stream |
| provider `gitlab-duo` (on anthropic/completions/responses APIs) | `provider_gitlab_duo.c` | `gitlab-duo.ts` | direct-access token exchange, then GitLab Cloud Connector proxy through the matching provider above |
| `gitlab-duo-agent` | `provider_gitlab_duo_workflow.c` | `gitlab-duo-workflow.ts` | Duo Workflow Service over WebSocket |
| `typesafe`, `openrouter-decisions`, `workers-ai-systemone` (judgment only) | `judgment_native.c` | `ai/src/judgment/{native,systemone}.ts` | typed-question APIs used by the eval judge chain, not by chat |

The catalog also lists APIs omc does not serve (`local-inference`, images, video, embeddings,
rerank, transcription, speech, `web-search`, `openai-decisions`). Selecting one yields an
"Unsupported API" error message.

Points worth knowing per provider:

- **Agent-style providers** (Cursor, Devin, GitLab Duo agent) run the agent loop on the server.
  Cursor asks the client to run tools mid-stream through exec frames. `ai_stream_opts_t.cursor_exec`
  is a bridge into the agent's local tools (`agent.c` `cursor_bridge_run`), and the resolved tool
  results are inserted after the assistant message. GitLab Duo agent turns end with a `runMCPTool`
  action (a normal tool call answered on the same socket), and `pause_turn` stops are re-sampled by
  the loop up to 8 times.
- **Server-side compaction** (driven by `coding/compaction.c`): `ai_openai_remote_compact()` posts the
  Responses `/responses/compact` request (`<base>/v1/responses/compact`, or `remoteCompaction.endpoint`)
  with the history encoded as Responses input; a user message whose `providerPayload` is
  `{type: "openaiResponsesHistory", provider, items}` replays those items (ids and output statuses
  stripped) for the same provider. `ai_stream_opts_t.anthropic_compaction` turns an Anthropic request
  into an on-demand compaction (`compaction: {type: "summarize", instructions}`, no `context_management`,
  no stop sequences, the `compact-2026-09-04` beta); the streamed `compaction` block comes back as
  `stopDetails {type: "compaction"}` + `providerPayload {type: "anthropicCompaction", …}`, and a summary
  message carrying that payload replays as a `compaction` block opening the following assistant turn
  (its `filesText` queued as a user turn before the first newer message). `anthropic_compaction_supported()`
  is omp's `supportsAnthropicCompaction` for the model's own route. Bedrock Converse compaction (an
  InvokeModel side request) and the Codex V2 streaming compaction are not implemented.
- **Transcript dialects** (`dialect.c`): each dialect's `renderTranscript` and
  `serializeConversationForSummary`, used for compaction and branch-summary input (not for in-band tool
  calls).
- **Leaked reasoning markup** (`<think>`, `<thinking>`, `<scratchpad>`, Gemini thinking fences,
  Gemma/Harmony channels) is re-routed into thinking blocks by `think_heal.c` for the Cloud Code
  Assist wire.
- **Schema normalization** for Google wires (`google_schema.c`) strips unsupported JSON Schema
  keywords and rewrites legacy `parameters` for Claude-on-CCA, porting
  `ai/src/utils/schema/*`.

## Credential transports (`provider_transport.c`)

Some login flows store structured credentials rather than a bare key. Before dispatch, the
transport layer turns them into the request shape. It ports the registry `prepareModel` /
`prepareRequest` hooks and the provider branches of `openai-shared.ts`:

- Cloudflare AI Gateway (`{token, accountId, gatewayId}` key or env vars; `anthropic/`, `openai/`,
  `workers-ai/` routes; `cf-aig-authorization` header)
- Muse Code key bundles, Snowflake account URLs (structured OAuth or `SNOWFLAKE_PAT`), QwenCloud and
  Xiaomi Token Plan keys (regional base URLs), Kilo device tokens
- GitHub Copilot: plan/enterprise endpoint from the stored key, Copilot CLI identity headers,
  per-turn `X-Initiator`, vision header, and a one-shot identity retry on 403 or
  `model_not_supported` with a per-credential cache

`AI_NO_AUTH_SENTINEL` (`"N/A"`) marks keyless providers: no Authorization header is sent.

## Credentials and login (`src/config/auth*.c`)

**Lookup order** (`auth_get_api_key(provider)`):

1. runtime override (`--api-key`, `omc_options_t.api_key`)
2. `<agent dir>/auth.json` (mode 0600, atomic rewrite):
   - `{type:"api_key", key}`, where `key` may be a literal, an env var name, or `!command` whose
     stdout is the key;
   - `{type:"oauth", access, refresh, expires, descriptor, ...}`, refreshed 60 s before expiry
     using the stored descriptor's refresh rule. Structured descriptors (`apiKeyFormat:
     structured`) yield a JSON key for the transport layer.
3. environment variables named by the catalog's `envVars` for the provider (`ai_env_api_key`).

**Login engines** port omp's declarative registry (`ai/src/registry/engine/*`, `registry/oauth/*`,
`registry/hooks/*`):

| Engine | File | Covers |
|---|---|---|
| `oauth-code` | `auth.c` + `auth_engine.c` | authorize URL with upstream param order/encoding, PKCE, state, local callback server with port fallback, paste-code, after-exchange hooks (Google project provisioning, Codex profile, Anthropic identity, Z.ai key minting) |
| `device-code` | `auth_engine.c` | RFC 8628 with interval / `slow_down` / expiry / denial, templated endpoints, OIDC discovery (xAI), JWT-claim fields |
| `api-key` | `auth_engine.c` | prompt, normalization, optional validation probes (chat completions / anthropic messages / models endpoint) |
| custom hooks | `auth_custom.c` | GitHub Copilot, Cursor, Codex device, Alibaba coding/token plan, Xiaomi, Perplexity, Cloudflare AI Gateway / Workers AI, Snowflake, Kilo, Mistral Vibe browser sign-in (mints a plain API key) |

Entry points: `omc login [provider]`, `/login` in the TUI, the setup wizard, and RPC
`get_login_providers`. MCP servers reuse the same oauth-code machinery (`src/mcp/oauth.c`, see
[tools.md](tools.md)). Environment-only credential chains live next to their providers:
`aws.c` (env, web identity, profiles with SSO / `credential_process` / `role_arn`, ECS, IMDSv2) and
`google_auth.c` (ADC: service account RS256 JWT, authorized user, workforce `external_account_authorized_user` at its
STS `token_url` with Basic client auth and in-memory rotated refresh tokens, impersonation, gcloud file,
metadata server). Both have process-wide caches.

## Retries and fallback

There are two levels:

1. **Transport** (`ai_sse_post_ex`, `ai_stream_post`, policies in `ai.h` `AI_RETRY_*`, helpers in `ai/retry.c`): a
   request that fails *before any response data arrived* is retried per provider like omp:
   - OpenAI chat completions / Responses / Azure (`postOpenAIStream` → pi-utils `fetchWithRetry`): 6 attempts at
     500 ms·2ⁿ for network errors and 408/429/5xx; a LiteLLM concurrency-admission 429 (`rate_limit_type:
     max_parallel_requests`) fails at once. Chat completions then retries one replay-safe provider error after
     500 ms (another 6 attempts) unless `max_http_attempts` (omp `maxHttpAttempts`; memory requests use 3) is set;
     Azure Responses also gets that one retry. So a plain 503 costs 12 requests / ~31.5 s per turn attempt.
   - Anthropic (anthropic.ts provider loop, client retries pinned to 0): 10 retries of provider-retryable errors
     (`isProviderRetryableError`: 408/429/5xx and transient wording, never usage limits or other 4xx) after
     min(0.5 s·2ⁿ, 8 s) with up to 25% jitter; a header retry-after wins when longer, one above `retry.maxDelayMs`
     fails at once (11 requests / ~48 s for a plain 503).
   - Codex SSE: 6 attempts at 500 ms·(n+1), 5 min cap. Ollama: 5 attempts at 2 s / 5 s / 10 s, llama.cpp tool-call
     parse 500s fail at once. Bedrock: fetchWithRetry defaults (5 attempts). Gemini CLI: 4 attempts at 1 s·2ⁿ on the
     last endpoint. Google generative AI / Vertex: no transport retries (one request).
   - fetchWithRetry delays use the server hint (`extractRetryHint`: retry-after-ms, Retry-After, x-ratelimit-reset*,
     then body text such as "Please retry in 250ms", `"retryDelay"`, "try again in 5 min", "reset after 1h2m3s");
     a hint above the cap returns the response unretried. A final HTTP error whose headers carry retry timing gets
     omp's ` retry-after-ms=N` suffix (read by the turn level).
   - Replay-safe stream retries (`ai_replay_safe_stream`, omp `withReplaySafeStreamRetry`): pre-output events of a
     discarded attempt never reach the agent; benign empty completions are retried twice (500 ms, 1 s) for OpenAI
     chat completions / Responses, Anthropic and Ollama (`accept_empty_response` turns it off).
   Once data has streamed, nothing is retried here. The e2e mock answers retryable `fail:` statuses with
   `retry-after-ms: 0` (`MOCK_FAIL_RETRY_AFTER_MS`) so these retries are immediate in the suite.
2. **Turn** (`hook_should_retry` in `app.c`, predicates in `coding/turn_recovery.c`): an assistant message that
   ended in error is classified with omp's `AIError.classifyMessage` / `retriable` (flags.ts patterns verbatim;
   usage-limit wording and the rate-limit reason stay the transport's approximations; omc's own connection wording
   from `net/http.c` counts as transient, and a premature stream close stands in for the `incomplete-stream` errorId
   omp's providers attach). Turn recovery (turn-recovery.ts) then decides:
   - `isRetryableError`: never replays a turn whose text was already shown (committed), or one with images, server
     tools or tool calls, unless every call is paired with a synthetic `executed: false` result and the error is
     retriable; replayed turns are dropped from the context. Text is not committed for subagents and print-mode text
     output (`app->text_buffered`, omp `setTextOutputCommitted(false)`), so their partial text replays.
   - A tool turn interrupted by a mid-stream transport failure (`classifyResolvedInterruptedToolTurn`: idle stall,
     HTTP/2 reset, premature close, truncated envelope) and any retriable failure whose calls never ran keep the
     failed turn and its synthetic results (`AGENT_RETRY_PRESERVE`): the next request shows the model its calls did
     not run.
   - `handleCommittedTextStreamStall`: a socket close, Codex steer rejection or mid-stream transport failure after
     text was shown keeps the partial turn, appends the hidden `stream-stall-continue.md` developer message (attempt
     n/3; `Agent.appendMessage`, so no message event and not persisted) and continues at once; at most 3 per prompt
     (`STREAM_STALL_CONTINUE_MAX_RETRIES`), then the error stays. `retry.enabled: false` turns it off.
   - A drop before any output is retried by the transport (item 1).
   Reasonless aborts, malformed-function-call and empty/unexpected-stop continuations, credential rotation and
   the Fireworks/usage-limit paths of omp's recovery are not part of this.
   Beyond that: context overflow (by message text, or a provider `errorFlags: ["contextOverflow"]`)
   runs the compaction ladder with a retry of the turn (see [agent.md](agent.md#compaction-srccodingcompactionc));
   a pass that cannot make progress ends the turn instead of retrying. Otherwise retryable errors are retried up to
   `retry.maxRetries` (10). The wait is omp's turn-recovery rule: `retry.baseDelayMs` (500) doubled per attempt,
   capped at 8 s, with up to 25% downward jitter (`mr_retry_backoff_ms`, fractional ms like omp's `delayMs`). A
   reason window raises it only when the error carries no parsed retry-after and is not a usage limit
   (`mr_reason_window_ms`): 5 s for concurrency caps, 30 s for rate limits, and 45–75 s for model capacity only
   with a 429/402 status or `429` / `MODEL_CAPACITY_EXHAUSTED` / `MODEL_CAPACITY_EXCEEDED` / resource-exhausted
   wording (a plain 503 or 529 keeps the exponential base). A provider retry-after wins when longer; a wait above
   `retry.maxDelayMs` (5 min) fails fast. `retry.enabled: false` turns this off. The transport retries of item 1 run under every turn attempt, as in
   omp.
   A usage-limit wait runs on a wall-clock deadline taken before `auto_retry_start`, so system sleep counts toward it;
   ordinary backoff stays monotonic.
3. **Model fallback** (`try_retry_fallback` in `app.c`, chains in `config/model_resolver.c`): `retry.fallbackChains`
   is walked on retryable and hard errors (see [status.md](status.md), "Retry model fallback"). A prompt whose model
   has no usable credential (`auth_has_usable_credential`: none stored or in the environment, or an OAuth token still
   expired after its refresh) walks the same chain before it is sent (`credential_preflight` in `app.c`, omp
   `applyMissingCredentialFallback`), unpinned so the primary returns with its credential. With no candidate, or
   `retry.modelFallback: false`, the prompt is refused with `No API key found for <provider>` (`APP_PROMPT_REFUSED`).

A connect failure on the OpenAI, Codex and Anthropic transports surfaces as omp's sentence
(`ai_network_failure_message`, omp `networkFailureMessage`): `Couldn't connect to <host> (connection refused)...`,
`Couldn't reach <host> (getaddrinfo ENOTFOUND)...`, `No internet connection: couldn't reach <host> (...)...`, `TLS
certificate verification failed for <host> (...)...`. In `-p` text mode each automatic retry prints
`Retrying (n/m): <error>` to stderr, after one `Working...` line.

Before the transport retry decision, a provider hook (`ai_post_ex_t.on_failure`) may resend the
request itself or fail it at once. Anthropic uses it for:

- **Fast-mode fallback** (omp `anthropic-state.ts`): a 400 rejecting `speed` or a 429 fast-mode
  entitlement error before any content drops `speed` + the fast-mode beta, resends once, and remembers
  the endpoint+model for the session (the `/fast` badge then goes away); `/fast on` re-arms it.
- **Subscription slow mode** (`anthropic_slow.c`, omp `anthropic-slow-mode.ts` both halves): only for
  first-party OAuth requests to `api.anthropic.com`. Per account lane (process-wide), response headers
  track the wrap-up allowance (`anthropic-ratelimit-unified-grace-*`) and the slow-lane offer; with
  `providers.anthropic.slowMode: auto` (`/slow on`) a session-limit 429 offering `treatment` enters the
  lane and requests carry `anthropic-usage-limit: slow` until the limit resets; `slot_busy` / 529 become
  jittered server-paced waits within the max-wait budget; weekly/budget/off/ineligible/wall end it with
  omp's notices. The status line shows the lane label (`low priority until …`, `limit reached ·
  wrapping up · resets …`) and the model is steered once per wrap-up window
  (`anthropic-usage-wrap-up.md`). omp's sibling-account health gate before auto-accept is not ported
  (omc does not rotate Claude accounts).

## Transports (`src/net/`)

| Transport | Implementation | Used by |
|---|---|---|
| HTTP/1.1 client | `net/http.c`: TLS via OpenSSL with hostname/IP verification, keep-alive pool (16), chunked and gzip decoding, a peer close (FIN before the last chunk / Content-Length, or RST) mid-body fails at once with Bun's `ECONNRESET: The socket connection was closed unexpectedly. …` (a silent connection is the stream watchdog's; `SO_KEEPALIVE` with the kernel's timers, as omp sets no TCP keepalive itself), `HTTP(S)_PROXY`/`NO_PROXY` (CONNECT for TLS), redirects, connect/idle/total timeouts, cancellation, body caps (64 MB buffered by default) | everything |
| SSE | `sse_parser_t` in `net/http.c`; `ai_sse_post` adds retries | Anthropic, OpenAI, Google, Copilot, ... |
| NDJSON / raw stream | `ai_stream_post` | Ollama |
| WebSocket (RFC 6455) | `net/ws.c`: masked client frames, fragmentation, ping/pong, close echo, 16 MiB message cap, wss via CONNECT proxy | GitLab Duo agent |
| Protobuf + Connect | `ai/pb.c` (schema-driven proto3 codec over `json_t`, unknown fields kept), `cursor_proto.c` (schema DSL from omp's `cursor-proto.ts`, same field order so bytes match), Connect envelopes and end-stream/error details | Cursor, Devin |
| AWS event-stream | `ai/aws.c` (prelude/message CRC32 via zlib) | Bedrock |

Extra CA bundles: `OMC_EXTRA_CA_CERTS`, `NODE_EXTRA_CA_CERTS` or `SSL_CERT_FILE`.
`OMC_INSECURE_TLS=1` disables verification (testing only).

Test hooks: `OMC_PROVIDER_URL_REWRITE="from=to,..."` redirects provider URLs and
`OMC_AUTH_URL_REWRITE` redirects login endpoints, so the e2e suite can point real-provider code
paths at local mocks (see [testing.md](testing.md)).

## Adding a provider

1. Write `src/ai/provider_<name>.c` with an `ai_stream_fn`, using `ai_builder_t` for events and
   `ai_sse_post` / `ai_stream_post` for HTTP. Expose request building and stream decoding in a
   header so they can be unit-tested offline.
2. Register the API in `ai_providers_init()`.
3. If logins store structured keys, add the shaping to `provider_transport.c`.
4. Add `tests/test_providers_*.c` cases (recorded streams) and, where a login or URL shape matters,
   a mock under `tests/e2e/`.
5. Update [status.md](status.md) and this file.

## Images at the provider boundary

Every request runs omp's image policy before the provider converts messages (`agent.c` →
`ai_prepare_context_images`, `ai/ai_images.c`):

1. **Count budget** (`clampProviderContextImages`): images in user/developer/tool-result messages past the
   host's `image-count-budget` are dropped oldest first (Anthropic 90, 600 on the official API / 100 for
   Claude < 4.6; OpenAI 200; Codex 200 / 2,000 official; Google 200; Antigravity 60, 20 for Claude; Bedrock 90;
   Cursor 60 / 20; OpenRouter 90; xAI 60; unknown hosts 5; a model's own `imageCountBudget` wins). A message
   left without content becomes `[image omitted: provider image limit]`.
2. **STB WebP rewrite** for llama.cpp-backed servers (Ollama, LM Studio, `imageInputDecoder: stb`): WebP history
   is re-encoded, or replaced by `[image omitted: WebP could not be decoded for this model]`.
3. **Undecodable images** (full decode, verdicts cached by payload hash) become
   `[image omitted: undecodable <mime> data (<reason>)]` so one bad payload cannot poison every later request.
4. **Byte budget** (`clampProviderContextImageBytes`) on budgeted hosts (official Anthropic 120 MB inline within
   the 128 MB body limit, keeping the newest user / tool-result images; Codex 24 MB; DeepSeek 40 MiB).

The Anthropic provider then applies Claude's per-image contract (`prepareAnthropicImageContext`): any image over
8000 px per side (2000 px when a request carries more than 20 images) or over 90% of the 10 MiB base64 cap is
re-encoded (PNG or JPEG q85, the smaller) up to four times, shrinking by the overshoot; compat
`maxImageDimension` / `maxImagePayloadBytes` override the limits and Claude on Antigravity uses 5 MiB. Images
entering the context are already normalized (read tool, composer attachments, RPC/ACP prompt images:
`ai_normalize_images` = `normalizeModelContextImages`): fit within 1568 px, at least 200 px on the short edge,
smallest of PNG / JPEG / WebP under 500 KiB with omp's quality and scale ladders (`img/imgpipe.c`).
