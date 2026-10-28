#!/usr/bin/env python3
"""Verify sync, native compilation, poweroff, and cold-boot persistence on a disk copy."""
import argparse
import hashlib
import os
import pathlib
import re
import select
import shlex
import subprocess
import time

from test_native_gui import type_line
from test_station34_kernel_selfhost import Guest
from test_station35_gui import Monitor


def debugfs_dump(volume, name, destination):
    result = subprocess.run(['debugfs', '-R', f'dump /home/cinux/persistence-demo/{name} {destination}', str(volume)], capture_output=True, text=True)
    return result.returncode == 0 and destination.exists()


def verify_sync_snapshot(snapshot, turn, cycle, expected_binary):
    """The first sync's snapshot must already carry this round's content.

    Reading the snapshot itself closes the loop a later sync or the
    poweroff flush would otherwise paper over: payload bytes, the edited
    source, the built binary at this instant, and nothing that was only
    written after the sync.
    """
    payload = turn / 'snapshot-payload.bin'
    if not debugfs_dump(snapshot, 'payload.bin', payload):
        raise RuntimeError('sync snapshot lacks payload.bin')
    if payload.read_bytes() != bytes([cycle % 256]) * 307200:
        raise RuntimeError('sync snapshot payload does not match this round')
    source = turn / 'snapshot-main.cpp'
    if not debugfs_dump(snapshot, 'main.cpp', source):
        raise RuntimeError('sync snapshot lacks the edited source')
    if f'Round {cycle} up to' not in source.read_text():
        raise RuntimeError('sync snapshot source predates this round edit')
    binary = turn / 'snapshot-mini-primes'
    if not debugfs_dump(snapshot, 'build/mini-primes', binary):
        raise RuntimeError('sync snapshot lacks the built binary')
    if hashlib.sha256(binary.read_bytes()).hexdigest() != expected_binary:
        raise RuntimeError('sync snapshot binary differs from the guest hash')
    for absent in ('pending', 'renamed'):
        probe = turn / f'snapshot-{absent}'
        probe.unlink(missing_ok=True)
        if debugfs_dump(snapshot, absent, probe):
            raise RuntimeError(f'sync snapshot unexpectedly carries {absent}')
    after_sync = turn / 'snapshot-after-sync'
    after_sync.unlink(missing_ok=True)
    if debugfs_dump(snapshot, 'after-sync', after_sync):
        expected_prior = f'round-{cycle - 1}'.encode() if cycle > 0 else None
        if after_sync.read_bytes() != expected_prior:
            raise RuntimeError('sync snapshot after-sync predates or postdates its round')


def check_disk(volume, log):
    result = subprocess.run(['e2fsck', '-fn', str(volume)], capture_output=True, text=True)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'filesystem check failed: {log}')


def python_command(guest, program, marker):
    return guest.command('python3 -B -S -c ' + shlex.quote(program) + f'; echo {marker}_$?', marker)


def wait_for_poweroff(guest):
    deadline = time.monotonic() + 30
    while guest.process.poll() is None and time.monotonic() < deadline:
        if select.select([guest.process.stdout], [], [], 0.2)[0]:
            guest.transcript.extend(os.read(guest.process.stdout.fileno(), 65536))
            guest.log.write_bytes(guest.transcript)
    if guest.process.poll() != 0:
        raise RuntimeError('poweroff did not exit QEMU successfully')
    guest.transcript.extend(guest.process.stdout.read())
    guest.log.write_bytes(guest.transcript)


def check_signal_wait(guest):
    program = """import os, signal, time
signals={signal.SIGUSR1}
old=signal.pthread_sigmask(signal.SIG_BLOCK, signals)
parent=os.getpid()
child=os.fork()
if child==0:
    time.sleep(0.05)
    os.kill(parent, signal.SIGUSR1)
    os._exit(0)
assert signal.sigtimedwait(signals, 1).si_signo==signal.SIGUSR1
os.waitpid(child, 0)
assert signal.sigtimedwait(signals, 0.01) is None
signal.pthread_sigmask(signal.SIG_SETMASK, old)
"""
    python_command(guest, program, 'SIGNALWAIT')


