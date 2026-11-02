# Build

## Quick reference

```sh
make                 # build/omc (parallel builds with -j work)
make test            # unit tests + e2e suite (needs python3 for the mock servers)
make lib             # optional: build/libomc.a, build/libomc.so.0, build/omc.pc
make install         # installs ~/.local/bin/omc (PREFIX defaults to $HOME/.local)
make install-lib     # installs PREFIX/include/omc.h, PREFIX/lib/libomc.{a,so*}, PREFIX/lib/pkgconfig/omc.pc
make clean
```

Variables: `CC`, `OPT` (default `-O2`), `CFLAGS` (default `$(OPT) -g`), `PKG` (pkg-config),
`PREFIX` (default `$HOME/.local`; use `sudo make install PREFIX=/usr/local` for a system-wide install), `DESTDIR`, `OBJCOPY`.

The build must stay free of warnings. On the shared development filesystem, file timestamps can be
skewed, so use `make -B -j8` to force a full rebuild (see [AGENTS.md](../AGENTS.md)).

## Dependencies

The binary links against nothing but system libraries:

| Library | Used for |
|---|---|
| libc (glibc or compatible; POSIX plus `openpty` from `<pty.h>`) | everything |
| OpenSSL (`libssl`, `libcrypto`) | TLS, SHA/HMAC (SigV4, PKCE, WebSocket keys), RS256 JWT signing (Google ADC), random bytes |
| zlib | gzip/deflate HTTP bodies, the compressed catalog assets, zip/tar.gz/gz reads, AWS event-stream CRC32 |
| pthread | threads (see [architecture.md](architecture.md)) |
| libm | color math, scoring |

The `Makefile` gets OpenSSL flags from `pkg-config --libs openssl`, falling back to
`-lssl -lcrypto`. No third-party source is vendored. The regex engine, YAML, JSON, markdown,
HTTP, WebSocket, protobuf, SQLite/PDF/archive readers and the TUI are all in this tree.

Optional runtime programs (never needed to build): `bash` for the bash tool (`OMC_SHELL` overrides),
`python3` for the `eval` tool only (`python.interpreter` setting), `ssh` for `ssh://` URLs,
language servers / debug adapters for `lsp` / `debug`, and `gh` as a GitHub token source for
`pr://`/`issue://`.

## Compiler flags and hardening

Every object is compiled with:

```
-std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable -Wshadow=local
-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIC -Isrc -Iinclude -MMD -MP
```

and the executable is linked with `-pie -rdynamic -Wl,-z,relro,-z,now` (full RELRO, PIE). The
shared library also gets `-z relro -z now`. `-MMD -MP` produce header dependency files, so
incremental builds are correct. The Makefile compiles every `src/*.c`, `src/*/*.c` and
`src/ui/tools/*.c` automatically, so a new source file needs no Makefile edit.

## Embedded assets

Everything omc reads from its own distribution is compiled into the binary, so `build/omc` is a
single self-contained file:

1. `build-tools/embedgen.c` (built first as `build/embedgen` with the host compiler) turns a list of
   files into `build/embedded.c`. Each file becomes a `static const unsigned char[]` array, and the
   table `omc_embedded[] = {name, data, len}` is emitted in `strcmp` order: embedgen sorts its own
   arguments, because `embed_get()` (`src/core/embed.c`) binary-searches the table by name.
2. The file list is every file under `prompts/` and `assets/`, plus `README.md` and `docs/*.md`
   (served to the model as `omc://` docs).
3. Code reads an asset with `embed_get("assets/...", &len)`. Prompts go through
   `prompt_load()` (`src/prompt/template.c`), which first looks for a user override at
   `<agent dir>/prompts/<rel>` and otherwise uses the embedded `prompts/<rel>`.

What is embedded and why:

| Path | Content | Why |
|---|---|---|
| `prompts/**` (about 370 files, MIT, `prompts/UPSTREAM-LICENSE`) | omp's prompt markdown verbatim: system prompt, tool descriptions, compaction, agents, rules, memory, goal, advisor, judgment, provider notes, pi-edit docs | The model must see byte-identical instructions to omp (checked by `tests/parity/request_diff.sh`) |
| `assets/models.json.z` | zlib-compressed model catalog (5,948 models, 79 providers): omp's `catalog/src/models.json` with per-API `compat` defaults factored out | Model metadata, costs, context windows, compat flags without network discovery |
| `assets/providers.json.z` | provider metadata (env vars, default models, base URLs) and the login descriptors of omp's auth registry | `omc login`, key resolution, `omc models` |
| `assets/tui/themes/*.json` | omp's 102 bundled themes (`dark`, `light`, the default `titanium`, ...; theme format unchanged) | `/settings` theme picker, setup wizard |
| `assets/tui/symbols.json` | glyph presets `unicode` / `nerd` / `ascii` and spinner frames from omp `theme/symbols.ts` | 1:1 glyphs |
| `assets/tui/settings-ui.json` | omp's full `/settings` registry (tabs, groups, labels, descriptions, options, defaults, conditions) | `/settings` lists exactly omp's settings in omp's order |
| `assets/tui/slash-commands.json` | builtin slash-command metadata (registry order, icons, descriptions) | autocomplete parity |
| `assets/tui/tips.txt` | the welcome-screen tips | welcome banner |
| `assets/lsp-servers.json`, `assets/dap-adapters.json` | omp's bundled language-server and debug-adapter definitions | `lsp` and `debug` tools |
| `assets/eval_kernel.py` | the Python side of the `eval` tool | passed to `python3 -u -c` at kernel start (never written to disk) |
| `README.md`, `docs/*.md` | these docs | served to the model as `omc://` |

