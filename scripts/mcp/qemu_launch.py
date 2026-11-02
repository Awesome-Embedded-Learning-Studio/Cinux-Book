#!/usr/bin/env python3
"""Exec QEMU with the host-side libslirp main-loop wakeup fix."""
import os
import pathlib
import sys


def main():
    if len(sys.argv) < 3:
        raise SystemExit('usage: qemu_launch.py <qemu> <wakeup-library> [QEMU arguments...]')
    program, library, *arguments = sys.argv[1:]
    environment = os.environ.copy()
    if environment.get('CINUX_QEMU_SLIRP_WAKEUP', '1') != '0':
        if not pathlib.Path(library).is_file():
            raise SystemExit('QEMU wakeup library is missing; build the qemu_disks target first: '
                             + library)
        previous = environment.get('LD_PRELOAD')
        environment['LD_PRELOAD'] = library + (':' + previous if previous else '')
    # Preserve the VM's PID, exit status, signals and gdbstub lifecycle.
    os.execvpe(program, [program, *arguments], environment)


if __name__ == '__main__':
    main()
