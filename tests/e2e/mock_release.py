#!/usr/bin/env python3
"""Dev-only mock of omc's GitHub releases for `omc update`, the startup update check and update.auto
(tests/e2e/update.sh, tests/e2e/update_tui.py). Never contacts the network.

  mock_release.py make <dir> <version> <os-arch> [<binary>]   write <dir>/v<version>/omc-<v>-<os-arch>.tar.gz and
      SHA256SUMS (like .github/workflows/release.yml). The archive's omc-<v>-<os-arch>/omc is <binary>, or a small sh
      script that prints `omc/<version>` for --version.
  mock_release.py serve <port> <dir>                           serve it:
      GET /<owner>/<repo>/releases/latest   302 to /<owner>/<repo>/releases/tag/v<version in <dir>/LATEST>
                                            (302 to /<owner>/<repo>/releases when LATEST is absent or empty: no release),
                                            after <dir>/DELAY seconds when that file exists
      GET /download/v<v>/<asset>            302 to /assets/v<v>/<asset> (the CDN hop GitHub makes)
      GET /assets/v<v>/<asset>              the file under <dir>, else 404
  Every request is appended to <dir>/requests.log as "<method> <path>"."""
import hashlib, io, os, sys, tarfile, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def make(d, version, plat, binary=None):
    rel = os.path.join(d, "v" + version)
    os.makedirs(rel, exist_ok=True)
    top = "omc-%s-%s" % (version, plat)
    data = open(binary, "rb").read() if binary else (
        "#!/bin/sh\ncase \"$1\" in --version|-v) echo omc/%s ;; *) echo fake omc %s ;; esac\n" % (version, version)).encode()
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.USTAR_FORMAT) as t:
        for name, body, mode in ((top, None, 0o755), (top + "/omc", data, 0o755), (top + "/README.md", b"readme\n", 0o644)):
            info = tarfile.TarInfo(name)
            info.mode = mode
            info.mtime = 1700000000
            if body is None:
                info.type = tarfile.DIRTYPE
                t.addfile(info)
            else:
                info.size = len(body)
                t.addfile(info, io.BytesIO(body))
    asset = top + ".tar.gz"
    with open(os.path.join(rel, asset), "wb") as f:
        f.write(buf.getvalue())
    sums = ["%s  %s" % (hashlib.sha256(open(os.path.join(rel, n), "rb").read()).hexdigest(), n)
            for n in sorted(os.listdir(rel)) if n != "SHA256SUMS"]
    with open(os.path.join(rel, "SHA256SUMS"), "w") as f:
        f.write("\n".join(sums) + "\n")


def serve(port, d):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def reply(self, code, body=b"", headers=()):
            self.send_response(code)
            for k, v in headers:
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def do_GET(self):
            with open(os.path.join(d, "requests.log"), "a") as f:
                f.write("%s %s\n" % (self.command, self.path))
            p = self.path.split("?")[0]
            if p.endswith("/releases/latest"):
                try:
                    latest = open(os.path.join(d, "LATEST")).read().strip()
                except OSError:
                    latest = ""
                try:
                    time.sleep(float(open(os.path.join(d, "DELAY")).read().strip() or 0))
                except (OSError, ValueError):
                    pass
                base = p[: -len("/latest")]
                loc = "%s/tag/v%s" % (base, latest) if latest else base
                return self.reply(302, b"", [("Location", loc)])
            if p.startswith("/download/"):
                return self.reply(302, b"", [("Location", "/assets/" + p[len("/download/"):])])
            if p.startswith("/assets/"):
                rel = os.path.normpath(p[len("/assets/"):])
                f = os.path.join(d, rel)
                if rel.startswith("..") or not os.path.isfile(f):
                    return self.reply(404, b"Not Found")
                return self.reply(200, open(f, "rb").read(), [("Content-Type", "application/octet-stream")])
            self.reply(404, b"Not Found")

        do_HEAD = do_GET

    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()


if __name__ == "__main__":
    if sys.argv[1] == "make":
        make(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5] if len(sys.argv) > 5 else None)
    else:
        serve(int(sys.argv[2]), sys.argv[3])