def build_project(guest, monitor, work):
    commands = ['cd /home/cinux/persistence-demo', 'cmake -B build -S .',
                'cmake --build build -j', './build/mini-primes 30']
    if monitor is None:
        for index, command in enumerate(commands):
            guest.command(command + f'; echo BUILD{index}_$?', f'BUILD{index}')
        return
    monitor.run('mouse_move 48 124', 'mouse_button 1', 'mouse_button 0',
                'mouse_button 1', 'mouse_button 0', pause=0.08)
    time.sleep(1)
    for command in commands:
        type_line(monitor, command)
    type_line(monitor, './build/mini-primes 30 > /tmp/persistence-result')
    type_line(monitor, 'echo $? > /tmp/persistence-status')
    for _ in range(90):
        output = guest.command('if test -f /tmp/persistence-status; then cat /tmp/persistence-status; fi; echo STATUS_$?', 'STATUS')
        if re.search(rb'\r?\n0\r?\nSTATUS_0', output):
            break
        time.sleep(1)
    else:
        raise RuntimeError('GUI native build failed; see serial log')
    result = guest.command('cat /tmp/persistence-result; echo RESULT_$?', 'RESULT')
    if b'Found 10 primes.' not in result:
        raise RuntimeError('GUI program output mismatch')
    monitor.run(f'screendump {work / "compiled.ppm"}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', type=pathlib.Path, default=pathlib.Path('build/boot/cinux.img'))
    parser.add_argument('--volume', type=pathlib.Path)
    parser.add_argument('--cycles', type=int, default=3)
    parser.add_argument('--gui', action='store_true')
    args = parser.parse_args()
    if args.cycles < 2:
        parser.error('--cycles must be at least 2')
    image = args.image.resolve()
    build = image.parent.parent
    source = args.volume.resolve() if args.volume else build / 'boot/native-build.ext2'
    work = build / ('persistence-gui' if args.gui else 'persistence-smoke')
    work.mkdir(exist_ok=True)
    volume = work / 'rootfs.ext2'
    if source == volume:
        parser.error('--volume must name a separate source image')
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(source), str(volume)], check=True)
    check_disk(volume, work / 'before.fsck.log')
    for cycle in range(args.cycles):
        turn = work / str(cycle)
        turn.mkdir(exist_ok=True)
        socket = turn / 'monitor.sock'
        socket.unlink(missing_ok=True)
        guest = Guest(image, volume, turn / 'serial.log', 120, memory=1024,
                      gui=args.gui, ps2_only=args.gui, monitor=socket)
        monitor = None
        try:
            guest.wait_for(rb'Cinux up\r?\n')
            if args.gui:
                guest.wait_for(rb'vesper-fbdev: running')
            guest.wait_for_prompt()
            monitor = Monitor(socket)
            if cycle == 0:
                check_signal_wait(guest)
                guest.command('test ! -e /home/cinux/persistence-demo && cp -r /home/cinux/Cinux/user/examples/mini-primes /home/cinux/persistence-demo; echo COPY_$?', 'COPY')
            else:
                python_command(guest, f"from pathlib import Path; p=Path('/home/cinux/persistence-demo'); assert (p/'payload.bin').read_bytes()==bytes([{(cycle - 1) % 256}])*307200; assert (p/'after-sync').read_bytes()==b'round-{cycle - 1}'", 'COLD')
                output = guest.command('/home/cinux/persistence-demo/build/mini-primes 30; echo COLD_RUN_$?', 'COLD_RUN')
                if f'Round {cycle - 1} up to 30:'.encode() not in output:
                    raise RuntimeError('cold-boot executable output mismatch')
            old = 'Primes up to' if cycle == 0 else f'Round {cycle - 1} up to'
            new = f'Round {cycle} up to'
            python_command(guest, f"from pathlib import Path; p=Path('/home/cinux/persistence-demo/main.cpp'); s=p.read_text(); assert {old!r} in s; p.write_text(s.replace({old!r}, {new!r}))", 'EDIT')
            build_project(guest, monitor if args.gui else None, turn)
            program = f"from pathlib import Path; p=Path('/home/cinux/persistence-demo'); (p/'payload.bin').write_bytes(bytes([{cycle % 256}])*307200); (p/'pending').write_text('temporary'); (p/'pending').rename(p/'renamed'); (p/'renamed').unlink()"
            python_command(guest, program, 'DATA')
            hashes = python_command(guest, "import hashlib; from pathlib import Path; p=Path('/home/cinux/persistence-demo'); print('BINARY_SHA256',hashlib.sha256((p/'build/mini-primes').read_bytes()).hexdigest())", 'HASH')
            expected = re.search(rb'BINARY_SHA256 ([0-9a-f]{64})', hashes)[1].decode()
            guest.command('sync; echo SYNC_$?', 'SYNC')
            monitor.run('stop')
            snapshot = turn / 'synced.ext2'
            subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(volume), str(snapshot)], check=True)
            monitor.run('cont')
            check_disk(snapshot, turn / 'sync.fsck.log')
            verify_sync_snapshot(snapshot, turn, cycle, expected)
            python_command(guest, f"from pathlib import Path; Path('/home/cinux/persistence-demo/after-sync').write_bytes(b'round-{cycle}')", 'AFTERSYNC')
            if args.gui:
                type_line(monitor, 'poweroff')
            else:
                guest.process.stdin.write(b'poweroff\n')
                guest.process.stdin.flush()
            wait_for_poweroff(guest)
            if re.search(rb'\[kern\] exception|syscall \d+ unserved|ahci quarantined', guest.transcript):
                raise RuntimeError('exception or unsupported syscall during persistence test')
        finally:
            if monitor:
                monitor.close()
            guest.close()
        check_disk(volume, turn / 'poweroff.fsck.log')
        for name in ('after-sync', 'payload.bin', 'build/mini-primes'):
            destination = turn / pathlib.Path(name).name
            destination.unlink(missing_ok=True)
            subprocess.run(['debugfs', '-R', f'dump /home/cinux/persistence-demo/{name} {destination}', str(volume)], capture_output=True, check=True)
            if name == 'after-sync':
                assert destination.read_bytes() == f'round-{cycle}'.encode()
            elif name == 'payload.bin':
                assert destination.read_bytes() == bytes([cycle % 256]) * 307200
            else:
                assert hashlib.sha256(destination.read_bytes()).hexdigest() == expected
        print(f'[persistence] cycle {cycle + 1}: build, sync, later write, poweroff, fsck and extracted content PASS', flush=True)
    print(f'[persistence] PASS: {args.cycles} cycles; source volume untouched: {source}', flush=True)


if __name__ == '__main__':
    main()
