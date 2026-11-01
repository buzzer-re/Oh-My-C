#!/usr/bin/env python3
"""Dev tool: cut a golden region out of a `tmux capture-pane -e` capture of the reference omp
(tests/tui/compare.sh shot) for tests/test_tui.c.

  mkgolden.py <capture.ansi> <first-line-text> <last-line-text> <out.ansi> [--before N] [--after N]

Lines are kept verbatim (tmux SGR, state carried from line to line); the region runs from the last line
containing <first-line-text> (minus N lines) to the next line containing <last-line-text>."""
import re, sys

src, first, last, out = sys.argv[1:5]
before = int(sys.argv[sys.argv.index("--before") + 1]) if "--before" in sys.argv else 0
after = int(sys.argv[sys.argv.index("--after") + 1]) if "--after" in sys.argv else 0
lines = open(src, encoding="utf-8").read().split("\n")
plain = [re.sub(r"\x1b\[[0-9;:]*m", "", l) for l in lines]
i = max(k for k, l in enumerate(plain) if first in l) - before
j = next(k for k in range(i, len(lines)) if last in plain[k]) + after
open(out, "w", encoding="utf-8").write("\n".join(lines[i : j + 1]) + "\n")
