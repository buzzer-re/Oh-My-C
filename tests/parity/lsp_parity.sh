#!/usr/bin/env bash
# lsp tool parity: runs every case of lsp_cases.py through tool_diff.sh in both omp and omc against clangd
# (fixtures/lsp_c; .clangd is its root marker) and reports byte differences in the tool result text.
#   tests/parity/lsp_parity.sh [case-name]
# Needs clangd and omp on PATH. Each case starts both apps cold; project-indexed actions wait out
# omp's 15 s project-load fallback (clangd reports no progress without a compile database), so a full run takes a while.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
command -v clangd >/dev/null || { echo "clangd not installed"; exit 0; }
D=${TD_DIR:-${TMPDIR:-/tmp}/omc-lspparity}
mkdir -p "$D"
python3 -I "$ROOT/tests/parity/lsp_cases.py" > "$D/cases.txt"
export TD_DIR=$D TD_SETUP="$ROOT/tests/parity/lsp_setup.sh" TD_FIXTURE="$ROOT/tests/parity/fixtures/lsp_c"
[ -n "${1:-}" ] && export TD_ONLY=$1
exec "$ROOT/tests/parity/tool_diff.sh" "$D/cases.txt"
