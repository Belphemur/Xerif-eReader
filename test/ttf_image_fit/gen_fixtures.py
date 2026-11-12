#!/usr/bin/env python3
"""Configure-time fixture generator for the TTF image fit suite (stdlib only).

Builds a minimal EPUB carrying one PROGRESSIVE JPEG (DC-only first scan) —
the format TJpgDec cannot decode, so the SDK's DC-path renderer is the only
host-exercisable implementation — plus the gray pattern PNG/BMP twins. The
DC scan uses quantizer 8, so DC coefficient = 8*(block_avg - 128) exactly and
the decoded gray equals the block average: tonal pins stay byte-exact.

Baseline JPEG/PNG fixtures come from the committed test/epubs books; this
generator only adds what no committed fixture carries.
"""
import os
import struct
import sys
import zipfile
import zlib

SRC = os.path.dirname(os.path.abspath(__file__))

W, H = 800, 1200
OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)


def pattern(x, y):
    # Left half: horizontal gradient. Right half: dark top / light bottom.
    if x < W // 2:
        return (x * 255) // (W // 2 - 1)
    return 30 if y < H // 2 else 235


pix = [[pattern(x, y) for x in range(W)] for y in range(H)]


def write_progressive_jpeg(path, source=None, corrupt_tail=False):
    global pix
    src = source if source is not None else pix
    def avg(x0, y0):
        s = 0
        for y in range(y0, min(y0 + 8, H)):
            row = src[y]
            for x in range(x0, min(x0 + 8, W)):
                s += row[x]
        n = (min(x0 + 8, W) - x0) * (min(y0 + 8, H) - y0)
        return s // n

    bw, bh = (W + 7) // 8, (H + 7) // 8
    out = bytearray(b"\xff\xd8")
    # DQT: quantizer 8 for DC (id 0) — DC coefficient = 8*(avg-128), q/8 exact.
    out += b"\xff\xdb" + struct.pack(">H", 3 + 64) + b"\x00" + bytes([8] * 64)
    # SOF2: progressive, 8-bit, one 1x1-sampled component.
    out += b"\xff\xc2" + struct.pack(">HBHHB", 8 + 3, 8, H, W, 1) + bytes([1, 0x11, 0])
    # DHT: DC table 0 — 12 category symbols (0..11), all at code length 5.
    counts = [0] * 16
    counts[4] = 12
    out += b"\xff\xc4" + struct.pack(">H", 2 + 1 + 16 + 12) + b"\x00" + bytes(counts) + bytes(range(12))
    # SOS: first scan, DC only (Ss=Se=0, Ah=0, Al=0).
    out += b"\xff\xda" + struct.pack(">H", 6 + 2) + b"\x01" + bytes([1, 0x00]) + b"\x00\x00\x00"

    bits = []

    def put(val, n):
        for i in range(n - 1, -1, -1):
            bits.append((val >> i) & 1)

    pred = 0
    for by in range(bh):
        for bx in range(bw):
            dc = avg(bx * 8, by * 8)
            diff = dc - 128 - pred  # DC quantized value: 8*(avg-128)/8
            pred = dc - 128
            s = 0 if diff == 0 else abs(diff).bit_length()
            put(s, 5)
            if s:
                if diff < 0:
                    diff += (1 << s) - 1
                put(diff & ((1 << s) - 1), s)
    while len(bits) % 8:
        bits.append(1)
    for i in range(0, len(bits), 8):
        b = 0
        for j in range(8):
            b = (b << 1) | bits[i + j]
        out.append(b)
        if b == 0xFF:
            out.append(0x00)
    if corrupt_tail:
        # A DHT whose table counts overflow (257 > 256 symbols) AFTER the
        # first scan's entropy data. ProgressiveJpeg's full decode walks the
        # marker between scans and refuses the stream (ParseError), while the
        # DC-only fallback renderer stops at the first scan — the SDK's
        # DC-fallback shape (renderJpegProgressiveFull returns OutOfMemory,
        # the 1/8-scale blocksW x blocksH dims hook fires). Used by the
        # worker dcFallback pin.
        out += b"\xff\xc4" + struct.pack(">H", 2 + 17) + b"\x00" + bytes([255, 2] + [0] * 14)
    out += b"\xff\xd9"
    atomic_write(path, out)


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def atomic_write(path, data):
    # kody rule: a failed generation must not leave a partial fixture behind.
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def write_png(path):
    import zlib
    rows = bytearray()
    for y in range(H):
        rows.append(0)
        rows.extend(pix[y])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6))
    png += chunk(b"IEND", b"")
    atomic_write(path, png)


