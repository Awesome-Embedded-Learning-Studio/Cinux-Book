#!/usr/bin/env python3
"""Station 35's GUI verdict: the desk lights up, clicks open a terminal,
typing reaches the shell under it, and the pointer drags a window.

Boots the volume under QEMU with the USB keyboard and tablet detached, so
the desk's input nodes are the PS/2 pair this station wired. Every step
asserts on pixels from monitor screendumps (PPM, stdlib-only decode) or on
the serial transcript; a step that cannot prove itself fails the run.
"""

import argparse
import os
import select
import pathlib
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "mcp"))
from frame import read_ppm
from qemu_args import device_args, qemu_command


def lit_in(shot, x0, y0, x1, y1, threshold=40):
    width, _, pixels = shot
    lit = 0
    for y in range(y0, y1):
        row = y * width
        for x in range(x0, x1):
            at = (row + x) * 3
            if pixels[at] > threshold or pixels[at + 1] > threshold or pixels[at + 2] > threshold:
                lit += 1
    return lit


def changed_between(before, after, x0, y0, x1, y1):
    wa, ha, pa = before
    wb, hb, pb = after
    if (wa, ha) != (wb, hb):
        raise RuntimeError("screendump geometry changed between shots")
    changed = 0
    for y in range(y0, y1):
        row = y * wa
        for x in range(x0, x1):
            at = (row + x) * 3
            if pa[at : at + 3] != pb[at : at + 3]:
                changed += 1
    return changed


class Monitor:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(5.0)
        self.socket.connect(str(path))
        time.sleep(0.2)

    def run(self, *commands, pause=0.25):
        for command in commands:
            self.socket.sendall((command + "\n").encode())
            time.sleep(pause)
        try:
            self.socket.recv(65536)
        except socket.timeout:
            pass

    def sendkey(self, *keys):
        self.run(*[f"sendkey {key}" for key in keys])

    def close(self):
        self.socket.close()


SYMBOL_KEYS = {
    " ": "spc", "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
    ";": "semicolon", "'": "apostrophe", "`": "grave_accent", "\\": "backslash",
    ",": "comma", ".": "dot", "/": "slash", "\t": "tab",
}
SHIFTED_KEYS = {"~": "grave_accent", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5",
                "^": "6", "&": "7", "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal",
                "{": "bracket_left", "}": "bracket_right", "|": "backslash", ":": "semicolon",
                "\"": "apostrophe", "<": "comma", ">": "dot", "?": "slash"}


def key_name(ch):
    if ch in SYMBOL_KEYS:
        return [SYMBOL_KEYS[ch]]
    if ch in SHIFTED_KEYS:
        return ["shift-" + SHIFTED_KEYS[ch]]
    if ch.isupper():
        return ["shift-" + ch.lower()]
    return [ch]


