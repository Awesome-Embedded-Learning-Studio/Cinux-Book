# Cinux musl tooling (F10-M1)

Build a self-contained **musl** sysroot and static user programs targeting the
Cinux (Linux x86_64 ABI) user runtime. Cinux does **not** ship its own libc
— musl is the libc. These scripts build it from source and compile programs
against it.

## Usage

```sh
# 1. Build the musl sysroot (~30 s; downloads musl-1.2.6 to build/musl/)
tools/musl/build-musl.sh

# 2. Compile the smoke binary -> build/musl/hello
tools/musl/build-hello.sh

# 3. Sanity-run on the host Linux (same ABI; proves the libc works)
./build/musl/hello
# -> Hello from musl on Cinux!
```

Output: `build/musl-sysroot/` (sysroot: `lib/libc.a`, `lib/crt1.o`,
`lib/Scrt1.o`, `lib/rcrt1.o`, `lib/crti.o`, `lib/crtn.o`, `include/`) and
`build/musl/hello` (static ELF). Both live under `build/` (gitignored).

Override the sysroot location with `MUSL_SYSROOT=...`.

## BusyBox smoke artifact (F-ECO)

Build a small static BusyBox against the musl sysroot:

```sh
tools/musl/build-busybox.sh
# -> build/musl/busybox
```

The script pins BusyBox by `BUSYBOX_VER` (default `1.36.0`) and copies
`tools/musl/busybox-1.36.0.config`, the known-good config used for the F-ECO
BusyBox applet work.  The ext2 image builder installs the binary as
`/bin/busybox`, `/bin/sh`, and hard links for common applets when
`build/musl/busybox` exists.

CI keeps this as a dedicated `busybox-smoke` job: it builds musl + BusyBox,
configures with `-DCINUX_BUSYBOX_SMOKE=ON`, then runs `run-kernel-test-all`.

## Dynamic linking (F10-M2)

The musl sysroot ships a shared libc — `lib/libc.so` is also the dynamic
linker (ldso). `configure` enables shared by default, so `build-musl.sh`
already produces it; no separate build step. Build a **dynamic** hello:

```sh
tools/musl/build-hello-dyn.sh
# -> build/musl/hello-dyn : ET_EXEC, dynamically linked,
#    interpreter /lib/ld-musl-x86_64.so.1
```

`hello-dyn` is a non-PIE dynamic executable (ET_EXEC + PT_INTERP +
PT_PHDR + PT_DYNAMIC). The kernel's F10-M2 execve path reads PT_INTERP,
loads the interpreter at `USER_INTERP_BASE`, and hands off — the ldso does
all GOT/PLT relocation in user space.

The interpreter lives at the PT_INTERP path `/lib/ld-musl-x86_64.so.1`.
`scripts/create_ext2_disk.sh` installs `libc.so` there (+ `hello-dyn`) when
both exist, so the QEMU ext2 image is runnable. Host-run is **not** possible
without installing that interpreter on the host (the host is glibc); the real
run is the ring-3 smoke (`CINUX_MUSL_DYN_SMOKE`, F10-M2 batch 3).

## Gotchas (baked into the scripts)

1. **No `--target` to configure.** musl prefixes the binutils (`x86_64-ar`)
   with the target triple, which the host lacks. Build natively with
   `CC=gcc AR=ar RANLIB=ranlib` (no `--target`).
2. **Don't use the `musl-gcc` wrapper on GCC ≥ 14.** The host specs inject
   `-latomic_asneeded`, which musl's specs don't suppress and which breaks the
   link. `build-hello.sh` links manually with `-nostdlib` instead.
3. **crt order matters.** The working static link is
   `Scrt1.o crti.o crtbeginS.o <objs> -lc -lgcc crtendS.o crtn.o`.
   Omitting GCC's `crtbeginS.o`/`crtendS.o` (which own `.init_array` /
   `.fini_array` / `.eh_frame`) produces a binary that segfaults at startup.
   `crtbeginS.o`/`crtendS.o` are located via `gcc -print-file-name=...`.

## What this enables

- **Batch 5** (this): produce a valid static musl ELF on the host.
- **Batch 6**: place `hello` on the ext2 image, load it via `execve` + the ELF
  loader + the batch-3 initial stack, and run it under QEMU — where musl's
  `printf` exercises the batch-4 `writev`/`arch_prctl`/`exit_group` syscalls.
- Later: `musl-gcc`-driven build of CFBox and other real user software.
