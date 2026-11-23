# Provider usage (WP-24) against tests/e2e/mock_usage.py; sourced from run.sh (uses $B $T $PORT $ROOT, check() and
# fail). Fake credentials only: Anthropic / Z.AI / Copilot reach the mock through models.yml baseUrl
# overrides (as omp does), Codex (canonical ChatGPT origin only) through OMC_AUTH_URL_REWRITE. Expected strings
# were checked against omp v18.14.2 driven against the same mock.
USGPORT=$((PORT + 13))
MOCK_LOG="$T/usage.jsonl" python3 -I "$ROOT/tests/e2e/mock_usage.py" $USGPORT & USGPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$USGPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/usgagent" "$T/usgwork"
cat > "$T/usgagent/models.yml" <<YML
providers:
  anthropic:
    baseUrl: http://127.0.0.1:$USGPORT/v1
  zai:
    baseUrl: http://127.0.0.1:$USGPORT/api/coding/paas/v4
  github-copilot:
    baseUrl: http://127.0.0.1:$USGPORT
YML
USGEXP=$(( ($(date +%s) + 86400) * 1000 ))
cat > "$T/usgcreds.json" <<JSON
{"version": 1, "credentials": [
 {"provider": "anthropic", "credential": {"type": "oauth", "access": "sk-ant-oat01-mock", "refresh": "sk-ant-ort01-mock", "expires": $USGEXP}},
 {"provider": "zai", "credential": {"type": "api_key", "key": "zai-mock-key"}},
 {"provider": "github-copilot", "credential": {"type": "oauth", "access": "tid=mock", "refresh": "gho_mock", "expires": $USGEXP}},
 {"provider": "openai-codex", "credential": {"type": "oauth", "access": "codex-mock-token", "refresh": "codex-r", "expires": $USGEXP, "accountId": "acct-codex-1", "email": "codex.user@example.com"}}
]}
JSON
USGENV=(env -u ANTHROPIC_API_KEY -u ANTHROPIC_OAUTH_TOKEN -u GH_TOKEN -u GITHUB_TOKEN -u COPILOT_GITHUB_TOKEN -u ZAI_API_KEY
        -u SYNTHETIC_API_KEY HOME="$T/usgwork" OMC_CODING_AGENT_DIR="$T/usgagent"
        OMC_AUTH_URL_REWRITE="https://chatgpt.com=http://127.0.0.1:$USGPORT")
"${USGENV[@]}" "$B" auth import "$T/usgcreds.json" >/dev/null 2>&1
pushd "$T/usgwork" >/dev/null
check "usage breakdown"  $'Anthropic \xe2\x80\x94 1 account\n  \xe2\x97\x8f claude.user@example.com \xc2\xb7 plan: Max 20x\n      \xe2\x97\x8f Claude 5 Hour         \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91  42.0% used \xc2\xb7 resets in 2h' \
    "${USGENV[@]}" "$B" usage
check "usage extra usd"  'Claude Extra Usage    ' "${USGENV[@]}" "$B" usage
check "usage capacity"   $'capacity: 5h \xe2\x86\x92 0.42/1 account used (0.58\xc3\x97 quota left) \xc2\xb7 7d \xe2\x86\x92 0.77/1 account used' "${USGENV[@]}" "$B" usage
check "usage codex"      $'\xe2\x97\x8f codex.user@example.com \xc2\xb7 plan: plus \xc2\xb7 daybreak \xc2\xb7 \xe2\x9c\xa6 2 saved resets' "${USGENV[@]}" "$B" usage -p openai-codex
check "usage codex spark" '5 hours (Spark)' "${USGENV[@]}" "$B" usage -p openai-codex
check "usage copilot"    "Premium Requests (individual) (Monthly)" "${USGENV[@]}" "$B" usage -p github-copilot
check "usage zai"        "account 1 · plan: pro" "${USGENV[@]}" "$B" usage -p zai
check "usage redact"     $'  \xe2\x97\x8f co* \xc2\xb7 plan: plus' "${USGENV[@]}" "$B" usage -p openai-codex -r
check "usage json"       '"availableCount": 2' "${USGENV[@]}" "$B" usage -p openai-codex --json
check "usage json raw"   "no raw" bash -c "${USGENV[*]} '$B' usage --json | grep -c '\"raw\"' | sed 's/^0$/no raw/'"
check "usage no provider" 'No credentials stored for provider "nope". Providers with stored credentials: anthropic, github-copilot, openai-codex, zai.' \
    "${USGENV[@]}" "$B" usage -p nope
