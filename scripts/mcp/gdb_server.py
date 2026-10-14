#!/usr/bin/env python3
"""The cinux-gdb MCP: a persistent gdb/MI session over a frozen QEMU.

Companion to the cinux-qemu MCP (running and observing) for the days a
machine has to be stopped and interrogated: breakpoints, registers,
memory, disassembly, backtraces. Hardware breakpoints are the default
because the kernel is loaded from disk long after the stub accepts
writes — a software breakpoint planted early gets overwritten and never
fires. debug_eval runs any console command when the shaped faces are
not enough.
"""

import json
import re
import signal
import sys

import gdb

SESSION = None


def require_session():
    if SESSION is None:
        raise RuntimeError("no session; call debug_boot first")
    return SESSION


def stopped_summary(stopped):
    if not stopped:
        return "running"
    addr = re.search(r'addr="([^"]+)"', stopped)
    reason = re.search(r'reason="([^"]+)"', stopped)
    parts = ["stopped"]
    if reason:
        parts.append(reason.group(1))
    if addr:
        parts.append("rip=" + addr.group(1))
    for field in ("func", "file", "line", "bkptno", "signal-name"):
        value = re.search(rf'{field}="([^"]+)"', stopped)
        if value:
            parts.append(field + "=" + value.group(1))
    return " ".join(parts)


def timeout_arg(args, default=gdb.STOP_TIMEOUT):
    timeout = args.get("timeout", default)
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not 0 <= timeout <= 30:
        raise ValueError("timeout must be a number between 0 and 30 seconds")
    return float(timeout)


def execution_summary(session, stopped, timeout):
    if session.state == "exited":
        return "target exited" + ("\n" + stopped_summary(stopped) if stopped else "")
    if stopped:
        return stopped_summary(stopped)
    return (f"running: no stop within {timeout:g}s; session retained. "
            "Use debug_wait to wait, debug_serial to inspect output, "
            "or debug_interrupt to pause.")


def session_status(session):
    try:
        session.refresh()
        state = session.state
    except gdb.SessionDisconnected as failure:
        state = str(failure)
    return (f"state: {state}\n"
            f"gdb port: {session.gdb_port}\n"
            f"gdb pid: {getattr(session.gdb, 'pid', None)}, "
            f"qemu pid: {getattr(session.qemu, 'pid', None)}\n"
            f"image: {session.image}\nelf: {session.elf}\n"
            f"serial log: {session.serial_log}\nqemu log: {session.qemu_log}\n"
            f"last stop: {stopped_summary(session.stopped) if session.stopped else '(none)'}")


def call_tool(name, args):
    global SESSION
    if name == "debug_boot":
        # Validate before replacing an existing machine.
        timeout = timeout_arg(args)
        image = gdb.resolve(args.get("image"), "build/boot/cinux.img", "image")
        elf = gdb.resolve(args.get("elf"), "build/kernel/cinux_kernel", "elf")
        if SESSION is not None:
            SESSION.close()
            SESSION = None
        SESSION = gdb.Session(image, elf)
        stop_at = args.get("stop_at", "KernelEntry")
        SESSION.breakpoint(stop_at, temporary=True)
        stopped = SESSION.continue_until_stop(timeout)
        return (f"attached to {elf}, image {image}, requested stop at {stop_at}\n"
                f"{execution_summary(SESSION, stopped, timeout)}\n"
                f"serial head:\n{SESSION.serial_head}")
    if name == "debug_status" and SESSION is None:
        return "no session; call debug_boot first"
    if name == "debug_quit" and SESSION is None:
        return "no session; already closed"
    session = require_session()
    if name == "debug_status":
        return session_status(session)
    if name == "debug_reconnect":
        session.reconnect()
        return "reconnected to existing machine\n" + session_status(session)
    if name == "debug_break":
        location = args["location"]
        hardware = args.get("hardware", True)
        answer = session.breakpoint(location, hardware)
        return (f"breakpoint set: {location} ({'hardware' if hardware else 'software'})\n"
                f"{answer}")
    actions = {
        "debug_continue": session.continue_until_stop,
        "debug_wait": session.wait_until_stop,
        "debug_interrupt": session.interrupt,
        "debug_step": session.step,
    }
    if name in actions:
        timeout = timeout_arg(args)
        return execution_summary(session, actions[name](timeout), timeout)
    if name == "debug_regs":
        wanted = args.get("regs") or gdb.GENERAL_REGS
        return session.console("info registers " + " ".join(wanted))
    if name == "debug_mem":
        address = args["address"]
        words = int(args.get("words", 8))
        return session.console(f"x/{words}gx {address}")
    if name == "debug_disasm":
        location = args.get("location", "$pc")
        count = int(args.get("count", 10))
        return session.console(f"x/{count}i {location}")
    if name == "debug_bt":
        return session.console("bt")
    if name == "debug_eval":
        command = args["command"]
        # Console foreground execution blocks MI even with mi-async enabled.
        # Route the common commands through the asynchronous driver instead.
        aliases = {"c": "debug_continue", "continue": "debug_continue",
                   "si": "debug_step", "stepi": "debug_step",
                   "interrupt": "debug_interrupt"}
        if command.strip() in aliases:
            return call_tool(aliases[command.strip()], {
                **args, "timeout": timeout_arg(args, gdb.COMMAND_TIMEOUT)})
        return session.console(command, timeout_arg(args, gdb.COMMAND_TIMEOUT))
    if name == "debug_serial":
        return session.serial(int(args.get("lines", 20)))
    if name == "debug_quit":
        tail = session.close()
        SESSION = None
        return f"session closed\nserial tail:\n{tail}"
    raise RuntimeError(f"unknown tool {name}")


