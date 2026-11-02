#!/usr/bin/env bash
# Dev-only: run `omp search` and `omc search` (the web_search tool's executeSearch) for each model selector against
# tests/e2e/mock_search.py and diff what each sent (method, path, body) and printed (the tool's model-facing text is
# what `search` renders; the request log is compared exactly, minus volatile ids).
#   tests/parity/search_diff.sh [selector ...]      (default: one selector per backend)
# Throwaway HOME/agent dirs and fake keys only; real endpoints are never reached: built-in providers are re-pointed
# with models.yml baseUrl overrides, SearXNG through SEARXNG_ENDPOINT, Kimi through MOONSHOT_SEARCH_BASE_URL.
# A selector "-" runs the web role chain (no --model). Env: OMP_BIN (default omp), OMC_BIN (default build/omc), SD_DIR
# (default $TMPDIR/omc-searchdiff), SD_QUERY, SD_CONFIG (config.yml text; @M@ = mock URL), SD_SEARXNG (SearXNG path).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OMP=${OMP_BIN:-omp}
OMC=${OMC_BIN:-$ROOT/build/omc}
D=${SD_DIR:-${TMPDIR:-/tmp}/omc-searchdiff}
Q=${SD_QUERY:-latest zig release notes}
SELECTORS=("$@")
[[ ${#SELECTORS[@]} -eq 0 ]] && SELECTORS=(anthropic/claude-sonnet-4-5 openai/gpt-5.2 google/gemini-2.5-pro xai/grok-4
    web/searxng web/kimi)
rm -rf "$D"
mkdir -p "$D"
PORT=$((20000 + RANDOM % 20000))
M=http://127.0.0.1:$PORT
MOCK_LOG="$D/mock.jsonl" python3 -I "$ROOT/tests/e2e/mock_search.py" $PORT &
SD_MOCK_PID=$!
trap 'kill $SD_MOCK_PID 2>/dev/null' EXIT
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done
status=0
for sel in "${SELECTORS[@]}"; do
    for side in omp omc; do
        A="$D/$side-agent"
        rm -rf "$A" "$D/$side-home" "$D/$side-work"
        mkdir -p "$A" "$D/$side-home" "$D/$side-work"
        cat > "$A/models.yml" <<YML
providers:
  anthropic:
    baseUrl: $M/anthropic
  openai:
    baseUrl: $M/openai/v1
  google:
    baseUrl: $M/google/v1beta
  xai:
    baseUrl: $M/xai/v1
YML
        printf "%s\n" "${SD_CONFIG:-}" | sed "s#@M@#$M#g" > "$A/config.yml"
        : > "$D/mock.jsonl"
        bin=$OMC
        [[ $side == omp ]] && bin=$OMP
        (cd "$D/$side-work" && env HOME="$D/$side-home" PI_CODING_AGENT_DIR="$A" OMC_CODING_AGENT_DIR="$A" \
            OMP_SKIP_SETUP=1 OMC_SKIP_SETUP=1 OMC_NO_LOCAL_DISCOVERY=1 NO_COLOR=1 \
            ANTHROPIC_API_KEY=sk-ant-api-fake OPENAI_API_KEY=sk-fake GEMINI_API_KEY=g-fake XAI_API_KEY=x-fake \
            MOONSHOT_SEARCH_API_KEY=k-fake MOONSHOT_SEARCH_BASE_URL="$M/kimi/coding/v1/search" SEARXNG_ENDPOINT="$M${SD_SEARXNG:-/searx}" \
            timeout 60 "$bin" search $([[ $sel != - ]] && echo --model "$sel") "$Q" >"$D/$side.out" 2>&1)
        python3 -I - "$D/mock.jsonl" > "$D/$side.req" <<'PY'
import json, re, sys
for line in open(sys.argv[1]):
    r = json.loads(line)
    s = json.dumps({"method": r["method"], "path": r["path"], "body": r["body"]}, indent=1, sort_keys=False)
    s = re.sub(r'"(id|requestId)": "[^"]*"', r'"\1": "<id>"', s)
    s = re.sub(r'omp-\d+-[0-9a-z]+|omc/[0-9.]+|omp/[0-9.]+', '<ua>', s)
    print(s)
PY
    done
    if diff -u "$D/omp.req" "$D/omc.req" > "$D/diff.txt"; then
        echo "same  $sel ($(grep -c '"method"' "$D/omc.req") requests)"
    else
        echo "DIFF  $sel"
        cat "$D/diff.txt"
        status=1
    fi
    sed 's/\x1b\[[0-9;]*m//g' "$D/omp.out" > "$D/omp.txt"
    sed 's/\x1b\[[0-9;]*m//g' "$D/omc.out" > "$D/omc.txt"
    diff -u "$D/omp.txt" "$D/omc.txt" > "$D/out.diff" || { echo "      rendered output differs ($D/out.diff)"; [[ -n ${SD_SHOW:-} ]] && cat "$D/out.diff"; }
done
exit $status
