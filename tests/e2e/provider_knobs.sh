# Sourced by tests/e2e/run.sh: WP-14 provider knobs against mock_providers.py (catalog models with their baseUrl
# pointed at the mock through models.yml) and mock_openai.py. Expected values were checked against omp v18.14.2
# with tests/parity/tier_diff.sh, tests/parity/cachewarm_diff.sh and tests/e2e/tiers_rpc.py.
KPORT=$((PORT + 11))
MOCK_CACHE_TOKENS=200000 MOCK_LOG="$T/knobs.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $KPORT & KPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$KPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/kagent" "$T/kroot"
cat > "$T/kagent/models.yml" <<YML
providers:
  openai:
    baseUrl: http://127.0.0.1:$KPORT/v1
  anthropic:
    baseUrl: http://127.0.0.1:$KPORT
    modelOverrides:
      claude-opus-4-6:
        promptCache:
          short: 12
YML
KENV=(env OMC_CODING_AGENT_DIR="$T/kagent" OMC_CONFIG_DIR="$T/kroot" OPENAI_API_KEY=sk-test ANTHROPIC_API_KEY=sk-ant-api-test)
KL=(python3 -I "$ROOT/tests/e2e/reqlog.py" "$T/knobs.jsonl")
kcfg() { printf '%b' "$1" > "$T/kagent/config.yml"; }
# service tiers on the wire (tier.* settings, --service-tier) and the persisted service_tier_change
kcfg 'tier:\n  openai: priority\n  google: flex\n'
check "tier openai"        "plain provider answer"  "${KENV[@]}" "$B" --model openai/gpt-5.2 -p hi
check "tier openai wire"   "priority"  "${KL[@]}" /v1/responses 'b.get("service_tier")' '"tools"'
# the latest entry of a type in the agent's sessions, as JSON of the named fields
SESS=(python3 -I -c 'import glob, json, sys
es = [json.loads(l) for f in sorted(glob.glob(sys.argv[1] + "/sessions/*/*.jsonl")) for l in open(f)]
es = [e for e in es if e.get("type") == sys.argv[2]]
print(json.dumps({k: es[-1].get(k) for k in sys.argv[3:]}) if es else "none")' "$T/kagent")
check "tier entry"         '{"serviceTier": {"openai": "priority", "google": "flex"}}'  "${SESS[@]}" service_tier_change serviceTier
check "service-tier flag"  "plain provider answer"  "${KENV[@]}" "$B" --model openai/gpt-5.2 --service-tier flex --no-session -p hi
check "service-tier wire"  "flex"  "${KL[@]}" /v1/responses 'b.get("service_tier")' '"tools"'
check "service-tier bad"   'Error: Invalid --service-tier value: "fastest". Expected one of: none, auto, default, flex, scale, priority, ultrafast.'  "${KENV[@]}" "$B" --service-tier fastest -p hi
kcfg 'tier:\n  anthropic: priority\n'
check "tier anthropic"     "plain provider answer"  "${KENV[@]}" "$B" --model anthropic/claude-opus-4-6 --no-session -p hi
check "tier anthropic wire" "fast True"  "${KL[@]}" /v1/messages 'b.get("speed"), "fast-mode-2026-02-01" in h.get("anthropic-beta", "")' '"tools"'
# /fast /slow /extended-context and set_fast_mode / set_slow_mode / set_cache_warming over RPC (omp's output)
kcfg 'modelRoles:\n  default: openai/gpt-5.2\n'
check "tiers rpc"          "rpc ok"  bash -c "${KENV[*]} OMC_NO_TITLE=1 python3 -I '$ROOT/tests/e2e/tiers_rpc.py' '$B' > '$T/tiers.out' 2>&1; diff '$ROOT/tests/e2e/tiers_rpc.expected' '$T/tiers.out' && echo rpc ok"
# extended context: gpt-5.6 runs at its 272K standard-pricing window unless extendedContext is on
check "standard window"    "│ gpt-5.6                  │    272K │"  "${KENV[@]}" "$B" models openai
kcfg 'extendedContext: true\n'
check "extended window"    "│ gpt-5.6                  │    1.1M │"  "${KENV[@]}" "$B" models openai
# prompt-cache warming: one RPC turn on a 12 s cache lifetime, then idle refreshes (one-token replays) that hit
kcfg 'modelRoles:\n  default: anthropic/claude-opus-4-6\n'
rm -rf "$T/kagent/sessions"
"${KENV[@]}" OMC_NO_TITLE=1 timeout 60 python3 -I "$ROOT/tests/parity/rpc_drive.py" "$B" "say hello" 5 >/dev/null 2>&1
check "cache warm replays" "replays ok"  python3 -I -c '
import json, sys
rs = [json.loads(l) for l in open(sys.argv[1]) if "opus-4-6" in l]
rs = [r["body"] for r in rs if r["body"].get("tools")]
key = lambda b: json.dumps({k: v for k, v in b.items() if k != "max_tokens"}, sort_keys=True)
seen, warm = {}, []
for b in rs:  # a replay is an earlier request again under a smaller output budget
    if key(b) in seen and b["max_tokens"] < seen[key(b)]: warm.append(b)
    else: seen.setdefault(key(b), b["max_tokens"])
print("replays ok" if warm else f"requests={len(rs)} warm=0")' "$T/knobs.jsonl"
check "cache warm usage"   '{"purpose": "cache-warm", "provider": "anthropic", "model": "claude-opus-4-6"}'  "${SESS[@]}" model_usage purpose provider model
kill $KPID 2>/dev/null
# stream watchdog (providers.streamFirstEventTimeoutSeconds) and providers.maxInFlightRequests slots
printf 'providers:\n  streamFirstEventTimeoutSeconds: 1\n  maxInFlightRequests:\n    mock: 1\nretry:\n  enabled: false\n' > "$T/agent/config.yml"
check "first-event timeout" "OpenAI completions stream timed out while waiting for the first event"  env OMC_CONFIG_DIR="$T/kroot" "$B" --model mock/mock-1 --no-session -p "slow:3:late"
check "in-flight slot"     "slot-0"  bash -c "ls '$T'/kroot/run/provider-inflight/*/"
rm -f "$T/agent/config.yml"
