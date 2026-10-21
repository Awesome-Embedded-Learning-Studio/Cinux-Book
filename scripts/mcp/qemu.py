#!/usr/bin/env python3
import os
import re
import socket
import subprocess
import time

import frame
from qemu_args import device_args

QEMU = "qemu-system-x86_64"
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VOLUME_PATH = os.path.join(REPO_ROOT, "build", "boot", "ext2.img")
MON_PATH = "/tmp/cinux_qemu_mon"
SERIAL_PATH = "/tmp/cinux_qemu_serial.log"
DUMP_PATH = "/tmp/cinux_qemu_screen.ppm"
PNG_PATH = "/tmp/cinux_qemu_screen.png"
BOOT_WAIT_SECONDS = 4.0


class Session:
    def __init__(self, image):
        self.image = image
        self.mon = None
        self.proc = None

    def boot(self):
        for stale in (MON_PATH, SERIAL_PATH):
            if os.path.exists(stale):
                os.unlink(stale)
        self.proc = subprocess.Popen(
            [
                QEMU, *device_args(self.image),
                "-drive", f"format=raw,file={self.image}",
                "-serial", f"file:{SERIAL_PATH}",
                "-monitor", f"unix:{MON_PATH},server,nowait",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        deadline = time.time() + BOOT_WAIT_SECONDS
        while time.time() < deadline:
            if os.path.exists(SERIAL_PATH) and self._serial_text().count("[kern]") >= 1:
                break
            if self.proc.poll() is not None:
                err = self.proc.stderr.read().decode(errors="replace")
                raise RuntimeError(f"qemu exited early: {err.strip()}")
            time.sleep(0.1)
        self._monitor_send("")
        return self._serial_tail(6)

    def _monitor_connect(self):
        if self.mon is None:
            self.mon = socket.socket(socket.AF_UNIX)
            self.mon.connect(MON_PATH)
        return self.mon

    def _monitor_send(self, command):
        sock = self._monitor_connect()
        sock.settimeout(0.05)
        while True:
            try:
                if not sock.recv(65536):
                    break
            except OSError:
                break
        sock.sendall((command + "\n").encode())
        chunks = []
        deadline = time.time() + 2.0
        while time.time() < deadline:
            try:
                sock.settimeout(0.5)
                piece = sock.recv(65536)
            except socket.timeout:
                break
            if not piece:
                break
            chunks.append(piece.decode(errors="replace"))
            if chunks[-1].endswith("(qemu) "):
                break
        return "".join(chunks)

    @staticmethod
    def _monitor_diagnostics(reply):
        if "(qemu)" not in reply:
            return ["incomplete monitor reply"]
        text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", reply)
        lines = text.splitlines()
        lines = lines[1:] if lines else lines
        found = []
        for line in lines:
            line = line.replace("(qemu)", "").strip()
            if line:
                found.append(line)
        return found

    def _serial_text(self):
        try:
            with open(SERIAL_PATH, "r", errors="replace") as handle:
                return handle.read()
        except FileNotFoundError:
            return ""

    def _serial_tail(self, lines):
        return "\n".join(self._serial_text().splitlines()[-lines:])

    def sendkeys(self, keys):
        accepted = 0
        for index, key in enumerate(keys):
            command = f"sendkey {key}"
            diagnostics = self._monitor_diagnostics(self._monitor_send(command))
            if diagnostics:
                detail = "; ".join(diagnostics)
                return (f"key {index} ({key!r}) rejected by monitor after "
                        f"{accepted} accepted: {detail}")
            accepted += 1
            time.sleep(0.2)
        return f"sent {accepted} key(s)"

    def screendump(self, text_rows):
        self._monitor_send(f"screendump {DUMP_PATH}")
        time.sleep(0.6)
        width, height, pixels = frame.read_ppm(DUMP_PATH)
        frame.write_png(PNG_PATH, width, height, pixels)
        stats = []
        for row in range(min(text_rows, height // frame.GLYPH_HEIGHT)):
            lit = frame.count_lit(pixels, width, row)
            stats.append(f"row {row}: {lit} lit px")
        return "\n".join([f"{width}x{height}, png: {PNG_PATH}"] + stats)

    def serial(self, lines):
        return self._serial_tail(lines)

    def _read_phys(self, where, count):
        reply = self._monitor_send(f"xp /{count}gx {where}")
        values = []
        for line in reply.splitlines():
            if ":" not in line:
                continue
            for piece in line.split(":", 1)[1].split():
                try:
                    values.append(int(piece, 16))
                except ValueError:
                    pass
        return values

    def _cr3(self):
        reply = self._monitor_send("info registers")
        for line in reply.splitlines():
            if line.startswith("CR3"):
                return int(line.split("=")[1].split()[0], 16)
        raise RuntimeError("monitor did not report CR3")

    def pagewalk(self, address, full_level):
        cr3 = self._cr3()
        lines = [f"cr3={cr3:#x}"]
        names = ["pml4e", "pdpte", "pde", "pte"]
        shifts = [39, 30, 21, 12]
        table = cr3
        for depth in range(4):
            index = (address >> shifts[depth]) & 0x1FF
            entry = self._read_phys(table + index * 8, 1)
            if not entry:
                lines.append(f"{names[depth]}[{index}]: unreadable at {table + index * 8:#x}")
                break
            value = entry[0]
            flags = "".join([
                "P" if value & 1 else "-",
                "W" if value & 2 else "-",
                "U" if value & 4 else "-",
                "L" if value & 0x80 else "-",
            ])
            target = value & 0x000FFFFFFFFFF000
            lines.append(f"{names[depth]}[{index}] = {value:#018x} {flags} -> {target:#x}")
            if full_level:
                rows = self._read_phys(table, 512)
                live = [i for i, v in enumerate(rows) if v & 1]
                lines.append(f"  level summary: {len(live)} present of 512: {live}")
            if not value & 1 or depth == 3 or value & 0x80:
                break
            table = target
        return "\n".join(lines)

    def quit(self, lines):
        self._monitor_send("quit")
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        if self.mon is not None:
            self.mon.close()
            self.mon = None
        return self._serial_tail(lines)
