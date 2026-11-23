# Native Windows port: guide for the agent doing it

This is a suggested plan, written on the Linux dev VM before the work moved to a Windows machine with WSL. Nothing
from it is implemented yet; `main` (omc v19.1.0) builds on Linux and macOS only. Treat the decisions below as
recommendations: verify them against the code and change them where the code says otherwise.

## Goal and order

1. `omc.exe` and `libomc.dll` (the opt-in library, `make lib`, public API `include/omc.h`) working natively on
   Windows 10 1809+ x64, with the same behaviour as on Linux.
2. An `install.ps1` (twin of `install.sh`) and a comprehensive Windows test script the user can run.
3. Only once the Windows build is stable: a `windows-latest` job in GitHub Actions CI and Windows release assets.
   Don't add CI or release work before that (the user's explicit order). Never tag or release from the port branch.

## Rules that still apply

Read `AGENTS.md` and `docs/CONVENTIONS.md` first. In short:

- **C11 only** in the product. PowerShell is fine for `install.ps1` and the Windows test script; Python only for test
  mocks and drivers under `tests/` and dev tools in `build-tools/`.
- **Dependencies:** Linux/macOS link only libc, OpenSSL, zlib, pthread, libm. Don't add libraries. No vendored code.
- **No package managers run by the agent, ever** (apt, pacman, choco, winget, pip, npm, cargo...), not even in a temp
  dir, and never fetch third-party code or run its build scripts. Scripts we write may *tell* the user what to install.
- **Parity is the spec:** omc behaves 1:1 like omp. Where omp has Windows-specific behaviour, port it from upstream's
  source (read as text; `git grep 'win32'` in oh-my-pi finds ~460 `process.platform === "win32"` branches in ~90
  coding-agent files). Never execute upstream code.
- **The shell is real bash.** On Windows that means Git Bash's `bash.exe` (Git for Windows). Never port omp's Rust
  shell engine (`crates/shell`) or its built-ins, and don't replace the bash tool with PowerShell. If bash is missing,
  omc says so clearly.
- **Never touch the user's real `~/.omc` / `~/.omp`** (nor `%USERPROFILE%\.omc`) or real credentials in tests; use the
  mocks in `tests/e2e/` and throwaway `HOME` / `OMC_CODING_AGENT_DIR`. Never contact real model providers.
- **Linux and macOS must not change behaviour:** `make -B -j8` with zero warnings, `make test` green,
  `build-tools/check_apple.sh` OK, after every step.
