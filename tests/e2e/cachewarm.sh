# Sourced by tests/e2e/run.sh: prompt-cache warming refresh methods (omp 0501d518bd) against mock_providers.py. The
# bundled anthropic model keeps its first-party base URL (OMC_PROVIDER_URL_REWRITE points the wire at the mock), so
# refreshes are max_tokens 0 pre-warms; a pre-warm the endpoint refuses (400) is replayed in the same slot, and the
# model is replayed for the rest of the session.
CWPORT=$((PORT + 33))
mkdir -p "$T/cwagent"
printf 'providers:\n  anthropic:\n    modelOverrides:\n      claude-opus-4-6:\n        promptCache:\n          short: 12\n' > "$T/cwagent/models.yml"
printf 'modelRoles:\n  default: anthropic/claude-opus-4-6\n' > "$T/cwagent/config.yml"
CWENV=(env OMC_CODING_AGENT_DIR="$T/cwagent" ANTHROPIC_API_KEY=sk-ant-api-test OMC_NO_TITLE=1
       OMC_PROVIDER_URL_REWRITE="https://api.anthropic.com=http://127.0.0.1:$CWPORT")
cw_run() { # tag [env...]: one RPC turn, then 5 idle seconds of refreshes (a 12 s lifetime refreshes every 2 s)
    local tag=$1; shift
    env "$@" MOCK_CACHE_TOKENS=200000 MOCK_LOG="$T/cw-$tag.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $CWPORT & local mp=$!
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$CWPORT) 2>/dev/null && break; sleep 0.1; done
    "${CWENV[@]}" timeout 60 python3 -I "$ROOT/tests/parity/rpc_drive.py" "$B" "say hello" 5 > "$T/cw-$tag.out" 2>/dev/null </dev/null
    kill $mp 2>/dev/null; wait $mp 2>/dev/null
}
cat > "$T/cw_check.py" <<'PY'
import json, sys
reqs = [json.loads(l)["body"] for l in open(sys.argv[1]) if "/messages" in l]
reqs = [b for b in reqs if b.get("tools")]
evs = [json.loads(l) for l in open(sys.argv[2]) if l.strip()]
starts = [e.get("method") for e in evs if e["type"] == "cache_warming_start"]
ends = [(e.get("method"), e.get("outcome")) for e in evs if e["type"] == "cache_warming_end"]
shape = sorted({(b.get("max_tokens"), b.get("stream")) for b in reqs[1:]})
print("starts=%s ends=%s warm=%s" % (",".join(starts[:3]), ",".join("%s:%s" % x for x in ends[:3]), shape))
PY
cw_run pre
check "cache warm prewarm" "starts=prewarm,prewarm ends=prewarm:hit,prewarm:hit warm=[(0, False)]"  python3 -I "$T/cw_check.py" "$T/cw-pre.jsonl" "$T/cw-pre.out"
cw_run refused MOCK_PREWARM_400=1
check "cache warm prewarm refused" "starts=prewarm,replay,replay ends=prewarm:error,replay:hit,replay:hit"  python3 -I "$T/cw_check.py" "$T/cw-refused.jsonl" "$T/cw-refused.out"
# a long tool call on a large prompt bridged with compaction (providers.cacheWarmingStrategy: compact): the handoff
# document summarizes the history before the pending call, goes out a minute before the entry expires (a 70 s lifetime:
# 10 s into the call), sends no refresh, and commits when the tool returns
printf 'providers:\n  anthropic:\n    modelOverrides:\n      claude-opus-4-6:\n        promptCache:\n          short: 70\n' > "$T/cwagent/models.yml"
printf 'modelRoles:\n  default: anthropic/claude-opus-4-6\ndateCwdReminder: false\nproviders:\n  cacheWarmingStrategy: compact\ncompaction:\n  methodOrder: [handoff]\n  keepRecentTokens: 10\n' > "$T/cwagent/config.yml"
CWBIG="say:$(printf 'filler words for the transcript %.0s' $(seq 2000))"
env MOCK_CACHE_TOKENS=200000 MOCK_LOG="$T/cw-bridge.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $CWPORT & CWPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$CWPORT) 2>/dev/null && break; sleep 0.1; done
"${CWENV[@]}" timeout 90 python3 -I "$ROOT/tests/e2e/rpc_seq.py" "$B" "$CWBIG" "bash:sleep 14; echo bridged" > "$T/cw-bridge.out" 2>/dev/null </dev/null
kill $CWPID 2>/dev/null; wait $CWPID 2>/dev/null
check "cache warm bridge" "refreshes=0 handoff_tail=user compaction=handoff next_has_doc=True"  python3 -I -c '
import glob, json, sys
reqs = [json.loads(l)["body"] for l in open(sys.argv[1]) if "/messages" in l]
ho = [b for b in reqs if "Write a handoff document" in json.dumps(b["messages"][-1:])]
tail = ho[0]["messages"][-2]["role"] if ho and len(ho[0]["messages"]) > 1 else None
ents = [json.loads(l) for f in glob.glob(sys.argv[2] + "/sessions/*/*.jsonl") for l in open(f) if l.strip()]
cmp = [e.get("method") for e in ents if e.get("type") == "compaction"]
turns = [b for b in reqs if b not in ho]
print("refreshes=%d handoff_tail=%s compaction=%s next_has_doc=%s" % (sum(b.get("max_tokens") == 0 or b.get("stream") is False for b in reqs), tail,
      ",".join(map(str, cmp)), "## Goal" in json.dumps(turns[-1]["messages"]) if turns else None))' "$T/cw-bridge.jsonl" "$T/cwagent"
