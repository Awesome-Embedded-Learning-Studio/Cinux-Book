# CinuxOS musl tooling (F10-M1)

Build a self-contained **musl** sysroot and static user programs targeting the
CinuxOS (Linux x86_64 ABI) user runtime. CinuxOS does **not** ship its own libc
— musl is the libc. These scripts build it from source and compile programs
against it.

## Usage

```sh
# 1. Build the musl sysroot (~30 s; downloads musl-1.2.5 to build/musl/)
tools/musl/build-musl.sh

# 2. Compile the smoke binary -> build/musl/hello
tools/musl/build-hello.sh

# 3. Sanity-run on the host Linux (same ABI; proves the libc works)
./build/musl/hello
# -> Hello from musl on CinuxOS!
```

Output: `build/musl-sysroot/` (sysroot: `lib/libc.a`, `lib/crt1.o`,
`lib/Scrt1.o`, `lib/rcrt1.o`, `lib/crti.o`, `lib/crtn.o`, `include/`) and
`build/musl/hello` (static ELF). Both live under `build/` (gitignored).

Override the sysroot location with `MUSL_SYSROOT=...`.

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
