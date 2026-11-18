# Terminal UI

The interactive mode must look and behave **exactly** like omp: same glyphs, colors, layout and
keys, verified cell by cell. The UI has two layers:

- `src/tui/`: a small framework that ports `packages/tui`'s core (`tui.ts`, `terminal.ts`,
  `keys.ts`, editor, components, theme, symbols, markdown). It knows nothing about agents.
- `src/ui/`: the coding-agent screens that port omp's `chat/`, `chrome/`, `status-line/`,
  `prompt/`, `setup/`, `overlays/` and `tools/` renderers. They are driven by
  `src/modes/interactive.c` (omp's `InteractiveMode`).

What already matches omp and what remains is tracked in [status.md](status.md) (Modes & UI).

## Framework (`src/tui/`)

### Component model (`render.h`)

A component is a struct that embeds `tui_component_t` first, plus a vtable:

```c
render(c, width, strvec_t *out)   // append owned lines (no '\n') for `width` columns
handle_key(c, key)                // optional; true if consumed
invalidate(c)                     // optional; drop caches (theme change, resize)
destroy(c)                        // optional; default free(c)
```

`tui_container_t` is a vertical stack that owns its children. Overlays (`tui_overlay_show`) are
composited over the viewport, anchored bottom, center or top, take focus, and restore the previous
focus when hidden. Basic components are in `components.c` (Text, TruncatedText, Box, Spacer,
Loader, SelectList). The multi-line editor is in `editor.c`.

### Differential renderer

`tui_run()` owns the loop. Each frame:

1. renders the root container's lines at the terminal width and composites the overlays;
2. appends `\x1b[0m\x1b]8;;\x07` to every line so styles and hyperlinks never bleed;
3. truncates over-wide lines;
4. compares against the previous frame and rewrites only the changed lines, wrapped in synchronized
   output (`CSI ?2026 h/l`) when the terminal supports it, then flushes in one `write`.

Rendering is **inline**, without the alternate screen: the transcript scrolls into the terminal's
native scrollback, as in omp. Rows above the screen are **committed history** (omp's retired history
batches): they are never repainted, so a change up there (a block re-wrapping, a card updating, a read
group's count) never clears the terminal. The renderer re-aligns the rows still on screen
(`align_viewport`: the shift that lines the old visible rows up with the new frame) and rewrites only
the screen rows that differ. A block that can still change calls `tui_hold_history(tui, row)` while it
renders (the streaming reply from its last Markdown block on, a running tool card, the open read
group, the welcome banner while its intro plays): held rows never scroll into scrollback; when the
frame outgrows the screen they are clipped and the screen shows the frame's tail (omp: an unfinalized
block never retires; the viewport shows its tail), and they are written to scrollback once they settle.
When a scrolled frame shrinks so its last row would end above the screen bottom (an autocomplete list,
picker or tall block going away), the rows above are pulled back down so the composer stays on the
last screen row, as omp's bottom-anchored viewport does (they are also still in scrollback).
Frames are framed like omp's paints (cursor hidden, `CSI ?2026` synchronized output, autowrap off,
each row written then erased to its end). Clearing screen and scrollback is reserved for explicit
full requests (`tui_request_full_render`: session replaced, transcript rebuilt, thinking visibility
toggled) and settled resizes in `rebuild` mode (below); the clear is ED2 then ED3 (tmux scrolls an ED2'd
screen into pane history, which ED3 then wipes). `tui_full_redraw_count` exposes the clears to tests, and
`OMC_TUI_DEBUG=<file>` logs one line per frame (kind: first/diff/above/none/full:request/full:resize/
resize:width/resize:height/resize:append/resize:alt, first changed row, viewport top, bytes written,
old/new excerpt of the changed row).

