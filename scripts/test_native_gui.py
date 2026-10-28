#!/usr/bin/env python3
"""Run the native CMake commands through the desktop's PS/2-to-PTY path."""
import argparse
import subprocess
import pathlib
import re
import time

from test_station34_kernel_selfhost import Guest
from test_station35_gui import Monitor, changed_between, read_ppm


def type_text(monitor, line):
    punctuation = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus',
                   '>': 'shift-dot', '$': 'shift-4', '?': 'shift-slash', ';': 'semicolon'}
    for letter in line:
        if letter in punctuation:
            key = punctuation[letter]
        elif letter.isupper():
            key = 'shift-' + letter.lower()
        else:
            key = letter
        monitor.run(f'sendkey {key} 30', pause=0.06)


def type_line(monitor, line):
    type_text(monitor, line)
    monitor.run('sendkey ret 30', pause=0.1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', type=pathlib.Path, default=pathlib.Path('build/boot/cinux.img'))
    parser.add_argument('--volume', type=pathlib.Path, help='source volume copied before testing')
    args = parser.parse_args()
    image = args.image.resolve()
    build = image.parent.parent
    shots = build / 'native-gui'
    shots.mkdir(exist_ok=True)
    source = args.volume.resolve() if args.volume else build / 'boot/native-build.ext2'
    volume = shots / 'rootfs.ext2'
    if source == volume:
        parser.error('--volume must name a separate source image')
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(source), str(volume)], check=True)
    socket = shots / 'monitor.sock'
    socket.unlink(missing_ok=True)
    guest = Guest(image, volume,
                  shots / 'serial.log', 120, memory=1024, monitor=socket, ps2_only=True, gui=True)
    monitor = None
    try:
        guest.wait_for(rb'Cinux up\r?\n')
        guest.wait_for_prompt()
        guest.wait_for(rb'vesper-fbdev: running')
        guest.command('rm -f /tmp/gui-configure-status /tmp/gui-build-status; echo RESET_$?', 'RESET')
        monitor = Monitor(socket)
        monitor.run(f'screendump {shots / "desktop.ppm"}')
        monitor.run('mouse_move 48 124', 'mouse_button 1', 'mouse_button 0',
                    'mouse_button 1', 'mouse_button 0', pause=0.08)
        time.sleep(1)
        monitor.run(f'screendump {shots / "terminal.ppm"}')
        if changed_between(read_ppm(shots / 'desktop.ppm'), read_ppm(shots / 'terminal.ppm'),
                           80, 40, 600, 400) < 1000:
            raise RuntimeError('double-click did not open a terminal')
        type_line(monitor, 'cd /home/cinux/Cinux')
        type_line(monitor, 'cmake -B build -S .')
        type_line(monitor, 'echo $? > /tmp/gui-configure-status')
        for _ in range(30):
            output = guest.command('if test -f /tmp/gui-configure-status; then cat /tmp/gui-configure-status; fi; echo STATUS_$?', 'STATUS')
            if re.search(rb'\r?\n0\r?\nSTATUS_0', output):
                break
            time.sleep(1)
        else:
            monitor.run(f'screendump {shots / "configure-failed.ppm"}')
            raise RuntimeError('GUI CMake configure did not succeed')
        output = guest.command('cd /home/cinux/Cinux; python3 -c "import os; print(\'SOURCE_STATE\', os.stat(\'kernel/kernel.cpp\').st_size, int(os.stat(\'build/kernel/CMakeFiles/cinux_kernel.dir/kernel.cpp.o\').st_mtime))"; echo BEFORE_$?', 'BEFORE')
        before = re.search(rb'SOURCE_STATE (\d+) (\d+)', output)
        if not before:
            raise RuntimeError('source and object metadata missing')
        type_line(monitor, 'echo >> kernel/kernel.cpp')
        type_line(monitor, 'cmake --build build -j')
        type_line(monitor, 'echo $? > /tmp/gui-build-status')
        for _ in range(120):
            output = guest.command('if test -f /tmp/gui-build-status; then cat /tmp/gui-build-status; fi; echo STATUS_$?', 'STATUS')
            if re.search(rb'\r?\n0\r?\nSTATUS_0', output):
                break
            time.sleep(1)
        else:
            monitor.run(f'screendump {shots / "build-failed.ppm"}')
            raise RuntimeError('GUI CMake build did not succeed')
        guest.command(f'python3 -c "import os; assert os.stat(\'kernel/kernel.cpp\').st_size == {int(before[1]) + 1}; assert os.stat(\'build/kernel/CMakeFiles/cinux_kernel.dir/kernel.cpp.o\').st_mtime > {int(before[2])}"; echo RECOMPILED_$?', 'RECOMPILED')
        monitor.run(f'screendump {shots / "cmake-success.ppm"}')
        guest.command('python3 -B -S scripts/native-build-proof.py; echo SYNC_$?', 'SYNC')
        if re.search(rb'\[kern\] exception|syscall \d+ unserved', guest.transcript):
            raise RuntimeError('exception or unserved syscall during GUI build')
        print('[native-gui] PASS: PS/2 input, PTY login shell, CMake configure and kernel rebuild')
    finally:
        if monitor:
            monitor.close()
        guest.close()


if __name__ == '__main__':
    main()
