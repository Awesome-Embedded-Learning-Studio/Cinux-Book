#!/usr/bin/env python3
"""Create a scratch disk once, preserving an existing disk's contents."""
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
path.parent.mkdir(parents=True, exist_ok=True)
try:
    with path.open('xb') as disk:
        disk.truncate(int(sys.argv[2]))
except FileExistsError:
    pass
