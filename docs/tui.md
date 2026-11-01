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
native scrollback, as in omp. When a change lands above the visible viewport (already in
scrollback) or the width changes, the TUI clears the screen and scrollback and redraws everything
(`tui_full_redraw_count` exposes this to tests). Fullscreen pages (`/settings`, `/resume`, the Models
hub, `/hotkeys`, the setup wizard) switch to the alternate screen themselves.

A focused component may emit `TUI_CURSOR_MARKER` (an APC sequence) where the hardware cursor should
sit (for IMEs). The renderer strips it and parks the cursor there. Renders are coalesced and
throttled (16 ms by default).

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
  (async-signal-safe) and re-raises. An `atexit` hook restores it too. Ctrl+Z suspends and redraws
  on resume.
- The key decoder handles UTF-8, C0 controls, CSI/SS3 with xterm modifiers, kitty CSI-u (with text
  and event types), modifyOtherKeys, Alt via an ESC prefix, bracketed paste, focus, and terminal
  replies. A lone ESC is held for `TUI_ESC_TIMEOUT_MS`. Key ids (`ctrl+c`, `alt+enter`,
  `shift+tab`) are matched against the keybinding table (`tui.editor.*`, `tui.input.*`, app actions),
  which mirrors omp's `keybindings.ts`.
- Non-tty file descriptors are supported (tests): probes are skipped and the size defaults to 80×24.

### Editor and composer (`editor.c`, `composer.c`)

The editor ports omp's `editor-component.ts`: word motion, kill/yank, undo (200 steps), history,
large-paste markers (`[Paste #N, +L lines]`), slash-command and `@file` autocomplete with omp's
ranking, and Enter-runs-command semantics. Its frame is drawn by a composer style, one of omp's 8
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
| `ask.c` | the `ask` tool's dialog in the composer's place | `overlays/ask-dialog.ts`, `hook-editor.ts` |
| `overlays.c` | selector host, thinking selector, `/hotkeys` report page | `overlays/{thinking-selector,report-panel}.ts`, `hotkeys-markdown.ts` |
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
