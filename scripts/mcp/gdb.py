"""GDB/MI driver plus the QEMU process it debugs.

One Session owns a frozen-at-reset QEMU (private local gdbstub, serial to a
file) and a persistent gdb -i=mi2 child talking to it. The MI child
stays alive across tool calls, so register peeks cost one round trip,
not the file/target-remote ceremony a cold gdb invocation needs.

MI result records acknowledge commands; asynchronous records describe
target state. Neither command completion nor stopping depends on a prompt.
"""

import glob
import json
import os
import re as RE
import select
import signal
import socket
import subprocess
import tempfile
import time

STOP_TIMEOUT = 10.0
COMMAND_TIMEOUT = 5.0
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

GENERAL_REGS = [
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "rip", "eflags",
]


def resolve(default, pattern, what):
    if default:
        path = default if os.path.isabs(default) else os.path.join(REPO_ROOT, default)
        if not os.path.isfile(path):
            raise ValueError(f"{what} does not exist: {path}")
        return os.path.abspath(path)
    matches = sorted(glob.glob(os.path.join(REPO_ROOT, pattern)))
    if not matches:
        raise RuntimeError(f"no {what} given and {pattern} is missing")
    return matches[0]


STREAM_ITEM = RE.compile(r'^~"(.*)"$')
STREAM_ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "r": "\r"}


class SessionDisconnected(RuntimeError):
    pass


class CommandTimeout(RuntimeError):
    pass