**Resize** is a transaction, ported from omp's tui.ts (`tui_handle_resize`, called on SIGWINCH). Every
step of a burst is folded into burst flags (width changed, resized, shrank, grew, total grow), so a drag
that ends where it started still counts; the burst settles once the size has been quiet for 120 ms.
Meanwhile: inside tmux in `rebuild` mode with synchronized output, and on Warp (`TERM_PROGRAM`), nothing
paints until the settle (the old screen stays up); everywhere else the burst borrows the alternate screen
and paints the frame's tail there, after blanking the live rows on the normal screen first (direct
terminals only; the erase rides in the first resize frame's synchronized update), so the terminal's
reflow can only push committed rows into scrollback. `PI_TUI_RESIZE_IN_PLACE=1|0` forces either path.
At the settle, `tui_set_resize_scrollback` (setting `tui.resizeScrollback`, default `rebuild`; the TUI
alone starts in `PI_TUI_RESIZE_SCROLLBACK` or `preserve`) decides (omp `#prepareResizeReplay`):
`rebuild` erases screen and scrollback and replays every row at the new width when the width moved or a
step shrank the height (the borrow's exit is fused into that write, and in tmux the replay renews DEC 2026
every 16 KiB and, from 7 MiB, is preceded by the bottom-row `↻ Rebuilding…` notice); `append` writes every
row from the viewport's anchor down, a current-width copy below the retained history, on a width change;
`preserve`, Warp, and height-only grows repaint the screen only. Non-destructive settles first recover the
reflowed anchor with a CPR (`CSI 6n`, 200 ms, once more on a multiplexer or after a grow; omp
`#resolveResizeAnchor`'s direct-terminal bound and multiplexer clip model as the fallbacks), so a frame
shorter than the screen stays where it was. Held rows (`tui_hold_history`) keep their rule through all of
this: a rebuild writes the rows above the hold and the screen's tail, and the held block enters
scrollback once it settles. A size change while a fullscreen page is up repaints the page; the normal
screen settles the same way when the page closes. 100 ms after a borrow settles a new size change paints
directly (omp's echo suppression). omp differences left: omp's viewport is its live (unretired) blocks, so
in `preserve` it writes the blocks that no longer fit after the resize into history again at the new
width; omc has no block ledger and treats the rows above the repainted screen as history. The CPR is
synchronous and untagged (omp tags each request with a cursor column), and the tmux replay does not
REP-encode its padding.
`tui_set_paint_listener` reports each written frame like omp's `TuiPaint` (rows newly committed to
scrollback, the rows on screen, `reset` for a scrollback clear, `alt` for a fullscreen page); `/record`
feeds its `.ompcast` encoder from it.

Fullscreen pages (`/settings`, `/resume`, the Models hub, `/hotkeys`, the setup wizard, the BTW
history, plan review) borrow the alternate screen through `tui_alt_screen(tui, on)`: the normal screen's frame is
parked meanwhile and resumes untouched on exit (the terminal restores that screen, nothing is
repainted); a full render requested while a page is open applies to the normal screen when it closes. An external editor
opened from a page (`$VISUAL`/`$EDITOR`) gets the normal screen back meanwhile; only the page is
repainted after it.

A focused component may emit `TUI_CURSOR_MARKER` (an APC sequence) where the hardware cursor should
sit (for IMEs). The renderer strips it and parks the cursor there. Renders are coalesced and
throttled like omp's (`MIN_RENDER_INTERVAL_MS`, 30 fps), backing off to twice the last frame's cost
(at most 200 ms) when frames get expensive.

**Threading:** everything runs on the UI thread except `tui_request_render()`, `tui_post()` (run a
function on the UI thread) and `tui_stop()`. The agent worker thread never touches components. It
posts events, which `interactive.c` applies on the UI thread (`uiev_t`, coalesced partial-message
updates). Timers (`tui_add_timer`) drive spinners, smooth streaming reveal and live tool cards.

### Terminal (`term.c`) and input (`keys.c`)

- Raw mode, size, buffered output, and protocol toggles: bracketed paste (`?2004`), focus events
  (`?1004`), and the kitty keyboard protocol (queried at start with a DA1 sentinel; when there is no
  kitty reply, xterm `modifyOtherKeys` mode 2 is used instead).
- Signals (SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGCONT) arrive through a self-pipe.
  When a terminating signal arrives outside the loop, the handler restores the terminal itself
  (async-signal-safe) and re-raises. An `atexit` hook restores it too. ISIG is off, so Ctrl+Z
  arrives as a key: `app.suspend` calls `tui_suspend`, which restores the terminal, stops the
  foreground process group with SIGSTOP (omp `handleCtrlZ`) and starts a fresh frame on SIGCONT.
- The key decoder handles UTF-8, C0 controls, CSI/SS3 with xterm modifiers, kitty CSI-u (with text
  and event types), modifyOtherKeys, Alt via an ESC prefix, bracketed paste, focus, and terminal
  replies. A lone ESC is held for `TUI_ESC_TIMEOUT_MS`. Key ids (`ctrl+c`, `alt+enter`,
  `shift+tab`) are matched against the keybinding table.

### Keybindings (`keys.c`, `config/keybindings.c`)

- The table holds omp's whole registry in upstream order: the 32 `tui.editor.*` / `tui.input.*` /
  `tui.select.*` actions of `keybindings.ts` and the 45 `app.*` actions of `app-keybindings.ts`, with
  the same default chords (`app.session.new/tree/fork/resume`, `app.tree.*` and `app.stt.toggle` are
  unbound). User overrides replace an action's chords; ids are lowercased like omp's
  `normalizeKeyId`. When the user binds Ctrl+Q or Shift+Up to another action, it drops out of the
  `app.message.followUp` / `app.message.dequeue` defaults (omp `getFallbackKey`).
- `kbcfg_init_global` (interactive start) loads `keybindings.yml` from the agent dir, else
  `keybindings.yaml`, else the legacy `keybindings.json` (JSONC). Legacy names (`interrupt`,
  `cursorUp`, `selectModel`, ...) are renamed; a renamed config or one read from the .json is written
  back as `keybindings.yml` in registry order, byte-identical to omp's. `kbcfg_load_merged` layers a
  profile over the default profile's bindings (read only); omc has no profiles yet, so nothing passes
  an inherited dir.
- `key_hook` (`modes/interactive.c`) dispatches every app action through `tui_kb()`: first the global
  listeners (`app.thinking.toggle`, `app.history.search`, `app.editor.external`, `app.local.open` (the `/local`
  picker), `app.display.reset`, `app.tools.expand`), then, while the composer has focus, `CustomEditor.handleInput`'s order (paste image,
  raw paste, suspend, model select / role cycle, thinking cycle, interrupt, clear, exit, dequeue, copy
  prompt, plan, session actions, follow-up, copy line, retry). `/hotkeys` (`ui_hotkeys_markdown`) is built
  from the live table like omp's `buildHotkeysMarkdown`: unbound actions read `Disabled`, and exit keys
  split into a forward-delete row and an exit row.
- Not handled yet although bound (as in omp they still appear in `/hotkeys`): `app.model.selectTemporary`,
  `app.tools.toggleVisibility`, `app.agents.hub`,
  `app.session.observe`, `app.tree.*`, `app.links.hint` and the speech actions; see
  [gaps.md](gaps.md) §2b. `app.approval.approve` is handled by the approval dialog (`ui/approval.c`).
- Non-tty file descriptors are supported (tests): probes are skipped and the size defaults to 80×24.
- `notify.c`: terminal identity (omp `detectTerminalId`, including the tmux client query) and desktop
  notifications in omp's order: Herdr / cmux CLIs, tmux DCS passthrough + BEL, Zellij, else the terminal's
  protocol (OSC 9, OSC 99 single line, BEL), plus the `notify-send` / `gdbus` toast for BEL terminals in a
  Linux desktop session. Writes are flushed at once (omp `writeTerminalSequence`).
- A hang-up (SIGHUP, or EOF / EIO / POLLNVAL on the input fd) stops the loop; a tool thread still waiting on
  the ask dialog or form is answered as cancelled so the exit never blocks on it.

### Editor and composer (`editor.c`, `composer.c`)

