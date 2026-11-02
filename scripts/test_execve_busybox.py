#!/usr/bin/env python3
"""Run fork/exec and SIGCHLD regressions on disposable copies of the disks.

Requires a built kernel, teaching volume, static busybox ELF, QEMU and GDB.
KVM is the default; TCG must be selected explicitly. Logs remain under /tmp.
The init argv is injected at JumpToRing3, so no source edits are necessary.
"""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "mcp"))
from qemu_args import qemu_command


def debugger_script(elf, sock, rounds):
    """Build startup arguments and stop at the first failure or parent exit."""
    command = (
        "PATH=/bin; export PATH; for i in " + " ".join(str(i) for i in range(rounds))
        + "; do /bin/busybox uname || exit 91; done; "
        "uname; echo path-uname-status-$?; /bin/uname; echo absolute-uname-status-$?; "
        "hello; echo path-hello-status-$?; missing-exec; echo path-missing-status-$?; "
        "/bin/hello; echo hello-status-$?; /bin/busybox false; "
        "echo exec-status-$?; /bin/missing-exec; echo missing-status-$?; echo exec-ok"
    )
    return f"""set pagination off
set confirm off
file {json.dumps(str(elf))}
target remote {sock}
hbreak KernelEntry
continue
delete breakpoints
break *JumpToRing3
commands
  silent
  set $usp = $rsi
  set $page = $usp & ~4095
  set {{unsigned long}}$usp = 3
  set {{unsigned long}}($usp+16) = $page+64
  set {{unsigned long}}($usp+24) = $page+80
  set {{unsigned long}}($usp+32) = 0
  set {{unsigned long}}($usp+40) = 0
  set {{unsigned long}}($usp+48) = 0
  set {{unsigned long}}($usp+56) = 0
  set {{char[3]}}($page+64) = "-c"
  set {{char[{len(command) + 1}]}}($page+80) = {json.dumps(command)}
  continue
end
set $sc = ('cinux::proc::Scheduler' *)&'cinux::base::Singleton<cinux::proc::Scheduler>::self()::instance'
break *'cinux::proc::Completion::finish(bool)' if ($esi & 255) == 0
commands
  silent
  echo EXEC_SMOKE_IO_FAILURE\\n
  bt
  quit 1
end
break *'cinux::proc::Scheduler::exit_current(long long)' if $rsi == 11 || $sc->current_->pid == 2
commands
  silent
  printf "EXEC_SMOKE pid=%llu code=%lld\\n", $sc->current_->pid, $rsi
  if $sc->current_->pid == 2 && $rsi == 0
    quit 0
  end
  bt
  quit 1
end
continue
"""


def main():
    """Prepare the isolated volume, execute the guest and check its results."""
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--busybox", type=pathlib.Path, required=True)
    parser.add_argument("--build-dir", type=pathlib.Path, default=root / "build")
    parser.add_argument("--volume", type=pathlib.Path)
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--rounds", type=int, choices=range(1, 51), default=10,
                        metavar="1..50")
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if args.accel == "kvm" and not pathlib.Path("/dev/kvm").exists():
        parser.error("/dev/kvm is unavailable; run with KVM access or explicitly select --accel tcg")
    build = args.build_dir.resolve()
    volume = (args.volume or build / "boot/ext2.img").resolve()
    elf = build / "kernel/cinux_kernel"
    boot = build / "boot/cinux.img"
    for path in (args.busybox, volume, elf, boot):
        if not path.is_file():
            parser.error(f"missing input: {path}")
    for program in ("qemu-system-x86_64", "gdb", "debugfs"):
        if shutil.which(program) is None:
            parser.error(f"missing executable: {program}")

    work = pathlib.Path(tempfile.mkdtemp(prefix="cinux-execve-"))
    print(f"Logs: {work}", flush=True)
    shutil.copyfile(volume, work / "volume.img")
    shutil.copyfile(boot, work / "boot.img")
    # Give debugfs a simple source path even when the supplied path has spaces.
    shutil.copy2(args.busybox, work / "busybox")
    with (work / "debugfs.log").open("w") as log:
        for command in ("rm /bin/sh", "rm /bin/busybox", "write busybox /bin/sh",
                        "rm /bin/uname", "ln /bin/sh /bin/busybox", "ln /bin/sh /bin/uname"):
            subprocess.run(["debugfs", "-w", "-R", command, "volume.img"], cwd=work,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
    sock = work / "gdb.sock"
    script = work / "check.gdb"
    script.write_text(debugger_script(elf, sock, args.rounds))
    vm = None
    status = 1
    try:
        with (work / "qemu.log").open("w") as errors:
            vm = subprocess.Popen([
                *qemu_command(boot), "-accel", args.accel, "-cpu", "max", "-m", "128M",
                "-smp", "1", "-display", "none", "-monitor", "none", "-no-reboot",
                "-snapshot", "-device", "ahci,id=sata0",
                "-drive", f"format=raw,file={work / 'boot.img'}",
                "-drive", f"format=raw,file={work / 'volume.img'},if=none,id=spare",
                "-device", "ide-hd,drive=spare,bus=sata0.0",
                "-serial", f"file:{work / 'serial.log'}", "-S",
                "-gdb", f"unix:{sock},server=on,wait=off",
            ], stdout=subprocess.DEVNULL, stderr=errors)
            deadline = time.monotonic() + 5
            while not sock.exists():
                if vm.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError((work / "qemu.log").read_text())
                time.sleep(.05)
            with (work / "gdb.log").open("w") as log:
                result = subprocess.run(["gdb", "-q", "-nx", "-batch", "-x", str(script)],
                                        stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
            serial = (work / "serial.log").read_text(errors="replace")
            debug = (work / "gdb.log").read_text(errors="replace")
            count = serial.splitlines().count("Cinux")
            passed = (result.returncode == 0 and "EXEC_SMOKE pid=2 code=0" in debug
                      and count == args.rounds + 2 and "hello from the volume" in serial
                      and "path-uname-status-0" in serial
                      and "absolute-uname-status-0" in serial
                      and "path-hello-status-42" in serial and "path-missing-status-127" in serial
                      and "hello-status-42" in serial and "exec-status-1" in serial
                      and "missing-status-127" in serial and "exec-ok" in serial
                      and "I/O error" not in serial)
            if passed:
                print(f"PASS ({args.accel}): {count} uname execs, PATH lookup, hello, exit status, SIGCHLD return")
                status = 0
            else:
                print("FAIL: " + debug[-4000:] + "\n" + serial[-3000:])
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}")
    finally:
        if vm is not None:
            vm.terminate()
            try:
                vm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                vm.kill()
                vm.wait()
    return status


if __name__ == "__main__":
    raise SystemExit(main())