check "usage accounts"   $'anthropic\n  (no identity key: no pool can name this account)\ngithub-copilot\n  (no identity key: no pool can name this account)\nopenai-codex\n  email:codex.user@example.com' "${USGENV[@]}" "$B" usage accounts
check "usage accounts redact" '`omc usage accounts` prints identity keys verbatim for configuration; --redact does not apply.' "${USGENV[@]}" "$B" usage accounts -r
check "usage history"    $'Usage history \xc2\xb7 last 7d \xc2\xb7 peak per bucket' "${USGENV[@]}" "$B" usage --history
check "usage invalidate" 'Invalidated cached usage reports for provider "zai".' "${USGENV[@]}" "$B" usage invalidate -p zai
check "usage reset list" $'Saved rate-limit resets:\n- codex.user@example.com [Codex \xc2\xb7 openai-codex/5]: 2 saved, 2 usable now, expires' "${USGENV[@]}" "$B" usage reset
check "usage reset claude" $'- account [Claude \xc2\xb7 anthropic/1]: 0 saved, 0 usable now (not eligible)' "${USGENV[@]}" "$B" usage reset
check "usage reset active" '`omc usage reset` runs outside a session, so no account is active.' "${USGENV[@]}" "$B" usage reset codex/active
check "usage reset spend" $'Reset applied for codex.user@example.com (Codex) \xe2\x80\x94 your rate-limit window has been refreshed.' "${USGENV[@]}" "$B" usage reset codex/5
check "usage help"       "Saved reset to spend with \`reset\`: <provider>/<credential id> as \`omc usage reset\` lists it" "$B" usage --help
# text-mode /usage over RPC (helpers/usage-report.ts) and ACP _omp/usage
check "usage rpc"        $'Anthropic\\n  Models with usage data\\n    anthropic/claude' \
    bash -c "printf '%s\n' '{\"id\":\"u\",\"type\":\"prompt\",\"message\":\"/usage\"}' | (cat; sleep 3) | ${USGENV[*]} '$B' --model anthropic/claude-sonnet-4-5 --mode rpc"
check "usage acp"        '"provider":"anthropic","fetchedAt"' \
    bash -c "printf '%s\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":1}}' '{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"_omp/usage\",\"params\":{}}' | (cat; sleep 3) | ${USGENV[*]} '$B' acp"
# probe shapes: Claude OAuth headers (usage betas, claude-cli UA), Codex account header, Z.AI raw Authorization
python3 -I - "$T/usage.jsonl" <<'PY' && echo "   ok  usage request shape" || { echo "   FAIL usage request shape"; fail=1; }
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
claude = [r for r in rows if r["path"] == "/api/oauth/usage"]
assert claude and claude[0]["headers"]["authorization"] == "Bearer sk-ant-oat01-mock", claude[:1]
assert claude[0]["headers"]["anthropic-beta"].startswith("claude-code-20250219,oauth-2025-04-20,"), claude[0]["headers"]
assert claude[0]["headers"]["user-agent"].startswith("claude-cli/"), claude[0]["headers"]
assert any(r["path"] == "/api/oauth/usage?cedar_ember=1&skip_spend=1" for r in rows)
codex = [r for r in rows if r["path"] == "/backend-api/wham/usage"]
assert codex and codex[0]["headers"]["chatgpt-account-id"] == "acct-codex-1", codex[:1]
zai = [r for r in rows if r["path"] == "/api/monitor/usage/quota/limit"]
assert zai and zai[0]["headers"]["authorization"] == "zai-mock-key", zai[:1]
consume = [r for r in rows if r["path"].endswith("/consume")]
assert consume and consume[0]["body"]["credit_id"] == "rc_1" and consume[0]["body"]["account_id"] == "acct-codex-1", consume
PY
popd >/dev/null
kill $USGPID 2>/dev/null
