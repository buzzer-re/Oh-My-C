# Sourced by tests/e2e/run.sh: `edit.mode: magic` (omp edit/magic.ts). The mock plays both models: the main model sends
# the `# path` payload, the apply model (smol role or edit.magic.model) answers the change's `OPS:` line operations.
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
# the apply requests of the last run, or the edit tool of its main request
MGQ='import json,sys
rs=[json.loads(l) for l in open(sys.argv[1])]
i=max(k for k,r in enumerate(rs) if r.get("tools") and len(r["messages"])==2)
run=rs[i:]
txt=lambda m: m["content"] if isinstance(m["content"],str) else "".join(b.get("text","") for b in m["content"])
ap=[r for r in run if txt(r["messages"][0]).startswith("You turn a described change into line operations")]
if sys.argv[2]=="tool":
    e=[t["function"] for t in rs[i]["tools"] if t["function"]["name"]=="edit"][0]
    print(e["description"].splitlines()[0], json.dumps(e["parameters"]))
elif sys.argv[2]=="apply":
    r=ap[0]; print(len(ap), r["model"], r.get("max_completion_tokens"), txt(r["messages"][1]).splitlines()[0])
    print(txt(r["messages"][1]).split("<file>\n",1)[1].split("\n</file>")[0])
elif sys.argv[2]=="retry":
    print(len(ap), txt(ap[-1]["messages"][1]).split("<rejection>\n",1)[-1].split("\n")[0])
else:
    print(" ".join(r["model"] for r in ap))'
MGQC=(python3 -I -c "$MGQ" "$T/req.jsonl")

mg_src mg.ts
check "magic apply"     "Tool said: [mg.ts]"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- Start the sum at one. OPS: [\"sub\",2,\"= 0\",\"= 1\"]"}'
check "magic result"    $'\tlet sum = 1;\n'  cat "$MG/work/mg.ts"
check "magic tool"      'Edit by description. {"type": "object", "properties": {"i": {"type": "string", "description": "concise intent"}, "input": {"type": "string", "description": "One section per file: the file, then the changes to it in words."}}, "required": ["input", "i"], "additionalProperties": false}'  "${MGQC[@]}" tool
# max_completion_tokens: a non-reasoning apply model gets ceil((shown file + change) * 0.5) + 256 = ceil(167 * 0.5) + 256
check "magic request"   $'1 mock-apply 340 File `mg.ts`. Each line is shown as its number, a tab, then its text; the number and the tab are not part of the file. A line that opens a block shows the block\'s first and last line instead of its number: `20-49` opens a block that ends on line 49.\n1-7\tfunction total(items) {\n2\t\tlet sum = 0;\n3-5\t\tfor (const item of items) {'  "${MGQC[@]}" apply
mg_src mg.ts
check "magic retry"     "Tool said: [mg.ts]"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- Rename. OPS: [\"sub\",3,\"sum\",\"total\"] || [\"sub\",2,\"sum = 0\",\"sum = 2\"]"}'
check "magic retry log" '2 Operation 1 (["sub",3,"sum","total"]): line 3 does not contain "sum"; the nearest line that does is 2 or 4.'  "${MGQC[@]}" retry
check "magic retry out" $'\tlet sum = 2;\n'  cat "$MG/work/mg.ts"
mg_src mg.ts
mg --no-session -p 'call:edit:{"input":"# mg.ts\n- Drop the loop. OPS: [\"delete\",3,4] || [\"delete\",3,5]"}'
check "magic block"     '2 Nothing was applied: line 3 opens a block that ends on line 5, but the range ends on line 4 (`sum += item;`). Correct the range, or send the same operations again if that is intended.'  "${MGQC[@]}" retry
check "magic block out" $'function total(items) {\n\tlet sum = 0;\n\treturn sum;\n}'  cat "$MG/work/mg.ts"
check "magic refusal"   "Tool said: No change was made to mg.ts: nothing named"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- Remove `drain`."}'
check "magic missing"   "Tool said: nope.ts does not exist; this edit changes existing files."  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# nope.ts\n- Delete line 1. OPS: [\"delete\",1,1]"}'
check "magic payload"   "Tool said: Every change belongs to a file: put it under that file's \`# path\` line."  "${MGE[@]}" --no-session -p 'call:edit:{"input":"- stray\n# mg.ts\n- x"}'
mg_src a.ts; mg_src b.ts
check "magic batch"     "[b.ts] not applied: No change was made to b.ts: nothing named"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# a.ts\n- Start at three. OPS: [\"sub\",2,\"= 0\",\"= 3\"]\n# b.ts\n- Remove `drain`."}'
check "magic batch out" $'\tlet sum = 3;\n'  cat "$MG/work/a.ts"
printf 'edit:\n  mode: magic\n  magic:\n    model: mock/mock-pick\ncomposer:\n  predictions: false\n' > "$MG/agent/config.yml"
mg_src mg.ts
check "magic selector"  "Tool said: [mg.ts]"  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- Start at four. OPS: [\"sub\",2,\"= 0\",\"= 4\"]"}'
check "magic sel model" "mock-pick"  "${MGQC[@]}" models
printf 'edit:\n  mode: magic\n  magic:\n    model: nosuch-model\ncomposer:\n  predictions: false\n' > "$MG/agent/config.yml"
check "magic sel none"  "Tool said: \`edit.magic.model\` (nosuch-model) matches no available model."  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- x"}'
printf 'edit:\n  mode: magic\ncomposer:\n  predictions: false\n' > "$MG/agent/config.yml"
check "magic no model"  "Tool said: \`edit.mode: magic\` needs a model to apply the change: configure the \`smol\` role or \`edit.magic.model\`."  "${MGE[@]}" --no-session -p 'call:edit:{"input":"# mg.ts\n- x"}'
# the apply model's completions are journaled as off-transcript model_usage entries (purpose magic-edit)
printf 'edit:\n  mode: magic\nmodelRoles:\n  smol: mock/mock-apply\ncomposer:\n  predictions: false\n' > "$MG/agent/config.yml"
mg_src mg.ts
mg -p 'call:edit:{"input":"# mg.ts\n- Start at five. OPS: [\"sub\",2,\"= 0\",\"= 5\"]"}'
check "magic usage"     '"purpose": "magic-edit"'  bash -c "grep -rhoE '\"purpose\": ?\"magic-edit\"' '$MG/agent/sessions' | head -1 | sed 's/\"purpose\":\"/\"purpose\": \"/'"
