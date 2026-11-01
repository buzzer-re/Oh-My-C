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
- `catalog_resolve(pattern)` accepts `provider/id`, a bare id or a fuzzy substring.

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
2. looks up the function by `model->api` and calls it;
3. lets the transport post-process the result (`ai_transport_finish`).

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
| custom hooks | `auth_custom.c` | GitHub Copilot, Cursor, Codex device, Alibaba coding/token plan, Xiaomi, Perplexity, Cloudflare AI Gateway / Workers AI, Snowflake, Kilo |

Entry points: `omc login [provider]`, `/login` in the TUI, the setup wizard, and RPC
`get_login_providers`. MCP servers reuse the same oauth-code machinery (`src/mcp/oauth.c`, see
[tools.md](tools.md)). Environment-only credential chains live next to their providers:
`aws.c` (env, web identity, profiles with SSO / `credential_process` / `role_arn`, ECS, IMDSv2) and
`google_auth.c` (ADC: service account RS256 JWT, authorized user, impersonation, gcloud file,
metadata server). Both have process-wide caches.

## Retries and fallback

There are two levels:

1. **Transport** (`ai_sse_post_ex`, `ai_stream_post`): a request that fails *before any response
   data arrived* with a network error, a retryable status (408, 409, 425, 429, 500, 502–504, 520,
   522, 524, 529) or a 400 "overloaded" body is retried up to
   `opts.max_retries` (default 3), with backoff `1s << attempt` or the server's `retry-after-ms` /
   `Retry-After` (seconds or HTTP date). A server delay above `max_retry_delay_ms` (default 60 s)
   fails immediately with the delay in the message. Once data has streamed, nothing is retried
   here.
2. **Turn** (`hook_should_retry` in `app.c`): an assistant message that ended in error is
   classified. Context overflow (by message text, or a provider `errorFlags: ["contextOverflow"]`)
   triggers one compaction and a retry. Otherwise retryable errors are retried up to
   `retry.maxRetries` (10) with exponential backoff (`retry.baseDelayMs` 2000, capped at
   `retry.maxDelayMs` 60000), showing a countdown notice. `retry.enabled: false` turns this off.

omp's retry-fallback model chains (switch to another model after failures) are not implemented. See
[status.md](status.md).

## Transports (`src/net/`)

| Transport | Implementation | Used by |
|---|---|---|
| HTTP/1.1 client | `net/http.c`: TLS via OpenSSL with hostname/IP verification, keep-alive pool (16), chunked and gzip decoding, `HTTP(S)_PROXY`/`NO_PROXY` (CONNECT for TLS), redirects, connect/idle/total timeouts, cancellation, body caps (64 MB buffered by default) | everything |
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
