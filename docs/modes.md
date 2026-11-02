# Run modes

`omc_main()` (`src/cli/cli.c`, omp `cli.ts`/`main.ts`) routes argv to a subcommand or to the launch
command, which creates one `app_t` and hands it to a mode in `src/modes/`. Every mode drives the same
session runtime ([agent.md](agent.md)). They differ only in how input arrives and how events leave. For
which commands are complete, see [status.md](status.md) (Modes & UI).

## Command line

`omc --help` and `omc <command> --help` print omp's help layout (USAGE / ARGUMENTS / FLAGS / EXAMPLES /
COMMANDS, then the environment and tool notes) listing only what omc implements.

```
omc [flags] [messages...]           interactive (TUI); each message is its own prompt; @file / @image args
omc -p [flags] <messages...>        print mode (piped stdin leads the first prompt; no TTY implies print)
omc --mode json -p <prompt>         print mode, JSON event lines
omc --mode rpc | rpc-ui             JSONL RPC on stdin/stdout (rpc-ui: the ask tool and tool approvals ask the client)
omc acp | --mode acp                Agent Client Protocol on stdio
omc <command> [args]                subcommand (see below)
```

Routing (`src/cli/args.c`, port of `cli-commands.ts resolveCliArgv`): a registered command name or alias
runs it, even behind leading launch flags (`omc --cwd x models` → `models`, launch-only flags dropped);
omp's plugin verbs (`list`, `remove`, `marketplace add …`) and omp commands omc does not port (`stats`,
`update`, `plugin`, `commit`, `usage`, `session`, `ttsr`, …) are refused with `error: …` (exit 1) instead of
reaching the model as a prompt; everything else is the launch command (`omc launch <words>` forces that).

Launch flags (`src/cli/args.c`, port of `cli/args.ts` + `flag-tables.ts`; `--flag=value` everywhere,
`--` ends options): `--model`, `--smol/--slow/--plan` (or `PI_SMOL_MODEL`/`PI_SLOW_MODEL`/`PI_PLAN_MODEL`;
runtime `modelRoles` overrides), `--models a,b` (scope: the first in-scope model starts a fresh session,
explicit `:level` honored), `--provider`, `--api-key`, `--thinking`, `--goal`, `--plan-yolo[-into]`,
`--system-prompt`, `--system-prompt-template <file>` (Handlebars, replaces the bundled template; a project or
agent-dir `SYSTEM_TEMPLATE.md` is discovered like `SYSTEM.md`), `--append-system-prompt`, `--cwd`, `--mode`,
`--config` (repeatable), `-p`, `-c`, `-r/--resume [id|path]` (no value: the session picker; Esc prints
`No session selected`), `--restore <session>` (resume, then go on with the interrupted turn:
`src/coding/restore.c`), `--session-dir`, `--no-session`, `--fork`, `--tools`, `--no-tools`, `--no-lsp`,
`--hide-thinking`, `--advisor`, `--skills <globs>` (`skills.includeSkills`), `--no-skills`, `--no-rules`,
`--no-context-files`, `--export <session> [out]`, `--no-title`, `--print-thoughts`, `--max-time 600|10m|1h`
(runs stop at the deadline), `--warn-cost` / `--max-cost` (`session.warnCost` / `session.maxCost`:
`src/coding/costgate.c`; a refused request ends print mode with exit 3), `--auto-approve`/`--yolo`,
`--approval-mode`. omc extras (not in the help): `--plan-mode`, `--max-turns`. Unknown flags and bad enum
values exit 2 with omp's `Error: …` lines.

Commands (`src/cli/commands.c`; parsed by `src/cli/cmd.c`, a port of pi-utils `Command.parse` over
node:util `parseArgs` strict mode, with its `error: … / USAGE / Run \`omc <cmd> --help\`` reports):
`acp`, `agents unpack`, `auth export|import`, `completions bash|zsh|fish` (+ hidden `__complete`),
`config list|get|set|reset|path` (`--json`, `--scope global|project`), `grep`, `login [provider]`,
`models [ls|find|refresh|<provider>]` (`--json`, `--kind`), `read`, `search` (`q`, `web-search`), `setup
[python]` (no component: the first-run wizard), `ssh add|remove|list`, `token <provider>`, `worktree
list|clear|add` (`wt`), `migrate`. Hidden omc extras: `mcp list|login|logout`, `export <session> [out]`,
`sessions`, `logout`, `glob`, `bash`, `version`.

