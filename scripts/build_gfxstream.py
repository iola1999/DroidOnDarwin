#!/usr/bin/env python3
"""构建固定版本的 gfxstream 与 Rutabaga，供 QEMU 的实验图形后端使用。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from build_arm64_exec import ROOT, LOGS, apply, run

REVISION = "07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe"
RUTABAGA_REVISION = "ced6af5b1135173c269a037e06213e8a263de271"


def write_notices(source, rust_source):
    metadata = json.loads(subprocess.check_output([
        "cargo", "metadata", "--manifest-path", str(rust_source / "ffi/Cargo.toml"),
        "--locked", "--features", "gfxstream", "--format-version", "1",
        "--filter-platform", "aarch64-apple-ios"], text=True))
    packages = {p["id"]: p for p in metadata["packages"]}
    nodes = {n["id"]: n for n in metadata["resolve"]["nodes"]}
    pending = [metadata["resolve"]["root"]]
    selected = set()
    while pending:
        ident = pending.pop()
        if ident in selected: continue
        selected.add(ident)
        for dep in nodes[ident]["deps"]:
            if (any(kind["kind"] is None for kind in dep["dep_kinds"]) and
                not any("proc-macro" in t["kind"] for t in packages[dep["pkg"]]["targets"])):
                pending.append(dep["pkg"])
    files = [(f"gfxstream {REVISION}: LICENSE", source / "LICENSE")]
    files += [("gfxstream source: " + str(p.relative_to(source)), p)
              for p in sorted((source / "third_party").rglob("LICENSE*")) if p.is_file()]
    files.append((f"Rutabaga {RUTABAGA_REVISION}", rust_source / "LICENSE"))
    for package in sorted((packages[i] for i in selected), key=lambda p: p["name"]):
        folder = Path(package["manifest_path"]).parent
        for path in sorted(folder.iterdir()):
            if path.is_file() and path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE")):
                files.append((f"{package['name']} {package['version']}: {path.name}", path))
    text = "gfxstream source and Rutabaga runtime dependency notices\n"
    for label, path in files:
        text += f"\n{'=' * 72}\n{label}\n\n{path.read_text(errors='replace').rstrip()}\n"
    (ROOT / "src/app/Husk/Resources/legal/LICENSES-gfxstream.txt").write_text(text)


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
    destinations = {
        "native-window.mm": "host/native_window",
        "egl-platform.mm": "host/gl/glestranslator/egl",
        "egl-platform.h": "host/gl/glestranslator/egl",
    }
    inputs = [ROOT / "patches/gfxstream-ios.patch", Path(__file__).resolve()]
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
    rust_source = ROOT / "work/rutabaga-src"
    if not rust_source.exists():
        run(["git", "init", rust_source], "rutabaga-init")
        run(["git", "remote", "add", "origin", "https://github.com/magma-gpu/rutabaga_gfx.git"],
            "rutabaga-remote", rust_source)
        run(["git", "fetch", "--depth=1", "origin", RUTABAGA_REVISION], "rutabaga-fetch", rust_source)
        run(["git", "checkout", "--detach", RUTABAGA_REVISION], "rutabaga-checkout", rust_source)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=rust_source, text=True).strip()
    if actual != RUTABAGA_REVISION:
        raise SystemExit(f"Rutabaga revision {actual}; expected {RUTABAGA_REVISION}")
    lock = ROOT / "scripts/locks/rutabaga-Cargo.lock"
    inputs.append(lock)
    shutil.copyfile(lock, rust_source / "Cargo.lock")
    rust_target = "aarch64-apple-ios" if platform == "ios" else "aarch64-apple-darwin"
    run(["env", f"GFXSTREAM_PATH={prefix}/lib", "IPHONEOS_DEPLOYMENT_TARGET=16.4",
         "cargo", "build", "--manifest-path", rust_source / "ffi/Cargo.toml", "--locked",
         "--release", "--features", "gfxstream", "--target", rust_target], f"rutabaga-{platform}")
    # Link Rust into QEMU on iOS; its generated dylib's string table currently
    # fails the Xcode 27 linker's alignment check.
    rutabaga = prefix / ("lib/librutabaga_gfx_ffi.a" if platform == "ios"
                         else "lib/librutabaga_gfx_ffi.dylib")
    shutil.copyfile(rust_source / f"target/{rust_target}/release" / rutabaga.name, rutabaga)
    if platform == "ios":
        (prefix / "lib/librutabaga_gfx_ffi.dylib").unlink(missing_ok=True)
    else:
        run(["xcrun", "install_name_tool", "-id", f"@rpath/{rutabaga.name}", rutabaga],
            f"rutabaga-install-name-{platform}")
    include = prefix / "include/rutabaga_gfx"
    include.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(rust_source / "ffi/src/include/rutabaga_gfx_ffi.h", include / "rutabaga_gfx_ffi.h")
    rpath = f" -Wl,-rpath,{prefix}/lib" if platform != "ios" else ""
    rust_link = "${libdir}/librutabaga_gfx_ffi.a -liconv" if platform == "ios" else "-lrutabaga_gfx_ffi"
    (prefix / "lib/pkgconfig/rutabaga_gfx_ffi.pc").write_text(
        f"prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n"
        "Name: rutabaga_gfx_ffi\nDescription: Rutabaga C interface with gfxstream\nVersion: 0.1.85\n"
        f"Libs: -L${{libdir}} {rust_link} -lgfxstream_backend{rpath}\n"
        "Cflags: -I${includedir}\n")
    run(["xcrun", "otool", "-L", artifact, rutabaga], f"gfxstream-dependencies-{platform}")
    sdk_name = "iphoneos" if platform == "ios" else "macosx"
    sdk = subprocess.check_output(["xcrun", "--sdk", sdk_name, "--show-sdk-version"],
                                  text=True).strip()
    (build / "manifest.json").write_text(json.dumps({
        "revision": revision, "rutabaga_revision": actual, "sdk": sdk, "platform": platform,
        "libraries": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in (artifact, rutabaga)},
        "inputs": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in inputs},
    }, indent=2) + "\n")
    if platform == "ios":
        write_notices(source, rust_source)
    print(f"Built gfxstream and Rutabaga for {platform}: {prefix}")


if __name__ == "__main__":
    main()
