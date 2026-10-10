#!/usr/bin/env python3
"""构建原生 Android 桌面使用的 gfxstream 渲染库。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LOGS = ROOT / "work/gfxstream-logs"


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

REVISION = "07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("ios", "macos", "macos-ios-api"), default="ios",
                        help="macos-ios-api runs the iOS EGL/platform code on a Mac for validation")
    args = parser.parse_args()
    platform = args.platform
    source = ROOT / "work/gfxstream-src"
    build = ROOT / f"work/gfxstream-build-{platform}"
    prefix = (ROOT / "build/ios-arm64/sysroot" if platform == "ios" else build / "prefix")
    LOGS.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        run(["git", "init", source], "gfxstream-init")
        run(["git", "remote", "add", "origin", "https://github.com/google/gfxstream.git"],
            "gfxstream-remote", source)
        run(["git", "fetch", "--depth=1", "origin", REVISION], "gfxstream-fetch", source)
        run(["git", "checkout", "--detach", REVISION], "gfxstream-checkout", source)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if revision != REVISION:
        raise SystemExit(f"gfxstream revision {revision}; expected {REVISION}")
    apply(source, "gfxstream-ios.patch")
    apply(source, "gfxstream-ios-serial-gles.patch")
    apply(source, "gfxstream-angle-shaders.patch")
    destinations = {
        "native-window.mm": "host/native_window",
        "egl-platform.mm": "host/gl/glestranslator/egl",
        "egl-platform.h": "host/gl/glestranslator/egl",
    }
    inputs = [ROOT / "patches/gfxstream-ios.patch", ROOT / "patches/gfxstream-ios-serial-gles.patch", ROOT / "patches/gfxstream-angle-shaders.patch", Path(__file__).resolve()]
    for name, directory in destinations.items():
        path = ROOT / "src/gfxstream-ios" / name
        shutil.copyfile(path, source / directory / name)
        inputs.append(path)
    ios_flags = ["-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_OSX_SYSROOT=iphoneos",
                 "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=16.4",
                 "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"] if platform == "ios" else []
    if platform == "macos-ios-api":
        ios_flags = ["-DGFXSTREAM_IOS=ON", *[
            f"-DCMAKE_{language}_FLAGS=-DGFXSTREAM_IOS" for language in ("C", "CXX", "OBJC", "OBJCXX")]]
    run(["cmake", "-S", source, "-B", build, "-G", "Ninja",
         "-DDEPENDENCY_RESOLUTION=SYSTEM", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", *ios_flags], f"gfxstream-configure-{platform}")
    # Upstream desktop tests depend on AppKit; build the renderer target only.
    run(["cmake", "--build", build, "--target", "gfxstream_backend", "-j", "4"],
        f"gfxstream-build-{platform}")
    (prefix / "lib/pkgconfig").mkdir(parents=True, exist_ok=True)
    artifact = prefix / "lib/libgfxstream_backend.dylib"
    shutil.copyfile(build / artifact.name, artifact)
    run(["xcrun", "install_name_tool", "-id", f"@rpath/{artifact.name}", artifact],
        f"gfxstream-install-name-{platform}")
    libraries = [artifact]
    run(["xcrun", "otool", "-L", *libraries], f"gfxstream-dependencies-{platform}")
    sdk_name = "iphoneos" if platform == "ios" else "macosx"
    sdk = subprocess.check_output(["xcrun", "--sdk", sdk_name, "--show-sdk-version"],
                                  text=True).strip()
    (build / "manifest.json").write_text(json.dumps({
        "revision": revision, "sdk": sdk, "platform": platform,
        "libraries": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in libraries},
        "inputs": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in inputs},
    }, indent=2) + "\n")
    print(f"Built {len(libraries)} graphics libraries for {platform}: {prefix}")


if __name__ == "__main__":
    main()
