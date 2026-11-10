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

Releases ship source only: omc is built on your machine. The installer downloads \`omc-$V.tar.gz\`, checks it against
\`SHA256SUMS\`, builds it and installs \`omc\` into \`~/.local/bin\`:

\`\`\`sh
curl -fsSL https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.sh | OMC_VERSION=$V sh
\`\`\`

Or by hand:

\`\`\`sh
sha256sum -c SHA256SUMS      # on macOS: shasum -a 256 -c SHA256SUMS
tar -xzf omc-$V.tar.gz && cd omc-$V
make && make install         # -> ~/.local/bin/omc
omc --version                # omc/$V
\`\`\`

It needs a C11 compiler, make, and the OpenSSL 3 and zlib development files (Debian/Ubuntu: \`build-essential
libssl-dev zlib1g-dev\`; Fedora: \`gcc make openssl-devel zlib-devel\`; macOS: the Xcode command line tools and
Homebrew's \`openssl@3\`). Use \`omc-$V.tar.gz\`, which \`SHA256SUMS\` covers, rather than GitHub's generated
"Source code" archives.

## What changed

$SECTION
EOF
