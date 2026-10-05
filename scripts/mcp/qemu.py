#!/usr/bin/env python3
import os
import socket
import subprocess
import time

import frame

QEMU = "qemu-system-x86_64"
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
                QEMU, "-accel", "kvm", "-display", "none", "-no-reboot",
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
        try:
            sock.recv(65536)
        except OSError:
            pass
        sock.send((command + "\n").encode())
        time.sleep(0.15 if command else 0.0)
        try:
            sock.settimeout(0.5)
            return sock.recv(65536).decode(errors="replace")
        except socket.timeout:
            return ""

    def _serial_text(self):
        try:
            with open(SERIAL_PATH, "r", errors="replace") as handle:
                return handle.read()
        except FileNotFoundError:
            return ""

    def _serial_tail(self, lines):
        return "\n".join(self._serial_text().splitlines()[-lines:])

    def sendkeys(self, keys):
        for key in keys:
            self._monitor_send(f"sendkey {key}")
            time.sleep(0.2)
        return f"sent {len(keys)} key(s)"

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
