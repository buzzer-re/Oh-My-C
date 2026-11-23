# kiro-api (native Kiro provider, omp v19.1.0) against tests/e2e/mock_kiro.py; sourced from run.sh (uses $B $T $PORT
# $ROOT, check() and fail). Fake credentials only: every Kiro host (runtime, management, AWS SSO-OIDC, the social auth
# service) is rewritten to the mock. Checked against the upstream sources and tests (kiro-*.test.ts), not a live omp.
KPORT=$((PORT + 57))
MOCK_LOG="$T/kiro.jsonl" python3 -I "$ROOT/tests/e2e/mock_kiro.py" $KPORT & KPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$KPORT) 2>/dev/null && break; sleep 0.1; done
KB="http://127.0.0.1:$KPORT"
KRW="https://runtime.us-east-1.kiro.dev=$KB/rt-us-east-1,https://runtime.eu-central-1.kiro.dev=$KB/rt-eu-central-1"
KRW="$KRW,https://management.us-east-1.kiro.dev=$KB/mgmt-us-east-1,https://management.eu-central-1.kiro.dev=$KB/mgmt-eu-central-1"
KRW="$KRW,https://oidc.us-east-1.amazonaws.com=$KB/oidc-us-east-1,https://prod.us-east-1.auth.desktop.kiro.dev=$KB/social"
mkdir -p "$T/kagent" "$T/kwork" "$T/kapi" "$T/klogin"
# session retries off: a classifier refusal is retried by the agent like omp's (turn-recovery), which only slows this down
printf 'retry:\n  enabled: false\n' > "$T/kagent/config.yml"
KENV=(env -u KIRO_API_KEY -u KIRO_API_REGION HOME="$T/kwork" OMC_CODING_AGENT_DIR="$T/kagent" OMC_PROVIDER_URL_REWRITE="$KRW"
      OMC_AUTH_URL_REWRITE="$KRW" OMC_NO_BROWSER=1 OMC_AUTH_POLL_SCALE_MS=20)
KEXP=$(( ($(date +%s) + 86400) * 1000 ))
cat > "$T/kcreds.json" <<JSON
{"version": 1, "credentials": [
 {"provider": "kiro", "credential": {"type": "oauth", "access": "kiro-access", "refresh": "kiro-refresh", "expires": $KEXP,
  "accountId": "login-e2e", "region": "us-east-1", "kiroProfileArn": "arn:aws:codewhisperer:us-east-1:123456789012:profile/TEAMA",
  "kiroClientId": "cid-1", "kiroClientSecret": "csec-1", "kiroOidcRegion": "us-east-1"}}
]}
JSON
"${KENV[@]}" "$B" auth import "$T/kcreds.json" >/dev/null 2>&1
# discovery: the roster is listed per credential (paged, deduplicated) and cached; never bundled
check "kiro models"       "claude-x"                   "${KENV[@]}" "$B" models list kiro
check "kiro models auto"  "auto"                       "${KENV[@]}" "$B" models list kiro
check "kiro model cache"  "kiro:models-v2:"            cat "$T/kagent/kiro-models.json"
check "kiro text"         "plain kiro answer"          "${KENV[@]}" "$B" --model kiro/auto --no-session -p "hi"
check "kiro tool"         "Tool said: kiro-e2e"        "${KENV[@]}" "$B" --model kiro/claude-x:off --no-session -p "bash:echo kiro-e2e"
check "kiro thinking"     '"thinking":"pondering","thinkingSignature":"sig-1"' "${KENV[@]}" "$B" --model kiro/claude-x:high --no-session --mode json -p "think"
check "kiro usage tokens" '"input":120,"output":7,"cacheRead":30,"cacheWrite":5' "${KENV[@]}" "$B" --model kiro/auto --no-session --mode json -p "hi"
check "kiro context"      '"contextTokens":3000'       "${KENV[@]}" "$B" --model kiro/auto --no-session --mode json -p "hi"
check "kiro mandatory"    "plain kiro answer"          "${KENV[@]}" "$B" --model kiro/gpt-k:off --no-session -p "hi"
check "kiro capacity"     "kiro answer after capacity retry" "${KENV[@]}" "$B" --model kiro/auto --no-session -p "capacity"
check "kiro refusal"      "Refusal (cyber): not allowed" "${KENV[@]}" "$B" --model kiro/auto --no-session -p "filtered"
check "kiro too long"     "Kiro HTTP 400 ValidationException: Input is too long." "${KENV[@]}" "$B" --model kiro/auto --no-session --mode json -p "toolong"
check "kiro usage"        "Kiro Pro credits"           "${KENV[@]}" "$B" usage -p kiro
check "kiro usage used"   "10.0% used"                 "${KENV[@]}" "$B" usage -p kiro
# API key: ksk_ keys carry tokentype; an unpinned key probes the bootstrap regions and runs where one accepts it
check "kiro api key"      "plain kiro answer"          env -u KIRO_API_REGION HOME="$T/kwork" OMC_CODING_AGENT_DIR="$T/kapi" KIRO_API_KEY=ksk_good \
    OMC_PROVIDER_URL_REWRITE="$KRW" OMC_AUTH_URL_REWRITE="$KRW" "$B" --model kiro/auto --no-session -p "hi"
