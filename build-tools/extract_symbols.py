#!/usr/bin/env python3
"""Dev-time only: extract upstream omp symbol presets (packages/tui/src/theme/symbols.ts) into
assets/tui/symbols.json. Usage: extract_symbols.py <symbols.ts> <out.json>"""
import json, re, sys

src = open(sys.argv[1], encoding="utf-8").read()
pair = re.compile(r'^\s*"([A-Za-z]+\.[A-Za-z0-9.]+)"\s*:\s*("(?:[^"\\]|\\.)*")\s*,?', re.M)

def js_str(v):
    # JS-only escapes: \u{XXXXX}
    v = re.sub(r"\\u\{([0-9a-fA-F]+)\}", lambda m: chr(int(m.group(1), 16)), v)
    return json.loads(v)

def block(name):
    start = src.index(f"const {name}: SymbolMap = {{")
    end = src.index("\n};", start)
    return {k: js_str(v) for k, v in pair.findall(src[start:end])}

out = {"presets": {"unicode": block("UNICODE_SYMBOLS"), "nerd": block("NERD_SYMBOLS"), "ascii": block("ASCII_SYMBOLS")}}
sp = src[src.index("export const SPINNER_FRAMES"):]
sp = sp[:sp.index("\n};")]
spin = {}
for preset in ("unicode", "nerd", "ascii"):
    m = re.search(preset + r":\s*\{\s*status:\s*(\[[^\]]*\]),\s*activity:\s*(\[[^\]]*\]),?\s*\}", sp)
    spin[preset] = {"status": json.loads(re.sub(r",\s*\]", "]", m.group(1))), "activity": json.loads(re.sub(r",\s*\]", "]", m.group(2)))}
out["spinners"] = spin
keys = [set(v) for v in out["presets"].values()]
assert keys[0] == keys[1] == keys[2], "preset key sets differ"
json.dump(out, open(sys.argv[2], "w", encoding="utf-8"), ensure_ascii=False, indent=1, sort_keys=True)
print(len(keys[0]), "symbol keys")
