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

W, H = 800, 1200
OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)


def pattern(x, y):
    # Left half: horizontal gradient. Right half: dark top / light bottom.
    if x < W // 2:
        return (x * 255) // (W // 2 - 1)
    return 30 if y < H // 2 else 235


pix = [[pattern(x, y) for x in range(W)] for y in range(H)]


def write_progressive_jpeg(path):
    def avg(x0, y0):
        s = 0
        for y in range(y0, min(y0 + 8, H)):
            row = pix[y]
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
    out += b"\xff\xd9"
    with open(path, "wb") as f:
        f.write(out)


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


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
    with open(path, "wb") as f:
        f.write(png)


write_progressive_jpeg(os.path.join(OUT, "prog.jpg"))
write_png(os.path.join(OUT, "pattern.png"))
def write_gif(path):
    """Minimal 96x144 GIF87a: 2-color palette, every pixel index 0 (dark).
    Uncompressed-style LZW: a clear code before every pixel code keeps the
    code size at the minimum, so no table management is needed."""
    GW, GH = 96, 144
    out = bytearray(b"GIF87a")
    out += struct.pack("<HH", GW, GH)
    out += bytes([0x80, 0, 0])  # GCT present, size bits 000 = 2 colors; bg 0; aspect 0
    out += bytes([10, 10, 10, 250, 250, 250])  # palette: dark, white
    out += b","  # image descriptor
    out += struct.pack("<HHHH", 0, 0, GW, GH)
    out += bytes([0])  # no local table, not interlaced
    out += bytes([2])  # LZW min code size

    bits = []

    def put(val, n):
        for i in range(n - 1, -1, -1):
            bits.append((val >> i) & 1)

    for _ in range(GW * GH):
        put(4, 3)  # CLEAR
        put(0, 3)  # pixel = palette index 0 (dark)
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
    with open(path, "wb") as f:
        f.write(out)


write_gif(os.path.join(OUT, "dark.gif"))

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
    <item id="i2" href="images/pattern.png" media-type="image/png"/>
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
       '</body></html>\n')

epub_path = os.path.join(OUT, "fit_fixtures.epub")
with zipfile.ZipFile(epub_path, "w") as z:
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
    z.write(os.path.join(OUT, "pattern.png"), "OEBPS/images/pattern.png")

print("ttf_image_fit fixtures:", epub_path)
