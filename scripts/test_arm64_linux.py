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
from build_arm64_exec import verify_qemu_build
from build_arm64_test_guest import main as build_guest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--cpu", default="cortex-a57")
    parser.add_argument("--mthp", action="store_true", help="Enable guest 16 KiB mTHP")
    parser.add_argument("--bulk", choices=("on", "off"), help="Exercise the bulk-memory ABI and its fallback")
    parser.add_argument("--modes", nargs="+", choices=("tcg", "hybrid", "direct", "direct-checked", "direct-fastmem"),
                        default=("tcg", "direct"))
    parser.add_argument("--output", type=Path, default=ROOT / "work/performance/arm64-linux")
    args = parser.parse_args()
    verify_qemu_build()
    if args.runs < 1:
        parser.error("--runs must be positive")
    build_guest(mthp=args.mthp, bulk=(args.bulk == "on") if args.bulk else None)
    args.output.mkdir(parents=True, exist_ok=True)
    artifacts = ("work/qemu10-src/build-macos/qemu-system-aarch64",
                 "work/performance/build-inputs/upstream/Payload/Husk.app/vmlinuz-virt",
                 "work/arm64-exec-guest/initramfs.cpio.gz")
    (args.output / "manifest.json").write_text(json.dumps({
        "host": platform.platform(), "runs": args.runs, "modes": args.modes, "cpu": args.cpu,
        "mthp": args.mthp, "bulk": args.bulk,
        "backend_env": {k: v for k, v in os.environ.items() if k.startswith("HUSK_ARM64_")},
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
                       HUSK_ARM64_FASTMEM="1" if mode == "direct-fastmem" else "0",
                       HUSK_ARM64_LOOP_MAP="0" if mode == "direct-checked" else "1")
            if args.bulk:
                env["HUSK_ARM64_BULK"] = "1" if args.bulk == "on" else "0"
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
                   "passed": "HUSK-EL0-TEST PASS" in output and "HUSK-EL0-TEST FAIL" not in output
                             and "PASS executable aliases, kernel writes and revoked execution" in output,
                   "engine_active": "EL0 engine active" in host_output,
                   "direct_active": "backend=direct" in host_output,
                   "fastmem_active": "[husk-fastmem] segmented mapping active" in host_output,
                   "pauth_assist_active": "PAUTH assist active" in host_output,
                   "native_pauth_active": "native implementation-defined PAUTH active" in host_output,
                   "kernel_active": "kernel Direct active" in host_output,
                   "hot_tier_active": "[husk-hot-tier] threshold=" in host_output,
                   "integer_ns": int(benchmark[1]) if benchmark else None,
                   "memory_ns": int(memory_benchmark[1]) if memory_benchmark else None,
                   "tagged_ns": int(tagged_benchmark[1]) if tagged_benchmark else None,
                   "syscall_ns": int(syscall_benchmark[1]) if syscall_benchmark else None,
                   "switch_ns": int(switch_benchmark[1]) if switch_benchmark else None}
            preemption = re.search(r"PASS timer preemption of native loop; worst_ns=(\d+)", output)
            if args.bulk:
                for kind in ("copy", "set"):
                    measured = re.search(rf"BENCH bulk_{kind}_ns=(\d+) bytes=(\d+) completed=(\d+)", output)
                    if measured:
                        row[f"bulk_{kind}_ns"] = int(measured[1])
                        row[f"bulk_{kind}_bytes"] = int(measured[2])
                        row[f"bulk_{kind}_completed"] = int(measured[3])
                    else:
                        row["passed"] = False
            row["timer_preemption_worst_ns"] = int(preemption[1]) if preemption else None
            if not preemption:
                row["passed"] = False
            if not all((benchmark, memory_benchmark, tagged_benchmark, syscall_benchmark, switch_benchmark)):
                row["passed"] = False
            for name in ("vector", "float", "vector_memory", "vector_structures", "counter", "calls", "indirect_calls", "plain_calls", "tail_calls", "dispatch_ip", "dispatch_general", "pac_calls", "pac_diverse", "exclusive", "atomic", "pointer", "pointer_write", "mixed_code", "widen", "sha256", "sha256_hash", "collision"):
                measured = re.search(rf"BENCH {name}_ns=(\d+) value=(\d+)", output)
                row[name + "_ns"] = int(measured[1]) if measured else None
                skipped = (name == "atomic" and "SKIP LSE unavailable" in output) or (
                    name in ("sha256", "sha256_hash") and "SKIP SHA256 unavailable" in output)
                if not measured and not skipped:
                    row["passed"] = False
            pauth = re.search(r"BENCH pauth_ns=(\d+) count=(\d+)", output)
            row["tlb_hash_active"] = "[husk-tlb] XOR page index active" in host_output
            if env.get("HUSK_ARM64_TLB_HASH") == "1" and not row["tlb_hash_active"]:
                row["passed"] = False
            for stride in (4096, 16384, 65536, 262144):
                measured = re.search(rf"BENCH stride_{stride}_ns=(\d+) value=2000000", output)
                row[f"stride_{stride}_ns"] = int(measured[1]) if measured else None
                if not measured:
                    row["passed"] = False
            row["pauth_ns"] = int(pauth[1]) if pauth else None
            if not pauth:
                row["passed"] = False
            mops = re.search(r"BENCH mops_tagged_ns=(\d+) value=(\d+)", output)
            row["mops_tagged_ns"] = int(mops[1]) if mops else None
            row["mops_checked"] = "PASS MOPS tagged pointers, large copy and Linux write fault" in output
            if not row["mops_checked"] and "SKIP MOPS unavailable" not in output:
                row["passed"] = False
            if mode != "tcg" and not row["engine_active"]:
                row["passed"] = False
            if mode.startswith("direct") and not row["direct_active"]:
                row["passed"] = False
            if mode == "direct-fastmem" and not row["fastmem_active"]:
                row["passed"] = False
            if mode.startswith("direct") and env.get("HUSK_ARM64_KERNEL") == "1" and not row["kernel_active"]:
                row["passed"] = False
            if mode.startswith("direct") and int(env.get("HUSK_ARM64_HOT_THRESHOLD", "0")) and not row["hot_tier_active"]:
                row["passed"] = False
            results.append(row)
            (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
            print(json.dumps(row), flush=True)
            if not row["passed"] or expired or process.returncode:
                raise SystemExit("Guest validation failed; inspect this run's serial and host logs")


if __name__ == "__main__":
    main()
