# AGENTS.md — omc (oh-my-c)

omc is a from-scratch C11 rewrite of [oh-my-pi](https://github.com/can1357/oh-my-pi) (omp) that must
behave 1:1 like omp: same TUI, same model-facing prompts/tools/output, same sessions and config.

**Upstream pin — last omp commit ported:** `7efd77219374ca4e8767979809835e4aa90666ba` (`v18.14.2`, 2026-10-31).
To pull later upstream fixes in, follow [UPDATE.md](UPDATE.md) and move this pin (and the one in
UPDATE.md) when done.

## Rules

- **Dependencies:** runtime links only libc, OpenSSL (libssl/libcrypto), zlib, pthread and libm. No Node/Bun,
  no vendored third-party code, no new libraries. Python is allowed only for dev-time tools in
  `build-tools/` and test mocks/fixtures in `tests/`.
- **Parity is the spec.** Port behaviour, exact strings, glyphs, colors and layout from the upstream
  source; when omc and omp differ, omc is wrong. Never advertise (in prompts/tool descriptions) a
  capability omc doesn't implement.
- **Never touch the user's real omp setup** (`~/.omp`) or real model credentials/tokens in tests or
  comparisons; use the mocks under `tests/e2e/` and throwaway agent dirs.
- **Default build stays a single binary:** `make` → `build/omc`. The library (`make lib`, public API in
  `include/omc.h`) is opt-in.
- Code style and conventions: [docs/CONVENTIONS.md](docs/CONVENTIONS.md).

## Build and test

```sh
make -B -j8        # -B: the shared filesystem has clock skew; must build with zero warnings
make test          # unit tests + tests/e2e/run.sh (mock servers, no network)
make lib           # optional: build/libomc.a, build/libomc.so.0, build/omc.pc
```

Parity harnesses (both run the installed `omp` next to `build/omc` against the same scripted mock model):

- `tests/tui/compare.sh` — TUI side by side in tmux (see its header for options).
- `tests/parity/request_diff.sh` — diff of the model request (tools, schemas, system prompt, messages).

## Documentation

- [docs/status.md](docs/status.md) — implementation status vs omp (single source of truth; update it in
  every change).
- [docs/](docs/) — architecture and module docs (keep them current when you change behaviour).
- [UPDATE.md](UPDATE.md) — how to track and port upstream omp commits; holds the upstream pin.
