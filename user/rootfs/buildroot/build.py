#!/usr/bin/env python3
"""Fetch pinned Buildroot, apply Cinux config fragments, and bake its rootfs."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request


def run(*command):
    subprocess.run([str(word) for word in command], check=True)


def extract_root(tar, destination):
    members = tar.getmembers()
    tar.extractall(destination, members=[item for item in members
                                        if not item.isdev() and not item.issym()], filter='data')
    for item in members:
        if not item.issym():
            continue
        path = destination / item.name
        if pathlib.Path(item.name).is_absolute() or not path.parent.resolve().is_relative_to(destination.resolve()):
            raise RuntimeError(f'archive link escapes extraction directory: {item.name}')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.unlink(missing_ok=True)
        path.symlink_to(item.linkname)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=pathlib.Path, required=True)
    parser.add_argument('--defconfig', type=pathlib.Path, required=True)
    parser.add_argument('--fragment', type=pathlib.Path, action='append', default=[])
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--volume', type=pathlib.Path,
                        help='output path of the ext2 development volume')
    parser.add_argument('--ensure', action='store_true',
                        help='keep an existing volume untouched, build only when missing')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    here = pathlib.Path(__file__).resolve().parent
    repo = here.parents[2]
    build = args.build_dir.resolve()
    image = args.volume if args.volume else build / 'boot/native-build.ext2'
    if args.ensure and image.exists():
        print(f'[buildroot] volume already present, keep it: {image}')
        return
    work = build / 'buildroot'
    work.mkdir(parents=True, exist_ok=True)
    lock = json.loads((here / 'source.json').read_text())
    cache = repo / '.cache/rootfs'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f'buildroot-{lock["version"]}.tar.xz'
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        try:
            urllib.request.urlretrieve(lock['url'], partial)
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != lock['sha256']:
        raise RuntimeError(f'Buildroot checksum mismatch: {archive}')
    source = work / f'source-{lock["version"]}'
    if not source.exists():
        unpacked = work / 'unpack'
        shutil.rmtree(unpacked, ignore_errors=True)
        unpacked.mkdir()
        with tarfile.open(archive) as tar:
            extract_root(tar, unpacked)
        (unpacked / f'buildroot-{lock["version"]}').rename(source)
        unpacked.rmdir()
    output = work / 'output'
    overlay = work / 'overlay'
    config = work / 'cinux.config'
    values = {}
    for fragment in [args.defconfig, *args.fragment]:
        for line in fragment.read_text().splitlines():
            if line.startswith('BR2_') and '=' in line:
                values[line.split('=', 1)[0]] = line
            elif line.startswith('# BR2_') and line.endswith(' is not set'):
                values[line.split()[1]] = line
    content = '\n'.join(values.values()) + '\n'
    content = content.replace('@CINUX_BUSYBOX_CONFIG@', str(repo / 'user/rootfs/busybox/busybox.config'))
    content = content.replace('@CINUX_NATIVE_OVERLAY@', str(overlay))
    content = content.replace('@CINUX_USERS_TABLE@', str(here / 'configs/users.txt'))
    if not config.exists() or config.read_text() != content:
        config.write_text(content)
    make = ['make', '-C', source, f'O={output}', f'BR2_DL_DIR={cache / "buildroot-dl"}']
    applied = work / 'applied.config'
    if not (output / '.config').exists() or not applied.exists() or applied.read_text() != content:
        if (output / '.config').exists():
            raise RuntimeError('Buildroot configuration changed; use a fresh build directory or remove buildroot/output before reconfiguring')
        run(*make, f'BR2_DEFCONFIG={config}', 'defconfig')
        applied.write_text(content)
    if args.configure_only:
        print(f'[buildroot] configured: {output / ".config"}', flush=True)
        return
    if not overlay.is_dir():
        run(sys.executable, here / 'prepare.py', '--build-dir', build, '--overlay-only')
    run(*make, f'-j{args.jobs}', 'all')
    image.parent.mkdir(parents=True, exist_ok=True)
    temporary = image.with_suffix('.ext2.tmp')
    with temporary.open('wb') as disk:
        disk.truncate(1536 * 1024 * 1024)
    run('mkfs.ext2', '-q', '-F', '-b', '1024', '-O', 'none', '-N', '65536',
        '-d', output / 'images/rootfs.tar', temporary)
    with temporary.open('r+b') as disk:
        disk.seek(512)
        disk.write(bytes(range(255, -1, -1)) * 2)
        disk.flush()
        os.fsync(disk.fileno())
    temporary.replace(image)
    print(f'[buildroot] Cinux development image: {image}', flush=True)


if __name__ == '__main__':
    main()
