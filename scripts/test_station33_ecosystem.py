#!/usr/bin/env python3
import argparse
import os
import pathlib
import re
import select
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "mcp"))
from qemu_args import device_args


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", default="build/boot/cinux.img")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    build = pathlib.Path(args.image).resolve().parent.parent
    log = build / "station33-ecosystem.serial.log"
    process = subprocess.Popen(
        ["qemu-system-x86_64", *device_args(args.image), "-snapshot",
         "-drive", f"format=raw,file={args.image}", "-serial", "stdio", "-monitor", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    transcript = bytearray()

    def wait_for(pattern):
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if re.search(pattern, transcript):
                return
            failure = re.search(rb"\r?\n(ECO_FAIL [^\r\n]+)\r?\n", transcript)
            if failure:
                raise RuntimeError(failure.group(1).decode())
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited: {process.returncode}")
            if select.select([process.stdout], [], [], 0.1)[0]:
                transcript.extend(os.read(process.stdout.fileno(), 65536))
        raise RuntimeError(f"marker timed out: {pattern!r}")

    def command(text, marker):
        process.stdin.write((text + "\n").encode())
        process.stdin.flush()
        wait_for(rb"(?:\r?\n)" + marker.encode() + rb"\r?\n")

    try:
        wait_for(rb"Cinux up")
        wait_for(rb"/ #")
        command("hello-glibc", "Hello from dynamic glibc on Cinux!")
        cases = [
            ("uname", 0), ("id", 0), ("whoami", 0), ("pwd", 0),
            ("true", 0), ("false", 1), ("sleep 0.01", 0), ("env", 0),
            ("hostname", 0), ("echo ecosystem", 0), ("cat /etc/passwd", 0),
            ("wc /etc/passwd", 0), ("ps", 0), ("free", 0),
        ]
        for index, (text, status) in enumerate(cases):
            marker = f"APPLET_{index}_{status}"
            command(f"{text}; echo APPLET_{index}_$?", marker)
        command("cat /proc/1/stat; echo INIT_STAT_$?", "INIT_STAT_0")
        if not re.search(rb"\n1 \(init\)", transcript):
            raise RuntimeError("PID 1 is not the userspace init")
        command("mount -a; echo MOUNT_ALL_$?", "MOUNT_ALL_0")
        command("cat /proc/mounts; echo MOUNT_TABLE_$?", "MOUNT_TABLE_0")
        if not re.search(rb"\nnone /tmp tmpfs rw 0 0\r?\n", transcript):
            raise RuntimeError("fstab did not mount tmpfs")
        command("echo tmpfs >/tmp/probe; cat /tmp/probe; echo TMPFS_$?", "TMPFS_0")
        command("ecotest --guest", "ECOSYSTEM_PASS shm unix poll relro")
        print(f"[ecosystem] PASS: glibc, init PID 1, 14/14 applets, fstab/tmpfs, shm/AF_UNIX/poll ({log})")
        return 0
    except RuntimeError as error:
        print(f"[ecosystem] FAIL: {error}")
        try:
            process.stdin.write(b"\n")
            process.stdin.flush()
            drain_deadline = time.monotonic() + 5
            while time.monotonic() < drain_deadline:
                if select.select([process.stdout], [], [], 0.2)[0]:
                    transcript.extend(os.read(process.stdout.fileno(), 65536))
        except OSError:
            pass
        print(transcript.decode(errors="replace")[-5000:])
        return 1
    finally:
        process.terminate()
        process.wait(timeout=5)
        log.write_bytes(transcript)


if __name__ == "__main__":
    sys.exit(main())
