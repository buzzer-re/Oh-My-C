# omc documentation

omc ("oh-my-c") is a from-scratch C11 rewrite of the TypeScript coding agent
[oh-my-pi (omp)](https://github.com/can1357/oh-my-pi). The goal is a drop-in replacement: the same
model requests, tools, session files, RPC protocol and terminal UI, built from about 120k lines of C
with no dependencies beyond libc, OpenSSL, zlib, pthreads and libm. Most modules are line-for-line
ports of a named omp file, and the docs below always say which one.

## Reading order

| Doc | What it covers |
|---|---|
| [architecture.md](architecture.md) | Layers, data flow, threads, ownership and error conventions, process-wide vs per-session state, and the **module map** (omc file to omp file) |
| [build.md](build.md) | `make`, dependencies, hardening flags, embedded assets, dev-only tools, `make lib` and the public C API |
| [ai.md](ai.md) | Provider layer: model catalog, API registry, stream events, every provider and transport, credentials and login flows, retries, model roles |
| [agent.md](agent.md) | Agent loop, tool execution, hooks, sessions (JSONL tree, compaction, branching), system prompt and discovery, TTSR, goal mode, advisor, memory, titles, plan mode, checkpoints, subagents and jobs |
| [tools.md](tools.md) | Every tool and its omp counterpart, `xd://` devices, internal URLs, output capture and artifacts, the eval kernel protocol, MCP/LSP/DAP clients |
| [tui.md](tui.md) | TUI framework (components, differential renderer, terminal), theme and symbols, UI modules, and how parity with omp is checked |
| [modes.md](modes.md) | Interactive, print/json, RPC, ACP and library embedding |
| [testing.md](testing.md) | Unit tests, e2e suite and mocks, side-by-side TUI harness, request parity diff, golden captures, adding tests |
| [security.md](security.md) | Supply-chain stance, trust boundaries, untrusted-input parsers, subprocess and path guards, secret handling |
| [status.md](status.md) | **Single source of truth for what is implemented** (feature tables with ✅ / 🟡 / ❌) |
| [CONVENTIONS.md](CONVENTIONS.md) | Coding rules for contributors |
| [../UPDATE.md](../UPDATE.md) | How to port later upstream omp commits; holds the **upstream pin** (last omp commit ported) |
| [../AGENTS.md](../AGENTS.md) | Short rules for coding agents working on this repo |

New contributors should read architecture.md first, then the doc for the area they will change. The
top-level [README](../README.md) covers installation and day-to-day use. The upstream version omc
matches is the pin in UPDATE.md (currently omp v18.14.2).

## How these docs are used at runtime

`make` embeds `README.md` and every `docs/*.md` file into the binary (see [build.md](build.md)). The
model can read them through the `omc://` internal URL (`read omc://` lists them, `read omc://agent.md`
opens one), the same way omp serves its own docs through `omp://`. That means these files are also
model-facing reference material. Keep them factual, and keep them in sync with the code.

The listing works today, but reading a doc returns an empty file until the embed-order bug described
in [build.md](build.md#embedded-assets) is fixed.

`omc://` drops the `docs/` prefix, so a doc must not share a name with a top-level embedded file.
That is why this index is `index.md` and not `docs/README.md`, which would collide with the top-level
`README.md`. Subdirectories of `docs/` are not embedded.

## Maintenance rule

**Update the docs in the same commit as the feature.** Every change that adds, removes or changes
behavior must:

1. update the matching row in [status.md](status.md) (always), and
2. update the doc that describes the area (module map entries in architecture.md, tool lists in
   tools.md, protocol tables in modes.md, and so on). If no doc covers the area yet, add a section.

Describe what the code does, not what it is meant to do. If you are unsure, read the code; never
guess at upstream behavior without checking the omp source at the matching tag. Don't copy the
status tables into other docs. Link to status.md instead.
