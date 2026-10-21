import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "mcp"))

import qemu
from test_execve_dyn import run_command, wait_for_serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", default="build/boot/cinux.img")
    args = parser.parse_args()
    session = qemu.Session(args.image)
    try:
        session.boot()
        for marker in ("[smp] 2 CPUs online", "[cpu 0]", "[cpu 1]", "/ #"):
            if not wait_for_serial(session, marker, 20):
                print(session.serial(80))
                return 1
        checks = [
            ("/bin/smpstress", ["SMP_PIPE_EOF_PASS", "SMP_FORK_PASS cpus=2 rounds=96"]),
            ("/bin/busybox sh /etc/smp-stress.sh", ["BUSYBOX_SMP_PASS"]),
        ]
        for command, markers in checks:
            if not run_command(session, command, markers, 45):
                return 1
        log = pathlib.Path(qemu.SERIAL_PATH).read_text(errors="replace")
        if any(marker in log for marker in ("PANIC", "[panic]", "ASSERT", "SMP_FORK_FAIL")):
            print(log[-8000:])
            return 1
        print("[smp] two CPU rows, cross-core fork/CoW and busybox stress PASS")
        return 0
    finally:
        if session.proc is not None and session.proc.poll() is None:
            session.quit(2)


if __name__ == "__main__":
    sys.exit(main())
