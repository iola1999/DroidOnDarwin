#!/usr/bin/env python3
"""验证 GLES 命令经宿主 C API 和 Linux DRM/QEMU 到达 Metal 后的像素结果。"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess

from build_arm64_exec import ROOT, verify_qemu_build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--angle-path", required=True, type=Path,
                        help="macOS arm64 libEGL.dylib and libGLESv2.dylib directory")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--ios-api", action="store_true",
                        help="Run the iOS platform implementation on macOS; skips the Linux/QEMU test")
    args = parser.parse_args()
    if not args.ios_api:
        verify_qemu_build()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    gfxstream = ROOT / "work/gfxstream-src"
    rutabaga = ROOT / "work/rutabaga-src"
    platform = "macos-ios-api" if args.ios_api else "macos"
    prefix = ROOT / f"work/gfxstream-build-{platform}/prefix"
    include = ["-I", gfxstream / "host/renderControl_dec", "-I", gfxstream / "host/gl/gles2_dec"]

    def run(command, name, seconds=60, env=None):
        with (output / f"{name}.log").open("w") as log:
            subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=seconds, env=env)

    run(["xcrun", "clang++", "-std=c++17", "-O2", ROOT / "tests/gfxstream/render.cpp",
         *include, "-I", gfxstream / "third_party/opengl/include",
         "-I", prefix / "include/rutabaga_gfx", "-L", prefix / "lib", "-lrutabaga_gfx_ffi",
         f"-Wl,-rpath,{prefix}/lib", "-o", output / "render-test"], "host-build")
    if args.ios_api:
        alias = prefix / "lib/libANGLE-shared.dylib"
        target = args.angle_path.resolve() / "libEGL.dylib"
        if not alias.exists():
            alias.symlink_to(target)
        elif alias.resolve() != target:
            raise SystemExit(f"ANGLE test alias points to a different library: {alias}")
    env = dict(os.environ, DYLD_LIBRARY_PATH=str(prefix / "lib") + ":" + str(args.angle_path.resolve()),
               ANGLE_DEFAULT_PLATFORM="metal", HUSK_GFXSTREAM_FEATURES="Surfaceless:enabled",
               HUSK_ARM64_EXEC="0")
    run([output / "render-test"], "host-render", env=env)
    host_text = (output / "host-render.log").read_text()
    if "PASS: GLES wire commands" not in host_text or "ANGLE Metal Renderer" not in host_text:
        raise SystemExit("Host test did not confirm pixel results and the Metal renderer")
    if args.ios_api:
        inputs = [prefix / "lib/libgfxstream_backend.dylib", prefix / "lib/librutabaga_gfx_ffi.dylib",
                  ROOT / "tests/gfxstream/render.cpp", args.angle_path / "libEGL.dylib",
                  args.angle_path / "libGLESv2.dylib"]
        (output / "results.json").write_text(json.dumps({"passed": True,
            "platform": platform, "iphone_tested": False,
            "sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
        }, indent=2) + "\n")
        print(f"iOS EGL code on macOS: PASS; {output}")
        return
    run(["xcrun", "clang", "--target=aarch64-none-elf", "-O2", "-march=armv8-a",
         "-mgeneral-regs-only", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
         "-fno-pic", *include, "-I", rutabaga / "third_party/mesa3d/src/magma/headers",
         "-c", ROOT / "tests/gfxstream/guest.c", "-o", output / "guest.o"], "guest-compile")
    run(["ld.lld", "-m", "aarch64elf", "-static", "-e", "_start", "--build-id=none",
         output / "guest.o", "-o", output / "init"], "guest-link")
    archive = bytearray()

    def entry(name, mode, data=b"", major=0, minor=0):
        fields = [1, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(name) + 1, 0]
        archive.extend(b"070701" + "".join(f"{n:08x}" for n in fields).encode())
        archive.extend(name.encode() + b"\0")
        archive.extend(b"\0" * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b"\0" * (-len(archive) % 4))

    entry("dev", stat.S_IFDIR | 0o755)
    entry("dev/dri", stat.S_IFDIR | 0o755)
    entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1)
    entry("dev/dri/renderD128", stat.S_IFCHR | 0o600, major=226, minor=128)
    entry("init", stat.S_IFREG | 0o755, (output / "init").read_bytes())
    entry("TRAILER!!!", 0)
    (output / "initramfs.cpio.gz").write_bytes(gzip.compress(archive, mtime=0))
    # The Android kernel includes virtio-gpu; the earlier Alpine test kernel uses a module.
    boot = ROOT / "work/boot-lab/downloads/lineage-boot.img"
    with boot.open("rb") as image:
        header = image.read(4096)
        if header[:8] != b"ANDROID!" or int.from_bytes(header[40:44], "little") != 4:
            raise SystemExit("Expected an Android boot image with header v4")
        (output / "kernel").write_bytes(image.read(int.from_bytes(header[8:12], "little")))
    command = [ROOT / "work/qemu10-src/build-macos/qemu-system-aarch64",
               "-name", "Husk gfxstream Metal test", "-machine", "virt", "-cpu", "max",
               "-accel", "tcg,thread=single", "-smp", "1", "-m", "768",
               "-kernel", output / "kernel", "-initrd", output / "initramfs.cpio.gz",
               "-append", "console=tty0 console=ttyAMA0 rdinit=/init selinux=0 panic=-1",
               "-device", "virtio-gpu-rutabaga-pci,x-gfxstream-gles=on",
               "-display", "cocoa", "-monitor", "none", "-no-reboot",
               "-serial", f"file:{output}/serial.log"]
    (output / "command.json").write_text(json.dumps([str(x) for x in command], indent=2) + "\n")
    run(command, "qemu", seconds=120, env=env)
    serial = (output / "serial.log").read_text(errors="replace")
    passed = "HUSK-GFXSTREAM PASS" in serial and "ANGLE Metal Renderer" in serial
    inputs = [Path(command[0]), boot, ROOT / "tests/gfxstream/guest.c",
              ROOT / "tests/gfxstream/render.cpp", args.angle_path / "libEGL.dylib",
              args.angle_path / "libGLESv2.dylib", prefix / "lib/libgfxstream_backend.dylib",
              prefix / "lib/librutabaga_gfx_ffi.dylib"]
    (output / "results.json").write_text(json.dumps({
        "passed": passed, "host_passed": True,
        "sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
    }, indent=2) + "\n")
    print(f"gfxstream guest test: {'PASS' if passed else 'FAIL'}; {output}")
    if not passed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
