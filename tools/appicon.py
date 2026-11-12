"""The program's icon in the formats the platforms take: `python tools/appicon.py`.

Renders `app/appicon/fsed.svg` with Qt's SVG renderer (PySide6: the renderer the window draws its
icon with, so every size looks as it does in the program) and writes beside it

- `fsed.ico`: 16 to 256 pixels, compiled into the Windows exe through `fsed.rc`. The sizes below
  256 are 32-bit bitmaps with their mask, 256 is a PNG, as Windows' own icons are built.
- `fsed.icns`: 32 to 1024 pixels as PNGs, the icon of the macOS bundle.

Sizes under 32 pixels come from `fsed-small.svg`: the same picture drawn for a 16 pixel grid, which
the window takes for those sizes too. Both results are kept in the repository, so a build needs
neither Python nor PySide6; run this again after a change to an SVG (`pip install PySide6`).
"""

import os
import struct
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QBuffer, QIODevice, QRectF, Qt  # noqa: E402
from PySide6.QtGui import QGuiApplication, QImage, QPainter  # noqa: E402
from PySide6.QtSvg import QSvgRenderer  # noqa: E402

DIR = Path(__file__).resolve().parent.parent / "app" / "appicon"
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SMALL_BELOW = 32
# The PNG types of an .icns and their pixels; ic11 to ic14 are the @2x images of 16, 32, 128 and 256.
ICNS_TYPES = [(b"ic11", 32), (b"ic12", 64), (b"ic07", 128), (b"ic13", 256), (b"ic08", 256), (b"ic14", 512),
              (b"ic09", 512), (b"ic10", 1024)]


class Source:
    def __init__(self):
        self.full = self.load(DIR / "fsed.svg")
        self.small = self.load(DIR / "fsed-small.svg")

    @staticmethod
    def load(path):
        renderer = QSvgRenderer(str(path))
        if not renderer.isValid():
            sys.exit(f"{path}: not an SVG Qt can draw")
        return renderer

    def image(self, size):
        """The icon at `size` pixels, straight (not premultiplied) alpha."""
        image = QImage(size, size, QImage.Format.Format_ARGB32_Premultiplied)
        image.fill(Qt.GlobalColor.transparent)
        painter = QPainter(image)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        (self.small if size < SMALL_BELOW else self.full).render(painter, QRectF(0, 0, size, size))
        painter.end()
        return image.convertToFormat(QImage.Format.Format_ARGB32)


def png(image):
    buffer = QBuffer()
    buffer.open(QIODevice.OpenModeFlag.WriteOnly)
    if not image.save(buffer, "PNG"):
        sys.exit("Qt could not write a PNG")
    return bytes(buffer.data())


def bitmap(image):
    """An icon's device-independent bitmap: the header with the height doubled, the BGRA rows from
    the bottom one up, then the 1-bit mask of the fully transparent pixels in rows of whole 32 bits."""
    size = image.width()
    stride = image.bytesPerLine()
    pixels = bytes(image.constBits())
    rows = [pixels[y * stride:y * stride + size * 4] for y in range(size - 1, -1, -1)]
    mask_stride = (size + 31) // 32 * 4
    mask = bytearray()
    for row in rows:
        bits = bytearray(mask_stride)
        for x in range(size):
            if row[x * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    color = b"".join(rows)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(color) + len(mask), 0, 0, 0, 0)
    return header + color + bytes(mask)


def write_ico(source, path):
    images = [png(source.image(s)) if s >= 256 else bitmap(source.image(s)) for s in ICO_SIZES]
    offset = 6 + 16 * len(images)
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    for size, data in zip(ICO_SIZES, images):
        side = size % 256  # 256 is written as 0
        out += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for data in images:
        out += data
    path.write_bytes(out)


def write_icns(source, path):
    cache = {}
    entries = []
    for kind, size in ICNS_TYPES:
        if size not in cache:
            cache[size] = png(source.image(size))
        entries.append((kind, cache[size]))
    toc = b"".join(kind + struct.pack(">I", 8 + len(data)) for kind, data in entries)
    body = b"TOC " + struct.pack(">I", 8 + len(toc)) + toc
    body += b"".join(kind + struct.pack(">I", 8 + len(data)) + data for kind, data in entries)
    path.write_bytes(b"icns" + struct.pack(">I", 8 + len(body)) + body)


def main():
    app = QGuiApplication(sys.argv[:1])  # noqa: F841 - Qt draws nothing without one
    source = Source()
    write_ico(source, DIR / "fsed.ico")
    write_icns(source, DIR / "fsed.icns")
    for name in ("fsed.ico", "fsed.icns"):
        print(f"{DIR / name}: {(DIR / name).stat().st_size} bytes")


if __name__ == "__main__":
    main()
