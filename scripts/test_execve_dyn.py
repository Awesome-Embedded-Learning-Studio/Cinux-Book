#!/usr/bin/env python3
"""Fire the dynamic-linking end-to-end path on a real machine, by keyboard.

Boots the built image with the teaching volume, where busybox ash holds the
login seat, then types the dynamic hello by bare name (PATH lookup) and its
static sibling by full path, watching the serial log for the greetings.
Requires the volume's default installs: busybox seated as /bin/sh and the
interpreter pair in /lib — all automatic when the source trees and musl
toolchain are reachable; see create_ext2_disk.sh's notices otherwise.

KVM only, matching the house rule. Logs stay under /tmp.
"""

import argparse
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "mcp"))

import qemu  # noqa: E402


def text_to_keys(text):
    named = {" ": "spc", "-": "minus", "/": "slash", ".": "dot"}
    return [named.get(ch, ch) for ch in text] + ["ret"]


def wait_for_serial(session, marker, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if marker in session.serial(40):
            return True
        time.sleep(0.5)
    return False


def run_command(session, line, markers, timeout):
    result = session.sendkeys(text_to_keys(line))
    if not result.startswith("sent "):
        print(f"[dyn-smoke] FAIL: {result}")
        return False
    for marker in markers:
        if not wait_for_serial(session, marker, timeout):
            print(f"[dyn-smoke] FAIL: {marker!r} never arrived after {line!r}")
            tail = session.serial(40)
            print(f"[dyn-smoke] serial tail:\n{tail}")
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="build/boot/cinux.img",
                        help="boot image to fire (default: build/boot/cinux.img)")
    parser.add_argument("--timeout", type=float, default=20.0,
                        help="seconds to wait per marker (default: 20)")
    args = parser.parse_args()

    session = qemu.Session(args.image)
    session.boot()
    if not wait_for_serial(session, "/ #", args.timeout):
        print("[dyn-smoke] FAIL: the ash prompt never arrived "
              "(busybox seated as /bin/sh?)")
        session.quit(2)
        return 1

    checks = [
        ("hello-dyn", ["Hello from dynamic musl on Cinux!"]),
        ("/bin/hello", ["hello from the volume"]),
    ]
    ok = True
    for line, markers in checks:
        ok = run_command(session, line, markers, args.timeout) and ok

    session.quit(2)
    if ok:
        print("[dyn-smoke] dynamic and static hello both PASS")
        return 0
    print("[dyn-smoke] hint: check the disk script's notices — busybox seat and "
          "interpreter pair must both install (see this file's docstring)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
