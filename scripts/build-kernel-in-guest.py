#!/usr/bin/env python3
# Runs every exported kernel compilation and both links inside Cinux.
# Resource blobs and kallsyms are generated here; only the bootloader is
# supplied as a bootstrap input. A fresh output tree prevents object reuse.
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import time


def run(arguments, directory):
    subprocess.run(arguments, cwd=directory, check=True)


def compile_source(job):
    directory = pathlib.Path(job["directory"])
    arguments = job["arguments"]
    (directory / arguments[arguments.index("-o") + 1]).parent.mkdir(parents=True, exist_ok=True)
    run(arguments, directory)


def remove_output(path):
    if path.is_dir():
        for child in path.iterdir():
            remove_output(child)
        path.rmdir()
    else:
        path.unlink()


def main():
    source = pathlib.Path("/src")
    config = json.loads((source / "build.json").read_text())
    output = source / "out"
    for child in output.iterdir():
        if child.name != "generated":
            remove_output(child)
    kernel = output / "kernel"
    kernel.mkdir()
    began = time.monotonic()
    (kernel / "font.psf").write_bytes((source / "assets/font.psf").read_bytes())
    run(["/bin/busybox", "tar", "-cf", str(kernel / "initrd.tar"), "readme.txt", "docs"],
        source / "assets/initrd")
    for stem, section, prefix in (("font.psf", "font", "font_psf"),
                                  ("initrd.tar", "initrd", "initrd")):
        symbol = stem.replace(".", "_")
        run(["objcopy", "-I", "binary", "-O", "elf64-x86-64", "-B", "i386:x86-64",
             "--rename-section", f".data=.rodata.{section}",
             "--redefine-sym", f"_binary_{symbol}_start=g_{prefix}_start",
             "--redefine-sym", f"_binary_{symbol}_end=g_{prefix}_end",
             "--set-section-flags", f".rodata.{section}=alloc,load,readonly,data",
             stem, f"{section}_blob.o"], kernel)
    run(["nm", "--version"], kernel)
    with (kernel / "preflight").open("wb") as target:
        target.write(b"kernel-selfhost-preflight\n")
        target.flush()
        os.fsync(target.fileno())
    print("[kernel-selfhost] resource and fsync preflight passed", flush=True)
    for index, job in enumerate(config["commands"], 1):
        print(f"[kernel-selfhost] compile {index}/{len(config['commands'])} {job['source']}", flush=True)
        started = time.monotonic()
        compile_source(job)
        print(f"[kernel-selfhost] compiled in {time.monotonic() - started:.3f}s", flush=True)
    (kernel / "CMakeFiles/cinux_kernel_unsym.dir").mkdir(parents=True, exist_ok=True)
    run(config["links"][0], kernel)
    run(["python3", "-B", "-S", str(source / "gen_kallsyms.py"), "--nm", "/usr/bin/nm",
         "--output", str(kernel / "cinux_kernel_kallsyms.cpp"),
         str(kernel / "cinux_kernel_unsym")], kernel)
    compile_source(config["kallsyms"])
    (kernel / "cinux_kernel_unsym").unlink()
    run(config["links"][1], kernel)
    run(["objcopy", "-O", "binary", "cinux_kernel", "kernel.bin"], kernel)
    binary = (kernel / "kernel.bin").read_bytes()
    if len(binary) < 40 or struct.unpack_from("<Q", binary, 16)[0] != len(binary):
        raise RuntimeError("kernel image header size does not match binary")
    image = output / "cinux.img"
    with image.open("wb") as target:
        target.write(bytes(max(1024 * 1024, 32 * 512 + len(binary))))
        target.seek(0)
        target.write((source / "boot/mbr.bin").read_bytes())
        target.seek(512)
        target.write((source / "boot/stage2.bin").read_bytes())
        target.seek(32 * 512)
        target.write(binary)
        target.flush()
        os.fsync(target.fileno())
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    print(f"[kernel-selfhost] IMAGE_SHA256 {digest}", flush=True)
    print(f"[kernel-selfhost] PASS {len(config['commands']) + 1} compilations, "
          f"two links, {len(binary)} kernel bytes, {time.monotonic() - began:.3f}s", flush=True)


if __name__ == "__main__":
    main()
