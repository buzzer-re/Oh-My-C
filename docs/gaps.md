# omc gap audit vs omp v18.14.2

This is an inventory of omp's whole user-facing surface, checked item by item against omc. It ends in a
prioritized gap list. [status.md](status.md) records what agents ported. This file starts from the
upstream side instead, so it also covers features nobody had listed.

- **Upstream reference:** omp v18.14.2 (`7efd772`), the pin in [../UPDATE.md](../UPDATE.md).
- **omc reference:** main at `41f5226`.
- **Method:** each category was enumerated from the upstream sources named in its section. Each item was
  checked in omc's code, and cheap cases were probed with `build/omc` against `tests/e2e/mock_openai.py`
  using throwaway agent dirs (no real models).
- **Legend:** ✅ matches · 🟡 partial or placeholder (the notes say exactly what differs) · ❌ missing ·
  ⛔ out of scope (the notes give the reason).

**What "out of scope" means here.** omc links only libc, OpenSSL, zlib, pthread and libm (see
[../AGENTS.md](../AGENTS.md)). A feature is ⛔ only if it needs one of the following:
- the Bun/JS runtime: JS/TS extensions, hooks, custom tools, the eval JS kernel;
- an omp-hosted web service: the `my.omp.sh` relay/share/stream servers, the skills.omp.sh registry;
- a browser engine (puppeteer/CDP);
- on-device ML or audio: ONNX tiny models, Kokoro TTS, STT, realtime voice;
- native desktop automation, or a macOS-only API.

Anything that is plain HTTP, files, git or a subprocess is in scope, even when omp happens to implement it
in TS.

When status.md and this file disagree, check the code. Several status.md rows marked ✅ are 🟡 here:
- the RPC and ACP wire shapes;
- the "Slash commands" row, where many commands are one-line notices that stand in for omp overlays;
- compaction, which is a single summary pass.

## Contents

