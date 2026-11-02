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
omc --mode rpc | rpc-ui             JSONL RPC on stdin/stdout
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
compact, shake, handoff, retry, memory, rename, dirs): RPC answers `{"agentInvoked":false}` and emits
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
uses it ([below](#library-embedding)).

On start the endpoint emits `{"type":"ready","protocolVersion":1,...}`. **Commands** carry
`{"type": "<command>", "id"?: "..."}` and get exactly one response:

```json
{"id":"1","type":"response","command":"prompt","success":true,"data":{...}}
{"id":"2","type":"response","command":"set_model","success":false,"error":"Unknown model: ..."}
```

| Group | Commands |
|---|---|
| Prompting | `prompt`, `steer`, `follow_up`, `abort`, `abort_and_prompt`, `abort_retry`, `abort_bash`, `abort_branch_summary`, `abort_and_restore_queue`, `remove_queued_message`, `promote_queued_message`, `bash`, `btw`, `handoff`, `compact` |
| State | `get_state`, `get_messages`, `get_messages_page`, `get_entries`, `get_last_assistant_text`, `get_session_stats`, `get_available_commands`, `get_login_providers` |
| Model | `set_model`, `cycle_model`, `get_available_models`, `set_thinking_level`, `cycle_thinking_level`, `get_available_thinking_levels` |
| Session | `new_session`, `open_session`, `switch_session`, `fork`, `branch`, `get_tree`, `navigate_tree`, `get_branch_messages`, `set_label`, `set_title`, `set_session_name`, `export_html` |
| Behavior | `set_steering_mode`, `set_follow_up_mode`, `set_interrupt_mode`, `set_auto_compaction`, `set_auto_retry`, `set_event_filter`, `set_todos`, `goal`, `set_advisor`, `negotiate_protocol` |
| Host tools | `set_host_tools`, `host_tool_result`, `host_tool_update` |

**Events** are agent events in omp's JSON shape, plus RPC frames: `notice`, `prompt_result`
(end of a prompt), `session_settled`, and host-tool frames. `set_event_filter` restricts which event
types are sent.

**Host tools** let the client implement tools. `set_host_tools` registers `{name, description,
parameters}` definitions. When the model calls one, omc sends
`{"type":"host_tool_call","id":"host-N","toolCallId","toolName","arguments"}` and blocks that tool
thread until the client answers `{"type":"host_tool_result","id":"host-N","result":{content,
details?},"isError":false}`. On abort it sends `host_tool_cancel` with `targetId`.

When stdin closes, an in-flight run finishes before exit. Titles are disabled in RPC mode.

## ACP (`modes/acp.c`)

This is the Agent Client Protocol for editors such as Zed (omp `modes/acp/*`): JSON-RPC 2.0 over
newline-delimited JSON on stdio. Methods: `initialize`, `session/new`, `session/load`,
`session/list`, `session/prompt`, `session/cancel`, `session/set_mode`. Agent output is streamed as
`session/update` notifications (message and thought chunks, `tool_call` / `tool_call_update`,
plan, usage). `bash` calls and edits that delete or move a file first ask the client through
`session/request_permission` (Allow once, Always allow, Reject, Always reject; see
[tools.md](tools.md#tool-approval)), unless `tools.approvalMode: yolo` is configured explicitly or
`--auto-approve` is given. Each ACP session is its own `app_t` with its own prompt thread.

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
