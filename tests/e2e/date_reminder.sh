# Sourced by tests/e2e/run.sh: date/cwd reminders are journaled per conversation (omp session/date-cwd-reminder.ts). A
# resumed session resends its turns with the reminders they were sent with; only a new turn takes today's date.
DR_WORK="$T/drwork"
mkdir -p "$DR_WORK"
DR_SESSIONS="$T/drsessions"
mkdir -p "$DR_SESSIONS"
cat > "$T/dr_check.py" <<'PY'
import json, re, sys, glob, os
mode, sess_dir, log = sys.argv[1:4]
files = sorted(glob.glob(os.path.join(sess_dir, "**", "*.jsonl"), recursive=True), key=os.path.getmtime)
path = files[-1]
entries = [json.loads(l) for l in open(path)]
marks = [e for e in entries if e.get("type") == "custom" and e.get("customType") == "date-cwd-reminder"]
if mode == "journal":
    d = marks[0]["data"] if marks else {}
    print("journal", len(marks), list(d.keys()), d.get("placement"), d.get("conversation") == d.get("turn"))
elif mode == "tamper":
    # the first turn was sent years ago, and the model was told again after the reply
    old = "Today: 2020-01-02;"
    out = []
    for e in entries:
        if e in marks:
            e["data"]["reminder"] = re.sub(r"Today: \d{4}-\d\d-\d\d;", old, e["data"]["reminder"])
        out.append(e)
    last = out[-1]
    asst = [e for e in out if e.get("type") == "message" and e["message"]["role"] == "assistant"][-1]
    control = {"type": "custom", "customType": "date-cwd-reminder", "id": "drctl001", "parentId": last["id"],
               "timestamp": last["timestamp"],
               "data": {"conversation": marks[0]["data"]["conversation"], "turn": "assistant:%d" % asst["message"]["timestamp"],
                        "reminder": re.sub(r"Today: \d{4}-\d\d-\d\d;", "Today: 2020-01-03;", marks[0]["data"]["reminder"]),
                        "placement": "control"}}
    out.append(control)
    with open(path, "w") as f:
        for e in out:
            f.write(json.dumps(e, ensure_ascii=False) + "\n")
elif mode == "wire":
    req = [json.loads(l) for l in open(log)][-1]
    raw = json.dumps(req)
    msgs = [m for m in req["messages"] if m["role"] != "system"]
    def text(m):
        c = m.get("content")
        return c if isinstance(c, str) else "".join(p.get("text", "") for p in c)
    ts = [text(m) for m in msgs]
    first = "2020-01-02" in ts[0]
    today = [i for i, t in enumerate(ts) if "Today: " in t and "2020-" not in t]
    ctl = [i for i, t in enumerate(ts) if "Today: 2020-01-03" in t]
    print("wire first-kept=%s control-role=%s today-on-last=%s leak=%s" % (
        first, msgs[ctl[0]]["role"] if ctl else None, today == [len(ts) - 1], "_dcrControl" in raw or "synthetic" in raw))
PY
(cd "$DR_WORK" && timeout 30 "$B" --model mock/mock-1 --session-dir "$DR_SESSIONS" -p "say:hello" </dev/null >/dev/null 2>&1)
check "reminder journaled" "journal 1 ['conversation', 'turn', 'reminder', 'placement', 'firstTurn'] prefix True" \
    python3 -I "$T/dr_check.py" journal "$DR_SESSIONS" "$T/req.jsonl"
python3 -I "$T/dr_check.py" tamper "$DR_SESSIONS" "$T/req.jsonl"
(cd "$DR_WORK" && timeout 30 "$B" --model mock/mock-1 --session-dir "$DR_SESSIONS" -c -p "say:again" </dev/null >/dev/null 2>&1)
# mock-1 has no reasoning, so a developer turn reaches this API as a user message of its own
check "resume keeps the sent dates" "wire first-kept=True control-role=user today-on-last=True leak=False" \
    python3 -I "$T/dr_check.py" wire "$DR_SESSIONS" "$T/req.jsonl"
