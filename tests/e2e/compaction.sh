# Sourced by tests/e2e/run.sh: context maintenance (coding/compaction.c) over RPC against the mocks — the
# compaction.methodOrder ladder, omp's summary requests, overflow recovery, and provider-native compaction
# (OpenAI Responses /responses/compact, the Anthropic compaction beta) with its replay on the next turn.
CMPORT=$((PORT + 11))
MOCK_LOG="$T/cprov.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $CMPORT & CMPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$CMPORT) 2>/dev/null && break; sleep 0.1; done
cagent() { # dir config-yaml: an agent dir with the mock providers and the given config
    mkdir -p "$1"
    cat > "$1/models.yml" <<YML
providers:
  mock:
    baseUrl: http://127.0.0.1:$PORT/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: mock-1
        contextWindow: 1000000
        maxTokens: 4096
  mockr:
    baseUrl: http://127.0.0.1:$CMPORT/v1
    apiKey: MOCK_KEY
    api: openai-responses
    models:
      - id: mock-r
        contextWindow: 100000
        maxTokens: 4096
        remoteCompaction:
          enabled: true
  mocka:
    baseUrl: http://127.0.0.1:$CMPORT
    apiKey: MOCK_KEY
    api: anthropic-messages
    models:
      - id: mock-a
        contextWindow: 100000
        maxTokens: 4096
        compat:
          supportsServerCompaction: true
          bedrockMessagesApi: true
YML
    printf '%s\n' "$2" > "$1/config.yml"
}
CR=(python3 -I "$ROOT/tests/e2e/rpc_compact.py" "$B")
BIG1=$(python3 -I -c 'print("say:" + " ".join("alpha%d beta%d gamma" % (i, i * 7) for i in range(4000)))')
BIG2=$(python3 -I -c 'print("say:" + " ".join("delta%d epsilon%d zeta" % (i, i * 3) for i in range(4000)))')
# threshold: the post-turn pass runs the soft summary (split turn: history + turn prefix + short summary)
cagent "$T/cg1" $'modelRoles:\n  default: mock/mock-1\ncompaction:\n  thresholdTokens: 75000\n  asyncEnabled: false\n  methodOrder: [soft]'
: > "$T/req.jsonl"
OMC_CODING_AGENT_DIR="$T/cg1" "${CR[@]}" "{\"type\":\"prompt\",\"message\":\"$BIG1\"}" "{\"type\":\"prompt\",\"message\":\"$BIG2\"}" > "$T/cg1.out"
OMC_CODING_AGENT_DIR="$T/cg1" python3 -I "$ROOT/tests/e2e/acp_compaction.py" "$B" "$BIG1" "$BIG2" > "$T/cgacp.out" 2>&1
check "acp compaction start"   "_omp/compaction/start True True ['action', 'reason', 'sessionId'] threshold context-full"  cat "$T/cgacp.out"
check "acp compaction end"     "_omp/compaction/end True True ['aborted', 'action', 'sessionId', 'tokensBefore', 'willRetry'] None context-full False False"  cat "$T/cgacp.out"
check "auto compaction start"  '"type": "auto_compaction_start", "reason": "threshold", "action": "context-full"'  cat "$T/cg1.out"
check "auto compaction result" '"type": "auto_compaction_end", "action": "context-full", "result": {"summary": "plain answer\n\n---\n\n**Turn Context (split turn):**\n\nplain answer", "shortSummary": "plain answer"'  cat "$T/cg1.out"
check "summary transcript"     $'<conversation>\n\n\nHuman: say:alpha0 beta0 gamma'  python3 -I -c '
import json, sys
for l in open(sys.argv[1]):
    r = json.loads(l)
    m = r.get("messages") or []
    if m and "Summarize user" in json.dumps(m[0]) and "alpha0" in json.dumps(m[1]):
        print(m[1]["content"][0]["text"][:80]); break' "$T/req.jsonl"
check "compaction entry"       "soft plain answer"  python3 -I -c '
import glob, json, sys
for f in glob.glob(sys.argv[1] + "/**/*.jsonl", recursive=True):
    for l in open(f):
        e = json.loads(l)
        if e.get("type") == "compaction": print(e["method"], e["shortSummary"], e["fromExtension"] is False)' "$T/cg1"
