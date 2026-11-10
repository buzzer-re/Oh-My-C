#!/bin/sh
# omc installer: downloads a release's source, checks it against the release's SHA256SUMS, builds it on this machine
# and installs `omc`. Releases ship no binaries.
#
#   curl -fsSL https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.sh | sh
#
# Plain POSIX sh: piping it into sh or bash works the same. It needs a C compiler, make, and the OpenSSL 3 and zlib
# development files; it installs none of them.
#
# Environment:
#   OMC_VERSION       release to install, e.g. 18.17.0 (default: the latest release)
#   OMC_INSTALL_DIR   where `omc` goes (default: ~/.local/bin)
#   OMC_JOBS          parallel build jobs (default: the number of CPUs)
#   OMC_REPO          GitHub repository (default: buzzer-re/Oh-My-C)
#   OMC_DOWNLOAD_URL  base URL holding <tag>/<asset> (default: https://github.com/$OMC_REPO/releases/download)
#
# Everything runs inside main(), so a download cut off halfway executes nothing.

main() {
    set -eu
    repo=${OMC_REPO:-buzzer-re/Oh-My-C}
    base=${OMC_DOWNLOAD_URL:-https://github.com/$repo/releases/download}
    dest=${OMC_INSTALL_DIR:-$HOME/.local/bin}

    if command -v curl >/dev/null 2>&1; then
        fetch() { curl -fsSL --proto '=https,file' --tlsv1.2 -o "$2" "$1"; }
        latest() { curl -fsSLI --proto '=https' --tlsv1.2 -o /dev/null -w '%{url_effective}' "https://github.com/$repo/releases/latest"; }
    elif command -v wget >/dev/null 2>&1; then
        fetch() { wget -q --https-only -O "$2" "$1"; }
        latest() { wget -q --https-only --max-redirect=5 -S -O /dev/null "https://github.com/$repo/releases/latest" 2>&1 | sed -n 's/^ *[Ll]ocation: *//p' | tail -1; }
    else
        die "neither curl nor wget is installed"
    fi

    case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) die "omc builds on Linux and macOS only" ;;
    esac
    need_build_tools "$os"

    if [ -n "${OMC_VERSION:-}" ]; then
        version=${OMC_VERSION#v}
    else
        url=$(latest) || die "could not find the latest release of $repo"
        version=${url##*/tag/v}
        case "$version" in
        "" | */* | *" "*) die "could not find the latest release of $repo (is it published?)" ;;
        esac
    fi
    tag=v$version
    name=omc-$version
    say "building omc $version from source and installing it into $dest"

    tmp=$(mktemp -d 2>/dev/null || mktemp -d -t omc)
    trap 'rm -rf "$tmp"' EXIT INT TERM
    fetch "$base/$tag/$name.tar.gz" "$tmp/$name.tar.gz" || die "download failed: $base/$tag/$name.tar.gz"
    fetch "$base/$tag/SHA256SUMS" "$tmp/SHA256SUMS" || die "download failed: $base/$tag/SHA256SUMS"

    want=$(awk -v f="$name.tar.gz" '$2 == f || $2 == "*" f { print $1 }' "$tmp/SHA256SUMS")
    [ -n "$want" ] || die "SHA256SUMS has no entry for $name.tar.gz"
    if command -v sha256sum >/dev/null 2>&1; then
        got=$(sha256sum "$tmp/$name.tar.gz" | awk '{print $1}')
    elif command -v shasum >/dev/null 2>&1; then
        got=$(shasum -a 256 "$tmp/$name.tar.gz" | awk '{print $1}')
    else
        die "need sha256sum or shasum to verify the download"
    fi
    [ "$got" = "$want" ] || die "checksum mismatch for $name.tar.gz (expected $want, got $got); nothing was built or installed"
    say "checksum ok"

    tar -xzf "$tmp/$name.tar.gz" -C "$tmp"
    [ -f "$tmp/$name/Makefile" ] || die "the archive has no $name/Makefile"
    jobs=${OMC_JOBS:-$( (getconf _NPROCESSORS_ONLN || nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)}
    say "building with $jobs job(s); this takes a minute or two"
    if ! make -C "$tmp/$name" -j"$jobs" >"$tmp/build.log" 2>&1; then
        tail -25 "$tmp/build.log" >&2
        die "the build failed (log above); $(deps_hint "$os")"
    fi
    "$tmp/$name/build/omc" --version >/dev/null 2>&1 || die "the freshly built omc does not run"

    mkdir -p "$dest"
    # install next to the old binary, then rename over it, so a running omc keeps working
    cp "$tmp/$name/build/omc" "$dest/.omc.new.$$"
    chmod 755 "$dest/.omc.new.$$"
    mv -f "$dest/.omc.new.$$" "$dest/omc"
    say "installed $("$dest/omc" --version 2>/dev/null || echo "omc $version") to $dest/omc"

    case ":$PATH:" in
    *":$dest:"*) ;;
    *) say "$dest is not on your PATH; add this to your shell profile: export PATH=\"$dest:\$PATH\"" ;;
    esac
}

# the compiler and make must exist before anything is downloaded; the OpenSSL and zlib headers show up in the build
need_build_tools() {
    command -v make >/dev/null 2>&1 || die "make is not installed; $(deps_hint "$1")"
    command -v cc >/dev/null 2>&1 || die "no C compiler (cc) is installed; $(deps_hint "$1")"
}

deps_hint() {
    case "$1" in
    macos) echo "omc needs the Xcode command line tools (xcode-select --install) and OpenSSL 3 from Homebrew (brew install openssl@3)" ;;
    *) echo "omc needs a C compiler, make, and the OpenSSL 3 and zlib headers (Debian/Ubuntu: build-essential libssl-dev zlib1g-dev; Fedora: gcc make openssl-devel zlib-devel; Arch: base-devel openssl zlib)" ;;
    esac
}

say() { printf 'omc-install: %s\n' "$*"; }
die() {
    printf 'omc-install: error: %s\n' "$*" >&2
    exit 1
}

main "$@"
