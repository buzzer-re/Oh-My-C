# Sourced by tests/e2e/run.sh: mid-stream connection drops (turn-recovery.ts). The mocks stream part of a turn
# and then stall silently, close (FIN before the last chunk) or reset the connection; a 1 s idle watchdog catches
# the stall. Text already shown continues with the stream-stall-continue reminder (max 3 per prompt), a tool turn
# resumes with the synthetic `executed: false` results, a drop before any output is retried, and print-mode text
# (buffered, not yet shown) replays the turn. Checked against omp 18.14.2 with the same mocks.
STPORT=$((PORT + 23)); STAPORT=$((PORT + 24))
MOCK_LOG="$T/stall-oai.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $STPORT & STPID=$!
MOCK_LOG="$T/stall-ant.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $STAPORT & STAPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$STPORT) 2>/dev/null && (exec 3<>/dev/tcp/127.0.0.1/$STAPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/stagent"
cat > "$T/stagent/models.yml" <<YML
providers:
  mock:
    baseUrl: http://127.0.0.1:$STPORT/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: mock-1
        contextWindow: 100000
        maxTokens: 4096
  mockant:
    baseUrl: http://127.0.0.1:$STAPORT
    apiKey: MOCK_KEY
    api: anthropic-messages
    models:
      - id: claude-mock
        contextWindow: 200000
        maxTokens: 4096
YML
printf 'providers:\n  streamIdleTimeoutSeconds: 1\nretry:\n  baseDelayMs: 50\n' > "$T/stagent/config.yml"
STENV=(env OMC_CODING_AGENT_DIR="$T/stagent")
STREQS=(python3 -I "$ROOT/tests/e2e/stall_reqs.py")
SJ=("${STENV[@]}" "$B" --model mock/mock-1 --no-session --mode json -p)
check "stall text continues"  '"resumed after drop'  "${SJ[@]}" "drop:stall:1:Partial stall text"
check "stall text error"      '"errorMessage":"OpenAI completions stream stalled while waiting for the next event"'  bash -c "grep -o '\"errorMessage\":\"[^\"]*\"' <(${SJ[*]} 'drop:stall:1:Partial stall two') | head -1"
check "stall text request"    "n=2 last=user,assistant,user reminders=Attempt #1/3"  "${STREQS[@]}" "$T/stall-oai.jsonl" "drop:stall:1:Partial stall text"
check "close text continues"  'ECONNRESET: The socket connection was closed unexpectedly. For more information, pass `verbose: true` in the second argument to fetch()'  "${SJ[@]}" "drop:close:1:Partial close text"
check "close text request"    "n=2 last=user,assistant,user reminders=Attempt #1/3"  "${STREQS[@]}" "$T/stall-oai.jsonl" "drop:close:1:Partial close text"
check "rst text continues"    '"resumed after drop'  "${SJ[@]}" "drop:rst:1:Partial rst text"
check "rst text request"      "n=2 last=user,assistant,user reminders=Attempt #1/3"  "${STREQS[@]}" "$T/stall-oai.jsonl" "drop:rst:1:Partial rst text"
# past STREAM_STALL_CONTINUE_MAX_RETRIES the error surfaces (no hang, no fourth reminder)
check "stall cap"             '"stopReason":"error"'  bash -c "${SJ[*]} 'drop:close:4:Partial cap text' | grep -o '\"type\":\"agent_end\".*' | grep -o '\"stopReason\":\"error\"' | tail -1"
check "stall cap requests"    "n=4 last=user,assistant,user,assistant,user,assistant,user reminders=Attempt #1/3Attempt #2/3Attempt #3/3"  "${STREQS[@]}" "$T/stall-oai.jsonl" "drop:close:4:Partial cap text"
# a tool call streamed before the drop never runs: its synthetic result is sent back and the turn resumes; the replay
# keeps the "not executed" fact without the provider's error text (omp 9bed2a4d8f), which stays in the session
check "stall tool resumes"    "Tool said: Tool call was not executed because the provider stream ended with an error before the tool could run."  "${STENV[@]}" "$B" --model mock/mock-1 --no-session -p "dropcall:stall:1:touch $T/stall-ran"
check "stall tool not run"    "absent"  bash -c "[ -e '$T/stall-ran' ] && echo ran || echo absent"
check "stall tool request"    "n=2 last=user,assistant,tool reminders="  "${STREQS[@]}" "$T/stall-oai.jsonl" "dropcall:stall:1:touch"
check "close tool resumes"    "Tool said: Tool call was not executed because the provider stream ended with an error before the tool could run."  "${STENV[@]}" "$B" --model mock/mock-1 --no-session -p "dropcall:close:1:echo close-tool"
check "close tool details"    '"details":{"__synthetic":true,"source":"assistant_stop_error","executed":false,"upstreamError":"ECONNRESET'  "${SJ[@]}" "dropcall:rst:1:echo rst-tool"
# nothing streamed yet: the transport retries the request; print-mode text is buffered, so the turn replays
check "drop before output"    "resumed after drop"  "${STENV[@]}" "$B" --model mock/mock-1 --no-session -p "drop:close:1:"
check "print mode replays"    "resumed after drop"  "${STENV[@]}" "$B" --model mock/mock-1 --no-session -p "drop:close:1:Buffered print text"
check "print replay request"  "n=2 last=user reminders="  "${STREQS[@]}" "$T/stall-oai.jsonl" "drop:close:1:Buffered print text"
# Anthropic Messages: same recovery over the anthropic transport
SA=("${STENV[@]}" "$B" --model mockant/claude-mock --no-session --mode json -p)
check "anthropic stall"       'Anthropic stream stalled while waiting for the next event'  "${SA[@]}" "drop:stall:1:Anthropic partial"
check "anthropic stall req"   "n=2 last=user,assistant,user reminders=Attempt #1/3"  "${STREQS[@]}" "$T/stall-ant.jsonl" "drop:stall:1:Anthropic partial"
check "anthropic rst"         '"resumed after drop"'  "${SA[@]}" "drop:rst:1:Anthropic reset"
kill $STPID $STAPID 2>/dev/null