check "kiro api key region" "Kiro API keys must use the ksk_… format" env -u KIRO_API_REGION HOME="$T/kwork" OMC_CODING_AGENT_DIR="$T/kapi" \
    OMC_PROVIDER_URL_REWRITE="$KRW" OMC_AUTH_URL_REWRITE="$KRW" bash -c "printf '3\nnot-a-key\n' | '$B' login kiro"
check "kiro login api key" "Logged in to Kiro"         bash -c "printf '3\nksk_good\n' | ${KENV[*]} OMC_CODING_AGENT_DIR='$T/klogin' '$B' login kiro"
check "kiro login api stored" '"key": "{\"kind\":\"api_key\",\"token\":\"ksk_good\",\"region\":\"us-east-1\"}"' cat "$T/klogin/auth.json"
# IAM Identity Center: device grant, then the profile picker (two profiles in the commercial partition)
mkdir -p "$T/kidc" "$T/ksoc"
check "kiro login idc"    "Confirm code KIRO-1234 in the browser" \
    bash -c "printf '1\nhttps://company.awsapps.com/start\nus-east-1\n2\n' | ${KENV[*]} OMC_CODING_AGENT_DIR='$T/kidc' '$B' login kiro"
check "kiro login idc saved" '"kiroProfileArn": "arn:aws:codewhisperer:us-east-1:123456789012:profile/TEAMB"' cat "$T/kidc/auth.json"
check "kiro login idc org" '"orgName": "Team B"'      cat "$T/kidc/auth.json"
check "kiro login bad url" "Invalid AWS access portal URL" \
    bash -c "printf '1\nhttp://company.awsapps.com/start\n' | ${KENV[*]} OMC_CODING_AGENT_DIR='$T/kidc' '$B' login kiro"
check "kiro login social" "Confirm code SOC-42 in the browser and sign in with GitHub" \
    bash -c "printf '5\n' | ${KENV[*]} OMC_CODING_AGENT_DIR='$T/ksoc' '$B' login kiro"
check "kiro login social saved" '"orgName": "GitHub"'  cat "$T/ksoc/auth.json"
check "kiro login bad method" "Invalid Kiro authentication selection" \
    bash -c "printf '9\n' | ${KENV[*]} OMC_CODING_AGENT_DIR='$T/ksoc' '$B' login kiro"
