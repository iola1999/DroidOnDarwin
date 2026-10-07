#!/usr/bin/env python3
"""构建独立 Linux 用户态测试及最小 initramfs，所有产物写入 work。"""
import gzip
from pathlib import Path
import shutil
import stat
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "work/arm64-exec-guest"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    linker = shutil.which("ld.lld")
    if not linker:
        raise SystemExit("ld.lld is required")
    subprocess.run([clang, "--target=aarch64-none-elf", "-O2", "-march=armv8-a",
                    "-mgeneral-regs-only", "-ffreestanding", "-fno-builtin",
                    "-fno-stack-protector", "-fno-pic", "-c",
                    str(ROOT / "tests/arm64-exec/guest.c"), "-o", str(OUT / "guest.o")], check=True)
    subprocess.run([linker, "-m", "aarch64elf", "-static", "-e", "_start",
                    "--build-id=none", str(OUT / "guest.o"), "-o", str(OUT / "init")], check=True)
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    subprocess.run([clang, "-isysroot", sdk, "-O2", "-arch", "arm64", "-mgeneral-regs-only",
                    str(ROOT / "tests/arm64-exec/native-bench.c"), "-o",
                    str(OUT / "native-bench")], check=True)

    # newc preserves the console device without creating a host device node.
    archive = bytearray()

    def entry(name, mode, data=b"", minor=0):
        fields = [1, mode, 0, 0, 1, 0, len(data), 0, 0, 5 if minor else 0, minor,
                  len(name.encode()) + 1, 0]
        archive.extend(b"070701" + "".join(f"{value:08x}" for value in fields).encode())
        archive.extend(name.encode() + b"\0")
        archive.extend(b"\0" * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b"\0" * (-len(archive) % 4))

    entry("dev", stat.S_IFDIR | 0o755)
    entry("dev/console", stat.S_IFCHR | 0o600, minor=1)
    entry("init", stat.S_IFREG | 0o755, (OUT / "init").read_bytes())
    entry("TRAILER!!!", 0)
    (OUT / "initramfs.cpio.gz").write_bytes(gzip.compress(archive, mtime=0))
    print(OUT / "initramfs.cpio.gz")


if __name__ == "__main__":
    main()
