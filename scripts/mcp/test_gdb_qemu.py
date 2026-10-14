"""Opt-in live test: CINUX_GDB_QEMU_TEST=1 python3 -m unittest discover -s scripts/mcp.

Uses an isolated, disposable boot sector under TCG because CI may lack KVM.
The production driver still requires KVM; no project disk is modified.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import gdb
import gdb_server


class TcgSession(gdb.Session):
    def _spawn_qemu(self):
        with open(self.qemu_log, "wb") as errors:
            self.qemu = subprocess.Popen([
                "qemu-system-x86_64", "-accel", "tcg", "-display", "none",
                "-no-reboot", "-snapshot", "-boot", "a",
                "-drive", f"format=raw,file={self.image},if=floppy",
                "-serial", f"file:{self.serial_log}", "-S",
                "-gdb", f"tcp:127.0.0.1:{self.gdb_port}",
            ], stdout=subprocess.DEVNULL, stderr=errors)


@unittest.skipUnless(os.environ.get("CINUX_GDB_QEMU_TEST") == "1",
                     "set CINUX_GDB_QEMU_TEST=1 to run live QEMU/GDB integration")
class LiveQemuTests(unittest.TestCase):
    def test_timeout_interrupt_step_error_and_reconnect(self):
        elf = os.path.join(gdb.REPO_ROOT, "build/kernel/cinux_kernel")
        if not all(shutil.which(tool) for tool in ("gdb", "qemu-system-x86_64")) or not os.path.isfile(elf):
            self.skipTest("requires gdb, qemu-system-x86_64 and built kernel symbols")
        with tempfile.TemporaryDirectory(prefix="cinux-gdb-test-") as directory:
            image = os.path.join(directory, "loop.img")
            # mov dx,0x3f8; mov al,'A'; out dx,al; cli; inc ax; jmp inc
            code = bytes.fromhex("ba f8 03 b0 41 ee fa 40 eb fd")
            with open(image, "wb") as disk:
                disk.write(code + bytes(510 - len(code)) + b"\x55\xaa")
            logs = os.path.join(directory, "logs")
            os.mkdir(logs)
            with mock.patch.object(gdb.tempfile, "mkdtemp", return_value=logs):
                session = TcgSession(image, elf)
            gdb_server.SESSION = session
            try:
                session.breakpoint("*0x7c00", temporary=True)
                stopped = session.continue_until_stop(timeout=5)
                self.assertIn('reason="breakpoint-hit"', stopped)
                self.assertIn("7c00", stopped)
                stopped = session.step(timeout=2)
                self.assertIn("7c03", stopped)
                self.assertEqual(session.console('printf "quoted: %s\\n", "ok"'), "quoted: ok")
                with self.assertRaises(gdb.GdbError):
                    session.breakpoint("definitely_not_a_symbol")
                self.assertIsNone(session.continue_until_stop(timeout=0.1))
                self.assertEqual(session.state, "running")
                self.assertIn("A", session.serial(1))
                self.assertIsNone(session.wait_until_stop(timeout=0.01))
                # Reattach after a disconnect while the VM is running too.
                running_qemu_pid = session.qemu.pid
                session.gdb.kill()
                session.gdb.wait(timeout=2)
                session.reconnect()
                self.assertEqual(session.qemu.pid, running_qemu_pid)
                self.assertEqual(session.state, "stopped")
                self.assertEqual(session._breakpoints, {})
                self.assertIsNone(session.continue_until_stop(timeout=0.01))
                self.assertIsNotNone(session.interrupt(timeout=2))
                self.assertIn("rip", session.console("info registers rip"))
                session.breakpoint("*0x7c07")
                qemu_pid, old_gdb_pid = session.qemu.pid, session.gdb.pid
                session.gdb.kill()
                session.gdb.wait(timeout=2)
                session.reconnect()
                self.assertEqual(session.qemu.pid, qemu_pid)
                self.assertNotEqual(session.gdb.pid, old_gdb_pid)
                self.assertEqual(len(session._breakpoints), 1)
                self.assertIn('reason="breakpoint-hit"', session.continue_until_stop(timeout=2))
                self.assertIn("A", session.serial(1))
            finally:
                session.close()
                gdb_server.SESSION = None


if __name__ == "__main__":
    unittest.main()
