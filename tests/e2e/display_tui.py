#!/usr/bin/env python3
"""WP-29 display options under a pty: inline images per protocol (PI_FORCE_IMAGE_PROTOCOL kitty / sixel / iterm2 /
off) on a resumed transcript with a grouped read image and a generic tool image, the cache-miss marker
(display.cacheMissMarker), Ctrl+Shift+O (display.hideToolActivity) and Ctrl+O while tool activity is hidden.
Escape shapes are omp v18.14.2's, captured under `script`.

usage: display_tui.py <omc> <work dir> <kitty|other|keys>   (agent dir from OMC_CODING_AGENT_DIR, like tests/e2e/run.sh)
Prints one "<step> ok" line per step, or "<step> FAIL: ..." and exits 1."""
import base64, json, os, pty, re, select, struct, subprocess, sys, time, zlib

omc, work, group = sys.argv[1], sys.argv[2], sys.argv[3]
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ANSI = re.compile(r"\x1b\[[0-9;?<>=:]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[PX^_][\s\S]*?\x1b\\|\x1b.")


def png(w, h):
    raw = b"".join(b"\x00" + bytes(v for x in range(w) for v in (x * 5 % 256, y * 10 % 256, 128)) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


b64 = base64.b64encode(png(48, 24)).decode()
spec = [{"user": "show the picture"},
        {"tool": "read", "args": {"path": "pic.png"}, "text": "Read image file [image/png]", "details": {},
         "images": [{"data": b64, "mimeType": "image/png"}]},
        {"user": "run it"},
        {"tool": "mytool", "args": {"x": 1}, "text": "made a picture", "details": {}, "images": [{"data": b64, "mimeType": "image/png"}]},
        {"say": "Done."}]
os.makedirs(work, exist_ok=True)
sp = os.path.join(work, "display-spec.json")
json.dump(spec, open(sp, "w"))
sess = os.path.join(work, "display-session.jsonl")
subprocess.run([sys.executable, "-I", os.path.join(root, "tests/tui/mksession.py"), sp, work, sess], check=True)
# a warm cache that goes cold on the last turn: the marker shows after it
lines = [json.loads(l) for l in open(sess)]
asst = [e for e in lines if e.get("type") == "message" and e["message"].get("role") == "assistant"]
for a in asst[:-1]:
    a["message"]["usage"].update({"input": 10, "cacheRead": 50000, "cacheWrite": 0})
asst[-1]["message"]["usage"].update({"input": 900, "cacheRead": 0, "cacheWrite": 50000})
with open(sess, "w") as f:
    for e in lines:
        f.write(json.dumps(e) + "\n")


def run(proto, keys=(), expect=(), settle=4.5, extra_env=None, config=None):
    cfgp = os.path.join(os.environ["OMC_CODING_AGENT_DIR"], "config.yml")
    saved = open(cfgp).read() if os.path.exists(cfgp) else None
    if config:
        open(cfgp, "a").write(("\n" if saved and not saved.endswith("\n") else "") + config)
    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1",
                   PI_FORCE_IMAGE_PROTOCOL=proto, COLUMNS="100", LINES="40")
        for k in ("TMUX", "STY", "ZELLIJ", "TERM_PROGRAM", "KITTY_WINDOW_ID", "PI_KITTY_PLACEHOLDERS"):
            env.pop(k, None)
        env.update(extra_env or {})
        os.chdir(work)
        os.execvpe(omc, [omc, "--model", "mock/mock-1", "-r", sess], env)
    import fcntl, termios
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 100, 0, 0))
    raw = b""

    def pump(secs):
        nonlocal raw
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    chunk = os.read(fd, 1 << 20)
                except OSError:
                    return
                if not chunk:
                    return
                raw += chunk

    try:
        pump(settle)
        for k in keys:
            os.write(fd, k.encode())
            pump(1.2)
    finally:
        try:
            os.write(fd, b"\x04")
            pump(0.8)
            os.kill(pid, 9)
        except OSError:
            pass
        try:
            os.waitpid(pid, 0)
        except OSError:
            pass
        if config:
            if saved is None:
                os.unlink(cfgp)
            else:
                open(cfgp, "w").write(saved)
    if os.environ.get("DISPLAY_TUI_DUMP"):  # debugging: the raw stream of each run
        open("%s.%s.raw" % (os.environ["DISPLAY_TUI_DUMP"], proto), "wb").write(raw)
    return raw.decode("utf-8", "replace")


def check(name, ok, detail=""):
    if not ok:
        print(name, "FAIL:", detail[-1500:], flush=True)
        sys.exit(1)
    print(name, "ok", flush=True)


if group == "kitty":
    out = run("kitty", config="display:\n  cacheMissMarker: true\n")
    tx = re.findall(r"\x1b_Ga=t,f=100,q=2,i=(\d+);iVBOR", out)
    pl = re.findall(r"\x1b7\x1b\[19A\x1b_Ga=p,q=2,C=1,i=(\d+),p=(\d+),c=80,r=20\x1b\\\x1b8", out)
    check("kitty transmit once", len(set(tx)) == 2 and len(tx) == 2, repr(tx))
    check("kitty placement", len(pl) >= 2 and set(i for i, _ in pl) == set(tx), repr(pl))
    check("cache miss", re.search(r"cache miss \S* 51K tokens", ANSI.sub("", out)) is not None, ANSI.sub("", out))
    out = run("kitty", extra_env={"PI_KITTY_PLACEHOLDERS": "1"})
    check("kitty placeholders", re.search(r"\x1b_Ga=p,U=1,q=2,i=(\d+),p=\1,c=80,r=20\x1b\\\x1b\[38;2;\d+;\d+;\d+m\x1b\[58:2::", out)
          is not None and "\U0010eeee̅̅" in out, out[:500])
if group == "other":
    out = run("sixel")
    check("sixel", re.search(r"\x1b7\x1b\[19A\x1bP9;1;0q\"1;1;720;360#0;2;", out) is not None, out[:500])
    out = run("iterm2")
    check("iterm2", "\x1b7\x1b[19A\x1b]1337;File=inline=1;width=80;height=20:iVBOR" in out, out[:500])
    out = run("off")
    plain = ANSI.sub("", out)
    check("fallback text", "[Image: image/png]" in plain and "[Image: [image/png] 48x24]" in plain, plain)
if group == "keys":
    out = run("off", keys=["\x1b[111;6u", "\x0f", "\x1b[111;6u"])
    plain = ANSI.sub("", out)
    check("hide tool activity", "Tool activity: hidden" in plain, plain)
    check("expand while hidden", "Tool activity is hidden — show it with" in plain and "or /settings before expanding" in plain, plain)
    check("show tool activity", "Tool activity: visible" in plain, plain)
    out = run("off", config="tui:\n  mouse: true\n")
    check("mouse capture", "\x1b[?1000h\x1b[?1003h\x1b[?1006h" in out, out[:400])
