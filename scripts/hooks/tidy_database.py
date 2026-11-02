#!/usr/bin/env python3
import json
import pathlib
import shlex
import sys

entries = json.loads(pathlib.Path(sys.argv[1]).read_text())
kept = []
seen = set()
for entry in entries:
    words = entry.get("arguments") or shlex.split(entry["command"])
    # GCC permits floating-point declarations with this code-generation restriction;
    # Clang rejects even libstdc++ declarations. Preserve the no-SIMD AST target
    # while leaving the actual GCC build flags untouched.
    if "-mgeneral-regs-only" in words:
        words = [word for word in words if word != "-mgeneral-regs-only"]
        words.extend(("-mno-sse", "-mno-sse2", "-mno-mmx"))
        entry = dict(entry)
        entry.pop("command", None)
        entry["arguments"] = words
    normalized = []
    index = 0
    while index < len(words):
        if words[index] == "-o":
            index += 2
        else:
            normalized.append(words[index])
            index += 1
    key = (entry["directory"], entry["file"], tuple(normalized))
    if key not in seen:
        seen.add(key)
        kept.append(entry)
pathlib.Path(sys.argv[2]).write_text(json.dumps(kept))
