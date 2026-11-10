#!/usr/bin/env bash
# End-to-end tests: drive build/omc against a local mock OpenAI-compatible server.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# OMC_BIN: `make test` passes this platform's binary (build/obj/<os>-<arch>/omc), because build/omc is shared with
# other machines building from the same checkout (a macOS make replaces it mid-run)
B="${OMC_BIN:-$ROOT/build/omc}"
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
      - id: mock-2
        contextWindow: 100000
        maxTokens: 4096
      - id: mock-flaky
        contextWindow: 100000
        maxTokens: 4096
      - id: mock-think
        contextWindow: 100000
        maxTokens: 4096
        reasoning: true
        thinking:
          mode: effort
          efforts: [low, medium, high, xhigh]
YML
MOCK_LOG="$T/req.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $PORT & MPID=$!
trap 'kill $(jobs -p) $MPID 2>/dev/null; rm -rf "$T"' EXIT
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done
export OMC_CODING_AGENT_DIR="$T/agent" MOCK_KEY=x OMC_NO_TITLE=1 OMC_NO_LOCAL_DISCOVERY=1
cd "$T/work"
fail=0
check() { # name expected-substring command...
    local name=$1 want=$2; shift 2
    local out; out=$(timeout 30 "$@" </dev/null 2>&1)
    if [[ "$out" == *"$want"* ]]; then echo "   ok  $name"; else echo "   FAIL $name: got: $out"; fail=1; fi
}
echo "== e2e"
check "bash tool"      "Tool said: hello-e2e"            "$B" --model mock/mock-1 -p "bash:echo hello-e2e"
check "write tool"     "Successfully wrote 14 bytes to a/b.txt" "$B" --model mock/mock-1 -p "write:a/b.txt:written by omc"
check "file content"   "written by omc"                  cat a/b.txt
check "read hashline"  "1:written by omc"                "$B" --model mock/mock-1 -p "read:a/b.txt"
# intent field: injected (required) into tool schemas, stripped before execution
check "intent schema"  '"i": {"type": "string", "description": "concise intent"}'  bash -c "python3 -c 'import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; t=[x for x in r if x.get(\"tools\")][-1][\"tools\"][0][\"function\"][\"parameters\"]; print(json.dumps(t[\"properties\"][\"i\"]) if \"i\" in t[\"required\"] else \"no i\")' '$T/req.jsonl' | sed 's/^/\"i\": /'"
check "intent stripped" "Tool said: hi-intent"           "$B" --model mock/mock-1 -p 'call:bash:{"i":"Checking echo","command":"echo hi-intent"}'
# xd:// devices: discoverable tools are mounted, listed, documented and dispatched through read/write
check "xd listing"     "mounted tool devices"             "$B" --model mock/mock-1 -p "read:xd://"
check "xd docs"        "type Args = {"                    "$B" --model mock/mock-1 -p "read:xd://debug"
check "xd dispatch"    "Invalid args for xd://lsp: Validation failed" "$B" --model mock/mock-1 -p 'call:write:{"path":"xd://lsp","content":"{\"action\":\"bogus\"}"}'
check "xd report"      "Noted, thanks!"                   "$B" --model mock/mock-1 -p 'call:write:{"path":"xd://report_issue","content":"read: wrong output"}'
check "glob tool"      $'Tool said: # a/\nb.txt'          "$B" --model mock/mock-1 -p "glob:**/*.txt"
check "grep tool"      "*1:written by omc"               "$B" --model mock/mock-1 -p "grep:written"
# grep / glob result text as omp words it (grouped path tree, errors, empty results)
check "grep grouped"   $'Tool said: # a/\n## b.txt#'      "$B" --model mock/mock-1 -p "grep:written"
# mr:// (WP-27): GitLab MR diffs through a fake glab (tests/e2e/bin-glab)
GLP="$ROOT/tests/e2e/bin-glab:$PATH"
check "mr listing"     $'Tool said: # Merge Request Diff: gitlab.example.com/grp/sub/proj!7 (2 files)\n\n1. src/app.c  +1 -0  [modified]\n   mr://gitlab.example.com/grp/sub/proj/7/diff/1' env PATH="$GLP" "$B" --model mock/mock-1 -p "read:mr://7/diff"
check "mr slice"       $'Tool said: diff --git a/docs/old.md b/docs/new.md\nsimilarity index 90%' env PATH="$GLP" "$B" --model mock/mock-1 -p "read:mr://gitlab.example.com/grp/sub/proj/7/diff/2"
check "mr range"       "mr:// diff resolution failed: mr://gitlab.example.com/grp/sub/proj/7/diff/5 is out of range; MR has 2 files." env PATH="$GLP" "$B" --model mock/mock-1 -p "read:mr://7/diff/5"
check "mr auth"        'mr:// diff resolution failed: GitLab CLI is not authenticated. Run `glab auth login`.' env PATH="$GLP" "$B" --model mock/mock-1 -p "read:mr://9/diff"
check "mr invalid"     "Invalid mr:// URL. Expected mr://<iid>/diff" env PATH="$GLP" "$B" --model mock/mock-1 -p "read:mr://bad"
# attachment:// (WP-27): the image file behind the prompt's Image #N
check "attachment none" "Available attachment URIs: none. Use one of the listed attachment URIs"  "$B" --model mock/mock-1 -p "read:attachment://1"
mkdir -p "$T/agent-img" && sed 's/^\(      - id: mock-1\)$/\1\n        input: [text, image]/' "$T/agent/models.yml" > "$T/agent-img/models.yml"
python3 -c 'import base64,sys; open(sys.argv[1],"wb").write(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAIAAAA7ljmRAAAAEElEQVR4nGP4z8AARww4OQD1MQv1NXv7ggAAAABJRU5ErkJggg=="))' "$T/work/red.png"
env OMC_CODING_AGENT_DIR="$T/agent-img" "$B" --model mock/mock-1 -p "attach:read:attachment://1" @red.png >/dev/null 2>&1 </dev/null
check "attachment read" "Read image file [image/"  python3 -I -c 'import json,sys; r=[json.loads(l) for l in open(sys.argv[1])][-1]; print([m["content"] for m in r["messages"] if m.get("role") == "tool"][-1])' "$T/req.jsonl"
check "attachment list" "Available attachment URIs: attachment://1."  env OMC_CODING_AGENT_DIR="$T/agent-img" "$B" --model mock/mock-1 -p "attach:read:attachment://3" @red.png
rm -f "$T/work/red.png"
check "grep file"      $'[a/b.txt#'                      "$B" --model mock/mock-1 -p 'call:grep:{"pattern":"written","path":"a/b.txt:1-5"}'
check "grep no match"  "Tool said: No matches found"     "$B" --model mock/mock-1 -p 'call:grep:{"pattern":"zzq_none"}'
check "grep missing"   "Path not found: nope/x"          "$B" --model mock/mock-1 -p 'call:grep:{"pattern":"x","path":"nope/x"}'
check "grep partial"   "Skipped missing paths: nope"     "$B" --model mock/mock-1 -p 'call:grep:{"pattern":"written","path":"a;nope"}'
check "glob none"      "No files found matching pattern" "$B" --model mock/mock-1 -p 'call:glob:{"path":"*.zzq"}'
check "glob limit"     "Requested limit 500 clamped to the max of 200" "$B" --model mock/mock-1 -p 'call:glob:{"path":"a","limit":500}'
check "plain answer"   "plain answer"                    "$B" --model mock/mock-1 -p "hi"
check "continue"       "plain answer"                    "$B" --model mock/mock-1 -c -p "again"
check "json mode"      '"type":"agent_end"'              "$B" --model mock/mock-1 --mode json -p "bash:echo j"
check "rpc mode"       '"status":"completed"'            bash -c "printf '%s\n' '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"bash:echo r\"}' | (cat; sleep 2) | '$B' --model mock/mock-1 --mode rpc"
check "sessions list"  "msgs"                            "$B" sessions
check "no-session"     "plain answer"                    "$B" --model mock/mock-1 --no-session -p "x"
# hashline edit: read to get the tag, then replace line 1 and append after it
printf 'alpha\nbeta\ngamma\n' > e.txt
TAG=$(timeout 30 "$B" --model mock/mock-1 -p "read:e.txt" </dev/null | grep -o 'e.txt#[0-9A-F]*' | head -1 | cut -d'#' -f2)
check "edit apply"     "[e.txt#"  "$B" --model mock/mock-1 -p "edit:[e.txt#$TAG]\nPUT 2.=2:\n+BETA\nPUT >3:\n+delta"
check "edit result"    $'alpha\nBETA\ngamma\ndelta'  cat e.txt
check "edit stale tag" "is not from this session."  "$B" --model mock/mock-1 -p "edit:[e.txt#$TAG]\nPUT 1.=1:\n+x"
# hashline engine messages (omp pi-edit wording): no-op, header-only, parse errors, unseen-line reveal, blocks
TAG=$(timeout 30 "$B" --model mock/mock-1 -p "read:e.txt" </dev/null | grep -o 'e.txt#[0-9A-F]*' | head -1 | cut -d'#' -f2)
check "edit no-op"     "parsed and applied cleanly, but produced no change"  "$B" --model mock/mock-1 -p "edit:[e.txt#$TAG]\nPUT 1.=1:\n+alpha"
check "edit hdr only"  "no ops follow the header; put them under this file's current tag, #$TAG."  "$B" --model mock/mock-1 -p "edit:[e.txt#$TAG]"
check "edit no header" 'input must begin with "[PATH#HASH]" on the first non-blank line'  "$B" --model mock/mock-1 -p "edit:PUT 1.=1:\n+x"
check "edit bare row"  'line 1: payload line has no preceding hunk header. Use `PUT N.=M:`'  "$B" --model mock/mock-1 -p "edit:[e.txt#$TAG]\nfoo"
check "edit range"     "Line 9 does not exist (file has 5 lines)"  "$B" --model mock/mock-1 -p "edit:[e.txt#$TAG]\nPUT 9.=9:\n+x"
seq 1 30 > big.txt
BTAG=$(timeout 30 "$B" --model mock/mock-1 -p "read:big.txt" </dev/null | grep -o 'big.txt#[0-9A-F]*' | head -1 | cut -d'#' -f2)
check "edit unseen"    "This edit anchors to lines 20 of big.txt that [big.txt#$BTAG] never displayed"  "$B" --model mock/mock-1 --mode json -p "calls:[[\"read\",{\"path\":\"big.txt:1-3\"}],[\"edit\",{\"input\":\"[big.txt#$BTAG]\\nPUT 20.=20:\\n+X\"}]]"
printf 'int f(void) {\n    return 1;\n}\n' > blk.c
CTAG=$(timeout 30 "$B" --model mock/mock-1 -p "read:blk.c" </dev/null | grep -o 'blk.c#[0-9A-F]*' | head -1 | cut -d'#' -f2)
check "edit block"     "PUT 1*: → resolved lines 1-3 (3 lines)"  "$B" --model mock/mock-1 -p "edit:[blk.c#$CTAG]\nPUT 1*:\n+int f(void) { return 2; }"
printf 'a\r\nb\r\n' > crlf.txt
check "write crlf"     "Note: kept the replaced file's CRLF line endings."  "$B" --model mock/mock-1 -p 'call:write:{"path":"crlf.txt","content":"x\ny\n"}'
check "write no content" "content is required for nc.txt."  "$B" --model mock/mock-1 -p 'call:write:{"path":"nc.txt"}'
check "write eisdir"   "EISDIR: illegal operation on a directory, open '"  "$B" --model mock/mock-1 -p "write:a:x"
# non-interactive env: a config-file sequence.editor and GCM prompts must not hang a command
check "bash env"       "true never" "$B" --model mock/mock-1 --no-session -p 'bash:echo $GIT_SEQUENCE_EDITOR $GCM_INTERACTIVE'
mkdir -p "$T/rebase" && (cd "$T/rebase" && git init -q . && git config user.email a@b && git config user.name a && git config sequence.editor "sleep 300" && echo 1 > a && git add a && git commit -qm one && echo 2 > a && git commit -qam two)
check "rebase -i"      "Successfully rebased" "$B" --model mock/mock-1 --no-session -p "bash:cd $T/rebase && git rebase -i HEAD~1 2>&1"
# read page budget (column-cut lines, :raw byte cap, multi-range stop) and the write read-projection guard
check "read budget" "read budget ok"  python3 -I "$ROOT/tests/e2e/read_budget.py" "$B" "$T/req.jsonl" "$T/agent/models.yml" "$T/rb"
# edit modes: apply_patch via config.yml (edit.mode), patch / replace / sloppy via PI_EDIT_VARIANT
printf 'edit:\n  mode: apply_patch\n' > "$T/agent/config.yml"
printf 'one\ntwo\nthree\n' > ap.txt; printf 'bye\n' > ap-gone.txt
check "apply_patch run" "[ap-moved.txt]"  "$B" --model mock/mock-1 -p 'call:edit:{"input":"*** Begin Patch\n*** Update File: ap.txt\n*** Move to: ap-moved.txt\n@@\n one\n-two\n+TWO\n*** Delete File: ap-gone.txt\n*** Add File: ap-new.txt\n+fresh\n*** End Patch"}'
check "apply_patch out" $'one\nTWO\nthree'  cat ap-moved.txt
check "apply_patch add" "fresh"  cat ap-new.txt
[[ ! -e ap.txt && ! -e ap-gone.txt ]] && echo "   ok  apply_patch move/delete" || { echo "   FAIL apply_patch move/delete"; fail=1; }
grep -q 'stripped-down, file-oriented diff' "$T/req.jsonl" && echo "   ok  apply_patch prompt" || { echo "   FAIL apply_patch prompt"; fail=1; }
check "apply_patch err" "Failed to find expected lines in ap-moved.txt"  "$B" --model mock/mock-1 -p 'call:edit:{"input":"*** Begin Patch\n*** Update File: ap-moved.txt\n@@\n-missing\n+x\n*** End Patch"}'
rm -f "$T/agent/config.yml"
printf 'def greet():\n    print("Hi")\n' > pm.py
check "patch mode"      '2:    print("Hello")'  env PI_EDIT_VARIANT=patch "$B" --model mock/mock-1 -p 'call:edit:{"path":"pm.py","edits":[{"op":"update","diff":"@@ def greet():\n def greet():\n-    print(\"Hi\")\n+    print(\"Hello\")\n"}]}'
check "patch result"    'print("Hello")'  cat pm.py
printf 'alpha = 1\nbeta = 2\n' > rp.txt
check "replace mode"    "[rp.txt]"  env PI_EDIT_VARIANT=replace "$B" --model mock/mock-1 -p 'call:edit:{"path":"rp.txt","old_string":"beta = 2","new_string":"beta = 3"}'
check "replace result"  "beta = 3"  cat rp.txt
check "replace error"   "Could not find a close enough match in rp.txt"  env PI_EDIT_VARIANT=replace "$B" --model mock/mock-1 -p 'call:edit:{"path":"rp.txt","old_string":"gamma = 9","new_string":"x"}'
printf 'start();\nconst timeout = 1000;\nfinish();\n' > sl.ts
check "sloppy mode"     "[sl.ts]"  env PI_EDIT_VARIANT=sloppy "$B" --model mock/mock-1 -p 'call:edit:{"input":"*** Edit File: sl.ts\n*** Find\nconst timeout = 1000;\n*** Replace\nconst timeout = 5000;\n*** Find\nfinish();\n*** Insert Before\nlog();\n"}'
check "sloppy result"   $'start();\nconst timeout = 5000;\nlog();\nfinish();'  cat sl.ts
check "todo tool"      "Remaining items (2)"  "$B" --model mock/mock-1 -p "todo:first,second"
# tool output parity (omp OutputSink): middle elision + session artifact, column cap, sanitizing, exit notice
LASTMSG='import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; m=[x for x in r[-1]["messages"] if x.get("role")=="tool"][-1]; c=m["content"]; print(c if isinstance(c,str) else "".join(b.get("text","") for b in c), end="")'
"$B" --model mock/mock-1 --session-dir "$T/sink" -p "bash:seq 1 30000" >/dev/null 2>&1 </dev/null
check "sink elision"   "[Showing lines 1-4317 and 24882-30000 of 30000; 20,564 middle lines (114.9KB) elided. Read artifact://0 for full output]"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
check "sink marker"    "4317"$'\n'"[…20564ln elided…]"$'\n'"24882"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
check "sink artifact"  "168894"  bash -c "wc -c < \"\$(ls $T/sink/*/0.bash.log)\""
"$B" --model mock/mock-1 --session-dir "$T/sink" -c -p "read:artifact://0:raw:29999-29999" >/dev/null 2>&1 </dev/null
check "artifact read"  "29999"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" --model mock/mock-1 --no-session -p "bash:python3 -c \"print('x'*2000)\"; printf 'a\\033[31mb\\rc\\n'; exit 3" >/dev/null 2>&1 </dev/null
check "sink columns"   "…"$'\n'"ab"$'\n'"c"$'\n\n\n'"Command exited with code 3"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
check "sink col note"  "[Some lines truncated to 768 bytes]"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
# tool result text parity with omp: argument validation (omptype messages, coercion, received args), todo/task
# errors, bash without bash's job-status notices
"$B" --model mock/mock-1 --no-session -p 'call:grep:{}' >/dev/null 2>&1 </dev/null
check "args missing"   "  - pattern: pattern must be a string (was missing)"  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" --model mock/mock-1 --no-session -p 'call:grep:{"pattern":true,"case":3}' >/dev/null 2>&1 </dev/null
check "args coerced"   $'  - case: case must be boolean (was a number)\n\nReceived arguments:\n{\n  "original": {\n    "pattern": true,'  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" --model mock/mock-1 --no-session -p 'call:todo:{"op":"bogus"}' >/dev/null 2>&1 </dev/null
check "todo bad op"    'Invalid todo arguments: op must be "init", "start", "done", "rm", "drop", "block", "unblock", "append" or "view" (was "bogus")'  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" --model mock/mock-1 --no-session -p 'seq:[["todo",{"op":"init","items":["a"]}],["todo",{"op":"start","task":"nope"}]]' >/dev/null 2>&1 </dev/null
check "todo errors"    $'Errors: Task "nope" not found\nRemaining items (1):'  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" --model mock/mock-1 --no-session -p 'call:task:{}' >/dev/null 2>&1 </dev/null
check "task shape"     'Missing `tasks`. Provide a `tasks` array (one subagent per item) with a shared `context`.'  python3 -I -c "$LASTMSG" "$T/req.jsonl"
"$B" --model mock/mock-1 --no-session -p "bash:sh -c 'kill -9 \$\$'; printf 'x'; sh -c 'kill -SEGV \$\$'; echo after-kill" >/dev/null 2>&1 </dev/null
check "bash no notice" "xafter-kill"  python3 -I -c "$LASTMSG; import re; sys.exit(1) if re.search('Killed|Segmentation', c) else None" "$T/req.jsonl"
# skills + internal URLs + context files
mkdir -p "$T/agent/skills/demo"
printf -- '---\nname: demo\ndescription: Demo skill for tests\n---\nSKILL BODY LINE\n' > "$T/agent/skills/demo/SKILL.md"
printf 'Project rule: always test.\n' > AGENTS.md
check "skill url"      "SKILL BODY LINE"  "$B" --model mock/mock-1 -p "read:skill://demo"
check "unknown skill"  "Unknown skill"    "$B" --model mock/mock-1 -p "read:skill://nope"
grep -q "Demo skill for tests" "$T/req.jsonl" && echo "   ok  skill in system prompt" || { echo "   FAIL skill in system prompt"; fail=1; }
# skill hints are compressed on the smol role chain (omp never falls back to the session model)
printf 'modelRoles:\n  smol: mock/mock-1\n' > "$T/agent/config.yml"
"$B" --model mock/mock-1 -p "warm hints" >/dev/null 2>&1 </dev/null
sleep 1; "$B" --model mock/mock-1 -p "hi again" >/dev/null 2>&1 </dev/null
tail -1 "$T/req.jsonl" | grep -q "Use for demo skill test routing" && echo "   ok  skill hint compressed" || { echo "   FAIL skill hint compressed"; fail=1; }
rm -f "$T/agent/config.yml"
# a prompt on a provider with no usable credential walks retry.fallbackChains (no request reaches its host)
mkdir -p "$T/nocred"
cat > "$T/nocred/models.yml" <<YML
providers:
  mock:
    baseUrl: http://127.0.0.1:$PORT/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: mock-1
