#!/usr/bin/env python3
"""Exercise shell completion without refocusing the desktop terminal."""
import argparse
import pathlib
import re
import subprocess
import time

from test_native_gui import type_line, type_text
from test_station34_kernel_selfhost import Guest
from test_station35_gui import Monitor
from test_station35_gui import read_ppm


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=pathlib.Path, default=pathlib.Path('build'))
    parser.add_argument('--volume', type=pathlib.Path,
                        default=pathlib.Path('build/boot/native-build.ext2'))
    parser.add_argument('--vesper', type=pathlib.Path)
    parser.add_argument('--output-dir', type=pathlib.Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    shots = args.output_dir.resolve() if args.output_dir else build / 'gui-tab'
    shots.mkdir(parents=True, exist_ok=True)
    volume = shots / 'rootfs.ext2'
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(args.volume.resolve()),
                    str(volume)], check=True)
    if args.vesper:
        commands = shots / 'update.debugfs'
        commands.write_text(f'rm /bin/vesper\nwrite {args.vesper.resolve()} /bin/vesper\n')
        subprocess.run(['debugfs', '-w', '-f', str(commands), str(volume)], check=True,
                       capture_output=True)
    socket = shots / 'monitor.sock'
    socket.unlink(missing_ok=True)
    guest = Guest(build / 'boot/cinux.img', volume, shots / 'serial.log', 120,
                  memory=1024, monitor=socket, ps2_only=True, gui=True)
    monitor = None

    def expect_file(path, value):
        for _ in range(10):
            output = guest.command(f'if test -f {path}; then cat {path}; fi; echo TAB_$?', 'TAB')
            if re.search(rb'\r?\n' + re.escape(value) + rb'\r?\nTAB_0', output):
                return
            time.sleep(0.2)
        monitor.run(f'screendump {shots / "failed.ppm"}')
        raise RuntimeError(f'Tab completion lost input or produced the wrong result: {path}')

    try:
        guest.wait_for(rb'Cinux up\r?\n')
        guest.wait_for_prompt()
        guest.wait_for(rb'vesper-fbdev: running')
        guest.command('rm -f /tmp/gui-tab-pwd /tmp/gui-tab-source /tmp/gui-tab-alive /tmp/gui-tab-user; '
                      'mkdir /tmp/gui-tab-candidates; '
                      'mkdir /tmp/gui-tab-candidates/alpha /tmp/gui-tab-candidates/alpine; '
                      'echo SETUP_$?', 'SETUP')
        monitor = Monitor(socket)
        monitor.run('mouse_move 48 124', 'mouse_button 1', 'mouse_button 0',
                    'mouse_button 1', 'mouse_button 0', pause=0.08)
        time.sleep(1)
        type_line(monitor, 'id -u > /tmp/gui-tab-user')
        expect_file('/tmp/gui-tab-user', b'1000')
        type_text(monitor, 'cd /home/cinux/C')
        monitor.run('sendkey tab 30', pause=0.2)
        type_line(monitor, '; pwd > /tmp/gui-tab-pwd')
        expect_file('/tmp/gui-tab-pwd', b'/home/cinux/Cinux')
        type_text(monitor, 'cat /home/cinux/C')
        monitor.run('sendkey tab 30', pause=0.2)
        type_text(monitor, 'CMa')
        monitor.run('sendkey tab 30', pause=0.2)
        type_line(monitor, ' > /tmp/gui-tab-source')
        for _ in range(10):
            output = guest.command('if test -s /tmp/gui-tab-source; then '
                                   'cmp /home/cinux/Cinux/CMakeLists.txt /tmp/gui-tab-source && '
                                   'echo SOURCE_MATCH; fi; echo TAB_$?', 'TAB')
            if re.search(rb'\r?\nSOURCE_MATCH\r?\n', output):
                break
            time.sleep(0.2)
        else:
            raise RuntimeError('directory and filename completion did not preserve input')
        monitor.run(f'screendump {shots / "pre-candidates.ppm"}')
        type_text(monitor, 'ls /tmp/gui-tab-candidates/al')
        monitor.run('sendkey tab 30', 'sendkey tab 30', pause=0.2)
        time.sleep(0.5)
        monitor.run(f'screendump {shots / "candidates.ppm"}')
        before = read_ppm(shots / 'pre-candidates.ppm')
        listing = read_ppm(shots / 'candidates.ppm')
        listing_changed = sum(
            1
            for index in range(0, min(len(before[2]), len(listing[2])), 3)
            if abs(before[2][index] - listing[2][index]) > 40
        )
        if listing_changed < 500:
            raise RuntimeError('double Tab listed no candidates on the terminal')
        monitor.run('sendkey ctrl-u 30', pause=0.2)
        type_line(monitor, 'echo alive > /tmp/gui-tab-alive')
        expect_file('/tmp/gui-tab-alive', b'alive')
        monitor.run(f'screendump {shots / "success.ppm"}')
        if re.search(rb'\[kern\] exception|syscall \d+ unserved', guest.transcript):
            raise RuntimeError('exception or unserved syscall during completion')
        print('[gui-tab] PASS: directory, filename and repeated Tab; continued typing without refocus')
    finally:
        if monitor:
            monitor.close()
        guest.close()


if __name__ == '__main__':
    main()
