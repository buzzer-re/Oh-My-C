# Windows

omc runs natively on Windows 10 (1809 or later; 1903+ for non-ASCII arguments and environment) and Windows 11, x64.
It is the same program as on Linux and macOS: the same TUI, tools, prompts, sessions and config, built from the same
sources by a MinGW-w64 cross build. Only system DLLs are imported (`KERNEL32`, `ADVAPI32`, `WS2_32`, `Secur32`,
`CRYPT32`, `bcrypt`, `msvcrt`); there is no OpenSSL, zlib or runtime to install.

## Install and build

```powershell
irm https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.ps1 | iex     # the latest release
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.ps1))) -Lib   # with options
.\install.ps1 -Build -Ref main                                                      # build a branch from source
.\install.ps1 -Build -Source C:\src\Oh-My-C -Lib                                    # a local checkout, plus libomc
```

`install.ps1` (Windows PowerShell 5.1 and PowerShell 7) installs `omc.exe` into `%LOCALAPPDATA%\Programs\omc`
(`-InstallDir`), adds it to the user PATH after asking (`-AddToPath`, `-Yes`, `-NoPath`), and renames a running
`omc.exe` aside before replacing it. A release install checks the zip against the release's `SHA256SUMS`. Run through
`irm ... | iex` it leaves the PowerShell session as it was (strict mode and the error preference are set inside its
function); `$env:OMC_VERSION`, `OMC_INSTALL_DIR`, `OMC_BUILD=1` and `OMC_NO_PATH=1` stand in for the parameters there. `-Build`
needs a MinGW-w64 toolchain and installs none: either WSL (`sudo apt install mingw-w64 make gcc`, the build runs
there and produces the Windows binary) or MSYS2 UCRT64 (`pacman -S mingw-w64-ucrt-x86_64-gcc make`).

From a shell with the toolchain:

```sh
make windows                      # build/omc.exe            (= make WINDOWS=1)
make WINDOWS=1 lib                # build/windows/libomc.{a,dll,dll.a}, omc.pc
make WINDOWS=1 test-build         # omc.exe, the unit tests, the ConPTY driver, both embed examples
make WINDOWS=1 MINGW= HOSTCC=gcc  # native MSYS2 build (no tool prefix)
```