YML
: > "$T/nocred/config.yml"
NOCRED=(env -u SCW_SECRET_KEY -u SCW_API_KEY HOME="$T/nocred" OMC_CODING_AGENT_DIR="$T/nocred")
out=$("${NOCRED[@]}" "$B" -p "hello" --model scaleway/glm-5.3 2>&1 </dev/null); nrc=$?
[[ $nrc == 1 && "$out" == *"No API key found for scaleway."* && "$out" == *"Use /login, set an API key environment variable"* ]] && echo "   ok  no credential refuses the prompt" || { echo "   FAIL no credential refuses the prompt: $nrc $out"; fail=1; }
printf 'retry:\n  fallbackChains:\n    scaleway/glm-5.3:\n      - mock/mock-1\n' > "$T/nocred/config.yml"
: > "$T/req.jsonl"
out=$("${NOCRED[@]}" "$B" -p "hello" --model scaleway/glm-5.3 2>&1 </dev/null); nrc=$?
[[ $nrc == 0 && "$out" == *"[warning] Fallback: scaleway/glm-5.3"*" -> mock/mock-1"$'\n'"no credential for scaleway"* && "$out" != *"No API key"* ]] && echo "   ok  no credential walks the chain" || { echo "   FAIL no credential walks the chain: $nrc $out"; fail=1; }
tail -1 "$T/req.jsonl" | grep -q '"model": "mock-1"' && echo "   ok  no credential fallback used" || { echo "   FAIL no credential fallback used"; fail=1; }
printf 'retry:\n  modelFallback: false\n  fallbackChains:\n    scaleway/glm-5.3:\n      - mock/mock-1\n' > "$T/nocred/config.yml"
out=$("${NOCRED[@]}" "$B" -p "hello" --model scaleway/glm-5.3 2>&1 </dev/null); nrc=$?
[[ $nrc == 1 && "$out" == *"No API key found for scaleway."* ]] && echo "   ok  modelFallback off refuses" || { echo "   FAIL modelFallback off refuses: $nrc $out"; fail=1; }
# `auth: none` needs no credential
cat >> "$T/nocred/models.yml" <<YML
  noauth:
    baseUrl: http://127.0.0.1:$PORT/v1
    auth: none
    api: openai-completions
    models:
      - id: mock-1
