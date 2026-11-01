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
check "write tool"     "Successfully wrote 14 bytes to a/b.txt" "$B" -m mock/mock-1 -p "write:a/b.txt:written by omc"
check "file content"   "written by omc"                  cat a/b.txt
check "read hashline"  "1:written by omc"                "$B" -m mock/mock-1 -p "read:a/b.txt"
# intent field: injected (required) into tool schemas, stripped before execution
check "intent schema"  '"i": {"type": "string", "description": "concise intent"}'  bash -c "python3 -c 'import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; t=[x for x in r if x.get(\"tools\")][-1][\"tools\"][0][\"function\"][\"parameters\"]; print(json.dumps(t[\"properties\"][\"i\"]) if \"i\" in t[\"required\"] else \"no i\")' '$T/req.jsonl' | sed 's/^/\"i\": /'"
check "intent stripped" "Tool said: hi-intent"           "$B" -m mock/mock-1 -p 'call:bash:{"i":"Checking echo","command":"echo hi-intent"}'
# xd:// devices: discoverable tools are mounted, listed, documented and dispatched through read/write
check "xd listing"     "mounted tool devices"             "$B" -m mock/mock-1 -p "read:xd://"
check "xd docs"        "type Args = {"                    "$B" -m mock/mock-1 -p "read:xd://debug"
check "xd dispatch"    "Invalid args for xd://lsp: Validation failed" "$B" -m mock/mock-1 -p 'call:write:{"path":"xd://lsp","content":"{\"action\":\"bogus\"}"}'
check "xd report"      "Noted, thanks!"                   "$B" -m mock/mock-1 -p 'call:write:{"path":"xd://report_issue","content":"read: wrong output"}'
check "glob tool"      $'Tool said: # a/\nb.txt'          "$B" -m mock/mock-1 -p "glob:**/*.txt"
check "grep tool"      "*1:written by omc"               "$B" -m mock/mock-1 -p "grep:written"
# grep / glob result text as omp words it (grouped path tree, errors, empty results)
check "grep grouped"   $'Tool said: # a/\n## b.txt#'      "$B" -m mock/mock-1 -p "grep:written"
check "grep file"      $'[a/b.txt#'                      "$B" -m mock/mock-1 -p 'call:grep:{"pattern":"written","path":"a/b.txt:1-5"}'
check "grep no match"  "Tool said: No matches found"     "$B" -m mock/mock-1 -p 'call:grep:{"pattern":"zzq_none"}'
check "grep missing"   "Path not found: nope/x"          "$B" -m mock/mock-1 -p 'call:grep:{"pattern":"x","path":"nope/x"}'
check "grep partial"   "Skipped missing paths: nope"     "$B" -m mock/mock-1 -p 'call:grep:{"pattern":"written","path":"a;nope"}'
check "glob none"      "No files found matching pattern" "$B" -m mock/mock-1 -p 'call:glob:{"path":"*.zzq"}'
check "glob limit"     "Requested limit 500 clamped to the max of 200" "$B" -m mock/mock-1 -p 'call:glob:{"path":"a","limit":500}'
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
check "edit stale tag" "is not from this session."  "$B" -m mock/mock-1 -p "edit:[e.txt#$TAG]\nPUT 1.=1:\n+x"
# hashline engine messages (omp pi-edit wording): no-op, header-only, parse errors, unseen-line reveal, blocks
TAG=$(timeout 30 "$B" -m mock/mock-1 -p "read:e.txt" </dev/null | grep -o 'e.txt#[0-9A-F]*' | head -1 | cut -d'#' -f2)
check "edit no-op"     "parsed and applied cleanly, but produced no change"  "$B" -m mock/mock-1 -p "edit:[e.txt#$TAG]\nPUT 1.=1:\n+alpha"
check "edit hdr only"  "no ops follow the header; put them under this file's current tag, #$TAG."  "$B" -m mock/mock-1 -p "edit:[e.txt#$TAG]"
check "edit no header" 'input must begin with "[PATH#HASH]" on the first non-blank line'  "$B" -m mock/mock-1 -p "edit:PUT 1.=1:\n+x"
check "edit bare row"  'line 1: payload line has no preceding hunk header. Use `PUT N.=M:`'  "$B" -m mock/mock-1 -p "edit:[e.txt#$TAG]\nfoo"
check "edit range"     "Line 9 does not exist (file has 5 lines)"  "$B" -m mock/mock-1 -p "edit:[e.txt#$TAG]\nPUT 9.=9:\n+x"
seq 1 30 > big.txt
BTAG=$(timeout 30 "$B" -m mock/mock-1 -p "read:big.txt" </dev/null | grep -o 'big.txt#[0-9A-F]*' | head -1 | cut -d'#' -f2)
check "edit unseen"    "This edit anchors to lines 20 of big.txt that [big.txt#$BTAG] never displayed"  "$B" -m mock/mock-1 --mode json -p "calls:[[\"read\",{\"path\":\"big.txt:1-3\"}],[\"edit\",{\"input\":\"[big.txt#$BTAG]\\nPUT 20.=20:\\n+X\"}]]"
printf 'int f(void) {\n    return 1;\n}\n' > blk.c
CTAG=$(timeout 30 "$B" -m mock/mock-1 -p "read:blk.c" </dev/null | grep -o 'blk.c#[0-9A-F]*' | head -1 | cut -d'#' -f2)
check "edit block"     "PUT 1*: → resolved lines 1-3 (3 lines)"  "$B" -m mock/mock-1 -p "edit:[blk.c#$CTAG]\nPUT 1*:\n+int f(void) { return 2; }"
printf 'a\r\nb\r\n' > crlf.txt
check "write crlf"     "Note: kept the replaced file's CRLF line endings."  "$B" -m mock/mock-1 -p 'call:write:{"path":"crlf.txt","content":"x\ny\n"}'
check "write no content" "content is required for nc.txt."  "$B" -m mock/mock-1 -p 'call:write:{"path":"nc.txt"}'
check "write eisdir"   "EISDIR: illegal operation on a directory, open '"  "$B" -m mock/mock-1 -p "write:a:x"
# edit modes: apply_patch via config.yml (edit.mode), patch / replace / sloppy via PI_EDIT_VARIANT
printf 'edit:\n  mode: apply_patch\n' > "$T/agent/config.yml"
printf 'one\ntwo\nthree\n' > ap.txt; printf 'bye\n' > ap-gone.txt
check "apply_patch run" "[ap-moved.txt]"  "$B" -m mock/mock-1 -p 'call:edit:{"input":"*** Begin Patch\n*** Update File: ap.txt\n*** Move to: ap-moved.txt\n@@\n one\n-two\n+TWO\n*** Delete File: ap-gone.txt\n*** Add File: ap-new.txt\n+fresh\n*** End Patch"}'
check "apply_patch out" $'one\nTWO\nthree'  cat ap-moved.txt
check "apply_patch add" "fresh"  cat ap-new.txt
[[ ! -e ap.txt && ! -e ap-gone.txt ]] && echo "   ok  apply_patch move/delete" || { echo "   FAIL apply_patch move/delete"; fail=1; }
grep -q 'stripped-down, file-oriented diff' "$T/req.jsonl" && echo "   ok  apply_patch prompt" || { echo "   FAIL apply_patch prompt"; fail=1; }
check "apply_patch err" "Failed to find expected lines in ap-moved.txt"  "$B" -m mock/mock-1 -p 'call:edit:{"input":"*** Begin Patch\n*** Update File: ap-moved.txt\n@@\n-missing\n+x\n*** End Patch"}'
rm -f "$T/agent/config.yml"
printf 'def greet():\n    print("Hi")\n' > pm.py
check "patch mode"      '2:    print("Hello")'  env PI_EDIT_VARIANT=patch "$B" -m mock/mock-1 -p 'call:edit:{"path":"pm.py","edits":[{"op":"update","diff":"@@ def greet():\n def greet():\n-    print(\"Hi\")\n+    print(\"Hello\")\n"}]}'
check "patch result"    'print("Hello")'  cat pm.py
printf 'alpha = 1\nbeta = 2\n' > rp.txt
check "replace mode"    "[rp.txt]"  env PI_EDIT_VARIANT=replace "$B" -m mock/mock-1 -p 'call:edit:{"path":"rp.txt","old_string":"beta = 2","new_string":"beta = 3"}'
check "replace result"  "beta = 3"  cat rp.txt
check "replace error"   "Could not find a close enough match in rp.txt"  env PI_EDIT_VARIANT=replace "$B" -m mock/mock-1 -p 'call:edit:{"path":"rp.txt","old_string":"gamma = 9","new_string":"x"}'
printf 'start();\nconst timeout = 1000;\nfinish();\n' > sl.ts
check "sloppy mode"     "[sl.ts]"  env PI_EDIT_VARIANT=sloppy "$B" -m mock/mock-1 -p 'call:edit:{"input":"*** Edit File: sl.ts\n*** Find\nconst timeout = 1000;\n*** Replace\nconst timeout = 5000;\n*** Find\nfinish();\n*** Insert Before\nlog();\n"}'
check "sloppy result"   $'start();\nconst timeout = 5000;\nlog();\nfinish();'  cat sl.ts
check "todo tool"      "Remaining items (2)"  "$B" -m mock/mock-1 -p "todo:first,second"
# tool output parity (omp OutputSink): middle elision + session artifact, column cap, sanitizing, exit notice
LASTMSG='import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; m=[x for x in r[-1]["messages"] if x.get("role")=="tool"][-1]; c=m["content"]; print(c if isinstance(c,str) else "".join(b.get("text","") for b in c), end="")'
"$B" -m mock/mock-1 --session-dir "$T/sink" -p "bash:seq 1 30000" >/dev/null 2>&1 </dev/null
check "sink elision"   "[Showing lines 1-4720 and 25283-30001 of 30001; 20,562 middle lines (114.9KB) elided. Read artifact://0 for full output]"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
check "sink marker"    "4317"$'\n'"43"$'\n'"[…20562ln elided…]"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
check "sink artifact"  "168894"  bash -c "wc -c < \"\$(ls $T/sink/*/0.bash.log)\""
"$B" -m mock/mock-1 --session-dir "$T/sink" -c -p "read:artifact://0:raw:29999-29999" >/dev/null 2>&1 </dev/null
check "artifact read"  "29999"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" -m mock/mock-1 --no-session -p "bash:python3 -c \"print('x'*2000)\"; printf 'a\\033[31mb\\rc\\n'; exit 3" >/dev/null 2>&1 </dev/null
check "sink columns"   "…"$'\n'"ab"$'\n'"c"$'\n\n\n'"Command exited with code 3"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
check "sink col note"  "[Some lines truncated to 768 bytes]"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
# tool result text parity with omp: argument validation (omptype messages, coercion, received args), todo/task
# errors, bash without bash's job-status notices
"$B" -m mock/mock-1 --no-session -p 'call:grep:{}' >/dev/null 2>&1 </dev/null
check "args missing"   "  - pattern: pattern must be a string (was missing)"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" -m mock/mock-1 --no-session -p 'call:grep:{"pattern":true,"case":3}' >/dev/null 2>&1 </dev/null
check "args coerced"   $'  - case: case must be boolean (was a number)\n\nReceived arguments:\n{\n  "original": {\n    "pattern": true,'  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" -m mock/mock-1 --no-session -p 'call:todo:{"op":"bogus"}' >/dev/null 2>&1 </dev/null
check "todo bad op"    'Invalid todo arguments: op must be "init", "start", "done", "rm", "drop", "block", "unblock", "append" or "view" (was "bogus")'  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" -m mock/mock-1 --no-session -p 'seq:[["todo",{"op":"init","items":["a"]}],["todo",{"op":"start","task":"nope"}]]' >/dev/null 2>&1 </dev/null
check "todo errors"    $'Errors: Task "nope" not found\nRemaining items (1):'  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" -m mock/mock-1 --no-session -p 'call:task:{}' >/dev/null 2>&1 </dev/null
check "task shape"     'Missing `tasks`. Provide a `tasks` array (one subagent per item) with a shared `context`.'  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" -m mock/mock-1 --no-session -p "bash:sh -c 'kill -9 \$\$'; printf 'x'; sh -c 'kill -SEGV \$\$'; echo after-kill" >/dev/null 2>&1 </dev/null
check "bash no notice" "xafter-kill"  python3 -I -c "$LASTMSG; import re; sys.exit(1) if re.search('Killed|Segmentation', c) else None" "$T/req.jsonl"
# skills + internal URLs + context files
mkdir -p "$T/agent/skills/demo"
printf -- '---\nname: demo\ndescription: Demo skill for tests\n---\nSKILL BODY LINE\n' > "$T/agent/skills/demo/SKILL.md"
printf 'Project rule: always test.\n' > AGENTS.md
check "skill url"      "SKILL BODY LINE"  "$B" -m mock/mock-1 -p "read:skill://demo"
check "unknown skill"  "Unknown skill"    "$B" -m mock/mock-1 -p "read:skill://nope"
grep -q "Demo skill for tests" "$T/req.jsonl" && echo "   ok  skill in system prompt" || { echo "   FAIL skill in system prompt"; fail=1; }
sleep 1; "$B" -m mock/mock-1 -p "hi again" >/dev/null 2>&1 </dev/null
tail -1 "$T/req.jsonl" | grep -q "Use for demo skill test routing" && echo "   ok  skill hint compressed" || { echo "   FAIL skill hint compressed"; fail=1; }
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
# MCP OAuth: discovery -> dynamic registration -> PKCE code (curl plays the browser) -> bearer token
OPORT=$((PORT + 1)); python3 -I "$ROOT/tests/e2e/mock_mcp_oauth.py" $OPORT & OPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$OPORT) 2>/dev/null && break; sleep 0.1; done
printf '{"mcpServers":{"oa":{"type":"http","url":"http://127.0.0.1:%s/mcp"}}}' $OPORT > "$T/agent/mcp.json"
check "mcp oauth needed" "omc mcp login oa"  bash -c "'$B' -m mock/mock-1 -p 'say:x' >/dev/null; OMC_LOG=/dev/stderr '$B' -m mock/mock-1 -p 'call:mcp__oa_whoami:{}' 2>&1 | grep -o 'omc mcp login oa' | head -1"
( sleep 20 | OMC_NO_BROWSER=1 "$B" mcp login oa > "$T/mcplogin.log" 2>&1 ) & LPID=$!
for _ in $(seq 100); do grep -q "authorize?" "$T/mcplogin.log" 2>/dev/null && break; sleep 0.1; done
curl -s -L -o /dev/null "$(grep -o 'http://127.0.0.1:[0-9]*/authorize?[^ ]*' "$T/mcplogin.log" | head -1)"
for _ in $(seq 100); do kill -0 $LPID 2>/dev/null || break; sleep 0.1; done
check "mcp oauth login" "Authorized oa"  cat "$T/mcplogin.log"
check "mcp oauth tool"  "authorized as tok-1"  "$B" -m mock/mock-1 -p 'call:mcp__oa_whoami:{}'
check "mcp oauth list"  "[oauth: logged in]"  "$B" mcp list
python3 -I -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); [v.update(expires=1) for k,v in d.items() if k.startswith("mcp:")]; json.dump(d,open(p,"w"))' "$T/agent/auth.json"
check "mcp oauth refresh" "authorized as tok-2"  "$B" -m mock/mock-1 -p 'call:mcp__oa_whoami:{}'
kill $OPID $LPID 2>/dev/null; rm -f "$T/agent/mcp.json"
# persistent bash: state survives across calls in one session
check "bash persists"  "FOO=kept"  bash -c "( printf '%s\n' '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"bash:export FOO=kept\"}'; sleep 2; printf '%s\n' '{\"id\":\"2\",\"type\":\"prompt\",\"message\":\"bash:echo FOO=\$FOO\"}'; sleep 2 ) | '$B' -m mock/mock-1 --mode rpc"
check "bash exit code" "exited with code 3"  "$B" bash 'exit 3'
# plan mode: read-only gating, local:// plan, xd://propose approval (auto-approved without a UI)
check "plan blocks"    "working tree is read-only"  "$B" -m mock/mock-1 --plan-mode -p "write:real.txt:x"
check "plan approve"   "approved. Plan mode ended"  bash -c "( printf '%s\n' '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"call:write:{\\\"path\\\":\\\"local://p-plan.md\\\",\\\"content\\\":\\\"# P\\\"}\"}'; sleep 2; printf '%s\n' '{\"id\":\"2\",\"type\":\"prompt\",\"message\":\"call:write:{\\\"path\\\":\\\"xd://propose\\\",\\\"content\\\":\\\"p\\\"}\"}'; sleep 3 ) | '$B' -m mock/mock-1 --plan-mode --mode rpc"
# eval (python kernel) when python3 is available
if command -v python3 >/dev/null; then
    check "eval python"    "Tool said: 42"  "$B" -m mock/mock-1 -p 'call:eval:{"language":"py","code":"6*7"}'
    # eval helpers: completion/wait, judge, judge_batch (drain/iter/attach/cancel/min_ok), budget, timeouts, xd://eval docs
    python3 -I "$ROOT/tests/e2e/eval_client.py" "$B" || fail=1
fi
# checkpoint / rewind (off by default: checkpoint.enabled)
printf "checkpoint:\n  enabled: true\n" > "$T/agent/config.yml"
check "rewind"         "Rewinding to checkpoint"  bash -c "( for m in 'call:checkpoint:{\"goal\":\"g\"}' 'read:a/b.txt' 'call:rewind:{\"report\":\"done\"}'; do python3 -c 'import json,sys; print(json.dumps({\"id\":\"x\",\"type\":\"prompt\",\"message\":sys.argv[1]}))' \"\$m\"; sleep 2; done ) | '$B' -m mock/mock-1 --mode rpc"
rm -f "$T/agent/config.yml"
# ast_grep structural search (off by default: astGrep.enabled)
printf "astGrep:\n  enabled: true\n" > "$T/agent/config.yml"
printf 'function f(a) {\n  console.log(a, 1);\n  if (q == q) {}\n}\n' > s.ts
check "ast_grep"       '$A = q'  "$B" -m mock/mock-1 -p 'call:ast_grep:{"pat":"$A == $A","path":"s.ts"}'
rm -f "$T/agent/config.yml"
# find: jfind cascade through the judge role (off by default: find.enabled)
printf "find:\n  enabled: on\n" > "$T/agent/config.yml"
mkdir -p fnd && printf 'int parse_token(const char *raw) {\n  return raw[0];\n}\n' > fnd/tok.c && printf 'int other(void) { return 1; } /* FAILJUDGE */\n' > fnd/fail.c
check "find hits"      $'1 hit(s) for "where are tokens parsed" in fnd/ (\xcf\x84 0.20), strongest first\npartial: 2 of 4 judge requests failed; unjudged files may still match\n\nfnd/tok.c  1.00  3 lines judged\n  fnd/tok.c:1-3  1.00  int parse_token(const char *raw) {\n\nlisted 2 \xc2\xb7 judged 2 \xc2\xb7 read 2 files' "$B" -m mock/mock-1 -p 'call:find:{"query":"where are tokens parsed","grep_keywords":["parse_token"],"path":"fnd"}'
check "find empty"     '`query` must be a non-empty description'  "$B" -m mock/mock-1 -p 'call:find:{"query":"  ","grep_keywords":[]}'
rm -f "$T/agent/config.yml"
check "ast_edit stage" "(proposed) 1 replacement" "$B" -m mock/mock-1 -p 'call:ast_edit:{"ops":[{"pat":"console.log($$$X)","out":"log($$$X)"}],"paths":["s.ts"]}'
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
check "ssh write"      "Successfully wrote 7 bytes to ssh://box"    env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" -m mock/mock-1 -p "call:write:{\"path\":\"ssh://box$T/sub/w.txt\",\"content\":\"via ssh\"}"
check "ssh write file" "via ssh"            cat "$T/sub/w.txt"
check "ssh replace"    "[ssh://box"  env PI_EDIT_VARIANT=replace PATH="$ROOT/tests/e2e/bin:$PATH" "$B" -m mock/mock-1 -p "call:edit:{\"path\":\"ssh://box$T/sub/w.txt\",\"old_string\":\"via ssh\",\"new_string\":\"replaced over ssh\"}"
check "ssh replace file" "replaced over ssh"  cat "$T/sub/w.txt"
check "ssh override"   "overrides are not allowed"  env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" -m mock/mock-1 -p "read:ssh://me@box/etc/hosts"
rm -f ssh.json
# goal mode over RPC: hidden context, auto-continuation (stops on no progress), completion, resume pauses
printf 'goal:\n  continuationModes: ["rpc"]\n' > "$T/agent/config.yml"
check "goal continue"  '"customType":"goal-continuation"'  bash -c "( echo '{\"id\":\"g\",\"type\":\"goal\",\"op\":\"create\",\"objective\":\"Ship it\"}'; sleep 0.5; echo '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"bash:echo step\"}'; sleep 4 ) | '$B' -m mock/mock-1 --mode rpc"
check "goal resumed paused" '"status":"paused"'  bash -c "( echo '{\"id\":\"s\",\"type\":\"goal\",\"op\":\"get\"}'; sleep 1 ) | '$B' -m mock/mock-1 -c --mode rpc"
check "goal complete"  'Status: complete'  bash -c "( echo '{\"id\":\"r\",\"type\":\"goal\",\"op\":\"resume\"}'; sleep 0.5; echo '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"call:goal:{\\\"op\\\":\\\"complete\\\"}\"}'; sleep 3 ) | '$B' -m mock/mock-1 -c --mode rpc"
rm -f "$T/agent/config.yml"
# advisor: shadow model reviews the turn, its concern continues the run as an <advisory>
printf 'advisor:\n  enabled: true\n' > "$T/agent/config.yml"
check "advisor note"   "acknowledged advisory"  "$B" -m mock/mock-1 --no-session -p "bash:echo ADVISE_ME"
rm -f "$T/agent/config.yml"
# RPC: host-implemented tools, tree navigation, paging
check "rpc host tool"  "Tool said: host says hi"  python3 -I "$ROOT/tests/e2e/rpc_client.py" "$B"
check "rpc navigate"   '"editorText": "second"'   python3 -I "$ROOT/tests/e2e/rpc_client.py" "$B"
# async jobs: background bash delivered as a follow-up; wait with nothing running returns at once
: > "$T/req.jsonl"
check "async bash"     "Backgrounded as job bg_1"  bash -c "'$B' -m mock/mock-1 --mode json -p 'call:bash:{\"command\":\"sleep 1; echo bgdone\",\"async\":true}' | grep -o 'Backgrounded as job bg_1'"
grep -q "Background job bg_1 has completed" "$T/req.jsonl" && echo "   ok  async delivered" || { echo "   FAIL async delivered"; fail=1; }
check "wait nothing"   "Nothing to wait for"  "$B" -m mock/mock-1 -p 'call:wait:{}'
# automatic session title: fork side request returns a card title
check "fork title"     "🧪 PARK: Fix flaky park tests"  bash -c "env -u OMC_NO_TITLE '$B' -m mock/mock-1 -p 'bash:echo titled' >/dev/null && '$B' sessions | head -3"
# omc migrate: a synthetic omp agent dir (SQLite credential/history stores built here, fake secrets)
OMPD="$T/ompdir"; mkdir -p "$OMPD/skills/demo" "$OMPD/sessions/--x--" "$OMPD/blobs"
printf 'theme:\n  dark: amethyst\n' > "$OMPD/config.yml"; printf '{"mcpServers":{}}' > "$OMPD/mcp.json"
printf -- '---\nname: demo\ndescription: d\n---\nbody\n' > "$OMPD/skills/demo/SKILL.md"; echo '{}' > "$OMPD/sessions/--x--/s.jsonl"; echo junk > "$OMPD/blobs/b"
python3 -I - "$OMPD" <<'PY'
import sqlite3, sys, json
d = sys.argv[1]
a = sqlite3.connect(d + "/agent.db")
a.execute("CREATE TABLE auth_credentials (id INTEGER PRIMARY KEY AUTOINCREMENT, provider TEXT NOT NULL, credential_type TEXT NOT NULL, data TEXT NOT NULL, disabled_cause TEXT DEFAULT NULL, identity_key TEXT DEFAULT NULL, created_at INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL DEFAULT 0)")
a.execute("INSERT INTO auth_credentials (provider, credential_type, data, updated_at) VALUES ('openrouter','api_key',?,1)", (json.dumps({"key": "sk-old"}),))
a.execute("INSERT INTO auth_credentials (provider, credential_type, data, updated_at) VALUES ('openrouter','api_key',?,2)", (json.dumps({"key": "sk-new"}),))
a.execute("INSERT INTO auth_credentials (provider, credential_type, data, updated_at, disabled_cause) VALUES ('xai','api_key',?,3,'revoked')", (json.dumps({"key": "x"}),))
a.execute("INSERT INTO auth_credentials (provider, credential_type, data, updated_at) VALUES ('anthropic','oauth',?,1)", (json.dumps({"access": "at", "refresh": "rt", "expires": 1}),))
a.commit()
h = sqlite3.connect(d + "/history.db")
h.execute("CREATE TABLE history (id INTEGER PRIMARY KEY AUTOINCREMENT, prompt TEXT NOT NULL UNIQUE, created_at INTEGER NOT NULL DEFAULT 0, cwd TEXT, session_id TEXT, use_count INTEGER NOT NULL DEFAULT 1)")
h.executemany("INSERT INTO history (prompt, created_at, cwd) VALUES (?,?,?)", [("second", 20, "/w"), ("first", 10, "/w")])
h.commit()
PY
MIG="$T/migagent"
check "migrate dry run"   "credentials: would import 2 (anthropic, openrouter)"  env OMC_CODING_AGENT_DIR="$MIG" "$B" migrate --from "$OMPD" --dry-run
check "migrate run"       "prompt history: imported 2 entries"  env OMC_CODING_AGENT_DIR="$MIG" "$B" migrate --from "$OMPD"
check "migrate config"    "amethyst"   cat "$MIG/config.yml"
check "migrate skill"     "body"       cat "$MIG/skills/demo/SKILL.md"
check "migrate creds"     '"key": "sk-new"'  cat "$MIG/auth.json"
check "migrate oauth"     '"type": "oauth"'  cat "$MIG/auth.json"
check "migrate history"   $'"text":"first"'  head -1 "$MIG/prompt-history.jsonl"
check "migrate no blobs"  "absent"     bash -c "[ -e '$MIG/blobs' ] && echo present || echo absent"
check "migrate idempotent" "kept omc's existing anthropic, openrouter"  env OMC_CODING_AGENT_DIR="$MIG" "$B" migrate --from "$OMPD"
# libomc embedding (built by `make test` via `make lib`)
if [ -x "$ROOT/build/examples/embed" ]; then
    check "lib host tool"  "hello from the host app, Ada"  "$ROOT/build/examples/embed" 'call:greet:{"name":"Ada"}' mock/mock-1
    check "lib rpc send"   '"command":"get_state","success":true'  "$ROOT/build/examples/embed" 'hi' mock/mock-1
fi
# ACP (Agent Client Protocol): permission request answered by the test client
check "acp prompt"     "tool_call_update completed acp-ok"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "bash:echo acp-ok"
check "acp reject"     "tool_call_update failed The user rejected"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "bash:echo no" reject_once
# Amazon Bedrock ConverseStream against a mock that verifies SigV4 independently and speaks event-stream
BPORT=$((PORT + 1))
MOCK_LOG="$T/bedrock.jsonl" MOCK_BEDROCK_BEARER=br-token python3 -I "$ROOT/tests/e2e/mock_bedrock.py" $BPORT & BPID=$!
trap 'kill $MPID $BPID 2>/dev/null; rm -rf "$T"' EXIT
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$BPORT) 2>/dev/null && break; sleep 0.1; done
cp "$T/agent/models.yml" "$T/models.yml.bak"
printf '  amazon-bedrock:\n    baseUrl: http://127.0.0.1:%s\n' $BPORT >> "$T/agent/models.yml"
BM=amazon-bedrock/us.anthropic.claude-sonnet-4-5-20250929-v1:0
BENV=(env -u AWS_PROFILE -u AWS_SESSION_TOKEN -u AWS_BEARER_TOKEN_BEDROCK -u AWS_DEFAULT_REGION -u AWS_WEB_IDENTITY_TOKEN_FILE \
    -u AWS_CONTAINER_CREDENTIALS_RELATIVE_URI -u AWS_CONTAINER_CREDENTIALS_FULL_URI -u AWS_ACCESS_KEY_ID -u AWS_SECRET_ACCESS_KEY \
    HOME="$T" AWS_REGION=us-west-2 AWS_EC2_METADATA_DISABLED=true AWS_CONFIG_FILE="$T/none" AWS_SHARED_CREDENTIALS_FILE="$T/none")
check "bedrock models"  "* amazon-bedrock"      "${BENV[@]}" AWS_ACCESS_KEY_ID=AKIDMOCK AWS_SECRET_ACCESS_KEY=mock-secret "$B" models sonnet-4-5
check "bedrock text"    "plain bedrock answer"  "${BENV[@]}" AWS_ACCESS_KEY_ID=AKIDMOCK AWS_SECRET_ACCESS_KEY=mock-secret "$B" -m "$BM" --no-session -p "hi"
check "bedrock tool"    "Tool said: br-e2e"     "${BENV[@]}" AWS_ACCESS_KEY_ID=AKIDMOCK AWS_SECRET_ACCESS_KEY=mock-secret "$B" -m "$BM" --no-session -p "bash:echo br-e2e"
check "bedrock bearer"  "plain bedrock answer"  "${BENV[@]}" AWS_BEARER_TOKEN_BEDROCK=br-token "$B" -m "$BM" --no-session -p "hi"
check "bedrock bad sig" "Bedrock HTTP 403: The request signature we calculated does not match"  "${BENV[@]}" AWS_ACCESS_KEY_ID=AKIDMOCK AWS_SECRET_ACCESS_KEY=wrong "$B" -m "$BM" --no-session -p "hi"
check "bedrock no creds" "Unable to resolve AWS credentials"  "${BENV[@]}" "$B" -m "$BM" --no-session -p "hi"
grep -q '"path": "/model/us.anthropic.claude-sonnet-4-5-20250929-v1%3A0/converse-stream"' "$T/bedrock.jsonl" && echo "   ok  bedrock request path" || { echo "   FAIL bedrock request path"; fail=1; }
grep -q '"toolResult": {"toolUseId": "tooluse_mock1"' "$T/bedrock.jsonl" && echo "   ok  bedrock toolResult replay" || { echo "   FAIL bedrock toolResult replay"; fail=1; }
cp "$T/models.yml.bak" "$T/agent/models.yml"
# Google: Vertex AI via Application Default Credentials (service-account RS256 JWT -> token endpoint) and Cloud Code
# Assist (google-gemini-cli / google-antigravity) with a stored OAuth credential, against a local mock; no real creds.
GPORT=$((PORT + 2))
MOCK_LOG="$T/google.jsonl" python3 -I "$ROOT/tests/e2e/mock_google.py" $GPORT & GPID=$!
trap 'kill $MPID $BPID $GPID 2>/dev/null; rm -rf "$T"' EXIT
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$GPORT) 2>/dev/null && break; sleep 0.1; done
printf '  google-vertex:\n    baseUrl: http://127.0.0.1:%s\n  google-gemini-cli:\n    baseUrl: http://127.0.0.1:%s\n  google-antigravity:\n    baseUrl: http://127.0.0.1:%s\n' \
    $GPORT $GPORT $GPORT >> "$T/agent/models.yml"
GENV=(env -u GOOGLE_APPLICATION_CREDENTIALS -u GOOGLE_CLOUD_ACCESS_TOKEN -u CLOUDSDK_AUTH_ACCESS_TOKEN -u GOOGLE_CLOUD_API_KEY \
    -u GOOGLE_CLOUD_PROJECT -u GCP_PROJECT -u GCLOUD_PROJECT -u GOOGLE_VERTEX_LOCATION -u GOOGLE_CLOUD_LOCATION -u VERTEX_LOCATION \
    HOME="$T" GCE_METADATA_HOST=127.0.0.1:1 PI_AI_ANTIGRAVITY_VERSION=2.26.0)
if openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$T/sa.pem" 2>/dev/null; then
    python3 -I -c 'import json,sys; print(json.dumps({"type":"service_account","client_email":"sa@mock.iam.gserviceaccount.com","private_key":open(sys.argv[1]).read(),"private_key_id":"k1","token_uri":"http://127.0.0.1:%s/token" % sys.argv[2]}))' "$T/sa.pem" $GPORT > "$T/sa.json"
    VENV=("${GENV[@]}" GOOGLE_APPLICATION_CREDENTIALS="$T/sa.json" GOOGLE_CLOUD_PROJECT=proj-v GOOGLE_CLOUD_LOCATION=us-central1)
    check "vertex models"     "* google-vertex"       "${VENV[@]}" "$B" models gemini-3.1-pro-preview
    check "vertex adc text"   "plain google answer"   "${VENV[@]}" "$B" -m google-vertex/gemini-3.1-pro-preview --no-session -p "hi"
    check "vertex adc tool"   "Tool said: vx-e2e"     "${VENV[@]}" "$B" -m google-vertex/gemini-3.1-pro-preview --no-session -p "bash:echo vx-e2e"
    check "vertex no project" "Vertex AI requires a project ID"  "${GENV[@]}" GOOGLE_APPLICATION_CREDENTIALS="$T/sa.json" "$B" -m google-vertex/gemini-3.1-pro-preview --no-session -p "hi"
    grep -q '"path": "/v1/projects/proj-v/locations/us-central1/publishers/google/models/gemini-3.1-pro-preview:streamGenerateContent?alt=sse", "ua": "[^"]*", "auth": "Bearer ya29.mock-sa"' "$T/google.jsonl" \
        && echo "   ok  vertex request path" || { echo "   FAIL vertex request path"; fail=1; }
fi
check "vertex no adc"     "  google-vertex"      "${GENV[@]}" "$B" models gemini-3.1-pro-preview
cp "$T/agent/auth.json" "$T/auth.json.bak" 2>/dev/null
FAR=$(( $(date +%s) * 1000 + 3600000 ))
printf '{"google-gemini-cli":{"type":"oauth","access":"cca-token","refresh":"r1","expires":%s,"projectId":"proj-cca","email":"me@example.com"},"google-antigravity":{"type":"oauth","access":"cca-token","refresh":"r2","expires":%s,"projectId":"proj-cca"}}' \
    $FAR $FAR > "$T/agent/auth.json"
check "gemini-cli models" "* google-gemini-cli"  "${GENV[@]}" "$B" models gemini-2.5-pro
check "gemini-cli text"   "plain cca answer"     "${GENV[@]}" "$B" -m google-gemini-cli/gemini-2.5-pro --no-session -p "hi"
check "gemini-cli tool"   "Tool said: gc-e2e"    "${GENV[@]}" "$B" -m google-gemini-cli/gemini-2.5-pro --no-session -p "bash:echo gc-e2e"
check "antigravity claude" "plain cca answer"    "${GENV[@]}" "$B" -m google-antigravity/claude-opus-4-6 --no-session -p "hi"
check "antigravity tool"  "Tool said: ag-e2e"    "${GENV[@]}" "$B" -m google-antigravity/gemini-3.1-pro --no-session -p "bash:echo ag-e2e"
grep -q '"ua": "GeminiCLI/0.46.0/gemini-2.5-pro (' "$T/google.jsonl" && echo "   ok  gemini-cli client headers" || { echo "   FAIL gemini-cli client headers"; fail=1; }
grep -q '"ua": "antigravity/hub/2.26.0 (aidev_client; .*"model": "claude-opus-4-6-thinking", "userAgent": "antigravity"' "$T/google.jsonl" \
    && echo "   ok  antigravity envelope" || { echo "   FAIL antigravity envelope"; fail=1; }
grep -q '"functionResponse": {"name": "bash"' "$T/google.jsonl" && echo "   ok  cca functionResponse replay" || { echo "   FAIL cca functionResponse replay"; fail=1; }
if [ -f "$T/auth.json.bak" ]; then mv "$T/auth.json.bak" "$T/agent/auth.json"; else rm -f "$T/agent/auth.json"; fi
# Cursor agent (Connect + protobuf over HTTP/1.1: RunSSE stream + BidiAppend unary appends, exec channel, KV blobs)
CPORT=$((PORT + 3))
MOCK_LOG="$T/cursor.jsonl" MOCK_CURSOR_TOKEN=cur-token python3 -I "$ROOT/tests/e2e/mock_cursor.py" $CPORT & CPID=$!
trap 'kill $MPID $BPID $GPID $CPID 2>/dev/null; rm -rf "$T"' EXIT
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$CPORT) 2>/dev/null && break; sleep 0.1; done
printf '  cursor:\n    baseUrl: http://127.0.0.1:%s\n' $CPORT >> "$T/agent/models.yml"
CENV=(env HOME="$T" CURSOR_ACCESS_TOKEN=cur-token)
CM=cursor/claude-4.5-sonnet
mkdir -p "$T/cur" && printf 'cursor line one\ncursor line two\n' > "$T/cur/r.txt"
check "cursor text"     "plain cursor answer"   "${CENV[@]}" "$B" -m "$CM" --no-session -p "hi"
check "cursor shell"    "Tool said: cur-e2e"    "${CENV[@]}" "$B" -m "$CM" --session-dir "$T/cur" -p "bash:echo cur-e2e"
check "cursor history"  "user,assistant,tool,assistant"  "${CENV[@]}" "$B" -m "$CM" --session-dir "$T/cur" -c -p "history"
check "cursor read"     "Read said: cursor line one"     "${CENV[@]}" "$B" -m "$CM" --no-session -p "read:$T/cur/r.txt"
check "cursor mcp"      "MCP said: "            "${CENV[@]}" "$B" -m "$CM" --no-session -p 'mcp:glob:{"path":"**/*.txt"}'
check "cursor end err"  "Connect error resource_exhausted: Error"  "${CENV[@]}" "$B" -m "$CM" --no-session -p "fail"
check "cursor retry"    "recovered"             "${CENV[@]}" "$B" -m "$CM" --no-session -p "flaky"
check "cursor 401"      "Cursor RunSSE failed with HTTP 401"  env HOME="$T" CURSOR_ACCESS_TOKEN=wrong "$B" -m "$CM" --no-session -p "hi"
check "cursor effort"   "plain cursor answer"   "${CENV[@]}" "$B" -m cursor/gpt-5.4 --thinking high --no-session -p "hi"
check "cursor json"     '"toolName":"bash"'     "${CENV[@]}" "$B" -m "$CM" --no-session --mode json -p "bash:echo j"
grep -q '"requestedModel": "gpt-5.4", "maxMode": false, "parameters": \[{"id": "reasoning", "value": "high"}\]' "$T/cursor.jsonl" \
    && echo "   ok  cursor effort parameters" || { echo "   FAIL cursor effort parameters"; fail=1; }
