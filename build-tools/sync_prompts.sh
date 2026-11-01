#!/usr/bin/env bash
# Dev-time only: sync the verbatim upstream prompt templates embedded under prompts/ from an omp checkout.
#   build-tools/sync_prompts.sh <omp checkout> [--apply]
# Layout: prompts/<package>/<path> mirrors packages/<package>/src/<path>; prompts/crates/pi-edit/<name>.md
# mirrors the file of that name under crates/pi-edit/. Without --apply it only reports what differs.
set -eu
U=${1:?usage: sync_prompts.sh <omp checkout> [--apply]}
APPLY=${2:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
changed=0
# existing prompts: changed or removed upstream
while IFS= read -r f; do
    rel=${f#prompts/}
    case "$rel" in
        crates/pi-edit/*) up=$(find "$U/crates/pi-edit" -name "$(basename "$rel")" -type f 2>/dev/null | head -1) ;;
        *) pkg=${rel%%/*}; up="$U/packages/$pkg/src/${rel#*/}" ;;
    esac
    if [ -z "${up:-}" ] || [ ! -f "$up" ]; then echo "removed upstream: $f"; changed=1; continue; fi
    if ! cmp -s "$up" "$f"; then
        echo "changed: $f"; changed=1
        [ "$APPLY" = --apply ] && cp "$up" "$f"
    fi
done < <(find prompts -name '*.md' -type f | LC_ALL=C sort)
# new upstream prompts (packages/*/src/**/*.md that omc doesn't embed yet)
while IFS= read -r up; do
    rel=${up#"$U/packages/"}; pkg=${rel%%/src/*}; rest=${rel#*/src/}
    f="prompts/$pkg/$rest"
    [ -f "$f" ] && continue
    case "$pkg" in typescript-edit-benchmark|*bench*) continue ;; esac
    echo "new upstream: $f"; changed=1
    if [ "$APPLY" = --apply ]; then mkdir -p "$(dirname "$f")"; cp "$up" "$f"; fi
done < <(find "$U/packages" -path '*/node_modules' -prune -o -path '*/src/*' -name '*.md' -type f -print | LC_ALL=C sort)
[ $changed = 0 ] && echo "prompts are in sync"
exit 0
