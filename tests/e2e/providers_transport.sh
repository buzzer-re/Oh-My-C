# Sourced by tests/e2e/run.sh: credentials stored by provider logins drive the chat request (URL, bearer, headers).
# Logins run over stdin against mock_auth.py (OMC_AUTH_URL_REWRITE); chat requests hit mock_providers.py through
# OMC_PROVIDER_URL_REWRITE, whose request log shows the URL/headers omc built. Fake credentials only.
XPORT=$((PORT + 8)); XAPORT=$((PORT + 9))
MOCK_LOG="$T/prov.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $XPORT & XPID=$!
python3 -I "$ROOT/tests/e2e/mock_auth.py" $XAPORT & XAPID=$!
for p in $XPORT $XAPORT; do for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && break; sleep 0.1; done; done
XB="http://127.0.0.1:$XPORT"; XA="http://127.0.0.1:$XAPORT"
mkdir -p "$T/pagent"
XENV=(env -u CLOUDFLARE_ACCOUNT_ID -u CLOUDFLARE_GATEWAY_ID -u CLOUDFLARE_AI_GATEWAY_API_KEY -u SNOWFLAKE_PAT -u SNOWFLAKE_ACCOUNT
      -u COPILOT_GITHUB_TOKEN -u COPILOT_INTEGRATION_ID -u XIAOMI_API_KEY -u KILO_API_KEY
      OMC_CODING_AGENT_DIR="$T/pagent" OMC_NO_BROWSER=1 OMC_AUTH_POLL_SCALE_MS=20 OMC_NO_TITLE=1
      OMC_AUTH_URL_REWRITE="https://github.com=$XA/gh,https://api.github.com=$XA/ghapi,https://api.individual.githubcopilot.com=$XA/capi,https://api.kilo.ai=$XA/kilo,https://auth.meta.com=$XA/meta,https://api.meta.ai=$XA/metaapi,https://qwen-custom.example=$XA/qwen"
      OMC_PROVIDER_URL_REWRITE="https://gateway.ai.cloudflare.com=$XB/cf,https://api.meta.ai=$XB/meta,https://acme-prod.snowflakecomputing.com=$XB/sf,https://qwen-custom.example=$XB/qwen,https://token-plan-sgp.xiaomimimo.com=$XB/xmtp,https://api.kilo.ai=$XB/kilo,https://api.individual.githubcopilot.com=$XB/capi,https://copilot-api.ghe.example.com=$XB/deny/ghe")
XL=(python3 -I "$ROOT/tests/e2e/reqlog.py" "$T/prov.jsonl")
# Cloudflare AI Gateway: {token, accountId, gatewayId} -> account/gateway URL, cf-aig-authorization, namespace routes
check "cf gateway login"  "Logged in to Cloudflare AI Gateway"  bash -c "printf 'cf-tok\nacct1\ngw1\n' | ${XENV[*]} '$B' login cloudflare-ai-gateway"
check "cf anthropic route" "plain provider answer"  "${XENV[@]}" "$B" --model cloudflare-ai-gateway/anthropic/claude-sonnet-4.5 --no-session -p hi
check "cf anthropic wire"  "/cf/v1/acct1/gw1/anthropic/v1/messages Bearer cf-tok None None claude-sonnet-4-5" \
    "${XL[@]}" /cf/ 'x["path"], h.get("cf-aig-authorization"), h.get("authorization"), h.get("x-api-key"), b.get("model")' claude-sonnet-4-5
check "cf openai route"   "plain provider answer"  "${XENV[@]}" "$B" --model cloudflare-ai-gateway/openai/gpt-5.1 --no-session -p hi
check "cf openai wire"    "/cf/v1/acct1/gw1/openai/chat/completions Bearer cf-tok None gpt-5.1" \
    "${XL[@]}" /cf/ 'x["path"], h.get("cf-aig-authorization"), h.get("authorization"), b.get("model")' gpt-5.1
