#!/usr/bin/env python3
"""Map headers to direct consumers present in the compilation database."""

import json
import os
from pathlib import Path
import re
import sys


def main() -> None:
    root = Path.cwd()
    entries = json.loads((root / "build/compile_commands.json").read_text())
    sources = sorted(
        {Path(entry["file"]).resolve() for entry in entries if Path(entry["file"]).suffix in (".cpp", ".cc", ".cxx", ".c")},
        key=lambda source: (0 if source.is_relative_to(root / "test/unit") else 1, str(source)),
    )
    contents = {source: source.read_text() for source in sources if source.exists()}
    inputs = []
    for argument in sys.argv[1:]:
        path = Path(argument).resolve()
        selected = path
        if path.suffix in (".h", ".hpp", ".hxx") and path.is_relative_to(root):
            names = {path.relative_to(root).as_posix()}
            if path.is_relative_to(root / "base/include"):
                names.add(path.relative_to(root / "base/include").as_posix())
            for source in sources:
                if source not in contents:
                    continue
                relative = Path(os.path.relpath(path, source.parent)).as_posix()
                spellings = names | {relative}
                pattern = r'^\s*#\s*include\s*[<"](?:' + '|'.join(re.escape(name) for name in spellings) + r')[>"]'
                if re.search(pattern, contents[source], re.MULTILINE):
                    selected = source
                    break
        inputs.append(str(selected))
    for selected in dict.fromkeys(inputs):
        print(selected)


if __name__ == "__main__":
    main()
