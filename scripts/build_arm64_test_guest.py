#!/usr/bin/env python3
"""构建独立 Linux 用户态测试及最小 initramfs，所有产物写入 work。"""
import gzip
from pathlib import Path
import shutil
import stat
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "work/arm64-exec-guest"


def write_initramfs(path, entries):
    """写入 newc 归档；entries 为名称、mode、内容和设备 minor。"""
    archive = bytearray()
    for name, mode, data, minor in [*entries, ("TRAILER!!!", 0, b"", 0)]:
        fields = [1, mode, 0, 0, 1, 0, len(data), 0, 0, 5 if minor else 0, minor,
                  len(name.encode()) + 1, 0]
        archive.extend(b"070701" + "".join(f"{value:08x}" for value in fields).encode())
        archive.extend(name.encode() + b"\0")
        archive.extend(b"\0" * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b"\0" * (-len(archive) % 4))
    path.write_bytes(gzip.compress(archive, mtime=0))


def main(mthp=False, bulk=None):
    OUT.mkdir(parents=True, exist_ok=True)
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    linker = shutil.which("ld.lld")
    if not linker:
        raise SystemExit("ld.lld is required")
    subprocess.run([clang, "--target=aarch64-none-elf", "-O2", "-march=armv8-a",
                    *(["-DHUSK_TEST_MTHP=1"] if mthp else []),
                    *(["-DHUSK_TEST_BULK=1", f"-DHUSK_TEST_BULK_ENABLED={int(bulk)}"] if bulk is not None else []),
                    "-mgeneral-regs-only", "-ffreestanding", "-fno-builtin",
                    "-fno-stack-protector", "-fno-pic", "-c",
                    str(ROOT / "tests/arm64-exec/guest.c"), "-o", str(OUT / "guest.o")], check=True)
    subprocess.run([clang, "--target=aarch64-none-elf", "-march=armv8-a", "-c",
                    str(ROOT / "tests/arm64-exec/guest-vector.S"), "-o", str(OUT / "guest-vector.o")], check=True)
    subprocess.run([linker, "-m", "aarch64elf", "-static", "-e", "_start",
                    "--build-id=none", str(OUT / "guest.o"), str(OUT / "guest-vector.o"),
                    "-o", str(OUT / "init")], check=True)
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    subprocess.run([clang, "-isysroot", sdk, "-O2", "-arch", "arm64", "-mgeneral-regs-only",
                    str(ROOT / "tests/arm64-exec/native-bench.c"), "-o",
                    str(OUT / "native-bench")], check=True)

    # newc preserves the console device without creating a host device node.
    write_initramfs(OUT / "initramfs.cpio.gz", [
        ("dev", stat.S_IFDIR | 0o755, b"", 0),
        ("sys", stat.S_IFDIR | 0o755, b"", 0),
        ("dev/console", stat.S_IFCHR | 0o600, b"", 1),
        ("init", stat.S_IFREG | 0o755, (OUT / "init").read_bytes(), 0)])
    print(OUT / "initramfs.cpio.gz")


if __name__ == "__main__":
    main()
