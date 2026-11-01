#!/usr/bin/env bash
# Side-by-side TUI comparison: runs the installed `omp` (reference) and build/omc in two tmux sessions
# of identical size, both talking to the scripted mock model (tests/e2e/mock_openai.py) through
# throwaway agent dirs, so the user's real ~/.omp config, sessions and tokens are never touched.
#
#   tests/tui/compare.sh start [cols] [rows]   start mock + both apps (first-run setup skipped in omp)
#   tests/tui/compare.sh keys <tmux keys...>    send the same keys to both (e.g. "bash:echo hi" Enter)
#   tests/tui/compare.sh shot [name]            capture both panes (plain + ANSI) into $CMP_DIR/<name>.{omp,omc}[.ansi]
#   tests/tui/compare.sh diff [name]            plain-text diff of the last shot
#   tests/tui/compare.sh stop
# HOME is a throwaway dir too (no ~/.claude MCP/plugin discovery); omp's update check is off.
# Env: CMP_DIR (default: $TMPDIR/omc-tuicmp), OMP_BIN (default: omp), CMP_SETUP=1 keeps omp's wizard,
#      CMP_NAME (tmux session prefix, default cmp; set a unique one when several harnesses run at once).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=${CMP_DIR:-${TMPDIR:-/tmp}/omc-tuicmp}
OMP=${OMP_BIN:-omp}
PORT_FILE=$D/port
N=${CMP_NAME:-cmp}
cmd=${1:-}; shift || true
mkagent() { # dir port
    mkdir -p "$1"
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
}
case "$cmd" in
start)
    cols=${1:-120}; rows=${2:-40}
    "$0" stop >/dev/null 2>&1
    rm -rf "$D"; mkdir -p "$D/work" "$D/home"
    port=$((30000 + RANDOM % 20000)); echo $port > "$PORT_FILE"
    mkagent "$D/omp-agent" $port; mkagent "$D/omc-agent" $port
    MOCK_LOG="$D/req.jsonl" python3 -I "$ROOT/tests/e2e/mock_openai.py" $port > "$D/mock.log" 2>&1 & echo $! > "$D/mock.pid"
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
    tmux new-session -d -s $N-omp -x "$cols" -y "$rows" -c "$D/work" "env HOME=$D/home MOCK_KEY=x PI_CODING_AGENT_DIR=$D/omp-agent $OMP"
    tmux new-session -d -s $N-omc -x "$cols" -y "$rows" -c "$D/work" "env HOME=$D/home MOCK_KEY=x OMC_CODING_AGENT_DIR=$D/omc-agent $ROOT/build/omc"
    sleep 6
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
keys)
    for s in $N-omp $N-omc; do tmux send-keys -t $s "$@"; done;;
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
*) sed -n '2,14p' "$0"; exit 2;;
esac
