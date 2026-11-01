"""Generate image codec fixtures for tests/test_img*.c.

Usage: python3 -I tests/img_fixtures.py <out-dir>
Test-time only (omc itself never runs Python). Every fixture <name>.<ext> gets a reference raster
<name>.rgba: 8-byte header (u32le width, u32le height) followed by width*height RGBA8 pixels.
PNG references are computed here from the source samples (exact: 16-bit samples keep the high byte);
JPEG / GIF / WebP references come from Pillow's decoder (JPEG and lossy WebP are compared with a
tolerance by the C tests, GIF and lossless WebP exactly).
"""
import io
import os
import random
import struct
import sys
import zlib

from PIL import Image

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rnd = random.Random(1234)


def P(name):
    return os.path.join(out, name)


def write_ref(name, w, h, rgba):
    with open(P(name + ".rgba"), "wb") as f:
        f.write(struct.pack("<II", w, h))
        f.write(bytes(rgba))


def pil_ref(name, im):
    im = im.convert("RGBA")
    write_ref(name, im.size[0], im.size[1], im.tobytes())


# ---------------------------------------------------------------- PNG (hand-written encoder)
def chunk(t, data):
    c = struct.pack(">I", len(data)) + t + data
    return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_rows(rows, bpp):
    """Apply filter type (row index % 5) to each scanline (exercises every filter)."""
    outb = bytearray()
    prev = bytes(len(rows[0])) if rows else b""
    for y, row in enumerate(rows):
        ft = y % 5
        f = bytearray(len(row))
        for i in range(len(row)):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            x = row[i]
            if ft == 0:
                v = x
            elif ft == 1:
                v = x - a
            elif ft == 2:
                v = x - b
            elif ft == 3:
                v = x - ((a + b) >> 1)
            else:
                v = x - paeth(a, b, c)
            f[i] = v & 0xFF
        outb.append(ft)
        outb += f
        prev = row
    return bytes(outb)


def pack_row(samples, depth):
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    outb = bytearray()
    per = 8 // depth
    for i in range(0, len(samples), per):
        v = 0
        grp = samples[i:i + per]
        for j in range(per):
            v = (v << depth) | (grp[j] if j < len(grp) else 0)
        outb.append(v)
    return bytes(outb)


CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def write_png(name, w, h, ct, depth, px, palette=None, trns=None, interlace=False, idat_split=0):
    """px: list of rows, each a list of per-pixel tuples of raw samples."""
    ch = CHANNELS[ct]
    bpp = max(1, ch * depth // 8)

    def scan(rows_px):
        return [pack_row([s for p in r for s in p], depth) for r in rows_px]

    if not interlace:
        raw = filter_rows(scan(px), bpp)
    else:
        raw = b""
        for x0, y0, dx, dy in ADAM7:
            sub = [[px[y][x] for x in range(x0, w, dx)] for y in range(y0, h, dy)]
            sub = [r for r in sub if r]
            if sub:
                raw += filter_rows(scan(sub), bpp)
    z = zlib.compress(raw, 9)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, 1 if interlace else 0))
    if palette:
        data += chunk(b"PLTE", b"".join(bytes(c) for c in palette))
    if trns is not None:
        data += chunk(b"tRNS", trns)
    data += chunk(b"tEXt", b"Comment\x00omc fixture")
    if idat_split:
        for i in range(0, len(z), idat_split):
            data += chunk(b"IDAT", z[i:i + idat_split])
    else:
        data += chunk(b"IDAT", z)
    data += chunk(b"IEND", b"")
    with open(P(name + ".png"), "wb") as f:
        f.write(data)
    return data


def png_ref(name, w, h, ct, depth, px, palette=None, trns=None):
    rgba = bytearray()
    maxv = (1 << depth) - 1
    tkey = None
    if trns is not None and ct in (0, 2):
        tkey = struct.unpack(">H", trns[:2])[0] if ct == 0 else struct.unpack(">HHH", trns[:6])
    for row in px:
        for p in row:
            if ct == 3:
                r, g, b = palette[p[0]]
                a = trns[p[0]] if trns is not None and p[0] < len(trns) else 255
                rgba += bytes((r, g, b, a))
                continue

            def to8(s):
                if depth == 16:
                    return s >> 8
                if depth == 8:
                    return s
                return s * 255 // maxv
            if ct == 0:
                g = to8(p[0])
                a = 0 if tkey is not None and p[0] == tkey else 255
                rgba += bytes((g, g, g, a))
            elif ct == 4:
                g = to8(p[0])
                rgba += bytes((g, g, g, to8(p[1])))
            elif ct == 2:
                a = 0 if tkey is not None and tuple(p) == tuple(tkey) else 255
                rgba += bytes((to8(p[0]), to8(p[1]), to8(p[2]), a))
            else:
                rgba += bytes(to8(s) for s in p)
    write_ref(name, w, h, rgba)


