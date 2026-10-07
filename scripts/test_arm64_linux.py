#!/usr/bin/env python3
"""用可见 QEMU 窗口比较 TCG 与混合后端，保存命令、日志和测试结果。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--cpu", default="cortex-a57")
    parser.add_argument("--modes", nargs="+", choices=("tcg", "hybrid", "direct", "direct-checked"),
                        default=("tcg", "hybrid", "direct"))
    parser.add_argument("--output", type=Path, default=ROOT / "work/performance/arm64-linux")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    artifacts = ("work/qemu10-src/build-macos/qemu-system-aarch64",
                 "work/performance/build-inputs/upstream/Payload/Husk.app/vmlinuz-virt",
                 "work/arm64-exec-guest/initramfs.cpio.gz")
    (args.output / "manifest.json").write_text(json.dumps({
        "host": platform.platform(), "runs": args.runs, "modes": args.modes,
        "sha256": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in artifacts},
    }, indent=2) + "\n")
    results = []
    for sample in range(args.runs):
        for mode in args.modes:
            stem = args.output / f"{sample}-{mode}"
            serial = stem.with_suffix(".serial.log")
            command = [str(ROOT / "work/qemu10-src/build-macos/qemu-system-aarch64"),
                       "-name", f"Husk EL0 {mode}", "-machine", "virt", "-cpu", args.cpu,
                       "-accel", "tcg,thread=single", "-smp", "1", "-m", "512",
                       "-kernel", str(ROOT / "work/performance/build-inputs/upstream/Payload/Husk.app/vmlinuz-virt"),
                       "-initrd", str(ROOT / "work/arm64-exec-guest/initramfs.cpio.gz"),
                       "-append", "console=tty0 console=ttyAMA0 rdinit=/init panic=-1",
                       "-device", "virtio-gpu-pci", "-display", "cocoa", "-monitor", "none",
                       "-serial", f"file:{serial}", "-no-reboot"]
            stem.with_suffix(".command.json").write_text(json.dumps(command, indent=2) + "\n")
            env = dict(os.environ, HUSK_ARM64_EXEC="0" if mode == "tcg" else "1",
                       HUSK_ARM64_BACKEND="direct" if mode.startswith("direct") else "dynarmic",
                       HUSK_ARM64_LOOP_MAP="0" if mode == "direct-checked" else "1")
            start = time.monotonic()
            expired = False
            with stem.with_suffix(".host.log").open("w") as log:
                process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    process.wait(timeout=120)
                except subprocess.TimeoutExpired:
                    expired = True
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            output = serial.read_text(errors="replace") if serial.exists() else ""
            host_output = stem.with_suffix(".host.log").read_text(errors="replace")
            benchmark = re.search(r"BENCH integer_ns=(\d+) sum=(\d+)", output)
            memory_benchmark = re.search(r"BENCH memory_ns=(\d+) value=(\d+)", output)
            tagged_benchmark = re.search(r"BENCH tagged_ns=(\d+) value=(\d+)", output)
            syscall_benchmark = re.search(r"BENCH syscall_ns=(\d+) count=(\d+)", output)
            switch_benchmark = re.search(r"BENCH switch_ns=(\d+) count=(\d+)", output)
            row = {"sample": sample, "mode": mode, "exit_code": process.returncode,
                   "elapsed_seconds": time.monotonic() - start, "timeout": expired,
                   "passed": "HUSK-EL0-TEST PASS" in output and "HUSK-EL0-TEST FAIL" not in output,
                   "engine_active": "integer EL0 engine active" in host_output,
                   "direct_active": "backend=direct" in host_output,
                   "integer_ns": int(benchmark[1]) if benchmark else None,
                   "memory_ns": int(memory_benchmark[1]) if memory_benchmark else None,
                   "tagged_ns": int(tagged_benchmark[1]) if tagged_benchmark else None,
                   "syscall_ns": int(syscall_benchmark[1]) if syscall_benchmark else None,
                   "switch_ns": int(switch_benchmark[1]) if switch_benchmark else None}
            if not all((benchmark, memory_benchmark, tagged_benchmark, syscall_benchmark, switch_benchmark)):
                row["passed"] = False
            if mode != "tcg" and not row["engine_active"]:
                row["passed"] = False
            if mode.startswith("direct") and not row["direct_active"]:
                row["passed"] = False
            results.append(row)
            (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
            print(json.dumps(row), flush=True)
            if not row["passed"] or expired or process.returncode:
                raise SystemExit("Guest validation failed; inspect this run's serial and host logs")


if __name__ == "__main__":
    main()
