#!/usr/bin/env python3
"""通过明确指定的 ADB 连接更新已启动的 HyperOS 客体驱动。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="adb devices 中的完整设备标识")
    parser.add_argument("--package", required=True, type=Path)
    args = parser.parse_args()
    if not args.package.is_file(): raise SystemExit("Guest driver package not found")
    adb = ["adb", "-s", args.serial]

    def run(*command):
        result = subprocess.run([*adb, *command], capture_output=True, text=True, timeout=120)
        if result.returncode: raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    if run("shell", "id -u").strip() != "0":
        print(run("root"), end="", flush=True)
        run("wait-for-device")
    if run("shell", "id -u").strip() != "0": raise SystemExit("ADB root shell unavailable")
    print(run("push", str(args.package.resolve()), "/data/local/tmp/husk-gfxstream.tar.gz"), end="")
    command = ("rm -rf /data/local/tmp/husk-gfxstream && "
               "mkdir /data/local/tmp/husk-gfxstream && "
               "cd /data/local/tmp/husk-gfxstream && "
               "tar -xzf ../husk-gfxstream.tar.gz && sh install.sh")
    output = run("shell", command)
    print(output, end="")
    if "HUSK-GFXSTREAM INSTALLED" not in output: raise SystemExit("Missing install completion marker")
    run("shell", "rm -rf /data/local/tmp/husk-gfxstream /data/local/tmp/husk-gfxstream.tar.gz")
    print("Driver update complete. Close and reopen the emulator before enabling Metal GLES.")


if __name__ == "__main__":
    main()
