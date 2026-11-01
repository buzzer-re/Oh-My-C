#!/usr/bin/env python3
"""Dev-only: cell-level diff of two `tmux capture-pane -e` captures (glyph, fg, bg, attributes).

  celldiff.py <omp.ansi> <omc.ansi> [anchor]

Both captures are aligned at the first row containing `anchor` (if given) and cut before the status
line; differing rows are printed with the first differing column."""
import re, sys, unicodedata

def cells(path):
    rows = []
    st = {"fg": "", "bg": "", "a": 0}
    for line in open(path, encoding="utf-8", errors="replace").read().split("\n"):
        out = []
        i = 0
        while i < len(line):
            ch = line[i]
            if ch == "\x1b":
                m = re.match(r"\x1b\[([0-9;:]*)m", line[i:])
                if m:
                    ps = [int(x) if x else 0 for x in re.split("[;:]", m.group(1))] if m.group(1) else [0]
                    j = 0
                    while j < len(ps):
                        c = ps[j]
                        if c == 0: st = {"fg": "", "bg": "", "a": 0}
                        elif c in (1, 2, 3, 4, 7, 9): st["a"] |= {1: 1, 2: 2, 3: 4, 4: 8, 7: 16, 9: 32}[c]
                        elif c == 22: st["a"] &= ~3
                        elif c == 23: st["a"] &= ~4
                        elif c == 24: st["a"] &= ~8
                        elif c == 27: st["a"] &= ~16
                        elif c == 29: st["a"] &= ~32
                        elif c == 39: st["fg"] = ""
                        elif c == 49: st["bg"] = ""
                        elif c in (38, 48):
                            if j + 1 < len(ps) and ps[j + 1] == 2:
                                v = "%d,%d,%d" % tuple(ps[j + 2:j + 5]); j += 4
                            elif j + 1 < len(ps) and ps[j + 1] == 5:
                                v = "p%d" % ps[j + 2]; j += 2
                            else: v = "?"
                            st["fg" if c == 38 else "bg"] = v
                        elif 30 <= c <= 37 or 90 <= c <= 97: st["fg"] = "b%d" % c
                        elif 40 <= c <= 47 or 100 <= c <= 107: st["bg"] = "b%d" % c
                        j += 1
                    i += len(m.group(0)); continue
                m = re.match(r"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", line[i:])
                if m: i += len(m.group(0)); continue
                i += 1; continue
            if unicodedata.combining(ch) or ch in "️‍":
                if out: out[-1] = (out[-1][0] + ch,) + out[-1][1:]
                i += 1; continue
            fg = st["fg"] if ch != " " else ""
            a = st["a"] if ch != " " else st["a"] & 24
            out.append((ch, fg, st["bg"], a))
            i += 1
        while out and out[-1][0] == " " and not out[-1][2] and not (out[-1][3] & 24): out.pop()
        rows.append(out)
    return rows

def text(r): return "".join(c[0] for c in r)

a, b = cells(sys.argv[1]), cells(sys.argv[2])
anchor = sys.argv[3] if len(sys.argv) > 3 else None
def start(rows):
    if anchor:
        for i, r in enumerate(rows):
            if anchor in text(r): return i
    return 0
def body(rows):
    for i, r in enumerate(rows):
        if " π > " in text(r) or "Mock One >" in text(r): return rows[:i]
    return rows
a, b = body(a[start(a):]), body(b[start(b):])
n = 0
for r in range(max(len(a), len(b))):
    ra = a[r] if r < len(a) else []
    rb = b[r] if r < len(b) else []
    if ra != rb:
        col = next((k for k in range(min(len(ra), len(rb))) if ra[k] != rb[k]), min(len(ra), len(rb)))
        print("row %d col %d: omp=%s omc=%s | %s" % (r, col, ra[col] if col < len(ra) else None,
                                                     rb[col] if col < len(rb) else None, text(ra)[:100]))
        n += 1
print("%d differing rows" % n)
