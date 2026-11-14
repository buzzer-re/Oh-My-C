#!/bin/sh
# Release notes for omc v<version>: what this build matches, how to install it, and the matching update section of
# docs/status.md. Used by .github/workflows/release.yml; run it locally to preview.
# Usage: build-tools/release_notes.sh <version>
set -eu
cd "$(dirname "$0")/.."
V=${1:?usage: release_notes.sh <version>}
SECTION=$(awk -v h="## omp v$V update" '
    index($0, h) == 1 { on = 1; next }
    on && /^## / { exit }
    on { print }' docs/status.md)
[ -n "$SECTION" ] || { echo "release_notes: docs/status.md has no '## omp v$V update' section" >&2; exit 1; }

cat <<EOF
omc v$V is the oh-my-c build that matches [oh-my-pi](https://github.com/can1357/oh-my-pi) v$V: the same TUI,
prompts, tools, sessions and config, rewritten in C with no Node or Bun.

## Install

The installer picks the prebuilt binary for your system, checks it against \`SHA256SUMS\` and installs \`omc\` into
\`~/.local/bin\`:

\`\`\`sh
curl -fsSL https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.sh | OMC_VERSION=$V sh
\`\`\`

| Archive | Runs on |
|---|---|
| \`omc-$V-linux-x86_64.tar.gz\` | Linux x86_64 with glibc 2.35+ and OpenSSL 3 (Ubuntu 22.04+, Debian 12+, Fedora 36+) |
| \`omc-$V-linux-arm64.tar.gz\` | Linux arm64, same requirements |
| \`omc-$V-macos-arm64.tar.gz\` | macOS on Apple silicon (OpenSSL is built in) |
| \`omc-$V.tar.gz\` | the source, for any other system |

To build from source instead, add \`--build\` (\`... | sh -s -- --build\`, or \`./install.sh --build\`). That needs a C11
compiler, make, and the OpenSSL 3 and zlib development files (Debian/Ubuntu: \`build-essential libssl-dev zlib1g-dev\`;
Fedora: \`gcc make openssl-devel zlib-devel\`; macOS: the Xcode command line tools and Homebrew's \`openssl@3\`). By hand:

\`\`\`sh
sha256sum -c SHA256SUMS --ignore-missing      # on macOS: shasum -a 256 -c SHA256SUMS --ignore-missing
tar -xzf omc-$V-linux-x86_64.tar.gz && install -m755 omc-$V-linux-x86_64/omc ~/.local/bin/omc
# or from source: tar -xzf omc-$V.tar.gz && cd omc-$V && make && make install
omc --version                                 # omc/$V
\`\`\`

The macOS binary is not notarized: a copy downloaded with a browser needs \`xattr -d com.apple.quarantine omc\` once.

## What changed

$SECTION
EOF
