# web_search providers (WP-25) against tests/e2e/mock_search.py; sourced from run.sh (uses $B $T $PORT $ROOT, check()
# and fail). Fake keys only; every backend origin is rewritten to the mock (OMC_PROVIDER_URL_REWRITE, SEARXNG_ENDPOINT),
# so no real search endpoint is reached. Request shapes were checked against omp v18.14.2 with
# tests/parity/search_diff.sh; the texts below are omc's own formatForLLM output of the mock answers.
WSPORT=$((PORT + 17))
MOCK_LOG="$T/websearch.jsonl" python3 -I "$ROOT/tests/e2e/mock_search.py" $WSPORT & WSPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$WSPORT) 2>/dev/null && break; sleep 0.1; done
WSM=http://127.0.0.1:$WSPORT
mkdir -p "$T/wsagent" "$T/wswork"
WSENV=(env -u ANTHROPIC_API_KEY -u ANTHROPIC_OAUTH_TOKEN -u OPENAI_API_KEY -u GEMINI_API_KEY -u XAI_API_KEY -u ZAI_API_KEY
       -u PERPLEXITY_API_KEY -u KEENABLE_API_KEY -u SOFYA_API_KEY -u EXA_API_KEY -u BRAVE_API_KEY -u TAVILY_API_KEY
       HOME="$T" OMC_CODING_AGENT_DIR="$T/wsagent" NO_COLOR=1 SEARXNG_ENDPOINT="$WSM/searx"
       OMC_PROVIDER_URL_REWRITE="https://api.anthropic.com=$WSM/anthropic,https://api.openai.com=$WSM/openai,https://chatgpt.com=$WSM/chatgpt,https://generativelanguage.googleapis.com=$WSM/google,https://api.x.ai=$WSM/xai,https://api.perplexity.ai=$WSM/pplx,https://api.kimi.com=$WSM/kimi,https://api.keenable.ai=$WSM/keenable,https://sofya.co=$WSM/sofya,https://api.z.ai=$WSM/zai,https://search.parallel.ai=$WSM/parallel,https://html.duckduckgo.com=$WSM/ddg,https://registry.npmjs.org=$WSM/npm")
