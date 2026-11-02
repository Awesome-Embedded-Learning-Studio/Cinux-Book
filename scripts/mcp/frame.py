#!/usr/bin/env python3
import struct
import zlib

GLYPH_HEIGHT = 16


def read_ppm(path):
    """Decode a binary P6 dump: magic, comments, three fields, then pixels."""
    data = path.read_bytes() if hasattr(path, "read_bytes") else open(path, "rb").read()
    if not data.startswith(b"P6"):
        raise RuntimeError(f"screendump is not P6: {path}")
    fields = []
    offset = 2
    while len(fields) < 3:
        while offset < len(data) and data[offset : offset + 1].isspace():
            offset += 1
        if data[offset : offset + 1] == b"#":
            while data[offset : offset + 1] not in (b"\n", b""):
                offset += 1
            continue
        start = offset
        while not data[offset : offset + 1].isspace():
            offset += 1
        fields.append(int(data[start:offset]))
    offset += 1
    width, height, _ = fields
    pixels = data[offset : offset + width * height * 3]
    if len(pixels) != width * height * 3:
        raise RuntimeError(f"screendump truncated: {path}")
    return width, height, pixels


def count_lit(pixels, width, text_row):
    start = width * text_row * GLYPH_HEIGHT * 3
    end = width * (text_row + 1) * GLYPH_HEIGHT * 3
    return sum(1 for i in range(start, min(end, len(pixels)), 3) if pixels[i] >= 200)


def write_png(path, width, height, rgb):
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw))
           + chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(png)
