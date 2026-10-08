#!/usr/bin/env python3
"""在 Apple Silicon Mac 上构建并测试 ARM64 用户态后端，接入已有 QEMU 10.0.12 源码。"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REVISION = "a46601580d5512d324104f985b5f0209dc980ddc"
SOURCE = ROOT / "work/dynarmic-src"
BUILD = ROOT / "work/arm64-exec-build"
LOGS = ROOT / "work/performance/architecture-research"


def verify_qemu_build():
    manifest = json.loads((BUILD / "manifest.json").read_text())
    stale = [name for name, digest in manifest["inputs"].items()
             if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest]
    binary = Path(manifest["qemu_source"]) / "build-macos/qemu-system-aarch64"
    if stale or hashlib.sha256(binary.read_bytes()).hexdigest() != manifest.get("qemu_sha256"):
        raise SystemExit("QEMU build differs from current inputs; run scripts/build_arm64_exec.py first")


def run(command, name, cwd=ROOT):
    log = LOGS / f"{name}.log"
    with log.open("w") as output:
        result = subprocess.run([str(x) for x in command], cwd=cwd,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f"Failed: {name}; see {log}")


def apply(source, patch_name):
    patch = ROOT / "patches" / patch_name
    command = ["patch", "--batch", "--no-backup-if-mismatch", "-r", "-",
               "-p1", "-i", str(patch)]
    for direction in ("--forward", "--reverse"):
        result = subprocess.run(command + ["--dry-run", direction], cwd=source,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if not result.returncode:
            if direction == "--forward":
                run(command + [direction], f"apply-{patch.stem}", source)
            return
    raise SystemExit(f"Patch does not match source: {patch}; inspect local changes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu-source", type=Path, default=ROOT / "work/qemu10-src")
    parser.add_argument("--engine-only", action="store_true")
    parser.add_argument("--gfxstream", action="store_true",
                        help="Link the graphics libraries from scripts/build_gfxstream.py --platform macos")
    args = parser.parse_args()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        parser.error("This build script requires an Apple Silicon Mac")
    for tool in ("git", "cmake", "ninja", "xcrun", "pkg-config"):
        if not shutil.which(tool):
            parser.error(f"Missing prerequisite: {tool}")
    qemu = args.qemu_source.resolve()
    if not args.engine_only and (not (qemu / "VERSION").exists() or
                                 (qemu / "VERSION").read_text().strip() != "10.0.12"):
        parser.error("Provide a prepared qemu-10.0.12-utm source tree with --qemu-source")
    LOGS.mkdir(parents=True, exist_ok=True)
    if not SOURCE.exists():
        run(["git", "init", SOURCE], "dynarmic-init")
        run(["git", "remote", "add", "origin", "https://github.com/azahar-emu/dynarmic.git"],
            "dynarmic-remote", SOURCE)
        run(["git", "fetch", "--depth=1", "origin", REVISION], "dynarmic-fetch", SOURCE)
        run(["git", "checkout", "--detach", REVISION], "dynarmic-checkout", SOURCE)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=SOURCE, text=True).strip()
    if revision != REVISION:
        raise SystemExit(f"Dynarmic revision differs: {revision}; expected {REVISION}")
    run(["git", "submodule", "update", "--init", "--depth=1", "externals/fmt", "externals/mcl",
         "externals/oaknut", "externals/robin-map"], "dynarmic-submodules", SOURCE)
    for patch in ("dynarmic-arm64-memory-abort.patch", "dynarmic-arm64-memory-cache.patch"):
        apply(SOURCE, patch)
    run(["cmake", "-S", ROOT / "src/arm64-exec", "-B", BUILD, "-G", "Ninja",
         f"-DDYNARMIC_SOURCE_DIR={SOURCE}", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"], "engine-configure")
    run(["cmake", "--build", BUILD, "-j", "8"], "engine-build")
    run(["ctest", "--test-dir", BUILD, "--output-on-failure"], "engine-tests")
    prefix = BUILD / "prefix"
    for subdir in ("lib/pkgconfig", "include"):
        (prefix / subdir).mkdir(parents=True, exist_ok=True)
    libraries = [BUILD / name for name in (
        "libhusk_arm64_exec.a", "dynarmic/src/dynarmic/libdynarmic.a",
        "dynarmic/externals/fmt/libfmt.a", "dynarmic/externals/mcl/src/libmcl.a")]
    run(["xcrun", "libtool", "-static", "-o", prefix / "lib/libhusk-arm64-exec.a", *libraries],
        "engine-archive")
    shutil.copyfile(ROOT / "src/arm64-exec/husk-arm64-exec.h", prefix / "include/husk-arm64-exec.h")
    (prefix / "lib/pkgconfig/husk-arm64-exec.pc").write_text(
        f"prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n"
        "Name: husk-arm64-exec\nDescription: Experimental ARM64 EL0 backend\nVersion: 0.1\n"
        "Libs: -L${libdir} -lhusk-arm64-exec -lc++\nCflags: -I${includedir}\n")
    if not args.engine_only:
        apply(qemu, "qemu-arm64-exec.patch")
        apply(qemu, "qemu-arm64-pauth-assist.patch")
        apply(qemu, "qemu-arm64-mops-gating.patch")
        apply(qemu, "qemu-arm64-host-sha256.patch")
        apply(qemu, "qemu-arm64-branch-assist.patch")
        apply(qemu, "qemu-arm64-fastmem-generation.patch")
        apply(qemu, "qemu-arm64-code-versions.patch")
        apply(qemu, "qemu-arm64-va-cache.patch")
        apply(qemu, "qemu-arm64-darwin-perfmap.patch")
        apply(qemu, "qemu-arm64-inline-lookup.patch")
        apply(qemu, "qemu-arm64-tlb-layout.patch")
        apply(qemu, "qemu-arm64-bulk-memory.patch")
        apply(qemu, "qemu-arm64-bulk-discovery.patch")
        apply(qemu, "qemu-arm64-bulk-acpi.patch")
        apply(qemu, "qemu-cocoa-surface-lifetime.patch")
        shutil.copyfile(ROOT / "src/arm64-exec/husk-arm64-qemu.c", qemu / "target/arm/tcg/husk-arm64-qemu.c")
        qbuild = qemu / "build-macos"
        qbuild.mkdir(exist_ok=True)
        if not (qbuild / "build.ninja").exists():
            run([qemu / "configure", "--target-list=aarch64-softmmu", "--enable-cocoa",
                 "--enable-slirp", "--disable-shared-lib", "--disable-sdl", "--disable-gtk",
                 "--disable-vnc", "--disable-spice", "--disable-opengl", "--disable-virglrenderer",
                 "--disable-curses", "--disable-curl", "--disable-libusb", "--disable-usb-redir",
                 "--disable-tpm", "--disable-docs", "--disable-guest-agent", "--disable-tools",
                 "--enable-hvf", "--disable-debug-info"], "qemu-engine-configure", qbuild)
        pkgpaths = [str(prefix / "lib/pkgconfig")]
        if args.gfxstream:
            pkgpaths.append(str(ROOT / "work/gfxstream-build-macos/prefix/lib/pkgconfig"))
        run([qbuild / "pyvenv/bin/meson", "configure", qbuild, "-Dhusk_arm64_exec=true",
             "-Dhvf=enabled",
             f"-Drutabaga_gfx={'enabled' if args.gfxstream else 'disabled'}",
             f"-Dpkg_config_path={','.join(pkgpaths)}"], "qemu-engine-meson")
        run(["ninja", "-C", qbuild, "-j", "8", "qemu-system-aarch64"], "qemu-engine-build")
    inputs = (list((ROOT / "src/arm64-exec").glob("*")) +
              list((ROOT / "patches").glob("*arm64*.patch")) +
              [ROOT / "patches/qemu-cocoa-surface-lifetime.patch"])
    manifest = {"dynarmic_revision": revision, "qemu_source": str(qemu),
                "gfxstream": args.gfxstream,
                "inputs": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in inputs if p.is_file()}}
    if not args.engine_only:
        manifest["qemu_sha256"] = hashlib.sha256((qbuild / "qemu-system-aarch64").read_bytes()).hexdigest()
    (BUILD / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Engine tests passed; build manifest: {BUILD / 'manifest.json'}")


if __name__ == "__main__":
    main()
