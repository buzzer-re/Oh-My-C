# Sourced by tests/e2e/run.sh: provider wire behaviour with no better home.
WPORT=$((PORT + 42))
MOCK_FAIL_RETRY_AFTER_MS=none MOCK_SHOULD_RETRY=false MOCK_LOG="$T/wire.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $WPORT & WPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$WPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/wagent"
sed "s#127.0.0.1:$PORT/#127.0.0.1:$WPORT/#" "$T/agent/models.yml" > "$T/wagent/models.yml"
printf 'retry:\n  enabled: false\n' > "$T/wagent/config.yml"
# a gateway's x-should-retry: false is terminal: one request instead of the transport's six and the replay-safe retry
OMC_CODING_AGENT_DIR="$T/wagent" timeout 30 "$B" --model mock/mock-1 --no-session -p "fail:503:gateway gave up" </dev/null >/dev/null 2>&1
n=$(grep -c "gateway gave up" "$T/wire.jsonl")
(( n == 2 )) && echo "   ok  x-should-retry false" || { echo "   FAIL x-should-retry false: $n requests"; fail=1; }
kill $WPID 2>/dev/null; wait $WPID 2>/dev/null
