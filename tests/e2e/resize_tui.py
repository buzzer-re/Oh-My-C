#!/usr/bin/env python3
"""A settled terminal resize refreshes native history per tui.resizeScrollback (omp tui.ts resize transaction). Under
a pty with no multiplexer the burst borrows the alternate screen, then: rebuild (the default) erases history (ED2, then
ED3) and replays every row in the write that leaves the borrow; append replays every row below retained history without
erasing; preserve only repaints the screen.
usage: resize_tui.py <omc> <models.yml> <dir> <mode|default>   prints "resize <mode> ok" or what went wrong"""
import fcntl, os, pty, select, shutil, signal, struct, sys, termios, time

omc, models, d, mode = sys.argv[1:5]
agent = os.path.join(d, "agent-" + mode)
os.makedirs(agent, exist_ok=True)
shutil.copy(models, os.path.join(agent, "models.yml"))
cfg = os.path.join(agent, "config.yml")
if mode != "default":
    with open(cfg, "w") as f:
        f.write("tui:\n  resizeScrollback: %s\n" % mode)
elif os.path.exists(cfg):
    os.unlink(cfg)
env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", OMC_CODING_AGENT_DIR=agent)
for k in ("TMUX", "STY", "ZELLIJ", "TERM_PROGRAM", "PI_TUI_RESIZE_IN_PLACE", "PI_TUI_RESIZE_SCROLLBACK"):
    env.pop(k, None)
pid, fd = pty.fork()
if pid == 0:
    fcntl.ioctl(sys.stdout.fileno(), termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    os.execve(omc, [omc, "--model", "mock/mock-1", "--no-session"], env)
out = bytearray()


def pump(t):
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], max(0, end - time.time()))
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return
            if not data:
                return
            out.extend(data)


def wait_for(needle, t):
    end = time.time() + t
    while time.time() < end and needle not in out:
        pump(0.1)
    return needle in out


def say(text):
    os.write(fd, text.encode())
    pump(0.3)
    os.write(fd, b"\r")


try:
    ok = wait_for("╰".encode(), 15)
    pump(0.5)
    long = " ".join("word%d lorem ipsum" % i for i in range(120))
    say("say:FIRSTMARK " + long)
    ok = ok and wait_for(b"word119 lorem ipsum", 10)
    say("say:second " + long)
    pump(2)
    mark = len(out)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
    pump(1.5)
    tail = bytes(out[mark:])
    enter, leave = tail.find(b"\x1b[?1049h"), tail.find(b"\x1b[?1049l")
    clear = tail.find(b"\x1b[H\x1b[2J\x1b[3J")
    replay = tail.find(b"FIRSTMARK", max(leave, 0))
    want = mode if mode != "default" else "rebuild"
    if not ok:
        print("resize %s: the answers never finished" % mode)
    elif enter < 0 or leave < enter:
        print("resize %s: expected the burst to borrow the alternate screen (enter %d, leave %d)" % (mode, enter, leave))
    elif want == "rebuild" and not (leave < clear < replay):
        print("resize %s: expected the exit, then ED2+ED3, then the replay (%d, %d, %d)" % (mode, leave, clear, replay))
    elif want == "append" and (clear >= 0 or replay < 0):
        print("resize %s: expected a replay below history and no erase (clear %d, replay %d)" % (mode, clear, replay))
    elif want == "preserve" and (clear >= 0 or replay >= 0):
        print("resize %s: expected a screen-only repaint (clear %d, replay %d)" % (mode, clear, replay))
    else:
        print("resize %s ok" % mode)
finally:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
