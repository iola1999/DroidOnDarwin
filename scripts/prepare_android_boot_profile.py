#!/usr/bin/env python3
"""安装并启用一次客体启动采样；要求实验镜像的 root ADB 和可写 vendor。"""
import argparse
from pathlib import Path
import subprocess
import tempfile


RC = """# Husk one-shot boot profiling.
service husk_boot_profile /system/bin/sh /vendor/bin/husk-boot-profile.sh
    disabled
    oneshot
    user root
    group root readproc
    seclabel u:r:su:s0

on post-fs-data
    start husk_boot_profile
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    args = parser.parse_args()

    def run(*command):
        result = subprocess.run(["adb", "-s", args.serial, *command], capture_output=True,
                                text=True, timeout=45)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    if run("shell", "id -u").strip() != "0":
        raise RuntimeError("ADB root is required")
    if run("shell", "getenforce").strip() != "Permissive":
        raise RuntimeError("This collector requires the experimental permissive image")
    target = "/vendor/etc/init/husk-boot-profile.rc"
    existing = run("shell", f"if [ -f {target} ]; then cat {target}; fi")
    if existing and existing != RC:
        raise RuntimeError("Existing profiling service differs from the expected content")
    source = Path(__file__).with_name("android-boot-profile.sh")
    with tempfile.TemporaryDirectory(prefix="husk-boot-profile-") as directory:
        local = Path(directory) / "profile.rc"
        local.write_text(RC)
        run("push", str(local), "/data/local/tmp/husk-boot-profile.rc")
        run("push", str(source), "/data/local/tmp/husk-boot-profile.sh")
        run("shell", "mount -o remount,rw /vendor && "
            f"cat /data/local/tmp/husk-boot-profile.rc > {target} && "
            "cat /data/local/tmp/husk-boot-profile.sh > /vendor/bin/husk-boot-profile.sh && "
            f"chmod 0644 {target} && chmod 0755 /vendor/bin/husk-boot-profile.sh && "
            f"restorecon {target} /vendor/bin/husk-boot-profile.sh")
        run("shell", "rm /data/local/tmp/husk-boot-profile.rc /data/local/tmp/husk-boot-profile.sh")
    if run("shell", f"cat {target}") != RC or run("shell", "cat /vendor/bin/husk-boot-profile.sh") != source.read_text():
        raise RuntimeError("Profiling service readback differs")
    run("shell", "touch /data/local/tmp/husk-profile-next-boot && sync")
    print("Boot profiling armed once; results: /data/local/tmp/husk-boot-profile")


if __name__ == "__main__":
    main()
