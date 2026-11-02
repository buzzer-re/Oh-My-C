#!/usr/bin/env python3
"""stall_reqs.py <mock log> <prompt>: summarize the model requests that carried <prompt> (tests/e2e/stall.sh):
"n=<count> last=<roles of the last request> reminders=<stream-stall reminder attempts in it>"."""
import json, sys

rs = []
for line in open(sys.argv[1]):
    r = json.loads(line)
    r = r.get("body", r)
    if r.get("tools") and sys.argv[2] in json.dumps(r.get("messages")):
        rs.append(r["messages"])


def txt(m):
    c = m.get("content")
    return c if isinstance(c, str) else "".join(x.get("text", "") for x in c or [] if isinstance(x, dict))


last = [m for m in rs[-1] if m.get("role") not in ("system", "developer")] if rs else []
marks = "".join(t[t.index("Attempt #"):t.index("Attempt #") + 12] for t in map(txt, last) if "Attempt #" in t)
print("n=%d last=%s reminders=%s" % (len(rs), ",".join(m["role"] for m in last), marks))
