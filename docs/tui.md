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
toggled); a resize repaints only the screen with the frame's tail. `tui_full_redraw_count` exposes the
clears to tests, and `OMC_TUI_DEBUG=<file>` logs one line per frame (kind: first/diff/above/none/full/
resize, first changed row, viewport top, bytes written, old/new excerpt of the changed row).

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
  listeners (`app.thinking.toggle`, `app.display.reset`, `app.tools.expand`), then, while the
  composer has focus, `CustomEditor.handleInput`'s order (paste image, suspend, model select / role
  cycle, thinking cycle, interrupt, clear, exit, dequeue, copy prompt, plan, session actions,
  follow-up, copy line, retry). `/hotkeys` (`ui_hotkeys_markdown`) is built from the live table like
  omp's `buildHotkeysMarkdown`: unbound actions read `Disabled`, and exit keys split into a
  forward-delete row and an exit row.
- Not handled yet although bound (as in omp they still appear in `/hotkeys`): `app.model.selectTemporary`,
  `app.tools.toggleVisibility`, `app.history.search`, `app.editor.external` (main composer),
  `app.local.open`, `app.clipboard.pasteTextRaw`, `app.approval.approve`, `app.agents.hub`,
  `app.session.observe`, `app.tree.*`, `app.links.hint` and the speech actions; see
  [gaps.md](gaps.md) §2b.
- Non-tty file descriptors are supported (tests): probes are skipped and the size defaults to 80×24.

### Editor and composer (`editor.c`, `composer.c`)

The editor ports omp's `editor-component.ts`: word motion, an emacs kill ring (`kill-ring.ts`:
consecutive kills accumulate, Ctrl+Y yanks, Alt+Y cycles older entries right after a yank),
character jump (Ctrl+] / Ctrl+Alt+] then a character), undo (200 steps), history,
large-paste markers (`[Paste #N, +L lines]`), slash-command and `@file` autocomplete with omp's
ranking, and Enter-runs-command semantics. Slash completion also matches aliases (the alias is the
row), completes arguments (`tui_ac_default_t.args`: subcommands, the live `/effort` levels) and paints
dim ghost text after an end-of-line cursor (`tui_editor_ghost`: the selected item's remainder +
usage, else the command's inline hint); Enter on an argument completion applies it and submits only
when the argument is already fully typed (`#selectedSlashArgumentIsAlreadyTyped`). The metadata comes
from the shared registry (`src/coding/slashcmd.c`) through `src/ui/slash.c`. Its frame is drawn by a composer style, one of omp's 8
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
  depth is capped.
- `highlight.c` uses hand-written lexers per language family that assign the color category omp's
  syntect-based highlighter (`crates/pi-natives` highlight.rs) would give each token: comment,
  keyword, function, string, number, type, operator, punctuation, inserted/deleted.
  Unsupported languages render plain.
- `ansi.c` holds the ANSI-aware width, slice, truncate and wrap helpers (escape sequences are zero
  width, tabs are 3 columns). `fuzzy.c` is omp's word-local fuzzy matcher (`fuzzy.ts`) used by
  pickers.

## Coding-agent screens (`src/ui/`)

| Module | Shows | omp source |
|---|---|---|
| `welcome.c` | gradient π logo with a 3 s intro sweep, half-block wordmark, version, weighted tip from `tips.txt` | `prompt/welcome.ts` |
| `statusline.c` | status band / bottom bar in every preset and layout (`box`, `band`, `plain-*`), context gauge with the compaction threshold, model/thinking/cost/VCS segments, brand spinner and turn timer, composer hints | `status-line/*`, `prompt/composer-hints.ts` |
| `chat.c` | transcript container (blank-row separated blocks), user bubbles, assistant prose and thinking, notices, custom/summary/TTSR messages, pinned error banner, working row, retry/compaction rows, replay from the session tree on resume and branch | `chat/*`, `chrome/*` |
| `toolcards.c` + `tools/tc_*.c` | tool execution cards: bash, read (grouped), write, edit diffs, grep/glob trees, todo, ask, goal, find, ast, debug, task (live async progress), eval cells, wait, MCP, lsp, web_search, generic fallback. Ctrl+O expands | `chat/tool-execution.ts`, `read-tool-group.ts`, `tui/src/tools/*`, `render/*` |
| `setup.c` | first-run wizard: splash, sign in, default model, glyph preset, composer shape, theme, outro. Writes `setupVersion: 2` | `setup/*` |
| `login.c` | provider list and "Login to …" dialog running `config/auth*` flows on a worker thread | `overlays/oauth-selector.ts`, `login-dialog.ts` |
| `modelhub.c`, `modelbrowser.c` | Models hub (`/model`, Alt+M): roles, providers, model browser, persistence to `modelRoles.*` | `overlays/model-hub.ts`, `hub-frame.ts`, `model-browser.ts` |
| `sessions.c` | `/resume` selector: search, all projects, pins, rename, delete | `overlays/session-selector.ts` |
| `settings.c` | `/settings` from omp's own registry dump (`assets/tui/settings-ui.json`) | `overlays/settings-selector.ts`, `components/settings-list.ts` |
| `btw.c` | `/btw`: inline answer panel below the transcript (streams Markdown, Esc cancels then closes, `c` copy, `f` follow up, `b` branch), fullscreen BTW history (topic list + answer pane, follow-up composer, copy, cancel), the controller (session moves dispose it) | `controllers/btw-controller.ts`, `overlays/btw-panel.ts`, `overlays/btw-history-panel.ts` |
| `ask.c` | the `ask` tool's dialog in the composer's place | `overlays/ask-dialog.ts`, `hook-editor.ts` |
| `overlays.c` | selector host, thinking selector, `/hotkeys` markdown | `overlays/thinking-selector.ts`, `hotkeys-markdown.ts` |
| `report.c` | command reports: docked in the report slot (between the `/btw` panel and the banner) when the box fits the rows above the chrome below it (measured from the last frame, omp's `commandReportRows`), else a focused full-screen page on the alternate screen; Esc dismisses the docked one; transcript resets drop both. Also the full-screen sheet host for `/context` | `overlays/report-panel.ts`, `controllers/command-controller.ts` (`showCommandReport`) |
| `context.c` | `/context` viewer: Overview (summary, occupancy map, legend) → category previews → entry page | `overlays/context-viewer*.ts`, `chrome/context-*.ts`, `session/context-view.ts` |
| `panels.c`, `todocmd.c` | `/session` info overlay (bottom-anchored over the full screen) + `delete`/`pin`, `/tools`, `/jobs [full\|kill]`, `/todo` verbs | `overlays/session-info-overlay.ts`, `job-tree.ts`, `prompt/tools-markdown.ts`, `controllers/todo-command-controller.ts` |
| `planreview.c` | fullscreen Plan Review (sections, Contents sidebar, delete/undo, annotations, options, model-tier slider) and the "Save and quit" path prompt | `overlays/{plan-review-overlay,plan-toc,plan-save-overlay}.ts`, `chrome/overlay-box.ts` split frame |
| `appearance.c`, `widgets.c`, `slash.c` | theme/glyph/color-blind state, box chrome and key hints, slash-command metadata | `theme/theme.ts`, `chrome/overlay-box.ts`, `slash-commands/builtin-*.ts` |

Tool card renderers in `src/ui/tools/` are pure functions from a `tc_call_t` snapshot (args, result
content and details, lifecycle flags) to ANSI lines at a fixed width. This keeps them unit-testable
without a running UI. Their input is omp's `details` shape, so a session written by omp renders the
same in omc.

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
