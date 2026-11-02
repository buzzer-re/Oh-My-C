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
4. ~~**No turn-end, ask or error notifications, and the terminal title never changes.**~~ Done (WP-11): completion / ask / error notifications per terminal, the `π > label` title with run state and spinner, OSC 7501, idle recap, sleep prevention.
5. ~~**The `/export` HTML is a placeholder; `/dump` lacks `all`/`anon` and the request-JSON sidecar.**~~ Done (WP-4): omp's HTML export document, `/dump` sidecar and `all`/`anon` archives.
6. ~~**`thinking: auto` turns thinking off.**~~ Done (WP-3): `auto` keeps a provisional level and classifies each user turn through the judge role chain like omp.
7. ~~**Slash-command argument bugs.**~~ Done (WP-4): shared registry, omp's argument parsing for `/advisor` `/compact` `/shake` `/handoff` `/resume` `/effort` `/delete` `/clear`, aliases `/models` `/rewind` `/q`, bare `exit`/`quit`/`q`. Aliases of commands omc lacks (`/providers`, `/status`, `/subagents`, `/worktree`, `/plugin`, `/force:`) arrive with those commands.
8. **Some bound app keys have no handler yet.** The keybinding layer is done (WP-5: all `app.*` actions,
   `keybindings.yml` overrides, `/hotkeys` from the live table), but these keys still do nothing:
   Ctrl+Shift+O. Like omp's, `/hotkeys` lists them. Ctrl+G, Ctrl+R and Ctrl+Shift+V are done (WP-6),
   Alt+P (session model picker) in WP-19, Alt+G (the `/local` picker) in WP-21. (WP-29)
