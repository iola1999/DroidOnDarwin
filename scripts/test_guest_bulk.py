#!/usr/bin/env python3
"""用真实 HyperOS Bionic 库在小型 Linux 客体中验证批量接口。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
from build_arm64_test_guest import write_initramfs
from build_arm64_exec import ROOT, verify_qemu_build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True, help="linker64 and Bionic libc/libdl/libm from this guest")
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--modes", nargs="+", choices=("tcg", "direct", "direct-fastmem"), default=("tcg", "direct"))
    args = parser.parse_args()
    verify_qemu_build()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    subprocess.run([clang, "--target=aarch64-none-elf", "-O2", "-ffreestanding", "-fno-builtin",
                    "-mgeneral-regs-only", "-fno-stack-protector", "-fno-pic", "-c",
                    ROOT / "tests/guest-bulk/init.c", "-o", output / "init.o"], check=True)
    subprocess.run([shutil.which("ld.lld"), "-m", "aarch64elf", "-static", "-e", "_start",
                    "--build-id=none", output / "init.o", "-o", output / "init"], check=True)
    files = {"init": output / "init", "system/bin/linker64": args.runtime / "linker64",
             "libhusk_bulk.so": args.package / "libhusk_bulk.so",
             "system/bin/husk-bulk-bench": args.package / "husk-bulk-bench"}
    for name in ("libc.so", "libdl.so", "libm.so"):
        files["apex/com.android.runtime/lib64/bionic/" + name] = args.runtime / name
    folders = {"dev", "sys", "proc", "linkerconfig"}
    for name in files:
        folders.update(str(p) for p in Path(name).parents if str(p) != ".")
    entries = [(name, stat.S_IFDIR | 0o755, b"", 0) for name in sorted(folders, key=lambda s: (s.count('/'), s))]
    entries += [("dev/console", stat.S_IFCHR | 0o600, b"", 1)]
    entries += [(name, stat.S_IFREG | 0o755, path.read_bytes(), 0) for name, path in files.items()]
    config = ("dir.test=/system/bin\n[test]\nnamespace.default.isolated=false\n"
              "namespace.default.search.paths=/apex/com.android.runtime/lib64/bionic\n")
    entries.append(("linkerconfig/ld.config.txt", stat.S_IFREG | 0o644, config.encode(), 0))
    archive = output / "initramfs.cpio.gz"
    write_initramfs(archive, entries)
    kernel = ROOT / "work/performance/build-inputs/upstream/Payload/Husk.app/vmlinuz-virt"
    qemu = ROOT / "work/qemu10-src/build-macos/qemu-system-aarch64"
    manifest = {"sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in files.items()},
                "qemu_sha256": hashlib.sha256(qemu.read_bytes()).hexdigest(),
                "kernel_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(), "modes": args.modes}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    results = []
    for mode in args.modes:
        serial = output / f"{mode}.serial.log"
        command = [str(qemu), "-name", "Husk Bionic " + mode, "-machine", "virt", "-cpu",
                   "max,pauth-impdef=on,sve=off,sme=off", "-accel", "tcg,thread=single",
                   "-smp", "1", "-m", "512", "-kernel", str(kernel), "-initrd", str(archive),
                   "-append", "console=tty0 console=ttyAMA0 rdinit=/init panic=-1",
                   "-device", "virtio-gpu-pci", "-display", "cocoa", "-monitor", "none",
                   "-serial", f"file:{serial}", "-no-reboot"]
        env = dict(os.environ, HUSK_ARM64_EXEC="0" if mode == "tcg" else "1",
                   HUSK_ARM64_BACKEND="direct", HUSK_ARM64_BULK="1",
                   HUSK_ARM64_FASTMEM="1" if mode == "direct-fastmem" else "0",
                   HUSK_ARM64_SLICE_BUDGET="65536")
        (output / f"{mode}.command.json").write_text(json.dumps(command, indent=2) + "\n")
        with (output / f"{mode}.host.log").open("w") as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            try: process.wait(timeout=180)
            except subprocess.TimeoutExpired:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
        text = serial.read_text(errors="replace")
        rows = []
        for sample, (kind, body) in enumerate(re.findall(r"BEGIN (\w+)\n(.*?)END PASS", text, re.S)):
            row = {"sample": sample // 3, "kind": kind, "enabled": "bulk_enabled=1" in body,
                   "mops": "mops=1" in body,
                   "measurements": [dict(size=int(m[1]), bytes=int(m[2]), copy_ns=int(m[3]), set_ns=int(m[4]))
                    for m in re.finditer(r"BENCH size=(\d+) bytes=(\d+) copy_ns=(\d+) set_ns=(\d+)", body)]}
            counts = re.search(r"completed=(\d+) refused=(\d+) transferred=(\d+)", body)
            if counts: row.update(completed=int(counts[1]), refused=int(counts[2]), transferred=int(counts[3]))
            rows.append(row)
        passed = process.returncode == 0 and "HUSK-BIONIC PASS" in text and len(rows) == 9
        passed &= all(r["enabled"] == (r["kind"] == "enabled" and not r["mops"])
                      and len(r["measurements"]) == 6
                      and (not r["enabled"] or r.get("completed", 0) > 0) for r in rows)
        result = {"mode": mode, "passed": passed, "rows": rows}
        results.append(result)
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(json.dumps(result), flush=True)
        if not passed: raise SystemExit("Bionic guest validation failed; inspect serial log")


if __name__ == "__main__":
    main()
