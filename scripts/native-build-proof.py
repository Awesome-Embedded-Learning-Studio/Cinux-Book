#!/usr/bin/env python3
"""Stamp the guest source for an observable rebuild, or sync and hash its image."""
import argparse
import hashlib
import os
import pathlib

MARKER = '[native-build] kernel rebuilt by native CMake'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--stamp', action='store_true')
    args = parser.parse_args()
    if args.stamp:
        source = pathlib.Path('kernel/kernel.cpp')
        text = source.read_text()
        needle = 'Println("[kern] 64-bit C++ world alive");'
        if MARKER not in text:
            if needle not in text:
                raise RuntimeError('kernel boot marker not found')
            source.write_text(text.replace(needle, needle + f'\n    Println("{MARKER}");', 1))
        return
    image = pathlib.Path('build/boot/cinux.img')
    with image.open('rb') as target:
        digest = hashlib.sha256(target.read()).hexdigest()
        os.fsync(target.fileno())
    print(f'[native-build] IMAGE_SHA256 {digest}', flush=True)


if __name__ == '__main__':
    main()
