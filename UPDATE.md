# Tracking upstream omp

omc is a 1:1 port of [oh-my-pi](https://github.com/can1357/oh-my-pi) (omp). This file says which upstream
commit omc currently matches and how to bring later upstream changes over.

## Pin: last upstream commit ported

| | |
|---|---|
| **Upstream commit** | `a7135b8f31826c878734388b648f52bed8da436e` |
| **Tag / version** | `v19.1.0` (`omp --version` → `omp/19.1.0`) |
| **Upstream date** | 2026-11-21 |
| **Pinned on** | 2026-11-23 |

Everything model-facing (prompts, tool schemas/descriptions, system prompt, output formatting), the TUI
(verified side by side against `omp/18.17.0`; the v18.18, v18.22 and v19.1 changes were then checked side by side against `omp/18.18.7`, `omp/18.22.0` and `omp/19.1.0`, see docs/status.md's update section) and the embedded data (catalog, themes,
symbols, settings table, slash commands, tips) corresponds to this commit. Update this table — and the
matching line in [AGENTS.md](AGENTS.md) — every time an update pass is finished.

## Update procedure

1. **Clone the pinned commit and fetch upstream** (anywhere outside the repo, e.g. a scratch dir):

   ```sh
   UP=/path/to/scratch/omp
   git clone https://github.com/can1357/oh-my-pi.git "$UP"
   cd "$UP"
   PIN=a7135b8f31826c878734388b648f52bed8da436e   # from the table above
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
   | Model catalog (`assets/models.json.z`) | `python3 build-tools/decode_catalog_v1.py "$UP/packages/catalog/src/models.json" out.json`, then `python3 build-tools/slim_catalog.py out.json assets/models.json.z` (upstream only discovers `charm-hyper` and `factory-droid` online: keep or update their rows) |
   | Glyph presets (`assets/tui/symbols.json`) | `python3 build-tools/extract_symbols.py "$UP/packages/tui/src/theme/symbols.ts" assets/tui/symbols.json` |
   | Themes (`assets/tui/themes/`) | `cp "$UP"/packages/tui/src/theme/{dark,light}.json assets/tui/themes/` (the other themes come from `packages/tui/src/theme/defaults.json`) |
   | Welcome tips (`assets/tui/tips.txt`) | `cp "$UP/packages/tui/src/prompt/tips.txt" assets/tui/tips.txt` |
   | Settings tables (`assets/tui/settings-ui.json`, `assets/config/settings-registry.json`) | `python3 build-tools/settings_patch.py "$UP" "$PIN" "$TARGET"` (report), then again with `--apply`; fix what it lists as unresolved by hand, and add generated keys such as `magicKeywords.<id>` by hand. It reads upstream as text and runs nothing |
   | Slash commands (`assets/tui/slash-commands.json`) | `python3 build-tools/extract_slash_commands.py "$UP" > assets/tui/slash-commands.json` |
   | Compat rules and providers (`assets/compat_rules.json.z`, `assets/providers.json.z`) | `python3 build-tools/slim_compat_rules.py "$UP/packages/catalog/src/compat" assets/compat_rules.json.z`, `python3 build-tools/slim_providers.py "$UP/packages/catalog/src/compat/rules.json" assets/providers.json.z` |
| Export viewer (`assets/export/`) | `file` and `integrity` from `$UP/packages/coding-agent/src/export/html/viewer.generated.json`, and bump `EXPORT_VIEWER_VERSION` in `src/session/export_html.h` |
| `/changelog` (`assets/CHANGELOG.md.z`, zlib) | `python3 -c 'import sys,zlib; open("assets/CHANGELOG.md.z","wb").write(zlib.compress(open(sys.argv[1],"rb").read(),9))' "$UP/packages/coding-agent/CHANGELOG.md"` (the released file: its newest `## [x.y.z]` heading must be the target version; tests/test_extracmds.c checks the top entry) |

   **Don't run `extract_settings.sh`:** it executes upstream TypeScript through the Bun embedded in `omp`, which breaks omc's no-Node and no-executing-upstream rules. Use `settings_patch.py` above.

   After refreshing the settings table, compare its defaults with `src/config/settings.c` (omc's
   defaults must equal omp's — see the "Settings defaults" row in [docs/status.md](docs/status.md)).
   After refreshing the settings tables, run
   `build-tools/settings_audit.py` and annotate every new unread key in `build-tools/settings_audit.tsv`
   (inert or ⛔); `test_settings_audit` fails until they are, and the counts in docs/gaps.md §3 come from it.

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

## Things that went wrong before, so check them

- The installed `omp` must match the target (`omp update`) for the side-by-side checks to mean anything. Run it only with a throwaway `HOME` and agent dir plus a symlink to `~/.omp/natives`. Some providers ignore a mock `baseUrl` and reach the real service (`google-vertex` does): keep those out of comparison matrices.
- `crates/pi-shell` is out of scope: omc keeps real bash (AGENTS.md).
- Most of a release's commits amend or revert earlier ones in it. Port the final state at the tag, not each step.
- `settings_patch.py` only sees `register({...})` calls: a setting registered through a helper (v18.18.7's
  `resetWindowSetting(id, label, description)`) shows up only as REMOVED for the key it replaced. Add those rows by hand.
- `settings_patch.py` can't evaluate a description written as a getter (`get description() { ... }`, usually with a key hint): it lists a changed one under UNRESOLVED, so copy that text by hand.
- Streams that split the commit list by path still overlap (one upstream commit touches tools, TUI and session): note cross-stream edits, merge the streams in one place and run the suite once on the merged result.
