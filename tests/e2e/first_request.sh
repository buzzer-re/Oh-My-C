# Sourced by tests/e2e/run.sh: the first model request of a fresh session against a local Qwen-style server, the
# setup of tests/parity/first_request_timing.py (a models.yml openai-completions provider on 127.0.0.1 with
# `reasoning: true`, image input and a thinking minLevel..maxLevel range, also the smol role; a git project with
# AGENTS.md and two skills). The mock serves one request at a time (MOCK_SLOT_SECS) and makes a request that would
# think (no `enable_thinking: false`) slower (MOCK_THINK_SECS). Pins what omp v18.14.2 sends: the detected Qwen
# compat (enable_thinking / preserve_thinking, no store, no reasoning_effort), skill-description compression with
# thinking off and plain-string content, the request order, and the first request's size (omc must never send
# more than omp: FR_OMP_BYTES is omp's size for this exact setup, measured with first_request_check.py size; omp
# 18.18.7 on Linux arm64).
FRPORT=$((PORT + 31))
FR_OMP_BYTES=31908
mkdir -p "$T/fragent" "$T/frhome" "$T/frwork/.omp/skills/deploy" "$T/frwork/.omp/skills/db-migrate"
cat > "$T/fragent/models.yml" <<YML
providers:
  splash:
    baseUrl: http://127.0.0.1:$FRPORT/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: qwen3.6-27b
        name: Qwen 3.6 27B
        reasoning: true
        input: [text, image]
        contextWindow: 131072
        maxTokens: 32768
        thinking:
          mode: effort
          minLevel: low
          maxLevel: high
YML
printf 'modelRoles:\n  default: splash/qwen3.6-27b\n  smol: splash/qwen3.6-27b\n' > "$T/fragent/config.yml"
git -C "$T/frwork" init -q 2>/dev/null
printf '# Project rules\n- Build with `make -j4`; run `make test` before committing.\n' > "$T/frwork/AGENTS.md"
printf -- '---\nname: deploy\ndescription: Deploy the service to staging or production using the release scripts, including canary checks, rollback steps and the on-call notification flow. Use when the user asks to ship, release or roll back.\n---\n# deploy\nSteps.\n' > "$T/frwork/.omp/skills/deploy/SKILL.md"
printf -- "---\nname: db-migrate\ndescription: Write and apply PostgreSQL schema migrations with the project's migration tool; covers naming, reversible steps and data backfills.\n---\n# db-migrate\nSteps.\n" > "$T/frwork/.omp/skills/db-migrate/SKILL.md"
MOCK_SLOT_SECS=0.2 MOCK_THINK_SECS=1.5 MOCK_TIMELOG="$T/fr-time.jsonl" MOCK_LOG="$T/fr.jsonl" \
    python3 -I "$ROOT/tests/e2e/mock_openai.py" $FRPORT & FRPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$FRPORT) 2>/dev/null && break; sleep 0.1; done
FRC=(python3 -I "$ROOT/tests/e2e/first_request_check.py" "$T/fr.jsonl" "$T/fr-time.jsonl" "$T/frwork")
check "first request run"       "hello there"  bash -c "cd '$T/frwork' && HOME='$T/frhome' OMC_CODING_AGENT_DIR='$T/fragent' '$B' -p 'say:hello there'"
check "first request params"    '{"chat_template_kwargs": {"preserve_thinking": true}, "enable_thinking": true, "max_completion_tokens": 32768, "model": "qwen3.6-27b", "preserve_thinking": true, "stream": true, "stream_options": {"include_usage": true}}'  "${FRC[@]}" params
check "first request side"      $'compress think=False content=str\ncompress think=False content=str'  "${FRC[@]}" side
check "first request order"     "main; 2 compress"  "${FRC[@]}" order
# compressions that thought would hold the slot for 2 x (0.2 + 1.5) s before the main request
check "first request queue"     "main queued <= 0.5s"  "${FRC[@]}" queue
check "first request size"      "within omp"  bash -c "\"\$@\" size | python3 -I -c 'import sys; s = sys.stdin.read(); n = int(s.split()[2]); print(\"within omp\" if s.startswith(\"11 tools\") and n <= $FR_OMP_BYTES else s)'" _ "${FRC[@]}"
kill $FRPID 2>/dev/null