YML
out=$("${NOCRED[@]}" "$B" -p "hello" --model noauth/mock-1 2>&1 </dev/null); nrc=$?
[[ $nrc == 0 && "$out" != *"No API key"* ]] && echo "   ok  auth none is keyless" || { echo "   FAIL auth none is keyless: $nrc $out"; fail=1; }
# a foreground command past the auto-background threshold: detaching it used to loop forever holding the jobs lock
printf 'bash:\n  autoBackground:\n    enabled: true\n    thresholdMs: 300\n' > "$T/agent/config.yml"
check "bash auto-background" "plain answer"  "$B" --model mock/mock-1 --no-session -p "bash:sleep 2; echo finished"
rm -f "$T/agent/config.yml"
# retry model fallback: a rate-limited model walks retry.fallbackChains
printf 'retry:\n  fallbackChains:\n    mock/mock-flaky:\n      - mock/mock-2\n' > "$T/agent/config.yml"
out=$(timeout 60 "$B" --model mock/mock-flaky -p "bash:echo fb-ok" </dev/null 2>&1)
[[ "$out" == *"Tool said: fb-ok"* && "$out" == *"[warning] Fallback: mock/mock-flaky -> mock/mock-2"* ]] && echo "   ok  retry fallback" || { echo "   FAIL retry fallback: $out"; fail=1; }
tail -1 "$T/req.jsonl" | grep -q '"model": "mock-2"' && echo "   ok  fallback model used" || { echo "   FAIL fallback model used"; fail=1; }
# a plain 503 retries after the exponential base (retry.baseDelayMs), not the 45-75 s capacity window
# (the transport's own retries of each request come on top; the mock's retry-after-ms: 0 makes them immediate)
printf 'retry:\n  maxRetries: 1\n  baseDelayMs: 10\n' > "$T/agent/config.yml"
s0=$(date +%s%N); out=$(timeout 40 "$B" --model mock/mock-1 --no-session -p "fail:503:try again later" </dev/null 2>&1); s1=$(date +%s%N)
(( (s1 - s0) / 1000000 < 30000 )) && [[ "$out" == *"Retrying (1/1): 503 try again later"* ]] && echo "   ok  503 retry backoff" || { echo "   FAIL 503 retry backoff ($(( (s1 - s0) / 1000000 )) ms): $out"; fail=1; }
# a rejected request: omp's error text, the dump line and file with its credentials redacted, no `omc:` prefix
printf 'retry:\n  enabled: false\n' > "$T/agent/config.yml"
out=$(timeout 30 "$B" --model mock/mock-1 --no-session -p "fail:400:bad things" </dev/null 2>&1)
dump=$(printf '%s\n' "$out" | sed -n 's/^raw-http-request=//p')
if [[ "$out" == *$'\n400 bad things (type=invalid_request_error)\nraw-http-request='* && -f "$dump" ]] && python3 -I -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d["provider"] == "mock" and d["errorResponse"]["status"] == 400 and d["errorResponse"]["message"].startswith("400 bad things")
assert d["headers"].get("Authorization", d["headers"].get("authorization")) == "[redacted]" and "messages" in d["body"]' "$dump"
then echo "   ok  rejected request dump"; else echo "   FAIL rejected request dump: $out"; fail=1; fi
# transport retries like omp (postOpenAIStream + withReplaySafeStreamRetry): 6 attempts, then one replay-safe
# provider retry after 500 ms = 12 requests per turn attempt; a 400 is sent once
printf 'retry:\n  enabled: false\n' > "$T/agent/config.yml"
s0=$(date +%s%N); out=$(timeout 30 "$B" --model mock/mock-1 --no-session -p "fail:503:transport-probe" </dev/null 2>&1); s1=$(date +%s%N)
n=$(grep -c "fail:503:transport-probe" "$T/req.jsonl")
(( n == 12 && (s1 - s0) / 1000000 >= 450 )) && [[ "$out" == *"503 transport-probe"* ]] && echo "   ok  transport retries (12 requests)" || { echo "   FAIL transport retries: $n requests in $(( (s1 - s0) / 1000000 )) ms: $out"; fail=1; }
timeout 30 "$B" --model mock/mock-1 --no-session -p "fail:400:transport-once" </dev/null >/dev/null 2>&1
n=$(grep -c "fail:400:transport-once" "$T/req.jsonl")
(( n == 1 )) && echo "   ok  400 not retried" || { echo "   FAIL 400 not retried: $n requests"; fail=1; }
rm -f "$T/agent/config.yml"
# without a header hint the delay comes from the body text ("Please retry in 100ms"): 5 x 100 ms + 500 ms + 5 x 100 ms
TR_PORT=$((20000 + RANDOM % 20000)); [[ $TR_PORT == "$PORT" ]] && TR_PORT=$((TR_PORT + 1))
mkdir -p "$T/agent-tr"
sed "s#127.0.0.1:$PORT/#127.0.0.1:$TR_PORT/#" "$T/agent/models.yml" > "$T/agent-tr/models.yml"
printf 'retry:\n  enabled: false\n' > "$T/agent-tr/config.yml"
MOCK_FAIL_RETRY_AFTER_MS=none MOCK_LOG="$T/req-tr.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $TR_PORT & TR_MPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$TR_PORT) 2>/dev/null && break; sleep 0.1; done
s0=$(date +%s%N); out=$(OMC_CODING_AGENT_DIR="$T/agent-tr" timeout 30 "$B" --model mock/mock-1 --no-session -p "fail:503:Please retry in 100ms" </dev/null 2>&1); s1=$(date +%s%N)
n=$(grep -c "Please retry in 100ms" "$T/req-tr.jsonl"); ms=$(( (s1 - s0) / 1000000 ))
(( n == 12 && ms >= 1400 && ms < 8000 )) && echo "   ok  transport retry hint from the body" || { echo "   FAIL transport retry hint: $n requests in $ms ms: $out"; fail=1; }
kill $TR_MPID 2>/dev/null; wait $TR_MPID 2>/dev/null
grep -q "Project rule: always test." "$T/req.jsonl" && echo "   ok  AGENTS.md in prompt" || { echo "   FAIL AGENTS.md in prompt"; fail=1; }
# TTSR: a stream-triggered rule interrupts, injects and retries
mkdir -p .omc/rules
printf -- '---\ndescription: no forbidden word\ncondition: "FORBIDDEN"\nscope: text\n---\nNever say the forbidden word.\n' > .omc/rules/no-forbidden.md
check "ttsr retry"     "complied with rule"  "$B" --model mock/mock-1 -p "say:this is FORBIDDEN text"
grep -q 'Never say the forbidden word' "$T/req.jsonl" && echo "   ok  ttsr injection sent" || { echo "   FAIL ttsr injection sent"; fail=1; }
check "builtin rule"   "Box::leak"  "$B" --model mock/mock-1 -p "write:src/x.rs:fn f() { Box::leak(v); }"
# MCP stdio server bridged as mcp__<server>_<tool>
printf '{"mcpServers":{"mockmcp":{"command":"python3","args":["-I","%s"]}}}' "$ROOT/tests/e2e/mock_mcp.py" > "$T/agent/mcp.json"
check "mcp tool"       "mcp echo: hi-mcp"  "$B" --model mock/mock-1 -p 'call:mcp__mockmcp_echo_text:{"text":"hi-mcp"}'
grep -q "Mock MCP server instructions" "$T/req.jsonl" && echo "   ok  mcp instructions in prompt" || { echo "   FAIL mcp instructions"; fail=1; }
rm -f "$T/agent/mcp.json"
# MCP OAuth: discovery -> dynamic registration -> PKCE code (curl plays the browser) -> bearer token
OPORT=$((PORT + 1)); python3 -I "$ROOT/tests/e2e/mock_mcp_oauth.py" $OPORT & OPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$OPORT) 2>/dev/null && break; sleep 0.1; done
printf '{"mcpServers":{"oa":{"type":"http","url":"http://127.0.0.1:%s/mcp"}}}' $OPORT > "$T/agent/mcp.json"
check "mcp oauth needed" "omc mcp login oa"  bash -c "'$B' --model mock/mock-1 -p 'say:x' >/dev/null; OMC_LOG=/dev/stderr '$B' --model mock/mock-1 -p 'call:mcp__oa_whoami:{}' 2>&1 | grep -o 'omc mcp login oa' | head -1"
( sleep 20 | OMC_NO_BROWSER=1 "$B" mcp login oa > "$T/mcplogin.log" 2>&1 ) & LPID=$!
for _ in $(seq 100); do grep -q "authorize?" "$T/mcplogin.log" 2>/dev/null && break; sleep 0.1; done
curl -s -L -o /dev/null "$(grep -o 'http://127.0.0.1:[0-9]*/authorize?[^ ]*' "$T/mcplogin.log" | head -1)"
for _ in $(seq 100); do kill -0 $LPID 2>/dev/null || break; sleep 0.1; done
check "mcp oauth login" "Authorized oa"  cat "$T/mcplogin.log"
check "mcp oauth tool"  "authorized as tok-1"  "$B" --model mock/mock-1 -p 'call:mcp__oa_whoami:{}'
check "mcp oauth list"  "[oauth: logged in]"  "$B" mcp list
python3 -I -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); [v.update(expires=1) for k,v in d.items() if k.startswith("mcp:")]; json.dump(d,open(p,"w"))' "$T/agent/auth.json"
check "mcp oauth refresh" "authorized as tok-2"  "$B" --model mock/mock-1 -p 'call:mcp__oa_whoami:{}'
kill $OPID $LPID 2>/dev/null; rm -f "$T/agent/mcp.json"
# persistent bash: state survives across calls in one session
check "bash persists"  "FOO=kept"  bash -c "( printf '%s\n' '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"bash:export FOO=kept\"}'; sleep 2; printf '%s\n' '{\"id\":\"2\",\"type\":\"prompt\",\"message\":\"bash:echo FOO=\$FOO\"}'; sleep 2 ) | '$B' --model mock/mock-1 --mode rpc"
check "bash exit code" "exited with code 3"  "$B" bash 'exit 3'
# plan mode: read-only gating (approval checks below)
check "plan blocks"    "working tree is read-only"  "$B" --model mock/mock-1 --plan-mode -p "write:real.txt:x"
# eval (python kernel) when python3 is available
if command -v python3 >/dev/null; then
    check "eval python"    "Tool said: 42"  "$B" --model mock/mock-1 -p 'call:eval:{"language":"py","code":"6*7"}'
    # eval helpers: completion/wait, judge, judge_batch (drain/iter/attach/cancel/min_ok), budget, timeouts, xd://eval docs
    python3 -I "$ROOT/tests/e2e/eval_client.py" "$B" || fail=1