check "cf workers route"  "plain provider answer"  "${XENV[@]}" "$B" --model "cloudflare-ai-gateway/workers-ai/@cf/google/gemma-4-26b-a4b-it" --no-session -p hi
check "cf workers wire"   "/cf/v1/acct1/gw1/compat/chat/completions workers-ai/@cf/google/gemma-4-26b-a4b-it" "${XL[@]}" /cf/ 'x["path"], b.get("model")' gemma
check "cf no gateway"     "Cloudflare AI Gateway ID is required"  "${XENV[@]}" CLOUDFLARE_ACCOUNT_ID=acct9 "$B" --model cloudflare-ai-gateway/openai/gpt-5.1 --api-key plain-tok --no-session -p hi
# Muse Code: device login mints the key bundle; requests send the minted key + x-api-version
check "muse login"        "Logged in to Muse Code (Subscription)"  bash -c "${XENV[*]} '$B' login muse-code </dev/null"
check "muse chat"         "plain provider answer"  "${XENV[@]}" "$B" --model muse-code/muse-spark-1.3 --no-session -p hi
check "muse wire"         "/meta/v1/responses Bearer muse-key 1.0.0"  "${XL[@]}" /meta/ 'x["path"], h.get("authorization"), h.get("x-api-version")'
# Muse Code subscriptions declare tools under the `_` prefix (403 client_attestation_denied otherwise); returned calls run
# under their own name and history replays the wire name
check "muse tool names"   "_bash True"  "${XL[@]}" /meta/ 'next(t["name"] for t in b["tools"] if t["name"].endswith("bash")), all(t["name"].startswith("_") for t in b["tools"])'
check "muse tool call"    "plain provider answer"  "${XENV[@]}" "$B" --model muse-code/muse-spark-1.3 --no-session -p "bash:echo muse-e2e"
check "muse call replay"  "_bash"  "${XL[@]}" /meta/ 'next(i["name"] for i in b["input"] if i.get("type") == "function_call")'
check "muse call result"  "muse-e2e"  "${XL[@]}" /meta/ 'next(i["output"] for i in b["input"] if i.get("type") == "function_call_output")'
# Snowflake: structured OAuth key -> account URL replaces the placeholder origin
FAR=$(( $(date +%s) * 1000 + 3600000 ))
python3 -I -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["snowflake"]={"type":"oauth","access":"sf-at","refresh":"sf-rt","expires":int(sys.argv[2]),"enterpriseUrl":"https://acme-prod.snowflakecomputing.com"}; json.dump(d,open(p,"w"))' "$T/pagent/auth.json" $FAR
check "snowflake chat"    "plain provider answer"  "${XENV[@]}" "$B" --model snowflake/claude-opus-4-6 --no-session -p hi
check "snowflake wire"    "/sf/api/v2/cortex/v1/messages Bearer sf-at"  "${XL[@]}" /sf/ 'x["path"], h.get("authorization")'
check "snowflake pat env" "Snowflake account is required"  "${XENV[@]}" SNOWFLAKE_PAT=pat-1 "$B" --model snowflake/claude-opus-4-6 --api-key " " --no-session -p hi
# QwenCloud Token Plan: custom region base URL rides in the JSON key
check "token plan login"  "Logged in to QwenCloud Token Plan"  bash -c "printf '3\nhttps://qwen-custom.example/compatible-mode/v1\nsk-tp-good\n\n' | ${XENV[*]} '$B' login alibaba-token-plan"
check "token plan chat"   "plain provider answer"  "${XENV[@]}" "$B" --model alibaba-token-plan/qwen3.7-max --no-session -p hi
check "token plan wire"   "/qwen/compatible-mode/v1/chat/completions Bearer sk-tp-good"  "${XL[@]}" /qwen/ 'x["path"], h.get("authorization")'
check "token plan bad"    "Invalid QwenCloud Token Plan credential"  "${XENV[@]}" "$B" --model alibaba-token-plan/qwen3.7-max --api-key not-a-key --no-session -p hi
# Xiaomi Token Plan keys use the regional cluster; Kilo device login bearer
printf '{"version":1,"credentials":[{"provider":"xiaomi","credential":{"type":"api_key","key":"tp-xm-key"}}]}' | "${XENV[@]}" "$B" auth import - >/dev/null 2>&1
check "xiaomi tp chat"    "plain provider answer"  "${XENV[@]}" "$B" --model xiaomi/mimo-v2-flash --no-session -p hi
check "xiaomi tp wire"    "/xmtp/v1/chat/completions Bearer tp-xm-key"  "${XL[@]}" /xmtp/ 'x["path"], h.get("authorization")'
check "kilo login"        "Logged in to Kilo Gateway"  bash -c "${XENV[*]} '$B' login kilo </dev/null"
check "kilo chat"         "plain provider answer"  "${XENV[@]}" "$B" --model "kilo/~anthropic/claude-sonnet-latest" --no-session -p hi
check "kilo wire"         "/kilo/api/gateway/chat/completions Bearer kilo-tok"  "${XL[@]}" /kilo/ 'x["path"], h.get("authorization")'
# GitHub Copilot: discovered plan endpoint, identity headers, initiator per turn, enterprise CLI identity + fallback
check "copilot login"     "Logged in to GitHub Copilot"  bash -c "printf '\n' | ${XENV[*]} '$B' login github-copilot"
check "copilot claude"    "Tool said: cp-e2e"  "${XENV[@]}" "$B" --model github-copilot/claude-haiku-4.5 --no-session -p "bash:echo cp-e2e"
check "copilot user turn" "/capi/v1/messages Bearer gho_test copilot-chat user conversation-user None None" \
    bash -c "python3 -I -c 'import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; x=[x for x in r if \"/capi/\" in x[\"path\"]][-2]; h=x[\"headers\"]; print(x[\"path\"], h.get(\"authorization\"), h.get(\"copilot-integration-id\"), h.get(\"x-initiator\"), h.get(\"x-interaction-type\"), h.get(\"x-api-key\"), h.get(\"anthropic-beta\"))' '$T/prov.jsonl'"