def type_text(monitor, text):
    for ch in text:
        monitor.sendkey(*key_name(ch))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", default="build/boot/cinux.img")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--keep", action="store_true", help="keep shots under build/station35/")
    args = parser.parse_args()

    build = pathlib.Path(args.image).resolve().parent.parent
    shots = build / "station35"
    shots.mkdir(exist_ok=True)
    mon_path = shots / "monitor.sock"
    if mon_path.exists():
        mon_path.unlink()

    flags = device_args(args.image)
    devices = []
    index = 0
    while index < len(flags):
        takes_value = flags[index] in ("-accel", "-cpu", "-smp", "-display", "-device",
                                       "-drive", "-netdev", "-serial", "-monitor")
        if (flags[index] == "-device" and index + 1 < len(flags)
                and ("usb-kbd" in flags[index + 1] or "usb-tablet" in flags[index + 1])):
            index += 2
            continue
        devices.append(flags[index])
        if takes_value:
            devices.append(flags[index + 1])
            index += 1
        index += 1
    process = subprocess.Popen(
        [*qemu_command(args.image), *devices, "-snapshot",
         "-drive", f"format=raw,file={args.image}",
         "-serial", "stdio",
         "-monitor", f"unix:{mon_path},server,nowait"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    transcript = bytearray()

    def pump(seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited early: {process.returncode}")
            if select.select([process.stdout], [], [], 0.1)[0]:
                transcript.extend(os.read(process.stdout.fileno(), 65536))

    def wait_for(pattern):
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if re.search(pattern, transcript):
                return
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited: {process.returncode}")
            pump(0.1)
        raise RuntimeError(f"marker timed out: {pattern!r}")

    def shot(name):
        path = shots / f"{name}.ppm"
        monitor.run(f"screendump {path}")
        time.sleep(0.4)
        return read_ppm(path)

    def fail(message):
        raise RuntimeError(message)

    monitor = None
    try:
        wait_for(rb"vesper-fbdev: running")
        monitor = Monitor(mon_path)
        pump(1.0)

        base = shot("01-desk")
        width, height, _ = base
        paper_height = height - 32
        desk_lit = lit_in(base, 0, 0, width, paper_height)
        if desk_lit < width * paper_height * 0.95:
            fail(f"the desktop did not fill the framebuffer: {desk_lit} lit pixels")

        monitor.run("mouse_move 48 124", pause=0.1)
        pump(0.2)
        pointed = shot("01-pointer")
        if changed_between(base, pointed, 48, 124, 60, 142) < 50:
            fail("moving the pointer painted no software cursor")
        if changed_between(base, pointed, 0, 0, 12, 18) < 50:
            fail("moving the pointer left its old cursor behind")

        monitor.run("mouse_move -2 6", pause=0.1)
        pump(0.2)
        small_move = shot("01-pointer-small-move")
        if changed_between(pointed, small_move, 46, 124, 60, 148) < 50:
            fail("reply-valued motion bytes lost the pointer's small movement")
        monitor.run("mouse_move 2 -6", pause=0.1)
        pump(0.2)
        restored = shot("01-pointer-restored")
        if restored != pointed:
            fail("opposite small movements did not restore the pointer and desktop")

        monitor.run("mouse_button 1", "mouse_button 0", pause=0.05)
        time.sleep(0.1)
        monitor.run("mouse_button 1", "mouse_button 0", pause=0.05)
        pump(1.0)
        opened = shot("02-terminal-open")
        if changed_between(base, opened, 16, 40, 620, 440) < 5000:
            fail("double-clicking the TERMINAL icon opened no window")

        monitor.sendkey("e", "c", "h", "o", "spc", "h", "i", "ret")
        type_text(monitor, "echo proof-lives >/tmp/guip")
        monitor.sendkey("ret")
        pump(1.5)
        process.stdin.write(b"cat /tmp/guip; echo SERIAL_GP_$?\n")
        process.stdin.flush()
        wait_for(rb"proof-lives")
        wait_for(rb"SERIAL_GP_0")
        typed = shot("03-typed")
        if changed_between(opened, typed, 24, 56, 600, 440) < 200:
            fail("typing did not reach the shell under the desk")

        if re.search(rb"\r?\necho hi\r?\n", transcript):
            fail("typed keys leaked to the console shell: the grab did not hold")

        monitor.run("mouse_move 252 -76")
        monitor.run("mouse_button 1")
        for step in range(1, 11):
            monitor.run("mouse_move 10 0")
        monitor.run("mouse_button 0")
        pump(1.0)
        dragged = shot("04-dragged")
        if changed_between(typed, dragged, 16, 36, 140, 60) < 200:
            fail("the title bar drag moved no window")

        print(f"station35: {width}x{height} desk filled, cursor moved and restored, "
              "terminal opened, typing answered, window dragged")
    finally:
        if monitor is not None:
            monitor.close()
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
        if not args.keep:
            for shot_file in shots.glob("*.ppm"):
                shot_file.unlink()
            if mon_path.exists():
                mon_path.unlink()


if __name__ == "__main__":
    main()
