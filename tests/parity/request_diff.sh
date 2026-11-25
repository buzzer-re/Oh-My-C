#!/usr/bin/env bash
# Compare the first model request omp and omc send for the same prompt (print mode, same cwd, same
# mock model through throwaway agent dirs; the product name omp->omc is normalized). Prints tool-set/param differences and a system-prompt diff.
#   tests/parity/request_diff.sh [prompt] [cwd]
# Env: RD_DIR (scratch, default $TMPDIR/omc-reqdiff), OMP_BIN (default omp), RD_SETUP=<script> runs
# `$RD_SETUP <agent-dir>` for both agent dirs before the run (e.g. to add skills/rules/settings),
# RD_WORKSETUP=<script> runs `$RD_WORKSETUP <cwd>` before each app's run (reseed files a tool changes),
# RD_MODE=rpc drives `--mode rpc` (tests/parity/rpc_drive.py) instead of `-p`, so session continuations print
# mode suppresses (todo reminders) reach the mock; the RPC events (todo_reminder, notice) land in $RD_DIR/<app>.out.
# The tool messages of the last request each app sent are written to $RD_DIR/tools.{omp,omc} and diffed.
# Each app runs with its own throwaway HOME ($RD_DIR/home.<app>), so skills or configs under the real ~ never leak in.
# RD_TITLE=1 keeps title generation on (both apps otherwise run with OMC_NO_TITLE / PI_NO_TITLE).
# Prints the size of each first request (bytes of the compact JSON body, system prompt, tool schemas) and every
# side request (title, skill-description compression) with its position relative to the main request.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=${RD_DIR:-${TMPDIR:-/tmp}/omc-reqdiff}
PROMPT=${1:-say:hello}
WORK=${2:-$D/work}
rm -rf "$D/ompa" "$D/omca" "$D/home.omp" "$D/home.omc" "$D"/*.jsonl; mkdir -p "$D" "$WORK" "$D/home.omp" "$D/home.omc"
# the default work dir is a repo root (a `.git` entry), so neither app's project discovery walks above it into the real
# home's .claude/.agents skills and rules (both apps stop their ancestor walk at the repo root)
[ -n "${2:-}" ] || mkdir -p "$WORK/.git"
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
    local notitle="OMC_NO_TITLE=1 PI_NO_TITLE=1"; [ -n "${RD_TITLE:-}" ] && notitle=
    MOCK_LOG="$D/$1.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $port >/dev/null 2>&1 & local mp=$!
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
    if [ "${RD_MODE:-print}" = rpc ]; then # session continuations print mode suppresses (todo reminders)
        (cd "$WORK" && env MOCK_KEY=x HOME="$D/home.$1" $notitle "$2=$3" timeout 150 python3 -I "$ROOT/tests/parity/rpc_drive.py" "$4" "$PROMPT" >"$D/$1.out" 2>&1)
    else
        (cd "$WORK" && env MOCK_KEY=x HOME="$D/home.$1" $notitle "$2=$3" timeout 90 "$4" -p "$PROMPT" </dev/null >"$D/$1.out" 2>&1)
    fi
    kill $mp 2>/dev/null; wait $mp 2>/dev/null
}
run omp PI_CODING_AGENT_DIR "$D/ompa" "${OMP_BIN:-omp}"
run omc OMC_CODING_AGENT_DIR "$D/omca" "${OMC_BIN:-$ROOT/build/omc}"
python3 -I - "$D" <<'PY'
import json, sys
D = sys.argv[1]
def main(name):
    rs = [json.loads(l) for l in open(f"{D}/{name}.jsonl")]
    rs = [r for r in rs if r.get("tools")]
    return rs[0] if rs else None
a, b = main("omp"), main("omc")
def prod(s):  # product name only: omp's -> omc's, omp:// -> omc://, `cfg://`: omp settings -> omc settings
    return s.replace("omp's", "omc's").replace("omp://", "omc://").replace("`cfg://`: omp settings", "`cfg://`: omc settings") if isinstance(s, str) else s
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
def size(o): return len(json.dumps(o, ensure_ascii=False, separators=(",", ":")).encode())
for name, r in (("omp", a), ("omc", b)):  # sizes of the first request, and side requests around it
    allr = [json.loads(l) for l in open(f"{D}/{name}.jsonl")]
    i = next(i for i, x in enumerate(allr) if x == r)
    side = [x for x in allr if x is not allr[i]]
    print(f"{name}: request {size(r)} B, system {len(txt(r['messages'][0]).encode())} B, {len(r['tools'])} tools "
          f"{sum(size(t) for t in r['tools'])} B, {len(r['messages'])} messages; {i} side request(s) before it, "
          f"{len(side) - i} after")
for t in a["tools"]:
    o = next((x for x in b["tools"] if x["function"]["name"] == t["function"]["name"]), None)
    if o is None or prod(json.dumps(t)) != json.dumps(o):
        print(f"  {t['function']['name']}: omp {size(t)} B, omc {size(o) if o else '-'} B")
for o in b["tools"]:
    if not any(x["function"]["name"] == o["function"]["name"] for x in a["tools"]):
        print(f"  {o['function']['name']}: omp - B, omc {size(o)} B")
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
