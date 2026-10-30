# omp → omc parity

Status of the port against upstream oh-my-pi. ✅ ported and tested · 🟡 partial · ❌ not yet.

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
| Agent loop, parallel shared tools / exclusive tools, steering, follow-ups | ✅ | |
| Session JSONL (title slot, v3 header, tree, branch/fork, resume, continue) | ✅ | |
| Compaction (auto + `/compact`), auto-retry, context-overflow recovery | ✅ | |
| Title generation | ✅ | |
| TTSR (stream rules, builtin rules, interrupt/inject/retry, tool reminders) | ✅ | regex conditions; AST conditions ❌ |
| Context files, skills, rules (`.omc/`, `.omp/`, AGENTS.md, CLAUDE.md, cursor, …) | ✅ | |
| Subagents (`task`, bundled + user agent definitions, parallel) | ✅ | worktree isolation ❌, IRC ❌, async jobs ❌ |
| Plan mode (local:// plans, xd://propose approval, read-only gating) | ✅ | |
| Goal mode (goal tool, hidden goal context + todo context, token/time budget accounting, budget-limit steer, auto-continuation with no-progress stop, pause on interrupt/resume, `/goal` + `--goal`, RPC `goal` command, mode_change persistence) | ✅ | guided-goal interview ❌ |
| Advisor (second model with read/grep/glob + `advise`, incremental session updates, nit/concern/blocker routing, steer mid-run, continuation at run end per continueSeverity, dedup, note budget, immune turns, `/advisor`, RPC `set_advisor`) | 🟡 | multiple named advisors, WATCHDOG.yml grants, secret obfuscation, transcript recorder ❌ |
| Memory backends, autolearn | ❌ | |

## Tools
| Tool | Status | Notes |
|---|---|---|
| read (selectors, hashline anchors, summaries, dirs, images, URLs, skill:// rule:// artifact:// local:// pr:// issue://) | ✅ | archives/sqlite/pdf/notebooks ❌ |
| write, edit (hashline PUT/CUT/REM/MV, registers, block anchors; replace mode) | ✅ | apply_patch/patch/sloppy modes ❌ |
| bash (persistent shell session, timeouts, PTY, artifact spill) | ✅ | services, async jobs ❌ |
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
| ssh:// URLs (ssh.json hosts, ControlMaster, POSIX shell probe, /~ HOME, 1 MiB UTF-8 reads, dir listings, staged byte-exact writes) | 🟡 | read/write ✅; grep/edit over ssh:// ❌ |
| browser, computer, image_gen, tts | ❌ | |

## Modes & UI
| Feature | Status |
|---|---|
| Print mode (text / json), RPC mode (core commands) | ✅ |
| Interactive TUI (streaming markdown, tool cards, editor, autocomplete, pickers, status line, themes) | ✅ |
| Slash commands: help model thinking/effort plan goal guided-goal new clear resume compact handoff session name/rename export copy dump fork tree branch retry btw queue shake delete todo tools context usage skills mcp dirs hotkeys quit/exit | ✅ |
| Slash commands: help, model, thinking, new, resume, compact, session, name, export, copy, fork, clear, quit | ✅ |
| ACP mode (`--mode acp`: initialize, session/new/load/list/prompt/cancel/set_mode, streamed message/thought/tool_call/plan/usage updates, session/request_permission for edit/execute tools) | ✅ |
| collab, share, stats dashboard, Agent Hub, git UI, marketplace/plugins, settings menu, vibe, loop, ratchet, security scans, tan, omfg, cleanse, wt, add-dir, restart | ❌ |
| OAuth login (generic PKCE flow from omp descriptors) | ✅ | device-code / custom hook providers ❌ |
