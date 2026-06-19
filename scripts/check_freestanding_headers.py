#!/usr/bin/env python3
"""check_freestanding_headers.py — forbid hosted/STL headers in the kernel.

The CinuxOS kernel is freestanding C++17 with no exceptions/RTTI and no STL
(DIRECTIVES A). Only a small set of <cxxx>/<type_traits>/<utility> headers is
permitted; containers, strings, streams, smart pointers, <atomic>/<thread>,
and exception/RTTI machinery are prohibited because they either heap-allocate,
depend on the hosted runtime, or duplicate facilities the kernel provides
itself (cinux::lib::Span/StringView/Atomic, kernel slab, kernel locks).

This scan exits non-zero if any production kernel source includes a
prohibited header. kernel/mini/ and kernel/test/ are excluded (the bootstrap
and in-kernel test harness have separate constraints), matching
check_line_limits.py.

Usage:
    python3 scripts/check_freestanding_headers.py [path ...]
"""

import os
import re
import sys

# Prohibited <...> headers (the angle-bracket name only, no brackets).
# Mirrors DIRECTIVES A's forbidden list plus the rest of the hosted STL family.
PROHIBITED = {
    # Containers (DIRECTIVES A; CinuxOS has no Array<T,N> yet)
    "array", "vector", "deque", "forward_list", "list", "map", "unordered_map",
    "set", "unordered_set", "multimap", "unordered_multimap", "multiset",
    "unordered_multiset", "bitset", "stack", "queue", "priority_queue",
    "span",  # std::span; use cinux::lib::Span
    # Strings
    "string", "string_view",  # cinux::lib::StringView instead
    # Memory / smart pointers
    "memory",
    # I/O streams
    "iostream", "istream", "ostream", "sstream", "streambuf", "fstream",
    "ios", "cstdio",
    # Algorithms / numerics
    "algorithm", "numeric",
    # Concurrency (kernel has its own primitives; cinux::lib::Atomic)
    "thread", "mutex", "shared_mutex", "condition_variable", "future",
    "atomic", "chrono", "latch", "semaphore", "barrier",
    # Exceptions / RTTI (banned: -fno-exceptions -fno-rtti)
    "exception", "stdexcept", "typeinfo", "typeindex",
    # Other hosted/dynamic facilities
    "functional", "regex", "random", "ratio", "locale", "format",
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*<([a-z0-9_]+)>', re.MULTILINE)


def scan_file(path: str):
    """Yield (line_no, header) for each prohibited include found in path."""
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return
    for idx, line in enumerate(text.splitlines(), start=1):
        m = INCLUDE_RE.match(line)
        if m and m.group(1) in PROHIBITED:
            yield idx, m.group(1)


def main() -> int:
    paths = sys.argv[1:] or ["kernel"]
    violations = []

    for base in paths:
        for root, _dirs, files in os.walk(base):
            rel = os.path.relpath(root, ".")
            parts = rel.split(os.sep)
            if "mini" in parts:
                continue
            if "test" in parts:
                continue
            for fname in files:
                ext = os.path.splitext(fname)[1].lower()
                if ext not in (".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".hxx"):
                    continue
                fpath = os.path.join(root, fname)
                for line_no, header in scan_file(fpath):
                    violations.append((fpath, line_no, header))

    if violations:
        print("ERROR: prohibited freestanding-hosted headers in kernel:", file=sys.stderr)
        for path, line_no, header in violations:
            print(f"  {path}:{line_no}: #include <{header}>", file=sys.stderr)
        print("\nCinuxOS is freestanding: use cinux::lib (Span/StringView/Atomic), "
              "the kernel slab/locks, or a local aggregate instead.", file=sys.stderr)
        return 1

    print("OK: no prohibited freestanding-hosted headers in kernel")
    return 0


if __name__ == "__main__":
    sys.exit(main())