## Interactive (`modes/interactive.c`)

This ports omp's `InteractiveMode`. The UI thread runs the TUI ([tui.md](tui.md)). Prompts run on a
worker thread (`worker_main`; `retry_main` for `/retry`; `btw_main` for `/btw`), and agent events are
posted back to the UI thread. While a run is active, input typed in the composer is queued as steering
or follow-up (`steeringMode`, `followUpMode`, `interruptMode`). Esc aborts the run (and pauses an active
goal). Ctrl+C clears the input or exits. Background jobs that settle while the session is idle wake it
through `app_t.wake`.

If no model is configured on a terminal, the TUI still starts so that `/login` and `/model` can fix
it, as omp does. A fresh agent dir shows the setup wizard (`OMC_SKIP_SETUP`/`OMP_SKIP_SETUP`,
`startup.setupWizard`). Slash commands come from one registry (`src/coding/slashcmd.c`, omp's
builtin-*.ts metadata filtered to what omc implements): `/name args` or `/name:args`, aliases
(`/models`, `/rewind`, `/q`), commands without allowArgs fall through to the model when given
arguments, and bare `exit`/`quit`/`q` quit an empty session (`input.bareExitOnEmptySession`;
`input.bareSlashCommands` extends this to every command with an Enter-twice confirm). RPC `prompt`
and ACP `session/prompt` run the builtins that have a text-mode handler (model, effort, advisor,
compact, shake, handoff, retry, memory, rename, fresh, pin, move, wt, add-dir, remove-dir, dirs): RPC answers `{"agentInvoked":false}` and emits
`command_output` frames, ACP streams the output as `agent_message_chunk` and ends the turn;
`get_available_commands` and ACP's `available_commands_update` (sent after session/new and
session/load) list those builtins, then (first name wins, builtin aliases reserved) `skill:<name>` for every
skill (`skills.enableSkillCommands`, hint `arguments`), MCP prompt commands (`<server>:<prompt>`,
source `mcp_prompt`) and file commands (source `file`, hint from `argument-hint`).

Non-builtin commands (`src/coding/commands.c`, port of omp's slash-command capability,
extensibility/slash-commands.ts and config/prompt-templates.ts):
- **File commands** are `commands/<name>.md` from the providers in omp's priority order: native
  (`<cwd>/.omc|.omp/commands`, then `<agentDir>/commands`), Claude Code (`~/.claude/commands` when
  `commands.enableClaudeUser` or the `claude` provider is opted in; `<cwd>/.claude/commands` unless
  `commands.enableClaudeProject: false`; recursive, nested files also as `dir:name`), installed Claude Code
  plugins (`<plugin>:<name>`), `.agent[s]/commands` (walk up to the repo root, then `~/`), Codex
  (`.codex/commands`), OpenCode (`.opencode/commands`, `~/.config/opencode/commands` opt-in), and the
  bundled `/init`. Frontmatter: `description` (else the first body line, 60 characters), `argument-hint`
  / `argumentHint`, `side: true`; Codex/OpenCode honour `name` (and, like omp, take the description from
  the body). `disabledProviders` and `disabledExtensions: [slash-command:<name>]` apply. The body expands
  with `$1`…, `$@`, `$ARGUMENTS`, `$@[start:len]`, then the template engine (`{{arguments}}`, `{{arg N}}`);
  arguments are appended after a blank line when the body has no placeholder.
- **Prompt templates** are `<agentDir>/prompts/**/<name>.md` and `<cwd>/.omc|.omp/prompts`; they expand the
  same way after file commands (and also in RPC `steer`/`follow_up`).
- **MCP prompts** of connected servers (`prompts/list`) run as `/<server>:<prompt> key=value …`
  (`prompts/get`; text and resource text parts joined by blank lines; an empty result consumes the input).
- **`/skill:<name> args`** (or a `/skill:<name>` token after prompt text) sends omp's user-invocation prompt
  (`prompts/skills/user-invocation.md`) as a `skill-prompt` custom message attributed to the user (the model
  sees a user turn; the session stores `custom_message` with `details {name, path, args, prompt, lineCount}`).

