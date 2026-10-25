#!/usr/bin/env python3
"""Check BusyBox nc over loopback with Ethernet attached and two guest CPUs.

Requires a built boot_image and a rootfs containing static BusyBox. QEMU uses
snapshot disks and private serial/log paths. Select --accel tcg without KVM.
"""

import argparse
import hashlib
import pathlib
import re
import socket
import subprocess
import tempfile
import time


def main():
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path, default=root / "build")
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    image = build / "boot/cinux.img"
    kernel = (build / "kernel/kernel.bin").read_bytes()
    with image.open("rb") as disk:
        disk.seek(32 * 512)
        if disk.read(len(kernel)) != kernel:
            parser.error("boot image differs from kernel.bin; rebuild target boot_image")
    if args.accel == "kvm" and not pathlib.Path("/dev/kvm").exists():
        parser.error("/dev/kvm is unavailable; explicitly select --accel tcg")
    options = (build / "qemu-device-args.txt").read_text().splitlines()
    options[options.index("-accel") + 1] = args.accel
    options[options.index("-cpu") + 1] = "host" if args.accel == "kvm" else "max"
    options[options.index("-smp") + 1] = "2"
    work = pathlib.Path(tempfile.mkdtemp(prefix="cinux-tcp-"))
    print(f"Logs: {work}", flush=True)
    print(f"kernel.bin: {len(kernel)} bytes, sha256 {hashlib.sha256(kernel).hexdigest()}",
          flush=True)
    serial = work / "serial.sock"
    received = bytearray()
    vm = None

    with socket.socket(socket.AF_UNIX) as channel, (work / "qemu.log").open("w") as errors:
        def wait_for(pattern, timeout):
            start = len(received)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if vm.poll() is not None:
                    raise RuntimeError(f"QEMU exited: {(work / 'qemu.log').read_text()}")
                try:
                    chunk = channel.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    raise RuntimeError("serial connection closed")
                received.extend(chunk)
                (work / "serial.log").write_bytes(received)
                text = received[start:].decode(errors="replace").replace("\r", "")
                if re.search(pattern, text):
                    return text
                if re.search(r"exception #|PANIC|\[panic\]|ASSERT", text):
                    raise RuntimeError(f"guest fault; see {work / 'serial.log'}")
            raise RuntimeError(f"missing output {pattern!r}; see {work / 'serial.log'}")

        def interleaved(token):
            # Two tasks may echo their exit lines onto the serial console
            # character-by-character; a strict line match never sees a clean
            # token. Allow a few foreign characters between every pair of
            # literal ones and the token still counts.
            return r"[\s\S]{0,4}".join(re.escape(ch) for ch in token)

        def command(text, pattern):
            # Pace UART input independently of slow monitor sendkey calls.
            for byte in (text + "\n").encode():
                channel.sendall(bytes((byte,)))
                time.sleep(.002)
            return wait_for(pattern, args.timeout)

        try:
            vm = subprocess.Popen([
                "qemu-system-x86_64", *options, "-snapshot",
                "-drive", f"format=raw,file={image}",
                "-serial", f"unix:{serial},server=on,wait=off",
                "-gdb", f"unix:{work / 'gdb.sock'},server=on,wait=off",
            ], stdout=subprocess.DEVNULL, stderr=errors)
            deadline = time.monotonic() + 5
            while not serial.exists() and vm.poll() is None and time.monotonic() < deadline:
                time.sleep(.05)
            channel.connect(str(serial))
            channel.settimeout(.1)
            wait_for(r"/ #", args.timeout)
            # Both stdin directions have EOF. -w bounds nc's final network read.
            command("(busybox nc -n -v -w 2 -l -p 4410 </dev/null; echo TCP_SERVER-$?) &",
                    r"listening on .*:4410")
            output = command(
                "echo hi | busybox nc -n -v -w 2 127.0.0.1 4410; rc=$?; "
                "busybox sleep 1; echo TCP_CLIENT-$rc",
                interleaved("TCP_CLIENT-0") + r"[\s\S]{0,200}/ #")
            if re.search(interleaved("TCP_CLIENT-0"), output) is None:
                raise RuntimeError(f"client failed: {output}")
            if not re.search(r"\nhi\n", output):
                raise RuntimeError(f"server did not print the payload: {output}")
            if re.compile(interleaved("TCP_SERVER-")).search(received.decode(errors="replace")) is None:
                wait_for(interleaved("TCP_SERVER-"), args.timeout)
            if re.compile(interleaved("TCP_SERVER-0")).search(received.decode(errors="replace")) is None:
                raise RuntimeError("server returned a nonzero status")
            command("wait; echo TCP_DONE", r"\nTCP_DONE\n[\s\S]*?/ #")
            text = received.decode(errors="replace")
            if re.search(r"PANIC|\[panic\]|ASSERT|nc: accept:", text):
                raise RuntimeError("guest failure marker in serial log")
            print("PASS: 2 CPUs, Ethernet attached, nc payload on serial, both exit codes 0")
            return 0
        except (OSError, RuntimeError) as error:
            print(f"FAIL: {error}", flush=True)
            return 1
        finally:
            if vm is not None and vm.poll() is None:
                vm.terminate()
                vm.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
