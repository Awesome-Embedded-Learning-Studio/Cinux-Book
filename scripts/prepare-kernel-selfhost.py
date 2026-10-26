#!/usr/bin/env python3
# Exports a configured kernel build as source and commands, without kernel
# objects, and bakes an isolated ext2 volume for the guest compiler.
import argparse
import json
import pathlib
import shlex
import shutil
import subprocess
import sysconfig


def copy_file(source, root, target=None):
    source = pathlib.Path(source)
    destination = root / (target or str(source)).lstrip("/")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source.resolve(), destination)


def stage_python(root):
    python = pathlib.Path(shutil.which("python3")).resolve()
    copy_file(python, root, "/usr/bin/python3")
    library = pathlib.Path(sysconfig.get_path("stdlib"))
    destination = root / str(library).lstrip("/")
    shutil.copytree(library, destination, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("site-packages", "__pycache__", "test", "tests", "config-*", "*.a",
                                                  "idlelib", "tkinter", "turtledemo", "ensurepip"))
    programs = [python, *destination.glob("lib-dynload/*.so")]
    for program in programs:
        listing = subprocess.run(["ldd", str(program)], check=True, capture_output=True,
                                 text=True).stdout
        for line in listing.splitlines():
            fields = line.split()
            if len(fields) >= 3 and fields[1] == "=>" and fields[2].startswith("/"):
                copy_file(fields[2], root)


def export_build(repo, build, root):
    source = root / "src"
    for name in ("kernel", "base", "assets", "boot/early"):
        shutil.copytree(repo / name, source / name)
    shutil.copytree(build / "generated", source / "out/generated")
    for name in ("gen_kallsyms.py", "build-kernel-in-guest.py"):
        copy_file(repo / "scripts" / name, root, f"/src/{name}")
    for name in ("mbr.bin", "stage2.bin"):
        copy_file(build / "boot" / name, root, f"/src/boot/{name}")

    def remap(argument):
        return argument.replace(str(build), "/src/out").replace(str(repo), "/src")

    database = json.loads((build / "compile_commands.json").read_text())
    commands = []
    kallsyms = None
    for entry in database:
        arguments = entry.get("arguments") or shlex.split(entry["command"])
        output = entry.get("output", arguments[arguments.index("-o") + 1])
        if "cinux_kernel_core.dir/" not in output and "cinux_kernel.dir/" not in output:
            continue
        compiler = "g++" if entry["file"].endswith(".cpp") else "gcc"
        arguments = [compiler, *[remap(arg) for arg in arguments[1:]]]
        job = {"directory": remap(entry["directory"]), "arguments": arguments,
               "source": remap(entry["file"])}
        if entry["file"].endswith("cinux_kernel_kallsyms.cpp"):
            kallsyms = job
        else:
            commands.append(job)
    if not commands or kallsyms is None:
        raise RuntimeError("configured kernel compile commands missing")
    links = []
    for target in ("cinux_kernel_unsym", "cinux_kernel"):
        arguments = shlex.split((build / "kernel/CMakeFiles" / f"{target}.dir/link.txt").read_text())
        arguments = ["g++", *[remap(arg) for arg in arguments[1:]]]
        if target.endswith("_unsym"):
            arguments = [arg.replace("cinux_kernel_unsym.dir/kernel.cpp.o",
                                     "cinux_kernel.dir/kernel.cpp.o") for arg in arguments]
        links.append(arguments)
    config = {"commands": commands, "kallsyms": kallsyms, "links": links}
    (source / "build.json").write_text(json.dumps(config, indent=2) + "\n")
    for binary in ("nm", "objcopy"):
        copy_file(shutil.which(binary), root, f"/usr/bin/{binary}")
    headers = subprocess.run(["g++", "-E", "-x", "c++", "-", "-v"], input="",
                             text=True, capture_output=True, check=True).stderr
    paths = headers.split("#include <...> search starts here:", 1)[1].split("End of search list.")[0]
    for line in paths.splitlines():
        directory = pathlib.Path(line.strip())
        if "/c++/" in str(directory) or "/gcc/" in str(directory):
            shutil.copytree(directory, root / str(directory).lstrip("/"), dirs_exist_ok=True)
    print(f"[kernel-selfhost] exported {len(commands)} source compilations + kallsyms; no kernel objects")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build", type=pathlib.Path)
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parent.parent
    build = args.build_dir.resolve()
    root = build / "kernel-selfhost-root"
    shutil.rmtree(root, ignore_errors=True)
    shutil.copytree(build / "rootfs-assemble", root, symlinks=True)
    export_build(repo, build, root)
    stage_python(root)
    image = build / "boot/kernel-selfhost.ext2"
    with image.open("wb") as target:
        target.truncate(256 * 1024 * 1024)
    subprocess.run(["mkfs.ext2", "-q", "-F", "-b", "1024", "-O", "none", "-N", "32768",
                    "-d", str(root), str(image)], check=True)
    with image.open("r+b") as target:
        target.seek(512)
        target.write(bytes(range(255, -1, -1)) * 2)
    print(f"[kernel-selfhost] volume: {image}")


if __name__ == "__main__":
    main()
