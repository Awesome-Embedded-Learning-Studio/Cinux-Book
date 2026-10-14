"""Protocol regression tests; run with python3 -m unittest discover -s scripts/mcp."""

import io
import json
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import gdb
import gdb_server


# A real pipe peer intentionally separates acknowledgements from async events,
# fragments records, omits prompt newlines and supports recoverable errors.
FAKE_MI = r'''
import json, re, signal, sys, threading
lock = threading.Lock()
number = 0
pending = None
cancelled = threading.Event()
def emit(text):
    with lock:
        sys.stdout.write(text)
        sys.stdout.flush()
def later(text):
    timer = threading.Timer(0.03, emit, args=(text,))
    timer.daemon = True
    timer.start()
def cancel(signum, frame):
    if pending:
        emit(pending + '^error,msg="cancelled"\n(gdb) ')
        cancelled.set()
signal.signal(signal.SIGINT, cancel)
emit('(gdb) ')
for line in sys.stdin:
    match = re.match(r'(\d+)(.*)', line.rstrip('\n'))
    token, command = match.groups()
    if command.startswith('-target-select'):
        emit(token + '^connected\n*stopped,frame={addr="0x1000"}\n(gdb) ')
    elif command == '-exec-continue':
        emit(token + '^running\n(gdb) ')
        later('*running,thread-id="all"\n')
    elif command == '-exec-step-instruction':
        emit(token + '^running\n(gdb) ')
        later('*running,thread-id="all"\n*stopped,reason="end-stepping-range",frame={addr="0x1001"}\n')
    elif command.startswith('-exec-interrupt'):
        emit(token + '^done\n(gdb) ')
        later('*stopped,reason="signal-received",signal-name="SIGINT",frame={addr="0x1002"}\n')
    elif command.startswith('-break-insert'):
        if 'BAD' in command:
            emit(token + '^error,msg="unknown symbol"\n(gdb) ')
        else:
            number += 1
            emit(token + '^done,bkpt={number="' + str(number) + '",addr="0x2000"}\n(gdb) ')
    elif command.startswith('-interpreter-exec console '):
        text = json.loads(command[len('-interpreter-exec console '):])
        if text == 'hang':
            pending = token
            cancelled.clear()
            cancelled.wait()
            pending = None
        elif text == 'fail':
            emit(token + '^error,msg="invalid command"\n(gdb) ')
        elif text == 'late-stop':
            later('*stopped,reason="breakpoint-hit",bkptno="1",frame={addr="0x2000"}\n')
            emit(token + '^done\n(gdb) ')
        elif text == 'split-record':
            emit('~"split')
            later(' record\\n"\n' + token + '^done\n(gdb) ')
        elif text == 'temp-hit':
            emit('*stopped,reason="breakpoint-hit",bkptno="1",frame={addr="0x2000"}\n'
                 '=breakpoint-deleted,id="1"\n' + token + '^done\n(gdb) ')
        else:
            emit('~' + json.dumps(text + '\n') + '\n' + token + '^done\n(gdb) ')
    else:
        emit(token + '^done\n(gdb) ')
'''

POPEN = subprocess.Popen


