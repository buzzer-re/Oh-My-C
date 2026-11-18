# Sourced by tests/e2e/run.sh after images.sh: the command line (WP-17). Expected texts were checked against omp
# v18.14.2 (`omp <args>` with throwaway HOME / PI_CODING_AGENT_DIR and the same mock model).
echo "== cli"
check "cli eq form"       "Tool said: eqform"    "$B" --model=mock/mock-1 --no-session -p "bash:echo eqform"
check "cli unknown flag"  $'Error: unknown flag: --bogus\nRun `omc --help` for available flags.'  "$B" --bogus x
check "cli unknown rc"    "rc=2"                 bash -c "'$B' --bogus x >/dev/null 2>&1; echo rc=\$?"
check "cli bad mode"      'Error: Invalid --mode value: "bad". Expected one of: text, json, rpc, rpc-ui, acp.'  "$B" --mode=bad
check "cli bad thinking"  'Invalid --thinking value: "zz". Expected one of: off, minimal, low, medium, high, xhigh, max, auto.'  "$B" --thinking zz -p x
check "cli max-time"      'Error: Invalid --max-time value: "abc". Expected a positive number of seconds or duration like "5s", "10m", "1h".'  "$B" --max-time=abc
check "cli max-cost"      'Error: Invalid --max-cost value: "-1". Expected a positive USD amount, e.g. 0.50.'  "$B" --max-cost=-1
check "cli spinner flag"  'Error: --spinner-interval: Spinner interval must be 0 (static) or a whole number of milliseconds ≥ 50, got "10".'  "$B" --spinner-interval 10
check "cli fps flag"      'Error: --max-fps: Max frames per second must be a whole number from 1 to 120, got "0".'  "$B" --max-fps 0
check "cli motion flag"   'Error: --motion: Motion mode must be one of full, reduced, none, got "fast".'  "$B" --motion fast
check "motion env"        "reduced"              env PI_MOTION=reduced "$B" config get tui.motion
check "spinner env"       "250"                  env PI_SPINNER_INTERVAL=250 "$B" config get tui.spinnerInterval
check "spinner set range" "Error: Spinner interval must be 0 (static) or a whole number of milliseconds ≥ 50, got 10."  "$B" config set tui.spinnerInterval 10
check "fps set range"     "Error: Max frames per second must be a whole number from 1 to 120, got 500."  "$B" config set tui.maxFps 500
check "cli goal print"    "Error: --goal requires an interactive terminal (not --print or --mode)."  "$B" --goal x -p y
check "cli restore print" "Error: --restore requires an interactive terminal (not --print or --mode)."  "$B" --restore -p
check "cli sp + tpl"      "Error: --system-prompt and --system-prompt-template cannot be combined"  "$B" --system-prompt x --system-prompt-template y
check "cli no tty"        "Error: interactive mode requires a terminal, but stdin is not a TTY."  "$B"
check "cli rpc @file"     "Error: @file arguments are not supported in RPC mode"  "$B" --mode rpc @x
check "cli reserved word" '`omc list` is not a top-level command. Run `omc launch list` if you meant to send "list" as a prompt.'  "$B" list
check "cli unported cmd"  'error: `omc stats` is not available in omc'  "$B" stats
check "cli -- literal"    "Tool said: --dash"    "$B" --model mock/mock-1 --no-session -p -- "bash:echo --dash"
check "cli version"       "omc/"                 "$B" --version
check "cli root help"     $'COMMANDS\n  acp '    "$B" --help
check "cli add-dir help"  "--add-dir=<value>                 Add a workspace directory beyond the working directory (repeatable)"  "$B" --help
check "cli cmd help"      $'Get the API key or OAuth token for a provider\n\nUSAGE\n  $ omc token PROVIDER [FLAGS]'  "$B" token --help
check "cli missing arg"   $'error: Missing required argument: provider\n\nUSAGE\n  $ omc token PROVIDER [FLAGS]\n\nRun `omc token --help` for details.'  "$B" token
check "cli bool value"    "error: Option '--raw' does not take an argument"  "$B" token x --raw=1
check "cli ambiguous"     "error: Option '--account' argument is ambiguous."  "$B" token x --account -2
check "cli int flag"      'error: Expected integer for --account, got "x"'  "$B" token x -a x
check "cli unknown opt"   "error: Unknown option '--foo'. To specify a positional argument starting with a '-', place it at the end of the command after '--', as in '-- \"--foo\""  "$B" token x --foo
check "token no cred"     'No active credential found for provider "anthropic".'  env -u ANTHROPIC_API_KEY -u ANTHROPIC_OAUTH_TOKEN "$B" token anthropic
check "token no oauth"    'No OAuth accounts found for provider "anthropic".'  "$B" token anthropic --list
# messages after the first are prompts of their own; @file text leads the first one
: > "$T/req.jsonl"
check "cli two prompts"   "Tool said: second"    "$B" --model mock/mock-1 --no-session -p "bash:echo first" "bash:echo second"
check "cli two requests"  "4"                    bash -c "wc -l < '$T/req.jsonl'"
printf 'file body text\n' > "$T/work/cli-at.txt"
check "cli @file"         "plain answer"         "$B" --model mock/mock-1 --no-session -p @cli-at.txt "bash:echo atfile"
check "cli @file wire"    "<file name=\\\"$T/work/cli-at.txt\\\">\\nfile body text\\n\\n</file>\\n"  grep -o '<file name=[^<]*</file>\\n' "$T/req.jsonl"
check "cli @missing"      "Error: File not found: $T/work/nope.txt"  "$B" --model mock/mock-1 --no-session -p @nope.txt
: > "$T/req.jsonl"
check "cli @image"        "plain answer"         bash -c "cd '$T/iwork' && ${IENV[*]} '$B' --model mock/vision-1 --no-session -p @shot.png 'say:x'"
check "cli @image note"   "[Image: original 3200x1800, displayed at 1568x882."  cat "$T/req.jsonl"
check "cli @image wire"   '"type": "image_url"'  cat "$T/req.jsonl"
# --system-prompt-template replaces the bundled template
printf 'CUSTOM TEMPLATE for {{cwd}}\n' > "$T/tpl.md"
: > "$T/req.jsonl"
check "cli template"      "tplok"                "$B" --model mock/mock-1 --no-session --system-prompt-template "$T/tpl.md" -p "say:tplok"
check "cli template wire" "CUSTOM TEMPLATE for $T/work"  cat "$T/req.jsonl"
check "cli template miss" "ENOENT: no such file or directory, open '$T/none.md'"  "$B" --system-prompt-template "$T/none.md" -p x
# systemPromptTemplates: the longest matching model pattern picks the template file; a missing file falls back to the
# bundled prompt (omp 8a09723b3f, ea9e49444c, f79f5c869c)
mkdir -p "$T/agent-spt" && cp "$T/agent/models.yml" "$T/agent-spt/"
printf 'SHORT {{model}}\n' > "$T/spt-short.md"; printf 'LONG TEMPLATE for {{model}}\n' > "$T/spt-long.md"
printf 'systemPromptTemplates:\n  "MOCK": %s\n  "mock/mock-1": %s\n  "mock-2": %s\n' "$T/spt-short.md" "$T/spt-long.md" "$T/spt-none.md" > "$T/agent-spt/config.yml"
: > "$T/req.jsonl"
OMC_CODING_AGENT_DIR="$T/agent-spt" timeout 30 "$B" --model mock/mock-1 --no-session -p "say:x" </dev/null >/dev/null 2>&1
check "model template"    "LONG TEMPLATE for mock/mock-1"  cat "$T/req.jsonl"
: > "$T/req.jsonl"
OMC_CODING_AGENT_DIR="$T/agent-spt" timeout 30 "$B" --model mock/mock-2 --no-session -p "say:x" </dev/null >/dev/null 2>&1
check "model template miss" "trusted coding assistant"  cat "$T/req.jsonl"
# --export (exportFromFile)
SESS=$(ls -t "$T/agent/sessions"/*/*.jsonl 2>/dev/null | head -1)
check "cli export"        "Exported to: $T/cli-export.html"  "$B" --export "$SESS" "$T/cli-export.html"
check "cli export rel"    "Exported to: $T/rel-export.html"  "$B" --export "$SESS" "sub/../../rel-export.html"  # omp 9ee085a440: resolved once
check "cli export miss"   "Error: File not found: $T/none.jsonl"  "$B" --export "$T/none.jsonl"
# subcommands
check "models json"       '"selector":"mock/mock-1"'  "$B" models --json mock
check "models table"      $'mock ('              "$B" models mock
check "models find"       '`omc models find` requires a search substring'  "$B" models find
check "config get"        "true"                 "$B" config get compaction.enabled
check "config bad bool"   "Error: Invalid boolean value: nope. Use true/false, yes/no, on/off, or 1/0"  "$B" config set compaction.enabled nope
check "config unknown"    "Unknown setting: nosuch.key"  "$B" config get nosuch.key
check "config set json"   '{"key":"display.showTokenUsage","value":true}'  "$B" config set display.showTokenUsage true --json
check "config reset"      "Reset display.showTokenUsage to false"  "$B" config reset display.showTokenUsage
check "config scope"      "Settings in the global layer:"  "$B" config list --scope global
check "agents unpack"     '"total": 6'           "$B" agents unpack --dir "$T/ag" --json
check "agents yaml"       'description: "Plain-text verdict summary, 1-3 sentences"'  cat "$T/ag/reviewer.md"
check "ssh add"           'Added SSH host "box" to project config'  "$B" ssh add box --host 10.0.0.1 --user me --port 2222
check "ssh list"          "box  10.0.0.1  me  port:2222"  "$B" ssh list
check "ssh remove"        'Removed SSH host "box" from project config'  "$B" ssh remove box
check "ssh bad port"      "Error: Port must be an integer between 1 and 65535"  "$B" ssh add x --host h --port 0
mkdir -p "$T/work/gr" && printf 'alpha\nfoo one\n' > "$T/work/gr/a.txt" && printf 'foo\n' > "$T/work/gr/b.md"
check "grep cli"          $'Total matches: 2\nFiles with matches: 2'  "$B" grep foo gr
check "grep count"        "b.md: 1 matches"      "$B" grep foo gr -c
check "read cli"          "foo one"              "$B" read gr/a.txt
check "auth import"       "Imported 1 credential(s) for cli-prov."  bash -c "printf '%s' '{\"version\":1,\"credentials\":[{\"provider\":\"cli-prov\",\"credential\":{\"type\":\"api_key\",\"key\":\"k1\"}}]}' | '$B' auth import -"
check "auth export"       '"provider": "cli-prov"'  "$B" auth export
check "auth bad version"  'error: stdin has export version 2; this omc reads version 1.'  bash -c "echo '{\"version\":2}' | '$B' auth import -"
check "completions bash"  "complete -F _omc omc"  "$B" completions bash
check "completions usage" "Usage: omc completions <bash|zsh|fish>"  "$B" completions tcsh
check "complete models"   $'mock/mock-1\tmock'   "$B" __complete models -- mock/mock-1
check "worktree none"     "No agent-managed worktrees found under"  env OMC_WORKTREE_DIR="$T/wt" "$B" worktree
check "setup needs comp"  "error: setup --check/--json requires a COMPONENT (python)"  "$B" setup --check
