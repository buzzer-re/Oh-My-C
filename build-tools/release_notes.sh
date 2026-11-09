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

Download the archive for your system, check it against \`SHA256SUMS\`, and put \`omc\` on your PATH:

\`\`\`sh
tar -xzf omc-$V-<os>-<arch>.tar.gz
install -m755 omc-$V-<os>-<arch>/omc ~/.local/bin/omc
omc --version   # omc/$V
\`\`\`

| Archive | Runs on |
|---|---|
| \`omc-$V-linux-x86_64.tar.gz\` | Linux x86_64 with glibc 2.35+ and OpenSSL 3 (Ubuntu 22.04+, Debian 12+, Fedora 36+) |
| \`omc-$V-linux-arm64.tar.gz\` | Linux arm64, same requirements |
| \`omc-$V-macos-arm64.tar.gz\` | macOS 14+ on Apple silicon (OpenSSL is built in) |

The macOS binary is not notarized: a copy downloaded with a browser needs \`xattr -d com.apple.quarantine omc\` once.
Building from source works anywhere with a C11 compiler and the OpenSSL and zlib headers (\`make && make install\`).

## What changed

$SECTION
EOF