class FakeSession(gdb.Session):
    def _spawn_qemu(self):
        with open(self.serial_log, "w") as serial:
            serial.write("[kern] serial remains readable\n")
        with open(self.qemu_log, "w"):
            pass
        self.qemu = POPEN([sys.executable, "-c", "import time; time.sleep(300)"])

    def _spawn_gdb(self):
        def spawn(command, **kwargs):
            return POPEN([sys.executable, "-u", "-c", FAKE_MI], **kwargs)
        with mock.patch.object(gdb.subprocess, "Popen", side_effect=spawn):
            super()._spawn_gdb()


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.logs = tempfile.TemporaryDirectory()
        with mock.patch.object(gdb.tempfile, "mkdtemp", return_value=self.logs.name), \
                mock.patch.object(gdb.socket, "socket") as socket:
            socket.return_value.__enter__.return_value.getsockname.return_value = ("127.0.0.1", 1234)
            self.session = FakeSession("image", "symbols with spaces")
        gdb_server.SESSION = self.session

    def tearDown(self):
        self.session.close()
        gdb_server.SESSION = None
        self.logs.cleanup()

    def call(self, name, **args):
        return gdb_server.handle({"method": "tools/call", "params": {
            "name": name, "arguments": args}})

    def test_continue_timeout_preserves_session_and_serial(self):
        result = self.call("debug_continue", timeout=0.05)
        self.assertNotIn("isError", result)
        self.assertIn("running", result["content"][0]["text"])
        self.assertIs(gdb_server.SESSION, self.session)
        self.assertIn("serial remains readable", self.call("debug_serial")["content"][0]["text"])
        self.assertIn("state: running", self.call("debug_status")["content"][0]["text"])
        old_token = self.session.token
        self.call("debug_continue", timeout=0)
        self.call("debug_wait", timeout=0)
        self.assertEqual(self.session.token, old_token)

    def test_zero_timeout_resumes_without_returning_old_stop(self):
        result = self.call("debug_continue", timeout=0)
        self.assertIn("running", result["content"][0]["text"])
        self.assertIsNone(self.session.stopped)

    def test_step_waits_for_new_stop(self):
        stopped = self.session.step(timeout=1)
        self.assertIn('addr="0x1001"', stopped)
        self.assertEqual(self.session.state, "stopped")

    def test_interrupt_then_inspect(self):
        self.session.continue_until_stop(timeout=0.05)
        stopped = self.session.interrupt(timeout=1)
        self.assertIn('signal-name="SIGINT"', stopped)
        self.assertIn("info registers rip", self.session.console("info registers rip"))

    def test_delayed_stop_is_not_lost_between_calls(self):
        self.session.continue_until_stop(timeout=0.05)
        self.session.console("late-stop")
        self.assertIn('addr="0x2000"', self.session.wait_until_stop(timeout=1))

    def test_invalid_commands_and_breakpoints_do_not_destroy_session(self):
        for name, args in (("debug_break", {"location": "BAD"}),
                           ("debug_eval", {"command": "fail"}),
                           ("unknown", {}),
                           ("debug_continue", {"timeout": -1})):
            with self.subTest(name=name):
                result = self.call(name, **args)
                self.assertTrue(result["isError"])
                self.assertIs(gdb_server.SESSION, self.session)
                self.assertIsNone(self.session.qemu.poll())
                self.assertIn("serial remains readable", self.session.serial(1))
        self.assertIn("healthy", self.session.console("healthy"))

    def test_console_quotes_and_fragmented_records(self):
        command = 'printf "quoted %s", "value\\n"'
        self.assertEqual(self.session.console(command), command)
        self.assertEqual(self.session.console("split-record"), "split record")

    def test_foreground_timeout_cancels_and_keeps_protocol_usable(self):
        result = self.call("debug_eval", command="hang", timeout=0.05)
        self.assertTrue(result["isError"])
        self.assertIn("timed out", result["content"][0]["text"])
        self.assertEqual(self.session.console("healthy"), "healthy")

    def test_reconnect_restores_breakpoints_and_keeps_qemu(self):
        self.session.breakpoint("cinux::loader::LoadElfIntoSpace")
        old_qemu, old_gdb = self.session.qemu.pid, self.session.gdb.pid
        self.session.gdb.kill()
        self.session.gdb.wait(timeout=2)
        self.assertIn("debug_reconnect", self.call("debug_status")["content"][0]["text"])
        self.assertIn("serial remains readable", self.session.serial(1))
        self.call("debug_reconnect")
        self.assertEqual(self.session.qemu.pid, old_qemu)
        self.assertNotEqual(self.session.gdb.pid, old_gdb)
        self.assertEqual(list(self.session._breakpoints.values()),
                         [("cinux::loader::LoadElfIntoSpace", True, False)])

    def test_consumed_temporary_breakpoint_is_not_restored(self):
        self.session.breakpoint("KernelEntry", temporary=True)
        self.session.console("temp-hit")
        self.session.reconnect()
        self.assertEqual(self.session._breakpoints, {})

    def test_serial_zero_lines_and_idempotent_quit(self):
        self.assertEqual(self.session.serial(0), "")
        self.assertNotIn("isError", self.call("debug_quit"))
        self.assertNotIn("isError", self.call("debug_quit"))

    def test_eval_continue_alias_uses_async_driver(self):
        self.assertIn("running", self.call("debug_eval", command="continue", timeout=0.05)["content"][0]["text"])

    def test_malformed_json_does_not_close_active_session(self):
        request = {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                   "params": {"name": "debug_serial"}}
        output = io.StringIO()
        with mock.patch.object(sys, "stdin", io.StringIO("bad json\n" + json.dumps(request) + "\n")), \
                mock.patch.object(sys, "stdout", output):
            gdb_server.serve()
        responses = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(responses[0]["error"]["code"], -32700)
        self.assertIn("serial remains readable", responses[1]["result"]["content"][0]["text"])
        self.assertIs(gdb_server.SESSION, self.session)

    def test_server_keeps_session_across_tool_error(self):
        requests = [{"jsonrpc": "2.0", "id": index, "method": "tools/call",
                     "params": {"name": name, "arguments": args}}
                    for index, (name, args) in enumerate([
                        ("debug_break", {"location": "BAD"}),
                        ("debug_continue", {"timeout": 0.05}),
                        ("debug_serial", {}),
                    ])]
        output = io.StringIO()
        with mock.patch.object(sys, "stdin", io.StringIO("\n".join(map(json.dumps, requests)))), \
                mock.patch.object(sys, "stdout", output):
            gdb_server.serve()
        responses = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertTrue(responses[0]["result"]["isError"])
        self.assertIn("running", responses[1]["result"]["content"][0]["text"])
        self.assertIn("serial remains readable", responses[2]["result"]["content"][0]["text"])
        self.assertIs(gdb_server.SESSION, self.session)

    def test_qemu_failure_retains_diagnostics(self):
        with open(self.session.qemu_log, "w") as log:
            log.write("test qemu failure")
        self.session.qemu.kill()
        self.session.qemu.wait(timeout=2)
        result = self.call("debug_regs")
        self.assertTrue(result["isError"])
        self.assertIn("test qemu failure", result["content"][0]["text"])
        self.assertIn("QEMU exited", self.call("debug_status")["content"][0]["text"])
        self.assertIn("serial remains readable", self.call("debug_serial")["content"][0]["text"])

    def test_constructor_failure_closes_partial_children(self):
        spawned = []
        class FailingSession(FakeSession):
            def _attach(inner):
                inner._spawn_gdb()
                spawned.extend((inner.qemu, inner.gdb))
                raise RuntimeError("attach failed")
        with tempfile.TemporaryDirectory() as logs, \
                mock.patch.object(gdb.tempfile, "mkdtemp", return_value=logs), \
                mock.patch.object(gdb.socket, "socket"), \
                self.assertRaisesRegex(RuntimeError, "attach failed"):
            FailingSession("image", "elf")
        self.assertEqual(len(spawned), 2)
        self.assertTrue(all(proc.poll() is not None for proc in spawned))

    def test_non_object_request_does_not_end_server(self):
        output = io.StringIO()
        with mock.patch.object(sys, "stdin", io.StringIO('null\n{"id":1,"method":"ping"}\n')), \
                mock.patch.object(sys, "stdout", output):
            gdb_server.serve()
        responses = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(responses[0]["error"]["code"], -32600)
        self.assertEqual(responses[1]["result"], {})


if __name__ == "__main__":
    unittest.main()
