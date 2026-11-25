# Sourced by tests/e2e/run.sh: `edit.mode: magic` (omp edit/magic.ts, v19.1.1 draft loop). The mock plays both models:
# the main model sends the `# path` payload, the apply model (smol role or edit.magic.model) plays the change's
# `TURNS: [...]` script, one entry per turn: native read/search/replace calls, text (`DONE`, `CANNOT: …`, written calls).
MG="$T/magic"
mkdir -p "$MG/agent" "$MG/work"
cat > "$MG/agent/models.yml" <<YML
providers:
  mock:
    baseUrl: http://127.0.0.1:$PORT/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: mock-1
        contextWindow: 100000
        maxTokens: 4096
      - id: mock-apply
        contextWindow: 100000
        maxTokens: 4096
        reasoning: false
      - id: mock-pick
        contextWindow: 100000
        maxTokens: 4096
        reasoning: false
YML
printf 'edit:\n  mode: magic\nmodelRoles:\n  smol: mock/mock-apply\ncomposer:\n  predictions: false\n' > "$MG/agent/config.yml"
mg_src() { printf 'function total(items) {\n\tlet sum = 0;\n\tfor (const item of items) {\n\t\tsum += item;\n\t}\n\treturn sum;\n}\n' > "$MG/work/$1"; }
# check() runs a command (not a function) under its timeout: the env prefix runs omc in the magic work dir
MGE=(env -C "$MG/work" OMC_CODING_AGENT_DIR="$MG/agent" "$B" --model mock/mock-1)
mg() { timeout 60 "${MGE[@]}" "$@" </dev/null >/dev/null 2>&1; }
# the apply requests of the last run (the conversation of each turn), or the edit tool of its main request
MGQ='import json,sys
rs=[json.loads(l) for l in open(sys.argv[1])]
txt=lambda m: m["content"] if isinstance(m["content"],str) else "".join(b.get("text","") for b in m["content"])
isap=lambda r: txt(r["messages"][0]).startswith("Apply every requested change to the draft of this one file.")
i=max(k for k,r in enumerate(rs) if r.get("tools") and len(r["messages"])==2 and not isap(r))
run=rs[i:]
ap=[r for r in run if isap(r)]
if sys.argv[2]=="tool":
    e=[t["function"] for t in rs[i]["tools"] if t["function"]["name"]=="edit"][0]
    print(e["description"].splitlines()[0], json.dumps(e["parameters"]))
elif sys.argv[2]=="apply":
    r=ap[0]; print(len(ap), r["model"], r.get("max_completion_tokens"), [t["function"]["name"] for t in r["tools"]], txt(r["messages"][1]).splitlines()[0])
    print(txt(r["messages"][1]).split("<file>\n",1)[1].split("\n</file>")[0])
elif sys.argv[2]=="results":
    print("\n".join(txt(m) for m in ap[-1]["messages"] if m["role"]=="tool"))
elif sys.argv[2]=="feedback":
    print(txt([m for m in ap[-2]["messages"] if m["role"]=="user"][-1]))
else:
    print(" ".join(r["model"] for r in ap))'
MGQC=(python3 -I -c "$MGQ" "$T/req.jsonl")

