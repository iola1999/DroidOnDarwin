#!/usr/bin/env python3
"""从 HyperOS AVD vendor 提取 GLES 驱动，生成原地更新客体的安装包。"""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
LIBRARIES = ["egl/libEGL_emulation.so", "egl/libGLESv1_CM_emulation.so",
             "egl/libGLESv2_emulation.so", "libGLESv1_enc.so", "libGLESv2_enc.so",
             "libOpenglCodecCommon.so", "libOpenglSystemCommon.so", "lib_renderControl_enc.so",
             "libandroidemu.so"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vendor", type=Path, required=True, help="HyperOS AVD EROFS vendor.img")
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    for name in LIBRARIES:
        target = output / "lib64" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("wb") as stream:
            subprocess.run(["dump.erofs", "--cat", f"--path=/lib64/{name}", args.vendor],
                           stdout=stream, check=True)
        if target.read_bytes()[:4] != b"\x7fELF":
            raise SystemExit(f"Invalid ELF: {name}")
    compiler = args.ndk / "toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang"
    subprocess.run([compiler, "-O2", "-Wall", "-Wextra", "-Werror",
                    ROOT / "src/gfxstream-ios/guest-detect.c", "-o", output / "husk-gfxstream-detect"],
                   check=True)
    shutil.copyfile(ROOT / "scripts/gfxstream-guest-install.sh", output / "install.sh")
    files = sorted(p for p in output.rglob("*") if p.is_file())
    (output / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(output)}\n" for p in files))
    archive = output.parent / (output.name + ".tar.gz")
    with tarfile.open(archive, "w:gz") as tar:
        for path in [*files, output / "SHA256SUMS"]:
            tar.add(path, arcname=str(path.relative_to(output)), recursive=False)
    print(archive)


if __name__ == "__main__":
    main()