ws_search() { # selector query [env...] -> rendered output of `omc search`
    local sel=$1 q=$2; shift 2
    (cd "$T/wswork" && timeout 30 "${WSENV[@]}" "$@" "$B" search ${sel:+--model "$sel"} "$q" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
}
ws_last() { tail -1 "$T/websearch.jsonl" | python3 -c "import json,sys; r=json.load(sys.stdin); print(r['path']); print(json.dumps(r['body']))"; }
out=$(ws_search "" "chain query")
[[ "$out" == *"Parallel Source"* && "$(ws_last)" == /parallel/mcp* ]] && echo "   ok  web search: default chain -> parallel" || { echo "   FAIL web search default chain: $out"; fail=1; }
out=$(ws_search anthropic/claude-sonnet-4-5 "anthropic query" ANTHROPIC_API_KEY=sk-ant-api-fake)
req=$(ws_last)
[[ "$out" == *"Anthropic answer about anthropic query"* && "$req" == *'"tools": [{"type": "web_search_20250305", "name": "web_search"}]'* && "$req" == *"/anthropic/v1/messages?beta=true"* ]] \
    && echo "   ok  web search: anthropic grounding" || { echo "   FAIL web search anthropic: $out / $req"; fail=1; }
# a search backend must always search: the instruction rides on API-key requests too; a decline is reported (omp 88e412881e)
[[ "$req" == *'Always call web_search at least once before answering'* ]] \
    && echo "   ok  web search: anthropic search instruction" || { echo "   FAIL web search anthropic instruction: $req"; fail=1; }
out=$(ws_search anthropic/claude-sonnet-4-5 "refuse me" ANTHROPIC_API_KEY=sk-ant-api-fake)
[[ "$out" == *"claude-sonnet-4-5 declined the query (cyber safety classifier): Not this one."* ]] \
    && echo "   ok  web search: anthropic refusal" || { echo "   FAIL web search anthropic refusal: $out"; fail=1; }
out=$(ws_search openai/gpt-5.2 "openai query" OPENAI_API_KEY=sk-fake)
req=$(ws_last)
[[ "$out" == *"Responses answer about openai query"* && "$req" == *'"tool_choice": {"type": "web_search"}'* && "$req" == *'"include": ["web_search_call.action.sources"]'* ]] \
    && echo "   ok  web search: openai responses" || { echo "   FAIL web search openai: $out / $req"; fail=1; }
out=$(ws_search google/gemini-2.5-pro "gemini query" GEMINI_API_KEY=g-fake)
[[ "$out" == *"Gemini answer about gemini query"* && "$(ws_last)" == *':streamGenerateContent?alt=sse'*'"googleSearch": {}'* ]] \
    && echo "   ok  web search: gemini grounding" || { echo "   FAIL web search gemini: $out"; fail=1; }
out=$(ws_search xai/grok-4 "xai query" XAI_API_KEY=x-fake)
[[ "$out" == *"Responses answer about xai query"* && "$(ws_last)" == *'{"type": "x_search"}'*'"max_turns": 2'* ]] \
    && echo "   ok  web search: xai web + x search" || { echo "   FAIL web search xai: $out"; fail=1; }
printf '{"openai-codex":{"type":"oauth","access":"tok-codex","refresh":"r","expires":4102444800000}}' > "$T/wsagent/auth.json"
out=$(ws_search openai-codex/gpt-6-luna "codex query")
rm -f "$T/wsagent/auth.json"
[[ "$out" == *"Codex answer about codex query"* && "$(ws_last)" == /chatgpt/backend-api/codex/responses* ]] \
    && echo "   ok  web search: codex grounding" || { echo "   FAIL web search codex: $out"; fail=1; }
# the Codex client version (omp f54745ac23): PI_AI_CODEX_CLIENT_VERSION pins it; unset, the newer of the 0.169.0 pin and
# <agent dir>/codex-client-version.json goes out, and a missing or 6h-old record is refreshed from npm in the background
printf '{"openai-codex":{"type":"oauth","access":"tok-codex","refresh":"r","expires":4102444800000}}' > "$T/wsagent/auth.json"
ws_search openai-codex/gpt-6-luna "codex pinned" PI_AI_CODEX_CLIENT_VERSION=0.150.0 >/dev/null
check "codex version pinned" "0.150.0"  python3 -I -c 'import json,sys; print([json.loads(l) for l in open(sys.argv[1])][-1]["headers"].get("version"))' "$T/websearch.jsonl"
ws_search openai-codex/gpt-6-luna "codex default" PI_AI_CODEX_CLIENT_VERSION= >/dev/null
for _ in $(seq 30); do grep -q '"path": "/npm/@openai/codex/latest"' "$T/websearch.jsonl" && break; sleep 0.1; done
check "codex version default" "0.169.0 True"  python3 -I -c 'import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; print([x for x in r if x["path"].endswith("/codex/responses")][-1]["headers"].get("version"), any(x["path"] == "/npm/@openai/codex/latest" for x in r))' "$T/websearch.jsonl"
printf '{"version":"0.181.0","checkedAt":%s}' "$(( $(date +%s) * 1000 ))" > "$T/wsagent/codex-client-version.json"
ws_search openai-codex/gpt-6-luna "codex recorded" PI_AI_CODEX_CLIENT_VERSION= >/dev/null
check "codex version recorded" "0.181.0"  python3 -I -c 'import json,sys; print([json.loads(l) for l in open(sys.argv[1])][-1]["headers"].get("version"))' "$T/websearch.jsonl"
rm -f "$T/wsagent/auth.json" "$T/wsagent/codex-client-version.json"
out=$(ws_search web/perplexity "pplx query" PERPLEXITY_API_KEY=p-fake)
[[ "$out" == *"Perplexity answer about pplx query"* && "$(ws_last)" == *'"return_related_questions": true'* ]] \
    && echo "   ok  web search: perplexity api" || { echo "   FAIL web search perplexity: $out"; fail=1; }
out=$(ws_search web/kimi "kimi query" MOONSHOT_SEARCH_API_KEY=k-fake)
[[ "$out" == *"Kimi Source"* && "$(ws_last)" == *'"text_query": "kimi query", "limit": 10'* ]] \
    && echo "   ok  web search: kimi" || { echo "   FAIL web search kimi: $out"; fail=1; }
out=$(ws_search web/zai "zai query" ZAI_API_KEY=z-fake)
[[ "$out" == *"Z.AI Source"* && $(grep -c '/web_search_prime/mcp' "$T/websearch.jsonl") -eq 3 ]] \
    && echo "   ok  web search: z.ai mcp" || { echo "   FAIL web search zai: $out"; fail=1; }
# Keenable: keyless on explicit selection only, operators never sent as keywords, keyed through X-API-Key
ws_hdr() { tail -1 "$T/websearch.jsonl" | python3 -c "import json,sys; r=json.load(sys.stdin); print(r['path'], json.dumps(r['body'], sort_keys=True), r['headers'].get('x-api-key'), r['headers'].get('x-keenable-title'))"; }
out=$(ws_search web/keenable "keen query")
[[ "$out" == *"Keenable Source"* && "$out" == *"https://example.com/two"* && "$out" != *"ftp://"* && "$(ws_hdr)" == '/keenable/v1/search/public {"max_results": 10, "query": "keen query"} None oh-my-pi' ]] \
    && echo "   ok  web search: keenable keyless" || { echo "   FAIL web search keenable keyless: $out / $(ws_hdr)"; fail=1; }
out=$(ws_search web/keenable 'site:github.com/x "exact phrase" foo after:2024 before:2025-02 -bar' KEENABLE_API_KEY=keen-fake)
[[ "$out" == *"Keenable Source"* && "$(ws_hdr)" == '/keenable/v1/search {"max_results": 10, "published_after": "2024-01-01", "published_before": "2025-02-01", "query": "\"exact phrase\" foo", "site": "github.com"} keen-fake None' ]] \
    && echo "   ok  web search: keenable keyed, operators" || { echo "   FAIL web search keenable keyed: $out / $(ws_hdr)"; fail=1; }
out=$(ws_search web/keenable 'site:a.com' KEENABLE_API_KEY=keen-fake)
[[ "$(ws_hdr)" == '/keenable/v1/search {"max_results": 10, "query": "a.com", "site": "a.com"} keen-fake None' ]] \
    && echo "   ok  web search: keenable site-only" || { echo "   FAIL web search keenable site-only: $(ws_hdr)"; fail=1; }
# Sofya (omp aff70c46f9): bearer key, phrases and exclusions kept, site:/-site: as domain lists, a closed freshness range
out=$(ws_search web/sofya 'site:github.com/x -site:spam.com "exact phrase" foo after:2024-01-01 before:2025-02-01 -bar' SOFYA_API_KEY=sofya-fake)
sofya_req=$(tail -1 "$T/websearch.jsonl" | python3 -c "import json,sys; r=json.load(sys.stdin); print(r['path'], json.dumps(r['body'], sort_keys=True), r['headers'].get('authorization'))")
[[ "$out" == *"Sofya Source"* && "$out" == *"page body"* && "$sofya_req" == '/sofya/v1/search {"exclude_domains": ["spam.com"], "freshness": "2024-01-01:2025-01-31", "include_domains": ["github.com"], "max_results": 10, "query": "\"exact phrase\" foo -bar", "search_depth": "snippets"} Bearer sofya-fake' ]] \
    && echo "   ok  web search: sofya" || { echo "   FAIL web search sofya: $out / $sofya_req"; fail=1; }
out=$(ws_search web/sofya 'plain' )
[[ "$out" == *"Sofya web search is unavailable. Configure its credentials"* ]] \
    && echo "   ok  web search: sofya no key" || { echo "   FAIL web search sofya no key: $out"; fail=1; }
out=$(ws_search web/keenable 'after:2024' KEENABLE_API_KEY=keen-fake)
[[ "$out" == *"Keenable needs search terms; date-only queries are not supported"* ]] \
    && echo "   ok  web search: keenable date-only" || { echo "   FAIL web search keenable date-only: $out"; fail=1; }
out=$(ws_search web/searxng "searx query")
[[ "$out" == *"searx answer"* && "$(ws_last)" == "/searx/search?q=searx+query&format=json"* ]] \
    && echo "   ok  web search: searxng" || { echo "   FAIL web search searxng: $out"; fail=1; }
# failures fall through the role chain with omp's note; a chain that fails throughout lists every failure
printf 'modelRoles:\n  web: web/searxng\nretry:\n  fallbackChains:\n    web: [web/kimi]\n' > "$T/wsagent/config.yml"
out=$(ws_search "" "fallback query" SEARXNG_ENDPOINT="$WSM/fail/searx" MOONSHOT_SEARCH_API_KEY=k-fake)
[[ "$out" == *"Kimi Source"* && "$(tail -2 "$T/websearch.jsonl" | head -1)" == *'"path": "/fail/searx/search?q=fallback+query&format=json"'* ]] \
    && echo "   ok  web search: role-chain fallback" || { echo "   FAIL web search fallback: $out"; fail=1; }
# details.failures names the providers that failed before the one that answered, and none when the first one answers
cp "$T/agent/models.yml" "$T/wsagent/models.yml"
ws_details() { (cd "$T/wswork" && timeout 30 "${WSENV[@]}" MOCK_KEY=x "$@" "$B" --model mock/mock-1 --no-session --mode json -p 'call:web_search:{"query":"fallback query"}' </dev/null 2>/dev/null | grep -o '"failures":[^]]*]' | head -1); }
out=$(ws_details SEARXNG_ENDPOINT="$WSM/fail/searx" MOONSHOT_SEARCH_API_KEY=k-fake)
[[ "$out" == '"failures":[{"provider":"searxng","label":"SearXNG","reason":"SearXNG API error (500):'*'"status":500}]' ]] \
    && echo "   ok  web search: details.failures" || { echo "   FAIL web search details.failures: $out"; fail=1; }
out=$(ws_details MOONSHOT_SEARCH_API_KEY=k-fake)
[[ -z "$out" ]] && echo "   ok  web search: no failures key" || { echo "   FAIL web search no failures key: $out"; fail=1; }
rm -f "$T/wsagent/models.yml"
out=$(ws_search "" "FAIL500 query" MOONSHOT_SEARCH_API_KEY=k-fake)
rm -f "$T/wsagent/config.yml"
[[ "$out" == *"Error: All web search providers failed: searxng: SearXNG API error (500): "*"; kimi: Kimi search API error (500): "* ]] \
    && echo "   ok  web search: all providers failed" || { echo "   FAIL web search all failed: $out"; fail=1; }
out=$(ws_search web/kimi "no key")
[[ "$out" == *"Error: Kimi web search is unavailable. Configure its credentials or select the automatic"* ]] \
    && echo "   ok  web search: explicit unavailable" || { echo "   FAIL web search explicit unavailable: $out"; fail=1; }
# providers.webKeyless off: keyless services are skipped (omp d1ab1a86c9)
mkdir -p "$T/wkagent" && cp "$T/agent/models.yml" "$T/wkagent/" && printf 'providers:\n  webKeyless: false\n' > "$T/wkagent/config.yml"
check "web search keyless off" "providers.webKeyless is off. Configure a search credential or turn providers.webKeyless on."  env -u PARALLEL_API_KEY -u EXA_API_KEY OMC_CODING_AGENT_DIR="$T/wkagent" timeout 30 "$B" --model mock/mock-1 --no-session -p 'call:web_search:{"query":"x"}'
kill $WSPID 2>/dev/null
wait $WSPID 2>/dev/null
