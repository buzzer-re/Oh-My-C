# Sourced by tests/e2e/run.sh: provider classifier refusals (Anthropic stop_reason "refusal") against mock_providers.py:
# a mid-stream reasoning_extraction refusal is resampled once on the same model (omp 92fe816a0e), the refused turn is
# kept as a provider_refusal custom entry instead of a session message (omp 6b28554ed7), and never replayed. A
# reasoning_extraction refusal of a request replaying another model's reasoning as text is resent at once without it,
# and that reasoning stays out of the session's later Claude requests (omp 873763609a, 69f22a4733). Other refusals walk
# the fallback chain, each model at most once, and never retry the refused model.
RFPORT=$((PORT + 31))
MOCK_LOG="$T/rfprov.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $RFPORT & RFPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$RFPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/rfagent/sessions"
cat > "$T/rfagent/models.yml" <<YML
providers:
  mocka:
    baseUrl: http://127.0.0.1:$RFPORT
    apiKey: MOCK_KEY
    api: anthropic-messages
    models:
      - id: mock-a
        contextWindow: 100000
        maxTokens: 4096
YML
check "refusal resampled" "plain provider answer"  env OMC_CODING_AGENT_DIR="$T/rfagent" "$B" --model mocka/mock-a -p "refuse:once"
check "refusal record" "records=1 category=reasoning_extraction token=False assistant_errors=0 requests=2" python3 -I -c '
import json,glob,sys
es=[json.loads(l) for f in glob.glob(sys.argv[1]+"/**/*.jsonl",recursive=True) for l in open(f) if l.strip()]
rec=[e for e in es if e.get("type")=="custom" and e.get("customType")=="provider_refusal"]
errs=[e for e in es if e.get("type")=="message" and e["message"].get("stopReason")=="error"]
reqs=[r for r in map(json.loads, open(sys.argv[2])) if r["body"].get("tools")]
d=rec[0]["data"] if rec else {}
print("records=%d category=%s token=%s assistant_errors=%d requests=%d" % (len(rec), d.get("category"), "fct-1" in json.dumps(d), len(errs), len(reqs)))' "$T/rfagent/sessions" "$T/rfprov.jsonl"
mkdir -p "$T/rfcot/sessions"
cat > "$T/rfcot/models.yml" <<YML
providers:
  mocka:
    baseUrl: http://127.0.0.1:$RFPORT
    apiKey: MOCK_KEY
    api: anthropic-messages
    models:
      - id: mock-a
        contextWindow: 100000
        maxTokens: 4096
      - id: claude-sonnet-4-5
        contextWindow: 100000
        maxTokens: 4096
YML
: > "$T/rfprov.jsonl"
check "cot carried" "plain provider answer" env OMC_CODING_AGENT_DIR="$T/rfcot" "$B" --model mocka/mock-a -p "think:carry"
check "cot strip retry" "plain provider answer" env OMC_CODING_AGENT_DIR="$T/rfcot" "$B" --model mocka/claude-sonnet-4-5 -c -p "refusecot:1" "refusecot:2"
# the refused request carried the reasoning as text; its retry and the next prompt's request leave it out. The refused
# turn, empty and retried, is also kept as a session message (persistTerminalEmptyErrorTurn) that replay drops
check "cot strip requests" "claude=3 carried=1,0,0 refusals=1 errors=1" python3 -I -c '
import json,glob,sys
reqs=[r["body"] for r in map(json.loads, open(sys.argv[2])) if r["body"].get("tools") and r["body"].get("model")=="claude-sonnet-4-5"]
carried=[sum("Private chain of thought." in (b.get("text") or "") for m in r["messages"] if m["role"]=="assistant" for b in m["content"] if isinstance(m["content"],list)) for r in reqs]
es=[json.loads(l) for f in glob.glob(sys.argv[1]+"/**/*.jsonl",recursive=True) for l in open(f) if l.strip()]
rec=[e for e in es if e.get("type")=="custom" and e.get("customType")=="provider_refusal"]
errs=[e for e in es if e.get("type")=="message" and e["message"].get("stopReason")=="error"]
print("claude=%d carried=%s refusals=%d errors=%d" % (len(reqs), ",".join(map(str,carried)), len(rec), len(errs)))' "$T/rfcot/sessions" "$T/rfprov.jsonl"
# a classifier refusal walks retry.fallbackChains instead of retrying the refused model, and visits any one model at
# most once per turn: chains naming each other stop after both refused; no chain ends the turn after one request
mkdir -p "$T/rfwalk"
cat > "$T/rfwalk/models.yml" <<YML
providers:
  mocka:
    baseUrl: http://127.0.0.1:$RFPORT
    apiKey: MOCK_KEY
    api: anthropic-messages
    models:
      - id: mock-a
        contextWindow: 100000
        maxTokens: 4096
      - id: mock-b
        contextWindow: 100000
        maxTokens: 4096
