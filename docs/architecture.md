# Architecture

omc is one C11 program (`build/omc`) that can optionally be built as a library (`make lib`, see
[build.md](build.md)). It is a port, not a redesign. Data shapes (messages, tool schemas, session
entries, RPC frames) are kept identical to omp so that sessions, prompts and protocols can be shared
between the two programs, and most source files name the omp file they port in their header comment.

## Layers

```
            cli/  (omc_main: flags, subcommands, mode dispatch)        lib/ (include/omc.h)
                 │                                                      │
            modes/  interactive · print/json · rpc · acp  ◄─────────────┘ (library = rpc endpoint)
                 │                         │
            ui/  (omp packages/tui screens)│
            tui/ (component framework)     │
                 │                         ▼
            coding/  app_t session runtime: model, tools, persistence, retry, compaction,
                     discovery + system prompt, ttsr, goal, advisor, memory, titles, subagents
                 │            │             │                 │
            tools/  mcp/  lsp/  dap/        session/ (JSONL)  config/ (settings, auth, logins)
                 │                                                 │
            agent/  turn loop: stream → tool calls → results → next turn
                 │
            ai/   catalog, API registry, providers, credential transports
                 │
            net/  HTTP/1.1 + TLS + SSE, WebSocket          prompt/ (Handlebars templates)
                 │                                         text/ (regex, glob, walker, search, yaml)
            core/  alloc, strings, vectors, hash map, JSON, fs, processes, embedded files
```

Lower layers never include higher ones. The few upward links are callbacks (agent hooks, tool
environment function pointers, UI callbacks in `tool_ui_t`) so that `agent/` knows nothing about
sessions and `tools/` knows nothing about the TUI.

### Data flow of one prompt

1. A mode calls `app_prompt()` (`src/coding/app.c`). It expands `@file` mentions, applies a `+Nk`
   turn budget directive, adds hidden goal context and starts `agent_prompt()`.
2. `run_loop()` in `src/agent/agent.c` emits `agent_start`/`turn_start`, then calls the
   `transform_context` hook. That hook adds the date/cwd `<system-reminder>`. It then calls
   `prepare_request`, which fills in the API key, cache retention, thinking budgets and service tier.
   Finally it calls `ai_stream()`.
3. `ai_stream()` (`src/ai/ai_common.c`) runs the credential transport layer
   (`provider_transport.c`), looks up the API implementation by `model->api` and streams. Providers
   build the assistant message through `ai_builder_t`, which emits the upstream event sequence
   (`start`, `text_delta`, `toolcall_end`, `done`, ...).
4. Each streaming event goes to the `on_stream_delta` hook (TTSR may abort the stream) and to
   listeners as `message_update`. Modes render these events (TUI), serialize them (json/rpc) or
   ignore them (print).
5. Tool calls run after the message ends: shared tools run in parallel, exclusive tools run alone
   (see [agent.md](agent.md)). Results come back as `toolResult` messages, and the loop continues
   until the model stops, the turn limit is reached or the run is aborted.
6. `app.c`'s listener persists every `message_end` to the session JSONL, tracks cost and context
   size, delivers settled background jobs as steering, and triggers auto-compaction and titles.

## Threads

omc uses plain pthreads. There is no event loop library and no green threads.

| Thread | Owner | Notes |
|---|---|---|
| main / UI | `tui_run()` in interactive mode; stdin reader in rpc/acp | Only the UI thread touches components. Other threads use `tui_post()` / `tui_request_render()` (thread-safe). |
| prompt worker | `modes/interactive.c` (`worker_main`), `modes/rpc.c`, `modes/acp.c` (`prompt_thread`), `omc_prompt()` | Runs the blocking `app_prompt()` / agent loop. One per session at a time (`agent_prompt` returns -1 while running). |
| tool threads | `execute_tools()` in `agent.c` | Up to 8 consecutive non-exclusive tool calls run in parallel, joined before the next step. |
| job threads | `tools/jobs.c` | Async/auto-backgrounded bash, subagent `task` jobs, service watchers. Results are delivered as follow-ups. |
| reader threads | MCP stdio/SSE, LSP, DAP, eval kernel, Cursor reader/writer | Each owns the read side of one pipe or socket. |
| background helpers | title (`title.c`), skill-description compression (`skilldesc.c`), advisor review (`advisor.c`), memory pipeline (`memory.c`), auto-learn capture (`autolearn.c`), eval completions/judge batches (`evalhost.c`), login workers (`ui/login.c`), git status (`ui/statusline.c`) | Each shuts down (cancel + join) in the owner's free function. |
| worker pools | `text/walker.c`, `text/search.c` | Parallel directory walk and search with deterministic ordering. |

