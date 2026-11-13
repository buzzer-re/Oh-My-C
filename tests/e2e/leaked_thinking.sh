# Sourced by tests/e2e/run.sh: reasoning markup a model leaks into its visible text becomes thinking blocks live
# (omp utils/leaked-thinking-stream.ts, StreamMarkupHealing), except on official first-party endpoints.
LT_SESS="$T/ltsessions"
mkdir -p "$LT_SESS" "$T/lt-agent"
cat > "$T/lt_check.py" <<'PY'
import json, glob, os, sys
mode, arg = sys.argv[1], sys.argv[2]
if mode == "saved":  # the newest session's assistant content
    f = sorted(glob.glob(os.path.join(arg, "**", "*.jsonl"), recursive=True), key=os.path.getmtime)[-1]
    for l in open(f):
        e = json.loads(l)
        if e.get("type") == "message" and e["message"]["role"] == "assistant": print(json.dumps(e["message"]["content"]))
else:  # the assistant turns of the last main request, as the model sees them again
    req = [r for r in (json.loads(l) for l in open(arg)) if r.get("tools")][-1]
    print(json.dumps([m for m in req["messages"] if m["role"] == "assistant"]))
PY
lt_run() { # session-dir model prompt [env...]
    local dir=$1 model=$2 prompt=$3; shift 3
    env "$@" timeout 30 "$B" --model "$model" --session-dir "$dir" -p "$prompt" </dev/null >/dev/null 2>&1
}
lt_run "$LT_SESS/a" mock/mock-1 'chunks:["<thin", "king>step one", " and two</thi", "nking>The answer."]'
check "leaked tag healed" '[{"type": "thinking", "thinking": "step one and two", "summary": false}, {"type": "text", "text": "The answer."}]' \
    python3 -I "$T/lt_check.py" saved "$LT_SESS/a"
lt_run "$LT_SESS/b" mock/mock-1 'chunks:["reasoning behind the opener", "</think>Answer."]'
check "prefilled opener" '[{"type": "thinking", "thinking": "reasoning behind the opener"}, {"type": "text", "text": "Answer."}]' \
    python3 -I "$T/lt_check.py" saved "$LT_SESS/b"
lt_run "$LT_SESS/c" mock/mock-1 'chunks:["```thinking\nfenced\n", "```\nreply"]'
check "thinking fence" '[{"type": "thinking", "thinking": "fenced\n", "summary": false}, {"type": "text", "text": "\nreply"}]' \
    python3 -I "$T/lt_check.py" saved "$LT_SESS/c"
lt_run "$LT_SESS/d" mock/mock-1 'chunks:[["r", "native"], "<think>leak</think>visible"]'
check "native wins" '[{"type": "thinking", "thinking": "native", "thinkingSignature": "reasoning_content", "summary": false}, {"type": "text", "text": "visible"}]' \
    python3 -I "$T/lt_check.py" saved "$LT_SESS/d"
(cd "$T/work" && timeout 30 "$B" --model mock/mock-1 --session-dir "$LT_SESS/a" -c -p "say:again" </dev/null >/dev/null 2>&1)
check "healed replay" '{"role": "assistant", "content": "The answer.", "reasoning_content": "step one and two"}' \
    python3 -I "$T/lt_check.py" wire "$T/req.jsonl"
# the official OpenAI endpoint (rewritten to the mock) returns structured reasoning: nothing is healed
sed -e 's/^  mock:$/  openai:/' -e "s#baseUrl: http://127.0.0.1:$PORT/v1#baseUrl: https://api.openai.com/v1#" "$T/agent/models.yml" > "$T/lt-agent/models.yml"
lt_run "$LT_SESS/e" openai/mock-1 'chunks:["<think>abc</think>tail"]' OMC_CODING_AGENT_DIR="$T/lt-agent" \
    OMC_PROVIDER_URL_REWRITE="https://api.openai.com=http://127.0.0.1:$PORT"
check "official endpoint" '[{"type": "text", "text": "<think>abc</think>tail"}]' python3 -I "$T/lt_check.py" saved "$LT_SESS/e"
