#!/usr/bin/env python3
"""Fire the ecosystem station's glibc verdict on a real machine, by keyboard.

Boots the built image with the teaching volume, where busybox ash holds the
login seat, then types the glibc-linked hello by bare name and watches the
serial log for its greeting. The volume's default installs carry the whole
runtime — the loader at /lib64/ld-linux-x86-64.so.2, libc at /lib/libc.so.6,
the binary at /bin/hello-glibc — all automatic when the host toolchain is
glibc; see create_ext2_disk.sh's notices otherwise.

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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="build/boot/cinux.img",
                        help="boot image to fire (default: build/boot/cinux.img)")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="seconds to wait per marker (default: 30)")
    args = parser.parse_args()

    session = qemu.Session(args.image)
    session.boot()
    if not wait_for_serial(session, "/ #", args.timeout):
        print("[glibc-smoke] FAIL: the ash prompt never arrived "
              "(busybox seated as /bin/sh?)")
        tail = session.serial(40)
        print(f"[glibc-smoke] serial tail:\n{tail}")
        session.quit(2)
        return 1

    session.sendkeys(text_to_keys("hello-glibc"))
    if not wait_for_serial(session, "Hello from dynamic glibc on Cinux!",
                           args.timeout):
        print("[glibc-smoke] FAIL: the glibc greeting never arrived")
        tail = session.serial(40)
        print(f"[glibc-smoke] serial tail:\n{tail}")
        session.quit(2)
        return 1

    session.quit(2)
    print("[glibc-smoke] first glibc binary on the volume PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