fi
# checkpoint / rewind (off by default: checkpoint.enabled)
printf "checkpoint:\n  enabled: true\n" > "$T/agent/config.yml"
check "rewind"         "Rewinding to checkpoint"  bash -c "( for m in 'call:checkpoint:{\"goal\":\"g\"}' 'read:a/b.txt' 'call:rewind:{\"report\":\"done\"}'; do python3 -c 'import json,sys; print(json.dumps({\"id\":\"x\",\"type\":\"prompt\",\"message\":sys.argv[1]}))' \"\$m\"; sleep 2; done ) | '$B' --model mock/mock-1 --mode rpc"
rm -f "$T/agent/config.yml"
# ast_grep structural search (off by default: astGrep.enabled)
printf "astGrep:\n  enabled: true\n" > "$T/agent/config.yml"
printf 'function f(a) {\n  console.log(a, 1);\n  if (q == q) {}\n}\n' > s.ts
check "ast_grep"       '$A = q'  "$B" --model mock/mock-1 -p 'call:ast_grep:{"pat":"$A == $A","path":"s.ts"}'
rm -f "$T/agent/config.yml"
# find: jfind cascade through the judge role (off by default: find.enabled)
printf "find:\n  enabled: on\n" > "$T/agent/config.yml"
mkdir -p fnd && printf 'int parse_token(const char *raw) {\n  return raw[0];\n}\n' > fnd/tok.c && printf 'int other(void) { return 1; } /* FAILJUDGE */\n' > fnd/fail.c
check "find hits"      $'1 hit(s) for "where are tokens parsed" in fnd/ (\xcf\x84 0.20), strongest first\npartial: 2 of 4 judge requests failed; unjudged files may still match\n\nfnd/tok.c  1.00  3 lines judged\n  fnd/tok.c:1-3  1.00  int parse_token(const char *raw) {\n\nlisted 2 \xc2\xb7 judged 2 \xc2\xb7 read 2 files' "$B" --model mock/mock-1 -p 'call:find:{"query":"where are tokens parsed","grep_keywords":["parse_token"],"path":"fnd"}'
# find's deadline: the hits verified before it come back marked partial; none verified is an error naming the phase
mkdir -p fnd2 && printf 'int parse_token(const char *raw) {\n  return raw[0];\n}\n' > fnd2/tok.c && printf 'int parse_tokens_slow(void) { return 1; } /* STALLVERIFY */\n' > fnd2/stall.c
check "find deadline"  "partial: timed out after 3.0s with 1 of 2 passages verified; unverified files may still match. Narrow \`path\` for a complete search."  env OMC_FIND_TIMEOUT_MS=3000 "$B" --model mock/mock-1 -p 'call:find:{"query":"where are tokens parsed","grep_keywords":["parse_token"],"path":"fnd2"}'
rm -f fnd2/tok.c
check "find deadline, no hits" 'find timed out after 2.0s at "verifying 1 passages in 1 files", before any hit was verified. Narrow `path` to a smaller tree, or use grep.'  env OMC_FIND_TIMEOUT_MS=2000 "$B" --model mock/mock-1 -p 'call:find:{"query":"where are tokens parsed","grep_keywords":["parse_token"],"path":"fnd2"}'
check "find empty"     '`query` must be a non-empty description'  "$B" --model mock/mock-1 -p 'call:find:{"query":"  ","grep_keywords":[]}'
rm -f "$T/agent/config.yml"
check "ast_edit stage" "(proposed) 1 replacement" "$B" --model mock/mock-1 -p 'call:ast_edit:{"ops":[{"pat":"console.log($$$X)","out":"log($$$X)"}],"paths":["s.ts"]}'
# debug tool (DAP) against a scripted mock adapter
printf 'debug:\n  adapters:\n    mockdap:\n      command: python3\n      args: ["-I", "%s"]\n      fileTypes: [".mock"]\n' "$ROOT/tests/e2e/mock_dap.py" > "$T/agent/config.yml"
touch prog.mock
check "debug session"  "x = 60"  bash -c "( for m in 'call:debug:{\"action\":\"launch\",\"program\":\"prog.mock\"}' 'call:debug:{\"action\":\"set_breakpoint\",\"file\":\"prog.mock\",\"line\":5}' 'call:debug:{\"action\":\"continue\"}' 'call:debug:{\"action\":\"step_over\"}' 'call:debug:{\"action\":\"variables\",\"variable_ref\":7}'; do python3 -c 'import json,sys; print(json.dumps({\"id\":\"x\",\"type\":\"prompt\",\"message\":sys.argv[1]}))' \"\$m\"; sleep 2; done ) | '$B' --model mock/mock-1 --mode rpc"
rm -f "$T/agent/config.yml"
# ssh:// internal URLs through a fake ssh client that runs the remote command locally
printf 'remote one\nremote two\n' > "$T/remote.txt"
printf '{"hosts":{"box":{"host":"localhost","username":"me","description":"test box"}}}' > ssh.json
check "ssh hosts"      "[box](ssh://box/)"  env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" --model mock/mock-1 -p "read:ssh://"
check "ssh read sel"   "2:remote two"       env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" --model mock/mock-1 -p "read:ssh://box$T/remote.txt:2"
check "ssh write"      "Successfully wrote 7 bytes to ssh://box"    env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" --model mock/mock-1 -p "call:write:{\"path\":\"ssh://box$T/sub/w.txt\",\"content\":\"via ssh\"}"
check "ssh write file" "via ssh"            cat "$T/sub/w.txt"
# a read projection written to a new remote file has no source to discard; over a longer one it is refused
SSHPROJ='remote one\n\n[Showing lines 1-1 of 60. Use :2 to continue]'
check "ssh projection new" "Successfully wrote"  env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" --model mock/mock-1 -p "call:write:{\"path\":\"ssh://box$T/sub/newproj.txt\",\"content\":\"$SSHPROJ\"}"
check "ssh projection over" "Refusing to overwrite"  env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" --model mock/mock-1 -p "call:write:{\"path\":\"ssh://box$T/remote.txt\",\"content\":\"$SSHPROJ\"}"
check "ssh edit refused" 'is written through `write`, not edited'  env PI_EDIT_VARIANT=replace PATH="$ROOT/tests/e2e/bin:$PATH" "$B" --model mock/mock-1 -p "call:edit:{\"path\":\"ssh://box$T/sub/w.txt\",\"old_string\":\"via ssh\",\"new_string\":\"replaced over ssh\"}"
check "ssh file kept"  "via ssh"            cat "$T/sub/w.txt"
check "ssh override"   "overrides are not allowed"  env PATH="$ROOT/tests/e2e/bin:$PATH" "$B" --model mock/mock-1 -p "read:ssh://me@box/etc/hosts"
rm -f ssh.json
# goal mode over RPC: hidden context, auto-continuation (stops on no progress), completion, resume pauses
printf 'goal:\n  continuationModes: ["rpc"]\n' > "$T/agent/config.yml"
check "goal continue"  '"customType":"goal-continuation"'  bash -c "( echo '{\"id\":\"g\",\"type\":\"goal\",\"op\":\"create\",\"objective\":\"Ship it\"}'; sleep 0.5; echo '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"bash:echo step\"}'; sleep 4 ) | '$B' --model mock/mock-1 --mode rpc"
check "goal resumed paused" '"status":"paused"'  bash -c "( echo '{\"id\":\"s\",\"type\":\"goal\",\"op\":\"get\"}'; sleep 1 ) | '$B' --model mock/mock-1 -c --mode rpc"
check "goal complete"  'Status: complete'  bash -c "( echo '{\"id\":\"r\",\"type\":\"goal\",\"op\":\"resume\"}'; sleep 0.5; echo '{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"call:goal:{\\\"op\\\":\\\"complete\\\"}\"}'; sleep 3 ) | '$B' --model mock/mock-1 -c --mode rpc"
rm -f "$T/agent/config.yml"
# advisor: shadow model reviews the turn, its concern continues the run as an <advisory>
# omp resolves the advisor only through the `advisor` role (no fallback to the session model)
printf 'advisor:\n  enabled: true\nmodelRoles:\n  advisor: mock/mock-1\n' > "$T/agent/config.yml"
check "advisor note"   "acknowledged advisory"  "$B" --model mock/mock-1 --no-session -p "bash:echo ADVISE_ME"
rm -f "$T/agent/config.yml"
# RPC: host-implemented tools, tree navigation, paging
check "rpc host tool"  "Tool said: host says hi"  python3 -I "$ROOT/tests/e2e/rpc_client.py" "$B"
check "rpc navigate"   '"editorText": "second"'   python3 -I "$ROOT/tests/e2e/rpc_client.py" "$B"
check "rpc host exclusive"  "host exclusive ok"  python3 -I "$ROOT/tests/e2e/host_exclusive_rpc.py" "$B"
# branch summary on navigation (WP-8): omp's branch_summary entry at the new leaf and summarizer request
check "branch summary entry" $'summary leaf True fromId parent True\nsummary "User explored another conversation branch, then returned here.\\nExploration summary:plain answer"\ndetails {"readFiles": [], "modifiedFiles": []} fromExtension False' python3 -I "$ROOT/tests/e2e/rpc_tree.py" "$B" "$T/req.jsonl"
check "branch summary request" 'request "<conversation>\n\n\nAssistant: plain answer\n</conversation>\n\nYou MUST create a structured summary' python3 -I "$ROOT/tests/e2e/rpc_tree.py" "$B" "$T/req.jsonl"
# plan mode without a review surface: xd://propose auto-approves (ACP-without-elicitation semantics)
check "plan propose"   'Tool said: Plan approved at local://retry-plan.md. Plan mode exited; proceed with the implementation.' python3 -I "$ROOT/tests/e2e/plan_rpc.py" "$B" --plan-mode
check "plan propose dispatch" '"xdev": {"args": {"title": "retry"}, "inner": {"planExists": true, "planFilePath": "local://retry-plan.md", "title": "retry"}, "mode": "execute", "tool": "propose"}' python3 -I "$ROOT/tests/e2e/plan_rpc.py" "$B" --plan-mode
check "plan propose off" 'No plan is awaiting approval' python3 -I "$ROOT/tests/e2e/plan_rpc.py" "$B"
# --plan-yolo: plan phase first, approved on the first propose, then implemented on the hand-off model
check "plan yolo"      '"tool": "propose"}} Plan approved. Implementing now with mock-1.' python3 -I "$ROOT/tests/e2e/plan_rpc.py" "$B" --plan-yolo --plan-yolo-into mock/mock-1
check "plan yolo into" '--plan-yolo-into requires --plan-yolo' "$B" --model mock/mock-1 --plan-yolo-into mock/mock-1 -p hi
check "plan yolo model" 'Model "nope/none" not found' "$B" --model mock/mock-1 --plan-yolo --plan-yolo-into nope/none -p hi
check "rpc btw"        "btw ok"                   python3 -I "$ROOT/tests/e2e/rpc_btw.py" "$B"
# WP-22: RPC subagent frames / commands, persisted subagent transcripts
check "rpc subagents"  $'level {\'level\': \'events\'}\nstarted task True 0 True\nlisted 1 True running task\nsteer True\nprogress True True slow:2:first answer\nevent True True\ncompleted True\nafter 0\ntranscript 0 True False user True True True\ntail True 0\nreset True 0\ncancel {\'cancelled\': True}\naborted True\ncancel again {\'cancelled\': False}\nlevel off {\'level\': \'off\'}' python3 -I "$ROOT/tests/e2e/rpc_subagents.py" "$B"
# WP-22b: parent waits on background subagents (awaitingAsyncWork, deferred session_settled), nested settle, advisories, <id>.md
check "rpc async wait" $'advisories True True\nfirst end False True\nprompt_result False\ndelivery True custom True agent background job delivery completed\nlast end True True\nwake prompt_results 0\nnested waited True\noutput md True True\nsubagent request True True True True'  python3 -I "$ROOT/tests/e2e/rpc_async_wait.py" "$B" "$T/req.jsonl"
# RPC wire parity (omp modes/rpc): framing v1/v2, messageId + delta, session events, response shapes, async bash,
# ask / approval dialogs over extension_ui_request
check "rpc wire"       "rpc wire ok"              python3 -I "$ROOT/tests/e2e/rpc_wire.py" "$B"
# message queue: steering / follow-ups sent mid-run are delivered one per turn (steeringMode/followUpMode
# one-at-a-time), steering first; "all" folds the queued steering into one turn
qrun() { # extra-commands...
    ( echo '{"id":"1","type":"prompt","message":"slow:1.5:x"}'; sleep 0.4
      for c in "$@"; do echo "$c"; done
      echo '{"id":"s1","type":"steer","message":"s1"}'; echo '{"id":"s2","type":"steer","message":"s2"}'
      echo '{"id":"f1","type":"follow_up","message":"f1"}'; sleep 5 ) | "$B" --model mock/mock-1 --no-session --mode rpc | python3 -I "$ROOT/tests/e2e/queue_events.py"
}
check "queue one-at-a-time" "slow:1.5:x,s1,s2,f1 / 4"  env B="$B" ROOT="$ROOT" bash -c "$(declare -f qrun); qrun"
check "queue steer all"     "slow:1.5:x,s1,s2,f1 / 3"  env B="$B" ROOT="$ROOT" bash -c "$(declare -f qrun); qrun '{\"id\":\"m\",\"type\":\"set_steering_mode\",\"mode\":\"all\"}'"
# async jobs: background bash delivered as a follow-up; wait with nothing running returns at once
: > "$T/req.jsonl"
check "async bash"     "Backgrounded as job bg_1"  bash -c "'$B' --model mock/mock-1 --mode json -p 'call:bash:{\"command\":\"sleep 1; echo bgdone\",\"async\":true}' | grep -o 'Backgrounded as job bg_1'"
grep -q "Background job bg_1 (sleep 1; echo bgdone) has completed" "$T/req.jsonl" && echo "   ok  async delivered" || { echo "   FAIL async delivered"; fail=1; }
# the start notice no longer echoes the command; the completion header names it on one line, middle elided past 400 chars
check "async start plain" "Backgrounded as job bg_1 (killed once" bash -c "'$B' --model mock/mock-1 --mode json -p 'call:bash:{\"command\":\"echo one; echo two\",\"async\":true}' | grep -o 'Backgrounded as job bg_1 ([a-z]* [a-z]*'"
check "async multi-line"  "async ok" bash -c "'$B' --model mock/mock-1 -p 'call:bash:{\"command\":\"echo a\\necho b\",\"async\":true}' >/dev/null; grep -qF 'Background job bg_1 (echo a\\\\necho b) has completed' '$T/req.jsonl' && echo async ok"
long_cmd=": $(printf 'x%.0s' $(seq 500))"
check "async long cmd"    "async ok" bash -c "'$B' --model mock/mock-1 -p 'call:bash:{\"command\":\"$long_cmd\",\"async\":true}' >/dev/null; python3 -I -c 'import sys;d=open(sys.argv[1],encoding=\"utf-8\").read();x=\"x\"*198;sys.exit(0 if \": \"+x+\"\\\\u2026\"+\"x\"*200+\") has completed\" in d or \": \"+x+\"…\"+\"x\"*200+\") has completed\" in d else 1)' '$T/req.jsonl' && echo async ok"
check "wait nothing"   "Nothing to wait for"  "$B" --model mock/mock-1 -p 'call:wait:{}'
check "edit none" "edit none ok"  python3 -I "$ROOT/tests/e2e/edit_none.py" "$B" "$T/req.jsonl" "$T/agent/models.yml" "$T/en"
check "shell scope" "shell scope ok"  python3 -I "$ROOT/tests/e2e/shell_scope.py" "$B" "$T/req.jsonl" "$T/agent/models.yml" "$T/ss"
# wait: idle ladder and its nudge; settle window, progress notes vs direct messages, no broadcast from a subagent
check "wait ladder" "wait ok"  python3 -I "$ROOT/tests/e2e/wait_ladder.py" "$B" "$T/req.jsonl" "$T/agent/models.yml" "$T/wl1" ladder
check "wait peers"  "wait ok"  python3 -I "$ROOT/tests/e2e/wait_ladder.py" "$B" "$T/req.jsonl" "$T/agent/models.yml" "$T/wl2" peers
# background jobs carry the call's intent: proc://, wait, /jobs; /jobs full requests the widget role's caption
check "jobs intent" "jobs intent ok"  python3 -I "$ROOT/tests/e2e/jobs_intent.py" "$B" "$T/req.jsonl" "$T/agent/models.yml" "$T/ji"
# lsp: payload "null" stays JSON null; reload restarts rust-analyzer when a new crate appeared
check "lsp sync" "lsp sync ok"  python3 -I "$ROOT/tests/e2e/lsp_sync.py" "$B" "$T/req.jsonl" "$T/agent/models.yml" "$T/ls"
check "lsp reload" "lsp reload ok"  python3 -I "$ROOT/tests/e2e/lsp_reload.py" "$B" "$T/req.jsonl" "$T/agent/models.yml" "$T/lr"
# automatic session title (interactive only, like omp's autoTitle): fork side request returns a card title
: > "$T/req.jsonl"
check "print no title"  "0"  bash -c "env -u OMC_NO_TITLE '$B' --model mock/mock-1 -p 'bash:echo untitled' >/dev/null; grep -c '<title-request>' '$T/req.jsonl'"
check "fork title req"  "title request sent"  python3 -I "$ROOT/tests/e2e/tui_title.py" "$B" "$T/req.jsonl" "bash:echo titled"
check "fork title"     "🧪 PARK: Fix flaky park tests"  bash -c "'$B' sessions | head -3"
# omc migrate: a synthetic omp agent dir (SQLite credential/history stores built here, fake secrets)
OMPD="$T/ompdir"; mkdir -p "$OMPD/skills/demo" "$OMPD/sessions/--x--" "$OMPD/blobs"
printf 'theme:\n  dark: amethyst\n' > "$OMPD/config.yml"; printf '{"mcpServers":{}}' > "$OMPD/mcp.json"
printf 'app.retry: alt+r\n' > "$OMPD/keybindings.yml"
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
check "migrate keybindings" "app.retry: alt+r"  cat "$MIG/keybindings.yml"
check "migrate creds"     '"key": "sk-new"'  cat "$MIG/auth.json"
check "migrate oauth"     '"type": "oauth"'  cat "$MIG/auth.json"
check "migrate history"   $'"text":"first"'  head -1 "$MIG/prompt-history.jsonl"
check "migrate no blobs"  "absent"     bash -c "[ -e '$MIG/blobs' ] && echo present || echo absent"
check "migrate idempotent" "kept omc's existing anthropic, openrouter"  env OMC_CODING_AGENT_DIR="$MIG" "$B" migrate --from "$OMPD"
# libomc embedding (built by `make test` via `make lib`)
if [ -x "$ROOT/build/examples/main_twice" ]; then
    check "lib main twice"  "main twice ok"  python3 -I "$ROOT/tests/e2e/main_twice_tui.py" "$ROOT/build/examples/main_twice" "$T/main_twice"
    check "lib sigpipe"     "host handler kept"  "$ROOT/build/examples/main_twice" --sigpipe-check
fi
if [ -x "$ROOT/build/examples/embed" ]; then
    check "lib host tool"  "hello from the host app, Ada"  "$ROOT/build/examples/embed" 'call:greet:{"name":"Ada"}' mock/mock-1
    check "lib rpc send"   '"command":"get_state","success":true'  "$ROOT/build/examples/embed" 'hi' mock/mock-1
fi
# ACP (Agent Client Protocol): permission request answered by the test client
check "acp prompt"     "tool_call_update completed acp-ok"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "bash:echo acp-ok"
check "acp reject"     "tool_call_update failed Tool call rejected by user (bash)"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "bash:echo no" reject_once
check "acp permission" "permission echo acp-title"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "bash:echo acp-title"
check "acp yolo"       "tool_call_update completed acp-yolo"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "bash:echo acp-yolo" reject_once --yolo
check "acp forget"     "Remembered approvals forgotten; the next read or write asks again."  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "/forget-approvals"
# ACP parity (WP-16): `omc acp`, session lifecycle (load replay / resume / fork / close), configOptions, steering,
# stop reasons, usage_update, and the fs / terminal / elicitation client bridges and client mcpServers
timeout 120 python3 -I "$ROOT/tests/e2e/acp_session.py" "$B" "$ROOT/tests/e2e/mock_mcp.py" > "$T/acp_session.out" 2>&1 </dev/null
for c in init configOptions bootstrap "fs read" "fs write" terminal usage_update "turn usage" "steering idle" \
         set_config_option refusal "fork close" "load replay" resume elicitation "mcp servers"; do
    check "acp $c" "ok $c" cat "$T/acp_session.out"
done
# tool approval (WP-10): tools.approvalMode tiers, tools.approval policies, restricted folders, CLI flags
check "approval no ui" 'Tool "bash" requires approval but no interactive UI available.'  "$B" --model mock/mock-1 --no-session --approval-mode always-ask -p "bash:echo x"
check "approval write" "Successfully wrote 3 bytes to ap.txt"  "$B" --model mock/mock-1 --no-session --approval-mode=write -p "write:ap.txt:abc"
check "approval yolo"  "Tool said: yolo-ok"  "$B" --model mock/mock-1 --no-session --approval-mode always-ask --yolo -p "bash:echo yolo-ok"
check "approval flag"  'Invalid --approval-mode value: "nope". Expected one of: always-ask, always-ask-cached, write, yolo.'  "$B" --approval-mode nope -p x
check "approval cached" 'Tool "read" requires approval'  "$B" --model mock/mock-1 --no-session --approval-mode always-ask-cached -p "read:ap.txt"
printf 'tools:\n  approval:\n    bash: deny\n  restrictedFolders: ["a"]\n' > "$T/agent/config.yml"
check "approval deny"  'To allow: remove "tools.approval.bash: deny" from config.'  "$B" --model mock/mock-1 --no-session -p "bash:echo x"
check "approval restricted" "A restricted folder (tools.restrictedFolders) asks in every approval mode."  "$B" --model mock/mock-1 --no-session -p "read:a/b.txt"
rm -f "$T/agent/config.yml"
if python3 -c 'import pty' 2>/dev/null; then
    check "approval dialog"  "approval ok kinds=permission"  python3 -I "$ROOT/tests/e2e/approval_tui.py" "$B" "bash:echo dialog-ok" '\x11' "Tool said: dialog-ok" --approval-mode always-ask
    check "approval deny key" "approval ok"  python3 -I "$ROOT/tests/e2e/approval_tui.py" "$B" "bash:echo nope" '\x1b' "Tool call denied by user: bash" --approval-mode always-ask
    # WP-22: badge / hint / HUD, Agent Hub (Alt+A, /hub, ←←), kill/revive, transcript viewer chat, session focus
    check "agent hub tui" $'hud ok\nhub ok\nactivity ok\nfilter ok\nagents ok\nkill ok\nrevive ok\ntree ok\nclose ok\nslash hub ok\nviewer ok\nchat ok\nviewer close ok\nhub close ok\nleft left ok\nfocus ok\nunfocus ok'  python3 -I "$ROOT/tests/e2e/agenthub_tui.py" "$B"
    # WP-22b: "Waiting for background job …" row, Esc stops waiting, /agents hub (strips, enable/disable -> config.yml)
    check "agents tui" $'waiting ok\ndelivered ok\nwaiting again ok\nstop waiting ok\nfolded ok\nagents ok\nstrip ok\nmodel strip ok\nback ok\ntoggle ok\nconfig True\nenable ok\nclose ok'  python3 -I "$ROOT/tests/e2e/agents_tui.py" "$B" "$T/agent"
    # WP-29: inline images per protocol, cache-miss marker, Ctrl+Shift+O tool activity, tui.mouse capture
    mkdir -p "$T/agent-disp" "$T/disp" && cp "$T/agent/models.yml" "$T/agent-disp/"
    check "display kitty" $'kitty transmit once ok\nkitty placement ok\ncache miss ok\nkitty placeholders ok'  env OMC_CODING_AGENT_DIR="$T/agent-disp" python3 -I "$ROOT/tests/e2e/display_tui.py" "$B" "$T/disp" kitty
    check "display sixel/iterm2" $'sixel ok\niterm2 ok\nfallback text ok'  env OMC_CODING_AGENT_DIR="$T/agent-disp" python3 -I "$ROOT/tests/e2e/display_tui.py" "$B" "$T/disp" other
    check "display keys" $'hide tool activity ok\nexpand while hidden ok\nshow tool activity ok\nmouse capture ok'  env OMC_CODING_AGENT_DIR="$T/agent-disp" python3 -I "$ROOT/tests/e2e/display_tui.py" "$B" "$T/disp" keys
fi
# slash commands (WP-4): one registry behind RPC/ACP advertising and builtin dispatch
check "acp commands"   "available_commands_update"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "bash:echo cmds"
check "acp slash"      "agent_message_chunk Workspace directories:"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "/dirs"
check "rpc slash"      "ok unknown slash goes to the model"  python3 -I "$ROOT/tests/e2e/slash_rpc.py" "$B"
# WP-28: /changelog, /skillful, /force over RPC (forced tool_choice then "none" on the wire)
check "rpc extracmds"  "extracmds rpc ok"  python3 -I "$ROOT/tests/e2e/extracmds_rpc.py" "$B" "$T/req.jsonl"
check "tui extracmds"  "extracmds tui ok"  python3 -I "$ROOT/tests/e2e/extracmds_tui.py" "$B" "$T/xc"
# workspace and session lifecycle (WP-21): --add-dir, /add-dir, /remove-dir, /dirs, /pin, /fresh, /move, /wt
check "rpc workspace"  "workspace rpc ok"  python3 -I "$ROOT/tests/e2e/workspace_rpc.py" "$B" "$T/req.jsonl"
# model switching (WP-19): /switch, /modelpreset, prewalk handoff, in a throwaway agent dir
mkdir -p "$T/agent-ms" && cp "$T/agent/models.yml" "$T/agent-ms/"
check "rpc model switch" "ok prewalk session entries"  env OMC_CODING_AGENT_DIR="$T/agent-ms" python3 -I "$ROOT/tests/e2e/model_switch_rpc.py" "$B" "$T/agent-ms" "$T/req.jsonl"
# custom commands (WP-9): file commands, prompt templates, /skill:, MCP prompts over RPC, ACP and print mode
check "rpc commands"   "ok empty MCP prompt consumed"  python3 -I "$ROOT/tests/e2e/commands_rpc.py" "$B" "$T/req.jsonl" "$ROOT/tests/e2e/mock_mcp.py"
# MCP and SSH management (WP-18): /mcp and /ssh over RPC (text mode) and in the TUI (wizard, test, cancel)
check "skill cost cap" "skill cost ok"  python3 -I "$ROOT/tests/e2e/skill_cost_rpc.py" "$B" "$T/req.jsonl" "$T/skill_cost" "$T/agent/models.yml"
check "mcp tool scope" "mcp scope ok"  python3 -I "$ROOT/tests/e2e/mcp_scope_rpc.py" "$B" "$T/req.jsonl" "$ROOT/tests/e2e/mock_mcp.py" "$T/mcp_scope"
check "rpc mcp/ssh"    "ok mcp rpc done"  python3 -I "$ROOT/tests/e2e/mcp_rpc.py" "$B" "$ROOT/tests/e2e/mock_mcp.py"
if python3 -c 'import pty' 2>/dev/null; then
    check "tui mcp/ssh"  "mcp tui ok"  python3 -I "$ROOT/tests/e2e/mcp_tui.py" "$B" "$ROOT/tests/e2e/mock_mcp.py" "$T/req.jsonl"
    check "tui quiet"    "quiet tui ok"  python3 -I "$ROOT/tests/e2e/quiet_tui.py" "$B" "$ROOT/tests/e2e/mock_mcp.py" "$T/quiet"
    check "tui resume"   "resume tui ok" python3 -I "$ROOT/tests/e2e/resume_tui.py" "$B" "$T/req.jsonl" "$T/resume"
    check "tui btw inject"  "btw inject tui ok"  python3 -I "$ROOT/tests/e2e/btw_inject_tui.py" "$B" "$T/req.jsonl" "$T/btw_inject"
    check "tui fs stall"  "fs stall ok"  python3 -I "$ROOT/tests/e2e/fs_stall_tui.py" "$B" "$T/fs_stall"
fi
check "acp file cmd"   "stop end_turn"  python3 -I "$ROOT/tests/e2e/acp_client.py" "$B" "/init"
grep -q "synthesize findings into one AGENTS.md" "$T/req.jsonl" && echo "   ok  acp /init expanded" || { echo "   FAIL acp /init expanded"; fail=1; }
mkdir -p .omc/commands
printf -- '---\ndescription: Print cmd\n---\nPrinted $1 and $2\n' > .omc/commands/pc.md
check "print file cmd" "plain answer"  "$B" --model mock/mock-1 -p "/pc one two"
grep -q "Printed one and two" "$T/req.jsonl" && echo "   ok  print /pc expanded" || { echo "   FAIL print /pc expanded"; fail=1; }
rm -rf .omc/commands
# Amazon Bedrock ConverseStream against a mock that verifies SigV4 independently and speaks event-stream
BPORT=$((PORT + 1))
MOCK_LOG="$T/bedrock.jsonl" MOCK_BEDROCK_BEARER=br-token python3 -I "$ROOT/tests/e2e/mock_bedrock.py" $BPORT & BPID=$!
trap 'kill $(jobs -p) $MPID $BPID 2>/dev/null; rm -rf "$T"' EXIT
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$BPORT) 2>/dev/null && break; sleep 0.1; done
cp "$T/agent/models.yml" "$T/models.yml.bak"
printf '  amazon-bedrock:\n    baseUrl: http://127.0.0.1:%s\n' $BPORT >> "$T/agent/models.yml"
BM=amazon-bedrock/us.anthropic.claude-sonnet-4-5-20250929-v1:0
BENV=(env -u AWS_PROFILE -u AWS_SESSION_TOKEN -u AWS_BEARER_TOKEN_BEDROCK -u AWS_DEFAULT_REGION -u AWS_WEB_IDENTITY_TOKEN_FILE \
    -u AWS_CONTAINER_CREDENTIALS_RELATIVE_URI -u AWS_CONTAINER_CREDENTIALS_FULL_URI -u AWS_ACCESS_KEY_ID -u AWS_SECRET_ACCESS_KEY \
    HOME="$T" AWS_REGION=us-west-2 AWS_EC2_METADATA_DISABLED=true AWS_CONFIG_FILE="$T/none" AWS_SHARED_CREDENTIALS_FILE="$T/none")
check "bedrock models"  "amazon-bedrock ("      "${BENV[@]}" AWS_ACCESS_KEY_ID=AKIDMOCK AWS_SECRET_ACCESS_KEY=mock-secret "$B" models sonnet-4-5
check "bedrock text"    "plain bedrock answer"  "${BENV[@]}" AWS_ACCESS_KEY_ID=AKIDMOCK AWS_SECRET_ACCESS_KEY=mock-secret "$B" --model "$BM" --no-session -p "hi"
check "bedrock tool"    "Tool said: br-e2e"     "${BENV[@]}" AWS_ACCESS_KEY_ID=AKIDMOCK AWS_SECRET_ACCESS_KEY=mock-secret "$B" --model "$BM" --no-session -p "bash:echo br-e2e"
check "bedrock bearer"  "plain bedrock answer"  "${BENV[@]}" AWS_BEARER_TOKEN_BEDROCK=br-token "$B" --model "$BM" --no-session -p "hi"
check "bedrock bad sig" 'Bedrock HTTP 403: {"message": "The request signature we calculated does not match'  "${BENV[@]}" AWS_ACCESS_KEY_ID=AKIDMOCK AWS_SECRET_ACCESS_KEY=wrong "$B" --model "$BM" --no-session -p "hi"
check "bedrock no creds" "No API key found for amazon-bedrock."  "${BENV[@]}" "$B" --model "$BM" --no-session -p "hi"
grep -q '"path": "/model/us.anthropic.claude-sonnet-4-5-20250929-v1%3A0/converse-stream"' "$T/bedrock.jsonl" && echo "   ok  bedrock request path" || { echo "   FAIL bedrock request path"; fail=1; }
grep -q '"toolResult": {"toolUseId": "tooluse_mock1"' "$T/bedrock.jsonl" && echo "   ok  bedrock toolResult replay" || { echo "   FAIL bedrock toolResult replay"; fail=1; }
cp "$T/models.yml.bak" "$T/agent/models.yml"
# Google: Vertex AI via Application Default Credentials (service-account RS256 JWT -> token endpoint) and Cloud Code
# Assist (google-gemini-cli / google-antigravity) with a stored OAuth credential, against a local mock; no real creds.
GPORT=$((PORT + 2))
MOCK_LOG="$T/google.jsonl" python3 -I "$ROOT/tests/e2e/mock_google.py" $GPORT & GPID=$!
trap 'kill $(jobs -p) $MPID $BPID $GPID 2>/dev/null; rm -rf "$T"' EXIT
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$GPORT) 2>/dev/null && break; sleep 0.1; done
printf '  google-vertex:\n    baseUrl: http://127.0.0.1:%s\n  google-gemini-cli:\n    baseUrl: http://127.0.0.1:%s\n  google-antigravity:\n    baseUrl: http://127.0.0.1:%s\n' \
    $GPORT $GPORT $GPORT >> "$T/agent/models.yml"
GENV=(env -u GOOGLE_APPLICATION_CREDENTIALS -u GOOGLE_CLOUD_ACCESS_TOKEN -u CLOUDSDK_AUTH_ACCESS_TOKEN -u GOOGLE_CLOUD_API_KEY \
    -u GOOGLE_CLOUD_PROJECT -u GCP_PROJECT -u GCLOUD_PROJECT -u GOOGLE_VERTEX_LOCATION -u GOOGLE_CLOUD_LOCATION -u VERTEX_LOCATION \
    HOME="$T" GCE_METADATA_HOST=127.0.0.1:1 PI_AI_ANTIGRAVITY_VERSION=2.26.0)
if openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$T/sa.pem" 2>/dev/null; then
    python3 -I -c 'import json,sys; print(json.dumps({"type":"service_account","client_email":"sa@mock.iam.gserviceaccount.com","private_key":open(sys.argv[1]).read(),"private_key_id":"k1","token_uri":"http://127.0.0.1:%s/token" % sys.argv[2]}))' "$T/sa.pem" $GPORT > "$T/sa.json"
    VENV=("${GENV[@]}" GOOGLE_APPLICATION_CREDENTIALS="$T/sa.json" GOOGLE_CLOUD_PROJECT=proj-v GOOGLE_CLOUD_LOCATION=us-central1)
    check "vertex models"     "google-vertex ("       "${VENV[@]}" "$B" models gemini-3.1-pro-preview
    check "vertex adc text"   "plain google answer"   "${VENV[@]}" "$B" --model google-vertex/gemini-3.1-pro-preview --no-session -p "hi"
    check "vertex adc tool"   "Tool said: vx-e2e"     "${VENV[@]}" "$B" --model google-vertex/gemini-3.1-pro-preview --no-session -p "bash:echo vx-e2e"
    check "vertex no project" "No API key found for google-vertex."  "${GENV[@]}" GOOGLE_APPLICATION_CREDENTIALS="$T/sa.json" "$B" --model google-vertex/gemini-3.1-pro-preview --no-session -p "hi"
    grep -q '"path": "/v1/projects/proj-v/locations/us-central1/publishers/google/models/gemini-3.1-pro-preview:streamGenerateContent?alt=sse", "ua": "[^"]*", "auth": "Bearer ya29.mock-sa"' "$T/google.jsonl" \
        && echo "   ok  vertex request path" || { echo "   FAIL vertex request path"; fail=1; }
fi
check "vertex no adc"     "No models matching \"gemini-3.1-pro-preview\""      "${GENV[@]}" "$B" models gemini-3.1-pro-preview
cp "$T/agent/auth.json" "$T/auth.json.bak" 2>/dev/null
FAR=$(( $(date +%s) * 1000 + 3600000 ))
printf '{"google-gemini-cli":{"type":"oauth","access":"cca-token","refresh":"r1","expires":%s,"projectId":"proj-cca","email":"me@example.com"},"google-antigravity":{"type":"oauth","access":"cca-token","refresh":"r2","expires":%s,"projectId":"proj-cca"}}' \
    $FAR $FAR > "$T/agent/auth.json"
check "gemini-cli models" "google-gemini-cli ("  "${GENV[@]}" "$B" models gemini-2.5-pro
check "gemini-cli text"   "plain cca answer"     "${GENV[@]}" "$B" --model google-gemini-cli/gemini-2.5-pro --no-session -p "hi"
check "gemini-cli tool"   "Tool said: gc-e2e"    "${GENV[@]}" "$B" --model google-gemini-cli/gemini-2.5-pro --no-session -p "bash:echo gc-e2e"
check "antigravity claude" "plain cca answer"    "${GENV[@]}" "$B" --model google-antigravity/claude-opus-4-6 --no-session -p "hi"
check "antigravity tool"  "Tool said: ag-e2e"    "${GENV[@]}" "$B" --model google-antigravity/gemini-3.1-pro --no-session -p "bash:echo ag-e2e"
grep -q '"ua": "GeminiCLI/0.46.0/gemini-2.5-pro (' "$T/google.jsonl" && echo "   ok  gemini-cli client headers" || { echo "   FAIL gemini-cli client headers"; fail=1; }
grep -q '"ua": "antigravity/hub/2.26.0 (aidev_client; .*"model": "claude-opus-4-6-thinking", "userAgent": "antigravity"' "$T/google.jsonl" \
    && echo "   ok  antigravity envelope" || { echo "   FAIL antigravity envelope"; fail=1; }
grep -q '"functionResponse": {"name": "bash"' "$T/google.jsonl" && echo "   ok  cca functionResponse replay" || { echo "   FAIL cca functionResponse replay"; fail=1; }
if [ -f "$T/auth.json.bak" ]; then mv "$T/auth.json.bak" "$T/agent/auth.json"; else rm -f "$T/agent/auth.json"; fi
# Cursor agent (Connect + protobuf over HTTP/1.1: RunSSE stream + BidiAppend unary appends, exec channel, KV blobs)
CPORT=$((PORT + 3))
MOCK_LOG="$T/cursor.jsonl" MOCK_CURSOR_TOKEN=cur-token python3 -I "$ROOT/tests/e2e/mock_cursor.py" $CPORT & CPID=$!
trap 'kill $(jobs -p) $MPID $BPID $GPID $CPID 2>/dev/null; rm -rf "$T"' EXIT
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$CPORT) 2>/dev/null && break; sleep 0.1; done
printf '  cursor:\n    baseUrl: http://127.0.0.1:%s\n' $CPORT >> "$T/agent/models.yml"
CENV=(env HOME="$T" CURSOR_ACCESS_TOKEN=cur-token)
CM=cursor/claude-4.5-sonnet
mkdir -p "$T/cur" && printf 'cursor line one\ncursor line two\n' > "$T/cur/r.txt"
check "cursor text"     "plain cursor answer"   "${CENV[@]}" "$B" --model "$CM" --no-session -p "hi"
check "cursor shell"    "Tool said: cur-e2e"    "${CENV[@]}" "$B" --model "$CM" --session-dir "$T/cur" -p "bash:echo cur-e2e"
check "cursor history"  "user,assistant,tool,assistant"  "${CENV[@]}" "$B" --model "$CM" --session-dir "$T/cur" -c -p "history"
check "cursor read"     "Read said: cursor line one"     "${CENV[@]}" "$B" --model "$CM" --no-session -p "read:$T/cur/r.txt"
check "cursor xd read"  "Read said: # debug"              "${CENV[@]}" "$B" --model "$CM" --no-session -p "read:xd://debug"
check "cursor mcp"      "MCP said: "            "${CENV[@]}" "$B" --model "$CM" --no-session -p 'mcp:glob:{"path":"**/*.txt"}'
check "cursor end err"  "Connect error resource_exhausted: Error"  "${CENV[@]}" "$B" --model "$CM" --no-session -p "fail"
check "cursor retry"    "recovered"             "${CENV[@]}" "$B" --model "$CM" --no-session -p "flaky"
check "cursor 401"      "Cursor RunSSE failed with HTTP 401"  env HOME="$T" CURSOR_ACCESS_TOKEN=wrong "$B" --model "$CM" --no-session -p "hi"
check "cursor effort"   "plain cursor answer"   "${CENV[@]}" "$B" --model cursor/gpt-5.4 --thinking high --no-session -p "hi"
check "cursor json"     '"toolName":"bash"'     "${CENV[@]}" "$B" --model "$CM" --no-session --mode json -p "bash:echo j"
grep -q '"requestedModel": "gpt-5.4", "maxMode": false, "parameters": \[{"id": "reasoning", "value": "high"}\]' "$T/cursor.jsonl" \
    && echo "   ok  cursor effort parameters" || { echo "   FAIL cursor effort parameters"; fail=1; }
grep -q '"modelDetails": "gpt-5.4-high"' "$T/cursor.jsonl" && echo "   ok  cursor model details" || { echo "   FAIL cursor model details"; fail=1; }
grep -q '"clientVersion": "cli-2026.09.02-c22c1a3", "clientType": "cli", "streaming": "true"' "$T/cursor.jsonl" \
    && echo "   ok  cursor client headers" || { echo "   FAIL cursor client headers"; fail=1; }
grep -q '"requestContext": {"tools": \[[^]]*"write"' "$T/cursor.jsonl" && echo "   ok  cursor request context" || { echo "   FAIL cursor request context"; fail=1; }
grep -q '"originalRequestId": "[0-9a-f-]\{36\}"' "$T/cursor.jsonl" && echo "   ok  cursor retry original id" || { echo "   FAIL cursor retry original id"; fail=1; }
# Cursor login hook (deep-control browser login polled with the PKCE verifier) and token refresh, against the mock
CAUTH=(env HOME="$T" OMC_TEST_CURSOR_API="http://127.0.0.1:$CPORT" OMC_NO_BROWSER=1)
check "cursor login"    "Logged in to Cursor (Claude, GPT, etc.)"  "${CAUTH[@]}" "$B" login cursor
grep -q '"email": "dev@example.com"' "$T/agent/auth.json" && echo "   ok  cursor login email" || { echo "   FAIL cursor login email"; fail=1; }
grep -q '"verifier": 128' "$T/cursor.jsonl" && echo "   ok  cursor login verifier" || { echo "   FAIL cursor login verifier"; fail=1; }
python3 -I -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["cursor"]["expires"]=1; json.dump(d,open(p,"w"))' "$T/agent/auth.json"
check "cursor refresh"  "plain cursor answer"   "${CAUTH[@]}" "$B" --model "$CM" --no-session -p "hi"
grep -q '"refresh": {"grant_type": "refresh_token"' "$T/cursor.jsonl" && echo "   ok  cursor refresh request" || { echo "   FAIL cursor refresh request"; fail=1; }
"$B" logout cursor >/dev/null 2>&1 </dev/null
cp "$T/models.yml.bak" "$T/agent/models.yml"
# structured reads: archives, SQLite, PDF, notebooks (fixtures generated with Python's zipfile/tarfile/sqlite3)
if python3 -I "$ROOT/tests/readfmt_fixtures.py" fx >/dev/null 2>&1; then
    check "read zip list"     "hello.txt (16B)"                "$B" --model mock/mock-1 -p "read:fx/t.zip"
    check "read zip member"   "row 12"                         "$B" --model mock/mock-1 -p "read:fx/t.zip:many.txt:12-12"
    check "read zip missing"  "not found inside archive"       "$B" --model mock/mock-1 -p "read:fx/t.zip:nope.txt"
    check "read tgz symlink"  "alpha tar"                      "$B" --model mock/mock-1 -p "read:fx/t.tgz:d/link.txt"
    check "read sqlite list"  "users (3000 rows)"              "$B" --model mock/mock-1 -p "read:fx/t.db"
    check "read sqlite table" "[2995 more rows; append :users?limit=20&offset=5"  "$B" --model mock/mock-1 -p "read:fx/t.db:users"
    check "read sqlite row"   "name: user0042"                 "$B" --model mock/mock-1 -p "read:fx/t.db:users:42"
    check "read sqlite query" "| key499 | 249001 |"           "$B" --model mock/mock-1 -p "read:fx/t.db?q=SELECT * FROM kv ORDER BY v DESC LIMIT 1"
    check "read sqlite wal"   "wal row 5"                      "$B" --model mock/mock-1 -p "read:fx/wal.db:events?where=id%3E4"
    check "read pdf"          "Hello PDF world"                "$B" --model mock/mock-1 -p "read:fx/simple.pdf"
    check "read pdf page"     "From the form"                  "$B" --model mock/mock-1 -p "read:fx/simple.pdf:page2.png"
    check "read pdf objstm"   "Ünïcode ✓ ok"                   "$B" --model mock/mock-1 -p "read:fx/objstm.pdf"
    check "read pdf in zip"   "Second line"                    "$B" --model mock/mock-1 -p "read:fx/docs.zip:papers/simple.pdf"
    check "read notebook"     "# %%% [code] cell:9"            "$B" --model mock/mock-1 -p "read:fx/t.ipynb"
fi
# Memory (memory.backend=local): learn -> learned.md, startup pipeline (phase 1 + 2) -> memory_summary.md / skills,
# memory block in the next system prompt, memory:// reads; auto-learn manage_skill + private capture turn
MA="$T/magent"; mkdir -p "$MA" "$T/mwork"
cp "$T/agent/models.yml" "$MA/models.yml"
printf 'modelRoles:\n  default: mock/mock-1\nmemory:\n  backend: local\nmemories:\n  minRolloutIdleHours: 0\n  stage1LeaseSeconds: 1\n  phase2LeaseSeconds: 1\nautolearn:\n  enabled: true\n  autoContinue: true\n  minToolCalls: 1\n' > "$MA/config.yml"
MENV=(env OMC_CODING_AGENT_DIR="$MA")
MROOT="$MA/memories/--$(cd "$T/mwork" && pwd | sed 's#^/##; s#[/:]#-#g')--"
pushd "$T/mwork" >/dev/null
check "learn tool"      "Lesson stored."        "${MENV[@]}" "$B" --model mock/mock-1 -p 'call:learn:{"memory":"Run <make test> before commit; key_ab12cd34ef56gh78","context":"ci"}'
check "learned.md"      "- Run make test before commit; [REDACTED] _(context: ci)_"  cat "$MROOT/learned.md"
check "manage_skill"    'Created managed skill "e2e-skill" (managed-skills/e2e-skill/SKILL.md)'  "${MENV[@]}" "$B" --model mock/mock-1 -p 'call:manage_skill:{"action":"create","name":"e2e-skill","description":"Use for e2e <tests>","body":"Do the thing."}'
check "managed SKILL.md" $'---\nname: e2e-skill\ndescription: Use for e2e tests\n---\n\nDo the thing.'  cat "$MA/managed-skills/e2e-skill/SKILL.md"
check "managed listed"  "Do the thing."         "${MENV[@]}" "$B" --model mock/mock-1 -p "read:skill://e2e-skill"
check "manage dup"      "already exists"        "${MENV[@]}" "$B" --model mock/mock-1 -p 'call:manage_skill:{"action":"create","name":"e2e-skill","description":"d","body":"b"}'
# the previous runs are idle rollouts now: a slow prompt keeps this process alive while startup consolidates them.
# Each short -p run above also starts the memory pipeline and is torn down mid-flight; like omp, an aborted phase
# leaves its lease until it expires, which would block this run's consolidation. Short leases + a 2 s wait make
# that deterministic (it used to depend on how fast the filesystem under TMPDIR is).
sleep 2
check "memory startup"  "done"                  "${MENV[@]}" "$B" --model mock/mock-1 -p "slow:10:done"
for _ in $(seq 50); do [ -f "$MROOT/memory_summary.md" ] && break; sleep 0.2; done
check "memory summary"  "MOCK SUMMARY from consolidation"  cat "$MROOT/memory_summary.md"
check "memory skill"    "echo mock"             cat "$MROOT/skills/mock-skill/scripts/run.sh"
check "memory rollout"  "Mock rollout summary." bash -c "cat '$MROOT'/rollout_summaries/*-mock_slug.md"
check "memory raw"      "[REDACTED]"            cat "$MROOT/raw_memories.md"
check "memory evil path" "absent"               bash -c "[ -e '$MROOT/skills/evil' ] || [ -e '$MROOT/skills/mock-skill/evil' ] && echo present || echo absent"
check "memory read url" "MOCK SUMMARY"          "${MENV[@]}" "$B" --model mock/mock-1 -p "read:memory://root"
check "memory url trav" "not allowed"           "${MENV[@]}" "$B" --model mock/mock-1 -p "read:memory://root/../x"
"${MENV[@]}" "$B" --model mock/mock-1 -p "hello memory" </dev/null >/dev/null 2>&1
python3 -I -c '
import json,sys
rs=[json.loads(l) for l in open(sys.argv[1])]
r=[x for x in rs if x.get("tools") and "hello memory" in json.dumps(x["messages"][-1])][-1]
s=r["messages"][0]["content"]; s=s if isinstance(s,str) else "".join(p.get("text","") for p in s)
ok="Memory summary:\nMOCK SUMMARY from consolidation" in s and "Learned lessons" in s and "## Auto-Learn (experimental)" in s and "`memory://root`" in s
print("ok" if ok else "missing memory block")' "$T/req.jsonl" | grep -q '^ok$' && echo "   ok  memory prompt block" || { echo "   FAIL memory prompt block"; fail=1; }
# auto-continue capture: one tool call reaches minToolCalls; rpc mode keeps the session alive for the private turn
printf '%s\n' '{"id":"1","type":"prompt","message":"bash:echo cap"}' | (cat; sleep 3) | "${MENV[@]}" "$B" --model mock/mock-1 --mode rpc >/dev/null 2>&1
grep -q 'Automated capture turn' "$T/req.jsonl" && echo "   ok  autolearn capture turn" || { echo "   FAIL autolearn capture turn"; fail=1; }
python3 -I -c '
import json,sys
rs=[json.loads(l) for l in open(sys.argv[1])]
c=[x for x in rs if "Automated capture turn" in json.dumps(x.get("messages",[])[-1:])]
main=[x for x in rs if x.get("tools") and "Automated capture turn" not in json.dumps(x.get("messages",[]))]
same=bool(c) and bool(main) and c[-1]["tools"]==main[-1]["tools"]
print("capture tools same-as-main=%s has=%s" % (same, {"learn","manage_skill"} <= {t["function"]["name"] for t in c[-1]["tools"]} if c else None))' "$T/req.jsonl" | grep -q "same-as-main=True has=True" && echo "   ok  autolearn capture tools" || { echo "   FAIL autolearn capture tools"; fail=1; }
popd >/dev/null
# LSP (only when clangd is installed)
if command -v clangd >/dev/null; then
    # omp only configures servers whose root markers exist in the cwd (.clangd here)
    mkdir -p lspw && pushd lspw >/dev/null && : > .clangd
    printf 'static int add(int a, int b) { return a + b; }\nint main(void) { return add(1, 2); }\n' > l.c
    lsp() { echo "call:write:{\"path\":\"xd://lsp\",\"content\":$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$1")}"; }
    check "lsp status"      "Language servers: clangd (configured, not started)" "$B" --model mock/mock-1 -p "$(lsp '{"action":"status"}')"
    check "lsp definition"  $'Found 1 definition(s):\n  l.c:1:12\n    1: static int add'  "$B" --model mock/mock-1 -p "$(lsp '{"action":"definition","file":"l.c","line":2,"symbol":"add"}')"
    check "lsp symbols"     $'Symbols in l.c:\n\xe2\x9f\xb2 add int (int, int) @ line 1'  "$B" --model mock/mock-1 -p "$(lsp '{"action":"symbols","file":"l.c"}')"
    check "lsp no symbol"   'LSP error: Symbol "nope" not found on line 2'  "$B" --model mock/mock-1 -p "$(lsp '{"action":"hover","file":"l.c","line":2,"symbol":"nope"}')"
    check "lsp diagnostics" $'1 error(s):\n# bad.c\n  1:25 [error] [clang] Use of undeclared identifier \'nope\' (undeclared_var_use)' \
        bash -c "printf 'int main(void) { return nope; }\n' > bad.c; '$B' --model mock/mock-1 -p '$(lsp '{"action":"diagnostics","file":"bad.c"}')'"
    check "lsp write diags" $'\n\nLSP Diagnostics (1 error(s)):\n# bad2.c\n  1:25 [error] [clang] Use of undeclared identifier'  "$B" --model mock/mock-1 -p 'write:bad2.c:int main(void) { return nope; }'
    popd >/dev/null