TOOLS = [
    {
        "name": "debug_boot",
        "description": (
            "Start QEMU frozen at reset (KVM, local gdbstub), attach a persistent "
            "gdb with kernel symbols, hardware-break at stop_at and continue until it "
            "hits. Defaults: image build/boot/cinux.img, elf build/kernel/cinux_kernel, "
            "stop_at KernelEntry. If no stop occurs within timeout (default 10s), "
            "returns running and keeps the session. Replaces the previous machine."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "image": {"type": "string"},
                "elf": {"type": "string"},
                "stop_at": {"type": "string"},
                "timeout": {"type": "number", "minimum": 0, "maximum": 30, "default": 10},
            },
        },
    },
    {
        "name": "debug_break",
        "description": (
            "Set a breakpoint at a symbol, address or file:line. Hardware by default: "
            "the kernel is loaded from disk after boot, so early software breakpoints "
            "get overwritten and never fire."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "location": {"type": "string"},
                "hardware": {"type": "boolean"},
            },
            "required": ["location"],
        },
    },
    {
        "name": "debug_continue",
        "description": "Resume and wait for a stop. If already running, just wait. Timeout returns running without closing the session; timeout 0 resumes immediately.",
        "inputSchema": {"type": "object", "properties": {
            "timeout": {"type": "number", "minimum": 0, "maximum": 30, "default": 10}}},
    },
    {
        "name": "debug_wait",
        "description": "Wait for the running target to stop without resuming it. Timeout keeps the session and target running.",
        "inputSchema": {"type": "object", "properties": {
            "timeout": {"type": "number", "minimum": 0, "maximum": 30, "default": 10}}},
    },
    {
        "name": "debug_interrupt",
        "description": "Pause the running machine and wait for a fresh stop event; preserves breakpoints and serial output.",
        "inputSchema": {"type": "object", "properties": {
            "timeout": {"type": "number", "minimum": 0, "maximum": 30, "default": 10}}},
    },
    {
        "name": "debug_status",
        "description": "Inspect running/stopped state, process health and log paths without resuming the target. Works after a GDB or QEMU failure.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "debug_reconnect",
        "description": "Restart GDB and reattach to the existing QEMU without rebooting. Restores breakpoints created by debug_break/debug_boot; console-created breakpoints and other GDB settings must be reapplied.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "debug_step",
        "description": "Step one instruction and report where it landed.",
        "inputSchema": {"type": "object", "properties": {
            "timeout": {"type": "number", "minimum": 0, "maximum": 30, "default": 10}}},
    },
    {
        "name": "debug_regs",
        "description": "List register values; all general-purpose ones plus rip/eflags unless a subset is named.",
        "inputSchema": {
            "type": "object",
            "properties": {"regs": {"type": "array", "items": {"type": "string"}}},
        },
    },
    {
        "name": "debug_mem",
        "description": "Read memory as 8-byte words, e.g. address '$rsp' or '0xffffffff80200000', words 8.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "string"},
                "words": {"type": "integer"},
            },
            "required": ["address"],
        },
    },
    {
        "name": "debug_disasm",
        "description": "Disassemble count instructions at a location (default $pc).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "location": {"type": "string"},
                "count": {"type": "integer"},
            },
        },
    },
    {
        "name": "debug_bt",
        "description": "Backtrace the current stack.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "debug_eval",
        "description": "Escape hatch: run any gdb console command (x/16bx $rsp, info breakpoints, p/x $eflags).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {"type": "string"},
                "timeout": {"type": "number", "minimum": 0, "maximum": 30, "default": 5},
            },
            "required": ["command"],
        },
    },
    {
        "name": "debug_serial",
        "description": "Tail the serial log of the debugged machine.",
        "inputSchema": {
            "type": "object",
            "properties": {"lines": {"type": "integer"}},
        },
    },
    {
        "name": "debug_quit",
        "description": "Close the gdb session and the QEMU it drove; returns the serial tail.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def handle(message):
    method = message.get("method")
    if method == "initialize":
        return {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "cinux-gdb", "version": "0.2.0"},
        }
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        try:
            result = call_tool(message["params"]["name"], message["params"].get("arguments", {}))
            return {"content": [{"type": "text", "text": result}]}
        except Exception as failure:
            # Tool failures are recoverable MCP results, not transport errors.
            # In particular, never discard a VM because a breakpoint is invalid.
            text = str(failure)
            if SESSION is not None:
                text += "\nSession retained; debug_status and debug_serial remain available."
            return {"isError": True, "content": [{"type": "text", "text": text}]}
    raise RuntimeError(f"unsupported method {method}")


def main():
    # MCP clients usually stop stdio servers with SIGTERM, not an EOF.
    def terminate(signum, frame):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, terminate)
    try:
        serve()
    finally:
        if SESSION is not None:
            SESSION.close()


def serve():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except (ValueError, TypeError) as failure:
            response = {"jsonrpc": "2.0", "id": None,
                        "error": {"code": -32700, "message": str(failure)}}
            sys.stdout.write(json.dumps(response) + "\n")
            sys.stdout.flush()
            continue
        if not isinstance(message, dict):
            response = {"jsonrpc": "2.0", "id": None,
                        "error": {"code": -32600, "message": "request must be an object"}}
            sys.stdout.write(json.dumps(response) + "\n")
            sys.stdout.flush()
            continue
        if "id" not in message:
            continue
        try:
            result = handle(message)
            response = {"jsonrpc": "2.0", "id": message["id"], "result": result}
        except Exception as failure:
            response = {"jsonrpc": "2.0", "id": message["id"],
                        "error": {"code": -32000, "message": str(failure)}}
        sys.stdout.write(json.dumps(response) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
