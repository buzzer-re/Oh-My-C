# Agent runtime

There are two layers here. `src/agent/agent.c` is the generic turn loop (omp `packages/agent`).
`src/coding/app.c` is the coding-agent session (omp `AgentSession`, `session/agent-session.ts`, and
`sdk.ts`): it installs tools, hooks and persistence around the loop. Feature completeness is tracked
in [status.md](status.md) (Agent & session).

## The turn loop (`agent.c`)

`agent_prompt(agent, messages)` blocks until the run ends. `run_loop()` does this:

```
agent_start                             # a continue onto an assistant tail with tool calls (a tool replay)
                                        # runs that batch first: resumedToolCallIds, turn_start, tools, turn_end
loop:
  turn_start
  assistant = stream_assistant()        # transform_context → prepare_request → ai_stream
  message_end(assistant)
  if TTSR aborted the stream → should_retry(-1) injects the rule, restart the turn
  if error/aborted: each tool call gets a synthetic never-ran result (emitted now, added below), then
    should_retry(attempt) → REPLAY: drop the turn and request again
                          → PRESERVE / CONTINUE: keep the turn + results (+ the hook's reminder), continue
  run tool calls (see below) → toolResult messages
  turn_end
  aborted during the tool batch? → the aborted boundary turn (see Abort), stop
  stop if error, aborted, or max_turns reached
  steering queued?      → turn_start, append, continue
  tool calls made?      → continue
  stopReason pause_turn → re-sample (up to 8 times)
  follow-up queued?     → turn_start, append, continue
  else break
agent_end(messages added in this run)
```

