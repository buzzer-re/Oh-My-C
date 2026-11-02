#!/usr/bin/env python3
"""Turn-lifecycle signals of an interactive run captured with `script` (tests/e2e/run.sh).
usage: signals_check.py <typescript> <kind>
Extracts the title (OSC 0), title stack (CSI 22/23;2t), OSC 7501 and notification (BEL / OSC 9 / tmux passthrough)
sequences, collapses consecutive working-spinner titles to one `π * <label>` row, and compares them with the
sequence omp v18.14.2 writes for the same scenario. Prints "signals ok" or a diff."""
import re, sys

data = open(sys.argv[1], "rb").read()
kind = sys.argv[2]
pat = re.compile(rb"\x1b\][0-9]*;.*?(?:\x07|\x1b\\)|\x1b\[2[23];2t|\x1bPtmux;.*?\x1b\x1b\\\x1b\\|\x07", re.S)
SPIN = re.compile("[⠀-⣿]")
got = []
for m in pat.finditer(data):
    s = m.group(0).decode("utf-8", "replace")
    if s.startswith("\x1b]") and not re.match(r"\x1b\]((0|7501|9|99);)", s):
        continue  # other OSCs (hyperlinks, colors, clipboard): their BEL terminator is no notification
    if s.startswith("\x1b]0;"):
        s = SPIN.sub("*", s)
    if got and got[-1] == s and "*" in s:
        continue
    got.append(s)

T = lambda t: "\x1b]0;" + t + "\x07"
R = lambda r: "\x1b]7501;" + r + "\x1b\\"
want = {
    # a plain turn on a BEL terminal: push, claim, idle, label, working spinner, done, completion BEL, teardown
    "turn": ["\x1b[22;2t", T("π >"), R("state=idle:app=omc"), T("π > work"), T("π * work"), R("state=working:app=omc"),
             T("π > work"), R("state=done:app=omc"), "\x07", R("state=clear"), T(""), "\x1b[23;2t"],
    # an OSC 9 terminal (WezTerm) failing with error.notify on, then a completed turn
    "error": ["\x1b[22;2t", T("π >"), R("state=idle:app=omc"), T("π > work"), T("π * work"), R("state=working:app=omc"),
              T("π > work"), R("state=error:app=omc:msg=NDAwIGJhZCB0aGluZ3M="), "\x1b]9;omc: Stopped with error\x1b\\",
              R("state=clear"), T(""), "\x1b[23;2t"],
}[kind]
if got == want:
    print("signals ok")
else:
    print("signals mismatch")
    for i in range(max(len(got), len(want))):
        g = got[i] if i < len(got) else None
        w = want[i] if i < len(want) else None
        print(("   " if g == w else "!! ") + repr(g) + "  want " + repr(w))
