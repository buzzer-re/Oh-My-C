# Sourced by tests/e2e/run.sh: thinking-capability probes for a model nothing classifies (omp 3f8862bf9e,
# packages/ai/src/thinking-probe.ts). A models.yml row without `reasoning` gets a provisional ladder; before its first
# request with a thinking level, 6 efforts plus a reasoning-off control are probed in parallel (openai-completions; the
# alternate formats only when none is accepted), the verdict is cached in <agent>/thinking-capability.json and applied.
# Expected requests are omp 18.22.0's against the same mock (verified side by side).
TP_DIR="$T/tp"
mkdir -p "$TP_DIR"
cat > "$TP_DIR/summary.py" <<'PY'
import json, sys
# summary.py <req.jsonl>: probe bodies (sorted), then the main request's reasoning knobs; truncates the log
rs = [json.loads(l) for l in open(sys.argv[1])]
open(sys.argv[1], "w").close()
probes, main = [], []
for b in rs:
    m = b.get("messages") or []
    t = m[-1].get("content") if m else ""
    extra = {k: v for k, v in b.items() if k not in ("messages", "tools", "model", "stream", "stream_options", "store")}
    if t == "What is 17 times 23? Answer with only the number.\n" and len(m) == 1:
        probes.append(json.dumps(extra, sort_keys=True))
    elif b.get("tools"):
        main.append(json.dumps({k: v for k, v in extra.items() if k != "max_completion_tokens"}, sort_keys=True))
print("probes=%d" % len(probes), *sorted(probes), sep="\n")
print("main=" + ",".join(main))
PY
cat > "$TP_DIR/cache.py" <<'PY'
import json, sys
# cache.py <agent dir>: the cached rows without their timestamps
try: rows = json.load(open(sys.argv[1] + "/thinking-capability.json"))["rows"]
except OSError: rows = []
for r in rows:
    c = json.loads(r["capability"]); c.pop("checkedAt")
    print(r["provider_id"], r["api"], r["endpoint"].rsplit(":", 1)[0], r["wire_model_id"], json.dumps(c, separators=(",", ":")))
