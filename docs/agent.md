# Agent runtime

There are two layers here. `src/agent/agent.c` is the generic turn loop (omp `packages/agent`).
`src/coding/app.c` is the coding-agent session (omp `AgentSession`, `session/agent-session.ts`, and
`sdk.ts`): it installs tools, hooks and persistence around the loop. Feature completeness is tracked
in [status.md](status.md) (Agent & session).

## The turn loop (`agent.c`)

`agent_prompt(agent, messages)` blocks until the run ends. `run_loop()` does this:

```
agent_start
loop:
  turn_start
  assistant = stream_assistant()        # transform_context → prepare_request → ai_stream
  message_end(assistant)
  if TTSR aborted the stream → should_retry(-1) injects the rule, restart the turn
  if error/aborted and should_retry(attempt) → retry the turn
  run tool calls (see below) → toolResult messages
  turn_end
  stop if error, aborted, or max_turns reached
  steering queued?      → append, continue
  tool calls made?      → continue
  stopReason pause_turn → re-sample (up to 8 times)
  follow-up queued?     → append, continue
  else break
agent_end(messages added in this run)
```

- **Steering** (`agent_steer`) is delivered after the current turn's tools. If steering arrives
  while tools are pending, the remaining calls of that assistant message are skipped with
  `Skipped due to queued user message.`. **Follow-ups** (`agent_follow_up`) are delivered only
  when the run would otherwise stop. Modes expose these as RPC `steer` / `follow_up`, and the TUI
  queues them from the composer.
- **Abort** (`agent_abort`) sets `agent->cancel`, which providers, tools, HTTP and retry waits
  poll. Pending tool calls get `Tool execution was aborted`.