fi
# credential shapes from logins -> provider request URL/headers (mock_providers.py)
source "$ROOT/tests/e2e/providers_transport.sh"
source "$ROOT/tests/e2e/providers_devin.sh"
source "$ROOT/tests/e2e/providers_gitlab.sh"
source "$ROOT/tests/e2e/images.sh"
source "$ROOT/tests/e2e/compaction.sh"
source "$ROOT/tests/e2e/date_reminder.sh"
source "$ROOT/tests/e2e/cli.sh"
source "$ROOT/tests/e2e/provider_knobs.sh"
source "$ROOT/tests/e2e/provider_threads.sh"
source "$ROOT/tests/e2e/provider_wire.sh"
source "$ROOT/tests/e2e/provider_responses_ws.sh"
source "$ROOT/tests/e2e/usage.sh"
source "$ROOT/tests/e2e/websearch.sh"
source "$ROOT/tests/e2e/settings.sh"
source "$ROOT/tests/e2e/stall.sh"
source "$ROOT/tests/e2e/mcp_cache.sh"
source "$ROOT/tests/e2e/first_request.sh"
# provider logins over stdin (omc login) against tests/e2e/mock_auth.py
APORT=$((PORT + 7)); python3 -I "$ROOT/tests/e2e/mock_auth.py" $APORT & APID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$APORT) 2>/dev/null && break; sleep 0.1; done
AB="http://127.0.0.1:$APORT"
mkdir -p "$T/lagent"
LENV=(env OMC_CODING_AGENT_DIR="$T/lagent" OMC_NO_BROWSER=1 OMC_AUTH_POLL_SCALE_MS=20 KIMI_CODE_OAUTH_HOST="$AB/kimi"
      OMC_AUTH_URL_REWRITE="https://api.x.ai=$AB/xaiapi,https://github.com=$AB/gh,https://api.github.com=$AB/ghapi,https://api.individual.githubcopilot.com=$AB/capi")
