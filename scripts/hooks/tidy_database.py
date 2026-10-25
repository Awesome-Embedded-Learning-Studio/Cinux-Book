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
