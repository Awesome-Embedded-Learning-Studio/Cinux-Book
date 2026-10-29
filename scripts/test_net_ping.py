#!/usr/bin/env python3
"""Exercise station 31 on a two-CPU KVM guest with the static busybox rootfs."""

import argparse
import os
import pathlib
import re
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "mcp"))
import qemu
from test_execve_dyn import text_to_keys


def command_keys(command):
    symbols = {"|": "shift-backslash", "&": "shift-7"}
    return [symbols.get(key, f"shift-{key.lower()}" if len(key) == 1 and key.isupper() else key)
            for key in text_to_keys(command)]


def serial_text():
    path = pathlib.Path(qemu.SERIAL_PATH)
    return path.read_text(errors="replace") if path.exists() else ""


def wait_after(offset, patterns, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()[offset:]
        if all(re.search(pattern, text) for pattern in patterns):
            return text
        time.sleep(0.1)
    raise RuntimeError(f"missing guest output: {patterns!r}\n{serial_text()[offset:]}")


def run_command(session, command, patterns, timeout):
    offset = len(serial_text())
    reply = session.sendkeys(command_keys(command))
    if not reply.startswith("sent "):
        raise RuntimeError(reply)
    return wait_after(offset, patterns, timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="build/boot/cinux.img")
    parser.add_argument("--log", type=pathlib.Path, default=pathlib.Path("build/station31-ping.log"))
    args = parser.parse_args()
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        parser.error("/dev/kvm is unavailable or inaccessible; KVM is required")
    session = qemu.Session(args.image, config_name="qemu-device-args.txt", snapshot=True)
    try:
        session.boot()
        wait_after(0, [r"\[smp\] 2 CPUs online", r"/ #"], 30)
        run_command(session, "ping -c 3 -W 2 10.0.2.2",
                    [r"bytes from 10\.0\.2\.2", r"3 packets transmitted", r"/ #"], 15)
        run_command(session, "ping -c 1 -W 1 10.0.2.254",
                    [r"1 packets transmitted, 0 packets received", r"/ #"], 10)
        offset = len(serial_text())
        session.sendkeys(command_keys("ping 10.0.2.2"))
        wait_after(offset, [r"bytes from 10\.0\.2\.2"], 10)
        offset = len(serial_text())
        session.sendkeys(["ctrl-c"])
        wait_after(offset, [r"ping statistics", r"/ #"], 10)
        run_command(session, "/bin/ttydemo", [r"type a secret line"], 10)
        run_command(session, "secret", [r"ttydemo: heard: secret", r"echo restored", r"/ #"], 10)
        run_command(session, "echo pipe | cat", [r"\npipe\r?\n", r"/ #"], 10)
        run_command(session, "/bin/busybox script -qc /bin/hello /dev/null",
                    [r"hello from the volume", r"/ #"], 20)
        run_command(session, "/bin/busybox sleep 1 &", [r"/ #"], 10)
        run_command(session, "wait", [r"/ #"], 10)
        run_command(session, "cat /proc/net/dev",
                    [r"    lo: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
                     r"  eth0: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0", r"/ #"], 10)
        run_command(session, "ifconfig",
                    [r"eth0\s+Link encap:Ethernet\s+HWaddr 52:54:00:12:34:56",
                     r"inet addr:10\.0\.2\.15\s+Bcast:10\.0\.2\.255\s+Mask:255\.255\.255\.0",
                     r"lo\s+Link encap:Local Loopback",
                     r"inet addr:127\.0\.0\.1\s+Mask:255\.0\.0\.0",
                     r"MTU:65536", r"/ #"], 15)
        run_command(session, "nslookup localhost",
                    [r"Server:\t\t10\.0\.2\.3",
                     r"Name:\tlocalhost\nAddress: 127\.0\.0\.1", r"/ #"], 25)
        run_command(session, "/bin/smpstress", [r"SMP_PIPE_EOF_PASS",
                    r"SMP_FORK_PASS cpus=2 rounds=96", r"/ #"], 45)
        run_command(session, "/bin/busybox sh /etc/smp-stress.sh",
                    [r"BUSYBOX_SMP_PASS", r"/ #"], 45)
        if re.search(r"PANIC|\[panic\]|ASSERT|#DF|SMP_FORK_FAIL", serial_text()):
            raise RuntimeError("guest failure marker in serial log")
        print("[station31] echo, SIGALRM timeout, Ctrl+C, TTY/PTY, pipeline, background wait, ifconfig, DNS and SMP shell PASS")
        return 0
    except RuntimeError as error:
        print(f"[station31] FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_text(serial_text())
        if session.proc is not None and session.proc.poll() is None:
            session.quit(2)


if __name__ == "__main__":
    sys.exit(main())