def gen_png(name, w, h, ct, depth, interlace=False, palette_n=0, trns=False, idat_split=0):
    ch = CHANNELS[ct]
    maxv = (1 << depth) - 1
    palette = None
    tr = None
    if ct == 3:
        n = palette_n or (1 << depth)
        palette = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(n)]
        px = [[(rnd.randrange(n),) for x in range(w)] for y in range(h)]
        if trns:
            tr = bytes(rnd.randrange(256) for _ in range(max(1, n // 2)))
    else:
        # smooth gradients + noise so every filter type has work to do
        px = []
        for y in range(h):
            row = []
            for x in range(w):
                row.append(tuple(min(maxv, ((x * 7 + y * 3 + c * 50) * maxv // (w * 7 + h * 3 + 150)) ^ (rnd.randrange(4) if maxv > 3 else 0)) for c in range(ch)))
            px.append(row)
        if trns:
            key = px[h // 2][w // 2]
            tr = struct.pack(">H", key[0]) if ct == 0 else struct.pack(">HHH", *key)
    write_png(name, w, h, ct, depth, px, palette, tr, interlace, idat_split)
    png_ref(name, w, h, ct, depth, px, palette, tr)


for depth in (1, 2, 4, 8, 16):
    gen_png(f"png_g{depth}", 37, 23, 0, depth)
gen_png("png_g8_trns", 31, 17, 0, 8, trns=True)
gen_png("png_g16_trns", 31, 17, 0, 16, trns=True)
for depth in (8, 16):
    gen_png(f"png_ga{depth}", 29, 19, 4, depth)
    gen_png(f"png_rgb{depth}", 41, 27, 2, depth)
    gen_png(f"png_rgba{depth}", 33, 21, 6, depth)
gen_png("png_rgb8_trns", 30, 20, 2, 8, trns=True)
for depth in (1, 2, 4, 8):
    gen_png(f"png_p{depth}", 35, 22, 3, depth)
gen_png("png_p8_trns", 35, 22, 3, 8, palette_n=200, trns=True)
gen_png("png_p4_short", 35, 22, 3, 4, palette_n=5)
# Adam7: odd sizes cover empty passes
for (w, h) in ((1, 1), (3, 2), (9, 7), (33, 17)):
    gen_png(f"png_i_rgba8_{w}x{h}", w, h, 6, 8, interlace=True)
gen_png("png_i_g1", 19, 13, 0, 1, interlace=True)
gen_png("png_i_p2", 21, 11, 3, 2, interlace=True)
gen_png("png_i_rgb16", 17, 15, 2, 16, interlace=True)
gen_png("png_split_idat", 64, 48, 2, 8, idat_split=97)

# Pillow-written PNGs (its own encoder: different filter choices / compression)
im = Image.new("RGB", (120, 80))
for y in range(80):
    for x in range(120):
        im.putpixel((x, y), ((x * 2) & 255, (y * 3) & 255, (x * y) & 255))
im.save(P("png_pil_rgb.png"))
pil_ref("png_pil_rgb", im)

# corrupt / truncated PNGs (must fail cleanly)
full = open(P("png_rgb8.png"), "rb").read()
open(P("bad_png_trunc.png"), "wb").write(full[: len(full) // 2])
bad = bytearray(full)
idat = full.index(b"IDAT")
for i in range(idat + 8, min(idat + 40, len(bad) - 16)):
    bad[i] ^= 0x5A
open(P("bad_png_corrupt.png"), "wb").write(bytes(bad))
open(P("bad_png_hugedim.png"), "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 100000, 100000, 8, 6, 0, 0, 0)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------- JPEG (Pillow encoder)
def photo(w, h, seed):
    r = random.Random(seed)
    im = Image.new("RGB", (w, h))
    pix = im.load()
    cx, cy = r.randrange(w), r.randrange(h)
    for y in range(h):
        for x in range(w):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            pix[x, y] = (int(128 + 100 * ((x / w) - 0.5)) & 255, int(d * 2) & 255, (x ^ y) & 255)
    return im


base = photo(97, 61, 7)  # odd size: partial MCUs
for sub, tag in ((0, "444"), (1, "422"), (2, "420")):
    for prog in (False, True):
        n = f"jpg_{tag}{'_prog' if prog else ''}"
        base.save(P(n + ".jpg"), quality=90, subsampling=sub, progressive=prog)
        pil_ref(n, Image.open(P(n + ".jpg")))
base.convert("L").save(P("jpg_gray.jpg"), quality=85)
pil_ref("jpg_gray", Image.open(P("jpg_gray.jpg")))
base.convert("L").save(P("jpg_gray_prog.jpg"), quality=85, progressive=True)
pil_ref("jpg_gray_prog", Image.open(P("jpg_gray_prog.jpg")))
base.save(P("jpg_restart.jpg"), quality=80, subsampling=2, restart_marker_blocks=3)
pil_ref("jpg_restart", Image.open(P("jpg_restart.jpg")))
base.save(P("jpg_restart_prog.jpg"), quality=80, subsampling=2, progressive=True, restart_marker_rows=1)
pil_ref("jpg_restart_prog", Image.open(P("jpg_restart_prog.jpg")))
base.convert("CMYK").save(P("jpg_cmyk.jpg"), quality=95)
pil_ref("jpg_cmyk", Image.open(P("jpg_cmyk.jpg")))
base.save(P("jpg_q20.jpg"), quality=20, optimize=True)
pil_ref("jpg_q20", Image.open(P("jpg_q20.jpg")))
photo(640, 480, 3).save(P("jpg_large_prog.jpg"), quality=75, progressive=True)
pil_ref("jpg_large_prog", Image.open(P("jpg_large_prog.jpg")))
full = open(P("jpg_420.jpg"), "rb").read()
open(P("bad_jpg_trunc.jpg"), "wb").write(full[: len(full) // 3])

# ---------------------------------------------------------------- GIF
g = base.convert("P", palette=Image.ADAPTIVE, colors=64)
g.save(P("gif_basic.gif"))
pil_ref("gif_basic", Image.open(P("gif_basic.gif")))
g.save(P("gif_interlaced.gif"), interlace=True)
pil_ref("gif_interlaced", Image.open(P("gif_interlaced.gif")))
gt = g.copy()
gt.info["transparency"] = 3
gt.save(P("gif_trns.gif"), transparency=3)
pil_ref("gif_trns", Image.open(P("gif_trns.gif")))
frames = [photo(40, 30, s).convert("P", palette=Image.ADAPTIVE, colors=16) for s in (1, 2, 3)]
frames[0].save(P("gif_anim.gif"), save_all=True, append_images=frames[1:], duration=100, loop=0)
pil_ref("gif_anim", Image.open(P("gif_anim.gif")))  # first frame
Image.new("1", (13, 9), 1).save(P("gif_2color.gif"))
pil_ref("gif_2color", Image.open(P("gif_2color.gif")))

# ---------------------------------------------------------------- WebP (libwebp via Pillow)
big = photo(203, 129, 11)
big.save(P("webp_lossy.webp"), quality=80)
pil_ref("webp_lossy", Image.open(P("webp_lossy.webp")))
big.save(P("webp_lossy_q30.webp"), quality=30)
pil_ref("webp_lossy_q30", Image.open(P("webp_lossy_q30.webp")))
big.save(P("webp_lossless.webp"), lossless=True)
pil_ref("webp_lossless", Image.open(P("webp_lossless.webp")))
rgba = big.convert("RGBA")
apx = rgba.load()
for y in range(rgba.size[1]):
    for x in range(rgba.size[0]):
        r_, g_, b_, _ = apx[x, y]
        apx[x, y] = (r_, g_, b_, (x * 255 // rgba.size[0]) if y % 7 else 0)
rgba.save(P("webp_lossy_alpha.webp"), quality=80)
pil_ref("webp_lossy_alpha", Image.open(P("webp_lossy_alpha.webp")))
rgba.save(P("webp_lossless_alpha.webp"), lossless=True)
pil_ref("webp_lossless_alpha", Image.open(P("webp_lossless_alpha.webp")))
# palette-heavy (exercises the color-indexing transform) and tiny images
pal = base.convert("P", palette=Image.ADAPTIVE, colors=8).convert("RGB")
pal.save(P("webp_lossless_pal.webp"), lossless=True)
pil_ref("webp_lossless_pal", Image.open(P("webp_lossless_pal.webp")))
Image.new("RGB", (1, 1), (10, 200, 30)).save(P("webp_tiny.webp"), lossless=True)
pil_ref("webp_tiny", Image.open(P("webp_tiny.webp")))
photo(17, 9, 5).save(P("webp_small_lossy.webp"), quality=60)
pil_ref("webp_small_lossy", Image.open(P("webp_small_lossy.webp")))
full = open(P("webp_lossy.webp"), "rb").read()
open(P("bad_webp_trunc.webp"), "wb").write(full[: len(full) // 2])

# Raw source rasters for encoder round trips (decoded by the C side's own decoder and by Pillow)
for name, im in (("src_photo", photo(160, 120, 21)), ("src_ui", None)):
    if im is None:
        im = Image.new("RGB", (180, 100), (30, 30, 36))
        from PIL import ImageDraw
        d = ImageDraw.Draw(im)
        for i in range(40):
            x, y = rnd.randrange(180), rnd.randrange(100)
            d.rectangle([x, y, x + rnd.randrange(5, 60), y + rnd.randrange(3, 20)], fill=(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)))
            d.text((rnd.randrange(170), rnd.randrange(90)), "omc", fill=(220, 220, 220))
    pil_ref(name, im)
print("ok")
