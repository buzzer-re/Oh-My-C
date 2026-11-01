# omc gap audit vs omp v18.14.2

This is an inventory of omp's whole user-facing surface, checked item by item against omc. It ends in a
prioritized gap list. [status.md](status.md) records what agents ported. This file starts from the
upstream side instead, so it also covers features nobody had listed.

- **Upstream reference:** omp v18.14.2 (`7efd772`), the pin in [../UPDATE.md](../UPDATE.md).
- **omc reference:** main at `fbe0c7f`.
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

## Prioritized work packages

_(being assembled; see the inventory sections below)_

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
| /plan [prompt] | builtin-modes.ts, interactive-mode.ts `handlePlanModeCommand` | `handle_command` "plan" → `app_set_plan_mode` | 🟡 | plain toggle; no `plan.enabled` check, no "Exit plan mode?" confirm when a draft exists, no paused state, no goal/vibe mutual exclusion message parity, no plan-file path in status; approval is a generic 2-option ask (`app_propose_plan`, `src/coding/app.c`) without the plan text — see WP-1 |
| /plan-review | builtin-modes.ts | — | ❌ | re-open plan review overlay |
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
| /compact [soft\|remote\|snapcompact] [focus] | builtin-lifecycle.ts, session/compact-modes.ts | `app_compact` | 🟡 | modes missing (**bug: `/compact soft` sends "soft" as summary instructions**); while streaming omp queues the compaction for the yield (`yieldCompaction`), omc runs it immediately; no "Tokens: a -> b (saved n)" summary |
| /shake [elide\|images\|thinking] | builtin-lifecycle.ts, shake-types.ts | `app_shake(n)` | 🟡 | takes a numeric threshold instead of the 3 modes (**bug: `/shake images` → `atoi` = threshold 0**); summary text differs |
| /handoff [focus] | builtin-lifecycle.ts, command-controller `handleHandoffCommand` | `app_handoff` | 🟡 | focus instructions ignored; no streaming/compacting/"nothing to hand off" guards; no cancellable loader |
| /resume [id] | builtin-lifecycle.ts | `ui_overlay_session` | 🟡 | argument (session id/path) ignored |
| /pin | builtin-lifecycle.ts | — (Alt+K in picker only) | ❌ | |
| /btw [q] | builtin-lifecycle.ts, btw-controller.ts, overlays/btw-panel.ts | detached thread → notice | 🟡 | omp shows a live BTW panel and a history browser (`/btw` without args) |
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
`key_hook` (`src/modes/interactive.c`): esc, ctrl+c, ctrl+d, shift+tab, ctrl+o, ctrl+t, alt+m, alt+enter.
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
| input.newLine (shift+enter, ctrl+j) | editor.c | 🟡 | omc also maps alt+enter, which `key_hook` takes as follow-up while running |
| input.submit, input.tab, select.* | editor.c, components.c | ✅ | `/resume` picker ignores pageUp/pageDown |
| input.copy (ctrl+c with selection) | — | ❌ | editor has no selection |
| Vim mode (`tui.vimMode`, vim.ts) | — | ❌ | setting shown but inert |
| User overrides (keybindings.yml, legacy .json migration, profile inherit) | — | ❌ | `/migrate` does not copy keybindings either |

### 2b. App actions (`packages/tui/src/app-keybindings.ts`)

