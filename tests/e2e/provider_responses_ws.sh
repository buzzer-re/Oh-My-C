# Sourced by tests/e2e/run.sh: opt-in standard Responses WebSockets against mock_responses_ws.py (a socket that holds the
# previous response per connection, with the HTTP/SSE endpoint behind it). Transport choices and incremental turns
# checked against omp v18.17.0 with the same mock.
RWPORT=$((PORT + 43))
mkdir -p "$T/rwagent" "$T/rwhome"
printf 'providers:\n  openai:\n    baseUrl: http://127.0.0.1:%s/v1\n' $RWPORT > "$T/rwagent/models.yml"
printf 'providers:\n  openaiResponsesWebsockets: true\nskills:\n  enabled: false\n' > "$T/rwagent/config.yml"
RWENV=(env HOME="$T/rwhome" OMC_CODING_AGENT_DIR="$T/rwagent" OMC_NO_TITLE=1 OPENAI_API_KEY=sk-fake)
rw_start() { MOCK_WS_UPGRADE="$1" MOCK_LOG="$T/rw.jsonl" python3 -I "$ROOT/tests/e2e/mock_responses_ws.py" $RWPORT & RWPID=$!; for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$RWPORT) 2>/dev/null && break; sleep 0.1; done; }
rw_stop() { kill $RWPID 2>/dev/null; wait $RWPID 2>/dev/null; }
# "<transport>:<input items>[:prev]" per request of the latest run
rw_run() { # prompt env...
    local prompt=$1; shift
    : > "$T/rw.jsonl"
    timeout 30 "$@" "$B" --model openai/gpt-5.5 --no-session -p "$prompt" </dev/null >"$T/rw.out" 2>&1
    python3 -I -c 'import json,sys
for l in open(sys.argv[1]):
    r=json.loads(l)
    if r["transport"]=="ws-handshake": print("up",end=" "); continue
    b=r["body"]; print(r["transport"]+("+prev" if b.get("previous_response_id") else "")+":"+str(len(b.get("input") or [])),end=" ")
print()' "$T/rw.jsonl"
}
rw_check() { # name want-substring prompt env...
    local name=$1 want=$2 prompt=$3; shift 3
    local out; out=$(rw_run "$prompt" "$@" 2>&1)
    if [[ "$out" == *"$want"* ]]; then echo "   ok  $name"; else echo "   FAIL $name: wanted '$want', got: $out / $(tail -3 "$T/rw.out")"; fail=1; fi
}
rw_start ""
rw_check "ws incremental"  "up ws:3 ws+prev:1 ws+prev:1"  "chain:2"  "${RWENV[@]}"
check "ws frame shape"     "response.create None None gpt-5.5"  python3 -I -c 'import json,sys; b=[json.loads(l) for l in open(sys.argv[1])][1]["body"]; print(b.get("type"), b.get("stream"), b.get("stream_options"), b["model"])' "$T/rw.jsonl"
check "ws session headers" "True True"  python3 -I -c 'import json,sys; h=json.loads(open(sys.argv[1]).readline())["headers"]; print("session_id" in h, "x-client-request-id" in h)' "$T/rw.jsonl"
rw_check "ws one socket"   "up ws:3 ws+prev:1"  "bash:echo one"  "${RWENV[@]}"
rw_check "ws stale anchor" "up ws:3 ws+prev:1 ws:5"  "bash:echo a stale-once"  "${RWENV[@]}"
rw_check "ws no chaining"  "up ws:3 ws:5"  "bash:echo nochain"  "${RWENV[@]}" PI_OPENAI_STATEFUL=0
rw_check "ws drop"         "up ws:3 http:3"  "drop-mid"  "${RWENV[@]}"
check "ws drop text"       "OpenAI responses stream closed before a terminal response event was received"  cat "$T/rw.out"
rw_check "ws rejection"    "up ws:3"  "reject"  "${RWENV[@]}"
check "ws rejection text"  "Error Code bad_param: request rejected"  cat "$T/rw.out"
# off by default, and a model's preferWebsockets: false vetoes the setting
printf 'skills:\n  enabled: false\n' > "$T/rwagent/config.yml"
rw_check "ws default off"  "http:3 http:5"  "bash:echo off"  "${RWENV[@]}"
printf 'providers:\n  openaiResponsesWebsockets: true\nskills:\n  enabled: false\n' > "$T/rwagent/config.yml"
rw_stop
rw_start 404
rw_check "ws upgrade 404"  "up http:3 http:5"  "bash:echo fall"  "${RWENV[@]}"
rw_stop
rw_start 401
rw_check "ws upgrade 401"  "up"  "bash:echo denied"  "${RWENV[@]}"
check "ws upgrade 401 text" "websocket upgrade rejected (HTTP 401)"  cat "$T/rw.out"
rw_stop
