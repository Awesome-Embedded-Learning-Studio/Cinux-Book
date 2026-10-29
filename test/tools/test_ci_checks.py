"""Exercise CI failure gates with synthetic processes, without claiming KVM coverage."""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class KernelRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='cinux-ci-test-')
        self.addCleanup(self.temporary.cleanup)
        self.work = pathlib.Path(self.temporary.name)
        tools = self.work / 'tools'
        tools.mkdir()
        # The runner's KVM preflight is mocked only inside this harness.
        (tools / 'test').write_text('#!/bin/sh\nexit 0\n')
        (tools / 'test').chmod(0o755)
        self.qemu = tools / 'qemu'
        self.qemu.write_text(f'#!{sys.executable}\n' + '''import json, os, pathlib, sys
args = sys.argv[1:]
pathlib.Path(os.environ['FAKE_ARGS']).write_text(json.dumps(args))
serial = args[args.index('-serial') + 1].removeprefix('file:')
pathlib.Path(serial).write_text(os.environ['FAKE_SUMMARY'])
sys.exit(int(os.environ['FAKE_EXIT']))
''')
        self.qemu.chmod(0o755)
        self.environment = dict(os.environ, PATH=f'{tools}:{os.environ["PATH"]}',
                                FAKE_ARGS=str(self.work / 'args.json'), FAKE_EXIT='3',
                                FAKE_SUMMARY='=== Results: 2 passed, 0 failed ===\n')

    def run_kernel(self, cpus=1, devices='-smp;2;-display;none', program=None):
        return subprocess.run(
            ['cmake', f'-DQEMU_PROGRAM={program or self.qemu}', f'-DQEMU_DEVICE_ARGS={devices}',
             f'-DCPU_COUNT={cpus}', '-DIMAGE_FILE=fixture.img',
             f'-DTEST_DIR={self.work / "logs"}', '-P',
             str(ROOT / 'cmake/test/run_kernel_tests.cmake')],
            env=self.environment, capture_output=True, text=True)

    def test_qemu_command_can_include_a_launcher_and_arguments(self):
        result = self.run_kernel(program=f'{sys.executable};{self.qemu}')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('-snapshot', json.loads((self.work / 'args.json').read_text()))

    def test_each_cpu_leg_replaces_the_default_and_uses_a_snapshot(self):
        for cpus in (1, 2):
            with self.subTest(cpus=cpus):
                result = self.run_kernel(cpus)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                args = json.loads((self.work / 'args.json').read_text())
                self.assertEqual(args.count('-smp'), 1)
                self.assertEqual(args[args.index('-smp') + 1], str(cpus))
                self.assertIn('-snapshot', args)

    def test_success_exit_cannot_hide_an_empty_or_failing_suite(self):
        for summary in ('', '=== Results: 0 passed, 0 failed ===',
                        '=== Results: 2 passed, 128 failed ==='):
            with self.subTest(summary=summary):
                self.environment['FAKE_SUMMARY'] = summary
                result = self.run_kernel()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('without a nonempty passing test summary', result.stderr)

    def test_failure_exit_cannot_be_overridden_by_a_passing_summary(self):
        self.environment['FAKE_EXIT'] = '5'
        result = self.run_kernel()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('1 failing case(s)', result.stderr)

    def test_early_qemu_death_keeps_its_diagnostic(self):
        self.environment['FAKE_EXIT'] = '1'
        result = self.run_kernel()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('early death', result.stderr)

    def test_invalid_topology_is_rejected_before_qemu(self):
        for cpus, devices in ((3, '-smp;2'), (1, '-display;none')):
            with self.subTest(cpus=cpus, devices=devices):
                result = self.run_kernel(cpus, devices)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.work / 'args.json').exists())


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'),
                     'requires Linux ELF dynamic linking and a C compiler')
class QemuWakeupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='cinux-qemu-wakeup-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = pathlib.Path(cls.temporary.name)
        cls.library = cls.work / 'wakeup.so'
        cls.program = cls.work / 'fake-qemu'
        # Use real ELF interposition: the imported input function must run
        # before the executable's exported main-loop notification function.
        (cls.work / 'slirp.c').write_text('''#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
void slirp_input(void *s, const uint8_t *p, int n) {
    if (s != NULL || n != 1 || *p != 42) abort();
    puts("input");
}
''')
        (cls.work / 'qemu.c').write_text('''#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
void slirp_input(void *, const uint8_t *, int);
void qemu_notify_event(void) { puts("notify"); }
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--signal") == 0) raise(SIGTERM);
    const uint8_t packet = 42;
    slirp_input(NULL, &packet, 1);
    for (int i = 1; i < argc; ++i) puts(argv[i]);
    return 17;
}
''')
        (cls.work / 'preload.c').write_text('''#include <stdio.h>
__attribute__((constructor)) static void loaded(void) { puts("existing-preload"); }
''')
        commands = [
            ['cc', '-shared', '-fPIC', '-pthread', '-Wall', '-Wextra', '-Werror',
             str(ROOT / 'cmake/qemu/slirp_wakeup.c'), '-ldl', '-o', str(cls.library)],
            ['cc', '-shared', '-fPIC', str(cls.work / 'slirp.c'),
             '-o', str(cls.work / 'libfake-slirp.so')],
            ['cc', str(cls.work / 'qemu.c'), '-L', str(cls.work), '-lfake-slirp',
             '-Wl,--export-dynamic', f'-Wl,-rpath,{cls.work}', '-o', str(cls.program)],
            ['cc', '-shared', '-fPIC', str(cls.work / 'preload.c'),
             '-o', str(cls.work / 'preload.so')],
        ]
        for command in commands:
            subprocess.run(command, check=True, capture_output=True, text=True)

    def launch(self, *arguments, enabled=True, library=None, preload=None):
        environment = dict(os.environ, CINUX_QEMU_SLIRP_WAKEUP='1' if enabled else '0')
        environment.pop('LD_PRELOAD', None)
        if preload:
            environment['LD_PRELOAD'] = str(preload)
        return subprocess.run(
            [sys.executable, str(ROOT / 'scripts/mcp/qemu_launch.py'), str(self.program),
             str(library or self.library), *arguments], env=environment,
            capture_output=True, text=True)

    def test_input_is_followed_by_notification_and_arguments_survive(self):
        result = self.launch('path with spaces', 'literal;$(no-shell)')
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertEqual(result.stdout.splitlines(),
                         ['input', 'notify', 'path with spaces', 'literal;$(no-shell)'])

    def test_existing_preload_is_preserved(self):
        result = self.launch(preload=self.work / 'preload.so')
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ['existing-preload', 'input', 'notify'])

    def test_opt_out_works_without_the_wakeup_library(self):
        result = self.launch(enabled=False, library=self.work / 'absent.so')
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ['input'])

    def test_missing_library_has_an_actionable_error(self):
        result = self.launch(library=self.work / 'absent.so')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('build the qemu_disks target', result.stderr)

    def test_signal_reaches_the_qemu_process(self):
        result = self.launch('--signal')
        self.assertEqual(result.returncode, -15, result.stderr)