| Action (default keys) | omc | Status | Notes |
|---|---|---|---|
| app.interrupt (esc) | key_hook | 🟡 | only aborts a running turn; no cancel for idle compaction/handoff, no bash-mode clear, **no double-Esc rewind/tree** (`doubleEscapeAction`), queued messages not restored to the editor |
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
| app.message.followUp (ctrl+q, ctrl+enter) | alt+enter | 🟡 | wrong key |
| app.retry (f5, alt+r) | `/retry` only | 🟡 | |
| app.message.dequeue (alt+up, shift+up) | — | ❌ | no queue view |
| app.clipboard.pasteImage (ctrl+v), pasteTextRaw (ctrl+shift+v) | — | ❌ | no clipboard read, no image attachments |
| app.clipboard.copyLine (alt+shift+l), copyPrompt (alt+shift+c) | — | ❌ | OSC 52 helper exists |
| app.agents.hub (alt+a), app.session.observe (ctrl+s) | — | ❌ | no agent hub |
| app.session.new/tree/fork/resume (unbound by default) | slash commands | 🟡 | not bindable without overrides |
| app.session.pin/unpin (alt+k / alt+shift+k), rename/delete in picker | `src/ui/sessions.c` | ✅ | |
| app.tree.previousBranch/nextBranch | — | ❌ | `/tree` is a flat list |
| app.plan.toggle (alt+shift+p) | `/plan` only | 🟡 | |
| app.history.search (ctrl+r) | — | ❌ | |
| app.links.hint (alt+o) | — | ⛔ | extension runtime |
| app.stt.pushToTalk, app.stt.toggle, app.live.toggle | — | ⛔ | audio stack |

### 2c. Interactive flows (`modes/interactive-mode.ts`, `modes/controllers/*`)

| Flow | omc | Status | Notes |
|---|---|---|---|
| Plan approval (`xd://propose` → PlanReviewOverlay) | `app_propose_plan` → generic Ask | 🟡 | (tmux) "Approve plan local://demo-plan.md?" with Approve / Keep planning / Other: **no plan text**, refine feedback typed via "Other" is dropped, no compact/keep-context/save/tier options. Approve does end plan mode and queue `plan-mode-approved.md` |
| Plan-mode model role switch/restore (`modelRoles.plan`) | — | ❌ | |
| /plan toggle + read-only gating | `/plan`, `hook_before_tool` | ✅ | |
| Submit / steer while streaming | `on_submit` → `agent_steer` | 🟡 | "Queued (steer)" notice instead of the pending-messages view |
| Pending-messages view (`updatePendingMessagesDisplay`) | — | ❌ | |
| **Delivered steer/follow-up shown as a user message** | `ui_apply_event` handles only `async-result` user messages | ❌ | (tmux) steered text gets an answer but never appears in the transcript until a rebuild |
| Esc restores queue to editor | — | ❌ | |
| `!cmd` / `!!cmd` | `run_bang` | 🟡 | **runs synchronously on the UI thread**: (tmux) spinner freezes, Esc ignored, typed keys garbled; while streaming `!cmd` is sent as steer text |
| `$code` / `$$code` user eval | — | ❌ | sent to the model as text although omc has the kernel |
| /compact, /handoff | synchronous on UI thread | 🟡 | UI frozen, not cancellable |
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
| /btw panel (branch/copy/follow-up keys) | notice | 🟡 | |
| Session focus (view a subagent, ←←) | — | ❌ | |
| Extension UI (hook selector/input/editor/widgets) | — | ⛔ | JS extension runtime |
| Abort marker after Esc | — | 🟡 | no "aborted" line seen; not compared against omp |

## 3. Settings

Method: every `path` in `assets/tui/settings-ui.json` (omp's own settings registry, 402 entries shown
in `/settings`) was searched as a string literal in `src/` outside the settings UI (`src/ui/settings.c`).
omc reads settings only by literal key (`settings_bool/str/int/num/get("key")`), so a key that never
appears is shown and saved by `/settings` but **has no effect**. Result (main at fbe0c7f): **98 read, 304 never read**.

The `/settings` UI should either honour these or hide them. Rows are grouped by what the gap means for a
user who never opens `/settings` (default-on behaviour omc lacks) versus switches that do nothing.

### 3a. Default-on behaviour omc does not have (user sees a difference without touching settings)

| Setting (default) | What omp does by default | omc |
|---|---|---|
| `tools.approvalMode` (yolo) + `tools.approval` ({}) + `tools.restrictedFolders` | Approval tiers (always-ask, ask-and-remember, write, yolo), per-tool allow/prompt/deny, approval dialog | ❌ only ACP reads `approvalMode`; TUI never asks. Users who set "Always ask" get yolo silently |
| `grep.contextBefore` (1) / `grep.contextAfter` (3) | grep results carry 1 line before / 3 after each match | ❌ `src/tools/grep_glob.c` requests no context lines: model-facing output differs |
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
