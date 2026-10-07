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
    return " ".join(parts)


def call_tool(name, args):
    global SESSION
    if name == "debug_boot":
        if SESSION is not None:
            SESSION.close()
        image = gdb.resolve(args.get("image"), "build/boot/cinux.img", "image")
        elf = gdb.resolve(args.get("elf"), "build/kernel/cinux_kernel", "elf")
        SESSION = gdb.Session(image, elf)
        stop_at = args.get("stop_at", "KernelEntry")
        SESSION.mi(f"-break-insert -t -h {stop_at}")
        stopped = SESSION.continue_until_stop()
        return (f"attached to {elf}, image {image}, stopped at {stop_at}\n"
                f"{stopped_summary(stopped)}\n"
                f"serial head:\n{SESSION.serial_head}")
    session = require_session()
    if name == "debug_break":
        location = args["location"]
        hardware = args.get("hardware", True)
        flag = "-h " if hardware else ""
        session.mi(f"-break-insert {flag}{location}")
        return f"breakpoint set: {location} ({'hardware' if hardware else 'software'})"
    if name == "debug_continue":
        return stopped_summary(session.continue_until_stop())
    if name == "debug_step":
        session.mi("-exec-step-instruction")
        return stopped_summary(session.stopped)
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
        return session.console(args["command"])
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
            "Start QEMU frozen at reset (KVM, gdbstub on :1234), attach a persistent "
            "gdb with kernel symbols, hardware-break at stop_at and continue until it "
            "hits. Defaults: image build/boot/cinux.img, elf build/kernel/cinux_kernel, "
            "stop_at KernelEntry."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "image": {"type": "string"},
                "elf": {"type": "string"},
                "stop_at": {"type": "string"},
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
        "description": "Continue until the machine stops again; reports the stop reason and rip.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "debug_step",
        "description": "Step one instruction and report where it landed.",
        "inputSchema": {"type": "object", "properties": {}},
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
            "properties": {"command": {"type": "string"}},
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
            "serverInfo": {"name": "cinux-gdb", "version": "0.1.0"},
        }
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        result = call_tool(message["params"]["name"], message["params"].get("arguments", {}))
        return {"content": [{"type": "text", "text": result}]}
    raise RuntimeError(f"unsupported method {method}")


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        message = json.loads(line)
        if "id" not in message:
            continue
        try:
            result = handle(message)
            response = {"jsonrpc": "2.0", "id": message["id"], "result": result}
        except Exception as failure:
            global SESSION
            if SESSION is not None:
                try:
                    SESSION.close()
                except Exception:
                    pass
                SESSION = None
            response = {"jsonrpc": "2.0", "id": message["id"],
                        "error": {"code": -32000, "message": str(failure)}}
        sys.stdout.write(json.dumps(response) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
