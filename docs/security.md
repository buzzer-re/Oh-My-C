# Security

omc exists partly as a supply-chain-safe replacement for omp: one auditable C program instead of a
Bun runtime plus an npm dependency tree and native addons. This page covers what that buys, where
the trust boundaries are, and how secrets are handled.

## Supply chain

- **Minimal dependencies.** The binary links only libc, OpenSSL, zlib, pthreads and libm, all from
  the system ([build.md](build.md#dependencies)). No package manager runs at build time and nothing
  is downloaded. There is no Node, Bun, npm, Python or Rust in the build.
- **No vendored third-party code.** Parsers and protocols (JSON, YAML, regex, markdown, HTTP/1.1,
  WebSocket, protobuf, SQLite/PDF/zip/tar readers, the TUI) are written in this tree. omp's prompts,
  themes, symbols, tips and catalog data are copied as *data* under omp's MIT license
  (`prompts/UPSTREAM-LICENSE`, `assets/tui/UPSTREAM-LICENSE`).
- **Self-contained binary.** Every asset is compiled in by the 29-line `build-tools/embedgen.c`, so
  nothing is read from an install directory that could be swapped. User overrides (prompts, themes,
  settings) are read only from the user's own agent dir and project dirs.
- **Dev-only scripts stay dev-only.** `build-tools/*.py`/`*.sh` and everything under `tests/`
  regenerate assets or test the binary. The binary never runs them. The only Python omc ever runs is
  the `eval` tool's kernel, at the model's request, using the system interpreter and the embedded
  kernel source passed with `-c` (never written to disk).
- **Hardened build.** `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, PIE and full RELRO
  (`-z relro -z now`). The library build keeps only the `omc_*` symbols global.

## Trust boundaries

A coding agent runs model-chosen commands with the user's privileges. omc follows omp's model here:

- Tools (`bash`, `write`, `edit`, `eval`, `ssh://` writes, MCP tools) run **as the user, with no
  sandbox and no per-call approval**. omp's approval modes (`tools.approvalMode`) are not
  implemented, so omc behaves like omp's default (`yolo`). The editor-facing ACP mode asks the
  client for permission before edit and execute tools (`session/request_permission`).
- **Plan mode is a workflow guard, not a security boundary.** It blocks writes outside `local://`
  and refuses bash commands that *look* state-changing (pattern list in `coding/app.c`), but it
  cannot prove a command harmless.
- **Project content is untrusted input to the model.** Context files, rules, skills, MCP configs
  and `.omc/`/`.omp/` settings are loaded from the working tree as in omp. Opening a repository
  means trusting its agent configuration. MCP servers from project configs are started at session
  start (`mcp.enabled: false`, or `no_mcp` in the library options, disables this).
- **Network peers are untrusted.** Every byte from providers, MCP/LSP/DAP servers, web pages and
  remote hosts goes through bounded parsers (below).

## Untrusted-input parsers

The conventions ([CONVENTIONS.md](CONVENTIONS.md)) require bounds checks on everything from the
network, files or the model, no unbounded recursion on untrusted input, and no untrusted format
strings. In practice:

| Parser | Limits |
|---|---|
| JSON (`core/json.c`) | nesting depth 512 |
| YAML (`text/yaml.c`) | depth 256, alias expansion budget (1M nodes) |
| Regex (`text/regex.c`, `rxbt.c`) | linear-time NFA/DFA engine (no catastrophic backtracking), program size (400k insts), repetition count (10k), nesting (250), DFA memory (16 MiB). The look-around/backref fallback has a 20M-step budget |
| Glob | alternation count and depth caps |
| Markdown (`tui/markdown.c`) | nesting depth 8, table columns 32 |
| Archives, SQLite, PDF, notebooks (`tools/readfmt.c`) | every offset/length checked; caps on entries (1M), in-memory size (256 MiB), member size (64 MiB), path length, link depth (40); decompression output capped; covered by a mutation fuzzer in `test_readfmt` |
| HTTP (`net/http.c`) | buffered bodies capped (64 MB default), idle and total timeouts, redirect limit |
| WebSocket (`net/ws.c`) | 16 MiB message cap, 64 KiB handshake, server frames must be unmasked |
| Protobuf / Connect (`ai/pb.c`, Devin) | length-checked varints/fields, 16 MB frame cap |
| Tool output (`tools/outsink.c`) | UTF-8 repair, ANSI/OSC/DCS stripping and control removal before the model or terminal sees it; per-line and total byte caps with artifact spill |
| ssh reads | 1 MiB, must be valid UTF-8 without NUL |

## Process and path guards

- **Subprocesses** are spawned with argv arrays, not shell strings, except where a shell is the
  point (`bash` tool, `!cmd`, `!command` keys). The persistent shell is
  `bash --noprofile --norc`, and each command's end marker carries a random nonce, so output can't
  forge completion.
- **ssh://** (`tools/ssh.c`): host and user names starting with `-` are rejected
  (argument-injection guard), ports are validated, percent-escapes are checked, remote paths are
  POSIX single-quoted (`ssh_quote`), connections use `BatchMode=yes` with a private ControlMaster
  socket, and writes are staged to a temp file next to the target and then copied over an existing
  regular file or moved into place (directories and special files are refused).
- **Path clamping:** `local://` paths are normalized and clamped to the session's local root.
  `skill://<name>/<file>` cannot escape the skill directory. `artifact://` ids reject `/` and `..`.
  Theme names, `omc://` doc names and embedded asset lookups reject `/` and `..`.
- **Managed skills** (auto-learn) refuse symlinked roots, directories and `SKILL.md` files
  (`lstat`, `O_NOFOLLOW`, `O_EXCL`), enforce strict names, and cap the size at 64 KB.
- **TLS** verifies the peer by default (hostname via `SSL_set1_host`, or IP), using the system trust
  store plus optional `OMC_EXTRA_CA_CERTS` / `NODE_EXTRA_CA_CERTS` / `SSL_CERT_FILE`.
  `OMC_INSECURE_TLS=1` exists for tests only.
- **Test redirects** (`OMC_PROVIDER_URL_REWRITE`, `OMC_AUTH_URL_REWRITE`) are environment-only and
  meant for the e2e suite. Treat a process environment that sets them as hostile.

## Secrets

- **Storage:** credentials live in `<agent dir>/auth.json`, written atomically with mode `0600`.
  Session dirs are created `0700`. Logs, session pins, service state and FIFOs are `0600`.
- **Indirection:** an `auth.json` key may name an environment variable or a `!command` (for example
  a password-manager CLI), so the secret itself need not be stored. `models.yml` `apiKey` may name
  an env var.
- **Refresh:** OAuth tokens are refreshed 60 s before expiry under a process-wide lock, and the
  refreshed row is persisted. Other processes pick it up through `auth_reload()`.
- **In transit:** keys go only to the provider's configured base URL (or the URL its login
  stored), never to third parties. Keyless providers (`N/A` sentinel) send no Authorization header.
- **Redaction:** the memory pipeline redacts secret-looking keywords and JWTs before anything is
  sent for summarization or written to memory files (`coding/memory.c`, port of
  `memory-backend/redact.ts`). The login dialog masks secret answers. `xd://report_issue` QA reports
  are written to a local file only, and only after consent (`dev.autoqaConsent`). They are never
  uploaded.
- **Borrowing:** some login flows can reuse an installed desktop app's token (the Perplexity
  desktop app on macOS). `PI_AUTH_NO_BORROW=1` disables this.

## Reporting and review

When changing a parser, protocol or path-handling routine: keep the explicit caps, add a fuzz or
corruption case next to the existing ones (`test_readfmt`, `test_ws`, `test_text`), and note
security-relevant behavior here.
