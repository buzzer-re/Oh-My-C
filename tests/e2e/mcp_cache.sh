# Sourced by tests/e2e/run.sh: omp's MCP tool cache (mcp/tool-cache.ts; omc: <agentDir>/mcp-tool-cache.json), the
# DeferredMCPTools a server still connecting after the 250 ms startup window gets from it, and print mode's MCP
# readiness wait (print-mode.ts evaluateReadiness), against tests/e2e/mock_mcp.py answering initialize after 2 s.
# Checked against omp 18.14.2 with the same mocks: RPC run 1 (no cache) sends no MCP route, run 2 sends the cached
# route in its first request and the call waits for the connection; print mode waits for the server first.
MCPC_PORT=$((PORT + 26))
MOCK_LOG="$T/mcpc.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $MCPC_PORT & MCPC_PID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$MCPC_PORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/mcpcagent"
sed "s#127.0.0.1:$PORT/#127.0.0.1:$MCPC_PORT/#" "$T/agent/models.yml" > "$T/mcpcagent/models.yml"
printf 'modelRoles:\n  default: mock/mock-1\n' > "$T/mcpcagent/config.yml"
printf '{"mcpServers":{"slowmcp":{"command":"python3","args":["-I","%s","--startup-delay=2"]}}}' "$ROOT/tests/e2e/mock_mcp.py" > "$T/mcpcagent/mcp.json"
MCPC_ENV=(env OMC_CODING_AGENT_DIR="$T/mcpcagent")
# first tool-carrying request: does the system prompt route the MCP tool / carry the server instructions; the last
# tool result the mock saw
cat > "$T/mcpc_summary.py" <<'PY'
import json, sys
rs = [json.loads(l) for l in open(sys.argv[1])]
rs = [r for r in rs if r.get("tools")]
def text(m):
    c = m.get("content")
    return c if isinstance(c, str) else "".join(x.get("text", "") for x in (c or []))
sp = text(rs[0]["messages"][0]) if rs else ""
tool = [text(m) for r in rs for m in r["messages"] if m["role"] == "tool"]
print("route=%s instructions=%s result=%s" % ("→ `mcp__slowmcp_echo_text`" in sp, "Mock MCP server instructions" in sp,
                                              tool[-1] if tool else "-"))
PY
MCPC_SUM=(python3 -I "$T/mcpc_summary.py" "$T/mcpc.jsonl")
# run 1, no cache yet: the server misses the startup window, so the first request has no route; it connects in
# the background (the session stays up past the 2 s delay) and its tools/list is cached
: > "$T/mcpc.jsonl"
"${MCPC_ENV[@]}" timeout 60 python3 -I "$ROOT/tests/parity/rpc_drive.py" "$B" "bash:sleep 3" >/dev/null 2>&1
check "mcp cache: cold start"   "route=False instructions=False"  "${MCPC_SUM[@]}"
check "mcp cache: written"      '"slowmcp":{"version":1,"configHash":"'  cat "$T/mcpcagent/mcp-tool-cache.json"
# run 2: the cached tools are registered as deferred tools at the end of the window, so the first request routes
# them; the call waits for the connection and succeeds
: > "$T/mcpc.jsonl"
"${MCPC_ENV[@]}" timeout 60 python3 -I "$ROOT/tests/parity/rpc_drive.py" "$B" 'call:mcp__slowmcp_echo_text:{"text":"cached"}' >/dev/null 2>&1
check "mcp cache: deferred"     "route=True instructions=False result=mcp echo: cached"  "${MCPC_SUM[@]}"
# a changed server config misses the cache
sed -i 's/--startup-delay=2/--startup-delay=2.0/' "$T/mcpcagent/mcp.json"
: > "$T/mcpc.jsonl"
"${MCPC_ENV[@]}" timeout 60 python3 -I "$ROOT/tests/parity/rpc_drive.py" "$B" "say:x" 0.2 >/dev/null 2>&1
check "mcp cache: config hash"  "route=False"  "${MCPC_SUM[@]}"
# print mode waits for MCP startup before the first turn, so the server's instructions make the first request
: > "$T/mcpc.jsonl"
check "mcp print readiness"     "mcp echo: printed"  "${MCPC_ENV[@]}" "$B" -p 'call:mcp__slowmcp_echo_text:{"text":"printed"}'
check "mcp print first request" "route=True instructions=True"  "${MCPC_SUM[@]}"
# a server that is not ready: warned about; OMP_MCP_REQUIRE_READY refuses the run (exit 1, no request)
: > "$T/mcpc.jsonl"
check "mcp print not ready"     'Warning: MCP server "slowmcp" failed to connect: '  "${MCPC_ENV[@]}" OMP_MCP_TIMEOUT_MS=300 "$B" -p "say:x"
check "mcp require ready"       $'Error: MCP servers not ready: slowmcp\nexit=1 requests=0' bash -c "${MCPC_ENV[*]} OMP_MCP_TIMEOUT_MS=300 OMP_MCP_REQUIRE_READY=1 '$B' -p say:y 2>&1 | grep -v '^Warning'; echo \"exit=\${PIPESTATUS[0]} requests=\$(grep -c say:y '$T/mcpc.jsonl')\""
kill $MCPC_PID 2>/dev/null; wait $MCPC_PID 2>/dev/null
