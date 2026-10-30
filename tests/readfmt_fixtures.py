"""Generate fixtures for the read tool's structured formats (archives, SQLite, PDF, notebooks).

Usage: python3 -I tests/readfmt_fixtures.py <out-dir>
Test-time only; omc itself never runs Python for these formats.
"""
import gzip
import io
import json
import os
import shutil
import sqlite3
import sys
import tarfile
import zipfile
import zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def P(name):
    return os.path.join(out, name)


# ---------- archives ----------
with zipfile.ZipFile(P("t.zip"), "w") as z:
    z.writestr(zipfile.ZipInfo("hello.txt"), "hello zip\nline2\n", compress_type=zipfile.ZIP_DEFLATED)
    z.writestr("dir/", "")
    z.writestr(zipfile.ZipInfo("dir/inner.txt"), "inner text\n", compress_type=zipfile.ZIP_STORED)
    z.writestr(zipfile.ZipInfo("dir/sub/deep.md"), "# deep\n", compress_type=zipfile.ZIP_DEFLATED)
    z.writestr(zipfile.ZipInfo("bin.dat"), b"\x00\x01\x02binary\x00", compress_type=zipfile.ZIP_DEFLATED)
    z.writestr(zipfile.ZipInfo("many.txt"), "".join("row %d\n" % i for i in range(1, 51)),
               compress_type=zipfile.ZIP_DEFLATED)
    z.writestr(zipfile.ZipInfo("Zeta.txt"), "z\n", compress_type=zipfile.ZIP_DEFLATED)


def add_bytes(t, name, data, **kw):
    ti = tarfile.TarInfo(name)
    ti.size = len(data)
    for k, v in kw.items():
        setattr(ti, k, v)
    t.addfile(ti, io.BytesIO(data))


LONG = "long/" + "n" * 120 + ".txt"


def fill_tar(t):
    add_bytes(t, "a.txt", b"alpha tar\n")
    d = tarfile.TarInfo("d")
    d.type = tarfile.DIRTYPE
    t.addfile(d)
    add_bytes(t, "d/b.txt", b"bravo\n")
    ln = tarfile.TarInfo("d/link.txt")
    ln.type = tarfile.SYMTYPE
    ln.linkname = "../a.txt"
    t.addfile(ln)
    add_bytes(t, LONG, b"long name member\n")


with tarfile.open(P("t.tar"), "w", format=tarfile.GNU_FORMAT) as t:
    fill_tar(t)
with tarfile.open(P("t.tgz"), "w:gz", format=tarfile.PAX_FORMAT) as t:
    fill_tar(t)
shutil.copy(P("t.tgz"), P("t2.tar.gz"))
with gzip.open(P("notes.txt.gz"), "wb") as g:
    g.write(b"gzip single member\nsecond\n")

# ---------- SQLite ----------
for f in ("t.db", "wal.db", "wal.db-wal", "wal.db-shm", "live.db", "live.db-wal", "live.db-shm"):
    if os.path.exists(P(f)):
        os.remove(P(f))
db = sqlite3.connect(P("t.db"))
c = db.cursor()
c.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL, data BLOB, note TEXT)")
rows = []
for i in range(1, 3001):
    note = None if i % 7 == 0 else "note %d" % i
    if i == 42:
        note = "x" * 10000  # overflow pages
    rows.append((i, "user%04d" % i, i * 1.5, bytes([i % 256]) * 3, note))
c.executemany("INSERT INTO users VALUES (?,?,?,?,?)", rows)
c.execute("CREATE INDEX users_name ON users(name)")
c.execute("CREATE TABLE kv(k TEXT PRIMARY KEY, v INTEGER) WITHOUT ROWID")
c.executemany("INSERT INTO kv VALUES (?,?)", [("key%03d" % i, i * i) for i in range(500)])
c.execute("CREATE TABLE plain(a, b)")
c.executemany("INSERT INTO plain VALUES (?,?)", [(1, "one"), (2, "two\twith tab"), (3, "multi\nline")])
c.execute("ALTER TABLE plain ADD COLUMN c TEXT DEFAULT 'dflt'")
c.execute("INSERT INTO plain VALUES (4, 'four', 'given')")
c.execute("CREATE TABLE empty(x INTEGER)")
c.execute('CREATE TABLE "odd name"("col one" TEXT, n INTEGER)')
c.execute('INSERT INTO "odd name" VALUES (\'v\', 7)')
db.commit()
db.close()

# WAL database whose newest rows live only in the -wal file (copied while the writer is still open)
w = sqlite3.connect(P("live.db"))
w.execute("PRAGMA journal_mode=WAL")
w.execute("PRAGMA wal_autocheckpoint=0")
w.execute("CREATE TABLE events(id INTEGER PRIMARY KEY, what TEXT)")
w.commit()
w.execute("PRAGMA wal_checkpoint(TRUNCATE)")
w.executemany("INSERT INTO events(what) VALUES (?)", [("wal row %d" % i,) for i in range(1, 6)])
w.commit()
shutil.copy(P("live.db"), P("wal.db"))
shutil.copy(P("live.db-wal"), P("wal.db-wal"))
w.close()
for f in ("live.db", "live.db-wal", "live.db-shm"):
    if os.path.exists(P(f)):
        os.remove(P(f))


