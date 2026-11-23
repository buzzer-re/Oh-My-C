#!/usr/bin/env python3
"""The startup update check under a pty (tests/e2e/update.sh), against tests/e2e/mock_release.py serving 99.0.0:
- the check runs in the background: the welcome banner and the resumed transcript paint while the release lookup is
  still held back (DELAY), and the "Update Available" notice (omp showNewVersionNotification) arrives after them;
- startup.checkUpdate: false shows nothing and makes no request;
- update.auto: true installs 99.0.0 over the running binary through the detached worker and shows `→ v99.0.0 ✔ next
  launch` after the version label, with no notice.
Every launch runs a throwaway copy of omc (<dir>/bin/omc), never the build itself.
usage: update_tui.py <omc> <dir> (env: HOME, OMC_CODING_AGENT_DIR and the OMC_LATEST_URL / OMC_DOWNLOAD_URL mocks;
the agent dir needs a models.yml for mock/mock-1). Prints "update tui ok" or the first failing step."""
import fcntl, json, os, pty, select, shutil, signal, struct, subprocess, sys, termios, time
NOPRED = os.path.join(os.path.dirname(os.path.abspath(__file__)), "no-predictions.yml")  # composer.predictions off

omc, d = sys.argv[1:3]
copy = os.path.join(d, "bin", "omc")
rel = os.path.join(d, "rel")
agent = os.environ["OMC_CODING_AGENT_DIR"]
work = os.path.join(d, "work")
os.makedirs(work, exist_ok=True)
os.makedirs(agent, exist_ok=True)
os.chdir(work)
config = os.path.join(agent, "config.yml")
state = os.path.join(os.environ["HOME"], ".omc", "update", "auto-update.json")
env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1")
for k in ("TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT", "OMC_NO_UPDATE_CHECK"):
    env.pop(k, None)
pid = fd = None
out = bytearray()


def stop():
    global pid
    if pid:
        try:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except OSError:
            pass
    pid = None


def fail(step, detail=""):
    print(f"update tui FAIL {step}: {detail!r}")
    stop()
    sys.exit(1)


def fresh_copy():
    if os.path.exists(copy):
        os.unlink(copy)
    shutil.copy2(omc, copy)


def latest_requests():
    try:
        return sum(1 for l in open(os.path.join(rel, "requests.log")) if l.rstrip().endswith("/releases/latest"))
    except OSError:
        return 0


def launch(*args):
    global pid, fd, out
    stop()
    out = bytearray()
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(copy, [copy, "--config", NOPRED, "--model", "mock/mock-1", *args], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))


def pump(t):
    end = time.time() + t
    while True:
        r, _, _ = select.select([fd], [], [], max(0, end - time.time()))
        if not r:
            return
        try:
            data = os.read(fd, 65536)
        except OSError:
            return
        if not data:
            return
        out.extend(data)


def wait_out(needle, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.05)
        if needle.encode() in out:
            return True
    return False


# a session to resume, so the startup transcript has a message to replay
fresh_copy()
open(config, "w").close()
subprocess.run([copy, "--config", NOPRED, "--model", "mock/mock-1", "-p", "say:transcript-marker"], env=env, stdin=subprocess.DEVNULL,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)

# 1. the notice follows the transcript; the lookup (held back 3 s) never delays the first frame
with open(os.path.join(rel, "DELAY"), "w") as f:
    f.write("3")
before = latest_requests()
t0 = time.time()
launch("-c")
if not wait_out("transcript-marker", 10):
    fail("transcript", out[-800:])
first_frame = time.time() - t0
if first_frame > 2.5:
    fail("first frame waited for the check", first_frame)
if not wait_out("is available. Run: ", 10):
    fail("notice", out[-800:])
text = bytes(out)
if text.find(b"Update Available") < text.find(b"transcript-marker"):
    fail("notice before the transcript", text[-800:])
for needle in (b"\x1b[1m", b"Update Available", b"New version 99.0.0 is available. Run: ", b"omc update"):
    if needle not in text:
        fail(f"notice text {needle!r}", text[-800:])
if latest_requests() != before + 1:
    fail("one lookup", latest_requests() - before)
stop()
os.unlink(os.path.join(rel, "DELAY"))

# 2. startup.checkUpdate off: no notice, no request
with open(config, "w") as f:
    f.write("startup:\n  checkUpdate: false\n")
before = latest_requests()
launch()
if not wait_out("OMP equivalent", 10):
    fail("banner (check off)", out[-800:])
pump(2.0)
if b"Update Available" in out or latest_requests() != before:
    fail("check off", (bytes(out[-400:]), latest_requests() - before))
stop()

# 3. update.auto: the worker installs 99.0.0 over the running copy; the banner says it is ready, no notice
with open(config, "w") as f:
    f.write("update:\n  auto: true\n")
launch()
if not wait_out("next launch", 20):
    fail("auto-update ready", out[-800:])
if b"\xe2\x86\x92 v99.0.0" not in out:
    fail("auto-update arrow", out[-800:])
pump(0.5)
if b"Update Available" in out:
    fail("auto-update notice", out[-800:])
stop()
v = subprocess.run([copy, "--version"], capture_output=True, text=True, timeout=10).stdout.strip()
if v != "omc/99.0.0":
    fail("auto-update installed", v)
try:
    st = json.load(open(state))
except (OSError, ValueError) as e:
    fail("auto-update state", str(e))
if st.get("phase") != "done" or st.get("version") != "99.0.0" or st.get("target") != "99.0.0":
    fail("auto-update state", st)
open(config, "w").close()
print("update tui ok")