The editor ports omp's `editor-component.ts`: word motion, an emacs kill ring (`kill-ring.ts`:
consecutive kills accumulate, Ctrl+Y yanks, Alt+Y cycles older entries right after a yank),
character jump (Ctrl+] / Ctrl+Alt+] then a character), undo (200 steps; omp's `#insertCharacter`
coalescing: a run of word characters is one step, a whitespace chunk and the word after it start new ones),
history, large-paste markers (`[Paste #N, +L lines]`, kept for editors without a host hook such as the ask
prompt), slash-command, `@file` and `#` autocomplete with omp's ranking (each refresh is a fresh list: the
selection restarts at the top), and Enter-runs-command semantics. Slash completion also matches aliases (the alias is the
row), completes arguments (`tui_ac_default_t.args`: subcommands, the live `/effort` levels) and paints
dim ghost text after an end-of-line cursor (`tui_editor_ghost`: the selected item's remainder +
usage, else the command's inline hint); Enter on an argument completion applies it and submits only
when the argument is already fully typed (`#selectedSlashArgumentIsAlreadyTyped`). The metadata comes
from the shared registry (`src/coding/slashcmd.c`) through `src/ui/slash.c` (live descriptions such as
`Fast: on`, `Slow mode: off`, `Extended context: off` replace the static text like omp's
`getTuiAutocompleteDescription`), followed by MCP prompt
commands (`… (bundled)`), `skill:<name>` commands, file commands (✎, `<hint> - <description>` and the hint as
ghost text while no argument is typed) and prompt templates. `skill:` commands collapse into one
`/skill:` namespace row (`N skills`) until the query commits to the namespace or a bare name / hyphen
segment out-ranks every other command (`collapseSkillNamespace`); accepting the row reopens the popup on
the skills. A `/token` after prompt text (`fix it /co`) completes skills only and replaces just the token
(`buildMidPromptSkillCompletions`). The editor's token hook (`tui_editor_set_token_hook`) turns a completed
`/skill:<name>` token of a registered skill into an atomic `✦ name` chip (skillChipStyle) that expands back on
submit. A submitted skill renders as `SkillMessageComponent` (`MV_SKILL` in `src/ui/chat.c`): a callout with
the skill rail, chip and line count when the token led the draft, else a user bubble with the chip; Ctrl+O
adds the rendered skill prompt. A `side: true` file command opens the /btw panel titled `/<name> args`.
Its frame is drawn by a composer style, one of omp's 8
shapes: `box`, `band` (default), `claude`, `pi`, `borderless`, `rule`, `field`, `rail`
(`composer.shape`).

### Theme, color and symbols

- `theme.c`: named color slots (`TUI_THEME_SLOTS`) with omp's theme keys. omp's JSON theme format is
  used unchanged (`{name, vars, colors, symbols}`). The 102 bundled themes are embedded
  (`assets/tui/themes`), and user themes load from `<agent dir>/themes/<name>.json`. The defaults are
  `titanium` (dark) and `light`, chosen by terminal background. Truecolor is used when `COLORTERM`
  (and similar) allows it; otherwise colors map to the nearest xterm-256 color. `colorBlindMode` shifts
  diff-added colors toward blue (`ui/appearance.c`).
- `color.c`: OKLCH/HSV math and the per-session accent color derived from the title (omp
  `utils/color.ts`, `theme/session-color.ts`).
- `symbols.c`: glyph presets `unicode` (default), `nerd`, `ascii` and spinner frames, read from
  `assets/tui/symbols.json` (generated from omp's `symbols.ts`). Code uses `tui_sym("status.success")`
  with omp's key names, never literal glyphs.

### Markdown and highlighting

- `markdown.c` renders a marked-style block lexer with omp's per-token spacing: headings, lists with
  hanging wraps, task lists, `▏` quotes, sharp tables with row rules, code fences, OSC 8 links,
  entities and glyph-preset rules. Unclosed fences render to the end, so streaming is safe. Nesting
  depth is capped. While a reply streams, a trailing lone `-` line that would underline a paragraph
  (a setext heading) is held back as a bullet being typed. The copy overlay's code/quote blocks
  (`tui_md_extract_blocks`) come from this same lexer.
- HTML follows omp's `markdown.ts` and its marked fork. Inline, any well-formed tag (`<name …>` up to
  the first `>` outside quotes) or `<!--…-->` comment is one raw token, so escapes inside it stay as
  written. `<code>…</code>` runs become code spans; every other tag goes through
  `normalizeHtmlForTerminal`. That function drops comments and decodes entities (a surrogate entity draws as omp's
  terminal writer draws the lone UTF-16 unit: a low one is U+FFFD, a high one combines unchecked with the next unit
  and swallows it, so `&#xD800; ` is `␠` and `&#xD83D;&#xDE00;` the emoji). `<br>`/`<hr>` break
  the line, `<p>`/`<blockquote>` separate paragraphs, and `<ol start=N>`/`<ul>`/`<li>` become
  numbered or `•` lines indented by list depth. `<span>`/`<text>` vanish and any other tag stays
  verbatim. A block (`isHtmlStart`'s element list or a comment) runs to the line holding its
  closing tag (or `-->`), or to the end of the input. A standalone `<hr>` in it draws a rule and a
  balanced `<blockquote>` draws a quote.
- `highlight.c` uses hand-written lexers per language family that assign the color category omp's
  syntect-based highlighter (`crates/pi-natives` highlight.rs) would give each token: comment,
  keyword, function, string, number, type, operator, punctuation, inserted/deleted.
  Unsupported languages render plain. The Regular Expression lexer (`re`, and the grep card's
  header pattern) follows syntect's RegExp grammar, checked cell by cell against omp
  (tests/tui/golden/regex.ansi); it restarts at each line, where syntect carries an open set across.
  Python builtin types/exceptions and the JS/TS grammar (identifiers variable color, call and control parens and
  index brackets unscoped, declared names, `new`/`extends` targets, object keys, annotation and conditional colons)
  were probed against omp 18.22.0 (tests/tui/golden/hlpyts.ansi, hlts.ansi).
- `ansi.c` holds the ANSI-aware width, slice, truncate and wrap helpers (escape sequences are zero
  width, tabs are 3 columns). `fuzzy.c` is omp's word-local fuzzy matcher (`fuzzy.ts`) used by
  pickers.

## Coding-agent screens (`src/ui/`)

| Module | Shows | omp source |
|---|---|---|
| `welcome.c` | gradient π logo with a 3 s intro sweep, half-block wordmark, version, weighted tip from `tips.txt` | `prompt/welcome.ts` |
| `statusline.c` | status band / bottom bar in every preset and layout (`box`, `band`, `plain-*`), context gauge with the compaction threshold, the `👥 N` running-subagent and `⚙ N` background-job badges leading the right group, model/thinking/cost/VCS segments, the `mode` segment (plan, goal, `/loop`: `↻ Loop: next prompt repeats` / `Loop running` / `⏸ Loop paused` with the budget left and `until: <cmd>`) (the model segment carries the fast-mode `icon.fast` badge when a fast tier is realized and the warning-coloured Anthropic usage-limit label), brand spinner and turn timer, composer hints | `status-line/*`, `prompt/composer-hints.ts` |
| `chat.c` | transcript container (blank-row separated blocks), user bubbles, assistant prose and thinking, notices, custom/summary/TTSR messages, pinned error banner, working row ("Waiting for background job <id>" while a finished run only waits on jobs; Esc stops waiting, WP-22b), retry/compaction rows, replay from the session tree on resume and branch (a returned `task` card replays with the job's newest frame) | `chat/*`, `chrome/*` |
| `toolcards.c` + `tools/tc_*.c` | tool execution cards: bash, read (grouped), write, edit diffs, grep/glob trees, todo, ask, goal, find, ast, debug, task (live async progress), eval cells, wait, MCP, lsp, web_search, generic fallback. Ctrl+O expands. The repaint ticker, previews, background-job frames, run/prior cards, the call-id map (`u->tools`), synthetic cards and the current/pinned assistant view borrow views; `mv_destroy` calls `toolcard_forget`, so a view removed any way (transcript rebuild after a compaction or a focus switch, a superseded retry, an evicted card) leaves none of them dangling | `chat/tool-execution.ts`, `read-tool-group.ts`, `tui/src/tools/*`, `render/*` |
| `setup.c` | first-run wizard: splash, sign in, default model, glyph preset, composer shape, theme, outro. Writes `setupVersion: 2`. `/setup [providers]` (`ui_setup_run_providers`) runs the sign-in scene alone without touching `setupVersion` or replaying the welcome intro | `setup/*` (`lazy.ts` runProviderSetupWizard) |
| `login.c` | provider list and "Login to …" dialog running `config/auth*` flows on a worker thread | `overlays/oauth-selector.ts`, `login-dialog.ts` |
| `modelhub.c`, `modelbrowser.c` | Models hub (`/model`, Alt+M): roles, providers, model browser, persistence to `modelRoles.*` | `overlays/model-hub.ts`, `hub-frame.ts`, `model-browser.ts` |
| `modelpicker.c` | session-only model picker (Alt+P, `/switch`): bottom-anchored box over the transcript (`tui_overlay_fill_screen`), the shared browser over the session scope (`ui_session_model_browser`), over-context rows (picking one compacts first through `iq_compact_then`), `@` quick-role rows colored by the Ctrl+P segment palette (`ui_segment_palette`), Alt+P again toggles task mode (red chrome via `ui_box_tone`), Tab / Shift+Tab step all models → each provider (`cycle_scope`), ←/→ a thinking slider for the highlighted model (session mode; status `· thinking <Step>` / `· no thinking`, the pick applies its level); `ui_switch_session_model` also serves `/switch <model>` | `overlays/model-picker.ts`, `controllers/selector-controller.ts` (`#showModelPicker`, `switchSessionModel`) |
| `sessions.c` | `/resume` selector: search, all projects, pins, rename, delete; two passes (rows from title slots at once, a background full scan swaps in prompts, status and untitled sessions, keeping query, selection, deletes and renames) | `overlays/session-selector.ts` |
| `usage.c` | `/usage` fullscreen dashboard on the alternate screen: provider cards (worst window first, stacked or inline bars, `% left`, reset countdowns, saved resets, Daybreak, `untouched:` line), activity heatmap (53 weeks from the session files, theme-accent ramp), `r` refresh, Enter/Tab/`d` Details (the classic per-account report, scroll / wheel), `q`/Esc back / close; `/usage reset` account picker in the composer's place (two-press confirm); the per-turn usage row text (`display.showTokenUsage` / `display.showTurnTime`, added by `chat.c` live and on replay) | `overlays/usage-dashboard.ts`, `usage-display.ts`, `usage-row.ts`, `reset-usage-selector.ts`, command-controller `renderUsageReports` |
| `settings.c` | `/settings` from omp's own registry dump (`assets/tui/settings-ui.json`) | `overlays/settings-selector.ts`, `components/settings-list.ts` |
| `btw.c` | `/btw`: inline answer panel below the transcript (streams Markdown, Esc cancels then closes, `c` copy, `f` follow up, `b` branch, `btw.inject`: an `<inject>` block behind the `btw.rail`, `app.btw.inject` (Alt+I) steers or starts a main-session turn with it, unexpanded, once per answer), fullscreen BTW history (topic list + answer pane, follow-up composer, copy, cancel, inject), the controller (session moves dispose it) | `controllers/btw-controller.ts`, `overlays/btw-panel.ts`, `overlays/btw-inject.ts`, `overlays/btw-history-panel.ts` |
| `formdlg.c` | the `ask` tool's HTML form in the composer's place: one column (section titles, context, option rows with an `Other:` input, one-row `‹ value ›` dropdowns that open a box, text inputs and textareas, Submit) in a fixed-height `Ask` panel paged at section breaks (`Ask · 2/3`, `X (cont.)`), per-option notes (`app.ask.note`: `n` on option rows, Alt+N on dropdowns; `✎ note` / `✎ draft` markers, the note editor covers the rows under its option, only notes on chosen options are submitted), validation on Submit (`N to fix`), `ask.timeout` countdown (`Ask (Ns)`, any key resets it), `app.ask.hide` row, draft input guard, `<img>` pictures loaded in the background (http(s), `data:`, files; rasters through the inline image layer) | `html-form/dialog.ts`, `html-form/media.ts`, `html-form/paginate.ts` |
| `ask.c` | the questions dialog (now only `cfg://` change approvals) in the composer's place | `overlays/ask-dialog.ts`, `hook-editor.ts` |
| `approval.c` | the tool-approval prompt in the composer's place: `Allow tool: <t>` panel, details (accent, one clipped row each, 8 rows then a scrollbar; `app.tools.expand` swaps in the full prompt wrapped, PgUp/PgDn scroll), choices (`☐ Remember across sessions` toggle + grants, `Approve` / `Approve Once`, `Approve <t> for this session`, `Deny`), key-hint footer; `app.approval.approve` (Ctrl+Enter / Ctrl+Q) approves, Esc denies, Enter and the approve key arm 500 ms after mount; the composer stays beneath it and takes typing: a draft turns Deny into `Deny and send message`, and the message sent with Enter is queued as steering (delivered first) and denies the call; the bridge blocks the tool thread (one dialog at a time; an abort or UI exit denies); the call reads as blocked (`kind=permission`) in the title / OSC 7501 and Warp gets `permission_request` / `permission_replied` | `overlays/approval-dialog.ts`, `controllers/extension-ui-controller.ts` (`showCollabAwareApproval`) |
| `histsearch.c` | Ctrl+R prompt history search over `prompt-history.jsonl` | `overlays/history-search.ts`, `session/history-storage.ts` |
| `hooksel.c`, `attach.c` | hook selector with option descriptions (the large-paste menu); attachment chips and band, image / large / raw paste | `overlays/hook-selector.ts`, `prompt/attachment-chips.ts`, `controllers/input-controller.ts` |
| `overlays.c` | selector host, thinking selector, `/hotkeys` markdown | `overlays/thinking-selector.ts`, `hotkeys-markdown.ts` |
| `modes/extractl.c` | `/changelog [full\|last [N]]` report panels (Recent Changes / Last Release / Last N Releases / Full Changelog over the embedded omp CHANGELOG, `coding/changelog.c`, with the dim `Use /changelog full …` hint), `/force` and `/skillful` status lines and their live autocomplete descriptions (`Force: N active tools`, `Skill listing: on`), `/setup`, `/pause` (`ui/pause.c`: pause-screen.ts over the agent pause gate), `/debug` (`modes/debugctl.c`), `/record` (toggles a `coding/record.c` writer on the renderer's paint observer; `● REC` in the status line's `stream` segment), the startup "What's New" block under the welcome banner (DynamicBorder, bold accent title, summary or markdown, DynamicBorder; `startup.changelogMode`) | `controllers/command-controller.ts` (`handleChangelogCommand`), `slash-commands/builtin-{session,control,modes}.ts` |
| `pause.c` | `/pause` screen: engages the agent pause gate (`agent_pause_gate_*` in `agent/agent.c`), owns the alternate screen with the bar glyph, title, body, settling status (unsettled turns / unfinished jobs, refreshed every second) and `paused for m:ss` clock, the compact card below 64x23 and its 1–3-row forms; the interrupt key / Enter / Space / Ctrl+C resume. A run parked at the gate gives up its sleep-prevention hold (`hooks.pause_change`) and takes it again before work resumes | `overlays/pause-screen.ts` |
| `modes/debugctl.c` | `/debug` and Shift+Ctrl+D: the "Debug Tools" selector in the composer's place and its tools (artifact folder, report bundles via `coding/debugtools.c`, system info and terminal state between DynamicBorder rows, transcript export, cache-clear confirm) | `debug/index.ts`, `apps/debug/terminal-info.ts` |
| `report.c` | command reports: docked in the report slot (between the `/btw` panel and the banner) when the box fits the rows above the chrome below it (measured from the last frame, omp's `commandReportRows`), else a focused full-screen page on the alternate screen; Esc dismisses the docked one; transcript resets drop both. Also the full-screen sheet host for `/context` | `overlays/report-panel.ts`, `controllers/command-controller.ts` (`showCommandReport`) |
| `context.c` | `/context` viewer: Overview (summary, occupancy map, legend) → category previews → entry page | `overlays/context-viewer*.ts`, `chrome/context-*.ts`, `session/context-view.ts` |
| `move.c` | `/move` overlay: `Path:` input over a live directory list (children of the cwd sorted with an approximation of localeCompare, or a case-insensitive filter of the typed prefix's directory in readdir order, 15 shown), Tab fills the highlighted path, Enter confirms it (else the typed path), Esc cancels; shown bottom-anchored over the full screen | `overlays/move-overlay.ts`, `modes/move-directory-source.ts` |
| `agenthub.c` | subagent visibility (WP-22): the "Subagents · N running" HUD above the working row (`display.pinnedAgents`, `display.subagentLivePreview`); the Agent Hub on the alternate screen (`/hub` opens the Activity tab; Alt+A, Ctrl+S and ←← on an empty composer the Agents tab): a split frame (`SplitPane` 0.58 / min 48 / right min 34 / split at 96) with the roster (frozen ranks, Flat / By parent tree rails, usage summary, `/` fuzzy filter, `… N more` windowing) and the inspector (PgUp/PgDn scroll; Tab on narrow terminals), the Activity log (live progress rows merged with transcript rows, follow / filter / scope / search), `r` revive, `x` kill, ←← closes; the transcript viewer over it (Enter on an activity row: replay through `ui_chat_replay_into`, 250 ms refresh, message box that revives and prompts the agent, stats line, j/k/g/G/PgUp/PgDn); session focus (`ui_focus_agent`: `u->view` pins the subagent's `app_t`, the transcript listener moves to it, the chat rebuilds from its messages, the status line shows `👻 <id>` dimmed and the outline goes faint; the composer prompts the agent, Esc / ←← return to main, a killed or released agent drops the view). Registry changes reach the UI through one coalesced `tui_post` | `overlays/agent-hub*.ts`, `agent-activity.ts`, `agent-transcript-viewer.ts`, `running-subagent-badge.ts`, `modes/controllers/session-focus-controller.ts`, `modes/interactive-mode.ts` (`renderSubagentHudLines`) |
| `agentshub.c` | the `/agents` hub (WP-22b, port of overlays/agents-hub.ts on hub-frame.ts, text mode): alternate screen, "Agents" split frame (sidebar 16-24 cols: ⬢ All agents, ● Project / User / Bundled, + New agent), body with a status row, type-to-filter search, the agent list (dot, name, source tag, right-aligned override / `pre:` / `adv:` badges, `+ New agent…`) and a pinned detail block (description, `model:` patterns → resolved, prewalk, advisor, file). Enter opens the agent strip (enable/disable, model, prewalk, advisor chips), a property opens its value strip (agent default / on / off / pick model… / pattern… / clear override), "pick model…" swaps the body for the model browser, "pattern…" an inline input; Space toggles, Ctrl+R reloads. The New agent flow streams the agent-creation architect on the session model, reviews the spec (Tab scope, r regenerate) and writes `<agents dir>/<id>.md`. Settings: task.disabledAgents / agentModelOverrides / agentPrewalk / agentAdvisor (global config.yml) | `overlays/agents-hub.ts`, `modes/agents-hub-deps.ts` |
| `panels.c`, `todocmd.c` | `/session` info overlay (bottom-anchored over the full screen) + `delete`/`pin`, `/tools`, `/jobs [full\|kill]` (the job tree of the session and its subagents, `buildJobTree`), `/todo` verbs | `overlays/session-info-overlay.ts`, `job-tree.ts`, `prompt/tools-markdown.ts`, `controllers/todo-command-controller.ts` |
| `tree.c` | `/tree` session tree selector: TreeView projection (branch-point connectors, compacted linear chains, shared gutter offset), filters, fuzzy search, labels, branch jumps; filtering is lazy like TreeView (applied on the next render/navigation) | `overlays/tree-selector.ts`, `components/tree-view.ts` |
| `rewind.c` | fullscreen rewind page (alternate screen): the branch replayed through `ui_chat_replay_into` into its own container, rows exactly as each view draws them (tool cards keep the Box padding the live transcript strips, a user `!cmd` / `$ code` run has none), dotted outline, sibling-branch strip with a 160 ms eased camera slide, word filter. Also the shared transcript-selector engine (`ui_tx_*`: replay with per-target entries, row rendering, outline in any colour with a top-rule caption, browser frame, scroll keys, SGR mouse) | `overlays/rewind-selector.ts`, `chat/transcript-outline.ts`, `chat/transcript-browser.ts` |
| `copysel.c` | `/copy` picker (alternate screen, `ui_tx_*` engine): green outline on the turn Enter copies with a `N blocks →` caption, Right replaces the turn with its captioned block previews (fenced code highlighted, `>`-quotes, bash/eval commands, tool output, http(s) links; 12 rows then `… +N more lines`), Enter copies the block, `o` / clickable `↗ open` open a link, `⧉ copy` clicks copy, `a` earlier turns, Ctrl+O expand; also the copy-targets helpers (`ui_md_extract_blocks`, `ui_md_extract_links` from the renderer's own OSC 8 spans, last code / command / link) behind `/copy code\|cmd\|link` and `/open` (`modes/copyctl.c`) | `overlays/copy-selector.ts`, `overlays/copy-targets.ts`, `controllers/selector-controller.ts` (`showCopySelector`) |
| `planreview.c` | fullscreen Plan Review (sections, Contents sidebar, delete/undo, annotations, options, model-tier slider), the "Save plan" path prompt, and the composer-slot hook selector (`ui_hook_select` / `ui_hook_confirm`) and hook editor (`ui_hook_editor`: box frame, Enter newline, Ctrl+Q / Ctrl+Enter submit) | `overlays/{plan-review-overlay,plan-toc,plan-save-overlay}.ts`, `chrome/overlay-box.ts` split frame |
| `signals.c`, `recap.c` | terminal title with run state and spinner (CSI 22/23;2t title stack), OSC 7501 program status, completion / error / ask notifications, Warp OSC 777 events, the idle recap row | `utils/title-generator.ts`, `utils/run-status.ts`, `controllers/event-controller.ts`, `modes/warp-events.ts`, `session/idle-maintenance.ts`, `chat/recap-notice.ts` |
| `mcpcmd.c`, `mcpwizard.c`, `sshcmd.c` | `/mcp` (help/list/resources/prompts/notifications reports; add, remove, test, reauth, unauth, enable, disable, reconnect, reload as bordered command messages; the animated `Connecting to "x"...` row; the `━━━ OAuth Authorization Required ━━━` block with the wrapped copy-URL rows; Esc stops an OAuth wait or a running `/mcp test`, `/login <value>` pastes the code), the startup `Connected to MCP server: x.` status, live MCP tool / prompt refresh and the `mcp.notifications` injection; the Add MCP Server wizard in the composer's place (name, transport, command/args or URL, connection test with auth detection, OAuth sign-in + health check or API key, scope, review); `/ssh add\|list\|remove\|help`; server-name argument completion for `/mcp` | `controllers/mcp-command-controller.ts`, `overlays/mcp-add-wizard.ts`, `controllers/ssh-command-controller.ts`, `slash-commands/builtin-completions.ts` |
| `appearance.c`, `widgets.c`, `slash.c` | theme/glyph/color-blind state, box chrome and key hints, slash-command metadata | `theme/theme.ts`, `chrome/overlay-box.ts`, `slash-commands/builtin-*.ts` |

Tool card renderers in `src/ui/tools/` are pure functions from a `tc_call_t` snapshot (args, result
content and details, lifecycle flags) to ANSI lines at a fixed width. This keeps them unit-testable
without a running UI. Their input is omp's `details` shape, so a session written by omp renders the
same in omc.

### Loop mode (`modes/loopctl.c`, `coding/loop.c`)

`/loop [count|duration] [--while|--until '<cmd>'] [prompt]` toggles omp's loop mode (InteractiveMode
`handleLoopCommand` / `#enableLoopMode` / `disableLoopMode`). The arguments parse like `parseLoopArgs`
(`10`, `10m`, `1h30m`, `10 minutes`, one shell-quoted condition flag, the rest is the prompt; limit- or
flag-shaped typos are errors). The prompt the user submits while idle (or the inline prompt, which runs
through the normal submit flow) becomes the loop prompt; `worker_done` calls `lp_run_end` and, 800 ms
after the run ends, the iteration checks the budget (`Loop limit reached.` / `Loop time limit reached.`),
runs the `--while` / `--until` command in the session cwd off the UI thread (`proc_shell`,
`loop.conditionTimeoutMs`; exit 0/1 decide, anything else stops with the first output line), consumes one
iteration, applies `loop.mode` (`compact` through `iq_compact_then`, `reset` as `/new`) and re-submits.
Esc (`iq_escape`, after a running compaction) aborts the live turn and pauses the loop; the next prompt
resumes it. The status line `mode` segment shows the state; like omp nothing repaints an idle screen for the
duration countdown. Loop mode wins over goal continuations.

## How parity is verified

1. **Side-by-side harness** `tests/tui/compare.sh`: the installed `omp` and `build/omc` run in two
   tmux sessions of identical size, both talking to the scripted mock model through throwaway agent
   dirs and HOME. `keys` sends the same input to both, `shot` captures plain text and ANSI, and
   `diff` compares them. `replay <spec>` resumes the same scripted transcript
   (`tests/tui/specs/*.json` → `mksession.py`) in both apps, so cards render from arbitrary
   args/details without running tools. `celldiff.py` diffs captures cell by cell (glyph, fg, bg,
   attributes).
2. **Golden captures** cut from omp screens with `mkgolden.py`:
   - `tests/tui/golden/*.ansi` (markdown fixtures, thinking, user bubble) are checked by
     `tests/test_tui.c`;
   - `tests/fixtures/chrome/` (welcome, status band) are checked by `tests/test_chrome.c`;
   - `tests/fixtures/toolcards/` are checked by `tests/test_toolcards.c`.
   These run in `make test`, without omp installed.

Treat any visible difference from omp as a bug. When omp is updated, re-run the harness against
the new version and refresh the captures. See [testing.md](testing.md).

## Image attachments (`ui/attach.c`)

Ports of omp's input-controller image paste flow and `prompt/{composer-attachments,attachment-chips}.ts`:

- A bracketed paste (or drag and drop) whose text is one or more image paths (`.png .jpg .jpeg .gif .webp`;
  absolute, `~/`, `./`, `file://`, quoted or shell-escaped; a whole-text path with spaces such as macOS
  screenshot names) attaches each image instead of inserting the text. An empty bracketed paste and `ctrl+v`
  (`app.clipboard.pasteImage`) read an image from the clipboard (`wl-paste` / `xclip` / `osascript`), falling
  back to the clipboard text; clipboard images are saved as `local://pasted-image-<hash>.<ext>`.
- Each attachment is normalized for the model (`images.autoResize`) and the buffer gets an atomic chip
  `📷 #N` (bold, identity color from omp's six-color palette) that expands to `[Image #N, WxH]` on submit.
  Referenced attachments draw as 14x6 rounded cards (`📷 #N` title, centered icon, `WxH` caption) between the
  status row and the editor gap.
- On submit, unreferenced attachments are dropped and markers renumbered (`compactImageMarkers`); each image
  with a file behind it sends the hidden `image-attachment` notice (`prompts/system/image-attachment.md`) as a
  developer turn before the user message. User bubbles show `[Image #N, ...]` markers as chips.
- Errors follow omp: `Image not found at <path>` (with the SSH hint), `Pasted path is not a supported image`,
  `Image file too large: ...`, `Failed to read pasted image path`, `Clipboard is empty`.

Not ported: Kitty-graphics thumbnails inside the cards (the icon fallback is what omp shows in terminals
without Kitty Unicode placeholders), video contact-sheet attachments, chip hyperlinks.

## Editor I/O (WP-6)

- **External editor (Ctrl+G, `app.editor.external`)**: `ui_external_edit` (`modes/interactive.c`) ports
  `getEditorCommand` (trimmed `$VISUAL`, else `$EDITOR`) and `openInEditor`: the text goes to
  `$TMPDIR/omc-editor-XXXXXXXXXX<ext>`, `sh -c '<editor> "$1"' sh <file>` runs with the pane's stdio while the
  TUI is stopped (a fullscreen page hands the alternate screen back first), a zero exit reads the file back,
  and one trailing newline is trimmed unless the caller keeps it (plan files). The composer sends its expanded
  text (chips expanded) as `.omp.md` and warns `No editor configured. Set $VISUAL or $EDITOR environment
  variable.`; the ask dialog's "Other" / note prompts and the hook editor use `ui_dialog_external_edit`
  (`.md`, silent without an editor, like `editDialogExternally`).
- **History search (Ctrl+R, `ui/histsearch.c`)**: `overlays/history-search.ts` in the composer's place
  (OverlayPanel `History`: the search Input with its inverse cursor cell, up to 10 rows centered on the
  selection with a scrollbar column, query tokens in the accent color, the `pickerAge` flush right, the
  `↑/↓ navigate · ⏎ select · ⎋ cancel` hints). omp's SQLite `HistoryStorage` is replaced by
  `prompt-history.jsonl` folded the same way: one row per `normalizePrompt` form with its latest
  submission's time and cwd, newest first; a query keeps rows matching every whitespace-delimited term, either as one
  phrase of the term's letter/digit runs (adjacent words, the last a prefix: `git-commit` finds `git commit`) or
  literally as a case-insensitive substring (`a/b` needs `a/b`), at most 100. Enter puts the prompt in the composer, Esc cancels.
- **Large pastes (`ui/attach.c`)**: a marker-sized paste (> 10 lines or > 1000 chars) in the composer becomes a
  text-attachment chip `📄 #N` (`tui_editor_insert_text_attachment`, numbered per draft, palette offset 2)
  whose card in the attachment band shows the first 4 rows x 12 cells and `+N lines` / `N chars`; it expands
  to the pasted text on submit. From `paste.largeMenuThreshold` lines (default 100, 0 = off) the large-paste
  menu opens first (`ui_hook_select_desc`, `ui/hooksel.c`): `Attach as a wrapped block` (`<attachment>` …
  `</attachment>` behind the chip), `Attach as local file` (`local://paste-N.md` in the session's local
  root, `Saved N pasted lines to local://paste-N.md`) or `Paste inline` / Esc (the chip).
- **Raw paste (Ctrl+Shift+V)**: the clipboard text (`pbpaste` / `wl-paste` / `xclip`) inserted as typed, no
  chip or marker; `No text in clipboard to paste raw`, and the SSH clipboard notice over SSH.
- **`#` prompt actions and GitHub references**: `#query` lists omp's seven actions (copy line / prompt,
  undo, cursor to message or line start / end) ranked by `subsequenceScore` over label, key hint and
  keywords; accepting removes the typed token and runs the action (`tui_editor_t.on_action`; undo uses
  `undoPastTransientText`). A standalone `#123` (or `pr #123` / `issue #123`) offers `PR #123` / `Issue #123`
  and inserts `pr://123 ` / `issue://123 `.
- **`@` mentions (`tui/atmention.c`)**: ports `extractAtFilePrefix`, `#getFileSuggestions`,
  `#getFuzzyFileSuggestions` and the native `fuzzyFind` scorer: a project-wide gitignore-aware walk (hidden
  entries kept, `.git` / `node_modules` skipped, symlinked directories followed, 1.5 s deadline, reused for
  2 s) scored by file name (exact / prefix / substring / fuzzy, by path when the query has a `/`,
  directories +10), best 100 by score, depth, path; `@dir/query` scopes to that directory; a bare `@`, paths
  outside the cwd and searches without hits list the directory. Files insert with a trailing space, paths
  with spaces are quoted (`@"docs/my notes.md" `), directories keep completing.

## Inline images and display options (`tui/termcaps.c`)

Ports of omp's terminal-capabilities.ts image/hyperlink policy, kitty-graphics.ts, sixel.ts and components/image.ts:

- **Protocol:** Kitty for kitty/ghostty/wezterm/warp/otty/rio/rex/tern/monstar (and `screen`/`ghostty` TERMs on a TTY),
  iTerm2 for iTerm2, none for VS Code/Alacritty; `PI_FORCE_IMAGE_PROTOCOL` overrides; Kitty is dropped inside tmux
  without `allow-passthrough`, under Paseo and Herdr. A DA1 reply with attribute 4 or an XTSMGRAPHICS geometry upgrades
  an unknown terminal to SIXEL (not inside tmux). Cell size comes from CSI 16 t, DEC 2048 in-band resize reports or iTerm2
  `ReportCellSize` (default 9x18); a change re-renders every image.
- **Image rows (`tui_image_render`):** fit into `width-2` columns and `tui.maxInlineImageColumns/Rows` (100 / 20, at most 60%
  of the screen). Kitty transmits each image id once (`a=t`, 4096-byte chunks) through a queue the renderer writes before
  the frame; terminals with Unicode placeholders get a text grid of U+10EEEE cells, others `rows-1` reserved rows and a last
  row `ESC 7 CUU a=p ESC 8`. iTerm2 and SIXEL use the same reserved-row shape. Image rows skip width fitting and are
  written after `CSI 2K` (SIXEL / iTerm2 also erase the rows they paint over). A destructive repaint sends Kitty's
  delete-all and transmits the live images again.
- **SIXEL encoder:** decode with `src/img`, resize (premultiplied when translucent), composite partial alpha over the
  terminal background, median-cut palette (256 colors), Floyd-Steinberg dithering, icy_sixel's band layout, whole 6 px bands.
- **Transcript:** grouped `read` images draw in the assistant block that issued the call (omp setToolResultImages), other
  tools' images below their card; without a protocol the text fallbacks are omp's (`[Image: image/png]`,
  `[Image: [image/png] 48x24]` in generic cards).
- **Display options:** `tui.hyperlinks` (off/auto/always over the detected OSC 8 support), `tui.tight` (Text, Markdown,
  WidthAwareText and Loader drop a padding column; boxed cards and notices keep theirs), `display.hideToolActivity` /
  Ctrl+Shift+O (tool activity views render no rows), `display.collapseCompacted`, `display.cacheMissMarker`
  (`────────── ⊘ cache miss · 51K tokens` after the turn), `composer.tokenRate` (`coding/tokrate.c`, docked before the band
  title on the working row and idle), `tui.mouse` (`CSI ?1000;1003;1006 h`; hover band and click-to-focus on the subagent HUD).

Not ported: the inline-image budget (`tui.maxInlineImages` demotion and purges), re-anchoring/cropping of image blocks clipped
at the screen top, OSC 11 background detection, image fallback hyperlinks, vim mode, charts, reactions, Mermaid, SVG and LaTeX.

## Motion

`src/tui/motion.c` holds the shared animation knobs (omp `spinner-clock.ts`, `frame-rate.ts`, `motion-effects.ts`). Every
spinner reads `tui_shared_spinner_frame` from the wall clock and repaints on `tui_add_clock_timer`, whose ticks land on the
period's boundaries, so N spinners cost one frame. `ui_apply_motion` resolves `tui.motion`'s preset under `tui.spinnerInterval`
(unset follows the preset) and `display.shimmer`, plus the `tui.maxFps` ceiling; it runs at startup and on every settings
change. Frames a keystroke waits on keep 30 fps under any ceiling. Decorative motion (brand fade, shimmer) stops when the
preset turns effects off. `--spinner-interval`, `--max-fps` and `--motion` are runtime overrides, which outrank
`PI_SPINNER_INTERVAL`, `PI_MAX_FPS` and `PI_MOTION`; those outrank every settings file (`env_layer` in `src/config/settings.c`).

## Markdown colors and the keyword glow

With `tui.colorfulMarkdown` on (the default) `src/tui/markdown.c` paints headings by level (magenta, yellow, blue), strong and
emphasis (magenta), inline code (green) and table headers (yellow) from the theme's `terminal.ansi` palette
(`tui_ansi_fg_seq`; a theme without one falls back to the terminal's own palette). A forced run color (thinking blocks, the user
bubble) stays over a role's color; `proseFg` (`assistantMessageText`) yields to it. `src/ui/keywordglow.c` paints standalone
magic keywords with their HSL gradient in sent bubbles and, rotating while the prompt is focused, in the composer.

## Prompt cache

`cachewarm_status` (`src/coding/cachewarm.c`) says what the next request finds in the prompt cache: the confirmed entry of the
last response while the transcript still extends it, else what the branch's newest cached reply left. The `cache` status-line
segment and `/session`'s Prompt Cache rows read it. The armed entry's lifetime counts from when the request is prepared.

The idle recap is journaled in `<agent>/session-recaps.jsonl` (omp uses history.db), and a transcript replay re-shows the newest one
while the conversation has not moved since.