# ---------- PDF ----------
def pdf_classic(objs):
    """objs: list of bytes bodies for objects 1..n; returns a PDF with a classic xref table."""
    b = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offs = []
    for i, body in enumerate(objs, 1):
        offs.append(len(b))
        b += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    x = len(b)
    b += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offs:
        b += b"%010d 00000 n \n" % o
    b += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
    return bytes(b)


def stream(data, compress=True, extra=b""):
    if compress:
        data = zlib.compress(data)
        return b"<< /Length %d /Filter /FlateDecode %s>>\nstream\n" % (len(data), extra) + data + b"\nendstream"
    return b"<< /Length %d %s>>\nstream\n" % (len(data), extra) + data + b"\nendstream"


page1 = b"BT /F1 12 Tf 72 720 Td (Hello PDF world) Tj 0 -14 Td (Second line) Tj ET"
page2 = (b"BT /F1 12 Tf 72 720 Td [(Pa) 20 (ge) -400 (two)] TJ ET\n"
         b"q 1 0 0 1 0 -100 cm /Fm1 Do Q")
form = b"BT /F1 10 Tf 72 600 Td (From the form) Tj ET"
simple = pdf_classic([
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 /Resources << /Font << /F1 5 0 R >> /XObject << /Fm1 8 0 R >> >> >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 6 0 R >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents [7 0 R] >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    stream(page1),
    stream(page2),
    stream(form, extra=b"/Type /XObject /Subtype /Form /BBox [0 0 612 792] "),
])
open(P("simple.pdf"), "wb").write(simple)

blank = pdf_classic([
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>",
    stream(b"0 0 m 100 100 l S"),
])
open(P("blank.pdf"), "wb").write(blank)

# PDF 1.5: catalog/pages/fonts inside an object stream, xref stream, Type0 font with a ToUnicode CMap
text = "Ünïcode ✓ ok"
gids = list(range(3, 3 + len(text)))
tounicode = ("/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
             "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
             "%d beginbfchar\n" % len(text)
             + "".join("<%04X> <%s>\n" % (g, ch.encode("utf-16-be").hex().upper()) for g, ch in zip(gids, text))
             + "endbfchar\nendcmap CMapName currentdict /CMap defineresource pop end end\n").encode()
content = b"BT /F2 14 Tf 50 700 Td <" + "".join("%04X" % g for g in gids).encode() + b"> Tj ET"
in_stm = {
    1: b"<< /Type /Catalog /Pages 2 0 R >>",
    2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F2 4 0 R >> >> /Contents 7 0 R >>",
    4: b"<< /Type /Font /Subtype /Type0 /BaseFont /Fake /Encoding /Identity-H /DescendantFonts [5 0 R] /ToUnicode 6 0 R >>",
    5: b"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Fake /DW 600 /W [3 [700 650]] "
       b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> >>",
}
hdr, body = b"", b""
for num, ob in in_stm.items():
    hdr += b"%d %d " % (num, len(body))
    body += ob + b"\n"
stm_data = hdr + body
b = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
offs = {}
offs[6] = len(b)
b += b"6 0 obj\n" + stream(tounicode) + b"\nendobj\n"
offs[7] = len(b)
b += b"7 0 obj\n" + stream(content) + b"\nendobj\n"
offs[8] = len(b)
b += b"8 0 obj\n" + stream(stm_data, extra=b"/Type /ObjStm /N %d /First %d " % (len(in_stm), len(hdr))) + b"\nendobj\n"
offs[9] = len(b)
rows = [b"\x00\x00\x00\xff"]
for n in range(1, 10):
    if n in in_stm:
        rows.append(b"\x02\x00\x08" + bytes([list(in_stm).index(n)]))
    else:
        rows.append(b"\x01" + offs[n].to_bytes(2, "big") + b"\x00")
xdata = b"".join(rows)
b += (b"9 0 obj\n" + stream(xdata, extra=b"/Type /XRef /Size 10 /W [1 2 1] /Root 1 0 R ") + b"\nendobj\n")
b += b"startxref\n%d\n%%%%EOF\n" % offs[9]
open(P("objstm.pdf"), "wb").write(bytes(b))

with zipfile.ZipFile(P("docs.zip"), "w") as z:
    z.writestr("papers/simple.pdf", simple)

# ---------- notebook ----------
nb = {
    "cells": [
        {"cell_type": "markdown", "metadata": {}, "source": ["# Title\n", "Some *markdown*."]},
        {"cell_type": "code", "metadata": {}, "execution_count": 1, "outputs": [],
         "source": "x = 1\n# %% [code] cell:9\nprint(x)"},
        {"cell_type": "raw", "metadata": {}, "source": []},
    ],
    "metadata": {}, "nbformat": 4, "nbformat_minor": 5,
}
json.dump(nb, open(P("t.ipynb"), "w"), indent=1)
print("fixtures ok")
