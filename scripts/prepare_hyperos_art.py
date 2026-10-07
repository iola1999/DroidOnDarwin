#!/usr/bin/env python3
"""检查指定应用的 ART 状态，按需生成 speed 预编译产物并保存结果。"""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--package", action="append", required=True)
    parser.add_argument("--compile", action="store_true", help="为指定应用生成 speed 产物")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if any(not re.fullmatch(r"[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+", p) for p in args.package):
        parser.error("Invalid Android package name")
    args.output.mkdir(parents=True, exist_ok=False)
    adb = ["adb", "-s", args.serial, "shell"]

    def run(name, *command, seconds=60):
        result = subprocess.run([*adb, *command], capture_output=True, text=True,
                                errors="replace", timeout=seconds)
        (args.output / (name + ".txt")).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"{name} failed; see its log")
        return result.stdout

    properties = run("properties", "getprop")
    identity = {}
    for line in properties.splitlines():
        match = re.fullmatch(r"\[([^]]+)\]: \[(.*)\]", line)
        if match and (match[1] in ("ro.build.fingerprint", "ro.build.version.sdk") or
                      match[1].startswith("dalvik.vm.isa.")):
            identity[match[1]] = match[2]
    results = {"identity": identity, "packages": [], "passed": False}
    try:
        for package in dict.fromkeys(args.package):
            before = run(package + "-before", "pm", "art", "dump", package)
            if f"[{package}]" not in before or "[status=" not in before:
                raise RuntimeError(f"No ART status for {package}")
            row = {"package": package, "before": before, "compiled": False}
            results["packages"].append(row)
            statuses = re.findall(r"\[status=([^]]+)\]", before)
            if args.compile and any(status != "speed" for status in statuses):
                run(package + "-compile", "pm", "compile", "-m", "speed", "-f", package,
                    seconds=900)
                row["compiled"] = True
            after = run(package + "-after", "pm", "art", "dump", package)
            row["after"] = after
            statuses = re.findall(r"\[status=([^]]+)\]", after)
            row["speed_verified"] = bool(statuses) and all(s == "speed" for s in statuses)
            print(package, "speed_verified", row["speed_verified"], flush=True)
            if args.compile and not row["speed_verified"]:
                raise RuntimeError(f"ART did not accept speed artifacts for {package}")
        results["passed"] = True
    finally:
        (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