print("rows=%d" % len(rows))
PY
tp_case() { # name [config.yml line] [mock env...]: a throwaway agent dir and mock; runs omc twice, summaries in $TP_DIR/<name>.<n>
    local name=$1 cfg=$2; shift 2
    local d="$TP_DIR/$name" port=$((PORT + 300 + RANDOM % 200))
    mkdir -p "$d/agent"
    sed -e "s#127.0.0.1:$PORT/v1#127.0.0.1:$port/v1#" "$T/agent/models.yml" > "$d/agent/models.yml"
    printf 'modelRoles:\n  default: mock/mock-1\n%s\n' "$cfg" > "$d/agent/config.yml"
    env "$@" MOCK_LOG="$d/req.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $port >/dev/null 2>&1 & local mp=$!
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
    for n in 1 2; do
        OMC_CODING_AGENT_DIR="$d/agent" timeout 60 "$B" -p "say:probe run $n" </dev/null >"$d/out.$n" 2>&1
        python3 -I "$TP_DIR/summary.py" "$d/req.jsonl" > "$TP_DIR/$name.$n"
    done
    kill $mp 2>/dev/null; wait $mp 2>/dev/null
    python3 -I "$TP_DIR/cache.py" "$d/agent" > "$TP_DIR/$name.cache"
}
EFF6='{"max_completion_tokens": 1024, "reasoning_effort": "high"}
{"max_completion_tokens": 1024, "reasoning_effort": "low"}
{"max_completion_tokens": 1024, "reasoning_effort": "max"}
{"max_completion_tokens": 1024, "reasoning_effort": "medium"}
{"max_completion_tokens": 1024, "reasoning_effort": "minimal"}
{"max_completion_tokens": 1024, "reasoning_effort": "xhigh"}
{"max_completion_tokens": 1024}'
tp_case plain ""
check "probe batch"    $'probes=7\n'"$EFF6"$'\nmain={"reasoning_effort": "high"}' cat "$TP_DIR/plain.1"
check "probe answer"   "probe run 1" cat "$TP_DIR/plain/out.1"
check "probe cache"    'mock openai-completions http://127.0.0.1 mock-1 {"format":"openai","efforts":["minimal","low","medium","high","xhigh","max"],"observed":false,"attempts":[{"format":"openai","effort":"high","accepted":true,"observed":false},{"format":"openai","effort":"minimal","accepted":true,"observed":false},{"format":"openai","effort":"low","accepted":true,"observed":false},{"format":"openai","effort":"medium","accepted":true,"observed":false},{"format":"openai","effort":"xhigh","accepted":true,"observed":false},{"format":"openai","effort":"max","accepted":true,"observed":false},{"format":"none","accepted":true,"observed":false}]}
rows=1' cat "$TP_DIR/plain.cache"
check "probe reuse"    $'probes=0\nmain={"reasoning_effort": "high"}' cat "$TP_DIR/plain.2"
# a server that rejects every reasoning control: the alternate formats, then the `none` verdict, no effort on the wire
tp_case reject "" MOCK_REJECT_REASONING=1
check "probe formats"  'probes=22
{"chat_template_kwargs": {"enable_thinking": true}, "max_completion_tokens": 1024}
{"chat_template_kwargs": {"reasoning_effort": "high", "thinking": true}, "max_completion_tokens": 1024}' cat "$TP_DIR/reject.1"
check "probe formats2" '{"enable_thinking": true, "max_completion_tokens": 1024}
{"max_completion_tokens": 1024, "reasoning": {"effort": "high"}}' cat "$TP_DIR/reject.1"
check "probe formats3" $'{"max_completion_tokens": 1024, "thinking": {"type": "enabled"}}\n{"max_completion_tokens": 1024}\nmain={}' cat "$TP_DIR/reject.1"
check "probe none"     'mock-1 {"format":"none","efforts":[],"observed":false,"attempts":[{"format":"openai","effort":"high","accepted":false' cat "$TP_DIR/reject.cache"
check "probe none reuse" $'probes=0\nmain={}' cat "$TP_DIR/reject.2"
# reasoning seen in a probe's answer
tp_case think "" MOCK_PROBE_THINK=1
check "probe observed" '"efforts":["minimal","low","medium","high","xhigh","max"],"observed":true,"attempts":[{"format":"openai","effort":"high","accepted":true,"observed":true}' cat "$TP_DIR/think.cache"
# some tiers rejected: the requested level is probed first and the request clamps onto the learned ladder
tp_case partial "defaultThinkingLevel: max" MOCK_REJECT_REASONING=max,minimal
check "probe clamp"    'main={"reasoning_effort": "xhigh"}' cat "$TP_DIR/partial.1"
check "probe ladder"   '{"format":"openai","efforts":["low","medium","high","xhigh"],"observed":false,"attempts":[{"format":"openai","effort":"max","accepted":false' cat "$TP_DIR/partial.cache"
check "probe ladder reuse" $'probes=0\nmain={"reasoning_effort": "xhigh"}' cat "$TP_DIR/partial.2"
# transient failures are inconclusive: nothing cached, the requested level still sent, probed again next time
tp_case fail "" MOCK_PROBE_STATUS=503
check "probe inconclusive" $'probes=7\n'"$EFF6"$'\nmain={"reasoning_effort": "high"}' cat "$TP_DIR/fail.1"
check "probe not cached" $'rows=0' cat "$TP_DIR/fail.cache"
check "probe retried"  'probes=7' cat "$TP_DIR/fail.2"
# thinking off: no probe, nothing on the wire
tp_case off "defaultThinkingLevel: off"
check "probe off"      $'probes=0\nmain={}' cat "$TP_DIR/off.1"
