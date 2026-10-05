#!/usr/bin/env python3
import struct
import zlib

GLYPH_HEIGHT = 16


def read_ppm(path):
    with open(path, "rb") as handle:
        data = handle.read()
    parts = data.split()
    width = int(parts[1])
    height = int(parts[2])
    offset = data.index(b"255\n", 3) + 4
    return width, height, data[offset:]


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