- [Top 15 gaps](#top-15-gaps-by-user-impact) · [Work packages](#prioritized-work-packages)
- Inventory: [1. Slash commands](#1-slash-commands) · [2. Keybindings and flows](#2-keybindings-and-interactive-flows) ·
  [3. Settings](#3-settings) · [4. Tools, xd://, internal URLs](#4-tools-xd-devices-and-internal-urls) ·
  [5. Overlays and controllers](#5-overlays-dialogs-and-command-controllers) · [6. RPC, ACP, CLI](#6-rpc-acp-and-cli) ·
  [7. Compaction and background systems](#7-compaction-and-background-systems)

## Top 15 gaps by user impact

Ranked by how soon a user hits the gap in a normal interactive session.

1. ~~**Plan approval doesn't show the plan.**~~ Done: `xd://propose` opens omp's Plan Review overlay with all five actions, the tier slider, annotations and save & quit (WP-1, see status.md Plan mode).
2. ~~**Steered and follow-up messages vanish.**~~ Done (WP-2): queued-messages band, delivered messages drawn, Ctrl+Q / Ctrl+Enter follow-up, Alt+Up dequeue, Esc restore.
3. ~~**`!cmd`, `/compact` and `/handoff` run on the UI thread.**~~ Done (WP-2): workers with omp's loaders and Esc cancel; `!cmd`/`$ code` deferred while streaming.
4. **No turn-end, ask or error notifications, and the terminal title never changes.** `completion.notify` and `ask.notify` are on by default in omp. `tui_term_set_title` exists but is never called. (WP-11)
5. **The `/export` HTML is a placeholder.** It is the markdown transcript inside a `<pre>`, while omp embeds the session in its interactive viewer (`export/html/template.*`). `/dump` lacks `all`/`anon` and the request-JSON sidecar. (WP-4)
6. **`thinking: auto` turns thinking off.** This happens via `--thinking auto` or `defaultThinkingLevel: auto`. (WP-3)
7. **Slash-command argument bugs.**
   - `/advisor status` toggles the advisor.
   - `/compact soft` sends "soft" as summary instructions.
   - `/shake images` runs with threshold 0.
   - `/handoff <focus>` and `/resume <id>` ignore their argument.
   - Aliases `/models`, `/rewind`, `/q`, `/providers` are missing. (WP-4)
8. **No app keybinding layer.**
   - These keys do nothing: Ctrl+Z, Ctrl+G (external editor), Ctrl+R (history search), Ctrl+P (role cycle), Alt+P, F5, Esc-Esc (rewind).
   - `keybindings.yml` overrides are ignored.
   - `/hotkeys` advertises about 20 unbound keys. (WP-5, WP-6)
9. **`/context`, `/session`, `/tools`, `/jobs`, `/todo` are one-line notices instead of omp's panels.** `/jobs kill` and the `/todo` subcommands are missing, and `/context` numbers don't add up. (WP-7)
10. **`/tree` and `/branch` are flat 14-row pickers.** omp has a tree selector (filters, labels, folding) and a rewind selector. (WP-8)
11. **Custom markdown commands and `/skill:<name>` don't work.** This covers `.omp/.claude/.opencode commands/*.md`. RPC `get_available_commands` advertises `skill:<name>` entries that nothing executes. (WP-9)
12. **304 of the 402 settings shown in `/settings` have no effect.** This includes `tools.approvalMode`, so the TUI never asks for approval even after the user picks "Always ask". (WP-10, WP-26)
13. **Default-on agent guards are missing.** Todo reminders, unexpected-stop detection, the tool-call loop guard, fabricated-result abort and magic keywords (`ultrathink` …) are absent. The `out://` reply channel and `cfg://` are missing too; their writes are now refused. (WP-12, WP-3)
14. **Compaction is a single summary pass.** No method ladder, server-side compaction, mid-turn/async compaction, stale-read pruning or prompt-cache warming. (WP-13, WP-14)
15. **`omc acp` starts a TUI instead of ACP mode.** Editors call `omp acp`. RPC/ACP prompts don't run slash commands, RPC message events have no `messageId`, the `ask` tool fails in RPC, and several RPC response shapes differ. (WP-15, WP-16)

## Prioritized work packages

WP numbers are stable IDs; the tiers give the order. Each package is sized for one agent. It lists the omc files to change and the upstream files to port
from: **T** = `packages/tui/src`, **U** = `packages/coding-agent/src`, **A** = `packages/agent/src`.
Every package must:
- update [status.md](status.md) and the matching rows here;
- add e2e or TUI checks;
- be verified side by side with `tests/tui/compare.sh` where it is visual.

### P0 — hit in every normal session

| WP | Scope | omc files | Upstream sources |
|---|---|---|---|
| WP-1 Plan review and plan-mode parity (done; remaining: overlay mouse, extension plan-review hooks, mid-stream deferred plan-model switch) | Fullscreen plan review that renders the plan markdown with a Contents sidebar. Five actions: execute in fresh context / compact / keep context (with token count) / refine (feedback to model) / save & quit. Also: tier slider, copy, Ctrl+G edit, `/plan-review`, "Exit plan mode?" confirm with a draft, paused state, `plan.enabled`, `plan.autosave(Dir)`, `modelRoles.plan` switch/restore | `src/coding/app.c` (`app_propose_plan`, `app_set_plan_mode`), new `src/ui/planreview.c`, `src/modes/interactive.c` | T/overlays/plan-review-overlay.ts, plan-toc.ts, plan-save-overlay.ts; U/modes/interactive-mode.ts (`handlePlanModeCommand`, `showPlanReview`, `#approvePlan`, `#exitPlanMode`); U/plan-mode/* |
| WP-2 Queue and non-blocking input ✅ (done: see §2b/§2c; remaining: live `$` output, `/compact` modes are WP-4/WP-13) | Render delivered steer/follow-up user messages; pending-messages view; dequeue (Alt+Up); Esc restores the queue to the editor; follow-up on Ctrl+Q / Ctrl+Enter. Move `!cmd`, `!!cmd`, `/compact`, `/handoff` to a worker with Esc cancel. Defer `!cmd` while streaming. `$code` / `$$code` user eval in the shared kernel. Queue `/compact` for the yield while streaming | `src/modes/interactive.c` (`on_submit`, `run_bang`, `ui_apply_event`, `key_hook`), `src/coding/app.c` (`app_user_bash`, `app_compact`, `app_handoff`), `src/ui/chat.c` | U/modes/controllers/input-controller.ts (`handleFollowUp`, `handleDequeue`, `restoreQueuedMessagesToEditor`), interactive-mode.ts (`updatePendingMessagesDisplay`), command-controller.ts (`handleBashCommand`, `handlePythonCommand`, `handleCompactCommand`), U/modes/prompt/queued-messages.ts |
| WP-3 Model-facing correctness bugs | Port `out://`, `cfg://`, `conflict://N` (write now refuses unknown schemes; main 44b5fc3). Make `thinking: auto` stop meaning off: resolve it like omp, or to the model default until the WP-12 classifier lands. Verify with `tests/parity/request_diff.sh` / `tool_diff.sh` | `src/tools/write_bash.c`, `src/tools/read.c`, `src/tools/vurl.c`, `src/coding/app.c` (thinking setup), `src/ui/settings.c` | U/internal-urls/{router,out-protocol,cfg-protocol,conflict-protocol}.ts, U/tools/conflict-detect.ts, U/auto-thinking/* |
| WP-4 Slash-command correctness and shared registry | One registry (name, aliases, subcommands, hints, handler) used by TUI, RPC and ACP. Fix `/advisor` (status/dump), `/compact [soft\|remote\|snapcompact] [focus]`, `/shake [elide\|images\|thinking]`, `/handoff [focus]`, `/resume <id>`, `/effort` errors, `/delete` on in-memory sessions. Add aliases (models, rewind, q, providers, status, subagents, worktree, plugin, force:). Bare `exit`/`quit`/`q`. `/export` with omp's HTML viewer (`export/html/template.*` embedded as assets). `/dump all\|anon` + request-JSON sidecar | `src/modes/interactive.c` (`handle_command`, `COMMANDS[]`), `src/ui/slash.c`, `src/coding/app.c`, `src/session/session.c` (`session_export_html`), `assets/` | U/slash-commands/builtin-*.ts, builtin-registry.ts, helpers/*, U/session/compact-modes.ts, shake-types.ts, session-dump-format.ts, U/export/html/* |
| WP-5 App keybinding layer | Add the 45 `app.*` actions to the key table. Load `keybindings.yml` (+ legacy .json migration, profile inherit); `/migrate` copies it. Make `key_hook` use `tui_kb()`. Generate `/hotkeys` from the live bindings. Cheap keys: Ctrl+Z suspend, F5/Alt+R retry, Alt+Shift+P plan, Ctrl+C semantics (500 ms, no abort), Ctrl+T status + persist, Ctrl+] jumps, Alt+Y kill ring, Alt+L display reset, Esc-Esc → `doubleEscapeAction`. Fix the false claims in docs/tui.md | `src/tui/keys.c`, `src/tui/editor.c`, `src/modes/interactive.c`, `src/ui/overlays.c` (`hotkeys_markdown`), `src/coding/migrate.c`, docs/tui.md | T/app-keybindings.ts, T/keybindings.ts, T/kill-ring.ts, T/hotkeys-markdown.ts, U/modes/controllers/input-controller.ts |
| WP-7 Report panels | `/context` viewer (category breakdown, browse content); `/session` info overlay + `delete`/`pin`; `/tools` markdown panel incl. xd:// tools; `/jobs` panel + job tree + `full`/`kill`; `/todo` subcommands; `/advisor status\|dump`. Report-panel inline docking for short reports | `src/modes/interactive.c`, `src/ui/overlays.c` (`ui_show_report`), new `src/ui/context.c`, `src/tools/jobs.c`, `src/tools/todo_ask.c` | T/overlays/context-viewer*.ts, session-info-overlay.ts, jobs-panel.ts, job-tree.ts, report-panel.ts; U/modes/controllers/command-controller.ts, todo-command-controller.ts; U/slash-commands/helpers/context-report.ts |
| WP-8 Tree and rewind selectors | `/tree` tree selector (glyphs, filters / `treeFilterMode`, labels, search, fold, branch prev/next); rewind selector for `/branch`, `/rewind`, Esc-Esc; optional branch summary prompt (`branchSummary.enabled`) | `src/modes/interactive.c` ("tree"/"branch"), new `src/ui/tree.c`, `src/coding/app.c` | T/overlays/tree-selector.ts, rewind-selector.ts; U/modes/controllers/selector-controller.ts; A/compaction/branch-summarization.ts |
| WP-9 Custom and skill commands | Discover `commands/*.md` (omp/Claude/OpenCode, user + project, `commands.enable*`) with `$ARGUMENTS`/frontmatter. `/skill:<name>` invocation + completion (`skills.enableSkillCommands`). MCP prompts as commands. Autocomplete entries. Execution from RPC `prompt` / ACP `session/prompt` (uses the WP-4 registry) | `src/coding/discovery.c`, `src/modes/interactive.c`, `src/ui/slash.c`, `src/tui/editor.c`, `src/modes/rpc.c`, `src/modes/acp.c` | U/capability/slash-command.ts, U/extensibility/{custom-commands,skills.ts,slash-commands.ts}, U/modes/skill-command.ts, U/modes/prompt/skill-tokens.ts, U/discovery/{claude,opencode}.ts |

### P1 — common, or the default behaviour differs

| WP | Scope | omc files | Upstream sources |
|---|---|---|---|
| WP-6 Editor I/O | Ctrl+G external editor (main editor + ask "Other"); Ctrl+R history search; copy line/prompt (Alt+Shift+L/C); Ctrl+V image paste + image-path paste + attachments; raw paste; large-paste chip/menu; fuzzy project-wide `@` mentions; `#` prompt actions (`#123` → pr:// / issue://) | `src/tui/editor.c`, `src/modes/interactive.c`, `src/ui/ask.c`, `src/tui/term.c`, new `src/ui/histsearch.c` | U/modes/controllers/input-controller.ts, T/overlays/history-search.ts, U/modes/prompt/{attachment-chips,prompt-action-autocomplete}.ts, T/autocomplete.ts |
| WP-10 Tool approval system | `tools.approvalMode` tiers (always-ask, ask-and-remember, write, yolo), `tools.approval` per-tool policies, `tools.restrictedFolders`, approval dialog (Ctrl+Enter/Ctrl+Q), remembered grants, `/forget-approvals`, `--approval-mode` / `--auto-approve` / `--yolo` | `src/coding/app.c` (`hook_before_tool`), new `src/ui/approval.c`, `src/modes/acp.c`, `src/cli/cli.c` | U/tools/approval.ts, approval-cache.ts, approval-targets.ts, search-approval-targets.ts; T/overlays/approval-dialog.ts |
| WP-11 Turn-lifecycle signals | Completion/ask/error notifications (OSC 9/777, `*.notify`); terminal title with run state + spinner (`tui.titleState`, `tui.titleSpinner`); OSC 7501 program status; idle recap (`recap.*`); sleep prevention (logind inhibitor); release check | `src/modes/interactive.c`, `src/tui/term.c` (`tui_term_set_title`), `src/coding/title.c`, `src/coding/app.c` | U/modes/controllers/event-controller.ts (`send*Notification`), T/desktop-notify.ts, U/utils/{run-status,title-generator}.ts, U/session/idle-maintenance.ts, U/session/power-assertion-backend.ts |
| WP-12 Default-on agent guards | Todo reminders + auto-clear (`todo.reminders`, `remindersMax`, `tasks.todoClearDelay`); unexpected-stop detection; tool-call loop guard / redirect and assistant loop guard (`model.loopGuard.*`); fabricated-result abort; magic keywords (ultrathink/orchestrate/workflow/jevify + hidden notices); auto-thinking classifier. Each must keep `request_diff.sh` clean | `src/coding/app.c` (turn end), `src/agent/agent.c`, `src/modes/interactive.c` (prompt pre-processing) | U/session/{todo-tracker,unexpected-stop-classifier,tool-call-loop-redirect}.ts, U/modes/magic-keywords.ts, U/prompts/system/*-notice.md, U/auto-thinking/* |
| WP-13 Compaction ladder | `compaction.methodOrder` (remote → snapcompact → handoff → shake → soft); shake/handoff as automatic methods (shake regions, tail protection, artifact offload, persisted); server-side compaction (OpenAI Responses, Anthropic, Bedrock); mid-turn + async compaction; pruning (`supersedeReads`, `dropUseless`); `handoffSaveToDisk`; per-model thresholds. snapcompact can be a follow-up package | `src/coding/app.c` (`app_compact`, auto-compaction, `app_shake`, `app_handoff`), `src/session/session.c`, `src/ai/provider_anthropic.c`, `src/ai/provider_openai_responses.c`, `src/ai/provider_bedrock.c` | U/session/{compaction-methods,session-maintenance,compact-modes}.ts, A/compaction/{compaction,shake,pruning,openai,anthropic,bedrock}.ts, packages/snapcompact |
| WP-14 Provider knobs and cache warming | Prompt cache warmer (`providers.cacheWarming`); service tiers `/fast`, `/slow`, `tier.*`, `--service-tier`; `/extended-context` + `extendedContext`; stream first-event/idle timeouts; `providers.maxInFlightRequests`; RPC `set_fast_mode` / `set_slow_mode` / `set_cache_warming` | `src/coding/app.c`, `src/ai/provider_*.c`, `src/ai/ai_common.c`, `src/modes/rpc.c` | U/session/cache-warmer.ts, anthropic-slow-mode.ts, model-controls.ts; U/slash-commands/builtin-modes.ts (fast/slow/extended-context) |
| WP-15 RPC wire parity | `messageId` + `partial` + start/done/error on message events, delta filter; session events (compaction, retry, model/thinking changed, goal_updated, queue_update, available_commands_update, …); response shapes (set_model, new/switch/open/branch/fork, get_entries leafId/since, get_messages_page cursor, get_session_stats, compact, handoff, btw); full `get_state`; `extension_ui_request`/`_response` so `ask` works; login/logout; async `bash` so `abort_bash` works; protocol v2 + `rpc_chunk`; subagent frames; mark status.md 🟡 | `src/modes/rpc.c`, `src/agent/agent.c` (`agent_event_to_json`), `src/coding/app.c` (structured events beside `notice`), `src/modes/print.c`, `src/tools/todo_ask.c` | U/modes/rpc/{rpc-types,rpc-mode,rpc-session-events,rpc-messages,rpc-compat,rpc-frame}.ts, U/session/agent-session-events.ts |
| WP-16 ACP parity | `omc acp` subcommand; slash commands in `session/prompt`; `session/resume`, `close`, `fork`, `set_config_option`, `_session/steering`; `configOptions` + `config_option_update` / `session_info_update` / `available_commands_update`; `mcpServers` in new/load; tool-call history replay; structured diff/terminal tool content; `usage_update {size,used,cost}`; errors → `end_turn`/`refusal`; fs/terminal/elicitation client bridges | `src/modes/acp.c`, `src/cli/cli.c` | U/modes/acp/{acp-agent,acp-event-mapper,acp-client-bridge}.ts, U/slash-commands/acp-builtins.ts, U/commands/acp.ts |
| WP-17 CLI flags and subcommands | `--flag=value`; unknown subcommand-shaped words become an error instead of a prompt; `-r` without a value opens the picker; `--restore`, `--export`, `--models`, `--smol/--slow/--plan`, `--hide-thinking`, `--advisor`, `--max-time/--max-cost/--warn-cost`, `--add-dir`, `--no-lsp`, `--skills`, `--system-prompt-template`, `--mode rpc-ui`; `@image` args. Subcommands: commit, completions, token, usage, setup, session, worktree, agents, ttsr, ssh, search. Fill gaps in models/grep/auth/config | `src/cli/cli.c`, `src/main.c` | U/cli/{args,flag-tables}.ts, U/cli-commands.ts, U/commands/*.ts, U/main.ts |

### P2 — specific workflows

| WP | Scope | omc files | Upstream sources |
|---|---|---|---|
| WP-18 MCP and SSH management | `/mcp add` wizard, list, remove, test, reauth, unauth, enable, disable, reconnect, reload, resources, prompts, notifications (`mcp.enableProjectConfig`, `mcp.notifications`, `mcp.renderMarkdownResults`); `/ssh add\|list\|remove` | `src/mcp/mcp.c`, `src/mcp/oauth.c`, `src/modes/interactive.c`, new `src/ui/mcpwizard.c`, `src/tools/ssh.c` | U/modes/controllers/{mcp-command-controller,ssh-command-controller}.ts, T/overlays/mcp-add-wizard.ts, U/slash-commands/helpers/{mcp,ssh}.ts |
| WP-19 Model switching | `/switch` + Alt+P temporary model picker; Ctrl+P / Shift+Ctrl+P role cycle with cycle track; `/modelpreset` + hub presets; prewalk (`/prewalk`, `prewalk.*`); retry fallback chains (`retry.*`, hub `f`) | `src/ui/modelhub.c`, `src/modes/interactive.c`, `src/coding/app.c` | T/overlays/model-picker.ts, model-hub.ts; U/modes/controllers/input-controller.ts (`cycleRoleModel`); U/session/{prewalk,retry-fallback-chains,role-models}.ts |
| WP-20 Conversation utilities | `/copy` picker + `code\|cmd\|link` targets; `/btw` streaming panel, follow-up and history browser; `/open` | `src/modes/interactive.c`, new `src/ui/copysel.c`, `src/ui/btw.c` | T/overlays/{copy-selector,copy-targets,btw-panel,btw-history-panel}.ts, U/modes/controllers/btw-controller.ts, U/session/btw-history.ts |
| WP-21 Workspace and session lifecycle | `/move`, `/wt` + `worktree.*`, `/add-dir` / `/remove-dir` / multi-root `/dirs`, `/restart`, `/fresh`, `/pin`, `/local` + Alt+G | `src/coding/app.c`, `src/session/session.c`, `src/modes/interactive.c` | U/slash-commands/builtin-lifecycle.ts, U/session/session-worktree.ts, T/overlays/move-overlay.ts, U/modes/local-file-editor.ts |
| WP-22 Subagent visibility | `/hub` agent hub (roster, chat view, revive, kill), `/agents`, running-subagent badge in the status line, agent transcript viewer, session focus (←←), live job widgets (`async.widgets`) | `src/ui/statusline.c`, new `src/ui/agenthub.c`, `src/coding/agents.c`, `src/tools/jobs.c` | T/overlays/{agent-hub*,agents-hub,agent-transcript-viewer,running-subagent-badge}.ts, U/modes/controllers/session-focus-controller.ts, U/job-widgets/* |
| WP-23 Loop mode | `/loop [count\|duration] [--while\|--until cmd] [prompt]`, `loop.*`, Esc suspends | `src/modes/interactive.c`, `src/coding/app.c` | U/slash-commands/builtin-modes.ts (loop), U/modes/{loop-limit,loop-condition}.ts |
| WP-24 Provider usage | `/usage` provider quota dashboard + `/usage reset` selector; per-provider usage fetchers; per-turn usage row (`display.showTokenUsage`, `display.showTurnTime`) | `src/modes/interactive.c`, `src/config/auth*.c`, `src/ui/chat.c` | T/overlays/{usage-dashboard,usage-display,usage-row,reset-usage-selector}.ts, U/slash-commands/helpers/usage-report.ts |
| WP-25 Web search providers | Search through logged-in providers (anthropic, openai, gemini, codex, xai, perplexity, kimi, zai), SearXNG (`searxng.endpoint`), `providers.fetch` | `src/tools/web.c` | U/web/search/{index,providers/*}.ts |
| WP-26 Settings honesty | Until each feature lands, hide ⛔-subsystem settings (§3c) from `/settings`, or mark them unsupported (decide the presentation with the user, since parity says the UI should match omp). Add a unit test that fails when a newly read key is undocumented here | `src/ui/settings.c`, `build-tools/extract_settings.sh` | T/overlays/settings-selector.ts |

### P3 — niche or large, low frequency

| WP | Scope | omc files | Upstream sources |
|---|---|---|---|
| WP-27 Remaining tools | generate_image, remote tts, security_scan + `security://` + `/security`, hindsight memory backend (retain/recall/reflect), `mr://`, `attachment://`, think, yield + `task.structured`, task isolation / outputSchema / perCallModel, eval `agent()` / workpool | `src/tools/*`, `src/coding/memory.c`, `src/tools/task.c` | U/tools/{image-gen,tts,security-scan,think,yield}.ts, U/hindsight/*, U/internal-urls/{mr,attachment,security}-protocol.ts, U/task/* |
| WP-28 Extra commands | `/omfg` (on top of `src/coding/ttsr.c`), `/tan`, `/cleanse`, `/vibe`, `/ratchet`, `/changelog` (embed CHANGELOG), `/debug` selector, `/record`, `/pause`, `/force`, `/skillful`, `/setup` reopen, marketplace/plugins install (git-based; JS parts ⛔), stats aggregation | `src/modes/interactive.c`, `src/coding/*` | U/slash-commands/builtin-*.ts, U/modes/controllers/{omfg,tan,cleanse}-*.ts, U/vibe/*, U/ratchet/*, U/extensibility/plugins/* |
| WP-29 Rich rendering and display options | Mermaid / SVG / inline images (kitty/iTerm/sixel, `terminal.showImages`), table auto-graph, reactions, `display.collapseCompacted`, `display.hideToolActivity` (Ctrl+Shift+O), cache-miss marker, `tui.hyperlinks`, `tui.mouse`, `tui.tight`, vim mode, composer token rate | `src/tui/markdown.c`, `src/ui/chat.c`, `src/tui/term.c`, `src/tui/editor.c` | T/{kitty-graphics,sixel,vim,latex-block}.ts, T/charts/*, T/chat/*, U/auto-graph/* |

Out of scope (⛔) and not packaged: JS/TS extensions, hooks and custom tools; the eval JS kernel;
collab/share/stream (hosted); the skills.omp.sh registry; browser and computer use; STT, live voice,
local TTS and tiny models; IDA; macOS spelling; npm self-update.

## 1. Slash commands

Upstream registry: `packages/coding-agent/src/slash-commands/builtin-*.ts` (87 commands, in this order:
modes, collaboration, session, lifecycle, marketplace, skills, control). omc: `COMMANDS[]` +
`handle_command()` in `src/modes/interactive.c`; autocomplete metadata from
`assets/tui/slash-commands.json` via `src/ui/slash.c` (only commands omc implements are offered).
An unknown `/word` falls through and is sent to the model as a prompt.

omc-only commands (not in omp): `/help`, `/thinking` (omp only has `/effort`), `/name`, `/migrate`.

| Command (aliases) | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| /security | builtin-modes.ts | — | ❌ | OMP-native security scans (plan/scan/status/…/disposition); needs scan engine + `security://` |
| /settings | builtin-modes.ts | `ui_overlay_settings` | ✅ | most settings have no effect, see §3 |
| /setup (providers) | builtin-modes.ts | — | ❌ | first-run wizard exists (`src/ui/setup.c`) but no command to reopen provider setup |
| /plan [prompt] | builtin-modes.ts, interactive-mode.ts `handlePlanModeCommand` | `handle_command` "plan" → `app_enter_plan_mode` / `app_exit_plan_mode` (`src/coding/planmode.c`) | ✅ | enter / "Exit plan mode?" confirm with a draft / paused / disabled, `plan.enabled`, plan-file path in the status and the live description; goal/vibe exclusion messages ❌ |
| /plan-review | builtin-modes.ts | `plan_review_command` (`src/modes/interactive.c`) | ✅ | newest `local://*plan.md`, aborts a running turn first |
| /vibe | builtin-modes.ts | — | ❌ | vibe mode (persistent fast/good worker sessions) |
| /goal | builtin-modes.ts | `goal_command` | ✅ | |
| /guided-goal | builtin-modes.ts | `handle_command` | ✅ | |
| /loop | builtin-modes.ts, loop-limit.ts, loop-condition.ts | — | ❌ | re-submit prompt after every yield, count/duration/`--while`/`--until` |
| /queue | builtin-modes.ts | `handle_command` | ✅ | |
| /model (models) | builtin-modes.ts | `ui_overlay_model` / `app_set_model` | 🟡 | `/models` alias missing (`@role` and `:level` selectors work) |
| /switch | builtin-modes.ts | — | ❌ | session-only (temporary) model switch, `alt+p` |
| /fast | builtin-modes.ts | — | ❌ | priority/ultrafast service tier toggle |
| /slow | builtin-modes.ts | — | ❌ | flex tier / Anthropic low priority |
| /skillful | builtin-modes.ts | — | ❌ | session toggle for skill listing in prompt (`skillful` setting unread too) |
| /extended-context | builtin-modes.ts | — | ❌ | 1M-context toggle (`extendedContext` unread) |
| /computer | builtin-modes.ts | — | ⛔ | native computer-use eval prelude (platform screen/input automation) |
| /ratchet | builtin-modes.ts | — | ❌ | eval hill-climb kickoff (low priority) |
| /prewalk | builtin-modes.ts, session/prewalk.ts | — | ❌ | one-shot model handoff at first edit (`prewalk.*` unread) |
| /modelpreset | builtin-modes.ts | — | ❌ | save/switch role-model presets (Models hub presets also missing) |
| /effort | builtin-modes.ts | alias → `/thinking` | 🟡 | invalid level opens the selector instead of an error; no "Thinking set to X." status; no-reasoning-model message missing |
| /advisor | builtin-collaboration.ts | `handle_command` "advisor" | 🟡 | on/off toggle only; **bug: any other argument toggles** (`/advisor status` switches it); `status`, `dump`, `configure` (advisor-config overlay) missing |
| /export [path] | builtin-collaboration.ts | `session_export_html/markdown` | 🟡 | placeholder HTML: `session_export_html` wraps the markdown transcript in a `<pre>`; omp embeds the session JSON in its interactive viewer (`export/html/template.{html.txt,css,js}`, 137 KB of static assets, portable) |
| /trace | builtin-collaboration.ts | — | ❌ | opens trace in stats dashboard (depends on /stats) |
| /dump [all\|anon] | builtin-collaboration.ts | `session_export_markdown` + OSC 52 | 🟡 | no `all`/`anon` subcommands, does not write the LLM request JSON to tmp, format differs from `session-dump-format.ts` |
| /share | builtin-collaboration.ts | — | ⛔/❌ | share server `my.omp.sh` is omp's hosted service (⛔); secret-gist mode is portable (❌) |
| /collab, /join, /leave | builtin-collaboration.ts, collab/* | — | ⛔ | live relay via `wss://my.omp.sh` + web UI (hosted service) |
| /browser | builtin-collaboration.ts | — | ⛔ | browser eval prelude (Chromium/CDP, puppeteer) |
| /copy | builtin-collaboration.ts, overlays/copy-selector.ts | copies last answer | 🟡 | omp opens a picker of text/code blocks from the conversation |
| /open | builtin-collaboration.ts | — | ❌ | open last link in browser |
| /todo [sub] | builtin-session.ts, todo-command-controller.ts | prints list | 🟡 | subcommands edit/copy/expand/collapse/export/import/append/start/done/drop/rm missing |
| /session [info\|delete\|pin] | builtin-session.ts, session-info-overlay.ts | info notice | 🟡 | plain notice instead of info overlay; `delete`, `pin` subcommands missing |
| /jobs [full\|kill] | builtin-session.ts, jobs-panel.ts | one-line-per-job notice | 🟡 | no jobs panel/job tree, no `full`, no `kill` |
| /usage [show\|reset] | builtin-session.ts, usage-dashboard.ts | token/cost notice | 🟡 | **different feature**: omp shows provider rate-limit usage/quotas per credential + reset selector; omc prints session token totals |
| /stats | builtin-session.ts, packages/stats | — | ❌ | local stats dashboard (HTTP server + web UI over session files) |
| /changelog [full\|last N] | builtin-session.ts | — | ❌ | needs an embedded CHANGELOG |
| /hotkeys | builtin-session.ts | `ui_overlay_hotkeys` | ✅ | |
| /tools | builtin-session.ts, command-controller `handleToolsCommand` | notice | 🟡 | omp shows an "Available Tools" markdown panel incl. xd:// tools |
| /context | builtin-session.ts, overlays/context-viewer*.ts | one-line notice | 🟡 | omp opens a fullscreen context viewer (breakdown by category, browse content) |
| /extensions (status) | builtin-session.ts | — | ❌ | Extension Control Center (skills/rules/MCP/hooks/tools inventory; JS extensions part ⛔) |
| /agents | builtin-session.ts, overlays/agents-hub.ts | — | ❌ | per-agent model/prewalk/advisor hub |
| /git | builtin-session.ts | — | ❌ | git UI (split diff, staging, commit composer) |
| /local | builtin-session.ts | — | ❌ | open a `local://` file in $EDITOR |
| /hub (subagents) | builtin-session.ts, overlays/agent-hub*.ts | — | ❌ | live Agent Hub for subagents |
| /branch (rewind) | builtin-session.ts | `handle_command` "branch" picker | 🟡 | `/rewind` alias missing; generic select list instead of omp's user-message selector |
| /fork [pane\|window\|tab] | builtin-session.ts | `app_fork_from` | 🟡 | no multiplexer placement subcommands; no streaming guard |
| /tree | builtin-session.ts, overlays/tree-selector.ts | generic picker | 🟡 | flat indented list; omp's tree selector has branch glyphs, filters (`treeFilterMode`), labels |
| /login, /logout | builtin-session.ts | `ui_overlay_login` | ✅ | |
| /mcp [sub] | builtin-session.ts, mcp-command-controller.ts | status notice | 🟡 | 17 subcommands (add wizard, list, remove, test, reauth, unauth, enable, disable, reconnect, reload, resources, prompts, notifications, smithery-*) missing |
| /ssh [add\|list\|remove] | builtin-lifecycle.ts, ssh-command-controller.ts | — | ❌ | ssh.json host management (ssh:// reads work) |
| /new | builtin-lifecycle.ts | `app_new_session` | ✅ | |
| /fresh | builtin-lifecycle.ts | — | ❌ | reset provider stream state (Codex/Cursor sessions) |
| /clear | builtin-lifecycle.ts | `app_clear_context` | ✅ | |
| /delete | builtin-lifecycle.ts | unlink + new | ✅ | (omp also errors "Nothing to delete (in-memory session)"; omc silently starts a new session) |
| /compact [soft\|remote\|snapcompact] [focus] | builtin-lifecycle.ts, session/compact-modes.ts | `app_compact` | 🟡 | modes missing (**bug: `/compact soft` sends "soft" as summary instructions**); follow-up-key `/compact` while streaming waits for the yield (WP-2); no "Tokens: a -> b (saved n)" summary |
| /shake [elide\|images\|thinking] | builtin-lifecycle.ts, shake-types.ts | `app_shake(n)` | 🟡 | takes a numeric threshold instead of the 3 modes (**bug: `/shake images` → `atoi` = threshold 0**); summary text differs |
| /handoff [focus] | builtin-lifecycle.ts, command-controller `handleHandoffCommand` | `app_handoff` | 🟡 | focus instructions ignored (guards and the cancellable loader landed in WP-2) |
| /resume [id] | builtin-lifecycle.ts | `ui_overlay_session` | 🟡 | argument (session id/path) ignored |
| /pin | builtin-lifecycle.ts | — (Alt+K in picker only) | ❌ | |
| /btw [q] | builtin-lifecycle.ts, btw-controller.ts, overlays/btw-panel.ts | `src/ui/btw.c` | ✅ | inline streaming panel + history browser ported; image follow-ups missing |
| /tan | builtin-lifecycle.ts, tan-command-controller.ts | — | ❌ | background agent for tangential work |
| /omfg | builtin-lifecycle.ts, omfg-controller.ts | — | ❌ | forge a TTSR rule from a complaint |
| /cleanse | builtin-lifecycle.ts, cleanse-command-controller.ts | — | ❌ | diagnostics fixing with parallel subagents |
| /retry | builtin-lifecycle.ts | `retry_main` | ✅ | |
| /debug | builtin-lifecycle.ts | — | ❌ | debug tools selector |
| /memory | builtin-lifecycle.ts | `memory_command` | ✅ | local backend; `mm *` only for hindsight (❌ backend) |
| /rename [title] | builtin-lifecycle.ts | `handle_command` | ✅ | |
| /move <dir> | builtin-lifecycle.ts | — | ❌ | move session to another cwd |
| /wt (worktree) | builtin-lifecycle.ts, session/session-worktree.ts | — | ❌ | move session into a new git worktree |
| /add-dir, /remove-dir | builtin-lifecycle.ts | — | ❌ | multi-root workspaces |
| /dirs | builtin-lifecycle.ts | prints cwd | 🟡 | single root only |
| /exit, /quit (q) | builtin-lifecycle.ts, builtin-control.ts | `tui_stop` | 🟡 | `/q` alias missing |
| /restart | builtin-lifecycle.ts | — | ❌ | re-exec with same flags, resume session |
| /marketplace, /plugins (plugin), /reload-plugins | builtin-marketplace.ts | — | ❌ | plugin sources/install (git-based, portable for skills/commands/MCP/agents); JS extension parts ⛔ |
| /skills [search\|install\|installed\|update] | builtin-skills.ts | lists discovered skills | 🟡 | **different feature**: omp searches/installs from the skills.omp.sh registry |
| /force <tool> (force:) | builtin-control.ts | — | ❌ | force next turn's tool_choice |
| /live | builtin-control.ts | — | ⛔ | Codex realtime voice (audio capture/playback) |
| /record | builtin-control.ts | — | ❌ | record screen to replayable file (`omp play`) |
| /pause | builtin-control.ts, overlays/pause-screen.ts | — | ❌ | freeze all agents |
| /forget-approvals | builtin-control.ts | — | ❌ | depends on the approval system (`tools.approval`), also missing |

Non-builtin command sources:

| Source | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| Custom prompt commands (`commands/*.md` in .omp/.claude/.opencode, user + project; `$ARGUMENTS`) | capability/slash-command.ts, extensibility/custom-commands, discovery/* | — | ❌ | `commands.enable*` settings unread; omc only copies them in `migrate.c` |
| Skill commands `/skill:<name>` | modes/skill-command.ts, extensibility/skills.ts | — | ❌ | **RPC `get_available_commands` advertises `skill:<name>` entries but nothing expands them** (`src/modes/rpc.c`) |
| MCP prompts as commands | mcp/* | — | ❌ | |
| Extension/plugin commands (JS) | extensibility/extensions | — | ⛔ | Bun/JS runtime |

## 2. Keybindings and interactive flows

omc's key table (`src/tui/keys.c` `DEFAULTS[]`) has only the 31 `tui.*` editor/select actions. The 45
`app.*` actions of `packages/tui/src/app-keybindings.ts` are not in it; eight keys are hard-coded in
`key_hook` (`src/modes/interactive.c`): esc, ctrl+q/ctrl+enter, alt+up/shift+up, ctrl+c, ctrl+d, shift+tab, ctrl+o, ctrl+t, alt+m.
`tui_keybindings_load_file` exists but is never called, so **user keybinding overrides are ignored**.
The `/hotkeys` page (`hotkeys_markdown`, `src/ui/overlays.c`) is a static copy of omp's and lists ~20 keys
omc does not bind. `docs/tui.md` claims "Ctrl+Z suspends" and that the table includes app actions;
both are false. Rows marked (tmux) were probed in tmux against the mock model.

### 2a. Editor / input / select actions (`packages/tui/src/keybindings.ts`)

| Action (keys) | omc | Status | Notes |
|---|---|---|---|
| cursor up/down/left/right, word left/right, line start/end, pageUp/Down | `src/tui/editor.c` | ✅ | history on first/last line works |
| jumpForward (ctrl+]) / jumpBackward (ctrl+alt+]) | defined in keys.c, not handled | ❌ | (tmux) ctrl+] then `t` inserts `t` |
| deleteChar/Word Backward/Forward, deleteToLineStart/End | editor.c | ✅ | |
| yank (ctrl+y) | single kill slot | 🟡 | consecutive kills not merged |
| yankPop (alt+y) | — | ❌ | no kill ring (`kill-ring.ts`) |
| undo (ctrl+-, ctrl+_) | editor.c | ✅ | |
| spellingSuggestions (ctrl+.) | — | ⛔ | macOS native spelling |
| input.newLine (shift+enter, ctrl+j) | editor.c | 🟡 | omc also maps alt+enter (`key_hook` no longer takes it as follow-up) |
| input.submit, input.tab, select.* | editor.c, components.c | ✅ | `/resume` picker ignores pageUp/pageDown |
| input.copy (ctrl+c with selection) | — | ❌ | editor has no selection |
| Vim mode (`tui.vimMode`, vim.ts) | — | ❌ | setting shown but inert |
| User overrides (keybindings.yml, legacy .json migration, profile inherit) | — | ❌ | `/migrate` does not copy keybindings either |

### 2b. App actions (`packages/tui/src/app-keybindings.ts`)

| Action (default keys) | omc | Status | Notes |
|---|---|---|---|
| app.interrupt (esc) | `iq_escape` (src/modes/iqueue.c) | 🟡 | cancels compaction/handoff, restores the queue to the editor and aborts a turn, cancels `!`/`$` runs, clears bash/python mode (WP-2); **no double-Esc rewind/tree** (`doubleEscapeAction`, WP-5) |
| app.clear (ctrl+c) | key_hook | 🟡 | omp: clear, 2nd press within 500 ms exits, never aborts; omc aborts a running turn, 1.5 s window + "Press ctrl+c again" notice |
| app.exit (ctrl+d) | key_hook | 🟡 | draft not saved for resume |
| app.suspend (ctrl+z) | — | ❌ | `tui_suspend` exists (render.c) but ISIG is off and the key is unbound |
| app.display.reset (alt+l) | — | ❌ | |
| app.thinking.cycle (shift+tab) | key_hook | ✅ | no "model does not support thinking" status |
| app.thinking.toggle (ctrl+t) | key_hook | 🟡 | no status line; `hideThinkingBlock` not saved |
| app.model.cycleForward/Backward (ctrl+p / shift+ctrl+p) | — | ❌ | role cycle + cycle track |
| app.model.select (alt+m) | `ui_overlay_model` | ✅ | |
| app.model.selectTemporary (alt+p) | — | ❌ | |
| app.tools.expand (ctrl+o) | key_hook | ✅ | |
| app.tools.toggleVisibility (ctrl+shift+o) | — | ❌ | |
| app.ask.hide (alt+h) | ask.c | ✅ | hard-coded |
| app.approval.approve (ctrl+enter, ctrl+q) | — | ❌ | no approval dialog |
| app.editor.external (ctrl+g) | — | ❌ | ask "Other" prompt shows a ctrl+g hint that does nothing |
| app.local.open (alt+g) | — | ❌ | |
| app.message.followUp (ctrl+q, ctrl+enter) | `iq_follow_up` | ✅ | WP-2; `/compact` with it while streaming waits for the yield |
| app.retry (f5, alt+r) | `/retry` only | 🟡 | |
| app.message.dequeue (alt+up, shift+up) | `iq_dequeue` | ✅ | WP-2 |
| app.clipboard.pasteImage (ctrl+v), pasteTextRaw (ctrl+shift+v) | — | ❌ | no clipboard read, no image attachments |
| app.clipboard.copyLine (alt+shift+l), copyPrompt (alt+shift+c) | — | ❌ | OSC 52 helper exists |
| app.agents.hub (alt+a), app.session.observe (ctrl+s) | — | ❌ | no agent hub |
| app.session.new/tree/fork/resume (unbound by default) | slash commands | 🟡 | not bindable without overrides |
| app.session.pin/unpin (alt+k / alt+shift+k), rename/delete in picker | `src/ui/sessions.c` | ✅ | |
| app.tree.previousBranch/nextBranch | — | ❌ | `/tree` is a flat list |
| app.plan.toggle (alt+shift+p) | `key_hook` → `/plan` | ✅ | |
| app.history.search (ctrl+r) | — | ❌ | |
| app.links.hint (alt+o) | — | ⛔ | extension runtime |
| app.stt.pushToTalk, app.stt.toggle, app.live.toggle | — | ⛔ | audio stack |

### 2c. Interactive flows (`modes/interactive-mode.ts`, `modes/controllers/*`)

| Flow | omc | Status | Notes |
|---|---|---|---|
| Plan approval (`xd://propose` → PlanReviewOverlay) | `src/ui/planreview.c` + `ui_plan_approval` | ✅ | cell-identical overlay (tmux), all branches; mouse ❌ |
| Plan-mode model role switch/restore (`modelRoles.plan`) | `app_plan_apply_model` / exit restore | ✅ | deferred switch while streaming ❌ |
| /plan toggle + read-only gating | `/plan`, `hook_before_tool` | ✅ | |
| Submit / steer while streaming | `iq_submit` | ✅ | WP-2: steering (user-interjection envelope), empty Enter interrupts a turn with queued input, `->`/`=>`/`/queue` list splitting, input typed during compaction queued behind it |
| Pending-messages view (`updatePendingMessagesDisplay`) | `src/ui/queue.c` | ✅ | WP-2: `Steering · N` / `After yield · N` band, cell-identical (tmux) |
| **Delivered steer/follow-up shown as a user message** | `iq_user_message_start` | ✅ | WP-2; steeringMode/followUpMode one-at-a-time vs all, interruptMode immediate vs wait |
| Esc restores queue to editor | `iq_escape` | ✅ | WP-2 |
| `!cmd` / `!!cmd` | `iqueue.c` worker | ✅ | WP-2: live output, `Running… (⎋ to cancel)`, Esc cancel, "already running" warning, deferred below the transcript while streaming and recorded after the run |
| `$code` / `$$code` user eval | `iqueue.c` → eval kernel | 🟡 | WP-2: runs in the eval tool's kernel off the UI thread, Esc cancel, pythonExecution messages; output appears when the cell ends (omp streams it live); omp's pasted-shell-prompt exception not ported |
| /compact, /handoff | `iqueue.c` workers | ✅ | WP-2: omp's loaders, Esc → `Compaction cancelled` / `Handoff cancelled`, input queued behind them, Enter `/compact` mid-run aborts, compacts and resumes (auto-continue) |
| `@` file mentions | `complete_path` | 🟡 | single-directory prefix listing; omp does fuzzy project-wide search |
| `#` prompt actions, `#123` → pr:// / issue:// | — | ❌ | advertised in `/hotkeys` |
| `^model` mentions, `:emoji` completion, internal-URL completion | — | ❌ | |
| `/skill:<name>` invocation + completion | — | ❌ | |
| Bare `exit`/`quit`/`q` on an empty session | — | ❌ | sent to the model |
| Large paste | `[Paste #n …]` marker | 🟡 | no attachment chip / attach-as-file menu |
| Image paste, image-path paste | — | ❌ | |
| Todo reminders, todo auto-clear, live todo panel | — | ❌ | |
| Auto-compaction loader, retry countdown, TTSR notice | string-matched notices → working row | ✅ | |
| Retry fallback events | — | ❌ | |
| Smooth streaming / tool-args reveal | `chat.c`, `ui_apply_partial` | ✅ | |
| Session title generation | `src/coding/title.c` | ✅ | |
| **Terminal title (OSC 0)** | `tui_term_set_title` never called | ❌ | |
| **Turn-end / error / ask desktop notification** | — | ❌ | `completion.notify`/`ask.notify` default on |
| Composer predictions | — | ❌ | default off |
| /btw panel (branch/copy/follow-up keys) | `src/ui/btw.c` | ✅ | |
| Session focus (view a subagent, ←←) | — | ❌ | |
| Extension UI (hook selector/input/editor/widgets) | — | ⛔ | JS extension runtime |
| Abort marker after Esc | — | 🟡 | no "aborted" line seen; not compared against omp |

## 3. Settings

Method: every `path` in `assets/tui/settings-ui.json` (omp's own settings registry, 402 entries shown
in `/settings`) was searched as a string literal in `src/` outside the settings UI (`src/ui/settings.c`).
omc reads settings only by literal key (`settings_bool/str/int/num/get("key")`), so a key that never
appears is shown and saved by `/settings` but **has no effect**. Result (main at 41f5226): **100 read, 302 never read**.

The `/settings` UI should either honour these or hide them. Rows are grouped by what the gap means for a
user who never opens `/settings` (default-on behaviour omc lacks) versus switches that do nothing.

### 3a. Default-on behaviour omc does not have (user sees a difference without touching settings)

| Setting (default) | What omp does by default | omc |
|---|---|---|
| `tools.approvalMode` (yolo) + `tools.approval` ({}) + `tools.restrictedFolders` | Approval tiers (always-ask, ask-and-remember, write, yolo), per-tool allow/prompt/deny, approval dialog | ❌ only ACP reads `approvalMode`; TUI never asks. Users who set "Always ask" get yolo silently |
| `magicKeywords.*` (on) | "ultrathink" / "orchestrate" / "workflow" / "jevify" in a prompt raise thinking and attach hidden notices (`modes/magic-keywords.ts`) | ❌ |
| `model.loopGuard.*`, `model.toolCallLoopGuard.enabled` (on) | detects repeated assistant output / identical tool-call loops and steers | ❌ |
| `todo.reminders` (on), `todo.remindersMax` (3), `todo.eager` | reminds the model of unfinished todos at stop; eager todo creation | ❌ |
| `completion.notify` / `ask.notify` (on), `error.notify` | desktop/terminal notification when a turn ends or ask waits | ❌ |
| `recap.enabled` (on), `recap.idleSeconds` (240) | idle recap message after inactivity | ❌ |
| `tui.titleState` (on), `tui.titleSpinner`, `terminal.programStatus` (on) | terminal title shows run state + spinner; OSC 7501 program status | ❌ |
| `compaction.methodOrder` ([remote, snapcompact, handoff, shake, soft]), `compaction.midTurnEnabled`, `compaction.asyncEnabled`, `compaction.supersedeReads`, `compaction.dropUseless` (all on) | multi-method compaction ladder, mid-turn and async compaction, stale-read supersession, uneventful-result elision | 🟡 omc has a single local summary compaction (see §7) |
| `display.collapseCompacted` (on) | compacted history collapses in the transcript | ❌ |
| `tui.renderMermaid`, `tui.renderSvg`, `tui.reactions` (on), `tui.autoGraph` (auto) | mermaid/SVG rendering, agent reactions, table charts | ❌ |
| `terminal.showImages` (on), `images.autoResize` (on), `images.describeForTextModels` (on) | inline images (kitty/iTerm/sixel), resize before send, describe images for text-only models | ❌ |
| `edit.recoverInlineEdits`, `edit.blockAutoGenerated` (on) | recovers edit payloads pasted inline; refuses to edit generated files | ❌ |
| `lsp.lazy`, `lsp.shared` (on) | lazy server start, shared servers across sessions | 🟡 servers start on first use like the default; not switchable; no cross-session sharing (`lsp.diagnosticsDeduplicate` is now read) |
| `shellMinimizer.enabled` (on) | compresses noisy shell output (`[raw output: artifact://N]`) | ❌ (also in status.md) |
| `tools.abortOnFabricatedResult` (on) | aborts a turn whose text fabricates a tool result | ❌ |
| `async.widgets` (on) | live job widgets above the editor | ❌ |
| `mcp.enableProjectConfig` (on), `mcp.renderMarkdownResults` (on) | project `.mcp.json` opt-out; markdown MCP results | 🟡 project config always loaded; not switchable |
| `skills.enableSkillCommands` (on), `commands.enableClaudeProject`/`enableOpencodeProject` (on) | `/skill:<name>` and file-based custom commands | ❌ (see §1) |
| `skillful` (on) | skills listed in system prompt (session-toggleable) | 🟡 always listed; not switchable |
| `plan.enabled` (on), `goal.statusInFooter` (on), `title.refreshOnReplan` (on) | plan mode can be disabled; goal in footer; retitle on replan | ❌ |
| `task.speculativeLaunch`, `task.completionProbe`, `task.softRequestBudget` (200) + notice | subagent launch/probing/budget behaviour | ❌ |
| `composer.recallClearedDrafts` (on), `input.bareExitOnEmptySession` (on), `emojiAutocomplete` (on), `doubleEscapeAction` (rewind), `autocompleteMaxVisible` (10), `paste.largeMenuThreshold` (100), `spelling.autocomplete` (auto) | editor conveniences (see §2) | ❌ |
| `features.unexpectedStopDetection` (mechanical) | detects/continues unexpected model stops | ❌ |
| `power.sleepPrevention` (idle) | prevents system sleep while running | ❌ (Linux: `systemd-inhibit`; macOS `caffeinate`) |
| `startup.checkUpdate` (on), `startup.changelogMode` (summary) | update check + changelog on start | ❌ |
| `providers.cacheWarming` (idle) | keeps prompt cache warm while idle | ❌ |
| `exa.enabled` (on), `fetch.enabled` (on), `archive.enabled`, `out.enabled`, `browser.enabled`, `ida.enabled` (on) | tool availability (see §4) | ❌ tools absent, switches inert |
| `eval.py`/`eval.js`/`eval.autoProvision`/`eval.tools.enabled` (on), `python.kernelMode` (session) | eval backends | 🟡 Python only, hard-wired |
| `statusLine.showHookStatus` (on) | hook statuses in the status line | ❌ (no hooks) |
| `advisor.evictStaleResults` (on) | advisor context hygiene | ❌ |
| `telemetry.otlpExportEnabled` (on, needs endpoint env) | OTLP export | ❌ |

### 3b. Switches that do nothing in omc (default off / opt-in; feature missing)

Appearance: `composer.tokenRate`, `images.blockImages`, `tui.resizeScrollback`, `tui.completeScrollback`,
`terminal.showProgress`, `tui.textSizing`, `tui.codexResetFireworks`, `tui.hyperlinks`, `tui.mouse`,
`tui.tight`, `display.shimmer`, `display.pinnedAgents`, `display.subagentLivePreview`,
`display.hideToolActivity`, `display.showTokenUsage`, `display.showTurnTime`, `display.cacheMissMarker`,
`display.advisorCollapsedLines`, `tui.imeSafeCursor`, `task.showResolvedModelBadge`.
Model: `modelRoleStorage`, `inlineToolDescriptors`, `includeWorkspaceTree`, `prewalk.*` (5),
`images.urls.*` (6), `expandThinkingBlocks`, `omitThinking`, `externalThinking`,
`providers.autoThinkingMaxEffort`, `minP`, `presencePenalty`, `repetitionPenalty`, `tier.anthropic`,
`tier.google`, `tier.subagent`, `tier.advisor`, `retry.waitForUsageReset`, `retry.modelFallback`,
`retry.fallbackOn`, `retry.rateLimitWalksChain`, `retry.usageAwareFallback`, `retry.usageReservePct`,
`retry.usageReservePolicy`, `retry.fallbackChains`, `retry.fallbackRevertPolicy`,
`providers.anthropic.serverSideFallback`, `advisor.syncBacklog`.
Interaction: `autoResume`, `startup.showSplash`, `update.channel`, `marketplace.autoUpdate`,
`session.worktreeScope`, `tui.vimMode`, `tui.vimModeDisplay`, `loop.mode`, `loop.conditionTimeoutMs`,
`input.bareSlashCommands`, `treeFilterMode`, `composer.predictions`, `spelling.typoDetection`/`autocorrect`
(macOS ⛔), `skills.registryUrl`.
Context: `contextPromotion.enabled`, `extendedContext`, `branchSummary.enabled`,
`compaction.experimentalContextManagement`, `compaction.handoffSaveToDisk`,
`compaction.remoteStreamingV2Enabled`, `compaction.idleEnabled`, `compaction.idleThresholdTokens`,
`compaction.idleTimeoutSeconds`, `snapcompact.*` (3), `tools.format`, `ttsr.judge`, `ttsr.contextMode`,
`ttsr.repeatMode`, `ttsr.repeatGap`.
Files/Shell: `edit.streamingAbort`, `edit.blackbox.enabled`, `edit.autoRepair.enabled`,
`read.renderMarkdown`, `read.toolResultPreview`, `read.summarize.prose`, `lsp.formatOnWrite`,
`bash.allowCompoundCommands`, `bash.gitGuard`, `bashInterceptor.enabled`, `bash.direnv`,
`shellMinimizer.sourceOutlineLevel`, `eval.workpool.freshAgents`, `eval.autoBackground.enabled`.
Tools: `speechgen.enabled`, `generate_image.enabled`, `ratchet.enabled`, `vault.enabled`,
`security.enabled`, `todo.remindersMax`, `tasks.todoClearDelay`,
`images.questionTimeoutMs`, `tools.speculativeExecution.*`, `async.printWake`,
`tools.xdevNotices`, `mcp.notifications`, `dev.autoqaPush.endpoint`.
Tasks: `task.isolation.*`, `isolation.backend`, `worktree.*` (10 total), `task.file`, `task.structured`,
`task.enableEffort`, `task.perCallModel`, `task.perCallModelAllow`, `task.enableLsp`, `task.maxRuntimeMs`,
`task.maxEffort`, `task.prewalk`, `plan.autosave`, `plan.autosaveDir`, `title.refreshAfterPrompts`,
`commands.enableClaudeUser`, `commands.enableOpencodeUser`.
Providers: `providers.maxInFlightRequests`, `providers.antigravityEndpoint`, `providers.fetch`,
`codexResets.autoRedeem`, `claudeResets.autoRedeem`, `searxng.endpoint`, `providers.fireworksTier`,
`providers.kimiApiFormat`, `providers.openaiWebsockets`, `providers.openaiLiveSteering`,
`providers.muse-code.storeResponses`, `providers.openai-codex.daybreak`, `providers.openrouterVariant`,
`provider.appendOnlyContext`, `providers.streamFirstEventTimeoutSeconds`,
`providers.streamIdleTimeoutSeconds`, `secrets.enabled`.

### 3c. Settings of out-of-scope subsystems (⛔, hide in `/settings` or label as unsupported)

`mnemopi.*` (17) and `hindsight.*` (9) and `sharpshooter.model` (memory backends needing an embedding
store / remote service; omc treats them as off), `collab.*` / `share.*` / `stream.serverUrl` (omp's hosted
relay/share/stream services), `browser.*` (11) and `computer.*` (2) (browser engine / native desktop
automation), `ida.*` (4) (IDA Pro + idalib), `stt.*`, `speech.*`, `tts.local*`, `live.voice` (audio
capture/playback and local ONNX models), `providers.tinyModelDevice/Dtype` (local ONNX inference),
`extensionHandlers.modeControl`, `extensionRenderers.assistantText` (JS extension runtime),
`eval.js` (Bun JS kernel).

## 4. Tools, `xd://` devices and internal URLs

Upstream registry: `BUILTIN_TOOLS` / `HIDDEN_TOOLS` in `packages/coding-agent/src/tools/index.ts` plus
`sdk.ts`. omc: `CTORS` in `src/tools/tools_common.c` plus `src/coding/app.c` (lsp, debug, checkpoint,
github, MCP). With every optional setting on, the mock model saw omc offer: read bash edit ast_grep
ast_edit debug eval glob grep find lsp checkpoint rewind task wait todo web_search write manage_skill
(+ github on main).

**Bug found by probing:**
- **`thinking: auto` turns thinking off.** `--thinking auto`, or `defaultThinkingLevel: auto`, fails
  `effort_parse` and leaves `eff = EFFORT_OFF` (`src/coding/app.c`, app setup). omp runs its
  auto-thinking classifier instead.

### 4a. Tools

| Tool | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| read | tools/read.ts | `src/tools/read.c` | ✅ | `fetch.enabled` ignored (URLs always readable) |
| write | tools/write.ts | `write_bash.c` | 🟡 | no device-only write: with an explicit tool list holding `read` but not `write`, xd:// isn't mounted so MCP tools stay top-level |
| edit | edit/* | `edit.c`, `patch.c`, `sloppy.c` | 🟡 | `edit.blockAutoGenerated` (default on), `edit.recoverInlineEdits`, `edit.modelVariants` missing; others in status.md |
| bash | tools/bash.ts, exec/* | `write_bash.c`, `shell.c` | 🟡 | `bash.direnv` (default auto), `bash.gitGuard`, `bashInterceptor`, shell minimizer missing |
| eval | tools/eval.ts | `eval.c`, `evalhost.c` | 🟡 | Python only; JS backend ⛔ (Bun); agent()/workpool/archive/ratchet preludes ❌; browser/computer preludes ⛔ |
| glob, grep | glob.ts, grep.ts | `grep_glob.c` | ✅ | result text ported on main (context lines, grouped tree, pages) |
| find | jfind/* | `find.c` | ✅ | jfind cascade with judge waves ported on main |
| ast_grep, ast_edit | ast-grep.ts, ast-edit.ts | `astgrep.c` | 🟡 | token-tree matcher, not tree-sitter |
| ask, todo, wait, goal | ask.ts, todo.ts, wait.ts, goals/ | `todo_ask.c`, `write_bash.c`, `goal.c` | ✅ | todo reminders missing (§7) |
| task | task/* | `task.c`, `agents.c` | 🟡 | no isolation/worktrees, `outputSchema`, `perCallModel`, `speculativeLaunch`, `completionProbe`, soft request budget |
| web_search | web/search/* (~30 providers) | `web.c` | 🟡 | 5 providers (parallel, brave, tavily, exa, duckduckgo); omp also searches through the user's logged-in providers (anthropic, openai, gemini, codex, xai, perplexity, kimi, zai, searxng …) |
| lsp | lsp/* | `src/lsp/lsp.c` | ✅ | ported on main (byte-identical text, tests/parity/lsp_parity.sh) |
| debug | debug.ts | `src/dap/` | ✅ | |
| checkpoint, rewind | checkpoint.ts | `app.c` | ✅ | |
| github | gh*.ts | `src/tools/gh_tool.c`, `gh.c` | ✅ | ported on main (gh CLI) |
| learn, manage_skill | learn.ts, manage-skill.ts | `autolearn.c` | 🟡 | `learn` only with the local memory backend |
| advise | advisor/advise-tool.ts | `advisor.c` | 🟡 | single advisor |
| security_scan | security-scan.ts | — | ❌ | `security.enabled` (off) |
| context_notes, new_context | context-notes.ts | — | ❌ | `compaction.experimentalContextManagement` (off) |
| retain, recall, reflect | memory-*.ts | — | ❌ | hindsight backend is HTTP (portable) |
| memory_edit | memory-edit.ts | — | ⛔ | mnemopi (on-device embeddings) |
| think | think.ts | — | ❌ | `externalThinking` (off) |
| yield | yield.ts | — | ❌ | only with `task.structured` (off) |
| generate_image | image-gen.ts | — | ❌ | remote APIs, portable (off by default) |
| tts | tts.ts | — | ❌/⛔ | xAI remote path portable; local Kokoro ⛔ |
| vibe_* | vibe.ts | — | ❌ | vibe mode |
| autoresearch tools | autoresearch/* | — | ❌ | portable, low priority |
| ida | ida.ts | — | ⛔ | needs IDA Pro / idalib |
| browser, computer | tools/browser, computer.ts | — | ⛔ | browser engine; native desktop control |
| MCP tools | mcp/tool-bridge.ts | `mcp_register_tools` | ✅ | |

### 4b. `xd://` devices

| Item | omc | Status | Notes |
|---|---|---|---|
| `read xd://` listing, `read xd://<tool>` docs + TS schema, `xd://<tool>/<topic>`, `write xd://<tool>` dispatch | `src/tools/xdev.c` | ✅ | |
| docs placement (`tools.xdevDocs`) | `xdev_prompt_docs` | ✅ | MCP route guidance block ❌; `tools.xdevNotices` ❌ |
| `xd://report_issue` | `record_issue` | 🟡 | local only; no `dev.autoqaPush.endpoint` push |
| `xd://resolve`, `xd://reject` | `write_bash.c` | 🟡 | ast_edit only; omp has a generic queueResolveHandler |
| `xd://propose` | `write_bash.c` → `planmode.c` propose handlers | ✅ | XdevDispatch details, review / auto-approve / plan-yolo handlers |

### 4c. Internal URL schemes (`internal-urls/router.ts`)

| Scheme | omc | Status | Notes |
|---|---|---|---|
| agent://, artifact://, local://, proc://, mcp://, xd:// | `agents.c`, `tools_common.c`, `vurl.c`, `read.c`, `xdev.c` | ✅ | |
| omp:// | `omc://` (`read_omc_docs`) | ✅ | renamed on purpose |
| history:// | `agents_history` | 🟡 | `history://current/full` missing |
| skill://, rule:// | `tool_resolve_internal_url` | 🟡 | no fallback to MCP-advertised `skill://` resources |
| memory:// | `memory_url_resolve` | 🟡 | local backend only |
| pr://, issue:// | `github.c` via gh (main) | ✅ | |
| ssh:// | `ssh.c` | 🟡 | grep over ssh:// missing |
| out:// | — | ❌ | `out.enabled` default on; write refuses it as an unknown URI-like target |
| cfg:// | — | ❌ | write refuses it |
| conflict:// | `:conflicts` selector only | 🟡 | `conflict://N` read/write missing |
| mr:// | — | ❌ | GitLab MRs via `glab` |
| attachment:// | — | ❌ | |
| security://, vault:// | — | ❌ | off by default |

## 5. Overlays, dialogs and command controllers

Upstream: `packages/tui/src/overlays/*`, `packages/coding-agent/src/modes/components/*`,
`packages/coding-agent/src/modes/controllers/*`. Probed in tmux against the mock model: `/session`,
`/context`, `/usage`, `/jobs`, `/todo`, `/tools` each print one inline notice; `/tree` opens a small
floating list; Esc-Esc and Ctrl+R do nothing although `/hotkeys` lists Ctrl+R.

### 5a. Overlays

| Overlay | Upstream | Opened by | omc | Status | Notes |
|---|---|---|---|---|---|
| plan-review-overlay + plan-toc | plan-review-overlay.ts (1943 lines), plan-toc.ts | `xd://propose`, `/plan-review` | `src/ui/planreview.c` | ✅ | sections, Contents sidebar, delete/undo, annotations, slider, copy, external editor; mouse ❌ |
| plan-save-overlay | plan-save-overlay.ts | "Save plan & quit" | `ui_plan_save_*` (`src/ui/planreview.c`) | ✅ | tiny-model filename suggestion |
| approval-dialog | approval-dialog.ts, tools/approval*.ts | tool call under `tools.approvalMode`/`tools.approval` | — (ACP only, `src/modes/acp.c:222`) | ❌ | `/hotkeys` lists "Approve a pending tool call" |
| ask-dialog | ask-dialog.ts | `ask` tool | `src/ui/ask.c` | ✅ | cell-verified |
| context-viewer (+format, overview) | context-viewer*.ts | `/context` | notice | 🟡 | two-line notice; its total contradicts its own system/tools estimate line |
| copy-selector + copy-targets | copy-selector.ts, copy-targets.ts | `/copy [code\|cmd\|link]` | last answer via OSC 52 | 🟡 | no picker, no targets; silent when nothing to copy |
| tree-selector | tree-selector.ts (1794) | `/tree` | `show_picker(5)` | 🟡 | 14-row list; no filters, labels, search, fold, non-message entries |
| rewind-selector | rewind-selector.ts (1131) | Esc-Esc, `/branch`, `/rewind` | `/branch` generic picker | 🟡 | no transcript outline / sibling strip; Esc-Esc unbound; `/rewind` alias missing |
| jobs-panel + job-tree | jobs-panel.ts, job-tree.ts | `/jobs`, jobs pill | notice | 🟡 | no tree, `Running:` count, `full`, `kill`, detail pane |
| btw-panel, btw-history(-panel) | btw-*.ts | `/btw` | `src/ui/btw.c` | ✅ | no image paste in follow-ups |
| session-info-overlay | session-info-overlay.ts | `/session` | notice | 🟡 | missing provider/auth section, tool calls, edit rejections, LSP/MCP servers; `delete`/`pin` subs |
| usage-dashboard / usage-display / usage-row | usage-*.ts | `/usage`; per-turn row (`display.showTokenUsage`) | token totals notice | 🟡/❌ | different feature (provider quotas + heatmap); no provider usage fetchers in `src/`; per-turn row ❌ |
| reset-usage-selector | reset-usage-selector.ts | `/usage reset` | — | ❌ | |
| history-search | history-search.ts | Ctrl+R | — | ❌ | history already loaded (`history_load`) |
| mcp-add-wizard | mcp-add-wizard.ts (1912) | `/mcp add` | — | ❌ | |
| theme-selector | theme-selector.ts | settings theme submenu | `src/ui/settings.c` | 🟡 | live preview ok; no swatches/current dot/"Apply ⏎"; `ui_overlay_theme` declared in `overlays.h` but never defined |
| agent-hub, agent-activity, session-observer-registry | agent-hub*.ts | `/hub` | — | ❌ | subagents exist but no roster/chat/revive/kill |
| agents-hub | agents-hub.ts | `/agents` | — | ❌ | |
| agent-transcript-viewer | agent-transcript-viewer.ts | agent hub | — | ❌ | |
| running-subagent-badge | running-subagent-badge.ts | status line | `statusline.c` `subagents` segment returns NULL | ❌ | |
| report-panel | report-panel.ts | `/tools`, `/jobs full`, `/advisor status`, `/changelog`, `/hotkeys`, `/memory` | `ui_show_report` (`src/ui/overlays.c`) | 🟡 | always fullscreen (omp docks short reports inline); only `/hotkeys`, `/memory` use it |
| pause-screen | pause-screen.ts | `/pause` | — | ❌ | |
| error-banner | error-banner.ts | provider error | `ui_error_banner_show` (`src/ui/chat.c`) | ✅ | |
| cleanse-panel, omfg-panel | *.ts | `/cleanse`, `/omfg` | — | ❌ | TTSR engine exists (`src/coding/ttsr.c`) |
| hook-selector / hook-input / hook-editor | hook-*.ts | extension `ctx.ui.*`; internal confirms | — | ⛔/❌ | ⛔ as extension API; internal uses (`/move`, `/modelpreset`, skill install) ❌ with their commands |
| bordered-loader | bordered-loader.ts | `/share`, hook UI | — | ⛔ | used only by out-of-scope flows |
| plugin-selector | plugin-selector.ts | `/marketplace`, `/plugins` | — | ❌ | |
| plugin-settings | plugin-settings.ts | `/settings` Plugins tab | `settings.c` | 🟡 | always "No plugins installed" even with discovered Claude plugins; suggests `omc plugin install`, which does not exist |
| advisor-config | advisor-config.ts | `/advisor configure` | — | ❌ | |
| model-hub, hub-frame, model-browser | model-hub.ts … | `/model`, Alt+M | `src/ui/modelhub.c`, `modelbrowser.c` | ✅/🟡 | gaps in status.md (presets, fallback chains, custom roles, F5, mouse) |
| model-picker | model-picker.ts | Alt+P, `/switch` | — | ❌ | Alt+P listed in `/hotkeys`, unbound |
| thinking-selector | thinking-selector.ts | `/thinking`, `/effort` | `ui_overlay_thinking` | ✅ | |
| settings-selector | settings-selector.ts | `/settings` | `src/ui/settings.c` | ✅ | most settings inert (§3) |
| session-selector | session-selector.ts | `/resume` | `src/ui/sessions.c` | ✅ | |
| oauth-selector, login-dialog | *.ts | `/login`, `/logout` | `src/ui/login.c` | ✅ | |
| logout-account-selector, session-account-selector | *.ts | multi-account `/logout`, `/session` | — | ❌ | omc stores one credential per provider |
| move-overlay | move-overlay.ts | `/move` | — | ❌ | |
| composer-shape-preview | composer-shape-preview.ts | settings, setup | `ui_composer_preview` | ✅ | |
| snapcompact-shape-preview | snapcompact-shape-preview.ts | settings | — | ❌ | |
| codex-reset-fireworks | codex-reset-fireworks.ts | Codex quota reset | — | ❌ | low impact; needs usage reports |
| extensions/* dashboard | overlays/extensions/*, components/extensions/* | `/extensions` | — | ❌ | MCP/skills/tools parts portable; JS-extension rows ⛔ |
| annotation-overlay | annotation-overlay.ts | `/annotate` (bundled TS custom command) | — | ⛔ | JS runtime |
| stats-notice | stats-notice.ts | `/stats` | — | ⛔ | omp-stats web dashboard |
| show-images-selector, queue-mode-selector, composer-shape-registry | *.ts | not reachable upstream / extension-only | — | ⛔ | dead code or extension API |

### 5b. Command controllers

| Handler | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| handleSessionCommand / handleContextCommand / handleUsageCommand / handleJobsCommand / handleToolsCommand | command-controller.ts | `handle_command` notices | 🟡 | see overlays above |
| handleHotkeysCommand | command-controller.ts | `ui_overlay_hotkeys` | ✅ | advertises keys omc does not bind: Ctrl+R, Ctrl+P, Alt+P, Alt+Shift+P, Ctrl+Shift+O, Alt+O, approve |
| handleExportCommand | command-controller.ts | `/export` | 🟡 | placeholder HTML (see §1); no open-in-browser |
| handleDumpCommand / All / Anon | command-controller.ts | `/dump` | 🟡 | no LLM-request JSON sidecar; no `all`/`anon` |
| handleAdvisorStatus/Dump, showAdvisorConfigure | command-controller.ts | `/advisor` | 🟡 | **bug: any argument other than on/off toggles the advisor** (`/advisor status` switches it) |
| handleDebugTranscriptCommand / showDebugSelector | command-controller.ts | — | ❌ | |
| handleShareCommand, handleTraceCommand | command-controller.ts | — | ⛔ | hosted share / stats web dashboard |
| handleChangelogCommand | command-controller.ts | — | ❌ | |
| handleMemoryCommand | command-controller.ts | `/memory` | ✅ | |
| handleResetContextCommand | command-controller.ts | `/clear` | ✅ | wording differs ("Context reset — N messages dropped; session continues.") |
| handleFreshCommand | command-controller.ts | — | ❌ | |
| handleForkCommand | command-controller.ts | `/fork` | 🟡 | no pane/window placement, no streaming guard |
| handleMoveCommand, handleWorktreeCommand | command-controller.ts | — | ❌ | |
| handleRenameCommand | command-controller.ts | `/rename` | ✅ | |
| handleBashCommand | command-controller.ts | `iqueue.c` `bang` | ✅ | WP-2 (PTY input forwarding for interactive commands ❌) |
| handlePythonCommand | command-controller.ts | `iqueue.c` `python` | 🟡 | WP-2; no live output streaming |
| handleCompactCommand | command-controller.ts | `/compact` | 🟡 | **bug: `/compact soft` sends "soft" as summary instructions**; loader + Esc cancel done (WP-2) |
| handleShakeCommand | command-controller.ts | `app_shake(atoi(arg))` | 🟡 | **bug: `/shake images` → threshold 0**; no transcript rebuild |
| handleHandoffCommand | command-controller.ts | `/handoff` | 🟡 | custom instructions dropped |
| mcp-command-controller | mcp-command-controller.ts (2343) | `mcp_status` | 🟡 | every subcommand missing; `mcp_oauth_login/logout` exist in `src/mcp/oauth.c` |
| ssh-command-controller | ssh-command-controller.ts | — | ❌ | `src/tools/ssh.c` already reads `ssh.json` |
| todo-command-controller | todo-command-controller.ts | read-only notice | 🟡 | all subcommands missing |
| live-command-controller | live-command-controller.ts | — | ⛔ | realtime voice audio I/O |
| cleanse / omfg / tan controllers | *.ts | — | ❌ | |
| tool-call-block | tool-call-block.ts | `toolcard_start` | 🟡 | `out://` reply-write block missing (no `out://` scheme in omc) |

## 6. RPC, ACP and CLI

Read `src/modes/rpc.c`, `acp.c`, `print.c`, `src/cli/cli.c`, `agent_event_to_json` (`src/agent/agent.c`);
probed `build/omc` in RPC (JSONL), ACP (fifo) and every flag/subcommand below against the mock model.
U = `packages/coding-agent/src`. status.md marks RPC and ACP ✅; both are 🟡 on wire shapes.

### 6a. RPC commands (U/modes/rpc/rpc-types.ts, rpc-mode.ts; omc `src/modes/rpc.c:handle`)

| Command | omc | Status | Notes |
|---|---|---|---|
| negotiate_protocol | handle | 🟡 | upstream accepts only v2 (chunked frames) and replies `{protocolVersion:2}`; omc accepts any, replies 1 |
| prompt | prompt_thread | 🟡 | **slash commands not executed** (`/compact` goes to the model); no `agentInvoked:false` path; errors lack `httpStatus`/`code`, `retryable` always false |
| steer, follow_up, promote_queued_message, abort_and_prompt | handle | ✅ | |
| remove_queued_message, abort_and_restore_queue | handle | 🟡 | text only (no images/`imagesDropped`/`truncated`); `queue` not checked |
| abort | agent_abort | ✅ | |
| new_session / switch_session / fork | app_* | 🟡 | return full state instead of `{cancelled}`; ignore `parentSession` / provider+modelId; no `session_busy` |
| open_session | alias of switch_session | 🟡 | upstream takes `sessionDir` and returns `{cancelled,resumed,sessionId,sessionFile}` |
| branch | alias of fork | 🟡 | upstream branches in place and returns `{text,cancelled}`; omc forks to a new file |
| get_state | state_json | 🟡 | missing autoCompactionEnabled, fast/slow mode, usageLimit, tokensPerSecond, queuedMessages(+Count), hasPendingAsyncWork, isSettled, todoPhases, systemPrompt, dumpTools, contextUsage; partial `model` |
| set_fast_mode / set_slow_mode / set_cache_warming / set_ask_dialog / get_session_lease | — | ❌ | "unknown command" |
| goal, get_tree, set_thinking_level, get_available_thinking_levels, set_steering/follow_up/interrupt_mode, set_auto_compaction/retry, export_html, navigate_tree, set_label, get_branch_messages, get_last_assistant_text, get_messages, get_login_providers, host_tool_result | handle | ✅ | navigate_tree lacks error codes; summary synchronous |
| get_available_commands | handle | 🟡 | 11 hard-coded builtins + `skill:<name>` (not executable); no aliases/hints/subcommands |
| get_entries | handle | 🟡 | no `leafId`; `since` ignored |
| set_todos | handle | 🟡 | not persisted as a user-todo-edit entry |
| set_host_tools | handle | 🟡 | ignores `loadMode`, `readsSkillUris` |
| set_host_uri_schemes, host_uri_result | — | ❌ | |
| set_subagent_subscription, get_subagents, get_subagent_messages, cancel_subagent, steer_subagent | — | ❌ | |
| set_event_filter | handle | 🟡 | echoes `messageUpdates:"delta"` but never applies it |
| set_model / cycle_model / get_available_models | handle | 🟡 | `set_model` returns state not `Model`; model objects have 5 fields |
| cycle_thinking_level | handle | 🟡 | cycles non-reasoning models to "off" instead of returning null |
| compact | app_compact | 🟡 | no `CompactionResult`; refuses while busy |
| abort_retry | app_abort | 🟡 | aborts the whole run, not just the retry wait |
| bash / abort_bash | app_user_bash | 🟡 | **runs synchronously on the stdin reader thread**, so RPC input (incl. `abort_bash`) blocks until it ends |
| get_session_stats | app_stats | 🟡 | flat fields instead of `tokens{}`; missing toolCalls, totalMessages, premiumRequests, credits |
| abort_branch_summary | no-op | 🟡 | |
| set_session_name | session_set_title | 🟡 | accepts empty name |
| handoff | app_handoff | 🟡 | returns `{}` not `{savedPath}`; ignores `customInstructions` |
| get_messages_page | handle | 🟡 | upstream: oldest-first, opaque base64 cursor, byte cap, `{messages,nextCursor,totalMessages}`, `session_busy`; omc: newest-first numeric cursor, `{hasMore,total}` |
| login / get_logout_accounts / logout | — | ❌ | |
| btw | rpc.c btw_ask | ✅ | |
| btw_cancel / get_btw_history | rpc.c | ✅ | |
| extension_ui_response | — | ❌ | |
| host_tool_update | ignored | 🟡 | not forwarded as `tool_execution_update` |
| live_* / dictation_* / predict_word* | — | ⛔ | realtime voice, local STT, local tiny ML models |
| set_advisor | app_set_advisor | n/a | omc-only |

### 6b. RPC events and frames

| Event | Status | Notes |
|---|---|---|
| ready | 🟡 | `supportedProtocolVersions:[1]`, no `maxReassembledFrameBytes` |
| rpc_chunk (v2 framing) | ❌ | frames over 1 MiB written whole |
| agent_start, turn_start/end, tool_execution_start/update/end, host_tool_call/cancel, prompt_result, session_settled, notice | ✅ | no `resumedToolCallIds`; notice lacks `source` |
| message_start/update/end | 🟡 | **no `messageId`**; `assistantMessageEvent` lacks `partial` and start/done/error subtypes; delta projection missing |
| agent_end | 🟡 | `isTerminal`/`yielded` always true; no `awaitingAsyncWork` |
| tool_stream_update | ❌ | |
| auto/manual_compaction_start/end, auto_retry_start/end, retry_fallback_applied/succeeded, cache_warming_start/end | ❌ | |
| model_changed, thinking_level_changed, goal_updated, queue_update, config_warnings_changed, advisor_cost_changed, advisor_yielded, ttsr_triggered, todo_reminder, todo_auto_clear, irc_message | ❌ | |
| available_commands_update, session_info_update, config_update, command_output, extension_error | ❌ | |
| extension_ui_request (select/confirm/input/editor/ask/cancel/notify/setStatus/setWidget/setTitle/set_editor_text/open_url) | ❌ | **in RPC the `ask` tool returns "no interactive user"** (`src/tools/todo_ask.c`) |
| host_uri_request/cancel, subagent_lifecycle/progress/event | ❌ | |
| btw_delta / btw_record | ✅ | |
| packages/wire (collab GuestFrame/HostFrame, skillshare, stream, tsp) | ⛔ | hosted relay/stream protocols, not RPC |

### 6c. ACP (U/modes/acp/*, slash-commands/acp-builtins.ts; omc `src/modes/acp.c`)

| Method / update | Status | Notes |
|---|---|---|
| initialize | 🟡 | empty `authMethods`; no `sessionCapabilities.fork/resume/close`, no `_meta.steering`; ignores clientCapabilities |
| authenticate | 🟡 | accepts any `methodId` |
| session/new | 🟡 | ignores `mcpServers`; no `configOptions`; no bootstrap `available_commands_update`/`session_info_update` |
| session/load | 🟡 | replays text only (no tool calls); ignores `mcpServers` |
| session/list | 🟡 | ignores `cwd`/`cursor`, caps at 100, no `nextCursor` |
| session/resume, session/fork, session/close, session/set_config_option, _session/steering | ❌ | -32601; sessions leak until EOF |
| session/set_mode, session/cancel | ✅ | no `config_option_update` |
| session/prompt | 🟡 | **no slash commands**; model error becomes a JSON-RPC error instead of `end_turn`/`refusal` |
| _omp/sessions/listAll, _omp/projects/list, _omp/chats/byCwd, _omp/usage | ❌ | |
| _omp/extensions(/toggle), speech.models.list | ⛔ | JS extensions, speech |
| outbound session/request_permission | 🟡 | edit/execute tools only; option sets differ from session-tools.ts |
| outbound fs/read_text_file, fs/write_text_file, terminal/create, elicitation (ask) | ❌ | unsaved editor buffers and client terminals unused |
| agent_message_chunk, agent_thought_chunk, user_message_chunk, plan, current_mode_update | ✅ | |
| tool_call / tool_call_update | 🟡 | no move/delete/fetch kinds; diffs as text not `{type:"diff"}`; no `terminal` content; no in-progress updates |
| usage_update | 🟡 | **wrong shape**: sends message `usage`; ACP expects `{size,used,cost:{amount,currency}}` |
| available_commands_update, config_option_update, session_info_update | ❌ | |

### 6d. CLI flags (U/cli/args.ts, flag-tables.ts; omc `src/cli/cli.c:omc_main`)

| Flag | Status | Notes |
|---|---|---|
| `--flag=value` syntax | ❌ | `--model=x` → "unknown option"; omp's own help uses this form |
| --model, --provider, --api-key, --thinking, --cwd, --config, --session-dir, --system-prompt, --append-system-prompt, --tools, --no-tools, --no-session, --no-context-files, --no-skills, --no-rules, --no-title, --print-thoughts, -p/--print, -c/--continue, --fork, --goal, --mode text/json/rpc/acp, -h, -v, piped stdin | ✅ | |
| -r/--resume/--session | 🟡 | value required; omp opens a picker without one |
| @file arguments | 🟡 | always inlined as text; images not attached; quotes not stripped |
| --restore, --mode rpc-ui, --models, --export, --hide-thinking, --advisor, --service-tier, --system-prompt-template, --skills | ❌ | several have settings behind them already |
| --smol / --slow / --plan (role model shortcuts) | ❌ | omc's `--plan-mode` means something else; `-m @role` works |
| --auto-approve/--yolo, --approval-mode | ❌ | approval system missing |
| --max-time, --max-cost, --warn-cost | ❌ | omc-only `--max-turns` |
| --add-dir, --allow-home, --profile, --alias, --from-claude, --from-codex, --no-lsp, --no-pty, --wait-async, --external-thinking, --prewalk*, --provider-session-id, --prompt-cache-key | ❌ | |
| --hook, -e/--extension, --trusted-extension, --no-extensions, --plugin-dir, --no-ui | ⛔ | JS/Bun extension runtime |

### 6e. CLI subcommands (U/cli-commands.ts, U/commands/*)

Unknown subcommand-shaped words (`token`, `stats`, `commit`, `setup`, …) start the TUI with the word as
the prompt.

| Subcommand | Status | Notes |
|---|---|---|
| acp | ❌ | **`omc acp` starts an interactive session with prompt "acp"**; only `--mode acp` works (editor integrations call `omp acp`) |
| models | 🟡 | no ls/find/refresh/`<provider>`, `--json`, `--kind`; `--help` taken as a filter |
| login | 🟡 | no picker without a provider |
| config | 🟡 | no `init-xdg`, `--json`, `--scope project` |
| read | ✅ | |
| grep | 🟡 | no -g/-l/-C/-f/-c/--no-gitignore; `--help` taken as pattern |
| auth | 🟡 | upstream is export/import JSON; omc has set/list/remove |
| commit, completions/__complete, token, usage, setup, session (pack/unpack/moved/lease), worktree/wt, agents, ttsr, ssh, search/q/web-search | ❌ | |
| stats | ❌ | local dashboard server |
| gc, shell, ps, find, compress, toks, render, play, anonymize, images, grievances, git, cleanse, gallery, bench, if-bench, dry-balance, auth-broker, auth-gateway | ❌ | low impact |
| plugin/plugins/install, update | ⛔ | npm/Bun packages, self-update of the npm distribution |
| share, join, collab, clip, stream, skill/skills | ⛔ | hosted relay/stream/Skillshare services |
| say, predict, tiny-models, browser-relay, launch | ⛔ | local TTS/ML runtimes, Chrome CDP relay, internal |
| omc-only: mcp, migrate, logout, sessions, export, glob, bash, version, help | n/a | |

## 7. Compaction and background systems

### 7a. Compaction

| Item | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| Method ladder `compaction.methodOrder` (remote → snapcompact → handoff → shake → soft) | session/compaction-methods.ts | — | ❌ | omc always runs the summary ("soft") method |
| Summary compaction | agent/src/compaction/compaction.ts | `app_compact` | 🟡 | single pass, chars/4 estimate; previous-summary update and `modelRoles.compaction` ok; no split-turn handling, no per-model thresholds |
| Server-side compaction (OpenAI Responses, Anthropic, Bedrock) | compaction/{openai,anthropic,bedrock}.ts | — | ❌ | `provider_anthropic.c` only recognises the `compaction` stop reason |
| snapcompact | packages/snapcompact | — | ❌ | renders history to PNG frames for vision models; portable but large |
| handoff | session-maintenance.ts | `app_handoff` | 🟡 | manual only; not an automatic method; no `handoffSaveToDisk` |
| shake | agent/src/compaction/shake.ts | `app_shake` | 🟡 | manual only; drops images too; no fenced/XML regions, recent-tail protection, artifact offload; not persisted |
| Threshold trigger | compaction-threshold.ts | `app_compaction_threshold` | ✅ | |
| Mid-turn compaction (`midTurnEnabled`, on) | session-maintenance.ts | — | ❌ | auto-compaction runs only after a prompt |
| Async compaction (`asyncEnabled`, on) | session-maintenance.ts | — | ❌ | status-line marker only |
| Overflow recovery | session-maintenance.ts | `app.c` | ✅ | `contextPromotion` ❌ |
| Pruning (`supersedeReads`, `dropUseless`, on) | compaction/pruning.ts | — | ❌ | `agent.c` sets a `useless` flag nothing consumes |
| Idle compaction | idle-maintenance.ts | — | ❌ | off by default |
| Branch summary | compaction/branch-summarization.ts | `rpc.c` navigate only | 🟡 | ad-hoc prompt; no interactive /tree prompt |

### 7b. Background systems

| System | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| Prompt cache warmer (`providers.cacheWarming` idle) | session/cache-warmer.ts | — | ❌ | cost impact |
| Idle recap (`recap.enabled`, 240 s) | idle-maintenance.ts | — | ❌ | |
| Turn-end / ask / error notifications | event-controller.ts, tui/desktop-notify.ts | — | ❌ | default on |
| Terminal title + OSC 7501 program status | utils/run-status.ts, title-generator.ts | `tui_term_set_title` never called | ❌ | |
| Update check | main.ts | — | ❌ | npm self-update ⛔, but a release check is portable |
| Auto-thinking classifier | auto-thinking/classifier.ts | — | ❌ | plus the `auto` → off bug above |
| Todo reminders, unexpected-stop detection, tool-call loop redirect, fabricated-result abort, loop guard | session/todo-tracker.ts, unexpected-stop-classifier.ts, tool-call-loop-redirect.ts | — | ❌ | all default on |
| Magic keywords (ultrathink/orchestrate/workflow/jevify) | modes/magic-keywords.ts | — | ❌ | default on |
| Sleep prevention | power-assertion-backend.ts | — | ❌ | Linux: logind inhibitor |
| Session cost caps (`--max-cost`, warn) | session/settings.ts | — | ❌ | |
| Prewalk / auto-prewalk, speculation | session/prewalk.ts, speculation/ | — | ❌ | off by default |
| Telemetry OTLP export | telemetry-export*.ts | — | ❌ | only with `OTEL_*` env |
| Stats (packages/stats) | packages/stats | — | ❌ | aggregation portable; the bundled web dashboard is the large part |
| Custom markdown commands, skill commands | custom-commands, skill-command.ts | — | ❌ | §1 |
| Plugins / marketplace | extensibility/plugins | `plugins.c` (Claude plugin skills) | 🟡 | install/update, plugin MCP/commands ❌ |
| Worktrees (`/wt`, `task.isolation`) | session-worktree.ts | — | ❌ | |
| Job widgets / activity | job-widgets/, activity/ | — | ❌ | |
| Secrets obfuscation | secrets/ | — | ❌ | off by default |
| IRC / agent messaging, checkpoints | irc/bus.ts, checkpoint.ts | `agents.c`, `app.c` | ✅ | |
| Hooks (`.claude/hooks`, `.omp` hooks) | extensibility/hooks | — | ⛔ | omp loads only `.ts`/`.js` hook modules under Bun |
| Extensions, custom tools | extensibility/extensions, custom-tools | — | ⛔ | Bun/JS runtime |
| Collab / share / stream | collab/, packages/collab-web | — | ⛔ | hosted relay/blob service (secret-gist share is portable) |
| Tern native rendering | tools/tern-media.ts | — | ⛔ | external native host |
| STT, live voice, predict_word, local TTS | stt/, live/, predict/, tts/ | — | ⛔ | on-device ML / audio |
| Skillshare registry | skillshare/ | — | ⛔ | hosted registry |
