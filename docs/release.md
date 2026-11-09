# CI and releases

## Versions

omc's version is the omp release it matches: `OMC_VERSION` in `src/core/core.h`, which equals the upstream pin in
[UPDATE.md](../UPDATE.md). `omc --version` prints `omc/<version>`; the welcome banner and `omc --help` show
`v<version> (OMP equivalent)`. When an update pass moves the pin, it moves `OMC_VERSION` with it.

## CI (`.github/workflows/ci.yml`)

Every push to `main` and every pull request builds and tests on Linux x86_64 (`ubuntu-22.04`), Linux arm64
(`ubuntu-22.04-arm`) and macOS arm64 (`macos-14`):

1. `build-tools/ci.sh check-deps`: a C compiler, `python3`, OpenSSL 3 and zlib are present. Nothing is installed.
2. `build-tools/ci.sh build`: a single compiler warning fails the build.
3. `build-tools/ci.sh smoke`: `--version` and `--help` show the version, and the binary links nothing beyond libc,
   OpenSSL, zlib, pthread and libm (on macOS: nothing outside `/usr/lib` and `/System`).
4. `build-tools/ci.sh test`: `make test` on Linux; `make test-unit` on macOS, where the e2e suite hasn't been
   verified yet.

`build-tools/ci.sh` runs the same way locally (`JOBS=3 build-tools/ci.sh build`).

## Releases (`.github/workflows/release.yml`)

Pushing a tag `v<OMC_VERSION>` (for example `v18.17.0`) releases it:

1. `prepare` checks the tag against `OMC_VERSION` and creates a **draft** GitHub release, with notes from
   `build-tools/release_notes.sh <version>`: install steps plus the `## omp v<version> update` section of
   [status.md](status.md), which must exist. It refuses to overwrite an existing release.
2. `build` runs the CI steps on each platform, then `make dist` (macOS links OpenSSL statically, so the binary needs
   no Homebrew), and uploads `omc-<version>-<os>-<arch>.tar.gz` with its `.sha256`.
3. `publish` collects `SHA256SUMS`, uploads it, and publishes the release only after every platform has uploaded.

A failed platform leaves the release as a draft: fix, delete the draft and the tag, and tag again.

To release after an update pass:

```sh
make version                 # e.g. 18.17.0, from src/core/core.h
build-tools/release_notes.sh 18.17.0 | less   # preview the notes
git tag -a v18.17.0 -m "omc v18.17.0"
git push origin main v18.17.0
```

`make dist` builds the same archive locally into `build/dist/`.

## Supply chain

The workflows use no third-party actions: the code is checked out with `git`, the token is passed as a one-off
header and never stored, and releases are published with the `gh` CLI that GitHub's runners ship with. The build
uses only the runner's compiler, OpenSSL and zlib, so a tampered package registry or action can't reach a release.

## Platforms and limits

- **Linux binaries** link the system's OpenSSL 3 and zlib dynamically, so they get the distribution's security updates.
  Built on Ubuntu 22.04, they need glibc 2.35+ and OpenSSL 3.
- **The macOS binary** targets macOS 14+ on Apple silicon. It isn't signed or notarized: a copy downloaded with a
  browser needs `xattr -d com.apple.quarantine omc` once. There is no Intel macOS build.
- **`ubuntu-22.04-arm`** runners are free only for public repositories. For a private repository, remove the
  `linux-arm64` entries from both workflows, or the job waits for a runner that never comes.
- **macOS minutes** cost about ten times Linux minutes on private repositories.
