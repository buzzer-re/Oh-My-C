#!/usr/bin/env bash
# CI and release steps, the same on a GitHub runner and locally (.github/workflows/*.yml call this).
#
#   build-tools/ci.sh check-deps   the C compiler, OpenSSL and zlib are present (installs nothing, by design)
#   build-tools/ci.sh build        build with zero warnings (a warning fails, as AGENTS.md requires)
#   build-tools/ci.sh test         Linux: make test (unit + e2e); macOS: make test-unit
#   build-tools/ci.sh smoke        the binary runs, reports its version and links only what it may
#   build-tools/ci.sh dist         release build into build/dist/omc-<version>-<os>-<arch>.tar.gz + .sha256
#   build-tools/ci.sh tag-check T  the release tag T is v<OMC_VERSION>
#
# Nothing here downloads or installs anything: no package managers, no third-party code. A runner that lacks a
# dependency fails with a message instead.
set -euo pipefail
cd "$(dirname "$0")/.."

OS=$(uname -s)
JOBS=${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)}
VERSION=$(make -s version 2>/dev/null)
STATIC=()

fail() {
    echo "ci: $*" >&2
    exit 1
}

openssl_prefix() {
    brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null || true
}

if [ "$OS" = Darwin ]; then
    # Link against Homebrew's OpenSSL statically and target the runner's macOS release, the oldest one Homebrew's
    # static libraries were built for (a lower target makes the Apple linker warn about every object file).
    export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(sw_vers -productVersion | cut -d. -f1).0}"
    STATIC=(STATIC_OPENSSL=1)
fi

check_deps() {
    command -v cc >/dev/null || fail "no C compiler (cc)"
    command -v python3 >/dev/null || fail "no python3 (the test mocks need it)"
    if [ "$OS" = Darwin ]; then
        local p
        p=$(openssl_prefix)
        [ -n "$p" ] && [ -f "$p/lib/libssl.a" ] && [ -f "$p/lib/libcrypto.a" ] ||
            fail "Homebrew OpenSSL 3 with static libraries not found (expected on GitHub's macOS runners)"
        echo "ci: OpenSSL from $p, macOS target $MACOSX_DEPLOYMENT_TARGET"
    else
        printf '#include <openssl/ssl.h>\n#include <zlib.h>\nint main(void){return 0;}\n' |
            cc -x c - -o /dev/null -lssl -lcrypto -lz 2>/dev/null || fail "OpenSSL or zlib development files missing"
        echo "ci: OpenSSL $(openssl version 2>/dev/null | cut -d' ' -f2), glibc $(ldd --version | head -1 | awk '{print $NF}')"
    fi
    echo "ci: omc $VERSION, $(cc --version | head -1)"
}

build() {
    mkdir -p build
    make -j"$JOBS" "${STATIC[@]}" 2>&1 | tee build/ci-build.log
    local n
    n=$(grep 'warning:' build/ci-build.log | grep -vc '^make' || true) # make's own clock-skew notices aren't the compiler's
    [ "$n" = 0 ] || { grep -A3 'warning:' build/ci-build.log | grep -v '^make'; fail "$n compiler warning(s); the build must have none"; }
}

run_tests() {
    if [ "$OS" = Darwin ]; then
        make -j"$JOBS" "${STATIC[@]}" test-unit
    else
        make -j"$JOBS" test
    fi
}

smoke() {
    local bin=build/omc out
    out=$("$bin" --version)
    [ "$out" = "omc/$VERSION" ] || fail "--version printed '$out', expected 'omc/$VERSION'"
    "$bin" --help | head -1 | grep -qF "v$VERSION (OMP equivalent)" || fail "--help does not show the version label"
    if [ "$OS" = Darwin ]; then
        out=$(otool -L "$bin" | tail -n +2 | awk '{print $1}')
        echo "$out"
        if echo "$out" | grep -vqE '^(/usr/lib/|/System/Library/)'; then
            fail "links a library outside the system (a release must not need Homebrew)"
        fi
    else
        out=$(ldd "$bin" | awk '{print $1}')
        echo "$out"
        if echo "$out" | grep -vqE '^(linux-vdso\.so|/lib|lib(ssl|crypto|z|m|c|pthread|dl|zstd)\.so|ld-linux)'; then
            fail "links a library outside libc, OpenSSL, zlib, pthread and libm"
        fi
    fi
    echo "ci: smoke ok ($out)" | head -1
}

dist() {
    make "${STATIC[@]}" dist
    ls -l build/dist/*.tar.gz build/dist/*.sha256
}

tag_check() {
    local tag=${1:?tag}
    [ "$tag" = "v$VERSION" ] || fail "tag $tag does not match OMC_VERSION $VERSION (src/core/core.h): tag v$VERSION"
    echo "ci: tag $tag matches"
}

case "${1:-}" in
check-deps) check_deps ;;
build) build ;;
test) run_tests ;;
smoke) smoke ;;
dist) dist ;;
tag-check) tag_check "${2:-}" ;;
*) fail "usage: $0 check-deps|build|test|smoke|dist|tag-check <tag>" ;;
esac