9. ~~**`/context`, `/session`, `/tools`, `/jobs`, `/todo` are one-line notices instead of omp's panels.**~~ Done (WP-7): context viewer, Session Info overlay with `delete`/`pin`, Available Tools panel, Background Jobs report with `full`/`kill`, the `/todo` verbs, and short reports docked above the editor.
10. ~~**`/tree` and `/branch` are flat 14-row pickers.**~~ Done (WP-8): omp's session tree selector, fullscreen rewind selector and branch summaries.
11. ~~**Custom markdown commands and `/skill:<name>` don't work.**~~ Done (WP-9): file commands from every omp provider, prompt templates, `/skill:<name>` (leading or mid-prompt, chips, skill transcript card), MCP prompt commands, in the TUI, RPC, ACP and print mode. Remaining: the bundled TS commands `/review` and `/green`.
12. **304 of the 402 settings shown in `/settings` have no effect.** (`tools.approvalMode`, `tools.approval`, `tools.restrictedFolders` and `bash.patterns` now work: WP-10.) (WP-26)
13. ~~**Default-on agent guards are missing.**~~ Done (WP-12, WP-3): todo reminders + mid-run nudge, empty/unexpected-stop recovery, tool-call loop guard, stream loop guard, magic keywords, `out://`, `cfg://`, `conflict://N`. Remaining: fabricated-result abort (needs in-band tool calling), `todo.eager`, `tasks.todoClearDelay`.
14. **Compaction: snapcompact and Codex V2 / Bedrock server compaction are missing.** The method ladder, omp's summary requests, auto shake/handoff, OpenAI Responses and Anthropic server compaction, mid-turn/async compaction and pruning landed (WP-13); prompt-cache warming landed (WP-14); snapcompact is a follow-up. (WP-13)
15. **RPC wire parity landed with WP-15** (remaining: subagent frames, host URI schemes, §6a-b). ACP is done (WP-16): `omc acp`, the full session lifecycle, config options, the fs / terminal / elicitation bridges and history replay.

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
| WP-3 Model-facing correctness bugs ✅ (done: out://, cfg://, conflict://N, `thinking: auto`) | Port `out://`, `cfg://`, `conflict://N` (write now refuses unknown schemes; main 44b5fc3). Make `thinking: auto` stop meaning off: resolve it like omp, or to the model default until the WP-12 classifier lands. Verify with `tests/parity/request_diff.sh` / `tool_diff.sh` | `src/tools/write_bash.c`, `src/tools/read.c`, `src/tools/vurl.c`, `src/coding/app.c` (thinking setup), `src/ui/settings.c` | U/internal-urls/{router,out-protocol,cfg-protocol,conflict-protocol}.ts, U/tools/conflict-detect.ts, U/auto-thinking/* |
| WP-4 Slash-command correctness and shared registry (done: registry `src/coding/slashcmd.c` for TUI/RPC/ACP, argument fixes, aliases, bare exit, argument completion + hints, live descriptions; `/export` with omp's HTML document, `/dump` + sidecar + `all\|anon`, `/advisor dump`; remaining: `/advisor configure`, multi-advisor rosters, shake block regions/artifacts, the 46 builtins omc lacks) | One registry (name, aliases, subcommands, hints, handler) used by TUI, RPC and ACP. Fix `/advisor` (status/dump), `/compact [soft\|remote\|snapcompact] [focus]`, `/shake [elide\|images\|thinking]`, `/handoff [focus]`, `/resume <id>`, `/effort` errors, `/delete` on in-memory sessions. Add aliases (models, rewind, q, providers, status, subagents, worktree, plugin, force:). Bare `exit`/`quit`/`q`. `/export` with omp's HTML viewer (`export/html/template.*` embedded as assets). `/dump all\|anon` + request-JSON sidecar | `src/modes/interactive.c` (`handle_command`, `COMMANDS[]`), `src/ui/slash.c`, `src/coding/app.c`, `src/session/session.c` (`session_export_html`), `assets/` | U/slash-commands/builtin-*.ts, builtin-registry.ts, helpers/*, U/session/compact-modes.ts, shake-types.ts, session-dump-format.ts, U/export/html/* |
| WP-5 App keybinding layer (done; remaining: handlers for selectTemporary, local.open, tools.toggleVisibility, agent hub — owned by WP-19/21/22/29 (history.search, editor.external, pasteTextRaw done in WP-6); Alt+L background re-probe; draft saved on Ctrl+D) | All `app.*` actions in the key table; `keybindings.yml` (+ legacy .json migration, profile inherit); `/migrate` copies it; `key_hook` via `tui_kb()`; `/hotkeys` from the live bindings; Ctrl+Z, F5/Alt+R, Ctrl+C (500 ms, no abort), Ctrl+T status + persist, Ctrl+] jumps, Alt+Y kill ring, Alt+L, copy line/prompt, Esc-Esc → `doubleEscapeAction` | `src/tui/keys.c`, `src/config/keybindings.c`, `src/tui/editor.c`, `src/modes/interactive.c`, `src/ui/overlays.c` (`ui_hotkeys_markdown`) | T/app-keybindings.ts, T/keybindings.ts, T/kill-ring.ts, T/hotkeys-markdown.ts, U/modes/controllers/input-controller.ts |
| WP-7 Report panels ✅ (done: report docking, `/context` viewer, `/session` overlay + `delete`/`pin`, `/tools`, `/jobs [full\|kill]`, `/todo` verbs; remaining: tokenizer-exact counts (omc estimates ~4.1 chars/token), snapcompact savings lines, multi-account `/session pin` selector, LSP rows in Session Info, nested subagent job tree, native jobs sheet / detail pane, live todo HUD for `/todo expand\|collapse`, RPC/ACP text handlers (`handle`) for /todo /session /jobs /context /tools) | `/context` viewer (category breakdown, browse content); `/session` info overlay + `delete`/`pin`; `/tools` markdown panel incl. xd:// tools; `/jobs` panel + job tree + `full`/`kill`; `/todo` subcommands; `/advisor status\|dump`. Report-panel inline docking for short reports | `src/modes/interactive.c`, `src/ui/overlays.c` (`ui_show_report`), new `src/ui/context.c`, `src/tools/jobs.c`, `src/tools/todo_ask.c` | T/overlays/context-viewer*.ts, session-info-overlay.ts, jobs-panel.ts, job-tree.ts, report-panel.ts; U/modes/controllers/command-controller.ts, todo-command-controller.ts; U/slash-commands/helpers/context-report.ts |
| WP-8 Tree and rewind selectors ✅ (done; remaining: `ask` re-answer from the tree, Ctrl+G in the custom-instructions editor, model transcript dialects other than xml/anthropic in the summary request, rewind mouse clicks) | `/tree` tree selector (glyphs, filters / `treeFilterMode`, labels, search, fold, branch prev/next); rewind selector for `/branch`, `/rewind`, Esc-Esc; optional branch summary prompt (`branchSummary.enabled`) | `src/modes/interactive.c` ("tree"/"branch"), new `src/ui/tree.c`, `src/coding/app.c` | T/overlays/tree-selector.ts, rewind-selector.ts; U/modes/controllers/selector-controller.ts; A/compaction/branch-summarization.ts |
| WP-9 Custom and skill commands ✅ (done; remaining: bundled TS `/review`, `/green`; skill chip OSC 8 links) | Discover `commands/*.md` (omp/Claude/OpenCode, user + project, `commands.enable*`) with `$ARGUMENTS`/frontmatter. `/skill:<name>` invocation + completion (`skills.enableSkillCommands`). MCP prompts as commands. Autocomplete entries. Execution from RPC `prompt` / ACP `session/prompt` (uses the WP-4 registry) | `src/coding/discovery.c`, `src/modes/interactive.c`, `src/ui/slash.c`, `src/tui/editor.c`, `src/modes/rpc.c`, `src/modes/acp.c` | U/capability/slash-command.ts, U/extensibility/{custom-commands,skills.ts,slash-commands.ts}, U/modes/skill-command.ts, U/modes/prompt/skill-tokens.ts, U/discovery/{claude,opencode}.ts |

### P1 — common, or the default behaviour differs

| WP | Scope | omc files | Upstream sources |
|---|---|---|---|
| WP-6 Editor I/O (done; remaining: Ctrl+G with the ask option list focused opens the composer's editor in omp, `submitAfterPaste` skips the menu, Kitty thumbnails / video chips, the `^model` / `:emoji` / internal-URL completions of the same provider) | Ctrl+G external editor (main editor + ask "Other"); Ctrl+R history search; copy line/prompt (Alt+Shift+L/C); Ctrl+V image paste + image-path paste + attachments; raw paste; large-paste chip/menu; fuzzy project-wide `@` mentions; `#` prompt actions (`#123` → pr:// / issue://) | `src/tui/editor.c`, `src/modes/interactive.c`, `src/ui/ask.c`, `src/tui/term.c`, new `src/ui/histsearch.c` | U/modes/controllers/input-controller.ts, T/overlays/history-search.ts, U/modes/prompt/{attachment-chips,prompt-action-autocomplete}.ts, T/autocomplete.ts |
| WP-10 Tool approval system (done; remaining: `bash.allowCompoundCommands`, nested `xd://` prompts for an explicit device policy, provider safety checks, extension `tool_approval_*` events, Cursor-bridge approvals) | `tools.approvalMode` tiers (always-ask, always-ask-cached "Ask and remember", write, yolo), `tools.approval` per-tool policies, `tools.restrictedFolders`, approval dialog (Ctrl+Enter/Ctrl+Q), remembered grants, `/forget-approvals`, `--approval-mode` / `--auto-approve` / `--yolo` | `src/coding/app.c` (`hook_before_tool`), new `src/ui/approval.c`, `src/modes/acp.c`, `src/cli/cli.c` | U/tools/approval.ts, approval-cache.ts, approval-targets.ts, search-approval-targets.ts; T/overlays/approval-dialog.ts |
| WP-11 Turn-lifecycle signals — **done** (see status.md "Turn-lifecycle signals"); remaining: OSC 99 `p=?` probe and structured Kitty notifications, the OSC 133;A program-status resend (omc writes no OSC 133 zones), `terminal.showProgress` (OSC 9;4), TSP native titles, Tern OSC 1337/OSC 7 session reports, Windows console titles, the desktop ScreenSaver inhibit for `display`/`system`, recap journaling to history.db | Completion/ask/error notifications (OSC 9/777, `*.notify`); terminal title with run state + spinner (`tui.titleState`, `tui.titleSpinner`); OSC 7501 program status; idle recap (`recap.*`); sleep prevention (logind inhibitor); release check | `src/modes/interactive.c`, `src/tui/term.c` (`tui_term_set_title`), `src/coding/title.c`, `src/coding/app.c` | U/modes/controllers/event-controller.ts (`send*Notification`), T/desktop-notify.ts, U/utils/{run-status,title-generator}.ts, U/session/idle-maintenance.ts, U/session/power-assertion-backend.ts |
| WP-12 Default-on agent guards 🟡 (done: todo reminders + mid-run nudge, unexpected/empty-stop recovery, tool-call + stream loop guards, magic keywords, auto-thinking classifier; open: fabricated-result abort (in-band tool calling), `todo.eager`, `tasks.todoClearDelay` auto-clear, todo-error reminder) | Todo reminders + auto-clear (`todo.reminders`, `remindersMax`, `tasks.todoClearDelay`); unexpected-stop detection; tool-call loop guard / redirect and assistant loop guard (`model.loopGuard.*`); fabricated-result abort; magic keywords (ultrathink/orchestrate/workflow/jevify + hidden notices); auto-thinking classifier. Each must keep `request_diff.sh` clean | `src/coding/app.c` (turn end), `src/agent/agent.c`, `src/modes/interactive.c` (prompt pre-processing) | U/session/{todo-tracker,unexpected-stop-classifier,tool-call-loop-redirect}.ts, U/modes/magic-keywords.ts, U/prompts/system/*-notice.md, U/auto-thinking/* |
| WP-13 Compaction ladder ✅ (done: `compaction.methodOrder` ladder for manual and automatic passes, omp's prepareCompaction/compact() requests (split turns, short summary, file lists, input windows), all transcript dialects, auto shake (regions, tail protection, artifact offload, persisted) and auto handoff (`handoffSaveToDisk`), OpenAI Responses `/responses/compact` + replay, the Anthropic compaction beta + signed replay, mid-turn, pre-prompt and async (speculative) compaction, supersede/useless/tool-output pruning, dead-end rescue + warnings, per-model thresholds, `auto_compaction_*` events; remaining: snapcompact, Codex V2 streaming compaction, Bedrock Converse (InvokeModel) compaction, anthropic-native speculation, `compaction.remoteEndpoint`, idle compaction, context promotion) | `src/coding/compaction.c`, `src/ai/dialect.c`, `src/ai/provider_{anthropic,openai_responses}.c`, `src/session/session.c` | U/session/{compaction-methods,session-maintenance,compact-modes,session-handoff}.ts, A/compaction/{compaction,shake,pruning,openai,anthropic,bedrock}.ts, packages/ai/src/dialect/*, packages/snapcompact |
| WP-14 Provider knobs and cache warming ✅ (done; remaining: `tier.advisor` / `task.agentServiceTierOverrides`, the cache-warming extension hook, the Anthropic slow-mode sibling-account gate, `usage.cttl` tier adoption, the model hub's "Opens … window" confirmation) | Prompt cache warmer (`providers.cacheWarming`); service tiers `/fast`, `/slow`, `tier.*`, `--service-tier`; `/extended-context` + `extendedContext`; stream first-event/idle timeouts; `providers.maxInFlightRequests`; RPC `set_fast_mode` / `set_slow_mode` / `set_cache_warming` | `src/coding/app.c`, `src/ai/provider_*.c`, `src/ai/ai_common.c`, `src/modes/rpc.c` | U/session/cache-warmer.ts, anthropic-slow-mode.ts, model-controls.ts; U/slash-commands/builtin-modes.ts (fast/slow/extended-context) |
| WP-15 RPC wire parity (done: §6a-b; remaining: subagent frames, host URI schemes, session-lease command, prompt error `httpStatus`/`cost_cap`, `retryErrors`) | `messageId` + `partial` + start/done/error on message events, delta filter; session events (compaction, retry, model/thinking changed, goal_updated, queue_update, available_commands_update, …); response shapes (set_model, new/switch/open/branch/fork, get_entries leafId/since, get_messages_page cursor, get_session_stats, compact, handoff, btw); full `get_state`; `extension_ui_request`/`_response` so `ask` works; login/logout; async `bash` so `abort_bash` works; protocol v2 + `rpc_chunk`; subagent frames; mark status.md 🟡 | `src/modes/rpc.c`, `src/agent/agent.c` (`agent_event_to_json`), `src/coding/app.c` (structured events beside `notice`), `src/modes/print.c`, `src/tools/todo_ask.c` | U/modes/rpc/{rpc-types,rpc-mode,rpc-session-events,rpc-messages,rpc-compat,rpc-frame}.ts, U/session/agent-session-events.ts |
| WP-16 ACP parity ✅ (done, verified with tests/parity/acp_diff.py; remaining: permission-request withdrawal on steering, ask timeouts, command re-advertising after rediscovery) | `omc acp` subcommand; slash commands in `session/prompt`; `session/resume`, `close`, `fork`, `set_config_option`, `_session/steering`; `configOptions` + `config_option_update` / `session_info_update` / `available_commands_update`; `mcpServers` in new/load; tool-call history replay; structured diff/terminal tool content; `usage_update {size,used,cost}`; errors → `end_turn`/`refusal`; fs/terminal/elicitation client bridges | `src/modes/acp.c`, `src/cli/cli.c` | U/modes/acp/{acp-agent,acp-event-mapper,acp-client-bridge}.ts, U/slash-commands/acp-builtins.ts, U/commands/acp.ts |
| WP-17 CLI flags and subcommands | **Done:** `--flag=value`, omp's routing (reserved words / unported omp commands error, hoisting, `launch`), strict subcommand parser + omp help pages, `-r` picker, `--restore` (turn only), `--export`, `--models` (startup model), `--smol/--slow/--plan`, `--hide-thinking`, `--advisor`, `--max-time/--max-cost/--warn-cost`, `--no-lsp`, `--skills`, `--system-prompt-template`, `--mode rpc-ui`, `@image` args; commands acp, agents, auth export/import, completions/__complete, config, grep, login, models, read, search, setup (wizard, python), ssh, token, worktree. **Remaining:** `--add-dir` (WP-21), `--restore` subagent revival, `--models` role scoping, commit, session pack/unpack/moved/lease, ttsr, setup speech (⛔) | `src/cli/*.c` | U/cli/{args,flag-tables}.ts, U/cli-commands.ts, U/commands/*.ts, U/main.ts |

### P2 — specific workflows

| WP | Scope | omc files | Upstream sources |
|---|---|---|---|
| WP-18 MCP and SSH management ✅ (done; remaining: Smithery registry verbs, `xd://` mount notices when MCP tools change, managed OAuth `auth` blocks, tools/list_changed outside the TUI) | `/mcp add` wizard, list, remove, test, reauth, unauth, enable, disable, reconnect, reload, resources, prompts, notifications (`mcp.enableProjectConfig`, `mcp.notifications`, `mcp.renderMarkdownResults`); `/ssh add\|list\|remove` | `src/mcp/mcp.c`, `src/mcp/oauth.c`, `src/modes/interactive.c`, new `src/ui/mcpwizard.c`, `src/tools/ssh.c` | U/modes/controllers/{mcp-command-controller,ssh-command-controller}.ts, T/overlays/mcp-add-wizard.ts, U/slash-commands/helpers/{mcp,ssh}.ts |
| WP-19 Model switching (done: `/switch` + Alt+P picker incl. quick roles / task mode / over-context compact-first, `/switch` argument completion, Ctrl+P cycle (WP-5 key, #reapplyThinkingLevel), `/modelpreset` (shared with the hub presets), prewalk (`/prewalk`, `prewalk.enabled/into/from`, `--prewalk[-into]`/`--no-prewalk`), retry fallback chains + hub `f` (earlier), the 503 retry-backoff bug (turn level); remaining: `prewalk.auto`/`autoTarget` (judge classifier), `--models` scope in the picker/presets, xd:// device writes as prewalk triggers (no approval tier recorded)) | `/switch` + Alt+P temporary model picker; Ctrl+P / Shift+Ctrl+P role cycle with cycle track; `/modelpreset` + hub presets; prewalk (`/prewalk`, `prewalk.*`); retry fallback chains (`retry.*`, hub `f`) | `src/ui/modelhub.c`, `src/modes/interactive.c`, `src/coding/app.c` | T/overlays/model-picker.ts, model-hub.ts; U/modes/controllers/input-controller.ts (`cycleRoleModel`); U/session/{prewalk,retry-fallback-chains,role-models}.ts |
| WP-20 Conversation utilities ✅ (done: `/copy` picker + `code\|cmd\|link` targets, `/open`, `/btw` panel + history + follow-ups; open: omp's native/TSP describe() variants of the picker (no native host in omc)) | `/copy` picker + `code\|cmd\|link` targets; `/btw` streaming panel, follow-up and history browser; `/open` | `src/modes/copyctl.c`, `src/ui/copysel.c`, `src/ui/rewind.c` (shared `ui_tx_*` engine), `src/ui/btw.c` | T/overlays/{copy-selector,copy-targets,btw-panel,btw-history-panel}.ts, U/modes/controllers/btw-controller.ts, U/session/btw-history.ts |
| WP-21 Workspace and session lifecycle | **Done:** `/move` (+ overlay, create-directory confirm; session file, artifacts and `local://` root move to the new bucket, settings/discovery/commands/prompt re-scoped), `/wt` (+ `worktree.cleanSource/onStart/onExit/base`, shared `src/session/worktree.c` with `omc worktree` and the github tool), `/add-dir` / `/remove-dir` / multi-root `/dirs` + `--add-dir` + `workspace.additionalDirectories` (session header, `<workspace-roots>`, per-root context files, forwarded to subagents), `/restart` (execv with restartArgv), `/fresh`, `/pin`, `/local` + Alt+G, linked-worktree status-line path. **Remaining:** clone-first worktree backends (`worktree.clone`, `isolation.backend`: always a plain checkout), the per-root workspace tree / nested AGENTS.md list (omc has no workspace tree), `/fresh`'s provider-state count is omc's per-endpoint estimate, ACP `session/new` additionalDirectories (WP-16) | `src/coding/app.c`, `src/session/session.c`, `src/session/worktree.c`, `src/modes/interactive.c`, `src/ui/move.c` | U/slash-commands/builtin-lifecycle.ts, U/session/session-worktree.ts, T/overlays/move-overlay.ts, U/modes/local-file-editor.ts |
| WP-22 Subagent visibility (done: running-subagent badge + background-jobs badge in the status line, `←← to see N running agents` composer hint, the pinned "Subagents · N running" HUD (`display.pinnedAgents`, `display.subagentLivePreview`), the Agent Hub (`/hub` / `/subagents`, Alt+A / Ctrl+S, ←←: Agents roster with Flat/By parent, inspector, Activity log with follow/filter/scope/search, `r` revive, `x` kill), the transcript viewer (chat with an agent), session focus (Enter on a roster row; Esc / ←← back; viewer-scoped `/jobs`, `/effort`, `/export`, `/usage`), RPC subagent frames and commands, persisted subagent transcripts `<artifacts>/<id>.jsonl`, the nested `/jobs` tree; WP-22b done: `/agents` hub (`src/ui/agentshub.c`), the `<id>.md` output artifact + inspector Output row, rebuilt task cards keep the newest job frame, the parent waits on background work (TUI waiting row, Esc stops waiting, RPC `awaitingAsyncWork` settle), subagents settle their own jobs, task `Tip:` / `Coordinate:` advisories and progress frames; remaining: live job widgets (`async.widgets`, U/job-widgets/*: widget-role spec authoring, rule runtime, spark/bars/meter/stat/grid rendering in the jobs glance, `/jobs` details and `wait` tiles), `/agents` mouse and saving user agents under the omc agent dir instead of `~/.omp/agent/agents`, mouse in the Agent Hub (wheel/hover/click), `/btw` `/model` `/switch` in a focused view, structured `yield` / `subagent-async-pending` notice (needs `task.structured`, WP-27), progress frames per event (omp coalesces at 150 ms and also reports text deltas)) | `/hub` agent hub (roster, chat view, revive, kill), `/agents`, running-subagent badge in the status line, agent transcript viewer, session focus (←←), live job widgets (`async.widgets`) | `src/ui/statusline.c`, new `src/ui/agenthub.c`, `src/coding/agents.c`, `src/tools/jobs.c` | T/overlays/{agent-hub*,agents-hub,agent-transcript-viewer,running-subagent-badge}.ts, U/modes/controllers/session-focus-controller.ts, U/job-widgets/* |
| WP-23 Loop mode ✅ (done: `/loop` arguments, budget, `--while`/`--until`, `loop.mode` prompt/compact/reset, `loop.conditionTimeoutMs`, Esc pause, status-line segment, live autocomplete text; open: omp's vibe-mode guard for reset loops (omc has no `/vibe`), condition commands run through `proc_shell` instead of omp's per-session persistent shell) | `/loop [count\|duration] [--while\|--until cmd] [prompt]`, `loop.*`, Esc suspends | `src/modes/loopctl.c`, `src/coding/loop.c`, `src/modes/iqueue.c`, `src/ui/statusline.c` | U/slash-commands/builtin-modes.ts (loop), U/modes/{loop-limit,loop-condition}.ts |
| WP-24 Provider usage 🟡 (**done:** fetchers for anthropic, openai-codex, github-copilot, google-gemini-cli, zai, synthetic, ollama(-cloud) with the shared cache + history; `/usage` dashboard + Details; `/usage reset` picker and Codex / Claude saved resets; per-turn usage row; `omc usage`; ACP `_omp/usage`; RPC `get_state.usageLimit`; text `/usage`. **Remaining:** the other 14 fetchers (kimi-code, minimax-code, opencode-go, google-antigravity, cursor, devin, xai-oauth, factory-droid, cline-pass, umans, commandcode, alibaba-token-plan, charm-hyper, muse-code), rate-limit header ingestion, usage-aware ranking / reserve fallback, account policies + tombstones + re-login / reset-expiry banners in `omc usage`, auto reset redemption, read-group usage rows, status-line usage segment) | `/usage` provider quota dashboard + `/usage reset` selector; per-provider usage fetchers; per-turn usage row (`display.showTokenUsage`, `display.showTurnTime`) | `src/modes/interactive.c`, `src/config/auth*.c`, `src/ui/chat.c` | T/overlays/{usage-dashboard,usage-display,usage-row,reset-usage-selector}.ts, U/slash-commands/helpers/usage-report.ts |
| WP-25 Web search providers (done: web role chain + hosted expansion, anthropic/openai/codex/gemini/xai grounding, perplexity (API key), kimi, zai, searxng, `omc search` on the same code; omp's transport retries per provider; remaining: query directives (parseSearchQuery / applyQueryConstraints / per-provider formatQuery), Perplexity OAuth/cookie/anonymous transport, keyless Exa MCP, OpenRouter grounding, the scraper engines (startpage, ecosia, google, mojeek, public) and firecrawl/jina/kagi/deepseek/ollama/synthetic/tinyfish, `providers.fetch` reader backends (omc always uses its native reader, which is also omp's `auto` first choice)) | Search through logged-in providers (anthropic, openai, gemini, codex, xai, perplexity, kimi, zai), SearXNG (`searxng.endpoint`), `providers.fetch` | `src/tools/web.c` | U/web/search/{index,providers/*}.ts |
| WP-26 Settings honesty | Until each feature lands, hide ⛔-subsystem settings (§3c) from `/settings`, or mark them unsupported (decide the presentation with the user, since parity says the UI should match omp). Add a unit test that fails when a newly read key is undocumented here | `src/ui/settings.c`, `build-tools/extract_settings.sh` | T/overlays/settings-selector.ts |

### P3 — niche or large, low frequency

| WP | Scope | omc files | Upstream sources |
|---|---|---|---|
| WP-27 Remaining tools | generate_image, remote tts, security_scan + `security://` + `/security`, hindsight memory backend (retain/recall/reflect), `mr://`, `attachment://`, think, yield + `task.structured`, task isolation / outputSchema / perCallModel, eval `agent()` / workpool | `src/tools/*`, `src/coding/memory.c`, `src/tools/task.c` | U/tools/{image-gen,tts,security-scan,think,yield}.ts, U/hindsight/*, U/internal-urls/{mr,attachment,security}-protocol.ts, U/task/* |
| WP-28 Extra commands | done: `/changelog`, `/setup`, `/force`, `/skillful` (status.md). Remaining: `/omfg` (on top of `src/coding/ttsr.c`; needs runEphemeralTurn, the omfg panel and rule validation against the history), `/tan`, `/cleanse`, `/vibe`, `/ratchet` (needs eval preludes), `/debug` selector, `/record` (+ `omc play`), `/pause` (pause screen + agent freeze), marketplace/plugins install (git-based; JS parts ⛔), stats aggregation, `startup.changelogMode`, re-queueing a `/force` choice after an aborted request | `src/modes/interactive.c`, `src/coding/*` | U/slash-commands/builtin-*.ts, U/modes/controllers/{omfg,tan,cleanse}-*.ts, U/vibe/*, U/ratchet/*, U/extensibility/plugins/* |
| WP-29 Rich rendering and display options | Mermaid / SVG / inline images (kitty/iTerm/sixel, `terminal.showImages`), table auto-graph, reactions, `display.collapseCompacted`, `display.hideToolActivity` (Ctrl+Shift+O), cache-miss marker, `tui.hyperlinks`, `tui.mouse`, `tui.tight`, vim mode, composer token rate | `src/tui/markdown.c`, `src/ui/chat.c`, `src/tui/term.c`, `src/tui/editor.c` | T/{kitty-graphics,sixel,vim,latex-block}.ts, T/charts/*, T/chat/*, U/auto-graph/* |

Out of scope (⛔) and not packaged: JS/TS extensions, hooks and custom tools; the eval JS kernel;
collab/share/stream (hosted); the skills.omp.sh registry; browser and computer use; STT, live voice,
local TTS and tiny models; IDA; macOS spelling; npm self-update.

## 1. Slash commands

Upstream registry: `packages/coding-agent/src/slash-commands/builtin-*.ts` (87 commands, in this order:
modes, collaboration, session, lifecycle, marketplace, skills, control). omc: one registry, `src/coding/slashcmd.c`, built from
`assets/tui/slash-commands.json` (build-tools/extract_slash_commands.py: names, aliases, icons,
descriptions, ACP copy, inline hints, subcommands, allowArgs, handle/handleTui) filtered to what omc
implements (commands and subcommands). TUI handlers: `handle_command()` in `src/modes/interactive.c`;
text-mode handlers (RPC `prompt`, ACP `session/prompt`, TUI fallback) in slashcmd.c; autocomplete in
`src/ui/slash.c`. An unknown `/word`, or args to a command without allowArgs, go to the model.

omc-only command (not in omp): `/migrate`.

| Command (aliases) | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| /security | builtin-modes.ts | — | ❌ | OMP-native security scans (plan/scan/status/…/disposition); needs scan engine + `security://` |
| /settings | builtin-modes.ts | `ui_overlay_settings` | ✅ | most settings have no effect, see §3 |
| /setup (providers) [providers] | builtin-modes.ts, setup/lazy.ts runProviderSetupWizard | `ui_setup_run_providers` (`src/ui/setup.c`), `src/modes/extractl.c` | ✅ | sign-in scene only, setupVersion untouched, no welcome intro; TUI only (no text handler, like omp) |
| /plan [prompt] | builtin-modes.ts, interactive-mode.ts `handlePlanModeCommand` | `handle_command` "plan" → `app_enter_plan_mode` / `app_exit_plan_mode` (`src/coding/planmode.c`) | ✅ | enter / "Exit plan mode?" confirm with a draft / paused / disabled, `plan.enabled`, plan-file path in the status and the live description; goal/vibe exclusion messages ❌ |
| /plan-review | builtin-modes.ts | `plan_review_command` (`src/modes/interactive.c`) | ✅ | newest `local://*plan.md`, aborts a running turn first |
| /vibe | builtin-modes.ts | — | ❌ | vibe mode (persistent fast/good worker sessions) |
| /goal | builtin-modes.ts | `goal_command` | ✅ | |
| /guided-goal | builtin-modes.ts | `handle_command` | ✅ | |
| /loop | builtin-modes.ts, loop-limit.ts, loop-condition.ts | `src/modes/loopctl.c`, `src/coding/loop.c` | ✅ | verified side by side with omp (count, duration, `--until`/`--while`, Esc pause, reset/compact modes, errors) |
| /queue | builtin-modes.ts | `handle_command` | ✅ | |
| /model (models) | builtin-modes.ts | `ui_overlay_model` / `app_set_model` | ✅ | `/models` alias; omp's `/model` takes no arguments (picker only), so `/model <pattern>` goes to the model like omp |
| /switch | builtin-modes.ts | `ui_overlay_model_picker`, `h_switch` | ✅ | picker without args, model-id / `@role` argument completion; `--models` scope not used (WP-19) |
| /fast | builtin-modes.ts | `src/coding/modelctl.c` (`modelctl_fast_command`), TUI `handle_command`, text handler `h_fast` | ✅ | on/ultra/off/status/toggle per model family, omp's replies and `Fast: <state>` autocomplete; status-line fast badge (WP-14) |
| /slow | builtin-modes.ts | `modelctl_slow_command`, `h_slow` | ✅ | flex tier on OpenAI/Google; `providers.anthropic.slowMode` + subscription low-priority lanes on Anthropic (WP-14) |
| /skillful [on\|off\|status] | builtin-modes.ts, agent-session.ts setSkillful | `extracmd_set_skillful` (`src/coding/extracmds.c`) | ✅ | session-only override; empty session rebuilds the prompt, mid-session enable appends the hidden `skillful-notice` (at once, not deferred to the next turn); the `skillful` setting drops `<skills>` from the system prompt |
| /extended-context | builtin-modes.ts | `modelctl_extended_context_command`, `h_extended_context` | ✅ | persists `extendedContext`, re-applies the catalog context-window tiers (WP-14) |
| /computer | builtin-modes.ts | — | ⛔ | native computer-use eval prelude (platform screen/input automation) |
| /ratchet | builtin-modes.ts | — | ❌ | eval hill-climb kickoff; needs the ratchet eval prelude (omc's eval kernel has no preludes) |
| /prewalk | builtin-modes.ts, session/prewalk.ts | `src/coding/prewalk.c` | ✅ | `[into]`, `restart [from] [into]`, `off`; `prewalk.auto` (judge classifier) ❌ |
| /modelpreset | builtin-modes.ts | `src/coding/modelpreset.c` | ✅ | list/save/switch/delete + the "Switch to model preset" selector; config.yml byte-identical |
| /effort | builtin-modes.ts | `handle_command` "effort", `slash_resolve_effort` | ✅ | live level list (incl. `auto`) in the dropdown and ghost hint, omp's errors and status texts |
| /advisor | builtin-collaboration.ts | `handle_command` "advisor" | 🟡 | on/off/toggle texts, `status` report (single advisor + no-model roster), status-line badge; `dump [raw]` (copies the advisor history like omp); `configure` (advisor-config overlay) missing; one advisor only |
| /export [--themes] [path] | builtin-collaboration.ts, export/html/* | `app_export_html` (`src/session/export_html.c`, `trace.c`) | ✅ | omp's document: template.html.txt with the hosted viewer pinned to 18.14.2, base64 session data (header, entries, leaf, system prompt, tools, sub-sessions, trace port of stats/trace.ts), `--themes` CSS; byte-identical to `omp --export` on the test fixtures; also `omc export <file>` and RPC `export_html`; opens the file with xdg-open/open like openPath |
| /trace | builtin-collaboration.ts | — | ❌ | opens trace in stats dashboard (depends on /stats) |
| /dump [all\|anon] | builtin-collaboration.ts, session/session-dump-format.ts, session-anonymizer.ts | `src/session/dump.c`, `anon.c` | ✅ | formatSessionAsText (tool inventory, YAML tool args, thinking, custom/system notices) + the LLM request JSON sidecar, `all` (session.md, llm-request.json, subagents/*.md) and `anon` (anonymized JSONL) zip archives with formatDumpArchiveReport; status texts and archive members verified against omp; the sidecar's model `compat` table differs from omp's resolved one |
| /share | builtin-collaboration.ts | — | ⛔/❌ | share server `my.omp.sh` is omp's hosted service (⛔); secret-gist mode is portable (❌) |
| /collab, /join, /leave | builtin-collaboration.ts, collab/* | — | ⛔ | live relay via `wss://my.omp.sh` + web UI (hosted service) |
| /browser | builtin-collaboration.ts | — | ⛔ | browser eval prelude (Chromium/CDP, puppeteer) |
| /copy | builtin-collaboration.ts, overlays/copy-selector.ts | `src/modes/copyctl.c`, `src/ui/copysel.c` | ✅ | picker + `code\|cmd\|link`; clipboard through OSC 52 only (omp also tries pbcopy / termux / native) |
| /open | builtin-collaboration.ts | `cc_open_command` (`ui_open_in_browser`) | ✅ | |
| /todo [sub] | builtin-session.ts, todo-command-controller.ts | `src/ui/todocmd.c` | 🟡 | every verb ported (`user_todo_edit` entry + developer reminder); `expand`/`collapse` only record the flag (no live todo HUD yet) |
| /session [info\|delete\|pin] | builtin-session.ts, session-info-overlay.ts | `src/ui/panels.c` | 🟡 | Session Info overlay, `delete` (confirm → selector), `pin`; omc keeps one credential per provider, so `pin` lists it as text instead of omp's account selector; no LSP rows; `Build:` is the running version (entries are not stamped) |
| /jobs [full\|kill] | builtin-session.ts, jobs-panel.ts | `src/ui/panels.c` | ✅ | Background Jobs report (`Running:` count, job rows, labels / `full` commands), `kill <id>\|all`; omc jobs have no subagent owners, so the job tree is flat; the native jobs sheet is ⛔ (text mode only) |
| /usage [show\|reset] | builtin-session.ts, usage-dashboard.ts | `src/ui/usage.c` | ✅ | dashboard, Details, reset picker, `/usage reset <target>`; text form for RPC/ACP (WP-24) |
| /stats | builtin-session.ts, packages/stats | — | ❌ | local stats dashboard (HTTP server + web UI over session files) |
| /changelog [full\|last [N]] | builtin-session.ts, utils/changelog.ts | `src/coding/changelog.c`, `src/modes/extractl.c` | ✅ | omp 18.14.2's CHANGELOG embedded (`assets/CHANGELOG.md.z`); panels and texts identical in compare.sh; text over RPC/ACP |
| /hotkeys | builtin-session.ts | `ui_overlay_hotkeys` | ✅ | |
| /tools | builtin-session.ts, command-controller `handleToolsCommand` | `ui_tools_command` | ✅ | Available Tools markdown table incl. `xd://` devices |
| /context | builtin-session.ts, overlays/context-viewer*.ts | `src/ui/context.c` | 🟡 | full viewer (summary, occupancy map, legend, category previews, entry pages); counts are omc's ~4.1 chars/token estimate (omp: native tokenizer); snapcompact savings lines missing |
| /extensions (status) | builtin-session.ts | — | ❌ | Extension Control Center (skills/rules/MCP/hooks/tools inventory; JS extensions part ⛔) |
| /agents | builtin-session.ts, overlays/agents-hub.ts | `ui_agents_hub_open` (`src/ui/agentshub.c`) | ✅ | text mode verified side by side (list, strips, pattern, model browser, create flow); no mouse (WP-22b) |
| /git | builtin-session.ts | — | ❌ | git UI (split diff, staging, commit composer) |
| /local [name] (Alt+G) | builtin-session.ts, modes/local-file-editor.ts | `local_command` (interactive.c) | ✅ | picker (newest first, sizes), `local://` URL or bare name, root containment, editor exit status; cell-identical in compare.sh |
| /hub (subagents) | builtin-session.ts, overlays/agent-hub*.ts | `ui_agent_hub_open` (`src/ui/agenthub.c`) | ✅ | opens on the Activity tab like omp; Alt+A / Ctrl+S / ←← open the Agents tab (WP-22) |
| /branch (rewind) | builtin-session.ts, overlays/rewind-selector.ts | `tc_rewind_open` (src/modes/treectl.c, src/ui/rewind.c) | ✅ | fullscreen transcript replica with the dotted outline, branch strip, filter; `/rewind` alias |
| /fork [pane\|window\|tab] | builtin-session.ts | `app_fork_from` | 🟡 | no multiplexer placement subcommands; no streaming guard |
| /tree | builtin-session.ts, overlays/tree-selector.ts | `tc_tree_open` (src/modes/treectl.c, src/ui/tree.c) | ✅ | `ask` re-answer from the tree not ported (the leaf moves onto the result) |
| /login, /logout | builtin-session.ts | `ui_overlay_login` | ✅ | |
| /mcp [sub] | builtin-session.ts, mcp-command-controller.ts, helpers/mcp.ts | `src/ui/mcpcmd.c`, `src/ui/mcpwizard.c`, `src/coding/slashmcp.c` | ✅ | help, list, add (wizard + quick add), remove, test, reauth, unauth, enable, disable, reconnect, reload, resources, prompts, notifications in the TUI and as text over RPC/ACP; server-name completion. `smithery-search/login/logout` ❌ (not advertised) |
| /ssh [add\|list\|remove] | builtin-lifecycle.ts, ssh-command-controller.ts, helpers/ssh.ts | `src/ui/sshcmd.c`, `src/coding/slashmcp.c`, `src/tools/ssh.c` | ✅ | ssh.json host management shared with `omc ssh` |
| /new | builtin-lifecycle.ts | `app_new_session` | ✅ | |
| /fresh | builtin-lifecycle.ts | `app_fresh_session` | ✅ | new provider session id; the pruned count is omc's estimate of omp's providerSessionState keys (completions per endpoint+model, Responses per provider, Codex, Anthropic, Antigravity, Duo Workflow) |
| /clear | builtin-lifecycle.ts | `app_clear_context` | ✅ | `✔ Context reset — N messages dropped; session continues.` and the streaming warning |
| /delete | builtin-lifecycle.ts | unlink + new | ✅ | in-memory error and `✔ Session deleted` like omp |
| /compact [soft\|remote\|snapcompact] [focus] | builtin-lifecycle.ts, session/compact-modes.ts | `app_compact_mode` (`compaction.c`) | 🟡 | parseCompactArgs (modes, focus, snapcompact focus error); the mode overrides `compaction.methodOrder` (soft, remote → soft); remote runs OpenAI Responses / Anthropic server compaction where the route supports it (else omp's "remote compaction is unavailable" warning); snapcompact errors (text-only message as omp; not implemented for vision models); the autocomplete offers only `soft`; no-op texts `Nothing to compact (session too small)` / `Already compacted`; the RPC `compact` response carries no result data |
| /shake [elide\|images\|thinking] | builtin-lifecycle.ts, shake-types.ts | `app_shake(mode)` | 🟡 | 3 modes, omp's parse error and formatShakeSummary, branch rewritten on disk; elide drops whole tool results outside the newest ~4000 tokens (`[shaken ~N tokens]`, skill results protected) but not fenced/XML blocks and saves no recovery artifact; token counts are estimates |
| /handoff [focus] | builtin-lifecycle.ts, command-controller `handleHandoffCommand` | `app_handoff(focus)` | ✅ | focus rendered into the handoff prompt (`additionalFocus`); guards and loader from WP-2 |
| /resume [id] | builtin-lifecycle.ts | `ui_overlay_session` / `session_resolve` | ✅ | `/resume <id\|path>` resumes or errors `Session "x" not found`; `@claude`/`@codex` foreign imports not offered |
| /pin [session id] | builtin-lifecycle.ts, session/session-pins.ts | `session_pin_toggle` | ✅ | `<agent>/session-pins.json` under a file lock; the picker's 📌 |
| /btw [q] | builtin-lifecycle.ts, btw-controller.ts, overlays/btw-panel.ts | `src/ui/btw.c` | ✅ | inline streaming panel + history browser ported; image follow-ups missing |
| /tan | builtin-lifecycle.ts, tan-command-controller.ts | — | ❌ | background agent for tangential work |
| /omfg | builtin-lifecycle.ts, omfg-controller.ts | — | ❌ | forge a TTSR rule from a complaint |
| /cleanse | builtin-lifecycle.ts, cleanse-command-controller.ts | — | ❌ | diagnostics fixing with parallel subagents |
| /retry | builtin-lifecycle.ts | `retry_main` | ✅ | |
| /debug | builtin-lifecycle.ts | — | ❌ | debug tools selector |
| /memory | builtin-lifecycle.ts | `memory_command` | ✅ | local backend; `mm *` only for hindsight (❌ backend) |
| /rename [title] | builtin-lifecycle.ts | `handle_command` | ✅ | |
| /move [dir] | builtin-lifecycle.ts, overlays/move-overlay.ts | `app_move`, `src/ui/move.c` | ✅ | overlay, `Create directory?` confirm, `✔ Moved to …`; text mode for RPC/ACP; cell-identical in compare.sh |
| /wt [branch] (worktree) | builtin-lifecycle.ts, session/session-worktree.ts | `session_wt_create` + `app_move` | 🟡 | changes carried over (dirty, staged, untracked + the source index); always a plain checkout (no clone backends), so the summary always says `checked out` |
| /add-dir, /remove-dir | builtin-lifecycle.ts | `app_add/remove_workspace_dir` | ✅ | session header `additionalDirectories`; the system prompt refreshes |
| /dirs | builtin-lifecycle.ts | text handler | ✅ | every root |
| /exit, /quit (q) | builtin-lifecycle.ts, builtin-control.ts | `tui_stop` | ✅ | `/q` alias; bare `exit`/`quit`/`q` on an empty session |
| /restart | builtin-lifecycle.ts | `restart_command` + cli.c `restart_exec` | ✅ | execv of /proc/self/exe with restartArgv (one-shot selectors and positionals dropped, `--resume <file>` when on disk) |
| /marketplace, /plugins (plugin), /reload-plugins | builtin-marketplace.ts | — | ❌ | plugin sources/install (git-based, portable for skills/commands/MCP/agents); JS extension parts ⛔ |
| /skills [search\|install\|installed\|update] | builtin-skills.ts | lists discovered skills | 🟡 | **different feature**: omp searches/installs from the skills.omp.sh registry |
| /force <tool> [prompt] (force:) | builtin-control.ts, agent-session.ts setForcedToolChoice, utils/tool-choice.ts | `extracmd_force_tool` / `extracmd_prepare_request` (`src/coding/extracmds.c`) | 🟡 | [named, "none"] served one per request (identical on the wire); not re-queued after an aborted/failed request |
| /live | builtin-control.ts | — | ⛔ | Codex realtime voice (audio capture/playback) |
| /record | builtin-control.ts | — | ❌ | record screen to replayable file (`omp play`) |
| /pause | builtin-control.ts, overlays/pause-screen.ts | — | ❌ | freeze all agents |
| /forget-approvals | builtin-control.ts | `h_forget_approvals` (`coding/slashcmd.c`) | ✅ | clears the session grants and `approval-cache.json`; TUI, RPC and ACP (WP-10) |

Non-builtin command sources:

| Source | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| Custom prompt commands (`commands/<name>.md`: native .omc/.omp + agent dir, Claude Code (user opt-in, project, nested `dir:name`), Claude Code plugins (`plugin:name`), `.agent[s]` walk-up + home, Codex, OpenCode; bundled `/init`) | capability/slash-command.ts, extensibility/slash-commands.ts, discovery/{builtin,claude,claude-plugins,agents,codex,opencode}.ts, task/commands.ts | `src/coding/commands.c`, `app_expand_input` | ✅ | provider priority + first-name-wins, `commands.enable{Claude,Opencode}{User,Project}`, `disabledProviders`, `disabledExtensions` (`slash-command:<name>`); frontmatter `description` / first line, `argument-hint`, `side: true` (→ /btw), Codex/OpenCode `name`; `$1` `$@` `$ARGUMENTS` `$@[s:l]` + template render + appended args; TUI rows with ✎ and hint ghost text; RPC/ACP/print expansion; request parity verified with request_diff.sh (print + RPC). omp extension-package (`omp-plugins`) command roots ❌ (omc has no omp extension packages) |
| Prompt templates (`prompts/<name>.md`, user + project) | config/prompt-templates.ts | `src/coding/commands.c` | ✅ | `(user:sub)` / `(project)` description suffix, autocomplete after file commands, expanded in prompt/steer/follow-up |
| Skill commands `/skill:<name>` | modes/skill-command.ts, extensibility/skills.ts, tui prompt/skill-tokens.ts, chat/skill-message.ts | `app_skill_message`, `app_prompt_skill`, `ix_invoke_skill`, `render_skill` (`src/ui/chat.c`) | ✅ | leading and mid-prompt tokens (allowsSkillTokens), omp's user-invocation prompt as a user-attributed `skill-prompt` custom message (session entry identical), keyword notices from the args, steer/follow-up queueing with the chip text, `Failed to load skill:` errors; TUI: `/skill:` namespace row + bare-name breakout, mid-prompt skill popup, composer chips, callout/inline transcript card with Ctrl+O prompt; RPC (`dispatchRpcSkillPrompt`) and ACP (`#tryRunSkillCommand`) run them; advertised as `skill:<name>`. Chips are not OSC 8 links to SKILL.md; skill images get no vision-description notice |
| MCP prompts as commands | sdk.ts buildMCPPromptCommands, mcp/client.ts | `mcp_prompt_commands`, `mcp_run_prompt` | ✅ | `/<server>:<prompt> key=value`, `prompts/get` text + resource text joined by blank lines, empty result consumed; TUI rows `… (bundled)` with the MCP icon; RPC `mcp_prompt` source. `notifications/prompts/list_changed` re-lists the catalog (ACP re-sends `available_commands_update`) |
| Bundled TS commands `/review`, `/green` | extensibility/custom-commands/bundled/{review,ci-green} | — | ❌ | interactive code review / CI-green loop; they also outrank skill breakout in the `/` popup (`/rev`) |
| Extension/plugin commands (JS) | extensibility/extensions | — | ⛔ | Bun/JS runtime |

## 2. Keybindings and interactive flows

The key table (`src/tui/keys.c` `DEFAULTS[]`) holds omp's whole registry: the 32 `tui.*` actions of
`keybindings.ts` and the 45 `app.*` actions of `app-keybindings.ts`, same defaults and order. `key_hook`
(`src/modes/interactive.c`) dispatches every app action through `tui_kb()` (WP-5). User overrides load from
`keybindings.yml` / `.yaml` / legacy `keybindings.json` in the agent dir (`src/config/keybindings.c`), and
`/hotkeys` is generated from the live table. Rows marked (tmux) were probed in tmux against the mock model;
"= omp" means compared side by side with `tests/tui/compare.sh`.

### 2a. Editor / input / select actions (`packages/tui/src/keybindings.ts`)

| Action (keys) | omc | Status | Notes |
|---|---|---|---|
| cursor up/down/left/right, word left/right, line start/end, pageUp/Down | `src/tui/editor.c` | ✅ | history on first/last line works |
| jumpForward (ctrl+]) / jumpBackward (ctrl+alt+]) | editor.c `jump_to_char` | ✅ | = omp. In tmux omp does not decode the legacy `ESC ^]` encoding of ctrl+alt+] (it types the target); omc does |
| deleteChar/Word Backward/Forward, deleteToLineStart/End | editor.c | ✅ | |
| yank (ctrl+y) / yankPop (alt+y) | editor.c kill ring (60 entries) | ✅ | = omp: consecutive kills accumulate (prepend backward / append forward), line joins kill `\n`, alt+y only right after a yank |
| undo (ctrl+-, ctrl+_) | editor.c | ✅ | |
| spellingSuggestions (ctrl+.) | — | ⛔ | macOS native spelling |
| input.newLine (shift+enter, ctrl+j) | editor.c | ✅ | alt+enter, ctrl+enter (unless bound to submit), shift+backspace / shift+delete are hard-coded fallbacks as upstream |
| input.submit, input.tab, select.* | editor.c, components.c | ✅ | `/resume` picker ignores pageUp/pageDown |
| input.copy (ctrl+c with selection) | — | ❌ | editor has no selection |
| Vim mode (`tui.vimMode`, vim.ts) | — | ❌ | setting shown but inert |
| User overrides (keybindings.yml, legacy .json migration, profile inherit) | `src/config/keybindings.c` | ✅ | = omp (overrides in `/hotkeys`, migrated YAML byte-identical); `/migrate` copies the files. Profile inherit is implemented (`kbcfg_load_merged`) but omc has no profiles to call it with |

### 2b. App actions (`packages/tui/src/app-keybindings.ts`)

| Action (default keys) | omc | Status | Notes |
|---|---|---|---|
| app.interrupt (esc) | `on_escape` → `iq_escape` | ✅ | WP-2 abort/queue restore/bash/python, then a draft resets the window and Esc-Esc (500 ms) on an empty editor runs `doubleEscapeAction` (rewind → `/branch`, tree → `/tree`, none); = omp ("No messages to branch from"). The selectors themselves are WP-8 |
| app.clear (ctrl+c) | `on_ctrl_c` | ✅ | = omp: clears (draft kept in Up history, `composer.recallClearedDrafts`), 2nd press within 500 ms exits, never aborts |
| app.exit (ctrl+d) | key_hook | 🟡 | forward-deletes with a draft when the key is also `deleteCharForward`; draft not saved for resume |
| app.suspend (ctrl+z) | `tui_suspend` | ✅ | SIGSTOP to the process group like omp; "Failed to suspend: …" on refusal |
| app.display.reset (alt+l) | key_hook | 🟡 | full repaint; omp also re-probes the terminal background (OSC 11) |
| app.thinking.cycle (shift+tab) | key_hook | ✅ | off → auto → efforts (WP-3); "Current model does not support thinking" |
| app.thinking.toggle (ctrl+t) | key_hook | ✅ | status "Thinking blocks: hidden/visible", persists `hideThinkingBlock`, "Thinking is off — …" gate |
| app.model.cycleForward/Backward (ctrl+p / shift+ctrl+p) | `app_cycle_role_model` | ✅ | role cycle + cycle track |
| app.model.select (alt+m) | `ui_overlay_model` | ✅ | |
| app.model.selectTemporary (alt+p) | `ui_overlay_model_picker` | ✅ | pressing it again inside the picker toggles task mode |
| app.tools.expand (ctrl+o) | key_hook | ✅ | |
| app.tools.toggleVisibility (ctrl+shift+o) | — | ❌ | |
| app.ask.hide (alt+h) | ask.c | ✅ | via the table; ask expansion uses `app.tools.expand` |
| app.approval.approve (ctrl+enter, ctrl+q) | `ui/approval.c` | ✅ | approves the pending tool call (armed 500 ms after the dialog mounts) |
| app.editor.external (ctrl+g) | `open_external_editor`, ask prompts, hook editor | ✅ | WP-6: `getEditorCommand` / `openInEditor` (`.omp.md`, trailing newline trimmed, "No editor configured…" warning); ask "Other"/note prompts and the hook editor via `editDialogExternally` (tmux = omp) |
| app.local.open (alt+g) | `key_hook` → `local_command` | ✅ | the `/local` picker |
| app.message.followUp (ctrl+q, ctrl+enter) | `iq_follow_up` | ✅ | WP-2; `/compact` with it while streaming waits for the yield |
| app.retry (f5, alt+r) | `retry_key` → `/retry` | ✅ | = omp "Nothing to retry" |
| app.message.dequeue (alt+up, shift+up) | `iq_dequeue` | ✅ | WP-2 |
| app.clipboard.pasteImage (ctrl+v) | `ui_attach_paste_clipboard` | ✅ | |
| app.clipboard.pasteTextRaw (ctrl+shift+v) | `ui_attach_paste_raw` | ✅ | WP-6: clipboard text inline, no chip; "No text in clipboard to paste raw", SSH notice |
| app.clipboard.copyLine (alt+shift+l), copyPrompt (alt+shift+c) | key_hook, OSC 52 | ✅ | = omp "Copied line: …" / "Copied: …" (30-char preview) |
| app.agents.hub (alt+a), app.session.observe (ctrl+s) | `key_hook` → `ui_agent_hub_open` | ✅ | both open (and close) the Agent Hub; ←← on an empty composer too (WP-22) |
| app.session.new/tree/fork/resume (unbound by default) | key_hook → `/new`, `/tree`, `/branch`, `/resume` | ✅ | bindable in keybindings.yml |
| app.session.pin/unpin (alt+k / alt+shift+k), rename/delete in picker | `src/ui/sessions.c` | ✅ | pin/unpin via the table; togglePath/toggleSort/rename/delete keys still hard-coded |
| app.tree.previousBranch/nextBranch (unbound by default) | `move_branch` (src/ui/tree.c) | ✅ | the help line names them once bound |
| app.plan.toggle (alt+shift+p) | `key_hook` → `/plan` | ✅ | via the table |
| app.history.search (ctrl+r) | `ui_history_search` (`src/ui/histsearch.c`) | ✅ | WP-6: HistorySearchComponent over `prompt-history.jsonl` folded like HistoryStorage (cell-identical in tmux); omp's 100 ms search debounce not ported (results update per key) |
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
| `@` file mentions | `src/tui/atmention.c` | ✅ | WP-6: fuzzy project-wide search (native fuzzyFind scorer, gitignore-aware walk), `@dir/` scopes, listings for `@`, outside-cwd paths and no hits, quoted paths; lists cell-identical in tmux. The walk is reused for 2 s (omp: stamp-validated listing cache) |
| `#` prompt actions, `#123` → pr:// / issue:// | `tui_ac_default_provider` + `on_prompt_action` | ✅ | WP-6: the seven actions with live key hints, `undoPastTransientText`, `pr`/`pull`/`issue` qualifiers (tmux = omp). A second `#undo` right after one differs (omp leaves a stray character) |
| `^model` mentions, `:emoji` completion, internal-URL completion | — | ❌ | |
| `/skill:<name>` invocation + completion | `ix_invoke_skill`, `src/tui/editor.c` (namespace row, mid-prompt popup, chips) | ✅ | see §1 (WP-9) |
| Bare `exit`/`quit`/`q` on an empty session | — | ❌ | sent to the model |
| Large paste | `📄 #N` chips (`attach.c`), large-paste menu (`hooksel.c`) | ✅ | WP-6: text-attachment chip + band card below `paste.largeMenuThreshold`, the menu (wrapped block / local file / inline) at it (tmux = omp, saved file byte-identical); a paste whose burst carries Enter still opens the menu (omp skips it) |
| Image paste, image-path paste | `src/ui/attach.c` | ✅ | see status.md (TUI image attachments) |
| Todo reminders, todo auto-clear, live todo panel | — | 🟡 | stop-time reminders (TodoReminderComponent notice card) ✅; auto-clear and live panel ❌ |
| Auto-compaction loader, retry countdown, TTSR notice | string-matched notices → working row | ✅ | |
| Retry fallback events | — | ❌ | |
| Smooth streaming / tool-args reveal | `chat.c`, `ui_apply_partial` | ✅ | |
| Session title generation | `src/coding/title.c` | ✅ | |
| Terminal title (OSC 0) | `src/ui/signals.c` | ✅ | `π <sep> <label>`, push/pop title stack |
| Turn-end / error / ask desktop notification | `src/tui/notify.c`, `src/ui/signals.c` | ✅ | OSC 9 / OSC 99 line / BEL + D-Bus, multiplexer routing |
| Composer predictions | — | ❌ | default off |
| /btw panel (branch/copy/follow-up keys) | `src/ui/btw.c` | ✅ | |
| Session focus (view a subagent, ←←) | `src/ui/agenthub.c` (`ui_focus_agent`) | 🟡 | Enter on a roster row views the agent's live session (transcript, status line `👻 <id>`, faint outline); the composer prompts it; Esc / ←← return; `/btw`, `/model`, `/switch` are not retargeted (WP-22) |
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
| `tools.approvalMode` (yolo) + `tools.approval` ({}) + `tools.restrictedFolders` + `bash.patterns` | Approval tiers (always-ask, always-ask-cached, write, yolo), per-tool allow/prompt/deny, approval dialog | ✅ WP-10 (`tools/approval.c`); `bash.allowCompoundCommands` ❌ |
| `magicKeywords.*` (on) | "ultrathink" / "orchestrate" / "workflow" / "jevify" in a prompt raise thinking and attach hidden notices (`modes/magic-keywords.ts`) | 🟡 notices ✅ (`src/coding/keywords.c`); `workflowz` notice withheld (teaches unimplemented eval `agent()`/`workpool()`); editor gradient ❌ |
| `model.loopGuard.*`, `model.toolCallLoopGuard.enabled` (on) | detects repeated assistant output / identical tool-call loops and steers | ✅ (`guards.c`, `thinkloop.c`) |
| `todo.reminders` (on), `todo.remindersMax` (3), `todo.eager` | reminds the model of unfinished todos at stop; eager todo creation | 🟡 reminders ✅; `todo.eager` ❌ |
| `completion.notify` / `ask.notify` (on), `error.notify` | desktop/terminal notification when a turn ends or ask waits | ✅ |
| `recap.enabled` (on), `recap.idleSeconds` (240) | idle recap message after inactivity | ✅ (not journaled: no history.db) |
| `tui.titleState` (on), `tui.titleSpinner`, `terminal.programStatus` (on) | terminal title shows run state + spinner; OSC 7501 program status | ✅ |
| `compaction.methodOrder` ([remote, snapcompact, handoff, shake, soft]), `compaction.midTurnEnabled`, `compaction.asyncEnabled`, `compaction.supersedeReads`, `compaction.dropUseless` (all on) | multi-method compaction ladder, mid-turn and async compaction, stale-read supersession, uneventful-result elision | ✅ snapcompact is passed over (not implemented) |
| `display.collapseCompacted` (on) | compacted history collapses in the transcript | ❌ |
| `tui.renderMermaid`, `tui.renderSvg`, `tui.reactions` (on), `tui.autoGraph` (auto) | mermaid/SVG rendering, agent reactions, table charts | ❌ |
| `terminal.showImages` (on), `images.autoResize` (on), `images.describeForTextModels` (on) | inline images (kitty/iTerm/sixel), resize before send, describe images for text-only models | ❌ |
| `edit.recoverInlineEdits`, `edit.blockAutoGenerated` (on) | recovers edit payloads pasted inline; refuses to edit generated files | ❌ |
| `lsp.lazy`, `lsp.shared` (on) | lazy server start, shared servers across sessions | 🟡 servers start on first use like the default; not switchable; no cross-session sharing (`lsp.diagnosticsDeduplicate` is now read) |
| `shellMinimizer.enabled` (on) | compresses noisy shell output (`[raw output: artifact://N]`) | ❌ (also in status.md) |
| `tools.abortOnFabricatedResult` (on) | aborts a turn whose text fabricates a tool result | ❌ only applies to in-band (owned-dialect, `tools.format`) tool calling, which omc does not implement; native tool calling never fabricates in-band results |
| `async.widgets` (on) | live job widgets above the editor | ❌ (WP-22b remainder) |
| `mcp.enableProjectConfig` (on), `mcp.renderMarkdownResults` (on), `mcp.notifications` (off) | project `.mcp.json` opt-out; markdown MCP results; resource-update injection | ✅ |
| `skills.enableSkillCommands` (on), `commands.enableClaudeProject`/`enableOpencodeProject` (on) | `/skill:<name>` and file-based custom commands | ✅ (WP-9, §1) |
| `skillful` (on) | skills listed in system prompt (session-toggleable) | 🟡 always listed; not switchable |
| `plan.enabled` (on), `goal.statusInFooter` (on), `title.refreshOnReplan` (on) | plan mode can be disabled; goal in footer; retitle on replan | ❌ |
| `task.speculativeLaunch`, `task.completionProbe`, `task.softRequestBudget` (200) + notice | subagent launch/probing/budget behaviour | ❌ |
| `composer.recallClearedDrafts` (on), `doubleEscapeAction` (rewind) | Ctrl+C keeps the cleared draft in Up history; Esc-Esc action | ✅ (WP-5) |
| `input.bareExitOnEmptySession` (on), `emojiAutocomplete` (on), `autocompleteMaxVisible` (10), `spelling.autocomplete` (auto) | editor conveniences (see §2) | ❌ (`paste.largeMenuThreshold` is honoured since WP-6) |
| `features.unexpectedStopDetection` (mechanical) | detects/continues unexpected model stops | ✅ (mechanical + smart; plus the empty-stop retry) |
| `power.sleepPrevention` (idle) | prevents system sleep while running | ✅ (Linux `systemd-inhibit` logind inhibitor; macOS `caffeinate`; no desktop ScreenSaver inhibit) |
| `startup.checkUpdate` (on), `update.channel` | update check on start | ⛔ inert and hidden from /settings: omp asks its build service / npm for a newer omp and advises `omp update`; omc has no release feed or self-update (omp skips the check for source checkouts too) |
| `startup.changelogMode` (summary) | changelog on start | ❌ |
| `providers.cacheWarming` (idle) | keeps prompt cache warm while idle | ✅ (`src/coding/cachewarm.c`, WP-14) |
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
Model: `modelRoleStorage`, `inlineToolDescriptors`, `includeWorkspaceTree`, `prewalk.auto`, `prewalk.autoTarget`,
`images.urls.*` (6), `expandThinkingBlocks`, `omitThinking`, `externalThinking`,
`providers.autoThinkingMaxEffort`, `minP`, `presencePenalty`, `repetitionPenalty`,
`tier.advisor`, `retry.waitForUsageReset`, `retry.modelFallback`,
`retry.fallbackOn`, `retry.rateLimitWalksChain`, `retry.usageAwareFallback`, `retry.usageReservePct`,
`retry.usageReservePolicy`, `retry.fallbackChains`, `retry.fallbackRevertPolicy`,
`providers.anthropic.serverSideFallback`, `advisor.syncBacklog`.
Interaction: `autoResume`, `startup.showSplash`, `update.channel`, `marketplace.autoUpdate`,
`session.worktreeScope`, `tui.vimMode`, `tui.vimModeDisplay`,
`input.bareSlashCommands`, `composer.predictions`, `spelling.typoDetection`/`autocorrect`
(macOS ⛔), `skills.registryUrl`.
Context: `contextPromotion.enabled`,
`compaction.experimentalContextManagement`,
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
`tools.xdevNotices`, `dev.autoqaPush.endpoint`.
Tasks: `task.isolation.*`, `isolation.backend`, `worktree.clone` (omc: always a plain checkout; `worktree.cleanSource/onStart/onExit/base` are read since WP-21), `task.file`, `task.structured`,
`task.enableEffort`, `task.perCallModel`, `task.perCallModelAllow`, `task.enableLsp`, `task.maxRuntimeMs`,
`task.maxEffort`, `task.prewalk`, `plan.autosave`, `plan.autosaveDir`, `title.refreshAfterPrompts`.
Providers: `providers.antigravityEndpoint`, `providers.fetch`,
`codexResets.autoRedeem`, `claudeResets.autoRedeem`, `searxng.endpoint`,
`providers.kimiApiFormat`, `providers.openaiWebsockets`, `providers.openaiLiveSteering`,
`providers.muse-code.storeResponses`, `providers.openai-codex.daybreak`, `providers.openrouterVariant`,
`provider.appendOnlyContext`, `secrets.enabled`.

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
- ~~**`thinking: auto` turns thinking off.**~~ Fixed (WP-3): `auto` resolves like omp (`src/coding/autothink.c`).

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
| ask, todo, wait, goal | ask.ts, todo.ts, wait.ts, goals/ | `todo_ask.c`, `write_bash.c`, `goal.c` | ✅ | todo reminders ✅ (`guards.c`); auto-clear ❌ |
| task | task/* | `task.c`, `agents.c` | 🟡 | no isolation/worktrees, `outputSchema`, `perCallModel`, `speculativeLaunch`, `completionProbe`, soft request budget |
| web_search | web/search/* (~30 providers) | `websearch.c`, `web.c` | 🟡 | web role chain, hosted grounding (anthropic, openai, codex, gemini, xai) and engines parallel, brave, tavily, exa (key), duckduckgo, perplexity (key), kimi, zai, searxng; query directives, Perplexity OAuth, OpenRouter grounding and the scraper engines ❌ (see status.md) |
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
| out:// | `out_url.c` | ✅ | write-only reply channel, bash redirects, `out.enabled` (unset: interactive TUI only) |
| cfg:// | `cfg_url.c` | 🟡 | tree/namespace/setting views over omp's registry; approved writes in the TUI; no env provenance layer |
| conflict:// | `conflict.c` | ✅ | blocks registered by `read`, `:conflicts` index, `conflict://N[/side]` read/write, `conflict://*` |
| mr:// | — | ❌ | GitLab MRs via `glab` |
| attachment:// | — | ❌ | |
| security://, vault:// | — | ❌ | off by default |

## 5. Overlays, dialogs and command controllers

Upstream: `packages/tui/src/overlays/*`, `packages/coding-agent/src/modes/components/*`,
`packages/coding-agent/src/modes/controllers/*`. Probed in tmux against the mock model: `/session`,
`/context`, `/usage`, `/jobs`, `/todo`, `/tools` each print one inline notice; Ctrl+R does nothing although
`/hotkeys` lists it (as omp does).

### 5a. Overlays

| Overlay | Upstream | Opened by | omc | Status | Notes |
|---|---|---|---|---|---|
| plan-review-overlay + plan-toc | plan-review-overlay.ts (1943 lines), plan-toc.ts | `xd://propose`, `/plan-review` | `src/ui/planreview.c` | ✅ | sections, Contents sidebar, delete/undo, annotations, slider, copy, external editor; mouse ❌ |
| plan-save-overlay | plan-save-overlay.ts | "Save plan & quit" | `ui_plan_save_*` (`src/ui/planreview.c`) | ✅ | tiny-model filename suggestion |
| approval-dialog | approval-dialog.ts, tools/approval*.ts | tool call under `tools.approvalMode`/`tools.approval` | `ui/approval.c` | ✅ | cell- and ANSI-identical side by side; the native (`describe`) form and the countdown (no timeout is passed for tool approvals) are not needed |
| ask-dialog | ask-dialog.ts | `ask` tool | `src/ui/ask.c` | ✅ | cell-verified |
| context-viewer (+format, overview) | context-viewer*.ts | `/context` | `src/ui/context.c` | 🟡 | layout, map, legend, previews, entry view match; token estimates differ slightly (no native tokenizer) |
| copy-selector + copy-targets | copy-selector.ts, copy-targets.ts | `/copy [code\|cmd\|link]` | `src/ui/copysel.c` | ✅ | transcript outline, block view, caption click controls, `o` open; cell-identical to omp in compare.sh |
| tree-selector | tree-selector.ts (1794) | `/tree` | `src/ui/tree.c` | ✅ | ANSI-identical to omp in tmux (filters, search, labels, empty states, paging) |
| rewind-selector | rewind-selector.ts (1131) | Esc-Esc, `/branch`, `/rewind` | `src/ui/rewind.c` | ✅ | ANSI-identical to omp (outline, branch strip, filter, expand); mouse wheel scrolls, clicks not handled |
| jobs-panel + job-tree | jobs-panel.ts, job-tree.ts | `/jobs`, jobs pill | `src/ui/panels.c` | 🟡 | text-mode report ✅ (flat tree: no subagent-owned jobs in omc); the jobs sheet/detail pane is native-only |
| btw-panel, btw-history(-panel) | btw-*.ts | `/btw` | `src/ui/btw.c` | ✅ | no image paste in follow-ups |
| session-info-overlay | session-info-overlay.ts | `/session` | `src/ui/panels.c` | 🟡 | overlay ✅ (file/ID/build, provider + auth source, messages, append-only, tokens, cost, MCP); edit rejections, routed models and LSP rows missing |
| usage-dashboard / usage-display / usage-row | usage-*.ts | `/usage`; per-turn row (`display.showTokenUsage`) | `src/ui/usage.c`, `chat.c` | ✅/🟡 | cell-identical to omp; heatmap from session files (omp: stats DB); read-group compact usage rows and 14 of 22 fetchers ❌ |
| reset-usage-selector | reset-usage-selector.ts | `/usage reset` | `src/ui/usage.c` | ✅ | Codex + Claude (Cedar / Juniper) accounts |
| history-search | history-search.ts | Ctrl+R | `src/ui/histsearch.c` | ✅ | WP-6 |
| mcp-add-wizard | mcp-add-wizard.ts (1912) | `/mcp add` | `src/ui/mcpwizard.c` | ✅ | ANSI rendering identical step by step (native-terminal `describe()` sheet not applicable) |
| theme-selector | theme-selector.ts | settings theme submenu | `src/ui/settings.c` | 🟡 | live preview ok; no swatches/current dot/"Apply ⏎"; `ui_overlay_theme` declared in `overlays.h` but never defined |
| agent-hub, agent-activity, session-observer-registry | agent-hub*.ts | `/hub` | `src/ui/agenthub.c` | ✅ | roster (frozen ranks, Flat / By parent tree), inspector, Activity tab, r/x, `/` filters, Tab narrow details; no mouse; Output row from the `<id>.md` artifact |
| agents-hub | agents-hub.ts | `/agents` | `src/ui/agentshub.c` | ✅ | text mode; generation streams the agent-creation architect on the session model; no mouse (WP-22b) |
| agent-transcript-viewer | agent-transcript-viewer.ts | agent hub | `src/ui/agenthub.c` | ✅ | in-memory transcript replay, message box (revive + steer/prompt), stats, j/k/g/G/PgUp/PgDn, Ctrl+O expands its cards |
| running-subagent-badge | running-subagent-badge.ts | status line | `statusline.c` (`👥 N` + `⚙ N` jobs badge) | ✅ | |
| report-panel | report-panel.ts | `/tools`, `/jobs full`, `/advisor status`, `/changelog`, `/hotkeys`, `/memory` | `src/ui/report.c` | ✅ | docks above the editor when it fits (Esc dismisses), else a full-screen page; used by `/tools`, `/jobs`, `/advisor status`, `/changelog`, `/hotkeys`, `/memory` |
| pause-screen | pause-screen.ts | `/pause` | — | ❌ | |
| error-banner | error-banner.ts | provider error | `ui_error_banner_show` (`src/ui/chat.c`) | ✅ | |
| cleanse-panel, omfg-panel | *.ts | `/cleanse`, `/omfg` | — | ❌ | TTSR engine exists (`src/coding/ttsr.c`) |
| hook-selector / hook-input / hook-editor | hook-*.ts | extension `ctx.ui.*`; internal confirms | — | ⛔/❌ | ⛔ as extension API; internal uses (`/move`, skill install) ❌ with their commands; `/modelpreset` uses the hook selector (WP-19) |
| bordered-loader | bordered-loader.ts | `/share`, hook UI | — | ⛔ | used only by out-of-scope flows |
| plugin-selector | plugin-selector.ts | `/marketplace`, `/plugins` | — | ❌ | |
| plugin-settings | plugin-settings.ts | `/settings` Plugins tab | `settings.c` | 🟡 | always "No plugins installed" even with discovered Claude plugins; suggests `omc plugin install`, which does not exist |
| advisor-config | advisor-config.ts | `/advisor configure` | — | ❌ | |
| model-hub, hub-frame, model-browser | model-hub.ts … | `/model`, Alt+M | `src/ui/modelhub.c`, `modelbrowser.c` | ✅/🟡 | gaps in status.md (presets, fallback chains, custom roles, F5, mouse) |
| model-picker | model-picker.ts | Alt+P, `/switch` | `src/ui/modelpicker.c` | ✅ | cell-identical (plain + ANSI) in session, `@` quick-role and task modes; `--models` scope ❌ |
| thinking-selector | thinking-selector.ts | `/thinking`, `/effort` | `ui_overlay_thinking` | ✅ | |
| settings-selector | settings-selector.ts | `/settings` | `src/ui/settings.c` | ✅ | most settings inert (§3) |
| session-selector | session-selector.ts | `/resume` | `src/ui/sessions.c` | ✅ | |
| oauth-selector, login-dialog | *.ts | `/login`, `/logout` | `src/ui/login.c` | ✅ | |
| logout-account-selector, session-account-selector | *.ts | multi-account `/logout`, `/session` | — | ❌ | omc stores one credential per provider |
| move-overlay | move-overlay.ts | `/move` | `src/ui/move.c` | ✅ | localeCompare approximated (punctuation < digits < letters, case-folded) |
| composer-shape-preview | composer-shape-preview.ts | settings, setup | `ui_composer_preview` | ✅ | |
| snapcompact-shape-preview | snapcompact-shape-preview.ts | settings | — | ❌ | |
| codex-reset-fireworks | codex-reset-fireworks.ts | Codex quota reset | — | ❌ | low impact (usage reports exist since WP-24) |
| extensions/* dashboard | overlays/extensions/*, components/extensions/* | `/extensions` | — | ❌ | MCP/skills/tools parts portable; JS-extension rows ⛔ |
| annotation-overlay | annotation-overlay.ts | `/annotate` (bundled TS custom command) | — | ⛔ | JS runtime |
| stats-notice | stats-notice.ts | `/stats` | — | ⛔ | omp-stats web dashboard |
| show-images-selector, queue-mode-selector, composer-shape-registry | *.ts | not reachable upstream / extension-only | — | ⛔ | dead code or extension API |

### 5b. Command controllers

| Handler | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| handleSessionCommand / handleContextCommand / handleUsageCommand / handleJobsCommand / handleToolsCommand | command-controller.ts | `src/ui/panels.c`, `context.c`, `usage.c` | 🟡 | session/context/jobs/tools ported (see overlays above); `/usage` dashboard + renderUsageReports ported (WP-24) |
| handleHotkeysCommand | command-controller.ts | `ui_overlay_hotkeys` | ✅ | built from the live bindings (`ui_hotkeys_markdown`), identical to omp with defaults and with user overrides; rows for actions without a handler yet (Ctrl+R, Alt+P, Ctrl+Shift+O, Alt+O, approve) stay, as in omp |
| handleExportCommand | command-controller.ts | `/export` | 🟡 | placeholder HTML (see §1); no open-in-browser |
| handleDumpCommand / All / Anon | command-controller.ts | `/dump` | 🟡 | no LLM-request JSON sidecar; no `all`/`anon` |
| handleAdvisorStatus/Dump, showAdvisorConfigure | command-controller.ts | `/advisor` | 🟡 | `status` report (docks like any command report) and `dump [raw]` ported (WP-4/WP-7); `configure` missing |
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
| mcp-command-controller | mcp-command-controller.ts (2343) | `src/ui/mcpcmd.c` | ✅ | Smithery flows ❌ |
| ssh-command-controller | ssh-command-controller.ts | `src/ui/sshcmd.c` | ✅ | |
| todo-command-controller | todo-command-controller.ts | `src/ui/todocmd.c` | ✅ | all verbs; `expand`/`collapse` wait for the live todo HUD |
| live-command-controller | live-command-controller.ts | — | ⛔ | realtime voice audio I/O |
| cleanse / omfg / tan controllers | *.ts | — | ❌ | |
| tool-call-block | tool-call-block.ts | `toolcard_start` | ✅ | `out://` reply writes draw as assistant prose (tc_misc.c) |

## 6. RPC, ACP and CLI

Read `src/modes/rpc.c`, `acp.c`, `print.c`, `src/cli/cli.c`, `agent_event_to_json` (`src/agent/agent.c`);
probed `build/omc` in RPC (JSONL), ACP (fifo) and every flag/subcommand below against the mock model.
U = `packages/coding-agent/src`. RPC wire parity is WP-15 (done, remaining rows below); ACP is WP-16 (done; tests/parity/acp_diff.py diffs omp's and omc's ACP transcripts for the same scripted client).

### 6a. RPC commands (U/modes/rpc/rpc-types.ts, rpc-mode.ts; omc `src/modes/rpc.c:handle`)

Verified against omp v18.14.2 with `tests/parity/rpc_diff.py` (scripts in `tests/parity/rpc_scripts/`); e2e in
`tests/e2e/rpc_wire.py`.

| Command | omc | Status | Notes |
|---|---|---|---|
| negotiate_protocol | handle | ✅ | only v2: `{protocolVersion:2}`, else `Unsupported RPC protocol version: N` |
| prompt, abort_and_prompt | prompt_thread | 🟡 | slash / skill / file commands run (WP-4, WP-9); a prompt queued into a live run gets its `prompt_result` at that run's yield; errors lack `httpStatus` (omc keeps no provider status on the message) and `code: cost_cap`; a prompt sent during a retry backoff is queued (omp rejects it as "Agent is already processing"); no input gate (an abort does not cancel input accepted before it) |
| steer, follow_up, promote_queued_message, abort | handle | ✅ | |
| remove_queued_message | handle | ✅ | `queue` validated, removed `images` returned (`imagesDropped` over the limit) |
| abort_and_restore_queue | handle | 🟡 | user-authored entries with `images`; no `imagesDropped`/`truncated` fitting |
| new_session / switch_session / fork / branch | session_change | 🟡 | `{cancelled}` (`branch`: `{text, cancelled}` in a new session file at the entry's parent); fork refuses while busy (`session_busy`); switch takes `provider`+`modelId`; `parentSession` ignored |
| open_session | session_change | ✅ | newest non-empty session in `sessionDir` or a fresh one there: `{cancelled, resumed, sessionId, sessionFile}` |
| get_state | state_json | 🟡 | full RpcSessionState (fast/slow fields from `modelctl.h`, WP-14; `usageLimit` from the Claude slow-mode lane, WP-24); tools without `examples` |
| set_fast_mode / set_slow_mode / set_cache_warming | `modelctl_rpc` | ✅ | omp's data and error texts (WP-14; tests/e2e/tiers_rpc.py) |
| get_session_lease | — | ❌ | "Unknown command" (no session leases in omc) |
| set_ask_dialog | handle | ✅ | the `ask` tool then sends one `ask` dialog |
| goal, get_tree, set_thinking_level, get_available_thinking_levels, cycle_thinking_level, set_steering/follow_up/interrupt_mode, set_auto_compaction/retry, export_html, get_branch_messages, get_last_assistant_text, get_messages, get_login_providers, host_tool_result, host_tool_update | handle | ✅ | host_tool_update streams as `tool_execution_update` |
| navigate_tree, set_label | handle | ✅ | `entry_not_found` / `session_busy` codes |
| get_available_commands | handle | ✅ | registry builtins (aliases, hints, subcommands), then `skill:<name>`, MCP prompt and file commands (WP-9) |
| get_entries | handle | ✅ | `{entries, leafId}`, `since` cursor, `unknown_since` |
| set_todos | handle | ✅ | persisted as a `user-todo-edit` custom entry |
| set_host_tools | handle | 🟡 | definitions validated like omp; `loadMode`, `readsSkillUris` ignored |
| set_host_uri_schemes, host_uri_result | — | ❌ | |
| set_subagent_subscription, get_subagents, get_subagent_messages, cancel_subagent, steer_subagent | `rpc_sub_command` (rpc.c) | ✅ | RpcSubagentRegistry port; transcripts read from the persisted `<artifacts>/<id>.jsonl` (tests/parity/rpc_scripts/subagents.json) |
| set_event_filter | handle | ✅ | validation, `messageUpdates: "delta"` projection |
| set_model / cycle_model / get_available_models | handle | 🟡 | full `Model` objects (chat models only); compat values come from omc's catalog (a few defaults differ, e.g. `clampOutputToModelMax`), custom reasoning models lack omp's derived `thinking` |
| compact | app_compact | ✅ | `CompactionResult`; a live run is aborted first; `manual_compaction_start/end` |
| abort_retry | app_abort | 🟡 | aborts the whole run, not just the retry wait |
| bash / abort_bash | bash_main | ✅ | runs off the reader thread; BashResult with `workingDir` |
| get_session_stats | session_stats_json | 🟡 | omp shape (`tokens{}`, toolCalls, totalMessages, premiumRequests, contextUsage); omc keeps `!cmd` output in the context as a user message, so it counts as a user message |
| abort_branch_summary | app_abort_branch_summary | ✅ | |
| set_session_name | session_set_title | ✅ | |
| handoff | app_handoff | ✅ | `{}` (`savedPath` only for an auto-triggered handoff) |
| get_messages_page | handle | ✅ | port of rpc-messages.ts (base64url cursor, byte cap, `stale_cursor`, `session_busy`) |
| login / get_logout_accounts / logout | login_command | 🟡 | `open_url` / `notify` / `input` dialogs, omp's errors; omc stores one credential per provider (`credentialId` 1, generic labels) |
| btw, btw_cancel, get_btw_history | rpc.c | ✅ | |
| extension_ui_response | ui_answer | ✅ | |
| live_* / dictation_* / predict_word* | — | ⛔ | realtime voice, local STT, local tiny ML models |
| set_advisor | app_set_advisor | n/a | omc-only |

### 6b. RPC events and frames

| Event | Status | Notes |
|---|---|---|
| ready | ✅ | `supportedProtocolVersions:[1,2]`, `maxReassembledFrameBytes` |
| rpc_chunk (v2 framing) | ✅ | v2 frames over 1 MiB as base64 chunks (64 MiB cap); v1: oversized responses become the transport-limit error, other frames are compacted (`agent_end` drops streamed messages) and shrunk like rpc-frame.ts |
| agent_start, turn_start/end, tool_execution_start/end, host_tool_call/cancel, prompt_result, session_settled, command_output | ✅ | turn_start precedes the run's input messages; no `resumedToolCallIds`; omp emits an ask's first dialog before its `tool_execution_start` (async event delivery), omc after |
| message_start/update/end | ✅ | `messageId`, `partial`, delta projection; providers' extra block fields (`partialArgs`, `streamIndex`, thinking `summary`, `responseId`) are provider parity, not RPC |
| agent_end | 🟡 | `isTerminal`/`yielded` always true; no `awaitingAsyncWork`; a run aborted after its tools (cancelled ask) ends without omp's `Interrupted by user` aborted turn (prompt_result still reports `aborted`) |
| model_changed, thinking_level_changed, goal_updated, queue_update, advisor_cost_changed (startup) | ✅ | omc keeps a thinking level across a non-reasoning model, so switching back to a reasoning model reports it |
| auto_compaction_start/end, manual_compaction_start/end, auto_retry_start/end, retry_fallback_applied/succeeded | 🟡 | `auto_retry_end` has no `retryErrors` |
| available_commands_update | ✅ | at startup and after a session change |
| notice | 🟡 | info/warning/error and `source: btw-history`; no session-persistence notices |
| extension_ui_request: select/editor/input/ask/cancel/notify/open_url | ✅ | the ask tool (`--mode rpc-ui`) and tool approvals (select with approvalChoiceLabels) answer over the protocol |
| extension_ui_request: confirm/setStatus/setWidget/setTitle/set_editor_text | ⛔ | only JS extensions send them |
| cache_warming_start/end | `cachewarm.c` → `notice_rpc` | ✅ | omp's fields and outcomes (WP-14; tests/parity/cachewarm_diff.sh) |
| tool_stream_update, config_warnings_changed, advisor_yielded, ttsr_triggered, todo_auto_clear, irc_message, session_info_update, config_update, extension_error | ❌ | (todo_reminder ✅) |
| subagent_lifecycle/progress/event | ✅ | omp payload shapes and key order; progress frames are per event (omp coalesces at 150 ms) |
| host_uri_request/cancel | ❌ | |
| btw_delta / btw_record | ✅ | |
| packages/wire (collab GuestFrame/HostFrame, skillshare, stream, tsp) | ⛔ | hosted relay/stream protocols, not RPC |

### 6c. ACP (U/modes/acp/*, slash-commands/acp-builtins.ts; omc `src/modes/acp.c`)

| Method / update | Status | Notes |
|---|---|---|
| initialize | ✅ | `agent` auth method (+ `terminal` with `clientCapabilities.auth.terminal`), `sessionCapabilities.list/fork/resume/close`, top-level `_meta.steering`; clientCapabilities drive the bridges below |
| authenticate | ✅ | rejects a `methodId` initialize did not advertise |
| session/new | ✅ | absolute-cwd check, `configOptions` (mode / model / thinking) + `modes`, client `mcpServers` (config-file MCP is off in ACP like omp's `enableMCP: false`), bootstrap `available_commands_update` + `session_info_update` 50 ms after the response, MCP connect failures as a chunk. The failure text always says "until the client restarts the session" (omc has no `/mcp reauth`) and omc's connect error texts differ |
| session/load | ✅ | full `#replaySessionHistory`: user / assistant text, thinking, images, tool calls (`tool_call` + `tool_call_update` with structured content), tool results, error-only turns; an already-loaded session checks the cwd and swaps its MCP servers |
| session/resume, session/fork, session/close | ✅ | fork refuses a session with a prompt in flight; close cancels the session's prompts (`cancelled`) and disposes it |
| session/list | ✅ | `cwd` filter, cursor paging (50 per page, `nextCursor`), `_meta {messageCount, size}` |
| session/set_mode, session/set_config_option | ✅ | `current_mode_update` + `config_option_update`; model / thinking push `config_option_update` only on a change; omp's error texts |
| session/prompt | ✅ | skill commands, builtins (WP-4), residual prompts, file / template / MCP prompt commands; a prompt while one runs cancels it and queues; stop reasons `cancelled` / `max_tokens` / `refusal` (`content filter` / `refusal` errors) / `end_turn`, never a JSON-RPC error for a model error; turn `usage`; end-of-turn `usage_update` + `session_info_update`; config changes during the turn as `config_option_update` (pushed at turn end, not at the instant they happen) |
| session/cancel | ✅ | also cancels prompts queued behind the running one |
| _session/steering | ✅ | validation (`Invalid params: …`), `injected` into a live client turn (steering or `_meta.steering.queue: "followUp"`), else `promptRequired` |
| _omp/sessions/listAll, _omp/projects/list, _omp/chats/byCwd | ✅ | |
| _omp/usage | ✅ | `{reports}` from the WP-24 fetchers (raw payloads included, like omp) |
| _omp/extensions(/toggle), speech.models.list | ⛔ | JS extensions, speech |
| outbound session/request_permission | 🟡 | bash + delete/move edits, omp's four options, per-session "always" answers, `$ cmd` content, absolute locations (WP-10); sent before the tool card like omp; an abort withdraws it (`$/cancel_request`); withdrawing it when the user steers ❌ |
| outbound fs/read_text_file, fs/write_text_file | ✅ | read tool (unsaved editor buffers, disk fallback), write and edit tools (session artifacts / internal URLs stay local) |
| outbound terminal/create, terminal/output, terminal/wait_for_exit, terminal/kill, terminal/release | ✅ | bash runs in the client terminal (`terminal` content, polled output, timeout kill, abort); PTY calls stay local |
| outbound elicitation/create (form) | ✅ | the `ask` tool (offered only when the client has form elicitation) and plan approval ("Approve and execute" / "Refine plan"); ask timeouts and the usage-reserve confirmer ❌ |
| agent_message_chunk, agent_thought_chunk, user_message_chunk, plan, current_mode_update | ✅ | message ids per assistant message, narration, the final-text and turn-error fallbacks, todo reminders as `plan` |
| tool_call / tool_call_update | ✅ | port of acp-event-mapper.ts: titles, kinds (incl. delete / move / fetch / think, `xd://` writes as execute), start content (`$ cmd`, eval cells), in-progress updates, `diff` content from edit results, `terminal` content, images / resources, merged fallback text, locations. Tool `rawOutput` details follow each tool's own (partly different) details |
| usage_update | ✅ | `{size, used, cost?}` with omc's context-gauge estimate |
| available_commands_update, config_option_update, session_info_update | ✅ | re-advertising commands after a skills / commands rediscovery ❌ |

### 6d. CLI flags (U/cli/args.ts, flag-tables.ts; omc `src/cli/args.c`, launch in `src/cli/cli.c`)

WP-17 ported parseArgs: one flag table (string / optional-value / value-less flags), `--flag=value`, `--`,
`@file` args (quotes stripped), unknown flags and invalid enum values reported after the launch checks with
omp's `Error: …` + `Run \`omc --help\`` lines (exit 2). Messages after the first are prompts of their own.

| Flag | Status | Notes |
|---|---|---|
| `--flag=value` syntax | ✅ | |
| --model, --provider, --api-key, --thinking, --cwd, --config, --session-dir, --system-prompt, --append-system-prompt, --tools, --no-tools, --no-session, --no-context-files, --no-skills, --no-rules, --no-title, --print-thoughts, -p/--print, -c/--continue, --fork, --goal, --mode text/json/rpc/acp, -h, -v, piped stdin | ✅ | omp's `--goal` / `--cwd` / `--api-key` / no-TTY errors; `-m` is gone like in omp |
| -r/--resume/--session | ✅ | no value: the session picker at startup (Esc → `No session selected`); `Session "x" not found.` + hint; omc's picker overlays the fresh TUI instead of running before it |
| @file arguments | ✅ | text files as `<file name=…>`, images attached (resized, dimension note), missing → `Error: File not found` (exit 1), oversized skipped; no markit document conversion, no video contact sheets |
| --restore | 🟡 | resumes, then appends interrupted tool results + the abort record and retries (`src/coding/restore.c`); interrupted subagents are not revived (task/restore.ts) |
| --mode rpc-ui | 🟡 | accepted, runs RPC; no extension-UI channel (the ask tool stays off) |
| --models | 🟡 | scope picks the startup model (saved default when in scope, explicit `:level`) and shows omp's `Model scope: … (Ctrl+P to cycle)` notice; Ctrl+P cycles roles (as omp 18.14.2); the scope is not applied to role / `--model` resolution |
| --service-tier | ✅ | OpenAI tier override, persisted as `service_tier_change` (WP-14) |
| --export, --hide-thinking, --advisor, --no-lsp, --skills, --system-prompt-template, --smol/--slow/--plan (+ PI_*_MODEL) | ✅ | runtime overrides like omp; `SYSTEM_TEMPLATE.md` discovery |
| --max-time | 🟡 | deadline aborts the running turn and stops new requests; print mode does not wait for background jobs (no `--wait-async`) |
| --max-cost, --warn-cost | ✅ | `src/coding/costgate.c` (warn once, refuse the next request, print exit 3); subagents count through the parent's total |
| --auto-approve/--yolo, --approval-mode | ✅ | |
| omc extras --plan-mode, --max-turns | n/a | not in `--help` |
| --add-dir | ✅ | repeatable; merged with `workspace.additionalDirectories` and the resumed session's roots |
| --allow-home, --profile, --alias, --from-claude, --from-codex, --no-pty, --wait-async, --external-thinking, --provider-session-id, --prompt-cache-key | ❌ | reported as unknown flags |
| --hook, -e/--extension, --trusted-extension, --no-extensions, --plugin-dir, --no-ui | ⛔ | JS/Bun extension runtime |
| print mode `Working...` stderr line | ❌ | omp writes it before the first text-mode turn (`src/modes/print.c`) |

### 6e. CLI subcommands (U/cli-commands.ts, U/commands/*; omc `src/cli/commands.c` + `sub_*.c`)

Routing ports resolveCliArgv: subcommands behind leading flags are hoisted, omp's reserved plugin verbs
error (`error: \`omc list\` is not a top-level command. Run \`omc launch list\` …`, without omp's pointer to
the plugin manager omc lacks), omp commands omc does not port error with `is not available in omc` (exit 1),
`omc launch …` forces a prompt. Strict parsing, help pages and usage errors port pi-utils/cli.ts.

| Subcommand | Status | Notes |
|---|---|---|
| acp | ✅ | forces ACP mode; `--acp-terminal-auth` opens the TUI instead (the client's terminal auth method) |
| models | ✅ | ls/find/refresh/`<provider>`, box tables, `--json`, `--kind`, `--config`; no `-e`/`--no-extensions` (⛔) |
| login | ✅ | numbered picker, prompts on stdin, `Logged in to <name>` + store path |
| config | ✅ | list (registry order/tabs, credentials redacted), get/set/reset (`--json`, shadowing notes), `--scope`; no `init-xdg` |
| read, grep, search/q/web-search | ✅ | grep through `src/text/search.c`; search runs web_search's executeSearch (role chain, or `--model` as an explicit candidate incl. grounded chat models) and renders its card |
| auth | ✅ | export/import JSON (one credential per provider in auth.json) |
| token | ✅ | `--list`/`--account` over the single stored OAuth credential; `--force-refresh` ignored |
| completions, __complete | ✅ | bash/zsh/fish from the command table (`_omc_*` functions) |
| agents unpack | ✅ | byte-identical files |
| ssh | ✅ | |
| setup | 🟡 | no component: the first-run wizard; `python` probe; `speech` ⛔ |
| worktree/wt | 🟡 | list/clear/add over ~/.omc/wt; add is a plain `git worktree add` (no clone-first backends) |
| session (pack/unpack/moved/lease), ttsr, commit | ❌ | refused; commit needs the agentic commit pipeline |
| usage | ✅ | WP-24: breakdown, `--json`, `-p`, `-r`, `--history`, `accounts`, `reset`, `invalidate`; `clients` (no auth broker) prints the empty state; `-e`/`--no-extensions` not offered (no extensions) |
| stats, gc, shell, ps, find, compress, toks, render, play, anonymize, images, grievances, git, cleanse, gallery, bench, if-bench, dry-balance, auth-broker, auth-gateway | ❌ | refused |
| plugin/plugins/install, update | ⛔ | npm/Bun packages, self-update of the npm distribution |
| share, join, collab, clip, stream, skill/skills | ⛔ | hosted relay/stream/Skillshare services |
| say, predict, tiny-models, browser-relay | ⛔ | local TTS/ML runtimes, Chrome CDP relay |
| omc-only: migrate (listed), mcp, logout, sessions, export, glob, bash, version | n/a | hidden from `--help` except migrate |

## 7. Compaction and background systems

### 7a. Compaction

| Item | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| Method ladder `compaction.methodOrder` (remote → snapcompact → handoff → shake → soft) | session/compaction-methods.ts | `compaction.c` `cmp_auto`, `app_compact_mode` | ✅ | manual: remote/soft (mode overrides); automatic: every method with fall-through, no-reduction check; snapcompact passed over |
| Summary compaction | agent/src/compaction/compaction.ts | `compaction.c` | ✅ | prepareCompaction cut point, split-turn prefix summary, short summary, `<files>` lists, input windows, request bytes identical to omp's with the mock; token counts are chars/4 estimates |
| Transcript dialects | ai/src/dialect/* | `ai/dialect.c` | ✅ | xml, anthropic, glm, gemini, gemma, kimi, qwen3, deepseek, minimax, harmony, hermes; preferredDialect from the catalog identity or the id |
| Server-side compaction: OpenAI Responses | compaction/openai.ts | `ai_openai_remote_compact`, replay in `provider_openai_responses.c` | 🟡 | V1 `/responses/compact` + replacement-history replay; Codex V2 streaming compaction ❌ (Codex models without an explicit endpoint skip `remote`); input trimming to the window ❌ |
| Server-side compaction: Anthropic | compaction/anthropic.ts, providers/anthropic.ts | `provider_anthropic.c`, `anthropic_compact` | ✅ | `compaction` param + beta, signed block replay at the head of the kept tail, `filesText`; legacy encrypted replay ❌ |
| Server-side compaction: Bedrock | compaction/bedrock.ts | — | ❌ | Converse InvokeModel side request and Bedrock OpenAI routes |
| `compaction.remoteEndpoint` | compaction/openai.ts requestRemoteCompaction | — | ❌ | ignored: it does not make `remote` usable |
| snapcompact | packages/snapcompact | — | ❌ | renders history to PNG frames for vision models; follow-up package |
| handoff | session-maintenance.ts, session-handoff.ts | `compaction.c` | ✅ | manual + automatic (threshold focus), live-context side request, `handoffSaveToDisk`, `method: "handoff"` entry |
| shake | agent/src/compaction/shake.ts | `compaction.c` | ✅ | manual (aggressive) and automatic (default config) regions incl. fenced/XML blocks, recent-tail protection, `artifact://` offload, persisted |
| Threshold trigger | compaction-threshold.ts | `app_compaction_threshold` | ✅ | per-model `compaction.modelThresholds` |
| Mid-turn compaction (`midTurnEnabled`, on) | session-maintenance.ts | `cmp_mid_turn` | ✅ | agent `turn_boundary` hook; dead-end cut-point memo ❌ |
| Pre-prompt compaction | session-maintenance.ts | `cmp_before_prompt` | ✅ | |
| Async compaction (`asyncEnabled`, on) | session-maintenance.ts | `compaction.c` speculation | 🟡 | soft, handoff and OpenAI-remote summaries in a background thread; Anthropic-native is not speculated |
| Overflow recovery | session-maintenance.ts | `compaction.c` | ✅ | every attempt, retry fit check, dead-end rescue; payload-rejection (413) classification and `contextPromotion` ❌ |
| Pruning (`supersedeReads`, `dropUseless`, on) | compaction/pruning.ts | `compaction.c` | 🟡 | supersede + useless + tool-output pruning; prompt-cache lookback positions ❌ |
| Idle compaction | idle-maintenance.ts | — | ❌ | off by default |
| Branch summary | compaction/branch-summarization.ts | `branchsum.c` | ✅ | shares the dialect serializer |

### 7b. Background systems

| System | Upstream | omc | Status | Notes |
|---|---|---|---|---|
| Prompt cache warmer (`providers.cacheWarming` idle) | session/cache-warmer.ts | `src/coding/cachewarm.c` | ✅ | omp's timing, economics, phases and stop reasons; replays verified against omp (tests/parity/cachewarm_diff.sh); extension `cache_warming_decision` and `usage.cttl` tier adoption ❌ |
| Idle recap (`recap.enabled`, 240 s) | idle-maintenance.ts | `src/ui/recap.c` | ✅ | not journaled to history.db |
| Turn-end / ask / error notifications | event-controller.ts, tui/desktop-notify.ts | `src/tui/notify.c`, `src/ui/signals.c` | ✅ | no OSC 99 `p=?` probe (single-line OSC 99 like omp before it answers) |
| Terminal title + OSC 7501 program status | utils/run-status.ts, title-generator.ts | `src/ui/signals.c` | ✅ | no OSC 133;A resend (omc writes no OSC 133 zones), no TSP/Tern/Windows paths |
| Update check | main.ts | — | ⛔ | inert: no release feed for omc; setting hidden |
| Auto-thinking classifier | auto-thinking/classifier.ts | `autothink.c` | ✅ | on-device bucket question ⛔ |
| Todo reminders, unexpected-stop detection, tool-call loop redirect, fabricated-result abort, loop guard | session/todo-tracker.ts, unexpected-stop-classifier.ts, tool-call-loop-redirect.ts, stream-guards.ts, ai/utils/thinking-loop.ts | `guards.c`, `thinkloop.c` | 🟡 | fabricated-result abort needs in-band tool calling (not implemented) |
| Magic keywords (ultrathink/orchestrate/workflow/jevify) | modes/magic-keywords.ts | `keywords.c` | 🟡 | `workflowz` notice withheld (eval agent()/workpool() missing) |
| Sleep prevention | power-assertion-backend.ts | `src/coding/power.c` | ✅ | `systemd-inhibit` / `caffeinate` children; no ScreenSaver inhibit |
| Session cost caps (`--max-cost`, warn) | session/settings.ts | — | ❌ | |
| Prewalk / auto-prewalk, speculation | session/prewalk.ts, speculation/ | `src/coding/prewalk.c` | 🟡 | prewalk ✅ (WP-19); auto-prewalk and speculation ❌ (off by default) |
| Telemetry OTLP export | telemetry-export*.ts | — | ❌ | only with `OTEL_*` env |
| Stats (packages/stats) | packages/stats | — | ❌ | aggregation portable; the bundled web dashboard is the large part |
| Custom markdown commands, skill commands | custom-commands, skill-command.ts | `src/coding/commands.c` | ✅ | §1 (bundled TS `/review`, `/green` ❌) |
| Plugins / marketplace | extensibility/plugins | `plugins.c` (Claude plugin skills and commands) | 🟡 | install/update, plugin MCP ❌ |
| Worktrees (`/wt`, `task.isolation`) | session-worktree.ts | `src/session/worktree.c` | 🟡 | `/wt` + `worktree.onStart/onExit/cleanSource/base` done; task isolation ❌ |
| Job widgets / activity | job-widgets/, activity/ | `src/ui/agenthub.c` (activity) | 🟡 | activity index ✅ (from in-memory transcripts); job widgets ❌ |
| Secrets obfuscation | secrets/ | — | ❌ | off by default |
| IRC / agent messaging, checkpoints | irc/bus.ts, checkpoint.ts | `agents.c`, `app.c` | ✅ | |
| Hooks (`.claude/hooks`, `.omp` hooks) | extensibility/hooks | — | ⛔ | omp loads only `.ts`/`.js` hook modules under Bun |
| Extensions, custom tools | extensibility/extensions, custom-tools | — | ⛔ | Bun/JS runtime |
| Collab / share / stream | collab/, packages/collab-web | — | ⛔ | hosted relay/blob service (secret-gist share is portable) |
| Tern native rendering | tools/tern-media.ts | — | ⛔ | external native host |
| STT, live voice, predict_word, local TTS | stt/, live/, predict/, tts/ | — | ⛔ | on-device ML / audio |
| Skillshare registry | skillshare/ | — | ⛔ | hosted registry |
