# GitLab Duo (sourced by tests/e2e/run.sh: uses $B, $T, $PORT, $ROOT, check(), fail):
#   gitlab-duo        direct_access token exchange + Cloud Connector proxy (Anthropic OAuth wire / Responses / Completions)
#   gitlab-duo-agent  Duo Workflow: namespace + project discovery, workflow create, inline flow over the WebSocket,
#                     tool action answered on the same socket, pause_turn replay, 4xxx close mapping
# Fake credentials only (GITLAB_TOKEN=glpat-test); real hosts are rewritten to tests/e2e/mock_gitlab.py.
GLPORT=$((PORT + 12))
MOCK_LOG="$T/gitlab.jsonl" python3 -I "$ROOT/tests/e2e/mock_gitlab.py" $GLPORT & GLPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$GLPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/glwork"
GLENV=(env -u GITLAB_DUO_NAMESPACE_ID -u GITLAB_DUO_PROJECT_ID -u GITLAB_DUO_PROJECT_PATH HOME="$T" GITLAB_TOKEN=glpat-test
       OMC_PROVIDER_URL_REWRITE="https://gitlab.com=http://127.0.0.1:$GLPORT/gl,https://cloud.gitlab.com=http://127.0.0.1:$GLPORT/cgl")
GLA=gitlab-duo-agent/claude_sonnet_4_6_vertex
pushd "$T/glwork" >/dev/null
check "duo anthropic"     "plain duo answer"   "${GLENV[@]}" "$B" --model gitlab-duo/duo-chat-sonnet-4-6 --no-session -p "hi"
check "duo responses"     "plain duo answer"   "${GLENV[@]}" "$B" --model gitlab-duo/duo-chat-gpt-5-codex --no-session -p "hi"
check "duo completions"   "plain duo answer"   "${GLENV[@]}" "$B" --model gitlab-duo/duo-chat-gpt-5-mini --no-session -p "hi"
check "duo tool"          "Tool said: duo-e2e" "${GLENV[@]}" "$B" --model gitlab-duo/duo-chat-sonnet-4-6 --no-session -p "bash:echo duo-e2e"
check "duo bad token"     "Failed to get GitLab Duo direct access token: 401" \
                          env HOME="$T" GITLAB_TOKEN=wrong OMC_PROVIDER_URL_REWRITE="https://gitlab.com=http://127.0.0.1:$GLPORT/gl" \
                          "$B" --model gitlab-duo/duo-chat-gpt-5-mini --no-session -p "hi"
check "duo-agent text"    "plain workflow answer" "${GLENV[@]}" "$B" --model "$GLA" --no-session -p "hi"
check "duo-agent tool"    "Tool said: wf-e2e"     "${GLENV[@]}" "$B" --model "$GLA" --no-session -p "bash:echo wf-e2e"
check "duo-agent pause"   "part two"              "${GLENV[@]}" "$B" --model "$GLA" --no-session -p "pause"
check "duo-agent reject"  "GitLab Duo Workflow closed the session with HTTP 403 (WebSocket 4403): nope" \
                          "${GLENV[@]}" "$B" --model "$GLA" --no-session -p "reject"
popd >/dev/null
GLL="$T/gitlab.jsonl"
grep -q '"path": "/cgl/ai/v1/proxy/anthropic/v1/messages?beta=true", "auth": "Bearer duo-direct-tok", "headers": {[^}]*"x-gitlab-realm": "saas"' "$GLL" \
    && grep -q '"model": "claude-sonnet-4-6"' "$GLL" && echo "   ok  duo anthropic proxy wire" || { echo "   FAIL duo anthropic proxy wire"; fail=1; }
grep -q '"path": "/cgl/ai/v1/proxy/openai/v1/chat/completions".*"model": "gpt-5-mini-2025-08-07"' "$GLL" \
    && echo "   ok  duo upstream model id" || { echo "   FAIL duo upstream model id"; fail=1; }
grep -q '"query": "/gl/api/v4/ai/duo_workflows/ws?project_id=7&namespace_id=42&root_namespace_id=42&user_selected_model_identifier=claude_sonnet_4_6_vertex&workflow_definition=ambient"' "$GLL" \
    && echo "   ok  duo-agent websocket url" || { echo "   FAIL duo-agent websocket url"; fail=1; }
grep -q '"x-gitlab-client-type": "node-websocket"' "$GLL" && echo "   ok  duo-agent websocket headers" || { echo "   FAIL duo-agent websocket headers"; fail=1; }
grep -q '"flowConfigSchemaVersion": "v1", "flowConfig": {"version": "v1", "environment": "ambient"' "$GLL" \
    && echo "   ok  duo-agent inline flow" || { echo "   FAIL duo-agent inline flow"; fail=1; }
grep -q '"frame": {"actionResponse": {"requestID": "req-1", "plainTextResponse": {"response": "wf-e2e\\n"}}}' "$GLL" \
    && echo "   ok  duo-agent action response" || { echo "   FAIL duo-agent action response"; fail=1; }
[ "$(grep -c '"ws": "start"' "$GLL")" = 4 ] && echo "   ok  duo-agent socket reuse" || { echo "   FAIL duo-agent socket reuse"; fail=1; }
grep -q '"method": "PATCH", "path": "/gl/api/v4/ai/duo_workflows/workflows/[0-9]*".*"status_event": "stop"' "$GLL" \
    && echo "   ok  duo-agent stop on rejection" || { echo "   FAIL duo-agent stop on rejection"; fail=1; }
kill $GLPID 2>/dev/null
