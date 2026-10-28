#!/usr/bin/env python3
"""Validate native CMake, parallel/incremental rebuilds, and boot the guest product."""
import argparse
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import time
from test_station34_kernel_selfhost import Guest

MARKER = b'[native-build] kernel rebuilt by native CMake'


def ensure_clean(transcript):
    if re.search(rb'Problem (?:opening|parsing) /proc/(?:cpuinfo|meminfo)', transcript):
        raise RuntimeError('CMake could not read kernel CPU/memory information')
    if re.search(rb'\[kern\] exception|syscall \d+ unserved', transcript):
        raise RuntimeError('unserved syscall or exception; see native-build.serial.log')
    if b'vesper-fbdev: running' in transcript:
        raise RuntimeError('headless boot unexpectedly started the desktop')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', type=pathlib.Path, default=pathlib.Path('build/boot/cinux.img'))
    parser.add_argument('--volume', type=pathlib.Path, help='reuse a development volume with another kernel image')
    parser.add_argument('--source-manifest', type=pathlib.Path, help='expected source-proof.json staged at /home/cinux/source-proof.json')
    parser.add_argument('--probe-only', action='store_true')
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--reuse', action='store_true', help='resume a previous guest build')
    args = parser.parse_args()
    image = args.image.resolve()
    build = image.parent.parent
    volume = args.volume.resolve() if args.volume else build / 'boot/native-build.ext2'
    guest = Guest(image, volume, build / 'native-build.serial.log', 120, memory=1024)
    began = time.monotonic()
    digest = None
    try:
        guest.wait_for(rb'Cinux up\r?\n')
        guest.wait_for_prompt()
        guest.command('python3 -c "import os,pwd; p=pwd.getpwuid(os.geteuid()); '
                      'assert (p.pw_name,os.getuid(),os.getgid(),os.environ[\'HOME\']) '
                      '== (\'cinux\',1000,1000,\'/home/cinux\'); '
                      'print(\'DEVELOPER\',p.pw_name,os.getuid(),os.environ[\'HOME\'])"; '
                      'echo USER_PROBE_$?', 'USER_PROBE')
        guest.command('cat /hello.c; echo STATIC_EXIT_$?', 'STATIC_EXIT')
        guest.command('cd /home/cinux/Cinux; cmake --version; echo CMAKE_PROBE_$?', 'CMAKE_PROBE')
        guest.command('ninja --version; echo NINJA_PROBE_$?', 'NINJA_PROBE')
        if args.source_manifest:
            expected = hashlib.sha256(args.source_manifest.read_bytes()).hexdigest()
            verifier = (
                "import hashlib,json,pathlib; "
                "p=pathlib.Path('/home/cinux/source-proof.json'); "
                f"assert hashlib.sha256(p.read_bytes()).hexdigest()=={expected!r}; "
                "m=json.loads(p.read_bytes()); r=pathlib.Path('/home/cinux/Cinux'); "
                "assert all((r/n).is_symlink() and (r/n).readlink().as_posix()==v['link'] "
                "if 'link' in v else hashlib.sha256((r/n).read_bytes()).hexdigest()==v['sha256'] "
                "for n,v in m['source'].items()); "
                "assert hashlib.sha256(pathlib.Path('/bin/nativeprobe').read_bytes()).hexdigest()==m['probe_sha256']; "
                "print('SOURCE_AND_PROBE_VERIFIED',len(m['source']))"
            )
            guest.command('python3 -B -S -c ' + shlex.quote(verifier) + '; echo SOURCE_PROOF_$?',
                          'SOURCE_PROOF')
        guest.command('/bin/nativeprobe; echo NATIVE_PROBE_$?', 'NATIVE_PROBE')
        guest.command(r"printf '#include <stdint.h>\nuint32_t x;\n' | g++ -m32 -ffreestanding -x c++ -c - -o /tmp/boot32.o; echo BOOT32_$?", 'BOOT32')
        guest.command('tar --format=ustar -C assets/initrd -cf /tmp/native-initrd.tar readme.txt docs; echo TAR_$?', 'TAR')
        if args.probe_only:
            guest.command('sync; echo SYNC_$?', 'SYNC')
            ensure_clean(guest.transcript)
            return
        if not args.reuse:
            guest.command('test ! -e build; echo EMPTY_BUILD_$?', 'EMPTY_BUILD')
        guest.command('cmake -B build -S .; echo CONFIGURE_$?', 'CONFIGURE')
        print('[native-build] configure passed', flush=True)
        if args.configure_only:
            guest.command('sync; echo SYNC_$?', 'SYNC')
            ensure_clean(guest.transcript)
            return
        guest.timeout = 1800
        output = guest.command('cmake --build build -j; echo BUILD_$?', 'BUILD')
        print(output.decode(errors='replace'), flush=True)
        print('[native-build] parallel build passed', flush=True)
        output = guest.command('cmake --build build -j; echo INCREMENTAL_$?', 'INCREMENTAL')
        if b'Building CXX object' in output or b'Building C object' in output:
            raise RuntimeError('unchanged build unexpectedly recompiled sources')
        guest.command('python3 -B -S scripts/native-build-proof.py --stamp; echo STAMP_$?', 'STAMP')
        if args.reuse:
            guest.command('echo >> kernel/kernel.cpp; echo EDIT_$?', 'EDIT')
        output = guest.command('cmake --build build -j; echo REBUILD_$?', 'REBUILD')
        if b'kernel.cpp.o' not in output:
            raise RuntimeError('changed source did not rebuild the kernel')
        output = guest.command('python3 -B -S scripts/native-build-proof.py; echo HASH_$?', 'HASH')
        match = re.search(rb'IMAGE_SHA256 ([0-9a-f]{64})', output)
        if not match:
            raise RuntimeError('guest image hash missing')
        digest = match[1].decode()
        ensure_clean(guest.transcript)
    finally:
        guest.close()
    product = build / 'boot/cinux-native.img'
    product.unlink(missing_ok=True)
    subprocess.run(['debugfs', '-R', f'dump /home/cinux/Cinux/build/boot/cinux.img {product}', str(volume)],
                   capture_output=True, check=True)
    if not product.is_file() or hashlib.sha256(product.read_bytes()).hexdigest() != digest:
        raise RuntimeError('extracted product differs from guest image')
    guest = Guest(product, volume, build / 'native-build-reboot.serial.log', 120, memory=1024)
    try:
        guest.wait_for(re.escape(MARKER))
        guest.wait_for(rb'Cinux up\r?\n')
        guest.wait_for_prompt()
        guest.command('/bin/nativeprobe; echo REBORN_PROBE_$?', 'REBORN_PROBE')
        guest.command('gcc /hello.c -o /tmp/reborn; echo REBORN_CC_$?', 'REBORN_CC')
        output = guest.command('/tmp/reborn; echo REBORN_RUN_$?', 'REBORN_RUN')
        if b'Hello from GCC!' not in output:
            raise RuntimeError('new kernel did not run the compiled program')
        guest.command('sync; echo REBORN_SYNC_$?', 'REBORN_SYNC')
        ensure_clean(guest.transcript)
    finally:
        guest.close()
    (build / 'native-build.metrics.json').write_text(json.dumps({
        'elapsed_seconds': time.monotonic() - began, 'image_sha256': digest,
        'memory_mib': 1024, 'cpus': 2, 'generator': 'Ninja',
        'reused_build': args.reuse,
    }, indent=2) + '\n')
    print(f'[native-build] PASS: configure, parallel build, incremental rebuild, hash and reboot ({product})', flush=True)


if __name__ == '__main__':
    main()
