# Tools

Tools are `agent_tool_t` objects (`src/agent/agent.h`): name, label, description, JSON Schema
`parameters`, an `execute` function, and flags (`exclusive`, `hidden`, `discoverable`, `mounted`,
`intent_mode`, `examples`). The built-ins live in `src/tools/` and share a per-session `tool_env_t`
(`src/tools/tools.h`): cwd, hashline snapshot store, todo state, persistent shell, jobs, artifact
store, discovery results, and callbacks into the session (subagents, completions, eval bridge,
plan proposals, UI `ask`). Per-tool completeness is tracked in [status.md](status.md) (Tools).

## Registration and wire parity

- `tools_register_builtin()` (`tools_common.c`) constructs the enabled built-ins. Gating follows
  omp's `resolveBuiltinToolPlan()`: `bash.enabled`, `todo.enabled`, `glob.enabled`,
  `grep.enabled`, `astGrep.enabled` (off), `astEdit.enabled`, `web_search.enabled`, `ask.enabled`
  (needs a UI), `find.enabled` (only `true`/`on`; omp's `auto` needs a native judge backend),
  `wait` (needs jobs), and `task` (depth below `task.maxRecursionDepth`). `--tools a,b` restricts
  the set, and `--no-tools` removes it.
- `install_agent()` (`coding/app.c`) adds the session-level tools: `lsp`, `debug`,
  `checkpoint`/`rewind`, `goal`, `learn`/`manage_skill`, MCP tools, and host tools.
- `tools_finalize()` sorts tools into omp's canonical `BUILTIN_TOOLS` order and mounts
  discoverable tools under `xd://`. The model therefore sees the same tool list, order and
  descriptions as omp.
- Descriptions are omp's markdown (`prompts/coding-agent/prompts/tools/<name>.md`, rendered by
  `tool_prompt()`, `omp://` → `omc://`). Schemas are omp's exact wire schemas written as JSON
  literals (`schema_lit`), including `additionalProperties`. The `i` intent field is injected
  separately ([agent.md](agent.md#the-intent-field-i)). `tests/parity/request_diff.sh` diffs the
  whole request against omp.

## Tool table

| Tool | omc source | omp counterpart | Notes |
|---|---|---|---|
| `read` | `tools/read.c`, `readfmt.c`, `summary.c`, `web.c` | `tools/read.ts`, `read-archive.ts`, `sqlite-reader.ts`, `read-pdf.ts`, `read-summary.ts`, `read-selector.ts` | Line selectors (`:50-80`, `:-20`, `:raw`, `:conflicts`), hashline headers `[path#TAG]`, structural summaries of whole code files, directories, images, URLs (HTML → markdown), zip/jar/whl/tar/tar.gz/gz members, SQLite tables and rows via an on-disk-format parser, PDF text, `.ipynb` cells, and every internal URL below |
| `write` | `tools/write_bash.c` | `tools/write.ts` | Exclusive. Also the "write" half of `xd://` devices, `agent://` messages, `proc://` control, and staged `ssh://` writes |
| `edit` | `tools/edit.c`, `patch.c`, `sloppy.c`, `fuzzy.c`, `diff.c` | `coding-agent/src/edit/*`, `crates/pi-edit` | Exclusive. `edit.mode` (`PI_EDIT_VARIANT` pins it): `hashline` (default: PUT/CUT/REM/MV ops anchored on the read tag), `replace`, `patch`, `apply_patch` (Codex envelope), `sloppy`. All-or-nothing staging, fuzzy matching ladder, numbered diffs, error strings byte-identical to pi-edit. Passes the upstream pi-edit fixture suites (`tests/fixtures/pi-edit`) |
| `bash` | `tools/write_bash.c`, `shell.c`, `jobs.c`, `services.c` | `tools/bash.ts`, `async/*`, `launch/*` (`crates/pi-shell`) | Exclusive. Persistent `bash --noprofile --norc` session (env, functions and aliases survive across calls), with a per-command nonce marker, timeouts and PTY. `async: true` jobs, auto-background after `bash.autoBackground.*`, named services (`launch.enabled`) |
| `wait` | `tools/write_bash.c`, `jobs.c` | `tools/wait.ts` | Waits for background jobs, services or agent replies |
| `grep` | `tools/grep_glob.c`, `text/search.c`, `text/regex.c` | `tools/grep.ts` (`crates/pi-natives`) | Rust-regex syntax, gitignore-aware parallel search, grouped file output, paging |
| `glob` | `tools/grep_glob.c`, `text/walker.c`, `text/glob.c` | `tools/glob.ts` (`crates/pi-walker`) | |
| `find` | `tools/find.c` | `tools/jfind/*` | Lexical IDF scan → passage windows → one model judge pass (judge role chain), lexical fallback |
| `ast_grep`, `ast_edit` | `tools/astgrep.c` | `tools/ast-grep.ts`, `ast-edit.ts` (`crates/pi-ast`) | Bracket-aware token-tree matcher with ast-grep metavariables (not tree-sitter). `ast_edit` stages a proposal that is resolved through `xd://resolve` / `xd://reject` |
| `todo` | `tools/todo_ask.c` | `tools/todo.ts` | Phased task list (`pending`/`in_progress`/`completed`/`abandoned`) |
| `ask` | `tools/todo_ask.c` + `ui/ask.c` | `tools/ask.ts` + `overlays/ask-dialog.ts` | Exclusive. Only with a UI that can answer (`tool_ui_t.ask`) |
| `web_search` | `tools/web.c` | `web/search/*` | Provider chain: Brave, Tavily, Exa, DuckDuckGo (keys from env) |
| `task` | `tools/task.c`, `coding/agents.c` | `task/*` | Subagents ([agent.md](agent.md#subagents-and-background-jobs)) |
| `eval` | `tools/eval.c`, `coding/evalhost.c`, `assets/eval_kernel.py` | `tools/eval.ts`, `eval/*` | Exclusive. Persistent Python kernel (below); not registered when no Python is found |
| `lsp` | `lsp/lsp.c` | `coding-agent/src/lsp/*` | 14 actions (definition, references, hover, symbols, diagnostics, rename, code_actions, capabilities, ...); 55 bundled server definitions (`assets/lsp-servers.json`) plus `lsp.json`/`lsp.yml` |
| `debug` | `dap/dap.c` | `tools/debug.ts`, `dap/*` | Exclusive. DAP client, one active session, 28 actions; bundled adapters (`assets/dap-adapters.json`) plus `debug.adapters` |
| `checkpoint`, `rewind` | `coding/app.c` | `tools/checkpoint.ts` | Off by default ([agent.md](agent.md#checkpoint-and-rewind)) |
| `goal` | `coding/goal.c` | `goals/tools` | Goal mode only |
| `learn`, `manage_skill` | `coding/autolearn.c` | `tools/learn.ts`, `manage-skill.ts` | `autolearn.enabled` |
| `advise` | `coding/advisor.c` | `advisor/*` | Only inside the advisor's own agent |
| `mcp__<server>_<tool>` | `mcp/mcp.c` | `mcp/*` | One per MCP server tool |
| host tools | `modes/rpc.c` (`set_host_tools`), `lib/omc.c` (`omc_register_tool`) | `modes/rpc/host-tools.ts` | Executed by the embedding program |

omp tools with no omc implementation (`security_scan`, `ida`, `github`, `context_notes`,
`new_context`, the hindsight/mnemopi memory tools, `browser`, `computer`, `image_gen`, `tts`) are
listed in [status.md](status.md). Their names stay in `BUILTIN_ORDER` so ordering remains canonical.

## `xd://` tool devices (`tools/xdev.c`)

This ports `tools/xdev.ts` and `internal-urls/xd-protocol.ts`. With `tools.xdev` on (the default)
and both `read` and `write` present, *discoverable* tools are taken off the request's top-level tool
list and driven through files:

| Operation | Effect |
|---|---|
| `read xd://` | list mounted devices with one-line summaries |
| `read xd://<tool>` | docs plus the parameter schema rendered as a TypeScript type (`type Args = {...}`) |
| `read xd://<tool>/<topic>` | named doc topics (for example `xd://eval/judge`, `xd://eval/helpers`) |
| `write xd://<tool>` | run the tool. `content` is the JSON arguments object, validated like a normal call |
| `write xd://report_issue` | automated-QA report, recorded locally to `autoqa.jsonl` after consent and never sent anywhere |
| `write xd://propose` | plan-mode approval ([agent.md](agent.md#plan-mode)) |
| `write xd://resolve`, `xd://reject` | finish a staged `ast_edit` |

The default mounts are `ast_grep`, `ast_edit`, `debug`, `lsp`, `checkpoint`, `rewind` and MCP tools.
`todo`, `ask`, `grep`, `web_search`, `read` and `write` stay top-level (omp's
`XDEV_KEEP_TOP_LEVEL`), as does any tool named explicitly in `--tools`. The system prompt carries the
device catalog or inline docs (`tools.xdevDocs`, `tools.xdevInlineDevices`).

## Internal URLs

`read` (and `write`/`edit` where noted) accept these schemes. They are resolved in
`read_execute()`, `tool_resolve_path()` / `tool_resolve_internal_url()` (`tools_common.c`) and
`tools/vurl.c`. The system prompt's "Internal URLs" section is generated by `internal_url_docs()`
from omp's `prompts/internal-urls/*.md`, listing only what the session can serve.

| URL | Served by | Meaning | omp |
|---|---|---|---|
| `skill://<name>[/file]` | `tools_common.c` | the skill's `SKILL.md`, or a file inside its dir (no escape) | `skill-protocol.ts` |
| `rule://<name>` | `tools_common.c` | a discovered rule file | `rule-protocol.ts` |
| `artifact://<id>[:sel]` | `tools_common.c`, `artifacts.c` | full output of a truncated tool call | `artifact-protocol.ts` |
| `local://<path>` | `tool_resolve_path` | per-session scratch dir `<session dir>/<id>/local` (plans live here); paths clamped to the root; read/write/edit | `local-protocol.ts` |
| `memory://root[/…]` | `coding/memory.c` | local-backend memory files | `memory-protocol.ts` |
| `agent://<id>[/json/path]`, `agent://all` | `vurl.c`, `coding/agents.c` | subagent outputs; `write` sends a peer message | `agent-protocol.ts` |
| `history://[<id>]` | `vurl.c` | subagent transcripts | `history-protocol.ts` |
| `proc://[<id>[/…]]` | `vurl.c`, `jobs.c`, `services.c` | jobs, agents and services: listing, status, output streams, `stdin`, `kill`, mode | `proc-protocol.ts` |
| `mcp://<resource-uri>` | `read.c`, `mcp.c` | MCP resource read (the server owning the URI answers) | `mcp-protocol.ts` |
| `pr://[owner/repo/]N[/diff[/i\|all]]`, `issue://[owner/repo/]N` | `tools/github.c` | GitHub REST; repo defaults to the cwd's github.com remote; token from `GITHUB_TOKEN`, `GH_TOKEN` or `gh` | `issue-pr-protocol.ts` |
| `ssh://[user@]host[:port]/path` | `tools/ssh.c` | remote UTF-8 text (≤1 MiB) and directory listings through OpenSSH (ControlMaster, `BatchMode=yes`, hosts from `ssh.json`); byte-exact staged writes; read/write/edit | `ssh-protocol.ts` |
| `omc://[doc]` | `read.c` | the embedded README and `docs/*.md` (omp's `omp://`) | `omp-protocol.ts` |
| `xd://…` | `xdev.c` | tool devices (above) | `xd-protocol.ts` |
| `http(s)://…` | `tools/web.c` | page as reader markdown, `:raw` for the original | `tools/read.ts` URL path |
| `file://…` | `tool_resolve_path` | plain path | |

## Output capture and artifacts (`tools/outsink.c`, `tools/artifacts.c`)

These port omp's `OutputSink` (`tui/src/tools/streaming-output.ts`), `output-meta.ts` and
`session/artifacts.ts`, so bash, eval and read results are byte-identical to omp's:

- `sanitizeText`: UTF-8 repair, ANSI/OSC/DCS/APC stripping, C0/C1 removal, CR/CRLF → LF.
- Per-line cap `tools.outputMaxColumns` (768 bytes) with a `…` marker and an `artifact://N:raw:L-L`
  recovery notice.
- Head window `tools.artifactHeadBytes` plus a rolling tail within
  `tools.artifactSpillThreshold` (50 KB). The middle is elided as `[…Nln elided…]` with a
  `Read artifact://N for full output` notice.
- Artifacts are stored as `<session file minus .jsonl>/<id>.<tool>.log`, numbered per session and
  shared with subagents. Ids are allocated per bash/eval call. Files are capped at
  `tools.artifactMaxBytes`, and they are copied when a session is forked.
- `tool_postprocess_result()` (the `after_tool` hook) spills any oversized result from any tool
  the same way and writes omp's `details.meta`.

## Eval kernel protocol (`tools/eval.c`, `assets/eval_kernel.py`)

The kernel is a persistent Python process started as `python3 -u -c <embedded source>` (interpreter
from `python.interpreter`, else `python3`/`python` on `PATH`). At startup it keeps private
duplicates of its stdin/stdout pipes for the protocol, points fd 0 at `/dev/null` and fd 1 at fd 2.
Anything user code, C extensions or subprocesses print lands on the stderr pipe, which the host
captures as raw cell output, so stray output can never corrupt the framing.

Newline-delimited JSON on the private channel:

```
kernel → host   {"type":"ready"}
                {"type":"call","id":M,"name":"read"|"__judge__"|…,"args":{…}}
                {"type":"cancel","id":M}
                {"type":"result","id":N,"stdout":"…","error":null|"…","images":[…]}
host → kernel   {"type":"exec","id":N,"code":"…"}
                {"type":"reply","id":M,"ok":true,"value":…} | {"type":"reply","id":M,"ok":false,"error":"…"}
```

- A host reader thread owns the read side. Each bridge `call` runs on its own worker thread, so
  concurrent helper calls (`asyncio.gather` over `judge()`) never block each other.
- `tool.<name>(...)` calls session tools. Session helpers go through `coding/evalhost.c`:
  `completion()` handles (default/smol/slow tiers, `schema` via a forced `respond` tool) with
  `wait()`/`.status`/`.cancel()`, `judge()` typed choice/bool/score answers (text judge with keyword
  prompts, `coding/judgment.c`, or native judgment APIs), `judge_batch()` host-owned async batches,
  `budget`, and `output()` over `agent://` outputs. Also `%load`, `%pip`, `%time`, `!cmd`, figures
  and printed images.
- Interrupts (timeout or abort) send SIGINT and cancel the cell's in-flight calls. A kernel that
  doesn't answer within 5 s is killed. Waiting on helpers pauses the cell deadline.

omp's JS/Bun eval runtime and the `agent()`/`workpool()`/browser preludes are not implemented and are
not advertised in the description.

## MCP client (`src/mcp/`)

- Transports: stdio (`command` + `args`, a reader thread per server), streamable HTTP (`url`,
  `type: http|streamable-http`) and legacy SSE (`type: sse`). Protocol version `2025-06-18`.
- Config discovery (project first, then user; `mcp_discover_config`): `.omc/mcp.json`,
  `.omp/mcp.json`, `mcp.json`, `.mcp.json`, `.claude/mcp.json`, `.cursor/mcp.json`,
  `.vscode/mcp.json`, `<agent dir>/mcp.json`, `~/.cursor/mcp.json`, `~/.claude/mcp.json` and
  `~/.claude.json` (global and per-project `mcpServers`).
- Servers connect in parallel at session start (`mcp.startupTimeoutMs`). Their tools become
  `mcp__<server>_<tool>` (mounted under `xd://` by default) and their instructions are appended to
  the system prompt. Resources are read through `mcp://`. Subagents share the parent's connections.
- OAuth for remote servers (`mcp/oauth.c`, port of `mcp/oauth-discovery.ts` and `oauth-flow.ts`):
  RFC 9728 resource metadata → RFC 8414/OIDC metadata → RFC 7591 dynamic registration (or a
  configured client) → authorization code + PKCE with the RFC 8707 `resource` indicator, through the
  shared login engine. Credentials are stored in `auth.json` under `mcp:<server url>` and refreshed
  automatically. Managed with `omc mcp list|login|logout`.

## LSP client (`src/lsp/lsp.c`)

JSON-RPC over stdio to language servers chosen by file type from `assets/lsp-servers.json` (omp's
definitions) merged with user `lsp.json`/`.lsp.json`/`lsp.yml`. Servers start lazily and are shared
with subagents. `lsp_after_write()` syncs written files and returns diagnostics, which are appended to
write results (`lsp.diagnosticsOnWrite`, on) and to edit results only with `lsp.diagnosticsOnEdit`
(off by default, like omp). The wait is `lsp.diagnosticsWaitMs`.
Timeouts follow omp's `resolveLspTimeouts`.

## DAP client (`src/dap/dap.c`)

The `debug` tool drives one active Debug Adapter Protocol session (launch/attach, breakpoints,
stepping, stack, scopes, variables, evaluate, ...). Adapters come from
`assets/dap-adapters.json` plus the `debug.adapters` setting, and are selected by `fileTypes`. A
reader thread handles events. The e2e suite runs it against a scripted adapter
(`tests/e2e/mock_dap.py`).

## Adding a tool

1. Implement `agent_tool_t *tool_<name>_new(tool_env_t *)` in `src/tools/`. Use omp's description
   file via `tool_prompt()` and omp's exact schema via `schema_lit()`. Set `exclusive` if the tool
   mutates state.
2. Add it to `CTORS` and gating in `tools_common.c` (and `TOOL_META` for label/summary). Keep
   `BUILTIN_ORDER` canonical.
3. Report failures with `tool_result_error()` using omp's wording. Put renderer data in
   `details` in omp's shape so the TUI card (and omp's card on a shared session) render it.
4. Add a card renderer in `src/ui/tools/` ([tui.md](tui.md)), an e2e check in
   `tests/e2e/run.sh`, and a request-parity check if the wire changes.
5. Update [status.md](status.md) and this file.