# the default ladder at the threshold: remote is unavailable, so auto-handoff writes the document (saved to the
# session artifacts with compaction.handoffSaveToDisk) and commits it as a handoff compaction
cagent "$T/cg5" $'modelRoles:\n  default: mock/mock-1\ncompaction:\n  thresholdTokens: 75000\n  asyncEnabled: false\n  handoffSaveToDisk: true'
OMC_CODING_AGENT_DIR="$T/cg5" "${CR[@]}" "{\"type\":\"prompt\",\"message\":\"$BIG1\"}" "{\"type\":\"prompt\",\"message\":\"$BIG2\"}" > "$T/cg5.out"
check "auto handoff"           '"type": "auto_compaction_end", "action": "handoff", "result": {"summary": "## Goal\nplain answer"'  cat "$T/cg5.out"
check "handoff saved"          "plain answer"  python3 -I -c '
import glob, sys
fs = glob.glob(sys.argv[1] + "/**/handoff-*.md", recursive=True)
print(open(fs[0]).read() if fs else "no handoff file")' "$T/cg5"
# a handoff document without its leading Goal heading never replaces the history: the ladder moves on (omp 421281302f)
cagent "$T/cg6" $'modelRoles:\n  default: mock/mock-1\ncompaction:\n  thresholdTokens: 75000\n  asyncEnabled: false'
OMC_CODING_AGENT_DIR="$T/cg6" "${CR[@]}" "{\"type\":\"prompt\",\"message\":\"BADHANDOFF $BIG1\"}" "{\"type\":\"prompt\",\"message\":\"$BIG2\"}" > "$T/cg6.out"
check "bad handoff skipped"    'auto_compaction_end'  bash -c 'grep -q "\"action\": \"handoff\", \"result\": {" "$1" && echo committed-handoff || grep -o auto_compaction_end "$1" | head -1' _ "$T/cg6.out"
# overflow: the ladder skips handoff, shake finds nothing, the soft summary commits and the turn retries
cagent "$T/cg2" $'modelRoles:\n  default: mock/mock-1\ncompaction:\n  asyncEnabled: false\n  keepRecentTokens: 10'
OMC_CODING_AGENT_DIR="$T/cg2" "${CR[@]}" '{"type":"prompt","message":"say:first answer here"}' \
    '{"type":"prompt","message":"fail:400:prompt is too long: 250000 tokens > 200000 maximum"}' > "$T/cg2.out"
check "overflow shake"         '"type": "auto_compaction_end", "action": "shake", "aborted": false, "willRetry": false, "skipped": true, "errorMessage": "Auto-shake found nothing eligible to drop; trying the next preferred compaction method."'  cat "$T/cg2.out"
check "overflow soft"          '"type": "auto_compaction_start", "reason": "overflow", "action": "context-full"'  cat "$T/cg2.out"
# OpenAI Responses remote compaction: /responses/compact with the native history, replayed next turn
cagent "$T/cg3" $'modelRoles:\n  default: mockr/mock-r\ncompaction:\n  keepRecentTokens: 10'
OMC_CODING_AGENT_DIR="$T/cg3" "${CR[@]}" '{"type":"prompt","message":"first question"}' '{"type":"prompt","message":"second question"}' \
    '{"type":"compact"}' '{"type":"prompt","message":"third question"}' > "$T/cg3.out"
check "openai compact request" "/v1/responses/compact message,message,message,message"  python3 -I -c '
import json, sys
for l in open(sys.argv[1]):
    r = json.loads(l)
    if r["path"].endswith("/responses/compact"): print(r["path"], ",".join(i.get("type", "?") for i in r["body"]["input"]))' "$T/cprov.jsonl"
check "openai compact replay"  "compaction ENC:4"  python3 -I -c '
import json, sys
rs = [json.loads(l) for l in open(sys.argv[1])]
rs = [r for r in rs if r["path"] == "/v1/responses"]
print(" ".join("%s %s" % (i.get("type"), i.get("encrypted_content")) for i in rs[-1]["body"]["input"] if i.get("type") == "compaction"))' "$T/cprov.jsonl"
check "openai compact entry"   "remote Remote compaction preserved provider-native history for this session. ENC:4"  python3 -I -c '
import glob, json, sys
for f in glob.glob(sys.argv[1] + "/**/*.jsonl", recursive=True):
    for l in open(f):
        e = json.loads(l)
        if e.get("type") == "compaction": print(e["method"], e["summary"], e["preserveData"]["openaiRemoteCompaction"]["compactionItem"]["encrypted_content"])' "$T/cg3"
