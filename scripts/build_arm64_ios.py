#!/usr/bin/env python3
"""构建 iOS ARM64 Direct 静态库，并将执行入口加入 QEMU 源码。"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from build_arm64_exec import ROOT, LOGS, apply, run


def main():
    source = ROOT / "src/arm64-exec"
    build = ROOT / "work/arm64-ios-build"
    prefix = ROOT / "build/ios-arm64/sysroot"
    oaknut = ROOT / "work/dynarmic-src/externals/oaknut"
    revision = "94c726ce0338b054eb8cb5ea91de8fe6c19f4392"
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=oaknut, text=True).strip()
    if actual != revision:
        raise SystemExit(f"Oaknut revision {actual}; expected {revision}")
    sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    build.mkdir(parents=True, exist_ok=True)
    LOGS.mkdir(parents=True, exist_ok=True)
    for folder in ("lib/pkgconfig", "include"):
        (prefix / folder).mkdir(parents=True, exist_ok=True)
    objects = []
    inputs = [source / name for name in (
        "husk-arm64-direct.cpp", "husk-arm64-direct-api.cpp", "husk-arm64-direct-entry.S",
        "husk-arm64-fastmem.cpp", "husk-arm64-bulk.cpp")]
    for path in inputs:
        obj = build / (path.name + ".o")
        flags = ["-std=c++20", "-Wall", "-Wextra"] if path.suffix == ".cpp" else []
        run(["xcrun", "--sdk", "iphoneos", "clang++", "-target", "arm64-apple-ios16.0",
             "-isysroot", sdk, "-O2", *flags, "-I", source, "-I", ROOT / "src/ios-jit",
             "-I", oaknut / "include", "-c", path, "-o", obj], "ios-" + path.name)
        objects.append(obj)
    archive = prefix / "lib/libhusk-arm64-exec.a"
    run(["xcrun", "libtool", "-static", "-o", archive, *objects], "ios-direct-archive")
    shutil.copyfile(source / "husk-arm64-exec.h", prefix / "include/husk-arm64-exec.h")
    (prefix / "lib/pkgconfig/husk-arm64-exec.pc").write_text(
        f"prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n"
        "Name: husk-arm64-exec\nDescription: ARM64 Direct iOS EL0 backend\nVersion: 0.2\n"
        "Libs: -L${libdir} -lhusk-arm64-exec -lc++\nCflags: -I${includedir}\n")
    qemu = ROOT / "third_party/build/qemu-10.0.12-utm"
    apply(qemu, "qemu-arm64-exec.patch")
    apply(qemu, "qemu-arm64-pauth-assist.patch")
    apply(qemu, "qemu-arm64-mops-gating.patch")
    apply(qemu, "qemu-arm64-branch-assist.patch")
    apply(qemu, "qemu-arm64-fastmem-generation.patch")
    apply(qemu, "qemu-arm64-bulk-memory.patch")
    apply(qemu, "qemu-arm64-bulk-discovery.patch")
    apply(qemu, "qemu-arm64-bulk-acpi.patch")
    shutil.copyfile(source / "husk-arm64-qemu.c", qemu / "target/arm/tcg/husk-arm64-qemu.c")
    tracked = inputs + [source / "husk-arm64-qemu.c"] + list(source.glob("*.h")) + list((ROOT / "src/ios-jit").glob("husk-ios-jit.*"))
    (build / "manifest.json").write_text(json.dumps({
        "sdk": sdk, "oaknut_revision": revision,
        "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "inputs": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked},
    }, indent=2) + "\n")
    print(f"iOS ARM64 Direct archive: {archive}")


if __name__ == "__main__":
    main()
