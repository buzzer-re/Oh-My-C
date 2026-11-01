# omc implementation status

Status of the port against upstream oh-my-pi (reference: omp v18.13.3). ✅ ported and tested · 🟡 partial · ❌ not yet.
This file is the single source of truth for what is implemented; it is updated with every change.

## Runtime & infrastructure
| Area | Status | Notes |
|---|---|---|
| HTTP/1.1 client (TLS, keep-alive pool, gzip, proxies, redirects) | ✅ | `src/net/http.c` |
| SSE / NDJSON streaming, retry with backoff + Retry-After | ✅ | |
| JSON (ordered, partial/streaming parse), YAML (parse + dump) | ✅ | |
| Regex (Rust syntax, linear time) + look-around/backrefs fallback | ✅ | bounded backtracking |
| Gitignore-aware parallel walker, ripgrep-style search | ✅ | |
| Handlebars-compatible prompt templates + omp format pass | ✅ | |
| Model catalog (5,948 bundled models, per-API compat) | ✅ | `models.yml` custom providers |

## Providers
| API | Status |
|---|---|
| anthropic-messages (API key + Claude OAuth identity) | ✅ |
| openai-completions / openrouter (compat flags, reasoning formats) | ✅ |
| openai-responses / azure-openai-responses / openai-codex-responses | ✅ |
| google-generative-ai (API key) | ✅ |
| google-vertex (ADC / service account), google-gemini-cli + google-antigravity (Cloud Code Assist, OAuth) | 🟡 | in progress |
| ollama-chat | ✅ |
| bedrock-converse-stream (SigV4 + bearer key, AWS credential chain: env / web identity / profile static-SSO-credential_process-role_arn / ECS / IMDSv2, event-stream, geo-aware region, cache points, thinking/effort/binding controls, guardrails) | ✅ (Bedrock server-side compaction via InvokeModel ❌) |
| cursor-agent (123 catalog models; HTTP/2 + Connect + protobuf), devin-agent, gitlab-duo-agent, typesafe, workers-ai-systemone, local-inference | ❌ |
| Non-chat APIs: openai/openrouter images, video, embeddings, rerank, transcriptions, speech (xai-tts, openai-speech), decisions | ❌ |
| Codex websocket transport, Anthropic server-side fallback/compaction | ❌ |

