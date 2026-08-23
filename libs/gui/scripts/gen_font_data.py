#!/usr/bin/env python3
"""Generate core/font_psf_data.hpp from a PSF2 font blob.

Embeds the font as a constexpr uint8_t array (a compile-time constant, NOT a
runtime file read) so the host-neutral core needs no file I/O to render text.

Usage: python3 scripts/gen_font_data.py <font.psf> <out.hpp>
"""
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: gen_font_data.py <font.psf> <out.hpp>")
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    if len(data) < 32:
        sys.exit("too short for a PSF2 header (%d bytes)" % len(data))
    magic = int.from_bytes(data[0:4], "little")
    if magic != 0x864AB572:
        sys.exit("not a PSF2 font (magic 0x%08x)" % magic)
    nglyph = int.from_bytes(data[16:20], "little")
    bpg = int.from_bytes(data[20:24], "little")
    height = int.from_bytes(data[24:28], "little")
    width = int.from_bytes(data[28:32], "little")

    out = []
    out.append("/**")
    out.append(" * @file core/font_psf_data.hpp")
    out.append(" * @brief PSF2 console font embedded as a constexpr byte array")
    out.append(" *")
    out.append(" * Source: CinuxOS assets/font.psf -- PSF2, %dx%d, %d glyphs," % (width, height, nglyph))
    out.append(" * %d bytes/glyph, MSB-first, no unicode table. Embedded as a compile-time" % bpg)
    out.append(" * constant (NOT a runtime file read) so core stays host-neutral. Parsed by")
    out.append(" * core/font.cpp.")
    out.append(" *")
    out.append(' * Regenerate: python3 scripts/gen_font_data.py <font.psf> core/font_psf_data.hpp')
    out.append(" *")
    out.append(" * AUTO-GENERATED -- do not hand-edit.")
    out.append(" */")
    out.append("#pragma once")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("namespace cinux::gui {")
    out.append("")
    out.append("constexpr uint8_t kFontPsfData[] = {")
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        out.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    out.append("};")
    out.append("")
    out.append("constexpr uint32_t kFontPsfSize = sizeof(kFontPsfData);")
    out.append("")
    out.append("}  // namespace cinux::gui")
    out.append("")
    open(dst, "w").write("\n".join(out))
    print("wrote %d bytes -> %s (%d glyphs %dx%d, %d bytes/glyph)"
          % (len(data), dst, nglyph, width, height, bpg))


if __name__ == "__main__":
    main()