write_progressive_jpeg(os.path.join(OUT, "prog.jpg"))
# All-dark progressive twin: the coverage fixture (legitimately white image
# regions paint no ink in the BW coverage check).
dark_pix = [[10] * W for _ in range(H)]
write_progressive_jpeg(os.path.join(OUT, "dark_prog.jpg"), dark_pix)
# DC-fallback twin: valid first scan + corrupt marker between scans, so the
# SDK's full decode fails and the DC fallback reports 1/8-scale dims.
write_progressive_jpeg(os.path.join(OUT, "dcfallback_prog.jpg"), dark_pix, corrupt_tail=True)
write_png(os.path.join(OUT, "pattern.png"))
def write_gif(path):
    """Minimal 96x144 GIF87a: 2-color palette, every pixel index 0 (dark),
    encoded with a real LZW emit loop (constant-index runs grow the standard
    string table) so stock decoders reproduce the pixels exactly."""
    GW, GH = 96, 144
    out = bytearray(b"GIF87a")
    out += struct.pack("<HH", GW, GH)
    out += bytes([0x80, 0, 0])  # GCT present, size bits 000 = 2 colors; bg 0; aspect 0
    out += bytes([10, 10, 10, 250, 250, 250])  # palette: dark, white
    out += b","  # image descriptor
    out += struct.pack("<HHHH", 0, 0, GW, GH)
    out += bytes([0])  # no local table, not interlaced
    out += bytes([2])  # LZW min code size

    min_code_size = 2
    clear = 1 << min_code_size       # 4
    end = clear + 1                  # 5
    bits = []

    def put(val, n):
        # GIF LZW packs codes LSB-first into the byte stream.
        for i in range(n):
            bits.append((val >> i) & 1)

    code_size = min_code_size + 1
    next_code = end + 1              # first assignable table code
    table = {}                       # (prefix_code, index) -> code
    put(clear, code_size)
    prefix = 0                       # first pixel: palette index 0
    for _ in range(GW * GH - 1):
        key = (prefix, 0)
        if key in table:
            prefix = table[key]
            continue
        put(prefix, code_size)
        table[key] = next_code
        next_code += 1
        if next_code == (1 << code_size) + 1 and code_size < 12:
            # Decoder-side convention: the width grows one code late.
            code_size += 1
        if next_code >= 4096:
            put(clear, code_size)
            table.clear()
            next_code = end + 1
            code_size = min_code_size + 1
        prefix = 0
    put(prefix, code_size)
    put(end, code_size)
    while len(bits) % 8:
        bits.append(0)
    data = bytearray()
    for i in range(0, len(bits), 8):
        b = 0
        for j in range(8):
            b |= bits[i + j] << j  # LZW packs LSB-first
        data.append(b)
    # Sub-blocks of <=255 bytes, then the block terminator.
    for i in range(0, len(data), 255):
        part = data[i:i + 255]
        out.append(len(part))
        out += part
    out.append(0)
    out += b";"
    atomic_write(path, out)


write_gif(os.path.join(OUT, "dark.gif"))


def write_small_bmp(path, w=64, h=48, gray=137):
    # 24-bit bottom-up BMP — a real BMP entry for the EPUB fixture.
    row_bytes = (w * 3 + 3) // 4 * 4
    payload = 40 + row_bytes * h
    fh = struct.pack("<2sIHHI", b"BM", 14 + payload, 0, 0, 14 + 40)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, payload, 2835, 2835, 0, 0)
    data = bytearray()
    for _ in range(h):
        row = bytearray(bytes((gray, gray, gray)) * w)
        row += b"\0" * (row_bytes - len(row))
        data += row
    atomic_write(path, fh + dib + bytes(data))


def write_dark_png(path):
    # All-dark 800x1200 PNG (value 10) — coverage fixtures must be dark, since
    # legitimately white image regions paint no ink in the BW coverage check.
    rows = bytearray()
    for _ in range(H):
        rows.append(0)
        rows.extend(bytes([10]) * W)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")
    import os
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(png)
    os.replace(tmp, path)