## Agent & session
| Feature | Status | Notes |
|---|---|---|
| Date/cwd `<system-reminder>` injected per request into user turns (system prompt stays byte-stable; turns keep the reminder they were first sent with; a changed day/cwd attaches to the newest unsent user turn; setting `dateCwdReminder`) | ✅ | multi-root inheritance across compaction/branches simplified (resets per conversation root) |
| Agent loop, parallel shared tools / exclusive tools, steering, follow-ups | ✅ | |
| Session JSONL (title slot, v3 header, tree, branch/fork, resume, continue) | ✅ | |
| Compaction (auto + `/compact`), auto-retry, context-overflow recovery | ✅ | |
| Skill descriptions in the system prompt compressed to routing hints (`compress-description.md`): per-session frozen snapshot, bounded preview while pending, background requests on smol → default → tiny (4 at a time), unusable answers remembered per model, cache `<agent_dir>/skill-descriptions.json` (`skills.compressDescriptions`) | ✅ | omp stores this cache in SQLite (`skill-descriptions.db`); omc uses JSON |
| Title generation: `title.generator: fork` (default) side request on the session model/system prompt/tools/history started when the reply begins, card titles `🧪 CODE: Title` (`title.icons`), low-signal input skipped, title-model fallback (tiny/commit/smol roles, `title-system.md`), `tiny` generator | ✅ | Nerd Font icons (no bundled nf catalog: emoji shown), replan title refresh, `/rename` card keeping, deferred retitle ❌ |
| TTSR (stream rules, builtin rules, interrupt/inject/retry, tool reminders) | ✅ | regex conditions; AST conditions ❌ |
| Context files, skills, rules (`.omc/`, `.omp/`, AGENTS.md, CLAUDE.md, cursor, …) | ✅ | |
| Subagents (`task`, bundled + user agent definitions, parallel) | ✅ | worktree isolation ❌, IRC ❌, async jobs ❌ |
| Plan mode (local:// plans, xd://propose approval, read-only gating) | ✅ | |
| Goal mode (goal tool, hidden goal context + todo context, token/time budget accounting, budget-limit steer, auto-continuation with no-progress stop, pause on interrupt/resume, `/goal` + `--goal`, RPC `goal` command, mode_change persistence) | ✅ | guided-goal interview ✅ (`/guided-goal`) |
| Advisor (second model with read/grep/glob + `advise`, incremental session updates, nit/concern/blocker routing, steer mid-run, continuation at run end per continueSeverity, dedup, note budget, immune turns, `/advisor`, RPC `set_advisor`) | 🟡 | multiple named advisors, WATCHDOG.yml grants, secret obfuscation, transcript recorder ❌ |
| Memory backends, autolearn | ❌ | |

## Tools
| Tool | Status | Notes |
|---|---|---|
| read (selectors, hashline anchors, summaries, dirs, images, URLs, skill:// rule:// artifact:// local:// pr:// issue://; zip/jar/whl/tar/tar.gz/gz members, SQLite tables/rows/`?where=`/`?q=SELECT` via an on-disk-format parser with WAL overlay, PDF text with page selection, `.ipynb` cells) | ✅ | other archive codecs (bz2/xz/zst/7z/rar/iso/...) ❌; SQLite `?q=` is single-table SELECT only (no JOIN/GROUP BY/subqueries); encrypted PDFs, OCR of image-only pages and `:pageN.png` page rendering ❌ (`:pageN.png` returns that page's extracted text) |
| write, edit (`edit.mode`/`PI_EDIT_VARIANT`: hashline PUT/CUT/REM/MV, registers, block anchors; replace, patch, apply_patch (Codex envelope, Add/Update/Delete/Move, first-match context ladder), sloppy (Find/Replace/Insert, … captures, ＋/－ rows); fuzzy ladder, numbered diffs + compact previews, notebooks) | 🟡 | replace/patch/apply_patch pass all upstream pi-edit fixtures; sloppy: legacy ⟪⟫ selections, desired-text/diff-shaped/typo recovery ❌; NFC normalization, tree-sitter diff context, unique-suffix path recovery, auto-generated guard, `edit.modelVariants`, streaming previews, apply_patch custom-tool wire ❌ |
| bash (persistent shell session, timeouts, PTY, artifact spill, `async` background jobs delivered as follow-ups / idle wake-up, `wait` tool, `/jobs`) | ✅ | services (launch), async task jobs ❌ |
| grep, glob | ✅ | |
| todo, ask | ✅ | |
| web_search (Brave, Tavily, Exa, DuckDuckGo) | 🟡 | other 19 providers ❌ |
| lsp (55 bundled servers; all 13 actions; diagnostics on write) | ✅ | |
| MCP (stdio, streamable HTTP, SSE; omp/Claude/Cursor/VS Code configs) | ✅ | |
| MCP OAuth for remote servers: RFC 9728 resource metadata (WWW-Authenticate `resource_metadata` + well-known fallbacks), RFC 8414/OIDC server metadata, RFC 7591 dynamic registration (or configured `oauth.clientId`/`clientSecret`/`scopes`/`callbackPort`), auth code + PKCE with RFC 8707 `resource`, bearer injection on every request, automatic refresh, `omc mcp list/login/logout`, 401 → actionable error | ✅ | interactive `/mcp` reauth UI, brokered/managed credential profiles ❌ |
| eval (persistent Python kernel, tool bridge, figures; JS/Bun runtime intentionally omitted) | ✅ | |
| debug (DAP: all 28 actions; 14 bundled adapters + user adapters) | ✅ | verified against a scripted adapter; native adapters can't ptrace in the dev VM |
| ast_grep, ast_edit (bracket-aware structural matcher with ast-grep metavariables; staged xd://resolve / xd://reject) | 🟡 | token-tree matching, not tree-sitter ASTs |
| find (lexical IDF scan → passage windows → model judge, lexical fallback) | ✅ | single judge pass (upstream: 3 waves) |
| checkpoint / rewind (context cut + session branch + report) | ✅ | |
| ssh:// URLs (ssh.json hosts, ControlMaster, POSIX shell probe, /~ HOME, 1 MiB UTF-8 reads, dir listings, staged byte-exact writes) | 🟡 | read/write/edit ✅; grep over ssh:// ❌ |
| browser, computer, image_gen, tts | ❌ | |

## Embedding
| Feature | Status | Notes |
|---|---|---|
| Library build (opt-in `make lib`): `libomc.a` (prelinked, only `omc_*` symbols global), `libomc.so.0` (version script), `omc.pc`, `make install-lib` | ✅ | public API `include/omc.h` |
| API: sessions with options, full RPC protocol via `omc_send` + event callback, blocking `omc_prompt`, abort/wait/busy, host-implemented native tools, `omc_main` (whole CLI/TUI in-process) | ✅ | `examples/embed.c`, checked by `make test` |
| Per-session configuration (settings/credentials/catalog are process-wide) | ❌ | |

## Modes & UI
| Feature | Status |
|---|---|
| Print mode (text / json), RPC mode: prompt/steer/follow_up/abort*, queue remove/promote, state, goal, advisor, todos, host tools (set_host_tools ↔ host_tool_call/result/cancel), event filter, tree/navigate/label/branch messages, messages paging, model/thinking cycle, compaction/retry toggles, export_html, handoff, btw, available commands, login providers | ✅ | host URI schemes, subagent streaming, live voice/dictation, predict_word, session lease ❌ |
| Interactive TUI: functional (streaming markdown, tool output, editor, autocomplete, pickers, slash commands, themes) | ✅ |
| Interactive TUI: 1:1 visual/behavioral parity with `omp` (verified side by side in tmux against the same mock model) | 🟡 | in progress. Diverges today: bordered tool cards (bash: `$ cmd` / Output / `Wall · Timeout` footer) |
| First-run setup wizard (omp setup/*, v18.14.2): 2.6 s water/starfield splash, dissolve, 5 scenes — sign in (provider panel with fuzzy search → "Login to …" dialog: authorize URL, /launch shortcut, paste-code / API-key prompt), default model (model browser), glyph mode (live preset preview), composer shape (8 shapes, live preview), theme (curated + Browse all, live preview) — "Setup saved" outro, then the welcome intro. Persists modelRoles.default, symbolPreset, composer.shape, theme.dark/light, colorBlindMode and `setupVersion: 2` exactly like omp; OMP_SKIP_SETUP/OMC_SKIP_SETUP, `startup.setupWizard`, skipped when resuming | ✅ | verified screen by screen in tmux (only the `omc` name differs); model step lacks omp's local-provider discovery (LM Studio/Ollama rows) and measured perf columns; API keys are stored without omp's validation request |
| Overlays: slash-command autocomplete (upstream registry order, icons, live descriptions, slash layout, Enter-vs-description-match rule), thinking selector (/thinking, /effort), /resume session selector (fullscreen, search, all-projects Tab), /login and /logout (provider panel + login dialog), /hotkeys report page, model picker (/model, alt+m) | 🟡 | the model picker is the model browser in a "Models" panel, not omp's full Models hub (roles/provider sidebar, kind filter); the session selector lacks delete/rename/pin and lifecycle status; /hotkeys tables differ where the shared markdown renderer omits per-row separators; ask dialog and the /settings menu (with its theme picker) are not ported yet |
| TUI screen chrome: welcome banner (gradient π logo + 3 s intro sweep, half-block `omc` wordmark, version, weighted tips from omp's tips.txt with key-hint expansion); status line (all presets and separators, `box`/`band`/plain layouts, embedded context gauge with compaction/speculation markers, overflow shrinking, session accent colors, brand spinner + turn timer, VCS branch and dirty counts); all 8 composer shapes (`composer.shape`, default `band`: `╰─` prompt, right-aligned `⇧⇥ to change thinking effort` hint retired after 3 uses); working row with the right-aligned session title; titanium default theme; hardware cursor; TUI starts without a model (`/login`, `/model`) | 🟡 | cell-for-cell identical to omp (glyphs + colors) at 80/120/160 columns in tests/tui/compare.sh and the `tests/test_chrome.c` fixtures, apart from the `omc` wordmark/version and the pre-first-turn context % (system prompt + tool schemas estimated as chars/4.1, which reproduces omp's tokenizer count of its own request: 8% vs omc's 9% today only because omc sends 19 tools where omp sends 12). Rechecked against omp v18.14.2 (no ANSI-visible chrome changes; tips.txt unchanged). Not yet: `pr`/`usage`/`vim`/`collab`/`stream`/subagent segments, token-rate meter (`composer.tokenRate`), focus-proxy dimming, hook statuses |
| TUI chat transcript: blocks separated by one blank row; user bubble (padded `userMessageBg`, contrast prose color); assistant prose (markdown, literal code rows) with thinking blocks (italic `thinkingText`, prose-only fence folding, comment-noise drop, hidden-thinking `✻ Thinking` pulse) and smooth streaming reveal (`display.smoothStreaming`); turn errors pinned in the `✘` banner above the editor and restored inline (`Error: …`, 8-row cap) at the next turn; user Esc aborts quiet; status/warning/error notices (consecutive status lines update in place); TTSR rule notice; framed custom messages; compaction/branch summary dividers (Ctrl+O expands); retry countdown and compaction rows; text after a tool call below its card; replay from the session tree on resume/branch/compaction (tool cards included). Markdown: marked-style block lexer with omp's per-token spacing, `- `/`N.` lists with depth indent and hanging wraps, tasks/setext/indented code/HTML blocks/ref defs, `▏` quotes, uncolored sharp tables with row rules, `[text] (url)` links (OSC 8 per omp's terminal policy), image alt text, entities, `<br>`/`<code>`, glyph-preset hr. Code highlighting (`src/tui/highlight.c`) follows omp's syntect scope→color mapping for C/C++, JS/TS, Python, Rust, Go, JVM, PHP, shell, JSON, YAML, TOML/INI, SQL, Ruby, Lua, diff, HTML, CSS | 🟡 | identical cells to omp v18.14.2 for the tests/tui/fixtures documents (golden captures in tests/tui/golden, checked by test_tui), thinking, user bubbles, banner, working row, abort and resume in tests/tui/compare.sh. Not yet: syntect-exact highlighting for every grammar (hand-written lexers approximate it; unsupported languages render plain), math/LaTeX, mermaid, svg figures/table charts/images, tree-guide hanging wraps, streaming speculation (open links/tables), reactions, cache-miss/served-model markers, read-tool grouping, advisor/IRC/async-job cards, provider error bodies (omp adds `(type=…)` and `raw-http-request=` lines) |
| Slash commands: help model thinking/effort plan goal guided-goal new clear resume compact handoff session name/rename export copy dump fork tree branch retry btw queue shake delete todo tools context usage skills mcp dirs hotkeys quit/exit | ✅ |
| ACP mode (`--mode acp`: initialize, session/new/load/list/prompt/cancel/set_mode, streamed message/thought/tool_call/plan/usage updates, session/request_permission for edit/execute tools) | ✅ |
| collab, share, stats dashboard, Agent Hub, git UI, marketplace/plugins, settings menu, vibe, loop, ratchet, security scans, tan, omfg, cleanse, wt, add-dir, restart | ❌ |
| OAuth login (generic PKCE flow from omp descriptors; upstream authorize-URL parameter order/encoding, templated authorizeParams, optional PKCE, local `/launch` shortcut; TUI login dialog) | ✅ | device-code / custom hook providers ❌ |
