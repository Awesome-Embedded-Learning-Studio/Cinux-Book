#!/usr/bin/env python3
import glob
import json
import sys

import qemu

SESSION = None


def resolve_image(image):
    if image:
        return image
    matches = sorted(glob.glob("build/boot/cinux.img"))
    if not matches:
        raise RuntimeError("no image given and build/boot/cinux.img is missing")
    return matches[0]


def call_tool(name, args):
    global SESSION
    if name == "qemu_boot":
        SESSION = qemu.Session(resolve_image(args.get("image")))
        tail = SESSION.boot()
        return f"booted {SESSION.image}\nserial head:\n{tail}"
    if SESSION is None:
        raise RuntimeError("no session; call qemu_boot first")
    if name == "qemu_sendkey":
        return SESSION.sendkeys(args.get("keys", []))
    if name == "qemu_screendump":
        return SESSION.screendump(int(args.get("text_rows", 10)))
    if name == "qemu_serial":
        return SESSION.serial(int(args.get("lines", 20)))
    if name == "qemu_quit":
        result = SESSION.quit(int(args.get("lines", 20)))
        SESSION = None
        return result
    raise RuntimeError(f"unknown tool {name}")


TOOLS = [
    {
        "name": "qemu_boot",
        "description": "Boot a Cinux image under QEMU with KVM (no display, serial to file, monitor on a unix socket) and wait for the first [kern] line. Defaults to build/boot/cinux.img.",
        "inputSchema": {
            "type": "object",
            "properties": {"image": {"type": "string", "description": "Path to the raw disk image"}},
        },
    },
    {
        "name": "qemu_sendkey",
        "description": "Feed key presses through the QEMU monitor, e.g. ['h','i','spc','ret']. Keys travel the full PS/2 path.",
        "inputSchema": {
            "type": "object",
            "properties": {"keys": {"type": "array", "items": {"type": "string"}}},
            "required": ["keys"],
        },
    },
    {
        "name": "qemu_screendump",
        "description": "Capture the framebuffer to /tmp/cinux_qemu_screen.png and report lit-pixel counts per 16px text row.",
        "inputSchema": {
            "type": "object",
            "properties": {"text_rows": {"type": "integer", "description": "How many text rows to profile"}},
        },
    },
    {
        "name": "qemu_serial",
        "description": "Return the last lines of the serial log.",
        "inputSchema": {
            "type": "object",
            "properties": {"lines": {"type": "integer"}},
        },
    },
    {
        "name": "qemu_quit",
        "description": "Shut the machine down cleanly and return the last lines of the serial log.",
        "inputSchema": {
            "type": "object",
            "properties": {"lines": {"type": "integer"}},
        },
    },
]


def handle(message):
    method = message.get("method")
    if method == "initialize":
        return {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "cinux-qemu", "version": "0.1.0"},
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
            response = {"jsonrpc": "2.0", "id": message["id"],
                        "error": {"code": -32000, "message": str(failure)}}
        sys.stdout.write(json.dumps(response) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
