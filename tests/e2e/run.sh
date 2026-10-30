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
# persistent bash: state survives across calls in one session
check "bash persists"  "FOO=kept"  bash -c "( printf '%s\n' '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"bash:export FOO=kept\"}'; sleep 2; printf '%s\n' '{\"id\":\"2\",\"type\":\"prompt\",\"message\":\"bash:echo FOO=\$FOO\"}'; sleep 2 ) | '$B' -m mock/mock-1 --mode rpc"
check "bash exit code" "exited with code 3"  "$B" bash 'exit 3'
# plan mode: read-only gating, local:// plan, xd://propose approval (auto-approved without a UI)
check "plan blocks"    "working tree is read-only"  "$B" -m mock/mock-1 --plan-mode -p "write:real.txt:x"
check "plan approve"   "approved. Plan mode ended"  bash -c "( printf '%s\n' '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"call:write:{\\\"path\\\":\\\"local://p-plan.md\\\",\\\"content\\\":\\\"# P\\\"}\"}'; sleep 2; printf '%s\n' '{\"id\":\"2\",\"type\":\"prompt\",\"message\":\"call:write:{\\\"path\\\":\\\"xd://propose\\\",\\\"content\\\":\\\"p\\\"}\"}'; sleep 3 ) | '$B' -m mock/mock-1 --plan-mode --mode rpc"
# eval (python kernel) when python3 is available
if command -v python3 >/dev/null; then
    check "eval python"    "Tool said: 42"  "$B" -m mock/mock-1 -p 'call:eval:{"language":"py","code":"6*7"}'
fi
# checkpoint / rewind
check "rewind"         "Rewinding to checkpoint"  bash -c "( for m in 'call:checkpoint:{\"goal\":\"g\"}' 'read:a/b.txt' 'call:rewind:{\"report\":\"done\"}'; do python3 -c 'import json,sys; print(json.dumps({\"id\":\"x\",\"type\":\"prompt\",\"message\":sys.argv[1]}))' \"\$m\"; sleep 2; done ) | '$B' -m mock/mock-1 --mode rpc"
# ast_grep structural search
printf 'function f(a) {\n  console.log(a, 1);\n  if (q == q) {}\n}\n' > s.ts
check "ast_grep"       '$A = q'  "$B" -m mock/mock-1 -p 'call:ast_grep:{"pat":"$A == $A","path":"s.ts"}'
check "ast_edit stage" "(proposed) 1 replacement"  "$B" -m mock/mock-1 -p 'call:ast_edit:{"ops":[{"pat":"console.log($$$X)","out":"log($$$X)"}],"paths":["s.ts"]}'
# debug tool (DAP) against a scripted mock adapter
printf 'debug:\n  adapters:\n    mockdap:\n      command: python3\n      args: ["-I", "%s"]\n      fileTypes: [".mock"]\n' "$ROOT/tests/e2e/mock_dap.py" > "$T/agent/config.yml"
touch prog.mock
check "debug session"  "x = 60"  bash -c "( for m in 'call:debug:{\"action\":\"launch\",\"program\":\"prog.mock\"}' 'call:debug:{\"action\":\"set_breakpoint\",\"file\":\"prog.mock\",\"line\":5}' 'call:debug:{\"action\":\"continue\"}' 'call:debug:{\"action\":\"step_over\"}' 'call:debug:{\"action\":\"variables\",\"variable_ref\":7}'; do python3 -c 'import json,sys; print(json.dumps({\"id\":\"x\",\"type\":\"prompt\",\"message\":sys.argv[1]}))' \"\$m\"; sleep 2; done ) | '$B' -m mock/mock-1 --mode rpc"
rm -f "$T/agent/config.yml"
# ssh:// internal URLs through a fake ssh client that runs the remote command locally
printf 'remote one\nremote two\n' > "$T/remote.txt"
printf '{"hosts":{"box":{"host":"localhost","username":"me","description":"test box"}}}' > ssh.json
check "ssh hosts"      "[box](ssh://box/)"  env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" -m mock/mock-1 -p "read:ssh://"
check "ssh read sel"   "2:remote two"       env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" -m mock/mock-1 -p "read:ssh://box$T/remote.txt:2"
check "ssh write"      "Wrote ssh://box"    env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" -m mock/mock-1 -p "call:write:{\"path\":\"ssh://box$T/sub/w.txt\",\"content\":\"via ssh\"}"
check "ssh write file" "via ssh"            cat "$T/sub/w.txt"
check "ssh override"   "overrides are not allowed"  env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" -m mock/mock-1 -p "read:ssh://me@box/etc/hosts"
rm -f ssh.json
# goal mode over RPC: hidden context, auto-continuation (stops on no progress), completion, resume pauses
printf 'goal:\n  continuationModes: ["rpc"]\n' > "$T/agent/config.yml"
check "goal continue"  '"customType":"goal-continuation"'  bash -c "( echo '{\"id\":\"g\",\"type\":\"goal\",\"op\":\"create\",\"objective\":\"Ship it\"}'; sleep 0.5; echo '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"bash:echo step\"}'; sleep 4 ) | '$B' -m mock/mock-1 --mode rpc"
check "goal resumed paused" '"status":"paused"'  bash -c "( echo '{\"id\":\"s\",\"type\":\"goal\",\"op\":\"get\"}'; sleep 1 ) | '$B' -m mock/mock-1 -c --mode rpc"
check "goal complete"  'Status: complete'  bash -c "( echo '{\"id\":\"r\",\"type\":\"goal\",\"op\":\"resume\"}'; sleep 0.5; echo '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"call:goal:{\\\"op\\\":\\\"complete\\\"}\"}'; sleep 3 ) | '$B' -m mock/mock-1 -c --mode rpc"
rm -f "$T/agent/config.yml"
# ACP (Agent Client Protocol): permission request answered by the test client
check "acp prompt"     "tool_call_update completed acp-ok"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "bash:echo acp-ok"
check "acp reject"     "tool_call_update failed The user rejected"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "bash:echo no" reject_once
# structured reads: archives, SQLite, PDF, notebooks (fixtures generated with Python's zipfile/tarfile/sqlite3)
if python3 -I "$ROOT/tests/readfmt_fixtures.py" fx >/dev/null 2>&1; then
    check "read zip list"     "hello.txt (16B)"                "$B" -m mock/mock-1 -p "read:fx/t.zip"
    check "read zip member"   "row 12"                         "$B" -m mock/mock-1 -p "read:fx/t.zip:many.txt:12-12"
    check "read zip missing"  "not found inside archive"       "$B" -m mock/mock-1 -p "read:fx/t.zip:nope.txt"
    check "read tgz symlink"  "alpha tar"                      "$B" -m mock/mock-1 -p "read:fx/t.tgz:d/link.txt"
    check "read sqlite list"  "users (3000 rows)"              "$B" -m mock/mock-1 -p "read:fx/t.db"
    check "read sqlite table" "[2995 more rows; append :users?limit=20&offset=5"  "$B" -m mock/mock-1 -p "read:fx/t.db:users"
    check "read sqlite row"   "name: user0042"                 "$B" -m mock/mock-1 -p "read:fx/t.db:users:42"
    check "read sqlite query" "| key499 | 249001 |"           "$B" -m mock/mock-1 -p "read:fx/t.db?q=SELECT * FROM kv ORDER BY v DESC LIMIT 1"
    check "read sqlite wal"   "wal row 5"                      "$B" -m mock/mock-1 -p "read:fx/wal.db:events?where=id%3E4"
    check "read pdf"          "Hello PDF world"                "$B" -m mock/mock-1 -p "read:fx/simple.pdf"
    check "read pdf page"     "From the form"                  "$B" -m mock/mock-1 -p "read:fx/simple.pdf:page2.png"
    check "read pdf objstm"   "Ünïcode ✓ ok"                   "$B" -m mock/mock-1 -p "read:fx/objstm.pdf"
    check "read pdf in zip"   "Second line"                    "$B" -m mock/mock-1 -p "read:fx/docs.zip:papers/simple.pdf"
    check "read notebook"     "# %%% [code] cell:9"            "$B" -m mock/mock-1 -p "read:fx/t.ipynb"
fi
# LSP (only when clangd is installed)
if command -v clangd >/dev/null; then
    printf 'static int add(int a, int b) { return a + b; }\nint main(void) { return add(1, 2); }\n' > l.c
    check "lsp definition"  "l.c:1:12"  "$B" -m mock/mock-1 -p 'call:lsp:{"action":"definition","file":"l.c","line":2,"symbol":"add"}'
    check "lsp write diags" "undeclared identifier"  "$B" -m mock/mock-1 -p 'write:bad.c:int main(void) { return nope; }'
fi
exit $fail
