#!/bin/sh
# check_apple.sh - compile the macOS-only branches (#ifdef __APPLE__) of every source file on a Linux host,
# syntax-only, so a missing include or an unused variable in code the Linux build never sees fails here
# instead of on a Mac. compat.h / compat_sys.h are skipped (their Darwin renames don't fit glibc's headers);
# errors that only come from that (st_*timespec) or from Darwin-only headers are filtered out.
# Usage: build-tools/check_apple.sh   (exit 1 on any finding)
cd "$(dirname "$0")/.." || exit 2
CC=${CC:-cc}
rc=0
for f in $(grep -rl '__APPLE__' src --include='*.c'); do
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
[ $rc = 0 ] && echo "check_apple: macOS branches OK"
exit $rc
