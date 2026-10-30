#!/usr/bin/env bash
# End-to-end tests: drive build/omc against a local mock OpenAI-compatible server.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B="$ROOT/build/omc"
T=$(mktemp -d "${TMPDIR:-/tmp}/omc-e2e.XXXXXX")
PORT=$((20000 + RANDOM % 20000))
mkdir -p "$T/agent" "$T/work"
cat > "$T/agent/models.yml" <<YML
providers:
  mock:
    baseUrl: http://127.0.0.1:$PORT/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: mock-1
        contextWindow: 100000
        maxTokens: 4096
YML
MOCK_LOG="$T/req.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $PORT & MPID=$!
trap 'kill $MPID 2>/dev/null; rm -rf "$T"' EXIT
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done
export OMC_CODING_AGENT_DIR="$T/agent" MOCK_KEY=x OMC_NO_TITLE=1
cd "$T/work"
fail=0
check() { # name expected-substring command...
    local name=$1 want=$2; shift 2
    local out; out=$(timeout 30 "$@" </dev/null 2>&1)
    if [[ "$out" == *"$want"* ]]; then echo "   ok  $name"; else echo "   FAIL $name: got: $out"; fail=1; fi
}
echo "== e2e"
check "bash tool"      "Tool said: hello-e2e"            "$B" -m mock/mock-1 -p "bash:echo hello-e2e"
check "write tool"     "Created a/b.txt"                 "$B" -m mock/mock-1 -p "write:a/b.txt:written by omc"
check "file content"   "written by omc"                  cat a/b.txt
check "read hashline"  "1:written by omc"                "$B" -m mock/mock-1 -p "read:a/b.txt"
check "glob tool"      "a/b.txt"                         "$B" -m mock/mock-1 -p "glob:**/*.txt"
check "grep tool"      "*1:written by omc"               "$B" -m mock/mock-1 -p "grep:written"
check "plain answer"   "plain answer"                    "$B" -m mock/mock-1 -p "hi"
check "continue"       "plain answer"                    "$B" -m mock/mock-1 -c -p "again"
check "json mode"      '"type":"agent_end"'              "$B" -m mock/mock-1 --mode json -p "bash:echo j"
check "rpc mode"       '"status":"completed"'            bash -c "printf '%s\n' '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"bash:echo r\"}' | (cat; sleep 2) | '$B' -m mock/mock-1 --mode rpc"
check "sessions list"  "msgs"                            "$B" sessions
check "no-session"     "plain answer"                    "$B" -m mock/mock-1 --no-session -p "x"
# hashline edit: read to get the tag, then replace line 1 and append after it
printf 'alpha\nbeta\ngamma\n' > e.txt
TAG=$(timeout 30 "$B" -m mock/mock-1 -p "read:e.txt" </dev/null | grep -o 'e.txt#[0-9A-F]*' | head -1 | cut -d'#' -f2)
check "edit apply"     "[e.txt#"  "$B" -m mock/mock-1 -p "edit:[e.txt#$TAG]\nPUT 2.=2:\n+BETA\nPUT >3:\n+delta"
check "edit result"    $'alpha\nBETA\ngamma\ndelta'  cat e.txt
check "edit stale tag" "current tag #"  "$B" -m mock/mock-1 -p "edit:[e.txt#$TAG]\nPUT 1.=1:\n+x"
check "todo tool"      "Remaining items (2)"  "$B" -m mock/mock-1 -p "todo:first,second"
# skills + internal URLs + context files
mkdir -p "$T/agent/skills/demo"
printf -- '---\nname: demo\ndescription: Demo skill for tests\n---\nSKILL BODY LINE\n' > "$T/agent/skills/demo/SKILL.md"
printf 'Project rule: always test.\n' > AGENTS.md
check "skill url"      "SKILL BODY LINE"  "$B" -m mock/mock-1 -p "read:skill://demo"
check "unknown skill"  "Unknown skill"    "$B" -m mock/mock-1 -p "read:skill://nope"
grep -q "Demo skill for tests" "$T/req.jsonl" && echo "   ok  skill in system prompt" || { echo "   FAIL skill in system prompt"; fail=1; }
grep -q "Project rule: always test." "$T/req.jsonl" && echo "   ok  AGENTS.md in prompt" || { echo "   FAIL AGENTS.md in prompt"; fail=1; }
# TTSR: a stream-triggered rule interrupts, injects and retries
mkdir -p .omc/rules
printf -- '---\ndescription: no forbidden word\ncondition: "FORBIDDEN"\nscope: text\n---\nNever say the forbidden word.\n' > .omc/rules/no-forbidden.md
check "ttsr retry"     "complied with rule"  "$B" -m mock/mock-1 -p "say:this is FORBIDDEN text"
grep -q 'Never say the forbidden word' "$T/req.jsonl" && echo "   ok  ttsr injection sent" || { echo "   FAIL ttsr injection sent"; fail=1; }
check "builtin rule"   "Box::leak"  "$B" -m mock/mock-1 -p "write:src/x.rs:fn f() { Box::leak(v); }"
# MCP stdio server bridged as mcp__<server>_<tool>
printf '{"mcpServers":{"mockmcp":{"command":"python3","args":["-I","%s"]}}}' "$ROOT/tests/e2e/mock_mcp.py" > "$T/agent/mcp.json"
check "mcp tool"       "mcp echo: hi-mcp"  "$B" -m mock/mock-1 -p 'call:mcp__mockmcp_echo_text:{"text":"hi-mcp"}'
grep -q "Mock MCP server instructions" "$T/req.jsonl" && echo "   ok  mcp instructions in prompt" || { echo "   FAIL mcp instructions"; fail=1; }
rm -f "$T/agent/mcp.json"
exit $fail