- **Commits:** end messages with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` only. Never add a
  `Claude-Session:` trailer.
- Keep `docs/status.md` (single source of truth) and a new `docs/windows.md` current.

## How omp does Windows (and why omc can't copy it)

omp is TypeScript on Bun, which is already cross-platform: processes, pipes, sockets, the event loop, files, TLS
(BoringSSL inside Bun) and gzip come from Bun. Rust native crates cover the rest (`crates/natives/src/pty.rs` uses
ConPTY, `altgr.rs`, `tty_writer.rs`), and since v18.17 its own Rust shell runs commands without bash. omc is C written
directly against POSIX, so it has to build what Bun gives omp: a platform layer, a Windows event loop, ConPTY, and TLS
and compression on Windows.

## What is POSIX-specific today (measured at v19.1.0, ~276k lines of C)

| Area | Sites | Notes |
|---|---|---|
| `fork` / `exec*` / `waitpid` | ~45 sites, ~10 files | `src/core/proc.c` and others |
| `openpty` / `forkpty` | 2 files | bash `pty: true` |
| `termios` (raw mode) | 5 files | TUI input |
| `sigaction` / `signal` | 19 files | SIGINT/TERM/HUP/PIPE/CHLD/WINCH, crash handler |
| `poll()` on pipes and sockets | 16 files | Windows can't poll pipes: the hard part |
| `flock` / `F_SETLK` | 7 files | session, auth, update install locks |
| `/proc/...`, `getpwuid` | ~6 files | exe path, user info |
| `AF_UNIX` | `src/tools/toolcmd.c` | tool commands; Windows has AF_UNIX (`afunix.h`) since 1803 |
| pthreads | ~3,000 calls, 130 files | mostly fine as is with MinGW winpthreads |
| OpenSSL | 16 files | TLS (`net/http.c`, `net/ws.c`), SHA-1/256, HMAC (AWS SigV4), `RAND_bytes`, RSA-SHA256 from PEM (`ai/google_auth.c`) |
| zlib | 13 files | `inflate*` (untrusted input), `deflate*` (PNG encode, gzip), `crc32`, one `gzwrite` |

macOS already has a small shim layer (`src/core/compat.h`, `compat_sys.h`): `pipe2`, `SOCK_CLOEXEC`, `getrandom`,
`execvpe`, `memrchr`. Every file that calls a shimmed function must include `core/compat_sys.h`.

## Recommended decisions

| Question | Recommendation | Why |
|---|---|---|
| Toolchain | MinGW-w64 GCC (`x86_64-w64-mingw32-gcc`, or MSYS2 UCRT64) | Same compiler and warnings as today; `pthread` = winpthreads, link it statically |
| Where to build | Cross-compile from WSL, run the `.exe` from WSL (Windows interop) | One place to edit, build and run both the Linux and the Windows binary |
| Paths and UTF-8 | Application manifest with `activeCodePage` UTF-8 and `longPathAware`; keep `/` internally | Plain C file APIs take UTF-8; Windows accepts `/`; handle drive letters in `path_abs`/`path_real`/`is_abs` |
| Config dir | `%USERPROFILE%\.omc` (HOME falls back to USERPROFILE) | Same as omp's `~/.omp` on Windows |
| Bash | Git Bash (`%ProgramFiles%\Git\bin\bash.exe`, PATH, or an override) | Keeps the real-bash rule; check omp's lookup and missing-bash message |
| TLS / crypto / compression | **Decide with the user.** (a) Windows builds of OpenSSL and zlib supplied by the user, linked statically (fastest); or (b) no third-party libraries on Windows: Schannel (SSPI) for TLS, CNG (`bcrypt`) for hashing/HMAC/random/RSA, and a from-scratch zlib-compatible inflate/deflate in C | (b) fits omc's "no third-party code" stance and lets the build link with only system DLLs, but is ~2-3k more lines (Schannel handshake loop, ALPN, chain validation mapped to omc's Bun-style error codes; a robust, fuzzed inflate) |
| System libraries | `ws2_32`, `secur32`, `crypt32`, `bcrypt`, `ncrypt`, `advapi32`, `shell32`, `ole32`, `dbghelp` | The Windows equivalent of libc |

## Plan

### Phase 1: platform layer, on Linux first (no behaviour change)

Add `src/plat/` with a small interface and POSIX implementations moved from today's code, then refactor call sites
file group by file group, keeping `make test` green:

- `plat_proc`: spawn (argv, env, cwd, pipes), wait, kill the whole tree, pty, process groups.
- `plat_poll`: wait on pipes, sockets and timers (replaces the `poll()` sites). Design this first and carefully: on
  Windows it becomes reader threads (or overlapped I/O) feeding a waitable queue, plus `WSAPoll` for sockets.
- `plat_term`: raw mode, size, resize notification (replaces SIGWINCH), reading input bytes.
- `plat_sig`: interrupt/terminate/hangup hooks, SIGPIPE, the crash handler (`<agent dir>/crash-<pid>.log`).
- `plat_fs`: locks, atomic replace, realpath, the executable's path, home/config/temp dirs, permissions.
- If option (b) above: `crypto.h` (hash, HMAC, random, RSA-SHA256 sign) and a TLS interface under `net/`, with the
  existing OpenSSL code as the POSIX backend.

### Phase 2: first Windows build

`make windows` (or `make CROSS=x86_64-w64-mingw32-`) producing `omc.exe` and `libomc.dll` + import lib:
`__declspec(dllexport)` for the `include/omc.h` API, an embedded manifest and version resource (`windres`), assets
embedded for PE/COFF (check how `make` embeds `assets/` today), `-static` so only system DLLs are imported
(`objdump -p` to check). First milestones: `omc.exe --version`; `omc.exe -p hi` against `tests/e2e/mock_openai.py`
(HTTP + TLS + JSON); the unit tests.

### Phase 3: processes and the shell

`CreateProcessW` with job objects (kill tree), correct command-line quoting (CommandLineToArgvW rules), environment
blocks, Git Bash discovery, the bash tool, ConPTY for `pty: true`, the tool-command socket (`afunix.h`; Git Bash shell
functions call `omc.exe __tool-command`). LSP, DAP and MCP over stdio then come for free through `plat_proc`.

### Phase 4: TUI

`SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_INPUT` / `ENABLE_VIRTUAL_TERMINAL_PROCESSING` and CP_UTF8: keys
arrive as VT sequences, so omc's key parser, renderer and mouse support stay. Resize via `WINDOW_BUFFER_SIZE_EVENT`.
Ctrl+C via a console control handler. Target Windows Terminal; conhost best effort. Port omp's Windows key handling
(AltGr, see `crates/natives/src/altgr.rs` and the TS callers).

### Phase 5: install and test scripts

- `install.ps1`: default mode installs a release zip verified against `SHA256SUMS` (for later); `-Build -Ref <branch>`
  builds from source after checking prerequisites (MinGW toolchain, Git for Windows, Python 3 for tests) and printing
  what's missing without installing anything; installs to `%LOCALAPPDATA%\Programs\omc` (+ `lib`, `include` for
  libomc), adds it to the user PATH with consent, renames a running `omc.exe` aside before replacing it. Works in
  Windows PowerShell 5.1 and PowerShell 7; all logic in functions.
- `tests/windows/run-tests.ps1`: one command, PASS/FAIL/SKIP per check, a summary and a report file to paste back.
  Throwaway HOME/agent dir, local mocks only. Cover: unit tests; `--version`/`--help`; print mode; every core tool
  (read/write/edit/grep/glob/find, bash via Git Bash incl. a background job and kill, a tool command such as
  `lsp --help` inside bash, todo); sessions, resume, export; RPC drivers that run on Windows Python (no `pty` module
  there); `omc update --check` against `tests/e2e/mock_release.py` with a Windows asset; TLS to github.com (the only
  real host) and a self-signed-cert rejection; `local://` spills; paths with spaces and non-ASCII; locks between two
  processes; Ctrl+C; a TUI smoke test through a small ConPTY driver in C (start omc at a fixed size, send keys, parse
  the VT output into a grid, check the banner/composer/answer); and a libomc.dll embedding test.

