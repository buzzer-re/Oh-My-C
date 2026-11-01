# Run modes

`omc_main()` (`src/cli/cli.c`, omp `cli.ts`/`main.ts`) parses flags and runs a subcommand, or it
creates one `app_t` and hands it to a mode in `src/modes/`. Every mode drives the same session
runtime ([agent.md](agent.md)). They differ only in how input arrives and how events leave. For which
commands are complete, see [status.md](status.md) (Modes & UI).

## Command line

```
omc [options] [prompt...]           interactive (TUI); prompt optional; @file args inline files
omc -p [options] <prompt...>        print mode (piped stdin is appended and implies print)
omc --mode json -p <prompt>         print mode, JSON event lines
omc --mode rpc                      JSONL RPC on stdin/stdout
omc --mode acp                      Agent Client Protocol on stdio
```

Options: `-m/--model <pattern[:effort]|@role>`, `--provider`, `--api-key`, `--thinking`,
`-c/--continue`, `-r/--resume <id|path>`, `--fork`, `--no-session`, `--session-dir`, `--cwd`,
`--tools a,b`, `--no-tools`, `--system-prompt`, `--append-system-prompt`, `--no-context-files`,
`--no-skills`, `--no-rules`, `--config <file>` (repeatable settings overlay), `--print-thoughts`,
`--max-turns`, `--plan-mode`, `--goal <objective>`, `-v`, `-h`.

Subcommands: `models [filter]`, `login [provider]`, `logout`, `auth set|list|remove`,
`mcp list|login|logout`, `sessions [--all]`, `export <session> [out.html|md]`,
`config get|set|unset|list|path`, and direct tool runs: `read`, `grep`, `glob`, `bash`.

## Interactive (`modes/interactive.c`)

This ports omp's `InteractiveMode`. The UI thread runs the TUI ([tui.md](tui.md)). Prompts run on a
worker thread (`worker_main`; `retry_main` for `/retry`; `btw_main` for `/btw`), and agent events are
posted back to the UI thread. While a run is active, input typed in the composer is queued as steering
or follow-up (`steeringMode`, `followUpMode`, `interruptMode`). Esc aborts the run (and pauses an active
goal). Ctrl+C clears the input or exits. Background jobs that settle while the session is idle wake it
through `app_t.wake`.

If no model is configured on a terminal, the TUI still starts so that `/login` and `/model` can fix
it, as omp does. A fresh agent dir shows the setup wizard (`OMC_SKIP_SETUP`/`OMP_SKIP_SETUP`,
`startup.setupWizard`). Slash commands come from omp's registry metadata, and only the commands omc
implements are offered. `!cmd` / `!!cmd` run shell commands as user bash (`app_user_bash`), recorded
as `bashExecution` messages (with `!!` they are kept out of the model's context).

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
plan, usage). Edit and execute tools first ask the client through `session/request_permission`
(allow once, always allow, reject). Each ACP session is its own `app_t` with its own prompt thread.

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
