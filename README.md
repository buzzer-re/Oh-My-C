# omc — oh-my-c

A from-scratch C rewrite of [oh-my-pi (omp)](https://github.com/can1357/oh-my-pi), the coding agent.
No Node, no Bun, no npm, no vendored third-party code: the only runtime dependencies are the
system's libc, OpenSSL (TLS), zlib (gzip) and pthreads.

```
$ ldd build/omc
    libssl.so.3  libcrypto.so.3  libz.so.1  libc.so.6
```

Everything else is implemented in this tree: HTTP/1.1 + SSE client, JSON, YAML, a linear-time regex
engine (with a bounded backtracking fallback for look-around), gitignore-aware parallel file walker,
ripgrep-style search, Handlebars templates, a differential terminal UI with markdown rendering,
the LLM provider protocols, the agent runtime, MCP and LSP clients.

omp's prompts (system prompt, tool descriptions, compaction, rules) are embedded verbatim
(MIT, see `prompts/UPSTREAM-LICENSE`) so the model sees the same instructions as in omp.

## Build

```sh
make            # -> build/omc  (needs a C11 compiler, OpenSSL and zlib headers)
make test       # unit tests + end-to-end tests against a local mock model server
sudo make install PREFIX=/usr/local
```

## Use

```sh
export ANTHROPIC_API_KEY=...        # or OPENAI_API_KEY, GEMINI_API_KEY, OPENROUTER_API_KEY, ...
omc                                 # interactive TUI
omc "fix the failing test"          # interactive with an initial prompt
omc -p "summarize src/"             # print mode
omc --mode json -p "..."            # JSON event stream
omc --mode rpc                      # JSONL RPC over stdin/stdout (omp protocol)
omc -c                              # continue the last session;  -r <id> resumes one
omc login anthropic                 # OAuth (Claude Pro/Max, ChatGPT/Codex, ...)
omc models opus                     # list models
omc read src/main.c                 # run the read tool directly (also: grep, glob)
```

Configuration lives in `~/.omc/agent/` (`config.yml`, `models.yml`, `auth.json`, `mcp.json`,
`lsp.json`, `AGENTS.md`, `RULES.md`, `skills/`, `rules/`, `agents/`). Projects can use `.omc/` or
omp's `.omp/` directories; `AGENTS.md`/`CLAUDE.md` and other tools' context files are discovered
like in omp. Sessions are stored as omp-compatible JSONL under `~/.omc/agent/sessions/`.

## Embedding (optional library build)

```sh
make lib            # -> build/libomc.a, build/libomc.so.0, build/omc.pc  (public API: include/omc.h)
sudo make install-lib PREFIX=/usr/local
cc app.c $(pkg-config --cflags --libs omc)        # or: -Iinclude build/libomc.a -lssl -lcrypto -lz -lpthread -lm
```

The library exports only the `omc_*` API (the static archive is prelinked so internal names never
clash with the host app). A session speaks the same JSON protocol as `omc --mode rpc`: send commands
with `omc_send()`, receive responses and agent events through one callback. `omc_prompt()` runs a
turn synchronously, `omc_register_tool()` adds native tools implemented by the host, and
`omc_main()` runs the whole CLI/TUI in-process. See `examples/embed.c`.

## Documentation

Architecture and design docs live in [docs/](docs/index.md): how omc is layered and how each module
maps to omp ([architecture](docs/architecture.md)), [build and embedded assets](docs/build.md), the
[provider layer](docs/ai.md), the [agent runtime](docs/agent.md), [tools](docs/tools.md), the
[terminal UI](docs/tui.md), [run modes and the library API](docs/modes.md), [testing](docs/testing.md)
and [security](docs/security.md). The same files are embedded in the binary and readable by the
model as `omc://`.

See [docs/status.md](docs/status.md) for what is and is not implemented yet, and
[docs/CONVENTIONS.md](docs/CONVENTIONS.md) for coding rules.
