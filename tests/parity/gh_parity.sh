#!/usr/bin/env bash
# github tool + pr:// / issue:// parity: runs tests/parity/gh_cases.py through tool_diff.sh with the fake `gh`
# (tests/e2e/bin-gh/gh) on PATH, `github.enabled: true`, a throwaway gh config dir / GitHub cache / worktree
# dir, and a fresh git checkout (bare "remote" + clone) per run.
#   tests/parity/gh_parity.sh [case-name]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export TD_DIR=${TD_DIR:-${TMPDIR:-/tmp}/omc-ghdiff}
mkdir -p "$TD_DIR"
case "${1:-}" in --reset) ;; *) [ -n "${1:-}" ] && export TD_ONLY=$1 ;; esac
if [ "${1:-}" = "--reset" ]; then # internal: build the git fixture in $2
    W=$2; R=$(dirname "$W")/remote.git; S=$(dirname "$W")/seed
    rm -rf "$R" "$S" "$WT_DIR"; mkdir -p "$WT_DIR"
    g() { git -c user.name=Fix -c user.email=fix@example.com -c init.defaultBranch=main -c commit.gpgsign=false "$@"; }
    g init -q --bare "$R"
    g init -q "$S"
    printf '# Widgets\n' > "$S/README.md"; g -C "$S" add -A; g -C "$S" commit -qm init
    g -C "$S" checkout -qb feature/frob; printf 'int frob(void);\n' > "$S/frob.h"; g -C "$S" add -A; g -C "$S" commit -qm frob
    g -C "$S" push -q "$R" main feature/frob
    rm -rf "$W"; g clone -q "$R" "$W" 2>/dev/null
    g -C "$W" config user.name Fix; g -C "$W" config user.email fix@example.com; g -C "$W" config commit.gpgsign false
    # a branch with pr_checkout's push metadata (as if `op: pr_checkout` had run) plus one local commit
    g -C "$W" branch -q pr-12-pre origin/feature/frob
    for kv in remote=origin merge=refs/heads/feature/frob pushRemote=origin ompPrHeadRef=feature/frob \
              ompPrUrl=https://github.com/acme/widgets/pull/12 ompPrIsCrossRepository=false ompPrMaintainerCanModify=true; do
        g -C "$W" config "branch.pr-12-pre.${kv%%=*}" "${kv#*=}"
    done
    g -C "$W" checkout -q pr-12-pre; printf 'x\n' > "$W/x.txt"; g -C "$W" add -A; g -C "$W" commit -qm local; g -C "$W" checkout -q main
    rm -rf "$S"
    exit 0
fi
setup() { printf 'github:\n  enabled: true\n' >> "$1/config.yml"; }
export -f setup
export WT_DIR="$TD_DIR/wt"
export TD_SETUP="$TD_DIR/setup.sh" TD_RESET="$TD_DIR/reset.sh" TD_PATH="$ROOT/tests/e2e/bin-gh"
printf '#!/bin/sh\nprintf "github:\\n  enabled: true\\n" >> "$1/config.yml"\n' > "$TD_SETUP"
printf '#!/bin/sh\nexec "%s" --reset "$1"\n' "$ROOT/tests/parity/gh_parity.sh" > "$TD_RESET"
chmod +x "$TD_SETUP" "$TD_RESET"
mkdir -p "$TD_DIR/ghconfig"
unset GH_TOKEN GITHUB_TOKEN GH_ENTERPRISE_TOKEN GITHUB_ENTERPRISE_TOKEN GH_HOST
export GH_CONFIG_DIR="$TD_DIR/ghconfig" OMP_GITHUB_CACHE_DB="$TD_DIR/gh-cache.db" OMP_WORKTREE_DIR="$WT_DIR" \
    OMC_WORKTREE_DIR="$WT_DIR" FAKE_GH_REMOTE="$TD_DIR/remote.git"
python3 "$ROOT/tests/parity/gh_cases.py" > "$TD_DIR/cases.txt"
exec "$ROOT/tests/parity/tool_diff.sh" "$TD_DIR/cases.txt"
