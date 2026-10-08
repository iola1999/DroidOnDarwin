#!/usr/bin/env python3
"""串行对比同一构建的映射策略，并检查长时间线程创建和资源释放。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "work/native-linux-nce/processes"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--stress-rounds", type=int, default=256)
    args = parser.parse_args()
    if args.repeat < 1 or not 1 <= args.stress_rounds <= 1024:
        parser.error("repeat >= 1; stress-rounds must be 1..1024")
    summary = json.loads((BUILD / "lkl-summary.json").read_text())
    for name, expected in summary["input_sha256"].items():
        if digest(ROOT / name) != expected:
            raise SystemExit(f"{name} changed; run scripts/probe_lkl.py --processes")
    if digest(BUILD / "lkl-image.bin") != summary["kernel_sha256"]:
        raise SystemExit("Kernel differs from the validated build")
    runs = []
    # Alternate the order to reduce bias from temperature and other host work.
    for repeat in range(args.repeat):
        for mode in (("full", "diff", "lazy") if repeat % 2 == 0 else ("lazy", "diff", "full")):
            runs.append((mode, 16))
    runs.append(("lazy", args.stress_rounds))
    output = BUILD / "memory-benchmark"
    output.mkdir(exist_ok=True)
    report = {"created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
              "platform": summary["platform"], "source_revision": summary["source_revision"],
              "kernel_sha256": summary["kernel_sha256"], "input_sha256": summary["input_sha256"],
              "probe_sha256": digest(BUILD / "lkl-probe"), "runs": []}
    for index, (mode, rounds) in enumerate(runs):
        print(f"Run {index + 1}/{len(runs)}: {mode}, {rounds} userspace rounds", flush=True)
        env = {**os.environ, "NCE_MM_MODE": mode, "NCE_STRESS_ROUNDS": str(rounds)}
        log = output / f"{index + 1:02d}-{mode}"
        with log.with_suffix(".jsonl").open("w") as stdout, log.with_suffix(".log").open("w") as stderr:
            subprocess.run([BUILD / "lkl-probe", BUILD / "lkl-image.bin"], env=env,
                           stdout=stdout, stderr=stderr, check=True, timeout=120)
        records = [json.loads(line) for line in log.with_suffix(".jsonl").read_text().splitlines()]
        cases = [r for r in records if r.get("bench") == "linux_mm_exchange"]
        if records[-1].get("result") != "passed" or len(cases) != 28:
            raise SystemExit(f"Incomplete run: {log}")
        footprint = [r["phys_footprint"] for r in records if r.get("memory") == "after_userspace"]
        run = {"mode": mode, "stress_rounds": rounds, "records": records,
               "footprint_bytes": {"first": footprint[0], "last": footprint[-1],
                                   "min": min(footprint), "max": max(footprint)}}
        report["runs"].append(run)
    comparisons = []
    fields = ("bytes", "shared_percent", "touch_all", "threads")
    for case in [r for r in report["runs"][0]["records"] if r.get("bench") == "linux_mm_exchange"]:
        comparison = {key: case[key] for key in fields}
        for mode in ("full", "diff", "lazy"):
            samples = [r["ns_per_roundtrip"] for run in report["runs"][:-1] if run["mode"] == mode
                       for r in run["records"] if r.get("bench") == "linux_mm_exchange"
                       and all(r[k] == case[k] for k in fields)]
            comparison[mode] = {"median_ns": statistics.median(samples), "min_ns": min(samples),
                                "max_ns": max(samples)}
        comparison["speedup"] = comparison["full"]["median_ns"] / comparison["lazy"]["median_ns"]
        comparisons.append(comparison)
    report["comparison"] = comparisons
    destination = ROOT / "experiments/native-linux/results-memory-macos.json"
    destination.write_text(json.dumps(report, indent=2) + "\n")
    for row in comparisons:
        if row["bytes"] == 16 * 1024 * 1024:
            print(json.dumps(row), flush=True)
    print("Stress footprint:", report["runs"][-1]["footprint_bytes"])
    print(destination)


if __name__ == "__main__":
    main()