YML
printf 'composer:\n  predictions: false\nretry:\n  fallbackChains:\n    mocka/mock-a:\n      - mocka/mock-b\n    mocka/mock-b:\n      - mocka/mock-a\n' > "$T/rfwalk/config.yml"
rfmodels() { python3 -I -c 'import json,sys; print(",".join(r["body"]["model"] for r in map(json.loads, open(sys.argv[1])) if r["body"].get("tools")))' "$T/rfprov.jsonl"; }
: > "$T/rfprov.jsonl"
out=$(timeout 60 env OMC_CODING_AGENT_DIR="$T/rfwalk" "$B" --model mocka/mock-a --no-session -p "refuseon:mock-a:walk" </dev/null 2>&1)
[[ "$out" == *"Retrying (1/10): Refusal (cyber): declined by mock-a"* && "$out" == *"plain provider answer"* && "$(rfmodels)" == "mock-a,mock-b" ]] &&
    echo "   ok  refusal walks the chain" || { echo "   FAIL refusal walks the chain: $(rfmodels) $out"; fail=1; }
: > "$T/rfprov.jsonl"
out=$(timeout 60 env OMC_CODING_AGENT_DIR="$T/rfwalk" "$B" --model mocka/mock-a --no-session -p "refuseon:mock-a,mock-b:walk" </dev/null 2>&1)
[[ "$out" == *"Refusal (cyber): declined by mock-b"* && "$(rfmodels)" == "mock-a,mock-b" ]] &&
    echo "   ok  refusal walk visits each model once" || { echo "   FAIL refusal walk visits each model once: $(rfmodels) $out"; fail=1; }
printf 'composer:\n  predictions: false\n' > "$T/rfwalk/config.yml"
: > "$T/rfprov.jsonl"
out=$(timeout 60 env OMC_CODING_AGENT_DIR="$T/rfwalk" "$B" --model mocka/mock-a --no-session -p "refuseon:mock-a:walk" </dev/null 2>&1)
[[ "$out" == *"Refusal (cyber): declined by mock-a"* && "$out" != *"Retrying"* && "$(rfmodels)" == "mock-a" ]] &&
    echo "   ok  refusal without a chain ends the turn" || { echo "   FAIL refusal without a chain ends the turn: $(rfmodels) $out"; fail=1; }
# an endpoint that refuses strict tools: the request is resent without them, and later requests to it send none
mkdir -p "$T/rfstrict"
sed "s|baseUrl: http://127.0.0.1:$RFPORT|baseUrl: http://127.0.0.1:$RFPORT/nostrict|" "$T/rfwalk/models.yml" > "$T/rfstrict/models.yml"
printf 'composer:\n  predictions: false\n' > "$T/rfstrict/config.yml"
: > "$T/rfprov.jsonl"
out=$(timeout 60 env OMC_CODING_AGENT_DIR="$T/rfstrict" "$B" --model mocka/mock-a --no-session -p "strict check" </dev/null 2>&1)
st=$(python3 -I -c 'import json,sys; print(",".join(str(any(t.get("strict") for t in r["body"]["tools"])) for r in map(json.loads, open(sys.argv[1])) if r["body"].get("tools")))' "$T/rfprov.jsonl")
[[ "$out" == *"plain provider answer"* && "$st" == "True,False" ]] && echo "   ok  strict tools refused" || { echo "   FAIL strict tools refused: $st $out"; fail=1; }
kill $RFPID 2>/dev/null
