#!/usr/bin/env bash
# Compare the first model request omp and omc send for the same prompt (print mode, same cwd, same
# mock model through throwaway agent dirs; the product name omp->omc is normalized). Prints tool-set/param differences and a system-prompt diff.
#   tests/parity/request_diff.sh [prompt] [cwd]
# Env: RD_DIR (scratch, default $TMPDIR/omc-reqdiff), OMP_BIN (default omp), RD_SETUP=<script> runs
# `$RD_SETUP <agent-dir>` for both agent dirs before the run (e.g. to add skills/rules/settings),
# RD_WORKSETUP=<script> runs `$RD_WORKSETUP <cwd>` before each app's run (reseed files a tool changes).
# The tool messages of the last request each app sent are written to $RD_DIR/tools.{omp,omc} and diffed.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=${RD_DIR:-${TMPDIR:-/tmp}/omc-reqdiff}
PROMPT=${1:-say:hello}
WORK=${2:-$D/work}
rm -rf "$D/ompa" "$D/omca" "$D"/*.jsonl; mkdir -p "$D" "$WORK"
port=$((30000 + RANDOM % 20000))
for d in ompa omca; do
    mkdir -p "$D/$d"
    cat > "$D/$d/models.yml" <<YML
providers:
  mock:
    baseUrl: http://127.0.0.1:$port/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: mock-1
        name: Mock One
        contextWindow: 100000
        maxTokens: 4096
YML
    printf 'modelRoles:\n  default: mock/mock-1\n' > "$D/$d/config.yml"
    [ -n "${RD_SETUP:-}" ] && "$RD_SETUP" "$D/$d"
done
run() { # log-name env-var agent-dir binary
    [ -n "${RD_WORKSETUP:-}" ] && "$RD_WORKSETUP" "$WORK"
    MOCK_LOG="$D/$1.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $port >/dev/null 2>&1 & local mp=$!
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
    (cd "$WORK" && env MOCK_KEY=x OMC_NO_TITLE=1 PI_NO_TITLE=1 "$2=$3" timeout 90 "$4" -p "$PROMPT" </dev/null >"$D/$1.out" 2>&1)
    kill $mp 2>/dev/null; wait $mp 2>/dev/null
}
run omp PI_CODING_AGENT_DIR "$D/ompa" "${OMP_BIN:-omp}"
run omc OMC_CODING_AGENT_DIR "$D/omca" "$ROOT/build/omc"
python3 -I - "$D" <<'PY'
import json, sys
D = sys.argv[1]
def main(name):
    rs = [json.loads(l) for l in open(f"{D}/{name}.jsonl")]
    rs = [r for r in rs if r.get("tools")]
    return rs[0] if rs else None
a, b = main("omp"), main("omc")
def prod(s):  # product name only: omp's -> omc's, omp:// -> omc://
    return s.replace("omp's", "omc's").replace("omp://", "omc://") if isinstance(s, str) else s
if not a or not b: sys.exit(f"missing request: omp={bool(a)} omc={bool(b)}")
ta = [t["function"]["name"] for t in a["tools"]]; tb = [t["function"]["name"] for t in b["tools"]]
print("tools omp:", ta); print("tools omc:", tb)
for t in a["tools"]:
    n = t["function"]["name"]
    o = next((x for x in b["tools"] if x["function"]["name"] == n), None)
    if o is None: continue
    for k in ("description", "parameters"):
        if prod(t["function"].get(k)) != o["function"].get(k):
            print(f"  tool {n}: {k} differs")
            json.dump(t["function"].get(k), open(f"{D}/tool.{n}.{k}.omp", "w"), indent=1, ensure_ascii=False)
            json.dump(o["function"].get(k), open(f"{D}/tool.{n}.{k}.omc", "w"), indent=1, ensure_ascii=False)
for k in sorted(set(a) | set(b)):
    if k not in ("messages", "tools") and a.get(k) != b.get(k): print(f"  param {k}: {a.get(k)} | {b.get(k)}")
def txt(m):
    c = m["content"]
    return c if isinstance(c, str) else "".join(x.get("text", "") for x in c)
open(f"{D}/sys.omp", "w").write(prod(txt(a["messages"][0])))
open(f"{D}/sys.omc", "w").write(txt(b["messages"][0]))
open(f"{D}/msgs.omp", "w").write(json.dumps(a["messages"][1:], indent=1, ensure_ascii=False))
open(f"{D}/msgs.omc", "w").write(json.dumps(b["messages"][1:], indent=1, ensure_ascii=False))
def tools(name):  # tool messages of the last request (the whole conversation so far)
    rs = [json.loads(l) for l in open(f"{D}/{name}.jsonl")]
    rs = [r for r in rs if r.get("tools")]
    ms = [m for m in rs[-1]["messages"] if m.get("role") == "tool"] if rs else []
    return "".join(f"=== {m.get('tool_call_id', '')}\n{txt(m)}\n" for m in ms)
open(f"{D}/tools.omp", "w").write(prod(tools("omp")))
open(f"{D}/tools.omc", "w").write(tools("omc"))
PY
diff "$D/sys.omp" "$D/sys.omc" > "$D/sys.diff" && echo "system prompt: identical" || echo "system prompt: differs ($(grep -c '^[<>]' "$D/sys.diff") lines; see $D/sys.diff)"
diff "$D/msgs.omp" "$D/msgs.omc" > "$D/msgs.diff" && echo "messages: identical" || echo "messages: differ (see $D/msgs.diff)"
if [ -s "$D/tools.omp" ] || [ -s "$D/tools.omc" ]; then
    diff "$D/tools.omp" "$D/tools.omc" > "$D/tools.diff" && echo "tool messages: identical" || echo "tool messages: differ (see $D/tools.diff)"
fi
