#!/usr/bin/env bash
# Compare the provider request omp and omc send for a real catalog model with service-tier / provider settings
# (WP-14): both apps run in print mode against tests/e2e/mock_providers.py, the provider's baseUrl pointed at the
# mock through models.yml, with throwaway agent dirs. Prints the request fields that carry service tiers (body
# service_tier / serviceTier / speed, anthropic-beta, anthropic-usage-limit) side by side and fails on a difference.
#   tests/parity/tier_diff.sh <provider/model> <config.yml lines> [omc extra args...]
# e.g. tests/parity/tier_diff.sh openai/gpt-5.4 'tier.openai: priority'
#      tests/parity/tier_diff.sh anthropic/claude-opus-4-6 'tier.anthropic: priority'
# Env: TD_DIR (scratch, default $TMPDIR/omc-tierdiff), OMP_BIN (default omp), TD_ARGS (args for both apps).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=${TD_DIR:-${TMPDIR:-/tmp}/omc-tierdiff}
SEL=$1
CFG=${2:-}
shift 2 || shift $#
PROV=${SEL%%/*}
rm -rf "$D"; mkdir -p "$D/work"
port=$((30000 + RANDOM % 20000))
case "$PROV" in
openai) BASE="http://127.0.0.1:$port/v1" ;;
anthropic) BASE="http://127.0.0.1:$port" ;;
google) BASE="http://127.0.0.1:$port/v1beta" ;;
*) BASE="http://127.0.0.1:$port/v1" ;;
esac
for d in ompa omca; do
    mkdir -p "$D/$d"
    printf 'providers:\n  %s:\n    baseUrl: %s\n' "$PROV" "$BASE" > "$D/$d/models.yml"
    printf 'modelRoles:\n  default: %s\n%s\n' "$SEL" "$CFG" > "$D/$d/config.yml"
done
run() { # log-name env-var agent-dir binary [args]
    local name=$1 var=$2 dir=$3 bin=$4
    shift 4
    MOCK_LOG="$D/$name.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $port >/dev/null 2>&1 & local mp=$!
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
    (cd "$D/work" && env OPENAI_API_KEY=sk-test ANTHROPIC_API_KEY=sk-ant-api-test GEMINI_API_KEY=g-test OMC_NO_TITLE=1 \
        PI_NO_TITLE=1 "$var=$dir" timeout 90 "$bin" ${TD_ARGS:-} "$@" -p "say hello" </dev/null >"$D/$name.out" 2>&1)
    kill $mp 2>/dev/null; wait $mp 2>/dev/null
}
run omp PI_CODING_AGENT_DIR "$D/ompa" "${OMP_BIN:-omp}"
run omc OMC_CODING_AGENT_DIR "$D/omca" "$ROOT/build/omc" "$@"
python3 -I - "$D" <<'PY'
import json, sys
D = sys.argv[1]
def first(name):
    try:
        rs = [json.loads(l) for l in open(f"{D}/{name}.jsonl")]
    except OSError:
        return None
    rs = [r for r in rs if r["path"].split("?")[0].endswith(("/messages", "/chat/completions", "/responses")) or ":stream" in r["path"]]
    main = [r for r in rs if r["body"].get("tools")]  # the agent turn (side requests such as titles carry no tools)
    return (main or rs)[0] if rs else None
def fields(r):
    b, h = r["body"], r["headers"]
    gen = b.get("generationConfig") or {}
    return {"service_tier": b.get("service_tier"), "serviceTier": b.get("serviceTier"), "speed": b.get("speed"),
            "anthropic-beta": sorted((h.get("anthropic-beta") or "").split(",")) if h.get("anthropic-beta") else None,
            "anthropic-usage-limit": h.get("anthropic-usage-limit"), "path": r["path"].split("?")[0]}
a, b = first("omp"), first("omc")
if not a or not b:
    sys.exit(f"missing request: omp={bool(a)} omc={bool(b)} (see {D}/*.out)")
fa, fb = fields(a), fields(b)
bad = False
for k in fa:
    mark = "  " if fa[k] == fb[k] else "!!"
    bad |= mark == "!!"
    print(f"{mark} {k}: omp={fa[k]!r} omc={fb[k]!r}")
sys.exit(1 if bad else 0)
PY
