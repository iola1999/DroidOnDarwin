#!/usr/bin/env python3
"""构建 Android 批量内存实验库与 libc 对照测试。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "work/guest-bulk")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    compiler = args.ndk / "toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang"
    common = [str(compiler), "-O2", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
              "-mno-outline-atomics",
              "-mbranch-protection=standard", "-Wl,-z,max-page-size=16384"]
    outputs = [args.output / "libhusk_bulk.so", args.output / "husk-bulk-bench"]
    subprocess.run([*common, "-fPIC", "-shared", "-Wl,-z,defs", ROOT / "src/guest-bulk/preload.c",
                    ROOT / "src/guest-bulk/entry.S",
                    "-ldl", "-o", outputs[0]], check=True)
    subprocess.run([*common, ROOT / "tests/guest-bulk/benchmark.c", "-ldl", "-o", outputs[1]], check=True)
    (args.output / "manifest.json").write_text(json.dumps({
        "compiler": str(compiler), "sha256": {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in outputs}}, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
