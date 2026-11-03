# Sourced by tests/e2e/run.sh: WP-26 settings wired to existing behaviour (fetch.enabled, sampling, skill sources,
# autoResume) against mock_openai.py, each with its own throwaway agent dir. omp references: tools/read.ts (URL reads),
# session/settings.ts cfgSampling, extensibility/skills.ts loadSkills, main.ts createSessionManager.
mkdir -p "$T/sagent/skills/alpha-skill" "$T/scustom/beta-skill" "$T/swork"
cp "$T/agent/models.yml" "$T/sagent/models.yml"
printf -- '---\nname: alpha-skill\ndescription: Alpha skill for settings e2e\n---\nbody\n' > "$T/sagent/skills/alpha-skill/SKILL.md"
printf -- '---\nname: beta-skill\ndescription: Beta skill from a custom directory\n---\nbody\n' > "$T/scustom/beta-skill/SKILL.md"
SENV=(env OMC_CODING_AGENT_DIR="$T/sagent")
scfg() { printf '%b' "$1" > "$T/sagent/config.yml"; }
# last logged request: the named fields, or whether the system prompt mentions a string
SREQ=(python3 -I -c 'import json, sys
r = [json.loads(l) for l in open(sys.argv[1])][-1]
if sys.argv[2] == "fields": print(json.dumps({k: r.get(k) for k in sys.argv[3:]}))
else: print("has" if sys.argv[3] in json.dumps(r["messages"][0]) else "missing")' "$T/req.jsonl")
cd "$T/swork"
scfg 'fetch:\n  enabled: false\n'
check "fetch disabled"     "Tool said: URL reads are disabled by settings."  "${SENV[@]}" "$B" --model mock/mock-1 --no-session -p "read:https://example.invalid/page"
scfg 'temperature: -1\ntopP: 0.25\n'
check "sampling topP"      '{"temperature": null, "top_p": 0.25}'  bash -c "${SENV[*]} '$B' --model mock/mock-1 --no-session -p hi >/dev/null 2>&1; ${SREQ[*]@Q} fields temperature top_p"
scfg ''
check "skills default"     "has"      bash -c "${SENV[*]} '$B' --model mock/mock-1 --no-session -p hi >/dev/null 2>&1; ${SREQ[*]@Q} prompt alpha-skill"
scfg 'skills:\n  enablePiUser: false\n'
check "skills pi user off" "missing"  bash -c "${SENV[*]} '$B' --model mock/mock-1 --no-session -p hi >/dev/null 2>&1; ${SREQ[*]@Q} prompt alpha-skill"
scfg "skills:\n  customDirectories: [\"$T/scustom\"]\n"
check "skills custom dir"  "has"      bash -c "${SENV[*]} '$B' --model mock/mock-1 --no-session -p hi >/dev/null 2>&1; ${SREQ[*]@Q} prompt beta-skill"
scfg "skills:\n  enabled: false\n  customDirectories: [\"$T/scustom\"]\n"
check "skills disabled"    "missing"  bash -c "${SENV[*]} '$B' --model mock/mock-1 --no-session -p hi >/dev/null 2>&1; ${SREQ[*]@Q} prompt alpha-skill"
# autoResume: a later plain run continues the newest session of this cwd (its messages are replayed)
scfg ''
"${SENV[@]}" "$B" --model mock/mock-1 -p "bash:echo first-run-marker" >/dev/null 2>&1 </dev/null
scfg 'autoResume: true\n'
check "autoResume"         "first-run-marker"  bash -c "${SENV[*]} '$B' --model mock/mock-1 -p hi >/dev/null 2>&1 </dev/null; python3 -I -c 'import json,sys; print(json.dumps([json.loads(l) for l in open(sys.argv[1])][-1][\"messages\"]))' '$T/req.jsonl'"
check "autoResume no-session" "missing" bash -c "${SENV[*]} '$B' --model mock/mock-1 --no-session -p hi >/dev/null 2>&1 </dev/null; python3 -I -c 'import json,sys; print(\"has\" if \"first-run-marker\" in json.dumps([json.loads(l) for l in open(sys.argv[1])][-1][\"messages\"]) else \"missing\")' '$T/req.jsonl'"
rm -f "$T/sagent/config.yml"
cd "$T/work"