- **Steering** (`agent_steer`) is delivered at the batch boundary, after the current turn's tools: the next turn opens
  (`turn_start`) and the steer is emitted into it. A steer queued while a batch runs (or while its calls were still
  streaming) never skips an already-emitted call (agent-loop.ts runTool, omp #10439): the expensive part, generating
  the call, is paid, and a skip only makes the model re-emit it. Before each tool starts, the steering queue is peeked
  without consuming it (`steeringQueueState`: user, agent, system, or `irc` for a peer's record) and the first hit
  latches the batch's interrupt. Only an `interruptible` tool (`agent_tool_t.interruptible`; omc's `wait`) is cut
  short, in either `interruptMode`; a not-yet-started one is skipped unless it `handles_tool_interrupt` (`wait` does:
  it runs and returns `Wait interrupted by message.`). A skipped one gets omp's `createSkippedToolResult` text
  (`Skipped due to <reason>. Do not count this skipped result as completed work or verification. After the <blocker>
  is handled on the next step, retry the skipped tool if it is still needed.`, details `{__synthetic, source:
  interrupt_skipped, executed: false}` or `{__interrupted, ..., execution: started}`). Outside `interruptMode` "wait"
  the latch also raises the cooperative steering signal (`agent_tool_steering_signal`): an auto-backgroundable bash
  backgrounds itself (`Backgrounded early to handle an incoming message; …`); in "wait" mode foreground tools finish
  undisturbed. Each tool's result message is emitted as soon as it and every earlier call have results.
  **Follow-ups** (`agent_follow_up`) are delivered only when the run would otherwise stop. Modes expose these as RPC
  `steer` / `follow_up`, and the TUI queues them from the composer.
- **Failed turns** (agent-loop.ts `createAbortedToolResult`): every tool call of a turn that ended in error or
  aborted gets a synthetic error result (`Tool call was not executed because the provider stream ended with an error
  before the tool could run: <error>` / `Tool execution was aborted: <error>`, details `{__synthetic, source:
  assistant_stop_error|assistant_stop_aborted, executed: false, upstreamError}`), emitted as a tool execution. The
  request transform keeps an errored turn's calls paired with these results (or `aborted`), so turn recovery can
  continue a preserved turn without re-running anything.
- **Abort** (`agent_abort`) sets `agent->cancel`, which providers, tools, HTTP and retry waits
  poll. Pending tool calls get `Tool execution was aborted`. `agent_abort_user` is omp's
  `abort({reason: USER_INTERRUPT_LABEL})` (Esc, `.`/`c`, loop-mode Esc, a cancelled ask form, RPC `abort`): the
  aborted assistant message reads `Interrupted by user` with errorId `Flag.UserInterrupt` instead of
  `Request was aborted`. A run aborted during a tool batch ends with agent-loop's `endAbortedBeforeModel` boundary:
  one more turn holding an empty aborted assistant message (no errorId on its `message_start`).
  An abort ends the stream at once (omp's abort race): nothing the provider pushes after `agent->cancel` reaches the
  listeners, and the aborted message is the partial as it stood, without the provider's `duration`/`ttft`. As in
  agent-loop's `retainCompletedToolCalls`, only tool calls that reached `toolcall_end` before the abort (or before an
  error) survive in an aborted or errored message; dropping one adds `stopDetails: {type:
  stream_interrupted_after_content, category, explanation}`, so a call still streaming when Esc lands leaves no
  synthetic result and no `F5 to retry` row. `turn_end`/`agent_end` carry the loop's own copy of a user-interrupted
  message, without the `UserInterrupt` errorId the session stamps on `message_end`.
- **Manual retry** (`app_retry_prepare`, omp `TurnRecovery.retry`; the tail rules are `tr_*` in
  `coding/turn_recovery.c`): the failed/aborted last turn, looked up past its synthetic tool results and any trailing
  context-only custom messages, is dropped and the agent continues. A fully failed tool batch of a completed
  `toolUse`/`stop` turn (optionally behind the aborted boundary, with only eval snapshots after it) is replayed
  instead: its results are stripped, the session leaf moves back to the calling turn and the continue re-executes the
  calls. When the live context has no failed tail, the session's display transcript (`transcript_messages`) decides,
  as for a restored session. `app_has_aborted_tool_call_tail` (omp `hasAbortedToolCallTail`) drives the TUI's idle
  `F5 to retry` row.
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
| `on_stream_delta` | TTSR matching and the stream loop guard (`thinkloop.c`): sees deltas, block starts, thinking ends and a final `AI_EV_DONE`; `AGENT_STREAM_INTERRUPT` aborts the stream and runs `should_retry(-1)` (TTSR, Gemini header runaway), `AGENT_STREAM_ERROR` ends the turn as an empty retryable error carrying `agent->stream_error` (thinking loop) |
| `before_tool` | plan-mode gating (read-only working tree) |
| `after_tool` | central output post-processing and artifact spill (`tool_postprocess_result`, [tools.md](tools.md)), TTSR tool reminders |
| `should_retry` | TTSR injection and the Gemini tool-call reminder (`attempt == -1`), overflow → compaction, turn recovery and transient error backoff ([ai.md](ai.md#retries-and-fallback)); returns `AGENT_RETRY_NONE`, `_REPLAY` (drop the failed turn), `_PRESERVE` (keep it with its synthetic tool results, spends a retry attempt) or `_CONTINUE` (keep it and add `*append`, the stream-stall reminder); a retried `Thinking loop detected` turn gets the `thinking-loop-redirect` notice |

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

Automatic titles run only in the interactive TUI, like omp's `autoTitle` (print, json, RPC and ACP set
`OMC_NO_TITLE`). Under `title.generator: fork` the title request starts when the first reply begins
(`text_start` / `toolcall_start`) as an ephemeral side turn (`app_ephemeral_turn`: session model, system prompt, tool
catalog, history, the streaming reply so far, the side-channel no-tools reminder and the session thinking level).
After `agent_prompt()` returns, `app_prompt()` runs title generation and post-turn context maintenance. The modes
then call `app_after_run()`, which may return a hidden follow-up prompt (async job results, an
advisor continuation, or a goal continuation) that the mode runs with `app_prompt_hidden()`.

Other entry points: `app_set_model`, `app_set_thinking`, `app_compact`, `app_new_session`,
`app_switch_session`, `app_fork_from`, `app_branch_to`, `app_clear_context`, `app_handoff`,
`app_retry`, `app_shake` (elide large tool results from the live context), `app_side_question`
(`/btw`: answer from context, no tools, not added to history) and `app_user_bash` (`!cmd`).

## Auto thinking, magic keywords and agent guards

These ports of omp's AgentSession behaviour are on by default and keep the model requests identical to omp's
(verified with `tests/parity/request_diff.sh`; `RD_MODE=rpc` drives `--mode rpc` for the session
continuations print mode suppresses).

- **`thinking: auto`** (`coding/autothink.c`; omp `auto-thinking/classifier.ts`, `model-controls.ts`). `app->auto_thinking`
  keeps a provisional effort (the model's `thinking.defaultLevel`, else high, clamped below max) until the first user
  turn. `app_prompt` then classifies the prompt (`tiny_preprocess`, the `level` question of
  `prompts/coding-agent/prompts/judge/auto-thinking.json` narrowed to the model's ladder, `providers.autoThinkingMaxEffort`
  ceiling) through `evalhost_judge` with a 4 s budget, or jumps to the model's max on `ultrathink`. Failures keep the
  last classified level. Resolutions are persisted as `thinking_level_change {configured: "auto"}`; an abort during
  classification drops the prompt (`abort_gen`). `app_set_thinking_level("auto")` / `app_set_thinking_auto` serve
  `/thinking`, the selector, Shift+Tab and RPC.
- **Magic keywords** (`coding/keywords.c`; omp `modes/magic-keywords.ts`, `markdown-prose.ts`). `keywords_notices`
  turns standalone prose `ultrathink` / `orchestrate` / `jevify` (never inside code fences, code spans or XML/HTML
  sections) into hidden developer messages (`attribution: user`, `customType: <id>-notice`) placed ahead of the user
  message, when the keyword's tools are enabled (active tools plus mounted xd:// devices). `workflowz` is recognised
  but not injected: its notice teaches eval `agent()`/`workpool()`, which omc lacks.
- **Guards** (`coding/guards.c`). `guards_on_event` runs the cross-turn tool-call loop guard at each turn end
  (`model.toolCallLoopGuard.*`; the redirect is steered in and persisted as a `tool-call-loop-redirect`
  custom_message) and counts mutating tool results for the todo tracker; `guards_mid_run_nudge` steers the hidden
  `mid-run-todo-nudge`. After every run `app_prompt` / `app_prompt_hidden` call `guards_after_run`, which may append a
  hidden developer message and continue the agent: the empty-stop retry (`empty-stop-retry.md`, 3 attempts), the
  unexpected-stop retry (`features.unexpectedStopDetection`: mechanical for signed-thinking-only stops, smart judges
  text stops with `judge/unexpected-stop.json`), and the stop-time todo reminder (`todo.reminders`, `remindersMax`,
  skipped when the reply waits for the user, while async jobs run, in plan mode and in print mode; surfaced as the
  TUI's todo-reminder notice card and RPC `todo_reminder`).
- **Stream loop guard** (`coding/thinkloop.c`; omp `ai/utils/thinking-loop.ts`, `stream-guards.ts`). Exact suffix cycles
  for every guarded model, plus near-duplicate and low-information paragraph heuristics for Gemini/DeepSeek/Grok;
  Claude is never guarded. A hit ends the turn as a retryable `Thinking loop detected` error, and the retry carries
  `thinking-loop-redirect`. A Gemini stream with 64 planning headers and no tool call is interrupted and re-sampled
  with `gemini-tool-call-reminder`.

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
- **Write failures** (disk full, a read-only session dir) never leave a hole in the parent chain: like omp's
  `#rewriteRequired`, the entries stay in memory and the next entry rewrites the whole file (temp file + rename). The
  first failure of an episode is reported once (`session_take_persist_error` → a `session-persistence` notice: an
  interactive warning, print mode's stderr line, an RPC `notice` frame with `source: "session-persistence"`); print mode
  retries at shutdown and, if the store still fails, prints omp's "still failing at shutdown" line and exits 1.
- **Context rebuild** (`session_build_context`) walks root → current leaf. It restarts after a
  `reset_boundary`, replaces everything before a `compaction`'s `firstKeptEntryId` with the summary
  (rendered through `compaction-summary-context.md`), includes branch summaries, and reports the
  latest model and thinking selections.
- **Branching**: `session_branch()` moves the leaf (`/tree`, `/branch`, RPC `navigate_tree`); new
  entries then grow a new branch. `session_fork()` writes a new file containing only root → leaf
  (`--fork`, `/fork`). Labels (`set_label`) annotate entries.
- **Listing / resume**: `session_list` (one project) and `session_list_all`; `-c` continues the
  newest one for the cwd; `-r <id-prefix|path>` resumes. Export to Markdown/HTML lives here too.
- **Threads**: one thread writes a session at a time (the prompt worker during a run, the UI or RPC thread while
  idle) and may read `session_t` fields directly. Other threads read through the locked calls
  (`session_current_path`, `session_leaf_dup`, `session_title_dup`, `session_path_to`, `session_get`,
  `session_build_context*`): the TUI rebuilds the transcript on `auto_compaction_end` while the worker keeps
  appending, the status line shows the title the worker may rename, RPC `get_entries` / `get_tree` run beside a
  prompt. Every mutation holds the session's recursive mutex, and code that edits entries in place (shake, the prune
  passes, the dead-end warning stamp in `compaction.c`) wraps the edit in `session_lock` / `session_unlock`. Never
  hold the session lock while waiting on the UI thread.

### Compaction (`src/coding/compaction.c`)

Context maintenance ports omp's `session-maintenance.ts`, `compaction-methods.ts`, `session-handoff.ts`
and `agent/compaction/{compaction,shake,pruning,openai,anthropic}.ts`. Token counts are local
chars/4 estimates (omp uses its tokenizer); provider usage floors them where omp does.

- **Preparation** (`prepareCompaction`): the branch after the latest readable compaction (or reset
  boundary) is cut with omp's `findCutPoint`: the oldest suffix fitting `compaction.keepRecentTokens`
  (shrunk to the recovery band under the trigger and by the billed/estimated ratio), cut only at
  user/assistant/custom turns, so a cut inside a turn is a *split turn*. File operations
  (read/write/edit tool calls plus the previous summary's lists) become the `<files>` section.
- **Soft summary** (`compact()`): the summarized messages are serialized in the summarizer's transcript
  dialect (`src/ai/dialect.c`: xml, anthropic, glm, gemini, gemma, kimi, qwen3, deepseek, minimax,
  harmony, hermes; shared with branch summaries) into omp's `<conversation>` /
  `<previous-summary>` prompt with `summarization-system.md`, `maxTokens = min(0.8 × reserve, 16384)`
  and the session thinking level. A split turn adds a turn-prefix summary; a short summary
  (`compaction-short-summary.md`) labels the entry. Oversized histories are folded through
  input-budget windows. Candidates follow `app_compaction_candidates()` (the model's compaction
  target, the session model, chat roles, the largest window).
- **Provider-native ("remote")**: OpenAI Responses `POST …/responses/compact` (provider `openai`, or a
  model with `remoteCompaction.enabled`) stores `preserveData.openaiRemoteCompaction`
  (`replacementHistory`, `compactionItem`) whose items replay verbatim on later requests; the Anthropic
  compaction beta (`compat.supportsServerCompaction` on a supported route) sends `compaction:
  {type: "summarize", instructions}` with `compact-2026-09-04`, keeps the signed block in
  `preserveData.anthropicCompaction` and replays it as a `compaction` block at the head of the kept
  tail. A native failure falls through to the next method. The context is built for the model that reads
  it (`session_build_context_as`): a model that cannot replay the newest OpenAI payload (another provider, or a
  non-Responses API) stands on the newest compaction it can read and carries the raw history behind the
  unreadable one, a model switch rebuilds the agent's messages when the two models stand on different
  boundaries, and compaction preparation re-expands a trailing payload the active model cannot reuse.
- **Method ladder** (`compaction.methodOrder`, default remote → snapcompact → handoff → shake → soft):
  manual `/compact` uses remote/soft (a `/compact <mode>` overrides the order); automatic passes
  (`cmp_auto`) run the first usable method and fall through on failure or when a pass would not reduce
  the context. Handoff is skipped for overflow; snapcompact is not implemented (explicit
  `/compact snapcompact` reports it, the ladder passes over it). Shake elides tool results and large
  fenced/XML regions outside a protected recent tail into an `artifact://` and falls back when the
  context stays above the recovery band. Handoff generates the document over the live context
  (`tool_choice: none`, `auto-handoff-threshold-focus.md`; `compaction.handoffSaveToDisk` writes
  `handoff-<time>.md` to the session artifacts) and commits it with `method: "handoff"`, rendered
  through `handoff-summary-context.md`. A document without a `## Goal` heading line (text before it is allowed) is
  rejected (an automatic handoff moves on to the next method, `/handoff` reports "Handoff generation produced an invalid
  document").
- **Triggers**: after each turn (`cmp_after_turn`: stale-read supersession and useless-result elision
  with `compaction.supersedeReads` / `dropUseless`, tool-output pruning, then the threshold), between
  tool-loop turns (`cmp_mid_turn`, `compaction.midTurnEnabled`, through the agent's `turn_boundary`
  hook), before a prompt (`cmp_before_prompt`), and on context overflow (every attempt, with a retry
  of the failed turn). With `compaction.asyncEnabled`, a background thread prepares the summary or
  handoff document inside `[threshold − lead, threshold)` and a threshold pass in the grace band
  defers to it; the armed result is committed by the next pass when still valid for the branch.
- **Dead ends**: when preparation finds nothing or a commit leaves no headroom (or no retry fit), a
  rescue elides inside the kept tail, then drops images; otherwise omp's dead-end warning is shown
  and stamped on the compaction entry. A threshold pass is not started at all while the system prompt and
  tool schemas alone exceed the recovery band: it blocks automatic continuation and warns once per session
  (`Compaction cannot create headroom …`), so it never loops every turn.
- **Commit**: `session_append_compaction_ex()` writes omp's entry fields (`shortSummary`,
  `tokensAfter`, `method`, `providerReplayThroughEntryId`, `details`, `fromExtension`, `preserveData`),
  the context is rebuilt and `auto_compaction_start` / `auto_compaction_end` events (omp's shapes) go to
  RPC and the TUI.

`app_compaction_threshold()` gives the trigger (`compaction.thresholdTokens`, `thresholdPercent`,
per-model `compaction.modelThresholds`, else the reserve-based budget); the status-line gauge uses the
same value. `app_handoff()` (`/handoff`) commits a handoff document the same way.

## System prompt assembly (`src/coding/discovery.c`)

`discover_all()` collects three kinds of input. `build_system_prompt()` then renders omp's
templates with the result. The system prompt is a list of parts:

1. `system-prompt.md`, or `custom-system-prompt.md` when a custom prompt is given (`--system-prompt`
   text or file, or `SYSTEM.md` in `.omc/`, `.omp/` or the agent dir), with tool inventory and
   labels, `xd://` device docs, skills, rules, internal-URL docs, delegation settings (`delegationTier`:
   the catalog delegation bias, `balanced` without a rule, lifted to `eager` by `task.eager` in the main
   agent only; `task.*`), personality (`personality` setting or `PERSONALITY.md`), the model
   name, and the append prompt (`--append-system-prompt` or `APPEND_SYSTEM.md`) composed after
   generated memory and auto-learn blocks (`user-append.md`);
2. `project-prompt.md` (context files, cwd);
3. `plan-mode-active.md` while plan mode is on;
4. MCP server instructions.

`app_rebuild_system_prompt()` re-renders it when tools, plan mode or memory change. The prompt is
byte-stable across turns: the volatile date and cwd ride on user messages instead (below).

**Context files.** User level: `<agent dir>/AGENTS.md`, `~/.agent(s)/AGENTS.md`. Project level,
farthest ancestor first: `.agents/AGENTS.md`, `.agent/AGENTS.md`, `AGENTS.md`,
`CLAUDE.md` (the first wins at one depth). Also the nearest `.omc/` / `.omp/` copy,
`.claude/CLAUDE.md`, `.gemini/GEMINI.md`, `.github/copilot-instructions.md`, and
`contextFiles.extra`.

Project discovery has three ancestor walks, each with omp's bound (`coding/discovery.c`):
- `.agent(s)/` dirs and `.claude/skills` (agents.ts `getProjectPathCandidates`, claude.ts `loadSkills`): the cwd up to
  the repo root (a `.git` entry), else up to home, else up to `/`; home itself is skipped.
- native `.omc/` / `.omp/` dirs and their skills (builtin.ts `getProjectAncestorDirs`): the same, but never at or past a
  ceiling (home, `os.tmpdir()`, `/tmp`, `/var/tmp`), and without a repo an inactive user layout (`<dir>/.omc/agent`,
  `<dir>/.omp/agent` that is not the agent dir) is skipped.
- `AGENTS.md`, `CLAUDE.md` and `contextFiles.extra` (helpers.ts `loadStandaloneContextFiles`): without a repo up to home
  (included) under it, else up to `/` (excluded); a repo nested below home continues to home (excluded); other repos
  stop at their root; dot-named directories are skipped.

**Skills** (`SKILL.md` with frontmatter). Sources: `.omc/skills`, `.omp/skills`,
`<agent dir>/skills`, Claude Code `~/.claude/skills` and `.claude/skills` up the tree,
`.agents/skills`, installed Claude Code / agent-plugins plugins (`coding/plugins.c`), and
auto-learn managed skills (lowest priority). The first name wins. Skill descriptions are compressed
into short routing hints by `coding/skilldesc.c` (`skills.compressDescriptions`). A session renders
a frozen snapshot from the cache `<agent dir>/skill-descriptions.json` and compresses missing ones in
the background for later sessions: one plain-string user message per skill on the `smol` role (never the
session model), at most 4 at once, `temperature: 0`, 1,024 output tokens and reasoning disabled. A hint fitting 160
characters is used however many words it has; an unusable answer is remembered per model and validation revision.
Each provider attempt passes the session tree's cost cap (a refusal aborts it) and adds its cost to the cap. These
requests start with the session, once its cost gate exists (the TUI starts them after its first frame), so on a
single-slot local server the first prompt queues behind them (in omp too). With
thinking disabled they stay short: a local Qwen gets `enable_thinking: false` from the detected compat
(docs/ai.md).

The system prompt lists a snapshot of the skills taken at startup (and on a cwd change). A skill that `learn` or
`manage_skill` writes or deletes mid-session reloads the registry only, so `skill://` and `/skill:` find it at once
while the prompt stays byte-identical for the provider cache. A `skill://` name no loaded skill owns rediscovers the
main session's skills (at most once every 2 seconds; a subagent's list is fixed). The model learns of what changed
through a hidden `skill-notice` message (`prompts/system/skill-notice.md`) that rides with the next user prompt:
skills added, skills whose description changed, and skills removed, compared with the prompt's list plus the notices
still in context. It is sent only while `skillful` is on and `read` is active (session-tools.ts `takeSkillNotice`).

**Rules** (markdown with `description`, `globs`, `alwaysApply`, `condition`, `scope`, ...).
Sources: `.omc|.omp/rules`, `<agent dir>/rules`, `RULES.md`, `.cursor/rules/*.mdc`,
`.cursorrules`, `.windsurfrules`, `.clinerules`, `.github/instructions/*.instructions.md`, and omp's
built-in rules (`ttsr.builtinRules`). Rules with a stream trigger become TTSR rules. The others are
listed in the prompt and readable as `rule://<name>`.

**Plugins** (`coding/plugins.c`): installed Claude Code plugins and omc's own registry are read for
their skills only. Plugin hooks, commands and marketplaces are not ported.

### Date/cwd reminder

This ports `session/date-cwd-reminder.ts` (`coding/dcr.c`). The `transform_context` hook prepends
`date-cwd-reminder.md` (date and cwd in a `<system-reminder>`) to the first user turn of each
request, so the system prompt stays byte-stable. Every turn keeps the reminder it was first sent with. A changed
reminder (new day, new cwd) goes on a newly appended user turn, or on a hidden developer turn after the last turn when
there is none. Turns are recognised by a key (role, timestamp, attribution, ordinal), and state is held per
conversation root, up to 8.

The main loop journals each placement as a `date-cwd-reminder` custom entry, so a resumed session or revived subagent
resends its turns with the same bytes. The state resets when the history is replaced rather than continued: a switched
session, a tree move, or another writer's placements after a reload. Placements before the latest `reset_boundary`
are ignored. The auto-learn capture keeps an unjournaled injector. Side requests running next to the turn (`/btw`,
speculative compaction) never touch the session: their placements wait for the next request on the writer's thread.
The setting is `dateCwdReminder`.

### Continue shortcut and rejected prompts

Typing `.` or `c` (`c` is a normal reply during a `/guided-goal` interview) resumes without a visible user message: a
prompt the model has not answered runs again as is, otherwise the hidden `manual-continue.md` directive goes out, and an
empty conversation reports "Nothing to resume". A compaction or branch summary is not a prompt to run again: a
conversation left with only a summary (after `/handoff` or a compaction) resumes with the directive. An empty Enter
while input is queued interrupts the turn and forces the queue into the continuation: user follow-ups move behind the
steers, and a message the loop took into the turn it aborted runs again. A prompt rejected before dispatch for a missing model or credential goes
back to the editor, never over a draft typed meanwhile. With `retry.fallbackChains` set the prompt goes out instead, so
the retry path can fall back. Background results that settle during a manual compaction wait for it to replace history.

## TTSR: time-traveling stream rules (`coding/ttsr.c`)

This ports `session/ttsr-coordinator.ts`. Rules with a regex `condition` watch the streaming output
of the scope they name (`text`, `thinking`, `tool`, `tool:edit(*.rs)`). On a match, the interrupt
mode decides what happens (`always`, `prose-only`, `tool-only`, `never`):

- **interrupt**: abort the stream, drop the partial message, inject `ttsr-interrupt.md` as a
  hidden `ttsr-injection` custom message, and retry the turn;
- **tool reminder**: append `ttsr-tool-reminder.md` to that tool call's result;
- **deferred**: queue the reminder as a follow-up after the message.

The repeat policy is `ttsr.repeatMode` (`once`, the default, or `after-gap`: again after `ttsr.repeatGap`
completed turns), overridden per rule by a `repeatMode` frontmatter key (`once`, `after-gap`, or `always`, a
one-turn gap). A rule's record is the turn its reminder lands in: an interrupt's retry, a deferred reminder's
continuation, or a tool reminder's own turn, so a reminder can never re-fire its rule. Records are in memory
only (not restored on resume). AST conditions (`astCondition`) and judged `question` triggers
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
A continuation an advisor note starts supersedes the reviews of the work it revises still queued (the one whose
note started it is kept): they are held and reviewed with the continuation's final boundary, so they cannot re-raise
what is being fixed. An `advise` call from a cancelled review records nothing (`Dropped: this review was cancelled.`).

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
  The private capture turn advertises the main turn's tool list in its order, so it reuses that turn's prompt cache
  (it sends the main session id as the cache key and keeps its own provider session id); every tool but `learn` and
  `manage_skill` is a copy that refuses the call without running anything.

## Titles (`coding/title.c`)

Under `title.generator: fork` (the default), when the reply to the user's first substantive message
begins, a side request on the same model, system prompt, tools and history asks for a card title
such as `🧪 FLAKY: Fix flaky park tests`. This reuses the prompt cache. Low-signal input
(greetings, acks) is skipped. The `tiny` generator and the fallback use the title model with
`title-system.md`. Titles are applied on the agent thread and written to the title slot.
`OMC_NO_TITLE=1` disables them (RPC mode always does). Every billed title request (the fork, each
title-model attempt, errors included) is journaled as an off-transcript `model_usage` entry with
`purpose: "title"` and charged to the session's cost gate, so `/usage` and `--max-cost` count it.

## Plan mode

`src/coding/planmode.c` holds the state; the TUI flow is in `src/modes/interactive.c` and the review
overlay in `src/ui/planreview.c`. `/plan`, `plan.defaultOnStartup` (fresh interactive sessions only),
omc's `--plan-mode` flag, ACP `session/set_mode` or the API option enter it. Entering appends a
`mode_change` entry (`plan`, `data.planFilePath`, default `local://PLAN.md`) and, interactively,
switches to the `plan` role model (restored on exit). Resuming re-enters the mode recorded by the last
`mode_change` on the branch; `/new` (and RPC `new_session`) ends plan mode, enabled or paused, and restores the
pre-plan model before the new session records it. The system prompt gains `plan-mode-active.md`, and the `before_tool` hook
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
  with `plan-mode-compact-instructions.md`), *Approve and keep context* (its context estimate dim
  beside it; disabled above 95% usage), *Refine plan* (listed first and preselected only while the plan
  has annotation/deletion feedback, which becomes the next prompt) and *Save* (path prompt, write; the
  review stays open, plan mode and the session are untouched). Approvals exit plan mode, apply the slider's role model,
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
  `security-reviewer`, `task` and `sonic` (omp `task/agents.ts` order), then user agents
  from `<agent dir>/agents` and project agents from `.omc/agents`, `.omp/agents`, `.claude/agents`
  (markdown with frontmatter: tools, model, ...).
- **task.steps** (off by default): items carry `steps` instead of `task` (schema and description switch). The steps
  go out in at most 4 prompts (`subagent-step.md`, earlier prompts taking the extra steps); the first is the
  assignment, each later one is sent once the child delivered the previous one with a clean final message, whose text
  reaches the spawner as an IRC progress note (`subagent-step-report.md`). The last prompt's result is the spawn's;
  `task-steps` custom entries record the prompts and how many went out. omc has no `--restore` of subagents, so they
  are written but not read back.
- **Spawning** (`coding/agents.c`, the process-wide agent registry, port of `AgentRegistry` plus the
  IRC bus): `task` creates child `app_t`s via `app_subagent_new()`. Children share the parent's MCP,
  LSP and artifact store, run at most `task.maxRecursionDepth` deep, and use the `task` model role
  or the definition's model. With `async.enabled` (the default), each spawn becomes a background
  job and the result is delivered automatically. Otherwise the call blocks.
- **Peer messaging**: subagents stay alive and idle after their run. `write agent://<id>` (or
  `agent://all`, main agent only) sends a message, an idle agent wakes to answer, and `wait` collects replies.
  `agent://<id>?progress` is an interim note. Broadcast legs and progress notes ask nothing, so they queue as asides
  (`agent_aside`) for the next step boundary: they never end a `wait` or interrupt bash. A direct message ends a wait, whose
  result carries every queued message (`details.waited`).
- **`wait`** (`tools/write_bash.c`, port of `tools/wait.ts`): with nothing new it returns after a window that climbs 5s, 10s,
  30s, 1m, 4m (inside a 5-minute prompt cache) across back-to-back idle waits, restarting after news or a 60 s gap; a
  back-to-back wait that is the only tool call of its turn takes the 4m top rung at once. The elapsed line says what to do
  once nothing else is left: the top-level interactive session is told to end its turn (results and messages start its
  next one; the description says so too), every other caller to call `wait` right away alone. From the second rung it
  appends the `wait-idle.md` nudge. A wait woken by a job keeps collecting for up to 1.5 s while other watched jobs run. A wait for a reply
  ends at once when the asked agent can no longer answer soon: it finished its turn, it waits on the caller, or a dialog of
  its own (`ask`, an approval) waits on the user.
  Outputs are readable at `agent://<id>`, transcripts at `history://<id>`, and running agents at
  `proc://`.
- **Registry for the UI and RPC** (WP-22): every spawn keeps an omp `AgentProgress` of its current
  executor run (task spawn, IRC wake or a chat prompt from the hub: status, current tool, recent tools
  and output, request/tool/token counts, context, cost, active time, resolved model, `modelOverride`).
  `agents_list()` returns omp `AgentRecordLike` rows (status `running` / `idle` / `parked` (session
  released) / `aborted` (killed)); `agents_kill` aborts and tombstones an agent, `agents_revive`
  (ensureLive) reopens a killed one or rebuilds a parked one from its transcript, `agents_prompt` revives
  and steers a running turn or starts a turn of an idle agent (a quiet job: nothing is delivered to the
  parent), `agents_pin` keeps a session alive while the UI views it. `agents_listen` notifies the UI of
  every change (coalesced on the UI loop); `agents_observe` feeds the RPC subagent channels with omp's
  `SubagentLifecyclePayload` / `SubagentProgressPayload` / `{id, event}`. `agents_job_tree` collects the
  jobs of a session and every subagent below it for `/jobs` and the jobs badge (each omc session has its
  own job manager; omp shares one).
- **Transcripts** of subagents persist next to the parent's artifacts as `<artifacts>/<id>.jsonl`
  (`app_subagent_new_at`, `session_create_file`), like omp; RPC `get_subagent_messages` reads them.
  The run's output lands beside it as `<artifacts>/<id>.md` (executor finalize: always for the spawn's
  own run, for a follow-up only when it is a successful, non-empty reply the user or the spawner asked
  for); the hub inspector's Output row shows it.
- **Waiting on background work** (WP-22b, task/executor.ts quiescence barrier + agent-session.ts
  `#hasPendingAsyncWake`): a subagent's run completes only after the jobs it owns settle; each settled
  batch is folded into its run as an `async-result` follow-up turn, so its answer is the last turn
  after every result (`settle_owned_jobs`). The main session's run that ends while jobs can still wake
  it is a scheduling pause: RPC reports `agent_end {isTerminal: false, yielded: true,
  awaitingAsyncWork: true}` and `prompt_result {sessionSettled: false}`, the wake run reports no
  `prompt_result` and its terminal `agent_end` emits `session_settled`; the TUI keeps the working row
  ("Waiting for background job <id>" / "Waiting for N background jobs: a, b, c +N") and the status-line
  activity window open, Esc ends the wait ("Stopped waiting · N background jobs still running (/jobs)")
  and later results fold into context without starting a turn until a turn ends on its own. Job
  wakes that land together deliver as one batch (100 ms coalescing).
- **Task result advisories** (task/index.ts `composeSpawnAdvisory`): a call that resolves ≥2 items to the
  generic `task`/`sonic` agents gets the `Tip:` specialization nudge (scout hint while scout is
  spawnable); a call with ≥2 async siblings gets the `Coordinate:` line (peer messaging on, spawn
  capacity left). Before returning, the call reports `Spawned agent `<id>`...` / `Spawned N agents...`;
  afterwards its card frames carry omp's texts (`Running background task <id>...`, `Running agent
  <id>...`, `Background task <id> complete.` / `failed.`), forwarded to RPC as `tool_execution_update`.
- **Subagent sessions** carry no personality block (sdk.ts passes `personality: "none"` for subagents),
  no `todo` tool (parent-owned bookkeeping) and no `lsp` unless `task.enableLsp`; the peer roster lists
  `main` / agent-type display names with kind `main` / `sub`, running before idle, and the progress gist
  as activity. `agent://`, `history://` and `proc://` are always listed in the internal-URL docs.
- **Jobs** (`tools/jobs.c`, port of `async/` AsyncJobManager): async bash (`async: true`), bash
  auto-backgrounded after `bash.autoBackground.*` thresholds, task jobs, and service watchers.
  Settled results go to the owning agent as an `async-result` follow-up, or wake an idle session
  through `app_t.wake`. The `wait` tool and `/jobs` inspect them. Print mode waits for outstanding
  jobs (`async.printWaitMs`) before the final answer.
- **Services** (`tools/services.c`, port of `launch/`): named long-lived processes under a detached
  supervisor with log/port readiness checks, controlled through `proc://`.
