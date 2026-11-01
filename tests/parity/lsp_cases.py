#!/usr/bin/env python3
"""Emit the lsp tool parity cases (tests/parity/tool_diff.sh format) for the fixtures/lsp_c project.
Every case calls the lsp device through `write xd://lsp` (omp mounts lsp under xd://)."""
import json

CASES = [
    ("status", {"action": "status"}),
    ("diag_main", {"action": "diagnostics", "file": "main.c"}),
    ("diag_glob", {"action": "diagnostics", "file": "*.c"}),
    ("diag_clean", {"action": "diagnostics", "file": "util.c"}),
    ("diag_nofile", {"action": "diagnostics"}),
    ("diag_nomatch", {"action": "diagnostics", "file": "nope/*.zz"}),
    ("definition", {"action": "definition", "file": "main.c", "line": 6, "symbol": "add"}),
    ("type_def", {"action": "type_definition", "file": "main.c", "line": 5, "symbol": "p"}),
    ("implementation", {"action": "implementation", "file": "main.c", "line": 6, "symbol": "add"}),
    ("references", {"action": "references", "file": "util.h", "line": 5, "symbol": "add"}),
    ("references_none", {"action": "references", "file": "main.c", "line": 7, "symbol": "unused_var"}),
    ("hover", {"action": "hover", "file": "main.c", "line": 6, "symbol": "add"}),
    ("hover_none", {"action": "hover", "file": "main.c", "line": 3}),
    ("symbol_missing", {"action": "hover", "file": "main.c", "line": 6, "symbol": "nosuch"}),
    ("symbols", {"action": "symbols", "file": "util.h"}),
    ("symbols_main", {"action": "symbols", "file": "main.c"}),
    ("wsymbols", {"action": "symbols", "file": "*", "query": "point"}),
    ("wsymbols_none", {"action": "symbols", "file": "*", "query": "zzzqqq"}),
    ("wsymbols_noquery", {"action": "symbols", "file": "*"}),
    ("rename_preview", {"action": "rename", "file": "util.h", "line": 5, "symbol": "add", "new_name": "plus", "apply": False}),
    ("rename_apply", {"action": "rename", "file": "util.h", "line": 5, "symbol": "add", "new_name": "plus"}),
    ("rename_noname", {"action": "rename", "file": "util.h", "line": 5, "symbol": "add"}),
    ("code_actions", {"action": "code_actions", "file": "main.c", "line": 7, "symbol": "unused_var"}),
    ("code_actions_none", {"action": "code_actions", "file": "util.c", "line": 1}),
    ("capabilities", {"action": "capabilities", "file": "main.c"}),
    ("request", {"action": "request", "file": "main.c", "query": "textDocument/documentHighlight", "line": 6, "symbol": "total"}),
    ("request_noquery", {"action": "request", "file": "main.c"}),
    ("request_badjson", {"action": "request", "file": "main.c", "query": "x/y", "payload": "{nope"}),
    ("reload", {"action": "reload", "file": "main.c"}),
    ("rename_file_preview", {"action": "rename_file", "file": "util.c", "new_name": "util2.c", "apply": False}),
    ("rename_file_apply", {"action": "rename_file", "file": "util.c", "new_name": "util2.c"}),
    ("rename_file_missing", {"action": "rename_file", "file": "nope.c", "new_name": "util2.c"}),
    ("rename_file_exists", {"action": "rename_file", "file": "util.c", "new_name": "main.c"}),
    ("nofile", {"action": "hover"}),
    ("noserver", {"action": "hover", "file": "compile_flags.txt", "line": 1}),
]

def xd(args):
    return ["write", {"path": "xd://lsp", "content": json.dumps(args)}]

# several calls in one session (warm server: rebuild-path diagnostics, cached diagnostics for code actions, ...)
SEQS = [
    ("warm", [{"action": "diagnostics", "file": "main.c"}, {"action": "status"},
              {"action": "diagnostics", "file": "main.c"}, {"action": "diagnostics", "file": "*.c"},
              {"action": "code_actions", "file": "main.c", "line": 9, "symbol": "missing_symbol"},
              {"action": "hover", "file": "util.h", "line": 7, "symbol": "point"}]),
    ("code_action_apply", [{"action": "diagnostics", "file": "main.c"},
                           {"action": "code_actions", "file": "main.c", "line": 7, "symbol": "unused_var", "query": "0", "apply": True},
                           {"action": "code_actions", "file": "main.c", "line": 7, "symbol": "unused_var", "query": "nomatch", "apply": True}]),
    ("write_then_diag", [["write", {"path": "bad.c", "content": "#include \"util.h\"\nint f(void) {\n    return add(1, nothere);\n}\n"}],
                         {"action": "diagnostics", "file": "bad.c"},
                         ["write", {"path": "bad.c", "content": "#include \"util.h\"\nint f(void) {\n    return add(1, nothere) + gone;\n}\n"}],
                         ["write", {"path": "bad.c", "content": "int f(void) { return 1; }\n"}]]),
    ("code_action_fix", [["write", {"path": "unused.c", "content": "#include \"util.h\"\nint g(void) { return 1; }\n"}],
                         {"action": "diagnostics", "file": "unused.c"},
                         {"action": "code_actions", "file": "unused.c", "line": 1},
                         {"action": "code_actions", "file": "unused.c", "line": 1, "query": "remove", "apply": True},
                         {"action": "diagnostics", "file": "unused.c"}]),
    ("rename_multi", [{"action": "rename", "file": "util.c", "line": 7, "symbol": "point_sum", "new_name": "psum", "apply": False},
                      {"action": "rename", "file": "util.c", "line": 7, "symbol": "point_sum", "new_name": "psum"},
                      {"action": "references", "file": "util.c", "line": 3, "symbol": "add"}]),
]

for name, args in CASES:
    print(f"# {name}")
    print("call:write:" + json.dumps({"path": "xd://lsp", "content": json.dumps(args)}))
for name, calls in SEQS:
    print(f"# {name}")
    print("seq:" + json.dumps([c if isinstance(c, list) else xd(c) for c in calls]))