write_small_bmp(os.path.join(OUT, "small.bmp"))
write_dark_png(os.path.join(OUT, "dark.png"))

opf = """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:ttf-image-fit-0001</dc:identifier>
    <dc:title>Fit Fixtures</dc:title><dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="ch1" href="text/ch1.xhtml" media-type="application/xhtml+xml"/>
    <item id="i1" href="images/prog.jpg" media-type="image/jpeg"/>
    <item id="i6" href="images/dark_prog.jpg" media-type="image/jpeg"/>
    <item id="i8" href="images/dcfallback_prog.jpg" media-type="image/jpeg"/>
    <item id="i2" href="images/pattern.png" media-type="image/png"/>
    <item id="i5" href="images/dark.png" media-type="image/png"/>
    <item id="i3" href="images/dark.gif" media-type="image/gif"/>
    <item id="i4" href="images/small.bmp" media-type="image/bmp"/>
    <item id="i7" href="images/baseline_stored.jpg" media-type="image/jpeg"/>
  </manifest>
  <spine><itemref idref="ch1"/></spine>
</package>
"""
nav = ('<?xml version="1.0" encoding="UTF-8"?>\n'
       '<html xmlns="http://www.w3.org/1999/xhtml"><body><nav epub:type="toc" '
       'xmlns:epub="http://www.idpf.org/2007/ops"><ol><li><a href="text/ch1.xhtml">c</a></li></ol></nav></body></html>\n')
ch1 = ('<?xml version="1.0" encoding="UTF-8"?>\n'
       '<html xmlns="http://www.w3.org/1999/xhtml"><body>'
       '<p><img src="../images/prog.jpg"/></p>'
       '<p><img src="../images/pattern.png"/></p>'
       '<p><img src="../images/dark.gif"/></p>'
       '<p><img src="../images/small.bmp"/></p>'
       '</body></html>\n')

epub_path = os.path.join(OUT, "fit_fixtures.epub")
# Atomic publish: an interrupted generator must not leave a half-written EPUB
# whose mtime would satisfy the CMake OUTPUT rule (review 2026-11-10).
epub_tmp = epub_path + ".tmp"
with zipfile.ZipFile(epub_tmp, "w") as z:
    z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", zipfile.ZIP_STORED)
    z.writestr("META-INF/container.xml",
               '<?xml version="1.0" encoding="UTF-8"?>\n'
               '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
               '<rootfiles><rootfile full-path="OEBPS/content.opf" '
               'media-type="application/oebps-package+xml"/></rootfiles></container>\n')
    z.writestr("OEBPS/content.opf", opf)
    z.writestr("OEBPS/nav.xhtml", nav)
    z.writestr("OEBPS/text/ch1.xhtml", ch1)
    z.write(os.path.join(OUT, "prog.jpg"), "OEBPS/images/prog.jpg")
    z.write(os.path.join(OUT, "dark_prog.jpg"), "OEBPS/images/dark_prog.jpg")
    z.write(os.path.join(OUT, "dcfallback_prog.jpg"), "OEBPS/images/dcfallback_prog.jpg")
    z.write(os.path.join(OUT, "pattern.png"), "OEBPS/images/pattern.png")
    z.write(os.path.join(OUT, "dark.png"), "OEBPS/images/dark.png")
    z.write(os.path.join(OUT, "dark.gif"), "OEBPS/images/dark.gif")
    z.write(os.path.join(OUT, "small.bmp"), "OEBPS/images/small.bmp")
    # STORED baseline JPEG (extracted from the committed jpeg-images book):
    # the arena-retry pin needs a small scratch arena, and a deflated entry
    # spends 32 KiB on the inflate window alone before the JPEG decoder
    # starts. Same reasoning as the SDK suite's stored512.epub.
    with zipfile.ZipFile(os.path.join(SRC, os.pardir, os.pardir, "test", "epubs", "test_jpeg_images.epub")) as src_epub:
        baseline = src_epub.read("OEBPS/images/scaling_test.jpg")
    z.writestr("OEBPS/images/baseline_stored.jpg", baseline, zipfile.ZIP_STORED)

os.replace(epub_tmp, epub_path)
print("ttf_image_fit fixtures:", epub_path)
