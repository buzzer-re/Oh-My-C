# Sourced by tests/e2e/run.sh: agent-core behaviour ported from omp 18.18 (sessions, skills, retries, task, titles).
AC="$T/acagent"; ACW="$T/acwork"
mkdir -p "$AC" "$ACW/.omc/skills"
cp "$T/agent/models.yml" "$AC/models.yml"
printf 'modelRoles:\n  default: mock/mock-1\nmemory:\n  backend: local\nautolearn:\n  enabled: true\n  autoContinue: false\n' > "$AC/config.yml"
ACENV=(env OMC_CODING_AGENT_DIR="$AC")
# ac_req.py <log> <mark> <code>: python over the main (tool-carrying) requests logged since line <mark>
cat > "$T/ac_req.py" <<'PY'
import json,sys
rs=[json.loads(l) for l in open(sys.argv[1])][int(sys.argv[2]):]
main=[r for r in rs if r.get("tools")]
def text(m):
    c=m.get("content"); return c if isinstance(c,str) else "".join(p.get("text","") for p in (c or []) if isinstance(p,dict))
exec(sys.argv[3])
PY
ac_mark() { cat "$T/req.jsonl" 2>/dev/null | wc -l; }
pushd "$ACW" >/dev/null
# skill notice (session-tools.ts takeSkillNotice): a skill manage_skill writes mid-session keeps the system prompt
# byte-stable and reaches the model as a hidden notice with the next prompt, once; a changed description is announced
# again under "Updated"; a skill created on disk mid-session is found by skill:// name (rediscoverForUnknownName)
M=$(ac_mark)
timeout 60 "${ACENV[@]}" python3 -I "$ROOT/tests/e2e/rpc_seq.py" "$B" --arg --model --arg mock/mock-1 \
    'call:manage_skill:{"action":"create","name":"sn-skill","description":"Use for notice tests","body":"Body one."}' \
    'say:after create' 'say:again' \
    'call:manage_skill:{"action":"update","name":"sn-skill","description":"Use for updated notice tests","body":"Body two."}' \
    'say:after update' \
    'bash:mkdir -p .omc/skills/late-skill && printf -- "---\nname: late-skill\ndescription: Late skill\n---\nLATE BODY\n" > .omc/skills/late-skill/SKILL.md' \
    'read:skill://late-skill' 'say:after late' </dev/null >/dev/null 2>&1
check "skill notice" "notice once=True stable=True updated=True late-read=True late-notice=True" python3 -I "$T/ac_req.py" "$T/req.jsonl" "$M" '
sysp=lambda r: text(r["messages"][0])
def notices(r): return [text(m) for m in r["messages"] if "Skills changed this session" in text(m)]
def at(word): return [r for r in main if word in text(r["messages"][-1])][0]
a, b, c, d, e = at("after create"), at("again"), at("after update"), at("LATE BODY"), at("after late")
once = len(notices(a)) == 1 and "Now available:\n- sn-skill: Use for notice tests" in notices(a)[0] and len(notices(b)) == 1
stable = len({sysp(r) for r in main}) == 1 and "sn-skill" not in sysp(a)
upd = len(notices(c)) == 2 and "Updated (this description replaces the earlier one):\n- sn-skill: Use for updated notice tests" in notices(c)[1]
late = any("late-skill: Late skill" in n for n in notices(e))
print("notice once=%s stable=%s updated=%s late-read=%s late-notice=%s" % (once, stable, upd, "LATE BODY" in text(d["messages"][-1]), late))'
popd >/dev/null
