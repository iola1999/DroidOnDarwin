#!/usr/bin/env python3
"""在可见 QEMU 窗口中验证 EL1 寄存器、页表切换和写保护异常。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from build_arm64_exec import ROOT, verify_qemu_build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("runs must be positive")
    verify_qemu_build()
    args.output.mkdir(parents=True, exist_ok=False)
    source = ROOT / "tests/arm64-exec/kernel-system.S"
    obj = args.output / "kernel.o"
    guest = args.output / "kernel.elf"
    linker = shutil.which("ld.lld")
    if not linker:
        raise SystemExit("ld.lld is required")
    subprocess.run(["xcrun", "clang", "--target=aarch64-none-elf", "-c", source, "-o", obj], check=True)
    subprocess.run([linker, "-m", "aarch64elf", "-static", "-Ttext=0x40080000", "-e", "_start",
                    "--build-id=none", obj, "-o", guest], check=True)
    binary = ROOT / "work/qemu10-src/build-macos/qemu-system-aarch64"
    results = []
    for sample in range(args.runs):
        for mode in ("tcg", "direct", "direct-system", "fastmem-system"):
            stem = args.output / f"{sample}-{mode}"
            serial = stem.with_suffix(".serial.log")
            command = [str(binary), "-name", "Husk EL1 " + mode, "-machine", "virt",
                       "-cpu", "max,pauth-impdef=on,sve=off,sme=off", "-accel", "tcg,thread=single",
                       "-smp", "1", "-m", "128", "-kernel", str(guest),
                       "-device", "virtio-gpu-pci", "-display", "cocoa", "-monitor", "none",
                       "-serial", "file:" + str(serial), "-semihosting-config", "enable=on,target=native"]
            env = dict(os.environ, HUSK_ARM64_EXEC="0" if mode == "tcg" else "1",
                       HUSK_ARM64_BACKEND="direct", HUSK_ARM64_KERNEL="1", HUSK_ARM64_TCG_BRIDGE="1",
                       HUSK_ARM64_KERNEL_SYSTEM="1" if mode.endswith("system") else "0",
                       HUSK_ARM64_FASTMEM="1" if mode == "fastmem-system" else "0", HUSK_ARM64_PROFILE="1")
            stem.with_suffix(".command.json").write_text(json.dumps(command, indent=2) + "\n")
            expired = False
            with stem.with_suffix(".host.log").open("w") as log:
                process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    expired = True
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            text = serial.read_text(errors="replace") if serial.exists() else ""
            host = stem.with_suffix(".host.log").read_text(errors="replace")
            bench = re.search(r"kernel_ticks=([0-9a-f]+) frequency=([0-9a-f]+)", text)
            branches = re.search(r"kernel_branch_ticks=([0-9a-f]+) frequency=([0-9a-f]+)", text)
            va_hash = re.search(r"VA_HASH=([0-9a-f]+)", text)
            row = {"sample": sample, "mode": mode, "exit_code": process.returncode, "timeout": expired,
                   "passed": not expired and process.returncode == 0 and "HUSK-KERNEL-TEST PASS" in text,
                   "kernel_system_active": "[husk-kernel-system] completed=" in host,
                   "kernel_ms": int(bench[1], 16) * 1000 / int(bench[2], 16) if bench else None}
            row["passed"] &= bool(bench and branches and va_hash)
            row["kernel_branch_ms"] = int(branches[1], 16) * 1000 / int(branches[2], 16) if branches else None
            row["va_hash"] = va_hash[1] if va_hash else None
            row["inline_lookup_active"] = "[husk-tcg] inline EL1 jump lookup active" in host
            row["backend_env"] = {k: v for k, v in env.items() if k.startswith("HUSK_ARM64_")}
            if results:
                row["passed"] &= row["va_hash"] == results[0]["va_hash"]
            if mode == "tcg" and env.get("HUSK_ARM64_TCG_INLINE_LOOKUP") == "1":
                row["passed"] &= row["inline_lookup_active"]
            if mode != "tcg":
                row["passed"] &= "kernel Direct active" in host
            if mode.endswith("system"):
                row["passed"] &= row["kernel_system_active"]
            results.append(row)
            (args.output / "results.json").write_text(json.dumps({
                "qemu_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "guest_sha256": hashlib.sha256(guest.read_bytes()).hexdigest(), "runs": results}, indent=2) + "\n")
            print(json.dumps(row), flush=True)
            if not row["passed"]:
                raise SystemExit("Kernel validation failed; inspect its serial and host logs")


if __name__ == "__main__":
    main()