Cancellation is cooperative. Long operations poll a `volatile sig_atomic_t *cancel` flag
(`agent->cancel`, `tool_ctx_t.cancel`, `ai_stream_opts_t.cancel`, `http_req_t.cancel`). Events from
parallel tool threads are serialized by `agent->ev_mu`, so listeners see one event at a time.

## Ownership and memory

The full rules are in [CONVENTIONS.md](CONVENTIONS.md). In short:

- All allocation goes through `xmalloc`/`xcalloc`/`xstrdup`/`xasprintf` (`src/core/core.h`), which
  abort on out-of-memory. No caller checks for NULL from allocation.
- A function returning `char *` hands ownership to the caller. One returning `const char *`
  returns borrowed memory. Headers note any exception (for example `json_t` accessors return
  borrowed nodes, and "takes ownership" is spelled out on setters).
- `json_t` (`src/core/json.h`) is the common currency. Messages, tool schemas, events, settings,
  session entries and RPC frames are all ordered JSON trees in the upstream shapes, so unknown
  fields round-trip untouched.
- Containers: `sb_t` string builder, `VEC(T)`, `strvec_t`, `hmap_t` (string keys).
- Long-lived shared objects use explicit refcounts where several sessions share them
  (`artifacts_ref`, task cards) and borrowed pointers otherwise (subagents borrow the parent's MCP
  and LSP managers through `app_opts_t.mcp/lsp`).

## Error handling

- Library-style functions return `0`/`-1` (or `NULL`) and report a message through an owned
  `char **err`. The caller frees it.
- Tools never fail the loop. They report through `tool_result_error()`, which yields a
  `toolResult` with `isError: true` that the model sees. Error strings are copied byte-for-byte
  from omp wherever models are trained on them (edit engines, ssh, read).
- Providers never return NULL. `ai_stream()` always returns an assistant message, with
  `stopReason: "error"` or `"aborted"` and an `errorMessage` on failure. Retry and overflow
  recovery decisions are made above, in `app.c`'s `should_retry` hook (see [ai.md](ai.md)).
- Untrusted input is bounds-checked and depth-capped (JSON 512, YAML 256, markdown, archives, ...).
  See [security.md](security.md).
- Diagnostics go to the log (`LOGD/LOGI/LOGW/LOGE`), never to the TUI's stdout. Logging is off
  unless `OMC_LOG=<file>` is set, and `OMC_DEBUG=1` adds debug lines. `die()` is reserved for
  programming errors and OOM.

## Process-wide vs per-session state

A process can host several sessions: subagents, `omc_session_new()` in the library, ACP sessions.
Some state is shared by all of them:

| Process-wide | Where |
|---|---|
| Settings (defaults < global `config.yml` < project `.omc/`/`.omp/` `config.yml` < `--config` overlays < runtime overrides; `disabledProviders: [native]` from any layer drops the project files except their `config.yml` `modelRoles`) | `config/settings.c` |
| Credentials (`auth.json`, runtime `--api-key`, OAuth refresh) | `config/auth.c` |
| Model catalog + custom `models.yml` providers, API registry | `ai/catalog.c`, `ai/ai_common.c` |
| Token caches: AWS credential chain, Google ADC, GitLab direct-access, Copilot identity | `ai/aws.c`, `ai/google_auth.c`, `ai/provider_gitlab_duo.c`, `ai/provider_transport.c` |
| HTTP keep-alive pool (16 connections), TLS context | `net/http.c` |
| Agent registry (Main + subagents, IRC messages, concurrency permits) | `coding/agents.c` |
| Intent-field tracing flag, theme, glyph preset | `agent/agent.c`, `tui/theme.c`, `tui/symbols.c` |
| Agent dir (`OMC_CODING_AGENT_DIR` / `PI_CODING_AGENT_DIR`, default `~/.omc/agent`) | `core/fs.c` |

Per-session state lives in `app_t` (`src/coding/app.h`): the `agent_t` (history, tools, queues),
the `session_t`, the `tool_env_t` (cwd, hashline snapshot store, todo state, persistent shell,
jobs, artifact store), discovery results, TTSR state, goal, title, advisor, memory runtime, eval
host and the MCP/LSP/DAP managers (shared with subagents). Per-session configuration is
not implemented yet (see [status.md](status.md), Embedding).