check "login api key"      "Logged in to xAI API"          bash -c "printf 'good-key\n' | ${LENV[*]} '$B' login xai"
check "login key stored"   '"key": "good-key"'            cat "$T/lagent/auth.json"
check "login key rejected" "xAI API key validation failed (401)"  bash -c "printf 'bad\n' | ${LENV[*]} '$B' login xai"
check "login device code"  "Enter code: ABCD-1234"        bash -c "${LENV[*]} '$B' login kimi-code </dev/null"
check "login device saved" '"refresh": "kimi-r1"'         cat "$T/lagent/auth.json"
check "login copilot"      "Logged in to GitHub Copilot" bash -c "printf '\n' | ${LENV[*]} '$B' login github-copilot"
check "login list"         "ChatGPT Plus/Pro (Codex, headless/device)"          "$B" login
kill $APID 2>/dev/null
# github tool + pr:// / issue:// through the fake `gh` (tests/e2e/bin-gh: canned acme/widgets data, never GitHub)
mkdir -p "$T/ghagent" "$T/ghcfg"; cp "$T/agent/models.yml" "$T/ghagent/"; printf 'github:\n  enabled: true\n' > "$T/ghagent/config.yml"
GENV=(env -u GH_TOKEN -u GITHUB_TOKEN -u GH_HOST PATH="$ROOT/tests/e2e/bin-gh:$PATH" GH_CONFIG_DIR="$T/ghcfg" OMC_CODING_AGENT_DIR="$T/ghagent")
check "gh repo_view"     "Default branch: main"  "${GENV[@]}" "$B" --model mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"repo_view\",\"repo\":\"acme/widgets\"}"}'
check "gh search"        "Labels: bug, p1"       "${GENV[@]}" "$B" --model mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"search_issues\",\"query\":\"crash\",\"repo\":\"acme/widgets\"}"}'
check "gh run_watch"     "All jobs passed."      "${GENV[@]}" "$B" --model mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"run_watch\",\"run\":\"1001\",\"repo\":\"acme/widgets\"}"}'
check "gh bad op"        "op must be github operation (was \"nope\")"  "${GENV[@]}" "$B" --model mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"nope\"}"}'
check "pr:// view"       "Review Comments (2)"   "${GENV[@]}" "$B" --model mock/mock-1 --no-session -p "read:pr://acme/widgets/12"
check "pr:// diff list"  "3. new.txt  +0 -0  [renamed]  (renamed from old.txt)"  "${GENV[@]}" "$B" --model mock/mock-1 --no-session -p "read:pr://acme/widgets/12/diff"
check "issue:// list"    "# Issues in acme/widgets (open, up to 30)"  "${GENV[@]}" "$B" --model mock/mock-1 --no-session -p "read:issue://acme/widgets"
check "issue:// error"   "issue:// resolution failed: GraphQL: Could not resolve"  "${GENV[@]}" "$B" --model mock/mock-1 --no-session -p "read:issue://acme/widgets/999"
# gh's default repo pick: origin over an upstream it is no fork of; a fork's upstream stays (fake gh logs its argv)
mkdir -p "$T/ghorigin/bin" "$T/ghorigin/w" && cat > "$T/ghorigin/bin/gh" <<'SH'
#!/bin/sh
echo "$*" >> "$FAKE_GH_LOG"
case "$*" in
    "repo view --json url -q .url") echo https://github.com/merged/from ;;
    "repo view git@github.com:me/fork.git --json url,parent") printf '{"url":"https://github.com/me/fork","parent":%s}\n' "$FAKE_GH_PARENT" ;;
    api*) echo '{"items":[],"total_count":0}' ;;
