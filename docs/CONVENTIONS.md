# omc coding conventions

omc is a from-scratch C11 (gnu11) rewrite of oh-my-pi (omp). Goals: 1:1 behavior with omp,
zero Node/Bun, minimal system deps (libc, OpenSSL, zlib, pthread, libm), high performance, safety.

## Rules
- No third-party source code or libraries beyond libc/OpenSSL/zlib/pthread/libm. Everything else
  (regex, YAML, markdown, TUI, HTTP) is written here.
- Use `src/core/core.h` helpers: `xmalloc/xstrdup/xasprintf` (abort on OOM), `sb_t` string builder,
  `VEC(T)`/`vec_push`, `strvec_t`, `hmap_t`, utf8 helpers, `str_display_width`.
- JSON is `src/core/json.h` (`json_t`, ordered objects). Messages/tools/events are json trees with
  upstream omp shapes (see `src/ai/ai.h` header comment).
- Files: `src/<area>/<name>.c` + `.h`. The Makefile auto-compiles `src/*/*.c`.
- Owned vs borrowed: `char *` returns are owned; `const char *` borrowed. Document exceptions.
- Bounds-check everything coming from the network, files or the model. No unbounded recursion on
  untrusted input (cap depth). Never pass untrusted strings as format strings.
- Style: 4-space indent, `snake_case`, module prefix on public functions (`rx_`, `yaml_`, `tui_`...),
  `static` for internals, brief comments only where non-obvious.
- Compile clean with: `gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Isrc -c file.c`
- Tests: `tests/test_<area>.c` with a `main` that returns non-zero on failure; link against needed objects.
- Upstream reference checkout (read-only): see the path given in your task brief.
