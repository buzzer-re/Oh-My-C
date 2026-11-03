#!/usr/bin/env python3
"""Dev-only: time the path from submitting the first prompt to the first model request, in omp and omc, against
tests/e2e/mock_openai.py modelling a single-slot local server (MOCK_SLOT_SECS prefill per request, MOCK_THINK_SECS more
when the request would think). Both apps run with throwaway HOME and agent dirs: a local "splash" openai-completions
provider (Qwen-style reasoning model, image input, thinking min/max levels) that is also the smol role, in a git
project with AGENTS.md and two skills (so skill-description compression runs).

  first_request_timing.py [--mode rpc|print|tui] [--only omp|omc] [--slot S] [--think S] [--title] [--keep]

For each app it prints every request the mock saw relative to the moment the prompt was submitted: arrival, queue
time in the slot, kind (main / compress / title / side), size, and whether a Qwen template would think on it.
Env: OMP_BIN (default omp), OMC_BIN (default build/omc), TMPDIR."""
import argparse, json, os, pty, select, shutil, signal, socket, subprocess, sys, tempfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ap = argparse.ArgumentParser()
ap.add_argument("--mode", default="rpc", choices=("rpc", "print", "tui"))
ap.add_argument("--only", choices=("omp", "omc"))
ap.add_argument("--slot", type=float, default=1.0)
ap.add_argument("--think", type=float, default=3.0)
ap.add_argument("--title", action="store_true", help="keep title generation on")
ap.add_argument("--keep", action="store_true")
ap.add_argument("--prompt", default="say:hello there, summarize the build rules")
args = ap.parse_args()

base = tempfile.mkdtemp(prefix="omc-frt-")
SKILLS = {"deploy": "Deploy the service to staging or production using the release scripts, including canary checks, "
                    "rollback steps and the on-call notification flow. Use when the user asks to ship, release or roll back.",
          "db-migrate": "Write and apply PostgreSQL schema migrations with the project's migration tool; covers naming, "
                        "reversible steps and data backfills."}

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def setup(app, port):
    agent, home, work = (os.path.join(base, app, d) for d in ("agent", "home", "work"))
    for d in (agent, home, work): os.makedirs(d)
    with open(os.path.join(agent, "models.yml"), "w") as f:
        f.write(f"""providers:
  splash:
    baseUrl: http://127.0.0.1:{port}/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: qwen3.6-27b
        name: Qwen 3.6 27B
        reasoning: true
        input: [text, image]
        contextWindow: 131072
        maxTokens: 32768
        thinking:
          mode: effort
          minLevel: low
          maxLevel: high
""")
    with open(os.path.join(agent, "config.yml"), "w") as f:
        f.write("modelRoles:\n  default: splash/qwen3.6-27b\n  smol: splash/qwen3.6-27b\n"
                "startup:\n  checkUpdate: false\nsetupVersion: 2\n")
    # omp extracts its native module into ~/.omp/natives: share that cache (omp's own files) like tests/tui/compare.sh
    natives = os.path.expanduser("~/.omp/natives")
    if app == "omp" and os.path.isdir(natives):
        os.makedirs(os.path.join(home, ".omp")); os.symlink(natives, os.path.join(home, ".omp", "natives"))
    subprocess.run(["git", "init", "-q", work], check=False)
    with open(os.path.join(work, "AGENTS.md"), "w") as f:
        f.write("# Project rules\n- Build with `make -j4`; run `make test` before committing.\n")
    for name, desc in SKILLS.items():
        d = os.path.join(work, ".omp", "skills", name); os.makedirs(d)
        with open(os.path.join(d, "SKILL.md"), "w") as f:
            f.write(f"---\nname: {name}\ndescription: {desc}\n---\n# {name}\nSteps.\n")
    return agent, home, work