`assets/tui/UPSTREAM-LICENSE` covers the theme, symbol, settings and tips data copied from omp.

## Dev-time tools (`build-tools/`)

Only `embedgen.c` takes part in the build. The rest regenerate committed assets from an upstream
checkout. They are run by hand when omp is updated, and the binary never runs them:

| Tool | Regenerates | Notes |
|---|---|---|
| `sync_prompts.sh <omp checkout> [--apply]` | `prompts/` | reports changed, removed and new upstream prompt files (`prompts/<pkg>/<path>` mirrors `packages/<pkg>/src/<path>`; `prompts/crates/pi-edit/` mirrors `crates/pi-edit`) and copies them with `--apply` |
| `slim_catalog.py <models.json> <out>` | `assets/models.json.z` | factors per-API compat defaults, zlib-compresses |
| `extract_symbols.py <symbols.ts> <out>` | `assets/tui/symbols.json` | asserts all presets have the same keys |
| `extract_slash_commands.py <omp checkout>` | `assets/tui/slash-commands.json` | reads `slash-commands/builtin-*.ts` in registry order |
| `extract_settings.sh <omp checkout>` + `settings-dump/{dump.ts,stub.js}` | `assets/tui/settings-ui.json` | runs omp's own settings registry with the Bun runtime embedded in the installed `omp` (`BUN_BE_BUN=1`), npm deps stubbed, `HOME`/agent dir pointed at throwaway dirs so only defaults are dumped |

Use a checkout whose tag matches `omp --version`. Themes and tips are plain copies from the upstream
tree. [UPDATE.md](../UPDATE.md) lists the exact command for every asset as part of the upstream
update procedure.

## Optional library (`make lib`)

`make` builds only the executable. `make lib` additionally produces:

- `build/libomc.a`: all objects except `main.o` are prelinked into one relocatable object
  (`ld -r`), then `objcopy --keep-global-symbols=build/libomc.syms` keeps only the public
  `omc_*` symbols global. Internal names (`json_*`, `sb_*`, ...) can't clash with the host
  application's symbols.
- `build/libomc.so.0` (+ `libomc.so` symlink): linked with a version script (`build/libomc.map`)
  that exports the same symbol list.
- `build/omc.pc`: pkg-config file. Version from `OMC_VERSION` in `src/core/core.h`, with the
  system libraries in `Libs.private`.

`build/libomc.syms` is generated from the `OMC_API` declarations in `include/omc.h`. To add a public
function, declare it there with `OMC_API` and nothing else changes.

```sh
cc app.c $(pkg-config --cflags --libs omc)
cc app.c -Iinclude build/libomc.a -lssl -lcrypto -lz -lpthread -lm
```

`src/main.c` is just `return omc_main(argc, argv);`, so the executable itself is a thin client of
the library API. The API (`omc_session_new`, `omc_send`, `omc_prompt`, `omc_register_tool`, ...) is
described in [modes.md](modes.md#library-embedding). `examples/embed.c` is built by `make test` and
exercised by the e2e suite.

## Version

`OMC_VERSION` in `src/core/core.h` (currently `0.1.0`) is reported by `omc --version`,
`omc_version()` and `omc.pc`. `OMC_API_VERSION` in `include/omc.h` versions the C API. `omc_options_t`
carries `struct_size` so that new option fields can be added without breaking the ABI.

## Platforms

omc builds on Linux (glibc or musl; GCC or Clang) and macOS (Apple Clang).

- **Build tree per platform:** objects, the embed generator, generated sources and test binaries go to
  `build/obj/<uname -s>-<uname -m>/`, so one checkout can be shared between machines (e.g. a macOS host
  and a Linux VM) without one platform picking up the other's objects. `build/omc` is refreshed from the
  current platform's binary on every `make`.
- **macOS:** install OpenSSL with Homebrew (`brew install openssl@3`); the Makefile finds the keg-only
  prefix through `brew --prefix` (override with `make OPENSSL_PREFIX=...`). zlib comes from the SDK. PIE is
  the linker default; the GNU-only `-z relro/now` flags are not used. `make lib` produces
  `libomc.0.dylib` with only the `omc_*` symbols exported.
- **Linux-only interfaces** (`pipe2`, `SOCK_CLOEXEC`/`SOCK_NONBLOCK`, `MSG_NOSIGNAL`, `getrandom`,
  `execvpe`, `st_mtim`, `<pty.h>`) are provided on macOS by `src/core/compat.h`, included from `core.h`.
- **Checking the macOS branches from Linux:** `#ifdef __APPLE__` code never compiles in a Linux build, so a
  missing include there only fails on a Mac. `build-tools/check_apple.sh` compiles those branches
  syntax-only with `-D__APPLE__` (implicit declarations are errors, unused variables warnings); run it
  after touching any `__APPLE__` code.
