#!/usr/bin/env python3
# Compiles the configured kernel entirely in Cinux, extracts the resulting
# boot image after QEMU exits, verifies its guest hash, and boots that image.
import argparse
import hashlib
import json
import os
import pathlib
import re
import select
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "mcp"))
from qemu_args import device_args


class Guest:
    def __init__(self, image, volume, log, timeout, memory=512, monitor=None, ps2_only=False,
                 gui=False, qmp=None):
        subprocess.run(['cmake', '--build', str(image.parent.parent), '--target', 'qemu_disks'],
                       check=True, stdout=subprocess.DEVNULL)
        devices = device_args(image, 'qemu-gui-args.txt' if gui else 'qemu-device-args.txt')
        if gui:
            for index, argument in enumerate(devices):
                if argument == '-display':
                    devices[index + 1] = 'none'
            devices = [word for index, word in enumerate(devices)
                       if word != '-m' and (index == 0 or devices[index - 1] != '-m')]
        if ps2_only:
            devices = [word for index, word in enumerate(devices)
                       if not (word.startswith(('usb-kbd', 'usb-tablet'))
                               or (word == '-device' and index + 1 < len(devices)
                                   and devices[index + 1].startswith(('usb-kbd', 'usb-tablet'))))]
        for index, argument in enumerate(devices):
            if argument.startswith("format=raw,file=") and argument.endswith(",if=none,id=spare"):
                devices[index] = f"format=raw,file={volume},if=none,id=spare"
        self.log = log
        self.timeout = timeout
        self.transcript = bytearray()
        self.process = subprocess.Popen(
            ["qemu-system-x86_64", *devices, "-m", f"{memory}M", "-boot", "order=c",
             "-drive", f"format=raw,file={image},if=none,id=boot",
             "-device", "ide-hd,drive=boot,bus=ide.0,bootindex=0",
             *(["-qmp", f"unix:{qmp},server,nowait"] if qmp else []),
             "-serial", "stdio", "-monitor", f"unix:{monitor},server,nowait" if monitor else "none"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def wait_for(self, pattern, start=0):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            match = re.search(pattern, self.transcript[start:])
            if match:
                return match
            if b"[kern] exception" in self.transcript[start:]:
                raise RuntimeError("kernel or user exception observed; see serial log")
            if self.process.poll() is not None:
                raise RuntimeError(f"QEMU exited: {self.process.returncode}")
            if select.select([self.process.stdout], [], [], 0.1)[0]:
                self.transcript.extend(os.read(self.process.stdout.fileno(), 65536))
                self.log.write_bytes(self.transcript)
        raise RuntimeError(f"marker timed out: {pattern!r}")

    def command(self, text, marker):
        start = len(self.transcript)
        self.process.stdin.write((text + "\n").encode())
        self.process.stdin.flush()
        match = self.wait_for(rb"\r?\n" + marker.encode() + rb"_(\d+)\r?\n", start)
        if match[1] != b"0":
            raise RuntimeError(f"command failed ({match[1].decode()}): {text}")
        self.wait_for_prompt(start + match.end())
        return bytes(self.transcript[start:])

    def wait_for_prompt(self, start=0):
        return self.wait_for(rb"(?:/[^\r\n]*|~[^\r\n]*) [#$] ", start)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.write_bytes(self.transcript)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", default="build/boot/cinux.img", type=pathlib.Path)
    parser.add_argument("--timeout", default=3600, type=float)
    args = parser.parse_args()
    image = args.image.resolve()
    build = image.parent.parent
    volume = build / "boot/kernel-selfhost.ext2"
    if not volume.is_file():
        raise RuntimeError("build target kernel_selfhost_volume first")
    with volume.open("rb") as disk:
        disk.seek(512)
        reserved_sector = disk.read(512)
    guest = Guest(image, volume, build / "station34-kernel-selfhost.serial.log", 120)
    try:
        guest.wait_for(rb"Cinux up\r?\n")
        guest.wait_for(rb"/ #")
        guest.timeout = args.timeout
        output = guest.command("python3 -B -S /src/build-kernel-in-guest.py; echo KBUILD_$?", "KBUILD")
        match = re.search(rb"\r?\n\[kernel-selfhost\] IMAGE_SHA256 ([0-9a-f]{64})\r?\n", output)
        if not match or b"[kernel-selfhost] PASS " not in output:
            raise RuntimeError("guest build proof missing")
        digest = match[1].decode()
        if re.search(rb"syscall \d+ unserved|\[kern\] exception", guest.transcript):
            raise RuntimeError("unserved syscall or kernel exception during compilation")
        stats = pathlib.Path(f"/proc/{guest.process.pid}/io").read_text()
        counters = {name: int(value) for name, value in
                    (line.split(":", 1) for line in stats.splitlines())}
        (build / "station34-kernel-selfhost.metrics.json").write_text(
            json.dumps({"qemu_io": counters, "image_sha256": digest}, indent=2) + "\n")
        print(output.decode(errors="replace"), flush=True)
    finally:
        guest.close()
    with volume.open("rb") as disk:
        disk.seek(512)
        if disk.read(512) != reserved_sector:
            raise RuntimeError("AHCI scratch probe changed the reserved sector")
    product = build / "boot/cinux-selfhost.img"
    elf = build / "kernel/cinux_kernel_selfhost"
    for source, destination in (("/src/out/cinux.img", product),
                                ("/src/out/kernel/cinux_kernel", elf)):
        destination.unlink(missing_ok=True)
        subprocess.run(["debugfs", "-R", f"dump {source} {destination}", str(volume)],
                       check=True, capture_output=True)
        if not destination.is_file():
            raise RuntimeError(f"guest artifact missing: {source}")
    if hashlib.sha256(product.read_bytes()).hexdigest() != digest:
        raise RuntimeError("extracted boot image differs from guest hash")
    guest = Guest(product, volume, build / "station34-kernel-selfhost-reboot.serial.log", 120)
    try:
        guest.wait_for(rb"Cinux up\r?\n")
        guest.wait_for(rb"/ #")
        output = guest.command("gcc /hello.c -o /reborn; echo REBORN_CC_$?", "REBORN_CC")
        output += guest.command("/reborn; echo REBORN_RUN_$?", "REBORN_RUN")
        if not re.search(rb"\r?\nHello from GCC!\r?\n", output):
            raise RuntimeError("new kernel failed the compiler/runtime check")
        if re.search(rb"syscall \d+ unserved|\[kern\] exception", guest.transcript):
            raise RuntimeError("unserved syscall or exception in guest-built kernel")
    finally:
        guest.close()
    print(f"[kernel-selfhost] PASS: guest compiled the complete kernel, hash {digest}, "
          f"reboot + GCC/run passed ({product})")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(f"[kernel-selfhost] FAIL: {error}", file=sys.stderr)
        sys.exit(1)