grep -q '"modelDetails": "gpt-5.4-high"' "$T/cursor.jsonl" && echo "   ok  cursor model details" || { echo "   FAIL cursor model details"; fail=1; }
grep -q '"clientVersion": "cli-2026.09.02-c22c1a3", "clientType": "cli", "streaming": "true"' "$T/cursor.jsonl" \
    && echo "   ok  cursor client headers" || { echo "   FAIL cursor client headers"; fail=1; }
grep -q '"requestContext": {"tools": \[[^]]*"write"' "$T/cursor.jsonl" && echo "   ok  cursor request context" || { echo "   FAIL cursor request context"; fail=1; }
grep -q '"originalRequestId": "[0-9a-f-]\{36\}"' "$T/cursor.jsonl" && echo "   ok  cursor retry original id" || { echo "   FAIL cursor retry original id"; fail=1; }
# Cursor login hook (deep-control browser login polled with the PKCE verifier) and token refresh, against the mock
CAUTH=(env HOME="$T" OMC_TEST_CURSOR_API="http://127.0.0.1:$CPORT" OMC_NO_BROWSER=1)
check "cursor login"    "Logged in to cursor."  "${CAUTH[@]}" "$B" login cursor
grep -q '"email": "dev@example.com"' "$T/agent/auth.json" && echo "   ok  cursor login email" || { echo "   FAIL cursor login email"; fail=1; }
grep -q '"verifier": 128' "$T/cursor.jsonl" && echo "   ok  cursor login verifier" || { echo "   FAIL cursor login verifier"; fail=1; }
python3 -I -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["cursor"]["expires"]=1; json.dump(d,open(p,"w"))' "$T/agent/auth.json"
check "cursor refresh"  "plain cursor answer"   "${CAUTH[@]}" "$B" -m "$CM" --no-session -p "hi"
grep -q '"refresh": {"grant_type": "refresh_token"' "$T/cursor.jsonl" && echo "   ok  cursor refresh request" || { echo "   FAIL cursor refresh request"; fail=1; }
"$B" logout cursor >/dev/null 2>&1 </dev/null
cp "$T/models.yml.bak" "$T/agent/models.yml"
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
# Memory (memory.backend=local): learn -> learned.md, startup pipeline (phase 1 + 2) -> memory_summary.md / skills,
# memory block in the next system prompt, memory:// reads; auto-learn manage_skill + private capture turn
MA="$T/magent"; mkdir -p "$MA" "$T/mwork"
cp "$T/agent/models.yml" "$MA/models.yml"
printf 'modelRoles:\n  default: mock/mock-1\nmemory:\n  backend: local\nmemories:\n  minRolloutIdleHours: 0\nautolearn:\n  enabled: true\n  autoContinue: true\n  minToolCalls: 1\n' > "$MA/config.yml"
MENV=(env OMC_CODING_AGENT_DIR="$MA")
MROOT="$MA/memories/--$(cd "$T/mwork" && pwd | sed 's#^/##; s#[/:]#-#g')--"
pushd "$T/mwork" >/dev/null
check "learn tool"      "Lesson stored."        "${MENV[@]}" "$B" -m mock/mock-1 -p 'call:learn:{"memory":"Run <make test> before commit; key_ab12cd34ef56gh78","context":"ci"}'
check "learned.md"      "- Run make test before commit; [REDACTED] _(context: ci)_"  cat "$MROOT/learned.md"
check "manage_skill"    'Created managed skill "e2e-skill" (managed-skills/e2e-skill/SKILL.md)'  "${MENV[@]}" "$B" -m mock/mock-1 -p 'call:manage_skill:{"action":"create","name":"e2e-skill","description":"Use for e2e <tests>","body":"Do the thing."}'
check "managed SKILL.md" $'---\nname: e2e-skill\ndescription: Use for e2e tests\n---\n\nDo the thing.'  cat "$MA/managed-skills/e2e-skill/SKILL.md"
check "managed listed"  "Do the thing."         "${MENV[@]}" "$B" -m mock/mock-1 -p "read:skill://e2e-skill"
check "manage dup"      "already exists"        "${MENV[@]}" "$B" -m mock/mock-1 -p 'call:manage_skill:{"action":"create","name":"e2e-skill","description":"d","body":"b"}'
# the previous runs are idle rollouts now: a slow prompt keeps this process alive while startup consolidates them
check "memory startup"  "done"                  "${MENV[@]}" "$B" -m mock/mock-1 -p "slow:10:done"
for _ in $(seq 50); do [ -f "$MROOT/memory_summary.md" ] && break; sleep 0.2; done
check "memory summary"  "MOCK SUMMARY from consolidation"  cat "$MROOT/memory_summary.md"
check "memory skill"    "echo mock"             cat "$MROOT/skills/mock-skill/scripts/run.sh"
check "memory rollout"  "Mock rollout summary." bash -c "cat '$MROOT'/rollout_summaries/*-mock_slug.md"
check "memory raw"      "[REDACTED]"            cat "$MROOT/raw_memories.md"
check "memory evil path" "absent"               bash -c "[ -e '$MROOT/skills/evil' ] || [ -e '$MROOT/skills/mock-skill/evil' ] && echo present || echo absent"
check "memory read url" "MOCK SUMMARY"          "${MENV[@]}" "$B" -m mock/mock-1 -p "read:memory://root"
check "memory url trav" "not allowed"           "${MENV[@]}" "$B" -m mock/mock-1 -p "read:memory://root/../x"
"${MENV[@]}" "$B" -m mock/mock-1 -p "hello memory" </dev/null >/dev/null 2>&1
python3 -I -c '
import json,sys
rs=[json.loads(l) for l in open(sys.argv[1])]
r=[x for x in rs if x.get("tools") and "hello memory" in json.dumps(x["messages"][-1])][-1]
s=r["messages"][0]["content"]; s=s if isinstance(s,str) else "".join(p.get("text","") for p in s)
ok="Memory summary:\nMOCK SUMMARY from consolidation" in s and "Learned lessons" in s and "## Auto-Learn (experimental)" in s and "`memory://root`" in s
print("ok" if ok else "missing memory block")' "$T/req.jsonl" | grep -q '^ok$' && echo "   ok  memory prompt block" || { echo "   FAIL memory prompt block"; fail=1; }
# auto-continue capture: one tool call reaches minToolCalls; rpc mode keeps the session alive for the private turn
printf '%s\n' '{"id":"1","type":"prompt","message":"bash:echo cap"}' | (cat; sleep 3) | "${MENV[@]}" "$B" -m mock/mock-1 --mode rpc >/dev/null 2>&1
grep -q 'Automated capture turn' "$T/req.jsonl" && echo "   ok  autolearn capture turn" || { echo "   FAIL autolearn capture turn"; fail=1; }
python3 -I -c '
import json,sys
rs=[json.loads(l) for l in open(sys.argv[1])]
c=[x for x in rs if "Automated capture turn" in json.dumps(x.get("messages",[])[-1:])]
print(sorted(t["function"]["name"] for t in c[-1]["tools"]) if c else "none")' "$T/req.jsonl" | grep -q "\['learn', 'manage_skill'\]" && echo "   ok  autolearn capture tools" || { echo "   FAIL autolearn capture tools"; fail=1; }
popd >/dev/null
# LSP (only when clangd is installed)
if command -v clangd >/dev/null; then
    # omp only configures servers whose root markers exist in the cwd (.clangd here)
    mkdir -p lspw && pushd lspw >/dev/null && : > .clangd
    printf 'static int add(int a, int b) { return a + b; }\nint main(void) { return add(1, 2); }\n' > l.c
    lsp() { echo "call:write:{\"path\":\"xd://lsp\",\"content\":$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$1")}"; }
    check "lsp status"      "Language servers: clangd (configured, not started)" "$B" -m mock/mock-1 -p "$(lsp '{"action":"status"}')"
    check "lsp definition"  $'Found 1 definition(s):\n  l.c:1:12\n    1: static int add'  "$B" -m mock/mock-1 -p "$(lsp '{"action":"definition","file":"l.c","line":2,"symbol":"add"}')"
    check "lsp symbols"     $'Symbols in l.c:\n\xe2\x9f\xb2 add int (int, int) @ line 1'  "$B" -m mock/mock-1 -p "$(lsp '{"action":"symbols","file":"l.c"}')"
    check "lsp no symbol"   'LSP error: Symbol "nope" not found on line 2'  "$B" -m mock/mock-1 -p "$(lsp '{"action":"hover","file":"l.c","line":2,"symbol":"nope"}')"
    check "lsp diagnostics" $'1 error(s):\n# bad.c\n  1:25 [error] [clang] Use of undeclared identifier \'nope\' (undeclared_var_use)' \
        bash -c "printf 'int main(void) { return nope; }\n' > bad.c; '$B' -m mock/mock-1 -p '$(lsp '{"action":"diagnostics","file":"bad.c"}')'"
    check "lsp write diags" $'\n\nLSP Diagnostics (1 error(s)):\n# bad2.c\n  1:25 [error] [clang] Use of undeclared identifier'  "$B" -m mock/mock-1 -p 'write:bad2.c:int main(void) { return nope; }'
    popd >/dev/null
