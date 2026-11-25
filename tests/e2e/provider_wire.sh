# Sourced by tests/e2e/run.sh: provider wire behaviour with no better home.
WPORT=$((PORT + 42))
MOCK_FAIL_RETRY_AFTER_MS=none MOCK_SHOULD_RETRY=false MOCK_LOG="$T/wire.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $WPORT & WPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$WPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/wagent"
sed "s#127.0.0.1:$PORT/#127.0.0.1:$WPORT/#" "$T/agent/models.yml" > "$T/wagent/models.yml"
printf 'retry:\n  enabled: false\n' > "$T/wagent/config.yml"
# a gateway's x-should-retry: false is terminal: one request instead of the transport's six and the replay-safe retry
OMC_CODING_AGENT_DIR="$T/wagent" timeout 30 "$B" --model mock/mock-1 --no-session -p "fail:503:gateway gave up" </dev/null >/dev/null 2>&1
n=$(grep -c "gateway gave up" "$T/wire.jsonl")
(( n == 2 )) && echo "   ok  x-should-retry false" || { echo "   FAIL x-should-retry false: $n requests"; fail=1; }
kill $WPID 2>/dev/null; wait $WPID 2>/dev/null
# an MCP tool whose schema OpenAI's validator refuses (a lookaround and a nested character class in `pattern`s, omp
# e48b03993b) reaches an OpenAI Responses host sanitized: the mock answers 400 invalid_function_parameters otherwise
SPORT=$((PORT + 44))
MOCK_OPENAI_SCHEMA_CHECK=1 MOCK_LOG="$T/rschema.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $SPORT & SPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$SPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/rschema"
printf 'providers:\n  mockr:\n    baseUrl: http://127.0.0.1:%s/v1\n    apiKey: MOCK_KEY\n    api: openai-responses\n    models:\n      - id: mock-r\n        reasoning: false\n' $SPORT > "$T/rschema/models.yml"
printf 'tools:\n  commands: false\n' > "$T/rschema/config.yml" # MCP tools top-level, not bash tool commands
python3 -I - "$T/rschema/mcp.json" "$ROOT/tests/e2e/mock_mcp.py" <<'PY'
import json, sys
schema = {"type": "object", "properties": {"fileKey": {"type": "string", "pattern": "^(?!undefined$|null$)"},
                                           "query": {"type": "string", "pattern": "^[^[\\]]*$"},
                                           "slug": {"type": "string", "pattern": "^[a-z0-9_-]+$"}, "meta": {"type": "object"}},
          "required": ["fileKey"]}
json.dump({"mcpServers": {"figma": {"command": "python3", "args": ["-I", sys.argv[2]],
                                    "env": {"MOCK_MCP_EXTRA_TOOL": "get_design_context", "MOCK_MCP_EXTRA_SCHEMA": json.dumps(schema)}}}},
          open(sys.argv[1], "w"))
PY
check "responses mcp schema" "plain provider answer"  env OMC_CODING_AGENT_DIR="$T/rschema" "$B" --model mockr/mock-r --no-session -p "hi"
python3 -I - "$T/rschema.jsonl" <<'PY' && echo "   ok  responses mcp schema sanitized" || { echo "   FAIL responses mcp schema sanitized"; fail=1; }
import json, sys
reqs = [r["body"] for r in map(json.loads, open(sys.argv[1])) if r["path"].endswith("/responses") and r["body"].get("tools")]
tool = next(t for t in reqs[-1]["tools"] if t.get("name", "").endswith("get_design_context"))
props = tool["parameters"]["properties"]
assert "pattern" not in props["fileKey"] and "pattern" not in props["query"], props
assert props["slug"]["pattern"] == "^[a-z0-9_-]+$" and props["meta"] == {"type": "object", "properties": {}, "additionalProperties": True}, props
PY
# Codex sends a tool strict only when the tool itself is (omp's per-tool `strict = true`); task and MCP tools are never
# strict and say so (`strict: false`)
mkdir -p "$T/rcodex"
cp "$T/rschema/config.yml" "$T/rschema/mcp.json" "$T/rcodex/"
printf '{"openai-codex":{"type":"oauth","access":"tok-codex","refresh":"r","expires":4102444800000}}' > "$T/rcodex/auth.json"
check "codex tool strict" "plain provider answer"  env OMC_CODING_AGENT_DIR="$T/rcodex" PI_AI_CODEX_CLIENT_VERSION=0.150.0 \
    OMC_PROVIDER_URL_REWRITE="https://chatgpt.com=http://127.0.0.1:$SPORT/chatgpt,https://api.openai.com=http://127.0.0.1:$SPORT/openai" \
    "$B" --model openai-codex/gpt-6-luna --no-session -p "hi"
python3 -I - "$T/rschema.jsonl" <<'PY' && echo "   ok  codex tool strict flags" || { echo "   FAIL codex tool strict flags"; fail=1; }
import json, sys
reqs = [r["body"] for r in map(json.loads, open(sys.argv[1])) if r["path"].endswith("/codex/responses") and r["body"].get("tools")]
tools = {t.get("name"): t for t in reqs[-1]["tools"]}
assert tools["read"].get("strict") is True and tools["read"]["parameters"].get("additionalProperties") is False, tools["read"]
assert tools["task"].get("strict") is False, tools["task"]
mcp = next(t for n, t in tools.items() if n.endswith("get_design_context"))
assert mcp.get("strict") is False and "additionalProperties" not in mcp["parameters"], mcp
PY
kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
