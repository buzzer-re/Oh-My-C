# CI and releases

## Versions

omc's version is the omp release it matches: `OMC_VERSION` in `src/core/core.h`, which equals the upstream pin in
[UPDATE.md](../UPDATE.md). `omc --version` prints `omc/<version>`; the welcome banner and `omc --help` show
`v<version> (OMP equivalent)`. When an update pass moves the pin, it moves `OMC_VERSION` with it.

## CI (`.github/workflows/ci.yml`)

Every push to `main` and every pull request builds and tests on Linux x86_64 (`ubuntu-22.04`), Linux arm64
(`ubuntu-22.04-arm`) and macOS arm64 (`macos-15`):

1. `build-tools/ci.sh check-deps`: a C compiler, `python3`, OpenSSL 3 and zlib are present. Nothing is installed.
2. `build-tools/ci.sh build`: a single compiler warning fails the build.
3. `build-tools/ci.sh smoke`: `--version` and `--help` show the version, and the binary links nothing beyond libc,
   OpenSSL, zlib, pthread and libm (on macOS: the system libraries and Homebrew's OpenSSL).
4. `build-tools/ci.sh test`: `make test` on Linux; `make test-unit` on macOS, where the e2e suite hasn't been
   verified yet. Tests that need Python's Pillow skip themselves on the runners (see [testing.md](testing.md)).

`build-tools/ci.sh` runs the same way locally (`JOBS=3 build-tools/ci.sh build`).

## Releases (`.github/workflows/release.yml`)

Pushing a tag `v<OMC_VERSION>` (for example `v18.18.7`) publishes a GitHub release with a prebuilt binary per platform
and the source:

1. `prepare` checks the tag against `OMC_VERSION` and creates a **draft** release, with notes from
   `build-tools/release_notes.sh <version>`: install steps plus the `## omp v<version> update` section of
   [status.md](status.md), which must exist. It refuses to overwrite an existing release.
2. `build` runs on Linux x86_64 (`ubuntu-22.04`), Linux arm64 (`ubuntu-22.04-arm`) and macOS arm64 (`macos-15`): the CI
   steps, then `build-tools/ci.sh dist` (`make dist`: the binary, README and omp's license notice in
   `omc-<version>-<os>-<arch>.tar.gz`, smoke-tested as packaged) and uploads the archive with its `.sha256`. The
   macOS binary links Homebrew's OpenSSL statically (`STATIC_OPENSSL=1`), so it needs no Homebrew on the target Mac;
   the Linux binaries link the system's OpenSSL 3 and zlib, built against glibc 2.35.
3. `source` runs `build-tools/ci.sh dist-src` (`make dist-src`: `git archive` of the tagged commit into
   `omc-<version>.tar.gz`, then the extracted archive is built on its own and smoke-tested) and uploads it.
4. `publish` collects `SHA256SUMS` over every asset, uploads it and publishes the release, only after all of the above
   succeeded.

A failed job leaves a draft: fix it, delete the draft and the tag, and tag again:

```sh
make version                                   # e.g. 18.18.7, from src/core/core.h
build-tools/release_notes.sh 18.18.7 | less    # preview the notes
git tag -a v18.18.7 -m "omc v18.18.7"
git push origin main v18.18.7
```

`build-tools/ci.sh dist` and `build-tools/ci.sh dist-src` build the same archives locally into `build/dist/`.

## Install script (`install.sh`)

`curl -fsSL https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.sh | sh` installs the latest release's
prebuilt binary for the OS and architecture (Linux x86_64 / arm64, macOS arm64). It verifies the archive against the
release's `SHA256SUMS` (a mismatch installs nothing), checks on Linux that the system has the libraries omc links,
checks that the binary runs, and renames it over the old one so a running omc keeps working.

`--build` (`./install.sh --build`, `... | sh -s -- --build`, or `OMC_BUILD=1`) installs from source instead: it checks
that `make` and a C compiler exist, downloads and verifies `omc-<version>.tar.gz`, builds it in a temporary directory
with `OMC_JOBS` jobs (default: every CPU) and installs `build/omc`. A failed build prints the end of the build log and
which packages provide the compiler, OpenSSL and zlib headers on common systems; the script never installs them
itself. A platform without a prebuilt binary (e.g. an Intel Mac) is told to use `--build`.

It is plain POSIX `sh`, and the whole body runs from `main()`, so a download cut off halfway executes nothing.
`OMC_VERSION`, `OMC_INSTALL_DIR`, `OMC_BUILD`, `OMC_JOBS`, `OMC_REPO` and `OMC_DOWNLOAD_URL` override the defaults. The
latest release is found by following GitHub's `releases/latest` redirect, so it needs no API token. The raw URL serves
`main`, so the script only works once the repository is public (a private repository's raw files and release assets
need authentication).

## Supply chain

The workflows use no third-party actions: the code is checked out with `git`, the token is passed as a one-off
header and never stored, and releases are published with the `gh` CLI that GitHub's runners ship with. The binaries are
built and tested by the release workflow from the tagged commit, and `SHA256SUMS` covers every asset; `install.sh
--build` compiles the verified source archive on the user's own machine instead, for anyone who prefers not to run a
binary built on a CI runner.

## Platforms and limits

- **Linux binaries** link the system's OpenSSL 3 and zlib dynamically, so they get the distribution's security updates.
  Built on Ubuntu 22.04, they need glibc 2.35+ and OpenSSL 3.
- **The macOS binary** targets macOS 15+ on Apple silicon (the runner's release: Homebrew's static OpenSSL is built for
  it) and has OpenSSL built in. It isn't signed or notarized: a copy downloaded with a browser needs
  `xattr -d com.apple.quarantine omc` once. There is no Intel macOS binary (`install.sh --build` covers it).
- omc builds from source on Linux (glibc; checked on Ubuntu 22.04 with gcc 11 and bash 5.1, and newer) and macOS with
  Homebrew's OpenSSL 3.
- **`ubuntu-22.04-arm`** runners are free only for public repositories. For a private repository, remove the
  `linux-arm64` entries from both workflows, or the job waits for a runner that never comes.
- **macOS minutes** cost about ten times Linux minutes on private repositories.