check "copilot agent turn" "agent conversation-agent copilot/1.0.82 2026-08-01"  "${XL[@]}" /capi/ 'h.get("x-initiator"), h.get("x-interaction-type"), h.get("user-agent"), h.get("x-github-api-version")'
check "copilot openai"    "plain provider answer"  "${XENV[@]}" "$B" --model github-copilot/gemini-2.5-pro --no-session -p hi
check "copilot openai wire" "/capi/chat/completions Bearer gho_test copilot-chat user"  "${XL[@]}" /capi/ 'x["path"], h.get("authorization"), h.get("copilot-integration-id"), h.get("x-initiator")'
check "copilot pinned id" "copilot-vscode"  bash -c "${XENV[*]} COPILOT_INTEGRATION_ID=copilot-vscode '$B' --model github-copilot/gemini-2.5-pro --no-session -p hi >/dev/null; python3 -I -c 'import json,sys; print([json.loads(l) for l in open(sys.argv[1])][-1][\"headers\"].get(\"copilot-integration-id\"))' '$T/prov.jsonl'"
check "copilot fallback"  "plain provider answer"  "${XENV[@]}" OMC_PROVIDER_URL_REWRITE="https://api.individual.githubcopilot.com=$XB/deny/capi" "$B" --model github-copilot/gemini-2.5-pro --no-session -p hi
check "copilot fallback wire" "copilot-chat copilot-developer-cli"  bash -c "python3 -I -c 'import json,sys; r=[json.loads(l) for l in open(sys.argv[1])][-2:]; print(*[x[\"headers\"].get(\"copilot-integration-id\") for x in r])' '$T/prov.jsonl'"
python3 -I -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["github-copilot"]["enterpriseUrl"]="ghe.example.com"; d["github-copilot"].pop("apiEndpoint",None); json.dump(d,open(p,"w"))' "$T/pagent/auth.json"
check "copilot enterprise" "plain provider answer"  "${XENV[@]}" "$B" --model github-copilot/gemini-2.5-pro --no-session -p hi
check "copilot ent wire"  "/deny/ghe/chat/completions copilot-developer-cli"  "${XL[@]}" /deny/ghe 'x["path"], h.get("copilot-integration-id")'
check "copilot denied"    "GitHub Copilot access denied (HTTP 403)"  "${XENV[@]}" COPILOT_INTEGRATION_ID=copilot-chat "$B" --model github-copilot/gemini-2.5-pro --no-session -p hi
# native judgment APIs as the judge role: TypeSafe System One (env key) and Workers AI Clef ({token, accountId} login)
JQ='call:eval:{"language":"py","code":"r = await judge({\"text\": \"ok\"}, {\"b\": {\"type\": \"bool\", \"instructions\": \"fine?\"}})\nprint(\"verdict\", r[\"b\"])"}'
printf 'modelRoles:\n  judge: typesafe/jev-latest\n' > "$T/pagent/config.yml"
check "typesafe judge"  "verdict {'type': 'bool', 'bool': 0.9}"  "${XENV[@]}" TYPESAFE_API_KEY=ts-key OMC_PROVIDER_URL_REWRITE="https://api.typesafe.ai=$XB/ts,https://api.kilo.ai=$XB/kilo" "$B" --model "kilo/~anthropic/claude-sonnet-latest" --no-session -p "$JQ"
check "typesafe wire"   "/ts/v1/systemone Bearer ts-key jev-latest {'b': {'type': 'noul', 'instructions': 'fine?'}}"  "${XL[@]}" /ts/ 'x["path"], h.get("authorization"), b.get("model"), b.get("questions")'
# OpenRouter Decisions: compat.openRouterRouting becomes `provider`, compat.extraBody overlays the body
mkdir -p "$T/ordagent"
printf 'providers:\n  ord:\n    baseUrl: %s/ord\n    apiKey: ord-key\n    api: openrouter-decisions\n    models:\n      - id: jev\n        name: Jev\n        compat:\n          openRouterRouting:\n            order: [cerebras]\n          extraBody:\n            zdr: true\n  mock:\n    baseUrl: %s/ord\n    apiKey: ord-key\n    api: openai-completions\n    models:\n      - id: c\n        name: C\n' "$XB" "$XB" > "$T/ordagent/models.yml"
printf 'modelRoles:\n  judge: ord/jev\n' > "$T/ordagent/config.yml"
check "decisions judge" "verdict {'type': 'bool', 'bool': 0.9}"  "${XENV[@]}" OMC_CODING_AGENT_DIR="$T/ordagent" "$B" --model mock/c --no-session -p "$JQ"
check "decisions wire"  "/ord/decisions Bearer ord-key {'order': ['cerebras']} True jev"  "${XL[@]}" /ord/decisions 'x["path"], h.get("authorization"), b.get("provider"), b.get("zdr"), b.get("model")'
check "workers login"   "Logged in to Cloudflare Workers AI"  bash -c "printf 'wa-tok\nacct7\n' | ${XENV[*]} '$B' login cloudflare-workers-ai"
printf 'modelRoles:\n  judge: cloudflare-workers-ai/@cf/cloudflare/clef\n' > "$T/pagent/config.yml"
check "workers judge"   "verdict {'type': 'bool', 'bool': 0.9}"  "${XENV[@]}" OMC_PROVIDER_URL_REWRITE="https://api.cloudflare.com=$XB/cfapi,https://api.kilo.ai=$XB/kilo" "$B" --model "kilo/~anthropic/claude-sonnet-latest" --no-session -p "$JQ"
check "workers wire"    "/cfapi/client/v4/accounts/acct7/ai/run/@cf/cloudflare/clef Bearer wa-tok clef"  "${XL[@]}" /cfapi/ 'x["path"], h.get("authorization"), b.get("model")'
rm -f "$T/pagent/config.yml"
# externalThinking (WP-27): the private think scratchpad is forced on the turn's first request with reasoning off
mkdir -p "$T/thagent"
printf 'providers:\n  thk:\n    baseUrl: %s/think\n    apiKey: TK\n    api: anthropic-messages\n    models:\n      - id: thk-1\n        contextWindow: 100000\n        maxTokens: 4096\n  thr:\n    baseUrl: %s/thinkr\n    apiKey: TK\n    api: openai-responses\n    models:\n      - id: thr-1\n        contextWindow: 100000\n        maxTokens: 4096\n' "$XB" "$XB" > "$T/thagent/models.yml"
printf 'externalThinking: true\n' > "$T/thagent/config.yml"
check "think forced"    "Tool said: ------"  env OMC_CODING_AGENT_DIR="$T/thagent" TK=x OMC_NO_TITLE=1 "$B" --model thk/thk-1 --no-session -p hi
# a custom anthropic-messages endpoint gets omp's cloaked request shape (resolveCustomModelIsOAuth): prefixed tool names
check "think wire"      "{'type': 'tool', 'name': '_think'} None private scratchpad; not shown to user"  "${XL[@]}" /think/ 'b.get("tool_choice"), b.get("thinking"), [t for t in b["tools"] if t["name"] == "_think"][0]["description"]' '"tool_choice"'
check "think continue"  "None ------"  "${XL[@]}" /think/ 'b.get("tool_choice"), b["messages"][-1]["content"][0]["content"]'
check "think responses" "{'effort': 'none'} {'type': 'function', 'name': 'think'}"  bash -c "env OMC_CODING_AGENT_DIR='$T/thagent' TK=x OMC_NO_TITLE=1 '$B' --model thr/thr-1 --no-session -p hi >/dev/null; ${XL[*]} /thinkr/ 'b.get(\"reasoning\"), b.get(\"tool_choice\")'"
mkdir -p "$T/thagent2" && cp "$T/agent/models.yml" "$T/thagent2/" && cp "$T/thagent/config.yml" "$T/thagent2/"
check "think off"       "False"  bash -c "env OMC_CODING_AGENT_DIR='$T/thagent2' '$B' --model mock/mock-1 --no-session -p hi >/dev/null 2>&1; python3 -I -c 'import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; r=[x for x in r if x.get(\"tools\")]; print(any(t[\"function\"][\"name\"]==\"think\" for t in r[-1][\"tools\"]))' '$T/req.jsonl'"
# generate_image (WP-27): the image role over xAI (openai-images) and an OpenAI hosted Responses carrier
mkdir -p "$T/igagent" && cp "$T/agent/models.yml" "$T/igagent/"
printf '  xai:\n    baseUrl: %s/img/v1\n    apiKey: XK\n  openai:\n    baseUrl: %s/imgo/v1\n    apiKey: XK\n' "$XB" "$XB" >> "$T/igagent/models.yml"
printf 'modelRoles:\n  image: xai/grok-imagine-image\ngenerate_image:\n  enabled: true\n' > "$T/igagent/config.yml"
IGC='call:write:{"path":"xd://generate_image","content":"{\"subject\":\"a red cube.\",\"aspect_ratio\":\"3:2\"}"}'
check "image gen"       "Tool said: Provider: xai"  env OMC_CODING_AGENT_DIR="$T/igagent" XK=x TMPDIR="$T" "$B" --model mock/mock-1 --no-session -p "$IGC"
check "image gen wire"  "/img/v1/images/generations grok-imagine-image a red cube. 3:2 1k b64_json"  "${XL[@]}" /img/ 'x["path"], b.get("model"), b.get("prompt"), b.get("aspect_ratio"), b.get("resolution"), b.get("response_format")'
# an exhausted chain names each provider failure, whitespace collapsed and its period dropped (omp a874c10430, f597096732)
mkdir -p "$T/igagent3" && cp "$T/agent/models.yml" "$T/igagent3/" && cp "$T/igagent/config.yml" "$T/igagent3/"
printf '  xai:\n    baseUrl: %s/img/v1\n    apiKey: XK\n' "$XB" >> "$T/igagent3/models.yml"
check "image gen fail"  "Image generation exhausted the resolved image chain: xai/grok-imagine-image. Failures: "  env OMC_CODING_AGENT_DIR="$T/igagent3" XK=x TMPDIR="$T" "$B" --model mock/mock-1 --no-session -p 'call:write:{"path":"xd://generate_image","content":"{\"subject\":\"fail now\"}"}'
printf 'modelRoles:\n  image: openai/gpt-image-2\ngenerate_image:\n  enabled: true\n' > "$T/igagent/config.yml"
check "image hosted"    "Model: gpt-image-2-2026 (catalog entry openai/gpt-image-2)"  env OMC_CODING_AGENT_DIR="$T/igagent" XK=x TMPDIR="$T" "$B" --model mock/mock-1 --no-session -p "$IGC"
check "image hosted wire" "/imgo/v1/responses gpt-5-nano {'type': 'image_generation'}"  "${XL[@]}" /imgo/ 'x["path"], b.get("model"), b.get("tool_choice")'
kill $XPID $XAPID 2>/dev/null
