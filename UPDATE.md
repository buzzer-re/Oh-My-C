# Tracking upstream omp

omc is a 1:1 port of [oh-my-pi](https://github.com/can1357/oh-my-pi) (omp). This file says which upstream
commit omc currently matches and how to bring later upstream changes over.

## Pin: last upstream commit ported

| | |
|---|---|
| **Upstream commit** | `7efd77219374ca4e8767979809835e4aa90666ba` |
| **Tag / version** | `v18.14.2` (`omp --version` → `omp/18.14.2`) |
| **Upstream date** | 2026-10-31 |
| **Pinned on** | 2026-11-01 |

Everything model-facing (prompts, tool schemas/descriptions, system prompt, output formatting), the TUI
(verified side by side against the installed `omp/18.14.2`) and the embedded data (catalog, themes,
symbols, settings table, slash commands, tips) corresponds to this commit. Update this table — and the
matching line in [AGENTS.md](AGENTS.md) — every time an update pass is finished.

## Update procedure

1. **Clone the pinned commit and fetch upstream** (anywhere outside the repo, e.g. a scratch dir):

   ```sh
   UP=/path/to/scratch/omp
   git clone https://github.com/can1357/oh-my-pi.git "$UP"
   cd "$UP"
   PIN=7efd77219374ca4e8767979809835e4aa90666ba   # from the table above
   git fetch --tags origin
   TARGET=origin/main                               # or the newest release tag: git tag --sort=-v:refname | head -1
   git log --oneline --no-merges "$PIN..$TARGET" | wc -l
   ```

   Prefer a release tag that matches the omp you can run locally (`omp --version`; `omp update` installs
   the latest), so the side-by-side harnesses compare against the same code.

2. **List what changed, by area:**

   ```sh
   git log --no-merges --format='%h %s' "$PIN..$TARGET"
   git diff --stat "$PIN" "$TARGET" -- packages crates
   # per area, e.g.:
   git log --oneline "$PIN..$TARGET" -- packages/coding-agent/src/tools
   git diff "$PIN" "$TARGET" -- packages/ai/src/providers/anthropic.ts
   ```

3. **Triage every commit.** Port it when it is a *meaningful* change to behaviour omc already has or
   should have:
   - bug fixes and correctness fixes (always);
   - anything the model sees: prompts, tool schemas/descriptions, system prompt, tool result text,
     output truncation/notices, compaction, title/memory pipelines;
   - provider/API wire changes (new headers, endpoints, error handling, catalog/model data);
   - TUI changes visible in the terminal renderer (omc does not port omp's native/TSP rendering);
   - security fixes.

   Skip: Bun/Node/JS-runtime-only internals with no observable behaviour, native (TSP) rendering,
   build/CI tooling, upstream tests themselves (port the behaviour they pin down instead), version bumps.
   When unsure, port it.

4. **Port.** Map the upstream file to its omc counterpart (see the module map in
   [docs/architecture.md](docs/architecture.md)), implement in C from scratch (no new dependencies —
   libc, OpenSSL, zlib, pthread, libm only; no vendored code), and add/adjust tests.

5. **Refresh embedded upstream data** (dev-time tools; they never run at build or runtime):

   | Data | Command |
   |---|---|
   | Prompt templates (`prompts/`) | `build-tools/sync_prompts.sh "$UP"` (report) then `build-tools/sync_prompts.sh "$UP" --apply` |
   | Model catalog (`assets/models.json.z`) | `python3 build-tools/slim_catalog.py "$UP/packages/catalog/src/models.json" assets/models.json.z` |
   | Glyph presets (`assets/tui/symbols.json`) | `python3 build-tools/extract_symbols.py "$UP/packages/tui/src/theme/symbols.ts" assets/tui/symbols.json` |
   | Themes (`assets/tui/themes/`) | `cp "$UP"/packages/tui/src/theme/{dark,light}.json "$UP"/packages/tui/src/theme/defaults/*.json assets/tui/themes/` |
   | Welcome tips (`assets/tui/tips.txt`) | `cp "$UP/packages/tui/src/prompt/tips.txt" assets/tui/tips.txt` |
   | Settings table (`assets/tui/settings-ui.json`) | `build-tools/extract_settings.sh "$UP" > assets/tui/settings-ui.json` |
   | Slash commands (`assets/tui/slash-commands.json`) | `python3 build-tools/extract_slash_commands.py "$UP" > assets/tui/slash-commands.json` |
   | `/changelog` (`assets/CHANGELOG.md.z`, zlib) | `python3 -c 'import sys,zlib; open("assets/CHANGELOG.md.z","wb").write(zlib.compress(open(sys.argv[1],"rb").read(),9))' "$UP/packages/coding-agent/CHANGELOG.md"` (the released file: its newest `## [x.y.z]` heading must be the target version; tests/test_extracmds.c checks the top entry) |

   After refreshing the settings table, compare its defaults with `src/config/settings.c` (omc's
   defaults must equal omp's — see the "Settings defaults" row in [docs/status.md](docs/status.md)).

6. **Verify parity** against an omp built from the target commit (the installed `omp` when versions match):
   - `make -B -j8` (zero warnings) and `make test` (unit + e2e) must pass;
   - `tests/parity/request_diff.sh` — first model request (tools, schemas, system prompt) identical;
   - `tests/tui/compare.sh` — TUI side by side (idle screen, a conversation, tool cards, overlays);
   - never point either app at real models or the user's real `~/.omp`; use the mocks under `tests/e2e/`.

7. **Record it.** Update [docs/status.md](docs/status.md) and the relevant docs under [docs/](docs/),
   then **move the pin** in the table above and in [AGENTS.md](AGENTS.md) to the commit you ported up to
   (full hash, tag, upstream date, today's date), and commit with a message like
   `upstream: port omp <old-tag>..<new-tag> (<summary>)`.

Partial passes are fine: if you stop midway, pin the last upstream commit whose changes are *all* ported
(or still triaged as skip), never a later one.
