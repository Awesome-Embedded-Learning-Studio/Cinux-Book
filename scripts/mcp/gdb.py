"""GDB/MI driver plus the QEMU process it debugs.

One Session owns a frozen-at-reset QEMU (gdbstub on :1234, serial to a
file) and a persistent gdb -i=mi2 child talking to it. The MI child
stays alive across tool calls, so register peeks cost one round trip,
not the file/target-remote ceremony a cold gdb invocation needs.

MI plumbing note: the "(gdb)" prompt arrives with no trailing newline,
so everything reads character-by-character through a rolling buffer
instead of readline, which would block forever on that prompt.
"""

import glob
import os
import re as RE
import select
import subprocess
import time

GDB_PORT = "1234"
SERIAL_LOG = "/tmp/cinux_gdb_serial.log"
STOP_TIMEOUT = 60.0
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

GENERAL_REGS = [
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "rip", "eflags",
]


def resolve(default, pattern, what):
    if default:
        return default
    matches = sorted(glob.glob(os.path.join(REPO_ROOT, pattern)))
    if not matches:
        raise RuntimeError(f"no {what} given and {pattern} is missing")
    return matches[0]


STREAM_ITEM = RE.compile(r'^~"(.*)"$')
STREAM_ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "r": "\r"}


def decode_console_stream(lines):
    out = []
    for line in lines:
        match = STREAM_ITEM.match(line)
        if match is None:
            continue
        raw = match.group(1)
        text = ""
        index = 0
        while index < len(raw):
            char = raw[index]
            if char == "\\" and index + 1 < len(raw) and raw[index + 1] in STREAM_ESCAPES:
                text += STREAM_ESCAPES[raw[index + 1]]
                index += 2
            else:
                text += char
                index += 1
        out.append(text)
    return "".join(out).strip() or "(no console output)"


class Session:
    def __init__(self, image, elf):
        self.image = image
        self.elf = elf
        self.token = 0
        self.stopped = None
        self.buffer = ""
        self._spawn_qemu()
        self._spawn_gdb()
        self.mi(f"file {elf}")
        self.console("set architecture i386:x86-64")
        self.mi("target remote :" + GDB_PORT)
        self.serial_head = self._read_serial(2)

    def _spawn_qemu(self):
        if not os.path.exists("/dev/kvm"):
            raise RuntimeError("/dev/kvm missing: KVM is required, no silent TCG fallback")
        subprocess.run(
            ["pkill", "-f", f"serial file:{SERIAL_LOG}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.3)
        if os.path.exists(SERIAL_LOG):
            os.remove(SERIAL_LOG)
        self.qemu = subprocess.Popen(
            [
                "qemu-system-x86_64", "-accel", "kvm", "-display", "none", "-no-reboot",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                "-drive", f"format=raw,file={self.image}",
                "-serial", f"file:{SERIAL_LOG}", "-S", "-gdb", f"tcp::{GDB_PORT}",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def _spawn_gdb(self):
        self.gdb = subprocess.Popen(
            ["gdb", "-q", "-nx", "-i=mi2"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=0,
        )
        self.buffer = ""
        self._wait_for(lambda: "(gdb) " in self.buffer, timeout=20.0)
        self.buffer = ""

    def _pump(self, timeout):
        ready, _, _ = select.select([self.gdb.stdout], [], [], timeout)
        if not ready:
            return False
        char = self.gdb.stdout.read(1)
        if not char:
            raise RuntimeError("gdb MI child exited")
        self.buffer += char.decode("utf-8", "replace")
        return True

    def _wait_for(self, predicate, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate():
                return True
            self._pump(0.2)
        raise RuntimeError("gdb MI read timed out")

    def mi(self, command, timeout=20.0):
        self.token += 1
        token = str(self.token)
        self.gdb.stdin.write(f"{token}{command}\n".encode())
        self.gdb.stdin.flush()
        prefix = token + "^"

        self._wait_for(lambda: RE.search(rf"(?m)^{token}\^.+\n", self.buffer) is not None,
                       timeout)
        self._wait_for(lambda: self.buffer.rstrip().endswith("(gdb)"), timeout=5.0)
        lines = self.buffer.splitlines()
        self.buffer = ""
        answer = next(line for line in lines if line.startswith(prefix))
        if "^error" in answer:
            raise RuntimeError("gdb: " + answer)
        for line in lines:
            if line.startswith("*stopped"):
                self.stopped = line
        return answer, lines

    def console(self, command, timeout=20.0):
        self.token += 1
        token = str(self.token)
        self.gdb.stdin.write(f'{token}-interpreter-exec console "{command}"\n'.encode())
        self.gdb.stdin.flush()
        prefix = token + "^"

        self._wait_for(lambda: RE.search(rf"(?m)^{token}\^.+\n", self.buffer) is not None,
                       timeout)
        self._wait_for(lambda: self.buffer.rstrip().endswith("(gdb)"), timeout=5.0)
        lines = self.buffer.splitlines()
        self.buffer = ""
        answer = next(line for line in lines if line.startswith(prefix))
        if "^error" in answer:
            raise RuntimeError("gdb: " + answer)
        for line in lines:
            if line.startswith("*stopped"):
                self.stopped = line
        return decode_console_stream(lines)

    def continue_until_stop(self, timeout=STOP_TIMEOUT):
        self.token += 1
        token = str(self.token)
        self.gdb.stdin.write(f"{token}-exec-continue\n".encode())
        self.gdb.stdin.flush()
        prefix = token + "^"
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._pump(0.5)
            for line in self.buffer.splitlines():
                if line.startswith(prefix) and "^error" in line:
                    self.buffer = ""
                    raise RuntimeError("gdb: " + line)
            marker = RE.search(r"(?m)^\*stopped.+\n", self.buffer)
            if marker is not None:
                stopped_line = marker.group(0).strip()
                self.buffer = ""
                self.stopped = stopped_line
                return stopped_line
        raise RuntimeError("continue timed out without the machine stopping")

    def _read_serial(self, lines):
        try:
            with open(SERIAL_LOG, "r") as handle:
                tail = handle.readlines()[-lines:]
            return "".join(tail).rstrip()
        except FileNotFoundError:
            return ""

    def serial(self, lines):
        return self._read_serial(lines)

    def close(self):
        for proc in (self.gdb, getattr(self, "qemu", None)):
            if proc is None:
                continue
            try:
                proc.terminate()
                proc.wait(timeout=5)
            except Exception:
                proc.kill()
        self.gdb = None
        self.qemu = None
        return self._read_serial(20)