### Phase 6 (later): CI and releases

Only after the Windows build is stable: a `windows-latest` job in `.github/workflows/ci.yml` and release.yml, an
`omc-<v>-windows-x86_64.zip` asset, and `omc update` taught the Windows asset name and the rename-aside replace.
After that, upstream updates port omp's Windows-specific commits instead of skipping them (~20 per release).

## Pitfalls seen in CI that apply here

- A new socket using `SOCK_CLOEXEC` without `#include "core/compat_sys.h"` broke macOS.
- `mkdtemp` needs `<unistd.h>` on macOS; `memmem`/`strcasestr` need `#define _GNU_SOURCE` on glibc 2.35 (the CI
  baseline). clang warns on adjacent string literals in array initializers (`-Wstring-concatenation`).
- `tests/e2e/run.sh` picks a random base port; an "Address already in use" from a mock is a collision, rerun.
- `make test` only under a lock if several agents share a machine; the full suite takes ~30-40 minutes.

## Useful references

- `AGENTS.md`, `UPDATE.md`, `docs/architecture.md` (module map), `docs/testing.md`, `docs/build.md`, `install.sh`,
  `build-tools/ci.sh`, `.github/workflows/release.yml`.
- Upstream: `crates/natives/src/{pty,altgr,tty_writer}.rs`, `packages/coding-agent/src/**` (`win32` branches),
  `packages/coding-agent/src/cli/profile-alias.ts` (PowerShell aliases).