# refresh: an expired IAM Identity Center row refreshes against oidc.<kiroOidcRegion>; a social row (no OIDC client)
# against the Kiro auth service, which may move it to another profile
mkdir -p "$T/kref" "$T/kref2"
sed -e "s/\"expires\": $KEXP/\"expires\": 1000/" "$T/kcreds.json" > "$T/kcreds-old.json"
"${KENV[@]}" OMC_CODING_AGENT_DIR="$T/kref" "$B" auth import "$T/kcreds-old.json" >/dev/null 2>&1
check "kiro refresh oidc"  "plain kiro answer"         "${KENV[@]}" OMC_CODING_AGENT_DIR="$T/kref" "$B" --model kiro/auto --no-session -p "hi"
check "kiro refresh saved" '"access": "kiro-access-2"' cat "$T/kref/auth.json"
sed -e 's/"kiroClientId": "cid-1", //' "$T/kcreds-old.json" > "$T/kcreds-soc.json"
"${KENV[@]}" OMC_CODING_AGENT_DIR="$T/kref2" "$B" auth import "$T/kcreds-soc.json" >/dev/null 2>&1
check "kiro refresh social" "plain kiro answer"        "${KENV[@]}" OMC_CODING_AGENT_DIR="$T/kref2" "$B" --model kiro/auto --no-session -p "hi"
check "kiro refresh moved" 'profile/TEAMA"'            cat "$T/kref2/auth.json"
# wire shape: Kiro CLI identity headers, the profile ARN, history replay, tool specs, usage request body
python3 -I - "$T/kiro.jsonl" <<'PY' && echo "   ok  kiro request shape" || { echo "   FAIL kiro request shape"; fail=1; }
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
gen = [r for r in rows if r["path"].startswith("/rt-")]
h = gen[0]["headers"]
assert h["x-amz-target"] == "AmazonCodeWhispererStreamingService.GenerateAssistantResponse", h
assert h["content-type"] == "application/x-amz-json-1.0" and h["x-amzn-codewhisperer-optout"] == "true", h
assert h["x-amzn-kiro-attempt"] == "1;max=2" and h["x-amzn-kiro-fallback"] == "0;max=0", h
assert h["authorization"] == "Bearer kiro-access" and "tokentype" not in h, h
assert h["user-agent"].endswith("md/appVersion-3.2.0 app/AmazonQ-For-CLI"), h
b = gen[0]["body"]
assert b["agentMode"] == "vibe" and b["profileArn"].endswith("profile/TEAMA"), b
cs = b["conversationState"]
assert cs["chatTriggerType"] == "MANUAL" and cs["agentTaskType"] == "vibe" and cs["conversationId"], cs
cur = cs["currentMessage"]["userInputMessage"]
assert cur["origin"] == "KIRO_CLI" and cur["modelId"] == "auto", cur
assert any(t["toolSpecification"]["name"] == "bash" for t in cur["userInputMessageContext"]["tools"]), cur
assert "history" not in cs and cur["content"].endswith("hi") and len(cur["content"]) > 100, "system prompt rides in the first turn"
retry = [r for r in gen if r["headers"]["x-amzn-kiro-attempt"] == "2;max=2"]
assert len(retry) == 1, "one capacity replay"
tool = [r["body"] for r in gen if r["body"]["conversationState"].get("history")]
hist = tool[0]["conversationState"]["history"]
uses = hist[-1]["assistantResponseMessage"]["toolUses"]
assert uses[0] == {"name": "bash", "toolUseId": "tooluse_k1", "input": {"command": "echo kiro-e2e"}}, uses
res = tool[0]["conversationState"]["currentMessage"]["userInputMessage"]["userInputMessageContext"]["toolResults"]
assert res[0]["toolUseId"] == "tooluse_k1" and res[0]["status"] == "success" and "kiro-e2e" in res[0]["content"][0]["text"], res
think = [r["body"] for r in gen if r["body"]["conversationState"]["currentMessage"]["userInputMessage"]["content"].endswith("think")]
assert think[0]["additionalModelRequestFields"]["thinking"] == {"type": "adaptive", "display": "summarized"}, think[0]
assert think[0]["additionalModelRequestFields"]["output_config"] == {"effort": "high"}, think[0]
off = [r["body"] for r in gen if r["body"]["conversationState"]["currentMessage"]["userInputMessage"]["modelId"] == "claude-x"
       and not r["body"]["conversationState"]["currentMessage"]["userInputMessage"]["content"].endswith("think")]
