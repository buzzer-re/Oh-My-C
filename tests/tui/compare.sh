#!/usr/bin/env bash
# Side-by-side TUI comparison: runs the installed `omp` (reference) and build/omc in two tmux sessions
# of identical size, both talking to the scripted mock model (tests/e2e/mock_openai.py) through
# throwaway agent dirs, so the user's real ~/.omp config, sessions and tokens are never touched.
#
#   tests/tui/compare.sh start [cols] [rows]   start mock + both apps (first-run setup skipped in omp)
#   tests/tui/compare.sh keys <tmux keys...>    send the same keys to both (e.g. "bash:echo hi" Enter)
#   tests/tui/compare.sh shot [name]            capture both panes (plain + ANSI) into $CMP_DIR/<name>.{omp,omc}[.ansi]
#   tests/tui/compare.sh diff [name]            plain-text diff of the last shot
#   tests/tui/compare.sh restart [args...]      restart both apps (same mock/agent dirs) with extra CLI args, e.g. -c
#   tests/tui/compare.sh stop
# HOME is a throwaway dir too (no ~/.claude MCP/plugin discovery); omp's update check is off.
# Env: CMP_DIR (default: $TMPDIR/omc-tuicmp), OMP_BIN (default: omp), CMP_SETUP=1 keeps omp's wizard,
#      CMP_NOMODEL=1 (no models.yml / default model: the no-credentials first screen),
#      CMP_CONFIG (extra config.yml lines for both, e.g. 'composer:\n  shape: box'),
#      CMP_NAME (tmux session prefix, default cmp; set a unique one when several harnesses run at once),
#      CMP_WAIT (seconds to wait after launching, default 6).
# Without CMP_SETUP both config.yml files start with setupVersion: 2 (both apps past the first-run wizard).
#      CMP_SPLIT=1 gives each app its own work dir ($D/omp/work, $D/omc/work: tools that modify files don't race),
#      CMP_SEED=<dir> is copied into the work dir(s) before start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=${CMP_DIR:-${TMPDIR:-/tmp}/omc-tuicmp}
OMP=${OMP_BIN:-omp}
PORT_FILE=$D/port
N=${CMP_NAME:-cmp}
cmd=${1:-}; shift || true
mkagent() { # dir port
    mkdir -p "$1"
    if [ -n "${CMP_NOMODEL:-}" ]; then printf 'startup:\n  checkUpdate: false\n' > "$1/config.yml"; return; fi
    cat > "$1/models.yml" <<YML
providers:
  mock:
    baseUrl: http://127.0.0.1:$2/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: mock-1
        name: Mock One
        contextWindow: 100000
        maxTokens: 4096
YML
    printf 'modelRoles:\n  default: mock/mock-1\nstartup:\n  checkUpdate: false\n' > "$1/config.yml"
    [ -n "${CMP_CONFIG:-}" ] && printf '%b\n' "$CMP_CONFIG" >> "$1/config.yml"
    [ -n "${CMP_SETUP:-}" ] || printf 'setupVersion: 2\n' >> "$1/config.yml"; true
}
workdirs() { if [ -n "${CMP_SPLIT:-}" ]; then WO=$D/omp/work; WC=$D/omc/work; else WO=$D/work; WC=$D/work; fi; }
launch() { # cols rows extra-args...
    local cols=$1 rows=$2; shift 2
    workdirs
    tmux new-session -d -s "$N-omp" -x "$cols" -y "$rows" -c "$WO" "env HOME=$D/home MOCK_KEY=x PI_CODING_AGENT_DIR=$D/omp-agent $OMP $*"
    tmux new-session -d -s "$N-omc" -x "$cols" -y "$rows" -c "$WC" "env HOME=$D/home MOCK_KEY=x OMC_CODING_AGENT_DIR=$D/omc-agent $ROOT/build/omc $*"
    echo "$cols $rows" > "$D/size"
}
case "$cmd" in
start)
    cols=${1:-120}; rows=${2:-40}
    "$0" stop >/dev/null 2>&1
    rm -rf "$D"; mkdir -p "$D/home/.omp"
    # omp extracts its native module into ~/.omp/natives; share the real cache (omp's own files) with the throwaway HOME
    [ -d "$HOME/.omp/natives" ] && ln -s "$HOME/.omp/natives" "$D/home/.omp/natives"
    workdirs; mkdir -p "$WO" "$WC"
    if [ -n "${CMP_SEED:-}" ]; then cp -R "$CMP_SEED/." "$WO/"; [ "$WO" = "$WC" ] || cp -R "$CMP_SEED/." "$WC/"; fi
    # pick a free port; retry when another harness grabbed it first (the mock exits on EADDRINUSE)
    for _ in 1 2 3 4 5 6 7 8; do
        port=$((30000 + RANDOM % 20000))
        (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && continue
        MOCK_LOG="$D/req.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $port > "$D/mock.log" 2>&1 & echo $! > "$D/mock.pid"
        for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
        kill -0 "$(cat "$D/mock.pid")" 2>/dev/null && break
    done
    echo $port > "$PORT_FILE"
    mkagent "$D/omp-agent" $port; mkagent "$D/omc-agent" $port
    launch "$cols" "$rows"
    sleep "${CMP_WAIT:-6}"
    if [ -z "${CMP_SETUP:-}" ]; then
        # a fresh omp agent dir opens the 5-step first-run wizard; Esc skips each step
        for _ in 1 2 3 4 5; do
            tmux capture-pane -p -t $N-omp | grep -q "Setup step" || break
            tmux send-keys -t $N-omp Escape; sleep 1.2
        done
    fi
    # wait for omp's main screen (after the wizard's hand-off animation)
    for _ in $(seq 40); do tmux capture-pane -p -t $N-omp | grep -q "π >\|╰─" && break; sleep 0.25; done
    sleep 0.5
    echo "started in $D (mock port $port)";;
restart)
    tmux kill-session -t "$N-omp" 2>/dev/null; tmux kill-session -t "$N-omc" 2>/dev/null
    read -r cols rows < "$D/size"
    launch "$cols" "$rows" "$@"
    sleep 5
    echo "restarted";;
keys)
    for s in "$N-omp" "$N-omc"; do tmux send-keys -t $s "$@"; done;;
shot)
    n=${1:-shot}
    for s in omp omc; do
        tmux capture-pane -p -t $N-$s > "$D/$n.$s"
        tmux capture-pane -p -e -t $N-$s > "$D/$n.$s.ansi"
    done
    echo "===== omp"; cat "$D/$n.omp"; echo "===== omc"; cat "$D/$n.omc";;
diff)
    n=${1:-shot}; diff "$D/$n.omp" "$D/$n.omc" && echo "identical";;
stop)
    tmux kill-session -t $N-omp 2>/dev/null; tmux kill-session -t $N-omc 2>/dev/null
    [ -f "$D/mock.pid" ] && kill "$(cat "$D/mock.pid")" 2>/dev/null; true;;
*) sed -n '2,17p' "$0"; exit 2;;
esac
