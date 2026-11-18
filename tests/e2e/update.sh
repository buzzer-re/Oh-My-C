# Sourced by tests/e2e/run.sh: `omc update`, the startup update notice and update.auto against tests/e2e/mock_release.py
# (omc's GitHub releases, served locally: the real GitHub is never contacted). Each run updates a throwaway copy of the
# binary under $T/upd, never $B itself. omp references: cli/update-cli.ts, main.ts checkForNewVersion,
# modes/utils/ui-helpers.ts showNewVersionNotification, cli/auto-update.ts.
UD="$T/upd"
mkdir -p "$UD/bin" "$UD/home/.omc/agent" "$UD/rel" "$UD/work"
cp "$T/agent/models.yml" "$UD/home/.omc/agent/models.yml"
case "$(uname -s)-$(uname -m)" in
Linux-x86_64 | Linux-amd64) UPLAT=linux-x86_64 ;;
Linux-aarch64 | Linux-arm64) UPLAT=linux-arm64 ;;
Darwin-arm64) UPLAT=macos-arm64 ;;
*) UPLAT= ;;
esac
CURV=$("$B" --version | sed 's|^omc/||')
python3 -I "$ROOT/tests/e2e/mock_release.py" make "$UD/rel" 99.0.0 "${UPLAT:-none}"
UPORT=$((PORT + 9)); python3 -I "$ROOT/tests/e2e/mock_release.py" serve $UPORT "$UD/rel" & UPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$UPORT) 2>/dev/null && break; sleep 0.1; done
UENV=(env HOME="$UD/home" OMC_CODING_AGENT_DIR="$UD/home/.omc/agent"
      OMC_LATEST_URL="http://127.0.0.1:$UPORT/buzzer-re/Oh-My-C/releases/latest" OMC_DOWNLOAD_URL="http://127.0.0.1:$UPORT/download")
ucopy() { rm -f "$UD/bin/omc"; cp "$B" "$UD/bin/omc"; }
ucopy
echo "$CURV" > "$UD/rel/LATEST"
check "update check current"  $'Current version: '"$CURV"$'\n\xe2\x9c\x94 Already up to date'  "${UENV[@]}" "$UD/bin/omc" update --check
echo 99.0.0 > "$UD/rel/LATEST"
check "update check newer"    $'Current version: '"$CURV"$'\nNew version available: 99.0.0'  "${UENV[@]}" "$UD/bin/omc" update --check
check "update check only"     "omc/$CURV"  "$UD/bin/omc" --version
check "update unknown flag"   "error: Unknown option '--canary'"  "${UENV[@]}" "$UD/bin/omc" update --canary
if [ -n "$UPLAT" ]; then
    cp "$UD/rel/v99.0.0/SHA256SUMS" "$UD/sums"
    python3 -I -c 'import sys; p=sys.argv[1]; l=open(p).read().split("  ",1); open(p,"w").write("0"*64+"  "+l[1])' "$UD/rel/v99.0.0/SHA256SUMS"
    check "update checksum mismatch" "checksum mismatch for omc-99.0.0-$UPLAT.tar.gz (expected 0000000000000000000000000000000000000000000000000000000000000000, got "  "${UENV[@]}" "$UD/bin/omc" update
    check "update mismatch untouched" "same"  bash -c "cmp -s '$B' '$UD/bin/omc' && [ \$(ls -A '$UD/bin' | wc -l) = 1 ] && echo same"
    printf 'abc  other.tar.gz\n' > "$UD/rel/v99.0.0/SHA256SUMS"
    check "update sums no entry"  "Update failed: SHA256SUMS has no entry for omc-99.0.0-$UPLAT.tar.gz; nothing was installed"  "${UENV[@]}" "$UD/bin/omc" update
    cp "$UD/sums" "$UD/rel/v99.0.0/SHA256SUMS"
    mv "$UD/rel/v99.0.0/omc-99.0.0-$UPLAT.tar.gz" "$UD/asset"
    check "update missing asset"  "Update failed: download failed: http://127.0.0.1:$UPORT/download/v99.0.0/omc-99.0.0-$UPLAT.tar.gz (HTTP 404)"  "${UENV[@]}" "$UD/bin/omc" update
    mv "$UD/asset" "$UD/rel/v99.0.0/omc-99.0.0-$UPLAT.tar.gz"
    check "update missing untouched" "same"  bash -c "cmp -s '$B' '$UD/bin/omc' && echo same"
    check "update installs"       $'Installing update...\n\n\xe2\x9c\x94 Updated to 99.0.0 at '"$(cd "$UD/bin" && pwd -P)/omc"$'\nRestart omc to use the new version'  "${UENV[@]}" "$UD/bin/omc" update
    check "update new binary"     "omc/99.0.0"  "$UD/bin/omc" --version
    check "update no leftovers"   "1"  bash -c "ls -A '$UD/bin' | wc -l | tr -d ' '"
    # OMC_UPDATE_TARGET: $B updates another binary, comparing against that binary's own version
    ucopy
    check "update target hook"    $'Current version: '"$CURV"$'\nNew version available: 99.0.0'  "${UENV[@]}" OMC_UPDATE_TARGET="$UD/bin/omc" "$B" update --check
    check "update target junk"    "refusing to replace $UD/sums: it does not report an omc version when run directly"  "${UENV[@]}" OMC_UPDATE_TARGET="$UD/sums" "$B" update
fi
: > "$UD/rel/LATEST"
check "update no release"     "Failed to check for updates: could not find the latest release of buzzer-re/Oh-My-C (is it published?)"  "${UENV[@]}" "$UD/bin/omc" update --check
check "update offline"        "Failed to check for updates: could not find the latest release of buzzer-re/Oh-My-C: connect to 127.0.0.1:1 failed"  "${UENV[@]}" OMC_LATEST_URL=http://127.0.0.1:1/x/releases/latest "$UD/bin/omc" update
echo 99.0.0 > "$UD/rel/LATEST"
# a checkout's build/omc updates through git
mkdir -p "$UD/src/src/core" "$UD/src/build" "$UD/src/.git"
touch "$UD/src/Makefile" "$UD/src/src/core/core.h"
cp "$B" "$UD/src/build/omc"
check "update source checkout" "$UD/src/build/omc is built from a source checkout; update it with \`git pull && make\` in that checkout."  "${UENV[@]}" "$UD/src/build/omc" update
check "update checkout untouched" "same"  bash -c "cmp -s '$B' '$UD/src/build/omc' && echo same"
# the startup notice and update.auto under a pty (OMC_NO_UPDATE_CHECK, which run.sh exports, lifted for these)
if [ -n "$UPLAT" ]; then
    check "update tui" "update tui ok"  env -u OMC_NO_UPDATE_CHECK "${UENV[@]}" python3 -I "$ROOT/tests/e2e/update_tui.py" "$B" "$UD"
fi
kill $UPID 2>/dev/null
