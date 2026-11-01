#!/usr/bin/env bash
# Compare the tool result text omp and omc return to the model for scripted tool calls.
# Each case is one mock-model prompt (see tests/e2e/mock_openai.py, e.g. `call:<tool>:<json>`); both
# apps run it in print mode from the same freshly copied work tree (TD_FIXTURE), against the same mock
# model through throwaway agent dirs. The text of every tool result message the app sends back is
# extracted from the request log and diffed.
#   tests/parity/tool_diff.sh <cases-file>      # one prompt per line; `# name` lines label the next case
# Env: TD_DIR (scratch, default $TMPDIR/omc-tooldiff), OMP_BIN (default omp), TD_FIXTURE (directory
# copied to $TD_DIR/work before each run; default: empty dir), TD_SETUP=<script> runs `$TD_SETUP <agent-dir>`
# for both agent dirs, TD_PATH (prepended to PATH for both apps, e.g. a fake `gh`), TD_ONLY=<name> runs one case.
# Exit status: number of differing cases.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=${TD_DIR:-${TMPDIR:-/tmp}/omc-tooldiff}
CASES=$1
rm -rf "$D/ompa" "$D/omca"; mkdir -p "$D/out"
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
    [ -n "${TD_SETUP:-}" ] && "$TD_SETUP" "$D/$d"
done
MOCK_LOG="$D/mock.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $port >/dev/null 2>&1 & MP=$!
trap 'kill $MP 2>/dev/null' EXIT
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
run() { # app env-var agent-dir binary prompt
    rm -rf "$D/work"; mkdir -p "$D/work"
    [ -n "${TD_FIXTURE:-}" ] && cp -a "$TD_FIXTURE/." "$D/work/"
    : > "$D/mock.jsonl"
    (cd "$D/work" && env MOCK_KEY=x OMC_NO_TITLE=1 PI_NO_TITLE=1 PATH="${TD_PATH:+$TD_PATH:}$PATH" "$2=$3" \
        timeout 120 "$4" -p "$5" </dev/null >"$D/out/$1.stdout" 2>&1)
    python3 -I - "$D/mock.jsonl" "$D/work" <<'PY' > "$D/out/$1.txt"
import json, sys
log, work = sys.argv[1], sys.argv[2]
reqs = [json.loads(l) for l in open(log)]
seen = set()
for r in reqs:
    for m in r.get("messages", []):
        if m.get("role") != "tool" or m.get("tool_call_id") in seen: continue
        seen.add(m.get("tool_call_id"))
        c = m.get("content")
        t = c if isinstance(c, str) else "".join(p.get("text", "") for p in c if isinstance(p, dict))
        sys.stdout.write(t.replace(work, "<work>") + "\n")
PY
}
fails=0; name=""; n=0
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        "# "*) name=${line#\# }; continue ;;
        ""|"#"*) continue ;;
    esac
    n=$((n + 1)); [ -n "$name" ] || name="case$n"
    if [ -n "${TD_ONLY:-}" ] && [ "$TD_ONLY" != "$name" ]; then name=""; continue; fi
    run "$name.omp" PI_CODING_AGENT_DIR "$D/ompa" "${OMP_BIN:-omp}" "$line"
    run "$name.omc" OMC_CODING_AGENT_DIR "$D/omca" "$ROOT/build/omc" "$line"
    if diff "$D/out/$name.omp.txt" "$D/out/$name.omc.txt" > "$D/out/$name.diff"; then
        echo "same   $name"
    else
        echo "DIFFER $name  ($D/out/$name.diff)"; fails=$((fails + 1))
    fi
    name=""
done < "$CASES"
exit $fails
