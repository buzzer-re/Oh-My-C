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
| bedrock-converse-stream, cursor-agent, google-gemini-cli, devin, gitlab-duo | ❌ |
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
| Goal mode, advisor, memory backends, autolearn | ❌ | |

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
| find, ast_grep, ast_edit, browser, computer, github, ssh, image_gen, tts, checkpoint/rewind | ❌ | |

## Modes & UI
| Feature | Status |
|---|---|
| Print mode (text / json), RPC mode (core commands) | ✅ |
| Interactive TUI (streaming markdown, tool cards, editor, autocomplete, pickers, status line, themes) | ✅ |
| Slash commands: help, model, thinking, new, resume, compact, session, name, export, copy, fork, clear, quit | ✅ |
| ACP mode, collab, stats dashboard, Agent Hub, /tree, /review, /commit, /annotate | ❌ |
| OAuth login (generic PKCE flow from omp descriptors) | ✅ | device-code / custom hook providers ❌ |
