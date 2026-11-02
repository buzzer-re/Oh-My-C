#!/usr/bin/env bash
# Extract omp's /settings panel definitions (tabs, groups, labels, descriptions, options, defaults,
# conditions) into assets/tui/settings-ui.json.
#
# Usage: build-tools/extract_settings.sh <omp checkout matching `omp --version`> > assets/tui/settings-ui.json
#        build-tools/extract_settings.sh <omp checkout> --registry > assets/config/settings-registry.json
#        (--registry: every registered setting with type/default/values/env/credential, for cfg://)
#
# Runs the upstream TypeScript registry with the bun runtime embedded in the installed omp binary
# (BUN_BE_BUN=1): workspace packages are symlinked into <checkout>/node_modules, npm dependencies are
# replaced by inert ESM stubs (exports added on demand), and omp's extracted native addon
# (~/.omp/natives/<version>) is linked in. HOME/PI_CODING_AGENT_DIR point at throwaway dirs so the
# dump never reads the user's config (defaults only). Dev-time only.
set -eu
UP=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)/settings-dump
SCRIPT=dump.ts
[ "${2:-}" = --registry ] && SCRIPT=registry.ts
NM=$UP/node_modules
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$NM/@oh-my-pi" "$TMP/home" "$TMP/agent" "$NM/.stubs"
for p in "$UP"/packages/*/; do
    name=$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['name'])" "$p/package.json" 2>/dev/null) || continue
    [ -e "$NM/$name" ] || ln -s "${p%/}" "$NM/$name"
done
ver=$(omp --version | sed 's|.*/||')
for f in "$HOME"/.omp/natives/"$ver"/*.node; do
    [ -e "$f" ] && ln -sf "$f" "$UP/packages/natives/native/$(basename "$f")"
done
for _ in $(seq 100); do
    rc=0
    env -i PATH="$PATH" HOME="$TMP/home" PI_CODING_AGENT_DIR="$TMP/agent" UPSTREAM_PACKAGES="$UP/packages" BUN_BE_BUN=1 \
        omp "$HERE/$SCRIPT" > "$TMP/out.json" 2> "$TMP/err.txt" || rc=$?
    [ $rc = 0 ] && { cat "$TMP/out.json"; exit 0; }
    exp=$(grep -o "export '[^']*' not found in '[^']*'" "$TMP/err.txt" | head -1 || true)
    if [ -n "$exp" ]; then
        name=$(echo "$exp" | sed "s/export '\([^']*\)'.*/\1/"); mod=$(echo "$exp" | sed "s/.* in '\(.*\)'/\1/")
        for ff in $(cat "$NM/.stubs/$mod"); do echo "export const $name = p;" >> "$ff"; done
        continue
    fi
    exp=$(grep -o "Export named '[^']*' not found in module '[^']*'" "$TMP/err.txt" | head -1 || true)
    if [ -n "$exp" ]; then
        name=$(echo "$exp" | sed "s/Export named '\([^']*\)'.*/\1/"); f=$(echo "$exp" | sed "s/.* in module '\(.*\)'/\1/")
        case "$f" in "$NM"/*) echo "export const $name = p;" >> "$f"; continue;; esac
    fi
    miss=$(grep -o "Cannot find \(module\|package\) '[^']*'" "$TMP/err.txt" | head -1 | sed "s/.*'\(.*\)'/\1/" || true)
    case "$miss" in ""|@oh-my-pi/*|.*|/*) cat "$TMP/err.txt" >&2; exit 1;; esac
    case "$miss" in @*/*) pkg=$(echo "$miss" | cut -d/ -f1-2);; *) pkg=$(echo "$miss" | cut -d/ -f1);; esac
    mkdir -p "$NM/$pkg" "$(dirname "$NM/.stubs/$miss")"
    printf '{"name":"%s","type":"module","main":"index.js"}\n' "$pkg" > "$NM/$pkg/package.json"
    sub=${miss#"$pkg"}
    if [ -n "$sub" ]; then
        mkdir -p "$NM/$pkg$sub"
        cp "$HERE/stub.js" "$NM/$pkg$sub/index.js"; cp "$HERE/stub.js" "$NM/$pkg$sub.js"
        echo "$NM/$pkg$sub/index.js $NM/$pkg$sub.js" > "$NM/.stubs/$miss"
    else
        cp "$HERE/stub.js" "$NM/$pkg/index.js"; echo "$NM/$pkg/index.js" > "$NM/.stubs/$miss"
    fi
done
echo "gave up resolving dependencies" >&2
exit 1