- **Events** (`agent_event_t`, serialized by `agent_event_to_json` in omp's shape): `agent_start`,
  `agent_end`, `turn_start`, `turn_end`, `message_start`, `message_update` (wraps the `ai` stream
  event), `message_end`, `tool_execution_start` (with the extracted intent),
  `tool_execution_update` (partial results), `tool_execution_end`. Listeners subscribe with
  `agent_subscribe()`. Delivery from tool threads is serialized by `ev_mu`.

### Tool execution

The tool calls of one assistant message run in order, grouped like this:

- **Exclusive** tools (`agent_tool_t.exclusive`) run alone: `bash`, `write`, `edit`, `ask`,
  `eval`, `ast_edit`, `debug`, `goal`, `checkpoint`, `rewind`.
- **Shared** tools in a consecutive run (read, grep, glob, find, lsp, web_search, task, MCP tools,
  ...) run in parallel threads, at most 8 at a time, and are joined before the next group. Results
  are appended in call order, whatever the completion order.

Each call goes through: intent extraction → `before_tool` hook (may block with a reason) → JSON
Schema validation (`schema_validate_args`, `agent/validate.c`: a port of omp's
validateToolArguments with its argument clean-ups — double-encoded keys, null/"null"/rejected ""
optionals, numeric strings, enum whitespace — up to 5 coercion passes, and omptype's messages such as
`pattern must be a string (was missing)`) → `execute` → `after_tool` hook. Validation errors return
`Validation failed for tool "<name>": ...` with the received arguments (2-space indented JSON,
`{original, normalized}` when something was coerced), as omp does. Unknown tools return `Tool <name> not found`.

### The intent field `i`

This ports `agent-loop.ts` `injectIntentIntoSchema` / `extractIntent`. When
`tools.intentTracing` is on (the default; `PI_NO_INTENT=1` / `OMC_NO_INTENT=1` disable it), every
tool's wire schema gets a leading `i: {type: string, description: "concise intent"}` property,
required unless the tool sets `INTENT_OPTIONAL` / `INTENT_OMIT`. The loop strips `i` from the
arguments before validation and reports it on `tool_execution_start`, where the TUI shows it. A
value over 200 characters is rejected without running the tool. Tool `examples` are rendered into
the wire description as Python-style calls (`ai/src/dialect/examples.ts`).

### Hooks (`agent_hooks_t`)

The loop's only coupling to the coding agent is these callbacks, all installed by `install_agent()`
in `app.c`:

| Hook | omc implementation |
|---|---|
| `transform_context` | date/cwd `<system-reminder>` injection (below) |
| `prepare_request` | resolve the API key per request (OAuth refresh), cache retention (`providers.cacheRetention` / `PI_CACHE_RETENTION`), thinking budgets, temperature, text verbosity, OpenAI service tier |
| `on_stream_delta` | TTSR matching; returning non-zero aborts the stream |
| `before_tool` | plan-mode gating (read-only working tree) |
| `after_tool` | central output post-processing and artifact spill (`tool_postprocess_result`, [tools.md](tools.md)), TTSR tool reminders |
| `should_retry` | TTSR injection (`attempt == -1`), overflow → compaction, transient error backoff ([ai.md](ai.md#retries-and-fallback)) |

omp's user-facing extension system (TypeScript hooks, custom tools and commands under
`extensibility/`) is not ported. Host programs add tools through the RPC `set_host_tools` command or
`omc_register_tool()` ([modes.md](modes.md)).

## Session runtime (`app.c`)

`app_create()` resolves the model and thinking level, creates the `tool_env_t`, runs discovery,
connects MCP servers, opens or creates the session, installs the agent, restores goal state and
builds the system prompt. Its listener (`on_agent_event`) then:

- persists every `message_end` to the session (hidden steering messages with a `customType` become
  `custom_message` entries, as in omp);
- accumulates cost, output tokens and the last context size;
- feeds the goal, title, advisor and auto-learn controllers;
- delivers settled background jobs as steering (`async-result`);
- applies checkpoint/rewind cuts.

After `agent_prompt()` returns, `app_prompt()` runs title generation and auto-compaction. The modes
then call `app_after_run()`, which may return a hidden follow-up prompt (async job results, an
advisor continuation, or a goal continuation) that the mode runs with `app_prompt_hidden()`.

Other entry points: `app_set_model`, `app_set_thinking`, `app_compact`, `app_new_session`,
`app_switch_session`, `app_fork_from`, `app_branch_to`, `app_clear_context`, `app_handoff`,
`app_retry`, `app_shake` (elide large tool results from the live context), `app_side_question`
(`/btw`: answer from context, no tools, not added to history) and `app_user_bash` (`!cmd`).

## Sessions (`src/session/session.c`)

Sessions are omp-compatible JSONL (format version 3, `session/session-manager.ts`) at
`<agent dir>/sessions/<encoded cwd>/<timestamp>_<id>.jsonl`. The cwd is encoded relative to `$HOME`
or `$TMPDIR` when inside them. Files are created lazily: nothing is written until the session has an
assistant message, and then the whole session is written at once.

```
{"type":"title","v":1,"title":"…","source":"auto","updatedAt":"…","pad":"   …"}   ← fixed 256-byte slot
{"type":"session","version":3,"id":"<uuidv7>","timestamp":"…","cwd":"…"}
{"type":"model_change","id":"a1b2c3d4","parentId":null,"timestamp":"…","model":"anthropic/…"}
{"type":"message","id":"…","parentId":"a1b2c3d4","timestamp":"…","message":{role,content,…}}
…
```

- The **title slot** is the first line, padded to exactly 256 bytes, so a rename rewrites it in
  place with `pwrite()` and no full rewrite is needed (`session-title-slot.ts`). A `title_change`
  entry records the history. User-set titles beat automatic ones.
- **Entries** form a tree through `id` / `parentId`. Types: `message`, `model_change`,
  `thinking_level_change`, `compaction`, `branch_summary`, `custom`, `custom_message`, `label`,
  `reset_boundary`, `title_change`, `mode_change` (goal/plan state).
- **Context rebuild** (`session_build_context`) walks root → current leaf. It restarts after a
  `reset_boundary`, replaces everything before a `compaction`'s `firstKeptEntryId` with the summary
  (rendered through `compaction-summary-context.md`), includes branch summaries, and reports the
  latest model and thinking selections.
- **Branching**: `session_branch()` moves the leaf (`/tree`, `/branch`, RPC `navigate_tree`); new
  entries then grow a new branch. `session_fork()` writes a new file containing only root → leaf
  (`--fork`, `/fork`). Labels (`set_label`) annotate entries.
- **Listing / resume**: `session_list` (one project) and `session_list_all`; `-c` continues the
  newest one for the cwd; `-r <id-prefix|path>` resumes. Export to Markdown/HTML lives here too.

### Compaction

`app_compact()` ports omp's summarization compaction:

1. Walking back from the leaf, keep about `compaction.keepRecentTokens` (20,000, estimated as
   chars/4) and cut at a user-message boundary.
2. Serialize the older messages into `<conversation>`. With a previous summary, use
   `compaction-update-summary.md`, otherwise `compaction-summary.md`, plus optional `/compact`
   instructions.
3. Summarize with `modelRoles.compaction` (else the session model) at low effort.
4. Append a `compaction` entry and reload the context.

Auto-compaction runs after a prompt when the last context size reaches the threshold computed by
`app_compaction_threshold()`: `compaction.thresholdTokens` or `compaction.thresholdPercent` if set,
else omp's reserve-based budget (`window - max(15% of window, compaction.reserveTokens)`). The
status-line gauge uses the same threshold. A context-overflow error triggers one compaction and a
retry. `app_handoff()` (`/handoff`) writes a handoff document and compacts in place.

## System prompt assembly (`src/coding/discovery.c`)

`discover_all()` collects three kinds of input. `build_system_prompt()` then renders omp's
templates with the result. The system prompt is a list of parts:

1. `system-prompt.md`, or `custom-system-prompt.md` when a custom prompt is given (`--system-prompt`
   text or file, or `SYSTEM.md` in `.omc/`, `.omp/` or the agent dir), with tool inventory and
   labels, `xd://` device docs, skills, rules, internal-URL docs, delegation settings (catalog
   `delegationBias`, `task.*`), personality (`personality` setting or `PERSONALITY.md`), the model
   name, and the append prompt (`--append-system-prompt` or `APPEND_SYSTEM.md`) composed after
   generated memory and auto-learn blocks (`user-append.md`);
2. `project-prompt.md` (context files, cwd);
3. `plan-mode-active.md` while plan mode is on;
4. MCP server instructions.

`app_rebuild_system_prompt()` re-renders it when tools, plan mode or memory change. The prompt is
byte-stable across turns: the volatile date and cwd ride on user messages instead (below).

**Context files.** User level: `<agent dir>/AGENTS.md`, `~/.agent(s)/AGENTS.md`. Project level,
walking from the repo root to the cwd: `.agents/AGENTS.md`, `.agent/AGENTS.md`, `AGENTS.md`,
`CLAUDE.md` (AGENTS.md wins at the same depth). Also `.omc/` / `.omp/` copies,
`.claude/CLAUDE.md`, `.gemini/GEMINI.md`, `.github/copilot-instructions.md`, and
`contextFiles.extra`.

**Skills** (`SKILL.md` with frontmatter). Sources: `.omc/skills`, `.omp/skills`,
`<agent dir>/skills`, Claude Code `~/.claude/skills` and `.claude/skills` up the tree,
`.agents/skills`, installed Claude Code / agent-plugins plugins (`coding/plugins.c`), and
auto-learn managed skills (lowest priority). The first name wins. Skill descriptions are compressed
into short routing hints by `coding/skilldesc.c` (`skills.compressDescriptions`). A session renders
a frozen snapshot from the cache `<agent dir>/skill-descriptions.json` and compresses missing ones in
the background for later sessions.

**Rules** (markdown with `description`, `globs`, `alwaysApply`, `condition`, `scope`, ...).
Sources: `.omc|.omp/rules`, `<agent dir>/rules`, `RULES.md`, `.cursor/rules/*.mdc`,
`.cursorrules`, `.windsurfrules`, `.clinerules`, `.github/instructions/*.instructions.md`, and omp's
built-in rules (`ttsr.builtinRules`). Rules with a stream trigger become TTSR rules. The others are
listed in the prompt and readable as `rule://<name>`.

**Plugins** (`coding/plugins.c`): installed Claude Code plugins and omc's own registry are read for
their skills only. Plugin hooks, commands and marketplaces are not ported.

### Date/cwd reminder

This ports `session/date-cwd-reminder.ts`. The `transform_context` hook prepends
`date-cwd-reminder.md` (date and cwd in a `<system-reminder>`) to the first user turn of each
request, and remembers which reminder each turn was first sent with. The cached prefix therefore
never changes. A new day or cwd attaches the new reminder to the newest unsent user turn. Setting:
`dateCwdReminder`.

## TTSR: time-traveling stream rules (`coding/ttsr.c`)

This ports `session/ttsr-coordinator.ts`. Rules with a regex `condition` watch the streaming output
of the scope they name (`text`, `thinking`, `tool`, `tool:edit(*.rs)`). On a match, the interrupt
mode decides what happens (`always`, `prose-only`, `tool-only`, `never`):

- **interrupt**: abort the stream, drop the partial message, inject `ttsr-interrupt.md` as a
  hidden `ttsr-injection` custom message, and retry the turn;
- **tool reminder**: append `ttsr-tool-reminder.md` to that tool call's result;
- **deferred**: queue the reminder as a follow-up after the message.

Each rule fires once per session. AST conditions (`astCondition`) and judged `question` triggers
are recognized but not evaluated.

## Goal mode (`coding/goal.c`)

This ports `goals/*`: one persistent objective per session, with `active`, `paused`,
`budget-limited`, `complete` and `dropped` states. It provides the `goal` tool (exclusive), hidden
goal and todo context per prompt, token/time budget accounting with a budget-limit steer, and
automatic continuation turns after a run (stopping when there is no progress). Interrupts pause the
goal. State persists as `mode_change` entries and is restored on resume. Entry points: `/goal`,
`/guided-goal`, `--goal <objective>`, RPC `goal`. Continuations are allowed in the modes listed in
`goal.continuationModes`.

## Advisor (`coding/advisor.c`)

This ports `advisor/*`. A second model (`modelRoles.advisor`) shadows the primary agent on its own
thread. It receives incremental transcript updates, can verify suspicions with read-only
`read`/`grep`/`glob`, and reports through an `advise` tool with severity `nit`, `concern` or
`blocker`. Notes reach the primary as hidden `<advisory>` messages: steered mid-run, or (at or above
`advisor.continueSeverity`, default `concern`) as a continuation turn after the run.
Lower-severity notes appear as notices. It is enabled with `advisor.enabled`, `/advisor` or RPC
`set_advisor`.

## Memory and auto-learn (`coding/memory.c`, `memstore.c`, `autolearn.c`)

- `memory.backend` selects `off` (default) or `local`. The other omp backends (hindsight, mnemopi,
  sharpshooter) are recognized and treated as off.
- The **local** backend runs omp's startup pipeline in the background. Stage 1 extracts summaries
  from idle past sessions (`stage_one_*` prompts, leases, retries, watermarks). Phase 2 consolidates
  them into `MEMORY.md`, `memory_summary.md`, `skills/` and `rollout_summaries/` under the memory
  root, with secrets redacted. A per-session memory block is added to the system prompt append, and
  `memory://root` exposes the files.
- omp keeps job state in SQLite. omc stores the same tables in `<agent dir>/memory-state.json`,
  with `flock` plus atomic rewrite per transaction (`memstore.h`).
- **Auto-learn** (`autolearn.enabled`) adds the `learn` and `manage_skill` tools, isolated managed
  skills in `<agent dir>/managed-skills` (strict names, symlink and size checks), standing prompt
  guidance, and a post-stop private capture turn.

## Titles (`coding/title.c`)

Under `title.generator: fork` (the default), when the reply to the user's first substantive message
begins, a side request on the same model, system prompt, tools and history asks for a card title
such as `🧪 FLAKY: Fix flaky park tests`. This reuses the prompt cache. Low-signal input
(greetings, acks) is skipped. The `tiny` generator and the fallback use the title model with
`title-system.md`. Titles are applied on the agent thread and written to the title slot.
`OMC_NO_TITLE=1` disables them (RPC mode always does).

## Plan mode

`src/coding/planmode.c` holds the state; the TUI flow is in `src/modes/interactive.c` and the review
overlay in `src/ui/planreview.c`. `/plan`, `plan.defaultOnStartup` (fresh interactive sessions only),
omc's `--plan-mode` flag, ACP `session/set_mode` or the API option enter it. Entering appends a
`mode_change` entry (`plan`, `data.planFilePath`, default `local://PLAN.md`) and, interactively,
switches to the `plan` role model (restored on exit). Resuming re-enters the mode recorded by the last
`mode_change` on the branch. The system prompt gains `plan-mode-active.md`, and the `before_tool` hook
enforces a read-only working tree:

- `write` only to `local://…` or `xd://propose`;
- `edit` only on `local://` files;
- `bash` commands that look state-changing (rm, mv, git commit/push/checkout, package installs,
  `sed -i`, redirections to files, ...) are refused.

A plan turn that ends in plain text gets the decision reminder (a developer message) and one more
turn with `tool_choice: required`, at most three times before a user turn, `ask` or a proposal.

**Proposal.** The agent writes `local://<slug>-plan.md`, then writes the slug to `xd://propose`. The
plan is resolved like omp's `resolveApprovedPlan` (supplied slug, a state plan the scan cannot see,
`local://*plan.md` newest first, the state plan) and the write result carries
`details.xdev = {tool: "propose", mode: "execute", args: {title}, inner: {planFilePath, title, planExists}}`.
What happens next depends on the host:

- **TUI** (`plan_review_ui`): the result is `Plan ready for review.` and the next model request is
  aborted as an internal transition (`agent_abort_next_silently`: the empty aborted message is stamped
  `__omp.silent_abort__`). Once the run settles the reviewed path is promoted into the plan state and
  the Plan Review overlay opens on the alternate screen. Its choices: *Approve and execute* (new
  session, local artifacts copied, plan rewritten there), *Approve and compact context* (compaction
  with `plan-mode-compact-instructions.md`), *Approve and keep context* (disabled above 95% usage),
  *Refine plan* (the annotation/deletion feedback becomes the next prompt) and *Save and quit* (path
  prompt, write, exit plan mode, new session). Approvals exit plan mode, apply the slider's role model,
  autosave (`plan.autosave`), name an unnamed session after the plan and run the hidden
  `plan-mode-approved.md` developer prompt; the overlay stays up (`— submitting…`) until then.
- **No review surface** (print, RPC, ACP): approved at once (`Plan approved at …`), the plan becomes
  the reference, ACP reports `current_mode_update`.
- **`--plan-yolo`**: the plan phase is armed before the first prompt; the first proposal autosaves,
  leaves plan mode, switches to `--plan-yolo-into` (default `@smol`) and steers `plan-yolo-handoff.md`.

After approval the plan file is the session's plan reference: `plan-mode-reference.md` is injected
(hidden) once per history rewrite (new session, compaction) unless the approval prompt already carried
it.

## Checkpoint and rewind

These are off by default (`checkpoint.enabled`) and limited to top-level sessions. `checkpoint`
records the boundary (message count and session entry). `rewind` with a report cuts the live context
back to that boundary at the end of the turn, branches the session to the checkpoint entry, and
appends the report (`rewind-report.md`). The exploration in between is replaced by its summary. Both
tools are exclusive and mountable as `xd://` devices.

## Subagents and background jobs

- **Agent definitions** (`tools/task.c` `agent_defs_load`): the bundled `scout`, `reviewer`,
  `security-reviewer`, `seance`, `task` and `sonic` (omp `task/agents.ts` order), then user agents
  from `<agent dir>/agents` and project agents from `.omc/agents`, `.omp/agents`, `.claude/agents`
  (markdown with frontmatter: tools, model, ...).
- **Spawning** (`coding/agents.c`, the process-wide agent registry, port of `AgentRegistry` plus the
  IRC bus): `task` creates child `app_t`s via `app_subagent_new()`. Children share the parent's MCP,
  LSP and artifact store, run at most `task.maxRecursionDepth` deep, and use the `task` model role
  or the definition's model. With `async.enabled` (the default), each spawn becomes a background
  job and the result is delivered automatically. Otherwise the call blocks.
- **Peer messaging**: subagents stay alive and idle after their run. `write agent://<id>` (or
  `agent://all`) sends a message, an idle agent wakes to answer, and `wait` collects replies.
  Outputs are readable at `agent://<id>`, transcripts at `history://<id>`, and running agents at
  `proc://`.
- **Jobs** (`tools/jobs.c`, port of `async/` AsyncJobManager): async bash (`async: true`), bash
  auto-backgrounded after `bash.autoBackground.*` thresholds, task jobs, and service watchers.
  Settled results go to the owning agent as an `async-result` follow-up, or wake an idle session
  through `app_t.wake`. The `wait` tool and `/jobs` inspect them. Print mode waits for outstanding
  jobs (`async.printWaitMs`) before the final answer.
- **Services** (`tools/services.c`, port of `launch/`): named long-lived processes under a detached
  supervisor with log/port readiness checks, controlled through `proc://`.
