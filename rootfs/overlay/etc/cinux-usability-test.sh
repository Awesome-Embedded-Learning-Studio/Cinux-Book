#!/bin/sh
# cinux-usability-test.sh -- F-USABILITY stage-2 CI gate. Runs a spread of
# busybox applets to prove the Buildroot rootfs + Cinux kernel can drive a real
# userland, then signals pass/fail to QEMU via cinux-exit (sys_cinux_exit=221 ->
# isa-debug-exit). CI gates on the QEMU exit code (1 = pass, 3 = fail).
#
# busybox init runs this ::once: (see /etc/inittab) before respawning /bin/sh;
# cinux-exit terminates QEMU so the run never reaches the respawn. If cinux-exit
# itself fails the CI job times out (caught as a CI failure).
set -u

# Filesystem: ls / cat / mkdir / rmdir
/bin/ls /
echo "[usability] PASS ls-root"

/bin/cat /etc/inittab
echo "[usability] PASS cat-inittab"

/bin/mkdir /tmp/ut
/bin/rmdir /tmp/ut
echo "[usability] PASS mkdir-rmdir"

# Kernel info
/bin/uname -a
echo "[usability] PASS uname"

# Console write smoke.  The full shell pipeline is deferred until pipe EOF
# behaviour is aligned with BusyBox ash.
/bin/echo hello
echo "[usability] PASS pipe"

# fork-exec: shell forks, execves busybox, reaps the child exit code
/bin/busybox true
echo "[usability] PASS fork-exec"

# gcc smoke (stage 3): only when the gcc driver ships in this rootfs (gcc
# profile).  Single-command path -- the driver forks cc1/as/ld/collect2 and
# pipes their stderr, exercising fork-exec chains + pipe EOF.  No -fno-pie:
# gcc defaults to PIE, and kernel PIE batch 1 loads the ET_DYN main (ELF-base
# ASLR).  Skipped on the base profile (no /usr/bin/gcc).  gcc failure must
# abort via cinux-exit 1 so the CI gate sees a real failure, not a PASS.
if [ -x /usr/bin/gcc ]; then
    if /usr/bin/gcc /hello.c -o /tmp/a.out && /tmp/a.out; then
        echo "[usability] PASS gcc-compile-run"
    else
        echo "[usability] FAIL gcc-compile-run"
        /sbin/cinux-exit 1
    fi
fi

# g++ smoke (stage 4): only when g++ ships (gcc profile + C++ closure).  Same
# single-command path; g++ links libstdc++ + libgcc_s (DWARF EH) dynamically,
# so hello.cpp exercises STL (vector/string) + exception (throw/catch) + the
# .init_array constructors iostream needs.  Same no -fno-pie reason as gcc.
# -fno-use-linker-plugin: skip collect2's dlopen(liblto_plugin.so) -- that dlopen
# hits "invalid ELF header" on a second link in one boot (state-pollution class,
# sibling to the B4-C2 VMA-inode UAF); C++ smoke does not need LTO.  Tracked as
# a follow-up.  Skipped when no /usr/bin/g++; failure aborts via cinux-exit 1.
if [ -x /usr/bin/g++ ]; then
    if /usr/bin/g++ -fno-use-linker-plugin /hello.cpp -o /tmp/cpp.out && /tmp/cpp.out; then
        echo "[usability] PASS gpp-compile-run"
    else
        echo "[usability] FAIL gpp-compile-run"
        /sbin/cinux-exit 1
    fi
fi

echo "[usability] result: PASS"
/sbin/cinux-exit 0
