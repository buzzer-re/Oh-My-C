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
| google-generative-ai / google-vertex (API key, bearer) | ✅ |
| ollama-chat | ✅ |
| bedrock-converse-stream (SigV4 + bearer key, AWS credential chain: env / web identity / profile static-SSO-credential_process-role_arn / ECS / IMDSv2, event-stream, geo-aware region, cache points, thinking/effort/binding controls, guardrails) | ✅ (Bedrock server-side compaction via InvokeModel ❌) |
| cursor-agent, google-gemini-cli, devin, gitlab-duo | ❌ |
| Codex websocket transport, Anthropic server-side fallback/compaction | ❌ |

## Agent & session
| Feature | Status | Notes |
|---|---|---|
| Date/cwd `<system-reminder>` injected per request into user turns (system prompt stays byte-stable; turns keep the reminder they were first sent with; a changed day/cwd attaches to the newest unsent user turn; setting `dateCwdReminder`) | ✅ | multi-root inheritance across compaction/branches simplified (resets per conversation root) |
| Agent loop, parallel shared tools / exclusive tools, steering, follow-ups | ✅ | |
| Session JSONL (title slot, v3 header, tree, branch/fork, resume, continue) | ✅ | |
| Compaction (auto + `/compact`), auto-retry, context-overflow recovery | ✅ | |
| Title generation | ✅ | |
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
| write, edit (hashline PUT/CUT/REM/MV, registers, block anchors; replace mode) | ✅ | apply_patch/patch/sloppy modes ❌ |
| bash (persistent shell session, timeouts, PTY, artifact spill, `async` background jobs delivered as follow-ups / idle wake-up, `wait` tool, `/jobs`) | ✅ | services (launch), async task jobs ❌ |
| grep, glob | ✅ | |
| todo, ask | ✅ | |
| web_search (Brave, Tavily, Exa, DuckDuckGo) | 🟡 | other 19 providers ❌ |
| lsp (55 bundled servers; all 13 actions; diagnostics on write) | ✅ | |
| MCP (stdio, streamable HTTP, SSE; omp/Claude/Cursor/VS Code configs) | ✅ | OAuth for MCP ❌ |
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
| Interactive TUI: 1:1 visual/behavioral parity with `omp` (verified side by side in tmux against the same mock model) | 🟡 | in progress. Diverges today: welcome screen (gradient logo, block-letter wordmark, version, rotating tips), first-run setup wizard (providers → default model → glyph mode → composer shape → theme), status band (`π > model > path ▶──ctx%──…╎──┃── window`), curved `╰─` composer with right hints, right-aligned session title, bordered tool cards (bash: `$ cmd` / Output / `Wall · Timeout` footer), user/assistant message chrome, model-written session titles with emoji/code, startup skill-description compression, `<system-reminder>` date/cwd injection |
| Slash commands: help model thinking/effort plan goal guided-goal new clear resume compact handoff session name/rename export copy dump fork tree branch retry btw queue shake delete todo tools context usage skills mcp dirs hotkeys quit/exit | ✅ |
| ACP mode (`--mode acp`: initialize, session/new/load/list/prompt/cancel/set_mode, streamed message/thought/tool_call/plan/usage updates, session/request_permission for edit/execute tools) | ✅ |
| collab, share, stats dashboard, Agent Hub, git UI, marketplace/plugins, settings menu, vibe, loop, ratchet, security scans, tan, omfg, cleanse, wt, add-dir, restart | ❌ |
| OAuth login (generic PKCE flow from omp descriptors) | ✅ | device-code / custom hook providers ❌ |