assert off[0]["additionalModelRequestFields"] == {"thinking": {"type": "disabled"}}, off[0]
filtered = [r for r in gen if r["body"]["conversationState"]["currentMessage"]["userInputMessage"]["content"].endswith("filtered")]
assert len(filtered) == 1, "no provider-level replay of a refusal"
refreshed = [r for r in rows if r["path"] == "/oidc-us-east-1/token" and r["body"].get("grantType") == "refresh_token"]
assert refreshed and refreshed[0]["body"] == {"grantType": "refresh_token", "refreshToken": "kiro-refresh", "clientId": "cid-1",
                                              "clientSecret": "csec-1"}, refreshed
social = [r for r in rows if r["path"] == "/social/refreshToken"]
assert social and social[0]["body"] == {"refreshToken": "kiro-refresh"}, social
assert any(r["headers"]["authorization"] == "Bearer kiro-access-2" for r in gen), "requests use the refreshed token"
gptk = [r["body"] for r in gen if r["body"]["conversationState"]["currentMessage"]["userInputMessage"]["modelId"] == "gpt-k"]
assert gptk[0]["additionalModelRequestFields"] == {"reasoning": {"mode": "standard", "effort": "low"}}, gptk[0]
keyed = [r for r in gen if r["headers"]["authorization"] == "Bearer ksk_good"]
assert keyed and keyed[0]["headers"]["tokentype"] == "API_KEY" and "profileArn" not in keyed[0]["body"], keyed
models = [r for r in rows if r["path"].startswith("/mgmt-") and r["headers"]["x-amz-target"].endswith("ListAvailableModels")]
oauth = [r for r in models if r["headers"]["authorization"] == "Bearer kiro-access"]
assert oauth[0]["body"] == {"origin": "KIRO_CLI", "profileArn": "arn:aws:codewhisperer:us-east-1:123456789012:profile/TEAMA"}, oauth[0]
assert any(r["body"].get("nextToken") == "p2" for r in oauth), "paged"
probes = {r["path"] for r in models if r["headers"]["authorization"] == "Bearer ksk_good" and "nextToken" not in r["body"]}
assert probes >= {"/mgmt-us-east-1/", "/mgmt-eu-central-1/"}, probes
usage = [r for r in rows if r["headers"].get("x-amz-target", "").endswith("GetUsageLimits")]
assert usage[0]["body"] == {"origin": "KIRO_CLI", "profileArn": "arn:aws:codewhisperer:us-east-1:123456789012:profile/TEAMA",
                            "resourceType": "AGENTIC_REQUEST"}, usage[0]
reg = [r for r in rows if r["path"] == "/oidc-us-east-1/client/register"]
assert reg[0]["body"] == {"clientName": "Kiro CLI", "clientType": "public", "scopes": ["codewhisperer:completions",
                          "codewhisperer:analysis", "codewhisperer:conversations"]}, reg[0]
assert reg[0]["headers"]["user-agent"].endswith("m/E,N app/AmazonQ-For-CLI") and reg[0]["headers"]["amz-sdk-request"] == "attempt=1; max=3"
grant = [r for r in rows if r["path"] == "/oidc-us-east-1/token"]
assert grant[0]["body"]["grantType"] == "urn:ietf:params:oauth:grant-type:device_code" and grant[0]["body"]["deviceCode"] == "dev-1"
soc = [r for r in rows if r["path"] == "/social/oauth/device/authorization"]
assert soc[0]["body"] == {"clientId": "Kiro-CLI", "loginProvider": "Github"} and soc[0]["headers"]["user-agent"] == "Kiro-CLI", soc[0]
PY
kill $KPID 2>/dev/null