AgentSession.prompt's order is kept everywhere: TUI Enter, RPC `prompt` and ACP `session/prompt` try a skill
invocation first (RPC/ACP), builtins, then MCP prompt commands, file commands and templates; print mode
(`-p`) expands MCP/file/template commands but, like omp, does not run skills. `!cmd` / `!!cmd` run shell
commands as user bash (`app_user_bash`), recorded as `bashExecution` messages (with `!!` they are kept out of
the model's context).

Workspace and session lifecycle (WP-21): the session's roots are its cwd plus the header's
`additionalDirectories` (`--add-dir`, `workspace.additionalDirectories`, `/add-dir`, `/remove-dir`; listed by
`/dirs`, rendered in the system prompt's `<workspace-roots>` with each root's context files). `/move [dir]` and
`/wt [branch]` relocate the session (`app_move`: session file, artifacts and `local://` root to the new cwd's
session bucket, then `chdir` and a re-scope of project settings, discovery, file commands and the system
prompt; a failure moves the session back). In the TUI `/move` without an argument opens the directory overlay
and offers to create a missing directory; `/wt` creates the worktree on the next tick so the `Creating worktree
on …` row paints, and the worktrees a launch created are offered for removal on exit per `worktree.onExit`
(planned in the TUI, removed after the terminal is handed back, messages on stderr). `worktree.onStart` runs on
fresh launches only. `/restart` closes the TUI like `/exit` and `cli.c` execs `/proc/self/exe` with omp's
restartArgv (one-shot session selectors and positionals dropped, `--resume <session file>` appended when the
session is on disk). `/local [name]` and Alt+G (`app.local.open`) open a `local://` file in $VISUAL/$EDITOR,
with a picker when no name is given.

## Print and JSON (`modes/print.c`)

This ports `print-mode.ts`. It runs the prompt(s) to completion. Text mode prints the final assistant
text to stdout (notices and `--print-thoughts` thinking go to stderr). JSON mode
(`--mode json`) writes one JSON object per line: first
`{"type":"session","id",...,"cwd","model"}`, then every agent event in omp's shape
(`agent_start`, `message_update`, `tool_execution_start`, ..., `agent_end`). Hidden follow-ups
(async job results, goal continuations) run before exit. Outstanding background jobs are awaited up
to `async.printWaitMs`.

## RPC (`modes/rpc.c`)

This is omp's RPC protocol (`modes/rpc/*`). One JSON object per line on stdin, and responses plus
events on stdout. `rpc_attach()` / `rpc_handle_json()` make the same endpoint embeddable: the library
uses it ([below](#library-embedding)). `--mode rpc-ui` is the same protocol with the `ask` tool enabled
(its questions go to the client).

On start the endpoint emits `{"type":"ready","protocolVersion":1,"supportedProtocolVersions":[1,2],
"maxFrameBytes":1048576,"maxReassembledFrameBytes":67108864}`, then `advisor_cost_changed` and
`available_commands_update`. **Commands** carry `{"type": "<command>", "id"?: "..."}` and get exactly one
response (`code` is a machine-readable reason on some errors):

```json
{"id":"1","type":"response","command":"prompt","success":true}
{"id":"2","type":"response","command":"set_model","success":false,"error":"Model not found: mock/nope"}
{"id":"3","type":"response","command":"get_entries","success":false,"error":"Unknown entries cursor: x","code":"unknown_since"}
```

| Group | Commands |
|---|---|
| Protocol | `negotiate_protocol` (only v2) |
| Prompting | `prompt`, `steer`, `follow_up`, `abort`, `abort_and_prompt`, `abort_retry`, `abort_bash`, `abort_branch_summary`, `abort_and_restore_queue`, `remove_queued_message`, `promote_queued_message`, `bash`, `btw`, `btw_cancel`, `handoff`, `compact` |
| State | `get_state`, `get_messages`, `get_messages_page`, `get_entries`, `get_last_assistant_text`, `get_session_stats`, `get_available_commands`, `get_btw_history` |
| Model | `set_model`, `cycle_model`, `get_available_models`, `set_thinking_level`, `cycle_thinking_level`, `get_available_thinking_levels` |
| Session | `new_session`, `open_session`, `switch_session`, `fork`, `branch`, `get_tree`, `navigate_tree`, `get_branch_messages`, `set_label`, `set_session_name`, `export_html` |
| Behavior | `set_steering_mode`, `set_follow_up_mode`, `set_interrupt_mode`, `set_auto_compaction`, `set_auto_retry`, `set_event_filter`, `set_ask_dialog`, `set_todos`, `goal`, `set_advisor` |
| Auth | `get_login_providers`, `login`, `get_logout_accounts`, `logout` |
| Host tools / UI | `set_host_tools`, `host_tool_result`, `host_tool_update`, `extension_ui_response` |

`prompt`, `bash` and `login` answer off the reader thread, so `abort`, `abort_bash` and
`extension_ui_response` overtake them. Each accepted prompt gets one `prompt_result` (`agentInvoked`,
`status`, `sessionSettled`) after its work yields; a prompt sent while a run streams is queued into it and
reports at that run's yield.

**Events** are agent events in omp's JSON shape. `message_start` / `message_update` / `message_end`
carry a `messageId` (`msg-N`, shared by one message's frames); `message_update.assistantMessageEvent`
carries the accumulated `partial`. `set_event_filter {events, messageUpdates: "delta"}` restricts the
event types and replaces the snapshots with increments (`*_start` carries the opened block, `*_end` the
block metadata). Session events: `model_changed`, `thinking_level_changed`, `goal_updated`,
`queue_update`, `auto_compaction_start/end`, `manual_compaction_start/end`, `auto_retry_start/end`,
`retry_fallback_applied/succeeded`, `todo_reminder`, `available_commands_update`, `notice`, plus
`command_output`, `prompt_result`, `session_settled`, `btw_delta` / `btw_record` and the host-tool frames.

**Framing.** A line is at most 1 MiB. After `negotiate_protocol {protocolVersion: 2}` larger frames
are split into `{"type":"rpc_chunk","chunkId","index","count","byteLength","data"}` lines (base64 of
the UTF-8 JSON, 256 KiB per chunk, 64 MiB per frame). Under v1 an oversized response becomes
`RPC response exceeded the transport limit`, an `agent_end` drops the messages already streamed, and
other frames are shrunk (`…[N chars elided for RPC frame]`).

**Dialogs.** Tools that need the user send `{"type":"extension_ui_request","id","method",...}` and
wait for `{"type":"extension_ui_response","id","value"|"confirmed"|"cancelled"|"answers"}`. The ask
tool uses `select` (options plus `Other (type your own)`, `optionDetails` for descriptions) and
`editor` (`promptStyle`), or one `ask` request after `set_ask_dialog {enabled: true}`; tool approvals
(`tools.approvalMode`) use a `select` with the full prompt as title and `Approve` / `Approve <tool>
for this session` / grant labels / `Deny`; `login` sends `open_url`, `notify` and `input`. A dialog
closed by an abort or timeout is followed by a `cancel` request; when stdin closes pending dialogs
fail.

**Host tools** let the client implement tools. `set_host_tools` registers `{name, description,
parameters}` definitions. When the model calls one, omc sends
`{"type":"host_tool_call","id":"host-N","toolCallId","toolName","arguments"}` and blocks that tool
thread until the client answers `{"type":"host_tool_result","id":"host-N","result":{content,
details?},"isError":false}` (`host_tool_update` partial results stream as `tool_execution_update`).
On abort it sends `host_tool_cancel` with `targetId`.

When stdin closes, an in-flight run finishes before exit. Titles are disabled in RPC mode.
`tests/parity/rpc_diff.py` diffs omp's and omc's frame streams for a scripted session.

## ACP (`modes/acp.c`)

This is the Agent Client Protocol for editors such as Zed (omp `modes/acp/*`, `commands/acp.ts`): JSON-RPC 2.0
over newline-delimited JSON on stdio. Editors spawn `omc acp` (the same as `omc --mode acp`); with
`--acp-terminal-auth` (the client's terminal auth method) it opens the interactive TUI instead. Every frame is
written to a private dup of fd 1, and fd 1 itself is pointed at stderr so stray output cannot corrupt the stream.

**Methods.** `initialize` (records the client capabilities), `authenticate`, `session/new`, `session/load`
(replays the history: user and assistant messages, thinking, tool calls with their results),
`session/resume` (no replay), `session/fork`, `session/close`, `session/list` (cwd filter, 50-entry pages with
`nextCursor`), `session/set_mode`, `session/set_config_option` (`mode`, `model`, `thinking`),
`session/prompt`, `session/cancel`, and the extension methods `_session/steering` (inject into a running client
turn, else `promptRequired`), `_omp/sessions/listAll`, `_omp/projects/list`, `_omp/chats/byCwd`. Errors use
omp's codes: `-32002` for an unknown session, `-32602` for steering validation, `-32603` with
`data.details` otherwise. Session responses carry `configOptions` and `modes`; 50 ms after them the bootstrap
updates follow (MCP connect failures, `available_commands_update`, `session_info_update`).

**Sessions.** Each ACP session is its own `app_t`. Prompts run on a chain of worker threads (a prompt that
arrives while one runs cancels it and queues), so the stdin reader stays free for cancels, steering and the
client's responses. A turn tries a skill command, then a builtin (output as `agent_message_chunk`), then
file / template / MCP prompt commands and the model. It ends with `usage_update {size, used, cost}`,
`session_info_update`, and the response `{stopReason, usage}` (stop reasons: `cancelled`, `max_tokens`,
`refusal` for content-filter / refusal errors, else `end_turn`). MCP servers come only from the client
(`mcpServers`, stdio / http / sse); config-file servers are not loaded, as in omp.

**Updates.** Agent events map like acp-event-mapper.ts: message and thought chunks with a message id per
assistant message, `tool_call` (title, kind, `$ cmd` start content, locations), `tool_call_update` (in-progress
frames, `diff` content from edit results, `terminal` content, merged result text), `plan` for todo results and
reminders, `current_mode_update` and `config_option_update` when the mode, model or thinking level changes.

**Client bridges.** When the client advertises them, the read tool reads through `fs/read_text_file` (unsaved
editor buffers), write and edit write through `fs/write_text_file` (session artifacts stay local), bash runs in
a client terminal (`terminal/create`, `wait_for_exit`, polled `terminal/output`, `kill` on timeout or abort,
`release`), and with form elicitation the `ask` tool and plan approval ask through `elicitation/create`.
Bash and delete/move edits ask through `session/request_permission` first (Allow once / Always allow /
Reject / Always reject), unless an explicit yolo or `--auto-approve` is set.

## Library embedding

`make lib` builds `libomc.a` / `libomc.so.0` with the public header `include/omc.h`
([build.md](build.md#optional-library-make-lib)). The API is a thin layer over the RPC endpoint
(`src/lib/omc.c`):

| Function | Purpose |
|---|---|
| `omc_main(argc, argv)` | run the whole CLI/TUI in-process (`src/main.c` is just this) |
| `omc_session_new(opts, on_event, ud, &err)` | create a session. `omc_options_t` (with `struct_size`) mirrors the CLI options: cwd, agent_dir, model, provider, api_key, thinking, system prompts, session dir/resume/no_session, tool allow-list, no_tools/context/skills/rules/mcp, plan_mode. Emits `{"type":"ready",...}` first |
| `omc_send(s, json)` | send any RPC command (asynchronous; the response arrives via `on_event`) |
| `omc_prompt(s, text, &answer)` | run a prompt to completion on the calling thread and return the final text |
| `omc_abort`, `omc_wait_idle`, `omc_busy` | control a running turn |
| `omc_register_tool(s, name, description, schema_json, fn, ud)` | native host tool. `fn(ud, args_json, &is_error)` returns malloc'd result text and runs on a worker thread |
| `omc_session_id`, `omc_version`, `omc_api_version`, `omc_free` | utilities |

`on_event` receives one JSON object per call (responses and events, exactly what `--mode rpc` would
print) on omc worker threads. Keep it short, and don't free the session from inside it. Settings,
credentials and the catalog are process-wide. The first session's `agent_dir` wins (it is applied as
`OMC_CODING_AGENT_DIR`). `examples/embed.c` shows a native tool and the RPC protocol, and the e2e suite
runs it.