The Windows objects live in `build/obj/Windows-x86_64`, the libraries in `build/windows/`, so a Linux build and a
Windows cross build share one checkout without clobbering each other. `make WINDOWS=1 dist` packs the release zip
(see [release.md](release.md#windows)). On a toolchain without `libssp` (newer mingw-w64 runtimes carry the stack
protector themselves, e.g. MinGW-Builds gcc 15 on GitHub's runners) it is left out of the link.

## libomc on Windows

`make WINDOWS=1 lib` (or the `lib\` and `include\` folders of the release zip, or `install.ps1 -Lib`, which puts them
next to `omc.exe`) gives:

| File | What it is |
|---|---|
| `libomc.dll` | the library; exports exactly the `omc_*` API of `include/omc.h` and imports only system DLLs (its C runtime is `msvcrt.dll`; libgcc and winpthreads are linked in) |
| `libomc.dll.a` | MinGW import library for the DLL |
| `libomc.a` | static library (MinGW), prelinked so only `omc_*` symbols are global |
| `libomc.def` | the export list, for making an MSVC import library |
| `omc.pc` | pkg-config file, relocatable: its prefix is the folder above it |
| `include/omc.h` | the public header (plain C; the same API as on Linux and macOS, see [modes.md](modes.md#library-embedding)) |

**MinGW-w64 (gcc or clang), using the DLL.** `-lomc` finds the import library before the static one:

```sh
gcc app.c -I<root>/include -L<root>/lib -lomc -o app.exe          # then put libomc.dll next to app.exe (or on PATH)
PKG_CONFIG_PATH=<root>/lib gcc app.c $(pkg-config --cflags --libs omc) -o app.exe
```

Defining `OMC_DLL` before including `omc.h` (`-DOMC_DLL`) declares the functions `__declspec(dllimport)`: a direct
call through the import table instead of a jump thunk. It is optional, and must stay undefined for the static
library.

**MinGW-w64, static.** Nothing to ship next to the program:

```sh
gcc -static app.c -I<root>/include <root>/lib/libomc.a -lpthread -lm -lws2_32 -lsecur32 -lcrypt32 -lbcrypt -lncrypt \
    -ladvapi32 -lshell32 -lole32 -luserenv -ldbghelp -lssp -o app.exe
PKG_CONFIG_PATH=<root>/lib gcc -static app.c $(pkg-config --static --cflags --libs omc) -o app.exe
```

**MSVC.** Use the DLL; `libomc.a` is a MinGW archive and does not link with MSVC. Make an import library from the
`.def` once, in a Developer Command Prompt, then link against it (not checked with MSVC here):

```bat
lib /def:libomc.def /machine:x64 /out:omc.lib
cl /I include app.c omc.lib
```

**Rules for the host program:**
- Memory omc returns (strings, results) is freed with `omc_free()`, never `free()`: the DLL has its own heap, and
  MSVC programs use a different C runtime.
- Strings in and out are UTF-8, paths included.
- `libomc.dll` is about as large as `omc.exe` (it embeds the prompts, catalog and docs) and has the same runtime needs:
  Git for Windows or PowerShell for the bash tool. `examples/embed.c` is a complete host; the tests build it both ways
  (`build/examples/embed.exe` static, `build/windows/embed_dll.exe` against the DLL).

## Runtime requirements

- **Git for Windows** (recommended) for the bash tool. omc runs real bash, found like omp's `resolveWindowsShell`:
  `Git\bin\bash.exe` under `%ProgramFiles%`, `%ProgramFiles(x86)%`, `%LOCALAPPDATA%\Programs`, `%GIT_INSTALL_ROOT%`
  or scoop, then a `bash.exe` on PATH that is not WSL's launcher (`System32\bash.exe`, the WindowsApps alias), then
  `sh.exe`. `OMC_SHELL` overrides it.
- **PowerShell** is used two ways (omp has no PowerShell tool; this is omc's Windows addition):
  - with a bash, the model also gets a `powershell` tool for Windows-specific work (registry, services, WMI/CIM, .NET,
    ACLs, installers); both descriptions tell it to prefer bash and use PowerShell only for those;
  - without any bash, the `bash` tool itself runs PowerShell, and its description says so (PowerShell syntax, a new
    process per call); there is no second tool. Hooks, `!` commands and services use it too. Tool commands (bash
    functions) are off; those tools are offered top-level instead.

  `pwsh.exe` (PowerShell 7) is used when installed, else Windows PowerShell 5.1. Each call is its own process with
  UTF-8 output and no progress bars; the exit status follows a shell's (`exit N`, else 0 when the last statement
  succeeded, the last native exit code or 1 when it failed). Approvals treat PowerShell commands like bash commands
  (`bash.patterns` apply); plan mode refuses them, since they cannot be checked for changes.
- A terminal: Windows Terminal is the target; the classic console (conhost) works, with its own rendering of
  emoji widths (see below).

## Where things are

| | Windows |
|---|---|
| Config and sessions | `%USERPROFILE%\.omc` (`HOME` first when set, like the tests and Git Bash do), same layout as `~/.omc` |
| Temp files | `TMPDIR` if set, else `%TEMP%`; omc sets `TMPDIR` to it at start so its children agree |
| `/tmp` | means `%TEMP%`, as Git for Windows mounts it, so omc and the bash tool see the same files |
| Editor | `$VISUAL`, `$EDITOR`, else Notepad, run as `cmd.exe /d /s /c "<editor> "<file>""` (omp's `resolveEditorSpawnCommand`) |
| Opening URLs | `powershell.exe -NoProfile -NonInteractive -EncodedCommand` with `Start-Process` (omp's `windowsOpenerCommand`) |
| Sleep prevention | `SetThreadExecutionState` on a dedicated thread (omp's natives power.rs) |
| Paths | `C:/x`, `C:\x`, `//server/share/x` and `/x` are absolute; omc keeps `/` internally and accepts `\` everywhere |

## How it works

omc is written against POSIX. On Windows every translation unit is compiled with `src/plat/win/posix.h` included
first, which maps the POSIX calls omc makes onto implementations in `src/plat/win/` (gnulib style, so the call sites
stay as they are); headers the C runtime lacks (`poll.h`, `sys/socket.h`, `termios.h`, `zlib.h`, the OpenSSL
headers, ...) are in `src/plat/win/include`. Code that cannot be expressed that way (fork) has `#ifdef _WIN32`
branches at its call sites.

| File | What it provides |
|---|---|
| `fd.c` | file descriptors: CRT fds for files, pipes and the console; a socket gets a CRT fd of its own (a NUL handle stands in), so read/write/close/poll/fcntl take both. `O_NONBLOCK` for pipes (PeekNamedPipe) and sockets. `poll` waits with `select` on sockets, on the console's input event, and checks pipes in 1-10 ms slices. Console input is read as key records (UTF-16 to UTF-8), console output written as UTF-16, so UTF-8 survives any code page. win32-input-mode records are decoded here (below). |
| `fs.c` | stat with symlinks and nanosecond times, `d_type` directories, UTF-8 paths through the wide APIs, files opened with full sharing (renamed over and deleted while open, as POSIX allows), POSIX-semantics delete and rename-replace, advisory locks (one byte far past the end, so Windows' mandatory byte locks never block I/O), `realpath`, `readlink`/`symlink` (symlinks need Developer Mode or admin), temp names, mmap, glob, `/proc/self/exe` |
| `proc.c` | `CreateProcessW` with MSVC-runtime quoting, a UTF-16 environment block, exactly the three stdio handles inherited (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), each child in a job object so `kill` ends the whole tree (a process group); `waitpid`/`waitid`/`kill` over a child table; ConPTY for `pty: true`; `posix_spawn` |
| `sig.c` | SIGINT/SIGTERM/SIGHUP from the console control handler (Ctrl+C, closing the window, logoff); SIGSEGV/SIGILL/SIGFPE from structured exceptions (the crash report keeps working); SIGWINCH from a resize watcher; `raise`/`kill(self)` |
| `term.c` | termios over console modes: raw mode is libuv's raw VT mode (what Bun's setRawMode uses): VT input, window events, no line/echo/processed input; VT output always on |
| `tls.c` | a TLS client over Schannel behind the OpenSSL calls `net/http.c` makes (non-blocking, `SSL_ERROR_WANT_*`), with the chain verified against the Windows store plus `OMC_EXTRA_CA_CERTS` / `NODE_EXTRA_CA_CERTS` / `SSL_CERT_FILE`, and failures reported as OpenSSL's `X509_V_ERR_*` codes, so the messages match Linux (`DEPTH_ZERO_SELF_SIGNED_CERT`, ...) |
| `crypto.c` | SHA-1/256/512, HMAC, random bytes and RSA-SHA256 signing (PKCS#8 / PKCS#1 PEM keys) over CNG |
| `zlib.c` | inflate (streaming, any input/output split), deflate (LZ77 + stored/fixed/dynamic blocks), zlib/gzip/raw framing, crc32/adler32, gz files; written for omc, checked against real zlib both ways |
| `regex.c` | POSIX `regcomp`/`regexec` over omc's own regex engine |
| `util.c` | UTF-8/UTF-16 conversion, errno mapping, the libc pieces the CRT lacks, start-up (`omc_win_init`, `omc_win_init_exe`) |

**Bash tool.** The persistent session shell is Git Bash. On Linux each command's output group goes through a FIFO;
a native process and Git Bash share no FIFO (an MSYS FIFO is not a Windows pipe, and Git Bash cannot open a Windows
named pipe reliably), so on Windows the group writes a per-command file that omc reads as it grows. Ordering, exit
codes, the EXIT trap, background jobs and `notify` behave as on Linux; a job left running writes into a file nobody
reads instead of a drained pipe. Tool commands (`lsp --help` inside bash, ...) reach omc over an AF_UNIX socket, which
Windows 10 1803+ has.

**Services** (`bash` with `name`) are supervised by omc itself (`omc __omc_service_supervisor <dir>`, started
detached) instead of a forked supervisor; the service tree lives in a job that ends with it, and its stdin is a file
the supervisor forwards.

**Keyboard.** Plain VT input folds Shift+Enter into Enter. Like omp on a local ConPTY, omc asks the console host for
win32-input-mode (`CSI ? 9001 h`) when the terminal does not answer the kitty keyboard query, and decodes the key
records into the bytes a legacy terminal sends, with kitty CSI-u for the chords legacy encoding loses (port of
omp's `windows-input-mode.ts`): Shift+Enter inserts a newline, AltGr text and dead keys compose. Over SSH it keeps
modifyOtherKeys.

**Rendering.** omp's ConPTY rules are followed: every row rewrite ends at column zero (ConPTY materializes a pending
wrap before the next cursor move, which would scroll the status line into the transcript), and writes are cut into
16 KiB pieces at line boundaries (ConPTY's viewport tracking breaks on larger writes). The classic console draws some
pictographs (🗑, the scratch-directory icon) two cells wide while omp's width model counts one, which shifts the
status line in conhost; Windows Terminal does not have this problem. omp has the same.

## Testing on Windows

`tests/windows/run-tests.ps1` runs everything natively: the unit tests, the CLI, print mode with every core tool
against `tests/e2e/mock_openai.py`, sessions, two processes at once, both libomc builds, the TUI on a real console
and through ConPTY (Shift+Enter, Ctrl+C), the RPC driver, TLS against github.com and a rejected self-signed server
(Git's `openssl`), `omc update` replacing a running copy of itself, and `install.ps1` run exactly as
`irm <url> | iex` (both against `tests/e2e/mock_release.py`, so GitHub is not contacted). It prints PASS/FAIL/SKIP per check and writes a report file. It needs a build
(`make WINDOWS=1 test-build` or `-Build`), Git for Windows and Python 3 for the mocks (checks that need a mock are
skipped without it; `-MockPort` uses a mock already running elsewhere). Everything runs in a throwaway HOME and agent
dir.

`tests/windows/conpty_drive.c` is the TUI driver it uses: it starts a program on a pseudo console (`-m conpty`, its
VT output parsed by a small emulator) or in a hidden real console window (`-m console`, keys injected as key events,
the screen read back from the console buffer) and runs a script of `send`, `key` (with modifiers), `raw`, `expect`,
`screen` and `wait` steps. Windows 10's inbox console never hands win32-input-mode records to a program, but Windows
Terminal's OpenConsole does, as key events without a virtual key; `raw` injects text that way, so the record decoder is
tested without Windows Terminal (it was also checked in Windows Terminal 1.24 with injected keystrokes).

The Linux suite (`make test`, `tests/e2e/run.sh`) is unchanged and stays the reference; the Windows script covers the
platform-specific paths rather than re-running all 970 e2e checks, many of which assume a POSIX environment
(`/tmp` paths, `kill -STOP`, ptys from Python's `pty` module).

## Updating

`omc update` installs from the release zip (`omc-<version>-windows-x86_64.zip`, checked against `SHA256SUMS`, read by
a strict zip reader in `src/core/tarx.c`). Windows cannot replace a running executable but can rename one, so the
running `omc.exe` is renamed to `omc.exe.old-<id>` first and the new one put in its place; the old copy is removed
when nothing runs it any more (at the next update, or by `install.ps1`). The startup notice and `update.auto` work as
on Linux. Re-running `install.ps1` also updates a release install.

## Known differences

- No `omc zsh`/`bash`/`sh` integration (not ported anywhere) and no PowerShell aliases (`profile-alias.ts`).
- Desktop notifications on BEL terminals use the Linux D-Bus path only; Windows toasts are not sent.
- The console window title is not set from the run state beyond what VT title sequences do.
- Symlink checks in the unit tests are skipped without Developer Mode; Windows directories have no write bit, so the
  read-only-directory session test is skipped.
