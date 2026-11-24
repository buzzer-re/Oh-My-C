#!/bin/sh
# check_apple.sh - compile the macOS-only branches (#ifdef __APPLE__) of every source file on a Linux host,
# syntax-only, so a missing include or an unused variable in code the Linux build never sees fails here
# instead of on a Mac. compat.h / compat_sys.h are skipped (their Darwin renames don't fit glibc's headers);
# errors that only come from that (st_*timespec) or from Darwin-only headers are filtered out.
# Usage: build-tools/check_apple.sh   (exit 1 on any finding)
cd "$(dirname "$0")/.." || exit 2
CC=${CC:-cc}
rc=0
for f in $(grep -rl '__APPLE__' src --include='*.c' | grep -v '^src/plat/win/'); do # plat/win: Windows-only
    out=$($CC -std=gnu11 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
        -Werror=implicit-function-declaration -D__APPLE__ -DOMC_COMPAT_H -DOMC_COMPAT_SYS_H -Isrc -Iinclude "$f" 2>&1 |
        grep -E 'error|warning' |
        grep -vE "st_[amc]timespec|'util\.h'|util\.h: No such|libproc\.h|mach/|CoreFoundation|sys/sysctl\.h: No such")
    if [ -n "$out" ]; then
        echo "== $f"
        echo "$out"
        rc=1
    fi
done
# glibc-only calls: glibc's headers declare them, so the syntax pass above can't see that macOS lacks them. Each file is
# preprocessed as macOS sees it (__APPLE__ defined, the real compat shims applied, which rename the calls they cover),
# and any of these names still called from the file's own code is a call that would not link on a Mac.
# empty stand-ins for the Darwin-only headers, so files that include them still preprocess on Linux
STUBS=$(mktemp -d)
trap 'rm -rf "$STUBS"' EXIT
mkdir -p "$STUBS/mach" "$STUBS/CoreFoundation" "$STUBS/sys"
for h in util.h libproc.h mach/mach.h mach/mach_time.h CoreFoundation/CoreFoundation.h sys/sysctl.h; do : > "$STUBS/$h"; done
GLIBC_ONLY='memrchr|pipe2|getrandom|execvpe|strchrnul|mempcpy|rawmemchr|accept4|dup3|get_current_dir_name|strverscmp|secure_getenv|explicit_bzero|copy_file_range|memfd_create|epoll_create1|inotify_init1|signalfd|eventfd|gettid'
for f in $(grep -rlwE "$GLIBC_ONLY" src --include='*.c' | grep -v '^src/plat/win/'); do
    pp=$($CC -std=gnu11 -E -D__APPLE__ -Isrc -Iinclude -idirafter "$STUBS" "$f" 2>/dev/null) || { echo "== $f: preprocessing failed"; rc=1; continue; }
    hits=$(printf '%s\n' "$pp" |
        awk -v want="\"$f\"" -v re="(^|[^A-Za-z0-9_])($GLIBC_ONLY)[ \t]*[(]" '
            /^# [0-9]+ "/ { cur = $3; next }
            cur == want && $0 ~ re { match($0, re); print substr($0, RSTART, RLENGTH) }') ||
        { echo "== $f: awk failed"; rc=1; continue; }
    hits=$(printf '%s' "$hits" | tr -d '( \t' | sed 's/^[^A-Za-z_]*//' | sort -u | tr '\n' ' ')
    if [ -n "$hits" ]; then
        echo "== $f: calls $hits- not on macOS (add a shim to core/compat_sys.h and include it)"
        rc=1
    fi
done
# glibc before 2.38 (Ubuntu 22.04, the release baseline) declares these only with _GNU_SOURCE, so a file calling them
# must define it before its first #include; a newer glibc hides the omission, which then only fails on older systems
for f in $(grep -rlwE 'memmem|strcasestr|vasprintf|asprintf|memrchr|pipe2|execvpe|getrandom' src --include='*.c' | grep -v '^src/plat/win/'); do
    calls=$(sed -e 's#/\*.*\*/##g' -e 's#//.*##' "$f" | grep -cE '(^|[^A-Za-z0-9_])(memmem|strcasestr|vasprintf|asprintf|memrchr|pipe2|execvpe|getrandom)[ \t]*[(]' || true)
    [ "$calls" -gt 0 ] || continue
    def=$(grep -n '^#define _GNU_SOURCE' "$f" | head -1 | cut -d: -f1)
    inc=$(grep -n '^#include' "$f" | head -1 | cut -d: -f1)
    if [ -z "$def" ] || { [ -n "$inc" ] && [ "$def" -gt "$inc" ]; }; then
        echo "== $f: calls a glibc extension without '#define _GNU_SOURCE' before its first #include"
        rc=1
    fi
done
[ $rc = 0 ] && echo "check_apple: macOS branches OK"
exit $rc
