#!/usr/bin/env bash
# tests/fuzz/run.sh - run the libFuzzer harnesses built by `make fuzz` (clang-21, ASan + UBSan).
#
#   tests/fuzz/run.sh [-t seconds] [target ...]     default: every target, 60 s each
#
# One fuzzer at a time, RSS-capped (FUZZ_RSS_MB, default 1024), one job. The working corpus of each target is
# build/fuzz/corpus/<target> (seeded from tests/fuzz/seeds/<target>, made by tests/fuzz/mkseeds.py); crashes, leaks,
# timeouts and OOMs land in build/fuzz/crash/<target>/. A finding, once fixed, goes to tests/fuzz/regress/<target>/
# (replayed by `make test` through test_fuzz_regress). Runs never touch the real ~/.omc or ~/.omp: each harness points
# HOME and the agent dirs at a throwaway dir under $TMPDIR.
set -u
cd "$(dirname "$0")/../.."
secs=60
while getopts t: o; do case $o in t) secs=$OPTARG ;; *) exit 2 ;; esac; done
shift $((OPTIND - 1))
targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=($(ls tests/fuzz/fuzz_*.c | sed 's|.*/fuzz_||; s|\.c$||'))
export OMC_NO_CRASH_HANDLER=1
# a smaller quarantine keeps long runs under the RSS cap (the default 256 MiB read as false OOMs in glob/regex)
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:allocator_may_return_null=1:quarantine_size_mb=64}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
rc=0
for t in "${targets[@]}"; do
    bin=build/fuzz/fuzz_$t
    [ -x "$bin" ] || { echo "missing $bin (make fuzz)"; rc=1; continue; }
    corpus=build/fuzz/corpus/$t crash=build/fuzz/crash/$t
    mkdir -p "$corpus" "$crash"
    seeds=()
    [ -d tests/fuzz/seeds/$t ] && seeds+=(tests/fuzz/seeds/$t)
    echo "== $t (${secs}s)"
    "$bin" "$corpus" "${seeds[@]}" -max_total_time="$secs" -rss_limit_mb="${FUZZ_RSS_MB:-1024}" -timeout=20 \
        -max_len="${FUZZ_MAX_LEN:-65536}" -print_final_stats=1 -artifact_prefix="$crash/" \
        >"build/fuzz/$t.log" 2>&1
    r=$?
    grep -E '^(stat::number_of_executed_units|stat::peak_rss_mb|#[0-9]+.*DONE)' "build/fuzz/$t.log" | tr '\n' ' '
    echo
    if [ $r -ne 0 ]; then
        echo "   FINDING ($t): see build/fuzz/$t.log and $crash/"
        grep -m3 -E 'ERROR: |runtime error|fuzz property failed|SUMMARY' "build/fuzz/$t.log" | sed 's/^/   /'
        rc=1
    fi
done
exit $rc
