# Sourced by tests/e2e/run.sh: MCP protocol revision 2026-07-28 (omp acdfffa248) against tests/e2e/mock_mcp.py --modern.
# server/discover decides the era (the legacy mock answers it with -32601 and keeps the initialize handshake, covered by
# every other MCP check); a modern server gets the request _meta on every request, no initialize session, roots through
# an input_required round trip, and its list changes on a subscriptions/listen stream.
mkdir -p "$T/mcpmod" && cp "$T/agent/models.yml" "$T/mcpmod/"
printf '{"mcpServers":{"modern":{"command":"python3","args":["-I","%s","--modern"]}}}' "$ROOT/tests/e2e/mock_mcp.py" > "$T/mcpmod/mcp.json"
MMENV=(env OMC_CODING_AGENT_DIR="$T/mcpmod")
check "mcp modern tool"  "Tool said: modern echo: hi"  "${MMENV[@]}" "$B" --model mock/mock-1 --no-session -p 'call:mcp__modern_echo_text:{"text":"hi"}'
check "mcp modern roots" "Tool said: roots True state=st-1 initialized=False"  "${MMENV[@]}" "$B" --model mock/mock-1 --no-session -p 'call:mcp__modern_echo_text:{"text":"roots"}'
check "mcp modern instructions" "Modern MCP server instructions."  python3 -I -c 'import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; print(json.dumps(r[-1]["messages"][0]))' "$T/req.jsonl"
timeout 60 "${MMENV[@]}" python3 -I "$ROOT/tests/e2e/rpc_seq.py" "$B" --arg --model --arg mock/mock-1 --arg --no-session \
    'call:mcp__modern_echo_text:{"text":"notify"}' 'call:mcp__modern_late_tool:{}' </dev/null >/dev/null 2>&1
check "mcp modern listen" "modern echo: "  python3 -I -c "$LASTMSG" "$T/req.jsonl"
