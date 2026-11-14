#!/bin/sh
# omc installer: downloads a release, checks it against the release's SHA256SUMS and installs `omc`.
#
#   curl -fsSL https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.sh | sh              # prebuilt binary
#   curl -fsSL https://raw.githubusercontent.com/buzzer-re/Oh-My-C/main/install.sh | sh -s -- --build   # from source
#   ./install.sh [--build]
#
# By default it installs the release's prebuilt binary for this OS and architecture (Linux x86_64 / arm64, macOS on
# Apple silicon). --build downloads the release's source archive instead and builds it here; that needs a C compiler,
# make, and the OpenSSL 3 and zlib development files, and installs none of them. Plain POSIX sh: piping it into sh or
# bash works the same.
#
# Environment:
#   OMC_VERSION       release to install, e.g. 18.18.7 (default: the latest release)
#   OMC_INSTALL_DIR   where `omc` goes (default: ~/.local/bin)
#   OMC_BUILD=1       the same as --build
#   OMC_JOBS          parallel build jobs for --build (default: the number of CPUs)
#   OMC_REPO          GitHub repository (default: buzzer-re/Oh-My-C)
#   OMC_DOWNLOAD_URL  base URL holding <tag>/<asset> (default: https://github.com/$OMC_REPO/releases/download)
#
# Everything runs inside main(), so a download cut off halfway executes nothing.

main() {
    set -eu
    build=${OMC_BUILD:-0}
    for arg in "$@"; do
        case "$arg" in
        --build) build=1 ;;
        -h | --help)
            sed -n '2,20p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//' || true
            return 0
            ;;
        *) die "unknown argument: $arg (use --build to build from source)" ;;
        esac
    done
    [ "$build" = 1 ] || [ "$build" = 0 ] || build=1

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
    *) die "omc runs on Linux and macOS only" ;;
    esac
    case "$(uname -m)" in
    x86_64 | amd64) arch=x86_64 ;;
    aarch64 | arm64) arch=arm64 ;;
    *) arch=$(uname -m) ;;
    esac
    if [ "$build" = 0 ]; then
        case "$os-$arch" in
        linux-x86_64 | linux-arm64 | macos-arm64) ;;
        *) die "no prebuilt omc for $os-$arch; build it from source with --build" ;;
        esac
    else
        need_build_tools "$os"
    fi

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
    if [ "$build" = 1 ]; then
        asset=omc-$version.tar.gz
        say "building omc $version from source and installing it into $dest"
    else
        asset=omc-$version-$os-$arch.tar.gz
        say "installing omc $version ($os-$arch) into $dest"
    fi

    tmp=$(mktemp -d 2>/dev/null || mktemp -d -t omc)
    trap 'rm -rf "$tmp"' EXIT INT TERM
    fetch "$base/$tag/$asset" "$tmp/$asset" || die "download failed: $base/$tag/$asset"
    fetch "$base/$tag/SHA256SUMS" "$tmp/SHA256SUMS" || die "download failed: $base/$tag/SHA256SUMS"

    want=$(awk -v f="$asset" '$2 == f || $2 == "*" f { print $1 }' "$tmp/SHA256SUMS")
    [ -n "$want" ] || die "SHA256SUMS has no entry for $asset"
    if command -v sha256sum >/dev/null 2>&1; then
        got=$(sha256sum "$tmp/$asset" | awk '{print $1}')
    elif command -v shasum >/dev/null 2>&1; then
        got=$(shasum -a 256 "$tmp/$asset" | awk '{print $1}')
    else
        die "need sha256sum or shasum to verify the download"
    fi
    [ "$got" = "$want" ] || die "checksum mismatch for $asset (expected $want, got $got); nothing was installed"
    say "checksum ok"

    tar -xzf "$tmp/$asset" -C "$tmp"
    if [ "$build" = 1 ]; then
        src=$tmp/omc-$version
        [ -f "$src/Makefile" ] || die "the archive has no omc-$version/Makefile"
        jobs=${OMC_JOBS:-$( (getconf _NPROCESSORS_ONLN || nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)}
        say "building with $jobs job(s); this takes a minute or two"
        if ! make -C "$src" -j"$jobs" >"$tmp/build.log" 2>&1; then
            tail -25 "$tmp/build.log" >&2
            die "the build failed (log above); $(deps_hint "$os")"
        fi
        bin=$src/build/omc
    else
        bin=$tmp/omc-$version-$os-$arch/omc
        [ -f "$bin" ] || die "the archive has no omc binary"
        if [ "$os" = linux ] && command -v ldd >/dev/null 2>&1 && ldd "$bin" 2>/dev/null | grep -q 'not found'; then
            ldd "$bin" | grep 'not found' >&2
            die "a library omc needs is missing (it needs glibc 2.35+ and OpenSSL 3, e.g. the libssl3 package), or build it here with --build"
        fi
    fi
    "$bin" --version >/dev/null 2>&1 || die "omc does not run on this system; try --build"

    mkdir -p "$dest"
    # install next to the old binary, then rename over it, so a running omc keeps working
    cp "$bin" "$dest/.omc.new.$$"
    chmod 755 "$dest/.omc.new.$$"
    mv -f "$dest/.omc.new.$$" "$dest/omc"
    say "installed $("$dest/omc" --version 2>/dev/null || echo "omc $version") to $dest/omc"

    case ":$PATH:" in
    *":$dest:"*) ;;
    *) say "$dest is not on your PATH; add this to your shell profile: export PATH=\"$dest:\$PATH\"" ;;
    esac
}

# --build: the compiler and make must exist before anything is downloaded; the OpenSSL and zlib headers show up in the build
need_build_tools() {
    command -v make >/dev/null 2>&1 || die "make is not installed; $(deps_hint "$1")"
    command -v cc >/dev/null 2>&1 || die "no C compiler (cc) is installed; $(deps_hint "$1")"
}

deps_hint() {
    case "$1" in
    macos) echo "building omc needs the Xcode command line tools (xcode-select --install) and OpenSSL 3 from Homebrew (brew install openssl@3)" ;;
    *) echo "building omc needs a C compiler, make, and the OpenSSL 3 and zlib headers (Debian/Ubuntu: build-essential libssl-dev zlib1g-dev; Fedora: gcc make openssl-devel zlib-devel; Arch: base-devel openssl zlib)" ;;
    esac
}

say() { printf 'omc-install: %s\n' "$*"; }
die() {
    printf 'omc-install: error: %s\n' "$*" >&2
    exit 1
}

main "$@"
