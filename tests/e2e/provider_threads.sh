# Sourced by tests/e2e/run.sh: Anthropic message threads (message-threads-2026-08-12) against mock_anthropic_threads.py,
# which stores each conversation like the API and rejects a continue that does not extend it. Shapes checked against
# omp v18.17.0 with the same mock (PI_ANTHROPIC_MESSAGE_THREADS=1 reaches the mock; the default needs the official URL).
THPORT=$((PORT + 41))
MOCK_LOG="$T/mt.jsonl" python3 -I "$ROOT/tests/e2e/mock_anthropic_threads.py" $THPORT & THPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$THPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/mtagent" "$T/mthome"
printf 'skills:\n  enabled: false\n' > "$T/mtagent/config.yml"
THENV=(env HOME="$T/mthome" OMC_CODING_AGENT_DIR="$T/mtagent" OMC_NO_TITLE=1 ANTHROPIC_API_KEY=sk-ant-api-fake
       ANTHROPIC_BASE_URL=http://127.0.0.1:$THPORT PI_ANTHROPIC_MESSAGE_THREADS=1)
# run a prompt with the given env prefix, then print the request kinds, oldest first, and any wire problem the mock saw
th_run() { # prompt env-and-binary...
    local prompt=$1; shift
    : > "$T/mt.jsonl"
    timeout 30 "$@" "$B" --model anthropic/claude-sonnet-4-6 --no-session -p "$prompt" </dev/null >"$T/mt.out" 2>&1
    python3 -I -c 'import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; print(" ".join(x["kind"] for x in r), *[x["problem"] for x in r if x["problem"]])' "$T/mt.jsonl"
}
th_check() { # name want prompt env...
    local name=$1 want=$2 prompt=$3; shift 3
    local out; out=$(th_run "$prompt" "$@" 2>&1)
    if [[ "$out" == "$want" ]]; then echo "   ok  $name"; else echo "   FAIL $name: wanted '$want', got: $out"; fail=1; fi
}
th_check "threads continue"  "create continue continue continue"  "chain:3"  "${THENV[@]}"
th_check "threads thinking"  "create continue continue continue"  "chain:3 thinky"  "${THENV[@]}"
check "threads beta"         "message-threads-2026-08-12"  python3 -I -c 'import json,sys; print(json.loads(open(sys.argv[1]).readline())["headers"]["anthropic-beta"])' "$T/mt.jsonl"
check "threads continue body" "None None continue 1"  python3 -I -c 'import json,sys; b=json.loads(open(sys.argv[1]).readlines()[1])["body"]; print(b.get("system"), b.get("tools"), b["thread"]["type"], len(b["messages"]))' "$T/mt.jsonl"
check "threads metadata"     'session_id'  python3 -I -c 'import json,sys; print(json.loads(open(sys.argv[1]).readlines()[1])["body"]["metadata"]["user_id"])' "$T/mt.jsonl"
# a turn of parallel tool calls: the first appended block reads the previous entry, the newest one writes this turn
th_check "threads many"      "create continue"  "many:30"  "${THENV[@]}"
check "threads breakpoints"  "0 29"  python3 -I -c 'import json,sys; b=json.loads(open(sys.argv[1]).readlines()[-1])["body"]; print(*[j for j,x in enumerate(b["messages"][0]["content"]) if x.get("cache_control")])' "$T/mt.jsonl"
th_check "threads replay"    "create continue create"  "bash:echo refuse-continue"  "${THENV[@]}"
th_check "threads refused"   "create plain plain"  "bash:echo unsupported"  "${THENV[@]}"
th_check "threads plain 400" "create plain plain"  "bash:echo plain400"  "${THENV[@]}"
th_check "threads pause"     "create"  "paused"  "${THENV[@]}"
# a gateway's x-should-retry: false ends the Anthropic transport loop at once (session retries off here; omp makes the
# same 11 requests with them on: the first plus 10 session retries)
mkdir -p "$T/mtagent2"
printf 'skills:\n  enabled: false\nretry:\n  enabled: false\n' > "$T/mtagent2/config.yml"
th_check "x-should-retry anthropic" "plain"  "noretry"  "${THENV[@]}" PI_ANTHROPIC_MESSAGE_THREADS=0 OMC_CODING_AGENT_DIR="$T/mtagent2"
th_check "threads env off"   "plain plain"  "bash:echo off"  "${THENV[@]}" PI_ANTHROPIC_MESSAGE_THREADS=0
# on by default on the official endpoint; a gateway reroute keeps the model flag on but must not see the field
THDEF=(env HOME="$T/mthome" OMC_CODING_AGENT_DIR="$T/mtagent" OMC_NO_TITLE=1 ANTHROPIC_API_KEY=sk-ant-api-fake)
th_check "threads default"   "create continue"  "bash:echo default"  "${THDEF[@]}" OMC_PROVIDER_URL_REWRITE="https://api.anthropic.com=http://127.0.0.1:$THPORT"
th_check "threads gateway"   "plain plain"  "bash:echo gw"  "${THDEF[@]}" ANTHROPIC_BASE_URL=http://127.0.0.1:$THPORT
kill $THPID 2>/dev/null; wait $THPID 2>/dev/null