fi
# credential shapes from logins -> provider request URL/headers (mock_providers.py)
source "$ROOT/tests/e2e/providers_transport.sh"
source "$ROOT/tests/e2e/providers_devin.sh"
source "$ROOT/tests/e2e/providers_gitlab.sh"
# provider logins over stdin (omc login) against tests/e2e/mock_auth.py
APORT=$((PORT + 7)); python3 -I "$ROOT/tests/e2e/mock_auth.py" $APORT & APID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$APORT) 2>/dev/null && break; sleep 0.1; done
AB="http://127.0.0.1:$APORT"
mkdir -p "$T/lagent"
LENV=(env OMC_CODING_AGENT_DIR="$T/lagent" OMC_NO_BROWSER=1 OMC_AUTH_POLL_SCALE_MS=20 KIMI_CODE_OAUTH_HOST="$AB/kimi"
      OMC_AUTH_URL_REWRITE="https://api.x.ai=$AB/xaiapi,https://github.com=$AB/gh,https://api.github.com=$AB/ghapi,https://api.individual.githubcopilot.com=$AB/capi")
check "login api key"      "Logged in to xai."            bash -c "printf 'good-key\n' | ${LENV[*]} '$B' login xai"
check "login key stored"   '"key": "good-key"'            cat "$T/lagent/auth.json"
check "login key rejected" "xAI API key validation failed (401)"  bash -c "printf 'bad\n' | ${LENV[*]} '$B' login xai"
check "login device code"  "Enter code: ABCD-1234"        bash -c "${LENV[*]} '$B' login kimi-code </dev/null"
check "login device saved" '"refresh": "kimi-r1"'         cat "$T/lagent/auth.json"
check "login copilot"      "Logged in to github-copilot." bash -c "printf '\n' | ${LENV[*]} '$B' login github-copilot"
check "login list"         "openai-codex-device"          "$B" login
kill $APID 2>/dev/null
# github tool + pr:// / issue:// through the fake `gh` (tests/e2e/bin-gh: canned acme/widgets data, never GitHub)
mkdir -p "$T/ghagent" "$T/ghcfg"; cp "$T/agent/models.yml" "$T/ghagent/"; printf 'github:\n  enabled: true\n' > "$T/ghagent/config.yml"
GENV=(env -u GH_TOKEN -u GITHUB_TOKEN -u GH_HOST PATH="$ROOT/tests/e2e/bin-gh:$PATH" GH_CONFIG_DIR="$T/ghcfg" OMC_CODING_AGENT_DIR="$T/ghagent")
check "gh repo_view"     "Default branch: main"  "${GENV[@]}" "$B" -m mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"repo_view\",\"repo\":\"acme/widgets\"}"}'
check "gh search"        "Labels: bug, p1"       "${GENV[@]}" "$B" -m mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"search_issues\",\"query\":\"crash\",\"repo\":\"acme/widgets\"}"}'
check "gh run_watch"     "All jobs passed."      "${GENV[@]}" "$B" -m mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"run_watch\",\"run\":\"1001\",\"repo\":\"acme/widgets\"}"}'
check "gh bad op"        "op must be github operation (was \"nope\")"  "${GENV[@]}" "$B" -m mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"nope\"}"}'
check "pr:// view"       "Review Comments (2)"   "${GENV[@]}" "$B" -m mock/mock-1 --no-session -p "read:pr://acme/widgets/12"
check "pr:// diff list"  "3. new.txt  +0 -0  [renamed]  (renamed from old.txt)"  "${GENV[@]}" "$B" -m mock/mock-1 --no-session -p "read:pr://acme/widgets/12/diff"
check "issue:// list"    "# Issues in acme/widgets (open, up to 30)"  "${GENV[@]}" "$B" -m mock/mock-1 --no-session -p "read:issue://acme/widgets"
check "issue:// error"   "issue:// resolution failed: GraphQL: Could not resolve"  "${GENV[@]}" "$B" -m mock/mock-1 --no-session -p "read:issue://acme/widgets/999"
check "gh needs enable"  "No such tool: xd://github"  env PATH="$ROOT/tests/e2e/bin-gh:$PATH" "$B" -m mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"repo_view\"}"}'
exit $fail
