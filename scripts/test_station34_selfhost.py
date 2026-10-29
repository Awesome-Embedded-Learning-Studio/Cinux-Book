#!/usr/bin/env python3
# Boots the self-host volume and checks as/ld, GCC and G++ products.
# Every run validates its own output, all products must be default PIE,
# and any unserved syscall fails the complete C/C++ compilation round.
import argparse
import os
import pathlib
import re
import select
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "mcp"))
from qemu_args import device_args, qemu_command


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", default="build/boot/cinux.img")
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()
    build = pathlib.Path(args.image).resolve().parent.parent
    manifest = build / "toolchain-staging.meta" / "manifest"
    if not manifest.is_file():
        raise RuntimeError(f"toolchain manifest missing: {manifest}")
    gcc_lib_dir = ""
    for line in manifest.read_text().splitlines():
        if line.startswith("GCC_LIB_DIR="):
            gcc_lib_dir = line.split("=", 1)[1]
    if not gcc_lib_dir:
        raise RuntimeError(f"GCC_LIB_DIR not found in {manifest}")
    link = "ld @/hello.ldargs"
    log = build / "station34-selfhost.serial.log"
    process = subprocess.Popen(
        [*qemu_command(args.image), *device_args(args.image), "-snapshot",
         "-drive", f"format=raw,file={args.image}", "-serial", "stdio", "-monitor", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    transcript = bytearray()

    def wait_for(pattern, start=0):
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            match = re.search(pattern, transcript[start:])
            if match:
                return match
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited: {process.returncode}")
            if select.select([process.stdout], [], [], 0.1)[0]:
                transcript.extend(os.read(process.stdout.fileno(), 65536))
                log.write_bytes(transcript)
        raise RuntimeError(f"marker timed out: {pattern!r}")

    def command(text, marker):
        start = len(transcript)
        began = time.monotonic()
        process.stdin.write((text + "\n").encode())
        process.stdin.flush()
        prefix, expected = marker.rsplit("_", 1)
        match = wait_for(rb"(?:\r?\n)" + prefix.encode() + rb"_\d+\r?\n", start)
        if not re.search(rb"(?:\r?\n)" + marker.encode() + rb"\r?\n", transcript[start:]):
            raise RuntimeError(f"command returned an unexpected status: {text}")
        wait_for(rb"/ #", start + match.end())
        elapsed = time.monotonic() - began
        print(f"[selfhost] {marker}: {elapsed:.3f}s", flush=True)
        return bytes(transcript[start:])

    try:
        wait_for(rb"Cinux up")
        wait_for(rb"/ #")
        command("as /hello.s -o /hello.o; echo AS_RC_$?", "AS_RC_0")
        command(f"{link}; echo LD_RC_$?", "LD_RC_0")
        output = command("/hello; echo RUN_RC_$?", "RUN_RC_0")
        if not re.search(rb"\r?\nHello from GCC!\r?\n", output):
            raise RuntimeError("hello output not seen on the console")
        command("gcc /hello.c -o /hello_g; echo GCC_RC_$?", "GCC_RC_0")
        output = command("/hello_g; echo RUNG_RC_$?", "RUNG_RC_0")
        if not re.search(rb"\r?\nHello from GCC!\r?\n", output):
            raise RuntimeError("gcc-built hello output not seen on the console")
        command("g++ /hello.cpp -o /hellopp; echo GPP_RC_$?", "GPP_RC_0")
        output = command("/hellopp; echo RUNPP_RC_$?", "RUNPP_RC_0")
        for marker in (b"Hello from G++!", b"exception: caught"):
            if marker + b"\r\n" not in output and marker + b"\n" not in output:
                raise RuntimeError(f"C++ output missing: {marker!r}")
        for product in ("hello", "hello_g", "hellopp"):
            output = command(f"/bin/busybox od -An -tu1 -j16 -N2 /{product}; echo PIE_{product}_$?",
                             f"PIE_{product}_0")
            if not re.search(rb"\r?\n\s*3\s+0\s*\r?\n", output):
                raise RuntimeError(f"/{product} is not ET_DYN")
        command("echo 'static long sys(long n,long a,long b,long c){long r;' > /free.c; echo W0_$?", "W0_0")
        command('echo \'asm volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),\' >> /free.c; echo W1_$?', "W1_0")
        command('echo \'"d"(c):"memory","rcx","r11");return r;}\' >> /free.c; echo W2_$?', "W2_0")
        command('echo \'void _start(void){const char m[]="freestanding on Cinux\\n";\' >> /free.c; echo W3_$?', "W3_0")
        command("echo 'sys(1,1,(long)m,22);sys(60,0,0,0);}' >> /free.c; echo W4_$?", "W4_0")
        command("gcc -ffreestanding -fno-stack-protector -nostdlib -no-pie -o /free /free.c; echo CC_$?", "CC_0")
        output = command("/free; echo RUN_$?", "RUN_0")
        if not re.search(rb"\r?\nfreestanding on Cinux\r?\n", output):
            raise RuntimeError("freestanding output missing")
        command("/bin/execprobe; echo EXECPROBE_$?", "EXECPROBE_0")
        command("/bin/sendfileprobe; echo SENDFILE_$?", "SENDFILE_0")
        command("gcc -wrapper /absent /hello.c -o /bad; echo SPAWN_FAIL_$?", "SPAWN_FAIL_1")
        if re.search(rb"syscall \d+ unserved", transcript):
            raise RuntimeError("unserved syscall observed during self-host compilation")
        print(f"[selfhost] PASS: as/ld + gcc + g++ STL/exceptions, three PIE products + freestanding no-libc, zero unserved syscalls ({log})")
        return 0
    except RuntimeError as error:
        print(f"[selfhost] FAIL: {error}")
        print(transcript.decode(errors="replace")[-5000:])
        return 1
    finally:
        process.terminate()
        process.wait(timeout=5)
        log.write_bytes(transcript)


if __name__ == "__main__":
    sys.exit(main())