esac
SH
chmod +x "$T/ghorigin/bin/gh"
(cd "$T/ghorigin/w" && git init -q . && git remote add origin git@github.com:me/fork.git && git remote add upstream https://github.com/merged/from.git)
GORIGIN=(env -u GH_TOKEN -u GITHUB_TOKEN -u GH_HOST HOME="$T" PATH="$T/ghorigin/bin:$PATH" GH_CONFIG_DIR="$T/ghcfg" OMC_CODING_AGENT_DIR="$T/ghagent" FAKE_GH_LOG="$T/ghorigin/log")
gh_search_q() { : > "$T/ghorigin/log"; (cd "$T/ghorigin/w" && "${GORIGIN[@]}" FAKE_GH_PARENT="$1" timeout 30 "$B" --model mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"search_prs\",\"query\":\"bug\"}"}' </dev/null >/dev/null 2>&1); cat "$T/ghorigin/log"; }
gh_search_q null > "$T/ghorigin/a"
check "gh prefers origin"  "repo:me/fork"  cat "$T/ghorigin/a"
gh_search_q '{"name":"from","owner":{"login":"merged"}}' > "$T/ghorigin/b"
check "gh keeps fork parent"  "repo:merged/from"  cat "$T/ghorigin/b"
check "gh needs enable"  "No such tool: xd://github"  env PATH="$ROOT/tests/e2e/bin-gh:$PATH" "$B" --model mock/mock-1 --no-session -p 'call:write:{"path":"xd://github","content":"{\"op\":\"repo_view\"}"}'
# implicit local servers (ollama / llama.cpp / LM Studio discovery) against tests/e2e/mock_local.py
DPORT=$((PORT + 9)); python3 -I "$ROOT/tests/e2e/mock_local.py" $DPORT & DPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$DPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/dagent"
DENV=(env -u OMC_NO_LOCAL_DISCOVERY OMC_CODING_AGENT_DIR="$T/dagent" OLLAMA_BASE_URL="http://127.0.0.1:$DPORT/ollama"
      LLAMA_CPP_BASE_URL="http://127.0.0.1:$DPORT/llama" LM_STUDIO_BASE_URL="http://127.0.0.1:$DPORT/lms/v1")
# omp's loopback metadata probes (ollama /api/show, LM Studio /api/v0/models) have 150-250 ms budgets, which a loaded
# test machine can miss (the model then falls back to defaults); retry the fresh discovery until every provider's
# detail probe landed, so the checks test parsing, not scheduling
# `models --json` rows as "provider id ctx=N [reasoning] [vision]"
mrows() { python3 -I -c 'import json,sys
for m in json.load(sys.stdin)["models"]:
    print(m["provider"], m["id"], "ctx=%s" % m["contextWindow"], *(["reasoning"] if m["reasoning"] else []), *(["vision"] if "image" in m["input"] else []))'; }
dmodels() { local o; for _ in 1 2 3 4 5; do rm -f "$T/dagent/local-models.json"; o=$("${DENV[@]}" "$B" models --json 2>/dev/null | mrows)
            [[ $o == *"ctx=40960 reasoning"* && $o == *"qwen2.5-vl-7b ctx=12000"* && $o == *"ctx=16384"* ]] && break; done; printf '%s\n' "$o"; }
DOUT=$(dmodels)
check "discover ollama"    "ollama qwen3:8b ctx=40960 reasoning"  printf '%s\n' "$DOUT"
check "discover llama.cpp" "llama.cpp gemma-local.gguf ctx=16384"  printf '%s\n' "$DOUT"
check "discover lm-studio" "lm-studio qwen2.5-vl-7b ctx=12000 vision"  printf '%s\n' "$DOUT"
check "discover reported limits" "lm-studio reported-limits ctx=54321"  printf '%s\n' "$DOUT"
check "discovery cache"    '"baseUrl": "http://127.0.0.1:'"$DPORT"'/lms/v1"'  python3 -I -m json.tool "$T/dagent/local-models.json"
kill $DPID 2>/dev/null
check "cached when down"   "│ llava:7b"  "${DENV[@]}" "$B" models
# a login refreshes that provider's models in the running session (selector-controller.ts refreshProvider), with its key
mkdir -p "$T/lragent" "$T/lrwork"; cp "$T/agent/models.yml" "$T/lragent/"; printf 'setupVersion: 2\n' > "$T/lragent/config.yml"
LRPORT=$((PORT + 41))
( cd "$T/lrwork" && check "login refresh" "login refresh ok" python3 -I "$ROOT/tests/e2e/login_refresh.py" "$B" "$ROOT/tests/e2e/mock_local.py" "$T/lragent" "$LRPORT" )
check "perf recorded"      '"mock/mock-1"'  cat "$T/agent/model-perf.json"
# default-on agent guards and auto thinking (WP-3 / WP-12); request_diff.sh checks the same scenarios against omp
REQS='import json,sys; r=[json.loads(l) for l in open(sys.argv[1])][int(sys.argv[2]):]; r=[x for x in r if x.get("tools") or len(sys.argv)>3]
def t(m):
    c=m.get("content"); return c if isinstance(c,str) else "".join(b.get("text","") for b in c or [])
print(len(r)); [print(x.get("reasoning_effort","-"), "|".join(t(m) for m in x["messages"][1:])) for x in r]'
mark() { wc -l < "$T/req.jsonl"; }
n=$(mark); "$B" --model mock/mock-1 --no-session -p 'say:ok ultrathink and `orchestrate`' >/dev/null 2>&1 </dev/null
check "ultrathink notice" "<system-notice>"$'\n'"Multi-step reasoning"  python3 -I -c "$REQS" "$T/req.jsonl" $n
out=$(python3 -I -c "$REQS" "$T/req.jsonl" $n); [[ "$out" != *"orchestration request"* ]] && echo "   ok  keyword in code span" || { echo "   FAIL keyword in code span"; fail=1; }
n=$(mark); "$B" --model mock/mock-1 --no-session -p 'say:' >/dev/null 2>&1 </dev/null
check "empty stop retry" "Stopped without actionable output; task incomplete."  python3 -I -c "$REQS" "$T/req.jsonl" $n
n=$(mark); "$B" --model mock/mock-1 --no-session -p 'calls:[["bash",{"command":"echo same"}],["bash",{"command":"echo same"}],["bash",{"command":"echo same"}],["bash",{"command":"echo same"}],["bash",{"command":"echo same"}],["bash",{"command":"echo same"}]]' >/dev/null 2>&1 </dev/null
check "tool-call loop guard" 'You called `bash` 5 consecutive times with identical arguments:'  python3 -I -c "$REQS" "$T/req.jsonl" $n
check "todo reminder (rpc)" '"type": "todo_reminder", "todos": [{"content": "alpha", "status": "in_progress"}, {"content": "beta", "status": "pending"}], "attempt": 1, "maxAttempts": 3'  python3 -I "$ROOT/tests/parity/rpc_drive.py" "$B" "todo:alpha,beta" 1
n=$(mark); "$B" --model mock/mock-think --thinking auto --no-session -p 'say:medium sized change' >/dev/null 2>&1 </dev/null
check "auto thinking judge" $'2\nlow State:\n<request>say:medium sized change</request>'  python3 -I -c "$REQS" "$T/req.jsonl" $n all
check "auto thinking level" "medium <system-reminder>"  python3 -I -c "$REQS" "$T/req.jsonl" $n
n=$(mark); "$B" --model mock/mock-think --thinking auto --no-session -p 'say:please ultrathink' >/dev/null 2>&1 </dev/null
check "ultrathink max level" "xhigh <system-notice>"  python3 -I -c "$REQS" "$T/req.jsonl" $n
n=$(mark); "$B" --model mock/mock-1 --no-session -p 'think:abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij |never' >/dev/null 2>&1 </dev/null
check "thinking loop redirect" 'reason="thinking_loop_detected"'  python3 -I -c "$REQS" "$T/req.jsonl" $n
# internal URLs: out:// (on when configured), cfg:// view, conflict:// without a registered block
printf 'out:\n  enabled: true\n' > "$T/agent/config.yml"
n=$(mark); "$B" --model mock/mock-1 --no-session -p 'calls:[["write",{"path":"out://","content":"halfway there"}]]' >/dev/null 2>&1 </dev/null
check "out:// write"  "Shown to the user."  python3 -I -c "$REQS" "$T/req.jsonl" $n
rm -f "$T/agent/config.yml"
check "cfg:// read"   "maxRetries: 10"  "$B" --model mock/mock-1 --no-session -p "read:cfg://retry"
check "conflict:// unknown"  "Conflict #1 not found: it was resolved"  "$B" --model mock/mock-1 --no-session -p "read:conflict://1"
# turn-lifecycle signals (WP-11): terminal title + run state, OSC 7501, completion / error notifications, under a pty
if script --version 2>/dev/null | grep -q util-linux; then
    tui_signals() { # out-file prompt env...
        local out=$1 prompt=$2; shift 2
        rm -f "$T/sig.fifo"; mkfifo "$T/sig.fifo"
        env -u TMUX -u STY -u ZELLIJ -u TERM_PROGRAM -u KITTY_WINDOW_ID -u WEZTERM_PANE -u GHOSTTY_RESOURCES_DIR \
            -u LC_TERMINAL -u HERDR_PANE_ID -u CMUX_SURFACE_ID TERM=xterm-256color PI_NO_DESKTOP_NOTIFY=1 OMC_SKIP_SETUP=1 "$@" \
            timeout 30 script -qfec "$B --model mock/mock-1 --no-session '$prompt'" "$out" < "$T/sig.fifo" >/dev/null 2>&1 &
        local pid=$!
        exec 7>"$T/sig.fifo"
        for _ in $(seq 150); do grep -qa $'7501;state=\\(done\\|error\\)' "$out" 2>/dev/null && break; sleep 0.1; done
        sleep 0.3; printf '\004' >&7; exec 7>&-
        wait $pid 2>/dev/null
    }
    tui_signals "$T/sig1.out" "bash:echo signals"
    check "turn signals"  "signals ok"  python3 -I "$ROOT/tests/e2e/signals_check.py" "$T/sig1.out" turn
    printf 'error:\n  notify: "on"\n' > "$T/agent/config.yml"
    tui_signals "$T/sig2.out" "fail:400:bad things" TERM_PROGRAM=WezTerm
    rm -f "$T/agent/config.yml"
    check "error signals" "signals ok"  python3 -I "$ROOT/tests/e2e/signals_check.py" "$T/sig2.out" error
fi
# editor I/O (WP-6) under a pty: Ctrl+G, Ctrl+R, paste chip / menu, raw paste, `#` actions and refs, `@` mentions
mkdir -p "$T/eio"
check "editor io" "editor io ok"  python3 -I "$ROOT/tests/e2e/editor_io.py" "$B" "$T/req.jsonl" "$ROOT/tests/e2e/fake_editor.sh" "$T/eio"
# /loop (WP-23: count budget, --until / --while conditions, Esc pause) and /copy picker, /copy targets, /open (WP-20)
mkdir -p "$T/lc"
check "loop mode" "loop ok"  python3 -I "$ROOT/tests/e2e/loop_copy_tui.py" "$B" "$T/req.jsonl" "$T/lc" loop
check "copy + open" "copy ok"  python3 -I "$ROOT/tests/e2e/loop_copy_tui.py" "$B" "$T/req.jsonl" "$T/lc" copy
exit $fail