# compaction spend is journaled per method: model_usage purpose compaction:<method> (omp 625d62739c)
check "compaction spend journal" "remote=1234/1000/56 soft=True handoff=True"  python3 -I -c '
import glob, json, sys
def usage(d):
    return [e for f in glob.glob(d + "/**/*.jsonl", recursive=True) for e in map(json.loads, open(f)) if e.get("type") == "model_usage"]
r = [e for e in usage(sys.argv[1]) if e["purpose"] == "compaction:remote"]
u = r[0]["usage"] if r else {}
print("remote=%s/%s/%s soft=%s handoff=%s" % (u.get("input", 0) + u.get("cacheRead", 0), u.get("cacheRead"), u.get("output"),
      any(e["purpose"] == "compaction:soft" for e in usage(sys.argv[2])), any(e["purpose"] == "compaction:handoff" for e in usage(sys.argv[3]))))' "$T/cg3" "$T/cg1" "$T/cg5"
check "openai compact strips metadata" "clean"  python3 -I -c '
import json, sys
rs = [json.loads(l) for l in open(sys.argv[1])]
rs = [r for r in rs if r["path"] == "/v1/responses"]
print("leak" if "internal_chat_message_metadata_passthrough" in json.dumps(rs[-1]["body"]["input"]) else "clean")' "$T/cprov.jsonl"
# a provider switch away from an OpenAI remote compaction (omp f470ea46f9, b899f57f13): the new model cannot replay the
# payload, so it reads the history behind it instead of the placeholder summary; switching back replays it again
cagent "$T/cg3s" $'modelRoles:\n  default: mockr/mock-r\ncompaction:\n  keepRecentTokens: 10'
CG3M=$(cat "$T/req.jsonl" 2>/dev/null | wc -l)
OMC_CODING_AGENT_DIR="$T/cg3s" "${CR[@]}" '{"type":"prompt","message":"first question"}' '{"type":"prompt","message":"second question"}' \
    '{"type":"compact"}' '{"type":"set_model","provider":"mock","modelId":"mock-1"}' '{"type":"prompt","message":"say:after the switch"}' \
    '{"type":"set_model","provider":"mockr","modelId":"mock-r"}' '{"type":"prompt","message":"back on responses"}' > "$T/cg3s.out"
check "remote compaction switch" "raw history=True placeholder=False back replays=True"  python3 -I -c '
import json, sys
rs = [json.loads(l) for l in open(sys.argv[1])][int(sys.argv[3]):]
r = [x for x in rs if "say:after the switch" in json.dumps(x.get("messages", [])[-1:])][-1]
body = json.dumps(r["messages"])
pv = [json.loads(l) for l in open(sys.argv[2])]
back = [x for x in pv if x["path"] == "/v1/responses" and "back on responses" in json.dumps(x["body"]["input"])][-1]
enc = any(i.get("type") == "compaction" for i in back["body"]["input"])
print("raw history=%s placeholder=%s back replays=%s" % ("first question" in body and "second question" in body,
      "Remote compaction preserved" in body, enc))' "$T/req.jsonl" "$T/cprov.jsonl" "$CG3M"
# Anthropic compaction beta: `compaction` request param + beta, the signed block replays at the head of the tail
: > "$T/cprov.jsonl"
cagent "$T/cg4" $'modelRoles:\n  default: mocka/mock-a\ncompaction:\n  keepRecentTokens: 10'
OMC_CODING_AGENT_DIR="$T/cg4" "${CR[@]}" '{"type":"prompt","message":"first question"}' '{"type":"prompt","message":"second question"}' \
    '{"type":"compact"}' '{"type":"prompt","message":"third question"}' > "$T/cg4.out"
check "anthropic compact request" "summarize True"  python3 -I -c '
import json, sys
for l in open(sys.argv[1]):
    r = json.loads(l)
    if "compaction" in r["body"]: print(r["body"]["compaction"]["type"], "compact-2026-09-04" in r["headers"].get("anthropic-beta", ""))' "$T/cprov.jsonl"
check "anthropic compact replay"  "assistant compaction SIG123"  python3 -I -c '
import json, sys
rs = [json.loads(l) for l in open(sys.argv[1])]
m = rs[-1]["body"]["messages"][0]
print(m["role"], m["content"][0].get("type"), m["content"][0].get("signature"))' "$T/cprov.jsonl"
kill $CMPID 2>/dev/null
