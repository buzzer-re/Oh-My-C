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

Releases ship **source only**. There are no prebuilt binaries: everyone builds omc on their own machine, by hand or
with `install.sh`. Pushing a tag `v<OMC_VERSION>` (for example `v18.17.0`) runs one job on `ubuntu-22.04` that:

1. checks the tag against `OMC_VERSION`;
2. builds the tagged commit with zero warnings and runs `make test`;
3. runs `build-tools/ci.sh dist`: `make dist` packs the commit with `git archive` into
   `build/dist/omc-<version>.tar.gz` (top directory `omc-<version>/`, committed files only) and writes
   `build/dist/SHA256SUMS`, then the archive is extracted, built on its own and smoke-tested, so it is known to build
   without the git checkout;
4. creates the GitHub release with notes from `build-tools/release_notes.sh <version>` (install steps plus the
   `## omp v<version> update` section of [status.md](status.md), which must exist), attaches the archive and
   `SHA256SUMS`, and publishes it. It refuses to overwrite an existing release.

GitHub also attaches its own generated "Source code" archives to every release. Those are not covered by
`SHA256SUMS`, and their bytes aren't guaranteed to stay the same; `install.sh` and the notes use `omc-<version>.tar.gz`.

A failed job leaves no release, or at most a draft: fix it, delete the draft and the tag, and tag again.

To release after an update pass:

```sh
make version                 # e.g. 18.17.0, from src/core/core.h
build-tools/release_notes.sh 18.17.0 | less   # preview the notes
git tag -a v18.17.0 -m "omc v18.17.0"
git push origin main v18.17.0
```

`build-tools/ci.sh dist` (or just `make dist`) builds the same archive locally into `build/dist/`.

## Install script (`install.sh`)

`curl -fsSL https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.sh | sh` installs the latest release
from source: it checks that `make` and a C compiler exist, downloads `omc-<version>.tar.gz` and `SHA256SUMS`,
verifies the archive (a mismatch builds and installs nothing), builds it in a temporary directory with
`OMC_JOBS` jobs (default: every CPU), and renames the new binary over the old one so a running omc keeps working. A
failed build prints the end of the build log and which packages provide the compiler, OpenSSL and zlib headers on
common systems; the script never installs them itself. It is plain POSIX `sh`, and the whole body runs from `main()`,
so a download cut off halfway executes nothing. `OMC_VERSION`, `OMC_INSTALL_DIR`, `OMC_JOBS`, `OMC_REPO` and
`OMC_DOWNLOAD_URL` override the defaults. The latest release is found by following GitHub's `releases/latest`
redirect, so it needs no API token.

The raw URL serves `main`, so the script only works once the repository is public (a private repository's raw files
and release assets need authentication).

## Supply chain

The workflows use no third-party actions: the code is checked out with `git`, the token is passed as a one-off
header and never stored, and releases are published with the `gh` CLI that GitHub's runners ship with. Since a
release is only the committed source plus its checksum, nothing built on a CI runner ever reaches a user: what runs
on a user's machine is compiled there, from the archive they verified, with their own compiler, OpenSSL and zlib.

## Platforms and limits

- omc builds on Linux (glibc; checked on Ubuntu 22.04 with gcc 11 and newer) and macOS on Apple silicon with
  Homebrew's OpenSSL 3. It links the system's OpenSSL and zlib dynamically, so it gets their security updates.
- **`ubuntu-22.04-arm`** runners are free only for public repositories. For a private repository, remove the
  `linux-arm64` entry from `ci.yml`, or the job waits for a runner that never comes.
- **macOS minutes** cost about ten times Linux minutes on private repositories.
