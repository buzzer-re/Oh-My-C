#!/usr/bin/env bash
# Compare omp's and omc's prompt-cache warming (WP-14, session/cache-warmer.ts) against tests/e2e/mock_providers.py:
# both apps run one RPC prompt on anthropic/claude-opus-4-6 with a 12 s promptCache override (a refresh every 2 s)
# and large mock cache usage, stay idle, and the replays (the turn's request again under a one-token output budget)
# are counted and compared.
#   tests/parity/cachewarm_diff.sh [cacheWarming mode] [idle seconds]
# Env: CW_DIR (scratch, default $TMPDIR/omc-cachewarm), OMP_BIN (default omp).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=${CW_DIR:-${TMPDIR:-/tmp}/omc-cachewarm}
MODE=${1:-idle}
IDLE=${2:-7}
rm -rf "$D"; mkdir -p "$D/work"
port=$((30000 + RANDOM % 20000))
for d in ompa omca; do
    mkdir -p "$D/$d"
    printf 'providers:\n  anthropic:\n    baseUrl: http://127.0.0.1:%s\n    modelOverrides:\n      claude-opus-4-6:\n        promptCache:\n          short: 12\n' \
        "$port" > "$D/$d/models.yml"
    printf 'modelRoles:\n  default: anthropic/claude-opus-4-6\nproviders:\n  cacheWarming: %s\n' "$MODE" > "$D/$d/config.yml"
done
run() { # name env-var agent-dir binary
    MOCK_CACHE_TOKENS=200000 MOCK_LOG="$D/$1.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $port >/dev/null 2>&1 & local mp=$!
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
    (cd "$D/work" && env OMC_LOG="$D/$1.log" OMC_DEBUG=1 ANTHROPIC_API_KEY=sk-ant-api-test OMC_NO_TITLE=1 PI_NO_TITLE=1 "$2=$3" timeout 120 \
        python3 -I "$ROOT/tests/parity/rpc_drive.py" "$4" "say hello" "$IDLE" >"$D/$1.out" 2>&1)
    kill $mp 2>/dev/null; wait $mp 2>/dev/null
}
run omp PI_CODING_AGENT_DIR "$D/ompa" "${OMP_BIN:-omp}"
run omc OMC_CODING_AGENT_DIR "$D/omca" "$ROOT/build/omc"
python3 -I - "$D" <<'PY'
import json, sys
D = sys.argv[1]
def reqs(name):
    try:
        rs = [json.loads(l) for l in open(f"{D}/{name}.jsonl")]
    except OSError:
        return []
    return [r for r in rs if r["path"].split("?")[0].endswith("/messages") and r["body"].get("tools")]
bad = False
counts, warm_tokens = {}, {}
def strip(b):
    b = dict(b); b.pop("max_tokens", None); return json.dumps(b, sort_keys=True)
for name in ("omp", "omc"):
    rs = reqs(name)
    seen, turns, warms = set(), [], []
    for r in rs:
        k = strip(r["body"])
        (warms if k in seen else turns).append(r)
        seen.add(k)
    counts[name] = len(warms)
    warm_tokens[name] = sorted({w["body"].get("max_tokens") for w in warms})
    print(f"{name}: {len(turns)} turn request(s), {len(warms)} warm replay(s), replay max_tokens {warm_tokens[name]}")
if (counts["omp"] == 0) != (counts["omc"] == 0) or abs(counts["omp"] - counts["omc"]) > 1:
    print(f"!! replay counts differ: omp={counts['omp']} omc={counts['omc']}")
    bad = True
if warm_tokens["omp"] != warm_tokens["omc"]:
    print("!! replay max_tokens differ")
    bad = True
sys.exit(1 if bad else 0)
PY