def run(app, binary):
    port = free_port()
    agent, home, work = setup(app, port)
    timelog = os.path.join(base, app, "time.jsonl")
    env = dict(os.environ, MOCK_TIMELOG=timelog, MOCK_SLOT_SECS=str(args.slot), MOCK_THINK_SECS=str(args.think),
               MOCK_LOG=os.path.join(base, app, "req.jsonl"))
    mock = subprocess.Popen([sys.executable, "-I", os.path.join(ROOT, "tests/e2e/mock_openai.py"), str(port)], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        try: socket.create_connection(("127.0.0.1", port), 0.2).close(); break
        except OSError: time.sleep(0.1)
    aenv = dict(os.environ, HOME=home, MOCK_KEY="x", TERM="xterm-256color", COLUMNS="120", LINES="40")
    aenv["PI_CODING_AGENT_DIR" if app == "omp" else "OMC_CODING_AGENT_DIR"] = agent
    if not args.title: aenv.update(PI_NO_TITLE="1", OMC_NO_TITLE="1")
    t_spawn = time.time(); t_send = None; ready = None
    if args.mode == "print":
        t_send = t_spawn
        subprocess.run([binary, "-p", args.prompt], cwd=work, env=aenv, stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    elif args.mode == "rpc":
        p = subprocess.Popen([binary, "--mode", "rpc"], cwd=work, env=aenv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True, bufsize=1)
        p.stdin.write(json.dumps({"id": "s", "type": "get_state"}) + "\n"); p.stdin.flush()
        sent = False; deadline = time.time() + 180
        while time.time() < deadline:
            line = p.stdout.readline()
            if not line: break
            try: m = json.loads(line)
            except ValueError: continue
            if m.get("id") == "s" and not sent:
                ready = time.time()
                p.stdin.write(json.dumps({"id": "1", "type": "prompt", "message": args.prompt}) + "\n"); p.stdin.flush()
                t_send = time.time(); sent = True
            elif m.get("type") == "agent_end":
                time.sleep(args.slot + args.think + 1)  # let trailing side requests (title) land
                break
        p.kill(); p.wait()
    else:  # tui: a pty; wait for the editor box, then for 1 s of quiet; type the prompt, press Enter
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(work); os.execvpe(binary, [binary], aenv)
        buf = b""; seen = None; deadline = time.time() + 60
        while time.time() < deadline and (seen is None or time.time() - seen < 2.0):  # editor box, then 2 s settle
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try: buf += os.read(fd, 65536)
                except OSError: break
            if seen is None and "╰".encode() in buf: seen = time.time()
        ready = time.time()
        os.write(fd, args.prompt.encode()); time.sleep(0.3)
        os.write(fd, b"\r"); t_send = time.time()
        end = time.time() + 60
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try: os.read(fd, 65536)
                except OSError: break
            evs = [json.loads(l) for l in open(timelog)] if os.path.exists(timelog) else []
            if any(e["kind"] == "main" and e["event"] == "start" for e in evs) and time.time() - max(e["t"] for e in evs) > args.slot + args.think + 1:
                break
        os.kill(pid, signal.SIGKILL); os.waitpid(pid, 0); os.close(fd)
    mock.kill(); mock.wait()
    evs = [json.loads(l) for l in open(timelog)] if os.path.exists(timelog) else []
    print(f"== {app} ({args.mode}): startup {((ready or t_send) - t_spawn):.2f}s before the prompt could be sent")
    for e in evs:
        rel = e["t"] - t_send
        if e["event"] == "arrive":
            print(f"  {rel:+7.2f}s arrive {e['kind']:8} {e['bytes']:6} B  thinks={e['thinks']}")
        else:
            print(f"  {rel:+7.2f}s served {e['kind']:8} (queued {e['queued']:.2f}s)")
    main = next((e for e in evs if e["kind"] == "main" and e["event"] == "arrive"), None)
    main_start = next((e for e in evs if e["kind"] == "main" and e["event"] == "start"), None)
    if main and main_start:
        print(f"  first main request on the wire {main['t'] - t_send:+.2f}s, answered after {main_start['t'] - t_send:.2f}s")

for app, binary in (("omp", os.environ.get("OMP_BIN", "omp")), ("omc", os.environ.get("OMC_BIN", os.path.join(ROOT, "build/omc")))):
    if args.only and args.only != app: continue
    run(app, binary)
if args.keep: print("kept", base)
else: shutil.rmtree(base, ignore_errors=True)