mg_src mg.ts
check "magic apply"     "Tool said: [mg.ts]"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- Start the sum at one. TURNS: [[[\"replace\",{\"old_string\":\"let sum = 0;\",\"new_string\":\"let sum = 1;\"}]],\"DONE\"]"}'
check "magic result"    $'\tlet sum = 1;\n'  cat "$MG/work/mg.ts"
check "magic tool"      'Edit by description. {"type": "object", "properties": {"i": {"type": "string", "description": "concise intent"}, "input": {"type": "string", "description": "One section per file: the file, then the changes to it in words."}}, "required": ["input", "i"], "additionalProperties": false}'  "${MGQC[@]}" tool
# max_completion_tokens: the apply model's limit capped at 16384; the tools are the draft's read/search/replace
check "magic request"   $'2 mock-apply 4096 [\'read\', \'search\', \'replace\'] File `mg.ts` (7 lines). In `N\xe2\x94\x82text`, only the text after `\xe2\x94\x82` is part of the file.\n1\xe2\x94\x82function total(items) {\n2\xe2\x94\x82\tlet sum = 0;'  "${MGQC[@]}" apply
check "magic replaced"  $'Replaced text on line 2.\nDraft near the edit:\n1\xe2\x94\x82function total(items) {\n2\xe2\x94\x82\tlet sum = 1;\n3\xe2\x94\x82\tfor (const item of items) {'  "${MGQC[@]}" results
# a failed replace stays with the model, successful ones stay in the draft; a repeated failure is reported as such
mg_src mg.ts
check "magic retry"     "Tool said: [mg.ts]"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- Rename. TURNS: [[[\"replace\",{\"old_string\":\"return sum;\",\"new_string\":\"return sum * 2;\"}],[\"replace\",{\"old_string\":\"let  sum = 0;\",\"new_string\":\"let sum = 2;\"}]],\"DONE\",[[\"replace\",{\"old_string\":\"let sum = 0;\",\"new_string\":\"let sum = 2;\"}]],\"DONE\"]"}'
check "magic retry log" $'old_string not found. Whitespace differs. Copy the exact text after \xe2\x94\x82:\n2\xe2\x94\x82\tlet sum = 0;'  "${MGQC[@]}" results
check "magic retry out" $'\tlet sum = 2;\n\tfor (const item of items) {\n\t\tsum += item;\n\t}\n\treturn sum * 2;'  cat "$MG/work/mg.ts"
check "magic feedback"  "At least one replacement failed. Correct the failed change in the draft before replying \`DONE\`."  "${MGQC[@]}" feedback
# written calls (a fenced JSON block, as models without native tool calls send them) run like native ones
mg_src mg.ts
check "magic written"   "Tool said: [mg.ts]"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- Return double. TURNS: [\"```json\\n{\\\"name\\\":\\\"replace\\\",\\\"arguments\\\":{\\\"old_string\\\":\\\"return sum;\\\",\\\"new_string\\\":\\\"return sum + sum;\\\"}}\\n```\",\"DONE\"]"}'
check "magic written out" $'\treturn sum + sum;\n}'  cat "$MG/work/mg.ts"
# a JSON draft that stops parsing is sent back with the replace that broke it; the repair is written
printf '{\n  "name": "demo",\n  "version": "1.0.0"\n}\n' > "$MG/work/pkg.json"
check "magic syntax"    "Tool said: [pkg.json]"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# pkg.json\n- Add a private flag. TURNS: [[[\"replace\",{\"old_string\":\"\\\"version\\\": \\\"1.0.0\\\"\",\"new_string\":\"\\\"version\\\": \\\"1.0.0\\\"\\n  \\\"private\\\": true\"}]],\"DONE\",[[\"replace\",{\"old_string\":\"\\\"1.0.0\\\"\\n\",\"new_string\":\"\\\"1.0.0\\\",\\n\"}]],\"DONE\"]"}'
check "magic syntax fb" $'The draft no longer parses. The draft first stopped parsing after replace #1 (then at line 3). Current lines near that location:\n1\xe2\x94\x82{\n2\xe2\x94\x82  "name": "demo",\n3\xe2\x94\x82  "version": "1.0.0"\n4\xe2\x94\x82  "private": true\n5\xe2\x94\x82}\nUse `read` and `replace` to repair the draft; successful edits are still in it.'  "${MGQC[@]}" feedback
check "magic syntax out" $'  "version": "1.0.0",\n  "private": true\n}'  cat "$MG/work/pkg.json"
check "magic refusal"   "Tool said: No change was made to mg.ts: nothing named"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- Remove `drain`."}'
check "magic missing"   "Tool said: nope.ts does not exist; this edit changes existing files."  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# nope.ts\n- Delete line 1. TURNS: [\"DONE\"]"}'
check "magic payload"   "Tool said: Every change belongs to a file: put it under that file's \`# path\` line."  "${MGE[@]}" --no-session -p 'call:edit:{"input":"- stray\n# mg.ts\n- x"}'
# replying DONE without a change twice ends the section with the repeated feedback
mg_src mg.ts
check "magic unfinished" "Tool said: The change to mg.ts did not finish; the file is unchanged. Apply the requested changes with \`replace\` before replying \`DONE\`. Use \`read\` or \`search\` if needed."  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- Nothing. TURNS: [\"DONE\"]"}'
mg_src a.ts; mg_src b.ts
check "magic batch"     "[b.ts] not applied: No change was made to b.ts: nothing named"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# a.ts\n- Start at three. TURNS: [[[\"replace\",{\"old_string\":\"= 0\",\"new_string\":\"= 3\"}]],\"DONE\"]\n# b.ts\n- Remove `drain`."}'
check "magic batch out" $'\tlet sum = 3;\n'  cat "$MG/work/a.ts"
printf 'edit:\n  mode: magic\n  magic:\n    model: mock/mock-pick\ncomposer:\n  predictions: false\n' > "$MG/agent/config.yml"
mg_src mg.ts
check "magic selector"  "Tool said: [mg.ts]"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- Start at four. TURNS: [[[\"replace\",{\"old_string\":\"= 0\",\"new_string\":\"= 4\"}]],\"DONE\"]"}'
check "magic sel model" "mock-pick mock-pick"  "${MGQC[@]}" models
printf 'edit:\n  mode: magic\n  magic:\n    model: nosuch-model\ncomposer:\n  predictions: false\n' > "$MG/agent/config.yml"
check "magic sel none"  "Tool said: \`edit.magic.model\` (nosuch-model) matches no available model."  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- x"}'
printf 'edit:\n  mode: magic\ncomposer:\n  predictions: false\n' > "$MG/agent/config.yml"
check "magic no model"  "Tool said: \`edit.mode: magic\` needs a model to apply the change: configure the \`smol\` role or \`edit.magic.model\`."  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- x"}'
# the apply model's completions are journaled as off-transcript model_usage entries (purpose magic-edit)
printf 'edit:\n  mode: magic\nmodelRoles:\n  smol: mock/mock-apply\ncomposer:\n  predictions: false\n' > "$MG/agent/config.yml"
mg_src mg.ts
mg -p 'call:edit:{"input":"# mg.ts\n- Start at five. TURNS: [[[\"replace\",{\"old_string\":\"= 0\",\"new_string\":\"= 5\"}]],\"DONE\"]"}'
check "magic usage"     '"purpose": "magic-edit"'  bash -c "grep -rhoE '\"purpose\": ?\"magic-edit\"' '$MG/agent/sessions' | head -1 | sed 's/\"purpose\":\"/\"purpose\": \"/'"
