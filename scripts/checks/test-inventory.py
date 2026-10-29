#!/usr/bin/env python3
"""Fail if a configured build silently loses a baseline CTest suite."""
import argparse
import json
import pathlib
import subprocess
import sys


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=pathlib.Path, required=True)
    args = parser.parse_args()
    output = subprocess.check_output(
        ['ctest', '--test-dir', str(args.build_dir / 'test'), '--show-only=json-v1'], text=True)
    inventory = json.loads(output)
    expected = json.loads((root / 'test/ctest-baseline.json').read_text())
    actual = {test['name'] for test in inventory['tests']}
    missing = sorted(set(expected) - actual)
    if missing:
        print('[inventory] missing CTest suites: ' + ', '.join(missing), file=sys.stderr)
        return 1
    kernel = sum(name.startswith('kernel_test_') for name in actual)
    print(f'[inventory] {len(actual) - kernel} host, {kernel} kernel CPU legs; baseline intact')
    (args.build_dir / 'test-inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