class InventoryTests(unittest.TestCase):
    def test_removing_a_suite_fails_even_when_other_suites_remain(self):
        baseline = json.loads((ROOT / 'test/ctest-baseline.json').read_text())
        with tempfile.TemporaryDirectory(prefix='cinux-inventory-test-') as temporary:
            build = pathlib.Path(temporary)
            (build / 'test').mkdir()
            registry = build / 'test/CTestTestfile.cmake'
            for names, expected_status in ((baseline, 0), (baseline[1:], 1)):
                registry.write_text(''.join(f'add_test({name} /bin/true)\n' for name in names))
                result = subprocess.run(
                    [sys.executable, str(ROOT / 'scripts/checks/test-inventory.py'),
                     '--build-dir', str(build)], capture_output=True, text=True)
                self.assertEqual(result.returncode, expected_status, result.stdout + result.stderr)
                if expected_status:
                    self.assertIn(baseline[0], result.stderr)

    def test_native_cell_refuses_an_existing_development_volume_before_configuring(self):
        with tempfile.TemporaryDirectory(prefix='cinux-native-guard-') as temporary:
            build = pathlib.Path(temporary)
            (build / 'boot').mkdir()
            volume = build / 'boot/native-build.ext2'
            volume.write_bytes(b'existing developer data')
            result = subprocess.run(
                ['bash', str(ROOT / 'scripts/checks/run-cell.sh'), 'native', 'Release',
                 'checks', str(build)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('refusing to overwrite a development volume', result.stderr)
            self.assertFalse((build / 'CMakeCache.txt').exists())
            self.assertEqual(volume.read_bytes(), b'existing developer data')


class LibstdcxxAssertionTests(unittest.TestCase):
    def test_real_atomic_contract_violation_reaches_the_kernel_panic_bridge(self):
        with tempfile.TemporaryDirectory(prefix='cinux-stdlib-test-') as temporary:
            work = pathlib.Path(temporary)
            source = work / 'probe.cpp'
            source.write_text('''#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "kernel/console/console.hpp"
#include "kernel/debug/panic.hpp"
namespace cinux::console {
void PutString(const char* text, bool newline) {
    std::fputs(text, stdout);
    if (newline) std::fputc('\\n', stdout);
}
}
namespace cinux::debug {
[[noreturn]] void PanicDump(unsigned long long) { std::exit(73); }
}
int main() {
    std::atomic<unsigned int> word{0};
    auto order = std::memory_order_release;
    __asm__ volatile("" : "+r"(order));
    return static_cast<int>(word.load(order));
}
''')
            binary = work / 'probe'
            compiler = os.environ.get('CXX', shutil.which('g++'))
            result = subprocess.run(
                [compiler, '-std=c++23', '-O0', '-D_GLIBCXX_ASSERTIONS',
                 '-I', str(ROOT), '-I', str(ROOT / 'base/include'), str(source),
                 str(ROOT / 'kernel/lib/assert.cpp'), str(ROOT / 'base/src/format/entry.cpp'),
                 str(ROOT / 'base/src/format/radix.cpp'), '-o', str(binary)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 73, result.stdout + result.stderr)
            self.assertIn('[panic] libstdc++ assertion:', result.stdout)
            self.assertIn('memory_order_release', result.stdout)


class MatrixTests(unittest.TestCase):
    def run_matrix(self, fail_worker='', fail_smoke=''):
        temporary = tempfile.TemporaryDirectory(prefix='cinux-matrix-test-')
        self.addCleanup(temporary.cleanup)
        root = pathlib.Path(temporary.name)
        checks = root / 'scripts/checks'
        checks.mkdir(parents=True)
        shutil.copy2(ROOT / 'scripts/checks/matrix.sh', checks / 'matrix.sh')
        (checks / 'run-cell.sh').write_text('''#!/bin/sh
printf '%s\\n' "$1:$2:$3" >> "$CINUX_MATRIX_DIR/worker-calls"
if [ "$1:$2:$3" = "$FAIL_WORKER" ]; then exit 7; fi
''')
        (root / 'scripts/test_station33_ecosystem.py').write_text('''import os, sys
with open(os.environ['CINUX_MATRIX_DIR'] + '/smoke-calls', 'a') as log:
    log.write(sys.argv[2] + '\\n')
if os.environ['FAIL_SMOKE'] and os.environ['FAIL_SMOKE'] in sys.argv[2]:
    sys.exit(9)
''')
        output = root / 'output'
        result = subprocess.run(
            ['bash', str(checks / 'matrix.sh')], capture_output=True, text=True,
            env=dict(os.environ, CINUX_MATRIX_DIR=str(output), FAIL_WORKER=fail_worker,
                     FAIL_SMOKE=fail_smoke))
        statuses = {p.stem: p.read_text().strip() for p in (output / 'logs').glob('*.status')}
        self.assertEqual(len((output / 'worker-calls').read_text().splitlines()), 11)
        self.assertEqual(len(statuses), 11)
        return result, output, statuses

    def test_complete_matrix_runs_all_seven_production_boots(self):
        result, output, statuses = self.run_matrix()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(set(statuses.values()), {'OK'})
        self.assertEqual(len((output / 'smoke-calls').read_text().splitlines()), 7)

    def test_worker_failure_is_reported_after_other_cells_run(self):
        result, _, statuses = self.run_matrix(fail_worker='host:Release:asan')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(statuses['host-Release-asan'], 'FAILED')
        self.assertEqual(statuses['ecosystem-Release-checks'], 'OK')

    def test_failed_production_boot_fails_a_passing_kernel_cell(self):
        result, _, statuses = self.run_matrix(fail_smoke='Debug-ubsan')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(statuses['kernel-Debug-ubsan'], 'FAILED')
        self.assertEqual(statuses['ecosystem-Release-checks'], 'OK')


if __name__ == '__main__':
    unittest.main()