## Configuration locations

- Agent dir `~/.omc/agent/` (override `OMC_CODING_AGENT_DIR`, or omp's `PI_CODING_AGENT_DIR`):
  `config.yml`, `models.yml`, `auth.json`, `mcp.json`, `lsp.json`, `AGENTS.md`, `SYSTEM.md`,
  `APPEND_SYSTEM.md`, `skills/`, `rules/`, `agents/`, `themes/`, `prompts/` (overrides for embedded
  prompts), `sessions/`, `managed-skills/`, `memory-state.json`, `skill-descriptions.json`.
- Project dirs: `.omc/` and omp's `.omp/` are both read, plus other tools' context files and MCP
  configs (Claude, Cursor, VS Code, Gemini, Copilot, ...). Details are in [agent.md](agent.md) and
  [tools.md](tools.md).

## Module map

omp is a Bun/TypeScript monorepo (`packages/*`) with Rust natives (`crates/*`). Paths below are
relative to `packages/` (or the repo root for `crates/`) at the upstream commit pinned in
[UPDATE.md](../UPDATE.md). The table maps each omc directory or file to what it ports. "omc-only" marks code with no single upstream counterpart,
usually because Bun, npm packages or Rust crates provide it in omp.

### Runtime base

| omc | omp counterpart |
|---|---|
| `core/core.c` (alloc, `sb_t`, `VEC`, `hmap_t`, UTF-8, width, uuid, base64, log) | Bun/JS runtime; `utils/src` helpers |
| `core/json.c` | JS `JSON` + `utils/src/json-parse.ts` (partial/streaming parse) |
| `core/fs.c`, `core/proc.c` | Node fs, `utils/src/{dirs,procmgr,ptree}.ts`, PTY spawning |
| `core/embed.c` + `build/embedded.c` | files read from the package at runtime |
| `net/http.c` | Bun `fetch` + `utils/src/{fetch-retry,tls-fetch}.ts`; SSE parsing from the providers |
| `net/ws.c` | Bun `WebSocket` (used by `ai/src/providers/gitlab-duo-workflow.ts`) |
| `text/regex.c`, `text/rxbt.c` | Rust `regex` crate (via `crates/pi-natives`); rxbt covers look-around/backrefs |
| `text/walker.c`, `text/glob.c` | `crates/pi-walker`, ignore/globset in `crates/pi-natives` |
| `text/search.c` | `crates/pi-natives` grep |
| `text/yaml.c`, `config/yaml_file.c` | YAML parser used by `utils/src/yaml-config.ts` and frontmatter |
| `text/fuzzy.c` | fzf-style scorer (omc-only; exercised by tests, not used at runtime) |
| `img/img.c`, `png.c`, `jpeg.c`, `gif.c`, `webp.c`, `webp_enc.c`, `vp8*.c` | `Bun.Image` (libpng / libjpeg-turbo / giflib / libwebp inside Bun): from-scratch bounds-checked decoders (PNG all color types + Adam7, JPEG baseline/progressive, GIF first frame, WebP VP8/VP8L/ALPH), encoders (PNG, baseline JPEG, lossy WebP), separable resampling; header sniffing = `utils/src/mime.ts` `parseImageMetadata` |
| `img/imgpipe.c` | `coding-agent/src/utils/image-resize.ts` (`resizeImage`, `formatDimensionNote`), `utils/image-loading.ts` (`loadImageInput`), `tui/src/chat/image-loading.ts` (`imageDecodeFailureReason`, size limits) |
| `prompt/template.c` | `utils/src/template.ts` (Handlebars) + `utils/src/prompt.ts` (format pass) |

### AI layer (`packages/ai`, `packages/catalog`)

| omc | omp counterpart |
|---|---|
| `ai/ai.h`, `ai/ai_common.c` | `ai/src/types.ts`, `stream.ts`, `api-registry.ts`, `usage.ts`, `providers/transform-messages.ts` |
| `ai/catalog.c` + `assets/models.json.z`, `assets/providers.json.z` | `catalog/src/models.json`, `catalog/src/model-manager.ts`; `coding-agent/src/config/{model-registry,custom-models}.ts` |
| `ai/providers.c` | `ai/src/providers/register-builtins.ts` |
| `ai/provider_anthropic.c` | `providers/anthropic.ts`, `anthropic-identity.ts`, `claude-code-fingerprint.ts` |
| `ai/provider_openai_completions.c` | `providers/openai-completions.ts`, `openai-shared.ts` |
| `ai/provider_openai_responses.c` | `providers/openai-responses.ts`, `azure-openai-responses.ts`, `openai-codex-responses.ts` |
| `ai/provider_google.c`, `google_schema.c`, `google_auth.c` | `providers/google.ts`, `google-vertex.ts`, `google-shared.ts`, `google-auth.ts`, `utils/schema/*` |
| `ai/provider_gemini_cli.c`, `google_cca.c` | `providers/google-gemini-cli.ts`, `catalog/src/wire/gemini-headers.ts`, `registry/oauth/google-{gemini-cli,antigravity}.ts` |
| `ai/provider_ollama.c` | `providers/ollama.ts` |
| `ai/provider_bedrock.c`, `aws.c` | `providers/amazon-bedrock.ts`, `aws-sigv4.ts`, `aws-eventstream.ts`, `aws-credentials.ts`, `registry/aws.ts` |
| `ai/provider_cursor.c`, `cursor_proto.c`, `pb.c` | `providers/cursor.ts`, `connect-frame.ts`, `connect-error-detail.ts`, `catalog/src/discovery/cursor-proto.ts`, `coding-agent/src/cursor.ts` (exec handlers) |
| `ai/provider_devin.c` | `providers/devin.ts`, `catalog/src/wire/devin*.ts` |
| `ai/provider_gitlab_duo.c` | `providers/gitlab-duo.ts` |
| `ai/provider_gitlab_duo_workflow.c` | `providers/gitlab-duo-workflow.ts`, `catalog/src/discovery/gitlab-duo-workflow.ts` |
| `ai/provider_transport.c` | `registry/{cloudflare-ai-gateway,muse-code,snowflake}.ts` prepareModel/prepareRequest, provider branches of `openai-shared.ts`, `github-copilot-headers.ts` |
| `ai/think_heal.c` | `ai/src/dialect/{thinking,fenced-thinking}.ts` |
| `ai/judgment_native.c` | `ai/src/judgment/{native,systemone}.ts` |
| `ai/ai_images.c` | `ai/src/providers/anthropic-images.ts`, `coding-agent/src/session/provider-image-budget.ts`, `utils/image-loading.ts` (`normalizeModelContextImages`, STB WebP rewrite), `tui/src/chat/image-loading.ts` (`modelLacksWebpSupport`), catalog `image-count-budget` / `inline-image-byte-budget` / `request-body-byte-limit` rules |
| `config/auth.c`, `auth_engine.c`, `auth_custom.c` | `ai/src/auth-storage.ts`, `registry/engine/*`, `registry/oauth/*`, `registry/hooks/*`, `registry/api-key-validation.ts` |

### Agent and coding agent (`packages/agent`, `packages/coding-agent`)

| omc | omp counterpart |
|---|---|
| `agent/agent.c` | `agent/src/agent-loop.ts`, `agent.ts`; tool examples from `ai/src/dialect/examples.ts` |
| `coding/app.c` | `coding-agent/src/session/agent-session.ts`, `sdk.ts`, `session/date-cwd-reminder.ts`, compaction in `agent/src/compaction` |
| `coding/discovery.c` | `coding-agent/src/discovery/*`, `capability/*`, `system-prompt.ts` |
| `coding/plugins.c` | `discovery/{claude-plugins,agent-plugins}.ts` |
| `coding/ttsr.c` | `session/ttsr-coordinator.ts`, `ttsr-outputs.ts` |
| `coding/goal.c` | `coding-agent/src/goals/*` |
| `coding/advisor.c` | `coding-agent/src/advisor/*` |
| `coding/memory.c`, `memstore.c` | `memory-backend/*`, `memories/*` (SQLite tables kept in a JSON file) |
| `coding/autolearn.c` | `autolearn/*`, `tools/{learn,manage-skill}.ts` |
| `coding/title.c` | `utils/title-card.ts`, `tiny/text.ts`, the session title flow |
| `coding/skilldesc.c` | `extensibility/skill-descriptions.ts` |
| `coding/agents.c` | `registry/agent-registry.ts`, `irc/*`, `task/*` spawn paths |
| `coding/evalhost.c`, `judgment.c` | `eval/*-bridge.ts`, `ai/src/judgment/text.ts` |
| `session/session.c` | `session/session-manager.ts`, `session-title-slot.ts`, `session-context.ts` |
| `session/blobs.c` | `session/blob-store.ts`, `session-persistence.ts` (image externalization), `session-loader.ts` (blob resolution) |
| `config/settings.c` | `coding-agent/src/config/settings.ts` (defaults subset; full table in `assets/tui/settings-ui.json`) |
| `tools/*` | `coding-agent/src/tools/*.ts` (per-tool table in [tools.md](tools.md)) |
| `tools/patch.c`, `sloppy.c`, `fuzzy.c`, `diff.c` | `crates/pi-edit` (replace/patch/apply_patch/sloppy, fuzzy ladder) |
| `tools/outsink.c`, `artifacts.c` | `tui/src/tools/streaming-output.ts`, `output-meta.ts`, `coding-agent/src/tools/output-meta.ts`, `session/artifacts.ts`, `utils/src/sanitize-text.ts` |
| `tools/jobs.c`, `services.c` | `coding-agent/src/async/*` (AsyncJobManager), `launch/*` |
| `tools/xdev.c`, `vurl.c`, `github.c`, `ssh.c` | `tools/xdev.ts`, `internal-urls/*` |
| `tools/shell.c` | `crates/pi-shell` (omp embeds a shell; omc keeps a persistent `bash`, each command a sourced file with its own output pipe, so `exec` redirections, `EXIT` traps and late job output end with the command) |
| `mcp/mcp.c`, `mcp/oauth.c` | `coding-agent/src/mcp/*` |
| `lsp/lsp.c` + `assets/lsp-servers.json` | `coding-agent/src/lsp/*` |
| `dap/dap.c` + `assets/dap-adapters.json` | `coding-agent/src/dap/*`, `tools/debug.ts` |
| `modes/interactive.c` | `modes/interactive-mode.ts` + controllers |
| `modes/print.c`, `rpc.c`, `acp.c` | `modes/print-mode.ts`, `modes/rpc/*`, `modes/acp/*` |
| `cli/cli.c` | `cli.ts`, `main.ts`, `cli-commands.ts` |
| `lib/omc.c`, `include/omc.h` | omc-only (closest analog: `sdk.ts`) |

### Terminal UI (`packages/tui`)

| omc | omp counterpart |
|---|---|
| `tui/render.c` | `tui/src/tui.ts` (component tree, differential renderer, overlays) |
| `tui/term.c`, `keys.c` | `terminal.ts`, `keys.ts`, `stdin-buffer.ts`, `keybindings.ts` |
| `tui/editor.c`, `composer.c` | `editor-component.ts`, `components/composer/*`, `autocomplete.ts` |
| `tui/components.c` | `components/{text,truncated-text,box,spacer,loader,select-list}.ts` |
| `tui/markdown.c`, `highlight.c` | `components/markdown.ts` + `utils/src/marked*`; `crates/pi-natives` highlight.rs |
| `tui/theme.c`, `color.c`, `symbols.c` | `theme/*`, `utils/src/color.ts`, `theme/session-color.ts`, `theme/symbols.ts` |
| `tui/fuzzy.c`, `ansi.c` | `fuzzy.ts`, width/slice helpers in `utils.ts` |
| `ui/chat.c` | `chat/*`, `chrome/{transcript-container,error-block,...}.ts` |
| `ui/toolcards.c`, `ui/tools/tc_*.c` | `chat/tool-execution.ts`, `chat/read-tool-group.ts`, `tui/src/tools/*`, `render/*` |
| `ui/statusline.c` | `status-line/*`, `prompt/composer-hints.ts` |
| `ui/welcome.c` | `prompt/welcome.ts` |
| `ui/setup.c`, `login.c`, `modelbrowser.c`, `modelhub.c`, `sessions.c`, `settings.c`, `ask.c`, `overlays.c`, `appearance.c`, `widgets.c`, `slash.c` | `setup/*`, `overlays/{oauth-selector,login-dialog,model-browser,model-hub,hub-frame,session-selector,settings-selector,ask-dialog,thinking-selector,report-panel,composer-shape-preview}.ts`, `theme/theme.ts`, `slash-commands/builtin-*.ts` metadata |

The prompts under `prompts/` mirror omp's source paths (`prompts/coding-agent/prompts/system/...` is
`packages/coding-agent/src/prompts/system/...`), and `prompts/crates/pi-edit` comes from the Rust
edit crate.