class GdbError(RuntimeError):
    pass


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
        self.state = "disconnected"
        self.gdb = None
        self.qemu = None
        self._breakpoints = {}
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.gdb_port = listener.getsockname()[1]
        self.log_dir = tempfile.mkdtemp(prefix="cinux-gdb-")
        self.serial_log = os.path.join(self.log_dir, "serial.log")
        self.qemu_log = os.path.join(self.log_dir, "qemu.log")
        try:
            self._spawn_qemu()
            self._attach()
            self.serial_head = self._read_serial(2)
        except Exception:
            self.close()
            raise

    def _attach(self):
        self._spawn_gdb()
        self.mi("-gdb-set pagination off")
        self.mi("-gdb-set confirm off")
        self.mi("-gdb-set mi-async on")
        self.mi("-gdb-set remotetimeout 2")
        self.mi("-file-exec-and-symbols " + json.dumps(self.elf, ensure_ascii=False))
        self.console("set architecture i386:x86-64")
        deadline = time.monotonic() + COMMAND_TIMEOUT
        while True:
            try:
                self.mi(f"-target-select remote 127.0.0.1:{self.gdb_port}")
                break
            except GdbError:
                if time.monotonic() >= deadline:
                    raise
                self._check_alive()
                time.sleep(0.05)
        if not self._wait_for(lambda: self.state == "stopped", 2.0):
            raise CommandTimeout("attached but no initial stop event; call debug_reconnect")

    def reconnect(self):
        """Replace only GDB; keep the VM, disks and serial log intact."""
        if self.qemu is None or self.qemu.poll() is not None:
            raise SessionDisconnected("QEMU exited; call debug_boot to start a new machine")
        saved = list(self._breakpoints.values())
        self._terminate(self.gdb)
        self.gdb = None
        self.state = "disconnected"
        self.stopped = None
        self._attach()
        self._breakpoints.clear()
        for location, hardware, temporary in saved:
            self.breakpoint(location, hardware, temporary)
        return self.stopped

    def _spawn_qemu(self):
        if not os.path.exists("/dev/kvm"):
            raise RuntimeError("/dev/kvm missing: KVM is required, no silent TCG fallback")
        # Keep diagnostics and avoid killing another debugger's QEMU.
        with open(self.qemu_log, "wb") as errors:
            self.qemu = subprocess.Popen(
                [
                    "qemu-system-x86_64", "-accel", "kvm", "-display", "none", "-no-reboot",
                    "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                    "-device", "ahci,id=sata0",
                    "-drive", f"format=raw,file={self.image}",
                    "-drive", f"format=raw,file={os.path.join(REPO_ROOT, 'build', 'boot', 'ext2.img')},if=none,id=spare",
                    "-device", "ide-hd,drive=spare,bus=sata0.0",
                    "-serial", f"file:{self.serial_log}", "-S", "-gdb", f"tcp:127.0.0.1:{self.gdb_port}",
                ],
                stdout=subprocess.DEVNULL,
                stderr=errors,
            )

    def _spawn_gdb(self):
        self.gdb = subprocess.Popen(
            ["gdb", "-q", "-nx", "-i=mi2"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=0,
        )
        self.buffer = b""
        self._results = {}
        self._command_lines = None
        self._needs_reconnect = False

    def _check_alive(self):
        if self.qemu is not None and self.qemu.poll() is not None:
            with open(self.qemu_log, errors="replace") as errors:
                detail = errors.read().strip()
            raise SessionDisconnected(f"QEMU exited ({self.qemu.returncode}): {detail}; "
                                      "serial log retained; call debug_boot")
        if self.gdb is None or self.gdb.poll() is not None:
            raise SessionDisconnected("GDB exited; VM and serial log retained; "
                                      "call debug_reconnect")
        if self._needs_reconnect:
            raise SessionDisconnected("GDB command is still pending; call debug_reconnect")

    def _record(self, line):
        if self._command_lines is not None:
            self._command_lines.append(line)
        result = RE.match(r"^(\d+)\^", line)
        if result:
            self._results[result.group(1)] = line
            # Timed-out commands may reply later; keep bounded history.
            if len(self._results) > 16:
                self._results.pop(next(iter(self._results)))
        event = RE.sub(r"^\d+(?=\*)", "", line)
        if event.startswith("*running"):
            self.state = "running"
            self.stopped = None
        elif event.startswith("*stopped"):
            self.stopped = event
            self.state = "exited" if 'reason="exited' in event else "stopped"
            hit = RE.search(r'bkptno="([^"]+)"', event)
            if hit and self._breakpoints.get(hit.group(1), (None, None, False))[2]:
                self._breakpoints.pop(hit.group(1), None)
        elif event.startswith("=thread-group-exited"):
            self.state = "exited"
        elif event.startswith("=breakpoint-deleted"):
            number = RE.search(r'id="([^"]+)"', event)
            if number:
                self._breakpoints.pop(number.group(1), None)

    def _pump(self, timeout):
        self._check_alive()
        ready, _, _ = select.select([self.gdb.stdout], [], [], timeout)
        if not ready:
            return False
        chunk = os.read(self.gdb.stdout.fileno(), 65536)
        if not chunk:
            raise SessionDisconnected("GDB connection closed; call debug_reconnect")
        self.buffer += chunk
        while self.buffer:
            if self.buffer.startswith(b"(gdb)"):
                self.buffer = self.buffer[5:].lstrip(b" \r\n")
            elif b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                self._record(line.decode("utf-8", "replace").strip())
            else:
                break
        return True

    def _wait_for(self, predicate, timeout):
        deadline = time.monotonic() + timeout
        while not predicate():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            self._pump(min(0.1, remaining))
        return True

    def refresh(self):
        self._check_alive()
        deadline = time.monotonic() + 0.05
        while time.monotonic() < deadline and self._pump(0):
            pass

    def mi(self, command, timeout=COMMAND_TIMEOUT):
        self.refresh()
        self.token += 1
        token = str(self.token)
        if "\n" in command or "\r" in command:
            raise ValueError("MI command must be a single line")
        self._command_lines = []
        try:
            self.gdb.stdin.write(f"{token}{command}\n".encode())
            self.gdb.stdin.flush()
            if not self._wait_for(lambda: token in self._results, timeout):
                # A foreground debug_eval command can block GDB. Cancel it,
                # then consume its acknowledgement before the next tool call.
                self.gdb.send_signal(signal.SIGINT)
                recovered = self._wait_for(lambda: token in self._results, 2.0)
                self._results.pop(token, None)
                self._needs_reconnect = not recovered
                hint = "session retained" if recovered else "call debug_reconnect"
                raise CommandTimeout(f"GDB command timed out after {timeout:g}s; {hint}")
            answer = self._results.pop(token)
            if "^error" in answer:
                raise GdbError("gdb: " + answer)
            return answer, self._command_lines[:]
        except (BrokenPipeError, OSError) as failure:
            raise SessionDisconnected("GDB pipe failed; call debug_reconnect") from failure
        finally:
            self._command_lines = None

    def console(self, command, timeout=COMMAND_TIMEOUT):
        _, lines = self.mi("-interpreter-exec console " + json.dumps(command, ensure_ascii=False), timeout)
        return decode_console_stream(lines)

    def breakpoint(self, location, hardware=True, temporary=False):
        flags = ("-h " if hardware else "") + ("-t " if temporary else "")
        answer, _ = self.mi("-break-insert " + flags + json.dumps(location, ensure_ascii=False))
        number = RE.search(r'number="([^"]+)"', answer)
        if number:
            self._breakpoints[number.group(1)] = (location, hardware, temporary)
        return answer

    def wait_until_stop(self, timeout=STOP_TIMEOUT):
        self.refresh()
        if self.state == "running":
            self._wait_for(lambda: self.state != "running", timeout)
        return self.stopped if self.state != "running" else None

    def continue_until_stop(self, timeout=STOP_TIMEOUT):
        self.refresh()
        if self.state != "running":
            return self._execute("-exec-continue", timeout)
        return self.wait_until_stop(timeout)

    def _execute(self, command, timeout):
        # The result record can precede *running in a separate pipe read.
        # Clear the old stop before sending so a step cannot return stale data.
        old_state, old_stop = self.state, self.stopped
        self.state, self.stopped = "running", None
        try:
            self.mi(command)
        except GdbError:
            self.state, self.stopped = old_state, old_stop
            raise
        return self.wait_until_stop(timeout)

    def step(self, timeout=STOP_TIMEOUT):
        self.refresh()
        if self.state == "running":
            raise RuntimeError("target is running; call debug_interrupt before stepping")
        return self._execute("-exec-step-instruction", timeout)

    def interrupt(self, timeout=STOP_TIMEOUT):
        self.refresh()
        if self.state == "running":
            self.mi("-exec-interrupt --all")
        return self.wait_until_stop(timeout)

    def _read_serial(self, lines):
        try:
            if lines < 0:
                raise ValueError("lines must be non-negative")
            with open(self.serial_log, "r", errors="replace") as handle:
                tail = handle.readlines()[-lines:] if lines else []
            return "".join(tail).rstrip()
        except FileNotFoundError:
            return ""

    def serial(self, lines):
        return self._read_serial(lines)

    @staticmethod
    def _terminate(proc):
        if proc is None:
            return
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        for pipe in (proc.stdin, proc.stdout, proc.stderr):
            if pipe is not None:
                pipe.close()

    def close(self):
        try:
            self._terminate(self.gdb)
        finally:
            self._terminate(self.qemu)
        self.gdb = None
        self.qemu = None
        self.state = "closed"
        return self._read_serial(20)
