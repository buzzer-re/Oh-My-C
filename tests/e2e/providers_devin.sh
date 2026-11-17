# devin-agent (Cascade over Connect/protobuf) against tests/e2e/mock_devin.py; sourced from run.sh
# (uses $B $T $PORT $ROOT, check() and fail). Fake credentials only.
DPORT=$((PORT + 11))
MOCK_LOG="$T/devin.jsonl" MOCK_DEVIN_KEY=devin-test-key python3 -I "$ROOT/tests/e2e/mock_devin.py" $DPORT & DVPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$DPORT) 2>/dev/null && break; sleep 0.1; done
DVENV=(env HOME="$T" DEVIN_API_KEY=devin-test-key OMC_PROVIDER_URL_REWRITE="https://server.codeium.com=http://127.0.0.1:$DPORT")
mkdir -p "$T/devin"
check "devin text"      "plain devin answer"   "${DVENV[@]}" "$B" --model devin/swe-1-6 --no-session -p "hi"
check "devin tool"      "Tool said: dv-e2e"    "${DVENV[@]}" "$B" --model devin/swe-1-6 --session-dir "$T/devin" -p "bash:echo dv-e2e"
check "devin end error" "Devin stream error resource_exhausted: quota exceeded [details: exa.QuotaInfo: {\"reason\":\"mock\"}]" \
    "${DVENV[@]}" "$B" --model devin/swe-1-6 --no-session -p "fail"
check "devin auth 401"  "Devin auth error 401 Unauthorized: invalid api key" \
    env HOME="$T" DEVIN_API_KEY=wrong OMC_PROVIDER_URL_REWRITE="https://server.codeium.com=http://127.0.0.1:$DPORT" \
    "$B" --model devin/swe-1-6 --no-session -p "hi"
check "devin json"      '"toolName":"bash"'    "${DVENV[@]}" "$B" --model devin/swe-1-6 --no-session --mode json -p "bash:echo j"
# router model (compat.modelRouter): AssignModel first, then chat with the assigned uid + assignment JWT
mkdir -p "$T/devin-agent"
printf 'providers:\n  devin:\n    baseUrl: https://server.codeium.com\n    api: devin-agent\n    models:\n      - id: adaptive\n        compat:\n          modelRouter: true\n' > "$T/devin-agent/models.yml"
check "devin router"    '"upstreamModel":"MODEL_ACTUAL"'  "${DVENV[@]}" OMC_CODING_AGENT_DIR="$T/devin-agent" "$B" --model devin/adaptive --no-session --mode json -p "hi"
# request shape: identity metadata, user JWT, Cascade request type/planner mode, tool choice, configuration
python3 -I - "$T/devin.jsonl" <<'PY' && echo "   ok  devin request shape" || { echo "   FAIL devin request shape"; fail=1; }
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
auth = [r for r in rows if r["path"].endswith("/GetUserJwt")]
chat = [r for r in rows if r["path"].endswith("/GetChatMessage")]
md = chat[0]["request"]["metadata"]
assert auth and auth[0]["metadata"]["apiKey"] == "devin-test-key" and auth[0]["metadata"]["userJwt"] == "", auth[0]
assert md["ideName"] == "devin-cli" and md["ideType"] == "chisel" and md["ideVersion"] == "3000.12.57", md
assert md["extensionName"] == "chisel" and md["locale"] == "en" and md["userJwt"] == "jwt-devin-1", md
r = chat[0]["request"]
assert r["chatModelUid"] == "swe-1-6" and r["requestType"] == 5 and r["plannerMode"] == 1 and r["toolChoice"] == "auto", r
assert r["systemPromptCache"] == 1 and not r["disableParallelToolCalls"] and r["config"]["numCompletions"] == 1, r
assert r["config"]["topK"] == 40 and r["config"]["maxNewlines"] == 400 and r["config"]["temperature"] == 1.0, r["config"]
assert any(t["name"] == "bash" for t in r["tools"]) and r["systemPrompt"], r["tools"]
h = chat[0]["headers"]
assert h["content-type"] == "application/connect+proto" and h["user-agent"] == "connect-go/1.18.1 (go1.26.3)", h
assert h["connect-protocol-version"] == "1" and h["connect-accept-encoding"] == "gzip" and h["accept-encoding"] == "identity", h
assert all(c["compressed"] == ("connect-content-encoding" in c["headers"]) for c in chat), "gzip header mismatch"
assert any(c["compressed"] for c in chat), "large requests are gzipped"
# tool turn replay: assistant (SYSTEM) tool call id normalized, TOOL result paired, stable cascade id
replay = [c["request"] for c in chat if any(p["source"] == 4 for p in c["request"]["prompts"])]
assert replay, "tool result replay"
ps = replay[0]["prompts"]
calls = [t for p in ps if p["source"] == 2 for t in p["toolCalls"]]
tool = [p for p in ps if p["source"] == 4][0]
assert calls[0]["id"] == "call_1_item" and tool["toolCallId"] == "call_1_item", (calls, tool)
assert json.loads(calls[0]["argumentsJson"]) == {"command": "echo dv-e2e"}, calls
assert [p for p in ps if p["source"] == 2][0]["thinking"] == "Let me run it.", ps
assign = [r for r in rows if r["path"].endswith("/AssignModel")]
assert assign and assign[0]["router"] == "adaptive" and assign[0]["metadata"]["userJwt"] == "", assign
routed = [c["request"] for c in chat if c["request"]["modelAssignmentJwt"]]
assert routed and routed[0]["chatModelUid"] == "MODEL_ASSIGNED_1" and routed[0]["modelAssignmentJwt"] == "assign-jwt", routed
assert routed[0]["cascadeId"] == assign[0]["cascadeId"], "router and chat share the cascade id"
PY
kill $DVPID 2>/dev/null
# devin-cli adopts the Devin CLI seat key, devin-windsurf exchanges the Windsurf code; both file a `devin` API key
mkdir -p "$T/devinlogin"
check "devin logins"    "windsurf denied ok"  python3 -I "$ROOT/tests/e2e/login_devin.py" "$B" "$T/devinlogin"
